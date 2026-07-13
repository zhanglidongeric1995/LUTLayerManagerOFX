#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

}  // namespace

int main() {
  try {
    testOneDimensionalLut();
    testThreeDimensionalLut();
    testCombinedOneDimensionalAndThreeDimensionalLut();
    testFloatRangeIsPreserved();
    std::cout << "LUTLayerManager tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "LUTLayerManager test failed: " << error.what() << "\n";
    return 1;
  }
}
