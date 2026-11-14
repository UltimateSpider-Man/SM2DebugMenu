# Source references and licensing

The new source files are provided under GPL-3.0-or-later. No development-build
game binaries or leaked game source are included. Windows APIs are called
through MinGW-w64's system headers and libraries.

- [Dear ImGui v1.91.9b](https://github.com/ocornut/imgui/tree/v1.91.9b):
  MIT-licensed drawing library and unchanged official D3D12/Win32 backends.
  Source, LICENSE.txt, and archive provenance are retained in `vendor/`.
- [Linux Sony PlayStation HID driver](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c)
  and [SDL HID controller drivers](https://github.com/libsdl-org/SDL/tree/main/src/joystick/hidapi):
  primary references for Sony USB/Bluetooth report layouts and checksums.
  Controller handling here is a new local implementation.

- [LDD565 / hbgda, SM2ScriptTemplate](https://github.com/hbgda/SM2ScriptTemplate):
  public reverse-engineered actor/component layouts, CRC convention, and baseline
  signatures. Its code is GPLv3; it informed this implementation. Relevant files:
  `game/Actor.cpp`, `game/HeroSystem.cpp`, `game/Component/Health.h`,
  `game/Component/ComponentRegistry.cpp`, `game/Hash.cpp`.
- [ExtCan, SM64-SpiderMan2 bindings](https://github.com/ExtCan/SM64-SpiderMan2/blob/main/package/resources/sm2mario/bindings.ini):
  corrected transform setter signature, checked independently against this EXE's
  exception-function table and disassembly. The older template signature was
  rejected and is not called.
- [Tkachov, Overstrike ScriptsProxy](https://github.com/Tkachov/Overstrike/blob/main/ScriptsProxy/dllmain.cpp):
  plain C `script_enable()` entry point and loader/package format. The existing
  installed loader is used; its code and binaries are not copied into this mod.
- The user's `SM2Launcher-main` and supplied screenshots provided menu labels,
  visual references, and development-build context. Its `dbgmenu` commands are
  not sent to the retail executable.

The four health-rule data addresses and health vtable targets were verified by
fresh static inspection of the user's local retail EXE. They are runtime memory
locations; the EXE file is never changed.

The 73 reflected settings and six aliases come from this EXE's retained registration
records and typed DDL descriptors. `verification/retained-debug-settings.json`
records the live/default distinction, offsets, type codes and enum metadata.
The adapter revalidates those records before each change. A separate raw Boolean
binding uses its independently pinned code consumer and exact Boolean storage;
it does not invent registration metadata.
Version 2.7.0's additional fields, original numeric limits and independently
reconstructed retail descriptors are recorded under
`verification/retail-reimplementation/gameplay-binding-contract.json` and
`gameplay-final-results.json`. Aliases share a single override/restoration owner.
`data/retail_binding_rows.json` supplies only labels and help to the catalog;
executable bindings remain separately verified in the runtime adapter.
The 1.5.0 hierarchy was recovered read-only from the user's
`H:\Marvel Spider-Man 2 Build 1.5.0\workspace` binaries. Seven source binaries
passed the user-supplied torrent's covering piece checks. Inventory, source hashes,
RTTI/registration evidence and unresolved registrations are retained under
`verification/build15-port/`; the reproducible presentation input is
`data/build15_debug_menu.json`. Original DLLs are not loaded into the retail game.
The source graph has 355 pages, 327 child links and no orphan pages; 169 unresolved
registrations are explicitly excluded. Source labels/help do not supply executable
retail bindings. Three SpeedRun GameControls settings have independently verified
retail descriptors and live code consumers in the `gameplay-*` evidence files.
`reload_options_runtime.cpp` separately ports the original NG+ checkbox by
validating current campaign-save ownership and reproducing its Boolean write and
two dirty-bit updates. It never calls the whole-object copy routine with a partial
save object. Its test fixture uses synthetic mapped memory, not the user's saves.
The same adapter reproduces Keep Current Loadout by optionally clearing the
validated campaign default-loadout flag and marking its two native dirty masks
after Reload admission. Normal/full Reload and mission Replay use this preparation
without changing their native call arguments. Exit to Lobby separately reproduces
the source save-mode write and native unload request, preserving the frontend-only
routing flag. Evidence and independent review are under
`verification/retail-reimplementation/`; synthetic runtime fixtures do not establish
successful live gameplay or save compatibility.
The Hero Respawn Clear binding additionally has two reviewed game consumers;
its exact six-field group and the unavailable complex fields are documented in
`verification/hero-binding-evidence.json`.
The six additional controls in version 2.2.0 have exact native consumer evidence
in `verification/debug-runtime-expansion-bindings.json` and
`verification/weapon-binding-review.json`. Infinite ammo validates the entire
native enum while permitting only the reviewed Off/On values to be selected;
owned restoration still preserves a pre-existing native NoReload value.
The Traffic > Pedestrians > Enable Pedestrians path is documented in the local
`SM2Launcher-main/Debug Menu Guide.docx`. Its retail implementation is verified
separately in `verification/traffic-pedestrians-binding-evidence.json`, including
the parent/child descriptor relationship and native request gate.
`verification/prefab-system-binding-evidence.json` documents the managed-prefab
population control and its impact on existing records. Neither setting promises
that restoring its flag restores the previous actor population.

The complete retail Dev hierarchy comes from the installed asset
`configs/system/system_TitleScreenOptions.config`, SHA256
`a906d9345fdc2f83a1024e1ad457c6c85e074b8a5bf4292697ea4e2830b184b0`.
The read-only decoder follows the public
[Overstrike DAT1 layout](https://github.com/Tkachov/Overstrike/tree/main/DAT1).
`verification/dev_retail_menu_manifest.json` preserves its exact typed values
and provenance; source menu data is not a native callable binding.

Reload contracts are independently checked against the same local executable in
`verification/reload-native-actions-evidence.json` and its reproducible static
verifier. The native Reload action supports the current checkpoint or a named
checkpoint in the loaded level; Exit to Frontend uses the complete action that
sets native routing flags before submitting the transition. The adapter validates
PE identity, code/unwind records, RTTI, live object ownership, checkpoint name
CRCs and a captured world-generation token. Static proof alone is not a live
action test. `verification/reload-reference-and-data.json` preserves all 20
reference rows and the separately extracted mission/objective/loadout data.
Those assets are research data, not callable bindings. Earlier primary-objective
and scalar gaps are recorded in
`verification/complete-primary-objective-binding-evidence.json` and
`verification/reload-settings-binding-evidence.json`. The primary gap is
superseded by the separate version-2.5 algorithm below. The current NG+ and
Keep Current Loadout adapters supersede those gaps. Version 2.8 implements manual
Start State and Extra Mission Branches; Item Loadout still requires a verified
retail execution contract.

Version 2.5.0 adds native full-unload proof in
`verification/reload-full-request-implementation.json` and the separate current
primary algorithm in `verification/complete-primary-native-algorithm.json`.
The latter resolves a real active objective using the current save block and
graph generations; it never constructs a ScriptAction or emits a fabricated
OnComplete script event. Runtime fixtures distinguish native-call routing in a
synthetic host from an observed successful game action. The former speculative
state-0 loading guard is superseded by the pinned state-22 readiness producer.

The Reload mission catalog follows typed graph references from the local TOC;
its generator refuses missing objective graphs and ambiguous scoped names.
`verification/reload-mission-catalog-generation.json` and
`verification/reload-mission-catalog-api-results.json` preserve provenance and
lookup checks. Native replay uses MissionNodeGame's replay handler rather than
constructing a script action or submitting a second Reload. The actor-save
adapter uses the game's canonical keep-collected cleanup, with map ownership,
tracking capacity and exact world-context checks. These are separate native
adapters, not inferred from the existence of catalog entries. Their fixtures
are in `tools/test_replay_runtime.ps1` and `tools/actor_save_runtime_verify.ps1`.

The local `bink2w64.dll` was an Ultimate ASI Loader 7.8.0 proxy. A captured
execute-at-zero startup fault followed the game's BinkSetMemory proxy call;
the forwarding pointer was already valid in the dump, so an initialization race
is a hypothesis rather than an established cause. For isolation, the original
RAD video library already present as `bink2w64Hooked.dll` was copied to the normal
filename. The proxy bytes and exact repair manifest are retained in the current
`verification/reload-resumed-*` run. The independent winmm script loader remains
installed. No active `.asi` plug-ins were present during this isolation.

Version 2.5.1's input capture uses the state-packet fields documented in the
primary [SDL PS4 HID backend](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps4.c)
and [SDL PS5 HID backend](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_ps5.c).
Bluetooth neutral reports retain transport metadata and receive the required
HID-A1 CRC. Raw Input follows Microsoft's
[GetRawInputData contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdata).
Only verified import slots in the supported main executable are changed;
the menu DLL's own HID/keyboard imports and system DLL code are preserved.
The existing native XInput initializer at `3222050` stores `XInputGetState`
at `C1DD680`; future resolutions are wrapped through the main import slot.
Already-resolved secondary object caches and alternative dynamic completion
APIs remain coverage limits until observed in game. The independent review is
`verification/input-capture-independent-review.json`; the reproducible CPU
checks are `tools/test_input_capture.ps1`. No physical input was used by those
fixtures.

Version 2.5.2's photo-entry gate is derived from the exact local retail executable
SHA256 `54215324e8c1e07c95c832a2d2d7b340d9028ca107ca4e5ad1072cd2809b7975`.
`PhotomodeSystem`'s native Open function `359A640` calls virtual `+40` before
writing either state DWORD. The controller route `15A6BD3` and system-menu
route `E9EBC9` both reach that function. The gate replaces only vtable pointer
`549A4D8`, with exact code, unwind, RTTI, static-object and ownership checks.
It returns false immediately while the overlay is visible; otherwise it
continues the original native caller without changing TLS or native lock byte
`+46`. Shutdown restores its pointer with CAS and retains failed page-protection
restoration obligations for retry. No executable instructions are patched.
`tools/verify_photo_gate.py` preserves the static contract and
`tools/test_photo_gate.ps1` exercises the actual 52-byte Open decision code in
an isolated host, including old-behavior reproduction and rollback faults.
The evidence is `verification/photo-gate-native-proof.json` and
`verification/photo-gate-tests.json`. These checks do not launch or control the
game and do not establish physical input coverage.

Version 2.8 derives manual mission start-state and branch preparation from the
user's local 1.5.0 modules and the exact retail image. The retained
ObjectiveSystem POD setter, typed graph maps, native SortedByDFT arrays,
AllowAvailable capability, DebugReloaded end reason and persistent refusal
behavior are recorded in
`verification/full-reimplementation-progress/reload-selection-implementation-proof.json`.
`tools/test_reload_selection_runtime.ps1` exercises the production preparation
and Reload/Full Reload adapters, including stale/foreign requests, owned retries,
64-branch capacity, empty-row compaction and absent objective definitions.
The array's replay flag has no verified retail load consumer and is refused;
the existing MissionNode replay adapter remains the supported route.
`verification/full-reimplementation-progress/reload-item-loadout-contract.json`
records the retained GameEntry property setter and nine gameplay loadouts, but
also the missing original hero-initialization override. The connection from that
property to the managed-file path used by hero creation remains unproved. Item
Loadout is not enabled by writing a surviving property without its consumer.

Camera Shake uses the exact CameraSystem flag, allocation identity and retained
runtime consumer documented in
`verification/full-reimplementation-progress/actor-camera-reimplementation-evidence.json`.
Its fixture covers callback admission, changed worlds/allocations, code mutation,
unrelated bits and conditional restoration. The original DebugActor action is
return-only in retail; its name is not treated as evidence of functionality.

The extra ragdoll and terrain policies are in
`verification/full-reimplementation-progress/gameplay-wave2-contract.json`.
`gameplay-reactions-contract.json` in the same directory documents a separately
pinned raw Boolean and its real Disable React Graph consumer. Parry debug drawing
was removed from the inspected retail consumers, so changing its leftover flag
would not recreate the displayed meter.

Screen Capture With HUD is a new D3D12 readback implementation. The source
enum order is BMP, DDS, JPG, PNG, recovered from the original ScreenCapture UI.
It records the real presentation buffer before overlay drawing, retains GPU
resources until fence completion and then uses Windows codecs or a BGRA DDS
writer. `tools/test_screen_capture.ps1` checks actual codec round trips;
`tools/test_overlay_renderer.ps1` checks the production GPU capture and resize
path in an isolated preview. These tests do not establish in-game gameplay
behavior. Original Without HUD, HDR and multipass capture need additional
resource/lifecycle contracts; retained retail CropOutput/SaveCapture routines
alone do not provide the original request API.
