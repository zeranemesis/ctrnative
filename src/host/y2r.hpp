#pragma once

// y2r:u, the YUV to RGB converter games play their videos through: the conversion
// runs at once when started (as Citra's), from the game's memory into it.
#include "kernel.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace host {

class Y2r {
public:
  // a y2r:u request (kernel lock held); false when a command is not handled
  bool handle(Kernel& kernel, uint32_t* command);

  // the conversion of a picture already in memory, for the self-check
  struct Stream {
    uint32_t address = 0, size = 0, unit = 0, gap = 0;
  };
  uint8_t input_format = 0, output_format = 0, rotation = 0, block = 0;
  uint16_t width = 0, lines = 0, alpha = 0xFF;
  int16_t coefficients[8] = {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}; // ITU-R BT.601
  Stream y, u, v, yuyv, out;
  void convert(Kernel& kernel);

private:
  std::shared_ptr<Event> done_;
  bool interrupt_ = false;
};

} // namespace host
