#pragma once

#include "CoreMinimal.h"
#include "WoTLeech.h"
#include "WoTTrapDetectLeech.generated.h"

// Port of Angreal.TrapDetectLeech (expands Leech) — the latent effect the Trap
// Detect ter'angreal attaches to the caster. Spec:
// exports/script/Angreal/TrapDetectLeech.uc.
//
// The owning client finds the most view-centered detectable trap within
// DetectRadius and tunes the heartbeat cadence by distance (closer = faster). Detectable
// = a DEPLOYED trap actor (AWoTTrap; Portcullis/Staircase only while Active) or an
// armed ExplosiveWard, whose GetTrapID is in the DetectableTrapTypes table (the
// TrapIconInfos[].TrapType projection. Heartbeat, icon, and two-pass sprayer are
// owner-only and use all six shipped table rows.
UCLASS()
class UWoTTrapDetectLeech : public UWoTLeech
{
	GENERATED_BODY()

public:
	UWoTTrapDetectLeech();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|TrapDetect") float DetectRadius = 1000.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|TrapDetect") float MinBeatTime = 0.3f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|TrapDetect") float MaxBeatTime = 3.0f;

	// The TrapIconInfos[].TrapType names (TrapDetectLeech.uc:286-291). GetTrapID must match one
	// for an actor to be detectable. The shipped names are UE1 CLASS names (no shipped trap sets
	// TrapType) — WallSlab, Pit, Portcullis, Spear, Staircase, AngrealExpWardProjectile.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|TrapDetect") TArray<FName> DetectableTrapTypes;

	// The trap we're currently indicating (null = none in range → no heartbeat).
	UPROPERTY() TObjectPtr<AActor> ClosestTrap;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void AffectHost(int32 Iterations = 0) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	// TrapDetectLeech.GetTrapID: a trap's TrapType if set, else the (UE1) class name. Our UE5
	// class names differ, so the trap ctors pre-set TrapType to the shipped fallback results; the
	// ward maps to its shipped class name explicitly.
	static FName GetTrapID(const AActor* Trap);

	int32 GetHeartbeatRequestCountForTest() const { return HeartbeatRequestCountForTest; }
	int32 GetTrapIconInfoCountForTest() const { return TrapIconInfos.Num(); }
	class AWoTWeaveSpriteEffect* GetTrapIconForTest() const { return TrapIcon.Get(); }
	class AWoTParticleSprayer* GetIndicatorForTest() const { return Indicator.Get(); }
#if WITH_AUTOMATION_TESTS
	AActor* GetClosestTrapForTest(float& TrapDistance, FVector& ClosestPoint) const
	{
		return GetClosestTrap(TrapDistance, ClosestPoint);
	}
#endif

private:
	struct FTrapIconInfo
	{
		FName TrapType;
		FName IconConfig;
		FName SprayerConfig;
		float IconScale = 0.3f;
	};
	TArray<FTrapIconInfo> TrapIconInfos;
	bool IsTrapRelevant(const AActor* Trap) const;
	bool ShouldRunClientOutput() const;
	AActor* GetClosestTrap(float& TrapDistance, FVector& ClosestPoint) const;
	FVector ClosestCollisionPoint(const AActor* Trap, const FVector& From) const;
	bool InitializeVisualsFor(AActor* Trap);
	void HideVisuals();
	void DestroyVisuals();
	void PlayHeartbeat();
	int32 HeartbeatRequestCountForTest = 0;
	float ClientBeatTimer = 0.0f;
	FName CurrentVisualTrapID;
	UPROPERTY() TObjectPtr<class AWoTWeaveSpriteEffect> TrapIcon;
	UPROPERTY() TObjectPtr<class AWoTParticleSprayer> Indicator;
};
