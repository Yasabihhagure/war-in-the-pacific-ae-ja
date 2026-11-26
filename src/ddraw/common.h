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

// display.cpp: ウィンドウモードの画面を拡大する
void DisplayInit();
void DisplayOnDirectDraw(void* dd);
void* DisplayHookRegisterClassA(void* orig);
void* DisplayHookCreateWindowExA(void* orig);
void* DisplayPrimarySurface();
