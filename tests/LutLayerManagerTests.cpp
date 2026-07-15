#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

#include "../src/LUTLayerManager.cpp"

namespace {

bool nearlyEqual(double a, double b, double epsilon = 1e-9) {
  return std::abs(a - b) <= epsilon;
}

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void requireColor(const Vec3 &actual, const Vec3 &expected, const std::string &message) {
  require(nearlyEqual(actual.r, expected.r) && nearlyEqual(actual.g, expected.g) && nearlyEqual(actual.b, expected.b),
          message);
}

void requireColorNear(const Vec3 &actual, const Vec3 &expected, double epsilon, const std::string &message) {
  require(nearlyEqual(actual.r, expected.r, epsilon) &&
            nearlyEqual(actual.g, expected.g, epsilon) &&
            nearlyEqual(actual.b, expected.b, epsilon),
          message);
}

std::string temporaryCubePath(const std::string &name) {
  return "/tmp/lut_layer_manager_" + std::to_string(getpid()) + "_" + name + ".cube";
}

void writeTextFile(const std::string &path, const std::string &contents) {
  std::ofstream out(path, std::ios::trunc);
  require(static_cast<bool>(out), "Unable to create temporary LUT file");
  out << contents;
}

std::string identity3DSamples() {
  std::string samples;
  for (int b = 0; b < 2; ++b) {
    for (int g = 0; g < 2; ++g) {
      for (int r = 0; r < 2; ++r) {
        samples += std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + "\n";
      }
    }
  }
  return samples;
}

void testOneDimensionalLut() {
  const std::string path = temporaryCubePath("1d");
  writeTextFile(path,
                "# identity 1D LUT\n"
                "LUT_1D_SIZE 2\n"
                "0.0 0.0 0.0\n"
                "1.0 1.0 1.0\n");
  const LutData lut = parseCubeFile(path);
  std::remove(path.c_str());

  require(lut.valid(), "1D LUT should parse");
  requireColor(lut.apply({0.25, 0.50, 0.75}), {0.25, 0.50, 0.75}, "1D interpolation should be channel independent");
}

void testThreeDimensionalLut() {
  const std::string path = temporaryCubePath("3d");
  std::string contents = "LUT_3D_SIZE 2\n";
  for (int b = 0; b < 2; ++b) {
    for (int g = 0; g < 2; ++g) {
      for (int r = 0; r < 2; ++r) {
        contents += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " + std::to_string(1 - b) + "\n";
      }
    }
  }
  writeTextFile(path, contents);
  const LutData lut = parseCubeFile(path);
  std::remove(path.c_str());

  require(lut.valid(), "3D LUT should parse");
  requireColor(lut.apply({0.25, 0.50, 0.75}), {0.75, 0.50, 0.25}, "3D LUT should use trilinear interpolation");
}

void testCombinedOneDimensionalAndThreeDimensionalLut() {
  const std::string path = temporaryCubePath("combined");
  writeTextFile(path,
                "LUT_1D_SIZE 2\n"
                "LUT_3D_SIZE 2\n"
                "0.0 0.0 0.0\n"
                "0.5 0.5 0.5\n" +
                identity3DSamples());
  const LutData lut = parseCubeFile(path);
  std::remove(path.c_str());

  require(lut.valid(), "Combined 1D and 3D LUT should parse");
  requireColor(lut.apply({0.50, 0.50, 0.50}), {0.25, 0.25, 0.25},
               "The 1D shaper should run before the 3D cube");
}

void testFloatRangeIsPreserved() {
  require(nearlyEqual(writeNorm<float>(-0.25), -0.25), "Float output should preserve negative values");
  require(nearlyEqual(writeNorm<float>(1.25), 1.25), "Float output should preserve headroom");
}

void testTransferFunctionRoundTrips() {
  const std::vector<double> linearSamples = {-0.01, 0.0, 0.001, 0.01, 0.18, 1.0, 2.0};
  for (int colorSpace = 0; colorSpace < kColorSpaceCount; ++colorSpace) {
    const ColorSpaceSpec &spec = colorSpaceSpec(colorSpace);
    const double epsilon = colorSpace == static_cast<int>(ColorSpaceId::DjiDLog) ? 1e-6 : 1e-9;
    for (double sample : linearSamples) {
      const double encoded = encodeTransfer(sample, spec);
      const double decoded = decodeTransfer(encoded, spec);
      require(nearlyEqual(decoded, sample, epsilon),
              "Transfer function should round-trip in color space " + std::to_string(colorSpace));
    }
  }
}

void testColorSpaceRoundTrips() {
  const std::vector<Vec3> samples = {
    {0.18, 0.18, 0.18},
    {0.31, 0.42, 0.57},
    {0.72, 0.48, 0.23},
  };

  for (int source = 0; source < kColorSpaceCount; ++source) {
    for (int destination = 0; destination < kColorSpaceCount; ++destination) {
      for (const Vec3 &sample : samples) {
        const Vec3 converted = convertColorSpace(sample, source, destination);
        const Vec3 restored = convertColorSpace(converted, destination, source);
        const double epsilon = source == static_cast<int>(ColorSpaceId::DjiDLog) ||
                                   destination == static_cast<int>(ColorSpaceId::DjiDLog)
                                 ? 3e-6
                                 : 2e-8;
        requireColorNear(restored, sample, epsilon,
                         "Color-space conversion should round-trip from " + std::to_string(source) +
                           " through " + std::to_string(destination));
      }
    }
  }
}

void testOpenColorIOReferenceVectors() {
  // Generated with OpenColorIO 2.3.2 and
  // studio-config-v2.1.0_aces-v1.3_ocio-v2.3, converting to ACES2065-1.
  const Vec3 sample{0.18, 0.42, 0.73};
  const std::vector<Vec3> expected = {
    {0.138268262147903, 0.148361265659332, 0.423399955034256},
    {0.439878046512604, -0.212964609265327, 4.69903326034546},
    {2.8994677066803, 0.712593495845795, 26.1612148284912},
    {1.17540812492371, -0.615771651268005, 11.0305881500244},
    {0.748072862625122, -0.578400015830994, 9.42356586456299},
    {1.41045439243317, 0.969790101051331, 8.40870761871338},
    {0.361560642719269, -0.0691375583410263, 3.55023288726807},
    {0.416302770376205, 0.149644002318382, 3.54024028778076},
    {1.56682729721069, -0.775757730007172, 15.8358640670776},
    {0.439001679420471, -0.249659270048141, 5.47329998016357},
  };

  for (size_t colorSpace = 0; colorSpace < expected.size(); ++colorSpace) {
    const ColorSpaceSpec &spec = colorSpaceSpec(static_cast<int>(colorSpace));
    const Vec3 reference = multiply(spec.toReference, decodeTransfer(sample, spec));
    requireColorNear(reference, expected[colorSpace], 6e-5,
                     "Color transform should match the OpenColorIO reference for space " +
                       std::to_string(colorSpace));
  }
}

void testDjiOfficialReferenceValues() {
  const ColorSpaceSpec &dji = colorSpaceSpec(static_cast<int>(ColorSpaceId::DjiDLog));
  require(nearlyEqual(encodeTransfer(0.0, dji), 95.0 / 1023.0, 5e-5),
          "DJI D-Log black should match the official 10-bit reference");
  require(nearlyEqual(encodeTransfer(0.18, dji), 408.0 / 1023.0, 7e-5),
          "DJI D-Log 18 percent gray should match the official 10-bit reference");
  require(nearlyEqual(encodeTransfer(0.9, dji), 586.0 / 1023.0, 2e-4),
          "DJI D-Log 90 percent white should match the official 10-bit reference");

  const Vec3 linearDji{0.3, 0.4, 0.2};
  const ColorSpaceSpec &rec709 = colorSpaceSpec(static_cast<int>(ColorSpaceId::Rec709Gamma24));
  const Vec3 linearRec709 = multiply(rec709.fromReference, multiply(dji.toReference, linearDji));
  const Vec3 officialRec709 = {
    1.6747 * linearDji.r - 0.5798 * linearDji.g - 0.0949 * linearDji.b,
    -0.0981 * linearDji.r + 1.3340 * linearDji.g - 0.2359 * linearDji.b,
    -0.0410 * linearDji.r - 0.2430 * linearDji.g + 1.2840 * linearDji.b,
  };
  requireColorNear(linearRec709, officialRec709, 6e-5,
                   "DJI D-Gamut should match the official D-Gamut to Rec.709 matrix");
}

void testIdentityLutPreservesColorAcrossWorkingSpaces() {
  const std::string path = temporaryCubePath("color_space_identity");
  writeTextFile(path, "LUT_3D_SIZE 2\n" + identity3DSamples());
  const LutData lut = parseCubeFile(path);
  std::remove(path.c_str());

  RenderParams params;
  params.nodeColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  params.lutInputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  params.lutOutputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);

  const Vec3 source{0.336, 0.336, 0.336};
  const Vec3 result = applyColorManagedLayeredLut(source, lut, params);
  requireColorNear(result, source, 1e-4,
                   "An identity LUT should preserve color when converting into and out of its working space");
}

void testLutWorkingSpaceChangesNonIdentityResult() {
  const std::string path = temporaryCubePath("color_space_invert");
  std::string contents = "LUT_3D_SIZE 2\n";
  for (int b = 0; b < 2; ++b) {
    for (int g = 0; g < 2; ++g) {
      for (int r = 0; r < 2; ++r) {
        contents += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " + std::to_string(1 - b) + "\n";
      }
    }
  }
  writeTextFile(path, contents);
  const LutData lut = parseCubeFile(path);
  std::remove(path.c_str());

  RenderParams rec709Params;
  rec709Params.nodeColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  rec709Params.lutInputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  rec709Params.lutOutputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);

  RenderParams logC3Params = rec709Params;
  logC3Params.lutInputColorSpace = static_cast<int>(ColorSpaceId::ArriLogC3);
  logC3Params.lutOutputColorSpace = static_cast<int>(ColorSpaceId::ArriLogC3);

  const Vec3 source{0.39, 0.44, 0.51};
  const Vec3 rec709Result = applyColorManagedLayeredLut(source, lut, rec709Params);
  const Vec3 logC3Result = applyColorManagedLayeredLut(source, lut, logC3Params);
  const double difference = std::abs(rec709Result.r - logC3Result.r) +
                            std::abs(rec709Result.g - logC3Result.g) +
                            std::abs(rec709Result.b - logC3Result.b);
  require(difference > 0.01, "Changing the LUT working space should affect a non-identity LUT result");
}

void testTechnicalLutInputAndOutputSpaces() {
  const std::string path = temporaryCubePath("technical_lut");
  std::string contents = "LUT_3D_SIZE 2\n";
  for (int b = 0; b < 2; ++b) {
    for (int g = 0; g < 2; ++g) {
      for (int r = 0; r < 2; ++r) {
        contents += std::to_string(1 - r) + " " + std::to_string(1 - g) + " " + std::to_string(1 - b) + "\n";
      }
    }
  }
  writeTextFile(path, contents);
  const LutData lut = parseCubeFile(path);
  std::remove(path.c_str());

  RenderParams params;
  params.nodeColorSpace = static_cast<int>(ColorSpaceId::DjiDLog);
  params.lutInputColorSpace = static_cast<int>(ColorSpaceId::DjiDLog);
  params.lutOutputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  params.returnToNodeColorSpace = false;

  const Vec3 source{0.32, 0.41, 0.56};
  const Vec3 expectedLutOutput = lut.apply(source);
  const Vec3 keptOutput = applyColorManagedLayeredLut(source, lut, params);
  requireColorNear(keptOutput, expectedLutOutput, 1e-9,
                   "Technical LUT mode should preserve the declared LUT output encoding");

  params.returnToNodeColorSpace = true;
  const Vec3 returnedOutput = applyColorManagedLayeredLut(source, lut, params);
  const Vec3 expectedReturned = convertColorSpace(expectedLutOutput,
                                                  static_cast<int>(ColorSpaceId::Rec709Gamma24),
                                                  static_cast<int>(ColorSpaceId::DjiDLog));
  requireColorNear(returnedOutput, expectedReturned, 1e-4,
                   "Color-managed mode should convert the declared LUT output back to the node space");
}

LutData makeBenchmarkLut(int size) {
  LutData lut;
  lut.type = LutData::Type::Lut3D;
  lut.size = size;
  lut.values.reserve(cubeSampleCount(size));
  for (int b = 0; b < size; ++b) {
    for (int g = 0; g < size; ++g) {
      for (int r = 0; r < size; ++r) {
        const double rf = static_cast<double>(r) / (size - 1);
        const double gf = static_cast<double>(g) / (size - 1);
        const double bf = static_cast<double>(b) / (size - 1);
        lut.values.push_back({std::pow(rf, 0.92), std::pow(gf, 1.04), std::pow(bf, 1.08)});
      }
    }
  }
  return lut;
}

void testColorSpaceConversionToggle() {
  const LutData lut = makeBenchmarkLut(33);
  RenderParams enabledParams;
  enabledParams.colorSpaceConversionEnabled = true;
  enabledParams.nodeColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  enabledParams.lutInputColorSpace = static_cast<int>(ColorSpaceId::DjiDLog);
  enabledParams.lutOutputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  enabledParams.returnToNodeColorSpace = false;

  RenderParams disabledParams = enabledParams;
  disabledParams.colorSpaceConversionEnabled = false;
  const PreparedProcessor disabledProcessor = prepareProcessor(lut, disabledParams);

  require(disabledProcessor.nodeToLutInput.identity &&
            disabledProcessor.layer.inputToOutput.identity &&
            disabledProcessor.lutOutputToNode.identity,
          "Disabling color-space conversion should make every color transform an identity");
  require(disabledProcessor.returnToNodeColorSpace,
          "The disabled mode should remain in the current node space");
  require(disabledProcessor.directLutOnly,
          "Default layer controls with conversion disabled should use the direct LUT path");

  const Vec3 source{0.32, 0.41, 0.56};
  const Vec3 directResult = lut.apply(source);
  const Vec3 disabledResult = disabledProcessor.apply(source);
  requireColorNear(disabledResult, directResult, 1e-12,
                   "Disabled color-space conversion should apply the LUT directly to node pixels");

  const Vec3 enabledResult = prepareProcessor(lut, enabledParams).apply(source);
  const double enabledDifference = std::max({std::abs(enabledResult.r - directResult.r),
                                              std::abs(enabledResult.g - directResult.g),
                                              std::abs(enabledResult.b - directResult.b)});
  require(enabledDifference > 1e-4,
          "Enabling color-space conversion should honor the selected LUT spaces");
  require(acceleratedCacheKey("toggle-test", enabledParams, 65) !=
            acceleratedCacheKey("toggle-test", disabledParams, 65),
          "The accelerated cache key should include the color-space conversion toggle");
}

void testPreparedColorTransformsMatchReference() {
  const Vec3 sample{0.27, 0.43, 0.61};
  double maximumFastError = 0.0;
  for (int source = 0; source < kColorSpaceCount; ++source) {
    for (int destination = 0; destination < kColorSpaceCount; ++destination) {
      const PreparedColorTransform transform = prepareColorTransform(source, destination);
      const Vec3 expected = convertColorSpace(sample, source, destination);
      const Vec3 actual = transform.apply(sample);
      requireColorNear(actual, expected, 1e-11,
                       "Prepared color transform should match the reference conversion");

      for (int i = 0; i < 256; ++i) {
        const Vec3 value{
          -0.05 + 1.1 * static_cast<double>((i * 17) % 251) / 250.0,
          -0.05 + 1.1 * static_cast<double>((i * 37) % 241) / 240.0,
          -0.05 + 1.1 * static_cast<double>((i * 67) % 239) / 238.0,
        };
        const Vec3 exact = transform.apply(value);
        const Vec3 fast = transform.applyFast(value);
        maximumFastError = std::max({maximumFastError,
                                     std::abs(exact.r - fast.r),
                                     std::abs(exact.g - fast.g),
                                     std::abs(exact.b - fast.b)});
      }
    }
  }
  require(maximumFastError < 3e-4,
          "Fast transfer lookup should remain visually lossless; error was " +
            std::to_string(maximumFastError));
}

void testAcceleratedLutMatchesPreparedProcessor() {
  const LutData lut = makeBenchmarkLut(33);
  RenderParams params;
  params.nodeColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  params.lutInputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  params.lutOutputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  params.returnToNodeColorSpace = true;
  params.lumaStrength = 0.86;
  params.colorStrength = 1.12;
  params.shadowStrength = 0.9;
  params.midtoneStrength = 1.05;
  params.highlightStrength = 0.82;
  params.neutralBias = 0.93;
  params.colorPurity = 1.04;
  params.hueShift = 2.0;

  const PreparedProcessor processor = prepareProcessor(lut, params);
  const std::shared_ptr<const AcceleratedLut> accelerated = buildAcceleratedLut(processor, 65);
  require(accelerated && accelerated->valid(), "Accelerated LUT should build successfully");

  double maximumError = 0.0;
  for (int i = 0; i < 2000; ++i) {
    const Vec3 source{
      0.12 + 0.76 * static_cast<double>((i * 17) % 997) / 996.0,
      0.12 + 0.76 * static_cast<double>((i * 37) % 991) / 990.0,
      0.12 + 0.76 * static_cast<double>((i * 67) % 983) / 982.0,
    };
    const Vec3 exact = processor.apply(source);
    const Vec3 fast = accelerated->applyUnitCube(source);
    maximumError = std::max({maximumError,
                             std::abs(exact.r - fast.r),
                             std::abs(exact.g - fast.g),
                             std::abs(exact.b - fast.b)});
  }
  require(maximumError < 0.0002,
          "Accelerated LUT should stay visually close to the exact processor; error was " +
            std::to_string(maximumError));
  require(!isInsideUnitCube({-0.01, 0.5, 0.5}) && !isInsideUnitCube({0.5, 1.01, 0.5}),
          "Out-of-range float pixels should use the exact fallback path");

  RenderParams technicalParams;
  technicalParams.nodeColorSpace = static_cast<int>(ColorSpaceId::SonySLog3Cine);
  technicalParams.lutInputColorSpace = static_cast<int>(ColorSpaceId::SonySLog3Cine);
  technicalParams.lutOutputColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  technicalParams.returnToNodeColorSpace = false;
  technicalParams.lumaStrength = 0.779;
  const PreparedProcessor technicalProcessor = prepareProcessor(lut, technicalParams);
  require(!technicalProcessor.acceleratedLutIsSafe,
          "Technical output transforms should stay on the high-precision transfer lookup path");
}

#ifdef __APPLE__
void testMetalProcessorMatchesCpu() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    return;
  }
  id<MTLCommandQueue> queue = [device newCommandQueue];
  require(queue != nil, "Metal command queue should be available");

  constexpr int width = 64;
  constexpr int height = 32;
  std::vector<float> source(static_cast<size_t>(width * height * 4));
  for (int i = 0; i < width * height; ++i) {
    source[static_cast<size_t>(i * 4)] = 0.03f + 0.94f * static_cast<float>((i * 17) % 997) / 996.0f;
    source[static_cast<size_t>(i * 4 + 1)] = 0.03f + 0.94f * static_cast<float>((i * 37) % 991) / 990.0f;
    source[static_cast<size_t>(i * 4 + 2)] = 0.03f + 0.94f * static_cast<float>((i * 67) % 983) / 982.0f;
    source[static_cast<size_t>(i * 4 + 3)] = 0.25f + 0.75f * static_cast<float>(i % 31) / 30.0f;
  }
  id<MTLBuffer> sourceBuffer = [device newBufferWithBytes:source.data()
                                                     length:source.size() * sizeof(float)
                                                    options:MTLResourceStorageModeShared];
  id<MTLBuffer> destinationBuffer = [device newBufferWithLength:source.size() * sizeof(float)
                                                           options:MTLResourceStorageModeShared];
  require(sourceBuffer != nil && destinationBuffer != nil, "Metal image buffers should allocate");

  ImageInfo sourceInfo;
  sourceInfo.data = (__bridge void *)sourceBuffer;
  sourceInfo.bounds[2] = width;
  sourceInfo.bounds[3] = height;
  sourceInfo.rowBytes = width * 4 * static_cast<int>(sizeof(float));
  sourceInfo.depth = PixelDepth::Float;
  sourceInfo.components = Components::RGBA;
  ImageInfo destinationInfo = sourceInfo;
  destinationInfo.data = (__bridge void *)destinationBuffer;

  const LutData lut = makeBenchmarkLut(33);
  RenderParams params;
  params.nodeColorSpace = static_cast<int>(ColorSpaceId::SonySLog3Cine);
  params.lutInputColorSpace = static_cast<int>(ColorSpaceId::SonySLog3Cine);
  params.lutOutputColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  params.returnToNodeColorSpace = false;
  params.lumaStrength = 0.779;
  params.colorStrength = 1.08;
  params.shadowStrength = 0.92;
  params.midtoneStrength = 1.04;
  params.highlightStrength = 0.83;
  params.neutralBias = 0.94;
  params.blackLevel = 1.03;
  params.whiteRolloff = 0.96;
  params.colorPurity = 1.02;
  params.densityOffset = 0.03;
  params.hueShift = 1.5;
  const PreparedProcessor processor = prepareProcessor(lut, params);

  resetMetalRenderCaches();
  require(executeMetalRender((__bridge void *)queue,
                             sourceInfo,
                             destinationInfo,
                             true,
                             0,
                             0,
                             width,
                             height,
                             "metal-test-lut",
                             lut,
                             params,
                             processor),
          "Metal render should be submitted");
  id<MTLCommandBuffer> barrier = [queue commandBuffer];
  [barrier commit];
  [barrier waitUntilCompleted];
  require(barrier.status == MTLCommandBufferStatusCompleted, "Metal render should complete");

  const float *output = static_cast<const float *>(destinationBuffer.contents);
  double maximumError = 0.0;
  for (int i = 0; i < width * height; ++i) {
    const Vec3 exact = processor.apply({source[static_cast<size_t>(i * 4)],
                                        source[static_cast<size_t>(i * 4 + 1)],
                                        source[static_cast<size_t>(i * 4 + 2)]});
    maximumError = std::max({maximumError,
                             std::abs(exact.r - output[static_cast<size_t>(i * 4)]),
                             std::abs(exact.g - output[static_cast<size_t>(i * 4 + 1)]),
                             std::abs(exact.b - output[static_cast<size_t>(i * 4 + 2)]),
                             std::abs(static_cast<double>(source[static_cast<size_t>(i * 4 + 3)] -
                                                          output[static_cast<size_t>(i * 4 + 3)]))});
  }
  require(maximumError < 5e-4,
          "Metal and CPU processing should match; error was " + std::to_string(maximumError));
  resetMetalRenderCaches();
}
#endif

void runPerformanceBenchmark() {
  constexpr int pixelCount = 1000000;
  const LutData lut = makeBenchmarkLut(33);
  RenderParams complexParams;
  complexParams.nodeColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  complexParams.lutInputColorSpace = static_cast<int>(ColorSpaceId::DjiDLog);
  complexParams.lutOutputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  complexParams.returnToNodeColorSpace = true;
  complexParams.lumaStrength = 0.82;
  complexParams.colorStrength = 1.14;
  complexParams.shadowStrength = 0.91;
  complexParams.midtoneStrength = 1.08;
  complexParams.highlightStrength = 0.76;
  complexParams.neutralBias = 0.88;
  complexParams.blackLevel = 1.05;
  complexParams.whiteRolloff = 0.93;
  complexParams.colorPurity = 1.07;
  complexParams.densityOffset = 0.08;
  complexParams.hueShift = 4.0;
  const PreparedProcessor complexProcessor = prepareProcessor(lut, complexParams);

  RenderParams technicalParams;
  technicalParams.nodeColorSpace = static_cast<int>(ColorSpaceId::SonySLog3Cine);
  technicalParams.lutInputColorSpace = static_cast<int>(ColorSpaceId::SonySLog3Cine);
  technicalParams.lutOutputColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  technicalParams.returnToNodeColorSpace = false;
  technicalParams.lumaStrength = 0.779;
  const PreparedProcessor technicalProcessor = prepareProcessor(lut, technicalParams);

  RenderParams cachedParams = complexParams;
  cachedParams.nodeColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  cachedParams.lutInputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  cachedParams.lutOutputColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);
  const PreparedProcessor cachedProcessor = prepareProcessor(lut, cachedParams);
  const auto buildStarted = std::chrono::steady_clock::now();
  const std::shared_ptr<const AcceleratedLut> accelerated = buildAcceleratedLut(cachedProcessor, 65);
  const double buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStarted).count();

  auto measure = [&](const char *label, const auto &apply) {
    Vec3 checksum;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < pixelCount; ++i) {
      const Vec3 source{
        static_cast<double>((i * 17) % 1000) / 999.0,
        static_cast<double>((i * 37) % 1000) / 999.0,
        static_cast<double>((i * 67) % 1000) / 999.0,
      };
      checksum = checksum + apply(source);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << label << ": " << (pixelCount / seconds / 1000000.0) << " MP/s, checksum "
              << checksum.r + checksum.g + checksum.b << "\n";
  };

  std::cout << "Accelerated LUT build: " << buildSeconds * 1000.0 << " ms\n";
  measure("Prepared lookup, color managed", [&](const Vec3 &source) { return complexProcessor.apply(source); });
  measure("Prepared lookup, technical output", [&](const Vec3 &source) { return technicalProcessor.apply(source); });
  measure("Prepared lookup, same space", [&](const Vec3 &source) { return cachedProcessor.apply(source); });
  measure("Cached 3D", [&](const Vec3 &source) { return accelerated->applyUnitCube(source); });
}

}  // namespace

int main() {
  try {
    testOneDimensionalLut();
    testThreeDimensionalLut();
    testCombinedOneDimensionalAndThreeDimensionalLut();
    testFloatRangeIsPreserved();
    testTransferFunctionRoundTrips();
    testColorSpaceRoundTrips();
    testOpenColorIOReferenceVectors();
    testDjiOfficialReferenceValues();
    testIdentityLutPreservesColorAcrossWorkingSpaces();
    testLutWorkingSpaceChangesNonIdentityResult();
    testTechnicalLutInputAndOutputSpaces();
    testColorSpaceConversionToggle();
    testPreparedColorTransformsMatchReference();
    testAcceleratedLutMatchesPreparedProcessor();
#ifdef __APPLE__
    testMetalProcessorMatchesCpu();
#endif
    if (std::getenv("LUT_LAYER_MANAGER_BENCHMARK")) {
      runPerformanceBenchmark();
    }
    std::cout << "LUTLayerManager tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "LUTLayerManager test failed: " << error.what() << "\n";
    return 1;
  }
}
