"""配布用の zip を dist/ に作る。

中身 (ゲームのフォルダにそのまま展開するもの):
  ddraw.dll                 差し替え DLL (src/ddraw/build/ddraw.dll。先に sh src/ddraw/build.sh)
  witpae_ja.ini             設定
  witpae_ja/ja.tsv          画面の文言の訳
  witpae_ja/unit_types.tsv  地上部隊の種類
  witpae_ja/names/*.tsv     名前の訳
  witpae_ja/SCEN/*.txt      シナリオ説明の訳 (CP932。tools/scen.py build で作る)
  witpae_ja/README.md, LICENSE

原作のファイルは含めない。シナリオ説明の書き出しには、手元のゲームの SCEN フォルダ (どのファイル名に訳を置くか) を使う。

使い方: python tools/package.py [版の名前 (既定は今日の日付)]
"""
import datetime
import glob
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ver = sys.argv[1] if len(sys.argv) > 1 else datetime.date.today().strftime("%Y%m%d")
    dll = os.path.join(ROOT, "src", "ddraw", "build", "ddraw.dll")
    if not os.path.exists(dll):
        sys.exit("ddraw.dll がない。先に sh src/ddraw/build.sh を実行する")
    files = [
        (dll, "ddraw.dll"),
        (os.path.join(ROOT, "witpae_ja.ini"), "witpae_ja.ini"),
        (os.path.join(ROOT, "translation", "ja.tsv"), "witpae_ja/ja.tsv"),
        (os.path.join(ROOT, "translation", "unit_types.tsv"), "witpae_ja/unit_types.tsv"),
        (os.path.join(ROOT, "README.md"), "witpae_ja/README.md"),
        (os.path.join(ROOT, "LICENSE"), "witpae_ja/LICENSE"),
    ]
    for p in sorted(glob.glob(os.path.join(ROOT, "translation", "names", "*.tsv"))):
        files.append((p, "witpae_ja/names/" + os.path.basename(p)))
    with tempfile.TemporaryDirectory() as tmp:
        scen = os.path.join(tmp, "SCEN")
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "scen.py"), "build", scen], check=True)
        for p in sorted(glob.glob(os.path.join(scen, "*"))):
            files.append((p, "witpae_ja/SCEN/" + os.path.basename(p)))
        os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
        out = os.path.join(ROOT, "dist", "witpae-ja-%s.zip" % ver)
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            for src, arc in files:
                z.write(src, arc)
    print("%s: %d files, %d bytes" % (out, len(files), os.path.getsize(out)))


if __name__ == "__main__":
    main()
