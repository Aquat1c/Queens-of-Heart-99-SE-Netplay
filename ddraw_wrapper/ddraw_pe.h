#ifndef QOH99_DDRAW_PE_H
#define QOH99_DDRAW_PE_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QOH99_IAT_ERR_ARGS       (-1)
#define QOH99_IAT_ERR_IMAGE      (-2)
#define QOH99_IAT_ERR_DIRECTORY  (-3)
#define QOH99_IAT_ERR_PROTECT    (-4)
#define QOH99_IAT_ERR_TXN_FULL   (-5)
#define QOH99_IAT_ERR_IDENTITY   (-6)

#define QOH99_IAT_TXN_MAX 32

typedef struct Qoh99IatSlot {
    void **slot;
    void  *previous;
} Qoh99IatSlot;

typedef struct Qoh99IatTxn {
    Qoh99IatSlot entries[QOH99_IAT_TXN_MAX];
    int          count;
} Qoh99IatTxn;

void qoh99_iat_txn_begin(Qoh99IatTxn *txn);
int  qoh99_iat_txn_rollback(Qoh99IatTxn *txn);
void qoh99_iat_txn_commit(Qoh99IatTxn *txn);

int qoh99_iat_patch_module(Qoh99IatTxn *txn, HMODULE module,
                           const char *dll, const char *fn,
                           void *replacement, void **original,
                           char *err, size_t errCap);

int qoh99_iat_patch(HMODULE image, const char *dll, const char *fn,
                    void *replacement, void **original,
                    char *err, size_t errCap);

int qoh99_pe_inspect_module(const void *moduleBase, unsigned long *sizeOfImage,
                            char *err, size_t errCap);
int qoh99_pe_inspect_file(const void *bytes, size_t len,
                          unsigned long *sizeOfImage, char *err, size_t errCap);

#ifdef __cplusplus
}
#endif

#endif
