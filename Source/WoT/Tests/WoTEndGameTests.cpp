// G32: GameInfo.EndGame 'EndGame'-tag fan-out + the SetEndCams end camera
// (the original engine's GameInfo.EndGame and PlayerPawn GameEnded state).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTInventory.h"
#include "../WoTCharacter.h"
#include "../WoTEvents.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "WoTTestSoundDispatcher.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "UObject/UnrealType.h"

namespace
{
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEndGameFanOutSpec,
	"WoT.Match.EndGameFanOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEndGameFanOutSpec::RunTest(const FString&)
{
	// GameInfo.EndGame: every 'EndGame'-tagged actor is
	// triggered. Census: zero retail authors — the leg is call-shape parity, pinned
	// here so a future SP hook works on day one.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("game mode"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	AWoTTestSoundDispatcherEventSink* Sink =
		World->SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
			AWoTTestSoundDispatcherEventSink::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("event sink spawned"), Sink))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	Sink->TagName = FName(TEXT("EndGame"));
	Sink->FinishSpawning(FTransform::Identity);
	AWoTTestSoundDispatcherEventSink* Bystander =
		World->SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
			AWoTTestSoundDispatcherEventSink::StaticClass(), FTransform::Identity);
	if (IsValid(Bystander))
	{
		Bystander->TagName = FName(TEXT("NotEndGame"));
		Bystander->FinishSpawning(FTransform::Identity);
	}

	Game->EndGame(TEXT("fraglimit"));
	TestEqual(TEXT("'EndGame'-tagged actor triggered once"), Sink->TriggerCount, 1);
	if (IsValid(Bystander))
	{
		TestEqual(TEXT("other tags untouched"), Bystander->TriggerCount, 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEndGameCameraSpec,
	"WoT.Match.EndCameraAndFreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEndGameCameraSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("controller"), PC) || !TestNotNull(TEXT("pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	PC->SetAsLocalPlayerController();
	PC->InitPlayerState();
	PC->Possess(Pawn);
	PC->SetControlRotation(FRotator(0.0f, 37.0f, 0.0f));

	PC->NotifyMatchEnded();

	// PlayerPawn.GameEnded.BeginState: collision off, physics off.
	TestFalse(TEXT("ended pawn collision disabled"), Pawn->GetActorEnableCollision());
	TestEqual(TEXT("ended pawn physics off (PHYS_None)"),
		static_cast<int32>(Pawn->GetCharacterMovement()->MovementMode.GetValue()),
		static_cast<int32>(MOVE_None));

	// FindGoodView runs before bBehindView. With UE1's ViewTarget == None self-view,
	// all 16 trials are the same first-person point: pitch changes, yaw does not.
	const FRotator EndView = PC->GetControlRotation();
	TestTrue(TEXT("end view pitch is the retail 56000 UU"),
		FMath::IsNearlyEqual(FRotator::NormalizeAxis(EndView.Pitch), -52.38f, 0.5f));
	TestTrue(TEXT("self-view end camera retains the current yaw"),
		FMath::IsNearlyEqual(FRotator::NormalizeAxis(EndView.Yaw), 37.0f, 0.5f));

	// The normal self-view branch uses CalcBehindView(..., 150). With the verified
	// 30-UU standoff formula and no obstruction, the camera is 120 UU from the pawn.
	FMinimalViewInfo View;
	PC->CalcCamera(0.0f, View);
	TestTrue(TEXT("self-view end camera uses the retail 150 distance"),
		FMath::IsNearlyEqual(
			FVector::Dist(View.Location, Pawn->GetActorLocation()), 120.0f, 1.0f));

	// An already-external ViewTarget takes PlayerCalcView's 180-distance branch during
	// FindGoodView, so a wall pinching yaw 0 must make the 16-trial sweep choose another yaw.
	AWoTPlayerController* ExternalPC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* ExternalPawn = World->SpawnActor<AWoTCharacter>(
		FVector(800, 500, 0), FRotator::ZeroRotator);
	AActor* ExternalTarget = World->SpawnActor<AActor>(
		FVector(1000, 0, 0), FRotator::ZeroRotator);
	AActor* Wall = World->SpawnActor<AActor>(
		FVector(940, 0, 0), FRotator::ZeroRotator);
	if (IsValid(Wall))
	{
		UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
		Wall->SetRootComponent(Box);
		Box->SetBoxExtent(FVector(10, 400, 200));
		Box->SetCollisionProfileName(TEXT("BlockAll"));
		Box->RegisterComponent();
		Wall->SetActorLocation(FVector(940, 0, 0));
	}
	if (IsValid(ExternalTarget))
	{
		USceneComponent* TargetRoot = NewObject<USceneComponent>(ExternalTarget);
		ExternalTarget->SetRootComponent(TargetRoot);
		TargetRoot->RegisterComponent();
		ExternalTarget->SetActorLocation(FVector(1000, 0, 0));
	}
	if (TestNotNull(TEXT("external-view controller"), ExternalPC)
		&& TestNotNull(TEXT("external-view pawn"), ExternalPawn)
		&& TestNotNull(TEXT("external view target"), ExternalTarget))
	{
		ExternalPC->SetAsLocalPlayerController();
		ExternalPC->Possess(ExternalPawn);
		ExternalPC->SetControlRotation(FRotator::ZeroRotator);
		ExternalPC->SetViewTarget(ExternalTarget);
		ExternalPC->NotifyMatchEnded();
		TestFalse(TEXT("external ViewTarget runs the farthest-camera yaw sweep"),
			FMath::IsNearlyZero(FRotator::NormalizeAxis(
				ExternalPC->GetControlRotation().Yaw), 0.5f));
		FMinimalViewInfo ExternalView;
		ExternalPC->CalcCamera(0.0f, ExternalView);
		TestTrue(TEXT("external ViewTarget sweep uses the retail 180 distance"),
			FMath::IsNearlyEqual(FVector::Dist(
				ExternalView.Location, ExternalTarget->GetActorLocation()), 150.0f, 1.0f));
	}

	// ClientGameEnded enters the same state body. Drive the RepNotify as an autonomous
	// proxy and prove the shared helper applies non-replicated collision plus PHYS_None.
	AWoTPlayerController* ClientPC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* ClientPawn = World->SpawnActor<AWoTCharacter>(
		FVector(1600, 0, 0), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("client-path controller"), ClientPC)
		&& TestNotNull(TEXT("client-path pawn"), ClientPawn))
	{
		ClientPC->SetAsLocalPlayerController();
		ClientPC->Possess(ClientPawn);
		ClientPC->SetRole(ROLE_AutonomousProxy);
		FBoolProperty* MatchEndedProperty = FindFProperty<FBoolProperty>(
			AWoTPlayerController::StaticClass(), TEXT("bMatchEnded"));
		UFunction* MatchEndedNotify = ClientPC->FindFunction(TEXT("OnRep_MatchEnded"));
		if (TestNotNull(TEXT("match-ended replicated property"), MatchEndedProperty)
			&& TestNotNull(TEXT("match-ended client notify"), MatchEndedNotify))
		{
			MatchEndedProperty->SetPropertyValue_InContainer(ClientPC, true);
			ClientPC->ProcessEvent(MatchEndedNotify, nullptr);
			TestFalse(TEXT("client match-end path disables pawn collision"),
				ClientPawn->GetActorEnableCollision());
			TestEqual(TEXT("client match-end path applies PHYS_None"),
				static_cast<int32>(ClientPawn->GetCharacterMovement()->MovementMode.GetValue()),
				static_cast<int32>(MOVE_None));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFragLimitVictimPossessionSpec,
	"WoT.Match.FragLimitVictimPossession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFragLimitVictimPossessionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTPlayerController* KillerPC = World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerController* VictimPC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* KillerPawn = World->SpawnActor<AWoTCharacter>(
		FVector(200, 0, 0), FRotator::ZeroRotator);
	AWoTCharacter* VictimPawn = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("game mode"), Game)
		|| !TestNotNull(TEXT("killer controller"), KillerPC)
		|| !TestNotNull(TEXT("victim controller"), VictimPC)
		|| !TestNotNull(TEXT("killer pawn"), KillerPawn)
		|| !TestNotNull(TEXT("victim pawn"), VictimPawn))
	{
		WoTTest::DestroySpecWorld(World);
		return true;
	}
	KillerPC->InitPlayerState();
	VictimPC->InitPlayerState();
	Game->ChangeTeam(KillerPC, 255);
	Game->ChangeTeam(VictimPC, 255);
	KillerPC->Possess(KillerPawn);
	VictimPC->Possess(VictimPawn);
	Game->FragLimit = 1;

	Game->PlayerKilled(KillerPC, VictimPawn, FName(TEXT("FragLimitSpec")));

	const AWoTPlayerState* KillerPS = KillerPC->GetPlayerState<AWoTPlayerState>();
	TestTrue(TEXT("ScoreKill runs before the match-ended possession gate"),
		KillerPS && KillerPS->Kills == 1);
	TestTrue(TEXT("the frag limit ends the game"), Game->bGameEnded);
	TestTrue(TEXT("frag-limit victim remains possessed for GameEnded"),
		VictimPC->GetPawn() == VictimPawn);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
