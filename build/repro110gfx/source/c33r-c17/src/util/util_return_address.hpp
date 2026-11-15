/* Modified 2026 by the MacRunner project (32-bit x86 build); Copyright (c) 2026 Timur Ravilov for the changes; see README-MACRUNNER.md */ #pragma once
// MacRunner c16 (17.10.2026): return addresses for the default-off swapchain/capability traces ported from the
// shipped x86-64 DXMT (MACRUNNER_DXMT_SWAPCHAIN_TRACE / CAP_TRACE). Those traces evaluate their arguments even when
// disabled. On ARM64EC the frame-record chain leaves native code at the entry thunk of an emulated x86-64 caller,
// so __builtin_return_address(n > 0) walks into a NULL/foreign frame: MTLDXGIFactory::CreateSwapChain faulted on a
// read of 0x8 (A/B ab3, 17.10). n == 0 is the link register and stays exact; deeper levels are nullptr on ARM64EC.
// HBDX9 07.11.2026: same fault on i386 (WoW64/FEX32): callers built without frame pointers -> CreateSwapChain read of 0
// at dxgi+0x3B4D (probe p11-c17).  Deeper levels are nullptr on i386 too.
#if defined(__arm64ec__) || defined(_M_ARM64EC) || defined(__i386__) || defined(_M_IX86)
#define DXMT_RETURN_ADDRESS(n) ((n) == 0 ? __builtin_return_address(0) : static_cast<void *>(nullptr))
#else
#define DXMT_RETURN_ADDRESS(n) __builtin_return_address(n)
#endif
