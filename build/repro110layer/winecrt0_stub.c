// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
/* Stand-in for Wine's libwinecrt0 (dlls/winecrt0/unix_lib.c). It only lets winemetal.dll link:
   d3d12.dll is linked against the import library of winemetal.dll, which depends on that DLL's
   export table and nothing else. The stand-in is never loaded or run. */
typedef unsigned long long unixlib_handle_t;
unixlib_handle_t __wine_unixlib_handle;
long (__stdcall *__wine_unix_call_dispatcher)(unixlib_handle_t, unsigned int, void *);
long __cdecl __wine_init_unix_call(void) { return 0; }
