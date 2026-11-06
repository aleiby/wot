#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestBattleClasses.h"

#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvEarthTremor.h"
#include "../Inventory/WoTAngrealInvExpWard.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvFork.h"
#include "../Inventory/WoTAngrealInvIce.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInvReflect.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTAngrealInvSwapPlaces.h"
#include "../Inventory/WoTAngrealInvTaint.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Inventory/WoTShieldReturnToSenderReflector.h"
#include "../Projectiles/WoTBurningChunk.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTBalefireFX.h"
#include "../WoTBounceableDecoration.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTDecal.h"
#include "../WoTMeshFX.h"
#include "../WoTMover.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSoundTable.h"
#include "../WoTWeaveFX.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
constexpr EAutomationTestFlags ParityDFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters DAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

template <typename TController = AWoTPlayerController>
TPair<TController*, AWoTCharacter*> DSpawnPlayer(UWorld& World,
	const FVector& Location)
{
	TController* Controller = World.SpawnActor<TController>(DAlwaysSpawn());
	AWoTCharacter* Pawn = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, DAlwaysSpawn());
	if (Controller && Pawn)
	{
		Controller->SetAsLocalPlayerController();
		Controller->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	return {Controller, Pawn};
}

AWoTCharacter* DSpawnPawn(UWorld& World, const FVector& Location)
{
	AWoTCharacter* Pawn = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, DAlwaysSpawn());
	if (Pawn) Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	return Pawn;
}

template <typename TArtifact>
TArtifact* DGive(UWorld& World, APawn* Owner)
{
	TArtifact* Artifact = World.SpawnActor<TArtifact>(
		Owner ? Owner->GetActorLocation() : FVector::ZeroVector,
		FRotator::ZeroRotator, DAlwaysSpawn());
	if (Artifact && Owner)
	{
		Artifact->GiveTo(Owner);
		Artifact->CurCharges = FMath::Max(Artifact->CurCharges, 20);
		WoTTest::TickWorld(&World, 0.02f);
	}
	return Artifact;
}

void DAimAt(AWoTCharacter* Caster, const FVector& Target)
{
	const FVector Source = Caster->GetActorLocation()
		+ FVector(0, 0, Caster->BaseEyeHeight);
	const FRotator Aim = (Target - Source).Rotation();
	Caster->SetActorRotation(FRotator(0, Aim.Yaw, 0));
	if (AController* Controller = Caster->GetController())
	{
		Controller->SetControlRotation(Aim);
	}
	else
	{
		Caster->SetActorRotation(Aim);
	}
}

template <typename TArtifact>
TArtifact* DCastAt(UWorld& World, AWoTCharacter* Caster, AActor* Target)
{
	TArtifact* Artifact = DGive<TArtifact>(World, Caster);
	if (Artifact && Caster && Target)
	{
		Artifact->SetVictim(Target);
		DAimAt(Caster, Target->GetActorLocation());
		Artifact->CastArtifact();
	}
	return Artifact;
}

template <typename Predicate>
bool DTickUntil(UWorld* World, float Timeout, Predicate&& Done)
{
	for (int32 Frame = 0; Frame < FMath::CeilToInt(Timeout * 60.0f); ++Frame)
	{
		if (Done()) return true;
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	}
	return Done();
}

template <typename TActor>
int32 DCount(UWorld* World)
{
	int32 Count = 0;
	for (TActorIterator<TActor> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed()) ++Count;
	}
	return Count;
}

AStaticMeshActor* DSpawnFloor(UWorld& World)
{
	AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(
		FVector(0, 0, -10), FRotator::ZeroRotator, DAlwaysSpawn());
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("ParityDFloor"));
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->SetBoxExtent(FVector(4000, 4000, 10));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	return Floor;
}

template <typename TValue>
FWoTRuntimeWorldProperty ParityD_WorldProperty(const TCHAR* Name,
	EWoTRuntimeWorldPropertyType Type, const TCHAR* StructName,
	const TValue& Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.StructName = StructName;
	Property.Value.SetNumUninitialized(sizeof(TValue));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(TValue));
	return Property;
}

AWoTCollisionBlocker* ParityD_ProjectBlockAll(UWorld& World,
	const FVector& Location, FWoTWorldProjectionResult& Projection)
{
	FWoTRuntimeClassDefaultRecord ActorClass;
	ActorClass.Identity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaultRecord BlockAllClass;
	BlockAllClass.Identity = TEXT("Engine.BlockAll");
	BlockAllClass.SuperclassIdentity = ActorClass.Identity;
	const float Radius = 40.0f;
	const float HalfHeight = 80.0f;
	BlockAllClass.Properties.Add(ParityD_WorldProperty(TEXT("CollisionRadius"),
		EWoTRuntimeWorldPropertyType::Float, TEXT(""), Radius));
	BlockAllClass.Properties.Add(ParityD_WorldProperty(TEXT("CollisionHeight"),
		EWoTRuntimeWorldPropertyType::Float, TEXT(""), HalfHeight));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!Defaults.LoadFixture({MoveTemp(ActorClass), MoveTemp(BlockAllClass)}, Error))
	{
		return nullptr;
	}
	FWoTRuntimeWorldActor Record;
	Record.Identity = TEXT("Fix1.BlockAll0");
	Record.ClassIdentity = TEXT("Engine.BlockAll");
	const float Position[3] = {
		static_cast<float>(Location.X), static_cast<float>(Location.Y),
		static_cast<float>(Location.Z)};
	Record.Properties.Add(ParityD_WorldProperty(TEXT("Location"),
		EWoTRuntimeWorldPropertyType::Struct, TEXT("Vector"), Position));
	TArray<FWoTRuntimeWorldActor> Records = {MoveTemp(Record)};
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("Fix1ProjectedBlockAll");
	Source.Records = Records;
	if (!FWoTWorldProjector::ProjectDeferred(
		World, Source, Defaults, Projection, Error))
	{
		return nullptr;
	}
	for (AActor* Deferred : Projection.DeferredActors)
	{
		if (AWoTCollisionBlocker* Blocker = Cast<AWoTCollisionBlocker>(Deferred))
		{
			Blocker->FinishSpawning(Blocker->GetActorTransform(), true);
			return Blocker;
		}
	}
	return nullptr;
}

int32 ParityD_CountDecals(UWorld* World, FName Type)
{
	int32 Count = 0;
	for (TActorIterator<AWoTDecal> It(World); It; ++It)
	{
		if (IsValid(*It) && It->DecalType == Type) ++Count;
	}
	return Count;
}

AWoTBalefireRemovalMarker* DFindRemoval(UWorld* World, const AActor* Target)
{
	for (TActorIterator<AWoTBalefireRemovalMarker> It(World); It; ++It)
	{
		if (It->Target == Target) return *It;
	}
	return nullptr;
}

TArray<AWoTEarthTremorChunk*> DCastEarth(UWorld* World)
{
	DSpawnFloor(*World);
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 120));
	Caster->SetActorRotation(FRotator::ZeroRotator);
	if (Controller) Controller->SetControlRotation(FRotator::ZeroRotator);
	AWoTAngrealInvEarthTremor* Earth = DGive<AWoTAngrealInvEarthTremor>(*World, Caster);
	WoTTest::TickWorld(World, 0.05f);
	Earth->CastArtifact();
	TArray<AWoTEarthTremorChunk*> Chunks;
	DTickUntil(World, 4.0f, [&]
	{
		Chunks.Reset();
		for (TActorIterator<AWoTEarthTremorChunk> It(World); It; ++It)
			if (IsValid(*It)) Chunks.Add(*It);
		return Chunks.Num() >= 5;
	});
	return Chunks;
}

AWoTSeekerProjectile* DFindReturnedSeeker(UWorld* World,
	const AWoTSeekerProjectile* Incoming, const APawn* NewSource,
	const AActor* Destination)
{
	for (TActorIterator<AWoTSeekerProjectile> It(World); It; ++It)
	{
		if (*It != Incoming && IsValid(*It) && !It->IsActorBeingDestroyed()
			&& It->SourcePawn == NewSource && It->Destination == Destination)
		{
			return *It;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDLightningChargeSpec,
	"WoT.Parity.D.LightningChargeTimer", ParityDFlags)
bool FParityDLightningChargeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector::ZeroVector);
	DSpawnPawn(*World, FVector(100, 0, 0));
	AWoTAngrealInvLightning* Lightning = DGive<AWoTAngrealInvLightning>(*World, Caster);
	Lightning->CurCharges = 5;
	Lightning->CastArtifact(); WoTTest::TickWorld(World, 0.6f);
	Lightning->UnCastArtifact();
	Lightning->CastArtifact(); WoTTest::TickWorld(World, 0.5f);
	TestEqual(TEXT("sub-second casts share their accumulated charge interval"),
		Lightning->CurCharges, 4);
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDFireballAttachedFlameSpec,
	"WoT.Parity.D.FireballAttachedFlame", ParityDFlags)
bool FParityDFireballAttachedFlameSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	const float EyeZ = Caster->GetActorLocation().Z + Caster->BaseEyeHeight;
	AWoTBreakableDecoration* Target = World->SpawnActor<AWoTBreakableDecoration>(
		FVector(300, 0, EyeZ), FRotator::ZeroRotator, DAlwaysSpawn());
	Target->Health = 1000;
	DCastAt<AWoTAngrealInvFireball>(*World, Caster, Target);
	AWoTFireballFire* Fire = nullptr;
	TestTrue(TEXT("real Fireball impact leaves a FireballFire follower"),
		DTickUntil(World, 2.0f, [&]
		{
			for (TActorIterator<AWoTFireballFire> It(World); It; ++It)
				if (IsValid(*It)) { Fire = *It; return true; }
			return false;
		}));
	if (Fire)
	{
		const FVector RelativeOffset = Fire->GetActorLocation()
			- Target->GetActorLocation();
		TestFalse(TEXT("FireballFire retains the actual impact-point offset"),
			RelativeOffset.IsNearlyZero(0.1f));
		Target->SetActorLocation(Target->GetActorLocation() + FVector(0, 80, 0));
		WoTTest::TickWorld(World, 0.1f);
		TestTrue(TEXT("flame follows the surviving dynamic target"),
			Fire->GetActorLocation().Equals(
				Target->GetActorLocation() + RelativeOffset, 1.0f));
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDSwapBlockedSpec,
	"WoT.Parity.D.SwapPlacesBlocked", ParityDFlags)
bool FParityDSwapBlockedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	AWoTCharacter* Target = DSpawnPawn(*World, FVector(450, 0, 100));
	AWoTAngrealInvSpiritShield* Shield =
		DGive<AWoTAngrealInvSpiritShield>(*World, Target);
	Shield->CastArtifact();
	const FVector SourceBefore = Caster->GetActorLocation();
	const FVector TargetBefore = Target->GetActorLocation();
	DCastAt<AWoTAngrealInvSwapPlaces>(*World, Caster, Target);
	WoTTest::TickWorld(World, 2.0f);
	TestTrue(TEXT("Spirit Shield blocks the real Swap Places payload"),
		Caster->GetActorLocation().Equals(SourceBefore, 1.0f)
		&& Target->GetActorLocation().Equals(TargetBefore, 1.0f));
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDTaintBlockedSpec,
	"WoT.Parity.D.TaintBlocked", ParityDFlags)
bool FParityDTaintBlockedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	AWoTCharacter* Target = DSpawnPawn(*World, FVector(300, 0, 100));
	AWoTAngrealInvDart* Held = DGive<AWoTAngrealInvDart>(*World, Target);
	AWoTAngrealInvFireShield* Shield = DGive<AWoTAngrealInvFireShield>(*World, Target);
	Shield->CastArtifact();
	DCastAt<AWoTAngrealInvTaint>(*World, Caster, Target);
	WoTTest::TickWorld(World, 3.0f);
	TestFalse(TEXT("Fire Shield blocks the all-element Taint payload"), Held->bTainted);
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDSourceDamageTypeSpec,
	"WoT.Parity.D.SourceDamageType", ParityDFlags)
bool FParityDSourceDamageTypeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = DSpawnPawn(*World, FVector(0, 0, 100));
	auto [TargetController, Target] =
		DSpawnPlayer<AWoTTestSealNotifyController>(*World, FVector(600, 0, 100));
	Target->MaxHealth = Target->Health = 500;
	DCastAt<AWoTAngrealInvSeeker>(*World, Caster, Target);
	TestTrue(TEXT("real Seeker reaches and damages its target"),
		DTickUntil(World, 6.0f, [&] { return Target->Health < 500; }));
	TestTrue(TEXT("source Fire+Air type produces the non-generic fire flash"),
		TargetController->DesiredFlashFog.Y > 0.001f
		&& TargetController->DesiredFlashFog.Z > 0.001f);
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDIceColliderSpec,
	"WoT.Parity.D.IceCollider", ParityDFlags)
bool FParityDIceColliderSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	AWoTCharacter* Target = DSpawnPawn(*World, FVector(450, 0, 100));
	DCastAt<AWoTAngrealInvIce>(*World, Caster, Target);
	UWoTIceLeech* Ice = nullptr;
	TestTrue(TEXT("real Ice cast installs its leech"),
		DTickUntil(World, 3.0f, [&]
		{
			Ice = Target->FindComponentByClass<UWoTIceLeech>();
			return IsValid(Ice) && IsValid(Ice->IceBlock);
		}));
	if (Ice && Ice->IceBlock)
	{
		TestTrue(TEXT("base DrawScale 3 keeps the retail 40x48 ice blocker"),
			FMath::IsNearlyEqual(Ice->IceBlock->Capsule->GetUnscaledCapsuleRadius(), 40.0f)
			&& FMath::IsNearlyEqual(Ice->IceBlock->Capsule->GetUnscaledCapsuleHalfHeight(), 48.0f));
		const int32 HealthBefore = Target->Health;
		DCastAt<AWoTAngrealInvDart>(*World, Caster, Target);
		WoTTest::TickWorld(World, 0.6f);
		TestEqual(TEXT("a real Dart bursts on ice without directly damaging the pawn"),
			Target->Health, HealthBefore);
		AWoTCharacter* Walker = DSpawnPawn(*World, Target->GetActorLocation()
			+ FVector(140, 0, 0));
		FHitResult Hit;
		Walker->SetActorLocation(Target->GetActorLocation(), true, &Hit);
		TestTrue(TEXT("the ice cylinder also blocks another pawn"),
			Hit.IsValidBlockingHit() && Hit.GetActor() == Ice->IceBlock);
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDFireballFireLifecycleSpec,
	"WoT.Parity.D.FireballFireLifecycle", ParityDFlags)
bool FParityDFireballFireLifecycleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	const float EyeZ = Caster->GetActorLocation().Z + Caster->BaseEyeHeight;
	AWoTBreakableDecoration* Target = World->SpawnActor<AWoTBreakableDecoration>(
		FVector(300, 0, EyeZ), FRotator::ZeroRotator, DAlwaysSpawn());
	Target->Health = 1000;
	DCastAt<AWoTAngrealInvFireball>(*World, Caster, Target);
	AWoTFireballFire* Fire = nullptr;
	DTickUntil(World, 2.0f, [&]
	{
		for (TActorIterator<AWoTFireballFire> It(World); It; ++It)
			if (IsValid(*It) && It->Flame) { Fire = *It; return true; }
		return false;
	});
	if (TestNotNull(TEXT("dynamic impact spawned Flame01 lifecycle"), Fire))
	{
		TestEqual(TEXT("the production flame uses Flame01"), Fire->Flame->ConfigName,
			FName(TEXT("Flame01")));
		const float Initial = Fire->Flame->Volume;
		TestTrue(TEXT("FireballFire starts with a seven-second life"),
			Fire->GetLifeSpan() > 6.5f && Fire->GetLifeSpan() <= 7.0f);
		WoTTest::TickWorld(World, 1.0f);
		TestTrue(TEXT("emission volume fades with remaining life"),
			Fire->Flame->Volume < Initial && Fire->Flame->Volume > 0.0f);
		Target->Destroy(); WoTTest::TickWorld(World, 0.1f);
		TestFalse(TEXT("emission switches off when the followed actor dies"), Fire->Flame->bOn);
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDReflectSpeedSpec,
	"WoT.Parity.D.ReflectSeekingSpeed", ParityDFlags)
bool FParityDReflectSpeedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	AWoTCharacter* Host = DSpawnPawn(*World, FVector(1000, 0, 100));
	DCastAt<AWoTAngrealInvSeeker>(*World, Caster, Host);
	AWoTSeekerProjectile* Incoming = nullptr;
	for (TActorIterator<AWoTSeekerProjectile> It(World); It; ++It) Incoming = *It;
	WoTTest::TickWorld(World, 2.5f);
	AWoTAngrealInvReflect* Reflect = DGive<AWoTAngrealInvReflect>(*World, Host);
	Reflect->CastArtifact();
	AWoTSeekerProjectile* Returned = nullptr;
	TestTrue(TEXT("accelerated Seeker is reflected through the live touch path"),
		DTickUntil(World, 5.0f, [&]
		{
			Returned = DFindReturnedSeeker(World, Incoming, Host, Caster);
			return Returned != nullptr;
		}));
	if (Returned)
	{
		TestTrue(TEXT("return replacement retains accumulated pursuit speed"),
			Returned->Speed > 175.0f && Returned->GetVelocity().Size() > 175.0f);
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDForkSpeedSpec,
	"WoT.Parity.D.ForkSeekingSpeed", ParityDFlags)
bool FParityDForkSpeedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	AWoTCharacter* Host = DSpawnPawn(*World, FVector(1000, 0, 100));
	Host->MaxHealth = Host->Health = 500;
	DCastAt<AWoTAngrealInvSeeker>(*World, Caster, Host);
	AWoTSeekerProjectile* Incoming = nullptr;
	for (TActorIterator<AWoTSeekerProjectile> It(World); It; ++It) Incoming = *It;
	WoTTest::TickWorld(World, 2.5f);
	AWoTAngrealInvFork* Fork = DGive<AWoTAngrealInvFork>(*World, Host);
	Fork->CastArtifact();
	AWoTSeekerProjectile* Returned = nullptr;
	TestTrue(TEXT("real Fork touch sends a Seeker copy to the source"),
		DTickUntil(World, 5.0f, [&]
		{
			Returned = DFindReturnedSeeker(World, Incoming, Host, Caster);
			return Returned != nullptr;
		}));
	if (Returned)
	{
		TestTrue(TEXT("fork return retains the original accumulated speed"),
			Returned->Speed > 175.0f && Returned->GetVelocity().Size() > 175.0f);
	}
	TestTrue(TEXT("the original Fork branch still damages its host"), Host->Health < 500);
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDBlackSmokeSpec,
	"WoT.Parity.D.BlackSmokeTrail", ParityDFlags)
bool FParityDBlackSmokeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TArray<AWoTEarthTremorChunk*> Chunks = DCastEarth(World);
	WoTTest::TickWorld(World, 0.55f);
	const FWoTSprayerBundleRow* Row = WoTWeaveFX::Find(TEXT("BlackSmoke01"));
	if (TestNotNull(TEXT("BlackSmoke01 shipped row is registered"), Row))
	{
		TestTrue(TEXT("black smoke keeps retail volume, rise and modulated style"),
			FMath::IsNearlyEqual(Row->Volume, 12.0f)
			&& FMath::IsNearlyEqual(Row->Gravity.Z, 40.0f) && Row->bModulateOnly);
	}
	for (AWoTEarthTremorChunk* Chunk : Chunks)
	{
		TestTrue(TEXT("each real Earth chunk creates a following BlackSmoke01 trail"),
			IsValid(Chunk) && IsValid(Chunk->Sprayer)
			&& Chunk->Sprayer->ConfigName == TEXT("BlackSmoke01")
			&& Chunk->Sprayer->FollowActor == Chunk);
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDSpawnChunksSpec,
	"WoT.Parity.D.SpawnChunks", ParityDFlags)
bool FParityDSpawnChunksSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	TArray<AWoTEarthTremorChunk*> Chunks = DCastEarth(World);
	TestEqual(TEXT("real Earth impact throws five LavaRocks"), Chunks.Num(), 5);
	for (AWoTEarthTremorChunk* Chunk : Chunks)
	{
		TestTrue(TEXT("chunk life is randomized inside the retail 1-3 second band"),
			Chunk->GetLifeSpan() > 0.0f && Chunk->GetLifeSpan() <= 3.0f);
		TestTrue(TEXT("surface-relative fan launches away from the floor"),
			Chunk->GetVelocity().Z >= -KINDA_SMALL_NUMBER);
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDReflectorExpirySpec,
	"WoT.Parity.D.ReflectorExpiryWarning", ParityDFlags)
bool FParityDReflectorExpirySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Player] =
		DSpawnPlayer<AWoTTestSealNotifyController>(*World, FVector::ZeroVector);
	AWoTAngrealInvReflect* Reflect = DGive<AWoTAngrealInvReflect>(*World, Player);
	Reflect->CastArtifact();
	WoTTest::TickWorld(World, 3.1f);
	TestEqual(TEXT("Reflect warns at two seconds remaining"), Controller->UISounds.Num(), 1);
	if (Controller->UISounds.Num() > 0)
	{
		TestEqual(TEXT("expiry sound key"), Controller->UISounds[0],
			FName(TEXT("WOT/DefaultExpiring")));
		TestTrue(TEXT("first warning is full volume"),
			FMath::IsNearlyEqual(Controller->UISoundVolumes[0], 1.0f));
	}
	WoTTest::TickWorld(World, 1.0f);
	TestEqual(TEXT("Reflect repeats at one second remaining"), Controller->UISounds.Num(), 2);
	if (Controller->UISoundVolumes.Num() > 1)
		TestTrue(TEXT("second warning is half volume"),
			FMath::IsNearlyEqual(Controller->UISoundVolumes[1], 0.5f));
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDShieldFeedbackSpec,
	"WoT.Parity.D.ShieldDeflectionNoOverlay", ParityDFlags)
bool FParityDShieldFeedbackSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = WoTCreatureData::SpawnCreature(
		*World, TEXT("Soldier"), FVector(0, 0, 100), 1, true);
	UWoTShieldDeflectBehavior* Shield = Soldier
		? Soldier->FindComponentByClass<UWoTShieldDeflectBehavior>() : nullptr;
	TestNotNull(TEXT("production Soldier has a real NPC controller"),
		Soldier ? Cast<AWoTNPCController>(Soldier->GetController()) : nullptr);
	if (TestNotNull(TEXT("production Soldier has shield behavior"), Shield))
		Shield->EnableDeflect();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(500, 0, 100));
	if (!Soldier || !Shield || !Caster)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 OverlaysBefore = DCount<AWoTPawnOverlayBurst>(World);
	DCastAt<AWoTAngrealInvDart>(*World, Caster, Soldier);
	WoTTest::TickWorld(World, 1.0f);
	TestEqual(TEXT("shield deflection suppresses the Reflect skin overlay"),
		DCount<AWoTPawnOverlayBurst>(World), OverlaysBefore);
	bool bSound = false;
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		bSound |= It->SoundSubPath == TEXT("Weapon/Shield/Shield_Deflect1");
	TestTrue(TEXT("shield deflection still emits its authored sound"), bSound);
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDBalefireCarcassSpec,
	"WoT.Parity.D.BalefireCarcass", ParityDFlags)
bool FParityDBalefireCarcassSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector::ZeroVector);
	const float EyeZ = Caster->GetActorLocation().Z + Caster->BaseEyeHeight;
	AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
		FVector(300, 0, EyeZ), FRotator::ZeroRotator, DAlwaysSpawn());
	if (TestNotNull(TEXT("existing production carcass"), Carcass))
	{
		AWoTAngrealInvBalefire* Balefire = DGive<AWoTAngrealInvBalefire>(*World, Caster);
		Balefire->PrimeTime = 0.1f;
		DAimAt(Caster, Carcass->GetActorLocation());
		Balefire->CastArtifact(); WoTTest::TickWorld(World, 0.12f);
		Balefire->UnCastArtifact();
		TestNotNull(TEXT("Balefire removes a corpse already lying in its beam"),
			DFindRemoval(World, Carcass));
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDFireballSnapshotSpec,
	"WoT.Parity.D.FireballBreakableImpactSnapshot", ParityDFlags)
bool FParityDFireballSnapshotSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	const float EyeZ = Caster->GetActorLocation().Z + Caster->BaseEyeHeight;
	AWoTBreakableDecoration* Target = World->SpawnActor<AWoTBreakableDecoration>(
		FVector(300, 0, EyeZ), FRotator::ZeroRotator, DAlwaysSpawn());
	Target->Health = 10;
	DCastAt<AWoTAngrealInvFireball>(*World, Caster, Target);
	TestTrue(TEXT("fatal Fireball hit reaches the breakable death path"),
		DTickUntil(World, 2.0f, [&] { return !IsValid(Target); }));
	TestEqual(TEXT("destroyed breakable still throws the retail five chunks"),
		DCount<AWoTBurningChunk>(World), 5);
	TestEqual(TEXT("destroyed breakable still enters the dynamic flame branch"),
		DCount<AWoTFireballFire>(World), 1);
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityDExpWardMoverSpec,
	"WoT.Parity.D.ExpWardMoverOccupant", ParityDFlags)
bool FParityDExpWardMoverSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	AWoTMover* Mover = World->SpawnActor<AWoTMover>(
		FVector(160, 0, Caster->GetActorLocation().Z + Caster->BaseEyeHeight),
		FRotator::ZeroRotator, DAlwaysSpawn());
	UBoxComponent* Face = NewObject<UBoxComponent>(Mover, TEXT("ParityDMoverFace"));
	Face->SetupAttachment(Mover->GetRootComponent());
	Face->SetBoxExtent(FVector(10, 100, 100));
	Face->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Face->SetCollisionObjectType(ECC_WorldDynamic);
	Face->SetCollisionResponseToAllChannels(ECR_Block);
	Face->RegisterComponent();
	DAimAt(Caster, Mover->GetActorLocation());
	AWoTAngrealInvExpWard* ExpWard = DGive<AWoTAngrealInvExpWard>(*World, Caster);
	ExpWard->CastArtifact();
	AWoTExpWardProjectile* Ward = nullptr;
	for (TActorIterator<AWoTExpWardProjectile> It(World); It; ++It) Ward = *It;
	if (TestNotNull(TEXT("real artifact cast plants a ward on the mover"), Ward))
	{
		TestTrue(TEXT("ward is based on the mover"),
			Ward->GetAttachParentActor() == Mover);
		AWoTCharacter* Occupant = DSpawnPawn(*World, Ward->GetActorLocation());
		Occupant->GetCapsuleComponent()->SetCapsuleSize(1.0f, 1.0f);
		Occupant->GetCapsuleComponent()->SetCollisionResponseToAllChannels(ECR_Ignore);
		TWeakObjectPtr<AWoTExpWardProjectile> WardPtr(Ward);
		WoTTest::TickWorld(World, 1.1f);
		TestTrue(TEXT("existing occupant does not trigger merely when the ward arms"),
			WardPtr.IsValid());
		Mover->AddActorWorldOffset(FVector(5, 0, 0), false, nullptr,
			ETeleportType::TeleportPhysics);
		WoTTest::TickWorld(World, 0.1f);
		TestFalse(TEXT("mover motion relative to the stationary occupant detonates"),
			WardPtr.IsValid());
	}
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityFix1CollisionDisabledSplashSpec,
	"WoT.Parity.Fix1.CollisionDisabledSplash", ParityDFlags)
bool FParityFix1CollisionDisabledSplashSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	const float EyeZ = Caster->GetActorLocation().Z + Caster->BaseEyeHeight;
	AWoTBreakableDecoration* Impact = World->SpawnActor<AWoTBreakableDecoration>(
		FVector(300, 0, EyeZ), FRotator::ZeroRotator, DAlwaysSpawn());
	Impact->Health = 1000;
	AWoTBounceableDecoration* Prop = World->SpawnActor<AWoTBounceableDecoration>(
		FVector(300, 100, EyeZ), FRotator::ZeroRotator, DAlwaysSpawn());
	if (TestNotNull(TEXT("real bounceable fixture spawns"), Prop))
	{
		Prop->ConfigureRuntimeCollision(false, false, false, 0.0f, 0.0f);
		TestEqual(TEXT("fixture has collision disabled"),
			Prop->Collision->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	}
	DCastAt<AWoTAngrealInvFireball>(*World, Caster, Impact);
	TestTrue(TEXT("real Fireball explosion reaches collision-disabled bProjTarget"),
		DTickUntil(World, 2.0f, [&] { return Prop && Prop->IsBouncing(); }));
	if (Prop)
	{
		TestTrue(TEXT("splash momentum launches the bounceable"),
			!Prop->GetBounceVelocity().IsNearlyZero());
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityFix1IceCreatureScaleSpec,
	"WoT.Parity.Fix1.IceCreatureScale", ParityDFlags)
bool FParityFix1IceCreatureScaleSpec::RunTest(const FString&)
{
	auto CheckCreature = [this](FName Creature, float ExpectedRadius,
		float ExpectedHalfHeight)
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
		AWoTNPCCharacter* Target = WoTCreatureData::SpawnCreature(
			*World, Creature, FVector(450, 0, 100), 1, false);
		TestNotNull(Creature.ToString() + TEXT(" production target"), Target);
		if (Target)
		{
			Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			DCastAt<AWoTAngrealInvIce>(*World, Caster, Target);
		}
		UWoTIceLeech* Ice = nullptr;
		TestTrue(Creature.ToString() + TEXT(" real Ice cast installs blocker"),
			Target && DTickUntil(World, 3.0f, [&]
			{
				Ice = Target->FindComponentByClass<UWoTIceLeech>();
				return IsValid(Ice) && IsValid(Ice->IceBlock);
			}));
		if (Ice && Ice->IceBlock)
		{
			TestEqual(Creature.ToString() + TEXT(" scaled ice radius"),
				Ice->IceBlock->Capsule->GetUnscaledCapsuleRadius(),
				ExpectedRadius, 0.01f);
			TestEqual(Creature.ToString() + TEXT(" scaled ice half-height"),
				Ice->IceBlock->Capsule->GetUnscaledCapsuleHalfHeight(),
				ExpectedHalfHeight, 0.01f);
		}
		WoTTest::DestroySpecWorld(World);
	};
	CheckCreature(TEXT("Trolloc"), 40.0f * 4.0f / 3.0f, 64.0f);
	CheckCreature(TEXT("Legion"), 160.0f, 192.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityFix1FireballIceImpactSpec,
	"WoT.Parity.Fix1.FireballIceImpact", ParityDFlags)
bool FParityFix1FireballIceImpactSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	AWoTCharacter* Target = DSpawnPawn(*World, FVector(450, 0, 100));
	DCastAt<AWoTAngrealInvIce>(*World, Caster, Target);
	UWoTIceLeech* Ice = nullptr;
	TestTrue(TEXT("real Ice cast exposes its blocking actor"),
		DTickUntil(World, 3.0f, [&]
		{
			Ice = Target->FindComponentByClass<UWoTIceLeech>();
			return IsValid(Ice) && IsValid(Ice->IceBlock);
		}));
	const int32 FlamesBefore = DCount<AWoTFireballFire>(World);
	const int32 ScorchesBefore = ParityD_CountDecals(World, TEXT("ScorchMark"));
	if (Ice && Ice->IceBlock)
	{
		DCastAt<AWoTAngrealInvFireball>(*World, Caster, Ice->IceBlock);
		TestTrue(TEXT("Fireball on Ice follows the actor-impact flame branch"),
			DTickUntil(World, 2.0f, [&]
			{
				return DCount<AWoTFireballFire>(World) > FlamesBefore;
			}));
		TestEqual(TEXT("Fireball on Ice leaves no level scorch"),
			ParityD_CountDecals(World, TEXT("ScorchMark")), ScorchesBefore);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityFix1FireballProjectedBlockAllSpec,
	"WoT.Parity.Fix1.FireballProjectedBlockAll", ParityDFlags)
bool FParityFix1FireballProjectedBlockAllSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto [Controller, Caster] = DSpawnPlayer(*World, FVector(0, 0, 100));
	const float EyeZ = Caster->GetActorLocation().Z + Caster->BaseEyeHeight;
	FWoTWorldProjectionResult Projection;
	AWoTCollisionBlocker* Blocker = ParityD_ProjectBlockAll(
		*World, FVector(350, 0, EyeZ), Projection);
	TestNotNull(TEXT("production projector materializes Engine.BlockAll"), Blocker);
	const int32 FlamesBefore = DCount<AWoTFireballFire>(World);
	const int32 ScorchesBefore = ParityD_CountDecals(World, TEXT("ScorchMark"));
	if (Blocker)
	{
		DCastAt<AWoTAngrealInvFireball>(*World, Caster, Blocker);
		TestTrue(TEXT("retail BlockAll HitWall still resolves as an actor flame"),
			DTickUntil(World, 2.0f, [&]
			{
				return DCount<AWoTFireballFire>(World) > FlamesBefore;
			}));
		TestEqual(TEXT("projected BlockAll is not level geometry and leaves no scorch"),
			ParityD_CountDecals(World, TEXT("ScorchMark")), ScorchesBefore);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityFix1BalefireLandedCarcassSpec,
	"WoT.Parity.Fix1.BalefireLandedCarcassCylinder", ParityDFlags)
bool FParityFix1BalefireLandedCarcassSpec::RunTest(const FString&)
{
	auto CastAtLandedCarcass = [this](bool bVisualOnlyRay) -> bool
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		DSpawnFloor(*World);
		AWoTCarcass* Carcass = World->SpawnActor<AWoTCarcass>(
			FVector(350, 0, 140), FRotator::ZeroRotator, DAlwaysSpawn());
		Carcass->SetVelocityForTest(FVector(0, 0, -100));
		TestTrue(TEXT("carcass reaches production Landed/AdjustCylinder state"),
			DTickUntil(World, 3.0f, [&] { return Carcass->IsLyingStill(); }));
		UBoxComponent* VisualOnly = nullptr;
		if (bVisualOnlyRay)
		{
			VisualOnly = NewObject<UBoxComponent>(Carcass, TEXT("Fix1VisualOnlyBounds"));
			VisualOnly->SetupAttachment(Carcass->Capsule);
			VisualOnly->SetRelativeLocation(FVector(0, 240, 0));
			VisualOnly->SetBoxExtent(FVector(30));
			VisualOnly->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			VisualOnly->RegisterComponent();
		}
		const FVector CapsuleCenter = Carcass->Capsule->GetComponentLocation();
		const float RayY = bVisualOnlyRay ? CapsuleCenter.Y + 240.0f : CapsuleCenter.Y;
		auto [Controller, Caster] = DSpawnPlayer(*World,
			FVector(0, RayY, CapsuleCenter.Z - 64.0f));
		Caster->SetActorLocation(FVector(0, RayY,
			CapsuleCenter.Z - Caster->BaseEyeHeight));
		AWoTAngrealInvBalefire* Balefire = DGive<AWoTAngrealInvBalefire>(*World, Caster);
		Balefire->PrimeTime = 0.1f;
		DAimAt(Caster, FVector(Carcass->GetActorLocation().X, RayY, CapsuleCenter.Z));
		Balefire->CastArtifact();
		WoTTest::TickWorld(World, 0.12f);
		Balefire->UnCastArtifact();
		WoTTest::TickWorld(World, 0.02f);
		const bool bRemoved = DFindRemoval(World, Carcass) != nullptr;
		WoTTest::DestroySpecWorld(World);
		return bRemoved;
	};
	TestFalse(TEXT("visual-only bounds outside the shifted landed capsule are ignored"),
		CastAtLandedCarcass(true));
	TestTrue(TEXT("a beam through the landed carcass's current capsule removes it"),
		CastAtLandedCarcass(false));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
