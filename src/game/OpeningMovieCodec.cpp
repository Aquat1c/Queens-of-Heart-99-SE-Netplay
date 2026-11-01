#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>

#include <atomic>
#include <cstring>
#include <memory>

#include "OpeningMovieCodec.h"
#include "OpeningMovieFrame.h"

namespace qmovie {
namespace {

std::atomic<bool> g_registered{false};
std::atomic<unsigned> g_instancesOpened{0u};
std::atomic<unsigned> g_framesDecoded{0u};
std::atomic<unsigned> g_framesRejected{0u};
std::atomic<unsigned> g_framesOutOfSequence{0u};

struct Instance {
    FrameDecoder decoder;
    uint32_t outputBytesPerPixel = 3u;
    int32_t outputHeight = 0;
    bool begun = false;
};

bool inputAcceptable(const BITMAPINFOHEADER* in) {
    return in != nullptr && in->biCompression == kQ99VFourcc && in->biWidth > 0
        && in->biHeight > 0 && (in->biWidth % 4) == 0 && (in->biHeight % 4) == 0
        && in->biWidth <= 4096 && in->biHeight <= 4096;
}

bool outputAcceptable(const BITMAPINFOHEADER* in, const BITMAPINFOHEADER* out) {
    if (out == nullptr) return true;
    if (out->biCompression != BI_RGB) return false;
    if (out->biBitCount != 24 && out->biBitCount != 32) return false;
    if (out->biPlanes != 1) return false;
    const LONG h = out->biHeight < 0 ? -out->biHeight : out->biHeight;
    return out->biWidth == in->biWidth && h == in->biHeight;
}

void fillOutputFormat(const BITMAPINFOHEADER* in, BITMAPINFOHEADER* out) {
    std::memset(out, 0, sizeof(*out));
    out->biSize = sizeof(BITMAPINFOHEADER);
    out->biWidth = in->biWidth;
    out->biHeight = in->biHeight;
    out->biPlanes = 1;
    out->biBitCount = 24;
    out->biCompression = BI_RGB;
    out->biSizeImage = static_cast<DWORD>(((in->biWidth * 3 + 3) & ~3) * in->biHeight);
}

LRESULT decompress(Instance& inst, ICDECOMPRESS* icd) {
    if (icd == nullptr || icd->lpbiInput == nullptr || icd->lpbiOutput == nullptr
        || icd->lpInput == nullptr || icd->lpOutput == nullptr)
        return ICERR_BADPARAM;
    const BITMAPINFOHEADER* in = icd->lpbiInput;
    const BITMAPINFOHEADER* out = icd->lpbiOutput;
    if (!inst.begun) {
        if (!inputAcceptable(in) || !outputAcceptable(in, out)) return ICERR_BADFORMAT;
        if (!inst.decoder.begin(static_cast<uint32_t>(in->biWidth),
                                static_cast<uint32_t>(in->biHeight)))
            return ICERR_BADFORMAT;
        inst.outputBytesPerPixel = out->biBitCount == 32 ? 4u : 3u;
        inst.outputHeight = out->biHeight;
        inst.begun = true;
    }
    const size_t outBytes = inst.decoder.dibStride(inst.outputBytesPerPixel) * inst.decoder.height();
    bool outOfSequence = false;
    const DecodeStatus status = inst.decoder.decode(
        static_cast<const uint8_t*>(icd->lpInput), in->biSizeImage,
        static_cast<uint8_t*>(icd->lpOutput), outBytes, inst.outputHeight,
        inst.outputBytesPerPixel, &outOfSequence);
    if (status != DecodeStatus::Ok) {
        ++g_framesRejected;
        return status == DecodeStatus::BadOutput ? ICERR_BADFORMAT : ICERR_ERROR;
    }
    ++g_framesDecoded;
    if (outOfSequence) ++g_framesOutOfSequence;
    return ICERR_OK;
}

LRESULT CALLBACK driverProc(DWORD_PTR driverId, HDRVR, UINT message, LPARAM l1, LPARAM l2) {
    Instance* inst = reinterpret_cast<Instance*>(driverId);
    switch (message) {
        case DRV_LOAD:
        case DRV_ENABLE:
        case DRV_DISABLE:
        case DRV_FREE:
            return 1;
        case DRV_OPEN: {
            const ICOPEN* open = reinterpret_cast<const ICOPEN*>(l2);
            if (open != nullptr && open->fccType != ICTYPE_VIDEO) return 0;
            ++g_instancesOpened;
            return reinterpret_cast<LRESULT>(new (std::nothrow) Instance());
        }
        case DRV_CLOSE:
            delete inst;
            return 1;
        case DRV_QUERYCONFIGURE:
            return 0;
        case DRV_CONFIGURE:
            return DRVCNF_OK;
        case ICM_GETINFO: {
            ICINFO* info = reinterpret_cast<ICINFO*>(l1);
            if (info == nullptr || static_cast<size_t>(l2) < sizeof(ICINFO)) return 0;
            std::memset(info, 0, sizeof(*info));
            info->dwSize = sizeof(ICINFO);
            info->fccType = ICTYPE_VIDEO;
            info->fccHandler = kQ99VFourcc;
            info->dwFlags = 0;
            info->dwVersion = 1;
            info->dwVersionICM = ICVERSION;
            wcscpy_s(info->szName, L"Q99V");
            wcscpy_s(info->szDescription, L"QOH99 netplay lossless opening movie");
            return sizeof(ICINFO);
        }
        case ICM_DECOMPRESS_QUERY: {
            const BITMAPINFOHEADER* in = reinterpret_cast<const BITMAPINFOHEADER*>(l1);
            const BITMAPINFOHEADER* out = reinterpret_cast<const BITMAPINFOHEADER*>(l2);
            return inputAcceptable(in) && outputAcceptable(in, out) ? ICERR_OK : ICERR_BADFORMAT;
        }
        case ICM_DECOMPRESS_GET_FORMAT: {
            const BITMAPINFOHEADER* in = reinterpret_cast<const BITMAPINFOHEADER*>(l1);
            if (!inputAcceptable(in)) return ICERR_BADFORMAT;
            if (l2 == 0) return sizeof(BITMAPINFOHEADER);
            fillOutputFormat(in, reinterpret_cast<BITMAPINFOHEADER*>(l2));
            return ICERR_OK;
        }
        case ICM_DECOMPRESS_BEGIN: {
            if (inst == nullptr) return ICERR_BADPARAM;
            const BITMAPINFOHEADER* in = reinterpret_cast<const BITMAPINFOHEADER*>(l1);
            const BITMAPINFOHEADER* out = reinterpret_cast<const BITMAPINFOHEADER*>(l2);
            if (!inputAcceptable(in) || !outputAcceptable(in, out)) return ICERR_BADFORMAT;
            if (!inst->decoder.begin(static_cast<uint32_t>(in->biWidth),
                                     static_cast<uint32_t>(in->biHeight)))
                return ICERR_BADFORMAT;
            inst->outputBytesPerPixel = (out != nullptr && out->biBitCount == 32) ? 4u : 3u;
            inst->outputHeight = out != nullptr ? out->biHeight : in->biHeight;
            inst->begun = true;
            return ICERR_OK;
        }
        case ICM_DECOMPRESS:
            if (inst == nullptr) return ICERR_BADPARAM;
            return decompress(*inst, reinterpret_cast<ICDECOMPRESS*>(l1));
        case ICM_DECOMPRESS_END:
            if (inst != nullptr) inst->begun = false;
            return ICERR_OK;
        default:
            return ICERR_UNSUPPORTED;
    }
}

}

bool registerOpeningMovieCodec() {
    if (g_registered.load()) return true;
    HMODULE msvfw = LoadLibraryW(L"msvfw32.dll");
    if (msvfw == nullptr) return false;
    typedef BOOL(VFWAPI * InstallFn)(DWORD, DWORD, LPARAM, LPSTR, UINT);
    const InstallFn install = reinterpret_cast<InstallFn>(GetProcAddress(msvfw, "ICInstall"));
    if (install == nullptr) return false;
    if (!install(ICTYPE_VIDEO, kQ99VFourcc, reinterpret_cast<LPARAM>(&driverProc), nullptr,
                 ICINSTALL_FUNCTION))
        return false;
    g_registered.store(true);
    return true;
}

bool openingMovieCodecRegistered() { return g_registered.load(); }

OpeningMovieCodecStats openingMovieCodecStats() {
    OpeningMovieCodecStats s;
    s.instancesOpened = g_instancesOpened.load();
    s.framesDecoded = g_framesDecoded.load();
    s.framesRejected = g_framesRejected.load();
    s.framesOutOfSequence = g_framesOutOfSequence.load();
    return s;
}

}
