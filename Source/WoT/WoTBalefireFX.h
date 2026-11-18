#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTBalefireFX.generated.h"

class APawn;
class AWoTParticleSprayer;
class UPointLightComponent;
class UWoTAmbientSoundComponent;
class UWoTSpriteBillboardComponent;

// BalefireCorona.uc: local first-person gather ornament. The owning inventory's
// replicated casting bit creates one on every rendering machine.
UCLASS()
class AWoTBalefireCorona : public AActor
{
	GENERATED_BODY()

public:
	AWoTBalefireCorona();
	void Configure(APawn* InPawn);
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;

	UPROPERTY() TObjectPtr<APawn> FollowPawn;
	UPROPERTY() TArray<TObjectPtr<AWoTParticleSprayer>> Sprayers;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTSpriteBillboardComponent> HandSprite;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UPointLightComponent> Light;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTAmbientSoundComponent> Ambient;

private:
	FVector LastLocation = FVector::ZeroVector;
	float FadeAlpha = 0.0f;
};

// Replicated removal cue. It presents BFExplodeA immediately, the victim's
// half-second overbright phase, and BFExpSprayer after the two-second delay.
UCLASS()
class AWoTBalefireRemovalMarker : public AActor
{
	GENERATED_BODY()

public:
	AWoTBalefireRemovalMarker();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;

	UPROPERTY(Replicated) TObjectPtr<AActor> Target;
	UPROPERTY(Replicated) bool bRemoveTarget = false;

private:
	float Elapsed = 0.0f;
	bool bReleasedTarget = false;
	bool bSpawnedAfterglow = false;
};

// BFExpSprayer's non-sprayer child: the expanding/fading AMAPearl globe.
UCLASS()
class AWoTBalefireAfterglow : public AActor
{
	GENERATED_BODY()
public:
	AWoTBalefireAfterglow();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	UPROPERTY() TObjectPtr<class AWoTMeshEffect> Globe;
private:
	float GlobeScale = 0.4f;
	float FrameAccumulator = 0.0f;
};

namespace WoTBalefireFX
{
	AWoTBalefireCorona* SpawnGather(UWorld& World, APawn* Pawn);
	AWoTBalefireRemovalMarker* SpawnRemoval(UWorld& World, AActor* Target,
		const FVector& Location, bool bRemoveTarget);
	AWoTBalefireAfterglow* SpawnAfterglow(UWorld& World, const FVector& Location);
}
