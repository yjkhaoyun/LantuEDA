/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004 Jean-Pierre Charras, jaen-pierre.charras@gipsa-lab.inpg.com
 * Copyright (C) 2011 Wayne Stambaugh <stambaughw@verizon.net>
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

#include <widgets/msgpanel.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcscreen.h>
#include <wx/dialog.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/toplevel.h>

#include <advanced_config.h>
#include <eda_base_frame.h>
#include <kiid.h>
#include <widgets/ui_common.h>


namespace
{

struct MSG_ITEM_GEOM
{
    int      x = 0;
    int      width = 0;
    int      upperY = 0;
    int      lowerY = 0;
    wxString upperText;
    wxString lowerText;
};

struct MSG_PANEL_UX_STATE
{
    explicit MSG_PANEL_UX_STATE( EDA_MSG_PANEL* aOwner ) :
            toastTimer( aOwner )
    {
    }

    std::vector<MSG_ITEM_GEOM> geoms;
    int                        totalWidth = 0;
    int                        scrollX = 0;
    int                        hoverIndex = -1;
    int                        selectedIndex = -1;
    int                        hoverNavBtn = 0; // -1 = left arrow, 1 = right arrow, 0 = none
    bool                       isDragging = false;
    int                        dragStartX = 0;
    int                        dragStartScrollX = 0;
    wxString                   toastText;
    wxTimer                    toastTimer;
};

static std::unordered_map<const EDA_MSG_PANEL*, std::unique_ptr<MSG_PANEL_UX_STATE>> s_uxStates;


static MSG_PANEL_UX_STATE& getUxState( EDA_MSG_PANEL* aPanel )
{
    auto it = s_uxStates.find( aPanel );

    if( it == s_uxStates.end() )
        it = s_uxStates.emplace( aPanel, std::make_unique<MSG_PANEL_UX_STATE>( aPanel ) ).first;

    return *it->second;
}


static void copyStringToClipboard( const wxString& aText )
{
    if( aText.IsEmpty() )
        return;

    if( wxTheClipboard && wxTheClipboard->Open() )
    {
        wxTheClipboard->SetData( new wxTextDataObject( aText ) );
        wxTheClipboard->Close();
    }
}


static wxString truncateForMenuLabel( const wxString& aStr, size_t aMaxChars = 38 )
{
    wxString clean = aStr;
    clean.Replace( wxS( "\r" ), wxS( " " ) );
    clean.Replace( wxS( "\n" ), wxS( " " ) );

    if( clean.Length() > aMaxChars )
        return clean.Left( aMaxChars ) + wxS( "..." );

    return clean;
}


static void parseMsgItemContent( const wxString& aUpper, const wxString& aLower,
                                 wxString& outPrimaryTitle, wxString& outPrimaryValue,
                                 wxString& outSecondaryTitle, wxString& outSecondaryValue )
{
    outPrimaryTitle.Clear();
    outPrimaryValue.Clear();
    outSecondaryTitle.Clear();
    outSecondaryValue.Clear();

    const wxString cnColon( L"\uFF1A" );
    bool upperHasColon = aUpper.Contains( wxS( ": " ) ) || aUpper.Contains( cnColon );
    bool lowerHasColon = aLower.Contains( wxS( ": " ) ) || aLower.Contains( cnColon );

    auto splitColonPair = [&cnColon]( const wxString& aLine, wxString& rKey, wxString& rVal )
    {
        int pos = aLine.Find( wxS( ": " ) );
        if( pos != wxNOT_FOUND )
        {
            rKey = aLine.Left( static_cast<size_t>( pos ) ).Strip( wxString::both );
            rVal = aLine.Mid( static_cast<size_t>( pos ) + 2 ).Strip( wxString::both );
            return;
        }

        int cnPos = aLine.Find( cnColon );
        if( cnPos != wxNOT_FOUND )
        {
            rKey = aLine.Left( static_cast<size_t>( cnPos ) ).Strip( wxString::both );
            rVal = aLine.Mid( static_cast<size_t>( cnPos ) + cnColon.Length() ).Strip( wxString::both );
            return;
        }

        rKey.Clear();
        rVal = aLine.Strip( wxString::both );
    };

    if( !aUpper.IsEmpty() && !aLower.IsEmpty() )
    {
        if( upperHasColon && lowerHasColon )
        {
            splitColonPair( aUpper, outPrimaryTitle, outPrimaryValue );
            splitColonPair( aLower, outSecondaryTitle, outSecondaryValue );
        }
        else
        {
            outPrimaryTitle = aUpper.Strip( wxString::both );
            outPrimaryValue = aLower.Strip( wxString::both );
        }
    }
    else if( !aUpper.IsEmpty() )
    {
        splitColonPair( aUpper, outPrimaryTitle, outPrimaryValue );
    }
    else if( !aLower.IsEmpty() )
    {
        splitColonPair( aLower, outPrimaryTitle, outPrimaryValue );
    }
}


static std::vector<wxString> splitSpecTokens( const wxString& aText )
{
    std::vector<wxString> tokens;

    if( !aText.Contains( wxS( ";" ) ) && !aText.Contains( wxS( "|" ) ) )
        return tokens;

    wxString current;

    for( size_t i = 0; i < aText.Length(); ++i )
    {
        wxUniChar ch = aText[i];

        if( ch == ';' || ch == '|' )
        {
            wxString trimmed = current.Strip( wxString::both );
            if( !trimmed.IsEmpty() )
                tokens.push_back( trimmed );
            current.Clear();
        }
        else
        {
            current += ch;
        }
    }

    wxString last = current.Strip( wxString::both );
    if( !last.IsEmpty() )
        tokens.push_back( last );

    return tokens;
}


static int getMaxScrollX( EDA_MSG_PANEL* aPanel, const MSG_PANEL_UX_STATE& aState )
{
    int clientW = aPanel->GetClientSize().x;

    if( clientW <= 0 || aState.totalWidth <= clientW )
        return 0;

    return aState.totalWidth - clientW + aPanel->FromDIP( 28 );
}


static void clampScrollX( EDA_MSG_PANEL* aPanel, MSG_PANEL_UX_STATE& aState )
{
    int maxScroll = getMaxScrollX( aPanel, aState );

    if( aState.scrollX > maxScroll )
        aState.scrollX = maxScroll;

    if( aState.scrollX < 0 )
        aState.scrollX = 0;
}


static int hitTestNavButton( EDA_MSG_PANEL* aPanel, const MSG_PANEL_UX_STATE& aState, const wxPoint& aPt )
{
    wxSize sz = aPanel->GetClientSize();
    int    navW = aPanel->FromDIP( 22 );
    int    maxScroll = getMaxScrollX( aPanel, aState );

    if( aState.scrollX > 0 && wxRect( 0, 0, navW, sz.y ).Contains( aPt ) )
        return -1;

    if( maxScroll > 0 && aState.scrollX < maxScroll && wxRect( sz.x - navW, 0, navW, sz.y ).Contains( aPt ) )
        return 1;

    return 0;
}


static int hitTestItem( EDA_MSG_PANEL* aPanel, const MSG_PANEL_UX_STATE& aState, const wxPoint& aPt )
{
    wxSize sz = aPanel->GetClientSize();

    if( aPt.y < 0 || aPt.y > sz.y )
        return -1;

    const int padHalf = aPanel->FromDIP( 6 );

    for( size_t i = 0; i < aState.geoms.size(); ++i )
    {
        int x0 = aState.geoms[i].x - aState.scrollX - padHalf;
        int x1 = x0 + aState.geoms[i].width;

        if( aPt.x >= x0 && aPt.x <= x1 )
            return static_cast<int>( i );
    }

    return -1;
}


static void updateHoverTooltip( EDA_MSG_PANEL* aPanel, const MSG_PANEL_UX_STATE& aState, int aItemIndex )
{
    if( aItemIndex < 0 || static_cast<size_t>( aItemIndex ) >= aState.geoms.size() )
    {
        aPanel->UnsetToolTip();
        return;
    }

    const MSG_ITEM_GEOM& item = aState.geoms[static_cast<size_t>( aItemIndex )];
    wxString title1, val1, title2, val2;
    parseMsgItemContent( item.upperText, item.lowerText, title1, val1, title2, val2 );

    wxString tip;
    if( !title1.IsEmpty() )
        tip += wxString( L"\u3010" ) + title1 + wxString( L"\u3011\n" );

    std::vector<wxString> tokens = splitSpecTokens( val1 );
    if( tokens.size() >= 2 )
    {
        for( const wxString& tk : tokens )
            tip += wxString( L"\u2022 " ) + tk + wxS( "\n" );
    }
    else if( !val1.IsEmpty() )
    {
        tip += val1 + wxS( "\n" );
    }

    if( !val2.IsEmpty() )
    {
        if( !title2.IsEmpty() )
            tip += wxS( "\n" ) + wxString( L"\u3010" ) + title2 + wxString( L"\u3011\n" );
        tip += val2 + wxS( "\n" );
    }

    // 提示: 右键复制 | 双击自由划选 | 滚轮左右滚动
    tip += wxString( L"\n\u63D0\u793A: \u53F3\u952E\u590D\u5236 | \u53CC\u51FB\u81EA\u7531\u5212\u9009 | \u6EDA\u8F6E\u5DE6\u53F3\u6EDA\u52A8" );
    aPanel->SetToolTip( tip );
}


static void showCopyToast( EDA_MSG_PANEL* aPanel, MSG_PANEL_UX_STATE& aState, const wxString& aCopiedText )
{
    // ✓ 已复制:
    aState.toastText = wxString( L"\u2713 \u5DF2\u590D\u5236: " ) + truncateForMenuLabel( aCopiedText, 24 );
    aPanel->Refresh();
    aState.toastTimer.StartOnce( 1400 );
}


static void restoreCanvasFocus( EDA_MSG_PANEL* aPanel )
{
    wxWindow* parent = aPanel->GetParent();

    while( parent )
    {
        if( auto* edaFrame = dynamic_cast<EDA_BASE_FRAME*>( parent ) )
        {
            if( wxWindow* canvas = edaFrame->GetToolCanvas() )
                canvas->SetFocus();
            return;
        }

        parent = parent->GetParent();
    }
}


static void showSelectableTextDialog( EDA_MSG_PANEL* aPanel, MSG_PANEL_UX_STATE& aState, int aItemIndex )
{
    if( aState.geoms.empty() )
        return;

    wxString fullContent;
    long     selStart = 0;
    long     selEnd   = 0;

    if( aItemIndex >= 0 && static_cast<size_t>( aItemIndex ) < aState.geoms.size() )
    {
        const MSG_ITEM_GEOM& target = aState.geoms[static_cast<size_t>( aItemIndex )];
        wxString t1, v1, t2, v2;
        parseMsgItemContent( target.upperText, target.lowerText, t1, v1, t2, v2 );

        fullContent = v1;
        selStart    = 0;
        selEnd      = static_cast<long>( v1.Length() );

        if( !v2.IsEmpty() )
            fullContent += wxS( "\r\n" ) + v2;

        // -------- 全部属性 --------
        fullContent += wxString( L"\r\n\r\n-------- \u5168\u90E8\u5C5E\u6027 --------\r\n" );
    }

    for( const MSG_ITEM_GEOM& it : aState.geoms )
    {
        wxString t1, v1, t2, v2;
        parseMsgItemContent( it.upperText, it.lowerText, t1, v1, t2, v2 );

        if( !t1.IsEmpty() )
            fullContent += t1 + wxS( ": " ) + v1 + wxS( "\r\n" );
        else if( !v1.IsEmpty() )
            fullContent += v1 + wxS( "\r\n" );

        if( !v2.IsEmpty() )
            fullContent += ( t2.IsEmpty() ? wxString() : wxString( t2 + wxS( ": " ) ) ) + v2 + wxS( "\r\n" );
    }

    wxWindow* topParent = wxGetTopLevelParent( aPanel );
    // 属性详情 (可自由划选并复制)
    wxDialog  dlg( topParent ? topParent : aPanel, wxID_ANY,
                   wxString( L"\u5C5E\u6027\u8BE6\u60C5 (\u53EF\u81EA\u7531\u5212\u9009\u5E76\u590D\u5236)" ),
                   wxDefaultPosition, aPanel->FromDIP( wxSize( 560, 320 ) ),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER );

    auto* mainSizer = new wxBoxSizer( wxVERTICAL );
    auto* textCtrl  = new wxTextCtrl( &dlg, wxID_ANY, fullContent, wxDefaultPosition, wxDefaultSize,
                                      wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL );
    textCtrl->SetFont( KIUI::GetControlFont( aPanel ) );

    auto* btnSizer = new wxBoxSizer( wxHORIZONTAL );
    // 复制选中内容 / 关闭
    auto* copyBtn  = new wxButton( &dlg, wxID_ANY, wxString( L"\u590D\u5236\u9009\u4E2D\u5185\u5BB9" ) );
    auto* closeBtn = new wxButton( &dlg, wxID_CANCEL, wxString( L"\u5173\u95ED" ) );

    copyBtn->Bind( wxEVT_BUTTON, [aPanel, &aState, textCtrl, &dlg]( wxCommandEvent& ) {
        wxString sel = textCtrl->GetStringSelection();
        if( sel.IsEmpty() )
            sel = textCtrl->GetValue();
        copyStringToClipboard( sel );
        showCopyToast( aPanel, aState, sel );
        dlg.EndModal( wxID_OK );
    } );

    btnSizer->AddStretchSpacer( 1 );
    btnSizer->Add( copyBtn, 0, wxRIGHT, aPanel->FromDIP( 8 ) );
    btnSizer->Add( closeBtn, 0 );

    mainSizer->Add( textCtrl, 1, wxEXPAND | wxALL, aPanel->FromDIP( 10 ) );
    mainSizer->Add( btnSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, aPanel->FromDIP( 10 ) );

    dlg.SetSizer( mainSizer );
    dlg.CentreOnParent();

    textCtrl->SetFocus();
    if( selEnd > selStart )
        textCtrl->SetSelection( selStart, selEnd );

    dlg.ShowModal();
    restoreCanvasFocus( aPanel );
}

} // namespace


BEGIN_EVENT_TABLE( EDA_MSG_PANEL, wxPanel )
    EVT_DPI_CHANGED( EDA_MSG_PANEL::OnDPIChanged )
    EVT_SIZE( EDA_MSG_PANEL::OnSize )
    EVT_PAINT( EDA_MSG_PANEL::OnPaint )
END_EVENT_TABLE()


EDA_MSG_PANEL::EDA_MSG_PANEL( wxWindow* aParent, int aId, const wxPoint& aPosition,
                              const wxSize& aSize, long style, const wxString& name ) :
        wxPanel( aParent, aId, aPosition, aSize, style, name )
{
    SetFont( KIUI::GetStatusFont( this ) );
    SetBackgroundColour( wxSystemSettings::GetColour( wxSYS_COLOUR_BTNFACE ) );
    SetBackgroundStyle( wxBG_STYLE_PAINT );

    m_last_x = 0;

    updateFontSize();
    InvalidateBestSize();

    MSG_PANEL_UX_STATE& initState = getUxState( this );

    Bind( wxEVT_MOTION,
          [this]( wxMouseEvent& aEvent )
          {
              MSG_PANEL_UX_STATE& st = getUxState( this );
              wxPoint             pt = aEvent.GetPosition();

              if( st.isDragging && aEvent.LeftIsDown() )
              {
                  int dx = pt.x - st.dragStartX;
                  if( std::abs( dx ) > FromDIP( 3 ) )
                  {
                      st.scrollX = st.dragStartScrollX - dx;
                      clampScrollX( this, st );
                      Refresh();
                      return;
                  }
              }

              int navHit  = hitTestNavButton( this, st, pt );
              int itemHit = ( navHit == 0 ) ? hitTestItem( this, st, pt ) : -1;

              if( navHit != st.hoverNavBtn || itemHit != st.hoverIndex )
              {
                  st.hoverNavBtn = navHit;
                  st.hoverIndex  = itemHit;

                  SetCursor( ( navHit != 0 || itemHit >= 0 ) ? wxCursor( wxCURSOR_HAND ) : wxNullCursor );
                  updateHoverTooltip( this, st, itemHit );
                  Refresh();
              }
          } );

    Bind( wxEVT_LEAVE_WINDOW,
          [this]( wxMouseEvent& )
          {
              MSG_PANEL_UX_STATE& st = getUxState( this );
              st.isDragging = false;

              if( st.hoverIndex != -1 || st.hoverNavBtn != 0 )
              {
                  st.hoverIndex  = -1;
                  st.hoverNavBtn = 0;
                  SetCursor( wxNullCursor );
                  Refresh();
              }
          } );

    Bind( wxEVT_LEFT_DOWN,
          [this]( wxMouseEvent& aEvent )
          {
              MSG_PANEL_UX_STATE& st = getUxState( this );
              wxPoint             pt = aEvent.GetPosition();

              int navHit = hitTestNavButton( this, st, pt );
              if( navHit != 0 )
              {
                  st.scrollX += navHit * FromDIP( 220 );
                  clampScrollX( this, st );
                  Refresh();
                  return;
              }

              st.selectedIndex    = hitTestItem( this, st, pt );
              st.isDragging       = true;
              st.dragStartX       = pt.x;
              st.dragStartScrollX = st.scrollX;

              SetFocus();
              Refresh();
          } );

    Bind( wxEVT_LEFT_UP,
          [this]( wxMouseEvent& )
          {
              getUxState( this ).isDragging = false;
          } );

    Bind( wxEVT_LEFT_DCLICK,
          [this]( wxMouseEvent& aEvent )
          {
              MSG_PANEL_UX_STATE& st  = getUxState( this );
              int                 idx = hitTestItem( this, st, aEvent.GetPosition() );

              if( idx >= 0 )
              {
                  st.selectedIndex = idx;
                  Refresh();
                  showSelectableTextDialog( this, st, idx );
              }
          } );

    Bind( wxEVT_RIGHT_DOWN,
          [this]( wxMouseEvent& aEvent )
          {
              MSG_PANEL_UX_STATE& st = getUxState( this );
              if( st.geoms.empty() )
                  return;

              int idx = hitTestItem( this, st, aEvent.GetPosition() );
              if( idx >= 0 )
                  st.selectedIndex = idx;
              else if( st.selectedIndex < 0 )
                  st.selectedIndex = 0;

              Refresh();

              wxMenu menu;
              constexpr int ID_COPY_PRIMARY_VAL   = 10001;
              constexpr int ID_COPY_PRIMARY_PAIR  = 10002;
              constexpr int ID_COPY_SECONDARY_VAL = 10003;
              constexpr int ID_COPY_ALL_ITEMS     = 10004;
              constexpr int ID_OPEN_SELECT_DLG    = 10005;
              constexpr int ID_COPY_TOKEN_BASE    = 10100;

              wxString title1, val1, title2, val2;
              if( st.selectedIndex >= 0 && static_cast<size_t>( st.selectedIndex ) < st.geoms.size() )
              {
                  const MSG_ITEM_GEOM& cur = st.geoms[static_cast<size_t>( st.selectedIndex )];
                  parseMsgItemContent( cur.upperText, cur.lowerText, title1, val1, title2, val2 );
              }

              std::vector<wxString> subTokens = splitSpecTokens( val1 );

              if( !val1.IsEmpty() )
              {
                  // 复制 "xxx" / 复制<标题> "xxx"
                  wxString labelPrefix = title1.IsEmpty()
                          ? wxString( L"\u590D\u5236 \"" )
                          : ( wxString( L"\u590D\u5236" ) + title1 + wxS( " \"" ) );
                  menu.Append( ID_COPY_PRIMARY_VAL,
                               labelPrefix + truncateForMenuLabel( val1 ) + wxS( "\"" ) );

                  if( !title1.IsEmpty() )
                  {
                      // 复制带标题 "xxx"
                      menu.Append( ID_COPY_PRIMARY_PAIR,
                                   wxString( L"\u590D\u5236\u5E26\u6807\u9898 \"" )
                                           + truncateForMenuLabel( title1 + wxS( ": " ) + val1 ) + wxS( "\"" ) );
                  }
              }

              if( !val2.IsEmpty() )
              {
                  wxString secPrefix = title2.IsEmpty()
                          ? wxString( L"\u590D\u5236 \"" )
                          : ( wxString( L"\u590D\u5236" ) + title2 + wxS( " \"" ) );
                  menu.Append( ID_COPY_SECONDARY_VAL,
                               secPrefix + truncateForMenuLabel( val2 ) + wxS( "\"" ) );
              }

              std::vector<wxString> tokenPureValues;
              if( subTokens.size() >= 2 )
              {
                  auto* subMenu = new wxMenu();
                  int   maxSub  = std::min<int>( static_cast<int>( subTokens.size() ), 25 );

                  for( int i = 0; i < maxSub; ++i )
                  {
                      const wxString& tk = subTokens[static_cast<size_t>( i )];
                      wxString pureVal = tk;
                      if( tk.Contains( wxS( ":" ) ) )
                          pureVal = tk.AfterFirst( ':' ).Strip( wxString::both );

                      tokenPureValues.push_back( pureVal.IsEmpty() ? tk : pureVal );
                      subMenu->Append( ID_COPY_TOKEN_BASE + i, truncateForMenuLabel( tk, 48 ) );
                  }

                  // 复制单项参数
                  menu.AppendSubMenu( subMenu, wxString( L"\u590D\u5236\u5355\u9879\u53C2\u6570" ) );
              }

              if( menu.GetMenuItemCount() > 0 )
                  menu.AppendSeparator();

              // 复制整行全部属性 / 查看/划选完整文本...
              menu.Append( ID_COPY_ALL_ITEMS, wxString( L"\u590D\u5236\u6574\u884C\u5168\u90E8\u5C5E\u6027" ) );
              menu.Append( ID_OPEN_SELECT_DLG, wxString( L"\u67E5\u770B/\u5212\u9009\u5B8C\u6574\u6587\u672C..." ) );

              menu.Bind( wxEVT_MENU, [&]( wxCommandEvent& aMenuEvt ) {
                  int id = aMenuEvt.GetId();

                  if( id == ID_COPY_PRIMARY_VAL )
                  {
                      copyStringToClipboard( val1 );
                      showCopyToast( this, st, val1 );
                  }
                  else if( id == ID_COPY_PRIMARY_PAIR )
                  {
                      wxString pairStr = title1 + wxS( ": " ) + val1;
                      copyStringToClipboard( pairStr );
                      showCopyToast( this, st, pairStr );
                  }
                  else if( id == ID_COPY_SECONDARY_VAL )
                  {
                      copyStringToClipboard( val2 );
                      showCopyToast( this, st, val2 );
                  }
                  else if( id == ID_COPY_ALL_ITEMS )
                  {
                      wxString allStr;
                      for( const MSG_ITEM_GEOM& it : st.geoms )
                      {
                          wxString k1, x1, k2, x2;
                          parseMsgItemContent( it.upperText, it.lowerText, k1, x1, k2, x2 );

                          wxString seg = k1.IsEmpty() ? x1 : wxString( k1 + wxS( ": " ) + x1 );
                          if( !seg.IsEmpty() )
                          {
                              if( !allStr.IsEmpty() )
                                  allStr += wxS( " | " );
                              allStr += seg;
                          }
                          if( !x2.IsEmpty() )
                          {
                              if( !allStr.IsEmpty() )
                                  allStr += wxS( " | " );
                              allStr += k2.IsEmpty() ? x2 : wxString( k2 + wxS( ": " ) + x2 );
                          }
                      }
                      copyStringToClipboard( allStr );
                      showCopyToast( this, st, wxString( L"\u5168\u90E8\u5C5E\u6027" ) );
                  }
                  else if( id == ID_OPEN_SELECT_DLG )
                  {
                      int targetIdx = st.selectedIndex;
                      CallAfter( [this, targetIdx]() {
                          MSG_PANEL_UX_STATE& laterSt = getUxState( this );
                          showSelectableTextDialog( this, laterSt, targetIdx );
                      } );
                  }
                  else if( id >= ID_COPY_TOKEN_BASE
                           && id < ID_COPY_TOKEN_BASE + static_cast<int>( tokenPureValues.size() ) )
                  {
                      const wxString& chosen = tokenPureValues[static_cast<size_t>( id - ID_COPY_TOKEN_BASE )];
                      copyStringToClipboard( chosen );
                      showCopyToast( this, st, chosen );
                  }
              } );

              PopupMenu( &menu, aEvent.GetPosition() );
              restoreCanvasFocus( this );
          } );

    Bind( wxEVT_MOUSEWHEEL,
          [this]( wxMouseEvent& aEvent )
          {
              MSG_PANEL_UX_STATE& st = getUxState( this );
              if( getMaxScrollX( this, st ) <= 0 )
                  return;

              int step = FromDIP( 90 );
              if( aEvent.GetWheelRotation() > 0 )
                  st.scrollX -= step;
              else if( aEvent.GetWheelRotation() < 0 )
                  st.scrollX += step;

              clampScrollX( this, st );
              Refresh();
          } );

    Bind( wxEVT_KEY_DOWN,
          [this]( wxKeyEvent& aEvent )
          {
              MSG_PANEL_UX_STATE& st = getUxState( this );

              if( aEvent.ControlDown() && ( aEvent.GetKeyCode() == 'C' || aEvent.GetKeyCode() == 'c' ) )
              {
                  if( st.selectedIndex >= 0 && static_cast<size_t>( st.selectedIndex ) < st.geoms.size() )
                  {
                      const MSG_ITEM_GEOM& cur = st.geoms[static_cast<size_t>( st.selectedIndex )];
                      wxString t1, v1, t2, v2;
                      parseMsgItemContent( cur.upperText, cur.lowerText, t1, v1, t2, v2 );

                      if( !v1.IsEmpty() )
                      {
                          copyStringToClipboard( v1 );
                          showCopyToast( this, st, v1 );
                          restoreCanvasFocus( this );
                          return;
                      }
                  }
              }

              restoreCanvasFocus( this );
              aEvent.Skip();
          } );

    Bind( wxEVT_TIMER,
          [this]( wxTimerEvent& )
          {
              MSG_PANEL_UX_STATE& st = getUxState( this );
              st.toastText.Clear();
              Refresh();
          },
          initState.toastTimer.GetId() );
}


EDA_MSG_PANEL::~EDA_MSG_PANEL()
{
    auto it = s_uxStates.find( this );

    if( it != s_uxStates.end() )
    {
        if( it->second->toastTimer.IsRunning() )
            it->second->toastTimer.Stop();

        s_uxStates.erase( it );
    }
}


void EDA_MSG_PANEL::updateFontSize()
{
    wxFont font = KIUI::GetControlFont( this );
    GetTextExtent( wxS( "W" ), &m_fontSize.x, &m_fontSize.y, nullptr, nullptr, &font );
}


wxSize EDA_MSG_PANEL::DoGetBestSize() const
{
    return wxSize( wxDefaultCoord, 2 * m_fontSize.y + 4 );
}


wxSize EDA_MSG_PANEL::DoGetBestClientSize() const
{
    return wxPanel::DoGetBestClientSize();
}


void EDA_MSG_PANEL::OnDPIChanged( wxDPIChangedEvent& aEvent )
{
    updateFontSize();
    rebuildItems();
    InvalidateBestSize();
    Refresh();

    aEvent.Skip();
}


void EDA_MSG_PANEL::rebuildItems()
{
    MSG_PANEL_UX_STATE& state = getUxState( this );
    state.geoms.clear();
    state.totalWidth = 0;
    m_last_x = 0;

    for( MSG_PANEL_ITEM& item : m_Items )
        updateItemPos( item );

    clampScrollX( this, state );
}


void EDA_MSG_PANEL::OnSize( wxSizeEvent& aEvent )
{
    rebuildItems();
    Refresh();
    aEvent.Skip();
}


void EDA_MSG_PANEL::OnPaint( wxPaintEvent& )
{
    MSG_PANEL_UX_STATE& state = getUxState( this );

    if( state.geoms.size() != m_Items.size() )
        rebuildItems();

    wxAutoBufferedPaintDC dc( this );

    erase( &dc );

    wxSize   clientSz = GetClientSize();
    wxColour bgColor  = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNFACE );
    bool     isDark   = ( 0.299 * bgColor.Red() + 0.587 * bgColor.Green() + 0.114 * bgColor.Blue() ) < 128.0;

    dc.SetPen( wxPen( isDark ? wxColour( 65, 70, 78 ) : wxColour( 214, 220, 228 ), 1 ) );
    dc.DrawLine( 0, 0, clientSz.x, 0 );

    wxFont baseFont = KIUI::GetControlFont( this );
    dc.SetFont( baseFont );
    dc.SetBackgroundMode( wxTRANSPARENT );

    wxTopLevelWindow* tlw      = dynamic_cast<wxTopLevelWindow*>( wxGetTopLevelParent( this ) );
    bool              isActive = ( !tlw || tlw->IsActive() );

    wxColour labelColor = !isActive
            ? wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT )
            : ( isDark ? wxColour( 162, 172, 188 ) : wxColour( 96, 106, 122 ) );
    wxColour valueColor = !isActive
            ? wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT )
            : wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT );
    wxColour sepColor   = isDark ? wxColour( 64, 68, 76 ) : wxColour( 220, 225, 232 );

    const int padHalf = FromDIP( 6 );

    for( size_t i = 0; i < state.geoms.size(); ++i )
    {
        const MSG_ITEM_GEOM& item = state.geoms[i];
        int drawX = item.x - state.scrollX;

        if( drawX + item.width < 0 || drawX > clientSz.x )
            continue;

        wxRect cellRect( drawX - padHalf, FromDIP( 2 ), item.width, clientSz.y - FromDIP( 4 ) );

        bool isSel   = ( static_cast<int>( i ) == state.selectedIndex );
        bool isHover = ( static_cast<int>( i ) == state.hoverIndex );

        if( isSel )
        {
            dc.SetBrush( wxBrush( isDark ? wxColour( 38, 76, 132 ) : wxColour( 212, 230, 252 ) ) );
            dc.SetPen( wxPen( isDark ? wxColour( 78, 144, 242 ) : wxColour( 48, 118, 226 ), 1 ) );
            dc.DrawRoundedRectangle( cellRect, 4 );
        }
        else if( isHover )
        {
            dc.SetBrush( wxBrush( isDark ? wxColour( 48, 62, 84 ) : wxColour( 228, 238, 252 ) ) );
            dc.SetPen( wxPen( isDark ? wxColour( 76, 108, 156 ) : wxColour( 178, 206, 244 ), 1 ) );
            dc.DrawRoundedRectangle( cellRect, 4 );
        }
        else if( i > 0 )
        {
            dc.SetPen( wxPen( sepColor, 1 ) );
            dc.DrawLine( cellRect.x, FromDIP( 5 ), cellRect.x, clientSz.y - FromDIP( 5 ) );
        }

        bool isPureHeaderValue = !item.upperText.IsEmpty() && !item.lowerText.IsEmpty()
                && !item.upperText.Contains( wxS( ": " ) );

        if( !item.upperText.IsEmpty() )
        {
            dc.SetTextForeground( isPureHeaderValue ? labelColor : valueColor );
            dc.DrawText( item.upperText, drawX, item.upperY );
        }

        if( !item.lowerText.IsEmpty() )
        {
            dc.SetTextForeground( valueColor );
            dc.DrawText( item.lowerText, drawX, item.lowerY );
        }
    }

    int maxScroll = getMaxScrollX( this, state );
    int navW      = FromDIP( 22 );

    auto drawNavArrow = [&]( const wxRect& aRect, const wxString& aArrow, bool aHovered )
    {
        dc.SetBrush( wxBrush( aHovered
                                      ? ( isDark ? wxColour( 58, 92, 148 ) : wxColour( 208, 226, 250 ) )
                                      : ( isDark ? wxColour( 42, 46, 54 ) : wxColour( 236, 240, 246 ) ) ) );
        dc.SetPen( wxPen( isDark ? wxColour( 82, 92, 108 ) : wxColour( 192, 202, 216 ), 1 ) );
        dc.DrawRectangle( aRect );

        dc.SetTextForeground( isDark ? wxColour( 220, 230, 245 ) : wxColour( 28, 88, 185 ) );
        wxSize tsz = dc.GetTextExtent( aArrow );
        dc.DrawText( aArrow,
                     aRect.x + ( aRect.width - tsz.x ) / 2,
                     aRect.y + ( aRect.height - tsz.y ) / 2 );
    };

    if( state.scrollX > 0 )
    {
        wxRect leftBtn( 0, 1, navW, clientSz.y - 1 );
        drawNavArrow( leftBtn, wxString( L"\u25C0" ), state.hoverNavBtn == -1 );
    }

    if( maxScroll > 0 && state.scrollX < maxScroll )
    {
        wxRect rightBtn( clientSz.x - navW, 1, navW, clientSz.y - 1 );
        drawNavArrow( rightBtn, wxString( L"\u25B6" ), state.hoverNavBtn == 1 );
    }

    if( !state.toastText.IsEmpty() )
    {
        wxSize tsz = dc.GetTextExtent( state.toastText );
        int    tw  = tsz.x + FromDIP( 20 );
        int    th  = std::min( clientSz.y - FromDIP( 4 ), tsz.y + FromDIP( 8 ) );
        int    tx  = std::max( FromDIP( 8 ),
                               clientSz.x - tw - ( maxScroll > 0 ? navW + FromDIP( 8 ) : FromDIP( 12 ) ) );
        int    ty  = ( clientSz.y - th ) / 2;

        dc.SetBrush( wxBrush( wxColour( 24, 96, 208 ) ) );
        dc.SetPen( *wxTRANSPARENT_PEN );
        dc.DrawRoundedRectangle( tx, ty, tw, th, 4 );

        dc.SetTextForeground( *wxWHITE );
        dc.DrawText( state.toastText, tx + FromDIP( 10 ), ty + ( th - tsz.y ) / 2 );
    }
}


void EDA_MSG_PANEL::updateItemPos( MSG_PANEL_ITEM& item )
{
    wxFont   font      = KIUI::GetControlFont( this );
    wxString upperText = item.GetUpperText();
    wxString lowerText = item.GetLowerText();
    wxSize   drawSize  = GetClientSize();

    int upperW = 0, upperH = 0;
    int lowerW = 0, lowerH = 0;

    if( !upperText.IsEmpty() )
        GetTextExtent( upperText, &upperW, &upperH, nullptr, nullptr, &font );

    if( !lowerText.IsEmpty() )
        GetTextExtent( lowerText, &lowerW, &lowerH, nullptr, nullptr, &font );

    int contentW = std::max( upperW, lowerW );
    int extraPad = std::max( FromDIP( 16 ), ( item.GetPadding() * m_fontSize.x ) / 2 );

    if( m_last_x == 0 )
        m_last_x = std::max( m_fontSize.x, FromDIP( 10 ) );

    item.m_X = m_last_x;

    int totalTextH = m_fontSize.y * 2 + FromDIP( 2 );
    int topStart   = std::max( FromDIP( 1 ), ( drawSize.y - totalTextH ) / 2 );

    item.m_UpperY = topStart;
    item.m_LowerY = topStart + m_fontSize.y + FromDIP( 1 );

    int itemWidth = contentW + extraPad;
    m_last_x += itemWidth + FromDIP( 4 );

    MSG_PANEL_UX_STATE& state = getUxState( this );
    MSG_ITEM_GEOM       geom;
    geom.x         = item.m_X;
    geom.width     = itemWidth;
    geom.upperY    = item.m_UpperY;
    geom.lowerY    = item.m_LowerY;
    geom.upperText = upperText;
    geom.lowerText = lowerText;
    state.geoms.push_back( std::move( geom ) );
    state.totalWidth = m_last_x;
}


void EDA_MSG_PANEL::AppendMessage( const wxString& aUpperText, const wxString& aLowerText, int aPadding )
{
    MSG_PANEL_ITEM item( aUpperText, aLowerText, aPadding );

    updateItemPos( item );
    m_Items.push_back( item );

    Refresh();
}


void EDA_MSG_PANEL::showItem( wxDC& aDC, const MSG_PANEL_ITEM& aItem )
{
    COLOR4D           color;
    wxTopLevelWindow* tlw = dynamic_cast<wxTopLevelWindow*>( wxGetTopLevelParent( this ) );

    if( tlw && !tlw->IsActive() )
        color = wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT );
    else
        color = wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT );

    aDC.SetTextForeground( color.ToColour() );

    int scrollOffset = getUxState( this ).scrollX;

    if( !aItem.m_UpperText.IsEmpty() )
        aDC.DrawText( aItem.m_UpperText, aItem.m_X - scrollOffset, aItem.m_UpperY );

    if( !aItem.m_LowerText.IsEmpty() )
        aDC.DrawText( aItem.m_LowerText, aItem.m_X - scrollOffset, aItem.m_LowerY );
}


void EDA_MSG_PANEL::EraseMsgBox()
{
    m_Items.clear();
    m_last_x = 0;

    MSG_PANEL_UX_STATE& state = getUxState( this );
    state.geoms.clear();
    state.totalWidth    = 0;
    state.scrollX       = 0;
    state.hoverIndex    = -1;
    state.selectedIndex = -1;
    state.hoverNavBtn   = 0;
    state.isDragging    = false;

    UnsetToolTip();
    Refresh();
}


void EDA_MSG_PANEL::erase( wxDC* aDC )
{
    wxSize   size  = GetClientSize();
    wxColour color = wxSystemSettings::GetColour( wxSYS_COLOUR_BTNFACE );

    aDC->SetPen( wxPen( color ) );
    aDC->SetBrush( wxBrush( color, wxBRUSHSTYLE_SOLID ) );
    aDC->DrawRectangle( 0, 0, size.x, size.y );
}


std::optional<wxString> GetMsgPanelDisplayUuid( const KIID& aKiid )
{
    const static int        showUuids = ADVANCED_CFG::GetCfg().m_MsgPanelShowUuids;
    std::optional<wxString> uuid;

    if( showUuids > 0 )
    {
        uuid = aKiid.AsString();

        if( showUuids == 2 )
            uuid = uuid->SubString( 0, 7 );
    }

    return uuid;
}