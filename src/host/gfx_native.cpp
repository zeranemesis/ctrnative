#include "gfx_native.hpp"

#include <cstdio>

namespace host {

namespace {

const char* name(uint32_t which) {
  switch (which) {
  case GfxNatives::kNngxCopyPreparedList: return "nngx: a prepared list copied into the current one";
  default: return "?";
  }
}

} // namespace

bool GfxNatives::call(Thread& thread, uint32_t which) {
  (void)thread;
  ++calls_[which];
  switch (which) {
  case kNngxCopyPreparedList:
    // not native yet: the copy, then picanative reading it, as before; counted to see how much is left
    return false;
  default:
    return false;
  }
}

void GfxNatives::report() const {
  for (const auto& [which, n] : calls_) {
    const auto done = done_.find(which);
    std::printf("[gfx] %s: %llu calls, %llu done natively\n", name(which), static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(done == done_.end() ? 0 : done->second));
  }
}

} // namespace host
