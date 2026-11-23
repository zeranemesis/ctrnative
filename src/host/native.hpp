#pragma once

// What the native API reaches (set up by main).
#include "dsp.hpp"
#include "gsp.hpp"
#include "kernel.hpp"
#include "services.hpp"

namespace host {

struct Native {
  Kernel* kernel = nullptr;
  Gsp* gsp = nullptr;
  Dsp* dsp = nullptr;
  Services* services = nullptr;
};
extern Native native;

} // namespace host
