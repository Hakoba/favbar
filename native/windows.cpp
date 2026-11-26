#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <cwchar>
#include <deque>
#include <dwmapi.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using std::wstring;

namespace
{
struct Row
{
    wstring left, right, hint, path;
    int style, depth, leftDetail, rightDetail;
};
struct Event
{
    int kind, row, side;
    wstring data;
};
std::vector<Row> rows;
std::vector<Row> nextRows;
std::deque<Event> events;
HWND window;
HWND timerDialog;
HWND strataDialog;
HWND folderSearch, queueDialog, filePreview;
bool previewSpaceDown = false;
wstring heldPreviewPath;
constexpr UINT_PTR previewHoldTimer = 2;
void endPreviewHold()
{
    if (previewSpaceDown)
        KillTimer(window, previewHoldTimer);
    previewSpaceDown = false;
    heldPreviewPath.clear();
    if (filePreview)
        DestroyWindow(filePreview);
}
bool toolMessage(MSG &m);
void refreshTimerTheme();
HWND tipWindow;
wstring tipText;
TOOLINFOW tipInfo{sizeof(TOOLINFOW)};
HFONT font, bold;
bool registered = false, visible = false, dark = false, preview = false, dragging = false, resizing = false;
int width = 260, edge = ABE_LEFT, top = 0, focus = -1, hover = -1, rowHeight = 23, startRow = -1;
POINT mon{}, press{};
COLORREF bg, fg, accent, surface;
UINT taskbarCreated;
wstring result, status;
void event(int kind, int row = -1, int side = 0, wstring data = {})
{
    events.push_back({kind, row, side, std::move(data)});
}
void colors();
int paletteMode = -1;
void palette(int mode)
{
    if (mode == paletteMode)
        return;
    paletteMode = mode;
    DWORD light = 1, bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
    dark = mode == 2 || (mode == 0 && !light);
    colors();
}
int dpi(int px)
{
    return MulDiv(px, window ? GetDpiForWindow(window) : 96, 96);
}
APPBARDATA bar()
{
    APPBARDATA b{sizeof(b)};
    b.hWnd = window;
    b.uCallbackMessage = WM_APP + 1;
    return b;
}
void dock()
{
    if (!visible)
        return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(mon, MONITOR_DEFAULTTOPRIMARY), &mi);
    APPBARDATA b = bar();
    b.uEdge = edge;
    b.rc = mi.rcMonitor;
    b.rc.top = mi.rcWork.top;
    b.rc.bottom = mi.rcWork.bottom;
    auto fit = [&]() {
        if (edge == ABE_LEFT)
            b.rc.right = b.rc.left + width;
        else
            b.rc.left = b.rc.right - width;
    };
    fit();
    if (!preview)
    {
        SHAppBarMessage(ABM_QUERYPOS, &b);
        fit();
        SHAppBarMessage(ABM_SETPOS, &b);
    }
    SetWindowPos(window, HWND_TOPMOST, b.rc.left, b.rc.top, b.rc.right - b.rc.left, b.rc.bottom - b.rc.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void display(bool show)
{
    if (show == visible)
        return;
    visible = show;
    APPBARDATA b = bar();
    if (show)
    {
        if (!preview)
            registered = SHAppBarMessage(ABM_NEW, &b) != 0;
        dock();
    }
    else
    {
        endPreviewHold();
        if (registered)
            SHAppBarMessage(ABM_REMOVE, &b);
        registered = false;
        ShowWindow(window, SW_HIDE);
    }
}
int clientWidth()
{
    RECT r{};
    GetClientRect(window, &r);
    return r.right;
}
#include "appearance.inc"
void dragFile(const wstring &path)
{
    ComPtr<IShellItem> item;
    ComPtr<IDataObject> data;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))))
    {
        DWORD effect = 0;
        SHDoDragDrop(window, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    }
}
class Target final : public IDropTarget
{
    ULONG refs = 1;
    CLIPFORMAT wideUrl = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocatorW");
    CLIPFORMAT ansiUrl = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocator");
    FORMATETC fmt(CLIPFORMAT f)
    {
        return {f, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    }
    DWORD effect(IDataObject *data, POINTL pt)
    {
        POINT p{pt.x, pt.y};
        ScreenToClient(window, &p);
        int i = hit(p.y);
        if (i < 0)
            return DROPEFFECT_NONE;
        int style = rows[i].style;
        if (style == 3 || style == 4)
        {
            auto f = fmt(CF_HDROP);
            return data->QueryGetData(&f) == S_OK ? DROPEFFECT_COPY : 0;
        }
        if (style == 5)
            for (auto format : {wideUrl, (CLIPFORMAT)CF_UNICODETEXT, ansiUrl, (CLIPFORMAT)CF_TEXT})
            {
                auto f = fmt(format);
                if (data->QueryGetData(&f) == S_OK)
                    return DROPEFFECT_COPY;
            }
        return DROPEFFECT_NONE;
    }
    ComPtr<IDataObject> current;
    DWORD negotiate(IDataObject *data, POINTL p, DWORD offered)
    {
        if (!data || !effect(data, p))
            return DROPEFFECT_NONE;
        if (offered & DROPEFFECT_COPY)
            return DROPEFFECT_COPY;
        return offered & DROPEFFECT_LINK;
    }

  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDropTarget)
            return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        auto count = --refs;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *d, DWORD, POINTL p, DWORD *e) override
    {
        current = d;
        *e = negotiate(d, p, *e);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL p, DWORD *e) override
    {
        *e = negotiate(current.Get(), p, *e);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        current.Reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *d, DWORD, POINTL p, DWORD *e) override
    {
        *e = negotiate(d, p, *e);
        current.Reset();
        if (!*e)
            return S_OK;
        POINT point{p.x, p.y};
        ScreenToClient(window, &point);
        auto i = hit(point.y);
        if (i < 0)
            return S_OK;
        if (rows[i].style == 5)
        {
            for (auto cf : {wideUrl, (CLIPFORMAT)CF_UNICODETEXT, ansiUrl, (CLIPFORMAT)CF_TEXT})
            {
                auto f = fmt(cf);
                STGMEDIUM m{};
                if (FAILED(d->GetData(&f, &m)))
                    continue;
                auto s = (wchar_t *)GlobalLock(m.hGlobal);
                if (s)
                {
                    auto bytes = GlobalSize(m.hGlobal);
                    if (cf == wideUrl || cf == CF_UNICODETEXT)
                    {
                        size_t len = wcsnlen_s(s, std::min<size_t>(8193, bytes / 2));
                        if (len <= 8192)
                            event(8, i, 0, wstring(s, len));
                    }
                    else
                    {
                        auto text = reinterpret_cast<const char *>(s);
                        int len = (int)strnlen_s(text, std::min<size_t>(bytes, 8193));
                        if (len > 0 && len <= 8192)
                        {
                            int count = MultiByteToWideChar(CP_ACP, 0, text, len, nullptr, 0);
                            wstring value(count, 0);
                            if (count && MultiByteToWideChar(CP_ACP, 0, text, len, value.data(), count))
                                event(8, i, 0, value);
                        }
                    }
                    GlobalUnlock(m.hGlobal);
                }
                ReleaseStgMedium(&m);
                break;
            }
        }
        else
        {
            auto f = fmt(CF_HDROP);
            STGMEDIUM m{};
            if (SUCCEEDED(d->GetData(&f, &m)))
            {
                auto drop = (HDROP)m.hGlobal;
                UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
                for (UINT n = 0; n < count; ++n)
                {
                    auto len = DragQueryFileW(drop, n, nullptr, 0);
                    wstring s(len + 1, 0);
                    DragQueryFileW(drop, n, s.data(), len + 1);
                    s.resize(len);
                    event(7, -1, 0, s);
                }
                ReleaseStgMedium(&m);
            }
        }
        return S_OK;
    }
};
struct MenuItem
{
    const wchar_t *label;
    int glyph, groupStart, destructive;
};
struct MenuEntry
{
    MenuItem item;
    int top, bottom, gutter;
};
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        paint();
        return 0;
    case WM_MEASUREITEM: {
        auto item = reinterpret_cast<MEASUREITEMSTRUCT *>(lp);
        if (item->CtlType != ODT_MENU || !item->itemData)
            break;
        HDC dc = GetDC(h);
        auto prev = SelectObject(dc, font);
        const auto &entry = *reinterpret_cast<const MenuEntry *>(item->itemData);
        auto title = entry.item.label;
        SIZE size{};
        GetTextExtentPoint32W(dc, title, (int)wcslen(title), &size);
        SelectObject(dc, prev);
        ReleaseDC(h, dc);
        item->itemWidth = std::clamp((int)size.cx + dpi(32 + entry.gutter), dpi(154), dpi(540));
        item->itemHeight = dpi(32 + entry.top + entry.bottom);
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto item = reinterpret_cast<DRAWITEMSTRUCT *>(lp);
        if (item->CtlType != ODT_MENU || !item->itemData)
            break;
        RECT rect = item->rcItem;
        const auto &entry = *reinterpret_cast<const MenuEntry *>(item->itemData);
        int saved = SaveDC(item->hDC);
        bool hot = (item->itemState & ODS_SELECTED) != 0;
        fill(item->hDC, rect, surface);
        if (entry.item.groupStart && entry.top == 9)
        {
            int y = rect.top + dpi(4);
            fill(item->hDC, {rect.left + dpi(12), y, rect.right - dpi(12), y + 1}, border);
        }
        rect.top += dpi(entry.top);
        rect.bottom -= dpi(entry.bottom);
        if (hot)
        {
            RECT highlight = rect;
            InflateRect(&highlight, -dpi(4), -dpi(1));
            box(item->hDC, highlight, hoverFill, hoverFill, 6);
        }
        COLORREF ink = entry.item.destructive ? errorColor : fg;
        if (entry.item.glyph)
            icon(item->hDC, {rect.left + dpi(11), rect.top, rect.left + dpi(31), rect.bottom}, entry.item.glyph,
                 entry.item.destructive ? ink
                 : hot                  ? accent
                                        : muted);
        rect.left += dpi(13 + entry.gutter);
        rect.right -= dpi(12);
        textAt(item->hDC, rect, entry.item.label, ink, font);
        RestoreDC(item->hDC, saved);
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        event(0);
        return 0;
    case WM_HOTKEY:
    case WM_APP + 3:
        display(!visible);
        event(6, -1, visible);
        return 0;
    case WM_APP + 1:
        if (wp == ABN_POSCHANGED)
            PostMessageW(h, WM_APP + 2, 0, 0);
        if (wp == ABN_FULLSCREENAPP)
            SetWindowPos(h, lp ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    case WM_APP + 2:
        dock();
        return 0;
    case WM_APP + 4:
        event(9);
        return 0;
    case WM_TIMER:
        if (wp == previewHoldTimer)
        {
            KillTimer(h, previewHoldTimer);
            if (previewSpaceDown && !heldPreviewPath.empty())
                event(23, -1, 0, std::move(heldPreviewPath));
            return 0;
        }
        event(5);
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_SETTINGCHANGE:
        paletteMode = -1;
        [[fallthrough]];
    case WM_DISPLAYCHANGE:
        if (window)
        {
            PostMessageW(h, WM_APP + 2, 0, 0);
            event(10);
        }
        return 0;
    case WM_SIZE:
        if (window)
            scroll(top);
        return 0;
    case WM_MOUSEWHEEL:
        scroll(top - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * dpi(66));
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_TRACKPOS};
        GetScrollInfo(h, SB_VERT, &si);
        switch (LOWORD(wp))
        {
        case SB_LINEUP:
            scroll(top - dpi(25));
            break;
        case SB_LINEDOWN:
            scroll(top + dpi(25));
            break;
        case SB_PAGEUP:
            scroll(top - Layout().page);
            break;
        case SB_PAGEDOWN:
            scroll(top + Layout().page);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            scroll(si.nTrackPos);
            break;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        setHover(-1, 0);
        if (tipWindow)
            SendMessageW(tipWindow, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tipInfo);
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i = hit(y);
        if (scrollbarGrab >= 0)
        {
            Layout layout;
            ScrollThumb thumb(layout);
            scroll(MulDiv(y - thumb.track - scrollbarGrab, thumb.range, std::max(1, thumb.length - thumb.height)));
            return 0;
        }
        if (resizing)
        {
            POINT p;
            GetCursorPos(&p);
            RECT r;
            GetWindowRect(h, &r);
            width = std::clamp((int)(edge == ABE_LEFT ? p.x - r.left : r.right - p.x), 160, 900);
            dock();
            return 0;
        }
        if (startRow >= 0 && startRow < (int)rows.size() && (wp & MK_LBUTTON) && rows[startRow].style == 4 &&
            (abs(x - press.x) > 4 || abs(y - press.y) > 4))
        {
            wstring path = rows[startRow].path;
            startRow = -1;
            dragging = true;
            ReleaseCapture();
            dragFile(path);
            return 0;
        }
        int cell = i < 0 ? 0 : side(i, x, y);
        if (hover != i || cell != hoverSide)
        {
            if (tipWindow)
                SendMessageW(tipWindow, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tipInfo);
            if (i >= 0 && rows[i].style == Meter && rows[i].left.find(L"GPU") == 0)
            {
                TRACKMOUSEEVENT request{sizeof(request), TME_HOVER, h, 450};
                TrackMouseEvent(&request);
            }
            bool enter = hover < 0;
            setHover(i, cell);
            if (enter)
            {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                event(11);
            }
        }
        return 0;
    }
    case WM_MOUSEHOVER:
        if (hover >= 0 && hover < (int)rows.size() && rows[hover].style == Meter && rows[hover].left.find(L"GPU") == 0)
            event(13, hover);
        return 0;
    case WM_DPICHANGED:
        if (window)
        {
            fonts(true);
            PostMessageW(h, WM_APP + 2, 0, 0);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            bool grip = edge == ABE_LEFT ? p.x >= clientWidth() - dpi(4) : p.x < dpi(4);
            SetCursor(LoadCursorW(nullptr, grip ? IDC_SIZEWE : hit(p.y) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        press = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Layout layout;
        ScrollThumb thumb(layout);
        if (thumb.contains(press.x, press.y))
        {
            startRow = -1;
            SetCapture(h);
            if (press.y >= thumb.y && press.y < thumb.y + thumb.height)
                scrollbarGrab = press.y - thumb.y;
            else
            {
                scrollbarGrab = thumb.height / 2;
                scroll(MulDiv(press.y - thumb.track - scrollbarGrab, thumb.range,
                              std::max(1, thumb.length - thumb.height)));
            }
            return 0;
        }
        SetFocus(h);
        resizing = edge == ABE_LEFT ? press.x >= clientWidth() - dpi(4) : press.x < dpi(4);
        startRow = hit(press.y);
        dragging = false;
        SetCapture(h);
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (scrollbarGrab >= 0)
        {
            scrollbarGrab = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (resizing)
        {
            resizing = false;
            event(12, -1, width);
            return 0;
        }
        int x = GET_X_LPARAM(lp), i = hit(GET_Y_LPARAM(lp));
        if (i >= 0 && i == startRow && !dragging)
        {
            setFocusRow(i);
            event(1, i, side(i, x, GET_Y_LPARAM(lp)));
        }
        startRow = -1;
        return 0;
    }
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(lp), i = hit(GET_Y_LPARAM(lp));
        setFocusRow(i);
        event(msg == WM_RBUTTONUP ? 2 : 3, i, i >= 0 ? side(i, x, GET_Y_LPARAM(lp)) : 0);
        return 0;
    }
    case WM_ACTIVATEAPP:
        if (!wp)
            endPreviewHold();
        break;
    case WM_KILLFOCUS:
        if (!filePreview || ((HWND)wp != filePreview && !IsChild(filePreview, (HWND)wp)))
            endPreviewHold();
        break;
    case WM_KEYUP:
        if (wp == VK_SPACE)
        {
            endPreviewHold();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == 'K' && GetKeyState(VK_CONTROL) < 0)
        {
            event(17);
            return 0;
        }
        if (wp == VK_SPACE)
        {
            if (previewSpaceDown || (lp & (1L << 30)))
                return 0;
            int i = focus;
            if ((i < 0 || i >= (int)rows.size() || rows[i].style != ShelfFile) && hover >= 0 &&
                hover < (int)rows.size() && rows[hover].style == ShelfFile)
                i = hover;
            if (i >= 0 && i < (int)rows.size() && rows[i].style == ShelfFile)
            {
                previewSpaceDown = true;
                heldPreviewPath = rows[i].path;
                if (!SetTimer(h, previewHoldTimer, 150, nullptr))
                    event(23, -1, 0, std::move(heldPreviewPath));
            }
            return 0;
        }
        if (wp == VK_ESCAPE && previewSpaceDown)
        {
            endPreviewHold();
            return 0;
        }
        if (wp == VK_PRIOR || wp == VK_NEXT)
        {
            scroll(top + (wp == VK_NEXT ? Layout().page : -Layout().page));
            return 0;
        }
        if (wp == VK_DOWN || wp == VK_UP)
        {
            focus = std::clamp(focus + (wp == VK_DOWN ? 1 : -1), 0, std::max(0, (int)rows.size() - 1));
            ensureFocus();
        }
        else if (wp == VK_RETURN && focus >= 0)
            event(1, focus, GetKeyState(VK_SHIFT) < 0);
        else
            event(4, focus, (int)wp);
        return 0;
    }
    if (taskbarCreated && msg == taskbarCreated && visible && !preview)
    {
        auto b = bar();
        registered = SHAppBarMessage(ABM_NEW, &b) != 0;
        dock();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace
extern "C"
{
    int ui_start(int test, int w, int right, int mx, int my)
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        OleInitialize(nullptr);
        preview = test != 0;
        width = std::clamp(w, 160, 900);
        edge = right ? ABE_RIGHT : ABE_LEFT;
        mon = {mx, my};
        auto inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FavBarRustSidebar";
        RegisterClassW(&wc);
        window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"FavBar", WS_POPUP, 0, 0, width, 700, nullptr,
                                 nullptr, inst, nullptr);
        if (!window)
            return 0;
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        fonts(true);
        auto target = new Target();
        RegisterDragDrop(window, target);
        target->Release();
        palette(0);
        return 1;
    }
    void ui_show(int yes)
    {
        display(yes != 0);
    }
    int ui_key(unsigned mods, unsigned key)
    {
        UnregisterHotKey(window, 1);
        return key && RegisterHotKey(window, 1, mods | MOD_NOREPEAT, key);
    }
    void ui_timer(int ms)
    {
        KillTimer(window, 1);
        if (ms > 0)
            SetTimer(window, 1, ms, nullptr);
    }
    void ui_wake()
    {
        PostMessageW(window, WM_APP + 4, 0, 0);
    }
    void ui_clear()
    {
        nextRows.clear();
    }
    void ui_row(const wchar_t *l, const wchar_t *r, const wchar_t *hint, const wchar_t *p, int style, int depth,
                int leftDetail, int rightDetail)
    {
        nextRows.push_back({l, r, hint, p, style, depth, leftDetail, rightDetail});
    }
    void ui_finish(int theme)
    {
        auto oldBg = bg, oldAccent = accent, oldSelection = selectionFill;
        palette(theme);
        bool themeChanged = oldBg != bg || oldAccent != accent || oldSelection != selectionFill;
        if (themeChanged)
        {
            refreshTimerTheme();
            if (strataDialog)
                SendMessageW(strataDialog, WM_THEMECHANGED, 0, 0);
            for (HWND h : {folderSearch, queueDialog, filePreview})
                if (h)
                    SendMessageW(h, WM_THEMECHANGED, 0, 0);
        }
        auto same = [](const Row &a, const Row &b) {
            return a.left == b.left && a.right == b.right && a.hint == b.hint && a.path == b.path &&
                   a.style == b.style && a.depth == b.depth && a.leftDetail == b.leftDetail &&
                   a.rightDetail == b.rightDetail;
        };
        bool relayout = themeChanged || rows.size() != nextRows.size() ||
                        !std::equal(rows.begin(), rows.end(), nextRows.begin(),
                                    [](const Row &a, const Row &b) { return a.style == b.style; });
        if (!relayout && std::equal(rows.begin(), rows.end(), nextRows.begin(), same))
            return;
        wstring oldHint = footerText();
        rows.swap(nextRows);
        focus = std::min(focus, (int)rows.size() - 1);
        hover = std::min(hover, (int)rows.size() - 1);
        if (relayout)
            scroll(top);
        else
        {
            Layout layout;
            for (auto &band : layout.bands)
                if (!same(rows[band.index], nextRows[band.index]))
                    invalidateBand(layout, band.index);
            if (oldHint != footerText())
                invalidateFooter();
        }
    }
    void ui_status(const wchar_t *s)
    {
        if (status != s)
        {
            status = s;
            invalidateFooter();
        }
    }
    void ui_edge(int right)
    {
        edge = right ? ABE_RIGHT : ABE_LEFT;
        dock();
    }
    BOOL CALLBACK collectMon(HMONITOR, HDC, LPRECT r, LPARAM data)
    {
        ((std::vector<RECT> *)data)->push_back(*r);
        return TRUE;
    }
    void ui_monitor(int *x, int *y)
    {
        std::vector<RECT> rects;
        EnumDisplayMonitors(nullptr, nullptr, collectMon, (LPARAM)&rects);
        if (rects.empty())
            return;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(mon, MONITOR_DEFAULTTOPRIMARY), &mi);
        size_t i = 0;
        for (; i < rects.size(); ++i)
            if (EqualRect(&mi.rcMonitor, &rects[i]))
                break;
        auto r = rects[(i + 1) % rects.size()];
        mon = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        *x = mon.x;
        *y = mon.y;
        dock();
    }
    int ui_next(int *row, int *sideOut, const wchar_t **data)
    {
        MSG m{};
        while (events.empty())
        {
            if (GetMessageW(&m, nullptr, 0, 0) <= 0)
                return 0;
            if (previewSpaceDown && (m.message == WM_KEYUP || m.message == WM_SYSKEYUP) && m.wParam == VK_SPACE)
            {
                endPreviewHold();
                continue;
            }
            if (timerDialog && IsDialogMessageW(timerDialog, &m))
                continue;
            if (strataDialog && IsDialogMessageW(strataDialog, &m))
                continue;
            if (toolMessage(m))
                continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        auto e = std::move(events.front());
        events.pop_front();
        *row = e.row;
        *sideOut = e.side;
        result = std::move(e.data);
        *data = result.c_str();
        return e.kind;
    }
    void ui_close()
    {
        ui_timer(0);
        display(false);
        UnregisterHotKey(window, 1);
        RevokeDragDrop(window);
        if (tipWindow)
            DestroyWindow(tipWindow);
        DestroyWindow(window);
        paintBuffer.reset();
        fonts(false);
        OleUninitialize();
    }
    void ui_tip(const wchar_t *body)
    {
        POINT p;
        GetCursorPos(&p);
        if (WindowFromPoint(p) != window)
            return;
        POINT client = p;
        ScreenToClient(window, &client);
        auto i = hit(client.y);
        if (i < 0 || rows[i].style != Meter || rows[i].left.find(L"GPU") != 0)
            return;
        if (!tipWindow)
        {
            tipWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                                        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, window, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
            if (!tipWindow)
                return;
            SetWindowTheme(tipWindow, L"", L"");
            tipInfo.hwnd = window;
            tipInfo.uId = 1;
            tipInfo.uFlags = TTF_TRACK | TTF_ABSOLUTE;
            tipInfo.lpszText = const_cast<wchar_t *>(L"");
            SendMessageW(tipWindow, TTM_ADDTOOLW, 0, (LPARAM)&tipInfo);
            SendMessageW(tipWindow, TTM_SETMAXTIPWIDTH, 0, dpi(340));
        }
        RECT padding{dpi(10), dpi(7), dpi(10), dpi(7)};
        SendMessageW(tipWindow, TTM_SETMARGIN, 0, (LPARAM)&padding);
        SendMessageW(tipWindow, WM_SETFONT, (WPARAM)font, FALSE);
        SendMessageW(tipWindow, TTM_SETTIPBKCOLOR, surface, 0);
        SendMessageW(tipWindow, TTM_SETTIPTEXTCOLOR, fg, 0);
        tipText = body;
        tipInfo.lpszText = tipText.data();
        SendMessageW(tipWindow, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tipInfo);
        SendMessageW(tipWindow, TTM_TRACKACTIVATE, TRUE, (LPARAM)&tipInfo);
        RECT barRect{}, tipRect{};
        GetWindowRect(window, &barRect);
        GetWindowRect(tipWindow, &tipRect);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
        int tw = tipRect.right - tipRect.left, th = tipRect.bottom - tipRect.top;
        int x = edge == ABE_LEFT ? barRect.right + dpi(6) : barRect.left - tw - dpi(6);
        SendMessageW(
            tipWindow, TTM_TRACKPOSITION, 0,
            MAKELPARAM(
                std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - tw)),
                std::clamp((int)p.y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - th))));
    }
    int ui_menu(const MenuItem *items, int n)
    {
        if (n <= 0)
            return -1;
        auto m = CreatePopupMenu();
        auto brush = CreateSolidBrush(surface);
        MENUINFO appearance{sizeof(appearance)};
        appearance.fMask = MIM_BACKGROUND | MIM_STYLE;
        appearance.dwStyle = MNS_NOCHECK;
        appearance.hbrBack = brush;
        SetMenuInfo(m, &appearance);
        int gutter = std::any_of(items, items + n, [](const MenuItem &item) { return item.glyph != 0; }) ? 25 : 0;
        std::vector<MenuEntry> entries;
        entries.reserve(n);
        for (int i = 0; i < n; i++)
        {
            entries.push_back({items[i], !i ? 4 : items[i].groupStart ? 9 : 0, i == n - 1 ? 4 : 0, gutter});
            MENUITEMINFOW item{sizeof(item)};
            item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_ID | MIIM_DATA;
            item.fType = MFT_OWNERDRAW;
            item.wID = i + 1;
            item.dwTypeData = const_cast<wchar_t *>(items[i].label);
            item.dwItemData = (ULONG_PTR)&entries.back();
            InsertMenuItemW(m, i, TRUE, &item);
        }
        POINT p;
        GetCursorPos(&p);
        int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
        DestroyMenu(m);
        DeleteObject(brush);
        return id - 1;
    }
    int ui_open(const wchar_t *path, const wchar_t *cwd)
    {
        return (INT_PTR)ShellExecuteW(window, L"open", path, nullptr, *cwd ? cwd : nullptr, SW_SHOWNORMAL) > 32;
    }
    const wchar_t *pickFrom(int folder, HWND owner, const wchar_t *initial)
    {
        result.clear();
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return result.c_str();
        DWORD flags;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT));
        if (initial && *initial)
        {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial, nullptr, IID_PPV_ARGS(&item))))
                dialog->SetFolder(item.Get());
        }
        if (SUCCEEDED(dialog->Show(owner)))
        {
            ComPtr<IShellItemArray> items;
            if (SUCCEEDED(dialog->GetResults(&items)))
            {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; i++)
                {
                    ComPtr<IShellItem> it;
                    PWSTR p = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)))
                    {
                        if (!result.empty())
                            result += L"\n";
                        result += p;
                        CoTaskMemFree(p);
                    }
                }
            }
        }
        return result.c_str();
    }
    const wchar_t *ui_pick(int folder)
    {
        return pickFrom(folder, window, nullptr);
    }
    const wchar_t *ui_clip(const wchar_t *value, int write)
    {
        result.clear();
        if (!OpenClipboard(window))
            return result.c_str();
        if (write)
        {
            auto bytes = (wcslen(value) + 1) * 2;
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (mem)
            {
                auto p = GlobalLock(mem);
                if (p)
                {
                    memcpy(p, value, bytes);
                    GlobalUnlock(mem);
                    EmptyClipboard();
                    if (!SetClipboardData(CF_UNICODETEXT, mem))
                        GlobalFree(mem);
                }
                else
                    GlobalFree(mem);
            }
        }
        else
        {
            auto mem = GetClipboardData(CF_UNICODETEXT);
            if (mem)
            {
                auto p = (const wchar_t *)GlobalLock(mem);
                if (p)
                {
                    result.assign(p, wcsnlen_s(p, GlobalSize(mem) / 2));
                    GlobalUnlock(mem);
                }
            }
        }
        CloseClipboard();
        return result.c_str();
    }
    const wchar_t *ui_explorer(const wchar_t *dest)
    {
        result.clear();
        ComPtr<IShellWindows> wins;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wins))))
        {
            long count = 0;
            wins->get_Count(&count);
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
            {
                wchar_t cls[64]{};
                GetClassNameW(h, cls, 64);
                if (wcscmp(cls, L"CabinetWClass") || !IsWindowVisible(h))
                    continue;
                for (long i = 0; i < count; i++)
                {
                    VARIANT ix{};
                    ix.vt = VT_I4;
                    ix.lVal = i;
                    ComPtr<IDispatch> d;
                    ComPtr<IWebBrowser2> b;
                    SHANDLE_PTR handle = 0;
                    if (FAILED(wins->Item(ix, &d)) || !d || FAILED(d.As(&b)) || FAILED(b->get_HWND(&handle)) ||
                        (HWND)handle != h)
                        continue;
                    if (*dest)
                    {
                        VARIANT path{};
                        path.vt = VT_BSTR;
                        path.bstrVal = SysAllocString(dest);
                        HRESULT hr = b->Navigate2(&path, nullptr, nullptr, nullptr, nullptr);
                        VariantClear(&path);
                        if (SUCCEEDED(hr))
                        {
                            ShowWindow(h, SW_RESTORE);
                            SetForegroundWindow(h);
                            result = dest;
                            return result.c_str();
                        }
                    }
                    else
                    {
                        BSTR url = nullptr;
                        if (SUCCEEDED(b->get_LocationURL(&url)) && url)
                        {
                            wchar_t buf[32768];
                            DWORD len = 32768;
                            if (SUCCEEDED(PathCreateFromUrlW(url, buf, &len, 0)))
                                result = buf;
                            SysFreeString(url);
                        }
                        return result.c_str();
                    }
                }
            }
        }
        if (*dest && ui_open(dest, L""))
            result = dest;
        return result.c_str();
    }
    int ui_single(int test)
    {
        if (test)
            return 1;
        static HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\FavBarRustSingle");
        if (!mutex)
            return 0;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return 1;
        if (HWND h = FindWindowW(L"FavBarRustSidebar", nullptr))
            PostMessageW(h, WM_APP + 3, 0, 0);
        return 0;
    }
}
#include "dialogs.inc"
#include "strata.inc"
#include "shutdown.inc"
#include "icons.inc"
#include "tool_windows.inc"
#include "preview.inc"
#include "metrics.inc"
