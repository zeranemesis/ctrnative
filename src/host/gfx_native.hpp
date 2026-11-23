#pragma once

// The graphics libraries' functions done natively, in place of the PICA200 commands they would make (see
// docs/RENDU-NATIF.md): the launcher finds them in a game's code by their bytes and replaces each with an override
// that hands the call here (svc 0xF0, r12: which one). One not done yet is counted and runs as the game's own code.
#include "kernel.hpp"

#include <cstdint>
#include <map>

namespace host {

class GfxNatives {
public:
  // the natives, numbered as tools/ctrn.py numbers them
  enum : uint32_t { kNngxCopyPreparedList = 1 };

  explicit GfxNatives(Kernel& kernel) : kernel_(kernel) {}
  // a call handed over: true when done here, false to run the game's own function
  bool call(Thread& thread, uint32_t which);
  // how often each one was called, and how often done here
  void report() const;

private:
  Kernel& kernel_;
  std::map<uint32_t, uint64_t> calls_, done_;
};

} // namespace host
