"""名前 (艦船・航空機・地上部隊・地名・人物など) の対訳表を作る・取り込む。

名前はシナリオのデータ SCEN\\wp?NNN.dat に、20 バイトの欄 (19 文字 + 終端) で入っている。
全シナリオから名前を集め、種類ごとの対訳表にする。

公開方針 (PLAN.md 1.1) に合わせて、対訳表は2層にする。
  translation/names/<種類>.tsv  公開する。「キー<TAB>日本語<TAB>メモ」。キーは英語名の FNV-1a 64bit
  work/names/<種類>.tsv         編集用 (Git 管理外)。「キー<TAB>英語<TAB>日本語<TAB>メモ<TAB>件数」

使い方:
  python tools/names.py export   # 編集用の対訳表を作る (今の日本語を入れて)
  python tools/names.py import   # 編集用の対訳表の日本語とメモを、公開する対訳表に書き戻す
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


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "export":
        export()
    elif cmd == "import":
        do_import()
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
