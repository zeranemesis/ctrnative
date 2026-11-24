#pragma once

// CTRN_TRACE_CMDS=file.csv: who writes the GPU command lists. Each word of each list the game hands the GPU is
// put down to the game function that wrote it, and the two that called that one (so that a copy by memcpy names
// the code that asked for it): the measure of what is still made of PICA200 commands, library by library, until
// nothing is. The pages of every list sent are made read-only; each write to them faults, is recorded, runs one
// step and the page is protected again (slow: a tracing run, not a game). Windows only for now.
#include "memory.hpp"
#include "module.hpp"

#include <cstdint>

namespace host {

namespace trace {

// on when CTRN_TRACE_CMDS is set
bool on();
void start(const Module& module);
// ProcessCommandList(address, size), virtual: its words counted by writer, then its pages watched
void submitted(Memory& memory, uint32_t address, uint32_t size);
// the table, to the file; a summary on stdout
void report();

} // namespace trace

} // namespace host
