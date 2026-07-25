Open Wallpaper Engine（修補版）
=========

[English](README.md) | **繁體中文** | [日本語](README.ja.md)

[![GitHub license](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

基於 [Open Wallpaper Engine](https://github.com/MrWindDog/wallpaper-engine-mac) 的修補分支，為 macOS 加入場景桌布渲染與網頁桌布修復。

> **注意：** 本專案與 Steam 上的商業版 Wallpaper Engine 無關。這是一個開源的 macOS 應用程式，可顯示來自 Wallpaper Engine Steam 創意工坊的桌布素材。

## 相關專案

- **[Open Wallpaper Engine for Linux](https://github.com/Unayung/simple-linux-wallpaperengine-gui)** — 基於 [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) 的 PyQt6 圖形介面，Steam 工作坊整合與 UI 設計移植自本 macOS 版本。

## 致謝

本專案建立於以下貢獻者的成果之上：

- **[MrWindDog](https://github.com/MrWindDog)** — 上游 [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) 分支的維護者，新增功能與 UI 優化
- **[Haren Chen](https://github.com/haren724)** — [open-wallpaper-engine-mac](https://github.com/haren724/open-wallpaper-engine-mac) 原作者，建構核心架構（SwiftUI、影片桌布播放、匯入系統、播放清單 UI）
- **[1ris_W](https://github.com/Erica-Iris)** — 中文 i18n 翻譯
- **[Klaus Zhu](https://github.com/klauszhu1105)** — 應用程式圖示
- **[Chen Chia Yang](https://github.com/Unayung)** — 場景桌布渲染、網頁桌布修復、Steam 創意工坊整合、多螢幕支援、Zip 匯入

採用 [GPL-3.0](LICENSE) 授權，與原始專案相同。

## 0.8.0 新功能

### 多螢幕支援
為每個連接的螢幕指定不同的桌布，並可個別啟用或停用。
- **顯示器設定面板** — 以視覺化佈局顯示所有連接的螢幕，點擊選取
- **個別螢幕桌布** — 每個螢幕可獨立顯示不同的桌布
- **啟用/停用切換** — 可針對每個螢幕開啟或關閉桌布
- **自動偵測** — 新連接的螢幕會自動偵測並啟用

### 多桌面支援
桌布現在可在所有 macOS 桌面（空間）上顯示並持續播放，切換桌面時不會中斷。

### 最近使用的桌布選單
可從狀態列選單快速切換桌布。最近使用的 10 個桌布可一鍵存取。

### 播放設定 — 已修復
效能播放設定（切換應用程式時暫停/靜音/停止）現在對所有桌布類型均可正常運作。

### Steam 創意工坊瀏覽器
直接在應用程式內瀏覽、搜尋及下載 Steam 創意工坊的桌布。
- **搜尋與篩選** — 依名稱搜尋，依內容分級（Everyone/Questionable/Mature）、類型（Scene/Video/Web）及風格標籤篩選
- **排序選項** — 熱門趨勢、最新發布、最受歡迎、最多訂閱
- **steamcmd 整合** — 自動偵測 steamcmd（Homebrew 或自訂路徑），未安裝時提供安裝指引
- **Steam 登入** — 支援密碼、Steam Guard 及快取 Session 驗證
- **下載進度顯示** — 即時狀態更新（驗證中、下載百分比、驗證、複製中）
- **安全預設** — 內容分級預設為「Everyone」，過濾成人內容

### Zip 匯入
直接匯入 `.zip` 桌布套件，無需手動解壓縮。支援 檔案 > 匯入 及拖放操作。

### 多選與批次取消訂閱
Cmd+點擊選取多個桌布，右鍵選擇批次取消訂閱。

### 桌布儲存隔離
桌布現在儲存在 `~/Documents/Open Wallpaper Engine/`，不再使用原始 Documents 目錄，避免克隆專案時出現「error」桌布。

## 修補內容

### 網頁桌布 — 修復灰色/空白渲染
基於 WebGL 的桌布因 `WKWebView` 阻擋本地檔案存取而顯示為灰色方塊。

**修復：** 在 WKWebView 設定中啟用 `allowFileAccessFromFileURLs` 和 `allowUniversalAccessFromFileURLs`，允許 WebGL 著色器載入本地紋理檔案。

### 場景桌布 — 從零開始實作
場景桌布（Steam 創意工坊最常見的類型）原本完全未實作——僅顯示「Hello, World!」。

**新實作包括：**
- **原生 SceneRuntime** — 由單一 C++ runtime 處理 PKGV/TEXV、模型、材質、shader 與腳本
- **OpenGL 渲染器** — 執行一致的 frame graph，無需 CPU readback 即可直接呈現在 `NSOpenGLView`
- **Wallpaper Engine assets directory** — 使用一般設定中選取的官方 shader 與材質資源
- **明確失敗** — 無效或尚未支援的場景會清除畫面並顯示錯誤，不會靜默保留舊畫面或預覽圖

### 匯入 — 修復資料夾匯入
匯入面板現在可正確處理單一桌布資料夾和包含多個桌布的父目錄。

## 目前限制

- **粒子相容性** — 確定性的 sprite 路徑已支援 box/sphere emitter、常用隨機 initializer、movement 與 alpha fade。Trail/rope、子系統與 control point、collision/boids、動態粒子紋理，以及非 `genericparticle` 的多 pass 材質仍未支援。
- **音訊互動場景** — 已支援場景內建音效播放，但 macOS 系統音訊擷取與 Wallpaper Engine audio-input buffer 尚不可用，並會明確回報錯誤。
- **文字相容性** — 已支援文字圖層渲染，但非零字元間距與進階行數、寬度、刪節號排版仍未支援。
- **效果相容性** — 已支援圖片材質、passthrough 場景合成圖層、多 pass effect、framebuffer binding、copy/swap/clear、相機視差與自動投影。Compose、puppet mesh 及其他尚未建模的效果功能會明確失敗。
- **使用者屬性** — Boolean、slider、combo、color 與文字輸入會即時套用，並依顯示器與 Scene 保存。Scene texture、檔案、資料夾與快捷鍵屬性目前可見但唯讀。

## 支援的桌布類型

| 類型 | 狀態 |
|------|------|
| 影片 (.mp4, .webm) | 正常運作（原始） |
| 網頁 (HTML/WebGL) | 正常運作（已修補） |
| 場景（圖片、效果、腳本） | 部分支援 |
| 場景（粒子、文字、音訊） | 部分支援 |
| 應用程式 | 不支援 |

## 從原始碼建置

### 前置需求
- macOS >= 13.0
- Xcode >= 14.4
- Xcode Command Line Tools

### 步驟
```sh
git clone https://github.com/unayung/wallpaper-engine-mac
cd wallpaper-engine-mac
open "Open Wallpaper Engine.xcodeproj"
```

在 Xcode 中，將簽署憑證更改為您自己的或選擇「Sign to Run Locally」，然後按 `Cmd + R` 建置並執行。

## 使用方式

### 從 Steam 創意工坊瀏覽與下載

1. 安裝 steamcmd（`brew install steamcmd`）或指向現有的二進位檔
2. 切換到 **Workshop** 分頁，使用 Steam 帳號登入（必須擁有 Wallpaper Engine）
3. 出現提示時輸入 [Steam Web API 金鑰](https://steamcommunity.com/dev/apikey)
4. 搜尋、篩選，然後點擊 **Download** 下載桌布

### 從本地檔案匯入

- **資料夾：** 檔案 > 從資料夾匯入——選擇包含 `project.json` 的桌布資料夾
- **Zip：** 檔案 > 匯入 或拖放包含桌布套件的 `.zip` 檔案
- **手動：** 直接將桌布資料夾複製到 `~/Documents/Open Wallpaper Engine/`

## 變更的檔案（相對上游）

**修改：**
- `WebWallpaperView.swift` — WKWebView 檔案存取設定
- `WallpaperView.swift` — 場景桌布分派
- `SceneWallpaperView.swift` — 使用原生 SceneRuntime 的 NSOpenGLView 實作
- `ImportPanels.swift` — 資料夾匯入邏輯修復

**新增：**
- `SceneRuntime/` — 套件解析、模型、腳本、frame graph、shader 與 OpenGL 執行管線
- `Services/SceneWallpaperViewModel.swift` — 原生 SceneRuntime session 的 App 端擁有者
- `Services/SteamCmdService.swift` — steamcmd 偵測、登入與創意工坊下載
- `Services/WorkshopAPIService.swift` — Steam Web API 客戶端
- `Services/WorkshopViewModel.swift` — 創意工坊瀏覽器狀態管理
- `Services/WallpaperDirectory.swift` — 集中式桌布儲存路徑
- `Services/ZipImporter.swift` — Zip 檔案解壓與匯入
- `ContentView/Components/WorkshopView.swift` — 創意工坊瀏覽器 UI
