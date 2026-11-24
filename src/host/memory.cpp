#include "memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "onecore.lib") // VirtualAlloc2, MapViewOfFile3
#else
#include <csignal>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace host {

namespace {
constexpr uint32_t kPages = 1u << 20;                 // 4 GiB of 4 KiB pages
constexpr uint64_t kSpace = (1ull << 32) + 0x10000;   // and 64 KiB past them, for a span near the end
// the section: FCRAM, VRAM, DSP RAM, AXI one after another
constexpr uint32_t kVramAt = Memory::kFcramSize, kDspAt = kVramAt + Memory::kVramSize, kAxiAt = kDspAt + Memory::kDspRamSize,
                   kSectionSize = kAxiAt + Memory::kAxiSize;

const Memory* the_memory = nullptr; // one process, for the fault handler

[[noreturn]] void fail(const char* what, uint64_t at) {
  std::fprintf(stderr, "[host] memory: %s at %08llx failed\n", what, static_cast<unsigned long long>(at));
  std::abort();
}

// a fault of the recompiled code in the guest's address space: which guest address, and what is there
bool report_guest_fault(uintptr_t address, const char* access) {
  const Memory* m = the_memory;
  if (!m || address < uintptr_t(m->fastmem()) || address - uintptr_t(m->fastmem()) >= kSpace) return false;
  const uint64_t guest = address - uintptr_t(m->fastmem());
  const bool mapped = guest < (1ull << 32) && m->read_pages()[guest >> 12];
  std::fprintf(stderr, "[host] the game %s %08llx, %s\n", access, static_cast<unsigned long long>(guest),
               mapped ? "which is read-only" : "where nothing is mapped");
  std::fflush(stderr);
  return true;
}

#if defined(_WIN32)
LONG WINAPI on_fault(EXCEPTION_POINTERS* e) {
  const EXCEPTION_RECORD* r = e->ExceptionRecord;
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
    report_guest_fault(r->ExceptionInformation[1], r->ExceptionInformation[0] == 1 ? "wrote to" : "read");
  return EXCEPTION_CONTINUE_SEARCH; // the crash report and the end, as any crash
}
#else
struct sigaction previous_segv, previous_bus;
void on_fault(int signal, siginfo_t* info, void*) {
  report_guest_fault(reinterpret_cast<uintptr_t>(info->si_addr), "reached"); // read or write: only the ucontext says
  sigaction(signal, signal == SIGSEGV ? &previous_segv : &previous_bus, nullptr); // the access faults again, there
}
#endif
} // namespace

Memory::Memory() : reads_(new uint8_t*[kPages]()), writes_(new uint8_t*[kPages]()), physical_(new uint32_t[kPages]()) {
#if defined(_WIN32)
  base_ = static_cast<uint8_t*>(
      VirtualAlloc2(nullptr, nullptr, kSpace, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kSectionSize, nullptr);
  if (!base_ || !section) fail("reserving the guest's memory", 0);
  handle_ = reinterpret_cast<intptr_t>(section);
  section_ = static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, kSectionSize));
  if (!section_) fail("mapping physical memory", 0);
  AddVectoredExceptionHandler(1, on_fault);
#else
  base_ = static_cast<uint8_t*>(mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "ctrnative", 0));
  if (base_ == MAP_FAILED || fd < 0 || ftruncate(fd, kSectionSize) != 0) fail("reserving the guest's memory", 0);
  handle_ = fd;
  section_ = static_cast<uint8_t*>(mmap(nullptr, kSectionSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (section_ == MAP_FAILED) fail("mapping physical memory", 0);
  struct sigaction action = {};
  action.sa_sigaction = on_fault;
  action.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &action, &previous_segv);
  sigaction(SIGBUS, &action, &previous_bus);
#endif
  the_memory = this;
}

int64_t Memory::section_offset(uint32_t address) {
  if (address - kFcram < kFcramSize) return address - kFcram;
  if (address - kVram < kVramSize) return kVramAt + (address - kVram);
  if (address - kDspRam < kDspRamSize) return kDspAt + (address - kDspRam);
  if (address - kAxi < kAxiSize) return kAxiAt + (address - kAxi);
  return -1;
}

uint8_t* Memory::physical(uint32_t address, uint32_t size) {
  const int64_t at = section_offset(address);
  if (at < 0 || size == 0) return at < 0 ? nullptr : section_ + at;
  // contiguous: the last byte in the same piece of physical memory
  const int64_t last = section_offset(address + size - 1);
  return last == at + size - 1 ? section_ + at : nullptr;
}

void Memory::map_view(uint64_t start, uint32_t offset, uint64_t size, bool writable) {
#if defined(_WIN32)
  // the placeholder around [start, start + size): the free run between two views, merged, then cut to it
  auto next = views_.lower_bound(start);
  const uint64_t free_end = next == views_.end() ? kSpace : next->first;
  const uint64_t free_start = next == views_.begin() ? 0 : std::prev(next)->second.end;
  VirtualFree(base_ + free_start, free_end - free_start, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS); // fails when one already
  if ((free_start != start || free_end != start + size) &&
      !VirtualFree(base_ + start, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
    fail("splitting a placeholder", start);
  if (!MapViewOfFile3(reinterpret_cast<HANDLE>(handle_), GetCurrentProcess(), base_ + start, offset, size,
                      MEM_REPLACE_PLACEHOLDER, writable ? PAGE_READWRITE : PAGE_READONLY, nullptr, 0))
    fail("mapping a view", start);
#else
  if (mmap(base_ + start, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED | MAP_FIXED, static_cast<int>(handle_),
           offset) == MAP_FAILED)
    fail("mapping a view", start);
#endif
  views_[start] = View{start + size, offset, writable};
}

void Memory::unmap_views(uint64_t start, uint64_t end) {
  auto it = views_.upper_bound(start);
  if (it != views_.begin()) --it;
  while (it != views_.end() && it->first < end) {
    const uint64_t from = it->first;
    const View v = it->second;
    if (v.end <= start) {
      ++it;
      continue;
    }
#if defined(_WIN32)
    if (!UnmapViewOfFile2(GetCurrentProcess(), base_ + from, MEM_PRESERVE_PLACEHOLDER)) fail("unmapping a view", from);
#else
    if (mmap(base_ + from, v.end - from, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED)
      fail("unmapping a view", from);
#endif
    it = views_.erase(it);
    // what of the view lies outside [start, end) stays mapped
    if (from < start) map_view(from, v.offset, start - from, v.writable);
    if (v.end > end) map_view(end, v.offset + static_cast<uint32_t>(end - from), v.end - end, v.writable);
  }
}

void Memory::map(uint32_t virtual_address, uint32_t physical_address, uint32_t size, bool writable) {
  for (uint32_t offset = 0; offset < size; offset += 0x1000) {
    uint8_t* page = physical(physical_address + offset, 0x1000);
    const uint32_t index = (virtual_address + offset) >> 12;
    reads_[index] = page;
    writes_[index] = writable ? page : nullptr;
    physical_[index] = page ? physical_address + offset : 0;
  }
  // the views: one per run of pages contiguous in the section
  std::lock_guard<std::mutex> lock(views_mutex_);
  const uint64_t pages = (uint64_t{size} + 0xFFF) & ~uint64_t{0xFFF};
  unmap_views(virtual_address, virtual_address + pages);
  for (uint64_t offset = 0; offset < pages;) {
    const int64_t at = section_offset(static_cast<uint32_t>(physical_address + offset));
    uint64_t run = 0x1000;
    if (at >= 0) {
      while (offset + run < pages && section_offset(static_cast<uint32_t>(physical_address + offset + run)) == at + int64_t(run))
        run += 0x1000;
      map_view(virtual_address + offset, static_cast<uint32_t>(at), run, writable);
    }
    offset += run;
  }
}

void Memory::unmap(uint32_t virtual_address, uint32_t size) {
  for (uint32_t offset = 0; offset < size; offset += 0x1000) {
    const uint32_t index = (virtual_address + offset) >> 12;
    reads_[index] = writes_[index] = nullptr;
    physical_[index] = 0;
  }
  std::lock_guard<std::mutex> lock(views_mutex_);
  unmap_views(virtual_address, virtual_address + ((uint64_t{size} + 0xFFF) & ~uint64_t{0xFFF}));
}

uint32_t Memory::to_physical(uint32_t virtual_address) const {
  const uint32_t page = physical_[virtual_address >> 12];
  return page ? page + (virtual_address & 0xFFF) : 0;
}

uint32_t Memory::read32(uint32_t address) const {
  uint32_t value = 0;
  read(address, &value, 4);
  return value;
}

void Memory::write32(uint32_t address, uint32_t value) { write(address, &value, 4); }

void Memory::read(uint32_t address, void* to, uint32_t size) const {
  auto* out = static_cast<uint8_t*>(to);
  for (uint32_t i = 0; i < size; ++i) {
    const uint8_t* page = reads_[(address + i) >> 12];
    out[i] = page ? page[(address + i) & 0xFFF] : 0;
  }
}

void Memory::write(uint32_t address, const void* from, uint32_t size) {
  const auto* in = static_cast<const uint8_t*>(from);
  for (uint32_t i = 0; i < size; ++i)
    if (uint8_t* page = writes_[(address + i) >> 12]) page[(address + i) & 0xFFF] = in[i];
}

bool Memory::write_any(uint32_t address, const void* from, uint32_t size) {
  const auto* in = static_cast<const uint8_t*>(from);
  bool all = true;
  for (uint32_t i = 0; i < size; ++i) {
    uint8_t* page = reads_[(address + i) >> 12]; // the same bytes, whatever the page's permission
    if (page) page[(address + i) & 0xFFF] = in[i];
    else all = false;
  }
  return all;
}

} // namespace host
