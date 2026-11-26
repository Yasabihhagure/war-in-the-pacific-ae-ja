#!/bin/sh
# 日本語化で追加したファイルをゲームのフォルダから取り除く（英語版に戻る）
G="/d/Matrix Games/War in the Pacific Admiral's Edition"
rm -f "$G/ddraw.dll" "$G/witpae_ja.ini" "$G/witpae_ja_log.txt" "$G/WITPAE ウィンドウモード.lnk"
rm -rf "$G/witpae_ja"
echo "removed"
