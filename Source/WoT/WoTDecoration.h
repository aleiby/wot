#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTVertexAnimComponent.h"
#include "World/WoTRuntimeActorVisuals.h"
#include "WoTRetailProjectileTarget.h"
#include "WoTCollisionBlocker.h"
#include "WoTDecoration.generated.h"

class UWoTMeshLight;
class UWoTSpriteBillboardComponent;

// Placed world decoration (lamps, torches, statues...) riding the vertex-anim
// component so ambient sequences can play later. The collision root is the
// shared flat-ended UE1 cylinder and consumes the retail effective flags.
// MeshLight carries the importer-baked UE1 actor lighting (gamma scheme).
UCLASS()
class AWoTDecoration : public AActor, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTDecoration();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TObjectPtr<UWoTVertexAnimComponent> Mesh;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision")
	TObjectPtr<UWoTFlatCylinderComponent> Collision;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WoT")
	TObjectPtr<UWoTMeshLight> MeshLight;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display")
	TObjectPtr<UWoTSpriteBillboardComponent> Sprite;

	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") float ScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") float AmbientGlow = 0.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") uint8 Style = 1;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") bool bUnlit = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Display") bool bStyleTranslucent = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision") bool bProjTarget = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision") bool bCollideActors = true;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision") bool bBlockActors = true;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision") bool bBlockPlayers = true;
	// Actor.bStatic. A static decoration can never be moved, so the swap
	// artifact cannot trade places with it (SwapPlacesEffect.uc:178-191). A
	// placed decoration is static unless its class clears the flag
	// (BounceableDecoration.uc:184, BreakableDecoration.uc:122,
	// HangingLightChain.uc:20 and a few animated classes); the world projection
	// reads each placed decoration's own value.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision") bool bStatic = true;
	virtual bool IsRetailProjectileTarget() const override { return bProjTarget; }
	virtual bool IsRetailProjectileTouchTarget() const override
	{
		return bProjTarget || (bBlockActors && bBlockPlayers);
	}

	// WOTDecoration ambient animation (WOTDecoration.uc:15-22): if bAutoAnimate,
	// PreBeginPlay looped AnimationSequence at RandRange(Min,MaxAnimRate).
	// DragonFly's class defaults are 1.5-2, while authored instance Min/Max values
	// survive projection and override them exactly as they do before PreBeginPlay.
	// Local cosmetic on every machine (UE1 simulated
	// PreBeginPlay; the animation never replicates).
	UPROPERTY(EditAnywhere, Category = "WoT|Anim") bool bAutoAnimate = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Anim") FName AnimationSequence = TEXT("All");
	UPROPERTY(EditAnywhere, Category = "WoT|Anim") float MinAnimRate = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Anim") float MaxAnimRate = 1.0f;
	// Fish.uc AnimatedFish state: an endless PlayAnim chain (Frame01->02->01->03),
	// each link at a fresh RandRange(Min,MaxAnimRate).
	UPROPERTY(EditAnywhere, Category = "WoT|Anim") TArray<FName> PlayAnimCycle;
	// A static posed decoration (the level-placed WOTCarcass* corpses): hold
	// PoseSequence at the PoseFrame fraction (instance AnimSequence/AnimFrame).
	UPROPERTY(EditAnywhere, Category = "WoT|Anim") FName PoseSequence;
	UPROPERTY(EditAnywhere, Category = "WoT|Anim") float PoseFrame = 0.0f;

	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void ConfigureRuntimeCollision(bool bInCollideActors, bool bInBlockActors,
		bool bInBlockPlayers, float Radius, float HalfHeight);
	// ConfigureRuntimeCollision for the editor import (Tools/import), which
	// otherwise leaves every placed decoration on the class-default blocking
	// cylinder: Arena_06's teleporter shimmer quads turn bCollideActors off.
	UFUNCTION(BlueprintCallable, Category = "WoT|Collision")
	void SetUE1Collision(bool bInCollideActors, bool bInBlockActors,
		bool bInBlockPlayers, float Radius, float HalfHeight)
	{
		ConfigureRuntimeCollision(bInCollideActors, bInBlockActors, bInBlockPlayers,
			Radius, HalfHeight);
	}
	void ConfigureRuntimePhysics(uint8 Physics, const FRotator& RotationRate,
		bool bFixedRotationDir);
	void ConfigureRuntimeVisualPresentation(
		const FWoTRuntimeActorVisualPresentation& Presentation);
	void ConfigureRuntimeSpritePresentation(
		TArray<FString> TextureIdentities, float TextureWidth,
		float TextureHeight, float MinFrameRate, float MaxFrameRate);
	void ConfigureRuntimeDistanceFade(float VisibilityRadius,
		float VisibilityHeight);
	// UE1 ENetRole values a placed decoration's Actor.RemoteRole can take.
	static constexpr uint8 UE1RoleNone = 0;
	static constexpr uint8 UE1RoleDumbProxy = 1;
	// The network setup of a placed decoration, made once its static flag is
	// known and before it finishes spawning, the same on the server and on
	// every client. InRemoteRole and bInTemporary are the decoration's
	// Actor.RemoteRole and Actor.bNetTemporary, its class's own or the map's.
	void ConfigureRuntimeReplication(uint8 InRemoteRole, bool bInTemporary);
	bool IsRuntimeSprite() const { return bRuntimeSprite; }
	bool ConfigureRuntimeClassDefaultVisuals(
		FStringView ClassIdentity, FString& OutError);
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;
	float LastDamageTaken = 0.0f;

private:
	UFUNCTION() void OnCycleAnimEnd(FName Sequence);
	void ApplyRuntimeSpritePresentation();
	void ApplyRetailCollision();
	void InstallClassIgnores();
	void ApplyClassIgnore(AActor* Actor);
	int32 CycleIndex = 0;
	// Fish.uc rolls ONE AnimRate before its loop — not per link.
	float CycleRate = 1.0f;
	FWoTRuntimeActorVisualPresentation RuntimeVisualPresentation;
	TArray<FString> RuntimeSpriteTextureIdentities;
	float RuntimeSpriteTextureWidth = 0.0f;
	float RuntimeSpriteTextureHeight = 0.0f;
	float RuntimeSpriteMinFrameRate = 0.0f;
	float RuntimeSpriteMaxFrameRate = 0.0f;
	bool bHasRuntimeVisualPresentation = false;
	bool bRuntimeSprite = false;
	bool bRuntimeSpriteApplied = false;
	bool bHasCollisionDimensions = true;
	FRotator RuntimeRotationRate = FRotator::ZeroRotator;
	bool bRuntimeFixedRotation = false;
	bool bRuntimeDistanceFade = false;
	float RuntimeVisibilityRadius = 0.0f;
	float RuntimeVisibilityHeight = 0.0f;
	float RuntimeDistanceFadeBaseGlow = 1.0f;
	FDelegateHandle ActorSpawnedHandle;
};
