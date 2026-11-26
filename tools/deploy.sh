#!/bin/sh
# ビルドした DLL と訳文をゲームのフォルダへ配置する（既存の元ファイルには触れない。開発用）
# ゲームの場所は環境変数 WITPAE_GAME で変えられる（Windows 形式でも可）
set -e
W="$(cd "$(dirname "$0")/.." && pwd)"
G="${WITPAE_GAME:-D:/Matrix Games/War in the Pacific Admiral's Edition}"
command -v cygpath >/dev/null 2>&1 && G="$(cygpath -u "$G")"
[ -f "$G/War in the Pacific Admiral Edition.exe" ] || { echo "ゲームが見つからない: $G（WITPAE_GAME で指定する）"; exit 1; }
cp "$W/src/ddraw/build/ddraw.dll" "$G/ddraw.dll"
[ -f "$G/witpae_ja.ini" ] || cp "$W/witpae_ja.ini" "$G/witpae_ja.ini"
mkdir -p "$G/witpae_ja/names"
cp "$W/translation/ja.tsv" "$G/witpae_ja/ja.tsv"
cp "$W/translation/unit_types.tsv" "$G/witpae_ja/unit_types.tsv"
rm -f "$G/witpae_ja/strings.tsv"
if ls "$W/translation/names/"*.tsv >/dev/null 2>&1; then cp "$W/translation/names/"*.tsv "$G/witpae_ja/names/"; fi
# シナリオ説明の訳。書き出し先は既定（ゲームの witpae_ja\SCEN）に任せる。
# Python に /d/... の形のパスを渡すと D:\d\... に書かれてしまうので渡さない。
python "$W/tools/scen.py" build
echo "deployed to $G"
