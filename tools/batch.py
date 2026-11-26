"""画面の文言を、翻訳しやすい大きさのバッチに分ける・訳を取り込む。

バッチは exe の中での並び順 (同じ画面の文が近くに並ぶ) にする。work/ の中だけで使い、Git には入れない。

使い方:
  python tools/batch.py make [件数]   # 訳していない文を work/batches/batch_NNN.tsv に分ける (既定 350 件)
  python tools/batch.py verify FILE   # 訳のファイル1つを検査する
  python tools/batch.py merge         # work/batches/batch_NNN.out.tsv の訳を translation/ja.tsv に取り込む
  python tools/batch.py check         # 取り込んだ訳の書式指定などを英文と突き合わせる

バッチの列: key, english, kind
  key は英文 (id の場合は末尾の %番号@ を外した英文) のハッシュ。
訳のファイル (batch_NNN.out.tsv) の列: key<TAB>訳文<TAB>メモ  (UTF-8、エスケープは英文と同じ)
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sheet  # noqa: E402

BATCHES = os.path.join(sheet.ROOT, "work", "batches")
ID_RE = re.compile(rb"%\d+@$")
FMT_RE = re.compile(rb"%[-+ #0]*(\d+|\*)?(\.(\d+|\*))?(hh|h|ll|l|L|I64)?[diouxXeEfgGcspn%]")


def load_rows():
    """作業用シートから、訳す対象の (key, 英文バイト列, kind) を exe の中の並び順で返す"""
    refs = {}
    path = os.path.join(sheet.ROOT, "work", "refs.tsv")
    with open(path, encoding="utf-8") as f:
        for line in f:
            col = line.rstrip("\n").split("\t")
            refs[col[0]] = int(col[3], 16)
    ja = sheet.read_ja()
    rows = {}
    with open(sheet.SHEET, encoding="utf-8") as f:
        head = next(f).rstrip("\n").split("\t")
        ik, ie, ikind, isrc = head.index("key"), head.index("english"), head.index("kind"), head.index("source")
        for line in f:
            col = line.rstrip("\n").split("\t")
            kind = col[ikind]
            if kind not in ("seen", "text", "id"):
                continue
            b = sheet.unescape(col[ie])
            src = set(col[isrc].split("+"))
            # ログの描いた行 (L) だけで、数字を含むものは組み立て済みの文なので訳さない
            if kind == "seen" and src <= {"L", "T"} and re.search(rb"\d", b):
                continue
            addr = refs.get(col[ik], 1 << 40)
            if kind == "id":
                b = ID_RE.sub(b"", b)
            if not b.strip() or not re.search(rb"[A-Za-z]{2}", b):
                continue
            k = sheet.key_of(b)
            if ja.get(k, ("", ""))[0]:
                continue
            if k not in rows or addr < rows[k][0]:
                rows[k] = (addr, b, kind)
    return sorted(((a, k, b, kind) for k, (a, b, kind) in rows.items()), key=lambda x: (x[0], x[2]))


def make(size):
    os.makedirs(BATCHES, exist_ok=True)
    for p in glob.glob(os.path.join(BATCHES, "batch_*.tsv")):
        if not p.endswith(".out.tsv"):
            os.remove(p)
    rows = load_rows()
    n = 0
    for i in range(0, len(rows), size):
        n += 1
        with open(os.path.join(BATCHES, "batch_%03d.tsv" % n), "w", encoding="utf-8", newline="\n") as f:
            f.write("key\tenglish\tkind\n")
            for a, k, b, kind in rows[i:i + size]:
                f.write("%s\t%s\t%s\n" % (k, sheet.escape(b), kind))
    print("%d rows -> %d batches in %s" % (len(rows), n, BATCHES))


def english_map():
    m = {}
    for p in glob.glob(os.path.join(BATCHES, "batch_*.tsv")):
        if p.endswith(".out.tsv"):
            continue
        with open(p, encoding="utf-8") as f:
            next(f)
            for line in f:
                col = line.rstrip("\n").split("\t")
                m[col[0]] = sheet.unescape(col[1])
    return m


def problems(en, ja_text):
    """書式指定・改行・~ が英文と合っているか。問題の説明の一覧を返す"""
    out = []
    jb = sheet.unescape(ja_text)
    if [m.group() for m in FMT_RE.finditer(en)] != [m.group() for m in FMT_RE.finditer(jb)]:
        out.append("書式指定が違う")
    for ch, name in ((b"\n", "改行"), (b"~", "~")):
        if en.count(ch) != jb.count(ch):
            out.append(name + "の数が違う")
    try:
        ja_text.encode("cp932")
    except UnicodeEncodeError as e:
        out.append("CP932 にない文字: %r" % ja_text[e.start:e.end])
    return out


def merge():
    en = english_map()
    ja = sheet.read_ja()
    n = bad = 0
    for p in sorted(glob.glob(os.path.join(BATCHES, "batch_*.out.tsv"))):
        with open(p, encoding="utf-8-sig") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line or line.startswith("#") or line.startswith("key\t"):
                    continue
                col = (line.split("\t") + ["", ""])[:3]
                k, t, memo = col
                if not t or k not in en:
                    continue
                pr = problems(en[k], t)
                if pr:
                    bad += 1
                    print("SKIP %s %s: %s | %s" % (os.path.basename(p), k, "; ".join(pr), t))
                    continue
                ja[k] = (t, memo)
                n += 1
    sheet.write_ja(ja)
    print("merged %d, skipped %d, total %d" % (n, bad, len(ja)))


def check():
    en = english_map()
    ja = sheet.read_ja()
    bad = 0
    for k, (t, memo) in ja.items():
        if k in en and t:
            pr = problems(en[k], t)
            if pr:
                bad += 1
                print("%s: %s | %s | %s" % (k, "; ".join(pr), sheet.escape(en[k]), t))
    print("problems: %d" % bad)


def verify(path):
    """訳のファイル1つを検査する (取り込みはしない)"""
    en = english_map()
    n = bad = 0
    with open(path, encoding="utf-8-sig") as f:
        for no, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if not line or line.startswith("#") or line.startswith("key\t"):
                continue
            col = (line.split("\t") + ["", ""])[:3]
            k, t = col[0], col[1]
            if k not in en:
                bad += 1
                print("line %d: 知らないキー %s" % (no, k))
                continue
            if not t:
                continue
            n += 1
            pr = problems(en[k], t)
            if pr:
                bad += 1
                print("line %d: %s | %s | %s" % (no, "; ".join(pr), sheet.escape(en[k]), t))
    print("translated %d, problems %d" % (n, bad))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "make":
        make(int(sys.argv[2]) if len(sys.argv) > 2 else 350)
    elif cmd == "verify":
        verify(sys.argv[2])
    elif cmd == "merge":
        merge()
    elif cmd == "check":
        check()
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
