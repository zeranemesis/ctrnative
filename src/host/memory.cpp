#include "memory.hpp"

#include <cstring>

namespace host {

namespace {
constexpr uint32_t kPages = 1u << 20; // 4 GiB of 4 KiB pages
}

Memory::Memory()
    : fcram_(kFcramSize), vram_(kVramSize), dsp_(kDspRamSize), axi_(kAxiSize), reads_(new uint8_t*[kPages]()),
      writes_(new uint8_t*[kPages]()), physical_(new uint32_t[kPages]()) {}

uint8_t* Memory::physical(uint32_t address, uint32_t size) {
  auto within = [&](uint32_t base, std::vector<uint8_t>& bytes) -> uint8_t* {
    if (address < base || uint64_t{address} + size > uint64_t{base} + bytes.size()) return nullptr;
    return bytes.data() + (address - base);
  };
  if (uint8_t* p = within(kFcram, fcram_)) return p;
  if (uint8_t* p = within(kVram, vram_)) return p;
  if (uint8_t* p = within(kDspRam, dsp_)) return p;
  return within(kAxi, axi_);
}

void Memory::map(uint32_t virtual_address, uint32_t physical_address, uint32_t size, bool writable) {
  for (uint32_t offset = 0; offset < size; offset += 0x1000) {
    uint8_t* page = physical(physical_address + offset, 0x1000);
    const uint32_t index = (virtual_address + offset) >> 12;
    reads_[index] = page;
    writes_[index] = writable ? page : nullptr;
    physical_[index] = page ? physical_address + offset : 0;
  }
}

void Memory::unmap(uint32_t virtual_address, uint32_t size) {
  for (uint32_t offset = 0; offset < size; offset += 0x1000) {
    const uint32_t index = (virtual_address + offset) >> 12;
    reads_[index] = writes_[index] = nullptr;
    physical_[index] = 0;
  }
}

uint32_t Memory::to_physical(uint32_t virtual_address) const {
  const uint32_t page = physical_[virtual_address >> 12];
  return page ? page + (virtual_address & 0xFFF) : 0;
}

uint32_t Memory::read32(uint32_t address) const {
  uint32_t value = 0;
  read(address, &value, 4);
  return value;
}

void Memory::write32(uint32_t address, uint32_t value) { write(address, &value, 4); }

void Memory::read(uint32_t address, void* to, uint32_t size) const {
  auto* out = static_cast<uint8_t*>(to);
  for (uint32_t i = 0; i < size; ++i) {
    const uint8_t* page = reads_[(address + i) >> 12];
    out[i] = page ? page[(address + i) & 0xFFF] : 0;
  }
}

void Memory::write(uint32_t address, const void* from, uint32_t size) {
  const auto* in = static_cast<const uint8_t*>(from);
  for (uint32_t i = 0; i < size; ++i)
    if (uint8_t* page = writes_[(address + i) >> 12]) page[(address + i) & 0xFFF] = in[i];
}

} // namespace host
