#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <memory>

namespace
{
struct FileFrame
{
    Pixels image;
    UINT width{}, height{};
    wstring info, empty;
    bool exists = false;
};
wstring previewPath;
FileFrame displayed;

void readImage(const wchar_t *path, FileFrame &frame)
{
    auto f = factory();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> original;
    if (!f ||
        FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &original)) || FAILED(original->GetSize(&frame.width, &frame.height)))
        return;
    auto source = thumbnail(f.Get(), decoder.Get(), std::min(768U, std::max(frame.width, frame.height)));
    if (!source)
        return;
    auto &p = frame.image;
    source->GetSize(&p.w, &p.h);
    p.data.resize(p.w * p.h * 4);
    if (FAILED(source->CopyPixels(nullptr, p.w * 4, (UINT)p.data.size(), p.data.data())))
        p = {};
    // Composite PNG transparency onto a neutral tile before passing pixels to GDI.
    for (UINT y = 0; y < p.h; ++y)
        for (UINT x = 0; x < p.w; ++x)
        {
            BYTE *px = &p.data[4 * (y * p.w + x)];
            int a = px[3];
            int paper = ((x / 12 + y / 12) % 2) ? 217 : 232;
            if (a != 255)
                for (int c = 0; c < 3; ++c)
                    px[c] = (BYTE)((px[c] * a + paper * (255 - a) + 127) / 255);
        }
}
struct MediaLibraries
{
    HMODULE platform = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE reader = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    template <typename T> static T symbol(HMODULE lib, const char *name)
    {
        return lib ? reinterpret_cast<T>(GetProcAddress(lib, name)) : nullptr;
    }
    decltype(&MFStartup) start = symbol<decltype(start)>(platform, "MFStartup");
    decltype(&MFShutdown) stop = symbol<decltype(stop)>(platform, "MFShutdown");
    decltype(&MFCreateAttributes) attributes = symbol<decltype(attributes)>(platform, "MFCreateAttributes");
    decltype(&MFCreateMediaType) type = symbol<decltype(type)>(platform, "MFCreateMediaType");
    decltype(&MFCreateSourceReaderFromURL) open = symbol<decltype(open)>(reader, "MFCreateSourceReaderFromURL");
    bool active = start && stop && attributes && type && open && SUCCEEDED(start(MF_VERSION, MFSTARTUP_FULL));
    ~MediaLibraries()
    {
        if (active)
            stop();
        if (reader)
            FreeLibrary(reader);
        if (platform)
            FreeLibrary(platform);
    }
};
void scaleVideo(const BYTE *first, LONG stride, FileFrame &f)
{
    auto &p = f.image;
    double ratio = std::min(1., 768. / std::max(f.width, f.height));
    p.w = std::max(1U, UINT(f.width * ratio));
    p.h = std::max(1U, UINT(f.height * ratio));
    p.data.resize(p.w * p.h * 4);
    for (UINT y = 0; y < p.h; ++y)
    {
        auto src = first + (ptrdiff_t)(y * f.height / p.h) * stride;
        for (UINT x = 0; x < p.w; ++x)
            memcpy(&p.data[4 * (y * p.w + x)], src + 4 * (x * f.width / p.w), 4);
    }
}
void readVideo(const wchar_t *path, FileFrame &f)
{
    MediaLibraries mf;
    if (!mf.active)
        return;
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> rgb, actual;
    constexpr DWORD stream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    if (FAILED(mf.attributes(&attrs, 3)))
        return;
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    if (FAILED(mf.open(path, attrs.Get(), &reader)) || FAILED(mf.type(&rgb)))
        return;
    ComPtr<IMFMediaType> encoded;
    UINT sourceWidth = 0, sourceHeight = 0;
    if (SUCCEEDED(reader->GetNativeMediaType(stream, 0, &encoded)))
        MFGetAttributeSize(encoded.Get(), MF_MT_FRAME_SIZE, &sourceWidth, &sourceHeight);
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(stream, TRUE);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(reader->SetCurrentMediaType(stream, nullptr, rgb.Get())))
        return;
    for (int n = 0; n < 128; ++n)
    {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample)) || flags & MF_SOURCE_READERF_ERROR)
            return;
        if (!sample)
        {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                return;
            continue;
        }
        if (FAILED(reader->GetCurrentMediaType(stream, &actual)) ||
            FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height)) || !f.width || !f.height ||
            f.width > 32768 || f.height > 32768)
            return;
        const UINT bufferWidth = f.width, bufferHeight = f.height;
        LONG left = 0, top = 0;
        // RGB transforms can pad rows to a macroblock. Use the visible aperture for the thumbnail.
        MFVideoArea area{};
        if ((SUCCEEDED(actual->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), nullptr)) ||
             SUCCEEDED(actual->GetBlob(MF_MT_GEOMETRIC_APERTURE, (UINT8 *)&area, sizeof(area), nullptr))) &&
            area.OffsetX.value >= 0 && area.OffsetY.value >= 0 && area.Area.cx > 0 && area.Area.cy > 0 &&
            (UINT64)area.OffsetX.value + area.Area.cx <= bufferWidth &&
            (UINT64)area.OffsetY.value + area.Area.cy <= bufferHeight)
        {
            left = area.OffsetX.value;
            top = area.OffsetY.value;
            f.width = area.Area.cx;
            f.height = area.Area.cy;
        }
        else if (sourceWidth && sourceHeight && sourceWidth <= bufferWidth && sourceHeight <= bufferHeight)
        {
            f.width = sourceWidth;
            f.height = sourceHeight;
        }
        auto scale = [&](const BYTE *row, LONG stride) {
            scaleVideo(row + (ptrdiff_t)top * stride + left * 4, stride, f);
        };
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMF2DBuffer> plane;
        BYTE *first = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buf)) && SUCCEEDED(buf.As(&plane)) &&
            SUCCEEDED(plane->Lock2D(&first, &pitch)))
        {
            if (llabs((long long)pitch) >= 4LL * bufferWidth)
                scale(first, pitch);
            plane->Unlock2D();
            return;
        }
        buf.Reset();
        if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
            return;
        DWORD len = 0;
        UINT32 stride = 0;
        if (FAILED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)))
            stride = (UINT32)(-(LONG)(4 * bufferWidth));
        pitch = (LONG)stride;
        if (FAILED(buf->Lock(&first, nullptr, &len)))
            return;
        auto step = llabs((long long)pitch);
        if (step >= 4LL * bufferWidth && step * (bufferHeight - 1) + 4LL * bufferWidth <= len)
            scale(first + (pitch < 0 ? (bufferHeight - 1) * step : 0), pitch);
        buf->Unlock();
        return;
    }
}
void drawFrame(DRAWITEMSTRUCT &d)
{
    auto r = d.rcItem;
    fill(d.hDC, r, surface);
    auto &p = displayed.image;
    if (p.data.empty())
    {
        textAt(d.hDC, r, displayed.empty, muted, font, DT_CENTER);
        return;
    }
    LONG w = r.right - r.left, h = r.bottom - r.top;
    double ratio = std::min(double(w) / p.w, double(h) / p.h);
    int dw = std::max(1, int(p.w * ratio)), dh = std::max(1, int(p.h * ratio));
    BITMAPINFO bi{};
    auto &b = bi.bmiHeader;
    b.biSize = sizeof(b);
    b.biWidth = p.w;
    b.biHeight = -(LONG)p.h;
    b.biPlanes = 1;
    b.biBitCount = 32;
    b.biCompression = BI_RGB;
    SetStretchBltMode(d.hDC, HALFTONE);
    SetBrushOrgEx(d.hDC, 0, 0, nullptr);
    StretchDIBits(d.hDC, r.left + (w - dw) / 2, r.top + (h - dh) / 2, dw, dh, 0, 0, p.w, p.h, p.data.data(), &bi,
                  DIB_RGB_COLORS, SRCCOPY);
}
LRESULT CALLBACK filePreviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_DRAWITEM && wp == PreviewCanvas)
    {
        drawFrame(*(DRAWITEMSTRUCT *)lp);
        return TRUE;
    }
    LRESULT out;
    if (popupPaint(h, msg, wp, lp, out))
        return out;
    switch (msg)
    {
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL)
            DestroyWindow(h);
        else if (LOWORD(wp) == PrimaryAction && displayed.exists)
            event(22, -1, 0, previewPath);
        return 0;
    case WM_THEMECHANGED:
        themeTool(h);
        return 0;
    case WM_DESTROY:
        filePreview = nullptr;
        previewPath.clear();
        displayed = {};
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace
extern "C" FileFrame *preview_load(const wchar_t *path)
{
    auto frame = std::make_unique<FileFrame>();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        frame->empty = L"Файл недоступен";
        return frame.release();
    }
    frame->exists = true;
    frame->empty = L"Для этого файла нет предпросмотра";
    wstring ext = PathFindExtensionW(path);
    CharLowerBuffW(ext.data(), (DWORD)ext.size());
    static const wstring movies[] = {L".mp4",  L".m4v", L".mov", L".avi",  L".mkv",
                                     L".webm", L".wmv", L".mpg", L".mpeg", L".m2ts"};
    bool video = std::find(std::begin(movies), std::end(movies), ext) != std::end(movies);
    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
    {
        if (video)
            readVideo(path, *frame);
        else
            readImage(path, *frame);
        CoUninitialize();
    }
    ULARGE_INTEGER size{};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    wchar_t bytes[64]{};
    StrFormatByteSizeW(size.QuadPart, bytes, 64);
    frame->info = bytes;
    if (frame->width && frame->height)
        frame->info =
            std::to_wstring(frame->width) + L" × " + std::to_wstring(frame->height) + L"   ·   " + frame->info;
    if (video && !frame->image.data.empty())
        frame->info += L"   ·   Первый кадр";
    return frame.release();
}
extern "C" void preview_free(FileFrame *frame)
{
    delete frame;
}
extern "C" int ui_preview_matches(const wchar_t *path)
{
    return filePreview && previewPath == path;
}
extern "C" void ui_preview_apply(FileFrame *f)
{
    if (!f || !filePreview)
        return;
    displayed = std::move(*f);
    SetDlgItemTextW(filePreview, PreviewInfo, displayed.info.c_str());
    EnableWindow(GetDlgItem(filePreview, PrimaryAction), displayed.exists);
    InvalidateRect(GetDlgItem(filePreview, PreviewCanvas), nullptr, FALSE);
}
extern "C" int ui_preview_file(const wchar_t *path)
{
    if (!previewSpaceDown)
        return 0;
    if (!filePreview)
    {
        HWND h = popupWindow(L"FavBarFilePreview", filePreviewProc, L"FavBar · Предпросмотр", 425, nullptr, 504);
        if (!h)
            return 0;
        filePreview = h;
        popupTitle(h, L"Предпросмотр");
        control(h, L"STATIC", L"", PreviewName, 20, 66, 464, 25, SS_ENDELLIPSIS | SS_NOPREFIX, bold);
        control(h, L"STATIC", L"", PreviewCanvas, 20, 97, 464, 260, SS_OWNERDRAW, font);
        control(h, L"STATIC", L"", PreviewInfo, 20, 373, 299, 34, SS_CENTERIMAGE, font);
        popupButton(h, L"Показать в папке", PrimaryAction, 326, 372, 158);
    }
    previewPath = path;
    displayed = {};
    displayed.empty = L"Чтение файла…";
    SetDlgItemTextW(filePreview, PreviewName, PathFindFileNameW(path));
    SetDlgItemTextW(filePreview, PreviewInfo, L"");
    EnableWindow(GetDlgItem(filePreview, PrimaryAction), FALSE);
    InvalidateRect(GetDlgItem(filePreview, PreviewCanvas), nullptr, FALSE);
    ShowWindow(filePreview, SW_SHOWNOACTIVATE);
    return 1;
}
