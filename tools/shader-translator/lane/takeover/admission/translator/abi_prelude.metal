// MacRunner root-signature ABI helpers (resources/ABI.md). Emitted into every translated shader
// that reads resources; the vertex part only into vertex functions with inputs.
using namespace metal;
template<typename T> struct spvDesc { T h; ulong meta; ulong aux; ulong pad; };
struct spvSamplerDesc { sampler s; sampler cube; ulong meta; ulong pad; };
template<typename T> struct spvDescArray { constant spvDesc<T>* p; T operator[](uint i) const { return p[i].h; } };
struct spvSamplerArray { constant spvSamplerDesc* p; sampler operator[](uint i) const { return p[i].s; } };
// The same 32 bytes as plain words: +8 meta (texture: min LOD, array length; typed buffer: first
// element, element count; buffer: byte length), +16 aux (UAV counter address; sampler: LOD bias).
struct spvRawDesc { ulong h; ulong meta; ulong aux; ulong pad; };
struct spvMetaArray { constant spvRawDesc* p; ulong operator[](uint i) const { return p[i].meta; } };
struct spvAuxArray { constant spvRawDesc* p; ulong operator[](uint i) const { return p[i].aux; } };
struct spvSamplerCubeArray { constant spvSamplerDesc* p; sampler operator[](uint i) const { return p[i].cube; } };
struct spvCounterArray { constant spvRawDesc* p; device uint& operator[](uint i) const { return *reinterpret_cast<device uint*>(p[i].aux); } };
// P is the pointer to the block, address space included.
template<typename P> struct spvBufferArray { constant spvDesc<P>* p; P operator[](uint i) const { return p[i].h; } };
static inline __attribute__((always_inline)) float spvSamplerBias(ulong aux) { return as_type<float>(uint(aux)); }
static inline __attribute__((always_inline)) float spvMinLod(ulong meta) { return as_type<float>(uint(meta)); }
// Layer of a texture array: nearest, clamped to the array length of the view.
static inline __attribute__((always_inline)) uint spvArrayIndex(float layer, ulong meta) { return uint(min(max(int(rint(layer)), 0), int(uint(meta >> 32)) - 1)); }
// Element of a typed buffer view inside the buffer's texture; outside the view it is an
// out-of-range coordinate (reads give zero, writes and atomics do nothing).
static inline __attribute__((always_inline)) uint spvBufferAt(uint element, ulong meta) { return element < uint(meta >> 32) ? element + uint(meta) : 0xffffffffu; }
