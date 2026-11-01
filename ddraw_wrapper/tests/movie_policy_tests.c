#include "qoh99_movie_policy.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static void test_policy_matrix(void)
{
    Qoh99MovieDecision d;

    d = qoh99_movie_policy_decide(0, 0);
    CHECK(d.policy == QOH99_MOVIE_LEAVE_ALONE);
    d = qoh99_movie_policy_decide(1, 0);
    CHECK(d.policy == QOH99_MOVIE_LEAVE_ALONE);

    d = qoh99_movie_policy_decide(0, 1);
    CHECK(d.policy == QOH99_MOVIE_PLAY);
    CHECK(d.skipReason == QOH99_MOVIE_SKIP_NONE);

    d = qoh99_movie_policy_decide(1, 1);
    CHECK(d.policy == QOH99_MOVIE_SKIP);
    CHECK(d.skipReason == QOH99_MOVIE_SKIP_REQUESTED);

    CHECK(strlen(qoh99_movie_policy_text(QOH99_MOVIE_LEAVE_ALONE)) > 0);
    CHECK(strlen(qoh99_movie_policy_text(QOH99_MOVIE_PLAY)) > 0);
    CHECK(strlen(qoh99_movie_policy_text(QOH99_MOVIE_SKIP)) > 0);
    CHECK(strlen(qoh99_movie_skip_reason_text(QOH99_MOVIE_SKIP_REQUESTED)) > 0);
    CHECK(strlen(qoh99_movie_skip_reason_text(QOH99_MOVIE_SKIP_UNPLAYABLE)) > 0);
}

static void test_skip_patch_encoding(void)
{
    unsigned char patch[QOH99_MOVIE_SKIP_PATCH_BYTES];
    size_t n;

    n = qoh99_movie_skip_patch_encode(patch, sizeof(patch), 0x004AB794u);
    CHECK(n == QOH99_MOVIE_SKIP_PATCH_BYTES);

    CHECK(patch[0] == 0xC6);
    CHECK(patch[1] == 0x05);
    CHECK(patch[2] == 0x94);
    CHECK(patch[3] == 0xB7);
    CHECK(patch[4] == 0x4A);
    CHECK(patch[5] == 0x00);
    CHECK(patch[6] == 0x00);
    CHECK(patch[7] == 0x33);
    CHECK(patch[8] == 0xC0);
    CHECK(patch[9] == 0xC3);

    n = qoh99_movie_skip_patch_encode(patch, sizeof(patch), 0x11223344u);
    CHECK(n == QOH99_MOVIE_SKIP_PATCH_BYTES);
    CHECK(patch[2] == 0x44 && patch[3] == 0x33
          && patch[4] == 0x22 && patch[5] == 0x11);

    CHECK(qoh99_movie_skip_patch_encode(patch, 4u, 0x004AB794u) == 0u);
    CHECK(qoh99_movie_skip_patch_encode(NULL, sizeof(patch), 0x004AB794u) == 0u);
}

static void test_skip_patch_site_verification(void)
{
    unsigned char expected[QOH99_MOVIE_PROBE_PROLOGUE_BYTES];
    unsigned char probe[QOH99_MOVIE_PROBE_PROLOGUE_BYTES];
    size_t n;

    n = qoh99_movie_skip_patch_expected(expected, sizeof(expected));
    CHECK(n == QOH99_MOVIE_PROBE_PROLOGUE_BYTES);
    CHECK(QOH99_MOVIE_PROBE_PROLOGUE_BYTES > QOH99_MOVIE_SKIP_PATCH_BYTES);

    CHECK(expected[0] == 0x81 && expected[1] == 0xEC && expected[2] == 0x08
          && expected[3] == 0x01 && expected[4] == 0x00 && expected[5] == 0x00);
    CHECK(expected[6] == 0xA1 && expected[7] == 0x9C && expected[8] == 0x77
          && expected[9] == 0x4A && expected[10] == 0x00);

    memcpy(probe, expected, sizeof(probe));
    CHECK(qoh99_movie_skip_patch_site_matches(probe, sizeof(probe)) == 1);

    {
        size_t i;
        for (i = 0; i < n; ++i) {
            memcpy(probe, expected, sizeof(probe));
            probe[i] ^= 0xFFu;
            CHECK(qoh99_movie_skip_patch_site_matches(probe, sizeof(probe)) == 0);
        }
    }

    CHECK(qoh99_movie_skip_patch_site_matches(expected, 3u) == 0);
    CHECK(qoh99_movie_skip_patch_site_matches(expected,
                                             QOH99_MOVIE_SKIP_PATCH_BYTES) == 0);
    CHECK(qoh99_movie_skip_patch_expected(expected,
                                         QOH99_MOVIE_SKIP_PATCH_BYTES) == 0u);
    CHECK(qoh99_movie_skip_patch_site_matches(NULL, sizeof(probe)) == 0);
}

static void test_mci_open_ownership(void)
{
    const unsigned int openElement =
        QOH99_MCI_FLAG_WAIT | QOH99_MCI_FLAG_OPEN_ELEMENT;

    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       ".\\System\\Opening.avi") == 1);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "D:\\Movie\\Opening.avi") == 1);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "C:\\GAME\\SYSTEM\\OPENING.AVI") == 1);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "opening.avi") == 1);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "d:opening.avi") == 1);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "./System/Opening.avi") == 1);

    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "C:\\music\\track01.mp3") == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "cdaudio") == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "C:\\v\\MyOpening.avi") == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement,
                                       "notopening.avi") == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, QOH99_MCI_FLAG_WAIT,
                                       ".\\System\\Opening.avi") == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement, NULL) == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_PLAY, openElement,
                                       ".\\System\\Opening.avi") == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_CLOSE, openElement,
                                       ".\\System\\Opening.avi") == 0);
    CHECK(qoh99_movie_mci_open_is_ours(QOH99_MCI_OPEN, openElement, "") == 0);
}

static void test_device_id_is_not_a_real_one(void)
{
    CHECK(qoh99_movie_mci_is_our_device(QOH99_MOVIE_MCI_DEVICE_ID) == 1);
    CHECK(qoh99_movie_mci_is_our_device(0u) == 0);
    CHECK(qoh99_movie_mci_is_our_device(1u) == 0);
    CHECK(qoh99_movie_mci_is_our_device(2u) == 0);
    CHECK(qoh99_movie_mci_is_our_device(255u) == 0);
    CHECK(QOH99_MOVIE_MCI_DEVICE_ID > 0x1000u);
}

int main(void)
{
    test_policy_matrix();
    test_skip_patch_encoding();
    test_skip_patch_site_verification();
    test_mci_open_ownership();
    test_device_id_is_not_a_real_one();

    if (g_failures != 0) {
        printf("%d movie policy checks failed\n", g_failures);
        return 1;
    }
    printf("movie policy checks passed\n");
    return 0;
}
