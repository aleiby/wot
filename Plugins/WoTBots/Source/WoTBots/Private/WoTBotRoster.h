#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTBotLinks.h"
#include "WoTBotRoster.generated.h"

class AWoTBotController;
class AGameModeBase;
class APlayerController;
class AController;
class APlayerState;
class AWoTAngrealSpawnPool;
class AWoTCharacter;

UCLASS()
class UWoTBotRoster : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	bool IsActive() const { return bActive; }
	bool IsFrozen() const { return bFrozen; }
	int32 GetBotCount() const;
	int32 GetDesiredBotCount() const;
	void Evaluate();
	void Freeze();
	void ParticipantDied(AWoTBotController* Bot);
	void ClaimPool(AWoTBotController* Bot, AWoTAngrealSpawnPool* Pool);
	void ReleasePool(AWoTBotController* Bot);
	bool IsPoolClaimed(const AWoTAngrealSpawnPool* Pool, const AWoTBotController* Except) const;
	// Engaged living bots per target, excluding one bot: shared intent, not position.
	void CountFighters(const AWoTBotController* Except, TMap<const AWoTCharacter*, int32>& Counts) const;
	int32 FightersFor(const AWoTCharacter* Target, const AWoTBotController* Except) const;
	// Links added to the bot mesh along the map's path network; INDEX_NONE until built.
	int32 GetLinkCount() const { return LinkCount; }
	// Builds those links once the bot mesh is built; returns GetLinkCount().
	int32 BuildLinks();
	// Teleporter reaches no link carries (too long for the mesh), routed by the bots.
	const TArray<WoTBotLinks::FDistantTeleport>& GetDistantTeleports() const { return DistantTeleports; }
private:
	struct FDeparture
	{
		TWeakObjectPtr<AWoTBotController> Bot;
		double Deadline = 0;
		double LastSeenTime = -1e9;
	};
	void PostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer);
	void Logout(AGameModeBase* GameMode, AController* Exiting);
	void MatchStateChanged(FName State);
	void AddBot();
	void Retire(AWoTBotController* Bot);
	void ReconcileDepartures();
	void DepartureDue();
	void ArmDepartureTimer();
	bool IsSeenByHuman(const AWoTBotController* Bot) const;
	void QueueCount();
	void PublishCount();
	int32 HumanCount() const;
	int32 ConnectedHumanCount() const;
	AWoTBotController* RetirementCandidate(bool bUnmarkedOnly) const;
	UClass* DealClass();
	FString NextName(UClass* PlayerClass) const;
	TArray<TWeakObjectPtr<AWoTBotController>> Bots;
	TMap<TWeakObjectPtr<AWoTBotController>, TWeakObjectPtr<AWoTAngrealSpawnPool>> PoolClaims;
	TArray<FDeparture> Departures;
	TSet<TWeakObjectPtr<APlayerState>> ExitingStates;
	TArray<TWeakObjectPtr<UClass>> ClassDeck;
	TArray<WoTBotLinks::FDistantTeleport> DistantTeleports;
	FTimerHandle EvaluationTimer;
	FTimerHandle AddTimer;
	FTimerHandle DepartureTimer;
	FTimerHandle PublishTimer;
	FDelegateHandle LoginHandle;
	FDelegateHandle LogoutHandle;
	FDelegateHandle MatchHandle;
	double NextAddTime = 0;
	double LastPublishTime = -2;
	int32 LastPublishedCount = INDEX_NONE;
	int32 FillTarget = 0;
	int32 LinkCount = INDEX_NONE;
	bool bActive = false;
	bool bFrozen = false;
	bool bEvaluating = false;
	bool bTearingDown = false;
};
