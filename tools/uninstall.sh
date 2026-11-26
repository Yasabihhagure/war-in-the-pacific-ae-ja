#!/bin/sh
# 日本語化で追加したファイルをゲームのフォルダから取り除く（英語版に戻る）
# ゲームの場所は環境変数 WITPAE_GAME で変えられる（Windows 形式でも可）
G="${WITPAE_GAME:-D:/Matrix Games/War in the Pacific Admiral's Edition}"
command -v cygpath >/dev/null 2>&1 && G="$(cygpath -u "$G")"
[ -f "$G/War in the Pacific Admiral Edition.exe" ] || { echo "ゲームが見つからない: $G（WITPAE_GAME で指定する）"; exit 1; }
rm -f "$G/ddraw.dll" "$G/witpae_ja.ini" "$G/witpae_ja_log.txt" "$G/witpae_ja_README.txt" "$G/WITPAE ウィンドウモード.lnk" "$G/WITPAE Windowed.lnk"
rm -rf "$G/witpae_ja"
echo "removed"
