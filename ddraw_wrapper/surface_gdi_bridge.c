#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "surface_gdi_bridge.h"

typedef struct DdwBgrxBitmapInfo {
    BITMAPINFOHEADER header;
} DdwBgrxBitmapInfo;

static int ddw_gdi_surface_shape_valid(const DdwSurfaceView *surface)
{
    size_t row_bytes;
    size_t required_bytes;

    if (!surface || !surface->pixels || surface->width <= 0
        || surface->height <= 0 || surface->pitch <= 0
        || surface->bytes_per_pixel <= 0) {
        return 0;
    }
    if ((size_t)surface->width > SIZE_MAX / (size_t)surface->bytes_per_pixel)
        return 0;
    row_bytes = (size_t)surface->width * (size_t)surface->bytes_per_pixel;
    if ((size_t)surface->pitch < row_bytes)
        return 0;
    if ((size_t)(surface->height - 1) >
        (SIZE_MAX - row_bytes) / (size_t)surface->pitch) {
        return 0;
    }
    required_bytes = (size_t)(surface->height - 1)
        * (size_t)surface->pitch + row_bytes;
    return surface->buffer_bytes >= required_bytes;
}

static int ddw_gdi_surface_format_supported(const DdwSurfaceView *surface)
{
    return surface != NULL && surface->bytes_per_pixel == 2
        && ddw_surface_format_is_rgb565(&surface->format);
}

static BOOL ddw_gdi_bridge_restore(DdwGdiBridge *bridge)
{
    if (bridge->callbacks.restore_dc)
        return bridge->callbacks.restore_dc(bridge->dc, bridge->save_level);
    return RestoreDC(bridge->dc, bridge->save_level);
}

static BOOL ddw_gdi_bridge_flush(DdwGdiBridge *bridge)
{
    if (bridge->callbacks.flush)
        return bridge->callbacks.flush();
    return GdiFlush();
}

static int ddw_gdi_copy_surface_to_dib(
    const DdwSurfaceView *surface,
    const DdwGdiBridge *bridge)
{
    const size_t scratch_row_bytes = (size_t)surface->width * 4u;
    int y;

    if (!surface || !bridge || !bridge->bits || bridge->pitch <= 0
        || bridge->pitch < (int)scratch_row_bytes) {
        return 0;
    }
    for (y = 0; y < surface->height; ++y) {
        uint8_t *dst = (uint8_t *)bridge->bits + (size_t)y * bridge->pitch;
        const uint8_t *src = surface->pixels + (size_t)y * surface->pitch;
        int x;

        for (x = 0; x < surface->width; ++x) {
            uint16_t pixel;
            uint8_t blue;
            uint8_t green;
            uint8_t red;

            memcpy(&pixel, src + (size_t)x * 2u, sizeof(pixel));
            ddw_rgb565_to_bgrx(pixel, &blue, &green, &red);
            dst[(size_t)x * 4u + 0u] = blue;
            dst[(size_t)x * 4u + 1u] = green;
            dst[(size_t)x * 4u + 2u] = red;
            dst[(size_t)x * 4u + 3u] = 0u;
        }
        if (bridge->pitch > (int)scratch_row_bytes) {
            memset(dst + scratch_row_bytes, 0,
                   (size_t)bridge->pitch - scratch_row_bytes);
        }
    }
    return 1;
}

static int ddw_gdi_copy_dib_to_surface(
    const DdwGdiBridge *bridge,
    DdwSurfaceView *surface)
{
    const size_t scratch_row_bytes = (size_t)surface->width * 4u;
    int y;

    if (!surface || !bridge || !bridge->bits || bridge->pitch <= 0
        || bridge->pitch < (int)scratch_row_bytes) {
        return 0;
    }
    for (y = 0; y < surface->height; ++y) {
        const uint8_t *src =
            (const uint8_t *)bridge->bits + (size_t)y * bridge->pitch;
        uint8_t *dst = surface->pixels + (size_t)y * surface->pitch;
        int x;

        for (x = 0; x < surface->width; ++x) {
            const uint8_t *pixel = src + (size_t)x * 4u;
            const uint16_t rgb565 = ddw_rgb565_from_bgrx(
                pixel[0], pixel[1], pixel[2]);
            memcpy(dst + (size_t)x * 2u, &rgb565, sizeof(rgb565));
        }
    }
    return 1;
}

static void ddw_gdi_bridge_clear_resources(DdwGdiBridge *bridge)
{
    if (!bridge)
        return;
    if (bridge->dc && bridge->previous_bitmap)
        (void)SelectObject(bridge->dc, bridge->previous_bitmap);
    if (bridge->bitmap)
        (void)DeleteObject(bridge->bitmap);
    if (bridge->dc)
        (void)DeleteDC(bridge->dc);
    bridge->dc = NULL;
    bridge->bitmap = NULL;
    bridge->previous_bitmap = NULL;
    bridge->bits = NULL;
    bridge->width = 0;
    bridge->height = 0;
    bridge->pitch = 0;
    bridge->scratch_bits_per_pixel = 0;
    bridge->scratch_compression = 0;
    ZeroMemory(&bridge->source_format, sizeof(bridge->source_format));
}

void ddw_gdi_bridge_destroy(DdwGdiBridge *bridge)
{
    DdwGdiBridgeCallbacks callbacks;

    if (!bridge)
        return;
    callbacks = bridge->callbacks;
    if (bridge->active && bridge->dc && bridge->save_level != 0)
        (void)ddw_gdi_bridge_restore(bridge);
    bridge->active = FALSE;
    bridge->save_level = 0;
    ddw_gdi_bridge_clear_resources(bridge);
    ZeroMemory(bridge, sizeof(*bridge));
    (void)callbacks;
}

static DdwGdiBridgeResult ddw_gdi_bridge_create(
    DdwGdiBridge *bridge,
    const DdwSurfaceView *surface)
{
    DdwBgrxBitmapInfo bmi;
    HGDIOBJ previous;
    BITMAP bitmap_info;
    void *bits = NULL;

    bridge->dc = CreateCompatibleDC(NULL);
    if (!bridge->dc)
        return DDW_GDI_BRIDGE_GDI_FAILURE;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.header.biSize = sizeof(bmi.header);
    bmi.header.biWidth = surface->width;
    bmi.header.biHeight = -surface->height;
    bmi.header.biPlanes = 1;
    bmi.header.biBitCount = 32;
    bmi.header.biCompression = BI_RGB;
    bridge->bitmap = CreateDIBSection(
        bridge->dc, (const BITMAPINFO *)&bmi, DIB_RGB_COLORS,
        &bits, NULL, 0);
    if (!bridge->bitmap || !bits) {
        ddw_gdi_bridge_clear_resources(bridge);
        return DDW_GDI_BRIDGE_OUT_OF_MEMORY;
    }

    previous = SelectObject(bridge->dc, bridge->bitmap);
    if (!previous || previous == HGDI_ERROR) {
        ddw_gdi_bridge_clear_resources(bridge);
        return DDW_GDI_BRIDGE_GDI_FAILURE;
    }
    ZeroMemory(&bitmap_info, sizeof(bitmap_info));
    if (GetObject(bridge->bitmap, sizeof(bitmap_info), &bitmap_info) == 0
        || bitmap_info.bmWidthBytes <= 0) {
        ddw_gdi_bridge_clear_resources(bridge);
        return DDW_GDI_BRIDGE_GDI_FAILURE;
    }

    bridge->previous_bitmap = previous;
    bridge->bits = bits;
    bridge->width = surface->width;
    bridge->height = surface->height;
    bridge->pitch = bitmap_info.bmWidthBytes;
    bridge->scratch_bits_per_pixel = 32;
    bridge->scratch_compression = BI_RGB;
    bridge->source_format = surface->format;
    return DDW_GDI_BRIDGE_OK;
}

static DdwGdiBridgeResult ddw_gdi_bridge_ensure(
    DdwGdiBridge *bridge,
    const DdwSurfaceView *surface,
    int *out_recreated)
{
    const int had_resources = bridge->dc || bridge->bitmap || bridge->bits;
    const int matching = bridge->dc && bridge->bitmap && bridge->bits
        && bridge->width == surface->width
        && bridge->height == surface->height
        && bridge->scratch_bits_per_pixel == 32
        && bridge->scratch_compression == BI_RGB
        && ddw_surface_format_equal(&bridge->source_format, &surface->format);

    if (out_recreated)
        *out_recreated = 0;
    if (matching)
        return DDW_GDI_BRIDGE_OK;
    if (bridge->active)
        return DDW_GDI_BRIDGE_DC_ALREADY_ACTIVE;
    if (had_resources)
        ddw_gdi_bridge_clear_resources(bridge);
    if (out_recreated && had_resources)
        *out_recreated = 1;
    return ddw_gdi_bridge_create(bridge, surface);
}

DdwGdiBridgeResult ddw_gdi_bridge_acquire(
    DdwGdiBridge *bridge,
    const DdwSurfaceView *surface,
    int surface_locked,
    HDC *out_dc,
    int *out_recreated)
{
    DdwGdiBridgeResult result;
    int save_level;

    if (out_dc)
        *out_dc = NULL;
    if (!bridge || !out_dc)
        return DDW_GDI_BRIDGE_INVALID_ARGUMENT;
    if (!ddw_gdi_surface_shape_valid(surface))
        return DDW_GDI_BRIDGE_INVALID_SURFACE;
    if (!ddw_gdi_surface_format_supported(surface))
        return DDW_GDI_BRIDGE_UNSUPPORTED_FORMAT;
    if (surface_locked)
        return DDW_GDI_BRIDGE_SURFACE_LOCKED;
    if (bridge->active)
        return DDW_GDI_BRIDGE_DC_ALREADY_ACTIVE;

    result = ddw_gdi_bridge_ensure(bridge, surface, out_recreated);
    if (result != DDW_GDI_BRIDGE_OK)
        return result;
    if (!ddw_gdi_copy_surface_to_dib(surface, bridge))
        return DDW_GDI_BRIDGE_INVALID_SURFACE;

    save_level = SaveDC(bridge->dc);
    if (save_level == 0)
        return DDW_GDI_BRIDGE_GDI_FAILURE;
    bridge->save_level = save_level;
    bridge->active = TRUE;
    *out_dc = bridge->dc;
    return DDW_GDI_BRIDGE_OK;
}

DdwGdiBridgeResult ddw_gdi_bridge_release(
    DdwGdiBridge *bridge,
    DdwSurfaceView *surface,
    HDC dc,
    int write_back)
{
    DdwGdiBridgeResult result = DDW_GDI_BRIDGE_OK;

    if (!bridge || !bridge->active || !dc || dc != bridge->dc)
        return DDW_GDI_BRIDGE_INVALID_DC;
    bridge->last_restore_success = FALSE;
    bridge->last_flush_success = FALSE;
    if (!ddw_gdi_surface_shape_valid(surface)
        || !ddw_gdi_surface_format_supported(surface)
        || surface->width != bridge->width
        || surface->height != bridge->height
        || !ddw_surface_format_equal(
            &surface->format, &bridge->source_format)) {
        result = DDW_GDI_BRIDGE_INVALID_SURFACE;
    }

    if (bridge->save_level != 0 && ddw_gdi_bridge_restore(bridge)) {
        bridge->last_restore_success = TRUE;
    } else {
        if (result == DDW_GDI_BRIDGE_OK)
            result = DDW_GDI_BRIDGE_GDI_FAILURE;
    }
    bridge->save_level = 0;

    /* A CPU read of DIB-section bits is valid only after GDI has flushed the caller's queued drawing. */
    if (result == DDW_GDI_BRIDGE_OK && ddw_gdi_bridge_flush(bridge)) {
        bridge->last_flush_success = TRUE;
    } else if (result == DDW_GDI_BRIDGE_OK) {
        result = DDW_GDI_BRIDGE_GDI_FAILURE;
    }
    if (result == DDW_GDI_BRIDGE_OK && write_back) {
        if (bridge->callbacks.before_readback)
            bridge->callbacks.before_readback(bridge->callbacks.user_data);
        if (!ddw_gdi_copy_dib_to_surface(bridge, surface))
            result = DDW_GDI_BRIDGE_INVALID_SURFACE;
    }
    bridge->active = FALSE;
    return result;
}

void ddw_gdi_bridge_set_callbacks(
    DdwGdiBridge *bridge,
    const DdwGdiBridgeCallbacks *callbacks)
{
    if (!bridge || bridge->active)
        return;
    if (callbacks)
        bridge->callbacks = *callbacks;
    else
        ZeroMemory(&bridge->callbacks, sizeof(bridge->callbacks));
}

int ddw_gdi_bridge_is_active(const DdwGdiBridge *bridge)
{
    return bridge != NULL && bridge->active != FALSE;
}

int ddw_gdi_bridge_lock_allowed(const DdwGdiBridge *bridge)
{
    return !ddw_gdi_bridge_is_active(bridge);
}

int ddw_gdi_bridge_pitch(const DdwGdiBridge *bridge)
{
    return bridge ? bridge->pitch : 0;
}

int ddw_gdi_bridge_scratch_bits_per_pixel(const DdwGdiBridge *bridge)
{
    return bridge ? bridge->scratch_bits_per_pixel : 0;
}

DWORD ddw_gdi_bridge_scratch_compression(const DdwGdiBridge *bridge)
{
    return bridge ? bridge->scratch_compression : 0u;
}
