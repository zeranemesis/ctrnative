#pragma once

// The graphics libraries' functions done natively, in place of the PICA200 commands they would make (see
// docs/RENDU-NATIF.md): the launcher finds them in a game's code by their bytes and replaces each with an override
// that hands the call here (svc 0xF0, r12: which one). One done here becomes a native operation of the host's, which
// the override marks in the game's command list (two words, through the game's own nngxAdd3DCommand, so that lists
// the game records and replays carry it too); picanative runs it where the mark is. One not done yet is counted and
// runs as the game's own code.
#include "kernel.hpp"

#include <picanative/api.h>

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace host {

class GfxNatives {
public:
  // the natives, numbered as tools/ctrn.py numbers them
  enum : uint32_t { kNngxCopyPreparedList = 1, kLytFlushBatch = 2 };

  GfxNatives(Kernel& kernel, pn_engine* engine);
  // a call handed over: true when done here (r12 then holds the operation to mark, 0 for none), false to run the
  // game's own function
  bool call(Thread& thread, uint32_t which);
  // how often each one was called, and how often done here
  void report() const;

private:
  using Operation = std::function<void(pn_engine*)>;
  // a native operation kept for the marks that name it (a recorded list replays them every frame); its number
  uint32_t add(Operation operation);
  static void run(void* self, uint32_t operation);
  // the mark of an operation, written where the game writes its commands (the word at cursor_at: the next free one)
  void mark(uint32_t cursor_at, uint32_t operation);
  // a register write; those of a command block in the game's constant data, read once
  struct Write {
    uint32_t reg, value, mask;
  };
  const std::vector<Write>& static_block(uint32_t address, uint32_t bytes);
  bool lyt_flush(Thread& thread);

  Kernel& kernel_;
  std::map<uint32_t, std::vector<Write>> blocks_;
  // operations by what they do (the native's number, then its data): one drawn again is marked again
  // shortcut: never forgotten; a game animating its menus makes new ones for good, evict the unused if memory grows
  std::map<std::vector<uint32_t>, uint32_t> known_;
  pn_engine* engine_;
  std::vector<Operation> operations_; // by number - 1
  std::map<uint32_t, uint64_t> calls_, done_;
  uint64_t unknown_marks_ = 0;
};

} // namespace host
