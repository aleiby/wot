#include "WoTSoundDispatcher.h"

#include "WoTSoundTable.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"

AWoTSoundDispatcher::AWoTSoundDispatcher()
{
	// Pin the inherited Engine.Trigger retail default explicitly for this
	// dispatcher's self-looping touch path.
	TriggerType = EWoTTriggerType::PlayerProximity;
}

void AWoTSoundDispatcher::WoTTrigger(AActor*, APawn* EventInstigator)
{
	// SoundDispatcher state code is authority-only. The latch models retail's
	// Disable('Touch') + Disable('Trigger') at Dispatch, but gates this override
	// only: AWoTTrigger's touch fan-out remains live so TagName == EventName can
	// self-route into this method.
	if (!HasAuthority() || !bTriggerEnabled || bDispatching)
	{
		return;
	}

	SetInstigator(EventInstigator);
	DispatchInstigator = EventInstigator;
	DispatchIndex = 0;
	bDispatching = true;
	ContinueDispatch();
}

void AWoTSoundDispatcher::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DispatchTimer);
	}
	Super::EndPlay(EndPlayReason);
}

bool AWoTSoundDispatcher::IsInstigatorOutOfRange() const
{
	const APawn* EventInstigator = DispatchInstigator.Get();
	if (!EventInstigator || MaxDistance == 0)
	{
		return false;
	}
	const AActor* Reference = bFromSoundSource && SoundSource.IsValid()
		? SoundSource.Get() : this;
	return FVector::Dist(Reference->GetActorLocation(),
		EventInstigator->GetActorLocation()) > static_cast<float>(MaxDistance);
}

void AWoTSoundDispatcher::ContinueDispatch()
{
	if (!bDispatching)
	{
		return;
	}

	while (EventList.IsValidIndex(DispatchIndex))
	{
		if (IsInstigatorOutOfRange())
		{
			if (bTerminate)
			{
				StopDispatchSpent();
				return;
			}
			GetWorld()->GetTimerManager().SetTimer(
				DispatchTimer, this, &AWoTSoundDispatcher::ContinueDispatch, 0.1f, false);
			return;
		}

		const FWoTSoundDispatcherEvent& Entry = EventList[DispatchIndex];
		if (Entry.Event.IsNone() && Entry.SoundSourceTag.IsNone())
		{
			// SoundDispatcher.uc:92 gates Sleep as well as the work: a blank
			// fixed-array slot consumes no authored delay.
			++DispatchIndex;
			continue;
		}
		if (Entry.Delay > 0.0f)
		{
			GetWorld()->GetTimerManager().SetTimer(
				DispatchTimer, this, &AWoTSoundDispatcher::ProcessCurrentEntry,
				Entry.Delay, false);
			return;
		}
		ProcessCurrentEntry();
		return;
	}

	FinishDispatch();
}

void AWoTSoundDispatcher::ProcessCurrentEntry()
{
	if (!bDispatching || !EventList.IsValidIndex(DispatchIndex))
	{
		return;
	}
	const FWoTSoundDispatcherEvent& Entry = EventList[DispatchIndex];
	if (!Entry.Event.IsNone())
	{
		GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
			Entry.Event, this, DispatchInstigator.Get());
	}
	if (!Entry.SoundSourceTag.IsNone())
	{
		SoundSource = GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FindFirstTagged(
			Entry.SoundSourceTag);
		PlayCurrentSound(Entry);
	}
	++DispatchIndex;
	ContinueDispatch();
}

void AWoTSoundDispatcher::PlayCurrentSound(
	const FWoTSoundDispatcherEvent& Entry)
{
	AActor* Source = SoundSource.Get();
	if (!Source)
	{
		return;
	}
	if (!Entry.Sound.IsNull())
	{
		FString PackagePath = FPackageName::ObjectPathToPackageName(
			Entry.Sound.ToSoftObjectPath().ToString());
		constexpr const TCHAR* SoundRoot = TEXT("/Game/WoT/Sounds/");
		if (PackagePath.RemoveFromStart(SoundRoot))
		{
			// UE1 PlaySound replicated natively. The module's replicated burst
			// preserves that behavior and applies the shared UE1 radius semantics.
			WoTSound::PlayReplicated(*GetWorld(), FName(*PackagePath),
				Source->GetActorLocation(), SoundRadius, SoundVolume, SoundPitch);
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("SoundDispatcher %s: sound path is outside /Game/WoT/Sounds: %s"),
				*GetName(), *Entry.Sound.ToSoftObjectPath().ToString());
		}
		return;
	}

	if (!Entry.SoundTableTag.IsNone())
	{
		// Rider: no MP SoundDispatcher authors this leg. The SoundTableInterf
		// actor search/slot bridge remains intentionally unported for this wave.
		if (!bWarnedSoundTable)
		{
			bWarnedSoundTable = true;
			UE_LOG(LogTemp, Warning,
				TEXT("SoundDispatcher %s: SoundTableTag '%s' is not ported"),
				*GetName(), *Entry.SoundTableTag.ToString());
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("SoundDispatcher %s entry %d has neither Sound nor SoundTableTag"),
			*GetName(), DispatchIndex);
	}
}

void AWoTSoundDispatcher::FinishDispatch()
{
	SoundSource.Reset();
	DispatchInstigator.Reset();
	DispatchIndex = 0;
	bDispatching = false;
}

void AWoTSoundDispatcher::StopDispatchSpent()
{
	// Retail Stop leaves both probes disabled forever. Keep the latch set and
	// turn off the inherited collision volume so the dispatcher is spent.
	GetWorld()->GetTimerManager().ClearTimer(DispatchTimer);
	if (Capsule)
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}
