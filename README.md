# War in the Pacific: Admiral's Edition 日本語化（非公式）

Matrix Games の『War in the Pacific: Admiral's Edition』を日本語で遊ぶための、非公式のファンメイド日本語化です。**開発中**です。

- ゲームの元ファイルは書き換えません。ゲームのフォルダにファイルを置くだけで日本語になり、外せば英語に戻ります。
- このリポジトリには、原作のプログラム・データ・英文は含まれません。訳文は、英文から計算したハッシュ値をキーにして保存しています。
- 遊ぶには、正規に入手したゲーム本体が必要です。

## 仕組み

差し替えの `ddraw.dll` をゲームのフォルダに置きます。この DLL は本物の `ddraw.dll` に処理を引き継ぎながら、ゲームが文字を描く Windows の関数（`CreateFontA`、`TextOutA` など）を横取りし、次のことをします。

- フォントを日本語フォントに差し替える
- 描かれる英文を、訳文ファイル（`witpae_ja\ja.tsv`）の日本語に置き換える

## フォルダ

| パス | 中身 |
| --- | --- |
| `src/ddraw/` | 差し替え ddraw.dll のソース（C++） |
| `translation/ja.tsv` | 訳文（キー、訳文、メモ） |
| `tools/` | 翻訳作業用シートの作成・取り込み、配置用のスクリプト |
| `witpae_ja.ini` | 設定ファイルのひな形 |

## ビルド

[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) の i686 向けコンパイラを使います。

```sh
sh src/ddraw/build.sh
```

## 翻訳の手順

```sh
python tools/sheet.py export   # 手元のゲームから英文を集め、work/sheet.tsv を作る（Git 管理外）
# work/sheet.tsv の ja 列を編集する
python tools/sheet.py import   # 訳文だけを translation/ja.tsv に書き戻す
```

## 謝辞

考え方は [Ultima 日本語化](https://higesho.github.io/ultima-ja/) を参考にしました。
