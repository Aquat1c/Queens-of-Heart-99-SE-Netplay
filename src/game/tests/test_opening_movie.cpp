#include "TestHarness.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include <dshow.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game/OpeningMovieCodec.h"
#include "game/OpeningMovieFrame.h"

using namespace qmovie;

namespace {

constexpr uint32_t kW = 64u;
constexpr uint32_t kH = 48u;

std::vector<uint8_t> flatPlanes(uint8_t y, uint8_t u, uint8_t v) {
    std::vector<uint8_t> planes(planeBytes(kW, kH));
    const size_t ySize = static_cast<size_t>(kW) * kH;
    const size_t cSize = (static_cast<size_t>(kW) / 4u) * (kH / 4u);
    std::memset(planes.data(), y, ySize);
    std::memset(planes.data() + ySize, u, cSize);
    std::memset(planes.data() + ySize + cSize, v, cSize);
    return planes;
}

std::vector<uint8_t> gradientPlanes(uint32_t seed) {
    std::vector<uint8_t> planes(planeBytes(kW, kH));
    for (size_t i = 0; i < planes.size(); ++i)
        planes[i] = static_cast<uint8_t>((i * 7u + seed * 13u + (i >> 5)) & 0xFFu);
    return planes;
}

struct Rgb { int r, g, b; };

Rgb dibPixel(const std::vector<uint8_t>& dib, const FrameDecoder& dec, uint32_t x,
             uint32_t y, uint32_t bpp) {
    const size_t stride = dec.dibStride(bpp);
    const uint8_t* px = dib.data() + (dec.height() - 1u - y) * stride + x * bpp;
    return Rgb{px[2], px[1], px[0]};
}

}

QNP_TEST(opening_movie_frame_round_trips_are_byte_exact) {
    FrameDecoder dec;
    QNP_CHECK(dec.begin(kW, kH));
    std::vector<uint8_t> dib(dec.dibStride(3u) * kH);
    const std::vector<uint8_t> a = gradientPlanes(1u);
    const std::vector<uint8_t> b = gradientPlanes(2u);
    const std::vector<uint8_t> c = gradientPlanes(3u);

    for (Predictor predictor : {Predictor::None, Predictor::Sub, Predictor::Up}) {
        FrameDecoder fresh;
        QNP_CHECK(fresh.begin(kW, kH));
        const std::vector<uint8_t> key =
            encodeFrame(a.data(), kW, kH, FrameKind::Key, predictor, 0u);
        QNP_CHECK(!key.empty());
        bool gap = true;
        QNP_CHECK(fresh.decode(key.data(), key.size(), dib.data(), dib.size(),
                               static_cast<int32_t>(kH), 3u, &gap) == DecodeStatus::Ok);
        QNP_CHECK(!gap);
        QNP_CHECK(fresh.lastPlanes() == a);
    }

    const std::vector<uint8_t> f0 = encodeFrame(a.data(), kW, kH, FrameKind::Key, Predictor::Sub, 0u);
    const std::vector<uint8_t> f1 = encodeFrame(b.data(), kW, kH, FrameKind::Delta, Predictor::None, 1u, a.data());
    const std::vector<uint8_t> f2 = encodeFrame(c.data(), kW, kH, FrameKind::Delta, Predictor::None, 2u, b.data());
    bool gap = false;
    QNP_CHECK(dec.decode(f0.data(), f0.size(), dib.data(), dib.size(), (int32_t)kH, 3u, &gap) == DecodeStatus::Ok);
    QNP_CHECK(dec.decode(f1.data(), f1.size(), dib.data(), dib.size(), (int32_t)kH, 3u, &gap) == DecodeStatus::Ok);
    QNP_CHECK(!gap);
    QNP_CHECK(dec.lastPlanes() == b);
    QNP_CHECK(dec.decode(f2.data(), f2.size(), dib.data(), dib.size(), (int32_t)kH, 3u, &gap) == DecodeStatus::Ok);
    QNP_CHECK(!gap);
    QNP_CHECK(dec.lastPlanes() == c);

    const std::vector<uint8_t> f4 = encodeFrame(a.data(), kW, kH, FrameKind::Delta, Predictor::None, 4u, c.data());
    QNP_CHECK(dec.decode(f4.data(), f4.size(), dib.data(), dib.size(), (int32_t)kH, 3u, &gap) == DecodeStatus::Ok);
    QNP_CHECK(gap);
    QNP_CHECK(dec.lastPlanes() == a);

    std::vector<uint8_t> dib32(dec.dibStride(4u) * kH);
    std::vector<uint8_t> dibTop(dec.dibStride(3u) * kH);
    const std::vector<uint8_t> k = encodeFrame(a.data(), kW, kH, FrameKind::Key, Predictor::Up, 5u);
    QNP_CHECK(dec.decode(k.data(), k.size(), dib.data(), dib.size(), (int32_t)kH, 3u, nullptr) == DecodeStatus::Ok);
    QNP_CHECK(dec.decode(k.data(), k.size(), dib32.data(), dib32.size(), (int32_t)kH, 4u, nullptr) == DecodeStatus::Ok);
    QNP_CHECK(dec.decode(k.data(), k.size(), dibTop.data(), dibTop.size(), -(int32_t)kH, 3u, nullptr) == DecodeStatus::Ok);
    bool same = true;
    for (uint32_t y = 0; y < kH && same; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const Rgb p = dibPixel(dib, dec, x, y, 3u);
            const Rgb q = dibPixel(dib32, dec, x, y, 4u);
            const uint8_t* t = dibTop.data() + y * dec.dibStride(3u) + x * 3u;
            if (p.r != q.r || p.g != q.g || p.b != q.b || t[2] != p.r || t[1] != p.g || t[0] != p.b) {
                same = false;
                break;
            }
        }
    }
    QNP_CHECK(same);
}

QNP_TEST(opening_movie_frame_refuses_damage) {
    FrameDecoder dec;
    QNP_CHECK(dec.begin(kW, kH));
    std::vector<uint8_t> dib(dec.dibStride(3u) * kH);
    const std::vector<uint8_t> planes = gradientPlanes(9u);
    std::vector<uint8_t> key = encodeFrame(planes.data(), kW, kH, FrameKind::Key, Predictor::Sub, 0u);

    const std::vector<uint8_t> delta = encodeFrame(planes.data(), kW, kH, FrameKind::Delta, Predictor::None, 0u, planes.data());
    QNP_CHECK(dec.decode(delta.data(), delta.size(), dib.data(), dib.size(), (int32_t)kH, 3u, nullptr) == DecodeStatus::DeltaWithoutKey);

    std::vector<uint8_t> bad = key;
    bad[0] = 'X';
    QNP_CHECK(dec.decode(bad.data(), bad.size(), dib.data(), dib.size(), (int32_t)kH, 3u, nullptr) == DecodeStatus::BadHeader);

    bad = key;
    const uint32_t wrongRaw = static_cast<uint32_t>(planeBytes(kW * 2u, kH));
    std::memcpy(bad.data() + 12, &wrongRaw, 4);
    QNP_CHECK(dec.decode(bad.data(), bad.size(), dib.data(), dib.size(), (int32_t)kH, 3u, nullptr) == DecodeStatus::BadSize);

    bad.assign(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(key.size() / 2u));
    QNP_CHECK(dec.decode(bad.data(), bad.size(), dib.data(), dib.size(), (int32_t)kH, 3u, nullptr) == DecodeStatus::Inflate);

    QNP_CHECK(dec.decode(key.data(), key.size(), dib.data(), dib.size() - 1u, (int32_t)kH, 3u, nullptr) == DecodeStatus::BadOutput);
    QNP_CHECK(dec.decode(key.data(), key.size(), dib.data(), dib.size(), (int32_t)kH, 2u, nullptr) == DecodeStatus::BadOutput);
    FrameDecoder odd;
    QNP_CHECK(!odd.begin(66u, 48u));
    QNP_CHECK(!odd.begin(0u, 48u));

    QNP_CHECK(dec.decode(key.data(), key.size(), dib.data(), dib.size(), (int32_t)kH, 3u, nullptr) == DecodeStatus::Ok);
    QNP_CHECK(dec.lastPlanes() == planes);
}

QNP_TEST(opening_movie_yuv_to_rgb_matches_bt601) {
    FrameDecoder dec;
    QNP_CHECK(dec.begin(kW, kH));
    std::vector<uint8_t> dib(dec.dibStride(3u) * kH);
    struct Case { uint8_t y, u, v; int r, g, b; };
    const Case cases[] = {
        {235, 128, 128, 255, 255, 255},
        {16, 128, 128, 0, 0, 0},
        {81, 90, 240, 255, 0, 0},
        {145, 54, 34, 0, 255, 0},
        {41, 240, 110, 0, 0, 255},
    };
    for (const Case& c : cases) {
        const std::vector<uint8_t> planes = flatPlanes(c.y, c.u, c.v);
        const std::vector<uint8_t> key = encodeFrame(planes.data(), kW, kH, FrameKind::Key, Predictor::None, 0u);
        QNP_CHECK(dec.decode(key.data(), key.size(), dib.data(), dib.size(), (int32_t)kH, 3u, nullptr) == DecodeStatus::Ok);
        const Rgb centre = dibPixel(dib, dec, kW / 2u, kH / 2u, 3u);
        const Rgb corner = dibPixel(dib, dec, 0u, kH - 1u, 3u);
        QNP_CHECK(std::abs(centre.r - c.r) <= 2 && std::abs(centre.g - c.g) <= 2 && std::abs(centre.b - c.b) <= 2);
        QNP_CHECK(corner.r == centre.r && corner.g == centre.g && corner.b == centre.b);
    }
}

namespace {

void appendChunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& data) {
    out.insert(out.end(), tag, tag + 4);
    const uint32_t size = static_cast<uint32_t>(data.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&size), reinterpret_cast<const uint8_t*>(&size) + 4);
    out.insert(out.end(), data.begin(), data.end());
    if (data.size() & 1u) out.push_back(0u);
}

std::vector<uint8_t> riffList(const char* tag, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.insert(out.end(), {'L', 'I', 'S', 'T'});
    const uint32_t size = static_cast<uint32_t>(body.size() + 4u);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&size), reinterpret_cast<const uint8_t*>(&size) + 4);
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

template <typename T>
void put(std::vector<uint8_t>& out, T value) {
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + sizeof(T));
}

std::vector<uint8_t> buildQ99vAvi(const std::vector<std::vector<uint8_t>>& frames,
                                  const std::vector<bool>& keys) {
    const uint32_t fps = 30u;
    const uint32_t audioPerFrame = 22050u * 2u / fps;
    uint32_t maxVideo = 0u;
    for (const auto& f : frames) maxVideo = std::max<uint32_t>(maxVideo, static_cast<uint32_t>(f.size()));

    std::vector<uint8_t> avih;
    for (uint32_t v : {1000000u / fps, 0u, 0u, 0x110u, static_cast<uint32_t>(frames.size()), 0u, 2u,
                       maxVideo + 8u, kW, kH, 0u, 0u, 0u, 0u})
        put(avih, v);

    std::vector<uint8_t> strhV;
    strhV.insert(strhV.end(), {'v', 'i', 'd', 's', 'Q', '9', '9', 'V'});
    for (uint32_t v : {0u, 0u, 0u, 1u, fps, 0u, static_cast<uint32_t>(frames.size()), maxVideo + 8u, 0xFFFFFFFFu, 0u})
        put(strhV, v);
    for (int16_t v : {int16_t(0), int16_t(0), int16_t(kW), int16_t(kH)}) put(strhV, v);
    std::vector<uint8_t> bih;
    put(bih, uint32_t(40)); put(bih, int32_t(kW)); put(bih, int32_t(kH)); put(bih, uint16_t(1)); put(bih, uint16_t(24));
    put(bih, kQ99VFourcc); put(bih, uint32_t(kW * kH * 3u)); put(bih, int32_t(0)); put(bih, int32_t(0)); put(bih, uint32_t(0)); put(bih, uint32_t(0));
    std::vector<uint8_t> strlV;
    appendChunk(strlV, "strh", strhV);
    appendChunk(strlV, "strf", bih);

    std::vector<uint8_t> strhA;
    strhA.insert(strhA.end(), {'a', 'u', 'd', 's', 0, 0, 0, 0});
    for (uint32_t v : {0u, 0u, 0u, 2u, 44100u, 0u, static_cast<uint32_t>(frames.size()) * audioPerFrame / 2u, audioPerFrame + 8u, 0xFFFFFFFFu, 2u})
        put(strhA, v);
    for (int16_t v : {int16_t(0), int16_t(0), int16_t(0), int16_t(0)}) put(strhA, v);
    std::vector<uint8_t> wfx;
    put(wfx, uint16_t(1)); put(wfx, uint16_t(2)); put(wfx, uint32_t(22050)); put(wfx, uint32_t(44100)); put(wfx, uint16_t(2)); put(wfx, uint16_t(8));
    std::vector<uint8_t> strlA;
    appendChunk(strlA, "strh", strhA);
    appendChunk(strlA, "strf", wfx);

    std::vector<uint8_t> hdrlBody;
    appendChunk(hdrlBody, "avih", avih);
    const std::vector<uint8_t> lv = riffList("strl", strlV);
    const std::vector<uint8_t> la = riffList("strl", strlA);
    hdrlBody.insert(hdrlBody.end(), lv.begin(), lv.end());
    hdrlBody.insert(hdrlBody.end(), la.begin(), la.end());

    std::vector<uint8_t> movi;
    std::vector<uint8_t> idx;
    uint32_t offset = 4u;
    const std::vector<uint8_t> silence(audioPerFrame, 0x80u);
    for (size_t i = 0; i < frames.size(); ++i) {
        idx.insert(idx.end(), {'0', '0', 'd', 'c'});
        put(idx, uint32_t(keys[i] ? 0x10u : 0u)); put(idx, offset); put(idx, static_cast<uint32_t>(frames[i].size()));
        const size_t before = movi.size();
        appendChunk(movi, "00dc", frames[i]);
        offset += static_cast<uint32_t>(movi.size() - before);
        idx.insert(idx.end(), {'0', '1', 'w', 'b'});
        put(idx, uint32_t(0x10u)); put(idx, offset); put(idx, audioPerFrame);
        const size_t beforeA = movi.size();
        appendChunk(movi, "01wb", silence);
        offset += static_cast<uint32_t>(movi.size() - beforeA);
    }

    std::vector<uint8_t> body;
    body.insert(body.end(), {'A', 'V', 'I', ' '});
    const std::vector<uint8_t> hdrl = riffList("hdrl", hdrlBody);
    const std::vector<uint8_t> moviList = riffList("movi", movi);
    body.insert(body.end(), hdrl.begin(), hdrl.end());
    body.insert(body.end(), moviList.begin(), moviList.end());
    appendChunk(body, "idx1", idx);

    std::vector<uint8_t> file;
    file.insert(file.end(), {'R', 'I', 'F', 'F'});
    put(file, static_cast<uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

std::string tempAviPath() {
    char dir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + "qoh99_opening_movie_test_" + std::to_string(GetCurrentProcessId()) + ".avi";
}

bool mciString(const std::string& command, std::string* reply) {
    char buf[256] = {};
    const MCIERROR err = mciSendStringA(command.c_str(), buf, sizeof(buf), nullptr);
    if (reply != nullptr) *reply = buf;
    if (err != 0) {
        char text[256] = {};
        mciGetErrorStringA(err, text, sizeof(text));
        std::printf("    mci '%s' -> error %lu: %s\n", command.c_str(), static_cast<unsigned long>(err), text);
        return false;
    }
    return true;
}

}

QNP_TEST(opening_movie_codec_is_found_and_driven_by_the_real_vfw_stack) {
    QNP_CHECK(registerOpeningMovieCodec());
    QNP_CHECK(openingMovieCodecRegistered());
    QNP_CHECK(registerOpeningMovieCodec());

    HIC byHandler = ICOpen(ICTYPE_VIDEO, kQ99VFourcc, ICMODE_DECOMPRESS);
    QNP_CHECK(byHandler != nullptr);
    if (byHandler != nullptr) ICClose(byHandler);
    BITMAPINFOHEADER in = {};
    in.biSize = sizeof(in); in.biWidth = kW; in.biHeight = kH; in.biPlanes = 1;
    in.biBitCount = 24; in.biCompression = kQ99VFourcc; in.biSizeImage = kW * kH * 3u;
    HIC byFormat = ICLocate(ICTYPE_VIDEO, 0, &in, nullptr, ICMODE_DECOMPRESS);
    QNP_CHECK(byFormat != nullptr);
    if (byFormat != nullptr) {
        BITMAPINFOHEADER out = {};
        QNP_CHECK(ICDecompressGetFormat(byFormat, &in, &out) == ICERR_OK);
        QNP_CHECK(out.biCompression == BI_RGB && out.biBitCount == 24 && out.biWidth == (LONG)kW);
        ICClose(byFormat);
    }

    const std::vector<uint8_t> red = flatPlanes(81, 90, 240);
    const std::vector<uint8_t> blue = flatPlanes(41, 240, 110);
    const std::vector<uint8_t> green = flatPlanes(145, 54, 34);
    std::vector<std::vector<uint8_t>> frames = {
        encodeFrame(red.data(), kW, kH, FrameKind::Key, Predictor::Sub, 0u),
        encodeFrame(blue.data(), kW, kH, FrameKind::Delta, Predictor::None, 1u, red.data()),
        encodeFrame(green.data(), kW, kH, FrameKind::Key, Predictor::Up, 2u),
    };
    const std::vector<uint8_t> avi = buildQ99vAvi(frames, {true, false, true});
    const std::string path = tempAviPath();
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        QNP_CHECK(f != nullptr);
        if (f != nullptr) {
            std::fwrite(avi.data(), 1, avi.size(), f);
            std::fclose(f);
        }
    }

    const OpeningMovieCodecStats before = openingMovieCodecStats();
    AVIFileInit();
    PAVISTREAM stream = nullptr;
    QNP_CHECK(AVIStreamOpenFromFileA(&stream, path.c_str(), streamtypeVIDEO, 0, OF_READ, nullptr) == 0);
    if (stream != nullptr) {
        PGETFRAME getter = AVIStreamGetFrameOpen(stream, nullptr);
        QNP_CHECK(getter != nullptr);
        if (getter != nullptr) {
            const int expected[3][3] = {{255, 0, 0}, {0, 0, 255}, {0, 255, 0}};
            for (LONG i = 0; i < 3; ++i) {
                const BITMAPINFOHEADER* frame = static_cast<const BITMAPINFOHEADER*>(AVIStreamGetFrame(getter, i));
                QNP_CHECK(frame != nullptr);
                if (frame == nullptr) continue;
                QNP_CHECK(frame->biWidth == (LONG)kW && frame->biBitCount == 24);
                const uint8_t* px = reinterpret_cast<const uint8_t*>(frame) + frame->biSize
                    + frame->biClrUsed * sizeof(RGBQUAD);
                const bool match = std::abs(px[2] - expected[i][0]) <= 2 && std::abs(px[1] - expected[i][1]) <= 2
                    && std::abs(px[0] - expected[i][2]) <= 2;
                if (!match) std::printf("    frame %ld pixel bgr=%02X %02X %02X\n", i, px[0], px[1], px[2]);
                QNP_CHECK(match);
            }
            AVIStreamGetFrameClose(getter);
        }
        AVIStreamRelease(stream);
    }
    AVIFileExit();
    const OpeningMovieCodecStats afterAvi = openingMovieCodecStats();
    QNP_CHECK(afterAvi.instancesOpened > before.instancesOpened);
    QNP_CHECK(afterAvi.framesDecoded >= before.framesDecoded + 3u);
    QNP_CHECK(afterAvi.framesRejected == before.framesRejected);

    std::string reply;
    const bool opened = mciString("open \"" + path + "\" type avivideo alias q99v", &reply);
    QNP_CHECK(opened);
    if (opened) {
        mciString("window q99v state hide", nullptr);
        QNP_CHECK(mciString("play q99v", nullptr));
        Sleep(400);
        QNP_CHECK(mciString("status q99v position", &reply));
        std::printf("    MCIAVI position after 400 ms: %s\n", reply.c_str());
        mciString("stop q99v", nullptr);
        QNP_CHECK(mciString("close q99v", nullptr));
        const OpeningMovieCodecStats afterMci = openingMovieCodecStats();
        QNP_CHECK(afterMci.framesDecoded > afterAvi.framesDecoded);
        QNP_CHECK(afterMci.framesRejected == before.framesRejected);
    }
    DeleteFileA(path.c_str());
}

QNP_TEST(opening_movie_codec_renders_through_directshow_like_the_wrapper) {
    QNP_CHECK(registerOpeningMovieCodec());

    const std::vector<uint8_t> red = flatPlanes(81, 90, 240);
    const std::vector<uint8_t> blue = flatPlanes(41, 240, 110);
    std::vector<std::vector<uint8_t>> frames;
    for (uint32_t i = 0; i < 30u; ++i) {
        const std::vector<uint8_t>& planes = (i % 2u) ? blue : red;
        frames.push_back(encodeFrame(planes.data(), kW, kH, FrameKind::Key, Predictor::Sub, i));
    }
    const std::vector<bool> keys(frames.size(), true);
    const std::vector<uint8_t> avi = buildQ99vAvi(frames, keys);
    const std::string path = tempAviPath() + ".ds.avi";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        QNP_REQUIRE(f != nullptr);
        std::fwrite(avi.data(), 1, avi.size(), f);
        std::fclose(f);
    }

    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IGraphBuilder* graph = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_IGraphBuilder, reinterpret_cast<void**>(&graph));
    QNP_REQUIRE(SUCCEEDED(created) && graph != nullptr);

    IBaseFilter* vmr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_VideoMixingRenderer9, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IBaseFilter, reinterpret_cast<void**>(&vmr))) && vmr != nullptr) {
        graph->AddFilter(vmr, L"QOH99 VMR9");
        vmr->Release();
    }

    wchar_t wide[MAX_PATH * 2] = {};
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide, MAX_PATH * 2);
    const OpeningMovieCodecStats before = openingMovieCodecStats();
    const HRESULT rendered = graph->RenderFile(wide, nullptr);
    if (rendered != S_OK) std::printf("    RenderFile -> 0x%08lX\n", static_cast<unsigned long>(rendered));
    QNP_CHECK(rendered == S_OK);

    long width = 0, height = 0;
    IBasicVideo* basic = nullptr;
    if (SUCCEEDED(graph->QueryInterface(IID_IBasicVideo, reinterpret_cast<void**>(&basic))) && basic != nullptr) {
        basic->GetVideoSize(&width, &height);
        basic->Release();
    }
    QNP_CHECK(width == static_cast<long>(kW) && height == static_cast<long>(kH));

    IVideoWindow* window = nullptr;
    if (SUCCEEDED(graph->QueryInterface(IID_IVideoWindow, reinterpret_cast<void**>(&window))) && window != nullptr) {
        window->put_AutoShow(OAFALSE);
        window->put_Visible(OAFALSE);
    }
    IMediaControl* control = nullptr;
    if (SUCCEEDED(graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&control))) && control != nullptr) {
        control->Run();
        Sleep(600);
        control->Stop();
        control->Release();
    }
    const OpeningMovieCodecStats after = openingMovieCodecStats();
    std::printf("    DirectShow decoded %u frames through the codec\n", after.framesDecoded - before.framesDecoded);
    QNP_CHECK(after.framesDecoded > before.framesDecoded);
    QNP_CHECK(after.framesRejected == before.framesRejected);

    if (window != nullptr) window->Release();
    graph->Release();
    if (coInit == S_OK || coInit == S_FALSE) CoUninitialize();
    DeleteFileA(path.c_str());
}

QNP_TEST(opening_movie_system_indeo_driver_registers_per_process) {
    char systemDir[MAX_PATH] = {};
    GetSystemDirectoryA(systemDir, MAX_PATH);
    const std::string shim = std::string(systemDir) + "\\ir50_32.dll";
    if (GetFileAttributesA(shim.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("    no %s on this machine; skipped\n", shim.c_str());
        return;
    }
    const DWORD fcc = mmioFOURCC('I', 'V', '5', '0');
    QNP_CHECK(ICInstall(ICTYPE_VIDEO, fcc, reinterpret_cast<LPARAM>(shim.c_str()), nullptr, ICINSTALL_DRIVER));
    HIC hic = ICOpen(ICTYPE_VIDEO, fcc, ICMODE_DECOMPRESS);
    QNP_CHECK(hic != nullptr);
    if (hic != nullptr) {
        ICINFO info = {};
        info.dwSize = sizeof(info);
        ICGetInfo(hic, &info, sizeof(info));
        std::printf("    Indeo driver: '%ls' from %ls\n", info.szDescription, info.szDriver);
        QNP_CHECK(wcsstr(info.szDescription, L"Indeo") != nullptr);
        ICClose(hic);
    }
    QNP_CHECK(ICRemove(ICTYPE_VIDEO, fcc, 0));
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        char value[MAX_PATH] = {};
        DWORD size = sizeof(value);
        QNP_CHECK(RegQueryValueExA(key, "vidc.iv50", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(value), &size) != ERROR_SUCCESS);
        RegCloseKey(key);
    }
}
