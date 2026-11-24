// ctrnative's menu: the games of a few folders shown as the console's HOME menu shows them (the icons
// on the bottom screen, the chosen game on the top one), the host's settings, and a game launched
// through tools/ctrn.py, the menu coming back when the game closes.
//   ctrnative_menu [--list] [--at X,Y]
// --list prints the games found and stops; --at places the window (a test off the screen).
// Settings: %LOCALAPPDATA%\ctrnative\menu.json (scale, layout, the ROM folders, 3dsrecomp).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static std::wstring widen(const std::string& s) {
  std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  return w;
}
static std::string narrow(const std::wstring& w) {
  std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
  return s;
}
static fs::path cache_dir() {
  const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
  return (base ? fs::path(base) : fs::temp_directory_path()) / "ctrnative";
}

// ---- the games: what their ROM's SMDH (the ExeFS's "icon") says ----
struct Game {
  fs::path rom;
  uint64_t title_id = 0;
  std::wstring title, description, publisher;
  std::vector<uint32_t> icon; // 48x48 ARGB, empty when the ROM has none (or is encrypted)
  bool encrypted = false, ready = false;
  SDL_Texture* texture = nullptr;
};

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
static bool read_at(FILE* f, uint64_t offset, void* out, size_t size) {
  return _fseeki64(f, int64_t(offset), SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}
static std::wstring utf16(const uint8_t* p, size_t chars) {
  std::wstring s;
  for (size_t i = 0; i < chars && (p[2 * i] | p[2 * i + 1]); ++i) s += wchar_t(p[2 * i] | p[2 * i + 1] << 8);
  return s;
}
// the large icon: 48x48 RGB565 in 8x8 tiles, row by row, each tile's pixels in Z order
static std::vector<uint32_t> untile(const uint8_t* p) {
  std::vector<uint32_t> out(48 * 48);
  for (int i = 0; i < 48 * 48; ++i) {
    const int tile = i / 64, in = i % 64;
    const int x = tile % 6 * 8 + ((in & 1) | (in >> 1 & 2) | (in >> 2 & 4));
    const int y = tile / 6 * 8 + ((in >> 1 & 1) | (in >> 2 & 2) | (in >> 3 & 4));
    const uint32_t c = p[2 * i] | p[2 * i + 1] << 8;
    out[y * 48 + x] = 0xFF000000u | ((c >> 11) * 255 / 31) << 16 | ((c >> 5 & 63) * 255 / 63) << 8 | (c & 31) * 255 / 31;
  }
  return out;
}

// false when the file is not a 3DS ROM (.3ds/.cci: NCSD, or a lone NCCH)
static bool load_game(const fs::path& rom, Game& g) {
  g.rom = rom;
  g.title = rom.stem().wstring();
  FILE* f = _wfopen(rom.c_str(), L"rb");
  if (!f) return false;
  uint8_t h[0x200];
  uint64_t ncch = 0;
  bool ok = read_at(f, 0, h, sizeof h);
  if (ok && !std::memcmp(h + 0x100, "NCSD", 4)) ok = read_at(f, ncch = uint64_t(le32(h + 0x120)) * 0x200, h, sizeof h);
  if (!ok || std::memcmp(h + 0x100, "NCCH", 4)) return std::fclose(f), false;
  g.title_id = le32(h + 0x108) | uint64_t(le32(h + 0x10C)) << 32;
  g.encrypted = !(h[0x18F] & 4); // NoCrypto
  const uint64_t exefs = ncch + uint64_t(le32(h + 0x1A0)) * 0x200;
  uint8_t table[0x200];
  if (!g.encrypted && le32(h + 0x1A4) && read_at(f, exefs, table, sizeof table))
    for (int i = 0; i < 10; ++i) {
      std::vector<uint8_t> smdh(0x36C0);
      if (std::memcmp(table + i * 16, "icon\0\0\0\0", 8) || le32(table + i * 16 + 12) < smdh.size() ||
          !read_at(f, exefs + 0x200 + le32(table + i * 16 + 8), smdh.data(), smdh.size()) ||
          std::memcmp(smdh.data(), "SMDH", 4))
        continue;
      // titles by language (0 Japanese, 1 English, 2 French...): French, else English, else the first given
      for (int lang : {2, 1, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
        const uint8_t* t = smdh.data() + 8 + lang * 0x200;
        if (utf16(t, 0x40).empty()) continue;
        g.title = utf16(t, 0x40), g.description = utf16(t + 0x80, 0x80), g.publisher = utf16(t + 0x180, 0x40);
        break;
      }
      g.icon = untile(smdh.data() + 0x24C0);
    }
  std::fclose(f);
  return true;
}

// shortcut: "ready" = a module was built for this title ID (any version of the ROM), not ctrn.py's digest
static bool is_ready(uint64_t title_id) {
  wchar_t prefix[24];
  swprintf(prefix, 24, L"%016llX-", static_cast<unsigned long long>(title_id));
  std::error_code e;
  for (const auto& d : fs::directory_iterator(cache_dir() / "games", e))
    if (d.path().filename().wstring().starts_with(prefix) && fs::is_regular_file(d.path() / "module.dll", e)) return true;
  return false;
}

static std::vector<Game> find_games(const std::wstring& folders) {
  std::vector<Game> games;
  std::wstringstream list(folders);
  for (std::wstring folder; std::getline(list, folder, L';');) {
    std::error_code e;
    for (const auto& entry : fs::directory_iterator(folder, e)) {
      std::wstring ext = entry.path().extension().wstring();
      for (auto& c : ext) c = towlower(c);
      Game g;
      if ((ext != L".3ds" && ext != L".cci") || !load_game(entry.path(), g)) continue;
      // the same game in two folders: the first one found
      if (std::any_of(games.begin(), games.end(), [&](const Game& o) { return o.title_id == g.title_id; })) continue;
      g.ready = is_ready(g.title_id);
      games.push_back(std::move(g));
    }
  }
  std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return _wcsicmp(a.title.c_str(), b.title.c_str()) < 0; });
  return games;
}

// ---- the settings, kept in menu.json ----
// each setting: an index into its values, 0 the game as it is (the console's look) except the layout's
enum { SCALE, LAYOUT, FILTER, FXAA, SHARPEN, LCD, SCANLINES, ANISOTROPY, MIPMAPS, STEREO, PACK, kChoices };
struct Choice {
  const char* key;                     // in menu.json
  const wchar_t* label;
  std::vector<const char*> names;      // in menu.json and in the variable passed to the game
  std::vector<const wchar_t*> values;  // shown
  const wchar_t* about;
};
static const Choice kChoices_[kChoices] = {
  {"scale", L"Échelle de rendu", {"1", "2", "3", "4"}, {L"1× (défaut)", L"2×", L"3×", L"4×"},
   L"La résolution interne du rendu, en multiple de celle de la console. 1× donne les pixels d'origine (défaut)."},
  {"layout", L"Disposition", {"stacked", "side", "large", "top"}, {L"Console", L"Côte à côte", L"Grand haut", L"Haut seul"},
   L"Comment les deux écrans se partagent la fenêtre (F10 en jeu). « Console » : l'un au-dessus de l'autre."},
  {"filter", L"Filtre de l'image", {"bilinear", "nearest", "sharp"}, {L"Désactivé", L"Pixels nets", L"Net lissé"},
   L"Comment l'image est agrandie à la taille de la fenêtre. Pixels nets : sans lissage ; net lissé : les pixels "
   L"restent nets, seuls leurs bords sont adoucis. Désactivé : lissage bilinéaire, comme jusqu'ici."},
  {"fxaa", L"Anticrénelage (FXAA)", {"off", "on"}, {L"Désactivé", L"Activé"},
   L"Adoucit les bords en escalier des objets 3D, sur l'image finale."},
  {"sharpen", L"Accentuation", {"off", "on"}, {L"Désactivé", L"Activé"},
   L"Rend l'image plus nette après l'agrandissement."},
  {"lcd", L"Couleurs de l'écran 3DS", {"off", "on"}, {L"Désactivé", L"Activé"},
   L"Imite les couleurs plus ternes de l'écran LCD de la console."},
  {"scanlines", L"Lignes de balayage", {"off", "on"}, {L"Désactivé", L"Activé"},
   L"Assombrit une ligne sur deux, façon écran cathodique."},
  {"anisotropy", L"Filtrage anisotrope", {"0", "2", "4", "8", "16"}, {L"Désactivé", L"2×", L"4×", L"8×", L"16×"},
   L"Des textures plus nettes quand on les voit de biais (sols, routes)."},
  {"mipmaps", L"Mipmaps", {"off", "on"}, {L"Désactivé", L"Activé"},
   L"Des versions réduites des textures qui n'en ont pas : moins de scintillement au loin."},
  {"stereo", L"3D", {"off", "anaglyph", "sbs"}, {L"Désactivé", L"Anaglyphe", L"Côte à côte"},
   L"L'écran du haut en relief, quand le jeu dessine les deux yeux. Anaglyphe : lunettes rouge et cyan ; "
   L"côte à côte : pour une télévision 3D ou un casque."},
  {"texture_pack", L"Pack de textures", {"off", "on"}, {L"Désactivé", L"Activé"},
   L"Remplace les textures du jeu par celles du dossier %LOCALAPPDATA%\\ctrnative\\textures\\<id du jeu> "
   L"(fichiers .tga nommés par leur contenu)."},
};
struct Settings {
  int choice[kChoices] = {}; // all 0: the game as it is
  std::wstring roms = L"C:\\jeu 3ds\\extrait;C:\\jeu 3ds";
  std::wstring recomp; // 3dsrecomp's executable, empty: CTRN_3DSRECOMP or found beside the repository
};
static fs::path settings_path() { return cache_dir() / "menu.json"; }

// the value of "key" in a flat JSON object: a string's text (\\ and \" undone) or a number's digits
static std::string json_value(const std::string& text, const std::string& key) {
  size_t at = text.find("\"" + key + "\"");
  if (at == std::string::npos || (at = text.find(':', at)) == std::string::npos) return {};
  at = text.find_first_not_of(" \t\r\n", at + 1);
  if (at == std::string::npos) return {};
  std::string out;
  if (text[at] != '"') {
    while (at < text.size() && std::strchr("-0123456789", text[at])) out += text[at++];
    return out;
  }
  for (++at; at < text.size() && text[at] != '"'; ++at) out += text[at] == '\\' && at + 1 < text.size() ? text[++at] : text[at];
  return out;
}
static std::string json_string(const std::wstring& s) {
  std::string out = "\"";
  for (char c : narrow(s)) out += c == '\\' || c == '"' ? std::string("\\") + c : std::string(1, c);
  return out + "\"";
}
static Settings load_settings() {
  Settings s;
  std::ifstream f(settings_path(), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), {});
  for (int c = 0; c < kChoices; ++c) {
    const std::string v = json_value(text, kChoices_[c].key);
    for (size_t i = 0; i < kChoices_[c].names.size(); ++i)
      if (v == kChoices_[c].names[i]) s.choice[c] = int(i);
  }
  if (const std::string v = json_value(text, "roms"); !v.empty()) s.roms = widen(v);
  s.recomp = widen(json_value(text, "3dsrecomp"));
  return s;
}
static void save_settings(const Settings& s) {
  std::error_code e;
  fs::create_directories(cache_dir(), e);
  std::ofstream f(settings_path(), std::ios::binary);
  f << "{\n";
  for (int c = 0; c < kChoices; ++c) f << "  \"" << kChoices_[c].key << "\": \"" << kChoices_[c].names[s.choice[c]] << "\",\n";
  f << "  \"roms\": " << json_string(s.roms) << ",\n  \"3dsrecomp\": " << json_string(s.recomp) << "\n}\n";
}

// ---- the cheats, as Luma3DS has them: <cheats>/<title ID>.txt ([name] then code lines), the ones on in
// <title ID>.on, one name a line (read by tools/ctrn.py); none on by default ----
struct Cheat { std::string name; int lines = 0; bool on = false; };
static fs::path cheats_path(uint64_t title_id, const char* ext) {
  const wchar_t* dir = _wgetenv(L"CTRN_CHEATS_DIR");
  char name[32];
  std::snprintf(name, sizeof name, "%016llX.%s", static_cast<unsigned long long>(title_id), ext);
  return (dir && *dir ? fs::path(dir) : cache_dir() / "cheats") / name;
}
static std::string trim(std::string s) { // as the host trims: the names must match its own
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s.erase(0, std::min(s.size(), s.find_first_not_of(" \t\r\n\v\f")));
}
// false when the game has no file of codes
static bool load_cheats(uint64_t title_id, std::vector<Cheat>& out) {
  out.clear();
  std::ifstream codes(cheats_path(title_id, "txt"), std::ios::binary);
  if (!codes) return false;
  std::vector<std::string> on;
  std::ifstream on_file(cheats_path(title_id, "on"), std::ios::binary);
  for (std::string l; std::getline(on_file, l);)
    if (!(l = trim(l)).empty()) on.push_back(l.starts_with("\xEF\xBB\xBF") ? l.substr(3) : l);
  for (std::string l; std::getline(codes, l);) {
    if (l.starts_with("\xEF\xBB\xBF")) l.erase(0, 3);
    if ((l = trim(l)).empty() || l[0] == '#' || l[0] == ';') continue;
    unsigned a = 0, b = 0;
    if (l.front() == '[' && l.back() == ']') {
      Cheat c{trim(l.substr(1, l.size() - 2))};
      c.on = std::find(on.begin(), on.end(), c.name) != on.end();
      out.push_back(c);
    } else if (!out.empty() && std::sscanf(l.c_str(), "%8x %8x", &a, &b) == 2) ++out.back().lines;
  }
  return true;
}
static void save_cheats(uint64_t title_id, const std::vector<Cheat>& cheats) {
  const fs::path path = cheats_path(title_id, "on");
  std::error_code e;
  if (std::none_of(cheats.begin(), cheats.end(), [](const Cheat& c) { return c.on; })) { fs::remove(path, e); return; }
  std::ofstream f(path, std::ios::binary);
  for (const Cheat& c : cheats) if (c.on) f << c.name << "\n";
}

// ---- text: drawn by GDI (any character of the titles, smoothed) into textures, kept by content ----
struct Label { SDL_Texture* texture = nullptr; int w = 0, h = 0; };
struct Texts {
  SDL_Renderer* renderer = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  std::map<std::wstring, Label> labels;
  std::map<std::wstring, HFONT> fonts;

  void clear() {
    for (auto& [_, l] : labels) SDL_DestroyTexture(l.texture);
    labels.clear();
  }
  // width > 0: the text's room in pixels (cut with "…" on one line, wrapped when wrap)
  const Label& get(const std::wstring& s, int px, bool bold, bool mono, int width, bool wrap) {
    const std::wstring face = mono ? L"Consolas" : L"Segoe UI";
    const std::wstring font_key = face + L"|" + std::to_wstring(px) + (bold ? L"b" : L"");
    const std::wstring key = font_key + L"|" + std::to_wstring(width) + (wrap ? L"w|" : L"|") + s;
    if (auto it = labels.find(key); it != labels.end()) return it->second;
    HFONT& font = fonts[font_key];
    if (!font)
      font = CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face.c_str());
    SelectObject(dc, font);
    const UINT flags = DT_NOPREFIX | (wrap ? DT_WORDBREAK : DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT r{0, 0, width > 0 ? width : 1 << 20, 0};
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_CALCRECT);
    Label& l = labels[key];
    l.w = width > 0 ? std::min<int>(r.right, width) : r.right, l.h = r.bottom;
    if (l.w <= 0 || l.h <= 0) return l;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), l.w, -l.h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return l;
    HGDIOBJ old = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT area{0, 0, l.w, l.h};
    DrawTextW(dc, s.c_str(), -1, &area, flags);
    GdiFlush();
    auto* p = static_cast<uint32_t*>(bits);
    for (int i = 0; i < l.w * l.h; ++i) { // white on black: the coverage becomes the alpha
      const uint32_t c = p[i], a = std::max({c & 0xFF, c >> 8 & 0xFF, c >> 16 & 0xFF});
      p[i] = a << 24 | 0xFFFFFF;
    }
    l.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, l.w, l.h);
    SDL_UpdateTexture(l.texture, nullptr, bits, l.w * 4);
    SDL_SetTextureBlendMode(l.texture, SDL_BLENDMODE_BLEND);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    return l;
  }
};

// ---- a game running: tools/ctrn.py in a job (its compilers and the host die with it), its output kept ----
struct Run {
  HANDLE process = nullptr, job = nullptr;
  std::thread reader;
  std::mutex lock;
  std::vector<std::wstring> recent; // the output's last lines
  std::string partial;              // a line not ended yet
  // the preparation's progress, from ctrn.py's and build_module.py's lines
  enum Phase { STARTING, TRANSLATING, COMPILING, LINKING, SHADERS, PLAYING } phase = STARTING;
  int to_compile = 0, compiled = 0;
  uint64_t phase_start = 0;
  bool built = false; // a translation happened in this run
  bool finished = false, window_seen = false, cancelled = false;
  DWORD code = 0;
  uint64_t start = 0;

  void line(std::string text) { // under the lock
    if (!text.empty() && text.back() == '\r') text.pop_back();
    const auto set = [&](Phase p) { phase = p, phase_start = SDL_GetTicks(); };
    int kept = 0, todo = 0;
    if (text.find("translating the code") != std::string::npos) set(TRANSLATING), built = true;
    else if (std::sscanf(text.c_str(), "%d objects kept, %d to compile", &kept, &todo) == 2) set(COMPILING), to_compile = todo, compiled = 0;
    else if (text.starts_with("compiled ") && ++compiled >= to_compile && phase == COMPILING) set(LINKING);
    else if (text.find("[ctrn] compiled in") != std::string::npos || text.find("shader binaries") != std::string::npos) set(SHADERS);
    else if (text.starts_with("[host]") && phase != PLAYING) set(PLAYING);
    // the compiled files, one line each, would push everything else out
    if (text.starts_with("compiled ") && !recent.empty() && recent.back().starts_with(L"compiled ")) recent.pop_back();
    recent.push_back(widen(text));
    if (recent.size() > 200) recent.erase(recent.begin(), recent.begin() + 100);
  }

  bool begin(std::wstring command, const fs::path& folder) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) return false;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul, si.hStdOutput = si.hStdError = write;
    PROCESS_INFORMATION pi{};
    // a console without a window: python's children (clang, the host) share it instead of opening theirs
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                        nullptr, folder.c_str(), &si, &pi);
    CloseHandle(write);
    CloseHandle(nul);
    if (!started) return CloseHandle(read), false;
    job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process = pi.hProcess;
    start = SDL_GetTicks();
    reader = std::thread([this, read] {
      char buffer[4096];
      DWORD n = 0;
      while (ReadFile(read, buffer, sizeof buffer, &n, nullptr) && n) {
        std::lock_guard<std::mutex> hold(lock);
        partial.append(buffer, n);
        for (size_t end; (end = partial.find('\n')) != std::string::npos; partial.erase(0, end + 1)) line(partial.substr(0, end));
      }
      CloseHandle(read);
    });
    return true;
  }
  // true once the launcher has stopped
  bool poll() {
    if (finished || !process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return finished;
    GetExitCodeProcess(process, &code);
    TerminateJobObject(job, 1); // anything left (the pipe's other writers)
    if (reader.joinable()) reader.join();
    return finished = true;
  }
  void cancel() {
    cancelled = true;
    if (job) TerminateJobObject(job, 1);
  }
  // a visible window of a process of the job: the game's
  bool game_window() const {
    struct Search { HANDLE job; bool found; } search{job, false};
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
      auto* s = reinterpret_cast<Search*>(p);
      DWORD pid = 0;
      if (!IsWindowVisible(w) || !GetWindowThreadProcessId(w, &pid)) return TRUE;
      if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        BOOL in = FALSE;
        s->found = IsProcessInJob(h, s->job, &in) && in;
        CloseHandle(h);
      }
      return !s->found;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
  }
  // the preparation's share done (0..1, or -1: not measurable now) and seconds left (-1: unknown)
  void progress(float& share, int& left) {
    std::lock_guard<std::mutex> hold(lock);
    const float elapsed = (SDL_GetTicks() - phase_start) / 1000.f;
    share = -1, left = -1;
    if (phase == COMPILING && to_compile > 0) {
      share = float(compiled) / to_compile;
      // the files' rate so far; the first ones say little
      if (compiled >= 3) left = int(elapsed / compiled * (to_compile - compiled)) + 1;
    } else if (phase == LINKING) share = 1;
  }
  ~Run() {
    if (job) TerminateJobObject(job, 1);
    if (reader.joinable()) reader.join();
    if (process) CloseHandle(process);
    if (job) CloseHandle(job);
  }
};

// ---- the window: the two screens in console pixels, scaled to the window ----
enum Where { TOP, BOTTOM };
static SDL_Renderer* g_renderer;
static Texts g_texts;
static float g_k = 1, g_x[2], g_y[2]; // pixels per console pixel; each screen's corner in pixels

static void color(uint32_t rgb, uint8_t alpha = 255) {
  SDL_SetRenderDrawColor(g_renderer, uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), alpha);
}
static SDL_FRect to_pixels(Where s, float x, float y, float w, float h) {
  return {g_x[s] + x * g_k, g_y[s] + y * g_k, w * g_k, h * g_k};
}
static void fill(Where s, float x, float y, float w, float h, uint32_t rgb, uint8_t alpha = 255) {
  color(rgb, alpha);
  const SDL_FRect r = to_pixels(s, x, y, w, h);
  SDL_RenderFillRect(g_renderer, &r);
}
static void frame(Where s, float x, float y, float w, float h, float thick, uint32_t rgb) {
  fill(s, x, y, w, thick, rgb), fill(s, x, y + h - thick, w, thick, rgb);
  fill(s, x, y, thick, h, rgb), fill(s, x + w - thick, y, thick, h, rgb);
}
static void gradient(Where s, float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
  auto c = [](uint32_t rgb) { return SDL_FColor{(rgb >> 16 & 255) / 255.f, (rgb >> 8 & 255) / 255.f, (rgb & 255) / 255.f, 1}; };
  const SDL_FRect r = to_pixels(s, x, y, w, h);
  const SDL_Vertex v[4] = {{{r.x, r.y}, c(top), {}}, {{r.x + r.w, r.y}, c(top), {}},
                           {{r.x, r.y + r.h}, c(bottom), {}}, {{r.x + r.w, r.y + r.h}, c(bottom), {}}};
  const int index[6] = {0, 1, 2, 2, 1, 3};
  SDL_RenderGeometry(g_renderer, nullptr, v, 4, index, 6);
}
enum { LEFT = 0, CENTER = 1, RIGHT = 2 };
struct Style { float size = 12; uint32_t rgb = 0x26303B; bool bold = false, mono = false, wrap = false; int align = LEFT; };
// draws s with its left (or center, or right) edge at x; width: its room (0: unbounded). Its height.
static float text(Where s, float x, float y, const std::wstring& str, Style st, float width = 0) {
  if (str.empty()) return 0;
  const Label& l = g_texts.get(str, int(st.size * g_k + 0.5f), st.bold, st.mono, int(width * g_k), st.wrap);
  if (!l.texture) return l.h / g_k;
  const SDL_FRect at = to_pixels(s, x, y, 0, 0);
  const float left = st.align == CENTER ? at.x - l.w / 2.f : st.align == RIGHT ? at.x - l.w : at.x;
  const SDL_FRect r{std::round(left), std::round(at.y), float(l.w), float(l.h)};
  SDL_SetTextureColorMod(l.texture, uint8_t(st.rgb >> 16), uint8_t(st.rgb >> 8), uint8_t(st.rgb));
  SDL_RenderTexture(g_renderer, l.texture, nullptr, &r);
  return l.h / g_k;
}
static void icon(Where s, const Game& g, float x, float y, float size) {
  if (!g.texture) {
    fill(s, x, y, size, size, 0xB9C1CB);
    text(s, x + size / 2, y + size / 2 - size * 0.3f, L"?", {size * 0.5f, 0xFFFFFF, true, false, false, CENTER});
    return;
  }
  const SDL_FRect r = to_pixels(s, x, y, size, size);
  SDL_RenderTexture(g_renderer, g.texture, nullptr, &r);
}
static bool inside(float px, float py, float x, float y, float w, float h) { return px >= x && px < x + w && py >= y && py < y + h; }

// ---- the screens ----
enum class Page { Home, Settings, Cheats, Launch };
static const uint32_t kAccent = 0x1E88E5, kInk = 0x26303B, kMuted = 0x66717E;
static const int kColumns = 5, kRows = 3; // the grid on the bottom screen: cells of 60, from (10, 30)

static void status_bar() {
  static const wchar_t* days[] = {L"dim.", L"lun.", L"mar.", L"mer.", L"jeu.", L"ven.", L"sam."};
  static const wchar_t* months[] = {L"janv.", L"févr.", L"mars", L"avr.", L"mai", L"juin", L"juil.", L"août", L"sept.", L"oct.", L"nov.", L"déc."};
  const time_t now = time(nullptr);
  tm t{};
  localtime_s(&t, &now);
  wchar_t clock[64], date[64];
  swprintf(clock, 64, L"%02d:%02d", t.tm_hour, t.tm_min);
  swprintf(date, 64, L"%ls %d %ls", days[t.tm_wday], t.tm_mday, months[t.tm_mon]);
  fill(TOP, 0, 0, 400, 18, 0xFFFFFF, 150);
  text(TOP, 8, 2, date, {11, kMuted});
  text(TOP, 392, 2, clock, {11, kInk, true, false, false, RIGHT});
}
static void top_background() {
  gradient(TOP, 0, 0, 400, 240, 0xF5F7FA, 0xD9E0E8);
  status_bar();
}
static void bottom_bars(const std::wstring& heading, const std::wstring& button, const std::wstring& hints) {
  fill(BOTTOM, 0, 0, 320, 240, 0xE4E8ED);
  fill(BOTTOM, 0, 0, 320, 26, 0xF8F9FB);
  fill(BOTTOM, 0, 26, 320, 1, 0xC9D0D8);
  text(BOTTOM, 8, 5, heading, {13, kInk, true}, 200);
  if (!button.empty()) {
    fill(BOTTOM, 236, 3, 78, 20, 0xFFFFFF);
    frame(BOTTOM, 236, 3, 78, 20, 1, 0xAEB8C4);
    text(BOTTOM, 275, 5, button, {12, kInk, false, false, false, CENTER});
  }
  fill(BOTTOM, 0, 214, 320, 26, 0xF8F9FB);
  fill(BOTTOM, 0, 213, 320, 1, 0xC9D0D8);
  text(BOTTOM, 160, 220, hints, {10.5f, kMuted, false, false, false, CENTER}, 310);
}
static void chip(float y, uint32_t rgb, const std::wstring& what) {
  fill(TOP, 26, y + 5, 7, 7, rgb);
  text(TOP, 40, y, what, {12, kInk}, 350);
}
static void game_card(const Game& g, float y) {
  fill(TOP, 18, y + 2, 366, 132, 0xC3CBD5);
  fill(TOP, 16, y, 366, 132, 0xFFFFFF);
  icon(TOP, g, 28, y + 12, 96);
  float at = y + 12;
  at += text(TOP, 138, at, g.title, {19, kInk, true, false, true}, 236) + 4;
  if (!g.description.empty() && g.description != g.title) text(TOP, 138, at, g.description, {12.5f, 0x3B4552, false, false, true}, 236);
  text(TOP, 138, y + 110, g.publisher, {11.5f, kMuted}, 236);
}

int main(int argc, char** argv) {
  Settings settings = load_settings();
  std::vector<Game> games = find_games(settings.roms);
  bool list_only = false;
  int at_x = SDL_WINDOWPOS_CENTERED, at_y = SDL_WINDOWPOS_CENTERED;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--list")) list_only = true;
    if (!std::strcmp(argv[i], "--at") && i + 1 < argc) std::sscanf(argv[++i], "%d,%d", &at_x, &at_y);
  }
  if (list_only) {
    for (const Game& g : games)
      std::printf("%016llX %-9s %-5s %s | %s | %s\n", static_cast<unsigned long long>(g.title_id),
                  g.encrypted ? "chiffree" : g.ready ? "pret" : "a-faire", g.icon.empty() ? "-" : "icone",
                  narrow(g.title).c_str(), narrow(g.publisher).c_str(), narrow(g.rom.filename().wstring()).c_str());
    return 0;
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return 1;
  SDL_Rect usable{0, 0, 1280, 1000};
  SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
  const int height = std::clamp(usable.h - 60, 480, 960);
  SDL_Window* window = SDL_CreateWindow("ctrnative", height * 400 / 480, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window || !(g_renderer = SDL_CreateRenderer(window, nullptr))) return 1;
  SDL_SetWindowPosition(window, at_x, at_y);
  SDL_SetRenderVSync(g_renderer, 1);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  g_texts.renderer = g_renderer;
  for (Game& g : games)
    if (!g.icon.empty()) {
      g.texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 48, 48);
      SDL_UpdateTexture(g.texture, nullptr, g.icon.data(), 48 * 4);
    }

  const fs::path root = CTRN_ROOT, here = SDL_GetBasePath();
  Page page = Page::Home;
  int selected = 0, top_row = 0, option = 0, games_count = int(games.size());
  std::unique_ptr<Run> run;
  int running = -1;
  uint64_t deaf_until = 0, last_window_check = 0;
  std::wstring error;
  bool quit = false;
  std::vector<Cheat> cheats; // the chosen game's, on the Cheats page
  bool has_cheats = false;
  int cheat = 0, cheat_top = 0;
  const int kCheatRows = 6;
  int option_top = 0;
  const int kOptionRows = 7;

  auto keep_visible = [&] {
    const int row = selected / kColumns;
    top_row = std::clamp(top_row, std::max(0, row - kRows + 1), row);
  };
  auto launch = [&](int i) {
    const Game& g = games[i];
    if (g.encrypted) return;
    fs::path host = here / "ctrnative.exe";
    std::wstring command = L"python -u \"" + (root / "tools" / "ctrn.py").wstring() + L"\" \"" + g.rom.wstring() + L"\"";
    if (fs::is_regular_file(host)) command += L" --host \"" + host.wstring() + L"\"";
    // 3dsrecomp for a first launch: CTRN_3DSRECOMP, else the setting, else a build beside the repository
    if (!_wgetenv(L"CTRN_3DSRECOMP")) {
      fs::path recomp = settings.recomp;
      for (const char* guess : {"3dsrecomp", "3dsrecomp-fastmem"})
        if (recomp.empty() && fs::is_regular_file(root.parent_path() / guess / "target" / "release" / "3dsrecomp.exe"))
          recomp = root.parent_path() / guess / "target" / "release" / "3dsrecomp.exe";
      if (!recomp.empty()) command += L" --3dsrecomp \"" + recomp.wstring() + L"\"";
    }
    // the settings, as the host and picanative read them; a setting off sets nothing (the game as it is)
    const int* c = settings.choice;
    auto pass = [](const wchar_t* name, const std::string& value) {
      SetEnvironmentVariableW(name, value.empty() ? nullptr : widen(value).c_str());
    };
    pass(L"CTRN_SCALE", kChoices_[SCALE].names[c[SCALE]]);
    pass(L"CTRN_LAYOUT", kChoices_[LAYOUT].names[c[LAYOUT]]);
    pass(L"CTRN_FILTER", c[FILTER] ? kChoices_[FILTER].names[c[FILTER]] : "");
    std::string effects;
    for (int e : {FXAA, SHARPEN, LCD, SCANLINES})
      if (c[e]) effects += (effects.empty() ? "" : ",") + std::string(kChoices_[e].key);
    pass(L"CTRN_EFFECTS", effects);
    pass(L"PN_ANISOTROPY", c[ANISOTROPY] ? kChoices_[ANISOTROPY].names[c[ANISOTROPY]] : "");
    pass(L"PN_MIPMAPS", c[MIPMAPS] ? "1" : "");
    pass(L"CTRN_STEREO", c[STEREO] ? kChoices_[STEREO].names[c[STEREO]] : "");
    char id[20];
    std::snprintf(id, sizeof id, "%016llX", static_cast<unsigned long long>(g.title_id));
    pass(L"PN_TEXTURE_PACK", c[PACK] ? narrow((cache_dir() / "textures" / id).wstring()) : "");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    if (fs::is_directory(L"C:\\Program Files\\LLVM\\bin")) { // clang, for the module's compilation
      std::wstring path(32767, L'\0');
      path.resize(GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size())));
      if (path.find(L"LLVM\\bin") == std::wstring::npos) SetEnvironmentVariableW(L"PATH", (L"C:\\Program Files\\LLVM\\bin;" + path).c_str());
    }
    run = std::make_unique<Run>();
    run->built = !g.ready; // the preparation's screen at once
    running = i;
    page = Page::Launch;
    error.clear();
    if (!run->begin(command, root)) error = L"Python introuvable : installez Python 3 et ajoutez-le au PATH.", run->finished = true;
  };
  auto back_home = [&] {
    if (running >= 0) games[running].ready = is_ready(games[running].title_id);
    run.reset();
    running = -1;
    page = Page::Home;
  };

  enum Action { NONE, UP, DOWN, LEFT_, RIGHT_, A, B, X, Y, QUIT };
  auto act = [&](Action a) {
    if (a == NONE || SDL_GetTicks() < deaf_until) return;
    if (page == Page::Home) {
      if (a == QUIT || a == B) quit = a == QUIT;
      if (games_count == 0) { if (a == Y) page = Page::Settings; return; }
      if (a == LEFT_ && selected > 0) --selected;
      if (a == RIGHT_ && selected + 1 < games_count) ++selected;
      if (a == UP && selected >= kColumns) selected -= kColumns;
      if (a == DOWN && selected / kColumns < (games_count - 1) / kColumns) selected = std::min(selected + kColumns, games_count - 1);
      if (a == A) launch(selected);
      if (a == Y) page = Page::Settings, option = 0;
      if (a == X) page = Page::Cheats, has_cheats = load_cheats(games[selected].title_id, cheats), cheat = cheat_top = 0;
      keep_visible();
    } else if (page == Page::Cheats) {
      const int n = int(cheats.size());
      if (a == UP && cheat > 0) --cheat;
      if (a == DOWN && cheat + 1 < n) ++cheat;
      if (a == A && cheat < n) cheats[cheat].on = !cheats[cheat].on, save_cheats(games[selected].title_id, cheats);
      if (a == B || a == QUIT || a == X) page = Page::Home;
      cheat_top = std::clamp(cheat_top, std::max(0, cheat - kCheatRows + 1), cheat);
    } else if (page == Page::Settings) {
      if (a == UP && option > 0) --option;
      if (a == DOWN && option + 1 < kChoices) ++option;
      if (a == LEFT_ || a == RIGHT_ || a == A) {
        const int n = int(kChoices_[option].values.size());
        settings.choice[option] = (settings.choice[option] + (a == LEFT_ ? n - 1 : 1)) % n;
        save_settings(settings);
      }
      if (a == B || a == QUIT || a == Y) page = Page::Home;
      option_top = std::clamp(option_top, std::max(0, option - kOptionRows + 1), option);
    } else if (run) {
      if (!run->finished && (a == B || a == QUIT)) run->cancel();
      else if (run->finished && (a == A || a == B || a == QUIT)) back_home();
    } else if (a == A || a == B || a == QUIT) back_home(); // python could not start
  };
  auto touch = [&](float x, float y) { // console pixels of the bottom screen
    if (SDL_GetTicks() < deaf_until) return;
    if (page == Page::Home) {
      if (inside(x, y, 236, 3, 78, 20)) return act(Y);
      if (inside(x, y, 152, 3, 78, 20)) return act(X);
      for (int i = top_row * kColumns; i < std::min(games_count, (top_row + kRows) * kColumns); ++i) {
        const float cx = 10 + i % kColumns * 60.f, cy = 32 + (i / kColumns - top_row) * 60.f;
        if (!inside(x, y, cx, cy, 60, 60)) continue;
        if (i == selected) return act(A); // as on the console: a touch chooses, a second one starts
        selected = i;
        return;
      }
    } else if (page == Page::Settings) {
      if (inside(x, y, 236, 3, 78, 20)) return act(B);
      const int i = option_top + int((y - 32) / 26);
      if (y >= 32 && y < 32 + kOptionRows * 26 && i < kChoices) {
        option = i;
        if (x >= 180) act(x < 245 ? LEFT_ : RIGHT_);
      }
    } else if (page == Page::Cheats) {
      if (inside(x, y, 236, 3, 78, 20)) return act(B);
      const int i = cheat_top + int((y - 32) / 30);
      if (y >= 32 && y < 32 + kCheatRows * 30 && i < int(cheats.size())) cheat = i, act(A);
    } else act(run && !run->finished ? B : A);
  };

  SDL_Gamepad* pads[8] = {};
  int stick[2] = {}; // the left stick's last direction per axis (-1, 0, 1): one step per push
  while (!quit) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, page == Page::Launch ? 50 : 250)) do {
      if (e.type == SDL_EVENT_QUIT) act(QUIT), quit = true;
      if (e.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode k = e.key.key;
        if (e.key.repeat && k != SDLK_UP && k != SDLK_DOWN && k != SDLK_LEFT && k != SDLK_RIGHT) continue;
        // the keys of picanative's window too (X/Space = A, C/Shift = B, V = Y)
        act(k == SDLK_UP || k == SDLK_W ? UP : k == SDLK_DOWN || k == SDLK_S ? DOWN : k == SDLK_LEFT || k == SDLK_A ? LEFT_
            : k == SDLK_RIGHT || k == SDLK_D ? RIGHT_ : k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_X || k == SDLK_J ? A
            : k == SDLK_BACKSPACE || k == SDLK_C || k == SDLK_K || k == SDLK_LSHIFT ? B : k == SDLK_TAB || k == SDLK_V || k == SDLK_I ? Y
            : k == SDLK_F2 || k == SDLK_U ? X
            : k == SDLK_ESCAPE ? (page == Page::Home ? QUIT : B) : NONE);
      }
      if (e.type == SDL_EVENT_GAMEPAD_ADDED)
        for (auto& p : pads) if (!p) { p = SDL_OpenGamepad(e.gdevice.which); break; }
      if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const int b = e.gbutton.button; // the console's layout, as picanative maps it: A east, B south
        act(b == SDL_GAMEPAD_BUTTON_DPAD_UP ? UP : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? DOWN : b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? LEFT_
            : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? RIGHT_ : b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_START ? A
            : b == SDL_GAMEPAD_BUTTON_SOUTH ? B : b == SDL_GAMEPAD_BUTTON_WEST || b == SDL_GAMEPAD_BUTTON_BACK ? Y
            : b == SDL_GAMEPAD_BUTTON_NORTH ? X : NONE);
      }
      if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.axis <= SDL_GAMEPAD_AXIS_LEFTY) {
        const int axis = e.gaxis.axis, now = e.gaxis.value > 16000 ? 1 : e.gaxis.value < -16000 ? -1 : 0;
        if (now && now != stick[axis]) act(axis == SDL_GAMEPAD_AXIS_LEFTX ? (now < 0 ? LEFT_ : RIGHT_) : (now < 0 ? UP : DOWN));
        stick[axis] = now;
      }
      if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_ConvertEventToRenderCoordinates(g_renderer, &e);
        touch((e.button.x - g_x[BOTTOM]) / g_k, (e.button.y - g_y[BOTTOM]) / g_k);
      }
      if (e.type == SDL_EVENT_MOUSE_WHEEL && (page == Page::Home || page == Page::Cheats)) act(e.wheel.y > 0 ? UP : e.wheel.y < 0 ? DOWN : NONE);
    } while (!quit && SDL_PollEvent(&e));

    // the game: its window found, the menu steps aside; the launcher stopped, it comes back
    if (run && page == Page::Launch) {
      if (!run->finished && SDL_GetTicks() - last_window_check > 250) {
        last_window_check = SDL_GetTicks();
        const bool open = run->game_window(), hidden = SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN;
        if (open && !hidden) run->window_seen = true, SDL_HideWindow(window);
        // the game stopped but ctrn.py goes on (it learnt an address: a rebuild, then the game again)
        if (!open && hidden) SDL_ShowWindow(window);
      }
      if (run->poll() && (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN)) {
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
        deaf_until = SDL_GetTicks() + 400; // the key that closed the game
      }
      if (run->finished && (run->cancelled || (run->window_seen && run->code == 0))) back_home();
    }
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) continue;

    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &w, &h);
    const float k = std::min(w / 400.f, h / 480.f);
    if (k != g_k) g_texts.clear(), g_k = k;
    g_x[TOP] = (w - 400 * k) / 2, g_x[BOTTOM] = (w - 320 * k) / 2;
    g_y[TOP] = (h - 480 * k) / 2, g_y[BOTTOM] = g_y[TOP] + 240 * k;
    SDL_SetRenderDrawColor(g_renderer, 0x23, 0x27, 0x2D, 255);
    SDL_RenderClear(g_renderer);
    top_background();

    if (page == Page::Home) {
      if (games.empty()) {
        text(TOP, 200, 80, L"Aucun jeu trouvé", {20, kInk, true, false, false, CENTER});
        text(TOP, 30, 115, L"Les ROM .3ds déchiffrées sont cherchées dans : " + settings.roms + L" (réglage « roms » de " +
             settings_path().wstring() + L").", {12, kMuted, false, false, true}, 340);
      } else {
        const Game& g = games[selected];
        game_card(g, 34);
        if (g.encrypted) chip(180, 0xD32F2F, L"ROM chiffrée : déchiffrez-la d'abord pour la jouer.");
        else if (g.ready) chip(180, 0x2E9D4A, L"Prêt à jouer");
        else chip(180, 0xEF8A17, L"Premier lancement : traduction et compilation, quelques minutes.");
        text(TOP, 26, 200, g.rom.filename().wstring(), {10, kMuted}, 356);
      }
      wchar_t heading[64];
      swprintf(heading, 64, L"Jeux (%d)", games_count);
      bottom_bars(heading, L"Réglages", L"A / Entrée : jouer     X / F2 : codes     Y / Tab : réglages     Échap : quitter");
      if (games_count) {
        fill(BOTTOM, 152, 3, 78, 20, 0xFFFFFF);
        frame(BOTTOM, 152, 3, 78, 20, 1, 0xAEB8C4);
        text(BOTTOM, 191, 5, L"Codes", {12, kInk, false, false, false, CENTER});
      }
      for (int i = top_row * kColumns; i < std::min(games_count, (top_row + kRows) * kColumns); ++i) {
        const float cx = 10 + i % kColumns * 60.f, cy = 32 + (i / kColumns - top_row) * 60.f;
        if (i == selected) fill(BOTTOM, cx + 2, cy + 2, 56, 56, kAccent);
        fill(BOTTOM, cx + 5, cy + 5, 50, 50, 0xFFFFFF);
        icon(BOTTOM, games[i], cx + 6, cy + 6, 48);
        if (games[i].encrypted || !games[i].ready) fill(BOTTOM, cx + 47, cy + 47, 7, 7, games[i].encrypted ? 0xD32F2F : 0xEF8A17);
      }
      const int rows = (games_count + kColumns - 1) / kColumns;
      if (rows > kRows) {
        fill(BOTTOM, 314, 32, 3, 180, 0xC9D0D8);
        fill(BOTTOM, 314, 32 + 180.f * top_row / rows, 3, 180.f * kRows / rows, 0x8A96A3);
      }
    } else if (page == Page::Settings) {
      text(TOP, 22, 28, L"Réglages", {20, kInk, true});
      fill(TOP, 16, 62, 368, 112, 0xFFFFFF);
      text(TOP, 28, 72, kChoices_[option].label, {14, kInk, true});
      text(TOP, 28, 94, kChoices_[option].about, {12, 0x3B4552, false, false, true}, 344);
      text(TOP, 22, 186, L"Pris en compte au prochain lancement d'un jeu ; en jeu, F1 ouvre ces réglages. Tout désactivé : "
                         L"le jeu tel qu'il est sur la console.", {11, kMuted, false, false, true}, 356);
      bottom_bars(L"Réglages", L"Retour", L"↑ ↓ : choisir      ← → : changer      B / Échap : retour");
      for (int i = option_top; i < std::min<int>(kChoices, option_top + kOptionRows); ++i) {
        const float y = 32 + (i - option_top) * 26.f;
        fill(BOTTOM, 6, y, 306, 24, 0xFFFFFF);
        if (i == option) frame(BOTTOM, 6, y, 306, 24, 2, kAccent);
        text(BOTTOM, 14, y + 5, kChoices_[i].label, {11.5f, kInk, true}, 166);
        text(BOTTOM, 186, y + 2, L"‹", {15, i == option ? kAccent : kMuted, true});
        text(BOTTOM, 246, y + 5, kChoices_[i].values[settings.choice[i]], {11.5f, settings.choice[i] && i != LAYOUT ? kAccent : kInk,
             false, false, false, CENTER}, 104);
        text(BOTTOM, 302, y + 2, L"›", {15, i == option ? kAccent : kMuted, true, false, false, RIGHT});
      }
      fill(BOTTOM, 315, 32, 2, 180, 0xC9D0D8);
      fill(BOTTOM, 315, 32 + 180.f * option_top / int(kChoices), 2, 180.f * kOptionRows / int(kChoices), 0x8A96A3);
    } else if (page == Page::Cheats) {
      const Game& g = games[selected];
      game_card(g, 30);
      const int on = int(std::count_if(cheats.begin(), cheats.end(), [](const Cheat& c) { return c.on; }));
      wchar_t t[120];
      if (!has_cheats) {
        chip(170, 0xEF8A17, L"Aucun fichier de codes pour ce jeu. Placez-le ici :");
        const fs::path p = cheats_path(g.title_id, "txt"); // folder and name apart: a path has no space to wrap at
        text(TOP, 40, 186, p.parent_path().wstring() + L"\\", {10.5f, kInk, false, true}, 344);
        text(TOP, 40, 200, p.filename().wstring(), {10.5f, kInk, true, true}, 344);
        text(TOP, 40, 217, L"Format Luma3DS : [Nom du code] puis des lignes XXXXXXXX YYYYYYYY", {10, kMuted}, 344);
      } else {
        if (on) swprintf(t, 120, L"%d code%ls actif%ls sur %d, au prochain lancement", on, on > 1 ? L"s" : L"", on > 1 ? L"s" : L"", int(cheats.size()));
        else swprintf(t, 120, L"Aucun code actif : le jeu d'origine (%d code%ls dans le fichier)", int(cheats.size()), cheats.size() > 1 ? L"s" : L"");
        chip(170, on ? 0xEF8A17 : 0x2E9D4A, t);
        text(TOP, 40, 190, cheats_path(g.title_id, "txt").wstring(), {10, kMuted}, 344);
      }
      wchar_t heading[64];
      swprintf(heading, 64, L"Codes (%d)", int(cheats.size()));
      bottom_bars(heading, L"Retour", L"↑ ↓ : choisir      A / Entrée : cocher      B / Échap : retour");
      if (has_cheats && cheats.empty()) text(BOTTOM, 160, 100, L"Le fichier ne contient aucun « [Nom] ».", {12, kMuted, false, false, false, CENTER});
      for (int i = cheat_top; i < std::min(int(cheats.size()), cheat_top + kCheatRows); ++i) {
        const float y = 32 + (i - cheat_top) * 30.f;
        fill(BOTTOM, 6, y, 308, 27, 0xFFFFFF);
        if (i == cheat) frame(BOTTOM, 6, y, 308, 27, 2, kAccent);
        frame(BOTTOM, 14, y + 7, 13, 13, 1.5f, cheats[i].on ? kAccent : 0x8A96A3);
        if (cheats[i].on) fill(BOTTOM, 17, y + 10, 7, 7, kAccent);
        text(BOTTOM, 36, y + 6, widen(cheats[i].name), {12, kInk, cheats[i].on}, 220);
        swprintf(t, 120, L"%d ligne%ls", cheats[i].lines, cheats[i].lines > 1 ? L"s" : L"");
        text(BOTTOM, 306, y + 7, t, {10, kMuted, false, false, false, RIGHT});
      }
      if (int(cheats.size()) > kCheatRows) {
        fill(BOTTOM, 316, 32, 2, 180, 0xC9D0D8);
        fill(BOTTOM, 316, 32 + 180.f * cheat_top / cheats.size(), 2, 180.f * kCheatRows / cheats.size(), 0x8A96A3);
      }
    } else if (running >= 0) {
      const Game& g = games[running];
      game_card(g, 30);
      std::vector<std::wstring> lines;
      Run::Phase phase = Run::STARTING;
      bool built = false;
      int to_compile = 0, compiled = 0;
      {
        std::lock_guard<std::mutex> hold(run->lock);
        lines = run->recent, phase = run->phase, built = run->built, to_compile = run->to_compile, compiled = run->compiled;
      }
      const uint64_t s = (SDL_GetTicks() - run->start) / 1000;
      wchar_t t[200];
      if (!error.empty()) chip(176, 0xD32F2F, error);
      else if (run->finished) {
        swprintf(t, 200, L"Le jeu s'est arrêté (code %lu) : voir la sortie ci-dessous.", run->code);
        chip(176, 0xD32F2F, t);
      } else if (built && phase != Run::PLAYING) {
        // the preparation (a first launch, or a module out of date): its step, a bar, the time left
        static const wchar_t* steps[] = {L"Lecture du code du jeu", L"Traduction du code du jeu en C (1/3)", L"Compilation du jeu (2/3)",
                                         L"Édition des liens (2/3)", L"Traduction des shaders (3/3)"};
        chip(170, 0xEF8A17, steps[phase]);
        swprintf(t, 200, L"%llu:%02llu", s / 60, s % 60);
        text(TOP, 374, 170, t, {12, kMuted, false, false, false, RIGHT});
        float share = 0;
        int left = -1;
        run->progress(share, left);
        fill(TOP, 26, 190, 348, 10, 0xC9D0D8);
        if (share >= 0) fill(TOP, 26, 190, 348 * share, 10, kAccent);
        else { // not measurable: a block going to and fro
          const float at = std::abs(float((SDL_GetTicks() / 8) % 560) - 280) / 280;
          fill(TOP, 26 + at * 278, 190, 70, 10, kAccent);
        }
        std::wstring detail;
        if (phase == Run::COMPILING) {
          swprintf(t, 200, L"%d %%  ·  %d / %d fichiers", int(share * 100), compiled, to_compile), detail = t;
          if (left >= 0) {
            swprintf(t, 200, left >= 60 ? L"  ·  reste environ %d min %02d s" : L"  ·  reste environ %d s", left >= 60 ? left / 60 : left, left % 60);
            detail += t;
          } else detail += L"  ·  estimation du temps restant…";
        } else if (phase == Run::LINKING) detail = L"Presque fini : assemblage du module.";
        else if (phase == Run::TRANSLATING) detail = L"En général moins d'une minute ; la compilation suit (quelques minutes).";
        else detail = L"Quelques minutes la première fois ; les lancements suivants sont immédiats.";
        text(TOP, 26, 205, detail, {11, kMuted}, 348);
      } else {
        swprintf(t, 200, L"Lancement du jeu…  %llu:%02llu", s / 60, s % 60);
        chip(176, 0x2E9D4A, t);
      }
      fill(BOTTOM, 0, 0, 320, 240, 0x1D2228);
      const int shown = 15;
      for (size_t i = lines.size() > shown ? lines.size() - shown : 0, n = 0; i < lines.size(); ++i, ++n)
        text(BOTTOM, 6, 6 + n * 13.f, lines[i], {10, 0xC9D1D9, false, true}, 308);
      fill(BOTTOM, 0, 214, 320, 26, 0x2A3038);
      text(BOTTOM, 160, 220, run && !run->finished && error.empty() ? L"B / Échap : annuler" : L"A / B : retour au menu",
           {10.5f, 0xAEB8C4, false, false, false, CENTER});
    }
    SDL_RenderPresent(g_renderer);
  }
  run.reset();
  g_texts.clear();
  SDL_Quit();
  return 0;
}
