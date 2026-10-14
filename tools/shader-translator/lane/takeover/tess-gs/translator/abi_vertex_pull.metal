struct spvVertexBufferEntry { ulong base; uint stride; uint length; };
static inline __attribute__((always_inline))
uint4 spvPull(constant spvVertexBufferEntry* table, uint entry, uint offset, uint format, uint step, uint vid, uint iid, uint binst)
{
    ulong base = table[entry].base;
    if (base == 0ul)
        return uint4(0u);
    uint index = step == 0xffffffffu ? vid : (step == 0u ? binst : binst + (iid - binst) / step);
    ulong at = base + ulong(table[entry].stride * index + offset);
#define SPV_AT(T) (*reinterpret_cast<const device T*>(at))
#define SPV_I(v) as_type<uint4>(v)
#define SPV_F(v) as_type<uint4>(v)
#define SPV_U8X4 as_type<uint>(uchar4(SPV_AT(packed_uchar4)))
#define SPV_U8X2 uint(as_type<ushort>(uchar2(SPV_AT(packed_uchar2))))
#define SPV_U16X2 as_type<uint>(ushort2(SPV_AT(packed_ushort2)))
    switch (format)
    {
    case 46: return SPV_I(int4(int(SPV_AT(char)), 0, 0, 1));
    case 4:  return SPV_I(int4(int2(char2(SPV_AT(packed_char2))), 0, 1));
    case 6:  return SPV_I(int4(char4(SPV_AT(packed_char4))));
    case 45: return SPV_I(int4(int(SPV_AT(uchar)), 0, 0, 1));
    case 1:  return SPV_I(int4(int2(uchar2(SPV_AT(packed_uchar2))), 0, 1));
    case 3:  return SPV_I(int4(uchar4(SPV_AT(packed_uchar4))));
    case 50: return SPV_I(int4(int(SPV_AT(short)), 0, 0, 1));
    case 16: return SPV_I(int4(int2(short2(SPV_AT(packed_short2))), 0, 1));
    case 18: return SPV_I(int4(short4(SPV_AT(packed_short4))));
    case 49: return SPV_I(int4(int(SPV_AT(ushort)), 0, 0, 1));
    case 13: return SPV_I(int4(int2(ushort2(SPV_AT(packed_ushort2))), 0, 1));
    case 15: return SPV_I(int4(ushort4(SPV_AT(packed_ushort4))));
    case 32: case 36: return SPV_I(int4(SPV_AT(int), 0, 0, 1));
    case 33: case 37: return SPV_I(int4(int2(SPV_AT(packed_int2)), 0, 1));
    case 34: case 38: return SPV_I(int4(int3(SPV_AT(packed_int3)), 1));
    case 35: case 39: return SPV_I(int4(SPV_AT(packed_int4)));
    case 53: return SPV_F(float4(float(SPV_AT(half)), 0.0, 0.0, 1.0));
    case 25: return SPV_F(float4(float2(half2(SPV_AT(packed_half2))), 0.0, 1.0));
    case 27: return SPV_F(float4(half4(SPV_AT(packed_half4))));
    case 28: return SPV_F(float4(SPV_AT(float), 0.0, 0.0, 1.0));
    case 29: return SPV_F(float4(float2(SPV_AT(packed_float2)), 0.0, 1.0));
    case 30: return SPV_F(float4(float3(SPV_AT(packed_float3)), 1.0));
    case 31: return SPV_F(float4(SPV_AT(packed_float4)));
    case 47: return SPV_F(float4(unpack_unorm4x8_to_float(uint(SPV_AT(uchar))).x, 0.0, 0.0, 1.0));
    case 7:  return SPV_F(float4(unpack_unorm4x8_to_float(SPV_U8X2).xy, 0.0, 1.0));
    case 9:  return SPV_F(unpack_unorm4x8_to_float(SPV_U8X4));
    case 42: return SPV_F(unpack_unorm4x8_to_float(SPV_U8X4).zyxw);
    case 48: return SPV_F(float4(unpack_snorm4x8_to_float(uint(SPV_AT(uchar))).x, 0.0, 0.0, 1.0));
    case 10: return SPV_F(float4(unpack_snorm4x8_to_float(SPV_U8X2).xy, 0.0, 1.0));
    case 12: return SPV_F(unpack_snorm4x8_to_float(SPV_U8X4));
    case 51: return SPV_F(float4(unpack_unorm2x16_to_float(uint(SPV_AT(ushort))).x, 0.0, 0.0, 1.0));
    case 19: return SPV_F(float4(unpack_unorm2x16_to_float(SPV_U16X2), 0.0, 1.0));
    case 21: return SPV_F(float4(unpack_unorm2x16_to_float(SPV_U16X2), unpack_unorm2x16_to_float(as_type<uint>(ushort2(*reinterpret_cast<const device packed_ushort2*>(at + 4ul))))));
    case 52: return SPV_F(float4(unpack_snorm2x16_to_float(uint(SPV_AT(ushort))).x, 0.0, 0.0, 1.0));
    case 22: return SPV_F(float4(unpack_snorm2x16_to_float(SPV_U16X2), 0.0, 1.0));
    case 24: return SPV_F(float4(unpack_snorm2x16_to_float(SPV_U16X2), unpack_snorm2x16_to_float(as_type<uint>(ushort2(*reinterpret_cast<const device packed_ushort2*>(at + 4ul))))));
    case 41: return SPV_F(unpack_unorm10a2_to_float(SPV_AT(uint)));
    // The compiler builtins behind the pixel types rg11b10f and rgb9e5 (metal_pixel): the same AIR functions
    // (air.unpack.unorm.rg11b10f / .rgb9e5) the SM5.0 path of the layer calls for these vertex formats.
    case 54: return SPV_F(float4(__metal_unpack_unorm_rg11b10f(SPV_AT(uint), float3()), 1.0));
    case 55: return SPV_F(float4(__metal_unpack_unorm_rgb9e5(SPV_AT(uint), float3()), 1.0));
    default: return uint4(0u); // Int1010102Normalized (40): no DXGI format maps to it
    }
#undef SPV_AT
#undef SPV_I
#undef SPV_F
#undef SPV_U8X4
#undef SPV_U8X2
#undef SPV_U16X2
}
