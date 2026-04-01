/*
 * ForceComposedFlip.c
 *
 * Forces DWM to use Composed Flip instead of Independent Flip, making
 * DLSS Frame Generation interpolated frames visible to screen capture
 * APIs (Sunshine/Moonlight streaming).
 *
 * Two mechanisms work together:
 *   1. An invisible 1x1 pixel topmost overlay window that prevents DWM
 *      from using Independent Flip (the overlay forces composition).
 *   2. A registry toggle to disable Multiplane Overlay (MPO), which can
 *      otherwise bypass the composition chain even with the overlay present.
 *
 * See README.md for full documentation.
 * License: GPLv3
 */

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <wininet.h>

/* ─── Constants ─────────────────────────────────────────────────────── */

/* Timer IDs used with SetTimer/KillTimer */
#define TIMER_REASSERT_TOPMOST  1   /* Re-assert overlay z-order every 500ms */
#define TIMER_CHECK_FOREGROUND  2   /* Poll foreground window every 2000ms   */
#define TIMER_RECREATE_OVERLAY  3   /* One-shot 500ms delay for recreation   */
#define TIMER_CLOSE_BALLOON    4   /* One-shot 3500ms auto-close for balloon */

/* Timer intervals in milliseconds */
#define INTERVAL_REASSERT       500
#define INTERVAL_FOREGROUND     2000
#define INTERVAL_RECREATE       500
#define INTERVAL_BALLOON        3500

/* Custom window messages */
#define WM_TRAYICON             (WM_APP + 1)  /* System tray icon events  */
#define WM_UPDATE_CHECK_DONE    (WM_APP + 2)  /* Background update result */

/* Menu item IDs for the tray context menu */
#define IDM_TOGGLE_MPO          1001
#define IDM_AUTOSTART           1002
#define IDM_CHECK_UPDATE        1003
#define IDM_EXIT                1004

/* Registry key and value for auto-start with Windows */
#define AUTOSTART_KEY           L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTOSTART_VALUE         L"ForceComposedFlip"

/* Mutex name to enforce single instance */
#define MUTEX_NAME              L"ForceComposedFlip_SingleInstance"

/* Window class names */
#define CLASS_MAIN              L"ForceComposedFlipMain"
#define CLASS_OVERLAY           L"ForceComposedFlipOverlay"

/* SetWindowPos flags: SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE */
#define SWP_TOPMOST_FLAGS       (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)

/* ─── Update State ──────────────────────────────────────────────────── */

typedef enum {
    UPDATE_UNKNOWN,    /* No check has completed yet       */
    UPDATE_CHECKING,   /* Background thread is in progress */
    UPDATE_AVAILABLE,  /* Newer version found on GitHub    */
    UPDATE_LATEST      /* Running the latest version       */
} UpdateState;

/* ─── Global State ──────────────────────────────────────────────────── */

static HWND g_hwndMain      = NULL;  /* Hidden message window (timers, tray)  */
static HWND g_hwndOverlay   = NULL;  /* The invisible topmost overlay         */
static HWND g_lastForeground = NULL; /* Last known foreground window handle   */
static HICON g_hIcon        = NULL;  /* Tray icon handle (embedded resource)  */
static NOTIFYICONDATAW g_nid;        /* System tray icon data                 */
static UpdateState g_updateState = UPDATE_UNKNOWN;
static WCHAR g_updateVersion[32] = {0}; /* Remote version string from GitHub */

/* ─── Forward Declarations ──────────────────────────────────────────── */

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
static void CreateOverlay(void);
static void DestroyOverlay(void);
static void RecreateOverlay(void);
static void ReassertTopmost(void);
static void CheckForeground(void);
static void SetupTrayIcon(void);
static void RemoveTrayIcon(void);
static void ShowContextMenu(void);
static void UpdateTooltip(const WCHAR *text);
static void ShowBalloon(const WCHAR *title, const WCHAR *text);
static BOOL IsMpoDisabled(void);
static BOOL RunElevated(const WCHAR *params);
static void SetMPO(BOOL disable);
static BOOL IsAutoStartEnabled(void);
static void SetAutoStart(BOOL enable);
static DWORD WINAPI CheckUpdateThread(LPVOID lpParam);
static void CleanExit(void);

/* ─── Entry Point ───────────────────────────────────────────────────── */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /*
     * Enforce single instance: create a named mutex. If it already exists,
     * another instance is running — exit immediately.
     */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    /*
     * Register the main (hidden) window class.
     * This window never becomes visible — it exists solely to receive
     * timer messages (WM_TIMER) and tray icon events (WM_TRAYICON).
     */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = CLASS_MAIN;
    RegisterClassExW(&wc);

    /*
     * Register the overlay window class.
     * The overlay is a separate window with its own (minimal) WndProc.
     * It uses a black background brush so the 1x1 pixel is black
     * (irrelevant visually at 1/255 opacity, but required by the API).
     */
    WNDCLASSEXW wcOverlay = {0};
    wcOverlay.cbSize        = sizeof(wcOverlay);
    wcOverlay.lpfnWndProc   = OverlayWndProc;
    wcOverlay.hInstance      = hInstance;
    wcOverlay.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcOverlay.lpszClassName  = CLASS_OVERLAY;
    RegisterClassExW(&wcOverlay);

    /* Create the hidden message window */
    g_hwndMain = CreateWindowExW(
        0, CLASS_MAIN, L"ForceComposedFlip",
        WS_OVERLAPPEDWINDOW,
        0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwndMain) {
        CloseHandle(hMutex);
        return 1;
    }

    /* Set up the system tray icon with gamepad icon and tooltip */
    SetupTrayIcon();

    /* Silently check for updates in the background at startup */
    CreateThread(NULL, 0, CheckUpdateThread, (LPVOID)FALSE, 0, NULL);

    /* Create the overlay immediately */
    CreateOverlay();

    /*
     * Start the two recurring timers:
     *
     *   TIMER_REASSERT_TOPMOST (500ms):
     *     Periodically re-asserts the overlay as topmost. Critical because
     *     FSO (Fullscreen Optimizations) games sit on a higher z-band than
     *     normal HWND_TOPMOST windows, so periodic re-assertion ensures
     *     DWM keeps the overlay in the composition chain.
     *
     *   TIMER_CHECK_FOREGROUND (2000ms):
     *     Polls the foreground window and recreates the overlay when it
     *     changes (e.g., a game launches or regains focus), ensuring the
     *     overlay is above the new window in the z-order.
     */
    SetTimer(g_hwndMain, TIMER_REASSERT_TOPMOST, INTERVAL_REASSERT, NULL);
    SetTimer(g_hwndMain, TIMER_CHECK_FOREGROUND, INTERVAL_FOREGROUND, NULL);

    /*
     * Main message loop — the heart of any Win32 GUI application.
     * GetMessage blocks until a message arrives, then DispatchMessage
     * routes it to the appropriate WndProc. This loop runs until
     * PostQuitMessage is called (from CleanExit via WM_DESTROY).
     */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* Cleanup */
    CloseHandle(hMutex);
    return (int)msg.wParam;
}

/* ─── Main Window Procedure ─────────────────────────────────────────── */

/*
 * Handles all messages for the hidden main window:
 *   WM_TIMER:             Dispatches timer events to the appropriate handler
 *   WM_TRAYICON:          Responds to clicks on the system tray icon
 *   WM_UPDATE_CHECK_DONE: Processes the result of the background update check
 *   WM_COMMAND:           Handles context menu item selections
 *   WM_DESTROY:           Performs cleanup before the application exits
 */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam)
{
    switch (msg) {

    case WM_TIMER:
        switch (wParam) {
        case TIMER_REASSERT_TOPMOST:
            ReassertTopmost();
            break;
        case TIMER_CHECK_FOREGROUND:
            CheckForeground();
            break;
        case TIMER_RECREATE_OVERLAY:
            /*
             * One-shot timer: fired 500ms after DestroyOverlay to complete
             * the recreation sequence. Kill it immediately so it doesn't
             * repeat. See RecreateOverlay() for why this delay exists.
             */
            KillTimer(hwnd, TIMER_RECREATE_OVERLAY);
            CreateOverlay();
            UpdateTooltip(NULL); /* Update tooltip with current timestamp */
            break;
        case TIMER_CLOSE_BALLOON:
            /* One-shot timer: dismiss the balloon notification after 3500ms */
            KillTimer(hwnd, TIMER_CLOSE_BALLOON);
            g_nid.uFlags = NIF_INFO;
            g_nid.szInfo[0] = L'\0';
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            break;
        }
        return 0;

    case WM_TRAYICON:
        /*
         * The tray icon sends us this custom message when the user
         * interacts with it. lParam contains the actual mouse message.
         * We show the context menu on right-click.
         */
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            ShowContextMenu();
        }
        return 0;

    case WM_UPDATE_CHECK_DONE: {
        /*
         * Posted by the background CheckUpdateThread when it finishes.
         *   wParam: status (1=update available, 0=up to date, -1=error)
         *   lParam: isManual (TRUE if user triggered, FALSE if startup)
         *
         * On silent startup checks, only show a balloon if an update is
         * available. Manual checks always show feedback.
         */
        int status = (int)wParam;
        BOOL isManual = (BOOL)lParam;

        if (status == 1) {
            g_updateState = UPDATE_AVAILABLE;
            WCHAR msg[128];
            wsprintfW(msg, L"Update to %s available!", g_updateVersion);
            ShowBalloon(L"ForceComposedFlip Update", msg);
        } else if (status == 0) {
            g_updateState = UPDATE_LATEST;
            if (isManual)
                ShowBalloon(L"ForceComposedFlip Update",
                            L"ForceComposedFlip is up to date.");
        } else {
            g_updateState = UPDATE_UNKNOWN;
            if (isManual)
                ShowBalloon(L"ForceComposedFlip Update",
                            L"Failed to check for updates.");
        }
        return 0;
    }

    case WM_COMMAND:
        /* Handle context menu item selections */
        switch (LOWORD(wParam)) {
        case IDM_TOGGLE_MPO:
            SetMPO(!IsMpoDisabled());
            break;
        case IDM_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            break;
        case IDM_CHECK_UPDATE:
            if (g_updateState == UPDATE_AVAILABLE) {
                /* Open the releases page so the user can download */
                ShellExecuteW(NULL, L"open",
                    L"https://github.com/fernandoenzo/ForceComposedFlip/releases/latest",
                    NULL, NULL, SW_SHOWNORMAL);
            } else if (g_updateState != UPDATE_CHECKING) {
                /* Trigger a manual check with user feedback */
                g_updateState = UPDATE_CHECKING;
                ShowBalloon(L"ForceComposedFlip Update",
                            L"Checking for updates...");
                CreateThread(NULL, 0, CheckUpdateThread,
                             (LPVOID)TRUE, 0, NULL);
            }
            break;
        case IDM_EXIT:
            CleanExit();
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ─── Overlay Window Procedure ──────────────────────────────────────── */

/*
 * Minimal WndProc for the overlay window. It only needs to handle
 * WM_DESTROY and pass everything else to the default handler.
 * The overlay never processes user input (WS_EX_TRANSPARENT makes it
 * click-through) and has no UI elements.
 */
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam)
{
    if (msg == WM_DESTROY) {
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ─── Overlay Management ────────────────────────────────────────────── */

/*
 * Creates the invisible overlay window that forces DWM Composed Flip.
 *
 * The window has these critical properties:
 *   WS_EX_TOPMOST:     Stays above all normal and most special windows
 *   WS_EX_TOOLWINDOW:  Hidden from taskbar and Alt+Tab switcher
 *   WS_EX_TRANSPARENT: Click-through — all mouse input passes through
 *   WS_EX_LAYERED:     Required for per-window opacity via
 *                       SetLayeredWindowAttributes
 *   WS_POPUP:          No title bar, no borders, no system menu
 *
 *   Size: 1x1 pixel at position (0,0)
 *   Opacity: 1/255 — nearly invisible but NOT fully transparent.
 *     A fully transparent window (opacity 0) would be optimized out by DWM
 *     and would not force composition. 1/255 is imperceptible to the eye
 *     but sufficient to keep DWM compositing every frame.
 *
 * After creation, SetWindowPos is called with HWND_TOPMOST to ensure
 * the window is at the very top of the z-order.
 */
static void CreateOverlay(void)
{
    if (g_hwndOverlay != NULL) {
        return; /* Already exists */
    }

    HINSTANCE hInstance = GetModuleHandleW(NULL);

    g_hwndOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        CLASS_OVERLAY,
        L"",            /* No window title */
        WS_POPUP,       /* No decorations */
        0, 0, 1, 1,     /* Position (0,0), size 1x1 pixel */
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwndOverlay) {
        return;
    }

    /*
     * Set window opacity to 1 out of 255.
     * LWA_ALPHA tells the function to use the bAlpha parameter (third arg)
     * as the window-wide opacity value.
     */
    SetLayeredWindowAttributes(g_hwndOverlay, 0, 1, LWA_ALPHA);

    /* Show without activating (don't steal focus from the current app) */
    ShowWindow(g_hwndOverlay, SW_SHOWNOACTIVATE);

    /*
     * Force topmost z-order via direct SetWindowPos call.
     * HWND_TOPMOST places this above all non-topmost windows.
     * Flags: SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE — only change z-order.
     */
    SetWindowPos(g_hwndOverlay, HWND_TOPMOST,
                 0, 0, 1, 1, SWP_TOPMOST_FLAGS);
}

/*
 * Destroys the overlay window and resets the global handle.
 * Safe to call even if no overlay exists.
 */
static void DestroyOverlay(void)
{
    if (g_hwndOverlay != NULL) {
        DestroyWindow(g_hwndOverlay);
        g_hwndOverlay = NULL;
    }
}

/*
 * Destroys the overlay and schedules its recreation after a 500ms delay.
 *
 * The delay is necessary because DWM needs time to process the removal
 * of the old overlay window internally. If we create the new overlay
 * immediately after destroying the old one, DWM may not have finished
 * updating its internal composition state, leading to z-order issues
 * where the new overlay ends up below the target application.
 *
 * Instead of blocking with Sleep(500) (which would freeze the entire
 * message loop and make the program unresponsive for half a second),
 * we use a one-shot timer:
 *   1. Destroy the overlay now
 *   2. Set a 500ms timer (TIMER_RECREATE_OVERLAY)
 *   3. Return to the message loop (program stays responsive)
 *   4. When the timer fires, CreateOverlay() is called from WndProc
 */
static void RecreateOverlay(void)
{
    DestroyOverlay();
    SetTimer(g_hwndMain, TIMER_RECREATE_OVERLAY, INTERVAL_RECREATE, NULL);
}

/* ─── Periodic Tasks ────────────────────────────────────────────────── */

/*
 * Re-asserts the overlay's topmost position.
 *
 * Called every 500ms because games running under Windows Fullscreen
 * Optimizations (FSO) operate in a special z-band that sits above
 * normal HWND_TOPMOST windows. The periodic re-assertion ensures
 * DWM keeps the overlay in the composition chain even when FSO
 * tries to optimize it away.
 */
static void ReassertTopmost(void)
{
    if (g_hwndOverlay == NULL) {
        return;
    }
    SetWindowPos(g_hwndOverlay, HWND_TOPMOST,
                 0, 0, 1, 1, SWP_TOPMOST_FLAGS);
}

/*
 * Monitors the foreground window and recreates the overlay when it changes.
 *
 * When a new window comes to the foreground (e.g., a game launches or
 * regains focus), the overlay may end up below it in the z-order.
 * Detecting this change and recreating the overlay ensures it is always
 * positioned above the active application.
 */
static void CheckForeground(void)
{
    HWND current = GetForegroundWindow();
    if (current != g_lastForeground) {
        g_lastForeground = current;
        RecreateOverlay();
    }
}

/* ─── System Tray ───────────────────────────────────────────────────── */

/*
 * Sets up the system tray icon.
 * Loads the icon embedded in the executable as resource ID 1
 * (defined in ForceComposedFlip.rc). Falls back to the generic
 * Windows application icon if the resource is not found (e.g.,
 * if compiled without the .rc resource file).
 */
static void SetupTrayIcon(void)
{
    /* Load the icon from our own executable's embedded resources */
    g_hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));

    /* Fall back to a default application icon if resource not found */
    if (g_hIcon == NULL) {
        g_hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }

    /* Fill in the NOTIFYICONDATAW structure for Shell_NotifyIconW */
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwndMain;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_hIcon;
    lstrcpyW(g_nid.szTip, L"ForceComposedFlip " VERSION_STRING);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

/*
 * Removes the system tray icon and frees the icon resource.
 */
static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hIcon) {
        DestroyIcon(g_hIcon);
        g_hIcon = NULL;
    }
}

/*
 * Updates the tray tooltip text.
 * If text is NULL, generates the default tooltip with the version
 * string and current local time in HH:mm:ss format.
 */
static void UpdateTooltip(const WCHAR *text)
{
    if (text) {
        lstrcpynW(g_nid.szTip, text,
                  sizeof(g_nid.szTip) / sizeof(WCHAR));
    } else {
        /* Build "ForceComposedFlip <version> - Active (HH:mm:ss)" */
        SYSTEMTIME st;
        GetLocalTime(&st);
        wsprintfW(g_nid.szTip,
                  L"ForceComposedFlip " VERSION_STRING
                  L" - Active (%02d:%02d:%02d)",
                  st.wHour, st.wMinute, st.wSecond);
    }
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/*
 * Shows a balloon (toast) notification from the system tray icon.
 *
 * Cancels any pending balloon first, then displays the new one with the
 * application icon. A one-shot timer auto-dismisses it after 3.5 seconds
 * to keep the notification unobtrusive.
 */
static void ShowBalloon(const WCHAR *title, const WCHAR *text)
{
    /* Cancel any pending balloon and its timer */
    g_nid.uFlags = NIF_INFO;
    g_nid.szInfo[0] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    KillTimer(g_hwndMain, TIMER_CLOSE_BALLOON);

    /* Show the new balloon with the application icon */
    g_nid.dwInfoFlags  = NIIF_USER | NIIF_LARGE_ICON;
    g_nid.hBalloonIcon = g_hIcon;
    lstrcpynW(g_nid.szInfoTitle, title,
              sizeof(g_nid.szInfoTitle) / sizeof(WCHAR));
    lstrcpynW(g_nid.szInfo, text,
              sizeof(g_nid.szInfo) / sizeof(WCHAR));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    /* Schedule auto-dismiss after 3.5 seconds */
    SetTimer(g_hwndMain, TIMER_CLOSE_BALLOON, INTERVAL_BALLOON, NULL);
}

/*
 * Shows the tray context menu at the current cursor position.
 *
 * Menu layout:
 *   "ForceComposedFlip <version>"       (disabled header label)
 *   ─────────────────────────────
 *   "✓ MPO disabled"                   (checkmark toggle)
 *   ─────────────────────────────
 *   "✓ Start with Windows"             (checkmark toggle)
 *   ─────────────────────────────
 *   "Check for updates"                (dynamic text based on state)
 *   "Exit"
 */
static void ShowContextMenu(void)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    /* Header label — grayed out, not clickable */
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0,
                L"ForceComposedFlip " VERSION_STRING);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    /* MPO toggle — checkmark reflects current registry state */
    BOOL mpoDisabled = IsMpoDisabled();
    AppendMenuW(hMenu,
                MF_STRING | (mpoDisabled ? MF_CHECKED : MF_UNCHECKED),
                IDM_TOGGLE_MPO, L"MPO disabled");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    /* Auto-start toggle — checkmark reflects HKCU\...\Run value */
    AppendMenuW(hMenu,
                MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    /* Update checker — dynamic text reflects background check state */
    if (g_updateState == UPDATE_AVAILABLE) {
        WCHAR msg[128];
        wsprintfW(msg, L"Update to %s available!", g_updateVersion);
        AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE, msg);
    } else if (g_updateState == UPDATE_CHECKING) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED | MF_DISABLED,
                    IDM_CHECK_UPDATE, L"Checking for updates...");
    } else if (g_updateState == UPDATE_LATEST) {
        AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE,
                    L"Check for updates (latest)");
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE,
                    L"Check for updates");
    }

    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    /*
     * SetForegroundWindow is required before TrackPopupMenu, otherwise
     * the menu won't dismiss when the user clicks outside of it.
     * This is a documented Win32 quirk (Microsoft KB135788).
     */
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwndMain);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwndMain, NULL);

    /*
     * Post a dummy message after TrackPopupMenu to force the message loop
     * to cycle, ensuring proper menu dismissal (another documented Win32
     * tray icon quirk).
     */
    PostMessage(g_hwndMain, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

/* ─── UAC Elevation ─────────────────────────────────────────────────── */

/*
 * Runs cmd.exe with the given parameters elevated via UAC.
 *
 * Uses ShellExecuteExW with the "runas" verb to trigger the UAC
 * elevation prompt. The console window is hidden (SW_HIDE) since
 * the user doesn't need to see it.
 *
 * Waits up to 5 seconds for the elevated process to complete before
 * returning, so the caller knows the operation has finished.
 *
 * Returns TRUE if the command was executed successfully.
 * Returns FALSE if the user cancelled UAC or the execution failed.
 */
static BOOL RunElevated(const WCHAR *params)
{
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS; /* We need the process handle */
    sei.lpVerb       = L"runas";                /* Triggers UAC elevation     */
    sei.lpFile       = L"cmd.exe";
    sei.lpParameters = params;
    sei.nShow        = SW_HIDE;                 /* Hide the console window    */

    if (!ShellExecuteExW(&sei)) {
        return FALSE; /* User cancelled UAC or execution failed */
    }

    /* Wait for the elevated process to finish (5 second timeout) */
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000);
        CloseHandle(sei.hProcess);
    }

    return TRUE;
}

/* ─── MPO Toggle ────────────────────────────────────────────────────── */

/*
 * Checks whether MPO (Multiplane Overlay) is currently disabled by
 * querying HKLM\SOFTWARE\Microsoft\Windows\Dwm\OverlayTestMode.
 * Returns TRUE if the value exists and equals 5.
 *
 * This read does not require elevation — standard users can read
 * HKLM keys. Only writing requires the UAC-elevated cmd.exe path
 * used by SetMPO().
 */
static BOOL IsMpoDisabled(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\Dwm",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    DWORD value = 0;
    DWORD valueSize = sizeof(value);
    DWORD valueType = 0;
    LONG ret = RegQueryValueExW(hKey, L"OverlayTestMode", NULL,
                                &valueType, (LPBYTE)&value, &valueSize);
    RegCloseKey(hKey);

    return (ret == ERROR_SUCCESS && valueType == REG_DWORD && value == 5);
}

/*
 * Adds or removes the MPO (Multiplane Overlay) disable registry key.
 *
 * MPO allows DWM to use hardware overlay planes to bypass full
 * composition. Even with our topmost overlay window, MPO can let
 * frames skip the composition chain. Disabling MPO via this registry
 * key forces DWM to composite all frames through the normal pipeline.
 *
 * The registry key is:
 *   HKLM\SOFTWARE\Microsoft\Windows\Dwm\OverlayTestMode = 5 (DWORD)
 *
 * After the registry change, the user is prompted to restart DWM
 * immediately (via "taskkill /f /im dwm.exe" — Windows automatically
 * restarts DWM after it's killed) or reboot later.
 *
 * Parameters:
 *   disable: TRUE  = add the key (disable MPO)
 *            FALSE = remove the key (enable MPO / restore default)
 */
static void SetMPO(BOOL disable)
{
    const WCHAR *regCmd;
    const WCHAR *successMsg;

    if (disable) {
        regCmd = L"/c reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\Dwm\" "
                 L"/v OverlayTestMode /t REG_DWORD /d 5 /f";
        successMsg =
            L"MPO disabled (registry key added).\n\n"
            L"Restart DWM now to apply immediately?\n"
            L"(Screen will flash briefly. If it doesn't work, "
            L"do a full reboot.)";
    } else {
        regCmd = L"/c reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\Dwm\" "
                 L"/v OverlayTestMode /f";
        successMsg =
            L"MPO enabled (registry key removed).\n\n"
            L"Restart DWM now to apply immediately?\n"
            L"(Screen will flash briefly. If it doesn't work, "
            L"do a full reboot.)";
    }

    if (RunElevated(regCmd)) {
        int answer = MessageBoxW(NULL, successMsg, L"ForceComposedFlip",
                                 MB_YESNO | MB_ICONWARNING);
        if (answer == IDYES) {
            if (!RunElevated(L"/c taskkill /f /im dwm.exe")) {
                MessageBoxW(NULL,
                    L"Could not restart DWM. "
                    L"Please reboot your PC to apply changes.",
                    L"ForceComposedFlip",
                    MB_OK | MB_ICONWARNING);
            }
        }
    } else {
        MessageBoxW(NULL,
            L"Operation cancelled or failed.",
            L"ForceComposedFlip",
            MB_OK | MB_ICONWARNING);
    }
}

/* ─── Auto-Start ────────────────────────────────────────────────────── */

/*
 * Checks whether ForceComposedFlip is registered to start with Windows.
 * Reads HKCU\Software\Microsoft\Windows\CurrentVersion\Run for a value
 * named "ForceComposedFlip". No elevation required — HKCU is per-user.
 */
static BOOL IsAutoStartEnabled(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY,
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    DWORD valueType = 0;
    DWORD valueSize = 0;
    LONG ret = RegQueryValueExW(hKey, AUTOSTART_VALUE, NULL,
                                &valueType, NULL, &valueSize);
    RegCloseKey(hKey);

    return (ret == ERROR_SUCCESS && valueType == REG_SZ);
}

/*
 * Enables or disables auto-start with Windows.
 * When enabling, writes the full path of the current executable to
 * HKCU\...\Run so Windows launches it at logon.
 * When disabling, deletes the registry value.
 * No elevation required — HKCU is per-user.
 */
static void SetAutoStart(BOOL enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY,
                      0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return;

    if (enable) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        RegSetValueExW(hKey, AUTOSTART_VALUE, 0, REG_SZ,
                       (const BYTE *)exePath,
                       (DWORD)((lstrlenW(exePath) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hKey, AUTOSTART_VALUE);
    }

    RegCloseKey(hKey);
}

/* ─── Update Checker ────────────────────────────────────────────────── */

/*
 * Background thread that checks GitHub for a newer release.
 *
 * Sends an HTTPS HEAD request to the "releases/latest" URL, which
 * GitHub 302-redirects to /releases/tag/<version>. The final URL is
 * retrieved after the redirect and the version tag is extracted from
 * the last path segment. Compared against the compile-time
 * VERSION_STRING to determine if an update is available.
 *
 * Communicates the result back to the main thread via PostMessageW
 * (WM_UPDATE_CHECK_DONE) so all UI updates happen on the message
 * loop thread — no cross-thread window access.
 *
 * lpParam: FALSE for silent startup check, TRUE for manual user check.
 */
static DWORD WINAPI CheckUpdateThread(LPVOID lpParam)
{
    BOOL isManual = (BOOL)(UINT_PTR)lpParam;
    int status = -1;

    HINTERNET hSession = InternetOpenW(
        L"ForceComposedFlip", INTERNET_OPEN_TYPE_PRECONFIG,
        NULL, NULL, 0);

    if (hSession) {
        DWORD timeout = 5000;
        InternetSetOptionW(hSession, INTERNET_OPTION_CONNECT_TIMEOUT,
                           &timeout, sizeof(timeout));
        InternetSetOptionW(hSession, INTERNET_OPTION_SEND_TIMEOUT,
                           &timeout, sizeof(timeout));
        InternetSetOptionW(hSession, INTERNET_OPTION_RECEIVE_TIMEOUT,
                           &timeout, sizeof(timeout));

        HINTERNET hConnect = InternetConnectW(
            hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT,
            NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);

        if (hConnect) {
            /*
             * HEAD request — we only need the redirect URL, not the body.
             * Aggressive cache-bypass flags ensure we always hit GitHub,
             * not a cached response from a corporate proxy.
             */
            HINTERNET hReq = HttpOpenRequestW(
                hConnect, L"HEAD",
                L"/fernandoenzo/ForceComposedFlip/releases/latest",
                NULL, NULL, NULL,
                INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                INTERNET_FLAG_NO_CACHE_WRITE |
                INTERNET_FLAG_PRAGMA_NOCACHE, 0);

            if (hReq) {
                HttpAddRequestHeadersW(
                    hReq, L"Cache-Control: no-cache\r\n",
                    -1, HTTP_ADDREQ_FLAG_ADD);

                if (HttpSendRequestW(hReq, NULL, 0, NULL, 0)) {
                    /*
                     * After the 302 redirect, INTERNET_OPTION_URL gives
                     * the final URL, e.g.:
                     *   https://github.com/.../releases/tag/1.5
                     * Extract the version from the last '/' segment.
                     */
                    WCHAR szUrl[512] = {0};
                    DWORD dwSize = sizeof(szUrl);
                    if (InternetQueryOptionW(hReq, INTERNET_OPTION_URL,
                                             szUrl, &dwSize)) {
                        WCHAR *pTag = wcsrchr(szUrl, L'/');
                        if (pTag) {
                            pTag++;
                            /* Strip leading 'v' or 'V' prefix if present */
                            if (*pTag == L'v' || *pTag == L'V')
                                pTag++;
                            if (lstrcmpW(pTag, VERSION_STRING) != 0) {
                                status = 1; /* Update available */
                                lstrcpynW(g_updateVersion, pTag,
                                    sizeof(g_updateVersion) / sizeof(WCHAR));
                            } else {
                                status = 0; /* Up to date */
                            }
                        }
                    }
                }
                InternetCloseHandle(hReq);
            }
            InternetCloseHandle(hConnect);
        }
        InternetCloseHandle(hSession);
    }

    PostMessageW(g_hwndMain, WM_UPDATE_CHECK_DONE,
                 (WPARAM)status, (LPARAM)isManual);
    return 0;
}

/* ─── Clean Exit ────────────────────────────────────────────────────── */

/*
 * Performs a clean shutdown:
 *   1. Kills all active timers (recurring and potential one-shot,
 *      including the balloon auto-dismiss timer)
 *   2. Destroys the overlay window
 *   3. Removes the system tray icon
 *   4. Destroys the main window, which triggers WM_DESTROY →
 *      PostQuitMessage → message loop exits → program terminates
 */
static void CleanExit(void)
{
    KillTimer(g_hwndMain, TIMER_REASSERT_TOPMOST);
    KillTimer(g_hwndMain, TIMER_CHECK_FOREGROUND);
    KillTimer(g_hwndMain, TIMER_RECREATE_OVERLAY);
    KillTimer(g_hwndMain, TIMER_CLOSE_BALLOON);
    DestroyOverlay();
    RemoveTrayIcon();
    DestroyWindow(g_hwndMain);
}
