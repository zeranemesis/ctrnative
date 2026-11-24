#include "services.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <set>

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
  if (service == "ldr:ro") return ldr_ro(kernel, c);
  // the infrared port, for the Circle Pad Pro: a console with nothing plugged, which never connects
  if (service == "ir:USER") {
    switch (command) {
    case 0x0018: { // InitializeIrNopShared(sizes x5, baud rate, handle descriptor, shared memory)
      if (auto o = kernel.resolve(c[8]); o && o->kind == Object::Kind::SharedMemory) {
        const auto& block = static_cast<const SharedMemory&>(*o);
        const uint32_t at = block.mapped_at ? block.mapped_at : block.own_address;
        if (at) kernel.memory().write(at + 0x0E, "\1", 1); // the header's "initialized"
      }
      reply(c, command);
      return true;
    }
    case 0x000A: case 0x000B: case 0x000C: { // GetReceiveEvent, GetSendEvent, GetConnectionStatusEvent
      auto& event = ir_events_[command - 0x000A];
      if (!event) event = std::make_shared<Event>();
      c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0, c[3] = kernel.add_handle(event);
      return true;
    }
    // FinalizeIrNop, clear the buffers, RequireConnection (it never comes: the game's wait times out), AutoConnection,
    // AnyConnection, Disconnect, ReleaseReceivedData, SetOwnMachineId
    case 0x0002: case 0x0003: case 0x0004: case 0x0006: case 0x0007: case 0x0008: case 0x0009: case 0x0019: case 0x001A:
      reply(c, command);
      return true;
    default: return false;
    }
  }
  // the amiibo reader, as a New 3DS's built-in one with nothing on it: ready at once, scanning finds no tag
  if (service == "nfc:u") {
    switch (command) {
    case 0x0001: nfc_state_ = 1; reply(c, command); return true; // Initialize
    case 0x0002: nfc_state_ = 0; reply(c, command); return true; // Shutdown
    case 0x0005: nfc_state_ = 2; reply(c, command); return true; // StartTagScanning
    case 0x0006: case 0x0008:                                    // StopTagScanning, ResetTagScanState
      nfc_state_ = nfc_state_ ? 1 : 0;
      reply(c, command);
      return true;
    case 0x0003: case 0x0004: reply(c, command); return true; // StartCommunication, StopCommunication
    case 0x000B: case 0x000C: { // GetTagInRangeEvent, GetTagOutOfRangeEvent
      auto& event = nfc_events_[command - 0x000B];
      if (!event) event = std::make_shared<Event>();
      c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0, c[3] = kernel.add_handle(event);
      return true;
    }
    case 0x000D: reply(c, command, {nfc_state_}); return true; // GetTagState
    case 0x000F: reply(c, command, {2}); return true;          // CommunicationGetStatus: connected
    case 0x0012: reply(c, command, {0}); return true;          // CommunicationGetResult: no error
    default: return false;
    }
  }
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
  if (service == "FSDir") {
    auto session = kernel.resolve(thread.ctx.r[0]);
    return session && fs_directory(kernel, static_cast<Session&>(*session).sub, c);
  }
  if (service == "FSFile") {
    auto session = kernel.resolve(thread.ctx.r[0]);
    return session && fs_file(kernel, static_cast<Session&>(*session).sub, c);
  }
  // fs: the game's RomFS and ExeFS (in the ROM), its save data and shared extra data (on the host)
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
    // GetThisSaveDataSecureValue(slot): the save's anti-rollback value; none was ever set here (a new save):
    // exists 0, not a game card's, value 0
    case 0x086F: reply(c, command, {0, 0, 0, 0}); return true;
    case 0x080C: { // OpenArchive(archive, path type, path size, <path>)
      uint32_t path[3] = {};
      kernel.memory().read(c[5], path, std::min<uint32_t>(c[3], sizeof path));
      if (c[1] == 3) { // SelfNCCH: the game's own RomFS, opened as a file below
        const uint64_t handle = next_archive_++;
        archives_[handle] = kSelfNcch;
        c[0] = header(command, 3, 0), c[1] = kOk, c[2] = uint32_t(handle), c[3] = uint32_t(handle >> 32);
        return true;
      }
      if (c[1] == 4) { // the game's save data: a directory of the host, once formatted
        const std::filesystem::path root = save_root();
        std::error_code ignored;
        if (!std::filesystem::is_directory(root, ignored)) {
          c[0] = header(command, 1, 0), c[1] = 0xC8A04554; // not formatted: the game formats it
          return true;
        }
        const uint64_t handle = next_archive_++;
        archives_[handle] = root;
        c[0] = header(command, 3, 0), c[1] = kOk, c[2] = uint32_t(handle), c[3] = uint32_t(handle >> 32);
        return true;
      }
      if (c[1] == 9 || c[1] == 0xA) { // the SD card (0xA write-only): a directory of the host, always there
        const char* user = std::getenv("CTRN_USER_DIR");
        const std::filesystem::path root = std::filesystem::path(user ? user : "user") / "sdmc";
        std::error_code ignored;
        std::filesystem::create_directories(root, ignored);
        const uint64_t handle = next_archive_++;
        archives_[handle] = root;
        c[0] = header(command, 3, 0), c[1] = kOk, c[2] = uint32_t(handle), c[3] = uint32_t(handle >> 32);
        return true;
      }
      if (c[1] != 6 && c[1] != 7) return false; // other archives: not native yet
      // extra data, the game's own (6) or shared (7): media, then the 64-bit id
      const std::filesystem::path root = extdata_root(c[1] == 7, path[1] | uint64_t{path[2]} << 32);
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
      if (c[2] == 3) { // SelfNCCH, the game itself: its 12-byte binary file path as OpenFile's below
        if (c[5] != 2 || c[6] != 12) {
          std::printf("[host] fs: SelfNCCH opened with file path type %u size %u (%08x %08x %08x %08x %08x), archive "
                      "path type %u size %u\n", c[5], c[6], file_path[0], file_path[1], file_path[2], file_path[3],
                      file_path[4], c[3], c[4]);
          return false;
        }
        char exefs_name[9] = {};
        std::memcpy(exefs_name, file_path + 1, 8);
        HostFile rom;
        if (!section_in_rom(file_path[0], exefs_name, rom)) {
          std::printf("[host] fs: SelfNCCH section %08x (%08x %08x) not in the ROM\n", file_path[0], file_path[1],
                      file_path[2]);
          return false;
        }
        const uint32_t id = next_file_++;
        files_[id] = rom;
        auto session = std::make_shared<Session>(Object::Kind::Session, "FSFile");
        session->sub = id;
        c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0x10, c[3] = kernel.add_handle(session);
        return true;
      }
      // the console's system titles (NCCH archive, NAND, a RomFS): the stand-ins Zakuro
      // gives: the Mii data an empty CFL_Res.dat (titles start, with no Mii to draw);
      // shortcut: the region manifest and the bad word list come as empty files, build them if a game reads them
      // the media type is the low byte (titles leave the rest unset)
      // a title's add-on content (0004008C) not installed: not found, as for a player without the DLC (Smash
      // looks for it at its start and goes on without)
      if (c[2] == 0x2345678A && archive_path[1] == 0x0004008C) {
        c[0] = header(command, 1, 0), c[1] = 0xC8804478; // FS: not found
        return true;
      }
      if (c[2] != 0x2345678A || (archive_path[2] & 0xFF) != 0 || file_path[0] != 0) {
        std::printf("[host] fs: a system file not native yet: archive %08x path %08x %08x %08x %08x, file %08x %08x %08x\n",
                    c[2], archive_path[0], archive_path[1], archive_path[2], archive_path[3], file_path[0], file_path[1],
                    file_path[2]);
        return false;
      }
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
    case 0x0809: // CreateDirectory(transaction, archive, path type, path size, attributes, <path>)
    case 0x0806: // DeleteDirectory(transaction, archive, path type, path size, <path>)
    case 0x0807: { // DeleteDirectoryRecursively
      const auto found = archives_.find(c[2] | uint64_t{c[3]} << 32);
      if (found == archives_.end() || found->second == kSelfNcch) return false;
      const std::filesystem::path path = found->second / read_path(kernel, c[4], c[5], command == 0x0809 ? c[8] : c[7]);
      std::error_code error;
      bool done;
      if (command == 0x0809) done = !std::filesystem::exists(path, error) && std::filesystem::create_directory(path, error);
      else if (command == 0x0806) done = std::filesystem::remove(path, error);
      else done = std::filesystem::remove_all(path, error) > 0;
      if (!done) {
        c[0] = header(command, 1, 0), c[1] = command == 0x0809 ? 0xC82044BE : 0xC8804471; // exists / not found
        return true;
      }
      reply(c, command);
      return true;
    }
    case 0x0805: // RenameFile(transaction, archive, path type, size, archive, path type, size, <path>, <path>)
    case 0x080A: { // RenameDirectory
      const auto from = archives_.find(c[2] | uint64_t{c[3]} << 32), to = archives_.find(c[6] | uint64_t{c[7]} << 32);
      if (from == archives_.end() || to == archives_.end() || from->second == kSelfNcch) return false;
      std::error_code error;
      std::filesystem::rename(from->second / read_path(kernel, c[4], c[5], c[11]),
                              to->second / read_path(kernel, c[8], c[9], c[13]), error);
      if (error) {
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
    case 0x080B: { // OpenDirectory(archive, path type, path size, <path>)
      const auto found = archives_.find(c[1] | uint64_t{c[2]} << 32);
      if (found == archives_.end() || found->second == kSelfNcch) return false;
      const std::filesystem::path path = found->second / read_path(kernel, c[3], c[4], c[6]);
      std::error_code ignored;
      if (!std::filesystem::is_directory(path, ignored)) {
        c[0] = header(command, 1, 0), c[1] = 0xC8804471; // path not found
        return true;
      }
      HostDirectory directory;
      for (const auto& entry : std::filesystem::directory_iterator(path, ignored)) directory.entries.push_back(entry);
      const uint32_t id = next_file_++;
      directories_[id] = std::move(directory);
      auto session = std::make_shared<Session>(Object::Kind::Session, "FSDir");
      session->sub = id;
      c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0x10, c[3] = kernel.add_handle(session);
      return true;
    }
    case 0x0845: { // GetFormatInfo(archive, path type, path size, <path>): the save data, or extra data (6, 7)
      if (c[1] != 4 && c[1] != 6 && c[1] != 7) return false;
      uint32_t path[3] = {};
      kernel.memory().read(c[5], path, std::min<uint32_t>(c[3], sizeof path));
      const std::filesystem::path root = c[1] == 4 ? save_root() : extdata_root(c[1] == 7, path[1] | uint64_t{path[2]} << 32);
      uint32_t info[4] = {};
      std::ifstream format(root.string() + ".format", std::ios::binary);
      std::error_code ignored;
      if (!format.read(reinterpret_cast<char*>(info), sizeof info) || !std::filesystem::is_directory(root, ignored)) {
        c[0] = header(command, 1, 0), c[1] = 0xC8A04554;
        return true;
      }
      reply(c, command, {info[0], info[1], info[2], info[3] & 0xFF}); // size, directories, files, duplicate
      return true;
    }
    case 0x084C:   // FormatSaveData(archive, path type, path size, blocks, directories, files, buckets x2, duplicate, <path>)
    case 0x080F: { // FormatThisUserSaveData(blocks, directories, files, buckets x2, duplicate): the same, its own
      if (command == 0x084C && c[1] != 4) return false;
      const uint32_t* a = command == 0x084C ? c + 4 : c + 1; // from the block count on
      const std::filesystem::path root = save_root();
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
      std::filesystem::create_directories(root, ignored);
      // as Zakuro keeps it: size in bytes, directories, files, duplicate
      const uint32_t info[4] = {a[0] * 512, a[1], a[2], a[5] & 0xFF};
      std::ofstream(root.string() + ".format", std::ios::binary).write(reinterpret_cast<const char*>(info), sizeof info);
      reply(c, command);
      return true;
    }
    case 0x0851: { // CreateExtSaveData(media, id low, id high, _, directories, files, size x2, icon size, <icon>)
      const std::filesystem::path root = extdata_root(false, c[2] | uint64_t{c[3]} << 32);
      std::error_code ignored;
      std::filesystem::create_directories(root, ignored);
      const uint32_t info[4] = {0, c[5], c[6], 0}; // the info is 4 words: media, the 64-bit id, one unknown
      std::ofstream(root.string() + ".format", std::ios::binary).write(reinterpret_cast<const char*>(info), sizeof info);
      reply(c, command);
      return true;
    }
    case 0x0852: { // DeleteExtSaveData(media, _, id low, id high, _)
      const std::filesystem::path root = extdata_root(false, c[2] | uint64_t{c[3]} << 32);
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
      std::filesystem::remove(root.string() + ".format", ignored);
      reply(c, command);
      return true;
    }
    case 0x0863: reply(c, command, {fs_priority_}); return true;
    default: return false;
    }
  }
  // daemons and the microphone: nothing to do offline, without a microphone (as Zakuro)
  if (service == "err:f" && command == 0x0001) { // ThrowFatalError(type, result, pc, process, title, app title, ...)
    char why[160];
    std::snprintf(why, sizeof why, "the game threw a fatal error: type %u, result %08x, at %08x", c[1] & 0xFF, c[2], c[3]);
    kernel.stop(why);
    reply(c, command);
    return true;
  }
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
    auto notification = std::make_shared<Event>();
    apt_parameter_event_ = std::make_shared<Event>();
    const uint32_t n = kernel.add_handle(notification), p = kernel.add_handle(apt_parameter_event_);
    post_parameter(kernel, {0, 1}); // a title starts by being handed a Wakeup parameter
    c[0] = header(0x0002, 1, 3), c[1] = kOk, c[2] = 0x04000000, c[3] = n, c[4] = p; // two handles
    return true;
  }
  case 0x000C: { // SendParameter(source, destination, signal, size, handle descriptor, handle, static descriptor, buffer)
    const uint32_t destination = c[2], signal = c[3];
    // to a library applet: its Request (the capture buffer's size) gets the applet's Response, with the
    // memory it would draw into; the applet itself starts with StartLibraryApplet
    if (destination >= 0x400 && destination < 0x500 && signal == 2) {
      uint32_t size = 0;
      if (c[4] >= 4) size = kernel.memory().read32(c[8]);
      auto block = std::make_shared<SharedMemory>();
      block->size = std::max<uint32_t>((size + 0xFFF) & ~0xFFFu, 0x1000);
      block->physical = kernel.allocate(Kernel::kBase, block->size, false);
      if (block->physical) std::memset(kernel.memory().physical(block->physical, block->size), 0, block->size);
      // the Response's buffer is the memory's size: the title maps that much (none: a mapping of 0 bytes fails and
      // Majora's Mask then captured its screen to address 0)
      std::vector<uint8_t> told(4);
      std::memcpy(told.data(), &block->size, 4);
      post_parameter(kernel, {destination, 3, std::move(told), block->physical ? kernel.add_handle(block) : 0});
    }
    reply(c, command);
    return true;
  }
  case 0x001E: { // StartLibraryApplet(applet, size, handle descriptor, handle, static descriptor, buffer)
    std::vector<uint8_t> config(std::min<uint32_t>(c[2], 0x10000));
    kernel.memory().read(c[6], config.data(), static_cast<uint32_t>(config.size()));
    const uint32_t applet = c[1], memory = c[4];
    std::printf("[apt] library applet %03x started (%zu bytes of settings): it answers at once\n", applet, config.size());
    // the applet closes: the title wakes with what it handed back (WakeupByExit)
    post_parameter(kernel, {applet, 0xA, run_applet(kernel, applet, std::move(config), memory), 0});
    reply(c, command);
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
  case 0x000D:                                                          // ReceiveParameter
  case 0x000E: {                                                        // GlanceParameter
    if (!has_parameter_) {
      c[0] = header(command, 1, 0), c[1] = 0xC8A0CFFC; // no parameter waiting
      return true;
    }
    uint32_t address, capacity;
    static_buffer(kernel, thread, 0, address, capacity);
    const uint32_t size = std::min({static_cast<uint32_t>(parameter_.buffer.size()), c[2], capacity});
    if (size) kernel.memory().write(address, parameter_.buffer.data(), size);
    c[0] = header(command, 4, 4), c[1] = kOk, c[2] = parameter_.sender, c[3] = parameter_.signal, c[4] = size;
    // the handle is moved to the title: a new one at each glance or receive (it may close one before the next)
    uint32_t handed = 0;
    if (parameter_.handle)
      if (auto object = kernel.resolve(parameter_.handle)) handed = kernel.add_handle(object);
    c[5] = 0x10, c[6] = handed, c[7] = static_descriptor(size, 0), c[8] = address;
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
  case 0x0044: { // GetSharedFont -> where the font's block is, and the block
    if (!shared_font_ && !load_shared_font(kernel)) {
      c[0] = header(0x0044, 1, 0), c[1] = 0xFFFFFFFF; // no font to hand over: what a console without one says
      return true;
    }
    c[0] = header(0x0044, 2, 2), c[1] = kOk, c[2] = shared_font_->own_address, c[3] = 0;
    c[4] = kernel.add_handle(shared_font_);
    return true;
  }
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

bool Services::load_shared_font(Kernel& kernel) {
  const char* path = std::getenv("CTRN_SHARED_FONT");
  std::vector<uint8_t> font;
  if (path) {
    std::ifstream in(path, std::ios::binary);
    font.assign(std::istreambuf_iterator<char>(in), {});
  }
  if (font.size() <= 0x84 || (std::memcmp(font.data() + 0x80, "CFNU", 4) && std::memcmp(font.data() + 0x80, "CFNT", 4))) {
    std::printf("[host] APT: no shared font (CTRN_SHARED_FONT: %s)\n", path ? path : "not set");
    return false;
  }
  auto get = [&](uint32_t at) { uint32_t v = 0; if (at + 4 <= font.size()) std::memcpy(&v, font.data() + at, 4); return v; };
  auto put = [&](uint32_t at, uint32_t v) { if (at + 4 <= font.size()) std::memcpy(font.data() + at, &v, 4); };
  const uint32_t loaded = 2; // a dump may have been taken before the system finished loading it
  put(0, loaded);
  const uint32_t size = (static_cast<uint32_t>(font.size()) + 0xFFF) & ~0xFFFu;
  const uint32_t physical = kernel.allocate(Kernel::kBase, size, false);
  if (!physical) {
    std::printf("[host] APT: no memory for the shared font\n");
    return false;
  }
  // among the system's memory where the linear mapping shows it: a title's own linear heap never reaches there
  const uint32_t address = kernel.linear_base() + (physical - Memory::kFcram);
  // the font's pointers are absolute: FINF's to TGLP says where it was laid out for (Zakuro's relocate)
  uint16_t header_size = 0;
  std::memcpy(&header_size, font.data() + 0x86, 2);
  uint32_t finf = 0, tglp = 0;
  for (uint32_t at = 0x80 + header_size, n = 0; n < std::min(get(0x90), 16u) && at + 8 <= font.size(); ++n) {
    if (!std::memcmp(font.data() + at, "FINF", 4) && !finf) finf = at;
    if (!std::memcmp(font.data() + at, "TGLP", 4) && !tglp) tglp = at;
    if (!get(at + 4) || (finf && tglp)) break;
    at += get(at + 4);
  }
  if (finf && tglp) {
    const uint32_t laid_out = get(finf + 16) - (tglp + 8), delta = address - laid_out;
    auto moved = [&](uint32_t at) { put(at, get(at) + delta); return get(at); };
    for (uint32_t f : {16u, 20u, 24u}) moved(finf + f);
    moved(tglp + 28);
    // the widths and the maps, each section pointing on to the next one's body
    for (auto [first, link] : {std::pair{finf + 20, 4u}, std::pair{finf + 24, 8u}}) {
      uint32_t body = get(first);
      for (int i = 0; i < 256 && body - address + link + 4 <= font.size() && get(body - address + link); ++i)
        body = moved(body - address + link);
    }
  }
  std::memcpy(kernel.memory().physical(physical, size), font.data(), font.size());
  kernel.memory().map(address, physical, size, false);
  shared_font_ = std::make_shared<SharedMemory>();
  shared_font_->physical = physical, shared_font_->size = size, shared_font_->own_address = address;
  std::printf("[host] APT: the shared font (%s, %zu KiB) at %08x\n", path, font.size() / 1024, address);
  return true;
}

// ldr:ro, as Zakuro does it (services/ldr_ro.rs)
bool Services::ldr_ro(Kernel& kernel, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  Memory& m = kernel.memory();
  if (!cro_) cro_ = std::make_unique<Cro>(m);
  constexpr uint32_t kNotLoaded = 0xD9012402;
  auto pages = [](uint32_t size) { return (size + 0xFFF) & ~0xFFFu; };
  // the buffer visible at address as well, the same memory; where the module is then
  auto mirror = [&](uint32_t address, uint32_t source, uint32_t size) {
    if (!address || address == source || (address | source) & 0xFFF) return source;
    for (uint32_t off = 0; off < pages(size); off += 0x1000)
      if (!m.to_physical(source + off)) return source;
    for (uint32_t off = 0; off < pages(size); off += 0x1000) m.map(address + off, m.to_physical(source + off), 0x1000, true);
    return address;
  };
  switch (command) {
  case 0x0001: { // Initialize(CRS buffer, size, address, <process>)
    const uint32_t base = mirror(c[3], c[1], c[2]);
    if (!cro_->initialize(base, c[2])) {
      c[0] = header(command, 1, 0), c[1] = kNotLoaded;
      return true;
    }
    reply(c, command);
    return true;
  }
  case 0x0002: case 0x0003: reply(c, command); return true; // LoadCRR, UnloadCRR: signatures, not checked
  case 0x0004:   // LoadCRO(buffer, address, size, data, 0, data size, bss, bss size, auto link, fix level, CRR, <process>)
  case 0x0009: { // LoadCRO_New: the same
    const uint32_t source = c[1], address = c[2], size = c[3], data = c[4], data_size = c[6], bss = c[7];
    const uint32_t bss_size = c[8], fix_level = c[10];
    const bool auto_link = (c[9] & 0xFF) != 0;
    // the module as the game hands it, before ldr:ro links it: kept to be translated if it has no code yet
    std::vector<uint8_t> original(size);
    m.read(source, original.data(), size);
    uint32_t base = source;
    if (address && address != source && !(address & 0xFFF)) {
      if (const uint32_t physical = kernel.allocate(Kernel::kSystem, pages(size), false)) {
        std::vector<uint8_t> bytes(size);
        m.read(source, bytes.data(), size);
        m.map(address, physical, pages(size), true);
        m.write_any(address, bytes.data(), size);
        cro_copies_[address] = {source, physical, pages(size), size};
        base = address;
      } else {
        std::printf("[host] ldr:ro: no memory for a %x-byte module, it runs from its buffer\n", size);
        base = mirror(address, source, size);
      }
    }
    uint32_t fixed = 0;
    if (!cro_->load(base, size, data, data_size, bss, bss_size, auto_link, fix_level, fixed)) {
      std::printf("[host] ldr:ro: the buffer at %08x is not a module that loads\n", source);
      if (auto copy = cro_copies_.find(base); copy != cro_copies_.end()) {
        m.unmap(base, copy->second.size), kernel.release(copy->second.physical, copy->second.size);
        cro_copies_.erase(copy);
      }
      c[0] = header(command, 1, 0), c[1] = kNotLoaded;
      return true;
    }
    if (auto found = cro_copies_.find(base); found != cro_copies_.end()) {
      CroCopy& copy = found->second;
      // the module as ldr:ro left it into the buffer too: the game copies its relocated data out of there
      std::vector<uint8_t> bytes(size);
      m.read(base, bytes.data(), size);
      m.write_any(copy.buffer, bytes.data(), size);
      // what fixing dropped goes back: the game maps the next module over it
      copy.fixed = fixed ? fixed : size;
      const uint32_t kept = pages(copy.fixed);
      if (kept < copy.size) {
        m.unmap(base + kept, copy.size - kept);
        kernel.release(copy.physical + kept, copy.size - kept);
        copy.size = kept;
      }
    }
    const std::string& name = cro_->last().name;
    std::printf("[host] ldr:ro: module %s loaded at %08x\n", name.c_str(), base);
    if (!module || !module->place(name, base)) {
      std::printf("[host] ldr:ro: %s loaded at %08x has no translated code: it cannot run\n", name.c_str(), base);
      // a module the RomFS has not as a .cro file (packed, unpacked by the game): saved as the game handed it, the
      // launcher translates it with the others (3dsrecomp --modules) and starts the game again
      if (const char* dir = std::getenv("CTRN_MODULES_DIR")) {
        const std::filesystem::path path = std::filesystem::path(dir) / (name + ".cro");
        std::error_code ignored;
        if (!std::filesystem::exists(path, ignored)) {
          std::filesystem::create_directories(dir, ignored);
          std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(original.data()),
                                                      static_cast<std::streamsize>(original.size()));
          kernel.stop("module " + name + " saved for translation");
        }
      }
    }
    static const bool log = std::getenv("CTRN_LOG_CRO") != nullptr;
    if (log) std::printf("[host] ldr:ro: %s at %08x, %x bytes, fixed to %x\n", name.c_str(), base, size, fixed);
    static std::set<std::string> said;
    for (const std::string& missing : cro_->unresolved())
      if (said.insert(missing).second) std::printf("[host] ldr:ro: nothing exports %s yet\n", missing.c_str());
    reply(c, command, {fixed});
    return true;
  }
  case 0x0005: { // UnloadCRO(address, 0, buffer, <process>)
    const uint32_t base = c[1];
    if (const Cro::Module* gone = cro_->module_at(base); gone && module) module->place(gone->name, 0);
    cro_->unload(base);
    if (auto found = cro_copies_.find(base); found != cro_copies_.end()) {
      // back into the buffer, what fixing left of it, as ldr:ro left it; its memory goes
      const CroCopy copy = found->second;
      std::vector<uint8_t> bytes(std::min(copy.fixed, copy.size));
      m.read(base, bytes.data(), static_cast<uint32_t>(bytes.size()));
      m.write_any(copy.buffer, bytes.data(), static_cast<uint32_t>(bytes.size()));
      m.unmap(base, copy.size);
      kernel.release(copy.physical, copy.size);
      cro_copies_.erase(found);
    }
    reply(c, command);
    return true;
  }
  case 0x0006: case 0x0007: case 0x0008: reply(c, command); return true; // linked as it loads; Shutdown
  default: return false;
  }
}

bool Services::answer_neutrally(const std::string& service, uint32_t* c) {
  static const bool strict = std::getenv("CTRN_STRICT") != nullptr;
  static const char* const core[] = {"fs:", "FS", "gsp::", "dsp::", "APT:", "srv:", "hid:", "y2r:", "ldr:", "csnd:", "err:"};
  if (strict) return false;
  for (const char* prefix : core)
    if (service.rfind(prefix, 0) == 0) return false;
  const uint32_t command = c[0] >> 16;
  static std::set<std::pair<std::string, uint32_t>> said;
  if (said.insert({service, command}).second)
    std::printf("[host] %s %04x: not native yet, answered neutrally (success, zeros)\n", service.c_str(), command);
  c[0] = header(command, 8, 0), c[1] = kOk;
  for (int i = 2; i < 9; ++i) c[i] = 0;
  return true;
}

void Services::post_parameter(Kernel& kernel, Parameter parameter) {
  parameter_ = std::move(parameter);
  has_parameter_ = true;
  if (apt_parameter_event_) kernel.signal(*apt_parameter_event_);
}

// the layouts are libctru's (SwkbdState, MiiSelectorReturn, errorConf)
std::vector<uint8_t> Services::run_applet(Kernel& kernel, uint32_t applet, std::vector<uint8_t> config, uint32_t memory) {
  auto put32 = [&](std::vector<uint8_t>& b, size_t at, uint32_t v) {
    if (at + 4 <= b.size()) std::memcpy(b.data() + at, &v, 4);
  };
  auto get32 = [&](size_t at) {
    uint32_t v = 0;
    if (at + 4 <= config.size()) std::memcpy(&v, config.data() + at, 4);
    return v;
  };
  switch (applet) {
  case 0x401: { // the software keyboard: the confirm button, with a name typed into the shared memory
    if (config.size() < 0x14C) break;
    auto block = kernel.resolve(memory);
    const bool numbers = get32(0) == 1;
    const uint32_t limit = static_cast<uint16_t>(get32(0x20));
    std::string text = numbers ? "0" : "Player";
    if (limit && text.size() > limit) text.resize(limit);
    if (block && block->kind == Object::Kind::SharedMemory) {
      auto& shared = static_cast<SharedMemory&>(*block);
      if (uint8_t* to = kernel.memory().physical(shared.physical, shared.size))
        for (size_t i = 0; i < text.size() && i * 2 + 2 < shared.size; ++i) to[i * 2] = uint8_t(text[i]), to[i * 2 + 1] = 0;
    } else {
      text.clear();
    }
    static const uint32_t confirm[] = {0, 2, 5}; // the rightmost button of 1, 2 or 3
    put32(config, 0x138, confirm[std::min<uint32_t>(get32(4), 2)]);
    put32(config, 0x144, 0);
    config[0x148] = uint8_t(text.size()), config[0x149] = 0;
    break;
  }
  case 0x402: { // the Mii selector: a standard Mii chosen ("none chosen" sends titles back to their menu)
    std::vector<uint8_t> result(0x84);
    put32(result, 8, 0xFFFFFFFF); // not a guest
    uint8_t* mii = result.data() + 0xC; // the 0x5C bytes of a Mii, as 3dbrew lays them out
    mii[0x00] = 3;                      // version
    mii[0x01] = 1;                      // copying allowed
    mii[0x03] = 3 << 4;                 // made on a 3DS
    const uint8_t system[8] = {0xD2, 0x85, 0xA3, 0xBE, 0x0A, 0xDB, 0xF8, 0xDA};
    std::memcpy(mii + 0x04, system, 8);
    put32(result, 0xC + 0x0C, 0x8A5B2C1Du); // a normal (not special) Mii
    const uint8_t mac[6] = {0x40, 0xF4, 0x07, 0x00, 0x00, 0x01};
    std::memcpy(mii + 0x10, mac, 6);
    const char name[] = "Player";
    for (size_t i = 0; i + 1 < sizeof name; ++i) mii[0x1A + i * 2] = uint8_t(name[i]), mii[0x48 + i * 2] = uint8_t(name[i]);
    mii[0x2E] = 0x40, mii[0x2F] = 0x40;    // height, build
    mii[0x32] = 0x21, mii[0x33] = 0x01;    // hair
    put32(result, 0xC + 0x34, 0x02684418); // eyes
    put32(result, 0xC + 0x38, 0x26344614); // eyebrows
    mii[0x3C] = 0x12, mii[0x3D] = 0x81;    // nose
    mii[0x3E] = 0x68, mii[0x3F] = 0x17;    // mouth
    mii[0x40] = 0x00, mii[0x41] = 0x0D;
    mii[0x42] = 0x29, mii[0x44] = 0x52;    // beard, glasses
    mii[0x46] = 0x50, mii[0x47] = 0x48;    // mole
    // CRC-16/CCITT (0x1021, from 0) of the Mii and the 2 bytes after it, stored big-endian
    uint16_t crc = 0;
    for (size_t i = 0; i < 0x5E; ++i) {
      crc ^= uint16_t(mii[i] << 8);
      for (int b = 0; b < 8; ++b) crc = crc & 0x8000 ? uint16_t(crc << 1 ^ 0x1021) : uint16_t(crc << 1);
    }
    result[0x6A] = uint8_t(crc >> 8), result[0x6B] = uint8_t(crc);
    return result;
  }
  case 0x407: // the error display and the EULA: acknowledged
    put32(config, 0xEFC, 1);
    break;
  default: break; // others: their settings back, unchanged
  }
  return config;
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
    if (!hid_shared_) {
      auto block = std::make_shared<SharedMemory>();
      block->size = 0x1000;
      block->physical = hid_physical_ = kernel.allocate(Kernel::kBase, 0x1000, false);
      std::memset(kernel.memory().physical(block->physical, 0x1000), 0, 0x1000);
      hid_shared_ = block;
      for (int i = 0; i < 5; ++i) hid_events_[i] = std::make_shared<Event>();
    }
    // new handles to the same block and events each time: a game may have closed the last ones
    c[0] = header(command, 1, 7), c[1] = kOk, c[2] = 5u << 26, c[3] = kernel.add_handle(hid_shared_); // six handles
    for (int i = 0; i < 5; ++i) c[4 + i] = kernel.add_handle(hid_events_[i]);
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

// a directory the service opened: Read gives entries as the console lays them out (0x228 bytes:
// the UTF-16 name, the 8.3 short name and extension, the attributes, the size)
bool Services::fs_directory(Kernel& kernel, uint32_t id, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  const auto found = directories_.find(id);
  if (found == directories_.end()) return false;
  HostDirectory& directory = found->second;
  switch (command) {
  case 0x0801: { // Read(count, <buffer>) -> how many
    const uint32_t count = c[1], to = c[3];
    uint32_t done = 0;
    for (; done < count && directory.next < directory.entries.size(); ++done, ++directory.next) {
      const auto& entry = directory.entries[directory.next];
      uint8_t e[0x228] = {};
      const std::u16string name = entry.path().filename().u16string();
      for (size_t i = 0; i < name.size() && i < 0x105; ++i) std::memcpy(e + 2 * i, &name[i], 2);
      const std::string narrow = entry.path().stem().string(), extension = entry.path().extension().string();
      for (size_t i = 0; i < 8 && i < narrow.size(); ++i) e[0x20C + i] = static_cast<uint8_t>(std::toupper(narrow[i]));
      for (size_t i = 1; i < 4 && i < extension.size(); ++i) e[0x215 + i - 1] = static_cast<uint8_t>(std::toupper(extension[i]));
      e[0x219] = 1;
      std::error_code ignored;
      const bool folder = entry.is_directory(ignored);
      e[0x21B] = folder;
      e[0x21D] = !folder; // the archive attribute, as the console sets it on files
      const uint64_t size = folder ? 0 : entry.file_size(ignored);
      std::memcpy(e + 0x220, &size, 8);
      kernel.memory().write(to + done * 0x228, e, sizeof e);
    }
    c[0] = header(command, 2, 2), c[1] = kOk, c[2] = done, c[3] = (count * 0x228) << 4 | 0xC, c[4] = to;
    return true;
  }
  case 0x0802: directories_.erase(found), reply(c, command); return true; // Close
  default: return false;
  }
}

// a file of an archive the service opened, on the host
bool Services::fs_file(Kernel& kernel, uint32_t id, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  if (command == 0x080C) { // OpenLinkFile: another session on the same file
    const uint32_t link = next_file_++;
    if (auto memory = memory_files_.find(id); memory != memory_files_.end()) memory_files_[link] = memory->second;
    else if (auto file = files_.find(id); file != files_.end()) files_[link] = file->second;
    else return false;
    auto session = std::make_shared<Session>(Object::Kind::Session, "FSFile");
    session->sub = link;
    c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0x10, c[3] = kernel.add_handle(session);
    return true;
  }
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
// level 3 (kind 0) or an ExeFS file by name (kind 2)
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
  // 5: the update's RomFS (a patch title installed apart); a cartridge's revision has none, its own RomFS is the
  // game's current data (Smash Rev 5 asks for it first)
  if (kind != 0 && kind != 5) return false;
  // a mod's RomFS (CTRN_ROMFS, built by ctrn.py): the game's files with the mod's in place, a level 3 of its own
  if (const char* mod = std::getenv("CTRN_ROMFS")) {
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(mod, error);
    if (!error) {
      out = {mod, 0, size, true};
      return true;
    }
  }
  const uint64_t ivfc = ncch + uint64_t{u32(0x1B0)} * 0x200, size = uint64_t{u32(0x1B4)} * 0x200;
  if (!in.seekg(static_cast<std::streamoff>(ivfc)).read(reinterpret_cast<char*>(h), 0x60) || std::memcmp(h, "IVFC", 4)) return false;
  const uint32_t master = u32(0x08), log2 = u32(0x4C);
  if (log2 > 31) return false;
  const uint64_t block = 1ull << log2, level3 = (0x60 + master + block - 1) & ~(block - 1);
  out = {rom, ivfc + level3, size - level3, true};
  return true;
}

// HID's shared memory, as the console's HID module fills it (3dbrew, "HID Shared Memory"):
// the pad at 0x00 and the touch screen at 0xA8, each a ring of 8 entries the game reads
// from the latest index; the tick at the head changes each time the index comes back to 0
std::filesystem::path Services::save_root() const {
  const char* user = std::getenv("CTRN_USER_DIR");
  char name[17];
  std::snprintf(name, sizeof name, "%016llX", static_cast<unsigned long long>(program_id));
  return std::filesystem::path(user ? user : "user") / "savedata" / name;
}

std::filesystem::path Services::extdata_root(bool shared, uint64_t id) const {
  const char* user = std::getenv("CTRN_USER_DIR");
  char name[17];
  std::snprintf(name, sizeof name, "%016llX", static_cast<unsigned long long>(id));
  return std::filesystem::path(user ? user : "user") / "extdata" / (shared ? "shared" : "game") / name;
}

void Services::vblank(Kernel& kernel, const Input& input) {
  if (!hid_events_[0]) return;
  if (uint8_t* m = kernel.memory().physical(hid_physical_, 0x1000)) {
    auto put32 = [&](uint32_t at, uint32_t v) { std::memcpy(m + at, &v, 4); };
    auto put16 = [&](uint32_t at, int32_t v) { const uint16_t w = static_cast<uint16_t>(v); std::memcpy(m + at, &w, 2); };
    auto put64 = [&](uint32_t at, uint64_t v) { std::memcpy(m + at, &v, 8); };
    hid_index_ = (hid_index_ + 1) % 8;
    if (hid_index_ == 0) {
      const uint64_t now = kernel.ticks();
      put64(0x08, hid_reset_tick_), put64(0xB0, hid_reset_tick_); // the previous reset
      put64(0x00, now), put64(0xA8, now);
      hid_reset_tick_ = now;
    }
    // the circle pad: about 155 at full tilt, and the four directions as buttons
    const auto axis = [](float v) { return static_cast<int32_t>((v > 1 ? 1 : v < -1 ? -1 : v) * 155.0f); };
    const int32_t x = axis(input.circle_x), y = axis(input.circle_y);
    uint32_t buttons = input.buttons;
    if (x > 77) buttons |= 1u << 28;
    if (x < -77) buttons |= 1u << 29;
    if (y > 77) buttons |= 1u << 30;
    if (y < -77) buttons |= 1u << 31;
    put32(0x10, hid_index_);
    put32(0x1C, buttons); // the raw state
    put16(0x20, x), put16(0x22, y);
    const uint32_t pad = 0x28 + hid_index_ * 0x10;
    put32(pad, buttons), put32(pad + 4, buttons & ~hid_previous_), put32(pad + 8, ~buttons & hid_previous_);
    put16(pad + 12, x), put16(pad + 14, y);
    hid_previous_ = buttons;
    const uint32_t touch = 0xC8 + hid_index_ * 8;
    put32(0xB8, hid_index_);
    for (uint32_t at : {0xC0u, touch}) {
      put16(at, input.touching ? input.touch_x : 0), put16(at + 2, input.touching ? input.touch_y : 0);
      put32(at + 4, input.touching ? 1 : 0);
    }
  }
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
