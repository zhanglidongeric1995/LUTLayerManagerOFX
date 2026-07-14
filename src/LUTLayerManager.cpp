#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <dispatch/dispatch.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char *kPluginIdentifier = "com.lidong.ofx.LUTLayerManager";
constexpr unsigned int kVersionMajor = 1;
constexpr unsigned int kVersionMinor = 2;

constexpr const char *kParamCubeChoice = "cubeChoice";
constexpr const char *kParamCubePath = "cubePath";
// Keep this ID for compatibility with projects created before the parameter
// was renamed from "working color space" to "node input color space".
constexpr const char *kParamWorkingSpace = "workingSpace";
constexpr const char *kParamLutWorkingSpace = "lutWorkingSpace";
constexpr const char *kParamLumaStrength = "lumaStrength";
constexpr const char *kParamColorStrength = "colorStrength";
constexpr const char *kParamShadowStrength = "shadowStrength";
constexpr const char *kParamMidtoneStrength = "midtoneStrength";
constexpr const char *kParamHighlightStrength = "highlightStrength";
constexpr const char *kParamNeutralBias = "neutralBias";
constexpr const char *kParamBlackLevel = "blackLevel";
constexpr const char *kParamWhiteRolloff = "whiteRolloff";
constexpr const char *kParamColorPurity = "colorPurity";
constexpr const char *kParamDensityOffset = "densityOffset";
constexpr const char *kParamHueShift = "hueShift";

OfxHost *gHost = nullptr;
OfxPropertySuiteV1 *gProp = nullptr;
OfxImageEffectSuiteV1 *gEffect = nullptr;
OfxParameterSuiteV1 *gParam = nullptr;
thread_local bool gParamSyncInProgress = false;

struct Vec3 {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

struct Vec4 {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double a = 1.0;
};

Vec3 operator+(const Vec3 &a, const Vec3 &b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
Vec3 operator-(const Vec3 &a, const Vec3 &b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
Vec3 operator*(const Vec3 &a, double s) { return {a.r * s, a.g * s, a.b * s}; }

enum class ColorSpaceId : int {
  Rec709Gamma24 = 0,
  ArriLogC3 = 1,
  ArriLogC4 = 2,
  BmdFilmGen5 = 3,
  DaVinciIntermediate = 4,
  ACEScct = 5,
  SonySLog3Cine = 6,
  PanasonicVLog = 7,
  RedLog3G10 = 8,
  CanonLog2 = 9,
};

constexpr int kColorSpaceCount = 10;

const std::vector<std::string> &colorSpaceLabels() {
  static const std::vector<std::string> labels = {
    "Rec.709 Gamma 2.4",
    "ARRI LogC3 / Wide Gamut 3 (EI800)",
    "ARRI LogC4 / Wide Gamut 4",
    "Blackmagic Film Gen 5 / Wide Gamut",
    "DaVinci Wide Gamut / Intermediate",
    "ACEScct / AP1",
    "Sony S-Log3 / S-Gamut3.Cine",
    "Panasonic V-Log / V-Gamut",
    "RED Log3G10 / REDWideGamutRGB",
    "Canon C-Log2 / Cinema Gamut",
  };
  return labels;
}

int sanitizeColorSpace(int value) {
  return value >= 0 && value < kColorSpaceCount ? value : static_cast<int>(ColorSpaceId::Rec709Gamma24);
}

struct Mat3 {
  std::array<double, 9> v{};
};

Vec3 multiply(const Mat3 &m, const Vec3 &c) {
  return {
    m.v[0] * c.r + m.v[1] * c.g + m.v[2] * c.b,
    m.v[3] * c.r + m.v[4] * c.g + m.v[5] * c.b,
    m.v[6] * c.r + m.v[7] * c.g + m.v[8] * c.b,
  };
}

Mat3 inverse(const Mat3 &m) {
  const double a = m.v[0];
  const double b = m.v[1];
  const double c = m.v[2];
  const double d = m.v[3];
  const double e = m.v[4];
  const double f = m.v[5];
  const double g = m.v[6];
  const double h = m.v[7];
  const double i = m.v[8];
  const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::abs(determinant) < 1e-15) {
    return {{{1.0, 0.0, 0.0,
              0.0, 1.0, 0.0,
              0.0, 0.0, 1.0}}};
  }

  const double s = 1.0 / determinant;
  return {{{
    (e * i - f * h) * s,
    (c * h - b * i) * s,
    (b * f - c * e) * s,
    (f * g - d * i) * s,
    (a * i - c * g) * s,
    (c * d - a * f) * s,
    (d * h - e * g) * s,
    (b * g - a * h) * s,
    (a * e - b * d) * s,
  }}};
}

enum class TransferKind { Gamma24, CameraLog, CanonLog2 };

struct CameraLogCurve {
  double base = 2.0;
  double linSideSlope = 1.0;
  double linSideOffset = 0.0;
  double logSideSlope = 1.0;
  double logSideOffset = 0.0;
  double linSideBreak = 0.0;
  double linearSlope = 0.0;
  double linearOffset = 0.0;
  double logSideBreak = 0.0;
};

CameraLogCurve prepareCameraLogCurve(CameraLogCurve curve) {
  const double logArgument = curve.linSideSlope * curve.linSideBreak + curve.linSideOffset;
  curve.logSideBreak = curve.logSideSlope * (std::log(logArgument) / std::log(curve.base)) + curve.logSideOffset;
  if (curve.linearSlope <= 0.0) {
    curve.linearSlope = curve.logSideSlope * curve.linSideSlope /
                        (logArgument * std::log(curve.base));
  }
  curve.linearOffset = curve.logSideBreak - curve.linearSlope * curve.linSideBreak;
  return curve;
}

struct ColorSpaceSpec {
  TransferKind transfer = TransferKind::Gamma24;
  CameraLogCurve log;
  Mat3 toReference;
  Mat3 fromReference;
  Vec3 lumaWeights;
};

Vec3 deriveLumaWeights(const Mat3 &toReference) {
  // ACES AP0 RGB to XYZ Y row. Multiplying by the source-to-AP0 matrix
  // yields weights appropriate to each source gamut.
  constexpr Vec3 ap0Y{0.343966449765075, 0.728166096613485, -0.0721325463785608};
  Vec3 weights = {
    ap0Y.r * toReference.v[0] + ap0Y.g * toReference.v[3] + ap0Y.b * toReference.v[6],
    ap0Y.r * toReference.v[1] + ap0Y.g * toReference.v[4] + ap0Y.b * toReference.v[7],
    ap0Y.r * toReference.v[2] + ap0Y.g * toReference.v[5] + ap0Y.b * toReference.v[8],
  };
  const double sum = weights.r + weights.g + weights.b;
  if (std::abs(sum) > 1e-12) {
    weights = weights * (1.0 / sum);
  }
  return weights;
}

ColorSpaceSpec makeColorSpaceSpec(TransferKind transfer, CameraLogCurve log, const Mat3 &toReference) {
  ColorSpaceSpec spec;
  spec.transfer = transfer;
  spec.log = transfer == TransferKind::CameraLog ? prepareCameraLogCurve(log) : log;
  spec.toReference = toReference;
  spec.fromReference = inverse(toReference);
  spec.lumaWeights = deriveLumaWeights(toReference);
  return spec;
}

const std::array<ColorSpaceSpec, kColorSpaceCount> &colorSpaceSpecs() {
  // Transfer parameters and source-to-ACES2065-1 matrices follow OpenColorIO
  // 2.3.2 / ACES Studio Config 2.1.0. The BSD license is bundled in Resources.
  static const std::array<ColorSpaceSpec, kColorSpaceCount> specs = [] {
    const Mat3 ap0ToRec709{{{
      2.52168618674388, -1.13413098823972, -0.387555198504164,
      -0.276479914229922, 1.37271908766826, -0.096239173438334,
      -0.0153780649660342, -0.152975335867399, 1.16835340083343,
    }}};

    return std::array<ColorSpaceSpec, kColorSpaceCount>{{
      makeColorSpaceSpec(TransferKind::Gamma24, {}, inverse(ap0ToRec709)),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {10.0, 5.55555555555556, 0.0522722750251688, 0.247189638318671,
                          0.385536998692443, 0.0105909904954696},
                         {{{0.680205505106279, 0.236136601606481, 0.0836578932872398,
                            0.0854149797421404, 1.01747087860704, -0.102885858349182,
                            0.00205652166929683, -0.0625625003847921, 1.06050597871549}}}),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {2.0, 2231.82630906769, 64.0, 0.0647954196341293,
                          -0.295908392682586, -0.0180569961199113},
                         {{{0.750957362824734, 0.144422786709757, 0.104619850465509,
                            0.000821837079380207, 1.007397584885, -0.00821942196438358,
                            -0.000499952143533471, -0.000854177231436971, 1.00135412937497}}}),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {2.71828182845905, 1.0, 0.00549407243225781, 0.0869287606549122,
                          0.530013339229194, 0.005},
                         {{{0.647091325580708, 0.242595385134207, 0.110313289285085,
                            0.0651915997328519, 1.02504756760476, -0.0902391673376125,
                            -0.0275570729194699, -0.0805887097177784, 1.10814578263725}}}),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {2.0, 1.0, 0.0075, 0.07329248, 0.51304736, 0.00262409, 10.44426855},
                         {{{0.748270290272981, 0.167694659554328, 0.0840350501726906,
                            0.0208421234689102, 1.11190474268894, -0.132746866157851,
                            -0.0915122574225729, -0.127746712807307, 1.21925897022988}}}),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {2.0, 1.0, 0.0, 0.0570776255707763, 0.554794520547945, 0.0078125},
                         {{{0.695452241357452, 0.140678696470294, 0.163869062172254,
                            0.0447945633720377, 0.859671118456422, 0.0955343181715404,
                            -0.00552588255811354, 0.00402521030597866, 1.00150067225213}}}),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {10.0, 5.26315789473684, 0.0526315789473684, 0.255620723362659,
                          0.410557184750733, 0.01125, 6.62194371177582},
                         {{{0.638788667185978, 0.272351433711262, 0.0888598991027595,
                            -0.00391590602528224, 1.0880732308974, -0.0841573248721177,
                            -0.0299072021239151, -0.0264325799101947, 1.05633978203411}}}),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {10.0, 1.0, 0.00873, 0.241514, 0.598206, 0.01},
                         {{{0.72461670413153, 0.166915288193706, 0.108468007674764,
                            0.021390245413146, 0.984908155703054, -0.00629840111620089,
                            -0.00923556287076561, -0.00105690563900513, 1.01029246850977}}}),
      makeColorSpaceSpec(TransferKind::CameraLog,
                         {10.0, 155.975327, 2.55975327, 0.224282, 0.0, -0.01},
                         {{{0.785058804068092, 0.0838587565440846, 0.131082439387823,
                            0.0231738348454756, 1.08789754919233, -0.111071384037806,
                            -0.0737604353682082, -0.314590072290208, 1.38835050765842}}}),
      makeColorSpaceSpec(TransferKind::CanonLog2, {},
                         {{{0.763064454775734, 0.14902116113706, 0.0879143840872055,
                            0.00365745670512393, 1.10696038037622, -0.110617837081339,
                            -0.0094077940457189, -0.218383304989987, 1.22779109903571}}}),
    }};
  }();
  return specs;
}

const ColorSpaceSpec &colorSpaceSpec(int colorSpace) {
  return colorSpaceSpecs()[static_cast<size_t>(sanitizeColorSpace(colorSpace))];
}

double cameraLogToLinear(double value, const CameraLogCurve &curve) {
  if (value <= curve.logSideBreak) {
    return (value - curve.linearOffset) / curve.linearSlope;
  }
  return (std::pow(curve.base, (value - curve.logSideOffset) / curve.logSideSlope) - curve.linSideOffset) /
         curve.linSideSlope;
}

double linearToCameraLog(double value, const CameraLogCurve &curve) {
  if (value <= curve.linSideBreak) {
    return curve.linearSlope * value + curve.linearOffset;
  }
  const double argument = std::max(curve.linSideSlope * value + curve.linSideOffset,
                                   std::numeric_limits<double>::min());
  return curve.logSideSlope * (std::log(argument) / std::log(curve.base)) + curve.logSideOffset;
}

double canonLog2ToLinear(double value) {
  constexpr double pivot = 0.092864125;
  constexpr double slope = 0.24136077;
  constexpr double gain = 87.099375;
  const double magnitude = value < pivot
                             ? -(std::pow(10.0, (pivot - value) / slope) - 1.0) / gain
                             : (std::pow(10.0, (value - pivot) / slope) - 1.0) / gain;
  return magnitude * 0.9;
}

double linearToCanonLog2(double value) {
  constexpr double pivot = 0.092864125;
  constexpr double slope = 0.24136077;
  constexpr double gain = 87.099375;
  const double normalized = value / 0.9;
  const double encodedMagnitude = slope * std::log10(1.0 + std::abs(normalized) * gain);
  return normalized < 0.0 ? pivot - encodedMagnitude : pivot + encodedMagnitude;
}

double decodeTransfer(double value, const ColorSpaceSpec &spec) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  switch (spec.transfer) {
    case TransferKind::Gamma24:
      return value < 0.0 ? value : std::pow(value, 2.4);
    case TransferKind::CameraLog:
      return cameraLogToLinear(value, spec.log);
    case TransferKind::CanonLog2:
      return canonLog2ToLinear(value);
  }
  return value;
}

double encodeTransfer(double value, const ColorSpaceSpec &spec) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  switch (spec.transfer) {
    case TransferKind::Gamma24:
      return value < 0.0 ? value : std::pow(value, 1.0 / 2.4);
    case TransferKind::CameraLog:
      return linearToCameraLog(value, spec.log);
    case TransferKind::CanonLog2:
      return linearToCanonLog2(value);
  }
  return value;
}

Vec3 decodeTransfer(const Vec3 &value, const ColorSpaceSpec &spec) {
  return {decodeTransfer(value.r, spec), decodeTransfer(value.g, spec), decodeTransfer(value.b, spec)};
}

Vec3 encodeTransfer(const Vec3 &value, const ColorSpaceSpec &spec) {
  return {encodeTransfer(value.r, spec), encodeTransfer(value.g, spec), encodeTransfer(value.b, spec)};
}

Vec3 convertColorSpace(const Vec3 &value, int sourceSpace, int destinationSpace) {
  const int source = sanitizeColorSpace(sourceSpace);
  const int destination = sanitizeColorSpace(destinationSpace);
  if (source == destination) {
    return value;
  }

  const ColorSpaceSpec &sourceSpec = colorSpaceSpec(source);
  const ColorSpaceSpec &destinationSpec = colorSpaceSpec(destination);
  const Vec3 referenceLinear = multiply(sourceSpec.toReference, decodeTransfer(value, sourceSpec));
  const Vec3 destinationLinear = multiply(destinationSpec.fromReference, referenceLinear);
  return encodeTransfer(destinationLinear, destinationSpec);
}

double clamp01(double v) {
  if (!std::isfinite(v)) {
    return 0.0;
  }
  return std::max(0.0, std::min(1.0, v));
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

Vec3 lerp(const Vec3 &a, const Vec3 &b, double t) {
  return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t)};
}

double smoothstep(double edge0, double edge1, double x) {
  double t = clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

std::string trim(const std::string &s) {
  const char *ws = " \t\r\n";
  const size_t first = s.find_first_not_of(ws);
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = s.find_last_not_of(ws);
  return s.substr(first, last - first + 1);
}

std::string unquote(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

std::string expandPath(std::string path) {
  path = unquote(path);
  if (path.rfind("~/", 0) == 0) {
    const char *home = std::getenv("HOME");
    if (home) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

bool fileExists(const std::string &path) {
  struct stat st {};
  return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool directoryExists(const std::string &path) {
  struct stat st {};
  return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string dirnameOf(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return {};
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

std::string preferenceDirectory() {
  const char *home = std::getenv("HOME");
  if (!home) {
    return {};
  }
  return std::string(home) + "/Library/Application Support/LIDONGFILMS/LUTLayerManager";
}

std::string preferenceFilePath() {
  const std::string dir = preferenceDirectory();
  return dir.empty() ? std::string() : dir + "/last_lut_path.txt";
}

std::string historyFilePath() {
  const std::string dir = preferenceDirectory();
  return dir.empty() ? std::string() : dir + "/lut_history.txt";
}

void mkdirIfNeeded(const std::string &path) {
  if (path.empty() || directoryExists(path)) {
    return;
  }
  const std::string parent = dirnameOf(path);
  if (!parent.empty() && parent != path) {
    mkdirIfNeeded(parent);
  }
  mkdir(path.c_str(), 0755);
}

std::string basenameOf(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentNameOf(const std::string &path) {
  const std::string parent = dirnameOf(path);
  if (parent.empty() || parent == "/") {
    return {};
  }
  return basenameOf(parent);
}

std::vector<std::string> readLutHistory() {
  std::vector<std::string> history;
  auto addUnique = [&history](const std::string &raw) {
    const std::string path = expandPath(raw);
    if (!fileExists(path)) {
      return;
    }
    if (std::find(history.begin(), history.end(), path) == history.end()) {
      history.push_back(path);
    }
  };

  const std::string historyPath = historyFilePath();
  if (!historyPath.empty()) {
    std::ifstream in(historyPath);
    std::string path;
    while (std::getline(in, path)) {
      addUnique(path);
    }
  }

  const std::string oldPref = preferenceFilePath();
  if (!oldPref.empty()) {
    std::ifstream in(oldPref);
    std::string path;
    if (std::getline(in, path)) {
      addUnique(path);
    }
  }

  return history;
}

void writeLutHistory(const std::vector<std::string> &history) {
  const std::string dir = preferenceDirectory();
  const std::string historyPath = historyFilePath();
  if (dir.empty() || historyPath.empty()) {
    return;
  }
  mkdirIfNeeded(dir);
  std::ofstream out(historyPath, std::ios::trunc);
  if (!out) {
    return;
  }
  int count = 0;
  for (const std::string &path : history) {
    if (fileExists(path)) {
      out << path << "\n";
      if (++count >= 40) {
        break;
      }
    }
  }
}

std::string readSavedLutPath() {
  const std::vector<std::string> history = readLutHistory();
  return history.empty() ? std::string() : history.front();
}

void writeSavedLutPath(const std::string &path) {
  const std::string expanded = expandPath(path);
  if (!fileExists(expanded)) {
    return;
  }

  std::vector<std::string> history = readLutHistory();
  history.erase(std::remove(history.begin(), history.end(), expanded), history.end());
  history.insert(history.begin(), expanded);
  writeLutHistory(history);

  const std::string pref = preferenceFilePath();
  if (!pref.empty()) {
    std::ofstream out(pref, std::ios::trunc);
    if (out) {
      out << expanded << "\n";
    }
  }
}

std::string displayNameForPath(const std::string &path, const std::vector<std::string> &history) {
  const std::string base = basenameOf(path);
  int sameBase = 0;
  for (const std::string &candidate : history) {
    if (basenameOf(candidate) == base) {
      ++sameBase;
    }
  }
  if (sameBase > 1) {
    const std::string parent = parentNameOf(path);
    if (!parent.empty()) {
      return parent + "/" + base;
    }
  }
  return base;
}

std::vector<std::string> cubeChoiceLabels(const std::vector<std::string> &history) {
  std::vector<std::string> labels;
  labels.push_back("选择 .cube 文件...");
  for (const std::string &path : history) {
    labels.push_back(displayNameForPath(path, history));
  }
  return labels;
}

int choiceIndexForPath(const std::string &path, const std::vector<std::string> &history) {
  const std::string expanded = expandPath(path);
  for (size_t i = 0; i < history.size(); ++i) {
    if (history[i] == expanded) {
      return static_cast<int>(i + 1);
    }
  }
  return 0;
}

std::string pathForChoiceIndex(int index, const std::vector<std::string> &history) {
  if (index <= 0) {
    return {};
  }
  const size_t historyIndex = static_cast<size_t>(index - 1);
  return historyIndex < history.size() ? history[historyIndex] : std::string();
}

#ifdef __APPLE__
std::string chooseCubeFile(const std::string &startPath) {
  __block std::string result;
  std::string initial = expandPath(startPath);
  if (initial.empty()) {
    initial = readSavedLutPath();
  }

  void (^runPanel)(void) = ^{
    @autoreleasepool {
      NSOpenPanel *panel = [NSOpenPanel openPanel];
      [panel setCanChooseFiles:YES];
      [panel setCanChooseDirectories:NO];
      [panel setAllowsMultipleSelection:NO];
      [panel setResolvesAliases:YES];
      [panel setCanCreateDirectories:NO];
      [panel setTitle:@"选择 .cube 文件"];
      [panel setPrompt:@"选择"];

      if (@available(macOS 11.0, *)) {
        UTType *cubeType = [UTType typeWithFilenameExtension:@"cube"];
        if (cubeType) {
          panel.allowedContentTypes = @[cubeType];
        }
      }

      std::string directory = fileExists(initial) ? dirnameOf(initial) : initial;
      if (directoryExists(directory)) {
        NSString *dir = [NSString stringWithUTF8String:directory.c_str()];
        if (dir) {
          [panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
        }
      }

      [NSApp activateIgnoringOtherApps:YES];
      if ([panel runModal] == NSModalResponseOK) {
        NSURL *url = [[panel URLs] firstObject];
        NSString *path = [url path];
        if (path) {
          result = [path UTF8String];
        }
      }
    }
  };

  if ([NSThread isMainThread]) {
    runPanel();
  } else {
    dispatch_sync(dispatch_get_main_queue(), runPanel);
  }
  return result;
}
#else
std::string chooseCubeFile(const std::string &) { return {}; }
#endif

size_t cubeSampleCount(int size) {
  if (size <= 1) {
    return 0;
  }
  const size_t dimension = static_cast<size_t>(size);
  if (dimension > std::numeric_limits<size_t>::max() / dimension ||
      dimension * dimension > std::numeric_limits<size_t>::max() / dimension) {
    return 0;
  }
  return dimension * dimension * dimension;
}

struct LutData {
  enum class Type { Invalid, Lut1D, Lut3D, Lut1DThen3D };

  Type type = Type::Invalid;
  int size = 0;
  int shaperSize = 0;
  Vec3 domainMin{0.0, 0.0, 0.0};
  Vec3 domainMax{1.0, 1.0, 1.0};
  std::vector<Vec3> values;
  std::vector<Vec3> shaperValues;
  std::string error;

  bool valid() const {
    if (type == Type::Lut1D) {
      return size > 1 && values.size() >= static_cast<size_t>(size);
    }
    if (type == Type::Lut3D) {
      return size > 1 && values.size() >= cubeSampleCount(size);
    }
    if (type == Type::Lut1DThen3D) {
      return shaperSize > 1 && size > 1 &&
             shaperValues.size() >= static_cast<size_t>(shaperSize) &&
             values.size() >= cubeSampleCount(size);
    }
    return false;
  }

  Vec3 apply(const Vec3 &rgb) const {
    if (!valid()) {
      return rgb;
    }

    const auto normalize = [](double v, double mn, double mx) {
      if (std::abs(mx - mn) < 1e-12) {
        return clamp01(v);
      }
      return clamp01((v - mn) / (mx - mn));
    };

    const double r = normalize(rgb.r, domainMin.r, domainMax.r);
    const double g = normalize(rgb.g, domainMin.g, domainMax.g);
    const double b = normalize(rgb.b, domainMin.b, domainMax.b);

    if (type == Type::Lut1D || type == Type::Lut1DThen3D) {
      const std::vector<Vec3> &shaper = type == Type::Lut1D ? values : shaperValues;
      const int shaperResolution = type == Type::Lut1D ? size : shaperSize;
      const Vec3 shaped = {
        sample1D(shaper, shaperResolution, r, 0),
        sample1D(shaper, shaperResolution, g, 1),
        sample1D(shaper, shaperResolution, b, 2),
      };
      if (type == Type::Lut1D) {
        return shaped;
      }
      return sample3D(shaped.r, shaped.g, shaped.b);
    }

    return sample3D(r, g, b);
  }

private:
  double sample1D(const std::vector<Vec3> &samples, int resolution, double v, int component) const {
    const double x = v * (resolution - 1);
    const int i0 = static_cast<int>(std::floor(x));
    const int i1 = std::min(i0 + 1, resolution - 1);
    const double t = x - i0;
    const Vec3 a = samples[static_cast<size_t>(i0)];
    const Vec3 b = samples[static_cast<size_t>(i1)];
    const double av = component == 0 ? a.r : (component == 1 ? a.g : a.b);
    const double bv = component == 0 ? b.r : (component == 1 ? b.g : b.b);
    return lerp(av, bv, t);
  }

  Vec3 sample3D(double r, double g, double b) const {
    const double x = r * (size - 1);
    const double y = g * (size - 1);
    const double z = b * (size - 1);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = std::min(x0 + 1, size - 1);
    const int y1 = std::min(y0 + 1, size - 1);
    const int z1 = std::min(z0 + 1, size - 1);
    const double tx = x - x0;
    const double ty = y - y0;
    const double tz = z - z0;

    const Vec3 c000 = at3D(x0, y0, z0);
    const Vec3 c100 = at3D(x1, y0, z0);
    const Vec3 c010 = at3D(x0, y1, z0);
    const Vec3 c110 = at3D(x1, y1, z0);
    const Vec3 c001 = at3D(x0, y0, z1);
    const Vec3 c101 = at3D(x1, y0, z1);
    const Vec3 c011 = at3D(x0, y1, z1);
    const Vec3 c111 = at3D(x1, y1, z1);

    const Vec3 c00 = lerp(c000, c100, tx);
    const Vec3 c10 = lerp(c010, c110, tx);
    const Vec3 c01 = lerp(c001, c101, tx);
    const Vec3 c11 = lerp(c011, c111, tx);
    const Vec3 c0 = lerp(c00, c10, ty);
    const Vec3 c1 = lerp(c01, c11, ty);
    return lerp(c0, c1, tz);
  }

  Vec3 at3D(int r, int g, int b) const {
    const size_t index = static_cast<size_t>(r + g * size + b * size * size);
    if (index >= values.size()) {
      return {};
    }
    return values[index];
  }
};

LutData parseCubeFile(const std::string &path) {
  LutData lut;
  std::ifstream in(path);
  if (!in) {
    lut.error = "Cannot open LUT file";
    return lut;
  }

  int declared1D = 0;
  int declared3D = 0;
  std::vector<Vec3> samples;
  std::string line;

  while (std::getline(in, line)) {
    const size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    std::stringstream ss(line);
    std::string tag;
    ss >> tag;

    if (tag == "TITLE") {
      continue;
    }
    if (tag == "LUT_1D_SIZE") {
      ss >> declared1D;
      continue;
    }
    if (tag == "LUT_3D_SIZE") {
      ss >> declared3D;
      continue;
    }
    if (tag == "DOMAIN_MIN") {
      ss >> lut.domainMin.r >> lut.domainMin.g >> lut.domainMin.b;
      continue;
    }
    if (tag == "DOMAIN_MAX") {
      ss >> lut.domainMax.r >> lut.domainMax.g >> lut.domainMax.b;
      continue;
    }

    char *end = nullptr;
    const double first = std::strtod(tag.c_str(), &end);
    if (end == tag.c_str() || *end != '\0') {
      continue;
    }

    double second = 0.0;
    double third = 0.0;
    if (ss >> second >> third) {
      samples.push_back({first, second, third});
    }
  }

  const size_t required3D = cubeSampleCount(declared3D);
  if (declared3D > 1 && required3D == 0) {
    lut.error = "3D LUT size is too large";
    return lut;
  }

  if (declared1D > 1 && declared3D > 1) {
    const size_t required1D = static_cast<size_t>(declared1D);
    if (required1D > std::numeric_limits<size_t>::max() - required3D ||
        samples.size() < required1D + required3D) {
      lut.error = "Combined 1D/3D LUT sample count is smaller than its declared size";
      return lut;
    }
    lut.type = LutData::Type::Lut1DThen3D;
    lut.shaperSize = declared1D;
    lut.size = declared3D;
    lut.shaperValues.assign(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(required1D));
    lut.values.assign(samples.begin() + static_cast<std::ptrdiff_t>(required1D),
                      samples.begin() + static_cast<std::ptrdiff_t>(required1D + required3D));
    return lut;
  }

  if (declared3D > 1) {
    lut.type = LutData::Type::Lut3D;
    lut.size = declared3D;
    if (samples.size() < required3D) {
      lut.error = "3D LUT sample count is smaller than LUT_3D_SIZE";
      lut.type = LutData::Type::Invalid;
      return lut;
    }
    lut.values.assign(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(required3D));
    return lut;
  }

  if (declared1D > 1) {
    lut.type = LutData::Type::Lut1D;
    lut.size = declared1D;
    if (samples.size() < static_cast<size_t>(declared1D)) {
      lut.error = "1D LUT sample count is smaller than LUT_1D_SIZE";
      lut.type = LutData::Type::Invalid;
      return lut;
    }
    lut.values.assign(samples.begin(), samples.begin() + declared1D);
    return lut;
  }

  lut.error = "No supported LUT_1D_SIZE or LUT_3D_SIZE found";
  return lut;
}

struct CacheEntry {
  time_t mtime = 0;
  off_t size = 0;
  LutData lut;
};

std::mutex gCacheMutex;
std::unordered_map<std::string, CacheEntry> gLutCache;

const LutData &identityLut() {
  static const LutData lut;
  return lut;
}

LutData getLut(const std::string &rawPath) {
  std::string path = expandPath(rawPath);
  if (path.empty()) {
    path = readSavedLutPath();
  }
  if (path.empty()) {
    return identityLut();
  }

  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return identityLut();
  }

  std::lock_guard<std::mutex> lock(gCacheMutex);
  auto found = gLutCache.find(path);
  if (found != gLutCache.end() && found->second.mtime == st.st_mtime && found->second.size == st.st_size) {
    return found->second.lut;
  }

  CacheEntry entry;
  entry.mtime = st.st_mtime;
  entry.size = st.st_size;
  entry.lut = parseCubeFile(path);
  auto result = gLutCache.insert_or_assign(path, std::move(entry));
  return result.first->second.lut;
}

struct RenderParams {
  std::string cubePath;
  int nodeColorSpace = 0;
  int lutColorSpace = -1;
  double lumaStrength = 1.0;
  double colorStrength = 1.0;
  double shadowStrength = 1.0;
  double midtoneStrength = 1.0;
  double highlightStrength = 1.0;
  double neutralBias = 1.0;
  double blackLevel = 1.0;
  double whiteRolloff = 1.0;
  double colorPurity = 1.0;
  double densityOffset = 0.0;
  double hueShift = 0.0;
};

Vec3 lumaWeights(int colorSpace) { return colorSpaceSpec(colorSpace).lumaWeights; }

double dotLuma(const Vec3 &v, const Vec3 &w) { return v.r * w.r + v.g * w.g + v.b * w.b; }

Vec3 hueRotate(const Vec3 &c, double degrees) {
  if (std::abs(degrees) < 1e-9) {
    return c;
  }

  const double angle = degrees * 3.14159265358979323846 / 180.0;
  const double cosA = std::cos(angle);
  const double sinA = std::sin(angle);

  return {
    c.r * (0.213 + cosA * 0.787 - sinA * 0.213) +
      c.g * (0.715 - cosA * 0.715 - sinA * 0.715) +
      c.b * (0.072 - cosA * 0.072 + sinA * 0.928),
    c.r * (0.213 - cosA * 0.213 + sinA * 0.143) +
      c.g * (0.715 + cosA * 0.285 + sinA * 0.140) +
      c.b * (0.072 - cosA * 0.072 - sinA * 0.283),
    c.r * (0.213 - cosA * 0.213 - sinA * 0.787) +
      c.g * (0.715 - cosA * 0.715 + sinA * 0.715) +
      c.b * (0.072 + cosA * 0.928 + sinA * 0.072),
  };
}

Vec3 applyLayeredLut(const Vec3 &src, const LutData &lut, const RenderParams &params, int colorSpace) {
  if (!lut.valid()) {
    return src;
  }

  const Vec3 weights = lumaWeights(colorSpace);
  const Vec3 lutRgb = lut.apply(src);

  const double srcY = dotLuma(src, weights);
  const double lutY = dotLuma(lutRgb, weights);
  const Vec3 srcChroma = src - Vec3{srcY, srcY, srcY};
  const Vec3 lutChroma = lutRgb - Vec3{lutY, lutY, lutY};

  const double y = clamp01(srcY);
  const double shadowMask = 1.0 - smoothstep(0.18, 0.45, y);
  const double highlightMask = smoothstep(0.55, 0.82, y);
  const double midMask = clamp01(1.0 - std::abs(y - 0.5) * 2.0);
  const double maskSum = std::max(1e-6, shadowMask + midMask + highlightMask);
  const double zoneStrength =
    (shadowMask * params.shadowStrength + midMask * params.midtoneStrength + highlightMask * params.highlightStrength) / maskSum;

  const double sat = std::max({std::abs(srcChroma.r), std::abs(srcChroma.g), std::abs(srcChroma.b)});
  const double neutralMask = 1.0 - smoothstep(0.035, 0.18, sat);
  const double neutralScale = std::max(0.0, 1.0 + (params.neutralBias - 1.0) * neutralMask);

  double lumaDelta = (lutY - srcY) * params.lumaStrength;
  lumaDelta *= 1.0 + (params.blackLevel - 1.0) * shadowMask;
  lumaDelta *= 1.0 + (params.whiteRolloff - 1.0) * highlightMask;

  Vec3 chromaDelta = (lutChroma - srcChroma) * (params.colorStrength * neutralScale);

  double outY = srcY + lumaDelta * zoneStrength;
  Vec3 outChroma = srcChroma + chromaDelta * zoneStrength;
  Vec3 result = {outY + outChroma.r, outY + outChroma.g, outY + outChroma.b};

  if (std::abs(params.hueShift) > 1e-9) {
    const double yBefore = dotLuma(result, weights);
    Vec3 rotated = hueRotate(result, params.hueShift);
    const double yAfter = dotLuma(rotated, weights);
    result = rotated + Vec3{yBefore - yAfter, yBefore - yAfter, yBefore - yAfter};
  }

  if (std::abs(params.colorPurity - 1.0) > 1e-9) {
    const double finalY = dotLuma(result, weights);
    result = Vec3{finalY, finalY, finalY} + (result - Vec3{finalY, finalY, finalY}) * params.colorPurity;
  }

  if (std::abs(params.densityOffset) > 1e-9) {
    const double gain = std::pow(2.0, -params.densityOffset);
    result = result * gain;
  }

  return result;
}

Vec3 applyColorManagedLayeredLut(const Vec3 &src, const LutData &lut, const RenderParams &params) {
  const int nodeSpace = sanitizeColorSpace(params.nodeColorSpace);
  const int lutSpace = params.lutColorSpace < 0 ? nodeSpace : sanitizeColorSpace(params.lutColorSpace);
  const Vec3 lutInput = convertColorSpace(src, nodeSpace, lutSpace);
  const Vec3 lutOutput = applyLayeredLut(lutInput, lut, params, lutSpace);
  return convertColorSpace(lutOutput, lutSpace, nodeSpace);
}

enum class PixelDepth { Byte, Short, Float, Unsupported };
enum class Components { RGB, RGBA, Unsupported };

PixelDepth parseDepth(const char *depth) {
  if (!depth) {
    return PixelDepth::Unsupported;
  }
  const std::string d(depth);
  if (d == kOfxBitDepthByte) {
    return PixelDepth::Byte;
  }
  if (d == kOfxBitDepthShort) {
    return PixelDepth::Short;
  }
  if (d == kOfxBitDepthFloat) {
    return PixelDepth::Float;
  }
  return PixelDepth::Unsupported;
}

Components parseComponents(const char *components) {
  if (!components) {
    return Components::Unsupported;
  }
  const std::string c(components);
  if (c == kOfxImageComponentRGB) {
    return Components::RGB;
  }
  if (c == kOfxImageComponentRGBA) {
    return Components::RGBA;
  }
  return Components::Unsupported;
}

int componentCount(Components c) { return c == Components::RGBA ? 4 : 3; }

struct ImageInfo {
  void *data = nullptr;
  int bounds[4] = {0, 0, 0, 0};
  int rowBytes = 0;
  PixelDepth depth = PixelDepth::Unsupported;
  Components components = Components::Unsupported;
};

bool getImageInfo(OfxPropertySetHandle image, ImageInfo &info) {
  if (!image || !gProp) {
    return false;
  }

  void *data = nullptr;
  int bounds[4] = {0, 0, 0, 0};
  int rowBytes = 0;
  char *depth = nullptr;
  char *components = nullptr;

  if (gProp->propGetPointer(image, kOfxImagePropData, 0, &data) != kOfxStatOK || !data) {
    return false;
  }
  if (gProp->propGetIntN(image, kOfxImagePropBounds, 4, bounds) != kOfxStatOK) {
    return false;
  }
  if (gProp->propGetInt(image, kOfxImagePropRowBytes, 0, &rowBytes) != kOfxStatOK) {
    return false;
  }
  if (gProp->propGetString(image, kOfxImageEffectPropPixelDepth, 0, &depth) != kOfxStatOK) {
    return false;
  }
  if (gProp->propGetString(image, kOfxImageEffectPropComponents, 0, &components) != kOfxStatOK) {
    return false;
  }

  info.data = data;
  std::copy(bounds, bounds + 4, info.bounds);
  info.rowBytes = rowBytes;
  info.depth = parseDepth(depth);
  info.components = parseComponents(components);
  return info.depth != PixelDepth::Unsupported && info.components != Components::Unsupported;
}

bool contains(const ImageInfo &image, int x, int y) {
  return x >= image.bounds[0] && x < image.bounds[2] && y >= image.bounds[1] && y < image.bounds[3];
}

template <typename T>
double readNorm(T v);

template <>
double readNorm<uint8_t>(uint8_t v) {
  return static_cast<double>(v) / 255.0;
}

template <>
double readNorm<uint16_t>(uint16_t v) {
  return static_cast<double>(v) / 65535.0;
}

template <>
double readNorm<float>(float v) {
  return static_cast<double>(v);
}

template <typename T>
T writeNorm(double v);

template <>
uint8_t writeNorm<uint8_t>(double v) {
  return static_cast<uint8_t>(std::lround(clamp01(v) * 255.0));
}

template <>
uint16_t writeNorm<uint16_t>(double v) {
  return static_cast<uint16_t>(std::lround(clamp01(v) * 65535.0));
}

template <>
float writeNorm<float>(double v) {
  return static_cast<float>(v);
}

template <typename T>
Vec4 readTypedPixel(const ImageInfo &image, int x, int y) {
  if (!contains(image, x, y)) {
    return {};
  }
  const int n = componentCount(image.components);
  const auto *row = reinterpret_cast<const T *>(static_cast<const char *>(image.data) + static_cast<ptrdiff_t>(y - image.bounds[1]) * image.rowBytes);
  const T *px = row + static_cast<ptrdiff_t>(x - image.bounds[0]) * n;
  Vec4 out;
  out.r = readNorm<T>(px[0]);
  out.g = readNorm<T>(px[1]);
  out.b = readNorm<T>(px[2]);
  out.a = image.components == Components::RGBA ? readNorm<T>(px[3]) : 1.0;
  return out;
}

Vec4 readPixel(const ImageInfo &image, int x, int y) {
  switch (image.depth) {
    case PixelDepth::Byte:
      return readTypedPixel<uint8_t>(image, x, y);
    case PixelDepth::Short:
      return readTypedPixel<uint16_t>(image, x, y);
    case PixelDepth::Float:
      return readTypedPixel<float>(image, x, y);
    default:
      return {};
  }
}

template <typename T>
void writeTypedPixel(const ImageInfo &image, int x, int y, const Vec4 &value) {
  if (!contains(image, x, y)) {
    return;
  }
  const int n = componentCount(image.components);
  auto *row = reinterpret_cast<T *>(static_cast<char *>(image.data) + static_cast<ptrdiff_t>(y - image.bounds[1]) * image.rowBytes);
  T *px = row + static_cast<ptrdiff_t>(x - image.bounds[0]) * n;
  px[0] = writeNorm<T>(value.r);
  px[1] = writeNorm<T>(value.g);
  px[2] = writeNorm<T>(value.b);
  if (image.components == Components::RGBA) {
    px[3] = writeNorm<T>(value.a);
  }
}

void writePixel(const ImageInfo &image, int x, int y, const Vec4 &value) {
  switch (image.depth) {
    case PixelDepth::Byte:
      writeTypedPixel<uint8_t>(image, x, y, value);
      break;
    case PixelDepth::Short:
      writeTypedPixel<uint16_t>(image, x, y, value);
      break;
    case PixelDepth::Float:
      writeTypedPixel<float>(image, x, y, value);
      break;
    default:
      break;
  }
}

void setLabels(OfxPropertySetHandle props, const char *label, const char *shortLabel = nullptr, const char *longLabel = nullptr) {
  if (!props || !gProp) {
    return;
  }
  gProp->propSetString(props, kOfxPropLabel, 0, label);
  gProp->propSetString(props, kOfxPropShortLabel, 0, shortLabel ? shortLabel : label);
  gProp->propSetString(props, kOfxPropLongLabel, 0, longLabel ? longLabel : label);
}

void setHint(OfxPropertySetHandle props, const char *hint) {
  if (props && hint) {
    gProp->propSetString(props, kOfxParamPropHint, 0, hint);
  }
}

void setParent(OfxPropertySetHandle props, const char *parent) {
  if (props && parent) {
    gProp->propSetString(props, kOfxParamPropParent, 0, parent);
  }
}

void defineGroup(OfxParamSetHandle paramSet, const char *name, const char *label, bool open) {
  OfxPropertySetHandle props = nullptr;
  if (gParam->paramDefine(paramSet, kOfxParamTypeGroup, name, &props) == kOfxStatOK) {
    setLabels(props, label);
    gProp->propSetInt(props, kOfxParamPropGroupOpen, 0, open ? 1 : 0);
  }
}

void defineDouble(OfxParamSetHandle paramSet,
                  const char *name,
                  const char *label,
                  const char *parent,
                  double def,
                  double min,
                  double max,
                  double displayMin,
                  double displayMax) {
  OfxPropertySetHandle props = nullptr;
  if (gParam->paramDefine(paramSet, kOfxParamTypeDouble, name, &props) != kOfxStatOK) {
    return;
  }
  setLabels(props, label);
  setParent(props, parent);
  gProp->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
  gProp->propSetDouble(props, kOfxParamPropDefault, 0, def);
  gProp->propSetDouble(props, kOfxParamPropMin, 0, min);
  gProp->propSetDouble(props, kOfxParamPropMax, 0, max);
  gProp->propSetDouble(props, kOfxParamPropDisplayMin, 0, displayMin);
  gProp->propSetDouble(props, kOfxParamPropDisplayMax, 0, displayMax);
}

void defineChoiceStrings(OfxParamSetHandle paramSet,
                         const char *name,
                         const char *label,
                         const char *parent,
                         const std::vector<std::string> &options,
                         int def) {
  OfxPropertySetHandle props = nullptr;
  if (gParam->paramDefine(paramSet, kOfxParamTypeChoice, name, &props) != kOfxStatOK) {
    return;
  }
  setLabels(props, label);
  setParent(props, parent);
  for (size_t i = 0; i < options.size(); ++i) {
    gProp->propSetString(props, kOfxParamPropChoiceOption, static_cast<int>(i), options[i].c_str());
  }
  gProp->propSetInt(props, kOfxParamPropDefault, 0, def);
  gProp->propSetInt(props, kOfxParamPropEvaluateOnChange, 0, 1);
}

void defineHiddenString(OfxParamSetHandle paramSet, const char *name, const std::string &defaultValue) {
  OfxPropertySetHandle props = nullptr;
  if (gParam->paramDefine(paramSet, kOfxParamTypeString, name, &props) != kOfxStatOK) {
    return;
  }
  setLabels(props, name);
  gProp->propSetString(props, kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);
  gProp->propSetString(props, kOfxParamPropDefault, 0, defaultValue.c_str());
  gProp->propSetInt(props, kOfxParamPropSecret, 0, 1);
  gProp->propSetInt(props, kOfxParamPropPluginMayWrite, 0, 1);
  gProp->propSetInt(props, kOfxParamPropPersistant, 0, 1);
}

OfxStatus describe(OfxImageEffectHandle effect) {
  OfxPropertySetHandle props = nullptr;
  if (gEffect->getPropertySet(effect, &props) != kOfxStatOK) {
    return kOfxStatFailed;
  }

  setLabels(props, "LUT 分层管理", "LUT 分层管理", "LIDONGFILMS | LUT 分层管理");
  gProp->propSetString(props, kOfxPropPluginDescription, 0, "Layered .cube LUT control: luma, chroma, shadows, midtones and highlights.");
  gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "LIDONG 色彩工具");
  gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);
  gProp->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
  gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
  gProp->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
  gProp->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
  gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
  gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
  gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
  gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);
  gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
  gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentRGB);
  return kOfxStatOK;
}

void defineClip(OfxImageEffectHandle effect, const char *name) {
  OfxPropertySetHandle props = nullptr;
  if (gEffect->clipDefine(effect, name, &props) == kOfxStatOK) {
    gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentRGB);
  }
}

OfxStatus describeInContext(OfxImageEffectHandle effect) {
  defineClip(effect, kOfxImageEffectSimpleSourceClipName);
  defineClip(effect, kOfxImageEffectOutputClipName);

  OfxParamSetHandle paramSet = nullptr;
  if (gEffect->getParamSet(effect, &paramSet) != kOfxStatOK) {
    return kOfxStatFailed;
  }

  const std::vector<std::string> history = readLutHistory();
  const std::string saved = history.empty() ? std::string() : history.front();
  const std::vector<std::string> lutOptions = cubeChoiceLabels(history);

  defineGroup(paramSet, "lutFileGroup", "LUT 文件", true);
  defineChoiceStrings(paramSet, kParamCubeChoice, ".cube 路径", "lutFileGroup", lutOptions, choiceIndexForPath(saved, history));
  defineHiddenString(paramSet, kParamCubePath, saved);

  defineGroup(paramSet, "colorSpaceGroup", "色彩空间转换", true);
  defineChoiceStrings(paramSet,
                      kParamWorkingSpace,
                      "节点输入色彩空间",
                      "colorSpaceGroup",
                      colorSpaceLabels(),
                      0);
  std::vector<std::string> lutSpaceLabels = {"与节点输入相同（不转换）"};
  lutSpaceLabels.insert(lutSpaceLabels.end(), colorSpaceLabels().begin(), colorSpaceLabels().end());
  defineChoiceStrings(paramSet,
                      kParamLutWorkingSpace,
                      "LUT 色彩空间",
                      "colorSpaceGroup",
                      lutSpaceLabels,
                      0);
  {
    OfxParamHandle param = nullptr;
    OfxPropertySetHandle props = nullptr;
    if (gParam->paramGetHandle(paramSet, kParamWorkingSpace, &param, &props) == kOfxStatOK) {
      setHint(props, "选择进入当前 OFX 节点的色彩空间；色彩管理项目通常选择时间线色彩空间。");
    }
    if (gParam->paramGetHandle(paramSet, kParamLutWorkingSpace, &param, &props) == kOfxStatOK) {
      setHint(props, "选择 .cube LUT 预期的输入和输出色彩空间；插件会自动转换并在处理后转回节点空间。");
    }
  }

  defineGroup(paramSet, "splitStrengthGroup", "分离强度", true);
  defineDouble(paramSet, kParamLumaStrength, "亮度", "splitStrengthGroup", 1.0, 0.0, 2.0, 0.0, 1.5);
  defineDouble(paramSet, kParamColorStrength, "色彩", "splitStrengthGroup", 1.0, 0.0, 2.0, 0.0, 1.5);

  defineGroup(paramSet, "zoneStrengthGroup", "分区强度", true);
  defineDouble(paramSet, kParamShadowStrength, "暗部", "zoneStrengthGroup", 1.0, 0.0, 2.0, 0.0, 1.5);
  defineDouble(paramSet, kParamMidtoneStrength, "中间调", "zoneStrengthGroup", 1.0, 0.0, 2.0, 0.0, 1.5);
  defineDouble(paramSet, kParamHighlightStrength, "高光", "zoneStrengthGroup", 1.0, 0.0, 2.0, 0.0, 1.5);

  defineGroup(paramSet, "advancedGroup", "高级", true);
  defineDouble(paramSet, kParamNeutralBias, "中性色偏", "advancedGroup", 1.0, 0.0, 2.0, 0.0, 1.5);
  defineDouble(paramSet, kParamBlackLevel, "黑位修正", "advancedGroup", 1.0, 0.0, 2.0, 0.0, 1.5);
  defineDouble(paramSet, kParamWhiteRolloff, "白点滚降", "advancedGroup", 1.0, 0.0, 2.0, 0.0, 1.5);
  defineDouble(paramSet, kParamColorPurity, "色彩纯度", "advancedGroup", 1.0, 0.0, 2.0, 0.0, 1.5);
  defineDouble(paramSet, kParamDensityOffset, "密度偏移", "advancedGroup", 0.0, -2.0, 2.0, -0.5, 0.5);
  defineDouble(paramSet, kParamHueShift, "色相偏移", "advancedGroup", 0.0, -180.0, 180.0, -45.0, 45.0);

  return kOfxStatOK;
}

bool getParamHandle(OfxParamSetHandle paramSet, const char *name, OfxParamHandle &param) {
  return gParam->paramGetHandle(paramSet, name, &param, nullptr) == kOfxStatOK && param;
}

double getDoubleParam(OfxParamSetHandle paramSet, const char *name, OfxTime time, double fallback) {
  OfxParamHandle param = nullptr;
  if (!getParamHandle(paramSet, name, param)) {
    return fallback;
  }
  double value = fallback;
  if (gParam->paramGetValueAtTime(param, time, &value) != kOfxStatOK) {
    return fallback;
  }
  return value;
}

int getChoiceParam(OfxParamSetHandle paramSet, const char *name, OfxTime time, int fallback) {
  OfxParamHandle param = nullptr;
  if (!getParamHandle(paramSet, name, param)) {
    return fallback;
  }
  int value = fallback;
  if (gParam->paramGetValueAtTime(param, time, &value) != kOfxStatOK) {
    return fallback;
  }
  return value;
}

int getCurrentChoiceParam(OfxParamSetHandle paramSet, const char *name, int fallback) {
  OfxParamHandle param = nullptr;
  if (!getParamHandle(paramSet, name, param)) {
    return fallback;
  }
  int value = fallback;
  if (gParam->paramGetValue(param, &value) != kOfxStatOK) {
    return fallback;
  }
  return value;
}

std::string getStringParam(OfxParamSetHandle paramSet, const char *name, OfxTime time) {
  OfxParamHandle param = nullptr;
  if (!getParamHandle(paramSet, name, param)) {
    return {};
  }
  char *value = nullptr;
  if (gParam->paramGetValueAtTime(param, time, &value) != kOfxStatOK || !value) {
    return {};
  }
  return value;
}

std::string getCurrentStringParam(OfxParamSetHandle paramSet, const char *name) {
  OfxParamHandle param = nullptr;
  if (!getParamHandle(paramSet, name, param)) {
    return {};
  }
  char *value = nullptr;
  if (gParam->paramGetValue(param, &value) != kOfxStatOK || !value) {
    return {};
  }
  return value;
}

void setCurrentStringParam(OfxParamSetHandle paramSet, const char *name, const std::string &value) {
  OfxParamHandle param = nullptr;
  if (getParamHandle(paramSet, name, param)) {
    const std::string current = getCurrentStringParam(paramSet, name);
    if (current == value) {
      return;
    }
    gParam->paramSetValue(param, value.c_str());
  }
}

void setCurrentChoiceParam(OfxParamSetHandle paramSet, const char *name, int value) {
  OfxParamHandle param = nullptr;
  if (getParamHandle(paramSet, name, param)) {
    const int current = getCurrentChoiceParam(paramSet, name, value);
    if (current == value) {
      return;
    }
    gParam->paramSetValue(param, value);
  }
}

struct ScopedParamSync {
  ScopedParamSync() : previous(gParamSyncInProgress) { gParamSyncInProgress = true; }
  ~ScopedParamSync() { gParamSyncInProgress = previous; }
  bool previous = false;
};

void updateCubeChoiceOptions(OfxParamSetHandle paramSet, const std::vector<std::string> &history) {
  OfxParamHandle param = nullptr;
  OfxPropertySetHandle props = nullptr;
  if (gParam->paramGetHandle(paramSet, kParamCubeChoice, &param, &props) != kOfxStatOK || !props) {
    return;
  }

  const std::vector<std::string> labels = cubeChoiceLabels(history);
  for (size_t i = 0; i < labels.size(); ++i) {
    gProp->propSetString(props, kOfxParamPropChoiceOption, static_cast<int>(i), labels[i].c_str());
  }
}

RenderParams readRenderParams(OfxImageEffectHandle effect, OfxTime time) {
  RenderParams params;
  OfxParamSetHandle paramSet = nullptr;
  if (gEffect->getParamSet(effect, &paramSet) != kOfxStatOK || !paramSet) {
    return params;
  }

  params.cubePath = getStringParam(paramSet, kParamCubePath, time);
  if (params.cubePath.empty()) {
    const std::vector<std::string> history = readLutHistory();
    params.cubePath = pathForChoiceIndex(getChoiceParam(paramSet, kParamCubeChoice, time, 0), history);
  }
  params.nodeColorSpace = sanitizeColorSpace(getChoiceParam(paramSet, kParamWorkingSpace, time, 0));
  const int lutSpaceChoice = getChoiceParam(paramSet, kParamLutWorkingSpace, time, 0);
  params.lutColorSpace = lutSpaceChoice <= 0 ? params.nodeColorSpace : sanitizeColorSpace(lutSpaceChoice - 1);
  params.lumaStrength = getDoubleParam(paramSet, kParamLumaStrength, time, 1.0);
  params.colorStrength = getDoubleParam(paramSet, kParamColorStrength, time, 1.0);
  params.shadowStrength = getDoubleParam(paramSet, kParamShadowStrength, time, 1.0);
  params.midtoneStrength = getDoubleParam(paramSet, kParamMidtoneStrength, time, 1.0);
  params.highlightStrength = getDoubleParam(paramSet, kParamHighlightStrength, time, 1.0);
  params.neutralBias = getDoubleParam(paramSet, kParamNeutralBias, time, 1.0);
  params.blackLevel = getDoubleParam(paramSet, kParamBlackLevel, time, 1.0);
  params.whiteRolloff = getDoubleParam(paramSet, kParamWhiteRolloff, time, 1.0);
  params.colorPurity = getDoubleParam(paramSet, kParamColorPurity, time, 1.0);
  params.densityOffset = getDoubleParam(paramSet, kParamDensityOffset, time, 0.0);
  params.hueShift = getDoubleParam(paramSet, kParamHueShift, time, 0.0);
  return params;
}

OfxStatus getRegionOfDefinition(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
  OfxImageClipHandle srcClip = nullptr;
  if (gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &srcClip, nullptr) != kOfxStatOK || !srcClip) {
    return kOfxStatReplyDefault;
  }

  double time = 0.0;
  gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time);

  OfxRectD rod {};
  if (gEffect->clipGetRegionOfDefinition(srcClip, time, &rod) != kOfxStatOK) {
    return kOfxStatReplyDefault;
  }

  const double values[4] = {rod.x1, rod.y1, rod.x2, rod.y2};
  gProp->propSetDoubleN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, values);
  return kOfxStatOK;
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  double time = 0.0;
  int window[4] = {0, 0, 0, 0};
  if (gProp->propGetDouble(inArgs, kOfxPropTime, 0, &time) != kOfxStatOK) {
    return kOfxStatFailed;
  }
  if (gProp->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, window) != kOfxStatOK) {
    return kOfxStatFailed;
  }

  OfxImageClipHandle srcClip = nullptr;
  OfxImageClipHandle dstClip = nullptr;
  if (gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &srcClip, nullptr) != kOfxStatOK ||
      gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &dstClip, nullptr) != kOfxStatOK) {
    return kOfxStatFailed;
  }

  OfxPropertySetHandle srcImage = nullptr;
  OfxPropertySetHandle dstImage = nullptr;
  OfxStatus srcStatus = gEffect->clipGetImage(srcClip, time, nullptr, &srcImage);
  OfxStatus dstStatus = gEffect->clipGetImage(dstClip, time, nullptr, &dstImage);
  if (dstStatus != kOfxStatOK || !dstImage) {
    if (srcImage) {
      gEffect->clipReleaseImage(srcImage);
    }
    return kOfxStatFailed;
  }

  ImageInfo dstInfo;
  if (!getImageInfo(dstImage, dstInfo)) {
    if (srcImage) {
      gEffect->clipReleaseImage(srcImage);
    }
    gEffect->clipReleaseImage(dstImage);
    return kOfxStatFailed;
  }

  ImageInfo srcInfo;
  const bool hasSource = srcStatus == kOfxStatOK && srcImage && getImageInfo(srcImage, srcInfo);

  const int x1 = std::max(window[0], dstInfo.bounds[0]);
  const int y1 = std::max(window[1], dstInfo.bounds[1]);
  const int x2 = std::min(window[2], dstInfo.bounds[2]);
  const int y2 = std::min(window[3], dstInfo.bounds[3]);

  const RenderParams params = readRenderParams(effect, time);
  const LutData lut = getLut(params.cubePath);

  for (int y = y1; y < y2; ++y) {
    if ((y & 31) == 0 && gEffect->abort(effect)) {
      break;
    }
    for (int x = x1; x < x2; ++x) {
      Vec4 src = hasSource ? readPixel(srcInfo, x, y) : Vec4{};
      if (!lut.valid()) {
        writePixel(dstInfo, x, y, src);
        continue;
      }
      Vec3 layered = applyColorManagedLayeredLut({src.r, src.g, src.b}, lut, params);
      writePixel(dstInfo, x, y, {layered.r, layered.g, layered.b, src.a});
    }
  }

  if (srcImage) {
    gEffect->clipReleaseImage(srcImage);
  }
  gEffect->clipReleaseImage(dstImage);
  return kOfxStatOK;
}

OfxStatus onLoad() {
  if (!gHost || !gHost->fetchSuite) {
    return kOfxStatFailed;
  }

  gProp = reinterpret_cast<OfxPropertySuiteV1 *>(const_cast<void *>(gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1)));
  gEffect = reinterpret_cast<OfxImageEffectSuiteV1 *>(const_cast<void *>(gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1)));
  gParam = reinterpret_cast<OfxParameterSuiteV1 *>(const_cast<void *>(gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1)));

  if (!gProp || !gEffect || !gParam) {
    return kOfxStatFailed;
  }
  return kOfxStatOK;
}

OfxStatus onUnload() {
  std::lock_guard<std::mutex> lock(gCacheMutex);
  gLutCache.clear();
  return kOfxStatOK;
}

OfxStatus createInstance(OfxImageEffectHandle effect) {
  OfxParamSetHandle paramSet = nullptr;
  if (gEffect->getParamSet(effect, &paramSet) == kOfxStatOK && paramSet) {
    ScopedParamSync sync;
    const std::string current = getCurrentStringParam(paramSet, kParamCubePath);
    std::vector<std::string> history = readLutHistory();
    const std::string saved = history.empty() ? std::string() : history.front();
    if (current.empty() && !saved.empty()) {
      setCurrentStringParam(paramSet, kParamCubePath, saved);
      setCurrentChoiceParam(paramSet, kParamCubeChoice, choiceIndexForPath(saved, history));
    } else if (!current.empty()) {
      const int index = choiceIndexForPath(current, history);
      if (index > 0) {
        setCurrentChoiceParam(paramSet, kParamCubeChoice, index);
      }
    }
  }
  return kOfxStatOK;
}

OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  if (gParamSyncInProgress) {
    return kOfxStatOK;
  }

  char *changedName = nullptr;
  if (inArgs) {
    gProp->propGetString(inArgs, kOfxPropName, 0, &changedName);
  }

  if (changedName && std::string(changedName) == kParamCubeChoice) {
    OfxParamSetHandle paramSet = nullptr;
    if (gEffect->getParamSet(effect, &paramSet) == kOfxStatOK && paramSet) {
      std::vector<std::string> history = readLutHistory();
      const int choice = getCurrentChoiceParam(paramSet, kParamCubeChoice, 0);
      if (choice == 0) {
        const std::string current = getCurrentStringParam(paramSet, kParamCubePath);
        const std::string chosen = chooseCubeFile(current);
        if (!chosen.empty()) {
          writeSavedLutPath(chosen);
          history = readLutHistory();
          ScopedParamSync sync;
          updateCubeChoiceOptions(paramSet, history);
          setCurrentStringParam(paramSet, kParamCubePath, chosen);
          setCurrentChoiceParam(paramSet, kParamCubeChoice, choiceIndexForPath(chosen, history));
        }
      } else {
        const std::string selected = pathForChoiceIndex(choice, history);
        if (!selected.empty()) {
          ScopedParamSync sync;
          setCurrentStringParam(paramSet, kParamCubePath, selected);
          writeSavedLutPath(selected);
          history = readLutHistory();
          updateCubeChoiceOptions(paramSet, history);
          setCurrentChoiceParam(paramSet, kParamCubeChoice, choiceIndexForPath(selected, history));
        }
      }
    }
  } else if (changedName && std::string(changedName) == kParamCubePath) {
    OfxParamSetHandle paramSet = nullptr;
    if (gEffect->getParamSet(effect, &paramSet) == kOfxStatOK && paramSet) {
      const std::string current = getCurrentStringParam(paramSet, kParamCubePath);
      writeSavedLutPath(current);
      const std::vector<std::string> history = readLutHistory();
      ScopedParamSync sync;
      updateCubeChoiceOptions(paramSet, history);
      setCurrentChoiceParam(paramSet, kParamCubeChoice, choiceIndexForPath(current, history));
    }
  }

  std::lock_guard<std::mutex> lock(gCacheMutex);
  gLutCache.clear();
  return kOfxStatOK;
}

OfxStatus pluginMain(const char *action, const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
  try {
    const std::string a(action ? action : "");

    if (a == kOfxActionLoad) {
      return onLoad();
    }
    if (a == kOfxActionUnload) {
      return onUnload();
    }
    if (a == kOfxActionDescribe) {
      return describe(reinterpret_cast<OfxImageEffectHandle>(const_cast<void *>(handle)));
    }
    if (a == kOfxImageEffectActionDescribeInContext) {
      return describeInContext(reinterpret_cast<OfxImageEffectHandle>(const_cast<void *>(handle)));
    }
    if (a == kOfxActionCreateInstance) {
      return createInstance(reinterpret_cast<OfxImageEffectHandle>(const_cast<void *>(handle)));
    }
    if (a == kOfxActionInstanceChanged) {
      return instanceChanged(reinterpret_cast<OfxImageEffectHandle>(const_cast<void *>(handle)), inArgs);
    }
    if (a == kOfxActionDestroyInstance ||
        a == kOfxActionBeginInstanceChanged || a == kOfxActionEndInstanceChanged ||
        a == kOfxImageEffectActionBeginSequenceRender ||
        a == kOfxImageEffectActionEndSequenceRender) {
      return kOfxStatOK;
    }
    if (a == kOfxImageEffectActionGetRegionOfDefinition) {
      return getRegionOfDefinition(reinterpret_cast<OfxImageEffectHandle>(const_cast<void *>(handle)), inArgs, outArgs);
    }
    if (a == kOfxImageEffectActionRender) {
      return render(reinterpret_cast<OfxImageEffectHandle>(const_cast<void *>(handle)), inArgs);
    }

    return kOfxStatReplyDefault;
  } catch (...) {
    return kOfxStatFailed;
  }
}

void setHost(OfxHost *host) { gHost = host; }

OfxPlugin gPlugin = {
  kOfxImageEffectPluginApi,
  kOfxImageEffectPluginApiVersion,
  kPluginIdentifier,
  kVersionMajor,
  kVersionMinor,
  setHost,
  pluginMain,
};

}  // namespace

extern "C" {

__attribute__((visibility("default"))) OfxStatus OfxSetHost(const OfxHost *host) {
  gHost = const_cast<OfxHost *>(host);
  return kOfxStatOK;
}

__attribute__((visibility("default"))) int OfxGetNumberOfPlugins(void) { return 1; }

__attribute__((visibility("default"))) OfxPlugin *OfxGetPlugin(int nth) {
  return nth == 0 ? &gPlugin : nullptr;
}

}
