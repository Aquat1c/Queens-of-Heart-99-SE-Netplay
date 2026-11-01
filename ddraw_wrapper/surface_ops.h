#ifndef QOH99_DDRAW_SURFACE_OPS_H
#define QOH99_DDRAW_SURFACE_OPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In the 32-bit DirectDraw ABI, DDBLTFX::dwFillColor is the DWORD at byte offset 80. */
#define DDW_DDBLTFX_FILL_COLOR_OFFSET 80u
#define DDW_DDBLTFX_FILL_COLOR_END    84u

typedef struct DdwRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} DdwRect;

typedef struct DdwSurfaceLayout {
    int width;
    int height;
    int bytes_per_pixel;
    int pitch;
    size_t buffer_bytes;
} DdwSurfaceLayout;

/* RGB565, RGB555 and A1R5G5B5 are all two bytes but differ in layout; never select a format from bytes-per-pixel. */
typedef enum DdwPixelFormatKind {
    DDW_PIXEL_FORMAT_UNKNOWN = 0,
    DDW_PIXEL_FORMAT_INDEX8,
    DDW_PIXEL_FORMAT_RGB565,
    DDW_PIXEL_FORMAT_RGB555,
    DDW_PIXEL_FORMAT_A1R5G5B5
} DdwPixelFormatKind;

typedef struct DdwSurfaceFormat {
    DdwPixelFormatKind kind;
    uint32_t bits_per_pixel;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t alpha_mask;
    uint32_t generation;
} DdwSurfaceFormat;

typedef struct DdwRgb8 {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} DdwRgb8;

typedef struct DdwSurfaceView {
    uint8_t *pixels;
    int width;
    int height;
    int pitch;
    int bytes_per_pixel;
    size_t buffer_bytes;
    DdwSurfaceFormat format;
} DdwSurfaceView;

typedef struct DdwLockRegion {
    DdwRect rect;
    int width;
    int height;
    size_t byte_offset;
} DdwLockRegion;

typedef struct DdwCopyRegion {
    int src_x;
    int src_y;
    int dst_x;
    int dst_y;
    int width;
    int height;
} DdwCopyRegion;

typedef enum DdwOpResult {
    DDW_OP_INVALID = -1,
    DDW_OP_EMPTY = 0,
    DDW_OP_OK = 1,
    DDW_OP_NEEDS_SCRATCH = 2
} DdwOpResult;

DdwSurfaceFormat ddw_surface_format_index8(uint32_t generation);
DdwSurfaceFormat ddw_surface_format_rgb565(uint32_t generation);
DdwSurfaceFormat ddw_surface_format_rgb555(uint32_t generation);
DdwSurfaceFormat ddw_surface_format_a1r5g5b5(uint32_t generation);
DdwSurfaceFormat ddw_surface_format_from_masks(
    uint32_t bits_per_pixel,
    uint32_t red_mask,
    uint32_t green_mask,
    uint32_t blue_mask,
    uint32_t alpha_mask,
    uint32_t generation);
int ddw_surface_format_equal(
    const DdwSurfaceFormat *left,
    const DdwSurfaceFormat *right);
int ddw_surface_format_is_rgb565(const DdwSurfaceFormat *format);
int ddw_surface_format_is_presentable(const DdwSurfaceFormat *format);

/* Canonical bridge conversion.  BGRX is byte order blue, green, red, zero. */
void ddw_rgb565_to_bgrx(
    uint16_t rgb565,
    uint8_t *out_blue,
    uint8_t *out_green,
    uint8_t *out_red);
uint16_t ddw_rgb565_from_bgrx(uint8_t blue, uint8_t green, uint8_t red);

int ddw_surface_format_decode_rgb(
    const DdwSurfaceFormat *format,
    uint16_t packed,
    DdwRgb8 *out_rgb);

int ddw_compute_surface_layout(
    int width,
    int height,
    int bytes_per_pixel,
    DdwSurfaceLayout *out_layout);

int ddw_clip_rect(
    DdwRect rect,
    int width,
    int height,
    DdwRect *out_rect);

int ddw_validate_lock_region(
    const DdwSurfaceView *surface,
    const DdwRect *requested_rect,
    DdwLockRegion *out_region);

int ddw_read_fill_color(const void *fx, uint32_t *out_fill_color);

DdwOpResult ddw_fill_rect(
    DdwSurfaceView *dst,
    DdwRect rect,
    uint32_t fill_color);

int ddw_clip_blt_fast(
    DdwRect src_rect,
    int src_width,
    int src_height,
    int32_t dst_x,
    int32_t dst_y,
    int dst_width,
    int dst_height,
    DdwCopyRegion *out_region);

DdwOpResult ddw_blt_fast(
    DdwSurfaceView *dst,
    int32_t dst_x,
    int32_t dst_y,
    const DdwSurfaceView *src,
    DdwRect src_rect,
    int use_source_color_key,
    uint32_t color_key_low,
    uint32_t color_key_high);

size_t ddw_blt_scratch_bytes(
    const DdwSurfaceView *dst,
    DdwRect dst_rect,
    const DdwSurfaceView *src,
    DdwRect src_rect);

DdwOpResult ddw_blt_nearest(
    DdwSurfaceView *dst,
    DdwRect dst_rect,
    const DdwSurfaceView *src,
    DdwRect src_rect,
    int use_source_color_key,
    uint32_t color_key_low,
    uint32_t color_key_high,
    void *scratch,
    size_t scratch_bytes);

void ddw_letterbox(int screenW, int screenH, int gameW, int gameH,
                   int *outX, int *outY, int *outW, int *outH);

int ddw_is_qoh99_frame_commit(
    int dst_is_primary,
    int src_is_primary,
    int dst_width,
    int dst_height,
    int src_width,
    int src_height,
    DdwRect dst_rect,
    DdwRect src_rect);

#ifdef __cplusplus
}
#endif

#endif
