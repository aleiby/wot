#pragma once

// Scripted player actions for the net-test harness. Test-only; the
// whole file compiles out of Shipping. Everything drives player 0's pawn
// through the character's real input path (server RPCs), so the same calls work
// on a client (the RPC ships to the server) or standalone.

#include "CoreMinimal.h"
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

class UWorld;
class FJsonObject;
class FJsonValue;
class APlayerController;
class AWoTCharacter;

namespace WoTNetTest
{
	// Held movement axis (x = forward/back, y = strafe), applied every frame.
	struct FMoveState
	{
		FVector2D Axis = FVector2D::ZeroVector;
	};

	APlayerController* LocalPlayerController(UWorld* World, int32 PlayerIndex = 0);
	AWoTCharacter* LocalPlayerPawn(UWorld* World, int32 PlayerIndex = 0);

	// AddMovementInput on player 0's pawn along its control rotation; call once
	// per frame while a move is held (no-op if the axis is zero).
	void ApplyMoveTick(UWorld* World, int32 PlayerIndex, const FMoveState& Move);
	inline void ApplyMoveTick(UWorld* World, const FMoveState& Move)
	{
		ApplyMoveTick(World, 0, Move);
	}

	// Player 0 Fire: controller FSM dispatches live casting, dead respawn, or match restart.
	// Release remains pawn-owned (hold-to-cast safe while alive).
	bool CastPress(UWorld* World, int32 PlayerIndex = 0);
	bool CastRelease(UWorld* World, int32 PlayerIndex = 0);
	// Player 0: cycle to the next inventory item.
	bool NextArtifact(UWorld* World, int32 PlayerIndex = 0);
	bool Jump(UWorld* World, int32 PlayerIndex = 0);
	bool StopJump(UWorld* World, int32 PlayerIndex = 0);
	bool SelectArtifact(UWorld* World, const FString& ClassPath, int32 PlayerIndex = 0);

	// Apply WoT damage to the named AWoTCharacter (authority only).
	bool DamagePawn(UWorld* World, const FString& PawnName, float Amount);

	// Grant an angreal to the named AWoTCharacter via the game mode's
	// GivePawnAngreal (authority only) so the diagnostic can drive each artifact
	// family, not just the spawn loadout. ClassPath is a bare native class name
	// ("WoTAngrealInvFireball"), a "Pkg.Class", or a full object path; the granted
	// item is auto-selected, so a subsequent client castpress fires it. Charges<=0
	// rolls the class's own MinInitial..MaxInitial spread. Returns false if the
	// pawn/class/game mode can't be resolved.
	bool GiveArtifact(UWorld* World, const FString& PawnName, const FString& ClassPath, int32 Charges);
	// Evidence-only bridge for maps whose authored starting-seal census is lower
	// than SealGoal. The guarded channel creates a real, carrier-team AWoTSeal and
	// uses the production GiveTo transition (as altar hand-back does); ordinary
	// world pickup phase/team rules remain untouched. Capture/banking/win still
	// use production overlap code.
	bool GiveSeal(UWorld* World, const FString& PawnName);

	// Guarded packaged-Test orchestration. These are deliberately channel-only
	// seams over the same front-end/session entry points used by the UI.
	TSharedPtr<FJsonObject> BuildStatus(UWorld* World);
	// Negative rule fields keep DefaultHostRequest's values; non-negative fields
	// override them exactly like the host menu (BuildHostTravelURL clamps).
	bool Host(UWorld* World, const FString& Map, const FString& Game,
		int32 FragLimit = -1, int32 TimeLimit = -1, bool bLan = true,
		int32 SealGoal = -1, int32 SealBudget = -1);
	// Validate and construct the same LAN/listen URL used by the front end, but
	// for an in-process ServerTravel between play-through maps. Map is either a
	// short retail map name or its /Game/WoT/Maps package. No URL options are
	// accepted from the channel.
	bool BuildTravelURL(const FString& Map, const FString& Game, FString& OutURL);
	// Remove extra local players from the prior world, then retain the listen
	// host through ServerTravel. The runner adds fresh puppets after materialize.
	bool Travel(UWorld* World, const FString& Map, const FString& Game);
	bool Join(UWorld* World, const FString& Address);
	// The REAL multiplayer browser on the front end's Play screen: Refresh
	// with the LAN box set (BrowserFind), then select the listed row and press
	// Join (BrowserJoin) -- the Find -> JoinSession -> resolved connect string
	// -> travel path a player takes, as opposed to Join's direct address.
	bool BrowserFind(UWorld* World, bool bLan);
	bool BrowserJoin(UWorld* World, int32 Row);
	// Test builds: pick the host panel's first Arena or Citadel map, as the
	// map list does, so a screenshot can show either layout. OutScrollable is
	// how far the panel could scroll as last laid out (0 = every row fits;
	// call again after a frame for the new map's layout), -1 if unavailable.
	bool BrowserSelectHostMapKind(UWorld* World, bool bBattle, double& OutScrollable);
	// {available, operation, status, rows[{row, server, map, players,
	// max_players, ping}], session}: what the browser shows plus the session
	// subsystem's gate readout. available=false off the front end.
	TSharedPtr<FJsonObject> BuildBrowserStatus(UWorld* World);
	// Select this instance's player class for its NEXT host/join travel
	// ("AesSedai", "Forsaken", "Hound", "Whitecloak"). In-memory only: the
	// preference is assigned without saving, so no user file is written.
	bool SetFaction(UWorld* World, const FString& FactionName, int32 PlayerIndex = 0);
	bool CreatePuppets(UWorld* World, int32 Count, const TArray<FString>& Names,
		const TArray<FString>& Factions, TArray<TSharedPtr<FJsonValue>>& OutPuppets);

	// Aim player 0's control rotation (yaw) at a named pawn / a world point (target
	// looked up by name, per the identity rule). Drives the cast-at-target and
	// walk-into-volume flows the void-cast harness could not. Returns false if the
	// pawn/controller/target can't be resolved.
	bool FaceTarget(UWorld* World, const FString& PawnName, int32 PlayerIndex = 0);
	bool FacePoint(UWorld* World, const FVector& Point, int32 PlayerIndex = 0);

	// Like Face* but sets control YAW **and PITCH** — aim from the caster's eye to the
	// target so a launched weave descends into the target capsule instead of flying
	// over it at muzzle height (the yaw-only Face leaves pitch=0). AimTarget aims at
	// the target pawn's centre. Movement is unaffected (bUseControllerRotationYaw).
	bool AimTarget(UWorld* World, const FString& PawnName, int32 PlayerIndex = 0);
	bool AimPoint(UWorld* World, const FVector& Point, int32 PlayerIndex = 0);

	// Teleport the named AWoTCharacter to a world point (authority only). Deterministic
	// placement for scenarios where walking is unreliable — Arena spawns are far apart and
	// a blind forward walk can fall off map geometry into a pit. The autonomous client is
	// corrected to the server position on its next move.
	bool Teleport(UWorld* World, const FString& PawnName, const FVector& To);

	// Destroy every live AWoTProjectile on the authority (lingering AMA mines,
	// EarthTremor rocks, placed wards) so one matrix cast cannot trip or damage
	// the next one's pawns. Returns the number destroyed; -1 on a client world.
	int32 ClearProjectiles(UWorld* World);

	// Spawn an AWoTZoneVolume at `At` carrying a current (ZoneVelocity), sized to a known
	// box extent. Authority only — returns false on a client world (mirrors
	// DamagePawn/GiveArtifact's NM_Client gate). Deterministic and map-independent
	// (AWoTZoneVolume is self-managed / headless-safe).
	bool SpawnZone(UWorld* World, const FVector& At, const FVector& ZoneVelocity);

	// End the match from the harness (authority only — mirrors the NM_Client gate). Drives
	// AWoTGameMode::EndGame, so the match-end signal (owner-only bMatchEnded) replicates to the
	// clients and the `matchended` snapshot field flips there. For the game_flow scenario.
	bool EndMatch(UWorld* World);

	// Player 0's client-initiated team switch (AWoTPlayerController::ServerChangeTeam) — the
	// RPC path. NOT authority-gated: it is a Server RPC, so it ships from a client to the
	// authority (and runs directly standalone). The game mode's ChangeTeam owns the resolution
	// (Arena FFA reassigns a unique team; Battle honors the request). Returns false if player 0's
	// WoT controller can't be resolved.
	bool ChangeTeam(UWorld* World, int32 Team, int32 PlayerIndex = 0);

	// Gate weave reels on a Battle map. Retail refuses every pickup (so every
	// GivePawnAngreal) and every cast for neutral players and during the edit
	// phase (giMPBattle.uc:474-486, WOTPlayer.bTeleportingDisabled). Puts local
	// player i on Teams[i] through the ServerChangeTeam path, then ends the edit
	// phase with the game's own EnableTeleporters (the step AssessBattlefield
	// takes once every citadel is ready). True only on a Battle authority world
	// where every listed local player holds its team and teleporters are on.
	bool OpenBattle(UWorld* World, const TArray<int32>& Teams);

	// Request a viewport screenshot WITH UI (HUD + Slate) on this process — for eyeballing a
	// presentation layer (the debug HUD) that the headless specs cannot see. Written to the
	// Mac's Saved/Screenshots after the next rendered frame. Uses FScreenshotRequest (a
	// backbuffer grab), not the GEngine->Exec `HighResShot` path (that command is owned by the
	// viewport, so GEngine->Exec never consumes it). No-op without a game viewport (a dedicated
	// server) — returns false there.
	bool Screenshot(UWorld* World);
}

#endif
