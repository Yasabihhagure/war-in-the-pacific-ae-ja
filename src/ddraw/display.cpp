// ウィンドウモード (-w) の画面を拡大して表示する
//
// - 窓を作るとき (CreateWindowExA) に、画面に収まる最大の倍率の大きさにする
// - ゲームが裏の画像を窓へ転送するとき (主サーフェスへの Blt) に、拡大して転送する
// - 窓に届くマウスの座標を倍率で割って、ゲームに渡す
//
// ゲームの描画の大きさは変えない (ウィンドウモードでは起動直後 800x600、その後 1024x768 など)。
// ゲームの解像度は [0x886a58] (幅) と [0x886a5c] (高さ) から読む。
// ゲームは WM_GETMINMAXINFO で窓の大きさを解像度+枠に固定するので、その制限を外す。
#include "common.h"
#include <cmath>
#include <cstring>

namespace {

// 設定 (witpae_ja.ini の [Display])
bool  g_enable = true;     // Scale: 0 で無効
double g_scaleCfg = 0;     // Scale: 0=自動 (画面に収まる最大), それ以外は倍率
bool  g_borderless = false;
bool  g_smooth = true;
int   g_gw = 1024, g_gh = 768; // ゲームの画面の大きさ (GameSize() で更新)
int   g_defW = 1024, g_defH = 768; // 読めないときの既定値 (ini の GameWidth/GameHeight)
int* const P_GAME_W = (int*)0x886a58;
int* const P_GAME_H = (int*)0x886a5c;

// 状態
bool  g_active = false;    // 拡大中
int   g_fw = 0, g_fh = 0;  // 窓の枠の太さ (外側 - 中身)
RECT  g_area = {};         // 窓を置く範囲 (作業領域、枠なしならモニター全体)
bool  g_inFit = false;
int   g_bltCount = 0;    // 主サーフェスへの Blt の回数
void* g_backSurf = nullptr; // 主サーフェスへ全体を転送するときの転送元 (裏の画像)
HWND  g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;
void* g_primary = nullptr;
double g_scale = 1.0;
int   g_offX = 0, g_offY = 0;  // 窓の中の、拡大した画像の左上 (左右・上下の余白)

// ゲームの解像度を読む。変わっていたら true
bool GameSize() {
    int w = *P_GAME_W, h = *P_GAME_H;
    if (w < 320 || w > 4096 || h < 200 || h > 4096) { w = g_defW; h = g_defH; }
    if (w == g_gw && h == g_gh) return false;
    g_gw = w; g_gh = h;
    return true;
}

// ---------------------------------------------------------------- 座標の変換
void Recalc() {
    if (!g_hwnd) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;
    g_scale = (std::min)((double)cw / g_gw, (double)ch / g_gh);
    g_offX = (cw - (int)lround(g_gw * g_scale)) / 2;
    g_offY = (ch - (int)lround(g_gh * g_scale)) / 2;
}

int ToGameX(int x) { int v = (int)floor((x - g_offX) / g_scale); return v < 0 ? 0 : (v >= g_gw ? g_gw - 1 : v); }
int ToGameY(int y) { int v = (int)floor((y - g_offY) / g_scale); return v < 0 ? 0 : (v >= g_gh ? g_gh - 1 : v); }
int ToWinX(int x) { return g_offX + (int)lround(x * g_scale); }
int ToWinY(int y) { return g_offY + (int)lround(y * g_scale); }

// 起動オプションにウィンドウモード (-w, -wd, -ww) があるか
bool HasWindowedSwitch() {
    const char* c = GetCommandLineA();
    for (const char* p = c; (p = strstr(p, "-w")) != nullptr; p += 2) {
        bool startOk = (p == c) || p[-1] == ' ' || p[-1] == '	' || p[-1] == '"';
        char n = p[2];
        bool endOk = n == 0 || n == ' ' || n == '	' || n == '"' ||
                     ((n == 'd' || n == 'w') && (p[3] == 0 || p[3] == ' ' || p[3] == '	' || p[3] == '"'));
        if (startOk && endOk) return true;
    }
    return false;
}

typedef HRESULT (WINAPI *GetDC_t)(void*, HDC*);
typedef HRESULT (WINAPI *ReleaseDC_t)(void*, HDC);

// 拡大した画像の外側 (余白) を黒で塗る
void PaintMargins(HDC dc) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int r = ToWinX(g_gw), b = ToWinY(g_gh);
    HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT m;
    if (g_offX > 0) { SetRect(&m, 0, 0, g_offX, rc.bottom); FillRect(dc, &m, k); }
    if (r < rc.right) { SetRect(&m, r, 0, rc.right, rc.bottom); FillRect(dc, &m, k); }
    if (g_offY > 0) { SetRect(&m, 0, 0, rc.right, g_offY); FillRect(dc, &m, k); }
    if (b < rc.bottom) { SetRect(&m, 0, b, rc.right, rc.bottom); FillRect(dc, &m, k); }
}

// 裏の画像の rect (ゲームの座標) を、窓の DC へ拡大して描く
bool StretchFrom(void* src, HDC wdc, const RECT& r) {
    if (!src) return false;
    void** svt = *(void***)src;
    HDC sdc = nullptr;
    if (((GetDC_t)svt[17])(src, &sdc) != 0 || !sdc) return false;
    RECT d = { ToWinX(r.left), ToWinY(r.top), ToWinX(r.right), ToWinY(r.bottom) };
    SetStretchBltMode(wdc, g_smooth ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(wdc, 0, 0, nullptr);
    StretchBlt(wdc, d.left, d.top, d.right - d.left, d.bottom - d.top,
               sdc, r.left, r.top, r.right - r.left, r.bottom - r.top, SRCCOPY);
    ((ReleaseDC_t)svt[26])(src, sdc);
    return true;
}

void FitWindow();

// ---------------------------------------------------------------- 窓のメッセージ
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_active && h == g_hwnd) {
        if ((msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK && msg != WM_MOUSEWHEEL) ||
            (msg >= WM_XBUTTONDOWN && msg <= WM_XBUTTONDBLCLK)) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            lp = MAKELPARAM(ToGameX(x), ToGameY(y));
        } else if (msg == WM_MOUSEWHEEL) {
            POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(h, &p);
            POINT o = { 0, 0 }; ClientToScreen(h, &o);
            lp = MAKELPARAM(o.x + ToGameX(p.x), o.y + ToGameY(p.y));
        } else if (msg == WM_GETMINMAXINFO) {
            // ゲームの処理 (窓を解像度+枠に固定する) のあとで、制限を外す
            LRESULT r = CallWindowProcA(g_origProc, h, msg, wp, lp);
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = 320; mm->ptMinTrackSize.y = 240;
            mm->ptMaxTrackSize.x = GetSystemMetrics(SM_CXVIRTUALSCREEN) + g_fw;
            mm->ptMaxTrackSize.y = GetSystemMetrics(SM_CYVIRTUALSCREEN) + g_fh;
            return r;
        } else if (msg == WM_SIZE) {
            Recalc();
        } else if (msg == WM_ERASEBKGND) {
            // 窓全体を塗ると、ゲームが一部だけ描き直すときに残りが黒くなる。余白だけ塗る
            PaintMargins((HDC)wp);
            return 1;
        } else if (msg == WM_PAINT) {
            // 再描画はゲームに任せる (ゲームは主サーフェスへ Blt し、h_Blt で拡大される)。
            // ゲームが Blt しなかったときだけ、裏の画像の全体を拡大して描く
            if (GameSize()) FitWindow();
            int before = g_bltCount;
            LRESULT r = CallWindowProcA(g_origProc, h, msg, wp, lp);
            if (g_bltCount == before) {
                void* src = g_backSurf ? g_backSurf : *(void**)0xfe7369c;
                HDC dc = GetDC(h);
                RECT all = { 0, 0, g_gw, g_gh };
                StretchFrom(src, dc, all);
                ReleaseDC(h, dc);
                ValidateRect(h, nullptr);
            }
            return r;
        }
    }
    return CallWindowProcA(g_origProc, h, msg, wp, lp);
}

// 窓の大きさを、今のゲームの解像度に合わせて画面に収まる最大にする
void FitWindow() {
    if (!g_hwnd || g_inFit) return;
    g_inFit = true;
    int aw = g_area.right - g_area.left, ah = g_area.bottom - g_area.top;
    int cw, ch;
    if (g_borderless) { cw = aw; ch = ah; }
    else {
        double s = g_scaleCfg > 0 ? g_scaleCfg
                                  : (std::min)((double)(aw - g_fw) / g_gw, (double)(ah - g_fh) / g_gh);
        if (s < 1.0) s = 1.0;
        cw = (int)floor(g_gw * s); ch = (int)floor(g_gh * s);
    }
    int nw = cw + g_fw, nh = ch + g_fh;
    int nx = g_area.left + (aw - nw) / 2, ny = g_area.top + (ah - nh) / 2;
    if (ny < g_area.top) ny = g_area.top;
    SetWindowPos(g_hwnd, nullptr, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    Recalc();
    RECT cr; GetClientRect(g_hwnd, &cr);
    LogLine("[display] fit game=%dx%d scale=%.3f client=%ldx%ld frame=%dx%d off=%d,%d borderless=%d",
            g_gw, g_gh, g_scale, cr.right, cr.bottom, g_fw, g_fh, g_offX, g_offY, g_borderless);
    g_inFit = false;
}

// ---------------------------------------------------------------- IAT フック
typedef ATOM (WINAPI *RegisterClassA_t)(const WNDCLASSA*);
typedef HWND (WINAPI *CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
RegisterClassA_t  o_RegisterClassA;
CreateWindowExA_t o_CreateWindowExA;

ATOM WINAPI h_RegisterClassA(const WNDCLASSA* wc) {
    if (!g_enable || !wc || g_origProc) return o_RegisterClassA(wc);
    WNDCLASSA c = *wc;
    g_origProc = c.lpfnWndProc;
    c.lpfnWndProc = WndProc;
    LogLine("[display] RegisterClassA %s", (HIWORD((DWORD)c.lpszClassName) ? c.lpszClassName : "(atom)"));
    return o_RegisterClassA(&c);
}

HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                              HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    LogLine("[display] CreateWindowExA ex=%lX style=%lX x=%d y=%d w=%d h=%d", ex, style, x, y, w, h);
    // 全画面用 (WS_POPUP) でなければウィンドウモード。
    // ゲームの窓は WS_CAPTION のビットを持たない (0x100F0000) が、Windows がタイトルバーをつける。
    bool windowed = (style & WS_POPUP) == 0 && HasWindowedSwitch();
    if (!g_enable || !windowed || g_hwnd || parent)
        return o_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);

    POINT pt = { x, y };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(mon, &mi);
    RECT area = g_borderless ? mi.rcMonitor : mi.rcWork;
    int aw = area.right - area.left, ah = area.bottom - area.top;

    DWORD nstyle = style, nex = ex;
    if (g_borderless) {
        nstyle = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
        nex = ex & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
    }
    // まず仮の大きさで作り、枠の太さを実測する
    HWND r = o_CreateWindowExA(nex, cls, title, nstyle, area.left, area.top, aw, ah, parent, menu, inst, param);
    if (!r) return r;
    RECT wr, cr;
    GetWindowRect(r, &wr); GetClientRect(r, &cr);
    int fw = (wr.right - wr.left) - cr.right, fh = (wr.bottom - wr.top) - cr.bottom;

    g_fw = fw; g_fh = fh; g_area = area;
    g_hwnd = r;
    g_active = true;
    GameSize();
    FitWindow();
    return r;
}

// ---------------------------------------------------------------- DirectDraw
typedef HRESULT (WINAPI *CreateSurface_t)(void*, void* desc, void** out, void*);
typedef HRESULT (WINAPI *Blt_t)(void*, RECT*, void*, RECT*, DWORD, void*);
typedef HRESULT (WINAPI *BltFast_t)(void*, DWORD, DWORD, void*, RECT*, DWORD);
CreateSurface_t o_CreateSurface;
Blt_t o_Blt;
BltFast_t o_BltFast;
bool g_surfPatched = false;

void PatchSlot(void** vt, int idx, void* hook, void** orig) {
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

HRESULT WINAPI h_Blt(void* self, RECT* dst, void* src, RECT* srcRect, DWORD flags, void* fx) {
    if (!g_active || self != g_primary || !src || !g_hwnd)
        return o_Blt(self, dst, src, srcRect, flags, fx);
    g_bltCount++;
    if (GameSize()) FitWindow();
    RECT r = srcRect ? *srcRect : RECT{ 0, 0, g_gw, g_gh };
    RECT d = { ToWinX(r.left), ToWinY(r.top), ToWinX(r.right), ToWinY(r.bottom) };
    static int logged = 0;
    if (logged < 5 || (logged < 50 && (r.right - r.left) != g_gw)) { logged++; LogLine("[display] Blt src=%ld,%ld-%ld,%ld flags=%lX", r.left, r.top, r.right, r.bottom, flags); }
    if (r.right > g_gw) r.right = g_gw;
    if (r.bottom > g_gh) r.bottom = g_gh;
    if (r.left == 0 && r.top == 0 && r.right == g_gw && r.bottom == g_gh) g_backSurf = src;
    {
        HDC wdc = GetDC(g_hwnd);
        bool ok = StretchFrom(src, wdc, r);
        ReleaseDC(g_hwnd, wdc);
        if (ok) return 0;
    }
    d = { ToWinX(r.left), ToWinY(r.top), ToWinX(r.right), ToWinY(r.bottom) };
    POINT o = { 0, 0 }; ClientToScreen(g_hwnd, &o);
    OffsetRect(&d, o.x, o.y);
    return o_Blt(self, &d, src, &r, flags, fx);
}

HRESULT WINAPI h_BltFast(void* self, DWORD x, DWORD y, void* src, RECT* srcRect, DWORD flags) {
    if (g_active && self == g_primary) {
        static int logged = 0;
        if (logged < 5) { logged++; LogLine("[display] BltFast to primary (not scaled) x=%lu y=%lu", x, y); }
    }
    return o_BltFast(self, x, y, src, srcRect, flags);
}

HRESULT WINAPI h_CreateSurface(void* dd, void* desc, void** out, void* outer) {
    HRESULT hr = o_CreateSurface(dd, desc, out, outer);
    if (hr == 0 && out && *out && desc) {
        DWORD caps = *(DWORD*)((BYTE*)desc + 0x68);
        if (caps & 0x200 /* DDSCAPS_PRIMARYSURFACE */) {
            g_primary = *out;
            LogLine("[display] primary surface %p caps=%lX", g_primary, caps);
        }
        if (!g_surfPatched) {
            g_surfPatched = true;
            void** svt = *(void***)*out;
            PatchSlot(svt, 5, (void*)h_Blt, (void**)&o_Blt);
            PatchSlot(svt, 7, (void*)h_BltFast, (void**)&o_BltFast);
        }
    }
    return hr;
}

} // namespace

void* DisplayPrimarySurface() { return g_primary; }

// DirectDrawCreateEx の直後に呼ぶ
void DisplayOnDirectDraw(void* dd) {
    if (!g_enable || !dd || o_CreateSurface) return;
    void** vt = *(void***)dd;
    PatchSlot(vt, 6, (void*)h_CreateSurface, (void**)&o_CreateSurface);
    LogLine("[display] IDirectDraw7::CreateSurface hooked");
}

// IAT の差し替え用 (ddraw_proxy.cpp の InstallHooks から使う)
void* DisplayHookRegisterClassA(void* orig) { o_RegisterClassA = (RegisterClassA_t)orig; return (void*)h_RegisterClassA; }
void* DisplayHookCreateWindowExA(void* orig) { o_CreateWindowExA = (CreateWindowExA_t)orig; return (void*)h_CreateWindowExA; }

void DisplayInit() {
    std::string ini = std::string(g_dir) + "witpae_ja.ini";
    char buf[32];
    GetPrivateProfileStringA("Display", "Scale", "auto", buf, sizeof buf, ini.c_str());
    if (_stricmp(buf, "auto") == 0) g_scaleCfg = 0;
    else { g_scaleCfg = atof(buf); if (g_scaleCfg == 0) g_enable = false; }
    g_borderless = GetPrivateProfileIntA("Display", "Borderless", 0, ini.c_str()) != 0;
    g_smooth = GetPrivateProfileIntA("Display", "Smooth", 1, ini.c_str()) != 0;
    g_defW = GetPrivateProfileIntA("Display", "GameWidth", 1024, ini.c_str());
    g_defH = GetPrivateProfileIntA("Display", "GameHeight", 768, ini.c_str());
    LogLine("[display] init enable=%d scale=%s borderless=%d smooth=%d", g_enable, buf, g_borderless, g_smooth);
}
