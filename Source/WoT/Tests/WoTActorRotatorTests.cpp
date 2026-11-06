// Headless specs for the Legend.ActorRotator orbit controller port.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTActorRotator.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
AActor* SpawnMovableDrivenActor(UWorld* World, const FVector& Location)
{
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* DrivenActor = World->SpawnActor<AActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	USceneComponent* DrivenRoot = NewObject<USceneComponent>(DrivenActor, TEXT("Root"));
	DrivenRoot->SetMobility(EComponentMobility::Movable);
	DrivenActor->AddInstanceComponent(DrivenRoot);
	DrivenActor->SetRootComponent(DrivenRoot);
	DrivenRoot->RegisterComponent();
	DrivenActor->SetActorLocation(Location);
	return DrivenActor;
}

AWoTActorRotator* SpawnDeferredRotator(UWorld* World, const FTransform& Transform)
{
	return World->SpawnActorDeferred<AWoTActorRotator>(
		AWoTActorRotator::StaticClass(), Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
}

float PhaseForNamedRotator(FName RequestedName, FName& OutActualName)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = RequestedName;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTActorRotator* Rotator = World->SpawnActor<AWoTActorRotator>(
		FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	OutActualName = Rotator->GetFName();
	const float Phase = Rotator->GetFRoll();
	WoTTest::DestroySpecWorld(World);
	return Phase;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTActorRotatorOrbitMathSpec,
	"WoT.ActorRotator.OrbitMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTActorRotatorOrbitMathSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FVector Center(100.0, 200.0, 300.0);
	AActor* DrivenActor = SpawnMovableDrivenActor(World, FVector::ZeroVector);
	const FTransform RotatorTransform(FQuat::Identity, Center);
	AWoTActorRotator* Rotator = SpawnDeferredRotator(World, RotatorTransform);
	Rotator->DrivenActors.Add(TSoftObjectPtr<AActor>(DrivenActor));
	Rotator->MinRadius = 100.0f;
	Rotator->MaxRadius = 100.0f;
	Rotator->MinRotationRate = 16384.0f;
	Rotator->MaxRotationRate = 16384.0f;
	Rotator->MinHeight = 25.0f;
	Rotator->MaxHeight = 25.0f;
	Rotator->bUpdateActorRotation = false;
	Rotator->FinishSpawning(RotatorTransform);

	float ExpectedFRoll = Rotator->GetFRoll();
	constexpr float Step = 0.25f;
	constexpr int32 NumTicks = 4;
	for (int32 TickIndex = 0; TickIndex < NumTicks; ++TickIndex)
	{
		ExpectedFRoll += 16384.0f * Step;
		Rotator->Tick(Step);
	}

	const int32 ExpectedRollUnits = static_cast<int32>(ExpectedFRoll) & 0xFFFF;
	const double RollRadians = static_cast<double>(ExpectedRollUnits)
		* (2.0 * PI / 65536.0);
	const FVector ExpectedLocation = Center + FVector(
		25.0, 100.0 * FMath::Cos(RollRadians), -100.0 * FMath::Sin(RollRadians));
	const FVector ActualLocation = DrivenActor->GetActorLocation();
	TestTrue(TEXT("four UE1 updates land at the hand-computed orbit point"),
		ActualLocation.Equals(ExpectedLocation, 0.01));
	TestTrue(TEXT("height remains on the rotation frame's X plane"),
		FMath::IsNearlyEqual(ActualLocation.X - Center.X, 25.0, 0.01));
	TestTrue(TEXT("the YZ orbit retains its fixed radius"),
		FMath::IsNearlyEqual(
			FVector2D(ActualLocation.Y - Center.Y, ActualLocation.Z - Center.Z).Size(),
			100.0, 0.01));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTActorRotatorBounceHeightSpec,
	"WoT.ActorRotator.BounceHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTActorRotatorBounceHeightSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AActor* DrivenActor = SpawnMovableDrivenActor(World, FVector::ZeroVector);
	AWoTActorRotator* Rotator = SpawnDeferredRotator(World, FTransform::Identity);
	Rotator->DrivenActors.Add(TSoftObjectPtr<AActor>(DrivenActor));
	Rotator->MinHeight = -220.0f;
	Rotator->MaxHeight = 50.0f;
	Rotator->HeightShiftRate = 100000.0f;
	Rotator->bBounceHeight = true;
	Rotator->FinishSpawning(FTransform::Identity);

	TestEqual(TEXT("bounce initializes toward the exact minimum"),
		Rotator->GetNextHeight(), -220.0f);
	Rotator->Tick(1.0f);
	TestEqual(TEXT("reaching minimum flips the target to the exact maximum"),
		Rotator->GetNextHeight(), 50.0f);
	Rotator->Tick(3.0f);
	TestEqual(TEXT("reaching maximum flips the target to the exact minimum"),
		Rotator->GetNextHeight(), -220.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTActorRotatorDeterministicPhaseSpec,
	"WoT.ActorRotator.DeterministicPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTActorRotatorDeterministicPhaseSpec::RunTest(const FString&)
{
	const FName RequestedName(TEXT("DeterministicActorRotator"));
	FName FirstActualName;
	FName SecondActualName;
	const float FirstPhase = PhaseForNamedRotator(RequestedName, FirstActualName);
	const float SecondPhase = PhaseForNamedRotator(RequestedName, SecondActualName);

	TestEqual(TEXT("fresh worlds preserve the requested actor name"),
		FirstActualName, SecondActualName);
	TestEqual(TEXT("equal name seeds initialize to equal FRoll"),
		FirstPhase, SecondPhase);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTActorRotatorLongDistanceOriginsSpec,
	"WoT.ActorRotator.LongDistanceOrigins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTActorRotatorLongDistanceOriginsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FVector FirstOrigin(10.0, 20.0, 30.0);
	const FVector SecondOrigin(450.0, -125.0, 80.0);
	AActor* FirstActor = SpawnMovableDrivenActor(World, FirstOrigin);
	AActor* SecondActor = SpawnMovableDrivenActor(World, SecondOrigin);
	AWoTActorRotator* Rotator = SpawnDeferredRotator(World, FTransform::Identity);
	Rotator->DrivenActors = {
		TSoftObjectPtr<AActor>(FirstActor), TSoftObjectPtr<AActor>(SecondActor)};
	Rotator->DrivenOrigins = {FirstOrigin, SecondOrigin};
	Rotator->MinRadius = 30.0f;
	Rotator->MaxRadius = 30.0f;
	Rotator->MinRotationRate = 0.0f;
	Rotator->MaxRotationRate = 0.0f;
	Rotator->MinHeight = 12.0f;
	Rotator->MaxHeight = 12.0f;
	Rotator->bUpdateActorRotation = false;
	Rotator->FinishSpawning(FTransform::Identity);
	Rotator->Tick(0.25f);

	const FVector FirstOffset = FirstActor->GetActorLocation() - FirstOrigin;
	const FVector SecondOffset = SecondActor->GetActorLocation() - SecondOrigin;
	TestTrue(TEXT("each driven actor receives the same orbit offset"),
		FirstOffset.Equals(SecondOffset, 0.01));
	TestTrue(TEXT("distinct long-distance origins retain their separation"),
		(SecondActor->GetActorLocation() - FirstActor->GetActorLocation()).Equals(
			SecondOrigin - FirstOrigin, 0.01));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
