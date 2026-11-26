// ウィンドウモード (-w) の画面を拡大して表示する
//
// - 窓を作るとき (CreateWindowExA) に、画面に収まる最大の倍率の大きさにする
// - ゲームが裏の画像を窓へ転送するとき (主サーフェスへの Blt) に、拡大して転送する
// - 窓に届くマウスの座標を倍率で割って、ゲームに渡す
//
// ゲームの描画は 1024x768 のまま。ゲームから見ると、拡大されていない窓に見える。
#include "common.h"
#include <cmath>

namespace {

// 設定 (witpae_ja.ini の [Display])
bool  g_enable = true;     // Scale: 0 で無効
double g_scaleCfg = 0;     // Scale: 0=自動 (画面に収まる最大), それ以外は倍率
bool  g_borderless = false;
bool  g_smooth = true;
int   g_gw = 1024, g_gh = 768; // ゲームの画面の大きさ

// 状態
bool  g_active = false;    // 拡大中
HWND  g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;
void* g_primary = nullptr;
double g_scale = 1.0;
int   g_offX = 0, g_offY = 0;  // 窓の中の、拡大した画像の左上 (左右・上下の余白)

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
        } else if (msg == WM_SIZE) {
            Recalc();
        } else if (msg == WM_ERASEBKGND) {
            RECT rc; GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
    }
    return CallWindowProcA(g_origProc, h, msg, wp, lp);
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
    bool windowed = (style & WS_CAPTION) == WS_CAPTION;
    if (!g_enable || !windowed || g_hwnd || parent) {
        HWND r = o_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
        return r;
    }
    // ゲームの画面の大きさ = 指定された窓の大きさから枠を引いたもの
    RECT adj = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&adj, style, FALSE, ex);
    int gw = w - (adj.right - adj.left), gh = h - (adj.bottom - adj.top);
    if (gw >= 320 && gh >= 240) { g_gw = gw; g_gh = gh; }

    POINT pt = { x, y };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(mon, &mi);
    RECT area = g_borderless ? mi.rcMonitor : mi.rcWork;
    int aw = area.right - area.left, ah = area.bottom - area.top;

    DWORD nstyle = style, nex = ex;
    int fw = 0, fh = 0;
    if (g_borderless) {
        nstyle = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
        nex = ex & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
    } else {
        RECT a2 = { 0, 0, 0, 0 };
        AdjustWindowRectEx(&a2, nstyle, FALSE, nex);
        fw = a2.right - a2.left; fh = a2.bottom - a2.top;
    }
    double s = g_scaleCfg > 0 ? g_scaleCfg
                              : (std::min)((double)(aw - fw) / g_gw, (double)(ah - fh) / g_gh);
    if (s < 1.0) s = 1.0;
    int cw, ch;
    if (g_borderless) { cw = aw; ch = ah; }
    else { cw = (int)lround(g_gw * s); ch = (int)lround(g_gh * s); }
    int nw = cw + fw, nh = ch + fh;
    int nx = area.left + (aw - nw) / 2, ny = area.top + (ah - nh) / 2;
    if (ny < area.top) ny = area.top;
    HWND r = o_CreateWindowExA(nex, cls, title, nstyle, nx, ny, nw, nh, parent, menu, inst, param);
    if (r) {
        g_hwnd = r;
        g_active = true;
        Recalc();
        LogLine("[display] scaled window game=%dx%d scale=%.3f client=%dx%d off=%d,%d borderless=%d",
                g_gw, g_gh, g_scale, cw, ch, g_offX, g_offY, g_borderless);
    }
    return r;
}

// ---------------------------------------------------------------- DirectDraw
typedef HRESULT (WINAPI *CreateSurface_t)(void*, void* desc, void** out, void*);
typedef HRESULT (WINAPI *Blt_t)(void*, RECT*, void*, RECT*, DWORD, void*);
typedef HRESULT (WINAPI *BltFast_t)(void*, DWORD, DWORD, void*, RECT*, DWORD);
typedef HRESULT (WINAPI *GetDC_t)(void*, HDC*);
typedef HRESULT (WINAPI *ReleaseDC_t)(void*, HDC);
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
    RECT r = srcRect ? *srcRect : RECT{ 0, 0, g_gw, g_gh };
    RECT d = { ToWinX(r.left), ToWinY(r.top), ToWinX(r.right), ToWinY(r.bottom) };
    static int logged = 0;
    if (logged < 5) { logged++; LogLine("[display] Blt src=%ld,%ld-%ld,%ld flags=%lX", r.left, r.top, r.right, r.bottom, flags); }
    if (g_smooth) {
        void** svt = *(void***)src;
        HDC sdc = nullptr;
        if (((GetDC_t)svt[17])(src, &sdc) == 0 && sdc) {
            HDC wdc = GetDC(g_hwnd);
            SetStretchBltMode(wdc, HALFTONE);
            SetBrushOrgEx(wdc, 0, 0, nullptr);
            StretchBlt(wdc, d.left, d.top, d.right - d.left, d.bottom - d.top,
                       sdc, r.left, r.top, r.right - r.left, r.bottom - r.top, SRCCOPY);
            ReleaseDC(g_hwnd, wdc);
            ((ReleaseDC_t)svt[26])(src, sdc);
            return 0;
        }
    }
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
    LogLine("[display] init enable=%d scale=%s borderless=%d smooth=%d", g_enable, buf, g_borderless, g_smooth);
}
