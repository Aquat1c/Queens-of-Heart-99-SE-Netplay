#include "qoh99_presentation_policy.h"

#include <stdio.h>

static int failures;

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);              \
        ++failures;                                                          \
    }                                                                        \
} while (0)

static void test_mode_migration(void)
{
    CHECK(qoh99_presentation_mode_from_ini(0, 0, 0)
          == QOH_PRESENTATION_WINDOWED);
    CHECK(qoh99_presentation_mode_from_ini(0, 0, 1)
          == QOH_PRESENTATION_BORDERLESS);
    CHECK(qoh99_presentation_mode_from_ini(1, 0, 1)
          == QOH_PRESENTATION_WINDOWED);
    CHECK(qoh99_presentation_mode_from_ini(1, 1, 0)
          == QOH_PRESENTATION_BORDERLESS);
    CHECK(qoh99_presentation_mode_from_ini(1, 2, 0)
          == QOH_PRESENTATION_EXCLUSIVE);
    CHECK(qoh99_presentation_mode_from_ini(1, -1, 1)
          == QOH_PRESENTATION_WINDOWED);
    CHECK(qoh99_presentation_mode_from_ini(1, 3, 1)
          == QOH_PRESENTATION_WINDOWED);
}

static void test_toggle_and_downgrade_mirror(void)
{
    CHECK(qoh99_preferred_fullscreen_toggle(
              QOH_PRESENTATION_WINDOWED, QOH_PRESENTATION_BORDERLESS)
          == QOH_PRESENTATION_BORDERLESS);
    CHECK(qoh99_preferred_fullscreen_toggle(
              QOH_PRESENTATION_WINDOWED, QOH_PRESENTATION_EXCLUSIVE)
          == QOH_PRESENTATION_EXCLUSIVE);
    CHECK(qoh99_preferred_fullscreen_toggle(
              QOH_PRESENTATION_BORDERLESS, QOH_PRESENTATION_EXCLUSIVE)
          == QOH_PRESENTATION_WINDOWED);
    CHECK(qoh99_preferred_fullscreen_toggle(
              QOH_PRESENTATION_EXCLUSIVE, QOH_PRESENTATION_BORDERLESS)
          == QOH_PRESENTATION_WINDOWED);
    CHECK(qoh99_presentation_legacy_borderless_mirror(
              QOH_PRESENTATION_BORDERLESS) == 1);
    CHECK(qoh99_presentation_legacy_borderless_mirror(
              QOH_PRESENTATION_WINDOWED) == 0);
    CHECK(qoh99_presentation_legacy_borderless_mirror(
              QOH_PRESENTATION_EXCLUSIVE) == 0);
}

static void test_backend_and_gdi_policy(void)
{
    int value;
    for (value = 0; value <= 3; ++value) {
        CHECK(qoh99_renderer_backend_valid(value));
        CHECK((int)qoh99_renderer_backend_from_ini(value) == value);
    }
    CHECK(!qoh99_renderer_backend_valid(-1));
    CHECK(!qoh99_renderer_backend_valid(4));
    CHECK(qoh99_renderer_backend_from_ini(-1) == QOH_RENDERER_AUTO);
    CHECK(qoh99_renderer_backend_from_ini(4) == QOH_RENDERER_AUTO);
    CHECK(qoh99_presentation_allows_gdi(QOH_PRESENTATION_WINDOWED));
    CHECK(qoh99_presentation_allows_gdi(QOH_PRESENTATION_BORDERLESS));
    CHECK(!qoh99_presentation_allows_gdi(QOH_PRESENTATION_EXCLUSIVE));
    CHECK(qoh99_presentation_backend_allows_gdi(
              QOH_PRESENTATION_WINDOWED, QOH_RENDERER_CLASSIC_D3D9));
    CHECK(!qoh99_presentation_backend_allows_gdi(
              QOH_PRESENTATION_WINDOWED, QOH_RENDERER_D3D9EX_FLIPEX));
    CHECK(!qoh99_presentation_backend_allows_gdi(
              QOH_PRESENTATION_EXCLUSIVE, QOH_RENDERER_CLASSIC_D3D9));
}

int main(void)
{
    test_mode_migration();
    test_toggle_and_downgrade_mirror();
    test_backend_and_gdi_policy();
    if (failures != 0) {
        printf("presentation policy: %d failure(s)\n", failures);
        return 1;
    }
    puts("presentation policy: all tests passed");
    return 0;
}
