#include <windows.h>

static DWORD disposition;

__declspec(dllexport) DWORD __cdecl qoh99np_presentation_disposition(void)
{
    return disposition;
}

__declspec(dllexport) void __cdecl test_set_disposition(DWORD value)
{
    disposition = value;
}
