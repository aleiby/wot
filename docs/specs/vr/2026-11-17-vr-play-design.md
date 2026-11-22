# VR play streamed from the PC (design, revision 3)

Issue #81, stage 2. Date: 2026-11-17. Status: approved by the owner on 2026-11-17, with
decisions A-E and I as recommended, F and H revised, G deferred and J settled (section
4). Implementation follows on this branch.

The Windows game runs on the PC; Steam streams it to Steam Frame with no game
involvement. VR and flat players share servers and the same movement, collision,
inventory, casting, damage and match rules. The optional `WoTVR` plugin changes local
input and presentation only; physical walking in the play space is local movement
input, like a stick (section 6).

## 1. Goals and boundaries

- Tracked stereo at the confirmed scale, with standing/seated calibration.
- Frame controller play: casting, selection, smooth movement, snap/smooth turning,
  jumping, scores and menus; optional comfort vignette.
- Readable HUD, messages, onboarding and menus; controller-attached hands and artifact
  through the separate PR #82 work. The desktop window keeps Unreal's default VR output.
- Physical walking in the play space moves the player's body, and walls stop it.
- One PC executable. The mode is chosen at startup; the target is switching between VR
  and flat at any time, in place (decision F). Flat-only builds and the plugin
  disabled/absent retain today's behavior.

Stage 1 established acceptable headset fidelity and performance for proceeding, not
controller input, competitive fairness, HUD comfort or a measured frame-time budget.
Stage 3 covers running the Windows build on the headset through Proton, with a native
build only if needed; it is separate from this stage.

**Scope constraint:** no new gameplay actions. Reserve unsupported crouch/world-use
controls; physical lowering changes the view, not the hitbox. UI Use means confirm;
world pickups/interactions retain their rules. Any missing shared action needs evidence
from the shipped scripts and separate parity work.

## 2. Owner rulings so far

Fixed inputs from the two headset tests on 2026-11-16; proposed adjustments remain proposals.

| Subject | Ruling or observation |
|---|---|
| Scale | Use `vr.WorldToMetersScale 52.5`. Approximately 16 units per foot; a 92-unit player with eyes at 88 units reads as about 1.68 m to the eyes. Confirmed in the headset. |
| Origin | Eye-level/Local, `vr.TrackingOrigin 2`, with `vr.HeadTracking.ResetPosition` for recentring. Confirmed in the headset. |
| Posture | Seated testing put the virtual floor about 0.5-0.6 m below the real floor. Standing/seated options and height calibration are required. |
| Eye placement | Historical rationale: `BaseEyeHeight 42` against `CollisionHeight 48`, six units below the top near the model's hair line. A VR-only downward view offset to the model's eyes was proposed. Simulation eye height stays unchanged; 42 is the normal standing baseline. |
| Projectile origin | Keep the existing eye-based origin; do not move projectiles to the artifact/controller. |
| HUD | The flat HUD was unsuitable: it needed moving farther away and likely needs a different, skeuomorphic treatment. |
| Controllers | No Frame input in the tests: no OpenXR action mappings; mouse and keyboard were used. |
| Presentation | Headset performance and fidelity were good enough to proceed. |

The owner ruled on the section 4 decisions on 2026-11-17; section 4 records each ruling.

The port initializes capsule half-height **46** and `BaseEyeHeight` **42**, giving 92
total and 88 to the eyes (`Source/WoT/WoTCharacter.cpp:170`). The historical 48-unit
observation does not authorize resizing the capsule. State-dependent eye heights remain
simulation behavior; VR does not force 42 in every state.

## 3. What exists

Port references are repository-relative, inspected at `0414c37`. PR #82 references use
`feature/first-person-presence` snapshot `a1cb330`, not promised merged APIs. Only port
files are cited; installed XR sources informed the input contract without being reproduced.

| Area | Current behavior and consequence |
|---|---|
| Input | Enhanced Input objects coexist with legacy keyboard/mouse mappings (`Config/DefaultInput.ini:86`, `:93`). The pawn binds movement/selection (`Source/WoT/WoTCharacter.cpp:4424`); the controller owns Fire/Jump edges for release/respawn across pawn loss (`Source/WoT/WoTPlayerController.cpp:4504`). |
| Gamepad | C++ creates gameplay/editor/team-selection contexts calling existing commands; context changes release held edges (`Source/WoT/Gamepad/WoTGamepadLayer.cpp:18`, `:175`, `:203`). Bindings: `Source/WoT/Gamepad/Resources/ControllerLayout.csv:1`. |
| Casting | `Fire()` dispatches by state, including respawn/restart (`Source/WoT/WoTPlayerController.cpp:1724`). `UseAngreal()` gates locally and sends `ServerUseAngreal(GetControlRotation())`; the server validates rotation, keeps hard-landing/match-end gates, sets rotation and invokes inventory use (`Source/WoT/WoTCharacter.cpp:4485`, `:5012`, `:5028`). |
| Launching | Source-pawn view rotation; actor location plus state eye height, class-specific rotated launch offset and spawn adjustment (`Source/WoT/Inventory/WoTProjectileLauncher.cpp:118`). Eye-based does not mean identical zero-offset launches. |
| Selection | Next/previous crosses slots/groups; direct groups and server-validated selection exist (`Source/WoT/WoTCharacter.cpp:4633`, `:4665`, `:5091`). Last-artifact quick-swap is new local UX. |
| Movement | Ground movement uses control yaw; swimming uses full control rotation and held Jump accelerates upward (`Source/WoT/WoTCharacter.cpp:5454`, `:5484`). No player crouch/general world-use binding was found in the inspected input paths. |
| Camera | Pawn camera then controller effects (`Source/WoT/WoTCharacter.cpp:3110`); bob, smoothed eye displacement and vertical shake alter location, while roll shake writes control rotation (`Source/WoT/WoTPlayerController.cpp:972`, `:5242`). A final camera filter alone is insufficient. |
| HUD | Layout commands coexist with direct Canvas drawing; `DrawHUD()` also advances state (`Source/WoT/WoTHUDDrawCmd.h:29`, `Source/WoT/WoTHUD.h:350`, `Source/WoT/WoTHUD.cpp:2108`, `:2520`). No complete render-target adapter exists. |
| Windows | Onboarding hosts Slate directly on the viewport (`Plugins/WoTOnboarding/Source/WoTOnboarding/Private/WoTOnboardingModule.cpp:473`, `:568`); Canvas capture misses it. |
| Optional features | Inert delegates, queries, settings and overlay focus hooks exist (`Source/WoT/WoTOnboardingSeams.h:9`). `CONTRIBUTING.md` defines optional plugins; `Plugins/WoTOnboarding` is the runtime example. |

## 4. Owner decisions

Ruled on 2026-11-17. A-E and I were approved as recommended; F, G, H and J record the
owner's changes.

| Decision | Options | Ruling |
|---|---|---|
| **A. Aim source in v1** | Head direction; controller direction with eye origin; controller ray choosing a target for eye aim. | **Approved: head direction**, sampled before simulation casting, with a trigger pull. Controller aiming follows fairness review. |
| **B. HUD treatment** | Soft-attached panel; wrist display; held artifact card. | **Approved: soft-attached panel for v1**, using existing frames/icons/text and keeping essential status visible; larger world-locked menus/scores. Other treatments share its data contract. |
| **C. Turn defaults** | Snap; smooth; physical only. | **Approved: 30-degree snap**, optional smooth yaw initially 90 degrees/s, physical turning always available; no artificial pitch/roll. Values need tuning. |
| **D. Calibration flow** | Automatic estimate; posture choice plus neutral pose; full floor-touch calibration. | **Approved: standing/seated choice and neutral-pose recentre**, optional standing eye-to-floor measurement. Save preferences; recalibrate after reference-space reset. Do not promise seated floor alignment. |
| **E. Artifact selection** | Cycle plus quick-swap; radial plus quick-swap; wrist carousel plus quick-swap. | **Approved: off-hand radial plus quick-swap**, retaining bumper cycling and existing groups/slots/selection commands. No match pause. |
| **F. Mode selection** | Automatic only; flags only; Auto/Flat/VR preference plus launch overrides; switching at any time. | **Revised: switch at any time.** The Auto/Flat/VR preference and `-vr`/`-novr` (which wins) choose the startup mode; Auto requires a usable HMD session. The owner's target is switching seamlessly between VR and flat at any time, in place: in a match, in menus or while dead, with no rejoin or front-end trip (section 14). Delivery may be staged, so every hook, rig and policy attaches and detaches at runtime from the start. |
| **G. Lowered view versus literal centre-of-view firing** | Zero model offset; measured lowered view with an accurate simulation-ray marker. | **Deferred by the owner.** v1 uses zero model offset, so rendered neutral eyes sit at the simulation eye; no per-model measurement. |
| **H. Physical head displacement** | Unrestricted leaning; bounded tracking with fade; rotation-only; body follows the head. | **Revised: room motion moves the body.** Physical walking drags the capsule through ordinary movement input, so walls and obstacles stop it, and the view fades instead of passing through walls (section 6). |
| **I. Movement reference** | Head-relative; off-hand-relative; calibrated forward. | **Approved: head-relative default**, preserving view-directed swimming. Offer controller-relative ground movement after verifying only the input basis changes. |
| **J. Desktop window** | Clean mono capture; eye crop; Unreal's default. | **Settled: Unreal's default.** The desktop window keeps Unreal's default VR spectator output. Steam streams the game to the Frame by itself, so the game implements no streaming output (section 12). |

## 5. Architecture and runtime setup

### 5.1 Optional plugin and approaches

Use `Plugins/WoTVR/`, runtime module `WoTVR`, with a saved master enable preference.
The game exposes generic local-player seams and never depends on/names `WoTVR`, OpenXR
or the Valve extension. VR owns tracking, input, calibration, presentation and mirror;
ordinary game classes remain gameplay authority.

| Approach | Assessment |
|---|---|
| VR branches in core classes | Reject: violates optionality. |
| Separate VR controller/pawn classes | Reject: unnecessary class combinations and network risk. |
| Local rig/adapters on ordinary classes | Recommended: mixed clients, one gameplay body, explicit seams. |

Class selection is server-owned (`Source/WoT/WoTGameMode.cpp:204`, `:737`); a listen
host's HMD must never select VR classes for joining players.

### 5.2 Generic hooks in the game module

Names are contracts, not final signatures. Extend PR #82's generalized surface if
landed; otherwise coordinate extraction. Do not copy unmerged code or add a rival registry.

| # | Hook | Current/default behavior | Optional consumer behavior |
|---|---|---|---|
| H1 | Local-player lifecycle/input registration | Normal bindings, possession/camera; no extra object without a consumer. | Possession-independent rig/UI with neutral or gameplay anchor; lifecycle contract in section 14. No server rig. |
| H2 | Rotation-only aim provider | Current control/view rotation. | Constrained tracked aim before cast/movement/view processing; no supplied origin, target, damage or cadence. |
| H3 | Movement-reference/look policy and extra local movement source | Existing ground yaw, swimming, mouse/stick and look preferences; no extra source. | Selected basis/artificial yaw; room-motion movement input summed with stick input before the legal clamp (section 6); suppress flat look assistants while tracking owns aim (section 8), preserving state gates/acceleration. |
| H4 | View/effect policy | Existing camera, bob, shake, FOV, near plane and view targets. | Stereo composition, independent stereo near-plane ownership, synthetic-motion removal and special-view adaptation (section 6). |
| H5 | Read-only HUD snapshot/layout/destination | Existing Canvas layout, visibility, timing/order. | Shared VR/flat presentation under section 10's update/draw contract. |
| H6 | Local UI host/focus routing | Viewport Slate and existing focus. | Offscreen scope, one Slate user, Frame pointer/navigation, chat/text-entry hosting and modal ownership (section 10). |
| H7 | Presentation anchor provider | PR #82 camera anchors, once available. | Tracked grips/validity and mode flag disabling flat warping/procedural position motion. |
| H8 | Command adapter/selection snapshot | Existing Fire/Release, Jump/Release, selection, scores/menu. | Frame actions/radial/quick-swap invoke the same commands; no new simulation capability. |

Use plain/engine types. Register by owner/local player; reject conflicts on exclusive
view/aim/input slots visibly, independent of callback order. Read-only observers may
coexist. Empty hooks preserve flat behavior and timing, and every consumer can register
and unregister at any time (decision F). Parity runs disable the plugin.
Later implementation records optional differences in `PORT_NOTES.md` and updates guarded
packaging; code-plugin seeds are explicit (`Tools/dist/build_stripped_pak.py:174`).

### 5.3 OpenXR and Frame support

`WoT.uproject` has no explicit OpenXR entry; the tests' temporary entry is not release
configuration. Commit reproducible enablement/packaging of `WoTVR`, stock `OpenXR`,
`EnhancedInput` and a verified Frame extension for supported PC clients. Dedicated
servers and flat-only builds must not require an HMD or initialize XR.

Inspected stock OpenXR is disabled by default, provides HMD/input modules, supports
extension profiles and explicit Enhanced Input context attachment. Register all Frame
contexts before session action sets attach; possession-time mapping alone is too late.
Toggle registered gameplay/UI/selection contexts later; action-set changes rebuild XR.

The inspected Valve extension is disabled by default, loads at `PostConfigInit`, depends
on OpenXR and registers `FrameController_*` keys, requesting optional
`XR_VALVE_frame_controller_interaction` with profile
`/interaction_profiles/valve/frame_controller_valve`. The prefix is not `ValveIndex`.
It supports Win64 but is absent from the project's engine; its engine-private rendering/
OpenXR dependencies mean binary compatibility with the patched engine is unproven.

**Integration gate:** establish compatible, reproducible engine installation/approved
distribution; do not assume permission to vendor its source. Verify the streamed PC
session's actual profile/extension. A compatibility profile needs verified mappings,
not guessed aliases/gamepad emulation. Unsupported controllers get a clear message and
flat option; keyboard/gamepad VR may remain diagnostic.

Boot: apply master preference/flags; register profiles/actions; attempt XR; select the
startup mode; create the neutral rig/UI; calibrate; join/host. Auto falls back to flat
without a usable session; explicit `-vr` failure offers a recoverable front-end error and
Flat action. Absent/disabled plugin means flat; `-novr` wins if both flags are present.
Later switches use section 14's in-place transitions. Whether the streamed session stays
usable while stereo rendering is off, so switching back is quick, needs headset
verification (section 16).

## 6. View, tracking and calibration

### Coordinate contract

Keep simulation eyes, calibrated tracking origin and rendered stereo eyes distinct.
The local-player rig/UI works without a pawn or `AHUD`: the front end has neither and
uses a different controller (`Source/WoT/WoTFrontEndGameMode.cpp:7`). Its neutral anchor
provides a stable view, calibration and pointing before possession, on return and during
recovery/pawnless travel. Possession selects the ordinary pawn's gameplay anchor;
world-bound objects rebind under section 14 without losing local-player UI ownership.

At neutral posture, rendered centre eyes equal the simulation eye, with no per-model
offset (decision G deferred), or the neutral anchor before possession. Add calibrated
tracked translation, rotated by an independent artificial heading; compose that heading
with HMD orientation. The body follows horizontal translation (room motion, below).
Do not inherit pawn camera pitch/roll or reapply head-derived control yaw. Turning changes
this heading once; derive legal simulation rotation before movement/casting. XR supplies
eye separation/projection. Game-thread pose drives simulation; late update affects
rendering only. Convert once at 52.5 units/m, including controllers/UI: 0.10 m is 5.25
units. Geometry, capsule size and movement/projectile speeds stay unchanged.

### Posture and recentre

1. Choose Standing/Seated; seated neutral eyes use standing avatar height.
2. Confirm comfortable forward/neutral Local pose, anchored at rendered eye height.
3. Optionally enter/measure standing eye-to-floor distance with a controller; show its
   difference from avatar height. Local origin alone cannot discover the real floor.
4. Menu Recentre updates tracking position/heading with compensation preserving pawn
   position/current world aim. Repeat calibration deliberately chooses a new neutral
   posture; it follows the input-release contract in section 14.

At fixed scale, aligning floors and preserving avatar eye height cannot be guaranteed
for seated users or different standing heights. Prioritize avatar eye reference;
floor-aligned/free-height views need another fairness decision, not a scale adjustment.
Save posture, handedness and tuning; pose origins are session-local.

Per-model view offsets (decision G) are deferred: v1 uses zero offset for every class.
If G is taken up later, measure each class's eye midpoint in its imported standing pose,
apply the signed difference from normal `BaseEyeHeight` to view/presentation only, and
leave state eye heights, hit regions, projectile origin and underwater tests intact.

### Room motion, near plane and effects

**Room motion moves the body** (decision H). Each local frame, the horizontal offset from
the capsule axis to the tracked centre eye, in world space after calibration and heading,
becomes movement input toward the head through H3. It is summed with stick input and
clamped to the legal input range, then follows the existing movement path and transport:
the server applies collision, steps, ledges, water, speed and acceleration as it does for
a stick, in every movement state. Nothing teleports the capsule or sends a head position,
so walls and obstacles stop the body and walking never adds speed. The rig absorbs the
capsule's displacement toward the head, never more than the head's offset, so movement
caused by walking leaves the view on the physical head, while stick, knockback and other
movement carry the view as usual. Movement input steers acceleration, so a follow
controller (a deadband, proposed 2 units, plus position and velocity gains) brakes as
well as steers, settling the body without drift or oscillation. Physical walking, about
1.4 m/s or 74 units/s, is far below ground speed, so the capsule normally trails the head
by a few units.

**The view never passes through walls.** Rendered eyes follow the tracked head while it
stays within a follow gap of the capsule (proposed 8 units, about 15 cm) and the swept
path from the capsule's eye to each rendered eye is clear. When a wall or obstacle stops
the body, or the head outruns it, the view fades to black beyond the gap or on contact
and invites a step back; it never pushes the camera against an invisible wall or shows
the far side. Vertical head motion changes only the view (no crouch, section 1): rising
more than a small margin above the standing eye, or sinking into the floor, fades the
same way. Because the body follows the head, leaning around a corner exposes the capsule
as a flat player's peek does; the follow gap bounds any head-only lead. Teleports,
respawn, travel and recentre re-anchor the rig so the head is over the capsule (section
14). Gap, deadband, gain and fade distances need headset tuning.

**VR owns the stereo near plane**, independent of selected artifact, hand visibility,
tracking or whether `WoTFirstPerson` is installed. Its flat modifier currently changes
the whole view while a model shows (PR #82
`Plugins/WoTFirstPerson/Source/WoTFirstPerson/Private/WoTFirstPersonCameraModifier.cpp:16`;
`Plugins/WoTFirstPerson/Source/WoTFirstPerson/Private/WoTFirstPersonTuning.h:7`). At 52.5
units/m, 10 units is about 19 cm. Choose/validate an explicit stereo value for close
geometry, depth precision and boundary fade; do not inherit a hand-dependent default.
The flat modifier must not override VR's policy; flat near-plane behavior stays intact.

| Existing effect | VR policy |
|---|---|
| Walk/swim/landing bob, cosmetic eye smoothing | Remove synthetic HMD displacement; keep actual movement/state eye heights. |
| Vertical/roll shake | Remove cosmetic pose/aim motion; retain damage/impulse and use audio, restrained haptics/status cues. |
| Fog, underwater tint, flash, visibility impairment | Preserve information restrictions in both eyes; validate stereo post-processing. |
| Forced turn/pitch/pose constraints | Keep gameplay/control consequences and responsive head tracking. Use stable framed views/fades for involuntary travel/roll; show constrained aim honestly. |
| FOV/zoom | Preserve restriction/magnification in bounded viewing surfaces, without changing HMD projection/IPD; verify every gameplay use. |
| Death, end orbit, remote/editor views | Stable world-space viewing surface for the non-head camera, with transition and accessible controls. |

Forced yaw, roll shake and post-view constraints use different paths
(`Source/WoT/WoTPlayerController.cpp:1806`, `:4692`, `:5242`); filtering must preserve
state processing. Never apply gameplay-forced rotation to the rendered head; avoid
motion blur and artificial HMD FOV animation.

## 7. Frame input and coexistence

### Proposed right-handed binding table

Exact inspected extension keys; streamed runtime delivery remains unverified. Left-hand
casting swaps trigger/hand roles and anchors, with independent stick-layout preferences.

| Action | Frame control / key | Behavior |
|---|---|---|
| Move | Left stick, `FrameController_Left_Thumbstick_X`, `FrameController_Left_Thumbstick_Y` | Existing movement; dead zone/normalized magnitude, no speed multiplier. |
| Turn | Right X axis, `FrameController_Right_Thumbstick_X` | Snap/smooth yaw; right-stick Y has no gameplay pitch. |
| Jump / swim up | Right A, `FrameController_Right_A_Click` | Jump/StopJumping including respawn; held swim-up once per frame. |
| Crouch | Right B, `FrameController_Right_B_Click` | Reserved/inactive under section 1; UI Back. |
| Fire / cast | Right trigger, `FrameController_Right_Trigger_Value` | Fire/CeaseFire edges; proposed press 0.55/release below 0.45, tune on hardware. No duplicate Trigger_Click binding. |
| Next / previous artifact | `FrameController_Right_Bumper_Click` / `FrameController_Left_Bumper_Click` | Existing NextItem/PrevItem across groups. |
| Quick-swap | Right X, `FrameController_Right_X_Click` | Last distinct still-owned artifact via normal selection; no valid history means no action. |
| Radial | Left squeeze, `FrameController_Left_Squeeze_Value` | Hold opens; left stick selects group/slot; release confirms valid highlight; centre/cancel leaves unchanged. |
| Use / confirm | Left trigger, `FrameController_Left_Trigger_Value` | UI select/activate text entry; artifact use remains Fire. |
| Scoreboard | Left View, `FrameController_Left_View_Click` | Hold; restore previous visibility on release, respecting death/match-end rules. |
| Menu | Right Menu, `FrameController_Right_Menu_Click` | Existing menu through world-space host; chat available there. |
| Recentre | Menu or hold `FrameController_Right_Y_Click` for 1 s | Section 14 release contract; short press inactive in gameplay. Available stationary/in UI, never a runtime System key. |
| Previous/next group | `FrameController_Left_DPadLeft_Click`, `FrameController_Left_DPadRight_Click` | Existing groups; UI/team context overrides. |
| UI navigation / Back | Left stick/D-pad; `FrameController_Right_B_Click` | Navigate offscreen host/keyboard with the same Slate user as pointer/confirm. |

System buttons belong to the runtime. Touch sensors animate fingers only; grip/aim poses
supply hand/UI tracking. Walking, drop, item information, team choice, chat and other
infrequent commands remain available through menu/selection UI and existing command paths.
The VR/flat switch (decision F) is a menu command with a bindable keyboard shortcut.

### Contexts and ownership

Frame contexts prioritize UI, then selection, then gameplay. Radial consumes movement
stick and suppresses casting without bypassing switch restrictions/cadence or pausing
the match. Modal/source transitions use section 14's release contract.

Retain gamepad contexts and legacy keyboard/mouse mappings; a full Enhanced Input
migration is unnecessary. Flat input stays unchanged. In VR, selected keyboard/gamepad
may supply buttons/movement for accessibility/diagnostics; HMD owns pose/aim and VR
policy owns artificial yaw. Mouse pitch/gamepad smooth 180-degree turn cannot bypass it.
Select one movement source to prevent summed axes/duplicate emulation; UI still accepts
keyboard navigation. Preserve the legacy per-frame swim-up path exactly once.

## 8. Aim and casting

Head-derived yaw/pitch after normal constraints supplies simulation aim. Trigger invokes
normal controller Fire, including respawn/restart/empty-slot handling, not a direct RPC.
H2 replaces local aim reads at every cast entry, including special selected-item branches.
Release/hold semantics, charges, delays, prediction and authoritative rejection stay intact.
The existing rotation-only `ServerUseAngreal(FRotator AimRotation)` leaves origin,
offsets, collision adjustment, targeting and trajectories server-controlled. Hand poses
never supply hit locations, shields, interaction reach or a second firing weapon.

**Tracked aim ownership suppresses flat look assistants**, including automatic centering,
look-spring, held centre-view and stair look, even when the head pose is stationary.
Preferences reload and these helpers write pitch (`Source/WoT/WoTCharacter.cpp:2598`);
absolute tracked rotation need not invoke explicit-look suppression in `AddPitchInput`
(`:5505`). Gate the helpers through H3 while tracking owns aim, preserve saved flat
preferences/restoration, and continue applying gameplay-imposed constraints.

**Held aim:** continuously update game-thread control/aim for repeating, continuous,
leading and seeking use; launcher ticks can fire after the initial RPC
(`Source/WoT/Inventory/WoTProjectileLauncher.cpp:77`). Existing movement/view transport
must deliver current aim even while stationary. No reliable per-render-frame pose RPC;
any transport deficiency needs separately reviewed generic flat/VR correction. The
combined two-client held-aim/look-assist test is in section 15.

Draw a binocularly consistent, depth-tested world marker from the unchanged simulation
ray, projected per eye; never reveal a target behind an occluder or bend shots toward it.
It is a direction/obstruction aid, not a straight-trajectory promise for curved/seeking/
destination artifacts. With zero model offset (decision G deferred), the neutral
centre-eye ray coincides with the simulation ray; posture changes and the room-motion
follow gap add small parallax, which the marker shows honestly.

| Later controller aim | Shared-server consequence |
|---|---|
| Controller orientation, existing eye origin | Fits rotation provider/RPC; needs aim feedback and movement/body-facing review. |
| Controller ray target, eye aims toward it | Also trace from legal eye, reject hand-only visibility, define far/no-hit target; convergence must not grant aim assistance. |

## 9. Locomotion and comfort

Use existing legal movement. Head-relative ground forward projects onto the ground;
near vertical, retain the last valid yaw. Off-hand-relative ground input uses projected
controller heading, with short tracking-loss grace then input release. Neither changes
aim. Room motion (section 6) is a second source, summed with the stick before the legal
clamp. Swimming keeps legal view pitch and held Jump rise; no new downward action.
Existing teleporters, movement effects and constraints retain their rules.

Snap rotates the section 6 heading about the current head, compensating rig translation
so displaced eyes do not orbit; the body keeps following the head (section 6). Proposed
thresholds: 0.7 trigger, below 0.3 re-arm; one deflection gives 30 degrees. Smooth yaw
integrates frame time with adjustable rate/dead zone. Neither injects pitch/roll; retain
body-facing replication.

Vignette Off/On plus strength, proposed default Off with first-run comfort choice.
Close peripheral vision during artificial movement/smooth turning, relax at rest;
physical walking and head motion do not trigger it. No speed/acceleration/response changes.
Both-eye material correctness, essential warnings and gameplay visibility effects must
survive the mask.

## 10. HUD, menus and onboarding

### Treatments and rendering

| Treatment | Tradeoff and shared feed |
|---|---|
| Soft-attached panel | Recommended compact health/charges/artifact/status below forward view; stable within yaw dead zone, gentle follow. No arm raise, but can obscure scene. Rearrange existing commands/icons/fonts/runtime text. |
| Off-hand wrist | Clears scene; requires arm raise/tracking and readable angular text. Same snapshot; expand scores/messages into a reading panel. |
| Held artifact card | Styled icon/charges/description beside off-hand; bulky text can obscure targets and reading depends on hand stability. Reuse item-info/onboarding; keep health/warnings independent. |

Prototype soft panel: 1.2 m (63 units), 35 degrees wide, below aim; adjustable scale,
distance/recentre, yaw dead zone, no head-roll attachment. Menus/scores open world-locked
at about 1.5 m (78.75 units) until dismissed/repositioned. Users may look away; urgent
warnings survive lost decorative hand tracking. Dimensions require headset validation.

H5 separates update/snapshot/layout/draw: advance permitted presentation state **once per
local-player frame**, preserving visibility and paused-message rules, then derive flat,
compact and reading layouts. Both eyes share one HUD texture, and a mode switch changes
only the layout. Never call live `DrawHUD()` per eye or share mutable `AHUD::Canvas`.
Destination-specific clip/scale and existing font/texture resolution consume the command
representation (`Source/WoT/WoTHUDDrawCmd.h:29`). Complete extraction of direct messages,
progress, team prompts, player ID, warnings and flash textures; expose no hidden/debug data.
Reuse scoreboard snapshot/sort/layout (`Source/WoT/WoTHUD.cpp:1742`,
`Source/WoT/WoTScoreboard.h:74`, `:136`); all 16 rows/columns must be readable, with
scrolling/paging if needed. Unchanged textures may throttle refresh; surfaces track each
frame. Headless layout checks cannot prove sampling, transparency or stereo readability.

### Window hosting and controller text entry

H6 hosts existing Slate content offscreen in VR, viewport content in flat mode, retaining
open/close/focus and one-active-window callbacks (`Source/WoT/WoTOnboardingSeams.h:29`,
`:115`). Include front end, browser, class/team choice, settings/menu, item help,
onboarding guide/cards/recaps, **chat and text-entry layers**, independent of pawn/HUD.
Chat currently has separate viewport hosting (`Source/WoT/WoTPlayerController.cpp:3369`).
Passive cards do not steal combat input; modal UI follows section 14. Network play
continues; long text wraps/scrolls at a readable angular size.

Register the offscreen window/surface in the controller UI input scope: the existing
router rejects out-of-scope focus (`Source/WoT/UI/WoTControllerUIInput.cpp:508`). Use
**one consistent Slate user** for pointer, focus, Frame confirm/back/navigation and text
entry. Translate Frame actions into the existing UI/entry routes, with keyboard/gamepad
navigation retained. Pointer events have no world-use/collision authority.

A Windows laser mouse click normally places the caret without starting entry
(`Source/WoT/UI/SWoTTextFieldActivator.cpp:110`); field activation must explicitly enter
the controller text-entry flow. Host its existing keyboard panel
(`Source/WoT/UI/WoTTextEntry.cpp:186`) whenever runtime entry is unavailable, with accessible
Done/Cancel, preserved field limits/password/filter semantics and focus restoration.
Runtime keyboard support over the stream is unverified; controller-only address/password
entry and chat must work using that panel, without a new custom keyboard.

## 11. Hands and the PR #82 boundary

PR #82 owns art, class styling, fingers, item visuals and presenter; VR owns tracking,
anchors, handedness and validity. One inventory/casting command; off-hand is decorative/UI.
Its existing slots/factories are not VR support (PR #82
`Plugins/WoTFirstPerson/Source/WoTFirstPerson/Private/WoTFirstPersonAnchor.h:7`,
`Source/WoT/WoTExtensionSeams.h:123`, `:127`, `:133`, `:139`). The presenter uses flat
camera anchors and first-person/capture-exclusion flags (PR #82
`Plugins/WoTFirstPerson/Source/WoTFirstPerson/Private/WoTFirstPersonPresenter.cpp:184`,
`:250`, `:320`).

H7 publishes local player, hand slot, parent component, grip/socket offset, validity and
flat/tracked mode. `WoTVR` publishes; `WoTFirstPerson` consumes without direct dependency.
Hands attach below controller grip components, artifact below hand socket, including
late update; no tick-copied free actor. Tracked mode removes flat FOV/scale warping,
camera placement, clearance pull and positional bob/sway; use 52.5 units/m and cosmetic
fingers/cast cues. Clipping fades/hides visuals, never changes reach/collision. Section 6
owns stereo near clipping independently of this presenter.

Coordinate two-hand assets/left grips with PR #82's owner; mirrored coverage is unproven.
Hide invalid hand/item poses, recover without animation jumps/stale muzzle, and never
fall back to flat anchors in VR. Absent/disabled hands leave VR view/input/HUD working,
with minimal UI pointer. Anchor disposal follows section 14. Integration remains later work.

## 12. Desktop window and reflective surfaces

The desktop window keeps Unreal's default VR spectator output (decision J). The game adds
no capture, composite or streaming path: Steam streams the game to the Frame by itself.
Switching to flat (decision F) restores the normal flat window and HUD.

**World reflections require per-view correctness.** Existing mirror planes have one
viewer/capture/material state (`Source/WoT/World/WoTRuntimeMirror.cpp:299`), and shared
projection coordinates (`Tools/import/mirror_material.py:55`). That cannot supply correct
reflected binocular disparity. Provide separate reflection view/projection/texture state
for each stereo eye, without one eye overwriting the other. If infeasible within budget,
release requires an explicitly specified, owner-accepted fallback with its stereo and
visibility compromises recorded; none is accepted here. A shared single-view reflection
is not silently adequate.

## 13. Performance budget and measurement

Budget against the **active session refresh**, not monitor refresh or assumed headset
modes. Stage-1 tests recorded no rate/timing; no live session was available for this design.
Record runtime period first; propose 90 Hz if offered by the tested streaming session.

| Active rate, if available | Display interval | Application target, 20% headroom |
|---|---|---|
| 72 Hz | 13.89 ms | 11.11 ms |
| 90 Hz | 11.11 ms | 8.89 ms |
| 120 Hz | 8.33 ms | 6.67 ms |
| 144 Hz | 6.94 ms | 5.56 ms |

These are conditional arithmetic budgets. Both GPU and limiting CPU thread should meet
the selected target; overlapping times are not added. Display interval is not streaming
latency. Use packaged PC CPU/GPU traces/Unreal Insights and runtime compositor timing;
record encode/transport/decode where exposed, drops/reprojection, hardware/runtime,
refresh, render size, resolution/codec and network conditions.
Frame-specific stream telemetry availability remains unverified.

Compare identical flat, stereo-only, plus HUD and plus hands scenes. Measure sustained
median/p95/p99, worst hitches and missed frames: dense fights, water/fog, translucency,
reflective surfaces, artifact and mode switching, large scores, menu opening and travel.
Separate load hitches/transition presentation from steady-state percentiles. Watch custom
stereo rendering, per-view reflection captures, overdraw, shadows/draw calls, hand lighting,
font/texture loads, render targets and late update. Do not assume stream foveation or
on-device Valve features reduce PC cost. Optimize only after both-eye correctness checks;
release must sustain chosen refresh without routine reprojection.

## 14. Networking and lifecycle

Existing movement acceleration/control rotation, aim-bearing cast RPC, releases and
selection remain the wire contract. Server owns collision, legal movement, origins/
trajectories, inventory, charges, damage and match state. No trusted HMD positions,
hand origins, hits, heights or HUD: room motion reaches the server only as ordinary
movement input within stick limits. Same-release flat/VR clients share ordinary
replicated bodies/view-facing; no server VR plugin, VR pawn population or remote pose
replication. A client's mode switch is invisible to the server. Arbitrary cross-commit
network compatibility is not promised.

### Single lifecycle/release contract

H1 owns rig/UI by local player, independent of controller/pawn possession. World-bound
anchors/surfaces are disposable; local preferences and UI ownership survive rebinding.

- On modal/radial entry, source/context change, focus/controller/tracking/session loss,
  calibration/recentre, mode switch, death, unpossession, travel or disable: release each
  held command once, clear movement (including room motion)/queued selections/snap turns,
  and block opening-input click-through. This extends gamepad release discipline
  (`Source/WoT/Gamepad/WoTGamepadLayer.cpp:203`).
- Resume only after relevant poses/calibration are valid, sticks neutral and buttons
  released, requiring fresh presses. Never silently recentre during a held cast. On
  session loss in VR mode the character holds still until recovery or a switch; it is
  never handed to mouse aim silently. Hide invalid hands; recovery/leave controls remain
  available, with a flat fallback if XR cannot render.
- Keep head tracking responsive in constrained gameplay; menu/loss grants no pause or
  invulnerability. Death selects stable death presentation; deliberate Fire/Jump uses
  existing respawn gates. Match-end/special-view commands retain their normal gates.
- Possession replaces only the gameplay anchor. Launch, return to front end and pawnless
  travel use the neutral anchor. Teleports and respawn re-anchor the rig so the head is
  over the capsule. Dispose old world objects/listeners, rebind new ones without
  accumulating height offsets, and preserve posture preferences. Reference-space resets
  invalidate pose calibration and require calibration before gameplay resumes.
- **Mode switches happen in place** (decision F): in a match, in menus or while dead, with
  no rejoin, travel or front-end trip. To flat: stop stereo rendering, unregister VR aim,
  movement, view, HUD and UI policies so the flat ones resume, and continue from the
  current aim without a jump. To VR: require a usable session, take the current aim yaw as
  the heading, re-anchor the head over the capsule and use saved posture calibration,
  offering recalibration without blocking play. An explicit user switch is the baseline;
  following the headset being worn or removed in Auto uses the same path once its session
  signals are verified (section 16).
- Disable/shutdown unregisters contexts/delegates/providers, disposes rig, surfaces and
  anchors, and closes XR; only the settings control needed to re-enable remains.

## 15. Testing and acceptance

Later implementation tests only; this design revision builds/runs no game or tests.
Synthetic pose/input automation under `WoT.VR.*` and tests of every inert hook need no XR for
pure policy/math. Source/synthetic checks cannot prove actual Frame profile delivery,
tracking comfort, visible pixels or performance.

### Without a headset

- **Optionality/mode:** disabled/absent plugin leaves flat input/view/HUD/timing unchanged;
  dedicated servers create no XR. Check guarded packages, preference/flag precedence,
  Auto/explicit failure, no-HMD launch and listen host with flat remote clients.
  Repeated in-place VR/flat switches in a live match (synthetic session): no rejoin or
  simulation change, exactly one release per switch, no leaked registrations, and the
  flat policies fully restored each time.
- **Pose:** convert 0.10 m to 5.25 units once; posture preserves capsule size and
  simulation eye height. Snap around displaced head preserves position and adds yaw once.
- **Room motion:** synthetic walks move the capsule only through movement input; walls
  and obstacles stop it; stick plus walking never exceeds stick speed; no drift or
  oscillation at rest; the rig absorbs only walking-caused displacement; the view fades
  past the follow gap or on contact and never renders beyond a wall; teleport, respawn
  and recentre re-anchor the head over the capsule.
- **Aim:** identical legal rotations/inputs preserve cast origins/offsets, cadence, charges
  and rejection; cover finite rotation validation, constraints/parallax and UI isolation.
  **Two-client held-aim test:** stationary and moving, turn then hold head still with both
  centering and stair-look preferences enabled. Check continuing/repeating/leading/seeking
  aim stays current through existing transport, helpers never overwrite tracked pitch,
  gameplay constraints still apply, and saved flat preferences work again after VR exits.
- **Selection/input:** hysteresis, snap re-arm, no duplicate movement/swim-up; group/slot
  order, empty/depleted/removed items, quick-swap history and server reconciliation.
  Failure grants no ownership/timing change; radial does not pause the server.
- **HUD/UI:** one update across both eyes and mode switches, visibility/message-pause rules, scoreboard
  ordering, long text, window exclusivity, onboarding disabled and same permitted data.
  Exercise offscreen scope, one-user focus, Frame navigation/confirm/back, chat/entry
  layers and keyboard Done/Cancel/focus restoration without a runtime keyboard.
- **Presentation:** valid/invalid hand anchors, one/two hands and plugin combinations;
  near-plane policy independent of hand state; per-view reflection state isolation.
- **Network:** listen/dedicated server lacking `WoTVR`, flat client plus VR fixture:
  movement, targeting/damage, switching, water, constraints and match-end rules. Room
  motion reaches the server as ordinary movement input, and a client's mid-match mode
  switch is invisible to it.

### Lifecycle acceptance checklist

Exercise the section 14 matrix with synthetic events, then actual headset/controller
loss and runtime overlays; this is the single transition acceptance checklist.

- Complete launch/calibration/join, death/respawn, match end, travel, leave/return to front
  end and recovery, including intervals without pawn/HUD and controller replacement, and
  switch VR/flat in each of these states. Neutral view, pointing and menus remain usable
  wherever XR is usable.
- Interrupt held inputs across every listed transition: exactly one release, no stuck or
  reconnect/close-triggered cast, click-through, queued selection or residual movement.
  Resume requires neutral controls/fresh press; test held-trigger physical reconnect.
- Repeat possession, travel, recentre and reference-space reset: no duplicate listeners,
  stale poses/anchors, accumulated height offsets or lost preferences. Disable/shutdown
  restores flat policy and removes all optional resources/registrations.

### Headset checklist

- Verify active PC runtime/profile/extensions, every binding/edge, handedness and UI
  navigation; special views/setup/editor remain operable through stable panels.
- Confirm 52.5/Local and neutral height for seated/standing users of different heights;
  explain floor mismatch. Walk around the play space: the body follows, walls, door edges
  and obstacles stop it, the view fades in both eyes instead of entering geometry,
  nothing drifts at rest, and physical lowering changes only the view. Test close
  geometry/near clipping with hands visible, hidden, untracked and plugin disabled/absent.
- Switch VR/flat repeatedly in a match, in menus and while dead: no rejoin, aim jump or
  stuck input, and the headset and desktop window each show the right output.
- Cast near/far, above/below and near cover against flat-client results; inspect parallax,
  launch offsets/obstructions. Hands never supply a muzzle or enable cover bypass.
- Walk/strafe/jump/land/swim, take knockback and enter constrained states; check movement
  bases, snap/smooth rate, no bob/roll/doubled yaw and vignette with fog/flash.
- Read health/status/charges/messages, all scores/long names, guides/cards/browser/menus
  in both eyes: locking, distance, sampling, depth/occlusion. Using only Frame controllers,
  enter server address/password and send/cancel chat through the fallback keyboard;
  test navigation, focus, Done/Back and no gameplay click-through.
- Check hand/artifact scale, late update, left grips and class changes.
- In reflective scenes inspect disparity, projection, occlusion and state isolation in
  both eyes; any fallback requires recorded owner acceptance. The desktop window shows
  Unreal's default spectator output.
- Measure sustained heavy-scene frame times and stream latency, including reflective
  scenes, at the selected refresh. Record any approved reflection compromise.

Acceptance requires recorded owner decisions, shared-rule tests, complete controller/
lifecycle/headset checks and measured refresh results. Unit tests alone cannot close
stage 2; rendering fallbacks remain explicit release gates.

## 16. Unresolved verification and dependencies

| Question | Evidence required |
|---|---|
| Frame profile/extension on the PC stream; compatible distribution? | Reproducible extension build/package and real session/profile log. Stock OpenXR does not establish those keys. |
| Refresh/stream telemetry and rendering budget? | Active period, runtime/settings trace; conditional budget verified with reflections enabled. |
| Room-motion follow gap, deadband, gain, fade and stereo near plane? | Headset tuning, close-geometry/depth checks and corner/cover fairness approval. |
| Does the parity movement model settle small walking inputs without overshoot? | Synthetic walks on every movement state, then the headset; tune the follow controller (deadband, position and velocity gains), never the movement rules. |
| In-place mode switching on the streamed session? | Stereo off and on at runtime while the session stays usable, how fast the headset resumes, and the session signals for the headset being worn or removed (Auto). |
| Lowered per-model view (decision G)? | Deferred by the owner; revisit after headset play. v1 uses zero offset. |
| PR #82 anchor API and two-hand assets? | Coordinate its final public surface and coverage; snapshot is evidence, not code to copy. |
| Special view/FOV effects and offscreen UI work in this XR build? | RHI/stereo/headset results preserving gameplay visibility; controller-only text entry through existing panel. |
| Runtime keyboard available over streaming? | Actual session verification; existing keyboard panel remains required fallback. |
| Per-view reflective surfaces fit correctness/performance requirements? | Both eyes pass section 15, or explicitly documented owner acceptance of a specified fallback. |

## 17. Out of scope

Beyond section 1's simulation boundary: on-headset/Android/native packaging, a new
renderer/gameplay implementation, competitive teleport, hand-origin firing or gameplay
gestures, independent dual-hand use, body IK, eye-tracked aiming, custom streaming,
desktop output or keyboard, wholesale keyboard/mouse input migration, direct-manipulation
setup editor, replacing PR #82's hand art, and per-model lowered views (decision G,
deferred). This document changes no configuration, code, assets, packaging or runtime
behavior.

## Review record

Revision 1's independent review found five P2 gaps and requested consolidation.
Revision 2 resolves the design contracts below; implementation and acceptance remain pending.

| Finding | Resolution |
|---|---|
| Flat look helpers can overwrite tracked pitch | H3/section 8 suppress assistants for all tracked poses, preserve flat preferences; combined held-aim test in section 15. |
| Visible Slate panels do not complete controller text entry/chat | H6/section 10 specify offscreen scope, one user, Frame actions and chat/entry layers with existing keyboard fallback; controller-only acceptance in section 15. |
| Rig has no pre-possession anchor | H1/section 6 define neutral anchor and independent rig/UI; section 14 lifecycle and section 15 transition checklist cover pawnless operation. |
| Near clipping depends on hand presentation | H4/section 6 give VR independent stereo near-plane ownership; section 15 covers every hand/plugin state. |
| World reflections share one viewer state | Section 12 requires per-view reflections or explicit accepted fallback; sections 13/15 include reflective correctness/performance scenes. |
| Decision scope and repetition | Crouch/use moved to section 1; G excludes origin changes; former K becomes output-quality decision J. Lifecycle, held aim, heading and HUD timing are consolidated; rejected approaches and exclusions shortened. |

Revision 3 records the owner's rulings of 2026-11-17.

| Ruling | Resolution |
|---|---|
| A-E and I as recommended | Recorded as approved in section 4. |
| F: eventually, switch modes seamlessly at any time | Startup preference and flags kept; in-place switching is the target (sections 4, 5.3, 14), every registration attaches and detaches at runtime, and section 15 tests repeated switches. |
| G: hold off | Deferred: v1 uses zero model offset (section 6); the aim marker stays part of head aim (section 8). |
| H: walking must drag the capsule, and walls must stop it | Section 6: walking becomes ordinary movement input toward the head, with collision and stick limits; the view fades instead of passing through walls; sections 9, 14, 15 and 16 updated. |
| J: Unreal's default; Steam handles streaming | Section 12: Unreal's default spectator output; no capture, composite or streaming path. |
