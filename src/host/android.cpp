// Android: SDL's activity calls SDL_main in libmain.so. The game's files live
// in the app's own folder (/storage/emulated/0/Android/data/<package>/files):
// rom.3ds (a decrypted dump), code.bin and exh.bin (from its ExeFS), user/ for
// the saves, cache/ for the shaders, host.log for what the host prints, and an
// optional env.txt of NAME=value lines (settings, automated runs).
#include <SDL3/SDL.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "picanative/api.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

int host_main(int argc, char** argv); // main.cpp's main, renamed for Android

// LowerScreen.java: the second display's surface (null when it goes away) and its touches
extern "C" JNIEXPORT void JNICALL Java_fr_portnatif_sm3dl_LowerScreen_nativeSurface(JNIEnv* env, jclass, jobject surface) {
  pn_android_second_screen(surface ? ANativeWindow_fromSurface(env, surface) : nullptr);
}
extern "C" JNIEXPORT void JNICALL Java_fr_portnatif_sm3dl_LowerScreen_nativeTouch(JNIEnv*, jclass, jboolean down, jfloat u,
                                                                                    jfloat v) {
  pn_android_second_touch(down, u, v);
}

extern "C" __attribute__((visibility("default"))) int SDL_main(int, char**) {
  const char* storage = SDL_GetAndroidExternalStoragePath();
  const std::string base = storage ? storage : ".";
  std::freopen((base + "/host.log").c_str(), "w", stdout);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  dup2(fileno(stdout), 2); // stderr too
  std::ifstream settings(base + "/env.txt");
  for (std::string line; std::getline(settings, line);) {
    const auto equals = line.find('=');
    if (equals != std::string::npos) setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
  }
  setenv("CTRN_ROM", (base + "/rom.3ds").c_str(), 0);
  setenv("CTRN_USER_DIR", (base + "/user").c_str(), 0);
  setenv("PICANATIVE_CACHE_DIR", (base + "/cache").c_str(), 0);
  setenv("CTRN_LAYOUT", "large", 0); // the Thor's 16:9 screen: the top screen large, the bottom one beside it
  std::string module = "libsm3dl_aot.so", code = base + "/code.bin", exheader = base + "/exh.bin", name = "sm3dl";
  char* argv[] = {name.data(), module.data(), code.data(), exheader.data(), nullptr};
  return host_main(4, argv);
}
