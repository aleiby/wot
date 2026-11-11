#pragma once

#include "CoreMinimal.h"
#include "WoTGameMode.h"
#include "WoTBattleGameMode.generated.h"

class AWoTBattleInfo;
class AWoTBudgetInfo;
class AWoTSealAltar;
class AWoTNPCCharacter;
class AWoTMashadarGuide;
class AController;
class APlayerController;

// Port of giMPBattle (exports/script/WOT/giMPBattle.uc): the team-siege citadel mode,
// extending the Arena rules (AWoTGameMode / giMP). It owns replicated BattleInfo team/phase
// bookkeeping, team membership and faction morphs, citadel editing/economy, teleporter
// transitions, loadouts, and the JoinTeam/LeaveTeam collision + overview + style flow.
UCLASS()
class AWoTBattleGameMode : public AWoTGameMode
{
	GENERATED_BODY()

public:
	AWoTBattleGameMode();
	virtual void InitGame(const FString& MapName, const FString& Options,
		FString& ErrorMessage) override;

	// --- giMPBattle config (defaultproperties) ---
	// bAutoTeamSelect: on login, auto-assign the smallest team instead of joining neutral.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") bool bAutoTeamSelect = false;
	// MaxTeamSize (3): the per-team player cap ChangeTeam enforces. PostBeginPlay pushes it
	// onto the BattleInfo so clients see it too.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") int32 MaxTeamSize = 3;
	// SealBudget (3): the per-team citadel build budget — consumed by BudgetInfo (step 4).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") int32 SealBudget = 3;
	// bHoldNewPlayers: hold joiners as spectators until they pick a team — spectator/edit
	// phase (step 5).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") bool bHoldNewPlayers = false;
	// FriendlyFireScale (1.0): same-team damage multiplier — consumed by ReduceDamage (step 6).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") float FriendlyFireScale = 1.0f;
	// MaxSpectatorPeriod (30s): the neutral-player JoinTeamTimeout seed.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") int32 MaxSpectatorPeriod = 30;
	// bTeamGame: this is a team mode. (bSpawnInTeamArea lives on the base — giCombatBase — and is
	// set true in this mode's constructor; it drives FindPlayerStart's team-area weighting.)
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") bool bTeamGame = true;

	// giMPBattle.WinningTeam (255 = none): set by the SealAltar when the last goal seal lands;
	// read by EndGame's win/lose sound fan-out.
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Battle") uint8 WinningTeam = 255;

	// giMPBattle.EndGame (uc:488-510): on "AltarFilled" every player hears WinGame (winning
	// team) or LoseGame, owner-local, before the base end-of-match handling.
	virtual void EndGame(FName Reason) override;

	// The placed BattleInfo (map data) located at PostBeginPlay; the central team state.
	UPROPERTY(BlueprintReadOnly, Category = "WoT|Battle") TObjectPtr<AWoTBattleInfo> Battle;

	// giMPBattle.ChangeTeam: honor the requested team (unlike the free-for-all Arena base) —
	// range-check against NumTeams, no-op if already on it, reject (false) if full at
	// MaxTeamSize; otherwise move the player's NumPlayers headcount and set their team. 255 =
	// leave to neutral. Returns false ONLY when the requested team is full.
	virtual bool ChangeTeam(AController* Controller, int32 Team) override;

	// giMPBattle.GetSmallestTeam: the team with the fewest players (ties -> lowest index).
	int32 GetSmallestTeam() const;

	// giMPBattle.Login team accounting (the port assigns team in PostLogin): auto-select the
	// smallest team when configured, else join neutral (255 -> LeaveTeam).
	virtual void PostLogin(APlayerController* NewPlayer) override;
	// giMPBattle.Logout: drop the leaving player from their team's headcount, set them neutral.
	virtual void Logout(AController* Exiting) override;

	// giMPBattle.RestartGame: reset the battle, then the base rotation/reload.
	virtual void RestartGame() override;
	virtual void Timer() override;

	// giMPBattle.ReduceDamage (giMPBattle.uc:440-467): no inter-pawn damage during the editing
	// phase (teleporters disabled), neutral players neither injure nor are injured, and same-team
	// damage is scaled by FriendlyFireScale. (The neutral-zone check is deferred — "not
	// implemented" in the original.)
	virtual int32 ReduceDamage(int32 Damage, FName DamageType, AWoTCharacter* Injured,
	                           APawn* InstigatedBy) override;

	// --- Citadel-editing state (giMPBattle.uc:111-208, 469-486). The mode-side layer the editor
	// drives; the interactive editor UI + the WOTPlayer edit input are presentation/input,
	// deferred. bEditing/bEditingComplete shadow WOTPlayer state on the BattleInfo. ---
	// giMPBattle.SetEditing / IsEditing: a team's in-progress editing flag.
	void SetEditing(int32 Team, bool bOn);
	bool IsEditing(int32 Team) const;
	// giMPBattle.EnableTeamReady: mark a team done editing (ready to begin play).
	void EnableTeamReady(int32 Team);
	// giMPBattle.IsBattlefieldClear: true while no team has readied up yet (all bEditingComplete 0).
	bool IsBattlefieldClear() const;
	// giMPBattle.CanEditCitadel / CanTeleport.
	bool CanEditCitadel() const { return true; }
	bool CanTeleport() const;
	// giMPBattle.EnableTeleporters: end the editing phase — teleporters on, the match clock
	// resumes, and every teamed player is un-frozen (bTeleportingDisabled off, overview off)
	// and re-armed with the battle loadout; everyone gets the begin-battle message + conch.
	void EnableTeleporters();
	// giMPBattle.DisableTeleporters (giMPBattle.uc:132-146): freeze every player's
	// teleporting + the mode/clock flags (the editing-phase entry; ResetBattle calls it).
	void DisableTeleporters();

	// giMPBattle localized strings, plus the two WOTPlayer battle prompts this mode
	// posts: loaded at InitGame from the retail WoT.int [giMPBattle]/[WOTPlayer]
	// (FWoTRetailText), never compiled in. The team-flow fragments
	// (giMPBattle.uc:26-29) are composed by $-concatenation exactly as ChangeTeam does.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString BattleBeginStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString WaitingForSealsStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString WaitingForEditStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString TeamIsYoursStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString TeamIsFilledStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString PlayerJoinTeamStr;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString PlayerLeaveTeamStr;

	// giMPBattle.AddDefaultInventory's grant body (Dart(25) + a Set1 roll + a Set2 roll +
	// the faction default), shared by spawn-time SetPlayerDefaults and the
	// EnableTeleporters / mid-battle JoinTeam re-arm paths. No phase check here — callers
	// gate on Battle->bTeleportersDisabled exactly like the original.
	void GrantBattleLoadout(APawn* PlayerPawn);

	// WOTPlayer.ServerAssessBattlefield: play begins once every team's seal budget is spent
	// and no team still has an active editor.
	void AssessBattlefield();
	// Server-side ownership helpers for the citadel edit session/toggle path.
	uint8 NearestAltarTeam(const FVector& Loc) const;
	static uint8 NearestAltarTeamInWorld(const UWorld* World, const FVector& Loc);
	AWoTBudgetInfo* FindTeamBudget(uint8 Team) const;
	AWoTSealAltar* FindTeamAltar(uint8 Team) const;

	// G15 authority registry: only NPCs created through the citadel editor enter this
	// set. Summons/map NPCs share AWoTNPCCharacter, so class/row checks alone are not
	// sufficient removal authority. Weak entries expire with their actors.
	void RegisterCitadelDeployedTroop(AWoTNPCCharacter* Troop, uint8 Team);
	void UnregisterCitadelDeployedTroop(AWoTNPCCharacter* Troop);
	bool IsCitadelDeployedTroop(const AWoTNPCCharacter* Troop,
		uint8 ExpectedTeam = 255) const;
	void GetCitadelDeployedTroops(uint8 Team,
		TArray<AWoTNPCCharacter*>& OutTroops) const;
	// MashadarFocusPawn keeps its selected guide across edit-session unbind/rebind.
	void SetCitadelTroopMashadarGuide(AWoTNPCCharacter* Troop,
		AWoTMashadarGuide* Guide);
	AWoTMashadarGuide* GetCitadelTroopMashadarGuide(
		const AWoTNPCCharacter* Troop) const;

	// MashadarManager's per-team reusable health slots. AddMashadar is a distinct
	// first step for every newly deployed guide; RequestActivation then lets any
	// same-team crack claim the first free healthy slot.
	bool AddMashadarSlot(uint8 Team, int32 MinHealth, int32 MaxHealth);
	bool RequestMashadarActivation(AWoTMashadarGuide* Guide);
	void ReleaseMashadarActivation(AWoTMashadarGuide* Guide);
	void UpdateMashadarHealth(AWoTMashadarGuide* Guide, int32 Health);
	AWoTNPCCharacter* FindMashadarFocus(uint8 Team) const;
	void ConsumeMashadarFocus(AWoTMashadarGuide* Guide, uint8 Team);

	// giMPBattle.PickupQuery (giMPBattle.uc:474-486): no pickups for neutral players, an editing
	// player, or anyone before play begins (until teleporters are enabled).
	virtual bool PickupQuery(APawn* Other, AWoTInventory* Item) const override;

	// giMPBattle.AddDefaultInventory (giMPBattle.uc:184-195): NOTHING while teleporters are
	// disabled (the editing phase); otherwise Dart(25) + one random pick from EACH of
	// AngrealSet1/AngrealSet2 + the faction default (Air Pulse) — retail grant ORDER preserved
	// (Dart, set1, set2, AirBurst) since inventory order drives selection cycling. This
	// spawn seam also re-applies the pawn half of persistent neutral-player ghost state.
	virtual void SetPlayerDefaults(APawn* PlayerPawn) override;
	// giMPBattle defaultproperties AngrealSet1/2 (giMPBattle.uc:522-527). Set2's third slot,
	// AngrealInvWallOfAir (Sever) occupies the shipped third Set2 roll.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle")
	TSubclassOf<class AWoTAngrealInventory> AngrealSet1[3];
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle")
	TSubclassOf<class AWoTAngrealInventory> AngrealSet2[3];

	// giMPBattle.ResetBattle: reset altars/budgets/seals to server config, then disable
	// teleporters until editing completes.
	void ResetBattle();

protected:
	// giMPBattle.uc:403-415 deliberately drops the entrant's login-time ?Skin=.
	virtual bool ShouldRecordLoginSkinOption() const override { return false; }

	// giMPBattle.PostBeginPlay: locate the placed BattleInfo, push MaxTeamSize onto it, reset.
	virtual void BeginPlay() override;

	// giMPBattle: a team member's pawn class IS the faction class (TeamClass[Team]); a
	// neutral player falls through to the inherited URL-selected SpawnClass identity.
	virtual UClass* GetDefaultPawnClassForController_Implementation(
		AController* InController) override;

private:
	int32 RequestedSealGoal = 4;
	int32 RequestedSealBudget = 3;
	int32 ResolveCitadelCount() const;
	void ApplyRequestedSealSettings();

	// Locate the level-placed BattleInfo and push MaxTeamSize onto it (once). Retail did this
	// in PostBeginPlay, which in UE1 ran before any Login; UE5's direct -game LoadMap spawns
	// the local player BEFORE world BeginPlay, so the login chain must locate on demand.
	void LocateBattle();

	// giMPBattle.PostBeginPlay's battle setup (giMPBattle.uc:40-59): locate the
	// BattleInfo, apply the host's seal settings, then ResetBattle, which opens the
	// editing phase. UE1 always ran it before any Login. UE5's LoadMap logs the
	// listen host in before world BeginPlay, so whichever comes first (that login
	// or BeginPlay) runs it, once. Without it the host joined while the battle
	// still read as begun, and LeaveTeam armed a join countdown that could not run.
	void PrepareBattle();
	bool bBattlePrepared = false;

	// giMPBattle.JoinTeam / LeaveTeam: the NumPlayers accounting lives in ChangeTeam (as in the
	// original); these carry the pawn/presentation side: faction morph, loadout strip/grant,
	// owner teleporter/overview flags, collision, and the replicated neutral ghost look.
	// EnterCitadel's assert-only binding is replaced by the per-edit-session budget binding.
	void JoinTeam(AController* Controller, int32 Team);
	void LeaveTeam(AController* Controller);

	// WOTPlayer.BroadcastLeftMessage (WOTPlayer.uc:1832-1840): every player's left
	// queue. (GameInfo.AllowsBroadcast is spam throttling — not ported, matching the
	// EnableTeleporters begin-battle fan-out.)
	void BroadcastLeftMessage(const FString& Message) const;

	TArray<TWeakObjectPtr<AWoTNPCCharacter>> CitadelDeployedTroops[4];
	TMap<TWeakObjectPtr<AWoTNPCCharacter>, TWeakObjectPtr<AWoTMashadarGuide>>
		CitadelTroopMashadarGuides;
	struct FMashadarSlot
	{
		int32 Health = 0;
		TWeakObjectPtr<AWoTMashadarGuide> ActiveGuide;
	};
	TArray<FMashadarSlot> MashadarSlots[4];

};
