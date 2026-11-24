#pragma once

// The system services the game asks for through srv:, natively. Each one is
// added when the game reaches it; until then the run stops there, saying
// which service and command, rather than inventing an answer.
#include "cro.hpp"
#include "kernel.hpp"
#include "module.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace host {

class Services {
public:
  uint64_t program_id = 0; // the game's title ID, from its exheader (ACI, 0x200)
  Module* module = nullptr; // the translated code, where ldr:ro places the CRO modules' own
  bool handle(Kernel& kernel, Thread& thread, const std::string& service, uint32_t* command);
  // once a frame, with the kernel's lock: what the services signal at each refresh
  // a frame's inputs, as the console's HID reports them (picanative's pn_input layout)
  struct Input {
    uint32_t buttons = 0;
    float circle_x = 0, circle_y = 0; // -1 to 1, up and right positive
    uint16_t touch_x = 0, touch_y = 0;
    bool touching = false;
  };
  // the screens' vertical blank: the inputs into HID's shared memory, then its events
  void vblank(Kernel& kernel, const Input& input);
  // a command of a peripheral service (network, friends, StreetPass, cameras...) not native yet: success and
  // zeros, as a console offline with nothing to report, said once in the log; false for the core services
  // (files, GPU, sound, applets, inputs), whose wrong answers would corrupt the game. CTRN_STRICT=1: never
  bool answer_neutrally(const std::string& service, uint32_t* command);

private:
  bool apt(Kernel& kernel, Thread& thread, uint32_t* command);
  // ldr:ro, the CRO modules: each loaded one runs from memory of its own at the address the game asked for, a copy
  // of its buffer (ldr:ro takes the buffer's pages away meanwhile); by address: the buffer, the memory, what fixing left
  bool ldr_ro(Kernel& kernel, uint32_t* command);
  struct CroCopy {
    uint32_t buffer, physical, size, fixed;
  };
  std::unique_ptr<Cro> cro_;
  // the system's shared font: a dump of the console's, or the stand-in tools/shared_font.py makes (CTRN_SHARED_FONT),
  // in the system's memory where the linear mapping shows it, its pointers moved there
  bool load_shared_font(Kernel& kernel);
  std::shared_ptr<SharedMemory> shared_font_;
  std::map<uint32_t, CroCopy> cro_copies_;
  bool cfg(Kernel& kernel, uint32_t* command);
  bool cecd(Kernel& kernel, uint32_t* command);

  // the console's settings (as the runtime had them for Zakuro)
  static constexpr uint8_t kRegionEurope = 2, kLanguage = 2; // French
  // StreetPass: idle, two events the service keeps
  uint32_t cecd_state_ = 1, fs_priority_ = 0;
  // HID: its shared block, which vblank fills with each frame's inputs, and its events
  bool hid(Kernel& kernel, uint32_t* command);
  uint32_t accelerometer_users_ = 0, gyroscope_users_ = 0;
  uint32_t hid_physical_ = 0;     // where the shared block lies
  uint32_t hid_previous_ = 0;     // the buttons of the last entry
  uint32_t hid_index_ = 7;        // the last entry written, of 8
  uint64_t hid_reset_tick_ = 0;   // when the index last came back to 0
  std::shared_ptr<Event> hid_events_[5];
  std::shared_ptr<Event> ir_events_[3]; // ir:USER's receive, send and connection status: never signaled, nothing plugged
  std::shared_ptr<Event> nfc_events_[2]; // nfc:u's tag in range, out of range: never signaled, no amiibo
  uint8_t nfc_state_ = 0;                 // nfc:u's tag state: 0 not initialized, 1 initialized, 2 scanning
  std::shared_ptr<SharedMemory> hid_shared_;
  // fs: the archives on host folders laid out as Zakuro lays them out, and their files' sessions
  bool fs_file(Kernel& kernel, uint32_t file, uint32_t* command);
  struct HostFile;
  bool section_in_rom(uint32_t kind, const char* name, HostFile& out);
  // CTRN_USER_DIR/savedata/<title ID>: the game's save data, as Zakuro keeps it
  std::filesystem::path save_root() const;
  // CTRN_USER_DIR/extdata/{shared,game}/<id>: extra data, shared between titles (7) or a game's own (6)
  std::filesystem::path extdata_root(bool shared, uint64_t id) const;
  std::map<uint64_t, std::filesystem::path> archives_;
  // a host file, or a read-only slice of one (the game's RomFS in the ROM)
  struct HostFile {
    std::filesystem::path path;
    uint64_t base = 0, size = 0;
    bool slice = false;
  };
  std::map<uint32_t, HostFile> files_;
  static constexpr const char* kSelfNcch = "<self ncch>"; // the archive of the game's own RomFS
  std::map<uint32_t, std::vector<uint8_t>> memory_files_; // the system titles' stand-ins
  // a directory the game opened: its entries, listed when opened, and how many it read
  struct HostDirectory {
    std::vector<std::filesystem::directory_entry> entries;
    size_t next = 0;
  };
  std::map<uint32_t, HostDirectory> directories_;
  bool fs_directory(Kernel& kernel, uint32_t id, uint32_t* c);
  uint64_t next_archive_ = 0x100000000ull;
  uint32_t next_file_ = 1;
  std::shared_ptr<Event> cecd_events_[2];

  uint32_t notification_ = 0;
  // APT: the lock, the events, the parameter waiting for the title
  struct Parameter {
    uint32_t sender = 0, signal = 0;
    std::vector<uint8_t> buffer;
    uint32_t handle = 0; // a handle handed along (an applet's shared memory), 0 when none
  };
  // a parameter for the title, from an applet or the system: it waits there until received
  void post_parameter(Kernel& kernel, Parameter parameter);
  // a library applet the title starts (keyboard, Mii selector, error display...): it ends at once,
  // answering as a player who confirms would; returns what the applet hands back
  std::vector<uint8_t> run_applet(Kernel& kernel, uint32_t applet, std::vector<uint8_t> config, uint32_t memory);
  std::shared_ptr<Event> apt_parameter_event_;
  uint32_t apt_lock_ = 0, cpu_time_limit_ = 0;
  bool has_parameter_ = false;
  Parameter parameter_;
  std::vector<uint8_t> capture_info_;
  uint32_t library_applet_ = 0;
};

} // namespace host
