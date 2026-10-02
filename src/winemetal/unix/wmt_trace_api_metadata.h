/* GENERATED: shared source of record/native ABI metadata. */
struct wmt_trace_api_field { unsigned offset, size; const char *name, *type, *direction, *kind, *object_type; };
struct wmt_trace_api_desc { unsigned ordinal, bytes, fields; const char *name, *record, *replay; const struct wmt_trace_api_field *field; };
static const struct wmt_trace_api_field wmt_api_fields_0[] = {
  {0,8,"object","obj_handle_t","IN","HANDLE","NSObject"},
};
static const struct wmt_trace_api_field wmt_api_fields_1[] = {
  {0,8,"object","obj_handle_t","IN","HANDLE","NSObject"},
};
static const struct wmt_trace_api_field wmt_api_fields_2[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","NSArray"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","NSObject/NSArray element"},
};
static const struct wmt_trace_api_field wmt_api_fields_3[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","NSArray"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_4[] = {
  {0,8,"ret","obj_handle_t","OUT","HANDLE","NSArray<MTLDevice>"},
};
static const struct wmt_trace_api_field wmt_api_fields_5[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_6[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_7[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","NSString"},
};
static const struct wmt_trace_api_field wmt_api_fields_8[] = {
  {0,8,"str","obj_handle_t","IN","HANDLE","NSString"},
  {8,8,"buffer_ptr","uint64_t","IN","VALUE",""},
  {16,8,"max_length","uint64_t","IN","VALUE",""},
  {24,8,"encoding","enum WMTStringEncoding","IN","VALUE",""},
  {32,4,"ret","uint32_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_9[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLCommandQueue"},
};
static const struct wmt_trace_api_field wmt_api_fields_10[] = {
  {0,8,"ret","obj_handle_t","OUT","HANDLE","NSAutoreleasePool"},
};
static const struct wmt_trace_api_field wmt_api_fields_11[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandQueue"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","MTLCommandBuffer"},
};
static const struct wmt_trace_api_field wmt_api_fields_12[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
};
static const struct wmt_trace_api_field wmt_api_fields_13[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
};
static const struct wmt_trace_api_field wmt_api_fields_14[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_15[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","MTLSharedEvent"},
};
static const struct wmt_trace_api_field wmt_api_fields_16[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLSharedEvent"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_17[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"arg0","obj_handle_t","IN","HANDLE","MTLSharedEvent/MTLEvent"},
  {16,8,"arg1","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_18[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTMemoryPointer","IN","POINTER",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLBuffer"},
};
static const struct wmt_trace_api_field wmt_api_fields_19[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTMemoryPointer","IN","POINTER",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLSamplerState"},
};
static const struct wmt_trace_api_field wmt_api_fields_20[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLDepthStencilState"},
};
static const struct wmt_trace_api_field wmt_api_fields_21[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTMemoryPointer","IN","POINTER",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLTexture"},
};
static const struct wmt_trace_api_field wmt_api_fields_22[] = {
  {0,8,"buffer","obj_handle_t","IN","HANDLE","MTLBuffer"},
  {8,8,"info","struct WMTMemoryPointer","IN","POINTER",""},
  {16,8,"offset","uint64_t","IN","VALUE",""},
  {24,8,"bytes_per_row","uint64_t","IN","VALUE",""},
  {32,8,"ret","obj_handle_t","OUT","HANDLE","MTLTexture"},
};
static const struct wmt_trace_api_field wmt_api_fields_23[] = {
  {0,8,"texture","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,4,"format","enum WMTPixelFormat","IN","VALUE",""},
  {12,4,"texture_type","enum WMTTextureType","IN","VALUE",""},
  {16,2,"level_start","uint16_t","IN","VALUE",""},
  {18,2,"level_count","uint16_t","IN","VALUE",""},
  {20,2,"slice_start","uint16_t","IN","VALUE",""},
  {22,2,"slice_count","uint16_t","IN","VALUE",""},
  {24,4,"swizzle","struct WMTTextureSwizzleChannels","IN","VALUE",""},
  {32,8,"ret","obj_handle_t","OUT","HANDLE","MTLTexture"},
  {40,8,"gpu_resource_id","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_24[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_25[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"data","obj_handle_t","IN","HANDLE","dispatch_data_t"},
  {16,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
  {24,8,"ret_library","obj_handle_t","OUT","HANDLE","MTLLibrary"},
};
static const struct wmt_trace_api_field wmt_api_fields_26[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLLibrary"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLFunction"},
};
static const struct wmt_trace_api_field wmt_api_fields_27[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","NSString"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_28[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","NSObject"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","NSString"},
};
static const struct wmt_trace_api_field wmt_api_fields_29[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
  {24,8,"ret_pso","obj_handle_t","OUT","HANDLE","MTLComputePipelineState"},
};
static const struct wmt_trace_api_field wmt_api_fields_30[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","MTLBlitCommandEncoder"},
};
static const struct wmt_trace_api_field wmt_api_fields_31[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLComputeCommandEncoder"},
};
static const struct wmt_trace_api_field wmt_api_fields_32[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLRenderCommandEncoder"},
};
static const struct wmt_trace_api_field wmt_api_fields_33[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandEncoder"},
};
static const struct wmt_trace_api_field wmt_api_fields_34[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
  {24,8,"ret_pso","obj_handle_t","OUT","HANDLE","MTLRenderPipelineState"},
};
static const struct wmt_trace_api_field wmt_api_fields_35[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
  {24,8,"ret_pso","obj_handle_t","OUT","HANDLE","MTLRenderPipelineState"},
};
static const struct wmt_trace_api_field wmt_api_fields_36[] = {
  {0,8,"encoder","obj_handle_t","IN","HANDLE","MTLBlitCommandEncoder"},
  {8,8,"cmd_head","struct WMTConstMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_37[] = {
  {0,8,"encoder","obj_handle_t","IN","HANDLE","MTLComputeCommandEncoder"},
  {8,8,"cmd_head","struct WMTConstMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_38[] = {
  {0,8,"encoder","obj_handle_t","IN","HANDLE","MTLRenderCommandEncoder"},
  {8,8,"cmd_head","struct WMTConstMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_39[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_40[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_41[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_42[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_43[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_44[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_45[] = {
  {0,8,"texture","obj_handle_t","IN","HANDLE","MTLTexture"},
  {8,24,"origin","struct WMTOrigin","IN","VALUE",""},
  {32,24,"size","struct WMTSize","IN","VALUE",""},
  {56,8,"level","uint64_t","IN","VALUE",""},
  {64,8,"slice","uint64_t","IN","VALUE",""},
  {72,8,"data","struct WMTMemoryPointer","IN","POINTER",""},
  {80,8,"bytes_per_row","uint64_t","IN","VALUE",""},
  {88,8,"bytes_per_image","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_46[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLBuffer"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_47[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"arg","obj_handle_t","IN","HANDLE","CAMetalDrawable"},
};
static const struct wmt_trace_api_field wmt_api_fields_48[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"arg0","obj_handle_t","IN","HANDLE","CAMetalDrawable"},
  {16,8,"arg1","double","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_49[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_50[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_51[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_52[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_53[] = {
  {0,8,"ret","obj_handle_t","OUT","HANDLE","MTLCaptureManager"},
};
static const struct wmt_trace_api_field wmt_api_fields_54[] = {
  {0,8,"capture_manager","obj_handle_t","IN","HANDLE","MTLCaptureManager"},
  {8,8,"info","struct WMTMemoryPointer","IN","POINTER",""},
  {16,1,"ret","bool","UNASSIGNED","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_55[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCaptureManager"},
};
static const struct wmt_trace_api_field wmt_api_fields_56[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLFXTemporalScaler"},
};
static const struct wmt_trace_api_field wmt_api_fields_57[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLFXSpatialScaler"},
};
static const struct wmt_trace_api_field wmt_api_fields_58[] = {
  {0,8,"cmdbuf","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"scaler","obj_handle_t","IN","HANDLE","MTLFXTemporalScaler"},
  {16,8,"color","obj_handle_t","IN","HANDLE","MTLTexture"},
  {24,8,"output","obj_handle_t","IN","HANDLE","MTLTexture"},
  {32,8,"depth","obj_handle_t","IN","HANDLE","MTLTexture"},
  {40,8,"motion","obj_handle_t","IN","HANDLE","MTLTexture"},
  {48,8,"exposure","obj_handle_t","IN","HANDLE","MTLTexture"},
  {56,8,"fence","obj_handle_t","IN","HANDLE","MTLFence"},
  {64,8,"props","struct WMTConstMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_59[] = {
  {0,8,"cmdbuf","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"scaler","obj_handle_t","IN","HANDLE","MTLFXSpatialScaler"},
  {16,8,"color","obj_handle_t","IN","HANDLE","MTLTexture"},
  {24,8,"output","obj_handle_t","IN","HANDLE","MTLTexture"},
  {32,8,"fence","obj_handle_t","IN","HANDLE","MTLFence"},
};
static const struct wmt_trace_api_field wmt_api_fields_60[] = {
  {0,8,"buffer_ptr","struct WMTConstMemoryPointer","IN","POINTER",""},
  {8,8,"encoding","enum WMTStringEncoding","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","NSString"},
};
static const struct wmt_trace_api_field wmt_api_fields_61[] = {
  {0,8,"buffer_ptr","struct WMTConstMemoryPointer","IN","POINTER",""},
  {8,8,"encoding","enum WMTStringEncoding","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","NSString"},
};
static const struct wmt_trace_api_field wmt_api_fields_62[] = {
  {0,8,"ret","obj_handle_t","OUT","HANDLE","DeveloperHUDProperties"},
};
static const struct wmt_trace_api_field wmt_api_fields_63[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","DeveloperHUDProperties"},
  {8,8,"arg0","obj_handle_t","IN","HANDLE","NSString"},
  {16,8,"arg1","obj_handle_t","IN","HANDLE","NSString"},
  {24,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_64[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","DeveloperHUDProperties"},
  {8,8,"arg0","obj_handle_t","IN","HANDLE","NSString"},
  {16,8,"arg1","obj_handle_t","IN","HANDLE","NSString"},
};
static const struct wmt_trace_api_field wmt_api_fields_65[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","DeveloperHUDProperties"},
  {8,8,"arg","obj_handle_t","IN","HANDLE","NSString"},
};
static const struct wmt_trace_api_field wmt_api_fields_66[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","CAMetalDrawable"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","MTLTexture"},
};
static const struct wmt_trace_api_field wmt_api_fields_67[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","CAMetalLayer"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","CAMetalDrawable"},
};
static const struct wmt_trace_api_field wmt_api_fields_68[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_69[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_70[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","CAMetalLayer"},
  {8,8,"arg","struct WMTConstMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_71[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","CAMetalLayer"},
  {8,8,"arg","struct WMTMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_72[] = {
  {0,8,"hwnd","uint64_t","IN","VALUE",""},
  {8,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {16,8,"ret_view","obj_handle_t","OUT","HANDLE","macdrv_metal_view/CAMetalLayer fallback"},
  {24,8,"ret_layer","obj_handle_t","OUT","HANDLE","CAMetalLayer"},
};
static const struct wmt_trace_api_field wmt_api_fields_73[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","macdrv_metal_view/CAMetalLayer"},
};
static const struct wmt_trace_api_field wmt_api_fields_74[] = {
  {0,8,"bytecode","const void *","IN","VALUE",""},
  {8,8,"bytecode_size","size_t","IN","VALUE",""},
  {16,8,"shader","sm50_shader_t *","IN","VALUE",""},
  {24,8,"reflection","void *","IN","VALUE",""},
  {32,8,"error","sm50_error_t *","IN","VALUE",""},
  {40,4,"ret","int","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_75[] = {
  {0,8,"shader","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_76[] = {
  {0,8,"shader","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {8,8,"args","void *","IN","VALUE",""},
  {16,8,"func_name","const char *","IN","VALUE",""},
  {24,8,"bitcode","sm50_bitcode_t *","IN","VALUE",""},
  {32,8,"error","sm50_error_t *","IN","VALUE",""},
  {40,4,"ret","int","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_77[] = {
  {0,8,"bitcode","sm50_bitcode_t","IN","HANDLE","sm50_bitcode_t"},
  {8,8,"data_out","struct SM50_COMPILED_BITCODE *","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_78[] = {
  {0,8,"bitcode","sm50_bitcode_t","IN","HANDLE","sm50_bitcode_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_79[] = {
  {0,8,"error","sm50_error_t","IN","HANDLE","sm50_error_t"},
  {8,8,"buffer","char *","IN","VALUE",""},
  {16,8,"buffer_size","size_t","IN","VALUE",""},
  {24,8,"ret_size","size_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_80[] = {
  {0,8,"error","sm50_error_t","IN","HANDLE","sm50_error_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_81[] = {
  {0,8,"vertex","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {8,8,"geometry","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {16,8,"vertex_args","void *","IN","VALUE",""},
  {24,8,"func_name","const char *","IN","VALUE",""},
  {32,8,"bitcode","sm50_bitcode_t *","IN","VALUE",""},
  {40,8,"error","sm50_error_t *","IN","VALUE",""},
  {48,4,"ret","int","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_82[] = {
  {0,8,"vertex","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {8,8,"geometry","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {16,8,"geometry_args","void *","IN","VALUE",""},
  {24,8,"func_name","const char *","IN","VALUE",""},
  {32,8,"bitcode","sm50_bitcode_t *","IN","VALUE",""},
  {40,8,"error","sm50_error_t *","IN","VALUE",""},
  {48,4,"ret","int","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_84[] = {
  {0,8,"vertex","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {8,8,"hull","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {16,8,"hull_args","void *","IN","VALUE",""},
  {24,8,"func_name","const char *","IN","VALUE",""},
  {32,8,"bitcode","sm50_bitcode_t *","IN","VALUE",""},
  {40,8,"error","sm50_error_t *","IN","VALUE",""},
  {48,4,"ret","int","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_85[] = {
  {0,8,"hull","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {8,8,"domain","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {16,8,"domain_args","void *","IN","VALUE",""},
  {24,8,"func_name","const char *","IN","VALUE",""},
  {32,8,"bitcode","sm50_bitcode_t *","IN","VALUE",""},
  {40,8,"error","sm50_error_t *","IN","VALUE",""},
  {48,4,"ret","int","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_86[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandEncoder"},
  {8,8,"arg","obj_handle_t","IN","HANDLE","NSString"},
};
static const struct wmt_trace_api_field wmt_api_fields_87[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_88[] = {
  {0,8,"shader","sm50_shader_t","IN","HANDLE","sm50_shader_t"},
  {8,8,"constant_buffers","struct MTL_SM50_SHADER_ARGUMENT *","IN","VALUE",""},
  {16,8,"arguments","struct MTL_SM50_SHADER_ARGUMENT *","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_89[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","NSError"},
};
static const struct wmt_trace_api_field wmt_api_fields_90[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","MTLLogContainer"},
};
static const struct wmt_trace_api_field wmt_api_fields_91[] = {
  {0,8,"enumerable","obj_handle_t","IN","HANDLE","MTLLogContainer"},
  {8,8,"start","uint64_t","IN","VALUE",""},
  {16,8,"buffer_size","uint64_t","IN","VALUE",""},
  {24,8,"buffer","struct WMTMemoryPointer","IN","POINTER",""},
  {32,8,"ret_read","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_92[] = {
  {0,8,"handle","obj_handle_t","IN","VALUE",""},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_93[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","CAMetalLayer"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_94[] = {
  {0,8,"ret","obj_handle_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_95[] = {
  {0,8,"ret","obj_handle_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_96[] = {
  {0,8,"handle","obj_handle_t","IN","VALUE",""},
  {8,8,"arg","struct WMTMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_97[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","CAMetalLayer"},
  {8,8,"arg","struct WMTMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_98[] = {
  {0,8,"library","obj_handle_t","IN","HANDLE","MTLLibrary"},
  {8,8,"name","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"constants","struct WMTConstMemoryPointer","IN","POINTER",""},
  {24,8,"num_constants","uint64_t","IN","VALUE",""},
  {32,8,"ret","obj_handle_t","OUT","HANDLE","MTLFunction"},
  {40,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
};
static const struct wmt_trace_api_field wmt_api_fields_99[] = {
  {0,8,"display_id","uint64_t","IN","VALUE",""},
  {8,8,"colorspace","enum WMTColorSpace","OUT","VALUE",""},
  {16,8,"hdr_metadata","struct WMTMemoryPointer","IN","POINTER",""},
  {24,1,"ret","bool","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_100[] = {
  {0,8,"display_id","uint64_t","IN","VALUE",""},
  {8,8,"colorspace","enum WMTColorSpace","IN","VALUE",""},
  {16,8,"hdr_metadata","struct WMTConstMemoryPointer","IN","POINTER",""},
};
static const struct wmt_trace_api_field wmt_api_fields_101[] = {
  {0,8,"layer","obj_handle_t","IN","HANDLE","CAMetalLayer"},
  {8,8,"version","uint64_t","OUT","VALUE",""},
  {16,8,"colorspace","enum WMTColorSpace","OUT","VALUE",""},
  {24,8,"hdr_metadata","struct WMTMemoryPointer","IN","POINTER",""},
  {32,8,"edr_value","struct WMTEDRValue","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_102[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"arg0","obj_handle_t","IN","HANDLE","MTLSharedEvent/MTLEvent"},
  {16,8,"arg1","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_103[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLSharedEvent"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_104[] = {
  {0,8,"shared_event","obj_handle_t","IN","HANDLE","MTLSharedEvent"},
  {8,8,"event_handle","obj_handle_t","IN","HANDLE","Win32 HANDLE"},
  {16,8,"shared_event_listener","obj_handle_t","IN","HANDLE","shared_event_listener_t"},
  {24,8,"value","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_105[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","MTLFence"},
};
static const struct wmt_trace_api_field wmt_api_fields_106[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","obj_handle_t","OUT","HANDLE","MTLEvent"},
};
static const struct wmt_trace_api_field wmt_api_fields_107[] = {
  {0,8,"buffer","obj_handle_t","IN","HANDLE","MTLBuffer"},
  {8,8,"offset","uint64_t","IN","VALUE",""},
  {16,8,"data","struct WMTConstMemoryPointer","IN","POINTER",""},
  {24,8,"length","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_108[] = {
  {0,8,"ret","obj_handle_t","OUT","HANDLE","shared_event_listener_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_109[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","shared_event_listener_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_110[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","shared_event_listener_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_111[] = {
  {0,8,"ret_major","uint64_t","OUT","VALUE",""},
  {8,8,"ret_minor","uint64_t","OUT","VALUE",""},
  {16,8,"ret_patch","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_112[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"url","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret_archive","obj_handle_t","OUT","HANDLE","MTLBinaryArchive"},
  {24,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
};
static const struct wmt_trace_api_field wmt_api_fields_113[] = {
  {0,8,"archive","obj_handle_t","IN","HANDLE","MTLBinaryArchive"},
  {8,8,"url","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
};
static const struct wmt_trace_api_field wmt_api_fields_114[] = {
  {0,8,"handle","obj_handle_t","IN","VALUE",""},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","dispatch_data_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_115[] = {
  {0,8,"path","struct WMTConstMemoryPointer","IN","POINTER",""},
  {8,8,"version","uint64_t","IN","VALUE",""},
  {16,8,"ret_cache","obj_handle_t","OUT","HANDLE","CacheReader"},
};
static const struct wmt_trace_api_field wmt_api_fields_116[] = {
  {0,8,"cache","obj_handle_t","IN","HANDLE","CacheReader"},
  {8,8,"key","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"key_length","uint64_t","IN","VALUE",""},
  {24,8,"ret_data","obj_handle_t","OUT","HANDLE","dispatch_data_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_117[] = {
  {0,8,"path","struct WMTConstMemoryPointer","IN","POINTER",""},
  {8,8,"version","uint64_t","IN","VALUE",""},
  {16,8,"ret_cache","obj_handle_t","OUT","HANDLE","CacheWriter"},
};
static const struct wmt_trace_api_field wmt_api_fields_118[] = {
  {0,8,"cache","obj_handle_t","IN","HANDLE","CacheWriter"},
  {8,8,"key","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"key_length","uint64_t","IN","VALUE",""},
  {24,8,"value_data","obj_handle_t","IN","HANDLE","dispatch_data_t"},
};
static const struct wmt_trace_api_field wmt_api_fields_119[] = {
  {0,8,"path","struct WMTConstMemoryPointer","IN","POINTER",""},
  {8,8,"ret_success","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_120[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTMemoryPointer","IN","POINTER",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLTexture"},
};
static const struct wmt_trace_api_field wmt_api_fields_121[] = {
  {0,128,"name","char[128]","IN","VALUE",""},
  {128,4,"mach_port","uint32_t","IN","VALUE",""},
  {132,4,"reserved","uint32_t","RESERVED","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_122[] = {
  {0,128,"name","char[128]","IN","VALUE",""},
  {128,4,"mach_port","uint32_t","OUT","VALUE",""},
  {132,4,"reserved","uint32_t","RESERVED","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_123[] = {
  {0,8,"event","obj_handle_t","IN","HANDLE","MTLSharedEvent"},
  {8,4,"ret_mach_port","uint32_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_124[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,4,"mach_port","uint32_t","IN","VALUE",""},
  {16,8,"ret_event","obj_handle_t","OUT","HANDLE","MTLSharedEvent"},
};
static const struct wmt_trace_api_field wmt_api_fields_125[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_126[] = {
  {0,8,"event","obj_handle_t","IN","HANDLE","MTLSharedEvent"},
  {8,8,"value","uint64_t","IN","VALUE",""},
  {16,8,"timeout_ms","uint64_t","IN","VALUE",""},
  {24,1,"ret_timeout","bool","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_127[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,4,"sample_count","uint32_t","IN","VALUE",""},
  {12,4,"shared","uint32_t","IN","VALUE",""},
  {16,8,"ret","obj_handle_t","OUT","HANDLE","MTLCounterSampleBuffer"},
};
static const struct wmt_trace_api_field wmt_api_fields_128[] = {
  {0,8,"sample_buffer","obj_handle_t","IN","HANDLE","MTLCounterSampleBuffer"},
  {8,4,"start","uint32_t","IN","VALUE",""},
  {12,4,"len","uint32_t","IN","VALUE",""},
  {16,8,"data_out","struct WMTMemoryPointer","IN","POINTER",""},
  {24,8,"data_length","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_129[] = {
  {0,8,"cmdbuf","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"attachments","struct WMTMemoryPointer","IN","POINTER",""},
  {16,8,"num_attachments","uint64_t","IN","VALUE",""},
  {24,8,"ret","obj_handle_t","OUT","HANDLE","MTLBlitCommandEncoder"},
};
static const struct wmt_trace_api_field wmt_api_fields_130[] = {
  {0,8,"handle","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"arg","uint64_t","IN","VALUE",""},
  {16,8,"ret","uint64_t","OUT","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_131[] = {
  {0,8,"device","obj_handle_t","IN","HANDLE","MTLDevice"},
  {8,8,"info","struct WMTConstMemoryPointer","IN","POINTER",""},
  {16,8,"ret_error","obj_handle_t","OUT","HANDLE","NSError"},
  {24,8,"ret_pso","obj_handle_t","OUT","HANDLE","MTLRenderPipelineState"},
};
static const struct wmt_trace_api_field wmt_api_fields_132[] = {
  {0,8,"cmdbuf","obj_handle_t","IN","HANDLE","MTLCommandBuffer"},
  {8,8,"texture","obj_handle_t","IN","HANDLE","MTLTexture"},
  {16,8,"frame","uint64_t","IN","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_133[] = {
  {0,8,"buffer","obj_handle_t","IN","HANDLE","MTLBuffer"},
  {8,8,"offset","uint64_t","IN","VALUE",""},
  {16,8,"count","uint64_t","IN","VALUE",""},
  {24,8,"stride","uint64_t","IN","VALUE",""},
  {32,4,"kind","uint32_t","IN","VALUE",""},
  {36,4,"reserved","uint32_t","RESERVED","VALUE",""},
};
static const struct wmt_trace_api_field wmt_api_fields_134[] = {
  {0,8,"buffer","obj_handle_t","IN","HANDLE","MTLBuffer"},
  {8,8,"offset","uint64_t","IN","VALUE",""},
  {16,8,"length","uint64_t","IN","VALUE",""},
  {24,4,"action","uint32_t","IN","VALUE",""},
  {28,4,"reserved","uint32_t","RESERVED","VALUE",""},
};
static const struct wmt_trace_api_desc wmt_trace_api_descriptors[] = {
  {0,8,1,"_NSObject_retain","LIFETIME_MODEL","LIFETIME_MODEL",wmt_api_fields_0},
  {1,8,1,"_NSObject_release","LIFETIME_MODEL","LIFETIME_MODEL",wmt_api_fields_1},
  {2,24,3,"_NSArray_object","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_2},
  {3,16,2,"_NSArray_count","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_3},
  {4,8,1,"_MTLCopyAllDevices","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_4},
  {5,16,2,"_MTLDevice_recommendedMaxWorkingSetSize","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_5},
  {6,16,2,"_MTLDevice_currentAllocatedSize","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_6},
  {7,16,2,"_MTLDevice_name","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_7},
  {8,40,5,"_NSString_getCString","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_8},
  {9,24,3,"_MTLDevice_newCommandQueue","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_9},
  {10,8,1,"_NSAutoreleasePool_alloc_init","LIFETIME_MODEL","LIFETIME_MODEL",wmt_api_fields_10},
  {11,16,2,"_MTLCommandQueue_commandBuffer","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_11},
  {12,8,1,"_MTLCommandBuffer_commit","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_12},
  {13,8,1,"_MTLCommandBuffer_waitUntilCompleted","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_13},
  {14,16,2,"_MTLCommandBuffer_status","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_14},
  {15,16,2,"_MTLDevice_newSharedEvent","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_15},
  {16,16,2,"_MTLSharedEvent_signaledValue","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_16},
  {17,24,3,"_MTLCommandBuffer_encodeSignalEvent","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_17},
  {18,24,3,"_MTLDevice_newBuffer","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_18},
  {19,24,3,"_MTLDevice_newSamplerState","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_19},
  {20,24,3,"_MTLDevice_newDepthStencilState","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_20},
  {21,24,3,"_MTLDevice_newTexture","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_21},
  {22,40,5,"_MTLBuffer_newTexture","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_22},
  {23,48,10,"_MTLTexture_newTextureView","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_23},
  {24,24,3,"_MTLDevice_minimumLinearTextureAlignmentForPixelFormat","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_24},
  {25,32,4,"_MTLDevice_newLibrary","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_25},
  {26,24,3,"_MTLLibrary_newFunction","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_26},
  {27,24,3,"_NSString_lengthOfBytesUsingEncoding","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_27},
  {28,16,2,"_NSObject_description","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_28},
  {29,32,4,"_MTLDevice_newComputePipelineState","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_29},
  {30,16,2,"_MTLCommandBuffer_blitCommandEncoder","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_30},
  {31,24,3,"_MTLCommandBuffer_computeCommandEncoder","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_31},
  {32,24,3,"_MTLCommandBuffer_renderCommandEncoder","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_32},
  {33,8,1,"_MTLCommandEncoder_endEncoding","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_33},
  {34,32,4,"_MTLDevice_newRenderPipelineState","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_34},
  {35,32,4,"_MTLDevice_newMeshRenderPipelineState","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_35},
  {36,16,2,"_MTLBlitCommandEncoder_encodeCommands","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_36},
  {37,16,2,"_MTLComputeCommandEncoder_encodeCommands","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_37},
  {38,16,2,"_MTLRenderCommandEncoder_encodeCommands","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_38},
  {39,16,2,"_MTLTexture_pixelFormat","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_39},
  {40,16,2,"_MTLTexture_width","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_40},
  {41,16,2,"_MTLTexture_height","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_41},
  {42,16,2,"_MTLTexture_depth","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_42},
  {43,16,2,"_MTLTexture_arrayLength","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_43},
  {44,16,2,"_MTLTexture_mipmapLevelCount","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_44},
  {45,96,8,"_MTLTexture_replaceRegion","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_45},
  {46,24,3,"_MTLBuffer_didModifyRange","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_46},
  {47,16,2,"_MTLCommandBuffer_presentDrawable","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_47},
  {48,24,3,"_MTLCommandBuffer_presentDrawableAfterMinimumDuration","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_48},
  {49,24,3,"_MTLDevice_supportsFamily","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_49},
  {50,16,2,"_MTLDevice_supportsBCTextureCompression","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_50},
  {51,24,3,"_MTLDevice_supportsTextureSampleCount","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_51},
  {52,16,2,"_MTLDevice_hasUnifiedMemory","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_52},
  {53,8,1,"_MTLCaptureManager_sharedCaptureManager","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_53},
  {54,24,3,"_MTLCaptureManager_startCapture","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_54},
  {55,8,1,"_MTLCaptureManager_stopCapture","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_55},
  {56,24,3,"_MTLDevice_newTemporalScaler","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_56},
  {57,24,3,"_MTLDevice_newSpatialScaler","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_57},
  {58,72,9,"_MTLCommandBuffer_encodeTemporalScale","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_58},
  {59,40,5,"_MTLCommandBuffer_encodeSpatialScale","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_59},
  {60,24,3,"_NSString_string","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_60},
  {61,24,3,"_NSString_alloc_init","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_61},
  {62,8,1,"_DeveloperHUDProperties_instance","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_62},
  {63,32,4,"_DeveloperHUDProperties_addLabel","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_63},
  {64,24,3,"_DeveloperHUDProperties_updateLabel","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_64},
  {65,16,2,"_DeveloperHUDProperties_remove","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_65},
  {66,16,2,"_MetalDrawable_texture","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_66},
  {67,16,2,"_MetalLayer_nextDrawable","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_67},
  {68,16,2,"_MTLDevice_supportsFXSpatialScaler","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_68},
  {69,16,2,"_MTLDevice_supportsFXTemporalScaler","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_69},
  {70,16,2,"_MetalLayer_setProps","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_70},
  {71,16,2,"_MetalLayer_getProps","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_71},
  {72,32,4,"_CreateMetalViewFromHWND","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_72},
  {73,8,1,"_ReleaseMetalView","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_73},
  {74,48,6,"thunk_SM50Initialize","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_74},
  {75,8,1,"thunk_SM50Destroy","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_75},
  {76,48,6,"thunk_SM50Compile","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_76},
  {77,16,2,"thunk_SM50GetCompiledBitcode","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_77},
  {78,8,1,"thunk_SM50DestroyBitcode","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_78},
  {79,32,4,"thunk_SM50GetErrorMessage","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_79},
  {80,8,1,"thunk_SM50FreeError","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_80},
  {81,56,7,"thunk_SM50CompileGeometryPipelineVertex","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_81},
  {82,56,7,"thunk_SM50CompileGeometryPipelineGeometry","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_82},
  {84,56,7,"thunk_SM50CompileTessellationPipelineHull","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_84},
  {85,56,7,"thunk_SM50CompileTessellationPipelineDomain","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_85},
  {86,16,2,"_MTLCommandEncoder_setLabel","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_86},
  {87,16,2,"_MTLDevice_setShouldMaximizeConcurrentCompilation","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_87},
  {88,24,3,"thunk_SM50GetArgumentsInfo","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_88},
  {89,16,2,"_MTLCommandBuffer_error","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_89},
  {90,16,2,"_MTLCommandBuffer_logs","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_90},
  {91,40,5,"_MTLLogContainer_enumerate","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_91},
  {92,16,2,"_CGColorSpace_checkColorSpaceSupported","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_92},
  {93,24,3,"_MetalLayer_setColorSpace","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_93},
  {94,8,1,"_WMTGetPrimaryDisplayId","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_94},
  {95,8,1,"_WMTGetSecondaryDisplayId","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_95},
  {96,16,2,"_WMTGetDisplayDescription","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_96},
  {97,16,2,"_MetalLayer_getEDRValue","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_97},
  {98,48,6,"_MTLLibrary_newFunctionWithConstants","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_98},
  {99,32,4,"_WMTQueryDisplaySetting","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_99},
  {100,24,3,"_WMTUpdateDisplaySetting","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_100},
  {101,40,5,"_WMTQueryDisplaySettingForLayer","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_101},
  {102,24,3,"_MTLCommandBuffer_encodeWaitForEvent","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_102},
  {103,16,2,"_MTLSharedEvent_signalValue","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_103},
  {104,32,4,"_MTLSharedEvent_setWin32EventAtValue","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_104},
  {105,16,2,"_MTLDevice_newFence","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_105},
  {106,16,2,"_MTLDevice_newEvent","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_106},
  {107,32,4,"_MTLBuffer_updateContents","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_107},
  {108,8,1,"_SharedEventListener_create","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_108},
  {109,8,1,"_SharedEventListener_start","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_109},
  {110,8,1,"_SharedEventListener_destroy","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_110},
  {111,24,3,"_WMTGetOSVersion","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_111},
  {112,32,4,"_MTLDevice_newBinaryArchive","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_112},
  {113,24,3,"_MTLBinaryArchive_serialize","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_113},
  {114,24,3,"_DispatchData_alloc_init","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_114},
  {115,24,3,"_CacheReader_alloc_init","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_115},
  {116,32,4,"_CacheReader_get","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_116},
  {117,24,3,"_CacheWriter_alloc_init","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_117},
  {118,32,4,"_CacheWriter_set","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_118},
  {119,16,2,"_WMTSetMetalShaderCachePath","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_119},
  {120,24,3,"_MTLDevice_newSharedTexture","EXTERNAL_REQUIRED","EXTERNAL_REQUIRED",wmt_api_fields_120},
  {121,136,3,"_WMTBootstrapRegister","EXTERNAL_REQUIRED","EXTERNAL_REQUIRED",wmt_api_fields_121},
  {122,136,3,"_WMTBootstrapLookUp","EXTERNAL_REQUIRED","EXTERNAL_REQUIRED",wmt_api_fields_122},
  {123,16,2,"_MTLSharedEvent_createMachPort","EXTERNAL_REQUIRED","EXTERNAL_REQUIRED",wmt_api_fields_123},
  {124,24,3,"_MTLDevice_newSharedEventWithMachPort","EXTERNAL_REQUIRED","EXTERNAL_REQUIRED",wmt_api_fields_124},
  {125,16,2,"_MTLDevice_registryID","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_125},
  {126,32,4,"_MTLSharedEvent_waitUntilSignaledValue","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_126},
  {127,24,4,"_MTLCounterSampleBuffer_newTimestampBuffer","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_127},
  {128,32,5,"_MTLCounterSampleBuffer_resolveCounterRange","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_128},
  {129,32,4,"_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_129},
  {130,24,3,"_MTLCommandBuffer_property","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_130},
  {131,32,4,"_MTLDevice_newTileRenderPipelineState","GENERATED_TYPED_CALL","NATIVE_TYPED_CALL",wmt_api_fields_131},
  {132,24,3,"_MTLCommandBuffer_scheduleFrameDump","FRONTEND_ARTIFACT","FRONTEND_ARTIFACT",wmt_api_fields_132},
  {133,40,6,"_MTLBuffer_traceFields","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_133},
  {134,32,5,"_MTLBuffer_traceOwnership","LEGACY_EFFECT_EVENT","LEGACY_EFFECT_EVENT",wmt_api_fields_134},
};
