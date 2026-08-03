# CutsceneComfort v0.1.0-alpha.1

First public alpha of the standalone UEVR cutscene comfort plugin.

## Included

- View-target cutscene detection using `PlayerCameraManager -> ViewTarget`.
- Temporary camera-offset, decoupled-pitch, aim, and UObjectHook changes with
  atomic crash-recovery journaling and eased restoration.
- Optional room-anchored stereoscopic cutscene aperture with exact width,
  height, distance, feather, rounded corners, symmetric curvature, surround
  color, opacity, fade time, preview, and recenter controls.
- Stock-plugin ImGui controls rendered in both VR and the desktop mirror while
  an HMD is active.
- A companion visibility panel whenever the UEVR menu is open, implemented
  entirely in the plugin DLL because the current public ABI has no native page
  extension callback.
- D3D11 and D3D12 standalone render paths plus the transient
  `CutsceneComfort.WindowMode.v1` bridge used by the companion UEVR fork.

## Validation boundary

- Release x64 build and shader compilation test pass.
- The exact DLL loaded in Halo Campaign Evolved on D3D12 through the Meta XR
  Operator runtime; the log confirmed the Operator final-eye bridge, ImGui
  initialization, `Engine.CameraActor` discovery, and live detection.
- The same controls were visible at the same time in Halo's desktop window and
  the Meta XR Simulator left-eye view.
- Real-cutscene entry/exit and eased restoration were previously exercised on
  a D3D11 Unreal host. A fresh Halo campaign cutscene pass is still open for
  this alpha.

## Installation

Copy `CutsceneComfort.dll` to:

`%APPDATA%\UnrealVRMod\<GameName>\plugins\CutsceneComfort.dll`

The plugin can run on stock UEVR. Use the matching `UEVR-6DOF-Window` release
when the spatial mask must be drawn into the final submitted eye textures.
