/* Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md */
#include "stddef.h"
#include "stdint.h"
#include "stdbool.h"

#ifndef __AIRCONV_H
#define __AIRCONV_H

#define AIRCONV_VERSION 27

#ifdef __cplusplus
#include <string>

enum class SM50BindingType : uint32_t {
  ConstantBuffer,
  Sampler,
  SRV,
  UAV,
};
#else
typedef uint32_t SM50BindingType;
#endif

enum SM50_BINDING_INDEX: uint32_t {
  SM50_BINDING_INDEX_CONSTANT_BUFFER = 0,
  SM50_BINDING_INDEX_ARGUMENT_TABLE = 1,
  SM50_BINDING_INDEX_VERTEX_BUFFER = 2,
  SM50_BINDING_INDEX_INDEX_BUFFER = 3,
  SM50_BINDING_INDEX_DRAW_ARGUMENTS = 4,
  SM50_BINDING_INDEX_INDIRECT_ARGUMENTS = 4,
  SM50_BINDING_INDEX_STREAM_OUTPUT0 = 5,
  /* For multiple stages in the same shader function */
  SM50_BINDING_INDEX_CONSTANT_BUFFER2 = 6,
  SM50_BINDING_INDEX_ARGUMENT_TABLE2 = 7,

  SM50_BINDING_INDEX_ROOT_ARGUMENTS = 0,
  SM50_BINDING_INDEX_STATIC_SAMPLERS = 1,
};

enum MTL_SM50_SHADER_ARGUMENT_FLAG : uint32_t {
  MTL_SM50_SHADER_ARGUMENT_BUFFER = 1 << 0,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE = 1 << 1,
  MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH = 1 << 2,
  MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER = 1 << 3,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP = 1 << 4,
  MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET = 1 << 5,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY = 1 << 6,
  MTL_SM50_SHADER_ARGUMENT_READ_ACCESS = 1 << 10,
  MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS = 1 << 11,
};

struct MTL_SM50_SHADER_ARGUMENT {
  SM50BindingType Type;
  /**
  bind point of it's corresponding resource space
  constant buffer:    cb1 -> 1
  srv:                t10 -> 10
  uav:                u0  -> 0
  sampler:            s2  -> 2
  */
  uint32_t SM50BindingSlot;
  enum MTL_SM50_SHADER_ARGUMENT_FLAG Flags;
  uint32_t StructurePtrOffset;
};

enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE {
  MTL_TESSELLATOR_OUTPUT_POINT = 1,
  MTL_TESSELLATOR_OUTPUT_LINE = 2,
  MTL_TESSELLATOR_OUTPUT_TRIANGLE_CW = 3,
  MTL_TESSELLATOR_TRIANGLE_CCW = 4
};

struct MTL_TESSELLATOR_REFLECTION {
  uint32_t Partition;
  float MaxFactor;
  enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE OutputPrimitive;
};

struct MTL_GEOMETRY_SHADER_PASS_THROUGH {
  uint8_t RenderTargetArrayIndexReg;
  uint8_t RenderTargetArrayIndexComponent;
  uint8_t ViewportArrayIndexReg;
  uint8_t ViewportArrayIndexComponent;
};

struct MTL_GEOMETRY_SHADER_REFLECTION {
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t GSPassThrough;
  };
  uint32_t Primitive;
};

struct MTL_POST_TESSELLATOR_REFLECTION {
  uint32_t MaxPotentialTessFactor;
  uint32_t MaxPotentialSplitTessFactor;
};

struct MTL_PIXEL_SHADER_REFLECTION {
  uint32_t ValidRenderTargets;
  uint32_t HasCoverageOutput;
};

struct MTL_SHADER_REFLECTION {
  uint32_t ConstanttBufferTableBindIndex;
  uint32_t ArgumentBufferBindIndex;
  uint32_t NumConstantBuffers;
  uint32_t NumArguments;
  union {
    uint32_t ThreadgroupSize[3];
    struct MTL_TESSELLATOR_REFLECTION Tessellator;
    struct MTL_GEOMETRY_SHADER_REFLECTION GeometryShader;
    struct MTL_POST_TESSELLATOR_REFLECTION PostTessellator;
    /**
    \deprecated for compatibility, use PixelShader.ValidRenderTargets instead
     */
    uint32_t PSValidRenderTargets;
    struct MTL_PIXEL_SHADER_REFLECTION PixelShader;
  };
  uint16_t ConstantBufferSlotMask;
  uint16_t SamplerSlotMask;
  uint64_t UAVSlotMask;
  uint64_t SRVSlotMaskHi;
  uint64_t SRVSlotMaskLo;
  uint32_t NumOutputElement;
  uint32_t ThreadsPerPatch;
  uint32_t ArgumentTableQwords;
};

#if defined(__LP64__) || defined(_WIN64)
typedef void *sm50_ptr64_t;
#else
typedef struct sm50_ptr64_t {
  uint64_t impl;

#ifdef __cplusplus
  sm50_ptr64_t() {
    impl = 0;
  }

  sm50_ptr64_t(void * ptr) {
    impl = (uint64_t)ptr;
  }

  sm50_ptr64_t(uint64_t v) {
    impl = v;
  }

  operator uint64_t () const {
    return impl;
  }
#endif

} sm50_ptr64_t;
#endif

typedef sm50_ptr64_t sm50_shader_t;
typedef sm50_ptr64_t sm50_bitcode_t;
typedef sm50_ptr64_t sm50_error_t;

#ifdef _WIN32
#ifdef WIN_EXPORT
#define AIRCONV_API __declspec(dllexport)
#else
#define AIRCONV_API __declspec(dllimport)
#endif
#else
#define AIRCONV_API __attribute__((sysv_abi))
#endif

struct SM50_COMPILED_BITCODE {
  sm50_ptr64_t Data;
  uint64_t Size;
};

#ifdef __cplusplus

inline uint32_t
GetArgumentIndex(SM50BindingType Type, uint32_t SM50BindingSlot) {
  switch (Type) {
  case SM50BindingType::ConstantBuffer:
    return SM50BindingSlot;
  case SM50BindingType::Sampler:
    return SM50BindingSlot + 32;
  case SM50BindingType::SRV:
    return SM50BindingSlot * 3 + 128;
  case SM50BindingType::UAV:
    return SM50BindingSlot * 3 + 512;
  }
};

inline uint32_t GetArgumentIndex(struct MTL_SM50_SHADER_ARGUMENT &Argument) {
  return GetArgumentIndex(Argument.Type, Argument.SM50BindingSlot);
};

extern "C" {
#endif

enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE {
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT = 1,
  SM50_SHADER_COMMON = 2,
  SM50_SHADER_PSO_PIXEL_SHADER = 3,
  SM50_SHADER_IA_INPUT_LAYOUT = 4,
  SM50_SHADER_GS_PASS_THROUGH = 5,
  SM50_SHADER_PSO_GEOMETRY_SHADER = 6,
  SM50_SHADER_PSO_TESSELLATOR = 7,
  SM50_SHADER_ROOT_SIGNATURE = 8,
  SM50_SHADER_ROOT_SIGNATURE2 = 9,
  SM50_SHADER_PS_INPUT_LINKAGE = 10,
  SM50_SHADER_TESS_SPLIT_WORKLOAD = 11,
  SM50_SHADER_TESS_VERTEX_LINKAGE = 12,
  SM50_SHADER_REUSE_AUDIT_QUERY = 13,
  SM50_SHADER_PS_AS_COMPUTE = 14, /* MacRunner POST-CS 25.10.2026, see SM50_SHADER_PS_AS_COMPUTE_DATA */
  SM50_SHADER_POST_CS_VS_KEY_QUERY = 15, /* MacRunner POST-CS, see SM50_SHADER_POST_CS_VS_KEY_DATA */
  /* 14, 15: MacRunner POST-CS (SM50_SHADER_PS_AS_COMPUTE, SM50_SHADER_POST_CS_VS_KEY_QUERY) */
  SM50_SHADER_TESS_PREPASS = 16, /* MacRunner GPU-PASS TESS_PREPASS, see SM50_SHADER_TESS_PREPASS_DATA */
  SM50_SHADER_ARGUMENT_TYPE_MAX = 0xffffffff,
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

/* Read-only source dependency query through the existing compile transport.
   Never append this node to a real compile/cache key. No Metal library is emitted. */
#define SM50_REUSE_AUDIT_APPLIED 0x52415531u
#define SM50_REUSE_AUDIT_ROWS 2048u
struct SM50_REUSE_AUDIT_ROW {
  uint32_t stage, space, binding, reg, bound, flags; /* flags: loaded=1, may-branch=2, may-loop-exit=4 */
};
struct SM50_SHADER_REUSE_AUDIT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t size, applied, count, unknown, overflow;
  struct SM50_REUSE_AUDIT_ROW rows[SM50_REUSE_AUDIT_ROWS];
};

/* Separate node preserves the existing tessellator argument ABI. Each compiler
   must acknowledge it so a stale provider cannot silently emit unsplit meshes. */
#define SM50_TESS_SPLIT_OBJECT_APPLIED 0x54535031u
#define SM50_TESS_SPLIT_DOMAIN_APPLIED 0x54535032u
struct SM50_SHADER_TESS_SPLIT_WORKLOAD_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t applied;
};

/* GPU-PASS TESS_PREPASS (25.10.2026, v2): compile a tessellation stage as a part of the compute pre-pass instead of the
   object / mesh function. HULL_KERNEL: the VS+HS object function as a kernel; its payload is one device slot per object
   threadgroup holding the control points / patch constants and, per emitted workload, a 4-byte record (patch | vertices << 8 |
   triangles << 16) in the mesh path's emission order. MATERIALIZE_KERNEL (built from HS+DS): one threadgroup per group,
   lane = patch, regenerates the workloads into a table (16 uints each: workload, group, 0) at their emission positions and
   writes the indices as (flat workload << 8 | local vertex). PULL_VERTEX: the domain body in a vertex function that takes its
   workload straight from the index value. FALLBACK_VERTEX: the domain body in a vertex function for a non-indexed draw that
   finds group / record / corner and regenerates the workload from the slot (no table) -- used when the table or the index
   buffer is too small. The compiler writes applied and the layout; a provider without this node returns the ordinary stage,
   so the caller requires applied == SM50_TESS_PREPASS_APPLIED. */
#define SM50_TESS_PREPASS_APPLIED 0x54505032u
enum SM50_TESS_PREPASS_MODE {
  SM50_TESS_PREPASS_HULL_KERNEL = 1,
  SM50_TESS_PREPASS_PULL_VERTEX = 4,
  SM50_TESS_PREPASS_MATERIALIZE_KERNEL = 5,
  SM50_TESS_PREPASS_FALLBACK_VERTEX = 6,
  SM50_TESS_PREPASS_DIAGNOSTIC_MESH = 7, /* mode-3 record observer; preserves the mesh function */
  SM50_TESS_PREPASS_LINEAR_PULL_VERTEX = 8, /* diagnostic: same table/index stream through non-indexed vertex invocations */
};
enum SM50_TESS_PREPASS_BINDING {
  SM50_TESS_PREPASS_BINDING_SLOTS = 16,     /* int[groups * slot_stride / 4] */
  SM50_TESS_PREPASS_BINDING_GROUPS = 17,    /* uint4[groups]: {workloads, vertices, indices, 0} */
  SM50_TESS_PREPASS_BINDING_OFFSETS = 19,   /* uint4[groups]: {first flat workload, vertex base, index base, 0} */
  SM50_TESS_PREPASS_BINDING_TABLE = 20,     /* uint[workloads * 16] */
  SM50_TESS_PREPASS_BINDING_INDICES = 21,   /* uint[] */
  SM50_TESS_PREPASS_BINDING_CONSTANTS = 22, /* uint4 {groups, 0, 0, 0} */
};
struct SM50_SHADER_TESS_PREPASS_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t mode;
  uint32_t applied;              /* out */
  uint32_t slot_stride;          /* out: bytes of one slot */
  uint32_t meta_offset;          /* out: byte offset of the workload records inside a slot */
  uint32_t workloads_per_group;  /* out: capacity of the records */
  uint32_t patches_per_group;    /* out */
  uint32_t threads_per_group;    /* out (hull): threadgroup size of the hull kernel */
  uint32_t reserved;
  uint32_t diagnostic_records;   /* in: bounded mode-3 observer, never enabled by the product gate alone */
  uint32_t diagnostic_stride;    /* out: uints per vertex record */
  uint32_t diagnostic_vertices;  /* out: maximum vertices of a whole workload */
};

/* A stale provider must not accept a relocated VS->HS interface unchanged. */
#define SM50_TESS_VERTEX_LINKAGE_APPLIED 0x54564c31u
struct SM50_SHADER_TESS_VERTEX_LINKAGE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t applied;
};

/* Rename AIR user attributes only; register contents and component lanes do not move. */
struct SM50_SHADER_PS_INPUT_LINK {
  uint32_t input_register, input_mask;
  uint32_t output_register, output_mask;
};

/* Written by the compiler, never by the caller. A provider that predates
   SM50_SHADER_PS_INPUT_LINKAGE walks past the unknown node and still reports
   success, so the caller would otherwise accept the original unlinked AIR. The
   caller zeroes this field, passes the node and requires the acknowledgement
   after a successful compile; a stale provider then fails the pipeline instead
   of rendering with a mismatched fragment input. */
#define SM50_SHADER_PS_INPUT_LINKAGE_APPLIED 0x50534c31u /* 'PSL1' */

struct SM50_SHADER_PS_INPUT_LINKAGE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t count;
  struct SM50_SHADER_PS_INPUT_LINK entries[32];
  uint32_t applied;
};

struct SM50_STREAM_OUTPUT_ELEMENT {
  uint32_t reg_id;
  uint32_t component;
  uint32_t output_slot;
  uint32_t offset;
};

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  struct SM50_STREAM_OUTPUT_ELEMENT *elements;
};

enum SM50_SHADER_METAL_VERSION {
  SM50_SHADER_METAL_310 = 310,
  SM50_SHADER_METAL_320 = 320,
  SM50_SHADER_METAL_MAX = 0xffffffff,
};

enum SM50_SHADER_FLAG {
  SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO = 1 << 0,
  SM50_SHADER_FLAG_DEFUSE_FMA = 1 << 1,
  /* Default-off MacRunner causal controls for one sealed diagnostic draw. */
  SM50_SHADER_FLAG_CAUSAL_FRAGMENT_INPUT_MAGENTA = 1 << 2,
  SM50_SHADER_FLAG_CAUSAL_VERTEX_REG1_MAGENTA = 1 << 3,
  SM50_SHADER_FLAG_CAUSAL_FRAGMENT_SIDECHANNEL = 1 << 4,
  SM50_SHADER_FLAG_CAUSAL_FRAGMENT_FINAL_MAGENTA = 1 << 5,
};

struct SM50_SHADER_COMMON_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flags;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  /** MTLPixelFormat */
  uint32_t pixel_formats[8];
};

struct SM50_IA_INPUT_ELEMENT {
  uint32_t reg;
  uint32_t slot;
  uint32_t aligned_byte_offset;
  /** MTLAttributeFormat */
  uint32_t format;
  uint32_t step_function: 1;
  uint32_t step_rate: 31;
};

enum SM50_INDEX_BUFFER_FORMAT {
  SM50_INDEX_BUFFER_FORMAT_NONE = 0,
  SM50_INDEX_BUFFER_FORMAT_UINT16 = 1,
  SM50_INDEX_BUFFER_FORMAT_UINT32 = 2,
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  struct SM50_IA_INPUT_ELEMENT *elements;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

struct SM50_SHADER_ROOT_SIGNATURE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  const void* bytecode;
  size_t bytecode_length;
};

/* MacRunner POST-CS 25.10.2026: the pixel shader of a one-target post-process pass (DXIL) as
   - SM50_POST_CS_KERNEL: a compute kernel with the same body. Thread (x, y) shades pixel (origin + (x, y)):
     SV_Position.xy = pixel + 0.5; interpolated inputs are read from the uv-map; the SV_Target0 value is written to
     texture(SM50_POST_CS_TARGET_SLOT) (write access, texel type of target_format); an uncovered pixel (coverage
     component of map 0 != 1.0) is not written. Root arguments and static samplers bind as for the fragment
     function (buffer 0, 1); buffer(SM50_POST_CS_PARAMS_SLOT) = SM50_POST_CS_PARAMS words (constant);
     buffer(SM50_POST_CS_WITNESS_SLOT) = uint[2] (device): thread (0, 0) compares the words the vertex stage of the
     pass reads (buffers SM50_POST_CS_SOURCE_SLOT + r, words 0..n-1) with the expected ones and on any difference
     adds 1 to word 0 and stores witness_id to word 1.
   - SM50_POST_CS_UVMAP: the fragment function of the uv-map pass: the same input declarations as the pixel
     shader (so the rasterizer interpolates exactly what the pixel shader would get), colour output i = input
     register map_reg[i] as float4 (RGBA32Float target), component cov_comp of map 0 = 1.0 (cleared to 0).
   The map layout (map_count, map_reg, map_mask, cov_comp) is a function of the input signature alone and is
   reported by both modes. The compiler sets applied = SM50_POST_CS_APPLIED; a compile that does not acknowledge
   the node must not be used. Unsupported shaders fail with a reason (SM50GetErrorMessage). */
#define SM50_POST_CS_APPLIED 0x50435331u /* 'PCS1' */
#define SM50_POST_CS_KERNEL 1u
#define SM50_POST_CS_UVMAP 2u
#define SM50_POST_CS_MAX_MAPS 4u
#define SM50_POST_CS_TARGET_SLOT 0u
#define SM50_POST_CS_MAP_SLOT 1u
#define SM50_POST_CS_PARAMS_SLOT 3u
#define SM50_POST_CS_WITNESS_SLOT 4u
#define SM50_POST_CS_SOURCE_SLOT 5u
#define SM50_POST_CS_MAX_SOURCES 4u
#define SM50_POST_CS_MAX_WITNESS_WORDS 64u
/* uint32 words of buffer(SM50_POST_CS_PARAMS_SLOT) */
struct SM50_POST_CS_PARAMS {
  uint32_t origin_x, origin_y;
  uint32_t source_count;                          /* witness sources bound at SM50_POST_CS_SOURCE_SLOT + r */
  uint32_t witness_id;
  uint32_t source_words[SM50_POST_CS_MAX_SOURCES]; /* words compared in source r */
  uint32_t expect[SM50_POST_CS_MAX_WITNESS_WORDS]; /* sources in order, words concatenated */
};
struct SM50_SHADER_PS_AS_COMPUTE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t mode;          /* SM50_POST_CS_KERNEL / SM50_POST_CS_UVMAP */
  uint32_t target_format; /* MTLPixelFormat of render target 0 (KERNEL) */
  uint32_t applied;       /* out */
  uint32_t map_count;     /* out */
  uint32_t map_reg[SM50_POST_CS_MAX_MAPS];  /* out */
  uint32_t map_mask[SM50_POST_CS_MAX_MAPS]; /* out: declared components of the register */
  uint32_t cov_comp;      /* out: component of map 0 holding coverage; 4 = none (coverage == 0) */
  uint32_t position_mask; /* out: SV_Position components the shader reads (0 = none) */
  uint32_t coverage;      /* in: 1 = coverage channel in map 0 (kernel skips uncovered pixels); 0 = none - the caller
                             proves per draw that every pixel of the dispatched rectangle is covered */
  uint32_t map_format;    /* out: channels of every map: 2 = RG32Float (coverage 0 and every map register within xy),
                             4 = RGBA32Float */
};

/* MacRunner POST-CS: what the outputs of a vertex shader (DXIL) depend on, for keying the uv-map of a pass by
   content. A read-only query through SM50Compile (like SM50_SHADER_REUSE_AUDIT_QUERY): no library is emitted,
   *ppBitcode = NULL, the node is filled and applied = SM50_POST_CS_APPLIED. ok = 1: the shader reads only its
   input attributes, constant buffers at constant offsets (cbufferLoadLegacy) and literals - no textures, buffers,
   UAVs or system values. rows[i] = a 16-byte constant buffer row it reads (register space, register, row). */
#define SM50_POST_CS_VS_MAX_ROWS 16u
struct SM50_POST_CS_VS_ROW {
  uint32_t space, reg, row;
};
/* Where SV_Position component c comes from: pos_kind[c] = SM50_POST_CS_POS_INPUT (input element pos_semantic /
   pos_semantic_index, component pos_b[c]), _LITERAL (pos_value[c]), _CB (rows[pos_a[c]], component pos_b[c]), _OTHER. */
#define SM50_POST_CS_POS_OTHER 0u
#define SM50_POST_CS_POS_INPUT 1u
#define SM50_POST_CS_POS_LITERAL 2u
#define SM50_POST_CS_POS_CB 3u
struct SM50_SHADER_POST_CS_VS_KEY_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t applied;
  uint32_t ok;
  uint32_t row_count;
  struct SM50_POST_CS_VS_ROW rows[SM50_POST_CS_VS_MAX_ROWS];
  char reason[96];
  uint32_t pos_kind[4], pos_a[4], pos_b[4];
  float pos_value[4];
  char pos_semantic[32];   /* the one input element the INPUT components read */
  uint32_t pos_semantic_index;
};

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
);
// Declared DXIL resources from the already parsed entry point. Type uses the D3D descriptor-range values
// (SRV=0, UAV=1, CBV=2, sampler=3). ConstantBytes is the actual DXIL cbuffer size, never a coverage placeholder.
struct SM50_DXIL_RESOURCE_BINDING {
  uint32_t Type, First, Last, Space, ConstantBytes;
};
// A null output queries Count. Failure clears Count and never publishes partial bindings. Does not change reflection ABI.
AIRCONV_API int SM50GetDxilResourceBindings(
  sm50_shader_t pShader, struct SM50_DXIL_RESOURCE_BINDING *pBindings, uint32_t Capacity, uint32_t *pCount
);
AIRCONV_API void SM50Destroy(sm50_shader_t pShader);
AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData
);
AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode);
AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize);
AIRCONV_API void SM50FreeError(sm50_error_t pError);

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
);

#ifdef __cplusplus
};

inline std::string SM50GetErrorMessageString(sm50_error_t pError) {
  std::string str;
  str.resize(256);
  auto size = SM50GetErrorMessage(pError, str.data(), str.size());
  str.resize(size);
  return str;
};

#endif

#endif
