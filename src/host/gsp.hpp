#pragma once

// gsp::Gpu, natively: the GX command ring, the screens, the interrupts, with
// picanative drawing (its engine and its SDL3 window) and no emulator between.
#include "kernel.hpp"

#include <picanative/api.h>

#include <array>
#include <map>
#include <memory>

namespace host {

class Gsp {
public:
  // the engine reaches the game's physical memory directly
  Gsp(Kernel& kernel, pn_engine* engine);

  // a request to the service (with the kernel's lock); false when not there yet
  bool handle(Thread& thread, uint32_t* command);

  // the screens refresh: the pictures set for them are taken, a ring held for
  // it runs, the vertical blank interrupts go out (with the kernel's lock)
  void vblank();
  // seconds spent in the GPU's requests and refreshes (picanative's work, with the kernel's lock)
  double engine_seconds = 0;
  // by kind (CTRN_SVC_STATS): command lists, FlushDataCache, InvalidateDataCache, transfers and fills, refreshes
  std::map<std::array<uint32_t, 3>, std::pair<uint64_t, double>> transfer_kinds; // count, seconds
  double kind_seconds[7] = {}; // ..., 5 display transfers, 6 texture copies (3 fills only)
  // what the window shows (with the kernel's lock)
  std::array<pn_screen, 2> screens() const;

private:
  enum Interrupt : uint8_t { kPsc0, kPsc1, kPdc0, kPdc1, kPpf, kP3d, kDma };
  void interrupt(Interrupt id);
  void run_ring();
  bool outdrawn();
  void apply_framebuffer_updates();
  void write_register(uint32_t offset, uint32_t value);
  uint32_t physical(uint32_t address) const;
  uint32_t shared() const { return block_ ? block_->mapped_at : 0; }

  Kernel& kernel_;
  pn_engine* engine_;
  std::shared_ptr<SharedMemory> block_;
  uint32_t block_handle_ = 0;
  std::shared_ptr<Object> event_;
  std::vector<uint32_t> registers_ = std::vector<uint32_t>(0x20000 / 4); // 0x1EF00000.., the LCD and transfer engines
  bool held_ = false, force_black_ = false;
  std::array<std::pair<uint32_t, uint32_t>, 2> waiting_{};
  std::array<bool, 2> has_waiting_{};
};

} // namespace host
