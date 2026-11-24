#pragma once

// Inputs a run plays by itself, for tests without a player, by frame:
//   CTRN_PRESS_A=1          A for 6 frames every 60
//   CTRN_TAP_AT=x,y         a touch there for 6 frames every 60 (bottom screen pixels)
//   CTRN_INPUT_SCRIPT=file  lines "from to BUTTON... [x=..] [y=..] [touch=x,y]": held on frames [from, to)
//                           (x, y: the circle pad, -1 to 1; touch: a bottom screen pixel, held as long)
//                           also "w32=ADDRESS:VALUE" / "w8=ADDRESS:VALUE" (hex): written into the game's memory on each
//                           of those frames, to put a test where a player would take hours (a level, a course);
//                           ADDRESS "*P+O" is O past the pointer stored at P
#include "memory.hpp"
#include "services.hpp"

#include <vector>

namespace host {

class TestInput {
public:
  TestInput(); // reads the variables
  void apply(uint64_t frame, Services::Input& input) const;
  void write(uint64_t frame, Memory& memory) const; // the script's writes for that frame

private:
  struct Step {
    uint64_t from = 0, to = 0;
    uint32_t buttons = 0;
    float x = 0, y = 0;
    bool touch = false;
    uint16_t touch_x = 0, touch_y = 0;
    struct Write { uint32_t pointer = 0, address = 0, value = 0, size = 4; bool indirect = false; };
    std::vector<Write> writes;
  };
  bool press_a_ = false, tap_ = false;
  uint16_t tap_x_ = 0, tap_y_ = 0;
  std::vector<Step> script_;
};

} // namespace host
