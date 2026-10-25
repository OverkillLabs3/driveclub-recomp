// Regression test for the head camera choices (launcher/headcam.cpp):
//   clang++ -std=c++20 -O2 tools/headcam_test.cpp launcher/headcam.cpp -o headcam_test.exe && ./headcam_test.exe
// With HEADCAM_REAL=<an extracted driver_head_camera_params.txt> it also checks the game's own file.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "../launcher/headcam.h"

// Same layout as the game's file (CRLF, tabs, comments, the IsHMD block, three shake lines that share their first
// word, a trailing space), with made-up values: no game data in the repository.
static const char* kSample =
    "* Driver head cam\r\n"
    "\r\n"
    "if IsHMD\r\n"
    "\tROTATION_OFFSET    0.0    0.0        0.0\r\n"
    "else\r\n"
    "\tROTATION_OFFSET\t   0.0\t  -1.4\t     0.0\r\n"
    "endif\r\n"
    "\r\n"
    "HIGH_SPEED_SETTING_MPH\t\t\t110.0\r\n"
    "FOCAL_LENGTH_AT_REST_MM\t\t\t14.01\r\n"
    "FOCAL_LENGTH_AT_HIGH_SPEED_MM\t\t14\r\n"
    "LOOKAT_HEAD_FRACTION_HEADING 0.98\r\n"
    "LOOKAT_HEAD_FRACTION_PITCH 0.98\r\n"
    "LOOKAT_HEAD_SMOOTHING 0.8\r\n"
    "LOOKAT_CLAMP_UP\t\t\t8\r\n"
    "LOOKAT_CLAMP_DOWN\t\t16\r\n"
    "LOOKAT_CLAMP_LEFT\t\t35\r\n"
    "LOOKAT_CLAMP_RIGHT\t\t35\r\n"
    "NECK_TRANSLATION_LIMIT_MIN -0.05 -0.05 -0.1\r\n"
    "* Factor in the vehicle's velocity to a look-at direction, to look into skids.\r\n"
    "VELOCITY_LOOKAT_FACTOR 0.\r\n"
    "YAW_LOOKAT_FACTOR 0.\r\n"
    "YAW_LOOKAT_SMOOTHING 0.97\r\n"
    "NECK_SPRING_CONSTANTS_NEG  1500  1200  500\r\n"
    "HEAD_SPRING_CONSTANTS_NEG  4000  2400  3000 \r\n"
    "NECK_SPRING_CONSTANTS_POS  1500  1200  500\r\n"
    "HEAD_SPRING_CONSTANTS_POS  4000  2400  3000\r\n"
    "NECK_DAMPER_CONSTANTS 50 25 100\r\n"
    "HEAD_DAMPER_CONSTANTS 50 50 50\r\n"
    "CAMERA_SHAKE_INPUT_MODIFIER Speed 0.0 0.075 0.05 0.0   0.0   0.0   0.0 0.0 0.0\r\n"
    "CAMERA_SHAKE_INPUT_MODIFIER Collision 0.0 0.075 0.05 0.0   0.0   0.0   0.0 0.0 0.0\r\n"
    "CAMERA_SHAKE_INPUT_MODIFIER ObjectImpact 0.0 0.075 0.05 0.0   0.0   0.0   0.0 0.0 0.0\r\n"
    "ROLL_DAMPING\t\t21";

// The line that starts with `key` (a key with a second word, e.g. "CAMERA_SHAKE_INPUT_MODIFIER Speed", matches that line).
static std::string Line(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  std::string line, found;
  int count = 0;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos) continue;
    std::string rest = line.substr(a);
    if (rest.compare(0, key.size(), key) == 0 && (rest.size() == key.size() || rest[key.size()] == ' ' ||
                                                  rest[key.size()] == '\t')) {
      found = line;
      ++count;
    }
  }
  if (count != 1) {
    std::fprintf(stderr, "expected one line '%s', found %d\n", key.c_str(), count);
    std::exit(1);
  }
  return found;
}

static std::vector<double> Numbers(const std::string& line) {
  static const std::regex num(R"([-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)");
  std::vector<double> out;
  for (auto it = std::sregex_iterator(line.begin(), line.end(), num); it != std::sregex_iterator(); ++it)
    out.push_back(std::stod(it->str()));
  return out;
}

static void Expect(const std::string& text, const std::string& key, std::vector<double> want) {
  const auto got = Numbers(Line(text, key));
  bool ok = got.size() == want.size();
  for (size_t i = 0; ok && i < got.size(); ++i) ok = std::fabs(got[i] - want[i]) < 1e-6;
  if (!ok) {
    std::fprintf(stderr, "%s: got", key.c_str());
    for (double v : got) std::fprintf(stderr, " %g", v);
    std::fprintf(stderr, ", want");
    for (double v : want) std::fprintf(stderr, " %g", v);
    std::fprintf(stderr, "\n");
    std::exit(1);
  }
}

// Every assignment line's first word (and second word when it is a name), as a multiset.
static std::map<std::string, int> Keys(const std::string& text) {
  static const std::regex key(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)(?:\s+([A-Za-z_][A-Za-z0-9_]*))?\s+[-+.\d])");
  std::map<std::string, int> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_search(line, m, key)) ++out[m[1].str() + (m[2].matched ? " " + m[2].str() : "")];
  }
  return out;
}

// Two choices: head look and head weight (the shake and the speed field of view were measured and left out).
static_assert(sizeof(HeadCamChoice) == 2 * sizeof(int), "HeadCamChoice holds look and weight only");

static HeadCamChoice Only(int HeadCamChoice::*field, int percent) {
  HeadCamChoice c;
  c.*field = percent;
  return c;
}

int main() {
  const std::string sample = kSample;

  // All at 0: the choice is the game's default and the text comes back byte for byte.
  assert(HeadCamChoice{}.Default());
  assert(HeadCamApply(sample, HeadCamChoice{}) == sample);
  assert(!Only(&HeadCamChoice::look, 1).Default());

  // Head look at 100%: the head fully into the direction of travel (velocity 1.0), half of the "violent" yaw term
  // (0.5), up to 90 degrees to the sides. Measured on a hairpin track with the AI driving: weaker values (0.45 / 0.12)
  // could not be told apart from the game's own camera. Pitch and the up/down limits stay as the file has them.
  {
    const auto out = HeadCamApply(sample, Only(&HeadCamChoice::look, 100));
    Expect(out, "VELOCITY_LOOKAT_FACTOR", {1});
    Expect(out, "YAW_LOOKAT_FACTOR", {0.5});
    Expect(out, "LOOKAT_HEAD_FRACTION_HEADING", {1});
    Expect(out, "LOOKAT_HEAD_SMOOTHING", {0.6});
    Expect(out, "LOOKAT_CLAMP_LEFT", {90});
    Expect(out, "LOOKAT_CLAMP_RIGHT", {90});
    assert(Line(out, "LOOKAT_HEAD_FRACTION_PITCH") == Line(sample, "LOOKAT_HEAD_FRACTION_PITCH"));
    assert(Line(out, "LOOKAT_CLAMP_UP") == Line(sample, "LOOKAT_CLAMP_UP"));
    assert(Line(out, "LOOKAT_CLAMP_DOWN") == Line(sample, "LOOKAT_CLAMP_DOWN"));
    assert(Line(out, "YAW_LOOKAT_SMOOTHING") == Line(sample, "YAW_LOOKAT_SMOOTHING"));
    assert(Line(out, "NECK_SPRING_CONSTANTS_NEG") == Line(sample, "NECK_SPRING_CONSTANTS_NEG"));
  }
  // Head look at 50%: half way from the file's value.
  {
    const auto out = HeadCamApply(sample, Only(&HeadCamChoice::look, 50));
    Expect(out, "VELOCITY_LOOKAT_FACTOR", {0.5});
    Expect(out, "YAW_LOOKAT_FACTOR", {0.25});
    Expect(out, "LOOKAT_CLAMP_LEFT", {62.5});
    Expect(out, "LOOKAT_HEAD_SMOOTHING", {0.7});
  }

  // Head weight: the four springs scale from x1 (0%) to x0.25 (100%); the dampers stay.
  {
    const auto out = HeadCamApply(sample, Only(&HeadCamChoice::weight, 100));
    Expect(out, "NECK_SPRING_CONSTANTS_NEG", {375, 300, 125});
    Expect(out, "NECK_SPRING_CONSTANTS_POS", {375, 300, 125});
    Expect(out, "HEAD_SPRING_CONSTANTS_NEG", {1000, 600, 750});
    Expect(out, "HEAD_SPRING_CONSTANTS_POS", {1000, 600, 750});
    assert(Line(out, "NECK_DAMPER_CONSTANTS") == Line(sample, "NECK_DAMPER_CONSTANTS"));
    assert(Line(out, "HEAD_DAMPER_CONSTANTS") == Line(sample, "HEAD_DAMPER_CONSTANTS"));
    const auto half = HeadCamApply(sample, Only(&HeadCamChoice::weight, 50));
    Expect(half, "NECK_SPRING_CONSTANTS_NEG", {937.5, 750, 312.5});
  }

  // Neither the field of view (the launcher has its own slider) nor the camera shake (the chase camera's values in the
  // cockpit could not be told apart from the game's own in the game) is the head camera's business: no choice
  // touches them, and the three shake lines that share their first word stay as they are.
  {
    const auto out = HeadCamApply(sample, HeadCamChoice{100, 100});
    for (const char* key : {"FOCAL_LENGTH_AT_HIGH_SPEED_MM", "HIGH_SPEED_SETTING_MPH", "FOCAL_LENGTH_AT_REST_MM",
                            "CAMERA_SHAKE_INPUT_MODIFIER Speed", "CAMERA_SHAKE_INPUT_MODIFIER Collision",
                            "CAMERA_SHAKE_INPUT_MODIFIER ObjectImpact"})
      assert(Line(out, key) == Line(sample, key));
  }

  // A line with three numbers gets all three (not the last one repeated).
  {
    const auto out = HeadCamApply(sample, Only(&HeadCamChoice::weight, 100));
    Expect(out, "HEAD_SPRING_CONSTANTS_NEG", {1000, 600, 750});
  }

  // Everything at 100%: same keys, CRLF kept, the IsHMD block and comments untouched, no NaN, no stray dots.
  {
    HeadCamChoice all{100, 100};
    const auto out = HeadCamApply(sample, all);
    assert(Keys(out) == Keys(sample));
    assert(out.find("\r\n") != std::string::npos && out.find('\n') == out.find("\r\n") + 1);
    assert(out.find("if IsHMD\r\n\tROTATION_OFFSET    0.0    0.0        0.0\r\n") != std::string::npos);
    assert(out.find("* Factor in the vehicle's velocity") != std::string::npos);
    assert(out.find("nan") == std::string::npos && out.find("inf") == std::string::npos);
    // the game's file has no newline after its last line: neither does the copy
    const std::string last = "ROLL_DAMPING\t\t21";
    assert(out.size() >= last.size() && out.compare(out.size() - last.size(), last.size(), last) == 0);
  }

  // Out of range percentages are clamped to 0..100.
  assert(HeadCamApply(sample, Only(&HeadCamChoice::look, -5)) == sample);
  assert(HeadCamApply(sample, Only(&HeadCamChoice::look, 250)) ==
         HeadCamApply(sample, Only(&HeadCamChoice::look, 100)));

  // The game's own file, when given: same keys at 100%, and byte for byte at 0%.
  if (const char* real = std::getenv("HEADCAM_REAL")) {
    std::ifstream f(real, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    assert(!text.empty());
    assert(HeadCamApply(text, HeadCamChoice{}) == text);
    assert(Keys(HeadCamApply(text, HeadCamChoice{100, 100})) == Keys(text));
    std::printf("game file: %zu bytes, %zu keys checked\n", text.size(), Keys(text).size());
  }

  std::printf("headcam_test: all passed\n");
  return 0;
}
