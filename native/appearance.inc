// Sidebar geometry is shared by painting, keyboard navigation, scrolling and drop targets.
enum Style
{
    Tile,
    Section,
    Folder,
    Shelf,
    ShelfFile,
    Pocket,
    Brand,
    Favorites,
    Group,
    Service,
    Meter,
    QueueLine,
    System
};
HFONT smallFont{}, captionFont{}, symbolFont{}, brandFont{}, metricFont{};
COLORREF muted, border, hoverFill, selectionFill, folderColor, thumbColor, onlineColor, warningColor, errorColor;
int hoverSide = 0;
COLORREF mixColor(COLORREF first, COLORREF second, double weight)
{
    auto channel = [weight](BYTE a, BYTE b) { return std::nearbyint(a * weight + b * (1.0 - weight)); };
    return RGB(channel(GetRValue(first), GetRValue(second)), channel(GetGValue(first), GetGValue(second)),
               channel(GetBValue(first), GetBValue(second)));
}
void colors()
{
    bg = dark ? RGB(27, 28, 32) : RGB(247, 248, 250);
    surface = dark ? RGB(35, 36, 41) : RGB(255, 255, 255);
    fg = dark ? RGB(227, 229, 234) : RGB(28, 32, 36);
    muted = dark ? RGB(144, 151, 161) : RGB(100, 107, 117);
    border = dark ? RGB(45, 47, 53) : RGB(226, 229, 233);
    hoverFill = dark ? RGB(42, 44, 50) : RGB(236, 238, 241);
    folderColor = dark ? RGB(143, 152, 168) : RGB(125, 134, 150);
    thumbColor = dark ? RGB(59, 62, 69) : RGB(200, 205, 211);
    onlineColor = dark ? RGB(60, 207, 127) : RGB(24, 160, 88);
    warningColor = dark ? RGB(227, 179, 65) : RGB(194, 124, 14);
    errorColor = dark ? RGB(244, 112, 103) : RGB(209, 36, 47);
    DWORD raw = 0, size = sizeof(raw);
    COLORREF systemAccent = RGB(10, 100, 200);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                     &raw, &size) == ERROR_SUCCESS)
        systemAccent = raw & 0xffffff;
    accent = dark ? mixColor(systemAccent, RGB(255, 255, 255), .6) : mixColor(systemAccent, RGB(0, 0, 0), .85);
    selectionFill = mixColor(systemAccent, bg, dark ? .24 : .14);
}
HFONT typeface(int size, int weight, const wchar_t *family)
{
    return CreateFontW(-dpi(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
}
void fonts(bool create)
{
    for (auto f : {font, bold, smallFont, captionFont, symbolFont, brandFont, metricFont})
        if (f)
            DeleteObject(f);
    if (!create)
        return;
    font = typeface(13, FW_NORMAL, L"Segoe UI");
    bold = typeface(15, FW_SEMIBOLD, L"Segoe UI");
    smallFont = typeface(11, FW_NORMAL, L"Segoe UI");
    captionFont = typeface(10, FW_SEMIBOLD, L"Segoe UI");
    symbolFont = typeface(14, FW_NORMAL, L"Segoe MDL2 Assets");
    brandFont = typeface(28, FW_NORMAL, L"Georgia");
    metricFont = typeface(16, FW_SEMIBOLD, L"Segoe UI");
}
int row_size(int style)
{
    switch (style)
    {
    case Brand:
        return dpi(56);
    case Favorites:
        return dpi(34);
    case Folder:
        return dpi(27);
    case Section:
    case System:
        return dpi(38);
    case Group:
        return dpi(24);
    case Tile:
        return dpi(32);
    case Service:
        return dpi(27);
    case Meter:
        return dpi(58);
    case QueueLine:
        return dpi(31);
    case Shelf:
        return dpi(35);
    case Pocket:
        return dpi(35);
    default:
        return dpi(28);
    }
}
int footer()
{
    return dpi(26);
}
struct Layout
{
    struct Band
    {
        int index, y, bottom;
    };
    int maximum = 0, page = 1, treeEnd = 2, treeViewport = 0, treeStart = 0, systemStart = -1;
    bool treeScroll = false;
    std::vector<Band> bands;
    explicit Layout(int pos = top)
    {
        if (rows.empty())
            return;
        RECT client{};
        GetClientRect(window, &client);
        const int height = client.bottom - footer(), header = row_size(Brand);
        int total = 0;
        for (int i = 0; i < (int)rows.size(); i++)
        {
            total += row_size(rows[i].style);
            if (rows[i].style == System)
                systemStart = i;
        }
        while (treeEnd < (int)rows.size() && rows[treeEnd].style == Folder)
            treeEnd++;
        const int treeHeight = std::max(0, treeEnd - 2) * row_size(Folder);
        int room = height - (total - treeHeight);
        treeScroll = total > height && room >= 3 * row_size(Folder) && treeHeight > room;
        page = treeScroll ? room : std::max(1, height - header);
        maximum = std::max(0, total - height);
        pos = std::clamp(pos, 0, maximum);
        int y = 0;
        for (int i = 0; i < (int)rows.size(); i++)
        {
            const int rh = row_size(rows[i].style);
            if (i == 1 && !treeScroll)
                y -= pos;
            if (i == 2)
                treeStart = y;
            if (i == 2 && treeScroll)
                y -= pos;
            if (treeScroll && i == treeEnd)
                y = treeStart + room;
            if (i == systemStart && total < height)
                y += height - total;
            bands.push_back({i, y, y + rh});
            y += rh;
        }
        treeViewport = room;
    }
    RECT clip(const Band &b, int width) const
    {
        int begin = b.y, end = b.bottom;
        if (b.index > 0)
            begin = std::max(begin, row_size(Brand));
        if (treeScroll && b.index >= 2 && b.index < treeEnd)
        {
            begin = std::max(begin, treeStart);
            end = std::min(end, treeStart + treeViewport);
        }
        RECT client{};
        GetClientRect(window, &client);
        return {0, begin, width, std::min(end, (int)client.bottom - footer())};
    }
};
RECT footerRect()
{
    RECT r{};
    GetClientRect(window, &r);
    r.top = std::max(0L, r.bottom - footer());
    return r;
}
void invalidateFooter()
{
    auto r = footerRect();
    InvalidateRect(window, &r, FALSE);
}
void invalidateBand(const Layout &layout, int index)
{
    if (index < 0 || index >= (int)layout.bands.size())
        return;
    auto r = layout.clip(layout.bands[index], clientWidth());
    if (!IsRectEmpty(&r))
        InvalidateRect(window, &r, FALSE);
}
const wstring &footerText()
{
    if (!status.empty() || rows.empty())
        return status;
    if (hover >= 0 && hover < (int)rows.size() && !rows[hover].hint.empty())
        return rows[hover].hint;
    return rows.front().hint;
}
void setHover(int index, int cell)
{
    if (index == hover && cell == hoverSide)
        return;
    Layout layout;
    int previous = hover;
    const auto &oldHint = footerText();
    hover = index;
    hoverSide = cell;
    invalidateBand(layout, previous);
    if (previous != index)
        invalidateBand(layout, index);
    if (oldHint != footerText())
        invalidateFooter();
}
void setFocusRow(int index)
{
    if (index != focus)
    {
        Layout layout;
        invalidateBand(layout, focus);
        focus = index;
        invalidateBand(layout, focus);
    }
}
struct ScrollThumb
{
    int y, height, track, length, range, x;
    explicit ScrollThumb(const Layout &l)
        : track(l.treeScroll ? l.treeStart : row_size(Brand)), length(l.page), range(l.maximum),
          x(clientWidth() - dpi(8))
    {
        height = std::min(length, std::max(dpi(22), MulDiv(length, length, std::max(1, length + range))));
        y = track + (range ? MulDiv(top, length - height, range) : 0);
    }
    bool contains(int px, int py) const
    {
        return range > 0 && px >= x - dpi(2) && px < x + dpi(5) && py >= track && py < track + length;
    }
};
int scrollbarGrab = -1;
void scroll(int pos)
{
    Layout layout(pos);
    top = std::clamp(pos, 0, layout.maximum);
    InvalidateRect(window, nullptr, FALSE);
}
void ensureFocus()
{
    Layout layout;
    if (focus >= 0 && focus < (int)layout.bands.size())
    {
        auto b = layout.bands[focus];
        int start = layout.treeScroll ? layout.treeStart : row_size(Brand);
        if (!layout.treeScroll || (focus >= 2 && focus < layout.treeEnd))
        {
            if (b.y < start)
                scroll(top + b.y - start);
            else if (b.bottom > start + layout.page)
                scroll(top + b.bottom - start - layout.page);
        }
    }
    InvalidateRect(window, nullptr, FALSE);
}
void fill(HDC dc, RECT r, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
void box(HDC dc, RECT r, COLORREF color, COLORREF line, int radius = 5)
{
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, line);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, dpi(radius * 2), dpi(radius * 2));
}
void textAt(HDC dc, RECT r, const wstring &s, COLORREF color, HFONT f, UINT align = DT_LEFT)
{
    SelectObject(dc, f);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | align);
}
void icon(HDC dc, RECT r, int unicode, COLORREF color)
{
    textAt(dc, r, wstring(1, (wchar_t)unicode), color, symbolFont, DT_CENTER);
}
void folder_icon(HDC, RECT &, const wstring &, COLORREF);
void brandMark(HDC dc, int x, int cy)
{
    int y = cy - dpi(14);
    RECT tile{x, y, x + dpi(28), y + dpi(28)};
    box(dc, tile, fg, fg, 7);
    COLORREF ink = bg;
    POINT mark[6] = {{x + dpi(7), y + dpi(7)},   {x + dpi(21), y + dpi(7)},  {x + dpi(19), y + dpi(11)},
                     {x + dpi(12), y + dpi(11)}, {x + dpi(10), y + dpi(21)}, {x + dpi(6), y + dpi(22)}};
    SetDCBrushColor(dc, ink);
    SetDCPenColor(dc, ink);
    Polygon(dc, mark, 6);
    POINT arm[4] = {
        {x + dpi(13), y + dpi(14)}, {x + dpi(20), y + dpi(14)}, {x + dpi(17), y + dpi(17)}, {x + dpi(12), y + dpi(17)}};
    Polygon(dc, arm, 4);
}
int split(const Row &r, int w)
{
    switch (r.style)
    {
    case Brand:
    case Favorites:
    case Pocket:
    case Shelf:
    case ShelfFile:
        return w - dpi(39);
    case Folder:
        return std::max(w / 2, w - dpi(78));
    default:
        return r.right.empty() ? w : w / 2;
    }
}
RECT folderContent(const Row &r, int w, int y, int bottom)
{
    int div = split(r, w), margin = dpi(12);
    int indent = std::min(dpi(12) * r.depth, std::max(0, div - margin - dpi(70)));
    return {margin + dpi(3) + indent, y, div - dpi(3), bottom};
}
bool folderBranch(const Row &r)
{
    return !r.right.empty() && (r.right.front() == L'▾' || r.right.front() == L'▸');
}
void tile(HDC dc, RECT r, const wstring &s, int glyph, bool over, bool featured)
{
    r.top += dpi(2);
    r.bottom -= dpi(2);
    auto face = over ? hoverFill : featured ? selectionFill : surface;
    box(dc, r, face, over ? accent : border, 5);
    icon(dc, {r.left + dpi(7), r.top, r.left + dpi(25), r.bottom}, glyph ? glyph : 0xe768, featured ? accent : muted);
    r.left += dpi(30);
    r.right -= dpi(5);
    SIZE needed{};
    SelectObject(dc, font);
    GetTextExtentPoint32W(dc, s.data(), (int)s.size(), &needed);
    textAt(dc, r, s, fg, needed.cx > r.right - r.left ? smallFont : font);
}
void service(HDC dc, RECT r, const wstring &s, bool active)
{
    if (s.empty())
        return;
    if (active)
        box(dc, {r.left, r.top + dpi(1), r.right, r.bottom - dpi(1)}, hoverFill, hoverFill);
    int cy = (r.top + r.bottom) / 2, x = r.left + dpi(7);
    COLORREF c = s[0] == L'●' ? onlineColor : muted;
    box(dc, {x, cy - dpi(3), x + dpi(6), cy + dpi(3)}, s[0] == L'●' ? c : bg, c, 3);
    r.left += dpi(19);
    textAt(dc, r, s.size() > 2 ? s.substr(2) : s, fg, font);
}
void meter(HDC dc, RECT r, const wstring &s, int pct, bool active)
{
    r.top += dpi(2);
    r.bottom -= dpi(2);
    box(dc, r, active ? hoverFill : surface, border, 6);
    auto cut = s.find(L"  ");
    wstring name = s.substr(0, cut);
    wstring value = cut == wstring::npos ? L"—" : s.substr(cut + 2);
    RECT content{r.left + dpi(9), r.top + dpi(5), r.right - dpi(8), r.top + dpi(18)};
    textAt(dc, content, name, muted, captionFont);
    content.top = r.top + dpi(17);
    content.bottom = r.bottom - dpi(11);
    textAt(dc, content, value, fg, metricFont);
    RECT bar{r.left + dpi(9), r.bottom - dpi(8), r.right - dpi(9), r.bottom - dpi(5)};
    fill(dc, bar, border);
    int pixels = MulDiv(bar.right - bar.left, std::clamp(pct, 0, 100), 100);
    bar.right = bar.left + pixels;
    if (pixels)
        fill(dc, bar, pct >= 92 ? errorColor : pct >= 80 ? warningColor : accent);
}
void drawBand(HDC dc, const Layout::Band &band, int w)
{
    int i = band.index, y = band.y, end = band.bottom, cy = (y + end) / 2;
    const Row &r = rows[i];
    bool over = i == hover;
    int margin = dpi(12), div = split(r, w);
    RECT row{margin, y, w - margin, end};
    RECT right{div, y, w - margin, end};
    auto arrow = r.left.find(L'▾') == 0 ? 0xe70d : 0xe76c;
    auto caption = [&](wstring text, int x, int finish) {
        CharUpperBuffW(text.data(), (DWORD)text.size());
        int old = SetTextCharacterExtra(dc, dpi(1));
        textAt(dc, {x, y, finish, end}, text, muted, captionFont);
        SetTextCharacterExtra(dc, old);
    };
    switch (r.style)
    {
    case Brand:
        brandMark(dc, margin, cy);
        textAt(dc, {margin + dpi(38), y, w - dpi(73), end}, L"FavBar", fg, brandFont);
        if (over)
        {
            int x = hoverSide ? w - dpi(36) : w - dpi(66);
            box(dc, {x, cy - dpi(13), x + dpi(26), cy + dpi(13)}, surface, border);
        }
        icon(dc, {w - dpi(66), y, w - dpi(40), end}, 0xe712, muted);
        icon(dc, {w - dpi(36), y, w - dpi(10), end}, edge == ABE_LEFT ? 0xe76b : 0xe76c, muted);
        fill(dc, {margin, end - 1, w - margin, end}, border);
        break;
    case Favorites:
        caption(L"Избранное", margin, w - dpi(100));
        icon(dc, {w - dpi(100), y, w - dpi(74), end}, 0xe721, over && hoverSide == 2 ? accent : muted);
        icon(dc, {w - dpi(68), y, w - dpi(42), end}, 0xe80f, over && !hoverSide ? accent : muted);
        icon(dc, right, 0xe710, over && hoverSide == 1 ? accent : muted);
        break;
    case Folder: {
        bool home = r.left.find(L'⌂') == 0;
        if (home || over || i == focus)
            box(dc, {margin - dpi(3), y + 1, w - margin + dpi(3), end - 1},
                over   ? hoverFill
                : home ? selectionFill
                       : surface,
                over   ? hoverFill
                : home ? selectionFill
                       : surface,
                5);
        RECT l = folderContent(r, w, y, end);
        if (r.depth)
            fill(dc, {l.left - dpi(6), y + dpi(2), l.left - dpi(6) + 1, end - dpi(2)}, border);
        folder_icon(dc, l, r.path, r.depth == 0 || r.right.find(L'▾') == 0 ? accent : folderColor);
        textAt(dc, l, home ? r.left.substr(2) : r.left, over && !hoverSide ? accent : fg, font);
        auto off = r.right.find(L' ');
        if (off != wstring::npos)
            textAt(dc, {div, y, w - margin - dpi(16), end}, r.right.substr(off + 1), muted, smallFont, DT_RIGHT);
        if (folderBranch(r))
            icon(dc, {w - margin - dpi(13), y, w - margin + dpi(2), end}, r.right[0] == L'▾' ? 0xe70d : 0xe76c,
                 over && hoverSide ? accent : muted);
        break;
    }
    case Pocket:
    case Shelf:
    case ShelfFile: {
        bool shelf = r.style == Shelf;
        if (r.style != ShelfFile)
            box(dc, {margin, y + dpi(3), w - margin, end - dpi(3)}, over ? hoverFill : surface, border, 5);
        icon(dc, {margin + dpi(6), y, margin + dpi(26), end},
             shelf               ? 0xe8b7
             : r.style == Pocket ? 0xe71b
                                 : 0xe7c3,
             shelf ? accent : muted);
        wstring lab = r.left;
        if (shelf && lab.size() > 2)
            lab = lab.substr(2);
        textAt(dc, {margin + dpi(32), y, div, end}, lab, fg, font);
        if (shelf && r.left.find(L"(0)") == wstring::npos)
            icon(dc, {div - dpi(27), y, div, end}, 0xe74d, hoverSide == 2 && over ? accent : muted);
        icon(dc, right, shelf ? 0xe710 : r.style == Pocket ? 0xe8a7 : 0xe8da, muted);
        break;
    }
    case Section:
    case System: {
        fill(dc, {margin, y + dpi(4), w - margin, y + dpi(4) + 1}, border);
        textAt(dc, {margin, y + dpi(5), w - dpi(39), end}, r.left.size() > 2 ? r.left.substr(2) : r.left, fg, bold);
        icon(dc, {w - margin - dpi(16), y + dpi(4), w - margin, end}, arrow, muted);
        break;
    }
    case Group: {
        bool fold = r.left.find(L'▾') == 0 || r.left.find(L'▸') == 0;
        caption(fold ? r.left.substr(2) : r.left, margin + dpi(2), w - margin - dpi(16));
        if (fold)
            icon(dc, {w - margin - dpi(16), y, w - margin, end}, arrow, muted);
        break;
    }
    case Service:
        service(dc, {margin, y, div - dpi(2), end}, r.left, over && hoverSide == 0);
        service(dc, {div + dpi(2), y, w - margin, end}, r.right, over && hoverSide == 1);
        break;
    case Meter:
        meter(dc, {margin, y, div - dpi(3), end}, r.left, r.leftDetail, over && !hoverSide);
        meter(dc, {div + dpi(3), y, w - margin, end}, r.right, r.rightDetail, over && hoverSide);
        break;
    case QueueLine:
        icon(dc, {margin, y, margin + dpi(19), end}, 0xe9f5, muted);
        textAt(dc, {margin + dpi(25), y, w - margin, end}, r.left, muted, font);
        break;
    default:
        tile(dc, {margin, y, r.right.empty() ? w - margin : div - dpi(3), end}, r.left, r.leftDetail,
             over && !hoverSide, r.leftDetail == 0xe714);
        if (!r.right.empty())
            tile(dc, {div + dpi(3), y, w - margin, end}, r.right, r.rightDetail, over && hoverSide, false);
        break;
    }
}
struct PaintBuffer
{
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ original{};
    int width{}, height{};
    void reset()
    {
        if (original)
            SelectObject(dc, original);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        *this = {};
    }
    HDC acquire(HDC target, int w, int h)
    {
        if (dc && w == width && h == height)
            return dc;
        reset();
        dc = CreateCompatibleDC(target);
        if (dc)
            bitmap = CreateCompatibleBitmap(target, w, h);
        if (bitmap)
            original = SelectObject(dc, bitmap);
        if (!original)
        {
            reset();
            return nullptr;
        }
        width = w;
        height = h;
        return dc;
    }
} paintBuffer;
void drawPanel(HDC dc, RECT dirty)
{
    int w = clientWidth();
    Layout layout;
    fill(dc, dirty, bg);
    for (auto &b : layout.bands)
    {
        RECT clip = layout.clip(b, w), area{};
        if (!IntersectRect(&area, &clip, &dirty) || !RectVisible(dc, &area))
            continue;
        int saved = SaveDC(dc);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        drawBand(dc, b, w);
        RestoreDC(dc, saved);
    }
    RECT foot = footerRect();
    if (RectVisible(dc, &foot))
    {
        fill(dc, foot, surface);
        fill(dc, {0, foot.top, w, foot.top + 1}, border);
        icon(dc, {dpi(9), foot.top, dpi(27), foot.bottom}, 0xe765, muted);
        textAt(dc, {dpi(32), foot.top, w - dpi(9), foot.bottom}, footerText(), muted, smallFont);
    }
    ScrollThumb thumb(layout);
    if (thumb.range)
    {
        fill(dc, {thumb.x, thumb.track, thumb.x + dpi(2), thumb.track + thumb.length}, border);
        fill(dc, {thumb.x, thumb.y, thumb.x + dpi(2), thumb.y + thumb.height},
             scrollbarGrab >= 0 ? accent : thumbColor);
    }
}
void paint()
{
    // BeginPaint validates the update region. Keep its disjoint strips for the memory DC.
    HRGN damage = CreateRectRgn(0, 0, 0, 0);
    int regionType = damage ? GetUpdateRgn(window, damage, FALSE) : ERROR;
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(window, &ps);
    if (target && !IsRectEmpty(&ps.rcPaint))
    {
        RECT r{};
        GetClientRect(window, &r);
        HDC buffer = paintBuffer.acquire(target, r.right, r.bottom);
        if (buffer)
        {
            int saved = SaveDC(buffer);
            if (regionType > NULLREGION)
                SelectClipRgn(buffer, damage);
            else
                IntersectClipRect(buffer, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
            drawPanel(buffer, ps.rcPaint);
            RestoreDC(buffer, saved);
            auto &p = ps.rcPaint;
            BitBlt(target, p.left, p.top, p.right - p.left, p.bottom - p.top, buffer, p.left, p.top, SRCCOPY);
        }
        else
            drawPanel(target, ps.rcPaint);
    }
    EndPaint(window, &ps);
    if (damage)
        DeleteObject(damage);
}
int hit(int y)
{
    Layout layout;
    for (auto &b : layout.bands)
    {
        auto clip = layout.clip(b, clientWidth());
        if (y >= clip.top && y < clip.bottom)
            return b.index;
    }
    return -1;
}
int side(int i, int x, int y)
{
    auto &r = rows[i];
    if (r.style == Favorites && x >= clientWidth() - dpi(101) && x < clientWidth() - dpi(72))
        return 2;
    if (r.style == Folder)
    {
        if (!folderBranch(r))
            return 0;
        auto b = Layout().bands[i];
        auto content = folderContent(r, clientWidth(), b.y, b.bottom);
        int mid = (b.y + b.bottom) / 2;
        RECT glyph{content.left, mid - dpi(8), content.left + dpi(16), mid + dpi(8)};
        if (PtInRect(&glyph, {x, y}))
            return 0;
        HDC dc = GetDC(window);
        auto previous = SelectObject(dc, font);
        const wchar_t *label = r.left.c_str() + (r.left.rfind(L"⌂ ", 0) == 0 ? 2 : 0);
        SIZE extent{};
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &extent);
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        content.left += dpi(20);
        content.right = std::min(content.right, content.left + extent.cx);
        content.top = mid - extent.cy / 2;
        content.bottom = content.top + extent.cy;
        return !PtInRect(&content, {x, y});
    }
    int cut = split(r, clientWidth());
    if (r.style == Shelf && r.left.find(L"(0)") == wstring::npos && x >= cut - dpi(27) && x < cut)
        return 2;
    return !r.right.empty() && x >= cut;
}
