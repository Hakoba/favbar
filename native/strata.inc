namespace
{
struct StrataFields
{
    int inkling, vector;
    unsigned context;
    int resume, web, skills;
};
enum StrataId
{
    Qwen = 100,
    Inkling,
    VectorOff,
    VectorOn,
    ResumeChat,
    WebTools,
    Skills,
    BrowseProject,
    RefreshServer,
    ServerBadge,
    SaveStrata,
    StartServer,
    FirstContext = 130
};
constexpr int contextSizes[] = {32, 64, 96, 128, 160, 192, 224, 256};
constexpr int settingsWidth = 504, settingsHeight = 478;
constexpr DWORD settingsStyle = popupStyle | WS_THICKFRAME;
bool strataReady = false;
struct ControlPosition
{
    HWND child;
    RECT rect;
};
std::vector<ControlPosition> strataPositions;
wstring fieldText(HWND h, int id)
{
    HWND edit = GetDlgItem(h, id);
    wstring value(GetWindowTextLengthW(edit) + 1, L'\0');
    value.resize(GetWindowTextW(edit, value.data(), (int)value.size()));
    return value;
}
bool isSelected(HWND h, int id)
{
    return GetPropW(GetDlgItem(h, id), L"FavBarSelected") != nullptr;
}
void selectButton(HWND h, int id, bool selected)
{
    HWND b = GetDlgItem(h, id);
    if (selected == isSelected(h, id))
        return;
    if (selected)
        SetPropW(b, L"FavBarSelected", (HANDLE)1);
    else
        RemovePropW(b, L"FavBarSelected");
    InvalidateRect(b, nullptr, FALSE);
}
void switchOption(HWND h, int id, bool selected)
{
    selectButton(h, id, selected);
    const wchar_t *name = id == ResumeChat ? L"Продолжить чат" : id == WebTools ? L"Веб-инструменты" : L"Навыки";
    SetDlgItemTextW(h, id, ((selected ? L"✓  " : L"") + wstring(name)).c_str());
}
void settingsReady(HWND h, bool ready)
{
    strataReady = ready;
    for (int id : {(int)SaveStrata, (int)StartServer, (int)PrimaryAction})
        EnableWindow(GetDlgItem(h, id), ready);
}
void applyModelChoice(HWND h, bool ink)
{
    selectButton(h, Qwen, !ink);
    selectButton(h, Inkling, ink);
    EnableWindow(GetDlgItem(h, VectorOff), !ink);
    EnableWindow(GetDlgItem(h, VectorOn), !ink);
}
LRESULT CALLBACK strataProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT out;
    if (popupPaint(h, msg, wp, lp, out))
        return out;
    switch (msg)
    {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDCANCEL)
        {
            DestroyWindow(h);
            return 0;
        }
        if (id == Hours && HIWORD(wp) == EN_CHANGE)
        {
            BOOL valid{};
            unsigned n = GetDlgItemInt(h, Hours, &valid, FALSE);
            for (int i = 0; i < 8; ++i)
                selectButton(h, FirstContext + i, valid && n == contextSizes[i] * 1024U);
            return 0;
        }
        if (HIWORD(wp) != BN_CLICKED || !lp)
            return 0;
        if (id == Qwen || id == Inkling)
        {
            if (strataReady && isSelected(h, id))
                return 0;
            applyModelChoice(h, id == Inkling);
            settingsReady(h, false);
            SetDlgItemTextW(h, TimerStatus, L"");
            SetDlgItemTextW(h, ServerBadge, L"Чтение настроек…");
            event(14, 0, id == Inkling);
        }
        else if (id == RefreshServer)
        {
            SetDlgItemTextW(h, TimerStatus, L"");
            event(14, strataReady, isSelected(h, Inkling));
        }
        else if (id == VectorOn || id == VectorOff)
        {
            selectButton(h, VectorOn, id == VectorOn);
            selectButton(h, VectorOff, id == VectorOff);
        }
        else if (id >= FirstContext && id < FirstContext + 8)
            SetDlgItemInt(h, Hours, contextSizes[id - FirstContext] * 1024U, FALSE);
        else if (id == ResumeChat || id == WebTools || id == Skills)
            switchOption(h, id, !isSelected(h, id));
        else if (id == BrowseProject)
        {
            auto folder = fieldText(h, InputField);
            auto chosen = pickFrom(1, h, folder.c_str());
            if (*chosen)
                SetDlgItemTextW(h, InputField, chosen);
        }
        else if (strataReady && (id == SaveStrata || id == StartServer || id == PrimaryAction))
            event(15, -1, id == SaveStrata ? 0 : id == StartServer ? 1 : 2);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        RECT r{0, 0, dpi(settingsWidth), dpi(settingsHeight)};
        AdjustWindowRectExForDpi(&r, settingsStyle, FALSE, WS_EX_DLGMODALFRAME, GetDpiForWindow(window));
        auto info = (MINMAXINFO *)lp;
        info->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        info->ptMaxTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && !strataPositions.empty())
        {
            int width = LOWORD(lp);
            for (const auto &c : strataPositions)
            {
                int x = MulDiv(c.rect.left, width, dpi(settingsWidth));
                int right = MulDiv(c.rect.right, width, dpi(settingsWidth));
                MoveWindow(c.child, x, c.rect.top, right - x, c.rect.bottom - c.rect.top, TRUE);
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_EXITSIZEMOVE: {
        RECT r{};
        GetWindowRect(h, &r);
        event(16, r.left, r.top);
        return 0;
    }
    case WM_THEMECHANGED:
        styleCaption(h);
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    case WM_DESTROY:
        strataDialog = nullptr;
        strataPositions.clear();
        strataReady = false;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace
extern "C" int ui_strata_activate()
{
    if (!strataDialog)
        return 0;
    if (IsIconic(strataDialog))
        ShowWindow(strataDialog, SW_RESTORE);
    SetForegroundWindow(strataDialog);
    return 1;
}
extern "C" int ui_strata_open(const StrataFields *fields, const wchar_t *folder, int x, int y)
{
    HWND h = popupWindow(L"FavBarStrataSettings", strataProc, L"FavBar · Strata и Qwen Code", settingsHeight, nullptr,
                         settingsWidth, settingsStyle);
    if (!h)
        return 0;
    strataDialog = h;
    if (x != INT_MIN && y != INT_MIN)
    {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({x, y}, MONITOR_DEFAULTTONEAREST), &mi);
        RECT r{};
        GetWindowRect(h, &r);
        auto &area = mi.rcWork;
        SetWindowPos(h, nullptr, std::clamp((LONG)x, area.left, std::max(area.left, area.right - (r.right - r.left))),
                     std::clamp((LONG)y, area.top, std::max(area.top, area.bottom - (r.bottom - r.top))), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    popupTitle(h, L"Strata и Qwen Code");
    auto label = [&](const wchar_t *s, int px, int py, int w, int id = 0) {
        return control(h, L"STATIC", s, id, px, py, w, 22, SS_CENTERIMAGE | SS_NOPREFIX, bold);
    };
    label(L"Модель", 20, 71, 96);
    control(h, L"STATIC", L"Чтение настроек…", ServerBadge, 161, 70, 226, 24, SS_RIGHT | SS_CENTERIMAGE, font);
    popupButton(h, L"Обновить", RefreshServer, 398, 68, 86, 27);
    popupButton(h, L"Qwen Flash-Next · 8080", Qwen, 20, 103, 228, 35);
    popupButton(h, L"Inkling SQ-10 SC117 · 8102", Inkling, 256, 103, 228, 35);
    label(L"Режим Strata", 20, 151, 118);
    popupButton(h, L"Без анценза", VectorOff, 148, 147, 164, 32);
    popupButton(h, L"С анцензом", VectorOn, 320, 147, 164, 32);
    label(L"Окно контекста", 20, 197, 190);
    control(h, L"EDIT", L"", Hours, 288, 200, 111, 18, WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER, font);
    control(h, L"STATIC", L"токенов", 0, 415, 197, 69, 22, SS_CENTERIMAGE, font);
    for (int i = 0; i < 8; ++i)
        popupButton(h, (std::to_wstring(contextSizes[i]) + L"K").c_str(), FirstContext + i, 20 + 59 * i, 231, 51, 29);
    label(L"Рабочая папка Qwen Code", 20, 274, 320);
    control(h, L"EDIT", folder, InputField, 26, 310, 349, 19, WS_TABSTOP | ES_AUTOHSCROLL, font);
    popupButton(h, L"Обзор…", BrowseProject, 392, 302, 92, 34);
    popupButton(h, L"", ResumeChat, 20, 350, 150, 32);
    popupButton(h, L"", WebTools, 177, 350, 165, 32);
    popupButton(h, L"", Skills, 349, 350, 135, 32);
    switchOption(h, ResumeChat, fields->resume);
    switchOption(h, WebTools, fields->web);
    switchOption(h, Skills, fields->skills);
    control(h, L"STATIC", L"", TimerStatus, 20, 389, 464, 36, SS_NOPREFIX, font);
    popupButton(h, L"Сохранить", SaveStrata, 20, 428, 109, 34);
    popupButton(h, L"Запустить Strata", StartServer, 140, 428, 148, 34);
    popupButton(h, L"Открыть Qwen Code", PrimaryAction, 299, 428, 185, 34);
    applyModelChoice(h, fields->inkling);
    settingsReady(h, false);
    for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        RECT r{};
        GetWindowRect(c, &r);
        MapWindowPoints(nullptr, h, (POINT *)&r, 2);
        strataPositions.push_back({c, r});
    }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(GetDlgItem(h, fields->inkling ? Inkling : Qwen));
    return 1;
}
extern "C" void ui_strata_model(int ink, int vector, unsigned context, const wchar_t *status, int statusOnly)
{
    HWND h = strataDialog;
    if (!h)
        return;
    SetDlgItemTextW(h, ServerBadge, status);
    if (!statusOnly || !strataReady)
    {
        applyModelChoice(h, ink);
        selectButton(h, VectorOn, ink || vector);
        selectButton(h, VectorOff, !ink && !vector);
        SetDlgItemInt(h, Hours, context, FALSE);
        settingsReady(h, true);
    }
}
extern "C" const wchar_t *ui_strata_fields(StrataFields *f)
{
    HWND h = strataDialog;
    if (!h)
        return L"";
    BOOL valid{};
    unsigned n = GetDlgItemInt(h, Hours, &valid, FALSE);
    auto raw = fieldText(h, Hours);
    bool digits = !raw.empty() && raw.find_first_not_of(L"0123456789") == wstring::npos;
    *f = {isSelected(h, Inkling),    isSelected(h, VectorOn), valid && digits ? n : 0,
          isSelected(h, ResumeChat), isSelected(h, WebTools), isSelected(h, Skills)};
    result = fieldText(h, InputField);
    return result.c_str();
}
extern "C" void ui_strata_message(const wchar_t *message, int error)
{
    if (!strataDialog)
        return;
    if (error)
        SetPropW(strataDialog, L"FavBarError", (HANDLE)1);
    else
        RemovePropW(strataDialog, L"FavBarError");
    if (!strataReady && error)
        SetDlgItemTextW(strataDialog, ServerBadge, L"Статус недоступен");
    SetDlgItemTextW(strataDialog, TimerStatus, message);
}
