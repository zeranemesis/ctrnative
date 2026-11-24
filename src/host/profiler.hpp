#pragma once

// CTRN_PROFILE=1 (or =file.csv): a sampling profiler. Every millisecond each thread that runs the game (its
// threads, the main loop, the audio) is stopped for an instant and where it is counted: a function of the game
// (its guest address), of picanative or of the host (by their .pdb), a driver, or the system waiting. At the
// end, the shares by kind and the hottest functions; the CSV has them all.
#include <cstdint>

namespace host {

class Module;

namespace profiler {

bool on();
// the calling thread is sampled from now on, under that name
void enroll(const char* role);
void start(const Module& module);
// the report (stdout, and the CSV if one was asked); the samples go on
void report();

} // namespace profiler
} // namespace host
