// A modal edit box uses the calling window's thread and message queue.
namespace
{
LRESULT CALLBACK input_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))
    {
        if (LOWORD(wp) == IDOK)
        {
            auto edit = GetDlgItem(h, 20);
            int n = GetWindowTextLengthW(edit);
            result.resize(n + 1);
            GetWindowTextW(edit, result.data(), n + 1);
            result.resize(n);
        }
        DestroyWindow(h);
        return 0;
    }
    if (msg == WM_CLOSE)
    {
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace
extern "C" const wchar_t *ui_input(const wchar_t *title, const wchar_t *initial)
{
    auto inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = input_proc;
    wc.lpszClassName = L"FavBarEdit";
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    POINT p;
    GetCursorPos(&p);
    MONITORINFO m{sizeof(m)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), &m);
    int w = dpi(340), height = dpi(118);
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, wc.lpszClassName, title, WS_CAPTION | WS_SYSMENU,
        std::clamp((int)p.x, (int)m.rcWork.left, std::max((int)m.rcWork.left, (int)m.rcWork.right - w)),
        std::clamp((int)p.y, (int)m.rcWork.top, std::max((int)m.rcWork.top, (int)m.rcWork.bottom - height)), w, height,
        window, nullptr, inst, nullptr);
    result.clear();
    if (!dlg)
        return result.c_str();
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                dpi(10), dpi(8), dpi(308), dpi(23), dlg, (HMENU)20, inst, nullptr);
    HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, dpi(230), dpi(40),
                            dpi(88), dpi(23), dlg, (HMENU)IDOK, inst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)font, 1);
    SendMessageW(edit, WM_SETFONT, (WPARAM)font, 1);
    EnableWindow(window, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    MSG mmsg{};
    while (IsWindow(dlg) && GetMessageW(&mmsg, nullptr, 0, 0) > 0)
    {
        if (mmsg.message == WM_KEYDOWN && (mmsg.wParam == VK_ESCAPE || mmsg.wParam == VK_RETURN))
        {
            SendMessageW(dlg, WM_COMMAND, mmsg.wParam == VK_ESCAPE ? IDCANCEL : IDOK, 0);
            continue;
        }
        if (!IsDialogMessageW(dlg, &mmsg))
        {
            TranslateMessage(&mmsg);
            DispatchMessageW(&mmsg);
        }
    }
    EnableWindow(window, TRUE);
    SetForegroundWindow(window);
    return result.c_str();
}
