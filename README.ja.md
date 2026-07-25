Open Wallpaper Engine（パッチ版）
=========

[English](README.md) | [繁體中文](README.zh-TW.md) | **日本語**

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
- **OpenGL レンダラー** — 一貫したフレームグラフを実行し、CPU readback なしで `NSOpenGLView` に直接表示
- **Wallpaper Engine assets directory** — 一般設定で選択した公式シェーダー／マテリアル資産を使用
- **明示的な失敗** — 無効・未対応のシーンでは古いフレームやプレビューにフォールバックせず、画面をクリアしてエラーを表示

### インポート — フォルダインポートの修正
インポートパネルが個別の壁紙フォルダと複数の壁紙を含む親ディレクトリの両方を正しく処理するようになりました。

## 現在の制限事項

- **パーティクル互換性** — 決定論的なスプライト経路は box/sphere emitter、主要なランダム initializer、movement、alpha fade に対応しています。Trail/rope、子システムと control point、collision/boids、アニメーション粒子テクスチャ、非 `genericparticle` のマルチパスマテリアルは未対応です。
- **オーディオリアクティブシーン** — シーン内の音声再生には対応していますが、macOS のシステム音声キャプチャと Wallpaper Engine の audio-input buffer は利用できず、明示的なエラーになります。
- **テキスト互換性** — テキストレイヤーは描画できますが、ゼロ以外の文字間隔と高度な行数・幅・省略記号レイアウトは未対応です。
- **エフェクト互換性** — 画像マテリアル、passthrough シーン合成レイヤー、マルチパス effect、framebuffer binding、copy/swap/clear、カメラパララックス、自動投影に対応しています。Compose、puppet mesh、その他未モデル化の機能は明示的に失敗します。
- **ユーザープロパティ** — Boolean、slider、combo、color、テキスト入力は即時適用され、ディスプレイと Scene ごとに保存されます。Scene texture、ファイル、ディレクトリ、ショートカットのプロパティは現在表示のみです。

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

### ローカルファイルからインポート

- **フォルダ：** ファイル > フォルダからインポート — `project.json` を含む壁紙フォルダを選択
- **Zip：** ファイル > インポート またはドラッグ＆ドロップで `.zip` ファイルを読み込み
- **手動：** 壁紙フォルダを `~/Documents/Open Wallpaper Engine/` に直接コピー

## 変更ファイル（上流との差分）

**変更：**
- `WebWallpaperView.swift` — WKWebView ファイルアクセス設定
- `WallpaperView.swift` — シーン壁紙ディスパッチ
- `SceneWallpaperView.swift` — ネイティブ SceneRuntime を使う NSOpenGLView 実装
- `ImportPanels.swift` — フォルダインポートロジック修正

**追加：**
- `SceneRuntime/` — パッケージ解析、モデル、スクリプト、フレームグラフ、シェーダー、OpenGL 実行パイプライン
- `Services/SceneWallpaperViewModel.swift` — ネイティブ SceneRuntime セッションのアプリ側所有者
- `Services/SteamCmdService.swift` — steamcmd 検出、ログイン、Workshop ダウンロード
- `Services/WorkshopAPIService.swift` — Steam Web API クライアント
- `Services/WorkshopViewModel.swift` — Workshop ブラウザ状態管理
- `Services/WallpaperDirectory.swift` — 集中壁紙ストレージパス
- `Services/ZipImporter.swift` — Zip ファイル解凍とインポート
- `ContentView/Components/WorkshopView.swift` — Workshop ブラウザ UI
