# CutsceneComfort handoff

Started at the end of the first build session and updated through the
fixed-spatial-window runtime pass on August 2, 2026. This is the state of
things, what is actually proven versus merely believed, the environment traps
that cost the most time, and where I would go next. The newest evidence in
this file supersedes the older backbuffer and simulator assumptions below.

## What this is

A UEVR plugin that notices when a game hands the camera to a cinematic and
undoes your VR camera tweaks for the duration, then eases them back. It also
has an optional theater frame that masks each eye down to a soft edged
rectangle so a cutscene reads as a physical window in the room rather than a
world wrapped around your face. The window is anchored once on cutscene entry;
it does not follow the head every frame, and the game remains stereo 3D inside
the aperture.

It was written from scratch. The old `CutsceneDetectionPlugin` on the D drive
was read as reference and nothing was copied from it.

## Layout

```
src/Plugin.cpp        detection, state changes, settings, ImGui window
src/VignetteParams.hpp shared physical-space constants and HLSL
src/Vignette.hpp/cpp  standalone D3D12 backbuffer fallback
src/VignetteD3D11.*   standalone D3D11 backbuffer fallback
tests/ShaderCompileTest.cpp  SM4/SM5 shader compile test
CMakeLists.txt        needs a UEVR checkout with submodules, see UEVR_REPO
build.ps1             configure plus build, Release x64
tools/ab_trials.ps1   paired A/B harness for stability questions
runtime/uevr-upstream-01139  extracted upstream runtime used in live tests
runtime/captures      composited-eye and simulator-window proof images
README.md             user facing docs
```

The headset-visible path also changes the Operator fork at
`E:\Github\UEVRMetaXROperator-6DOF-Window`: `PluginLoader` accepts the plugin
and forwards `CutsceneComfort.WindowMode.v1`, while `WindowMode` consumes that
transient cutscene state and renders it into the final eye textures. The
transient state is deliberately separate from the user's normal all-game
Window Mode configuration.

Deployed copies live in `%APPDATA%\UnrealVRMod\<GameName>\plugins\`.
Settings live next to them in `cutscene_comfort.ini`.

## `plugins-for-godot` review

A separate agent inspected `E:\Github\plugins-for-godot`. The useful design
lesson was to keep the physical aperture geometry independent from the world
render and expose explicit dimensions, aspect policy and corner radius. Those
ideas are reflected in the meter-based width/height, optional 16:9 lock and
rounded-rectangle controls.

There was no OpenXR or renderer code worth transplanting. In particular, the
Apple-style inverse-camera compensation in that project was not adopted: it
would cancel the head-relative parallax this mode is intended to preserve.

## What is actually proven

- The normal Release build is clean. A separate `/W4 /EHsc` build reports no
  warnings in this project's sources; the remaining warnings are in UEVR's
  helper headers and renderlib.
- Loads and initializes under two different backends, the local fork and the
  upstream praydog nightly.
- Reaches "detection is live" and fires cutscene entry with the correct view
  target logged.
- Detects real in-engine cutscenes on D3D11. SpongeBob Rehydrated produced
  entry for `CameraMenu (CineCameraActor)` and `CameraActor3
  (CineCameraActor)`, then exit when control returned to the pawn.
- The restore path is measured end to end. From a deliberately non-default
  baseline of `12.5, -3.25, 7.75`, the 1.5 second smooth restore sampled
  `0.111, -0.029, 0.069`, then `4.005, -1.041, 2.483`, and landed exactly on
  the baseline. Aim method 2 and decoupled pitch true were also restored.
- The crash journal survives a forced process termination during a real menu
  cutscene. On the next launch it recovered offsets `12.5, -3.25, 7.75`,
  decoupled pitch true, aim method 2 and UObjectHook enabled before detection
  was allowed to enter the menu cutscene again. Returning to gameplay restored
  all observable values, explicitly saved that baseline to UEVR's
  `config.txt`, and cleared `parked_state`. A further cold launch, with no
  recovery journal, read the same baseline and parked it again on menu entry.
- The full non-render path, settings hot reload and ImGui initialization have
  run under D3D11. Average plugin tick cost in the stable host was 19.9 to
  20.5 microseconds over 2,000 ticks.
- The theater frame renders correctly. A capture of the live double wide
  backbuffer during an active cutscene shows two independent soft edged
  apertures, one per eye, black falling off on all four sides with evenly
  rounded corners, and the scene untouched inside each one.
- The frame does not destabilize the game. Twelve matched pairs alternating
  only `theater_frame` gave 4/6 survival with it on and 3/6 with it off. See
  the README for the numbers.
- The exact current CutsceneComfort and Operator binaries load together on
  D3D11. The log reports `final-eye spatial bridge: Operator`, and the
  Operator status surface receives the requested physical dimensions,
  feather, radius, curvature, surround color and opacity.
- Meta XR Simulator's composited left eye shows the mask around the live game,
  proving it reaches the submitted eye texture instead of only the desktop
  backbuffer.
- Literal tracked-pose movement is now exercised synthetically. Starting from
  identity, the head was translated 0.5 m right and yawed 25 degrees. The
  aperture changed viewpoint while the stored anchor remained at origin with
  its center approximately one meter forward. Returning to identity centered
  the curved window again. This proves the aperture is fixed in tracking space
  and that curvature is centered rather than right-biased.
- `CutsceneComfortShaderTest` compiles the shared shader for VS/PS 4.0 and 5.0
  with warnings treated as errors; CTest passes 1/1.

## What is not proven

- **A person wearing a physical headset.** Meta XR Simulator now proves the
  tracked-pose mechanism and final composited-eye result, but comfort and
  scale still need a human headset check.
- **The current Operator bridge on D3D12.** Its exact binaries are built, but
  the final-eye bridge was exercised live only on the stable D3D11 host.
- **A full in-game cinematic with the new spatial bridge from entry through
  exit.** The detector and smooth restore were already proven across real
  `CineCameraActor` entry and exit, and the new bridge is proven on the real
  menu `CineCameraActor`, but the newest combined path has not yet been watched
  through a narrative cutscene and its visual ease-out.
- **Stock UEVR headset output from the standalone backbuffer callback.** On
  the tested D3D11 host that callback affects the desktop backbuffer but is too
  late to reach the final eye textures. Treat the Operator fork as required
  for the currently validated headset-visible mode.

## Environment traps, all of which cost real time

**Backends are not interchangeable.**

- `E:\Github\UEVR` is a working fork, currently on branch
  `codex/port-afr-openxr-cadence`, plugin API 2.39.
- The untouched `E:\Github\UEVRMetaXROperator` checkout has a hardcoded plugin
  allowlist and silently skips CutsceneComfort. Use
  `E:\Github\UEVRMetaXROperator-6DOF-Window`, branch
  `agent/operator-6dof-window`; its allowlist and final-eye bridge contain the
  required integration.
- The stock upstream nightly is the most useful for plugin work. There is a
  source zip at
  `E:\Github\HaloCampaignEvolved-UEVR\dist\upstream\praydog-UEVR-nightly-01139-74b76bc\uevr.zip`
  and the permanent extracted copy used here is
  `runtime\uevr-upstream-01139`.

**Plugin version has to match.** The plugin reports the version of whatever
headers it was built against. Build against a newer checkout than the backend
you inject and the log says "requires a newer minor version" and the plugin is
skipped. Point `UEVR_REPO` at the checkout matching the backend you actually
run.

**openxr_loader.dll.** Neither game shipped one, and without it UEVR logs
"Could not load openxr_loader.dll" and never brings up VR. Copy the one from
your UEVR build next to the game exe.

**Steam gets stuck.** More than once Steam retained stale app state and routed
later launch requests incorrectly. Restarting Steam clears it. Current runtime
work is Halo-only; do not use another game as a fallback host.

**Halo dies on roughly forty percent of launches** in this configuration,
usually within ten seconds of reaching a rendered state, with no crash log.
This is the single biggest obstacle to measuring anything and it is not
caused by this plugin.

**SpongeBob Rehydrated is the stable D3D11 host.** Launch
`Pineapple-Win64-Shipping.exe` directly with `-windowed -ResX=1280
-ResY=720`, wait about ten seconds, then attach the Operator fork's test
runtime injector. Its game directory also needs `openxr_loader.dll`. The main
menu is a real `CineCameraActor`; loading the first save returns to a pawn,
which makes entry, exit and restoration cheap to repeat.

**`Responding` is not liveness.** `Get-Process ... Responding` returns False
for a busy window pump and I misread that as death several times. Poll the
uevr_mcp HTTP endpoint instead, it is the honest signal.

**Do not drive Meta XR Simulator pose with window keystrokes.** That route did
nothing and originally made the lean appear untestable. The Operator's
`openxr_set_head_pose` service does move and rotate the simulated HMD, and
`openxr_get_head_pose` verifies the resulting pose. Use those APIs for fixed
anchor tests.

## The lean, measured

The frame now has a D3D11 backend as well, which is what made this possible.
Chasing a D3D12 host that would both hold a session and show real 3D was a
dead end; SpongeBob Rehydrated is stable, has real geometry and enters real
`CineCameraActor` cutscenes, so bringing the frame to it was the cheaper move.

`tools/lean_trials.ps1` runs the measurement. A moving scene changes between
any two captures, so a single before and after cannot separate a viewpoint
translation from the scene simply animating. Each repetition therefore takes
one pair with no change between the captures and one pair with a translation
between them, using the same delay both times. Animation contributes equally
to both, so the difference between the two series is what the translation
added.

Six repetitions, on the framed stereo image, during a real
`CameraMenu (CineCameraActor)` cutscene:

```
control mean scene diff (no translation) =  7.989
test    mean scene diff (translation)    = 21.405
test exceeded control in 6 of 6 reps
largest mask diff seen anywhere          =  6.682 out of 255
```

A forty centimetre lateral translation produced roughly two and a half times
the scene change of a time matched control and won every single repetition,
while the mask band stayed within a couple of percent of black. That is the
thing the feature claims: translating the viewpoint changes what you see
through the aperture, and the aperture itself does not move with you.

Two honest caveats. The translation came from `VR_CameraRightOffset` rather
than a head in a headset, though both land in the same world space camera
position. And the scene sample overlapped UEVR's own menu panel, which is
static, so it dilutes the measured difference rather than inflating it; the
real separation is larger than the numbers above.

## An earlier attempt that came back empty, and why

`tools/lean_capture.ps1` grabs the game's client area to a png.
`tools/lean_compare.ps1` diffs two of them in two regions: a patch inside the
left eye aperture, where a viewpoint translation should show up, and a band
along the outer edge that is solid mask in both, which should not move because
the mask is drawn in screen space.

The lever is `VR_CameraRightOffset` through the MCP settings endpoint, which
translates the VR camera laterally in world space. That is the same view
translation leaning produces. It can be set mid cutscene and it sticks,
because the plugin only writes offsets on entry and on restore, not per tick.

Run with the frame fully faded in, a no change control first, then two
translations:

```
CONTROL  P0 vs P0b, nothing changed   scene 0.000   mask 1.199
TEST 1   offset 0 vs +40cm            scene 0.000   mask 1.306
TEST 2   offset +40cm vs -40cm        scene 0.000   mask 0.282
```

Every scene number is zero, including across an eighty centimetre swing. The
offset really was applied, the endpoint confirmed `"set":"40"` each time. The
reason nothing moved is that the only D3D12 scene that could be held long
enough was Halo's menu, and `uevr_get_camera` reports it sitting at
`(0, 0, 64)` with zero rotation. It is a flat backdrop, not a 3D scene
rendered through the VR camera, so there is no depth in the shot for a
translation to produce parallax from.

That was a null result about the test location, not about the plugin. The rig
behaved correctly: the control showed a perfectly static scene, which is
exactly what a still menu image should look like.

The fix was not to keep hunting for a better D3D12 host but to give the frame
a D3D11 backend and go to the stable game, which is what the section above
describes.

## The measurement lesson

This is worth reading before touching the stability question again.

At one point the frame was on and the game died in five seconds, the frame was
off and the game ran a hundred and fifty. That is a clean looking result and I
reported it as decisive. It was one sample per arm against a host that fails
about half the time on its own, so it carried no information at all. Six pairs
later the effect had vanished and the frame arm was very slightly ahead.

`tools/ab_trials.ps1` exists so this does not have to be relearned. It runs
matched pairs, times each from the same milestone, and prints a summary.

```powershell
.\tools\ab_trials.ps1 -Pairs 10 -WatchSeconds 90
```

## Bugs fixed during the session

All three were genuine and are worth knowing about if the render code gets
touched again.

1. **Wrong declared resource state.** The backbuffer was declared as
   `RENDER_TARGET`, but the host has not transitioned it yet at the point the
   callback fires, so it is still `PRESENT`.
2. **Own command list racing the host.** The original design built a command
   list and submitted it to the game's queue, which lands out of order against
   whatever the host is recording at that moment. It now records into the list
   the host passes in.
3. **Descriptor aliasing.** Two RTV descriptor slots were rewritten every
   frame while command lists from previous frames could still reference them.
   They now rotate through a sixteen slot ring.

There is also a safety net worth keeping: before applying any temporary
change, the plugin atomically journals camera offsets, decoupled pitch, aim
method and UObjectHook state into its own ini. It clears the journal only once
everything is restored. Recovery is deliberately delayed until engine ticks
are underway because UEVR performs a second config load just after plugin
initialization; recovering in `on_initialize` produced a truthful log and was
then silently overwritten. Detection stays disabled if journal validation or
read-back verification fails. The restored baseline is explicitly saved
through UEVR before the journal is cleared; otherwise a later clean launch can
resurrect temporary values from `config.txt`. The second-config-load bug,
missing aim/pitch recovery, and stale UEVR config were all found by extending
the forced-restart test through one additional cold launch.

## Where I would go next

Roughly in order of value.

**Confirm comfort and scale in a physical headset.** The fixed anchor and
parallax mechanism now have synthetic tracked-pose coverage, so the remaining
question is human comfort rather than whether the window follows the head.

**Rerun the final binary on D3D12.** The exact latest DLL contains the new
full-state crash journal. Its render code is unchanged, but one load/render
check on a reliable D3D12 host would close the final-binary coverage gap.

**Then, improvements worth making:**

- *Automatic cutscene aspect matching.* The aperture is now sized in physical
  meters and can optionally lock to 16:9. Reading a sequence's actual aspect
  would make 2.39:1 and other authored formats automatic.
- *Ease the frame in with the camera cut, not the state change.* Right now the
  fade starts when detection flips. Starting it on the actual view target
  change would feel tighter.
- *Detection beyond the view target.* `LevelSequencePlayer` and
  `MatineeActor` play and stop, and `PlayerController::SetCinematicMode`, are
  all reasonable secondary signals. The old Lua version used a reference
  counted stack across several of these and that idea was sound even if the
  implementation was not.
- *Exercise the final-eye bridge on D3D12.* The current exact binaries need a
  reliable D3D12 host and the same composited-eye plus pose-offset checks.
- *Expose optional follow strength only as a separate mode.* The requested
  default is now a rigid tracking-space anchor. Any partial follow or lag
  experiment must not silently weaken that behavior.
