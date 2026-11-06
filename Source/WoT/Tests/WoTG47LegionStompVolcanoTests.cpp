// G47: the live LegionStompVolcano Touch window.  Helpers and automation names
// carry the G47 prefix so this file cannot collide with the broad AI suite.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTLegionStompVolcano.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../WoTAnimData.h"
#include "../WoTPlayerController.h"
#include "../WoTWeaveFX.h"
#include "WoTTestAIClasses.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

namespace
{
	FActorSpawnParameters G47AlwaysSpawnParams(AActor* Owner = nullptr)
	{
		FActorSpawnParameters Params;
		Params.Owner = Owner;
		Params.Instigator = Cast<APawn>(Owner);
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AWoTTestNPCCharacter* G47SpawnPawn(UWorld* World, const FVector& Location)
	{
		AWoTTestNPCCharacter* Pawn = World->SpawnActor<AWoTTestNPCCharacter>(
			Location, FRotator::ZeroRotator, G47AlwaysSpawnParams());
		if (Pawn)
		{
			Pawn->Health = 100;
			Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
		return Pawn;
	}

	AWoTLegionStompVolcano* G47SpawnVolcano(
		UWorld* World, const FVector& Location, AActor* Owner = nullptr)
	{
		return World->SpawnActor<AWoTLegionStompVolcano>(
			AWoTLegionStompVolcano::StaticClass(), Location,
			FRotator::ZeroRotator, G47AlwaysSpawnParams(Owner));
	}

	AWoTLegionStompVolcano* G47SpawnProxyVolcano(
		UWorld* World, const FVector& Location, AActor* Owner)
	{
		const FTransform Transform(FRotator::ZeroRotator, Location);
		AWoTLegionStompVolcano* Volcano =
			World->SpawnActorDeferred<AWoTLegionStompVolcano>(
				AWoTLegionStompVolcano::StaticClass(), Transform, Owner,
				Cast<APawn>(Owner), ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Volcano)
		{
			Volcano->SetRole(ROLE_SimulatedProxy);
			Volcano->FinishSpawning(Transform);
		}
		return Volcano;
	}

	void G47MovePawn(AWoTTestNPCCharacter* Pawn, const FVector& Location)
	{
		Pawn->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		Pawn->GetCapsuleComponent()->UpdateOverlaps();
	}

	AWoTPlayerController* G47PossessAsLocalPlayer(
		UWorld* World, AWoTTestNPCCharacter* Pawn)
	{
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
		if (Controller && Pawn)
		{
			Controller->SetAsLocalPlayerController();
			Controller->Possess(Pawn);
		}
		return Controller;
	}

	float G47AnimStageDuration(
		const TArray<FWoTAnimSequence>& Sequences, const FName Name)
	{
		for (const FWoTAnimSequence& Sequence : Sequences)
		{
			if (Sequence.Name == Name)
			{
				return (Sequence.NumFrames - 1)
					/ (Sequence.Rate * AWoTLegionStompVolcano::VolcanoAnimRate);
			}
		}
		return 0.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG47VolcanoInitialEncroachTest,
	"WoT.AI.G47LegionStompVolcano.InitialEncroachOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG47VolcanoInitialEncroachTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Victim = G47SpawnPawn(World, FVector(0, 0, 100));
	AWoTTestNPCCharacter* Edge = G47SpawnPawn(World, FVector(500, 0, 100));
	AWoTTestNPCCharacter* Outside = G47SpawnPawn(World, FVector(600, 0, 100));
	const float ContactDistance = AWoTLegionStompVolcano::VolcanoRadius
		+ Edge->GetSimpleCollisionRadius();
	G47MovePawn(Edge, FVector(ContactDistance - 1.0f, 0, 100));
	G47MovePawn(Outside, FVector(ContactDistance + 10.0f, 0, 100));
	AWoTLegionStompVolcano* Volcano = G47SpawnVolcano(World, FVector(0, 0, 100));
	TestNotNull(TEXT("volcano spawned"), Volcano);
	TestEqual(TEXT("spawn encroachment applies exactly one flat-10 Touch"),
		Victim->Health, 90);
	TestEqual(TEXT("48 plus victim-radius cylinder retains the former edge hit"),
		Edge->Health, 90);
	TestEqual(TEXT("a pawn beyond 48 plus victim radius is outside"),
		Outside->Health, 100);
	WoTTest::TickWorld(World, 0.05f);
	TestEqual(TEXT("remaining inside does not continuously damage"), Victim->Health, 90);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG47VolcanoVerticalReentryTest,
	"WoT.AI.G47LegionStompVolcano.VerticalLaunchReentryRedamages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG47VolcanoVerticalReentryTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Victim = G47SpawnPawn(World, FVector(0, 0, 100));
	G47PossessAsLocalPlayer(World, Victim);
	// Keep the fixture's ballistic apex outside its own exact vertical contact
	// extent, so this spec exercises the retail enter/exit story without a
	// horizontal teleport.  The predicate uses these live victim dimensions.
	Victim->GetCapsuleComponent()->SetCapsuleSize(8.0f, 8.0f);
	UCharacterMovementComponent* Movement = Victim->GetCharacterMovement();
	Movement->Velocity = FVector::ZeroVector;
	Movement->SetMovementMode(MOVE_Walking);

	const FVector VolcanoLocation = Victim->GetActorLocation();
	AWoTLegionStompVolcano* Volcano =
		G47SpawnVolcano(World, VolcanoLocation);
	TestEqual(TEXT("standing pawn is touched once at volcano spawn"),
		Victim->Health, 90);
	TestEqual(TEXT("volcano forces the standing pawn to falling"),
		static_cast<int32>(Movement->MovementMode.GetValue()),
		static_cast<int32>(MOVE_Falling));
	TestEqual(TEXT("retail Touch adds the literal +256 directly to live Z velocity"),
		static_cast<float>(Movement->Velocity.Z),
		static_cast<float>(AWoTLegionStompVolcano::VolcanoLaunchZ));

	const float ExitHeight = AWoTLegionStompVolcano::VolcanoHalfHeight
		+ Victim->GetSimpleCollisionHalfHeight();
	bool bExitedExactCylinder = false;
	while (IsValid(Volcano) && Victim->Health > 80)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 120.0f);
		bExitedExactCylinder |=
			FMath::Abs(Victim->GetActorLocation().Z - VolcanoLocation.Z) > ExitHeight;
	}
	TestTrue(TEXT("+256 alone carries the pawn beyond 16 plus victim half-height"),
		bExitedExactCylinder);
	TestEqual(TEXT("gravity return crosses a fresh enter edge and touches again"),
		Victim->Health, 80);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG47VolcanoStompCompositionTest,
	"WoT.AI.G47LegionStompVolcano.ApplyStompComposesVolcanoAndShake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG47VolcanoStompCompositionTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Legion = G47SpawnPawn(World, FVector(0, 0, 100));
	AWoTTestNPCCharacter* Victim = G47SpawnPawn(World, FVector(1000, 0, 100));
	AWoTPlayerController* VictimController = G47PossessAsLocalPlayer(World, Victim);
	UCharacterMovementComponent* Movement = Victim->GetCharacterMovement();
	Movement->GravityScale = 0.0f;
	// Legion.ShakeRadius is authored for PHYS_Walking pawns. Keep this fixture
	// in the production branch it is proving until the toss changes it to falling.
	Movement->SetMovementMode(MOVE_Walking);
	Movement->Velocity = FVector::ZeroVector;

	UWoTLegionSpiritsStompBehavior* Stomp =
		Cast<UWoTLegionSpiritsStompBehavior>(
			UWoTCreatureBehavior::Create(*Legion, TEXT("LegionSpiritsStomp")));
	if (!TestNotNull(TEXT("stomp behavior created"), Stomp))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	const float Distance =
		FVector::Dist(Legion->GetActorLocation(), Victim->GetActorLocation());
	const float ShakeZ = UWoTLegionSpiritsStompBehavior::StompKnockbackZ(
		Distance, Movement->Mass);
	Stomp->ApplyStompAt(Victim->GetActorLocation(), Victim);
	TestEqual(TEXT("damage, volcano and Legion stomp shakes reach only the owner"),
		VictimController ? VictimController->GetShakeRequestCountForTest() : -1, 4);
	TestEqual(TEXT("ApplyStompAt's volcano applies flat damage"), Victim->Health, 90);
	TestEqual(TEXT("retail ShakeRadius toss occurs before volcano Touch"),
		static_cast<float>(Movement->Velocity.Z),
		ShakeZ, 1.0f);

	WoTTest::TickWorld(World, 1);
	TestTrue(FString::Printf(
		TEXT("zero-gravity victim retains ShakeRadius %.1f after volcano Touch (got %.1f)"),
		ShakeZ, Movement->Velocity.Z),
		FMath::IsNearlyEqual(Movement->Velocity.Z, ShakeZ, 1.0f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG47VolcanoClosedWindowTest,
	"WoT.AI.G47LegionStompVolcano.WindowCloses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG47VolcanoClosedWindowTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Victim = G47SpawnPawn(World, FVector(0, 0, 100));
	AWoTLegionStompVolcano* Volcano = G47SpawnVolcano(World, FVector(0, 0, 100));
	G47MovePawn(Victim, FVector(300, 0, 100));
	WoTTest::TickWorld(World, Volcano->GetEruptionLifetimeSeconds() + 0.1f);
	TestFalse(TEXT("volcano is destroyed after both animation stages"), IsValid(Volcano));
	G47MovePawn(Victim, FVector(0, 0, 100));
	TestEqual(TEXT("re-entry after destruction cannot damage"), Victim->Health, 90);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG47VolcanoOwnerDamageTest,
	"WoT.AI.G47LegionStompVolcano.TargetOwnerTakesDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG47VolcanoOwnerDamageTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Target = G47SpawnPawn(World, FVector(0, 0, 100));
	G47SpawnVolcano(World, FVector(0, 0, 100), Target);
	TestEqual(TEXT("the targeted owner is touched by its own volcano"), Target->Health, 90);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG47VolcanoAuthorityGuardTest,
	"WoT.AI.G47LegionStompVolcano.SimulatedProxyCannotDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG47VolcanoAuthorityGuardTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTTestNPCCharacter* Legion = G47SpawnPawn(World, FVector(500, 0, 100));
	AWoTTestNPCCharacter* Victim = G47SpawnPawn(World, FVector(0, 0, 100));
	AWoTLegionStompVolcano* Proxy =
		G47SpawnProxyVolcano(World, FVector(0, 0, 100), Legion);
	TestNotNull(TEXT("simulated-proxy volcano spawned"), Proxy);
	TestEqual(TEXT("simulated-proxy overlap applies no damage"), Victim->Health, 100);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG47VolcanoLifetimeAndRowsTest,
	"WoT.AI.G47LegionStompVolcano.AnimLifetimeAndSprayerRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG47VolcanoLifetimeAndRowsTest::RunTest(const FString&)
{
	const TArray<FWoTAnimSequence>* Sequences =
		WoTAnimData::FindSequences(TEXT("LegionStompVolcano"));
	if (!TestNotNull(TEXT("LegionStompVolcano animation-table row exists"), Sequences))
	{
		return false;
	}
	const float Expected =
		G47AnimStageDuration(*Sequences, TEXT("VolcanoStageOne"))
		+ G47AnimStageDuration(*Sequences, TEXT("VolcanoStageThree"));
	TestEqual(TEXT("two 10-frame, 30fps stages at 0.5 rate total 1.2 seconds"),
		Expected, 1.2f);
	TestEqual(TEXT("actor lifetime is derived from the two animation-table stages"),
		AWoTLegionStompVolcano::ResolveEruptionLifetimeSeconds(), Expected);

	const FWoTSprayerBundleRow* A = WoTWeaveFX::Find(TEXT("LegionStompSprayerA"));
	const FWoTSprayerBundleRow* B = WoTWeaveFX::Find(TEXT("LegionStompSprayerB"));
	const FWoTSprayerBundleRow* C = WoTWeaveFX::Find(TEXT("LegionStompSprayerC"));
	const FWoTSprayerBundleRow* D = WoTWeaveFX::Find(TEXT("LegionStompSprayerD"));
	TestNotNull(TEXT("sprayer A row"), A); TestNotNull(TEXT("sprayer B row"), B);
	TestNotNull(TEXT("sprayer C row"), C); TestNotNull(TEXT("sprayer D row"), D);
	if (A && B && C && D)
	{
		TestEqual(TEXT("A 0.75s emission / 2.25s life"),
			FVector2D(A->BurstSeconds, A->SelfDestroySeconds), FVector2D(0.75f, 2.25f));
		TestEqual(TEXT("B authored reversed velocity endpoints retained"),
			FVector2D(B->Templates[0].Template.MinInitialVelocity,
				B->Templates[0].Template.MaxInitialVelocity), FVector2D(70.0f, -5.0f));
		TestEqual(TEXT("C has two authored templates"), C->Templates.Num(), 2);
		TestEqual(TEXT("D 0.75s emission / 2.75s life"),
			FVector2D(D->BurstSeconds, D->SelfDestroySeconds), FVector2D(0.75f, 2.75f));
	}
	return true;
}

#endif
