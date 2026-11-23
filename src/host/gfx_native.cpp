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

bool GfxNatives::call(Thread& thread, uint32_t which) {
  ++calls_[which];
  switch (which) {
  case kNngxCopyPreparedList:
    // not native yet: the copy, then picanative reading it, as before; counted to see how much is left
    (void)thread;
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
  if (!operations_.empty() || unknown_marks_)
    std::printf("[gfx] %zu native operations, %llu marks naming none\n", operations_.size(),
                static_cast<unsigned long long>(unknown_marks_));
}

} // namespace host
