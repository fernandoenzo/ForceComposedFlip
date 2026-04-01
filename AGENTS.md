# AGENTS.md

## Project Overview

**ForceComposedFlip** is a lightweight Windows utility that forces the Desktop Window Manager (DWM) to use Composed Flip instead of Independent Flip. This makes DLSS Frame Generation interpolated frames visible to screen capture APIs (DXGI Desktop Duplication and Windows.Graphics.Capture), solving a streaming problem with Sunshine/Moonlight where interpolated frames are invisible to the stream.

## How It Works

Two mechanisms work together:

1. **Invisible overlay window** — A 1x1 pixel, near-invisible (opacity 1/255), click-through, topmost window that forces DWM to composite every frame.
2. **MPO toggle** — A registry key (`HKLM\SOFTWARE\Microsoft\Windows\Dwm\OverlayTestMode=5`) that disables Multiplane Overlay, preventing DWM from bypassing composition via hardware overlay planes.

## File Structure

```
ForceComposedFlip.ahk     # Original AutoHotkey v2 implementation (188 lines)
ForceComposedFlip.c       # Native Win32 C implementation (~640 lines)
ForceComposedFlip.rc      # Windows resource script (embeds the .ico into the .exe)
ForceComposedFlip.ico     # Application icon (gamepad icon from LibreICONS)
Makefile                  # Cross-compilation with MinGW-w64 from Linux
README.md                 # Full documentation
LICENSE                   # GPLv3
AGENTS.md                 # AI agent coding guidelines
.gitignore                # Ignores build artifacts (*.exe, *.o, *.res)
```

Both the `.ahk` and `.c` implementations are functionally equivalent and maintained in parallel.

## Building

Requires `mingw-w64` for cross-compilation from Linux:

```bash
# Install (Debian/Ubuntu)
sudo apt install mingw-w64

# Build
make

# Clean
make clean
```

This produces `ForceComposedFlip.exe`, a standalone Windows executable with the icon embedded as a resource.

## Key Win32 APIs Used

- `CreateWindowExW` with `WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED` — overlay window
- `SetLayeredWindowAttributes` — sets opacity to 1/255
- `SetWindowPos` with `HWND_TOPMOST` — re-asserts z-order every 500ms
- `GetForegroundWindow` — polls foreground window every 2000ms
- `Shell_NotifyIconW` — system tray icon and tooltip
- `ShellExecuteExW` with `"runas"` verb — UAC elevation for registry changes
- `WaitForSingleObject` / `CloseHandle` — waits for elevated processes
- `CreateMutexW` — enforces single instance

## Conventions

- All code comments and documentation must be written in **English**.
- The C code uses the Win32 Unicode API exclusively (all `W`-suffixed functions, `wchar_t` strings with `L"..."` prefix). The `-municode` compiler flag handles the `UNICODE` / `_UNICODE` defines.
- The project is intentionally single-file (one `.c` file). Do not split it into multiple source files.
- Comments use `/* C-style */` only (no `//`). Every function has a block comment describing its purpose and any non-obvious behavior.
- Comments should explain **why**, not just **what**. Every function has a block comment describing its purpose and any non-obvious behavior.
- Section headers use Unicode box-drawing characters: `/* ─── Section Name ──── */`
- Constants: `UPPER_SNAKE_CASE` via `#define`, grouped by category with prefixes (`TIMER_`, `IDM_`).
- Globals: `static g_camelCase`, all initialized to `NULL`/`0`/`FALSE`.
- Functions: `static PascalCase(void)`, all internal linkage.
- Local variables: `camelCase`.
- K&R brace style, 4-space indentation.
- No heap allocation — all stack locals and static globals.
- All git commits must be **signed** (`git commit -S`).
- Git tags must be **lightweight** (`git tag 1.2`), not annotated or signed.
- When elaborating plans with the user, ALWAYS save them to a temporary text file (e.g., `.opencode/plans/plan.md`) to ensure they survive conversation compressions. Iterate on this file throughout the conversation as the plan evolves. The AI agent has full permission to create, modify, and delete this file even in plan-only mode — this is the only exception to the read-only constraint. This file should never be tracked in the repository. After executing the plan, ALWAYS ask the user for permission to delete the file.

## Execution Protocol

**NEVER execute changes without explicit double confirmation:**

1. Always plan first, present to user, wait for approval
2. Ask if the user wants to execute the plan
3. If user confirms, ask once more for final confirmation before proceeding
4. **This applies even in Build Mode** — no exceptions

The user has final say on every action via double confirmation.

## Important Technical Details

- The overlay opacity must be exactly 1 (not 0). Fully transparent windows are optimized out by DWM and will not force Composed Flip.
- The `Sleep(500)` delay between overlay destroy and recreate (present in the AHK version) is implemented as a one-shot timer in the C version to avoid blocking the message loop.
- The `ReassertTopmost` timer (500ms) is critical — FSO games sit on a higher z-band than normal topmost windows, so periodic re-assertion is required.
- The `ExtractIconW` index is 0-based, while AHK's `TraySetIcon` index is 1-based. Keep this in mind if referencing icon indices from the AHK version.

## License

GPLv3+. The application icon is MIT-licensed by Dennis Suitters (LibreICONS). See README.md for full attribution.