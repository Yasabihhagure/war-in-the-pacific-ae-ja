# War in the Pacific Admiral's Edition 日本語化 実装計画

最終更新: 2026-11-26

## 1. 目的と方針

- ゲーム内の文字（画面の文言・メッセージ・シナリオ説明）を日本語にする。
- 参考: https://higesho.github.io/ultima-ja/ （Ultima 日本語化）。考え方だけを借りる。DLL は流用しない（向こうは DOSBox 用の opengl32.dll で、仕組みが違う）。
- **ゲームの元ファイルは書き換えない。** 置くだけのファイル（差し替え `ddraw.dll` と訳文ファイル）で日本語化し、外せば英語に戻る。
- 配布するなら「ゲームのフォルダにコピーするだけの zip」にする。原作のファイルは含めない。

### 1.1 GitHub 公開の方針（2026-11-26 決定）

- 最終的に GitHub で公開する（Ultima 日本語化と同様）。
- **原作のコード・データ・英文をリポジトリに入れない。**
  - 訳文ファイル `translation/ja.tsv` は `キー<TAB>訳文<TAB>メモ`。キーは英文のバイト列の FNV-1a 64bit を `h:` + 16桁の16進で書いたもの。英文そのものは書かない。
  - 英文と訳文を並べた作業用シートは `python tools/sheet.py export` で手元のゲームから作る（`work/sheet.tsv`）。編集後に `import` で訳文だけを ja.tsv に戻す。
  - `work/`、`extract/`、`logs/`、ゲームのログ、ビルド結果、原作の拡張子（dat、bmp など）は `.gitignore` で除外している。
  - 書式つきの文の照合（4.2）でも、英文の書式文字列は実行時に exe のメモリから読み、ハッシュで訳文と結びつける。英文をリポジトリに持たない。
  - メモ欄にも原作の英文を写さない。
- コミットの作者: `yasabihhagure <yasabihhagure@users.noreply.github.com>`（リポジトリの git config に設定済み）。
- ライセンスは未定（Ultima 日本語化は MIT）。公開前に決める。
- 訳文そのもの（シナリオ解説の翻訳など）は原作の二次的な著作物にあたる。ファンメイド翻訳の通例として公開するが、権利者から求めがあれば応じる旨を README に書く。

## 2. 場所

| 用途 | パス |
| --- | --- |
| ゲーム本体（インストール先） | `D:\Matrix Games\War in the Pacific Admiral's Edition` |
| 作業フォルダ（ソース・訳文・ツール） | `d:\ゲーム日本語化\WarInThePacificAdmiralsEdition` |
| 元ファイルのバックアップ | `d:\ゲーム日本語化\backup_original` |

予定しているフォルダ構成（作業フォルダの中）:

```
PLAN.md            この計画書（進捗ログつき）
CLAUDE.md          新しい会話向けの案内
src/ddraw/         差し替え ddraw.dll のソース（C++、VS2022、Win32/x86）
tools/             抽出・変換スクリプト（Python）
extract/           抽出した英文（自動生成）
translation/       訳文の対応表（人が編集するもの、UTF-8）
dist/              配布用の組み立て結果
logs/              ゲーム実行時のログの写し
```

## 3. 調査で分かったこと（2026-11-26）

- exe: `War in the Pacific Admiral Edition.exe`（2016-07-10、5,054,464 バイト）。**32ビット（i386）**。
- 画面: `DDRAW.dll` の `DirectDrawCreateEx` と `DirectDrawCreateClipper` だけを import している。
- 文字: GDI32 の `CreateFontA`、`TextOutA`、`GetTextExtentPoint32A`、`SetTextColor`、`SetBkColor` を使う。フォント名で見つかったのは `Times New Roman` だけ。**自前の画像フォントではない** → 日本語フォント＋Shift_JIS 文字列で表示できる見込み。
- USER32: `SetDlgItemTextA`、`MessageBoxA`、`DialogBoxParamA`、`CreateWindowExA` などもある（ダイアログ類）。
- exe 内の英文（6文字以上）は約9,000件。そのうち画面やログの文章らしいものは約1,600件。`%s`、`%d`、`%.20s` などの書式指定が多い（sprintf 系は静的リンクなので、IAT フックはできない）。
- `pwsdll.dll` は KERNEL32 だけを import する（文字描画には関係なさそう）。`pwhexe.dat` などはコピー保護関係と思われる。**exe の改変は保護に引っかかる恐れがあるので避ける。**
- `SCEN\`: `AHistory*.txt`、`ADetail*.txt` などの解説文（プレーンテキスト、英語）。シナリオの `aei*.dat`（各 2,656,000 バイト）には艦名・部隊名・スクリプト名が固定長で入っている。
- `ART\`: bmp が約8,000枚。ボタン画像（`Button*.bmp`）など。文字入りの画像は少なそう（未精査）。
- 開発環境: Visual Studio 2022 あり。Python 3.12 あり（pefile は未インストール）。

## 4. 技術設計

### 4.1 差し替え ddraw.dll（プロキシ DLL）

1. ゲームのフォルダに置いた `ddraw.dll` が先に読み込まれる。
2. 本物の `ddraw.dll` を `GetSystemDirectory`（32ビットプロセスでは SysWOW64）から読み込み、`DirectDrawCreateEx` などを本物へ転送する。転送する関数は、主要なエクスポートをすべて（DirectDrawCreate、DirectDrawCreateEx、DirectDrawCreateClipper、DirectDrawEnumerateA/ExA ほか）。
3. `DllMain` で exe の IAT を書き換え、次をフックする。
   - `CreateFontA`: フォント名を日本語フォント（既定は MS ゴシック。設定で変更可）に、文字セットを `SHIFTJIS_CHARSET` にする。高さは元のまま（必要なら補正）。
   - `TextOutA`、`GetTextExtentPoint32A`: 英文を訳文に置き換えてから本物を呼ぶ（幅の計算も訳文で行う）。
   - `SetDlgItemTextA`、`MessageBoxA` など: 同様に置き換える。
4. 設定ファイル `witpae_ja.ini`（フォント名、ログの ON/OFF など）。
5. ログ機能: `TextOutA` などに渡された文字列を、重複を除いて `witpae_ja_textlog.txt` に書き出す。翻訳対象の洗い出しに使う。

### 4.2 訳文の置き換え方（2段構え）

- **方式A（まずこれ）: 描画時の置き換え**
  - 完全一致: `英文 → 訳文` の辞書を引く。
  - 書式つき: exe 内の書式文字列（例 `Saving game in slot %d`）から照合パターンを作る。描画された文字列がパターンに合えば、埋め込まれた値を取り出して訳文の書式（例 `スロット%dにセーブ中`）に入れ直す。語順の入れ替え用に `%1$s` 風の番号指定を訳文側で使えるようにする。
- **方式B（方式Aで足りない場合）: メモリ上の文字列ポインタの差し替え**
  - 起動時に、exe のメモリ上の英文を指すポインタ（コード中の push imm32 / mov など）を、訳文（Shift_JIS）を指すように書き換える。ファイルは書き換えない。
  - 比較や内部処理に使われる文字列を誤って変えると壊れるので、対象は確認したものだけにする。

### 4.3 訳文ファイル

- `translation/ja.tsv`（UTF-8、タブ区切り）: `h:ハッシュ<TAB>訳文<TAB>メモ`（1.1 参照）。辞書にない文は英語のまま表示する。ゲームのフォルダには `witpae_ja\ja.tsv` として置く。
- DLL が読み込むときに CP932（Shift_JIS）へ変換する。CP932 にない文字は警告を出す。
- シナリオ説明（`SCEN\*.txt`）は別扱い（4.4）。

### 4.4 シナリオ説明の txt

- ゲームが `CreateFileA` で `SCEN\AHistory001.txt` などを開くときにフックし、`ja\SCEN\AHistory001.txt`（Shift_JIS）があればそちらに差し替える。元ファイルは書き換えない。
- 1行の文字数や行数に制限があるかは試して確かめる。

### 4.5 注意点（既知のリスク）

- 枠の幅は英語に合わせてある → 訳文が長いとはみ出す。短い訳にするか、フォントの大きさを少し下げる。
- `%.20s` のようなバイト数での切り詰めや、1バイトずつの文字処理（大文字化など）で、日本語が文字の途中で切れて化ける。→ 置き換え後にバイト境界を直す処理を DLL に入れる。
- 艦名・基地名・指揮官名は固定長のデータで、数が多い。**当面は英語のまま。** 余力があれば、描画時の辞書で置き換える。
- 画像の中の文字は当面英語のまま。
- 自動翻訳（plamo-translate MCP）は下訳に使えるが、軍事用語・艦種略号（CV、BB、DD など）は用語集で統一する。

## 5. 工程とチェックリスト

### フェーズ0: 準備
- [x] 元ファイルのバックアップ（ゲームのフォルダを丸ごと `d:\ゲーム日本語化\backup_original\` にコピー済み、1.7GB）
- [x] 作業フォルダの構成を作る
- [x] Win32 DLL をビルドできることを確認する → Windows SDK がないため、**llvm-mingw（`D:\Tools\llvm-mingw`、20261117版）を使う**。ビルドは `sh src/ddraw/build.sh`（Git Bash）。配置は `sh tools/deploy.sh`、取り外しは `sh tools/uninstall.sh`。

### フェーズ1: 試作（日本語が出るかの確認）← 最大の山場
- [ ] プロキシ ddraw.dll（転送だけ）を作り、ゲームが普通に起動することを確認する
- [ ] IAT フック（CreateFontA / TextOutA / GetTextExtentPoint32A）を入れ、ログを取る
- [ ] ゲームを動かしてログを集め、どんな単位で文字列が描かれているか（1行ずつか、単語ずつか、整形済みか）を確認する
- [ ] フォントを日本語にし、数行だけ訳した辞書で、日本語が画面に出ることを確認する
- [ ] 結果に応じて、方式A で行けるか、方式B が必要かを判断し、この計画に書く

### フェーズ2: 抽出
- [ ] exe の英文を抽出し、CRT のエラー文などのゴミを除く（`tools/extract_exe.py`）
- [ ] ログで集めた実際の表示文と突き合わせて、翻訳対象の一覧を作る
- [ ] `SCEN\*.txt` を一覧にする

### フェーズ3: 翻訳
- [ ] 用語集を作る（`translation/glossary.tsv`。艦種、部隊、ゲーム用語）
- [ ] メイン画面・メニュー・よく出るメッセージから訳す
- [ ] 戦闘報告・作戦報告
- [ ] シナリオ説明（History / Detail）

### フェーズ4: 仕上げ
- [ ] はみ出し・文字化けの修正
- [ ] 文字の途中で切れる問題への対策
- [ ] 設定ファイル、導入手順書（README）
- [ ] 配布用 zip の組み立て（`dist/`）

## 6. 新しい会話で再開するときの手順

1. この `PLAN.md` を読み、5章のチェックリストと7章の進捗ログで現在地を確認する。
2. 7章の最新の「次にやること」から再開する。
3. 作業の区切りごとに、チェックリストを更新し、7章へ1〜数行の記録を追記する。

## 7. 進捗ログ

- 2026-11-26: 調査完了。計画書を作成。次にやること: フェーズ0（バックアップ、フォルダ作成、ビルド確認）。
- 2026-11-26: バックアップとフォルダ作成が完了。試作 DLL のソースを作成した（`src/ddraw/ddraw_proxy.cpp`、`ddraw.def`、`build.bat`）。中身は ddraw の転送、CreateFontA/TextOutA/GetTextExtentPoint32A/SetDlgItemTextA/MessageBoxA の IAT フック、文字列ログ（`witpae_ja_log.txt`）、辞書（`witpae_ja\strings.tsv`）。ビルドしようとしたら Windows SDK がなくて失敗した。注意: build.bat に日本語を書くと cmd が誤動作するので、ASCII だけにする。次にやること: SDK を入れてビルドし、ゲームのフォルダに ddraw.dll を置いて起動を試す。
- 2026-11-26: ユーザーの希望（Dドライブに入れる）で llvm-mingw を `D:\Tools\llvm-mingw` に展開した。MSVC 用の build.bat は削除し、`build.sh` に置き換えた。ビルドが通り、エクスポート21件と、転送の入口（jmp）が正しいことを確認した。試験用の辞書（21件）と ini を作り、ゲームのフォルダに配置した（ddraw.dll、witpae_ja.ini、witpae_ja\strings.tsv）。次にやること: ユーザーにゲームを起動してもらい、`witpae_ja_log.txt` と画面を確認する。
- 2026-11-26: GitHub 公開の方針を決めた（1.1）。辞書をハッシュキー方式（`translation/ja.tsv`）に変え、`tools/sheet.py` を作った。ログを共有モードで開くように直した（実行中も読める）。git init して初回コミット。この時点で、ユーザーが旧版 DLL（英文キーの strings.tsv 版）でゲームを起動中だったため、新版の DLL はまだゲームのフォルダに配置できていない。次にやること: ゲーム終了後に `sh tools/deploy.sh`、ログを確認する。
