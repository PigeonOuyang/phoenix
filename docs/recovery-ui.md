# Recovery UI

The native C++20 compositor uses the existing Breeze palette, 22 px thin-line
icon geometry, and a 1440 × 1280 logical window at (360, 440) in each 2160 px eye.
Rendering, evdev input, task dispatch, and the screenshot socket remain native.

The status bar has NCM, disabled Bluetooth, disabled Wi-Fi, and disabled Ethernet
icons on its left. Simultaneous upload and download rates occupy the upper-right
header area below the battery, retaining their icon and font sizes.
The page heading reserves that area, and the log viewport begins below it.
Date and time retain a fixed central slot without the weekday.
Net battery power precedes the battery icon,
and the percentage is at the far right. Unavailable power displays `n/a W`,
without a separate power label. Rate units are bytes per second with binary
KiB/MiB/GiB multiples. Upload means headset TX; download means headset RX.

Numeric and Chinese status labels use a 24 px visible height and a shared
vertical center; Chinese uses the heavier CJK face to match Latin visual weight.
Network and bandwidth icons use 28 px visible height and stronger strokes.
Rate units use 20 px visible height, with three-digit rate formatting to retain
readable units in both directions. Each font run is checked separately.
The renderer measures actual font ink and icon bounds and preserves proportions.
Battery percentage follows the actual battery ink with an 8 px logical gap,
including zero, two-digit and three-digit percentages.
Bandwidth icons adapt the endpoint/arrow grammar from Breeze's
[upload bandwidth](https://github.com/KDE/breeze-icons/blob/744be60c294e79719c7506ead95f1f103e539993/icons/actions/22/kt-set-max-upload-speed.svg)
and [download bandwidth](https://github.com/KDE/breeze-icons/blob/744be60c294e79719c7506ead95f1f103e539993/icons/actions/22/edit-download.svg) icons.
KDE's documented percentage style is a badge overlay covering part of the icon,
rather than percentage text integrated into the battery shape. The recovery UI
therefore keeps the requested percentage to the right of the icon.
See [KDE's design discussion](https://community.kde.org/Get_Involved/Design/Frequently_Discussed_Topics#Battery_status_icon_and_charge_percentage).

NCM classification reads the selected gadget UDC, its USB state, the selected
network interface, administrative flags, carrier, operational state and actual
IPv4 addresses. It distinguishes disabled, administratively down, disconnected,
connecting, no IPv4 address, connected, suspended and unknown. Connected denotes
the local USB IPv4 link; no Internet reachability is inferred.

Traffic rates use unsigned sysfs counter deltas and actual monotonic elapsed
time. Each backend owns its history. The first sample, failed reads, interface
replacement, counter resets and backwards clock samples clear the rate until
a new valid baseline is available. Intermediate refreshes retain the last rate;
the regular UI loop updates once per second.

The installation log uses the weighted stage and substep deck. The rolling task
log uses that deck for an installation or a compact rail for a maintenance task
that owns the log. Boot, kernel, services, pstore and saved build logs show no
unrelated task rail. Idle installation shows a waiting message.
Its log viewport defaults to 1970 × 1032 with a fixed Back control. Log and
report readers use measured glyph widths, 27 px text, 42 px rows and a right
scroll arc centered in lens space. Reports show a pinned, illustrative query
above the read-only results; entering a report reads fixed files and never
invokes a shell.

The original body still defines the first and last full-brightness rows.
Above and below it, the text and pane blend to 47% opacity over 28 px.
Context extends to the eye canvas without a circular mask. Log row
colors, numbers and selection keep their original horizontal alignment.
The opaque commands and fixed controls remain in front.

In both readers, a volume tap moves one visual row; holding it begins repeat
after 340 ms and ramps up nonlinearly from 4 to 50 rows/s (20 ms per row
at the cap). Release freezes the selected row and lets the view coast with
a nonlinear slowdown for up to 360 ms. Pressing the same direction during
that slowdown resumes from the current speed and rejoins the original held
speed curve without waiting 340 ms. Only full-screen logs have selection
catch-up: the selected row
accelerates toward the leading edge during this handoff (up to about 120 ms)
without jumping the viewport. The coast travels less far below the cap, stops
at the document bounds, and keeps the selected log row visible. Menus keep their usual repeat
behavior. The full-screen log still enters with Back selected; volume moves
into the log and Power returns to Back. The viewport stays put while the
selected line is visible and scrolls only when the next selection crosses
an edge.

Cairo and FreeType rasterize on the CPU; full GPU acceleration for 2D UI
rendering is not yet integrated. The interface is usable, but smoothness is
not yet optimal. MSM DRM/KMS scans out both eyes in the native 90 Hz mode and
reports actual page-flip timestamps; this does not imply a consistently
rendered 90 fps UI. The readers
cache opaque body row pixels and composite the faded context behind fixed
controls. Reader refresh continues to sample telemetry and task status;
disk inventory is refreshed on return to pages that can use it.
At startup the service continues waiting if the DRM node takes longer than
60 seconds to appear. After the native display mode is ready, the renderer
commits a startup page before reading preferences or inspecting the system
partition. DRM readiness, the startup commit, and the first interactive
commit are recorded in the Recovery service and kernel logs. These markers
identify which part of startup was reached without relying on USB or the UI.
Ordinary pages use a centered, constrained bottom key hint group. The redundant
physical-three-keys note is removed.

At 2160 × 2160 per eye, the measured largest fully visible centered circle
was 1980 px in diameter with the standard facial interface at its default
height and without the glasses spacer. Removing the facial interface allows
more of the image to be seen. This wearing-specific observation is not a
resolution limit or a universal optical boundary; no circular software crop
is applied.

When Recovery does not see the `pico4-system` ext volume label on `super`,
it starts in the full-screen install log. The label is read without mounting
the partition and is evidence of an image, not successful installation or boot.
The first session uses English until the owner chooses a language. The log
follows Recovery boot messages and the on-device install log; browsing history
pauses following. The installer writes its own messages in English. In Chinese,
the display translates known installer stages, common package operations and error
labels. Names, paths, digests and unrecognized tool output remain visible. Turning
on Chinese does not modify the stored logs. See the [locale catalogs](../src/recovery-ui/locales/README.md).
Recovery updates the view when its RAM logs change and reads
task receipts as they are published. Clock and network telemetry still sample once
per second. Power on **Back to Home** opens the language picker until a
choice is saved. An active installer holds the preference writer lock; first-run
language save waits for that lock without stopping the installation.

The installer and image builders inherit Recovery's upstream umask. The
task lock, receipts and logs have their own private modes. Preferences use
ordinary root-owned configuration directories (`0755`) and files (`0644`)
under the upstream `022` umask; creating them keeps shared `var` and `var/lib`
searchable. Builds made with an older installer that passed a
private umask into the build may still contain restrictive cached metadata.
Validate upgrades from those builds with a fresh build root and verify the
installed image and existing data independently.

During installation the strip reads the actual `task.json` stages (seven for prebuilt images, nine for an
on-headset build). A new installation from the host opens the live log once;
returning Home or browsing other pages does not restart the task or force the
log back open. A saved language remains in use. The active stage expands its substeps beneath
the rails without moving the log. The main rail allocates room using timing budgets; its solid fill
advances with measured device counts or completed substeps. Work without a trustworthy total shows
moving texture and a failure retains the last earned fill. If Recovery hands off to normal boot,
successful first boot is verified separately by the host.

The Windows desktop replay uses WSLg and the same native renderer with synthetic receipts.
After building it with `scripts/Start-RecoveryPreview.ps1 -Action Build`, use
`-Action Run -Mode headset -At build/package-install` to reopen the paused build log.
`1` and `2` switch between PC and headset, `[` and `]` switch scenarios, and the left and
right arrows step through the route. Hold Shift with an arrow to jump by main stage,
use Space to play or pause, `L` to change language, and `M` to toggle reduced motion.

`tooling/ui_install_preview.py` prepares an isolated nine-stage replay using
the task-state serializer without invoking the installer. A root-owned
private `/run/overpressure/ui-install-preview` directory and `active` file enable
the replay at UI startup. Preview saves language only to that RAM directory
and holds the installer lock to block real writes. To leave, remove the
`active` file, then restart the UI process; no headset reboot is required.

`scripts/test-recovery-network.sh` checks classification, parsing and counter
history. `scripts/test-recovery-ui.sh` verifies layout, action alignment, and the
shared regular/compact progress behavior. `scripts/test-recovery-progress.sh`
checks cumulative progress and rejected counter sources. `pico4-recovery-ui --render-gallery`
renders all pages and network/progress edge states without modifying task files
or committing those gallery frames to KMS; `tooling/ui_alignment.py` measures the
native glyph pixels and single-line widths.

`tooling/ui_network_validation.py` validates the live rates over authenticated
SSH using bounded transfers from `/dev/zero` and to `/dev/null`, recording each
rate's actual counter delta and update interval. This test does not change
network configuration or use factory/storage partitions.
