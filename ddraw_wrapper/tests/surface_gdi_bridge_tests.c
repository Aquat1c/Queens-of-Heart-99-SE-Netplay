#include "surface_gdi_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_EQ(actual, expected) do { \
    long long actual_value = (long long)(actual); \
    long long expected_value = (long long)(expected); \
    if (actual_value != expected_value) { \
        fprintf(stderr, "FAIL %s:%d: %s=%lld expected %lld\n", \
            __FILE__, __LINE__, #actual, actual_value, expected_value); \
        ++g_failures; \
    } \
} while (0)

static DdwSurfaceView make_surface(
    uint8_t *pixels,
    int width,
    int height,
    int pitch,
    DdwSurfaceFormat format,
    size_t buffer_bytes)
{
    DdwSurfaceView view;
    ZeroMemory(&view, sizeof(view));
    view.pixels = pixels;
    view.width = width;
    view.height = height;
    view.pitch = pitch;
    view.bytes_per_pixel = (int)(format.bits_per_pixel / 8u);
    view.buffer_bytes = buffer_bytes;
    view.format = format;
    return view;
}

static uint16_t read_word(const uint8_t *row, int x)
{
    uint16_t word;
    memcpy(&word, row + (size_t)x * 2u, sizeof(word));
    return word;
}

static void write_word(uint8_t *row, int x, uint16_t word)
{
    memcpy(row + (size_t)x * 2u, &word, sizeof(word));
}

static int draw_hollow_ellipse(
    DdwGdiBridge *bridge,
    DdwSurfaceView *surface,
    HDC dc,
    COLORREF color,
    int width,
    int left,
    int top,
    int right,
    int bottom,
    int write_back)
{
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HBRUSH brush = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    int drew;
    DdwGdiBridgeResult released;

    if (!pen || !brush) {
        if (pen)
            DeleteObject(pen);
        return 0;
    }
    drew = SelectObject(dc, pen) != HGDI_ERROR
        && SelectObject(dc, brush) != HGDI_ERROR
        && Ellipse(dc, left, top, right, bottom);
    released = ddw_gdi_bridge_release(bridge, surface, dc, write_back);
    if (!DeleteObject(pen))
        drew = 0;
    return drew && released == DDW_GDI_BRIDGE_OK;
}

static int any_pixel_changed(
    const uint8_t *before,
    const uint8_t *after,
    size_t bytes)
{
    return memcmp(before, after, bytes) != 0;
}

static void test_rgb565_canonical_scratch_and_visible_ellipse(void)
{
    uint8_t pixels[32 * 24 * 2];
    uint8_t before[sizeof(pixels)];
    DdwSurfaceView surface = make_surface(
        pixels, 32, 24, 64, ddw_surface_format_rgb565(1u), sizeof(pixels));
    DdwGdiBridge bridge;
    HDC dc = NULL;

    ZeroMemory(&bridge, sizeof(bridge));
    memset(pixels, 0x39, sizeof(pixels));
    memcpy(before, pixels, sizeof(pixels));

    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    CHECK(dc != NULL);
    CHECK_EQ(ddw_gdi_bridge_scratch_bits_per_pixel(&bridge), 32);
    CHECK_EQ(ddw_gdi_bridge_scratch_compression(&bridge), BI_RGB);
    CHECK_EQ(ddw_gdi_bridge_pitch(&bridge), 32 * 4);
    CHECK(draw_hollow_ellipse(
        &bridge, &surface, dc, RGB(255, 0, 0), 1, 8, 6, 24, 20, 1));
    CHECK(any_pixel_changed(before, pixels, sizeof(pixels)));

    CHECK_EQ(read_word(pixels + (size_t)12 * surface.pitch, 16),
             read_word(before + (size_t)12 * surface.pitch, 16));
    CHECK(memcmp(pixels, before, (size_t)5 * surface.pitch) == 0);
    CHECK(memcmp(pixels + (size_t)21 * surface.pitch,
                 before + (size_t)21 * surface.pitch,
                 (size_t)3 * surface.pitch) == 0);
    ddw_gdi_bridge_destroy(&bridge);
}

static void test_top_centre_sample_hits_hollow_ellipse_after_writeback(void)
{
    enum { WIDTH = 32, HEIGHT = 24, PITCH = WIDTH * 2 };
    const int left = 8;
    const int top = 6;
    const int right = 24;
    const int bottom = 20;
    const int sample_x = left + (right - left) / 2;
    uint8_t pixels[PITCH * HEIGHT];
    uint8_t before[sizeof(pixels)];
    DdwSurfaceView surface = make_surface(
        pixels, WIDTH, HEIGHT, PITCH, ddw_surface_format_rgb565(11u),
        sizeof(pixels));
    DdwGdiBridge bridge;
    HDC dc = NULL;

    ZeroMemory(&bridge, sizeof(bridge));
    memset(pixels, 0x39, sizeof(pixels));
    memcpy(before, pixels, sizeof(pixels));

    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    CHECK(dc != NULL);
    CHECK(draw_hollow_ellipse(
        &bridge, &surface, dc, RGB(255, 0, 0), 1,
        left, top, right, bottom, 1));
    CHECK(bridge.last_restore_success);
    CHECK(bridge.last_flush_success);
    CHECK_EQ(read_word(pixels + (size_t)top * surface.pitch, left),
             read_word(before + (size_t)top * surface.pitch, left));
    CHECK(read_word(pixels + (size_t)top * surface.pitch, sample_x)
          != read_word(before + (size_t)top * surface.pitch, sample_x));
    ddw_gdi_bridge_destroy(&bridge);
}

static void test_historical_discard_and_unequal_pitches(void)
{
    enum { WIDTH = 17, HEIGHT = 12, SURFACE_PITCH = 40 };
    uint8_t pixels[SURFACE_PITCH * HEIGHT];
    uint8_t before[sizeof(pixels)];
    DdwSurfaceView surface = make_surface(
        pixels,
        WIDTH,
        HEIGHT,
        SURFACE_PITCH,
        ddw_surface_format_rgb565(2u),
        sizeof(pixels));
    DdwGdiBridge bridge;
    HDC dc = NULL;

    ZeroMemory(&bridge, sizeof(bridge));
    memset(pixels, 0x55, sizeof(pixels));
    memcpy(before, pixels, sizeof(pixels));

    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    CHECK(ddw_gdi_bridge_pitch(&bridge) != surface.pitch);
    CHECK(draw_hollow_ellipse(
        &bridge, &surface, dc, RGB(0, 255, 0), 3, 2, 1, 15, 10, 0));
    CHECK(memcmp(pixels, before, sizeof(pixels)) == 0);

    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    CHECK(draw_hollow_ellipse(
        &bridge, &surface, dc, RGB(0, 0, 255), 1, 1, 1, 8, 6, 1));
    CHECK(any_pixel_changed(before, pixels, sizeof(pixels)));
    CHECK(memcmp(pixels, before, (size_t)6 * surface.pitch) != 0);
    CHECK(memcmp(pixels + (size_t)8 * surface.pitch,
                 before + (size_t)8 * surface.pitch,
                 (size_t)4 * surface.pitch) == 0);
    ddw_gdi_bridge_destroy(&bridge);
}

static void test_multiple_ellipses_one_transaction_and_no_late_pixels(void)
{
    uint8_t pixels[40 * 30 * 2];
    uint8_t after_first[sizeof(pixels)];
    DdwSurfaceView surface = make_surface(
        pixels, 40, 30, 80, ddw_surface_format_rgb565(3u), sizeof(pixels));
    DdwGdiBridge bridge;
    HDC dc = NULL;
    HPEN red = NULL;
    HPEN blue = NULL;
    HBRUSH brush = (HBRUSH)GetStockObject(HOLLOW_BRUSH);

    ZeroMemory(&bridge, sizeof(bridge));
    memset(pixels, 0, sizeof(pixels));
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    red = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    blue = CreatePen(PS_SOLID, 1, RGB(0, 0, 255));
    CHECK(red != NULL && blue != NULL);
    if (red && blue) {
        CHECK(SelectObject(dc, red) != HGDI_ERROR);
        CHECK(SelectObject(dc, brush) != HGDI_ERROR);
        CHECK(Ellipse(dc, 2, 2, 16, 16));
        CHECK(SelectObject(dc, blue) != HGDI_ERROR);
        CHECK(Ellipse(dc, 22, 10, 37, 25));
    }
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &surface, dc, 1),
             DDW_GDI_BRIDGE_OK);
    CHECK(DeleteObject(red));
    CHECK(DeleteObject(blue));
    CHECK(read_word(pixels + (size_t)2 * surface.pitch, 9) != 0u);
    CHECK(read_word(pixels + (size_t)10 * surface.pitch, 29) != 0u);
    memcpy(after_first, pixels, sizeof(pixels));

    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &surface, dc, 1),
             DDW_GDI_BRIDGE_OK);
    CHECK(memcmp(pixels, after_first, sizeof(pixels)) == 0);
    ddw_gdi_bridge_destroy(&bridge);
}

typedef struct SyncProbe {
    int order;
    int restore_order;
    int flush_order;
    int readback_order;
    BOOL restore_result;
    BOOL flush_result;
} SyncProbe;

static SyncProbe *g_sync_probe = NULL;

static BOOL WINAPI probe_restore(HDC dc, int save_level)
{
    if (g_sync_probe) {
        g_sync_probe->restore_order = ++g_sync_probe->order;
        if (!g_sync_probe->restore_result)
            return FALSE;
    }
    return RestoreDC(dc, save_level);
}

static BOOL WINAPI probe_flush(void)
{
    if (g_sync_probe)
        g_sync_probe->flush_order = ++g_sync_probe->order;
    if (g_sync_probe && !g_sync_probe->flush_result)
        return FALSE;
    return GdiFlush();
}

static void probe_before_readback(void *user_data)
{
    SyncProbe *probe = (SyncProbe *)user_data;
    probe->readback_order = ++probe->order;
}

static void set_scratch_pixel(DdwGdiBridge *bridge, uint8_t blue, uint8_t green,
                              uint8_t red)
{
    uint8_t *bits = (uint8_t *)bridge->bits;
    bits[0] = blue;
    bits[1] = green;
    bits[2] = red;
    bits[3] = 0u;
}

static void test_flush_restore_order_and_fail_closed(void)
{
    uint8_t pixels[8 * 8 * 2];
    uint8_t before[sizeof(pixels)];
    DdwSurfaceView surface = make_surface(
        pixels, 8, 8, 16, ddw_surface_format_rgb565(4u), sizeof(pixels));
    DdwGdiBridge bridge;
    DdwGdiBridgeCallbacks callbacks;
    SyncProbe probe;
    HDC dc = NULL;

    ZeroMemory(&bridge, sizeof(bridge));
    ZeroMemory(&callbacks, sizeof(callbacks));
    ZeroMemory(&probe, sizeof(probe));
    callbacks.restore_dc = probe_restore;
    callbacks.flush = probe_flush;
    callbacks.before_readback = probe_before_readback;
    callbacks.user_data = &probe;
    ddw_gdi_bridge_set_callbacks(&bridge, &callbacks);
    memset(pixels, 0, sizeof(pixels));

    probe.restore_result = TRUE;
    probe.flush_result = TRUE;
    g_sync_probe = &probe;
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    set_scratch_pixel(&bridge, 0u, 0u, 255u);
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &surface, dc, 1),
             DDW_GDI_BRIDGE_OK);
    CHECK(probe.restore_order > 0);
    CHECK(probe.flush_order > probe.restore_order);
    CHECK(probe.readback_order > probe.flush_order);
    CHECK_EQ(read_word(pixels, 0), 0xF800u);

    memcpy(before, pixels, sizeof(pixels));
    ZeroMemory(&probe, sizeof(probe));
    probe.restore_result = TRUE;
    probe.flush_result = FALSE;
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    set_scratch_pixel(&bridge, 255u, 0u, 0u);
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &surface, dc, 1),
             DDW_GDI_BRIDGE_GDI_FAILURE);
    CHECK(memcmp(pixels, before, sizeof(pixels)) == 0);
    CHECK_EQ(probe.readback_order, 0);

    ZeroMemory(&probe, sizeof(probe));
    probe.restore_result = FALSE;
    probe.flush_result = TRUE;
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    set_scratch_pixel(&bridge, 0u, 255u, 0u);
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &surface, dc, 1),
             DDW_GDI_BRIDGE_GDI_FAILURE);
    CHECK(memcmp(pixels, before, sizeof(pixels)) == 0);
    CHECK_EQ(probe.flush_order, 0);
    CHECK_EQ(probe.readback_order, 0);
    g_sync_probe = NULL;
    ddw_gdi_bridge_destroy(&bridge);
}

static void test_identity_rejection_recreation_and_exclusion(void)
{
    uint8_t pixels[16 * 16 * 2];
    uint8_t pixels8[16 * 16];
    DdwSurfaceView rgb565 = make_surface(
        pixels, 16, 16, 32, ddw_surface_format_rgb565(5u), sizeof(pixels));
    DdwSurfaceView rgb565_new_generation = rgb565;
    DdwSurfaceView rgb555 = rgb565;
    DdwSurfaceView a1r5g5b5 = rgb565;
    DdwSurfaceView indexed = make_surface(
        pixels8, 16, 16, 16, ddw_surface_format_index8(5u), sizeof(pixels8));
    DdwGdiBridge bridge;
    HDC dc = NULL;
    HDC nested_dc = NULL;
    HDC wrong = CreateCompatibleDC(NULL);
    int recreated = 0;

    ZeroMemory(&bridge, sizeof(bridge));
    memset(pixels, 0, sizeof(pixels));
    rgb565_new_generation.format.generation = 6u;
    rgb555.format = ddw_surface_format_rgb555(5u);
    a1r5g5b5.format = ddw_surface_format_a1r5g5b5(5u);

    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &rgb565, 1, &dc, NULL), DDW_GDI_BRIDGE_SURFACE_LOCKED);
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &rgb565, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    CHECK(!ddw_gdi_bridge_lock_allowed(&bridge));
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &rgb565, 0, &nested_dc, NULL),
             DDW_GDI_BRIDGE_DC_ALREADY_ACTIVE);
    CHECK(nested_dc == NULL);
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &rgb565, wrong, 1),
             DDW_GDI_BRIDGE_INVALID_DC);
    CHECK(ddw_gdi_bridge_is_active(&bridge));
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &rgb565, dc, 1),
             DDW_GDI_BRIDGE_OK);
    CHECK(ddw_gdi_bridge_lock_allowed(&bridge));

    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &rgb565_new_generation, 0, &dc, &recreated),
             DDW_GDI_BRIDGE_OK);
    CHECK(recreated);
    CHECK_EQ(ddw_gdi_bridge_release(&bridge, &rgb565_new_generation, dc, 1),
             DDW_GDI_BRIDGE_OK);
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &rgb555, 0, &dc, NULL), DDW_GDI_BRIDGE_UNSUPPORTED_FORMAT);
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &a1r5g5b5, 0, &dc, NULL), DDW_GDI_BRIDGE_UNSUPPORTED_FORMAT);
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &indexed, 0, &dc, NULL), DDW_GDI_BRIDGE_UNSUPPORTED_FORMAT);
    CHECK(wrong != NULL);
    if (wrong)
        DeleteDC(wrong);
    ddw_gdi_bridge_destroy(&bridge);
}

static void test_repeated_transactions_and_active_destruction(void)
{
    uint8_t pixels[20 * 16 * 2];
    DdwSurfaceView surface = make_surface(
        pixels, 20, 16, 40, ddw_surface_format_rgb565(7u), sizeof(pixels));
    DdwGdiBridge bridge;
    HDC dc = NULL;
    DWORD handles_before;
    DWORD handles_after;
    int i;

    ZeroMemory(&bridge, sizeof(bridge));
    memset(pixels, 0, sizeof(pixels));
    handles_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (i = 0; i < 32; ++i) {
        CHECK_EQ(ddw_gdi_bridge_acquire(
                     &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
        CHECK_EQ(ddw_gdi_bridge_release(&bridge, &surface, dc, 0),
                 DDW_GDI_BRIDGE_OK);
    }
    CHECK_EQ(ddw_gdi_bridge_acquire(
                 &bridge, &surface, 0, &dc, NULL), DDW_GDI_BRIDGE_OK);
    ddw_gdi_bridge_destroy(&bridge);
    CHECK(!ddw_gdi_bridge_is_active(&bridge));
    handles_after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    CHECK(handles_after <= handles_before + 1u);
}

int main(void)
{
    test_rgb565_canonical_scratch_and_visible_ellipse();
    test_top_centre_sample_hits_hollow_ellipse_after_writeback();
    test_historical_discard_and_unequal_pitches();
    test_multiple_ellipses_one_transaction_and_no_late_pixels();
    test_flush_restore_order_and_fail_closed();
    test_identity_rejection_recreation_and_exclusion();
    test_repeated_transactions_and_active_destruction();

    if (g_failures != 0) {
        fprintf(stderr, "%d surface GDI bridge checks failed\n", g_failures);
        return EXIT_FAILURE;
    }
    puts("surface GDI bridge checks passed");
    return EXIT_SUCCESS;
}
