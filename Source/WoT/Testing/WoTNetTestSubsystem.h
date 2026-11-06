#pragma once

// Glue subsystem for the net-test harness. Owns the TCP
// command channel, pumps it each tick, applies held movement, and (optionally)
// appends periodic snapshot probes to a log file. Only created when the process
// is launched with -wotnettest (and never in Shipping). LAN-only, token-gated;
// zero gameplay-behavior change.

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "WoTNetTestSubsystem.generated.h"

UCLASS()
class UWoTNetTestSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	bool Tick(float DeltaTime);

	// Raw pointer (not TUniquePtr): a forward-declared TUniquePtr member in a
	// UObject header trips UHT's generated FVTableHelper ctor over the incomplete
	// type. new/delete happen in Initialize/Deinitialize where the type is full.
	class FWoTNetTestChannel* Channel = nullptr;
	FTSTicker::FDelegateHandle TickHandle;
	FString ProbeLogPath;
	double LastProbeTime = 0.0;
#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
};
