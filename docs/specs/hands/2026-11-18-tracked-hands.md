# Tracked hands (design, stage 1)

The first-person hand is drawn from authored grip poses. In VR that is the wrong way round:
the headset runtime already reports where every joint of the player's hand is, and a hand that
ignores the player's own fingers looks wrong. This design drives the hand from that data first
and derives the authored poses from it afterwards.

## Stages

1. **Live hand (this document).** The hand skeleton follows the runtime's hand joints. A
   developer can tune how the joints map onto each hand model and save the result.
2. **Captured grips.** With an item in the hand, the developer shapes a grip with their own
   hand and saves it. Saved grips become the fallback poses for flat play and for controllers
   that report no finger data, replacing poses written by hand.
3. **Contact.** Fingers stop at the surface of the held item instead of passing through it.
4. **VR item handling.** Choosing, grabbing and holding items by hand. This is new behaviour
   and gets its own design before any code.

Stages 2 to 4 are outlined only so that stage 1 does not block them.

## Stage 1

### Source

The engine exposes hand joints through its hand-tracker interface: 26 joints per hand (palm,
wrist, four thumb joints, five joints for each finger), each with a world position and
rotation. Any runtime that implements the interface works, including controllers that
estimate finger curl from touch and squeeze sensors. The first-person plugin reads the
interface directly and has no dependency on the VR plugin; without a running headset session
there is no data and nothing changes.

A second, synthetic source produces the same 26 joints from a curl value, with the wrist held a
little ahead of the view. It exists for automation specs and for checking the path on a machine
without a headset.

### Mapping

The hand models already follow the same joint set: a bone's local X runs to its child and its
local Z points out of the nail side, which is also how the engine reports a tracked joint. So
a bone's local rotation is the rotation between its parent joint and its own joint:

    local(bone) = inverse(joint(parent) * fix(parent)) * (joint(bone) * fix(bone))

`fix` is a constant rotation per joint, identity by default. It absorbs differences between a
runtime's joint frames and a model's bone frames, and is what the developer tunes. Bone
lengths stay the model's own; only rotations are taken from tracking.

The hand is placed so its wrist bone sits on the tracked wrist joint. Its size comes from the
player's hand: the tracked length from wrist to middle fingertip, measured along the joints,
over the same length on the model.

The forearm follows the wrist rigidly in this stage. Elbow estimation is later work.

Known limits of this stage: the engine refreshes its tracking-to-world transform once at the
start of a frame, before the player moves, so the hand trails a moving or turning player by
about one frame; and the hand does not get the head view's render-thread late update. Both are
acceptable for tuning the mapping and are removed when the VR plugin supplies the hand's anchor.
Without a registered hand tracker (flat play, shipping builds, platforms without a headset
runtime) the path does nothing.

### Rendering

While tracked, the hand is placed in the world and none of the flat presentation applies: no
camera-relative framing, no field-of-view correction, no clearance pull, no bob or sway. The hand
and the held item render as ordinary world geometry. When tracking stops, the flat presentation
resumes on the next frame, with the flat bone pose, placement and rendering restored.

As in flat play, the hand shows only while an item is selected. An empty tracked hand belongs to
item handling (stage 4).

Only the right hand is tracked in this stage. The left hand is a mirror of the same data and
follows once the right one is tuned.

### Developer commands (not in shipping builds)

| Command | Effect |
|---|---|
| `wot.fp.track 0\|1\|sim [curl]` | Off, live joints, or the synthetic source. Off by default; the `-WoTHandTracking` launch option starts it live. |
| `wot.fp.trackfix <bone> <pitch> <yaw> <roll>` | Set the `fix` of the joint a bone stands for (`index_proximal_r`). |
| `wot.fp.trackfix all` | Reset every `fix`. |
| `wot.fp.trackfix save` / `load` | Write or read the fixes (`WoTFirstPerson/hand_tracking_fixes.json` in the project's saved folder). Saved fixes load at startup. |
| `wot.fp.trackdump` | Log the hand trackers, every tracked joint in view space and the bone rotation taken from it. |
| `wot.fp.posecapture <name>` | Save the current pose under a name in `WoTFirstPerson/captured_poses.json`; capturing a name again replaces it. |

A captured pose records each bone's rotation relative to the model's rest pose, as the same
`[flex, spread, twist]` degrees the authored grip poses use, plus the wrist's transform relative
to the view and the hand's scale: what a flat fallback pose needs.

## Out of scope for stage 1

Contact handling, item handling by hand, the left hand, elbow estimation, a tuning interface
inside the headset, and any change to simulation. Aim, casting and item selection are untouched.

## Open questions

- Which motion range a given runtime reports for controller-estimated fingers (hand wrapped
  round the controller, or closed as if empty). It decides whether a captured fist is a fist.
- Whether the tracked wrist of each runtime matches the wrist of the hand models closely
  enough that one `fix` set serves every class hand.
