#include "trace.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace host::trace {

namespace {

constexpr uint32_t kHost = 0xFFFFFFFF;    // written by the host or the GPU engine, not by the game's code
constexpr uint32_t kUnknown = 0xFFFFFFFE; // written before its page was watched (a buffer's first use)

// the game function that wrote a word, and the ones that called it, nearest first (0: none)
constexpr int kDepth = 6;
struct Writer {
  uint32_t fn[kDepth] = {kUnknown};
  bool operator<(const Writer& o) const { return std::lexicographical_compare(fn, fn + kDepth, o.fn, o.fn + kDepth); }
};

const Module* g_module = nullptr;
std::mutex g_mutex;
std::unordered_set<uintptr_t> g_watched;           // host pages made read-only
std::unordered_map<uintptr_t, Writer> g_writers;   // host address of a word -> who wrote it last
std::map<Writer, uint64_t> g_counts;               // words sent to the GPU, by writer
uint64_t g_words = 0;

#if defined(_WIN32)
thread_local std::vector<void*> t_reprotect;
thread_local std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> t_before; // pages as they were, to diff
thread_local Writer t_writer;

bool in_module(DWORD64 rip) {
  HMODULE m = nullptr;
  return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(rip), &m) &&
         m == static_cast<HMODULE>(g_module->library());
}

// the writer, from the faulting context: its function, then up the native stack (each translated function is a
// C function, a guest call a native one) to the next two distinct game functions
Writer who(const CONTEXT* faulting) {
  Writer w;
  CONTEXT c = *faulting;
  if (!in_module(c.Rip)) return w.fn[0] = kHost, w;
  int found = 0;
  for (int frame = 0; frame < 32 && found < kDepth && c.Rip; ++frame) {
    if (in_module(c.Rip)) {
      const uint32_t fn = g_module->guest_function(reinterpret_cast<const void*>(c.Rip));
      if (fn && (found == 0 || w.fn[found - 1] != fn)) w.fn[found++] = fn;
    }
    DWORD64 image = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (!entry) break; // a leaf without unwind data: what was found will do
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, entry, &c, &handler_data, &establisher, nullptr);
  }
  for (int i = found; i < kDepth; ++i) w.fn[i] = 0;
  return w;
}

LONG WINAPI fault(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD& r = *e->ExceptionRecord;
  DWORD old;
  if (r.ExceptionCode == EXCEPTION_SINGLE_STEP) {
    if (t_reprotect.empty()) return EXCEPTION_CONTINUE_SEARCH;
    {
      // the words of the page the step changed are this writer's
      std::lock_guard<std::mutex> lock(g_mutex);
      for (const auto& [page, bytes] : t_before)
        for (uint32_t i = 0; i < 0x1000; i += 4)
          if (std::memcmp(reinterpret_cast<const void*>(page + i), bytes.data() + i, 4)) g_writers[page + i] = t_writer;
    }
    for (void* page : t_reprotect) VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
    t_reprotect.clear(), t_before.clear();
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (r.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r.NumberParameters < 2 || r.ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t page = r.ExceptionInformation[1] & ~uintptr_t{0xFFF};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_watched.count(page)) return EXCEPTION_CONTINUE_SEARCH;
  }
  if (t_reprotect.empty()) t_writer = who(e->ContextRecord); // a write spanning two pages faults twice
  t_before.emplace_back(page, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(page),
                                                   reinterpret_cast<const uint8_t*>(page) + 0x1000));
  VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
  t_reprotect.push_back(reinterpret_cast<void*>(page));
  e->ContextRecord->EFlags |= 0x100; // one instruction, then the page is watched again
  return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

std::string name(uint32_t fn) {
  char s[16];
  if (fn == kHost) return "host";
  if (fn == kUnknown) return "unknown";
  if (!fn) return "";
  std::snprintf(s, sizeof s, "%08x", fn);
  return s;
}

} // namespace

bool on() {
  static const bool enabled = std::getenv("CTRN_TRACE_CMDS") != nullptr;
  return enabled;
}

void start(const Module& module) {
  g_module = &module;
#if defined(_WIN32)
  AddVectoredExceptionHandler(1, fault);
  std::printf("[trace] the GPU command lists' writers go to %s\n", std::getenv("CTRN_TRACE_CMDS"));
#else
  std::printf("[trace] CTRN_TRACE_CMDS: Windows only for now\n");
#endif
}

void submitted(Memory& memory, uint32_t address, uint32_t size) {
#if defined(_WIN32)
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<uintptr_t> pages;
  for (uint32_t i = 0; i + 4 <= size; i += 4) {
    // the game's own view of its memory (fastmem): where its code writes, so where a protection traps it; the
    // host writes through the section instead, which these pages do not watch (its words count as unknown)
    if (!memory.host(address + i)) continue;
    const uint8_t* word = memory.fastmem() + address + i;
    const auto found = g_writers.find(reinterpret_cast<uintptr_t>(word));
    ++g_counts[found != g_writers.end() ? found->second : Writer{}];
    ++g_words;
    const uintptr_t page = reinterpret_cast<uintptr_t>(word) & ~uintptr_t{0xFFF};
    if (pages.empty() || pages.back() != page) pages.push_back(page);
  }
  // the next lists written there are watched
  for (uintptr_t page : pages) {
    if (!g_watched.insert(page).second) continue;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READONLY, &old);
  }
#else
  (void)memory, (void)address, (void)size;
#endif
}

void report() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<std::pair<uint64_t, Writer>> rows;
  std::map<uint32_t, uint64_t> by_function;
  for (const auto& [w, n] : g_counts) rows.emplace_back(n, w), by_function[w.fn[0]] += n;
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (const char* path = std::getenv("CTRN_TRACE_CMDS")) {
    std::ofstream out(path);
    out << "words,writer,caller,caller2,caller3,caller4,caller5\n";
    for (const auto& [n, w] : rows) {
      out << n;
      for (uint32_t fn : w.fn) out << ',' << name(fn);
      out << '\n';
    }
  }
  std::vector<std::pair<uint64_t, uint32_t>> top;
  for (const auto& [fn, n] : by_function) top.emplace_back(n, fn);
  std::sort(top.rbegin(), top.rend());
  std::printf("[trace] %llu command words sent; by the function that wrote them:\n", static_cast<unsigned long long>(g_words));
  for (size_t i = 0; i < top.size() && i < 20; ++i)
    std::printf("[trace]   %5.1f %%  %s\n", g_words ? 100.0 * top[i].first / g_words : 0.0, name(top[i].second).c_str());
}

} // namespace host::trace
