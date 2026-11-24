// The port's native host: the recompiled game on our own kernel and services,
// drawn by picanative in its SDL3 window, no emulator.
// usage: ctrnative <module> <rom.3ds>  (or <module> <code.bin> <exheader.bin>)
// CTRN_HEADLESS=1: no window; CTRN_FRAMES=n: stop after n frames.
#include "profiler.hpp"
#include "dsp.hpp"
#include "gfx_native.hpp"
#include "gsp.hpp"
#include "kernel.hpp"
#include "memory.hpp"
#include "module.hpp"
#include "native.hpp"
#include "rom.hpp"
#include "input.hpp"
#include "services.hpp"
#include "trace.hpp"
#include "y2r.hpp"

#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
  uint32_t v;
  std::memcpy(&v, b.data() + at, 4);
  return v;
}

// a screen w pixels wide (400 the top, 320 the bottom) as a PPM (CTRN_SHOT_EVERY): the framebuffer is stored a column at a
// time from the bottom, in the console's byte order (ABGR, BGR, then 16-bit formats)
void save_screen(pn_engine* engine, host::Memory& memory, const pn_screen& s, const std::string& path, uint32_t w) {
  static const uint32_t bpp[] = {4, 3, 2, 2, 2, 2, 2, 2};
  const uint32_t f = s.format & 7, px = bpp[f], stride = s.stride ? s.stride : 240 * px, h = 240;
  if (!s.address) return;
  // the first call starts the GPU's copy back, the second waits for it to land in memory
  pn_sync_for_cpu(engine, s.address, stride * w);
  pn_sync_for_cpu(engine, s.address, stride * w);
  const uint8_t* fb = memory.physical(s.address, stride * w);
  if (!fb) return;
  std::vector<uint8_t> rgb(w * h * 3);
  for (uint32_t x = 0; x < w; ++x)
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* p = fb + x * stride + (h - 1 - y) * px;
      uint8_t* o = &rgb[(y * w + x) * 3];
      const uint16_t v = static_cast<uint16_t>(p[0] | p[1] << 8);
      if (f == 0) o[0] = p[3], o[1] = p[2], o[2] = p[1];
      else if (f == 1) o[0] = p[2], o[1] = p[1], o[2] = p[0];
      else if (f == 2) o[0] = (v >> 11) << 3, o[1] = ((v >> 5) & 63) << 2, o[2] = (v & 31) << 3;
      else if (f == 3) o[0] = (v >> 11) << 3, o[1] = ((v >> 6) & 31) << 3, o[2] = ((v >> 1) & 31) << 3;
      else o[0] = (v >> 12) << 4, o[1] = ((v >> 8) & 15) << 4, o[2] = ((v >> 4) & 15) << 4;
    }
  // the engine's draw count so far: a run that draws nothing is stuck, whatever the picture
  uint64_t st[3];
  pn_stats(engine, st);
  std::printf("[host] shot %s: draws %llu, pipelines %llu, shader errors %llu, framebuffer %08x format %u stride %u\n",
              path.c_str(), static_cast<unsigned long long>(st[0]), static_cast<unsigned long long>(st[1]),
              static_cast<unsigned long long>(st[2]), s.address, s.format, stride);
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

#if defined(_WIN32)
// a crash nothing else caught (a decompiled function run without the check):
// the faulting module and offset, for llvm-objdump on that DLL
const host::Module* g_crash_module = nullptr; // to name the guest function a crash is in
host::Kernel* g_crash_kernel = nullptr;        // to show the guest thread's registers and stack

LONG WINAPI report_crash(EXCEPTION_POINTERS* e) {
  void* pc = e->ExceptionRecord->ExceptionAddress;
  if (g_crash_module)
    if (const uint32_t guest = g_crash_module->guest_function(pc))
      std::fprintf(stderr, "[host] crash in the guest function %08x\n", guest);
  // the guest thread's registers as its context holds them (the translated code may keep newer ones in its own),
  // and the top of its stack
  if (g_crash_kernel)
    if (auto t = g_crash_kernel->current()) {
      const uint32_t* r = t->ctx.r;
      for (int i = 0; i < 16; ++i)
        std::fprintf(stderr, "%sr%d=%08x%s", i % 8 ? " " : "[host] ", i, r[i], i % 8 == 7 ? "\n" : "");
      for (uint32_t n = 0; n < 0x700; n += 32) {
        uint32_t w[8] = {};
        g_crash_kernel->memory().read((r[13] & ~3u) + n, w, sizeof w);
        std::fprintf(stderr, "[host] stack +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", n, w[0], w[1], w[2], w[3],
                     w[4], w[5], w[6], w[7]);
      }
      std::fflush(stderr);
      g_crash_kernel->print_threads(); // without the lock: a crash report, a torn line at worst
    }
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(pc), &module))
    GetModuleFileNameA(module, name, sizeof name);
  std::fprintf(stderr, "[host] crash: exception %08lx at %p = %s+0x%llx (access %llx)\n",
               e->ExceptionRecord->ExceptionCode, pc, name,
               static_cast<unsigned long long>(static_cast<char*>(pc) - reinterpret_cast<char*>(module)),
               e->ExceptionRecord->NumberParameters > 1 ? static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]) : 0ull);
  std::fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

// The console's GPU writes its pictures into memory as it draws; picanative keeps them on the host's GPU and
// writes them when the range is read. Where that is the linear heap, the game may meanwhile reuse the memory
// (a render target freed, its bytes given to other data) and the late write would wipe what it put there. So
// pages with a drawing pending are read-only in the game's view; its first write there has the drawing written
// first (pn_cpu_wrote), as on the console, then goes on. CTRN_NO_GUARD=1 turns it off.
struct Guard {
  static constexpr uint32_t kLinear = 0x14000000, kPages = host::Memory::kFcramSize >> 12;
  host::Kernel* kernel = nullptr;
  host::Memory* memory = nullptr;
  pn_engine* engine = nullptr;
  std::vector<uint16_t> count = std::vector<uint16_t>(kPages); // pending ranges over each linear page
  uint64_t faults = 0;
} g_guard;

void guard_protect(uint32_t page, bool guarded) { // page: of the linear heap
  DWORD old;
  VirtualProtect(g_guard.memory->fastmem() + g_guard.kLinear + (uint64_t{page} << 12), 0x1000,
                 guarded ? PAGE_READONLY : PAGE_READWRITE, &old);
}

// picanative's pending ranges (physical), under the kernel's lock
void guard_pending(void*, uint32_t address, uint32_t size, int pending) {
  if (address < host::Memory::kFcram || address >= host::Memory::kFcram + host::Memory::kFcramSize || !size) return;
  const uint32_t first = (address - host::Memory::kFcram) >> 12;
  const uint32_t last = std::min((address - host::Memory::kFcram + size - 1) >> 12, g_guard.kPages - 1);
  for (uint32_t p = first; p <= last; ++p) {
    // the game's linear mapping of that page (none: nothing of the game's code writes there)
    if (g_guard.memory->to_physical(g_guard.kLinear + (p << 12)) != host::Memory::kFcram + (p << 12)) continue;
    uint16_t& n = g_guard.count[p];
    if (pending) {
      if (n++ == 0) guard_protect(p, true);
    } else if (n && --n == 0) {
      guard_protect(p, false);
    }
  }
}

LONG WINAPI guard_writes(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD& r = *e->ExceptionRecord;
  if (r.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r.NumberParameters < 2 || r.ExceptionInformation[0] != 1 ||
      !g_guard.memory)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t at = r.ExceptionInformation[1], linear = reinterpret_cast<uintptr_t>(g_guard.memory->fastmem()) + g_guard.kLinear;
  if (at < linear || at >= linear + host::Memory::kFcramSize) return EXCEPTION_CONTINUE_SEARCH;
  const uint32_t page = static_cast<uint32_t>((at - linear) >> 12);
  auto lock = g_guard.kernel->lock();
  if (!g_guard.count[page]) {
    // released by another thread meanwhile: write again; never guarded: not ours
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof info);
    return info.Protect == PAGE_READWRITE ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
  }
  ++g_guard.faults;
  pn_cpu_wrote(g_guard.engine, host::Memory::kFcram + (page << 12), 0x1000);
  if (g_guard.count[page]) g_guard.count[page] = 0, guard_protect(page, false); // whatever is left: the game's now
  return EXCEPTION_CONTINUE_EXECUTION;
}

// CTRN_WATCH_WRITE=address: every write to the 4 bytes of guest memory there, by the game's code, the host or the
// GPU engine, with the writer (module and offset), the guest function last entered and the value: the pages
// holding them, in the game's view of its memory (fastmem, its code's writes) and the host's (the services' and
// the engine's), are made read-only, each write faults, runs one step and the page is protected again
struct {
  uintptr_t at[2] = {};
  void* page[2] = {};
  int hit = -1; // the view the step under way writes through
  const host::Kernel* kernel = nullptr;
  const host::Module* module = nullptr;
  uint32_t guest = 0, before = 0;
  bool ours = false;
} g_watch;

LONG WINAPI watch_writes(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD& r = *e->ExceptionRecord;
  if (!g_watch.page[0]) return EXCEPTION_CONTINUE_SEARCH;
  DWORD old;
  int view = -1;
  if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters > 1 && r.ExceptionInformation[0] == 1)
    for (int v = 0; v < 2; ++v)
      if (r.ExceptionInformation[1] - reinterpret_cast<uintptr_t>(g_watch.page[v]) < 0x1000) view = v;
  if (view >= 0) {
    const uintptr_t a = r.ExceptionInformation[1];
    g_watch.hit = view;
    g_watch.ours = a + 8 > g_watch.at[view] && a < g_watch.at[view] + 4;
    if (g_watch.ours) {
      std::memcpy(&g_watch.before, reinterpret_cast<void*>(g_watch.at[view]), 4);
      HMODULE module = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCSTR>(r.ExceptionAddress), &module))
        GetModuleFileNameA(module, name, sizeof name);
      const char* base = std::strrchr(name, '\\');
      std::fprintf(stderr, "[watch] %08x written by %s+0x%llx (guest function %08x), guest code last entered %08x",
                   g_watch.guest, base ? base + 1 : name,
                   static_cast<unsigned long long>(static_cast<char*>(r.ExceptionAddress) - reinterpret_cast<char*>(module)),
                   g_watch.module ? g_watch.module->guest_function(r.ExceptionAddress) : 0,
                   g_watch.kernel ? g_watch.kernel->running_pc() : 0);
    }
    VirtualProtect(g_watch.page[view], 0x1000, PAGE_READWRITE, &old);
    e->ContextRecord->EFlags |= 0x100; // one instruction, then protected again
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (r.ExceptionCode == EXCEPTION_SINGLE_STEP && g_watch.hit >= 0) {
    if (g_watch.ours) {
      uint32_t now;
      std::memcpy(&now, reinterpret_cast<void*>(g_watch.at[g_watch.hit]), 4);
      std::fprintf(stderr, ": %08x -> %08x (%s)\n", g_watch.before, now, g_watch.hit ? "host's view" : "game's view");
      g_watch.ours = false;
    }
    VirtualProtect(g_watch.page[g_watch.hit], 0x1000, PAGE_READONLY, &old);
    g_watch.hit = -1;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  SetUnhandledExceptionFilter(report_crash);
#endif
  const auto started = std::chrono::steady_clock::now();
  if (argc != 3 && argc != 4) {
    std::fprintf(stderr, "usage: ctrnative <module> <rom.3ds>\n       ctrnative <module> <code.bin> <exheader.bin>  (with CTRN_ROM for the RomFS)\n");
    return 2;
  }
  // the game: a decrypted ROM, or its code and exheader already extracted
  host::Rom rom;
  std::string rom_error;
  if (argc == 3) {
    if (!rom.load(argv[2], rom_error)) {
      std::fprintf(stderr, "%s: %s\n", argv[2], rom_error.c_str());
      return 2;
    }
#if defined(_WIN32)
    _putenv_s("CTRN_ROM", argv[2]); // the services read the RomFS from it
#else
    setenv("CTRN_ROM", argv[2], 1);
#endif
  } else {
    rom.code = read_file(argv[2]), rom.exheader = read_file(argv[3]);
    if (rom.exheader.size() >= 8)
      rom.title.assign(reinterpret_cast<const char*>(rom.exheader.data()), strnlen(reinterpret_cast<const char*>(rom.exheader.data()), 8));
  }
  const std::vector<uint8_t>& code = rom.code;
  const std::vector<uint8_t>& exheader = rom.exheader;
  if (code.empty() || exheader.size() < 0x400) {
    std::fprintf(stderr, "cannot read the code or the exheader\n");
    return 2;
  }
  {
    uint64_t hash = 14695981039346656037ull; // FNV-1a: which code this is, to compare runs
    for (uint8_t b : code) hash = (hash ^ b) * 1099511628211ull;
    std::printf("[host] %s: code %zu bytes, fnv %016llx\n", rom.title.c_str(), code.size(), static_cast<unsigned long long>(hash));
  }
  // the exheader's code set: .text, .rodata, .data (address, pages, size), stack, bss
  host::Kernel::Image image{};
  image.code = code.data();
  image.code_size = static_cast<uint32_t>(code.size());
  image.text_address = u32(exheader, 0x10), image.text_pages = u32(exheader, 0x14);
  image.stack_size = u32(exheader, 0x1C);
  image.ro_address = u32(exheader, 0x20), image.ro_pages = u32(exheader, 0x24);
  image.data_address = u32(exheader, 0x30), image.data_pages = u32(exheader, 0x34), image.data_size = u32(exheader, 0x38);
  image.bss_size = u32(exheader, 0x3C);
  image.priority = exheader[0x20F];
  // the application's memory by the old 3DS's system mode, flag 0's high nibble: 0 prod 64 MiB, 2 dev1 96 MiB,
  // 3 dev2 80 MiB, 4 dev3 72 MiB, 5 dev4 32 MiB
  switch (exheader[0x20E] >> 4) {
  case 2: image.app_memory = 96u << 20; break;
  case 3: image.app_memory = 80u << 20; break;
  case 4: image.app_memory = 72u << 20; break;
  case 5: image.app_memory = 32u << 20; break;
  default: image.app_memory = 64u << 20; break;
  }
  // CTRN_APP_MEMORY_MB=n: another size, to tell a game short of memory from one that is broken (diagnosis only)
  if (const char* mb = std::getenv("CTRN_APP_MEMORY_MB")) image.app_memory = static_cast<uint32_t>(std::atoi(mb)) << 20;
  const std::string& title = rom.title;

  host::Memory memory;
  host::Module module;
  std::string error;
  if (!module.load(argv[1], code.data(), image.text_address, image.code_size, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
#if defined(_WIN32)
  g_crash_module = &module;
#endif

  // the engine draws from the game's physical memory, and owns the window
  char message[512] = {};
  pn_engine* engine = pn_create_ex(
      [](void* user, uint32_t address, uint32_t size) { return static_cast<host::Memory*>(user)->physical(address, size); },
      // CTRN_UNCACHED=1: no surfaces kept on the GPU, every one read back (4 times slower; captures
      // work either way, each copies its screen back from the GPU first)
      &memory,
      (std::getenv("CTRN_UNCACHED") ? 0 : PN_CACHE_SURFACES) | (std::getenv("CTRN_FIXED_STEP") ? 0 : PN_ASYNC_PIPELINES),
      message, sizeof message);
  if (!engine) {
    std::fprintf(stderr, "picanative: %s\n", message);
    return 2;
  }
  // the game's GPU programs, translated when its module was built (tools/ctrn.py), beside the module
  const std::string translated = (std::filesystem::path(argv[1]).parent_path() / "shaders.pnvs").string();
  if (!pn_load_shaders(engine, translated.c_str(), message, sizeof message))
    std::printf("[host] no shaders translated ahead (%s: %s)\n", translated.c_str(), message);
  // the GPU pipelines of the game's earlier runs, made again while it starts (CTRN_NO_WARM=1: not)
  if (!std::getenv("CTRN_NO_WARM")) {
    const std::string recipes = (std::filesystem::path(argv[1]).parent_path() / "pipelines.pnpl").string();
    std::printf("[host] %u GPU pipelines of earlier runs made again ahead\n", pn_warm_pipelines(engine, recipes.c_str()));
  }
  // the internal resolution: CTRN_SCALE times the console's (1 by default, the console's pixels: an improvement is an option)
  const char* scale_env = std::getenv("CTRN_SCALE");
  pn_set_resolution_scale(engine, scale_env ? static_cast<uint32_t>(std::atoi(scale_env)) : 1);
  const bool headless = std::getenv("CTRN_HEADLESS") != nullptr;
  pn_window* window = headless ? nullptr : pn_window_open(engine, title.c_str(), 2, message, sizeof message);
  if (!headless && !window) {
    std::fprintf(stderr, "window: %s\n", message);
    return 2;
  }
  if (window) pn_window_set_vsync(window, 0); // the loop below paces the frames
  // CTRN_WINDOW_SHOT=scale: with each screenshot, what a window at that scale shows (w<frame>.ppm), with no window
  pn_window* look = nullptr;
  if (const char* shot_scale = std::getenv("CTRN_WINDOW_SHOT"))
    look = pn_offscreen_open(engine, static_cast<uint32_t>(std::max(1, std::atoi(shot_scale))));
  // improvements to how the screens are shown, off by default: CTRN_FILTER bilinear (the default), nearest or
  // sharp; CTRN_EFFECTS a list of fxaa, sharpen, lcd (the 3DS screen's colors), scanlines
  for (pn_window* target : {window, look}) {
    if (!target) continue;
    pn_window* window = target;
    const std::string filter = std::getenv("CTRN_FILTER") ? std::getenv("CTRN_FILTER") : "";
    const std::string effects = std::getenv("CTRN_EFFECTS") ? std::getenv("CTRN_EFFECTS") : "";
    uint32_t mask = 0;
    if (effects.find("fxaa") != std::string::npos) mask |= PN_EFFECT_FXAA;
    if (effects.find("sharpen") != std::string::npos) mask |= PN_EFFECT_SHARPEN;
    if (effects.find("lcd") != std::string::npos) mask |= PN_EFFECT_LCD_COLORS;
    if (effects.find("scanlines") != std::string::npos) mask |= PN_EFFECT_SCANLINES;
    pn_window_set_filter(window, filter == "nearest" ? 1 : filter == "sharp" ? 2 : 0, mask);
  }
  // how the screens share the window: CTRN_LAYOUT stacked (the console's), side, large or top (F10 cycles them)
  if (const char* layout = std::getenv("CTRN_LAYOUT"); window && layout) {
    const std::string name = layout;
    pn_window_set_layout(window, name == "side" ? PN_LAYOUT_SIDE_BY_SIDE : name == "large" ? PN_LAYOUT_LARGE_TOP
                                 : name == "top"  ? PN_LAYOUT_TOP_ONLY : PN_LAYOUT_STACKED);
  }

  host::Kernel kernel(memory, module);
#if defined(_WIN32)
  g_crash_kernel = &kernel;
  if (!std::getenv("CTRN_UNCACHED") && !std::getenv("CTRN_NO_GUARD")) {
    g_guard.kernel = &kernel, g_guard.memory = &memory, g_guard.engine = engine;
    AddVectoredExceptionHandler(1, guard_writes);
    pn_set_pending_hook(engine, guard_pending, nullptr);
  }
#endif
  if (host::trace::on()) host::trace::start(module);
  host::GfxNatives gfx_natives(kernel, engine);
  kernel.set_natives([&](host::Thread& thread, uint32_t which) { return gfx_natives.call(thread, which); });
  host::Services services;
  services.module = &module;
  services.program_id = uint64_t{u32(exheader, 0x200)} | uint64_t{u32(exheader, 0x204)} << 32;
  host::Gsp gsp(kernel, engine);
  host::Dsp dsp(kernel);
  host::Y2r y2r;
  host::native = {&kernel, &gsp, &dsp, &services}; // the native API the platform layer calls
  kernel.set_services([&](host::Kernel& k, host::Thread& t, const std::string& name, uint32_t* command) {
    if (name == "gsp::Gpu") return gsp.handle(t, command);
    if (name == "dsp::DSP") return dsp.handle(t, command);
    if (name == "y2r:u") return y2r.handle(k, command);
    return services.handle(k, t, name, command) || services.answer_neutrally(name, command);
  });
  kernel.load(image);
  std::printf("[host] %u recompiled entries; running natively (no emulator, no interpreter)\n", module.entry_count());
  std::fflush(stdout);
#if defined(_WIN32)
  timeBeginPeriod(1); // waits to the millisecond: audio frames are 4.9 ms
#endif
  // CTRN_FIXED_STEP=1: the game's time is virtual, moved frame by frame and audio block by audio block: a run
  // replays the same (same inputs, same pictures), to compare two builds picture by picture
  const bool fixed_step = std::getenv("CTRN_FIXED_STEP") != nullptr;
  kernel.set_virtual_time(fixed_step);
  kernel.start();

  // the DSP's clock: a frame of 160 samples at 32728 Hz, every 4.889 ms of real
  // time; frames late are run one by one, so their count follows the clock
  // CTRN_AUDIO_WAV=file: what the DSP puts out, stereo 16-bit at its 32728 Hz, and at the end how loud it was and
  // its longest silence (a test that the sound is there, without a player)
  std::ofstream wav;
  uint64_t wav_samples = 0, wav_silent = 0, wav_longest_silence = 0;
  int wav_peak = 0;
  if (const char* path = std::getenv("CTRN_AUDIO_WAV")) {
    wav.open(path, std::ios::binary);
    wav.write(std::string(44, '\0').data(), 44); // the header once the size is known
  }
  // one audio block: the DSP's frame, its samples to the window and the file
  auto audio_block = [&] {
        std::vector<int16_t> samples;
        {
          auto lock = kernel.lock();
          dsp.audio_frame();
          samples = dsp.take_output();
          kernel.wake();
        }
        if (window && !samples.empty())
          pn_window_audio(window, samples.data(), static_cast<uint32_t>(samples.size() / 2));
        if (wav.is_open() && !samples.empty()) {
          wav.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 2));
          for (size_t i = 0; i + 1 < samples.size(); i += 2) {
            const int level = std::max(std::abs(int{samples[i]}), std::abs(int{samples[i + 1]}));
            wav_peak = std::max(wav_peak, level);
            wav_silent = level < 64 ? wav_silent + 1 : 0; // under -54 dBFS: silence
            wav_longest_silence = std::max(wav_longest_silence, wav_silent);
          }
          wav_samples += samples.size() / 2;
        }
  };
  const uint64_t audio_ns = 160ull * 1000000000ull / 32728;
  host::profiler::enroll("main");
  host::profiler::start(module);
  std::thread audio([&] {
    host::profiler::enroll("audio");
    if (fixed_step) return; // the main loop runs the blocks at their virtual times
    const auto audio_period = std::chrono::nanoseconds(audio_ns);
    auto due = std::chrono::steady_clock::now() + audio_period;
    while (!kernel.stopped()) {
      std::this_thread::sleep_until(due);
      for (int late = 0; std::chrono::steady_clock::now() >= due && late < 8 && !kernel.stopped(); ++late) {
        audio_block();
        due += audio_period;
      }
      if (std::chrono::steady_clock::now() > due + audio_period * 8) due = std::chrono::steady_clock::now(); // far behind
    }
  });
  uint64_t audio_at = 0; // fixed step: the virtual time of the last audio block

  // the console's refresh, 59.83 Hz: the screens' vertical blanks, and what the window shows
  const auto period = std::chrono::nanoseconds(16713668);
  auto next = std::chrono::steady_clock::now();
  uint64_t frames = 0;
  const char* frames_env = std::getenv("CTRN_FRAMES");
  const uint64_t limit = frames_env ? std::strtoull(frames_env, nullptr, 10) : 0;
  uint32_t last_top = 0, pictures = 0;
  bool shown = false; // the first picture, for the start time
  // CTRN_SHOT_EVERY=n: the top screen every n frames into CTRN_SHOT_DIR (default .), f<frame>.ppm
  const char* shot_env = std::getenv("CTRN_SHOT_EVERY");
  const uint64_t shot_every = shot_env ? std::strtoull(shot_env, nullptr, 10) : 0;
  const std::string shot_dir = std::getenv("CTRN_SHOT_DIR") ? std::getenv("CTRN_SHOT_DIR") : ".";
  auto rate_since = std::chrono::steady_clock::now();
  double idle_at = 0, engine_at = 0;
  const host::TestInput test_input;
#if defined(_WIN32)
  static const char* watch_env = std::getenv("CTRN_WATCH_WRITE");
  if (watch_env) AddVectoredExceptionHandler(1, watch_writes), g_watch.kernel = &kernel, g_watch.module = &module;
#endif
  while (!kernel.stopped()) {
#if defined(_WIN32)
    if (watch_env && !g_watch.page[0]) { // armed once the game has mapped the address
      const uint32_t guest = static_cast<uint32_t>(std::strtoul(watch_env, nullptr, 16));
      if (uint8_t* host_byte = memory.host(guest)) {
        g_watch.guest = guest;
        g_watch.at[0] = reinterpret_cast<uintptr_t>(memory.fastmem() + guest), g_watch.at[1] = reinterpret_cast<uintptr_t>(host_byte);
        for (int v = 1; v >= 0; --v) { // the game's view last: page[0] set arms the handler
          void* page = reinterpret_cast<void*>(g_watch.at[v] & ~uintptr_t{0xFFF});
          DWORD old;
          VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
          g_watch.page[v] = page;
        }
        std::fprintf(stderr, "[watch] %08x armed\n", guest);
      }
    }
#endif
    pn_input input{};
    if (window && !pn_window_poll(window, &input)) {
      kernel.stop("the window was closed");
      break;
    }
    host::Services::Input frame_input{input.buttons, input.circle_x, input.circle_y, input.touch_x, input.touch_y,
                                      input.touching != 0};
    test_input.apply(frames, frame_input);
    {
      auto lock = kernel.lock();
      test_input.write(frames, memory);
      // CTRN_DUMP=frame:file: the game's mapped memory at that frame, as (address, 4 KiB) records, to diff runs
      static const char* dump_env = std::getenv("CTRN_DUMP");
      if (dump_env && std::strtoull(dump_env, nullptr, 10) == frames) {
        if (std::FILE* f = std::fopen(std::strchr(dump_env, ':') + 1, "wb")) {
          for (uint64_t page = 0; page < 0x40000000; page += 0x1000)
            if (const uint8_t* bytes = memory.host(static_cast<uint32_t>(page))) {
              const uint32_t address = static_cast<uint32_t>(page);
              std::fwrite(&address, 4, 1, f), std::fwrite(bytes, 1, 0x1000, f);
            }
          std::fclose(f);
        }
      }
      gsp.vblank();
      services.vblank(kernel, frame_input);
      const auto screens = gsp.screens();
      if (window) pn_window_present(window, screens.data());
      // the game's frame rate: a new top-screen picture is a frame it finished
      if (screens[0].address != last_top && !shown)
        shown = true, std::printf("[host] first picture after %.2f s (frame %llu)\n",
                                  std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                                  static_cast<unsigned long long>(frames));
      pictures += screens[0].address != last_top;
      last_top = screens[0].address;
      if (shot_every && frames % shot_every == 0) {
        char name[32];
        std::snprintf(name, sizeof name, "/f%06llu.ppm", static_cast<unsigned long long>(frames));
        save_screen(engine, memory, screens[0], shot_dir + name, 400);
        std::snprintf(name, sizeof name, "/b%06llu.ppm", static_cast<unsigned long long>(frames));
        save_screen(engine, memory, screens[1], shot_dir + name, 320); // the bottom screen
        if (look) {
          std::snprintf(name, sizeof name, "/w%06llu.ppm", static_cast<unsigned long long>(frames));
          pn_window_shot(look, screens.data(), (shot_dir + name).c_str());
        }
      }
    }
    // CTRN_RATE_FRAMES=n: the measure every n frames instead of every 10 s, with the game's own rate (pictures per
    // 60 frames: what the console shows) apart from the host's (pictures per second of the host's time, fixed step:
    // how fast it could go)
    static const uint64_t rate_frames = std::getenv("CTRN_RATE_FRAMES") ? std::strtoull(std::getenv("CTRN_RATE_FRAMES"), nullptr, 10) : 0;
    static uint64_t rate_frame = 0;
    if (const auto now = std::chrono::steady_clock::now();
        rate_frames ? frames - rate_frame >= rate_frames : now - rate_since >= std::chrono::seconds(10)) {
      const double seconds = std::chrono::duration<double>(now - rate_since).count();
      if (rate_frames)
        std::printf("[host] frames %llu-%llu: %.1f images per 60 frames, host %.1f images/s\n",
                    static_cast<unsigned long long>(rate_frame), static_cast<unsigned long long>(frames),
                    60.0 * pictures / static_cast<double>(frames - rate_frame), pictures / seconds);
      rate_frame = frames;
      double idle, engine;
      {
        auto lock = kernel.lock();
        idle = kernel.idle_seconds(), engine = gsp.engine_seconds;
      }
      // busy near 100 %: the host's CPU is what limits the game; lower, the game waits on the console's pace.
      // of it, the GPU's share (picanative, inside the game's requests) and the rest, the game's own code
      const double busy = 1.0 - (idle - idle_at) / seconds, gpu = (engine - engine_at) / seconds;
      std::printf("[host] %.1f images/s (top screen, last 10 s), busy %.0f %% (GPU %.0f %%, game code %.0f %%), frame %llu\n",
                  pictures / seconds, 100 * busy, 100 * gpu, 100 * (busy - gpu), static_cast<unsigned long long>(frames));
      // CTRN_PEEK=address[,address...]: 16 bytes of the game's memory at each, for diagnosis
      for (const char* p = std::getenv("CTRN_PEEK"); p && *p;) {
        char* end;
        const uint32_t address = static_cast<uint32_t>(std::strtoul(p, &end, 16));
        if (end == p) break;
        uint8_t bytes[16] = {};
        {
          auto lock = kernel.lock();
          memory.read(address, bytes, sizeof bytes);
        }
        std::printf("[host] peek %08x:", address);
        for (uint8_t b : bytes) std::printf(" %02x", b);
        std::printf("\n");
        p = *end == ',' ? end + 1 : end;
      }
      std::fflush(stdout);
      pictures = 0, rate_since = now, idle_at = idle, engine_at = engine;
    }
    gfx_natives.frame();
    if (limit && ++frames >= limit) kernel.stop("frame limit reached");
    if (fixed_step) { // the game runs up to its next frame, audio blocks on the way; no waiting for the host's clock
      const uint64_t frame_at = frames * static_cast<uint64_t>(period.count());
      while (!kernel.stopped() && audio_at + audio_ns <= frame_at) {
        {
          auto lock = kernel.lock();
          kernel.advance_to(audio_at + audio_ns, lock);
        }
        audio_at += audio_ns;
        audio_block();
      }
      auto lock = kernel.lock();
      kernel.advance_to(frame_at, lock);
      continue;
    }
    next += period;
    const auto now = std::chrono::steady_clock::now();
    if (next < now - period * 4) next = now; // far behind: no catching up in a burst
    std::this_thread::sleep_until(next);
  }
  const std::string why = kernel.wait();
  if (host::trace::on()) host::trace::report();
  gfx_natives.report();
  pn_print_register_stats(engine);
  audio.join();
  if (wav.is_open()) {
    const uint32_t rate = 32728, bytes = static_cast<uint32_t>(wav_samples * 4);
    auto u32 = [&](uint32_t v) { wav.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { wav.write(reinterpret_cast<const char*>(&v), 2); };
    wav.seekp(0);
    wav.write("RIFF", 4), u32(36 + bytes), wav.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(rate), u32(rate * 4),
        u16(4), u16(16), wav.write("data", 4), u32(bytes);
    wav.close();
    std::printf("[host] audio: %.1f s recorded, peak %d (%.1f dBFS), longest silence %.2f s\n", wav_samples / double(rate),
                wav_peak, wav_peak ? 20 * std::log10(wav_peak / 32768.0) : -999.0, wav_longest_silence / double(rate));
  }
  if (std::getenv("CTRN_SVC_STATS")) {
    kernel.report();
    const double* k = gsp.kind_seconds;
    std::printf("[host] GPU time: command lists %.1f s, FlushDataCache %.1f s, InvalidateDataCache %.1f s, "
                "fills %.1f s, display transfers %.1f s, texture copies %.1f s, refreshes %.1f s\n", k[0], k[1], k[2],
                k[3], k[5], k[6], k[4]);
    for (const auto& [kind, use] : gsp.transfer_kinds)
      std::printf("[host]   display transfer %08x -> %08x flags %08x: %llu times, %.2f s\n", kind[0], kind[1], kind[2],
                  static_cast<unsigned long long>(use.first), use.second);
  }
  uint64_t programs[3];
  double times[7];
  pn_shader_stats(engine, programs);
  pn_times(engine, times);
  host::profiler::report();
  pn_print_stats(engine);
  if (std::getenv("CTRN_LOG_THREADS")) {
    auto lock = kernel.lock();
    kernel.print_threads();
  }
#if defined(_WIN32)
  if (g_guard.faults) std::printf("[host] the game wrote over %llu pending drawings: written first\n", static_cast<unsigned long long>(g_guard.faults));
#endif
  std::printf("[host] engine: recording draws %.1f s (textures %.1f s, pipelines %.1f s), encoding %.1f s, waiting for "
              "the GPU %.1f s (storing to memory %.1f s), loading from memory %.1f s\n",
              times[0], times[1], times[2], times[3], times[4], times[5], times[6]);
  {
    double t[4];
    uint64_t calls[4] = {};
    pn_texture_times(engine, t, calls);
    std::printf("[host] textures: %llu lookups, %llu decoded; write-backs %.1f s, known %.1f s, hashing and decoding "
                "%.1f s (of it decoding %.1f s)\n", static_cast<unsigned long long>(calls[0]),
                static_cast<unsigned long long>(calls[1]), t[0], t[1], t[2], t[3]);
    std::printf("[host] engine: %llu submits, %llu render passes\n", static_cast<unsigned long long>(calls[2]),
                static_cast<unsigned long long>(calls[3]));
  }
  std::printf("[host] vertex programs: %llu translated ahead, %llu translated during the game (%.1f ms); "
              "pipelines %.1f s\n", static_cast<unsigned long long>(programs[0]),
              static_cast<unsigned long long>(programs[1]), programs[2] / 1000.0, times[2]);
  std::printf("[host] stopped after %llu frames: %s\n", static_cast<unsigned long long>(frames), why.c_str());
  std::fflush(stdout);
  module.finish();
  std::_Exit(0); // game threads may still be parked: no unwinding them
}
