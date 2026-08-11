Open Wallpaper Engine（パッチ版）
=========

[English](README.md) | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | **日本語**

[![GitHub license](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

[Open Wallpaper Engine](https://github.com/MrWindDog/wallpaper-engine-mac) のパッチフォークです。macOS 向けにシーン壁紙のレンダリングと Web 壁紙の修正を追加しています。

> **注意：** 本プロジェクトは Steam の商用版 Wallpaper Engine とは無関係です。Steam Workshop の壁紙アセットを表示できるオープンソースの macOS アプリケーションです。

## 関連プロジェクト

- **[Open Wallpaper Engine for Linux](https://github.com/Unayung/simple-linux-wallpaperengine-gui)** — [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) 向けの PyQt6 GUI。Steam Workshop 統合と UI デザインは本 macOS バージョンから移植されました。

## クレジット

本プロジェクトは以下の貢献者の成果に基づいています：

- **[MrWindDog](https://github.com/MrWindDog)** — 上流 [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) フォークのメンテナー、新機能と UI 改善を追加
- **[Haren Chen](https://github.com/haren724)** — [open-wallpaper-engine-mac](https://github.com/haren724/open-wallpaper-engine-mac) のオリジナル作者、コアアーキテクチャを構築（SwiftUI、動画壁紙再生、インポートシステム、プレイリスト UI）
- **[1ris_W](https://github.com/Erica-Iris)** — 中国語 i18n 翻訳
- **[Klaus Zhu](https://github.com/klauszhu1105)** — アプリロゴアイコン
- **[Chen Chia Yang](https://github.com/Unayung)** — シーン壁紙レンダリング、Web 壁紙修正、Steam Workshop 統合、マルチディスプレイ対応、Zip インポート

[GPL-3.0](LICENSE) ライセンス（オリジナルプロジェクトと同一）。

## 0.8.1 の新機能

### シーン Runtime の互換性

シーン壁紙はネイティブ C++/Metal パイプラインで動作するようになり、Linux 版 Wallpaper Engine runtime の動作契約により近づきました。Wallpaper Engine パッケージ、モデル、GLSL から MSL へのシェーダー変換、スクリプト、フレームグラフ、テクスチャーを処理し、framebuffer を CPU に読み戻しません。

- **シーンレイヤーとエフェクト** — グループレイヤー、画像マテリアル、マルチパスエフェクト、framebuffer 操作、シーン合成、カーソル操作、カメラパララックス、自動投影に対応。
- **テキスト、メディア、動画** — 埋め込み／システムフォント、制作者指定のレイアウトとテキストエフェクトをネイティブ描画。動画テクスチャー、シーン内音声、対応スクリプト向けの macOS「再生中」メタデータとアートワークをサポート。
- **音声リアクティブシーン** — オプションの Core Audio システム音声キャプチャーが、Wallpaper Engine 音声入力を要求するシーンにスペクトラムデータを供給。
- **忠実な表示** — 制作者の座標方向とアスペクト比を維持し、自動、ストレッチ、フィット、フィルのスケーリングを提供。シーン壁紙は 1 枚の仮想キャンバスで複数ディスプレイにまたがることもできます。
- **レンダリング負荷の削減** — framebuffer の計画で互換ターゲットを再利用し、不要な framebuffer 処理を回避。

多くのシーン壁紙には公式 Wallpaper Engine の `assets` ディレクトリが必要です。これらのアセットはプロプライエタリであり、本プロジェクトには同梱されません。[Wallpaper Engine assets を設定する](#wallpaper-engine-assets-を設定する)を参照してください。

### シーンプロパティの操作

壁紙の詳細画面で runtime が提供するシーンのユーザープロパティを操作できます。Boolean、slider、combo、color、テキスト入力は実行中のシーンに即時反映され、ディスプレイおよび壁紙ごとに保存されます。プロパティ説明は整形テキスト、リンク、リモート画像をサポートし、グループ化と既定値へのリセットも可能です。

### 再生・音声ポリシー

再生設定は、動画、Web、シーン壁紙に共通のポリシーとして適用されます。他のアプリがフォーカス中、フルスクリーンまたは最大化、音声再生中、またはラップトップがバッテリー使用中の場合に、実行継続、ミュート、一時停止、停止を設定できます。複数条件が成立した場合は最も制限の強い動作が選ばれます。ディスプレイのスリープ時は常に一時停止し、復帰時に再開します。

「他のアプリが音声を再生中」ルールと音声リアクティブシーンには、一般設定での**システム音声キャプチャー**有効化が必要です。キャプチャーエラーはルールを黙って無効化せず、アプリ内に表示されます。

### Workshop の閲覧とプレビュー

Workshop ブラウザは検索・ソートに加え、コンテンツレーティング、壁紙タイプ、ジャンルでフィルタリングできます。リモート画像と GIF プレビューの読み込みも再設計され、検索結果と壁紙カードがコンテンツの読み込み中に安定して更新されます。

### シーン品質の操作

シーン専用のパフォーマンス設定に、レンダリング品質、FPS、表示スケーリング、マルチディスプレイのスパン表示を追加しました。未対応のアンチエイリアス、後処理、テクスチャ解像度、反射のコントロールは、利用可能であるかのように見せず明示的に無効化されます。

## 0.8.0 の新機能

### マルチディスプレイ対応
接続された各モニターに異なる壁紙を割り当て、画面ごとに有効/無効を制御できます。
- **ディスプレイ設定パネル** — 接続されたすべての画面をビジュアルレイアウトで表示、クリックで選択
- **画面ごとの壁紙** — 各ディスプレイで異なる壁紙を独立して表示
- **有効/無効トグル** — モニターごとに壁紙のオン/オフを切り替え
- **自動検出** — 新しいモニターは接続時に自動的に検出・有効化

### マルチデスクトップ対応
壁紙がすべての macOS デスクトップ（Spaces）で連続再生されるようになりました。デスクトップ切り替え時も中断しません。

### 最近使った壁紙メニュー
ステータスバーメニューから壁紙を素早く切り替えられます。最近使用した10件の壁紙にワンクリックでアクセスできます。

### 再生設定 — 修正済み
パフォーマンス再生設定（他のアプリがフォーカスされた時の一時停止/ミュート/停止）がすべての壁紙タイプで正しく動作するようになりました。

### Steam Workshop ブラウザ
アプリ内から直接 Steam Workshop の壁紙を閲覧、検索、ダウンロードできます。
- **検索とフィルター** — 名前で検索、コンテンツレーティング（Everyone/Questionable/Mature）、タイプ（Scene/Video/Web）、ジャンルタグでフィルター
- **ソートオプション** — トレンド、最新、人気順、サブスクライブ数順
- **steamcmd 統合** — steamcmd を自動検出（Homebrew またはカスタムパス）、未インストール時はインストール手順を表示
- **Steam ログイン** — パスワード、Steam Guard、キャッシュセッション認証に対応
- **ダウンロード進捗表示** — リアルタイムステータス更新（認証中、ダウンロード %、検証、コピー中）
- **安全なデフォルト** — コンテンツレーティングを「Everyone」に設定し、成人向けコンテンツをフィルタリング

### Zip インポート
`.zip` ファイルから壁紙パッケージを直接インポート。手動解凍は不要です。ファイル > インポートおよびドラッグ＆ドロップに対応。

### 複数選択と一括解除
Cmd+クリックで複数の壁紙を選択し、右クリックで一括サブスクライブ解除。

### 壁紙ストレージの分離
壁紙は `~/Documents/Open Wallpaper Engine/` に保存されるようになり、Documents ディレクトリを直接使用しなくなりました。リポジトリをクローンした際の「error」壁紙を防止します。

## パッチ内容

### Web 壁紙 — グレー/空白レンダリングの修正
WebGL ベースの壁紙は `WKWebView` がローカルファイルアクセスをブロックしていたため、グレーの矩形として表示されていました。

**修正：** WKWebView 設定で `allowFileAccessFromFileURLs` と `allowUniversalAccessFromFileURLs` を有効にし、WebGL シェーダーがローカルテクスチャファイルを読み込めるようにしました。

### シーン壁紙 — ゼロから実装
シーン壁紙（Steam Workshop で最も一般的なタイプ）は完全に未実装で、「Hello, World!」のみ表示されていました。

**新しい実装：**
- **ネイティブ SceneRuntime** — PKGV/TEXV、モデル、マテリアル、シェーダー、スクリプトを単一の C++ ランタイムで処理
- **Metal レンダラー** — 一貫したフレームグラフを実行し、CPU readback なしで `MTKView` に直接表示
- **Wallpaper Engine assets directory** — 一般設定で選択した公式シェーダー／マテリアル資産を使用
- **明示的な失敗** — 無効・未対応のシーンでは古いフレームやプレビューにフォールバックせず、画面をクリアしてエラーを表示

### インポート — フォルダインポートの修正
インポートパネルが個別の壁紙フォルダと複数の壁紙を含む親ディレクトリの両方を正しく処理するようになりました。

## 現在の制限事項

- **パーティクル互換性** — 決定論的なスプライト経路は box/sphere emitter、主要なランダム initializer、movement、alpha fade に対応しています。Trail/rope、子システムと control point、collision/boids、アニメーション粒子テクスチャ、非 `genericparticle` のマルチパスマテリアルは未対応です。
- **エフェクト互換性** — シーン合成と一般的な画像／エフェクト経路には対応していますが、一部の高度な compose、puppet mesh、未モデル化のエフェクト機能はまだ描画できません。
- **ユーザープロパティ** — Boolean、slider、combo、color、テキスト入力は編集可能です。Scene texture、ファイル、ディレクトリ、ショートカットのプロパティは表示のみです。
- **プラットフォーム依存入力** — システム音声キャプチャーと「再生中」連携は macOS サービスに依存します。サービスが利用できない場合、アプリは失敗を表示し、影響を受けるシーン入力は利用できません。

## サポートされている壁紙タイプ

| タイプ | ステータス |
|--------|------------|
| 動画 (.mp4, .webm) | 動作中（オリジナル） |
| Web (HTML/WebGL) | 動作中（パッチ済み） |
| シーン（画像、エフェクト、スクリプト） | 部分対応 |
| シーン（パーティクル、テキスト、音声） | 部分対応 |
| アプリケーション | 未サポート |

## ソースからビルド

### 前提条件
- macOS >= 13.0
- Xcode >= 14.4
- Xcode Command Line Tools

### 手順
```sh
git clone https://github.com/unayung/wallpaper-engine-mac
cd wallpaper-engine-mac
open "Open Wallpaper Engine.xcodeproj"
```

Xcode で署名証明書を自分のものに変更するか「Sign to Run Locally」を選択し、`Cmd + R` でビルド・実行します。

## 使い方

### Steam Workshop から閲覧・ダウンロード

1. steamcmd をインストール（`brew install steamcmd`）するか、既存のバイナリを指定
2. **Workshop** タブに切り替え、Steam アカウントでログイン（Wallpaper Engine の所有が必要）
3. プロンプトが表示されたら [Steam Web API キー](https://steamcommunity.com/dev/apikey) を入力
4. 検索、フィルターし、**Download** をクリックして壁紙をダウンロード

### Wallpaper Engine assets を設定する

多くのシーン壁紙は、オリジナルの Wallpaper Engine シェーダーとマテリアルに依存しています。これらのプロプライエタリファイルは、このリポジトリおよびアプリには含まれていません。

1. Wallpaper Engine を所有する Steam アカウントで、Windows の Steam からインストールするか、そのアカウントの既存インストールを使用します。
2. Steam で **Wallpaper Engine > 管理 > ローカルファイルを閲覧** を開きます。既定の Steam ライブラリではアプリケーションディレクトリは `C:\Program Files (x86)\Steam\steamapps\common\wallpaper_engine` で、必要なディレクトリはその中の `assets` です。
3. `assets` ディレクトリを Mac 上の安定して読み取り可能な場所にコピーします。このアプリの **設定 > 一般 > Wallpaper Engine assets** でその `assets` ディレクトリ、または親の `wallpaper_engine` ディレクトリを選択します。選択したディレクトリには `shaders/` が必要です。

これらのファイルをプロジェクトに追加、コミット、再配布しないでください。Steam ライブラリのパスは異なる場合があります。Wallpaper Engine の[公式サポートドキュメント](https://help.wallpaperengine.io/en/steam/contentfile.html)も同じ `steamapps/common/wallpaper_engine` のインストール構成を使用しています。

### ローカルファイルからインポート

- **フォルダ：** ファイル > フォルダからインポート — `project.json` を含む壁紙フォルダを選択
- **Zip：** ファイル > インポート またはドラッグ＆ドロップで `.zip` ファイルを読み込み
- **保存場所：** **設定 > 一般 > 壁紙の保存場所** でダウンロードとインポートの保存先を選択します。既定の保存先は `~/Documents/Open Wallpaper Engine/` で、変更しても既存の壁紙は移動されません。

## アーキテクチャ

- `SceneRuntime/` はネイティブのパッケージ解析、モデル、スクリプト、シェーダー、フレームグラフ、音声、Metal 実行パイプラインを含みます。
- アプリ層は、ディスプレイごとのシーンセッション、プロパティ保存、再生ポリシー、macOS 音声キャプチャー、「再生中」入力を管理します。
- Workshop 閲覧は Steam Web API でコンテンツを見つけ、`steamcmd` で認証とダウンロードを行います。
