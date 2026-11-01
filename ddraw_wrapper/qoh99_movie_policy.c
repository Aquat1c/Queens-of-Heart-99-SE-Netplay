#include "qoh99_movie_policy.h"

#include <string.h>

static const char kOpeningLeafName[] = "opening.avi";

Qoh99MovieDecision qoh99_movie_policy_decide(int casterRequestedSkip,
                                            int movieFilePresent)
{
    Qoh99MovieDecision decision;
    decision.policy = QOH99_MOVIE_LEAVE_ALONE;
    decision.skipReason = QOH99_MOVIE_SKIP_NONE;

    if (!movieFilePresent)
        return decision;

    if (casterRequestedSkip) {
        decision.policy = QOH99_MOVIE_SKIP;
        decision.skipReason = QOH99_MOVIE_SKIP_REQUESTED;
        return decision;
    }

    decision.policy = QOH99_MOVIE_PLAY;
    return decision;
}

const char *qoh99_movie_policy_text(Qoh99MoviePolicy policy)
{
    switch (policy) {
        case QOH99_MOVIE_LEAVE_ALONE: return "leave the game's behaviour alone";
        case QOH99_MOVIE_PLAY:        return "play through DirectShow";
        case QOH99_MOVIE_SKIP:        return "skip as if the file were absent";
    }
    return "unknown";
}

const char *qoh99_movie_skip_reason_text(Qoh99MovieSkipReason reason)
{
    switch (reason) {
        case QOH99_MOVIE_SKIP_NONE:       return "";
        case QOH99_MOVIE_SKIP_REQUESTED:  return "the launcher asked for no intro";
        case QOH99_MOVIE_SKIP_UNPLAYABLE: return "nothing here can decode it";
    }
    return "unknown";
}

size_t qoh99_movie_skip_patch_encode(unsigned char *out, size_t cap,
                                     unsigned int filenameAddr)
{
    if (out == NULL || cap < QOH99_MOVIE_SKIP_PATCH_BYTES)
        return 0u;
    /* mov byte ptr ds:[filenameAddr], 0   -> C6 05 <addr32> 00 */
    out[0] = 0xC6;
    out[1] = 0x05;
    out[2] = (unsigned char)(filenameAddr & 0xFFu);
    out[3] = (unsigned char)((filenameAddr >> 8) & 0xFFu);
    out[4] = (unsigned char)((filenameAddr >> 16) & 0xFFu);
    out[5] = (unsigned char)((filenameAddr >> 24) & 0xFFu);
    out[6] = 0x00;
    /* xor eax, eax  -> 33 C0   (report "no movie found") */
    out[7] = 0x33;
    out[8] = 0xC0;
    out[9] = 0xC3;
    return QOH99_MOVIE_SKIP_PATCH_BYTES;
}

size_t qoh99_movie_skip_patch_expected(unsigned char *out, size_t cap)
{
    static const unsigned char kPrologue[] = {
        0x81, 0xEC, 0x08, 0x01, 0x00, 0x00,
        0xA1, 0x9C, 0x77, 0x4A, 0x00
    };
    if (out == NULL || cap < sizeof(kPrologue))
        return 0u;
    memcpy(out, kPrologue, sizeof(kPrologue));
    return sizeof(kPrologue);
}

int qoh99_movie_skip_patch_site_matches(const unsigned char *actual, size_t len)
{
    unsigned char expected[16];
    size_t have;

    if (actual == NULL)
        return 0;
    have = qoh99_movie_skip_patch_expected(expected, sizeof(expected));
    if (have == 0u || len < have)
        return 0;
    return memcmp(actual, expected, have) == 0 ? 1 : 0;
}

static char lowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int qoh99_movie_mci_open_is_ours(unsigned int msg, unsigned int flags,
                                 const char *elementName)
{
    size_t nameLen;
    size_t leafLen;
    size_t i;
    const char *leafStart;

    if (msg != QOH99_MCI_OPEN)
        return 0;
    if ((flags & QOH99_MCI_FLAG_OPEN_ELEMENT) == 0u || elementName == NULL)
        return 0;

    nameLen = strlen(elementName);
    leafLen = sizeof(kOpeningLeafName) - 1u;
    if (nameLen < leafLen)
        return 0;

    leafStart = elementName + (nameLen - leafLen);
    if (leafStart != elementName) {
        const char before = leafStart[-1];
        if (before != '\\' && before != '/' && before != ':')
            return 0;
    }
    for (i = 0; i < leafLen; ++i) {
        if (lowerAscii(leafStart[i]) != kOpeningLeafName[i])
            return 0;
    }
    return 1;
}

int qoh99_movie_mci_is_our_device(unsigned int deviceId)
{
    return deviceId == QOH99_MOVIE_MCI_DEVICE_ID ? 1 : 0;
}
