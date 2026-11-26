// ddraw_proxy.cpp と game_hooks.cpp で共有する宣言
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

extern char g_dir[MAX_PATH];          // ゲームのフォルダ (末尾に \ )
extern char g_fontFace[LF_FACESIZE];  // 日本語の描画に使うフォント名

void LogLine(const char* fmt, ...);
// 新しく見た文字列をログに残す (tag: 種類を表す1文字)
void Seen(const char* tag, const char* s, int n);
// 英文に対応する訳文 (CP932) を返す。なければ nullptr
const std::string* Lookup(const char* s, int n);

// game_hooks.cpp: exe 内部の関数をメモリ上で差し替える
void InstallGameHooks();
// シナリオ説明の文 (CP932) を、幅 maxWidth px・1行 maxBytes バイト以内に改行し直す (改行は CRLF)
std::string WrapScenText(const std::string& text, int maxWidth, int maxBytes);

// display.cpp: ウィンドウモードの画面を拡大する
void DisplayInit();
void DisplayOnDirectDraw(void* dd);
void* DisplayHookRegisterClassA(void* orig);
void* DisplayHookCreateWindowExA(void* orig);
void* DisplayPrimarySurface();

// scaler.cpp: ゲーム画面の写し (キャンバス) を持ち、自前で拡大して窓へ描く
void CanvasResize(int w, int h);
bool CanvasValid();
bool CanvasUpdate(void* surf, RECT sr, int gx, int gy);
bool CanvasCopyTo(void* surf, RECT dr, int gx, int gy);
bool CanvasPresent(HDC wdc, int gx0, int gy0, int gx1, int gy1, int dx, int dy, int dw, int dh, bool smooth);
