#include "gfx_native.hpp"

#include <cstdio>
#include <cstring>

namespace host {

namespace {

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
//   0x80000000 | first << 8 | count, 4 × count words   vertex float uniforms, w first
struct GfxNatives::Program {
  std::vector<uint32_t> words;
  void write(uint32_t reg, uint32_t value, uint32_t mask = 0xF) {
    words.push_back(reg | mask << 16);
    words.push_back(value);
  }
  void uniforms(uint32_t first, const std::vector<uint32_t>& data) {
    if (data.empty()) return;
    words.push_back(0x80000000u | first << 8 | static_cast<uint32_t>(data.size() / 4));
    words.insert(words.end(), data.begin(), data.end());
  }
};

GfxNatives::GfxNatives(Kernel& kernel, pn_engine* engine) : kernel_(kernel), engine_(engine) {
  pn_set_native_operations(engine_, &GfxNatives::run, this);
}

void GfxNatives::run(void* self, uint32_t operation) {
  auto& natives = *static_cast<GfxNatives*>(self);
  if (operation == 0 || operation > natives.operations_.size()) {
    ++natives.unknown_marks_;
    return;
  }
  const std::vector<uint32_t>& w = natives.operations_[operation - 1];
  for (size_t i = 0; i + 1 < w.size();) {
    if (w[i] >> 31) {
      const uint32_t first = w[i] >> 8 & 0x7F, count = w[i] & 0xFF;
      pn_vertex_uniforms(natives.engine_, first, &w[i + 1], count);
      i += 1 + count * 4;
    } else {
      pn_write_register(natives.engine_, w[i] & 0xFFFF, w[i + 1], w[i] >> 16 & 0xF);
      i += 2;
    }
  }
}

uint32_t GfxNatives::operation(const Program& program) {
  // one done before (most of a menu, every frame): its operation again
  auto [it, added] = known_.emplace(program.words, 0);
  if (added) {
    operations_.push_back(program.words);
    it->second = static_cast<uint32_t>(operations_.size());
  }
  return it->second;
}

uint32_t GfxNatives::mark_at(uint32_t where, uint32_t operation) {
  Memory& m = kernel_.memory();
  m.write32(where, operation);
  m.write32(where + 4, 0x000F0000u | 0x2F0u);
  return where + 8;
}

void GfxNatives::emit(Program& program) {
  if (program.words.empty()) return;
  Memory& m = kernel_.memory();
  const uint32_t cursor = m.read32(lyt_.cursor);
  m.write32(cursor, operation(program));
  m.write32(cursor + 4, 0x000F0000u | 0x2F0u); // the mark: one register, all bytes
  m.write32(lyt_.cursor, cursor + 8);
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
    if (reg == 0x2F1) { // a padding of ours, where a block was: nothing
      i += 2 + extra;
      i += i & 1;
      continue;
    }
    if (reg + (consecutive ? extra : 0) >= 0x238 && reg <= 0x23D) return false;
    for (uint32_t k = 0; k <= extra; ++k)
      program.write(consecutive ? reg + k : reg, k == 0 ? first : m.read32(address + (i + 1 + k) * 4), mask);
    i += 2 + extra;
    i += i & 1;
  }
  return true;
}

void GfxNatives::learn_lyt(uint32_t flush) {
  if (lyt_.flush) return;
  const Memory& m = kernel_.memory();
  lyt_.flush = flush;
  lyt_.cursor = literal(m, flush + 0x18);                            // ldr r6, =cursor
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
  // a block of commands kept in memory (a model's, a recorded list): translated once, again only if it changed
  Memory& m = kernel_.memory();
  if (size < 8 || !m.host(address) || !m.host(address + size - 1)) return 0;
  const uint8_t* bytes = m.fastmem() + address; // the game's own view: its addresses, in order
  uint64_t hash = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i + 8 <= size; i += 8) {
    uint64_t w;
    std::memcpy(&w, bytes + i, 8);
    hash = (hash ^ w) * 0x100000001B3ull;
  }
  auto& known = translated_[{address, size}];
  if (known.first != hash || known.second == 0) {
    Program block;
    if (!commands(block, address, size) || block.words.empty()) return 0;
    known = {hash, operation(block)};
    translated_words_ += size / 4;
  }
  return known.second;
}

bool GfxNatives::list_copy(uint32_t destination, uint32_t source, uint32_t size) {
  // a block copied where the game's next command goes: its mark, then a padding picanative skips, as long as the
  // block (the caller moves on by its size)
  const uint32_t operation = translated(source, size);
  if (!operation) return false;
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

bool GfxNatives::nngx_add(Program& program, uint32_t function, uint32_t address, uint32_t size, bool copy) {
  // nngxAdd3DCommand 0x5407e4: the block's commands become an operation. Copied, they are read now; not copied (a
  // request of its own: a model's commands, prepared once), translated once and kept while the block is unchanged
  Memory& m = kernel_.memory();
  if (!copy) {
    const uint32_t cursor_at = literal(m, function + 0x10);
    if (!lyt_.cursor) lyt_.cursor = cursor_at;
    if (cursor_at != lyt_.cursor) return false;
    uint64_t hash = 0xCBF29CE484222325ull;
    if (size < 8 || !m.host(address) || !m.host(address + size - 1)) return false;
    const uint8_t* bytes = m.fastmem() + address; // the game's own view: its addresses, in order
    for (uint32_t i = 0; i + 8 <= size; i += 8) {
      uint64_t w;
      std::memcpy(&w, bytes + i, 8);
      hash = (hash ^ w) * 0x100000001B3ull;
    }
    auto& known = translated_[{address, size}];
    if (known.first != hash || known.second == 0) {
      Program block;
      if (!commands(block, address, size)) return false;
      known = {hash, operation(block)};
      translated_words_ += size / 4;
    }
    uncopied_words_ += size / 4;
    program.words.clear();
    // its mark, as the copied ones (the request it would have been is not made)
    const uint32_t cursor = m.read32(lyt_.cursor);
    m.write32(cursor, known.second);
    m.write32(cursor + 4, 0x000F0000u | 0x2F0u);
    m.write32(lyt_.cursor, cursor + 8);
    return true;
  }
  const uint32_t cursor_at = literal(m, function + 0x10);
  if (!lyt_.cursor) lyt_.cursor = cursor_at;
  if (cursor_at != lyt_.cursor || !commands(program, address, size)) return false;
  // what it keeps: the current list's used size, before the block
  const uint32_t list = m.read32(literal(m, function + 0x0C) + 0x9C);
  m.write32(list + 0xC, m.read32(cursor_at) - m.read32(list + 4));
  return true;
}

bool GfxNatives::call(Thread& thread, uint32_t which) {
  ++calls_[which];
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
  case kCopyUnaligned:
  case kCopyAligned:
  case kCopy:
    done = list_copy(r[0], r[1], r[2]);
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
    thread.ctx.r[0] = mark_at(r[1], operation(program));
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
    thread.ctx.r[0] = mark_at(r[1], operation(program));
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
    m.write32(base, operation(program));
    m.write32(base + 4, 0x000F0000u | 0x2F0u);
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
  for (const auto& [which, n] : calls_) {
    const auto done = done_.find(which);
    std::printf("[gfx] %s: %llu calls, %llu done natively\n", name(which), static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(done == done_.end() ? 0 : done->second));
  }
  if (copied_words_)
    std::printf("[gfx] blocks copied into lists: %llu words, as marks\n", static_cast<unsigned long long>(copied_words_));
  if (uncopied_words_)
    std::printf("[gfx] blocks run where they are: %llu words, %llu of them translated\n",
                static_cast<unsigned long long>(uncopied_words_), static_cast<unsigned long long>(translated_words_));
  if (!operations_.empty() || unknown_marks_)
    std::printf("[gfx] %zu native operations, %llu marks naming none\n", operations_.size(),
                static_cast<unsigned long long>(unknown_marks_));
}

} // namespace host
