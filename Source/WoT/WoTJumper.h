#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTUE1Collision.h"
#include "WoTJumper.generated.h"

/** Native port of WOT.Jumper: an authority-only directional pawn launcher. */
UCLASS()
class AWoTJumper final : public AActor
{
	GENERATED_BODY()

public:
	AWoTJumper();
	virtual void BeginPlay() override;

	UPROPERTY(EditAnywhere, Category = "WoT|Jumper") bool bOnceOnly = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Jumper") float JumpZ = 0.0f;
	/** Retail class identity (only WOT.WOTPawn and WOT.Grunt ship in MP maps). */
	UPROPERTY(EditAnywhere, Category = "WoT|Jumper") FName LimitedToClassIdentity;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Jumper")
	TObjectPtr<class UCapsuleComponent> Capsule;
	void SetUE1CollisionSize(float Radius, float HalfHeight);

	UFUNCTION()
	void OnOverlap(UPrimitiveComponent* Component, AActor* Other,
		UPrimitiveComponent* OtherComponent, int32 BodyIndex,
		bool bFromSweep, const FHitResult& Hit);

private:
	bool AcceptsPawn(const APawn* Pawn) const;
	void LaunchPending();

	TWeakObjectPtr<APawn> Pending;
	FTimerHandle LaunchTimer;
	float UE1CollisionRadius = 40.0f;
	float UE1CollisionHalfHeight = 40.0f;
	WoTUE1Collision::FTouchTracker UE1Touches;
};
