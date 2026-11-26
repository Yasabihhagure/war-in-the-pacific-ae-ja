// DirectDraw の裏の画像を読み出し、自前で拡大して窓へ描く
//
// GDI の StretchBlt で DirectDraw のサーフェスから直接拡大すると、
// 1024x768 -> 2804x2103 で HALFTONE 約2.7秒、COLORONCOLOR でも約0.4秒かかった。
// ここでは Lock で画素を読み、CPU で拡大 (複数スレッド) してから、拡大なしで転送する。
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

std::vector<uint32_t> g_src;      // 32bit に変換した元画像
int g_sw = 0, g_sh = 0;

HDC     g_memDC = nullptr;
HBITMAP g_dib = nullptr;
uint32_t* g_dst = nullptr;        // 拡大後の画像 (DIB セクション、上から下)
int g_dw = 0, g_dh = 0;

// 拡大の対応表 (出力の x/y ごとに、元の位置と重み 0..256)
std::vector<int> g_xi, g_xw, g_yi, g_yw;
int g_tblSw = 0, g_tblSh = 0, g_tblDw = 0, g_tblDh = 0;

int ShiftOf(DWORD m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
int BitsOf(DWORD m) { int b = 0; while (m) { b += m & 1; m >>= 1; } return b; }

bool ReadSurface(void* surf) {
    void** vt = *(void***)surf;
    Desc d = {};
    d.dwSize = sizeof d;
    // DDLOCK_WAIT(1) | DDLOCK_READONLY(0x10)
    if (((Lock_t)vt[25])(surf, nullptr, &d, 0x11, nullptr) != 0 || !d.lpSurface) return false;
    int w = (int)d.dwWidth, h = (int)d.dwHeight;
    if ((int)g_src.size() != w * h) g_src.resize((size_t)w * h);
    g_sw = w; g_sh = h;
    const BYTE* base = (const BYTE*)d.lpSurface;
    if (d.bpp == 32) {
        for (int y = 0; y < h; y++) {
            const uint32_t* s = (const uint32_t*)(base + (size_t)y * d.lPitch);
            uint32_t* o = &g_src[(size_t)y * w];
            for (int x = 0; x < w; x++) o[x] = s[x] & 0xffffff;
        }
    } else if (d.bpp == 16) {
        // 565 / 555 をマスクから判定して 8bit に広げる (対応表で高速化)
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
        for (int y = 0; y < h; y++) {
            const uint16_t* s = (const uint16_t*)(base + (size_t)y * d.lPitch);
            uint32_t* o = &g_src[(size_t)y * w];
            for (int x = 0; x < w; x++) o[x] = lut[s[x]];
        }
    } else if (d.bpp == 24) {
        for (int y = 0; y < h; y++) {
            const BYTE* s = base + (size_t)y * d.lPitch;
            uint32_t* o = &g_src[(size_t)y * w];
            for (int x = 0; x < w; x++) o[x] = s[x * 3] | (s[x * 3 + 1] << 8) | (s[x * 3 + 2] << 16);
        }
    } else {
        ((Unlock_t)vt[32])(surf, nullptr);
        static bool warned = false;
        if (!warned) { warned = true; LogLine("[scaler] unsupported bpp=%lu", d.bpp); }
        return false;
    }
    ((Unlock_t)vt[32])(surf, nullptr);
    return true;
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

void ScaleRows(int y0, int y1, bool smooth) {
    const int sw = g_sw, dw = g_dw;
    const int* xi = g_xi.data(); const int* xw = g_xw.data();
    for (int y = y0; y < y1; y++) {
        uint32_t* o = g_dst + (size_t)y * dw;
        int sy = g_yi[y];
        const uint32_t* r0 = &g_src[(size_t)sy * sw];
        if (!smooth) {
            // 最近傍: 重みが半分以上なら次の画素
            const uint32_t* r = (g_yw[y] >= 128 && sy + 1 < g_sh) ? r0 + sw : r0;
            for (int x = 0; x < dw; x++) { int k = xi[x] + (xw[x] >= 128 && xi[x] + 1 < sw ? 1 : 0); o[x] = r[k]; }
            continue;
        }
        const uint32_t* r1 = (sy + 1 < g_sh) ? r0 + sw : r0;
        uint32_t wy = (uint32_t)g_yw[y];
        for (int x = 0; x < dw; x++) {
            int k = xi[x], k1 = (k + 1 < sw) ? k + 1 : k;
            uint32_t wx = (uint32_t)xw[x];
            uint32_t top = Lerp(r0[k], r0[k1], wx);
            uint32_t bot = Lerp(r1[k], r1[k1], wx);
            o[x] = Lerp(top, bot, wy);
        }
    }
}

} // namespace

// 裏の画像 surf を読み、窓の DC の (dx,dy) に dw x dh で描く。失敗したら false
bool ScalePresent(void* surf, HDC wdc, int dx, int dy, int dw, int dh, bool smooth) {
    if (!surf || dw <= 0 || dh <= 0) return false;
    if (!ReadSurface(surf)) return false;
    if (!EnsureDib(wdc, dw, dh)) return false;
    BuildTables(g_sw, g_sh, dw, dh);
    static int nThreads = 0;
    if (!nThreads) { nThreads = (int)std::thread::hardware_concurrency(); if (nThreads < 1) nThreads = 1; if (nThreads > 8) nThreads = 8; }
    if (nThreads == 1) ScaleRows(0, dh, smooth);
    else {
        std::vector<std::thread> th;
        int step = (dh + nThreads - 1) / nThreads;
        for (int i = 1; i < nThreads; i++) {
            int a = i * step, b = (i + 1) * step; if (b > dh) b = dh;
            if (a < b) th.emplace_back(ScaleRows, a, b, smooth);
        }
        ScaleRows(0, step < dh ? step : dh, smooth);
        for (auto& t : th) t.join();
    }
    BitBlt(wdc, dx, dy, dw, dh, g_memDC, 0, 0, SRCCOPY);
    return true;
}
