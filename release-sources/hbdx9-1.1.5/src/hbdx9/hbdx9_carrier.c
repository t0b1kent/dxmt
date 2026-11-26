/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 — unix-call carrier hbdx9.dll (builtin-marked, paired by Wine with hbdx9.so).  Lane HBDX9, 07.11.2026.
 *
 * Why a separate DLL: Wine searches its own lib/wine directory BEFORE WINEDLLPATH for builtins
 * (dlls/ntdll/unix/loader.c set_dll_path), so a builtin d3d9.dll of ours is shadowed by Wine's.  d3d9.dll is therefore
 * a NATIVE module (layer copy in syswow64/system32, override d3d9=n,b) and only this carrier — a name Wine does not
 * have — is builtin, which is what lets the loader pair it with the native core.  Same pattern as DXMT's winemetal.dll. */
#include <windows.h>
#include "../winemetal/wineunixlib.h"

NTSTATUS WINAPI HBDX9Call(unsigned int code, void *params) { return WINE_UNIX_CALL(code, params); }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  DisableThreadLibraryCalls(instance);
  return !__wine_init_unix_call();
}
