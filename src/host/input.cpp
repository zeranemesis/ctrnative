#include "input.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace host {

TestInput::TestInput() {
  const char* a = std::getenv("CTRN_PRESS_A");
  press_a_ = a && std::strcmp(a, "1") == 0;
  if (const char* at = std::getenv("CTRN_TAP_AT")) {
    unsigned x = 0, y = 0;
    tap_ = std::sscanf(at, "%u,%u", &x, &y) == 2 && x < 320 && y < 240;
    tap_x_ = static_cast<uint16_t>(x), tap_y_ = static_cast<uint16_t>(y);
  }
  const char* path = std::getenv("CTRN_INPUT_SCRIPT");
  if (!path) return;
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "[input] no script at %s\n", path);
    return;
  }
  static const struct { const char* name; uint32_t bit; } names[] = {
      {"A", 1u << 0}, {"B", 1u << 1}, {"SELECT", 1u << 2}, {"START", 1u << 3}, {"RIGHT", 1u << 4}, {"LEFT", 1u << 5},
      {"UP", 1u << 6}, {"DOWN", 1u << 7}, {"R", 1u << 8}, {"L", 1u << 9}, {"X", 1u << 10}, {"Y", 1u << 11},
      {"ZL", 1u << 14}, {"ZR", 1u << 15},
  };
  for (std::string line; std::getline(in, line);) {
    line = line.substr(0, line.find('#'));
    std::istringstream words(line);
    Step s;
    if (!(words >> s.from >> s.to)) continue;
    for (std::string w; words >> w;) {
      if (w.rfind("x=", 0) == 0) { s.x = std::strtof(w.c_str() + 2, nullptr); continue; }
      if (w.rfind("y=", 0) == 0) { s.y = std::strtof(w.c_str() + 2, nullptr); continue; }
      if (w.rfind("touch=", 0) == 0) {
        unsigned tx = 0, ty = 0;
        s.touch = std::sscanf(w.c_str() + 6, "%u,%u", &tx, &ty) == 2 && tx < 320 && ty < 240;
        if (!s.touch) std::fprintf(stderr, "[input] a touch off the bottom screen: %s\n", w.c_str());
        s.touch_x = static_cast<uint16_t>(tx), s.touch_y = static_cast<uint16_t>(ty);
        continue;
      }
      bool known = false;
      for (const auto& n : names)
        if (w == n.name) s.buttons |= n.bit, known = true;
      if (!known) std::fprintf(stderr, "[input] unknown button in the script: %s\n", w.c_str());
    }
    script_.push_back(s);
  }
  std::fprintf(stderr, "[input] script: %zu ranges (%s)\n", script_.size(), path);
}

void TestInput::apply(uint64_t frame, Services::Input& input) const {
  const bool beat = frame % 60 < 6;
  if (press_a_ && beat) input.buttons |= 1u;
  if (tap_ && beat && !input.touching) input.touching = true, input.touch_x = tap_x_, input.touch_y = tap_y_;
  for (const Step& s : script_)
    if (frame >= s.from && frame < s.to) {
      input.buttons |= s.buttons, input.circle_x += s.x, input.circle_y += s.y;
      if (s.touch) input.touching = true, input.touch_x = s.touch_x, input.touch_y = s.touch_y;
    }
}

} // namespace host
