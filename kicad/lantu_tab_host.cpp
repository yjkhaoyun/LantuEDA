// ==================== kicad/lantu_tab_host.cpp ====================
#include "lantu_tab_host.h"
#include <wx/dcbuffer.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

LANTU_TAB_HOST::LANTU_TAB_HOST( wxWindow* aParent )
    : wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxCLIP_CHILDREN )
{
    auto* mainSizer = new wxBoxSizer( wxVERTICAL );

    // 1. 顶部扁平标签栏（科技蓝指示条风格）
    m_tabBar = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxSize( -1, TAB_BAR_HEIGHT ) );
    m_tabBar->SetBackgroundStyle( wxBG_STYLE_PAINT );
    m_tabBar->Bind( wxEVT_PAINT, &LANTU_TAB_HOST::OnTabBarPaint, this );
    m_tabBar->Bind( wxEVT_LEFT_DOWN, &LANTU_TAB_HOST::OnTabBarMouseDown, this );
    m_tabBar->Bind( wxEVT_MOTION, &LANTU_TAB_HOST::OnTabBarMouseMove, this );
    m_tabBar->Bind( wxEVT_LEAVE_WINDOW, &LANTU_TAB_HOST::OnTabBarMouseLeave, this );

    // 2. 下方子窗口内容宿主容器
    m_contentHost = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxCLIP_CHILDREN );
    m_contentHost->SetBackgroundColour( wxColour( 245, 247, 250 ) );
    m_contentHost->Bind( wxEVT_SIZE, &LANTU_TAB_HOST::OnContentResize, this );

    mainSizer->Add( m_tabBar, 0, wxEXPAND );
    mainSizer->Add( m_contentHost, 1, wxEXPAND );
    SetSizer( mainSizer );
}

LANTU_TAB_HOST::~LANTU_TAB_HOST() = default;

void LANTU_TAB_HOST::AddPanelTab( const wxString& aId, const wxString& aTitle, wxWindow* aPanel, bool aClosable )
{
    for( size_t i = 0; i < m_tabs.size(); ++i )
    {
        if( m_tabs[i].id == aId )
        {
            SelectTabByIndex( static_cast<int>( i ) );
            return;
        }
    }

    aPanel->Reparent( m_contentHost );
    LANTU_TAB_ITEM item;
    item.id       = aId;
    item.title    = aTitle;
    item.window   = aPanel;
    item.isFrame  = false;
    item.closable = aClosable;

    m_tabs.push_back( item );
    SelectTabByIndex( static_cast<int>( m_tabs.size() ) - 1 );
}

void LANTU_TAB_HOST::ApplyWin32ChildStyle( wxFrame* aFrame )
{
#ifdef __WXMSW__
    HWND childHwnd = reinterpret_cast<HWND>( aFrame->GetHWND() );
    HWND hostHwnd  = reinterpret_cast<HWND>( m_contentHost->GetHWND() );
    if( !childHwnd || !hostHwnd )
        return;

    // 1. 剥离独立窗口标题栏、边框、最小化/最大化按钮，转换为原生 WS_CHILD
    LONG_PTR style = ::GetWindowLongPtr( childHwnd, GWL_STYLE );
    style &= ~( WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER );
    style |= ( WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS );
    ::SetWindowLongPtr( childHwnd, GWL_STYLE, style );

    // 2. 剥离 WS_EX_APPWINDOW，让该窗口彻底从 Windows 任务栏和 Alt+Tab 列表中消失！
    LONG_PTR exStyle = ::GetWindowLongPtr( childHwnd, GWL_EXSTYLE );
    exStyle &= ~( WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME );
    ::SetWindowLongPtr( childHwnd, GWL_EXSTYLE, exStyle );

    // 3. 挂载到主壳的 m_contentHost 容器内部
    ::SetParent( childHwnd, hostHwnd );

    // 4. 强制刷新窗口非客户区样式并铺满宿主区
    wxSize sz = m_contentHost->GetClientSize();
    ::SetWindowPos( childHwnd, nullptr, 0, 0, sz.GetWidth(), sz.GetHeight(),
                    SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW );
#endif
}

void LANTU_TAB_HOST::EmbedFrameTab( const wxString& aId, const wxString& aTitle, wxFrame* aFrame, bool aClosable )
{
    if( !aFrame )
        return;

    // 如果该编辑器标签页已经打开，直接激活切换过去（更新标题）
    for( size_t i = 0; i < m_tabs.size(); ++i )
    {
        if( m_tabs[i].window == aFrame || m_tabs[i].id == aId )
        {
            m_tabs[i].window = aFrame;
            m_tabs[i].title  = aTitle;
            SelectTabByIndex( static_cast<int>( i ) );
            return;
        }
    }

    ApplyWin32ChildStyle( aFrame );

    // 监听子窗口销毁/隐藏事件，防止出现僵尸标签
    aFrame->Bind( wxEVT_DESTROY, [this, aFrame]( wxWindowDestroyEvent& evt ) {
        if( evt.GetWindow() == aFrame )
            DetachWindow( aFrame );
        evt.Skip();
    } );

    LANTU_TAB_ITEM item;
    item.id       = aId;
    item.title    = aTitle;
    item.window   = aFrame;
    item.isFrame  = true;
    item.closable = aClosable;

    m_tabs.push_back( item );
    SelectTabByIndex( static_cast<int>( m_tabs.size() ) - 1 );
}

void LANTU_TAB_HOST::SelectTab( const wxString& aId )
{
    for( size_t i = 0; i < m_tabs.size(); ++i )
    {
        if( m_tabs[i].id == aId )
        {
            SelectTabByIndex( static_cast<int>( i ) );
            return;
        }
    }
}

void LANTU_TAB_HOST::SelectTabByIndex( int aIndex )
{
    if( aIndex < 0 || aIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    m_activeIndex = aIndex;

    for( int i = 0; i < static_cast<int>( m_tabs.size() ); ++i )
    {
        wxWindow* win = m_tabs[i].window;
        if( !win )
            continue;

        bool isActive = ( i == m_activeIndex );
        if( isActive )
        {
            win->Show( true );
            ResizeActiveWindow();
#ifdef __WXMSW__
            if( m_tabs[i].isFrame )
            {
                HWND hwnd = reinterpret_cast<HWND>( win->GetHWND() );
                ::ShowWindow( hwnd, SW_SHOW );
                ::SetFocus( hwnd );
            }
            else
            {
                win->SetFocus();
            }
#else
            win->SetFocus();
#endif
        }
        else
        {
            win->Show( false );
        }
    }

    m_tabBar->Refresh();
}

void LANTU_TAB_HOST::ResizeActiveWindow()
{
    if( m_activeIndex < 0 || m_activeIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    wxWindow* win = m_tabs[m_activeIndex].window;
    if( !win )
        return;

    wxSize sz = m_contentHost->GetClientSize();
    if( sz.GetWidth() <= 0 || sz.GetHeight() <= 0 )
        return;

#ifdef __WXMSW__
    if( m_tabs[m_activeIndex].isFrame )
    {
        HWND hwnd = reinterpret_cast<HWND>( win->GetHWND() );
        ::SetWindowPos( hwnd, nullptr, 0, 0, sz.GetWidth(), sz.GetHeight(),
                        SWP_NOZORDER | SWP_NOACTIVATE );
        return;
    }
#endif
    win->SetSize( 0, 0, sz.GetWidth(), sz.GetHeight() );
}

void LANTU_TAB_HOST::CloseTabByIndex( int aIndex )
{
    if( aIndex < 0 || aIndex >= static_cast<int>( m_tabs.size() ) )
        return;

    if( !m_tabs[aIndex].closable )
        return;

    wxWindow* win = m_tabs[aIndex].window;
    if( win && m_tabs[aIndex].isFrame )
    {
        // 触发 KiCad 原生的 Close() 逻辑（如果原理图或 PCB 有未保存修改，会正常弹出保存确认框）
        if( !win->Close() )
            return; // 用户点了取消保存，则不关闭标签
    }

    DetachWindow( win );
}

void LANTU_TAB_HOST::DetachWindow( wxWindow* aWindow )
{
    for( auto it = m_tabs.begin(); it != m_tabs.end(); ++it )
    {
        if( it->window == aWindow )
        {
            int idx = static_cast<int>( std::distance( m_tabs.begin(), it ) );
            m_tabs.erase( it );

            if( m_tabs.empty() )
            {
                m_activeIndex = -1;
            }
            else if( m_activeIndex >= idx )
            {
                SelectTabByIndex( std::max( 0, m_activeIndex - 1 ) );
            }
            m_tabBar->Refresh();
            return;
        }
    }
}

bool LANTU_TAB_HOST::CloseAllEmbeddedFrames()
{
    // 倒序关闭所有嵌入的 wxFrame，确保每个编辑器都有机会提示保存
    for( int i = static_cast<int>( m_tabs.size() ) - 1; i >= 0; --i )
    {
        if( m_tabs[i].isFrame && m_tabs[i].window )
        {
            if( !m_tabs[i].window->Close() )
                return false;
        }
    }
    return true;
}

void LANTU_TAB_HOST::OnContentResize( wxSizeEvent& aEvent )
{
    ResizeActiveWindow();
    aEvent.Skip();
}

void LANTU_TAB_HOST::OnTabBarPaint( wxPaintEvent& )
{
    wxAutoBufferedPaintDC dc( m_tabBar );
    wxSize sz = m_tabBar->GetClientSize();

    // 标签栏背景（浅灰底 + 底部分割线）
    dc.SetBrush( wxBrush( wxColour( 236, 240, 245 ) ) );
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.DrawRectangle( 0, 0, sz.GetWidth(), sz.GetHeight() );

    dc.SetPen( wxPen( wxColour( 212, 218, 226 ), 1 ) );
    dc.DrawLine( 0, sz.GetHeight() - 1, sz.GetWidth(), sz.GetHeight() - 1 );

    wxFont font = m_tabBar->GetFont();
    font.SetPointSize( 9 );
    dc.SetFont( font );

    int x = 6;
    for( int i = 0; i < static_cast<int>( m_tabs.size() ); ++i )
    {
        auto& tab = m_tabs[i];
        wxSize txtSz = dc.GetTextExtent( tab.title );
        int tabW = std::max( 110, txtSz.GetWidth() + ( tab.closable ? 44 : 28 ) );
        wxRect r( x, 4, tabW, sz.GetHeight() - 4 );
        tab.tabRect = r;

        bool isActive = ( i == m_activeIndex );
        bool isHover  = ( i == m_hoverIndex );

        if( isActive )
        {
            // 激活态：纯白背景 + 顶部深科技蓝高亮条
            dc.SetBrush( wxBrush( *wxWHITE ) );
            dc.SetPen( wxPen( wxColour( 212, 218, 226 ), 1 ) );
            dc.DrawRoundedRectangle( r.x, r.y, r.width, r.height + 4, 4 );

            // 顶部科技蓝指示条
            dc.SetBrush( wxBrush( wxColour( 22, 100, 225 ) ) );
            dc.SetPen( *wxTRANSPARENT_PEN );
            dc.DrawRectangle( r.x + 1, r.y, r.width - 2, 3 );

            dc.SetTextForeground( wxColour( 22, 100, 225 ) );
        }
        else
        {
            if( isHover )
            {
                dc.SetBrush( wxBrush( wxColour( 224, 230, 238 ) ) );
                dc.SetPen( *wxTRANSPARENT_PEN );
                dc.DrawRoundedRectangle( r.x, r.y + 2, r.width, r.height - 2, 4 );
            }
            dc.SetTextForeground( wxColour( 65, 75, 90 ) );
        }

        // 绘制标签文字
        int textY = r.y + ( r.height - txtSz.GetHeight() ) / 2 + 1;
        dc.DrawText( tab.title, r.x + 14, textY );

        // 绘制关闭按钮 ×
        if( tab.closable )
        {
            wxRect cr( r.GetRight() - 22, r.y + ( r.height - 14 ) / 2, 14, 14 );
            tab.closeRect = cr;
            dc.SetPen( wxPen( isActive ? wxColour( 120, 130, 145 ) : wxColour( 150, 158, 170 ), 1 ) );
            dc.DrawLine( cr.x + 3, cr.y + 3, cr.GetRight() - 3, cr.GetBottom() - 3 );
            dc.DrawLine( cr.GetRight() - 3, cr.y + 3, cr.x + 3, cr.GetBottom() - 3 );
        }
        else
        {
            tab.closeRect = wxRect();
        }

        x += tabW + 4;
    }
}

void LANTU_TAB_HOST::OnTabBarMouseDown( wxMouseEvent& aEvent )
{
    wxPoint pt = aEvent.GetPosition();
    for( int i = 0; i < static_cast<int>( m_tabs.size() ); ++i )
    {
        if( m_tabs[i].closable && m_tabs[i].closeRect.Contains( pt ) )
        {
            CloseTabByIndex( i );
            return;
        }
        if( m_tabs[i].tabRect.Contains( pt ) )
        {
            SelectTabByIndex( i );
            return;
        }
    }
}

void LANTU_TAB_HOST::OnTabBarMouseMove( wxMouseEvent& aEvent )
{
    wxPoint pt = aEvent.GetPosition();
    int newHover = -1;
    for( int i = 0; i < static_cast<int>( m_tabs.size() ); ++i )
    {
        if( m_tabs[i].tabRect.Contains( pt ) )
        {
            newHover = i;
            break;
        }
    }
    if( newHover != m_hoverIndex )
    {
        m_hoverIndex = newHover;
        m_tabBar->Refresh();
    }
}

void LANTU_TAB_HOST::OnTabBarMouseLeave( wxMouseEvent& )
{
    if( m_hoverIndex != -1 )
    {
        m_hoverIndex = -1;
        m_tabBar->Refresh();
    }
}