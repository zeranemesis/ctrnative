#include "cheats.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace host {

namespace {
// a variable as UTF-8 (Windows hands getenv the ANSI code page: an accented name would not match the file's)
std::string environment(const char* name) {
#if defined(_WIN32)
  wchar_t wide_name[64] = {};
  for (size_t i = 0; name[i] && i < 63; ++i) wide_name[i] = static_cast<wchar_t>(name[i]);
  const wchar_t* value = _wgetenv(wide_name);
  if (!value) return {};
  std::string out;
  for (const wchar_t* p = value; *p; ++p) { // UTF-16 to UTF-8, pairs included
    uint32_t c = *p;
    if (c >= 0xD800 && c < 0xDC00 && p[1] >= 0xDC00 && p[1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (*++p - 0xDC00);
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) out += static_cast<char>(0xC0 | c >> 6), out += static_cast<char>(0x80 | (c & 0x3F));
    else if (c < 0x10000)
      out += static_cast<char>(0xE0 | c >> 12), out += static_cast<char>(0x80 | (c >> 6 & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
    else
      out += static_cast<char>(0xF0 | c >> 18), out += static_cast<char>(0x80 | (c >> 12 & 0x3F)),
          out += static_cast<char>(0x80 | (c >> 6 & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
  }
  return out;
#else
  const char* value = std::getenv(name);
  return value ? value : "";
#endif
}

std::string trimmed(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}
} // namespace

Cheats::Cheats() {
  const char* path = std::getenv("CTRN_CHEATS");
  if (!path) return;
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "[cheats] no file at %s\n", path);
    return;
  }
  std::set<std::string> on;
  if (const std::string names = environment("CTRN_CHEATS_ON"); !names.empty())
    for (std::stringstream list(names); list.good();) {
      std::string name;
      std::getline(list, name, ';');
      if (!(name = trimmed(name)).empty()) on.insert(name);
    }
  for (std::string line; std::getline(in, line);) {
    line = trimmed(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      codes_.push_back({trimmed(line.substr(1, line.size() - 2)), {}, false});
      codes_.back().on = on.count(codes_.back().name) || on.count("*");
      continue;
    }
    unsigned a = 0, b = 0;
    if (!codes_.empty() && std::sscanf(line.c_str(), "%8x %8x", &a, &b) == 2) codes_.back().lines.emplace_back(a, b);
  }
  size_t running = 0;
  for (const Code& c : codes_) running += c.on;
  std::fprintf(stderr, "[cheats] %zu codes in %s, %zu on\n", codes_.size(), path, running);
}

std::vector<std::pair<std::string, bool>> Cheats::list() const {
  std::vector<std::pair<std::string, bool>> out;
  for (const Code& c : codes_) out.emplace_back(c.name, c.on);
  return out;
}

void Cheats::report() const {
  if (!std::getenv("CTRN_CHEATS_REPORT")) return;
  for (const Code& c : codes_)
    if (c.on)
      std::printf("[cheats] %s: %llu writes, %llu to unmapped memory, %llu into the code; conditions held %llu, failed %llu\n",
                  c.name.c_str(), static_cast<unsigned long long>(c.written), static_cast<unsigned long long>(c.unmapped),
                  static_cast<unsigned long long>(c.into_code), static_cast<unsigned long long>(c.held),
                  static_cast<unsigned long long>(c.failed));
  std::fflush(stdout);
}

void Cheats::run(Memory& memory, uint32_t keys) const {
  for (const Code& c : codes_)
    if (c.on) run(c, memory, keys);
}

// one code, as Gateway and Luma3DS run them: an offset and a data register; a condition that fails skips to its
// D0 (or a D2), conditions nesting; C starts a loop its D1 (or D2) ends
void Cheats::run(const Code& code, Memory& memory, uint32_t keys) const {
  uint32_t offset = 0, data = 0;
  int skip = 0;                               // the depth of failed conditions being skipped
  size_t loop_start = 0, loop_count = 0;      // the lines after the C code, its repeats left
  bool looping = false;
  auto read = [&](uint32_t address, uint32_t size) -> uint32_t {
    uint32_t value = 0;
    for (uint32_t k = 0; k < size; ++k)
      if (const uint8_t* b = memory.host(address + k)) value |= uint32_t{*b} << (8 * k);
    return value;
  };
  // a write: to data (the game's writable memory), into its code (read-only: the code runs translated, a code
  // patching it changes nothing), or nowhere (unmapped)
  auto write = [&](uint32_t address, uint32_t value, uint32_t size) {
    if (!memory.host(address)) {
      ++code.unmapped;
      return;
    }
    if (!memory.write_pages()[address >> 12]) {
      ++code.into_code;
      return;
    }
    ++code.written;
    for (uint32_t k = 0; k < size; ++k)
      if (memory.host(address + k)) memory.write(address + k, reinterpret_cast<const uint8_t*>(&value) + k, 1);
  };
  const auto& lines = code.lines;
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto [a, b] = lines[i];
    const uint32_t type = a >> 28, at = (a & 0x0FFFFFFF) + offset;
    if (skip) { // only what ends or nests a condition counts
      if ((type >= 3 && type <= 0xA) || (a >> 24) == 0xDD) ++skip;
      else if ((a >> 24) == 0xD0) --skip;
      else if ((a >> 24) == 0xD2) skip = 0, offset = data = 0, looping = false;
      continue;
    }
    const bool condition = (type >= 3 && type <= 0xA) || (a >> 24) == 0xDD;
    switch (type) {
    case 0x0: write(at, b, 4); break;
    case 0x1: write(at, b & 0xFFFF, 2); break;
    case 0x2: write(at, b & 0xFF, 1); break;
    case 0x3: if (!(b > read(at, 4))) skip = 1; break;
    case 0x4: if (!(b < read(at, 4))) skip = 1; break;
    case 0x5: if (!(b == read(at, 4))) skip = 1; break;
    case 0x6: if (!(b != read(at, 4))) skip = 1; break;
    case 0x7: case 0x8: case 0x9: case 0xA: {
      const uint32_t value = read(at, 2) & ~(b >> 16) & 0xFFFF, y = b & 0xFFFF;
      const bool pass = type == 0x7 ? y > value : type == 0x8 ? y < value : type == 0x9 ? y == value : y != value;
      if (!pass) skip = 1;
      break;
    }
    case 0xB: offset = read(at, 4); break;
    case 0xC: loop_start = i + 1, loop_count = b, looping = true; break;
    case 0xD:
      switch (a >> 24) {
      case 0xD0: break; // a condition's end, met when it held
      case 0xD1:
        if (looping && loop_count) --loop_count, i = loop_start - 1;
        else looping = false;
        break;
      case 0xD2:
        if (looping && loop_count) {
          --loop_count, i = loop_start - 1;
          break;
        }
        offset = data = 0, looping = false;
        break;
      case 0xD3: offset = b; break;
      case 0xD4: data += b; break;
      case 0xD5: data = b; break;
      case 0xD6: write(b + offset, data, 4), offset += 4; break;
      case 0xD7: write(b + offset, data & 0xFFFF, 2), offset += 2; break;
      case 0xD8: write(b + offset, data & 0xFF, 1), offset += 1; break;
      case 0xD9: data = read(b + offset, 4); break;
      case 0xDA: data = read(b + offset, 2); break;
      case 0xDB: data = read(b + offset, 1); break;
      case 0xDC: offset += b; break;
      case 0xDD: if ((keys & b) != b) skip = 1; break; // keys held
      default: break;
      }
      break;
    case 0xE: { // b bytes from the next lines (8 a line, low byte first) to at
      uint32_t left = b, put = at;
      while (left && i + 1 < lines.size()) {
        const auto [w0, w1] = lines[++i];
        const uint32_t words[2] = {w0, w1};
        for (uint32_t k = 0; k < 8 && left; ++k, --left) write(put++, words[k / 4] >> (8 * (k % 4)) & 0xFF, 1);
      }
      break;
    }
    case 0xF: // b bytes from the offset to a's address
      for (uint32_t k = 0; k < b; ++k) write((a & 0x0FFFFFFF) + k, read(offset + k, 1), 1);
      break;
    default: break;
    }
    if (condition) ++(skip ? code.failed : code.held);
  }
}

} // namespace host
