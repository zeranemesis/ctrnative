#include "aac.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#endif

namespace host {

namespace {

constexpr uint16_t kCodecDecodeAac = 1;
constexpr uint16_t kCommandInit = 0, kCommandDecode = 1;
constexpr uint32_t kResultError = 1;
constexpr uint32_t kFcram = 0x20000000; // the DSP addresses memory physically
// the sample rates ADTS numbers, and how the answer numbers them
constexpr uint32_t kAdtsRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
constexpr uint32_t kAnswerRates[9][2] = {{48000, 0}, {44100, 1}, {32000, 2}, {24000, 3}, {22050, 4},
                                         {16000, 5}, {12000, 6}, {11025, 7}, {8000, 8}};

uint32_t word(const uint8_t* m, size_t at) {
  uint32_t v;
  std::memcpy(&v, m + at, 4);
  return v;
}
void put(uint8_t* m, size_t at, uint32_t v) { std::memcpy(m + at, &v, 4); }

} // namespace

#if defined(_WIN32)
// the system's AAC decoder, fed whole ADTS frames, giving 16-bit PCM
struct Aac::Decoder {
  IMFTransform* mft = nullptr;
  uint32_t rate = 0, channels = 0;
  ~Decoder() {
    if (mft) mft->Release();
  }

  bool open(uint32_t sample_rate, uint32_t channel_count) {
    static thread_local bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    (void)com;
    if (!started || FAILED(CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft))))
      return false;
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channel_count);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1); // ADTS: each frame with its header
    // HEAACWAVEINFO past the WAVEFORMATEX: payload type ADTS, profile unknown, nothing more (no config: ADTS says it)
    const uint8_t info[12] = {1, 0, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    in->SetBlob(MF_MT_USER_DATA, info, sizeof info);
    const bool input = SUCCEEDED(mft->SetInputType(0, in, 0));
    in->Release();
    bool output = false;
    for (DWORD i = 0; input && !output; ++i) {
      IMFMediaType* out = nullptr;
      if (FAILED(mft->GetOutputAvailableType(0, i, &out))) break;
      GUID subtype{};
      UINT32 bits = 0;
      out->GetGUID(MF_MT_SUBTYPE, &subtype);
      out->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
      if (subtype == MFAudioFormat_PCM && bits == 16) output = SUCCEEDED(mft->SetOutputType(0, out, 0));
      out->Release();
    }
    if (!output) return false;
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    rate = sample_rate, channels = channel_count;
    return true;
  }

  // one ADTS frame in, what the decoder has ready out (interleaved)
  bool decode(const uint8_t* frame, size_t size, std::vector<int16_t>& pcm) {
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    MFCreateSample(&sample);
    MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    BYTE* bytes = nullptr;
    buffer->Lock(&bytes, nullptr, nullptr);
    std::memcpy(bytes, frame, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));
    sample->AddBuffer(buffer);
    const HRESULT taken = mft->ProcessInput(0, sample, 0);
    buffer->Release(), sample->Release();
    if (FAILED(taken)) return false;
    for (;;) {
      MFT_OUTPUT_STREAM_INFO info{};
      mft->GetOutputStreamInfo(0, &info);
      MFT_OUTPUT_DATA_BUFFER out{};
      IMFSample* made = nullptr;
      if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        IMFMediaBuffer* room = nullptr;
        MFCreateSample(&made);
        MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 2048 * 2 * 8), &room);
        made->AddBuffer(room);
        room->Release();
        out.pSample = made;
      }
      DWORD status = 0;
      const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
      if (out.pEvents) out.pEvents->Release();
      if (SUCCEEDED(hr) && out.pSample) {
        IMFMediaBuffer* whole = nullptr;
        out.pSample->ConvertToContiguousBuffer(&whole);
        BYTE* data = nullptr;
        DWORD length = 0;
        whole->Lock(&data, nullptr, &length);
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        pcm.insert(pcm.end(), samples, samples + length / 2);
        whole->Unlock();
        whole->Release();
      }
      if (out.pSample) out.pSample->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
      if (FAILED(hr)) return false;
    }
  }
};
#else
// shortcut: no decoder outside Windows yet (Android: MediaCodec); the answer of a decoding that failed
struct Aac::Decoder {
  uint32_t rate = 0, channels = 0;
  bool open(uint32_t, uint32_t) { return false; }
  bool decode(const uint8_t*, size_t, std::vector<int16_t>&) { return false; }
};
#endif

Aac::Aac() = default;
Aac::~Aac() = default;

std::array<uint8_t, Aac::kMessage> Aac::answer(Memory& memory, const uint8_t* request, size_t size) {
  std::array<uint8_t, kMessage> message{};
  std::memcpy(message.data(), request, std::min(size, kMessage));
  uint16_t codec, command;
  std::memcpy(&codec, message.data(), 2);
  std::memcpy(&command, message.data() + 2, 2);
  if (codec != kCodecDecodeAac) {
    std::printf("[dsp] the binary pipe got codec %u, not AAC decoding\n", codec);
    put(message.data(), 4, kResultError);
    return message;
  }
  if (command != kCommandDecode) { // initializing, shutting down and the rest succeed
    if (command == kCommandInit) decoder_.reset();
    put(message.data(), 4, 0);
    return message;
  }
  const uint32_t source = word(request, 8), length = word(request, 12), left = word(request, 16), right = word(request, 20);
  std::array<uint8_t, kMessage> answer{};
  std::memcpy(answer.data(), request, 4);
  // what Citra answers when decoding fails, which keeps titles going
  put(answer.data(), 8, 0), put(answer.data(), 12, 2), put(answer.data(), 16, length), put(answer.data(), 28, 1024);
  const uint8_t* data = source >= kFcram ? memory.physical(source, length) : nullptr;
  if (!data) {
    std::printf("[dsp] AAC data at %08x (%x bytes) not in FCRAM\n", source, length);
    return answer;
  }
  std::vector<int16_t> pcm;
  uint32_t rate = 0, channels = 0;
  for (size_t at = 0; at + 7 <= length;) {
    const uint8_t* f = data + at;
    if (f[0] != 0xFF || (f[1] & 0xF0) != 0xF0) { // an ADTS header: sync word, rate, channels, length
      std::printf("[dsp] no ADTS frame at %zu of %u bytes\n", at, length);
      break;
    }
    const uint32_t rate_index = f[2] >> 2 & 0xF, layout = (f[2] & 1) << 2 | f[3] >> 6;
    const size_t frame = std::min<size_t>((f[3] & 3u) << 11 | f[4] << 3 | f[5] >> 5, length - at);
    if (rate_index >= 13 || frame <= 7) break;
    rate = kAdtsRates[rate_index], channels = layout ? layout : 2;
    if (!decoder_ || decoder_->rate != rate || decoder_->channels != channels) {
      decoder_ = std::make_unique<Decoder>();
      if (!decoder_->open(rate, channels)) {
        static bool said = false;
        if (!said) std::printf("[dsp] no AAC decoder for %u Hz, %u channels: silence\n", rate, channels), said = true;
        decoder_.reset();
        break;
      }
    }
    if (!decoder_->decode(f, frame, pcm)) std::printf("[dsp] an AAC frame did not decode\n");
    at += frame;
  }
  if (pcm.empty() || !channels) return answer;
  if (static bool said = false; !said) { // once: that the music decodes, and is not silence
    int peak = 0;
    for (int16_t v : pcm) peak = std::max(peak, std::abs(static_cast<int>(v)));
    std::printf("[dsp] AAC decodes: %zu samples at %u Hz, %u channels, peak %d\n", pcm.size() / channels, rate, channels, peak);
    said = true;
  }
  // planar out: each channel where the request said
  const size_t samples = pcm.size() / channels;
  const uint32_t targets[2] = {left, right};
  for (uint32_t ch = 0; ch < std::min(channels, 2u); ++ch) {
    if (targets[ch] < kFcram) continue;
    if (auto* out = reinterpret_cast<int16_t*>(memory.physical(targets[ch], static_cast<uint32_t>(samples * 2))))
      for (size_t i = 0; i < samples; ++i) out[i] = pcm[i * channels + ch];
  }
  uint32_t rate_code = 0;
  for (const auto& r : kAnswerRates)
    if (r[0] == rate) rate_code = r[1];
  put(answer.data(), 8, rate_code), put(answer.data(), 12, channels), put(answer.data(), 28, static_cast<uint32_t>(samples));
  return answer;
}

} // namespace host
