"""シナリオ説明の txt (ゲームの SCEN フォルダ) をファイルごと訳す。

ゲームが SCEN\\xxx.txt を開くとき、DLL は witpae_ja\\SCEN\\xxx.txt (CP932) があればそちらを開く。
訳文は translation/scen/<ファイル名>.txt (UTF-8、日本語だけ) に置く。原文はリポジトリに入れない。
中身がまったく同じ原文のファイルは1つにまとめ、代表のファイル名 (名前順で最初) で1つだけ訳す。

使い方:
  python tools/scen.py export         # 代表の原文を work/scen/en/ に写し、まとめ方を work/scen/groups.tsv に書く
  python tools/scen.py check [名前..]  # 訳文を検査する (CP932、1行の長さ、大きさ、~ の有無)
  python tools/scen.py build [出力先]  # 全ファイルの訳を CP932・CRLF で書き出す (既定: ゲームの witpae_ja\\SCEN)
  python tools/scen.py status         # 訳のある/ないファイルの一覧

訳文の決まり (translation/STYLE.md の 4 章「シナリオの説明文」も参照):
  - 1行は 120 バイト (CP932) 以内。段落の途中で改行してよい。ゲームが枠の幅で折り返す。
  - 空行・見出し・日付の行など、段落の区切りは原文に合わせる。
  - ~ (半角チルダ) と全角の ～ は使わない。
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sheet  # noqa: E402

SCEN = os.path.join(sheet.GAME, "SCEN")
TRANS = os.path.join(sheet.ROOT, "translation", "scen")
WORK = os.path.join(sheet.ROOT, "work", "scen")
OUT_DEFAULT = os.path.join(sheet.GAME, "witpae_ja", "SCEN")
MAX_LINE = 120     # 1行のバイト数の上限 (原文の最長は 135 バイト)
MAX_RATIO = 1.5    # 原文に対する大きさの上限 (警告)
SKIP = {"answer.txt", "guadalcanal 004 faq.txt"}  # 説明文ではないもの (ゲームの画面には出ない)


def game_files():
    """SCEN の txt (説明文) の名前の一覧"""
    return sorted(n for n in os.listdir(SCEN)
                  if n.lower().endswith(".txt") and n.lower() not in SKIP)


def groups():
    """{代表の名前: [同じ中身のファイル名...]} と {ファイル名: 代表の名前}"""
    by_hash = {}
    for n in game_files():
        with open(os.path.join(SCEN, n), "rb") as f:
            h = hashlib.sha1(f.read()).hexdigest()
        by_hash.setdefault(h, []).append(n)
    rep = {}
    canon = {}
    for names in by_hash.values():
        names.sort(key=str.lower)
        rep[names[0]] = names
        for n in names:
            canon[n] = names[0]
    return rep, canon


def trans_path(name):
    return os.path.join(TRANS, name)


def find_trans(name, canon):
    """name の訳文のパス (自分の名前の訳、なければ代表の訳)。大文字小文字は区別しない"""
    if not os.path.isdir(TRANS):
        return None
    have = {n.lower(): n for n in os.listdir(TRANS)}
    for cand in (name, canon.get(name, name)):
        if cand.lower() in have:
            return os.path.join(TRANS, have[cand.lower()])
    return None


def export():
    rep, _ = groups()
    os.makedirs(os.path.join(WORK, "en"), exist_ok=True)
    with open(os.path.join(WORK, "groups.tsv"), "w", encoding="utf-8") as g:
        g.write("代表\t同じ中身のファイル\tバイト数\n")
        for r in sorted(rep, key=str.lower):
            with open(os.path.join(SCEN, r), "rb") as f:
                data = f.read()
            text = data.decode("cp1252", errors="replace").replace("\r\n", "\n")
            with open(os.path.join(WORK, "en", r), "w", encoding="utf-8", newline="\n") as o:
                o.write(text)
            g.write(f"{r}\t{' '.join(rep[r])}\t{len(data)}\n")
    print(f"{len(rep)} files -> {os.path.join(WORK, 'en')}")


def check_one(path, orig_size):
    """問題の一覧を返す"""
    probs = []
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if text.startswith("﻿"):
        probs.append("BOM がある")
        text = text[1:]
    try:
        data = text.replace("\r\n", "\n").replace("\n", "\r\n").encode("cp932")
    except UnicodeEncodeError as e:
        return [f"CP932 にない文字: {text[e.start:e.end]!r}"]
    for i, line in enumerate(text.split("\n"), 1):
        b = line.encode("cp932")
        if len(b) > MAX_LINE:
            probs.append(f"{i}行目が長い ({len(b)} バイト)")
        if "~" in line or "～" in line:
            probs.append(f"{i}行目に ~ がある")
    if orig_size and len(data) > orig_size * MAX_RATIO:
        probs.append(f"原文の {len(data) / orig_size:.2f} 倍 ({len(data)} / {orig_size} バイト)")
    return probs


def check(names):
    rep, canon = groups()
    targets = names or sorted(rep, key=str.lower)
    bad = 0
    for n in targets:
        p = find_trans(n, canon)
        if not p:
            if names:
                print(f"{n}: 訳がない")
                bad += 1
            continue
        size = os.path.getsize(os.path.join(SCEN, canon.get(n, n)))
        probs = check_one(p, size)
        for msg in probs:
            print(f"{n}: {msg}")
        bad += bool(probs)
    print("OK" if not bad else f"{bad} files with problems")
    return bad


def build(out):
    rep, canon = groups()
    os.makedirs(out, exist_ok=True)
    done = missing = 0
    for n in game_files():
        p = find_trans(n, canon)
        dst = os.path.join(out, n)
        if not p:
            missing += 1
            if os.path.exists(dst):
                os.remove(dst)  # 古い訳が残らないように
            continue
        with open(p, encoding="utf-8-sig") as f:
            text = f.read().replace("\r\n", "\n")
        with open(dst, "wb") as o:
            o.write(text.replace("\n", "\r\n").encode("cp932"))
        done += 1
    print(f"built {done} files, {missing} without translation -> {out}")


def status():
    rep, canon = groups()
    for r in sorted(rep, key=str.lower):
        mark = "o" if find_trans(r, canon) else "-"
        print(f"{mark} {r}\t({len(rep[r])} files)")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "export":
        export()
    elif cmd == "check":
        sys.exit(1 if check(sys.argv[2:]) else 0)
    elif cmd == "build":
        build(sys.argv[2] if len(sys.argv) > 2 else OUT_DEFAULT)
    elif cmd == "status":
        status()
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
