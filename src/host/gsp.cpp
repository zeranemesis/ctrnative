#include "gsp.hpp"

#include <cstring>

namespace host {

namespace {

constexpr uint32_t kSharedSize = 0x1000;
constexpr uint32_t kRing = 0x800, kRingStride = 0x200; // the GX command ring, per client
constexpr uint32_t kInterruptStride = 0x40;             // the interrupt queue, per client
constexpr uint32_t kFramebufferInfo = 0x200, kFramebufferClient = 0x80, kFramebufferScreen = 0x40;
// the LCD controllers' registers, as byte offsets from 0x1EF00000
constexpr uint32_t kLcd[2] = {0x400, 0x500};
constexpr uint32_t kALeft = 0x68, kARight = 0x6C, kFormat = 0x70, kSelect = 0x78, kStride = 0x90, kBLeft = 0x94,
                   kBRight = 0x98;

uint32_t header(uint32_t command, uint32_t normal, uint32_t translate) {
  return command << 16 | (normal & 0x3F) << 6 | (translate & 0x3F);
}

uint32_t fill_width(uint32_t control) {
  switch (control & 0x300) {
  case 0x000: return 2;
  case 0x100: return 3;
  default: return 4;
  }
}

} // namespace

Gsp::Gsp(Kernel& kernel, pn_engine* engine) : kernel_(kernel), engine_(engine) {}

// a virtual address a title hands GSP (VRAM, the linear heap) to the physical one the GPU takes
uint32_t Gsp::physical(uint32_t address) const {
  if (address >= 0x1F000000 && address < 0x1F600000) return Memory::kVram + (address - 0x1F000000);
  if (address >= 0x14000000 && address < 0x1C000000) return Memory::kFcram + (address - 0x14000000);
  return address;
}

void Gsp::interrupt(Interrupt id) {
  const uint32_t queue = shared();
  if (!queue) return;
  Memory& m = kernel_.memory();
  uint8_t state[2];
  m.read(queue, state, 2);
  const uint32_t index = state[0], count = state[1];
  if (count >= 0x34) { // full: the hardware sets the error byte and drops it
    const uint8_t one = 1;
    m.write(queue + 2, &one, 1);
    return;
  }
  const uint8_t value = id, next = static_cast<uint8_t>(count + 1);
  m.write(queue + 0xC + (index + count) % 0x34, &value, 1);
  m.write(queue + 1, &next, 1);
  if (event_ && event_->kind == Object::Kind::Event) kernel_.signal(static_cast<Event&>(*event_));
}

void Gsp::write_register(uint32_t offset, uint32_t value) {
  // GSP offsets are from 0x1EB00000; the GPU's registers start at 0x1EF00000
  if (offset < 0x400000) return;
  const uint32_t at = offset - 0x400000;
  if (at / 4 < registers_.size()) registers_[at / 4] = value;
}

std::array<pn_screen, 2> Gsp::screens() const {
  std::array<pn_screen, 2> out{};
  for (int s = 0; s < 2; ++s) {
    auto reg = [&](uint32_t r) { return registers_[(kLcd[s] + r) / 4]; };
    const bool b = reg(kSelect) & 1;
    out[s].address = force_black_ ? 0 : (b ? reg(kBLeft) : reg(kALeft));
    out[s].format = reg(kFormat);
    out[s].stride = reg(kStride);
  }
  return out;
}

void Gsp::apply_framebuffer_updates() {
  const uint32_t base = shared();
  if (!base) return;
  Memory& m = kernel_.memory();
  for (uint32_t s = 0; s < 2; ++s) {
    const uint32_t record = base + kFramebufferInfo + s * kFramebufferScreen; // client 0
    const uint32_t head = m.read32(record), index = head & 0xFF;
    if (!((head >> 8) & 0xFF)) continue;
    const uint32_t entry = record + 4 + index * 0x1C;
    const uint32_t left = physical(m.read32(entry + 4)), right = physical(m.read32(entry + 8));
    const uint32_t lcd = kLcd[s];
    registers_[(lcd + (index ? kBLeft : kALeft)) / 4] = left;
    registers_[(lcd + (index ? kBRight : kARight)) / 4] = right;
    registers_[(lcd + kStride) / 4] = m.read32(entry + 0xC);
    registers_[(lcd + kFormat) / 4] = m.read32(entry + 0x10);
    registers_[(lcd + kSelect) / 4] = index & 1;
    m.write32(record, index); // picked up: the flag clears, as the real GSP does
  }
}

// a title drawing past what the screens can show (a picture over one still
// waiting) has its ring held until the next refresh, which paces it as the
// console's slower GPU would (as Zakuro)
bool Gsp::outdrawn() {
  const uint32_t base = shared();
  if (!base || held_) return held_;
  Memory& m = kernel_.memory();
  for (uint32_t s = 0; s < 2; ++s) {
    const uint32_t record = base + kFramebufferInfo + s * kFramebufferScreen;
    const uint32_t head = m.read32(record);
    if (!((head >> 8) & 0xFF)) continue;
    const uint32_t index = head & 0xFF, left = m.read32(record + 4 + index * 0x1C + 4);
    if (!has_waiting_[s]) has_waiting_[s] = true, waiting_[s] = {index, left};
    else if (waiting_[s] != std::make_pair(index, left)) held_ = true;
  }
  return held_;
}

void Gsp::run_ring() {
  const uint32_t base = shared();
  if (!base) return;
  Memory& m = kernel_.memory();
  const uint32_t ring = base + kRing; // client 0
  for (;;) {
    const uint32_t control = m.read32(ring), index = control & 0xFF, count = (control >> 8) & 0xFF;
    if (!count) break;
    const uint32_t entry = ring + 0x20 + (index % 15) * 0x20;
    const uint32_t id = m.read32(entry) & 0xFF;
    uint32_t d[7];
    m.read(entry + 4, d, sizeof d);
    m.write32(ring, (index + 1) % 15 | (count - 1) << 8); // taken before it runs
    switch (id) {
    case 0x00: { // RequestDma(source, destination, size)
      std::vector<uint8_t> bytes(d[2]);
      pn_sync_for_cpu(engine_, physical(d[0]), d[2]);
      pn_cpu_wrote(engine_, physical(d[1]), d[2]);
      m.read(d[0], bytes.data(), d[2]);
      m.write(d[1], bytes.data(), d[2]);
      interrupt(kDma);
      break;
    }
    case 0x01: // ProcessCommandList(address, size)
      pn_command_list(engine_, physical(d[0] & ~7u), d[1] & ~3u);
      interrupt(kP3d);
      break;
    case 0x02: // MemoryFill(start0, value0, end0, start1, value1, end1, controls)
      for (int bank = 0; bank < 2; ++bank) {
        const uint32_t start = d[bank * 3], value = d[bank * 3 + 1], end = d[bank * 3 + 2];
        if (!start || end <= start) continue;
        const uint32_t p = physical(start);
        pn_memory_fill(engine_, p, p + (end - start), value, fill_width(d[6] >> (bank * 16)));
        interrupt(bank ? kPsc1 : kPsc0);
      }
      break;
    case 0x03: // DisplayTransfer(input, output, input dims, output dims, flags)
      pn_display_transfer(engine_, physical(d[0]), physical(d[1]), d[2], d[3], d[4]);
      interrupt(kPpf);
      break;
    case 0x04: // TextureCopy(input, output, size, input gap, output gap)
      pn_texture_copy(engine_, physical(d[0]), physical(d[1]), d[2], d[3], d[4]);
      interrupt(kPpf);
      break;
    default: break; // 0x05 CacheFlush: memory is coherent
    }
  }
}

void Gsp::vblank() {
  apply_framebuffer_updates();
  has_waiting_ = {};
  if (held_) {
    held_ = false;
    run_ring();
  }
  interrupt(kPdc0);
  interrupt(kPdc1);
  kernel_.wake();
}

bool Gsp::handle(Thread& thread, uint32_t* c) {
  (void)thread;
  Memory& m = kernel_.memory();
  const uint32_t id = c[0] >> 16;
  auto reply = [&](uint32_t words = 0, uint32_t value = 0) {
    c[0] = header(id, 1 + words, 0), c[1] = kOk;
    if (words) c[2] = value;
  };
  switch (id) {
  case 0x0001: // WriteHWRegs(offset, size, <buffer>)
  case 0x0002: // WriteHWRegsWithMask(offset, size, <buffer>, <mask buffer>)
    if (c[1] < 0x420000 && c[2] <= 0x80)
      for (uint32_t i = 0; i < c[2] / 4; ++i) {
        uint32_t value = m.read32(c[4] + i * 4);
        if (id == 0x0002) {
          const uint32_t mask = m.read32(c[6] + i * 4), at = c[1] + i * 4;
          const uint32_t old = at >= 0x400000 && (at - 0x400000) / 4 < registers_.size() ? registers_[(at - 0x400000) / 4] : 0;
          value = (old & ~mask) | (value & mask);
        }
        write_register(c[1] + i * 4, value);
      }
    reply();
    return true;
  case 0x0003: // WriteHWRegRepeat(offset, size, <buffer>)
    for (uint32_t i = 0; i < c[2] / 4; ++i) write_register(c[1], m.read32(c[4] + i * 4));
    reply();
    return true;
  case 0x0004: { // ReadHWRegs(offset, size) into a static buffer
    for (uint32_t i = 0; i < c[2] / 4; ++i) {
      const uint32_t at = c[1] + i * 4;
      const uint32_t value = at >= 0x400000 && (at - 0x400000) / 4 < registers_.size() ? registers_[(at - 0x400000) / 4] : 0;
      m.write32(c[4] + i * 4, value);
    }
    reply();
    return true;
  }
  case 0x0005: { // SetBufferSwap(screen, active, left, right, stride, format)
    const uint32_t screen = c[1], active = c[2];
    if (screen < 2) {
      const uint32_t lcd = kLcd[screen];
      registers_[(lcd + (active ? kBLeft : kALeft)) / 4] = physical(c[3]);
      registers_[(lcd + (active ? kBRight : kARight)) / 4] = physical(c[4]);
      registers_[(lcd + kStride) / 4] = c[5];
      registers_[(lcd + kFormat) / 4] = c[6];
      registers_[(lcd + kSelect) / 4] = active & 1;
    }
    reply();
    return true;
  }
  case 0x0006: // SetCommandList(address, size)
    pn_command_list(engine_, physical(c[1] & ~7u), c[2] & ~3u);
    interrupt(kP3d);
    reply();
    return true;
  case 0x0008: // FlushDataCache(address, size): the CPU wrote what the GPU will read
    pn_cpu_wrote(engine_, physical(c[1]), c[2]);
    reply();
    return true;
  case 0x0009: // InvalidateDataCache(address, size): the CPU will read what the GPU wrote
    pn_sync_for_cpu(engine_, physical(c[1]), c[2]);
    reply();
    return true;
  case 0x001F: // StoreDataCache
  case 0x000A: // RegisterInterruptEvents
  case 0x0010: case 0x0011: case 0x0019: case 0x001A: case 0x001B: case 0x001C: case 0x001E:
    reply();
    return true;
  case 0x000B: // SetLcdForceBlack(u8)
    force_black_ = (c[1] & 0xFF) != 0;
    reply();
    return true;
  case 0x000C: // TriggerCmdReqQueue
    if (!outdrawn()) run_ring();
    reply();
    return true;
  case 0x0012: reply(1, 0); return true; // GetPerfLog
  case 0x0013: { // RegisterInterruptRelayQueue(flags, _, event) -> thread index, shared memory
    event_ = kernel_.resolve(c[3]);
    if (!block_) {
      block_ = std::make_shared<SharedMemory>();
      block_->size = kSharedSize;
      block_->physical = kernel_.allocate(Kernel::kBase, kSharedSize, false);
      std::memset(m.physical(block_->physical, kSharedSize), 0, kSharedSize);
      block_handle_ = kernel_.add_handle(block_);
    }
    // 0x2A07 with success: the GPU was already set up (as Zakuro, the title skips its own setup)
    c[0] = header(id, 2, 2), c[1] = 0x2A07, c[2] = 0, c[3] = 0, c[4] = block_handle_;
    return true;
  }
  case 0x0014: event_.reset(), reply(); return true; // UnregisterInterruptRelayQueue
  case 0x0015:                                         // TryAcquireRight
  case 0x0016:                                         // AcquireRight
  case 0x0017: reply(); return true;                   // ReleaseRight
  default: return false;
  }
}

} // namespace host
