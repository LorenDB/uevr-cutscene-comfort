# CutsceneComfort

A UEVR plugin that notices when the game hands the camera to a cinematic and
quietly undoes your VR camera tweaks for the duration of the scene.

The plugin is distributed as a normal `CutsceneComfort.dll`. Its detection,
camera restoration, settings UI, and desktop/VR UI rendering do not require a
custom UEVR build. The optional final-eye spatial aperture is most reliable
with the companion `UEVR-6DOF-Window` fork described below.

Camera offsets that feel great in first person look wrong the moment a
cutscene camera takes over. You end up floating above the shot or clipping
through an actor. This plugin zeroes the offsets when a cutscene starts,
optionally flattens decoupled pitch and pauses UObjectHook, then eases
the camera offsets back once gameplay resumes while restoring the other
toggles immediately.

## How detection works

Every engine tick the plugin reads the camera the engine is actually
rendering from: `PlayerController -> PlayerCameraManager -> ViewTarget.Target`.
If that target is a `CameraActor` (which includes `CineCameraActor`), you are
watching a cutscene. Custom blueprint cameras are caught by name when their
class contains `CineCamera`, `CinematicCamera` or `CutsceneCamera`. There is
also an optional loose mode that treats any non pawn view target as a scene,
for games with unusual camera setups.

A small debounce keeps single frame camera blips during blends and level
loads from toggling anything.

This path has been exercised against real `CineCameraActor` targets in
SpongeBob Rehydrated on D3D11. Both entry and exit fired, and a 1.5 second
restore from `12.5, -3.25, 7.75` progressed through intermediate values before
landing exactly on that baseline. The plugin atomically journals every value
it temporarily changes, so a forced process termination during a cutscene can
recover offsets, decoupled pitch, aim method and UObjectHook state on the next
launch before detection resumes. Once restoration finishes, the verified
baseline is also saved back through UEVR before the recovery journal is
cleared, so a later clean launch cannot resurrect temporary cutscene values.

## Fixed screen during cutscenes

Optional, and off until you enable it. When a cutscene is detected the
headset view fades to black, then comes back as UEVR's 2D screen: a quad
in front of you, with the game camera no longer following the headset.
When the cutscene ends, the screen fades to black and the tracked VR view
fades back in.

The fade uses the engine camera fade (`StartCameraFade`), so the mode
change happens while the picture is black. Each half defaults to 0.15
seconds and can be set from 0.10 to 0.30 seconds. That keeps the change
visible and still short enough that a prompt immediately after the scene
is readable.

Two pictures are available:

- **Stereo portal** (the default) is UEVR's 2D screen as it already works
  on OpenXR. Each eye keeps its own view, so the quad has depth, and the
  headset pose is not applied to the camera.
- **Flat 2D** feeds both eyes the same camera position, so the quad is a
  single picture.

The screen distance and height are UEVR's UI distance and UI size for the
duration of the scene. "Keep the screen in front of my view" turns on
UEVR's follow-view placement and centers the quad; your previous UI
placement is restored afterwards. Roomscale movement is held off for the
same span so walking around does not move the character, and it is
restored with the other values. If UEVR's GUI toggle was off, it is
turned on so the quad is submitted, then restored.

The originals are journaled in `cutscene_comfort.ini` before anything is
changed. A crash mid-scene restores them on the next launch, then asks
UEVR to save that restored config.

This path uses stock UEVR. It does not need the Operator fork. Preview
runs the same fade for five seconds without waiting for a real cutscene.

## Fixed 6DOF cutscene window

Optional and off by default. While the fixed screen above is enabled, this
window stays off. When a cutscene begins, the plugin captures a
tracking-space anchor and places the visible scene in a physical aperture in
front of that anchor. The aperture does not get re-centered every frame. You
can translate or turn your head and look at it from another angle while the
cutscene inside remains the game's original per-eye stereo render.

The tested final-eye path uses the accompanying Operator fork at
`E:\Github\UEVRMetaXROperator-6DOF-Window`. CutsceneComfort sends a transient
`CutsceneComfort.WindowMode.v1` state to Operator, and Operator draws the mask
directly into each final eye texture before OpenXR submission. It does not
enable or overwrite Operator's normal all-game Window Mode settings.

The plugin exposes these cutscene-only controls:

- **Lock width to 16:9** derives height from width when desired.
- **Window width / height** set the exact clear-aperture dimensions in meters.
- **Distance** sets how far the aperture's center is from the captured anchor.
- **Feather** sets the physical width of the soft edge in meters.
- **Corner radius** changes the aperture from square corners to a rounded
  rectangle.
- **Curvature** blends from a flat plane to a centered cylindrical window.
  Curvature is symmetric around the captured forward direction; it is not
  biased to the right.
- **Surround color** and **surround opacity** control everything outside the
  aperture.
- **Fade time** controls the cutscene entry and exit transition.
- **Preview / Recenter** shows the window outside a cutscene and captures a
  fresh anchor in front of the current head pose.

All dimensions are physical OpenXR tracking-space meters rather than NDC or
desktop-window percentages.

### Live validation

The exact current D3D11 binaries were injected into SpongeBob Rehydrated and
viewed through Meta XR Simulator during a real
`CameraMenu (CineCameraActor)` target. Operator reported the effective
cutscene settings as 0.8 m wide, 0.45 m high, 1 m away, 0.05 m feather,
0.2 m corner radius, and the requested purple surround. The composited left
eye visibly contained the rounded aperture; this is final-eye output, not a
desktop backbuffer inference.

The simulated head was then moved 0.5 m right and yawed 25 degrees. The view
of the aperture changed, while Operator continued to report the original
anchor origin `(0, 0, 0)` and center approximately `(0, 0, -1)`. Returning
the head to the identity pose centered the curved window again. This proves
the window is fixed in tracking space instead of following head rotation.

Earlier matched testing remains useful for the other half of the feature:
six translated-view trials changed the 3D scene through the aperture more
than their time-matched controls in 6/6 repetitions, while the mask stayed
fixed. Twelve paired stability trials also found no excess crashes with the
frame enabled. The repeatable harnesses are `tools/lean_trials.ps1` and
`tools/ab_trials.ps1`.

### Important backend boundary

The standalone plugin retains its D3D11/D3D12 backbuffer renderer as a stock
UEVR fallback, but that callback is too late to reach the submitted eye
textures on the tested D3D11 host. A double-wide desktop capture is therefore
not proof that the headset sees the mask. Use the Operator fork for the
currently validated headset-visible fixed-window path. This corrects the
older assumption that the post-framework backbuffer would be copied into the
eyes afterward.

The backbuffer fallback still contains the three renderer fixes found during
development: correct resource-state declaration, recording into the host's
command list, and an RTV descriptor ring that avoids aliasing in-flight
lists.

## Backend compatibility

Built against the plugin API in a UEVR checkout, currently version 2.39, and
verified loading under both a local UEVR fork build and the upstream praydog
nightly. Two things are worth knowing if a build refuses to load it:

- Backends packaged for a specific game may carry a plugin allowlist in
  `PluginLoader::early_init` and will silently skip anything not on it.
- The plugin version has to match the backend's major and be no newer than
  its minor, so building against a newer checkout than the backend you inject
  gets you a "requires a newer minor version" line in the UEVR log.

## UEVR menu and desktop mirror

UEVR's public plugin ABI reports whether the framework menu is open but does
not currently expose a callback for adding widgets inside a native menu page.
CutsceneComfort therefore opens a small companion panel at the upper-right
whenever the UEVR menu is visible. That panel can show or hide the full
cinematic controls without patching UEVR. F7 remains the default direct
show/hide hotkey.

The plugin renders the same ImGui draw data to the VR UI target and the normal
desktop backbuffer while an HMD is active. This was observed simultaneously in
Halo Campaign Evolved's desktop window and Meta XR Simulator. The D3D12
desktop helper is compiled into the plugin and submits each command list once;
UEVR itself is not rebuilt for this feature.

## Building

Needs CMake, Visual Studio 2022 and a UEVR source checkout with submodules.

```powershell
.\build.ps1
ctest --test-dir build -C Release --output-on-failure
```

The script expects a sibling `..\UEVR` checkout by default. Pass
`-UevrRepo C:\path\to\UEVR` when yours is elsewhere. The checkout must have
its submodules initialized.

For the validated final-eye spatial output, use the matching public
`UEVR-6DOF-Window` release as well as installing the plugin.

## Installing

Copy `build\bin\Release\CutsceneComfort.dll` into your game's UEVR profile
plugins folder, for example:

```
%APPDATA%\UnrealVRMod\<GameName>\plugins\CutsceneComfort.dll
```

Settings live in `cutscene_comfort.ini` next to the profile and are edited
from the in game window, no manual editing needed. Two keys are only in the
file, since they are mostly useful for setting things up:

- `simulate_cutscene=1` behaves as though a cutscene is running from the
  moment the game loads, which is handy for sizing the frame.
- `show_ui=0` starts with the window hidden.

The window can be hidden with its close button or the show/hide hotkey
(F7 by default, configurable in the window, set it to None if you want no
hotkey at all). The window also shows what the plugin costs per engine tick,
which should sit well under a tenth of a millisecond.

The release archive also includes `cutscene_comfort.ini.example`. UEVR creates
the real file in the game profile after the plugin saves its settings.
