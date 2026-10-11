# Ace Combat: Assault Horizon VR

Experimental SteamVR / OpenXR mod for **Ace Combat: Assault Horizon Enhanced Edition (Windows)**. Adds headset-tracked stereo rendering and an aircraft-aligned HUD; menus and unsupported rendering use a virtual screen. Use your usual game controls. Motion-controller weapons are not implemented.

## Requirements

- Your own installed copy of the game, 64-bit Windows, and a SteamVR-compatible headset with SteamVR running.
- The mod targets one executable build (SHA-256: `b39bb7b55ed34ed7acc80e46181d1dbf76ad2f13d39b6d401daf4b2a67d6cf9f`). Other builds are unsupported; keep `allow_unknown_exe=0`.
- Start with **2560×1440 fullscreen** and the supplied settings. Larger backbuffers are unsupported. Performance and rendering compatibility remain experimental.

## Install

1. Download `acah-vr-v0.1.0.zip` from [Releases](https://github.com/votizlov/ace-combat-assault-horizon-vr/releases). GitHub's source ZIP does not contain the ready-to-run binaries.
2. Close the game. Extract the archive into the game installation folder so `vr-poc` sits beside `Ace Combat_AH.exe`:
   ```text
   Game folder/
     Ace Combat_AH.exe       (your existing game)
     vr-poc/
       Launch-VR.cmd
       tools/
       package/
         d3d9.dll
         acah_vr.ini
         acah_vr_host.exe
         openxr_loader.dll
   ```
3. Open PowerShell in the game folder and run:
   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\vr-poc\tools\Install.ps1
   ```
   This copies only the mod's `d3d9.dll` and `acah_vr.ini` beside the game executable. If either file already exists, back it up and resolve the conflict yourself; other D3D9 proxy mods may conflict.

## Play

Start SteamVR, connect your headset, then double-click `vr-poc\Launch-VR.cmd`. It starts the VR host and game. Select cockpit view, face forward, and press **F9** to recenter. Keep the game window active.

| Key | Action |
| --- | --- |
| F9 | Recenter headset |
| F8 | Toggle stereo / virtual screen |
| F10 | Return to virtual screen |
| Ctrl+Shift+F12 | Exit the VR host |

If SteamVR is installed elsewhere, launch from PowerShell with its folder:
```powershell
.\vr-poc\Launch-VR.cmd -SteamVR 'D:\SteamLibrary\steamapps\common\SteamVR'
```

Edit the **game-folder** `acah_vr.ini` with the game closed, then restart. `units_per_meter` adjusts tracking scale; `spatial_hud=0` disables the spatial HUD. Leave the supplied 512 MiB clone budget initially.

Stereo may fall back to the screen for unsupported passes. Visibility, effects, HUD placement, and frame delivery can vary; smooth headset performance is not guaranteed. For launch failures, read `vr-poc\package\launch-*.host.*.log` and the game-folder `acah_vr_hook.log`. Avoid sharing shader dumps or game files in issue reports.

## Uninstall / update

Close the game and VR host, then run:
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\vr-poc\tools\Remove.ps1
```
Modified files are preserved. Add `-RemoveEditedConfig` if you also want to remove your edited mod configuration. Remove the old installation before extracting an update; keep the old install manifest until removal completes.

## Build from source (optional)

Requires Visual Studio 2022 with Desktop development with C++, a Windows SDK, CMake 3.24+, and Git on PATH. Place the checkout in the game folder as `vr-poc`, then run `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\vr-poc\tools\Build.ps1`. It builds the x86 proxy and x64 host, downloads OpenXR SDK 1.1.63, and fills `package/`. Then follow the installation steps above.

Only mod source, scripts, configuration, and mod binaries are distributed. No game executables, assets, captured shaders, or game archives are included. This is an unofficial fan project. Third-party notices for the OpenXR loader are in [third-party/](third-party/).
