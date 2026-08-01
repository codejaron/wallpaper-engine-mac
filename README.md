Open Wallpaper Engine (Patched)
=========

**English** | [简体中文](README.zh-CN.md) | [繁體中文](README.zh-TW.md) | [日本語](README.ja.md)

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

## What's New in 0.8.1

### Scene Runtime Compatibility
Scene wallpapers now run through a native C++/OpenGL pipeline that follows the Linux Wallpaper Engine runtime contract much more closely. It handles Wallpaper Engine packages, models, shaders, scripts, frame graphs, and textures without a CPU framebuffer readback.

- **Scene layers and effects** — Supports group layers, image materials, multi-pass effects, framebuffer operations, scene composition, cursor interaction, camera parallax, and automatic projection.
- **Text, media, and video** — Renders embedded and system fonts with authored layout and text effects; supports video textures, authored scene audio, and macOS Now Playing metadata/artwork for compatible scripts.
- **Audio-reactive scenes** — Optional Core Audio system capture supplies spectrum data to scenes that request Wallpaper Engine audio input.
- **Faithful presentation** — Preserves authored coordinate orientation and aspect ratio, with automatic, stretch, fit, and fill scaling options. Scene wallpapers can also span multiple displays on one virtual canvas.
- **Lower rendering overhead** — Framebuffer planning reuses compatible targets and avoids unnecessary framebuffer work.

Most Scene wallpapers need the official Wallpaper Engine `assets` directory. The assets are proprietary and are not bundled with this project; see [Configure Wallpaper Engine assets](#configure-wallpaper-engine-assets) for the supported way to provide them.

### Scene Property Controls
The wallpaper details view exposes Scene user properties from the runtime. Boolean, slider, combo, color, and text-input properties update a running Scene immediately and are persisted independently for each display and wallpaper. Property descriptions support formatted text, links, and remote images; grouped properties and reset-to-default are also available.

### Playback and Audio Policies
Playback settings now use one policy across video, web, and Scene wallpapers. Configure whether to keep running, mute, pause, or stop when another app is focused, fullscreen or maximized, playing audio, or when a laptop is on battery. When multiple conditions apply, the strictest action wins. Display sleep always pauses playback and restores it on wake.

The **Other Application Playing Audio** rule and audio-reactive Scenes require **System Audio Capture** in General settings. Capture errors are surfaced in the app instead of silently disabling the rule.

### Workshop Browsing and Previews
The Workshop browser now filters by content rating, wallpaper type, and genre in addition to search and sort. Preview loading was also reworked for remote images and animated GIFs, so search results and wallpaper cards update reliably while content loads.

### Scene Quality Controls
Scene-only performance settings now include render quality, FPS, presentation scaling, and multi-display spanning. Unsupported anti-aliasing, post-processing, texture-resolution, and reflection controls are visibly disabled rather than suggesting that they work.

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
- **Effect compatibility** — Scene composition and the common image/effect paths are supported, but some advanced compose, puppet-mesh, and unmodeled effect features still cannot be rendered.
- **User properties** — Boolean, slider, combo, color, and text-input values are editable. Scene-texture, file, directory, and shortcut properties are visible but read-only.
- **Platform-dependent inputs** — System-audio capture and Now Playing integration depend on macOS services. If either service is unavailable, the app reports the failure and the affected Scene input is unavailable.

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

### Configure Wallpaper Engine assets

Most Scene wallpapers rely on the original Wallpaper Engine shaders and materials. These proprietary files are not included in this repository or the app.

1. Use a Steam account that owns Wallpaper Engine and install it through Steam on Windows, or use an existing installation from that account.
2. In Steam, open **Wallpaper Engine > Manage > Browse local files**. In a default Steam library, the application directory is `C:\Program Files (x86)\Steam\steamapps\common\wallpaper_engine`; the required folder is its `assets` subdirectory.
3. Copy the `assets` directory to a stable, readable location on the Mac. In this app, open **Settings > General > Wallpaper Engine assets** and select either that `assets` directory or its `wallpaper_engine` parent directory. The selected directory must contain `shaders/`.

Do not add, commit, or redistribute these files with this project. Steam library paths can differ; Wallpaper Engine's [official support documentation](https://help.wallpaperengine.io/en/steam/contentfile.html) uses the same `steamapps/common/wallpaper_engine` installation layout.

### Import from Local Files

- **Folder:** File > Import from Folder — select wallpaper folders containing `project.json`
- **Zip:** File > Import or drag-and-drop a `.zip` file containing wallpaper packages
- **Storage location:** In **Settings > General > Wallpaper Storage**, choose where downloads and imports are saved. The default is `~/Documents/Open Wallpaper Engine/`; changing it does not move existing wallpapers.

## Architecture

- `SceneRuntime/` contains the native package, model, script, shader, frame-graph, audio, and OpenGL execution pipeline.
- The app layer owns per-display Scene sessions, property persistence, playback policy, macOS audio capture, and Now Playing input.
- Workshop browsing uses the Steam Web API for discovery and `steamcmd` for authentication and downloads.
