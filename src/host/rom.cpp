#include "rom.hpp"

#include <cstring>
#include <fstream>

namespace host {

namespace {

uint32_t u32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

bool read_at(std::ifstream& in, uint64_t at, void* out, size_t size) {
  in.seekg(static_cast<std::streamoff>(at));
  return static_cast<bool>(in.read(static_cast<char*>(out), static_cast<std::streamsize>(size)));
}

} // namespace

// the code's footer: the compressed part's start (bottom byte of the first word, from the end)
// and end (its low 24 bits, from the end), then how much bigger the code is decompressed;
// the data decompresses backwards, in place, from the end of the grown buffer
bool decompress_code(std::vector<uint8_t>& data) {
  if (data.size() < 8) return false;
  const size_t size = data.size();
  const uint32_t bounds = u32(&data[size - 8]), extra = u32(&data[size - 4]);
  const size_t top = bounds >> 24, bottom = bounds & 0xFFFFFF;
  if (top > size || bottom > size || bottom < top) return false;
  data.resize(size + extra);
  size_t in = size - top, stop = size - bottom, out = data.size();
  while (in > stop) {
    uint8_t control = data[--in];
    for (int bit = 0; bit < 8 && in > stop; ++bit, control <<= 1) {
      if (control & 0x80) {
        if (in < 2) return false;
        in -= 2;
        const uint32_t pair = data[in] | data[in + 1] << 8;
        const size_t length = ((pair >> 12) & 0xF) + 3, distance = (pair & 0xFFF) + 2;
        if (out < length || out + distance >= data.size()) return false;
        // each byte from distance past the one just written (the copy may overlap what it writes)
        for (size_t i = 0; i < length; ++i, --out) data[out - 1] = data[out + distance];
      } else {
        if (out < 1) return false;
        data[--out] = data[--in];
      }
    }
  }
  return true;
}

bool Rom::load(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  uint8_t header[0x200];
  if (!in || !read_at(in, 0, header, sizeof header)) return error = "cannot read " + path, false;
  uint64_t ncch = 0;
  if (std::memcmp(header + 0x100, "NCSD", 4) == 0) {
    ncch = uint64_t{u32(header + 0x120)} * 0x200; // partition 0: the game
    if (!read_at(in, ncch, header, sizeof header)) return error = "truncated ROM", false;
  }
  if (std::memcmp(header + 0x100, "NCCH", 4) != 0) return error = "not a 3DS ROM (no NCSD or NCCH header)", false;
  if (!(header[0x18F] & 4)) return error = "the ROM is encrypted: decrypt it first (on the console, or with a decryption tool)", false;
  std::memcpy(&title_id, header + 0x108, 8);

  exheader.resize(0x800);
  if (u32(header + 0x180) < 0x400 || !read_at(in, ncch + 0x200, exheader.data(), exheader.size()))
    return error = "the ROM has no exheader (not an application)", false;
  const bool compressed = exheader[0x0D] & 1;
  title.assign(reinterpret_cast<const char*>(exheader.data()), strnlen(reinterpret_cast<const char*>(exheader.data()), 8));

  // the ExeFS: ten (name, offset, size) entries, the files after its 0x200-byte header
  const uint64_t exefs = ncch + uint64_t{u32(header + 0x1A0)} * 0x200;
  uint8_t table[0x200];
  if (!read_at(in, exefs, table, sizeof table)) return error = "truncated ExeFS", false;
  for (int i = 0; i < 10; ++i) {
    const char* name = reinterpret_cast<const char*>(table + i * 16);
    const uint32_t offset = u32(table + i * 16 + 8), size = u32(table + i * 16 + 12);
    if (std::strncmp(name, ".code", 8) == 0) {
      code.resize(size);
      if (!read_at(in, exefs + 0x200 + offset, code.data(), size)) return error = "truncated code", false;
      if (compressed && !decompress_code(code)) return error = "the code's compression is malformed", false;
    } else if (std::strncmp(name, "icon", 8) == 0 && size >= 0x2000) {
      // SMDH: per language 0x200 bytes from 0x8, the short title first (UTF-16); 1 is English
      uint16_t short_title[0x40] = {};
      if (read_at(in, exefs + 0x200 + offset + 0x8 + 0x200, short_title, sizeof short_title) && short_title[0]) {
        std::string utf8;
        for (uint16_t ch : short_title) {
          if (!ch) break;
          if (ch < 0x80) utf8 += static_cast<char>(ch);
          else if (ch < 0x800) utf8 += static_cast<char>(0xC0 | ch >> 6), utf8 += static_cast<char>(0x80 | (ch & 0x3F));
          else utf8 += static_cast<char>(0xE0 | ch >> 12), utf8 += static_cast<char>(0x80 | (ch >> 6 & 0x3F)),
               utf8 += static_cast<char>(0x80 | (ch & 0x3F));
        }
        title = utf8;
      }
    }
  }
  if (code.empty()) return error = "the ROM's ExeFS has no code", false;
  return true;
}

} // namespace host
