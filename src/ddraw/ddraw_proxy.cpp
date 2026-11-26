// War in the Pacific AE 日本語化用 差し替え ddraw.dll
// - 本物の ddraw.dll (System32/SysWOW64) へ転送する
// - exe の IAT を書き換えて GDI/USER の文字関数をフックする
// - 描画される文字列をログに取り、辞書で訳文 (CP932) に置き換える
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <share.h>
#include "common.h"

// ---------------------------------------------------------------- 設定
char g_dir[MAX_PATH];        // ゲームのフォルダ (末尾に \ )
char g_fontFace[LF_FACESIZE] = "MS Gothic";
static int  g_fontCharset = SHIFTJIS_CHARSET;
static int  g_heightAdjust = 0;
static bool g_replaceFont = true;
static bool g_logText = true;
static bool g_translate = true;

static CRITICAL_SECTION g_cs;
static FILE* g_log = nullptr;
static std::unordered_set<std::string> g_seen;
// 英文のハッシュ (FNV-1a 64bit) -> 訳文(CP932)
// リポジトリに原作の英文を含めないため、辞書のキーはハッシュにしている
static std::unordered_map<unsigned long long, std::string> g_dict;
// 地上部隊の種類 (名前のあとに "%s %s" で付く語) の英文のハッシュ。witpae_ja\unit_types.tsv
static std::unordered_set<unsigned long long> g_unitTypes;

static unsigned long long Fnv1a64(const char* s, int n) {
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 0x100000001b3ULL; }
    return h;
}

void LogLine(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static std::string Escape(const char* s, int n) {
    std::string o;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') o += "\\\\";
        else if (c == '\t') o += "\\t";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c < 0x20) { char b[8]; sprintf_s(b, "\\x%02X", c); o += b; }
        else o += (char)c;
    }
    return o;
}

static std::string Unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            if (c == 't') o += '\t';
            else if (c == 'n') o += '\n';
            else if (c == 'r') o += '\r';
            else if (c == '\\') o += '\\';
            else if (c == 'x' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
            else { o += '\\'; o += c; }
        } else o += s[i];
    }
    return o;
}

static std::string Utf8ToCp932(const std::string& u) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring w(wn, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), &w[0], wn);
    int an = WideCharToMultiByte(932, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string a(an, 0);
    WideCharToMultiByte(932, 0, w.data(), wn, &a[0], an, nullptr, nullptr);
    return a;
}

// ja.tsv: キー \t 訳文 \t メモ  (UTF-8, '#' 始まりはコメント)
// キーは "h:" + 英文の FNV-1a 64bit を16桁の16進で書いたもの
// 1つの対訳ファイルを読む。名前の対訳 (names\*.tsv) は、画面の文言 (ja.tsv) より後に読み、
// 同じキーがあれば画面の文言のほうを優先する (prefer=false)
static int LoadDictFile(const std::string& path, bool prefer) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return -1;
    char buf[8192];
    int n = 0;
    bool first = true;
    while (fgets(buf, sizeof buf, f)) {
        std::string line(buf);
        if (first) { first = false; if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3); }
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        size_t p = 0;
        while (true) { size_t q = line.find('\t', p); col.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
        if (col.size() < 2 || col[1].empty() || col[0].size() != 18 || col[0].compare(0, 2, "h:") != 0) continue;
        unsigned long long k = strtoull(col[0].c_str() + 2, nullptr, 16);
        if (!prefer && g_dict.count(k)) continue;
        g_dict[k] = Utf8ToCp932(Unescape(col[1]));
        n++;
    }
    fclose(f);
    return n;
}

static void LoadDict() {
    std::string dir = std::string(g_dir) + "witpae_ja\\";
    int n = LoadDictFile(dir + "ja.tsv", true);
    LogLine("[dict] ja.tsv: %d entries", n);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "names\\*.tsv").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int m = LoadDictFile(dir + "names\\" + fd.cFileName, false);
            LogLine("[dict] names\\%s: %d entries", fd.cFileName, m);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    LogLine("[dict] total %u entries", (unsigned)g_dict.size());
    // 地上部隊の種類 (key だけ使う)
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "unit_types.tsv").c_str(), "rb") == 0 && f) {
        char buf[1024];
        while (fgets(buf, sizeof buf, f))
            if (buf[0] == 'h' && buf[1] == ':') g_unitTypes.insert(strtoull(buf + 2, nullptr, 16));
        fclose(f);
    }
    LogLine("[dict] unit_types.tsv: %u entries", (unsigned)g_unitTypes.size());
}

bool IsUnitType(const char* s) {
    if (!s || g_unitTypes.empty()) return false;
    size_t n = strnlen(s, 64);
    return n > 0 && n < 64 && g_unitTypes.count(Fnv1a64(s, (int)n)) != 0;
}

static void LoadIni() {
    std::string ini = std::string(g_dir) + "witpae_ja.ini";
    GetPrivateProfileStringA("Font", "Face", g_fontFace, g_fontFace, sizeof g_fontFace, ini.c_str());
    g_fontCharset = GetPrivateProfileIntA("Font", "Charset", g_fontCharset, ini.c_str());
    g_heightAdjust = GetPrivateProfileIntA("Font", "HeightAdjust", g_heightAdjust, ini.c_str());
    g_replaceFont = GetPrivateProfileIntA("Font", "Replace", 1, ini.c_str()) != 0;
    g_logText = GetPrivateProfileIntA("Debug", "LogText", 1, ini.c_str()) != 0;
    g_translate = GetPrivateProfileIntA("Text", "Translate", 1, ini.c_str()) != 0;
}

// 新しく見た文字列をログに残す
void Seen(const char* tag, const char* s, int n) {
    if (!g_logText || !s || n <= 0) return;
    std::string key(tag); key += '\t'; key.append(s, n);
    EnterCriticalSection(&g_cs);
    if (g_seen.insert(key).second) LogLine("%s\t%s", tag, Escape(s, n).c_str());
    LeaveCriticalSection(&g_cs);
}

// 訳文を引く。見つからなければ nullptr
const std::string* Lookup(const char* s, int n) {
    if (!g_translate || !s || n <= 0) return nullptr;
    if (g_dict.empty()) return nullptr;
    unsigned long long h = Fnv1a64(s, n);
    auto it = g_dict.find(h);
    if (it != g_dict.end()) return &it->second;
    // 末尾の "%番号@" (ツールチップなどの番号) を外して探し、見つかれば番号を付け直す。
    // 例: "Exit%2873@" → "Exit" の訳 + "%2873@"。作った訳は次から直接引けるように辞書に入れる
    if (n >= 3 && s[n - 1] == '@') {
        int i = n - 2;
        while (i >= 0 && s[i] >= '0' && s[i] <= '9') i--;
        if (i >= 0 && i < n - 2 && s[i] == '%') {
            auto base = g_dict.find(Fnv1a64(s, i));
            if (base != g_dict.end()) {
                EnterCriticalSection(&g_cs);
                std::string& v = g_dict[h];
                v = base->second;
                v.append(s + i, n - i);
                LeaveCriticalSection(&g_cs);
                return &v;
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------- フック
typedef HFONT (WINAPI *CreateFontA_t)(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCSTR);
typedef BOOL  (WINAPI *TextOutA_t)(HDC,int,int,LPCSTR,int);
typedef BOOL  (WINAPI *GetTextExtentPoint32A_t)(HDC,LPCSTR,int,LPSIZE);
typedef BOOL  (WINAPI *SetDlgItemTextA_t)(HWND,int,LPCSTR);
typedef int   (WINAPI *MessageBoxA_t)(HWND,LPCSTR,LPCSTR,UINT);

static CreateFontA_t           o_CreateFontA;
static TextOutA_t              o_TextOutA;
static GetTextExtentPoint32A_t o_GetTextExtentPoint32A;
static SetDlgItemTextA_t       o_SetDlgItemTextA;
static MessageBoxA_t           o_MessageBoxA;

static HFONT WINAPI h_CreateFontA(int h, int w, int esc, int ori, int wt, DWORD it, DWORD ul, DWORD so,
                                  DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, LPCSTR face) {
    EnterCriticalSection(&g_cs);
    LogLine("[font] h=%d w=%d wt=%d it=%lu cs=%lu q=%lu pf=0x%lX face=%s", h, w, wt, it, cs, q, pf, face ? face : "(null)");
    LeaveCriticalSection(&g_cs);
    if (g_replaceFont) {
        int nh = h;
        if (g_heightAdjust) nh = (h < 0) ? h - g_heightAdjust : h + g_heightAdjust;
        return o_CreateFontA(nh, 0, esc, ori, wt, it, ul, so, g_fontCharset, op, cp, q, pf, g_fontFace);
    }
    return o_CreateFontA(h, w, esc, ori, wt, it, ul, so, cs, op, cp, q, pf, face);
}

static BOOL WINAPI h_TextOutA(HDC dc, int x, int y, LPCSTR s, int n) {
    Seen("T", s, n);
    if (const std::string* t = Lookup(s, n)) return o_TextOutA(dc, x, y, t->data(), (int)t->size());
    return o_TextOutA(dc, x, y, s, n);
}

static BOOL WINAPI h_GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE sz) {
    Seen("E", s, n);
    if (const std::string* t = Lookup(s, n)) return o_GetTextExtentPoint32A(dc, t->data(), (int)t->size(), sz);
    return o_GetTextExtentPoint32A(dc, s, n, sz);
}

static BOOL WINAPI h_SetDlgItemTextA(HWND w, int id, LPCSTR s) {
    int n = s ? lstrlenA(s) : 0;
    Seen("D", s, n);
    if (const std::string* t = Lookup(s, n)) return o_SetDlgItemTextA(w, id, t->c_str());
    return o_SetDlgItemTextA(w, id, s);
}

static int WINAPI h_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT f) {
    int n1 = text ? lstrlenA(text) : 0, n2 = cap ? lstrlenA(cap) : 0;
    Seen("M", text, n1); Seen("M", cap, n2);
    const std::string* t1 = Lookup(text, n1);
    const std::string* t2 = Lookup(cap, n2);
    return o_MessageBoxA(w, t1 ? t1->c_str() : text, t2 ? t2->c_str() : cap, f);
}

// ---------------------------------------------------------------- シナリオ説明の txt の差し替え
// ゲームが scen フォルダの .txt を読むとき、witpae_ja\SCEN\ に同じ名前の訳 (CP932) があればそちらを開く。
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t o_CreateFileA;
static bool g_replaceScen = true;

static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_replaceScen && name && disp == OPEN_EXISTING && !(access & GENERIC_WRITE)) {
        size_t n = strlen(name);
        const char* base = name + n;
        while (base > name && base[-1] != '\\' && base[-1] != '/') base--;
        // 直前のフォルダ名が scen で、拡張子が .txt
        const char* dirEnd = base - 1;
        const char* dir = dirEnd;
        while (dir > name && dir[-1] != '\\' && dir[-1] != '/' && dir[-1] != ':') dir--;
        bool isScen = base > name && dirEnd - dir == 4 && _strnicmp(dir, "scen", 4) == 0;
        if (isScen && n > 4 && _stricmp(name + n - 4, ".txt") == 0) {
            std::string alt = std::string(g_dir) + "witpae_ja\\SCEN\\" + base;
            DWORD a = GetFileAttributesA(alt.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                // ゲームの読み込み (fgets 80 バイト、幅 280px、空白でしか折り返さない) に合わせて
                // 改行し直したものを witpae_ja\cache\ に書き、そちらを開く。失敗したら訳をそのまま開く
                std::string src;
                HANDLE r = o_CreateFileA(alt.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (r != INVALID_HANDLE_VALUE) {
                    DWORD size = GetFileSize(r, nullptr), got = 0;
                    if (size != INVALID_FILE_SIZE && size < (1u << 20)) {
                        src.resize(size);
                        if (!ReadFile(r, &src[0], size, &got, nullptr)) got = 0;
                        src.resize(got);
                    }
                    CloseHandle(r);
                }
                if (!src.empty()) {
                    int width = GetPrivateProfileIntA("Text", "ScenWidth", 270, (std::string(g_dir) + "witpae_ja.ini").c_str());
                    std::string wrapped = WrapScenText(src, width, 78);
                    std::string dir2 = std::string(g_dir) + "witpae_ja\\cache";
                    CreateDirectoryA(dir2.c_str(), nullptr);
                    std::string tmp = dir2 + "\\" + base;
                    HANDLE w = o_CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (w != INVALID_HANDLE_VALUE) {
                        DWORD put = 0;
                        BOOL ok = WriteFile(w, wrapped.data(), (DWORD)wrapped.size(), &put, nullptr);
                        CloseHandle(w);
                        if (ok && put == wrapped.size()) alt = tmp;
                    }
                }
                HANDLE h = o_CreateFileA(alt.c_str(), access, share, sa, disp, flags, tmpl);
                if (h != INVALID_HANDLE_VALUE) {
                    static int logged = 0;
                    if (logged < 50) { logged++; LogLine("[scen] %s -> %s", name, alt.c_str()); }
                    return h;
                }
            }
        }
    }
    return o_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

// exe の IAT の1項目を書き換える
static bool PatchIAT(HMODULE mod, const char* dll, const char* func, void* hook, void** orig) {
    BYTE* base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    FARPROC target = GetProcAddress(GetModuleHandleA(dll), func);
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto othunk = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
        for (int i = 0; thunk[i].u1.Function; i++) {
            bool match = false;
            if (othunk && !(othunk[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + othunk[i].u1.AddressOfData);
                match = strcmp((char*)ibn->Name, func) == 0;
            } else {
                match = (FARPROC)thunk[i].u1.Function == target;
            }
            if (!match) continue;
            DWORD old;
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *orig = (void*)thunk[i].u1.Function;
            thunk[i].u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

static void InstallHooks() {
    HMODULE exe = GetModuleHandleA(nullptr);
    struct { const char* dll; const char* fn; void* hook; void** orig; } t[] = {
        {"GDI32.dll",  "CreateFontA",           (void*)h_CreateFontA,           (void**)&o_CreateFontA},
        {"GDI32.dll",  "TextOutA",              (void*)h_TextOutA,              (void**)&o_TextOutA},
        {"GDI32.dll",  "GetTextExtentPoint32A", (void*)h_GetTextExtentPoint32A, (void**)&o_GetTextExtentPoint32A},
        {"USER32.dll", "SetDlgItemTextA",       (void*)h_SetDlgItemTextA,       (void**)&o_SetDlgItemTextA},
        {"USER32.dll", "MessageBoxA",           (void*)h_MessageBoxA,           (void**)&o_MessageBoxA},
        {"KERNEL32.dll", "CreateFileA",         (void*)h_CreateFileA,           (void**)&o_CreateFileA},
    };
    g_replaceScen = GetPrivateProfileIntA("Text", "ReplaceScenText", 1, (std::string(g_dir) + "witpae_ja.ini").c_str()) != 0;
    for (auto& e : t) {
        bool ok = PatchIAT(exe, e.dll, e.fn, e.hook, e.orig);
        LogLine("[hook] %s!%s %s", e.dll, e.fn, ok ? "ok" : "FAILED");
        if (!ok) *e.orig = (void*)GetProcAddress(GetModuleHandleA(e.dll), e.fn);
    }
    // ウィンドウモードの拡大 (display.cpp)
    void* orig = nullptr;
    if (PatchIAT(exe, "USER32.dll", "RegisterClassA", DisplayHookRegisterClassA(nullptr), &orig)) DisplayHookRegisterClassA(orig);
    LogLine("[hook] USER32.dll!RegisterClassA %s", orig ? "ok" : "FAILED");
    orig = nullptr;
    if (PatchIAT(exe, "USER32.dll", "CreateWindowExA", DisplayHookCreateWindowExA(nullptr), &orig)) DisplayHookCreateWindowExA(orig);
    LogLine("[hook] USER32.dll!CreateWindowExA %s", orig ? "ok" : "FAILED");
}

// ---------------------------------------------------------------- ddraw 転送
static HMODULE g_real;
#define DDFWD(name) static FARPROC p_##name;
#define DDLIST(X) X(DirectDrawCreate) X(DirectDrawCreateEx) X(DirectDrawCreateClipper) \
    X(DirectDrawEnumerateA) X(DirectDrawEnumerateExA) X(DirectDrawEnumerateW) X(DirectDrawEnumerateExW) \
    X(DllCanUnloadNow) X(DllGetClassObject) X(AcquireDDThreadLock) X(ReleaseDDThreadLock) \
    X(CompleteCreateSysmemSurface) X(D3DParseUnknownCommand) X(DDGetAttachedSurfaceLcl) \
    X(DDInternalLock) X(DDInternalUnlock) X(DSoundHelp) X(GetDDSurfaceLocal) X(GetOLEThunkData) \
    X(GetSurfaceFromDC) X(RegisterSpecialCase) X(SetAppCompatData)
DDLIST(DDFWD)

static void LoadReal() {
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    strcat_s(sys, "\\ddraw.dll");
    g_real = LoadLibraryA(sys);
    LogLine("[ddraw] real=%s %s", sys, g_real ? "ok" : "FAILED");
#define DDGET(name) p_##name = g_real ? GetProcAddress(g_real, #name) : nullptr;
    DDLIST(DDGET)
}

// 引数はそのままで本物へ jmp する (clang/gcc, i686)
#define DDSTUB(name) extern "C" __attribute__((naked, used)) void fwd_##name() { \
    __asm__ __volatile__("jmp *%0" :: "m"(p_##name)); }
DDLIST(DDSTUB)

// DirectDrawCreateEx だけは、作成後に IDirectDraw7 へ処理を差し込む (display.cpp)
typedef HRESULT (WINAPI *DirectDrawCreateEx_t)(GUID*, void**, REFIID, void*);
extern "C" HRESULT WINAPI my_DirectDrawCreateEx(GUID* guid, void** dd, REFIID iid, void* outer) {
    if (!p_DirectDrawCreateEx) return E_FAIL;
    HRESULT hr = ((DirectDrawCreateEx_t)p_DirectDrawCreateEx)(guid, dd, iid, outer);
    LogLine("[ddraw] DirectDrawCreateEx hr=%08lX", hr);
    if (hr == 0 && dd && *dd) DisplayOnDirectDraw(*dd);
    return hr;
}

// ---------------------------------------------------------------- 表示スケール対策
// Windows の表示スケール(125%/150%など)でゲーム画面が拡大されて切れるのを防ぐ。
// 窓やウィンドウを作る前(=DllMain)に、プロセスを DPI 対応として登録する。
static void SetDpiAware() {
    HMODULE u = GetModuleHandleA("user32.dll");
    typedef BOOL (WINAPI *Ctx_t)(HANDLE);
    typedef BOOL (WINAPI *Old_t)();
    if (auto f = (Ctx_t)GetProcAddress(u, "SetProcessDpiAwarenessContext")) {
        BOOL ok = f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */) || f((HANDLE)-2 /* SYSTEM_AWARE */);
        LogLine("[dpi] SetProcessDpiAwarenessContext %s (err=%lu)", ok ? "ok" : "FAILED", ok ? 0 : GetLastError());
        if (ok) return;
    }
    if (auto f = (Old_t)GetProcAddress(u, "SetProcessDPIAware"))
        LogLine("[dpi] SetProcessDPIAware %s", f() ? "ok" : "FAILED");
}

// ---------------------------------------------------------------- DllMain
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        if (char* p = strrchr(g_dir, '\\')) p[1] = 0;
        LoadIni();
        std::string logPath = std::string(g_dir) + "witpae_ja_log.txt";
        g_log = _fsopen(logPath.c_str(), "w", _SH_DENYNO); // ゲーム実行中も読めるように共有で開く
        LogLine("[start] WITPAE ja proxy. ACP=%u", GetACP());
        if (GetPrivateProfileIntA("Display", "DpiAware", 1, (std::string(g_dir) + "witpae_ja.ini").c_str()))
            SetDpiAware();
        LoadReal();
        LoadDict();
        DisplayInit();
        InstallHooks();
        if (GetPrivateProfileIntA("Text", "GameHooks", 1, (std::string(g_dir) + "witpae_ja.ini").c_str()))
            InstallGameHooks();
    }
    return TRUE;
}
