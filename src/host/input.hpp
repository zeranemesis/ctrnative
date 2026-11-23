#pragma once

// Inputs a run plays by itself, for tests without a player, by frame:
//   CTRN_PRESS_A=1          A for 6 frames every 60
//   CTRN_TAP_AT=x,y         a touch there for 6 frames every 60 (bottom screen pixels)
//   CTRN_INPUT_SCRIPT=file  lines "from to BUTTON... [x=..] [y=..]": held on frames [from, to)
#include "services.hpp"

#include <vector>

namespace host {

class TestInput {
public:
  TestInput(); // reads the variables
  void apply(uint64_t frame, Services::Input& input) const;

private:
  struct Step {
    uint64_t from = 0, to = 0;
    uint32_t buttons = 0;
    float x = 0, y = 0;
  };
  bool press_a_ = false, tap_ = false;
  uint16_t tap_x_ = 0, tap_y_ = 0;
  std::vector<Step> script_;
};

} // namespace host
