Open Wallpaper Engine（修补版）
=========

[English](README.md) | **简体中文** | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md)

[![GitHub license](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

基于 [Open Wallpaper Engine](https://github.com/MrWindDog/wallpaper-engine-mac) 的 macOS 修补分支，提供场景壁纸渲染、网页壁纸修复，以及 Steam 创意工坊集成。

> **注意：** 本项目与 Steam 上的商业版 Wallpaper Engine 没有关联。这是一个可以显示 Wallpaper Engine Steam 创意工坊壁纸资源的开源 macOS 应用。

## 相关项目

- **[Open Wallpaper Engine for Linux](https://github.com/Unayung/simple-linux-wallpaperengine-gui)** — 基于 [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) 的 PyQt6 图形界面；Steam 创意工坊集成与 UI 设计移植自本 macOS 版本。

## 致谢

本项目建立在以下贡献者的工作之上：

- **[MrWindDog](https://github.com/MrWindDog)** — 上游 [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) 分支维护者，贡献新功能和 UI 优化
- **[Haren Chen](https://github.com/haren724)** — [open-wallpaper-engine-mac](https://github.com/haren724/open-wallpaper-engine-mac) 原作者，构建了核心架构（SwiftUI、视频壁纸播放、导入系统和播放列表 UI）
- **[1ris_W](https://github.com/Erica-Iris)** — 中文本地化翻译
- **[Klaus Zhu](https://github.com/klauszhu1105)** — 应用图标
- **[Chen Chia Yang](https://github.com/Unayung)** — 场景壁纸渲染、网页壁纸修复、Steam 创意工坊集成、多显示器支持和 Zip 导入

采用 [GPL-3.0](LICENSE) 许可证，与原始项目一致。

## 0.8.1 新功能

### 场景运行时兼容性

场景壁纸现在通过原生 C++/OpenGL 管线运行，更贴近 Linux Wallpaper Engine runtime 的行为约定。它可处理 Wallpaper Engine 包、模型、着色器、脚本、帧图和纹理，无需把帧缓冲读回 CPU。

- **场景图层与效果** — 支持分组图层、图片材质、多 pass 效果、帧缓冲操作、场景合成、光标交互、相机视差和自动投影。
- **文字、媒体与视频** — 原生渲染嵌入字体和系统字体，并支持作者设定的排版及文字效果；兼容视频纹理、场景内置音频，以及可用脚本读取的 macOS“正在播放”媒体信息和封面。
- **音频响应场景** — 可选的 Core Audio 系统音频采集为需要 Wallpaper Engine 音频输入的场景提供频谱数据。
- **更准确的呈现** — 保持作者坐标系方向与画面比例，支持自动、拉伸、适应和填充缩放；场景壁纸还可使用一张虚拟画布跨越多个显示器。
- **更低的渲染开销** — 帧缓冲规划会复用兼容目标，并避免不必要的帧缓冲操作。

大多数场景壁纸还需要官方 Wallpaper Engine 的 `assets` 目录。这些资源受专有许可保护，不会随本项目分发；请参阅[配置 Wallpaper Engine assets](#配置-wallpaper-engine-assets) 了解合规的获取方式。

### 场景属性控制

壁纸详情页会展示 runtime 提供的场景用户属性。布尔值、滑块、下拉选项、颜色和文本输入可在场景运行时立即生效，并按显示器与壁纸分别保存。属性说明支持富文本、链接和远程图片，也支持属性分组及恢复默认值。

### 播放与音频策略

播放设置现通过一套策略统一作用于视频、网页和场景壁纸。可设置在其他应用获得焦点、全屏或最大化、正在播放音频，或笔记本使用电池时继续运行、静音、暂停或停止。多个条件同时满足时，以限制最严格的操作为准。显示器休眠始终暂停播放，并会在唤醒后恢复。

“其他应用正在播放音频”规则和音频响应场景都需要在通用设置中开启**系统音频采集**。采集失败会在应用中显示原因，而不会悄悄停用规则。

### 创意工坊浏览与预览

创意工坊浏览器除搜索和排序外，现可按内容分级、壁纸类型和题材筛选。远程图片和 GIF 预览的加载逻辑也已重构，搜索结果与壁纸卡片会在内容加载时稳定更新。

### 场景质量控制

场景专用的性能设置新增渲染质量、FPS、呈现缩放和跨显示器模式。不支持的抗锯齿、后处理、纹理分辨率和反射选项会明确禁用，不再造成可用的误解。

## 0.8.0 新功能

### 多显示器支持

可为每台已连接显示器指定不同壁纸，并分别启用或停用。

- **显示器设置面板** — 以可视化布局展示所有已连接显示器，点击即可选择
- **每屏独立壁纸** — 每台显示器可独立显示不同壁纸
- **启用/停用开关** — 可针对每台显示器开启或关闭壁纸
- **自动检测** — 新连接的显示器会自动被发现并启用

### 多桌面支持

壁纸可显示在所有 macOS 桌面（Spaces）并持续播放，切换桌面时不会中断。

### 最近使用壁纸菜单

可从菜单栏快速切换壁纸；最近使用的 10 个壁纸支持一键访问。

### 播放设置修复

性能播放设置（在其他应用获得焦点时暂停、静音或停止）现可正确用于所有壁纸类型。

### Steam 创意工坊浏览器

无需离开应用，即可浏览、搜索和下载 Steam 创意工坊壁纸。

- **搜索与筛选** — 可按名称搜索，并按内容分级（Everyone/Questionable/Mature）、类型（Scene/Video/Web）及题材标签筛选
- **排序选项** — 热门趋势、最新发布、最受欢迎、订阅最多
- **steamcmd 集成** — 自动检测 Homebrew 或自定义路径中的 steamcmd；未安装时提供安装说明
- **Steam 登录** — 支持密码、Steam Guard 和已缓存的会话认证
- **下载进度** — 实时显示认证、下载百分比、验证和复制等状态
- **安全默认值** — 内容分级默认设为“Everyone”，过滤成人内容

### Zip 导入

可直接导入 `.zip` 壁纸包，无需手动解压；支持“文件 > 导入”和拖放操作。

### 多选与批量取消订阅

使用 Cmd+单击选择多个壁纸，再通过右键菜单批量取消订阅。

### 壁纸存储隔离

壁纸保存在 `~/Documents/Open Wallpaper Engine/`，而不是 Documents 根目录，避免在全新机器克隆仓库后出现“error”壁纸。

## 修补内容

### 网页壁纸：修复灰色或空白渲染

基于 WebGL 的壁纸会因为 `WKWebView` 阻止本地文件访问而呈现灰色矩形。

**修复方式：** 在 WKWebView 配置中启用 `allowFileAccessFromFileURLs` 和 `allowUniversalAccessFromFileURLs`，让 WebGL 着色器能够加载本地纹理文件。

### 场景壁纸：从零实现

场景壁纸（Steam 创意工坊中最常见的类型）此前完全没有实现，只会显示“Hello, World!”。

**当前实现包括：**

- **原生 SceneRuntime** — 通过一个 C++ runtime 解析 PKGV/TEXV、场景模型、材质、着色器和脚本
- **OpenGL 渲染器** — 执行连贯的帧图，并直接在 `NSOpenGLView` 呈现，无 CPU readback
- **Wallpaper Engine assets 目录** — 使用在通用设置中选择的官方着色器与材质资源
- **明确失败** — 无效或不支持的场景会清空画面并报告错误，不会静默保留旧帧或预览图

### 导入：修复文件夹导入

导入面板现可正确处理单个壁纸文件夹，以及包含多个壁纸的父目录。

## 当前限制

- **粒子兼容性** — 确定性精灵路径支持 box/sphere 发射器、常用随机初始化器、移动和 alpha 渐隐；尚不支持轨迹/绳索、子系统与控制点、碰撞/boids、动态粒子纹理及非 `genericparticle` 的多 pass 材质。
- **效果兼容性** — 已支持常用场景合成以及图像/效果路径，但部分高级 compose、puppet mesh 和尚未建模的效果功能仍无法渲染。
- **用户属性** — 布尔值、滑块、下拉选项、颜色和文本输入可编辑；场景纹理、文件、目录和快捷键属性目前仅可查看，不能安全编辑。
- **平台相关输入** — 系统音频采集和“正在播放”集成依赖 macOS 服务。服务不可用时，应用会报告失败，相应的场景输入也不可用。

## 支持的壁纸类型

| 类型 | 状态 |
|------|------|
| 视频（.mp4、.webm） | 正常工作（原始功能） |
| 网页（HTML/WebGL） | 正常工作（已修补） |
| 场景（图片、效果、脚本） | 部分支持 |
| 场景（粒子、文字、音频） | 部分支持 |
| 应用程序 | 不支持 |

## 从源代码构建

### 前置条件

- macOS >= 13.0
- Xcode >= 14.4
- Xcode Command Line Tools

### 步骤

```sh
git clone https://github.com/unayung/wallpaper-engine-mac
cd wallpaper-engine-mac
open "Open Wallpaper Engine.xcodeproj"
```

在 Xcode 中将签名证书改为自己的证书，或选择“Sign to Run Locally”，然后按 `Cmd + R` 构建并运行。

## 使用方法

### 浏览并下载 Steam 创意工坊壁纸

1. 安装 steamcmd（`brew install steamcmd`），或在应用中指定已有二进制文件。
2. 打开 **创意工坊** 标签页并登录 Steam 账户（账户必须拥有 Wallpaper Engine）。
3. 按提示输入 [Steam Web API key](https://steamcommunity.com/dev/apikey)。
4. 搜索、筛选，然后点击任意壁纸的 **下载**。

### 配置 Wallpaper Engine assets

大多数场景壁纸依赖原版 Wallpaper Engine 的着色器和材质。这些专有文件不包含在本仓库或应用中。

1. 使用已拥有 Wallpaper Engine 的 Steam 账户，在 Windows 上通过 Steam 安装它；也可以使用该账户已有的安装目录。
2. 在 Steam 中打开 **Wallpaper Engine > 管理 > 浏览本地文件**。默认 Steam 库中的应用目录为 `C:\Program Files (x86)\Steam\steamapps\common\wallpaper_engine`，所需目录是其中的 `assets`。
3. 将 `assets` 目录复制到 Mac 上稳定且可读的位置。在本应用打开 **设置 > 通用 > Wallpaper Engine assets**，选择该 `assets` 目录，或选择它的 `wallpaper_engine` 父目录。所选目录必须包含 `shaders/`。

请勿将这些文件加入仓库、提交或重新分发。Steam 库路径可能不同；Wallpaper Engine 的[官方支持文档](https://help.wallpaperengine.io/en/steam/contentfile.html)采用相同的 `steamapps/common/wallpaper_engine` 安装目录结构。

### 从本地文件导入

- **文件夹：** 选择“文件 > 从文件夹导入”，然后选择包含 `project.json` 的壁纸文件夹。
- **Zip：** 选择“文件 > 导入”，或将包含壁纸包的 `.zip` 文件拖入应用。
- **存储位置：** 在 **设置 > 通用 > 壁纸存储位置** 选择下载和导入的保存目录。默认目录为 `~/Documents/Open Wallpaper Engine/`；更改目录不会移动现有壁纸。

## 架构

- `SceneRuntime/` 包含原生包解析、模型、脚本、着色器、帧图、音频和 OpenGL 执行管线。
- 应用层负责按显示器管理场景会话、属性持久化、播放策略、macOS 音频采集和“正在播放”输入。
- 创意工坊浏览使用 Steam Web API 发现内容，并用 `steamcmd` 完成认证和下载。
