# War in the Pacific AE 日本語化プロジェクト

- 作業を始める前に、必ず [PLAN.md](PLAN.md) を読む。現在地は5章のチェックリストと7章の進捗ログで分かる。
- 作業の区切りごとに PLAN.md のチェックリストを更新し、進捗ログに追記する。
- ゲームの元ファイル（`D:\Matrix Games\War in the Pacific Admiral's Edition` の既存ファイル）は書き換えない。追加するのは差し替え `ddraw.dll`、設定ファイル、`ja\` フォルダだけ。
- 対象 exe は32ビット。DLL は Win32（x86）でビルドする。
- **GitHub で公開する予定。原作のコード・データ・英文をリポジトリに入れない。** 訳文は `translation/ja.tsv` に「英文のハッシュ（h:FNV-1a 64bit）と訳文」だけで書く。英文入りの作業用シートは `work/`（Git 管理外）に置く。詳しくは PLAN.md の 1.1。
- コミットの作者は `yasabihhagure <yasabihhagure@users.noreply.github.com>`（リポジトリの git config に設定済み）。
