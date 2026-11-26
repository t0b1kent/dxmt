// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../winemetal_thunks.h"
#include <cstddef>
static_assert(sizeof(WMTArgumentBinding)==40);
static_assert(offsetof(WMTArgumentBinding,index)==0);
static_assert(offsetof(WMTArgumentBinding,kind)==8);
static_assert(offsetof(WMTArgumentBinding,resource)==16);
static_assert(offsetof(WMTArgumentBinding,offset)==24);
static_assert(offsetof(WMTArgumentBinding,length)==32);
static_assert(sizeof(unixcall_mtlfunction_newargumentbuffer)==56);
static_assert(offsetof(unixcall_mtlfunction_newargumentbuffer,bindings)==16);
static_assert(offsetof(unixcall_mtlfunction_newargumentbuffer,count)==24);
static_assert(offsetof(unixcall_mtlfunction_newargumentbuffer,info)==32);
static_assert(offsetof(unixcall_mtlfunction_newargumentbuffer,ret)==40);
static_assert(offsetof(unixcall_mtlfunction_newargumentbuffer,status)==48);
static_assert(sizeof(WMTBufferInfo)==32);
static_assert(offsetof(WMTBufferInfo,memory)==16);
static_assert(offsetof(WMTBufferInfo,gpu_address)==24);
static_assert(sizeof(WMTComputeBinding)==64);
static_assert(offsetof(WMTComputeBinding,kind)==8);
static_assert(offsetof(WMTComputeBinding,alignment)==24);
static_assert(offsetof(WMTComputeBinding,depth)==56);
static_assert(sizeof(WMTComputeBindingLayout)==10192);
static_assert(offsetof(WMTComputeBindingLayout,bindings)==16);
static_assert(sizeof(unixcall_mtl_reflected_compute)==40);
static_assert(offsetof(unixcall_mtl_reflected_compute,layout)==16);
static_assert(offsetof(unixcall_mtl_reflected_compute,ret)==24);
static_assert(offsetof(unixcall_mtl_reflected_compute,status)==32);
static_assert(sizeof(unixcall_mtl_validate_compute)==40);
static_assert(offsetof(unixcall_mtl_validate_compute,requirements)==8);
static_assert(offsetof(unixcall_mtl_validate_compute,bindings)==16);
static_assert(offsetof(unixcall_mtl_validate_compute,count)==24);
static_assert(offsetof(unixcall_mtl_validate_compute,status)==32);
int argument_layout_cpu_only() { return 0; }
