// G18: hearing-based threat acquisition (Grunt.OnHearNoise, Grunt.uc:545) + the
// alarm→Intermediate hook (Grunt.AlarmSounded, Grunt.uc:566). These specs drive the
// hearing entry (AWoTCharacter::EmitNoise → AWoTNPCController::BroadcastNoise)
// without ticking the perception samplers, so an acquisition is unambiguously from
// hearing and never from sight. Goal-cache visibility is forced OFF; native hearing
// still uses its own retail visibility trace in this unobstructed fixture world.
// Helper names are file-unique (unity build folds all Tests/*.cpp into one TU).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Traps/WoTAlarm.h"
#include "../WoTSoundTable.h"
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	AWoTTestNPCCharacter* SpawnG18NPC(UWorld* World, const FVector& Location, uint8 Team)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTTestNPCCharacter* NPC =
			World->SpawnActor<AWoTTestNPCCharacter>(Location, FRotator::ZeroRotator, Params);
		if (NPC)
		{
			NPC->AssignedTeam = Team;
			NPC->Health = 100;   // a bare spawn's alive gate (DispatchSeePawn rejects <=0)
			NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
		return NPC;
	}

	AWoTNPCController* G18Controller(AWoTNPCCharacter* NPC)
	{
		return NPC ? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	}

	// Put the observer into unacquired Tracking with goal-cache visibility off, so any
	// Threat it ends up with came through hearing.
	void ArmG18Listener(AWoTNPCController* Ctl)
	{
		if (Ctl)
		{
			Ctl->SetBrainStateForG20Test(EWoTBrainState::Tracking);
			if (UWoTTestBrainComponent* TestBrain = Cast<UWoTTestBrainComponent>(Ctl->GetBrain()))
			{
				TestBrain->bForceVisible = false;
			}
		}
	}
}

// --- (a) An enemy's audible noise in the open acquires a Threat. ------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG18HearAcquireSpec,
	"WoT.AI.HearAcquire", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG18HearAcquireSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Observer (team 1) at the origin; an enemy (team 2) 1000uu away — inside the
	// 2000uu Loudness-1 audible radius. The enemy makes noise (EmitNoise, the seam
	// every footstep/landing routes through).
	AWoTTestNPCCharacter* Observer = SpawnG18NPC(World, FVector::ZeroVector, 1);
	AWoTTestNPCCharacter* Enemy = SpawnG18NPC(World, FVector(1000, 0, 0), 2);
	WoTTest::TickWorld(World, 0.1f);   // let AutoPossess seat the controllers
	AWoTNPCController* Ctl = G18Controller(Observer);
	if (!TestNotNull(TEXT("observer"), Observer) || !TestNotNull(TEXT("enemy"), Enemy) ||
	    !TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	ArmG18Listener(Ctl);
	TestFalse(TEXT("observer starts unacquired"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));

	Enemy->EmitNoise(1.0f);

	TestTrue(TEXT("audible unobstructed noise acquires a Threat"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestEqual(TEXT("the Threat targets the noisemaker"),
		Observer->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
		static_cast<AActor*>(Enemy));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- (b) Noise beyond the loudness-scaled radius does not acquire. -----------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG18HearOutOfRangeSpec,
	"WoT.AI.HearOutOfRange", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG18HearOutOfRangeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Enemy at 3000uu — past the 2000uu radius at Loudness 1.
	AWoTTestNPCCharacter* Observer = SpawnG18NPC(World, FVector::ZeroVector, 1);
	AWoTTestNPCCharacter* Enemy = SpawnG18NPC(World, FVector(3000, 0, 0), 2);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = G18Controller(Observer);
	if (!TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	ArmG18Listener(Ctl);

	Enemy->EmitNoise(1.0f);

	TestFalse(TEXT("noise beyond earshot does not acquire"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));

	// Native stimulus at 3000uu is Loudness * (1200000/9000000).
	// Loudness 8 clears both the hard range and HearingThreshold=1 gates.
	Enemy->EmitNoise(8.0f);
	TestTrue(TEXT("a louder noise widens the radius and acquires"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- (c) Hearing a friendly NPC transfers ITS current threat. ----------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG18HearFriendlyTransferSpec,
	"WoT.AI.HearFriendlyTransfer", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG18HearFriendlyTransferSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Observer + a friendly ally (both team 1) within earshot, and the ally's own
	// enemy (team 2) sitting far away — the observer must inherit WHO the ally fights,
	// not acquire the ally.
	AWoTTestNPCCharacter* Observer = SpawnG18NPC(World, FVector::ZeroVector, 1);
	AWoTTestNPCCharacter* Friend = SpawnG18NPC(World, FVector(500, 0, 0), 1);
	AWoTTestNPCCharacter* SharedFoe = SpawnG18NPC(World, FVector(0, 8000, 0), 2);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = G18Controller(Observer);
	if (!TestNotNull(TEXT("controller"), Ctl) || !TestNotNull(TEXT("friend"), Friend) ||
	    !TestNotNull(TEXT("foe"), SharedFoe))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	ArmG18Listener(Ctl);
	// The ally is already engaged with the shared foe.
	Friend->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, SharedFoe);

	Friend->EmitNoise(1.0f);

	TestTrue(TEXT("hearing an engaged friendly adopts a Threat"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestEqual(TEXT("the adopted Threat is the friendly's foe, not the friendly"),
		Observer->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
		static_cast<AActor*>(SharedFoe));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- (d) An already-acquired observer ignores noise (the acquired states stub it). --
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG18HearAcquiredIgnoresSpec,
	"WoT.AI.HearAcquiredIgnores", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG18HearAcquiredIgnoresSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTTestNPCCharacter* Observer = SpawnG18NPC(World, FVector::ZeroVector, 1);
	AWoTTestNPCCharacter* FirstFoe = SpawnG18NPC(World, FVector(600, 0, 0), 2);
	AWoTTestNPCCharacter* SecondFoe = SpawnG18NPC(World, FVector(0, 600, 0), 2);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = G18Controller(Observer);
	if (!TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	ArmG18Listener(Ctl);
	// Already locked onto the first foe (the Acquired* ladder stubs HearNoise).
	Observer->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, FirstFoe);
	Ctl->TransitionOnGoalPriority(); // select the acquired operation, not just a stored slot
	TestFalse(TEXT("a locked observer is not hearing-live"), Ctl->IsHearingLive());

	SecondFoe->EmitNoise(1.0f);

	TestTrue(TEXT("the Threat is unchanged after noise"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));
	TestEqual(TEXT("noise does not re-target an acquired observer"),
		Observer->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
		static_cast<AActor*>(FirstFoe));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- (e) A hidden noisemaker is ignored (Grunt.uc:548 !NoiseMaker.bHidden). --------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG18HearHiddenIgnoredSpec,
	"WoT.AI.HearHiddenIgnored", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG18HearHiddenIgnoredSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTTestNPCCharacter* Observer = SpawnG18NPC(World, FVector::ZeroVector, 1);
	AWoTTestNPCCharacter* Enemy = SpawnG18NPC(World, FVector(800, 0, 0), 2);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = G18Controller(Observer);
	if (!TestNotNull(TEXT("controller"), Ctl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	ArmG18Listener(Ctl);
	Enemy->SetActorHiddenInGame(true);   // bHidden

	Enemy->EmitNoise(1.0f);

	TestFalse(TEXT("a hidden noisemaker is not heard"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));

	// Revealing it and re-emitting acquires, proving the gate was the hidden flag.
	Enemy->SetActorHiddenInGame(false);
	// A different native noise area avoids the 50uu / 0.2s emitter dedup gate.
	Enemy->SetActorLocation(Enemy->GetActorLocation() + FVector(60, 0, 0));
	Enemy->EmitNoise(1.0f);
	TestTrue(TEXT("the revealed noisemaker is heard"),
		Observer->Brain->IsGoalValid(EWoTGoalIndex::Threat));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// --- (f) A rung alarm gives a plain grunt a GI_Intermediate goal at the alarm. -----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG18AlarmIntermediateSpec,
	"WoT.AI.AlarmIntermediate", EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::ProductFilter)
bool FWoTG18AlarmIntermediateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A plain grunt (not a captain) within the SoundRadius*16 = 4080uu fan-out.
	AWoTTestNPCCharacter* Grunt = SpawnG18NPC(World, FVector::ZeroVector, 1);
	AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(FVector(1000, 0, 0), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	AWoTNPCController* Ctl = G18Controller(Grunt);
	if (!TestNotNull(TEXT("controller"), Ctl) || !TestNotNull(TEXT("alarm"), Alarm))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("a fresh grunt can be given new orders"), Ctl->CanBeGivenNewOrders());

	Alarm->ActivateAlarm(nullptr);   // damage/bump-style ring, no ringer

	TestTrue(TEXT("the rung alarm sets a GI_Intermediate goal (Grunt.AlarmSounded)"),
		Grunt->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
	TestEqual(TEXT("the intermediate goal targets the alarm"),
		Grunt->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(),
		static_cast<AActor*>(Alarm));
	bool bGong = false;
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		bGong |= It->SoundSubPath == TEXT("WoTTraps/ActivateAM");
	}
	TestTrue(TEXT("player-deployable Alarm path already emits the live gong"), bGong);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif  // WITH_AUTOMATION_TESTS
