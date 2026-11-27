# 訳を直す方法

このリポジトリの訳文ファイルには、原作の英語を入れていません（キーは英文のハッシュです）。訳を直すときは、手元のゲームから「元の英語と訳を並べた表」を作り、それを直してから、訳だけを書き戻します。

## 用意するもの

- War in the Pacific: Admiral's Edition（インストール済みのもの。既定の場所は `D:\Matrix Games\War in the Pacific Admiral's Edition`。違う場所なら、環境変数 `WITPAE_GAME` にゲームのフォルダを入れる。例: Git Bash で `export WITPAE_GAME="C:\Games\War in the Pacific Admiral's Edition"`、PowerShell で `$env:WITPAE_GAME = "C:\Games\..."`）
- Python 3
- 表を編集できるもの（テキストエディタ、または表計算ソフト。保存は UTF-8・タブ区切りのまま）

英語入りの表は `work/` に作られます。`work/` は Git の管理外です。公開する場所（`translation/`）に英語を書かないでください。メモ欄にも書かないでください。

## 1. 画面の文言

1. `python tools/sheet.py export` を実行すると、`work/sheet.tsv` ができる。列は `key`、`english`（元の英語）、`ja`（訳）、`memo` など。
2. `ja` を直す。書式の決まり（`%s`、`%d`、`~`、`\n` を変えない）は [STYLE.md](STYLE.md) を見る。
3. `python tools/sheet.py import` で `translation/ja.tsv` に書き戻す。

## 2. 艦船・艦型・航空機・地名・部隊の名前

1. `python tools/names.py export` を実行すると、`work/names/<種類>.tsv` ができる。種類は ships（艦船）、ship_classes（艦型）、aircraft（航空機）、places（地名）、air_groups（航空隊）、ground_units（地上部隊）、leaders（指揮官）、pilots（パイロット）。列は `key`、`english`、`ja`、`memo`、`count`（出てくるシナリオの数）。
2. `ja` を直す。日本語は Shift_JIS（CP932）で 19 バイト（全角 9 文字）以内。`~` と `%` は使えない。
3. `python tools/names.py import` で `translation/names/<種類>.tsv` に書き戻す。
4. `python tools/names.py aliases` を実行する。末尾に空白のある名前（`Aoba-1 ` など）の訳を、空白を落とした形（`Aoba-1`）のキーにも入れる（メモは「別名: 末尾の空白なし」。元の訳を直したら、これで作り直される）。ゲームは空白つきのまま描くが、空白を落として引く道具でも引けるようにするため。

注意:

- 名前の訳は、画面のすべての文の名前の部分に効きます。一般の単語や「18th」のような序数だけの名前を訳すと、思わぬ文が変わることがあります。
- 地上部隊の名前は「名前の部分」と「種類」（Army、Base Force など）を組み合わせて表示されます。名前の部分の訳に種類を含めないでください（例: `Kure 3rd` は「呉第3」。後ろに「陸戦隊」が付く）。

## 3. 人物の名前（指揮官・パイロット）

人物の名前は、姓ごとの訳から「姓の訳 + 頭文字」（例: 南雲 C.、ハルゼー W.F.）を組み立てています。日本人の姓の漢字は多くがローマ字からの推測です（メモ欄に「推測」とあるもの）。

姓の訳を直すとき:

1. `python tools/names.py export`（人物の表を最新にする）
2. `python tools/names.py surname-export` を実行すると、`work/names/surnames.tsv` ができる。列は `key`、`surname`（元の姓）、`ja`、`memo`、`count`（その姓の人数）、`examples`（実例の名前）。
3. `ja` を直す。確かめた漢字なら、メモの「推測」を消す。
4. `python tools/names.py surname-import` で `translation/surnames.tsv` に書き戻し、人物の表を組み立て直す。

一人だけ別の訳にしたいとき（例: 「Nagumo, Chuichi」を「南雲忠一」にする）:

1. `python tools/names.py export`
2. `work/names/leaders.tsv`（パイロットは `pilots.tsv`）のその人の行の `ja` を直し、`memo` を「姓から」以外（空でもよい）にする。メモが「姓から」の行は、姓の訳から組み立て直すときに上書きされます。
3. `python tools/names.py import`

## 4. シナリオの説明文

`translation/scen/` の txt（UTF-8、日本語だけ）を直接直します。元の英文は、ゲームの `SCEN` フォルダの同じ名前のファイルです。決まりは [STYLE.md](STYLE.md) の 5.1。

## 5. ゲームに反映する

`sh tools/deploy.sh`（Git Bash）で、訳文をゲームのフォルダの `witpae_ja\` に配置します。ゲームの起動中は配置できないので、終了してから行ってください。

手で反映する場合は、次のように置きます。

- `translation/ja.tsv` → `witpae_ja\ja.tsv`
- `translation/names/*.tsv` → `witpae_ja\names\`
- `translation/unit_types.tsv` → `witpae_ja\unit_types.tsv`
- シナリオの説明文は `python tools/scen.py build`（Shift_JIS に変換して `witpae_ja\SCEN\` に書く）

`translation/surnames.tsv` はゲームには置きません（人物の表を作るための資料です）。
