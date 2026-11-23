#pragma once

// The graphics libraries' functions done natively, in place of the PICA200 commands they would make (see
// docs/RENDU-NATIF.md): the launcher finds them in a game's code by their bytes and replaces each with an override
// that hands the call here (svc 0xF0, r12: which one, r15: the function). One done here becomes a native operation
// of the host's: what it sets in picanative, directly. Its mark (register 0x2F0, which the PICA200 has not) goes where
// the game writes its commands, so that lists the game records and replays carry it too; picanative runs the
// operation where the mark is. One not done (yet) runs as the game's own code.
#include "kernel.hpp"

#include <picanative/api.h>

#include <cstdint>
#include <map>
#include <vector>

namespace host {

class GfxNatives {
public:
  // the natives, numbered as tools/ctrn.py numbers them
  enum : uint32_t {
    kNngxCopyPreparedList = 1,
    kLytFlushBatch = 2,
    kLytMaterial = 3,
    kLytTextures = 4,
    kLytMatrices = 5,
    kFontMatrix = 6,
    kFontColors = 7,
    kFontBuilt = 8, // after the game's function
    kLytDrawBegin = 9,
    kLytDrawEnd = 10,
    kLytBeginProjected = 11,
    kNngxAdd3DCommand = 12,
    kNngxAddBlock = 13,
  };

  GfxNatives(Kernel& kernel, pn_engine* engine);
  // a call handed over: true when done here, false to run the game's own function
  bool call(Thread& thread, uint32_t which);
  // how often each one was called, and how often done here
  void report() const;

private:
  struct Program;
  static void run(void* self, uint32_t operation);
  // the program's operation (one done before is reused)
  uint32_t operation(const Program& program);
  // the program as an operation, marked at the game's command cursor
  void emit(Program& program);
  // what a block of commands the game keeps sets; false if it runs other lists
  bool commands(Program& program, uint32_t address, uint32_t bytes);

  // NSMB2's nw::lyt drawer (docs/NSMB2-LYT.md): its addresses, learnt from its batch flush
  struct Lyt {
    uint32_t flush = 0, cursor = 0, draw_block = 0, draw_begin = 0;
  } lyt_;
  void learn_lyt(uint32_t flush);
  void lyt_flush(Program& program, uint32_t drawer);
  void lyt_flush_pending(Program& program, uint32_t drawer);
  bool lyt_material(Program& program, uint32_t function, uint32_t drawer, uint32_t material);
  bool nngx_add(Program& program, uint32_t function, uint32_t address, uint32_t size, bool copy);
  bool lyt_block(Program& program, uint32_t block);
  bool lyt_begin(Program& program, uint32_t function, uint32_t drawer, uint32_t flag);
  bool lyt_textures(Program& program, uint32_t function, uint32_t drawer, uint32_t material, bool flag);

  Kernel& kernel_;
  pn_engine* engine_;
  // operations by number - 1; by what they do, to reuse one
  // shortcut: never forgotten; menus animating make new ones for good, evict the unused if memory grows
  std::vector<std::vector<uint32_t>> operations_;
  std::map<std::vector<uint32_t>, uint32_t> known_;
  std::map<uint32_t, uint64_t> calls_, done_;
  uint64_t unknown_marks_ = 0;
};

} // namespace host
