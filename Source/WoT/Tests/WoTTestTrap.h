#pragma once

#include "CoreMinimal.h"
#include "../Traps/WoTTrap.h"
#include "../WoTEvents.h"
#include "Engine/World.h"
#include "WoTTestTrap.generated.h"

// Concrete trap for the AWoTTrap base spec (the base is Abstract). Records DamageActor / TrapTouch
// dispatch (the latter for the TrapBlocker relay spec).
UCLASS()
class AWoTTestTrap : public AWoTTrap
{
	GENERATED_BODY()

public:
	int32 DamageActorCalls = 0;
	int32 TrapTouchCalls = 0;
	virtual void DamageActor(AActor* Other) override { ++DamageActorCalls; }
	virtual void TrapTouch(AActor* Other) override { ++TrapTouchCalls; }
};

// A slab-style child whose GetBaseResource returns its Owner (mirrors WallSlab.GetBaseResource),
// so two slabs sharing a manager read as the SameLogicalActor.
UCLASS()
class AWoTTestTrapSlab : public AWoTTrap
{
	GENERATED_BODY()

public:
	virtual AActor* GetBaseResource() override { return GetOwner() ? GetOwner() : this; }
};

// Registers its tag with UWoTEventSubsystem in BeginPlay and counts WoTTrigger fan-outs, so the
// spec can prove AWoTTrap::DestroyedTrigger fired DestroyedEvent through the subsystem.
UCLASS()
class AWoTTestTrapEventSink : public AActor, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	UPROPERTY() FName TagName;
	int32 TriggerCount = 0;

	// Defined inline, like every other method here. UHT emits this class's
	// vtable unconditionally, so an out-of-line definition living inside
	// `#if WITH_AUTOMATION_TESTS` in the .cpp left the vtable referencing a
	// symbol that does not exist in Test/Shipping, and the monolithic link
	// failed. Keep declaration and definition inseparable.
	virtual void BeginPlay() override
	{
		Super::BeginPlay();
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->RegisterTag(TagName, this);
		}
	}
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override { ++TriggerCount; }
	virtual FName GetUE1Tag() const override { return TagName; }
};
