#include "module.hpp"

#include <algorithm>
#include <filesystem>
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
  // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR refuses a relative path (error 87)
  const std::string full = std::filesystem::absolute(path).string();
  HMODULE library = LoadLibraryExA(full.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
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
  if (const auto* modules = static_cast<const uint32_t*>(symbol("recomp_module_count")); modules && *modules) {
    modules_ = static_cast<const ::Module*>(symbol("recomp_modules"));
    module_count_ = modules_ ? *modules : 0;
  }
  finish_ =reinterpret_cast<void (*)()>(const_cast<void*>(symbol("recomp_finish")));
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

namespace {

Code find(const Entry* entries, uint32_t count, uint32_t address) {
  const Entry* end = entries + count;
  const Entry* at = std::lower_bound(entries, end, address, [](const Entry& e, uint32_t a) { return e.address < a; });
  return at != end && at->address == address ? at->code : nullptr;
}

} // namespace

Code Module::lookup(uint32_t address) const {
  if (Code code = find(entries_, count_, address)) return code;
  for (const Placed& p : placed_) // a module's entries are offsets from where it was loaded
    if (address - p.base < p.size) return find(p.module->entries, p.module->count, address - p.base);
  return nullptr;
}

uint32_t Module::guest_function(const void* native) const {
  if (natives_stale_) {
    // a function is entered at many labels, one Code for all: its lowest guest address names it
    std::vector<std::pair<uintptr_t, uint32_t>> all;
    auto add = [&](const Entry* entries, uint32_t count, uint32_t base) {
      for (uint32_t i = 0; i < count; ++i)
        if (entries[i].code) all.emplace_back(reinterpret_cast<uintptr_t>(entries[i].code), base + entries[i].address);
    };
    add(entries_, count_, 0);
    for (const Placed& p : placed_) add(p.module->entries, p.module->count, p.base);
    std::sort(all.begin(), all.end());
    natives_.clear();
    for (const auto& e : all)
      if (natives_.empty() || natives_.back().first != e.first) natives_.push_back(e);
      else natives_.back().second = std::min(natives_.back().second, e.second);
    natives_stale_ = false;
  }
  const uintptr_t at = reinterpret_cast<uintptr_t>(native);
  auto it = std::upper_bound(natives_.begin(), natives_.end(), std::pair<uintptr_t, uint32_t>{at, UINT32_MAX});
  if (it == natives_.begin()) return 0;
  --it;
  // past the last function: not this module's code (a 1 MiB margin, generous for one function)
  if (std::next(it) == natives_.end() && at - it->first > 0x100000) return 0;
  return it->second;
}

bool Module::place(const std::string& name, uint32_t base) {
  for (uint32_t i = 0; i < module_count_; ++i) {
    const ::Module& m = modules_[i];
    if (name != m.name) continue;
    *m.base = base; // what the module's code reads on entry
    std::erase_if(placed_, [&](const Placed& p) { return p.module == &m; });
    if (base) placed_.push_back({base, m.size, &m});
    natives_stale_ = true;
    return true;
  }
  return false;
}

} // namespace host
