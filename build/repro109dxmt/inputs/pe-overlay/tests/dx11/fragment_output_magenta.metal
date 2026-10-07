#include <metal_stdlib>
using namespace metal;

struct MacRunnerMagentaOut {
  float4 color [[color(0)]];
};

fragment MacRunnerMagentaOut macrunner_probe_opaque_magenta() {
  MacRunnerMagentaOut out;
  out.color = float4(1.0, 0.0, 1.0, 1.0);
  return out;
}
