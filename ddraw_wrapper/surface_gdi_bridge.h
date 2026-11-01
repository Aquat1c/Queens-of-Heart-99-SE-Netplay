#ifndef QOH99_DDRAW_SURFACE_GDI_BRIDGE_H
#define QOH99_DDRAW_SURFACE_GDI_BRIDGE_H

#include <windows.h>

#include "surface_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DdwGdiBridgeResult {
    DDW_GDI_BRIDGE_OK = 0,
    DDW_GDI_BRIDGE_INVALID_ARGUMENT = -1,
    DDW_GDI_BRIDGE_INVALID_SURFACE = -2,
    DDW_GDI_BRIDGE_UNSUPPORTED_FORMAT = -3,
    DDW_GDI_BRIDGE_OUT_OF_MEMORY = -4,
    DDW_GDI_BRIDGE_GDI_FAILURE = -5,
    DDW_GDI_BRIDGE_SURFACE_LOCKED = -6,
    DDW_GDI_BRIDGE_DC_ALREADY_ACTIVE = -7,
    DDW_GDI_BRIDGE_INVALID_DC = -8
} DdwGdiBridgeResult;

typedef BOOL (WINAPI *DdwGdiBridgeRestoreDcFn)(HDC dc, int save_level);
typedef BOOL (WINAPI *DdwGdiBridgeFlushFn)(void);
typedef void (*DdwGdiBridgeBeforeReadbackFn)(void *user_data);

typedef struct DdwGdiBridgeCallbacks {
    DdwGdiBridgeRestoreDcFn restore_dc;
    DdwGdiBridgeFlushFn flush;
    DdwGdiBridgeBeforeReadbackFn before_readback;
    void *user_data;
} DdwGdiBridgeCallbacks;

typedef struct DdwGdiBridge {
    HDC     dc;
    HBITMAP bitmap;
    HGDIOBJ previous_bitmap;
    void   *bits;
    int     width;
    int     height;
    int     pitch;
    int     scratch_bits_per_pixel;
    DWORD   scratch_compression;
    DdwSurfaceFormat source_format;
    int     save_level;
    BOOL    active;
    BOOL    last_restore_success;
    BOOL    last_flush_success;
    DdwGdiBridgeCallbacks callbacks;
} DdwGdiBridge;

DdwGdiBridgeResult ddw_gdi_bridge_acquire(
    DdwGdiBridge *bridge,
    const DdwSurfaceView *surface,
    int surface_locked,
    HDC *out_dc,
    int *out_recreated);

DdwGdiBridgeResult ddw_gdi_bridge_release(
    DdwGdiBridge *bridge,
    DdwSurfaceView *surface,
    HDC dc,
    int write_back);

void ddw_gdi_bridge_destroy(DdwGdiBridge *bridge);

void ddw_gdi_bridge_set_callbacks(
    DdwGdiBridge *bridge,
    const DdwGdiBridgeCallbacks *callbacks);

int ddw_gdi_bridge_is_active(const DdwGdiBridge *bridge);
int ddw_gdi_bridge_lock_allowed(const DdwGdiBridge *bridge);
int ddw_gdi_bridge_pitch(const DdwGdiBridge *bridge);
int ddw_gdi_bridge_scratch_bits_per_pixel(const DdwGdiBridge *bridge);
DWORD ddw_gdi_bridge_scratch_compression(const DdwGdiBridge *bridge);

#ifdef __cplusplus
}
#endif

#endif
