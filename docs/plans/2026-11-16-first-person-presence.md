# First-person presence, phase 1: the held artifact

**Status:** design, revised after reviews 1 and 2 (branch `feature/first-person-presence`) ·
**Date:** 2026-11-16 · **Issue:** #80 (phase 1 of 4); feeds #81 (VR)

The original game shows nothing in first person. The held item stays hidden
(`AWoTInventory::BecomeItem` hides it, as retail `BecomeItem` did), the pawn's
own body is owner-no-see (`WoTCharacter.cpp:182-194`), and the only "hands"
are the 2D HUD interface. Phase 1 shows the selected artifact in first person,
using its existing pickup model, as an **optional plugin**. It is also meant
to be the reference example for later optional feature plugins, so the core
boundary matters as much as the feature.

## Scope

**In phase 1**

- The selected artifact's pickup model, lower right of the view, drawn with
  its own field of view and pulled toward the camera so it does not clip into
  nearby walls.
- Procedural motion: idle float, sway behind view turns, walk bob, and
  cosmetic feedback when the player presses, holds and releases Fire, plus a
  lower/raise when the selection changes.
- A Settings section with a master toggle (default ON), a motion amount and
  the model's field of view.
- A generalized extension seam surface in the game module (see Architecture
  §2), with the onboarding plugin migrated onto it.
- Guarded packaging and funnel support for a second optional plugin.

**Later phases (outline only, separate designs)**

2. Hands: a hand and forearm per playable class (`AWoTAesSedai`,
   `AWoTForsaken`, `AWoTHound`, `AWoTWhitecloak`), generated art under `Art/`,
   rigged and posed by us. The hand mesh's grip socket becomes the artifact's
   attach point.
3. VR (#81): the same model presenter attached to motion-controller
   components (two hand slots), with no first-person warp: the headset's own
   projection and physical scale apply, and a hand whose tracking is lost
   hides. Whether the second hand is decorative or independently usable is a
   gameplay decision for that phase; two visual hand slots do not imply
   two-handed casting.
4. Other players visibly hold their artifact. The converter already writes a
   per-frame hand position (`Tools/convert/mesh2glb.py:434-437`,
   `sockets.socket0` in `<mesh>.meta.json`; positions only, no orientation)
   that nothing consumes yet. This phase needs the selected item visible to
   non-owners, which is a replication change (`SelectedItem` is
   `COND_OwnerOnly`), so it gets its own design.

**Not in phase 1:** hands, VR, remote players, charge counters on the model,
split-screen or multiple local players (the presenter serves the primary local
player only), and any change to simulation, replication, RPCs or the 2D HUD
hand interface.

## Architecture

1. **All behaviour lives in `Plugins/WoTFirstPerson/`** (runtime module
   `WoTFirstPerson`, enabled in `WoT.uproject`). The plugin links the extension
   seam header and the engine, and no game class. A parity build disables the
   plugin or ships without it; the parity-capture and golden-compare lanes run
   with it disabled, as they do for onboarding.

2. **One general extension surface instead of one struct per plugin.**
   `FWoTOnboardingSeams` already describes itself as "the seed of a general
   improvement-module surface", but its name and one detail do not generalize:
   the Settings screen draws every extension row under a hard-coded
   "ONBOARDING" header (`Settings/WoTSettingsSlate.cpp:925-932`). With a second
   plugin arriving:
   - Rename `FWoTOnboardingSeams` to `FWoTExtensionSeams`
     (`Source/WoT/WoTExtensionSeams.h`), grouped by domain (client windows and
     menus; settings; client presentation; display text and fonts). Consumers
     to migrate together: game startup (`WoT.cpp`), the controller, the
     inventory component, the front-end and in-game menus,
     `UI/WoTMenuNavigation.h`, the settings screen, the onboarding plugin's
     module and widgets, both sets of specs, and `CONTRIBUTING.md`, which
     names the header.
   - Replace the flat `SettingsRows` with `SettingsSections`: each section has
     an `Owner`, a `Header` label callback, an order key and its rows. The
     Settings screen draws one header per non-empty section. Onboarding
     registers its rows in a section headed "ONBOARDING", so its screen is
     unchanged. Keep today's guarantees: registration is unconditional,
     removal is owner-scoped, callbacks are copied at build time, and
     controller navigation still reaches every row.
   - Rejected alternative: a second seam struct for this plugin. It would
     leave two patterns to copy, and the Settings header problem would remain.

3. **New client-presentation seams.** Owning client only, each inert when
   unbound, plain and engine types only. "Local player" below means the
   primary local `APlayerController`'s current pawn, never a server-side AI
   (NPC loadouts go through the same inventory paths,
   `AI/WoTCasterComponent.cpp:87`).

   | Seam | Kind | Fired or bound at |
   |---|---|---|
   | `SelectedItemClass` (existing) | pull `FName()`, **polled every frame** | already bound by `AWoTPlayerController` for local controllers only (`WoTPlayerController.cpp:538`, `:600-606`). It reads the controller's own pawn's local hand model (`GetSelectedItemForInfo`, `:4150`), which the predicted switch updates at once (`DriveHandSelection`) and every reconciliation corrects. Those include `NotifyHandSelected`, `OnRep_Inventory`, ownership set-up through `OnRep_Controller` → `EnsureAngrealHands`, deletion and pawn replacement. `NAME_None` with no pawn or an empty hand. No broadcast sites are added: two reviews each found a selection path the proposed sites missed, and comparing one name per frame makes a missed path impossible |
   | `LocalFirePressed(FName ClassName)` | broadcast | `AWoTCharacter::UseAngreal`, only when a request is sent to the server (after the early-exit gates at `WoTCharacter.cpp:4512-4537`; not for a selected Seal). This is an attempt, not a confirmed cast: the server may still refuse it |
   | `LocalFireReleased()` | broadcast | `AWoTCharacter::CeaseUsingAngreal` |
   | `IsFirstPersonViewActive` | pull `bool()` | bound by `AWoTPlayerController`: true only while a living local `AWoTCharacter` is the view target in normal play. False in the Citadel editor (orbit and first-person editing alike, `WoTPlayerController.cpp:828-836`), the match-end camera, Distant Eye, spectating, death and the front end |
   | `SpawnItemPresentation` | factory `AActor*(UWorld*, FName ClassName, const FTransform&)` | bound by the game module: spawns a local, non-replicated, collision-free game-module actor (`AWoTItemPresentation`, opaque to the plugin) that draws the class's pickup presentation exactly as a world pickup is configured. That is the visual helpers (`WoTRuntimeReadClassVisualPresentation`, `WoTRuntimeApplyActorVisualPresentation`, which bind mesh, style, curvature, skin, scale and fatness) plus the owner lighting properties world pickups copy (`AWoTInventory::ConfigureRuntimeVisualPresentation`, `WoTInventory.cpp:125`): `ScaleGlow`, `AmbientGlow`, `Style`, `bUnlit` and `bStyleTranslucent`, carried so that `UWoTMeshLight`'s owner property readers (`WoTMeshLight.cpp:936-941`) find them. The actor spawns at the given transform and bakes its lighting after its properties are set. The model holds its rest frame: binding a mesh stops playback (`WoTVertexAnimComponent.cpp:319`) and phase 1 starts none. Null when the class has no visual. The caller owns and destroys the actor |

   **Network compatibility.** No new RPC, replicated property or wire-layout
   change. Players of the same release interoperate whatever each one's
   plugin setting is. (Every release still gets a new net version, because
   `WoTNetVersion.cpp:28` hashes the release commit; that is existing policy,
   not something this feature changes.) The factory is the one place the core
   hands a plugin a game-built object, and it is typed as `AActor`, so the
   plugin still links no game class.

4. **The presenter is anchor-based, so VR keeps it.** The presentation actor
   is always *attached*, never moved by hand each frame:
   - An anchor is `(USceneComponent* Parent, FName Socket, FTransform Grip)`,
     with addressable hand slots (`Primary`, `Secondary`). Phase 1 uses only
     `Primary`.
   - **Flat anchor (phase 1):** the local `APlayerCameraManager`'s transform
     component, which follows the final camera every frame, so the attachment
     needs no extra update and has no lag. Procedural motion changes only the
     relative transform.
   - **Hand anchor (phase 2):** the hand mesh's grip socket.
   - **VR anchor (phase 3):** a `UMotionControllerComponent` per hand. An
     attached child is gathered into the motion controller's late update
     (`LateUpdateManager.cpp:101`). A transform copied onto a free actor would
     not be, and would lag the controller.

   The **flat presentation mode** is separate from the shared model
   presenter. It owns a `UWoTFirstPersonCameraModifier`, registered at the
   lowest priority so it runs last, which sets `bUseFirstPersonParameters`,
   `FirstPersonFOV` and `FirstPersonScale` on the view. Phase 3's VR mode sets
   none of them. The presentation actor's primitives get
   `FirstPersonPrimitiveType = FirstPerson` through the runtime setter, plus
   `bHiddenInSceneCapture = true`, no shadow casting and no collision. Mirrors
   therefore keep the port's documented behaviour, where first-person-only
   items do not appear (`PORT_NOTES.md:654-655`; the original did reflect the
   viewer's weapon, and the port already differs there).

5. **Lighting under the first-person warp.** UE warps a first-person
   primitive's rasterized vertices (`BasePassVertexShader.usf:60`), and the
   pixel world position is rebuilt from the warped position. Our mesh masters
   feed that position to the original's light math
   (`Tools/import/mesh_master_bootstrap.py:127`, subtracted from real light
   positions at `:38`), so a warped model would be lit as if it sat where it
   is drawn, close to the eye. The fix is in the mesh master generator: its
   `WorldPosition` input uses "excluding shader offsets", which the engine
   captures before the warp (`PixelPositionExcludingWPO`,
   `BasePassVertexShader.usf:82`). Only `_wire_light` changes: the four lit
   masters (`MeshLit`, `MeshLitMasked`, `MeshLit2S`, `MeshLitT`) share it,
   `MeshModulate` has no lighting position, and `MeshLitT`'s separate
   environment-map position input stays as it is. No mesh master uses world
   position offset or pixel depth offset, so every existing mesh gets a
   mathematically equal position. The value is interpolated per vertex rather
   than rebuilt from the pixel position, so floating-point identity is not
   guaranteed, and a golden-compare run must show unchanged world rendering
   within its normal tolerance. The material translator adds the extra
   interpolant by itself; no project setting changes. `LIGHT_GRAPH_VERSION`
   advances so the existing tagged masters regenerate through the import
   path, with no hand edits.

6. **Framing.** Pickup meshes differ in size and orientation (their draw
   scales range from 0.3 to 1.3, `Tools/import/import_pickups.py:26-70`). The
   presenter fits each model's local bounds to a target size, then applies an
   optional per-class pose row (offset, rotation, scale multiplier). Pose rows
   are authored in `Plugins/WoTFirstPerson/Data/held_artifact_poses.csv` and
   compiled into the plugin as a generated include, following the onboarding
   precedent, because the guarded build cooks no project data assets.
   Unlisted classes use the default pose. A development console command
   (`wot.fp.pose`) nudges the current pose and prints the CSV row, for
   tuning. Size, offset and `FirstPersonScale` are chosen together so that
   every pose and motion extreme stays beyond the 10-unit near plane and
   clear of walls at the player's collision radius. A spec checks the
   extremes.

7. **Procedural motion is cosmetic.** It is driven by local input and view
   state, not by confirmed casts. The unchanged network protocol cannot say
   whether a cast succeeded, and charge changes are not casts: curses drain
   charges (`WoTDecreaseChargesLeech.cpp:56`) and sustained effects spend
   them over time. Critically damped springs integrated by `DeltaTime`:
   - idle float (slow, small);
   - sway: lag behind view rotation changes, clamped;
   - walk bob from the pawn's ground speed, phased by distance travelled;
   - Fire feedback: a kick on `LocalFirePressed`, a small tremble while
     held, and a settle on `LocalFireReleased`;
   - switch: when the per-frame selection read changes, lower the old model out of view, swap,
     raise the new one (about 0.12 s each way); hide at once for `NAME_None`.

   The Motion setting scales every amplitude; 0 holds the model still. The
   motion constants live in one plugin header.

8. **Settings** (`UWoTFirstPersonSettings`, `Config=GameUserSettings`, the
   onboarding settings pattern): `bShowHeldArtifact` (default ON), `Motion`
   (0 to 1, default 1) and `ModelFieldOfView` (default 70 degrees). The rows
   are registered in a "FIRST PERSON" section whenever the module is loaded,
   including while the feature is switched off, so the player can switch it
   back on; the section is removed only when the module shuts down (the
   onboarding rule, `WoTOnboardingModule.cpp:145`). Switched off, the plugin
   holds no presentation subscription, no camera modifier and no actor.

9. **Packaging.** The guarded package allows exactly one optional code
   plugin today (`Tools/dist/build_stripped_pak.py:174`, `SEED_CODE_PLUGINS`),
   and the first-run funnel consumes the descriptor allowlist generated from
   it (`run_first_run_funnel.py:1223`). Phase 1 adds `WoTFirstPerson` to that
   one list, with tool tests for builds that have the plugin enabled and
   disabled.

## Behaviour details

- **Visibility:** the model shows only while `IsFirstPersonViewActive` is true,
  the camera manager is not blending between view targets (the manager's
  actor moves to the pending target before the blend is applied,
  `PlayerCameraManager.cpp:467` vs `:950`; current gameplay cuts, so this is a
  guard), a selection exists and the setting is on. Death hides it at once;
  after respawn the per-frame selection read brings it back.
- **Seals** (Citadel) show their pickup model like any other item. They are
  what is in the hand.
- **Listen-server hosts** behave like clients: everything is keyed on the
  primary local player's own pawn.
- **Invisibility and disguise:** phase 1 draws the model normally. Matching
  those states is a follow-up once seen in play.
- **Zone lighting:** the presentation actor sits at the eye, so `UWoTMeshLight`
  bakes it from the player's own zone and leaf. It re-bakes on its 0.25 s
  timer when the zone or leaf changes or the actor moves more than 128 units
  (`WoTMeshLight.cpp:21-22`), so a brief lighting pop on a zone change is
  expected and acceptable.

## Related finding (separate parity fix, own branch)

`AWoTPlayerController::CalcCamera` (`WoTPlayerController.cpp:956-1010`) adds
the original's walk bob, smoothed eye height, vertical shake and view flash.
The engine calls a controller's `CalcCamera` only while the controller is its
own view target, which happens only in the Citadel edit orbit (`:831`) and
the match-end camera (`:2068`). Possession and client restart make the pawn
the view target (`PlayerController.cpp:865`, `:923`), and
`AActor::CalcCamera` then falls back to the pawn's eyes. So in normal play
those four effects are missing. Review 1 confirmed this from the code. The fix
applies them on the normal pawn path, and its spec verifies through
`APlayerCameraManager::UpdateCamera`. VR (#81) will then need these effects
off for comfort. The held-artifact presenter follows the final camera either
way.

## Tests

Automation specs under `WoT.FirstPerson.*` (headless, leaf names unique,
worlds torn down):

- Every new seam is inert when unbound. With the plugin disabled there is no
  section, presenter, camera modifier or actor. With its master switch off the
  section stays and nothing else exists; switching off, reopening Settings and
  switching back on restores the model.
- The Settings screen draws one header per section; the onboarding section and
  its controller navigation are unchanged.
- Selection: a predicted hand change shows the new model before replication
  arrives; inventory arriving before possession, late inventory resolution, and
  pawn replacement with the same selected class all end with the right model;
  a server-side NPC's inventory changes never reach the presenter.
- Each `IsFirstPersonViewActive=false` path hides the model.
- The flat mode sets the first-person view parameters, and the model's
  attached transform follows the camera manager in the same update.
- The factory's actor carries the pickup's lighting properties.
- Pose rows parse; an unknown class gets the default pose; the near-plane and
  wall-clearance extremes hold for every pose row.
- Packaging tool tests: builds with the plugin enabled and disabled.

Then a golden-compare run for the mesh master change, and a runtime check in a
packaged build with screenshots of a handful of artifacts for Aaron, before
the PR leaves draft.

## Tasks (for the implementation plan)

1. Seam generalization: the rename, settings sections, the onboarding
   migration and `CONTRIBUTING.md`. Build on both platforms; the onboarding
   specs stay green.
2. The client-presentation seams with their broadcast and bind sites, the
   presentation factory, and the mesh master lighting-position change with its
   golden compare.
3. Plugin scaffold: module, settings, Settings section, lifecycle, master
   switch, and the packaging and funnel allowlists.
4. Model presenter, anchors, flat presentation mode, framing and the pose table.
5. Procedural motion.
6. `PORT_NOTES.md` entry, both-platform builds, the full suite, the packaged
   runtime check.

## Decisions taken in this design (owner may overturn)

- Motion feedback is cosmetic, driven by input, not by confirmed casts. The
  alternative needs new network data.
- "Compatible" means players of one release can play together with any mix of
  plugin settings. Joining across releases is governed by the existing
  per-release net version.
- Phase 1 serves one local player.

## Constraints

- Simulation, replication and damage are untouched. Everything here is
  client-side presentation, a material input that is value-identical for every
  existing mesh, packaging allowlists, and seams that do nothing when unbound.
- Exported plugin symbols use `WOTFIRSTPERSON_API`.
- C++ builds on both platforms before a task is done, and only one UE build
  runs per machine at a time.
- Public text (docs, commits, PR) avoids the original setting's proper nouns;
  code identifiers stay as they are.
