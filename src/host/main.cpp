// The port's native host: the recompiled game on our own kernel and services,
// drawn by picanative in its SDL3 window, no emulator.
// usage: ctrnative <module> <exefs code.bin> <exheader>
// CTRN_HEADLESS=1: no window; CTRN_FRAMES=n: stop after n frames.
#include "dsp.hpp"
#include "gsp.hpp"
#include "kernel.hpp"
#include "memory.hpp"
#include "module.hpp"
#include "native.hpp"
#include "services.hpp"

#include <chrono>
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

// the top screen as a PPM (CTRN_SHOT_EVERY): the framebuffer is stored a column at a
// time from the bottom, in the console's byte order (ABGR, BGR, then 16-bit formats)
void save_screen(pn_engine* engine, host::Memory& memory, const pn_screen& s, const std::string& path) {
  static const uint32_t bpp[] = {4, 3, 2, 2, 2, 2, 2, 2};
  const uint32_t f = s.format & 7, px = bpp[f], stride = s.stride ? s.stride : 240 * px, w = 400, h = 240;
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
  std::printf("[host] shot %s: draws %llu\n", path.c_str(), static_cast<unsigned long long>(st[0]));
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

#if defined(_WIN32)
// a crash nothing else caught (a decompiled function run without the check):
// the faulting module and offset, for llvm-objdump on that DLL
LONG WINAPI report_crash(EXCEPTION_POINTERS* e) {
  void* pc = e->ExceptionRecord->ExceptionAddress;
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
#endif

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  SetUnhandledExceptionFilter(report_crash);
#endif
  if (argc != 4) {
    std::fprintf(stderr, "usage: ctrnative <module> <code.bin> <exheader.bin>\n");
    return 2;
  }
  const std::vector<uint8_t> code = read_file(argv[2]), exheader = read_file(argv[3]);
  if (code.empty() || exheader.size() < 0x400) {
    std::fprintf(stderr, "cannot read the code or the exheader\n");
    return 2;
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
  // shortcut: old-3DS system mode 0 (64 MiB for the application); decode 0x20E's high nibble for games that ask more
  image.app_memory = 64u << 20;
  // the window's title: the exheader's application title (8 bytes, zero-padded)
  const std::string title(reinterpret_cast<const char*>(exheader.data()), strnlen(reinterpret_cast<const char*>(exheader.data()), 8));

  host::Memory memory;
  host::Module module;
  std::string error;
  if (!module.load(argv[1], code.data(), image.text_address, image.code_size, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }

  // the engine draws from the game's physical memory, and owns the window
  char message[512] = {};
  pn_engine* engine = pn_create_ex(
      [](void* user, uint32_t address, uint32_t size) { return static_cast<host::Memory*>(user)->physical(address, size); },
      // captures (CTRN_SHOT_EVERY) read the screens from memory: no surfaces kept on the GPU then
      &memory, (std::getenv("CTRN_SHOT_EVERY") ? 0 : PN_CACHE_SURFACES) | PN_ASYNC_PIPELINES, message, sizeof message);
  if (!engine) {
    std::fprintf(stderr, "picanative: %s\n", message);
    return 2;
  }
  // the internal resolution: CTRN_SCALE times the console's (2 by default, 1 = the console's pixels)
  const char* scale_env = std::getenv("CTRN_SCALE");
  pn_set_resolution_scale(engine, scale_env ? static_cast<uint32_t>(std::atoi(scale_env)) : 2);
  const bool headless = std::getenv("CTRN_HEADLESS") != nullptr;
  pn_window* window = headless ? nullptr : pn_window_open(engine, title.c_str(), 2, message, sizeof message);
  if (!headless && !window) {
    std::fprintf(stderr, "window: %s\n", message);
    return 2;
  }
  if (window) pn_window_set_vsync(window, 0); // the loop below paces the frames
  // how the screens share the window: CTRN_LAYOUT stacked (the console's), side, large or top (F10 cycles them)
  if (const char* layout = std::getenv("CTRN_LAYOUT"); window && layout) {
    const std::string name = layout;
    pn_window_set_layout(window, name == "side" ? PN_LAYOUT_SIDE_BY_SIDE : name == "large" ? PN_LAYOUT_LARGE_TOP
                                 : name == "top"  ? PN_LAYOUT_TOP_ONLY : PN_LAYOUT_STACKED);
  }

  host::Kernel kernel(memory, module);
  host::Services services;
  services.program_id = uint64_t{u32(exheader, 0x200)} | uint64_t{u32(exheader, 0x204)} << 32;
  host::Gsp gsp(kernel, engine);
  host::Dsp dsp(kernel);
  host::native = {&kernel, &gsp, &dsp, &services}; // the native API the platform layer calls
  kernel.set_services([&](host::Kernel& k, host::Thread& t, const std::string& name, uint32_t* command) {
    if (name == "gsp::Gpu") return gsp.handle(t, command);
    if (name == "dsp::DSP") return dsp.handle(t, command);
    return services.handle(k, t, name, command);
  });
  kernel.load(image);
  std::printf("[host] %u recompiled entries; running natively (no emulator, no interpreter)\n", module.entry_count());
  std::fflush(stdout);
#if defined(_WIN32)
  timeBeginPeriod(1); // waits to the millisecond: audio frames are 4.9 ms
#endif
  kernel.start();

  // the DSP's clock: a frame of 160 samples at 32728 Hz, every 4.889 ms of real
  // time; frames late are run one by one, so their count follows the clock
  std::thread audio([&] {
    const auto audio_period = std::chrono::nanoseconds(160ull * 1000000000ull / 32728);
    auto due = std::chrono::steady_clock::now() + audio_period;
    while (!kernel.stopped()) {
      std::this_thread::sleep_until(due);
      for (int late = 0; std::chrono::steady_clock::now() >= due && late < 8 && !kernel.stopped(); ++late) {
        std::vector<int16_t> samples;
        {
          auto lock = kernel.lock();
          dsp.audio_frame();
          samples = dsp.take_output();
          kernel.wake();
        }
        if (window && !samples.empty())
          pn_window_audio(window, samples.data(), static_cast<uint32_t>(samples.size() / 2));
        due += audio_period;
      }
      if (std::chrono::steady_clock::now() > due + audio_period * 8) due = std::chrono::steady_clock::now(); // far behind
    }
  });

  // the console's refresh, 59.83 Hz: the screens' vertical blanks, and what the window shows
  const auto period = std::chrono::nanoseconds(16713668);
  auto next = std::chrono::steady_clock::now();
  uint64_t frames = 0;
  const char* frames_env = std::getenv("CTRN_FRAMES");
  const uint64_t limit = frames_env ? std::strtoull(frames_env, nullptr, 10) : 0;
  uint32_t last_top = 0, pictures = 0;
  // CTRN_SHOT_EVERY=n: the top screen every n frames into CTRN_SHOT_DIR (default .), f<frame>.ppm
  const char* shot_env = std::getenv("CTRN_SHOT_EVERY");
  const uint64_t shot_every = shot_env ? std::strtoull(shot_env, nullptr, 10) : 0;
  const std::string shot_dir = std::getenv("CTRN_SHOT_DIR") ? std::getenv("CTRN_SHOT_DIR") : ".";
  auto rate_since = std::chrono::steady_clock::now();
  while (!kernel.stopped()) {
    pn_input input{};
    if (window && !pn_window_poll(window, &input)) {
      kernel.stop("the window was closed");
      break;
    }
    {
      auto lock = kernel.lock();
      gsp.vblank();
      services.vblank(kernel);
      const auto screens = gsp.screens();
      if (window) pn_window_present(window, screens.data());
      // the game's frame rate: a new top-screen picture is a frame it finished
      pictures += screens[0].address != last_top;
      last_top = screens[0].address;
      if (shot_every && frames % shot_every == 0) {
        char name[32];
        std::snprintf(name, sizeof name, "/f%06llu.ppm", static_cast<unsigned long long>(frames));
        save_screen(engine, memory, screens[0], shot_dir + name);
      }
    }
    if (const auto now = std::chrono::steady_clock::now(); now - rate_since >= std::chrono::seconds(10)) {
      std::printf("[host] %.1f images/s (top screen, last 10 s)\n",
                  pictures / std::chrono::duration<double>(now - rate_since).count());
      std::fflush(stdout);
      pictures = 0, rate_since = now;
    }
    if (limit && ++frames >= limit) kernel.stop("frame limit reached");
    next += period;
    const auto now = std::chrono::steady_clock::now();
    if (next < now - period * 4) next = now; // far behind: no catching up in a burst
    std::this_thread::sleep_until(next);
  }
  const std::string why = kernel.wait();
  audio.join();
  if (std::getenv("CTRN_SVC_STATS")) kernel.report();
  std::printf("[host] stopped after %llu frames: %s\n", static_cast<unsigned long long>(frames), why.c_str());
  std::fflush(stdout);
  module.finish();
  std::_Exit(0); // game threads may still be parked: no unwinding them
}
