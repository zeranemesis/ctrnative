#include "kernel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace host {

namespace {

constexpr uint32_t kExitMagic = 0xFFFFFFF0; // lr of a new thread: returning there ends it
constexpr uint32_t kTlsBase = 0x1FF82000, kTlsStride = 0x200;
constexpr int32_t kBudget = 1 << 20;        // instructions between looks at the scheduler
// Zakuro's: Sleep(0) with others ready waits this long (lower priorities run),
// and so does each service request; a thread running this long without
// blocking lets the others of its priority or lower run every other pass
constexpr auto kYield = std::chrono::nanoseconds(74595);   // 20000 ticks
constexpr auto kRequest = std::chrono::nanoseconds(39000); // 10456 ticks
constexpr auto kStarving = std::chrono::milliseconds(67);  // 4 frames

// a host thread with a stack as deep as the recompiled code may go
void start_host_thread(void (*body)(void*), void* argument) {
  constexpr size_t kStack = 16u << 20;
#if defined(_WIN32)
  struct Start {
    void (*body)(void*);
    void* argument;
  };
  auto* start = new Start{body, argument};
  const uintptr_t handle = _beginthreadex(
      nullptr, kStack,
      [](void* p) -> unsigned {
        auto* s = static_cast<Start*>(p);
        s->body(s->argument);
        delete s;
        return 0;
      },
      start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (handle) CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, kStack);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  struct Start {
    void (*body)(void*);
    void* argument;
  };
  auto* start = new Start{body, argument};
  pthread_create(
      &thread, &attributes,
      [](void* p) -> void* {
        auto* s = static_cast<Start*>(p);
        s->body(s->argument);
        delete s;
        return nullptr;
      },
      start);
  pthread_attr_destroy(&attributes);
#endif
}

Clock::time_point after_ns(uint64_t ns) {
  const auto now = Clock::now();
  if (ns > uint64_t(365) * 24 * 3600 * 1000000000ull) return Clock::time_point::max();
  return now + std::chrono::nanoseconds(ns);
}

} // namespace

// the recompiled code's view of the host: code lookup, svcs (memory is Memory::fastmem)
struct Callbacks {
  static Kernel& kernel(Context* ctx);
  static void interpret(Context* ctx, uint32_t address, uint32_t opcode) {
    char why[96];
    std::snprintf(why, sizeof why, "instruction %08x at %08x is not recompiled (no interpreter)", opcode, address);
    kernel(ctx).stop(why);
    ctx->exit = EXIT_UNWIND;
  }
  static Code lookup(Context* ctx, uint32_t address) { return kernel(ctx).module_.lookup(address); }
  static void svc(Context* ctx, uint32_t number) { kernel(ctx).svc(*static_cast<Thread*>(ctx->user), number); }
};

namespace {
Kernel* the_kernel = nullptr; // one process
}
Kernel& Callbacks::kernel(Context*) { return *the_kernel; }

Kernel::Kernel(Memory& memory, const Module& module) : memory_(memory), module_(module) {
  the_kernel = this;
  host_ = Host{Callbacks::interpret, Callbacks::lookup, Callbacks::svc};
}

uint64_t Kernel::ticks() const {
  const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
  return ns / 1000000000ull * kTicksPerSecond + ns % 1000000000ull * kTicksPerSecond / 1000000000ull;
}

uint32_t Kernel::allocate(int index, uint32_t size, bool top) {
  Region& region = regions_[index];
  const uint32_t pages = (size + 0xFFF) / 0x1000, total = static_cast<uint32_t>(region.used.size());
  if (pages == 0) return Memory::kFcram;
  if (pages > total) return 0;
  // the console's allocators want physically contiguous runs
  for (uint32_t i = 0; i + pages <= total; ++i) {
    const uint32_t first = top ? total - pages - i : i;
    bool free = true;
    for (uint32_t p = 0; p < pages && free; ++p) free = !region.used[first + p];
    if (!free) continue;
    for (uint32_t p = 0; p < pages; ++p) region.used[first + p] = true;
    return region.base + first * 0x1000;
  }
  return 0;
}

void Kernel::release(uint32_t physical, uint32_t size) {
  for (auto& region : regions_)
    for (uint32_t a = physical; a < physical + size; a += 0x1000)
      if (a >= region.base && a < region.base + region.size) region.used[(a - region.base) / 0x1000] = false;
}

void Kernel::load(const Image& image) {
  // FCRAM: the application's, the system's (up to 32 MiB), the base region
  const uint32_t app = image.app_memory, system = std::min(Memory::kFcramSize - app, 32u << 20);
  regions_[kApplication] = {Memory::kFcram, app, std::vector<bool>(app / 0x1000)};
  regions_[kSystem] = {Memory::kFcram + app, system, std::vector<bool>(system / 0x1000)};
  const uint32_t base = Memory::kFcramSize - app - system;
  regions_[kBase] = {Memory::kFcram + app + system, base, std::vector<bool>(base / 0x1000)};

  // the code, each segment from the top of the application's region, .text highest
  uint32_t offset = 0;
  auto segment = [&](uint32_t address, uint32_t pages, bool writable) {
    const uint32_t bytes = pages * 0x1000, physical = allocate(kApplication, bytes, true);
    uint8_t* to = memory_.physical(physical, bytes);
    std::memset(to, 0, bytes);
    std::memcpy(to, image.code + offset, std::min(bytes, image.code_size - std::min(offset, image.code_size)));
    memory_.map(address, physical, bytes, writable);
    offset += bytes;
  };
  segment(image.text_address, image.text_pages, false);
  segment(image.ro_address, image.ro_pages, false);
  segment(image.data_address, image.data_pages, true);
  const uint32_t bss_start = image.data_address + image.data_pages * 0x1000;
  const uint32_t bss_end = image.data_address + image.data_size + image.bss_size;
  if (bss_end > bss_start) {
    const uint32_t bytes = (bss_end - bss_start + 0xFFF) & ~0xFFFu, physical = allocate(kApplication, bytes, true);
    std::memset(memory_.physical(physical, bytes), 0, bytes);
    memory_.map(bss_start, physical, bytes, true);
  }
  // the code's extent, as QueryMemory tells it
  code_ = {image.text_address, image.text_address + image.text_pages * 0x1000, image.ro_address + image.ro_pages * 0x1000,
           std::max(bss_start, (bss_end + 0xFFF) & ~0xFFFu)};
  // the main thread's stack, ending at 0x10000000
  const uint32_t stack =(std::max(image.stack_size, 0x4000u) + 0xFFF) & ~0xFFFu;
  const uint32_t stack_physical = allocate(kApplication, stack, true);
  std::memset(memory_.physical(stack_physical, stack), 0, stack);
  memory_.map(0x10000000 - stack, stack_physical, stack, true);

  // the GPU's and the DSP's memory, the configuration and shared pages
  memory_.map(0x1F000000, Memory::kVram, Memory::kVramSize, true);
  memory_.map(0x1FF00000, Memory::kDspRam, Memory::kDspRamSize, true);
  memory_.map(0x1FF80000, Memory::kAxi, 0x1000, false);
  memory_.map(0x1FF81000, Memory::kAxi + 0x1000, 0x1000, false);
  uint8_t* config = memory_.physical(Memory::kAxi, 0x2000);
  std::memset(config, 0, 0x2000);
  auto put32 = [&](uint32_t at, uint32_t value) { std::memcpy(config + at, &value, 4); };
  config[0x01] = 0x34, config[0x02] = 0x02; // kernel 2.52.0, as Zakuro reports it
  put32(0x10, 2);
  config[0x14] = 1, config[0x15] = 1; // retail unit
  put32(0x18, 0xF297);
  put32(0x40, app), put32(0x44, system), put32(0x48, base);
  config[0x61] = 0x34, config[0x62] = 0x02;
  put32(0x64, 2), put32(0x68, 0xF297);
  uint8_t* shared = config + 0x1000;
  shared[0x04] = 1, shared[0x05] = 1; // running hardware, old 3DS
  const uint8_t mac[6] = {0x40, 0xF4, 0x07, 0x00, 0x00, 0x01};
  std::memcpy(shared + 0x60, mac, 6);
  shared[0x66] = 3, shared[0x67] = 2;
  shared[0x85] = 0x15; // battery
  // shortcut: the date and time slots (0x20, 0x40) stay 0; fill them from the host clock when a game reads them

  create_thread(image.text_address, 0x10000000, 0, image.priority); // the main thread: no handle, as Zakuro
}

std::shared_ptr<Thread> Kernel::create_thread(uint32_t entry, uint32_t stack, uint32_t argument, int priority) {
  auto thread = std::make_shared<Thread>();
  thread->priority = std::clamp(priority, 0, 0x3F);
  thread->id = next_thread_id_++;
  thread->index = static_cast<uint32_t>(threads_.size());
  Context& c = thread->ctx;
  c.r[0] = argument;
  c.r[13] = stack;
  c.r[14] = kExitMagic;
  c.r[15] = entry & ~1u;
  c.thumb = entry & 1;
  c.vfp = thread->vfp;
  c.fpscr = &thread->fpscr;
  c.host = &host_;
  c.user = thread.get();
  c.fastmem = memory_.fastmem();
  map_tls(*thread);
  threads_.push_back(thread);
  start_host_thread([](void* p) { the_kernel->thread_main(static_cast<Thread*>(p)); }, thread.get());
  return thread;
}

void Kernel::map_tls(Thread& thread) {
  // the lowest block a dead thread left, or the next one
  if (!free_tls_.empty()) {
    std::sort(free_tls_.begin(), free_tls_.end());
    thread.tls_slot = free_tls_.front();
    free_tls_.erase(free_tls_.begin());
  } else {
    thread.tls_slot = next_tls_++;
  }
  const uint32_t address = kTlsBase + thread.tls_slot * kTlsStride;
  if (!memory_.host(address & ~0xFFFu)) {
    const uint32_t physical = allocate(kBase, 0x1000, false);
    std::memset(memory_.physical(physical, 0x1000), 0, 0x1000);
    memory_.map(address & ~0xFFFu, physical, 0x1000, true);
  }
  std::memset(memory_.host(address), 0, kTlsStride);
  thread.ctx.tls = address;
}

uint32_t Kernel::add_handle(std::shared_ptr<Object> object) {
  const uint32_t handle = next_handle_++;
  handles_[handle] = std::move(object);
  return handle;
}

std::shared_ptr<Object> Kernel::resolve(uint32_t handle) {
  if (handle == 0xFFFF8000) return running_ ? threads_[running_->index] : nullptr;
  if (handle == 0xFFFF8001) return process_;
  const auto found = handles_.find(handle);
  return found == handles_.end() ? nullptr : found->second;
}

void Kernel::stop(const std::string& why) {
  // with or without the lock: only flags here
  if (!stopped_.exchange(true)) why_ = why;
  finished_.notify_all();
  wake_timekeeper_.notify_all();
}

void Kernel::native_call(Thread& self, const std::function<void()>& work) {
  auto lock = this->lock();
  work();
  if (std::any_of(threads_.begin(), threads_.end(),
                  [&](auto& t) { return t.get() != &self && t->state == Thread::State::Ready; })) {
    self.wake_at = Clock::now() + kRequest;
    self.state = Thread::State::Sleeping;
    block(self, lock);
  }
}

void Kernel::report() const {
  std::vector<std::pair<uint64_t, std::string>> lines;
  for (auto& [site, count] : svc_sites_) {
    char line[64];
    std::snprintf(line, sizeof line, "svc %02x from %08x", site.first, site.second);
    lines.emplace_back(count, line);
  }
  for (auto& [key, count] : requests_) lines.emplace_back(count, "request " + key);
  std::sort(lines.rbegin(), lines.rend());
  uint64_t svcs = 0, requests = 0;
  for (auto& [site, count] : svc_sites_) svcs += count;
  for (auto& [key, count] : requests_) requests += count;
  std::printf("[host] the game called the 3DS system %llu times (of which %llu service requests):\n",
              (unsigned long long)svcs, (unsigned long long)requests);
  for (auto& [count, line] : lines) std::printf("  %10llu  %s\n", (unsigned long long)count, line.c_str());
  // who waits on what at the end: a run that stops drawing is usually a thread waiting forever
  static const char* states[] = {"ready", "running", "sleeping", "wait-sync", "wait-arbiter", "dead"};
  for (const auto& t : threads_) {
    if (t->state == Thread::State::Dead) continue;
    std::printf("[host] thread %u prio %d %s, last dispatch %08x lr %08x", t->id, t->priority,
                states[static_cast<int>(t->state)], t->last_pc, t->ctx.r[14]);
    for (const auto& o : t->waiting) {
      if (o->kind == Object::Kind::Session || o->kind == Object::Kind::Port)
        std::printf(" [%s]", static_cast<const Session&>(*o).name.c_str());
      else
        for (const auto& [handle, object] : handles_)
          if (object == o) std::printf(" [handle %x kind %d]", handle, static_cast<int>(o->kind));
    }
    std::printf("\n");
  }
}

// ---- scheduling

bool Kernel::signaled(const Object& object, const Thread& waiter) const {
  switch (object.kind) {
  case Object::Kind::Event: return static_cast<const Event&>(object).signaled;
  case Object::Kind::Mutex: {
    const auto& m = static_cast<const Mutex&>(object);
    return !m.owner || m.owner == &waiter;
  }
  case Object::Kind::Semaphore: return static_cast<const Semaphore&>(object).count > 0;
  case Object::Kind::Thread: return static_cast<const Thread&>(object).state == Thread::State::Dead;
  default: return true; // sessions, ports, shared memory, the process, limits: as Zakuro has them
  }
}

void Kernel::acquire(Object& object, Thread& waiter) {
  switch (object.kind) {
  case Object::Kind::Event: {
    auto& e = static_cast<Event&>(object);
    if (e.reset == 0) e.signaled = false; // one-shot: the first waiter takes it
    break;
  }
  case Object::Kind::Mutex: {
    auto& m = static_cast<Mutex&>(object);
    m.owner = &waiter;
    ++m.count;
    break;
  }
  case Object::Kind::Semaphore: --static_cast<Semaphore&>(object).count; break;
  default: break;
  }
}

void Kernel::pass() {
  const auto now = Clock::now();
  // in thread-table order, as Zakuro: it decides who gets a contended object
  for (auto& t : threads_) {
    switch (t->state) {
    case Thread::State::Sleeping:
      if (now >= t->wake_at) t->state = Thread::State::Ready, t->result0 = kOk;
      break;
    case Thread::State::WaitSync: {
      bool done = false;
      if (t->wait_all) {
        done = std::all_of(t->waiting.begin(), t->waiting.end(), [&](auto& o) { return signaled(*o, *t); });
        if (done) {
          for (auto& o : t->waiting) acquire(*o, *t);
          t->result1 = 0;
        }
      } else {
        for (size_t i = 0; i < t->waiting.size() && !done; ++i)
          if (signaled(*t->waiting[i], *t)) {
            acquire(*t->waiting[i], *t);
            t->result1 = static_cast<uint32_t>(i);
            done = true;
          }
      }
      if (done) t->result0 = kOk;
      else if (now >= t->wake_at) done = true, t->result0 = kTimeout;
      if (done) {
        t->waiting.clear();
        t->state = Thread::State::Ready;
      }
      break;
    }
    case Thread::State::WaitArbiter:
      if (now >= t->wake_at) t->state = Thread::State::Ready, t->result0 = kTimeout;
      break;
    default: break;
    }
  }
  for (Event* e : pulsed_) e->signaled = false;
  pulsed_.clear();
}

Thread* Kernel::pick() {
  int best = 0x40;
  for (auto& t : threads_)
    if (t->state == Thread::State::Ready || t->state == Thread::State::Running) best = std::min(best, t->priority);
  if (best == 0x40) return nullptr;
  // the running thread keeps the core against equals; only higher priority preempts
  if (running_ && running_->state == Thread::State::Running && running_->priority <= best) return running_;
  const size_t n = threads_.size(), start = running_ ? running_->index + 1 : 0;
  for (size_t i = 0; i < n; ++i) {
    Thread* t = threads_[(start + i) % n].get();
    if ((t->state == Thread::State::Ready || t->state == Thread::State::Running) && t->priority == best) return t;
  }
  return nullptr;
}

void Kernel::hand_to(Thread* next) {
  running_ = next;
  preempt_ = false;
  // the time no game thread can run: the game waits for the console, not for the host's CPU
  if (!next && idle_since_ == Clock::time_point{}) idle_since_ = Clock::now();
  if (next && idle_since_ != Clock::time_point{}) idle_ += Clock::now() - idle_since_, idle_since_ = {};
  if (next) {
    next->state = Thread::State::Running;
    next->baton.notify_one();
  } else {
    wake_timekeeper_.notify_one(); // nobody can run: time will wake someone
  }
}

void Kernel::reschedule(Thread& self, std::unique_lock<std::mutex>& lock) {
  pass();
  Thread* next = pick();
  if (next == &self) {
    self.state = Thread::State::Running;
    return;
  }
  if (self.state == Thread::State::Running) self.state = Thread::State::Ready;
  hand_to(next);
  if (self.state == Thread::State::Dead) return;
  self.baton.wait(lock, [&] { return running_ == &self || stopped_; });
  self.ctx.exclusive = 0; // another thread ran: the exclusive monitor is lost
  self.running_since = Clock::now();
}

void Kernel::block(Thread& self, std::unique_lock<std::mutex>& lock) {
  wake_timekeeper_.notify_one(); // a new deadline, maybe
  reschedule(self, lock);
}

void Kernel::timekeeper() {
  auto lock = this->lock();
  while (!stopped_) {
    pass();
    if (!running_) {
      hand_to(pick());
    } else {
      // a higher priority woke (a timeout, a host signal): the running thread
      // gives way at its next budget check; so does one starving the others
      Thread* next = pick();
      const bool starving = next == running_ && Clock::now() - running_->running_since > kStarving &&
                            std::any_of(threads_.begin(), threads_.end(), [&](auto& t) {
                              return t.get() != running_ && t->state == Thread::State::Ready;
                            });
      if (next != running_ || starving) preempt_ = true;
    }
    auto deadline = Clock::now() + std::chrono::milliseconds(2); // the starvation check's period
    bool timed = false; // a game thread's own deadline, not the check's
    for (auto& t : threads_)
      if ((t->state == Thread::State::Sleeping || t->state == Thread::State::WaitSync ||
           t->state == Thread::State::WaitArbiter) && t->wake_at <= deadline)
        deadline = t->wake_at, timed = true;
    // the host's waits wake a millisecond late or more (Windows' timer): a game's short sleeps and the
    // pauses after its requests (tens of microseconds) would take that long each, its loading crawl;
    // so the last millisecond before a game's deadline is waited by spinning, the lock let go
    constexpr auto kSpin = std::chrono::milliseconds(1);
    if (timed && deadline - Clock::now() <= kSpin) {
      lock.unlock();
      while (Clock::now() < deadline && !stopped_) std::this_thread::yield();
      lock.lock();
    } else {
      wake_timekeeper_.wait_until(lock, timed ? deadline - kSpin : deadline);
    }
  }
}

void Kernel::thread_main(Thread* self) {
  {
    auto lock = this->lock();
    self->baton.wait(lock, [&] { return running_ == self || stopped_; });
    self->running_since = Clock::now();
  }
  Context& c = self->ctx;
  while (!stopped_ && self->state != Thread::State::Dead) {
    const uint32_t pc = c.r[15] | (c.thumb ? 1u : 0u);
    if (c.r[15] == kExitMagic) { // returned from its entry: as ExitThread
      auto lock = this->lock();
      end_thread(*self);
      break;
    }
    Code code = module_.lookup(pc);
    if (!code) {
      // where it came from: the last dispatch of this thread, the registers, code-like words on its stack
      fprintf(stderr, "[kernel] bad pc %08x, last dispatch %08x, lr %08x sp %08x r0-r3 %08x %08x %08x %08x prio %d, stack:",
              pc, self->last_pc, c.r[14], c.r[13], c.r[0], c.r[1], c.r[2], c.r[3], self->priority);
      for (uint32_t a = c.r[13], n = 0; n < 16 && a < c.r[13] + 0x400; a += 4) {
        uint8_t* page = memory_.read_pages()[a >> 12];
        if (!page) break;
        uint32_t w;
        memcpy(&w, page + (a & 0xFFF), 4);
        if (w >= 0x100000 && w < 0x3E0000) fprintf(stderr, " %08x", w), n++;
      }
      fprintf(stderr, "\n");
      // a virtual call through a null entry: r0 the object, its first word the table
      const uint32_t table = memory_.read32(c.r[0]);
      fprintf(stderr, "[kernel] r0 points at %08x %08x %08x %08x; table %08x: %08x %08x %08x %08x\n",
              memory_.read32(c.r[0]), memory_.read32(c.r[0] + 4), memory_.read32(c.r[0] + 8), memory_.read32(c.r[0] + 12),
              table, memory_.read32(table), memory_.read32(table + 4), memory_.read32(table + 8), memory_.read32(table + 12));
      char why[64];
      std::snprintf(why, sizeof why, "no recompiled code at %08x", pc);
      stop(why);
      break;
    }
    c.exit = EXIT_NONE;
    c.budget = kBudget;
    self->last_pc = pc;
    code(&c);
    if (c.exit == EXIT_BUDGET && preempt_) {
      auto lock = this->lock();
      give_way(*self, lock);
    }
  }
}

// a budget exit the timekeeper asked for (preempt_): also reached from inside
// decompiled code, through svc 0xFF (Port/bridge/bridge.cpp), as each game thread has its own stack
void Kernel::give_way(Thread& self, std::unique_lock<std::mutex>& lock) {
  if (!preempt_) return;
  // starvation relief: give the core to the most urgent other ready thread
  Thread* next = pick();
  if (next != &self) return reschedule(self, lock);
  Thread* other = nullptr;
  for (auto& t : threads_)
    if (t.get() != &self && t->state == Thread::State::Ready && (!other || t->priority < other->priority))
      other = t.get();
  if (!other) {
    preempt_ = false;
    return;
  }
  self.state = Thread::State::Ready;
  hand_to(other);
  self.baton.wait(lock, [&] { return running_ == &self || stopped_; });
  self.ctx.exclusive = 0;
  self.running_since = Clock::now();
}

void Kernel::end_thread(Thread& thread) {
  thread.state = Thread::State::Dead;
  thread.waiting.clear();
  free_tls_.push_back(thread.tls_slot);
  // what it held goes free (Zakuro force-releases its mutexes)
  for (auto& [handle, object] : handles_)
    if (object->kind == Object::Kind::Mutex) {
      auto& m = static_cast<Mutex&>(*object);
      if (m.owner == &thread) m.owner = nullptr, m.count = 0;
    }
  if (running_ == &thread) {
    pass();
    hand_to(pick());
  }
  if (std::all_of(threads_.begin(), threads_.end(), [](auto& t) { return t->state == Thread::State::Dead; }))
    stop("every thread has ended");
}

void Kernel::signal(Event& event) {
  if (event.reset == 2) { // pulse: only those waiting now see it
    const bool waited = std::any_of(threads_.begin(), threads_.end(), [&](auto& t) {
      return t->state == Thread::State::WaitSync &&
             std::any_of(t->waiting.begin(), t->waiting.end(), [&](auto& o) { return o.get() == &event; });
    });
    if (!waited) return;
    pulsed_.push_back(&event);
  }
  event.signaled = true;
  wake_timekeeper_.notify_one();
}

void Kernel::start() {
  keeper_ = std::thread([this] { timekeeper(); });
  auto lock = this->lock();
  pass();
  hand_to(pick());
}

void Kernel::wake() { wake_timekeeper_.notify_one(); }

std::string Kernel::wait() {
  {
    auto lock = this->lock();
    finished_.wait(lock, [&] { return stopped_.load(); });
    for (auto& t : threads_) t->baton.notify_all(); // let the game threads see the stop
  }
  wake_timekeeper_.notify_all();
  if (keeper_.joinable()) keeper_.join();
  return why_;
}

// ---- the system calls

void Kernel::svc(Thread& self, uint32_t number) {
  Context& c = self.ctx;
  uint32_t* r = c.r;
  auto lock = this->lock();
  ++svc_sites_[{number, r[14]}];
  auto signed64 = [](uint32_t low, uint32_t high) { return static_cast<int64_t>(uint64_t{high} << 32 | low); };
  // a state change: whoever should run now runs (only higher priority preempts)
  auto yield = [&] { reschedule(self, lock); };

  switch (number) {
  case 0x01: { // ControlMemory(op, addr0, addr1, size, perm) -> address
    const uint32_t op = r[0] & 0xFF, addr0 = r[1], addr1 = r[2], size = r[3];
    const bool linear = r[0] & 0x10000;
    if (size & 0xFFF) {
      r[0] = kMisalignedSize;
      break;
    }
    if (op == 3 && linear) {
      const uint32_t physical = allocate(kApplication, size, false);
      if (!physical) {
        r[0] = kOutOfMemory;
        break;
      }
      const uint32_t address = linear_base_ + (physical - Memory::kFcram);
      memory_.map(address, physical, size, true);
      r[0] = kOk, r[1] = address;
    } else if (op == 3) {
      // the heap: physically contiguous from the top, not zeroed (as Zakuro)
      const uint32_t base = addr0 ? addr0 : heap_top_, physical = allocate(kApplication, size, true);
      if (!physical) {
        r[0] = kOutOfMemory;
        break;
      }
      memory_.map(base, physical, size, true);
      heap_top_ = std::max(heap_top_, base + size);
      r[0] = kOk, r[1] = base;
    } else if (op == 1) { // free
      for (uint32_t a = addr0; a < addr0 + size; a += 0x1000) {
        const uint32_t physical = memory_.to_physical(a);
        for (auto& region : regions_)
          if (physical >= region.base && physical < region.base + region.size)
            region.used[(physical - region.base) / 0x1000] = false;
      }
      memory_.unmap(addr0, size);
      r[0] = kOk, r[1] = addr0;
    } else if (op == 4) { // map: alias addr1 at addr0
      const uint32_t physical = memory_.to_physical(addr1);
      if (!physical) {
        r[0] = kInvalidAddress;
        break;
      }
      memory_.map(addr0, physical, size, true);
      r[0] = kOk, r[1] = addr0;
    } else if (op == 5) {
      memory_.unmap(addr0, size);
      r[0] = kOk, r[1] = addr0;
    } else if (op == 6 || op == 2) {
      r[0] = kOk;
    } else {
      r[0] = kInvalidEnum;
    }
    break;
  }
  case 0x02: { // QueryMemory(_, _, address) -> base, size, permission, state, page flags
    // what the console says of the block of pages around address that share a kind: the game's code
    // (text RX, read-only data R, data and bss RW, all "Code"), the linear heap ("Continuous", 7), the
    // main stack ("Locked", 11), other mapped memory ("Private", 5), nothing mapped ("Free", 0)
    auto kind = [&](uint32_t page) -> uint32_t { // permission | state << 8
      const uint32_t a = page << 12;
      if (!memory_.read_pages()[page]) return 0;
      if (a >= code_.text && a < code_.end) return (a < code_.text_end ? 5 : a < code_.ro_end ? 1 : 3) | 4 << 8;
      const uint32_t permission = memory_.write_pages()[page] ? 3 : 1;
      if (a >= linear_base_ && a < linear_base_ + Memory::kFcramSize) return permission | 7 << 8;
      if (a >= 0x0E000000 && a < 0x10000000) return permission | 11 << 8;
      return permission | 5 << 8;
    };
    const uint32_t page = r[2] >> 12, at = kind(page);
    uint32_t first = page, last = page;
    while (first > 0 && kind(first - 1) == at) --first;
    while (last < 0xFFFFF && kind(last + 1) == at) ++last;
    r[0] = kOk, r[1] = first << 12, r[2] = (last - first + 1) << 12, r[3] = at & 0xFF, r[4] = at >> 8, r[5] = 0;
    break;
  }
  case 0x03: stop("the game exited"); break;
  case 0x08: { // CreateThread(priority, entry, argument, stack, processor) -> handle
    // shortcut: every thread on the application core, the processor id ignored (as Zakuro)
    if (std::getenv("CTRN_LOG_THREADS"))
      fprintf(stderr, "[kernel] thread entry %08x prio %d arg %08x stack top %08x, created from %08x by a prio %d thread\n",
              r[1], int(r[0]), r[2], r[3], r[14], self.priority);
    auto thread = create_thread(r[1], r[3], r[2], static_cast<int>(r[0]));
    r[0] = kOk, r[1] = add_handle(thread);
    yield();
    break;
  }
  case 0x09: // ExitThread
    end_thread(self);
    c.exit = EXIT_UNWIND;
    return;
  case 0x0A: { // SleepThread(ns as u64)
    const uint64_t ns = uint64_t{r[1]} << 32 | r[0];
    r[0] = kOk;
    if (ns == 0) {
      const bool others = std::any_of(threads_.begin(), threads_.end(), [&](auto& t) {
        return t.get() != &self && (t->state == Thread::State::Ready || t->state == Thread::State::Running);
      });
      if (!others) break;
      self.wake_at = Clock::now() + kYield;
    } else {
      self.wake_at = after_ns(ns);
    }
    self.state = Thread::State::Sleeping;
    block(self, lock);
    break;
  }
  case 0x0B: { // GetThreadPriority(_, handle)
    auto o = resolve(r[1]);
    if (!o || o->kind != Object::Kind::Thread) {
      r[0] = kInvalidHandle;
      break;
    }
    r[0] = kOk, r[1] = static_cast<uint32_t>(static_cast<Thread&>(*o).priority);
    break;
  }
  case 0x0C: { // SetThreadPriority(handle, priority)
    auto o = resolve(r[0]);
    if (!o || o->kind != Object::Kind::Thread) {
      r[0] = kInvalidHandle;
      break;
    }
    static_cast<Thread&>(*o).priority = std::clamp(static_cast<int>(r[1]), 0, 0x3F);
    r[0] = kOk;
    yield();
    break;
  }
  case 0x13: { // CreateMutex(_, locked) -> handle
    auto m = std::make_shared<Mutex>();
    if (r[1]) m->owner = &self, m->count = 1;
    r[0] = kOk, r[1] = add_handle(m);
    break;
  }
  case 0x14: { // ReleaseMutex(handle); no owner check, as Zakuro
    auto o = resolve(r[0]);
    if (!o || o->kind != Object::Kind::Mutex) {
      r[0] = kInvalidHandle;
      break;
    }
    auto& m = static_cast<Mutex&>(*o);
    if (m.count) --m.count;
    if (!m.count) m.owner = nullptr;
    r[0] = kOk;
    yield();
    break;
  }
  case 0x15: { // CreateSemaphore(_, initial, max) -> handle
    auto s = std::make_shared<Semaphore>();
    s->count = static_cast<int32_t>(r[1]), s->max = static_cast<int32_t>(r[2]);
    r[0] = kOk, r[1] = add_handle(s);
    break;
  }
  case 0x16: { // ReleaseSemaphore(_, handle, count) -> previous
    auto o = resolve(r[1]);
    if (!o || o->kind != Object::Kind::Semaphore) {
      r[0] = kInvalidHandle;
      break;
    }
    auto& s = static_cast<Semaphore&>(*o);
    const int32_t previous = s.count;
    s.count = std::min(s.max, s.count + static_cast<int32_t>(r[2]));
    r[0] = kOk, r[1] = static_cast<uint32_t>(previous);
    yield();
    break;
  }
  case 0x17: { // CreateEvent(_, reset) -> handle
    auto e = std::make_shared<Event>();
    e->reset = r[1] == 1 ? 1 : r[1] == 2 ? 2 : 0;
    r[0] = kOk, r[1] = add_handle(e);
    break;
  }
  case 0x18: { // SignalEvent(handle)
    auto o = resolve(r[0]);
    if (!o) {
      r[0] = kInvalidHandle;
      break;
    }
    if (o->kind == Object::Kind::Event) signal(static_cast<Event&>(*o));
    r[0] = kOk;
    yield();
    break;
  }
  case 0x19: { // ClearEvent(handle)
    auto o = resolve(r[0]);
    if (!o) {
      r[0] = kInvalidHandle;
      break;
    }
    if (o->kind == Object::Kind::Event) static_cast<Event&>(*o).signaled = false;
    r[0] = kOk;
    break;
  }
  case 0x1E: { // CreateMemoryBlock(other perm, addr, size, own perm) -> handle
    auto block = std::make_shared<SharedMemory>();
    block->size = r[2];
    if (r[1] == 0) {
      block->physical = allocate(kBase, r[2], false);
      if (!block->physical) {
        r[0] = kOutOfMemory;
        break;
      }
      std::memset(memory_.physical(block->physical, r[2]), 0, r[2]);
    } else {
      block->physical = memory_.to_physical(r[1]);
      block->own_address = r[1];
      if (!block->physical) {
        r[0] = kInvalidAddress;
        break;
      }
    }
    r[0] = kOk, r[1] = add_handle(block);
    break;
  }
  case 0x1F: { // MapMemoryBlock(handle, addr, perm)
    auto o = resolve(r[0]);
    if (!o || o->kind != Object::Kind::SharedMemory) {
      r[0] = kInvalidHandle;
      break;
    }
    auto& block = static_cast<SharedMemory&>(*o);
    uint32_t address = r[1] ? r[1] : block.own_address;
    if (!address) // the first free fit from 0x10000000
      for (uint32_t a = 0x10000000; a < 0x14000000 && !address; a += 0x1000) {
        bool free = true;
        for (uint32_t p = 0; p < block.size && free; p += 0x1000) free = !memory_.host(a + p);
        if (free) address = a;
      }
    memory_.map(address, block.physical, (block.size + 0xFFF) & ~0xFFFu, true);
    block.mapped_at = address;
    r[0] = kOk;
    break;
  }
  case 0x20: { // UnmapMemoryBlock(handle, addr): always succeeds, as Zakuro
    auto o = resolve(r[0]);
    if (o && o->kind == Object::Kind::SharedMemory)
      memory_.unmap(r[1], (static_cast<SharedMemory&>(*o).size + 0xFFF) & ~0xFFFu);
    r[0] = kOk;
    break;
  }
  case 0x21: // CreateAddressArbiter -> handle
    r[0] = kOk, r[1] = add_handle(std::make_shared<Arbiter>());
    break;
  case 0x22: { // ArbitrateAddress(handle, address, type, value, timeout low, timeout high)
    const uint32_t address = r[1], type = r[2];
    const int32_t value = static_cast<int32_t>(r[3]);
    const int64_t timeout = signed64(r[4], r[5]);
    if (type > 4) {
      r[0] = kInvalidEnum;
      break;
    }
    auto o = resolve(r[0]);
    if (!o) {
      r[0] = kInvalidHandle;
      break;
    }
    auto* arbiter = o->kind == Object::Kind::Arbiter ? static_cast<Arbiter*>(o.get()) : nullptr;
    if (type == 0) { // signal: up to value waiters (all when negative), first come first woken
      int woken = 0;
      if (arbiter)
        for (auto it = arbiter->waiting.begin(); it != arbiter->waiting.end() && (value < 0 || woken < value);) {
          Thread* t = it->first;
          if (it->second == address && t->state == Thread::State::WaitArbiter) {
            t->state = Thread::State::Ready, t->result0 = kOk;
            ++woken;
          }
          if (it->second == address || t->state != Thread::State::WaitArbiter) it = arbiter->waiting.erase(it);
          else ++it;
        }
      r[0] = kOk;
      yield();
      break;
    }
    const int32_t current = static_cast<int32_t>(memory_.read32(address));
    if (current >= value) {
      r[0] = kOk;
      break;
    }
    if (type == 2 || type == 4) memory_.write32(address, static_cast<uint32_t>(current - 1));
    if (arbiter) arbiter->waiting.emplace_back(&self, address);
    self.wake_at = (type == 3 || type == 4) && timeout >= 0 ? after_ns(static_cast<uint64_t>(timeout))
                                                             : Clock::time_point::max();
    self.state = Thread::State::WaitArbiter;
    block(self, lock);
    r[0] = self.result0;
    break;
  }
  case 0x23: // CloseHandle: always succeeds, as Zakuro
    handles_.erase(r[0]);
    r[0] = kOk;
    break;
  case 0x24: // WaitSynchronization1(handle, _, timeout low, timeout high)
  case 0x25: { // WaitSynchronizationN(timeout low, handles, count, all, timeout high) -> index
    std::vector<std::shared_ptr<Object>> objects;
    int64_t timeout;
    bool all = false;
    if (number == 0x24) {
      auto o = resolve(r[0]);
      if (!o) {
        r[0] = kInvalidHandle;
        break;
      }
      objects.push_back(o);
      timeout = signed64(r[2], r[3]);
    } else {
      for (uint32_t i = 0; i < r[2]; ++i) {
        auto o = resolve(memory_.read32(r[1] + i * 4));
        if (!o) break;
        objects.push_back(o);
      }
      if (objects.size() != r[2]) {
        r[0] = kInvalidHandle;
        break;
      }
      all = r[3] != 0;
      timeout = signed64(r[0], r[4]);
    }
    // already satisfied: no wait
    if (all ? std::all_of(objects.begin(), objects.end(), [&](auto& o) { return signaled(*o, self); }) : false) {
      for (auto& o : objects) acquire(*o, self);
      r[0] = kOk, r[1] = 0;
      break;
    }
    if (!all) {
      bool done = false;
      for (size_t i = 0; i < objects.size() && !done; ++i)
        if (signaled(*objects[i], self)) {
          acquire(*objects[i], self);
          r[0] = kOk, r[1] = static_cast<uint32_t>(i);
          done = true;
        }
      if (done) break;
    }
    if (timeout == 0) {
      r[0] = kTimeout;
      break;
    }
    self.waiting = std::move(objects);
    self.wait_all = all;
    self.wake_at = timeout < 0 ? Clock::time_point::max() : after_ns(static_cast<uint64_t>(timeout));
    self.state = Thread::State::WaitSync;
    block(self, lock);
    r[0] = self.result0;
    if (number == 0x25 && self.result0 == kOk) r[1] = self.result1;
    break;
  }
  case 0x27: { // DuplicateHandle(_, handle) -> handle
    auto o = resolve(r[1]);
    if (!o) {
      r[0] = kInvalidHandle;
      break;
    }
    r[0] = kOk, r[1] = add_handle(o);
    break;
  }
  case 0x28: { // GetSystemTick -> ticks
    const uint64_t t = ticks();
    r[0] = static_cast<uint32_t>(t), r[1] = static_cast<uint32_t>(t >> 32);
    break;
  }
  case 0x2D: { // ConnectToPort(_, name) -> handle
    char name[13] = {};
    memory_.read(r[1], name, 12);
    r[0] = kOk, r[1] = add_handle(std::make_shared<Session>(Object::Kind::Port, name));
    break;
  }
  case 0x32: { // SendSyncRequest(handle): the command buffer at TLS + 0x80
    auto o = resolve(r[0]);
    if (!o || (o->kind != Object::Kind::Session && o->kind != Object::Kind::Port)) {
      r[0] = kInvalidHandle;
      break;
    }
    uint32_t* command = reinterpret_cast<uint32_t*>(memory_.host(c.tls + 0x80));
    const std::string& service = static_cast<Session&>(*o).name;
    {
      char key[64];
      std::snprintf(key, sizeof key, "%s %04x from %08x", service.c_str(), command[0] >> 16, r[14]);
      ++requests_[key];
      // CTRN_LOG_IPC=1: every request as it goes (what a game that stops was asking last)
      // (CTRN_LOG_IPC=all: GPU, DSP and file requests too)
      static const char* log_ipc = std::getenv("CTRN_LOG_IPC");
      if (log_ipc && (std::strcmp(log_ipc, "all") == 0 ||
                      (service != "gsp::Gpu" && service != "dsp::DSP" && service.rfind("FS", 0) != 0)))
        std::printf("[ipc] %s %08x %08x %08x %08x %08x (thread %u, lr %08x)\n", service.c_str(), command[0], command[1],
                    command[2], command[3], command[4], self.id, r[14]);
    }
    // the service reads the session's handle in r0 (a file session finds its file by it)
    if (!services_ || !services_(*this, self, service, command)) {
      char why[160];
      std::snprintf(why, sizeof why, "service %s: command %08x (%08x %08x %08x %08x) is not native yet", service.c_str(),
                    command[0], command[1], command[2], command[3], command[4]);
      stop(why);
      c.exit = EXIT_UNWIND;
      return;
    }
    r[0] = kOk;
    // as Zakuro: a request lets the others run a little while
    if (std::any_of(threads_.begin(), threads_.end(),
                    [&](auto& t) { return t.get() != &self && t->state == Thread::State::Ready; })) {
      self.wake_at = Clock::now() + kRequest;
      self.state = Thread::State::Sleeping;
      block(self, lock);
    }
    break;
  }
  case 0x2B: { // GetProcessInfo(_, process, type) -> s64
    uint64_t value;
    switch (r[2]) {
    case 0: case 2: { // the memory the process uses (2: its private memory, which is all of it here)
      uint64_t pages = 0;
      for (bool used : regions_[kApplication].used) pages += used;
      value = pages * 0x1000;
      break;
    }
    case 20: value = Memory::kFcram - linear_base_; break; // from a linear address to its physical one, as an offset
    case 21: value = linear_base_; break;                  // the linear heap's start
    case 22: value = linear_base_ + regions_[kApplication].size; break; // its end
    default: {
      char why[64];
      std::snprintf(why, sizeof why, "svc 2b (GetProcessInfo) type %u is not native yet", r[2]);
      stop(why);
      c.exit = EXIT_UNWIND;
      return;
    }
    }
    r[0] = kOk, r[1] = static_cast<uint32_t>(value), r[2] = static_cast<uint32_t>(value >> 32);
    break;
  }
  case 0x35: // GetProcessId -> 0x22
    r[0] = kOk, r[1] = 0x22;
    break;
  case 0x37: { // GetThreadId(_, handle) -> id
    auto o = resolve(r[1]);
    if (!o || o->kind != Object::Kind::Thread) {
      r[0] = kInvalidHandle;
      break;
    }
    r[0] = kOk, r[1] = static_cast<Thread&>(*o).id;
    break;
  }
  case 0x38: // GetResourceLimit -> handle
    r[0] = kOk, r[1] = add_handle(std::make_shared<Simple>(Object::Kind::ResourceLimit));
    break;
  case 0x39:   // GetResourceLimitLimitValues(values, handle, names, count)
  case 0x3A: { // GetResourceLimitCurrentValues
    for (uint32_t i = 0; i < r[3]; ++i) {
      const uint32_t name = memory_.read32(r[2] + i * 4);
      int64_t value = 0;
      if (number == 0x39) {
        static const int64_t limits[10] = {0x18, 0, 128, 128, 64, 32, 16, 32, 8, 0};
        value = name == 1 ? regions_[kApplication].size : name < 10 ? limits[name] : 0;
      } else if (name == 0) {
        value = 0x18;
      } else if (name == 1) {
        value = std::count(regions_[kApplication].used.begin(), regions_[kApplication].used.end(), true) * 0x1000ll;
      } else if (name == 2) {
        value = std::count_if(threads_.begin(), threads_.end(), [](auto& t) { return t->state != Thread::State::Dead; });
      }
      memory_.write(r[0] + i * 8, &value, 8);
    }
    r[0] = kOk;
    break;
  }
  case 0x3C: { // Break
    char why[64];
    std::snprintf(why, sizeof why, "the game broke (svcBreak %u)", r[0]);
    stop(why);
    c.exit = EXIT_UNWIND;
    return;
  }
  case 0xFF: // not a console svc: the bridge's time slice ran out inside decompiled code
    give_way(self, lock);
    break;
  default: {
    char why[64];
    std::snprintf(why, sizeof why, "svc %02x is not native yet", number);
    stop(why);
    c.exit = EXIT_UNWIND;
    return;
  }
  }
  if (stopped_) c.exit = EXIT_UNWIND;
}

} // namespace host
