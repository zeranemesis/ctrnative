#include "module.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace host {

namespace {

uint64_t fnv(const uint8_t* bytes, size_t size) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
  return hash;
}

} // namespace

bool Module::load(const std::string& path, const uint8_t* code, uint32_t base, uint32_t size, std::string& error) {
#if defined(_WIN32)
  HMODULE library = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!library) {
    error = "cannot load " + path + " (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  auto symbol = [&](const char* name) { return reinterpret_cast<const void*>(GetProcAddress(library, name)); };
#else
  void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    error = "cannot load " + path + ": " + dlerror();
    return false;
  }
  auto symbol = [&](const char* name) { return static_cast<const void*>(dlsym(library, name)); };
#endif
  library_ = library;
  const auto* abi = static_cast<const uint32_t*>(symbol("recomp_abi"));
  const auto* count = static_cast<const uint32_t*>(symbol("recomp_entry_count"));
  const auto* origins = static_cast<const Origins*>(symbol("recomp_origins"));
  entries_ = static_cast<const Entry*>(symbol("recomp_entries"));
  if (!abi || *abi != RECOMP_ABI || !count || !*count || !entries_ || !origins || !origins->pieces) {
    error = "not a 3dsrecomp module of ABI " + std::to_string(RECOMP_ABI);
    return false;
  }
  count_ = *count;
  finish_ = reinterpret_cast<void (*)()>(const_cast<void*>(symbol("recomp_finish")));
  // the module was generated from this very code: every piece's hash matches
  for (uint32_t i = 0; i < origins->piece_count; ++i) {
    const Piece& piece = origins->pieces[i];
    if (piece.start < base || piece.end < piece.start || piece.end > base + size ||
        fnv(code + (piece.start - base), piece.end - piece.start) != piece.hash) {
      error = "the module was generated from different code";
      return false;
    }
  }
  for (uint32_t i = 1; i < count_; ++i)
    if (entries_[i - 1].address >= entries_[i].address) {
      error = "the module's entries are not sorted";
      return false;
    }
  return true;
}

Module::~Module() {
#if defined(_WIN32)
  if (library_) FreeLibrary(static_cast<HMODULE>(library_));
#else
  if (library_) dlclose(library_);
#endif
}

Code Module::lookup(uint32_t address) const {
  uint32_t low = 0, high = count_;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    if (entries_[mid].address < address) low = mid + 1;
    else high = mid;
  }
  return low < count_ && entries_[low].address == address ? entries_[low].code : nullptr;
}

} // namespace host
