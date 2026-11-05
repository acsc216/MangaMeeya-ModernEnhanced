/*
 * webp.spi - WebP image plug-in for MangaMeeya (Susie 32-bit SPI)
 *
 * MangaMeeya 7.4 has no built-in WebP support.  This plug-in implements the
 * Susie import-filter API (00IN) and decodes WebP through Windows Imaging
 * Component (WIC).  Windows 11 includes the WebP codec; on Windows 10 it is
 * provided by the Microsoft Store package "WebP Image Extensions".
 *
 * The image is returned as a 24bpp bottom-up DIB.  WebP alpha is flattened
 * onto white because MangaMeeya's Susie image path only uses the RGB
 * channels (the built-in PNG/GIF paths composite alpha, but the Susie path
 * does not).
 *
 * Build (32-bit):
 *   cl /nologo /LD /MT /O2 /W0 webp.c /link /DEF:webp.def windowscodecs.lib ole32.lib user32.lib /OUT:webp.spi
 *
 * Install: copy webp.spi into the SusiePlugin folder (see [Susie] PluginPath).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <wincodec.h>
#include <string.h>

typedef struct {
    long left, top;
    long width, height;
    WORD x_density, y_density;
    short colorDepth;
    HLOCAL hInfo;
} PictureInfo;

typedef struct {
    BYTE* data;
    DWORD size;
    BOOL own;
} SPIINPUT;

static BOOL load_input(LPSTR buf, long len, UINT flag, SPIINPUT* in)
{
    in->data = NULL; in->size = 0; in->own = FALSE;
    if ((flag & 7) == 1) {
        in->data = (BYTE*)buf;
        in->size = (DWORD)len;
        return TRUE;
    }
    {
        HANDLE h = CreateFileA(buf, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD size, got = 0;
        BYTE* data;
        if (h == INVALID_HANDLE_VALUE)
            return FALSE;
        size = GetFileSize(h, NULL);
        if (size == INVALID_FILE_SIZE) { CloseHandle(h); return FALSE; }
        if (len > 0) {
            if (SetFilePointer(h, len, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
                CloseHandle(h); return FALSE;
            }
            size -= (DWORD)len;
        }
        data = (BYTE*)LocalAlloc(LMEM_FIXED, size ? size : 1);
        if (!data) { CloseHandle(h); return FALSE; }
        if (!ReadFile(h, data, size, &got, NULL)) got = 0;
        CloseHandle(h);
        in->data = data; in->size = got; in->own = TRUE;
        return TRUE;
    }
}

static void free_input(SPIINPUT* in)
{
    if (in->own && in->data) LocalFree(in->data);
    in->data = NULL;
}

static BOOL decode_memory(const BYTE* data, DWORD size, UINT* pw, UINT* ph,
                          BYTE** ppBits, UINT* pStride)
{
    IWICImagingFactory* factory = NULL;
    IWICStream* stream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;
    HRESULT hr;
    BOOL uninit = FALSE;

    *pw = *ph = 0;
    if (ppBits) *ppBits = NULL;
    if (pStride) *pStride = 0;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    else if (SUCCEEDED(hr)) uninit = TRUE;
    if (FAILED(hr)) return FALSE;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void**)&factory);
    if (FAILED(hr)) goto done;

    hr = factory->lpVtbl->CreateStream(factory, &stream);
    if (FAILED(hr)) goto done;
    hr = stream->lpVtbl->InitializeFromMemory(stream, (BYTE*)data, size);
    if (FAILED(hr)) goto done;
    hr = factory->lpVtbl->CreateDecoderFromStream(factory, (IStream*)stream, NULL,
                                                  WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) goto done;
    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) goto done;
    hr = frame->lpVtbl->GetSize(frame, pw, ph);
    if (FAILED(hr)) goto done;

    if (ppBits) {
        UINT srcstride = (*pw) * 4;
        UINT srcsize = srcstride * (*ph);
        UINT stride = ((*pw) * 3 + 3) & ~3u;
        UINT dstsize = stride * (*ph);
        BYTE* src = (BYTE*)LocalAlloc(LMEM_FIXED, srcsize ? srcsize : 1);
        BYTE* dst = NULL;
        BOOL alpha = FALSE;
        UINT x, y;

        if (!src) { hr = E_OUTOFMEMORY; goto done; }
        hr = factory->lpVtbl->CreateFormatConverter(factory, &conv);
        if (SUCCEEDED(hr))
            hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource*)frame,
                                          &GUID_WICPixelFormat32bppBGRA,
                                          WICBitmapDitherTypeNone, NULL, 0.0,
                                          WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr))
            hr = conv->lpVtbl->CopyPixels(conv, NULL, srcstride, srcsize, src);
        if (FAILED(hr)) { LocalFree(src); goto done; }

        for (y = 0; y < (*ph) && !alpha; y++) {
            BYTE* p = src + y * srcstride + 3;
            for (x = 0; x < (*pw); x++, p += 4)
                if (*p != 255) { alpha = TRUE; break; }
        }

        dst = (BYTE*)LocalAlloc(LMEM_FIXED, dstsize ? dstsize : 1);
        if (!dst) { LocalFree(src); hr = E_OUTOFMEMORY; goto done; }
        for (y = 0; y < (*ph); y++) {
            BYTE* s = src + y * srcstride;
            BYTE* d = dst + ((*ph) - 1 - y) * stride;
            for (x = 0; x < (*pw); x++, s += 4, d += 3) {
                if (alpha) {
                    UINT a = s[3];
                    d[0] = (BYTE)((s[0] * a + 255 * (255 - a) + 127) / 255);
                    d[1] = (BYTE)((s[1] * a + 255 * (255 - a) + 127) / 255);
                    d[2] = (BYTE)((s[2] * a + 255 * (255 - a) + 127) / 255);
                } else {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                }
            }
        }
        LocalFree(src);
        *ppBits = dst;
        if (pStride) *pStride = stride;
    }

done:
    if (conv) conv->lpVtbl->Release(conv);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    if (stream) stream->lpVtbl->Release(stream);
    if (factory) factory->lpVtbl->Release(factory);
    if (uninit) CoUninitialize();
    return SUCCEEDED(hr);
}

static int copy_info(const char* s, LPSTR buf, int buflen)
{
    int n = (int)strlen(s);
    if (buflen <= 0) return 0;
    if (n > buflen - 1) n = buflen - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return n;
}

__declspec(dllexport) int WINAPI GetPluginInfo(int infono, LPSTR buf, int buflen)
{
    switch (infono) {
    case 0: return copy_info("00IN", buf, buflen);
    case 1: return copy_info("WebP Image Plug-in v1.0 (WIC)", buf, buflen);
    case 2: return copy_info("*.WEBP", buf, buflen);
    case 3: return copy_info("WebP Image", buf, buflen);
    default: return 0;
    }
}

__declspec(dllexport) int WINAPI IsSupported(LPSTR filename, DWORD dw)
{
    BYTE hdr[12];
    DWORD got = 0;
    (void)filename;
    if (HIWORD(dw)) {
        memcpy(hdr, (BYTE*)dw, 12);
    } else {
        HANDLE h = (HANDLE)dw;
        DWORD pos;
        if (h == NULL || h == INVALID_HANDLE_VALUE) return 0;
        pos = SetFilePointer(h, 0, NULL, FILE_CURRENT);
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        if (!ReadFile(h, hdr, 12, &got, NULL) || got < 12) {
            SetFilePointer(h, pos, NULL, FILE_BEGIN);
            return 0;
        }
        SetFilePointer(h, pos, NULL, FILE_BEGIN);
    }
    return memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WEBP", 4) == 0;
}

__declspec(dllexport) int WINAPI GetPictureInfo(LPSTR buf, long len, UINT flag, PictureInfo* info)
{
    SPIINPUT in;
    UINT w = 0, h = 0;
    int rc;
    if (!info) return 8;
    if (!load_input(buf, len, flag, &in)) return 6;
    rc = decode_memory(in.data, in.size, &w, &h, NULL, NULL) ? 0 : 2;
    free_input(&in);
    if (rc) return rc;
    ZeroMemory(info, sizeof(*info));
    info->width = (long)w;
    info->height = (long)h;
    info->x_density = 96;
    info->y_density = 96;
    info->colorDepth = 24;
    return 0;
}

__declspec(dllexport) int WINAPI GetPicture(LPSTR buf, long len, UINT flag,
                                           HANDLE* pHBInfo, HANDLE* pHBm,
                                           FARPROC progress, long data)
{
    SPIINPUT in;
    UINT w = 0, h = 0, stride = 0;
    BYTE* bits = NULL;
    BITMAPINFO* bi;
    (void)progress; (void)data;
    if (!pHBInfo || !pHBm) return 8;
    *pHBInfo = NULL;
    *pHBm = NULL;
    if (!load_input(buf, len, flag, &in)) return 6;
    if (!decode_memory(in.data, in.size, &w, &h, &bits, &stride)) {
        free_input(&in);
        return 2;
    }
    free_input(&in);
    bi = (BITMAPINFO*)LocalAlloc(LMEM_FIXED, sizeof(BITMAPINFO));
    if (!bi) { LocalFree(bits); return 4; }
    ZeroMemory(bi, sizeof(BITMAPINFO));
    bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth = (LONG)w;
    bi->bmiHeader.biHeight = (LONG)h;
    bi->bmiHeader.biPlanes = 1;
    bi->bmiHeader.biBitCount = 24;
    bi->bmiHeader.biCompression = BI_RGB;
    bi->bmiHeader.biSizeImage = 0;
    *pHBInfo = (HANDLE)bi;
    *pHBm = (HANDLE)bits;
    return 0;
}

__declspec(dllexport) int WINAPI GetPreview(LPSTR buf, long len, UINT flag,
                                            HANDLE* pHBInfo, HANDLE* pHBm,
                                            FARPROC progress, long data)
{
    (void)buf; (void)len; (void)flag; (void)pHBInfo; (void)pHBm;
    (void)progress; (void)data;
    return -1;
}

BOOL WINAPI DllMain(HMODULE hm, DWORD reason, LPVOID p)
{
    (void)p;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hm);
    return TRUE;
}
