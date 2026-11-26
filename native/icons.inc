// WIC handles imported PNG thumbnails and images selected from disk.
#include <map>
#include <wincodec.h>
#include <wincrypt.h>

namespace
{
ComPtr<IWICImagingFactory> factory()
{
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}
ComPtr<IWICBitmapSource> thumbnail(IWICImagingFactory *f, IWICBitmapDecoder *d, UINT side)
{
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(d->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 32768 || h > 32768)
        return {};
    UINT ow = std::max(1u, UINT(double(w) * side / std::max(w, h))),
         oh = std::max(1u, UINT(double(h) * side / std::max(w, h)));
    if (FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), ow, oh, WICBitmapInterpolationModeFant)) ||
        FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return {};
    ComPtr<IWICBitmapSource> source;
    conv.As(&source);
    return source;
}
struct Pixels
{
    UINT w = 0, h = 0;
    std::vector<BYTE> data;
};
Pixels pixels(const wstring &s)
{
    Pixels out;
    if (s.size() > 131072)
        return out;
    DWORD len = 0;
    auto text = s.c_str() + 4;
    if (!CryptStringToBinaryW(text, 0, CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr) || !len)
        return out;
    std::vector<BYTE> raw(len);
    if (!CryptStringToBinaryW(text, 0, CRYPT_STRING_BASE64, raw.data(), &len, nullptr, nullptr))
        return out;
    auto f = factory();
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (!f || FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(raw.data(), len)) ||
        FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)))
        return out;
    auto image = thumbnail(f.Get(), decoder.Get(), dpi(16));
    if (!image)
        return out;
    image->GetSize(&out.w, &out.h);
    out.data.resize(out.w * out.h * 4);
    if (FAILED(image->CopyPixels(nullptr, out.w * 4, (UINT)out.data.size(), out.data.data())))
        out = {};
    return out;
}
void folder_icon(HDC dc, RECT &r, const wstring &spec, COLORREF defaultColor)
{
    static const std::map<wstring, COLORREF> colors = {{L"pink", RGB(233, 99, 161)},
                                                       {L"orange", RGB(228, 144, 50)},
                                                       {L"green", RGB(76, 166, 108)},
                                                       {L"blue", RGB(84, 141, 221)},
                                                       {L"violet", RGB(153, 114, 220)}};
    int x = r.left, y = (r.top + r.bottom - dpi(14)) / 2;
    if (spec.rfind(L"png:", 0) == 0)
    {
        static std::map<wstring, Pixels> cache;
        if (cache.size() > 128)
            cache.clear();
        auto it = cache.find(spec);
        if (it == cache.end())
            it = cache.emplace(spec, pixels(spec)).first;
        auto &im = it->second;
        if (!im.data.empty())
        {
            for (UINT iy = 0; iy < im.h; iy++)
                for (UINT ix = 0; ix < im.w; ix++)
                {
                    auto p = &im.data[(iy * im.w + ix) * 4];
                    if (p[3] > 100)
                        SetPixelV(dc, x + ix, y + iy, RGB(p[2], p[1], p[0]));
                }
        }
    }
    else
    {
        auto it = colors.find(spec);
        auto c = it == colors.end() ? defaultColor : it->second;
        POINT outline[] = {{x + dpi(1), y + dpi(3)},  {x + dpi(6), y + dpi(3)},   {x + dpi(8), y + dpi(5)},
                           {x + dpi(14), y + dpi(5)}, {x + dpi(14), y + dpi(12)}, {x + dpi(1), y + dpi(12)},
                           {x + dpi(1), y + dpi(3)}};
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, c);
        Polyline(dc, outline, 7);
        MoveToEx(dc, x + dpi(3), y + dpi(7), nullptr);
        LineTo(dc, x + dpi(12), y + dpi(7));
    }
    r.left += dpi(20);
}
} // namespace
extern "C" const wchar_t *ui_icon(const wchar_t *file)
{
    result.clear();
    auto f = factory();
    ComPtr<IWICBitmapDecoder> decoder;
    if (!f || FAILED(f->CreateDecoderFromFilename(file, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        return result.c_str();
    auto image = thumbnail(f.Get(), decoder.Get(), 64);
    if (!image)
        return result.c_str();
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &options)) || FAILED(frame->Initialize(options.Get())))
        return result.c_str();
    UINT w = 0, h = 0;
    image->GetSize(&w, &h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(image.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return result.c_str();
    STATSTG stat{};
    HGLOBAL global = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 131072 ||
        FAILED(GetHGlobalFromStream(stream.Get(), &global)))
        return result.c_str();
    auto bytes = (BYTE *)GlobalLock(global);
    if (bytes)
    {
        DWORD n = 0;
        auto flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
        if (CryptBinaryToStringW(bytes, stat.cbSize.LowPart, flags, nullptr, &n))
        {
            wstring b64(n, 0);
            if (CryptBinaryToStringW(bytes, stat.cbSize.LowPart, flags, b64.data(), &n))
                result = L"png:" + wstring(b64.c_str());
        }
        GlobalUnlock(global);
    }
    return result.c_str();
}
