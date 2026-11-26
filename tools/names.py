"""名前 (艦船・航空機・地上部隊・地名・人物など) の対訳表を作る・取り込む。

名前はシナリオのデータ SCEN\\wp?NNN.dat に、20 バイトの欄 (19 文字 + 終端) で入っている。
全シナリオから名前を集め、種類ごとの対訳表にする。

公開方針 (PLAN.md 1.1) に合わせて、対訳表は2層にする。
  translation/names/<種類>.tsv  公開する。「キー<TAB>日本語<TAB>メモ」。キーは英語名の FNV-1a 64bit
  work/names/<種類>.tsv         編集用 (Git 管理外)。「キー<TAB>英語<TAB>日本語<TAB>メモ<TAB>件数」

使い方:
  python tools/names.py export   # 編集用の対訳表を作る (今の日本語を入れて)
  python tools/names.py import   # 編集用の対訳表の日本語とメモを、公開する対訳表に書き戻す
  python tools/names.py batch 種類 [件数]  # 訳していない名前を work/names/batches/<種類>_NNN.tsv に分ける (既定 400)
  python tools/names.py verify FILE        # 訳のファイル <種類>_NNN.out.tsv (key<TAB>日本語<TAB>メモ) を検査する
  python tools/names.py merge              # 訳のファイルを編集用の対訳表に入れ、公開する対訳表に書き戻す
  python tools/names.py rules              # 番号つきの日本の艦艇 (伊58、第5号駆潜艇など) を規則で訳す
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sheet  # noqa: E402

SCEN = os.path.join(sheet.GAME, "SCEN")
PUB = os.path.join(sheet.ROOT, "translation", "names")
WORK = os.path.join(sheet.ROOT, "work", "names")

# 種類 -> (ファイルの頭文字, 説明)
# wpl には拠点の地名と地上部隊・司令部が一緒に入っているので、UNIT_RE で振り分ける
KINDS = {
    "ships":        ("wps", "艦船の名前"),
    "ship_classes": ("wpc", "艦型の名前"),
    "aircraft":     ("wpa", "航空機の型式"),
    "air_groups":   ("wpg", "航空隊の名前"),
    "ground_units": ("wpl", "地上部隊・司令部の名前"),
    "places":       ("wpl", "拠点・地名"),
    "leaders":      ("wph", "指揮官などの人物"),
    "pilots":       ("wpp", "パイロット"),
}

# 地上部隊・司令部らしい名前 (数字を含む、部隊を表す語を含む、すべて大文字の見出し)
UNIT_RE = re.compile(
    rb"\d|\b(Div|Division|Bde|Brigade|Regt|Regiment|Bn|Battalion|Co|Company|Corps|Army|Area|Fleet|"
    rb"HQ|HQs|Group|Grp|Det|Detachment|Force|Command|Cmd|Base|Unit|Squadron|Sqn|Sentai|Ku|Tank|Cav|"
    rb"Inf|Infantry|Artillery|Arty|Gun|Guns|Cannon|Engineer|Eng|Const|Construction|Naval|Marine|Marines|"
    rb"Mtn|Mountain|AA|AAA|Fld|Field|Garrison|Gar|Defense|Defence|Combat|Temp|Ind|Indep|SNLF|Rikusentai|"
    rb"Coastal|CD|Support|Fortress|Militia|Police|Bty|Battery|Rifles|Light|Heavy|Mixed|Special|Signal|Deploy|Invasion|Coy|Depot|Convoy|Reinforcements|RCAF|RAAF|RAF|USN|USMC|IJA|IJN|Type)\b"
    rb"|^[A-Z0-9 &'.-]+$")

SLOT = 20
NAME_RE = re.compile(rb"[A-Za-z0-9'\"(.][\x20-\x7e]*")


def names_in(path):
    """20 バイトの欄に並んだ名前を返す。名前の並びが 3000 欄以上途切れたら終わり"""
    d = open(path, "rb").read()
    out = []
    gap = 0
    for i in range(len(d) // SLOT):
        s = d[i * SLOT:(i + 1) * SLOT]
        z = s.find(b"\0")
        if z > 0 and NAME_RE.fullmatch(s[:z]) and not s[z:].strip(b"\0"):
            out.append(s[:z])
            gap = 0
        else:
            gap += 1
            if gap > 3000 and out:
                break
    return out


def read_pub(kind):
    d = {}
    path = os.path.join(PUB, kind + ".tsv")
    if os.path.exists(path):
        with open(path, encoding="utf-8-sig") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line or line.startswith("#"):
                    continue
                col = (line.split("\t") + ["", ""])[:3]
                d[col[0]] = (col[1], col[2])
    return d


def write_pub(kind, d):
    os.makedirs(PUB, exist_ok=True)
    with open(os.path.join(PUB, kind + ".tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# %s の対訳。key\\t日本語\\tメモ。key は英語名の FNV-1a 64bit (tools/names.py)\n" % KINDS[kind][1])
        for k, (ja, memo) in sorted(d.items(), key=lambda x: x[0]):
            if ja or memo:
                f.write("%s\t%s\t%s\n" % (k, ja, memo))


def export():
    os.makedirs(WORK, exist_ok=True)
    for kind, (prefix, desc) in KINDS.items():
        counts = {}
        files = sorted(glob.glob(os.path.join(SCEN, prefix + "*.dat")))
        for path in files:
            for b in names_in(path):
                if prefix == "wpl" and (kind == "places") == bool(UNIT_RE.search(b)):
                    continue
                counts[b] = counts.get(b, 0) + 1
        pub = read_pub(kind)
        keys = set()
        with open(os.path.join(WORK, kind + ".tsv"), "w", encoding="utf-8", newline="\n") as f:
            f.write("key\tenglish\tja\tmemo\tcount\n")
            for b in sorted(counts, key=lambda x: x.lower()):
                k = sheet.key_of(b)
                keys.add(k)
                ja, memo = pub.get(k, ("", ""))
                f.write("%s\t%s\t%s\t%s\t%d\n" % (k, b.decode("latin-1"), ja, memo, counts[b]))
            for k, (ja, memo) in pub.items():
                if k not in keys:
                    f.write("%s\t\t%s\t%s\t0\n" % (k, ja, memo))
        done = sum(1 for k in keys if pub.get(k, ("", ""))[0])
        print("%-13s %-22s files=%3d names=%6d translated=%d" % (kind, desc, len(files), len(counts), done))


def do_import():
    for kind in KINDS:
        path = os.path.join(WORK, kind + ".tsv")
        if not os.path.exists(path):
            continue
        pub = read_pub(kind)
        with open(path, encoding="utf-8") as f:
            head = next(f).rstrip("\r\n").split("\t")
            ik, ij, im = head.index("key"), head.index("ja"), head.index("memo")
            for line in f:
                col = line.rstrip("\r\n").split("\t")
                col += [""] * (len(head) - len(col))
                k, ja, memo = col[ik], col[ij], col[im]
                if ja or memo:
                    pub[k] = (ja, memo)
                else:
                    pub.pop(k, None)
        write_pub(kind, pub)
        print("%-13s %d entries" % (kind, sum(1 for v in pub.values() if v[0])))


BATCHES = os.path.join(WORK, "batches")

# 番号つきの日本の艦艇を規則で訳す (艦船の名前)。(正規表現, 訳の書式)
SHIP_RULES = [
    (r"I-(\d+)", "伊{}"),               # 伊号潜水艦
    (r"RO-(\d+)", "呂{}"),              # 呂号潜水艦
    (r"Ha-(\d+)", "波{}"),              # 波号潜水艦
    (r"Ch (\d+)", "第{}号駆潜艇"),
    (r"CHa-(\d+)", "第{}号駆潜特務艇"),
    (r"W-(\d+)", "第{}号掃海艇"),
    (r"Wa (\d+)", "第{}号掃海特務艇"),
    (r"T-(\d+)", "第{}号輸送艦"),
    (r"Yu-(\d+)", "ゆ{}"),              # 陸軍の潜航輸送艇
]


_ORD = r"(\d+)(?:st|nd|rd|th)?"
# 航空隊・地上部隊の決まった形。訳の書式の {0} {1} は正規表現の組 (数字は先頭の 0 を落とす)
UNIT_RULES = {
    "air_groups": [
        (_ORD + r" Sentai", "飛行第{0}戦隊"),
        (_ORD + r" Ku ([A-Z]-\d+)", "{0}空 {1}"),
        (_ORD + r" Ku", "{0}空"),
        (_ORD + r" Shinbu-tai", "第{0}振武隊"),
        (_ORD + r" Fighter Group", "第{0}戦闘機群"),
        (_ORD + r" Bomber Group", "第{0}爆撃群"),
        (_ORD + r" Troop Carrier Group", "第{0}輸送機群"),
        (_ORD + r" Photo Group", "第{0}写真偵察群"),
        (_ORD + r" Night Fighter Group", "第{0}夜戦群"),
        (_ORD + r" BG", "第{0}爆撃群"),
        (_ORD + r" FG", "第{0}戦闘機群"),
        (_ORD + r" BS", "第{0}爆撃飛行隊"),
        (_ORD + r" FS", "第{0}戦闘飛行隊"),
    ],
    "ground_units": [
        (_ORD + r" (?:Division|Div)", "第{0}師団"),
        (_ORD + r" Ind\.? ?Mixed", "独立混成第{0}旅団"),
        (_ORD + r" Garrison(?: Unit)?", "第{0}守備隊"),
    ],
}
# 英語のまま残す形 (略記や見出し)。バッチに入れない
KEEP_RE = {
    "air_groups": re.compile(
        _ORD + r" [A-Z]{2,5}/" + _ORD + r" [A-Z]{2,5}( [A-Z]{2,4})?"   # 93rd BG/329th BS
        r"|\d?/?V[A-Z]{1,4}-\d+[A-Z]?(/\d)?"                          # VF-3, 1/VCS-13
        r"|\d+ [A-Za-z]+-?[A-Za-z]*/\d+ [A-Za-z]+"                    # 316 ShAD/336 ShAP
        r"|[A-Z0-9 ./*-]+"),                                          # 見出し
    "ground_units": re.compile(r"[A-Z0-9 ./*-]+"),
}


def _num(s):
    return str(int(s)) if s.isdigit() else s


def apply_rules():
    """SHIP_RULES / UNIT_RULES に合う名前に訳を入れる (訳が空のものだけ)"""
    rules = {"ships": [(p, f.replace("{}", "{0}")) for p, f in SHIP_RULES]}
    rules.update(UNIT_RULES)
    for kind, rl in rules.items():
        head, rows = read_work(kind)
        ie, ij = head.index("english"), head.index("ja")
        n = 0
        for r in rows:
            if r[ij]:
                continue
            e = r[ie].strip()
            for pat, fmt in rl:
                m = re.fullmatch(pat, e)
                if m:
                    ja = fmt.format(*[_num(g) for g in m.groups()])
                    if len(ja.encode("cp932")) <= 19:
                        r[ij] = ja
                        n += 1
                    break
        write_work(kind, head, rows)
        print("%s: %d names by rules" % (kind, n))
    do_import()


def read_work(kind):
    """編集用の対訳表 -> (見出し, 行のリスト)"""
    with open(os.path.join(WORK, kind + ".tsv"), encoding="utf-8") as f:
        head = next(f).rstrip("\r\n").split("\t")
        rows = []
        for line in f:
            col = line.rstrip("\r\n").split("\t")
            rows.append(col + [""] * (len(head) - len(col)))
    return head, rows


def write_work(kind, head, rows):
    with open(os.path.join(WORK, kind + ".tsv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(head) + "\n")
        for r in rows:
            f.write("\t".join(r) + "\n")


def make_batches(kind, size):
    """訳していない名前を work/names/batches/<種類>_NNN.tsv (key, english, count) に分ける"""
    head, rows = read_work(kind)
    ik, ie, ij, ic = head.index("key"), head.index("english"), head.index("ja"), head.index("count")
    todo = [r for r in rows if r[ie] and not r[ij]]
    os.makedirs(BATCHES, exist_ok=True)
    n = 0
    for i in range(0, len(todo), size):
        n += 1
        with open(os.path.join(BATCHES, "%s_%03d.tsv" % (kind, n)), "w", encoding="utf-8", newline="\n") as f:
            f.write("key\tenglish\tcount\n")
            for r in todo[i:i + size]:
                f.write("%s\t%s\t%s\n" % (r[ik], r[ie], r[ic]))
    print("%s: %d names -> %d batches" % (kind, len(todo), n))


def check_out(path, english):
    """訳のファイル (key<TAB>日本語<TAB>メモ) を検査し、(訳の辞書, 問題の一覧) を返す"""
    out, probs = {}, []
    with open(path, encoding="utf-8-sig") as f:
        for no, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if not line or line.startswith("#") or line.startswith("key\t"):
                continue
            col = (line.split("\t") + ["", ""])[:3]
            k, ja, memo = col
            if k not in english:
                probs.append("%d行目: 知らないキー %s" % (no, k))
                continue
            if not ja:
                continue
            try:
                b = ja.encode("cp932")
            except UnicodeEncodeError:
                probs.append("%d行目: CP932 にない文字 %r" % (no, ja))
                continue
            if len(b) > 19:
                probs.append("%d行目: 19 バイトを超える (%d) %s" % (no, len(b), ja))
            if "~" in ja or "～" in ja or "%" in ja:
                probs.append("%d行目: ~ や %% がある %s" % (no, ja))
            out[k] = (ja, memo)
    return out, probs


def merge_batches():
    """work/names/batches/<種類>_NNN.out.tsv の訳を編集用の対訳表に入れ、公開する対訳表に書き戻す"""
    for kind in KINDS:
        files = sorted(glob.glob(os.path.join(BATCHES, kind + "_[0-9][0-9][0-9].out.tsv")))
        if not files:
            continue
        head, rows = read_work(kind)
        ik, ie, ij, im = head.index("key"), head.index("english"), head.index("ja"), head.index("memo")
        english = {r[ik]: r[ie] for r in rows}
        got = {}
        for p in files:
            out, probs = check_out(p, english)
            for msg in probs:
                print("%s: %s" % (os.path.basename(p), msg))
            got.update(out)
        n = 0
        for r in rows:
            if r[ik] in got and not r[ij]:
                r[ij], memo = got[r[ik]]
                if memo and not r[im]:
                    r[im] = memo
                n += 1
        write_work(kind, head, rows)
        print("%s: merged %d from %d files" % (kind, n, len(files)))
    do_import()


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # 日本語を含む結果の表示で止まらないように
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "export":
        export()
    elif cmd == "import":
        do_import()
    elif cmd == "batch":
        make_batches(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 400)
    elif cmd == "verify":
        kind = os.path.basename(sys.argv[2]).rsplit("_", 1)[0]
        head, rows = read_work(kind)
        english = {r[head.index("key")]: r[head.index("english")] for r in rows}
        out, probs = check_out(sys.argv[2], english)
        for msg in probs:
            print(msg)
        print("%d translations, %s" % (len(out), "OK" if not probs else "%d problems" % len(probs)))
        sys.exit(1 if probs else 0)
    elif cmd == "merge":
        merge_batches()
    elif cmd == "rules":
        apply_rules()
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
