"""翻訳作業用シートの作成と取り込み。

原作の英文はリポジトリに入れない。公開する translation/ja.tsv には
「英文のハッシュ」と「訳文」だけを書く。英文と訳文を並べた作業用シート
(work/sheet.tsv、Git 管理外) は、手元にあるゲームから毎回作る。

使い方:
  python tools/sheet.py collect  # ゲームのログを logs/ に貯める (起動ごとに上書きされるため)
  python tools/sheet.py export   # ゲームの exe とログから英文を集め、work/sheet.tsv を作る
  python tools/sheet.py import   # work/sheet.tsv の訳文を translation/ja.tsv に書き戻す
  python tools/sheet.py key "Exit"   # 英文のキーを表示する

work/sheet.tsv の列: key, english, ja, memo, kind, source
  ja と memo だけを編集する。english などは編集しても取り込まれない。
  kind: seen=ゲーム中に表示された / text=文らしい / id=末尾に %番号@ がつく文 (番号を外した文で訳す)
        name=末尾に %番号@ がつく人名 (訳さない)
        code=内部の名前 / file=ファイル名 / junk=コードから参照されていない (ゴミ) / ctrl=制御文字・1文字
  source: F=書式のログ, L=描いた行のログ, T/E/D/M=旧形式のログ, exe=exe の文字列
"""
import glob
import os
import re
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# ゲームの場所。環境変数 WITPAE_GAME で変えられる (Windows 形式のパス)
GAME = os.environ.get("WITPAE_GAME") or r"D:\Matrix Games\War in the Pacific Admiral's Edition"
EXE = os.path.join(GAME, "War in the Pacific Admiral Edition.exe")
LOG = os.path.join(GAME, "witpae_ja_log.txt")
JA = os.path.join(ROOT, "translation", "ja.tsv")
SHEET = os.path.join(ROOT, "work", "sheet.tsv")
LOGS = os.path.join(ROOT, "logs")

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


def log_files():
    files = sorted(glob.glob(os.path.join(LOGS, "*.txt")))
    if os.path.exists(LOG):
        files.append(LOG)
    return files


def log_strings():
    """ログから (タグ, バイト列) を返す"""
    for path in log_files():
        with open(path, "rb") as f:
            for line in f:
                line = line.rstrip(b"\r\n")
                if len(line) > 2 and line[1:2] == b"\t" and line[:1] in b"FLTEDM":
                    yield line[:1].decode(), unescape(line[2:].decode("latin-1"))


FILE_RE = re.compile(rb"(?i)\.(bmp|wav|dat|txt|avi|pws|ini|exe|dll|ogg|jpg|png|csv|sfx|cmt|pdf|db)\s*$|\\")
CODE_RE = re.compile(rb"^[A-Za-z_][A-Za-z0-9_]*$")
ID_RE = re.compile(rb"%\d+@$")


def load_refs():
    """tools/refs.py の結果 (work/refs.tsv) から、参照されている文字列のキーを読む"""
    path = os.path.join(os.path.dirname(SHEET), "refs.tsv")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return {line.split("	", 1)[0] for line in f}


def classify(b, tags, referenced=True):
    if any(c < 0x20 and c not in (9, 10, 13) for c in b) or len(b.strip()) <= 1:
        return "ctrl"
    if FILE_RE.search(b):
        return "file"
    if ID_RE.search(b):
        base = ID_RE.sub(b"", b)
        # 人名 (大文字で始まる1語) は訳さない。それ以外は、番号を外した文で訳す (DLL が番号を付け直す)
        return "name" if re.match(rb"^[A-Z][A-Za-z'\-]*$", base) else "id"
    if tags & {"F", "L", "T"}:
        return "seen"
    if CODE_RE.match(b) and (re.search(rb"[a-z][A-Z]", b) or b"_" in b):
        return "code"
    if not referenced:
        return "junk"
    return "text"


def collect():
    if not os.path.exists(LOG):
        print("no log: %s" % LOG)
        return
    os.makedirs(LOGS, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S", time.localtime(os.path.getmtime(LOG)))
    dst = os.path.join(LOGS, "log_%s.txt" % stamp)
    if os.path.exists(dst):
        print("already collected: %s" % dst)
        return
    shutil.copyfile(LOG, dst)
    print("collected: %s" % dst)


ORDER = {"seen": 0, "text": 1, "id": 2, "name": 3, "code": 4, "file": 5, "junk": 6, "ctrl": 7}


def export():
    ja = read_ja()
    rows = {}  # key -> [bytes, set(tags)]
    for tag, b in log_strings():
        rows.setdefault(key_of(b), [b, set()])[1].add(tag)
    for b in exe_strings():
        rows.setdefault(key_of(b), [b, set()])[1].add("exe")
    refs = load_refs()
    if refs is None:
        print("note: work/refs.tsv がないので、参照の有無で分けていない (python tools/refs.py で作る)")
    items = []
    for k, (b, tags) in rows.items():
        items.append((classify(b, tags, refs is None or k in refs), k, b, tags))
    items.sort(key=lambda x: (ORDER[x[0]], x[2].lower()))
    missing = [k for k in ja if k not in rows]
    counts = {}
    os.makedirs(os.path.dirname(SHEET), exist_ok=True)
    with open(SHEET, "w", encoding="utf-8", newline="\n") as f:
        f.write("key\tenglish\tja\tmemo\tkind\tsource\n")
        for kind, k, b, tags in items:
            counts[kind] = counts.get(kind, 0) + 1
            t, memo = ja.get(k, ("", ""))
            f.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (k, escape(b), t, memo, kind, "+".join(sorted(tags))))
        for k in missing:  # 英文が見つからない訳（exe の版違いなど）
            t, memo = ja[k]
            f.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (k, "", t, memo, "?", "not-found"))
    done = sum(1 for k in rows if ja.get(k, ("", ""))[0])
    print("wrote %s: %d rows, %d translated, %d not found" % (SHEET, len(rows), done, len(missing)))
    print("kinds: " + ", ".join("%s=%d" % (k, counts[k]) for k in sorted(counts, key=lambda x: ORDER[x])))


def do_import():
    ja = read_ja()
    n = 0
    with open(SHEET, encoding="utf-8") as f:
        head = next(f).rstrip("\r\n").split("\t")
        ik, it, im = head.index("key"), head.index("ja"), head.index("memo")
        for line in f:
            col = line.rstrip("\r\n").split("\t")
            while len(col) < len(head):
                col.append("")
            k, t, memo = col[ik], col[it], col[im]
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
    if cmd == "collect":
        collect()
    elif cmd == "export":
        export()
    elif cmd == "import":
        do_import()
    elif cmd == "key":
        print(key_of(sys.argv[2].encode("latin-1")))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
