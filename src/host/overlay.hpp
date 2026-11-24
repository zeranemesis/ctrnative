// The settings over the game (F1): the screens' look and the texture improvements, changed while the game is
// paused, each change applied at once. Windows only: a window of its own over the game's, drawn by GDI.
#pragma once
#include <functional>

namespace host {

struct Look {
  int filter = 0;     // pn_window_set_filter: 0 bilinear, 1 nearest, 2 sharp
  int effects = 0;    // PN_EFFECT_*
  int anisotropy = 0; // 0 off, else samples
  int mipmaps = 0;
  int stereo = 0;     // pn_window_set_stereo
  int scale = 1;      // pn_set_resolution_scale, 1 to 4
};

// true when F1 was just pressed over the game's window (Windows; false elsewhere)
bool overlay_asked();
// the menu over the game's window until it is closed (F1, Escape); apply at each change; frame between messages
// (the window kept alive, the console's buttons it read), false once the game's window is closed
void overlay_run(Look& look, const std::function<void(const Look&)>& apply, const std::function<bool(unsigned& buttons)>& frame);

} // namespace host
