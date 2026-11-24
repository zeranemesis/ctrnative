#include "overlay.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace host {
namespace {

struct Row {
  const wchar_t* label;
  int count;
  const wchar_t* values[5];
  const wchar_t* about;
};
const Row kRows[] = {
  {L"Filtre de l'image", 3, {L"Désactivé", L"Pixels nets", L"Net lissé"},
   L"Comment l'image est agrandie. Pixels nets : sans lissage ; net lissé : pixels nets aux bords adoucis."},
  {L"Anticrénelage (FXAA)", 2, {L"Désactivé", L"Activé"}, L"Adoucit les bords en escalier des objets 3D."},
  {L"Accentuation", 2, {L"Désactivé", L"Activé"}, L"Rend l'image plus nette après l'agrandissement."},
  {L"Couleurs de l'écran 3DS", 2, {L"Désactivé", L"Activé"}, L"Imite les couleurs plus ternes de l'écran de la console."},
  {L"Lignes de balayage", 2, {L"Désactivé", L"Activé"}, L"Assombrit une ligne sur deux, façon écran cathodique."},
  {L"Filtrage anisotrope", 5, {L"Désactivé", L"2×", L"4×", L"8×", L"16×"},
   L"Textures plus nettes vues de biais. Appliqué à la reprise : les textures sont rechargées."},
  {L"Mipmaps", 2, {L"Désactivé", L"Activé"},
   L"Moins de scintillement des textures au loin. Appliqué à la reprise : les textures sont rechargées."},
  {L"3D", 3, {L"Désactivé", L"Anaglyphe", L"Côte à côte"},
   L"L'écran du haut en relief, quand le jeu dessine les deux yeux : lunettes rouge et cyan, ou télévision 3D."},
  {L"Échelle de rendu", 4, {L"1×", L"2×", L"3×", L"4×"},
   L"La résolution interne du rendu (1× : les pixels de la console). Appliquée à la reprise, après un court rechargement."},
};
constexpr int kCount = sizeof kRows / sizeof kRows[0];
constexpr int kAniso[5] = {0, 2, 4, 8, 16};

int get(const Look& l, int row) {
  if (row == 0) return l.filter;
  if (row <= 4) return l.effects >> (row - 1) & 1; // PN_EFFECT_FXAA, SHARPEN, LCD_COLORS, SCANLINES: bits 0-3
  if (row == 5) return int(std::find(kAniso, kAniso + 5, l.anisotropy) - kAniso) % 5;
  return row == 6 ? l.mipmaps : row == 7 ? l.stereo : l.scale - 1;
}
void set(Look& l, int row, int v) {
  if (row == 0) l.filter = v;
  else if (row <= 4) l.effects = (l.effects & ~(1 << (row - 1))) | v << (row - 1);
  else if (row == 5) l.anisotropy = kAniso[v];
  else if (row == 6) l.mipmaps = v;
  else if (row == 7) l.stereo = v;
  else l.scale = v + 1;
}

struct State {
  Look* look = nullptr;
  const std::function<void(const Look&)>* apply = nullptr;
  int selected = 0;
  bool closed = false;
  float row_top = 0, row_h = 1; // in client pixels, for the mouse
} g;
bool g_f1 = false;     // F1 pressed over the game's window (its messages, seen by a hook as SDL takes them)
HWND g_overlay = nullptr;
LRESULT CALLBACK on_message(int code, WPARAM wp, LPARAM lp) {
  const MSG* m = reinterpret_cast<const MSG*>(lp);
  if (code == HC_ACTION && wp == PM_REMOVE && m->message == WM_KEYDOWN && m->wParam == VK_F1 && !(m->lParam & (1 << 30)) &&
      m->hwnd != g_overlay)
    g_f1 = true;
  return CallNextHookEx(nullptr, code, wp, lp);
}
constexpr COLORREF kKey = RGB(255, 0, 255); // outside the panel: the game, seen as it is

void change(int step) {
  const int n = kRows[g.selected].count;
  set(*g.look, g.selected, (get(*g.look, g.selected) + step + n) % n);
  (*g.apply)(*g.look);
}

void paint(HWND w, HDC out) {
  RECT r;
  GetClientRect(w, &r);
  const int cw = r.right, ch = r.bottom;
  HDC dc = CreateCompatibleDC(out);
  HBITMAP bitmap = CreateCompatibleBitmap(out, cw, ch);
  HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
  HBRUSH key = CreateSolidBrush(kKey);
  FillRect(dc, &r, key);
  DeleteObject(key);
  // the panel over the bottom half (the console's bottom screen, in the default layout): the top screen stays seen
  const float u = std::min(cw / 400.0f, ch / 480.0f);
  const int px = int(cw / 2 - 170 * u), pw = int(340 * u), py = int(ch / 2 + 4 * u), ph = int(232 * u);
  auto box = [&](int x, int y, int bw, int bh, COLORREF c) {
    RECT b{x, y, x + bw, y + bh};
    HBRUSH brush = CreateSolidBrush(c);
    FillRect(dc, &b, brush);
    DeleteObject(brush);
  };
  box(px, py, pw, ph, RGB(29, 34, 40));
  SetBkMode(dc, TRANSPARENT);
  auto font = [&](float size, bool bold) {
    return CreateFontW(-int(size * u + 0.5f), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  };
  HFONT title = font(14, true), normal = font(11.5f, false), bold = font(11.5f, true), small = font(10, false);
  auto say = [&](HFONT f, COLORREF c, int x, int y, int x2, int y2, const std::wstring& s, UINT flags) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT t{x, y, x2, y2};
    DrawTextW(dc, s.c_str(), -1, &t, flags | DT_NOPREFIX);
  };
  const int m = int(10 * u);
  say(title, RGB(240, 243, 246), px + m, py + int(6 * u), px + pw - m, py + int(26 * u), L"Réglages  ·  jeu en pause",
      DT_SINGLELINE | DT_VCENTER);
  g.row_top = py + 30 * u, g.row_h = 17 * u;
  for (int i = 0; i < kCount; ++i) {
    const int y = int(g.row_top + i * g.row_h), h = int(g.row_h) - 1;
    const bool on = i == g.selected;
    if (on) box(px + m / 2, y, pw - m, h, RGB(30, 136, 229));
    const int v = get(*g.look, i);
    say(on ? bold : normal, RGB(230, 234, 238), px + m, y, px + pw / 2 + m, y + h, kRows[i].label, DT_SINGLELINE | DT_VCENTER);
    say(on ? bold : normal, v ? (on ? RGB(255, 255, 255) : RGB(110, 180, 245)) : RGB(170, 180, 190), px + pw / 2, y,
        px + pw - m, y + h, (on ? L"‹  " : L"") + std::wstring(kRows[i].values[v]) + (on ? L"  ›" : L""),
        DT_SINGLELINE | DT_VCENTER | DT_CENTER);
  }
  const int about_y = int(g.row_top + kCount * g.row_h + 5 * u);
  say(small, RGB(200, 207, 214), px + m, about_y, px + pw - m, py + ph - int(18 * u), kRows[g.selected].about, DT_WORDBREAK);
  say(small, RGB(140, 150, 160), px + m, py + ph - int(17 * u), px + pw - m, py + ph - int(3 * u),
      L"↑ ↓ choisir    ← → / Entrée changer    F1 / Échap reprendre", DT_SINGLELINE | DT_CENTER | DT_VCENTER);
  BitBlt(out, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
  for (HFONT f : {title, normal, bold, small}) DeleteObject(f);
  SelectObject(dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
}

LRESULT CALLBACK proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
  switch (message) {
  case WM_KEYDOWN:
    if (wp == VK_UP) g.selected = (g.selected + kCount - 1) % kCount;
    if (wp == VK_DOWN) g.selected = (g.selected + 1) % kCount;
    if (wp == VK_LEFT) change(-1);
    if (wp == VK_RIGHT || wp == VK_RETURN || wp == VK_SPACE) change(1);
    if (wp == VK_ESCAPE || wp == VK_F1) g.closed = true;
    InvalidateRect(w, nullptr, FALSE);
    return 0;
  case WM_LBUTTONDOWN: {
    const int i = int((short(HIWORD(lp)) - g.row_top) / g.row_h);
    if (short(HIWORD(lp)) >= g.row_top && i < kCount) {
      if (i == g.selected) change(1);
      g.selected = i;
      InvalidateRect(w, nullptr, FALSE);
    }
    return 0;
  }
  case WM_ERASEBKGND: return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    paint(w, dc);
    EndPaint(w, &ps);
    return 0;
  }
  }
  return DefWindowProcW(w, message, wp, lp);
}

// the game's window: the largest visible one of this process
HWND game_window() {
  struct Search { HWND best = nullptr; long area = 0; } s;
  EnumWindows([](HWND w, LPARAM p) -> BOOL {
    auto* s = reinterpret_cast<Search*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    RECT r;
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && GetClientRect(w, &r) && r.right * r.bottom > s->area)
      s->best = w, s->area = r.right * r.bottom;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&s));
  return s.best;
}

} // namespace

bool overlay_asked() {
  static const HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, on_message, nullptr, GetCurrentThreadId());
  (void)hook;
  const bool asked = g_f1;
  g_f1 = false;
  return asked;
}

void overlay_run(Look& look, const std::function<void(const Look&)>& apply, const std::function<bool(unsigned&)>& frame) {
  HWND game = game_window();
  if (!game) return;
  static const bool registered = [] {
    WNDCLASSW c{};
    c.lpfnWndProc = proc;
    c.hInstance = GetModuleHandleW(nullptr);
    c.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    c.lpszClassName = L"ctrnative_settings";
    return RegisterClassW(&c) != 0;
  }();
  if (!registered) return;
  g = State{};
  g.look = &look, g.apply = &apply;
  auto place = [&](RECT& r) {
    GetClientRect(game, &r);
    MapWindowPoints(game, nullptr, reinterpret_cast<POINT*>(&r), 2);
  };
  RECT at;
  place(at);
  HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"ctrnative_settings", L"Réglages", WS_POPUP, at.left, at.top,
                           at.right - at.left, at.bottom - at.top, game, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!w) return;
  g_overlay = w;
  SetLayeredWindowAttributes(w, kKey, 240, LWA_COLORKEY | LWA_ALPHA);
  ShowWindow(w, SW_SHOW);
  SetForegroundWindow(w);
  unsigned before = ~0u; // buttons held when the menu opened do nothing until let go
  while (!g.closed) {
    unsigned buttons = 0;
    if (!frame(buttons)) break; // the game's window closed (its messages and ours are pumped there)
    // a gamepad: the console's buttons as picanative reads them (A 0, B 1, right 4, left 5, up 6, down 7)
    const unsigned now = buttons & ~before;
    before = buttons;
    if (now & (1u << 6)) g.selected = (g.selected + kCount - 1) % kCount;
    if (now & (1u << 7)) g.selected = (g.selected + 1) % kCount;
    if (now & (1u << 5)) change(-1);
    if (now & (1u << 4 | 1u << 0)) change(1);
    if (now & (1u << 1)) g.closed = true;
    if (now) InvalidateRect(w, nullptr, FALSE);
    RECT r;
    place(r);
    if (std::memcmp(&r, &at, sizeof r)) // the game's window moved or was resized
      at = r, SetWindowPos(w, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(15);
  }
  DestroyWindow(w);
  SetForegroundWindow(game);
  g_overlay = nullptr, g_f1 = false; // the F1 that closed it does not open it again
}

} // namespace host

#else
namespace host {
bool overlay_asked() { return false; }
void overlay_run(Look&, const std::function<void(const Look&)>&, const std::function<bool(unsigned&)>&) {}
} // namespace host
#endif
