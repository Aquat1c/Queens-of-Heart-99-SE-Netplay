#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ddraw_pe.h"

static int g_failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++g_failures; \
    } \
} while (0)

#define CHECK_EQ(actual, expected) do { \
    long long actual_value = (long long)(actual); \
    long long expected_value = (long long)(expected); \
    if (actual_value != expected_value) { \
        fprintf(stderr, "FAIL %s:%d: %s=%lld expected %lld\n", \
            __FILE__, __LINE__, #actual, actual_value, expected_value); \
        ++g_failures; \
    } \
} while (0)

#define IMG_BYTES     0x2000u
#define RVA_NT        0x0080u
#define RVA_DESC      0x0400u
#define RVA_OFT0      0x0480u
#define RVA_IAT0      0x04A0u
#define RVA_DLLNAME0  0x04C0u
#define RVA_OFT1      0x0500u
#define RVA_IAT1      0x0520u
#define RVA_DLLNAME1  0x0540u
#define RVA_FN_OTHER  0x0600u
#define RVA_FN_TARGET 0x0620u

static const char *const kDll0 = "OTHER.dll";
static const char *const kDll1 = "TESTDLL.dll";
static const char *const kFnOther = "OtherFunc";
static const char *const kFnTarget = "TargetFunc";

static volatile long g_fixture_target_mark;
static volatile long g_fixture_other_mark;
static volatile long g_fixture_replacement_mark;

static void fixture_target_func(void) { g_fixture_target_mark += 11; }
static void fixture_other_func(void) { g_fixture_other_mark += 22; }
static void fixture_replacement(void) { g_fixture_replacement_mark += 33; }

#define REPLACEMENT ((void *)(ULONG_PTR)&fixture_replacement)
#define ORIGINAL_TARGET ((DWORD)(ULONG_PTR)&fixture_target_func)
#define ORIGINAL_OTHER  ((DWORD)(ULONG_PTR)&fixture_other_func)

static unsigned char *image_alloc(void)
{
    unsigned char *p = (unsigned char *)VirtualAlloc(
        NULL, IMG_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p != NULL)
        memset(p, 0, IMG_BYTES);
    return p;
}

static void image_free(unsigned char *p)
{
    if (p != NULL)
        VirtualFree(p, 0, MEM_RELEASE);
}

static IMAGE_NT_HEADERS32 *image_nt(unsigned char *img)
{
    return (IMAGE_NT_HEADERS32 *)(void *)(img + RVA_NT);
}

static IMAGE_IMPORT_DESCRIPTOR *image_desc(unsigned char *img, unsigned index)
{
    return (IMAGE_IMPORT_DESCRIPTOR *)(void *)(img + RVA_DESC) + index;
}

static DWORD *image_dword(unsigned char *img, unsigned rva)
{
    return (DWORD *)(void *)(img + rva);
}

static unsigned char *image_build_valid(void)
{
    unsigned char *img = image_alloc();
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;

    if (img == NULL)
        return NULL;

    dos = (IMAGE_DOS_HEADER *)(void *)img;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = (LONG)RVA_NT;

    nt = image_nt(img);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader =
        (WORD)sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfHeaders = 0x400;
    nt->OptionalHeader.SizeOfImage = IMG_BYTES;
    nt->OptionalHeader.AddressOfEntryPoint = 0x1000;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
        .VirtualAddress = RVA_DESC;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
        (DWORD)(3 * sizeof(IMAGE_IMPORT_DESCRIPTOR));

    image_desc(img, 0)->OriginalFirstThunk = RVA_OFT0;
    image_desc(img, 0)->Name = RVA_DLLNAME0;
    image_desc(img, 0)->FirstThunk = RVA_IAT0;
    image_desc(img, 1)->OriginalFirstThunk = RVA_OFT1;
    image_desc(img, 1)->Name = RVA_DLLNAME1;
    image_desc(img, 1)->FirstThunk = RVA_IAT1;

    *image_dword(img, RVA_OFT0 + 0) = RVA_FN_OTHER;
    *image_dword(img, RVA_OFT0 + 4) = 0;
    *image_dword(img, RVA_IAT0 + 0) = ORIGINAL_OTHER;
    *image_dword(img, RVA_IAT0 + 4) = 0;

    *image_dword(img, RVA_OFT1 + 0) = RVA_FN_TARGET;
    *image_dword(img, RVA_OFT1 + 4) = RVA_FN_OTHER;
    *image_dword(img, RVA_OFT1 + 8) = 0;
    *image_dword(img, RVA_IAT1 + 0) = ORIGINAL_TARGET;
    *image_dword(img, RVA_IAT1 + 4) = ORIGINAL_OTHER;
    *image_dword(img, RVA_IAT1 + 8) = 0;

    memcpy(img + RVA_DLLNAME0, kDll0, strlen(kDll0) + 1);
    memcpy(img + RVA_DLLNAME1, kDll1, strlen(kDll1) + 1);
    memcpy(img + RVA_FN_OTHER + sizeof(WORD), kFnOther, strlen(kFnOther) + 1);
    memcpy(img + RVA_FN_TARGET + sizeof(WORD), kFnTarget, strlen(kFnTarget) + 1);
    return img;
}

static int inspect_file(unsigned char *img, size_t len, char *err, size_t cap)
{
    unsigned long size = 0;
    return qoh99_pe_inspect_file(img, len, &size, err, cap);
}

static int patch_target(unsigned char *img, Qoh99IatTxn *txn, void **original,
                        char *err, size_t cap)
{
    return qoh99_iat_patch_module(txn, (HMODULE)(void *)img, kDll1, kFnTarget,
                                  REPLACEMENT, original, err, cap);
}

static void test_fixture_addresses_distinct(void)
{
    CHECK(&fixture_target_func != &fixture_other_func);
    CHECK(&fixture_target_func != &fixture_replacement);
    CHECK(&fixture_other_func != &fixture_replacement);
    if (&fixture_target_func == &fixture_replacement) {
        fprintf(stderr,
                "note: the linker folded the fixture stubs onto one address "
                "(%p); every patch would degrade to 'already patched'\n",
                (void *)(ULONG_PTR)&fixture_target_func);
    }
}

static void test_valid_header_accepted(void)
{
    unsigned char *img = image_build_valid();
    unsigned long size = 0;
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    CHECK_EQ(qoh99_pe_inspect_file(img, IMG_BYTES, &size, err, sizeof(err)), 1);
    CHECK_EQ(size, IMG_BYTES);
    CHECK_EQ(qoh99_pe_inspect_module(img, &size, err, sizeof(err)), 1);
    CHECK_EQ(size, IMG_BYTES);
    image_free(img);
}

static void test_truncated_dos(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    CHECK_EQ(inspect_file(img, 10, err, sizeof(err)), 0);
    CHECK(err[0] != '\0');
    image_free(img);
}

static void test_bad_dos_signature(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    ((IMAGE_DOS_HEADER *)(void *)img)->e_magic = 0x1234;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);
}

static void test_bad_lfanew(void)
{
    static const LONG kBad[] = {
        -4,
        0,
        (LONG)(RVA_NT + 1),
        0x00100000,
        0x7FFFFFF0
    };
    unsigned i;

    for (i = 0; i < sizeof(kBad) / sizeof(kBad[0]); ++i) {
        unsigned char *img = image_build_valid();
        char err[256];
        CHECK(img != NULL);
        if (img == NULL)
            continue;
        ((IMAGE_DOS_HEADER *)(void *)img)->e_lfanew = kBad[i];
        CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
        CHECK(err[0] != '\0');
        image_free(img);
    }
}

static void test_bad_nt_signature(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->Signature = 0x11223344;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);
}

static void test_pe32plus_and_machine(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader.Magic = 0x020B;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);

    img = image_build_valid();
    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->FileHeader.Machine = 0x8664;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);
}

static void test_short_optional_header(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->FileHeader.SizeOfOptionalHeader = 0x40;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);
}

static void test_bad_size_of_image(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader.SizeOfImage = 0;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);

    img = image_build_valid();
    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader.SizeOfImage = 0x7FFFFFFFu;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);

    img = image_build_valid();
    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader.AddressOfEntryPoint = IMG_BYTES + 0x1000u;
    CHECK_EQ(inspect_file(img, IMG_BYTES, err, sizeof(err)), 0);
    image_free(img);
}

static void test_import_directory_out_of_range(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress =
            IMG_BYTES + 0x1000u;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_DIRECTORY);
    CHECK(err[0] != '\0');
    image_free(img);

    img = image_build_valid();
    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress =
            IMG_BYTES - 8u;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_DIRECTORY);
    image_free(img);
}

static void test_no_import_directory(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader.NumberOfRvaAndSizes =
        IMAGE_DIRECTORY_ENTRY_IMPORT;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_DIRECTORY);
    image_free(img);

    img = image_build_valid();
    CHECK(img != NULL);
    if (img == NULL)
        return;
    image_nt(img)->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 4;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_DIRECTORY);
    image_free(img);
}

static void test_directory_size_is_a_real_bound(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;

    image_nt(img)->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size =
            (DWORD)sizeof(IMAGE_IMPORT_DESCRIPTOR);

    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)), 0);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    CHECK_EQ(qoh99_iat_patch_module(NULL, (HMODULE)(void *)img, kDll0,
                                    kFnOther, REPLACEMENT, NULL, err,
                                    sizeof(err)),
             1);
    image_free(img);
}

static void test_thunk_array_off_image(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;

    image_desc(img, 1)->OriginalFirstThunk = IMG_BYTES - 4u;
    image_desc(img, 1)->FirstThunk = IMG_BYTES - 8u;
    *image_dword(img, IMG_BYTES - 4u) = RVA_FN_OTHER;
    *image_dword(img, IMG_BYTES - 8u) = ORIGINAL_OTHER;

    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)), 0);
    CHECK(err[0] != '\0');
    image_free(img);
}

static void test_dll_name_unterminated(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;

    image_desc(img, 1)->Name = IMG_BYTES - 2u;
    img[IMG_BYTES - 2u] = 'A';
    img[IMG_BYTES - 1u] = 'B';

    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)), 0);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    image_free(img);
}

static void test_import_name_unterminated(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;

    *image_dword(img, RVA_OFT1 + 0) = IMG_BYTES - 3u;
    img[IMG_BYTES - 3u] = 0;
    img[IMG_BYTES - 2u] = 0;
    img[IMG_BYTES - 1u] = 'T';
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)), 0);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    image_free(img);

    img = image_build_valid();
    CHECK(img != NULL);
    if (img == NULL)
        return;
    *image_dword(img, RVA_OFT1 + 0) = IMG_BYTES - 1u;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)), 0);
    image_free(img);
}

static void test_ordinal_import_skipped(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;

    *image_dword(img, RVA_OFT1 + 0) = IMAGE_ORDINAL_FLAG32 | 7u;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)), 0);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    image_free(img);
}

static void test_bound_descriptor_refused(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;

    image_desc(img, 1)->OriginalFirstThunk = 0;
    image_desc(img, 1)->TimeDateStamp = 0xFFFFFFFFu;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_DIRECTORY);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    image_free(img);
}

static void test_unusable_slot_values(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    *image_dword(img, RVA_IAT1 + 0) = 0;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_DIRECTORY);
    image_free(img);

    img = image_build_valid();
    CHECK(img != NULL);
    if (img == NULL)
        return;
    *image_dword(img, RVA_IAT1 + 0) = 0xDEAD0001u;
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_DIRECTORY);
    image_free(img);
}

static void test_forwarder_identity_refused(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;
    if ((ULONG_PTR)img > 0xFFFFFFFFul) {
        image_free(img);
        return;
    }

    *image_dword(img, RVA_IAT1 + 0) = (DWORD)((ULONG_PTR)img + 0x100u);
    CHECK_EQ(patch_target(img, NULL, NULL, err, sizeof(err)),
             QOH99_IAT_ERR_IDENTITY);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0),
             (DWORD)((ULONG_PTR)img + 0x100u));
    image_free(img);
}

static void test_patch_and_rollback(void)
{
    unsigned char *img = image_build_valid();
    char err[256];
    void *original = NULL;
    Qoh99IatTxn txn;

    CHECK(img != NULL);
    if (img == NULL)
        return;

    qoh99_iat_txn_begin(&txn);
    CHECK_EQ(patch_target(img, &txn, &original, err, sizeof(err)), 1);
    CHECK_EQ(txn.count, 1);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), (DWORD)(ULONG_PTR)REPLACEMENT);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 4), ORIGINAL_OTHER);
    CHECK_EQ((ULONG_PTR)original, (ULONG_PTR)ORIGINAL_TARGET);

    CHECK_EQ(qoh99_iat_txn_rollback(&txn), 1);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    CHECK_EQ(txn.count, 0);

    original = NULL;
    qoh99_iat_txn_begin(&txn);
    CHECK_EQ(patch_target(img, &txn, &original, err, sizeof(err)), 1);
    CHECK_EQ(patch_target(img, &txn, &original, err, sizeof(err)), 0);
    CHECK_EQ(txn.count, 1);
    CHECK_EQ(qoh99_iat_txn_rollback(&txn), 1);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    image_free(img);
}

static void test_multi_slot_rollback(void)
{
    unsigned char *img = image_build_valid();
    char err[256];
    Qoh99IatTxn txn;

    CHECK(img != NULL);
    if (img == NULL)
        return;

    qoh99_iat_txn_begin(&txn);
    CHECK_EQ(patch_target(img, &txn, NULL, err, sizeof(err)), 1);
    CHECK_EQ(qoh99_iat_patch_module(&txn, (HMODULE)(void *)img, kDll0,
                                    kFnOther, REPLACEMENT, NULL, err,
                                    sizeof(err)),
             1);
    CHECK_EQ(txn.count, 2);
    CHECK_EQ(qoh99_iat_txn_rollback(&txn), 2);
    CHECK_EQ(*image_dword(img, RVA_IAT1 + 0), ORIGINAL_TARGET);
    CHECK_EQ(*image_dword(img, RVA_IAT0 + 0), ORIGINAL_OTHER);
    image_free(img);
}

static void test_name_matching(void)
{
    unsigned char *img = image_build_valid();
    char err[256];

    CHECK(img != NULL);
    if (img == NULL)
        return;

    CHECK_EQ(qoh99_iat_patch_module(NULL, (HMODULE)(void *)img, "testdll",
                                    kFnTarget, REPLACEMENT, NULL, err,
                                    sizeof(err)),
             1);
    CHECK_EQ(qoh99_iat_patch_module(NULL, (HMODULE)(void *)img, kDll1,
                                    "NoSuchFunc", REPLACEMENT, NULL, err,
                                    sizeof(err)),
             0);
    CHECK(err[0] != '\0');
    CHECK_EQ(qoh99_iat_patch_module(NULL, (HMODULE)(void *)img, "nope.dll",
                                    kFnTarget, REPLACEMENT, NULL, err,
                                    sizeof(err)),
             0);
    image_free(img);
}

static void test_null_arguments(void)
{
    char err[256];
    unsigned long size = 0;

    CHECK_EQ(qoh99_iat_patch(NULL, "a.dll", "fn", REPLACEMENT, NULL, err,
                             sizeof(err)),
             QOH99_IAT_ERR_ARGS);
    CHECK_EQ(qoh99_pe_inspect_file(NULL, 0, &size, err, sizeof(err)), 0);
}

static void test_live_module(void)
{
    char err[256];
    unsigned long size = 0;
    HMODULE self = GetModuleHandleA(NULL);

    CHECK(self != NULL);
    if (self == NULL)
        return;
    CHECK_EQ(qoh99_pe_inspect_module((const void *)self, &size, err,
                                     sizeof(err)),
             1);
    CHECK(size > 0);
    CHECK_EQ(qoh99_iat_patch(self, "no-such-module.dll", "NoSuchFunction",
                             REPLACEMENT, NULL, err, sizeof(err)),
             0);
}

int main(void)
{
    test_fixture_addresses_distinct();
    test_valid_header_accepted();
    test_truncated_dos();
    test_bad_dos_signature();
    test_bad_lfanew();
    test_bad_nt_signature();
    test_pe32plus_and_machine();
    test_short_optional_header();
    test_bad_size_of_image();
    test_import_directory_out_of_range();
    test_no_import_directory();
    test_directory_size_is_a_real_bound();
    test_thunk_array_off_image();
    test_dll_name_unterminated();
    test_import_name_unterminated();
    test_ordinal_import_skipped();
    test_bound_descriptor_refused();
    test_unusable_slot_values();
    test_forwarder_identity_refused();
    test_patch_and_rollback();
    test_multi_slot_rollback();
    test_name_matching();
    test_null_arguments();
    test_live_module();

    if (g_failures != 0) {
        fprintf(stderr, "pe_parser_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("pe_parser_tests: all checks passed\n");
    return 0;
}
