// Explorer Watcher - Windows CE launcher (Win32 + GDI, no external libs)
// Screens: lock (clock) -> home (tiles) -> settings -> wallpaper picker; photo viewer.
// This program NEVER touches touch calibration or any system setting.
// Build: Visual Studio 2005/2008, Smart Device Win32 project, Unicode, SDK of your device.
#include <windows.h>
#include <shellapi.h>

#define MAXPICS   300

enum Screen { S_LOCK, S_HOME, S_SET, S_WALL, S_PHOTO, S_FILES };
enum Act { A_FILES, A_CARS, A_USB, A_SUBS, A_PHOTOS, A_SET };

struct Tile { const TCHAR* title; const TCHAR* sub; int app; int icon; int act; RECT r; };

// app = index into apps.ini keys (Series, Movies, Player, Files, Cars); -1 = none.
// If apps.ini has a path for the tile, that program starts. If not, the tile opens the built-in file browser.
static Tile g_t[] = {
  { TEXT("Series"),       TEXT("Browse your series"), 0, 0, A_FILES },
  { TEXT("Movies"),       NULL, 1, 1, A_FILES },
  { TEXT("Media Player"), NULL, 2, 2, A_FILES },
  { TEXT("Photos"),       NULL, -1, 7, A_PHOTOS },
  { TEXT("File Manager"), NULL, 3, 3, A_FILES },
  { TEXT("USB Mode"),     NULL, 5, 4, A_USB },
  { TEXT("Subtitles"),    NULL, 6, 5, A_SUBS },
  { TEXT("Car Apps"),     NULL, 4, 6, A_CARS },
  { TEXT("Settings"),     NULL, -1, 9, A_SET },
};
static const int N = sizeof(g_t) / sizeof(g_t[0]);

struct Theme { const TCHAR* name; COLORREF a, b; };
static Theme g_th[] = {
  { TEXT("Midnight"), RGB(14,20,27),  RGB(30,45,70)   },
  { TEXT("Ocean"),    RGB(6,42,60),   RGB(20,120,130) },
  { TEXT("Sunset"),   RGB(60,20,50),  RGB(220,120,60) },
  { TEXT("Graphite"), RGB(25,25,28),  RGB(70,72,78)   },
};
static const int NTH = 4;

static int     g_screen = S_LOCK, g_down = -1, g_W, g_H;
static int     g_theme = 0;
static TCHAR   g_wpPath[MAX_PATH] = TEXT("");      // empty = use gradient theme
static HBITMAP g_bg = NULL;
static TCHAR   g_wfiles[16][MAX_PATH];             // wallpaper picker files (full paths)
static int     g_nw = 0, g_page = 0;
static TCHAR   g_pics[MAXPICS][MAX_PATH];          // photos found on SD / USB
static int     g_np = 0, g_pi = 0;
static HBITMAP g_img = NULL;                       // current photo
static TCHAR   g_msg[64] = TEXT("");
static RECT    g_back, g_next, g_lockBtn, g_card[8], g_row[3], g_pPrev, g_pSet, g_pNext;
static int     g_barH, g_botH;
static TCHAR   g_dir[MAX_PATH];                    // folder of this exe: config + Wallpapers live here
static TCHAR   g_app[8][MAX_PATH];
static bool    g_hideBar = true;                   // hide the Windows taskbar (apps.ini: Taskbar=show to keep it)                 // optional program paths from apps.ini
struct Ent { TCHAR name[MAX_PATH]; bool dir; };
static Ent     g_fe[200]; static int g_nfe = 0, g_fpage = 0;
static TCHAR   g_cwd[MAX_PATH] = TEXT("");         // file browser folder, "" = root
static int     g_fbMode = 0;                       // 0 = browse, 1 = pick wallpaper
static int     g_photoBack = S_HOME;
static RECT    g_frow[6], g_browse;

static const COLORREF C_BG = RGB(14,20,27), C_PANEL = RGB(23,33,44), C_PANEL2 = RGB(31,44,58),
  C_INK = RGB(233,238,243), C_MUTE = RGB(132,151,168), C_TEAL = RGB(53,194,179), C_HERO = RGB(242,163,58);

// ---------- helpers ----------
static HFONT Font(int h, int w) {
  return CreateFont(-h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, DEFAULT_QUALITY, 0, TEXT("Tahoma"));
}
static void Fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void Grad(HDC dc, RECT r, COLORREF a, COLORREF b) {
  const int n = 48; int h = r.bottom - r.top;
  for (int i = 0; i < n; i++) {
    COLORREF c = RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * i / n,
                     GetGValue(a) + (GetGValue(b) - GetGValue(a)) * i / n,
                     GetBValue(a) + (GetBValue(b) - GetBValue(a)) * i / n);
    RECT s = { r.left, r.top + h * i / n, r.right, r.top + h * (i + 1) / n + 1 };
    Fill(dc, s, c);
  }
}
// CE has no MoveToEx/LineTo: use Polyline.
static void Ln(HDC dc, int x1, int y1, int x2, int y2) { POINT p[2] = { {x1,y1}, {x2,y2} }; Polyline(dc, p, 2); }
static void Panel(HDC dc, RECT r, COLORREF c) {
  HBRUSH br = CreateSolidBrush(c); HPEN pn = CreatePen(PS_SOLID, 1, c);
  HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pn);
  RoundRect(dc, r.left, r.top, r.right, r.bottom, 14, 14);
  SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pn);
}
static void Button(HDC dc, RECT r, const TCHAR* txt, int fh) {
  Panel(dc, r, C_PANEL);
  HFONT f = Font(fh, FW_BOLD); HGDIOBJ o = SelectObject(dc, f);
  SetTextColor(dc, C_INK); DrawText(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(dc, o); DeleteObject(f);
}
static const TCHAR* BaseName(const TCHAR* p) { const TCHAR* b = p; for (; *p; p++) if (*p == '\\') b = p + 1; return b; }
static bool IsImage(const TCHAR* n) {
  const TCHAR* e = NULL; for (const TCHAR* p = n; *p; p++) if (*p == '.') e = p;
  if (!e) return false;
  return !lstrcmpi(e, TEXT(".bmp")) || !lstrcmpi(e, TEXT(".jpg")) || !lstrcmpi(e, TEXT(".jpeg")) || !lstrcmpi(e, TEXT(".png")) || !lstrcmpi(e, TEXT(".gif"));
}

static void PathIn(TCHAR* out, const TCHAR* rel) { lstrcpy(out, g_dir); lstrcat(out, TEXT("\\")); lstrcat(out, rel); }
static bool IsExe(const TCHAR* n) {
  const TCHAR* e = NULL; for (const TCHAR* p = n; *p; p++) if (*p == '.') e = p;
  return e && !lstrcmpi(e, TEXT(".exe"));
}
static bool Launch(const TCHAR* path) {
  PROCESS_INFORMATION pi;
  if (!CreateProcess(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) return false;
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
}
static TCHAR* Trim(TCHAR* s) { while (*s == ' ' || *s == '\t') s++; int n = lstrlen(s); while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0; return s; }
// apps.ini (same folder as the exe), lines like:  Cars=\Storage Card\Navi\navi.exe
static void LoadApps()
{
  TCHAR p[MAX_PATH]; PathIn(p, TEXT("apps.ini"));
  HANDLE f = CreateFile(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  static char raw[4096]; static TCHAR txt[4096]; DWORD n = 0;
  ReadFile(f, raw, sizeof(raw) - 2, &n, NULL); CloseHandle(f); raw[n] = 0; raw[n + 1] = 0;
  if (n >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) lstrcpyn(txt, (TCHAR*)(raw + 2), 4096);
  else MultiByteToWideChar(CP_ACP, 0, raw, -1, txt, 4096);
  static const TCHAR* keys[8] = { TEXT("Series"), TEXT("Movies"), TEXT("Player"), TEXT("Files"), TEXT("Cars"), TEXT("Usb"), TEXT("Subs"), TEXT("Taskbar") };
  TCHAR* s = txt;
  while (*s) {
    TCHAR* e = s; while (*e && *e != '\n' && *e != '\r') e++;
    TCHAR save = *e; *e = 0;
    TCHAR* line = Trim(s);
    if (*line && *line != ';' && *line != '#') {
      TCHAR* eq = line; while (*eq && *eq != '=') eq++;
      if (*eq == '=') { *eq = 0; TCHAR* k = Trim(line); TCHAR* v = Trim(eq + 1);
        for (int i = 0; i < 8; i++) if (!lstrcmpi(k, keys[i])) lstrcpy(g_app[i], v); }
    }
    if (!save) break; s = e + 1;
  }
}
// ---------- taskbar ----------
typedef BOOL (WINAPI *PFN_SHFS)(HWND, DWORD);
static void BarVisible(HWND hw, bool show)
{
  HWND t = FindWindow(TEXT("HHTaskBar"), NULL);               // taskbar of Windows CE / Windows Mobile
  if (t) ShowWindow(t, show ? SW_SHOW : SW_HIDE);
  static bool tried = false; static PFN_SHFS fs = NULL;       // SHFullScreen exists on some devices only
  if (!tried) { tried = true; HMODULE h = LoadLibrary(TEXT("aygshell.dll")); if (h) fs = (PFN_SHFS)GetProcAddress(h, TEXT("SHFullScreen")); }
  if (fs && hw) fs(hw, show ? 0x0001 : 0x0002);               // SHFS_SHOWTASKBAR / SHFS_HIDETASKBAR
}

// ---------- built-in file browser ----------
static void ListDir()
{
  g_nfe = 0; g_fpage = 0;
  TCHAR pat[MAX_PATH]; lstrcpy(pat, g_cwd); lstrcat(pat, TEXT("\\*.*"));
  for (int pass = 0; pass < 2; pass++) {                 // folders first, then files
    WIN32_FIND_DATA fd; HANDLE h = FindFirstFile(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
      bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if (d == (pass == 0) && g_nfe < 200) { lstrcpy(g_fe[g_nfe].name, fd.cFileName); g_fe[g_nfe].dir = d; g_nfe++; }
    } while (FindNextFile(h, &fd));
    FindClose(h);
  }
}
static void UpDir() { int n = lstrlen(g_cwd); while (n > 0 && g_cwd[n-1] != '\\') n--; if (n > 0) g_cwd[n-1] = 0; else g_cwd[0] = 0; }

// ---------- picture loading ----------
// BMP is loaded natively on every CE device. JPG/PNG/GIF use SHLoadImageFile (aygshell.dll)
// only if this device has it; if not, those files show "cannot open" (convert them to BMP).
typedef HBITMAP (WINAPI *PFN_LOADIMG)(LPCTSTR);
static HBITMAP LoadPicture(const TCHAR* path)
{
  const TCHAR* e = NULL; for (const TCHAR* p = path; *p; p++) if (*p == '.') e = p;
  if (e && !lstrcmpi(e, TEXT(".bmp"))) return (HBITMAP)LoadImage(NULL, path, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
  static bool tried = false; static PFN_LOADIMG fn = NULL;
  if (!tried) { tried = true; HMODULE h = LoadLibrary(TEXT("aygshell.dll")); if (h) fn = (PFN_LOADIMG)GetProcAddress(h, TEXT("SHLoadImageFile")); }
  return fn ? fn(path) : NULL;
}

// ---------- photo scan: SD / USB cards are found automatically ----------
static void Walk(const TCHAR* dir, int depth)
{
  TCHAR pat[MAX_PATH]; if (lstrlen(dir) + 5 >= MAX_PATH) return;
  lstrcpy(pat, dir); lstrcat(pat, TEXT("\\*.*"));
  WIN32_FIND_DATA fd; HANDLE h = FindFirstFile(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (lstrlen(dir) + lstrlen(fd.cFileName) + 2 >= MAX_PATH) continue;
    TCHAR full[MAX_PATH]; lstrcpy(full, dir); lstrcat(full, TEXT("\\")); lstrcat(full, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (depth < 3 && lstrcmpi(fd.cFileName, TEXT("Windows")) && lstrcmpi(fd.cFileName, TEXT("System Volume Information"))) Walk(full, depth + 1);
    } else if (IsImage(fd.cFileName) && g_np < MAXPICS) lstrcpy(g_pics[g_np++], full);
  } while (g_np < MAXPICS && FindNextFile(h, &fd));
  FindClose(h);
}
static void ScanPics()
{
  g_np = 0;
  // storage cards (SD, USB) show up in the root as folders flagged TEMPORARY
  WIN32_FIND_DATA fd; HANDLE h = FindFirstFile(TEXT("\\*.*"), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (fd.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY)) {
        TCHAR root[MAX_PATH]; lstrcpy(root, TEXT("\\")); lstrcat(root, fd.cFileName); Walk(root, 0);
      }
    } while (FindNextFile(h, &fd));
    FindClose(h);
  }
  if (g_np == 0) {                                 // fallback: common names
    Walk(TEXT("\\Storage Card"), 0); Walk(TEXT("\\USB Disk"), 0); Walk(TEXT("\\Hard Disk"), 0);
  }
  if (g_pi >= g_np) g_pi = 0;
}

// ---------- wallpaper ----------
static void ScanWall()
{
  g_nw = 0;
  TCHAR wd[MAX_PATH], pat[MAX_PATH]; PathIn(wd, TEXT("Wallpapers")); lstrcpy(pat, wd); lstrcat(pat, TEXT("\\*.*"));
  WIN32_FIND_DATA fd; HANDLE h = FindFirstFile(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImage(fd.cFileName) && g_nw < 16) {
      lstrcpy(g_wfiles[g_nw], wd); lstrcat(g_wfiles[g_nw], TEXT("\\")); lstrcat(g_wfiles[g_nw], fd.cFileName); g_nw++;
    }
  } while (FindNextFile(h, &fd));
  FindClose(h);
}
static int TotalWall() { return NTH + g_nw; }

static void SaveCfg()
{
  TCHAR cfg[MAX_PATH]; PathIn(cfg, TEXT("wallpaper.cfg"));
  HANDLE f = CreateFile(cfg, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  TCHAR buf[MAX_PATH + 4];
  if (g_wpPath[0]) { buf[0] = 'F'; lstrcpy(buf + 1, g_wpPath); } else wsprintf(buf, TEXT("T%d"), g_theme);
  DWORD w; WriteFile(f, buf, (lstrlen(buf) + 1) * sizeof(TCHAR), &w, NULL); CloseHandle(f);
}
static void LoadCfg()
{
  TCHAR cfg[MAX_PATH]; PathIn(cfg, TEXT("wallpaper.cfg"));
  HANDLE f = CreateFile(cfg, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  TCHAR buf[MAX_PATH + 4] = {0}; DWORD r; ReadFile(f, buf, sizeof(buf) - sizeof(TCHAR), &r, NULL); CloseHandle(f);
  if (buf[0] == 'T') { g_theme = buf[1] - '0'; if (g_theme < 0 || g_theme >= NTH) g_theme = 0; }
  else if (buf[0] == 'F') lstrcpy(g_wpPath, buf + 1);
}
static void LoadBg(HWND hw)
{
  if (g_bg) { DeleteObject(g_bg); g_bg = NULL; }
  if (!g_wpPath[0]) return;                         // gradient theme: no bitmap
  HBITMAP src = LoadPicture(g_wpPath);
  if (!src) { g_wpPath[0] = 0; return; }
  HDC sd = GetDC(hw), a = CreateCompatibleDC(sd), b = CreateCompatibleDC(sd);
  g_bg = CreateCompatibleBitmap(sd, g_W, g_H);
  BITMAP bm; GetObject(src, sizeof(bm), &bm);
  HGDIOBJ oa = SelectObject(a, src), ob = SelectObject(b, g_bg);
  SetStretchBltMode(b, COLORONCOLOR);
  StretchBlt(b, 0, 0, g_W, g_H, a, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
  SelectObject(a, oa); SelectObject(b, ob); DeleteDC(a); DeleteDC(b); ReleaseDC(hw, sd); DeleteObject(src);
}
static bool SetWallpaper(HWND hw, const TCHAR* path)
{
  lstrcpy(g_wpPath, path); LoadBg(hw);
  if (!g_wpPath[0]) return false;                   // could not decode
  SaveCfg(); return true;
}
static void DrawBg(HDC m)
{
  RECT rc = { 0, 0, g_W, g_H };
  if (g_bg) { HDC s = CreateCompatibleDC(m); HGDIOBJ o = SelectObject(s, g_bg); BitBlt(m, 0, 0, g_W, g_H, s, 0, 0, SRCCOPY); SelectObject(s, o); DeleteDC(s); }
  else Grad(m, rc, g_th[g_theme].a, g_th[g_theme].b);
}

// ---------- photo viewer ----------
static void LoadPhoto()
{
  if (g_img) { DeleteObject(g_img); g_img = NULL; }
  if (g_np > 0) g_img = LoadPicture(g_pics[g_pi]);
}

// ---------- icons (drawn inside a square, never outside it) ----------
static void Icon(HDC dc, int id, int L, int T, int s, COLORREF col)
{
  HPEN pen = CreatePen(PS_SOLID, s > 60 ? 4 : 2, col);
  HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
#define X(v) (L + (v) * s / 24)
#define Y(v) (T + (v) * s / 24)
  switch (id) {
  case 0: RoundRect(dc, X(3),Y(5),X(21),Y(17),6,6); Ln(dc,X(8),Y(21),X(16),Y(21)); Ln(dc,X(12),Y(17),X(12),Y(21)); break;
  case 1: RoundRect(dc, X(3),Y(4),X(21),Y(20),6,6); Ln(dc,X(8),Y(4),X(8),Y(20)); Ln(dc,X(16),Y(4),X(16),Y(20)); break;
  case 2: { Ellipse(dc, X(3),Y(3),X(21),Y(21)); POINT p[3] = { {X(10),Y(8)}, {X(16),Y(12)}, {X(10),Y(16)} }; Polygon(dc, p, 3); } break;
  case 3: { POINT p[7] = { {X(3),Y(6)}, {X(9),Y(6)}, {X(11),Y(9)}, {X(21),Y(9)}, {X(21),Y(19)}, {X(3),Y(19)}, {X(3),Y(6)} }; Polyline(dc, p, 7); } break;
  case 4: Ln(dc,X(12),Y(3),X(12),Y(17)); Ellipse(dc,X(9),Y(17),X(15),Y(23)); Ln(dc,X(12),Y(12),X(7),Y(9)); Ln(dc,X(12),Y(9),X(17),Y(7)); break;
  case 5: RoundRect(dc, X(3),Y(5),X(21),Y(19),6,6); Ln(dc,X(6),Y(11),X(10),Y(11)); Ln(dc,X(12),Y(11),X(18),Y(11)); Ln(dc,X(6),Y(15),X(12),Y(15)); Ln(dc,X(14),Y(15),X(18),Y(15)); break;
  case 6: { POINT p[7] = { {X(4),Y(16)}, {X(4),Y(12)}, {X(6),Y(7)}, {X(18),Y(7)}, {X(20),Y(12)}, {X(20),Y(16)}, {X(4),Y(16)} }; Polyline(dc, p, 7); }
          Ellipse(dc,X(6),Y(15),X(10),Y(19)); Ellipse(dc,X(14),Y(15),X(18),Y(19)); break;
  case 7: RoundRect(dc, X(3),Y(4),X(21),Y(20),6,6); { POINT p[5] = { {X(3),Y(17)}, {X(9),Y(11)}, {X(14),Y(16)}, {X(17),Y(13)}, {X(21),Y(17)} }; Polyline(dc, p, 5); }
          Ellipse(dc,X(15),Y(6),X(18),Y(9)); break;
  case 8: RoundRect(dc, X(5),Y(11),X(19),Y(21),4,4);
          { POINT p[6] = { {X(8),Y(11)}, {X(8),Y(7)}, {X(10),Y(4)}, {X(14),Y(4)}, {X(16),Y(7)}, {X(16),Y(11)} }; Polyline(dc, p, 6); } break;
  case 9: Ellipse(dc,X(6),Y(6),X(18),Y(18)); Ellipse(dc,X(10),Y(10),X(14),Y(14));
          Ln(dc,X(12),Y(2),X(12),Y(6)); Ln(dc,X(12),Y(18),X(12),Y(22)); Ln(dc,X(2),Y(12),X(6),Y(12)); Ln(dc,X(18),Y(12),X(22),Y(12));
          Ln(dc,X(5),Y(5),X(8),Y(8)); Ln(dc,X(16),Y(16),X(19),Y(19)); Ln(dc,X(19),Y(5),X(16),Y(8)); Ln(dc,X(5),Y(19),X(8),Y(16)); break;
  }
#undef X
#undef Y
  SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
}

// ---------- layout ----------
static void Layout(int W, int H)
{
  g_W = W; g_H = H;
  int bar = H / 10, pad = W / 50, gap = W * 14 / 1000;
  g_barH = bar; g_botH = H / 8;
  int gw = W - 2 * pad, gh = H - bar - pad - gap;
  int unit = (gw - 4 * gap) * 100 / 525;            // 4*1.0 + 1.25 units
  int heroW = unit * 125 / 100, ch = (gh - gap) / 2;
  g_t[0].r.left = pad; g_t[0].r.top = bar + gap; g_t[0].r.right = pad + heroW; g_t[0].r.bottom = bar + gap + gh;
  for (int i = 1; i < N; i++) {
    int k = i - 1, col = k % 4, row = k / 4;
    g_t[i].r.left = pad + heroW + gap + col * (unit + gap);
    g_t[i].r.top = bar + gap + row * (ch + gap);
    g_t[i].r.right = g_t[i].r.left + unit; g_t[i].r.bottom = g_t[i].r.top + ch;
  }
  g_back.left = pad; g_back.top = H / 40; g_back.right = pad + W / 6; g_back.bottom = bar - H / 40;
  g_next.top = g_back.top; g_next.bottom = g_back.bottom; g_next.left = W * 48 / 100; g_next.right = W * 64 / 100;
  g_lockBtn = g_next; g_lockBtn.right = W - pad; g_lockBtn.left = W - pad - W / 8;
  g_browse = g_next; g_browse.left = W * 66 / 100; g_browse.right = W - pad;
  int cw = (gw - 3 * gap) / 4, cH = (H - bar - pad - gap) / 2 - gap;
  for (int k = 0; k < 8; k++) {
    int col = k % 4, row = k / 4;
    g_card[k].left = pad + col * (cw + gap); g_card[k].top = bar + gap + row * (cH + gap);
    g_card[k].right = g_card[k].left + cw;   g_card[k].bottom = g_card[k].top + cH;
  }
  int rh = H * 16 / 100;
  for (int k = 0; k < 3; k++) {
    g_row[k].left = pad; g_row[k].right = W - pad;
    g_row[k].top = bar + gap * 2 + k * (rh + gap); g_row[k].bottom = g_row[k].top + rh;
  }
  int frh = (H - bar - g_botH - gap) / 6;
  for (int k = 0; k < 6; k++) { g_frow[k].left = pad; g_frow[k].right = W - pad; g_frow[k].top = bar + gap / 2 + k * frh; g_frow[k].bottom = g_frow[k].top + frh - 4; }
  int by0 = H - g_botH + 6, by1 = H - 6;
  g_pPrev.left = pad; g_pPrev.right = W * 30 / 100; g_pSet.left = W * 32 / 100; g_pSet.right = W * 68 / 100;
  g_pNext.left = W * 70 / 100; g_pNext.right = W - pad;
  g_pPrev.top = g_pSet.top = g_pNext.top = by0; g_pPrev.bottom = g_pSet.bottom = g_pNext.bottom = by1;
}

// ---------- painting ----------
static void TimeStr(TCHAR* t, TCHAR* d)
{
  SYSTEMTIME st; GetLocalTime(&st);
  wsprintf(t, TEXT("%02d:%02d"), st.wHour, st.wMinute);
  if (d) GetDateFormat(LOCALE_USER_DEFAULT, 0, &st, TEXT("dddd, d MMMM"), d, 64);
}
static void TopBar(HDC m, const TCHAR* title, bool back, const TCHAR* right)
{
  RECT bar = { 0, 0, g_W, g_barH }; Fill(m, bar, C_BG); SetBkMode(m, TRANSPARENT);
  if (back) Button(m, g_back, TEXT("< Back"), g_H / 20);
  HFONT f = Font(g_H / 20, FW_BOLD); HGDIOBJ o = SelectObject(m, f);
  RECT t = back ? g_back : bar; t.left = back ? g_back.right + 10 : g_W * 3 / 100; t.top = 0; t.bottom = g_barH; t.right = g_W * 70 / 100;
  SetTextColor(m, C_INK); DrawText(m, title, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (right) { RECT r = { g_W / 2, 0, g_W - g_W / 50, g_barH }; SetTextColor(m, C_MUTE); DrawText(m, right, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
  SelectObject(m, o); DeleteObject(f);
}

static void PaintLock(HDC m)
{
  DrawBg(m); SetBkMode(m, TRANSPARENT);
  TCHAR t[16], d[64]; TimeStr(t, d);
  HFONT fT = Font(g_H / 3, FW_LIGHT), fD = Font(g_H / 13, FW_NORMAL), fH = Font(g_H / 22, FW_NORMAL);
  HGDIOBJ of = SelectObject(m, fT); SetTextColor(m, RGB(255,255,255));
  RECT r1 = { 0, g_H * 14 / 100, g_W, g_H * 56 / 100 }; DrawText(m, t, -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(m, fD);
  RECT r2 = { 0, g_H * 56 / 100, g_W, g_H * 68 / 100 }; DrawText(m, d, -1, &r2, DT_CENTER | DT_TOP | DT_SINGLELINE);
  SelectObject(m, fH); SetTextColor(m, RGB(220,226,232));
  RECT r3 = { 0, g_H * 86 / 100, g_W, g_H * 96 / 100 }; DrawText(m, TEXT("Touch the screen to unlock"), -1, &r3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(m, of); DeleteObject(fT); DeleteObject(fD); DeleteObject(fH);
}

static void PaintHome(HDC m)
{
  DrawBg(m);
  TCHAR t[16]; TimeStr(t, NULL);
  TopBar(m, TEXT("Explorer Watcher"), false, NULL);
  HFONT fc = Font(g_H / 20, FW_NORMAL); HGDIOBJ oc = SelectObject(m, fc); SetTextColor(m, C_MUTE);
  RECT cr = { g_W / 2, 0, g_lockBtn.left - 14, g_barH }; DrawText(m, t, -1, &cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  SelectObject(m, oc); DeleteObject(fc);
  Button(m, g_lockBtn, TEXT("Lock"), g_H / 24);

  for (int i = 0; i < N; i++) {
    Tile& tl = g_t[i]; bool hero = (i == 0), down = (i == g_down);
    COLORREF fill = hero ? C_HERO : (down ? C_PANEL2 : C_PANEL);
    HBRUSH br = CreateSolidBrush(fill); HPEN pn = CreatePen(PS_SOLID, down ? 3 : 1, down ? C_TEAL : fill);
    HGDIOBJ ob = SelectObject(m, br), op = SelectObject(m, pn);
    RoundRect(m, tl.r.left, tl.r.top, tl.r.right, tl.r.bottom, 14, 14);
    SelectObject(m, ob); SelectObject(m, op); DeleteObject(br); DeleteObject(pn);

    int tw = tl.r.right - tl.r.left, th = tl.r.bottom - tl.r.top, m8 = 12;
    int is = hero ? th * 38 / 100 : th * 40 / 100;
    if (is > tw - 2 * m8) is = tw - 2 * m8;
    Icon(m, tl.icon, tl.r.left + m8, tl.r.top + m8, is, hero ? RGB(26,18,4) : C_TEAL);

    // text area = everything under the icon square (separate rect -> no overlap)
    RECT tr = { tl.r.left + m8, tl.r.top + m8 + is + 4, tl.r.right - m8, tl.r.bottom - m8 };
    HFONT ft = Font(hero ? g_H / 11 : g_H / 22, FW_BOLD); HGDIOBJ o2 = SelectObject(m, ft);
    SetTextColor(m, hero ? RGB(26,18,4) : C_INK);
    UINT fl = DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (hero && tl.sub) {
      int sh = g_H / 24; HFONT fs = Font(sh, FW_NORMAL);
      RECT sr = { tr.left, tr.bottom - sh - 2, tr.right, tr.bottom }; tr.bottom -= sh + 4;
      DrawText(m, tl.title, -1, &tr, fl);
      SelectObject(m, fs); SetTextColor(m, RGB(74,53,16));
      DrawText(m, tl.sub, -1, &sr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
      SelectObject(m, ft); DeleteObject(fs);
    } else DrawText(m, tl.title, -1, &tr, fl);
    SelectObject(m, o2); DeleteObject(ft);
  }
}

static void PaintSettings(HDC m)
{
  RECT all = { 0, 0, g_W, g_H }; Fill(m, all, C_BG);
  TopBar(m, TEXT("Settings"), true, NULL);
  TCHAR sub2[80]; wsprintf(sub2, TEXT("Found %d photos on SD / USB"), g_np);
  const TCHAR* tt[3] = { TEXT("Wallpaper"), TEXT("Lock now"), TEXT("Photo sources") };
  const TCHAR* ss[3] = { TEXT("Choose a colour or a picture"), TEXT("Show the lock screen"), sub2 };
  int ic[3] = { 7, 8, 3 };
  for (int k = 0; k < 3; k++) {
    Panel(m, g_row[k], C_PANEL);
    int rh = g_row[k].bottom - g_row[k].top, is = rh * 55 / 100;
    Icon(m, ic[k], g_row[k].left + 16, g_row[k].top + (rh - is) / 2, is, C_TEAL);
    int tx = g_row[k].left + 16 + is + 16;
    HFONT f1 = Font(g_H / 15, FW_BOLD), f2 = Font(g_H / 26, FW_NORMAL);
    HGDIOBJ o = SelectObject(m, f1); SetTextColor(m, C_INK);
    RECT r1 = { tx, g_row[k].top + 6, g_row[k].right - 50, g_row[k].top + rh / 2 + 6 };
    DrawText(m, tt[k], -1, &r1, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(m, f2); SetTextColor(m, C_MUTE);
    RECT r2 = { tx, g_row[k].top + rh / 2 + 8, g_row[k].right - 50, g_row[k].bottom - 6 };
    DrawText(m, ss[k], -1, &r2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT r3 = { g_row[k].right - 50, g_row[k].top, g_row[k].right - 10, g_row[k].bottom };
    SelectObject(m, f1); DrawText(m, TEXT(">"), -1, &r3, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(m, o); DeleteObject(f1); DeleteObject(f2);
  }
}

static bool WallSelected(int idx)
{
  if (idx < NTH) return !g_wpPath[0] && g_theme == idx;
  return lstrcmpi(g_wfiles[idx - NTH], g_wpPath) == 0;
}
static void PaintWall(HDC m)
{
  RECT all = { 0, 0, g_W, g_H }; Fill(m, all, C_BG);
  TopBar(m, TEXT("Wallpaper"), true, NULL);
  if (TotalWall() > 8) Button(m, g_next, TEXT("Next >"), g_H / 20);
  Button(m, g_browse, TEXT("Browse folders"), g_H / 24);
  HFONT fS = Font(g_H / 24, FW_NORMAL); HGDIOBJ of = SelectObject(m, fS);
  for (int k = 0; k < 8; k++) {
    int idx = g_page * 8 + k; if (idx >= TotalWall()) break;
    RECT c = g_card[k];
    if (idx < NTH) Grad(m, c, g_th[idx].a, g_th[idx].b); else Fill(m, c, C_PANEL);
    if (WallSelected(idx)) {
      HPEN pn = CreatePen(PS_SOLID, 4, C_TEAL); HGDIOBJ op = SelectObject(m, pn), ob = SelectObject(m, GetStockObject(NULL_BRUSH));
      Rectangle(m, c.left, c.top, c.right, c.bottom); SelectObject(m, op); SelectObject(m, ob); DeleteObject(pn);
    }
    RECT tr = { c.left + 10, c.top + 6, c.right - 10, c.bottom - 8 };
    SetTextColor(m, idx < NTH ? RGB(255,255,255) : C_MUTE);
    DrawText(m, idx < NTH ? g_th[idx].name : BaseName(g_wfiles[idx - NTH]), -1, &tr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  SelectObject(m, of); DeleteObject(fS);
}

static void PaintFiles(HDC m)
{
  RECT all = { 0, 0, g_W, g_H }; Fill(m, all, C_BG);
  int pages = (g_nfe + 5) / 6; if (pages < 1) pages = 1;
  TCHAR rt[48]; wsprintf(rt, g_fbMode ? TEXT("Pick a picture   %d/%d") : TEXT("%d/%d"), g_fpage + 1, pages);
  TopBar(m, g_cwd[0] ? g_cwd : TEXT("\\  (all storage)"), true, rt);
  HFONT f = Font(g_H / 18, FW_NORMAL); HGDIOBJ o = SelectObject(m, f); SetBkMode(m, TRANSPARENT);
  for (int k = 0; k < 6; k++) {
    int idx = g_fpage * 6 + k; if (idx >= g_nfe) break;
    RECT r = g_frow[k]; Panel(m, r, C_PANEL);
    int rh = r.bottom - r.top, is = rh * 60 / 100;
    int ic = g_fe[idx].dir ? 3 : (IsImage(g_fe[idx].name) ? 7 : 2);
    Icon(m, ic, r.left + 14, r.top + (rh - is) / 2, is, g_fe[idx].dir ? C_HERO : C_TEAL);
    RECT t = { r.left + 14 + is + 14, r.top, r.right - 14, r.bottom };
    SetTextColor(m, C_INK); DrawText(m, g_fe[idx].name, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  if (g_nfe == 0) { SetTextColor(m, C_MUTE); RECT t = { 0, g_barH, g_W, g_H - g_botH }; DrawText(m, TEXT("This folder is empty"), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
  SelectObject(m, o); DeleteObject(f);
  RECT bot = { 0, g_H - g_botH, g_W, g_H }; Fill(m, bot, C_BG);
  Button(m, g_pPrev, TEXT("< Up folder"), g_H / 20);
  Button(m, g_pSet, TEXT("Previous page"), g_H / 22);
  Button(m, g_pNext, TEXT("Next page"), g_H / 22);
}

static void PaintPhoto(HDC m)
{
  RECT all = { 0, 0, g_W, g_H }; Fill(m, all, RGB(0,0,0));
  TCHAR cnt[32]; wsprintf(cnt, TEXT("%d / %d"), g_np ? g_pi + 1 : 0, g_np);
  TopBar(m, g_np ? BaseName(g_pics[g_pi]) : TEXT("Photos"), true, cnt);
  RECT bot = { 0, g_H - g_botH, g_W, g_H }; Fill(m, bot, C_BG);
  Button(m, g_pPrev, TEXT("< Previous"), g_H / 20);
  Button(m, g_pSet, TEXT("Set as wallpaper"), g_H / 20);
  Button(m, g_pNext, TEXT("Next >"), g_H / 20);
  SetBkMode(m, TRANSPARENT);
  int ax = 0, ay = g_barH, aw = g_W, ah = g_H - g_barH - g_botH;
  if (g_img) {
    BITMAP bm; GetObject(g_img, sizeof(bm), &bm);
    int dw, dh;
    if (bm.bmWidth * ah > bm.bmHeight * aw) { dw = aw; dh = bm.bmHeight * aw / bm.bmWidth; } else { dh = ah; dw = bm.bmWidth * ah / bm.bmHeight; }
    HDC s = CreateCompatibleDC(m); HGDIOBJ o = SelectObject(s, g_img);
    SetStretchBltMode(m, COLORONCOLOR);
    StretchBlt(m, ax + (aw - dw) / 2, ay + (ah - dh) / 2, dw, dh, s, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    SelectObject(s, o); DeleteDC(s);
  } else {
    HFONT f = Font(g_H / 22, FW_NORMAL); HGDIOBJ o = SelectObject(m, f); SetTextColor(m, C_MUTE);
    RECT r = { ax + 20, ay, ax + aw - 20, ay + ah };
    DrawText(m, g_np ? TEXT("Cannot open this file.\nBMP always works. JPG/PNG need a decoder on this device.")
                     : TEXT("No photos found on SD or USB."), -1, &r, DT_CENTER | DT_VCENTER);
    SelectObject(m, o); DeleteObject(f);
  }
  if (g_msg[0]) {
    HFONT f = Font(g_H / 24, FW_BOLD); HGDIOBJ o = SelectObject(m, f);
    RECT r = { g_W / 4, g_H - g_botH - g_H / 9, g_W * 3 / 4, g_H - g_botH - 10 };
    Panel(m, r, RGB(0,0,0)); SetTextColor(m, C_INK); DrawText(m, g_msg, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(m, o); DeleteObject(f);
  }
}

static void Paint(HWND hw)
{
  PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
  HDC m = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, g_W, g_H); HGDIOBJ ob = SelectObject(m, bmp);
  switch (g_screen) {
  case S_LOCK: PaintLock(m); break;   case S_HOME: PaintHome(m); break;
  case S_SET: PaintSettings(m); break; case S_WALL: PaintWall(m); break;
  case S_FILES: PaintFiles(m); break;
  default: PaintPhoto(m); break;
  }
  BitBlt(dc, 0, 0, g_W, g_H, m, 0, 0, SRCCOPY);
  SelectObject(m, ob); DeleteObject(bmp); DeleteDC(m); EndPaint(hw, &ps);
}

// ---------- input ----------
static int HitTile(int x, int y) { POINT p = { x, y }; for (int i = 0; i < N; i++) if (PtInRect(&g_t[i].r, p)) return i; return -1; }
static void Go(HWND hw, int s) { g_screen = s; InvalidateRect(hw, NULL, FALSE); }
static void Say(HWND hw, const TCHAR* t) { lstrcpy(g_msg, t); SetTimer(hw, 2, 1600, NULL); InvalidateRect(hw, NULL, FALSE); }
static void Step(HWND hw, int d) { if (g_np < 1) return; g_pi = (g_pi + d + g_np) % g_np; LoadPhoto(); InvalidateRect(hw, NULL, FALSE); }

static void OpenFolderBrowser(HWND hw, int mode)
{
  g_fbMode = mode; g_cwd[0] = 0; ListDir(); Go(hw, S_FILES);
}
static void RunTile(HWND hw, int i)
{
  Tile& t = g_t[i]; TCHAR b[400];
  if (t.app >= 0 && g_app[t.app][0]) {
    if (GetFileAttributes(g_app[t.app]) == 0xFFFFFFFF) { wsprintf(b, TEXT("Program not found:\n%s\n\nCheck apps.ini."), g_app[t.app]); MessageBox(NULL, b, t.title, MB_OK); }
    else if (!Launch(g_app[t.app])) MessageBox(NULL, TEXT("Could not start the program."), t.title, MB_OK);
    return;
  }
  if (t.act == A_FILES) { OpenFolderBrowser(hw, 0); return; }
  if (t.act == A_CARS) {
    TCHAR ini[MAX_PATH]; PathIn(ini, TEXT("apps.ini"));
    wsprintf(b, TEXT("Car Apps needs a program path.\nOpen this file and add a line:\n%s\n\nCars=\\path\\to\\program.exe"), ini);
    MessageBox(NULL, b, t.title, MB_OK); return;
  }
  if (t.act == A_USB) {                                        // generic try: ActiveSync helper, if this device has it
    if (GetFileAttributes(TEXT("\\Windows\\repllog.exe")) != 0xFFFFFFFF && Launch(TEXT("\\Windows\\repllog.exe"))) return;
    TCHAR ini[MAX_PATH]; PathIn(ini, TEXT("apps.ini"));
    wsprintf(b, TEXT("USB mode is device specific.\nFind the program your device uses for it and add a line to:\n%s\n\nUsb=\\path\\to\\program.exe"), ini);
    MessageBox(NULL, b, t.title, MB_OK); return;
  }
  if (t.act == A_SUBS) {
    MessageBox(NULL, TEXT("Subtitles belong to the video player.\nSet Player= in apps.ini to a player that supports .srt (for example TCPMP or CorePlayer), or Subs= to a subtitle program."), t.title, MB_OK); return;
  }
}
static void OpenEntry(HWND hw, int i)
{
  Ent& e = g_fe[i]; TCHAR full[MAX_PATH];
  if (lstrlen(g_cwd) + lstrlen(e.name) + 2 >= MAX_PATH) return;
  lstrcpy(full, g_cwd); lstrcat(full, TEXT("\\")); lstrcat(full, e.name);
  if (e.dir) { lstrcpy(g_cwd, full); ListDir(); InvalidateRect(hw, NULL, FALSE); return; }
  if (IsImage(e.name)) {
    if (g_fbMode == 1) {
      if (SetWallpaper(hw, full)) MessageBox(NULL, TEXT("Wallpaper set."), TEXT("Wallpaper"), MB_OK);
      else MessageBox(NULL, TEXT("Cannot open this picture.\nBMP works on every device; JPG/PNG need a decoder."), TEXT("Wallpaper"), MB_OK);
      Go(hw, S_WALL); return;
    }
    g_np = 0; int start = 0;                                   // viewer shows every picture of this folder
    for (int k = 0; k < g_nfe; k++)
      if (!g_fe[k].dir && IsImage(g_fe[k].name) && g_np < MAXPICS) {
        if (k == i) start = g_np;
        lstrcpy(g_pics[g_np], g_cwd); lstrcat(g_pics[g_np], TEXT("\\")); lstrcat(g_pics[g_np], g_fe[k].name); g_np++;
      }
    g_pi = start; LoadPhoto(); g_photoBack = S_FILES; Go(hw, S_PHOTO); return;
  }
  if (g_fbMode == 1) return;                                   // wallpaper mode: only pictures
  if (IsExe(e.name)) { if (!Launch(full)) MessageBox(NULL, TEXT("Could not start this program."), e.name, MB_OK); return; }
  SHELLEXECUTEINFO sei; ZeroMemory(&sei, sizeof(sei));         // videos, music, etc: open with the device's own player
  sei.cbSize = sizeof(sei); sei.lpVerb = TEXT("open"); sei.lpFile = full; sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteEx(&sei)) MessageBox(NULL, TEXT("No program is set to open this file type."), e.name, MB_OK);
}

static LRESULT CALLBACK Proc(HWND hw, UINT m, WPARAM w, LPARAM l)
{
  int x = LOWORD(l), y = HIWORD(l); POINT p = { x, y };
  switch (m) {
  case WM_SIZE: if (LOWORD(l) && HIWORD(l)) { Layout(LOWORD(l), HIWORD(l)); LoadBg(hw); } return 0;
  case WM_PAINT: Paint(hw); return 0;
  case WM_ERASEBKGND: return 1;
  case WM_TIMER:
    if (w == 2) { KillTimer(hw, 2); g_msg[0] = 0; }
    if (w == 1 && g_hideBar) BarVisible(hw, false);            // keep it hidden if the system brings it back
    InvalidateRect(hw, NULL, FALSE); return 0;
  case WM_LBUTTONDOWN:
    if (g_screen == S_HOME) { g_down = HitTile(x, y); InvalidateRect(hw, NULL, FALSE); }
    return 0;
  case WM_LBUTTONUP:
    switch (g_screen) {
    case S_LOCK: Go(hw, S_HOME); break;
    case S_HOME: {
      int i = HitTile(x, y), d = g_down; g_down = -1;
      if (PtInRect(&g_lockBtn, &p)) { Go(hw, S_LOCK); break; }
      InvalidateRect(hw, NULL, FALSE); UpdateWindow(hw);
      if (i >= 0 && i == d) {
        if (g_t[i].act == A_SET) { ScanPics(); Go(hw, S_SET); }
        else if (g_t[i].act == A_PHOTOS) {
          ScanPics();
          if (g_np == 0) MessageBox(NULL, TEXT("No photos found automatically.\nOpen File Manager and browse to your folder."), TEXT("Photos"), MB_OK);
          else { LoadPhoto(); g_photoBack = S_HOME; Go(hw, S_PHOTO); }
        } else RunTile(hw, i);
      }
      break; }
    case S_SET:
      if (PtInRect(&g_back, &p)) Go(hw, S_HOME);
      else if (PtInRect(&g_row[0], &p)) { ScanWall(); g_page = 0; Go(hw, S_WALL); }
      else if (PtInRect(&g_row[1], &p)) Go(hw, S_LOCK);
      else if (PtInRect(&g_row[2], &p)) {
        TCHAR b[160]; ScanPics();
        wsprintf(b, TEXT("Found %d photos.\nSD and USB cards are detected automatically.\nBMP always works; JPG/PNG need a decoder on this device."), g_np);
        MessageBox(NULL, b, TEXT("Photo sources"), MB_OK); InvalidateRect(hw, NULL, FALSE);
      }
      break;
    case S_WALL:
      if (PtInRect(&g_back, &p)) { Go(hw, S_SET); break; }
      if (PtInRect(&g_browse, &p)) { OpenFolderBrowser(hw, 1); break; }
      if (TotalWall() > 8 && PtInRect(&g_next, &p)) { g_page = (g_page + 1) % ((TotalWall() + 7) / 8); InvalidateRect(hw, NULL, FALSE); break; }
      for (int k = 0; k < 8; k++) if (PtInRect(&g_card[k], &p)) {
        int idx = g_page * 8 + k; if (idx >= TotalWall()) break;
        if (idx < NTH) { g_wpPath[0] = 0; g_theme = idx; LoadBg(hw); SaveCfg(); }
        else SetWallpaper(hw, g_wfiles[idx - NTH]);
        InvalidateRect(hw, NULL, FALSE); break;
      }
      break;
    case S_FILES: {
      int pages = (g_nfe + 5) / 6; if (pages < 1) pages = 1;
      if (PtInRect(&g_back, &p)) Go(hw, g_fbMode ? S_WALL : S_HOME);
      else if (PtInRect(&g_pPrev, &p)) { UpDir(); ListDir(); InvalidateRect(hw, NULL, FALSE); }
      else if (PtInRect(&g_pSet, &p)) { if (g_fpage > 0) g_fpage--; InvalidateRect(hw, NULL, FALSE); }
      else if (PtInRect(&g_pNext, &p)) { if (g_fpage < pages - 1) g_fpage++; InvalidateRect(hw, NULL, FALSE); }
      else for (int k = 0; k < 6; k++) if (PtInRect(&g_frow[k], &p)) { int idx = g_fpage * 6 + k; if (idx < g_nfe) OpenEntry(hw, idx); break; }
      break; }
    case S_PHOTO:
      if (PtInRect(&g_back, &p)) { if (g_img) { DeleteObject(g_img); g_img = NULL; } Go(hw, g_photoBack); }
      else if (PtInRect(&g_pPrev, &p)) Step(hw, -1);
      else if (PtInRect(&g_pNext, &p)) Step(hw, 1);
      else if (PtInRect(&g_pSet, &p) && g_np > 0) Say(hw, SetWallpaper(hw, g_pics[g_pi]) ? TEXT("Wallpaper set") : TEXT("Cannot use this file"));
      break;
    }
    return 0;
  case WM_DESTROY: BarVisible(hw, true); PostQuitMessage(0); return 0;
  }
  return DefWindowProc(hw, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPTSTR, int)
{
  HANDLE mx = CreateMutex(NULL, TRUE, TEXT("ExplorerWatcherLauncher"));
  if (GetLastError() == ERROR_ALREADY_EXISTS) { HWND o = FindWindow(TEXT("EWLauncher"), NULL); if (o) SetForegroundWindow(o); return 0; }
  WNDCLASS wc = {0}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = TEXT("EWLauncher");
  RegisterClass(&wc);
  int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
  GetModuleFileName(NULL, g_dir, MAX_PATH);                  // config files live next to the exe
  { int n = lstrlen(g_dir); while (n > 0 && g_dir[n-1] != '\\') n--; if (n > 0) g_dir[n-1] = 0; }
  LoadApps();
  Layout(W, H); LoadCfg();
  HWND hw = CreateWindow(TEXT("EWLauncher"), TEXT("Explorer Watcher"), WS_VISIBLE | WS_POPUP, 0, 0, W, H, NULL, NULL, hi, NULL);
  if (!lstrcmpi(g_app[7], TEXT("show"))) g_hideBar = false;
  if (g_hideBar) BarVisible(hw, false);
  LoadBg(hw); SetTimer(hw, 1, 10000, NULL);
  ShowWindow(hw, SW_SHOW); UpdateWindow(hw);
  MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
  CloseHandle(mx); return (int)msg.wParam;
}
