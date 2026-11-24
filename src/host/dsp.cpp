#include "dsp.hpp"
#include "aac.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

// Rust neither fuses multiplies into adds nor lets them be fused: the same
// samples as Zakuro need the same roundings (arm64 has FMA)
#ifdef __clang__
#pragma clang fp contract(off)
#endif

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

// Rust's float to integer `as`: saturating, NaN to 0
template <typename T, typename F> T saturate(F value) {
  if (value != value) return 0;
  if (value <= static_cast<F>(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
  if (value >= static_cast<F>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
  return static_cast<T>(value);
}

// i32 arithmetic wrapping, as Rust's in release
int32_t wrap(int64_t value) { return static_cast<int32_t>(value); }

uint8_t read8(Memory& m, uint32_t address) {
  uint8_t v = 0;
  m.read(address, &v, 1);
  return v;
}
uint16_t read16(Memory& m, uint32_t address) {
  uint16_t v = 0;
  m.read(address, &v, 2);
  return v;
}
void write8(Memory& m, uint32_t address, uint8_t v) { m.write(address, &v, 1); }
void write16(Memory& m, uint32_t address, uint16_t v) { m.write(address, &v, 2); }
// a 32-bit value the way the DSP stores it, high half first
uint32_t read_dsp32(Memory& m, uint32_t address) { return uint32_t{read16(m, address)} << 16 | read16(m, address + 2); }
void write_dsp32(Memory& m, uint32_t address, uint32_t value) {
  write16(m, address, static_cast<uint16_t>(value >> 16));
  write16(m, address + 2, static_cast<uint16_t>(value));
}
float read_float(Memory& m, uint32_t address) {
  const uint32_t bits = m.read32(address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// ---- dsp.rs ----

// dsp::DSP's commands
enum : uint32_t {
  kRecvData = 0x0001, kRecvDataIsReady = 0x0002, kSendData = 0x0003, kSendDataIsEmpty = 0x0004,
  kSendFifoEx = 0x0005, kRecvFifoEx = 0x0006, kSetSemaphore = 0x0007, kGetSemaphore = 0x0008,
  kClearSemaphore = 0x0009, kMaskSemaphore = 0x000A, kCheckSemaphoreRequest = 0x000B,
  kConvertProcessAddressFromDspDram = 0x000C, kWriteProcessPipe = 0x000D, kReadPipe = 0x000E,
  kGetPipeReadableSize = 0x000F, kReadPipeIfPossible = 0x0010, kLoadComponent = 0x0011,
  kUnloadComponent = 0x0012, kFlushDataCache = 0x0013, kInvalidateDataCache = 0x0014,
  kRegisterInterruptEvents = 0x0015, kGetSemaphoreEventHandle = 0x0016, kSetSemaphoreMask = 0x0017,
  kGetHeadphoneStatus = 0x001F, kForceHeadphoneOut = 0x0020, kGetIsDspOccupied = 0x0021,
};

constexpr uint32_t kPipeAudio = 2;  // the pipe a title drives the audio firmware through
constexpr uint32_t kPipeBinary = 3; // the pipe the AAC decoder takes requests on
constexpr uint32_t kPipeCount = 8;

// what the title writes to the audio pipe to change the firmware's state
enum : uint8_t { kStateInitialize, kStateShutdown, kStateWakeup, kStateSleep };

// addresses in DSP data memory of the structures the firmware exposes
constexpr uint16_t kFrameCounter = 0xBFFF, kSourceConfigurations = 0x9E92, kSourceStatuses = 0x8680,
                   kAdpcmCoefficients = 0xA792, kDspConfiguration = 0x9430, kDspStatus = 0x8400,
                   kFinalSamples = 0x8540, kIntermediateMixSamples = 0x9492, kCompressor = 0x8710,
                   kDspDebug = 0x8410, kUnknown10 = 0xA912, kUnknown11 = 0xAA12, kUnknown12 = 0xAAD2,
                   kUnknown13 = 0xAC52, kUnknown14 = 0xAC5C;

constexpr uint32_t kDspRamVirtual = 0x1FF00000;
// a DSP data-memory address as the ARM side sees it
uint32_t dsp_dram_to_arm(uint32_t address) { return kDspRamVirtual + (address << 1) + 0x40000; }

// the most output kept for a frontend not taking it, a second (in stereo pairs)
constexpr size_t kMaxOutput = 32768;

// ---- dsp_voices.rs ----

constexpr int kVoices = 24;
constexpr int kFrameSamples = 160; // samples in one audio frame
constexpr uint32_t kConfigSize = 192, kStatusSize = 12;
constexpr uint32_t kCoefficientsSize = 32;    // the 16 ADPCM coefficients each voice has
constexpr uint32_t kRegionStride = 0x20000;   // region 1 starts this far after region 0

using Sample = std::array<int16_t, 2>;
using Frame = std::array<Sample, kFrameSamples>; // one frame of stereo samples
using Channel = std::array<int32_t, kFrameSamples>;
// one frame of an intermediate mix: two left and two right channels, one after the other as the title sees it
using QuadFrame = std::array<Channel, 4>;
using Mixes = std::array<QuadFrame, 3>;
static_assert(sizeof(Frame) == kFrameSamples * 4 && sizeof(QuadFrame) == 4 * kFrameSamples * 4);

// how far through a frame each sample is, for gains fading over it
const std::array<float, kFrameSamples> kProgress = [] {
  std::array<float, kFrameSamples> p{};
  for (int i = 0; i < kFrameSamples; ++i) p[i] = static_cast<float>(i) / static_cast<float>(kFrameSamples - 1);
  return p;
}();

// bits of a voice configuration's dirty word: which fields the title changed
namespace dirty {
constexpr uint32_t kFormat = 1u << 0, kMonoOrStereo = 1u << 1, kAdpcmCoefficients = 1u << 2,
                   kPartialEmbeddedBuffer = 1u << 3, kPartialReset = 1u << 4, kEnable = 1u << 16,
                   kInterpolation = 1u << 17, kRateMultiplier = 1u << 18, kBufferQueue = 1u << 19,
                   kPlayPosition = 1u << 21, kFiltersEnabled = 1u << 22, kSimpleFilter = 1u << 23,
                   kBiquadFilter = 1u << 24,
                   kGain = 1u << 25, // one bit per intermediate mix from here up
                   kSyncCount = 1u << 28, kReset = 1u << 29, kEmbeddedBuffer = 1u << 30;
} // namespace dirty

// offsets into one voice's configuration
namespace config {
constexpr uint32_t kDirty = 0x00, kGain = 0x04, kRateMultiplier = 0x34, kInterpolation = 0x38,
                   kFiltersEnabled = 0x3A, kSimpleFilter = 0x3C, kBiquadFilter = 0x40, kBuffersDirty = 0x4A,
                   kBuffers = 0x4C, kBufferSize = 20, kEnable = 0xA0, kSyncCount = 0xA2, kPlayPosition = 0xA4,
                   kAddress = 0xAC, kLength = 0xB0, kFlags1 = 0xB4, kAdpcmYn = 0xB8, kFlags2 = 0xBC,
                   kBufferId = 0xBE;
} // namespace config

enum class Format { Pcm8, Pcm16, Adpcm };
// Polyphase: the firmware's polyphase filter, played as linear
enum class Interpolation { Polyphase, Linear, None };

struct Buffer {
  uint32_t address = 0, length = 0; // address: physical
  Format format = Format::Pcm16;
  bool stereo = false, looping = false;
  uint16_t id = 0;
  std::optional<std::array<int16_t, 2>> adpcm; // the ADPCM history to start from, when the title set it
  bool from_queue = false;                     // queued buffers report when they start, the embedded one does not
  uint32_t play_position = 0;
  bool has_played = false;
};

// y = b0 x + a1 y1, 15 fraction bits
struct SimpleFilter {
  int32_t b0 = 1 << 15, a1 = 0;
  int32_t y1[2] = {};
};
// second order, 14 fraction bits, feedback negated
struct BiquadFilter {
  int32_t b[3] = {1 << 14, 0, 0}, a[2] = {};
  int32_t x[2][2] = {}, y[2][2] = {};
};

// what an ADPCM buffer was decoded from (bytes, coefficients, starting history) and the history it left
struct AdpcmSource {
  std::vector<uint8_t> data;
  size_t count = 0;
  std::array<int16_t, 16> coefficients{};
  std::array<int16_t, 2> start{}, end{};
};

struct Filters {
  std::optional<SimpleFilter> simple;
  std::optional<BiquadFilter> biquad;

  void process(Sample* samples, int count) {
    for (int s = 0; s < count; ++s)
      for (int channel = 0; channel < 2; ++channel) {
        int32_t value = samples[s][channel];
        if (simple) {
          SimpleFilter& f = *simple;
          value = std::clamp(wrap(int64_t{f.b0} * value + int64_t{f.a1} * f.y1[channel]) >> 15, -32768, 32767);
          f.y1[channel] = value;
        }
        if (biquad) {
          BiquadFilter& f = *biquad;
          const int32_t x0 = value;
          value = std::clamp(wrap(int64_t{f.b[0]} * x0 + int64_t{f.b[1]} * f.x[0][channel] +
                                  int64_t{f.b[2]} * f.x[1][channel] + int64_t{f.a[0]} * f.y[0][channel] +
                                  int64_t{f.a[1]} * f.y[1][channel]) >>
                                 14,
                             -32768, 32767);
          f.x[1][channel] = f.x[0][channel];
          f.x[0][channel] = x0;
          f.y[1][channel] = f.y[0][channel];
          f.y[0][channel] = value;
        }
        samples[s][channel] = static_cast<int16_t>(value);
      }
  }
};

// GC ADPCM: frames of 8 bytes, a header and 14 samples of 4 bits, each predicted from the two before it
void decode_adpcm(const uint8_t* data, size_t size, size_t count, const std::array<int16_t, 16>& coefficients,
                  std::array<int16_t, 2>& history, std::vector<int16_t>& samples) {
  samples.clear();
  samples.reserve(count);
  int32_t yn1 = history[0], yn2 = history[1];
  for (size_t at = 0; at < size; at += 8) {
    const uint8_t* frame = data + at;
    const size_t length = std::min<size_t>(8, size - at);
    const int32_t scale = 1 << (frame[0] & 0xF);
    const int predictor = (frame[0] >> 4) & 7;
    const int32_t c1 = coefficients[predictor * 2], c2 = coefficients[predictor * 2 + 1];
    auto decode = [&](uint32_t nibble) {
      const int32_t xn = (static_cast<int32_t>(nibble << 28) >> 28) * scale; // the nibble is signed
      const int32_t value =
          std::clamp(wrap(int64_t{xn} * 2048 + 0x400 + int64_t{c1} * yn1 + int64_t{c2} * yn2) >> 11, -32768, 32767);
      yn2 = yn1;
      yn1 = value;
      return static_cast<int16_t>(value);
    };
    // shortcut: Zakuro's whole-frame fast path folded into this loop, same samples
    for (size_t i = 1; i < length; ++i)
      for (uint32_t nibble : {uint32_t{frame[i]} >> 4, uint32_t{frame[i]} & 0xFu}) {
        if (samples.size() == count) {
          history = {static_cast<int16_t>(yn1), static_cast<int16_t>(yn2)};
          return;
        }
        samples.push_back(decode(nibble));
      }
  }
  history = {static_cast<int16_t>(yn1), static_cast<int16_t>(yn2)};
}

// the sample a fraction of the way from one input to the next
Sample lerp(Sample x0, Sample x1, double fraction) {
  auto between = [&](int16_t a, int16_t b) {
    const double x = a, y = b;
    return saturate<int16_t>(x + (y - x) * fraction);
  };
  return {between(x0[0], x1[0]), between(x0[1], x1[1])};
}

int16_t le16(const uint8_t* p) { return static_cast<int16_t>(p[0] | p[1] << 8); }
int16_t pcm8(uint8_t byte) { return static_cast<int16_t>(static_cast<int8_t>(byte) * 256); }

// one sample of a PCM buffer read from memory by itself, both channels
Sample read_pcm(Memory& m, const Buffer& buffer, uint32_t index) {
  const uint32_t channels = buffer.stereo ? 2 : 1, at = buffer.address & ~3u;
  if (buffer.format == Format::Pcm8) {
    const uint8_t* data = m.physical(at + index * channels, channels);
    return data ? Sample{pcm8(data[0]), pcm8(data[channels - 1])} : Sample{};
  }
  const uint8_t* data = m.physical(at + index * channels * 2, channels * 2);
  return data ? Sample{le16(data), le16(data + channels * 2 - 2)} : Sample{};
}

// adds samples to a mix channel at a steady gain; out wraps as Rust's i32 does in release
// shortcut: Zakuro's unchecked quick path (|gain| <= 16384) is the same saturating cast, merged
void add(Channel& out, const std::array<float, kFrameSamples>& input, float gain) {
  for (int i = 0; i < kFrameSamples; ++i)
    out[i] = wrap(int64_t{out[i]} + saturate<int32_t>(gain * input[i]));
}

// adds samples to a mix channel at a gain going from one value to another over the frame
void fade(Channel& out, const std::array<float, kFrameSamples>& input, float from, float to) {
  const float change = to - from;
  for (int i = 0; i < kFrameSamples; ++i)
    out[i] = wrap(int64_t{out[i]} + saturate<int32_t>((from + change * kProgress[i]) * input[i]));
}

struct Voice {
  bool enabled = false;
  uint16_t sync_count = 0;
  double rate = 1.0;
  Format format = Format::Pcm16;
  bool stereo = false;
  Interpolation interpolation = Interpolation::Polyphase;
  Filters filters;
  std::array<std::array<float, 4>, 3> gain{}; // how loud in each channel of each intermediate mix
  std::array<std::optional<std::array<float, 4>>, 3> ramp; // gains a mix fades from over the next frame
  std::array<int16_t, 16> coefficients{};
  std::array<int16_t, 2> history{}; // the last two ADPCM samples, which the next follow on from
  std::vector<Buffer> queue;
  std::optional<Buffer> playing;
  std::vector<int16_t> decoded;             // an ADPCM buffer, decoded when it started
  std::optional<AdpcmSource> decoded_from;  // a looping buffer comes round again and is not decoded again
  uint32_t remaining = 0, length = 0;       // samples left in the buffer playing, and its length
  double fraction = 0.0;                    // input consumed but not yet a whole sample
  uint16_t current_buffer_id = 0, last_buffer_id = 0;
  bool buffer_update = false;               // a new queued buffer started, reported once
  uint32_t position = 0;                    // where in its buffer at the start of the frame
  Frame frame{};                            // what the voice played this frame
  // the buffer ran out on the frame's last sample: the next follows on from
  // the next frame's first, as on the console, not after a silent frame
  bool ran_out_at_frame_end = false;

  // starts the next buffer, the lowest id waiting
  bool dequeue(Memory& m) {
    if (queue.empty()) return false;
    size_t index = 0;
    for (size_t i = 1; i < queue.size(); ++i)
      if (queue[i].id < queue[index].id) index = i;
    const Buffer buffer = queue[index];
    if (buffer.looping) queue[index].has_played = true;
    else queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));

    // only the first time through starts at the play position
    const uint32_t start = buffer.has_played ? 0 : std::min(buffer.play_position, buffer.length);
    length = buffer.length;
    remaining = buffer.length - start;
    position = start;
    fraction = 0.0;
    current_buffer_id = buffer.id;
    last_buffer_id = 0;
    buffer_update = buffer.from_queue && !buffer.has_played;

    if (buffer.adpcm) history = *buffer.adpcm;
    if (buffer.format == Format::Adpcm) {
      const uint32_t bytes = (buffer.length / 14 + (buffer.length % 14 != 0)) * 8;
      if (const uint8_t* data = m.physical(buffer.address & ~3u, bytes)) decode(data, bytes, buffer.length);
      else forget_decoded(); // not in memory
    } else {
      forget_decoded();
    }
    playing = buffer;
    return true;
  }

  // decodes an ADPCM buffer on from the history, unless decoded holds it already
  void decode(const uint8_t* data, size_t size, size_t count) {
    const auto start = history;
    if (decoded_from && decoded_from->count == count && decoded_from->start == start &&
        decoded_from->coefficients == coefficients && decoded_from->data.size() == size &&
        std::equal(data, data + size, decoded_from->data.begin())) {
      history = decoded_from->end;
      return;
    }
    decode_adpcm(data, size, count, coefficients, history, decoded);
    decoded_from = AdpcmSource{std::vector<uint8_t>(data, data + size), count, coefficients, start, history};
  }

  void forget_decoded() {
    decoded.clear();
    decoded_from.reset();
  }

  // plays one audio frame's worth of input
  void play_frame(Memory& m) {
    frame = {};
    const bool follows_on = std::exchange(ran_out_at_frame_end, false) && !queue.empty();
    if (remaining == 0 && !follows_on) {
      if (dequeue(m)) return;
      // out of buffers: the voice switches itself off and reports the buffer
      // it finished as the last, none current; a title waits on that
      enabled = false;
      buffer_update = true;
      last_buffer_id = current_buffer_id;
      current_buffer_id = 0;
      position = 0;
      return;
    }
    position = length - remaining;
    int output = 0;
    while (output < kFrameSamples) {
      if (remaining == 0 && !dequeue(m)) break;
      output = play_buffer(m, output);
    }
    ran_out_at_frame_end = remaining == 0 && output == kFrameSamples;
    filters.process(frame.data(), output);
  }

  // plays the buffer at hand into the frame from output on (at least one
  // sample, until the frame is full or the buffer ran out); returns where the frame got to
  int play_buffer(Memory& m, int output) {
    if (!playing) return play_from(output, 0, [](uint32_t) { return Sample{}; });
    const Buffer buffer = *playing;
    const uint32_t last = buffer.length ? buffer.length - 1 : 0;
    if (buffer.format == Format::Adpcm)
      return play_from(output, last, [&](uint32_t index) {
        const int16_t s = index < decoded.size() ? decoded[index] : 0;
        return Sample{s, s};
      });
    const uint32_t size = buffer.format == Format::Pcm8 ? 1 : 2, channels = buffer.stereo ? 2 : 1;
    const uint64_t bytes = uint64_t{buffer.length} * size * channels;
    const uint8_t* data =
        buffer.length && bytes <= 0xFFFFFFFFu ? m.physical(buffer.address & ~3u, static_cast<uint32_t>(bytes)) : nullptr;
    if (!data) // not all of it in memory, each sample read as it comes
      return play_from(output, last, [&](uint32_t index) { return read_pcm(m, buffer, index); });
    if (buffer.format == Format::Pcm8)
      return buffer.stereo ? play_from(output, last, [&](uint32_t i) { return Sample{pcm8(data[i * 2]), pcm8(data[i * 2 + 1])}; })
                           : play_from(output, last, [&](uint32_t i) { const int16_t s = pcm8(data[i]); return Sample{s, s}; });
    return buffer.stereo ? play_from(output, last, [&](uint32_t i) { return Sample{le16(data + i * 4), le16(data + i * 4 + 2)}; })
                         : play_from(output, last, [&](uint32_t i) { const int16_t s = le16(data + i * 2); return Sample{s, s}; });
  }

  // plays input read by index (up to last) into the frame from output on
  template <typename Read> int play_from(int output, uint32_t last, Read read) {
    const bool fits = rate < static_cast<double>(1u << 31);
    for (;;) {
      if (fits && remaining > 0 && fraction >= 0.0 && fraction < 1.0) {
        // a whole rate from no fraction leaves none, nothing to interpolate
        const bool whole = fraction == 0.0 && rate == static_cast<double>(saturate<uint32_t>(rate));
        return interpolation == Interpolation::None || whole ? play_steady<false>(output, last, read)
                                                             : play_steady<true>(output, last, read);
      }
      // a sample the long way: a buffer that started spent, a fraction of one or more, a rate past u32
      const uint32_t at = length - remaining;
      const Sample x0 = read(std::min(at, last));
      frame[output] = interpolation == Interpolation::None || fraction == 0.0
                          ? x0
                          : lerp(x0, read(std::min(at + 1, last)), fraction);
      ++output;
      // each output sample consumes rate input samples
      const double input = fraction + rate;
      const uint32_t consumed = std::min(saturate<uint32_t>(input), remaining);
      fraction = std::max(input - static_cast<double>(consumed), 0.0);
      remaining -= consumed;
      if (output == kFrameSamples || remaining == 0) return output;
    }
  }

  // as play_from with the fraction below one: each output sample consumes the
  // whole of the rate or one more, the fraction left to a subtraction
  template <bool Lerp, typename Read> int play_steady(int output, uint32_t last, Read& read) {
    uint32_t left = remaining;
    double frac = fraction;
    const uint32_t whole = saturate<uint32_t>(rate);
    const double below = static_cast<double>(whole), above = static_cast<double>(whole) + 1.0;
    for (;;) {
      const uint32_t at = length - left;
      const Sample x0 = read(std::min(at, last));
      if constexpr (Lerp) frame[output] = lerp(x0, read(std::min(at + 1, last)), frac);
      else frame[output] = x0;
      ++output;
      const double input = frac + rate;
      const bool over = input >= above;
      const uint32_t consumed = whole + (over ? 1 : 0);
      if (consumed >= left) { // the buffer runs out with this sample
        const uint32_t taken = std::min(saturate<uint32_t>(input), left);
        frac = std::max(input - static_cast<double>(taken), 0.0);
        left -= taken;
        break;
      }
      frac = input - (over ? above : below);
      left -= consumed;
      if (output == kFrameSamples) break;
    }
    remaining = left;
    fraction = frac;
    return output;
  }

  // adds what the voice played to the intermediate mixes, each fading from
  // the gains it had when they just changed
  void mix_into(Mixes& mixes) {
    const auto ramps = std::exchange(ramp, {});
    if (!enabled) return;
    std::array<float, kFrameSamples> inputs[2];
    for (int i = 0; i < kFrameSamples; ++i) inputs[0][i] = frame[i][0], inputs[1][i] = frame[i][1];
    for (int mix = 0; mix < 3; ++mix)
      for (int channel = 0; channel < 4; ++channel) {
        const auto& input = inputs[channel & 1];
        const float g = gain[mix][channel];
        // a gain fading to where it was stays put
        if (ramps[mix] && (*ramps[mix])[channel] != g) fade(mixes[mix][channel], input, (*ramps[mix])[channel], g);
        else if (g != 0.0f) add(mixes[mix][channel], input, g);
      }
  }
};

struct VoiceLayout {
  uint32_t configurations, statuses, coefficients, frame_counter;
};

// which region the title wrote most recently, allowing for the frame counters wrapping
uint32_t current_region(Memory& m, uint32_t frame_counter) {
  const uint16_t first = read16(m, frame_counter), second = read16(m, frame_counter + kRegionStride);
  if (first == 0xFFFF && second != 0xFFFE) return 1;
  if (second == 0xFFFF && first != 0xFFFE) return 0;
  return first > second ? 0 : 1;
}

// applies what the title changed in a voice's configuration, then clears its dirty flags as the firmware does
void parse_config(Voice& voice, Memory& m, uint32_t base, uint32_t coefficients) {
  const uint32_t flags = m.read32(base + config::kDirty);
  if (!flags) return;
  if (flags & dirty::kReset) voice = Voice{};
  if (flags & dirty::kPartialReset) {
    voice.queue.clear();
    voice.ran_out_at_frame_end = false;
  }
  if (flags & dirty::kEnable) {
    voice.enabled = read8(m, base + config::kEnable) != 0;
    voice.ran_out_at_frame_end = false;
  }
  if (flags & dirty::kSyncCount) voice.sync_count = read16(m, base + config::kSyncCount);
  if (flags & dirty::kRateMultiplier) {
    const double rate = read_float(m, base + config::kRateMultiplier);
    voice.rate = rate > 0.0 && std::isfinite(rate) ? rate : 1.0;
  }
  if (flags & dirty::kInterpolation) {
    switch (read8(m, base + config::kInterpolation)) {
    case 1: voice.interpolation = Interpolation::Linear; break;
    case 2: voice.interpolation = Interpolation::None; break;
    default: voice.interpolation = Interpolation::Polyphase; break;
    }
  }
  if (flags & dirty::kAdpcmCoefficients)
    for (uint32_t i = 0; i < 16; ++i) voice.coefficients[i] = static_cast<int16_t>(read16(m, coefficients + i * 2));
  for (uint32_t mix = 0; mix < 3; ++mix)
    if (flags & (dirty::kGain << mix)) {
      voice.ramp[mix] = voice.gain[mix];
      for (uint32_t channel = 0; channel < 4; ++channel) {
        const float g = read_float(m, base + config::kGain + (mix * 4 + channel) * 4);
        voice.gain[mix][channel] = std::isfinite(g) ? g : 0.0f;
      }
    }
  if (flags & dirty::kFiltersEnabled) {
    const uint16_t enabled = read16(m, base + config::kFiltersEnabled);
    // a filter switched off forgets its state and coefficients
    if (!(enabled & 1)) voice.filters.simple.reset();
    else if (!voice.filters.simple) voice.filters.simple.emplace();
    if (!(enabled & 2)) voice.filters.biquad.reset();
    else if (!voice.filters.biquad) voice.filters.biquad.emplace();
  }
  auto half = [&](uint32_t offset) { return int32_t{static_cast<int16_t>(read16(m, base + offset))}; };
  if (flags & dirty::kSimpleFilter) {
    if (!voice.filters.simple) voice.filters.simple.emplace();
    voice.filters.simple->b0 = half(config::kSimpleFilter);
    voice.filters.simple->a1 = half(config::kSimpleFilter + 2);
  }
  if (flags & dirty::kBiquadFilter) {
    if (!voice.filters.biquad) voice.filters.biquad.emplace();
    BiquadFilter& f = *voice.filters.biquad;
    auto field = [&](uint32_t i) { return half(config::kBiquadFilter + i * 2); };
    f.a[0] = field(1), f.a[1] = field(0);
    f.b[0] = field(4), f.b[1] = field(3), f.b[2] = field(2);
  }
  // the embedded buffer brings its format along
  const uint16_t flags1 = read16(m, base + config::kFlags1);
  if (flags & (dirty::kFormat | dirty::kEmbeddedBuffer)) {
    switch ((flags1 >> 2) & 3) {
    case 0: voice.format = Format::Pcm8; break;
    case 2: voice.format = Format::Adpcm; break;
    default: voice.format = Format::Pcm16; break;
    }
  }
  if (flags & (dirty::kMonoOrStereo | dirty::kEmbeddedBuffer)) voice.stereo = (flags1 & 3) == 2;

  const uint32_t play_position = flags & dirty::kPlayPosition ? read_dsp32(m, base + config::kPlayPosition) : 0;

  // the title lengthened the buffer already playing
  if (flags & dirty::kPartialEmbeddedBuffer) {
    const uint32_t length = read_dsp32(m, base + config::kLength);
    const uint32_t played = voice.length - voice.remaining;
    voice.length = std::max(length, played);
    voice.remaining = voice.length - played;
    if (voice.playing) voice.playing->length = voice.length;
  }

  if (flags & dirty::kEmbeddedBuffer) {
    const uint16_t flags2 = read16(m, base + config::kFlags2);
    Buffer b;
    b.address = read_dsp32(m, base + config::kAddress);
    b.length = read_dsp32(m, base + config::kLength);
    b.format = voice.format;
    b.stereo = voice.stereo;
    b.looping = (flags2 & 2) != 0;
    b.id = read16(m, base + config::kBufferId);
    if (flags2 & 1)
      b.adpcm = std::array<int16_t, 2>{static_cast<int16_t>(read16(m, base + config::kAdpcmYn)),
                                       static_cast<int16_t>(read16(m, base + config::kAdpcmYn + 2))};
    b.from_queue = false;
    b.play_position = play_position;
    voice.queue.push_back(b);
  }

  if (flags & dirty::kBufferQueue) {
    const uint16_t queued = read16(m, base + config::kBuffersDirty);
    for (uint32_t slot = 0; slot < 4; ++slot) {
      if (!(queued & (1u << slot))) continue;
      const uint32_t at = base + config::kBuffers + slot * config::kBufferSize;
      const uint32_t length = read_dsp32(m, at + 4);
      if (!length) continue;
      Buffer b;
      b.address = read_dsp32(m, at);
      b.length = length;
      b.format = voice.format;
      b.stereo = voice.stereo;
      b.looping = read8(m, at + 15) != 0;
      b.id = read16(m, at + 16);
      if (read8(m, at + 14))
        b.adpcm = std::array<int16_t, 2>{static_cast<int16_t>(read16(m, at + 10)), static_cast<int16_t>(read16(m, at + 12))};
      b.from_queue = true;
      voice.queue.push_back(b);
    }
    write16(m, base + config::kBuffersDirty, 0);
  }
  m.write32(base + config::kDirty, 0);
}

void write_status(Voice& voice, Memory& m, uint32_t base) {
  write8(m, base, voice.enabled);
  write8(m, base + 1, voice.buffer_update);
  voice.buffer_update = false;
  write16(m, base + 2, voice.sync_count);
  write_dsp32(m, base + 4, voice.position);
  write16(m, base + 8, voice.current_buffer_id);
  write16(m, base + 10, voice.last_buffer_id);
}

// one audio frame: take in what the title changed, play, report, return the three intermediate mixes
Mixes tick_voices(std::array<Voice, kVoices>& voices, Memory& m, const VoiceLayout& layout) {
  const uint32_t read = current_region(m, layout.frame_counter), write = 1 - read;
  const uint32_t configurations = layout.configurations + read * kRegionStride;
  const uint32_t coefficients = layout.coefficients + read * kRegionStride;
  const uint32_t statuses = layout.statuses + write * kRegionStride;
  Mixes mixes{};
  for (uint32_t index = 0; index < kVoices; ++index) {
    Voice& voice = voices[index];
    parse_config(voice, m, configurations + index * kConfigSize, coefficients + index * kCoefficientsSize);
    if (voice.enabled) voice.play_frame(m);
    voice.mix_into(mixes);
    write_status(voice, m, statuses + index * kStatusSize);
  }
  return mixes;
}

// ---- dsp_mixer.rs ----

namespace mixer_dirty {
constexpr uint32_t kAuxBusEnable = 1u << 8;     // one bit per aux bus from here up
constexpr uint32_t kMasterVolume = 1u << 16;
constexpr uint32_t kAuxReturnVolume = 1u << 24; // one bit per aux bus from here up
constexpr uint32_t kOutputFormat = 1u << 26;
} // namespace mixer_dirty
namespace mixer_config {
constexpr uint32_t kDirty = 0x00, kMasterVolume = 0x04, kAuxReturnVolume = 0x08, kOutputFormat = 0x16,
                   kAuxBusEnable = 0x28;
}
// bytes of one intermediate mix a title can process: 4 channels of 32-bit samples
constexpr uint32_t kAuxMixSize = 4 * kFrameSamples * 4;

struct MixerLayout {
  uint32_t configuration, final_samples, intermediate_samples;
};

// adds a four channel mix to the stereo output at a volume
void downmix(Frame& frame, const QuadFrame& mix, float volume, bool stereo) {
  auto clamp = [](float value) { return std::clamp(saturate<int32_t>(value), -32768, 32767); };
  for (int s = 0; s < kFrameSamples; ++s) {
    const float a = static_cast<float>(mix[0][s]) * volume, b = static_cast<float>(mix[1][s]) * volume,
                c = static_cast<float>(mix[2][s]) * volume, d = static_cast<float>(mix[3][s]) * volume;
    int32_t left, right;
    if (stereo) {
      left = clamp(a + c), right = clamp(b + d);
    } else {
      left = right = clamp((a + b + c + d) / 2.0f);
    }
    frame[s][0] = static_cast<int16_t>(std::clamp(frame[s][0] + left, -32768, 32767));
    frame[s][1] = static_cast<int16_t>(std::clamp(frame[s][1] + right, -32768, 32767));
  }
}

struct Mixer {
  // how loud each intermediate mix comes out: the master volume, then the aux returns
  std::array<float, 3> volume{};
  std::array<bool, 2> aux{}; // whether mixes 1 and 2 go through the title first, for its effects
  bool stereo = true;
  Mixes mixes{}; // the mixes as they reach the output

  void parse_config(Memory& m, uint32_t base) {
    const uint32_t flags = m.read32(base + mixer_config::kDirty);
    if (!flags) return;
    auto read_volume = [&](uint32_t offset) {
      const float v = read_float(m, base + offset);
      return std::isfinite(v) ? v : 0.0f;
    };
    if (flags & mixer_dirty::kMasterVolume) volume[0] = read_volume(mixer_config::kMasterVolume);
    for (uint32_t i = 0; i < 2; ++i) {
      if (flags & (mixer_dirty::kAuxReturnVolume << i)) volume[i + 1] = read_volume(mixer_config::kAuxReturnVolume + i * 4);
      if (flags & (mixer_dirty::kAuxBusEnable << i)) aux[i] = read16(m, base + mixer_config::kAuxBusEnable + i * 2) != 0;
    }
    // surround comes out as stereo
    if (flags & mixer_dirty::kOutputFormat) stereo = read16(m, base + mixer_config::kOutputFormat) != 0;
    m.write32(base + mixer_config::kDirty, 0);
  }

  // mixes one frame from what the voices made, read being the region the title wrote last
  // shortcut: Zakuro copies whole mixes when every page is mapped and goes a word at a
  // time otherwise; Memory::read/write go a byte at a time, which only differs when the
  // DSP RAM is partly unmapped (it never is)
  Frame tick(Memory& m, const MixerLayout& layout, uint32_t read, const Mixes& made) {
    const uint32_t write = 1 - read;
    parse_config(m, layout.configuration + read * kRegionStride);
    // mixes 1 and 2 go out to the title, and what it made of the last ones comes back
    const uint32_t from = layout.intermediate_samples + read * kRegionStride;
    const uint32_t to = layout.intermediate_samples + write * kRegionStride;
    mixes[0] = made[0];
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t offset = i * kAuxMixSize;
      if (aux[i]) {
        m.read(from + offset, mixes[i + 1].data(), kAuxMixSize);
        m.write(to + offset, made[i + 1].data(), kAuxMixSize);
      } else {
        mixes[i + 1] = made[i + 1];
      }
    }
    Frame frame{};
    for (int i = 0; i < 3; ++i)
      if (volume[i] != 0.0f) downmix(frame, mixes[i], volume[i], stereo); // a mix at no volume adds nothing
    m.write(layout.final_samples + write * kRegionStride, frame.data(), sizeof frame);
    return frame;
  }
};

} // namespace

struct Dsp::State {
  bool component_loaded = false, running = false;
  // the event the firmware signals, kept however many handles to it the title opens and closes
  std::shared_ptr<Event> semaphore_event;
  std::vector<std::shared_ptr<Object>> interrupt_events; // registered for the DSP interrupts
  uint32_t semaphore_mask = 0, semaphore_value = 0;      // value: what GetSemaphore reports
  std::array<std::vector<uint8_t>, kPipeCount> pipes;    // data waiting to be read out of each pipe
  std::array<Voice, kVoices> voices;
  Mixer mixer;
  std::vector<int16_t> output; // what came out since the frontend last took it, interleaved
  Aac aac;                     // the binary pipe's AAC decoder

  void reset_pipes() {
    for (auto& pipe : pipes) pipe.clear();
  }
  // the reply the real firmware queues after initialization: how many structures it exposes, then where
  void write_struct_addresses() {
    static constexpr uint16_t kAddresses[15] = {
        kFrameCounter, kSourceConfigurations, kSourceStatuses, kAdpcmCoefficients, kDspConfiguration,
        kDspStatus,    kFinalSamples,         kIntermediateMixSamples, kCompressor, kDspDebug,
        kUnknown10,    kUnknown11,            kUnknown12,      kUnknown13,         kUnknown14};
    auto& pipe = pipes[kPipeAudio];
    pipe.clear();
    auto put = [&](uint16_t v) { pipe.push_back(static_cast<uint8_t>(v)), pipe.push_back(static_cast<uint8_t>(v >> 8)); };
    put(15);
    for (uint16_t a : kAddresses) put(a);
  }
};

Dsp::Dsp(Kernel& kernel) : kernel_(kernel), state_(std::make_unique<State>()) {}
Dsp::~Dsp() = default;

bool Dsp::handle(Thread& thread, uint32_t* c) {
  State& s = *state_;
  Memory& m = kernel_.memory();
  const uint32_t id = c[0] >> 16;
  switch (id) {
  case kRecvData: reply(c, id, {s.running ? 0u : 1u}); return true; // RecvData(register) -> the DSP's reply
  case kRecvDataIsReady: reply(c, id, {1}); return true;
  case kSendData: case kSendFifoEx: case kSetSemaphore: case kClearSemaphore: case kMaskSemaphore:
    reply(c, id);
    return true;
  case kSendDataIsEmpty: case kCheckSemaphoreRequest: reply(c, id, {1}); return true;
  case kGetSemaphore: reply(c, id, {s.semaphore_value}); return true;
  case kRecvFifoEx: reply(c, id, {0}); return true;
  case kConvertProcessAddressFromDspDram: reply(c, id, {dsp_dram_to_arm(c[1])}); return true; // mapped one to one
  case kWriteProcessPipe: { // WriteProcessPipe(channel, size, <buffer>)
    const uint32_t channel = c[1], size = c[2], source = c[4];
    std::vector<uint8_t> data(std::min<uint32_t>(size, 0x1000));
    m.read(source, data.data(), static_cast<uint32_t>(data.size()));
    if (!write_pipe(channel, data)) return false;
    reply(c, id);
    return true;
  }
  case kReadPipe: case kReadPipeIfPossible: { // (channel, peer, size) into static buffer 0
    const uint32_t channel = c[1], size = c[3] & 0xFFFF;
    const uint32_t destination = m.read32(thread.ctx.tls + 0x184);
    // Zakuro: size.min(capacity.max(size)), which is the size; the capacity is not checked
    std::vector<uint8_t> data;
    if (channel < kPipeCount) {
      auto& pipe = s.pipes[channel];
      const size_t count = std::min<size_t>(size, pipe.size());
      data.assign(pipe.begin(), pipe.begin() + static_cast<std::ptrdiff_t>(count));
      pipe.erase(pipe.begin(), pipe.begin() + static_cast<std::ptrdiff_t>(count));
    }
    const uint32_t count = static_cast<uint32_t>(data.size());
    if (destination && count) m.write(destination, data.data(), count);
    c[0] = header(id, 2, 2), c[1] = kOk, c[2] = count, c[3] = count << 14 | 2, c[4] = destination;
    return true;
  }
  case kGetPipeReadableSize: // (channel, peer)
    reply(c, id, {c[1] < kPipeCount ? static_cast<uint32_t>(s.pipes[c[1]].size()) : 0u});
    return true;
  case kLoadComponent: // (size, program mask, data mask, <buffer>): the HLE audio path takes over
    s.component_loaded = true;
    // the second word: the component was accepted
    c[0] = header(id, 2, 2), c[1] = kOk, c[2] = 1, c[3] = 0x100000u << 4 | 0xA, c[4] = 0;
    return true;
  case kUnloadComponent:
    s.component_loaded = false, s.running = false;
    reply(c, id);
    return true;
  case kFlushDataCache: case kInvalidateDataCache: reply(c, id); return true;
  case kRegisterInterruptEvents: { // (interrupt, channel, event handle): kept until the title is done
    if (auto object = kernel_.resolve(c[4]))
      if (std::find(s.interrupt_events.begin(), s.interrupt_events.end(), object) == s.interrupt_events.end())
        s.interrupt_events.push_back(object);
    reply(c, id);
    return true;
  }
  case kGetSemaphoreEventHandle: {
    // the same event in a new handle of the title's own each time: a title that
    // closed its handle when it let the DSP go asks again after loading it anew
    if (!s.semaphore_event) s.semaphore_event = std::make_shared<Event>(); // one-shot
    c[0] = header(id, 1, 2), c[1] = kOk, c[2] = 0, c[3] = kernel_.add_handle(s.semaphore_event); // one handle, copied
    return true;
  }
  case kSetSemaphoreMask: s.semaphore_mask = c[1], reply(c, id); return true;
  case kGetHeadphoneStatus: reply(c, id, {0}); return true; // no headphones
  case kForceHeadphoneOut: reply(c, id); return true;
  case kGetIsDspOccupied: reply(c, id, {0}); return true;
  default: return false;
  }
}

bool Dsp::write_pipe(uint32_t channel, const std::vector<uint8_t>& data) {
  State& s = *state_;
  // a request for the AAC decoder, answered at once in the same pipe (as Zakuro, dsp_aac.rs)
  if (channel == kPipeBinary && !data.empty()) {
    const auto answer = s.aac.answer(kernel_.memory(), data.data(), data.size());
    s.pipes[kPipeBinary].assign(answer.begin(), answer.end());
    signal_semaphore();
    return true;
  }
  if (channel != kPipeAudio || data.empty()) return true;
  switch (data[0]) {
  case kStateInitialize: s.reset_pipes(), s.write_struct_addresses(), s.running = true; break;
  case kStateShutdown: s.reset_pipes(), s.running = false; break;
  case kStateWakeup: s.running = true; break;
  case kStateSleep: s.running = false; break;
  default: break; // an unknown state change
  }
  // the firmware answers every state change by signalling the semaphore
  signal_semaphore();
  return true;
}

// one audio frame of the firmware: the voices play through their buffers and
// publish their statuses (how a title's sound library knows its audio moves), then mix
void Dsp::advance() {
  State& s = *state_;
  if (!s.running) return;
  Memory& m = kernel_.memory();
  const VoiceLayout voices{dsp_dram_to_arm(kSourceConfigurations), dsp_dram_to_arm(kSourceStatuses),
                           dsp_dram_to_arm(kAdpcmCoefficients), dsp_dram_to_arm(kFrameCounter)};
  const MixerLayout mixer{dsp_dram_to_arm(kDspConfiguration), dsp_dram_to_arm(kFinalSamples),
                          dsp_dram_to_arm(kIntermediateMixSamples)};
  const uint32_t read = current_region(m, voices.frame_counter);
  const Mixes mixes = tick_voices(s.voices, m, voices);
  const Frame frame = s.mixer.tick(m, mixer, read, mixes);
  for (const Sample& pair : frame) s.output.push_back(pair[0]), s.output.push_back(pair[1]);
  if (s.output.size() > kMaxOutput * 2)
    s.output.erase(s.output.begin(), s.output.begin() + static_cast<std::ptrdiff_t>(s.output.size() - kMaxOutput * 2));
}

// signals the DSP's events, as the firmware does at the end of every audio frame
void Dsp::signal_semaphore() {
  State& s = *state_;
  // real firmware reports the requested mask back as satisfied when it signals;
  // a driver reading 0 after waking sees a spurious wakeup and sleeps again
  s.semaphore_value = s.semaphore_mask;
  if (s.semaphore_event) kernel_.signal(*s.semaphore_event);
  for (auto& object : s.interrupt_events)
    if (object->kind == Object::Kind::Event) kernel_.signal(static_cast<Event&>(*object));
}

// Zakuro calls this every 160 * 8192 CPU cycles (4.89 ms); the host calls it every 160 samples of real time
void Dsp::audio_frame() {
  if (!state_->running) return;
  advance();
  signal_semaphore();
}

std::vector<int16_t> Dsp::take_output() { return std::exchange(state_->output, {}); }

} // namespace host
