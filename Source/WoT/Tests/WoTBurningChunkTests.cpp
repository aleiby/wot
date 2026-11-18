#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvEarthTremor.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTBurningChunk.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTGameInstance.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTRandom.h"
#include "Components/BoxComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/ScopeExit.h"

namespace
{
constexpr EAutomationTestFlags ChunkFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
// The original engine's default zone terminal velocity; port zones currently
// have no terminal-velocity override.
constexpr double TerminalSpeed = 2500.0;

UWorld* ChunkContactWorld()
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.URL = TEXT("/Engine/Maps/Entry?FragLimit=0?TimeLimit=0");
	Options.BeforeInitializeActorsForPlay = [](UWorld* World)
	{
		AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
			FVector(0, 0, -50), FRotator::ZeroRotator);
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetupAttachment(Floor->GetRootComponent());
		Box->SetBoxExtent(FVector(3000, 2000, 50));
		Box->SetMobility(EComponentMobility::Static);
		Box->SetCollisionProfileName(TEXT("BlockAll"));
		Box->RegisterComponent();
		World->SpawnActor<APlayerStart>(FVector(-1000, 0, 50), FRotator::ZeroRotator);
		World->SpawnActor<APlayerStart>(FVector(1000, 0, 50), FRotator(0, 180, 0));
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	// Join an ongoing match, after the artifact's initial group cooldown.
	WoTTest::TickWorld(World, 12.0f);
	return World;
}

struct FChunkSpeedSamples
{
	double PeakSpeed = 0.0;
	bool bFinite = true;
	int32 Count = 0;
	void Observe(const AWoTBurningChunk& Chunk)
	{
		const FVector V = Chunk.ProjectileMovement->Velocity;
		const double Speed = V.Size();
		bFinite &= FMath::IsFinite(V.X) && FMath::IsFinite(V.Y)
			&& FMath::IsFinite(V.Z) && FMath::IsFinite(Speed);
		PeakSpeed = FMath::Max(PeakSpeed, Speed);
		++Count;
	}
	void Check(FAutomationTestBase& Test) const
	{
		Test.TestTrue(TEXT("observed live chunks"), Count > 0);
		Test.TestTrue(TEXT("every observed chunk velocity is finite"), bFinite);
		Test.TestTrue(FString::Printf(TEXT("chunk speed stays within terminal velocity: peak %.17g"), PeakSpeed),
			PeakSpeed <= TerminalSpeed + UE_KINDA_SMALL_NUMBER);
	}
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FWoTChunkGroundContactSpec,
	"WoT.Parity.A.BurningChunksTerminalVelocity.GroundContact", ChunkFlags)
void FWoTChunkGroundContactSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	Names = {TEXT("Base"), TEXT("Lava")};
	Commands = Names;
}
bool FWoTChunkGroundContactSpec::RunTest(const FString& Case)
{
	UWorld* World = ChunkContactWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	const SIZE_T EnsuresBefore = FDebug::GetNumEnsureFailures();
	const FRotator Heading(38.241829, -179.766533, 0);
	AWoTBurningChunk* Chunk = World->SpawnActor<AWoTBurningChunk>(
		Case == TEXT("Lava") ? AWoTEarthTremorChunk::StaticClass() : AWoTBurningChunk::StaticClass(),
		FVector(100, 100, 0), Heading);
	if (!TestNotNull(TEXT("ground-contact chunk"), Chunk)) { return false; }
	Chunk->ProjectileMovement->Velocity = Heading.Vector() * 480.0;
	FChunkSpeedSamples Samples;
	Samples.Observe(*Chunk);
	for (int32 Frame = 0; Frame < 12; ++Frame)
	{
		WoTTest::TickWorld(World, 1);
		if (!TestTrue(TEXT("chunk survives the contact simulation"), IsValid(Chunk))) { break; }
		Samples.Observe(*Chunk);
	}
	Samples.Check(*this);
	TestEqual(TEXT("ground contact causes no ensure"), FDebug::GetNumEnsureFailures(), EnsuresBefore);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTChunkHumanCastSpec,
	"WoT.Parity.A.BurningChunksTerminalVelocity.HumanCastSeed1", ChunkFlags)
bool FWoTChunkHumanCastSpec::RunTest(const FString&)
{
	UWorld* World = ChunkContactWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Human = World->SpawnActor<AWoTCharacter>(
		World->GetAuthGameMode()->DefaultPawnClass, FVector(0, 0, 50), FRotator::ZeroRotator, Params);
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("human pawn"), Human)
		|| !TestNotNull(TEXT("human controller"), Controller)) { return false; }
	Controller->Possess(Human);
	Human->SetCanBeDamaged(false);
	const FTransform Transform(Human->GetActorTransform());
	AWoTAngrealInvEarthTremor* Item = World->SpawnActorDeferred<AWoTAngrealInvEarthTremor>(
		AWoTAngrealInvEarthTremor::StaticClass(), Transform, Human, Human,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("held artifact"), Item)) { return false; }
	Item->bHeldItem = true;
	Item->FinishSpawning(Transform);
	Item->GiveTo(Human);
	Item->SetCharges();
	Human->InventoryComponent->SelectItem(Item);
	WoTRandom::Seed(1);
	const SIZE_T EnsuresBefore = FDebug::GetNumEnsureFailures();
	Controller->SetControlRotation(FRotator(18.262891, 0, 0));
	Human->UseAngreal();
	TestEqual(TEXT("ordinary human input launches one cast"), Item->CastUseCount, 1);
	Human->CeaseUsingAngreal();
	bool bSawPrimary = false;
	for (TActorIterator<AWoTEarthTremorProjectile> It(World); It; ++It) { bSawPrimary = true; }
	TestTrue(TEXT("cast creates a real primary projectile"), bSawPrimary);
	bool bSawRock = false;
	FChunkSpeedSamples Samples;
	for (int32 Frame = 0; Frame < 300; ++Frame)
	{
		WoTTest::TickWorld(World, 1);
		for (TActorIterator<AWoTEarthTremorRock> It(World); It; ++It) { bSawRock = true; }
		for (TActorIterator<AWoTEarthTremorChunk> It(World); It; ++It) { Samples.Observe(**It); }
	}
	TestTrue(TEXT("primary impact creates ground-quake rocks"), bSawRock);
	Samples.Check(*this);
	TestEqual(TEXT("real cast causes no ensure"), FDebug::GetNumEnsureFailures(), EnsuresBefore);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FWoTChunkBounceCallbackSpec,
	"WoT.Parity.A.BurningChunksTerminalVelocity.Callback", ChunkFlags)
void FWoTChunkBounceCallbackSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	Names = {TEXT("WallBelowLimit"), TEXT("WallAboveLimit"), TEXT("TouchBelowLimit"), TEXT("TouchAboveLimit")};
	Commands = Names;
}
bool FWoTChunkBounceCallbackSpec::RunTest(const FString& Case)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	const bool bAbove = Case.Contains(TEXT("Above"));
	const FTransform Transform(FRotator::ZeroRotator, FVector(0, 0, 500));
	AWoTBurningChunk* Chunk = World->SpawnActorDeferred<AWoTBurningChunk>(
		AWoTBurningChunk::StaticClass(), Transform);
	if (!TestNotNull(TEXT("callback chunk"), Chunk)) { return false; }
	Chunk->MinChunkSpeed = Chunk->MaxChunkSpeed = bAbove ? 400.0f : 20.0f;
	Chunk->FinishSpawning(Transform);
	const SIZE_T EnsuresBefore = FDebug::GetNumEnsureFailures();
	if (Case.StartsWith(TEXT("Touch")))
	{
		AWoTCharacter* Other = World->SpawnActor<AWoTCharacter>(FVector(100, 0, 500), FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("touch actor"), Other)) { return false; }
		// The base damage/explosion path ignores the instigator, but Touch still bounces.
		Chunk->SetIgnoredPawn(Other);
		Chunk->ProcessTouch(Other, Other->GetActorLocation());
	}
	else
	{
		FHitResult Hit;
		Chunk->NotifyHit(nullptr, nullptr, nullptr, true, Chunk->GetActorLocation(),
			-FVector::ForwardVector, FVector::ZeroVector, Hit);
	}
	// V=(20,0,0) squares to (400,0,0); V=(400,0,0) reaches the terminal bound.
	// A reflected velocity would point in -X, so this also protects the script fold.
	TestTrue(TEXT("callback retains the bounded component-wise bounce"),
		Chunk->ProjectileMovement->Velocity.Equals(FVector(bAbove ? TerminalSpeed : 400.0, 0, 0), UE_KINDA_SMALL_NUMBER));
	TestTrue(TEXT("bounce keeps the chunk alive and moving"),
		IsValid(Chunk) && Chunk->ProjectileMovement->IsActive());
	TestEqual(TEXT("callback causes no ensure"), FDebug::GetNumEnsureFailures(), EnsuresBefore);
	return !HasAnyErrors();
}

#endif
