#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/GameModeBase.h"
#include "WoTGameMode.generated.h"

// A preceding unity translation unit may have included <windows.h>. Keep the
// retail method name from being rewritten to the Win32 FormatMessageW macro.
#ifdef FormatMessage
#undef FormatMessage
#endif

class AWoTInventory;
class AWoTAngrealInventory;
class AWoTCharacter;
class AWoTPlayerState;
class AWoTGameState;
class AController;
class AActor;
class APawn;
class AWoTPlayerController;

struct FWoTVoteRequest
{
	FString Request;
	TSet<TWeakObjectPtr<AWoTPlayerController>> Voters;
	float RemainingSeconds = 0.0f;
};

struct FWoTHostBan
{
	FString Identity;
	FString Address;
};

// Which step of UE1's player placement refused a pawn at a start (SpawnActor:
// FindSpot against the level, then CheckEncroachment against actors).
enum class EWoTStartSpawnRejection : uint8
{
	None,
	// SpawnActor itself returned no pawn.
	SpawnFailed,
	// FindSpot: no nearby spot where the cylinder fits the level (including a
	// start whose centre stays in solid BSP).
	NoFreeSpot,
	// CheckEncroachment: the placed cylinder overlaps a blocking actor.
	Encroaches,
};

struct FWoTStartSpawnResult
{
	EWoTStartSpawnRejection Rejection = EWoTStartSpawnRejection::None;
	// Where FindSpot put the cylinder (the authored point when it already fit).
	FVector ResolvedLocation = FVector::ZeroVector;
	// The actor the cylinder encroached (Encroaches only).
	FString Blocker;
};

const TCHAR* WoTStartSpawnRejectionName(EWoTStartSpawnRejection Rejection);

// Bring-up game mode: spawns AWoTCharacter at the original PlayerStarts.
// The real rules triad (giWOT/giMPArena/giMPBattle ports) replaces this
// in the game-rules phase. PickupQuery/ShouldRespawn are the
// GameInfo inventory queries those ports will refine.
UCLASS()
class AWoTGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AWoTGameMode();
	// GameInfo.SetPause gate: standalone may pause; network games require admin.
	bool TrySetPlayerPause(APlayerController* RequestingPlayer, ENetMode NetMode);
	virtual bool AllowPausing(APlayerController* PC = nullptr) override;

	// SP charge seeding interprets GameInfo difficulty indices in ascending
	// easy-to-hard order (AngrealInventory.uc:95).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") int32 Difficulty = 1;

	// giMPArena.AddDefaultInventory: every fresh player starts with 25 charges of Dart
	// (giMPArena.uc:11). EditDefaultsOnly so Battle/other rulesets can retune it; the
	// random AngrealSet1/2 Battle picks (giMPBattle.uc:187) are deferred with that mode.
	UPROPERTY(EditDefaultsOnly, Category = "WoT")
	TSubclassOf<AWoTAngrealInventory> DefaultAngreal;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") int32 DefaultAngrealCharges = 25;

	// GameInfo.PickupQuery: false = an existing item absorbed/denied the pickup
	virtual bool PickupQuery(APawn* Other, AWoTInventory* Item) const;
	// GameInfo.ShouldRespawn: never in standalone; else RespawnTime != 0
	virtual bool ShouldRespawn(AWoTInventory* Item) const;

	// Seconds a dead player waits before respawning. Retail respawn is player-fire-driven:
	// the dying state arms OKToFireTime = now + WOTPlayer.RespawnDelaySecs (=1.0;
	// WOTPlayer.uc:115,3738,7126) and the player's next Fire past that gate restarts them
	// (WOTPlayer.uc:3620). The controller's Dying FSM consumes the first eligible Fire.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float RespawnDelay = 1.0f;

	// giCombatBase.InitGame (giCombatBase.uc:20-27): the host URL overrides the match
	// rules — ?FragLimit= / ?TimeLimit= / ?Cycle= (Cycle maps to bChangeLevels). Absent
	// options keep the class defaults, matching UnrealScript's GetIntOption fallback.
	virtual void InitGame(const FString& MapName, const FString& Options,
		FString& ErrorMessage) override;
	virtual void PreLogin(const FString& Options, const FString& Address,
		const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	bool IsAdminPassword(const FString& CandidateToken) const;
	bool IsCapacityOverridePassword(const FString& CandidateToken) const;
	bool AdminLogin(AWoTPlayerController* Player, const FString& Password);
	void AdminLogout(AWoTPlayerController* Player);
	bool ExecuteAdminCommand(AWoTPlayerController* Player,
		const FString& Command);
#if WITH_AUTOMATION_TESTS
	using FWoTPlayerRemovalHook =
		TFunction<bool(APlayerController*, const FText&)>;
	void SetPlayerRemovalHookForTesting(FWoTPlayerRemovalHook&& Hook)
	{
		PlayerRemovalHookForTesting = MoveTemp(Hook);
	}
	int32 HostBanCountForTesting() const { return HostBans.Num(); }
#endif
	int32 MaxPlayers = 6;
	int32 MaxClientTickRate = 0;

	// GameInfo.Login's per-player class option: accept the retail package-qualified
	// names ("WOTPawns.AesSedai", etc.) and their bare class-name forms without case
	// sensitivity. Missing/unknown values resolve to the retail DefaultPlayerClass,
	// AesSedai. Public and pure so the URL contract can be covered by automation.
	static TSubclassOf<AWoTCharacter> ResolvePlayerClassOption(const FString& Options);

	// UE5 merges the listen server's LastURL into a relative ServerTravel before it
	// notifies clients. Remove retail's per-player URL fields from the remote copy so
	// the host cannot overwrite every client's defaults on map restart. Public/pure for
	// the restart-URL contract spec; ProcessClientTravel keeps the host URL untouched.
	static FString StripPersonalOptionsFromTravelURL(const FString& URL);

	// Stage the unmerged, sanitized request for the remote half of UE's client-travel
	// fan-out, then delegate all URL construction and server switching to the engine.
	virtual void ProcessServerTravel(const FString& URL, bool bAbsolute) override;

	// AGameModeBase receives the complete per-player URL here, after the controller's
	// PlayerState exists and before PostLogin performs the first spawn. Stamp the chosen
	// faction identity there so pawn selection and every client scoreboard see it.
	virtual FString InitNewPlayer(APlayerController* NewPlayerController,
		const FUniqueNetIdRepl& UniqueId, const FString& Options,
		const FString& Portal = TEXT("")) override;
	// giCombatBase.ChangeName rejects a name already held by another player,
	// case-insensitively, and composes its feedback from the retail localization.
	virtual void ChangeName(AController* Other, const FString& S,
		bool bNameChange) override;

	// giMPArena.FragLimit (giMPArena.uc:20): first to this many frags ends the match.
	// 0 disables (TimeLimit-only). CalcScore = Kills - Suicides.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") int32 FragLimit = 20;

	// giCombatBase.TimeLimit (minutes): 0 disables the clock (frag-limited only, the
	// giMPArena default). > 0 seeds GameState.RemainingTime = 60*TimeLimit and ends the
	// match when it runs out (Timer -> EndGame "timelimit").
	UPROPERTY(EditDefaultsOnly, Category = "WoT") int32 TimeLimit = 0;

	// giCombatBase.bChangeLevels (giMPArena sets True): rotate to the next map when the
	// match restarts, instead of reloading the current one.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bChangeLevels = true;

	// GameInfo.bCanChangeSkin (default True): allow the personal
	// ?Skin= login option and in-game ServerChangeSkin requests.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bCanChangeSkin = true;

	// giCombatBase.bSpawnInTeamArea (False in Arena; the Battle mode sets True): weight
	// FindPlayerStart toward a player's own team-area starts (AWoTPlayerStart.TeamNumber).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bSpawnInTeamArea = false;

	// giWOT.Map ("last map selected", a config var the retail host menu saved as the
	// hosted map before launching it, menuStartArenaServer.uc:141-146). The port's
	// host flow is the front end, so BeginPlay seeds it from the running world's map
	// identity; NextMap then advances it exactly like giWOT.NextMap.
	UPROPERTY(BlueprintReadOnly, Category = "WoT") FString Map;
	// giMPArena.MapPrefix ("Arena_"; giMPBattle "Battle2_"). NOT config (GameInfo
	// declares it var()), so a relaunched server always scans its class default.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FString MapPrefix = TEXT("Arena_");

	// UE1's native Actor.GetMapName: the installed
	// maps starting with Prefix, de-duplicated case-insensitively, in directory order;
	// MapName is matched case-sensitively and stepped by Dir with wrap-around; a map
	// that is not in the list yields the first one, an empty list yields "".
	static FString GetMapName(const FString& Prefix, const FString& MapName,
		int32 Dir, TConstArrayView<FString> InstalledMaps);
	// The installed map identities (retail: FindFiles over the Maps path): guarded
	// builds list the authenticated worlds, dev builds /Game/WoT/Maps.
	TArray<FString> GetInstalledMapNames() const;
	// The travel URL for a map identity: a guarded build routes through the
	// authenticated neutral shell the front end's Host uses (a bare identity is
	// refused by route admission); dev builds use the map package. Empty = none.
	static FString TravelURLForMap(const FString& MapIdentity);
	static bool CanPawnSeeStartForScoring(const APawn* Viewer,
		const AActor* Start);
#if WITH_AUTOMATION_TESTS
	void SetInstalledMapsForTesting(TArray<FString> Maps)
	{
		InstalledMapsForTesting = MoveTemp(Maps);
		bHasInstalledMapsForTesting = true;
	}
#endif

	// giCombatBase.bGameEnded — set once the match has been decided.
	UPROPERTY(BlueprintReadOnly, Category = "WoT") bool bGameEnded = false;

	// Pawn.Died -> GameInfo.Killed: score the frag and arm the victim's respawn-fire gate.
	// Killer is the killer's controller (null for a suicide/environment death).
	void PlayerKilled(AController* Killer, AWoTCharacter* Victim, FName DamageType);
	// giWOT.ScoreKill: killer +1 frag (unless self/none), victim +1 death, self/none
	// death +1 suicide. Team scoring deferred (Battle mode). Reads/writes AWoTPlayerState.
	// A killer-less (environment) death first checks the victim's SuicideInstigator: someone
	// who displaced it into the hazard within SuicideInstigationRelevance is credited instead.
	void ScoreKill(AController* Killer, AWoTCharacter* Victim);

	// MashadarManager.HurtMashadar calls Game.Killed for its named synthetic
	// focus victim. It shares ScoreKill's owner-credit, friendly-fire, accident,
	// direct-killer-name and typed-message policy without materializing a PlayerState.
	void ScoreMashadarKill(AController* Killer, uint8 DefenderTeam,
		const FString& VictimDisplayName, int32& VictimDeaths,
		int32& VictimSuicides);

	// giWOT.SuicideInstigationRelevance (giWOT.uc default 3.0): how long after a displacement
	// (Whirlwind / SwapPlaces) a death still counts as the instigator's kill.
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float SuicideInstigationRelevance = 3.0f;

	// giWOT's localized death-announcement fragments (giWOT.uc:42-46), loaded at
	// InitGame from the retail WoT.int [giWOT] (FWoTRetailText), never compiled in.
	// ScoreKill composes VictimName + fragment (+ KillerName).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Messages") FString DeathConjunction;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Messages") FString SuicideConjunctionMale;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Messages") FString SuicideConjunctionFemale;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Messages") FString AccidentConjunctionMale;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Messages") FString AccidentConjunctionFemale;

	// giWOT.FormatMessage: hack off leading spaces, then capitalize the first character.
	static FString FormatMessage(const FString& Message);

	// giWOT.ScoreKill's BroadcastMessage(..., 'WOTDeathMessage') delivery: every player's
	// WoT controller receives the typed message (WOTPlayer.ClientMessage routes it to the
	// right-message HUD queue, MP only). Virtual so specs can capture the broadcast.
	virtual void BroadcastTypedMessage(const FString& Message, FName Type);
	void ScoreKillIdentity(AController* Killer, AController* VictimController,
		AWoTCharacter* VictimPawn, AWoTPlayerState* VictimState,
		const FString& VictimName, uint8 VictimTeam, bool bVictimFemale);

	// (giWOT.OnSameTeam's controller-level fold — AreSameTeam — was subsumed by ScoreKill's
	// pawn-team comparison when NPC killers landed: NPCs have no PlayerState, so the team
	// read moved to the pawn, matching the original's pawn-PRI reads.)

	// GameInfo.ReduceDamage: the game-mode damage-reduction seam Pawn.TakeDamage consults BEFORE
	// the pawn's own shield/reflector fold (in that order in UE1's Pawn.TakeDamage). Base returns
	// Damage unchanged (GameInfo default — Arena is a free-for-all, no team damage rules); the
	// Battle mode overrides it with friendly-fire / neutral / editing-phase reductions.
	virtual int32 ReduceDamage(int32 Damage, FName DamageType, AWoTCharacter* Injured,
	                           APawn* InstigatedBy);

	// giCombatBase.ChangeTeam: the free-for-all assignment — hand the player the smallest
	// team number no other player holds (so everyone is a unique "team" / enemy). Ignores the
	// requested Team (the Battle override honors it with MaxTeamSize). Returns true on success.
	virtual bool ChangeTeam(AController* Controller, int32 Team);

	// GameInfo.Login assigns a team on join (ChangeTeam(newPlayer, InTeam));
	// the port does it in PostLogin so every player gets a unique Arena team before scoring.
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void HandleStartingNewPlayer_Implementation(
		APlayerController* NewPlayer) override;
	const FString& LastInitialSpawnFailureReasonForTest() const
	{
		return LastInitialSpawnFailureReason;
	}
	virtual void RestartPlayerAtPlayerStart(AController* NewPlayer,
		AActor* StartSpot) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(
		AController* NewPlayer, const FTransform& SpawnTransform) override;

	// GameInfo.RestartPlayer for a pawnless controller (login, death respawn): the
	// start retail's FindPlayerStart rates best is tried first. Divergence
	// (PORT_NOTES): when that start refuses the pawn, the remaining starts are
	// tried in rating order before the restart fails. A controller that still
	// owns a pawn takes the engine path, which keeps that pawn.
	virtual void RestartPlayer(AController* NewPlayer) override;

	// PlayerPawn.ServerReStartPlayer -> GameInfo.RestartPlayer: the death respawn
	// and the Battle team-select restart. Retail moves the player's one PlayerPawn
	// to the rated start (SetLocation, i.e. FarMoveActor); the port spawns a pawn
	// there with the same FindSpot rule. A live pawn is hidden and collisionless
	// while the starts are tried, and destroyed only once its replacement exists.
	// When every start refuses, the live pawn is restored where it stood and the
	// player keeps control (retail's "Restartplayer failed"). Returns true when a
	// new pawn was possessed.
	bool ServerReStartPlayer(AController* Player);

	// UE1 placement of a player pawn at a PlayerStart transform: FindSpot against
	// the level for the pawn's cylinder (bAssumeFit for SpawnActor, not for
	// FarMoveActor), then actor encroachment. Returns the pawn, or null with the
	// refusing step in OutResult.
	static APawn* SpawnPawnAtRetailStart(UWorld& World, UClass* PawnClass,
		const FTransform& SpawnTransform, APawn* SpawnInstigator,
		bool bAssumeFit = true, FWoTStartSpawnResult* OutResult = nullptr);

	// giCombatBase.FindPlayerStart's rating of every eligible start, best first;
	// equal ratings keep the level's actor order, as retail's strict `>` does.
	// bOutBestAboveFloor reports whether the best rating clears retail's -100000
	// floor; when it does not, ChoosePlayerStart falls back to the engine picker.
	void RankPlayerStarts(AController* Player, TArray<AActor*>& OutRanked,
		bool* bOutBestAboveFloor = nullptr);

	// The most recent refused start spawn (diagnostics).
	const FWoTStartSpawnResult& LastStartSpawnResult() const
	{
		return LastStartSpawn;
	}
	FString LastInitialSpawnFailureReason;
	/** Record remote-client departure before the engine invalidates its PlayerState. */
	virtual void Logout(AController* Exiting) override;
	// giCombatBase.EndGame: the match has been decided. Flags bGameEnded and drives
	// GameState.RemainingTime to -1 so Timer() forces the restart after GameRestartDelay.
	virtual void EndGame(FName Reason);

	// giCombatBase.PostBeginPlay: seed GameState.RemainingTime = 60*TimeLimit and set the
	// display-remaining flag. Public so the rules spec can (re)seed with a chosen TimeLimit
	// without rebuilding the world (a spawned mode's TimeLimit is fixed at construction).
	void SeedMatchClock();

	// giCombatBase.Timer (1 Hz): once the TimeLimit clock (GameState.RemainingTime) hits
	// zero, end the match; once it passes -GameRestartDelay after that, restart. Public so
	// the game-rules spec can pump match-flow transitions without ticking real minutes.
	virtual void Timer();

	// Engine.GameInfo.AllowsBroadcast: one shared
	// per-game, per-second character budget. The broadcaster argument is part of
	// the retail contract even though the base implementation does not inspect it.
	bool AllowsBroadcast(const AActor* Broadcaster, int32 MessageLength);

	// giCombatBase.RestartGame: with bChangeLevels, advance the rotation and travel to the
	// next map; otherwise (or when the rotation is empty) fall back to reloading the match.
	// Not an override — the port extends the minimal AGameModeBase, which has no RestartGame
	// (that lives on the heavier AGameMode); this is the GameInfo.RestartGame port.
	virtual void RestartGame();

	// giWOT's authority-owned vote table. Player controllers submit only an
	// untrusted request string; this layer admits the four shipped verbs, counts
	// unique connected voters, expires requests after 90 seconds, and executes at
	// a two-thirds majority.
	void VoteRequest(AWoTPlayerController* Player, const FString& Request);
	void ResetVoteRequest(int32 Index);
	virtual void Tick(float DeltaSeconds) override;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Vote")
	float VoteRequestTimeout = 90.0f;
#if WITH_AUTOMATION_TESTS
	int32 ActiveVoteRequestCountForTesting() const;
	float VoteRequestRemainingForTesting(int32 ActiveIndex) const;
	const FString& LastExecutedVoteRequestForTesting() const
	{
		return LastExecutedVoteRequestForTest;
	}
#endif

	// giWOT.NextMap: Map = GetMapName(MapPrefix, Map, 1); stage its travel URL as
	// PendingMapURL (empty when there is no map to go to).
	void NextMap();

	// The map RestartGame last chose to travel to (empty until a rotation fires). Recorded
	// even when the actual ServerTravel is suppressed (automation), for tests/diagnostics.
	UPROPERTY(BlueprintReadOnly, Category = "WoT") FString PendingMapURL;

	// giCombatBase.FindPlayerStart: the scored, LOS-aware start selection. Random base per
	// start, reward distance from other players, hard-reject any start a player occupies,
	// penalize starts an opponent can already see. Used on both login and every respawn.
	// (bSpawnInTeamArea team-area weighting lands with the Battle/teams slice.)
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	// Force a fresh ChoosePlayerStart on every spawn — the original scores FindPlayerStart
	// anew each time; don't let the engine cache and reuse the first StartSpot.
	virtual bool ShouldSpawnAtStartSpot(AController* Player) override;

	// GameInfo.AddDefaultInventory hook — UE calls this on each freshly (re)spawned,
	// possessed player pawn. Grants the starting loadout.
	virtual void SetPlayerDefaults(APawn* PlayerPawn) override;
	// giMP.GivePawnAngreal: give a player one of an angreal class it doesn't already
	// hold, at Charges (0 => the class's random MinInitial..MaxInitial), gated by
	// PickupQuery. Public so the game-rules spec can exercise it without a full restart.
	void GivePawnAngreal(APawn* P, TSubclassOf<AWoTAngrealInventory> Class, int32 Charges = 0);

protected:
	/** Listen-host heartbeat for a remote client's PostLogin/Logout edge. */
	void RecordClientLifecycle(APlayerController* Player, bool bLeaving);

	// Base GameInfo.Login consumes ?Skin=, but giMPBattle.Login rebuilds Options with
	// only Name/Team/Password before Super.Login (giMPBattle.uc:403-415).
	virtual bool ShouldRecordLoginSkinOption() const { return true; }

	// giCombatBase.PostBeginPlay: seed GameState.RemainingTime = 60*TimeLimit and start
	// the 1 Hz match Timer.
	virtual void BeginPlay() override;

	// Retail broadcasts the unmerged travel URL to remote clients, then appends the
	// listen player's personal defaults only to its own NextURL. Fan out the sanitized
	// request staged by ProcessServerTravel to remote net connections while preserving
	// UE's merged URL for the local PreClientTravel and engine NextURL paths.
	virtual APlayerController* ProcessClientTravel(FString& URL, bool bSeamless,
		bool bAbsolute) override;

	// Arena/base players respawn as the faction class chosen in their login URL.
	// Battle has a more-derived override which continues to select TeamClass[Team].
	virtual UClass* GetDefaultPawnClassForController_Implementation(
		AController* InController) override;

private:
	FString HostAdminPassword;
	FString HostGamePassword;
	TArray<FWoTHostBan> HostBans;
	TMap<FString, FString> PendingLoginAddresses;
	TMap<TWeakObjectPtr<APlayerController>, FString> PlayerLoginAddresses;
	void LoadHostBans();
	bool SaveHostBans() const;
	bool IsHostBanned(const FUniqueNetIdRepl& UniqueId,
		const FString& Address) const;
	bool RemovePlayer(APlayerController* Player, bool bBan,
		const FText& Reason);
	static FString StableIdentityKey(const FUniqueNetIdRepl& UniqueId);
#if WITH_AUTOMATION_TESTS
	FWoTPlayerRemovalHook PlayerRemovalHookForTesting;
#endif
	TWeakObjectPtr<AActor> RestartStartSpot;
	FWoTStartSpawnResult LastStartSpawn;
	// Set while ServerReStartPlayer runs: retail moves the existing PlayerPawn
	// (FarMoveActor) instead of spawning one (SpawnActor).
	bool bRestartMovesPlayerPawn = false;
	// Tries the rated starts in order. Outgoing (when set) is the live pawn the
	// replacement supersedes. False when every start refused the pawn.
	bool RestartPlayerAtRatedStarts(AController* NewPlayer, APawn* Outgoing);
	// Engine.GameInfo.SentText. Timer resets it every second, including matches
	// with no time limit; suppressed messages still consume budget.
	UPROPERTY(Transient)
	int32 SentText = 0;

	// giCombatBase.GameRestartDelay (15s): how long the match lingers past 0:00 (on the
	// end-game cams) before the map rotates.
	static constexpr float GameRestartDelay = 15.0f;

	// giCombatBase.bAlreadyChanged: latch so a single decided match rotates the map once.
	bool bAlreadyChanged = false;
#if WITH_AUTOMATION_TESTS
	TArray<FString> InstalledMapsForTesting;
	bool bHasInstalledMapsForTesting = false;
#endif

	// ProcessServerTravel is synchronous through ProcessClientTravel. These fields carry
	// its original request across that engine call without replacing the engine routine.
	FString RemoteClientTravelURL;
	bool bHasRemoteClientTravelURL = false;

	FTimerHandle MatchTimerHandle;
	TArray<FWoTVoteRequest> VoteRequests;
#if WITH_AUTOMATION_TESTS
	FString LastExecutedVoteRequestForTest;
#endif
	bool NormalizeVoteRequest(const FString& Request, FString& OutRequest) const;
	void BroadcastVoteCenterMessage(const FString& Message) const;
	void ExecuteVoteRequest(const FString& Request, int32 NumPlayers);
};
