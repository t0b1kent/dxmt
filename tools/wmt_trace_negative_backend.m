#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "winemetal_thunks.h"
typedef int (*call_fn)(void *);
call_fn __wine_unix_call_funcs[135];
static call_fn *original;
static void *base;
static unsigned long long hits;
static const char *mode;
static int texture(void *arg) {
  struct unixcall_mtldevice_newtexture *p=arg;
  if(strcmp(mode,"format"))return original[21](arg);
  struct WMTTextureInfo info=*(struct WMTTextureInfo *)p->info.ptr;
  struct unixcall_mtldevice_newtexture copy=*p;
  info.pixel_format=WMTPixelFormatRGBA8Unorm_sRGB;copy.info.ptr=&info;
  if(!hits++)fprintf(stderr,"NEGATIVE_CONTROL format ACTIVE\n");
  int rc=original[21](&copy);p->ret=copy.ret;return rc;
}
static int pipeline(void *arg) {
  struct unixcall_mtldevice_newrenderpso *p=arg;
  if(strcmp(mode,"blend"))return original[34](arg);
  struct WMTRenderPipelineInfo info=*(const struct WMTRenderPipelineInfo *)p->info.ptr;
  struct unixcall_mtldevice_newrenderpso copy=*p;
  for(unsigned i=0;i<8;++i) {
    info.colors[i].blending_enabled=true;
    info.colors[i].src_rgb_blend_factor=WMTBlendFactorZero;
    info.colors[i].dst_rgb_blend_factor=WMTBlendFactorZero;
  }
  copy.info.ptr=&info;
  if(!hits++)fprintf(stderr,"NEGATIVE_CONTROL blend ACTIVE\n");
  int rc=original[34](&copy);p->ret_error=copy.ret_error;p->ret_pso=copy.ret_pso;return rc;
}
static int render(void *arg) {
  struct unixcall_generic_obj_cmd_noret *p=arg;
  if(!strcmp(mode,"skip")) {
    for(struct wmtcmd_base *cmd=(struct wmtcmd_base *)p->cmd_head.ptr;cmd;cmd=cmd->next.ptr) {
      if(cmd->type==WMTRenderCommandSetPSO) {
        cmd->type=0; /* Explicit NOP in original packed-command consumer. */
        if(!hits++)fprintf(stderr,"NEGATIVE_CONTROL skip_SetPSO ACTIVE\n");
      }
    }
  }
  return original[38](arg);
}
uint64_t wmt_native_foreign_callback_count(void) {
  uint64_t (*fn)(void)=dlsym(base,"wmt_native_foreign_callback_count");
  return fn?fn():UINT64_MAX;
}
__attribute__((constructor)) static void initialize(void) {
  mode=getenv("MACRUNNER_WMT_STAND_MUTATION");
  const char *path=getenv("MACRUNNER_WMT_STAND_BASE_BACKEND");
  if(!mode||!path)abort();
  base=dlopen(path,RTLD_NOW|RTLD_LOCAL);
  original=base?dlsym(base,"__wine_unix_call_funcs"):NULL;
  if(!original)abort();
  memcpy(__wine_unix_call_funcs,original,sizeof(__wine_unix_call_funcs));
  __wine_unix_call_funcs[21]=texture;
  __wine_unix_call_funcs[34]=pipeline;
  __wine_unix_call_funcs[38]=render;
}
