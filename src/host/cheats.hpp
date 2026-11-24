#pragma once

// Cheats as Luma3DS (and Gateway, Citra) have them: Action Replay codes in a text file, "[name]" then lines of two
// hexadecimal words. CTRN_CHEATS=file names the file; CTRN_CHEATS_ON=name;name the codes that run (none by
// default: the game as it is). Each frame, each code on runs once, writing the game's memory.
#include "memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace host {

class Cheats {
public:
  Cheats(); // reads the variables
  bool any() const { return !codes_.empty(); }
  // the codes on, once (a frame); keys: the buttons held now (for the DD code)
  void run(Memory& memory, uint32_t keys) const;
  // a code's name and whether it is on, for a menu
  std::vector<std::pair<std::string, bool>> list() const;

private:
  struct Code {
    std::string name;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    bool on = false;
  };
  std::vector<Code> codes_;
  void run(const Code& code, Memory& memory, uint32_t keys) const;
};

} // namespace host
