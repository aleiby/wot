#pragma once

#include "CoreMinimal.h"
#include "WoTEvents.h"
#include "WoTSoundDispatcher.generated.h"

class USoundBase;

// One SoundDispatcher.SoundEvent entry (SoundDispatcher.uc:26-33). Retail's
// fixed [16] becomes an importer-authored array; no sentinel clamp is needed.
USTRUCT()
struct FWoTSoundDispatcherEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") float Delay = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") FName Event;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") FName SoundSourceTag;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher")
	TSoftObjectPtr<USoundBase> Sound;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") FName SoundTableTag;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") FName SoundTableSlot;
};

// UE1 SoundDispatcher: an AWoTTrigger whose incoming tag starts a sequential,
// latent event/sound list. Touch-shaped instances use TagName == EventName so
// the inherited overlap fan-out routes back into WoTTrigger.
UCLASS()
class AWoTSoundDispatcher : public AWoTTrigger
{
	GENERATED_BODY()

public:
	AWoTSoundDispatcher();

	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher")
	TArray<FWoTSoundDispatcherEvent> EventList;

	// UE1 SoundSlot is intentionally omitted: UE5's PlayOneShot path has no
	// per-actor mixing slots. bSoundOverride is retained as authored parity data;
	// the replicated marker path has no override flag.
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") float SoundVolume = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") float SoundRadius = 2048.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") float SoundPitch = 1.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") bool bSoundOverride = false;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") int32 MaxDistance = 0;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") bool bFromSoundSource = false;
	UPROPERTY(EditAnywhere, Category = "WoT|SoundDispatcher") bool bTerminate = false;

	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	bool IsInstigatorOutOfRange() const;
	void ContinueDispatch();
	void ProcessCurrentEntry();
	void FinishDispatch();
	void StopDispatchSpent();
	void PlayCurrentSound(const FWoTSoundDispatcherEvent& Entry);

	TWeakObjectPtr<APawn> DispatchInstigator;
	TWeakObjectPtr<AActor> SoundSource;
	FTimerHandle DispatchTimer;
	int32 DispatchIndex = 0;
	bool bDispatching = false;
	bool bWarnedSoundTable = false;
};
