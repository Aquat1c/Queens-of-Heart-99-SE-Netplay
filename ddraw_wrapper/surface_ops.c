#include "surface_ops.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static DdwSurfaceFormat ddw_surface_format_make(
    DdwPixelFormatKind kind,
    uint32_t bits_per_pixel,
    uint32_t red_mask,
    uint32_t green_mask,
    uint32_t blue_mask,
    uint32_t alpha_mask,
    uint32_t generation)
{
    DdwSurfaceFormat format;

    format.kind = kind;
    format.bits_per_pixel = bits_per_pixel;
    format.red_mask = red_mask;
    format.green_mask = green_mask;
    format.blue_mask = blue_mask;
    format.alpha_mask = alpha_mask;
    format.generation = generation;
    return format;
}

DdwSurfaceFormat ddw_surface_format_index8(uint32_t generation)
{
    return ddw_surface_format_make(
        DDW_PIXEL_FORMAT_INDEX8, 8u, 0u, 0u, 0u, 0u, generation);
}

DdwSurfaceFormat ddw_surface_format_rgb565(uint32_t generation)
{
    return ddw_surface_format_make(
        DDW_PIXEL_FORMAT_RGB565,
        16u,
        0xF800u,
        0x07E0u,
        0x001Fu,
        0u,
        generation);
}

DdwSurfaceFormat ddw_surface_format_rgb555(uint32_t generation)
{
    return ddw_surface_format_make(
        DDW_PIXEL_FORMAT_RGB555,
        16u,
        0x7C00u,
        0x03E0u,
        0x001Fu,
        0u,
        generation);
}

DdwSurfaceFormat ddw_surface_format_a1r5g5b5(uint32_t generation)
{
    return ddw_surface_format_make(
        DDW_PIXEL_FORMAT_A1R5G5B5,
        16u,
        0x7C00u,
        0x03E0u,
        0x001Fu,
        0x8000u,
        generation);
}

DdwSurfaceFormat ddw_surface_format_from_masks(
    uint32_t bits_per_pixel,
    uint32_t red_mask,
    uint32_t green_mask,
    uint32_t blue_mask,
    uint32_t alpha_mask,
    uint32_t generation)
{
    DdwSurfaceFormat format = ddw_surface_format_make(
        DDW_PIXEL_FORMAT_UNKNOWN,
        bits_per_pixel,
        red_mask,
        green_mask,
        blue_mask,
        alpha_mask,
        generation);

    if (bits_per_pixel == 8u && red_mask == 0u && green_mask == 0u
        && blue_mask == 0u && alpha_mask == 0u) {
        format.kind = DDW_PIXEL_FORMAT_INDEX8;
    } else if (bits_per_pixel == 16u && red_mask == 0xF800u
        && green_mask == 0x07E0u && blue_mask == 0x001Fu
        && alpha_mask == 0u) {
        format.kind = DDW_PIXEL_FORMAT_RGB565;
    } else if (bits_per_pixel == 16u && red_mask == 0x7C00u
        && green_mask == 0x03E0u && blue_mask == 0x001Fu
        && alpha_mask == 0u) {
        format.kind = DDW_PIXEL_FORMAT_RGB555;
    } else if (bits_per_pixel == 16u && red_mask == 0x7C00u
        && green_mask == 0x03E0u && blue_mask == 0x001Fu
        && alpha_mask == 0x8000u) {
        format.kind = DDW_PIXEL_FORMAT_A1R5G5B5;
    }
    return format;
}

int ddw_surface_format_equal(
    const DdwSurfaceFormat *left,
    const DdwSurfaceFormat *right)
{
    return left != NULL && right != NULL
        && left->kind == right->kind
        && left->bits_per_pixel == right->bits_per_pixel
        && left->red_mask == right->red_mask
        && left->green_mask == right->green_mask
        && left->blue_mask == right->blue_mask
        && left->alpha_mask == right->alpha_mask
        && left->generation == right->generation;
}

int ddw_surface_format_is_rgb565(const DdwSurfaceFormat *format)
{
    return format != NULL
        && format->kind == DDW_PIXEL_FORMAT_RGB565
        && format->bits_per_pixel == 16u
        && format->red_mask == 0xF800u
        && format->green_mask == 0x07E0u
        && format->blue_mask == 0x001Fu
        && format->alpha_mask == 0u;
}

static int ddw_mask_shift_and_width(
    uint32_t mask,
    unsigned int *out_shift,
    unsigned int *out_width)
{
    unsigned int shift = 0u;
    unsigned int width = 0u;
    uint32_t shifted;

    if (!mask || !out_shift || !out_width)
        return 0;
    while ((mask & 1u) == 0u) {
        ++shift;
        mask >>= 1u;
    }
    shifted = mask;
    while ((shifted & 1u) != 0u) {
        ++width;
        shifted >>= 1u;
    }
    if (shifted != 0u || width == 0u || shift + width > 16u)
        return 0;
    *out_shift = shift;
    *out_width = width;
    return 1;
}

int ddw_surface_format_is_presentable(const DdwSurfaceFormat *format)
{
    unsigned int ignored_shift;
    unsigned int ignored_width;
    uint32_t rgb_union;

    if (!format)
        return 0;
    if (format->kind == DDW_PIXEL_FORMAT_INDEX8)
        return format->bits_per_pixel == 8u
            && format->red_mask == 0u && format->green_mask == 0u
            && format->blue_mask == 0u && format->alpha_mask == 0u;
    if (format->bits_per_pixel != 16u)
        return 0;
    if (!ddw_mask_shift_and_width(
            format->red_mask, &ignored_shift, &ignored_width)
        || !ddw_mask_shift_and_width(
            format->green_mask, &ignored_shift, &ignored_width)
        || !ddw_mask_shift_and_width(
            format->blue_mask, &ignored_shift, &ignored_width)) {
        return 0;
    }
    rgb_union = format->red_mask | format->green_mask | format->blue_mask;
    if ((format->red_mask & format->green_mask) != 0u
        || (format->red_mask & format->blue_mask) != 0u
        || (format->green_mask & format->blue_mask) != 0u
        || (format->alpha_mask & rgb_union) != 0u
        || ((rgb_union | format->alpha_mask) > 0xFFFFu)) {
        return 0;
    }
    return 1;
}

void ddw_rgb565_to_bgrx(
    uint16_t rgb565,
    uint8_t *out_blue,
    uint8_t *out_green,
    uint8_t *out_red)
{
    const uint8_t red5 = (uint8_t)((rgb565 >> 11u) & 0x1Fu);
    const uint8_t green6 = (uint8_t)((rgb565 >> 5u) & 0x3Fu);
    const uint8_t blue5 = (uint8_t)(rgb565 & 0x1Fu);

    if (out_blue)
        *out_blue = (uint8_t)((blue5 << 3u) | (blue5 >> 2u));
    if (out_green)
        *out_green = (uint8_t)((green6 << 2u) | (green6 >> 4u));
    if (out_red)
        *out_red = (uint8_t)((red5 << 3u) | (red5 >> 2u));
}

uint16_t ddw_rgb565_from_bgrx(uint8_t blue, uint8_t green, uint8_t red)
{
    return (uint16_t)(((uint16_t)(red >> 3u) << 11u)
        | ((uint16_t)(green >> 2u) << 5u)
        | (uint16_t)(blue >> 3u));
}

static uint8_t ddw_expand_component(
    uint16_t packed,
    uint32_t mask)
{
    unsigned int shift;
    unsigned int width;
    uint32_t max_value;
    uint32_t value;

    if (!ddw_mask_shift_and_width(mask, &shift, &width))
        return 0u;
    max_value = (1u << width) - 1u;
    value = ((uint32_t)packed & mask) >> shift;
    return (uint8_t)((value * 255u + max_value / 2u) / max_value);
}

int ddw_surface_format_decode_rgb(
    const DdwSurfaceFormat *format,
    uint16_t packed,
    DdwRgb8 *out_rgb)
{
    if (!format || !out_rgb || !ddw_surface_format_is_presentable(format)
        || format->kind == DDW_PIXEL_FORMAT_INDEX8) {
        return 0;
    }
    if (ddw_surface_format_is_rgb565(format)) {
        ddw_rgb565_to_bgrx(
            packed, &out_rgb->blue, &out_rgb->green, &out_rgb->red);
        return 1;
    }
    out_rgb->red = ddw_expand_component(packed, format->red_mask);
    out_rgb->green = ddw_expand_component(packed, format->green_mask);
    out_rgb->blue = ddw_expand_component(packed, format->blue_mask);
    return 1;
}

static int checked_add_size(size_t a, size_t b, size_t *out)
{
    if (!out || a > SIZE_MAX - b)
        return 0;
    *out = a + b;
    return 1;
}

static int checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (!out || (a != 0 && b > SIZE_MAX / a))
        return 0;
    *out = a * b;
    return 1;
}

int ddw_compute_surface_layout(
    int width,
    int height,
    int bytes_per_pixel,
    DdwSurfaceLayout *out_layout)
{
    DdwSurfaceLayout layout;
    size_t row_bytes;
    size_t aligned_row_bytes;
    size_t total_bytes;

    memset(&layout, 0, sizeof(layout));
    if (out_layout)
        *out_layout = layout;

    if (!out_layout || width <= 0 || height <= 0
        || (bytes_per_pixel != 1 && bytes_per_pixel != 2))
        return 0;

    if (!checked_mul_size((size_t)width, (size_t)bytes_per_pixel, &row_bytes)
        || !checked_add_size(row_bytes, 3u, &aligned_row_bytes))
        return 0;

    aligned_row_bytes &= ~(size_t)3u;
    if (aligned_row_bytes == 0 || aligned_row_bytes > (size_t)INT_MAX)
        return 0;
    if (!checked_mul_size(aligned_row_bytes, (size_t)height, &total_bytes)
        || total_bytes == 0)
        return 0;

    layout.width = width;
    layout.height = height;
    layout.bytes_per_pixel = bytes_per_pixel;
    layout.pitch = (int)aligned_row_bytes;
    layout.buffer_bytes = total_bytes;
    *out_layout = layout;
    return 1;
}

static int validate_surface_view(const DdwSurfaceView *surface)
{
    size_t row_bytes;
    size_t required_bytes;

    if (!surface || !surface->pixels || surface->width <= 0
        || surface->height <= 0 || surface->pitch <= 0
        || (surface->bytes_per_pixel != 1 && surface->bytes_per_pixel != 2))
        return 0;

    if (!checked_mul_size(
            (size_t)surface->width,
            (size_t)surface->bytes_per_pixel,
            &row_bytes)
        || row_bytes > (size_t)surface->pitch)
        return 0;

    if (!checked_mul_size(
            (size_t)surface->pitch,
            (size_t)surface->height,
            &required_bytes)
        || required_bytes > surface->buffer_bytes)
        return 0;

    return 1;
}

int ddw_clip_rect(
    DdwRect rect,
    int width,
    int height,
    DdwRect *out_rect)
{
    DdwRect clipped;

    memset(&clipped, 0, sizeof(clipped));
    if (out_rect)
        *out_rect = clipped;
    if (!out_rect || width <= 0 || height <= 0)
        return 0;
    if ((int64_t)rect.right <= (int64_t)rect.left
        || (int64_t)rect.bottom <= (int64_t)rect.top)
        return 0;

    clipped.left = rect.left < 0 ? 0 : rect.left;
    clipped.top = rect.top < 0 ? 0 : rect.top;
    clipped.right = rect.right > width ? width : rect.right;
    clipped.bottom = rect.bottom > height ? height : rect.bottom;

    if (clipped.right <= clipped.left || clipped.bottom <= clipped.top)
        return 0;

    *out_rect = clipped;
    return 1;
}

int ddw_validate_lock_region(
    const DdwSurfaceView *surface,
    const DdwRect *requested_rect,
    DdwLockRegion *out_region)
{
    DdwLockRegion region;
    DdwRect rect;
    size_t top_offset;
    size_t left_offset;
    size_t last_row_offset;
    size_t row_bytes;
    size_t end_offset;

    memset(&region, 0, sizeof(region));
    if (out_region)
        *out_region = region;
    if (!out_region || !validate_surface_view(surface))
        return 0;

    if (requested_rect) {
        rect = *requested_rect;
    } else {
        rect.left = 0;
        rect.top = 0;
        rect.right = surface->width;
        rect.bottom = surface->height;
    }

    if (rect.left < 0 || rect.top < 0
        || rect.right > surface->width || rect.bottom > surface->height
        || rect.right <= rect.left || rect.bottom <= rect.top)
        return 0;

    if (!checked_mul_size((size_t)rect.top, (size_t)surface->pitch, &top_offset)
        || !checked_mul_size(
            (size_t)rect.left,
            (size_t)surface->bytes_per_pixel,
            &left_offset)
        || !checked_add_size(top_offset, left_offset, &region.byte_offset))
        return 0;

    region.width = rect.right - rect.left;
    region.height = rect.bottom - rect.top;
    if (!checked_mul_size(
            (size_t)(region.height - 1),
            (size_t)surface->pitch,
            &last_row_offset)
        || !checked_mul_size(
            (size_t)region.width,
            (size_t)surface->bytes_per_pixel,
            &row_bytes)
        || !checked_add_size(region.byte_offset, last_row_offset, &end_offset)
        || !checked_add_size(end_offset, row_bytes, &end_offset)
        || end_offset > surface->buffer_bytes)
        return 0;

    region.rect = rect;
    *out_region = region;
    return 1;
}

int ddw_read_fill_color(const void *fx, uint32_t *out_fill_color)
{
    uint32_t size;
    uint32_t fill;

    if (!out_fill_color)
        return 0;
    *out_fill_color = 0;
    if (!fx)
        return 1;

    memcpy(&size, fx, sizeof(size));
    if (size < DDW_DDBLTFX_FILL_COLOR_END)
        return 0;

    memcpy(
        &fill,
        (const uint8_t *)fx + DDW_DDBLTFX_FILL_COLOR_OFFSET,
        sizeof(fill));
    *out_fill_color = fill;
    return 1;
}

static uint32_t read_pixel(
    const uint8_t *row,
    int x,
    int bytes_per_pixel)
{
    if (bytes_per_pixel == 1)
        return row[x];
    else {
        uint16_t value;
        memcpy(&value, row + (size_t)x * 2u, sizeof(value));
        return value;
    }
}

static void write_pixel(
    uint8_t *row,
    int x,
    int bytes_per_pixel,
    uint32_t value)
{
    if (bytes_per_pixel == 1) {
        row[x] = (uint8_t)value;
    } else {
        uint16_t value16 = (uint16_t)value;
        memcpy(row + (size_t)x * 2u, &value16, sizeof(value16));
    }
}

DdwOpResult ddw_fill_rect(
    DdwSurfaceView *dst,
    DdwRect rect,
    uint32_t fill_color)
{
    DdwRect clipped;
    int y;

    if (!validate_surface_view(dst))
        return DDW_OP_INVALID;
    if (!ddw_clip_rect(rect, dst->width, dst->height, &clipped))
        return DDW_OP_EMPTY;

    for (y = clipped.top; y < clipped.bottom; ++y) {
        uint8_t *row = dst->pixels + (size_t)y * (size_t)dst->pitch;
        if (dst->bytes_per_pixel == 1) {
            memset(
                row + clipped.left,
                (uint8_t)fill_color,
                (size_t)(clipped.right - clipped.left));
        } else {
            int x;
            for (x = clipped.left; x < clipped.right; ++x)
                write_pixel(row, x, 2, fill_color);
        }
    }

    return DDW_OP_OK;
}

int ddw_clip_blt_fast(
    DdwRect src_rect,
    int src_width,
    int src_height,
    int32_t dst_x,
    int32_t dst_y,
    int dst_width,
    int dst_height,
    DdwCopyRegion *out_region)
{
    DdwCopyRegion region;
    int64_t sx0 = src_rect.left;
    int64_t sy0 = src_rect.top;
    int64_t sx1 = src_rect.right;
    int64_t sy1 = src_rect.bottom;
    int64_t dx0 = dst_x;
    int64_t dy0 = dst_y;
    int64_t shift;
    int64_t width;
    int64_t height;

    memset(&region, 0, sizeof(region));
    if (out_region)
        *out_region = region;
    if (!out_region || src_width <= 0 || src_height <= 0
        || dst_width <= 0 || dst_height <= 0
        || sx1 <= sx0 || sy1 <= sy0)
        return 0;

    if (sx0 < 0) {
        shift = -sx0;
        sx0 = 0;
        dx0 += shift;
    }
    if (sy0 < 0) {
        shift = -sy0;
        sy0 = 0;
        dy0 += shift;
    }
    if (sx1 > src_width)
        sx1 = src_width;
    if (sy1 > src_height)
        sy1 = src_height;

    if (dx0 < 0) {
        shift = -dx0;
        dx0 = 0;
        sx0 += shift;
    }
    if (dy0 < 0) {
        shift = -dy0;
        dy0 = 0;
        sy0 += shift;
    }

    if (sx1 <= sx0 || sy1 <= sy0 || dx0 >= dst_width || dy0 >= dst_height)
        return 0;

    width = sx1 - sx0;
    height = sy1 - sy0;
    if (width > (int64_t)dst_width - dx0)
        width = (int64_t)dst_width - dx0;
    if (height > (int64_t)dst_height - dy0)
        height = (int64_t)dst_height - dy0;
    if (width <= 0 || height <= 0)
        return 0;

    region.src_x = (int)sx0;
    region.src_y = (int)sy0;
    region.dst_x = (int)dx0;
    region.dst_y = (int)dy0;
    region.width = (int)width;
    region.height = (int)height;
    *out_region = region;
    return 1;
}

static int copy_regions_overlap(const DdwCopyRegion *region)
{
    int64_t src_right = (int64_t)region->src_x + region->width;
    int64_t src_bottom = (int64_t)region->src_y + region->height;
    int64_t dst_right = (int64_t)region->dst_x + region->width;
    int64_t dst_bottom = (int64_t)region->dst_y + region->height;

    return (int64_t)region->src_x < dst_right
        && (int64_t)region->dst_x < src_right
        && (int64_t)region->src_y < dst_bottom
        && (int64_t)region->dst_y < src_bottom;
}

DdwOpResult ddw_blt_fast(
    DdwSurfaceView *dst,
    int32_t dst_x,
    int32_t dst_y,
    const DdwSurfaceView *src,
    DdwRect src_rect,
    int use_source_color_key,
    uint32_t color_key_low,
    uint32_t color_key_high)
{
    DdwCopyRegion region;
    int same_buffer;
    int overlap;
    int y_begin;
    int y_end;
    int y_step;
    int row_index;

    if (!validate_surface_view(dst) || !validate_surface_view(src)
        || dst->bytes_per_pixel != src->bytes_per_pixel)
        return DDW_OP_INVALID;

    if (!ddw_clip_blt_fast(
            src_rect,
            src->width,
            src->height,
            dst_x,
            dst_y,
            dst->width,
            dst->height,
            &region))
        return DDW_OP_EMPTY;

    same_buffer = dst->pixels == src->pixels;
    overlap = same_buffer && copy_regions_overlap(&region);
    y_begin = 0;
    y_end = region.height;
    y_step = 1;
    if (overlap && region.dst_y > region.src_y) {
        y_begin = region.height - 1;
        y_end = -1;
        y_step = -1;
    }

    for (row_index = y_begin; row_index != y_end; row_index += y_step) {
        const uint8_t *src_row =
            src->pixels
            + (size_t)(region.src_y + row_index) * (size_t)src->pitch;
        uint8_t *dst_row =
            dst->pixels
            + (size_t)(region.dst_y + row_index) * (size_t)dst->pitch;

        if (!use_source_color_key) {
            size_t bytes =
                (size_t)region.width * (size_t)dst->bytes_per_pixel;
            uint8_t *dst_start =
                dst_row
                + (size_t)region.dst_x * (size_t)dst->bytes_per_pixel;
            const uint8_t *src_start =
                src_row
                + (size_t)region.src_x * (size_t)src->bytes_per_pixel;
            if (same_buffer)
                memmove(dst_start, src_start, bytes);
            else
                memcpy(dst_start, src_start, bytes);
        } else {
            int x_begin = 0;
            int x_end = region.width;
            int x_step = 1;
            int column;

            if (overlap
                && region.dst_y + row_index == region.src_y + row_index
                && region.dst_x > region.src_x) {
                x_begin = region.width - 1;
                x_end = -1;
                x_step = -1;
            }

            for (column = x_begin; column != x_end; column += x_step) {
                uint32_t pixel = read_pixel(
                    src_row,
                    region.src_x + column,
                    src->bytes_per_pixel);
                if (pixel < color_key_low || pixel > color_key_high) {
                    write_pixel(
                        dst_row,
                        region.dst_x + column,
                        dst->bytes_per_pixel,
                        pixel);
                }
            }
        }
    }

    return DDW_OP_OK;
}

static int rect_dimensions(DdwRect rect, int64_t *width, int64_t *height)
{
    int64_t w = (int64_t)rect.right - (int64_t)rect.left;
    int64_t h = (int64_t)rect.bottom - (int64_t)rect.top;

    if (w <= 0 || h <= 0)
        return 0;
    if (width)
        *width = w;
    if (height)
        *height = h;
    return 1;
}

size_t ddw_blt_scratch_bytes(
    const DdwSurfaceView *dst,
    DdwRect dst_rect,
    const DdwSurfaceView *src,
    DdwRect src_rect)
{
    int64_t dst_width;
    int64_t dst_height;
    int64_t src_width;
    int64_t src_height;

    if (!validate_surface_view(dst) || !validate_surface_view(src)
        || dst->pixels != src->pixels
        || !rect_dimensions(dst_rect, &dst_width, &dst_height)
        || !rect_dimensions(src_rect, &src_width, &src_height)
        || (dst_width == src_width && dst_height == src_height))
        return 0;

    return src->buffer_bytes;
}

DdwOpResult ddw_blt_nearest(
    DdwSurfaceView *dst,
    DdwRect dst_rect,
    const DdwSurfaceView *src,
    DdwRect src_rect,
    int use_source_color_key,
    uint32_t color_key_low,
    uint32_t color_key_high,
    void *scratch,
    size_t scratch_bytes)
{
    int64_t dst_width;
    int64_t dst_height;
    int64_t src_width;
    int64_t src_height;
    DdwRect clipped_dst;
    DdwSurfaceView source_view;
    const DdwSurfaceView *source = src;
    int y;

    if (!validate_surface_view(dst) || !validate_surface_view(src)
        || dst->bytes_per_pixel != src->bytes_per_pixel)
        return DDW_OP_INVALID;
    if (!rect_dimensions(dst_rect, &dst_width, &dst_height)
        || !rect_dimensions(src_rect, &src_width, &src_height))
        return DDW_OP_EMPTY;

    if (dst_width == src_width && dst_height == src_height) {
        return ddw_blt_fast(
            dst,
            dst_rect.left,
            dst_rect.top,
            src,
            src_rect,
            use_source_color_key,
            color_key_low,
            color_key_high);
    }

    if (!ddw_clip_rect(dst_rect, dst->width, dst->height, &clipped_dst))
        return DDW_OP_EMPTY;

    if (dst->pixels == src->pixels) {
        if (!scratch || scratch == src->pixels
            || scratch_bytes < src->buffer_bytes)
            return DDW_OP_NEEDS_SCRATCH;
        memcpy(scratch, src->pixels, src->buffer_bytes);
        source_view = *src;
        source_view.pixels = (uint8_t *)scratch;
        source = &source_view;
    }

    for (y = clipped_dst.top; y < clipped_dst.bottom; ++y) {
        uint64_t relative_y =
            (uint64_t)((int64_t)y - (int64_t)dst_rect.top);
        int64_t source_y =
            (int64_t)src_rect.top
            + (int64_t)(
                (relative_y * (uint64_t)src_height)
                / (uint64_t)dst_height);
        uint8_t *dst_row;
        const uint8_t *src_row;
        int x;

        if (source_y < 0 || source_y >= source->height)
            continue;
        dst_row = dst->pixels + (size_t)y * (size_t)dst->pitch;
        src_row =
            source->pixels + (size_t)source_y * (size_t)source->pitch;

        for (x = clipped_dst.left; x < clipped_dst.right; ++x) {
            uint64_t relative_x =
                (uint64_t)((int64_t)x - (int64_t)dst_rect.left);
            int64_t source_x =
                (int64_t)src_rect.left
                + (int64_t)(
                    (relative_x * (uint64_t)src_width)
                    / (uint64_t)dst_width);
            uint32_t pixel;

            if (source_x < 0 || source_x >= source->width)
                continue;
            pixel = read_pixel(src_row, (int)source_x, source->bytes_per_pixel);
            if (use_source_color_key
                && pixel >= color_key_low && pixel <= color_key_high)
                continue;
            write_pixel(dst_row, x, dst->bytes_per_pixel, pixel);
        }
    }

    return DDW_OP_OK;
}

void ddw_letterbox(int screenW, int screenH, int gameW, int gameH,
                   int *outX, int *outY, int *outW, int *outH)
{
    int w = screenW;
    int h = (gameW > 0) ? screenW * gameH / gameW : screenH;
    if (h > screenH) {
        h = screenH;
        w = (gameH > 0) ? screenH * gameW / gameH : screenW;
    }
    if (outX != NULL) *outX = (screenW - w) / 2;
    if (outY != NULL) *outY = (screenH - h) / 2;
    if (outW != NULL) *outW = w;
    if (outH != NULL) *outH = h;
}

int ddw_is_qoh99_frame_commit(
    int dst_is_primary,
    int src_is_primary,
    int dst_width,
    int dst_height,
    int src_width,
    int src_height,
    DdwRect dst_rect,
    DdwRect src_rect)
{
    int64_t commit_dst_width;
    int64_t commit_dst_height;

    /* The commit is identified by its source: a full-surface read of the 320x240
           back buffer targeting the primary. Destination size is not a discriminator. */
    if (!dst_is_primary || src_is_primary
        || src_width != 320 || src_height != 240)
        return 0;
    if (!((dst_width == 640 && dst_height == 480)
          || (dst_width == 320 && dst_height == 240)))
        return 0;

    if (src_rect.left != 0 || src_rect.top != 0
        || src_rect.right != 320 || src_rect.bottom != 240)
        return 0;
    if (!rect_dimensions(
            dst_rect,
            &commit_dst_width,
            &commit_dst_height))
        return 0;

    if (commit_dst_width < 320 || commit_dst_height < 240)
        return 0;
    if (commit_dst_width > 16384 || commit_dst_height > 16384)
        return 0;

    return 1;
}
