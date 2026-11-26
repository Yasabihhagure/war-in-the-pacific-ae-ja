"""exe の中の文字列のうち、コードやデータの表から実際に参照されているものを調べる。

参照されている文字列だけが、ゲームが使う本物の文字列。偶然文字に見えるバイト列 (ゴミ) を除ける。
結果は work/refs.tsv (Git 管理外) に「キー<TAB>参照の数<TAB>最初の参照元」で書く。英文は書かない。

使い方: python tools/refs.py
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sheet  # noqa: E402

BASE = 0x400000


def main():
    d = open(sheet.EXE, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    so = opt + struct.unpack_from("<H", d, pe + 20)[0]
    secs = []
    for i in range(struct.unpack_from("<H", d, pe + 6)[0]):
        name, vs, va, rs, ra = struct.unpack_from("<8sIIII", d, so + i * 40)
        secs.append((name.rstrip(b"\0").decode(), BASE + va, ra, rs))
    text = [s for s in secs if s[0] == ".text"][0]
    data_secs = [s for s in secs if s[0] in (".rdata", ".data")]

    # 文字列の先頭アドレス -> バイト列
    strings = {}
    for name, va, ra, rs in data_secs:
        blob = d[ra:ra + rs]
        for m in re.finditer(rb"[\x09\x0a\x0d\x20-\x7e]{2,}(?=\x00)", blob):
            strings[va + m.start()] = m.group()

    refs = {}

    def add(addr, where):
        if addr in strings:
            refs.setdefault(addr, []).append(where)

    # コード中の 4 バイトの即値 (push imm32, mov reg/mem, imm32 など) を全部見る
    tname, tva, tra, trs = text
    code = d[tra:tra + trs]
    lo = min(s[1] for s in data_secs)
    hi = max(s[1] + s[3] for s in data_secs)
    for off in range(0, len(code) - 3):
        v = struct.unpack_from("<I", code, off)[0]
        if lo <= v < hi:
            add(v, "code:%06x" % (tva + off))
    # データ中のポインタの表
    for name, va, ra, rs in data_secs:
        blob = d[ra:ra + rs]
        for off in range(0, len(blob) - 3, 4):
            v = struct.unpack_from("<I", blob, off)[0]
            if lo <= v < hi:
                add(v, "data:%06x" % (va + off))

    os.makedirs(os.path.dirname(sheet.SHEET), exist_ok=True)
    out = os.path.join(os.path.dirname(sheet.SHEET), "refs.tsv")
    n = 0
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for addr, where in sorted(refs.items()):
            b = strings[addr]
            f.write("%s\t%d\t%s\t%06x\n" % (sheet.key_of(b), len(where), where[0], addr))
            n += 1
    print("referenced strings: %d / %d -> %s" % (n, len(strings), out))


if __name__ == "__main__":
    main()
