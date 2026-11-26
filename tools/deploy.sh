#!/bin/sh
# ビルドした DLL と訳文をゲームのフォルダへ配置する（既存の元ファイルには触れない）
set -e
W="/d/ゲーム日本語化/WarInThePacificAdmiralsEdition"
G="/d/Matrix Games/War in the Pacific Admiral's Edition"
cp "$W/src/ddraw/build/ddraw.dll" "$G/ddraw.dll"
[ -f "$G/witpae_ja.ini" ] || cp "$W/witpae_ja.ini" "$G/witpae_ja.ini"
mkdir -p "$G/witpae_ja/names"
cp "$W/translation/ja.tsv" "$G/witpae_ja/ja.tsv"
rm -f "$G/witpae_ja/strings.tsv"
if ls "$W/translation/names/"*.tsv >/dev/null 2>&1; then cp "$W/translation/names/"*.tsv "$G/witpae_ja/names/"; fi
echo "deployed to $G"
