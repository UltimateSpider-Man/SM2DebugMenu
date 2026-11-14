# SM2 Debug Menu

A native debug-menu mod built for the local retail `Spider-Man2.exe` version
`1.0130.0.0`. Start **Launch SM2 Debug Menu.cmd** in the game directory, then
press the controller's **PS button** or **F10** while the game has focus. The
same button hides it. The debug-menu launcher uses the LTSC compatibility
launcher when that layer is installed.

The menu draws directly into the game's existing D3D12 swapchain, using pure
white text throughout Debug and Dev menus, blue selection, and a dimmed game background.
Titles, headings, option labels and values, disabled entries, hints, and status
messages all use white text. Unavailable actions still show their existing hints
and remain disabled.
The installed menu creates no separate window. Version 2.9.1 supports native
Windows DirectX 12, including the game's bundled Agility runtime. With no
app-local graphics wrappers, it uses the exact System32 DXGI module. A complete
app-local DXVK/VKD3D stack remains supported; incomplete stacks are rejected.
The obsolete LTSC layer is not required on this Windows 10 22H2 installation.

Version 2.9.2 stops refreshing menu-only mission, checkpoint and settings data
while the overlay is closed. Automatic healing, queued actions, restoration and
pending reload transitions continue to run. Opening the menu requests fresh data
before its gameplay controls become available. This reduces background polling;
it does not change the game's graphics settings or shader cache.

Use the D-pad to navigate and change values, Cross to select, Circle to go back,
and L1/R1 to switch between **Debug Menu** and **Dev Menu**. Keyboard fallback:
arrows, Enter, Escape, Page Up/Down.
Version 2.9.0 adds **Weapons > Equip M2 flamethrower** for the original Mary
Jane variants. It requires the accompanying FlamethrowerExpansion assets and
a loaded world or mission. Select it, close the menu, then aim and hold Fire.
The action adds one weapon through the native loadout system without clearing
mission inventory. Version 2.9.3 also assigns and selects the M2 in Mary Jane's
held-weapon slot. Earlier versions could leave the M2 owned while her Sonic
Shotgun remained active. The assignment checks the exact item, native slot
configuration and loaded world, then reads back the selected slot while checking
that the other slots and item quantity were preserved. Mission scripts may change
equipment; select the action again when necessary. Slot assignment alone does
not prove that streamed appearance or firing has finished. See
[the expansion notes](../FlamethrowerExpansion/README.md)
for installation and current gameplay verification limits.
Version 2.8.1 also accepts the exact permanent LTSC startup variant of this
retail EXE. Its gameplay code, image dimensions and runtime binding checks stay
the same. The separate [LTSC startup integration](../LTSC2019Compatibility/README.md)
lets the game prepare compatibility and load this menu when opened directly.
Version 2.8.0 adds **Reload > Start State** and the **Extra Mission Branches**
editor. With Replay Mode off, choose a mission, objective and Active, Available
or Complete state, then explicitly select Reload or Full Reload. Available is
offered only when the mission permits it. The editor supports a main branch and
63 extra branches; empty extra rows are removed when preparing the request.
**Mission Order** now uses the game's actual depth-first traversal. Merely
browsing or editing does not apply mission overrides. Untouched reloads preserve
the existing native override state; this editor does not yet reconstruct every
running branch as the development menu did. A refused native request retains its
preparation, and a fresh explicit request may retry only preparation still owned
by this mod. Replay continues through the separately verified native Replay
action; the retail override array has no verified replay consumer.

**Camera > Enable Camera Shake** changes the verified camera flag and supports
conditional restoration. **Reactions > Disable React Graph** changes the retained
reaction-processing flag. The scalar adapter now covers 73 reflected fields,
one separately verified Boolean and six shared aliases. Newly recovered setting
policies include ragdoll undersampling and terrain Quads Per Meter.

**Screen Capture > Capture With HUD** saves the displayed game frame before this
overlay is drawn, in BMP, DDS, JPG or PNG format under `SM2DebugMenu/captures`.
The debug menu closes when a capture is accepted. This implementation supports
8-bit RGBA/BGRA displays; HDR/float swapchains and the source's Without HUD,
HDR and multipass commands still require separate implementations. Image
encoding runs after GPU completion and may briefly extend a frame.

Version 2.7.0 adds 26 verified scalar settings, bringing the adapter to 71 unique
fields with six shared aliases. Recovered original controls now include lighting,
shadows, terrain, physics, asset loading, save diagnostics, weapons, and difficulty.
Original numeric limits and steps are preserved. **Reload > Keep Current Loadout**
defaults to Off: Reload clears the campaign's default-loadout-applied flag after
native admission. On skips that reset. This applies to normal Reload, Full Reload
and mission Replay; mission scripts can still change inventory. A reset is retained
if the subsequent native request refuses, matching the original behavior.
**Exit to Lobby** uses the retail native unload route; its destination follows the
game's existing routing and does not create a multiplayer lobby. Accepted reload
and exit requests close the overlay automatically.
Version 2.6.0 ports the recovered Debug hierarchy from the user's verified
`H:\Marvel Spider-Man 2 Build 1.5.0\workspace` binaries into this retail mod.
The source contributes 355 pages, 29 root categories and 327 verified child
links. Existing working retail controls remain available. Original controls
without a verified retail implementation show **Unavailable** with their
original help where recovered; browsing a source page does not execute a
development-build callback. This is not full native action parity.
**GameControls** adds **BubbleSkipOption**, **AllowFastTravelDuringDialog** and
**AllowCharacterSwitchDuringDialog** using retained retail settings and their
verified game consumers. The dialog switches remove only the dialog restriction;
other travel and switching restrictions still apply. **Reload > NG+** changes
the current campaign save's NG+ flag immediately, matching the original checkbox.
It does not start a new playthrough or increase the NG+ count. The action requires
a validated current save and rechecks its identity on the engine callback.
Version 2.5.7 keeps **Full Reload** available while Replay Mode is on. It fully
reloads the **current world at its current checkpoint**; **Reload** uses the
selected replay mission and objective and still requires an eligible native
target. The Full Reload hint explains this distinction. Opening either menu
also captures the verified PlayStation SDK's own HID reads, including batches
of up to 32 USB or Bluetooth reports. This closes a route bypassing the earlier
main-executable input filter. XInput capture now updates the packet number when
its visible controls change, so the game cannot retain cached held controls.
Menu navigation continues independently; after closing, release held controls
before pressing them again. The SDK installation retries if the library loads
after the mod and is reported by `input_capture_sony_sdk_installed`.
Version 2.5.6 fixes the Reload mission picker getting stuck on **Waiting for
mission** when no active primary mission is available. With a loaded game,
enable **Replay Mode**, then use Left/Right on **Mission**, **Objective** or
**Graph Filter** to browse the existing catalog. The current primary mission
is preferred when available; otherwise the picker starts at the first catalog
mission in its known graph, or the first nonempty graph. This is a selected
replay target, not a claim that the mission is currently active. Browsing
does not load a mission; **Reload** still requires a validated native replay
target and current mission context.
Version 2.5.5 makes all 29 Debug root categories and every listed Enemies,
GameSystems and NetworkSystems submenu openable with Cross/Enter. The 43 newly
accessible pages say **Controls unavailable in this build** where original
controls have not been implemented. Circle/Escape returns to the parent.
Unsupported options in existing Debug pages are labeled **Unavailable**.
Version 2.5.4 connects **Dev Menu > Tracking Demo** and **Missions and
Activities > Act 2 Missions / Act 3 Missions / Silver Path** to their existing
retail menu pages. Cross/Enter opens each page and Circle/Escape returns to its
parent. The destinations are the exact retained native GotoMenu routes.
This fixes four missing navigation links; native loading actions inside those
pages still need a verified live menu instance and execution binding.
To switch heroes, open **Debug Menu > Hero**, choose a name on **Character**
with Left/Right, then select **Switch character** and press Cross/Enter.
The action becomes available only after a hero is loaded and the dispatcher
has verified the live engine context. The inspected live configuration contains
33 choices, including `Venom` and `MilesMoralesHero`.
Sony DualShock 4, DualSense, and DualSense Edge input is read through shared,
read-only HID handles. No controller output or feature reports are sent.

Version 2.5.1 captures game-facing controller, keyboard and mouse input while
either Debug Menu or Dev Menu is visible. Menu navigation stays active. After
closing, release held buttons/sticks/keys before using them in the game again;
this prevents the closing press from also triggering a gameplay action.
The isolated capture checks cover USB/Bluetooth Sony packets, pending reads,
Raw Input, XInput, held-control release and hook rollback. A physical in-game
capture test remains required for the candidate build.

Version 2.5.2 also blocks the game's native photo-mode Open request whenever
either Debug or Dev is visible. It gates the native permission check used by
controller and pause-menu entry, independently of Windows/Steam input routing.
Closing the overlay restores the original permission checks. An already open
photo session is left intact. Startup retries handle delayed native-system
initialization; diagnostics record installation and blocked-request counts.
The isolated test runs the actual retail Open decision code against a test
object; physical in-game camera-button validation remains a separate check.

## Menus and controls

The combined catalog contains 398 Debug/Dev pages and 2,987 rows. The 1.5.0 port
replaces recovered placeholders and extends the previous 98-page catalog while
preserving its working controls. Its video
reference portion recreates
23 observed pages with 250 rows; six added retail pages and eight controls use
verified settings retained in this executable. The Debug Menu
uses the compact centered layout; the Dev Menu uses the large left panel from
the recording. Connected Playthrough, Press Preview, UX Playthrough, Missions
and Activities, Events/Crimes, and compact Debug Frontend preserve the observed
hierarchy. The supplied build-root screenshot contributes the 29 Debug roots.

**Dev Menu > All retail Dev menus** also exposes all 25 menus and 183 ordered
entries retained in this retail build's title-screen configuration. The index
includes four menus outside the native start tree. Their exact mission,
checkpoint, objective, loadout and save-slot parameters are preserved as typed
metadata. Local navigation works; native loading actions still require a
validated binding to the game's live menu instance. An old Crimes link and the
Propagation audio-test level are absent from this build and remain unavailable.

As requested, reference controls without verified retail bindings remain visible
and disabled. Their values are shown as `Unavailable` in Debug and `N/A` in Dev,
rather than copied console values.
Recording inventories, timestamps, label corrections, and evidence frames are
preserved under `verification/reference-video`.
The November 13 runtime review also found that the retail DebugReactionsUI,
DebugQuadUI, DebugAllyTargetingUI, DebugShootableTrapHunterUI and DebugWallTunnelUI
classes retain return-only UI methods. Their names and labels alone cannot
provide working controls. Reproductions and the new live recording inventory
are under `verification/menu-access`; the remaining `N/A` actions are still
unimplemented, not unlocked by the four navigation repairs.
The static catalog includes the Common, Engine and Hero utility pages. The local
Health and Rendering pages and dynamic Extra Mission Branches editor are counted
separately. Hero contains one of the typed setting bindings. The audit distinguishes implemented controls from
navigation links and unsupported entries; opening a menu does not implement
the actions inside it.

- **Health:** four existing hero/enemy damage/death flags, plus six retained
  display/drop/hit-pause settings, readable health and owned-override reset.
- **Loading:** 27 retained controls for streaming, displays, loading verbosity
  and instantiation/deletion budgets. Values come from registered live settings,
  including four enum descriptors; recorded console values are never used.
- **Hero / Position:** inspect character names and coordinates and save a
  position snapshot; use the retained Respawn Clear setting and open Health
  controls directly. Switching, load/undo and signed movement are implemented
  behind the verified engine-callback guard.
- **Rendering / Window:** viewport and process diagnostics, borderless-window
  override, restore the original window style and placement; runtime motion
  blur and visual-effects processing controls.
- **DebugDraw:** native tile-boundary rendering flag.
- **DebugScriptMessages:** prototype-message processing and debug-message
  logging. These require applicable events; message contexts created while
  disabled may need a reload before enabling takes effect.
- **Weapons:** Infinite ammo Off/On. The native NoReload enum member is not
  selectable because its runtime behavior has not been verified.
- **Traffic > Pedestrians:** Enable Pedestrians controls the game's pedestrian
  loading requests. Existing pedestrians may remain until native unloading.
- **Prefab Actor Systems:** All systems enabled controls managed prefab
  populations. Turning it off can move existing records to the native inactive
  state and change populations. Reset restores the setting, not the old world
  population.
- **Reload:** select the current checkpoint or a named checkpoint from the
  loaded level, request its native reload, copy that selection to the clipboard,
  or exit to the frontend through the game's complete routing handler. Version
  2.5.0 connects 14 of the 20 reference controls, including **Full Reload**,
  **Complete Primary Objective (Preserve Progression)**, mission/objective/graph
  selection for **Replay Mode**, alphabetical browsing order, checkpoint
  filtering, and **Reset Actor Save on Manual Reload**. Full Reload
  uses the native force flag that bypasses level reuse, unloads all loaded-level
  entries, clears retained-state data and releases the old level resource.
  Primary completion uses the current saved primary objective and the normal
  objective-completion method. It does not start a new game or explicitly reset
  inventory, XP or skills; normal mission scripts, rewards and autosaves apply.
  Version 2.8 adds Start State, Extra Mission Branches and native Mission Order
  for manual reloads. **Item Loadout** remains unavailable until its complete
  retail execution contract is verified. **Keep Current Loadout**, **NG+**, and **Exit to Lobby** are
  implemented in the current version as described above.
- **Engine:** export `diagnostics.json` and reset this menu's overrides.

Open **Debug Menu > Reload**, change **Checkpoint** with Left/Right, then select
**Reload** with Cross/Enter. The default is the game's current checkpoint.
Reload can discard actions since the checkpoint. Choices come only from the
current world's live checkpoint table, with native name/key validation; extracted
mission assets do not authorize loading another world. Changing levels resets
the UI selection. Queued requests retain their exact 32-bit identifier and
level/kernel generation and are rejected if the context changes before execution.
Native Reload and Exit to Frontend run only in the verified engine callback;
copying uses the window thread and does not execute an engine action. Accepted
transitions clear this menu's per-hero health/position records before the next
snapshot. No actor is dereferenced after the native request.
Full Reload and primary completion use the same execution-context requirement.
Primary completion also checks the saved active-objective state, actual graph
generations and node identity again when the queued action runs. Its engine
lookup must agree with the independently read handle before completion.

Loading readiness requires the verified native stable state 22. State 0 starts
a load and is rejected. The native Reload API additionally requires its active
mode and level-kernel object; these guards are retained. Startup validation
accepts the executable's original PE base or its actual Windows ASLR base while
preserving the exact file hash, code, type and unwind checks. Live gameplay and
transition checks are tracked separately from synthetic action fixtures.

Health flags apply to the game's global hero/enemy classes. Position snapshots
belong to the current hero instance and are not written to a save file. Native
heal, auto-refill, maximum-health changes, teleport, character switching and the
new typed settings remain unavailable until the genuine engine-thread
dispatcher observes 30 consecutive frames with the expected object, caller,
engine thread and main-thread TLS role. The previous character-switch crash
was traced to a character switch executing on a window thread without the
required game TLS context. A backend guard rejects those calls even if requested
outside the UI. Queued hero actions retain their selected hero generation;
character switches also retain the exact selected name. Reset restores values
still owned by this menu, deferring paired budget restoration when necessary.
The Hero footer explains that Load needs a saved position and Undo needs an
earlier move/load. Switching heroes clears both records. Visual-effects
processing controls do not promise immediate removal of existing particles.
The pedestrian setting is an inline child of the registered Traffic object;
both descriptors, the parent field's type identity and bounds, and the final
Boolean field are revalidated before access. The backend does not call a
pedestrian loader function directly.

The independent `Surge.asi` plug-in was moved to
`Surge.asi.disabled-by-debug-menu` for stability testing: it handles D-pad Up
and writes game values in a background loop. Its original bytes are preserved;
the change is recorded in `verification/surge-isolation.json`. This does not
establish that it caused the earlier crashes.

This is a new implementation for the installed retail PC build. The recording
comes from a PS5 DevKit/TestKit; it supplies menu structure and appearance, not
retail-PC engine bindings. Unsupported entries do not execute engine actions.

## Compatibility and validation

The version-2.8.0 build passed 8,032 scalar checks, 70 menu-model cases,
48 camera cases, 64 mission-selection cases and the four-format image-codec
fixture. Two actual D3D12 preview runs passed rendering, capture, resize and
injected-resource-failure recovery. The capture test checked every output pixel
against the known pre-overlay frame. Package/import checks passed on this Windows
installation, and all 128 recorded build inputs remained unchanged during the
build. These are isolated fixtures and GPU previews; the newly added gameplay
actions still need live in-game validation. Evidence and remaining functional
gaps are in `verification/full-reimplementation-progress/result.json`.

Gameplay access requires one of these two exact executable SHA256 identities:

`54215324e8c1e07c95c832a2d2d7b340d9028ca107ca4e5ad1072cd2809b7975`

Approved permanent LTSC startup variant:
`01fea797e8c957018ccd01b8f5ecda99d4cea13036ee1ed2b06862340cf8a6e9`

At startup the mod checks that hash, requires unique code signatures, verifies
callable function boundaries, and checks actor/component ownership and plausible
values before health writes. Two verified leaf functions have no Windows unwind
record, so their exact known entry RVAs are accepted after the hash/signature
checks. Passive actor/component snapshots use guarded memory reads, without
calling engine getters or updating their caches. Actions requiring native engine
execution are rejected on the window thread. Once an engine callback owns
simulation state, loss of readiness never transfers that ownership back to the
window thread. Window changes and diagnostic exports remain on the window
dispatcher. The new dispatcher changes one verified vtable data slot, preserves
its original no-op method, and never changes executable instructions or game TLS.

Version 2.1.2 removes a startup refusal caused by the game already owning the
first 64 Windows TLS slots. Callback authorization now uses a scoped record on
the verified engine thread's current stack, with thread, stack-bound and atomic
ownership checks. It allocates no TLS slot and retains all game-context checks.
The earlier version-2.3.0 production run verified 4,447 engine frames
and 4,416 completed callbacks with no rejected frames or callback failures.
Both new settings passed read-only checks of their live values and complete
metadata; their current values were On. The settings fixture passed 1,257
checks, including 50 nested-identity rejection cases, and the menu model passed
21 cases. Build/package and LTSC import-loading checks passed. A subsequent
gameplay capture confirmed the new Pedestrians menu with Enable Pedestrians
displayed Off. Read-only inspection confirmed the native value changed from
1 to 0 after controller input. This confirms the live setting path; population
changes and the prefab control's gameplay effects remain unverified. The
successful gameplay capture supersedes the initial black startup capture.
Those earlier results are recorded in `verification/all-debug-options-status.json`.

The catalog audit before the Reload expansion found 14 top-level Debug categories without working
implementations and 327 unsupported reference controls or unresolved navigation
entries across Debug and Dev. Those historical totals predate the current
Reload bindings. No placeholder pages have been counted
as functional menus. See `verification/all-debug-options-inventory.json` and
`verification/debug-disabled-panel-source-map.json`. The short Bandicam clip
adds some Hero reference labels but no contents for the remaining categories;
its separate inventory is in `verification/short-bandicam-audit/inventory.json`.

The Hero switch action remains enabled. In the preceding 2.1.2 run, a
controller-driven change to MaryJane was confirmed in the live manager and
displayed character model; that historical evidence is retained in
`verification/switch-character-status.json`. Complete gameplay behavior for
each character, including Venom, remains unverified. Not every development
menu option is implemented, and these checks do not establish that separate
startup and graphics-driver crashes are fixed.

The LTSC layer has reached the retail title screen and opening scene on build
17763.9487. The reported opening-scene crashes have involved invalid model,
shader, and input-device memory in separate dumps. Removing passive engine
getter calls reduces one possible race; it does not establish that those
crashes are fixed. The character-switch crash now has a reproduced rejection
regression; the earlier unrelated crashes still require separate investigation.

The debug-menu installer does not patch the game EXE, asset archives, TOC or save
files. The separately installed LTSC startup patch is described above.
The existing Overstrike script loader is used with `-scripts`. It reads
`scripts.txt` and calls the mod's `script_enable` export.

## Install, rebuild, and remove

For mission replay, enable **Reload > Replay Mode**, choose **Graph Filter**,
**Mission** and **Objective**, and select **Reload**. The catalog contains five
graphs, 316 missions and 2,648 objectives from this installation. Catalog
membership alone does not enable an action: the adapter requires a verified
active primary objective, and the selected mission must be in the current
loaded graph and pass the game's replay eligibility check.
This adapter admits only the verified native type-1 mission-replay policy;
missions using other policies remain unavailable.
`Mission default` uses the native default-objective route. Replay resolves its
own checkpoint and performs its own reload; Full Reload and actor-save reset
cannot be combined with it. Native scripts and replay/save effects still apply.
Mission and objective sorting support alphabetical browsing and the loaded
graph's native depth-first **Mission Order**. An unavailable native order disables
that picker instead of substituting a different order.

With Replay Mode off, **Filter Checkpoint Selection** restricts the live list
to exact checkpoint names belonging to the current mission. The current-checkpoint
choice remains available. **Reset Actor Save on Manual Reload** clears saved
deleted-actor and actor-flag maps while keeping collected items, immediately
before the manual Reload/Full Reload request. This persistent reset is not
rolled back if the subsequent request fails. It is refused if map ownership,
capacity or the captured world changes, and can run only once per captured
world. Turn the option off to reload without resetting again.

The source fixtures verify call admission and routing in a synthetic host;
they do not establish successful mission transitions in the running game.

The supplied installer adds `scripts/sm2debugmenu.dll`, preserves other loader
entries, and creates timestamped backups in `SM2DebugMenu/backups`. Installation
and removal require the game to be closed.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\SM2DebugMenu\build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\SM2DebugMenu\install.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\SM2DebugMenu\uninstall.ps1
```

Build requires MinGW-w64 `g++` (the default compiler path is in `build.ps1`).
ExecutionPolicy applies only to that PowerShell process. The build also produces
`dist/SM2DebugMenuPreview.exe`, an isolated UI preview with gameplay disabled,
and `dist/SM2DebugMenu.script` for Overstrike.

Overstrike recreates the script list when installing mods. Enable the new
`SM2DebugMenu.script` from Mods Library in the profile pointing to this game
directory if you reinstall mods with Overstrike. Existing local profiles may
point to an old drive; the direct installer does not modify those profiles.

## Checks

`tools/verify_bindings.py` reads the EXE without executing it and verifies the
build, signatures, function starts, health vtables, and live health flag accesses.
It explicitly rejects an obsolete public position-setter pattern that matches
the interior of an unrelated function in this build.

`dist/SM2DebugMenuPreview.exe --overlay-smoke-test` exercises the actual private
D3D12 device and swapchain, rendering, Present/Present1, resize, and hidden
frames without loading the game or enabling its gameplay bindings. The preview
test window is separate from the installed in-game rendering path.

`tools/controller_tests.exe` checks controller report parsing, PS-button edges,
focus changes, held buttons, repeat limits, and malformed reports. The raw
snapshot fixture checks ownership, generation, bounds, and unchanged passive
game data. Evidence is retained under `verification`.

`tools/menu_model_verify.ps1` checks both menu systems, controller-equivalent
navigation, disabled controls, and every Loading row using the actual model and
a CPU-only drawing context. `verification/live-ui-events.jsonl` also records a
real PS-button toggle from the connected controller in the tested game process.

`tools/check_reload_verification.ps1` requires all six Reload-related suites to
pass and checks every recorded input hash against the current source. Add
`-IncludeRelease` after running `tools/verify_release.ps1` to also require the
current DLL, preview, package and metadata to match the LTSC loader checks.
This gate records synthetic results separately from live gameplay validation.

`tools/smoke_installer.ps1` exercises six isolated installation scenarios,
including omitted default paths, repeated installation, byte-exact script-list
restoration, preserving user changes, and rollback after a filesystem failure.

The `debugmenu.log` and `overlay.log` files record binding and renderer setup.
Source references are listed in [SOURCES.md](SOURCES.md).
