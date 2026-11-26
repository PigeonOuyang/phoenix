# Local native Recovery preview

The Windows launcher builds the x64 Linux Recovery compositor and displays one
unscaled logical eye through SDL on the desktop via WSLg. Resizing the window
scales the output, not the logical layout. The window uses the production App,
Cairo painter, Breeze atlas, and four production font files. It links a
fixture-only backend: no Recovery paths are opened, no actions can dispatch,
and preference changes stay in memory. It needs no headset or USB connection.

This is an optional development-workspace launcher. Its current defaults
expect the external workspace's `tools/invoke_rea_linux.ps1` entry point and
`pico4-recovery-progress-design/assets/flows.js`. Neither is bundled in this
Phoenix change. The headset UI does not depend on them. Supply a scenario
source with `-Flows` and four font files with `-FontDirectory`, or use
`-FontImage` with an existing local Recovery image; no private image is
included in the repository.

From the Overpressure checkout root in that workspace, in PowerShell:

```powershell
$preview = '.\devices\phoenix\scripts\Start-RecoveryPreview.ps1'
& $preview -Action Build -MirrorDeps -Flows $scenarioSource -FontDirectory $fontDirectory
& $preview -Action Run -Mode headset -At 'build/package-install'
```

`Build` runs production decoder and native offscreen checks before launch.
`Accept` runs the decoder checks without building a window or needing fonts.
`Run` opens the already-built window only. `All` combines Build and Run.
`Snapshot` writes a representative native PNG without opening a window.
`-FontDirectory` accepts the four unpacked Recovery fonts instead of an image.
`-Flows` selects the approved `assets/flows.js` when it is not in the default
workspace location. `-MirrorDeps` checks Ubuntu package checksums and mirrors
only development packages to the build directory; it does not use `sudo` or
modify the distro. Later builds reuse the mirror without downloading.

The default destination is `out/recovery-preview/` in the Overpressure checkout.
Requirements: Windows PowerShell 7 and Node.js; the workspace's Ubuntu-24.04
WSL toolchain (C++20, Python 3, FreeType, PNG, Cairo, JSON-C, DRM headers and SDL2).
Only the presenter uses SDL. The device entry point and hardware paths are not
retained in the preview executable.

## Keyboard

| Key | Action |
| --- | --- |
| `1` / `2` | PC prebuilt (7 stages) / headset build (9 stages) |
| `[` / `]` | Previous / next scenario within the current route |
| `Space` | Play or pause; replay from the beginning when finished |
| `Left` / `Right` | Seek one replay substep or boundary |
| `Shift` + `Left` / `Right` | Seek to the previous / next main stage |
| `Home` / `End` | Return to idle / jump to this scenario's final state |
| `L` | English / Simplified Chinese |
| `M` | Toggle reduced motion |
| `Tab` | Toggle live log and current task overview |
| `Up` / `Down` / `Enter` | Native volume up / volume down / short power press |
| `Esc` | Close the preview |

The title bar identifies the route, scenario, replay position and locale.
Start in a particular case with `-Mode headset -Scenario download-error`,
`-Locale zh-CN`, `-ReducedMotion`, or `-Play`. For a failed PC readback use
`-Mode host -Scenario hash-error`. In an observed route, step to disk sync
after a write to see unknown activity without invented progress. All numerics
come from the approved synthetic design, never device recordings.
