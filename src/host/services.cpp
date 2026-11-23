#include "services.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>

namespace host {

namespace {

uint32_t header(uint32_t command, uint32_t normal, uint32_t translate) {
  return command << 16 | (normal & 0x3F) << 6 | (translate & 0x3F);
}

void reply(uint32_t* c, uint32_t command, std::initializer_list<uint32_t> values = {}) {
  c[0] = header(command, static_cast<uint32_t>(values.size()) + 1, 0);
  c[1] = kOk;
  uint32_t i = 2;
  for (uint32_t v : values) c[i++] = v;
}

std::string read_path(Kernel& kernel, uint32_t type, uint32_t size, uint32_t address);

// A RomFS (level 3) holding files at its root, laid out as Zakuro's builder
// lays them out (its romfs_build.rs): the root, a nameless content directory,
// the files' data 16-aligned after the metadata.
std::vector<uint8_t> romfs(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  constexpr uint32_t kInvalid = 0xFFFFFFFF;
  auto utf16 = [](const std::string& name) {
    std::vector<uint8_t> out;
    for (char ch : name) out.push_back(static_cast<uint8_t>(ch)), out.push_back(0);
    return out;
  };
  auto align = [](uint32_t v, uint32_t a) { return (v + a - 1) / a * a; };
  auto hash = [](uint32_t parent, const std::string& name) {
    uint32_t h = parent ^ 123456789u;
    for (char ch : name) h = (h >> 5 | h << 27) ^ static_cast<uint8_t>(ch);
    return h;
  };
  auto buckets = [](size_t entries) {
    uint32_t count = static_cast<uint32_t>(std::max<size_t>(entries, 1));
    if (count < 3) return 3u;
    if (count < 19) return count | 1;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 ||
           count % 13 == 0 || count % 17 == 0)
      ++count;
    return count;
  };
  auto put = [](std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  auto put_name = [&](std::vector<uint8_t>& out, const std::string& name) {
    const auto bytes = utf16(name);
    put(out, static_cast<uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
    while (out.size() % 4) out.push_back(0);
  };
  // two directories: the root (offset 0) and the content root (offset 0x18), both nameless
  const uint32_t content = 0x18, dir_meta_size = 0x30;
  std::vector<uint32_t> file_offsets;
  uint32_t file_meta_size = 0;
  for (auto& f : files) file_offsets.push_back(file_meta_size), file_meta_size += 0x20 + align(uint32_t(utf16(f.first).size()), 4);
  const uint32_t dir_buckets = buckets(2), file_buckets = buckets(files.size());
  std::vector<uint32_t> dir_hash(dir_buckets, kInvalid), file_hash(file_buckets, kInvalid);
  std::vector<uint8_t> dir_meta, file_meta, data;
  const uint32_t dir_offsets[2] = {0, content};
  for (int d = 0; d < 2; ++d) {
    const uint32_t bucket = hash(0, "") % dir_buckets, next_hash = dir_hash[bucket];
    dir_hash[bucket] = dir_offsets[d];
    put(dir_meta, 0);        // parent: the root
    put(dir_meta, kInvalid); // next sibling
    put(dir_meta, d == 0 ? content : kInvalid);
    put(dir_meta, d == 1 && !files.empty() ? 0u : kInvalid);
    put(dir_meta, next_hash);
    put_name(dir_meta, "");
  }
  for (size_t i = 0; i < files.size(); ++i) {
    while (data.size() % 16) data.push_back(0);
    const uint64_t offset = data.size(), size = files[i].second.size();
    data.insert(data.end(), files[i].second.begin(), files[i].second.end());
    const uint32_t bucket = hash(content, files[i].first) % file_buckets, next_hash = file_hash[bucket];
    file_hash[bucket] = file_offsets[i];
    put(file_meta, content);
    put(file_meta, i + 1 < files.size() ? file_offsets[i + 1] : kInvalid);
    put(file_meta, uint32_t(offset)), put(file_meta, uint32_t(offset >> 32));
    put(file_meta, uint32_t(size)), put(file_meta, uint32_t(size >> 32));
    put(file_meta, next_hash);
    put_name(file_meta, files[i].first);
  }
  const uint32_t dir_hash_offset = 0x28, dir_meta_offset = dir_hash_offset + dir_buckets * 4;
  const uint32_t file_hash_offset = dir_meta_offset + dir_meta_size, file_meta_offset = file_hash_offset + file_buckets * 4;
  const uint32_t file_data_offset = align(file_meta_offset + file_meta_size, 16);
  std::vector<uint8_t> out;
  for (uint32_t v : {0x28u, dir_hash_offset, dir_buckets * 4, dir_meta_offset, dir_meta_size, file_hash_offset,
                     file_buckets * 4, file_meta_offset, file_meta_size, file_data_offset})
    put(out, v);
  for (uint32_t v : dir_hash) put(out, v);
  out.insert(out.end(), dir_meta.begin(), dir_meta.end());
  for (uint32_t v : file_hash) put(out, v);
  out.insert(out.end(), file_meta.begin(), file_meta.end());
  out.resize(file_data_offset, 0);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

void reply_handle(uint32_t* c, uint32_t command, uint32_t handle) {
  c[0] = header(command, 1, 2);
  c[1] = kOk;
  c[2] = 0x10; // one handle, moved
  c[3] = handle;
}

} // namespace

bool Services::handle(Kernel& kernel, Thread& thread, const std::string& service, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  if (service == "srv:") {
    switch (command) {
    case 0x0001: reply(c, command); return true; // RegisterClient
    case 0x0002: {                                // EnableNotification: one semaphore, the same handle each time
      if (!notification_) {
        auto s = std::make_shared<Semaphore>();
        s->max = 16;
        notification_ = kernel.add_handle(s);
      }
      reply_handle(c, command, notification_);
      return true;
    }
    case 0x0005: { // GetServiceHandle(name, name, length, flags)
      char name[9] = {};
      std::memcpy(name, c + 1, 8);
      name[std::min<uint32_t>(c[3], 8)] = 0;
      reply_handle(c, command, kernel.add_handle(std::make_shared<Session>(Object::Kind::Session, name)));
      return true;
    }
    case 0x0009: // Subscribe
    case 0x000A: reply(c, command); return true;      // Unsubscribe
    case 0x000B: reply(c, command, {0}); return true; // ReceiveNotification: none
    case 0x000E: reply(c, command, {1}); return true; // IsServiceRegistered
    default: return false;
    }
  }
  if (service == "APT:U" || service == "APT:A" || service == "APT:S") return apt(kernel, thread, c);
  if (service == "cfg:u" || service == "cfg:s" || service == "cfg:i") return cfg(kernel, c);
  if (service == "cecd:u") return cecd(kernel, c);
  if (service == "hid:USER" || service == "hid:SPVR") return hid(kernel, c);
  if (service == "FSFile") {
    auto session = kernel.resolve(thread.ctx.r[0]);
    return session && fs_file(kernel, static_cast<Session&>(*session).sub, c);
  }
  // fs: the game's files and saves go through the platform layer (Port/platform/fs.c);
  // what reaches the service is the session's own business
  if (service == "fs:USER") {
    switch (command) {
    case 0x0801: case 0x0861: case 0x0840: case 0x085A: case 0x085D: case 0x086E:
      reply(c, command);
      return true;
    case 0x0812: reply(c, command, {512u << 20, 0}); return true;                 // GetFreeBytes
    case 0x0813: reply(c, command, {0}); return true;                             // GetCardType
    case 0x0814: case 0x0815: case 0x0849: reply(c, command, {0x200, 0x4000, 0x80000, 0x80000}); return true;
    case 0x0817: case 0x0818: case 0x0821: case 0x083D: reply(c, command, {1}); return true;
    case 0x0862: fs_priority_ = c[1], reply(c, command); return true;
    case 0x080C: { // OpenArchive(archive, path type, path size, <path>) for what fs.c leaves here
      uint32_t path[3] = {};
      kernel.memory().read(c[5], path, std::min<uint32_t>(c[3], sizeof path));
      if (c[1] == 3) { // SelfNCCH: the game's own RomFS, opened as a file below
        const uint64_t handle = next_archive_++;
        archives_[handle] = kSelfNcch;
        c[0] = header(command, 3, 0), c[1] = kOk, c[2] = uint32_t(handle), c[3] = uint32_t(handle >> 32);
        return true;
      }
      if (c[1] != 7) return false; // other archives: not native yet
      // shared extra data: media, then the 64-bit id
      const char* user = std::getenv("CTRN_USER_DIR");
      char name[17];
      std::snprintf(name, sizeof name, "%016llX", (unsigned long long)(path[1] | uint64_t{path[2]} << 32));
      const std::filesystem::path root = std::filesystem::path(user ? user : "user") / "extdata" / "shared" / name;
      std::error_code ignored;
      if (!std::filesystem::is_directory(root, ignored)) {
        if (path[1] != 0xF000000B) { // never created on this console: "not formatted" (as Zakuro)
          c[0] = header(command, 1, 0), c[1] = 0xC8A04554;
          return true;
        }
        // the play coins every console has: 42 coins, no steps, as a console that never walked
        std::filesystem::create_directories(root, ignored);
        std::ofstream(root.string() + ".format", std::ios::binary).write("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
        const uint8_t coins[20] = {0x00, 0x4F, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xDE, 0x07, 12, 29};
        std::ofstream(root / "gamecoin.dat", std::ios::binary).write(reinterpret_cast<const char*>(coins), sizeof coins);
      }
      const uint64_t handle = next_archive_++;
      archives_[handle] = root;
      c[0] = header(command, 3, 0), c[1] = kOk, c[2] = uint32_t(handle), c[3] = uint32_t(handle >> 32);
      return true;
    }
    case 0x0803: { // OpenFileDirectly(transaction, archive, archive path type, size, file path type, size, flags, attributes, <archive path>, <file path>)
      uint32_t archive_path[4] = {}, file_path[5] = {};
      kernel.memory().read(c[10], archive_path, std::min<uint32_t>(c[4], sizeof archive_path));
      kernel.memory().read(c[12], file_path, std::min<uint32_t>(c[6], sizeof file_path));
      // the console's system titles (NCCH archive, NAND, a RomFS): the stand-ins Zakuro
      // gives: the Mii data an empty CFL_Res.dat (titles start, with no Mii to draw);
      // shortcut: the region manifest and the bad word list come as empty files, build them if a game reads them
      if (c[2] != 0x2345678A || archive_path[2] != 0 || file_path[0] != 0) return false;
      std::vector<uint8_t> data;
      if (archive_path[1] == 0x0004009B && archive_path[0] == 0x00010202)
        data = romfs({{"CFL_Res.dat", std::vector<uint8_t>(0x1000)}});
      const uint32_t id = next_file_++;
      memory_files_[id] = std::move(data);
      auto session = std::make_shared<Session>(Object::Kind::Session, "FSFile");
      session->sub = id;
      c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0x10, c[3] = kernel.add_handle(session);
      return true;
    }
    case 0x0802: { // OpenFile(transaction, archive, path type, path size, flags, attributes, <path>)
      const uint64_t archive = c[2] | uint64_t{c[3]} << 32;
      const auto found = archives_.find(archive);
      if (found == archives_.end()) return false;
      const uint32_t flags = c[6];
      if (found->second == kSelfNcch) { // a 12-byte binary path, its first word 0: the RomFS, from its level 3
        // a 12-byte binary path: kind 0 the RomFS (from its level 3), kind 2 an ExeFS file by name
        if (c[4] != 2 || c[5] != 12) return false;
        const uint32_t kind = kernel.memory().read32(c[9]);
        char exefs_name[9] = {};
        kernel.memory().read(c[9] + 4, exefs_name, 8);
        HostFile rom;
        if (!section_in_rom(kind, exefs_name, rom)) return false;
        const uint32_t id = next_file_++;
        files_[id] = rom;
        auto session = std::make_shared<Session>(Object::Kind::Session, "FSFile");
        session->sub = id;
        c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0x10, c[3] = kernel.add_handle(session);
        return true;
      }
      const std::filesystem::path file = found->second / read_path(kernel, c[4], c[5], c[9]);
      std::error_code ignored;
      if (!std::filesystem::exists(file, ignored)) {
        if (!(flags & 4)) { // no create flag
          c[0] = header(command, 1, 0), c[1] = 0xC8804470;
          return true;
        }
        std::ofstream(file, std::ios::binary);
      }
      const uint32_t id = next_file_++;
      files_[id] = {file};
      auto session = std::make_shared<Session>(Object::Kind::Session, "FSFile");
      session->sub = id;
      c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0x10, c[3] = kernel.add_handle(session);
      return true;
    }
    case 0x0808: { // CreateFile(transaction, archive, path type, path size, attributes, size, <path>)
      const auto found = archives_.find(c[2] | uint64_t{c[3]} << 32);
      if (found == archives_.end()) return false;
      const std::filesystem::path file = found->second / read_path(kernel, c[4], c[5], c[10]);
      std::error_code ignored;
      if (std::filesystem::exists(file, ignored)) {
        c[0] = header(command, 1, 0), c[1] = 0xC82044B4; // already exists
        return true;
      }
      std::ofstream(file, std::ios::binary);
      std::filesystem::resize_file(file, c[7] | uint64_t{c[8]} << 32, ignored);
      reply(c, command);
      return true;
    }
    case 0x0804: { // DeleteFile(transaction, archive, path type, path size, <path>)
      const auto found = archives_.find(c[2] | uint64_t{c[3]} << 32);
      if (found == archives_.end()) return false;
      std::error_code missing;
      if (!std::filesystem::remove(found->second / read_path(kernel, c[4], c[5], c[7]), missing)) {
        c[0] = header(command, 1, 0), c[1] = 0xC8804470;
        return true;
      }
      reply(c, command);
      return true;
    }
    case 0x080D: { // ControlArchive: commits, which host files need not
      uint32_t descriptors[4];
      for (int i = 0; i < 4; ++i) descriptors[i] = c[6 + i];
      c[0] = header(command, 1, 4), c[1] = kOk;
      for (int i = 0; i < 4; ++i) c[2 + i] = descriptors[i];
      return true;
    }
    case 0x080E: archives_.erase(c[1] | uint64_t{c[2]} << 32), reply(c, command); return true; // CloseArchive
    case 0x0863: reply(c, command, {fs_priority_}); return true;
    default: return false;
    }
  }
  // daemons and the microphone: nothing to do offline, without a microphone (as Zakuro)
  if (service == "ndm:u") {
    switch (command) {
    case 0x0001: case 0x0002: case 0x0006: case 0x0007: case 0x0008: case 0x0009: case 0x000A: case 0x000E:
    case 0x0014: reply(c, command); return true;
    default: return false;
    }
  }
  if (service == "mic:u") {
    switch (command) {
    case 0x0001: case 0x0002: case 0x0003: case 0x0004: case 0x0005: case 0x0008: case 0x0009: case 0x000A:
    case 0x000D: reply(c, command); return true;
    default: return false;
    }
  }
  return false; // the services to come, one by one
}

namespace {
// a path the game passes: ASCII (3) or UTF-16 (4), without its leading slash
std::string read_path(Kernel& kernel, uint32_t type, uint32_t size, uint32_t address) {
  std::string name;
  for (uint32_t i = 0; i < size; i += type == 4 ? 2 : 1) {
    uint8_t ch = 0;
    kernel.memory().read(address + i, &ch, 1);
    if (!ch) break;
    name += static_cast<char>(ch);
  }
  while (!name.empty() && name.front() == '/') name.erase(name.begin());
  return name;
}

// a static buffer the client set up to receive into: TLS + 0x180 + 8 * id
void static_buffer(Kernel& kernel, Thread& thread, uint32_t id, uint32_t& address, uint32_t& capacity) {
  const uint32_t descriptor = kernel.memory().read32(thread.ctx.tls + 0x180 + id * 8);
  address = kernel.memory().read32(thread.ctx.tls + 0x184 + id * 8);
  capacity = descriptor >> 14;
}
uint32_t static_descriptor(uint32_t size, uint32_t id) { return size << 14 | id << 10 | 2; }
} // namespace

// APT, the applet manager: one title, no applets (as Zakuro)
bool Services::apt(Kernel& kernel, Thread& thread, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  switch (command) {
  case 0x0001: { // GetLockHandle(flags) -> attributes, power state, lock
    if (!apt_lock_) apt_lock_ = kernel.add_handle(std::make_shared<Mutex>());
    const uint32_t attributes = c[1];
    c[0] = header(0x0001, 3, 2), c[1] = kOk, c[2] = attributes, c[3] = 0, c[4] = 0, c[5] = apt_lock_;
    return true;
  }
  case 0x0002: { // Initialize(app id, attributes) -> notification event, parameter event
    auto notification = std::make_shared<Event>(), parameter = std::make_shared<Event>();
    const uint32_t n = kernel.add_handle(notification), p = kernel.add_handle(parameter);
    // a title starts by being handed a Wakeup parameter
    has_parameter_ = true;
    parameter_ = {0, 1};
    kernel.signal(*parameter);
    c[0] = header(0x0002, 1, 3), c[1] = kOk, c[2] = 0x04000000, c[3] = n, c[4] = p; // two handles
    return true;
  }
  case 0x0003:
  case 0x0004: reply(c, command); return true;                           // Enable, Finalize
  case 0x0005: reply(c, command, {0, 0, 0x101, 0x300}); return true;     // GetAppletManInfo
  case 0x0006:                                                          // GetAppletInfo
    reply(c, command, {uint32_t(program_id), uint32_t(program_id >> 32), 0, 1, 1, 0});
    return true;
  case 0x0009: reply(c, command, {c[1] != 0x400 || library_applet_ ? 1u : 0u}); return true; // IsRegistered
  case 0x000B: reply(c, command, {0}); return true;                      // InquireNotification: none
  case 0x000C: reply(c, command); return true;                           // SendParameter
  case 0x000D:                                                          // ReceiveParameter
  case 0x000E: {                                                        // GlanceParameter
    if (!has_parameter_) {
      c[0] = header(command, 1, 0), c[1] = 0xC8A0CFFC; // no parameter waiting
      return true;
    }
    uint32_t address, capacity;
    static_buffer(kernel, thread, 0, address, capacity);
    c[0] = header(command, 4, 4), c[1] = kOk, c[2] = parameter_.sender, c[3] = parameter_.signal, c[4] = 0;
    c[5] = 0x10, c[6] = 0, c[7] = static_descriptor(0, 0), c[8] = address;
    if (command == 0x000D) has_parameter_ = false;
    return true;
  }
  case 0x000F: has_parameter_ = false, reply(c, command, {1}); return true; // CancelParameter
  case 0x0016:
  case 0x0018: library_applet_ = c[1], reply(c, command); return true;     // Preload/PrepareToStart LibraryApplet
  case 0x0015: case 0x0017: case 0x0019: case 0x001B: case 0x001F:
  case 0x003E: case 0x003F: case 0x0043: case 0x0055:
    reply(c, command);
    return true;
  case 0x0040: { // SendCaptureBufferInfo(size, buffer)
    const uint32_t size = c[1], address = c[3];
    capture_info_.resize(size);
    kernel.memory().read(address, capture_info_.data(), size);
    reply(c, command);
    return true;
  }
  case 0x0041: { // ReceiveCaptureBufferInfo(size)
    uint32_t address, capacity;
    static_buffer(kernel, thread, 0, address, capacity);
    const uint32_t size = std::min({static_cast<uint32_t>(capture_info_.size()), c[1], capacity});
    if (size) kernel.memory().write(address, capture_info_.data(), size);
    c[0] = header(0x0041, 2, 2), c[1] = kOk, c[2] = size, c[3] = static_descriptor(size, 0), c[4] = address;
    return true;
  }
  case 0x0044: // GetSharedFont: shortcut: none yet (as Zakuro without a dumped font); map it if the game needs it
    c[0] = header(0x0044, 1, 0), c[1] = 0xFFFFFFFF;
    return true;
  case 0x0035: case 0x0046: case 0x0047: reply(c, command, {0, 0}); return true;
  case 0x004B: reply(c, command, {0}); return true;                      // AppletUtility
  case 0x004F: cpu_time_limit_ = c[2], reply(c, command); return true;  // SetAppCpuTimeLimit
  case 0x0050: reply(c, command, {cpu_time_limit_}); return true;
  case 0x0056: reply(c, command, {1}); return true;                      // GetScreenCapPostPermission
  case 0x0058: reply(c, command, {uint32_t(program_id), uint32_t(program_id >> 32)}); return true;
  case 0x0101: case 0x0102: reply(c, command, {0}); return true;         // not a New 3DS
  case 0x0103: reply(c, command, {1}); return true;                      // platform: 3DS
  default: return false;
  }
}

// cfg: the console's configuration blocks
bool Services::cfg(Kernel& kernel, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  switch (command) {
  case 0x0001: { // GetConfigInfoBlk2(size, block id) into a static buffer
    const uint32_t size = c[1], block = c[2], out = c[4];
    std::vector<uint8_t> data(size);
    auto put = [&](size_t at, uint8_t value) {
      if (at < data.size()) data[at] = value;
    };
    switch (block) {
    case 0x00090000: { // the console's number
      const uint64_t id = 0x0123456789ABCDEFull;
      for (size_t i = 0; i < 8; ++i) put(i, uint8_t(id >> (8 * i)));
      break;
    }
    case 0x000A0000: { // the user's name, UTF-16
      const char name[] = "Player";
      for (size_t i = 0; i + 1 < sizeof name; ++i) put(i * 2, uint8_t(name[i]));
      break;
    }
    case 0x000A0001: put(0, 1), put(1, 1); break; // birthday
    case 0x000A0002: put(0, kLanguage); break;
    case 0x000B0000:
    case 0x000B0001: put(3, 110); break; // country: United Kingdom
    case 0x000D0000: put(0, 1), put(1, 1); break; // EULA version
    case 0x00070001: put(0, 1); break;              // sound: stereo
    case 0x00050005: { // the stereo camera's parameters (zeroes turn the 3D camera into NaN)
      const float values[8] = {62.0f, 289.0f, 76.8f, 46.08f, 10.0f, 5.0f, 55.58f, 21.57f};
      for (size_t i = 0; i < 8 && i * 4 + 4 <= data.size(); ++i) std::memcpy(data.data() + i * 4, &values[i], 4);
      break;
    }
    default: break; // parental controls, 3D settings and the rest: zeroes
    }
    kernel.memory().write(out, data.data(), size);
    reply(c, command);
    return true;
  }
  case 0x0002: reply(c, command, {kRegionEurope}); return true;             // SecureInfoGetRegion
  case 0x0003: reply(c, command, {0x33415A55, 0x00005255}); return true;   // GenHashConsoleUnique
  case 0x0004: reply(c, command, {0}); return true;                        // GetRegionCanadaUSA
  case 0x0005: reply(c, command, {0}); return true;                        // GetSystemModel: old 3DS
  case 0x0006: reply(c, command, {1}); return true;                        // not a 2DS
  default: return false;
  }
}

bool Services::hid(Kernel& kernel, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  switch (command) {
  case 0x000A: { // GetIPCHandles -> the shared block and five events
    if (!hid_block_) {
      auto block = std::make_shared<SharedMemory>();
      block->size = 0x1000;
      block->physical = kernel.allocate(Kernel::kBase, 0x1000, false);
      std::memset(kernel.memory().physical(block->physical, 0x1000), 0, 0x1000);
      hid_block_ = kernel.add_handle(block);
      for (int i = 0; i < 5; ++i) {
        hid_events_[i] = std::make_shared<Event>();
        hid_event_handles_[i] = kernel.add_handle(hid_events_[i]);
      }
    }
    c[0] = header(command, 1, 7), c[1] = kOk, c[2] = 5u << 26, c[3] = hid_block_; // six handles
    for (int i = 0; i < 5; ++i) c[4 + i] = hid_event_handles_[i];
    return true;
  }
  case 0x0011: ++accelerometer_users_, reply(c, command); return true;
  case 0x0012: accelerometer_users_ -= accelerometer_users_ > 0, reply(c, command); return true;
  case 0x0013: ++gyroscope_users_, reply(c, command); return true;
  case 0x0014: gyroscope_users_ -= gyroscope_users_ > 0, reply(c, command); return true;
  case 0x0015: { // GetGyroscopeLowRawToDpsCoefficient
    const float coefficient = 14.375f;
    uint32_t bits;
    std::memcpy(&bits, &coefficient, 4);
    reply(c, command, {bits});
    return true;
  }
  case 0x0016: reply(c, command, {0, 0, 0, 0, 0}); return true; // GetGyroscopeLowCalibrateParam
  case 0x0017: reply(c, command, {0x3F}); return true;          // GetSoundVolume
  default: return false;
  }
}

// a file of an archive the service opened, on the host
bool Services::fs_file(Kernel& kernel, uint32_t id, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  if (auto memory = memory_files_.find(id); memory != memory_files_.end()) {
    const std::vector<uint8_t>& bytes = memory->second;
    switch (command) {
    case 0x0802: { // Read(offset, size, <buffer>)
      const uint64_t offset = c[1] | uint64_t{c[2]} << 32;
      const uint32_t size = c[3], to = c[5];
      const uint32_t read = offset < bytes.size() ? static_cast<uint32_t>(std::min<uint64_t>(size, bytes.size() - offset)) : 0;
      if (read) kernel.memory().write(to, bytes.data() + offset, read);
      c[0] = header(command, 2, 2), c[1] = kOk, c[2] = read, c[3] = size << 4 | 0xC, c[4] = to;
      return true;
    }
    case 0x0804: reply(c, command, {uint32_t(bytes.size()), 0}); return true;
    case 0x0808: memory_files_.erase(memory), reply(c, command); return true;
    case 0x0806: case 0x080B: reply(c, command, {0}); return true;
    case 0x0807: case 0x0809: case 0x080A: reply(c, command); return true;
    default: return false; // read-only
    }
  }
  const auto found = files_.find(id);
  if (found == files_.end()) return false;
  const HostFile& host_file = found->second;
  const std::filesystem::path& path = host_file.path;
  std::error_code ignored;
  switch (command) {
  case 0x0802: { // Read(offset, size, <buffer>)
    const uint64_t offset = c[1] | uint64_t{c[2]} << 32;
    const uint32_t size = c[3], to = c[5];
    std::ifstream in(path, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(host_file.base + offset));
    const uint32_t wanted = host_file.slice ? static_cast<uint32_t>(offset < host_file.size ? std::min<uint64_t>(size, host_file.size - offset) : 0) : size;
    std::vector<char> bytes(size);
    in.read(bytes.data(), wanted);
    const uint32_t read = static_cast<uint32_t>(std::max<std::streamsize>(in.gcount(), 0));
    kernel.memory().write(to, bytes.data(), read);
    c[0] = header(command, 2, 2), c[1] = kOk, c[2] = read, c[3] = size << 4 | 0xC, c[4] = to;
    return true;
  }
  case 0x0803: { // Write(offset, size, flags, <buffer>)
    if (host_file.slice) { // the ROM is read-only
      c[0] = header(command, 2, 2), c[1] = kOk, c[2] = 0, c[3] = c[3] << 4 | 0xA, c[4] = c[6];
      return true;
    }
    const uint64_t offset = c[1] | uint64_t{c[2]} << 32;
    const uint32_t size = c[3], from = c[6];
    std::vector<char> bytes(size);
    kernel.memory().read(from, bytes.data(), size);
    std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(static_cast<std::streamoff>(offset));
    out.write(bytes.data(), size);
    c[0] = header(command, 2, 2), c[1] = kOk, c[2] = out ? size : 0, c[3] = size << 4 | 0xA, c[4] = from;
    return true;
  }
  case 0x0804: { // GetSize
    const uint64_t size = host_file.slice ? host_file.size : std::filesystem::file_size(path, ignored);
    reply(c, command, {uint32_t(size), uint32_t(size >> 32)});
    return true;
  }
  case 0x0805: std::filesystem::resize_file(path, c[1] | uint64_t{c[2]} << 32, ignored), reply(c, command); return true;
  case 0x0806: reply(c, command, {0}); return true;     // GetAttributes: an ordinary file
  case 0x0808: files_.erase(found), reply(c, command); return true; // Close
  case 0x0807: case 0x0809: case 0x080A: reply(c, command); return true;
  case 0x080B: reply(c, command, {0}); return true;
  default: return false;
  }
}

// where a part of the game lies in the ROM (CTRN_ROM, a decrypted .3ds): its RomFS
// level 3 (kind 0, as fs.c finds it) or an ExeFS file by name (kind 2)
bool Services::section_in_rom(uint32_t kind, const char* name, HostFile& out) {
  const char* rom = std::getenv("CTRN_ROM");
  if (!rom) return false;
  std::ifstream in(rom, std::ios::binary);
  uint8_t h[0x200];
  auto u32 = [&](size_t at) {
    uint32_t v;
    std::memcpy(&v, h + at, 4);
    return v;
  };
  if (!in.read(reinterpret_cast<char*>(h), 0x200) || std::memcmp(h + 0x100, "NCSD", 4)) return false;
  const uint64_t ncch = uint64_t{u32(0x120)} * 0x200;
  if (!in.seekg(static_cast<std::streamoff>(ncch)).read(reinterpret_cast<char*>(h), 0x200) || std::memcmp(h + 0x100, "NCCH", 4) ||
      !(h[0x18F] & 4)) // encrypted: not ours to read
    return false;
  if (kind == 2) {
    const uint64_t exefs = ncch + uint64_t{u32(0x1A0)} * 0x200;
    if (!in.seekg(static_cast<std::streamoff>(exefs)).read(reinterpret_cast<char*>(h), 0x200)) return false;
    for (int i = 0; i < 10; ++i)
      if (std::strncmp(reinterpret_cast<const char*>(h + i * 16), name, 8) == 0 && h[i * 16]) {
        out = {rom, exefs + 0x200 + u32(i * 16 + 8), u32(i * 16 + 12), true};
        return true;
      }
    return false;
  }
  if (kind != 0) return false;
  const uint64_t ivfc = ncch + uint64_t{u32(0x1B0)} * 0x200, size = uint64_t{u32(0x1B4)} * 0x200;
  if (!in.seekg(static_cast<std::streamoff>(ivfc)).read(reinterpret_cast<char*>(h), 0x60) || std::memcmp(h, "IVFC", 4)) return false;
  const uint32_t master = u32(0x08), log2 = u32(0x4C);
  if (log2 > 31) return false;
  const uint64_t block = 1ull << log2, level3 = (0x60 + master + block - 1) & ~(block - 1);
  out = {rom, ivfc + level3, size - level3, true};
  return true;
}

void Services::vblank(Kernel& kernel) {
  if (!hid_events_[0]) return;
  kernel.signal(*hid_events_[0]); // the pad
  kernel.signal(*hid_events_[1]); // the touch screen
  if (accelerometer_users_) kernel.signal(*hid_events_[2]);
  if (gyroscope_users_) kernel.signal(*hid_events_[3]);
}

// cecd, StreetPass: this console never meets another
bool Services::cecd(Kernel& kernel, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  for (auto& event : cecd_events_)
    if (!event) event = std::make_shared<Event>();
  switch (command) {
  case 0x0001: reply(c, command); return true;
  case 0x000B: // Start / Stop: a title waits for the state change
  case 0x000C:
    cecd_state_ = command == 0x000B ? 2 : 1;
    kernel.signal(*cecd_events_[1]);
    reply(c, command);
    return true;
  case 0x000E: reply(c, command, {cecd_state_}); return true;
  case 0x000F:
  case 0x0010: {
    const uint32_t handle = kernel.add_handle(cecd_events_[command - 0x000F]);
    c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0, c[3] = handle;
    return true;
  }
  case 0x0012: reply(c, command, {0}); return true; // OpenAndRead: the inbox reads empty
  default:
    // the rest of StreetPass (opening boxes, writing messages...) has nothing to
    // do on a console that meets no other: success, outputs zeroed (as Zakuro)
    reply(c, command);
    for (int i = 2; i < 8; ++i) c[i] = 0;
    return true;
  }
}

} // namespace host
