Open Wallpaper Engine (Patched)
=========

**English** | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md)

[![GitHub license](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

A patched fork of [Open Wallpaper Engine](https://github.com/MrWindDog/wallpaper-engine-mac) for macOS, adding scene wallpaper rendering and web wallpaper fixes.

> **Note:** This is NOT affiliated with the commercial Wallpaper Engine on Steam. This is an open-source macOS app that can display wallpaper assets from Wallpaper Engine's Steam Workshop.

## Related Projects

- **[Open Wallpaper Engine for Linux](https://github.com/Unayung/simple-linux-wallpaperengine-gui)** — A PyQt6 GUI for [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine), with Steam Workshop integration and UI design ported from this macOS version.

## Credits

This project is built on top of the work of:

- **[MrWindDog](https://github.com/MrWindDog)** — Maintainer of the upstream [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) fork, added new features and UI refinements
- **[Haren Chen](https://github.com/haren724)** — Original creator of [open-wallpaper-engine-mac](https://github.com/haren724/open-wallpaper-engine-mac), built the core app architecture (SwiftUI, video wallpaper playback, import system, playlist UI)
- **[1ris_W](https://github.com/Erica-Iris)** — Chinese i18n translation
- **[Klaus Zhu](https://github.com/klauszhu1105)** — App logo icons
- **[Chen Chia Yang](https://github.com/Unayung)** — Scene wallpaper rendering, web wallpaper fixes, Steam Workshop integration, multi-display support, zip import

Licensed under [GPL-3.0](LICENSE), same as the original project.

## What's New in 0.8.0

### Multi-Display Support
Assign different wallpapers to each connected monitor with per-screen enable/disable control.
- **Display Settings panel** — Visual monitor layout showing all connected screens, click to select
- **Per-screen wallpaper** — Each display can show a different wallpaper independently
- **Enable/disable toggle** — Turn wallpaper on or off per monitor
- **Auto-detect** — New monitors are automatically detected and enabled when connected

### Multi-Desktop Support
Wallpapers now display across all macOS desktops (Spaces) with continuous playback — no interruption when switching desktops.

### Recent Wallpapers Menu
Quickly switch wallpapers from the status bar menu. The last 10 wallpapers you've used are listed for one-click access.

### Playback Settings — Fixed
Performance playback settings (pause/mute/stop when other apps are focused) now work correctly for all wallpaper types.

### Steam Workshop Browser
Browse, search, and download wallpapers directly from the Steam Workshop without leaving the app.
- **Search & filter** — Search by name, filter by content rating (Everyone/Questionable/Mature), type (Scene/Video/Web), and genre tags
- **Sort options** — Trending, Most Recent, Most Popular, Most Subscribed
- **steamcmd integration** — Auto-detects steamcmd (Homebrew or custom path), with install instructions if not found
- **Steam login** — Supports password, Steam Guard, and cached session authentication
- **Download with progress** — Real-time status updates during download (authenticating, downloading %, validating, copying)
- **Safe defaults** — Content rating defaults to "Everyone" to filter out mature content

### Zip Import
Import wallpaper packages directly from `.zip` files — no need to manually extract first. Works via File > Import and drag-and-drop.

### Multi-Select & Batch Unsubscribe
Cmd+click to select multiple wallpapers, then right-click to batch unsubscribe.

### Wallpaper Storage Isolation
Wallpapers are now stored in `~/Documents/Open Wallpaper Engine/` instead of the raw Documents directory, preventing "error" wallpapers when cloning the repo on a fresh machine.

## What's Patched

### Web Wallpapers — Fixed gray/blank rendering
WebGL-based wallpapers rendered as gray rectangles because `WKWebView` blocked local file access for textures and assets.

**Fix:** Enabled `allowFileAccessFromFileURLs` and `allowUniversalAccessFromFileURLs` on the WKWebView configuration, allowing WebGL shaders to load local texture files.

### Scene Wallpapers — Implemented from scratch
Scene wallpapers (the most common type on Steam Workshop) were completely unimplemented — just showed "Hello, World!".

**New implementation includes:**
- **Native scene runtime** — Parses PKGV/TEXV assets, scene models, materials, shaders, and scripts through one C++ runtime
- **OpenGL renderer** — Executes coherent frame graphs and presents them directly in an `NSOpenGLView` without CPU readback
- **Wallpaper Engine assets directory** — Uses the official shader and material assets selected in General settings
- **Explicit failures** — Invalid or unsupported scenes are cleared and reported instead of silently showing a stale frame or preview

### Import — Fixed folder import
The import panel now correctly handles both individual wallpaper folders and parent directories containing multiple wallpapers.

## Current Limitations

- **Particle compatibility** — The deterministic sprite path supports box/sphere emitters, common random initializers, movement, and alpha fading. Trails/ropes, child and control-point systems, collision/boids, animated particle textures, and non-`genericparticle` multi-pass materials are not supported yet.
- **Audio-reactive scenes** — Authored sound playback is supported, but macOS system-audio capture and Wallpaper Engine audio-input buffers are unavailable and fail explicitly.
- **Text compatibility** — Text layers render, but non-zero character spacing and advanced row/width/ellipsis layout are not supported yet.
- **Effect compatibility** — Image materials, passthrough scene-composite layers, multi-pass effects, framebuffer bindings, copy/swap/clear operations, camera parallax, and automatic projection are supported. Compose, puppet meshes, and other unmodeled effect features fail explicitly.
- **User properties** — Boolean, slider, combo, color, and text-input values are applied live and persisted per display and Scene. Scene-texture, file, directory, and shortcut properties are currently visible but read-only.

## Supported Wallpaper Types

| Type | Status |
|------|--------|
| Video (.mp4, .webm) | Working (original) |
| Web (HTML/WebGL) | Working (patched) |
| Scene (images, effects, scripts) | Partial |
| Scene (particles, text, audio) | Partial |
| Application | Not supported |

## Build from Source

### Prerequisites
- macOS >= 13.0
- Xcode >= 14.4
- Xcode Command Line Tools

### Steps
```sh
git clone https://github.com/unayung/wallpaper-engine-mac
cd wallpaper-engine-mac
open "Open Wallpaper Engine.xcodeproj"
```

In Xcode, change the signing certificate to your own or select "Sign to Run Locally", then press `Cmd + R` to build and run.

## Usage

### Browse & Download from Steam Workshop

1. Install steamcmd (`brew install steamcmd`) or point the app to an existing binary
2. Switch to the **Workshop** tab and log in with your Steam account (must own Wallpaper Engine)
3. Enter a [Steam Web API key](https://steamcommunity.com/dev/apikey) when prompted
4. Search, filter, and click **Download** on any wallpaper

### Import from Local Files

- **Folder:** File > Import from Folder — select wallpaper folders containing `project.json`
- **Zip:** File > Import or drag-and-drop a `.zip` file containing wallpaper packages
- **Manual:** Copy wallpaper folders directly into `~/Documents/Open Wallpaper Engine/`

## Files Changed (vs upstream)

**Modified:**
- `WebWallpaperView.swift` — WKWebView file access configuration
- `WallpaperView.swift` — Scene wallpaper dispatch
- `SceneWallpaperView.swift` — Uses an NSOpenGLView backed by the native SceneRuntime
- `ImportPanels.swift` — Folder import logic fix

**Added:**
- `SceneRuntime/` — Native package parsing, model, script, frame-graph, shader, and OpenGL execution pipeline
- `Services/SceneWallpaperViewModel.swift` — App-facing owner for native SceneRuntime sessions
- `Services/SteamCmdService.swift` — steamcmd detection, login, and workshop download
- `Services/WorkshopAPIService.swift` — Steam Web API client for workshop browsing
- `Services/WorkshopViewModel.swift` — Workshop browser state management
- `Services/WallpaperDirectory.swift` — Centralized wallpaper storage path
- `Services/ZipImporter.swift` — Zip file extraction and import
- `ContentView/Components/WorkshopView.swift` — Workshop browser UI
