#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef HRESULT (WINAPI *CreateDraw)(GUID *, LPDIRECTDRAW *, IUnknown *);
typedef void (__cdecl *SetDisposition)(DWORD);

#define REQUIRE(expr) do {                                                \
    if (!(expr)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);    \
        return EXIT_FAILURE;                                             \
    }                                                                    \
} while (0)

static HRESULT fill(LPDIRECTDRAWSURFACE surface, DWORD color)
{
    DDBLTFX fx = {0};
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = color;
    return IDirectDrawSurface_Blt(
        surface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}

static int has_color(LPDIRECTDRAWSURFACE surface, uint16_t color)
{
    DDSURFACEDESC desc = {0};
    DWORD x, y;
    int matches = 1;
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface_Lock(
            surface, NULL, &desc, DDLOCK_READONLY | DDLOCK_WAIT, NULL)))
        return 0;
    if (desc.ddpfPixelFormat.dwRGBBitCount != 16u) matches = 0;
    for (y = 0u; y < desc.dwHeight; ++y) {
        const uint16_t *row = (const uint16_t *)(
            (const unsigned char *)desc.lpSurface + y * desc.lPitch);
        for (x = 0u; x < desc.dwWidth; ++x)
            if (row[x] != color) matches = 0;
    }
    if (FAILED(IDirectDrawSurface_Unlock(surface, desc.lpSurface))) return 0;
    return matches;
}

int main(int argc, char **argv)
{
    HMODULE stub, wrapper;
    CreateDraw create_draw;
    SetDisposition set_disposition;
    LPDIRECTDRAW draw = NULL;
    LPDIRECTDRAWSURFACE resource = NULL, frame = NULL, primary = NULL;
    LPDIRECTDRAWSURFACE back = NULL;
    DDSURFACEDESC desc = {0};
    DDSCAPS back_caps = {DDSCAPS_BACKBUFFER};
    DDBLTFX invalid_fx = {0};
    RECT full = {0, 0, 320, 240};

    REQUIRE(argc == 3);
    stub = LoadLibraryA(argv[2]);
    REQUIRE(stub != NULL);
    set_disposition = (SetDisposition)GetProcAddress(stub, "test_set_disposition");
    REQUIRE(set_disposition != NULL);
    wrapper = LoadLibraryA(argv[1]);
    REQUIRE(wrapper != NULL);
    create_draw = (CreateDraw)GetProcAddress(wrapper, "DirectDrawCreate");
    REQUIRE(create_draw != NULL);
    REQUIRE(SUCCEEDED(create_draw(NULL, &draw, NULL)));
    REQUIRE(SUCCEEDED(IDirectDraw_SetDisplayMode(draw, 320, 240, 16)));

    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.dwWidth = 320;
    desc.dwHeight = 240;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    REQUIRE(SUCCEEDED(IDirectDraw_CreateSurface(draw, &desc, &resource, NULL)));
    REQUIRE(SUCCEEDED(IDirectDraw_CreateSurface(draw, &desc, &frame, NULL)));
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    REQUIRE(SUCCEEDED(IDirectDraw_CreateSurface(draw, &desc, &primary, NULL)));
    REQUIRE(SUCCEEDED(IDirectDrawSurface_GetAttachedSurface(primary, &back_caps, &back)));

    REQUIRE(SUCCEEDED(fill(primary, 0x07E0u)));
    REQUIRE(SUCCEEDED(fill(back, 0x07E0u)));
    REQUIRE(SUCCEEDED(fill(frame, 0x07E0u)));
    REQUIRE(SUCCEEDED(IDirectDrawSurface_BltFast(
        primary, 0, 0, frame, &full, DDBLTFAST_NOCOLORKEY)));

    set_disposition(1u);
    REQUIRE(SUCCEEDED(fill(resource, 0xF81Fu)));
    REQUIRE(has_color(resource, 0xF81Fu));
    REQUIRE(FAILED(IDirectDrawSurface_Blt(
        resource, NULL, NULL, NULL, DDBLT_COLORFILL, &invalid_fx)));
    REQUIRE(SUCCEEDED(fill(primary, 0u)));
    REQUIRE(SUCCEEDED(fill(back, 0u)));
    REQUIRE(SUCCEEDED(fill(frame, 0u)));
    REQUIRE(has_color(primary, 0x07E0u));
    REQUIRE(has_color(back, 0x07E0u));
    REQUIRE(has_color(frame, 0x07E0u));

    set_disposition(0u);
    REQUIRE(SUCCEEDED(IDirectDrawSurface_BltFast(
        frame, 0, 0, resource, &full, DDBLTFAST_NOCOLORKEY)));
    REQUIRE(has_color(frame, 0xF81Fu));
    REQUIRE(SUCCEEDED(fill(resource, 0x001Fu)));
    REQUIRE(has_color(resource, 0x001Fu));
    set_disposition(2u);
    REQUIRE(SUCCEEDED(fill(frame, 0xFFFFu)));
    REQUIRE(has_color(frame, 0xFFFFu));

    IDirectDrawSurface_Release(back);
    IDirectDrawSurface_Release(primary);
    IDirectDrawSurface_Release(frame);
    IDirectDrawSurface_Release(resource);
    IDirectDraw_Release(draw);
    FreeLibrary(wrapper);
    FreeLibrary(stub);
    puts("hidden resource fills: production DirectDraw checks passed");
    return EXIT_SUCCESS;
}
