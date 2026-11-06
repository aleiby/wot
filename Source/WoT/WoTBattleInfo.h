#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "Templates/SubclassOf.h"
#include "WoTBattleInfo.generated.h"

class AWoTCharacter;

// Port of BattleInfo (exports/script/WOT/BattleInfo.uc): the central, always-relevant
// replicated team-state actor for the giMPBattle citadel/siege mode. One is placed per
// Battle map (map data, like the zone volumes) carrying the team count + each team's pawn
// class; the rest is runtime bookkeeping the game mode / seal altars drive. Arrays are
// indexed by team number 0..3; 255 is the spectator / no-team sentinel.
UCLASS()
class AWoTBattleInfo : public AInfo
{
	GENERATED_BODY()

public:
	AWoTBattleInfo();

	// Map data (var() in the original): the team count this Battle map runs (2..4) and the
	// pawn class each team plays. class<WOTPlayer> -> TSubclassOf<AWoTCharacter> (the faction
	// pawn, carrying TeamDescription). Set when the actor is placed in the map.
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Battle") int32 NumTeams = 0;
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Battle") TSubclassOf<AWoTCharacter> TeamClass[4];

	// Runtime, replicated team bookkeeping (indexed by team number).
	UPROPERTY(Replicated) int32 NumPlayers[4];         // current players on this team
	UPROPERTY(Replicated) int32 NumSeals[4];           // seals in this team's possession
	UPROPERTY(Replicated) uint8 bEditing[4];           // shadow of WOTPlayer.bEditing
	UPROPERTY(Replicated) uint8 bEditingComplete[4];   // team finished editing, ready to play

	UPROPERTY(Replicated) int32 MaxTeamSize = 0;
	UPROPERTY(Replicated) bool bTeleportersDisabled = false;

	// GetTeamStatus display strings (localized string in the original; FText/localization
	// deferred as presentation). They drive the status text the citadel scoreboard shows.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString EditingStr = TEXT("Editing");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString PlayingStr = TEXT("Playing");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString ReadyStr = TEXT("Ready");
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Battle") FString WaitingStr = TEXT("Waiting");

	// --- Seal bookkeeping (BattleInfo.uc:59-80). Team-index guarded (C++ static arrays are
	// not OOB-tolerant like UnrealScript's). ---
	void ResetTeamSealCount(uint8 Team);
	void IncrementTeamSealCount(uint8 Team);
	void DecrementTeamSealCount(uint8 Team);
	int32 GetTeamSealCount(uint8 Team) const;

	// --- Team description / status (BattleInfo.uc:82-96) ---
	// 255 -> "Spectator"; otherwise TeamClass[Team]'s CDO TeamDescription ("" if unset).
	FString GetTeamDescription(uint8 Team) const;
	FString GetTeamStatus(int32 Team) const;

	// BattleInfo.PreBeginPlay: validate placed map data.
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
