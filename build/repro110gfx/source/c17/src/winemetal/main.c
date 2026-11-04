#include "windef.h"
#include "winbase.h"
#include "wineunixlib.h"
#include <stdio.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  NTSTATUS status;

  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  status = __wine_init_unix_call();
  if (status)
    fprintf(stderr, "winemetal_init_unix_call status=0x%08lx\n",
            (unsigned long)status);
  return !status;
}

extern BOOL WINAPI DllMainCRTStartup(HANDLE hDllHandle, DWORD dwReason,
                                       LPVOID lpreserved);
