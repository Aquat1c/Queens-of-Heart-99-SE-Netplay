#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "ddraw_pe.h"

#include "platform/PeImage.h"

namespace {

void copy_error(char *err, size_t errCap, const std::string &text)
{
    if (!err || errCap == 0)
        return;
    const size_t n = text.size() < (errCap - 1) ? text.size() : (errCap - 1);
    if (n != 0)
        std::memcpy(err, text.c_str(), n);
    err[n] = '\0';
}

void set_error(char *err, size_t errCap, const char *text)
{
    if (!err || errCap == 0)
        return;
    std::snprintf(err, errCap, "%s", text ? text : "");
}

bool error_is_not_found(const std::string &text)
{
    return text.find("was not found") != std::string::npos;
}

bool slot_inside_image(const void *target, uintptr_t base, unsigned long size)
{
    const uintptr_t value = reinterpret_cast<uintptr_t>(target);
    return base != 0 && value >= base
        && value < base + static_cast<uintptr_t>(size);
}

int txn_record(Qoh99IatTxn *txn, void **slot, void *previous)
{
    if (txn == nullptr)
        return 1;
    if (txn->count >= QOH99_IAT_TXN_MAX)
        return 0;
    txn->entries[txn->count].slot = slot;
    txn->entries[txn->count].previous = previous;
    ++txn->count;
    return 1;
}

}

extern "C" void qoh99_iat_txn_begin(Qoh99IatTxn *txn)
{
    if (txn != nullptr)
        std::memset(txn, 0, sizeof(*txn));
}

extern "C" int qoh99_iat_txn_rollback(Qoh99IatTxn *txn)
{
    int restored = 0;

    if (txn == nullptr)
        return 0;
    for (int i = txn->count - 1; i >= 0; --i) {
        qplat::IatSlot slot;
        std::string error;
        slot.slot = txn->entries[i].slot;
        slot.original = txn->entries[i].previous;
        if (slot.slot == nullptr || slot.original == nullptr)
            continue;
        if (qplat::restoreImportSlot(slot, error))
            ++restored;
    }
    txn->count = 0;
    return restored;
}

extern "C" void qoh99_iat_txn_commit(Qoh99IatTxn *txn)
{
    if (txn != nullptr)
        txn->count = 0;
}

extern "C" int qoh99_iat_patch_module(Qoh99IatTxn *txn, HMODULE module,
                                      const char *dll, const char *fn,
                                      void *replacement, void **original,
                                      char *err, size_t errCap)
{
    if (err != nullptr && errCap != 0)
        err[0] = '\0';
    if (module == nullptr || dll == nullptr || fn == nullptr
        || replacement == nullptr) {
        set_error(err, errCap, "invalid arguments");
        return QOH99_IAT_ERR_ARGS;
    }

    qplat::PeImageInfo info;
    std::string error;
    if (!qplat::inspectLoadedImage(static_cast<void *>(module), info, error)) {
        copy_error(err, errCap, error);
        return QOH99_IAT_ERR_IMAGE;
    }

    qplat::IatSlot slot;
    if (!qplat::findImportSlot(static_cast<void *>(module), dll, fn, slot,
                               error)) {
        copy_error(err, errCap, error);
        return error_is_not_found(error) ? 0 : QOH99_IAT_ERR_DIRECTORY;
    }
    if (slot.slot == nullptr) {
        set_error(err, errCap, "parser returned an empty slot");
        return QOH99_IAT_ERR_DIRECTORY;
    }

    void *const current = *slot.slot;
    if (current == replacement) {
        set_error(err, errCap, "slot already carries this patch");
        return 0;
    }
    if (slot_inside_image(current, info.base, info.sizeOfImage)) {
        char text[192];
        std::snprintf(text, sizeof(text),
                      "%.48s!%.48s currently points inside the host image (%p);"
                      " refusing to hook a forwarder/trampoline",
                      dll, fn, current);
        set_error(err, errCap, text);
        return QOH99_IAT_ERR_IDENTITY;
    }

    if (!txn_record(txn, slot.slot, current)) {
        char text[96];
        std::snprintf(text, sizeof(text), "IAT transaction is full (%d slots)",
                      QOH99_IAT_TXN_MAX);
        set_error(err, errCap, text);
        return QOH99_IAT_ERR_TXN_FULL;
    }

    if (!qplat::patchImportSlot(slot, replacement, error)) {
        copy_error(err, errCap, error);
        if (txn != nullptr && txn->count > 0)
            --txn->count;
        return QOH99_IAT_ERR_PROTECT;
    }
    if (!error.empty())
        copy_error(err, errCap, error);

    if (original != nullptr && *original == nullptr)
        *original = current;
    return 1;
}

extern "C" int qoh99_iat_patch(HMODULE image, const char *dll, const char *fn,
                               void *replacement, void **original,
                               char *err, size_t errCap)
{
    Qoh99IatTxn txn;
    qoh99_iat_txn_begin(&txn);
    const int result = qoh99_iat_patch_module(&txn, image, dll, fn, replacement,
                                              original, err, errCap);
    if (result < 0)
        qoh99_iat_txn_rollback(&txn);
    else
        qoh99_iat_txn_commit(&txn);
    return result;
}

extern "C" int qoh99_pe_inspect_module(const void *moduleBase,
                                       unsigned long *sizeOfImage,
                                       char *err, size_t errCap)
{
    qplat::PeImageInfo info;
    std::string error;

    if (err != nullptr && errCap != 0)
        err[0] = '\0';
    if (sizeOfImage != nullptr)
        *sizeOfImage = 0;
    if (!qplat::inspectLoadedImage(const_cast<void *>(moduleBase), info,
                                   error)) {
        copy_error(err, errCap, error);
        return 0;
    }
    if (sizeOfImage != nullptr)
        *sizeOfImage = info.sizeOfImage;
    return 1;
}

extern "C" int qoh99_pe_inspect_file(const void *bytes, size_t len,
                                     unsigned long *sizeOfImage,
                                     char *err, size_t errCap)
{
    qplat::PeImageInfo info;
    std::string error;

    if (err != nullptr && errCap != 0)
        err[0] = '\0';
    if (sizeOfImage != nullptr)
        *sizeOfImage = 0;
    if (!qplat::inspectFileImage(static_cast<const uint8_t *>(bytes), len, info,
                                 error)) {
        copy_error(err, errCap, error);
        return 0;
    }
    if (sizeOfImage != nullptr)
        *sizeOfImage = info.sizeOfImage;
    return 1;
}
