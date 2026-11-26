# War in the Pacific: Admiral's Edition 日本語化（非公式）

Matrix Games の『War in the Pacific: Admiral's Edition』を日本語で遊ぶための、非公式のファンメイド日本語化です。

- 画面の文言、戦闘報告、シナリオの説明文、艦船・艦型・航空機・地名・部隊・人物の名前を日本語にします。
- ゲームの元ファイルは書き換えません。ゲームのフォルダにファイルを置くだけで日本語になり、外せば英語に戻ります。
- 遊ぶには、正規に入手したゲーム本体が必要です。このリポジトリと配布物には、原作のプログラム・データ・英文は含まれません。

## 対応する環境

- Windows 10 / 11
- ゲーム本体: `War in the Pacific Admiral Edition.exe`（2016-07-10、5,054,464 バイト）。ほかの版では動きません。DLL は差し替え先の機械語を確かめ、合わない部分は差し替えずに飛ばしますが、ほかの版での動作は確かめていません。

## 導入

1. 配布用の zip（`witpae-ja-日付.zip`）を、ゲームのフォルダ（`War in the Pacific Admiral Edition.exe` があるフォルダ）に展開する。次のものが置かれます。
   - `ddraw.dll`（差し替え DLL）
   - `witpae_ja.ini`（設定）
   - `witpae_ja\`（訳文）
2. いつもどおりゲームを起動する。

### おすすめの起動方法

フルスクリーンで Alt+Tab から戻ると、画面が乱れることがあります（このゲームにもともとある問題です）。ウィンドウモードで起動するのがおすすめです。

- ゲームの exe のショートカットを作り、リンク先の末尾に ` -w -skipVideo` を付ける。
- `-w` はウィンドウモード、`-skipVideo` は起動時の動画を飛ばす（ウィンドウを拡大していると動画が黒く映るため）。
- ウィンドウモードでは、DLL がウィンドウを画面に収まる大きさまで拡大します（`witpae_ja.ini` の `[Display]`）。

## 取り外し

ゲームのフォルダから、次のものを削除すると英語版に戻ります。

- `ddraw.dll`
- `witpae_ja.ini`
- `witpae_ja_log.txt`
- `witpae_ja\` フォルダ

## 設定（`witpae_ja.ini`）

| 項目 | 内容 |
| --- | --- |
| `[Text] Translate` | 0 で訳文に置き換えない（英語のまま） |
| `[Text] ScenWidth` | シナリオ説明の1行の幅（ピクセル） |
| `[Display] Scale` | ウィンドウモードの拡大率。`auto`（画面に収まる最大）、`0`（拡大しない）、`1.5` などの数値 |
| `[Display] Borderless` | 1 で枠なしの全画面ウィンドウ |
| `[Display] Smooth` | 1 でなめらかに拡大（0 は粗いが軽い） |
| `[Display] DpiAware` | 1 で Windows の表示スケールによる拡大を止める（画面が切れる対策） |
| `[JpFont] HeightAdjust`、`YOffset` | 日本語の文字の大きさと上下の位置の補正 |
| `[Font] Face` | 日本語の描画に使うフォント（既定は MS ゴシック） |
| `[Debug] LogText` | 1 で、描かれた文字列を `witpae_ja_log.txt` に記録する（訳の作業用） |

## 既知の問題と、英語のまま残るもの

- 兵器の名前（`100 kg GP Bomb`、`Type 95 Tankette` など）は英語のままです。
- 名前の欄（19 バイト）に収まらない長い名前（`F4F-4 Wildcat` など）は英語のままです。
- 画像の中の文字（メインメニューのボタン、`COMBAT SUMMARY` など）は英語のままです。
- 人物の名前は「姓の訳 + 頭文字」（例: 南雲 C.、ハルゼー W.F.）です。日本人の姓の漢字の多くは、ローマ字からの推測です。
- 艦と地名で同じ綴りの名前（Sendai など）は、ゲームの中で艦のデータから表示される場合だけ艦の訳（川内）になります。
- 枠の幅は英語に合わせてあるため、訳文がはみ出す場所があるかもしれません。

## 仕組み

差し替えの `ddraw.dll` は、本物の `ddraw.dll` に処理を引き継ぎながら、ゲームの中の関数をメモリ上で差し替えます（exe のファイルは書き換えません）。

- 文字の描画と幅の計算: ゲームは英数字だけの自前の文字画像で文字を描くので、日本語の部分は GDI（日本語フォント）で描きます。折り返しも日本語に合わせます。
- 文の組み立て（sprintf など）: 書式の文を訳文に、`%s` に入る文字列（名前など）を対訳表の訳に差し替えます。
- シナリオの説明文: `SCEN\*.txt` を開くときに、訳文のファイルに差し替えます。
- ウィンドウモードの拡大: 画面の写しを持ち、自前で拡大して描きます。

詳しい解析のメモは [PLAN.md](PLAN.md) にあります。

## 訳を直す・翻訳に参加する

訳文ファイルには原作の英語を入れていません（キーは英文のハッシュ値です）。手元のゲームから英語と訳を並べた表を作って直し、訳だけを書き戻します。手順は [translation/HOWTO.md](translation/HOWTO.md)、表記の決まりは [translation/STYLE.md](translation/STYLE.md)。

| パス | 中身 |
| --- | --- |
| `translation/ja.tsv` | 画面の文言の訳 |
| `translation/names/` | 名前の訳（艦船、艦型、航空機、地名、航空隊、地上部隊、指揮官、パイロット） |
| `translation/surnames.tsv` | 人物の姓の訳（人物の名前を組み立てる資料。ゲームには置かない） |
| `translation/scen/` | シナリオの説明文の訳 |
| `translation/unit_types.tsv` | 地上部隊の種類（名前のつなぎ方に使う） |
| `src/ddraw/` | 差し替え DLL のソース（C++） |
| `tools/` | 作業用の表の作成・取り込み、配置、配布物の作成 |

## ビルドと配布物の作成

[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) の i686 向けコンパイラ（`D:\Tools\llvm-mingw` に置く想定。違う場所なら `src/ddraw/build.sh` を直す）と Python 3 を使います。

```sh
sh src/ddraw/build.sh          # src/ddraw/build/ddraw.dll を作る
sh tools/deploy.sh             # ゲームのフォルダに配置する（開発用）
python tools/package.py        # dist/witpae-ja-日付.zip を作る
```

## ライセンス

ソースコードと訳文は [MIT ライセンス](LICENSE) です。

『War in the Pacific: Admiral's Edition』の著作権は、その権利者（Matrix Games / 2 by 3 Games）にあります。この日本語化は非公式のもので、権利者とは関係ありません。訳文は原作の二次的な著作物にあたるため、権利者から求めがあれば公開を取りやめるなど、求めに応じます。

## 謝辞

考え方は [Ultima 日本語化](https://higesho.github.io/ultima-ja/) を参考にしました。
