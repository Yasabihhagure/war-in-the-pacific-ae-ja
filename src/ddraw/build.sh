#!/bin/sh
# 差し替え ddraw.dll をビルドする（i686 / llvm-mingw）
# llvm-mingw の場所は環境変数 LLVM_MINGW で変えられる（既定 D:\Tools\llvm-mingw）
set -e
LLVM="${LLVM_MINGW:-D:/Tools/llvm-mingw}"
cd "$(dirname "$0")"
mkdir -p build
"$LLVM/bin/i686-w64-mingw32-clang++" -O2 -std=c++17 -shared -static \
  -o build/ddraw.dll ddraw_proxy.cpp game_hooks.cpp display.cpp scaler.cpp ddraw.def -lgdi32 -luser32 \
  -Wl,--enable-stdcall-fixup -Wl,--kill-at
echo BUILD OK
