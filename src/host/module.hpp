#pragma once

// The recompiled game: the native module 3dsrecomp built from the ROM's code,
// checked against that code before anything runs.
#include "abi.hpp"

#include <cstdint>
#include <string>

namespace host {

class Module {
public:
  // loads the module and checks it was generated from `code` (the ROM's
  // code.bin, as mapped at `base`); false with the reason otherwise
  bool load(const std::string& path, const uint8_t* code, uint32_t base, uint32_t size, std::string& error);
  ~Module();

  // the recompiled code for an address (bit 0 set for Thumb), or null
  Code lookup(uint32_t address) const;
  uint32_t entry_count() const { return count_; }
  // what the module wants done before the process ends (a coverage build
  // writes its counts), as the host leaves with _Exit and no atexit runs
  void finish() const { if (finish_) finish_(); }

private:
  void* library_ = nullptr;
  void (*finish_)() = nullptr;
  const Entry* entries_ = nullptr;
  uint32_t count_ = 0;
};

} // namespace host
