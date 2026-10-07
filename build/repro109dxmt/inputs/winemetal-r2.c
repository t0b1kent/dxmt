#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#import <Cocoa/Cocoa.h>
#import <ColorSync/ColorSync.h>
#import <CoreFoundation/CFRunLoop.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include "objc/objc-runtime.h"
#include <bootstrap.h>
#include <mach/mach_port.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "../airconv_thunks.h"

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL 0xC0000001

/*
 * Metal render state persists for the lifetime of an MTLRenderCommandEncoder,
 * not for the lifetime of one Wine unix-call command list. DXMT may submit a
 * SetPSO command and a later Draw through separate encodeCommands calls, so the
 * software guard must have the same lifetime as the native encoder state.
 */
static char winemetal_render_encoder_has_pso_key;
static char macrunner_render_probe_state_key;
static char macrunner_pipeline_probe_info_key;
static char macrunner_magenta_fragment_function_key;
static char macrunner_causal_present_surface_phase_key;
static char macrunner_present_attachment_trace_key;

#define MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX 28u
#define MACRUNNER_CAUSAL_SIDECHANNEL_WIDTH 1024u
#define MACRUNNER_CAUSAL_SIDECHANNEL_HEIGHT 768u
#define MACRUNNER_CAUSAL_SIDECHANNEL_SENTINEL UINT32_C(0x7fc0c0de)

enum MacRunnerGPUReadbackPhase {
  MacRunnerGPUReadbackAfterClear = 1,
  MacRunnerGPUReadbackAfterRender = 2,
  MacRunnerGPUReadbackPrePresent = 3,
  MacRunnerGPUReadbackShaderTexture = 4,
  MacRunnerGPUReadbackCausalC0 = 5,
  MacRunnerGPUReadbackCausalC1 = 6,
  MacRunnerGPUReadbackCausalC2 = 7,
  MacRunnerGPUReadbackCausalC3 = 8,
  MacRunnerGPUReadbackCausalPresentC0 = 9,
  MacRunnerGPUReadbackCausalPresentC1 = 10,
  MacRunnerGPUReadbackCausalPresentC2 = 11,
  MacRunnerGPUReadbackCausalPresentC3 = 12,
  MacRunnerGPUReadbackPresentedSurface = 13,
};

static atomic_uint_fast64_t macrunner_present_surface_ordinal;

static void macrunner_gpu_readback_schedule(id<MTLCommandBuffer> command_buffer,
                                            id<MTLTexture> texture, uint32_t phase,
                                            uint64_t tag);

static const char *
macrunner_frame_dump_format_name(MTLPixelFormat format) {
  switch (format) {
  case MTLPixelFormatBGRA8Unorm: return "BGRA8Unorm";
  case MTLPixelFormatBGRA8Unorm_sRGB: return "BGRA8Unorm_sRGB";
  case MTLPixelFormatRGBA8Unorm: return "RGBA8Unorm";
  case MTLPixelFormatRGBA8Unorm_sRGB: return "RGBA8Unorm_sRGB";
  default: return "unsupported";
  }
}

static char *
macrunner_frame_dump_directory(void) {
  const char *configured = getenv("MACRUNNER_DXMT_FRAME_DUMP_DIR");
  if (configured && configured[0])
    return strdup(configured);

  const char *run_dir = getenv("MACRUNNER_RUN_DIR");
  if (!run_dir || !run_dir[0])
    run_dir = getenv("DXMT_LOG_PATH");
  if (!run_dir || !run_dir[0])
    return NULL;

  size_t run_len = strlen(run_dir);
  static const char suffix[] = "/dxmt-frame-dumps";
  if (run_len > PATH_MAX - sizeof(suffix))
    return NULL;

  char *directory = malloc(run_len + sizeof(suffix));
  if (!directory)
    return NULL;
  memcpy(directory, run_dir, run_len);
  memcpy(directory + run_len, suffix, sizeof(suffix));
  return directory;
}

static BOOL
macrunner_frame_dump_mkdirs(char *directory) {
  if (!directory || !directory[0])
    return NO;

  for (char *cursor = directory + 1; *cursor; cursor++) {
    if (*cursor != '/')
      continue;
    *cursor = '\0';
    if (mkdir(directory, 0700) && errno != EEXIST) {
      *cursor = '/';
      return NO;
    }
    *cursor = '/';
  }
  return !mkdir(directory, 0700) || errno == EEXIST;
}

static BOOL
macrunner_frame_dump_write_all(int fd, const uint8_t *bytes, size_t length) {
  while (length) {
    ssize_t written = write(fd, bytes, length);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return NO;
    bytes += written;
    length -= (size_t)written;
  }
  return YES;
}

static void
macrunner_frame_dump_schedule(id<MTLCommandBuffer> command_buffer,
                              id<MTLTexture> texture, uint64_t frame) {
  if (!command_buffer || !texture) {
    fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=skip reason=null-object\n",
            (unsigned long long)frame);
    return;
  }

  const MTLPixelFormat format = texture.pixelFormat;
  const BOOL bgra = format == MTLPixelFormatBGRA8Unorm ||
                    format == MTLPixelFormatBGRA8Unorm_sRGB;
  const BOOL rgba = format == MTLPixelFormatRGBA8Unorm ||
                    format == MTLPixelFormatRGBA8Unorm_sRGB;
  const NSUInteger width = texture.width;
  const NSUInteger height = texture.height;
  if ((!bgra && !rgba) || texture.textureType != MTLTextureType2D ||
      texture.sampleCount != 1 || !width || !height || width > 16384 ||
      height > 16384 || width > SIZE_MAX / 4) {
    fprintf(stderr,
            "dxmt-frame-dump: frame=%llu stage=skip reason=unsupported "
            "format=%lu type=%lu samples=%lu size=%lux%lu\n",
            (unsigned long long)frame, (unsigned long)format,
            (unsigned long)texture.textureType, (unsigned long)texture.sampleCount,
            (unsigned long)width, (unsigned long)height);
    return;
  }

  NSUInteger alignment = [texture.device minimumLinearTextureAlignmentForPixelFormat:format];
  if (!alignment)
    alignment = 256;
  const NSUInteger packed_row = width * 4;
  const NSUInteger row_bytes = (packed_row + alignment - 1) & ~(alignment - 1);
  if (height > SIZE_MAX / row_bytes) {
    fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=skip reason=size-overflow\n",
            (unsigned long long)frame);
    return;
  }
  const NSUInteger byte_count = row_bytes * height;
  char *directory = macrunner_frame_dump_directory();
  if (!directory) {
    fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=skip reason=no-output-dir\n",
            (unsigned long long)frame);
    return;
  }

  id<MTLBuffer> staging = [texture.device newBufferWithLength:byte_count
                                                        options:MTLResourceStorageModeShared];
  if (!staging) {
    fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=skip reason=buffer-allocation\n",
            (unsigned long long)frame);
    free(directory);
    return;
  }

  id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
  [blit copyFromTexture:texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(width, height, 1)
               toBuffer:staging
      destinationOffset:0
 destinationBytesPerRow:row_bytes
destinationBytesPerImage:byte_count];
  [blit endEncoding];
  fprintf(stderr,
          "dxmt-frame-dump: frame=%llu stage=scheduled size=%lux%lu format=%s row=%lu\n",
          (unsigned long long)frame, (unsigned long)width, (unsigned long)height,
          macrunner_frame_dump_format_name(format), (unsigned long)row_bytes);

  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    if (completed.status != MTLCommandBufferStatusCompleted) {
      fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=failed status=%lu\n",
              (unsigned long long)frame, (unsigned long)completed.status);
      [staging release];
      free(directory);
      return;
    }

    if (!macrunner_frame_dump_mkdirs(directory)) {
      fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=failed reason=mkdir errno=%d\n",
              (unsigned long long)frame, errno);
      [staging release];
      free(directory);
      return;
    }

    char path[PATH_MAX];
    int path_len = snprintf(path, sizeof(path),
                            "%s/frame-%06llu-%lux%lu.rgba", directory,
                            (unsigned long long)frame, (unsigned long)width,
                            (unsigned long)height);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
      fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=failed reason=path-too-long\n",
              (unsigned long long)frame);
      [staging release];
      free(directory);
      return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=failed reason=open errno=%d\n",
              (unsigned long long)frame, errno);
      [staging release];
      free(directory);
      return;
    }

    const uint8_t *source = (const uint8_t *)staging.contents;
    uint8_t *rgba_row = malloc(packed_row);
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0, nonzero = 0;
    uint8_t min_r = 255, min_g = 255, min_b = 255;
    uint8_t max_r = 0, max_g = 0, max_b = 0;
    BOOL write_ok = source && rgba_row;
    for (NSUInteger y = 0; write_ok && y < height; y++) {
      const uint8_t *src_row = source + y * row_bytes;
      for (NSUInteger x = 0; x < width; x++) {
        const uint8_t *pixel = src_row + x * 4;
        const uint8_t r = bgra ? pixel[2] : pixel[0];
        const uint8_t g = pixel[1];
        const uint8_t b = bgra ? pixel[0] : pixel[2];
        const uint8_t a = pixel[3];
        uint8_t *dst = rgba_row + x * 4;
        dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
        sum_r += r; sum_g += g; sum_b += b;
        nonzero += (r | g | b) != 0;
        if (r < min_r) min_r = r; if (r > max_r) max_r = r;
        if (g < min_g) min_g = g; if (g > max_g) max_g = g;
        if (b < min_b) min_b = b; if (b > max_b) max_b = b;
      }
      write_ok = macrunner_frame_dump_write_all(fd, rgba_row, packed_row);
    }
    const int close_rc = close(fd);
    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (!write_ok || close_rc) {
      fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=failed reason=write errno=%d\n",
              (unsigned long long)frame, errno);
      unlink(path);
    } else {
      fprintf(stderr,
              "dxmt-frame-dump: frame=%llu stage=complete path=%s size=%lux%lu "
              "format=%s min_rgb=%u,%u,%u max_rgb=%u,%u,%u "
              "mean_rgb=%.3f,%.3f,%.3f nonzero_fraction=%.6f\n",
              (unsigned long long)frame, path, (unsigned long)width,
              (unsigned long)height, macrunner_frame_dump_format_name(format),
              min_r, min_g, min_b, max_r, max_g, max_b,
              pixels ? (double)sum_r / pixels : 0.0,
              pixels ? (double)sum_g / pixels : 0.0,
              pixels ? (double)sum_b / pixels : 0.0,
              pixels ? (double)nonzero / pixels : 0.0);
    }
    free(rgba_row);
    [staging release];
    free(directory);
  }];
}

@interface MacRunnerRenderProbeState : NSObject {
@public
  obj_handle_t command_buffer;
  obj_handle_t render_target;
  obj_handle_t pso;
  obj_handle_t depth_stencil_state;
  uint32_t stencil_ref;
  float blend_color[4];
  obj_handle_t vertex_buffers[31];
  uint64_t vertex_buffer_offsets[31];
  obj_handle_t fragment_buffers[31];
  uint64_t fragment_buffer_offsets[31];
  obj_handle_t fragment_textures[32];
  obj_handle_t shader_textures[8];
  uint64_t shader_texture_ids[8];
  uint32_t shader_texture_count;
  struct WMTViewport viewport;
  struct WMTScissorRect scissor;
  enum WMTTriangleFillMode fill_mode;
  enum WMTCullMode cull_mode;
  enum WMTDepthClipMode depth_clip_mode;
  uint32_t render_target_width;
  uint32_t render_target_height;
  uint32_t draw_count;
  uint32_t causal_phase;
  uint32_t causal_target_draw_count;
  enum WMTLoadAction color_load_action;
  BOOL viewport_valid;
  BOOL scissor_valid;
  BOOL causal_target_seen;
  BOOL causal_signature_match;
}
@end

@implementation MacRunnerRenderProbeState
@end

/*
 * Per-command-buffer attachment inventory.  This deliberately retains every
 * non-null attachment pointer for the lifetime of the Metal command buffer:
 * an absence result is valid only when no attachment has been silently
 * dropped by a fixed-size sample.
 */
@interface MacRunnerPresentAttachmentTrace : NSObject {
 @public
  uint64_t render_encoder_count;
  uint64_t color_slot_count;
  uint64_t color_nonnull_count;
  uint64_t resolve_slot_count;
  uint64_t resolve_nonnull_count;
  uint64_t color_clear_count;
  uint64_t draw_count;
  uint64_t direct_control_writes;
  NSMutableSet *color_textures;
  NSMutableSet *resolve_textures;
}
@end

@implementation MacRunnerPresentAttachmentTrace
- (id)init {
  self = [super init];
  if (self) {
    color_textures = [[NSMutableSet alloc] init];
    resolve_textures = [[NSMutableSet alloc] init];
  }
  return self;
}
- (void)dealloc {
  [color_textures release];
  [resolve_textures release];
  [super dealloc];
}
@end

@interface MacRunnerPipelineProbeInfo : NSObject {
@public
  obj_handle_t vertex_function;
  obj_handle_t fragment_function;
  uint64_t pso_id;
  uint64_t fragment_hash;
  char vertex_name[160];
  char fragment_name[160];
  NSUInteger metal_rt0_format;
  NSUInteger metal_write_mask;
  NSUInteger metal_rgb_op;
  NSUInteger metal_alpha_op;
  NSUInteger metal_src_rgb;
  NSUInteger metal_dst_rgb;
  NSUInteger metal_src_alpha;
  NSUInteger metal_dst_alpha;
  NSUInteger metal_depth_format;
  NSUInteger metal_stencil_format;
  NSUInteger metal_sample_count;
  NSUInteger metal_input_topology;
  BOOL metal_blend_enabled;
  BOOL metal_alpha_to_coverage;
  BOOL magenta_override;
  obj_handle_t causal_pso[4];
  BOOL causal_ready;
}
@end

@implementation MacRunnerPipelineProbeInfo
- (void)dealloc {
  for (unsigned i = 0; i < 4; i++)
    [(id)causal_pso[i] release];
  [super dealloc];
}
@end

struct MacRunnerCausalDrawSignature {
  uint64_t schema_version;
  uint64_t logical_pso_id;
  uint64_t vertex_name_hash;
  uint64_t fragment_name_hash;
  uint64_t fragment_hash;
  uint64_t index_buffer_offset;
  uint64_t index_count;
  uint64_t primitive_type;
  uint64_t index_type;
  uint64_t instance_count;
  uint64_t base_vertex;
  uint64_t base_instance;
  uint64_t viewport_valid;
  uint64_t viewport_x_bits;
  uint64_t viewport_y_bits;
  uint64_t viewport_width_bits;
  uint64_t viewport_height_bits;
  uint64_t viewport_znear_bits;
  uint64_t viewport_zfar_bits;
  uint64_t scissor_valid;
  uint64_t scissor_x;
  uint64_t scissor_y;
  uint64_t scissor_width;
  uint64_t scissor_height;
  uint64_t index_buffer_length;
  uint64_t index_storage_mode;
  uint64_t index_slice_valid;
  uint64_t index_slice_bytes;
  uint64_t index_slice_hash;
  uint64_t metal_rt0_format;
  uint64_t metal_write_mask;
  uint64_t metal_blend_enabled;
  uint64_t metal_rgb_op;
  uint64_t metal_alpha_op;
  uint64_t metal_src_rgb;
  uint64_t metal_dst_rgb;
  uint64_t metal_src_alpha;
  uint64_t metal_dst_alpha;
  uint64_t metal_depth_format;
  uint64_t metal_stencil_format;
  uint64_t metal_sample_count;
  uint64_t metal_input_topology;
  uint64_t metal_alpha_to_coverage;
  uint64_t render_target_width;
  uint64_t render_target_height;
  uint64_t fill_mode;
  uint64_t cull_mode;
  uint64_t depth_clip_mode;
  uint64_t stencil_ref;
  uint64_t color_load_action;
  uint64_t blend_color_bits[4];
};

#define MACRUNNER_CAUSAL_SIGNATURE_SCHEMA UINT64_C(1)
#define MACRUNNER_CAUSAL_SIGNATURE_FIELD_COUNT 54u
#define MACRUNNER_CAUSAL_SIGNATURE_CHECKSUM UINT64_C(0x07194c46f282d32d)

static const struct MacRunnerCausalDrawSignature
    macrunner_causal_c0_signature = {
        .schema_version = UINT64_C(1),
        .logical_pso_id = UINT64_C(0x9d2b46c27af732f4),
        .vertex_name_hash = UINT64_C(0x9159b629ce5383e1),
        .fragment_name_hash = UINT64_C(0x937eaceb50c989a0),
        .fragment_hash = UINT64_C(0x0bcd488ba1612b3f),
        .index_buffer_offset = UINT64_C(0x000000000000000c),
        .index_count = 6,
        .primitive_type = 3,
        .index_type = 0,
        .instance_count = 1,
        .base_vertex = UINT64_C(0x0000000000000300),
        .base_instance = 0,
        .viewport_valid = 1,
        .viewport_x_bits = UINT64_C(0x0000000000000000),
        .viewport_y_bits = UINT64_C(0x4050000000000000),
        .viewport_width_bits = UINT64_C(0x4090000000000000),
        .viewport_height_bits = UINT64_C(0x4084000000000000),
        .viewport_znear_bits = UINT64_C(0x0000000000000000),
        .viewport_zfar_bits = UINT64_C(0x3ff0000000000000),
        .scissor_valid = 1,
        .scissor_x = 0,
        .scissor_y = 64,
        .scissor_width = 1024,
        .scissor_height = 640,
        .index_buffer_length = UINT64_C(0x0000000000020000),
        .index_storage_mode = 0,
        .index_slice_valid = 1,
        .index_slice_bytes = 12,
        .index_slice_hash = UINT64_C(0x39ccf7d8033c06e7),
        .metal_rt0_format = UINT64_C(0x46),
        .metal_write_mask = UINT64_C(0x0f),
        .metal_blend_enabled = 0,
        .metal_rgb_op = 0,
        .metal_alpha_op = 0,
        .metal_src_rgb = 1,
        .metal_dst_rgb = 0,
        .metal_src_alpha = 1,
        .metal_dst_alpha = 0,
        .metal_depth_format = UINT64_C(0x104),
        .metal_stencil_format = UINT64_C(0x104),
        .metal_sample_count = 1,
        .metal_input_topology = 3,
        .metal_alpha_to_coverage = 0,
        .render_target_width = 1024,
        .render_target_height = 768,
        .fill_mode = 0,
        .cull_mode = 0,
        .depth_clip_mode = 0,
        .stencil_ref = 0,
        .color_load_action = 2,
        .blend_color_bits = {UINT64_C(0x3f800000), UINT64_C(0x3f800000),
                             UINT64_C(0x3f800000), UINT64_C(0x3f800000)},
};

_Static_assert(sizeof(struct MacRunnerCausalDrawSignature) ==
                   MACRUNNER_CAUSAL_SIGNATURE_FIELD_COUNT * sizeof(uint64_t),
               "causal signature must be a packed sequence of semantic u64 fields");

static atomic_bool macrunner_causal_stage_busy;
static atomic_uint macrunner_causal_control_claimed_mask;
static atomic_bool macrunner_causal_invalid;
static atomic_uint macrunner_causal_signature_skip_logs;

static BOOL
winemetal_render_encoder_has_pso(id<MTLRenderCommandEncoder> encoder) {
  return encoder &&
         objc_getAssociatedObject(encoder, &winemetal_render_encoder_has_pso_key) != nil;
}

static void
winemetal_render_encoder_set_has_pso(id<MTLRenderCommandEncoder> encoder, BOOL has_pso) {
  if (!encoder)
    return;
  objc_setAssociatedObject(encoder, &winemetal_render_encoder_has_pso_key,
                           has_pso ? @YES : nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static int
macrunner_gpu_readback_probe_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("MACRUNNER_HB_GPU_READBACK_PROBE");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  }
  return enabled;
}

static int
macrunner_shader_inputs_probe_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("MACRUNNER_HB_SHADER_INPUTS_PROBE");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  }
  return enabled;
}

static int
macrunner_fragment_output_probe_enabled(void) {
  static int enabled;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_FRAGMENT_OUTPUT_PROBE");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  });
  return enabled;
}

static int
macrunner_force_magenta_fragment_enabled(void) {
  static int enabled;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_FORCE_MAGENTA_FRAGMENT");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  });
  return enabled;
}

/*
 * Runtime A/B control requested by the lane contract.  Unlike the exact-ID
 * developer control above, this switches every color-producing PSO so A and B
 * differ by one child-environment variable only.  The probe still hashes the
 * original fragment function and state, allowing comparison to fail closed if
 * the same PSO/Draw is not observed.
 */
static int
macrunner_force_fragment_magenta_global_enabled(void) {
  static int enabled;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_FORCE_FRAGMENT_MAGENTA");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  });
  return enabled;
}

struct MacRunnerCausalControlConfig {
  unsigned count;
  unsigned phases[4];
};

static BOOL
macrunner_causal_parse_phase_token(const char *value, size_t length,
                                   unsigned *phase_out) {
  if (!value || length != 2 || value[0] != 'C' || value[1] < '0' ||
      value[1] > '3')
    return NO;
  if (phase_out)
    *phase_out = (unsigned)(value[1] - '0');
  return YES;
}

static const struct MacRunnerCausalControlConfig *
macrunner_causal_control_config(void) {
  static struct MacRunnerCausalControlConfig config;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_CAUSAL_CONTROL");
    unsigned phase = UINT32_MAX;
    if (value && macrunner_causal_parse_phase_token(value, strlen(value),
                                                    &phase)) {
      config.count = 1;
      config.phases[0] = phase;
    } else if (value && strcmp(value, "C0,C2,C3") == 0) {
      config.count = 3;
      config.phases[0] = 0;
      config.phases[1] = 2;
      config.phases[2] = 3;
    }
  });
  return &config;
}

static unsigned
macrunner_causal_control(void) {
  const struct MacRunnerCausalControlConfig *config =
      macrunner_causal_control_config();
  return config->count == 1 ? config->phases[0] : UINT32_MAX;
}

static unsigned
macrunner_causal_control_count(void) {
  return macrunner_causal_control_config()->count;
}

static unsigned
macrunner_causal_control_phase_at(unsigned index) {
  const struct MacRunnerCausalControlConfig *config =
      macrunner_causal_control_config();
  return index < config->count ? config->phases[index] : UINT32_MAX;
}

static uint32_t
macrunner_causal_control_mask(void) {
  const struct MacRunnerCausalControlConfig *config =
      macrunner_causal_control_config();
  uint32_t mask = 0;
  for (unsigned i = 0; i < config->count; i++)
    if (config->phases[i] < 4)
      mask |= UINT32_C(1) << config->phases[i];
  return mask;
}

static BOOL
macrunner_causal_phase_requested(unsigned phase) {
  return phase < 4 && (macrunner_causal_control_mask() & (UINT32_C(1) << phase));
}

static int
macrunner_causal_ladder_enabled(void) {
  return macrunner_causal_control_count() > 0;
}

static int
macrunner_causal_sidechannel_grid_enabled(void) {
  static int enabled;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_CAUSAL_SIDECHANNEL_GRID");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  });
  return enabled;
}

static int
macrunner_causal_sidechannel_grid_control_phase(void) {
  static int phase = -1;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_CAUSAL_SIDECHANNEL_GRID_CONTROL");
    if (!value || !value[0] || !strcmp(value, "0"))
      return;
    if (!strcmp(value, "C1"))
      phase = 1;
    else if (!strcmp(value, "C2"))
      phase = 2;
    else if (!strcmp(value, "C3"))
      phase = 3;
    else
      fprintf(stderr,
              "macrunner-hb-causal-sidechannel-grid: control=INVALID result=skip "
              "reason=bad-grid-control value=%s\n", value);
  });
  return phase;
}

static const char *
macrunner_causal_phase_name(unsigned phase) {
  static const char *names[] = {"C0", "C1", "C2", "C3"};
  return phase < 4 ? names[phase] : "INVALID";
}

static void
macrunner_causal_sidechannel_analyze(id<MTLCommandBuffer> command_buffer,
                                     id<MTLBuffer> buffer, unsigned phase) {
  const uint32_t *words = (const uint32_t *)[buffer contents];
  const uint64_t pixel_count =
      (uint64_t)MACRUNNER_CAUSAL_SIDECHANNEL_WIDTH *
      MACRUNNER_CAUSAL_SIDECHANNEL_HEIGHT;
  uint64_t written = 0, black = 0, magenta = 0, other = 0, nonfinite = 0;
  const int grid_control_phase = macrunner_causal_sidechannel_grid_control_phase();
  const BOOL grid_enabled = (phase == 0 && macrunner_causal_sidechannel_grid_enabled()) ||
                            phase == (unsigned)grid_control_phase;
  uint64_t grid_finite = 0, grid_nonzero = 0;
  float grid_min[3] = {INFINITY, INFINITY, INFINITY};
  float grid_max[3] = {-INFINITY, -INFINITY, -INFINITY};
  double grid_sum[3] = {0.0, 0.0, 0.0};
  uint64_t hash = UINT64_C(1469598103934665603);
  if (command_buffer.status == MTLCommandBufferStatusCompleted && words) {
    for (uint64_t pixel = 0; pixel < pixel_count; pixel++) {
      const uint32_t *rgba_words = words + pixel * 4;
      if (rgba_words[0] == MACRUNNER_CAUSAL_SIDECHANNEL_SENTINEL &&
          rgba_words[1] == MACRUNNER_CAUSAL_SIDECHANNEL_SENTINEL &&
          rgba_words[2] == MACRUNNER_CAUSAL_SIDECHANNEL_SENTINEL &&
          rgba_words[3] == MACRUNNER_CAUSAL_SIDECHANNEL_SENTINEL)
        continue;
      float rgba[4];
      memcpy(rgba, rgba_words, sizeof(rgba));
      written++;
      for (unsigned component = 0; component < 4; component++) {
        uint32_t word = rgba_words[component];
        for (unsigned byte = 0; byte < 4; byte++) {
          hash ^= (uint8_t)(word >> (byte * 8));
          hash *= UINT64_C(1099511628211);
        }
      }
      if (!isfinite(rgba[0]) || !isfinite(rgba[1]) ||
          !isfinite(rgba[2]) || !isfinite(rgba[3])) {
        nonfinite++;
      } else if (grid_enabled) {
        for (unsigned component = 0; component < 3; component++) {
          grid_min[component] = fminf(grid_min[component], rgba[component]);
          grid_max[component] = fmaxf(grid_max[component], rgba[component]);
          grid_sum[component] += rgba[component];
        }
        grid_finite++;
        if (fabsf(rgba[0]) > (1.0f / 255.0f) ||
            fabsf(rgba[1]) > (1.0f / 255.0f) ||
            fabsf(rgba[2]) > (1.0f / 255.0f))
          grid_nonzero++;
        if (fabsf(rgba[0]) <= (1.0f / 255.0f) &&
            fabsf(rgba[1]) <= (1.0f / 255.0f) &&
            fabsf(rgba[2]) <= (1.0f / 255.0f)) {
          black++;
        } else if (rgba[0] >= 0.9f && fabsf(rgba[1]) <= 0.1f &&
                   rgba[2] >= 0.9f && rgba[3] >= 0.9f) {
          magenta++;
        } else {
          other++;
        }
      } else if (fabsf(rgba[0]) <= (1.0f / 255.0f) &&
                 fabsf(rgba[1]) <= (1.0f / 255.0f) &&
                 fabsf(rgba[2]) <= (1.0f / 255.0f)) {
        black++;
      } else if (rgba[0] >= 0.9f && fabsf(rgba[1]) <= 0.1f &&
                 rgba[2] >= 0.9f && rgba[3] >= 0.9f) {
        magenta++;
      } else {
        other++;
      }
    }
  }
  const char *classification =
      !written ? "EMPTY" : nonfinite ? "NONFINITE" :
      black == written ? "BLACK" : magenta == written ? "MAGENTA" : "MIXED";
  BOOL valid = command_buffer.status == MTLCommandBufferStatusCompleted &&
               words && written;
  if (!valid)
    atomic_store_explicit(&macrunner_causal_invalid, true,
                          memory_order_release);
  fprintf(
      stderr,
      "macrunner-hb-causal-ladder: phase=sidechannel-complete control=%s "
      "result=%s class=%s written=%llu black=%llu magenta=%llu other=%llu "
      "nonfinite=%llu total=%llu hash=0x%016llx status=%ld error=%s\n",
      macrunner_causal_phase_name(phase), valid ? "ok" : "invalid",
      classification, (unsigned long long)written, (unsigned long long)black,
      (unsigned long long)magenta, (unsigned long long)other,
      (unsigned long long)nonfinite, (unsigned long long)pixel_count,
      (unsigned long long)hash, (long)command_buffer.status,
      command_buffer.error ? [[command_buffer.error description] UTF8String] : "none");
  if (grid_enabled) {
    const double divisor = grid_finite ? (double)grid_finite : 1.0;
    const uint64_t missing = pixel_count - written;
    const char *coverage =
        !valid ? "invalid" : written == pixel_count ? "full" :
        written ? "partial" : "empty";
    fprintf(
        stderr,
        "macrunner-hb-causal-sidechannel-grid: control=%s result=%s "
        "coverage=%s grid=%ux%u sampled=%llu total=%llu missing=%llu "
        "coverage_fraction=%.9g finite=%llu nonfinite=%llu min_rgb=%.9g,%.9g,%.9g "
        "max_rgb=%.9g,%.9g,%.9g mean_rgb=%.9g,%.9g,%.9g "
        "nonzero=%llu nonzero_fraction=%.9g\n",
        macrunner_causal_phase_name(phase),
        valid && grid_finite ? "ok" : valid ? "empty" : "invalid",
        coverage,
        MACRUNNER_CAUSAL_SIDECHANNEL_WIDTH,
        MACRUNNER_CAUSAL_SIDECHANNEL_HEIGHT,
        (unsigned long long)written, (unsigned long long)pixel_count,
        (unsigned long long)missing, (double)written / (double)pixel_count,
        (unsigned long long)grid_finite,
        (unsigned long long)nonfinite, grid_min[0], grid_min[1], grid_min[2],
        grid_max[0], grid_max[1], grid_max[2], grid_sum[0] / divisor,
        grid_sum[1] / divisor, grid_sum[2] / divisor,
        (unsigned long long)grid_nonzero,
        (double)grid_nonzero / divisor);
  }
  fflush(stderr);
}

static BOOL
macrunner_causal_pipeline_is_target(uint64_t pso_id, const char *vertex_name,
                                    const char *fragment_name) {
  return macrunner_causal_ladder_enabled() &&
         pso_id == UINT64_C(0x9d2b46c27af732f4) &&
         vertex_name && fragment_name &&
         strcmp(vertex_name,
                "vs_59c2a0b5_adcd61f38fa5038495d5d87e7f3a67bb8d794d86") == 0 &&
         strcmp(fragment_name,
                "ps_bef43b20_f8e9cf38dc7294cc7203c7fb6cd68b27ed713b6a") == 0;
}

static int
macrunner_force_magenta_target_id(uint64_t *target_id) {
  static int valid;
  static uint64_t target;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_FORCE_MAGENTA_PSO_ID");
    char *end = NULL;
    unsigned long long parsed = value && value[0] ? strtoull(value, &end, 0) : 0;
    if (value && value[0] && (!end || end == value || *end != '\0')) {
      end = NULL;
      parsed = strtoull(value, &end, 16);
    }
    valid = end && end != value && *end == '\0' && parsed != 0;
    target = valid ? (uint64_t)parsed : 0;
  });
  if (target_id)
    *target_id = target;
  return valid;
}

static int
macrunner_fragment_output_probe_take_slot(unsigned *ordinal) {
  static atomic_uint count;
  static unsigned limit;
  static dispatch_once_t once;
  if (!macrunner_fragment_output_probe_enabled())
    return 0;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_FRAGMENT_OUTPUT_PROBE_MAX");
    char *end = NULL;
    unsigned long parsed = value && value[0] ? strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 4096 ? parsed : 64;
  });
  unsigned current = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
  if (ordinal)
    *ordinal = current + 1;
  return current < limit;
}

static int
macrunner_fragment_output_pso_take_slot(unsigned *ordinal) {
  static atomic_uint count;
  static unsigned limit;
  static dispatch_once_t once;
  if (!macrunner_fragment_output_probe_enabled())
    return 0;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_FRAGMENT_OUTPUT_PROBE_MAX");
    char *end = NULL;
    unsigned long parsed = value && value[0] ? strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 4096 ? parsed : 64;
  });
  unsigned current = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
  if (ordinal)
    *ordinal = current + 1;
  return current < limit;
}

static int
macrunner_vertex_data_probe_enabled(void) {
  const char *value = getenv("MACRUNNER_HB_VERTEX_DATA_PROBE");
  return value && value[0] && strcmp(value, "0") != 0;
}

static int
macrunner_vertex_data_draw_take_slot(unsigned *ordinal) {
  static atomic_uint count;
  static unsigned limit;
  static dispatch_once_t once;
  if (!macrunner_vertex_data_probe_enabled())
    return 0;
  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_VERTEX_DATA_DRAW_MAX");
    char *end = NULL;
    unsigned long parsed = value && value[0] ? strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 65536 ? parsed : 4096;
  });
  unsigned current = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
  if (ordinal)
    *ordinal = current + 1;
  return current < limit;
}

static int
macrunner_render_probe_state_enabled(void) {
  return macrunner_gpu_readback_probe_enabled() || macrunner_shader_inputs_probe_enabled() ||
         macrunner_fragment_output_probe_enabled() || macrunner_force_magenta_fragment_enabled() ||
         macrunner_force_fragment_magenta_global_enabled() ||
         macrunner_causal_ladder_enabled() || macrunner_vertex_data_probe_enabled();
}

/*
 * Maps the native DXMT render-pass attachments on a command buffer to the
 * CAMetalDrawable texture observed at Present.  The trace is off by default
 * and has no fixed sampling limit: each command buffer retains its complete
 * set of non-null color/resolve texture identities until it is released.
 */
static BOOL
macrunner_present_attachment_trace_enabled(void) {
  const char *value = getenv("MACRUNNER_HB_PRESENT_SURFACE_ATTACHMENT_TRACE");
  return value && value[0] && strcmp(value, "0");
}

static MacRunnerPresentAttachmentTrace *
macrunner_present_attachment_trace_for_command_buffer(id<MTLCommandBuffer> command_buffer,
                                                      BOOL create) {
  if (!macrunner_present_attachment_trace_enabled() || !command_buffer)
    return nil;
  MacRunnerPresentAttachmentTrace *trace = objc_getAssociatedObject(
      command_buffer, &macrunner_present_attachment_trace_key);
  if (!trace && create) {
    trace = [[MacRunnerPresentAttachmentTrace alloc] init];
    objc_setAssociatedObject(command_buffer, &macrunner_present_attachment_trace_key, trace,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [trace release];
  }
  return trace;
}

static void
macrunner_present_attachment_trace_record(id<MTLCommandBuffer> command_buffer,
                                          const struct WMTRenderPassInfo *info) {
  MacRunnerPresentAttachmentTrace *trace =
      macrunner_present_attachment_trace_for_command_buffer(command_buffer, YES);
  if (!trace || !info)
    return;
  trace->render_encoder_count++;
  for (unsigned i = 0; i < 8; i++) {
    trace->color_slot_count++;
    if (info->colors[i].texture) {
      trace->color_nonnull_count++;
      if (info->colors[i].load_action == WMTLoadActionClear)
        trace->color_clear_count++;
      [trace->color_textures addObject:
          [NSValue valueWithPointer:(void *)info->colors[i].texture]];
    }
    trace->resolve_slot_count++;
    if (info->colors[i].resolve_texture) {
      trace->resolve_nonnull_count++;
      [trace->resolve_textures addObject:
          [NSValue valueWithPointer:(void *)info->colors[i].resolve_texture]];
    }
  }
}

static void
macrunner_present_attachment_trace_log(id<MTLCommandBuffer> command_buffer,
                                       id<MTLTexture> texture, uint64_t ordinal,
                                       BOOL direct_control_match) {
  if (!macrunner_present_attachment_trace_enabled())
    return;
  MacRunnerPresentAttachmentTrace *trace =
      macrunner_present_attachment_trace_for_command_buffer(command_buffer,
                                                             direct_control_match);
  uint64_t encoders = trace ? trace->render_encoder_count : 0;
  uint64_t color_slots = trace ? trace->color_slot_count : 0;
  uint64_t color_nonnull = trace ? trace->color_nonnull_count : 0;
  uint64_t resolve_slots = trace ? trace->resolve_slot_count : 0;
  uint64_t resolve_nonnull = trace ? trace->resolve_nonnull_count : 0;
  uint64_t color_clears = trace ? trace->color_clear_count : 0;
  uint64_t draws = trace ? trace->draw_count : 0;
  uint64_t control_writes = trace ? trace->direct_control_writes : 0;
  NSUInteger color_unique = trace ? [trace->color_textures count] : 0;
  NSUInteger resolve_unique = trace ? [trace->resolve_textures count] : 0;
  BOOL color_match = trace && texture && [trace->color_textures containsObject:
      [NSValue valueWithPointer:(void *)texture]];
  BOOL resolve_match = trace && texture && [trace->resolve_textures containsObject:
      [NSValue valueWithPointer:(void *)texture]];
  fprintf(stderr,
          "macrunner-hb-present-attachment: phase=map result=ok ordinal=%llu "
          "command_buffer=%p texture=%p encoders=%llu color_slots=%llu "
          "color_nonnull=%llu color_unique=%lu color_match=%u "
          "color_clears=%llu draws=%llu "
          "resolve_slots=%llu resolve_nonnull=%llu resolve_unique=%lu "
          "resolve_match=%u direct_control_writes=%llu direct_control_match=%u\n",
          (unsigned long long)ordinal, command_buffer, texture,
          (unsigned long long)encoders, (unsigned long long)color_slots,
          (unsigned long long)color_nonnull, (unsigned long)color_unique, color_match,
          (unsigned long long)color_clears, (unsigned long long)draws,
          (unsigned long long)resolve_slots, (unsigned long long)resolve_nonnull,
          (unsigned long)resolve_unique, resolve_match,
          (unsigned long long)control_writes, direct_control_match);
  fflush(stderr);
}

static void
macrunner_fragment_output_log_draw(MacRunnerRenderProbeState *state,
                                   const char *kind) {
  unsigned ordinal;
  if (!state || !state->pso ||
      !macrunner_fragment_output_probe_take_slot(&ordinal))
    return;
  MacRunnerPipelineProbeInfo *pipeline =
      objc_getAssociatedObject((id)state->pso, &macrunner_pipeline_probe_info_key);
  if (!pipeline)
    return;
  fprintf(
      stderr,
      "macrunner-hb-fragment-output: side=winemetal phase=draw ordinal=%u "
      "pso_id=0x%016llx draw=%s pso=%p rtv=%p ps=%s ps_hash=%016llx "
      "override_magenta=%u metal_rt0_format=%lu metal_write_mask=0x%lx "
      "metal_blend=%u metal_rgb_op=%lu metal_alpha_op=%lu "
      "metal_src_rgb=%lu metal_dst_rgb=%lu metal_src_alpha=%lu metal_dst_alpha=%lu "
      "viewport_valid=%u viewport=%.3f,%.3f,%.3f,%.3f,%.6f,%.6f\n",
      ordinal, (unsigned long long)pipeline->pso_id, kind,
      (void *)(uintptr_t)state->pso, (void *)(uintptr_t)state->render_target,
      pipeline->fragment_name, (unsigned long long)pipeline->fragment_hash,
      pipeline->magenta_override, (unsigned long)pipeline->metal_rt0_format,
      (unsigned long)pipeline->metal_write_mask, pipeline->metal_blend_enabled,
      (unsigned long)pipeline->metal_rgb_op, (unsigned long)pipeline->metal_alpha_op,
      (unsigned long)pipeline->metal_src_rgb, (unsigned long)pipeline->metal_dst_rgb,
      (unsigned long)pipeline->metal_src_alpha, (unsigned long)pipeline->metal_dst_alpha,
      state->viewport_valid, state->viewport.originX, state->viewport.originY,
      state->viewport.width, state->viewport.height, state->viewport.znear,
      state->viewport.zfar);
  fflush(stderr);
}

static uint64_t
macrunner_causal_hash_bytes(const void *data, size_t size) {
  const uint8_t *bytes = data;
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t
macrunner_causal_hash_string(const char *value) {
  return macrunner_causal_hash_bytes(value, value ? strlen(value) : 0);
}

static uint64_t
macrunner_causal_signature_checksum(
    const struct MacRunnerCausalDrawSignature *signature) {
  uint64_t checksum = UINT64_C(14695981039346656037);
  const uint8_t *bytes = (const uint8_t *)signature;
  for (unsigned field = 0; field < MACRUNNER_CAUSAL_SIGNATURE_FIELD_COUNT;
       field++) {
    uint64_t value = 0;
    memcpy(&value, bytes + field * sizeof(uint64_t), sizeof(value));
    for (unsigned shift = 0; shift < 64; shift += 8) {
      checksum ^= (uint8_t)(value >> shift);
      checksum *= UINT64_C(1099511628211);
    }
  }
  return checksum;
}

static int
macrunner_causal_signature_artifact_valid(void) {
  static int valid;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    uint64_t observed =
        macrunner_causal_signature_checksum(&macrunner_causal_c0_signature);
    valid = observed == MACRUNNER_CAUSAL_SIGNATURE_CHECKSUM;
    if (macrunner_causal_ladder_enabled()) {
      fprintf(stderr,
              "macrunner-hb-causal-ladder: phase=signature-artifact "
              "control=%s result=%s schema=%llu fields=%u "
              "expected_checksum=0x%016llx observed_checksum=0x%016llx\n",
              macrunner_causal_phase_name(macrunner_causal_control()),
              valid ? "ok" : "invalid",
              (unsigned long long)MACRUNNER_CAUSAL_SIGNATURE_SCHEMA,
              MACRUNNER_CAUSAL_SIGNATURE_FIELD_COUNT,
              (unsigned long long)MACRUNNER_CAUSAL_SIGNATURE_CHECKSUM,
              (unsigned long long)observed);
      fflush(stderr);
    }
  });
  return valid;
}

static void
macrunner_causal_log_u64(unsigned phase, const char *scope,
                         const char *field, uint64_t expected,
                         uint64_t observed) {
  fprintf(stderr,
          "macrunner-hb-causal-ladder: phase=signature-field control=%s "
          "scope=%s field=%s expected=0x%016llx observed=0x%016llx "
          "changed=%u\n",
          macrunner_causal_phase_name(phase), scope, field,
          (unsigned long long)expected, (unsigned long long)observed,
          expected != observed);
}

static void
macrunner_causal_log_signature_diff(
    unsigned phase, const struct MacRunnerCausalDrawSignature *expected,
    const struct MacRunnerCausalDrawSignature *observed) {
#define LOG_SIGNATURE_U64(field)                                              \
  do {                                                                        \
    if (expected->field != observed->field)                                   \
      macrunner_causal_log_u64(phase, "semantic", #field, expected->field,   \
                               observed->field);                              \
  } while (0)
  LOG_SIGNATURE_U64(schema_version);
  LOG_SIGNATURE_U64(logical_pso_id);
  LOG_SIGNATURE_U64(vertex_name_hash);
  LOG_SIGNATURE_U64(fragment_name_hash);
  LOG_SIGNATURE_U64(fragment_hash);
  LOG_SIGNATURE_U64(index_buffer_offset);
  LOG_SIGNATURE_U64(index_count);
  LOG_SIGNATURE_U64(primitive_type);
  LOG_SIGNATURE_U64(index_type);
  LOG_SIGNATURE_U64(instance_count);
  LOG_SIGNATURE_U64(base_vertex);
  LOG_SIGNATURE_U64(base_instance);
  LOG_SIGNATURE_U64(viewport_valid);
  LOG_SIGNATURE_U64(viewport_x_bits);
  LOG_SIGNATURE_U64(viewport_y_bits);
  LOG_SIGNATURE_U64(viewport_width_bits);
  LOG_SIGNATURE_U64(viewport_height_bits);
  LOG_SIGNATURE_U64(viewport_znear_bits);
  LOG_SIGNATURE_U64(viewport_zfar_bits);
  LOG_SIGNATURE_U64(scissor_valid);
  LOG_SIGNATURE_U64(scissor_x);
  LOG_SIGNATURE_U64(scissor_y);
  LOG_SIGNATURE_U64(scissor_width);
  LOG_SIGNATURE_U64(scissor_height);
  LOG_SIGNATURE_U64(index_buffer_length);
  LOG_SIGNATURE_U64(index_storage_mode);
  LOG_SIGNATURE_U64(index_slice_valid);
  LOG_SIGNATURE_U64(index_slice_bytes);
  LOG_SIGNATURE_U64(index_slice_hash);
  LOG_SIGNATURE_U64(metal_rt0_format);
  LOG_SIGNATURE_U64(metal_write_mask);
  LOG_SIGNATURE_U64(metal_blend_enabled);
  LOG_SIGNATURE_U64(metal_rgb_op);
  LOG_SIGNATURE_U64(metal_alpha_op);
  LOG_SIGNATURE_U64(metal_src_rgb);
  LOG_SIGNATURE_U64(metal_dst_rgb);
  LOG_SIGNATURE_U64(metal_src_alpha);
  LOG_SIGNATURE_U64(metal_dst_alpha);
  LOG_SIGNATURE_U64(metal_depth_format);
  LOG_SIGNATURE_U64(metal_stencil_format);
  LOG_SIGNATURE_U64(metal_sample_count);
  LOG_SIGNATURE_U64(metal_input_topology);
  LOG_SIGNATURE_U64(metal_alpha_to_coverage);
  LOG_SIGNATURE_U64(render_target_width);
  LOG_SIGNATURE_U64(render_target_height);
  LOG_SIGNATURE_U64(fill_mode);
  LOG_SIGNATURE_U64(cull_mode);
  LOG_SIGNATURE_U64(depth_clip_mode);
  LOG_SIGNATURE_U64(stencil_ref);
  LOG_SIGNATURE_U64(color_load_action);
  for (unsigned i = 0; i < 4; i++) {
    char field[32];
    snprintf(field, sizeof(field), "blend_color_bits[%u]", i);
    if (expected->blend_color_bits[i] != observed->blend_color_bits[i])
      macrunner_causal_log_u64(phase, "semantic", field,
                               expected->blend_color_bits[i],
                               observed->blend_color_bits[i]);
  }
  fflush(stderr);
#undef LOG_SIGNATURE_U64
}

static void
macrunner_causal_signature_fill(
    struct MacRunnerCausalDrawSignature *signature,
    MacRunnerRenderProbeState *state,
    MacRunnerPipelineProbeInfo *pipeline,
    const struct wmtcmd_render_draw_indexed *draw) {
  memset(signature, 0, sizeof(*signature));
  signature->schema_version = MACRUNNER_CAUSAL_SIGNATURE_SCHEMA;
  signature->logical_pso_id = pipeline->pso_id;
  signature->vertex_name_hash = macrunner_causal_hash_string(pipeline->vertex_name);
  signature->fragment_name_hash =
      macrunner_causal_hash_string(pipeline->fragment_name);
  signature->fragment_hash = pipeline->fragment_hash;
  signature->index_buffer_offset = draw->index_buffer_offset;
  signature->index_count = draw->index_count;
  signature->primitive_type = draw->primitive_type;
  signature->index_type = draw->index_type;
  signature->instance_count = draw->instance_count;
  signature->base_vertex = (uint64_t)(int64_t)draw->base_vertex;
  signature->base_instance = draw->base_instance;
  signature->viewport_valid = state->viewport_valid;
  memcpy(&signature->viewport_x_bits, &state->viewport.originX,
         sizeof(signature->viewport_x_bits));
  memcpy(&signature->viewport_y_bits, &state->viewport.originY,
         sizeof(signature->viewport_y_bits));
  memcpy(&signature->viewport_width_bits, &state->viewport.width,
         sizeof(signature->viewport_width_bits));
  memcpy(&signature->viewport_height_bits, &state->viewport.height,
         sizeof(signature->viewport_height_bits));
  memcpy(&signature->viewport_znear_bits, &state->viewport.znear,
         sizeof(signature->viewport_znear_bits));
  memcpy(&signature->viewport_zfar_bits, &state->viewport.zfar,
         sizeof(signature->viewport_zfar_bits));
  signature->scissor_valid = state->scissor_valid;
  signature->scissor_x = state->scissor.x;
  signature->scissor_y = state->scissor.y;
  signature->scissor_width = state->scissor.width;
  signature->scissor_height = state->scissor.height;
  signature->metal_rt0_format = pipeline->metal_rt0_format;
  signature->metal_write_mask = pipeline->metal_write_mask;
  signature->metal_blend_enabled = pipeline->metal_blend_enabled;
  signature->metal_rgb_op = pipeline->metal_rgb_op;
  signature->metal_alpha_op = pipeline->metal_alpha_op;
  signature->metal_src_rgb = pipeline->metal_src_rgb;
  signature->metal_dst_rgb = pipeline->metal_dst_rgb;
  signature->metal_src_alpha = pipeline->metal_src_alpha;
  signature->metal_dst_alpha = pipeline->metal_dst_alpha;
  signature->metal_depth_format = pipeline->metal_depth_format;
  signature->metal_stencil_format = pipeline->metal_stencil_format;
  signature->metal_sample_count = pipeline->metal_sample_count;
  signature->metal_input_topology = pipeline->metal_input_topology;
  signature->metal_alpha_to_coverage = pipeline->metal_alpha_to_coverage;
  signature->render_target_width = state->render_target_width;
  signature->render_target_height = state->render_target_height;
  signature->fill_mode = state->fill_mode;
  signature->cull_mode = state->cull_mode;
  signature->depth_clip_mode = state->depth_clip_mode;
  signature->stencil_ref = state->stencil_ref;
  signature->color_load_action = state->color_load_action;
  for (unsigned i = 0; i < 4; i++) {
    uint32_t bits = 0;
    memcpy(&bits, &state->blend_color[i], sizeof(bits));
    signature->blend_color_bits[i] = bits;
  }

  id<MTLBuffer> index_buffer = (id<MTLBuffer>)draw->index_buffer;
  if (!index_buffer)
    return;
  signature->index_buffer_length = index_buffer.length;
  signature->index_storage_mode = index_buffer.storageMode;
  const uint64_t stride =
      draw->index_type == WMTIndexTypeUInt32 ? 4u : 2u;
  if (draw->index_count > UINT64_MAX / stride)
    return;
  const uint64_t bytes = draw->index_count * stride;
  signature->index_slice_bytes = bytes;
  if ((index_buffer.storageMode != MTLStorageModeShared &&
       index_buffer.storageMode != MTLStorageModeManaged) ||
      draw->index_buffer_offset > index_buffer.length ||
      bytes > index_buffer.length - draw->index_buffer_offset)
    return;
  const uint8_t *contents = (const uint8_t *)index_buffer.contents;
  if (!contents)
    return;
  signature->index_slice_hash = macrunner_causal_hash_bytes(
      contents + draw->index_buffer_offset, bytes);
  signature->index_slice_valid = 1;
}

static id<MTLRenderPipelineState>
macrunner_causal_prepare_indexed_draw(
    MacRunnerRenderProbeState *state,
    id<MTLRenderCommandEncoder> encoder,
    const struct wmtcmd_render_draw_indexed *draw, unsigned requested_phase,
    unsigned *phase_out,
    id<MTLBuffer> *sidechannel_out) {
  if (phase_out)
    *phase_out = UINT32_MAX;
  if (sidechannel_out)
    *sidechannel_out = nil;
  if (!macrunner_causal_phase_requested(requested_phase))
    return nil;
  if (!macrunner_causal_ladder_enabled() || !state || !state->pso ||
      !state->viewport_valid || state->viewport.originX != 0.0 ||
      state->viewport.originY != 64.0 || state->viewport.width != 1024.0 ||
      state->viewport.height != 640.0 ||
      atomic_load_explicit(&macrunner_causal_invalid, memory_order_acquire))
    return nil;

  MacRunnerPipelineProbeInfo *pipeline =
      objc_getAssociatedObject((id)state->pso, &macrunner_pipeline_probe_info_key);
  if (!pipeline || pipeline->pso_id != UINT64_C(0x9d2b46c27af732f4))
    return nil;
  const uint32_t phase_bit = UINT32_C(1) << requested_phase;
  if (atomic_load_explicit(&macrunner_causal_control_claimed_mask,
                           memory_order_acquire) & phase_bit)
    return nil;
  unsigned phase = requested_phase;
  if (phase >= 4)
    return nil;
  if (!macrunner_causal_signature_artifact_valid()) {
    atomic_store_explicit(&macrunner_causal_invalid, true, memory_order_release);
    return nil;
  }

  struct MacRunnerCausalDrawSignature signature;
  macrunner_causal_signature_fill(&signature, state, pipeline, draw);
  const uint64_t observed_checksum =
      macrunner_causal_signature_checksum(&signature);
  const BOOL signature_match =
      observed_checksum == MACRUNNER_CAUSAL_SIGNATURE_CHECKSUM &&
      memcmp(&signature, &macrunner_causal_c0_signature,
             sizeof(signature)) == 0;

  if (!signature_match) {
    unsigned skip_log = atomic_fetch_add_explicit(
        &macrunner_causal_signature_skip_logs, 1, memory_order_acq_rel);
    if (skip_log < 4) {
      macrunner_causal_log_signature_diff(
          phase, &macrunner_causal_c0_signature, &signature);
      fprintf(stderr,
              "macrunner-hb-causal-ladder: phase=draw-select control=%s "
              "result=skip reason=canonical-logical-signature-mismatch "
              "logical_pso=0x%016llx expected_checksum=0x%016llx "
              "observed_checksum=0x%016llx skip_index=%u\n",
              macrunner_causal_phase_name(phase),
              (unsigned long long)pipeline->pso_id,
              (unsigned long long)MACRUNNER_CAUSAL_SIGNATURE_CHECKSUM,
              (unsigned long long)observed_checksum, skip_log);
      fflush(stderr);
    }
    return nil;
  }

  bool expected_busy = false;
  if (!atomic_compare_exchange_strong_explicit(
          &macrunner_causal_stage_busy, &expected_busy, true,
          memory_order_acq_rel, memory_order_acquire))
    return nil;
  if (atomic_load_explicit(&macrunner_causal_control_claimed_mask,
                           memory_order_acquire) & phase_bit) {
    atomic_store_explicit(&macrunner_causal_stage_busy, false,
                          memory_order_release);
    return nil;
  }
  if (!pipeline->causal_ready) {
    atomic_store_explicit(&macrunner_causal_invalid, true, memory_order_release);
    atomic_store_explicit(&macrunner_causal_stage_busy, false,
                          memory_order_release);
    fprintf(stderr,
            "macrunner-hb-causal-ladder: phase=draw-select control=%s "
            "result=invalid reason=physical-variants-unavailable "
            "logical_pso=0x%016llx\n",
            macrunner_causal_phase_name(phase),
            (unsigned long long)pipeline->pso_id);
    return nil;
  }

  id<MTLRenderPipelineState> selected =
      (id<MTLRenderPipelineState>)pipeline->causal_pso[phase];
  if (!selected) {
    atomic_store_explicit(&macrunner_causal_invalid, true, memory_order_release);
    atomic_store_explicit(&macrunner_causal_stage_busy, false, memory_order_release);
    fprintf(stderr,
            "macrunner-hb-causal-ladder: phase=draw-select control=%s "
            "result=invalid reason=null-physical-pso logical_pso=0x%016llx\n",
            macrunner_causal_phase_name(phase),
            (unsigned long long)pipeline->pso_id);
    return nil;
  }

  const NSUInteger sidechannel_length =
      (NSUInteger)MACRUNNER_CAUSAL_SIDECHANNEL_WIDTH *
      MACRUNNER_CAUSAL_SIDECHANNEL_HEIGHT * 16u;
  id<MTLCommandBuffer> command_buffer =
      (id<MTLCommandBuffer>)state->command_buffer;
  id<MTLBuffer> sidechannel = [command_buffer.device
      newBufferWithLength:sidechannel_length
                  options:MTLResourceStorageModeShared];
  uint32_t *words = (uint32_t *)[sidechannel contents];
  if (!sidechannel || !words) {
    [sidechannel release];
    atomic_store_explicit(&macrunner_causal_invalid, true,
                          memory_order_release);
    atomic_store_explicit(&macrunner_causal_stage_busy, false,
                          memory_order_release);
    fprintf(stderr,
            "macrunner-hb-causal-ladder: phase=sidechannel-prepare "
            "control=%s result=invalid reason=shared-buffer-allocation\n",
            macrunner_causal_phase_name(phase));
    return nil;
  }
  const NSUInteger word_count = sidechannel_length / sizeof(uint32_t);
  for (NSUInteger i = 0; i < word_count; i++)
    words[i] = MACRUNNER_CAUSAL_SIDECHANNEL_SENTINEL;
  sidechannel.label = [NSString stringWithFormat:
      @"MacRunner exact fragment output %s", macrunner_causal_phase_name(phase)];
  [encoder setFragmentBuffer:sidechannel
                      offset:0
                     atIndex:MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX];
  if (sidechannel_out)
    *sidechannel_out = sidechannel;

  uint32_t old_claimed = atomic_fetch_or_explicit(
      &macrunner_causal_control_claimed_mask, phase_bit, memory_order_acq_rel);
  if (old_claimed & phase_bit) {
    [sidechannel release];
    atomic_store_explicit(&macrunner_causal_stage_busy, false,
                          memory_order_release);
    return nil;
  }
  state->causal_target_seen = YES;
  state->causal_signature_match = YES;
  state->causal_phase = phase;
  state->causal_target_draw_count = state->draw_count + 1;
  if (phase_out)
    *phase_out = phase;
  fprintf(
      stderr,
      "macrunner-hb-causal-ladder: phase=draw-select control=%s result=ok "
      "logical_pso=0x%016llx original_pso=%p physical_pso=%p "
      "primitive=%u index_count=%llu index_type=%u index_buffer=%p "
      "index_offset=%llu instances=%u base_vertex=%d base_instance=%u "
      "viewport=%.0f,%.0f,%.0f,%.0f scissor=%llu,%llu,%llu,%llu "
      "sequence_count=%u expected_mask=0x%02x claimed_mask=0x%02x\n",
      macrunner_causal_phase_name(phase),
      (unsigned long long)pipeline->pso_id, (void *)(uintptr_t)state->pso,
      selected, draw->primitive_type, (unsigned long long)draw->index_count,
      draw->index_type, (void *)(uintptr_t)draw->index_buffer,
      (unsigned long long)draw->index_buffer_offset, draw->instance_count,
      draw->base_vertex, draw->base_instance, state->viewport.originX,
      state->viewport.originY, state->viewport.width, state->viewport.height,
      (unsigned long long)state->scissor.x,
      (unsigned long long)state->scissor.y,
      (unsigned long long)state->scissor.width,
      (unsigned long long)state->scissor.height,
      macrunner_causal_control_count(),
      (unsigned)macrunner_causal_control_mask(),
      (unsigned)atomic_load_explicit(&macrunner_causal_control_claimed_mask,
                                     memory_order_acquire));
  fprintf(stderr,
          "macrunner-hb-causal-ladder: phase=sidechannel-prepare control=%s "
          "result=ok buffer=%p slot=%u bytes=%lu restored_buffer=%p "
          "restored_offset=%llu\n",
          macrunner_causal_phase_name(phase), sidechannel,
          MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX,
          (unsigned long)sidechannel_length,
          (void *)(uintptr_t)state->fragment_buffers[
              MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX],
          (unsigned long long)state->fragment_buffer_offsets[
              MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX]);
  fflush(stderr);
  return selected;
}

static void
macrunner_causal_finish_draw(MacRunnerRenderProbeState *state,
                             id<MTLBuffer> sidechannel, unsigned phase) {
  if (!state || !sidechannel || phase >= 4) {
    [sidechannel release];
    atomic_store_explicit(&macrunner_causal_invalid, true,
                          memory_order_release);
    atomic_store_explicit(&macrunner_causal_stage_busy, false,
                          memory_order_release);
    return;
  }
  id<MTLCommandBuffer> command_buffer =
      (id<MTLCommandBuffer>)state->command_buffer;
  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    macrunner_causal_sidechannel_analyze(completed, sidechannel, phase);
    [sidechannel release];
  }];
  const uint32_t phase_bit = UINT32_C(1) << phase;
  BOOL scheduled =
      macrunner_causal_phase_requested(phase) &&
      (atomic_load_explicit(&macrunner_causal_control_claimed_mask,
                            memory_order_acquire) & phase_bit);
  if (!scheduled)
    atomic_store_explicit(&macrunner_causal_invalid, true,
                          memory_order_release);
  atomic_store_explicit(&macrunner_causal_stage_busy, false,
                        memory_order_release);
  fprintf(stderr,
          "macrunner-hb-causal-ladder: phase=sidechannel-schedule control=%s "
          "result=%s target_draw=%u buffer=%p one_shot=1 same_logical_draw=1 "
          "sequence_count=%u expected_mask=0x%02x claimed_mask=0x%02x\n",
          macrunner_causal_phase_name(phase), scheduled ? "ok" : "invalid",
          state->causal_target_draw_count, sidechannel,
          macrunner_causal_control_count(),
          (unsigned)macrunner_causal_control_mask(),
          (unsigned)atomic_load_explicit(&macrunner_causal_control_claimed_mask,
                                         memory_order_acquire));
  fflush(stderr);
}

/*
 * The side-channel proves a fragment output, not the Metal attachment that
 * receives it.  Keep this separate, explicit, and default-off: it samples the
 * exact causal draw's active color target after its encoder ends.  It is not a
 * claim that this target is the surface ultimately presented by DXMT.
 */
static BOOL
macrunner_causal_target_readback_enabled(void) {
  const char *value = getenv("MACRUNNER_HB_CAUSAL_TARGET_READBACK");
  return value && value[0] && strcmp(value, "0");
}

/*
 * This is deliberately separate from the causal target readback: the latter
 * samples the attachment of the selected draw, whereas this one samples the
 * CAMetalDrawable texture that the same command buffer actually presents.
 * It is armed only by a valid causal encoder and defaults off.
 */
static BOOL
macrunner_causal_present_surface_readback_enabled(void) {
  const char *value = getenv("MACRUNNER_HB_CAUSAL_PRESENT_SURFACE_READBACK");
  return value && value[0] && strcmp(value, "0");
}

static BOOL
macrunner_causal_present_surface_apply_control(id<MTLCommandBuffer> command_buffer,
                                               id<MTLTexture> texture,
                                               unsigned phase) {
  const char *control = getenv("MACRUNNER_HB_CAUSAL_PRESENT_SURFACE_CONTROL");
  if (!control || !control[0] || !strcmp(control, "0"))
    return YES;
  if (strcmp(control, "C1") || phase != 1) {
    fprintf(stderr,
            "macrunner-hb-causal-present-surface: phase=control result=invalid "
            "control=%s causal_control=%s reason=only-C1-is-supported\n",
            control, macrunner_causal_phase_name(phase));
    return NO;
  }
  MTLRenderPassDescriptor *descriptor = [[MTLRenderPassDescriptor alloc] init];
  descriptor.colorAttachments[0].texture = texture;
  descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
  descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
  descriptor.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:descriptor];
  [descriptor release];
  if (!encoder) {
    fprintf(stderr,
            "macrunner-hb-causal-present-surface: phase=c1-inject result=invalid "
            "reason=render-encoder-allocation texture=%p\n", texture);
    return NO;
  }
  [encoder endEncoding];
  fprintf(stderr,
          "macrunner-hb-causal-present-surface: phase=c1-inject result=ok "
          "texture=%p value=1,0,1,1\n", texture);
  return YES;
}

/* Direct observable surface, intentionally not restricted to a causal draw. */
static BOOL
macrunner_present_surface_readback_enabled(void) {
  const char *value = getenv("MACRUNNER_HB_PRESENT_SURFACE_READBACK");
  return value && value[0] && strcmp(value, "0");
}

/*
 * A late frame is the relevant boundary for Hollow Knight.  Keeping the
 * ordinal count uncapped while copying every drawable would allocate a staging
 * buffer for every Present, so an opt-in selector takes exactly one sample.
 * The selector is parsed once because the child environment is immutable.
 */
static BOOL
macrunner_present_surface_readback_ordinal_matches(uint64_t ordinal) {
  static dispatch_once_t once;
  static BOOL valid = YES;
  static BOOL configured;
  static uint64_t selected_ordinal;

  dispatch_once(&once, ^{
    const char *value = getenv("MACRUNNER_HB_PRESENT_SURFACE_READBACK_ORDINAL");
    if (!value || !value[0] || !strcmp(value, "0"))
      return;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || end == value || !end || *end || !parsed) {
      valid = NO;
      fprintf(stderr,
              "macrunner-hb-present-surface: phase=selector result=invalid "
              "value=%s reason=expected-positive-decimal-ordinal\n", value);
      return;
    }
    configured = YES;
    selected_ordinal = (uint64_t)parsed;
    fprintf(stderr,
            "macrunner-hb-present-surface: phase=selector result=ok ordinal=%llu\n",
            (unsigned long long)selected_ordinal);
  });
  return valid && (!configured || ordinal == selected_ordinal);
}

static BOOL
macrunner_present_surface_apply_control(id<MTLCommandBuffer> command_buffer,
                                         id<MTLTexture> texture,
                                         BOOL *did_inject) {
  if (did_inject)
    *did_inject = NO;
  const char *control = getenv("MACRUNNER_HB_PRESENT_SURFACE_CONTROL");
  if (!control || !control[0] || !strcmp(control, "0"))
    return YES;
  if (strcmp(control, "C1")) {
    fprintf(stderr,
            "macrunner-hb-present-surface: phase=control result=invalid "
            "control=%s reason=only-C1-is-supported\n", control);
    return NO;
  }
  MTLRenderPassDescriptor *descriptor = [[MTLRenderPassDescriptor alloc] init];
  descriptor.colorAttachments[0].texture = texture;
  descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
  descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
  descriptor.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:descriptor];
  [descriptor release];
  if (!encoder) {
    fprintf(stderr,
            "macrunner-hb-present-surface: phase=c1-inject result=invalid "
            "reason=render-encoder-allocation texture=%p\n", texture);
    return NO;
  }
  [encoder endEncoding];
  if (did_inject)
    *did_inject = YES;
  fprintf(stderr,
          "macrunner-hb-present-surface: phase=c1-inject result=ok "
          "texture=%p value=1,0,1,1\n", texture);
  return YES;
}

static void
macrunner_present_surface_schedule(id<MTLCommandBuffer> command_buffer,
                                   id<CAMetalDrawable> drawable) {
  if (!macrunner_present_surface_readback_enabled())
    return;
  uint64_t ordinal = atomic_fetch_add_explicit(&macrunner_present_surface_ordinal, 1,
                                               memory_order_relaxed) + 1;
  if (!macrunner_present_surface_readback_ordinal_matches(ordinal))
    return;
  id<MTLTexture> texture = drawable ? drawable.texture : nil;
  if (!texture) {
    fprintf(stderr,
            "macrunner-hb-present-surface: phase=schedule result=invalid "
            "ordinal=%llu command_buffer=%p drawable=%p reason=null-texture\n",
            (unsigned long long)ordinal, command_buffer, drawable);
  } else {
    BOOL direct_control_match = NO;
    if (!macrunner_present_surface_apply_control(command_buffer, texture,
                                                 &direct_control_match))
      return;
    if (direct_control_match) {
      MacRunnerPresentAttachmentTrace *trace =
          macrunner_present_attachment_trace_for_command_buffer(command_buffer, YES);
      if (trace)
        trace->direct_control_writes++;
    }
    macrunner_present_attachment_trace_log(command_buffer, texture, ordinal,
                                           direct_control_match);
    fprintf(stderr,
            "macrunner-hb-present-surface: phase=schedule result=ok "
            "ordinal=%llu command_buffer=%p drawable=%p texture=%p\n",
            (unsigned long long)ordinal, command_buffer, drawable, texture);
    macrunner_gpu_readback_schedule(command_buffer, texture,
                                    MacRunnerGPUReadbackPresentedSurface, ordinal);
  }
}

static void
macrunner_causal_finish_encoder(MacRunnerRenderProbeState *state) {
  if (!state || !state->causal_target_seen)
    return;
  unsigned phase = state->causal_phase;
  const uint32_t expected_mask = macrunner_causal_control_mask();
  const uint32_t claimed_mask = atomic_load_explicit(
      &macrunner_causal_control_claimed_mask, memory_order_acquire);
  BOOL valid = phase < 4 && expected_mask != 0 &&
               (claimed_mask & expected_mask) == expected_mask &&
               state->causal_signature_match &&
               !atomic_load_explicit(&macrunner_causal_invalid,
                                     memory_order_acquire);
  fprintf(stderr,
          "macrunner-hb-causal-ladder: phase=encoder-end control=%s "
          "result=%s isolation=shader-sidechannel target_draw=%u "
          "final_draw=%u rtv=%p sequence_count=%u expected_mask=0x%02x "
          "claimed_mask=0x%02x\n",
          macrunner_causal_phase_name(phase), valid ? "ok" : "invalid",
          state->causal_target_draw_count, state->draw_count,
          (void *)(uintptr_t)state->render_target,
          macrunner_causal_control_count(), (unsigned)expected_mask,
          (unsigned)claimed_mask);
  if (!valid)
    atomic_store_explicit(&macrunner_causal_invalid, true, memory_order_release);
  if (valid && macrunner_causal_present_surface_readback_enabled()) {
    id<MTLCommandBuffer> command_buffer = (id<MTLCommandBuffer>)state->command_buffer;
    NSNumber *existing = objc_getAssociatedObject(
        command_buffer, &macrunner_causal_present_surface_phase_key);
    if (existing) {
      fprintf(stderr,
              "macrunner-hb-causal-present-surface: phase=arm result=skip "
              "control=%s command_buffer=%p reason=already-armed\n",
              macrunner_causal_phase_name(phase), command_buffer);
    } else {
      NSNumber *armed_phase = [NSNumber numberWithUnsignedInt:phase];
      objc_setAssociatedObject(command_buffer, &macrunner_causal_present_surface_phase_key,
                               armed_phase, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        NSNumber *pending = objc_getAssociatedObject(
            completed, &macrunner_causal_present_surface_phase_key);
        if (pending) {
          fprintf(stderr,
                  "macrunner-hb-causal-present-surface: phase=complete result=not-presented "
                  "control=%s command_buffer=%p\n",
                  macrunner_causal_phase_name(pending.unsignedIntValue), completed);
          fflush(stderr);
        }
      }];
      fprintf(stderr,
              "macrunner-hb-causal-present-surface: phase=arm result=ok "
              "control=%s command_buffer=%p\n",
              macrunner_causal_phase_name(phase), command_buffer);
    }
  }
  fflush(stderr);
}

static NSMapTable *
macrunner_shader_texture_registry(void) {
  static NSMapTable *registry;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    registry = [[NSMapTable strongToWeakObjectsMapTable] retain];
  });
  return registry;
}

static void
macrunner_shader_texture_register(uint64_t gpu_resource_id, id<MTLTexture> texture) {
  if (!macrunner_shader_inputs_probe_enabled() || !gpu_resource_id || !texture)
    return;
  NSMapTable *registry = macrunner_shader_texture_registry();
  @synchronized(registry) {
    [registry setObject:texture forKey:[NSNumber numberWithUnsignedLongLong:gpu_resource_id]];
  }
}

static id<MTLTexture>
macrunner_shader_texture_lookup(uint64_t gpu_resource_id) {
  if (!gpu_resource_id)
    return nil;
  NSMapTable *registry = macrunner_shader_texture_registry();
  @synchronized(registry) {
    return [registry objectForKey:[NSNumber numberWithUnsignedLongLong:gpu_resource_id]];
  }
}

static int
macrunner_shader_texture_readback_take_slot(uint64_t gpu_resource_id) {
  static uint64_t seen[16];
  static unsigned count;
  NSMapTable *registry = macrunner_shader_texture_registry();
  @synchronized(registry) {
    for (unsigned i = 0; i < count; i++)
      if (seen[i] == gpu_resource_id)
        return 0;
    if (count >= sizeof(seen) / sizeof(seen[0]))
      return 0;
    seen[count++] = gpu_resource_id;
    return 1;
  }
}

static uint64_t
macrunner_shader_inputs_hash(const uint8_t *bytes, size_t length) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (size_t i = 0; i < length; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static void
macrunner_vertex_data_log_draw(MacRunnerRenderProbeState *state,
                               const char *kind, uint64_t primitive_type,
                               uint64_t count, uint64_t instance_count,
                               uint64_t start, int64_t base_vertex,
                               uint64_t base_instance,
                               obj_handle_t index_handle,
                               uint64_t index_offset,
                               uint64_t index_type) {
  unsigned ordinal;
  if (!state || !state->pso ||
      !macrunner_vertex_data_draw_take_slot(&ordinal))
    return;

  MacRunnerPipelineProbeInfo *pipeline =
      objc_getAssociatedObject((id)state->pso, &macrunner_pipeline_probe_info_key);
  id<MTLBuffer> table = (id<MTLBuffer>)state->vertex_buffers[16];
  uint64_t table_offset = state->vertex_buffer_offsets[16];
  NSUInteger table_length = table ? [table length] : 0;
  const uint8_t *table_contents = table ? (const uint8_t *)[table contents] : NULL;
  uint64_t entry[2] = {};
  BOOL entry_valid = table_contents && table_offset <= table_length &&
                     table_length - table_offset >= sizeof(entry);
  if (entry_valid)
    memcpy(entry, table_contents + table_offset, sizeof(entry));

  uint64_t vertex_gpu = entry[0];
  uint32_t vertex_stride = (uint32_t)entry[1];
  uint32_t vertex_length = (uint32_t)(entry[1] >> 32);

  id<MTLBuffer> index_buffer = (id<MTLBuffer>)index_handle;
  NSUInteger index_length = index_buffer ? [index_buffer length] : 0;
  const uint8_t *index_contents =
      index_buffer ? (const uint8_t *)[index_buffer contents] : NULL;
  size_t index_size = index_type == MTLIndexTypeUInt32 ? 4 : 2;
  uint64_t requested = count <= SIZE_MAX / index_size ? count * index_size : SIZE_MAX;
  size_t index_scan = 0;
  if (index_contents && index_offset <= index_length) {
    uint64_t available = index_length - index_offset;
    uint64_t bounded = MIN(requested, available);
    index_scan = (size_t)MIN(bounded, UINT64_C(1024) * 1024);
  }
  const uint8_t *index_bytes = index_scan ? index_contents + index_offset : NULL;
  uint64_t index_hash =
      index_bytes ? macrunner_causal_hash_bytes(index_bytes, index_scan) : 0;
  uint32_t indices[6] = {};
  uint32_t index_min = UINT32_MAX;
  uint32_t index_max = 0;
  size_t parsed_indices = index_scan / index_size;
  for (size_t i = 0; i < parsed_indices; i++) {
    uint32_t index;
    if (index_size == 4)
      memcpy(&index, index_bytes + i * index_size, sizeof(index));
    else {
      uint16_t value;
      memcpy(&value, index_bytes + i * index_size, sizeof(value));
      index = value;
    }
    if (i < 6)
      indices[i] = index;
    index_min = MIN(index_min, index);
    index_max = MAX(index_max, index);
  }

  BOOL exact = pipeline &&
      pipeline->pso_id == UINT64_C(0x9d2b46c27af732f4) &&
      strcmp(pipeline->vertex_name,
             "vs_59c2a0b5_adcd61f38fa5038495d5d87e7f3a67bb8d794d86") == 0 &&
      strcmp(pipeline->fragment_name,
             "ps_bef43b20_f8e9cf38dc7294cc7203c7fb6cd68b27ed713b6a") == 0;
  fprintf(
      stderr,
      "macrunner-hb-vertex-data: side=winemetal phase=draw ordinal=%u exact=%u "
      "kind=%s pso_id=0x%016llx vs=%s ps=%s primitive=%llu count=%llu "
      "instances=%llu start=%llu base_vertex=%lld base_instance=%llu "
      "table=%p table_offset=%llu table_length=%llu table_cpu_visible=%u "
      "entry_valid=%u vertex_gpu=0x%016llx vertex_stride=%u vertex_length=%u "
      "index_buffer=%p index_offset=%llu index_type=%llu index_length=%llu "
      "index_cpu_visible=%u index_scan=%zu index_hash=0x%016llx "
      "index_parsed=%zu index_min=%u index_max=%u indices=%u,%u,%u,%u,%u,%u\n",
      ordinal, exact, kind,
      (unsigned long long)(pipeline ? pipeline->pso_id : 0),
      pipeline ? pipeline->vertex_name : "unknown",
      pipeline ? pipeline->fragment_name : "unknown",
      (unsigned long long)primitive_type, (unsigned long long)count,
      (unsigned long long)instance_count, (unsigned long long)start,
      (long long)base_vertex, (unsigned long long)base_instance, table,
      (unsigned long long)table_offset, (unsigned long long)table_length,
      table_contents != NULL, entry_valid, (unsigned long long)vertex_gpu,
      vertex_stride, vertex_length, index_buffer,
      (unsigned long long)index_offset, (unsigned long long)index_type,
      (unsigned long long)index_length, index_contents != NULL, index_scan,
      (unsigned long long)index_hash, parsed_indices,
      parsed_indices ? index_min : 0, parsed_indices ? index_max : 0,
      indices[0], indices[1], indices[2], indices[3], indices[4], indices[5]);
  fflush(stderr);
}

static int
macrunner_shader_inputs_draw_take_slot(unsigned *ordinal) {
  static atomic_uint count;
  static unsigned limit;
  if (!macrunner_shader_inputs_probe_enabled())
    return 0;
  if (!limit) {
    const char *value = getenv("MACRUNNER_HB_SHADER_INPUTS_DRAW_MAX");
    char *end = NULL;
    unsigned long parsed = value && value[0] ? strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 1024 ? parsed : 32;
  }
  unsigned current = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
  if (ordinal)
    *ordinal = current + 1;
  return current < limit;
}

static void
macrunner_shader_inputs_log_table(MacRunnerRenderProbeState *state,
                                  unsigned ordinal, const char *stage,
                                  unsigned slot, obj_handle_t handle,
                                  uint64_t offset) {
  id<MTLBuffer> buffer = (id<MTLBuffer>)handle;
  if (!buffer) {
    fprintf(stderr,
            "macrunner-hb-shader-inputs: side=winemetal kind=table ordinal=%u "
            "stage=%s slot=%u buffer=null offset=%llu\n",
            ordinal, stage, slot, (unsigned long long)offset);
    return;
  }
  NSUInteger length = [buffer length];
  const uint8_t *contents = (const uint8_t *)[buffer contents];
  size_t scan = contents && offset < length ? MIN((NSUInteger)512, length - offset) : 0;
  const uint8_t *bytes = scan ? contents + offset : NULL;
  size_t nonzero = 0;
  for (size_t i = 0; i < scan; i++)
    nonzero += bytes[i] != 0;
  uint64_t q[8] = {};
  size_t qcount = MIN((size_t)8, scan / sizeof(uint64_t));
  if (qcount)
    memcpy(q, bytes, qcount * sizeof(uint64_t));
  if (state && stage[0] == 'p' && slot == 30) {
    for (size_t i = 0; i < qcount; i++) {
      id<MTLTexture> texture = macrunner_shader_texture_lookup(q[i]);
      if (!texture)
        continue;
      BOOL duplicate = NO;
      for (uint32_t j = 0; j < state->shader_texture_count; j++)
        duplicate |= state->shader_texture_ids[j] == q[i];
      if (!duplicate && state->shader_texture_count < 8) {
        uint32_t index = state->shader_texture_count++;
        state->shader_texture_ids[index] = q[i];
        state->shader_textures[index] = (obj_handle_t)texture;
        fprintf(stderr,
                "macrunner-hb-shader-inputs: side=winemetal kind=texture-map "
                "ordinal=%u qword=%zu gpu_id=0x%llx texture=%p format=%lu size=%lux%lu\n",
                ordinal, i, (unsigned long long)q[i], texture,
                (unsigned long)[texture pixelFormat],
                (unsigned long)[texture width], (unsigned long)[texture height]);
      }
    }
  }
  fprintf(stderr,
          "macrunner-hb-shader-inputs: side=winemetal kind=table ordinal=%u "
          "stage=%s slot=%u buffer=%p offset=%llu length=%llu storage=%lu "
          "cpu_visible=%u scan=%zu hash=%016llx nonzero=%zu "
          "qwords=%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx\n",
          ordinal, stage, slot, buffer, (unsigned long long)offset,
          (unsigned long long)length, (unsigned long)[buffer storageMode],
          contents != NULL, scan,
          (unsigned long long)(bytes ? macrunner_shader_inputs_hash(bytes, scan) : 0),
          nonzero, (unsigned long long)q[0], (unsigned long long)q[1],
          (unsigned long long)q[2], (unsigned long long)q[3],
          (unsigned long long)q[4], (unsigned long long)q[5],
          (unsigned long long)q[6], (unsigned long long)q[7]);
}

static void
macrunner_shader_inputs_log_draw(MacRunnerRenderProbeState *state,
                                 const char *kind) {
  unsigned ordinal;
  if (!state || !macrunner_shader_inputs_draw_take_slot(&ordinal))
    return;
  fprintf(stderr,
          "macrunner-hb-shader-inputs: side=winemetal kind=draw ordinal=%u draw=%s "
          "pso=%p rtv=%p\n", ordinal, kind,
          (void *)(uintptr_t)state->pso, (void *)(uintptr_t)state->render_target);
  macrunner_shader_inputs_log_table(state, ordinal, "vs", 29, state->vertex_buffers[29],
                                    state->vertex_buffer_offsets[29]);
  macrunner_shader_inputs_log_table(state, ordinal, "vs", 30, state->vertex_buffers[30],
                                    state->vertex_buffer_offsets[30]);
  macrunner_shader_inputs_log_table(state, ordinal, "ps", 29, state->fragment_buffers[29],
                                    state->fragment_buffer_offsets[29]);
  macrunner_shader_inputs_log_table(state, ordinal, "ps", 30, state->fragment_buffers[30],
                                    state->fragment_buffer_offsets[30]);
  fflush(stderr);
}

static int
macrunner_gpu_readback_draw_take_slot(unsigned *ordinal) {
  static atomic_uint count;
  static unsigned limit;
  if (!macrunner_gpu_readback_probe_enabled())
    return 0;
  if (!limit) {
    const char *value = getenv("MACRUNNER_HB_GPU_READBACK_DRAW_MAX");
    char *end = NULL;
    unsigned long parsed = value && value[0] ? strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 1024 ? parsed : 32;
  }
  unsigned current = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
  if (ordinal)
    *ordinal = current + 1;
  return current < limit;
}

static int
macrunner_gpu_readback_phase_take_slot(uint32_t phase, uint64_t *tag) {
  static atomic_uint counters[3];
  static atomic_ullong sequence;
  static unsigned limit;
  if (!macrunner_gpu_readback_probe_enabled() ||
      phase < MacRunnerGPUReadbackAfterClear || phase > MacRunnerGPUReadbackPrePresent)
    return 0;
  if (!limit) {
    const char *value = getenv("MACRUNNER_HB_GPU_READBACK_MAX");
    char *end = NULL;
    unsigned long parsed = value && value[0] ? strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 64 ? parsed : 4;
  }
  unsigned current = atomic_fetch_add_explicit(&counters[phase - 1], 1, memory_order_relaxed);
  if (tag)
    *tag = atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed) + 1;
  return current < limit;
}

static void
macrunner_gpu_readback_log_draw(
    MacRunnerRenderProbeState *state, id<MTLRenderCommandEncoder> encoder,
    const char *kind, unsigned primitive, uint64_t count, uint32_t instances,
    uint64_t start, int32_t base_vertex, uint32_t base_instance,
    obj_handle_t index_buffer, uint64_t index_offset,
    obj_handle_t indirect_buffer, uint64_t indirect_offset) {
  unsigned ordinal;
  if (!state || !macrunner_gpu_readback_draw_take_slot(&ordinal))
    return;

  MacRunnerPipelineProbeInfo *pipeline =
      state->pso ? objc_getAssociatedObject((id)state->pso, &macrunner_pipeline_probe_info_key) : nil;
  fprintf(
      stderr,
      "macrunner-hb-gpu-probe: phase=draw ordinal=%u kind=%s encoder=%p "
      "rtv=%p rt=%ux%u pso=%p vs=%p ps=%p prim=%u count=%llu instances=%u "
      "start=%llu base_vertex=%d base_instance=%u ib=%p iboff=%llu indirect=%p indirect_off=%llu "
      "viewport_valid=%u viewport=%.3f,%.3f,%.3f,%.3f,%.6f,%.6f "
      "scissor_valid=%u scissor=%llu,%llu,%llu,%llu raster=%u,%u,%u "
      "vb0=%p+%llu vb1=%p+%llu vb2=%p+%llu vb3=%p+%llu vb16=%p+%llu "
      "fb0=%p+%llu fb1=%p+%llu fb29=%p+%llu fb30=%p+%llu "
      "ft0=%p ft1=%p ft2=%p ft3=%p ft4=%p ft5=%p ft6=%p ft7=%p "
      "samplers=argument-buffer\n",
      ordinal, kind, encoder, (void *)(uintptr_t)state->render_target,
      state->render_target_width, state->render_target_height,
      (void *)(uintptr_t)state->pso,
      pipeline ? (void *)(uintptr_t)pipeline->vertex_function : NULL,
      pipeline ? (void *)(uintptr_t)pipeline->fragment_function : NULL,
      primitive, (unsigned long long)count, instances,
      (unsigned long long)start, base_vertex, base_instance,
      (void *)(uintptr_t)index_buffer, (unsigned long long)index_offset,
      (void *)(uintptr_t)indirect_buffer, (unsigned long long)indirect_offset,
      state->viewport_valid, state->viewport.originX, state->viewport.originY,
      state->viewport.width, state->viewport.height, state->viewport.znear,
      state->viewport.zfar, state->scissor_valid,
      (unsigned long long)state->scissor.x, (unsigned long long)state->scissor.y,
      (unsigned long long)state->scissor.width, (unsigned long long)state->scissor.height,
      state->fill_mode, state->cull_mode, state->depth_clip_mode,
      (void *)(uintptr_t)state->vertex_buffers[0],
      (unsigned long long)state->vertex_buffer_offsets[0],
      (void *)(uintptr_t)state->vertex_buffers[1],
      (unsigned long long)state->vertex_buffer_offsets[1],
      (void *)(uintptr_t)state->vertex_buffers[2],
      (unsigned long long)state->vertex_buffer_offsets[2],
      (void *)(uintptr_t)state->vertex_buffers[3],
      (unsigned long long)state->vertex_buffer_offsets[3],
      (void *)(uintptr_t)state->vertex_buffers[16],
      (unsigned long long)state->vertex_buffer_offsets[16],
      (void *)(uintptr_t)state->fragment_buffers[0],
      (unsigned long long)state->fragment_buffer_offsets[0],
      (void *)(uintptr_t)state->fragment_buffers[1],
      (unsigned long long)state->fragment_buffer_offsets[1],
      (void *)(uintptr_t)state->fragment_buffers[29],
      (unsigned long long)state->fragment_buffer_offsets[29],
      (void *)(uintptr_t)state->fragment_buffers[30],
      (unsigned long long)state->fragment_buffer_offsets[30],
      (void *)(uintptr_t)state->fragment_textures[0],
      (void *)(uintptr_t)state->fragment_textures[1],
      (void *)(uintptr_t)state->fragment_textures[2],
      (void *)(uintptr_t)state->fragment_textures[3],
      (void *)(uintptr_t)state->fragment_textures[4],
      (void *)(uintptr_t)state->fragment_textures[5],
      (void *)(uintptr_t)state->fragment_textures[6],
      (void *)(uintptr_t)state->fragment_textures[7]
  );
  fflush(stderr);
}

static int macrunner_render_pipeline_probe_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("MACRUNNER_HB_RENDER_PIPELINE_PROBE");
    enabled = value && value[0] && strcmp(value, "0") != 0;
  }
  return enabled;
}

static int macrunner_render_pipeline_probe_take_slot(void) {
  static atomic_uint count;
  static unsigned limit;
  if (!macrunner_render_pipeline_probe_enabled())
    return 0;
  if (!limit) {
    const char *value = getenv("MACRUNNER_HB_RENDER_PIPELINE_PROBE_MAX");
    char *end = NULL;
    unsigned long parsed = value && value[0] ? strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 1000000 ? parsed : 8192;
  }
  return atomic_fetch_add_explicit(&count, 1, memory_order_relaxed) < limit;
}

static void macrunner_render_pipeline_probe_log(
    const char *phase, const void *encoder, const void *head, unsigned cmd_type,
    const void *pso, unsigned commands, unsigned set_pso, unsigned draws,
    unsigned missing_pso) {
  if (!macrunner_render_pipeline_probe_take_slot())
    return;
  fprintf(stderr,
          "macrunner-hb-render-pipeline: layer=winemetal-stream phase=%s "
          "encoder=%p head=%p cmd=%u pso=%p commands=%u setpso=%u draws=%u "
          "missing=%u\n",
          phase, encoder, head, cmd_type, pso, commands, set_pso, draws,
          missing_pso);
  fflush(stderr);
}

void
execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

static NTSTATUS
_NSObject_retain(NSObject **obj) {
  [*obj retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_release(NSObject **obj) {
  [*obj release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_object(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSArray *)params->handle objectAtIndex:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_count(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(NSArray *)params->handle count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCopyAllDevices(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)MTLCopyAllDevices();
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_recommendedMaxWorkingSetSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle recommendedMaxWorkingSetSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_currentAllocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle currentAllocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_name(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle name];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_getCString(void *obj) {
  struct unixcall_nsstring_getcstring *params = obj;
  params->ret = (uint32_t)[(NSString *)params->str getCString:(char *)params->buffer_ptr
                                                    maxLength:params->max_length
                                                     encoding:params->encoding];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newCommandQueue(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandQueueWithMaxCommandBufferCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSAutoreleasePool_alloc_init(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[NSAutoreleasePool alloc] init];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_commandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandQueue>)params->handle commandBuffer];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle commit];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_waitUntilCompleted(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle waitUntilCompleted];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_status(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLCommandBuffer>)params->handle status];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newSharedEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signaledValue(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLSharedEvent>)params->handle signaledValue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSignalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeSignalEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBuffer(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  if (info->memory.ptr) {
    buffer = [device newBufferWithBytesNoCopy:info->memory.ptr
                                       length:info->length
                                      options:(enum MTLResourceOptions)info->options
                                  deallocator:NULL];
  } else {
    buffer = [device newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
    info->memory.ptr = [buffer storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];
  }
  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSamplerState(void *obj) {
  struct unixcall_mtldevice_newsamplerstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTSamplerInfo *info = params->info.ptr;

  MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.borderColor = (MTLSamplerBorderColor)info->border_color;
  sampler_desc.rAddressMode = (MTLSamplerAddressMode)info->r_address_mode;
  sampler_desc.sAddressMode = (MTLSamplerAddressMode)info->s_address_mode;
  sampler_desc.tAddressMode = (MTLSamplerAddressMode)info->t_address_mode;
  sampler_desc.magFilter = (MTLSamplerMinMagFilter)info->mag_filter;
  sampler_desc.minFilter = (MTLSamplerMinMagFilter)info->min_filter;
  sampler_desc.mipFilter = (MTLSamplerMipFilter)info->mip_filter;
  sampler_desc.compareFunction = (MTLCompareFunction)info->compare_function;
  sampler_desc.lodMaxClamp = info->lod_max_clamp;
  sampler_desc.lodMinClamp = info->lod_min_clamp;
  sampler_desc.maxAnisotropy = info->max_anisotroy;
  sampler_desc.lodAverage = info->lod_average;
  sampler_desc.normalizedCoordinates = info->normalized_coords;
  sampler_desc.supportArgumentBuffers = info->support_argument_buffers;

  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];
  info->gpu_resource_id = info->support_argument_buffers ? [sampler gpuResourceID]._impl : 0;
  params->ret = (obj_handle_t)sampler;
  [sampler_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newDepthStencilState(void *obj) {
  struct unixcall_mtldevice_newdepthstencilstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTDepthStencilInfo *info = params->info.ptr;

  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)info->depth_compare_function;
  desc.depthWriteEnabled = info->depth_write_enabled;

  if (info->front_stencil.enabled) {
    desc.frontFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->front_stencil.depth_stencil_pass_op;
    desc.frontFaceStencil.depthFailureOperation = (MTLStencilOperation)info->front_stencil.depth_fail_op;
    desc.frontFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->front_stencil.stencil_fail_op;
    desc.frontFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->front_stencil.stencil_compare_function;
    desc.frontFaceStencil.writeMask = info->front_stencil.write_mask;
    desc.frontFaceStencil.readMask = info->front_stencil.read_mask;
  }

  if (info->back_stencil.enabled) {
    desc.backFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->back_stencil.depth_stencil_pass_op;
    desc.backFaceStencil.depthFailureOperation = (MTLStencilOperation)info->back_stencil.depth_fail_op;
    desc.backFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->back_stencil.stencil_fail_op;
    desc.backFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->back_stencil.stencil_compare_function;
    desc.backFaceStencil.writeMask = info->back_stencil.write_mask;
    desc.backFaceStencil.readMask = info->back_stencil.read_mask;
  }

  params->ret = (obj_handle_t)[device newDepthStencilStateWithDescriptor:desc];
  [desc release];
  return STATUS_SUCCESS;
}

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return (MTLPixelFormat)ORIGINAL_FORMAT(format);
}

void
fill_texture_descriptor(MTLTextureDescriptor *desc, struct WMTTextureInfo *info) {
  desc.textureType = (MTLTextureType)info->type;
  desc.pixelFormat = to_metal_pixel_format(info->pixel_format);
  desc.width = info->width;
  desc.height = info->height;
  desc.depth = info->depth;
  desc.arrayLength = info->array_length;
  desc.mipmapLevelCount = info->mipmap_level_count;
  desc.sampleCount = info->sample_count;
  desc.usage = (MTLTextureUsage)info->usage;
  desc.resourceOptions = (MTLResourceOptions)info->options;
};

void
extract_texture_descriptor(id<MTLTexture> desc, struct WMTTextureInfo *info) {
  info->type = desc.textureType;
  info->pixel_format = desc.pixelFormat;
  info->width = desc.width;
  info->height = desc.height;
  info->depth = desc.depth;
  info->array_length = desc.arrayLength;
  info->mipmap_level_count = desc.mipmapLevelCount;
  info->sample_count = desc.sampleCount;
  info->usage = desc.usage;
  info->options = (enum WMTResourceOptions)desc.resourceOptions;
  info->reserved = 0;
};

static NTSTATUS
_MTLDevice_newTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [device newTextureWithDescriptor:desc];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  macrunner_shader_texture_register(info->gpu_resource_id, ret);
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_newTexture(void *obj) {
  struct unixcall_mtlbuffer_newtexture *params = obj;
  id<MTLBuffer> buffer = (id<MTLBuffer>)params->buffer;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [buffer newTextureWithDescriptor:desc offset:params->offset bytesPerRow:params->bytes_per_row];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  macrunner_shader_texture_register(info->gpu_resource_id, ret);
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static inline MTLTextureSwizzleChannels
to_metal_swizzle(struct WMTTextureSwizzleChannels swizzle, enum WMTPixelFormat format) {
  if (format & WMTPixelFormatRGB1Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatR001Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormat0R01Swizzle) {
    return MTLTextureSwizzleChannelsMake(
        MTLTextureSwizzleOne, (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleOne, MTLTextureSwizzleOne
    );
  }
  if (format & WMTPixelFormatGBARSwizzle) {
    return MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b, (MTLTextureSwizzle)swizzle.a,
        (MTLTextureSwizzle)swizzle.r
    );
  }
  return MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b,
      (MTLTextureSwizzle)swizzle.a
  );
}

static NTSTATUS
_MTLTexture_newTextureView(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;

  id<MTLTexture> ret = [texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(params->format)
                        textureType:(MTLTextureType)params->texture_type
                             levels:NSMakeRange(params->level_start, params->level_count)
                             slices:NSMakeRange(params->slice_start, params->slice_count)
                            swizzle:to_metal_swizzle(params->swizzle, params->format)];
  params->ret = (obj_handle_t)ret;
  params->gpu_resource_id = [ret gpuResourceID]._impl;
  macrunner_shader_texture_register(params->gpu_resource_id, ret);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_minimumLinearTextureAlignmentForPixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle minimumLinearTextureAlignmentForPixelFormat:to_metal_pixel_format(params->arg)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newLibrary(void *obj) {
  struct unixcall_mtldevice_newlibrary *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSError *err = NULL;
  params->ret_library = (obj_handle_t)[device newLibraryWithData:(dispatch_data_t)params->data error:&err];
  params->ret_error = (obj_handle_t)err;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunction(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->handle;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->arg encoding:NSUTF8StringEncoding];
  params->ret = (obj_handle_t)[library newFunctionWithName:name];
  [name release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_lengthOfBytesUsingEncoding(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = (uint64_t)[(NSString *)params->handle lengthOfBytesUsingEncoding:(NSStringEncoding)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_description(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSObject *)params->handle description];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newComputePipelineState(void *obj) {
  struct unixcall_mtldevice_newcomputepso *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTComputePipelineInfo *info = params->info.ptr;
  MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
  NSError *err = NULL;
  descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
  descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_buffers & (1 << i))
      descriptor.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso =
      (obj_handle_t)[device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&err];
  params->ret_error = (obj_handle_t)err;
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addComputePipelineFunctionsWithDescriptor:descriptor
                                                                                                      error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle blitCommandEncoder];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle
      computeCommandEncoderWithDispatchType:params->arg ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  struct WMTRenderPassInfo *info = (struct WMTRenderPassInfo *)params->arg;
  MTLRenderPassDescriptor *descriptor = [[MTLRenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    descriptor.colorAttachments[i].level = info->colors[i].level;
    descriptor.colorAttachments[i].slice = info->colors[i].slice;
    descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
    descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
    descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
    descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
    descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
    descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
    descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }

  if (info->depth.texture) {
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }

  if (info->stencil.texture) {
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }

  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength = info->render_target_array_length;
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;

  if (info->tile_height && info->tile_width) {
    descriptor.tileWidth = info->tile_width;
    descriptor.tileHeight = info->tile_height;
  }

  macrunner_present_attachment_trace_record((id<MTLCommandBuffer>)params->handle, info);
  id<MTLRenderCommandEncoder> encoder =
      [(id<MTLCommandBuffer>)params->handle renderCommandEncoderWithDescriptor:descriptor];
  MacRunnerPresentAttachmentTrace *attachment_trace =
      macrunner_present_attachment_trace_for_command_buffer(
          (id<MTLCommandBuffer>)params->handle, NO);
  if (encoder && attachment_trace)
    objc_setAssociatedObject(encoder, &macrunner_present_attachment_trace_key,
                             attachment_trace, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  winemetal_render_encoder_set_has_pso(encoder, NO);
  if (encoder && macrunner_render_probe_state_enabled()) {
    MacRunnerRenderProbeState *state = [[MacRunnerRenderProbeState alloc] init];
    id<MTLTexture> render_target = (id<MTLTexture>)info->colors[0].texture;
    state->command_buffer = params->handle;
    state->render_target = info->colors[0].texture;
    state->render_target_width = info->render_target_width ? info->render_target_width : (uint32_t)[render_target width];
    state->render_target_height = info->render_target_height ? info->render_target_height : (uint32_t)[render_target height];
    state->fill_mode = WMTTriangleFillModeFill;
    state->cull_mode = WMTCullModeNone;
    state->depth_clip_mode = WMTDepthClipModeClip;
    state->color_load_action = info->colors[0].load_action;
    state->causal_phase = UINT32_MAX;
    objc_setAssociatedObject(encoder, &macrunner_render_probe_state_key, state,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [state release];
  }
  params->ret = (obj_handle_t)encoder;

  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_endEncoding(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)params->handle;
  MacRunnerRenderProbeState *state = macrunner_render_probe_state_enabled()
      ? objc_getAssociatedObject(encoder, &macrunner_render_probe_state_key) : nil;
  winemetal_render_encoder_set_has_pso((id<MTLRenderCommandEncoder>)params->handle, NO);
  [(id<MTLCommandEncoder>)params->handle endEncoding];
  macrunner_causal_finish_encoder(state);
  if (state && state->causal_target_seen && state->causal_phase < 4 &&
      state->causal_signature_match && macrunner_causal_target_readback_enabled()) {
    id<MTLTexture> target = (id<MTLTexture>)state->render_target;
    const char *control = macrunner_causal_phase_name(state->causal_phase);
    if (target) {
      fprintf(stderr,
              "macrunner-hb-causal-target-readback: phase=schedule control=%s "
              "result=ok target=%p size=%lux%lu target_draw=%u "
              "surface_claim=causal-target-not-final-backbuffer\n",
              control, target, (unsigned long)[target width],
              (unsigned long)[target height], state->causal_target_draw_count);
      fflush(stderr);
      macrunner_gpu_readback_schedule((id<MTLCommandBuffer>)state->command_buffer,
                                      target,
                                      MacRunnerGPUReadbackCausalC0 + state->causal_phase,
                                      state->causal_target_draw_count);
    } else {
      fprintf(stderr,
              "macrunner-hb-causal-target-readback: phase=schedule control=%s "
              "result=no-target surface_claim=causal-target-not-final-backbuffer\n",
              control);
      fflush(stderr);
    }
  }
  if (state) {
    uint64_t tag;
    if (state->draw_count) {
      for (uint32_t i = 0; i < state->shader_texture_count; i++) {
        if (macrunner_shader_texture_readback_take_slot(state->shader_texture_ids[i]))
          macrunner_gpu_readback_schedule((id<MTLCommandBuffer>)state->command_buffer,
                                          (id<MTLTexture>)state->shader_textures[i],
                                          MacRunnerGPUReadbackShaderTexture,
                                          state->shader_texture_ids[i]);
      }
      if (state->render_target &&
          macrunner_gpu_readback_phase_take_slot(MacRunnerGPUReadbackAfterRender, &tag))
        macrunner_gpu_readback_schedule((id<MTLCommandBuffer>)state->command_buffer,
                                        (id<MTLTexture>)state->render_target,
                                        MacRunnerGPUReadbackAfterRender, tag);
      if (state->fragment_textures[0] && state->fragment_textures[0] != state->render_target &&
          macrunner_gpu_readback_phase_take_slot(MacRunnerGPUReadbackPrePresent, &tag))
        macrunner_gpu_readback_schedule((id<MTLCommandBuffer>)state->command_buffer,
                                        (id<MTLTexture>)state->fragment_textures[0],
                                        MacRunnerGPUReadbackPrePresent, tag);
    } else if (state->render_target && state->color_load_action == WMTLoadActionClear &&
               macrunner_gpu_readback_phase_take_slot(MacRunnerGPUReadbackAfterClear, &tag)) {
      macrunner_gpu_readback_schedule((id<MTLCommandBuffer>)state->command_buffer,
                                      (id<MTLTexture>)state->render_target,
                                      MacRunnerGPUReadbackAfterClear, tag);
    }
  }
  return STATUS_SUCCESS;
}

#ifndef DXMT_NO_PRIVATE_API

typedef NS_ENUM(NSUInteger, MTLLogicOperation) {
  MTLLogicOperationClear,
  MTLLogicOperationSet,
  MTLLogicOperationCopy,
  MTLLogicOperationCopyInverted,
  MTLLogicOperationNoop,
  MTLLogicOperationInvert,
  MTLLogicOperationAnd,
  MTLLogicOperationNand,
  MTLLogicOperationOr,
  MTLLogicOperationNor,
  MTLLogicOperationXor,
  MTLLogicOperationEquivalence,
  MTLLogicOperationAndReverse,
  MTLLogicOperationAndInverted,
  MTLLogicOperationOrReverse,
  MTLLogicOperationOrInverted,
};

@interface
MTLRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

@interface
MTLMeshRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

#endif

static id<MTLFunction>
macrunner_opaque_magenta_fragment_function(id<MTLDevice> device) {
  if (!device)
    return nil;
  id<MTLFunction> function =
      objc_getAssociatedObject(device, &macrunner_magenta_fragment_function_key);
  if (function)
    return function;

  @synchronized(device) {
    function = objc_getAssociatedObject(device, &macrunner_magenta_fragment_function_key);
    if (function)
      return function;

    NSString *source =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct MacRunnerMagentaOut { float4 color [[color(0)]]; };\n"
         "fragment MacRunnerMagentaOut macrunner_probe_opaque_magenta() {\n"
         "  MacRunnerMagentaOut out; out.color = float4(1.0, 0.0, 1.0, 1.0); return out;\n"
         "}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
      fprintf(stderr,
              "macrunner-hb-fragment-output: side=winemetal phase=override-compile "
              "result=fail error=%s\n",
              error ? [[error description] UTF8String] : "<nil NSError>");
      fflush(stderr);
      return nil;
    }
    function = [library newFunctionWithName:@"macrunner_probe_opaque_magenta"];
    [library release];
    if (!function) {
      fprintf(stderr,
              "macrunner-hb-fragment-output: side=winemetal phase=override-compile "
              "result=no-function\n");
      fflush(stderr);
      return nil;
    }
    objc_setAssociatedObject(device, &macrunner_magenta_fragment_function_key,
                             function, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [function release];
    function = objc_getAssociatedObject(device, &macrunner_magenta_fragment_function_key);
    fprintf(stderr,
            "macrunner-hb-fragment-output: side=winemetal phase=override-compile "
            "result=ok function=%s\n", [[function name] UTF8String]);
    fflush(stderr);
  }
  return function;
}

static NTSTATUS
_MTLDevice_newRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  id<MTLFunction> original_vertex_function = (id<MTLFunction>)info->vertex_function;
  id<MTLFunction> original_fragment_function = (id<MTLFunction>)info->fragment_function;
  const char *vertex_name = original_vertex_function ? [[original_vertex_function name] UTF8String] : "";
  const char *fragment_name = original_fragment_function ? [[original_fragment_function name] UTF8String] : "";
  uint64_t pso_id = WMTComputeRenderPipelineProbeID(info, vertex_name, fragment_name);
  uint64_t fragment_hash = WMTComputeStringProbeHash(fragment_name);
  BOOL magenta_override = NO;
  BOOL causal_target = macrunner_causal_pipeline_is_target(
      pso_id, vertex_name, fragment_name);
  BOOL causal_ready = NO;
  id<MTLRenderPipelineState> causal_psos[4] = {nil, nil, nil, nil};

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_vertex_buffers & (1 << i))
      descriptor.vertexBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  descriptor.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  descriptor.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  descriptor.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  descriptor.maxTessellationFactor = info->max_tessellation_factor;

  descriptor.vertexFunction = original_vertex_function;
  descriptor.fragmentFunction = original_fragment_function;

  uint64_t magenta_target = 0;
  BOOL force_magenta_global = macrunner_force_fragment_magenta_global_enabled();
  BOOL force_magenta_exact = macrunner_force_magenta_fragment_enabled() &&
      macrunner_force_magenta_target_id(&magenta_target) && magenta_target == pso_id;
  if ((force_magenta_global || force_magenta_exact) && original_fragment_function &&
      descriptor.colorAttachments[0].pixelFormat != MTLPixelFormatInvalid) {
    id<MTLFunction> magenta = macrunner_opaque_magenta_fragment_function(
        (id<MTLDevice>)params->device);
    if (magenta) {
      descriptor.fragmentFunction = magenta;
      magenta_override = YES;
    }
  }

  unsigned probe_ordinal = 0;
  if (macrunner_fragment_output_pso_take_slot(&probe_ordinal)) {
    MTLRenderPipelineColorAttachmentDescriptor *rt0 = descriptor.colorAttachments[0];
    fprintf(
        stderr,
        "macrunner-hb-fragment-output: side=winemetal phase=create ordinal=%u "
        "pso_id=0x%016llx vs=%s ps=%s ps_hash=%016llx override_magenta=%u "
        "force_global=%u target_valid=%u target_id=0x%016llx metal_rt0_format=%lu "
        "metal_write_mask=0x%lx metal_blend=%u metal_rgb_op=%lu metal_alpha_op=%lu "
        "metal_src_rgb=%lu metal_dst_rgb=%lu metal_src_alpha=%lu metal_dst_alpha=%lu\n",
        probe_ordinal, (unsigned long long)pso_id, vertex_name, fragment_name,
        (unsigned long long)fragment_hash, magenta_override, force_magenta_global,
        macrunner_force_magenta_target_id(NULL), (unsigned long long)magenta_target,
        (unsigned long)rt0.pixelFormat, (unsigned long)rt0.writeMask,
        rt0.blendingEnabled, (unsigned long)rt0.rgbBlendOperation,
        (unsigned long)rt0.alphaBlendOperation, (unsigned long)rt0.sourceRGBBlendFactor,
        (unsigned long)rt0.destinationRGBBlendFactor,
        (unsigned long)rt0.sourceAlphaBlendFactor,
        (unsigned long)rt0.destinationAlphaBlendFactor);
    fflush(stderr);
  }

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  NSError *err = NULL;
  MTLPipelineOption options =
      !magenta_override && info->fail_on_binary_archive_miss
          ? MTLPipelineOptionFailOnBinaryArchiveMiss
          : MTLPipelineOptionNone;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
  params->ret_error = (obj_handle_t)err;
  if (causal_target && params->ret_pso) {
    id<MTLFunction> c0_fragment =
        (id<MTLFunction>)info->causal_fragment_capture_function;
    id<MTLFunction> c1_fragment =
        (id<MTLFunction>)info->causal_fragment_magenta_function;
    id<MTLFunction> c2_fragment =
        (id<MTLFunction>)info->causal_fragment_input_function;
    id<MTLFunction> c3_vertex =
        (id<MTLFunction>)info->causal_vertex_output_function;
    NSError *causal_errors[4] = {nil, nil, nil, nil};

    if (magenta_override || !c0_fragment || !c1_fragment || !c2_fragment ||
        !c3_vertex) {
      atomic_store_explicit(&macrunner_causal_invalid, true, memory_order_release);
      fprintf(stderr,
              "macrunner-hb-causal-ladder: phase=pso-variants result=invalid "
              "logical_pso=0x%016llx reason=%s c0=%p c1=%p c2=%p c3=%p\n",
              (unsigned long long)pso_id,
              magenta_override ? "original-pso-was-overridden" :
                                 "missing-diagnostic-function",
              c0_fragment, c1_fragment, c2_fragment, c3_vertex);
    } else {
      descriptor.vertexFunction = original_vertex_function;
      descriptor.fragmentFunction = c0_fragment;
      causal_psos[0] = [(id<MTLDevice>)params->device
          newRenderPipelineStateWithDescriptor:descriptor
                                     options:MTLPipelineOptionNone
                                  reflection:nil
                                       error:&causal_errors[0]];

      descriptor.fragmentFunction = c1_fragment;
      causal_psos[1] = [(id<MTLDevice>)params->device
          newRenderPipelineStateWithDescriptor:descriptor
                                     options:MTLPipelineOptionNone
                                  reflection:nil
                                       error:&causal_errors[1]];

      descriptor.fragmentFunction = c2_fragment;
      causal_psos[2] = [(id<MTLDevice>)params->device
          newRenderPipelineStateWithDescriptor:descriptor
                                     options:MTLPipelineOptionNone
                                  reflection:nil
                                       error:&causal_errors[2]];

      descriptor.vertexFunction = c3_vertex;
      descriptor.fragmentFunction = c0_fragment;
      causal_psos[3] = [(id<MTLDevice>)params->device
          newRenderPipelineStateWithDescriptor:descriptor
                                     options:MTLPipelineOptionNone
                                  reflection:nil
                                       error:&causal_errors[3]];
      descriptor.vertexFunction = original_vertex_function;
      descriptor.fragmentFunction = original_fragment_function;
      causal_ready = causal_psos[0] && causal_psos[1] && causal_psos[2] &&
                     causal_psos[3];
      fprintf(
          stderr,
          "macrunner-hb-causal-ladder: phase=pso-variants result=%s "
          "logical_pso=0x%016llx original=%p C1=%p C2=%p C3=%p "
          "vs=%s ps=%s errors=%s|%s|%s|%s\n",
          causal_ready ? "ok" : "invalid", (unsigned long long)pso_id,
          causal_psos[0], causal_psos[1], causal_psos[2], causal_psos[3],
          vertex_name, fragment_name,
          causal_errors[0] ? [[causal_errors[0] description] UTF8String] : "none",
          causal_errors[1] ? [[causal_errors[1] description] UTF8String] : "none",
          causal_errors[2] ? [[causal_errors[2] description] UTF8String] : "none",
          causal_errors[3] ? [[causal_errors[3] description] UTF8String] : "none");
      if (!causal_ready) {
        atomic_store_explicit(&macrunner_causal_invalid, true,
                              memory_order_release);
        for (unsigned i = 0; i < 4; i++) {
          [causal_psos[i] release];
          causal_psos[i] = nil;
        }
      }
    }
    fflush(stderr);
  }
  if (params->ret_pso &&
      (macrunner_gpu_readback_probe_enabled() ||
       macrunner_fragment_output_probe_enabled() ||
       macrunner_force_magenta_fragment_enabled() ||
       macrunner_force_fragment_magenta_global_enabled() || causal_target ||
       macrunner_vertex_data_probe_enabled())) {
    MacRunnerPipelineProbeInfo *probe = [[MacRunnerPipelineProbeInfo alloc] init];
    probe->vertex_function = info->vertex_function;
    probe->fragment_function = info->fragment_function;
    probe->pso_id = pso_id;
    probe->fragment_hash = fragment_hash;
    snprintf(probe->vertex_name, sizeof(probe->vertex_name), "%s", vertex_name);
    snprintf(probe->fragment_name, sizeof(probe->fragment_name), "%s", fragment_name);
    MTLRenderPipelineColorAttachmentDescriptor *rt0 = descriptor.colorAttachments[0];
    probe->metal_rt0_format = rt0.pixelFormat;
    probe->metal_write_mask = rt0.writeMask;
    probe->metal_blend_enabled = rt0.blendingEnabled;
    probe->metal_rgb_op = rt0.rgbBlendOperation;
    probe->metal_alpha_op = rt0.alphaBlendOperation;
    probe->metal_src_rgb = rt0.sourceRGBBlendFactor;
    probe->metal_dst_rgb = rt0.destinationRGBBlendFactor;
    probe->metal_src_alpha = rt0.sourceAlphaBlendFactor;
    probe->metal_dst_alpha = rt0.destinationAlphaBlendFactor;
    probe->metal_depth_format = descriptor.depthAttachmentPixelFormat;
    probe->metal_stencil_format = descriptor.stencilAttachmentPixelFormat;
    probe->metal_sample_count = descriptor.rasterSampleCount;
    probe->metal_input_topology = descriptor.inputPrimitiveTopology;
    probe->metal_alpha_to_coverage = descriptor.alphaToCoverageEnabled;
    probe->magenta_override = magenta_override;
    probe->causal_ready = causal_ready;
    if (causal_ready)
      for (unsigned i = 0; i < 4; i++)
        probe->causal_pso[i] = (obj_handle_t)causal_psos[i];
    objc_setAssociatedObject((id)params->ret_pso, &macrunner_pipeline_probe_info_key, probe,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [probe release];
  }
  if (!err && !magenta_override && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                                                     error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newMeshRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *params = obj;
  const struct WMTMeshRenderPipelineInfo *info = params->info.ptr;
  MTLMeshRenderPipelineDescriptor *descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_mesh_buffers & (1 << i))
      descriptor.meshBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_object_buffers & (1 << i))
      descriptor.objectBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef DXMT_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;

  descriptor.objectFunction = (id<MTLFunction>)info->object_function;
  descriptor.meshFunction = (id<MTLFunction>)info->mesh_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;
  descriptor.payloadMemoryLength = info->payload_memory_length;

  descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
  descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithMeshDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)err;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (!err && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addMeshRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBlitCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLBlitCommandEncoder> encoder = (id<MTLBlitCommandEncoder>)params->encoder;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTextureWithBlitOption: {
      struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *body =
          (struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                      options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResolveCounters: {
      struct wmtcmd_blit_resolvecounters *body = (struct wmtcmd_blit_resolvecounters *)next;
      [encoder resolveCounters:(id<MTLCounterSampleBuffer>)body->sample_buffer
                       inRange:NSMakeRange(body->start, body->len)
             destinationBuffer:(id<MTLBuffer>)body->dst_buffer
             destinationOffset:body->dst_offset];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputeCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLComputeCommandEncoder> encoder = (id<MTLComputeCommandEncoder>)params->encoder;
  MTLSize threadgroup_size = {0, 0, 0};
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      assert(!next->type && "unhandled compute command type");
      break;
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      [encoder dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                                threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      threadgroup_size.width = body->threadgroup_size.width;
      threadgroup_size.height = body->threadgroup_size.height;
      threadgroup_size.depth = body->threadgroup_size.depth;
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      [encoder setBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      [encoder setBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandUseResource: {
      struct wmtcmd_compute_useresource *body = (struct wmtcmd_compute_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource usage:(MTLResourceUsage)body->usage];
      break;
    }
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      [encoder setBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      [encoder setTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      struct wmtcmd_compute_memory_barrier *body = (struct wmtcmd_compute_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLRenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)params->encoder;
  BOOL has_pso = winemetal_render_encoder_has_pso(encoder);
  MacRunnerRenderProbeState *probe_state = macrunner_render_probe_state_enabled()
      ? objc_getAssociatedObject(encoder, &macrunner_render_probe_state_key) : nil;
  MacRunnerPresentAttachmentTrace *attachment_trace =
      macrunner_present_attachment_trace_enabled()
          ? objc_getAssociatedObject(encoder, &macrunner_present_attachment_trace_key) : nil;
  BOOL reported_missing_pso = NO;
  unsigned command_count = 0;
  unsigned set_pso_count = 0;
  unsigned draw_count = 0;
  unsigned missing_pso_count = 0;
  const void *cmd_head = next;
  macrunner_render_pipeline_probe_log("batch-begin", encoder, cmd_head, 0,
                                      NULL, 0, 0, 0, 0);
  while (next) {
    command_count++;
    switch ((enum WMTRenderCommandType)next->type) {
    default:
      assert(!next->type && "unhandled render command type");
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      struct wmtcmd_render_useresource *body = (struct wmtcmd_render_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource
                     usage:(MTLResourceUsage)body->usage
                    stages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setVertexBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      if (probe_state && body->index < 31) {
        probe_state->vertex_buffers[body->index] = body->buffer;
        probe_state->vertex_buffer_offsets[body->index] = body->offset;
      }
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setVertexBufferOffset:body->offset atIndex:body->index];
      if (probe_state && body->index < 31)
        probe_state->vertex_buffer_offsets[body->index] = body->offset;
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setFragmentBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      if (probe_state && body->index < 31) {
        probe_state->fragment_buffers[body->index] = body->buffer;
        probe_state->fragment_buffer_offsets[body->index] = body->offset;
      }
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setFragmentBufferOffset:body->offset atIndex:body->index];
      if (probe_state && body->index < 31)
        probe_state->fragment_buffer_offsets[body->index] = body->offset;
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setMeshBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setMeshBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setObjectBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      [encoder setFragmentBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      [encoder setFragmentTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      if (probe_state && body->index < 32)
        probe_state->fragment_textures[body->index] = body->texture;
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
      [encoder setCullMode:(MTLCullMode)body->cull_mode];
      [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
      [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      if (probe_state) {
        probe_state->fill_mode = body->fill_mode;
        probe_state->cull_mode = body->cull_mode;
        probe_state->depth_clip_mode = body->depth_clip_mode;
      }
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      if (probe_state && body->viewport_count && body->viewports.ptr) {
        probe_state->viewport = *(const struct WMTViewport *)body->viewports.ptr;
        probe_state->viewport_valid = YES;
      }
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      if (probe_state && body->rect_count && body->scissor_rects.ptr) {
        probe_state->scissor = *(const struct WMTScissorRect *)body->scissor_rects.ptr;
        probe_state->scissor_valid = YES;
      }
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      has_pso = YES;
      winemetal_render_encoder_set_has_pso(encoder, YES);
      if (probe_state)
        probe_state->pso = body->pso;
      set_pso_count++;
      macrunner_render_pipeline_probe_log(
          "set-pso", encoder, cmd_head, next->type,
          (const void *)(uintptr_t)body->pso, command_count,
          set_pso_count, draw_count, missing_pso_count);
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilReferenceValue:body->stencil_ref];
      if (probe_state) {
        probe_state->depth_stencil_state = body->dsso;
        probe_state->stencil_ref = body->stencil_ref;
      }
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      [encoder setStencilReferenceValue:body->stencil_ref];
      if (probe_state) {
        probe_state->blend_color[0] = body->red;
        probe_state->blend_color[1] = body->green;
        probe_state->blend_color[2] = body->blue;
        probe_state->blend_color[3] = body->alpha;
        probe_state->stencil_ref = body->stencil_ref;
      }
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      draw_count++;
      if (attachment_trace)
        attachment_trace->draw_count++;
      if (!has_pso) {
        missing_pso_count++;
        macrunner_render_pipeline_probe_log(
            "draw-missing-pso", encoder, cmd_head, next->type, NULL,
            command_count, set_pso_count, draw_count, missing_pso_count);
        if (!reported_missing_pso) {
          fprintf(stderr, "winemetal[render]: skipping draw commands because no render PSO was set\n");
          reported_missing_pso = YES;
        }
        break;
      }
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      macrunner_shader_inputs_log_draw(probe_state, "draw");
      macrunner_fragment_output_log_draw(probe_state, "draw");
      macrunner_vertex_data_log_draw(
          probe_state, "draw", body->primitive_type, body->vertex_count,
          body->instance_count, body->vertex_start, 0, body->base_instance,
          0, 0, 0);
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      if (probe_state)
        probe_state->draw_count++;
      macrunner_gpu_readback_log_draw(probe_state, encoder, "draw", body->primitive_type,
                                      body->vertex_count, body->instance_count,
                                      body->vertex_start, 0, body->base_instance,
                                      0, 0, 0, 0);
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      draw_count++;
      if (attachment_trace)
        attachment_trace->draw_count++;
      if (!has_pso) {
        missing_pso_count++;
        macrunner_render_pipeline_probe_log(
            "draw-indexed-missing-pso", encoder, cmd_head, next->type, NULL,
            command_count, set_pso_count, draw_count, missing_pso_count);
        if (!reported_missing_pso) {
          fprintf(stderr, "winemetal[render]: skipping indexed draw commands because no render PSO was set\n");
          reported_missing_pso = YES;
        }
        break;
      }
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      macrunner_shader_inputs_log_draw(probe_state, "draw-indexed");
      macrunner_fragment_output_log_draw(probe_state, "draw-indexed");
      macrunner_vertex_data_log_draw(
          probe_state, "draw-indexed", body->primitive_type, body->index_count,
          body->instance_count, 0, body->base_vertex, body->base_instance,
          body->index_buffer, body->index_buffer_offset, body->index_type);
      BOOL causal_sequence_drawn = NO;
      const unsigned causal_sequence_count = macrunner_causal_control_count();
      for (unsigned causal_index = 0; causal_index < causal_sequence_count;
           causal_index++) {
        unsigned requested_phase =
            macrunner_causal_control_phase_at(causal_index);
        unsigned causal_phase = UINT32_MAX;
        id<MTLBuffer> causal_sidechannel = nil;
        id<MTLRenderPipelineState> causal_pso =
            macrunner_causal_prepare_indexed_draw(
                probe_state, encoder, body, requested_phase, &causal_phase,
                &causal_sidechannel);
        if (!causal_pso) {
          if (causal_sequence_drawn) {
            atomic_store_explicit(&macrunner_causal_invalid, true,
                                  memory_order_release);
            fprintf(stderr,
                    "macrunner-hb-causal-ladder: phase=draw-select "
                    "control=%s result=invalid "
                    "reason=sequence-phase-missing sequence_index=%u "
                    "sequence_count=%u same_logical_draw=1\n",
                    macrunner_causal_phase_name(requested_phase),
                    causal_index, causal_sequence_count);
            fflush(stderr);
          }
          break;
        }
        causal_sequence_drawn = YES;
        [encoder setRenderPipelineState:causal_pso];
        [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                            indexCount:body->index_count
                             indexType:(MTLIndexType)body->index_type
                           indexBuffer:(id<MTLBuffer>)body->index_buffer
                     indexBufferOffset:body->index_buffer_offset
                         instanceCount:body->instance_count
                            baseVertex:body->base_vertex
                          baseInstance:body->base_instance];
        [encoder setRenderPipelineState:
            (id<MTLRenderPipelineState>)probe_state->pso];
        [encoder setFragmentBuffer:
            (id<MTLBuffer>)probe_state->fragment_buffers[
                MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX]
                            offset:probe_state->fragment_buffer_offsets[
                                MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX]
                           atIndex:MACRUNNER_CAUSAL_SIDECHANNEL_BUFFER_INDEX];
        macrunner_causal_finish_draw(probe_state, causal_sidechannel,
                                     causal_phase);
      }
      if (!causal_sequence_drawn)
        [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                            indexCount:body->index_count
                             indexType:(MTLIndexType)body->index_type
                           indexBuffer:(id<MTLBuffer>)body->index_buffer
                     indexBufferOffset:body->index_buffer_offset
                         instanceCount:body->instance_count
                            baseVertex:body->base_vertex
                          baseInstance:body->base_instance];
      if (probe_state)
        probe_state->draw_count++;
      macrunner_gpu_readback_log_draw(probe_state, encoder, "draw-indexed", body->primitive_type,
                                      body->index_count, body->instance_count, 0,
                                      body->base_vertex, body->base_instance,
                                      body->index_buffer, body->index_buffer_offset, 0, 0);
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      draw_count++;
      if (attachment_trace)
        attachment_trace->draw_count++;
      if (!has_pso) {
        missing_pso_count++;
        macrunner_render_pipeline_probe_log(
            "draw-indirect-missing-pso", encoder, cmd_head, next->type, NULL,
            command_count, set_pso_count, draw_count, missing_pso_count);
        if (!reported_missing_pso) {
          fprintf(stderr, "winemetal[render]: skipping indirect draw commands because no render PSO was set\n");
          reported_missing_pso = YES;
        }
        break;
      }
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      macrunner_shader_inputs_log_draw(probe_state, "draw-indirect");
      macrunner_fragment_output_log_draw(probe_state, "draw-indirect");
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
          indirectBufferOffset:body->indirect_args_offset];
      if (probe_state)
        probe_state->draw_count++;
      macrunner_gpu_readback_log_draw(probe_state, encoder, "draw-indirect", body->primitive_type,
                                      0, 0, 0, 0, 0, 0, 0,
                                      body->indirect_args_buffer, body->indirect_args_offset);
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      draw_count++;
      if (attachment_trace)
        attachment_trace->draw_count++;
      if (!has_pso) {
        missing_pso_count++;
        macrunner_render_pipeline_probe_log(
            "draw-indexed-indirect-missing-pso", encoder, cmd_head,
            next->type, NULL, command_count, set_pso_count, draw_count,
            missing_pso_count);
        if (!reported_missing_pso) {
          fprintf(stderr, "winemetal[render]: skipping indexed indirect draw commands because no render PSO was set\n");
          reported_missing_pso = YES;
        }
        break;
      }
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      macrunner_shader_inputs_log_draw(probe_state, "draw-indexed-indirect");
      macrunner_fragment_output_log_draw(probe_state, "draw-indexed-indirect");
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                      indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                indirectBufferOffset:body->indirect_args_offset];
      if (probe_state)
        probe_state->draw_count++;
      macrunner_gpu_readback_log_draw(probe_state, encoder, "draw-indexed-indirect",
                                      body->primitive_type, 0, 0, 0, 0, 0,
                                      body->index_buffer, body->index_buffer_offset,
                                      body->indirect_args_buffer, body->indirect_args_offset);
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      if (attachment_trace)
        attachment_trace->draw_count++;
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(
                                                          body->object_threadgroup_size.width,
                                                          body->object_threadgroup_size.height,
                                                          body->object_threadgroup_size.depth
                                                      )
                            threadsPerMeshThreadgroup:MTLSizeMake(
                                                          body->mesh_threadgroup_size.width,
                                                          body->mesh_threadgroup_size.height,
                                                          body->mesh_threadgroup_size.depth
                                                      )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope
                          afterStages:(MTLRenderStages)body->stages_after
                         beforeStages:(MTLRenderStages)body->stages_before];
      break;
    }
    case WMTRenderCommandDXMTGeometryDraw: {
      struct wmtcmd_render_dxmt_geometry_draw *body = (struct wmtcmd_render_dxmt_geometry_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexed: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed *body = (struct wmtcmd_render_dxmt_geometry_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indirect *body = (struct wmtcmd_render_dxmt_geometry_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDraw: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:21];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer offset:body->index_buffer_offset atIndex:20];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer offset:body->indirect_args_offset atIndex:21];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      if (probe_state) {
        probe_state->viewport = body->viewport;
        probe_state->viewport_valid = YES;
      }
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = body->scissor_rect};
      [encoder setScissorRect:u.dst];
      if (probe_state) {
        probe_state->scissor = body->scissor_rect;
        probe_state->scissor_valid = YES;
      }
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      struct wmtcmd_render_dispatch_threads_per_tile *body = (struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [encoder dispatchThreadsPerTile:MTLSizeMake(body->width, body->height, 1)];
      break;
    }
    }
    next = next->next.ptr;
  }
  macrunner_render_pipeline_probe_log(
      "batch-end", encoder, cmd_head, 0, NULL, command_count, set_pso_count,
      draw_count, missing_pso_count);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_pixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle pixelFormat];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_width(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle width];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_height(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle height];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_depth(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle depth];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_arrayLength(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle arrayLength];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_mipmapLevelCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle mipmapLevelCount];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_replaceRegion(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  [(id<MTLTexture>)params->texture replaceRegion:MTLRegionMake3D(
                                                     params->origin.x, params->origin.y, params->origin.z,
                                                     params->size.width, params->size.height, params->size.depth
                                                 )
                                     mipmapLevel:params->level
                                           slice:params->slice
                                       withBytes:params->data.ptr
                                     bytesPerRow:params->bytes_per_row
                                   bytesPerImage:params->bytes_per_image];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_didModifyRange(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  [(id<MTLBuffer>)params->handle didModifyRange:NSMakeRange(params->arg, params->ret)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  id<MTLCommandBuffer> command_buffer = (id<MTLCommandBuffer>)params->handle;
  id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)params->arg;
  id<MTLTexture> drawable_texture = drawable ? drawable.texture : nil;
  macrunner_present_surface_schedule(command_buffer, drawable);
  NSNumber *armed_phase = macrunner_causal_present_surface_readback_enabled() ?
      objc_getAssociatedObject(command_buffer, &macrunner_causal_present_surface_phase_key) : nil;
  if (armed_phase) {
    unsigned phase = armed_phase.unsignedIntValue;
    id<MTLTexture> texture = drawable_texture;
    if (!texture) {
      fprintf(stderr,
              "macrunner-hb-causal-present-surface: phase=schedule result=invalid "
              "control=%s command_buffer=%p drawable=%p reason=null-texture\n",
              macrunner_causal_phase_name(phase), command_buffer, drawable);
    } else if (macrunner_causal_present_surface_apply_control(command_buffer, texture, phase)) {
      fprintf(stderr,
              "macrunner-hb-causal-present-surface: phase=schedule result=ok "
              "control=%s command_buffer=%p drawable=%p texture=%p\n",
              macrunner_causal_phase_name(phase), command_buffer, drawable, texture);
      macrunner_gpu_readback_schedule(
          command_buffer, texture, MacRunnerGPUReadbackCausalPresentC0 + phase, 0);
    }
    objc_setAssociatedObject(command_buffer, &macrunner_causal_present_surface_phase_key,
                             nil, OBJC_ASSOCIATION_ASSIGN);
  }
  [command_buffer presentDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawableAfterMinimumDuration(void *obj) {
  struct unixcall_generic_obj_obj_double_noret *params = obj;
  id<MTLCommandBuffer> command_buffer = (id<MTLCommandBuffer>)params->handle;
  macrunner_present_surface_schedule(command_buffer, (id<CAMetalDrawable>)params->arg0);
  [command_buffer presentDrawable:(id<MTLDrawable>)params->arg0
                                    afterMinimumDuration:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFamily(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsFamily:(MTLGPUFamily)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsBCTextureCompression(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsBCTextureCompression];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsTextureSampleCount(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsTextureSampleCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_hasUnifiedMemory(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle hasUnifiedMemory];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_sharedCaptureManager(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[MTLCaptureManager sharedCaptureManager];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_startCapture(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *params = obj;
  MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
  const struct WMTCaptureInfo *info = params->info.ptr;
  desc.destination = (MTLCaptureDestination)info->destination;
  desc.captureObject = (id)info->capture_object;
  NSString *path_str = [[NSString alloc] initWithCString:info->output_url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  desc.outputURL = url;
  [(MTLCaptureManager *)params->capture_manager startCaptureWithDescriptor:desc error:nil];
  [url release];
  [path_str release];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_stopCapture(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(MTLCaptureManager *)params->handle stopCapture];
  return STATUS_SUCCESS;
}

#include <signal.h>

void
temp_handler(int signum) {
  fprintf(stderr, "received signal %d in temp_handler(), and it may cause problem!\n", signum);
}

static const int SIGNALS[] = {
    SIGHUP,
    SIGINT,
    SIGTERM,
    SIGUSR2,
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGFPE,
    SIGBUS,
    SIGSEGV,
    SIGQUIT
#ifdef SIGSYS
    ,
    SIGSYS
#endif
#ifdef SIGXCPU
    ,
    SIGXCPU
#endif
#ifdef SIGXFSZ
    ,
    SIGXFSZ
#endif
#ifdef SIGEMT
    ,
    SIGEMT
#endif
    ,
    SIGUSR1
#ifdef SIGINFO
    ,
    SIGINFO
#endif
};

static NTSTATUS
_MTLDevice_newTemporalScaler(void *obj) {
  struct unixcall_mtldevice_newfxtemporalscaler *params = obj;
  MTLFXTemporalScalerDescriptor *desc = [[MTLFXTemporalScalerDescriptor alloc] init];
  const struct WMTFXTemporalScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.depthTextureFormat = to_metal_pixel_format(info->depth_format);
  desc.motionTextureFormat = to_metal_pixel_format(info->motion_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  desc.inputContentMaxScale = info->input_content_max_scale;
  desc.inputContentMinScale = info->input_content_min_scale;
  desc.inputContentPropertiesEnabled = info->input_content_properties_enabled;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    desc.requiresSynchronousInitialization = info->requires_synchronous_initialization;
  }
  #endif
  desc.autoExposureEnabled = info->auto_exposure;

  struct sigaction old_action[sizeof(SIGNALS) / sizeof(int)], new_action;
  if (@available(macOS 16, *)) {} else {
    new_action.sa_handler = temp_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &new_action, &old_action[i]);
  }

  params->ret = (obj_handle_t)[desc newTemporalScalerWithDevice:(id<MTLDevice>)params->device];

  if (@available(macOS 16, *)) {} else {
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &old_action[i], NULL);
  }

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSpatialScaler(void *obj) {
  struct unixcall_mtldevice_newfxspatialscaler *params = obj;
  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  const struct WMTFXSpatialScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  params->ret = (obj_handle_t)[desc newSpatialScalerWithDevice:(id<MTLDevice>)params->device];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeTemporalScale(void *obj) {
  struct unixcall_mtlcommandbuffer_temporal_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXTemporalScaler> scaler = (id<MTLFXTemporalScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.depthTexture = (id<MTLTexture>)params->depth;
  scaler.motionTexture = (id<MTLTexture>)params->motion;
  scaler.exposureTexture = (id<MTLTexture>)params->exposure;
  scaler.fence = (id<MTLFence>)params->fence;
  const struct WMTFXTemporalScalerProps *props = params->props.ptr;
  scaler.inputContentWidth = props->input_content_width;
  scaler.inputContentHeight = props->input_content_height;
  scaler.reset = props->reset;
  scaler.depthReversed = props->depth_reversed;
  scaler.motionVectorScaleX = props->motion_vector_scale_x;
  scaler.motionVectorScaleY = props->motion_vector_scale_y;
  scaler.jitterOffsetX = props->jitter_offset_x;
  scaler.jitterOffsetY = props->jitter_offset_y;
  scaler.preExposure = props->pre_exposure;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtlcommandbuffer_spatial_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.fence = (id<MTLFence>)params->fence;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_string(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [NSString stringWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_alloc_init(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [[NSString alloc] initWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_instance(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret =
      (obj_handle_t)((id(*)(id, SEL))objc_msgSend)(objc_lookUpClass("_CADeveloperHUDProperties"), @selector(instance));
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_addLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_uint64_ret *params = obj;
  params->ret = ((bool (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(addLabel:after:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_updateLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(updateLabel:value:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_remove(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id))objc_msgSend)((id)params->handle, @selector(remove:), (id)params->arg);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalDrawable_texture(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<CAMetalDrawable>)params->handle texture];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_nextDrawable(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(CAMetalLayer *)params->handle nextDrawable];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXSpatialScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXTemporalScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXTemporalScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setProps(void *obj) {
  struct unixcall_generic_obj_constptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  const struct WMTLayerProps *props = params->arg.ptr;
  execute_on_main(^{
    layer.device = (id<MTLDevice>)props->device;
    layer.opaque = props->opaque;
    layer.framebufferOnly = props->framebuffer_only;
    layer.contentsScale = props->contents_scale;
    layer.displaySyncEnabled = props->display_sync_enabled;
    layer.drawableSize = CGSizeMake(props->drawable_width, props->drawable_height);
    layer.pixelFormat = to_metal_pixel_format(props->pixel_format);
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_getProps(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTLayerProps *props = params->arg.ptr;
  props->device = (obj_handle_t)layer.device;
  props->opaque = layer.opaque;
  props->framebuffer_only = layer.framebufferOnly;
  props->contents_scale = layer.contentsScale;
  props->display_sync_enabled = layer.displaySyncEnabled;
  props->drawable_height = layer.drawableSize.height;
  props->drawable_width = layer.drawableSize.width;
  props->pixel_format = layer.pixelFormat;
  return STATUS_SUCCESS;
}

typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct macdrv_opaque_window_data *macdrv_window_data;
typedef struct opaque_window_surface *window_surface;
typedef struct opaque_HWND *HWND;
struct macdrv_win_data {
  HWND hwnd; /* hwnd that this private data belongs to */
  macdrv_window cocoa_window;
  macdrv_view cocoa_view;
  macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(BOOL);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};

static const CFSetCallBacks fallback_metal_layer_callbacks = {
    0, NULL, NULL, NULL, NULL, NULL,
};
static CFMutableSetRef fallback_metal_layers;
/* Maps CAMetalLayer* -> NSWindow* for real-window fallback cleanup. */
static CFMutableDictionaryRef fallback_nswindow_map;

static void
register_fallback_metal_layer(CAMetalLayer *layer) {
  if (!fallback_metal_layers)
    fallback_metal_layers = CFSetCreateMutable(NULL, 0, &fallback_metal_layer_callbacks);
  CFSetAddValue(fallback_metal_layers, layer);
}

static CAMetalLayer *
create_fallback_metal_layer(macdrv_metal_device device) {
  __block CAMetalLayer *layer = nil;

  execute_on_main(^{
    layer = [[CAMetalLayer alloc] init];
    layer.device = (id<MTLDevice>)device;
    layer.opaque = YES;
    layer.framebufferOnly = NO;
    layer.contentsScale = 1.0;
    layer.drawableSize = CGSizeMake(1.0, 1.0);
    layer.frame = CGRectMake(0.0, 0.0, 1.0, 1.0);
    register_fallback_metal_layer(layer);
  });

  return layer;
}

/* Create a real, screen-sized NSWindow backed by a CAMetalLayer.
 * Used as a fallback when the winemac HWND path is unavailable. */
static CAMetalLayer *
create_real_nswindow_metal_layer(macdrv_metal_device device, int trace, void *hwnd_trace) {
  __block CAMetalLayer *layer = nil;

  execute_on_main(^{
    NSScreen *screen = [NSScreen mainScreen];
    CGRect frame = screen ? NSRectToCGRect([screen frame]) : CGRectMake(0, 0, 1280, 720);
    CGFloat scale = screen ? [screen backingScaleFactor] : 1.0;

    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSRectFromCGRect(frame)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [window setOpaque:YES];
    [window setLevel:NSNormalWindowLevel];

    layer = [[CAMetalLayer alloc] init];
    layer.device = (id<MTLDevice>)device;
    layer.opaque = YES;
    layer.framebufferOnly = NO;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(frame.size.width * scale, frame.size.height * scale);

    /* Attach the layer to the window's content view so it renders on screen. */
    [[window contentView] setWantsLayer:YES];
    [[window contentView] setLayer:layer];
    [window makeKeyAndOrderFront:nil];

    /* Register in fallback set so _ReleaseMetalView handles cleanup. */
    register_fallback_metal_layer(layer);

    /* Store layer->window mapping so we can close the window on release.
     * Manually retain the window (no ARC); released in _ReleaseMetalView. */
    if (!fallback_nswindow_map)
      fallback_nswindow_map = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    [window retain];
    CFDictionarySetValue(fallback_nswindow_map, (const void *)layer, (const void *)window);

    if (trace)
      fprintf(stderr, "winemetal[HWND]: hwnd=%p real-NSWindow fallback "
              "window=%p layer=%p frame=%.0fx%.0f scale=%.1f\n",
              hwnd_trace, (void *)window, (void *)layer,
              frame.size.width, frame.size.height, scale);
  });

  return layer;
}

static NTSTATUS
_CreateMetalViewFromHWND(void *obj) {
  struct unixcall_create_metal_view_from_hwnd *params = obj;
  const int trace = getenv("WINEMETAL_TRACE_HWND") != NULL;

  struct macdrv_win_data *(*pfn_get_win_data)(HWND hwnd) = NULL;
  void (*pfn_release_win_data)(struct macdrv_win_data *data) = NULL;
  macdrv_metal_view (*pfn_macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d) = NULL;
  macdrv_metal_layer (*pfn_macdrv_view_get_metal_layer)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_get_win_data = macdrv_functions->get_win_data;
    pfn_release_win_data = macdrv_functions->release_win_data;
    pfn_macdrv_view_create_metal_view = macdrv_functions->macdrv_view_create_metal_view;
    pfn_macdrv_view_get_metal_layer = macdrv_functions->macdrv_view_get_metal_layer;
  } else {
    pfn_get_win_data = dlsym(RTLD_DEFAULT, "get_win_data");
    pfn_release_win_data = dlsym(RTLD_DEFAULT, "release_win_data");
    pfn_macdrv_view_create_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_create_metal_view");
    pfn_macdrv_view_get_metal_layer = dlsym(RTLD_DEFAULT, "macdrv_view_get_metal_layer");
  }

  if (pfn_get_win_data && pfn_release_win_data && pfn_macdrv_view_create_metal_view &&
      pfn_macdrv_view_get_metal_layer) {
    struct macdrv_win_data *win_data = pfn_get_win_data((HWND)params->hwnd);
    macdrv_view client_cocoa_view = win_data ? win_data->client_cocoa_view : NULL;
    if (win_data && win_data->client_cocoa_view) {
      macdrv_metal_view view =
          pfn_macdrv_view_create_metal_view(win_data->client_cocoa_view, (macdrv_metal_device)params->device);
      params->ret_view = (obj_handle_t)view;
      if (view)
        params->ret_layer = (obj_handle_t)pfn_macdrv_view_get_metal_layer(view);
    }
    if (trace || !params->ret_view || !params->ret_layer)
      fprintf(stderr, "winemetal[HWND]: hwnd=%p macdrv_functions=%p get_win_data=%p release_win_data=%p "
              "create_metal_view=%p get_metal_layer=%p win_data=%p client_cocoa_view=%p "
              "ret_view=%p ret_layer=%p attached_to_hwnd=%d\n",
              (void *)params->hwnd, (void *)macdrv_functions, (void *)pfn_get_win_data,
              (void *)pfn_release_win_data, (void *)pfn_macdrv_view_create_metal_view,
              (void *)pfn_macdrv_view_get_metal_layer, (void *)win_data, (void *)client_cocoa_view,
              (void *)params->ret_view, (void *)params->ret_layer,
              (int)(win_data && win_data->hwnd == (HWND)params->hwnd && client_cocoa_view &&
                    params->ret_view && params->ret_layer));
    if (win_data)
      pfn_release_win_data(win_data);
    if (params->ret_view && params->ret_layer)
      return STATUS_SUCCESS;
  } else if (trace) {
    fprintf(stderr, "winemetal[HWND]: hwnd=%p macdrv symbols not found"
            " (macdrv_functions=%p get_win_data=%p)\n",
            (void *)params->hwnd, (void *)macdrv_functions, (void *)pfn_get_win_data);
  }

  /* macdrv path failed.
   * DXMT_HEADLESS=1: use 1x1 detached fallback for headless tests.
   * DXMT_ALLOW_ORPHAN_WINDOW=1: explicitly allow the diagnostic orphan NSWindow.
   * Otherwise abort loudly: a live HWND path must attach CAMetalLayer to the game
   * window, not silently present to a detached fallback. */
  CAMetalLayer *fallback_layer;
  if (getenv("DXMT_HEADLESS")) {
    fallback_layer = create_fallback_metal_layer((macdrv_metal_device)params->device);
    /* This is a 1x1 detached CAMetalLayer with no on-screen surface: every
     * Present renders into a 1x1 offscreen drawable that is never displayed.
     * That is acceptable ONLY for an explicit headless test (DXMT_HEADLESS=1);
     * for a real game window it silently swallows every frame.  Warn
     * UNCONDITIONALLY (not gated on WINEMETAL_TRACE_HWND) so this degenerate
     * path can never drop frames silently -- e.g. a HK gate run that left
     * DXMT_HEADLESS=1 set now logs this line instead of producing a mystery
     * black window. */
    fprintf(stderr, "winemetal[HWND]: hwnd=%p WARNING: DXMT_HEADLESS=1 -> degenerate 1x1 "
            "detached fallback layer=%p; frames render offscreen and are NOT displayed\n",
            (void *)params->hwnd, (void *)fallback_layer);
    fflush(stderr);
  } else if (getenv("DXMT_ALLOW_ORPHAN_WINDOW")) {
    fprintf(stderr, "winemetal[HWND]: hwnd=%p DXMT_ALLOW_ORPHAN_WINDOW=1; "
            "creating diagnostic orphan NSWindow because macdrv HWND binding failed\n",
            (void *)params->hwnd);
    fallback_layer = create_real_nswindow_metal_layer(
        (macdrv_metal_device)params->device, trace, (void *)params->hwnd);
  } else {
    fprintf(stderr, "winemetal[HWND]: fatal: hwnd=%p macdrv HWND->CAMetalLayer binding failed; "
            "refusing silent orphan NSWindow fallback. Set DXMT_HEADLESS=1 for headless tests "
            "or DXMT_ALLOW_ORPHAN_WINDOW=1 for explicit diagnostic fallback.\n",
            (void *)params->hwnd);
    fflush(stderr);
    abort();
  }
  if (fallback_layer) {
    params->ret_view = (obj_handle_t)fallback_layer;
    params->ret_layer = (obj_handle_t)fallback_layer;
    return STATUS_SUCCESS;
  }

  /* Fallback layer allocation failed.  Do NOT fall through to STATUS_SUCCESS:
   * that hands the caller success with ret_view/ret_layer = 0, and the swapchain
   * is then built around a null CAMetalLayer whose nextDrawable() returns nil so
   * every Present silently drops its frame.  Fail loudly instead, mirroring the
   * macdrv guard above (only return SUCCESS when a usable layer exists). */
  fprintf(stderr, "winemetal[HWND]: hwnd=%p fatal: fallback CAMetalLayer allocation failed; "
          "returning STATUS_UNSUCCESSFUL instead of success-with-null\n",
          (void *)params->hwnd);
  fflush(stderr);
  return STATUS_UNSUCCESSFUL;
}

static NTSTATUS
_ReleaseMetalView(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  __block BOOL released_fallback = NO;

  void (*pfn_macdrv_view_release_metal_view)(macdrv_metal_view v) = NULL;

  execute_on_main(^{
    if (fallback_metal_layers &&
        CFSetContainsValue(fallback_metal_layers, (const void *)params->handle)) {
      /* Close the backing NSWindow if this was a real-window fallback. */
      if (fallback_nswindow_map) {
        const void *key = (const void *)(uintptr_t)params->handle;
        NSWindow *window = (NSWindow *)CFDictionaryGetValue(fallback_nswindow_map, key);
        if (window) {
          [window close];
          [window release];  /* balance [window retain] done at create time */
          CFDictionaryRemoveValue(fallback_nswindow_map, key);
        }
      }
      CFSetRemoveValue(fallback_metal_layers, (const void *)params->handle);
      [(CAMetalLayer *)params->handle release];
      released_fallback = YES;
    }
  });
  if (released_fallback)
    return STATUS_SUCCESS;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_macdrv_view_release_metal_view = macdrv_functions->macdrv_view_release_metal_view;
  } else {
    pfn_macdrv_view_release_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_release_metal_view");
  }

  if (pfn_macdrv_view_release_metal_view)
    pfn_macdrv_view_release_metal_view((macdrv_metal_view)params->handle);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Initialize(void *args) {
  struct sm50_initialize_params *params = args;

  params->ret =
      SM50Initialize(params->bytecode, params->bytecode_size, params->shader, params->reflection, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Destroy(void *args) {
  struct sm50_destroy_params *params = args;

  SM50Destroy(params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Compile(void *args) {
  struct sm50_compile_params *params = args;

  params->ret = SM50Compile(params->shader, params->args, params->func_name, params->bitcode, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params *params = args;

  SM50GetCompiledBitcode(params->bitcode, params->data_out);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50DestroyBitcode(void *args) {
  struct sm50_destroy_bitcode_params *params = args;

  SM50DestroyBitcode(params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, params->buffer, params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50FreeError(void *args) {
  struct sm50_free_error_params *params = args;

  SM50FreeError(params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params *params = args;

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, params->hull_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params *params = args;

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, params->domain_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params *params = args;

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, params->vertex_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params *params = args;

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, params->geometry_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_setLabel(void *args) {
  struct unixcall_generic_obj_obj_noret *params = args;
  [(id<MTLCommandEncoder>)params->handle setLabel:(NSString *)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_setShouldMaximizeConcurrentCompilation(void *args) {
  struct unixcall_generic_obj_uint64_noret *params = args;
  [(id<MTLDevice>)params->handle setShouldMaximizeConcurrentCompilation:(BOOL)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params *params = args;
  SM50GetArgumentsInfo(params->shader, params->constant_buffers, params->arguments);
  return STATUS_SUCCESS;
}

static inline void *
UInt32ToPtr(uint32_t v) {
  return (void *)(uint64_t)v;
}

#ifndef DXMT_NATIVE

static NTSTATUS
thunk32_SM50Initialize(void *args) {
  struct sm50_initialize_params32 *params = args;

  params->ret = SM50Initialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader),
      UInt32ToPtr(params->reflection), UInt32ToPtr(params->error)
  );

  return STATUS_SUCCESS;
}

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  uint32_t elements;
};

struct SM50_SHADER_COMMON_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flag;
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

void
sm50_compilation_argument32_convert(
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32
) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;

  first_arg->type = SM50_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  while (args32) {
    switch (args32->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *data =
          malloc(sizeof(struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->num_output_slots = src->num_output_slots;
      data->num_elements = src->num_elements;
      data->strides[0] = src->strides[0];
      data->strides[1] = src->strides[1];
      data->strides[2] = src->strides[2];
      data->strides[3] = src->strides[3];
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_COMMON: {
      struct SM50_SHADER_COMMON_DATA32 *src = (void *)args32;
      struct SM50_SHADER_COMMON_DATA *data = malloc(sizeof(struct SM50_SHADER_COMMON_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->metal_version = src->metal_version;
      data->flags = src->flag;
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->unorm_output_reg_mask = src->unorm_output_reg_mask;
      data->disable_depth_output = src->disable_depth_output;
      data->sample_mask = src->sample_mask;
      data->dual_source_blending = src->dual_source_blending;
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *data = malloc(sizeof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->slot_mask = src->slot_mask;
      data->index_buffer_format = src->index_buffer_format;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      struct SM50_SHADER_GS_PASS_THROUGH_DATA32 *src = (void *)args32;
      struct SM50_SHADER_GS_PASS_THROUGH_DATA *data = malloc(sizeof(struct SM50_SHADER_GS_PASS_THROUGH_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->Data = src->Data;
      data->RasterizationDisabled = src->RasterizationDisabled;
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->strip_topology = src->strip_topology;
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      struct SM50_SHADER_PSO_TESSELLATOR_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_TESSELLATOR_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_TESSELLATOR_DATA));
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->max_potential_tess_factor = src->max_potential_tess_factor;
      break;
    }
    case SM50_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
    args32 = UInt32ToPtr(args32->next);
  }
}

void
sm50_compilation_argument32_free(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = first_arg->next;

  while (arg) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    free(arg);
    arg = next;
  }
}

static NTSTATUS
thunk32_SM50Compile(void *args) {
  struct sm50_compile_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50Compile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params32 *params = args;

  SM50GetCompiledBitcode(params->bitcode, UInt32ToPtr(params->data_out));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params32 *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, UInt32ToPtr(params->buffer), params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->hull_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->domain_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->vertex_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->geometry_args);
  sm50_compilation_argument32_convert(&first_arg, args32);

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params32 *params = args;

  SM50GetArgumentsInfo(params->shader, UInt32ToPtr(params->constant_buffers), UInt32ToPtr(params->arguments));

  return STATUS_SUCCESS;
}
#endif /* DXMT_NATIVE */

static NTSTATUS
_MTLCommandBuffer_scheduleFrameDump(void *obj) {
  struct unixcall_mtlcommandbuffer_frame_dump *params = obj;
  macrunner_frame_dump_schedule((id<MTLCommandBuffer>)params->cmdbuf,
                                (id<MTLTexture>)params->texture, params->frame);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_error(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle error];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_logs(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle logs];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLogContainer_enumerate(void *obj) {
  struct unixcall_enumerate *params = obj;
  uint64_t count = 0;
  uint64_t read = 0;
  id *buffer = params->buffer.ptr;
  for (id _ in (id<MTLLogContainer>)params->enumerable) {
    if (count >= params->start) {
      if (count < params->start + params->buffer_size) {
        buffer[count - params->start] = _;
        read++;
      } else {
        break;
      }
    }
    count++;
  }
  params->ret_read = read;
  return STATUS_SUCCESS;
}

CFStringRef
GetColorSpaceName(enum WMTColorSpace colorspace) {
  switch (colorspace) {
  case WMTColorSpaceSRGB:
    return kCGColorSpaceSRGB;
  case WMTColorSpaceSRGBLinear:
  case WMTColorSpaceHDR_scRGB:
    return kCGColorSpaceExtendedLinearSRGB;
  case WMTColorSpaceBT2020:
    return kCGColorSpaceITUR_2020_sRGBGamma;
  case WMTColorSpaceHDR_PQ:
    return kCGColorSpaceITUR_2100_PQ;
  default:
    return nil;
  }
}

static NTSTATUS
_CGColorSpace_checkColorSpaceSupported(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = false;
  CFStringRef name = GetColorSpaceName((enum WMTColorSpace)params->handle);
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  CGColorSpaceRelease(ref);
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setColorSpace(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  enum WMTColorSpace colorspace = params->arg;
  CFStringRef name = GetColorSpaceName(colorspace);
  params->ret = false;
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  execute_on_main(^{
    layer.colorspace = ref;
    layer.wantsExtendedDynamicRangeContent = WMT_COLORSPACE_IS_HDR(colorspace);
    CGColorSpaceRelease(ref);
  });
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetPrimaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = CGMainDisplayID();
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetSecondaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = kCGNullDirectDisplay;

  uint32_t count = 0;
  CGGetOnlineDisplayList(0, NULL, &count);

  if (count == 0)
    return STATUS_SUCCESS;

  CGDirectDisplayID main_display = CGMainDisplayID();
  CGDirectDisplayID displays[count];
  CGGetOnlineDisplayList(count, displays, &count);

  for (uint32_t i = 0; i < count; i++) {
    CGDirectDisplayID id = displays[i];
    if (id == main_display)
      continue;
    if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
      continue;
    params->ret = id;
    break;
  }

  return STATUS_SUCCESS;
}

typedef struct icc_XYZ_t {
  uint32_t sig;      // 0x205a5958
  uint32_t reserved; // 0
  int32_t x;
  int32_t y;
  int32_t z;
} icc_XYZ_t;

bool
GetChromaticity_xy(ColorSyncProfileRef profile, CFStringRef tag, float *out_x, float *out_y) {
  CFDataRef tag_data = ColorSyncProfileCopyTag(profile, tag);
  if (!tag_data)
    return false;
  if (CFDataGetLength(tag_data) != sizeof(icc_XYZ_t))
    return false;
  icc_XYZ_t *data = (icc_XYZ_t *)CFDataGetBytePtr(tag_data);
  if (data->sig != 0x205a5958)
    return false;
  double X = (int32_t)__builtin_bswap32(data->x) / 65536.0;
  double Y = (int32_t)__builtin_bswap32(data->y) / 65536.0;
  double Z = (int32_t)__builtin_bswap32(data->z) / 65536.0;
  *out_x = X / (X + Y + Z);
  *out_y = Y / (X + Y + Z);
  return true;
}

bool
GetDisplayColorGamut(ColorSyncProfileRef profile, struct WMTDisplayDescription *desc_out) {
  return GetChromaticity_xy(
             profile, kColorSyncSigMediaWhitePointTag, &desc_out->white_points[0], &desc_out->white_points[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigRedColorantTag, &desc_out->red_primaries[0], &desc_out->red_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigGreenColorantTag, &desc_out->green_primaries[0], &desc_out->green_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigBlueColorantTag, &desc_out->blue_primaries[0], &desc_out->blue_primaries[1]
         );
}

NSScreen *
GetNSScreenForDisplayID(CGDirectDisplayID display_id) {
  for (NSScreen *screen in [NSScreen screens]) {
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    if (id == display_id) {
      return screen;
    }
  }
  return nil;
}

static NTSTATUS
_WMTGetDisplayDescription(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CGDirectDisplayID display_id = params->handle;
  struct WMTDisplayDescription *desc_out = params->arg.ptr;
  ColorSyncProfileRef profile = ColorSyncProfileCreateWithDisplayID(display_id);
  if (!profile || !GetDisplayColorGamut(profile, desc_out))
    GetDisplayColorGamut(ColorSyncProfileCreateWithName(kColorSyncGenericRGBProfile), desc_out);
  NSScreen *screen = GetNSScreenForDisplayID(display_id);
  if (screen) {
    desc_out->maximum_edr_color_component_value = [screen maximumExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_reference_edr_color_component_value =
        [screen maximumReferenceExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_potential_edr_color_component_value =
        [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
  } else {
    desc_out->maximum_edr_color_component_value = 1.0;
    desc_out->maximum_reference_edr_color_component_value = 0.0;
    desc_out->maximum_potential_edr_color_component_value = 1.0;
  }
  return STATUS_SUCCESS;
}

struct DisplaySetting {
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTHDRMetadata hdr_metadata;
};

struct DisplaySetting g_display_settings[2] = {{0, 0, {}}, {0, 0, {}}};

static NTSTATUS
_MetalLayer_getEDRValue(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTEDRValue *value = params->arg.ptr;
  value->maximum_edr_color_component_value = 1.0;
  value->maximum_potential_edr_color_component_value = 1.0;

  if (![layer.delegate isKindOfClass:NSView.class])
    return STATUS_SUCCESS;

  NSView *view = (NSView *)layer.delegate;
  if (!view.window)
    return STATUS_SUCCESS;

  if (!view.window.screen)
    return STATUS_SUCCESS;

  NSScreen *screen = view.window.screen;

  value->maximum_edr_color_component_value =
      layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
  value->maximum_potential_edr_color_component_value = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunctionWithConstants(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->library;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->name.ptr encoding:NSUTF8StringEncoding];
  struct WMTFunctionConstant *constants = (struct WMTFunctionConstant *)params->constants.ptr;
  NSError *err = NULL;
  MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
  for (uint64_t i = 0; i < params->num_constants; i++)
    [values setConstantValue:constants[i].data.ptr type:(MTLDataType)constants[i].type atIndex:constants[i].index];

  params->ret = (obj_handle_t)[library newFunctionWithName:name constantValues:values error:&err];
  params->ret_error = (obj_handle_t)err;
  [name release];
  [values release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySetting(void *obj) {
  struct unixcall_query_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  params->ret = false;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (setting->version) {
    *value = setting->hdr_metadata;
    params->colorspace = setting->colorspace;
    params->ret = true;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTUpdateDisplaySetting(void *obj) {
  struct unixcall_update_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  const struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (value) {
    setting->hdr_metadata = *value;
    setting->colorspace = params->colorspace;
    setting->version++;
  } else {
    setting->version = 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySettingForLayer(void *obj) {
  struct unixcall_query_display_setting_for_layer *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->layer;
  struct WMTHDRMetadata *hdr_metadata_out = params->hdr_metadata.ptr;

  params->version = 0;
  if (![layer.delegate isKindOfClass:NSView.class])
    return STATUS_SUCCESS;

  NSView *view = (NSView *)layer.delegate;
  if (!view.window)
    return STATUS_SUCCESS;

  if (!view.window.screen)
    return STATUS_SUCCESS;

  NSScreen *screen = view.window.screen;
  CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];

  struct DisplaySetting *setting = &g_display_settings[id == CGMainDisplayID()];
  *hdr_metadata_out = setting->hdr_metadata;
  params->version = setting->version;
  params->colorspace = setting->colorspace;
  params->edr_value.maximum_edr_color_component_value =
      layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
  params->edr_value.maximum_potential_edr_color_component_value =
      screen.maximumPotentialExtendedDynamicRangeColorComponentValue;

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeWaitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeWaitForEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signalValue(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTLSharedEvent>)params->handle setSignaledValue:params->arg];
  return STATUS_SUCCESS;
}

#ifndef DXMT_NATIVE

typedef struct {
  _Atomic(CFRunLoopRef) runloop_ref;
  MTLSharedEventListener *shared_listener;
} *shared_event_listener_t;

extern NTSTATUS NtSetEvent(void *handle, void *prev_state);

static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  struct unixcall_mtlsharedevent_setevent *params = obj;
  void *nt_event_handle = (shared_event_listener_t)params->event_handle;
  shared_event_listener_t q = (shared_event_listener_t)params->shared_event_listener;
  [(id<MTLSharedEvent>)params->shared_event
      notifyListener:q->shared_listener
             atValue:params->value
               block:^(id<MTLSharedEvent> _e, uint64_t _v) {
                 // NOTE: must ensure no more notification comes after listener been destroyed.
                 while (!atomic_load_explicit(&q->runloop_ref, memory_order_acquire)) {
#if defined(__x86_64__)
                   _mm_pause();
#elif defined(__aarch64__)
          __asm__ __volatile__("yield");
#endif
                 }
                 CFRunLoopPerformBlock(q->runloop_ref, kCFRunLoopCommonModes, ^{
                   NtSetEvent(nt_event_handle, NULL);
                 });
                 CFRunLoopWakeUp(q->runloop_ref);
               }];
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  CFRunLoopRef uninited = NULL;
  if (q && atomic_compare_exchange_strong(&q->runloop_ref, &uninited, CFRunLoopGetCurrent())) {
    /* Add a dummy source so the runloop stays running */
    CFRunLoopSourceContext source_context = {0};
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, &source_context);
    CFRunLoopAddSource(q->runloop_ref, source, kCFRunLoopCommonModes);
    CFRunLoopRun();
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  shared_event_listener_t q = malloc(sizeof(*q));
  if (q) {
    q->runloop_ref = NULL;
    q->shared_listener = [[MTLSharedEventListener alloc] init];
  }
  params->ret = (obj_handle_t)q;
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  shared_event_listener_t q = (shared_event_listener_t)params->handle;
  if (q && q->runloop_ref) {
    CFRunLoopStop(q->runloop_ref);
    q->runloop_ref = NULL;
    [q->shared_listener release];
    q->shared_listener = nil;
    free(q);
  }
  return STATUS_SUCCESS;
}

#else
static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  // nop
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  return STATUS_SUCCESS;
}

#endif

static NTSTATUS
_MTLDevice_newFence(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newFence];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_updateContents(void *obj) {
  struct unixcall_mtlbuffer_updatecontents *params = obj;
  memcpy((void *)((char *)[(id<MTLBuffer>)params->buffer contents] + params->offset), params->data.ptr, params->length);
  if ([(id<MTLBuffer>)params->buffer storageMode] == MTLStorageModeManaged)
    [(id<MTLBuffer>)params->buffer didModifyRange:NSMakeRange(params->offset, params->length)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetOSVersion(void *obj) {
  struct unixcall_get_os_version *params = obj;
  NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
  params->ret_major = version.majorVersion;
  params->ret_minor = version.minorVersion;
  params->ret_patch = version.patchVersion;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBinaryArchive(void *obj) {
  struct unixcall_mtldevice_newbinaryarchive *params = obj;
  NSString *path_str = NULL;
  NSURL *url = NULL;
  MTLBinaryArchiveDescriptor *desc = [[MTLBinaryArchiveDescriptor alloc] init];
  if (params->url.ptr != NULL) {
    path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
    url = [[NSURL alloc] initFileURLWithPath:path_str];
    desc.url = url;
  }
  NSError *err = NULL;
  params->ret_archive = (obj_handle_t)[(id<MTLDevice>)params->device newBinaryArchiveWithDescriptor:desc error:&err];
  params->ret_error = (obj_handle_t)err;
  [desc release];
  if (url)
    [url release];
  if (path_str)
    [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBinaryArchive_serialize(void *obj) {
  struct unixcall_mtlbinaryarchive_serialize *params = obj;
  NSString *path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  NSError *err = NULL;
  [(id<MTLBinaryArchive>)params->archive serializeToURL:url error:&err];
  params->ret_error = (obj_handle_t)err;
  [url release];
  [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_alloc_init(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)dispatch_data_create((void *)params->handle, params->arg, NULL, NULL);
  return STATUS_SUCCESS;
}

@interface MTLSharedTextureHandle ()

- (MTLSharedTextureHandle *)initWithMachPort:(mach_port_t)port;
- (mach_port_t)createMachPort;

@end

static NTSTATUS
_MTLDevice_newSharedTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;

  if (info->mach_port) {
    MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:info->mach_port];
    id<MTLTexture> ret = [device newSharedTextureWithHandle:handle];
    extract_texture_descriptor(ret, info);
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    [handle release];
  } else {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    id<MTLTexture> ret = [device newSharedTextureWithDescriptor:desc];
    MTLSharedTextureHandle *handle = [ret newSharedTextureHandle];
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    info->mach_port = [handle createMachPort]; // implicitly add ref to underlying IOSurface
    [handle release];
    [desc release];
  }

  return STATUS_SUCCESS;
}

/* Private API to register a mach port with the bootstrap server */
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp, int flags);

static NTSTATUS
_WMTBootstrapRegister(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = params->mach_port;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_register2(bp, params->name, rp, 0) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  return ret;
}

static NTSTATUS
_WMTBootstrapLookUp(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = 0;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_look_up(bp, params->name, &rp) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  params->mach_port = rp;
  return ret;
}

@protocol MTLDeviceSPI <MTLDevice>

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;

@end

@interface MTLSharedEventHandle ()

- (mach_port_t)eventPort;

@end

static NTSTATUS
_MTLSharedEvent_createMachPort(void *obj) {
  struct unixcall_mtlsharedevent_createmachport *params = obj;
  id<MTLSharedEvent> event = (id<MTLSharedEvent>)params->event;
  MTLSharedEventHandle *handle = [event newSharedEventHandle];
  mach_port_t port = [handle eventPort];
  
  // The eventPort method returns a send right that's owned by the handle.
  // We need to add our own send right since we're keeping the port but releasing the handle.
  // This increments the send right count so the port remains valid.
  mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
  
  params->ret_mach_port = port;
  [handle release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEventWithMachPort(void *obj) {
  struct unixcall_mtldevice_newsharedeventwithmachport *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLDeviceSPI> deviceSPI = (id<MTLDeviceSPI>)device;
  params->ret_event = (obj_handle_t)[deviceSPI newSharedEventWithMachPort:params->mach_port];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_registryID(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle registryID];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_waitUntilSignaledValue(void *obj) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue *params = obj;
  bool timeout = [(id<MTLSharedEvent>)params->event waitUntilSignaledValue:params->value timeoutMS:params->timeout_ms];
  params->ret_timeout = timeout;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_newTimestampBuffer(void *obj) {
  struct unixcall_mtlcountersamplebuffer_newtimestampbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;

  MTLCounterSampleBufferDescriptor *desc = [[MTLCounterSampleBufferDescriptor alloc] init];
  NSArray<id<MTLCounterSet>> *counter_sets = [device counterSets];
  id<MTLCounterSet> timestamp_counter_set = nil;
  for (id<MTLCounterSet> counterSet in counter_sets) {
    if ([counterSet.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestamp_counter_set = counterSet;
      break;
    }
  }
  desc.counterSet = timestamp_counter_set;
  desc.sampleCount = params->sample_count;
  desc.storageMode = params->shared ? MTLStorageModeShared : MTLStorageModePrivate;

  NSError *error = nil;
  params->ret = (obj_handle_t)[device newCounterSampleBufferWithDescriptor:desc error:&error];

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_resolveCounterRange(void *obj) {
  struct unixcall_mtlcountersamplebuffer_resolvecounterrange *params = obj;
  id<MTLCounterSampleBuffer> sample_buffer = (id<MTLCounterSampleBuffer>)params->sample_buffer;

  NSData *data = [sample_buffer resolveCounterRange:NSMakeRange(params->start, params->len)];
  if (data && params->data_out.ptr) {
    [data getBytes:params->data_out.ptr length:params->data_length];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(void *obj) {
  struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  struct WMTSampleBufferAttachmentInfo *attachments = params->attachments.ptr;

  MTLBlitPassDescriptor *blit_desc = [[MTLBlitPassDescriptor alloc] init];
  for (uint64_t i = 0; i < params->num_attachments; i++) {
    MTLBlitPassSampleBufferAttachmentDescriptor *desc = blit_desc.sampleBufferAttachments[i];
    desc.sampleBuffer = (id<MTLCounterSampleBuffer>)attachments[i].sample_buffer;
    desc.startOfEncoderSampleIndex = attachments[i].start_of_encoder_sample_index;
    desc.endOfEncoderSampleIndex = attachments[i].end_of_encoder_sample_index;
  }

  params->ret = (obj_handle_t)[cmdbuf blitCommandEncoderWithDescriptor:blit_desc];

  [blit_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_property(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->handle;
  double ns = 1000000000;
  switch (params->arg) {
  case WMTCommandBufferPropertyKernelStartTime:
    params->ret = (uint64_t)([cmdbuf kernelStartTime] * ns);
    break;
  case WMTCommandBufferPropertyKernelEndTime:
    params->ret = (uint64_t)([cmdbuf kernelEndTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUStartTime:
    params->ret = (uint64_t)([cmdbuf GPUStartTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUEndTime:
    params->ret = (uint64_t)([cmdbuf GPUEndTime] * ns);
    break;
  default:
    params->ret = 0;
    break;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newTileRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTTileRenderPipelineInfo *info = params->info.ptr;
  MTLTileRenderPipelineDescriptor *descriptor = [[MTLTileRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->color_formats[i]);
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_tile_buffers & (1 << i))
      descriptor.tileBuffers[i].mutability = MTLMutabilityImmutable;
  }

  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.threadgroupSizeMatchesTileSize = info->tgsize_matches_tile_size;


  descriptor.tileFunction = (id<MTLFunction>)info->tile_function;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithTileDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)err;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (!err && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addTileRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static const char *
macrunner_gpu_readback_phase_name(uint32_t phase) {
  switch ((enum MacRunnerGPUReadbackPhase)phase) {
  case MacRunnerGPUReadbackAfterClear:
    return "after-clear";
  case MacRunnerGPUReadbackAfterRender:
    return "after-draws";
  case MacRunnerGPUReadbackPrePresent:
    return "pre-present";
  case MacRunnerGPUReadbackShaderTexture:
    return "shader-texture";
  case MacRunnerGPUReadbackCausalC0:
    return "causal-C0";
  case MacRunnerGPUReadbackCausalC1:
    return "causal-C1";
  case MacRunnerGPUReadbackCausalC2:
    return "causal-C2";
  case MacRunnerGPUReadbackCausalC3:
    return "causal-C3";
  case MacRunnerGPUReadbackCausalPresentC0:
    return "causal-present-C0";
  case MacRunnerGPUReadbackCausalPresentC1:
    return "causal-present-C1";
  case MacRunnerGPUReadbackCausalPresentC2:
    return "causal-present-C2";
  case MacRunnerGPUReadbackCausalPresentC3:
    return "causal-present-C3";
  case MacRunnerGPUReadbackPresentedSurface:
    return "presented-surface";
  default:
    return "unknown";
  }
}

static void
macrunner_gpu_readback_schedule(id<MTLCommandBuffer> command_buffer,
                                id<MTLTexture> texture, uint32_t phase_value,
                                uint64_t tag) {
  const char *phase = macrunner_gpu_readback_phase_name(phase_value);

  BOOL causal_phase = phase_value >= MacRunnerGPUReadbackCausalC0 &&
                      phase_value <= MacRunnerGPUReadbackCausalPresentC3;
  BOOL presented_surface_phase = phase_value == MacRunnerGPUReadbackPresentedSurface;
  if (!macrunner_gpu_readback_probe_enabled() && !causal_phase && !presented_surface_phase &&
      !(macrunner_shader_inputs_probe_enabled() &&
        phase_value == MacRunnerGPUReadbackShaderTexture))
    return;
  if (!command_buffer || !texture) {
    fprintf(stderr, "macrunner-hb-gpu-probe: phase=readback-skip checkpoint=%s tag=0x%llx reason=null-object\n",
            phase, (unsigned long long)tag);
    return;
  }

  MTLPixelFormat format = texture.pixelFormat;
  BOOL bgra = format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB;
  BOOL rgba = format == MTLPixelFormatRGBA8Unorm || format == MTLPixelFormatRGBA8Unorm_sRGB;
  NSUInteger width = texture.width;
  NSUInteger height = texture.height;
  if ((!bgra && !rgba) || texture.textureType != MTLTextureType2D || texture.sampleCount != 1 ||
      !width || !height || width > 16384 || height > 16384) {
    fprintf(stderr,
            "macrunner-hb-gpu-probe: phase=readback-skip checkpoint=%s tag=0x%llx "
            "reason=unsupported format=%lu type=%lu samples=%lu size=%lux%lu\n",
            phase, (unsigned long long)tag, (unsigned long)format,
            (unsigned long)texture.textureType, (unsigned long)texture.sampleCount,
            (unsigned long)width, (unsigned long)height);
    return;
  }

  id<MTLDevice> device = texture.device;
  NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:format];
  if (!alignment)
    alignment = 256;
  if (width > SIZE_MAX / 4) {
    fprintf(stderr, "macrunner-hb-gpu-probe: phase=readback-skip checkpoint=%s tag=0x%llx reason=row-overflow\n",
            phase, (unsigned long long)tag);
    return;
  }
  NSUInteger packed_row_bytes = width * 4;
  NSUInteger row_bytes = ((packed_row_bytes + alignment - 1) / alignment) * alignment;
  if (row_bytes < packed_row_bytes || height > SIZE_MAX / row_bytes) {
    fprintf(stderr, "macrunner-hb-gpu-probe: phase=readback-skip checkpoint=%s tag=0x%llx reason=size-overflow\n",
            phase, (unsigned long long)tag);
    return;
  }
  NSUInteger byte_count = row_bytes * height;
  id<MTLBuffer> staging = [device newBufferWithLength:byte_count options:MTLResourceStorageModeShared];
  id<MTLBlitCommandEncoder> blit = staging ? [command_buffer blitCommandEncoder] : nil;
  if (!staging || !blit) {
    fprintf(stderr, "macrunner-hb-gpu-probe: phase=readback-skip checkpoint=%s tag=0x%llx reason=allocation\n",
            phase, (unsigned long long)tag);
    [staging release];
    return;
  }

  [blit copyFromTexture:texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(width, height, 1)
               toBuffer:staging
      destinationOffset:0
 destinationBytesPerRow:row_bytes
destinationBytesPerImage:byte_count];
  [blit endEncoding];
  fprintf(stderr,
          "macrunner-hb-gpu-probe: phase=readback-scheduled checkpoint=%s tag=0x%llx "
          "texture=%p format=%lu size=%lux%lu row=%lu\n",
          phase, (unsigned long long)tag, texture, (unsigned long)format,
          (unsigned long)width, (unsigned long)height, (unsigned long)row_bytes);

  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    const uint8_t *bytes = (const uint8_t *)staging.contents;
    uint64_t fnv = UINT64_C(1469598103934665603);
    uint64_t black = 0, nonblack = 0, colorful = 0;
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
    uint8_t min_r = 255, min_g = 255, min_b = 255, min_a = 255;
    uint8_t max_r = 0, max_g = 0, max_b = 0, max_a = 0;
    uint64_t pixels = (uint64_t)width * (uint64_t)height;
    for (NSUInteger y = 0; y < height; y++) {
      const uint8_t *row = bytes + y * row_bytes;
      for (NSUInteger x = 0; x < width; x++) {
        const uint8_t *p = row + x * 4;
        uint8_t r = bgra ? p[2] : p[0];
        uint8_t g = p[1];
        uint8_t b = bgra ? p[0] : p[2];
        uint8_t a = p[3];
        for (unsigned i = 0; i < 4; i++) {
          fnv ^= p[i];
          fnv *= UINT64_C(1099511628211);
        }
        uint8_t hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
        uint8_t lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (hi <= 4)
          black++;
        else
          nonblack++;
        if (hi > 16 && (unsigned)(hi - lo) > 24)
          colorful++;
        sum_r += r; sum_g += g; sum_b += b; sum_a += a;
        if (r < min_r) min_r = r; if (r > max_r) max_r = r;
        if (g < min_g) min_g = g; if (g > max_g) max_g = g;
        if (b < min_b) min_b = b; if (b > max_b) max_b = b;
        if (a < min_a) min_a = a; if (a > max_a) max_a = a;
      }
    }
    fprintf(stderr,
            "macrunner-hb-gpu-probe: phase=readback-complete checkpoint=%s tag=0x%llx "
            "status=%lu texture=%p format=%lu size=%lux%lu hash=0x%016llx "
            "pixels=%llu black=%llu nonblack=%llu colorful=%llu "
            "min=%u,%u,%u,%u max=%u,%u,%u,%u mean=%.3f,%.3f,%.3f,%.3f\n",
            phase, (unsigned long long)tag, (unsigned long)completed.status, texture,
            (unsigned long)format, (unsigned long)width, (unsigned long)height,
            (unsigned long long)fnv, (unsigned long long)pixels,
            (unsigned long long)black, (unsigned long long)nonblack,
            (unsigned long long)colorful, min_r, min_g, min_b, min_a,
            max_r, max_g, max_b, max_a,
            pixels ? (double)sum_r / pixels : 0.0, pixels ? (double)sum_g / pixels : 0.0,
            pixels ? (double)sum_b / pixels : 0.0, pixels ? (double)sum_a / pixels : 0.0);
    fflush(stderr);
    [staging release];
  }];
  fflush(stderr);
}

/*
 * Definition from cache.c
 */

NTSTATUS _CacheReader_alloc_init(void *obj);
NTSTATUS _CacheReader_get(void *obj);
NTSTATUS _CacheWriter_alloc_init(void *obj);
NTSTATUS _CacheWriter_set(void *obj);
NTSTATUS _WMTSetMetalShaderCachePath(void *obj);

const void *__wine_unix_call_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk_SM50Compile,
    &thunk_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk_SM50CompileGeometryPipelineVertex,
    &thunk_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk_SM50CompileTessellationPipelineHull,
    &thunk_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk_SM50GetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_MTLCommandBuffer_scheduleFrameDump,
};

#ifndef DXMT_NATIVE
const void *__wine_unix_call_wow64_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk32_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk32_SM50Compile,
    &thunk32_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk32_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk32_SM50CompileGeometryPipelineVertex,
    &thunk32_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk32_SM50CompileTessellationPipelineHull,
    &thunk32_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk32_SM50GetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_MTLCommandBuffer_scheduleFrameDump,
};
#endif
