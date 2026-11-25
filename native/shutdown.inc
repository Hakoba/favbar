// Windows owns the scheduled shutdown. Closing this modeless window only stops its display timer.
#include <ctime>
extern "C" int favbar_shutdown_command(int seconds);
namespace
{
constexpr int presetMinutes[] = {15, 30, 60, 120};
std::time_t shutdownAt{};
bool timerFieldsChanging{};
int durationInput(HWND h, int id, int maximum)
{
    wchar_t value[5]{};
    int count = GetWindowTextLengthW(GetDlgItem(h, id));
    if (count <= 0 || count >= 5)
        return -1;
    GetDlgItemTextW(h, id, value, 5);
    int n = 0;
    for (int i = 0; i < count; ++i)
    {
        if (value[i] < L'0' || value[i] > L'9')
            return -1;
        n = n * 10 + (value[i] - L'0');
    }
    return n <= maximum ? n : -1;
}
int durationMinutes(HWND h)
{
    int hours = durationInput(h, Hours, 168), mins = durationInput(h, Minutes, 59);
    int n = hours * 60 + mins;
    return hours >= 0 && mins >= 0 && n > 0 && n <= 7 * 24 * 60 ? n : 0;
}
wstring clockAt(std::time_t at)
{
    std::tm t{};
    localtime_s(&t, &at);
    wchar_t buf[32]{};
    std::wcsftime(buf, 32, L"%d.%m в %H:%M", &t);
    return buf;
}
void timerStatus(HWND h, const wstring &s, bool error = false)
{
    if (error)
        SetPropW(h, L"FavBarError", (HANDLE)1);
    else
        RemovePropW(h, L"FavBarError");
    SetDlgItemTextW(h, TimerStatus, s.c_str());
}
void timerFields(HWND h)
{
    int minutes = durationMinutes(h);
    for (int i = 0; i < 4; ++i)
    {
        HWND b = GetDlgItem(h, FirstPreset + i);
        bool selected = minutes == presetMinutes[i];
        if (selected != (GetPropW(b, L"FavBarSelected") != nullptr))
        {
            if (selected)
                SetPropW(b, L"FavBarSelected", (HANDLE)1);
            else
                RemovePropW(b, L"FavBarSelected");
            InvalidateRect(b, nullptr, FALSE);
        }
    }
    EnableWindow(GetDlgItem(h, PrimaryAction), minutes && !shutdownAt);
    wstring text = shutdownAt ? L"Выключение " + clockAt(shutdownAt)
                   : minutes  ? L"Выключение " + clockAt(std::time(nullptr) + minutes * 60)
                              : L"Укажите время от 1 минуты до 7 суток";
    SetDlgItemTextW(h, TargetTime, text.c_str());
}
void setDuration(HWND h, int minutes)
{
    timerFieldsChanging = true;
    SetDlgItemInt(h, Hours, minutes / 60, FALSE);
    SetDlgItemInt(h, Minutes, minutes % 60, FALSE);
    timerFieldsChanging = false;
    timerFields(h);
}
void countdown(HWND h)
{
    int s = (int)std::max<std::time_t>(0, shutdownAt - std::time(nullptr));
    wchar_t buf[64]{};
    swprintf_s(buf, L"Осталось %02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    timerStatus(h, buf);
    if (!s)
    {
        KillTimer(h, 1);
        shutdownAt = 0;
        timerFields(h);
    }
}
void runTimer(HWND h, bool cancel)
{
    int minutes = durationMinutes(h);
    if (!cancel && (!minutes || shutdownAt))
        return;
    int code = favbar_shutdown_command(cancel ? -1 : minutes * 60);
    if (cancel && (code == 0 || code == 1116))
    {
        shutdownAt = 0;
        KillTimer(h, 1);
        timerFields(h);
        timerStatus(h, code == 0 ? L"Выключение отменено." : L"Активного таймера нет.");
    }
    else if (code == 1190)
        timerStatus(h, L"Выключение уже запланировано. Чтобы изменить время, сначала отмените таймер.", true);
    else if (code)
    {
        wchar_t msg[320]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)code, 0, msg, 320,
                       nullptr);
        timerStatus(h, *msg ? msg : L"Windows вернула ошибку " + std::to_wstring(code), true);
    }
    else
    {
        shutdownAt = std::time(nullptr) + minutes * 60;
        timerFields(h);
        countdown(h);
        SetTimer(h, 1, 1000, nullptr);
    }
}
LRESULT CALLBACK timerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT out;
    if (popupPaint(h, msg, wp, lp, out))
        return out;
    switch (msg)
    {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDCANCEL)
            DestroyWindow(h);
        else if (id >= FirstPreset && id < FirstPreset + 4 && HIWORD(wp) == BN_CLICKED)
            setDuration(h, presetMinutes[id - FirstPreset]);
        else if ((id == Hours || id == Minutes) && HIWORD(wp) == EN_CHANGE && !timerFieldsChanging)
            timerFields(h);
        else if ((id == PrimaryAction || id == StopTimer) && HIWORD(wp) == BN_CLICKED)
            runTimer(h, id == StopTimer);
        return 0;
    }
    case WM_TIMER:
        countdown(h);
        return 0;
    case WM_DESTROY:
        timerDialog = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
void refreshTimerTheme()
{
    if (timerDialog)
    {
        styleCaption(timerDialog);
        RedrawWindow(timerDialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}
} // namespace
extern "C" void ui_shutdown_timer()
{
    if (timerDialog)
    {
        ShowWindow(timerDialog, SW_RESTORE);
        SetForegroundWindow(timerDialog);
        return;
    }
    HWND h = popupWindow(L"FavBarShutdownTimer", timerProc, L"FavBar · Таймер выключения", 335);
    if (!h)
        return;
    timerDialog = h;
    popupTitle(h, L"Выключить через…");
    for (int i = 0; i < 4; ++i)
    {
        const wchar_t *labels[] = {L"15 мин", L"30 мин", L"1 час", L"2 часа"};
        popupButton(h, labels[i], FirstPreset + i, 20 + i * 94, 75, 86, 32);
    }
    control(h, L"STATIC", L"Своё время", 0, 20, 124, 97, 31, SS_CENTERIMAGE, font);
    for (int i = 0; i < 2; ++i)
    {
        int x = 124 + 104 * i;
        HWND input =
            control(h, L"EDIT", L"", i ? Minutes : Hours, x + 5, 131, 44, 17, WS_TABSTOP | ES_CENTER | ES_NUMBER, font);
        SendMessageW(input, EM_SETLIMITTEXT, i ? 2 : 3, 0);
        control(h, L"STATIC", i ? L"мин" : L"ч", 0, x + 63, 124, 40, 31, SS_CENTERIMAGE, font);
    }
    control(h, L"STATIC", L"", TargetTime, 20, 169, popupWidth - 40, 25, SS_CENTERIMAGE, bold);
    control(h, L"STATIC", L"", TimerStatus, 20, 198, popupWidth - 40, 43, SS_NOPREFIX, font);
    popupButton(h, L"Запустить таймер", PrimaryAction, 20, 246, 180, 38);
    popupButton(h, L"Отменить таймер", StopTimer, 208, 246, 180, 38);
    control(h, L"STATIC", L"Сохраните файлы до выключения. Окно таймера можно закрыть.", 0, 20, 298, popupWidth - 40,
            34, SS_NOPREFIX, smallFont);
    if (shutdownAt && shutdownAt <= std::time(nullptr))
        shutdownAt = 0;
    setDuration(h, 30);
    if (shutdownAt)
    {
        countdown(h);
        SetTimer(h, 1, 1000, nullptr);
    }
    else
        timerStatus(h, L"Таймер запустится после нажатия кнопки.");
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(GetDlgItem(h, FirstPreset + 1));
}
