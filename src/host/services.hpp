#pragma once

// The system services the game asks for through srv:, natively. Each one is
// added when the game reaches it; until then the run stops there, saying
// which service and command, rather than inventing an answer.
#include "kernel.hpp"

#include <filesystem>
#include <map>
#include <vector>

namespace host {

class Services {
public:
  uint64_t program_id = 0; // the game's title ID, from its exheader (ACI, 0x200)
  bool handle(Kernel& kernel, Thread& thread, const std::string& service, uint32_t* command);
  // once a frame, with the kernel's lock: what the services signal at each refresh
  void vblank(Kernel& kernel);

private:
  bool apt(Kernel& kernel, Thread& thread, uint32_t* command);
  bool cfg(Kernel& kernel, uint32_t* command);
  bool cecd(Kernel& kernel, uint32_t* command);

  // the console's settings (as the runtime had them for Zakuro)
  static constexpr uint8_t kRegionEurope = 2, kLanguage = 2; // French
  // StreetPass: idle, two events the service keeps
  uint32_t cecd_state_ = 1, fs_priority_ = 0;
  // HID: the pad and touch screen come from the platform layer (Port/platform/hid.c);
  // the service only hands out its block and signals its events every frame
  bool hid(Kernel& kernel, uint32_t* command);
  uint32_t hid_block_ = 0, accelerometer_users_ = 0, gyroscope_users_ = 0;
  std::shared_ptr<Event> hid_events_[5];
  uint32_t hid_event_handles_[5] = {};
  // fs: the archives fs.c leaves to the service (shared extra data), on host
  // folders laid out as Zakuro lays them out, and their files' sessions
  bool fs_file(Kernel& kernel, uint32_t file, uint32_t* command);
  struct HostFile;
  bool section_in_rom(uint32_t kind, const char* name, HostFile& out);
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
  uint64_t next_archive_ = 0x100000000ull;
  uint32_t next_file_ = 1;
  std::shared_ptr<Event> cecd_events_[2];

  uint32_t notification_ = 0;
  // APT: the lock, the events, the parameter waiting for the title
  struct Parameter {
    uint32_t sender = 0, signal = 0;
  };
  uint32_t apt_lock_ = 0, cpu_time_limit_ = 0;
  bool has_parameter_ = false;
  Parameter parameter_;
  std::vector<uint8_t> capture_info_;
  uint32_t library_applet_ = 0;
};

} // namespace host
