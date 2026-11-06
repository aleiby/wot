// Warder.uc:96,418-425: a Warder holding WaitingIdle on a threat ducks its own
// sounds for the complete WaitAcquired tween+hold window. Helper names are
// file-unique because the Tests unity build merges translation units.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../WoTAnimTable.h"
#include "WoTTestAIClasses.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
	AWoTNPCCharacter* WarderThreatWaitDuckSpecSpawnWarder(
		UWorld* World, AWoTNPCController*& OutController,
		UWoTWarderFadeCloakBehavior*& OutBehavior,
		TSubclassOf<AWoTNPCCharacter> PawnClass =
			AWoTTestNPCCharacter::StaticClass())
	{
		AWoTNPCCharacter* Warder = WoTCreatureData::SpawnCreature(
			*World, TEXT("Warder"), FVector(0, 0, 100), 1,
			/*bCombatReady*/ true, PawnClass);
		OutController = Warder
			? Cast<AWoTNPCController>(Warder->GetController()) : nullptr;
		OutBehavior = Warder
			? Warder->FindComponentByClass<UWoTWarderFadeCloakBehavior>() : nullptr;
		return Warder;
	}

	AActor* WarderThreatWaitDuckSpecSpawnThreat(UWorld* World)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		// Beyond the Threat slot's 8000uu ZeroIfGreater priority gate: this IS
		// the retail WaitingIdle scenario (a VALID threat too far to track —
		// "waiting for goal to get closer", Warder.uc:421) and it keeps the
		// possession-time priority pass falling back to Waiting, which the
		// repossession regression depends on.
		return World->SpawnActor<AActor>(
			AActor::StaticClass(), FVector(9000, 0, 100),
			FRotator::ZeroRotator, Params);
	}

	float WarderThreatWaitDuckSpecExpectedVolume(const AWoTNPCCharacter& Warder)
	{
		const AWoTNPCCharacter* Defaults =
			Warder.GetClass()->GetDefaultObject<AWoTNPCCharacter>();
		return Defaults ? 0.10f * Defaults->SoundVolumeMultiplier : 0.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWarderThreatWaitDuckWindowSpec,
	"WoT.AI.WarderThreatWaitDuck.Window",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWarderThreatWaitDuckWindowSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCController* Controller = nullptr;
	UWoTWarderFadeCloakBehavior* Behavior = nullptr;
	// The quiet class authors SoundVolumeMultiplier=0.5, so the ducked level
	// (0.05) is distinguishable from a hard-coded 0.10 AND from 0.10 * the
	// current runtime value (review-caught spec gap).
	AWoTNPCCharacter* Warder = WarderThreatWaitDuckSpecSpawnWarder(
		World, Controller, Behavior, AWoTTestQuietNPCCharacter::StaticClass());
	AActor* Threat = WarderThreatWaitDuckSpecSpawnThreat(World);
	if (!TestNotNull(TEXT("Warder"), Warder)
		|| !TestNotNull(TEXT("controller"), Controller)
		|| !TestNotNull(TEXT("fade behavior"), Behavior)
		|| !TestNotNull(TEXT("threat"), Threat))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Compute the retail duck window exactly as the behavior does: WaitAcquired
	// tween-in + full hold (Warder.uc:422-424 TweenPlaySlotAnim + FinishAnim).
	float WindowDuration = 0.25f;
	if (const FWoTAnimTableEntry* Pick =
			Warder->StampSlotAnim(TEXT("WaitAcquired")))
	{
		WindowDuration = WoTAnimTable::ResolveTweenTime(Pick)
			+ FMath::Max(Warder->SequenceDurationSeconds(
				Pick->AnimName, WoTAnimTable::ResolveRate(Pick)), 0.01f);
	}

	const float OriginalVolume = 0.73f;
	Warder->SoundVolumeMultiplier = OriginalVolume;
	Warder->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
	Behavior->TickBehavior(0.0f);
	const float ExpectedDuckedVolume =
		WarderThreatWaitDuckSpecExpectedVolume(*Warder);
	TestEqual(TEXT("quiet class default is authored non-1.0"),
		ExpectedDuckedVolume, 0.05f);
	TestEqual(TEXT("threat wait ducks to ten percent of the class default"),
		Warder->SoundVolumeMultiplier, ExpectedDuckedVolume);

	// The duck covers the WHOLE tween+hold: still ducked mid-window even after
	// the threat expires (retail restores only after FinishAnim), and restored
	// only once the window boundary passes.
	Warder->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
	Behavior->TickBehavior(WindowDuration * 0.5f);
	TestEqual(TEXT("mid-window stays ducked past threat expiry"),
		Warder->SoundVolumeMultiplier, ExpectedDuckedVolume);
	Behavior->TickBehavior(WindowDuration);
	TestEqual(TEXT("threat wait window end restores the pre-duck value"),
		Warder->SoundVolumeMultiplier, OriginalVolume);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWarderPlainWaitVolumeSpec,
	"WoT.AI.WarderThreatWaitDuck.PlainWait",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWarderPlainWaitVolumeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCController* Controller = nullptr;
	UWoTWarderFadeCloakBehavior* Behavior = nullptr;
	AWoTNPCCharacter* Warder = WarderThreatWaitDuckSpecSpawnWarder(
		World, Controller, Behavior);
	if (!TestNotNull(TEXT("Warder"), Warder)
		|| !TestNotNull(TEXT("controller"), Controller)
		|| !TestNotNull(TEXT("fade behavior"), Behavior))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const float OriginalVolume = 0.61f;
	Warder->SoundVolumeMultiplier = OriginalVolume;
	Behavior->TickBehavior(0.0f);
	TestEqual(TEXT("plain Wait start preserves sound volume"),
		Warder->SoundVolumeMultiplier, OriginalVolume);
	Behavior->TickBehavior(60.0f);
	TestEqual(TEXT("plain Wait completion preserves sound volume"),
		Warder->SoundVolumeMultiplier, OriginalVolume);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWarderThreatWaitDuckTeardownSpec,
	"WoT.AI.WarderThreatWaitDuck.Teardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWarderThreatWaitDuckTeardownSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCController* Controller = nullptr;
	UWoTWarderFadeCloakBehavior* Behavior = nullptr;
	AWoTNPCCharacter* Warder = WarderThreatWaitDuckSpecSpawnWarder(
		World, Controller, Behavior);
	AActor* Threat = WarderThreatWaitDuckSpecSpawnThreat(World);
	if (!TestNotNull(TEXT("Warder"), Warder)
		|| !TestNotNull(TEXT("controller"), Controller)
		|| !TestNotNull(TEXT("fade behavior"), Behavior)
		|| !TestNotNull(TEXT("threat"), Threat))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const float OriginalVolume = 0.47f;
	Warder->SoundVolumeMultiplier = OriginalVolume;
	Warder->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
	Behavior->TickBehavior(0.0f);
	TestEqual(TEXT("teardown leg begins ducked"), Warder->SoundVolumeMultiplier,
		WarderThreatWaitDuckSpecExpectedVolume(*Warder));

	Controller->UnPossess();
	TestEqual(TEXT("unpossess mid-window restores the pre-duck value"),
		Warder->SoundVolumeMultiplier, OriginalVolume);

	// Repossession while the controller ends up Waiting must re-enter the window
	// and duck again — a stale LastBrainState (still Waiting from before the
	// unpossess) would skip the re-entry and run the old timer un-ducked
	// (review-caught MEDIUM). The FORCED test brain makes the possession-time
	// priority pass land in Tracking even for this beyond-gate threat
	// (bForceReachable defeats the 8000uu ZeroIfGreater fallback a real brain
	// takes), so the hook returns the controller to Waiting through EnterState;
	// the sentinel-vs-stale distinction under test is untouched: without the
	// possession reset this tick sees Waiting==Waiting and never ducks.
	Controller->Possess(Warder);
	Controller->SetBrainStateForG20Test(EWoTBrainState::Waiting);
	Behavior->TickBehavior(0.0f);
	TestEqual(TEXT("repossession re-enters the window and re-ducks"),
		Warder->SoundVolumeMultiplier,
		WarderThreatWaitDuckSpecExpectedVolume(*Warder));
	Controller->UnPossess();
	TestEqual(TEXT("second unpossess restores again"),
		Warder->SoundVolumeMultiplier, OriginalVolume);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWarderThreatWaitDuckReentrySpec,
	"WoT.AI.WarderThreatWaitDuck.Reentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWarderThreatWaitDuckReentrySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCController* Controller = nullptr;
	UWoTWarderFadeCloakBehavior* Behavior = nullptr;
	AWoTNPCCharacter* Warder = WarderThreatWaitDuckSpecSpawnWarder(
		World, Controller, Behavior);
	AActor* Threat = WarderThreatWaitDuckSpecSpawnThreat(World);
	if (!TestNotNull(TEXT("Warder"), Warder)
		|| !TestNotNull(TEXT("controller"), Controller)
		|| !TestNotNull(TEXT("fade behavior"), Behavior)
		|| !TestNotNull(TEXT("threat"), Threat))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const float OriginalVolume = 0.83f;
	const float ExpectedDuckedVolume =
		WarderThreatWaitDuckSpecExpectedVolume(*Warder);
	Warder->SoundVolumeMultiplier = OriginalVolume;
	Warder->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
	Behavior->TickBehavior(0.0f);
	TestEqual(TEXT("first threat window uses the class default"),
		Warder->SoundVolumeMultiplier, ExpectedDuckedVolume);

	// Completing while the threat remains valid immediately starts another retail
	// WaitAcquired window. It must restore then re-duck, never multiply 0.10 by 0.10.
	Behavior->TickBehavior(60.0f);
	TestEqual(TEXT("threat-window re-entry does not compound the duck"),
		Warder->SoundVolumeMultiplier, ExpectedDuckedVolume);

	Warder->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
	Behavior->TickBehavior(60.0f);
	TestEqual(TEXT("re-entry retains the original restore value"),
		Warder->SoundVolumeMultiplier, OriginalVolume);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
