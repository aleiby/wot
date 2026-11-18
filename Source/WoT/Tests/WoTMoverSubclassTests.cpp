// Headless behavioral specs for the dependency-free mover subclass ports.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTAssertMover.h"
#include "../WoTLoopMover.h"
#include "../WoTRotatingMover.h"
#include "../WoTTimedMover.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

namespace
{
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRotatingMoverSpec,
	"WoT.Mover.Rotating", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRotatingMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTRotatingMover* Mover = World ? World->SpawnActorDeferred<AWoTRotatingMover>(
		AWoTRotatingMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("rotating mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->RotateRate = FRotator(0, 90, 0);
	Mover->FinishSpawning(FTransform::Identity);

	WoTTest::TickWorld(World, 15);
	TestTrue(TEXT("rotation tick starts disabled"), Mover->GetActorRotation().IsNearlyZero());
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 30);
	TestTrue(TEXT("trigger enables continuous rotation"),
		FMath::IsNearlyEqual(Mover->GetActorRotation().Yaw, 45.0f, 2.0f));
	Mover->WoTUnTrigger(nullptr, nullptr);
	const float StoppedYaw = Mover->GetActorRotation().Yaw;
	WoTTest::TickWorld(World, 15);
	TestTrue(TEXT("untrigger freezes the current rotation"),
		FMath::IsNearlyEqual(Mover->GetActorRotation().Yaw, StoppedYaw, 0.1f));

	const FProperty* State = FindFProperty<FProperty>(
		AWoTRotatingMover::StaticClass(), TEXT("bRotating"));
	TestTrue(TEXT("rotation activity replicates"), State && State->HasAnyPropertyFlags(CPF_Net));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLoopMoverSpec,
	"WoT.Mover.Loop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTLoopMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTLoopMover* Mover = World ? World->SpawnActorDeferred<AWoTLoopMover>(
		AWoTLoopMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("loop mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(200, 100, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
	                 FRotator::ZeroRotator};
	Mover->MoveTime = 0.1f;
	Mover->FinishSpawning(FTransform::Identity);
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 13);
	TestEqual(TEXT("the cycle reaches the last key"), Mover->GetCurrentKey(), 2);
	// The wrap must be a straight 2->0 leg. Walking key 1 would put Y near zero instead.
	TestTrue(TEXT("last-to-first interpolation is direct"), Mover->GetActorLocation().Y > 50.0f);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 8);
	TestFalse(TEXT("untrigger stops after the active leg finishes"), Mover->IsLooping());
	TestEqual(TEXT("the completed wrap stops at key zero"), Mover->GetCurrentKey(), 0);

	const FProperty* State = FindFProperty<FProperty>(
		AWoTLoopMover::StaticClass(), TEXT("LoopPhase"));
	TestTrue(TEXT("loop phase replicates"), State && State->HasAnyPropertyFlags(CPF_Net));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTimedMoverSpec,
	"WoT.Mover.Timed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTimedMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTimedMover* Mover = World ? World->SpawnActorDeferred<AWoTTimedMover>(
		AWoTTimedMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("timed mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(200, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
	                 FRotator::ZeroRotator};
	Mover->MoveTimes = {0.1f, 0.1f, 0.1f, 0, 0, 0, 0, 0};
	Mover->Delays = {0.05f, 0.05f, 0.05f, 0, 0, 0, 0, 0};
	Mover->bTriggerOnceOnly = true;
	Mover->FinishSpawning(FTransform::Identity);
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 24);
	TestTrue(TEXT("once-only sequence finishes idle"), Mover->IsSequenceIdle());
	TestEqual(TEXT("once-only sequence remains at its final key"), Mover->GetCurrentKey(), 2);
	TestTrue(TEXT("per-key move times carried it to key two"),
		FMath::IsNearlyEqual(Mover->GetActorLocation().X, 200.0f, 1.0f));

	const FProperty* State = FindFProperty<FProperty>(
		AWoTTimedMover::StaticClass(), TEXT("SequencePhase"));
	TestTrue(TEXT("timed sequence phase replicates"), State && State->HasAnyPropertyFlags(CPF_Net));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAssertMoverBaseStateSpec,
	"WoT.Mover.AssertBaseState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAssertMoverBaseStateSpec::RunTest(const FString&)
{
	// Battle2_02 AssertMover2 authors InitialState=TriggerOpenTimed: UE1 replaces the
	// AssertTriggerOpenTimed auto state COMPLETELY with the base TriggerOpenTimed state
	// (open, dwell StayOpenTime, close; NO UnTrigger response), while AssertMover.uc:38-56's
	// plain-function DoOpen/DoClose overrides keep selecting OpenTimes/CloseTimes — not
	// MoveTime — as the leg durations.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAssertMover* Mover = World ? World->SpawnActorDeferred<AWoTAssertMover>(
		AWoTAssertMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("base-state assert mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mover->InitialState = EWoTMoverState::TriggerOpenTimed;
	Mover->MoveTime = 10.0f;                       // must be IGNORED (DoOpen/DoClose win)
	Mover->OpenTimes = {0.2f, 0, 0, 0, 0, 0};
	Mover->CloseTimes = {0.1f, 0, 0, 0, 0, 0};
	Mover->StayOpenTime = 0.1f;
	Mover->WaitUnAssertTime = 5.0f;                // assert-FSM knob; must never engage
	Mover->FinishSpawning(FTransform::Identity);

	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 6);                      // 0.1s into the 0.2s OpenTimes[0] leg
	const float MidOpenX = Mover->GetActorLocation().X;
	TestTrue(TEXT("opening leg runs on OpenTimes[0], not MoveTime"),
		MidOpenX > 25.0f && MidOpenX < 75.0f);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 9);                      // ~0.25s total: fully open regardless
	TestTrue(TEXT("base TriggerOpenTimed ignores UnTrigger"),
		FMath::IsNearlyEqual(Mover->GetActorLocation().X, 100.0f, 1.0f));
	TestEqual(TEXT("assert FSM stays dormant under a base InitialState"),
		Mover->GetAssertPhase(), EWoTAssertMoverPhase::Idle);
	WoTTest::TickWorld(World, 15);                     // dwell 0.1s, then CloseTimes[0]=0.1s close
	TestTrue(TEXT("dwells StayOpenTime then closes on CloseTimes[0]"),
		Mover->GetActorLocation().IsNearlyZero(1.0f));
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 3);
	TestTrue(TEXT("not once-only: a re-trigger reopens from closed"),
		Mover->GetActorLocation().X > 5.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAssertMoverSpec,
	"WoT.Mover.Assert", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAssertMoverSpec::RunTest(const FString&)
{
	// Retail probe cadence: `Open:` disables Trigger until the terminal re-arm
	// (AssertMover.uc:92,130) and both the unassert wait and the top-key dwell
	// disable UnTrigger (AssertMover.uc:99,111) — so mid-cycle Trigger/UnTrigger
	// probes are IGNORED; only the re-armed closed mover accepts a Trigger.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAssertMover* Mover = World ? World->SpawnActorDeferred<AWoTAssertMover>(
		AWoTAssertMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("assert mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(200, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
	                 FRotator::ZeroRotator};
	Mover->OpenTimes = {0.1f, 0.1f, 0, 0, 0, 0};
	Mover->CloseTimes = {0.1f, 0.1f, 0, 0, 0, 0};
	Mover->WaitUnAssertTime = 0.15f;
	Mover->StayOpenTime = 0.2f;
	Mover->FinishSpawning(FTransform::Identity);

	// --- Unassert mid-open: the pending wait survives every mid-cycle probe. ---
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 3);                      // 0.05s into the 0.1s key-0 leg
	Mover->WoTUnTrigger(nullptr, nullptr);         // step out: wait after this leg
	Mover->WoTTrigger(nullptr, nullptr);           // re-assert mid-leg: Trigger disabled
	WoTTest::TickWorld(World, 5);                      // leg ends at 0.1s
	TestEqual(TEXT("re-trigger mid-leg does not cancel the pending unassert wait"),
		Mover->GetAssertPhase(), EWoTAssertMoverPhase::WaitingUnasserted);
	TestEqual(TEXT("the unassert wait holds at the reached key"), Mover->GetCurrentKey(), 1);
	Mover->WoTTrigger(nullptr, nullptr);           // re-assert during the wait: disabled
	WoTTest::TickWorld(World, 2);
	TestEqual(TEXT("re-trigger during the unassert wait does not reopen"),
		Mover->GetAssertPhase(), EWoTAssertMoverPhase::WaitingUnasserted);
	Mover->WoTUnTrigger(nullptr, nullptr);         // second unassert: UnTrigger disabled
	WoTTest::TickWorld(World, 3);                      // 0.117s of the 0.15s wait elapsed
	TestEqual(TEXT("a second untrigger does not shortcut the unassert wait"),
		Mover->GetAssertPhase(), EWoTAssertMoverPhase::WaitingUnasserted);
	WoTTest::TickWorld(World, 13);                     // wait ends at 0.15s, close leg 0.1s
	TestEqual(TEXT("the expired wait closes back to key zero"), Mover->GetCurrentKey(), 0);
	TestEqual(TEXT("repeatable mover re-arms at key zero"), Mover->GetAssertPhase(),
		EWoTAssertMoverPhase::Idle);
	TestTrue(TEXT("closed pose is key zero"), Mover->GetActorLocation().IsNearlyZero(1.0f));

	// --- Full open: the top-key dwell survives UnTrigger for its whole StayOpenTime. ---
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 14);                     // both 0.1s legs done at 0.2s -> dwell
	TestEqual(TEXT("full assertion dwells open at the top key"),
		Mover->GetAssertPhase(), EWoTAssertMoverPhase::DwellingOpen);
	Mover->WoTUnTrigger(nullptr, nullptr);         // step out during the dwell: disabled
	WoTTest::TickWorld(World, 2);
	TestEqual(TEXT("untrigger during the open dwell is ignored"),
		Mover->GetAssertPhase(), EWoTAssertMoverPhase::DwellingOpen);
	TestTrue(TEXT("the dwell holds the fully-open pose"),
		FMath::IsNearlyEqual(Mover->GetActorLocation().X, 200.0f, 1.0f));
	WoTTest::TickWorld(World, 28);                     // dwell ends at 0.2s, close 2 x 0.1s
	TestEqual(TEXT("unassertion closes fully through every key"), Mover->GetCurrentKey(), 0);
	TestEqual(TEXT("the full cycle re-arms at key zero"), Mover->GetAssertPhase(),
		EWoTAssertMoverPhase::Idle);
	Mover->WoTTrigger(nullptr, nullptr);           // probes re-enabled after the close
	WoTTest::TickWorld(World, 3);
	TestTrue(TEXT("the re-armed mover reopens on a fresh trigger"),
		Mover->GetActorLocation().X > 5.0f);

	const FProperty* State = FindFProperty<FProperty>(
		AWoTAssertMover::StaticClass(), TEXT("AssertPhase"));
	TestTrue(TEXT("assert phase replicates"), State && State->HasAnyPropertyFlags(CPF_Net));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTimedMoverSoundEdgesSpec,
	"WoT.TimedMover.SoundEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTTimedMoverSoundEdgesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTimedMover* Mover = World ? World->SpawnActorDeferred<AWoTTimedMover>(
		AWoTTimedMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("timed mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0), FVector(200, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator,
		FRotator::ZeroRotator};
	Mover->MoveTimes = {0.05f, 0.05f, 0.05f, 0, 0, 0, 0, 0};
	Mover->Delays = {0.0f, 0.2f, 0.0f, 0, 0, 0, 0, 0};
	Mover->Sounds = {FName(TEXT("Spec/ReverseStep")), FName(TEXT("Spec/ForwardStep")),
		FName(TEXT("Spec/TopStep")), NAME_None, NAME_None, NAME_None, NAME_None, NAME_None};
	Mover->OpeningSoundPath = TEXT("Spec/Opening");
	Mover->OpenedSoundPath = TEXT("Spec/Opened");
	Mover->ClosingSoundPath = TEXT("Spec/Closing");
	Mover->ClosedSoundPath = TEXT("Spec/Closed");
	Mover->FinishSpawning(FTransform::Identity);
	const int32 InitialEdgeCount = Mover->PhaseSoundPlayCount;
	Mover->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("forward sequence has one opening edge"), Mover->LastPhaseSound,
		FName(TEXT("Spec/Opening")));
	TestTrue(TEXT("per-key ambient is active while moving"), Mover->bMoveLoopOn);
	TestEqual(TEXT("sequence start plays exactly one Opening edge"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 1);
	WoTTest::TickWorld(World, 4);
	TestFalse(TEXT("per-key dwell is silent"), Mover->bMoveLoopOn);
	TestEqual(TEXT("next authored key does not replay Opening"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 1);
	WoTTest::TickWorld(World, 12);
	TestEqual(TEXT("second forward key still has no extra Opening edge"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 1);
	WoTTest::TickWorld(World, 4);
	TestEqual(TEXT("full-open edge precedes backward sequence edge; closing wins last"),
		Mover->LastPhaseSound, FName(TEXT("Spec/Closing")));
	TestEqual(TEXT("full open plays one Opened and one Closing edge"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 3);
	TestTrue(TEXT("backward per-key ambient is active"), Mover->bMoveLoopOn);
	WoTTest::TickWorld(World, 4);
	TestEqual(TEXT("backward key changes do not replay Closing"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 3);
	WoTTest::TickWorld(World, 13);
	TestEqual(TEXT("sequence emits closed edge at key zero"), Mover->LastPhaseSound,
		FName(TEXT("Spec/Closed")));
	TestEqual(TEXT("sequence close plays exactly one Closed edge"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 4);
	TestFalse(TEXT("idle timed mover is silent"), Mover->bMoveLoopOn);
	Mover->ClosingSoundPath = NAME_None;
	const int32 ReplayStartCount = Mover->PhaseSoundPlayCount;
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 20);
	TestEqual(TEXT("FinishedOpening emits its full-open edge before reverse starts"),
		Mover->LastPhaseSound, FName(TEXT("Spec/Opened")));
	TestEqual(TEXT("replay has one Opening and one full-open Opened edge"),
		Mover->PhaseSoundPlayCount, ReplayStartCount + 2);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAssertMoverSoundEdgesSpec,
	"WoT.AssertMover.SoundEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAssertMoverSoundEdgesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAssertMover* Mover = World ? World->SpawnActorDeferred<AWoTAssertMover>(
		AWoTAssertMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("assert mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mover->OpenTimes = {0.1f, 0, 0, 0, 0, 0};
	Mover->CloseTimes = {0.1f, 0, 0, 0, 0, 0};
	Mover->WaitUnAssertTime = 0.1f;
	Mover->OpeningSoundPath = TEXT("Spec/Opening");
	Mover->OpenedSoundPath = TEXT("Spec/Opened");
	Mover->ClosingSoundPath = TEXT("Spec/Closing");
	Mover->ClosedSoundPath = TEXT("Spec/Closed");
	Mover->MoveAmbientSoundPath = TEXT("Spec/Loop");
	Mover->FinishSpawning(FTransform::Identity);
	const int32 InitialEdgeCount = Mover->PhaseSoundPlayCount;
	Mover->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("opening leg edge"), Mover->LastPhaseSound,
		FName(TEXT("Spec/Opening")));
	TestEqual(TEXT("opening leg plays one edge"), Mover->PhaseSoundPlayCount,
		InitialEdgeCount + 1);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("Wait label emits its single opened edge"),
		Mover->LastPhaseSound, FName(TEXT("Spec/Opened")));
	TestEqual(TEXT("unassert Wait transition plays exactly one Opened edge"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 2);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("wait expiry starts closing leg"), Mover->LastPhaseSound,
		FName(TEXT("Spec/Closing")));
	TestEqual(TEXT("closing leg plays one edge"), Mover->PhaseSoundPlayCount,
		InitialEdgeCount + 3);
	WoTTest::TickWorld(World, 7);
	TestEqual(TEXT("closing leg emits closed edge"), Mover->LastPhaseSound,
		FName(TEXT("Spec/Closed")));
	TestEqual(TEXT("closed leg plays one edge"), Mover->PhaseSoundPlayCount,
		InitialEdgeCount + 4);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLoopMoverSoundEdgesSpec,
	"WoT.LoopMover.SoundEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTLoopMoverSoundEdgesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTLoopMover* Mover = World ? World->SpawnActorDeferred<AWoTLoopMover>(
		AWoTLoopMover::StaticClass(), FTransform::Identity) : nullptr;
	if (!TestNotNull(TEXT("loop mover"), Mover))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mover->KeyPos = {FVector::ZeroVector, FVector(100, 0, 0)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mover->MoveTime = 0.05f;
	Mover->OpeningSoundPath = TEXT("Spec/Opening");
	Mover->OpenedSoundPath = TEXT("Spec/Opened");
	Mover->MoveAmbientSoundPath = TEXT("Spec/Loop");
	Mover->FinishSpawning(FTransform::Identity);
	const int32 InitialEdgeCount = Mover->PhaseSoundPlayCount;
	Mover->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("each loop leg starts with opening edge"), Mover->LastPhaseSound,
		FName(TEXT("Spec/Opening")));
	TestTrue(TEXT("loop ambient starts"), Mover->bMoveLoopOn);
	TestEqual(TEXT("first loop leg plays one Opening edge"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 1);
	WoTTest::TickWorld(World, 4);
	TestEqual(TEXT("completed leg starts the next leg with Opening"),
		Mover->LastPhaseSound, FName(TEXT("Spec/Opening")));
	TestEqual(TEXT("each continued loop leg plays Opened plus Opening"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 3);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 4);
	TestFalse(TEXT("untrigger parks after the active leg"), Mover->IsLooping());
	TestEqual(TEXT("InactiveState emits one final opened edge"),
		Mover->LastPhaseSound, FName(TEXT("Spec/Opened")));
	TestEqual(TEXT("parking leg plays one final Opened and no Opening"),
		Mover->PhaseSoundPlayCount, InitialEdgeCount + 4);
	TestTrue(TEXT("empty InterpolateEnd keeps ambient audible at rest"),
		Mover->bMoveLoopOn);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
