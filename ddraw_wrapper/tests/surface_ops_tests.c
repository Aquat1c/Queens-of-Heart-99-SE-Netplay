#include "surface_ops.h"
#include "qoh99_instrument_policy.h"
#include "qoh99_presentation_policy.h"

#include <limits.h>
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
    int bytes_per_pixel,
    size_t buffer_bytes)
{
    DdwSurfaceView view;
    memset(&view, 0, sizeof(view));
    view.pixels = pixels;
    view.width = width;
    view.height = height;
    view.pitch = pitch;
    view.bytes_per_pixel = bytes_per_pixel;
    view.buffer_bytes = buffer_bytes;
    return view;
}

static int abs_int(int value)
{
    return value < 0 ? -value : value;
}

static void test_surface_format_identity_and_rgb565_conversion(void)
{
    const int grayscale_values[] = {
        1, 2, 3, 7, 16, 23, 25, 33, 35, 50, 60, 66, 70, 75, 80,
        90, 100, 120, 150, 180, 200, 240
    };
    const int red_values[] = { 30, 47, 60, 78, 86, 93, 97, 100 };
    DdwSurfaceFormat rgb565 = ddw_surface_format_rgb565(7u);
    DdwSurfaceFormat rgb555 = ddw_surface_format_rgb555(7u);
    DdwSurfaceFormat a1r5g5b5 = ddw_surface_format_a1r5g5b5(7u);
    DdwSurfaceFormat changed_generation = ddw_surface_format_rgb565(8u);
    DdwSurfaceFormat reconstructed = ddw_surface_format_from_masks(
        16u, 0xF800u, 0x07E0u, 0x001Fu, 0u, 7u);
    unsigned int word;
    size_t i;

    CHECK(ddw_surface_format_is_rgb565(&rgb565));
    CHECK(ddw_surface_format_is_presentable(&rgb565));
    CHECK(ddw_surface_format_is_presentable(&rgb555));
    CHECK(ddw_surface_format_is_presentable(&a1r5g5b5));
    CHECK(!ddw_surface_format_equal(&rgb565, &rgb555));
    CHECK(!ddw_surface_format_equal(&rgb565, &changed_generation));
    CHECK(ddw_surface_format_equal(&rgb565, &reconstructed));

    for (word = 0u; word <= 0xFFFFu; ++word) {
        uint8_t blue;
        uint8_t green;
        uint8_t red;
        DdwRgb8 decoded;

        ddw_rgb565_to_bgrx((uint16_t)word, &blue, &green, &red);
        if (ddw_rgb565_from_bgrx(blue, green, red) != (uint16_t)word) {
            fprintf(stderr, "FAIL RGB565 round trip word=%04X\n", word);
            ++g_failures;
            break;
        }
        CHECK(ddw_surface_format_decode_rgb(&rgb565, (uint16_t)word, &decoded));
        CHECK_EQ(decoded.red, red);
        CHECK_EQ(decoded.green, green);
        CHECK_EQ(decoded.blue, blue);
    }

    for (i = 0; i < sizeof(grayscale_values) / sizeof(grayscale_values[0]); ++i) {
        const uint8_t value = (uint8_t)grayscale_values[i];
        const uint16_t packed = ddw_rgb565_from_bgrx(value, value, value);
        DdwRgb8 decoded;

        CHECK(ddw_surface_format_decode_rgb(&rgb565, packed, &decoded));
        CHECK(abs_int((int)decoded.red - (int)decoded.green) <= 4);
        CHECK(abs_int((int)decoded.green - (int)decoded.blue) <= 4);
    }

    for (i = 0; i < sizeof(red_values) / sizeof(red_values[0]); ++i) {
        const uint8_t value = (uint8_t)red_values[i];
        const uint16_t packed = ddw_rgb565_from_bgrx(0u, 0u, value);
        DdwRgb8 decoded;

        CHECK(ddw_surface_format_decode_rgb(&rgb565, packed, &decoded));
        CHECK(decoded.red > 0u);
        CHECK_EQ(decoded.green, 0u);
        CHECK_EQ(decoded.blue, 0u);
    }

    {
        DdwRgb8 decoded;
        CHECK(ddw_surface_format_decode_rgb(&rgb565, 0xF800u, &decoded));
        CHECK_EQ(decoded.red, 255u);
        CHECK_EQ(decoded.green, 0u);
        CHECK_EQ(decoded.blue, 0u);
        CHECK(ddw_surface_format_decode_rgb(&rgb565, 0x07E0u, &decoded));
        CHECK_EQ(decoded.red, 0u);
        CHECK_EQ(decoded.green, 255u);
        CHECK_EQ(decoded.blue, 0u);
        CHECK(ddw_surface_format_decode_rgb(&rgb565, 0x001Fu, &decoded));
        CHECK_EQ(decoded.red, 0u);
        CHECK_EQ(decoded.green, 0u);
        CHECK_EQ(decoded.blue, 255u);
        CHECK(ddw_surface_format_decode_rgb(&rgb565, 0xFFFFu, &decoded));
        CHECK_EQ(decoded.red, 255u);
        CHECK_EQ(decoded.green, 255u);
        CHECK_EQ(decoded.blue, 255u);
        CHECK(ddw_surface_format_decode_rgb(&rgb565, 0u, &decoded));
        CHECK_EQ(decoded.red, 0u);
        CHECK_EQ(decoded.green, 0u);
        CHECK_EQ(decoded.blue, 0u);
        CHECK(ddw_surface_format_decode_rgb(&rgb555, 0x7C00u, &decoded));
        CHECK_EQ(decoded.red, 255u);
        CHECK_EQ(decoded.green, 0u);
        CHECK_EQ(decoded.blue, 0u);
        CHECK(ddw_surface_format_decode_rgb(&a1r5g5b5, 0xFC00u, &decoded));
        CHECK_EQ(decoded.red, 255u);
    }
}

static void test_surface_layout(void)
{
    DdwSurfaceLayout layout;

    CHECK(ddw_compute_surface_layout(320, 240, 2, &layout));
    CHECK_EQ(layout.pitch, 640);
    CHECK_EQ(layout.buffer_bytes, 153600);

    CHECK(ddw_compute_surface_layout(3, 2, 1, &layout));
    CHECK_EQ(layout.pitch, 4);
    CHECK_EQ(layout.buffer_bytes, 8);

    CHECK(!ddw_compute_surface_layout(0, 2, 1, &layout));
    CHECK(!ddw_compute_surface_layout(2, -1, 1, &layout));
    CHECK(!ddw_compute_surface_layout(2, 2, 4, &layout));
    CHECK(!ddw_compute_surface_layout(INT_MAX, 2, 2, &layout));
}

static void test_fill_color_abi_offset(void)
{
    uint8_t fx[100];
    uint32_t size = (uint32_t)sizeof(fx);
    uint32_t wrong_field = 0x11223344u;
    uint32_t fill_field = 0x55667788u;
    uint32_t actual = 0;

    memset(fx, 0, sizeof(fx));
    memcpy(fx, &size, sizeof(size));
    memcpy(fx + 76, &wrong_field, sizeof(wrong_field));
    memcpy(
        fx + DDW_DDBLTFX_FILL_COLOR_OFFSET,
        &fill_field,
        sizeof(fill_field));

    CHECK(ddw_read_fill_color(fx, &actual));
    CHECK_EQ(actual, fill_field);

    size = DDW_DDBLTFX_FILL_COLOR_END - 1;
    memcpy(fx, &size, sizeof(size));
    CHECK(!ddw_read_fill_color(fx, &actual));

    CHECK(ddw_read_fill_color(NULL, &actual));
    CHECK_EQ(actual, 0);
}

static void test_rectangle_clipping_and_fill(void)
{
    DdwRect clipped;
    DdwRect rect = { -2, -1, 3, 3 };
    uint8_t pixels[16];
    DdwSurfaceView surface = make_surface(pixels, 4, 4, 4, 1, sizeof(pixels));
    int x;
    int y;

    memset(pixels, 0, sizeof(pixels));
    CHECK(ddw_clip_rect(rect, 4, 4, &clipped));
    CHECK_EQ(clipped.left, 0);
    CHECK_EQ(clipped.top, 0);
    CHECK_EQ(clipped.right, 3);
    CHECK_EQ(clipped.bottom, 3);

    CHECK_EQ(ddw_fill_rect(&surface, rect, 7), DDW_OP_OK);
    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            CHECK_EQ(pixels[y * 4 + x], (x < 3 && y < 3) ? 7 : 0);
        }
    }

    rect.left = 8;
    rect.top = 8;
    rect.right = 10;
    rect.bottom = 10;
    CHECK_EQ(ddw_fill_rect(&surface, rect, 9), DDW_OP_EMPTY);
}

static void test_lock_region_validation(void)
{
    uint8_t pixels[32];
    DdwSurfaceView surface = make_surface(pixels, 4, 4, 8, 2, sizeof(pixels));
    DdwRect rect = { 1, 1, 4, 3 };
    DdwLockRegion region;

    CHECK(ddw_validate_lock_region(&surface, &rect, &region));
    CHECK_EQ(region.width, 3);
    CHECK_EQ(region.height, 2);
    CHECK_EQ(region.byte_offset, 10);

    CHECK(ddw_validate_lock_region(&surface, NULL, &region));
    CHECK_EQ(region.byte_offset, 0);
    CHECK_EQ(region.width, 4);
    CHECK_EQ(region.height, 4);

    rect.left = -1;
    CHECK(!ddw_validate_lock_region(&surface, &rect, &region));
    rect.left = 1;
    rect.right = 5;
    CHECK(!ddw_validate_lock_region(&surface, &rect, &region));
    rect.right = rect.left;
    CHECK(!ddw_validate_lock_region(&surface, &rect, &region));
}

static void test_blt_fast_clipping(void)
{
    uint8_t src_pixels[16];
    uint8_t dst_pixels[16];
    DdwSurfaceView src = make_surface(src_pixels, 4, 4, 4, 1, sizeof(src_pixels));
    DdwSurfaceView dst = make_surface(dst_pixels, 4, 4, 4, 1, sizeof(dst_pixels));
    DdwRect src_rect = { 0, 0, 4, 4 };
    DdwCopyRegion region;
    int i;

    for (i = 0; i < 16; ++i)
        src_pixels[i] = (uint8_t)(i + 1);
    memset(dst_pixels, 0, sizeof(dst_pixels));

    CHECK(ddw_clip_blt_fast(src_rect, 4, 4, -2, -1, 4, 4, &region));
    CHECK_EQ(region.src_x, 2);
    CHECK_EQ(region.src_y, 1);
    CHECK_EQ(region.dst_x, 0);
    CHECK_EQ(region.dst_y, 0);
    CHECK_EQ(region.width, 2);
    CHECK_EQ(region.height, 3);

    CHECK_EQ(
        ddw_blt_fast(&dst, -2, -1, &src, src_rect, 0, 0, 0),
        DDW_OP_OK);
    CHECK_EQ(dst_pixels[0], src_pixels[1 * 4 + 2]);
    CHECK_EQ(dst_pixels[1], src_pixels[1 * 4 + 3]);
    CHECK_EQ(dst_pixels[4], src_pixels[2 * 4 + 2]);
    CHECK_EQ(dst_pixels[8], src_pixels[3 * 4 + 2]);

    memset(dst_pixels, 0, sizeof(dst_pixels));
    src_rect.left = -2;
    src_rect.top = 1;
    src_rect.right = 3;
    src_rect.bottom = 3;
    CHECK_EQ(
        ddw_blt_fast(&dst, 0, 0, &src, src_rect, 0, 0, 0),
        DDW_OP_OK);
    CHECK_EQ(dst_pixels[2], src_pixels[4]);
    CHECK_EQ(dst_pixels[3], src_pixels[5]);
    CHECK_EQ(dst_pixels[4], 0);
}

static void test_same_surface_overlap(void)
{
    uint8_t horizontal[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t vertical[16];
    DdwRect source;
    DdwSurfaceView hsurf = make_surface(horizontal, 8, 1, 8, 1, sizeof(horizontal));
    DdwSurfaceView vsurf = make_surface(vertical, 4, 4, 4, 1, sizeof(vertical));
    int i;

    source.left = 0;
    source.top = 0;
    source.right = 6;
    source.bottom = 1;
    CHECK_EQ(
        ddw_blt_fast(&hsurf, 2, 0, &hsurf, source, 0, 0, 0),
        DDW_OP_OK);
    {
        const uint8_t expected[8] = { 0, 1, 0, 1, 2, 3, 4, 5 };
        CHECK(memcmp(horizontal, expected, sizeof(expected)) == 0);
    }

    for (i = 0; i < 16; ++i)
        vertical[i] = (uint8_t)i;
    source.left = 0;
    source.top = 0;
    source.right = 4;
    source.bottom = 3;
    CHECK_EQ(
        ddw_blt_fast(&vsurf, 0, 1, &vsurf, source, 0, 0, 0),
        DDW_OP_OK);
    for (i = 0; i < 4; ++i) {
        CHECK_EQ(vertical[4 + i], i);
        CHECK_EQ(vertical[8 + i], 4 + i);
        CHECK_EQ(vertical[12 + i], 8 + i);
    }
}

static void test_color_key_overlap_direction(void)
{
    uint8_t pixels[8] = { 1, 2, 0, 3, 4, 5, 6, 7 };
    DdwSurfaceView surface = make_surface(pixels, 8, 1, 8, 1, sizeof(pixels));
    DdwRect source = { 0, 0, 6, 1 };
    const uint8_t expected[8] = { 1, 2, 1, 2, 4, 3, 4, 5 };

    CHECK_EQ(
        ddw_blt_fast(&surface, 2, 0, &surface, source, 1, 0, 0),
        DDW_OP_OK);
    CHECK(memcmp(pixels, expected, sizeof(expected)) == 0);
}

static void test_blt_fast_exhaustive_snapshot_semantics(void)
{
    uint8_t before[16];
    uint8_t actual[16];
    uint8_t expected[16];
    DdwSurfaceView surface = make_surface(actual, 4, 4, 4, 1, sizeof(actual));
    int left;
    int top;
    int width;
    int height;
    int dst_x;
    int dst_y;
    int keyed;
    int i;

    for (i = 0; i < 16; ++i)
        before[i] = (uint8_t)(i + 1);

    for (left = -1; left <= 4; ++left) {
        for (top = -1; top <= 4; ++top) {
            for (width = 1; width <= 5; ++width) {
                for (height = 1; height <= 5; ++height) {
                    DdwRect source = {
                        left,
                        top,
                        left + width,
                        top + height
                    };
                    for (dst_x = -2; dst_x <= 5; ++dst_x) {
                        for (dst_y = -2; dst_y <= 5; ++dst_y) {
                            for (keyed = 0; keyed <= 1; ++keyed) {
                                DdwOpResult result;
                                int mapped = 0;
                                int row;
                                int column;

                                memcpy(actual, before, sizeof(actual));
                                memcpy(expected, before, sizeof(expected));
                                for (row = 0; row < height; ++row) {
                                    int sy = top + row;
                                    int dy = dst_y + row;
                                    if (sy < 0 || sy >= 4 || dy < 0 || dy >= 4)
                                        continue;
                                    for (column = 0; column < width; ++column) {
                                        int sx = left + column;
                                        int dx = dst_x + column;
                                        uint8_t pixel;

                                        if (sx < 0 || sx >= 4
                                            || dx < 0 || dx >= 4)
                                            continue;
                                        mapped = 1;
                                        pixel = before[sy * 4 + sx];
                                        if (!keyed || pixel < 5 || pixel > 7)
                                            expected[dy * 4 + dx] = pixel;
                                    }
                                }

                                result = ddw_blt_fast(
                                    &surface,
                                    dst_x,
                                    dst_y,
                                    &surface,
                                    source,
                                    keyed,
                                    5,
                                    7);
                                if (result != (mapped ? DDW_OP_OK : DDW_OP_EMPTY)
                                    || memcmp(actual, expected, sizeof(actual)) != 0) {
                                    fprintf(
                                        stderr,
                                        "FAIL exhaustive BltFast src=(%d,%d %dx%d) "
                                        "dst=(%d,%d) keyed=%d result=%d\n",
                                        left,
                                        top,
                                        width,
                                        height,
                                        dst_x,
                                        dst_y,
                                        keyed,
                                        (int)result);
                                    ++g_failures;
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void test_overlapping_scaled_blt_uses_scratch(void)
{
    uint8_t pixels[16];
    uint8_t scratch[16];
    DdwSurfaceView surface = make_surface(pixels, 4, 4, 4, 1, sizeof(pixels));
    DdwRect src_rect = { 0, 0, 2, 2 };
    DdwRect dst_rect = { 0, 0, 4, 4 };
    int i;

    for (i = 0; i < 16; ++i)
        pixels[i] = (uint8_t)i;

    CHECK_EQ(
        ddw_blt_scratch_bytes(&surface, dst_rect, &surface, src_rect),
        sizeof(pixels));
    CHECK_EQ(
        ddw_blt_nearest(
            &surface, dst_rect, &surface, src_rect, 0, 0, 0, NULL, 0),
        DDW_OP_NEEDS_SCRATCH);
    CHECK_EQ(
        ddw_blt_nearest(
            &surface,
            dst_rect,
            &surface,
            src_rect,
            0,
            0,
            0,
            scratch,
            sizeof(scratch)),
        DDW_OP_OK);

    {
        const uint8_t expected[16] = {
            0, 0, 1, 1,
            0, 0, 1, 1,
            4, 4, 5, 5,
            4, 4, 5, 5
        };
        CHECK(memcmp(pixels, expected, sizeof(expected)) == 0);
    }
}

static void test_qoh99_commit_classifier(void)
{
    DdwRect src = { 0, 0, 320, 240 };
    DdwRect dst = { 960, 491, 1600, 971 };

    CHECK(ddw_is_qoh99_frame_commit(
        1, 0, 640, 480, 320, 240, dst, src));

    src.left = 1;
    src.right = 321;
    CHECK(!ddw_is_qoh99_frame_commit(
        1, 0, 640, 480, 320, 240, dst, src));
    src.left = 0;
    src.right = 320;

    dst.right--;
    CHECK(ddw_is_qoh99_frame_commit(
        1, 0, 640, 480, 320, 240, dst, src));
    dst.right++;

    CHECK(!ddw_is_qoh99_frame_commit(
        0, 0, 640, 480, 320, 240, dst, src));
    CHECK(!ddw_is_qoh99_frame_commit(
        1, 1, 640, 480, 320, 240, dst, src));
    CHECK(!ddw_is_qoh99_frame_commit(
        1, 0, 800, 600, 320, 240, dst, src));

    {
        DdwRect dst11 = { 960, 491, 1280, 731 };
        CHECK(ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, dst11, src));

        DdwRect partialSrc = { 0, 0, 320, 232 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, dst11, partialSrc));
    }

    {
        DdwRect dst320 = { 0, 0, 320, 240 };
        CHECK(ddw_is_qoh99_frame_commit(
            1, 0, 320, 240, 320, 240, dst320, src));
    }

    {
        DdwRect dstAny = { 0, 0, 640, 480 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 1024, 768, 320, 240, dstAny, src));
    }

    {
        DdwRect dstFull = { 0, 0, 640, 480 };
        DdwRect srcFull = { 0, 0, 640, 480 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 640, 480, dstFull, srcFull));
    }

    {
        DdwRect dstResized = { 960, 471, 1776, 1083 };
        CHECK(ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, dstResized, src));
    }

    {
        DdwRect odd = { -37, 12, 1243, 941 };
        CHECK(ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, odd, src));

        DdwRect wide = { 0, 0, 2560, 960 };
        CHECK(ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, wide, src));
    }

    {
        DdwRect dstResized = { 960, 471, 1776, 1083 };
        DdwRect partial = { 8, 8, 312, 232 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, dstResized, partial));

        DdwRect halfWidth = { 0, 0, 160, 240 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, dstResized, halfWidth));
    }

    {
        DdwRect tooSmall = { 0, 0, 319, 240 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, tooSmall, src));

        DdwRect shortSide = { 0, 0, 320, 239 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, shortSide, src));

        DdwRect oneToOne = { 0, 0, 320, 240 };
        CHECK(ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, oneToOne, src));
    }

    {
        DdwRect absurd = { 0, 0, 16385, 480 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, absurd, src));

        DdwRect empty = { 100, 100, 100, 340 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, empty, src));

        DdwRect inverted = { 500, 100, 100, 400 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 640, 480, 320, 240, inverted, src));
    }

    {
        DdwRect plausible = { 0, 0, 1280, 960 };
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 0, 800, 600, 320, 240, plausible, src));
        CHECK(!ddw_is_qoh99_frame_commit(
            0, 0, 640, 480, 320, 240, plausible, src));
        CHECK(!ddw_is_qoh99_frame_commit(
            1, 1, 640, 480, 320, 240, plausible, src));
    }
}

static void test_letterbox_is_one_rule(void)
{
    int x = 0, y = 0, w = 0, h = 0;

    ddw_letterbox(640, 480, 640, 480, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 640 && h == 480);
    ddw_letterbox(816, 612, 640, 480, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == 816 && h == 612);

    ddw_letterbox(2560, 1377, 640, 480, &x, &y, &w, &h);
    CHECK(h == 1377);
    CHECK(w == 1377 * 4 / 3);
    CHECK(x == (2560 - w) / 2);
    CHECK(y == 0);
    CHECK(x > 0);
    CHECK(w * 3 == h * 4);

    ddw_letterbox(640, 1000, 640, 480, &x, &y, &w, &h);
    CHECK(w == 640);
    CHECK(h == 480);
    CHECK(x == 0);
    CHECK(y == (1000 - 480) / 2);
    CHECK(y > 0);

    ddw_letterbox(640, 480, 0, 0, &x, &y, &w, &h);
    CHECK(w >= 0 && h >= 0);

    ddw_letterbox(640, 480, 640, 480, NULL, NULL, NULL, NULL);
}

static void test_instrumentation_requires_exact_opt_in(void)
{
    CHECK(!qoh99_instr_policy_exact_opt_in(NULL));
    CHECK(!qoh99_instr_policy_exact_opt_in(""));
    CHECK(qoh99_instr_policy_exact_opt_in("1"));
    CHECK(!qoh99_instr_policy_exact_opt_in("0"));
    CHECK(!qoh99_instr_policy_exact_opt_in("true"));
    CHECK(!qoh99_instr_policy_exact_opt_in("1 "));
    CHECK(!qoh99_instr_policy_exact_opt_in("01"));
}

static void test_historical_presentation_policy(void)
{
    CHECK(!qoh99_presentation_suppresses_surface_work(
        QOH99_PRESENT_NORMAL));
    CHECK(qoh99_presentation_suppresses_surface_work(
        QOH99_PRESENT_SUPPRESS_HISTORICAL));
    CHECK(!qoh99_presentation_suppresses_surface_work(
        QOH99_PRESENT_CORRECTED_FINAL));
    CHECK(!qoh99_presentation_suppresses_surface_work(99));

    CHECK(qoh99_presentation_executes_surface_work(
        QOH99_PRESENT_NORMAL, 0));
    CHECK(!qoh99_presentation_executes_surface_work(
        QOH99_PRESENT_SUPPRESS_HISTORICAL, 0));
    CHECK(qoh99_presentation_executes_surface_work(
        QOH99_PRESENT_SUPPRESS_HISTORICAL, 1));
    CHECK(qoh99_presentation_executes_surface_work(
        QOH99_PRESENT_CORRECTED_FINAL, 0));
    CHECK(qoh99_presentation_executes_surface_work(99, 0));

    CHECK(qoh99_presentation_executes_pacing(QOH99_PRESENT_NORMAL));
    CHECK(!qoh99_presentation_executes_pacing(
        QOH99_PRESENT_SUPPRESS_HISTORICAL));
    CHECK(qoh99_presentation_executes_pacing(
        QOH99_PRESENT_CORRECTED_FINAL));
    CHECK(qoh99_presentation_executes_pacing(99));
}

int main(void)
{
    test_surface_format_identity_and_rgb565_conversion();
    test_surface_layout();
    test_fill_color_abi_offset();
    test_rectangle_clipping_and_fill();
    test_lock_region_validation();
    test_blt_fast_clipping();
    test_same_surface_overlap();
    test_color_key_overlap_direction();
    test_blt_fast_exhaustive_snapshot_semantics();
    test_overlapping_scaled_blt_uses_scratch();
    test_qoh99_commit_classifier();
    test_letterbox_is_one_rule();
    test_instrumentation_requires_exact_opt_in();
    test_historical_presentation_policy();

    if (g_failures != 0) {
        fprintf(stderr, "%d surface helper checks failed\n", g_failures);
        return EXIT_FAILURE;
    }

    puts("surface helper checks passed");
    return EXIT_SUCCESS;
}
