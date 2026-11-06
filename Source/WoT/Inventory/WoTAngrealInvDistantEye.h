#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvDistantEye.generated.h"

// Port of Angreal.AngrealInvDistantEye. Cast phase one fixes a shootable eye;
// phase two enters its view. The shipped toggle-off branch is commented out,
// so every exit funnels through movement/death/drop/reset instead.
UCLASS()
class AWoTAngrealInvDistantEye : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvDistantEye();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	FVector SpawnOffset = FVector(48.0f, 0.0f, 96.0f);
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	float MovementTolerance = 10.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|DistantEye")
	TSubclassOf<class AWoTDistantEyeProjectile> EyeClass;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds")
	TSoftObjectPtr<USoundBase> DropSound;
	// Literal retail display defaults, retained for native CDO parity even
	// though the generated pickup Blueprint also applies this mesh scale.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display")
	float PickupViewScale = 0.7f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display")
	float DrawScale = 0.7f;

	AWoTDistantEyeProjectile* GetEye() const { return Eye.Get(); }
	bool IsViewing() const { return bActive; }
	void NotifyEyeDestroyed(AWoTDistantEyeProjectile* DestroyedEye);

	virtual void CastArtifact() override;
	virtual void UnCastArtifact() override;
	virtual void Reset() override;
	virtual void NotifyPutInBag(class AWoTBagHolding* Bag) override;
	virtual void DropFrom(FVector StartLocation) override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void ExitViewing();
	void DestroyEye();

	UPROPERTY() TObjectPtr<AWoTDistantEyeProjectile> Eye;
	FVector InitialOwnerLocation = FVector::ZeroVector;
};
