#pragma once

// CRO modules, the code a game loads while it runs (ldr:ro), and the CRS, the static module that lists what
// the main executable exports: rebased from file offsets to addresses, relocated and linked in the game's memory
// as ldr:ro leaves them. A port of Zakuro's (fearkov, MIT) cro module, which follows Citra's ldr:ro.
#include "memory.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace host {

class Cro {
public:
  struct Module {
    std::string name;
    uint32_t base = 0, size = 0;
    std::map<std::string, uint32_t> exports; // named exports, to addresses
    std::vector<uint32_t> indexed_exports;
    std::vector<uint32_t> segments; // each segment's address
    bool auto_link = false;
  };

  explicit Cro(Memory& memory) : memory_(memory) {}

  // Initialize: the static module at base, whose exports the modules link against; false if it is not one
  bool initialize(uint32_t base, uint32_t size);
  // LoadCRO: the module at base rebased, relocated and linked; how much of it is left once fixed (0: all),
  // or false if it is not a module
  bool load(uint32_t base, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size,
            bool auto_link, uint32_t fix_level, uint32_t& fixed);
  // UnloadCRO: what linked against it goes back to its own handler, the module back to file offsets
  void unload(uint32_t base);

  const Module* module_at(uint32_t base) const;
  // the last module loaded
  const Module& last() const { return modules_.back(); }
  const std::vector<std::string>& unresolved() const { return unresolved_; }

private:
  struct Segment {
    uint32_t offset = 0, size = 0, kind = 0; // kind: 0 code, 1 read-only data, 2 data, 3 bss
  };

  uint32_t field(uint32_t base, uint32_t offset) const { return memory_.read32(base + offset); }
  void set_field(uint32_t base, uint32_t offset, uint32_t value) { memory_.write_any(base + offset, &value, 4); }
  uint8_t read8(uint32_t address) const;
  void write8(uint32_t address, uint8_t value) { memory_.write_any(address, &value, 1); }
  std::string string_at(uint32_t address) const;

  bool segment(uint32_t base, uint32_t index, Segment& out) const;
  void set_segment(uint32_t base, uint32_t index, const Segment& s);
  bool tag_address(uint32_t base, uint32_t tag, uint32_t& out) const;
  uint32_t unresolved_handler(uint32_t base) const;

  void relocate(uint32_t target, uint8_t kind, uint32_t addend, uint32_t symbol, uint32_t target_final);
  void relocate_batch(uint32_t base, uint32_t batch, uint32_t symbol, bool resolved);
  bool batch_resolved(uint32_t batch) const { return batch && read8(batch + 6) != 0; }

  bool rebase(uint32_t base, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size,
              bool is_static);
  void move_table(uint32_t base, uint32_t table_field, uint32_t count_field, uint32_t entry_size,
                  std::initializer_list<uint32_t> offsets, bool forward);
  void internal_relocations(uint32_t base, uint32_t previous_data);
  void reset_external_relocations(uint32_t base);
  uint32_t fix(uint32_t base, uint32_t size, uint32_t level);
  Module describe(uint32_t base, uint32_t size, bool auto_link);
  void register_module(uint32_t base, bool auto_link);
  void unregister_module(uint32_t base);
  void unrebase(uint32_t base);

  // linking
  bool lookup(const std::string& name, uint32_t& address) const;
  const Module* by_name(const std::string& name) const;
  void import_named(uint32_t base);
  void import_from_modules(uint32_t base);
  void export_to_loaded(size_t index);

  Memory& memory_;
  uint32_t crs_ = 0;
  std::vector<Module> modules_; // the static module first, then the loaded ones, oldest first
  std::vector<std::string> unresolved_;
};

} // namespace host
