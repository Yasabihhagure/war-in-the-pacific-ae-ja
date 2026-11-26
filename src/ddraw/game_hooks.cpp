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
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace {

// ---------------------------------------------------------------- exe のアドレス
const DWORD A_DRAWLINE = 0x7fa920; // void (int x, int y, const char* s, IDirectDrawSurface7* target, RECT* clip)
const RECT* const P_SCREENCLIP = (const RECT*)0xfe732ac; // clip が空で描く先が画面のときの切り抜き範囲 (0x7fa330)
const DWORD A_WIDTH    = 0x7f43e0; // int  (const char* s, Font* font)
const DWORD A_VSNPRINT = 0x7f13c0; // int  (int size, char* buf, const char* fmt, va_list va)
const DWORD A_SPRINTF  = 0x80f1ab; // int  (char* buf, const char* fmt, ...)  C 標準ライブラリ
const DWORD A_VSPRINTF = 0x8111b0; // int  (char* buf, const char* fmt, va_list va)  C 標準ライブラリ

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
// +0x10 文字の色 (COLORREF と同じ 0x00BBGGRR。0x7f8c80 が白で文字画像を作り、0x7f6cd0 がこの色に塗り替える)
// +0x14〜+0x1c は文字画像の塗り替えに使うほかの色 (0x7f3e60(font, 番号, 色) で設定)
inline DWORD FontColor(void* f)  { return ((DWORD*)f)[4]; }
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
// clip: 切り抜く範囲 (0x7fa330 と同じ決まり。空で描く先が画面なら P_SCREENCLIP、それ以外の空はサーフェス全体)
int DrawSjis(int x, int y, const char* s, int n, void* target, const RECT* clip) {
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
    if (!clip && surf == *P_SCREEN) clip = P_SCREENCLIP;
    if (clip && clip->right > clip->left && clip->bottom > clip->top)
        IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c & 0xFFFFFF);
    // 小さくした分の半分だけ下げて、英字の行の高さの中に収める
    int shrink = g_jpHeightAdjust < 0 ? -g_jpHeightAdjust / 2 : 0;
    TextOutW(dc, x, y + g_jpYOffset + shrink, w.c_str(), (int)w.size());
    SelectObject(dc, old);
    ((SurfReleaseDC_t)vt[0x68 / 4])(surf, dc);
    static int logged = 0;
    if (logged < 20) { logged++; LogLine("[jp] draw x=%d y=%d color=%06lX h=%d n=%d", x, y, c, FontHeight(font), n); }
    return width;
}

// UTF-8 の文字列を Shift_JIS (CP932) に直す (ソース中の日本語の定数用)
std::string Sjis(const char* utf8) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(wn, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wn);
    int an = WideCharToMultiByte(932, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string a(an, 0);
    WideCharToMultiByte(932, 0, w.c_str(), -1, &a[0], an, nullptr, nullptr);
    if (!a.empty() && a.back() == 0) a.pop_back();
    return a;
}

// 全角文字だけを並べた list (Shift_JIS) に、p の2バイト文字があるか
bool HasSjisChar(const std::string& list, const char* p) {
    for (size_t i = 0; i + 1 < list.size(); i += 2)
        if (p[0] == list[i] && p[1] == list[i + 1]) return true;
    return false;
}

// ---------------------------------------------------------------- 名前の種類の見分け
// シナリオのデータはファイルのままメモリに読み込まれる (0x54b790(path, 読み込み先)、0x54b8c0 付近)。
// 文字列がその範囲にあれば、どの種類の名前かが分かるので、その種類の対訳表を先に引く
// (同じ英語名でも、艦は「川内」、地名は「仙台」のように訳し分ける)。
struct DataRange { DWORD begin, size; const char* kinds[2]; };
// ゲーム中の配列 (0x54b7c0 付近で初期化。範囲はログの [addr] で確かめた):
//   艦船 0x1dad540 (0x8d9a00)、艦型 0x3aee328 (0x1cfde0)
// 艦船の名前の一部は艦型の表にだけ訳がある (艦型と同じ名前は艦船の表で訳していない) ので、両方を引く
const DataRange kRanges[] = {
    { 0x1dad540, 0x8d9a00, { "ships", "ship_classes" } },         // 艦船 (ゲーム中)
    { 0x3aee328, 0x1cfde0, { "ship_classes", "ships" } },         // 艦型 (ゲーム中)
    { 0x3cbe1a8, 0x583220, { "ships", nullptr } },                 // wps
    { 0x756c4a8, 0x152228, { "ship_classes", nullptr } },          // wpc
    { 0x3159a88, 0x3b883c, { "places", "ground_units" } },         // wpl
};

// 調査用: 名前の対訳表にある文字列が、メモリのどこから渡されたかをログに残す
// (64KB の区画と、その名前がある表の組み合わせごとに1回。ini [Debug] LogNameAddr=1)
bool g_logNameAddr = false;
void LogNameAddress(const char* s, int n) {
    if (!g_logNameAddr) return;
    static const char* const kinds[] = { "ships", "ship_classes", "places", "ground_units", "air_groups", "aircraft" };
    int mask = 0;
    for (int i = 0; i < 6; i++) if (LookupKind(kinds[i], s, n)) mask |= 1 << i;
    if (!mask) return;
    static std::unordered_map<unsigned long long, int> seen;
    static int count = 0;
    unsigned long long key = ((unsigned long long)((DWORD)s >> 16) << 8) | mask;
    if (count >= 400 || seen.count(key)) return;
    seen[key] = 1; count++;
    std::string ks;
    for (int i = 0; i < 6; i++) if (mask & (1 << i)) { if (!ks.empty()) ks += ','; ks += kinds[i]; }
    LogLine("[addr] %p %s: %.*s", s, ks.c_str(), n, s);
}

const std::string* LookupAt(const char* s, int n) {
    DWORD a = (DWORD)s;
    for (const DataRange& r : kRanges) {
        if (a - r.begin >= r.size) continue;
        for (const char* k : r.kinds) {
            if (!k) continue;
            if (const std::string* t = LookupKind(k, s, n)) {
                static int logged = 0;
                if (logged < 10) { logged++; LogLine("[kind] %s at %p: %.*s", k, s, n, s); }
                return t;
            }
        }
        break;
    }
    LogNameAddress(s, n);
    return Lookup(s, n);
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
typedef void (__cdecl *DrawLine_t)(int, int, const char*, void*, RECT*);
typedef int  (__cdecl *Width_t)(const char*, void*);

DrawLine_t o_DrawLine;
Width_t    o_Width;

void __cdecl h_DrawLine(int x, int y, const char* s, void* target, RECT* clip) {
    if (!s || !*P_READY || !*P_CURFONT) { o_DrawLine(x, y, s, target, clip); return; }
    int n = (int)strlen(s);
    Seen("L", s, n);
    if (const std::string* t = LookupAt(s, n)) { s = t->c_str(); n = (int)t->size(); }
    if (!HasHigh(s, n)) { o_DrawLine(x, y, s, target, clip); return; }

    int cx = x, cy = y;
    std::string run;
    const char* p = s;
    while (*p) {
        int k = LowRunLen(p);
        if (k > 0) {
            run.assign(p, k);
            o_DrawLine(cx, cy, run.c_str(), target, clip);
            // 元の関数は末尾の空白の分だけ位置を進めないことがあるので、幅を測って大きいほうを使う
            int adv = cx + o_Width(run.c_str(), nullptr);
            cx = *P_X > adv ? *P_X : adv; cy = *P_Y;
            p += k;
            continue;
        }
        k = HighRunLen(p);
        cx += DrawSjis(cx, cy, p, k, target, clip);
        *P_X = cx; *P_Y = cy;
        p += k;
    }
}

int __cdecl h_Width(const char* s, void* font) {
    if (!s) return o_Width(s, font);
    int n = (int)strlen(s);
    if (const std::string* t = LookupAt(s, n)) { s = t->c_str(); n = (int)t->size(); }
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

// ---------------------------------------------------------------- 折り返し
// 0x7ed780 Wrap(str, char** starts, int* lens, int maxLines, int len, int maxWidth, const char* brk) -> 行数
// 元の関数は空白でしか折り返さず、0x80 以上のバイトの幅を文字画像の表の外から読む。
// 日本語を含み、maxWidth > 0、brk == 0 のときだけ、ここで折り返す (それ以外は元の関数)。
// 元の関数の決まり (PLAN.md 3.1 参照):
//   '\n' は印を立てるだけ。印のあとの '\r' と '~' は改行。印のない '\r' は空白として扱う。0x20 未満は無視。
//   行の長さは「最後の空白でない文字」まで。次の行は折り返した位置の次のバイトから始まる。
typedef int (__cdecl *Wrap_t)(const char*, const char**, int*, int, int, int, const char*);
Wrap_t o_Wrap;
const DWORD A_WRAP = 0x7ed780;

std::unordered_map<unsigned long long, int> g_charWidth; // (フォント, 文字) -> 幅

// 文字画像の表は1文字 56 バイト (0x7fa9c8: c*7*8)。等幅のときは +4 の幅 ([0xfe8aeea] != 0)
int AsciiAdvance(void* font, BYTE c) {
    if (*(BYTE*)0xfe8aeea) return ((int*)font)[1];
    return *(int*)((BYTE*)font + 0x824 + 56 * c);
}

int SjisCharWidth(void* font, const char* p) {
    unsigned long long key = ((unsigned long long)(uintptr_t)font << 16) | ((BYTE)p[0] << 8) | (BYTE)p[1];
    auto it = g_charWidth.find(key);
    if (it != g_charWidth.end()) return it->second;
    int w = MeasureSjis(font, p, 2);
    g_charWidth[key] = w;
    return w;
}

// 行頭に置かない文字 (句読点、閉じ括弧、長音、小書きの仮名など)
// ソースは UTF-8 なので、Shift_JIS の2バイト文字の並びに直してから比べる
bool NoLineStart(const char* p, bool dbcs) {
    if (!dbcs) return strchr(")]},.:;!?%", *p) != nullptr && *p;
    static const std::string list = Sjis(u8"、。，．・：；？！ー」』）】〕｝〉》々ぁぃぅぇぉっゃゅょァィゥェォッャュョ…％");
    return HasSjisChar(list, p);
}
// 行末に置かない文字 (開き括弧)
bool NoLineEnd(const char* p, bool dbcs) {
    if (!dbcs) return *p == '(' || *p == '[' || *p == '"';
    static const std::string list = Sjis(u8"「『（【〔｛〈《");
    return HasSjisChar(list, p);
}

int __cdecl h_Wrap(const char* s, const char** starts, int* lens, int maxLines, int len, int maxWidth, const char* brk) {
    void* font = *P_CURFONT;
    if (s && len > 0 && (maxWidth <= 0 || brk || !font)) {
        static int logged0 = 0;
        if (logged0 < 30 && HasHigh(s, (int)strnlen(s, (size_t)len))) {
            logged0++;
            LogLine("[wrap] original caller=%p max=%d len=%d brk=%p", __builtin_return_address(0), maxWidth, len, brk);
        }
    }
    if (!s || len <= 0 || maxWidth <= 0 || brk || !font) return o_Wrap(s, starts, lens, maxLines, len, maxWidth, brk);
    int n = (int)strnlen(s, (size_t)len);
    if (!HasHigh(s, n)) return o_Wrap(s, starts, lens, maxLines, len, maxWidth, brk);

    int count = 0, i = 0;
    while (i < n && count < maxLines) {
        int lineStart = i, width = 0, lastNormal = lineStart - 1, breakPos = -1, prev = -1; // prev: 直前の文字の先頭
        bool prevDbcs = false, flag = false, forced = false, wrapped = false;
        while (i < n) {
            BYTE c = (BYTE)s[i];
            if (c == '\n') { flag = true; i++; continue; }
            if (c == '~' || (c == '\r' && flag)) { forced = true; i++; break; }
            bool space = c == ' ' || c == '\r';
            if (c < 0x20 && !space) { i++; continue; }
            bool dbcs = c >= 0x80 && IsDBCSLeadByteEx(932, c) && i + 1 < n;
            int step = dbcs ? 2 : 1;
            if (space) {
                if (lastNormal >= lineStart) breakPos = lastNormal;
                width += AsciiAdvance(font, ' ');
            } else {
                // 日本語の文字の前後では、空白がなくても折り返せる
                if (prev >= 0 && (dbcs || prevDbcs) && !NoLineStart(s + i, dbcs) && !NoLineEnd(s + prev, prevDbcs))
                    breakPos = i - 1;
                width += dbcs ? SjisCharWidth(font, s + i) : (c < 0x80 ? AsciiAdvance(font, c) : 0);
                lastNormal = i + step - 1;
                prev = i; prevDbcs = dbcs;
            }
            i += step;
            if (width > maxWidth && breakPos >= lineStart) { i = breakPos + 1; wrapped = true; break; }
        }
        // 幅を超えて折り返したなら折り返し位置まで、改行や文の終わりなら最後の空白でない文字まで
        int end = wrapped ? breakPos : lastNormal;
        if (end < lineStart) end = lineStart; // 元の関数と同じく、空の行も長さ 1
        starts[count] = s + lineStart;
        lens[count] = end - lineStart + 1;
        count++;
    }
    static int logged = 0;
    if (count > 1 && logged < 40) {
        logged++;
        std::string out;
        for (int k = 0; k < count; k++) { out += '|'; out.append(starts[k], lens[k]); }
        LogLine("[wrap] caller=%p max=%d len=%d font=%p fh=%d lines=%d %s", __builtin_return_address(0), maxWidth, len,
                font, FontHeight(font), count, out.c_str());
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------- シナリオ説明の行の整形
// シナリオ説明の読み込み (0x61ef00 付近) は、fgets で最大 80 バイトずつ1行を読み、
// 幅 280px (0x118) を超える行は strrchr(' ') で最後の空白を探して切り詰める。空白がなければ行を捨てる。
// 日本語には空白がないので、ファイルを渡す前に、ここで幅と長さに収まるよう改行しておく。
std::string WrapScenText(const std::string& text, int maxWidth, int maxBytes) {
    void* font = (void*)0x3aebcd0; // 通常のフォント (PLAN.md 3.1)
    int fh = FontHeight(font);
    if (fh <= 0 || fh > 100 || ((int*)font)[0] != 128) font = *P_CURFONT;
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 1行を、幅と長さに収まる部分に分ける
        const char* s = line.c_str();
        int n = (int)line.size(), i = 0;
        do {
            int start = i, width = 0, breakPos = -1, prev = -1;
            bool prevDbcs = false;
            int j = i;
            while (j < n) {
                BYTE c = (BYTE)s[j];
                bool dbcs = c >= 0x80 && IsDBCSLeadByteEx(932, c) && j + 1 < n;
                int step = dbcs ? 2 : 1;
                int w = !font ? (dbcs ? 12 : 6) : dbcs ? SjisCharWidth(font, s + j) : (c >= 0x20 && c < 0x80 ? AsciiAdvance(font, c) : 0);
                if (c == ' ') breakPos = j; // 空白の前で切る (空白は捨てる)
                else if (prev >= 0 && (dbcs || prevDbcs) && !NoLineStart(s + j, dbcs) && !NoLineEnd(s + prev, prevDbcs))
                    breakPos = j;           // j の前で切る
                if (j > start && (width + w > maxWidth || j + step - start > maxBytes) && breakPos > start) break;
                width += w;
                if (c != ' ') { prev = j; prevDbcs = dbcs; }
                j += step;
            }
            int end = j >= n ? n : breakPos;
            out.append(s + start, end - start);
            out += "\r\n";
            i = end;
            while (i < n && s[i] == ' ') i++; // 折り返した位置の空白は捨てる
        } while (i < n);
        if (eol >= text.size()) break;
        pos = eol + 1;
    }
    return out;
}

// ---------------------------------------------------------------- 書式の差し替え
// 引数の数が決まっていない関数 (sprintf など) は、スタック上の書式の引数だけを訳文に
// 差し替えてから、元の関数 (の先頭を写した入口) へ飛ぶ。
//   0x7f13c0 (size, buf, fmt, va)  ゲーム側の書式組み立ての共通の入口 (sprintf/vsprintf 相当、
//                                   ツールチップの登録 0x40d180 もここを通る)
//   0x80f1ab (buf, fmt, ...)       C 標準ライブラリの sprintf (静的リンク、1479 か所から呼ばれる)
//
// あわせて、書式の %s に入る文字列 (文の断片や、艦名・地名などの名前) も対訳表で引き、
// 訳があれば訳文を指すポインタに差し替える。%.20s のような上限があるときは、
// 日本語の文字の途中で切れないよう、上限のバイト数に収めた訳文を渡す。
// ファイル名を組み立てる書式 (.bmp や \ を含む) では、引数を差し替えない。
namespace {
bool g_translateArgs = true;
CRITICAL_SECTION g_argCs;
std::unordered_map<unsigned long long, std::string> g_truncCache; // (訳文のポインタ, 上限) -> 切り詰めた訳文

bool IsFileFormat(const char* f) {
    static const char* const ext[] = { ".bmp", ".BMP", ".wav", ".WAV", ".dat", ".txt", ".pws", ".avi",
                                       ".ogg", ".csv", ".ini", ".cmt", ".sfx", ".SFX", ".prfs", ".db" };
    if (strchr(f, '\\')) return true;
    for (const char* e : ext) if (strstr(f, e)) return true;
    return false;
}

// s の訳を引く。prec >= 0 なら、そのバイト数に収める (2バイト文字を途中で切らない)
const char* LookupArg(const char* s, int prec) {
    size_t n = strnlen(s, 1024);
    if (n == 0 || n >= 1024) return nullptr;
    const std::string* t = LookupAt(s, (int)n);
    if (!t) return nullptr;
    if (prec < 0 || (int)t->size() <= prec) return t->c_str();
    unsigned long long key = (unsigned long long)(uintptr_t)t * 1024 + (unsigned)prec;
    EnterCriticalSection(&g_argCs);
    auto it = g_truncCache.find(key);
    if (it == g_truncCache.end()) {
        int cut = 0;
        while (cut < (int)t->size()) {
            int step = IsDBCSLeadByteEx(932, (BYTE)(*t)[cut]) ? 2 : 1;
            if (cut + step > prec) break;
            cut += step;
        }
        it = g_truncCache.emplace(key, t->substr(0, cut)).first;
    }
    const char* r = it->second.c_str();
    LeaveCriticalSection(&g_argCs);
    return r;
}

// 書式の中の %s 1つ分の記録
struct SArg {
    const char* begin;  // 書式の中の '%' の位置
    const char* end;    // 's' の次の位置
    char** slot;        // 引数の置き場所
    const char* orig;   // 差し替える前の文字列
};

// 書式 f をたどり、引数の並び ap の中の %s の文字列を訳文に差し替える。specs があれば %s の記録を入れる
void TranslateArgs(const char* f, char* ap, std::vector<SArg>* specs = nullptr) {
    for (const char* p = f; *p; p++) {
        if (*p != '%') continue;
        const char* begin = p;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0", *p)) p++;
        if (*p == '*') { ap += 4; p++; }
        else while (*p >= '0' && *p <= '9') p++;
        int prec = -1;
        if (*p == '.') {
            p++;
            if (*p == '*') { prec = *(int*)ap; ap += 4; p++; }
            else { prec = 0; while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        }
        int size = 4;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { size = 8; p += 3; }
        else if (p[0] == 'l' && p[1] == 'l') { size = 8; p += 2; }
        else if (*p == 'L') { size = 8; p++; }
        else if (*p == 'h' || *p == 'l' || *p == 'w') p++;
        char c = *p;
        if (!c) break;
        if (strchr("eEfgGaA", c)) size = 8;
        if (c == 's') {
            char** slot = (char**)ap;
            const char* orig = *slot;
            if (*slot) {
                if (const char* t = LookupArg(*slot, prec)) *slot = (char*)t;
            }
            if (specs) specs->push_back({ begin, p + 1, slot, orig });
        }
        ap += size;
    }
}

// ---------------------------------------------------------------- 地上部隊の名前のつなぎ方
// 地上部隊の名前は「名前 %s 種類」(例 "%s %s" に "Kure 3rd" と "SNLF") で組み立てられる。
// 名前が日本語に訳されていて、種類 (witpae_ja\unit_types.tsv) も日本語のとき:
//   - 間の空白を詰める (呉第3 陸戦隊 → 呉第3陸戦隊)
//   - 名前の訳がすでに種類を含んでいれば (隊・団・部・群で終わる、種類の訳で終わる、海軍・陸軍以外の「軍」で終わる)、
//     種類を付けない (第10特別根拠地隊 根拠地隊 → 第10特別根拠地隊)
std::unordered_map<std::string, std::string> g_joinFmts; // 空白を詰めた書式 (書式の文字列 + 位置 -> 新しい書式)

bool EndsWith(const char* s, const char* t) {
    size_t n = strlen(s), m = strlen(t);
    if (m == 0 || m > n) return false;
    if (memcmp(s + n - m, t, m) != 0) return false;
    // 2バイト文字の途中から一致していないか確かめる
    size_t i = 0;
    while (i < n - m) i += IsDBCSLeadByteEx(932, (BYTE)s[i]) ? 2 : 1;
    return i == n - m;
}

bool HasUnitWord(const char* name, const char* type) {
    static const std::string words = Sjis(u8"隊団部群");
    static const std::string gun = Sjis(u8"軍"), notGun = Sjis(u8"海陸空");
    if (EndsWith(name, type)) return true;
    size_t n = strlen(name);
    if (n < 2) return false;
    const char* last = name + n - 2;
    if (!EndsWith(name, std::string(last, 2).c_str())) return false; // 最後が2バイト文字でない
    if (HasSjisChar(words, last)) return true;
    if (last[0] != gun[0] || last[1] != gun[1]) return false;
    // 「軍」で終わる: 直前が 海・陸・空 (第1海軍 など、名前の一部) でなければ種類を含むとみなす
    if (n >= 4 && EndsWith(name, std::string(last - 2, 4).c_str()) && HasSjisChar(notGun, last - 2)) return false;
    return true;
}

// f2: 訳した書式。specs: TranslateArgs の記録。空白を詰めた書式を返す (変えなければ f2)
const char* JoinUnitNames(const char* f2, std::vector<SArg>& specs) {
    std::vector<int> cut; // 書式から消す空白の位置
    for (size_t i = 0; i + 1 < specs.size(); i++) {
        SArg& a = specs[i];
        SArg& b = specs[i + 1];
        if (a.end[0] != ' ' || a.end + 1 != b.begin) continue;
        if (!a.orig || !b.orig || !IsUnitType(b.orig)) continue;
        const char* name = *a.slot;
        const char* type = *b.slot;
        if (!name || !type || name == a.orig || type == b.orig) continue; // どちらかが訳されていない
        if (!HasHigh(name, (int)strlen(name)) || !HasHigh(type, (int)strlen(type))) continue;
        if (HasUnitWord(name, type)) *b.slot = (char*)"";
        cut.push_back((int)(a.end - f2));
    }
    if (cut.empty()) return f2;
    std::string key(f2);
    for (int c : cut) { key += '\x01'; key += std::to_string(c); }
    EnterCriticalSection(&g_argCs);
    auto it = g_joinFmts.find(key);
    if (it == g_joinFmts.end()) {
        std::string s(f2);
        for (size_t k = cut.size(); k-- > 0;) s.erase(cut[k], 1);
        it = g_joinFmts.emplace(key, s).first;
    }
    const char* r = it->second.c_str();
    LeaveCriticalSection(&g_argCs);
    return r;
}
} // namespace

extern "C" {
void* t_Vsn = nullptr;
void* t_Sprintf = nullptr;

// fmt を訳文の書式に差し替え、引数の %s も訳す。args は引数の並びの先頭 (va_list)
const char* __cdecl TranslateFmtArgs(const char* fmt, char* args) {
    if (!fmt) return fmt;
    int n = (int)strlen(fmt);
    Seen("F", fmt, n);
    const char* f2 = fmt;
    if (const std::string* t = Lookup(fmt, n)) f2 = t->c_str();
    if (g_translateArgs && args && !IsFileFormat(fmt)) {
        std::vector<SArg> specs;
        TranslateArgs(f2, args, &specs);
        if (specs.size() >= 2) f2 = JoinUnitNames(f2, specs);
    }
    return f2;
}

// 入口では [esp]=戻り先、[esp+4]=size、[esp+8]=buf、[esp+12]=fmt、[esp+16]=va
__attribute__((naked, used)) void s_Vsn() {
    __asm__ __volatile__(
        "pushl 16(%esp)\n\t"        // va
        "pushl 16(%esp)\n\t"        // fmt (1つ積んだので +4 ずれている)
        "call _TranslateFmtArgs\n\t"
        "addl $8, %esp\n\t"
        "movl %eax, 12(%esp)\n\t"
        "jmp *_t_Vsn\n\t");
}

// vsprintf (0x8111b0)。入口では [esp]=戻り先、[esp+4]=buf、[esp+8]=fmt、[esp+12]=va
// 戦闘報告の行を組み立てる 0x505290 / 0x5051c0 付近から呼ばれる
void* t_Vsprintf = nullptr;
__attribute__((naked, used)) void s_Vsprintf() {
    __asm__ __volatile__(
        "pushl 12(%esp)\n\t"        // va
        "pushl 12(%esp)\n\t"        // fmt (1つ積んだので +4 ずれている)
        "call _TranslateFmtArgs\n\t"
        "addl $8, %esp\n\t"
        "movl %eax, 8(%esp)\n\t"
        "jmp *_t_Vsprintf\n\t");
}

// 入口では [esp]=戻り先、[esp+4]=buf、[esp+8]=fmt、[esp+12]=可変長の引数の先頭
__attribute__((naked, used)) void s_Sprintf() {
    __asm__ __volatile__(
        "leal 12(%esp), %eax\n\t"   // 引数の並びの先頭
        "pushl %eax\n\t"
        "pushl 12(%esp)\n\t"        // fmt (1つ積んだので +4 ずれている)
        "call _TranslateFmtArgs\n\t"
        "addl $8, %esp\n\t"
        "movl %eax, 8(%esp)\n\t"
        "jmp *_t_Sprintf\n\t");
}
}

namespace {
} // namespace

void InstallGameHooks() {
    std::string ini = std::string(g_dir) + "witpae_ja.ini";
    g_jpHeightAdjust = GetPrivateProfileIntA("JpFont", "HeightAdjust", -2, ini.c_str());
    g_jpYOffset = GetPrivateProfileIntA("JpFont", "YOffset", 0, ini.c_str());
    g_translateArgs = GetPrivateProfileIntA("Text", "TranslateArgs", 1, ini.c_str()) != 0;
    g_logNameAddr = GetPrivateProfileIntA("Debug", "LogNameAddr", 0, ini.c_str()) != 0;
    InitializeCriticalSection(&g_argCs);
    struct Item { const char* name; DWORD addr; const char* bytes; int copy; void* hook; void** orig; };
    Item items[] = {
        // name       addr        先頭の機械語 (版の確認)         写す長さ  フック            元の関数への入口
        {"DrawLine", A_DRAWLINE, "83ec18833d4484391000",            10, (void*)h_DrawLine, (void**)&o_DrawLine},
        {"Width",    A_WIDTH,    "558b6c2408",                      5,  (void*)h_Width,    (void**)&o_Width},
        {"Vsnprintf", A_VSNPRINT, "81ec1c0f0000",                   6,  (void*)s_Vsn,      &t_Vsn},
        {"sprintf",  A_SPRINTF,  "558bec83ec20",                    6,  (void*)s_Sprintf,  &t_Sprintf},
        {"vsprintf", A_VSPRINTF, "ff74240c6a00",                    6,  (void*)s_Vsprintf, &t_Vsprintf},
        {"Wrap",     A_WRAP,     "83ec185333db",                    6,  (void*)h_Wrap,     (void**)&o_Wrap},
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
