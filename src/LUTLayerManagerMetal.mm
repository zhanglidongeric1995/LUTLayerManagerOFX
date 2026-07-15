#import <Metal/Metal.h>

#include "LUTLayerManagerMetal.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

const char *kMetalKernelSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

enum IntParam {
  RenderX = 0,
  RenderY,
  RenderWidth,
  RenderHeight,
  SourceX1,
  SourceY1,
  SourceX2,
  SourceY2,
  DestinationX1,
  DestinationY1,
  SourceRowBytes,
  DestinationRowBytes,
  SourceComponents,
  DestinationComponents,
  PixelDepth,
  HasSource,
  LutType,
  LutSize,
  ShaperSize,
  LutOffset,
  NodeTransfer,
  InputTransfer,
  OutputTransfer,
  ReturnToNode,
  PrimaryMixIsDefault,
  HueActive,
  PurityActive,
  DensityActive,
  DirectLutOnly,
  NodeToInputIdentity,
  InputToOutputIdentity,
  OutputToNodeIdentity,
};

enum FloatParam {
  DomainMinimum = 0,
  DomainMaximum = 3,
  LumaWeights = 6,
  LumaStrength = 9,
  ColorStrength,
  ShadowStrength,
  MidtoneStrength,
  HighlightStrength,
  NeutralBias,
  BlackLevel,
  WhiteRolloff,
  ColorPurity,
  DensityGain,
  HueMatrix,
  NodeToInputMatrix = 28,
  InputToOutputMatrix = 37,
  OutputToNodeMatrix = 46,
  NodeCurve = 55,
  InputCurve = 64,
  OutputCurve = 73,
};

float readNormalized(const device uchar *bytes, ulong offset, int depth) {
  if (depth == 0) {
    return float(bytes[offset]) / 255.0f;
  }
  if (depth == 1) {
    return float(*(reinterpret_cast<const device ushort *>(bytes + offset))) / 65535.0f;
  }
  return *(reinterpret_cast<const device float *>(bytes + offset));
}

void writeNormalized(device uchar *bytes, ulong offset, int depth, float value) {
  if (depth == 0) {
    bytes[offset] = uchar(round(clamp(value, 0.0f, 1.0f) * 255.0f));
  } else if (depth == 1) {
    *(reinterpret_cast<device ushort *>(bytes + offset)) =
      ushort(round(clamp(value, 0.0f, 1.0f) * 65535.0f));
  } else {
    *(reinterpret_cast<device float *>(bytes + offset)) = value;
  }
}

float3 multiplyMatrix(float3 value, constant float *matrix) {
  return float3(
    matrix[0] * value.x + matrix[1] * value.y + matrix[2] * value.z,
    matrix[3] * value.x + matrix[4] * value.y + matrix[5] * value.z,
    matrix[6] * value.x + matrix[7] * value.y + matrix[8] * value.z
  );
}

float decodeTransfer(float value, int transfer, constant float *curve) {
  if (!isfinite(value)) {
    return 0.0f;
  }
  if (transfer == 0) {
    return value < 0.0f ? value : pow(value, 2.4f);
  }
  if (transfer == 1) {
    if (value <= curve[8]) {
      return (value - curve[7]) / curve[6];
    }
    return (pow(curve[0], (value - curve[4]) / curve[3]) - curve[2]) / curve[1];
  }
  if (transfer == 2) {
    const float pivot = 0.092864125f;
    const float slope = 0.24136077f;
    const float gain = 87.099375f;
    const float magnitude = value < pivot
      ? -(pow(10.0f, (pivot - value) / slope) - 1.0f) / gain
      : (pow(10.0f, (value - pivot) / slope) - 1.0f) / gain;
    return magnitude * 0.9f;
  }
  if (value <= 0.14f) {
    return (value - 0.0929f) / 6.025f;
  }
  return (pow(10.0f, 3.89616f * value - 2.27752f) - 0.0108f) / 0.9892f;
}

float encodeTransfer(float value, int transfer, constant float *curve) {
  if (!isfinite(value)) {
    return 0.0f;
  }
  if (transfer == 0) {
    return value < 0.0f ? value : pow(value, 1.0f / 2.4f);
  }
  if (transfer == 1) {
    if (value <= curve[5]) {
      return curve[6] * value + curve[7];
    }
    const float argument = max(curve[1] * value + curve[2], 1.17549435e-38f);
    return curve[3] * (log(argument) / log(curve[0])) + curve[4];
  }
  if (transfer == 2) {
    const float pivot = 0.092864125f;
    const float slope = 0.24136077f;
    const float gain = 87.099375f;
    const float normalized = value / 0.9f;
    const float encoded = slope * log10(1.0f + abs(normalized) * gain);
    return normalized < 0.0f ? pivot - encoded : pivot + encoded;
  }
  if (value <= 0.0078f) {
    return 6.025f * value + 0.0929f;
  }
  const float argument = max(value * 0.9892f + 0.0108f, 1.17549435e-38f);
  return log10(argument) * 0.256663f + 0.584555f;
}

float3 decodeTransfer(float3 value, int transfer, constant float *curve) {
  return float3(
    decodeTransfer(value.x, transfer, curve),
    decodeTransfer(value.y, transfer, curve),
    decodeTransfer(value.z, transfer, curve)
  );
}

float3 encodeTransfer(float3 value, int transfer, constant float *curve) {
  return float3(
    encodeTransfer(value.x, transfer, curve),
    encodeTransfer(value.y, transfer, curve),
    encodeTransfer(value.z, transfer, curve)
  );
}

float3 transformColor(float3 value,
                      bool identity,
                      int sourceTransfer,
                      int destinationTransfer,
                      constant float *sourceCurve,
                      constant float *destinationCurve,
                      constant float *matrix) {
  if (identity) {
    return value;
  }
  return encodeTransfer(multiplyMatrix(decodeTransfer(value, sourceTransfer, sourceCurve), matrix),
                        destinationTransfer,
                        destinationCurve);
}

float sample1D(const device float4 *lut, int resolution, float value, int component, int offset) {
  const float x = clamp(value, 0.0f, 1.0f) * float(resolution - 1);
  const int lower = int(floor(x));
  const int upper = min(lower + 1, resolution - 1);
  const float fraction = x - float(lower);
  return mix(lut[offset + lower][component], lut[offset + upper][component], fraction);
}

float3 sample3D(const device float4 *lut, int size, int offset, float3 value) {
  const float3 position = clamp(value, 0.0f, 1.0f) * float(size - 1);
  const int3 lower = int3(floor(position));
  const int3 upper = min(lower + int3(1), int3(size - 1));
  const float3 fraction = position - float3(lower);
  const int plane = size * size;
  const int i000 = offset + lower.x + lower.y * size + lower.z * plane;
  const int i100 = offset + upper.x + lower.y * size + lower.z * plane;
  const int i010 = offset + lower.x + upper.y * size + lower.z * plane;
  const int i110 = offset + upper.x + upper.y * size + lower.z * plane;
  const int i001 = offset + lower.x + lower.y * size + upper.z * plane;
  const int i101 = offset + upper.x + lower.y * size + upper.z * plane;
  const int i011 = offset + lower.x + upper.y * size + upper.z * plane;
  const int i111 = offset + upper.x + upper.y * size + upper.z * plane;
  const float3 c00 = mix(lut[i000].xyz, lut[i100].xyz, fraction.x);
  const float3 c10 = mix(lut[i010].xyz, lut[i110].xyz, fraction.x);
  const float3 c01 = mix(lut[i001].xyz, lut[i101].xyz, fraction.x);
  const float3 c11 = mix(lut[i011].xyz, lut[i111].xyz, fraction.x);
  return mix(mix(c00, c10, fraction.y), mix(c01, c11, fraction.y), fraction.z);
}

float3 applyLut(float3 rgb,
                const device float4 *lut,
                constant int *integers,
                constant float *floats) {
  const int type = integers[LutType];
  if (type == 0) {
    return rgb;
  }
  const float3 minimum = float3(floats[DomainMinimum], floats[DomainMinimum + 1], floats[DomainMinimum + 2]);
  const float3 maximum = float3(floats[DomainMaximum], floats[DomainMaximum + 1], floats[DomainMaximum + 2]);
  float3 normalized;
  for (int component = 0; component < 3; ++component) {
    const float span = maximum[component] - minimum[component];
    normalized[component] = abs(span) < 1.0e-12f
      ? clamp(rgb[component], 0.0f, 1.0f)
      : clamp((rgb[component] - minimum[component]) / span, 0.0f, 1.0f);
  }

  if (type == 1 || type == 3) {
    const int resolution = type == 1 ? integers[LutSize] : integers[ShaperSize];
    const float3 shaped = float3(
      sample1D(lut, resolution, normalized.x, 0, 0),
      sample1D(lut, resolution, normalized.y, 1, 0),
      sample1D(lut, resolution, normalized.z, 2, 0)
    );
    if (type == 1) {
      return shaped;
    }
    return sample3D(lut, integers[LutSize], integers[LutOffset], shaped);
  }
  return sample3D(lut, integers[LutSize], integers[LutOffset], normalized);
}

float smoothMask(float edge0, float edge1, float value) {
  const float t = clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float3 processColor(float3 source,
                    const device float4 *lut,
                    constant int *integers,
                    constant float *floats) {
  if (integers[LutType] == 0) {
    return source;
  }
  if (integers[DirectLutOnly] != 0) {
    return applyLut(source, lut, integers, floats);
  }

  const float3 lutInput = transformColor(source,
                                         integers[NodeToInputIdentity] != 0,
                                         integers[NodeTransfer],
                                         integers[InputTransfer],
                                         floats + NodeCurve,
                                         floats + InputCurve,
                                         floats + NodeToInputMatrix);
  const float3 lutRgb = applyLut(lutInput, lut, integers, floats);
  float3 result = lutRgb;

  if (integers[PrimaryMixIsDefault] == 0) {
    const float3 reference = transformColor(lutInput,
                                            integers[InputToOutputIdentity] != 0,
                                            integers[InputTransfer],
                                            integers[OutputTransfer],
                                            floats + InputCurve,
                                            floats + OutputCurve,
                                            floats + InputToOutputMatrix);
    const float3 weights = float3(floats[LumaWeights], floats[LumaWeights + 1], floats[LumaWeights + 2]);
    const float sourceY = dot(reference, weights);
    const float lutY = dot(lutRgb, weights);
    const float3 sourceChroma = reference - sourceY;
    const float3 lutChroma = lutRgb - lutY;
    const float y = clamp(sourceY, 0.0f, 1.0f);
    const float shadowMask = 1.0f - smoothMask(0.18f, 0.45f, y);
    const float highlightMask = smoothMask(0.55f, 0.82f, y);
    const float midMask = clamp(1.0f - abs(y - 0.5f) * 2.0f, 0.0f, 1.0f);
    const float maskSum = max(1.0e-6f, shadowMask + midMask + highlightMask);
    const float zoneStrength =
      (shadowMask * floats[ShadowStrength] +
       midMask * floats[MidtoneStrength] +
       highlightMask * floats[HighlightStrength]) / maskSum;
    const float saturation = max(max(abs(sourceChroma.x), abs(sourceChroma.y)), abs(sourceChroma.z));
    const float neutralMask = 1.0f - smoothMask(0.035f, 0.18f, saturation);
    const float neutralScale = max(0.0f, 1.0f + (floats[NeutralBias] - 1.0f) * neutralMask);
    float lumaDelta = (lutY - sourceY) * floats[LumaStrength];
    lumaDelta *= 1.0f + (floats[BlackLevel] - 1.0f) * shadowMask;
    lumaDelta *= 1.0f + (floats[WhiteRolloff] - 1.0f) * highlightMask;
    const float3 chromaDelta = (lutChroma - sourceChroma) * (floats[ColorStrength] * neutralScale);
    const float outputY = sourceY + lumaDelta * zoneStrength;
    const float3 outputChroma = sourceChroma + chromaDelta * zoneStrength;
    result = outputY + outputChroma;
  }

  const float3 weights = float3(floats[LumaWeights], floats[LumaWeights + 1], floats[LumaWeights + 2]);
  if (integers[HueActive] != 0) {
    const float before = dot(result, weights);
    const float3 rotated = multiplyMatrix(result, floats + HueMatrix);
    result = rotated + (before - dot(rotated, weights));
  }
  if (integers[PurityActive] != 0) {
    const float luma = dot(result, weights);
    result = luma + (result - luma) * floats[ColorPurity];
  }
  if (integers[DensityActive] != 0) {
    result *= floats[DensityGain];
  }

  if (integers[ReturnToNode] != 0) {
    result = transformColor(result,
                            integers[OutputToNodeIdentity] != 0,
                            integers[OutputTransfer],
                            integers[NodeTransfer],
                            floats + OutputCurve,
                            floats + NodeCurve,
                            floats + OutputToNodeMatrix);
  }
  return result;
}

kernel void LUTLayerManagerKernel(const device uchar *source [[buffer(0)]],
                                  device uchar *destination [[buffer(1)]],
                                  const device float4 *lut [[buffer(2)]],
                                  constant int *integers [[buffer(3)]],
                                  constant float *floats [[buffer(4)]],
                                  uint2 id [[thread_position_in_grid]]) {
  if (id.x >= uint(integers[RenderWidth]) || id.y >= uint(integers[RenderHeight])) {
    return;
  }
  const int x = integers[RenderX] + int(id.x);
  const int y = integers[RenderY] + int(id.y);
  const int depth = integers[PixelDepth];
  const int bytesPerComponent = depth == 0 ? 1 : (depth == 1 ? 2 : 4);
  float4 sourcePixel = float4(0.0f, 0.0f, 0.0f, 1.0f);
  if (integers[HasSource] != 0 &&
      x >= integers[SourceX1] && x < integers[SourceX2] &&
      y >= integers[SourceY1] && y < integers[SourceY2]) {
    const ulong sourceOffset = ulong(y - integers[SourceY1]) * ulong(integers[SourceRowBytes]) +
      ulong(x - integers[SourceX1]) * ulong(integers[SourceComponents] * bytesPerComponent);
    sourcePixel.x = readNormalized(source, sourceOffset, depth);
    sourcePixel.y = readNormalized(source, sourceOffset + bytesPerComponent, depth);
    sourcePixel.z = readNormalized(source, sourceOffset + bytesPerComponent * 2, depth);
    if (integers[SourceComponents] == 4) {
      sourcePixel.w = readNormalized(source, sourceOffset + bytesPerComponent * 3, depth);
    }
  }

  const float3 result = processColor(sourcePixel.xyz, lut, integers, floats);
  const ulong destinationOffset = ulong(y - integers[DestinationY1]) * ulong(integers[DestinationRowBytes]) +
    ulong(x - integers[DestinationX1]) * ulong(integers[DestinationComponents] * bytesPerComponent);
  writeNormalized(destination, destinationOffset, depth, result.x);
  writeNormalized(destination, destinationOffset + bytesPerComponent, depth, result.y);
  writeNormalized(destination, destinationOffset + bytesPerComponent * 2, depth, result.z);
  if (integers[DestinationComponents] == 4) {
    writeNormalized(destination, destinationOffset + bytesPerComponent * 3, depth, sourcePixel.w);
  }
}
)METAL";

struct MetalFloat4 {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 0.0f;
};

struct MetalLutCacheEntry {
  std::string key;
  id<MTLBuffer> buffer = nil;
  uint64_t lastUsed = 0;
};

std::mutex gPipelineMutex;
id<MTLDevice> gPipelineDevice = nil;
id<MTLComputePipelineState> gPipeline = nil;

std::mutex gLutMutex;
std::vector<MetalLutCacheEntry> gLutCache;
uint64_t gLutUseCounter = 0;

id<MTLComputePipelineState> pipelineForQueue(id<MTLCommandQueue> queue) {
  if (!queue) {
    return nil;
  }
  id<MTLDevice> device = queue.device;
  std::lock_guard<std::mutex> lock(gPipelineMutex);
  if (gPipeline && gPipelineDevice == device) {
    return gPipeline;
  }

  NSError *error = nil;
  MTLCompileOptions *options = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  options.fastMathEnabled = YES;
#pragma clang diagnostic pop
  id<MTLLibrary> library = [device newLibraryWithSource:@(kMetalKernelSource)
                                                 options:options
                                                   error:&error];
  if (!library) {
    fprintf(stderr, "LUTLayerManager Metal library failed: %s\n",
            error.localizedDescription.UTF8String ?: "unknown error");
    return nil;
  }
  id<MTLFunction> function = [library newFunctionWithName:@"LUTLayerManagerKernel"];
  if (!function) {
    fprintf(stderr, "LUTLayerManager Metal kernel was not found\n");
    return nil;
  }
  id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
  if (!pipeline) {
    fprintf(stderr, "LUTLayerManager Metal pipeline failed: %s\n",
            error.localizedDescription.UTF8String ?: "unknown error");
    return nil;
  }
  gPipelineDevice = device;
  gPipeline = pipeline;
  return gPipeline;
}

id<MTLBuffer> lutBufferForRequest(id<MTLDevice> device, const MetalRenderRequest &request) {
  const std::string key = request.lutRevision + "|" +
                          std::to_string(reinterpret_cast<uintptr_t>((__bridge void *)device));
  std::lock_guard<std::mutex> lock(gLutMutex);
  for (MetalLutCacheEntry &entry : gLutCache) {
    if (entry.key == key) {
      entry.lastUsed = ++gLutUseCounter;
      return entry.buffer;
    }
  }

  std::vector<MetalFloat4> samples;
  samples.reserve(request.lut.shaperValueCount + request.lut.valueCount);
  const auto append = [&samples](const double *values, size_t count) {
    if (!values) {
      return;
    }
    for (size_t i = 0; i < count; ++i) {
      samples.push_back({
        static_cast<float>(values[i * 3]),
        static_cast<float>(values[i * 3 + 1]),
        static_cast<float>(values[i * 3 + 2]),
        0.0f,
      });
    }
  };
  append(request.lut.shaperValues, request.lut.shaperValueCount);
  append(request.lut.values, request.lut.valueCount);
  if (samples.empty()) {
    samples.push_back({});
  }

  id<MTLBuffer> buffer = [device newBufferWithBytes:samples.data()
                                             length:samples.size() * sizeof(MetalFloat4)
                                            options:MTLResourceStorageModeShared];
  if (!buffer) {
    return nil;
  }

  constexpr size_t kMaximumEntries = 6;
  if (gLutCache.size() >= kMaximumEntries) {
    auto oldest = std::min_element(gLutCache.begin(), gLutCache.end(),
                                   [](const MetalLutCacheEntry &a, const MetalLutCacheEntry &b) {
                                     return a.lastUsed < b.lastUsed;
                                   });
    gLutCache.erase(oldest);
  }
  gLutCache.push_back({key, buffer, ++gLutUseCounter});
  return buffer;
}

}  // namespace

bool runMetalRender(const MetalRenderRequest &request) {
  if (!request.commandQueue || !request.destinationBuffer ||
      request.integers[MetalRenderWidth] <= 0 || request.integers[MetalRenderHeight] <= 0) {
    return false;
  }

  id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)request.commandQueue;
  id<MTLComputePipelineState> pipeline = pipelineForQueue(queue);
  id<MTLBuffer> lutBuffer = lutBufferForRequest(queue.device, request);
  if (!pipeline || !lutBuffer) {
    return false;
  }

  id<MTLBuffer> source = request.sourceBuffer
    ? (__bridge id<MTLBuffer>)request.sourceBuffer
    : (__bridge id<MTLBuffer>)request.destinationBuffer;
  id<MTLBuffer> destination = (__bridge id<MTLBuffer>)request.destinationBuffer;
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  commandBuffer.label = @"LUT 分层管理";
  id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
  if (!encoder) {
    return false;
  }

  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:source offset:0 atIndex:0];
  [encoder setBuffer:destination offset:0 atIndex:1];
  [encoder setBuffer:lutBuffer offset:0 atIndex:2];
  [encoder setBytes:request.integers.data()
              length:request.integers.size() * sizeof(int32_t)
             atIndex:3];
  [encoder setBytes:request.floats.data()
              length:request.floats.size() * sizeof(float)
             atIndex:4];

  const NSUInteger threadWidth = std::max<NSUInteger>(1, pipeline.threadExecutionWidth);
  const NSUInteger threadHeight = std::max<NSUInteger>(1,
    std::min<NSUInteger>(16, pipeline.maxTotalThreadsPerThreadgroup / threadWidth));
  const MTLSize grid = MTLSizeMake(static_cast<NSUInteger>(request.integers[MetalRenderWidth]),
                                   static_cast<NSUInteger>(request.integers[MetalRenderHeight]),
                                   1);
  const MTLSize group = MTLSizeMake(threadWidth, threadHeight, 1);
  [encoder dispatchThreads:grid threadsPerThreadgroup:group];
  [encoder endEncoding];
  [commandBuffer commit];
  return true;
}

void resetMetalRenderCaches() {
  {
    std::lock_guard<std::mutex> lock(gLutMutex);
    gLutCache.clear();
    gLutUseCounter = 0;
  }
  {
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    gPipeline = nil;
    gPipelineDevice = nil;
  }
}
