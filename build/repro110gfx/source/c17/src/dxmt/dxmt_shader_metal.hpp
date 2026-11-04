#pragma once
#include "Metal.hpp"

namespace dxmt {
// Shared OS/GPU/config selection without the D3D11 encoder implementation.
WMTMetalVersion GetShaderMetalVersion(WMT::Device device);
}
