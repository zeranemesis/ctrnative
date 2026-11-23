#include "gfx_native.hpp"

#include <cstdio>

namespace host {

namespace {

const char* name(uint32_t which) {
  switch (which) {
  case GfxNatives::kNngxCopyPreparedList: return "nngx: a prepared list copied into the current one";
  case GfxNatives::kLytFlushBatch: return "nw::lyt: a batch of rectangles drawn";
  default: return "?";
  }
}

// the target of the `bl` at `at`
uint32_t branch_target(const Memory& m, uint32_t at) {
  const uint32_t word = m.read32(at);
  return at + 8 + (static_cast<uint32_t>(static_cast<int32_t>(word << 8) >> 6));
}

// the constant an `ldr rN, [pc, #i]` at `at` loads
uint32_t literal(const Memory& m, uint32_t at) { return m.read32(at + 8 + (m.read32(at) & 0xFFF)); }

} // namespace

GfxNatives::GfxNatives(Kernel& kernel, pn_engine* engine) : kernel_(kernel), engine_(engine) {
  pn_set_native_operations(engine_, &GfxNatives::run, this);
}

uint32_t GfxNatives::add(Operation operation) {
  // shortcut: operations are kept for good; a long game makes many, free those no mark names any more
  operations_.push_back(std::move(operation));
  return static_cast<uint32_t>(operations_.size());
}

void GfxNatives::run(void* self, uint32_t operation) {
  auto& natives = *static_cast<GfxNatives*>(self);
  if (operation == 0 || operation > natives.operations_.size()) {
    ++natives.unknown_marks_;
    return;
  }
  natives.operations_[operation - 1](natives.engine_);
}

void GfxNatives::mark(uint32_t cursor_at, uint32_t operation) {
  Memory& m = kernel_.memory();
  const uint32_t cursor = m.read32(cursor_at);
  m.write32(cursor, operation);
  m.write32(cursor + 4, 0x000F0000u | 0x2F0u); // one register, all bytes
  m.write32(cursor_at, cursor + 8);
}

const std::vector<GfxNatives::Write>& GfxNatives::static_block(uint32_t address, uint32_t bytes) {
  auto& writes = blocks_[address];
  if (!writes.empty()) return writes;
  // a command block in the game's constant data: read once, its writes kept
  const Memory& m = kernel_.memory();
  for (uint32_t i = 0; i + 2 <= bytes / 4;) {
    const uint32_t first = m.read32(address + i * 4), header = m.read32(address + i * 4 + 4);
    const uint32_t reg = header & 0xFFFF, mask = header >> 16 & 0xF, extra = header >> 20 & 0xFF;
    const bool consecutive = header >> 31;
    for (uint32_t k = 0; k <= extra; ++k) {
      const uint32_t value = k == 0 ? first : m.read32(address + (i + 1 + k) * 4);
      writes.push_back({consecutive ? reg + k : reg, value, mask});
    }
    i += 2 + extra;
    i += i & 1;
  }
  return writes;
}

bool GfxNatives::lyt_flush(Thread& thread) {
  // NSMB2's nw::lyt drawer (docs/NSMB2-LYT-NATIF.md): rectangles, matrices and colors, texture coordinates, each an
  // array of vec4 behind a uniform block; the vertex program reads them all. Done here: the same uniforms, the same
  // draw, set directly in picanative, with no command made.
  Memory& m = kernel_.memory();
  const uint32_t function = thread.ctx.r[15], drawer = thread.ctx.r[0];
  const uint32_t cursor_at = literal(m, function + 0x18); // ldr r6, =cursor
  const uint32_t draw_block = literal(m, branch_target(m, function + 0x164));

  auto rows = [&](uint32_t block, uint32_t pointer, uint32_t count, uint32_t& first, std::vector<uint32_t>& words) {
    first = m.read32(drawer + block) & 0x7F; // the 0x2C0 index the block starts with
    const uint32_t at = m.read32(drawer + pointer);
    words.resize(count * 4);
    for (uint32_t i = 0; i < count * 4; ++i) words[i] = m.read32(at + i * 4);
  };
  struct Batch {
    uint32_t rect_first = 0, matrix_first = 0, coord_first = 0;
    std::vector<uint32_t> rects, matrices, coords;
    uint32_t coord_sets = 0, index = 0, count = 0;
  };
  auto b = std::make_shared<Batch>();
  uint8_t counts[3]; // rectangles, matrix rows, coordinate rows
  m.read(drawer + 0x24, counts, 3);
  const uint32_t rects = counts[0], matrices = counts[1], coords = counts[2];
  rows(0x88, 0x668, rects, b->rect_first, b->rects);
  rows(0x238, 0x66C, matrices, b->matrix_first, b->matrices);
  rows(0x448, 0x670, coords, b->coord_first, b->coords);
  b->coord_sets = m.read32(drawer + 0x69C);
  b->count = rects * 6;
  // the index buffer's offset, as 0x4944a8 picks it
  const uint32_t from = b->count > 0x10 ? b->count * 2 - 0x20 : 0;
  b->index = 0x80000000u | (((m.read32(drawer + 0x680) + from) & 0xFFF) >= 0xFE0 ? 0x20u : 0u);
  const auto* block = &static_block(draw_block, 0x40);

  // a batch drawn before (most of a menu, every frame): its operation again
  std::vector<uint32_t> key{kLytFlushBatch, b->rect_first, b->matrix_first, b->coord_first, b->coord_sets, b->index,
                            b->count, rects, matrices, coords, draw_block};
  for (const auto* part : {&b->rects, &b->matrices, &b->coords}) key.insert(key.end(), part->begin(), part->end());
  if (const auto known = known_.find(key); known != known_.end()) {
    mark(cursor_at, known->second);
    const uint8_t empty[3] = {};
    m.write(drawer + 0x24, empty, 3);
    return true;
  }

  const uint32_t operation = add([b, block](pn_engine* e) {
    pn_vertex_uniforms(e, b->rect_first, b->rects.data(), static_cast<uint32_t>(b->rects.size() / 4));
    if (!b->matrices.empty())
      pn_vertex_uniforms(e, b->matrix_first, b->matrices.data(), static_cast<uint32_t>(b->matrices.size() / 4));
    if (!b->coords.empty())
      pn_vertex_uniforms(e, b->coord_first, b->coords.data(), static_cast<uint32_t>(b->coords.size() / 4));
    pn_write_register(e, 0x2B1, b->coord_sets, 0xF); // i0: the loop over texture coordinate sets
    pn_write_register(e, 0x227, b->index, 0xF);
    pn_write_register(e, 0x228, b->count, 0xF);
    for (const Write& w : *block) pn_write_register(e, w.reg, w.value, w.mask);
  });
  known_.emplace(std::move(key), operation);
  mark(cursor_at, operation);
  // what the game's flush leaves: an empty batch
  const uint8_t empty[3] = {};
  m.write(drawer + 0x24, empty, 3);
  return true;
}

bool GfxNatives::call(Thread& thread, uint32_t which) {
  ++calls_[which];
  bool done = false;
  switch (which) {
  case kNngxCopyPreparedList:
    // not native yet: the copy, then picanative reading it, as before; counted to see how much is left
    break;
  case kLytFlushBatch:
    done = lyt_flush(thread);
    break;
  default:
    break;
  }
  if (done) ++done_[which];
  return done;
}

void GfxNatives::report() const {
  for (const auto& [which, n] : calls_) {
    const auto done = done_.find(which);
    std::printf("[gfx] %s: %llu calls, %llu done natively\n", name(which), static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(done == done_.end() ? 0 : done->second));
  }
  if (!operations_.empty() || unknown_marks_)
    std::printf("[gfx] %zu native operations, %llu marks naming none\n", operations_.size(),
                static_cast<unsigned long long>(unknown_marks_));
}

} // namespace host
