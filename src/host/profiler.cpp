#include "profiler.hpp"

#include "module.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace host::profiler {

#if defined(_WIN32)
namespace {

struct Enrolled {
  HANDLE thread;
  std::string role;
};

struct State {
  std::mutex mutex;
  std::vector<Enrolled> threads;
  const Module* module = nullptr;
  HMODULE game = nullptr; // the recompiled code's DLL
  // the samples: the game's by guest function, the rest by address (named at the report)
  std::unordered_map<uint32_t, uint64_t> guest;
  std::unordered_map<uint64_t, uint64_t> native;
  std::map<std::string, uint64_t> by_role;
  uint64_t samples = 0;
};

State& state() {
  static State s;
  return s;
}

const char* setting() { return std::getenv("CTRN_PROFILE"); }

HMODULE module_of(uint64_t address) {
  HMODULE m = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(address), &m);
  return m;
}

std::string file_name(HMODULE m) {
  char path[MAX_PATH] = "?";
  if (m) GetModuleFileNameA(m, path, sizeof path);
  const char* slash = std::strrchr(path, '\\');
  return slash ? slash + 1 : path;
}

void sampler() {
  State& s = state();
  timeBeginPeriod(1);
  std::vector<std::pair<uint64_t, size_t>> taken;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    taken.clear();
    {
      std::lock_guard lock(s.mutex);
      // nothing allocated while a thread is stopped (it may hold the heap's lock)
      for (size_t i = 0; i < s.threads.size(); ++i) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(s.threads[i].thread) == static_cast<DWORD>(-1)) continue;
        const bool got = GetThreadContext(s.threads[i].thread, &context);
        ResumeThread(s.threads[i].thread);
        if (got) taken.emplace_back(context.Rip, i);
      }
      for (const auto& [rip, i] : taken) {
        ++s.samples;
        ++s.by_role[s.threads[i].role];
        // shortcut: the module's table of functions is read while ldr:ro may add one; a rare wrong name at worst
        if (s.game && module_of(rip) == s.game) {
          if (const uint32_t g = s.module->guest_function(reinterpret_cast<const void*>(rip))) {
            ++s.guest[g];
            continue;
          }
        }
        ++s.native[rip];
      }
    }
  }
}

struct Named {
  std::string kind, name;
};

Named name_of(uint64_t address) {
  const HMODULE m = module_of(address);
  const std::string file = file_name(m);
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  DWORD64 displacement = 0;
  std::string name = file;
  if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) name += "!" + std::string(symbol->Name);
  else if (m) {
    char offset[32];
    std::snprintf(offset, sizeof offset, "+0x%llx", static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(m)));
    name += offset;
  }
  std::string kind;
  std::string lower = file;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower == "picanative_engine.dll") kind = "picanative";
  else if (lower == "ctrnative.exe") kind = "host";
  else if ((lower == "ntdll.dll" || lower == "kernelbase.dll" || lower == "kernel32.dll") &&
           (name.find("Wait") != std::string::npos || name.find("Delay") != std::string::npos ||
            name.find("Sleep") != std::string::npos || name.find("Alert") != std::string::npos))
    kind = "waiting";
  else if (lower == "ntdll.dll" || lower == "kernelbase.dll" || lower == "kernel32.dll" || lower == "ucrtbase.dll" ||
           lower.rfind("vcruntime", 0) == 0 || lower.rfind("msvcp", 0) == 0)
    kind = "system";
  else if (lower == "webgpu_dawn.dll" || lower.find("d3d") != std::string::npos || lower.find("dxgi") != std::string::npos ||
           lower.find("nv") == 0 || lower.find("amd") == 0 || lower.find("ig") == 0)
    kind = "gpu driver";
  else kind = "other";
  return {kind, name};
}

} // namespace

bool on() { return setting() != nullptr; }

void enroll(const char* role) {
  if (!on()) return;
  HANDLE thread = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
  if (!thread) return;
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.threads.push_back({thread, role});
}

void start(const Module& module) {
  if (!on()) return;
  State& s = state();
  s.module = &module;
  module.guest_function(nullptr); // its table made here, not under a stopped thread
  // the recompiled code's DLL: the one holding the guest functions' native code
  {
    std::lock_guard lock(s.mutex);
    s.game = GetModuleHandleA("module.dll");
  }
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  std::thread(sampler).detach();
}

void report() {
  if (!on()) return;
  State& s = state();
  std::unordered_map<uint32_t, uint64_t> guest;
  std::unordered_map<uint64_t, uint64_t> native;
  std::map<std::string, uint64_t> by_role;
  uint64_t samples;
  {
    std::lock_guard lock(s.mutex);
    guest = s.guest, native = s.native, by_role = s.by_role, samples = s.samples;
  }
  if (!samples) return;
  // by function name, and by kind
  std::map<std::string, std::pair<std::string, uint64_t>> functions; // name -> kind, samples
  std::map<std::string, uint64_t> kinds;
  for (const auto& [g, n] : guest) {
    char name[32];
    std::snprintf(name, sizeof name, "game %08x", g);
    functions[name] = {"game", n};
    kinds["game"] += n;
  }
  for (const auto& [address, n] : native) {
    const Named named = name_of(address);
    auto& f = functions[named.name];
    f.first = named.kind, f.second += n;
    kinds[named.kind] += n;
  }
  std::vector<std::pair<uint64_t, std::string>> order;
  for (const auto& [name, f] : functions) order.emplace_back(f.second, name);
  std::sort(order.rbegin(), order.rend());
  std::printf("[profile] %llu samples:", static_cast<unsigned long long>(samples));
  for (const auto& [kind, n] : kinds) std::printf(" %s %.1f %%", kind.c_str(), 100.0 * n / samples);
  std::printf("\n[profile] by thread:");
  for (const auto& [role, n] : by_role) std::printf(" %s %.1f %%", role.c_str(), 100.0 * n / samples);
  std::printf("\n");
  for (size_t i = 0; i < order.size() && i < 40; ++i)
    std::printf("[profile] %5.1f %%  %-12s %s\n", 100.0 * order[i].first / samples,
                functions[order[i].second].first.c_str(), order[i].second.c_str());
  const char* path = setting();
  if (path && std::strcmp(path, "1") != 0)
    if (std::FILE* f = std::fopen(path, "w")) {
      std::fprintf(f, "kind,function,samples,share\n");
      for (const auto& [n, name] : order)
        std::fprintf(f, "%s,\"%s\",%llu,%.4f\n", functions[name].first.c_str(), name.c_str(),
                     static_cast<unsigned long long>(n), static_cast<double>(n) / samples);
      std::fclose(f);
    }
  std::fflush(stdout);
}

#else
bool on() { return false; }
void enroll(const char*) {}
void start(const Module&) {}
void report() {}
#endif

} // namespace host::profiler
