#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "WoTRetailProjectileTarget.h"
#include "WoTCollisionBlocker.generated.h"

class UBodySetup;

/** Asset-free 16-sided convex prism matching UE1's flat-ended cylinder. */
UCLASS(ClassGroup = Collision, meta = (DisplayName = "WoT Flat Cylinder Collision"))
class UWoTFlatCylinderComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UWoTFlatCylinderComponent();
	void SetCylinderSize(float InRadius, float InHalfHeight);
	void SetAnalyticCursorTrace(bool bEnabled);
	bool IsAnalyticCursorTrace() const { return bAnalyticCursorTrace; }
	bool ExactLineTrace(const FVector& Start, const FVector& End,
		FHitResult& OutHit) const;
	float GetUnscaledCapsuleRadius() const { return CylinderRadius; }
	float GetUnscaledCapsuleHalfHeight() const { return CylinderHalfHeight; }
	virtual UBodySetup* GetBodySetup() override;
	virtual FBoxSphereBounds CalcBounds(
		const FTransform& LocalToWorld) const override;
	virtual bool IsZeroExtent() const override;

private:
	UPROPERTY(Transient) TObjectPtr<UBodySetup> CylinderBodySetup;
	UPROPERTY() float CylinderRadius = 40.0f;
	UPROPERTY() float CylinderHalfHeight = 40.0f;
	UPROPERTY() bool bAnalyticCursorTrace = false;
};

UENUM()
enum class EWoTCollisionBlockerMode : uint8
{
	All,
	Players,
	Monsters,
	Actor,
};

/** Runtime representation of Engine.BlockAll/BlockPlayer and WOT.BlockMonsters. */
UCLASS()
class AWoTCollisionBlocker final : public AActor,
	public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTCollisionBlocker();
	void Configure(EWoTCollisionBlockerMode InMode, float Radius, float HalfHeight);
	void Configure(bool bInPlayerOnly, float Radius, float HalfHeight);
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual bool IsRetailProjectileTarget() const override { return false; }
	virtual bool IsRetailProjectileTouchTarget() const override
	{
		return Mode == EWoTCollisionBlockerMode::All
			|| Mode == EWoTCollisionBlockerMode::Actor;
	}

	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision")
	TObjectPtr<UWoTFlatCylinderComponent> Capsule;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision") bool bPlayerOnly = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision") bool bMonstersOnly = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Collision")
	EWoTCollisionBlockerMode Mode = EWoTCollisionBlockerMode::All;

private:
	void InstallClassIgnores();
	void ApplyClassIgnore(AActor* Actor);
	FDelegateHandle ActorSpawnedHandle;
};
