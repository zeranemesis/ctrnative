#pragma once

// dsp::DSP, the audio DSP, natively: the audio firmware's HLE as Zakuro has
// it (services/dsp.rs, dsp_voices.rs, dsp_mixer.rs), 24 voices playing their
// buffers into three intermediate mixes, mixed down to what the speakers play.
#include "kernel.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace host {

class Dsp {
public:
  explicit Dsp(Kernel& kernel);
  ~Dsp();

  // a dsp::DSP request (kernel lock held); false when a command is not handled
  bool handle(Thread& thread, uint32_t* command);
  // one DSP audio frame, 160 samples (Zakuro's audio_frame: advance, then
  // signal_semaphore); kernel lock held
  void audio_frame();
  // the stereo samples produced since the last call, interleaved left/right
  // (Zakuro's take_audio), at 268111856 / 8192 Hz (32728 Hz)
  std::vector<int16_t> take_output();

private:
  struct State;
  bool write_pipe(uint32_t channel, const std::vector<uint8_t>& data);
  void advance();
  void signal_semaphore();

  Kernel& kernel_;
  std::unique_ptr<State> state_;
};

} // namespace host
