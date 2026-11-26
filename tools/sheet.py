"""翻訳作業用シートの作成と取り込み。

原作の英文はリポジトリに入れない。公開する translation/ja.tsv には
「英文のハッシュ」と「訳文」だけを書く。英文と訳文を並べた作業用シート
(work/sheet.tsv、Git 管理外) は、手元にあるゲームから毎回作る。

使い方:
  python tools/sheet.py export   # ゲームの exe とログから英文を集め、work/sheet.tsv を作る
  python tools/sheet.py import   # work/sheet.tsv の訳文を translation/ja.tsv に書き戻す
  python tools/sheet.py key "Exit"   # 英文のキーを表示する

work/sheet.tsv の列: key, english, ja, memo, source
  ja と memo だけを編集する。english は編集しても取り込まれない。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = r"D:\Matrix Games\War in the Pacific Admiral's Edition"
EXE = os.path.join(GAME, "War in the Pacific Admiral Edition.exe")
LOG = os.path.join(GAME, "witpae_ja_log.txt")
JA = os.path.join(ROOT, "translation", "ja.tsv")
SHEET = os.path.join(ROOT, "work", "sheet.tsv")

HEADER = "# key\t訳文\tメモ  (UTF-8, タブ区切り。改行は \\n、タブは \\t と書く。key は英文の FNV-1a 64bit)\n"


def fnv1a64(b: bytes) -> int:
    h = 0xCBF29CE484222325
    for c in b:
        h ^= c
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def key_of(b: bytes) -> str:
    return "h:%016x" % fnv1a64(b)


def escape(b: bytes) -> str:
    out = []
    for c in b:
        if c == 0x5C:
            out.append("\\\\")
        elif c == 0x09:
            out.append("\\t")
        elif c == 0x0A:
            out.append("\\n")
        elif c == 0x0D:
            out.append("\\r")
        elif c < 0x20 or c >= 0x7F:
            out.append("\\x%02X" % c)
        else:
            out.append(chr(c))
    return "".join(out)


def unescape(s: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(s):
        ch = s[i]
        if ch == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "t":
                out += b"\t"; i += 2; continue
            if n == "n":
                out += b"\n"; i += 2; continue
            if n == "r":
                out += b"\r"; i += 2; continue
            if n == "\\":
                out += b"\\"; i += 2; continue
            if n == "x" and i + 3 < len(s):
                out.append(int(s[i + 2:i + 4], 16)); i += 4; continue
        out += ch.encode("utf-8")
        i += 1
    return bytes(out)


def read_ja():
    """translation/ja.tsv -> {key: (ja, memo)}（順序を保つ）"""
    d = {}
    if not os.path.exists(JA):
        return d
    with open(JA, encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            col = line.split("\t")
            while len(col) < 3:
                col.append("")
            d[col[0]] = (col[1], col[2])
    return d


def write_ja(d):
    os.makedirs(os.path.dirname(JA), exist_ok=True)
    with open(JA, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)
        for k, (ja, memo) in d.items():
            if ja or memo:
                f.write("%s\t%s\t%s\n" % (k, ja, memo))


def exe_strings():
    data = open(EXE, "rb").read()
    for m in re.finditer(rb"[\x09\x0a\x0d\x20-\x7e]{2,}(?=\x00)", data):
        b = m.group()
        if re.search(rb"[A-Za-z]", b):
            yield b


def log_strings():
    if not os.path.exists(LOG):
        return
    with open(LOG, "rb") as f:
        for line in f:
            line = line.rstrip(b"\r\n")
            if len(line) > 2 and line[1:2] == b"\t" and line[:1] in b"TEDM":
                yield unescape(line[2:].decode("latin-1"))


def export():
    ja = read_ja()
    rows = {}
    for src, it in (("log", log_strings()), ("exe", exe_strings())):
        for b in it:
            k = key_of(b)
            if k in rows:
                if src not in rows[k][1]:
                    rows[k][1].append(src)
                continue
            rows[k] = (b, [src])
    os.makedirs(os.path.dirname(SHEET), exist_ok=True)
    missing = [k for k in ja if k not in rows]
    with open(SHEET, "w", encoding="utf-8", newline="\n") as f:
        f.write("key\tenglish\tja\tmemo\tsource\n")
        for k, (b, srcs) in rows.items():
            t, memo = ja.get(k, ("", ""))
            f.write("%s\t%s\t%s\t%s\t%s\n" % (k, escape(b), t, memo, "+".join(srcs)))
        for k in missing:  # 英文が見つからない訳（exe の版違いなど）
            t, memo = ja[k]
            f.write("%s\t%s\t%s\t%s\t%s\n" % (k, "", t, memo, "not-found"))
    done = sum(1 for k in rows if ja.get(k, ("", ""))[0])
    print("wrote %s: %d rows, %d translated, %d not found" % (SHEET, len(rows), done, len(missing)))


def do_import():
    ja = read_ja()
    n = 0
    with open(SHEET, encoding="utf-8") as f:
        next(f)
        for line in f:
            col = line.rstrip("\r\n").split("\t")
            while len(col) < 5:
                col.append("")
            k, _, t, memo = col[0], col[1], col[2], col[3]
            if t or memo:
                if ja.get(k) != (t, memo):
                    n += 1
                ja[k] = (t, memo)
            elif k in ja:
                del ja[k]
                n += 1
    write_ja(ja)
    print("updated %s: %d changes, %d entries" % (JA, n, len(ja)))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "export":
        export()
    elif cmd == "import":
        do_import()
    elif cmd == "key":
        print(key_of(sys.argv[2].encode("latin-1")))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
