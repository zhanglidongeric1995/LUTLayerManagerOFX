#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

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
    for (double sample : linearSamples) {
      const double encoded = encodeTransfer(sample, spec);
      const double decoded = decodeTransfer(encoded, spec);
      require(nearlyEqual(decoded, sample, 1e-9),
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
        requireColorNear(restored, sample, 2e-8,
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

  for (int colorSpace = 0; colorSpace < kColorSpaceCount; ++colorSpace) {
    const ColorSpaceSpec &spec = colorSpaceSpec(colorSpace);
    const Vec3 reference = multiply(spec.toReference, decodeTransfer(sample, spec));
    requireColorNear(reference, expected[static_cast<size_t>(colorSpace)], 6e-5,
                     "Color transform should match the OpenColorIO reference for space " +
                       std::to_string(colorSpace));
  }
}

void testIdentityLutPreservesColorAcrossWorkingSpaces() {
  const std::string path = temporaryCubePath("color_space_identity");
  writeTextFile(path, "LUT_3D_SIZE 2\n" + identity3DSamples());
  const LutData lut = parseCubeFile(path);
  std::remove(path.c_str());

  RenderParams params;
  params.nodeColorSpace = static_cast<int>(ColorSpaceId::DaVinciIntermediate);
  params.lutColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);

  const Vec3 source{0.336, 0.336, 0.336};
  const Vec3 result = applyColorManagedLayeredLut(source, lut, params);
  requireColorNear(result, source, 2e-8,
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
  rec709Params.lutColorSpace = static_cast<int>(ColorSpaceId::Rec709Gamma24);

  RenderParams logC3Params = rec709Params;
  logC3Params.lutColorSpace = static_cast<int>(ColorSpaceId::ArriLogC3);

  const Vec3 source{0.39, 0.44, 0.51};
  const Vec3 rec709Result = applyColorManagedLayeredLut(source, lut, rec709Params);
  const Vec3 logC3Result = applyColorManagedLayeredLut(source, lut, logC3Params);
  const double difference = std::abs(rec709Result.r - logC3Result.r) +
                            std::abs(rec709Result.g - logC3Result.g) +
                            std::abs(rec709Result.b - logC3Result.b);
  require(difference > 0.01, "Changing the LUT working space should affect a non-identity LUT result");
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
    testIdentityLutPreservesColorAcrossWorkingSpaces();
    testLutWorkingSpaceChangesNonIdentityResult();
    std::cout << "LUTLayerManager tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "LUTLayerManager test failed: " << error.what() << "\n";
    return 1;
  }
}
