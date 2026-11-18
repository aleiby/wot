#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "WoTPlayerState.generated.h"

class AWoTCharacter;

// Port of the WoT deathmatch score fields carried on PlayerReplicationInfo.
// giWOT.ScoreKill tracks Kills / Deaths / Suicides; the match score used for the
// FragLimit check is giWOT.CalcScore = Kills - Suicides (a free-for-all frag count).
// Team/citadel scoring (giMPBattle) lands with that mode.
UCLASS()
class AWoTPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	AWoTPlayerState();

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Score") int32 Kills = 0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Score") int32 Deaths = 0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Score") int32 Suicides = 0;
	// PlayerReplicationInfo.bAdmin: granted by the login/admin-password paths and
	// visible to every client for the same moderation/cheat gates as retail.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Admin")
	bool bAdmin = false;

	// PlayerReplicationInfo.Team (byte; 255 = neutral / unassigned). Free-for-all (Arena)
	// hands every player a unique number so all are enemies; team modes (Battle) assign
	// 0..NumTeams-1. giWOT.OnSameTeam compares this.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Team") uint8 Team = 255;

	// WOTPlayer.PlayerClass plus PlayerReplicationInfo.PlayerType. Arena stamps the
	// login-time class before first spawn; Battle's existing faction-morph seam updates
	// the same pair. Both replicate to every client for scoreboard identity.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Faction")
	TSubclassOf<AWoTCharacter> FactionClass;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Faction")
	uint8 PlayerType = 0;

	// PlayerReplicationInfo.Color — the personal skin-variant selection the scoreboard
	// color layer reads (SB-D1): 0 Base / 1 Black / 2 Blue / 3 Red / 4 Green, classified
	// from the accepted skin texture's name by WOTPlayer.SetSkin's substring rules
	// (WOTPlayer.uc:2752-2757; the port's classifier is AWoTCharacter::ClassifySkinColor).
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Faction")
	uint8 Color = 0;
	// PlayerReplicationInfo.PacketLoss, sampled from the owning UE connection.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "WoT|Score")
	uint8 PacketLoss = 0;
	virtual void Tick(float DeltaSeconds) override;
#if WITH_AUTOMATION_TESTS
	using FPacketLossSampler = TFunction<float(const class UNetConnection*)>;
	static void SetPacketLossSamplerForTesting(FPacketLossSampler Sampler);
#endif

	// WOTPlayer.ServerChangeClass mapping (WOTPlayer.uc:4198-4201).
	static uint8 PlayerTypeForFactionClass(TSubclassOf<AWoTCharacter> NewFactionClass);
	void SetFactionClass(TSubclassOf<AWoTCharacter> NewFactionClass);

	// Seamless travel is not enabled by today's modes, but retain the faction identity
	// if it is enabled later and APlayerState replaces this instance during the handoff.
	virtual void CopyProperties(APlayerState* PlayerState) override;

	// giWOT.CalcScore — the value compared against FragLimit.
	int32 CalcScore() const { return Kills - Suicides; }

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
