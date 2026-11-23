#pragma once

// The game's memory: the console's physical memory as host buffers, and the
// process's virtual address space as a page table over them, which the
// recompiled code reads directly (read_pages / write_pages).
#include <cstdint>
#include <memory>
#include <vector>

namespace host {

class Memory {
public:
  // physical memory, as the console has it
  static constexpr uint32_t kFcram = 0x20000000, kFcramSize = 0x08000000; // 128 MiB (old 3DS)
  static constexpr uint32_t kVram = 0x18000000, kVramSize = 0x00600000;   // 6 MiB
  static constexpr uint32_t kDspRam = 0x1FF00000, kDspRamSize = 0x00080000;
  static constexpr uint32_t kAxi = 0x1FF80000, kAxiSize = 0x00080000; // the configuration and shared pages

  Memory();

  // host bytes of physical [address, address + size), contiguous, or null
  uint8_t* physical(uint32_t address, uint32_t size);

  // maps virtual pages onto physical memory; writable or read-only
  void map(uint32_t virtual_address, uint32_t physical_address, uint32_t size, bool writable);
  void unmap(uint32_t virtual_address, uint32_t size);
  // the physical address behind a virtual one, or 0 when unmapped
  uint32_t to_physical(uint32_t virtual_address) const;

  // for the recompiled code
  uint8_t* const* read_pages() const { return reads_.get(); }
  uint8_t* const* write_pages() const { return writes_.get(); }

  // slow paths (page crossings, unmapped), and the host's own accesses
  uint8_t* host(uint32_t virtual_address) const {
    uint8_t* page = reads_[virtual_address >> 12];
    return page ? page + (virtual_address & 0xFFF) : nullptr;
  }
  uint32_t read32(uint32_t address) const;
  void write32(uint32_t address, uint32_t value);
  void read(uint32_t address, void* to, uint32_t size) const;
  void write(uint32_t address, const void* from, uint32_t size);

private:
  std::vector<uint8_t> fcram_, vram_, dsp_, axi_;
  std::unique_ptr<uint8_t*[]> reads_, writes_;
  std::unique_ptr<uint32_t[]> physical_; // per virtual page, its physical page address
};

} // namespace host
