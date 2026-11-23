#pragma once

// The audio firmware's AAC decoder, which titles drive through the DSP's binary pipe (the third): a request says
// where ADTS frames are and where each channel's PCM goes, the answer how much came out. Pokémon X/Y and its
// successors play their music through it. Messages as Citra lays them out, answers as Zakuro (dsp_aac.rs) gives
// them; the decoding by the system's own decoder (Windows: Media Foundation), kept between requests, as each frame
// overlaps the one before.
#include "memory.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace host {

class Aac {
public:
  static constexpr size_t kMessage = 32;
  Aac();
  ~Aac();
  // a request written to the binary pipe; the answer to put back in it
  std::array<uint8_t, kMessage> answer(Memory& memory, const uint8_t* request, size_t size);

private:
  struct Decoder;
  std::unique_ptr<Decoder> decoder_;
};

} // namespace host
