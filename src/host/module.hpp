#pragma once

// The recompiled game: the native module 3dsrecomp built from the ROM's code,
// checked against that code before anything runs.
#include "abi.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace host {

class Module {
public:
  // loads the module and checks it was generated from `code` (the ROM's
  // code.bin, as mapped at `base`); false with the reason otherwise
  bool load(const std::string& path, const uint8_t* code, uint32_t base, uint32_t size, std::string& error);
  ~Module();

  // the recompiled code for an address (bit 0 set for Thumb), in the executable or a placed CRO module, or null
  Code lookup(uint32_t address) const;
  uint32_t entry_count() const { return count_; }
  // a CRO module ldr:ro loaded at base (0: unloaded): its translated code runs there; false when there is none
  bool place(const std::string& name, uint32_t base);
  // the guest function whose translated code holds a native address (its lowest entry), or 0 when none does;
  // each function's native code taken to run up to the next one's (diagnostics: the command tracer)
  uint32_t guest_function(const void* native) const;
  void* library() const { return library_; } // the loaded module (HMODULE / dlopen handle)
  // what the module wants done before the process ends (a coverage build
  // writes its counts), as the host leaves with _Exit and no atexit runs
  void finish() const { if (finish_) finish_(); }

private:
  void* library_ = nullptr;
  void (*finish_)() = nullptr;
  const Entry* entries_ = nullptr;
  uint32_t count_ = 0;
  const ::Module* modules_ = nullptr; // the CRO modules' code, by name (3dsrecomp's recomp_modules)
  uint32_t module_count_ = 0;
  struct Placed {
    uint32_t base, size;
    const ::Module* module;
  };
  std::vector<Placed> placed_;
  // native function starts with their guest address, sorted; built again when a module is placed
  mutable std::vector<std::pair<uintptr_t, uint32_t>> natives_;
  mutable bool natives_stale_ = true;
};

} // namespace host
