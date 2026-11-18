#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "WoTAngrealInvLevitate.generated.h"

// Port of Angreal.AngrealInvLevitate — the Levitate ter'angreal. Spec:
// exports/script/Angreal/AngrealInvLevitate.uc. First NON-launcher artifact: a
// sustained self-cast that expands AngrealInventory directly (no projectile).
//
// While held down it floats the caster RiseHeight above whatever ground is below
// (MOVE_Flying + a per-tick downward trace that drives vertical velocity toward
// the hover height at LevitateSpeed) and drains one charge per 60/RoundsPerMinute
// seconds; running out (UseCharge -> GoEmpty) or releasing stops it and restores
// the caster's movement mode. "This is not flight" — horizontal input is
// unchanged; only the hover height is forced.
//
// LoopLT is a controlled ambient for exactly the held cast and stops before
// DeActivateLT; the shipped class deliberately has no light or visual actor.
UCLASS()
class AWoTAngrealInvLevitate : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvLevitate();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Levitate") float RiseHeight = 24.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Levitate") float LevitateSpeed = 96.0f;

	virtual void CastArtifact() override;     // Cast()
	virtual void UnCastArtifact() override;   // UnCast()
	virtual void GoEmpty() override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnRep_Owner() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	class UWoTAmbientSoundComponent* GetLevitateLoopForTest() const
	{
		return LevitateLoop.Get();
	}
#if WITH_AUTOMATION_TESTS
	void SetLevitatingForTest(bool bInLevitating) { bLevitating = bInLevitating; }
#endif

private:
	// AngrealInvLevitate.uc:126 plays LoopLT through the owner. Replicating
	// this state lets the remote owning client drive that ambient locally.
	UPROPERTY(ReplicatedUsing = OnRep_Levitating) bool bLevitating = false;
	UFUNCTION() void OnRep_Levitating();
	void ReconcileLevitatingLoop();
	TEnumAsByte<EMovementMode> InitialMovementMode = MOVE_Walking;
	uint8 InitialCustomMovementMode = 0;
	float ChargeTimer = 0.0f;
	UPROPERTY() TObjectPtr<class UWoTAmbientSoundComponent> LevitateLoop;
};
