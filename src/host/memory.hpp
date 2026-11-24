#pragma once

// The game's memory: the console's physical memory in one shared section, and
// the process's virtual address space as 4 GiB of the host's where each
// mapping is a view of that section (fastmem), which the recompiled code
// reaches at fastmem() + address. The host keeps page tables over the same
// bytes for its own accesses.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

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

  // for the recompiled code: guest address a is at fastmem() + a, unmapped and read-only as the guest has it
  uint8_t* fastmem() const { return base_; }

  // the host's page tables over the same bytes, a host pointer per page or null (read-only pages: no write entry)
  uint8_t* const* read_pages() const { return reads_.get(); }
  uint8_t* const* write_pages() const { return writes_.get(); }

  // the host's own accesses, through the section: any permission
  uint8_t* host(uint32_t virtual_address) const {
    uint8_t* page = reads_[virtual_address >> 12];
    return page ? page + (virtual_address & 0xFFF) : nullptr;
  }
  uint32_t read32(uint32_t address) const;
  void write32(uint32_t address, uint32_t value);
  void read(uint32_t address, void* to, uint32_t size) const;
  void write(uint32_t address, const void* from, uint32_t size);
  // as the system writes: read-only pages too (ldr:ro relocating a module's code); unmapped bytes are dropped
  bool write_any(uint32_t address, const void* from, uint32_t size);

private:
  struct View {
    uint64_t end;
    uint32_t offset; // in the section
    bool writable;
  };
  // where a physical address is in the section, or -1
  static int64_t section_offset(uint32_t physical_address);
  void map_view(uint64_t start, uint32_t offset, uint64_t size, bool writable);
  void unmap_views(uint64_t start, uint64_t end);

  uint8_t* base_ = nullptr;    // the guest's 4 GiB, and a guard past them
  uint8_t* section_ = nullptr; // the whole section, for the host and the GPU
  intptr_t handle_ = -1;       // the section: a HANDLE on Windows, a memfd elsewhere
  std::map<uint64_t, View> views_; // by virtual address
  std::mutex views_mutex_;
  std::unique_ptr<uint8_t*[]> reads_, writes_;
  std::unique_ptr<uint32_t[]> physical_; // per virtual page, its physical page address
};

} // namespace host
