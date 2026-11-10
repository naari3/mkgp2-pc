/* MinGW-w64 spells it in lower case; sources written for MSVC do not. */
#include <windows.h>
/* In kernel32 since Windows 10 1607, absent from MinGW-w64 11 headers. */
#ifdef __cplusplus
extern "C"
#endif
HRESULT WINAPI SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription);
