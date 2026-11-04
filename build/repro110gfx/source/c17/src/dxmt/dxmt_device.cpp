#include "dxmt_device.hpp"
#include "dxmt_shader_metal.hpp"
#include "config/config.hpp"
#include "dxmt_command_queue.hpp"
#include "Metal.hpp"

namespace dxmt {

WMTMetalVersion
GetShaderMetalVersion(WMT::Device device) {
  int version_conf = Config::getInstance().getOption<int>("dxmt.shaderMetalVersion", 0);
  auto version = version_conf == WMTMetal310 || version_conf == WMTMetal320
                     ? (WMTMetalVersion)version_conf : WMTMetalVersionMax;
  uint64_t macos_major_version = 0;
  WMTGetOSVersion(&macos_major_version, nullptr, nullptr);
  version = std::min(macos_major_version >= 15 ? WMTMetal320 : WMTMetal310, version);
  return device.supportsFamily(WMTGPUFamilyApple7) ? version : WMTMetal310;
}

class DeviceImpl : public Device {
public:
  virtual WMT::Device
  device() override {
    return device_;
  };
  virtual CommandQueue &
  queue() override {
    return cmd_queue_;
  };

  virtual WMTMetalVersion metalVersion() override {
    return metal_version_;
  };

  virtual uint64_t maxObjectThreadgroups() final {
    return max_object_threadgroups_;
  };

  DeviceImpl(const DEVICE_DESC &desc) : device_(desc.device), cmd_queue_(device_) {
    uint64_t macos_major_version = 0, macos_minor_version = 0;
    metal_version_ = GetShaderMetalVersion(device_);
    WMTGetOSVersion(&macos_major_version, &macos_minor_version, nullptr);
    if (!device_.supportsFamily(WMTGPUFamilyApple7)) {
      WARN("Experimental non-Apple GPU support");
      max_object_threadgroups_ = 1024;
      // macOS 26 bug: setShouldMaximizeConcurrentCompilation crashes on AMDGPU
      if (!(macos_major_version >= 16 && macos_major_version <= 26 && macos_minor_version < 2))
        device_.setShouldMaximizeConcurrentCompilation(true);
    } else {
      max_object_threadgroups_ = -1ull;
      device_.setShouldMaximizeConcurrentCompilation(true);
    }
  }

private:
  WMT::Reference<WMT::Device> device_;
  CommandQueue cmd_queue_;
  WMTMetalVersion metal_version_;
  uint64_t max_object_threadgroups_;
};

std::unique_ptr<Device>
CreateDXMTDevice(const DEVICE_DESC &desc) {
  return std::make_unique<DeviceImpl>(desc);
};

} // namespace dxmt
