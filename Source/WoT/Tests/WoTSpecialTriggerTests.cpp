#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestSoundDispatcher.h"
#include "../WoTCharacter.h"
#include "../WoTEvents.h"
#include "../WoTMover.h"
#include "../WoTSpecialTriggers.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTStaircase.h"

#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
constexpr EAutomationTestFlags WoTSpecialTriggerFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters SpecialTriggerAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AWoTTestSoundDispatcherEventSink* SpawnEventSink(UWorld* World, FName Tag)
{
	AWoTTestSoundDispatcherEventSink* Sink =
		World->SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
			AWoTTestSoundDispatcherEventSink::StaticClass(), FTransform::Identity);
	Sink->TagName = Tag;
	Sink->FinishSpawning(FTransform::Identity);
	return Sink;
}

AWoTTriggerLight* SpawnTriggerLight(UWorld* World, EWoTTriggerLightMode Mode,
	bool bInitiallyOn = false)
{
	AWoTTriggerLight* Light = World->SpawnActorDeferred<AWoTTriggerLight>(
		AWoTTriggerLight::StaticClass(), FTransform::Identity);
	Light->Mode = Mode;
	Light->bInitiallyOn = bInitiallyOn;
	Light->ChangeTime = 0.1f;
	Light->RemainOnTime = 0.2f;
	Light->FullIntensity = 8.0f;
	Light->FinishSpawning(FTransform::Identity);
	return Light;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSpecialEventRuntimeSpec,
	"WoT.World.Projector.Runtime.SpecialEvents", WoTSpecialTriggerFlags)

bool FWoTSpecialEventRuntimeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestSoundDispatcherEventSink* Sink =
		SpawnEventSink(World, TEXT("ProjectedSpecialOut"));
	AWoTSpecialEventTrigger* Trigger =
		World->SpawnActorDeferred<AWoTSpecialEventTrigger>(
			AWoTSpecialEventTrigger::StaticClass(), FTransform::Identity);
	Trigger->EventName = TEXT("ProjectedSpecialOut");
	Trigger->TriggerType = EWoTTriggerType::PawnProximity;
	Trigger->RepeatTriggerTime = 0.02f;
	Trigger->SetUE1CollisionSize(150.0f, 32.0f);
	Trigger->FinishSpawning(FTransform::Identity);
	TestTrue(TEXT("projected-style trigger reached BeginPlay"),
		Trigger->HasActorBegunPlay());

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator,
		SpecialTriggerAlwaysSpawn());
	Trigger->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	TestEqual(TEXT("pawn one floor above 150x32 trigger is filtered"),
		Sink->TriggerCount, 0);
	Pawn->SetActorLocation(FVector(0, 0, -100), false);
	Trigger->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	TestEqual(TEXT("pawn one floor below 150x32 trigger is filtered"),
		Sink->TriggerCount, 0);
	Pawn->SetActorLocation(FVector::ZeroVector, false);
	if (Sink->TriggerCount == 0)
	{
		Trigger->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	}
	TestTrue(TEXT("pawn inside authored cylinder triggers"), Sink->TriggerCount >= 1);
	TestEqual(TEXT("event preserves Other payload"), Sink->LastOther.Get(),
		static_cast<AActor*>(Pawn));
	const int32 BeforeRepeat = Sink->TriggerCount;
	WoTTest::TickWorld(World, 0.05f, 0.005f);
	TestTrue(TEXT("held overlap repeats"), Sink->TriggerCount > BeforeRepeat);

	AWoTSpecialEvent* Damage = World->SpawnActorDeferred<AWoTSpecialEvent>(
		AWoTSpecialEvent::StaticClass(), FTransform::Identity);
	Damage->TagName = TEXT("ProjectedDamage");
	Damage->Config.Action = EWoTSpecialEventAction::DamageInstigator;
	Damage->Config.Damage = 25;
	Damage->FinishSpawning(FTransform::Identity);
	const int32 InitialHealth = Pawn->Health;
	World->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		TEXT("ProjectedDamage"), Pawn, Pawn);
	TestEqual(TEXT("SpecialEvent damages Other"), Pawn->Health,
		InitialHealth - 25);
	Damage->Config.Damage = -10;
	Damage->WoTTrigger(Pawn, Pawn);
	TestEqual(TEXT("SpecialEvent heals with negative damage"), Pawn->Health,
		InitialHealth - 15);

	AWoTSpecialEventTrigger* Kill =
		World->SpawnActorDeferred<AWoTSpecialEventTrigger>(
			AWoTSpecialEventTrigger::StaticClass(), FTransform::Identity);
	Kill->Config.Action = EWoTSpecialEventAction::KillInstigator;
	Kill->Config.DamageType = TEXT("Fell");
	Kill->FinishSpawning(FTransform::Identity);
	Kill->WoTTrigger(Pawn, Pawn);
	TestTrue(TEXT("KillInstigator calls unconditional Died"), Pawn->bDead);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRandomAndLightRuntimeSpec,
	"WoT.World.Projector.Runtime.RandomAndTriggerLight", WoTSpecialTriggerFlags)

bool FWoTRandomAndLightRuntimeSpec::RunTest(const FString&)
{
	FMath::RandInit(0x6202);
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Activator = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 0), FRotator::ZeroRotator,
		SpecialTriggerAlwaysSpawn());
	AWoTTestSoundDispatcherEventSink* LocalSink =
		SpawnEventSink(World, TEXT("LocalRandomOut"));
	AWoTRandomEventTrigger* Local =
		World->SpawnActorDeferred<AWoTRandomEventTrigger>(
			AWoTRandomEventTrigger::StaticClass(), FTransform::Identity);
	Local->Events = {FName(TEXT("LocalRandomOut"))};
	Local->TriggerProbability = 1.0f;
	Local->MinRecheckTime = Local->MaxRecheckTime = 0.01f;
	Local->bLocalOnly = true;
	Local->FinishSpawning(FTransform::Identity);
	Local->WoTTrigger(Local, Activator);
	WoTTest::TickWorld(World, 0.03f, 0.005f);
	TestTrue(TEXT("SpecialEffectsTrigger timer dispatches"),
		LocalSink->TriggerCount > 0);
	TestEqual(TEXT("first TriggeredActive dispatch passes triggerInstigator"),
		LocalSink->LastOther.Get(), static_cast<AActor*>(Activator));

	AWoTTestSoundDispatcherEventSink* AuthoritySink =
		SpawnEventSink(World, TEXT("AuthorityRandomOut"));
	AWoTRandomEventTrigger* Stochastic =
		World->SpawnActorDeferred<AWoTRandomEventTrigger>(
			AWoTRandomEventTrigger::StaticClass(), FTransform::Identity);
	Stochastic->Events = {FName(TEXT("AuthorityRandomOut"))};
	Stochastic->TriggerProbability = 1.0f;
	Stochastic->MinRecheckTime = Stochastic->MaxRecheckTime = 0.01f;
	Stochastic->bInitiallyActive = true;
	Stochastic->FinishSpawning(FTransform::Identity);
	WoTTest::TickWorld(World, 0.03f, 0.005f);
	TestTrue(TEXT("StochasticTrigger deterministic timer dispatches"),
		AuthoritySink->TriggerCount > 0);
	TestNull(TEXT("AlwaysActive dispatch preserves None Other"),
		AuthoritySink->LastOther.Get());

	AWoTTriggerLight* TurnsOn = SpawnTriggerLight(
		World, EWoTTriggerLightMode::TurnsOn);
	TurnsOn->WoTTrigger(nullptr, nullptr);
	TurnsOn->Tick(0.2f);
	TestEqual(TEXT("TriggerTurnsOn fades to full"),
		TurnsOn->Light->Intensity, 8.0f, 0.01f);
	AWoTTriggerLight* TurnsOff = SpawnTriggerLight(
		World, EWoTTriggerLightMode::TurnsOff, true);
	TurnsOff->WoTTrigger(nullptr, nullptr);
	TurnsOff->Tick(0.2f);
	TestEqual(TEXT("TriggerTurnsOff fades dark"),
		TurnsOff->Light->Intensity, 0.0f, 0.01f);
	AWoTTriggerLight* Toggle = SpawnTriggerLight(
		World, EWoTTriggerLightMode::Toggle);
	Toggle->WoTTrigger(nullptr, nullptr);
	Toggle->Tick(0.2f);
	TestEqual(TEXT("TriggerToggle reverses target"),
		Toggle->Light->Intensity, 8.0f, 0.01f);
	AWoTTriggerLight* Control = SpawnTriggerLight(
		World, EWoTTriggerLightMode::Control);
	Control->WoTTrigger(nullptr, nullptr);
	Control->Tick(0.2f);
	TestEqual(TEXT("TriggerControl trigger turns on"),
		Control->Light->Intensity, 8.0f, 0.01f);
	Control->WoTUnTrigger(nullptr, nullptr);
	Control->Tick(0.2f);
	TestEqual(TEXT("TriggerControl untrigger restores"),
		Control->Light->Intensity, 0.0f, 0.01f);
	AWoTTriggerLight* Pound = SpawnTriggerLight(
		World, EWoTTriggerLightMode::Pound);
	Pound->WoTTrigger(nullptr, nullptr);
	Pound->Tick(0.2f);
	TestEqual(TEXT("TriggerPound begins on"),
		Pound->Light->Intensity, 8.0f, 0.01f);
	WoTTest::TickWorld(World, 0.12f, 0.01f);
	Pound->Tick(0.2f);
	TestEqual(TEXT("TriggerPound reverses"),
		Pound->Light->Intensity, 0.0f, 0.01f);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTrapProjectionRuntimeSpec,
	"WoT.World.Projector.Runtime.TrapResolutionAndPitClaim", WoTSpecialTriggerFlags)

bool FWoTTrapProjectionRuntimeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* StairMover = World->SpawnActorDeferred<AWoTMover>(
		AWoTMover::StaticClass(), FTransform::Identity);
	StairMover->TagName = TEXT("ProjectedStairMover");
	StairMover->InitialState = EWoTMoverState::TriggerToggle;
	StairMover->FinishSpawning(FTransform::Identity);
	AWoTStaircase* Stair = World->SpawnActorDeferred<AWoTStaircase>(
		AWoTStaircase::StaticClass(), FTransform::Identity);
	Stair->MoverTag = TEXT("ProjectedStairMover");
	Stair->Team = 0;
	Stair->SetTriggerSize(150.0f, 32.0f);
	Stair->FinishSpawning(FTransform::Identity);
	TestEqual(TEXT("staircase resolves mover by tag and sets dwell"),
		StairMover->StayOpenTime, 6.0f);
	Stair->Activate();
	AWoTCharacter* StairPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator,
		SpecialTriggerAlwaysSpawn());
	const EWoTMoverPhase BeforeStair = StairMover->GetMoverPhase();
	Stair->OnStaircaseOverlap(nullptr, StairPawn, nullptr, 0, false, FHitResult());
	TestEqual(TEXT("pawn one floor above 150x32 staircase is filtered"),
		StairMover->GetMoverPhase(), BeforeStair);
	StairPawn->SetActorLocation(FVector::ZeroVector, false);
	Stair->OnStaircaseOverlap(nullptr, StairPawn, nullptr, 0, false, FHitResult());
	TestTrue(TEXT("pawn inside 150x32 staircase triggers mover"),
		StairMover->GetMoverPhase() != BeforeStair);
	StairPawn->Destroy();

	AWoTPortcullisMover* GateMover =
		World->SpawnActorDeferred<AWoTPortcullisMover>(
			AWoTPortcullisMover::StaticClass(), FTransform(FVector(400, 0, 0)));
	GateMover->TagName = TEXT("ProjectedGateMover");
	GateMover->FinishSpawning(FTransform(FVector(400, 0, 0)));
	AWoTPortcullis* Gate = World->SpawnActorDeferred<AWoTPortcullis>(
		AWoTPortcullis::StaticClass(), FTransform(FVector(400, 0, 0)));
	Gate->MoverTag = TEXT("ProjectedGateMover");
	Gate->FinishSpawning(FTransform(FVector(400, 0, 0)));
	TestEqual(TEXT("portcullis resolves mover by tag"), Gate->GetMover(), GateMover);

	AWoTPitOpening* Opening = World->SpawnActorDeferred<AWoTPitOpening>(
		AWoTPitOpening::StaticClass(), FTransform(FVector(800, 0, 0)));
	Opening->SetUE1CollisionSize(128.0f, 40.0f);
	Opening->FinishSpawning(FTransform(FVector(800, 0, 0)));
	AWoTPit* Pit = World->SpawnActorDeferred<AWoTPit>(
		AWoTPit::StaticClass(), FTransform(FVector(1200, 0, 0)));
	Pit->FinishSpawning(FTransform(FVector(1200, 0, 0)));
	TestNotNull(TEXT("projected PitOpening is claimed during Pit BeginPlay"),
		Opening->ThePitTrap.Get());
	TestEqual(TEXT("pit links back to projected opening"),
		Pit->ThePitTrap ? Pit->ThePitTrap->ThePitOpening.Get() : nullptr, Opening);

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(800, 0, 100), FRotator::ZeroRotator,
		SpecialTriggerAlwaysSpawn());
	Opening->NotifyActorBeginOverlap(Pawn);
	TestEqual(TEXT("pawn above squat PitOpening does not overlap"),
		Pawn->GetActorLocation().X, 800.0, 0.1);
	Pawn->SetActorLocation(FVector(800, 0, 0), false);
	Opening->NotifyActorBeginOverlap(Pawn);
	TestTrue(TEXT("pawn inside squat PitOpening is accepted"),
		Pawn->GetActorLocation().X > 1100.0);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// UE1 raises Touch/UnTouch when the flat cylinders meet/part, whatever movement
// brought that about. The UE capsule is only a containing broad phase, so a pawn
// that enters it OUTSIDE the exact cylinder (dropping in from above) must touch
// on the frame it reaches the cylinder, and one that rises out of the cylinder
// while still inside the capsule must untouch. Real engine overlaps throughout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUE1TouchTrackerSpec,
	"WoT.Events.UE1Touch.BroadPhaseEntryThenCylinder", WoTSpecialTriggerFlags)

bool FWoTUE1TouchTrackerSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FName Tag(TEXT("UE1TouchTracker"));
	AWoTTestSoundDispatcherEventSink* Sink = SpawnEventSink(World, Tag);
	AWoTTriggerLight* Light = World->SpawnActorDeferred<AWoTTriggerLight>(
		AWoTTriggerLight::StaticClass(), FTransform::Identity);
	Light->Mode = EWoTTriggerLightMode::Control;   // Trigger -> on, UnTrigger -> off
	Light->ChangeTime = 0.01f;
	Light->FullIntensity = 8.0f;
	Light->TagName = Tag;
	Light->FinishSpawning(FTransform::Identity);

	// The Battle2_01 staircase shape: 150 radius, 32 half-height.
	AWoTTrigger* Trigger = World->SpawnActorDeferred<AWoTTrigger>(
		AWoTTrigger::StaticClass(), FTransform::Identity);
	Trigger->EventName = Tag;
	Trigger->TriggerType = EWoTTriggerType::PawnProximity;
	Trigger->SetUE1CollisionSize(150.0f, 32.0f);
	Trigger->FinishSpawning(FTransform::Identity);
	TestTrue(TEXT("broad phase contains the flat cylinder"),
		Trigger->Capsule->GetUnscaledCapsuleHalfHeight()
			- Trigger->Capsule->GetUnscaledCapsuleRadius() >= 32.0f);

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(5000, 0, 0), FRotator::ZeroRotator, SpecialTriggerAlwaysSpawn());
	if (!TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pawn->GetCharacterMovement()->DisableMovement();   // positional, not physical
	float PawnRadius = 0.0f, PawnHalfHeight = 0.0f;
	Pawn->GetSimpleCollisionCylinder(PawnRadius, PawnHalfHeight);
	const FVector Above(0, 0, 32.0f + PawnHalfHeight + 40.0f);
	const FVector Inside(0, 0, 0);

	Pawn->SetActorLocation(Above);   // enters the capsule, not the cylinder
	WoTTest::TickWorld(World, 3);
	TestEqual(TEXT("entering the broad phase above the cylinder does not touch"),
		Sink->TriggerCount, 0);
	TestTrue(TEXT("the pawn is inside the containing capsule"),
		Trigger->Capsule->IsOverlappingActor(Pawn));

	Pawn->SetActorLocation(Inside);  // no new engine begin-overlap
	WoTTest::TickWorld(World, 3);
	TestEqual(TEXT("reaching the cylinder inside the capsule touches"),
		Sink->TriggerCount, 1);
	TestTrue(TEXT("TriggerControl light switched on by the touch"),
		Light->Light->Intensity > 7.0f);

	Pawn->SetActorLocation(Above);   // still inside the capsule
	WoTTest::TickWorld(World, 3);
	TestTrue(TEXT("leaving the cylinder inside the capsule untouches (UnTrigger)"),
		Light->Light->Intensity < 1.0f);
	TestEqual(TEXT("leaving does not re-fire Touch"), Sink->TriggerCount, 1);

	Pawn->SetActorLocation(Inside);
	WoTTest::TickWorld(World, 3);
	TestEqual(TEXT("re-entering the cylinder touches again"), Sink->TriggerCount, 2);

	Pawn->SetActorLocation(FVector(5000, 0, 0));   // leaves the broad phase
	WoTTest::TickWorld(World, 3);
	TestTrue(TEXT("leaving the broad phase untouches"), Light->Light->Intensity < 1.0f);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
