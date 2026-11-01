#ifndef QOH99_MOVIE_POLICY_H
#define QOH99_MOVIE_POLICY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Qoh99MoviePolicy {
    QOH99_MOVIE_LEAVE_ALONE = 0,
    QOH99_MOVIE_PLAY = 1,
    QOH99_MOVIE_SKIP = 2
} Qoh99MoviePolicy;

typedef enum Qoh99MovieSkipReason {
    QOH99_MOVIE_SKIP_NONE = 0,
    QOH99_MOVIE_SKIP_REQUESTED = 1,
    QOH99_MOVIE_SKIP_UNPLAYABLE = 2
} Qoh99MovieSkipReason;

typedef struct Qoh99MovieDecision {
    Qoh99MoviePolicy policy;
    Qoh99MovieSkipReason skipReason;
} Qoh99MovieDecision;

Qoh99MovieDecision qoh99_movie_policy_decide(int casterRequestedSkip,
                                            int movieFilePresent);

const char *qoh99_movie_policy_text(Qoh99MoviePolicy policy);
const char *qoh99_movie_skip_reason_text(Qoh99MovieSkipReason reason);

#define QOH99_MOVIE_SKIP_PATCH_BYTES 10u

#define QOH99_MOVIE_PROBE_PROLOGUE_BYTES 11u

size_t qoh99_movie_skip_patch_encode(unsigned char *out, size_t cap,
                                     unsigned int filenameAddr);

size_t qoh99_movie_skip_patch_expected(unsigned char *out, size_t cap);

int qoh99_movie_skip_patch_site_matches(const unsigned char *actual, size_t len);

#define QOH99_MCI_OPEN            0x0803u
#define QOH99_MCI_CLOSE           0x0804u
#define QOH99_MCI_PLAY            0x0806u
#define QOH99_MCI_SEEK            0x0807u
#define QOH99_MCI_STOP            0x0808u
#define QOH99_MCI_PAUSE           0x0809u
#define QOH99_MCI_SET             0x080Du
#define QOH99_MCI_STATUS          0x0814u
#define QOH99_MCI_WINDOW          0x0841u
#define QOH99_MCI_PUT             0x0842u

#define QOH99_MCI_FLAG_NOTIFY        0x00000001u
#define QOH99_MCI_FLAG_WAIT          0x00000002u
#define QOH99_MCI_FLAG_FROM          0x00000004u
#define QOH99_MCI_FLAG_OPEN_ELEMENT  0x00000200u
#define QOH99_MCI_FLAG_STATUS_ITEM   0x00000100u

#define QOH99_MCI_DGV_STATUS_HWND    0x00004001u

int qoh99_movie_mci_open_is_ours(unsigned int msg, unsigned int flags,
                                 const char *elementName);

#define QOH99_MOVIE_MCI_DEVICE_ID 0x0000D199u

int qoh99_movie_mci_is_our_device(unsigned int deviceId);

#ifdef __cplusplus
}
#endif

#endif
