/* GENERATED: no manual edits. Include after original API definitions. */
static void *wmt_trace_api_begin(unsigned ordinal, void *params);
static void wmt_trace_api_end(unsigned ordinal, void *params, int status, void *ticket);
static int wmt_trace_api_dispatch_0(void *params) {
  void *ticket = wmt_trace_api_begin(0, params);
  int status = _NSObject_retain(params);
  wmt_trace_api_end(0, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_1(void *params) {
  void *ticket = wmt_trace_api_begin(1, params);
  int status = _NSObject_release(params);
  wmt_trace_api_end(1, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_2(void *params) {
  void *ticket = wmt_trace_api_begin(2, params);
  int status = _NSArray_object(params);
  wmt_trace_api_end(2, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_3(void *params) {
  void *ticket = wmt_trace_api_begin(3, params);
  int status = _NSArray_count(params);
  wmt_trace_api_end(3, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_4(void *params) {
  void *ticket = wmt_trace_api_begin(4, params);
  int status = _MTLCopyAllDevices(params);
  wmt_trace_api_end(4, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_5(void *params) {
  void *ticket = wmt_trace_api_begin(5, params);
  int status = _MTLDevice_recommendedMaxWorkingSetSize(params);
  wmt_trace_api_end(5, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_6(void *params) {
  void *ticket = wmt_trace_api_begin(6, params);
  int status = _MTLDevice_currentAllocatedSize(params);
  wmt_trace_api_end(6, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_7(void *params) {
  void *ticket = wmt_trace_api_begin(7, params);
  int status = _MTLDevice_name(params);
  wmt_trace_api_end(7, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_8(void *params) {
  void *ticket = wmt_trace_api_begin(8, params);
  int status = _NSString_getCString(params);
  wmt_trace_api_end(8, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_9(void *params) {
  void *ticket = wmt_trace_api_begin(9, params);
  int status = _MTLDevice_newCommandQueue(params);
  wmt_trace_api_end(9, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_10(void *params) {
  void *ticket = wmt_trace_api_begin(10, params);
  int status = _NSAutoreleasePool_alloc_init(params);
  wmt_trace_api_end(10, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_11(void *params) {
  void *ticket = wmt_trace_api_begin(11, params);
  int status = _MTLCommandQueue_commandBuffer(params);
  wmt_trace_api_end(11, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_12(void *params) {
  void *ticket = wmt_trace_api_begin(12, params);
  int status = _MTLCommandBuffer_commit(params);
  wmt_trace_api_end(12, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_13(void *params) {
  void *ticket = wmt_trace_api_begin(13, params);
  int status = _MTLCommandBuffer_waitUntilCompleted(params);
  wmt_trace_api_end(13, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_14(void *params) {
  void *ticket = wmt_trace_api_begin(14, params);
  int status = _MTLCommandBuffer_status(params);
  wmt_trace_api_end(14, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_15(void *params) {
  void *ticket = wmt_trace_api_begin(15, params);
  int status = _MTLDevice_newSharedEvent(params);
  wmt_trace_api_end(15, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_16(void *params) {
  void *ticket = wmt_trace_api_begin(16, params);
  int status = _MTLSharedEvent_signaledValue(params);
  wmt_trace_api_end(16, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_17(void *params) {
  void *ticket = wmt_trace_api_begin(17, params);
  int status = _MTLCommandBuffer_encodeSignalEvent(params);
  wmt_trace_api_end(17, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_18(void *params) {
  void *ticket = wmt_trace_api_begin(18, params);
  int status = _MTLDevice_newBuffer(params);
  wmt_trace_api_end(18, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_19(void *params) {
  void *ticket = wmt_trace_api_begin(19, params);
  int status = _MTLDevice_newSamplerState(params);
  wmt_trace_api_end(19, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_20(void *params) {
  void *ticket = wmt_trace_api_begin(20, params);
  int status = _MTLDevice_newDepthStencilState(params);
  wmt_trace_api_end(20, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_21(void *params) {
  void *ticket = wmt_trace_api_begin(21, params);
  int status = _MTLDevice_newTexture(params);
  wmt_trace_api_end(21, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_22(void *params) {
  void *ticket = wmt_trace_api_begin(22, params);
  int status = _MTLBuffer_newTexture(params);
  wmt_trace_api_end(22, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_23(void *params) {
  void *ticket = wmt_trace_api_begin(23, params);
  int status = _MTLTexture_newTextureView(params);
  wmt_trace_api_end(23, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_24(void *params) {
  void *ticket = wmt_trace_api_begin(24, params);
  int status = _MTLDevice_minimumLinearTextureAlignmentForPixelFormat(params);
  wmt_trace_api_end(24, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_25(void *params) {
  void *ticket = wmt_trace_api_begin(25, params);
  int status = _MTLDevice_newLibrary(params);
  wmt_trace_api_end(25, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_26(void *params) {
  void *ticket = wmt_trace_api_begin(26, params);
  int status = _MTLLibrary_newFunction(params);
  wmt_trace_api_end(26, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_27(void *params) {
  void *ticket = wmt_trace_api_begin(27, params);
  int status = _NSString_lengthOfBytesUsingEncoding(params);
  wmt_trace_api_end(27, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_28(void *params) {
  void *ticket = wmt_trace_api_begin(28, params);
  int status = _NSObject_description(params);
  wmt_trace_api_end(28, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_29(void *params) {
  void *ticket = wmt_trace_api_begin(29, params);
  int status = _MTLDevice_newComputePipelineState(params);
  wmt_trace_api_end(29, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_30(void *params) {
  void *ticket = wmt_trace_api_begin(30, params);
  int status = _MTLCommandBuffer_blitCommandEncoder(params);
  wmt_trace_api_end(30, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_31(void *params) {
  void *ticket = wmt_trace_api_begin(31, params);
  int status = _MTLCommandBuffer_computeCommandEncoder(params);
  wmt_trace_api_end(31, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_32(void *params) {
  void *ticket = wmt_trace_api_begin(32, params);
  int status = _MTLCommandBuffer_renderCommandEncoder(params);
  wmt_trace_api_end(32, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_33(void *params) {
  void *ticket = wmt_trace_api_begin(33, params);
  int status = _MTLCommandEncoder_endEncoding(params);
  wmt_trace_api_end(33, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_34(void *params) {
  void *ticket = wmt_trace_api_begin(34, params);
  int status = _MTLDevice_newRenderPipelineState(params);
  wmt_trace_api_end(34, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_35(void *params) {
  void *ticket = wmt_trace_api_begin(35, params);
  int status = _MTLDevice_newMeshRenderPipelineState(params);
  wmt_trace_api_end(35, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_36(void *params) {
  void *ticket = wmt_trace_api_begin(36, params);
  int status = _MTLBlitCommandEncoder_encodeCommands(params);
  wmt_trace_api_end(36, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_37(void *params) {
  void *ticket = wmt_trace_api_begin(37, params);
  int status = _MTLComputeCommandEncoder_encodeCommands(params);
  wmt_trace_api_end(37, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_38(void *params) {
  void *ticket = wmt_trace_api_begin(38, params);
  int status = _MTLRenderCommandEncoder_encodeCommands(params);
  wmt_trace_api_end(38, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_39(void *params) {
  void *ticket = wmt_trace_api_begin(39, params);
  int status = _MTLTexture_pixelFormat(params);
  wmt_trace_api_end(39, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_40(void *params) {
  void *ticket = wmt_trace_api_begin(40, params);
  int status = _MTLTexture_width(params);
  wmt_trace_api_end(40, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_41(void *params) {
  void *ticket = wmt_trace_api_begin(41, params);
  int status = _MTLTexture_height(params);
  wmt_trace_api_end(41, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_42(void *params) {
  void *ticket = wmt_trace_api_begin(42, params);
  int status = _MTLTexture_depth(params);
  wmt_trace_api_end(42, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_43(void *params) {
  void *ticket = wmt_trace_api_begin(43, params);
  int status = _MTLTexture_arrayLength(params);
  wmt_trace_api_end(43, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_44(void *params) {
  void *ticket = wmt_trace_api_begin(44, params);
  int status = _MTLTexture_mipmapLevelCount(params);
  wmt_trace_api_end(44, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_45(void *params) {
  void *ticket = wmt_trace_api_begin(45, params);
  int status = _MTLTexture_replaceRegion(params);
  wmt_trace_api_end(45, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_46(void *params) {
  void *ticket = wmt_trace_api_begin(46, params);
  int status = _MTLBuffer_didModifyRange(params);
  wmt_trace_api_end(46, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_47(void *params) {
  void *ticket = wmt_trace_api_begin(47, params);
  int status = _MTLCommandBuffer_presentDrawable(params);
  wmt_trace_api_end(47, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_48(void *params) {
  void *ticket = wmt_trace_api_begin(48, params);
  int status = _MTLCommandBuffer_presentDrawableAfterMinimumDuration(params);
  wmt_trace_api_end(48, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_49(void *params) {
  void *ticket = wmt_trace_api_begin(49, params);
  int status = _MTLDevice_supportsFamily(params);
  wmt_trace_api_end(49, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_50(void *params) {
  void *ticket = wmt_trace_api_begin(50, params);
  int status = _MTLDevice_supportsBCTextureCompression(params);
  wmt_trace_api_end(50, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_51(void *params) {
  void *ticket = wmt_trace_api_begin(51, params);
  int status = _MTLDevice_supportsTextureSampleCount(params);
  wmt_trace_api_end(51, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_52(void *params) {
  void *ticket = wmt_trace_api_begin(52, params);
  int status = _MTLDevice_hasUnifiedMemory(params);
  wmt_trace_api_end(52, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_53(void *params) {
  void *ticket = wmt_trace_api_begin(53, params);
  int status = _MTLCaptureManager_sharedCaptureManager(params);
  wmt_trace_api_end(53, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_54(void *params) {
  void *ticket = wmt_trace_api_begin(54, params);
  int status = _MTLCaptureManager_startCapture(params);
  wmt_trace_api_end(54, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_55(void *params) {
  void *ticket = wmt_trace_api_begin(55, params);
  int status = _MTLCaptureManager_stopCapture(params);
  wmt_trace_api_end(55, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_56(void *params) {
  void *ticket = wmt_trace_api_begin(56, params);
  int status = _MTLDevice_newTemporalScaler(params);
  wmt_trace_api_end(56, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_57(void *params) {
  void *ticket = wmt_trace_api_begin(57, params);
  int status = _MTLDevice_newSpatialScaler(params);
  wmt_trace_api_end(57, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_58(void *params) {
  void *ticket = wmt_trace_api_begin(58, params);
  int status = _MTLCommandBuffer_encodeTemporalScale(params);
  wmt_trace_api_end(58, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_59(void *params) {
  void *ticket = wmt_trace_api_begin(59, params);
  int status = _MTLCommandBuffer_encodeSpatialScale(params);
  wmt_trace_api_end(59, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_60(void *params) {
  void *ticket = wmt_trace_api_begin(60, params);
  int status = _NSString_string(params);
  wmt_trace_api_end(60, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_61(void *params) {
  void *ticket = wmt_trace_api_begin(61, params);
  int status = _NSString_alloc_init(params);
  wmt_trace_api_end(61, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_62(void *params) {
  void *ticket = wmt_trace_api_begin(62, params);
  int status = _DeveloperHUDProperties_instance(params);
  wmt_trace_api_end(62, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_63(void *params) {
  void *ticket = wmt_trace_api_begin(63, params);
  int status = _DeveloperHUDProperties_addLabel(params);
  wmt_trace_api_end(63, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_64(void *params) {
  void *ticket = wmt_trace_api_begin(64, params);
  int status = _DeveloperHUDProperties_updateLabel(params);
  wmt_trace_api_end(64, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_65(void *params) {
  void *ticket = wmt_trace_api_begin(65, params);
  int status = _DeveloperHUDProperties_remove(params);
  wmt_trace_api_end(65, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_66(void *params) {
  void *ticket = wmt_trace_api_begin(66, params);
  int status = _MetalDrawable_texture(params);
  wmt_trace_api_end(66, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_67(void *params) {
  void *ticket = wmt_trace_api_begin(67, params);
  int status = _MetalLayer_nextDrawable(params);
  wmt_trace_api_end(67, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_68(void *params) {
  void *ticket = wmt_trace_api_begin(68, params);
  int status = _MTLDevice_supportsFXSpatialScaler(params);
  wmt_trace_api_end(68, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_69(void *params) {
  void *ticket = wmt_trace_api_begin(69, params);
  int status = _MTLDevice_supportsFXTemporalScaler(params);
  wmt_trace_api_end(69, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_70(void *params) {
  void *ticket = wmt_trace_api_begin(70, params);
  int status = _MetalLayer_setProps(params);
  wmt_trace_api_end(70, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_71(void *params) {
  void *ticket = wmt_trace_api_begin(71, params);
  int status = _MetalLayer_getProps(params);
  wmt_trace_api_end(71, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_72(void *params) {
  void *ticket = wmt_trace_api_begin(72, params);
  int status = _CreateMetalViewFromHWND(params);
  wmt_trace_api_end(72, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_73(void *params) {
  void *ticket = wmt_trace_api_begin(73, params);
  int status = _ReleaseMetalView(params);
  wmt_trace_api_end(73, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_74(void *params) {
  void *ticket = wmt_trace_api_begin(74, params);
  int status = thunk_SM50Initialize(params);
  wmt_trace_api_end(74, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_75(void *params) {
  void *ticket = wmt_trace_api_begin(75, params);
  int status = thunk_SM50Destroy(params);
  wmt_trace_api_end(75, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_76(void *params) {
  void *ticket = wmt_trace_api_begin(76, params);
  int status = thunk_SM50Compile(params);
  wmt_trace_api_end(76, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_77(void *params) {
  void *ticket = wmt_trace_api_begin(77, params);
  int status = thunk_SM50GetCompiledBitcode(params);
  wmt_trace_api_end(77, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_78(void *params) {
  void *ticket = wmt_trace_api_begin(78, params);
  int status = thunk_SM50DestroyBitcode(params);
  wmt_trace_api_end(78, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_79(void *params) {
  void *ticket = wmt_trace_api_begin(79, params);
  int status = thunk_SM50GetErrorMessage(params);
  wmt_trace_api_end(79, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_80(void *params) {
  void *ticket = wmt_trace_api_begin(80, params);
  int status = thunk_SM50FreeError(params);
  wmt_trace_api_end(80, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_81(void *params) {
  void *ticket = wmt_trace_api_begin(81, params);
  int status = thunk_SM50CompileGeometryPipelineVertex(params);
  wmt_trace_api_end(81, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_82(void *params) {
  void *ticket = wmt_trace_api_begin(82, params);
  int status = thunk_SM50CompileGeometryPipelineGeometry(params);
  wmt_trace_api_end(82, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_84(void *params) {
  void *ticket = wmt_trace_api_begin(84, params);
  int status = thunk_SM50CompileTessellationPipelineHull(params);
  wmt_trace_api_end(84, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_85(void *params) {
  void *ticket = wmt_trace_api_begin(85, params);
  int status = thunk_SM50CompileTessellationPipelineDomain(params);
  wmt_trace_api_end(85, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_86(void *params) {
  void *ticket = wmt_trace_api_begin(86, params);
  int status = _MTLCommandEncoder_setLabel(params);
  wmt_trace_api_end(86, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_87(void *params) {
  void *ticket = wmt_trace_api_begin(87, params);
  int status = _MTLDevice_setShouldMaximizeConcurrentCompilation(params);
  wmt_trace_api_end(87, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_88(void *params) {
  void *ticket = wmt_trace_api_begin(88, params);
  int status = thunk_SM50GetArgumentsInfo(params);
  wmt_trace_api_end(88, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_89(void *params) {
  void *ticket = wmt_trace_api_begin(89, params);
  int status = _MTLCommandBuffer_error(params);
  wmt_trace_api_end(89, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_90(void *params) {
  void *ticket = wmt_trace_api_begin(90, params);
  int status = _MTLCommandBuffer_logs(params);
  wmt_trace_api_end(90, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_91(void *params) {
  void *ticket = wmt_trace_api_begin(91, params);
  int status = _MTLLogContainer_enumerate(params);
  wmt_trace_api_end(91, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_92(void *params) {
  void *ticket = wmt_trace_api_begin(92, params);
  int status = _CGColorSpace_checkColorSpaceSupported(params);
  wmt_trace_api_end(92, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_93(void *params) {
  void *ticket = wmt_trace_api_begin(93, params);
  int status = _MetalLayer_setColorSpace(params);
  wmt_trace_api_end(93, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_94(void *params) {
  void *ticket = wmt_trace_api_begin(94, params);
  int status = _WMTGetPrimaryDisplayId(params);
  wmt_trace_api_end(94, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_95(void *params) {
  void *ticket = wmt_trace_api_begin(95, params);
  int status = _WMTGetSecondaryDisplayId(params);
  wmt_trace_api_end(95, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_96(void *params) {
  void *ticket = wmt_trace_api_begin(96, params);
  int status = _WMTGetDisplayDescription(params);
  wmt_trace_api_end(96, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_97(void *params) {
  void *ticket = wmt_trace_api_begin(97, params);
  int status = _MetalLayer_getEDRValue(params);
  wmt_trace_api_end(97, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_98(void *params) {
  void *ticket = wmt_trace_api_begin(98, params);
  int status = _MTLLibrary_newFunctionWithConstants(params);
  wmt_trace_api_end(98, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_99(void *params) {
  void *ticket = wmt_trace_api_begin(99, params);
  int status = _WMTQueryDisplaySetting(params);
  wmt_trace_api_end(99, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_100(void *params) {
  void *ticket = wmt_trace_api_begin(100, params);
  int status = _WMTUpdateDisplaySetting(params);
  wmt_trace_api_end(100, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_101(void *params) {
  void *ticket = wmt_trace_api_begin(101, params);
  int status = _WMTQueryDisplaySettingForLayer(params);
  wmt_trace_api_end(101, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_102(void *params) {
  void *ticket = wmt_trace_api_begin(102, params);
  int status = _MTLCommandBuffer_encodeWaitForEvent(params);
  wmt_trace_api_end(102, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_103(void *params) {
  void *ticket = wmt_trace_api_begin(103, params);
  int status = _MTLSharedEvent_signalValue(params);
  wmt_trace_api_end(103, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_104(void *params) {
  void *ticket = wmt_trace_api_begin(104, params);
  int status = _MTLSharedEvent_setWin32EventAtValue(params);
  wmt_trace_api_end(104, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_105(void *params) {
  void *ticket = wmt_trace_api_begin(105, params);
  int status = _MTLDevice_newFence(params);
  wmt_trace_api_end(105, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_106(void *params) {
  void *ticket = wmt_trace_api_begin(106, params);
  int status = _MTLDevice_newEvent(params);
  wmt_trace_api_end(106, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_107(void *params) {
  void *ticket = wmt_trace_api_begin(107, params);
  int status = _MTLBuffer_updateContents(params);
  wmt_trace_api_end(107, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_108(void *params) {
  void *ticket = wmt_trace_api_begin(108, params);
  int status = _SharedEventListener_create(params);
  wmt_trace_api_end(108, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_109(void *params) {
  void *ticket = wmt_trace_api_begin(109, params);
  int status = _SharedEventListener_start(params);
  wmt_trace_api_end(109, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_110(void *params) {
  void *ticket = wmt_trace_api_begin(110, params);
  int status = _SharedEventListener_destroy(params);
  wmt_trace_api_end(110, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_111(void *params) {
  void *ticket = wmt_trace_api_begin(111, params);
  int status = _WMTGetOSVersion(params);
  wmt_trace_api_end(111, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_112(void *params) {
  void *ticket = wmt_trace_api_begin(112, params);
  int status = _MTLDevice_newBinaryArchive(params);
  wmt_trace_api_end(112, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_113(void *params) {
  void *ticket = wmt_trace_api_begin(113, params);
  int status = _MTLBinaryArchive_serialize(params);
  wmt_trace_api_end(113, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_114(void *params) {
  void *ticket = wmt_trace_api_begin(114, params);
  int status = _DispatchData_alloc_init(params);
  wmt_trace_api_end(114, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_115(void *params) {
  void *ticket = wmt_trace_api_begin(115, params);
  int status = _CacheReader_alloc_init(params);
  wmt_trace_api_end(115, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_116(void *params) {
  void *ticket = wmt_trace_api_begin(116, params);
  int status = _CacheReader_get(params);
  wmt_trace_api_end(116, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_117(void *params) {
  void *ticket = wmt_trace_api_begin(117, params);
  int status = _CacheWriter_alloc_init(params);
  wmt_trace_api_end(117, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_118(void *params) {
  void *ticket = wmt_trace_api_begin(118, params);
  int status = _CacheWriter_set(params);
  wmt_trace_api_end(118, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_119(void *params) {
  void *ticket = wmt_trace_api_begin(119, params);
  int status = _WMTSetMetalShaderCachePath(params);
  wmt_trace_api_end(119, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_120(void *params) {
  void *ticket = wmt_trace_api_begin(120, params);
  int status = _MTLDevice_newSharedTexture(params);
  wmt_trace_api_end(120, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_121(void *params) {
  void *ticket = wmt_trace_api_begin(121, params);
  int status = _WMTBootstrapRegister(params);
  wmt_trace_api_end(121, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_122(void *params) {
  void *ticket = wmt_trace_api_begin(122, params);
  int status = _WMTBootstrapLookUp(params);
  wmt_trace_api_end(122, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_123(void *params) {
  void *ticket = wmt_trace_api_begin(123, params);
  int status = _MTLSharedEvent_createMachPort(params);
  wmt_trace_api_end(123, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_124(void *params) {
  void *ticket = wmt_trace_api_begin(124, params);
  int status = _MTLDevice_newSharedEventWithMachPort(params);
  wmt_trace_api_end(124, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_125(void *params) {
  void *ticket = wmt_trace_api_begin(125, params);
  int status = _MTLDevice_registryID(params);
  wmt_trace_api_end(125, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_126(void *params) {
  void *ticket = wmt_trace_api_begin(126, params);
  int status = _MTLSharedEvent_waitUntilSignaledValue(params);
  wmt_trace_api_end(126, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_127(void *params) {
  void *ticket = wmt_trace_api_begin(127, params);
  int status = _MTLCounterSampleBuffer_newTimestampBuffer(params);
  wmt_trace_api_end(127, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_128(void *params) {
  void *ticket = wmt_trace_api_begin(128, params);
  int status = _MTLCounterSampleBuffer_resolveCounterRange(params);
  wmt_trace_api_end(128, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_129(void *params) {
  void *ticket = wmt_trace_api_begin(129, params);
  int status = _MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(params);
  wmt_trace_api_end(129, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_130(void *params) {
  void *ticket = wmt_trace_api_begin(130, params);
  int status = _MTLCommandBuffer_property(params);
  wmt_trace_api_end(130, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_131(void *params) {
  void *ticket = wmt_trace_api_begin(131, params);
  int status = _MTLDevice_newTileRenderPipelineState(params);
  wmt_trace_api_end(131, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_132(void *params) {
  void *ticket = wmt_trace_api_begin(132, params);
  int status = _MTLCommandBuffer_scheduleFrameDump(params);
  wmt_trace_api_end(132, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_133(void *params) {
  void *ticket = wmt_trace_api_begin(133, params);
  int status = _MTLBuffer_traceFields(params);
  wmt_trace_api_end(133, params, status, ticket);
  return status;
}
static int wmt_trace_api_dispatch_134(void *params) {
  void *ticket = wmt_trace_api_begin(134, params);
  int status = _MTLBuffer_traceOwnership(params);
  wmt_trace_api_end(134, params, status, ticket);
  return status;
}
const void *__wine_unix_call_funcs[] = {
  &wmt_trace_api_dispatch_0,
  &wmt_trace_api_dispatch_1,
  &wmt_trace_api_dispatch_2,
  &wmt_trace_api_dispatch_3,
  &wmt_trace_api_dispatch_4,
  &wmt_trace_api_dispatch_5,
  &wmt_trace_api_dispatch_6,
  &wmt_trace_api_dispatch_7,
  &wmt_trace_api_dispatch_8,
  &wmt_trace_api_dispatch_9,
  &wmt_trace_api_dispatch_10,
  &wmt_trace_api_dispatch_11,
  &wmt_trace_api_dispatch_12,
  &wmt_trace_api_dispatch_13,
  &wmt_trace_api_dispatch_14,
  &wmt_trace_api_dispatch_15,
  &wmt_trace_api_dispatch_16,
  &wmt_trace_api_dispatch_17,
  &wmt_trace_api_dispatch_18,
  &wmt_trace_api_dispatch_19,
  &wmt_trace_api_dispatch_20,
  &wmt_trace_api_dispatch_21,
  &wmt_trace_api_dispatch_22,
  &wmt_trace_api_dispatch_23,
  &wmt_trace_api_dispatch_24,
  &wmt_trace_api_dispatch_25,
  &wmt_trace_api_dispatch_26,
  &wmt_trace_api_dispatch_27,
  &wmt_trace_api_dispatch_28,
  &wmt_trace_api_dispatch_29,
  &wmt_trace_api_dispatch_30,
  &wmt_trace_api_dispatch_31,
  &wmt_trace_api_dispatch_32,
  &wmt_trace_api_dispatch_33,
  &wmt_trace_api_dispatch_34,
  &wmt_trace_api_dispatch_35,
  &wmt_trace_api_dispatch_36,
  &wmt_trace_api_dispatch_37,
  &wmt_trace_api_dispatch_38,
  &wmt_trace_api_dispatch_39,
  &wmt_trace_api_dispatch_40,
  &wmt_trace_api_dispatch_41,
  &wmt_trace_api_dispatch_42,
  &wmt_trace_api_dispatch_43,
  &wmt_trace_api_dispatch_44,
  &wmt_trace_api_dispatch_45,
  &wmt_trace_api_dispatch_46,
  &wmt_trace_api_dispatch_47,
  &wmt_trace_api_dispatch_48,
  &wmt_trace_api_dispatch_49,
  &wmt_trace_api_dispatch_50,
  &wmt_trace_api_dispatch_51,
  &wmt_trace_api_dispatch_52,
  &wmt_trace_api_dispatch_53,
  &wmt_trace_api_dispatch_54,
  &wmt_trace_api_dispatch_55,
  &wmt_trace_api_dispatch_56,
  &wmt_trace_api_dispatch_57,
  &wmt_trace_api_dispatch_58,
  &wmt_trace_api_dispatch_59,
  &wmt_trace_api_dispatch_60,
  &wmt_trace_api_dispatch_61,
  &wmt_trace_api_dispatch_62,
  &wmt_trace_api_dispatch_63,
  &wmt_trace_api_dispatch_64,
  &wmt_trace_api_dispatch_65,
  &wmt_trace_api_dispatch_66,
  &wmt_trace_api_dispatch_67,
  &wmt_trace_api_dispatch_68,
  &wmt_trace_api_dispatch_69,
  &wmt_trace_api_dispatch_70,
  &wmt_trace_api_dispatch_71,
  &wmt_trace_api_dispatch_72,
  &wmt_trace_api_dispatch_73,
  &wmt_trace_api_dispatch_74,
  &wmt_trace_api_dispatch_75,
  &wmt_trace_api_dispatch_76,
  &wmt_trace_api_dispatch_77,
  &wmt_trace_api_dispatch_78,
  &wmt_trace_api_dispatch_79,
  &wmt_trace_api_dispatch_80,
  &wmt_trace_api_dispatch_81,
  &wmt_trace_api_dispatch_82,
  NULL,
  &wmt_trace_api_dispatch_84,
  &wmt_trace_api_dispatch_85,
  &wmt_trace_api_dispatch_86,
  &wmt_trace_api_dispatch_87,
  &wmt_trace_api_dispatch_88,
  &wmt_trace_api_dispatch_89,
  &wmt_trace_api_dispatch_90,
  &wmt_trace_api_dispatch_91,
  &wmt_trace_api_dispatch_92,
  &wmt_trace_api_dispatch_93,
  &wmt_trace_api_dispatch_94,
  &wmt_trace_api_dispatch_95,
  &wmt_trace_api_dispatch_96,
  &wmt_trace_api_dispatch_97,
  &wmt_trace_api_dispatch_98,
  &wmt_trace_api_dispatch_99,
  &wmt_trace_api_dispatch_100,
  &wmt_trace_api_dispatch_101,
  &wmt_trace_api_dispatch_102,
  &wmt_trace_api_dispatch_103,
  &wmt_trace_api_dispatch_104,
  &wmt_trace_api_dispatch_105,
  &wmt_trace_api_dispatch_106,
  &wmt_trace_api_dispatch_107,
  &wmt_trace_api_dispatch_108,
  &wmt_trace_api_dispatch_109,
  &wmt_trace_api_dispatch_110,
  &wmt_trace_api_dispatch_111,
  &wmt_trace_api_dispatch_112,
  &wmt_trace_api_dispatch_113,
  &wmt_trace_api_dispatch_114,
  &wmt_trace_api_dispatch_115,
  &wmt_trace_api_dispatch_116,
  &wmt_trace_api_dispatch_117,
  &wmt_trace_api_dispatch_118,
  &wmt_trace_api_dispatch_119,
  &wmt_trace_api_dispatch_120,
  &wmt_trace_api_dispatch_121,
  &wmt_trace_api_dispatch_122,
  &wmt_trace_api_dispatch_123,
  &wmt_trace_api_dispatch_124,
  &wmt_trace_api_dispatch_125,
  &wmt_trace_api_dispatch_126,
  &wmt_trace_api_dispatch_127,
  &wmt_trace_api_dispatch_128,
  &wmt_trace_api_dispatch_129,
  &wmt_trace_api_dispatch_130,
  &wmt_trace_api_dispatch_131,
  &wmt_trace_api_dispatch_132,
  &wmt_trace_api_dispatch_133,
  &wmt_trace_api_dispatch_134,
};
