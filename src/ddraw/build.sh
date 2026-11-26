#!/bin/sh
# 差し替え ddraw.dll をビルドする（i686 / llvm-mingw: D:\Tools\llvm-mingw）
set -e
cd "$(dirname "$0")"
mkdir -p build
/d/Tools/llvm-mingw/bin/i686-w64-mingw32-clang++ -O2 -std=c++17 -shared -static \
  -o build/ddraw.dll ddraw_proxy.cpp game_hooks.cpp display.cpp scaler.cpp ddraw.def -lgdi32 -luser32 \
  -Wl,--enable-stdcall-fixup -Wl,--kill-at
echo BUILD OK
