#include "WoTStageTrigger.h"

#include "WoTElevatorMover.h"

#include "EngineUtils.h"

AWoTStageTrigger::AWoTStageTrigger()
{
	WaterEvents.Init(NAME_None, 8);
	DispatcherTags.Init(NAME_None, 8);
	Dispatchers.SetNum(8);
}

void AWoTStageTrigger::BeginPlay()
{
	Super::BeginPlay();
	for (int32 Index = 0; Index < DispatcherTags.Num(); ++Index)
	{
		Dispatchers[Index].Reset();
		if (DispatcherTags[Index].IsNone())
		{
			continue;
		}
		for (TActorIterator<AWoTDispatcher> It(GetWorld()); It; ++It)
		{
			if (It->TagName == DispatcherTags[Index])
			{
				Dispatchers[Index] = *It;
				break;
			}
		}
	}
}

void AWoTStageTrigger::EnableTriggers(bool bEnable)
{
	if (!HasAuthority())
	{
		return;
	}
	for (const TWeakObjectPtr<AWoTDispatcher>& Dispatcher : Dispatchers)
	{
		if (Dispatcher.IsValid())
		{
			Dispatcher->SetTriggerEnabled(bEnable);
		}
	}
}

void AWoTStageTrigger::ChangeStage(AActor*, int32 Delta)
{
	if (!HasAuthority())
	{
		return;
	}
	if (!EventName.IsNone())
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
			EventName, this, nullptr);
	}
	CurrentStage = FMath::Clamp(CurrentStage + Delta, 0,
		FMath::Max(0, MaxKeyFrame));
	for (TActorIterator<AWoTElevatorMover> It(GetWorld()); It; ++It)
	{
		if (It->TagName == EventName)
		{
			It->MoveKeyframe(CurrentStage, 1.0f);
		}
	}
	// WaterEvents are intentionally dormant until the TriggerWaterZone seam
	// exists; the shipped SP-only data has no MP placement.
}
