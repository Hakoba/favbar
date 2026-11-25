namespace
{
enum DialogId
{
    InputField = 20,
    BodyText,
    Heading,
    PrimaryAction = 32,
    Hours,
    Minutes,
    TargetTime,
    TimerStatus,
    StopTimer,
    FirstPreset = 60
};
constexpr int popupWidth = 408;
constexpr DWORD popupStyle = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
void styleCaption(HWND h)
{
    BOOL useDark = dark;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &bg, sizeof(bg));
    DwmSetWindowAttribute(h, DWMWA_TEXT_COLOR, &muted, sizeof(muted));
}
HWND popupWindow(const wchar_t *cls, WNDPROC proc, const wchar_t *title, int height, void *data = nullptr,
                 int width = popupWidth, DWORD style = popupStyle)
{
    auto inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.lpfnWndProc = proc;
    wc.lpszClassName = cls;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT owner{};
    GetWindowRect(window, &owner);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT outer{0, 0, dpi(width), dpi(height)};
    AdjustWindowRectExForDpi(&outer, style, FALSE, WS_EX_DLGMODALFRAME, GetDpiForWindow(window));
    int w = outer.right - outer.left, h = outer.bottom - outer.top;
    const auto &work = mi.rcWork;
    int x = edge == ABE_LEFT ? owner.right + dpi(12) : owner.left - w - dpi(12);
    x = std::clamp(x, (int)work.left, std::max((int)work.left, (int)work.right - w));
    int y = work.top + std::max(0, (int)(work.bottom - work.top - h) / 2);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title, style, x, y, w, h, window, nullptr, inst, data);
    if (dlg)
        styleCaption(dlg);
    return dlg;
}
HWND control(HWND h, const wchar_t *cls, const wchar_t *label, int id, int x, int y, int w, int height, DWORD style,
             HFONT face)
{
    auto c = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, dpi(x), dpi(y), dpi(w), dpi(height), h,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)face, FALSE);
    return c;
}
LRESULT CALLBACK buttonHover(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_MOUSEMOVE && !GetPropW(h, L"FavBarHot"))
    {
        SetPropW(h, L"FavBarHot", (HANDLE)1);
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        InvalidateRect(h, nullptr, FALSE);
    }
    if (msg == WM_MOUSELEAVE)
    {
        RemovePropW(h, L"FavBarHot");
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefSubclassProc(h, msg, wp, lp);
}
HWND popupButton(HWND h, const wchar_t *label, int id, int x, int y, int width, int height = 34)
{
    HWND c = control(h, L"BUTTON", label, id, x, y, width, height, BS_OWNERDRAW | WS_TABSTOP, font);
    SetWindowSubclass(c, buttonHover, 1, 0);
    return c;
}
void popupTitle(HWND h, const wchar_t *title)
{
    control(h, L"STATIC", title, Heading, 58, 16, popupWidth - 78, 32, SS_CENTERIMAGE | SS_NOPREFIX, brandFont);
}
bool popupPaint(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT &out)
{
    out = 0;
    switch (msg)
    {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        fill(dc, ps.rcPaint, bg);
        brandMark(dc, dpi(20), dpi(32));
        RECT r{};
        GetClientRect(h, &r);
        fill(dc, {dpi(20), dpi(58), r.right - dpi(20), dpi(58) + 1}, border);
        for (int id : {InputField, Hours, Minutes})
        {
            if (auto edit = GetDlgItem(h, id))
            {
                RECT frame{};
                GetWindowRect(edit, &frame);
                MapWindowPoints(nullptr, h, (POINT *)&frame, 2);
                InflateRect(&frame, dpi(5), dpi(5));
                box(dc, frame, surface, border, 5);
            }
        }
        EndPaint(h, &ps);
        return true;
    }
    case WM_ERASEBKGND:
        out = 1;
        return true;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        COLORREF face = msg == WM_CTLCOLOREDIT ? surface : bg;
        COLORREF ink = id == TimerStatus && GetPropW(h, L"FavBarError") ? (dark ? RGB(247, 164, 146) : RGB(148, 56, 43))
                       : id == TargetTime                               ? mint
                       : id == 0                                        ? muted
                                                                        : fg;
        SetBkColor(dc, face);
        SetTextColor(dc, ink);
        SetDCBrushColor(dc, face);
        out = (LRESULT)GetStockObject(DC_BRUSH);
        return true;
    }
    case WM_DRAWITEM: {
        auto &item = *(DRAWITEMSTRUCT *)lp;
        if (item.CtlType != ODT_BUTTON)
            return false;
        RECT r = item.rcItem;
        HDC dc = item.hDC;
        int saved = SaveDC(dc);
        bool disabled = (item.itemState & ODS_DISABLED) != 0;
        bool primary = item.CtlID == PrimaryAction || item.CtlID == IDOK;
        bool selected = GetPropW(item.hwndItem, L"FavBarSelected") != nullptr;
        bool hot = GetPropW(item.hwndItem, L"FavBarHot") || (item.itemState & ODS_SELECTED);
        COLORREF primaryFill = dark ? RGB(200, 220, 198) : RGB(51, 82, 62);
        COLORREF face = disabled   ? bg
                        : primary  ? (hot ? mint : primaryFill)
                        : selected ? goldWash
                        : hot      ? hoverFill
                                   : surface;
        COLORREF ink = disabled   ? muted
                       : primary  ? (dark ? RGB(25, 38, 28) : RGB(255, 255, 251))
                       : selected ? accent
                                  : fg;
        fill(dc, r, bg);
        box(dc, r, face, primary && !disabled ? face : selected ? accent : border, 6);
        wchar_t label[160]{};
        GetWindowTextW(item.hwndItem, label, 160);
        textAt(dc, r, label, ink, font, DT_CENTER);
        if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
        {
            InflateRect(&r, -dpi(4), -dpi(4));
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCPenColor(dc, primary ? ink : mint);
            RoundRect(dc, r.left, r.top, r.right, r.bottom, dpi(6), dpi(6));
        }
        RestoreDC(dc, saved);
        out = TRUE;
        return true;
    }
    default:
        return false;
    }
}
struct Prompt
{
    bool accepted = false;
    wstring value;
};
LRESULT CALLBACK promptProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    LRESULT out;
    if (popupPaint(h, msg, wp, lp, out))
        return out;
    if (msg == WM_CLOSE || (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)))
    {
        auto &p = *(Prompt *)GetWindowLongPtrW(h, GWLP_USERDATA);
        p.accepted = msg == WM_COMMAND && LOWORD(wp) == IDOK;
        if (p.accepted)
            if (auto edit = GetDlgItem(h, InputField))
            {
                p.value.resize(GetWindowTextLengthW(edit) + 1);
                p.value.resize(GetWindowTextW(edit, p.value.data(), (int)p.value.size()));
            }
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
bool prompt(const wchar_t *title, const wchar_t *body, const wchar_t *initial, bool ask, Prompt &answer)
{
    int contentHeight = 34;
    if (!initial)
    {
        HDC dc = GetDC(window);
        auto old = SelectObject(dc, font);
        RECT r{0, 0, dpi(popupWidth - 58), 0};
        DrawTextW(dc, body, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(window, dc);
        contentHeight = std::clamp(MulDiv(r.bottom, 96, GetDpiForWindow(window)) + 12, 34, 260);
    }
    HWND h = popupWindow(L"FavBarPrompt", promptProc, L"FavBar", 146 + contentHeight, &answer);
    if (!h)
        return false;
    popupTitle(h, title);
    HWND edit{};
    if (initial)
    {
        edit = control(h, L"EDIT", initial, InputField, 25, 81, popupWidth - 50, 20, WS_TABSTOP | ES_AUTOHSCROLL, font);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(dpi(8), dpi(8)));
    }
    else if (contentHeight == 260)
    {
        wstring text;
        for (auto c = body; *c; ++c)
        {
            if (*c == L'\n' && (c == body || c[-1] != L'\r'))
                text += L'\r';
            text += *c;
        }
        control(h, L"EDIT", text.c_str(), BodyText, 20, 76, popupWidth - 40, contentHeight,
                ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, font);
    }
    else
        control(h, L"STATIC", body, BodyText, 20, 76, popupWidth - 40, contentHeight, SS_NOPREFIX, font);
    HWND cancel{};
    if (initial || ask)
        cancel = popupButton(h, L"Отмена", IDCANCEL, popupWidth - 226, 90 + contentHeight, 94);
    HWND ok = popupButton(h,
                          initial ? L"Сохранить"
                          : ask   ? L"Да"
                                  : L"Закрыть",
                          IDOK, popupWidth - 124, 90 + contentHeight, 104);
    EnableWindow(window, FALSE);
    ShowWindow(h, SW_SHOW);
    SetFocus(edit ? edit : ask ? cancel : ok);
    if (edit)
        SendMessageW(edit, EM_SETSEL, 0, -1);
    MSG m{};
    while (IsWindow(h) && GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (m.message == WM_KEYDOWN && (m.wParam == VK_ESCAPE || m.wParam == VK_RETURN) &&
            (m.hwnd == h || IsChild(h, m.hwnd)))
        {
            SendMessageW(h, WM_COMMAND, m.wParam == VK_ESCAPE || (cancel && GetFocus() == cancel) ? IDCANCEL : IDOK, 0);
            continue;
        }
        if (!IsDialogMessageW(h, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(window, TRUE);
    SetForegroundWindow(window);
    return answer.accepted;
}
} // namespace
extern "C" const wchar_t *ui_input(const wchar_t *title, const wchar_t *initial)
{
    Prompt p;
    prompt(title, nullptr, initial, false, p);
    result = std::move(p.value);
    return result.c_str();
}
extern "C" int ui_msg(const wchar_t *s, int ask)
{
    if (!window)
        return MessageBoxW(nullptr, s, L"FavBar", ask ? MB_YESNO | MB_ICONQUESTION : MB_OK) == IDYES;
    Prompt p;
    wstring heading = ask ? L"Подтверждение" : L"Сообщение";
    wstring body = s;
    auto first = body.find(L"\n\n");
    if (!ask && first != wstring::npos && first <= 26)
    {
        heading = body.substr(0, first);
        if (heading == L"FavBar")
            heading = L"Справка";
        body.erase(0, first + 2);
    }
    return prompt(heading.c_str(), body.c_str(), nullptr, ask != 0, p);
}
