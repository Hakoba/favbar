namespace
{
enum ToolId
{
    ResultList = 210,
    ToolStatus,
    RefreshQueue,
    OpenComfy,
    RemoveJob,
    PreviewCanvas,
    PreviewName,
    PreviewInfo
};
struct ToolRow
{
    wstring title, key;
    bool running = false;
    bool operator==(const ToolRow &other) const
    {
        return title == other.title && key == other.key && running == other.running;
    }
};
std::vector<ToolRow> folders, jobRows, nextJobs;
std::vector<size_t> matches;
int selection(HWND h)
{
    return (int)SendDlgItemMessageW(h, ResultList, LB_GETCURSEL, 0, 0);
}
HWND toolList(HWND h, int top, int height)
{
    auto list = control(
        h, L"LISTBOX", L"", ResultList, 20, top, 464, height,
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, font);
    SendMessageW(list, LB_SETITEMHEIGHT, 0, dpi(47));
    return list;
}
void themeTool(HWND h)
{
    styleCaption(h);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void refreshFolders()
{
    if (!folderSearch)
        return;
    auto query = fieldText(folderSearch, InputField);
    HWND list = GetDlgItem(folderSearch, ResultList);
    if (!list)
        return;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    matches.clear();
    for (size_t i = 0; i < folders.size(); ++i)
    {
        const auto &item = folders[i];
        if (!query.empty() && !StrStrIW(item.title.c_str(), query.c_str()) &&
            !StrStrIW(item.key.c_str(), query.c_str()))
            continue;
        matches.push_back(i);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)item.title.c_str());
    }
    if (!matches.empty())
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, FALSE);
    EnableWindow(GetDlgItem(folderSearch, PrimaryAction), !matches.empty());
    auto status = matches.empty() ? wstring(L"Папок не найдено") : L"Найдено: " + std::to_wstring(matches.size());
    SetDlgItemTextW(folderSearch, ToolStatus, status.c_str());
}
void openMatch()
{
    int i = selection(folderSearch);
    if (i >= 0 && i < (int)matches.size())
        event(18, -1, 0, folders[matches[i]].key);
}
void queueButtons()
{
    int i = selection(queueDialog);
    EnableWindow(GetDlgItem(queueDialog, RemoveJob), i >= 0 && i < (int)jobRows.size() && !jobRows[i].running);
}
LRESULT CALLBACK toolListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    bool search = h == folderSearch;
    if (msg == WM_DRAWITEM && wp == ResultList)
    {
        auto &d = *(DRAWITEMSTRUCT *)lp;
        if (d.itemID == (UINT)-1)
            return TRUE;
        auto i = (size_t)d.itemID;
        if ((search && i >= matches.size()) || (!search && i >= jobRows.size()))
            return TRUE;
        auto &item = search ? folders[matches[i]] : jobRows[i];
        auto r = d.rcItem;
        bool selected = (d.itemState & ODS_SELECTED) != 0;
        int saved = SaveDC(d.hDC);
        fill(d.hDC, r, selected ? hoverFill : bg);
        if (selected)
            fill(d.hDC, {r.left, r.top + dpi(6), r.left + dpi(2), r.bottom - dpi(6)}, mint);
        RECT sym{r.left + dpi(7), r.top, r.left + dpi(33), r.bottom};
        icon(d.hDC, sym, search ? 0xe8b7 : item.running ? 0xe768 : 0xe823, search || item.running ? accent : muted);
        r.left += dpi(39);
        r.right -= dpi(8);
        textAt(d.hDC, {r.left, r.top + dpi(2), r.right, r.top + dpi(27)}, item.title, fg, font);
        auto sub = search ? item.key : (item.running ? L"В работе   ·   " : L"Ожидает   ·   ") + item.key;
        textAt(d.hDC, {r.left, r.top + dpi(24), r.right, r.bottom - dpi(2)}, sub, muted, smallFont);
        RestoreDC(d.hDC, saved);
        return TRUE;
    }
    if (msg == WM_CTLCOLORLISTBOX)
    {
        SetDCBrushColor((HDC)wp, bg);
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    LRESULT out;
    if (popupPaint(h, msg, wp, lp, out))
        return out;
    switch (msg)
    {
    case WM_COMMAND: {
        int id = LOWORD(wp), n = HIWORD(wp);
        if (id == IDCANCEL)
        {
            DestroyWindow(h);
            return 0;
        }
        if (search)
        {
            if (id == InputField && n == EN_CHANGE)
                refreshFolders();
            else if (id == PrimaryAction || (id == ResultList && n == LBN_DBLCLK))
                openMatch();
        }
        else if (id == ResultList && n == LBN_SELCHANGE)
            queueButtons();
        else if (id == RefreshQueue)
            event(19);
        else if (id == OpenComfy)
            event(21);
        else if (id == RemoveJob)
        {
            int i = selection(h);
            if (i >= 0 && i < (int)jobRows.size() && !jobRows[i].running)
            {
                auto item = jobRows[i];
                event(20, -1, 0, item.key);
                EnableWindow(GetDlgItem(h, RemoveJob), FALSE);
            }
        }
        return 0;
    }
    case WM_THEMECHANGED:
        themeTool(h);
        return 0;
    case WM_DESTROY:
        if (search)
        {
            folderSearch = nullptr;
            folders = {};
            matches = {};
        }
        else if (h == queueDialog)
        {
            queueDialog = nullptr;
            jobRows = {};
            nextJobs = {};
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
bool toolMessage(MSG &m)
{
    for (HWND h : {folderSearch, queueDialog, filePreview})
    {
        if (!h || (m.hwnd != h && !IsChild(h, m.hwnd)))
            continue;
        if (m.message == WM_KEYDOWN)
        {
            if (m.wParam == VK_ESCAPE)
            {
                DestroyWindow(h);
                return true;
            }
            if (h == folderSearch)
            {
                if (m.wParam == VK_RETURN)
                {
                    openMatch();
                    return true;
                }
                if (m.hwnd == GetDlgItem(h, InputField) && (m.wParam == VK_UP || m.wParam == VK_DOWN))
                {
                    if (!matches.empty())
                        SendDlgItemMessageW(
                            h, ResultList, LB_SETCURSEL,
                            std::clamp(selection(h) + (m.wParam == VK_UP ? -1 : 1), 0, (int)matches.size() - 1), 0);
                    return true;
                }
            }
        }
        return IsDialogMessageW(h, &m) != FALSE;
    }
    return false;
}
} // namespace
extern "C" void ui_search_begin()
{
    folders.clear();
}
extern "C" void ui_search_item(const wchar_t *title, const wchar_t *path)
{
    folders.push_back({title, path});
}
extern "C" void ui_search_show()
{
    if (!folderSearch)
    {
        HWND h = popupWindow(L"FavBarFolderSearch", toolListProc, L"FavBar · Быстрый переход", 407, nullptr, 504);
        if (!h)
            return;
        folderSearch = h;
        popupTitle(h, L"Быстрый переход");
        auto edit = control(h, L"EDIT", L"", InputField, 26, 83, 452, 22, ES_AUTOHSCROLL | WS_TABSTOP, font);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Название папки или часть пути");
        toolList(h, 124, 219);
        control(h, L"STATIC", L"", ToolStatus, 20, 359, 266, 32, SS_CENTERIMAGE, smallFont);
        popupButton(h, L"Открыть папку", PrimaryAction, 329, 357, 155);
    }
    refreshFolders();
    ShowWindow(folderSearch, SW_SHOW);
    SetForegroundWindow(folderSearch);
    HWND edit = GetDlgItem(folderSearch, InputField);
    SetFocus(edit);
    SendMessageW(edit, EM_SETSEL, 0, -1);
}
extern "C" void ui_search_close()
{
    if (folderSearch)
        DestroyWindow(folderSearch);
}
extern "C" int ui_queue_visible()
{
    return queueDialog != nullptr;
}
extern "C" void ui_queue_open()
{
    if (!queueDialog)
    {
        HWND h = popupWindow(L"FavBarComfyQueue", toolListProc, L"FavBar · Очередь ComfyUI", 406, nullptr, 504);
        if (!h)
            return;
        queueDialog = h;
        popupTitle(h, L"Очередь ComfyUI");
        control(h, L"STATIC", L"Чтение очереди…", ToolStatus, 20, 70, 357, 27, SS_CENTERIMAGE, font);
        popupButton(h, L"Обновить", RefreshQueue, 390, 70, 94, 28);
        toolList(h, 110, 234);
        popupButton(h, L"Открыть ComfyUI", OpenComfy, 20, 356, 153);
        popupButton(h, L"Снять выбранную", RemoveJob, 321, 356, 163);
        queueButtons();
    }
    ShowWindow(queueDialog, SW_SHOW);
    SetForegroundWindow(queueDialog);
}
extern "C" void ui_queue_begin()
{
    nextJobs.clear();
}
extern "C" void ui_queue_item(const wchar_t *id, const wchar_t *title, int running)
{
    nextJobs.push_back({title, id, running != 0});
}
extern "C" void ui_queue_finish(int online)
{
    if (!queueDialog)
        return;
    HWND h = queueDialog, list = GetDlgItem(h, ResultList);
    if (nextJobs != jobRows)
    {
        int i = selection(h), top = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        wstring selected = i >= 0 && i < (int)jobRows.size() ? jobRows[i].key : L"";
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        jobRows.swap(nextJobs);
        for (size_t k = 0; k < jobRows.size(); ++k)
        {
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)jobRows[k].title.c_str());
            if (jobRows[k].key == selected)
                SendMessageW(list, LB_SETCURSEL, k, 0);
        }
        if (!jobRows.empty())
            SendMessageW(list, LB_SETTOPINDEX, std::min(top, (int)jobRows.size() - 1), 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, FALSE);
    }
    nextJobs.clear();
    size_t active = std::count_if(jobRows.begin(), jobRows.end(), [](auto &j) { return j.running; });
    wstring status = !online           ? L"Нет связи с ComfyUI"
                     : jobRows.empty() ? L"Очередь пуста"
                                       : L"В работе: " + std::to_wstring(active) + L"    ·    Ожидают: " +
                                             std::to_wstring(jobRows.size() - active);
    if (fieldText(h, ToolStatus) != status)
        SetDlgItemTextW(h, ToolStatus, status.c_str());
    queueButtons();
}
extern "C" void ui_queue_notice(const wchar_t *s)
{
    if (queueDialog)
        SetDlgItemTextW(queueDialog, ToolStatus, s);
}
