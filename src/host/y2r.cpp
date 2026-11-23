#include "y2r.hpp"

#include <algorithm>
#include <cstdio>
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

// the console's four standard sets (Citra's y2r_u.cpp): BT.601, BT.709, then both scaled to 16-235
constexpr int16_t kStandard[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B},
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B},
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},
};

// a DMA stream: unit bytes, then gap bytes skipped, again
std::vector<uint8_t> read_stream(Kernel& kernel, const Y2r::Stream& s, uint32_t count) {
  std::vector<uint8_t> bytes(count);
  const uint32_t unit = s.unit && s.gap ? s.unit : count;
  for (uint32_t done = 0, at = s.address; done < count; at += unit + s.gap) {
    const uint32_t n = std::min(unit, count - done);
    kernel.memory().read(at, bytes.data() + done, n);
    done += n;
  }
  return bytes;
}

void write_stream(Kernel& kernel, const Y2r::Stream& s, const std::vector<uint8_t>& bytes) {
  const uint32_t count = static_cast<uint32_t>(bytes.size()), unit = s.unit && s.gap ? s.unit : count;
  for (uint32_t done = 0, at = s.address; done < count; at += unit + s.gap) {
    const uint32_t n = std::min(unit, count - done);
    kernel.memory().write(at, bytes.data() + done, n);
    done += n;
  }
}

void set_stream(Y2r::Stream& s, const uint32_t* c) { s = {c[1], c[2], c[3], c[4]}; }

} // namespace

void Y2r::convert(Kernel& kernel) {
  const uint32_t w = width, h = lines;
  if (!w || !h) return;
  if (rotation) {
    static bool said = false;
    if (!said) std::printf("[y2r] rotation %u not native yet: converted unrotated\n", rotation), said = true;
  }
  // shortcut: the 16-bit inputs (2, 3) keep each sample's low byte; check against a game that sends them
  const uint32_t sample = input_format == 2 || input_format == 3 ? 2 : 1;
  const bool half_height = input_format == 1 || input_format == 3;
  std::vector<uint8_t> ys, us, vs;
  if (input_format == 4) {
    ys = read_stream(kernel, yuyv, w * h * 2);
  } else {
    ys = read_stream(kernel, y, w * h * sample);
    const uint32_t chroma = w / 2 * (half_height ? h / 2 : h) * sample;
    us = read_stream(kernel, u, chroma), vs = read_stream(kernel, v, chroma);
  }
  static const uint32_t bpp[] = {4, 3, 2, 2};
  const uint32_t px = bpp[output_format & 3];
  std::vector<uint8_t> rgb(w * h * px);
  const int32_t* k = nullptr;
  int32_t c[8];
  for (int i = 0; i < 8; ++i) c[i] = coefficients[i];
  k = c;
  for (uint32_t row = 0; row < h; ++row)
    for (uint32_t x = 0; x < w; ++x) {
      int32_t Y, U, V;
      if (input_format == 4) {
        const uint8_t* p = &ys[(row * w + (x & ~1u)) * 2];
        Y = p[(x & 1) * 2], U = p[1], V = p[3];
      } else {
        const uint32_t crow = half_height ? row / 2 : row, ci = (crow * (w / 2) + x / 2) * sample;
        Y = ys[(row * w + x) * sample], U = us[ci], V = vs[ci];
      }
      // as Citra's ConvertYUVToRGB: 3 fractional bits, then the offsets and 5 more
      const int32_t cy = k[0] * Y;
      const int32_t r = std::clamp(((cy + k[1] * V) >> 3) + k[5] + 0x18, 0, 0x1FFF) >> 5;
      const int32_t g = std::clamp(((cy - k[2] * V - k[3] * U) >> 3) + k[6] + 0x18, 0, 0x1FFF) >> 5;
      const int32_t b = std::clamp(((cy + k[4] * U) >> 3) + k[7] + 0x18, 0, 0x1FFF) >> 5;
      // linear, or in 8x8 tiles (Morton order inside each) a strip of 8 lines at a time
      uint32_t index = row * w + x;
      if (block) {
        const uint32_t tx = x & 7, ty = row & 7;
        const uint32_t morton = (tx & 1) | (ty & 1) << 1 | (tx & 2) << 1 | (ty & 2) << 2 | (tx & 4) << 2 | (ty & 4) << 3;
        index = (row / 8) * w * 8 + (x / 8) * 64 + morton;
      }
      uint8_t* o = &rgb[index * px];
      switch (output_format & 3) {
      case 0: o[0] = uint8_t(alpha), o[1] = uint8_t(b), o[2] = uint8_t(g), o[3] = uint8_t(r); break;
      case 1: o[0] = uint8_t(b), o[1] = uint8_t(g), o[2] = uint8_t(r); break;
      case 2: {
        const uint16_t v16 = uint16_t((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (alpha >> 7 & 1));
        std::memcpy(o, &v16, 2);
        break;
      }
      default: {
        const uint16_t v16 = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        std::memcpy(o, &v16, 2);
      }
      }
    }
  write_stream(kernel, out, rgb);
}

bool Y2r::handle(Kernel& kernel, uint32_t* c) {
  const uint32_t command = c[0] >> 16;
  switch (command) {
  case 0x0001: input_format = uint8_t(c[1]), reply(c, command); return true;
  case 0x0002: reply(c, command, {input_format}); return true;
  case 0x0003: output_format = uint8_t(c[1]), reply(c, command); return true;
  case 0x0004: reply(c, command, {output_format}); return true;
  case 0x0005: rotation = uint8_t(c[1]), reply(c, command); return true;
  case 0x0006: reply(c, command, {rotation}); return true;
  case 0x0007: block = uint8_t(c[1]), reply(c, command); return true;
  case 0x0008: reply(c, command, {block}); return true;
  case 0x0009: case 0x000B: reply(c, command); return true; // dithering: none
  case 0x000A: case 0x000C: reply(c, command, {0}); return true;
  case 0x000D: interrupt_ = c[1] != 0, reply(c, command); return true;
  case 0x000E: reply(c, command, {interrupt_ ? 1u : 0u}); return true;
  case 0x000F: // GetTransferEndEvent: the same event, a new handle each time (games close the last one)
    if (!done_) done_ = std::make_shared<Event>();
    c[0] = header(command, 1, 2), c[1] = kOk, c[2] = 0, c[3] = kernel.add_handle(done_);
    return true;
  case 0x0010: set_stream(y, c), reply(c, command); return true;
  case 0x0011: set_stream(u, c), reply(c, command); return true;
  case 0x0012: set_stream(v, c), reply(c, command); return true;
  case 0x0013: set_stream(yuyv, c), reply(c, command); return true;
  case 0x0018: set_stream(out, c), reply(c, command); return true;
  case 0x0014: case 0x0015: case 0x0016: case 0x0017: case 0x0019: reply(c, command, {1}); return true; // finished
  case 0x001A: width = uint16_t(c[1]), reply(c, command); return true;
  case 0x001B: reply(c, command, {width}); return true;
  case 0x001C: lines = uint16_t(c[1]), reply(c, command); return true;
  case 0x001D: reply(c, command, {lines}); return true;
  case 0x001E: std::memcpy(coefficients, c + 1, 16), reply(c, command); return true;
  case 0x001F: {
    uint32_t w[4];
    std::memcpy(w, coefficients, 16);
    reply(c, command, {w[0], w[1], w[2], w[3]});
    return true;
  }
  case 0x0020: std::memcpy(coefficients, kStandard[c[1] & 3], 16), reply(c, command); return true;
  case 0x0021: {
    uint32_t w[4];
    std::memcpy(w, kStandard[c[1] & 3], 16);
    reply(c, command, {w[0], w[1], w[2], w[3]});
    return true;
  }
  case 0x0022: alpha = uint16_t(c[1]), reply(c, command); return true;
  case 0x0023: reply(c, command, {alpha}); return true;
  case 0x0024: reply(c, command); return true;
  case 0x0025: reply(c, command, {0, 0, 0, 0, 0, 0, 0, 0}); return true;
  case 0x0026: // StartConversion: done at once, and said so
    convert(kernel);
    if (done_) kernel.signal(*done_);
    reply(c, command);
    return true;
  case 0x0027: case 0x002B: case 0x002C: reply(c, command); return true; // Stop, DriverInitialize/Finalize
  case 0x0028: case 0x002A: reply(c, command, {0}); return true;          // IsBusyConversion, PingProcess
  case 0x0029: { // SetPackageParameter: formats, rotation, block, width, lines, standard set, alpha
    uint8_t p[12];
    std::memcpy(p, c + 1, sizeof p);
    input_format = p[0], output_format = p[1], rotation = p[2], block = p[3];
    width = uint16_t(p[4] | p[5] << 8), lines = uint16_t(p[6] | p[7] << 8);
    std::memcpy(coefficients, kStandard[p[8] & 3], 16);
    alpha = uint16_t(p[10] | p[11] << 8);
    reply(c, command);
    return true;
  }
  default: return false;
  }
}

} // namespace host
