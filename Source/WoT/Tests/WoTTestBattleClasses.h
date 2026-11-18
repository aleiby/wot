#pragma once

#include "CoreMinimal.h"
#include "../WoTCharacter.h"
#include "../WoTEvents.h"
#include "../WoTPlayerController.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTSeal.h"
#include "WoTTestBattleClasses.generated.h"

// Test-only faction pawns with distinct TeamDescription defaults, so the BattleInfo spec
// can verify GetTeamDescription reads TeamClass[Team]'s CDO. Inert in play.
UCLASS()
class AWoTTestTeamAlpha : public AWoTCharacter
{
	GENERATED_BODY()

public:
	AWoTTestTeamAlpha() { TeamDescription = TEXT("Alpha"); }
};

UCLASS()
class AWoTTestTeamBravo : public AWoTCharacter
{
	GENERATED_BODY()

public:
	AWoTTestTeamBravo() { TeamDescription = TEXT("Bravo"); }
};

// Records every typed client message instead of routing it, so a spec can assert
// what giWOT.ScoreKill broadcast ('WOTDeathMessage' composition) without depending
// on the standalone test world's MP-only right-queue gate.
UCLASS()
class AWoTTestMessageCaptureController : public AWoTPlayerController
{
	GENERATED_BODY()

public:
	TArray<FString> CapturedMessages;
	TArray<FName> CapturedTypes;
	FRotator PendingRotationInputForTesting() const { return RotationInput; }
	void ClearRotationInputForTesting() { RotationInput = FRotator::ZeroRotator; }

	virtual void ClientTypedMessage_Implementation(
		const FString& S, FName Type) override
	{
		CapturedMessages.Add(S);
		CapturedTypes.Add(Type);
	}
};

// Captures the owner RPC that normally feeds BaseHUD's left-message queue. Chat
// routing specs use this seam so they exercise the authority fan-out without
// making several standalone controllers pretend to own one viewport.
UCLASS()
class AWoTTestLeftMessageCaptureController : public AWoTPlayerController
{
	GENERATED_BODY()

public:
	TArray<FString> LeftMessages;

	virtual void ClientLeftMessage_Implementation(const FString& Text) override
	{
		LeftMessages.Add(Text);
	}
};

// A silent cast probe for input-stack specs. It records the exact gameplay
// press/release callbacks without creating projectiles, sounds, or effects.
UCLASS()
class AWoTTestChatAngreal : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	int32 CastCount = 0;
	int32 UnCastCount = 0;

	virtual void CastArtifact() override { ++CastCount; }
	virtual void UnCastArtifact() override { ++UnCastCount; }
};

// Captures the seal possession/capture notification RPCs (G13:
// Seal.SendTakeNotification + SealAltar.SendCaptureNotification).
UCLASS()
class AWoTTestSealNotifyController : public AWoTPlayerController
{
	GENERATED_BODY()

public:
	TArray<FString> CenterMessages;
	TArray<FName> UISounds;
	TArray<float> UISoundVolumes;

	virtual void ClientCenterMessage_Implementation(const FString& Text,
	                                                bool bEcho) override
	{
		CenterMessages.Add(Text);
	}
	virtual void ClientPlayUISound_Implementation(FName SoundSubPath,
	                                             float Volume) override
	{
		UISounds.Add(SoundSubPath);
		UISoundVolumes.Add(Volume);
	}
};

// G34 review pin: a Budget EndEditEvent listener capturing the watched seal's
// editing state at fire time — retail EndState runs every EndEditingResource
// sweep BEFORE Budget.EndEdit (WOTPlayer.uc:5195-5211), so a listener must
// never observe a resource still in its editing state.
UCLASS()
class AWoTTestSealEditOrderSink : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	UPROPERTY() FName TagName;
	UPROPERTY() TObjectPtr<AWoTSeal> WatchedSeal;
	int32 TriggerCount = 0;
	bool bSawEditingGlow = false;

	virtual void BeginPlay() override
	{
		Super::BeginPlay();
		if (UWoTEventSubsystem* Events =
			GetWorld() ? GetWorld()->GetSubsystem<UWoTEventSubsystem>() : nullptr)
		{
			Events->RegisterTag(TagName, this);
		}
	}
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override
	{
		++TriggerCount;
		if (IsValid(WatchedSeal) && WatchedSeal->bEditingGlow)
		{
			bSawEditingGlow = true;
		}
	}
	virtual FName GetUE1Tag() const override { return TagName; }
};
