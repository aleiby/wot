// Automation specs for BudgetInfo edit events and the SoundDispatcher port.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestSoundDispatcher.h"
#include "../WoTBudgetInfo.h"
#include "../WoTEvents.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSoundDispatcher.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"

namespace
{
	FActorSpawnParameters SoundDispatcherSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	// These specs pin dispatcher/timer cadence at 10ms steps (the pre-fixture
	// local helper's exact CeilToInt count math) — the fixture's 1/60 default
	// would shift tick counts across the timing boundaries under test.
	void SoundDispatcherSpecTick(UWorld* World, float Seconds)
	{
		WoTTest::TickWorld(World, FMath::CeilToInt(Seconds / 0.01f), 0.01f);
	}

	AWoTTestSoundDispatcherEventSink* SoundDispatcherSpecSpawnSink(
		UWorld* World, FName Tag, int32 OrderToken = 0)
	{
		AWoTTestSoundDispatcherEventSink* Sink =
			World->SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
				AWoTTestSoundDispatcherEventSink::StaticClass(), FTransform::Identity);
		Sink->TagName = Tag;
		Sink->OrderToken = OrderToken;
		Sink->FinishSpawning(FTransform::Identity);
		return Sink;
	}

	AWoTSoundDispatcher* SoundDispatcherSpecSpawnDispatcher(
		UWorld* World, const TArray<FWoTSoundDispatcherEvent>& Entries,
		FName Tag = NAME_None, FName Event = NAME_None)
	{
		AWoTSoundDispatcher* Dispatcher =
			World->SpawnActorDeferred<AWoTSoundDispatcher>(
				AWoTSoundDispatcher::StaticClass(), FTransform::Identity);
		Dispatcher->TagName = Tag;
		Dispatcher->EventName = Event;
		Dispatcher->EventList = Entries;
		Dispatcher->FinishSpawning(FTransform::Identity);
		return Dispatcher;
	}

	APawn* SoundDispatcherSpecSpawnPawn(UWorld* World)
	{
		return World->SpawnActor<APawn>(SoundDispatcherSpecAlwaysSpawnParams());
	}

	AWoTPlayerController* SoundDispatcherSpecSpawnController(
		UWorld* World, uint8 Team, APawn*& OutPawn)
	{
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
			SoundDispatcherSpecAlwaysSpawnParams());
		Controller->InitPlayerState();
		Controller->GetPlayerState<AWoTPlayerState>()->Team = Team;
		OutPawn = SoundDispatcherSpecSpawnPawn(World);
		Controller->Possess(OutPawn);
		return Controller;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBudgetInfoEditEventsSpec,
	"WoT.Events.BudgetInfoEditEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBudgetInfoEditEventsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestSoundDispatcherEventSink* BeginSink =
		SoundDispatcherSpecSpawnSink(World, TEXT("BudgetBegin"));
	AWoTTestSoundDispatcherEventSink* EndSink =
		SoundDispatcherSpecSpawnSink(World, TEXT("BudgetEnd"));
	AWoTBudgetInfo* Budget = World->SpawnActor<AWoTBudgetInfo>(
		SoundDispatcherSpecAlwaysSpawnParams());
	Budget->Team = 1;
	Budget->BeginEditEvent = TEXT("BudgetBegin");
	Budget->EndEditEvent = TEXT("BudgetEnd");
	APawn* Pawn = nullptr;
	AWoTPlayerController* Controller =
		SoundDispatcherSpecSpawnController(World, 1, Pawn);

	TestTrue(TEXT("first BeginEdit is granted"), Budget->BeginEdit(Controller));
	TestEqual(TEXT("BeginEditEvent fires once"), BeginSink->TriggerCount, 1);
	TestEqual(TEXT("BeginEdit increments before fan-out"),
		BeginSink->ObservedBudgetEditCount, 1);
	TestTrue(TEXT("BeginEdit other is the budget"), BeginSink->LastOther.Get() == Budget);
	TestTrue(TEXT("BeginEdit instigator is the player's pawn"),
		BeginSink->LastInstigator.Get() == Pawn);

	TestFalse(TEXT("PlayerEditLimit deny path returns false"),
		Budget->BeginEdit(Controller));
	TestEqual(TEXT("PlayerEditLimit deny path fires nothing"),
		BeginSink->TriggerCount, 1);
	TestEqual(TEXT("PlayerEditLimit deny path does not fire EndEditEvent"),
		EndSink->TriggerCount, 0);

	Budget->EndEdit(Controller);
	TestEqual(TEXT("EndEditEvent fires once"), EndSink->TriggerCount, 1);
	TestEqual(TEXT("EndEdit decrements before fan-out"),
		EndSink->ObservedBudgetEditCount, 0);
	TestTrue(TEXT("EndEdit instigator is the player's pawn"),
		EndSink->LastInstigator.Get() == Pawn);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundDispatcherOrderedChainSpec,
	"WoT.Events.SoundDispatcher.OrderedChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSoundDispatcherOrderedChainSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestSoundDispatcherEventSink::ResetTriggerOrder();
	AWoTTestSoundDispatcherEventSink* First =
		SoundDispatcherSpecSpawnSink(World, TEXT("open_gates"), 1);
	AWoTTestSoundDispatcherEventSink* Second =
		SoundDispatcherSpecSpawnSink(World, TEXT("kill_me_01"), 2);
	FWoTSoundDispatcherEvent Open;
	Open.Event = TEXT("open_gates");
	FWoTSoundDispatcherEvent Kill;
	Kill.Event = TEXT("kill_me_01");
	AWoTSoundDispatcher* Dispatcher =
		SoundDispatcherSpecSpawnDispatcher(World, {Open, Kill});

	Dispatcher->WoTTrigger(nullptr, nullptr);
	SoundDispatcherSpecTick(World, 0.02f);
	TestEqual(TEXT("first Battle3_02 event fires once"), First->TriggerCount, 1);
	TestEqual(TEXT("second Battle3_02 event fires once"), Second->TriggerCount, 1);
	TestEqual(TEXT("two events were recorded"),
		AWoTTestSoundDispatcherEventSink::TriggerOrder.Num(), 2);
	if (AWoTTestSoundDispatcherEventSink::TriggerOrder.Num() == 2)
	{
		TestEqual(TEXT("open_gates fires first"),
			AWoTTestSoundDispatcherEventSink::TriggerOrder[0], 1);
		TestEqual(TEXT("kill_me fires second"),
			AWoTTestSoundDispatcherEventSink::TriggerOrder[1], 2);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundDispatcherDelaySpec,
	"WoT.Events.SoundDispatcher.Delay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSoundDispatcherDelaySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestSoundDispatcherEventSink* Sink =
		SoundDispatcherSpecSpawnSink(World, TEXT("DelayedEvent"));
	FWoTSoundDispatcherEvent Entry;
	Entry.Delay = 0.2f;
	Entry.Event = TEXT("DelayedEvent");
	AWoTSoundDispatcher* Dispatcher =
		SoundDispatcherSpecSpawnDispatcher(World, {Entry});

	Dispatcher->WoTTrigger(nullptr, nullptr);
	SoundDispatcherSpecTick(World, 0.19f);
	TestEqual(TEXT("event does not fire before its 0.2 second delay"),
		Sink->TriggerCount, 0);
	// A fresh FTimerManager skips its first same-frame tick (the GFrameCounter
	// guard), so the manager runs one Step behind world time — tick well past
	// the boundary rather than exactly to it.
	SoundDispatcherSpecTick(World, 0.05f);
	TestEqual(TEXT("event fires after its 0.2 second delay"),
		Sink->TriggerCount, 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundDispatcherLatchSpec,
	"WoT.Events.SoundDispatcher.Latch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSoundDispatcherLatchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	APawn* Pawn = SoundDispatcherSpecSpawnPawn(World);
	AWoTTestSoundDispatcherEventSink* Sink =
		SoundDispatcherSpecSpawnSink(World, TEXT("LatchDone"));
	FWoTSoundDispatcherEvent Entry;
	Entry.Delay = 0.1f;
	Entry.Event = TEXT("LatchDone");
	AWoTSoundDispatcher* Dispatcher = SoundDispatcherSpecSpawnDispatcher(
		World, {Entry}, TEXT("LatchLoop"), TEXT("LatchLoop"));

	// The touch leg mirrors the authored portal shape (TT_AnyProximity —
	// PlayerProximity would reject the controllerless fixture pawn).
	Dispatcher->TriggerType = EWoTTriggerType::AnyProximity;
	Dispatcher->WoTTrigger(nullptr, Pawn);
	Dispatcher->WoTTrigger(nullptr, Pawn);
	SoundDispatcherSpecTick(World, 0.15f);   // margin: the TM runs a tick behind
	TestEqual(TEXT("second trigger while dispatching is ignored"),
		Sink->TriggerCount, 1);

	Dispatcher->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	SoundDispatcherSpecTick(World, 0.15f);
	TestEqual(TEXT("touch can dispatch again after the latch releases"),
		Sink->TriggerCount, 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundDispatcherEmptyAndSelfLoopSpec,
	"WoT.Events.SoundDispatcher.EmptyAndSelfLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSoundDispatcherEmptyAndSelfLoopSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	APawn* Pawn = SoundDispatcherSpecSpawnPawn(World);
	AWoTTestSoundDispatcherEventSink* EmptySink =
		SoundDispatcherSpecSpawnSink(World, TEXT("AfterEmpty"));
	FWoTSoundDispatcherEvent Empty;
	Empty.Delay = 5.0f;
	FWoTSoundDispatcherEvent AfterEmpty;
	AfterEmpty.Event = TEXT("AfterEmpty");
	AWoTSoundDispatcher* EmptyDispatcher =
		SoundDispatcherSpecSpawnDispatcher(World, {Empty, AfterEmpty});
	EmptyDispatcher->WoTTrigger(nullptr, Pawn);
	TestEqual(TEXT("empty entry consumes no delay"), EmptySink->TriggerCount, 1);

	AWoTTestSoundDispatcherEventSink* LoopSink =
		SoundDispatcherSpecSpawnSink(World, TEXT("LoopChain"));
	FWoTSoundDispatcherEvent LoopEntry;
	LoopEntry.Event = TEXT("LoopChain");
	AWoTSoundDispatcher* LoopDispatcher = SoundDispatcherSpecSpawnDispatcher(
		World, {LoopEntry}, TEXT("PortalLoop"), TEXT("PortalLoop"));
	LoopDispatcher->TriggerType = EWoTTriggerType::AnyProximity;
	LoopDispatcher->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult());
	TestEqual(TEXT("base touch fan-out self-loops into the dispatch chain"),
		LoopSink->TriggerCount, 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEventFindFirstTaggedSpec,
	"WoT.Events.FindFirstTagged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEventFindFirstTaggedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTaggedMarker* Marker = World->SpawnActorDeferred<AWoTTaggedMarker>(
		AWoTTaggedMarker::StaticClass(), FTransform::Identity);
	Marker->TagName = TEXT("PortalSoundSource");
	Marker->FinishSpawning(FTransform::Identity);
	SoundDispatcherSpecSpawnSink(World, TEXT("PortalSoundSource"));

	TestTrue(TEXT("FindFirstTagged prefers the retail-source marker over a later carrier"),
		World->GetSubsystem<UWoTEventSubsystem>()->FindFirstTagged(
			TEXT("PortalSoundSource")) == Marker);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
