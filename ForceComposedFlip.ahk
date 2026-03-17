; ForceComposedFlip.ahk
; Forces DWM Composed Flip to capture DLSS Frame Generation frames
; with Sunshine/Moonlight. See README.md for full documentation.
; License: GPLv3

#Requires AutoHotkey v2.0
#SingleInstance Force
Persistent

global overlay := ""
global overlayHwnd := 0
global lastForeground := 0

; Monitor foreground window changes every 2 seconds
SetTimer(CheckForeground, 2000)

; Re-assert topmost position every 500ms to prevent DWM from
; optimizing the overlay out of the composition chain
SetTimer(ReassertTopmost, 500)

; System tray setup - gamepad icon
A_IconTip := "ForceComposedFlip - Waiting for game..."
TraySetIcon("ddores.dll", 30)

tray := A_TrayMenu
tray.Delete()
tray.Add("ForceComposedFlip - Active", (*) => "")
tray.Disable("ForceComposedFlip - Active")
tray.Add()
tray.Add("Recreate overlay now", (*) => RecreateOverlay())
tray.Add()
tray.Add("Disable MPO (add registry key)", (*) => SetMPO(true))
tray.Add("Enable MPO (remove registry key)", (*) => SetMPO(false))
tray.Add()
tray.Add("Exit", (*) => CleanExit())

; Creates the invisible overlay window with all necessary flags
CreateOverlay() {
    global overlay, overlayHwnd
    if (overlay != "") {
        return
    }
    
    ; +AlwaysOnTop     = WS_EX_TOPMOST
    ; -Caption         = No title bar
    ; +ToolWindow      = Hidden from taskbar and Alt+Tab
    ; +E0x00000020     = WS_EX_TRANSPARENT (click-through)
    overlay := Gui("+AlwaysOnTop -Caption +ToolWindow +E0x00000020")
    overlay.BackColor := "000000"
    ; Opacity must be 1 (not 0) — fully transparent windows are optimized
    ; out by DWM and won't force Composed Flip. 1/255 is imperceptible.
    WinSetTransparent(1, overlay)
    overlay.Show("x0 y0 w1 h1 NoActivate")
    
    overlayHwnd := overlay.Hwnd
    
    ; Force HWND_TOPMOST via Win32 API for maximum z-order priority
    ; Flags: SWP_NOMOVE (0x02) | SWP_NOSIZE (0x01) | SWP_NOACTIVATE (0x10) = 0x13
    DllCall("SetWindowPos"
        , "Ptr", overlayHwnd
        , "Ptr", -1          ; HWND_TOPMOST
        , "Int", 0
        , "Int", 0
        , "Int", 1
        , "Int", 1
        , "UInt", 0x0013)
}

; Destroys the overlay window
DestroyOverlay() {
    global overlay, overlayHwnd
    if (overlay != "") {
        try {
            overlay.Destroy()
        }
        overlay := ""
        overlayHwnd := 0
    }
}

; Destroys and recreates the overlay (needed when foreground app changes)
RecreateOverlay() {
    DestroyOverlay()
    Sleep(500)
    CreateOverlay()
    A_IconTip := "ForceComposedFlip - Active (" FormatTime(, "HH:mm:ss") ")"
}

; Periodically re-asserts the topmost position of the overlay window.
; Critical for FSO games which sit on a higher z-band than normal windows.
ReassertTopmost(*) {
    global overlayHwnd
    if (overlayHwnd = 0) {
        return
    }
    
    try {
        DllCall("SetWindowPos"
            , "Ptr", overlayHwnd
            , "Ptr", -1          ; HWND_TOPMOST
            , "Int", 0
            , "Int", 0
            , "Int", 1
            , "Int", 1
            , "UInt", 0x0013)
    }
}

; Monitors foreground window changes and recreates the overlay when
; a new window comes to the front (e.g. a game launching or regaining focus)
CheckForeground(*) {
    global lastForeground
    try {
        current := WinGetID("A")
    } catch {
        return
    }
    if (current != lastForeground) {
        lastForeground := current
        RecreateOverlay()
    }
}

; Runs a command elevated via UAC using ShellExecuteEx.
; Returns true if the command was executed, false if UAC was cancelled.
RunElevated(params) {
    ; SHELLEXECUTEINFOW structure (size = 112 bytes on x64)
    sizeOfStruct := A_PtrSize = 8 ? 112 : 60
    sei := Buffer(sizeOfStruct, 0)
    
    NumPut("UInt", sizeOfStruct, sei, 0)                          ; cbSize
    NumPut("UInt", 0x40, sei, 4)                                  ; fMask = SEE_MASK_NOCLOSEPROCESS
    NumPut("Ptr", StrPtr("runas"), sei, A_PtrSize = 8 ? 16 : 12) ; lpVerb
    NumPut("Ptr", StrPtr("cmd.exe"), sei, A_PtrSize = 8 ? 24 : 16) ; lpFile
    NumPut("Ptr", StrPtr(params), sei, A_PtrSize = 8 ? 32 : 20)  ; lpParameters
    NumPut("Int", 0, sei, A_PtrSize = 8 ? 60 : 44)               ; nShow = SW_HIDE
    
    result := DllCall("shell32\ShellExecuteExW", "Ptr", sei, "Int")
    
    if (!result) {
        return false
    }
    
    ; Wait for the elevated process to finish
    hProcess := NumGet(sei, A_PtrSize = 8 ? 104 : 56, "Ptr")
    if (hProcess) {
        DllCall("WaitForSingleObject", "Ptr", hProcess, "UInt", 5000) ; 5s timeout
        DllCall("CloseHandle", "Ptr", hProcess)
    }
    
    return true
}

; Adds or removes the MPO disable registry key and optionally restarts DWM.
; Requests admin elevation via UAC only when needed.
SetMPO(disable) {
    if (disable) {
        if RunElevated('/c reg add "HKLM\SOFTWARE\Microsoft\Windows\Dwm" /v OverlayTestMode /t REG_DWORD /d 5 /f') {
            answer := MsgBox("MPO disabled (registry key added).`n`nRestart DWM now to apply immediately?`n(Screen will flash briefly. If it doesn't work, do a full reboot.)", "ForceComposedFlip", "YesNo Icon!")
            if (answer = "Yes") {
                if !RunElevated("/c taskkill /f /im dwm.exe") {
                    MsgBox("Could not restart DWM. Please reboot your PC to apply changes.", "ForceComposedFlip", "OK Icon!")
                }
            }
        } else {
            MsgBox("Operation cancelled or failed.", "ForceComposedFlip", "OK Icon!")
        }
    } else {
        if RunElevated('/c reg delete "HKLM\SOFTWARE\Microsoft\Windows\Dwm" /v OverlayTestMode /f') {
            answer := MsgBox("MPO enabled (registry key removed).`n`nRestart DWM now to apply immediately?`n(Screen will flash briefly. If it doesn't work, do a full reboot.)", "ForceComposedFlip", "YesNo Icon!")
            if (answer = "Yes") {
                if !RunElevated("/c taskkill /f /im dwm.exe") {
                    MsgBox("Could not restart DWM. Please reboot your PC to apply changes.", "ForceComposedFlip", "OK Icon!")
                }
            }
        } else {
            MsgBox("Operation cancelled or failed.", "ForceComposedFlip", "OK Icon!")
        }
    }
}

; Clean exit: destroy overlay before exiting
CleanExit(*) {
    DestroyOverlay()
    ExitApp()
}

return
