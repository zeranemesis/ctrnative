#include "cro.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace host {

namespace {

// the header's fields, as byte offsets
constexpr uint32_t kMagic = 0x080, kNameOffset = 0x084, kNextCro = 0x088, kPreviousCro = 0x08C, kFixedSize = 0x098;
constexpr uint32_t kOnUnresolved = 0x0AC;
constexpr uint32_t kCodeOffset = 0x0B0, kCodeSize = 0x0B4, kModuleName = 0x0C0;
constexpr uint32_t kSegmentTable = 0x0C8, kSegmentNum = 0x0CC;
constexpr uint32_t kExportNamedTable = 0x0D0, kExportNamedNum = 0x0D4;
constexpr uint32_t kExportIndexedTable = 0x0D8, kExportIndexedNum = 0x0DC;
constexpr uint32_t kImportModuleTable = 0x0F0, kImportModuleNum = 0x0F4;
constexpr uint32_t kExternalRelocationTable = 0x0F8, kExternalRelocationNum = 0x0FC;
constexpr uint32_t kImportNamedTable = 0x100, kImportNamedNum = 0x104;
constexpr uint32_t kImportIndexedTable = 0x108, kImportIndexedNum = 0x10C;
constexpr uint32_t kImportAnonymousTable = 0x110, kImportAnonymousNum = 0x114;
constexpr uint32_t kStaticAnonymousTable = 0x120;
constexpr uint32_t kInternalRelocationTable = 0x128, kInternalRelocationNum = 0x12C;
constexpr uint32_t kStaticRelocationTable = 0x130;
constexpr uint32_t kHeaderSize = 0x138;
// the fields from the code on, (offset, size) pairs, whose offsets become addresses on rebase
constexpr uint32_t kFirstRebased = kCodeOffset, kLastRebased = kStaticRelocationTable;

constexpr uint32_t kSegmentEntry = 0xC, kExportNamedEntry = 8, kExportIndexedEntry = 4, kExportTreeEntry = 8;
constexpr uint32_t kImportModuleEntry = 0x14, kImportNamedEntry = 8, kImportIndexedEntry = 8, kImportAnonymousEntry = 8;
constexpr uint32_t kRelocationEntry = 0xC;
constexpr uint32_t kData = 2, kBss = 3;

// each table the header lists from the module name on, its entry size (1: counted in bytes)
constexpr uint32_t kTableEntrySizes[15] = {1, kSegmentEntry, kExportNamedEntry, kExportIndexedEntry, 1,
                                           kExportTreeEntry, kImportModuleEntry, kRelocationEntry, kImportNamedEntry,
                                           kImportIndexedEntry, kImportAnonymousEntry, 1, 8, kRelocationEntry,
                                           kRelocationEntry};
// the first table each fix level drops (1: what loading alone needed, 2: the imports too, 3: the exports too)
constexpr uint32_t kFixBarriers[4] = {kHeaderSize, kStaticAnonymousTable, kImportModuleTable, kExportNamedTable};

uint32_t tag_segment(uint32_t tag) { return tag & 0xF; }
uint32_t tag_offset(uint32_t tag) { return tag >> 4; }

} // namespace

uint8_t Cro::read8(uint32_t address) const {
  uint8_t value = 0;
  memory_.read(address, &value, 1);
  return value;
}

std::string Cro::string_at(uint32_t address) const {
  std::string s;
  if (!address) return s;
  for (uint32_t i = 0; i < 512; ++i) {
    const uint8_t c = read8(address + i);
    if (!c) break;
    s += static_cast<char>(c);
  }
  return s;
}

bool Cro::segment(uint32_t base, uint32_t index, Segment& out) const {
  if (index >= field(base, kSegmentNum)) return false;
  const uint32_t entry = field(base, kSegmentTable) + index * kSegmentEntry;
  out = {memory_.read32(entry), memory_.read32(entry + 4), memory_.read32(entry + 8)};
  return true;
}

void Cro::set_segment(uint32_t base, uint32_t index, const Segment& s) {
  const uint32_t entry = field(base, kSegmentTable) + index * kSegmentEntry;
  const uint32_t words[3] = {s.offset, s.size, s.kind};
  memory_.write_any(entry, words, sizeof words);
}

bool Cro::tag_address(uint32_t base, uint32_t tag, uint32_t& out) const {
  Segment s;
  if (!segment(base, tag_segment(tag), s) || tag_offset(tag) >= s.size) return false;
  out = s.offset + tag_offset(tag);
  return true;
}

uint32_t Cro::unresolved_handler(uint32_t base) const {
  uint32_t address = 0;
  tag_address(base, field(base, kOnUnresolved), address);
  return address;
}

// one relocation; the numbering is not contiguous: 2 and 10 store an address (a module's jump tables are 2), 3 an
// offset from where it goes
void Cro::relocate(uint32_t target, uint8_t kind, uint32_t addend, uint32_t symbol, uint32_t target_final) {
  uint32_t value;
  switch (kind) {
  case 0: return;
  case 2: case 10: value = symbol + addend; break;
  case 3: value = symbol + addend - target_final; break;
  default: { // branches (4, 5, 6) and 42: no title seen using them; a wrong address would be worse than saying so
    static bool said = false;
    if (!said) std::printf("[host] ldr:ro: relocation type %u is not native yet\n", kind), said = true;
    return;
  }
  }
  if (!memory_.write_any(target, &value, 4)) std::printf("[host] ldr:ro: a relocation targets unmapped %08x\n", target);
}

// a batch of external relocations, each with symbol; its first entry's byte 6 says whether it is resolved
void Cro::relocate_batch(uint32_t base, uint32_t batch, uint32_t symbol, bool resolved) {
  for (uint32_t address = batch;; address += kRelocationEntry) {
    const uint32_t tag = memory_.read32(address), addend = memory_.read32(address + 8);
    const uint8_t kind = read8(address + 4);
    const bool end = read8(address + 5) != 0;
    uint32_t target;
    if (tag_address(base, tag, target)) relocate(target, kind, addend, symbol, target);
    if (end) break;
    if (address > batch + kRelocationEntry * 0x1000) { // a malformed table must not loop forever
      std::printf("[host] ldr:ro: the relocation batch at %08x has no end\n", batch);
      break;
    }
  }
  write8(batch + 6, resolved ? 1 : 0);
}

bool Cro::rebase(uint32_t base, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size,
                 bool is_static) {
  char magic[4];
  memory_.read(base + kMagic, magic, 4);
  if (std::memcmp(magic, "CRO0", 4) != 0 && std::memcmp(magic, "CRS0", 4) != 0) return false;
  // every offset from the code on becomes an address
  if (const uint32_t name = field(base, kNameOffset)) set_field(base, kNameOffset, name + base);
  for (uint32_t f = kFirstRebased; f <= kLastRebased; f += 8)
    if (const uint32_t value = field(base, f)) set_field(base, f, value + base);

  // code and read-only data stay in the module's buffer, data and bss go where the game said; the static module's
  // segments describe the executable, already addresses
  uint32_t previous_data = 0;
  const uint32_t count = field(base, kSegmentNum);
  for (uint32_t i = 0; i < count && !is_static; ++i) {
    Segment s;
    if (!segment(base, i, s)) continue;
    if (s.kind == kData && s.size) {
      if (s.size > data_size) {
        std::printf("[host] ldr:ro: module at %08x: data of %x bytes, %x given\n", base, s.size, data_size);
        return false;
      }
      previous_data = s.offset + base;
      s.offset = data;
    } else if (s.kind == kBss && s.size) {
      if (s.size > bss_size) {
        std::printf("[host] ldr:ro: module at %08x: bss of %x bytes, %x given\n", base, s.size, bss_size);
        return false;
      }
      s.offset = bss;
    } else if (s.offset) { // the rest (empty data and bss too) stays in the buffer
      s.offset += base;
      if (s.offset > base + size) {
        std::printf("[host] ldr:ro: module at %08x: segment %u past its end\n", base, i);
        return false;
      }
    } else {
      continue;
    }
    set_segment(base, i, s);
  }

  move_table(base, kExportNamedTable, kExportNamedNum, kExportNamedEntry, {0}, true);
  move_table(base, kImportModuleTable, kImportModuleNum, kImportModuleEntry, {0, 4, 12}, true);
  move_table(base, kImportNamedTable, kImportNamedNum, kImportNamedEntry, {0, 4}, true);
  move_table(base, kImportIndexedTable, kImportIndexedNum, kImportIndexedEntry, {4}, true);
  move_table(base, kImportAnonymousTable, kImportAnonymousNum, kImportAnonymousEntry, {4}, true);
  internal_relocations(base, previous_data);
  return true;
}

void Cro::move_table(uint32_t base, uint32_t table_field, uint32_t count_field, uint32_t entry_size,
                     std::initializer_list<uint32_t> offsets, bool forward) {
  const uint32_t table = field(base, table_field), count = field(base, count_field);
  if (!table) return;
  for (uint32_t i = 0; i < count; ++i)
    for (uint32_t offset : offsets) {
      const uint32_t at = table + i * entry_size + offset, value = memory_.read32(at);
      if (!value) continue;
      const uint32_t moved = forward ? value + base : value - base;
      memory_.write_any(at, &moved, 4);
    }
}

// relocations within the module; one into its data goes to the copy still in the buffer, the game moves it after
void Cro::internal_relocations(uint32_t base, uint32_t previous_data) {
  const uint32_t table = field(base, kInternalRelocationTable), count = field(base, kInternalRelocationNum);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = table + i * kRelocationEntry;
    const uint32_t tag = memory_.read32(entry), addend = memory_.read32(entry + 8);
    const uint8_t kind = read8(entry + 4), symbol_segment = read8(entry + 5);
    Segment target_segment, symbol;
    if (!segment(base, tag_segment(tag), target_segment) || tag_offset(tag) >= target_segment.size) continue;
    const uint32_t target_final = target_segment.offset + tag_offset(tag);
    const uint32_t target = target_segment.kind == kData ? previous_data + tag_offset(tag) : target_final;
    if (!segment(base, symbol_segment, symbol)) continue;
    relocate(target, kind, addend, symbol.offset, target_final);
  }
}

// every import at the module's handler for what nothing provides, unresolved: how a module starts
void Cro::reset_external_relocations(uint32_t base) {
  const uint32_t handler = unresolved_handler(base);
  const uint32_t table = field(base, kExternalRelocationTable), count = field(base, kExternalRelocationNum);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = table + i * kRelocationEntry;
    if (handler) {
      uint32_t target;
      if (tag_address(base, memory_.read32(entry), target))
        relocate(target, read8(entry + 4), memory_.read32(entry + 8), handler, target);
    }
    write8(entry + 6, 0);
  }
}

// drops the tables the fix level says the module no longer needs, so the game can take that memory back; returns
// how much of the module is left (0: no fixing)
uint32_t Cro::fix(uint32_t base, uint32_t size, uint32_t level) {
  if (level == 0 || level > 3) return 0;
  const uint32_t barrier = kFixBarriers[level];
  uint32_t end = field(base, kCodeOffset) + field(base, kCodeSize);
  uint32_t offset = kModuleName;
  for (uint32_t entry_size : kTableEntrySizes) {
    const uint32_t table = field(base, offset), count = field(base, offset + 4);
    if (offset < barrier) {
      if (count) end = std::max(end, table + count * entry_size);
    } else {
      set_field(base, offset + 4, 0);
    }
    offset += 8;
  }
  const uint32_t used = end > base ? end - base : 0;
  const uint32_t fixed = std::min((used + 0xFFF) & ~0xFFFu, size);
  for (uint32_t f = barrier; f <= kStaticRelocationTable; f += 8) set_field(base, f, base + fixed);
  set_field(base, kFixedSize, fixed);
  return fixed;
}

Cro::Module Cro::describe(uint32_t base, uint32_t size, bool auto_link) {
  Module m;
  m.base = base, m.size = size, m.auto_link = auto_link;
  m.name = string_at(field(base, kModuleName));
  const uint32_t named = field(base, kExportNamedTable), named_count = field(base, kExportNamedNum);
  for (uint32_t i = 0; i < named_count; ++i) {
    const uint32_t entry = named + i * kExportNamedEntry;
    uint32_t address;
    if (tag_address(base, memory_.read32(entry + 4), address)) m.exports[string_at(memory_.read32(entry))] = address;
  }
  const uint32_t indexed = field(base, kExportIndexedTable), indexed_count = field(base, kExportIndexedNum);
  for (uint32_t i = 0; i < indexed_count; ++i) {
    uint32_t address = 0;
    tag_address(base, memory_.read32(indexed + i * kExportIndexedEntry), address);
    m.indexed_exports.push_back(address);
  }
  const uint32_t segments = field(base, kSegmentNum);
  for (uint32_t i = 0; i < segments; ++i) {
    Segment s;
    m.segments.push_back(segment(base, i, s) ? s.offset : 0);
  }
  return m;
}

// the lists the game walks from the static module: its next field heads the modules that link on their own, its
// previous field the others; a list's head keeps the tail in its previous field, the tail has no next
void Cro::register_module(uint32_t base, bool auto_link) {
  const uint32_t head_field = auto_link ? kNextCro : kPreviousCro, head = field(crs_, head_field);
  if (!head) {
    set_field(crs_, head_field, base);
    set_field(base, kPreviousCro, base);
  } else {
    const uint32_t tail = field(head, kPreviousCro);
    set_field(tail, kNextCro, base);
    set_field(base, kPreviousCro, tail);
    set_field(head, kPreviousCro, base);
  }
  set_field(base, kNextCro, 0);
}

void Cro::unregister_module(uint32_t base) {
  const uint32_t next = field(base, kNextCro), previous = field(base, kPreviousCro);
  if (!previous) return; // in no list
  const uint32_t heads[2] = {kNextCro, kPreviousCro};
  const uint32_t* head_field = nullptr;
  for (const uint32_t& h : heads)
    if (field(crs_, h) == base) head_field = &h;
  if (head_field) { // the head: the next one heads the list now and keeps the tail
    if (next) set_field(next, kPreviousCro, previous);
    set_field(crs_, *head_field, next);
  } else if (next) {
    set_field(previous, kNextCro, next);
    set_field(next, kPreviousCro, previous);
  } else { // the tail: the one before is the tail now
    set_field(previous, kNextCro, 0);
    for (uint32_t h : heads) {
      const uint32_t head = field(crs_, h);
      if (head && field(head, kPreviousCro) == base) set_field(head, kPreviousCro, previous);
    }
  }
  set_field(base, kNextCro, 0);
  set_field(base, kPreviousCro, 0);
}

// the module back as its file has it, offsets rather than addresses: how ldr:ro hands the buffer back
void Cro::unrebase(uint32_t base) {
  move_table(base, kExportNamedTable, kExportNamedNum, kExportNamedEntry, {0}, false);
  move_table(base, kImportModuleTable, kImportModuleNum, kImportModuleEntry, {0, 4, 12}, false);
  move_table(base, kImportNamedTable, kImportNamedNum, kImportNamedEntry, {0, 4}, false);
  move_table(base, kImportIndexedTable, kImportIndexedNum, kImportIndexedEntry, {4}, false);
  move_table(base, kImportAnonymousTable, kImportAnonymousNum, kImportAnonymousEntry, {4}, false);
  const uint32_t count = field(base, kSegmentNum);
  for (uint32_t i = 0; i < count; ++i) {
    Segment s;
    if (!segment(base, i, s)) continue;
    s.offset = s.kind == kBss || !s.offset ? 0 : s.offset - base;
    set_segment(base, i, s);
  }
  if (const uint32_t name = field(base, kNameOffset)) set_field(base, kNameOffset, name - base);
  for (uint32_t f = kFirstRebased; f <= kLastRebased; f += 8)
    if (const uint32_t value = field(base, f)) set_field(base, f, value - base);
  set_field(base, kFixedSize, 0);
}

// a symbol in the static module, then in the modules that link on their own, in the order they came
bool Cro::lookup(const std::string& name, uint32_t& address) const {
  for (const Module& m : modules_) {
    if (!m.auto_link) continue;
    if (auto it = m.exports.find(name); it != m.exports.end()) return address = it->second, true;
  }
  return false;
}

const Cro::Module* Cro::by_name(const std::string& name) const {
  for (const Module& m : modules_)
    if (m.name == name) return &m;
  return nullptr;
}

const Cro::Module* Cro::module_at(uint32_t base) const {
  for (const Module& m : modules_)
    if (m.base == base) return &m;
  return nullptr;
}

void Cro::import_named(uint32_t base) {
  const uint32_t table = field(base, kImportNamedTable), count = field(base, kImportNamedNum);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = table + i * kImportNamedEntry, batch = memory_.read32(entry + 4);
    if (!batch || batch_resolved(batch)) continue;
    const std::string name = string_at(memory_.read32(entry));
    uint32_t address;
    if (lookup(name, address)) relocate_batch(base, batch, address, true);
    else if (unresolved_.size() < 64) unresolved_.push_back(name);
  }
}

// the indexed and anonymous symbols a module takes from the modules it names
void Cro::import_from_modules(uint32_t base) {
  const uint32_t table = field(base, kImportModuleTable), count = field(base, kImportModuleNum);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = table + i * kImportModuleEntry;
    const Module* source = by_name(string_at(memory_.read32(entry)));
    if (!source) continue;
    const uint32_t indexed = memory_.read32(entry + 4), indexed_count = memory_.read32(entry + 8);
    const uint32_t anonymous = memory_.read32(entry + 12), anonymous_count = memory_.read32(entry + 16);
    for (uint32_t j = 0; j < indexed_count; ++j) {
      const uint32_t symbol = indexed + j * kImportIndexedEntry, index = memory_.read32(symbol);
      const uint32_t batch = memory_.read32(symbol + 4);
      if (!batch || batch_resolved(batch) || index >= source->indexed_exports.size()) continue;
      relocate_batch(base, batch, source->indexed_exports[index], true);
    }
    for (uint32_t j = 0; j < anonymous_count; ++j) {
      const uint32_t symbol = anonymous + j * kImportAnonymousEntry, tag = memory_.read32(symbol);
      const uint32_t batch = memory_.read32(symbol + 4);
      if (!batch || batch_resolved(batch) || tag_segment(tag) >= source->segments.size()) continue;
      relocate_batch(base, batch, source->segments[tag_segment(tag)] + tag_offset(tag), true);
    }
  }
}

// the imports of the modules already there that the new one provides
void Cro::export_to_loaded(size_t index) {
  const Module provider = modules_[index];
  for (size_t k = 0; k < modules_.size(); ++k) {
    if (k == index) continue;
    const uint32_t base = modules_[k].base;
    const uint32_t named = field(base, kImportNamedTable), named_count = field(base, kImportNamedNum);
    for (uint32_t i = 0; i < named_count; ++i) {
      const uint32_t entry = named + i * kImportNamedEntry, batch = memory_.read32(entry + 4);
      if (!batch || batch_resolved(batch)) continue;
      if (auto it = provider.exports.find(string_at(memory_.read32(entry))); it != provider.exports.end())
        relocate_batch(base, batch, it->second, true);
    }
    const uint32_t table = field(base, kImportModuleTable), count = field(base, kImportModuleNum);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t entry = table + i * kImportModuleEntry;
      if (string_at(memory_.read32(entry)) != provider.name) continue;
      const uint32_t indexed = memory_.read32(entry + 4), indexed_count = memory_.read32(entry + 8);
      const uint32_t anonymous = memory_.read32(entry + 12), anonymous_count = memory_.read32(entry + 16);
      for (uint32_t j = 0; j < indexed_count; ++j) {
        const uint32_t symbol = indexed + j * kImportIndexedEntry, which = memory_.read32(symbol);
        const uint32_t batch = memory_.read32(symbol + 4);
        if (!batch || batch_resolved(batch) || which >= provider.indexed_exports.size()) continue;
        relocate_batch(base, batch, provider.indexed_exports[which], true);
      }
      for (uint32_t j = 0; j < anonymous_count; ++j) {
        const uint32_t symbol = anonymous + j * kImportAnonymousEntry, tag = memory_.read32(symbol);
        const uint32_t batch = memory_.read32(symbol + 4);
        if (!batch || batch_resolved(batch) || tag_segment(tag) >= provider.segments.size()) continue;
        relocate_batch(base, batch, provider.segments[tag_segment(tag)] + tag_offset(tag), true);
      }
    }
  }
}

bool Cro::initialize(uint32_t base, uint32_t size) {
  if (!rebase(base, size, 0, 0, 0, 0, true)) return false;
  reset_external_relocations(base);
  Module m = describe(base, size, true);
  set_field(base, kNextCro, 0); // no module hangs off it yet
  set_field(base, kPreviousCro, 0);
  std::printf("[host] ldr:ro: static module %s at %08x, %zu named exports\n", m.name.c_str(), base, m.exports.size());
  crs_ = base;
  modules_.clear();
  modules_.push_back(std::move(m));
  return true;
}

bool Cro::load(uint32_t base, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size,
               bool auto_link, uint32_t fix_level, uint32_t& fixed) {
  if (!rebase(base, size, data, data_size, bss, bss_size, false)) return false;
  reset_external_relocations(base);
  modules_.push_back(describe(base, size, auto_link));
  const size_t index = modules_.size() - 1;
  if (crs_) register_module(base, auto_link);
  // linked against what is there, then what is there against it
  import_named(base);
  import_from_modules(base);
  export_to_loaded(index);
  fixed = fix(base, size, fix_level);
  return true;
}

void Cro::unload(uint32_t base) {
  auto found = std::find_if(modules_.begin(), modules_.end(), [&](const Module& m) { return m.base == base; });
  if (found == modules_.end()) return;
  const Module gone = *found;
  modules_.erase(found);
  // what linked against it points at its own handler again, or it would call into memory the game reuses
  for (const Module& other : modules_) {
    if (!other.auto_link) continue;
    const uint32_t ob = other.base, handler = unresolved_handler(ob);
    auto reset = [&](uint32_t batch) {
      if (batch && batch_resolved(batch)) relocate_batch(ob, batch, handler, false);
    };
    const uint32_t named = field(ob, kImportNamedTable), named_count = field(ob, kImportNamedNum);
    for (uint32_t i = 0; i < named_count; ++i) {
      const uint32_t entry = named + i * kImportNamedEntry;
      if (gone.exports.count(string_at(memory_.read32(entry)))) reset(memory_.read32(entry + 4));
    }
    const uint32_t table = field(ob, kImportModuleTable), count = field(ob, kImportModuleNum);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t entry = table + i * kImportModuleEntry;
      if (string_at(memory_.read32(entry)) != gone.name) continue;
      const uint32_t tables[2][3] = {{memory_.read32(entry + 4), memory_.read32(entry + 8), kImportIndexedEntry},
                                     {memory_.read32(entry + 12), memory_.read32(entry + 16), kImportAnonymousEntry}};
      for (const auto& t : tables)
        for (uint32_t j = 0; j < t[1]; ++j) reset(memory_.read32(t[0] + j * t[2] + 4));
    }
  }
  if (crs_) unregister_module(base);
  unrebase(base);
}

} // namespace host
