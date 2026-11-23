#include "gfx_native.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace host {

namespace {

// CTRN_GFX_PROFILE: where the natives' time goes (host side when called, picanative side when run)
const bool kProfile = std::getenv("CTRN_GFX_PROFILE") != nullptr;
struct Clocked {
  double& total;
  std::chrono::steady_clock::time_point start;
  explicit Clocked(double& t) : total(t), start(kProfile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
  ~Clocked() {
    if (kProfile) total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }
};

const char* name(uint32_t which) {
  switch (which) {
  case GfxNatives::kNngxCopyPreparedList: return "nngx: a prepared list copied into the current one";
  case GfxNatives::kLytFlushBatch: return "nw::lyt: a batch of rectangles drawn";
  case GfxNatives::kLytMaterial: return "nw::lyt: a material set up";
  case GfxNatives::kLytTextures: return "nw::lyt: a material's textures set";
  case GfxNatives::kLytMatrices: return "nw::lyt: pending matrices and colors sent";
  case GfxNatives::kFontMatrix: return "nw::font: a text's matrix sent";
  case GfxNatives::kFontColors: return "nw::font: a text's colors sent";
  case GfxNatives::kFontBuilt: return "nw::font: a text's list built, translated once";
  case GfxNatives::kLytDrawBegin: return "nw::lyt: drawing begun";
  case GfxNatives::kLytDrawEnd: return "nw::lyt: drawing ended";
  case GfxNatives::kLytBeginProjected: return "nw::lyt: drawing begun with a projection";
  case GfxNatives::kNngxAdd3DCommand: return "nngx: a block of commands added (copied)";
  case GfxNatives::kNngxAddBlock: return "nngx: a block {address, size, copied} added";
  case GfxNatives::kViewport: return "NSMB2: a viewport set";
  case GfxNatives::kAroundFirst + 0: return "nw::gfx: a material activated (simple), around";
  case GfxNatives::kAroundFirst + 1: return "nw::gfx: a material activated, around";
  case GfxNatives::kAroundFirst + 2: return "nw::gfx: lighting tables' inputs, around";
  case GfxNatives::kAroundFirst + 3: return "nw::gfx: texture coordinates, around";
  case GfxNatives::kAroundFirst + 4: return "nw::gfx: light environment, around";
  case GfxNatives::kAroundFirst + 5: return "NSMB2: a render target (translated after)";
  case GfxNatives::kAroundFirst + 6: return "nw::gfx: a 312-byte block copied";
  case GfxNatives::kAroundFirst + 7: return "nngx: the list split, around";
  case GfxNatives::kAroundFirst + 8: return "nw::gfx: a light's parameters, around";
  case GfxNatives::kAroundFirst + 9: return "nw::gfx: fog, around";
  case GfxNatives::kAroundFirst + 10: return "NSMB2: an effect's matrix, around";
  case GfxNatives::kGameDraw: return "NSMB2: an indexed draw";
  case GfxNatives::kGameVertexFormat: return "NSMB2: a vertex format set (translated after)";
  case GfxNatives::kGfxCamera: return "nw::gfx: a camera's matrices sent";
  case GfxNatives::kGfxRows3: return "nw::gfx: a 3x4 matrix sent";
  case GfxNatives::kGfxRows4: return "nw::gfx: a 4x4 matrix sent";
  case GfxNatives::kGfxVec3: return "nw::gfx: a vector sent";
  case GfxNatives::kGfxVec4: return "nw::gfx: a vec4 sent";
  case GfxNatives::kGfxRows3Data: return "nw::gfx: a bone's matrix sent (palette)";
  case GfxNatives::kNngxSaveStart: return "nngx: a recording started";
  case GfxNatives::kNngxSaveStop: return "nngx: a recording stopped";
  case GfxNatives::kNngxClear: return "nngx: a list cleared";
  case GfxNatives::kGfxUniformBlock: return "nw::gfx: a shader's booleans and integers sent";
  case GfxNatives::kCopyUnaligned: return "__aeabi_memcpy (unaligned): a block copied into the list";
  case GfxNatives::kCopyAligned: return "__aeabi_memcpy4: a block copied into the list";
  case GfxNatives::kCopy: return "__aeabi_memcpy: a block copied into the list";
  case GfxNatives::kScissor: return "NSMB2: a scissor set";
  default: return "?";
  }
}

// the target of the `b`/`bl` at `at`
uint32_t branch_target(const Memory& m, uint32_t at) {
  const uint32_t word = m.read32(at);
  return at + 8 + (static_cast<uint32_t>(static_cast<int32_t>(word << 8) >> 6));
}

// the constant an `ldr rN, [pc, #i]` at `at` loads
uint32_t literal(const Memory& m, uint32_t at) { return m.read32(at + 8 + (m.read32(at) & 0xFFF)); }

uint8_t read8(const Memory& m, uint32_t at) {
  uint8_t b = 0;
  m.read(at, &b, 1);
  return b;
}
void write8(Memory& m, uint32_t at, uint8_t b) { m.write(at, &b, 1); }

// NSMB2's float conversions for the PICA (0x488da4): 1.7.16 and 1.7.23, exponent bias 63, too small = signed zero
uint32_t float24(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  const uint32_t sign = b >> 31;
  if ((b & 0x7FFFFFFF) == 0) return sign << 23;
  const int32_t exponent = static_cast<int32_t>(b << 1 >> 24) - 0x40;
  if (exponent < 0) return sign << 23;
  return (b << 9 >> 16) | static_cast<uint32_t>(exponent) << 16 | sign << 23;
}
uint32_t float31(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  const uint32_t sign = b >> 31;
  if ((b & 0x7FFFFFFF) == 0) return sign << 30;
  const int32_t exponent = static_cast<int32_t>(b << 1 >> 24) - 0x40;
  if (exponent < 0) return sign << 30;
  return (b << 9 >> 9) | static_cast<uint32_t>(exponent) << 23 | sign << 30;
}

} // namespace

// An operation's program: register writes and vertex uniform loads, in order.
//   reg | mask << 16, value              a register (reg < 0x300)
//   0x80000000 | geometry << 15 | first << 8 | count, 4 × count words   float uniforms, w first
struct GfxNatives::Program {
  std::vector<uint32_t> words;
  void write(uint32_t reg, uint32_t value, uint32_t mask = 0xF) {
    words.push_back(reg | mask << 16);
    words.push_back(value);
  }
  void uniforms(uint32_t first, const std::vector<uint32_t>& data, bool geometry = false) {
    if (data.empty()) return;
    words.push_back(0x80000000u | (geometry ? 1u : 0u) << 15 | first << 8 | static_cast<uint32_t>(data.size() / 4));
    words.insert(words.end(), data.begin(), data.end());
  }
};

GfxNatives::GfxNatives(Kernel& kernel, pn_engine* engine) : kernel_(kernel), engine_(engine) {
  pn_set_native_operations(engine_, &GfxNatives::run, this);
  // where nngx keeps the next command's place: the launcher read it in nngxAdd3DCommand's code; not guessed from
  // a native's own code (a function matching only in part read it at a wrong place: Mario Kart 7 broke)
  if (const char* cursor = std::getenv("CTRN_COMMAND_CURSOR"))
    lyt_.cursor = static_cast<uint32_t>(std::strtoul(cursor, nullptr, 16));
}

void GfxNatives::run(void* self, uint32_t operation) {
  auto& natives = *static_cast<GfxNatives*>(self);
  std::lock_guard lock(natives.mutex_);
  const bool outer = natives.run_depth_++ == 0;
  struct Depth {
    int& d;
    ~Depth() { --d; }
  } depth{natives.run_depth_};
  double ignored = 0;
  Clocked clocked(outer ? natives.prof_run_ : ignored);
  ++natives.prof_runs_;
  if (operation == 0 || operation > natives.ops_.size() || !natives.ops_[operation - 1].live) {
    ++natives.unknown_marks_;
    return;
  }
  // no copy: nothing is freed while an operation runs (the lock is held, a nested run only runs)
  const std::vector<uint32_t>& w = natives.ops_[operation - 1].words;
  pn_run_program(natives.engine_, w.data(), static_cast<uint32_t>(w.size()));
  return;
  for (size_t i = 0; i + 1 < w.size();) {
    if (w[i] >> 31) {
      const uint32_t first = w[i] >> 8 & 0x7F, count = w[i] & 0xFF;
      if (w[i] >> 15 & 1) pn_geometry_uniforms(natives.engine_, first, &w[i + 1], count);
      else pn_vertex_uniforms(natives.engine_, first, &w[i + 1], count);
      i += 1 + count * 4;
    } else {
      pn_write_register(natives.engine_, w[i] & 0xFFFF, w[i + 1], w[i] >> 16 & 0xF);
      i += 2;
    }
  }
}

// the operations a program's marks name (a block translated with marks in it)
template <typename F> static void each_nested(const std::vector<uint32_t>& w, F f) {
  for (size_t i = 0; i + 1 < w.size();) {
    if ((w[i] >> 30) == 3) { // one register, many values
      const uint32_t count = w[i + 1];
      if ((w[i] & 0xFFFF) == 0x2F0)
        for (uint32_t k = 0; k < count && i + 2 + k < w.size(); ++k) f(w[i + 2 + k]);
      i += 2 + count;
    } else if (w[i] >> 31) {
      i += 1 + (w[i] & 0xFF) * 4;
    } else {
      if ((w[i] & 0xFFFF) == 0x2F0) f(w[i + 1]);
      i += 2;
    }
  }
}

uint32_t GfxNatives::operation(const Program& program) {
  Clocked clocked(prof_lookup_);
  // one done before (most of a menu, every frame): its operation again
  if (const auto found = known_.find(program.words); found != known_.end()) return found->second;
  uint32_t id;
  if (!free_.empty()) id = free_.back(), free_.pop_back();
  else ops_.emplace_back(), id = static_cast<uint32_t>(ops_.size());
  Op& op = ops_[id - 1];
  op.words = program.words;
  op.refs = 0;
  op.frame = frame_; // a new one lives at least through this frame
  op.live = true;
  each_nested(op.words, [&](uint32_t inner) {
    if (inner && inner <= ops_.size() && ops_[inner - 1].live) ++ops_[inner - 1].refs;
  });
  known_.emplace(op.words, id);
  ++live_;
  return id;
}

uint64_t GfxNatives::holder() const {
  // what keeps an operation marked now: the list being recorded (until the game clears it), else this frame's lists
  return recording_ ? (uint64_t{2} << 62 | recording_) : (uint64_t{1} << 62 | frame_);
}

void GfxNatives::hold(uint64_t who, uint32_t id) {
  if (!id || id > ops_.size() || !ops_[id - 1].live) return;
  if (who >> 62 == 1) { // a frame's lists: a stamp, no set (most marks are these)
    ops_[id - 1].frame = frame_;
    return;
  }
  if (holders_[who].insert(id).second) ++ops_[id - 1].refs;
}

void GfxNatives::release(uint64_t who) {
  const auto found = holders_.find(who);
  if (found == holders_.end()) return;
  const std::unordered_set<uint32_t> ids = std::move(found->second);
  holders_.erase(found);
  for (uint32_t id : ids) unref(id);
}

void GfxNatives::unref(uint32_t id) {
  if (!id || id > ops_.size() || !ops_[id - 1].live) return;
  if (ops_[id - 1].refs) --ops_[id - 1].refs;
  if (ops_[id - 1].refs || ops_[id - 1].frame + 8 > frame_) return; // still held, or marked in a recent frame
  Op& op = ops_[id - 1];
  op.live = false;
  ++op.generation;
  known_.erase(op.words);
  std::vector<uint32_t> words = std::move(op.words);
  op.words.clear();
  free_.push_back(id);
  --live_;
  each_nested(words, [&](uint32_t inner) { unref(inner); });
}

void GfxNatives::frame() {
  std::lock_guard lock(mutex_);
  // a frame's lists are done with a few frames on (GSP runs them in order, the window shows the last ones)
  ++frame_;
  // what only frames held, and none of the last 8: freed (a scan of the operations, a few thousand)
  for (uint32_t id = 1; id <= ops_.size(); ++id) {
    const Op& op = ops_[id - 1];
    if (op.live && op.refs == 0 && op.frame + 8 <= frame_) {
      ++ops_[id - 1].refs; // unref takes it back to 0 and frees it
      unref(id);
    }
  }
}

uint32_t GfxNatives::mark_at(uint32_t where, uint32_t operation, uint64_t who) {
  Memory& m = kernel_.memory();
  if (who == kAtItsPlace) { // kept while the mark is there: a buffer the game builds once and runs many times
    who = uint64_t{4} << 62 | where;
    hold(who | uint64_t{1} << 61, operation); // the new one first: the same operation there again stays
    release(who);
    holders_[who] = std::move(holders_[who | uint64_t{1} << 61]);
    holders_.erase(who | uint64_t{1} << 61);
  } else {
    hold(who ? who : holder(), operation);
  }
  m.write32(where, operation);
  m.write32(where + 4, 0x000F0000u | 0x2F0u); // the mark: one register, all bytes
  return where + 8;
}

void GfxNatives::emit(Program& program) {
  if (program.words.empty()) return;
  Memory& m = kernel_.memory();
  const uint32_t cursor = m.read32(lyt_.cursor);
  m.write32(lyt_.cursor, mark_at(cursor, operation(program)));
  program.words.clear();
}

bool GfxNatives::commands(Program& program, uint32_t address, uint32_t bytes) {
  // a block of commands the game keeps (constant data, a template it fills in, a list it built once): what it sets;
  // false for one that runs other lists (a jump), which cannot become an operation
  const Memory& m = kernel_.memory();
  for (uint32_t i = 0; i + 2 <= bytes / 4;) {
    const uint32_t first = m.read32(address + i * 4), header = m.read32(address + i * 4 + 4);
    const uint32_t reg = header & 0xFFFF, mask = header >> 16 & 0xF, extra = header >> 20 & 0xFF;
    const bool consecutive = header >> 31;
    if (reg >= 0x300) break; // the list is over
    if (i + 2 + extra > bytes / 4) return false; // a command running past the block: not a block of commands
    if (reg == 0x2F1) { // a padding of ours, where a block was: nothing
      i += 2 + extra;
      i += i & 1;
      continue;
    }
    if (reg + (consecutive ? extra : 0) >= 0x238 && reg <= 0x23D) return false;
    if (consecutive || extra == 0) {
      for (uint32_t k = 0; k <= extra; ++k)
        program.write(consecutive ? reg + k : reg, k == 0 ? first : m.read32(address + (i + 1 + k) * 4), mask);
    } else { // one register, many values: data of uniforms, program code, a table, in one step
      program.words.push_back(0xC0000000u | mask << 16 | reg);
      program.words.push_back(extra + 1);
      program.words.push_back(first);
      for (uint32_t k = 1; k <= extra; ++k) program.words.push_back(m.read32(address + (i + 1 + k) * 4));
    }
    i += 2 + extra;
    i += i & 1;
  }
  return true;
}

void GfxNatives::learn_lyt(uint32_t flush) {
  if (lyt_.flush) return;
  const Memory& m = kernel_.memory();
  lyt_.flush = flush;
  if (!lyt_.cursor) lyt_.cursor = literal(m, flush + 0x18);          // ldr r6, =cursor
  lyt_.draw_block = literal(m, branch_target(m, flush + 0x164));     // bl → ldr r0, =block
}

// NSMB2's nw::lyt drawer: docs/NSMB2-LYT.md (§8) and NSMB2-LYT-NATIF.md
void GfxNatives::lyt_flush(Program& program, uint32_t drawer) {
  Memory& m = kernel_.memory();
  uint8_t counts[3]; // rectangles, matrix and color rows, rectangle and coordinate rows
  m.read(drawer + 0x24, counts, 3);
  auto rows = [&](uint32_t block, uint32_t pointer, uint32_t count) {
    std::vector<uint32_t> words(count * 4);
    const uint32_t at = m.read32(drawer + pointer);
    for (uint32_t i = 0; i < count * 4; ++i) words[i] = m.read32(at + i * 4);
    program.uniforms(m.read32(drawer + block) & 0x7F, words); // the block starts with its 0x2C0 index
  };
  rows(0x88, 0x668, counts[0]);
  rows(0x238, 0x66C, counts[1]);
  rows(0x448, 0x670, counts[2]);
  const uint32_t vertices = counts[0] * 6u;
  const uint32_t from = vertices > 0x10 ? vertices * 2 - 0x20 : 0; // 0x4944a8: indices never across 4 KiB
  program.write(0x2B1, m.read32(drawer + 0x69C));                // i0: the coordinate rows per rectangle
  program.write(0x227, 0x80000000u | (((m.read32(drawer + 0x680) + from) & 0xFFF) >= 0xFE0 ? 0x20u : 0u));
  program.write(0x228, vertices);
  commands(program, lyt_.draw_block, 0x40);
  const uint8_t empty[3] = {};
  m.write(drawer + 0x24, empty, 3);
}

void GfxNatives::lyt_flush_pending(Program& program, uint32_t drawer) {
  if (read8(kernel_.memory(), drawer + 0x24)) lyt_flush(program, drawer);
}

bool GfxNatives::lyt_material(Program& program, uint32_t function, uint32_t drawer, uint32_t material) {
  // SetupGraphics 0x191c4c (docs/NSMB2-LYT.md §6)
  Memory& m = kernel_.memory();
  learn_lyt(branch_target(m, function + 0x78));
  const uint32_t flags2c = m.read32(material + 0x2C), flags30 = m.read32(material + 0x30);
  uint8_t mode = read8(m, drawer + 0x6A2);
  if (read8(m, material + 0x4D) & 2) { // nothing to set
    if (mode != 0 && mode != 1) lyt_flush_pending(program, drawer);
    return true;
  }
  // not decoded yet: the game's own function
  if ((flags2c & 0x400) || (flags30 & 0x1C0) || (flags30 & 3) == 3) return false;

  bool barrier = false;
  auto set_barrier = [&] {
    barrier = true;
    program.write(0x111, 1, 0); // [1][0x00000111]: no byte written
  };
  // alpha test
  if (flags2c & 0x200) {
    lyt_flush_pending(program, drawer);
    write8(m, drawer + 0x6A1, 0);
    set_barrier();
    const uint32_t at = m.read32(material + 0x34) + (flags2c & 3) * 32 + (flags2c >> 2 & 3) * 20 + (flags2c >> 4 & 3) * 4;
    const uint32_t table = literal(m, function + 0xA4); // lyt comparison → PICA
    float ref;
    const uint32_t ref_bits = m.read32(at + 4);
    std::memcpy(&ref, &ref_bits, 4);
    const uint32_t value = static_cast<uint32_t>(ref * 255.0f + 0.5f) & 0xFF;
    program.write(0x104, m.read32(table + read8(m, at) * 4) << 4 | value << 8 | 1);
  } else if (read8(m, drawer + 0x6A1) == 0) {
    lyt_flush_pending(program, drawer);
    write8(m, drawer + 0x6A1, 1);
    set_barrier();
    program.write(0x104, 0x61);
  }
  // blending (the custom one is left to the game above)
  if (read8(m, drawer + 0x6A0) == 0) {
    lyt_flush_pending(program, drawer);
    write8(m, drawer + 0x6A0, 1);
    if (!barrier) set_barrier();
    commands(program, literal(m, function + 0x1D0), 0x10);
  }
  // TEV, by the number of textures
  const uint32_t black = m.read32(material + 0x10), white = m.read32(material + 0x14);
  const uint32_t t = literal(m, function + 0x224); // 0x5f4920: the templates
  switch (flags30 & 3) {
  case 0:
    if (mode != 0) {
      lyt_flush_pending(program, drawer);
      m.write32(t + 0x20, white);
      commands(program, t + 0x10, 0x18);
      write8(m, drawer + 0x6A2, 0);
    } else if (m.read32(t + 0x20) != white) {
      lyt_flush_pending(program, drawer);
      m.write32(t + 0x20, white);
      program.write(0xFB, white);
    }
    break;
  case 1:
    if (mode != 1) {
      lyt_flush_pending(program, drawer);
      m.write32(t + 0x38, black);
      m.write32(t + 0x50, white);
      commands(program, t + 0x28, 0x48);
      write8(m, drawer + 0x6A2, 1);
    } else if (m.read32(t + 0x38) != black || m.read32(t + 0x50) != white) {
      lyt_flush_pending(program, drawer);
      m.write32(t + 0x38, black);
      m.write32(t + 0x50, white);
      program.write(0xDB, black);
      program.write(0xF3, white);
    }
    break;
  case 2: { // 0x192934
    lyt_flush_pending(program, drawer);
    const uint32_t mix = m.read32(material + 0x28);
    if (mode != 2) {
      m.write32(t + 0xE0, mix);
      m.write32(t + 0xF8, white);
      m.write32(t + 0x100, black);
      commands(program, t + 0xD0, 0x58);
    } else if (m.read32(t + 0xE0) != mix || m.read32(t + 0xF8) != white || m.read32(t + 0x100) != black) {
      m.write32(t + 0xE0, mix);
      m.write32(t + 0xF8, white);
      m.write32(t + 0x100, black);
      program.write(0xDB, mix);
      program.write(0xF3, white);
      program.write(0xFD, black);
    }
    write8(m, drawer + 0x6A2, 2);
    break;
  }
  }
  return true;
}

bool GfxNatives::lyt_textures(Program& program, uint32_t function, uint32_t drawer, uint32_t material, bool flag) {
  // 0x1921d8 (docs/NSMB2-LYT.md §7)
  Memory& m = kernel_.memory();
  const uint32_t t = literal(m, function + 0x08); // 0x5f4990: the units' commands, 32 bytes each
  if (material == 0) {
    m.write32(t + 0x08, 0), m.write32(t + 0x28, 0), m.write32(t + 0x48, 0);
    if (flag) { // 0x1aa150: the units off
      const uint32_t off = branch_target(m, function + 0x1A0);
      if (!lyt_.cursor) lyt_.cursor = literal(m, off);
      program.write(0x6F, 0, 0x2);
      commands(program, literal(m, off + 0x24), 0x10); // a write to 0x080 with no byte
      program.write(0x80, 0x1000, 0xB);
    }
    return true;
  }
  learn_lyt(branch_target(m, function + 0xCC));
  const bool none_pending = read8(m, drawer + 0x24) == 0;
  const uint32_t n = std::min<uint32_t>(m.read32(material + 0x30) & 3, 3);
  const uint32_t entries = m.read32(material + 0x34);
  bool changed = false;
  uint32_t mask = 0;
  for (uint32_t i = 0; i < n; ++i) {
    mask |= 1u << i;
    const uint32_t e = entries + i * 32, u = t + i * 32;
    const uint32_t address = m.read32(e + 4) >> 3, param = m.read32(e + 0x14);
    if (m.read32(u + 8) != address || m.read32(u + 0x18) != param) {
      changed = true;
      m.write32(u + 0x00, m.read32(e + 0x18));
      m.write32(u + 0x08, address);
      m.write32(u + 0x10, m.read32(e + 0x1C));
      m.write32(u + 0x18, param);
    }
  }
  if (none_pending || changed) {
    lyt_flush_pending(program, drawer);
    commands(program, literal(m, function + 0xD0) + 0x10, 0x10); // a write to 0x080 with no byte
  }
  if (none_pending) {
    program.write(0x6F, mask << 8, 0x2);
    program.write(0x80, mask | 0x11000, 0xB);
  }
  if (none_pending || changed) program.write(0x80, mask | 0x11000, 0x4); // the texture cache cleared
  if (changed) commands(program, t, 32 * n);
  return true;
}

bool GfxNatives::lyt_block(Program& program, uint32_t block) {
  // a block {address, size, ?, copied} the drawer hands nngxAdd3DCommand (0x5407cc): copied, its commands
  Memory& m = kernel_.memory();
  if (!read8(m, block + 0xC)) return false; // run where it is (a request of its own): left to the game
  return commands(program, m.read32(block), m.read32(block + 4));
}

bool GfxNatives::lyt_begin(Program& program, uint32_t function, uint32_t drawer, uint32_t flag) {
  // DrawBegin 0x1aaf30 (docs/NSMB2-LYT.md §9)
  Memory& m = kernel_.memory();
  if (!lyt_.cursor) lyt_.cursor = literal(m, function + 0x60);
  Program block;
  if (!(flag & 1) && !lyt_block(block, drawer + 4)) return false;
  lyt_.draw_begin = function;
  m.write32(drawer + 0x698, 0), m.write32(drawer + 0x69C, 0), m.write32(drawer + 0x694, 0);
  write8(m, drawer + 0x6A2, 6), write8(m, drawer + 0x6A0, 1);
  const uint32_t textures = literal(m, branch_target(m, function + 0x38) + 0x08); // 0x1921d8(drawer, 0, 0)
  m.write32(textures + 0x08, 0), m.write32(textures + 0x28, 0), m.write32(textures + 0x48, 0);
  if (!(flag & 1)) {
    program.write(0x6F, 0, 0x2);
    commands(program, literal(m, function + 0x80), 0x10); // a write to 0x080 with no byte
    program.write(0x80, 0x1000, 0xB);
    program.words.insert(program.words.end(), block.words.begin(), block.words.end());
  }
  m.write32(drawer + 0x684, 0);
  return true;
}

uint32_t GfxNatives::translated(uint32_t address, uint32_t size) {
  Clocked clocked(prof_translated_);
  prof_hashed_ += size;
  // a block of commands kept in memory (a model's, a recorded list): translated once, again only if it changed
  Memory& m = kernel_.memory();
  if (size < 8 || !m.host(address) || !m.host(address + size - 1)) return 0;
  const uint8_t* bytes = m.fastmem() + address; // the game's own view: its addresses, in order
  auto& known = translated_[{address, size}];
  const bool alive = known.id && known.id <= ops_.size() && ops_[known.id - 1].live &&
                     ops_[known.id - 1].generation == known.generation;
  bool same;
  {
    Clocked compare(prof_compare_);
    same = alive && known.bytes.size() == size && std::memcmp(known.bytes.data(), bytes, size) == 0;
  }
  if (!same) {
    Clocked again(prof_retranslate_);
    Program block;
    if (!commands(block, address, size) || block.words.empty()) return 0;
    known.bytes.assign(bytes, bytes + size);
    known.id = operation(block);
    known.generation = ops_[known.id - 1].generation;
    translated_words_ += size / 4;
  }
  return known.id;
}

bool GfxNatives::list_copy(uint32_t destination, uint32_t source, uint32_t size) {
  // a block copied where the game's next command goes: its mark, then a padding picanative skips, as long as the
  // block (the caller moves on by its size)
  if (size < 8 || (size & 7)) {
    ++refused_copies_[size];
    return false;
  }
  const uint32_t operation = translated(source, size);
  if (!operation) {
    ++refused_copies_[size | 0x80000000u]; // not a block of its own (a jump in it, or not commands)
    return false;
  }
  Memory& m = kernel_.memory();
  uint32_t at = mark_at(destination, operation), left = size / 4 - 2;
  while (left) {
    const uint32_t extra = std::min<uint32_t>(left - 2, 254);
    m.write32(at, 0);
    m.write32(at + 4, 0x2F1u | extra << 20);
    at += (2 + extra) * 4;
    left -= 2 + extra;
  }
  copied_words_ += size / 4;
  return true;
}

bool GfxNatives::palette_rows(uint32_t where, uint32_t source, bool first, uint32_t header) {
  // a bone palette (0x169a98): one index command, one 0x2C1 command over many calls, 3 rows each (0x11d7d0 for
  // the first, with the header, 0x11d798 after). The game writes them as usual while the rows are gathered; the
  // last call writes the whole as a mark and a padding instead (a broken sequence leaves the game's words alone)
  Memory& m = kernel_.memory();
  if (first) {
    palette_ = {};
    if (!lyt_.cursor || where - 8 != m.read32(lyt_.cursor)) return false;
    const uint32_t index = m.read32(where - 8), reg = m.read32(where - 4) & 0xFFFF;
    const uint32_t extra = header >> 20 & 0xFF;
    if (!(index >> 31) || (reg != 0x2C0 && reg != 0x290) || (header & 0xFFFF) != reg + 1 || (extra + 1) % 12)
      return false;
    palette_ = {where - 8, where + 0x34, (extra + 1) / 4, index & 0x7F, reg == 0x290, {}};
  } else if (!palette_.start || where != palette_.next) {
    palette_ = {};
    return false;
  } else {
    palette_.next = where + 0x30;
  }
  for (uint32_t i = 0; i < 3; ++i)
    for (uint32_t k = 0; k < 4; ++k) palette_.words.push_back(m.read32(source + i * 16 + (3 - k) * 4));
  if (palette_.words.size() / 4 < palette_.rows) return false; // the game writes this one
  Program program;
  program.uniforms(palette_.first, palette_.words, palette_.geometry);
  const uint32_t footprint = (2 + 1 + palette_.rows * 4 + 1) & ~1u;
  uint32_t at = mark_at(palette_.start, operation(program)), left = footprint - 2;
  while (left) {
    const uint32_t extra = std::min<uint32_t>(left - 2, 254);
    m.write32(at, 0);
    m.write32(at + 4, 0x2F1u | extra << 20);
    at += (2 + extra) * 4;
    left -= 2 + extra;
  }
  palette_ = {};
  return true;
}

void GfxNatives::pad(uint32_t at, uint32_t words) {
  // what follows a mark up to the end of the place the game's words took: skipped unread (words even, >= 0)
  Memory& m = kernel_.memory();
  while (words) {
    const uint32_t extra = std::min<uint32_t>(words - 2, 254);
    m.write32(at, 0);
    m.write32(at + 4, 0x2F1u | extra << 20);
    at += (2 + extra) * 4;
    words -= 2 + extra;
  }
}

bool GfxNatives::written(uint32_t start, uint32_t end, uint64_t who) {
  // a game function's commands, just written at [start, end): as an operation, its mark and a padding in their place
  // (the game computed the values, its own code; the host sets them, picanative reads nothing there)
  if (end <= start || ((end - start) & 7) || end - start > 0x4000) return false;
  const uint32_t id = translated(start, end - start);
  if (!id) return false;
  pad(mark_at(start, id, who), (end - start) / 4 - 2);
  return true;
}

bool GfxNatives::uniform_rows(uint32_t where, uint32_t source, uint32_t rows, uint32_t header, bool vec3) {
  // nw::gfx's uniform writers (0x11d7d0 3 rows, 0x11d854 4 rows, 0x11d8a0 a vec3): the caller put the index command
  // at the cursor, they write the data after it. Taken when the whole is one command here: the rows from their
  // source, set directly; the index command and the data, a mark and a padding
  Memory& m = kernel_.memory();
  if (!lyt_.cursor || where - 8 != m.read32(lyt_.cursor)) return false;
  const uint32_t index = m.read32(where - 8), index_header = m.read32(where - 4);
  const uint32_t reg = index_header & 0xFFFF;
  if (!(index >> 31) || (reg != 0x2C0 && reg != 0x290)) return false;
  uint32_t footprint; // words, from the index command
  std::vector<uint32_t> w(rows * 4);
  if (vec3 || rows == 0) { // a vec3 (w = 0) or a vec4 (rows 0: 0x11d710)
    if (index_header != (0x804F0000u | reg)) return false; // the index, then the 4 data words in one command
    w = {vec3 ? 0 : m.read32(source + 12), m.read32(source + 8), m.read32(source + 4), m.read32(source)};
    footprint = 6;
  } else {
    if ((header & 0xFFFF) != reg + 1 || (header >> 20 & 0xFF) != rows * 4 - 1) return false;
    for (uint32_t i = 0; i < rows; ++i)
      for (uint32_t k = 0; k < 4; ++k) w[i * 4 + k] = m.read32(source + i * 16 + (3 - k) * 4);
    footprint = (2 + 1 + rows * 4 + 1) & ~1u;
  }
  Program program;
  program.uniforms(index & 0x7F, w, reg == 0x290);
  uint32_t at = mark_at(where - 8, operation(program)), left = footprint - 2;
  while (left) {
    const uint32_t extra = std::min<uint32_t>(left - 2, 254);
    m.write32(at, 0);
    m.write32(at + 4, 0x2F1u | extra << 20);
    at += (2 + extra) * 4;
    left -= 2 + extra;
  }
  return true;
}

bool GfxNatives::nngx_add(Program& program, uint32_t function, uint32_t address, uint32_t size, bool copy) {
  // nngxAdd3DCommand 0x5407e4: the block's commands become an operation. Copied, they are read now; not copied (a
  // request of its own: a model's commands, prepared once), translated once and kept while the block is unchanged
  Memory& m = kernel_.memory();
  // not copied: the SDK makes a request of its own (and keeps its books on it); skipping that broke Mario Kart 7 at
  // its start. Left to the game until those books are done natively too.
  if (!copy) return false;
  if (!copy) {
    const uint32_t cursor_at = literal(m, function + 0x10);
    if (!lyt_.cursor) lyt_.cursor = cursor_at;
    if (cursor_at != lyt_.cursor) return false;
    const uint32_t id = translated(address, size);
    if (!id) return false;
    uncopied_words_ += size / 4;
    program.words.clear();
    // its mark, as the copied ones (the request it would have been is not made)
    m.write32(lyt_.cursor, mark_at(m.read32(lyt_.cursor), id));
    return true;
  }
  const uint32_t cursor_at = literal(m, function + 0x10);
  if (!lyt_.cursor) lyt_.cursor = cursor_at;
  if (cursor_at != lyt_.cursor) return false;
  // the block's place in the list, as the game's copy takes it (it moves on by size & ~3): the mark and a padding
  // there, never less (a caller may count on where its block ends)
  const uint32_t cursor = m.read32(cursor_at), end = m.read32(cursor_at + 4), advance = size & ~3u;
  if (advance < 8 || (advance & 7) || cursor > end || advance > end - cursor) return false;
  const uint32_t id = translated(address, advance);
  if (!id) return false;
  // what it keeps: the current list's used size, before the block
  const uint32_t list = m.read32(literal(m, function + 0x0C) + 0x9C);
  m.write32(list + 0xC, cursor - m.read32(list + 4));
  pad(mark_at(cursor, id), advance / 4 - 2);
  m.write32(cursor_at, cursor + advance);
  program.words.clear();
  return true;
}

bool GfxNatives::call(Thread& thread, uint32_t which) {
  std::lock_guard lock(mutex_);
  which &= kNatives - 1; // the counters are arrays: no lookup per call
  Clocked clocked(prof_call_[which]);
  ++calls_[which];
  // CTRN_NO_NATIVES=3,17: those left to the game's own code (to find which one draws wrong)
  static const std::vector<uint32_t> off = [] {
    std::vector<uint32_t> v;
    for (const char* p = std::getenv("CTRN_NO_NATIVES"); p && *p;) {
      char* end;
      v.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 10)));
      p = *end == ',' ? end + 1 : end;
      if (end == p && *p) break;
    }
    return v;
  }();
  if (std::find(off.begin(), off.end(), which) != off.end()) return false;
  const uint32_t function = thread.ctx.r[15];
  const uint32_t* r = thread.ctx.r;
  Program program;
  bool done = false;
  switch (which) {
  case kNngxCopyPreparedList: // not native yet: the copy, then picanative reading it; counted
    break;
  case kLytFlushBatch:
    learn_lyt(function);
    lyt_flush(program, r[0]);
    done = true;
    break;
  case kLytMatrices: { // 0x1a98dc: the matrix and color rows alone
    if (!lyt_.flush) break; // the cursor is learnt from the flush, the material or the textures first
    Memory& m = kernel_.memory();
    const uint32_t d = r[0], count = read8(m, d + 0x25);
    std::vector<uint32_t> words(count * 4);
    const uint32_t at = m.read32(d + 0x66C);
    for (uint32_t i = 0; i < count * 4; ++i) words[i] = m.read32(at + i * 4);
    program.uniforms(m.read32(d + 0x238) & 0x7F, words);
    write8(m, d + 0x25, 0);
    done = true;
    break;
  }
  case kLytMaterial:
    done = lyt_material(program, function, r[0], r[1]);
    break;
  case kNngxAdd3DCommand:
    done = nngx_add(program, function, r[0], r[1], r[2] != 0);
    break;
  case kNngxAddBlock: { // 0x5407cc, then 0x5407e4 18 bytes on
    Memory& m = kernel_.memory();
    done = nngx_add(program, function + 0x18, m.read32(r[0]), m.read32(r[0] + 4), read8(m, r[0] + 0xC) != 0);
    break;
  }
  case kNngxSaveStart: // the current list is recorded from here: what is marked in it lives with it
    recording_ = kernel_.memory().read32(literal(kernel_.memory(), function) + 0x9C);
    break;
  case kNngxSaveStop:
    recording_ = 0;
    break;
  case kNngxClear: // the current list emptied: what its recording held goes
    release(uint64_t{2} << 62 | kernel_.memory().read32(literal(kernel_.memory(), function) + 0x9C));
    break;
  case kCopyUnaligned:
  case kCopyAligned:
  case kCopy:
    done = list_copy(r[0], r[1], r[2]);
    break;
  case kGfxRows3:
    done = uniform_rows(r[0], r[1], 3, r[2], false) || palette_rows(r[0], r[1], true, r[2]);
    break;
  case kGfxRows3Data:
    done = palette_rows(r[0], r[1], false, 0);
    break;
  case kGfxRows4:
    done = uniform_rows(r[0], r[1], 4, r[2], false);
    break;
  case kGfxVec3:
    done = uniform_rows(r[0], r[1], 1, 0, true);
    break;
  case kGfxVec4:
    done = uniform_rows(r[0], r[1], 0, 0, false);
    break;
  case kGfxCamera: { // 0x170c74(ctx, camera, force): view (camera+0x148, 3 rows) in c90, projection (+0x1a8) in c86
    Memory& m = kernel_.memory();
    if (!lyt_.cursor) lyt_.cursor = literal(m, function + 0x18);
    const uint32_t ctx = r[0], camera = r[1];
    if (m.read32(ctx + 0x28) == camera && r[2] == 0) { done = true; break; }
    m.write32(ctx + 0x28, camera);
    // the game sends its rows reversed (x last): as a command list carries them
    auto rows = [&](uint32_t at, uint32_t count) {
      std::vector<uint32_t> w(count * 4);
      for (uint32_t i = 0; i < count; ++i)
        for (uint32_t k = 0; k < 4; ++k) w[i * 4 + k] = m.read32(at + i * 16 + (3 - k) * 4);
      return w;
    };
    const auto view = rows(camera + 0x148, 3), projection = rows(camera + 0x1A8, 4);
    program.uniforms(90, view), program.uniforms(86, projection);
    program.uniforms(90, view, true), program.uniforms(86, projection, true);
    done = true;
    break;
  }
  case kGfxUniformBlock: { // 0x11eb1c(where, block): a shader unit's booleans and integers, 6 words, into the list
    Memory& m = kernel_.memory();
    if (!lyt_.cursor || r[0] != m.read32(lyt_.cursor)) break;
    done = list_copy(r[0], r[1], 24);
    if (done) thread.ctx.r[0] = r[0] + 24; // where it stopped: the caller's next command goes there
    break;
  }
  case kGameDraw: { // NSMB2 0x487e00(?, where, {indices, count, 8-bit}): an indexed draw, 18 words at `where`
    Memory& m = kernel_.memory();
    const uint32_t d = r[2], base = m.read32(literal(m, function + 0x14)); // the attribute buffers' base, / 8
    const uint32_t indices = m.read32(d) - base * 8;
    program.write(0x25F, 1);
    program.write(0x227, (read8(m, d + 8) ? 0u : 0x80000000u) | indices);
    program.write(0x228, m.read32(d + 4));
    program.write(0x245, 0, 0x1);
    program.write(0x22F, 1);
    program.write(0x245, 1, 0x1);
    program.write(0x231, 1);
    program.write(0x25E, 0, 0x8);
    program.write(0x25E, 0, 0x8);
    const uint32_t id = operation(program);
    program.words.clear();
    pad(mark_at(r[1], id, kAtItsPlace), 16);
    thread.ctx.r[0] = r[1] + 0x48;
    done = true;
    break;
  }
  case kGameVertexFormat: // after NSMB2 0x487ea0(layout, where): what it wrote, up to where it stopped (r3)
    done = written(r[1], r[3], kAtItsPlace);
    break;
  case kAroundFirst + 5: // after NSMB2 0x10e8f4(state, where): a render target's commands, up to where it stopped
    done = written(r[1], r[3], kAtItsPlace);
    break;
  case kAroundFirst + 6: { // 0x11efd8(where, block): 312 bytes copied (an unrolled copy), the end returned
    Memory& m = kernel_.memory();
    // inside the current list only, from the cursor to its end (nngx keeps the end in the word after the cursor)
    if (!lyt_.cursor) break;
    const uint32_t cursor = m.read32(lyt_.cursor), end = m.read32(lyt_.cursor + 4);
    if (r[0] < cursor || r[0] >= end || 312 > end - r[0]) break;
    done = list_copy(r[0], r[1], 312);
    if (done) thread.ctx.r[0] = r[0] + 312;
    break;
  }
  case kAroundFirst + 0: case kAroundFirst + 1: case kAroundFirst + 2: case kAroundFirst + 3:
  case kAroundFirst + 4: case kAroundFirst + 7:
  case kAroundFirst + 8: case kAroundFirst + 9: case kAroundFirst + 10:
    // what the game's function just wrote at the cursor, [r2, r3): an operation of the frame (or the recording)
    if (!lyt_.cursor) lyt_.cursor = 0; // the cursor's place is in the generated code; the words are where they are
    done = r[3] != r[2] && written(r[2], r[3], 0);
    if (r[3] != r[2] && !done) ++refused_copies_[(r[3] - r[2]) | 0x40000000u];
    break;
  case kViewport: { // 0x488da4(rect {x, y, w, h}, where): the viewport's commands, written at `where`
    if (!lyt_.cursor) break; // the cursor learnt first (the words go into the current list)
    Memory& m = kernel_.memory();
    const uint32_t x = m.read32(r[0]), y = m.read32(r[0] + 4), w = m.read32(r[0] + 8), h = m.read32(r[0] + 0xC);
    program.write(0x41, float24(static_cast<float>(w) * 0.5f), 0x7);
    program.write(0x42, float31(2.0f / static_cast<float>(w)) << 1);
    program.write(0x43, float24(static_cast<float>(h) * 0.5f), 0x7);
    program.write(0x44, float31(2.0f / static_cast<float>(h)) << 1);
    program.write(0x68, x | y << 16);
    thread.ctx.r[0] = mark_at(r[1], operation(program), kAtItsPlace);
    program.words.clear();
    done = true;
    break;
  }
  case kScissor: { // 0x4883d0(scissor {on, x, y, w, h, buffer w, buffer h}, where)
    if (!lyt_.cursor) break;
    Memory& m = kernel_.memory();
    const uint32_t s = r[0];
    const int32_t x = static_cast<int32_t>(m.read32(s + 4)), y = static_cast<int32_t>(m.read32(s + 8));
    const int32_t right = x + static_cast<int32_t>(m.read32(s + 0xC)) - 1;
    const int32_t top = y + static_cast<int32_t>(m.read32(s + 0x10)) - 1;
    const int32_t bw = static_cast<int32_t>(m.read32(s + 0x14)), bh = static_cast<int32_t>(m.read32(s + 0x18));
    const uint32_t x0 = x < 0 ? 0 : static_cast<uint32_t>(bw <= x ? bw - 1 : x);
    const uint32_t y0 = y < 0 ? 0 : static_cast<uint32_t>(bh <= y ? bh - 1 : y);
    program.write(0x65, read8(m, s) ? 3 : 0);
    program.write(0x66, y0 << 16 | x0);
    program.write(0x67, right < 0 ? 0 : static_cast<uint32_t>(right) | static_cast<uint32_t>(top < 0 ? 0 : top) << 16);
    thread.ctx.r[0] = mark_at(r[1], operation(program), kAtItsPlace);
    program.words.clear();
    done = true;
    break;
  }
  case kLytDrawBegin:
    done = lyt_begin(program, function, r[0], r[1]);
    break;
  case kLytDrawEnd: { // 0x1aaef4: the pending batch, then (flag bit 0 clear) the drawer's ending block
    Memory& m = kernel_.memory();
    learn_lyt(branch_target(m, function + 0x14));
    Program block;
    if (!(r[1] & 1) && !lyt_block(block, r[0] + 0x14)) break;
    lyt_flush_pending(program, r[0]);
    program.words.insert(program.words.end(), block.words.begin(), block.words.end());
    done = true;
    break;
  }
  case kLytBeginProjected: { // 0x1aa924: DrawBegin(0) (virtual), then the projection and parallax in c0-c4
    Memory& m = kernel_.memory();
    const uint32_t d = r[0], source = r[1] + 4;
    if (!lyt_.draw_begin || m.read32(m.read32(d) + 0xC) != lyt_.draw_begin) break;
    if (!lyt_begin(program, lyt_.draw_begin, d, 0)) break;
    // 0x1aa954: the matrix kept in the drawer's block (D+0x30 m0, D+0x34 the 0x2C1 header, m1-m15 after)
    std::vector<uint32_t> c(20);
    for (uint32_t i = 0; i < 16; ++i) c[i] = m.read32(source + i * 4);
    for (uint32_t i = 0; i < 16; ++i) m.write32(d + 0x34 + i * 4, c[i]);
    m.write32(d + 0x30, c[0]);
    m.write32(d + 0x34, 0x013F02C1);
    m.write32(d + 0x84, 0);
    for (uint32_t i = 0; i < 4; ++i) c[16 + i] = m.read32(d + 0x74 + i * 4); // parallax
    program.uniforms(m.read32(d + 0x28) & 0x7F, c);
    done = true;
    break;
  }
  case kFontMatrix: { // 0x1aae20: 16 floats from c35 ([0x80000023][0x2C0] in its code)
    Memory& m = kernel_.memory();
    if (!lyt_.cursor) lyt_.cursor = literal(m, function);
    std::vector<uint32_t> words(16);
    for (uint32_t i = 0; i < 16; ++i) words[i] = m.read32(r[0] + i * 4);
    program.uniforms(35, words);
    done = true;
    break;
  }
  case kFontColors: // 0x1aae8c: the gradient's two colors, to the TEV buffer or stage 3, and stage 4
    if (!lyt_.cursor) lyt_.cursor = literal(kernel_.memory(), function);
    if (r[2]) program.write(0xFD, r[1]), program.write(0xF3, r[0]);
    else program.write(0xDB, r[0]), program.write(0xF3, r[1]);
    done = true;
    break;
  case kFontBuilt: { // after 0x1aa1a4: the list it built, as an operation; its mark in its place
    Memory& m = kernel_.memory();
    const uint32_t buffer = m.read32(r[1] + 0x44);
    if (read8(m, buffer + 9) & 6) break; // replayed by another path (transformed, chained): left as it is
    const uint32_t base = m.read32(buffer + 0x10), words = m.read32(buffer + 0x18);
    if (words < 2 || !commands(program, base, words * 4) || program.words.empty()) break;
    // the text's own list holds it, until the text is built again (the same text again keeps its operation)
    const uint64_t text = uint64_t{3} << 62 | buffer, next = text | uint64_t{1} << 61;
    mark_at(base, operation(program), next);
    release(text);
    holders_[text] = std::move(holders_[next]);
    holders_.erase(next);
    m.write32(buffer + 0x18, 2);
    program.words.clear();
    done = true;
    break;
  }
  case kLytTextures:
    done = lyt_textures(program, function, r[0], r[1], r[2] != 0);
    break;
  default:
    break;
  }
  if (done) {
    emit(program);
    ++done_[which];
  }
  return done;
}

void GfxNatives::report() const {
  if (kProfile) {
    double calls = 0;
    for (double t : prof_call_) calls += t;
    std::printf("[gfx] profile: host side %.3f s (of it: block lookups %.3f s, translated blocks %.3f s, %.1f MB hashed),"
                " run side %.3f s for %llu runs; %.1f MB translated, %zu blocks known; compare %.3f s, retranslate %.3f s\n",
                calls, prof_lookup_,
                prof_translated_, prof_hashed_ / 1e6, prof_run_, static_cast<unsigned long long>(prof_runs_),
                static_cast<double>(translated_words_) * 4 / 1e6, translated_.size(), prof_compare_, prof_retranslate_);
    std::vector<std::pair<double, uint32_t>> rows;
    for (uint32_t which = 0; which < kNatives; ++which) rows.emplace_back(prof_call_[which], which);
    std::sort(rows.rbegin(), rows.rend());
    for (const auto& [t, which] : rows)
      if (t > 0.01) std::printf("[gfx] profile:   %6.3f s  %s\n", t, name(which));
  }
  for (uint32_t which = 0; which < kNatives; ++which)
    if (calls_[which])
      std::printf("[gfx] %s: %llu calls, %llu done natively\n", name(which),
                  static_cast<unsigned long long>(calls_[which]), static_cast<unsigned long long>(done_[which]));
  for (const auto& [size, n] : refused_copies_)
    std::printf("[gfx] %s left to the game: %u bytes%s, %llu times\n", size >> 30 == 1 ? "words written around" : "copy into the list",
                size & 0x3FFFFFFF, size >> 31 ? " (not translatable)" : "", static_cast<unsigned long long>(n));
  if (copied_words_)
    std::printf("[gfx] blocks copied into lists: %llu words, as marks\n", static_cast<unsigned long long>(copied_words_));
  if (uncopied_words_)
    std::printf("[gfx] blocks run where they are: %llu words, %llu of them translated\n",
                static_cast<unsigned long long>(uncopied_words_), static_cast<unsigned long long>(translated_words_));
  if (!ops_.empty() || unknown_marks_)
    std::printf("[gfx] native operations: %zu numbers used, %zu alive at the end; %llu marks naming none\n",
                ops_.size(), live_, static_cast<unsigned long long>(unknown_marks_));
}

} // namespace host
