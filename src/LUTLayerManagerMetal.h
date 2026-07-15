#pragma once

#ifdef __APPLE__

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

enum MetalIntParam : size_t {
  MetalRenderX = 0,
  MetalRenderY,
  MetalRenderWidth,
  MetalRenderHeight,
  MetalSourceX1,
  MetalSourceY1,
  MetalSourceX2,
  MetalSourceY2,
  MetalDestinationX1,
  MetalDestinationY1,
  MetalSourceRowBytes,
  MetalDestinationRowBytes,
  MetalSourceComponents,
  MetalDestinationComponents,
  MetalPixelDepth,
  MetalHasSource,
  MetalLutType,
  MetalLutSize,
  MetalShaperSize,
  MetalLutOffset,
  MetalNodeTransfer,
  MetalInputTransfer,
  MetalOutputTransfer,
  MetalReturnToNode,
  MetalPrimaryMixIsDefault,
  MetalHueActive,
  MetalPurityActive,
  MetalDensityActive,
  MetalDirectLutOnly,
  MetalNodeToInputIdentity,
  MetalInputToOutputIdentity,
  MetalOutputToNodeIdentity,
  MetalIntParamCount,
};

enum MetalFloatParam : size_t {
  MetalDomainMinimum = 0,
  MetalDomainMaximum = 3,
  MetalLumaWeights = 6,
  MetalLumaStrength = 9,
  MetalColorStrength,
  MetalShadowStrength,
  MetalMidtoneStrength,
  MetalHighlightStrength,
  MetalNeutralBias,
  MetalBlackLevel,
  MetalWhiteRolloff,
  MetalColorPurity,
  MetalDensityGain,
  MetalHueMatrix,
  MetalNodeToInputMatrix = 28,
  MetalInputToOutputMatrix = 37,
  MetalOutputToNodeMatrix = 46,
  MetalNodeCurve = 55,
  MetalInputCurve = 64,
  MetalOutputCurve = 73,
  MetalFloatParamCount = 82,
};

struct MetalLutView {
  const double *values = nullptr;
  size_t valueCount = 0;
  const double *shaperValues = nullptr;
  size_t shaperValueCount = 0;
};

struct MetalRenderRequest {
  void *commandQueue = nullptr;
  void *sourceBuffer = nullptr;
  void *destinationBuffer = nullptr;
  std::array<int32_t, MetalIntParamCount> integers{};
  std::array<float, MetalFloatParamCount> floats{};
  std::string lutRevision;
  MetalLutView lut;
};

bool runMetalRender(const MetalRenderRequest &request);
void resetMetalRenderCaches();

#endif
