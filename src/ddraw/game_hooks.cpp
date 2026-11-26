// exe 内部の関数をメモリ上で差し替える (exe のファイルは書き換えない)
//
// このゲームは、起動時に英数字127文字の画像を作り、自前の処理で貼り付けて文字を描く。
// 0x80 以上のバイトは読み飛ばされるため、日本語 (Shift_JIS) の部分は
// ここで GDI の日本語フォントを使って描き、幅の計算にも反映する。
//
// 対象: War in the Pacific Admiral Edition.exe (2016-07-10, 5,054,464 バイト)
// アドレスの根拠は PLAN.md の 3.1 を参照。
#include "common.h"
#include <cstring>
#include <map>

namespace {

// ---------------------------------------------------------------- exe のアドレス
const DWORD A_DRAWLINE = 0x7fa920; // void (int x, int y, const char* s, IDirectDrawSurface7* target, int color)
const DWORD A_WIDTH    = 0x7f43e0; // int  (const char* s, Font* font)
const DWORD A_VFMT     = 0x7f2b70; // int  (char* buf, const char* fmt, va_list va)
const DWORD A_VSNPRINT = 0x7f13c0; // int  (int size, char* buf, const char* fmt, va_list va)

void** const P_CURFONT = (void**)0xfe73458;  // 現在のフォント
void** const P_SCREEN  = (void**)0xfe7369c;  // 既定の描画先サーフェス
int*   const P_X       = (int*)0xfdf9da8;    // 描画位置 x
int*   const P_Y       = (int*)0xfdf9dc4;    // 描画位置 y
int*   const P_READY   = (int*)0x10398444;   // 描画の準備ができていれば 0 以外

// 太字のフォント (0x401c80 付近で作られる)
const DWORD FONT_BOLD1 = 0x7adeb58;
const DWORD FONT_BOLD2 = 0x4270ae8;

// フォントの管理用データ
inline int   FontHeight(void* f) { return ((int*)f)[2]; }         // +8
inline DWORD FontColor(void* f)  { return ((DWORD*)f)[4]; }       // +0x10 (0x00RRGGBB)
inline bool  FontBold(void* f)   { return (DWORD)f == FONT_BOLD1 || (DWORD)f == FONT_BOLD2; }

// ---------------------------------------------------------------- フックの道具
bool CheckBytes(DWORD addr, const char* hex) {
    const BYTE* p = (const BYTE*)addr;
    for (int i = 0; hex[i * 2] && hex[i * 2 + 1]; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        if (p[i] != (BYTE)strtoul(b, nullptr, 16)) return false;
    }
    return true;
}

// addr の先頭 n バイト (命令の切れ目であること) を写し、続きへ jmp する入口を作る
void* MakeTrampoline(DWORD addr, int n) {
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, (void*)addr, n);
    t[n] = 0xE9;
    *(DWORD*)(t + n + 1) = (addr + n) - (DWORD)(t + n + 5);
    return t;
}

void WriteJmp(DWORD addr, void* to) {
    DWORD old;
    VirtualProtect((void*)addr, 5, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE*)addr = 0xE9;
    *(DWORD*)(addr + 1) = (DWORD)to - (addr + 5);
    VirtualProtect((void*)addr, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, 5);
}

// ---------------------------------------------------------------- 日本語の描画
std::map<int, HFONT> g_jpFonts; // (高さ*2 + 太字) -> フォント
HDC g_measureDC;
int g_jpHeightAdjust = 0; // ini [JpFont] HeightAdjust: 日本語の高さの補正 (px)
int g_jpYOffset = 0;      // ini [JpFont] YOffset: 日本語の上下位置の補正 (px、正で下へ)

HFONT JpFont(void* font) {
    int h = FontHeight(font);
    if (h <= 0 || h > 100) h = 14;
    h += g_jpHeightAdjust;
    if (h < 6) h = 6;
    bool bold = FontBold(font);
    int key = h * 2 + (bold ? 1 : 0);
    auto it = g_jpFonts.find(key);
    if (it != g_jpFonts.end()) return it->second;
    wchar_t face[LF_FACESIZE];
    MultiByteToWideChar(CP_ACP, 0, g_fontFace, -1, face, LF_FACESIZE);
    HFONT f = CreateFontW(h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                          FIXED_PITCH | FF_MODERN, face);
    LogLine("[jp] font h=%d bold=%d face=%s -> %p", h, bold, g_fontFace, f);
    g_jpFonts[key] = f;
    return f;
}

std::wstring ToWide(const char* s, int n) {
    int wn = MultiByteToWideChar(932, 0, s, n, nullptr, 0);
    std::wstring w(wn, 0);
    MultiByteToWideChar(932, 0, s, n, &w[0], wn);
    return w;
}

int MeasureSjis(void* font, const char* s, int n) {
    if (!font) return 0;
    if (!g_measureDC) g_measureDC = CreateCompatibleDC(nullptr);
    std::wstring w = ToWide(s, n);
    HGDIOBJ old = SelectObject(g_measureDC, JpFont(font));
    SIZE sz = {};
    GetTextExtentPoint32W(g_measureDC, w.c_str(), (int)w.size(), &sz);
    SelectObject(g_measureDC, old);
    return sz.cx;
}

typedef HRESULT (__stdcall *SurfGetDC_t)(void*, HDC*);
typedef HRESULT (__stdcall *SurfReleaseDC_t)(void*, HDC);

// 描画先サーフェスの DC に日本語を描き、幅を返す
int DrawSjis(int x, int y, const char* s, int n, void* target) {
    void* font = *P_CURFONT;
    if (!font) return 0;
    void* surf = target ? target : *P_SCREEN;
    int width = MeasureSjis(font, s, n);
    if (!surf) return width;
    void** vt = *(void***)surf;
    HDC dc = nullptr;
    if (((SurfGetDC_t)vt[0x44 / 4])(surf, &dc) != 0 || !dc) return width;
    std::wstring w = ToWide(s, n);
    DWORD c = FontColor(font);
    HGDIOBJ old = SelectObject(dc, JpFont(font));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff));
    TextOutW(dc, x, y + g_jpYOffset, w.c_str(), (int)w.size());
    SelectObject(dc, old);
    ((SurfReleaseDC_t)vt[0x68 / 4])(surf, dc);
    static int logged = 0;
    if (logged < 20) { logged++; LogLine("[jp] draw x=%d y=%d color=%06lX h=%d n=%d", x, y, c, FontHeight(font), n); }
    return width;
}

bool HasHigh(const char* s, int n) {
    for (int i = 0; i < n; i++) if ((BYTE)s[i] >= 0x80) return true;
    return false;
}

// s から始まる、0x80 以上のバイトが続く部分の長さ (2バイト文字は分けない)
int HighRunLen(const char* s) {
    int i = 0;
    while (s[i] && (BYTE)s[i] >= 0x80) {
        if (IsDBCSLeadByteEx(932, (BYTE)s[i]) && s[i + 1]) i += 2; else i += 1;
    }
    return i;
}

int LowRunLen(const char* s) {
    int i = 0;
    while (s[i] && (BYTE)s[i] < 0x80) i++;
    return i;
}

// ---------------------------------------------------------------- フック本体
typedef void (__cdecl *DrawLine_t)(int, int, const char*, void*, int);
typedef int  (__cdecl *Width_t)(const char*, void*);
typedef int  (__cdecl *VSnprint_t)(int, char*, const char*, va_list);

DrawLine_t o_DrawLine;
Width_t    o_Width;

void __cdecl h_DrawLine(int x, int y, const char* s, void* target, int color) {
    if (!s || !*P_READY || !*P_CURFONT) { o_DrawLine(x, y, s, target, color); return; }
    int n = (int)strlen(s);
    Seen("L", s, n);
    if (const std::string* t = Lookup(s, n)) { s = t->c_str(); n = (int)t->size(); }
    if (!HasHigh(s, n)) { o_DrawLine(x, y, s, target, color); return; }

    int cx = x, cy = y;
    std::string run;
    const char* p = s;
    while (*p) {
        int k = LowRunLen(p);
        if (k > 0) {
            run.assign(p, k);
            o_DrawLine(cx, cy, run.c_str(), target, color);
            cx = *P_X; cy = *P_Y;
            p += k;
            continue;
        }
        k = HighRunLen(p);
        cx += DrawSjis(cx, cy, p, k, target);
        *P_X = cx; *P_Y = cy;
        p += k;
    }
}

int __cdecl h_Width(const char* s, void* font) {
    if (!s) return o_Width(s, font);
    int n = (int)strlen(s);
    if (const std::string* t = Lookup(s, n)) { s = t->c_str(); n = (int)t->size(); }
    if (!HasHigh(s, n)) return o_Width(s, font);
    void* f = font ? font : *P_CURFONT;
    int w = 0;
    std::string run;
    const char* p = s;
    while (*p) {
        int k = LowRunLen(p);
        if (k > 0) { run.assign(p, k); w += o_Width(run.c_str(), font); p += k; continue; }
        k = HighRunLen(p);
        w += MeasureSjis(f, p, k);
        p += k;
    }
    return w;
}

// 書式を訳文の書式に差し替えてから組み立てる
int __cdecl h_Vfmt(char* buf, const char* fmt, va_list va) {
    if (fmt) {
        int n = (int)strlen(fmt);
        Seen("F", fmt, n);
        if (const std::string* t = Lookup(fmt, n)) fmt = t->c_str();
    }
    return ((VSnprint_t)A_VSNPRINT)(0x200, buf, fmt, va);
}

} // namespace

void InstallGameHooks() {
    std::string ini = std::string(g_dir) + "witpae_ja.ini";
    g_jpHeightAdjust = GetPrivateProfileIntA("JpFont", "HeightAdjust", 0, ini.c_str());
    g_jpYOffset = GetPrivateProfileIntA("JpFont", "YOffset", 0, ini.c_str());
    struct Item { const char* name; DWORD addr; const char* bytes; int copy; void* hook; void** orig; };
    Item items[] = {
        // name       addr        先頭の機械語 (版の確認)         写す長さ  フック            元の関数への入口
        {"DrawLine", A_DRAWLINE, "83ec18833d4484391000",            10, (void*)h_DrawLine, (void**)&o_DrawLine},
        {"Width",    A_WIDTH,    "558b6c2408",                      5,  (void*)h_Width,    (void**)&o_Width},
        {"Vfmt",     A_VFMT,     "8b44240c8b4c24088b542404",        0,  (void*)h_Vfmt,     nullptr},
    };
    for (auto& it : items) {
        if (!CheckBytes(it.addr, it.bytes)) { LogLine("[game] %s: bytes mismatch, skipped (different exe version?)", it.name); continue; }
        if (it.orig) {
            *it.orig = MakeTrampoline(it.addr, it.copy);
            if (!*it.orig) { LogLine("[game] %s: trampoline failed", it.name); continue; }
        }
        WriteJmp(it.addr, it.hook);
        LogLine("[game] %s hooked at %06lX", it.name, it.addr);
    }
}
