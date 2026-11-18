#include "WoTPlayerState.h"

#include "WoTFactionCharacters.h"
#include "Engine/NetConnection.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

namespace
{
#if WITH_AUTOMATION_TESTS
AWoTPlayerState::FPacketLossSampler GPacketLossSamplerForTesting;
#endif
}

#if WITH_AUTOMATION_TESTS
void AWoTPlayerState::SetPacketLossSamplerForTesting(FPacketLossSampler Sampler)
{
	GPacketLossSamplerForTesting = MoveTemp(Sampler);
}
#endif

AWoTPlayerState::AWoTPlayerState()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 1.0f;
	// GameInfo.DefaultPlayerClass and WOTPlayer.PreBeginPlay both default to AesSedai.
	FactionClass = AWoTAesSedai::StaticClass();
	PlayerType = 0;
}

void AWoTPlayerState::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority()) { return; }
	const APlayerController* PC = Cast<APlayerController>(GetOwner());
	const UNetConnection* Connection = PC ? PC->GetNetConnection() : nullptr;
	float LossPercentage = Connection
		? Connection->GetInLossPercentage().GetAvgLossPercentage() : 0.0f;
#if WITH_AUTOMATION_TESTS
	if (GPacketLossSamplerForTesting)
	{
		LossPercentage = GPacketLossSamplerForTesting(Connection);
	}
#endif
	const uint8 NewLoss = static_cast<uint8>(FMath::Clamp(
		FMath::RoundToInt(LossPercentage), 0, 100));
	if (PacketLoss != NewLoss)
	{
		PacketLoss = NewLoss;
		ForceNetUpdate();
	}
}

uint8 AWoTPlayerState::PlayerTypeForFactionClass(
	TSubclassOf<AWoTCharacter> NewFactionClass)
{
	const UClass* Class = NewFactionClass.Get();
	if (Class && Class->IsChildOf(AWoTForsaken::StaticClass()))
	{
		return 1;
	}
	if (Class && Class->IsChildOf(AWoTHound::StaticClass()))
	{
		return 2;
	}
	if (Class && Class->IsChildOf(AWoTWhitecloak::StaticClass()))
	{
		return 3;
	}
	return 0;
}

void AWoTPlayerState::SetFactionClass(
	TSubclassOf<AWoTCharacter> NewFactionClass)
{
	FactionClass = NewFactionClass;
	if (!FactionClass)
	{
		FactionClass = AWoTAesSedai::StaticClass();
	}
	PlayerType = PlayerTypeForFactionClass(FactionClass);
}

void AWoTPlayerState::CopyProperties(APlayerState* PlayerState)
{
	Super::CopyProperties(PlayerState);
	// No current WoT mode opts into seamless travel. This mirrors the replicated pair
	// onto the replacement state so a future opt-in cannot reset faction to Aes Sedai.
	if (AWoTPlayerState* WoTPlayerState = Cast<AWoTPlayerState>(PlayerState))
	{
		WoTPlayerState->FactionClass = FactionClass;
		WoTPlayerState->PlayerType = PlayerType;
		WoTPlayerState->Color = Color;
		WoTPlayerState->PacketLoss = PacketLoss;
		WoTPlayerState->bAdmin = bAdmin;
	}
}

void AWoTPlayerState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Score fields fan out to every client (scoreboard). PlayerReplicationInfo
	// replicated these unconditionally in the original.
	DOREPLIFETIME(AWoTPlayerState, Kills);
	DOREPLIFETIME(AWoTPlayerState, Deaths);
	DOREPLIFETIME(AWoTPlayerState, Suicides);
	DOREPLIFETIME(AWoTPlayerState, bAdmin);
	DOREPLIFETIME(AWoTPlayerState, Team);
	DOREPLIFETIME(AWoTPlayerState, FactionClass);
	DOREPLIFETIME(AWoTPlayerState, PlayerType);
	DOREPLIFETIME(AWoTPlayerState, Color);
	DOREPLIFETIME(AWoTPlayerState, PacketLoss);
}
