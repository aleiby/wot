# Arena bots: practice and server fill (design)

Issue #11. Status: approved by the owner on 2026-11-17 (revision 2, decisions A-E as
recommended), after two independent adversarial reviews (see "Review record").

The original game shipped bot classes (`WoTPawns/WOTBot.uc` and four faction
subclasses) but never wired bots into any multiplayer game type or menu. This adds
bots to Arena so a host can practise against them and a public server never looks
empty. It is new gameplay, not a port item; see the PORT_NOTES entry at the end.

## Owner rulings

1. **Purpose:** both practice (a host plays with bots) and filling sparse servers, in v1.
2. **Modes:** Arena only. Battle bots need seal and altar objectives and are a later spec.
3. **Fairness:** bots play like a player: the player body, 100 health, the same starting
   kit, and artifacts collected from the map's pickups. The shipped bot classes only
   supply each faction's artifact preferences.
4. **Control:** one setting, "Fill with bots to N players" (0 = off), independent of Max
   Players. Bots = N minus the humans present, never negative.
5. **Server browser:** bots are shown separately from players, e.g. "3/16 + 3 bots".
6. **Approach:** a player-bodied bot. An AI controller with a real, bot-flagged
   PlayerState possesses a normal player pawn.

## Decisions for the owner

New in revision 2. The owner approved every recommendation on 2026-11-17; the design
below follows them.

- **A. Plugin or core.** Recommended: an optional `WoTBots` plugin plus generic hooks
  in the game module (section 1), following the 2026-11-16 rule that new non-parity
  features are optional plugins. The alternative is the revision-1 layout, all code in
  `Source/WoT/Bots/`.
- **B. Pickup replenishment counts bots.** The spawn pools pace their respawn by the
  player count (`AWoTAngrealSpawnPool`, via `GetNumPlayers`, which counts humans only).
  Recommended: count every combatant, so one human and 15 bots play like 16 players.
- **C. Bot ceiling and dedicated servers.** Recommended: participants (humans plus bots)
  never exceed 16, the scoreboard's and Max Players' limit. A listen server therefore has
  at most 15 bots; a dedicated server with no humans can have 16. A dedicated server
  reads its fill from `DefaultBotFill` in config when the URL has no `?BotFill`.
- **D. Summoning artifacts.** Bots use summoning artifacts they pick up, by an explicit
  rule (section 4). The alternative is that bots ignore them in v1.
- **E. Inherited summon rules stay.** Two existing player rules carry over unchanged to
  bots: a summon's kill credits its owner only while the owner's current body has a
  controller, and a summon's kill doesn't trigger the frag-limit check (the original
  checks the actual killer). Recommended: keep both, for bots as for humans.
- Revision 1's decisions D1-D16 stand, except D5 (bot departures, now section 3) and D15
  (the caster changes, now section 2).

## What exists

- **The shipped bot classes:** `WotBot` expands `AngrealUser`. Each faction subclass
  carries an artifact loadout with priorities (`AII_AngrealInventoryStr`,
  `AII_Priority`) and usage intervals (`MinOffensiveAngrealUsageInterval`,
  `MinDefensiveAngrealUsageInterval`). Their MP health is 150-800, and their kits
  recharge forever. The classes are commented "stubbed in for bots -- TBD".
- **The ported NPC AI (`Source/WoT/AI`):**
  - It is server-authoritative and already runs in multiplayer (Battle troops, summons).
  - The pieces bots reuse:
    - the sight test (view cone plus line of sight);
    - friend/foe by team, including Disguise;
    - NavMesh movement;
    - `UWoTCasterComponent`'s offensive and defensive artifact choice. It already
      selects among every item in the pawn's inventory by each item's effective
      priority, and casts through the same `UseAngreal` chain players use.
- **Navigation:** NavMesh generates at map load on every imported map
  (`RuntimeGeneration=Dynamic`; `import_map.py` places the bounds volumes). No navigation
  agent is configured, so the engine default (radius 35, height 144, step 35) applies.
- **Gaps** (all verified against main 0de61a8):
  - Respawn only arms `AWoTPlayerController` victims (`AWoTGameMode`, the respawn arm
    after a kill).
  - Scoring needs the victim's and killer's PlayerState (`ScoreKill`).
  - The free-for-all team scan walks player controllers only (`ChangeTeam`).
  - `EndGame` notifies player controllers only; nothing else freezes.
  - Charge-use notifications reach a caster component only on NPC pawns
    (`AWoTCharacter::NotifyChargeUsed`), and the caster's usage-interval clocks depend on
    them.
  - The hard-landing cast rejection lives in `ServerUseAngreal`, which the caster
    bypasses.
  - No game mode spawns non-human participants.

## Design

### 1. Structure: an optional plugin over generic hooks

All bot behaviour lives in `Plugins/WoTBots/` (runtime module `WoTBots`, enabled in
`WoT.uproject`, added to the guarded package's code-plugin list like `WoTFirstPerson`).
With the plugin disabled or absent, `?BotFill` is ignored, the host panel has no bot row,
the browser shows no bot suffix, and play is identical to today.

**How the plugin pattern generalizes.** A presentation plugin (`WoTFirstPerson`,
`WoTOnboarding`) links no game class and works through inert seams and factories. Bots
are gameplay: the plugin subclasses and calls game classes (`AWoTCharacter`,
`AWoTPlayerState`, `UWoTCasterComponent`, the inventory and spawn pools). The rule that
carries over is one-way: the plugin may depend on the game module, the game module never
names the plugin, and every hook the game module gains is generic, inert without
participants that use it, and leaves humans and NPCs unchanged.

**Generic hooks in the game module:**

| # | Hook | Today | With the hook |
|---|---|---|---|
| H1 | AI participant respawn | Respawn arms `AWoTPlayerController` victims only | Also arms any controller whose PlayerState `IsABot()`, through the disposal contract in section 5. The guard against routing NPC controllers through `RestartPlayer` stays: NPCs have no PlayerState. |
| H2 | Free-for-all team scan | Walks player controllers | Walks `GameState->PlayerArray`, so any participant with a PlayerState gets a unique team. |
| H3 | Match participants | `EndGame` calls `NotifyMatchEnded` on player controllers | Also calls `IWoTMatchParticipant::OnMatchEnded` on any controller implementing the new interface. |
| H4 | Unattended restart | An ended match rotates only when a human fires after the delay (`TimeLimit=0`, the Arena default, skips the timer path) | An ended match with no humans connected restarts on its own after `GameRestartDelay` (15 s). A match with humans is unchanged. |
| H5 | Charge-use notification | Forwarded to a caster component only on NPC pawns | Forwarded to any attached `UWoTCasterComponent`, in the same order relative to the reflector fan-out. Humans have none, so nothing changes for them. |
| H6 | Casting gate | `ServerUseAngreal` rejects casts while hard landing, and after the match is decided through the player controller's ended flag | The checks move to `AWoTCharacter::CanCastNow()`, called by `ServerUseAngreal` and by the caster for non-NPC pawns. A pawn controlled by a player controller still reads that controller's ended flag, exactly as today; any other controller reads the game mode's ended state. NPC casting is unchanged. |
| H7 | Combatant count | Spawn-pool replenishment uses `GetNumPlayers()` | Uses `AWoTGameMode::GetNumCombatants()`, which counts humans plus bot PlayerStates (decision B). Connection capacity still counts humans only. |
| H8 | Dropped-item AI state | An item's AI victim (`Target`) survives a death drop and a bag pickup; the launcher prefers it over the holder's aim | When an item leaves the inventory of a pawn whose PlayerState `IsABot()` (death drop, bag, toss), it clears its AI victim and restores its class priority. Items leaving human or NPC holders are untouched, so their behaviour is unchanged; a human who loots a bot no longer inherits the bot's target. |
| H9 | Advertised session settings | No hook | The session subsystem holds an extra advertised integer map, keeps the last value across travel, and republishes on session updates. The browser row shows "+N bots" when the `WOTBOTS` key is above zero. |
| H10 | Host options | No hook | A host-option section seam, like the Settings sections: a plugin registers a row that writes a URL option. |

### 2. Components (in the plugin)

All server-side; clients see bots through ordinary replication.

- **`AWoTBotController : AAIController`, implementing `IWoTMatchParticipant`**
  - `bWantsPlayerState = true`. Its `AWoTPlayerState` is flagged with
    `SetIsABot(true)` and carries the bot's name and `FactionClass`.
  - It possesses the faction's normal player pawn and runs the think loop (section 4).
  - On possession it sets `bUseAccelerationForPaths` on the pawn's movement component, so
    path following drives acceleration like player input. The port's swimming integrates
    acceleration directly (`UWoTCharacterMovement`), and velocity-request path following
    would stall in water.
- **`UWoTBotRoster`**, a world subsystem active in Arena only: the fill target, the
  human count, adding and removing bots, bot identity, pending departures, and the
  session count (section 3).
- **`FWoTBotFactionProfile`**, one per playable class: artifact priorities and usage
  intervals, decoded at runtime from the shipped bot class defaults (`AesSedaiBot`,
  `ForsakenBot`, `HoundBot`, `WhitecloakBot`) the way creature rows are decoded. No
  retail data is copied into source. Classes the port doesn't bind are skipped with a
  log line.
- **`UWoTBotSettings`** (`UCLASS(Config=Game)`): the tunables (section 9), including
  `DefaultBotFill`.

**Reuse, not copies.** The sight and friend/foe helpers now on `AWoTNPCController` move
to `WoTAIUtil` as free functions shared by both controllers; NPC behaviour is unchanged.
`UWoTCasterComponent` gains:
- `bBuildLoadout` (default true; bots false), so it selects from the bot's own inventory
  without spawning a kit;
- `ResetAdaptation()`, which clears the adapted-item exclusion (section 4, Engage). NPCs
  never call it, so their policy is unchanged.

### 3. Roster and fill

- **Target:** `?BotFill=N` in the host URL, which the plugin's roster reads from the game
  mode's options string when the world begins play, with `DefaultBotFill` from config when
  absent (the URL wins). It is a
  non-personal option, so it survives map rotation like `FragLimit`. Arena only.
- **Desired bots** = `max(0, min(N, 16) - humans)`. Humans are PlayerStates that are
  not bots and not inactive. Participants never exceed 16 (decision C).
- **Evaluation:** on `PostLogin`, `Logout`, match start, and every 2 s.
- **Adding:** one bot per 0.5 s until the count matches. A new bot joins like a player:
  PlayerState, team assignment (H2), the ordinary join message, then `RestartPlayer`.
- **Removing:**
  - If a joining human would push participants above 16, a bot leaves at once
    (preferring a dead one, else the lowest-scoring). The newcomer never misses a
    scoreboard row.
  - Otherwise departures are graceful: a dead bot leaves at once; else the
    lowest-scoring bot is marked to leave at its next death or after
    `LeaveTimeoutSeconds` (15 s).
  - **Pending departures** are tracked. Each evaluation reconciles them against the
    current excess: if humans leave and the excess shrinks, the newest marks are
    cancelled; each departure revalidates the excess when it fires.
  - A departing bot uses the ordinary leave message. Destroying its controller runs
    `Logout` for its PlayerState. Its live summons are retired first, so their team
    number isn't reused while they fight.
- **Identity:**
  - **Class:** the four classes are dealt in a shuffled round robin; a bot keeps its
    class until it leaves.
  - **Name:** the game's own display name for the class plus a number (e.g. "<class
    name> 2"), from the original game's text at runtime; the repository adds no names.
- **Session count:** the roster publishes its bot count through H9 when it changes,
  coalesced to at most once per 2 s; the latest value is always published, including the
  final 0. A world without a roster (Battle, or the plugin off) publishes 0.

### 4. Behaviour

Each bot thinks on its own staggered timer (`ThinkInterval`, 0.1 s); movement and aim
update every frame. States: Roam, SeekItem, Engage, plus Dead while respawning and
Frozen after the match ends.

- **Perception:**
  - **Sight:** a candidate enemy is any living `AWoTCharacter` the shared friend/foe
    test calls hostile: human, bot or summon, within `SightRange`, inside the view cone
    (`SightConeDegrees`), and in line of sight to its head.
  - **Hearing:** damage reveals its instigator at once, and the bot turns toward it.
  - **Memory:** a lost target's last known position is kept for `TargetMemorySeconds`.
- **Target choice:** the nearest visible enemy, weighted up for whoever damaged the bot
  recently, with hysteresis so it doesn't flick between two players. A target that two
  or more other bots already engage counts as four times as far, unless it damaged this
  bot recently.
- **Goal choice (running the map):** after a play-test in which the bots "all group up in
  one place", the owner asked them to balance fighting with running the map to collect
  more artifacts; difficulty (aim error, reaction, sight) is unchanged. Each think
  compares a collect value with a fight value (`WoTBotDecision.h`), and the bot changes
  goal only when the other value is clearly better: more than 1.25 times the current
  value plus 0.15, so it doesn't flip-flop. The same margin keeps a chosen pool until
  another is clearly better. A body in the air keeps its goal until it lands.
  - **Collect:** `(0.5 + priority/10) * need / (1 + path/1600) * rotation * claim`, with
    the priority as in SeekItem below and `need = 0.45 + (1.2 for a class not held, else
    1 - charge fraction) + (for an offensive artifact, 0.4 per distinct charged offensive
    artifact short of 3)`. A class held at full charges is skipped. With nothing worth
    collecting, running the map toward a pool scores
    `0.45 * rotation * (0.5 + min(straight-line distance/3000, 1.5)) * claim`, so a
    stocked bot keeps moving.
  - **Fight:** `(0.15 + 1.4/(1 + distance/600)) * (1 in sight, 0.3 remembered)
    * (0.25 + 0.75 * health) * (0.25 + 0.75 * min(offensive uses/12, 1))
    * (1 + 1.5 * max(0, health - target health))`, plus 1 for a target in sight within
    `min(600, EngageWhileSeekingRange)` and 1.5 for a recent attacker, times 0.25 when two
    or more other bots engage the target and it is not attacking this bot. It is zero
    without a usable offensive artifact or summon. The target's health is read only while
    it is in sight; the starting kit's close-range artifact doesn't count as supply.
  - **Diverting:** on the way to a stocked pool a bot turns to fight only for a close
    threat or an attacker, and only when the fight value clears the margin; it still casts
    at enemies in range on the way. A bot running the map commits whenever the fight is
    clearly better: close, attacked, or clearly the stronger side.
  - **Spread:** a bot heading for a pool claims it in the roster, and other bots give a
    claimed pool 0.15 of its value. A claim ends on arrival, when the pool empties, on a
    goal change, on recovery, death or a pending departure, at retirement and at match end.
  - **Rotation:** a pool the bot collected from, found empty or reached is skipped for 3 s,
    then regains value linearly from 0.08 to 1 over 45 s; the memory survives respawns.
- **Engage:**
  - **Casting gate:** every activation goes through `CanCastNow()` (H6), so a bot can't
    cast while hard landing or after the match is decided, exactly like a human.
  - **Reaction:** a bot holds fire for `ReactionSeconds` after first seeing a target.
  - **Offence:** `SelectOffensiveWeave(Target)` then `BeginCast`/`Release`. Usage
    intervals hold because charge use reaches the caster (H5). A bot in SeekItem or Roam
    fires offensive artifacts the same way at a target in sight, without leaving its
    route; summons stay Engage-only.
  - **Disengage:** a fight with no sight of the target and no damage dealt or taken for
    3 s ends, and the bot goes back to running the map.
  - **Interrupted volleys:** the NPC policy excludes the last-adapted item from the next
    selection until it can no longer be used. A bot with only its starting kit would
    then never fire the Dart again after losing sight mid-volley (AirBurst reaches only
    120 units). A bot calls `ResetAdaptation()` when its target is lost and when a new
    target is acquired. Charge and cadence limits still apply.
  - **Aim:** the controller's rotation aims at the target's predicted position for the
    projectile's speed, plus a random error of up to `AimErrorDegrees` that shrinks while
    the target stays in view.
  - **Defence:** `FindIncomingWeave()` then `SelectDefensiveWeave()`, so a bot holding
    Reflect or a shield can use it against incoming weaves. Heal is excluded from this
    choice; a bot casts Heal only below `HealBelowHealth` (50%).
  - **Summons (decision D):** the caster's selectors skip summoning artifacts (they are
    neither offensive nor defensive). A bot casts a summoning artifact it holds when it is
    engaged and has no live helper, subject to the artifact's own helper limit and
    placement checks.
  - **Movement:** the bot holds its current artifact's standoff range and strafes; every
    `StrafeRepathSeconds` it picks a NavMesh point at that range, offset sideways from the
    target.
- **SeekItem:** the bot heads for the `AWoTAngrealSpawnPool` with the best collect value
  whose `PickupItem` is waiting. The priority is the item's faction priority if the
  profile lists it, else the class priority. Bots know which pools are stocked, like a
  player who knows the map, but never see players through walls. A pool whose route ends
  without the body touching it counts as visited.
- **Roam:** with nothing worth collecting, the bot runs to the pool with the best roaming
  value, preferring far and long-unvisited ones, and keeps it until it arrives unless
  another bot claims it too (re-scoring a goal whose distance shrinks on the way would
  chase ever farther pools). With no reachable pool, and in Engage
  when no standoff point lies on the NavMesh, it wanders to reachable NavMesh points
  biased toward pools.
- **Original scripts:** the shipped bot classes carry no item-seeking or roaming AI.
  `WoTPawns/AngrealUser.uc` keeps a commented-out experiment (`PickLocalInventory`,
  `FindAvailableInventory`) that weighs an item's desirability by its distance, considers
  only spawn pools that hold an item, checks the path, and after reaching a pool looks
  for the next. Value per distance and moving from pool to pool follow it; claims,
  crowding and rotation have no original counterpart.
- **Faction preference:** when an artifact enters a bot's inventory, its priority is set
  from the bot's faction profile if listed. When the item leaves (death drop), H8
  restores the class priority.
- **Navigation agent:** a player-sized agent (radius 17, height 92, max step 25) is added
  to the NavMesh's supported agents, and bots path on it. The existing default agent
  stays for NPCs. The cost of building a second agent at map load is measured on the
  largest Arena map before merge.
- **Stuck:** no path progress for 3 s triggers a new destination; stuck for
  `StuckRespawnSeconds` (20 s) triggers a silent respawn (section 5). Falling is not path
  progress: a body that drops through a gap in a floor's collision would otherwise fall
  for minutes before reaching KillZ.
- **Frozen:** on `OnMatchEnded` (H3) a bot releases any cast, stops moving and thinking,
  and the roster freezes: no adds, no departures, no pending respawns until travel.

### 5. Scoring, respawn and match flow

- **Scoring:** bots have PlayerStates, so `ScoreKill` credits and debits them, and a bot
  can reach the frag limit and win. Kill messages name bots like players. Summon kills
  follow the inherited player rules (decision E).
- **Respawn (H1), the disposal contract:**
  1. On death the bot controller unpossesses its pawn first, keeping the controller and
     PlayerState. A destroyed dead pawn would otherwise destroy a non-player controller
     still possessing it (`AWoTCharacter` destruction path).
  2. The corpse follows the ordinary carcass lifecycle.
  3. After `RespawnDelay` plus 0-1 s of jitter, `RestartPlayer` spawns a new pawn
     through `ChoosePlayerStart` and `SetPlayerDefaults`: the same starting kit as a
     human.
  4. If no start accepts, the bot retries every 1 s.
- **Silent stuck respawn:** the bot unpossesses and destroys its live pawn (no death, no
  score change, no drop), then follows steps 3-4. `RestartPlayer` alone can't do this: it
  keeps a pawn the controller still owns.
- **Match end:** see Frozen (section 4). With humans present, the match rotates as today.
  With none (a bots-only dedicated server), H4 restarts it after `GameRestartDelay`.
- **Map change:** bots are destroyed with the level. The next map's roster rebuilds from
  the persisted `?BotFill` (or config), and scores reset as for humans.
- **Voting, chat, taunts:** bots take no part. Vote thresholds already iterate human
  controllers.

### 6. Presentation

- **Scoreboard:** bots are listed with players, with a small "bot" tag
  (`PlayerState->IsABot()`). Participants never exceed its 16 rows (section 3).
- **Server browser:** "3/16 + 3 bots" from `WOTBOTS` (H9). Player counts stay
  humans-only, since bots hold no connections. LAN and EOS carry the key the same way;
  older clients ignore it. Steam is dormant.
- **Host panel:** a "Fill with bots to" slider beside Max Players, 0 to Max Players,
  default 0 (off), registered through H10. It travels as `?BotFill=N`.

### 7. Networking and authority

Bots exist only on the server. Their pawns, PlayerStates (`bIsABot` is replicated by the
engine) and items replicate like a human's.

### 8. Edge cases

- **Max Players reached by humans:** no bots.
- **Fill target above 16:** clamped.
- **Disguise:** bots honour it through the shared friend/foe test, as NPCs do.
- **Falling out of the world or other deaths without a killer:** an ordinary death, then
  a bot respawn.
- **Host leaves:** the session ends as today.
- **Performance:** at most 15 bots on a listen server, 16 on an empty dedicated server;
  each think loop is staggered across frames. Server cost is measured with a full roster
  before merge.

### 9. Tunables (`UWoTBotSettings`, `DefaultGame.ini`)

`DefaultBotFill` 0, `ThinkInterval` 0.1, `SightRange` 4000, `SightConeDegrees` 140,
`ReactionSeconds` 0.35, `AimErrorDegrees` 4, `TargetMemorySeconds` 4,
`StrafeRepathSeconds` 1.5, `EngageWhileSeekingRange` 1500, `HealBelowHealth` 0.5,
`LeaveTimeoutSeconds` 15, `StuckRespawnSeconds` 20, `AddIntervalSeconds` 0.5.

These are starting values to tune in play. There is one skill level in v1. The goal-choice
constants (section 4) are code constants in `WoTBotDecision.h`, not settings;
`EngageWhileSeekingRange` now only caps the close-threat range at `min(600, value)`.

### 10. Testing

Headless automation specs (`-nullrhi -unattended`), named under `WoT.Bots.*`, plus the
game-module hooks under their own systems:

- **Hooks:** H1-H10 each have a spec showing the hook works for a bot participant and
  that a human-only and an NPC game behave exactly as before. That includes the
  plugin-disabled build: no row, no suffix, `?BotFill` ignored.
- **Roster:** fill target vs humans vs the 16 ceiling; add pacing; immediate retirement
  at capacity; graceful departures; pending departures cancelled when humans leave; the
  session count coalesced and travel-safe, publishing a final 0.
- **Teams and scoring:** unique free-for-all teams with bots; `ScoreKill` credits and
  debits bot killers and victims; a bot reaches the frag limit; summon kills follow the
  inherited rules.
- **Casting:** usage intervals hold on a player body (H5); no cast while hard landing
  (H6); a starting-kit bot that loses and reacquires a distant target fires the Dart
  again; Heal only below the threshold; a summon is cast when engaged with no helper.
- **Items:** a human who loots a bot's dropped item has no inherited victim and the
  class priority (H8).
- **Respawn:** a dead bot respawns with the starting kit; a failed spawn retries; a
  silent stuck respawn changes no score and drops nothing.
- **Match flow:** after `EndGame`, bots stop casting and moving and pending timers are
  cancelled; a bots-only match restarts after the delay.
- **Movement:** a bot crosses a water volume (entry, swim, exit) and follows a route
  through a player-width passage on the player-sized agent.
- **Behaviour scenario:** in a small test arena with NavMesh, two pools and two bots,
  within 60 s both bots pick up an item and at least one kill is scored.
- **Goal choice:** collect versus fight ordering and hysteresis, pool claims and their
  release, target crowding and the attacker exception, disengaging, rotation and far
  roaming, keeping a roaming pool until arrival, casting on a route, and a pool the body
  can't touch not stalling a bot (`WoT.Bots.Decisions.*`); an endless fall ending in a
  silent respawn (`WoT.Bots.Movement.EndlessFallIsNotProgress`).
- **Soak (opt-in, `WOT_BOT_SOAK=1`):** `WoT.Bots.Soak.AllArenaMaps` runs every Arena map
  and reports, per map, pickups per bot-minute, pools visited and collected, the time
  share in each state and spent falling, and clustering: the share of one-second samples
  with three living bots, none falling, all within 600 units of one another, and the
  median and mean distance between such bots.
- **Net:** with a listen server and one client, the client sees two bot PlayerStates
  with `IsABot()`, the scoreboard rows, and the session's `WOTBOTS`.
- **Shared helpers:** the existing NPC AI specs still pass after the move.
- **Packaging:** guarded builds with the plugin enabled and disabled.

Hands-on play on a real Arena map is the final tuning check.

### 11. Out of scope for v1

Battle bots; difficulty levels; bot chat, taunts and voting; human-style bot names; an
offline (non-networked) practice mode (hosting stays a listen server).

## Review record

Revision 1 (aaab004) was reviewed independently twice, read-only, against main 0de61a8:
once on the Mac and once on Windows. Both returned "revise before implementation" with
no blocker for the player-bodied approach. Every finding was checked against the source
before it was accepted.

| Finding | Source | Resolution |
|---|---|---|
| A looted bot item keeps the bot's AI victim, and the launcher prefers it over aim | `WoTCasterComponent` (SetVictim), `WoTBagHolding`, `WoTProjectileLauncher::GetBestTarget` | H8 |
| Usage-interval clocks never update on a player body | `AWoTCharacter::NotifyChargeUsed` (NPC-only) | H5 |
| A starting-kit bot can stop firing after an interrupted volley | the caster's `AdaptedWeave` exclusion | `ResetAdaptation()`, section 4 |
| Bots could cast during a hard landing | `ServerUseAngreal` holds the only check | H6 |
| Summoning artifacts are never selected; Heal wins at full health | the caster's offensive/defensive filters; Heal's priority | Explicit summon rule and Heal threshold, section 4 |
| Velocity-request path following stalls in water | `UWoTCharacterMovement` swimming | `bUseAccelerationForPaths`, section 2 |
| The default nav agent doesn't match the player capsule | `DefaultEngine.ini`, engine defaults | Player-sized agent, section 4 |
| Bots keep acting after `EndGame`; a bots-only match never rotates | `EndGame`, `Timer` with `TimeLimit=0` | H3, H4, Frozen |
| Graceful departures can overflow the 16-row scoreboard | `WoTHUD` gathering, `WoTScoreboard` | Immediate retirement at capacity, section 3 |
| Pending departures aren't reconciled | revision 1, section 2 | Section 3 |
| `RestartPlayer` keeps an existing pawn; a dead pawn's destruction destroys its AI controller | `AWoTGameMode::RestartPlayer`, `AWoTCharacter` destruction | The disposal contract, section 5 |
| The session bot count isn't travel-safe | `UWoTSessionSubsystem` outlives worlds | H9, section 3 |
| Pickup replenishment counts humans only | `AWoTAngrealSpawnPool`, `GetNumPlayers` | H7, decision B |
| No config default for dedicated servers; 15-bot claim vs a 16-bot formula | `InitGame` URL parsing | `DefaultBotFill`, decision C |
| Summon credit after the owner's death and summon frag-limit wins | `ScoreKill`, `Killed` | Inherited player rules, decision E |
| Non-parity features belong in optional plugins (owner rule, 2026-11-16) | revision 1 put everything in the game module | Section 1, decision A |

## PORT_NOTES entry (added with the code)

### Bots fill Arena matches
- **Original:** the game shipped bot classes but never added bots to any multiplayer
  mode or menu.
- **Port:** a host can set "Fill with bots to N players". Bots play by player rules
  (player body and health, the starting kit, artifacts from pickups), using each
  faction's artifact preferences from the shipped bot classes, and leave as humans join.
  The feature is the optional `WoTBots` plugin; without it, play is unchanged.
- **Why:** with a small community, an empty server is the onboarding problem (#11).
