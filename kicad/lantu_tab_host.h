// ==================== kicad/lantu_tab_host.h ====================
#ifndef LANTU_TAB_HOST_H
#define LANTU_TAB_HOST_H

#include <wx/wx.h>
#include <vector>

struct LANTU_TAB_ITEM
{
    wxString  id;           // 唯一标识，如 "HOME", "SCH", "PCB", "3D"
    wxString  title;        // 标签显示名，如 "开始页", "原理图", "PCB1", "3D 视图"
    wxWindow* window;       // 指向 wxPanel (开始页) 或被嵌入的 wxFrame (原理图/PCB/3D)
    bool      isFrame;      // true = Win32 嵌入的顶层 wxFrame; false = 普通 wxPanel
    bool      closable;     // 是否显示关闭按钮 × (开始页为 false)
    wxRect    tabRect;      // 标签热区
    wxRect    closeRect;    // 关闭按钮热区
};

class LANTU_TAB_HOST : public wxPanel
{
public:
    explicit LANTU_TAB_HOST( wxWindow* aParent );
    ~LANTU_TAB_HOST() override;

    // 添加普通面板标签（如“开始页”）
    void AddPanelTab( const wxString& aId, const wxString& aTitle, wxWindow* aPanel, bool aClosable = false );

    // 🌟 核心方法：将 KiCad 的独立 wxFrame (原理图/PCB/3D) 无损嵌入为标签页
    void EmbedFrameTab( const wxString& aId, const wxString& aTitle, wxFrame* aFrame, bool aClosable = true );

    // 切换到指定标签页
    void SelectTab( const wxString& aId );
    void SelectTabByIndex( int aIndex );

    // 移除标签页（当用户点击 × 或子窗口自身关闭时调用）
    void CloseTabByIndex( int aIndex );
    void DetachWindow( wxWindow* aWindow );

    // 主窗口退出前，优雅关闭所有已嵌入的编辑器（触发 KiCad 原生未保存提醒）
    bool CloseAllEmbeddedFrames();

    wxPanel* GetContentContainer() const { return m_contentHost; }

private:
    static constexpr int TAB_BAR_HEIGHT = 36;

    wxPanel*                    m_tabBar;       // 顶部自定义现代标签栏
    wxPanel*                    m_contentHost;  // 下方子窗口宿主容器
    std::vector<LANTU_TAB_ITEM> m_tabs;
    int                         m_activeIndex = -1;
    int                         m_hoverIndex  = -1;

    void ResizeActiveWindow();
    void ApplyWin32ChildStyle( wxFrame* aFrame );

    // 事件响应
    void OnTabBarPaint( wxPaintEvent& aEvent );
    void OnTabBarMouseDown( wxMouseEvent& aEvent );
    void OnTabBarMouseMove( wxMouseEvent& aEvent );
    void OnTabBarMouseLeave( wxMouseEvent& aEvent );
    void OnContentResize( wxSizeEvent& aEvent );
};

#endif // LANTU_TAB_HOST_H