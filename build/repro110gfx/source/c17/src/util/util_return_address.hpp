#pragma once
// MacRunner c16 (17.10.2026): return addresses for the default-off swapchain/capability traces ported from the
// shipped x86-64 DXMT (MACRUNNER_DXMT_SWAPCHAIN_TRACE / CAP_TRACE). Those traces evaluate their arguments even when
// disabled. On ARM64EC the frame-record chain leaves native code at the entry thunk of an emulated x86-64 caller,
// so __builtin_return_address(n > 0) walks into a NULL/foreign frame: MTLDXGIFactory::CreateSwapChain faulted on a
// read of 0x8 (A/B ab3, 17.10). n == 0 is the link register and stays exact; deeper levels are nullptr on ARM64EC.
#if defined(__arm64ec__) || defined(_M_ARM64EC)
#define DXMT_RETURN_ADDRESS(n) ((n) == 0 ? __builtin_return_address(0) : static_cast<void *>(nullptr))
#else
#define DXMT_RETURN_ADDRESS(n) __builtin_return_address(n)
#endif
