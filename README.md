# ForceComposedFlip

Forces the Windows Desktop Window Manager (DWM) to use **Composed Flip** presentation instead of **Independent Flip**, enabling screen capture APIs (DXGI Desktop Duplication and Windows.Graphics.Capture) to capture **DLSS Frame Generation interpolated frames** when streaming with [Sunshine](https://github.com/LizardByte/Sunshine) + [Moonlight](https://github.com/moonlight-stream).

This solves the long-standing issue where only "real" rendered frames are captured during streaming, while Frame Generation interpolated frames are invisible to the stream — effectively halving the framerate benefit of FG for the client.

**Related issue:** [LizardByte/Sunshine#3621 — DLSS frame generation versions 310.1 and 310.2 only streaming native frame rate (1/2) from host](https://github.com/LizardByte/Sunshine/issues/3621)

## Table of Contents

- [The Problem](#the-problem)
- [The Solution](#the-solution)
  - [1. Disable Multiplane Overlay (MPO)](#1-disable-multiplane-overlay-mpo)
  - [2. Run ForceComposedFlip](#2-run-forcecomposedflip)
- [Quick Start](#quick-start)
- [Capture Method Compatibility](#capture-method-compatibility)
- [Game Mode Compatibility](#game-mode-compatibility)
- [Tray Menu Options](#tray-menu-options)
- [Performance Impact](#performance-impact)
  - [Comparison with Virtual Display Driver (VDD)](#comparison-with-virtual-display-driver-vdd)
- [How It Was Discovered](#how-it-was-discovered)
- [A Note for Sunshine Developers](#a-note-for-sunshine-developers)
- [Tested Configuration](#tested-configuration)
- [Download](#download)
- [License](#license)

## The Problem

DLSS Frame Generation v310.x+ (shipped with RTX 50 series and backported to RTX 40 series via driver updates) changed how interpolated frames are presented. NVIDIA introduced **hardware-level flip metering** on the GPU, which causes interpolated frames to be inserted via **Independent Flip / Direct Flip** — bypassing the Desktop Window Manager (DWM) compositor entirely and going straight to the display scanout.

Since both DXGI Desktop Duplication and WGC capture from the DWM compositor, they only see frames that pass through composition. The "real" rendered frames pass through DWM, but the interpolated FG frames go directly to the monitor. This is why you see exactly half the expected framerate on the Moonlight client when FG is enabled.

## The Solution

The fix has two components that **must both be applied**:

### 1. Disable Multiplane Overlay (MPO)

MPO allows the DWM to use hardware overlay planes to avoid full composition. Disabling it forces all composition through software, ensuring FG frames pass through the compositor.

The easiest way is from the **ForceComposedFlip tray menu**: right-click the gamepad icon → "Disable MPO". The program will ask for admin elevation, apply the registry change, and then offer to restart DWM immediately so the change takes effect without a full reboot.

To revert, use "Enable MPO" from the same tray menu. It will remove the registry key and offer to restart DWM in the same way.

If you prefer to do it manually:

Disable MPO:
```
reg add "HKLM\SOFTWARE\Microsoft\Windows\Dwm" /v OverlayTestMode /t REG_DWORD /d 5 /f
```

Enable MPO (revert):
```
reg delete "HKLM\SOFTWARE\Microsoft\Windows\Dwm" /v OverlayTestMode /f
```

In both cases, a DWM restart is required for the change to take effect. Run `taskkill /f /im dwm.exe` as administrator — the screen will flash briefly as DWM restarts automatically and re-reads the registry value. If that doesn't work, do a full reboot.

### 2. Run ForceComposedFlip

Even with MPO disabled, the DWM will still use Independent Flip if there's nothing to composite on top of the game. ForceComposedFlip creates a tiny (1x1 pixel), invisible, click-through window that stays on top of all other windows. This forces the DWM to maintain Composed Flip mode because it must composite this overlay window on every frame.

The window is re-asserted as topmost every 500ms via the Win32 `SetWindowPos` API, which is critical for games running under Fullscreen Optimizations (FSO) — these games sit on a higher z-band than normal windows, so periodic re-assertion ensures the DWM maintains composition.

## Quick Start

1. Download `ForceComposedFlip.exe` from [Releases](../../releases).
2. Run it. A gamepad icon will appear in the system tray. That's it — leave it running.
3. Right-click the tray icon → "Disable MPO" if you haven't already. Accept the admin permission prompt and confirm the DWM restart when asked (one-time setup, persists across reboots).
4. Launch your game in **Borderless Windowed** or **Fullscreen** mode.
5. All Frame Generation frames will now be captured by Sunshine.

## Capture Method Compatibility

| Capture Method | FG Frames Captured | NVIDIA Overlay | RTSS Overlay |
|---|---|---|---|
| WGC (Windows.Graphics.Capture) | ✅ Yes | ✅ Yes (metrics work correctly) | ✅ Yes |
| DXGI Desktop Duplication | ✅ Yes | ⚠️ Partial (overlay may show incorrect metrics) | ✅ Yes |

**Recommendation:** Use WGC for best results. WGC operates at the compositor level and doesn't interfere with the NVIDIA overlay's injection into the game process. DXGI DD competes with the overlay for framebuffer access, causing metric display issues. RTSS works correctly with both methods.

Note: WGC is available in Sunshine's latest portable builds and requires running Sunshine manually (not as a service).

## Game Mode Compatibility

| Game Mode | Works? | Notes |
|---|---|---|
| Borderless Windowed | ✅ Yes | Works perfectly |
| Fullscreen (with FSO, default) | ✅ Yes | Works perfectly |
| True Exclusive Fullscreen | ❌ No | DWM is completely disabled; no solution possible |

Both Borderless Windowed and Fullscreen (FSO) work identically with this solution. There is **no performance penalty** for using either mode. In practice, virtually all modern games on Windows 11 use FSO by default even when set to "Fullscreen" / "Exclusive Fullscreen" in their settings. If Sunshine can capture the game at all (no black screen), it's using FSO, and this solution will work.

## Tray Menu Options

Right-click the gamepad icon in the system tray:

- **Recreate overlay now** — Manually destroys and recreates the invisible window. Normally not needed as the script auto-detects foreground window changes.
- **Disable MPO (add registry key)** — Adds the MPO disable registry key. Prompts for admin permission. Asks if you want to restart DWM immediately to apply without rebooting.
- **Enable MPO (remove registry key)** — Removes the registry key. Same admin and DWM restart prompts.
- **Exit** — Closes the script and removes the overlay window.

## Performance Impact

In a Sunshine/Moonlight streaming context, the performance impact of this solution is effectively **zero**:

- **MPO disabled:** The only known side effect is that VRR/G-Sync may not work correctly with video streaming apps (Netflix, Disney+, etc.) on the host's physical monitor. However, in a streaming setup, the host monitor is irrelevant — it only serves as an anchor for the GPU to render. VRR/G-Sync is applied on the client device running Moonlight, not on the host display. If your host PC is dedicated to streaming, this has zero practical impact.
- **Transparent window:** The 1x1 pixel window compositing is essentially free. The SetWindowPos call every 500ms has no measurable CPU or GPU cost.
- **Composed Flip vs Independent Flip:** Adds < 1ms of presentation latency on the host's local display. Irrelevant for streaming — the latency that matters is the end-to-end encode/network/decode pipeline to the Moonlight client.

### Comparison with Virtual Display Driver (VDD)

Until discovering this solution, I personally worked around this issue by using a Virtual Display Driver ([MikeTheTech's VDD](https://github.com/VirtualDrivers/Virtual-Display-Driver)), which inherently forces DWM composition and captures all FG frames without any additional hacks. VDD is an excellent solution and works reliably. Here's how the two approaches compare:

| | ForceComposedFlip | Virtual Display Driver (VDD) |
|---|---|---|
| **Setup** | Run an exe + one-time registry change | Install driver + configure resolutions/refresh rates |
| **FG frames captured** | ✅ Yes | ✅ Yes |
| **NVIDIA overlay (Alt+R)** | ✅ Works | ❌ Shows N/A (NVIDIA doesn't recognize IDD displays) |
| **RTSS overlay** | ✅ Works | ✅ Works |
| **Gaming performance** | Identical | Identical |
| **Requires physical monitor or dummy plug** | Yes | No |
| **Additional software running** | Small background process | None (driver-level) |

Both approaches are equally valid. If you don't need the NVIDIA overlay and prefer not to have a physical monitor or dummy plug connected, VDD is a perfectly valid and clean solution. If you want NVIDIA overlay support or prefer a simpler setup with no driver installation, ForceComposedFlip is the way to go.

## How It Was Discovered

The investigation started with a different goal: getting the NVIDIA performance overlay (Alt+R) to work on a Virtual Display Driver, which was being used because VDD correctly captures FG frames. The NVIDIA overlay doesn't work on VDD because NVIDIA doesn't recognize IDD (Indirect Display Driver) displays.

While testing various configurations, I noticed that having the NVIDIA overlay open but NOT hooked into the game process (showing "N/A" for FPS/latency metrics) caused FG frames to suddenly appear in the Sunshine stream on a real monitor. Conversely, when the overlay successfully hooked into the game (showing real metrics), FG frames disappeared.

This led to the realization that the NVIDIA overlay, when present as an unhooked window, was acting as an overlay that forced DWM Composed Flip — exactly the same principle as the transparent window in this script. The overlay, when hooked into the game's swapchain, changed the presentation mode in a way that broke FG frame capture.

From there, it was a matter of isolating the two requirements (MPO disabled + topmost window) and making it work reliably across game modes including FSO.

## A Note for Sunshine Developers

Given the zero computational cost of this approach (a 1x1 pixel window + a SetWindowPos call every 500ms) and the long-standing nature of this problem affecting every user who streams with Frame Generation on RTX 40/50 series, I wonder if this solution could be integrated natively into Sunshine itself. Sunshine already manages display configuration, virtual displays, and capture methods — adding an option to force Composed Flip via a topmost window when FG capture is desired seems like a natural fit. Just a thought.

## Tested Configuration

- **GPU:** NVIDIA RTX 5080
- **OS:** Windows 11 24H2
- **Sunshine:** Latest portable build with WGC capture
- **Client:** Moonlight
- **Games tested:** Final Fantasy XVI (Xbox/MS Store), Final Fantasy VII Rebirth (Steam), Spider-Man Remastered (Steam)
- **FG modes tested:** DLSS Frame Generation (1x), Multi Frame Generation (up to 4x)

## Download

Download the latest release from the [Releases](../../releases) page:

- **`ForceComposedFlip.exe`** — Pre-compiled, ready to run. No dependencies needed.
- **`ForceComposedFlip.ahk`** — Source code. Requires [AutoHotkey v2](https://www.autohotkey.com/) if you want to run or modify it directly.

## License

This project is licensed under the [GNU General Public License v3 or later (GPLv3+)](https://choosealicense.com/licenses/gpl-3.0/).

The application icon is ["libre-device-controller"](https://github.com/DennisSuitters/LibreICONS/blob/master/svg-color/libre-device-controller.svg) from [LibreICONS](https://github.com/DennisSuitters/LibreICONS) by [Dennis Suitters](https://github.com/DennisSuitters), licensed under [MIT](https://opensource.org/licenses/MIT).
