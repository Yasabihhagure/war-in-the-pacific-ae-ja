// DirectDraw の裏の画像を読み出し、自前で拡大して窓へ描く
//
// GDI の StretchBlt で DirectDraw のサーフェスから直接拡大すると、
// 1024x768 -> 2804x2103 で HALFTONE 約2.7秒、COLORONCOLOR でも約0.4秒かかった。
// ここでは Lock で画素を読み、CPU で拡大 (複数スレッド) してから、拡大なしで転送する。
//
// ゲームは画面の一部だけを転送することがある (ツールチップなど)。転送元は作業用の画像で、
// 転送した部分以外は描かれていないことがあるため、ゲーム画面の写し (キャンバス) を持ち、
// 転送された部分だけを書き込んで、その周りだけを拡大して描く。
#include "common.h"
#include <cstdint>
#include <thread>
#include <vector>

namespace {

typedef HRESULT (WINAPI *Lock_t)(void*, RECT*, void* desc, DWORD flags, HANDLE);
typedef HRESULT (WINAPI *Unlock_t)(void*, RECT*);

// DDSURFACEDESC2 (0x7c バイト) の必要な部分
struct Desc {
    DWORD dwSize, dwFlags, dwHeight, dwWidth;
    LONG  lPitch;
    BYTE  pad1[0x24 - 0x14];
    void* lpSurface;            // 0x24
    BYTE  pad2[0x48 - 0x28];
    DWORD pfSize, pfFlags, pfFourCC, bpp, rMask, gMask, bMask, aMask; // 0x48 ddpfPixelFormat
    BYTE  pad3[0x7c - 0x68];
};
static_assert(sizeof(Desc) == 0x7c, "DDSURFACEDESC2 size");

std::vector<uint32_t> g_canvas;   // ゲーム画面の写し (32bit, 0x00RRGGBB)
int g_cw = 0, g_ch = 0;
bool g_canvasValid = false;

HDC     g_memDC = nullptr;
HBITMAP g_dib = nullptr;
uint32_t* g_dst = nullptr;        // 拡大後の画像 (DIB セクション、上から下)
int g_dw = 0, g_dh = 0;

// 拡大の対応表 (出力の x/y ごとに、元の位置と重み 0..256)
std::vector<int> g_xi, g_xw, g_yi, g_yw;
int g_tblSw = 0, g_tblSh = 0, g_tblDw = 0, g_tblDh = 0;

int ShiftOf(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
int BitsOf(DWORD m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }

// 16bit (565 / 555) の画素を 0x00RRGGBB に変える表
const uint32_t* Lut16(const Desc& d) {
    static uint32_t lut[65536];
    static DWORD lr = 0, lg = 0, lb = 0;
    if (lr != d.rMask || lg != d.gMask || lb != d.bMask) {
        lr = d.rMask; lg = d.gMask; lb = d.bMask;
        int rs = ShiftOf(lr), gs = ShiftOf(lg), bs = ShiftOf(lb);
        int rb = BitsOf(lr), gb = BitsOf(lg), bb = BitsOf(lb);
        for (uint32_t v = 0; v < 65536; v++) {
            uint32_t r = (v & lr) >> rs, g = (v & lg) >> gs, b = (v & lb) >> bs;
            r = rb ? r * 255 / ((1u << rb) - 1) : 0;
            g = gb ? g * 255 / ((1u << gb) - 1) : 0;
            b = bb ? b * 255 / ((1u << bb) - 1) : 0;
            lut[v] = (r << 16) | (g << 8) | b;
        }
    }
    return lut;
}

// surf の sr (転送元の座標。空なら全体) を、キャンバスの (gx, gy) に書き込む
bool ReadRect(void* surf, RECT sr, int gx, int gy) {
    void** vt = *(void***)surf;
    Desc d = {};
    d.dwSize = sizeof d;
    // DDLOCK_WAIT(1) | DDLOCK_READONLY(0x10)
    if (((Lock_t)vt[25])(surf, nullptr, &d, 0x11, nullptr) != 0 || !d.lpSurface) return false;
    int sw = (int)d.dwWidth, sh = (int)d.dwHeight;
    if (sr.right <= sr.left || sr.bottom <= sr.top) sr = RECT{ 0, 0, sw, sh };
    // 転送元とキャンバスの両方の範囲に切り詰める
    if (sr.left < 0) { gx -= sr.left; sr.left = 0; }
    if (sr.top < 0) { gy -= sr.top; sr.top = 0; }
    if (sr.right > sw) sr.right = sw;
    if (sr.bottom > sh) sr.bottom = sh;
    if (gx < 0) { sr.left -= gx; gx = 0; }
    if (gy < 0) { sr.top -= gy; gy = 0; }
    int w = sr.right - sr.left, h = sr.bottom - sr.top;
    if (gx + w > g_cw) w = g_cw - gx;
    if (gy + h > g_ch) h = g_ch - gy;
    bool ok = true;
    if (w > 0 && h > 0) {
        const BYTE* base = (const BYTE*)d.lpSurface;
        const uint32_t* lut = d.bpp == 16 ? Lut16(d) : nullptr;
        for (int y = 0; y < h && ok; y++) {
            const BYTE* row = base + (size_t)(sr.top + y) * d.lPitch;
            uint32_t* o = &g_canvas[(size_t)(gy + y) * g_cw + gx];
            if (d.bpp == 32) {
                const uint32_t* s = (const uint32_t*)row + sr.left;
                for (int x = 0; x < w; x++) o[x] = s[x] & 0xffffff;
            } else if (d.bpp == 16) {
                const uint16_t* s = (const uint16_t*)row + sr.left;
                for (int x = 0; x < w; x++) o[x] = lut[s[x]];
            } else if (d.bpp == 24) {
                const BYTE* s = row + sr.left * 3;
                for (int x = 0; x < w; x++) o[x] = s[x * 3] | (s[x * 3 + 1] << 8) | (s[x * 3 + 2] << 16);
            } else ok = false;
        }
    }
    ((Unlock_t)vt[32])(surf, nullptr);
    if (!ok) {
        static bool warned = false;
        if (!warned) { warned = true; LogLine("[scaler] unsupported bpp=%lu", d.bpp); }
    }
    return ok;
}

bool EnsureDib(HDC ref, int dw, int dh) {
    if (g_dib && g_dw == dw && g_dh == dh) return true;
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    if (!g_memDC) g_memDC = CreateCompatibleDC(ref);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = -dh; // 上から下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_dib = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_dib) return false;
    SelectObject(g_memDC, g_dib);
    g_dst = (uint32_t*)bits;
    g_dw = dw; g_dh = dh;
    return true;
}

void BuildTables(int sw, int sh, int dw, int dh) {
    if (g_tblSw == sw && g_tblSh == sh && g_tblDw == dw && g_tblDh == dh) return;
    auto build = [](int s, int d, std::vector<int>& idx, std::vector<int>& wt) {
        idx.resize(d); wt.resize(d);
        for (int i = 0; i < d; i++) {
            double f = (i + 0.5) * s / d - 0.5;
            if (f < 0) f = 0;
            int k = (int)f;
            if (k >= s - 1) { k = s - 1; f = k; }
            idx[i] = k;
            wt[i] = (int)((f - k) * 256 + 0.5);
        }
    };
    build(sw, dw, g_xi, g_xw);
    build(sh, dh, g_yi, g_yw);
    g_tblSw = sw; g_tblSh = sh; g_tblDw = dw; g_tblDh = dh;
}

inline uint32_t Lerp(uint32_t a, uint32_t b, uint32_t w) {
    uint32_t iw = 256 - w;
    uint32_t rb = (((a & 0xff00ff) * iw + (b & 0xff00ff) * w) >> 8) & 0xff00ff;
    uint32_t g  = (((a & 0x00ff00) * iw + (b & 0x00ff00) * w) >> 8) & 0x00ff00;
    return rb | g;
}

// 出力の行 y0..y1、列 x0..x1 を計算する
void ScaleRect(int y0, int y1, int x0, int x1, bool smooth) {
    const int sw = g_cw, sh = g_ch, dw = g_dw;
    const int* xi = g_xi.data(); const int* xw = g_xw.data();
    for (int y = y0; y < y1; y++) {
        uint32_t* o = g_dst + (size_t)y * dw;
        int sy = g_yi[y];
        const uint32_t* r0 = &g_canvas[(size_t)sy * sw];
        if (!smooth) {
            const uint32_t* r = (g_yw[y] >= 128 && sy + 1 < sh) ? r0 + sw : r0;
            for (int x = x0; x < x1; x++) { int k = xi[x] + (xw[x] >= 128 && xi[x] + 1 < sw ? 1 : 0); o[x] = r[k]; }
            continue;
        }
        const uint32_t* r1 = (sy + 1 < sh) ? r0 + sw : r0;
        uint32_t wy = (uint32_t)g_yw[y];
        for (int x = x0; x < x1; x++) {
            int k = xi[x], k1 = (k + 1 < sw) ? k + 1 : k;
            uint32_t wx = (uint32_t)xw[x];
            o[x] = Lerp(Lerp(r0[k], r0[k1], wx), Lerp(r1[k], r1[k1], wx), wy);
        }
    }
}

} // namespace

// キャンバス (ゲーム画面の写し) の大きさを合わせる
void CanvasResize(int w, int h) {
    if (w == g_cw && h == g_ch) return;
    g_cw = w; g_ch = h;
    g_canvas.assign((size_t)w * h, 0);
    g_canvasValid = false;
}

bool CanvasValid() { return g_canvasValid; }

// ゲームの転送 (surf の sr → ゲーム画面の (gx, gy)) をキャンバスに反映する
bool CanvasUpdate(void* surf, RECT sr, int gx, int gy) {
    if (!surf || g_cw <= 0) return false;
    bool whole = sr.right <= sr.left;
    if (!ReadRect(surf, sr, gx, gy)) return false;
    if (whole || (gx <= 0 && gy <= 0 && sr.right - sr.left >= g_cw && sr.bottom - sr.top >= g_ch)) g_canvasValid = true;
    return true;
}

// キャンバスのゲーム座標 (gx0,gy0)-(gx1,gy1) の周りを拡大し、窓の DC の (dx,dy) を基準に描く
bool CanvasPresent(HDC wdc, int gx0, int gy0, int gx1, int gy1, int dx, int dy, int dw, int dh, bool smooth) {
    if (g_cw <= 0 || dw <= 0 || dh <= 0) return false;
    if (!EnsureDib(wdc, dw, dh)) return false;
    BuildTables(g_cw, g_ch, dw, dh);
    // 双線形補間は隣の画素も使うので、1画素広げる
    gx0 -= 1; gy0 -= 1; gx1 += 1; gy1 += 1;
    int xd0 = (int)((long long)gx0 * dw / g_cw), xd1 = (int)(((long long)gx1 * dw + g_cw - 1) / g_cw);
    int yd0 = (int)((long long)gy0 * dh / g_ch), yd1 = (int)(((long long)gy1 * dh + g_ch - 1) / g_ch);
    if (xd0 < 0) xd0 = 0;
    if (yd0 < 0) yd0 = 0;
    if (xd1 > dw) xd1 = dw;
    if (yd1 > dh) yd1 = dh;
    if (xd0 >= xd1 || yd0 >= yd1) return true;
    int rows = yd1 - yd0;
    static int nThreads = 0;
    if (!nThreads) { nThreads = (int)std::thread::hardware_concurrency(); if (nThreads < 1) nThreads = 1; if (nThreads > 8) nThreads = 8; }
    long long px = (long long)rows * (xd1 - xd0);
    if (nThreads == 1 || px < 200000) ScaleRect(yd0, yd1, xd0, xd1, smooth);
    else {
        std::vector<std::thread> th;
        int step = (rows + nThreads - 1) / nThreads;
        for (int i = 1; i < nThreads; i++) {
            int a = yd0 + i * step, b = a + step;
            if (b > yd1) b = yd1;
            if (a < b) th.emplace_back(ScaleRect, a, b, xd0, xd1, smooth);
        }
        ScaleRect(yd0, (yd0 + step < yd1 ? yd0 + step : yd1), xd0, xd1, smooth);
        for (auto& t : th) t.join();
    }
    BitBlt(wdc, dx + xd0, dy + yd0, xd1 - xd0, yd1 - yd0, g_memDC, xd0, yd0, SRCCOPY);
    return true;
}
