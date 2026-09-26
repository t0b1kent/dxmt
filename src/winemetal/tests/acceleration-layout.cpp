// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../winemetal_thunks.h"
#include <cstddef>
static_assert(sizeof(WMTASTriangleGeometry)==80);
static_assert(offsetof(WMTASTriangleGeometry,index_type)==56);
static_assert(sizeof(WMTASBuildDesc)==80);
static_assert(offsetof(WMTASBuildDesc,acceleration_structures)==56);
static_assert(sizeof(WMTASUserIDInstance)==68);
static_assert(offsetof(WMTASUserIDInstance,user_id)==64);
static_assert(sizeof(WMTASSizeInfo)==24);
static_assert(sizeof(unixcall_mtl_as_sizes)==32);
static_assert(offsetof(unixcall_mtl_as_sizes,info)==16);
static_assert(sizeof(unixcall_mtl_as_new)==32);
static_assert(offsetof(unixcall_mtl_as_new,ret)==16);
static_assert(sizeof(unixcall_mtl_as_build)==56);
static_assert(offsetof(unixcall_mtl_as_build,status)==48);
static_assert(sizeof(wmtcmd_compute_setaccelerationstructure)==32);
static_assert(offsetof(wmtcmd_compute_setaccelerationstructure,index)==24);
int acceleration_layout_cpu_only(){return 0;}
