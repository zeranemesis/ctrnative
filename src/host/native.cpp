// The host's native API for a game's own platform layer (a port that replaces SDK functions):
// what the SDK's service wrappers did through IPC, called directly. The game
// finds these exports at run time; no command buffer, handle or svc.
#include "native.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define HOST_EXPORT extern "C" __declspec(dllexport)
#else
#define HOST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace host {
Native native;
}

using namespace host;

namespace {

Thread& thread_of(Context* ctx) { return *static_cast<Thread*>(ctx->user); }
Memory& memory() { return native.kernel->memory(); }

// runs a service's own code on the words its request carries, and returns its
// result; out gets the reply from its word 2 on (what the wrapper read back)
uint32_t call(Context* ctx, const char* service, std::initializer_list<uint32_t> words, uint32_t* out = nullptr,
              size_t count = 1) {
  uint32_t c[64] = {};
  std::copy(words.begin(), words.end(), c);
  Thread& self = thread_of(ctx);
  const std::string name = service;
  native.kernel->native_call(self, [&] {
    const bool done = name == "gsp::Gpu"   ? native.gsp->handle(self, c)
                      : name == "dsp::DSP" ? native.dsp->handle(self, c)
                                           : native.services->handle(*native.kernel, self, name, c);
    if (!done) native.kernel->stop("a native call reached a command the service does not have");
  });
  if (out) std::copy(c + 2, c + 2 + count, out);
  return c[1];
}

} // namespace

// nn::gx: the GX command ring runs (TriggerCmdReqQueue)
HOST_EXPORT uint32_t host_gx_trigger(Context* ctx) { return call(ctx, "gsp::Gpu", {0x000C0000u}); }
// nn::gx: the CPU wrote what the GPU will read (FlushDataCache)
HOST_EXPORT uint32_t host_gx_flush(Context* ctx, uint32_t address, uint32_t size) {
  return call(ctx, "gsp::Gpu", {0x00080082u, address, size, 0u, 0xFFFF8001u});
}
// nn::dsp: the CPU wrote what the DSP will read, or will read what it wrote
HOST_EXPORT uint32_t host_dsp_flush(Context* ctx, uint32_t address, uint32_t size) {
  return call(ctx, "dsp::DSP", {0x00130082u, address, size, 0u, 0xFFFF8001u});
}
HOST_EXPORT uint32_t host_dsp_invalidate(Context* ctx, uint32_t address, uint32_t size) {
  return call(ctx, "dsp::DSP", {0x00140082u, address, size, 0u, 0xFFFF8001u});
}
// nn::dsp: whether headphones are in
HOST_EXPORT uint32_t host_dsp_headphones(Context* ctx, uint32_t* status) { return call(ctx, "dsp::DSP", {0x001F0000u}, status); }
// nn::dsp: a DSP RAM address as the process sees it
HOST_EXPORT uint32_t host_dsp_convert(Context* ctx, uint32_t address, uint32_t* out) {
  return call(ctx, "dsp::DSP", {0x000C0040u, address}, out);
}

// The rest of the wrappers, those the game calls at boot. Each takes the
// wrapper's arguments as the wrapper got them (in: r0-r3, then the stack, 12
// words) and returns its result; out (16 words) gets the reply from its word 2
// on, which the platform layer writes where the wrapper wrote it. The session
// handle the wrappers send to is not needed: the service is named.
#define NATIVE_API(name) HOST_EXPORT uint32_t name(Context* ctx, [[maybe_unused]] const uint32_t* in, uint32_t* out)
#define CALL_OUT out, 16

// srv: RegisterClient, EnableNotification(&semaphore), GetServiceHandle(&handle, name, length,
// flags): the handles are still made, the game waits on some of them
NATIVE_API(host_srv_register) { return call(ctx, "srv:", {0x00010002u, 0x20u}, CALL_OUT); }
NATIVE_API(host_srv_notification) { return call(ctx, "srv:", {0x00020000u}, CALL_OUT); }
NATIVE_API(host_srv_service) {
  uint32_t name[2];
  memory().read(in[1], name, 8);
  return call(ctx, "srv:", {0x00050100u, name[0], name[1], in[2], in[3]}, CALL_OUT);
}

// APT: GetLockHandle(&lock, flags, &attributes, &power), Initialize(app, attributes, &event,
// &event), Enable(attributes), Receive/GlanceParameter(&sender, app, &command, buffer, size,
// &size, &handle), 0x43(app), AppletUtility(id, input, input size, output, output size, &result)
NATIVE_API(host_apt_lock) { return call(ctx, "APT:U", {0x00010040u, in[1]}, CALL_OUT); }
NATIVE_API(host_apt_initialize) { return call(ctx, "APT:U", {0x00020080u, in[0], in[1]}, CALL_OUT); }
NATIVE_API(host_apt_enable) { return call(ctx, "APT:U", {0x00030040u, in[0]}, CALL_OUT); }
// shortcut: the parameter's buffer is not handed over (the service writes none, size 0); pass it if a title gets one
NATIVE_API(host_apt_receive) { return call(ctx, "APT:U", {0x000D0080u, in[1], in[4]}, CALL_OUT); }
NATIVE_API(host_apt_glance) { return call(ctx, "APT:U", {0x000E0080u, in[1], in[4]}, CALL_OUT); }
NATIVE_API(host_apt_0043) { return call(ctx, "APT:U", {0x00430040u, in[0]}, CALL_OUT); }
NATIVE_API(host_apt_utility) {
  return call(ctx, "APT:U", {0x004B00C2u, in[0], in[2], in[4], in[2] << 14 | 0x402, in[1]}, CALL_OUT);
}

// cecd: 0x0001(a, b, c, &out) and 0x0002(&out, buffer, size)
NATIVE_API(host_cecd_0001) { return call(ctx, "cecd:u", {0x000100C2u, in[0], in[1], in[2], 0x20u}, CALL_OUT); }
NATIVE_API(host_cecd_0002) { return call(ctx, "cecd:u", {0x00020042u, in[2], in[2] << 4 | 0xC, in[1]}, CALL_OUT); }

// cfg: GetConfigInfoBlk2(buffer, size, block), GenHashConsoleUnique(salt, &hash)
NATIVE_API(host_cfg_block) { return call(ctx, "cfg:u", {0x00010082u, in[1], in[2], in[1] << 4 | 0xC, in[0]}, CALL_OUT); }
NATIVE_API(host_cfg_hash) { return call(ctx, "cfg:u", {0x00030040u, in[0]}, CALL_OUT); }

// dsp (r0: the session): SetSemaphore(value), WriteProcessPipe(channel, buffer, size),
// ReadPipeIfPossible(channel, peer, buffer, size, &read), LoadComponent(buffer, size, program
// mask, data mask, &loaded), RegisterInterruptEvents(event, interrupt, channel),
// GetSemaphoreEventHandle(&event), SetSemaphoreMask(mask)
NATIVE_API(host_dsp_semaphore) { return call(ctx, "dsp::DSP", {0x00070040u, in[1] & 0xFFFF}, CALL_OUT); }
NATIVE_API(host_dsp_write_pipe) {
  return call(ctx, "dsp::DSP", {0x000D0082u, in[1], in[3], in[3] << 14 | 0x402, in[2]}, CALL_OUT);
}
NATIVE_API(host_dsp_read_pipe) {
  // the DSP service finds its destination where the wrapper put it, static buffer 0
  // in the thread's TLS: put there for the call only, as the wrapper did
  const uint32_t at = thread_of(ctx).ctx.tls + 0x184, kept = memory().read32(at);
  memory().write32(at, in[3]);
  const uint32_t result = call(ctx, "dsp::DSP", {0x001000C0u, in[1], in[2], in[4] & 0xFFFF}, CALL_OUT);
  memory().write32(at, kept);
  return result;
}
NATIVE_API(host_dsp_load) {
  return call(ctx, "dsp::DSP", {0x001100C2u, in[2], in[3] & 0xFFFF, in[4] & 0xFFFF, in[2] << 4 | 0xA, in[1]}, CALL_OUT);
}
NATIVE_API(host_dsp_interrupt) { return call(ctx, "dsp::DSP", {0x00150082u, in[2], in[3], 0u, in[1]}, CALL_OUT); }
NATIVE_API(host_dsp_event) { return call(ctx, "dsp::DSP", {0x00160000u}, CALL_OUT); }
NATIVE_API(host_dsp_mask) { return call(ctx, "dsp::DSP", {0x00170040u, in[1] & 0xFFFF}, CALL_OUT); }

// gsp (r0: the session): WriteHWRegs(register, data, size), WriteHWRegsWithMask(register, data,
// mask, size), FlushDataCache(process, address, size), SetLcdForceBlack(on),
// RegisterInterruptRelayQueue(event, flags, &shared memory, &thread index), AcquireRight(process,
// flags), 0x1E(a, b)
NATIVE_API(host_gsp_write) { return call(ctx, "gsp::Gpu", {0x00010082u, in[1], in[3], in[3] << 14 | 2, in[2]}, CALL_OUT); }
NATIVE_API(host_gsp_write_masked) {
  return call(ctx, "gsp::Gpu", {0x00020084u, in[1], in[4], in[4] << 14 | 2, in[2], in[4] << 14 | 0x402, in[3]}, CALL_OUT);
}
NATIVE_API(host_gsp_flush) { return call(ctx, "gsp::Gpu", {0x00080082u, in[2], in[3], 0u, in[1]}, CALL_OUT); }
NATIVE_API(host_gsp_black) { return call(ctx, "gsp::Gpu", {0x000B0040u, in[1] & 0xFF}, CALL_OUT); }
NATIVE_API(host_gsp_relay) { return call(ctx, "gsp::Gpu", {0x00130042u, in[2], 0u, in[1]}, CALL_OUT); }
NATIVE_API(host_gsp_right) { return call(ctx, "gsp::Gpu", {0x00160042u, in[2] & 0xFF, 0u, in[1]}, CALL_OUT); }
NATIVE_API(host_gsp_001e) { return call(ctx, "gsp::Gpu", {0x001E0080u, in[1], in[2]}, CALL_OUT); }

// hid: GetIPCHandles, EnableAccelerometer, EnableGyroscopeLow, the gyroscope's coefficient and calibration
NATIVE_API(host_hid_handles) { return call(ctx, "hid:USER", {0x000A0000u}, CALL_OUT); }
NATIVE_API(host_hid_accelerometer) { return call(ctx, "hid:USER", {0x00110000u}, CALL_OUT); }
NATIVE_API(host_hid_gyroscope) { return call(ctx, "hid:USER", {0x00130000u}, CALL_OUT); }
NATIVE_API(host_hid_coefficient) { return call(ctx, "hid:USER", {0x00150000u}, CALL_OUT); }
NATIVE_API(host_hid_calibration) { return call(ctx, "hid:USER", {0x00160000u}, CALL_OUT); }

// mic: MapSharedMem(memory, size), StartSampling(encoding, rate, offset, size, loop),
// SetGain(gain), GetGain(&gain), SetPower(on), 0x0D(value)
NATIVE_API(host_mic_map) { return call(ctx, "mic:u", {0x00010042u, in[1], 0u, in[0]}, CALL_OUT); }
NATIVE_API(host_mic_start) {
  return call(ctx, "mic:u", {0x00030140u, in[0] & 0xFF, in[1] & 0xFF, in[2], in[3], in[4] & 0xFF}, CALL_OUT);
}
NATIVE_API(host_mic_set_gain) { return call(ctx, "mic:u", {0x00080040u, in[0] & 0xFF}, CALL_OUT); }
NATIVE_API(host_mic_gain) { return call(ctx, "mic:u", {0x00090000u}, CALL_OUT); }
NATIVE_API(host_mic_power) { return call(ctx, "mic:u", {0x000A0040u, in[0] & 0xFF}, CALL_OUT); }
NATIVE_API(host_mic_000d) { return call(ctx, "mic:u", {0x000D0040u, in[0] & 0xFF}, CALL_OUT); }

// ndm: SuspendDaemons(mask), ResumeDaemons(mask), SuspendScheduler(async), ResumeScheduler,
// OverrideDefaultDaemons(mask)
NATIVE_API(host_ndm_suspend) { return call(ctx, "ndm:u", {0x00060040u, in[0]}, CALL_OUT); }
NATIVE_API(host_ndm_resume) { return call(ctx, "ndm:u", {0x00070040u, in[0]}, CALL_OUT); }
NATIVE_API(host_ndm_suspend_scheduler) { return call(ctx, "ndm:u", {0x00080040u, in[0] & 0xFF}, CALL_OUT); }
NATIVE_API(host_ndm_resume_scheduler) { return call(ctx, "ndm:u", {0x00090000u}, CALL_OUT); }
NATIVE_API(host_ndm_default) { return call(ctx, "ndm:u", {0x00140040u, in[0]}, CALL_OUT); }

// fs, on the wrappers' own arguments:
//   Initialize(session)
//   OpenFile(session, &file, transaction, -, archive (2), path type, path, path size, flags, attributes)
//   OpenFileDirectly(session, &file, transaction, archive, archive path type, archive path,
//     archive path size, file path type, file path, file path size, flags, attributes)
//   CreateFile(session, transaction, archive (2), path type, path, path size, attributes, size (2))
//   DeleteFile(session, transaction, archive (2), path type, path, path size)
//   OpenArchive(session, &archive, archive, path type, path, path size)
//   CloseArchive(session, -, archive (2))
NATIVE_API(host_fs_initialize) { return call(ctx, "fs:USER", {0x08010002u, 0x20u}, CALL_OUT); }
NATIVE_API(host_fs_open_file) {
  return call(ctx, "fs:USER", {0x080201C2u, in[2], in[4], in[5], in[6], in[8], in[9], in[10], in[8] << 14 | 2, in[7]}, CALL_OUT);
}
NATIVE_API(host_fs_open_directly) {
  return call(ctx, "fs:USER", {0x08030204u, in[2], in[3], in[4], in[6], in[7], in[9], in[10], in[11],
                               in[6] << 14 | 0x802, in[5], in[9] << 14 | 2, in[8]}, CALL_OUT);
}
NATIVE_API(host_fs_create) {
  return call(ctx, "fs:USER", {0x08080202u, in[1], in[2], in[3], in[4], in[6], in[7], in[8], in[9], in[6] << 14 | 2, in[5]},
              CALL_OUT);
}
NATIVE_API(host_fs_delete) {
  return call(ctx, "fs:USER", {0x08040142u, in[1], in[2], in[3], in[4], in[6], in[6] << 14 | 2, in[5]}, CALL_OUT);
}
NATIVE_API(host_fs_open_archive) {
  return call(ctx, "fs:USER", {0x080C00C2u, in[2], in[3], in[5], in[5] << 14 | 2, in[4]}, CALL_OUT);
}
NATIVE_API(host_fs_close_archive) { return call(ctx, "fs:USER", {0x080E0080u, in[2], in[3]}, CALL_OUT); }

// a file the fs service opened (r0: its handle's address): Read(&read, offset (2), buffer,
// size), Write(&written, offset (2), buffer, size, flags), GetSize(&size), Close. The service
// finds the file by the handle in r0, as a request leaves it there.
uint32_t file_call(Context* ctx, const uint32_t* in, std::initializer_list<uint32_t> words, uint32_t* out) {
  thread_of(ctx).ctx.r[0] = memory().read32(in[0]);
  return call(ctx, "FSFile", words, CALL_OUT);
}
NATIVE_API(host_file_read) { return file_call(ctx, in, {0x080200C2u, in[2], in[3], in[5], in[5] << 4 | 0xC, in[4]}, out); }
NATIVE_API(host_file_write) {
  return file_call(ctx, in, {0x08030102u, in[2], in[3], in[5], in[6], in[5] << 4 | 0xA, in[4]}, out);
}
NATIVE_API(host_file_size) { return file_call(ctx, in, {0x08040000u}, out); }
NATIVE_API(host_file_close) { return file_call(ctx, in, {0x08080000u}, out); }
