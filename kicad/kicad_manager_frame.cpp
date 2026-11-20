/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2017 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2013 CERN (www.cern.ch)
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "kicad_id.h"
#include "pcm.h"
#include "pgm_kicad.h"
#include "project_tree.h"
#include "project_tree_pane.h"
#include <dialogs/dialog_git_mr_review.h>
#include "local_history_pane.h"
#include "widgets/bitmap_button.h"

#include <advanced_config.h>
#include <background_jobs_monitor.h>
#include <bitmaps.h>
#include <build_version.h>
#include <confirm.h>
#include <dialogs/panel_kicad_launcher.h>
#include <dialogs/panel_jobset.h>
#include <dialogs/dialog_edit_cfg.h>
#include <local_history.h>
#include <widgets/wx_progress_reporters.h>
#include <wx/accel.h>
#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/eventfilter.h>
#include <wx/ffile.h>
#include <wx/filedlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/process.h>
#include <wx/settings.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/treectrl.h>
#include <eda_base_frame.h>
#include <executable_names.h>
#include <file_history.h>
#include <policy_keys.h>
#include <gestfich.h>
#include <kiplatform/app.h>
#include <kiplatform/environment.h>
#include <kiplatform/ui.h>
#include <kiplatform/policy.h>
#include <kiway.h>
#include <kiway_mail.h>
#include <launch_ext.h>
#include <lockfile.h>
#include <notifications_manager.h>
#include <reporter.h>
#include <project/project_local_settings.h>
#include <sch_file_versions.h>
#include <settings/common_settings.h>
#include <settings/settings_manager.h>
#include <tool/action_manager.h>
#include <tool/action_toolbar.h>
#include <tool/common_control.h>
#include <tool/tool_dispatcher.h>
#include <tool/tool_manager.h>
#include <tools/kicad_manager_actions.h>
#include <tools/kicad_manager_control.h>
#include <toolbars_kicad_manager.h>
#include <wildcards_and_files_ext.h>
#include <widgets/app_progress_dialog.h>
#include <widgets/kistatusbar.h>
#include <widgets/ui_common.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <update_manager.h>
#include <jobs/jobset.h>
#include <widgets/wx_aui_art_providers.h>

#include <../pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h>   // for SEXPR_BOARD_FILE_VERSION def

#ifdef __WXMAC__
#include <MacTypes.h>
#include <ApplicationServices/ApplicationServices.h>
#endif

#include "kicad_manager_frame.h"
#include "settings/kicad_settings.h"

#include <project/project_file.h>

// See below the purpose of this include
#include <wx/xml/xml.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#define EDITORS_CAPTION        wxString::FromUTF8( "开始页" )
#define PROJECT_FILES_CAPTION  _( "Project Files" )

#define ID_INIT_WATCHED_PATHS  52913
#define ID_LANTU_ACTIVATE_TAB  59901

#define SEP()   wxFileName::GetPathSeparator()


// ============================================================================
// LantuEDA 单窗口标签页工作台 & 顶部动态菜单栏引擎（含 Alt+3 重复跳转修复）
// ============================================================================
namespace
{

static bool IsFrameInTopLevelList( wxFrame* aFrame )
{
    if( !aFrame )
        return false;

    for( wxWindow* win : wxTopLevelWindows )
    {
        if( win == aFrame )
            return true;
    }

    return false;
}


static bool IsDescendantOf( wxWindow* aWin, wxWindow* aAncestor )
{
    for( wxWindow* w = aWin; w != nullptr; w = w->GetParent() )
    {
        if( w == aAncestor )
            return true;
    }

    return false;
}


class LANTU_EMBEDDED_FRAME_PAGE;

#ifdef __WXMSW__
struct LANTU_SUBCLASS_INFO
{
    WNDPROC                    origProc = nullptr;
    LANTU_EMBEDDED_FRAME_PAGE* hostPage = nullptr;
};

static std::unordered_map<HWND, LANTU_SUBCLASS_INFO> s_subclassedFrames;
static LRESULT CALLBACK LantuEmbeddedChildWndProc( HWND aHwnd, UINT aMsg, WPARAM aWParam, LPARAM aLParam );
#endif


/**
 * 顶部全局动态菜单栏（含右侧快捷面板折叠按钮）：
 * 1. 左侧自动映射当前激活标签页（开始页 / 原理图 / PCB / 3D视图等）的原生 wxMenuBar；
 * 2. 右侧提供【工程栏】与【属性栏】一键显隐开关，随时释放最大画布空间；
 * 3. 使用 TPM_RETURNCMD 精准捕获 3D 查看器/切换视图菜单命令，确保已打开的标签可直接激活。
 */
class LANTU_PROXY_MENUBAR : public wxPanel
{
public:
    explicit LANTU_PROXY_MENUBAR( wxWindow* aParent )
        : wxPanel( aParent, wxID_ANY, wxDefaultPosition,
                   wxSize( aParent->FromDIP( 1200 ), aParent->FromDIP( 28 ) ) ),
          m_targetFrame( nullptr ),
          m_hoverIdx( -1 ),
          m_openIdx( -1 ),
          m_hoverQuickBtn( 0 ),
          m_isEditorActive( false ),
          m_projectTreeVisible( true ),
          m_propertiesVisible( false )
    {
        SetMinSize( wxSize( FromDIP( 200 ), FromDIP( 28 ) ) );
        SetMaxSize( wxSize( 10000, FromDIP( 28 ) ) );
        SetBackgroundStyle( wxBG_STYLE_PAINT );
        Bind( wxEVT_PAINT, &LANTU_PROXY_MENUBAR::OnPaint, this );
        Bind( wxEVT_SIZE, &LANTU_PROXY_MENUBAR::OnSize, this );
        Bind( wxEVT_MOTION, &LANTU_PROXY_MENUBAR::OnMouseMove, this );
        Bind( wxEVT_LEAVE_WINDOW, &LANTU_PROXY_MENUBAR::OnMouseLeave, this );
        Bind( wxEVT_LEFT_DOWN, &LANTU_PROXY_MENUBAR::OnMouseDown, this );
    }

    void SetCallbacks( std::function<void()> aOnToggleProjectTree,
                       std::function<void()> aOnToggleProperties,
                       std::function<void( wxFrame*, const wxString& )> aOnMenuCommand,
                       std::function<void()> aOnRestoreFocus )
    {
        m_onToggleProjectTree = std::move( aOnToggleProjectTree );
        m_onToggleProperties  = std::move( aOnToggleProperties );
        m_onMenuCommand       = std::move( aOnMenuCommand );
        m_onRestoreFocus      = std::move( aOnRestoreFocus );
    }

    void SetTargetFrame( wxFrame* aFrame, bool aIsEditorActive )
    {
        if( m_targetFrame != aFrame || m_isEditorActive != aIsEditorActive )
        {
            m_targetFrame    = aFrame;
            m_isEditorActive = aIsEditorActive;
            m_hoverIdx       = -1;
            m_openIdx        = -1;
            Refresh();
        }
    }

    void SetQuickStates( bool aProjectTreeVisible, bool aPropertiesVisible )
    {
        if( m_projectTreeVisible != aProjectTreeVisible || m_propertiesVisible != aPropertiesVisible )
        {
            m_projectTreeVisible = aProjectTreeVisible;
            m_propertiesVisible  = aPropertiesVisible;
            Refresh();
        }
    }

    wxFrame* GetTargetFrame() const { return m_targetFrame; }

private:
    wxFrame*                                         m_targetFrame;
    std::vector<wxRect>                              m_itemRects;
    wxRect                                           m_btnProjectRect;
    wxRect                                           m_btnPropsRect;
    int                                              m_hoverIdx;
    int                                              m_openIdx;
    int                                              m_hoverQuickBtn; // 0=none, 1=projectTree, 2=properties
    bool                                             m_isEditorActive;
    bool                                             m_projectTreeVisible;
    bool                                             m_propertiesVisible;
    std::function<void()>                            m_onToggleProjectTree;
    std::function<void()>                            m_onToggleProperties;
    std::function<void( wxFrame*, const wxString& )> m_onMenuCommand;
    std::function<void()>                            m_onRestoreFocus;

    void OnSize( wxSizeEvent& aEvt )
    {
        Refresh();
        aEvt.Skip();
    }

    void DrawQuickToggleButton( wxDC& aDC, const wxRect& aRect, const wxString& aLabel,
                                bool aActive, bool aHover )
    {
        if( aHover )
        {
            aDC.SetBrush( wxBrush( wxColour( 222, 234, 252 ) ) );
            aDC.SetPen( wxPen( wxColour( 150, 186, 240 ), 1 ) );
            aDC.DrawRoundedRectangle( aRect, 4 );
            aDC.SetTextForeground( wxColour( 20, 78, 175 ) );
        }
        else if( aActive )
        {
            aDC.SetBrush( wxBrush( wxColour( 235, 242, 252 ) ) );
            aDC.SetPen( wxPen( wxColour( 192, 212, 242 ), 1 ) );
            aDC.DrawRoundedRectangle( aRect, 4 );
            aDC.SetTextForeground( wxColour( 28, 90, 190 ) );
        }
        else
        {
            aDC.SetBrush( wxBrush( wxColour( 244, 246, 249 ) ) );
            aDC.SetPen( wxPen( wxColour( 218, 223, 230 ), 1 ) );
            aDC.DrawRoundedRectangle( aRect, 4 );
            aDC.SetTextForeground( wxColour( 90, 98, 110 ) );
        }

        wxSize tsz = aDC.GetTextExtent( aLabel );
        int tx = aRect.x + ( aRect.width - tsz.x ) / 2;
        int ty = aRect.y + ( aRect.height - tsz.y ) / 2;
        aDC.DrawText( aLabel, tx, ty );
    }

    void OnPaint( wxPaintEvent& )
    {
        wxAutoBufferedPaintDC dc( this );
        wxSize sz = GetClientSize();
        if( sz.x <= 0 || sz.y <= 0 )
            return;

        dc.SetBrush( wxBrush( wxColour( 248, 250, 252 ) ) );
        dc.SetPen( *wxTRANSPARENT_PEN );
        dc.DrawRectangle( 0, 0, sz.x, sz.y );

        dc.SetPen( wxPen( wxColour( 214, 220, 228 ), 1 ) );
        dc.DrawLine( 0, sz.y - 1, sz.x, sz.y - 1 );

        wxFont font = wxSystemSettings::GetFont( wxSYS_DEFAULT_GUI_FONT );
        dc.SetFont( font );

        m_btnProjectRect = wxRect();
        m_btnPropsRect   = wxRect();

        int rightEdge = sz.x - FromDIP( 8 );
        const int btnPadX = FromDIP( 10 );
        const int btnH    = sz.y - FromDIP( 6 );
        const int btnY    = FromDIP( 3 );

        if( m_isEditorActive )
        {
            wxString propsTxt = m_propertiesVisible
                    ? wxString::FromUTF8( "隐藏属性" )
                    : wxString::FromUTF8( "显示属性" );
            wxSize ptsz = dc.GetTextExtent( propsTxt );
            int bw = ptsz.x + btnPadX * 2;
            m_btnPropsRect = wxRect( rightEdge - bw, btnY, bw, btnH );
            DrawQuickToggleButton( dc, m_btnPropsRect, propsTxt, m_propertiesVisible, m_hoverQuickBtn == 2 );
            rightEdge = m_btnPropsRect.x - FromDIP( 6 );
        }

        {
            wxString prjTxt = m_projectTreeVisible
                    ? wxString::FromUTF8( "隐藏工程栏" )
                    : wxString::FromUTF8( "显示工程栏" );
            wxSize tsz = dc.GetTextExtent( prjTxt );
            int bw = tsz.x + btnPadX * 2;
            m_btnProjectRect = wxRect( rightEdge - bw, btnY, bw, btnH );
            DrawQuickToggleButton( dc, m_btnProjectRect, prjTxt, m_projectTreeVisible, m_hoverQuickBtn == 1 );
            rightEdge = m_btnProjectRect.x - FromDIP( 10 );
        }

        m_itemRects.clear();

        if( !m_targetFrame || !IsFrameInTopLevelList( m_targetFrame ) )
            return;

        wxMenuBar* mb = m_targetFrame->GetMenuBar();
        if( !mb )
            return;

        int x = FromDIP( 6 );
        const int padX = FromDIP( 9 );
        const size_t count = mb->GetMenuCount();

        for( size_t i = 0; i < count; ++i )
        {
            wxString label = mb->GetMenuLabelText( i );
            if( label.IsEmpty() )
                label = mb->GetMenuLabel( i );

            label.Replace( wxS( "&" ), wxEmptyString );

            wxSize textSz = dc.GetTextExtent( label );
            int itemW = textSz.x + padX * 2;

            if( x + itemW > rightEdge )
                break;

            wxRect rect( x, FromDIP( 2 ), itemW, sz.y - FromDIP( 5 ) );
            m_itemRects.push_back( rect );

            bool isActive = ( static_cast<int>( i ) == m_openIdx );
            bool isHover  = ( static_cast<int>( i ) == m_hoverIdx );

            if( isActive )
            {
                dc.SetBrush( wxBrush( wxColour( 208, 226, 250 ) ) );
                dc.SetPen( wxPen( wxColour( 146, 186, 240 ), 1 ) );
                dc.DrawRoundedRectangle( rect, 3 );
                dc.SetTextForeground( wxColour( 18, 72, 168 ) );
            }
            else if( isHover )
            {
                dc.SetBrush( wxBrush( wxColour( 228, 236, 248 ) ) );
                dc.SetPen( *wxTRANSPARENT_PEN );
                dc.DrawRoundedRectangle( rect, 3 );
                dc.SetTextForeground( wxColour( 24, 32, 46 ) );
            }
            else
            {
                dc.SetTextForeground( wxColour( 38, 44, 54 ) );
            }

            int textY = rect.y + ( rect.height - textSz.y ) / 2;
            dc.DrawText( label, rect.x + padX, textY );

            x += itemW + FromDIP( 2 );
        }
    }

    int HitTestMenu( const wxPoint& aPt ) const
    {
        for( size_t i = 0; i < m_itemRects.size(); ++i )
        {
            if( m_itemRects[i].Contains( aPt ) )
                return static_cast<int>( i );
        }
        return -1;
    }

    int HitTestQuickBtn( const wxPoint& aPt ) const
    {
        if( !m_btnProjectRect.IsEmpty() && m_btnProjectRect.Contains( aPt ) )
            return 1;
        if( !m_btnPropsRect.IsEmpty() && m_btnPropsRect.Contains( aPt ) )
            return 2;
        return 0;
    }

    void OnMouseMove( wxMouseEvent& aEvt )
    {
        wxPoint pt = aEvt.GetPosition();
        int idx = HitTestMenu( pt );
        int qbtn = HitTestQuickBtn( pt );

        if( idx != m_hoverIdx || qbtn != m_hoverQuickBtn )
        {
            m_hoverIdx      = idx;
            m_hoverQuickBtn = qbtn;
            Refresh();
        }
    }

    void OnMouseLeave( wxMouseEvent& )
    {
        if( m_hoverIdx != -1 || m_hoverQuickBtn != 0 )
        {
            m_hoverIdx      = -1;
            m_hoverQuickBtn = 0;
            Refresh();
        }
    }

    void OnMouseDown( wxMouseEvent& aEvt )
    {
        wxPoint pt = aEvt.GetPosition();

        int qbtn = HitTestQuickBtn( pt );
        if( qbtn == 1 && m_onToggleProjectTree )
        {
            m_onToggleProjectTree();
            if( m_onRestoreFocus )
                m_onRestoreFocus();
            return;
        }
        if( qbtn == 2 && m_onToggleProperties )
        {
            m_onToggleProperties();
            if( m_onRestoreFocus )
                m_onRestoreFocus();
            return;
        }

        int idx = HitTestMenu( pt );
        if( idx < 0 || !m_targetFrame || !IsFrameInTopLevelList( m_targetFrame ) )
            return;

        wxMenuBar* mb = m_targetFrame->GetMenuBar();
        if( !mb || static_cast<size_t>( idx ) >= mb->GetMenuCount() )
            return;

        wxMenu* menu = mb->GetMenu( static_cast<size_t>( idx ) );
        if( !menu )
            return;

        m_openIdx = idx;
        Refresh();
        Update();

        wxString clickedItemLabel;
        wxFrame* dispatchedFrame = m_targetFrame;

#ifdef __WXMSW__
        HMENU hSubMenu = reinterpret_cast<HMENU>( menu->GetHMenu() );
        HWND targetHwnd = reinterpret_cast<HWND>( m_targetFrame->GetHWND() );
        wxWindow* topWin = wxGetTopLevelParent( this );
        HWND topHwnd = topWin ? reinterpret_cast<HWND>( topWin->GetHWND() ) : targetHwnd;

        if( hSubMenu && targetHwnd && ::IsWindow( targetHwnd ) )
        {
            wxPoint screenPt = ClientToScreen( wxPoint( m_itemRects[idx].x, GetClientSize().y ) );
            ::SetForegroundWindow( topHwnd );
            UINT cmdId = ::TrackPopupMenuEx( hSubMenu,
                                             TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD,
                                             screenPt.x, screenPt.y, targetHwnd, nullptr );
            ::PostMessage( topHwnd, WM_NULL, 0, 0 );

            if( cmdId != 0 )
            {
                if( wxMenuItem* mi = menu->FindItem( static_cast<int>( cmdId ) ) )
                    clickedItemLabel = mi->GetItemLabelText();

                ::PostMessage( targetHwnd, WM_COMMAND, MAKEWPARAM( cmdId, 0 ), 0 );
            }
        }
#endif
        m_openIdx  = -1;
        m_hoverIdx = -1;
        Refresh();

        if( !clickedItemLabel.IsEmpty() && m_onMenuCommand )
            m_onMenuCommand( dispatchedFrame, clickedItemLabel );
        else if( m_onRestoreFocus )
            m_onRestoreFocus();
    }
};


/**
 * 标签页内部宿主面板：承载一个被 Win32 SetParent 嵌入的独立 KiCad 编辑器 wxFrame。
 */
class LANTU_EMBEDDED_FRAME_PAGE : public wxPanel
{
public:
    LANTU_EMBEDDED_FRAME_PAGE( wxWindow* aParent, wxFrame* aEmbeddedFrame, wxFrame* aManagerFrame )
        : wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxCLIP_CHILDREN ),
          m_frame( aEmbeddedFrame ),
          m_managerFrame( aManagerFrame ),
          m_suppressRaise( false )
    {
        SetBackgroundColour( wxColour( 240, 243, 248 ) );
        Bind( wxEVT_SIZE, &LANTU_EMBEDDED_FRAME_PAGE::OnSize, this );
        Bind( wxEVT_SHOW, &LANTU_EMBEDDED_FRAME_PAGE::OnShow, this );

        AttachFrame();
    }

    ~LANTU_EMBEDDED_FRAME_PAGE() override
    {
        DetachFrame();
    }

    wxFrame* GetEmbeddedFrame() const { return m_frame; }
    bool     IsSuppressingRaise() const { return m_suppressRaise; }

    void NotifyRequestedToActivate()
    {
        if( !m_managerFrame || !m_frame || !IsFrameInTopLevelList( m_frame ) )
            return;

        wxCommandEvent evt( wxEVT_MENU, ID_LANTU_ACTIVATE_TAB );
        evt.SetClientData( m_frame );
        wxPostEvent( m_managerFrame, evt );
    }

    void AttachFrame()
    {
#ifdef __WXMSW__
        if( !m_frame || !IsFrameInTopLevelList( m_frame ) )
            return;

        HWND childHwnd = reinterpret_cast<HWND>( m_frame->GetHWND() );
        HWND hostHwnd  = reinterpret_cast<HWND>( GetHWND() );
        if( !childHwnd || !hostHwnd || !::IsWindow( childHwnd ) )
            return;

        m_suppressRaise = true;

        LONG_PTR style = ::GetWindowLongPtr( childHwnd, GWL_STYLE );
        style &= ~( WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
                    | WS_SYSMENU | WS_BORDER | WS_DLGFRAME | WS_MAXIMIZE | WS_MINIMIZE );
        style |= ( WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS );
        ::SetWindowLongPtr( childHwnd, GWL_STYLE, style );

        LONG_PTR exStyle = ::GetWindowLongPtr( childHwnd, GWL_EXSTYLE );
        exStyle &= ~( WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE );
        ::SetWindowLongPtr( childHwnd, GWL_EXSTYLE, exStyle );

        if( ::GetMenu( childHwnd ) != nullptr )
            ::SetMenu( childHwnd, nullptr );

        ::SetParent( childHwnd, hostHwnd );

        // 挂载 Win32 子类钩子，捕获对该内嵌窗口的 BringWindowToTop / ShowWindow / SetFocus
        if( s_subclassedFrames.find( childHwnd ) == s_subclassedFrames.end() )
        {
            WNDPROC origProc = reinterpret_cast<WNDPROC>(
                    ::SetWindowLongPtrW( childHwnd, GWLP_WNDPROC,
                                         reinterpret_cast<LONG_PTR>( &LantuEmbeddedChildWndProc ) ) );
            if( origProc )
                s_subclassedFrames[childHwnd] = LANTU_SUBCLASS_INFO{ origProc, this };
        }
        else
        {
            s_subclassedFrames[childHwnd].hostPage = this;
        }

        wxSize sz = GetClientSize();
        ::SetWindowPos( childHwnd, HWND_TOP, 0, 0, std::max<int>( 1, sz.x ), std::max<int>( 1, sz.y ),
                        SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW );

        m_suppressRaise = false;
#endif
    }

    void DetachFrame()
    {
#ifdef __WXMSW__
        if( m_frame && IsFrameInTopLevelList( m_frame ) )
        {
            HWND childHwnd = reinterpret_cast<HWND>( m_frame->GetHWND() );
            HWND hostHwnd  = reinterpret_cast<HWND>( GetHWND() );
            if( childHwnd && ::IsWindow( childHwnd ) )
            {
                auto it = s_subclassedFrames.find( childHwnd );
                if( it != s_subclassedFrames.end() )
                {
                    if( ::GetWindowLongPtrW( childHwnd, GWLP_WNDPROC )
                        == reinterpret_cast<LONG_PTR>( &LantuEmbeddedChildWndProc ) )
                    {
                        ::SetWindowLongPtrW( childHwnd, GWLP_WNDPROC,
                                             reinterpret_cast<LONG_PTR>( it->second.origProc ) );
                    }
                    s_subclassedFrames.erase( it );
                }

                if( ::GetParent( childHwnd ) == hostHwnd )
                {
                    m_suppressRaise = true;
                    ::ShowWindow( childHwnd, SW_HIDE );
                    ::SetParent( childHwnd, nullptr );

                    // 若窗口只是被隐藏而非销毁，还原顶层样式防止下次打开状态异常
                    if( !m_frame->IsBeingDeleted() )
                    {
                        LONG_PTR style = ::GetWindowLongPtr( childHwnd, GWL_STYLE );
                        style &= ~WS_CHILD;
                        style |= WS_OVERLAPPEDWINDOW;
                        ::SetWindowLongPtr( childHwnd, GWL_STYLE, style );
                    }

                    m_suppressRaise = false;
                }
            }
        }
#endif
        m_frame = nullptr;
    }

    void SyncChildSize()
    {
#ifdef __WXMSW__
        if( !m_frame || !IsFrameInTopLevelList( m_frame ) )
            return;

        HWND childHwnd = reinterpret_cast<HWND>( m_frame->GetHWND() );
        HWND hostHwnd  = reinterpret_cast<HWND>( GetHWND() );
        if( !childHwnd || !hostHwnd || !::IsWindow( childHwnd ) )
            return;

        if( ::GetParent( childHwnd ) != hostHwnd )
            AttachFrame();

        m_suppressRaise = true;

        if( ::GetMenu( childHwnd ) != nullptr )
            ::SetMenu( childHwnd, nullptr );

        LONG_PTR style = ::GetWindowLongPtr( childHwnd, GWL_STYLE );
        if( style & ( WS_CAPTION | WS_MAXIMIZE | WS_THICKFRAME ) )
        {
            style &= ~( WS_CAPTION | WS_MAXIMIZE | WS_THICKFRAME | WS_POPUP );
            style |= ( WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS );
            ::SetWindowLongPtr( childHwnd, GWL_STYLE, style );
        }

        wxSize sz = GetClientSize();
        if( sz.x > 0 && sz.y > 0 )
        {
            RECT rc{};
            ::GetWindowRect( childHwnd, &rc );
            POINT topLeft{ rc.left, rc.top };
            ::ScreenToClient( hostHwnd, &topLeft );

            int curW = rc.right - rc.left;
            int curH = rc.bottom - rc.top;

            if( topLeft.x != 0 || topLeft.y != 0 || curW != sz.x || curH != sz.y )
            {
                ::SetWindowPos( childHwnd, nullptr, 0, 0, sz.x, sz.y,
                                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
            }
        }

        m_suppressRaise = false;
#endif
    }

    void FocusEmbeddedFrame()
    {
        if( !m_frame || !IsFrameInTopLevelList( m_frame ) )
            return;

        m_suppressRaise = true;

        if( auto* edaFrame = dynamic_cast<EDA_BASE_FRAME*>( m_frame ) )
        {
            if( wxWindow* canvas = edaFrame->GetToolCanvas() )
            {
                canvas->SetFocus();
                m_suppressRaise = false;
                return;
            }
        }

        m_frame->SetFocus();
#ifdef __WXMSW__
        HWND childHwnd = reinterpret_cast<HWND>( m_frame->GetHWND() );
        if( childHwnd && ::IsWindow( childHwnd ) )
            ::SetFocus( childHwnd );
#endif
        m_suppressRaise = false;
    }

private:
    wxFrame* m_frame;
    wxFrame* m_managerFrame;
    bool     m_suppressRaise;

    void OnSize( wxSizeEvent& aEvt )
    {
        SyncChildSize();
        aEvt.Skip();
    }

    void OnShow( wxShowEvent& aEvt )
    {
        if( aEvt.IsShown() )
        {
            SyncChildSize();
            CallAfter( [this]() { SyncChildSize(); } );
        }
        aEvt.Skip();
    }
};


#ifdef __WXMSW__
static LRESULT CALLBACK LantuEmbeddedChildWndProc( HWND aHwnd, UINT aMsg, WPARAM aWParam, LPARAM aLParam )
{
    auto it = s_subclassedFrames.find( aHwnd );
    if( it == s_subclassedFrames.end() )
        return ::DefWindowProcW( aHwnd, aMsg, aWParam, aLParam );

    WNDPROC                    origProc = it->second.origProc;
    LANTU_EMBEDDED_FRAME_PAGE* host     = it->second.hostPage;

    if( aMsg == WM_NCDESTROY )
    {
        s_subclassedFrames.erase( it );
        return ::CallWindowProcW( origProc, aHwnd, aMsg, aWParam, aLParam );
    }

    if( host && !host->IsSuppressingRaise() && !host->IsShown() )
    {
        bool shouldActivate = false;

        if( aMsg == WM_WINDOWPOSCHANGING )
        {
            if( auto* wp = reinterpret_cast<WINDOWPOS*>( aLParam ) )
            {
                bool bringToTop   = ( ( wp->flags & SWP_NOZORDER ) == 0 && wp->hwndInsertAfter == HWND_TOP );
                bool explicitShow = ( ( wp->flags & SWP_SHOWWINDOW ) != 0 );
                if( bringToTop || explicitShow )
                    shouldActivate = true;
            }
        }
        else if( aMsg == WM_SHOWWINDOW && aWParam == TRUE && aLParam == 0 )
        {
            shouldActivate = true;
        }
        else if( aMsg == WM_ACTIVATE && LOWORD( aWParam ) != WA_INACTIVE )
        {
            shouldActivate = true;
        }
        else if( aMsg == WM_SETFOCUS )
        {
            shouldActivate = true;
        }

        if( shouldActivate )
            host->NotifyRequestedToActivate();
    }

    return ::CallWindowProcW( origProc, aHwnd, aMsg, aWParam, aLParam );
}
#endif


/**
 * 单窗口标签调度与空间布局优化中心：
 * 1. 自动收纳原理图 / PCB / 3D 查看器 / 符号与封装编辑器为顶部标签页；
 * 2. 通过 wxEventFilter 全局接管 Alt+3 与工具栏/菜单/工程树跨标签跳转，解决重复按 Alt+3 不跳转问题；
 * 3. 切页/关页后异步恢复画布键盘焦点，保证快捷键 100% 持续可用。
 */
class LANTU_TAB_COORDINATOR : public wxEvtHandler, public wxEventFilter
{
public:
    LANTU_TAB_COORDINATOR( KICAD_MANAGER_FRAME* aManager, wxAuiManager* aAuiMgr,
                           wxWindow* aProjectTreePane, wxWindow* aLeftToolbar,
                           wxAuiNotebook* aNotebook, LANTU_PROXY_MENUBAR* aMenuBar )
        : m_manager( aManager ),
          m_auiMgr( aAuiMgr ),
          m_projectTreePane( aProjectTreePane ),
          m_leftToolbar( aLeftToolbar ),
          m_notebook( aNotebook ),
          m_menuBar( aMenuBar ),
          m_shuttingDown( false ),
          m_filterInstalled( false )
    {
        m_timer.SetOwner( this );
        Bind( wxEVT_TIMER, &LANTU_TAB_COORDINATOR::OnTimer, this, m_timer.GetId() );
        m_notebook->Bind( wxEVT_AUINOTEBOOK_PAGE_CHANGED, &LANTU_TAB_COORDINATOR::OnPageChanged, this );

        if( m_menuBar )
        {
            m_menuBar->SetCallbacks(
                    [this]() { ToggleProjectTree(); },
                    [this]() { ToggleActivePropertiesPane(); },
                    [this]( wxFrame* aFromFrame, const wxString& aLabel ) { HandleProxyMenuCommand( aFromFrame, aLabel ); },
                    [this]() { ScheduleFocusActiveTab(); } );
            m_menuBar->SetTargetFrame( m_manager, false );
        }

        wxEvtHandler::AddFilter( this );
        m_filterInstalled = true;

#ifdef __WXMSW__
        if( m_manager )
            ::SetMenu( reinterpret_cast<HWND>( m_manager->GetHWND() ), nullptr );
#endif

        SyncChromeToActiveTab();
        m_timer.Start( 120 );
    }

    ~LANTU_TAB_COORDINATOR() override
    {
        Shutdown();
    }

    void SetLeftToolbar( wxWindow* aLeftToolbar )
    {
        m_leftToolbar = aLeftToolbar;
        SyncChromeToActiveTab();
    }

    void Shutdown()
    {
        if( m_shuttingDown )
            return;

        m_shuttingDown = true;
        m_timer.Stop();

        if( m_filterInstalled )
        {
            wxEvtHandler::RemoveFilter( this );
            m_filterInstalled = false;
        }

        if( m_notebook )
            m_notebook->Unbind( wxEVT_AUINOTEBOOK_PAGE_CHANGED, &LANTU_TAB_COORDINATOR::OnPageChanged, this );

        DetachAllEmbeddedFrames();
    }

    void DetachAllEmbeddedFrames()
    {
        if( !m_notebook )
            return;

        for( size_t i = 0; i < m_notebook->GetPageCount(); ++i )
        {
            if( auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( i ) ) )
                host->DetachFrame();
        }
    }

    void CleanupClosedTabs()
    {
        if( m_shuttingDown || !m_notebook )
            return;

        bool removedAny = false;

        for( int i = static_cast<int>( m_notebook->GetPageCount() ) - 1; i >= 0; --i )
        {
            auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( i ) );
            if( !host )
                continue;

            wxFrame* frame = host->GetEmbeddedFrame();
            if( !frame || !IsFrameInTopLevelList( frame ) || frame->IsBeingDeleted() || !frame->IsShown() )
            {
                host->DetachFrame();
                m_notebook->DeletePage( i );
                removedAny = true;
            }
        }

        SyncChromeToActiveTab();

        if( removedAny )
            ScheduleFocusActiveTab();
    }

    void RemoveTabForFrame( wxFrame* aFrame )
    {
        if( !m_notebook || !aFrame )
            return;

        for( size_t i = 0; i < m_notebook->GetPageCount(); ++i )
        {
            auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( i ) );
            if( host && host->GetEmbeddedFrame() == aFrame )
            {
                host->DetachFrame();
                m_notebook->DeletePage( i );
                SyncChromeToActiveTab();
                ScheduleFocusActiveTab();
                return;
            }
        }
    }

    void ActivateTabForFrame( wxFrame* aFrame )
    {
        if( m_shuttingDown || !m_notebook || !aFrame || !IsFrameInTopLevelList( aFrame ) || aFrame->IsBeingDeleted() )
            return;

        if( FindPageIndexByFrame( aFrame ) == wxNOT_FOUND && ShouldEmbedFrame( aFrame ) )
            EmbedFrame( aFrame );

        int idx = FindPageIndexByFrame( aFrame );
        if( idx != wxNOT_FOUND )
        {
            if( m_notebook->GetSelection() != idx )
                m_notebook->SetSelection( idx );

            if( auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( idx ) ) )
                host->SyncChildSize();

            SyncChromeToActiveTab();
            ScheduleFocusActiveTab();
        }
    }

    void FocusActiveTab()
    {
        if( m_shuttingDown || !m_notebook )
            return;

        int sel = m_notebook->GetSelection();
        if( sel >= 0 && static_cast<size_t>( sel ) < m_notebook->GetPageCount() )
        {
            if( auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( sel ) ) )
            {
                host->SyncChildSize();
                host->FocusEmbeddedFrame();
            }
        }
    }

    void ScheduleFocusActiveTab()
    {
        if( m_shuttingDown || !m_manager )
            return;

        m_manager->CallAfter( [this]() {
            if( !m_shuttingDown )
                FocusActiveTab();
        } );
    }

    void SyncActiveTabSize()
    {
        if( m_shuttingDown || !m_notebook )
            return;

        int sel = m_notebook->GetSelection();
        if( sel >= 0 && static_cast<size_t>( sel ) < m_notebook->GetPageCount() )
        {
            if( auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( sel ) ) )
                host->SyncChildSize();
        }

        if( m_menuBar )
            m_menuBar->Refresh();
    }

    wxFrame* GetActiveEmbeddedFrame() const
    {
        if( !m_notebook )
            return nullptr;

        int sel = m_notebook->GetSelection();
        if( sel >= 0 && static_cast<size_t>( sel ) < m_notebook->GetPageCount() )
        {
            if( auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( sel ) ) )
            {
                if( wxFrame* f = host->GetEmbeddedFrame(); f && IsFrameInTopLevelList( f ) && !f->IsBeingDeleted() )
                    return f;
            }
        }
        return nullptr;
    }

    void ToggleProjectTree()
    {
        if( !m_auiMgr || !m_projectTreePane )
            return;

        wxAuiPaneInfo& pane = m_auiMgr->GetPane( m_projectTreePane );
        if( pane.IsOk() )
        {
            pane.Show( !pane.IsShown() );
            m_auiMgr->Update();
            SyncActiveTabSize();
            SyncChromeToActiveTab();
        }
    }

    void ToggleActivePropertiesPane()
    {
        wxFrame* activeFrame = GetActiveEmbeddedFrame();
        if( !activeFrame )
            return;

        wxAuiManager* childAui = wxAuiManager::GetManager( activeFrame );
        if( !childAui )
            return;

        wxAuiPaneInfoArray& panes = childAui->GetAllPanes();
        for( size_t i = 0; i < panes.GetCount(); ++i )
        {
            wxAuiPaneInfo& p = panes.Item( i );
            if( p.name.Lower().Contains( wxS( "properties" ) ) )
            {
                bool nextShow = !p.IsShown();
                if( nextShow && p.dock_direction == wxAUI_DOCK_LEFT )
                    p.Right().Layer( 4 ).Position( 0 );
                p.Show( nextShow );
                childAui->Update();
                break;
            }
        }

        SyncChromeToActiveTab();
    }

    void SyncChromeToActiveTab()
    {
        if( m_shuttingDown || !m_manager || !m_notebook )
            return;

        wxFrame* activeEditor = GetActiveEmbeddedFrame();
        bool     isEditorTab  = ( activeEditor != nullptr );
        wxFrame* menuTarget   = isEditorTab ? activeEditor : static_cast<wxFrame*>( m_manager );

        bool needAuiUpdate = false;
        if( m_auiMgr && m_leftToolbar )
        {
            wxAuiPaneInfo& tbPane = m_auiMgr->GetPane( m_leftToolbar );
            if( tbPane.IsOk() && tbPane.IsShown() == isEditorTab )
            {
                tbPane.Show( !isEditorTab );
                needAuiUpdate = true;
            }
        }

        if( wxStatusBar* sb = m_manager->GetStatusBar() )
        {
            if( sb->IsShown() == isEditorTab )
            {
                sb->Show( !isEditorTab );
                m_manager->SendSizeEvent();
            }
        }

        if( needAuiUpdate )
        {
            m_auiMgr->Update();
            SyncActiveTabSize();
        }

        if( m_menuBar )
        {
            m_menuBar->SetTargetFrame( menuTarget, isEditorTab );

            bool prjShown = m_auiMgr && m_projectTreePane
                    && m_auiMgr->GetPane( m_projectTreePane ).IsShown();
            bool propShown = false;

            if( activeEditor )
            {
                if( wxAuiManager* childAui = wxAuiManager::GetManager( activeEditor ) )
                {
                    wxAuiPaneInfoArray& panes = childAui->GetAllPanes();
                    for( size_t i = 0; i < panes.GetCount(); ++i )
                    {
                        if( panes.Item( i ).name.Lower().Contains( wxS( "properties" ) ) )
                        {
                            propShown = panes.Item( i ).IsShown();
                            break;
                        }
                    }
                }
            }

            m_menuBar->SetQuickStates( prjShown, propShown );
        }
    }

    void ScanAndSyncFrames()
    {
        if( m_shuttingDown || !m_manager || !m_notebook )
            return;

#ifdef __WXMSW__
        HWND mgrHwnd = reinterpret_cast<HWND>( m_manager->GetHWND() );
        if( mgrHwnd && ::GetMenu( mgrHwnd ) != nullptr )
            ::SetMenu( mgrHwnd, nullptr );

        std::vector<wxFrame*> toEmbed;
        for( wxWindow* win : wxTopLevelWindows )
        {
            wxFrame* frame = dynamic_cast<wxFrame*>( win );
            if( !frame || frame == m_manager )
                continue;

            if( !frame->IsShown() || frame->IsBeingDeleted() )
                continue;

            if( FindPageIndexByFrame( frame ) != wxNOT_FOUND )
                continue;

            if( ShouldEmbedFrame( frame ) )
                toEmbed.push_back( frame );
        }

        for( wxFrame* frame : toEmbed )
            EmbedFrame( frame );

        bool removedAny = false;
        for( int i = static_cast<int>( m_notebook->GetPageCount() ) - 1; i >= 0; --i )
        {
            auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( i ) );
            if( !host )
                continue;

            wxFrame* frame = host->GetEmbeddedFrame();
            if( !frame || !IsFrameInTopLevelList( frame ) || frame->IsBeingDeleted() || !frame->IsShown() )
            {
                host->DetachFrame();
                m_notebook->DeletePage( i );
                removedAny = true;
                continue;
            }

            wxString desiredTitle = BuildTabTitle( frame );
            if( m_notebook->GetPageText( i ) != desiredTitle )
                m_notebook->SetPageText( i, desiredTitle );

            if( m_notebook->GetSelection() == i )
                host->SyncChildSize();
        }

        if( removedAny )
            ScheduleFocusActiveTab();
#endif

        SyncChromeToActiveTab();
    }

    void RefreshMenuBarVisual()
    {
        if( m_menuBar )
            m_menuBar->Refresh();
    }

    /**
     * 全局事件过滤器：
     * 1. 解决反复按 Alt+3 无法跳转到已打开 3D 标签页（及关掉重开时焦点丢失）的问题；
     * 2. 捕获工具栏 3D 按钮、工程树双击切换已打开的原理图/PCB 标签页。
     */
    int FilterEvent( wxEvent& aEvent ) override
    {
        if( m_shuttingDown || !m_manager || !m_notebook )
            return Event_Skip;

        const wxEventType evtType = aEvent.GetEventType();

        // 1. 拦截全局 Alt+3 快捷键（兼容大小写、小键盘及中文输入法状态）
        if( evtType == wxEVT_CHAR_HOOK )
        {
            const auto& keyEvt  = static_cast<const wxKeyEvent&>( aEvent );
            const int   keyCode = keyEvt.GetKeyCode();
            const int   rawCode = static_cast<int>( keyEvt.GetRawKeyCode() );

            const bool isAltOnly = keyEvt.AltDown() && !keyEvt.ControlDown() && !keyEvt.ShiftDown();
            const bool isKey3    = ( keyCode == '3' || keyCode == WXK_NUMPAD3 || rawCode == '3' || rawCode == 0x63 );

            if( isAltOnly && isKey3 )
            {
                auto now = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>( now - m_lastAlt3Time ).count();
                if( elapsedMs < 150 )
                    return Event_Processed;

                m_lastAlt3Time = now;

                wxFrame* viewer3D = FindEmbeddedOrLiveFrame( FRAME_PCB_DISPLAY3D );
                if( viewer3D )
                {
                    // 3D 查看器已打开：若当前不在 3D 页，直接切过去！
                    if( GetActiveEmbeddedFrame() != viewer3D )
                    {
                        wxFrame*  pcbFrame = FindEmbeddedOrLiveFrame( FRAME_PCB_EDITOR );
                        wxWindow* focusWin = wxWindow::FindFocus();
                        bool      focusOnPcbCanvas = false;

                        if( auto* edaPcb = dynamic_cast<EDA_BASE_FRAME*>( pcbFrame ) )
                            focusOnPcbCanvas = ( focusWin != nullptr && IsDescendantOf( focusWin, edaPcb->GetToolCanvas() ) );

                        m_manager->CallAfter( [this, viewer3D]() {
                            if( !m_shuttingDown )
                                ActivateTabForFrame( viewer3D );
                        } );

                        // 若焦点正在 PCB 画布上，放行让 KiCad 同步刷新一次 3D 视图，随后 CallAfter 立刻切到 3D 标签；
                        // 若焦点在标签栏/工程树等处，直接消费避免系统报警音并切到 3D 标签。
                        return focusOnPcbCanvas ? Event_Skip : Event_Processed;
                    }

                    return Event_Skip;
                }
                else
                {
                    // 3D 查看器尚未打开（或已被关闭）：确保 PCB 编辑器能可靠响应 Alt+3 打开新 3D 标签
                    wxFrame* pcbFrame = FindEmbeddedOrLiveFrame( FRAME_PCB_EDITOR );
                    if( pcbFrame )
                    {
                        wxWindow* focusWin = wxWindow::FindFocus();
                        bool      focusInPcb = ( focusWin != nullptr && IsDescendantOf( focusWin, pcbFrame ) );

                        m_manager->CallAfter( [this]() {
                            if( !m_shuttingDown )
                                ScanAndSyncFrames();
                        } );

                        if( !focusInPcb )
                        {
                            if( Trigger3DViewerCommandOnFrame( pcbFrame ) )
                                return Event_Processed;
                        }
                    }
                }
            }
        }
        // 2. 捕获 PCB / 封装编辑器工具栏上的「3D 查看器」按钮点击
        else if( evtType == wxEVT_LEFT_UP )
        {
            if( auto* win = dynamic_cast<wxWindow*>( aEvent.GetEventObject() ) )
            {
                wxString tip = win->GetToolTipText();
                if( !tip.IsEmpty() && ( tip.Contains( wxS( "3D" ) ) || tip.Contains( wxS( "Alt+3" ) ) ) )
                {
                    wxFrame* viewer3D = FindEmbeddedOrLiveFrame( FRAME_PCB_DISPLAY3D );
                    if( viewer3D && GetActiveEmbeddedFrame() != viewer3D )
                    {
                        m_manager->CallAfter( [this, viewer3D]() {
                            if( !m_shuttingDown )
                                ActivateTabForFrame( viewer3D );
                        } );
                    }
                    else
                    {
                        m_manager->CallAfter( [this]() {
                            if( !m_shuttingDown )
                                ScanAndSyncFrames();
                        } );
                    }
                }
            }
        }
        // 3. 双击左侧【工程文件】树中的 .kicad_sch / .kicad_pcb 时，若标签已打开则直接切换过去
        else if( evtType == wxEVT_TREE_ITEM_ACTIVATED )
        {
            if( auto* tree = dynamic_cast<wxTreeCtrl*>( aEvent.GetEventObject() ) )
            {
                if( m_projectTreePane && IsDescendantOf( tree, m_projectTreePane ) )
                {
                    const auto& treeEvt = static_cast<const wxTreeEvent&>( aEvent );
                    wxString    itemTxt = tree->GetItemText( treeEvt.GetItem() ).Lower();

                    if( itemTxt.EndsWith( wxS( ".kicad_pcb" ) ) || itemTxt.EndsWith( wxS( ".brd" ) ) )
                    {
                        if( wxFrame* pcbFrame = FindEmbeddedOrLiveFrame( FRAME_PCB_EDITOR ) )
                        {
                            m_manager->CallAfter( [this, pcbFrame]() {
                                if( !m_shuttingDown )
                                    ActivateTabForFrame( pcbFrame );
                            } );
                        }
                    }
                    else if( itemTxt.EndsWith( wxS( ".kicad_sch" ) ) || itemTxt.EndsWith( wxS( ".sch" ) ) )
                    {
                        if( wxFrame* schFrame = FindEmbeddedOrLiveFrame( FRAME_SCH ) )
                        {
                            m_manager->CallAfter( [this, schFrame]() {
                                if( !m_shuttingDown )
                                    ActivateTabForFrame( schFrame );
                            } );
                        }
                    }
                }
            }
        }

        return Event_Skip;
    }

private:
    KICAD_MANAGER_FRAME*                  m_manager;
    wxAuiManager*                         m_auiMgr;
    wxWindow*                             m_projectTreePane;
    wxWindow*                             m_leftToolbar;
    wxAuiNotebook*                        m_notebook;
    LANTU_PROXY_MENUBAR*                  m_menuBar;
    wxTimer                               m_timer;
    bool                                  m_shuttingDown;
    bool                                  m_filterInstalled;
    std::chrono::steady_clock::time_point m_lastAlt3Time{};

    int FindPageIndexByFrame( wxFrame* aFrame ) const
    {
        if( !m_notebook || !aFrame )
            return wxNOT_FOUND;

        for( size_t i = 0; i < m_notebook->GetPageCount(); ++i )
        {
            auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( i ) );
            if( host && host->GetEmbeddedFrame() == aFrame )
                return static_cast<int>( i );
        }
        return wxNOT_FOUND;
    }

    bool IsFrameMatchingType( wxFrame* aFrame, FRAME_T aType ) const
    {
        if( !aFrame || !IsFrameInTopLevelList( aFrame ) || aFrame->IsBeingDeleted() )
            return false;

        if( auto* edaFrame = dynamic_cast<EDA_BASE_FRAME*>( aFrame ) )
        {
            if( edaFrame->IsType( aType ) )
                return true;
        }

        if( aType == FRAME_PCB_DISPLAY3D && aFrame->GetName() == wxS( "Frame3D" ) )
            return true;

        return false;
    }

    wxFrame* FindEmbeddedOrLiveFrame( FRAME_T aType ) const
    {
        if( m_notebook )
        {
            for( size_t i = 0; i < m_notebook->GetPageCount(); ++i )
            {
                auto* host = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( i ) );
                if( host && IsFrameMatchingType( host->GetEmbeddedFrame(), aType ) )
                    return host->GetEmbeddedFrame();
            }
        }

        for( wxWindow* win : wxTopLevelWindows )
        {
            if( auto* frame = dynamic_cast<wxFrame*>( win ) )
            {
                if( frame->IsShown() && IsFrameMatchingType( frame, aType ) )
                    return frame;
            }
        }

        return nullptr;
    }

    bool Trigger3DViewerCommandOnFrame( wxFrame* aFrame )
    {
        if( !aFrame || !IsFrameInTopLevelList( aFrame ) )
            return false;

        wxMenuBar* mb = aFrame->GetMenuBar();
        if( !mb )
            return false;

        int accelCmdId    = wxID_NONE;
        int fallbackCmdId = wxID_NONE;

        std::function<void( wxMenu* )> scanMenu = [&]( wxMenu* aMenu )
        {
            if( !aMenu || accelCmdId != wxID_NONE )
                return;

            for( wxMenuItem* item : aMenu->GetMenuItems() )
            {
                if( !item || item->IsSeparator() )
                    continue;

                if( item->IsSubMenu() )
                {
                    scanMenu( item->GetSubMenu() );
                    if( accelCmdId != wxID_NONE )
                        return;
                    continue;
                }

                std::unique_ptr<wxAcceleratorEntry> accel( item->GetAccel() );
                if( accel )
                {
                    int flags = accel->GetFlags();
                    int key   = accel->GetKeyCode();
                    if( ( flags & wxACCEL_ALT ) && !( flags & wxACCEL_CTRL ) && !( flags & wxACCEL_SHIFT )
                        && ( key == '3' || key == WXK_NUMPAD3 ) )
                    {
                        accelCmdId = item->GetId();
                        return;
                    }
                }

                if( fallbackCmdId == wxID_NONE )
                {
                    wxString lbl = item->GetItemLabelText();
                    if( lbl.Contains( wxS( "3D" ) ) && !lbl.Lower().Contains( wxS( "export" ) )
                        && !lbl.Contains( wxString::FromUTF8( "导出" ) ) )
                    {
                        fallbackCmdId = item->GetId();
                    }
                }
            }
        };

        for( size_t i = 0; i < mb->GetMenuCount() && accelCmdId == wxID_NONE; ++i )
            scanMenu( mb->GetMenu( i ) );

        int cmdId = ( accelCmdId != wxID_NONE ) ? accelCmdId : fallbackCmdId;
        if( cmdId == wxID_NONE )
            return false;

        wxCommandEvent cmdEvt( wxEVT_MENU, cmdId );
        wxPostEvent( aFrame, cmdEvt );
        return true;
    }

    void HandleProxyMenuCommand( wxFrame* aFromFrame, const wxString& aMenuLabel )
    {
        if( m_shuttingDown || !m_manager )
            return;

        wxString lower = aMenuLabel.Lower();

        if( ( aMenuLabel.Contains( wxS( "3D" ) ) )
            && !lower.Contains( wxS( "export" ) )
            && !aMenuLabel.Contains( wxString::FromUTF8( "导出" ) ) )
        {
            m_manager->CallAfter( [this]() {
                if( m_shuttingDown )
                    return;
                ScanAndSyncFrames();
                if( wxFrame* viewer3D = FindEmbeddedOrLiveFrame( FRAME_PCB_DISPLAY3D ) )
                    ActivateTabForFrame( viewer3D );
            } );
            return;
        }

        if( lower.Contains( wxS( "pcb" ) )
            && ( aMenuLabel.Contains( wxString::FromUTF8( "切换" ) )
                 || aMenuLabel.Contains( wxString::FromUTF8( "编辑" ) )
                 || lower.Contains( wxS( "switch" ) ) ) )
        {
            m_manager->CallAfter( [this]() {
                if( m_shuttingDown )
                    return;
                ScanAndSyncFrames();
                if( wxFrame* pcbFrame = FindEmbeddedOrLiveFrame( FRAME_PCB_EDITOR ) )
                    ActivateTabForFrame( pcbFrame );
            } );
            return;
        }

        if( ( aMenuLabel.Contains( wxString::FromUTF8( "原理图" ) ) || lower.Contains( wxS( "schematic" ) ) )
            && ( aMenuLabel.Contains( wxString::FromUTF8( "切换" ) )
                 || aMenuLabel.Contains( wxString::FromUTF8( "编辑" ) )
                 || lower.Contains( wxS( "switch" ) ) ) )
        {
            m_manager->CallAfter( [this]() {
                if( m_shuttingDown )
                    return;
                ScanAndSyncFrames();
                if( wxFrame* schFrame = FindEmbeddedOrLiveFrame( FRAME_SCH ) )
                    ActivateTabForFrame( schFrame );
            } );
            return;
        }

        ScheduleFocusActiveTab();
    }

    bool ShouldEmbedFrame( wxFrame* aFrame ) const
    {
        auto* edaFrame = dynamic_cast<EDA_BASE_FRAME*>( aFrame );
        if( !edaFrame )
            return false;

        if( edaFrame->IsType( KICAD_MAIN_FRAME_T ) )
            return false;

        if( ( aFrame->GetWindowStyleFlag() & wxFRAME_FLOAT_ON_PARENT ) != 0 )
            return false;

        if( edaFrame->IsType( FRAME_SCH )
            || edaFrame->IsType( FRAME_PCB_EDITOR )
            || edaFrame->IsType( FRAME_PCB_DISPLAY3D )
            || edaFrame->IsType( FRAME_SCH_SYMBOL_EDITOR )
            || edaFrame->IsType( FRAME_FOOTPRINT_EDITOR )
            || edaFrame->IsType( FRAME_GERBER )
            || edaFrame->IsType( FRAME_PL_EDITOR ) )
        {
            return true;
        }

        wxString name = aFrame->GetName();
        if( name == wxS( "Frame3D" ) || name == wxS( "Bm2CmpFrame" ) || name == wxS( "PcbCalculatorFrame" ) )
            return true;

        return false;
    }

    wxString BuildTabTitle( wxFrame* aFrame ) const
    {
        wxString rawTitle = aFrame->GetTitle();
        bool     isDirty  = rawTitle.StartsWith( wxT( "*" ) );
        wxString prefix   = isDirty ? wxString( wxS( "*" ) ) : wxString();

        if( auto* edaFrame = dynamic_cast<EDA_BASE_FRAME*>( aFrame ) )
        {
            if( edaFrame->IsType( FRAME_SCH ) )
            {
                wxString name = rawTitle.BeforeFirst( wxUniChar( 0x2014 ) ).Trim( true ).Trim( false );
                if( name.StartsWith( wxT( "*" ) ) )
                    name = name.Mid( 1 );
                return name.IsEmpty()
                        ? prefix + wxString::FromUTF8( "原理图" )
                        : prefix + wxString::FromUTF8( "原理图: " ) + name;
            }
            if( edaFrame->IsType( FRAME_PCB_EDITOR ) )
            {
                wxString name = rawTitle.BeforeFirst( wxUniChar( 0x2014 ) ).Trim( true ).Trim( false );
                if( name.StartsWith( wxT( "*" ) ) )
                    name = name.Mid( 1 );
                return name.IsEmpty()
                        ? prefix + wxString::FromUTF8( "PCB" )
                        : prefix + wxString::FromUTF8( "PCB: " ) + name;
            }
            if( edaFrame->IsType( FRAME_PCB_DISPLAY3D ) )
                return wxString::FromUTF8( "3D 视图" );
            if( edaFrame->IsType( FRAME_SCH_SYMBOL_EDITOR ) )
                return prefix + wxString::FromUTF8( "符号编辑器" );
            if( edaFrame->IsType( FRAME_FOOTPRINT_EDITOR ) )
                return prefix + wxString::FromUTF8( "封装编辑器" );
            if( edaFrame->IsType( FRAME_GERBER ) )
                return wxString::FromUTF8( "Gerber 查看器" );
            if( edaFrame->IsType( FRAME_PL_EDITOR ) )
                return prefix + wxString::FromUTF8( "图框编辑器" );
        }

        return rawTitle.IsEmpty() ? wxString::FromUTF8( "编辑器" ) : rawTitle;
    }

    void OptimizeEmbeddedEditorLayout( wxFrame* aFrame )
    {
        auto* edaFrame = dynamic_cast<EDA_BASE_FRAME*>( aFrame );
        if( !edaFrame )
            return;

        wxAuiManager* childAui = wxAuiManager::GetManager( aFrame );
        if( !childAui )
            return;

        bool changed = false;
        wxAuiPaneInfoArray& panes = childAui->GetAllPanes();

        if( edaFrame->IsType( FRAME_SCH ) )
        {
            for( size_t i = 0; i < panes.GetCount(); ++i )
            {
                wxAuiPaneInfo& p = panes.Item( i );
                wxString lname = p.name.Lower();

                if( lname.Contains( wxS( "properties" ) ) )
                {
                    p.Right().Layer( 4 ).Position( 0 ).Show( false );
                    changed = true;
                }
                else if( lname.Contains( wxS( "selectionfilter" ) ) )
                {
                    p.Right().Layer( 4 ).Position( 2 ).Show( false );
                    changed = true;
                }
                else if( lname.Contains( wxS( "hierarchy" ) ) )
                {
                    p.Right().Layer( 4 ).Position( 1 ).Show( false );
                    changed = true;
                }
            }
        }
        else if( edaFrame->IsType( FRAME_PCB_EDITOR ) )
        {
            for( size_t i = 0; i < panes.GetCount(); ++i )
            {
                wxAuiPaneInfo& p = panes.Item( i );
                if( p.name.Lower().Contains( wxS( "properties" ) ) )
                {
                    p.Right().Layer( 4 ).Position( 0 ).Show( false );
                    changed = true;
                }
            }
        }

        if( changed )
            childAui->Update();
    }

    void EmbedFrame( wxFrame* aFrame )
    {
        if( !aFrame || !m_notebook )
            return;

        OptimizeEmbeddedEditorLayout( aFrame );

        auto* host = new LANTU_EMBEDDED_FRAME_PAGE( m_notebook, aFrame, m_manager );
        wxString title = BuildTabTitle( aFrame );

        m_notebook->AddPage( host, title, true );
        m_notebook->SetTabCtrlHeight( -1 );

        m_manager->CallAfter( [this, aFrame]() {
            if( m_shuttingDown )
                return;
            int idx = FindPageIndexByFrame( aFrame );
            if( idx != wxNOT_FOUND )
            {
                if( auto* h = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( m_notebook->GetPage( idx ) ) )
                {
                    h->SyncChildSize();
                    h->FocusEmbeddedFrame();
                }
            }
            SyncChromeToActiveTab();
        } );
    }

    void OnTimer( wxTimerEvent& )
    {
        ScanAndSyncFrames();
    }

    void OnPageChanged( wxAuiNotebookEvent& aEvt )
    {
        SyncActiveTabSize();
        SyncChromeToActiveTab();
        ScheduleFocusActiveTab();

        aEvt.Skip();
    }
};

static std::unique_ptr<LANTU_TAB_COORDINATOR> s_tabCoordinator;

} // namespace


// Menubar and toolbar event table
BEGIN_EVENT_TABLE( KICAD_MANAGER_FRAME, EDA_BASE_FRAME )
    // Window events
    EVT_SIZE( KICAD_MANAGER_FRAME::OnSize )
    EVT_IDLE( KICAD_MANAGER_FRAME::OnIdle )

    // Menu events
    EVT_MENU( wxID_EXIT, KICAD_MANAGER_FRAME::OnExit )
    EVT_MENU( ID_EDIT_LOCAL_FILE_IN_TEXT_EDITOR, KICAD_MANAGER_FRAME::OnOpenFileInTextEditor )
    EVT_MENU( ID_EDIT_ADVANCED_CFG, KICAD_MANAGER_FRAME::OnEditAdvancedCfg )
    EVT_MENU( ID_IMPORT_CADSTAR_ARCHIVE_PROJECT, KICAD_MANAGER_FRAME::OnImportCadstarArchiveFiles )
    EVT_MENU( ID_IMPORT_EAGLE_PROJECT, KICAD_MANAGER_FRAME::OnImportEagleFiles )
    EVT_MENU( ID_IMPORT_EASYEDA_PROJECT, KICAD_MANAGER_FRAME::OnImportEasyEdaFiles )
    EVT_MENU( ID_IMPORT_EASYEDAPRO_PROJECT, KICAD_MANAGER_FRAME::OnImportEasyEdaProFiles )
    EVT_MENU( ID_IMPORT_ALTIUM_PROJECT, KICAD_MANAGER_FRAME::OnImportAltiumProjectFiles )
    EVT_MENU( ID_IMPORT_PADS_PROJECT, KICAD_MANAGER_FRAME::OnImportPadsProjectFiles )
    EVT_MENU( ID_IMPORT_PCAD_PROJECT, KICAD_MANAGER_FRAME::OnImportPcadProjectFiles )
    EVT_MENU( ID_IMPORT_GEDA_PROJECT, KICAD_MANAGER_FRAME::OnImportGedaFiles )
    EVT_MENU( ID_IMPORT_DIPTRACE_PROJECT, KICAD_MANAGER_FRAME::OnImportDipTraceFiles )
    EVT_MENU( ID_IMPORT_ORCAD_PROJECT, KICAD_MANAGER_FRAME::OnImportOrcadFiles )
    EVT_MENU( ID_COMPARE_PROJECT_BRANCHES,
              KICAD_MANAGER_FRAME::OnCompareProjectBranches )

    // Range menu events
    EVT_MENU_RANGE( ID_FILE1, ID_FILEMAX, KICAD_MANAGER_FRAME::OnFileHistory )
    EVT_MENU( ID_FILE_LIST_CLEAR, KICAD_MANAGER_FRAME::OnClearFileHistory )

    // Special functions
    EVT_MENU( ID_INIT_WATCHED_PATHS, KICAD_MANAGER_FRAME::OnChangeWatchedPaths )

    // Drop files event
    EVT_DROP_FILES( KICAD_MANAGER_FRAME::OnDropFiles )

END_EVENT_TABLE()


KICAD_MANAGER_FRAME::KICAD_MANAGER_FRAME( wxWindow* parent, const wxString& title,
                                          const wxPoint& pos, const wxSize&   size ) :
        EDA_BASE_FRAME( parent, KICAD_MAIN_FRAME_T, title, pos, size, KICAD_DEFAULT_DRAWFRAME_STYLE,
                        KICAD_MANAGER_FRAME_NAME, &::Kiway, unityScale ),
        m_openSavedWindows( false ),
        m_restoredFromHistory( false ),
        m_active_project( false ),
        m_showHistoryPanel( false ),
        m_projectTreePane( nullptr ),
        m_historyPane( nullptr ),
        m_launcher( nullptr ),
        m_lastToolbarIconSize( 0 ),
        m_pcmButton( nullptr ),
        m_pcmUpdateCount( 0 )
{
    const int defaultLeftWinWidth = FromDIP( 200 );

    m_leftWinWidth = defaultLeftWinWidth;
    m_aboutTitle = "LantuEDA";

    wxXmlDocument dummy;

    CreateStatusBar( 2 );
    Pgm().GetBackgroundJobMonitor().RegisterStatusBar( (KISTATUSBAR*) GetStatusBar() );
    Pgm().GetNotificationsManager().RegisterStatusBar( (KISTATUSBAR*) GetStatusBar() );
    Pgm().RegisterLibraryLoadStatusBar( (KISTATUSBAR*) GetStatusBar() );
    GetStatusBar()->SetFont( KIUI::GetStatusFont( this ) );

    wxIcon icon;
    wxIconBundle icon_bundle;

    if( IsNightlyVersion() )
    {
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad_nightly, 48 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad_nightly, 128 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad_nightly, 256 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad_nightly_32 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad_nightly_16 ) );
        icon_bundle.AddIcon( icon );
    }
    else
    {
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad, 48 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad, 128 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad, 256 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad_32 ) );
        icon_bundle.AddIcon( icon );
        icon.CopyFromBitmap( KiBitmap( BITMAPS::icon_kicad_16 ) );
        icon_bundle.AddIcon( icon );
    }

    SetIcons( icon_bundle );

    LoadSettings( config() );

    if( m_leftWinWidth > FromDIP( 240 ) || m_leftWinWidth < FromDIP( 160 ) )
        m_leftWinWidth = defaultLeftWinWidth;

    m_projectTreePane = new PROJECT_TREE_PANE( this );

    setupTools();
    setupUIConditions();

    m_toolbarSettings = GetToolbarSettings<KICAD_MANAGER_TOOLBAR_SETTINGS>( "kicad-toolbars" );
    configureToolbars();
    RecreateToolbars();
    ReCreateMenuBar();

    m_auimgr.SetManagedWindow( this );
    m_auimgr.SetFlags( wxAUI_MGR_LIVE_RESIZE );

    LANTU_PROXY_MENUBAR* proxyMenuBar = nullptr;
#ifdef __WXMSW__
    proxyMenuBar = new LANTU_PROXY_MENUBAR( this );
    wxAuiPaneInfo menuPane;
    menuPane.Name( wxS( "LantuGlobalMenuBar" ) )
            .Top().Layer( 10 ).Row( 0 ).Position( 0 )
            .CaptionVisible( false ).CloseButton( false )
            .Gripper( false ).PaneBorder( false )
            .Floatable( false ).Movable( false )
            .Resizable( true )
            .DockFixed( true )
            .MinSize( FromDIP( 200 ), FromDIP( 28 ) )
            .BestSize( FromDIP( 1600 ), FromDIP( 28 ) )
            .MaxSize( 10000, FromDIP( 28 ) );
    menuPane.dock_proportion = 1;
    m_auimgr.AddPane( proxyMenuBar, menuPane );
#endif

    m_auimgr.AddPane( m_tbLeft, EDA_PANE().VToolbar().Name( "TopMainToolbar" ).Left().Layer( 2 ) );

    m_auimgr.AddPane( m_projectTreePane,
                      EDA_PANE().Palette().Name( "ProjectTree" ).Left().Layer( 1 )
                                .Caption( PROJECT_FILES_CAPTION ).PaneBorder( false )
                                .MinSize( m_leftWinWidth, -1 ).BestSize( m_leftWinWidth, -1 )
                                .Floatable( false ).Movable( false ) );

    m_historyPane = new LOCAL_HISTORY_PANE( this );
    m_auimgr.AddPane( m_historyPane,
                      EDA_PANE().Palette().Name( "LocalHistory" ).Left().Layer( 1 ).Position( 1 )
                                .Caption( _( "Local History" ) ).PaneBorder( false )
                                .Floatable( false ).Movable( false ).CloseButton( true ).Hide() );

    if( m_showHistoryPanel )
        m_auimgr.GetPane( m_historyPane ).Show();

    wxSize client_size = GetClientSize();
    m_notebook = new wxAuiNotebook( this, wxID_ANY, wxPoint( client_size.x, client_size.y ),
                                    FromDIP( wxSize( 700, 590 ) ),
                                    wxAUI_NB_TOP | wxAUI_NB_CLOSE_ON_ALL_TABS | wxAUI_NB_TAB_MOVE
                                            | wxAUI_NB_SCROLL_BUTTONS | wxNO_BORDER );

    m_notebook->SetArtProvider( new WX_AUI_TAB_ART() );

    m_notebook->Bind( wxEVT_AUINOTEBOOK_PAGE_CLOSE, &KICAD_MANAGER_FRAME::onNotebookPageCloseRequest, this );
    m_notebook->Bind( wxEVT_AUINOTEBOOK_PAGE_CLOSED, &KICAD_MANAGER_FRAME::onNotebookPageCountChanged, this );
    m_launcher = new PANEL_KICAD_LAUNCHER( m_notebook );

    m_notebook->Freeze();
    m_launcher->SetClosable( false );
    m_notebook->AddPage( m_launcher, EDITORS_CAPTION, false );
    m_notebook->SetTabCtrlHeight( -1 );
    m_notebook->Thaw();

    m_auimgr.AddPane( m_notebook, EDA_PANE().Canvas().Name( "Editors" ).Center().Caption( EDITORS_CAPTION )
                                            .PaneBorder( false ).MinSize( m_notebook->GetBestSize() ) );

    m_auimgr.Update();

    s_tabCoordinator = std::make_unique<LANTU_TAB_COORDINATOR>(
            this, &m_auimgr, m_projectTreePane, m_tbLeft, m_notebook, proxyMenuBar );

    Bind( wxEVT_MENU,
          []( wxCommandEvent& aEvt )
          {
              if( s_tabCoordinator )
              {
                  if( auto* targetFrame = static_cast<wxFrame*>( aEvt.GetClientData() ) )
                      s_tabCoordinator->ActivateTabForFrame( targetFrame );
              }
          },
          ID_LANTU_ACTIVATE_TAB );

    m_auimgr.GetPane( m_projectTreePane ).MinSize( FromDIP( 160 ), FromDIP( 80 ) );

    CallAfter( [this]() {
        if( !IsMaximized() && GetSize().x < FromDIP( 1280 ) )
            Maximize( true );
        if( s_tabCoordinator )
            s_tabCoordinator->SyncActiveTabSize();
    } );

    if( ADVANCED_CFG::GetCfg().m_HideVersionFromTitle )
        SetTitle( wxT( "LantuEDA" ) );
    else
        SetTitle( wxString( "LantuEDA " ) + GetMajorMinorVersion() );

    m_projectTreePane->SetFocus();

    m_acceptedExts.emplace( FILEEXT::ProjectFileExtension, &KICAD_MANAGER_ACTIONS::loadProject );
    m_acceptedExts.emplace( FILEEXT::LegacyProjectFileExtension, &KICAD_MANAGER_ACTIONS::loadProject );

    m_acceptedExts.emplace( FILEEXT::GerberFileExtension, &KICAD_MANAGER_ACTIONS::viewDroppedGerbers );
    m_acceptedExts.emplace( FILEEXT::GerberJobFileExtension, &KICAD_MANAGER_ACTIONS::viewDroppedGerbers );
    m_acceptedExts.emplace( FILEEXT::DrillFileExtension, &KICAD_MANAGER_ACTIONS::viewDroppedGerbers );

    DragAcceptFiles( true );
}


KICAD_MANAGER_FRAME::~KICAD_MANAGER_FRAME()
{
    if( s_tabCoordinator )
    {
        s_tabCoordinator->Shutdown();
        s_tabCoordinator.reset();
    }

    Unbind( wxEVT_CHAR, &TOOL_DISPATCHER::DispatchWxEvent, m_toolDispatcher );
    Unbind( wxEVT_CHAR_HOOK, &TOOL_DISPATCHER::DispatchWxEvent, m_toolDispatcher );

    m_notebook->Unbind( wxEVT_AUINOTEBOOK_PAGE_CLOSE, &KICAD_MANAGER_FRAME::onNotebookPageCloseRequest, this );
    m_notebook->Unbind( wxEVT_AUINOTEBOOK_PAGE_CLOSED, &KICAD_MANAGER_FRAME::onNotebookPageCountChanged, this );

    Pgm().GetBackgroundJobMonitor().UnregisterStatusBar( (KISTATUSBAR*) GetStatusBar() );
    Pgm().GetNotificationsManager().UnregisterStatusBar( (KISTATUSBAR*) GetStatusBar() );
    Pgm().UnregisterLibraryLoadStatusBar( (KISTATUSBAR*) GetStatusBar() );

    if( m_toolManager )
        m_toolManager->ShutdownAllTools();

    if( m_pcm )
        m_pcm->StopBackgroundUpdate();

    m_updateManager.reset();

    delete m_actions;
    delete m_toolManager;
    delete m_toolDispatcher;

    m_auimgr.UnInit();
}


void KICAD_MANAGER_FRAME::HideTabsIfNeeded()
{
    m_notebook->SetTabCtrlHeight( -1 );
}


void KICAD_MANAGER_FRAME::onNotebookPageCountChanged( wxAuiNotebookEvent& evt )
{
    HideTabsIfNeeded();

    if( s_tabCoordinator )
    {
        s_tabCoordinator->SyncChromeToActiveTab();
        s_tabCoordinator->ScheduleFocusActiveTab();
    }
}


void KICAD_MANAGER_FRAME::onNotebookPageCloseRequest( wxAuiNotebookEvent& evt )
{
    wxAuiNotebook* notebook = (wxAuiNotebook*) evt.GetEventObject();
    wxWindow*      page = notebook->GetPage( evt.GetSelection() );

    if( auto* hostPage = dynamic_cast<LANTU_EMBEDDED_FRAME_PAGE*>( page ) )
    {
        evt.Veto();

        wxFrame* target = hostPage->GetEmbeddedFrame();
        if( target && IsFrameInTopLevelList( target ) )
        {
            CallAfter(
                    [target]()
                    {
                        if( IsFrameInTopLevelList( target ) )
                        {
                            if( target->Close( false ) && s_tabCoordinator )
                                s_tabCoordinator->RemoveTabForFrame( target );
                        }
                    } );
        }
        else if( s_tabCoordinator )
        {
            CallAfter(
                    []()
                    {
                        if( s_tabCoordinator )
                            s_tabCoordinator->CleanupClosedTabs();
                    } );
        }

        return;
    }

    if( PANEL_NOTEBOOK_BASE* panel = dynamic_cast<PANEL_NOTEBOOK_BASE*>( page ) )
    {
        if( panel->GetClosable() )
        {
            if( !panel->GetCanClose() )
                evt.Veto();

            CallAfter(
                    [this]()
                    {
                        SaveOpenJobSetsToLocalSettings();
                    } );
        }
        else
        {
            evt.Veto();
        }
    }
}


wxStatusBar* KICAD_MANAGER_FRAME::OnCreateStatusBar( int number, long style, wxWindowID id,
                                                     const wxString& name )
{
    KISTATUSBAR* sb = new KISTATUSBAR( number, this, id,
                                       static_cast<KISTATUSBAR::STYLE_FLAGS>( KISTATUSBAR::NOTIFICATION_ICON
                                                                            | KISTATUSBAR::CANCEL_BUTTON
                                                                            | KISTATUSBAR::WARNING_ICON ) );

    size_t sbFieldCnt = static_cast<size_t>( sb->GetFieldsCount() );
    std::vector<int> sbFieldSizes( sbFieldCnt );

    for( size_t i = 0; i < sbFieldCnt; i++ )
        sbFieldSizes[i] = sb->GetStatusWidth( static_cast<int>( i ) );

    if( sbFieldCnt > 0 )
        sbFieldSizes[0] = -1;

    if( sbFieldCnt > 1 )
    {
        int margin = KIUI::GetTextSize( wxT( "XX" ), sb ).x;
        int watcherWidth = std::max( KIUI::GetTextSize( _( "Local path: monitoring folder changes" ), sb ).x,
                                     KIUI::GetTextSize( _( "Network path: not monitoring folder changes" ), sb ).x );

        sbFieldSizes[1] = watcherWidth + margin;
    }

    sb->SetStatusWidths( sbFieldCnt, sbFieldSizes.data() );

    return sb;
}


void KICAD_MANAGER_FRAME::CreatePCM()
{
    if( m_pcm )
        return;

    m_pcm = std::make_shared<PLUGIN_CONTENT_MANAGER>(
            [this]( int aUpdateCount )
            {
                if( Pgm().m_Quitting )
                    return;

                m_pcmUpdateCount = aUpdateCount;

                if( aUpdateCount > 0 )
                {
                    Pgm().GetNotificationsManager().CreateOrUpdate(
                            wxS( "pcm" ),
                            _( "PCM Updates Available" ),
                            wxString::Format( _( "%d package update(s) available" ), aUpdateCount ),
                            wxT( "" ) );
                }
                else
                {
                    Pgm().GetNotificationsManager().Remove( wxS( "pcm" ) );
                }

                CallAfter(
                        [this]()
                        {
                            updatePcmButtonBadge();
                        } );
            } );

    m_pcm->SetRepositoryList( kicadSettings()->m_PcmRepositories );
}


void KICAD_MANAGER_FRAME::setupTools()
{
    m_toolManager = new TOOL_MANAGER;
    m_toolManager->SetEnvironment( nullptr, nullptr, nullptr, config(), this );
    m_actions = new KICAD_MANAGER_ACTIONS();

    m_toolDispatcher = new TOOL_DISPATCHER( m_toolManager );

    Bind( wxEVT_CHAR, &TOOL_DISPATCHER::DispatchWxEvent, m_toolDispatcher );
    Bind( wxEVT_CHAR_HOOK, &TOOL_DISPATCHER::DispatchWxEvent, m_toolDispatcher );

    m_toolManager->RegisterTool( new COMMON_CONTROL );
    m_toolManager->RegisterTool( new KICAD_MANAGER_CONTROL );
    m_toolManager->InitTools();
}


void KICAD_MANAGER_FRAME::setupUIConditions()
{
    EDA_BASE_FRAME::setupUIConditions();

    ACTION_MANAGER* manager = m_toolManager->GetActionManager();

    wxASSERT( manager );

    auto activeProject =
            [this] ( const SELECTION& )
            {
                return m_active_project;
            };

#define ENABLE( x ) ACTION_CONDITIONS().Enable( x )

    ACTION_CONDITIONS activeProjectCond;
    activeProjectCond.Enable( activeProject );

    manager->SetConditions( ACTIONS::saveAs,                       activeProjectCond );
    manager->SetConditions( KICAD_MANAGER_ACTIONS::closeProject,   activeProjectCond );
    manager->SetConditions( KICAD_MANAGER_ACTIONS::archiveProject, activeProjectCond );
    manager->SetConditions( KICAD_MANAGER_ACTIONS::newJobsetFile,  activeProjectCond );
    manager->SetConditions( KICAD_MANAGER_ACTIONS::openJobsetFile, activeProjectCond );

    auto historyCond =
            [this]( const SELECTION& )
            {
                return HistoryPanelShown();
            };

    manager->SetConditions( KICAD_MANAGER_ACTIONS::showLocalHistory, ACTION_CONDITIONS().Check( historyCond ) );

    manager->SetConditions( ACTIONS::cut,     ENABLE( SELECTION_CONDITIONS::ShowNever ) );
    manager->SetConditions( ACTIONS::copy,    ENABLE( SELECTION_CONDITIONS::ShowNever ) );
    manager->SetConditions( ACTIONS::paste,   ENABLE( SELECTION_CONDITIONS::ShowNever ) );

#undef ENABLE
}


wxWindow* KICAD_MANAGER_FRAME::GetToolCanvas() const
{
    return m_projectTreePane;
}


APP_SETTINGS_BASE* KICAD_MANAGER_FRAME::config() const
{
    APP_SETTINGS_BASE* ret = PgmTop().PgmSettings();
    wxASSERT( ret );
    return ret;
}


KICAD_SETTINGS* KICAD_MANAGER_FRAME::kicadSettings() const
{
    KICAD_SETTINGS* ret = dynamic_cast<KICAD_SETTINGS*>( config() );
    wxASSERT( ret );
    return ret;
}


void KICAD_MANAGER_FRAME::PreloadAllLibraries()
{
    CallAfter(
            [&]()
            {
                KIFACE* schface = Kiway().KiFACE( KIWAY::FACE_SCH );
                schface->PreloadLibraries( &Kiway() );

                KIFACE* pcbface = Kiway().KiFACE( KIWAY::FACE_PCB );
                pcbface->PreloadLibraries( &Kiway() );

                Pgm().PreloadDesignBlockLibraries( &Kiway() );
            } );
}


wxString KICAD_MANAGER_FRAME::GetCurrentFileName() const
{
    return GetProjectFileName();
}


const wxString KICAD_MANAGER_FRAME::GetProjectFileName() const
{
    return Pgm().GetSettingsManager().IsProjectOpen() ? Prj().GetProjectFullName()
                                                      : wxString( wxEmptyString );
}


const wxString KICAD_MANAGER_FRAME::SchFileName()
{
    wxFileName fn( GetProjectFileName() );

    fn.SetExt( FILEEXT::KiCadSchematicFileExtension );
    return fn.GetFullPath();
}


const wxString KICAD_MANAGER_FRAME::SchLegacyFileName()
{
    wxFileName fn( GetProjectFileName() );

    fn.SetExt( FILEEXT::LegacySchematicFileExtension );
    return fn.GetFullPath();
}


const wxString KICAD_MANAGER_FRAME::PcbFileName()
{
    wxFileName fn( GetProjectFileName() );

    fn.SetExt( FILEEXT::PcbFileExtension );
    return fn.GetFullPath();
}


const wxString KICAD_MANAGER_FRAME::PcbLegacyFileName()
{
    wxFileName fn( GetProjectFileName() );

    fn.SetExt( FILEEXT::LegacyPcbFileExtension );
    return fn.GetFullPath();
}


void KICAD_MANAGER_FRAME::ReCreateTreePrj()
{
    m_projectTreePane->ReCreateTreePrj();
}


const SEARCH_STACK& KICAD_MANAGER_FRAME::sys_search()
{
    return PgmTop().SysSearch();
}


wxString KICAD_MANAGER_FRAME::help_name()
{
    return PgmTop().GetHelpFileName();
}


void KICAD_MANAGER_FRAME::OnSize( wxSizeEvent& event )
{
    if( m_auimgr.GetManagedWindow() )
        m_auimgr.Update();

    if( s_tabCoordinator )
        s_tabCoordinator->SyncActiveTabSize();

    event.Skip();
}


void KICAD_MANAGER_FRAME::DoWithAcceptedFiles()
{
    for( const wxFileName& fileName : m_AcceptedFiles )
    {
        wxString ext = fileName.GetExt();

        if( ext == FILEEXT::ProjectFileExtension || ext == FILEEXT::LegacyProjectFileExtension )
        {
            wxString fn = fileName.GetFullPath();
            m_toolManager->RunAction<wxString*>( *m_acceptedExts.at( fileName.GetExt() ), &fn );

            return;
        }
    }

    wxString gerberFiles;

    for( const wxFileName& fileName : m_AcceptedFiles )
    {
        wxString ext = fileName.GetExt();

        if( ext == FILEEXT::GerberJobFileExtension
            || ext == FILEEXT::DrillFileExtension
            || FILEEXT::IsGerberFileExtension( ext ) )
        {
            gerberFiles += wxT( '\"' );
            gerberFiles += fileName.GetFullPath() + wxT( '\"' );
            gerberFiles = gerberFiles.Pad( 1 );
        }
        else
        {
            wxString fn = fileName.GetFullPath();
            m_toolManager->RunAction<wxString*>( *m_acceptedExts.at( fileName.GetExt() ), &fn );
        }
    }

    if( !gerberFiles.IsEmpty() )
    {
        wxString fullEditorName = FindKicadFile( GERBVIEW_EXE );

        if( wxFileExists( fullEditorName ) )
        {
            wxString command = fullEditorName + " " + gerberFiles;
            m_toolManager->RunAction<wxString*>( *m_acceptedExts.at( FILEEXT::GerberFileExtension ), &command );
        }
    }
}


bool KICAD_MANAGER_FRAME::canCloseWindow( wxCloseEvent& aEvent )
{
    KICAD_SETTINGS* settings = kicadSettings();
    settings->m_OpenProjects = GetSettingsManager()->GetOpenProjects();

    for( size_t i = 0; i < m_notebook->GetPageCount(); i++ )
    {
        wxWindow* page = m_notebook->GetPage( i );

        if( PANEL_NOTEBOOK_BASE* panel = dynamic_cast<PANEL_NOTEBOOK_BASE*>( page ) )
        {
            if( !panel->GetCanClose() )
                return false;
        }
    }

    if( CloseProject( true ) )
    {
        if( s_tabCoordinator )
            s_tabCoordinator->Shutdown();

        aEvent.StopPropagation();

        return true;
    }
    else
    {
        if( aEvent.CanVeto() )
            aEvent.Veto();

        return false;
    }
}


void KICAD_MANAGER_FRAME::doCloseWindow()
{
#ifdef _WINDOWS_
    static std::atomic<unsigned int> lock_close_event( 0 );

    if( ++lock_close_event > 1 )
    {
        return;
    }
#endif

    if( s_tabCoordinator )
        s_tabCoordinator->Shutdown();

    m_projectTreePane->Show( false );
    Pgm().m_Quitting = true;

    Destroy();

#ifdef _WINDOWS_
    lock_close_event = 0;
#endif
}


void KICAD_MANAGER_FRAME::SaveOpenJobSetsToLocalSettings( bool aIsExplicitUserSave )
{
    PROJECT_LOCAL_SETTINGS& cfg = Prj().GetLocalSettings();

    if( !aIsExplicitUserSave && !cfg.ShouldAutoSave() )
        return;

    cfg.m_OpenJobSets.clear();

    for( size_t i = 0; i < m_notebook->GetPageCount(); i++ )
    {
        if( PANEL_JOBSET* jobset = dynamic_cast<PANEL_JOBSET*>( m_notebook->GetPage( i ) ) )
        {
            wxFileName jobsetFn( jobset->GetFilePath() );
            jobsetFn.MakeRelativeTo( Prj().GetProjectPath() );
            cfg.m_OpenJobSets.emplace_back( jobsetFn.GetFullPath() );
        }
    }

    cfg.SaveToFile( Prj().GetProjectPath() );
}


void KICAD_MANAGER_FRAME::OnExit( wxCommandEvent& event )
{
    Close( true );
}


bool KICAD_MANAGER_FRAME::CloseProject( bool aSave )
{
    if( !Kiway().PlayersClose( false ) )
        return false;

    if( s_tabCoordinator )
        s_tabCoordinator->CleanupClosedTabs();

    KIFACE* schface = Kiway().KiFACE( KIWAY::FACE_SCH );
    schface->CancelPreload();

    KIFACE* pcbface = Kiway().KiFACE( KIWAY::FACE_PCB );
    pcbface->CancelPreload();

    if( m_active_project )
    {
        SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();

        if( Prj().GetLocalSettings().ShouldAutoSave() && Prj().GetProjectFile().ShouldAutoSave() )
        {
            mgr.TriggerBackupIfNeeded( NULL_REPORTER::GetInstance() );

            if( aSave )
                mgr.SaveProject();
        }

        wxString projPath = Prj().GetProjectPath();

        Kiway().LocalHistory().WaitForPendingSave();

        if( Pgm().GetCommonSettings()->AutosaveUsesLocalHistory()
                && !projPath.IsEmpty() && Kiway().LocalHistory().HistoryExists( projPath ) )
        {
            if( Kiway().LocalHistory().HeadNewerThanLastSave( projPath ) )
            {
                Kiway().LocalHistory().CommitFullProjectSnapshot( projPath, wxS( "Close" ) );
                Kiway().LocalHistory().TagSave( projPath, wxS( "project" ) );
            }
        }

        m_active_project = false;

        if( Pgm().GetCommonSettings()
                && Pgm().GetCommonSettings()->AutosaveUsesLocalHistory() )
        {
            unsigned long long int limit = Pgm().GetCommonSettings()->m_Backup.limit_total_size;

            if( limit > 0 )
            {
                WX_PROGRESS_REPORTER reporter( this, _( "Local History" ), 3, PR_NO_ABORT );
                Kiway().LocalHistory().EnforceSizeLimit( Prj().GetProjectPath(), (size_t) limit, &reporter );
            }
        }

        Kiway().LocalHistory().UnregisterSaver( &Prj() );

        mgr.UnloadProject( &Prj() );
    }

    SetStatusText( "" );

    for( int i = (int) m_notebook->GetPageCount() - 1; i >= 0; i-- )
    {
        wxWindow* page = m_notebook->GetPage( i );

        if( PANEL_NOTEBOOK_BASE* panel = dynamic_cast<PANEL_NOTEBOOK_BASE*>( page ) )
        {
            if( panel->GetProjectTied() )
                m_notebook->DeletePage( i );
        }
    }

    m_projectTreePane->EmptyTreePrj();
    HideTabsIfNeeded();

    return true;
}


void KICAD_MANAGER_FRAME::OpenJobsFile( const wxFileName& aFileName, bool aCreate, bool aResaveProjectPreferences )
{
    for( size_t i = 0; i < m_notebook->GetPageCount(); i++ )
    {
        if( PANEL_JOBSET* panel = dynamic_cast<PANEL_JOBSET*>( m_notebook->GetPage( i ) ) )
        {
            if( aFileName.GetFullPath() == panel->GetFilePath() )
            {
                m_notebook->SetSelection( i );
                return;
            }
        }
    }

    try
    {
        std::unique_ptr<JOBSET> jobsFile = std::make_unique<JOBSET>( aFileName.GetFullPath().ToStdString() );

        jobsFile->LoadFromFile();

        if( aCreate && !aFileName.FileExists() )
        {
            JOBSET_DESTINATION* dest = jobsFile->AddNewDestination( JOBSET_DESTINATION_T::FOLDER );
            dest->m_outputHandler->SetOutputPath( aFileName.GetName() );
            jobsFile->SaveToFile( wxEmptyString, true );
        }

        PANEL_JOBSET* jobPanel = new PANEL_JOBSET( m_notebook, this, std::move( jobsFile ) );
        jobPanel->SetProjectTied( true );
        jobPanel->SetClosable( true );
        m_notebook->AddPage( jobPanel, aFileName.GetFullName(), true );
        HideTabsIfNeeded();

        if( aResaveProjectPreferences )
            SaveOpenJobSetsToLocalSettings();
    }
    catch( ... )
    {
        DisplayErrorMessage( this, _( "Error opening jobs file" ) );
    }
}


bool KICAD_MANAGER_FRAME::LoadProject( const wxFileName& aProjectFileName )
{
    if( !aProjectFileName.Exists() )
        return false;

    wxString fullPath = aProjectFileName.GetFullPath();

    LOCKFILE lockFile = LOCKFILE::Inspect( fullPath );

    bool lockOverrideGranted = false;

    if( !lockFile.Valid() )
    {
        wxString msg;
        msg.Printf( _( "Project '%s' is already open by '%s' at '%s'." ),
                    fullPath,
                    lockFile.GetUsername(),
                    lockFile.GetHostname() );

        if( !AskOverrideLock( this, msg ) )
            return false;

        LOCKFILE overridden( fullPath );
        overridden.OverrideLock();

        lockOverrideGranted = true;
    }

    if( !CloseProject( true ) )
        return false;

    m_active_project = true;

    Pgm().GetSettingsManager().LoadProject( fullPath );

    if( lockOverrideGranted )
        Prj().SetLockOverrideGranted( true );

    LoadWindowState( aProjectFileName.GetFullName() );

    if( aProjectFileName.IsDirWritable() )
        SetMruPath( Prj().GetProjectPath() );

    if( Pgm().GetCommonSettings()->AutosaveUsesLocalHistory()
            && Kiway().LocalHistory().HeadNewerThanLastSave( Prj().GetProjectPath() ) )
    {
        wxString head = Kiway().LocalHistory().GetHeadHash( Prj().GetProjectPath() );

        KICAD_MESSAGE_DIALOG dlg( this, _( "KiCad found unsaved changes from your last session that are newer than "
                                           "the saved project files." ),
                                  _( "Recover Unsaved Changes" ), wxYES_NO | wxICON_QUESTION );

        dlg.SetExtendedMessage( _( "This can happen if your previous session ended unexpectedly.\n\n"
                                   "Choose 'Restore' to recover those changes, or 'Discard' to keep the "
                                   "currently saved files." ) );

        dlg.SetYesNoLabels( _( "Restore" ), _( "Discard" ) );

        if( dlg.ShowModal() == wxID_YES )
        {
            Kiway().LocalHistory().RestoreCommit( Prj().GetProjectPath(), head, this, false );
        }
        else
        {
            Kiway().LocalHistory().CommitFullProjectSnapshot( Prj().GetProjectPath(), wxS( "Declined restore" ) );
            Kiway().LocalHistory().TagSave( Prj().GetProjectPath(), wxS( "project" ) );
        }
    }

    KICAD_SETTINGS* settings = kicadSettings();
    SaveSettings( settings );
    settings->SaveToFile( Pgm().GetSettingsManager().GetPathForSettingsFile( settings ) );

    m_projectTreePane->ReCreateTreePrj();
    m_historyPane->RefreshHistory( Prj().GetProjectPath() );

    for( const wxString& jobset : Prj().GetLocalSettings().m_OpenJobSets )
    {
        wxFileName jobsetFn( jobset );
        jobsetFn.MakeAbsolute( Prj().GetProjectPath() );

        if( jobsetFn.Exists() )
            OpenJobsFile( jobsetFn.GetFullPath(), false, false );
    }

    m_notebook->SetSelection( 0 );

    wxCommandEvent cmd( wxEVT_COMMAND_MENU_SELECTED, ID_INIT_WATCHED_PATHS );

    wxPostEvent( this, cmd );

    PrintPrjInfo();

    KIPLATFORM::APP::RegisterApplicationRestart( aProjectFileName.GetFullPath() );
    m_openSavedWindows = true;

    KIPLATFORM::ENV::AddToRecentDocs( aProjectFileName.GetFullPath() );

    PreloadAllLibraries();
    return true;
}


void KICAD_MANAGER_FRAME::CreateNewProject( const wxFileName& aProjectFileName, bool aCreateStubFiles )
{
    wxCHECK_RET( aProjectFileName.DirExists() && aProjectFileName.IsDirWritable(),
                 "Project folder must exist and be writable to create a new project." );

    if( !aProjectFileName.FileExists() )
    {
        wxFileName legacyPro( aProjectFileName );
        legacyPro.SetExt( FILEEXT::LegacyProjectFileExtension );

        if( legacyPro.FileExists() )
        {
            GetSettingsManager()->LoadProject( legacyPro.GetFullPath() );
            GetSettingsManager()->SaveProject();

            wxRemoveFile( legacyPro.GetFullPath() );
        }
        else
        {
            wxString srcFileName = sys_search().FindValidPath( "kicad.kicad_pro" );

            wxFileName destFileName( aProjectFileName );
            destFileName.SetExt( FILEEXT::ProjectFileExtension );

            if( !wxFileName::FileExists( srcFileName )
                || !wxCopyFile( srcFileName, destFileName.GetFullPath() ) )
            {
                wxFFile file( destFileName.GetFullPath(), "wb" );

                if( file.IsOpened() )
                    file.Write( wxT( "{\n}\n" ) );
            }
        }
    }

    if( aCreateStubFiles )
    {
        wxFileName fn( aProjectFileName.GetFullPath() );
        fn.SetExt( FILEEXT::KiCadSchematicFileExtension );

        if( !fn.FileExists() )
        {
            wxFFile file( fn.GetFullPath(), "wb" );

            if( file.IsOpened() )
            {
                file.Write( wxString::Format( "(kicad_sch\n"
                                              "\t(version %d)\n"
                                              "\t(generator \"eeschema\")\n"
                                              "\t(generator_version \"%s\")\n"
                                              "\t(uuid %s)\n"
                                              "\t(paper \"A4\")\n"
                                              "\t(lib_symbols)\n"
                                              "\t(sheet_instances\n"
                                              "\t\t(path \"/\"\n"
                                              "\t\t\t(page \"1\")\n"
                                              "\t\t)\n"
                                              "\t)\n"
                                              "\t(embedded_fonts no)\n"
                                              ")",
                                              SEXPR_SCHEMATIC_FILE_VERSION,
                                              GetMajorMinorVersion(),
                                              KIID().AsString() ) );
            }
        }

        fn.SetExt( FILEEXT::KiCadPcbFileExtension );
        wxFileName leg_fn( fn );
        leg_fn.SetExt( FILEEXT::LegacyPcbFileExtension );

        if( !fn.FileExists() && !leg_fn.FileExists() )
        {
            wxFFile file( fn.GetFullPath(), "wb" );

            if( file.IsOpened() )
            {
                file.Write( wxString::Format( "(kicad_pcb (version %d) (generator \"pcbnew\") (generator_version \"%s\")\n)",
                                              SEXPR_BOARD_FILE_VERSION, GetMajorMinorVersion() ) );
            }
        }
    }

    KICAD_SETTINGS* settings = kicadSettings();
    SaveSettings( settings );
    settings->SaveToFile( Pgm().GetSettingsManager().GetPathForSettingsFile( settings ) );

    m_openSavedWindows = true;
}


void KICAD_MANAGER_FRAME::OnCompareProjectBranches( wxCommandEvent& event )
{
    git_repository* repo = ( m_projectTreePane && m_projectTreePane->m_TreeProject )
                                   ? m_projectTreePane->m_TreeProject->GetGitRepo()
                                   : nullptr;

    if( !repo )
    {
        wxMessageBox( _( "Compare Project Branches requires a project that is "
                         "tracked by git." ),
                      _( "Compare Branches" ),
                      wxOK | wxICON_INFORMATION, this );
        return;
    }

    wxString currentBranch;
    git_reference* head = nullptr;

    if( git_repository_head( &head, repo ) == 0 )
    {
        const char* shorthand = git_reference_shorthand( head );

        if( shorthand && *shorthand )
            currentBranch = wxString::FromUTF8( shorthand );

        git_reference_free( head );
    }

    std::vector<wxString> refs;

    auto pushUnique = [&]( const wxString& aRef )
    {
        if( aRef.IsEmpty() )
            return;

        for( const wxString& existing : refs )
        {
            if( existing == aRef )
                return;
        }

        refs.push_back( aRef );
    };

    pushUnique( wxS( "main" ) );
    pushUnique( wxS( "master" ) );
    pushUnique( currentBranch );
    pushUnique( wxS( "HEAD" ) );

    DIALOG_GIT_MR_REVIEW dlg( this, repo, refs );
    dlg.ShowModal();
}


void KICAD_MANAGER_FRAME::OnOpenFileInTextEditor( wxCommandEvent& event )
{
    wxString wildcard = FILEEXT::AllFilesWildcard();
    wxString default_dir = Prj().GetProjectPath();

    wxFileDialog dlg( this, _( "Edit File in Text Editor" ), default_dir, wxEmptyString, wildcard,
                      wxFD_OPEN );

    KIPLATFORM::UI::AllowNetworkFileSystems( &dlg );

    if( dlg.ShowModal() == wxID_CANCEL )
        return;

    wxString filename = dlg.GetPath();

    if( !dlg.GetPath().IsEmpty() && !Pgm().GetTextEditor().IsEmpty() )
        m_toolManager->RunAction<wxString*>( KICAD_MANAGER_ACTIONS::openTextEditor, &filename );
}


void KICAD_MANAGER_FRAME::OnEditAdvancedCfg( wxCommandEvent& WXUNUSED( event ) )
{
    DIALOG_EDIT_CFG dlg( this );
    dlg.ShowModal();
}


void KICAD_MANAGER_FRAME::RefreshProjectTree()
{
    m_projectTreePane->ReCreateTreePrj();
}


void KICAD_MANAGER_FRAME::ShowChangedLanguage()
{
    EDA_BASE_FRAME::ShowChangedLanguage();

    RecreateToolbars();
    m_launcher->CreateLaunchers();

    int pageId = m_notebook->FindPage( m_launcher );

    if( pageId != wxNOT_FOUND )
        m_notebook->SetPageText( pageId, EDITORS_CAPTION );

    m_auimgr.GetPane( m_projectTreePane ).Caption( PROJECT_FILES_CAPTION );
    m_auimgr.Update();

    if( s_tabCoordinator )
        s_tabCoordinator->RefreshMenuBarVisual();

    m_projectTreePane->FileWatcherReset();

    PrintPrjInfo();
}


void KICAD_MANAGER_FRAME::CommonSettingsChanged( int aFlags )
{
    EDA_BASE_FRAME::CommonSettingsChanged( aFlags );

    if( m_pcm && ( aFlags & ENVVARS_CHANGED ) )
        m_pcm->ReadEnvVar();

    COMMON_SETTINGS* settings = Pgm().GetCommonSettings();

    if( m_lastToolbarIconSize == 0
        || m_lastToolbarIconSize != settings->m_Appearance.toolbar_icon_size )
    {
        onToolbarSizeChanged();
        m_lastToolbarIconSize = settings->m_Appearance.toolbar_icon_size;
    }

    m_projectTreePane->ReCreateTreePrj();
}


void KICAD_MANAGER_FRAME::ProjectChanged()
{
    wxString file = GetProjectFileName();

    if( !Prj().IsNullProject() &&
        Prj().GetProjectLock() == nullptr )
    {
        LOCKFILE lockFile( file );

        if( !lockFile.Valid() )
        {
            wxString msg;
            msg.Printf( _( "Project '%s' is already open by '%s' at '%s'." ),
                        file,
                        lockFile.GetUsername(),
                        lockFile.GetHostname() );

            if( AskOverrideLock( this, msg ) )
            {
                lockFile.OverrideLock();
            }
        }

        Prj().SetReadOnly( !lockFile.Valid() || Prj().GetProjectFile().IsReadOnly() );
        Prj().SetProjectLock( new LOCKFILE( std::move( lockFile ) ) );
    }

    wxString title;

    if( !file.IsEmpty() )
    {
        wxFileName fn( file );

        title = fn.GetName();

        if( Prj().IsReadOnly() )
            title += wxS( " " ) + _( "[Read Only]" );
    }
    else
    {
        title = _( "[no project loaded]" );
    }

    if( ADVANCED_CFG::GetCfg().m_HideVersionFromTitle )
        title += wxT( " \u2014 " ) + wxString( wxS( "LantuEDA" ) );
    else
        title += wxT( " \u2014 " ) + wxString( wxS( "LantuEDA " ) ) + GetMajorMinorVersion();

    SetTitle( title );

    Kiway().LocalHistory().RegisterSaver( &Prj(),
            [this]( const wxString& aProjectPath, std::vector<HISTORY_FILE_DATA>& aFileData )
            {
                Prj().SaveToHistory( aProjectPath, aFileData );
            },
            Prj().GetHistoryLifetimeToken() );
}


void KICAD_MANAGER_FRAME::LoadSettings( APP_SETTINGS_BASE* aCfg )
{
    EDA_BASE_FRAME::LoadSettings( aCfg );

    auto settings = dynamic_cast<KICAD_SETTINGS*>( aCfg );

    wxCHECK( settings, /*void*/ );

    m_leftWinWidth = settings->m_LeftWinWidth;
    m_showHistoryPanel = settings->m_ShowHistoryPanel;
}


void KICAD_MANAGER_FRAME::SaveSettings( APP_SETTINGS_BASE* aCfg )
{
    EDA_BASE_FRAME::SaveSettings( aCfg );

    KICAD_SETTINGS* settings = dynamic_cast<KICAD_SETTINGS*>( aCfg );

    wxCHECK( settings, /*void*/ );

    if( m_projectTreePane && m_projectTreePane->IsShown() )
        settings->m_LeftWinWidth = m_projectTreePane->GetSize().x;

    settings->m_ShowHistoryPanel = m_historyPane && m_auimgr.GetPane( m_historyPane ).IsShown();

    if( !m_isClosing )
        settings->m_OpenProjects = GetSettingsManager()->GetOpenProjects();
}


void KICAD_MANAGER_FRAME::PrintPrjInfo()
{
    wxString status = wxString::Format( _( "Project: %s" ), Prj().GetProjectFullName() );
    SetStatusText( status, 0 );
}


bool KICAD_MANAGER_FRAME::IsProjectActive()
{
    return m_active_project;
}


void KICAD_MANAGER_FRAME::OnIdle( wxIdleEvent& aEvent )
{
    if( s_tabCoordinator )
        s_tabCoordinator->ScanAndSyncFrames();

    if( !m_openSavedWindows )
        return;

    m_openSavedWindows = false;

    if( Pgm().GetCommonSettings()->m_Session.remember_open_files )
    {
        int previousOpenCount = std::count_if( Prj().GetLocalSettings().m_files.begin(),
                                               Prj().GetLocalSettings().m_files.end(),
                [&]( const PROJECT_FILE_STATE& f )
                {
                    return !f.fileName.EndsWith( FILEEXT::ProjectFileExtension ) && f.open;
                } );

        if( previousOpenCount > 0 )
        {
            APP_PROGRESS_DIALOG progressReporter( _( "Restoring session" ), wxEmptyString, previousOpenCount, this );

            std::set<wxString> openedFiles;

            int i = 0;

            for( const PROJECT_FILE_STATE& file : Prj().GetLocalSettings().m_files )
            {
                if( file.open && !openedFiles.count( file.fileName ) )
                {
                    progressReporter.Update( i++, wxString::Format( _( "Restoring '%s'" ), file.fileName ) );

                    openedFiles.insert( file.fileName );
                    wxFileName fn( file.fileName );

                    if( fn.GetExt() == FILEEXT::LegacySchematicFileExtension
                        || fn.GetExt() == FILEEXT::KiCadSchematicFileExtension )
                    {
                        GetToolManager()->RunAction( KICAD_MANAGER_ACTIONS::editSchematic );
                    }
                    else if( fn.GetExt() == FILEEXT::LegacyPcbFileExtension
                             || fn.GetExt() == FILEEXT::KiCadPcbFileExtension )
                    {
                        GetToolManager()->RunAction( KICAD_MANAGER_ACTIONS::editPCB );
                    }
                }

                wxYield();
            }
        }
    }

    Prj().GetLocalSettings().ClearFileState();

    if( m_restoredFromHistory )
    {
        m_restoredFromHistory = false;

        if( KIWAY_PLAYER* schFrame = Kiway().Player( FRAME_SCH, false ) )
            schFrame->OnModify();

        if( KIWAY_PLAYER* pcbFrame = Kiway().Player( FRAME_PCB_EDITOR, false ) )
            pcbFrame->OnModify();
    }

    KICAD_SETTINGS* settings = kicadSettings();

    if( KIPLATFORM::POLICY::GetPolicyBool( POLICY_KEY_PCM ) != KIPLATFORM::POLICY::PBOOL::DISABLED
        && settings->m_PcmUpdateCheck )
    {
        if( !m_pcm )
            CreatePCM();

        m_pcm->RunBackgroundUpdate();
    }

    Raise();
}


void KICAD_MANAGER_FRAME::SetPcmButton( BITMAP_BUTTON* aButton )
{
    m_pcmButton = aButton;

    updatePcmButtonBadge();
}


void KICAD_MANAGER_FRAME::updatePcmButtonBadge()
{
    if( m_pcmButton )
    {
        if( m_pcmUpdateCount > 0 )
        {
            m_pcmButton->SetShowBadge( true );
            m_pcmButton->SetBadgeText( wxString::Format( "%d", m_pcmUpdateCount ) );
        }
        else
        {
            m_pcmButton->SetShowBadge( false );
        }

        m_pcmButton->Refresh();
    }
}


void KICAD_MANAGER_FRAME::onToolbarSizeChanged()
{
    m_auimgr.DetachPane( m_tbLeft );
    delete m_tbLeft;
    m_tbLeft = nullptr;
    RecreateToolbars();
    m_auimgr.AddPane( m_tbLeft, EDA_PANE().HToolbar().Name( "TopMainToolbar" ).Left().Layer( 2 ) );

    if( s_tabCoordinator )
        s_tabCoordinator->SetLeftToolbar( m_tbLeft );

    m_auimgr.Update();
}


void KICAD_MANAGER_FRAME::ToggleLocalHistory()
{
    wxAuiPaneInfo& pane = m_auimgr.GetPane( m_historyPane );
    bool show = !pane.IsShown();
    pane.Show( show );

    if( show )
        m_historyPane->RefreshHistory( Prj().GetProjectPath() );

    m_auimgr.Update();
}


void KICAD_MANAGER_FRAME::RestoreCommitFromHistory( const wxString& aHash )
{
    if( !Kiway().PlayersClose( true ) )
        return;

    if( Kiway().LocalHistory().RestoreCommit( Prj().GetProjectPath(), aHash, this ) )
    {
        m_restoredFromHistory = true;
    }

    m_projectTreePane->ReCreateTreePrj();
    m_openSavedWindows = true;
    m_historyPane->RefreshHistory( Prj().GetProjectPath() );
}


bool KICAD_MANAGER_FRAME::HistoryPanelShown()
{
    return m_historyPane && m_auimgr.GetPane( m_historyPane ).IsShown();
}