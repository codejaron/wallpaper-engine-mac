Open Wallpaper Engine（修補版）
=========

[English](README.md) | [简体中文](README.zh-CN.md) | **繁體中文** | [日本語](README.ja.md)

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

## 0.8.1 新功能

### 場景 Runtime 相容性

場景桌布現在透過原生 C++/Metal 管線執行，並更貼近 Linux Wallpaper Engine runtime 的行為契約。可處理 Wallpaper Engine 套件、模型、GLSL 到 MSL shader 轉換、腳本、frame graph 與紋理，無須將 framebuffer 讀回 CPU。

- **場景圖層與效果** — 支援群組圖層、圖片材質、多 pass 效果、framebuffer 操作、場景合成、游標互動、相機視差與自動投影。
- **文字、媒體與影片** — 原生繪製嵌入與系統字型、作者設定的版面與文字效果；支援影片紋理、場景內建音效，以及相容腳本使用的 macOS「正在播放」中繼資料與封面。
- **音訊反應場景** — 可選的 Core Audio 系統音訊擷取會向需要 Wallpaper Engine 音訊輸入的場景提供頻譜資料。
- **正確呈現** — 保留作者座標方向與長寬比，提供自動、拉伸、符合與填滿縮放；場景桌布也能以一張虛擬畫布跨越多個螢幕。
- **較低的渲染成本** — framebuffer 規劃會重用相容目標，並避免不必要的 framebuffer 工作。

大多數場景桌布需要官方 Wallpaper Engine 的 `assets` 目錄。這些資產受專有授權保護，並不隨本專案提供；請參閱[設定 Wallpaper Engine assets](#設定-wallpaper-engine-assets)。

### 場景屬性控制

桌布詳細資訊頁會顯示 runtime 提供的場景使用者屬性。Boolean、slider、combo、color 與文字輸入屬性會立即套用到執行中的場景，並依螢幕與桌布分別保存。屬性說明支援格式化文字、連結與遠端圖片，也支援屬性分組和還原預設值。

### 播放與音訊策略

播放設定現在由一套策略統一套用到影片、網頁與場景桌布。可設定在其他應用程式取得焦點、全螢幕或最大化、播放音訊，或筆電使用電池時繼續執行、靜音、暫停或停止。多個條件同時成立時，會採用限制最嚴格的動作；螢幕休眠時一律暫停，喚醒後恢復。

「其他應用程式正在播放音訊」規則與音訊反應場景都需要在一般設定啟用**系統音訊擷取**。擷取失敗會在 App 中顯示原因，而不會靜默停用規則。

### 創意工坊瀏覽與預覽

創意工坊瀏覽器除搜尋與排序外，現在可按內容分級、桌布類型與題材篩選。遠端圖片和 GIF 預覽的載入邏輯也已重構，搜尋結果與桌布卡片會在內容載入時穩定更新。

### 場景品質控制

場景專用的效能設定新增渲染品質、FPS、呈現縮放與跨螢幕模式。不支援的抗鋸齒、後處理、紋理解析度和反射控制會明確停用，不再造成可用的誤解。

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
- **Metal 渲染器** — 執行一致的 frame graph，無需 CPU readback 即可直接呈現在 `MTKView`
- **Wallpaper Engine assets directory** — 使用一般設定中選取的官方 shader 與材質資源
- **明確失敗** — 無效或尚未支援的場景會清除畫面並顯示錯誤，不會靜默保留舊畫面或預覽圖

### 匯入 — 修復資料夾匯入
匯入面板現在可正確處理單一桌布資料夾和包含多個桌布的父目錄。

## 目前限制

- **粒子相容性** — 確定性的 sprite 路徑已支援 box/sphere emitter、常用隨機 initializer、movement 與 alpha fade。Trail/rope、子系統與 control point、collision/boids、動態粒子紋理，以及非 `genericparticle` 的多 pass 材質仍未支援。
- **效果相容性** — 已支援場景合成與常見圖片/效果路徑，但部分進階 compose、puppet mesh 和尚未建模的效果功能仍無法繪製。
- **使用者屬性** — Boolean、slider、combo、color 與文字輸入可編輯；Scene texture、檔案、資料夾與快捷鍵屬性目前可見但唯讀。
- **平台相關輸入** — 系統音訊擷取與「正在播放」整合依賴 macOS 服務。服務不可用時，App 會回報失敗，受影響的場景輸入也無法使用。

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

### 設定 Wallpaper Engine assets

大多數場景桌布依賴原版 Wallpaper Engine 的 shader 與材質。這些專有檔案不包含在本儲存庫或 App 中。

1. 使用擁有 Wallpaper Engine 的 Steam 帳號，透過 Steam 在 Windows 安裝它，或使用該帳號既有的安裝目錄。
2. 在 Steam 開啟 **Wallpaper Engine > 管理 > 瀏覽本機檔案**。預設 Steam 資料庫的應用程式目錄為 `C:\Program Files (x86)\Steam\steamapps\common\wallpaper_engine`，所需目錄是其中的 `assets`。
3. 將 `assets` 目錄複製到 Mac 上穩定且可讀的位置。在本 App 開啟 **設定 > 一般 > Wallpaper Engine assets**，選取該 `assets` 目錄，或它的 `wallpaper_engine` 父目錄。所選目錄必須包含 `shaders/`。

請勿將這些檔案加入儲存庫、提交或重新散布。Steam 資料庫路徑可能不同；Wallpaper Engine 的[官方支援文件](https://help.wallpaperengine.io/en/steam/contentfile.html)採用相同的 `steamapps/common/wallpaper_engine` 安裝目錄結構。

### 從本地檔案匯入

- **資料夾：** 檔案 > 從資料夾匯入——選擇包含 `project.json` 的桌布資料夾
- **Zip：** 檔案 > 匯入 或拖放包含桌布套件的 `.zip` 檔案
- **儲存位置：** 在 **設定 > 一般 > 桌布儲存位置** 選擇下載與匯入的儲存目錄。預設目錄為 `~/Documents/Open Wallpaper Engine/`；變更目錄不會移動既有桌布。

## 架構

- `SceneRuntime/` 包含原生套件解析、模型、腳本、shader、frame graph、音訊與 Metal 執行管線。
- App 層負責按螢幕管理場景 session、屬性保存、播放策略、macOS 音訊擷取和「正在播放」輸入。
- 創意工坊瀏覽使用 Steam Web API 探索內容，並透過 `steamcmd` 完成驗證與下載。
