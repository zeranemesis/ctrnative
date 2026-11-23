#pragma once

// A decrypted game ROM (.3ds/.cci: an NCSD holding the game's NCCH, or a bare
// .cxi NCCH): what the host needs from it to start the game.
#include <cstdint>
#include <string>
#include <vector>

namespace host {

struct Rom {
  std::vector<uint8_t> code;     // the ExeFS .code, decompressed
  std::vector<uint8_t> exheader; // 0x800 bytes: the system control info, then the access control info
  std::string title;             // the icon's English short title, else the exheader's name
  uint64_t title_id = 0;

  // false with the reason when the file is not a decrypted game ROM
  bool load(const std::string& path, std::string& error);
};

// the ExeFS code's compression (a backwards LZ, its footer at the end), in place; false if malformed
bool decompress_code(std::vector<uint8_t>& data);

} // namespace host
