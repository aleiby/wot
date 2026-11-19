#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS
#include "../WoTMover.h"
#include "../WoTCharacter.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "GameFramework/PlayerController.h"
#include "../WoTAssertMover.h"
#include "../WoTGradualMover.h"
#include "../WoTLoopMover.h"
#include "../WoTRotatingMover.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
AWoTMover* MakeMover(UWorld* World, EWoTMoverState State,
	UClass* Class = AWoTMover::StaticClass(), float Delay = 0, bool Once = false)
{
	AWoTMover* Mover = World->SpawnActorDeferred<AWoTMover>(Class, FTransform::Identity);
	Mover->InitialState = State;
	Mover->KeyPos = {FVector::ZeroVector, FVector(120, 0, 0)};
	Mover->MoveTime = 0.3f;
	Mover->StayOpenTime = 0.2f;
	Mover->DelayTime = Delay;
	Mover->bTriggerOnceOnly = Once;
	Mover->TagName = TEXT("ReleaseDoor");
	Mover->FinishSpawning(FTransform::Identity);
	return Mover;
}
ACharacter* MakeBody(UWorld* World, FVector At)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACharacter* Body = World->SpawnActor<ACharacter>(At, FRotator::ZeroRotator, Params);
	Body->GetCharacterMovement()->DisableMovement();
	return Body;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTControlOccupancySpec,
	"WoT.Mover.UnTrigger.ControlOccupancy", Flags)
bool FWoTControlOccupancySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerControl);
	AWoTTrigger* Trigger = World->SpawnActorDeferred<AWoTTrigger>(
		AWoTTrigger::StaticClass(), FTransform(FVector(1000, 0, 0)));
	Trigger->TriggerType = EWoTTriggerType::PawnProximity;
	Trigger->EventName = Mover->TagName;
	Trigger->SetUE1CollisionSize(150, 100);
	Trigger->FinishSpawning(FTransform(FVector(1000, 0, 0)));
	ACharacter* First = MakeBody(World, FVector(1500, 0, 0));
	ACharacter* Second = MakeBody(World, FVector(1500, 200, 0));
	First->SetActorLocation(FVector(1000, -60, 0));
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("entry opens the door"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("the occupant holds it past StayOpenTime"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	Second->SetActorLocation(FVector(1000, 60, 0));
	WoTTest::TickWorld(World, 0.4f);
	First->SetActorLocation(FVector(1500, 0, 0));
	WoTTest::TickWorld(World, 0.6f);
	TestEqual(TEXT("one remaining occupant keeps it open"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	Second->SetActorLocation(FVector(1500, 200, 0));
	WoTTest::TickWorld(World, 2);
	TestEqual(TEXT("last exit starts closing within two ticks"), Mover->GetMoverPhase(), EWoTMoverPhase::Closing);
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("last exit closes the door"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTControlReverseSpec,
	"WoT.Mover.UnTrigger.ControlReversesAndCancelsDelay", Flags)
bool FWoTControlReverseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerControl);
	Mover->WoTTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 6);
	const FVector Before = Mover->GetActorLocation();
	Mover->WoTUnTrigger(nullptr, nullptr);
	TestEqual(TEXT("release reverses an opening leg immediately"), Mover->GetMoverPhase(), EWoTMoverPhase::Closing);
	TestTrue(TEXT("reversal does not teleport"), Before.Equals(Mover->GetActorLocation()));
	WoTTest::TickWorld(World, 2);
	TestTrue(TEXT("the next poses move toward closed"), Mover->GetActorLocation().X < Before.X);
	Mover->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("reassertion reverses closing"), Mover->GetMoverPhase(), EWoTMoverPhase::Opening);
	WoTTest::TickWorld(World, 0.4f);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 0.4f);
	Mover->WoTUnTrigger(nullptr, nullptr); // excess releases cannot make a negative count
	WoTTest::TickWorld(World, 0.4f);
	Mover->DelayTime = 0.5f;
	Mover->WoTTrigger(nullptr, nullptr);
	TestEqual(TEXT("a fresh trigger observes its delay"), Mover->GetMoverPhase(), EWoTMoverPhase::Delaying);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 0.8f);
	TestEqual(TEXT("release cancels the delayed opening"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPoundReleaseSpec,
	"WoT.Mover.UnTrigger.PoundLastRelease", Flags)
bool FWoTPoundReleaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerPound);
	AActor* Source = World->SpawnActor<AActor>();
	Mover->WoTTrigger(Source, nullptr);
	Mover->WoTTrigger(Source, nullptr);
	WoTTest::TickWorld(World, 6);
	Mover->WoTUnTrigger(Source, nullptr);
	TestEqual(TEXT("first release leaves the active opening alone"), Mover->GetMoverPhase(), EWoTMoverPhase::Opening);
	const FVector Before = Mover->GetActorLocation();
	Mover->WoTUnTrigger(Source, nullptr);
	TestEqual(TEXT("last release reverses the pound"), Mover->GetMoverPhase(), EWoTMoverPhase::Closing);
	TestTrue(TEXT("pound reversal does not teleport"), Before.Equals(Mover->GetActorLocation()));
	WoTTest::TickWorld(World, 2);
	TestTrue(TEXT("released pound moves toward closed"), Mover->GetActorLocation().X < Before.X);
	WoTTest::TickWorld(World, 1.0f);
	TestEqual(TEXT("released pound stops closed"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	Mover->WoTTrigger(Source, nullptr);
	WoTTest::TickWorld(World, 0.9f);
	TestEqual(TEXT("held pound starts another cycle after the closed dwell"), Mover->GetMoverPhase(), EWoTMoverPhase::Opening);
	Mover->WoTUnTrigger(Source, nullptr);
	WoTTest::TickWorld(World, 1.0f);
	TestEqual(TEXT("release also stops a later cycle"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBaseReleaseDispatchSpec,
	"WoT.Mover.UnTrigger.AuthoredBaseStateOnSubclasses", Flags)
bool FWoTBaseReleaseDispatchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	for (UClass* Class : {AWoTAssertMover::StaticClass(), AWoTLoopMover::StaticClass(),
		AWoTGradualMover::StaticClass(), AWoTRotatingMover::StaticClass()})
	{
		AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerControl, Class);
		Mover->WoTTrigger(nullptr, nullptr);
		WoTTest::TickWorld(World, 0.1f);
		Mover->WoTUnTrigger(nullptr, nullptr);
		TestEqual(Class->GetName() + TEXT(" dispatches the authored base-state release"),
			Mover->GetMoverPhase(), EWoTMoverPhase::Closing);
		Mover->Destroy();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTToggleReleaseSpec,
	"WoT.Mover.UnTrigger.ToggleIgnoresRelease", Flags)
bool FWoTToggleReleaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerToggle);
	Mover->WoTTrigger(nullptr, nullptr);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 1.0f);
	TestEqual(TEXT("toggle stays open after release"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTimedReleaseSpec,
	"WoT.Mover.UnTrigger.OpenTimedIgnoresRelease", Flags)
bool FWoTTimedReleaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerOpenTimed);
	Mover->WoTTrigger(nullptr, nullptr);
	Mover->WoTUnTrigger(nullptr, nullptr);
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("timed door still reaches its open dwell"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("timed door closes on its timer"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPoundTimingSpec,
	"WoT.Mover.UnTrigger.PoundTimingAndOnceOnly", Flags)
bool FWoTPoundTimingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerPound,
		AWoTMover::StaticClass(), 0, true);
	FFloatProperty* OtherTime = FindFProperty<FFloatProperty>(Mover->GetClass(), TEXT("OtherTime"));
	if (!TestNotNull(TEXT("Pound exposes its authored open dwell"), OtherTime)) return false;
	OtherTime->SetPropertyValue_InContainer(Mover, 0.5f);
	AActor* Source = World->SpawnActor<AActor>();
	Mover->WoTTrigger(Source, nullptr);
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("once-only pound first opens"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	WoTTest::TickWorld(World, 0.3f);
	TestEqual(TEXT("OtherTime holds it open longer than StayOpenTime"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	Mover->WoTUnTrigger(Source, nullptr);
	TestEqual(TEXT("once-only pound can be released during the open dwell"), Mover->GetMoverPhase(), EWoTMoverPhase::Closing);
	WoTTest::TickWorld(World, 0.7f);
	Mover->WoTTrigger(Source, nullptr);
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("once-only pound stays closed after its completed cycle"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);

	AWoTMover* Control = MakeMover(World, EWoTMoverState::TriggerControl,
		AWoTMover::StaticClass(), 0, true);
	Control->WoTTrigger(Source, nullptr);
	WoTTest::TickWorld(World, 0.4f);
	Control->WoTUnTrigger(Source, nullptr);
	WoTTest::TickWorld(World, 0.7f);
	TestEqual(TEXT("once-only control instead stays open after completing its opening"),
		Control->GetMoverPhase(), EWoTMoverPhase::Open);
	return true;
}


namespace
{
AWoTTrigger* MakePlayerTrigger(UWorld* World, AWoTMover* Mover)
{
	const FTransform At(FVector(1000, 0, 0));
	AWoTTrigger* Trigger = World->SpawnActorDeferred<AWoTTrigger>(AWoTTrigger::StaticClass(), At);
	Trigger->TriggerType = EWoTTriggerType::PlayerProximity;
	Trigger->EventName = Mover->TagName;
	Trigger->SetUE1CollisionSize(150, 100);
	Trigger->FinishSpawning(At);
	return Trigger;
}
AWoTCharacter* MakePlayerBody(UWorld* World, APlayerController*& Controller)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Body = World->SpawnActor<AWoTCharacter>(FVector(1500, 0, 0),
		FRotator::ZeroRotator, Params);
	Controller = World->SpawnActor<APlayerController>();
	Controller->Possess(Body);
	Body->GetCharacterMovement()->DisableMovement();
	return Body;
}
void CopyPhaseToProxy(AWoTMover* Server, AWoTMover* Proxy)
{
	for (const TCHAR* Name : {TEXT("Phase"), TEXT("PhaseStartAlpha"), TEXT("PhaseStartServerTime")})
	{
		FProperty* Property = FindFProperty<FProperty>(AWoTMover::StaticClass(), Name);
		Property->CopyCompleteValue_InContainer(Proxy, Server);
	}
	Proxy->ProcessEvent(Proxy->FindFunctionChecked(TEXT("OnRep_Phase")), nullptr);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTControlPlayerExitSpec,
	"WoT.Mover.UnTrigger.PlayerExitLifecycle", Flags)
bool FWoTControlPlayerExitSpec::RunTest(const FString&)
{
	for (const FString Exit : {TEXT("destroy"), TEXT("death"), TEXT("teleport"), TEXT("swap")})
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerControl);
		AWoTTrigger* Trigger = MakePlayerTrigger(World, Mover);
		APlayerController* Controller = nullptr;
		AWoTCharacter* Body = MakePlayerBody(World, Controller);
		Body->SetActorLocation(Trigger->GetActorLocation());
		WoTTest::TickWorld(World, 0.4f);
		TestEqual(Exit + TEXT(" starts occupied"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
		if (Exit == TEXT("destroy"))
		{
			Body->Destroy(); // Pawn destruction clears possession before ending overlaps.
		}
		else if (Exit == TEXT("death"))
		{
			Controller->UnPossess();
			Body->Health = 0;
			Body->Died(nullptr, NAME_None, Body->GetActorLocation());
			TestTrue(TEXT("death leaves a live corpse actor"), IsValid(Body) && Body->bDead);
		}
		else if (Exit == TEXT("teleport"))
		{
			Controller->UnPossess();
			TestTrue(TEXT("teleport succeeds"), Body->TeleportTo(FVector(1500, 0, 0), FRotator::ZeroRotator));
		}
		else
		{
			ACharacter* Destination = MakeBody(World, FVector(1500, 0, 0));
			AWoTSwapPlacesProjectile* Swap = World->SpawnActor<AWoTSwapPlacesProjectile>(
				FVector(2000, 0, 1000), FRotator::ZeroRotator);
			Controller->UnPossess();
			Swap->PerformSwap(Body, Destination);
			TestTrue(TEXT("swap moves the accepted toucher out"), Body->GetActorLocation().Equals(FVector(1500, 0, 0), 1.0f));
		}
		WoTTest::TickWorld(World, 2);
		TestEqual(Exit + TEXT(" releases within two ticks"), Mover->GetMoverPhase(), EWoTMoverPhase::Closing);
		WoTTest::TickWorld(World, 0.4f);
		TestEqual(Exit + TEXT(" closes the door"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
		// A duplicate exit or a never-accepted pawn cannot release a new occupant.
		APlayerController* NextController = nullptr;
		AWoTCharacter* Next = MakePlayerBody(World, NextController);
		Next->SetActorLocation(Trigger->GetActorLocation());
		WoTTest::TickWorld(World, 0.4f);
		Trigger->OnEndOverlap(nullptr, Body, nullptr, 0);
		ACharacter* Stranger = MakeBody(World, FVector(2000, 0, 0));
		Trigger->OnEndOverlap(nullptr, Stranger, nullptr, 0);
		WoTTest::TickWorld(World, 0.4f);
		TestEqual(Exit + TEXT(" ignores unmatched exits"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
		Next->Destroy();
		WoTTest::TickWorld(World, 0.4f);
		TestEqual(Exit + TEXT(" permits a later complete occupancy cycle"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTControlMapChangeSpec,
	"WoT.Mover.UnTrigger.PlayerMapChange", Flags)
bool FWoTControlMapChangeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMover* Mover = MakeMover(World, EWoTMoverState::TriggerControl);
	AWoTTrigger* Trigger = MakePlayerTrigger(World, Mover);
	APlayerController* Controller = nullptr;
	AWoTCharacter* Body = MakePlayerBody(World, Controller);
	Body->SetActorLocation(Trigger->GetActorLocation());
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("old map is occupied"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	World->BeginTearingDown();
	World->EndPlay(EEndPlayReason::LevelTransition);
	WoTTest::DestroySpecWorld(World);
	World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	Mover = MakeMover(World, EWoTMoverState::TriggerControl);
	Trigger = MakePlayerTrigger(World, Mover);
	TestEqual(TEXT("new map starts closed"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	Body = MakePlayerBody(World, Controller);
	Body->SetActorLocation(Trigger->GetActorLocation());
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("new map accepts its own occupant"), Mover->GetMoverPhase(), EWoTMoverPhase::Open);
	Body->Destroy();
	WoTTest::TickWorld(World, 0.4f);
	TestEqual(TEXT("new map has no retained occupancy"), Mover->GetMoverPhase(), EWoTMoverPhase::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTControlDelayedPoseSpec,
	"WoT.Mover.UnTrigger.DelayedReversalPose", Flags)
bool FWoTControlDelayedPoseSpec::RunTest(const FString&)
{
	for (const bool Release : {false, true})
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		AWoTMover* Server = MakeMover(World, EWoTMoverState::TriggerControl);
		Server->WoTTrigger(nullptr, nullptr);
		Server->Tick(0.3f);
		Server->WoTUnTrigger(nullptr, nullptr);
		Server->Tick(0.15f);
		TestTrue(TEXT("closing starts the delay at half travel"), FMath::IsNearlyEqual(Server->GetActorLocation().X, 60.0, 0.01));
		Server->DelayTime = 0.5f;
		Server->WoTTrigger(nullptr, nullptr);
		AWoTMover* Proxy = MakeMover(World, EWoTMoverState::TriggerControl);
		Proxy->SetRole(ROLE_SimulatedProxy);
		CopyPhaseToProxy(Server, Proxy);
		Proxy->Tick(0.1f);
		Server->Tick(0.1f);
		TestTrue(TEXT("late proxy holds the server's partial pose during delay"), Proxy->GetActorLocation().Equals(Server->GetActorLocation(), 0.01));
		// A repeated assertion restarts the delay without replacing the held alpha.
		Server->WoTTrigger(nullptr, nullptr);
		Server->WoTUnTrigger(nullptr, nullptr);
		if (Release) Server->WoTUnTrigger(nullptr, nullptr);
		else Server->Tick(0.5f);
		CopyPhaseToProxy(Server, Proxy);
		Server->Tick(0.03f);
		Proxy->Tick(0.03f);
		// Default cubic glide at alpha 0.45 or 0.55 on the 120-unit path.
		const double Expected = Release ? 51.03 : 68.97;
		TestTrue(TEXT("next leg starts at the held pose"), FMath::IsNearlyEqual(Server->GetActorLocation().X, Expected, 0.01));
		TestTrue(TEXT("proxy follows the same next leg"), Proxy->GetActorLocation().Equals(Server->GetActorLocation(), 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTControlTimedChainSpec,
	"WoT.Mover.UnTrigger.TimedChainRelease", Flags)
bool FWoTControlTimedChainSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTMover* Source = MakeMover(World, EWoTMoverState::TriggerOpenTimed);
	AWoTMover* Target = MakeMover(World, EWoTMoverState::TriggerControl);
	// Only the target receives this tag; both were registered by BeginPlay.
	World->GetSubsystem<UWoTEventSubsystem>()->UnregisterActor(Source);
	Source->EventName = Target->TagName;
	Source->WoTTrigger(nullptr, nullptr);
	Source->Tick(0.3f);
	Target->Tick(0.3f);
	TestEqual(TEXT("finished opening asserts the chained control"), Target->GetMoverPhase(), EWoTMoverPhase::Open);
	Source->Tick(0.1f);
	TestEqual(TEXT("target stays asserted during source dwell"), Target->GetMoverPhase(), EWoTMoverPhase::Open);
	Source->Tick(0.1f);
	TestEqual(TEXT("source begins closing"), Source->GetMoverPhase(), EWoTMoverPhase::Closing);
	TestEqual(TEXT("target releases at close start, before source moves"), Target->GetMoverPhase(), EWoTMoverPhase::Closing);
	TestTrue(TEXT("source is still at its open pose"), FMath::IsNearlyEqual(Source->GetActorLocation().X, 120.0));
	Target->Tick(0.3f);
	TestEqual(TEXT("chained control closes"), Target->GetMoverPhase(), EWoTMoverPhase::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTControlReversalChainSpec,
	"WoT.Mover.UnTrigger.ReversalChainRelease", Flags)
bool FWoTControlReversalChainSpec::RunTest(const FString&)
{
	for (const EWoTMoverState State : {EWoTMoverState::TriggerControl, EWoTMoverState::TriggerToggle})
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
		AWoTMover* Source = MakeMover(World, State);
		AWoTMover* Target = MakeMover(World, EWoTMoverState::TriggerControl);
		World->GetSubsystem<UWoTEventSubsystem>()->UnregisterActor(Source);
		Source->EventName = Target->TagName;
		// An already asserted target must see the closing edge even when the
		// source reverses before reaching its open key.
		Target->WoTTrigger(Source, nullptr);
		Target->Tick(0.3f);
		Source->WoTTrigger(nullptr, nullptr);
		Source->Tick(0.15f);
		if (State == EWoTMoverState::TriggerControl) Source->WoTUnTrigger(nullptr, nullptr);
		else Source->WoTTrigger(nullptr, nullptr);
		TestEqual(TEXT("reversing source closes"), Source->GetMoverPhase(), EWoTMoverPhase::Closing);
		TestEqual(TEXT("closing reversal releases its target immediately"), Target->GetMoverPhase(), EWoTMoverPhase::Closing);
		// Closing ticks must not send additional releases.
		Target->WoTTrigger(nullptr, nullptr);
		Target->Tick(0.3f);
		Source->Tick(0.3f);
		TestEqual(TEXT("release fires only on the closing edge"), Target->GetMoverPhase(), EWoTMoverPhase::Open);
	}
	return true;
}

#endif
