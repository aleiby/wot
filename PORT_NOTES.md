# Port notes

WoT Reborn is a multiplayer port of the original 1999 game to Unreal Engine 5. The port matches the original's simulation: movement, timing, damage and cadence. Presentation may modernize, as long as it does not change how the game plays. This file lists every current, intentional difference from the original, grouped by area, and each entry says what the original does, what the port does, and why. Contributors add or update an entry in the same pull request as the change (see CONTRIBUTING.md). Fixes that bring the port closer to the original remove or shrink entries here.

## Gameplay

### Zones are box volumes
- **Original:** Zones come from the compiled BSP. A ZoneInfo actor tags a region bounded by portal surfaces, and every actor inside that region uses its properties.
- **Port:** Each effect-bearing zone is an `AWoTZoneVolume` box sized to the zone's extracted bounds, with the same water, pain, heal, gravity, current and friction properties. Membership uses those axis-aligned bounds, not exact BSP containment.
- **Why:** UE5 has no BSP zone system, and the shipped map data gives zone bounds but not a per-zone solid.

### Swimming is driven by the zone, without buoyancy
- **Original:** A water zone switches the pawn to swimming through the engine's native zone physics.
- **Port:** The custom movement component (`UWoTCharacterMovement`) enters swimming in a water zone, leaves it on exit, and keeps the mode stable for other clients. Buoyancy stays at zero because there is no real engine water volume.
- **Why:** The zones are not UE5 physics volumes, so the engine's own swim logic would drop the pawn out of swimming every frame.

### Zone forces are predicted on the client
- **Original:** The server applies zone current and gravity directly to pawn velocity.
- **Port:** Current, gravity, ground friction and swim mode are computed inside the movement component, the same way on the server and on the controlling client. As a side effect, an idle pawn that nobody controls no longer drifts in a current.
- **Why:** With UE5 client prediction, server-only forces make the player rubber-band.

### Water currents move swimmers after the swim step
- **Original:** The zone current is folded into the swimming move before velocity is worked out from the distance moved.
- **Port:** The current carries the swimmer once, after the swim move, separate from the pawn's own velocity. A zone crossing caused by the current is noticed on the next movement update.
- **Why:** It keeps the current consistent with client-side movement prediction and replay.

### Swimming step-up checks for room first
- **Original:** When the swim step check passes, the swimmer always steps up onto the ledge.
- **Port:** The step-up also has to pass the engine's own step-up test. If it fails, the swimmer slides along the obstacle instead, as ladder movement already does.
- **Why:** UE5's movement component only allows a step-up the capsule can actually complete.

### No repeated exit check while bobbing at the surface
- **Original:** In the swimming state, the player re-checks for leaving the water on every move while bobbing at the waterline.
- **Port:** Only the zone-change exit check runs.
- **Why:** Each bob crosses the waterline again, so the zone-change check already covers the same case.

### Underwater head check and breath timers
- **Original:** One shared pain timer drives drowning and pain zones. The underwater head check uses the current eye height, which eye bob, camera smoothing and the swim-entry animation can briefly change.
- **Port:** Separate per-zone timers reproduce the shared timer's visible quirks. The head check uses a fixed eye height: players use their steady swimming eye height (half the standing height), NPCs their authored height.
- **Why:** A cleaner timer model, and physics and prediction replay must not depend on animation timing or eye bob.

### Leaving a zone is counted on disconnect
- **Original:** A disconnecting player never left their zone, so the zone's player count could leak.
- **Port:** Disconnects and pawn replacement run the zone-leave step exactly once. Counts never go below zero, and the last player out fires the zone's untrigger event.
- **Why:** Fixes an original bug instead of reproducing it.

### Destructive zones
- **Original:** A zone flagged as destructive instantly destroys debris fragments and decorations that enter it.
- **Port:** Fragments and decorations ignore the flag.
- **Why:** No multiplayer map uses a destructive zone.

### Weaves spawned underwater
- **Original:** A weave spawned inside water fizzles during spawning.
- **Port:** It fizzles at the end of the frame it spawned in. This differs only if its first move touches a pawn in that same frame.
- **Why:** Engine difference in spawn timing.

### Ladder and slippery-surface physics
- **Original:** Climbing and low-friction sliding are handled inside the engine's native movement code.
- **Port:** Includes a falling-too-fast gate, a falling player can grab a ladder, 0.8 damping while climbing upward, and a slide clamp. The shipped game may not have all of these refinements.
- **Why:** The exact shipped behavior is not confirmed. The constants will be rechecked if climbing feels different.

### Player starts buried in solid geometry are never chosen
- **Original:** Start selection could pick a start whose center lay inside solid world geometry (Battle3_01 has two). Placing a player there always fails, so a joining player was refused and a restart failed.
- **Port:** Starts whose center is in solid space are never rated.
- **Why:** Stops players being refused on two buried starts.

### A refused start falls back to the next rated start
- **Original:** A join, a respawn or a team change uses only the best-rated start. If the player cannot be placed there, a joining player is refused, a dead player stays dead until they press fire again, and a player changing team stays where they stood.
- **Port:** The best-rated start is still tried first. If it refuses the player, the other starts are tried in rating order, so a player is refused, or left waiting to respawn, only when every start refuses. A start also refuses a player who would overlap another blocking actor, such as a player or a decoration; the original refuses only for movers and places the player overlapping anything else.
- **Why:** A blocked start must not leave a player without a body, and the original's first choice is kept.

### Player-start scoring
- **Original:** The occupied-start penalty uses the game object's own collision size. If every start is occupied, no start is returned.
- **Port:** The penalty uses the occupying pawn's real collision size. If every start is occupied, the engine's default picker chooses one.
- **Why:** Same intent per pawn, and safer in the degenerate case.

### Respawn creates a new pawn
- **Original:** The same player pawn object is reused across death and respawn.
- **Port:** Each respawn creates a fresh pawn, and the previous corpse is destroyed just before it.
- **Why:** UE5 splits the player into a persistent controller and a disposable pawn.

### Team-select restart replaces the pawn
- **Original:** Choosing a team restarts the same player pawn by moving it to a team start. If no start takes it, the pawn stays where it stood and the player keeps playing.
- **Port:** A new team pawn spawns at a team start, and the old pawn is removed only once the new one exists. While the starts are tried, the old pawn is hidden and does not collide. If no start takes the new pawn, the old pawn reappears where it stood and the player keeps control of it.
- **Why:** The UE5 split between controller and pawn. This matches the port's normal respawn.

### Carcass pose on an instant respawn
- **Original:** The corpse swap follows the death animation's notify.
- **Port:** A player who respawns at the first allowed moment still leaves a carcass, posed about one second into the death animation.
- **Why:** The respawn replaces the pawn, so the carcass is swapped in early.

### Suicide-instigator credit is kept on the controller
- **Original:** The pawn that knocked a victim into a hazard is stored and credited for the kill.
- **Port:** The instigator's controller is stored, so the credit still lands if the instigator dies and respawns within the credit window.
- **Why:** In UE5 a pawn loses its player state when it is replaced on respawn.

### Rejected skin request
- **Original:** A skin change that fails the mesh-name check is discarded.
- **Port:** It is still stored on the player and checked again against each pawn the player takes over.
- **Why:** UE5 separates the controller from the pawn. The result differs only if the player later switches to the mesh the skin name matches.

### Match-end freeze leaves look free
- **Original:** At match end the player is frozen, including turning.
- **Port:** Movement input is suppressed on the player's own client and turning stays free, so players can look around the scoreboard. Casting stays blocked on the server.
- **Why:** Usability. A modified client could still move after match end, which does not affect results.

### Early restart press at match end
- **Original:** Pressing fire during the post-match freeze restarts the freeze delay.
- **Port:** The early press is simply ignored.
- **Why:** A minor quirk that was left out on purpose.

### Map rotation order and the starting map
- **Original:** Rotation cycles through the installed maps in the order the filesystem lists them. The current map comes from a config value that the host menu saved.
- **Port:** Rotation uses the order Windows lists files on an NTFS drive. The current map is taken from the world that is running. Three- and four-citadel Battle hosts restart their current map.
- **Why:** The port's maps do not live in a directory, and the port's front end replaces the old host menu. The old stale-config behavior for servers started outside the menu is not reproduced.

### Fall-damage gib bypass for fully immune pawns
- **Original:** A pawn whose damage reduction covers all damage types skips the guaranteed fall gib.
- **Port:** There is no hook for that bypass. The gib damage still passes through the reflector chain, which reduces it.
- **Why:** No matching extension point.

### Recursive traces have no 64-hit limit
- **Original:** Native recursive traces could return at most 64 candidates.
- **Port:** One ordered engine query returns every candidate. The first accepted hit in distance order and the world-boundary rule are unchanged.
- **Why:** No shipped behavior depends on that overflow, and an internal storage limit should not decide results.

### Chain Lightning latch range
- **Original:** Chain Lightning measures latch range to the closest point of the target's collision.
- **Port:** Latch range uses the distance to the target's center minus its radius.
- **Why:** A simpler query. The effect on play is negligible.

### Disguise details
- **Original:** Disguise copies the target's default speeds, its field of view and several single-player physics values, and applies a slowdown when walking backwards.
- **Port:** Speeds are copied from the target's current values rather than its defaults (in standalone play only, as in the original). Field of view, buoyancy, jump, mass and rotation rate are not copied, and there is no backward-walk slowdown. Charges start draining only after the shape change succeeds.
- **Why:** All port pawns share one camera field of view, the skipped values only matter in single player, and current and default speeds are almost always the same.

### Tracer ignores the caster's own seals
- **Original:** Tracer's search loop includes seals the caster is carrying, so it can lock onto the caster at zero distance, even though the item's description says held seals are ignored.
- **Port:** Tracer skips the caster's own seals.
- **Why:** Follows the item's described behavior; the self-target is a latent bug in the original.

### Wall of Air collision and knock-down
- **Original:** The wall uses a collision cylinder. Knocked-down weaves fall under UE1 gravity.
- **Port:** The wall uses a 75-unit sphere on a pass-through profile. Knocked-down weaves fall with gravity scaled to the UE1 value.
- **Why:** UE5 collision shapes and its gravity constant differ.

### Weapon projectile collision
- **Original:** Each weapon projectile has its own collision cylinder, for example the halberd's radius 5 and half-height 15. Arrows have zero collision size, so a missed arrow sticks exactly on the wall surface.
- **Port:** Projectiles use spheres. The halberd matches the radius 5. Arrows use a 2-unit sphere, so a stuck arrow rests 2 units off the surface. The other weapon projectiles still use the default 32-unit sphere.
- **Why:** The shared UE5 projectile root is a sphere, and UE5 sweeps cannot handle zero-size collision. Matching each remaining projectile's size is still open work.

### Earth Tremor rocks on clients
- **Original:** Tremor rocks exist only on the server, so other players never see them.
- **Port:** One replicated marker lets every machine draw the rocks. Only the server's rocks deal damage.
- **Why:** Makes visible an effect the original meant to show, without changing damage.

### Pickup collision shape
- **Original:** Pickups use a collision cylinder with a radius and a height of 16.
- **Port:** Pickups use a sphere with the authored radius.
- **Why:** This is the port's shared inventory collision. Each map's authored radius is kept exactly.

### Seal pickup shape
- **Original:** The seal is picked up inside a cylinder of radius 17 and half-height 3.
- **Port:** Uses a 17-unit sphere.
- **Why:** The port uses spheres for touch shapes. The radius matches; the reach upward and downward is larger.

### Dead pawns cannot drop items; unlooted bags take their contents with them
- **Original:** Death emptied the inventory, and a destroyed bag left its contents behind as orphaned actors.
- **Port:** Drop requests from a dead pawn are refused. A bag destroyed before anyone loots it also destroys its hidden contents. Seals are never put in bags.
- **Why:** A dead pawn can stay possessed at match end. Without these rules items could be duplicated or leaked.

### Artifact hand order is fixed
- **Original:** A player could reorder the artifact hands by editing a config file.
- **Port:** The hand layout is fixed data that always uses the shipped default order.
- **Why:** Neither game has a menu for reordering hands, and the shipped default is what players actually used.

### Hand cursor follows the server's selection
- **Original:** The client controls its hand cursor between explicit selection syncs.
- **Port:** The cursor snaps to the server's selected item on every inventory change, so in a rare race it can visibly jump where the original would not.
- **Why:** It keeps client and server from disagreeing about the selection.

### Seal placement occupancy check
- **Original:** When a seal is placed, every visible actor inside the summed collision cylinder counts as an occupant.
- **Port:** The check collects nearby actors from their collision overlaps, so a visible actor with no collision can be missed.
- **Why:** UE5 gathers candidates through physics overlap queries.

### Citadel editing gate is per team
- **Original:** The no-pickups-while-editing rule checks each player's own editing flag.
- **Port:** It checks the team's editing flag. With one editor per team (PlayerEditLimit=1) the result is usually the same, but the whole team is gated while its editor works.
- **Why:** The per-player editor input state is not ported.

### Citadel editor access and zone checks
- **Original:** The client cursor limits what can be edited, and edit zones are exact BSP regions.
- **Port:** The server decides which citadel a player edits from the nearest SealAltar and requires the viewer's current zone to allow editing. Edit zones are box volumes, and the smallest overlapping box wins.
- **Why:** Server-side hardening, and UE5 volumes instead of BSP zones.

### Citadel placement tolerances and occupancy
- **Original:** Occupancy uses a radius test that ignores height, and placement uses exact equality checks.
- **Port:** Occupancy uses UE5 shape overlap queries, so accept and reject edges can shift by about 20 units. Placement uses small numeric tolerances (1 unit, normal dot 0.99). Teleporter and mover exclusions still use the original radius rule.
- **Why:** UE5 collision queries.

### Only citadel-built troops can be removed in the editor
- **Original:** The citadel editor decides whether a creature can be removed (and refunded) by its class alone.
- **Port:** Each team keeps a list of the creatures it built, and only those can be removed and refunded.
- **Why:** In the port, map creatures and summons share one NPC class, and they must not become removable just because their class matches a troop type.

### Gates stay clickable while editing a citadel
- **Original:** When editing starts, active portcullis and staircase gates cannot be clicked until their type has been selected and deselected once.
- **Port:** Active gates can always be clicked while editing, which is how the original behaves after that first cycle.
- **Why:** The deactivate action needs to reach those gates.

### Portcullis archways with an empty event
- **Original:** An archway with an empty event grabs an arbitrary mover, a map authoring bug on two Battle3_02 archways.
- **Port:** The importer falls back to the archway's own tag, which wires it to the intended gate.
- **Why:** Follows clear authoring intent.

### Bad map data warns instead of halting
- **Original:** Out-of-range team indices quietly read defaults. Bad BattleInfo or BudgetInfo setup triggers fatal asserts.
- **Port:** Team-index reads are bounds-checked. Validation problems log a warning and the game continues.
- **Why:** C++ does not tolerate out-of-range array reads, and a half-configured map should not crash the editor or server.

### Multi-key AssertMover closes fully
- **Original:** With three or more keys, the close sequence jumps to a missing label, stops after one key and never re-arms.
- **Port:** The mover closes through every key and re-arms.
- **Why:** An original script defect. Every multiplayer AssertMover has two keys, where both behave the same.

### LoopMover untrigger while stopped
- **Original:** Untriggering a stopped LoopMover re-fires its event with no motion.
- **Port:** Untrigger does nothing unless a leg is in flight.
- **Why:** A script artifact, not authored behavior. No LoopMover is placed on multiplayer maps.

### Mid-leg group stop and immediate close
- **Original:** TimedMover group return or stop, and AssertMover immediate close during an opening leg, keep the exact in-flight position.
- **Port:** These restart or stop at the nearest authored key boundary. Normal sequences keep exact keys and timing.
- **Why:** The native mover state machine tracks key boundaries, not fractional progress.

### Mover group commands in custom movers
- **Original:** Groups of linked movers can be told to stop or return. Some custom mover types react by reversing or freezing.
- **Port:** Loop, Gradual, Mix, Elevator, Assert and Rotating movers ignore group commands while their own state machine runs the movement. A return command on a parked mover does nothing.
- **Why:** Multiplayer maps never trigger these cases, and ignoring them protects state that should only change once.

### Attached slave movers
- **Original:** A slave mover attached to a master runs its own collision checks as it moves.
- **Port:** An attached slave moves with its master and does not check its own collisions.
- **Why:** UE5 attachment works this way. No multiplayer map has a working master/slave pair.

### Exploding walls and movers credit the real instigator
- **Original:** A breakable wall or exploding mover fires its Event before it records the new instigator, so whatever listens to the Event sees the previous instigator, often none.
- **Port:** The Event is sent with the actor that actually caused the explosion.
- **Why:** The old order credits the wrong actor. No shipped multiplayer map gives these actors an Event, so play is unchanged.

### Breakable decorations fire their Event once
- **Original:** A BreakableDecoration that shatters fires its Event while breaking, then fires it again when the actor is destroyed.
- **Port:** The Event fires once, when the decoration breaks.
- **Why:** The second firing duplicates the first. No shipped multiplayer decoration has an Event.

### Stacked decorations fall when their support breaks
- **Original:** Each decoration tracks what it is resting on, and the engine drops it when that support goes away.
- **Port:** When a breakable decoration shatters, it looks for other breakables resting on top of it and starts them falling.
- **Why:** UE5 has no equivalent of that resting-on tracking. The barrel stacks still collapse the same way.

### Decoration features multiplayer never uses
- **Original:** Decorations can be carried and thrown, can be set off by a trigger, and can drop items when broken.
- **Port:** None of these is implemented.
- **Why:** These are single-player furniture features. No multiplayer map, and no BreakableDecoration default, uses them.

### Debris fragment cleanup
- **Original:** A resting debris fragment, for example from an exploding mover, is removed once no player can see it, checked on the server.
- **Port:** Whether the piece was recently drawn decides it. A dedicated server, which draws nothing, removes a resting fragment at the first check (about 1.5 seconds). The fragment's noise event for NPCs is omitted.
- **Why:** UE5 has no per-player can-see test on a dedicated server.

### Gib chunks are server-spawned and stay shootable
- **Original:** Extra gib chunks spawn separately on each machine, and chunk shooting is disabled in multiplayer.
- **Port:** All chunks spawn on the server and replicate, and they stay shootable in multiplayer.
- **Why:** Simpler networking and consistency across machines.

### Flattened carcass collision
- **Original:** A carcass squashes its collision cylinder to a thin disc.
- **Port:** The carcass capsule's half-height cannot go below its radius, so corpses keep a taller minimum collision.
- **Why:** A UE5 engine rule for capsules.

### Which corpse is removed at the limit
- **Original:** When a zone has too many corpses, one that no player can see is removed first; otherwise roughly the oldest.
- **Port:** Removes the oldest corpse.
- **Why:** A dedicated server has no viewer, and the port keeps no hidden corpses. This matches the original's own fallback.

### NPC navigation uses a navigation mesh
- **Original:** NPCs and teleporting creatures route hop by hop between placed path nodes, rank routes by path cost, and check reachability by test-moving the pawn.
- **Port:** NPCs route over a UE5 navigation mesh. Movement acceptance, strike and give-up timing, and goal choice stay the same, but route shapes can differ. To reach an off-mesh goal such as a wall alarm, an NPC walks to the nearest mesh point beside it, and the single agent radius is coarser than each pawn's own reach.
- **Why:** UE5 has no UE1 path-node graph for NPC movement; the visible movement rules are what must match.

### NPCs stop retrying an unreachable post
- **Original:** A returning NPC keeps tracking its post until it classifies as unreachable.
- **Port:** If the post looks reachable but movement keeps failing, after three counted failures the NPC adopts its current spot as the post, as the original does when a post is unreachable.
- **Why:** Modern path following can fail on goals the classification accepts. Without the limit a guard could freeze in a retry loop.

### NPC movement speed fraction
- **Original:** NPC movement speed comes from GroundSpeed values.
- **Port:** NPCs move at 0.8 of their authored GroundSpeed; for example, a Trolloc's 425 pursue speed gives 340.
- **Why:** An existing calibration of the port's NPC movement.

### NPC goal storage and sight cost
- **Original:** Goal objects are shared by up to 16 pawns through per-pawn contexts and silently overflow past that. Long sight traces are thinned by half.
- **Port:** Each pawn has its own goal data, copied on assignment, so there is no cap. Every sight trace runs.
- **Why:** Same behavior without the overflow. The thinning was only a performance optimization.

### Range-band handling
- **Original:** The prefer-visibility shortcut checks only the handler's entry rules. Range handlers carry re-entry and duration limits.
- **Port:** The shortcut also requires real line of sight. Band choice is by proximity only. Flying pawns stop hard on reaching their standoff distance.
- **Why:** Avoids holding position and shooting blind. Flying pawns in UE5 have no braking.

### Defensive weave detection is polled
- **Original:** A caster's defensive perimeter cylinder reacts when an incoming weave touches it.
- **Port:** The same cylinder is scanned on the 0.05 s defensive pulse.
- **Why:** No collision-proxy setup is needed, and the result matches within one pulse.

### NPC dodge
- **Original:** A dodge is a step inside tracking and only pauses the defensive detector. The detector predicts hits with a collision sweep.
- **Port:** Dodging is its own state and pauses all of the NPC's detectors for the short dodge. Incoming shots are predicted from approach direction plus an arrival-time window.
- **Why:** Fits the UE5 controller state machine. The difference lasts under a second.

### Alarm threat handoff works
- **Original:** Captains write the threat into fields that grunts never read, so alarms never pass a threat to grunts.
- **Port:** The threat is carried through, so grunts adopt it.
- **Why:** Fixes an original bug. No multiplayer map places alarms, so multiplayer play is unchanged.

### Captain gong timing
- **Original:** A captain turns to face the alarm before striking. A captain with no gong animation waits out whatever loop it is playing.
- **Port:** The captain turns while it strikes. The Hound captain, which has no gong animation, rings at once.
- **Why:** The folded controller design, and a missing animation slot.

### Creature blocking flags
- **Original:** A creature has three separate flags: blocks actors, blocks players, and can be hit by projectiles.
- **Port:** The three flags become one collision rule. The creature blocks if any flag is set and is ignored only if all three are off.
- **Why:** In the port, pawn contact and projectile hits use the same collision channel. Every shipped creature has the three flags agreeing.

### NPC artifact respawn cleanup
- **Original:** The helper that gives an NPC its artifact back has a long safety lifespan. If the NPC dies first, the artifact is left behind.
- **Port:** The helper is part of the NPC and is removed with it. A held artifact is destroyed when its NPC dies.
- **Why:** UE5 component lifetime handles the cleanup and avoids the leftover.

### Myrddraal blink destination
- **Original:** The Myrddraal walks to a separate teleport proxy actor and reappears at a slightly wandered spot near it.
- **Port:** The chosen shadow node itself is the walk goal and the exact landing point.
- **Why:** The port has no proxy actor; the node's position stands in for it.

### BATrolloc stagger
- **Original:** The BATrolloc stagger interrupts the current attack.
- **Port:** The stagger blocks new attacks, but a swing already in progress finishes.
- **Why:** Follows the shape of the ported NPC state machine.

### Archer weapon switch
- **Original:** The Archer switches between bow and sword by re-picking its most desirable weapon.
- **Port:** The switch is a toggle of prefer-visibility on the range ladder, with the same 5-second and 320-unit thresholds.
- **Why:** The behavior is folded onto the shared range-band ladder.

### Legion stomp and seeker
- **Original:** The Legion cannot move for a short window at the start of the stomp, and its spirit seeker launches on an animation cue.
- **Port:** The Legion stays still for the whole stomp, and the seeker fires as soon as the cast starts.
- **Why:** Fits the shared caster and behavior systems. Pacing, homing and damage are kept.

### Mashadar growth and retraction
- **Original:** New tail segments are seeded by re-linking the chain every frame, and retracting walks the chain in reverse.
- **Port:** A replicated chain phase and counters drive growth and retraction by distance. Growth appends as many joints as needed in one tick until the tail reaches the lair, with 48-unit head spacing and 42-unit body spacing. Authority begins full retraction after the body count exceeds 100; the original's separate 150-segment client safety cap is not implemented.
- **Why:** The choreography only moved segments nobody can see, and this form replicates reliably. Damage and collision behave the same.

### Mashadar chasing a Personal Illusion
- **Original:** The Mashadar tendril has rate limits and shock/rest checks on how often it re-targets what it sees.
- **Port:** Simplified versions of these checks. Setting the same chase target again changes nothing.
- **Why:** Some supporting systems are not ported. Chase behavior is unchanged in practice.

### Item scrolling runs across hands
- **Original:** Scrolling past the last angreal of a hand (the mouse wheel's next or previous item) moves to the adjacent hand's last-used angreal.
- **Port:** Scrolling past a hand's last angreal enters the next hand at its first; scrolling back past its first enters the previous hand at its last. The mouse wheel and the controller shoulder buttons behave the same.
- **Why:** Scrolling reads as one continuous list, so the selection never jumps to an unexpected angreal.

### The joystick option is on by default
- **Original:** The Joystick Enabled option is off by default (`UseJoystick=False`).
- **Port:** It is on by default, and settings saved before controller support are switched on once. It stays as an off switch.
- **Why:** Controllers and the Steam Deck are supported; before this, a controller did nothing.

### Controller sticks
- **Original:** The joystick's movement axes have a per-axis 20% dead zone, and their strength depends on the frame rate: at 60 fps a deflection about a fifth past the dead zone already equals a held key. Its look axes turn far too fast to use.
- **Port:** Sticks use a radial dead zone (a setting, default 20%). Movement reaches full key strength at the original's 60 fps point at any frame rate; below that point the stick moves proportionally slower, where the original accelerated more slowly to the same top speed. Look uses tuned turn rates with a response curve, has its own sensitivity and invert options, and adds to mouse look.
- **Why:** The same feel at every frame rate, and a usable look stick, with no aim assist.

### Controller layout
- **Original:** The default joystick setup binds Fire, Jump and four hat directions only.
- **Port:** Every gameplay action is on the controller, including the citadel editor: B enters and leaves it. The left trigger turns the player around. On a Battle map, a player without a team picks one with the D-pad (left, right, up and down for teams 1 to 4), and the team prompt names those buttons when the joystick is enabled. D-pad up opens chat (see "Chat and taunts by controller and from the game menu"). Every menu works by controller: the front end and its notices, the multiplayer screen, the player and settings screens, the setup screen, and the game menu and its windows. The D-pad or stick moves one highlight (held, it repeats after 0.35 s and then every 0.08 s), A chooses, and B or View backs out one level: an open notice or prompt closes before the screen behind it, a held B backs out only once, and B never quits from the setup screen. While the controller is in use the mouse pointer is hidden and nothing under it highlights; moving the mouse more than a few pixels, clicking or typing brings it back. A gold outline marks the focused control, and a strip at the bottom of each menu shows what the buttons do there (for example "A Select, B Back"); both appear only while the controller is in use. The D-pad's Left and Right adjust a slider. Escape keeps its own behaviour on every screen, except that it closes an open drop-down list or on-screen keyboard first, and that on the settings screens it asks before dropping unsaved changes (see "Settings ask before dropping unsaved changes"). A on a text field or the chat line opens an on-screen keyboard (see "Text fields by controller and touch").
- **Why:** A match must be playable on a controller alone.

### Menu navigation for every player
- **Original:** The keyboard moves through the main menu lists and wraps at either end. Each screen opens with its usual first control focused.
- **Port:** Every menu screen (the main menu, the multiplayer, player and settings screens, notices and the setup screen) has explicit navigation links built from its live rows, so the arrow keys and the D-pad wrap at list ends and reach every row, including rows added at run time. On the multiplayer screen, Left and Right move between the host and browser columns. While a multiplayer notice or prompt is open, the panels behind it are disabled until it closes. After controller input a screen opens on the control a controller needs first (the map list on the multiplayer screen, Save on the player screen, the first setting on the settings screens); after mouse or keyboard input it opens where it always has. Likewise the gold focus outline is drawn only after controller input; keyboard players keep the existing highlight.
- **Why:** Owner ruling (2026-11-08): the menu improvements made for controllers apply to every player; only the first focus and the focus outline follow the last device used.

### Dropdowns are touch-friendly
- **Original:** A drop-down list (the window framework's combo list) opens short rows, 15 units tall, that highlight under the mouse; releasing the button over a row picks it, and a long list scrolls with its scroll bar.
- **Port:** The host screen's map list and the Hardware screen's choices (brightness, resolution, window mode, quality and frame-rate limit) open a list over the screen with rows at least 48 pixels tall. A press only arms the row under it: releasing within 8 pixels picks that row, and moving further drags the list instead, for the mouse and touch alike; the wheel scrolls it. With the keyboard or a controller the open list highlights the current choice, the arrows, D-pad or stick move the highlight (wrapping at the ends) without changing anything, A or Enter picks, and B or Escape closes the list without a change; a second Escape then backs out of the screen as before. A closed list never changes on Up or Down: they move to the next setting.
- **Why:** Touch and controller play on the Steam Deck. Owner ruling (2026-11-08): release-picking and dragging apply to mouse players too, and Escape on an open drop-down list closes only the list.

### Settings ask before dropping unsaved changes
- **Original:** The options and configuration menus apply each change as it is made and keep it when the player backs out; there is nothing to confirm.
- **Port:** The Controls and Hardware screens edit a draft with Save and Back buttons. With unsaved changes, the back action (B, View or Escape) asks "Save changes?": Save applies them and leaves, exactly as the Save button does (a display change still waits for Keep), Discard drops them and leaves, exactly as the Back button does, and B or Escape on the question returns to the screen with nothing lost. The question takes focus, is worked with the D-pad, stick or keyboard, and shows its own buttons in the prompt strip. With nothing changed, the back action leaves at once, as before; clicking Save or Back is unchanged.
- **Why:** Owner ruling (Steam Deck session, 2026-11-09): a B pressed to leave a settings screen should not throw away the changes made on it.

### Text fields by controller and touch
- **Original:** Every text field (the player name, the host's server name and passwords, a server's password, the direct address and the chat line) is typed on a keyboard.
- **Port:** A controller's A on a focused text field opens an on-screen keyboard over the screen: rows of digits, letters and `. - _ :`, with Shift, Space, Backspace and Done keys. The D-pad or stick moves the highlighted key (wrapping) and A types it; X deletes, Y types a space, Menu is Done and B cancels. View does nothing there, and Shift has no button of its own: it is the on-screen Shift key, pressed with A. A held A or X repeats after 0.35 s, then every 0.08 s. The keyboard is drawn large, twice its first size (each key about 83 by 74 pixels on a Steam Deck), with key labels at least 16 pixels tall there; it sits just above a dark band along the bottom of the screen that carries the button strip, so nothing behind shows through the prompts, and it shrinks to fit only on a screen too small for it. What is typed so far stays visible above the keys, wrapping when it is long. Keys can be tapped or clicked, physical typing still works while it is open, and Escape, or a press outside it, cancels it. Done checks the text first: a password the game would refuse keeps the keyboard open and says why. Cancel leaves the field as it was. On a Steam Deck a tap on a field opens it too, and Steam's own on-screen keyboard is used instead by default ("Use Steam Keyboard on Steam Deck" on the Controls page, offered only on a Deck; a player who turns it off keeps the project's keyboard); Y then switches to the project's keyboard, and B cancels. That choice is stored in its own preferences file beside the player's preferences, not in the versioned settings, so older builds keep reading the settings file; a player who never set it gets the default. Typing with a keyboard and clicking a field with the mouse are unchanged.
- **Why:** A match must be playable on a controller alone, and on the Steam Deck's touchscreen. The button map, the size and the dark band, and Steam's keyboard being on by default are the owner's rulings from the first Steam Deck session (2026-11-09).

### On-screen keyboard limits
- **Original:** The host's server name and the browser's direct address take whatever the keyboard types into them.
- **Port:** The on-screen keyboard caps the server name at 32 characters and the direct address at 64. The other fields keep their existing limits there (player name 20, passwords 32, chat 128). Keyboard typing into the boxes is unchanged.
- **Why:** Every entry on the on-screen keyboard is bounded; these cover any real server name or address with a port.

### Chat shows "Say:" and "Team say:"
- **Original:** Talk (T) and Team Talk (Y) open the console line with the editable text "Say " or "TeamSay " already typed after the "(>" prompt.
- **Port:** That text shows as a "Say:" or "Team say:" label before what the player types, for keyboard chat too, and the line sent is the same. Backspace on an empty line turns the label back into editable text, as Backspace over the typed "Say " did. With a controller, A on the chat line opens the on-screen keyboard over chat: gameplay keys still held are released when it opens, and chat continues as before when it closes.
- **Why:** Owner ruling (2026-11-08): the label shows which channel the line goes to, by keyboard or by controller.

### Chat and taunts by controller and from the game menu
- **Original:** Chat opens only from the keyboard (T for Say, Y for Team Say), and taunts play only from their keys (taunts 1 to 6 and a random one). The game menu has no chat or taunt entries.
- **Port:** On a controller, a tap of D-pad up opens chat and a hold of half a second opens team chat, with the on-screen keyboard (or, on a Steam Deck, Steam's keyboard unless the player turned it off) open in the same step; D-pad down still plays a random taunt. The half second is the same hold time that separates drop from drop tainted. While a Battle player is picking a team, D-pad up still picks team 3, and in the citadel editor it still cycles the view. The game menu gains Chat, Team Chat and Taunts, right after Help. Chat and Team Chat close the menu and open the chat line; after controller input the on-screen keyboard opens with it, and after mouse or keyboard input it is the ordinary typing line. Taunts opens a page with Taunt 1 to Taunt 6 and Random, whose entries wrap like the menu's; choosing one closes the menu and plays that taunt, exactly as its key does. B, View or Escape on that page return to the game menu. The lines sent and the taunts played are the same as from the keys.
- **Why:** A match must be playable on a controller alone, and the menu entries also reach chat and taunts by touch on the Steam Deck.

## Networking

### Mover clients sync from a server time stamp
- **Original:** Movers send their interpolation progress to clients and periodically snap them to the server position.
- **Port:** The server sends the move phase plus one server-time stamp. Each client works out its own progress locally, so late joiners and reversals line up without transform streaming.
- **Why:** Fewer moving parts in UE5 replication, with the same visible result.

### Position corrections on world geometry
- **Original:** Server corrections are sent relative to whatever the player is standing on.
- **Port:** If the client cannot resolve that base, the correction is sent in absolute coordinates. Movers still use relative corrections.
- **Why:** The port's world geometry is built locally on each machine and cannot be referenced by name over the network.

### No ping-based collision nudge
- **Original:** Clients apply a ping-scaled position nudge when updating position.
- **Port:** Not ported. UE5 movement prediction and correction are used instead.
- **Why:** Replaced by modern prediction.

### Whirlwind lift is server-driven
- **Original:** Clients run the Whirlwind lift themselves, easing the victim toward the replicated target location every frame.
- **Port:** Only the server moves the lifted pawn. Clients follow through ordinary movement correction and smoothing.
- **Why:** UE5 character movement prediction. Corrections converge with no visible stutter, so the client-side simulation was not ported.

### Seekers use standard replicated movement
- **Original:** Seekers broadcast their server position once a second and clients snap to it.
- **Port:** Seekers use UE5's replicated movement.
- **Why:** Modern netcode gives smoother, more accurate client movement.

### Dropped item movement
- **Original:** A dropped item changes its network role and priority while it falls, then lowers its priority on a timer after landing.
- **Port:** Standard UE5 movement replication carries the fall, and the kill-floor height handles items that fall out of the world. Pickups are hidden and shown instead of having their glow zeroed and restored.
- **Why:** UE5's networking does the same job, with no visible difference.

### Replicated values, locally rebuilt presentation
- **Original:** The engine replicates display properties (mesh, skin, hidden, collision) and sounds automatically.
- **Port:** A small value is replicated instead, such as item state, wall health, creature row name, mover phase or carcass pose, and each machine rebuilds the visuals from it. One-shot blood, sound and effect bursts travel as short-lived marker actors.
- **Why:** UE5 does not replicate those display properties for free. The visible result is the same.

### Animation replicated as a named intent
- **Original:** Animation state replicates as a packed sequence byte plus rate fields.
- **Port:** A sequence name, rate, loop flag and re-fire token replicate to everyone except the controlling client.
- **Why:** Animation stays observable without a mesh, for example on a headless server. Bandwidth is small at 16 players.

### Warder fade skins replicated explicitly
- **Original:** The engine replicates the actor's style and multi-skin settings.
- **Port:** `FadeSkinIndex` replicates the fade band, and each client swaps materials locally.
- **Why:** UE5 has no native skin or style replication. The visible result is the same.

### AMA pearl delivery
- **Original:** The anti-magic aura reaches clients through a separate network spawner workaround.
- **Port:** One replicated pearl actor decides absorption on the server. Its dome, sparks and sound play locally on each machine.
- **Why:** Simpler networking with the same visible result.

### Projectile damage is server-only
- **Original:** Simulated projectile copies run the impact path on clients, and damage flows through the server's effect pipeline.
- **Port:** Clients run flight and impact teardown for their own copies, but all damage is applied only on the server.
- **Why:** A client detonation must never apply damage twice or race replicated health.

### Reflected projectiles are replaced by the server
- **Original:** A reflected weave is destroyed and spawned again aimed back at the attacker, but a seeking or instigator-less projectile simply reverses in place. Clients' simulated copies run the reflection too.
- **Port:** The server replaces every reflected projectile, seekers included, with a new replicated one that keeps its live speed. Clients only retire their own copy when it touches the reflecting pawn.
- **Why:** These projectiles are replicated once at spawn, so an in-place change on the server would never reach clients.

### Explosive Ward uses a persistent channel
- **Original:** The ward is a fire-and-forget projectile.
- **Port:** The ward keeps an open network channel, so an early server-side detonation removes it on clients right away.
- **Why:** Avoids a ghost ward lingering on clients for up to 60 s. It is cheap for a near-static actor.

### Fireworks fuse is rolled per machine
- **Original:** The fuse timer also runs on each client's simulated copy.
- **Port:** The server and each client roll their own 2-6 s fuse, so a client's burst time can differ slightly from the server's. Damage stays on the server.
- **Why:** The bouncing projectile cannot be torn down on clients any other way.

### Leeches and reflectors replicate as inert copies
- **Original:** Most leeches and reflectors exist only on the server. Only a few reflector types replicate and run on clients.
- **Port:** All of them replicate their source references to clients, but gameplay effects run only on the server.
- **Why:** One uniform rule is cheap, and keeping effects on the server prevents clients from applying them twice.

### Summons track their summoner separately
- **Original:** The summoner is stored in the engine link that also ties an actor to a player's network connection.
- **Port:** A separate server-only reference records the summoner, and the summon's connection link is left alone.
- **Why:** Tying an NPC pawn to one player's connection would break network relevancy.

### Cast request carries the aim
- **Original:** The movement updates carry the full view, so the server fires along the client's aim.
- **Port:** The cast request includes the client's control rotation, which the server applies before launching.
- **Why:** UE5 replicates control yaw but not pitch, so the server would otherwise fire flat.

### Server RPCs are validated
- **Original:** Server functions accept whatever the client sends.
- **Port:** All player server RPCs use validation. A non-finite aim rotation disconnects the client.
- **Why:** Security. A bad value would corrupt server-side traces.

### Chat input is validated on the server
- **Original:** The typing line itself limited message length and could not produce line breaks; the server trusted what it received.
- **Port:** The server drops a client that sends an over-long chat message, turns control and line-break characters into spaces, and discards empty team messages. Escape closes the typing line in one press, a plain typing line never reaches the console-command path, and the line closes on map travel but stays open through death.
- **Why:** Security. Modern remote calls accept whatever a client sends, so the server has to enforce what the original client did.

### Remote administrator commands are limited
- **Original:** The admin command passed any console line through to the server.
- **Port:** Remote admin accepts only kick, kick-ban, restart-server and open-installed-map, the same verbs the vote path uses. Admin login and logout work as before.
- **Why:** Security. Remote players do not get the host's full console.

### Join passwords are sent as a token
- **Original:** The joining player's password travelled as plain text in the connection URL and ended up in host logs.
- **Port:** The client sends a versioned SHA-256 token of the upper-cased password, and the host compares it with a token made from its own settings. Case-insensitive matching and all admission rules are unchanged. The token works like the password itself (anyone with it can replay it), so all it does is keep the plain text out of logs.
- **Why:** Keeps reusable passwords out of engine logs.

### Exact build match for release play
- **Original:** Clients and servers check package versions and can send the player to an upgrade menu.
- **Port:** In release builds, the network version includes a unique build identifier. A mismatched client is refused by UE's normal version check, and there is no in-game upgrade menu.
- **Why:** Keeps clients and servers from mixing builds, which would break replication.

### No package downloads from servers
- **Original:** A client can download missing packages from the server while connecting, with progress shown.
- **Port:** No such download exists. The connecting message shows only the host and map, and URL options are hidden.
- **Why:** Cooked UE5 builds have no runtime package transfer. Hiding the options keeps passwords and similar values off the screen.

## Online

### Internet play through Epic Online Services
- **Original:** Players found internet games through a third-party master-server list that no longer exists.
- **Port:** The host runs a listen server and advertises one game. Internet hosting and browsing use Epic Online Services: an anonymous device sign-in with no Epic account, one lobby per listen host, and peer-to-peer connections that fall back to a relay. LAN play and direct connect by hostname or IPv4 address (with an optional port) work as before, but an internet host accepts only peers that connect through the service, so Direct Address cannot reach it.
- **Why:** The original service is gone, and a modern hosted service is the practical replacement.

### Privacy disclosure, identity reset and relay option
- **Original:** There was no online identity to manage.
- **Port:** A one-time disclosure appears before anything is sent to the online service; declining keeps LAN play. Players can reset their anonymous online identity and can choose to always route traffic through the relay. Shipping builds ignore command-line overrides of the online configuration.
- **Why:** Player privacy and consent, and protection against tampering on a modern online service.

### No spectator logins
- **Original:** Players can join as spectators with a spectator-only connect option, up to 2 per server.
- **Port:** Spectator logins are not supported.
- **Why:** The front end and connect flows never request it, and there is no spectate control in the interface.

## Presentation

### Modern renderer
- **Original:** A software or Glide/Direct3D 7 rasterizer.
- **Port:** UE5 forward rendering with no temporal anti-aliasing and no MSAA, and tone mapping turned off so colors match the original. Lighting still comes from the original's own lightmaps.
- **Why:** Modern platform rendering that keeps the original look.

### No 16-bit framebuffer banding
- **Original:** Output goes through a 16-bit framebuffer (dithered on real 3dfx hardware).
- **Port:** Output is full-color. A flat banding step is not reproduced.
- **Why:** The banding works on the final mixed color, so it cannot be baked into textures, and real hardware dithered it away.

### Brightness uses the Glide gamma ramp, scene only
- **Original:** The brightness setting changed the hardware gamma ramp, which lifted the whole screen including HUD and menus. The Direct3D renderers used different curves.
- **Port:** The Glide ramp, the 1999 default device, is applied to the 3D scene. HUD and menus draw after it and keep their unramped brightness.
- **Why:** Engine difference. The ramp is applied in post-processing instead of on the display hardware.

### World geometry crack and sliver repair
- **Original:** Small T-junction cracks, thin fan slivers and a few near-black lightmap ribbons showed in some maps.
- **Port:** At load time the render copy of world and mover geometry is repaired: cracks are closed, bad fan diagonals are swapped, and isolated sub-unit dark ribbons borrow the lighting of the coplanar edge next to them. Collision and the original data are untouched.
- **Why:** Presentation hardening. Gameplay is unaffected.

### Sorting of animated world surfaces
- **Original:** World geometry is one rendered whole.
- **Port:** On maps with animated procedural surfaces, the world is split into one piece per surface, so see-through surfaces sort against tighter bounds.
- **Why:** Each surface needs its own visibility signal to drive the procedural texture timing below.

### Texture mipmaps generated by UE5
- **Original:** Textures use the mip levels stored in the game packages, as palette-indexed images.
- **Port:** Only the full-size image is taken from the player's game files. Smaller mips are rebuilt with UE5's standard filter, and masked textures have their key color bled into the transparent pixels.
- **Why:** Matches how UE5 prepares imported textures, and avoids colored fringes around transparent holes.

### Texture filtering
- **Original:** World textures were filtered isotropically with a detail bias of -1.5, so grazing floors and ceilings blurred a short way from the camera.
- **Port:** Smooth world and mover textures use anisotropic filtering with blended mips and an LOD bias of -1. Surfaces flagged NoSmooth use point sampling.
- **Why:** Presentation modernization for modern GPUs. UE5 only accepts a whole-number bias, and -1 is the closer choice that does not over-sharpen.

### Detail textures on
- **Original:** The shipped default Direct3D setup had detail textures off; the Glide renderer drew them.
- **Port:** Detail textures are drawn, matching the Glide look. The detail does not follow the base texture's pan, and its fade is per pixel rather than per vertex.
- **Why:** A presentation choice, toward the 1999 reference look.

### Missing-texture tiling
- **Original:** Surfaces with no texture draw the engine default texture at its own scale.
- **Port:** The default texture is substituted, but UVs tile at a denser fallback rate.
- **Why:** Pipeline normalization. It averages out to the same look at distance.

### Mesh lighting is per pixel
- **Original:** Mesh actors are lit per vertex with Gouraud shading.
- **Port:** The same lighting formula is evaluated per pixel. It measured closer to the original than a per-vertex variant.
- **Why:** UE5 material pipeline, with smoother shading and no gameplay effect. Highlights match, and midtones differ slightly.

### Mesh light count and moving brushes
- **Original:** The actor light cap depends on the viewer's zone and scene complexity, and the lights for each face of a moving brush (mover) are picked on every rendered frame.
- **Port:** Mesh lighting uses the actor's own zone maximum, capped at 16. Mover faces pick their lights again on a 0.25-second timer whenever the mover moves or turns, with at most 20 lights per face (the shipped maximum is 17).
- **Why:** The light bake has no viewer, and the timer is a cheap material-only update with at most one interval of lag.

### Dynamic lights on world surfaces and meshes
- **Original:** Each surface keeps its own list of up to 256 lights, merged in light space with per-texel rounding and random flicker.
- **Port:** The eight brightest lights near the viewer apply, added together in display space. Flicker comes from a repeatable hash, and per-texel rounding is not reproduced.
- **Why:** This is how a modern shader approach works. Only a ninth or later light at the same moment is dropped.

### Animated lightmap shimmer
- **Original:** Lightmaps are re-merged every frame, with random values stepped through each light's own texel box.
- **Port:** The shared lightmap is rebuilt on the CPU each frame. Random values are stepped through the surface's interior texels in row order.
- **Why:** An equivalent method. Endpoints and averages match, and the only difference is a fixed offset into the random table.

### Dynamic light on fixed pickups
- **Original:** A few fixed pickups are marked as dynamic lights.
- **Port:** Those pickups are not lit dynamically.
- **Why:** The port's pickup actors have no setting for it.

### Translucent blending
- **Original:** Translucent surfaces keep the background color separately per color channel.
- **Port:** World translucent materials use one opacity value, taken from the strongest channel. This is exact for grey effects and over black backgrounds, and slightly darker on strongly colored ones. Sky sheets over visible geometry use an exact per-channel blend.
- **Why:** UE5's alpha-composite blending cannot apply a separate factor per channel, and a scene-color alternative would break stacked translucent layers.

### Mesh-particle translucency
- **Original:** Mesh `bParticles` draws every current source vertex with screen-aligned texture UVs. Vertex positions ignore actor DrawScale; only each icon's width and height use it (`UnMesh.cpp:433,574`, `UnMeshRn.cpp:384-460`). Translucency blends each color channel as `Src + Dst*(1-Src)` (`und3d.cpp:1261-1265`).
- **Port:** Mashadar and the mesh-effect particle clouds (including all five elemental shields) preserve that vertex space at spawn and through scale changes; growth changes icon size only. Mashadar includes the animated unreferenced tip vertex on both asset and runtime paths; editor recovery caches the decoded frames per retail identity. Its MyTex2 texture is a live WetTexture on both paths. Runtime mesh resolution remains guarded in production; automation uses an explicit test entry point. The shared mesh-particle renderer still uses a single additive pass, `Src + Dst`, making overlap/background contributions brighter than retail.
- **Why:** An exact replacement must preserve per-particle ordering of background attenuation and addition. Adding one pooled modulation pass before the pooled additive pass is not equivalent for overlapping puffs. A contained follow-up should implement and visually verify ordered per-channel screen blending; this difference is not corrected by changing cloud density, sprite size or glow.

### Volumetric fog
- **Original:** Fog maps are drawn per surface and screen-blended over the frame per color channel, only on opaque surfaces in fog zones and only while the viewer stands in a fog zone.
- **Port:** Each fog light draws as a sphere that reproduces the same per-channel screen blend, with the fog amount worked out per pixel. It also covers pixels behind the sphere, including translucent ones, and the pass is gated on the camera being inside a fog zone's bounding box.
- **Why:** Engine difference. No visible effect was found in any multiplayer map.

### Sky drawn as a camera-locked backdrop
- **Original:** The sky is rendered live as a child view starting in the sky zone.
- **Port:** The sky-zone geometry is drawn as a camera-locked backdrop that turns with the view but never moves, rendered into a cube capture that the sky surfaces sample. Texture panning stays live.
- **Why:** UE5 has no UE1-style child-view rendering.

### Mirror surfaces
- **Original:** A mirror drew a mirrored child view clipped to the surface's screen area. Mirrors could nest, and the viewer's own weapon showed in the reflection.
- **Port:** One scene capture per mirror plane renders the whole reflected view. A mirror seen inside a reflection shows its own main reflection. The sky stays centered on the viewer, first-person-only items do not appear, and the underside of a one-sided pond is not drawn. Platforms without clip-plane support skip mirrors.
- **Why:** Engine difference. This is the closest modern equivalent within the frame budget.

### Procedural textures animate when recently seen
- **Original:** A procedural fire, water or ice texture advances only on frames where a visible surface draws it.
- **Port:** A texture advances when any surface using it was drawn within the last 0.1 seconds. Each live texture also runs its warm-up steps as soon as it is bound, so it never shows its raw starting image.
- **Why:** UE5 only reports visibility after the frame is drawn, so this is the closest stable equivalent.

### Random order across procedural textures
- **Original:** All procedural textures share one random stream, used in whatever order the renderer draws them.
- **Port:** Live textures take from the shared stream in the order they were first activated.
- **Why:** UE5 does not expose the order it submits draws, so that order cannot be reproduced.

### Rising-fire bottom edge
- **Original:** Rising fire wraps at the bottom edge using the bottom two rows as they were before the flame spread.
- **Port:** The last rows read the already-updated top rows. Only rising fire differs, and only at that one boundary row pair.
- **Why:** A minor implementation difference: a few pixels of flame texture.

### Wet textures start from still water
- **Original:** A wet texture starts from its saved ripple state.
- **Port:** A wet texture starts from calm water, and its drops rebuild the motion within seconds.
- **Why:** The saved ripple state is original game data, which the port does not ship.

### Particle sprayers
- **Original:** Sprayers build their plumes up from empty the first time they are seen, and support a linear distribution mode and mesh particles.
- **Port:** The first time the camera comes near, an emitter is pre-simulated until its plume is fully formed. Linear distribution is approximated as weighted random, with the same long-run mix. Mesh particles are not implemented.
- **Why:** Matches how long-running matches look. These details are cosmetic, and the core particle behavior is ported as it was.

### Cosmetic randomness uses local streams
- **Original:** Cosmetic random choices drew from the same random generator as gameplay.
- **Port:** Sprayer, beam, mesh-effect, Earth Tremor and Decay cosmetic choices use local per-visual random streams. Dedicated servers use none.
- **Why:** A listen host's cosmetics must not change authoritative gameplay randomness.

### Orbit motion is deterministic
- **Original:** ActorRotator randomizes its orbit state on the server and replicates it.
- **Port:** Each rotator seeds its randomness from its actor name, so every client and every run shares the same phases without replication.
- **Why:** The motion is visual only, and the result looks the same with no network traffic.

### Mashadar fade cadence
- **Original:** Mashadar fades run per rendered frame.
- **Port:** Mashadar fades use a fixed 30 Hz equivalent. Fade swell changes icon size without moving the vertex cloud, as retail does. Sprite texture animation is separately shared by retail identity and advances on observed draws.
- **Why:** Frame-rate independence for the fade; modern frame rates would otherwise shorten its duration.

### Lightning beams draw straight
- **Original:** Lightning streaks can bend through navigation points around obstacles.
- **Port:** Beams draw as a direct line from caster to target.
- **Why:** The lightning latch needs line of sight, so the bend never changes what connects.

### Light Sphere pixies
- **Original:** The pixie jar eases toward its target once per frame, with world collision on the jar.
- **Port:** The ease is fixed to a 30 Hz step, so it looks the same at any frame rate. The jar has no collision, and each machine reads the followed actor directly.
- **Why:** Purely cosmetic, and it removes the dependence on frame rate.

### Network effect edge cases
- **Original:** Clients simulate the Air Burst and Explosive Ward glow from replicated data.
- **Port:** If the caster is not yet known, Air Burst moves in world space and rebases without a snap. A client that joins mid-glow-cycle starts that half-cycle at its local endpoint.
- **Why:** Robust handling of late or missing references.

### AppearEffect fade-in
- **Original:** The appear rings can fade in, but only when a flag that no shipped effect sets is on.
- **Port:** The fade-in is not implemented.
- **Why:** Multiplayer games never fade, so nothing changes in play.

### TrapBurst lingers slightly
- **Original:** The burst vanishes when its animation finishes.
- **Port:** It holds the last frame about 0.4 seconds longer.
- **Why:** A fixed lifespan is used instead of an animation-end hook.

### Decay drip
- **Original:** A falling drip sprite leaves a splat.
- **Port:** The general flying-decal system replaces the falling sprite phase.
- **Why:** Reuses one mechanism with the same landing result.

### Decal distance shrink and decal cap
- **Original:** The renderer shrinks decals with distance and can override the maximum decal count.
- **Port:** Neither renderer behavior is reproduced. Map and class decal limits, shrink and expiry still apply.
- **Why:** The modern engine exposes neither behavior at this layer.

### Underwater bubbles
- **Original:** Each bubble is its own replicated actor.
- **Port:** One local group actor draws billboard bubbles from a server-decided marker.
- **Why:** Cheaper, and looks the same.

### Startup-scene lightning timing
- **Original:** Lightning flashes are timed by the actor's clamped frame delta and a latent sleep that wakes half a frame late.
- **Port:** Flashes use exact elapsed deadlines.
- **Why:** The scene is presentation only, so its timing is bounded and frame-rate independent.

### Curved surfaces on round meshes
- **Original:** Curved-surface subdivision existed but shipped switched off.
- **Port:** Meshes flagged as curvy get two fixed subdivision levels: AMAPearl, the shield pickup, the BFExpGlobe explosion globe, and the Battle glass cylinders. The Anti-Magic Aura pickup box and the Absorb ring stay flat.
- **Why:** Presentation modernization that rounds low-poly spheres.

### Shield visuals drawn as surfaces
- **Original:** The five shield visuals drew a sprite at each mesh vertex.
- **Port:** They draw the mesh surface, unlit, in the same colors.
- **Why:** Engine difference.

### Vertex-mesh Fatness normals
- **Original:** Fatness normals were computed from the interpolated vertices.
- **Port:** Cached per-frame normals are blended, then renormalized. This can differ slightly while morphing between frames; neutral Fatness is identical.
- **Why:** Engine difference in the runtime mesh path.

### Spear thrust animation fitted to its timer
- **Original:** The thrust lasts as long as its animation.
- **Port:** The animation rate is fitted to the fixed thrust window, and the timers stay authoritative.
- **Why:** Keeps the timing of the simulation exact.

### Decoration animation details
- **Original:** Fish blend between animation links with a rate-controlled tween.
- **Port:** The fish cycle plays at one rolled rate without that tween form.
- **Why:** Minor visual simplification.

### Dying player's own death animation
- **Original:** The server picked one death animation for everyone.
- **Port:** The dying player's own client picks and plays its death variant, so they may see a different fall from the one other players see.
- **Why:** Engine replication skips the dying player's own connection for that property.

### Listen-server host animation
- **Original:** Every machine plays one-shot pawn animations, the host included.
- **Port:** On a listen server, the host's own view of some server-run pawns can miss these one-shots. Dedicated servers and remote players see them.
- **Why:** A known gap in how animation intents replay on the host.

### First-person body hiding
- **Original:** The renderer skips whatever the view target is, unless the camera is behind the player.
- **Port:** The player's own body is hidden from that player only.
- **Why:** UE5 hides by viewing player rather than by view target. The two are the same in every shipped multiplayer situation, since there is no behind-view.

### NPC held weapons
- **Original:** Weapons attach to a triangle in the hand mesh. Tossed gear on death becomes a full bounceable decoration.
- **Port:** Weapons attach to a Weapon socket where the mesh has one, otherwise at a fixed offset, because the cache-based vertex-mesh renderer does not expose attachment points. Death tosses are a simple bouncing prop that lasts 60 seconds, and some weapon materials fade by glow and a visibility cutoff instead of real translucency.
- **Why:** UE5 rendering and attachment differ. Combat is unchanged.

### NPC turn before moving
- **Original:** NPCs turn in place before starting a move.
- **Port:** Turning and moving are separate, so NPCs can turn while moving.
- **Why:** Modern movement with no gameplay effect.

### Remote Distant Eye camera
- **Original:** The view comes from the eye's own rotation, which is kept equal to the caster's view rotation.
- **Port:** The camera puts the caster's control rotation directly at the eye's position.
- **Why:** The result looks the same, and it avoids a frame of lag waiting for the replicated rotation.

### Integer HUD scaling and Mac Retina
- **Original:** The HUD is drawn at native resolution, which makes it tiny on modern displays.
- **Port:** The HUD scales up by a whole number, keeping at least 640x480 logical pixels, and `wot.HUDScale=1` restores native size. Mac builds render at full Retina resolution.
- **Why:** Display modernization. Simulation is untouched.

### HUD interface level is remembered
- **Original:** The minimized-HUD level resets each time the game starts.
- **Port:** The chosen level is saved to config.
- **Why:** Convenience. It does not affect gameplay.

### Item info window
- **Original:** The item info window uses the original window art and forces a 640x480 minimum display resolution.
- **Port:** It is drawn as a new overlay with the same content and order, and it never changes the display mode.
- **Why:** Presentation modernization. Pausing has no effect in multiplayer in either game.

### Scoreboard sorting and drawing
- **Original:** On a listen server, every local human's row is highlighted. The scoreboard draws under the HUD and ignores the HUD-hidden flag. Re-selecting a sort key can fail because of a counting bug.
- **Port:** Only the local player's row is highlighted, the scoreboard draws over the HUD under the same visibility rule, and re-selected sort keys work as clearly intended.
- **Why:** Presentation only. The original highlighting was an accident, and no shipped code calls the buggy sort path.

### Citadel victory banner
- **Original:** The scoreboard clears the victory banner in a way that keeps it from showing with two or more players.
- **Port:** The banner is worked out before the header and shows in the header slot after the clock.
- **Why:** Presentation fix. No gameplay effect.

### Banked seals fit and center on the altar
- **Original:** Banked seals took fixed slots across a row sized for the whole seal goal. On small altars the end seal hung off the edge, partial rows looked lopsided, and seals floated above the pedestal.
- **Port:** Seals already banked are centered, the row is compressed to fit the altar's radius, and each seal rests on the pedestal top. Slot order and the rules for handing seals back are unchanged.
- **Why:** Presentation only; banked seals never collide.

### Citadel editor cursor and placement preview
- **Original:** The editor uses a moving mouse cursor. The placement ghost is a replicated actor that only the editing player can see.
- **Port:** A fixed-center reticle with mouse-look still active. Hover highlights and the placement ghost are local only: the ghost is never replicated, and it cannot block a placement, count toward a team, or play sounds.
- **Why:** Modern controls, and the ghost is purely a preview for one player. Server rules are unchanged.

### Modern in-game menu
- **Original:** Escape opens the UWindow menu system, which has no Scoreboard, Help or Edit Citadel rows.
- **Port:** Escape opens a game-menu overlay with Resume, Controls, Hardware and Quit to Main Menu, plus Scoreboard, Help and Edit Citadel rows that close the menu and run the same bound action. The original order is kept: during a citadel edit, the first Escape ends editing.
- **Why:** A modern, more discoverable interface that does not change how the game plays.

### Controls and Hardware settings screens
- **Original:** Options live in the period UWindow dialogs.
- **Port:** Dedicated screens cover mouse sensitivity (the eight original steps), invert, key bindings, resolution, window mode, VSync (on by default), quality, frame-rate cap and master volume. Video changes revert after 15 seconds unless the player keeps them.
- **Why:** Modern display and input options that are safe to try.

### Front end and server browser
- **Original:** The game connects through its own console and server browser, built on the original window framework, with draggable and resizable windows, tabbed server lists, a right-click server menu and password prompts.
- **Port:** All connecting, hosting and failure handling go through a code-built front end with one server list and no dragging, resizing, tabs or right-click menu. Host, Join and direct-connect items exist only in the port.
- **Why:** The original window framework and browser are not ported.

### Menu skin, fonts and sliders
- **Original:** Menu bitmap fonts and sliders are drawn at native size for 1999 resolutions, and the server browser uses a stock grey-and-blue Windows 95 style.
- **Port:** Multiplayer menus use the port's dark charcoal and gold skin with the original Tahoma face. Menu text uses exact 2x pixel copies of the original fonts, and slider artwork is enlarged evenly.
- **Why:** Readability at modern resolutions, with no stretching and no outside fonts. The skin is a presentation choice.

### Small text meets the Steam Deck legibility targets
- **Original:** Help text could select an 8-pixel font, the item info window drew its element names in an 8-pixel font, and the chat line, the item info window's other text and the server list's resource error drew 14-pixel text. All were sized for 640x480 screens.
- **Port:** On screens shorter than 1080 pixels, such as the Steam Deck's 1280x800 and 1280x720, the chat line, its button prompts and the on-screen keyboard over it, and the item info window draw exact twice-size copies of the original fonts: 28 pixels, with the item's title in its own 30-pixel font. The item info window widens so its text wraps at about 1000 pixels, and the chat line's strip grows to hold its button prompts beside the text. Help's 8-pixel lines draw at twice size (16 pixels) in its full-screen page; if a page falls back to the 640x480 window at 1x, they use the original 14-pixel font of the same family instead, so nothing is stretched or blurred. The server list's resource error uses the menus' twice-size text on every screen. The update banner wraps a long line instead of switching to the 14-pixel font on wide screens; it still switches on the smallest windows, such as 640x480, where its twice-size text would cover the menu. A controller button's letter is never under 12 pixels. From 1080p up the chat line and the item info window are unchanged.
- **Why:** Steam Deck verification requires on-screen text of at least 9 pixels at 1280x800, and the port's own targets are 12 pixels for every text and 16 for text read during play. The original drew small text only because its screens were 640x480. Presentation only.

### HUD text at twice size on small screens
- **Original:** The HUD draws its text in its 14-pixel message font at the screen's own size, sized for 640x480. The scoreboards place their rows 20 pixels apart (14 in a Citadel match) from a quarter of the way down the screen and drop any header line that would reach the column titles.
- **Port:** Where the HUD itself draws at 1x (screens under 1280x960, such as the Steam Deck's 1280x800 and 1280x720), every HUD text draws at exactly twice its font's size: chat and kill messages, pickup names, Select Team, the connection progress lines and player names. The icons, and the numbers drawn on them (health, charges, hand keys), stay at 1x. The scoreboards and the Citadel team overview draw on a board half the screen's size, shown at twice size, which keeps the original column proportions. Every header line shows: where one would reach the column titles, the titles move down instead. Where the board doesn't fit, it first tightens (Arena rows 16 pixels apart instead of 20; Citadel team blocks without their two blank lines), and only where that doesn't fit either does it draw at 1x as before. At 1280x800 a 16-player Arena match and two Citadel teams of eight fit at twice size; four Citadel teams of four use the 1x board. From 1280x960 up nothing changes.
- **Why:** Steam Deck legibility: at 1x the HUD's text is 14 pixels tall on a 1280x800 screen. Presentation only; the scoreboards show the same rows in the same order.

### Help fills small screens
- **Original:** Help is a 640x480 window. Its own command list lays the text out in one column, with the function-key table placed at a fixed position near the bottom.
- **Port:** Where the window would draw at 1x (screens narrower than 1280 or shorter than 960 pixels, such as the Steam Deck's), Help fills the screen at twice size and reflows its page: the title, the opening line across the full width, the headed sections in two columns, the function-key table below them at its own column offsets, and the subtitle at the bottom. The shipped page uses 764 of the Deck's 800 rows of pixels (684 of 720 at 1280x720). A page that could not fit this way would keep the 640x480 window at 1x. From 1280x960 up the window is unchanged.
- **Why:** Steam Deck legibility: the 1x window's text is 14 pixels tall and uses 60% of the Deck's screen height. Presentation only; the text is the purchaser's own help page.

### Menu slider travel and host-setting stops
- **Original:** The slider knob stopped a few pixels short of each end. The host screen's kill limit, time limit, max players and max team size were numbers stepped by one. View Bob's nine steps shared the slider's eight cells, so its top two steps showed on one cell.
- **Port:** The knob reaches the track's end borders. Those host settings are sliders with one stop per unit; max players spans 2 to 16, and max team size has one stop per size the map's citadel count allows. View Bob has one stop per step.
- **Why:** Presentation and usability; all values stay reachable.

### Host seal settings and citadel count
- **Original:** The host screen had a separate citadel selector.
- **Port:** The citadel count comes from the chosen Battle map. Seal goal and budget reset to the original defaults each time the host screen opens.
- **Why:** The port's map list replaces the separate selector.

### Player name defaults to the computer name
- **Original:** An unset player name used a fixed default character name.
- **Port:** It defaults to the cleaned-up computer name (or a generic name if nothing usable is left), and follows the machine until the player sets one. The twenty-character limit is unchanged.
- **Why:** Gives players a distinct default name.

### Some built-in messages reworded
- **Original:** Some player-facing messages were written directly into the original code, not localized: no free pit opening, the brightness readout, locked-door defaults, an invalid taunt number, connection failure, and parts of the Mashadar defender-death message.
- **Port:** These use the port's own wording, or the purchaser's localized item title, at the same moments and with the same gameplay flow. Localized and map-authored values still override where the original allowed it.
- **Why:** The port does not include text copied from the original code.

### Team-change refusal and kill announcements
- **Original:** The refusal shown when a player tries to change team while editing goes to the console channel, and kill announcements go through the broadcast flood throttle.
- **Port:** That refusal is shown in the center of the screen. Player chat keeps the flood throttle, but server-composed kill announcements skip it.
- **Why:** Players can see the message, and kill announcements are server announcements, not chat.

### Optional onboarding aids
- **Original:** Items were taught by the single-player campaign: mission and tutorial maps mark chosen pickups to flash a notice and play a sound the first time they are picked up. No multiplayer map marks any, and the game's built-in pickup hint is switched on but no shipped item asks for it. In multiplayer, players opened the item info window or help themselves. The death message names the killer, never the item that did it.
- **Port:** An optional plugin, on by default, adds a field guide to every multiplayer item and trap and what counters each (F3, or a menu row), a short card the first time each item is picked up, and, after a death, a card naming the item that killed you and its counter. For that card the server tells each victim, and only the victim, which item killed them; it does so whatever its own settings, and a client with the aids off or without the plugin ignores it. The main-menu guide row sits between Multiplayer and Controls; in the game menu it sits immediately before Quit to Main Menu, which stays last. The guide fills 95% of the largest 960x540 window that fits the screen and closes on any mouse button press outside its window. Pickup cards keep their whole-step scale. Its switches are on the Controls screen, after player ID and before the key bindings. With its master switch off, or with the plugin disabled or absent, the game behaves as the original.
- **Why:** The port ships multiplayer only, so the campaign that taught items is not there.

## Audio

### Default sound radius
- **Original:** Sounds with no radius use the audio driver's default.
- **Port:** Unset radii fall back to 1600 units.
- **Why:** UE5 needs an explicit attenuation.

### Positional one-shot sounds
- **Original:** Some sounds play from the triggering actor, for example spawn-pool rarity sounds.
- **Port:** These play as positional sounds at the actor's location.
- **Why:** The port uses one audio pattern everywhere, and they sound the same to players.

### Ambient loops and random ambience
- **Original:** Looping sounds stop at range and restart on re-entry. Random ambient picks are rolled on the server and replicated.
- **Port:** Out-of-range loops stay virtualized, and random ambient picks roll on each machine.
- **Why:** UE5 audio behavior, and less network traffic for cosmetic sounds.

### Underwater muffle timing
- **Original:** Muffling is recomputed every audio frame and on every sound event.
- **Port:** The ambient muffle updates on a 10 Hz poll, and one-shots apply it when they spawn.
- **Why:** Cheaper, and inaudible in practice.

### Footsteps
- **Original:** Only BSP floors choose footstep sounds, and texture stubs can override the sound. Variation is rolled per machine. The landing thud waits one tick, and pawn mass is per pawn.
- **Port:** Any surface hit (including movers and decorations) chooses the sound by its material name, and texture stubs are not used. Variation is rolled once on the server. The thud plays at landing, using mass 175 for every pawn.
- **Why:** Richer surface coverage, and consistent sound across machines.

### Mashadar and Legion voices
- **Original:** Mashadar breathes when more than two segments grow in one tick. A dying Legion's hidden pawn hums beside its carcass.
- **Port:** Mashadar breathes on the third growth within one second. Growth can append multiple segments per tick, but the sound counter retains its one-second window. The pawn's hum stops when the carcass appears.
- **Why:** The sound counter differs from the original per-tick counter, and the port avoids a doubled hum.

### SoundDispatcher extras
- **Original:** A SoundDispatcher can choose a mixing slot per actor and look sounds up from a sound table.
- **Port:** Neither is supported. A warning is logged once if a table is used.
- **Why:** The port has no per-actor mixing slots, and no multiplayer map uses sound tables.

## Install & updates

### Built from the player's own copy of the game
- **Original:** All game content ships with the game and installs alongside it.
- **Port:** WoT Reborn ships no original game data; textures, sounds, meshes and level geometry are stripped from the client. On first launch it checks the player's original game installer and builds a verified local cache of textures, sounds, maps and lighting from it. The client refuses to start a map if that data is missing or does not match.
- **Why:** The project cannot redistribute the original assets, so the port only uses content the player already owns.

### Setup screen and locally captured backdrop
- **Original:** There was no setup or first-run content step.
- **Port:** A setup screen shows content preparation and updates. Its blurred backdrop is captured from the main-menu scene on the player's own machine the first time the main menu shows. Until then, including a fresh install's first setup screen, a default blurred capture of the same scene that ships with the game is shown; a generated gradient remains only as a fallback.
- **Why:** The port builds content from the player's own game copy, so there is no scene to capture before the first setup finishes. The shipped default is a small, heavily blurred screenshot, not original game data, and it gives the first run the same kind of backdrop later runs show.

### One movable folder with verified updates
- **Original:** A standard installer with its own settings files, and patches installed by hand.
- **Port:** On Windows the game installs as one movable folder: a launcher, a replaceable game folder, a local data folder and an update area. The updater checks each new version against signed records, switches to it in one step and rolls back if anything fails; updates replace only the game folder. Crash reporting and engine analytics are off.
- **Why:** Simple installs and safe updates that protect the install's integrity on modern systems.

### What's new notice
- **Original:** No equivalent.
- **Port:** Each packaged version shows a one-time release-notes notice.
- **Why:** Informs players about updates.

### Version label on the main menu
- **Original:** The main menu showed no version number.
- **Port:** The main menu shows the running version small in its bottom-right corner, for example `v0.1.2`; a development build shows `dev` and its commit. Update banners and the update-failed notice use the same version number when the release names one.
- **Why:** Players and testers need to see which build they are running, and updates name versions the same way the menu does.

### The game can share the original installer on the local network
- **Original:** None. This is a port-only feature for the Steam Deck install route (owner decision 3).
- **Port:** Once setup has finished, a running copy offers the original game installer it finds on this computer (beside the game, the file chosen with Browse, a copy it received over the network, or Downloads; not Downloads on a Mac) to other WoT Reborn copies on the same local network. It never listens: it broadcasts a small offer every 2 seconds from a port the system picks, reads only replies to those offers from private network addresses, and sends the file over outgoing connections, at most two at once. It checks the file's SHA-256 while sending and stops offering a file that is not the original installer. While a copy is sent, the player sees a short "Sending the original game installer to ..." notice in the bottom-left corner of every screen, menus and matches alike; it goes away 6 seconds after the last copy is sent or stopped. Settings > Hardware > Network > "Share the original installer on this network" turns it off; it is on by default. On a Mac it starts only after the player has seen the game's Local Network notice.
- **Receiving:** A copy waiting at the "Original game needed" screen can receive the installer from such a sharer. Under Wine on Linux (Proton on a Steam Deck) it listens by itself as soon as that screen appears; on Windows and Mac it listens only after the player clicks **Copy from another computer**, because listening is what raises the Windows firewall or macOS Local Network prompt (the screen says "If Windows asks, click Allow"). It answers only offers carrying this build's exact installer size and SHA-256 from private network addresses, checks for the installer's size plus 64 MB of free space first, and accepts one incoming connection from that sharer with the token it handed out; it only ever reads that connection. The file streams into `OriginalGame/<random>.part` in the game's data folder and the screen shows "Copying the original game installer from <a Windows PC / a Mac / a Steam Deck>... NN%" with a Cancel button. A complete copy whose SHA-256 matches is renamed to the installer's own name in `OriginalGame`, and setup continues exactly as for an installer found in Downloads (searched after a remembered Browse pick and before Downloads; a broken file there is deleted without changing the screen's message). A stopped copy is kept and resumed from the next sharer; a damaged one is deleted and that sharer ignored, and after three damaged copies it stops listening until the player clicks again (or reopens the game on a Deck). Cancel stops the copy, keeps it, and ignores that sharer for 30 seconds. Browse stays available throughout, and nothing listens before that screen or after it.
- **Why:** A Steam Deck can copy the 1 GB installer from the owner's PC or Mac instead of downloading it again. Nothing leaves the local network, and a copy is accepted only with the installer's exact size and SHA-256, as for an installer found on disk.

## Platform

### macOS Local Network notice
- **Original:** Not applicable.
- **Port:** On macOS, if LAN search or hosting finds Local Network access denied, the game explains how to allow it and offers to open System Settings.
- **Why:** macOS privacy settings silently block LAN discovery.

## Tooling

### Retail data read directly
- **Original:** Content came out of the original packages through the original UCC tool.
- **Port:** A pure-Python reader exports scripts and levels from the retail files on every platform, with no UCC or Wine.
- **Why:** Portable, deterministic exports on every platform.
