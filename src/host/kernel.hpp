#pragma once

// The 3DS kernel, natively: the game's threads are host threads, its
// synchronization objects are ours, and time is the host's clock. One game
// thread runs at a time (the console's application core: the recompiled
// ldrex/strex are not atomic across host threads), chosen by priority as the
// console's scheduler chooses. The rules follow what Zakuro does, which this
// game is known to run with (see the notes at each one).
#include "abi.hpp"
#include "memory.hpp"
#include "module.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace host {

using Clock = std::chrono::steady_clock;
constexpr uint64_t kTicksPerSecond = 268111856; // the CPU clock, GetSystemTick's unit

enum : uint32_t {
  kOk = 0,
  kInvalidHandle = 0xD8E007F7,
  kOutOfMemory = 0xD86007F3,
  kInvalidEnum = 0xD8E00405,
  kInvalidAddress = 0xE0E01BF5,
  kMisalignedSize = 0xE0E01BF2,
  kTimeout = 0x09401BFE,
};

struct Thread;

struct Object {
  enum class Kind { Thread, Event, Mutex, Semaphore, Timer, Arbiter, SharedMemory, Session, Port, Process, ResourceLimit };
  explicit Object(Kind k) : kind(k) {}
  virtual ~Object() = default;
  const Kind kind;
};

struct Event : Object {
  Event() : Object(Kind::Event) {}
  int reset = 0; // 0 one-shot, 1 sticky, 2 pulse
  bool signaled = false;
};
struct Mutex : Object {
  Mutex() : Object(Kind::Mutex) {}
  Thread* owner = nullptr;
  uint32_t count = 0;
};
struct Semaphore : Object {
  Semaphore() : Object(Kind::Semaphore) {}
  int32_t count = 0, max = 0;
};
struct Arbiter : Object {
  Arbiter() : Object(Kind::Arbiter) {}
  std::deque<std::pair<Thread*, uint32_t>> waiting; // in arrival order, with the address
};
struct SharedMemory : Object {
  SharedMemory() : Object(Kind::SharedMemory) {}
  uint32_t physical = 0, size = 0, own_address = 0;
  uint32_t mapped_at = 0; // where the game mapped it, once it has
};
struct Session : Object { // a client session to a service, or a port
  explicit Session(Kind k, std::string n) : Object(k), name(std::move(n)) {}
  std::string name;
  uint32_t sub = 0;
};
struct Simple : Object { // process, resource limit
  explicit Simple(Kind k) : Object(k) {}
};

struct Thread : Object {
  Thread() : Object(Kind::Thread) {}
  enum class State { Ready, Running, Sleeping, WaitSync, WaitArbiter, Dead } state = State::Ready;
  Context ctx{};
  uint32_t vfp[32] = {}, fpscr = 0;
  int priority = 0;
  uint32_t id = 0, index = 0, tls_slot = 0;
  std::condition_variable baton; // notified when it may run
  // a wait
  std::vector<std::shared_ptr<Object>> waiting;
  bool wait_all = false;
  Clock::time_point wake_at = Clock::time_point::max();
  uint32_t result0 = 0, result1 = 0; // what the blocking svc returns when it wakes
  Clock::time_point running_since{};
  uint32_t last_pc = 0; // the last address the host loop dispatched, for a bad-pc report
};

// What a service answers: the command buffer is at TLS + 0x80; returns false
// when the service or command is not there yet (the run stops, saying which).
using ServiceHandler = std::function<bool(class Kernel&, Thread&, const std::string& service, uint32_t* command)>;

class Kernel {
public:
  Kernel(Memory& memory, const Module& module);

  // the process as the exheader describes it, the main thread ready at entry
  struct Image {
    const uint8_t* code;
    uint32_t code_size;
    uint32_t text_address, text_pages, ro_address, ro_pages, data_address, data_pages, data_size, bss_size;
    uint32_t stack_size;
    int priority;
    uint32_t app_memory; // bytes of FCRAM for the application
  };
  void load(const Image& image);

  // starts the game's threads; the host's own loop runs meanwhile
  void start();
  bool stopped() const { return stopped_; }
  // waits for the game to stop; returns why
  std::string wait();

  void set_services(ServiceHandler handler) { services_ = std::move(handler); }
  // the natives library functions are replaced by (svc 0xF0 from the launcher's overrides, see docs/RENDU-NATIF.md):
  // true when one took the call (its results in the registers), false to run the game's own function
  using NativeHandler = std::function<bool(Thread&, uint32_t which)>;
  void set_natives(NativeHandler handler) { natives_ = std::move(handler); }
  // what the game asked of the 3DS system: svcs by number and return address,
  // service requests by service and command (what is left to make native)
  void report() const;
  // a native call from the game's platform layer in place of a service request:
  // runs `work` with the lock, then lets the others run a little while, as a
  // request does (so scheduling stays as it was)
  void native_call(Thread& self, const std::function<void()>& work);

  // for services and the host: objects, handles, events
  uint32_t add_handle(std::shared_ptr<Object> object);
  std::shared_ptr<Object> resolve(uint32_t handle);
  std::shared_ptr<Thread> current() { return running_ ? threads_[running_->index] : nullptr; }
  void signal(Event& event);           // with the lock held
  // with the lock: a thread waiting on what was just signaled may preempt
  // the running one at its next budget check (for the host's own signals)
  void wake();
  std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(mutex_); }
  void stop(const std::string& why);   // ends the run with this reason
  Memory& memory() { return memory_; }
  uint64_t ticks() const;
  // physical memory from the process's regions (bottom or top), 0 when out
  uint32_t allocate(int region, uint32_t size, bool top);
  void release(uint32_t physical, uint32_t size); // pages allocate gave, free again
  enum { kApplication, kSystem, kBase };
  uint32_t linear_base() const { return linear_base_; } // where FCRAM's start appears in the linear mapping
  // the guest code the running thread last entered, for diagnostics (read without the lock)
  uint32_t running_pc() const { return running_ ? running_->last_pc : 0; }

private:
  friend struct Callbacks;
  void svc(Thread& self, uint32_t number);
  void thread_main(Thread* self);
  Thread& current(Context* ctx) { return *static_cast<Thread*>(ctx->user); }
  std::shared_ptr<Thread> create_thread(uint32_t entry, uint32_t stack, uint32_t argument, int priority);
  void end_thread(Thread& thread);
  // with the lock: wakes what can wake, then picks who runs
  void pass();
  Thread* pick();
  // with the lock: gives the core to whoever should have it, and waits
  // until this thread has it again (when it is still alive)
  void reschedule(Thread& self, std::unique_lock<std::mutex>& lock);
  void hand_to(Thread* next);
  void block(Thread& self, std::unique_lock<std::mutex>& lock);
  void give_way(Thread& self, std::unique_lock<std::mutex>& lock);
  bool signaled(const Object& object, const Thread& waiter) const;
  void acquire(Object& object, Thread& waiter);
  void timekeeper();
  void map_tls(Thread& thread);

  Memory& memory_;
  const Module& module_;
  Host host_{};
  ServiceHandler services_;
  NativeHandler natives_;

  std::mutex mutex_;
  std::condition_variable wake_timekeeper_, finished_;
  std::thread keeper_;
  std::vector<std::shared_ptr<Thread>> threads_;
  Thread* running_ = nullptr;
  std::atomic<bool> preempt_{false}, stopped_{false}; // read by running game threads without the lock
  std::string why_;
  std::map<uint32_t, std::shared_ptr<Object>> handles_;
  uint32_t next_handle_ = 0x10000, next_thread_id_ = 0;
  std::vector<uint32_t> free_tls_;
  uint32_t next_tls_ = 0;
  std::vector<Event*> pulsed_;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> svc_sites_;
  std::map<std::string, uint64_t> requests_;
  std::shared_ptr<Object> process_ = std::make_shared<Simple>(Object::Kind::Process);
  Clock::time_point start_ = Clock::now();
  Clock::time_point idle_since_{};
  Clock::duration idle_{};

public:
  // seconds so far no game thread could run (with the lock): the game waited for the console,
  // not for the host's CPU; the busy share of a stretch of time is the rest
  double idle_seconds() const {
    auto idle = idle_;
    if (idle_since_ != Clock::time_point{}) idle += Clock::now() - idle_since_;
    return std::chrono::duration<double>(idle).count();
  }

  // physical memory: the process's regions, page by page
  struct Region {
    uint32_t base, size;
    std::vector<bool> used;
  } regions_[3];
  struct { uint32_t text, text_end, ro_end, end; } code_{}; // the game's segments, .text first
  uint32_t heap_top_ = 0x08000000;
  uint32_t linear_base_ = 0x14000000;
};

} // namespace host
