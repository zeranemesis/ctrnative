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
#include <mutex>
#include <unordered_map>
#include <unordered_set>
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
    kViewport = 14,
    kScissor = 15,
    kCopyUnaligned = 16, // copies: asked only for a block copied where the next command goes
    kCopyAligned = 17,
    kCopy = 18,
    kGfxCamera = 19,
    kGfxUniformBlock = 20,
    kGfxRows3 = 21,
    kGfxRows4 = 22,
    kGfxVec3 = 23,
    kNngxSaveStart = 24, // notices: the game's own function runs after
    kNngxSaveStop = 25,
    kNngxClear = 26,
    kGfxRows3Data = 27,
  };

  GfxNatives(Kernel& kernel, pn_engine* engine);
  // a call handed over: true when done here, false to run the game's own function
  bool call(Thread& thread, uint32_t which);
  // how often each one was called, and how often done here
  void report() const;
  // a frame went by: what only past frames' lists held is freed
  void frame();

private:
  struct Program;
  static void run(void* self, uint32_t operation);
  // the program's operation (one done before is reused)
  uint32_t operation(const Program& program);
  // the program as an operation, marked at the game's command cursor
  void emit(Program& program);
  // an operation's mark written at `where` (a place the game reserved in its list); the word after it
  uint32_t mark_at(uint32_t where, uint32_t operation, uint64_t holder = 0);
  // who keeps operations alive: a frame's lists, a recorded list, a text's list; one lives while one holds it
  uint64_t holder() const;
  void hold(uint64_t who, uint32_t id);
  void release(uint64_t who);
  void unref(uint32_t id);
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
  // a block in memory as an operation, translated once (0: not one)
  uint32_t translated(uint32_t address, uint32_t size);
  bool list_copy(uint32_t destination, uint32_t source, uint32_t size);
  struct Palette {
    uint32_t start = 0, next = 0, rows = 0, first = 0;
    bool geometry = false;
    std::vector<uint32_t> words;
  } palette_;
  bool palette_rows(uint32_t where, uint32_t source, bool first, uint32_t header);
  bool uniform_rows(uint32_t where, uint32_t source, uint32_t rows, uint32_t header, bool vec3);
  bool nngx_add(Program& program, uint32_t function, uint32_t address, uint32_t size, bool copy);
  bool lyt_block(Program& program, uint32_t block);
  bool lyt_begin(Program& program, uint32_t function, uint32_t drawer, uint32_t flag);
  bool lyt_textures(Program& program, uint32_t function, uint32_t drawer, uint32_t material, bool flag);

  Kernel& kernel_;
  pn_engine* engine_;
  // operations by number - 1; by what they do, to reuse one
  // shortcut: never forgotten; menus animating make new ones for good, evict the unused if memory grows
  struct Op {
    std::vector<uint32_t> words;
    uint32_t refs = 0, generation = 0;
    bool live = false;
  };
  std::vector<Op> ops_;        // by number - 1
  std::vector<uint32_t> free_; // numbers to use again
  size_t live_ = 0;
  std::map<std::vector<uint32_t>, uint32_t> known_;
  std::unordered_map<uint64_t, std::unordered_set<uint32_t>> holders_;
  uint64_t frame_ = 0;
  uint32_t recording_ = 0;     // the list nngx records, or 0
  std::recursive_mutex mutex_; // game threads call, picanative runs (an operation may run others)
  std::map<uint32_t, uint64_t> calls_, done_;
  uint64_t unknown_marks_ = 0;
  // blocks translated once: (address, size) → their content's hash and operation (while it lives)
  struct Translated {
    uint64_t hash = 0;
    uint32_t id = 0, generation = 0;
  };
  std::map<std::pair<uint32_t, uint32_t>, Translated> translated_;
  uint64_t uncopied_words_ = 0, translated_words_ = 0, copied_words_ = 0;
};

} // namespace host
