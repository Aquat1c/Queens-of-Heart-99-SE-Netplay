#ifndef QOH99_PRESENTATION_POLICY_H
#define QOH99_PRESENTATION_POLICY_H

/* Persisted in [Window] PresentationMode; these values must stay out of every peer and wire identity. */
typedef enum QohPresentationMode {
    QOH_PRESENTATION_WINDOWED = 0,
    QOH_PRESENTATION_BORDERLESS = 1,
    QOH_PRESENTATION_EXCLUSIVE = 2
} QohPresentationMode;

typedef enum QohRendererBackend {
    QOH_RENDERER_AUTO = 0,
    QOH_RENDERER_CLASSIC_D3D9 = 1,
    QOH_RENDERER_D3D9EX_FLIPEX = 2,
    QOH_RENDERER_GDI_DIAGNOSTIC = 3
} QohRendererBackend;

typedef enum QohRendererReadiness {
    QOH_RENDERER_NOT_REQUESTED = 0,
    QOH_RENDERER_PENDING = 1,
    QOH_RENDERER_READY = 2,
    QOH_RENDERER_FALLBACK_READY = 3,
    QOH_RENDERER_FAILED = 4
} QohRendererReadiness;

static int qoh99_renderer_backend_valid(int value)
{
    return value >= (int)QOH_RENDERER_AUTO
        && value <= (int)QOH_RENDERER_GDI_DIAGNOSTIC;
}

static QohRendererBackend qoh99_renderer_backend_from_ini(int value)
{
    return qoh99_renderer_backend_valid(value)
        ? (QohRendererBackend)value : QOH_RENDERER_AUTO;
}

static int qoh99_presentation_mode_valid(int value)
{
    return value >= (int)QOH_PRESENTATION_WINDOWED
        && value <= (int)QOH_PRESENTATION_EXCLUSIVE;
}

static QohPresentationMode qoh99_presentation_mode_from_ini(
    int has_presentation_mode,
    int presentation_mode,
    int legacy_borderless)
{
    if (has_presentation_mode)
        return qoh99_presentation_mode_valid(presentation_mode)
            ? (QohPresentationMode)presentation_mode
            : QOH_PRESENTATION_WINDOWED;
    return legacy_borderless
        ? QOH_PRESENTATION_BORDERLESS
        : QOH_PRESENTATION_WINDOWED;
}

static QohPresentationMode qoh99_preferred_fullscreen_toggle(
    QohPresentationMode current,
    QohPresentationMode last_fullscreen)
{
    if (current == QOH_PRESENTATION_WINDOWED) {
        return last_fullscreen == QOH_PRESENTATION_EXCLUSIVE
            ? QOH_PRESENTATION_EXCLUSIVE
            : QOH_PRESENTATION_BORDERLESS;
    }
    return QOH_PRESENTATION_WINDOWED;
}

static int qoh99_presentation_legacy_borderless_mirror(
    QohPresentationMode mode)
{
    return mode == QOH_PRESENTATION_BORDERLESS;
}

static int qoh99_presentation_allows_gdi(
    QohPresentationMode effective_mode)
{
    return effective_mode != QOH_PRESENTATION_EXCLUSIVE;
}

static int qoh99_presentation_backend_allows_gdi(
    QohPresentationMode effective_mode,
    QohRendererBackend effective_backend)
{
    return qoh99_presentation_allows_gdi(effective_mode)
        && effective_backend != QOH_RENDERER_D3D9EX_FLIPEX;
}

/* The completed-frame Blt is never elided: it is the sole native frame-commit boundary. */
enum Qoh99PresentationDisposition {
    QOH99_PRESENT_NORMAL = 0,
    QOH99_PRESENT_SUPPRESS_HISTORICAL = 1,
    QOH99_PRESENT_CORRECTED_FINAL = 2
};

static int qoh99_presentation_suppresses_surface_work(
    unsigned long disposition)
{
    return disposition == QOH99_PRESENT_SUPPRESS_HISTORICAL;
}

static int qoh99_presentation_executes_surface_work(
    unsigned long disposition,
    int is_completed_frame_commit)
{
    return is_completed_frame_commit
        || !qoh99_presentation_suppresses_surface_work(disposition);
}

static int qoh99_presentation_executes_pacing(
    unsigned long disposition)
{
    return !qoh99_presentation_suppresses_surface_work(disposition);
}

#endif
