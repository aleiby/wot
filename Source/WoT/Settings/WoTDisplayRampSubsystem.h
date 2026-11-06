#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTDisplayRampSubsystem.generated.h"

/**
 * World-local projection of the retail Glide display ramp.  The current
 * exponent is process-wide settings state; each renderable world owns the MPC
 * instance receiving it.
 */
UCLASS()
class WOT_API UWoTDisplayRampSubsystem final : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Update existing render worlds; future worlds receive this in Initialize. */
	static void SetRampInvGamma(float RampInvGamma);

#if WITH_AUTOMATION_TESTS
	/** Headless editor specs have no viewport, so they exercise the MPC write directly. */
	static bool ApplyToWorldForTesting(UWorld* World, float RampInvGamma);
#endif

private:
	void Refresh();
	bool IsRenderWorld() const;
	static bool ApplyToWorld(UWorld* World, float RampInvGamma);

	uint64 AppliedRevision = 0;
};
