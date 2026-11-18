#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

// The swap artifact's two moves, as the original makes them
// (SwapPlacesEffect.uc:101-205): both ends move or neither does, a far end the
// original could not move (its level, its static actors) never moves, and a
// refused swap returns before the rotation swap, seeker redirect, effect
// exchange, suicide credit and flashes, telling a player caster why.
//
// A projectile that has lost its target flies on as a plain one
// (SeekingProjectile.uc:360-363) and a wall hit names the level as what it hit
// (GenericProjectile.uc:216-222), so the artifact's projectile then swaps with
// the level (AngrealSwapPlacesProjectile.uc:93-121).

#include "WoTTestBattleClasses.h"
#include "WoTTestLeech.h"
#include "WoTTestWorld.h"
#include "../Inventory/WoTAngrealInvSwapPlaces.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../WoTBounceableDecoration.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTCharacter.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTDecoration.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTRetailText.h"
#include "../WoTUE1Collision.h"
#include "../WoTZoneVolume.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace WoTSwapPlacesLevelGeometry
{
constexpr float Step = 1.0f / 30.0f;
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters AlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

// One Static, BlockAll, WorldStatic box part under Owner's root: the shape the
// runtime level builder gives each level part.
UBoxComponent* AddPart(AActor* Owner, FName Name, const FVector& Center,
	const FVector& Extent, bool bRegister = true)
{
	UBoxComponent* Part = NewObject<UBoxComponent>(Owner, Name);
	Part->SetupAttachment(Owner->GetRootComponent());
	Part->SetRelativeLocation(Center);
	Part->SetBoxExtent(Extent);
	Part->SetMobility(EComponentMobility::Static);
	Part->SetCollisionProfileName(TEXT("BlockAll"));
	Part->SetCollisionObjectType(ECC_WorldStatic);
	Owner->AddInstanceComponent(Part);
	Owner->AddOwnedComponent(Part);
	if (bRegister) Part->RegisterComponent();
	return Part;
}

UBoxComponent* AddFloor(AActor* Owner, bool bRegister = true)
{
	// Top face at Z=0.
	return AddPart(Owner, TEXT("Floor"), FVector(0, 0, -50), FVector(4000, 4000, 50),
		bRegister);
}

// The runtime level actor the way the release build makes it: deferred spawn,
// Static parts under the scene root, then FinishSpawning (which promotes the
// parts to the root's mobility, so they follow it).
AWoTRuntimeBSPWorld* SpawnRuntimeLevel(UWorld* World,
	TFunctionRef<void(AWoTRuntimeBSPWorld&)> AddParts)
{
	FActorSpawnParameters Params = AlwaysSpawn();
	Params.bDeferConstruction = true;
	AWoTRuntimeBSPWorld* Level = World->SpawnActor<AWoTRuntimeBSPWorld>(
		AWoTRuntimeBSPWorld::StaticClass(), FTransform::Identity, Params);
	if (!Level) return nullptr;
	AddParts(*Level);
	Level->FinishSpawning(FTransform::Identity, true);
	TInlineComponentArray<UPrimitiveComponent*> Parts(Level);
	for (UPrimitiveComponent* Part : Parts)
	{
		if (!Part->IsRegistered()) Part->RegisterComponent();
	}
	return Level;
}

// A player caster that records its centre messages, holding the swap artifact.
struct FCaster
{
	AWoTTestSealNotifyController* Controller = nullptr;
	AWoTCharacter* Pawn = nullptr;
	AWoTAngrealInvSwapPlaces* Artifact = nullptr;
};

FCaster SpawnCaster(UWorld* World, const FVector& At, float Yaw, bool bFlying)
{
	FCaster Caster;
	Caster.Controller = World->SpawnActor<AWoTTestSealNotifyController>(AlwaysSpawn());
	Caster.Pawn = World->SpawnActor<AWoTCharacter>(At, FRotator(0, Yaw, 0), AlwaysSpawn());
	if (!Caster.Controller || !Caster.Pawn) return Caster;
	Caster.Controller->SetAsLocalPlayerController();
	Caster.Controller->Possess(Caster.Pawn);
	Caster.Controller->SetControlRotation(FRotator(0, Yaw, 0));
	Caster.Pawn->SetActorRotation(FRotator(0, Yaw, 0));
	Caster.Pawn->GetCharacterMovement()->SetMovementMode(
		bFlying ? MOVE_Flying : MOVE_Falling);
	Caster.Artifact = World->SpawnActor<AWoTAngrealInvSwapPlaces>(
		At, FRotator::ZeroRotator, AlwaysSpawn());
	if (Caster.Artifact) Caster.Artifact->GiveTo(Caster.Pawn);
	return Caster;
}

// The caster's projectile, about to detonate at At.
AWoTSwapPlacesProjectile* SpawnSwap(UWorld* World, const FCaster& Caster,
	const FVector& At)
{
	AWoTSwapPlacesProjectile* Swap = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), At, FRotator::ZeroRotator, AlwaysSpawn());
	if (!Swap) return nullptr;
	if (Caster.Artifact) Swap->SetSourceAngreal(Caster.Artifact);
	Swap->InitiatingPawn = Caster.Pawn;
	Swap->SetIgnoredPawn(Caster.Pawn);
	return Swap;
}

// The text a player caster sees: the artifact title and the localized
// SwapPlacesEffect fragment (SwapPlacesEffect.uc:130, :149, :168, :182). Empty
// when the purchaser's localization is unavailable.
FString FailText(const FCaster& Caster, const TCHAR* Key)
{
	const FString* Suffix = FWoTRetailText::EnsureLoaded()
		? FWoTRetailText::FindSectionValue(TEXT("SwapPlacesEffect"), Key) : nullptr;
	return Suffix && Caster.Artifact ? Caster.Artifact->Title + TEXT(" ") + *Suffix
		: FString();
}

void ExpectLastMessage(FAutomationTestBase& Test, const FCaster& Caster,
	const TCHAR* Key, const FString& What)
{
	const FString Expected = FailText(Caster, Key);
	if (Expected.IsEmpty())
	{
		Test.AddWarning(TEXT("retail localization is unavailable; the refusal text is not checked"));
		return;
	}
	const FString Got = Caster.Controller && !Caster.Controller->CenterMessages.IsEmpty()
		? Caster.Controller->CenterMessages.Last() : FString();
	Test.TestEqual(What + TEXT(": the caster is told why"), Got, Expected);
}

// Spawns a caster on Level's floor, strikes Level with a destination-less swap
// projectile through the ordinary wall-hit entry point, and checks that neither the
// level geometry nor the caster moved and that the caster still stands.
void Run(FAutomationTestBase& Test, UWorld* World, AActor* Level, UPrimitiveComponent* Floor)
{
	// The caster faces 90 degrees: the original swaps facings only after both
	// moves succeed (SwapPlacesEffect.uc:193-205), so nothing may turn either.
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(1500, 700, 150), FRotator(0, 90, 0),
		AlwaysSpawn());
	if (!Test.TestNotNull(TEXT("caster spawned"), Caster)) return;
	Caster->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(World, 1.0f, Step);
	if (!Test.TestFalse(TEXT("caster stands on the level floor before the swap"),
		Caster->GetCharacterMovement()->IsFalling())) return;
	const FVector LevelBefore = Level->GetActorLocation();
	const FRotator LevelRotationBefore = Level->GetActorRotation();
	const FVector FloorBefore = Floor->GetComponentLocation();
	const FVector CasterBefore = Caster->GetActorLocation();
	const FRotator CasterRotationBefore = Caster->GetActorRotation();

	AWoTSwapPlacesProjectile* Swap = World->SpawnActor<AWoTSwapPlacesProjectile>(
		AWoTSwapPlacesProjectile::StaticClass(), CasterBefore + FVector(300, 0, 0),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (!Test.TestNotNull(TEXT("swap projectile spawned"), Swap)) return;
	Swap->InitiatingPawn = Caster;
	Test.TestNull(TEXT("the projectile has no destination (its target is gone)"),
		Swap->Destination.Get());
	const FVector Impact = FVector(Swap->GetActorLocation().X, Swap->GetActorLocation().Y, 0);
	const FHitResult Hit(Level, Floor, Impact, FVector::UpVector);
	Swap->NotifyHit(nullptr, Level, Floor, true, Impact, FVector::UpVector,
		FVector::ZeroVector, Hit);

	Test.TestTrue(FString::Printf(TEXT("level geometry stays put (now %s, was %s)"),
		*Level->GetActorLocation().ToString(), *LevelBefore.ToString()),
		Level->GetActorLocation().Equals(LevelBefore, 0.01));
	Test.TestTrue(FString::Printf(TEXT("level floor collision stays put (now %s)"),
		*Floor->GetComponentLocation().ToString()),
		Floor->GetComponentLocation().Equals(FloorBefore, 0.01));
	Test.TestTrue(FString::Printf(TEXT("level geometry keeps its rotation (now %s)"),
		*Level->GetActorRotation().ToString()),
		Level->GetActorRotation().Equals(LevelRotationBefore, 0.01));
	Test.TestTrue(FString::Printf(TEXT("caster keeps its facing (now %s)"),
		*Caster->GetActorRotation().ToString()),
		Caster->GetActorRotation().Equals(CasterRotationBefore, 0.01));
	Test.TestTrue(FString::Printf(TEXT("caster stays put (now %s, was %s)"),
		*Caster->GetActorLocation().ToString(), *CasterBefore.ToString()),
		Caster->GetActorLocation().Equals(CasterBefore, 1.0));
	Test.TestFalse(TEXT("the projectile is spent"), IsValid(Swap));
	WoTTest::TickWorld(World, 1.0f, Step);
	Test.TestFalse(FString::Printf(TEXT("caster still stands a second later (at %s)"),
		*Caster->GetActorLocation().ToString()), Caster->GetCharacterMovement()->IsFalling());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesRuntimeLevelGeometrySpec,
	"WoT.Inventory.SwapPlacesLostTarget.RuntimeLevelGeometryStaysPut",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesRuntimeLevelGeometrySpec::RunTest(const FString&)
{
	// Release builds project each map at runtime into one AWoTRuntimeBSPWorld,
	// whose Movable scene root carries every part. Moving it moves the level.
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	UBoxComponent* Floor = nullptr;
	AWoTRuntimeBSPWorld* Level = WoTSwapPlacesLevelGeometry::SpawnRuntimeLevel(World,
		[&Floor](AWoTRuntimeBSPWorld& Owner)
		{ Floor = WoTSwapPlacesLevelGeometry::AddFloor(&Owner, false); });
	if (!TestNotNull(TEXT("runtime level geometry spawned"), Level)
		|| !TestNotNull(TEXT("runtime level floor"), Floor)) return false;
	TestTrue(TEXT("the floor is part of the level geometry actor's hierarchy"),
		Floor->GetAttachParent() == Level->GetRootComponent());
	TestEqual(TEXT("the floor was promoted to the root's mobility"),
		int32(Floor->Mobility), int32(EComponentMobility::Movable));
	WoTSwapPlacesLevelGeometry::Run(*this, World, Level, Floor);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesImportedLevelGeometrySpec,
	"WoT.Inventory.SwapPlacesLostTarget.ImportedLevelGeometryStaysPut",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesImportedLevelGeometrySpec::RunTest(const FString&)
{
	// Editor maps import level geometry as Static StaticMeshActors at the origin
	// (Tools/import/import_map.py _spawn_world_meshes).
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AStaticMeshActor* Level = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FTransform::Identity,
		WoTSwapPlacesLevelGeometry::AlwaysSpawn());
	if (!TestNotNull(TEXT("imported level geometry spawned"), Level)) return false;
	Level->GetRootComponent()->SetMobility(EComponentMobility::Static);
	WoTSwapPlacesLevelGeometry::Run(*this, World, Level,
		WoTSwapPlacesLevelGeometry::AddFloor(Level));
	return !HasAnyErrors();
}

// The level the original names is its LevelInfo, a static actor standing where
// the map's author left it. The caster's move there either fails in solid
// space (SwapPlacesEffect.uc:164-176) or succeeds, after which the level's own
// move fails and the caster is put back (uc:178-191). Either way nothing else
// happens, and the player is told which end did not fit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesLevelInfoMessageSpec,
	"WoT.Inventory.SwapPlacesLostTarget.LevelInfoDecidesTheMessage",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesLevelInfoMessageSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	FWoTRetailText::EnsureLoaded();
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	UBoxComponent* Floor = nullptr;
	// A floor and, off to one side, a solid block of level geometry.
	AWoTRuntimeBSPWorld* Level = SpawnRuntimeLevel(World,
		[&Floor](AWoTRuntimeBSPWorld& Owner)
		{
			Floor = AddFloor(&Owner, false);
			AddPart(&Owner, TEXT("SolidBlock"), FVector(2500, 2500, 500),
				FVector(400, 400, 400), false);
		});
	if (!TestNotNull(TEXT("runtime level"), Level)) return false;
	TStrongObjectPtr<UWoTMapLightTable> Table(NewObject<UWoTMapLightTable>());
	UWoTMeshLight::RegisterRuntimeMapLightTable(World, Table.Get());
	UWoTZoneIndex* Zones = World->GetSubsystem<UWoTZoneIndex>();
	if (!TestNotNull(TEXT("zone index"), Zones)) return false;

	FCaster Caster = SpawnCaster(World, FVector(1500, 700, 150), 90.0f, false);
	if (!TestNotNull(TEXT("caster"), Caster.Pawn)
		|| !TestNotNull(TEXT("caster's artifact"), Caster.Artifact)) return false;
	WoTTest::TickWorld(World, 1.0f, Step);
	if (!TestFalse(TEXT("caster stands on the floor"),
		Caster.Pawn->GetCharacterMovement()->IsFalling())) return false;

	struct FCase
	{
		const TCHAR* Label;
		FVector LevelInfoAt;
		const TCHAR* MessageKey;
	};
	const FCase Cases[] = {
		// The caster fits at the LevelInfo, so it is the level that cannot move.
		{TEXT("LevelInfo in open space"), FVector(-1500, -1500, 300),
			TEXT("DestinationFailMessage")},
		// The LevelInfo sits in solid space, so the caster cannot move there.
		{TEXT("LevelInfo in solid space"), FVector(2500, 2500, 500),
			TEXT("SourceFailMessage")},
	};
	for (const FCase& Case : Cases)
	{
		Table->bHasLevelInfoLocation = true;
		Table->LevelInfoLocation = Case.LevelInfoAt;
		const FVector CasterBefore = Caster.Pawn->GetActorLocation();
		const FRotator CasterRotationBefore = Caster.Pawn->GetActorRotation();
		const FRotator ViewBefore = Caster.Controller->GetControlRotation();
		const int32 MessagesBefore = Caster.Controller->CenterMessages.Num();
		const int32 RelocationsBefore = Zones->UpdateCountForTest(Caster.Pawn);
		AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster,
			CasterBefore + FVector(0, 300, 0));
		if (!TestNotNull(TEXT("swap projectile"), Swap)) return false;
		const FVector Impact(CasterBefore.X, CasterBefore.Y + 300, 0);
		const FHitResult Hit(Level, Floor, Impact, FVector::UpVector);
		Swap->NotifyHit(nullptr, Level, Floor, true, Impact, FVector::UpVector,
			FVector::ZeroVector, Hit);
		const FString Label(Case.Label);
		TestTrue(Label + TEXT(": the level stays put"),
			Level->GetActorLocation().IsNearlyZero(0.01)
			&& Level->GetActorRotation().IsNearlyZero(0.01));
		TestTrue(FString::Printf(TEXT("%s: the caster stays put (now %s, was %s)"),
			Case.Label, *Caster.Pawn->GetActorLocation().ToString(),
			*CasterBefore.ToString()),
			Caster.Pawn->GetActorLocation().Equals(CasterBefore, 1.0));
		TestTrue(Label + TEXT(": the caster keeps its facing and view"),
			Caster.Pawn->GetActorRotation().Equals(CasterRotationBefore, 0.01)
			&& Caster.Controller->GetControlRotation().Equals(ViewBefore, 0.01));
		// The level can never move, so the port decides before the caster
		// moves at all (PORT_NOTES: a swap that cannot finish).
		TestEqual(Label + TEXT(": the caster never visits the LevelInfo's spot"),
			Zones->UpdateCountForTest(Caster.Pawn) - RelocationsBefore, 0);
		TestEqual(Label + TEXT(": one message"),
			Caster.Controller->CenterMessages.Num(), MessagesBefore + 1);
		ExpectLastMessage(*this, Caster, Case.MessageKey, Label);
		TestFalse(Label + TEXT(": the projectile is spent"), IsValid(Swap));
		WoTTest::TickWorld(World, 0.5f, Step);
		TestFalse(Label + TEXT(": the caster still stands"),
			Caster.Pawn->GetCharacterMovement()->IsFalling());
	}
	return !HasAnyErrors();
}

// The far end cannot move to where the caster stood, after the caster has
// moved: the caster is put back (SwapPlacesEffect.uc:178-191). Here a tall pawn
// cannot fit under the low ceiling the caster stood beneath.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesPutBackSpec,
	"WoT.Inventory.SwapPlacesRules.FarEndThatDoesNotFitPutsCasterBack",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesPutBackSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	FWoTRetailText::EnsureLoaded();
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	// The floor's top is at 0; a ceiling 120 units up covers the caster's spot.
	AWoTRuntimeBSPWorld* Level = SpawnRuntimeLevel(World,
		[](AWoTRuntimeBSPWorld& Owner)
		{
			AddFloor(&Owner, false);
			AddPart(&Owner, TEXT("LowCeiling"), FVector(0, 0, 170),
				FVector(300, 300, 50), false);
		});
	if (!TestNotNull(TEXT("runtime level"), Level)) return false;

	FCaster Caster = SpawnCaster(World, FVector(0, 0, 48.15f), 0.0f, true);
	AWoTCharacter* Tall = World->SpawnActor<AWoTCharacter>(
		FVector(800, 0, 90.15f), FRotator(0, 180, 0), AlwaysSpawn());
	if (!TestNotNull(TEXT("caster"), Caster.Pawn)
		|| !TestNotNull(TEXT("tall target"), Tall)) return false;
	Tall->GetCapsuleComponent()->SetCapsuleSize(30.0f, 88.0f);
	Tall->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTSeekerProjectile* OnCaster = World->SpawnActor<AWoTSeekerProjectile>(
		AWoTSeekerProjectile::StaticClass(), FVector(-1500, 1500, 300),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("seeker"), OnCaster)) return false;
	OnCaster->SetDestination(Caster.Pawn);

	const FVector CasterBefore = Caster.Pawn->GetActorLocation();
	const FVector TallBefore = Tall->GetActorLocation();
	UWoTZoneIndex* Zones = World->GetSubsystem<UWoTZoneIndex>();
	if (!TestNotNull(TEXT("zone index"), Zones)) return false;
	const int32 RelocationsBefore = Zones->UpdateCountForTest(Caster.Pawn);
	AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster, TallBefore - FVector(40, 0, 0));
	if (!TestNotNull(TEXT("swap projectile"), Swap)) return false;
	Swap->SetDestination(Tall);
	Swap->Explode(Swap->GetActorLocation(), FVector::UpVector);

	TestTrue(FString::Printf(TEXT("the caster is put back (now %s, was %s)"),
		*Caster.Pawn->GetActorLocation().ToString(), *CasterBefore.ToString()),
		Caster.Pawn->GetActorLocation().Equals(CasterBefore, 1.0));
	TestEqual(TEXT("the caster went to the target's spot first, then came back"),
		Zones->UpdateCountForTest(Caster.Pawn) - RelocationsBefore, 2);
	TestTrue(FString::Printf(TEXT("the tall pawn stays where it fits (now %s, was %s)"),
		*Tall->GetActorLocation().ToString(), *TallBefore.ToString()),
		Tall->GetActorLocation().Equals(TallBefore, 1.0));
	TestTrue(TEXT("nobody turns"),
		FMath::IsNearlyZero(Caster.Pawn->GetActorRotation().Yaw, 0.01f)
		&& FMath::IsNearlyEqual(Tall->GetActorRotation().Yaw, 180.0f, 0.01f));
	TestTrue(TEXT("the seeker on the caster keeps its target"),
		OnCaster->Destination == Caster.Pawn);
	TestEqual(TEXT("a refused swap shows no flashes"), Swap->LastSwapFlashCount, 0);
	ExpectLastMessage(*this, Caster, TEXT("DestinationFailMessage"),
		TEXT("the target does not fit where the caster stood"));
	return !HasAnyErrors();
}

// A swap onto a projectile: the original turns the far end's collision off
// before the caster moves into its place (SwapPlacesEffect.uc:160), so the
// projectile never touches the caster; it moves to where the caster stood, and
// a seeker that was after the caster is redirected to the far end, which here
// is itself (uc:229-237), so it detonates there, away from the caster.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesOntoProjectileSpec,
	"WoT.Inventory.SwapPlacesRules.OntoAProjectile",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesOntoProjectileSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FCaster Caster = SpawnCaster(World, FVector(0, 0, 300), 0.0f, true);
	if (!TestNotNull(TEXT("caster"), Caster.Pawn)) return false;
	AWoTSeekerProjectile* Incoming = World->SpawnActor<AWoTSeekerProjectile>(
		AWoTSeekerProjectile::StaticClass(), FVector(400, 0, 300),
		FRotator(0, 180, 0), AlwaysSpawn());
	if (!TestNotNull(TEXT("incoming seeker"), Incoming)) return false;
	Incoming->SetDestination(Caster.Pawn);
	const int32 HealthBefore = Caster.Pawn->Health;
	const FVector CasterBefore = Caster.Pawn->GetActorLocation();
	const FVector IncomingBefore = Incoming->GetActorLocation();

	AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster,
		IncomingBefore - FVector(8, 0, 0));
	if (!TestNotNull(TEXT("swap projectile"), Swap)) return false;
	Swap->SetDestination(Incoming);
	Swap->Explode(Swap->GetActorLocation(), FVector::UpVector);

	TestTrue(FString::Printf(TEXT("the caster takes the projectile's place (now %s)"),
		*Caster.Pawn->GetActorLocation().ToString()),
		Caster.Pawn->GetActorLocation().Equals(IncomingBefore, 1.0));
	if (TestTrue(TEXT("the projectile survives the caster's arrival"), IsValid(Incoming)))
	{
		TestTrue(FString::Printf(TEXT("the projectile takes the caster's place (now %s)"),
			*Incoming->GetActorLocation().ToString()),
			Incoming->GetActorLocation().Equals(CasterBefore, 1.0));
		TestTrue(TEXT("the seeker that was after the caster now seeks the far end, itself"),
			Incoming->Destination == Incoming);
	}
	TestEqual(TEXT("the caster is not hit by the projectile it swapped with"),
		Caster.Pawn->Health, HealthBefore);
	TestEqual(TEXT("both ends flash"), Swap->LastSwapFlashCount, 2);
	TestTrue(TEXT("no refusal message"), Caster.Controller->CenterMessages.IsEmpty());

	WoTTest::TickWorld(World, 0.2f, Step);
	TestFalse(TEXT("the seeker detonates where the caster stood"), IsValid(Incoming));
	TestEqual(TEXT("the detonation is out of the caster's reach"),
		Caster.Pawn->Health, HealthBefore);
	return !HasAnyErrors();
}

// An ordinary swap between two pawns standing on the level: both move, both
// turn, the seekers trade targets and a harmful lasting effect changes hosts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesPawnsOnLevelSpec,
	"WoT.Inventory.SwapPlacesRules.PawnsOnTheLevelFloor",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesPawnsOnLevelSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTRuntimeBSPWorld* Level = SpawnRuntimeLevel(World,
		[](AWoTRuntimeBSPWorld& Owner) { AddFloor(&Owner, false); });
	if (!TestNotNull(TEXT("runtime level"), Level)) return false;

	FCaster Caster = SpawnCaster(World, FVector(-400, 0, 150), 0.0f, false);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(400, 0, 150), FRotator(0, 180, 0), AlwaysSpawn());
	if (!TestNotNull(TEXT("caster"), Caster.Pawn)
		|| !TestNotNull(TEXT("target"), Target)) return false;
	Target->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(World, 1.0f, Step);
	if (!TestFalse(TEXT("both stand on the floor"),
		Caster.Pawn->GetCharacterMovement()->IsFalling()
		|| Target->GetCharacterMovement()->IsFalling())) return false;

	AWoTSeekerProjectile* OnCaster = World->SpawnActor<AWoTSeekerProjectile>(
		AWoTSeekerProjectile::StaticClass(), FVector(0, 1500, 600),
		FRotator::ZeroRotator, AlwaysSpawn());
	AWoTSeekerProjectile* OnTarget = World->SpawnActor<AWoTSeekerProjectile>(
		AWoTSeekerProjectile::StaticClass(), FVector(0, -1500, 600),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("seekers"), OnCaster) || !TestNotNull(TEXT("seekers"), OnTarget))
		return false;
	OnCaster->SetDestination(Caster.Pawn);
	OnTarget->SetDestination(Target);
	UWoTTestLeech* Harm = NewObject<UWoTTestLeech>(Target);
	Harm->bDeleterious = true;
	Harm->AttachTo(Target);

	const FVector CasterBefore = Caster.Pawn->GetActorLocation();
	const FVector TargetBefore = Target->GetActorLocation();
	AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster, TargetBefore - FVector(30, 0, 0));
	if (!TestNotNull(TEXT("swap projectile"), Swap)) return false;
	Swap->SetDestination(Target);
	Swap->Explode(Swap->GetActorLocation(), FVector::UpVector);

	TestTrue(FString::Printf(TEXT("the caster takes the target's place (now %s, target was %s)"),
		*Caster.Pawn->GetActorLocation().ToString(), *TargetBefore.ToString()),
		Caster.Pawn->GetActorLocation().Equals(TargetBefore, 3.0));
	TestTrue(FString::Printf(TEXT("the target takes the caster's place (now %s, caster was %s)"),
		*Target->GetActorLocation().ToString(), *CasterBefore.ToString()),
		Target->GetActorLocation().Equals(CasterBefore, 3.0));
	TestTrue(TEXT("the caster now faces where the target faced"),
		FMath::IsNearlyZero(FRotator::NormalizeAxis(
			Caster.Pawn->GetActorRotation().Yaw - 180.0f), 0.5f));
	TestTrue(TEXT("the caster's view turns with it"),
		FMath::IsNearlyZero(FRotator::NormalizeAxis(
			Caster.Controller->GetControlRotation().Yaw - 180.0f), 0.5f));
	TestTrue(TEXT("the target now faces where the caster faced"),
		FMath::IsNearlyZero(FRotator::NormalizeAxis(Target->GetActorRotation().Yaw), 0.5f));
	TestTrue(TEXT("the seeker after the caster now seeks the target"),
		OnCaster->Destination == Target);
	TestTrue(TEXT("the seeker after the target now seeks the caster"),
		OnTarget->Destination == Caster.Pawn);
	TestTrue(TEXT("the harmful effect moves onto the caster"), Harm->GetOwner() == Caster.Pawn);
	TestEqual(TEXT("both ends flash"), Swap->LastSwapFlashCount, 2);
	TestTrue(TEXT("no refusal message"), Caster.Controller->CenterMessages.IsEmpty());
	TestTrue(TEXT("the level stays put"), Level->GetActorLocation().IsNearlyZero(0.01));
	WoTTest::TickWorld(World, 1.0f, Step);
	TestFalse(TEXT("both still stand a second later"),
		Caster.Pawn->GetCharacterMovement()->IsFalling()
		|| Target->GetCharacterMovement()->IsFalling());
	return !HasAnyErrors();
}

// The original's own check (SwapPlacesEffect.uc:60-64): a caster that swaps
// with a frozen target ends up in the ice, frozen, and the target ends up where
// the caster stood, free. The ice in the way does not refuse the caster's move;
// only a mover would.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesFrozenTargetSpec,
	"WoT.Inventory.SwapPlacesRules.OntoAFrozenTarget",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesFrozenTargetSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FCaster Caster = SpawnCaster(World, FVector(0, 0, 300), 0.0f, true);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(400, 0, 300), FRotator(0, 180, 0), AlwaysSpawn());
	if (!TestNotNull(TEXT("caster"), Caster.Pawn)
		|| !TestNotNull(TEXT("target"), Target)) return false;
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTIceLeech* Ice = NewObject<UWoTIceLeech>(Target);
	Ice->AttachTo(Target);
	if (!TestTrue(TEXT("the target is frozen in a block of ice"),
		IsValid(Ice->IceBlock.Get()))) return false;

	const FVector CasterBefore = Caster.Pawn->GetActorLocation();
	const FVector TargetBefore = Target->GetActorLocation();
	// Clear of the ice, which a projectile with no target would strike instead.
	AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster, TargetBefore - FVector(200, 0, 0));
	if (!TestNotNull(TEXT("swap projectile"), Swap)) return false;
	Swap->SetDestination(Target);
	Swap->Explode(Swap->GetActorLocation(), FVector::UpVector);

	TestTrue(FString::Printf(TEXT("the caster ends up in the ice (at %s)"),
		*Caster.Pawn->GetActorLocation().ToString()),
		Caster.Pawn->GetActorLocation().Equals(TargetBefore, 1.0));
	TestTrue(FString::Printf(TEXT("the target ends up where the caster stood (at %s)"),
		*Target->GetActorLocation().ToString()),
		Target->GetActorLocation().Equals(CasterBefore, 1.0));
	TestTrue(TEXT("the caster is frozen now"), Ice->GetOwner() == Caster.Pawn);
	TestNull(TEXT("the target is free"), Target->FindComponentByClass<UWoTIceLeech>());
	TestTrue(TEXT("no refusal message"), Caster.Controller->CenterMessages.IsEmpty());
	return !HasAnyErrors();
}

// Map-placed blockers and static decorations are static actors in the
// original: the caster may fit where they stand, but their own move fails, so
// the caster stays and is told the target does not fit
// (SwapPlacesEffect.uc:178-191). A decoration that is not static swaps.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwapPlacesStaticActorsSpec,
	"WoT.Inventory.SwapPlacesRules.StaticActorsStayPut",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTSwapPlacesStaticActorsSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	FWoTRetailText::EnsureLoaded();
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FCaster Caster = SpawnCaster(World, FVector(0, 0, 300), 0.0f, true);
	if (!TestNotNull(TEXT("caster"), Caster.Pawn)) return false;

	AWoTCollisionBlocker* Blocker = World->SpawnActor<AWoTCollisionBlocker>(
		FVector(600, 0, 300), FRotator::ZeroRotator, AlwaysSpawn());
	AWoTDecoration* Lamp = World->SpawnActor<AWoTDecoration>(
		FVector(0, 600, 300), FRotator::ZeroRotator, AlwaysSpawn());
	AWoTBounceableDecoration* Loose = World->SpawnActor<AWoTBounceableDecoration>(
		FVector(-600, 0, 300), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("blocker"), Blocker) || !TestNotNull(TEXT("lamp"), Lamp)
		|| !TestNotNull(TEXT("loose decoration"), Loose)) return false;
	Blocker->Configure(EWoTCollisionBlockerMode::All, 40.0f, 40.0f);
	Lamp->ConfigureRuntimeCollision(true, true, true, 15.0f, 20.0f);
	TestTrue(TEXT("a placed blocker is static"), Blocker->bStatic);
	TestTrue(TEXT("a placed decoration is static by default"), Lamp->bStatic);
	TestFalse(TEXT("a bounceable decoration is not static"), Loose->bStatic);

	auto Strike = [&](AActor* Far, bool bWall)
	{
		AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster,
			Far->GetActorLocation() - FVector(0, 0, 80));
		if (!Swap) return;
		UPrimitiveComponent* Part = Cast<UPrimitiveComponent>(Far->GetRootComponent());
		const FVector Impact = Far->GetActorLocation();
		if (bWall)
		{
			const FHitResult Hit(Far, Part, Impact, FVector::UpVector);
			Swap->NotifyHit(nullptr, Far, Part, true, Impact, FVector::UpVector,
				FVector::ZeroVector, Hit);
		}
		else
		{
			Swap->ProcessTouch(Far, Impact);
		}
	};
	for (AActor* Far : {static_cast<AActor*>(Blocker), static_cast<AActor*>(Lamp)})
	{
		const FVector CasterBefore = Caster.Pawn->GetActorLocation();
		const FVector FarBefore = Far->GetActorLocation();
		const int32 MessagesBefore = Caster.Controller->CenterMessages.Num();
		Strike(Far, Far == Blocker);
		const FString Label = Far == Blocker ? TEXT("a blocker") : TEXT("a static decoration");
		TestTrue(FString::Printf(TEXT("%s stays put (now %s)"), *Label,
			*Far->GetActorLocation().ToString()),
			Far->GetActorLocation().Equals(FarBefore, 0.01));
		TestTrue(FString::Printf(TEXT("the caster stays put after hitting %s (now %s)"),
			*Label, *Caster.Pawn->GetActorLocation().ToString()),
			Caster.Pawn->GetActorLocation().Equals(CasterBefore, 1.0));
		TestEqual(Label + TEXT(": one message"),
			Caster.Controller->CenterMessages.Num(), MessagesBefore + 1);
		ExpectLastMessage(*this, Caster, TEXT("DestinationFailMessage"), Label);
	}

	const FVector CasterBefore = Caster.Pawn->GetActorLocation();
	const FVector LooseBefore = Loose->GetActorLocation();
	Strike(Loose, false);
	TestTrue(FString::Printf(TEXT("a decoration that is not static takes the caster's place (now %s)"),
		*Loose->GetActorLocation().ToString()),
		Loose->GetActorLocation().Equals(CasterBefore, 1.0));
	TestTrue(FString::Printf(TEXT("and the caster takes its place (now %s)"),
		*Caster.Pawn->GetActorLocation().ToString()),
		Caster.Pawn->GetActorLocation().Equals(LooseBefore, 1.0));
	return !HasAnyErrors();
}

// The far end does not collide while the two ends trade places
// (SwapPlacesEffect.uc:160, :208-210), but it keeps its size: the zones it is
// in after its move are those of its whole cylinder, its feet a full collision
// height below its middle. A pawn swapped to where the caster stood with its
// feet in water and its middle above it has its feet in the water at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationSwapZoneSpec,
	"WoT.Relocation.SwapFarEndZoneUsesItsFullHeight",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTRelocationSwapZoneSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	UWoTZoneIndex* Zones = World->GetSubsystem<UWoTZoneIndex>();
	// Water from 100 units down up to 20 units up.
	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(
		FVector(0, 0, -40), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("zone index"), Zones) || !TestNotNull(TEXT("water"), Water)) return false;
	Water->Collision->SetBoxExtent(FVector(300, 300, 60));
	Water->bWaterZone = true;

	FCaster Caster = SpawnCaster(World, FVector(0, 0, 50), 0.0f, true);
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(800, 0, 300), FRotator(0, 180, 0), AlwaysSpawn());
	if (!TestNotNull(TEXT("caster"), Caster.Pawn) || !TestNotNull(TEXT("target"), Target))
		return false;
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTZoneIndex::RefreshAfterRelocation(Caster.Pawn);
	UWoTZoneIndex::RefreshAfterRelocation(Target);
	if (!TestTrue(TEXT("the caster's feet are in the water and its middle is not"),
		Zones->GetFootRegion(Caster.Pawn) == Water && Zones->GetRegion(Caster.Pawn) == nullptr)
		|| !TestTrue(TEXT("the target is out of the water"),
			Zones->GetFootRegion(Target) == nullptr)) return false;

	const FVector CasterBefore = Caster.Pawn->GetActorLocation();
	AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster,
		Target->GetActorLocation() - FVector(30, 0, 0));
	if (!TestNotNull(TEXT("swap projectile"), Swap)) return false;
	Swap->SetDestination(Target);
	Swap->Explode(Swap->GetActorLocation(), FVector::UpVector);

	if (!TestTrue(FString::Printf(TEXT("the target takes the caster's place (now %s)"),
		*Target->GetActorLocation().ToString()),
		Target->GetActorLocation().Equals(CasterBefore, 1.0))) return false;
	TestTrue(TEXT("the target's feet are in the water as soon as it arrives"),
		Zones->GetFootRegion(Target) == Water);
	TestTrue(TEXT("and its middle is not"), Zones->GetRegion(Target) == nullptr);
	TestTrue(TEXT("the caster's feet are out of the water"),
		Zones->GetFootRegion(Caster.Pawn) == nullptr);
	return !HasAnyErrors();
}

// A breakable decoration collides with the world and is 29 units each way
// (BreakableDecoration.uc:123-125). Moved to where the caster stood, it has to
// fit the level there like any world-colliding actor (SwapPlacesEffect.uc:178):
// it is nudged out of the wall the caster stood against, and where it fits
// nowhere its move fails and the caster is put back (uc:178-191).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRelocationBreakableFitSpec,
	"WoT.Relocation.BreakableDecorationFitsWhereTheCasterStood",
	WoTSwapPlacesLevelGeometry::Flags)

bool FWoTRelocationBreakableFitSpec::RunTest(const FString&)
{
	using namespace WoTSwapPlacesLevelGeometry;
	FWoTRetailText::EnsureLoaded();
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	// The floor (top at 0), a wall whose face is at x = 100 and, off to one
	// side, a corridor 50 units wide between faces at y = 2975 and y = 3025.
	AWoTRuntimeBSPWorld* Level = SpawnRuntimeLevel(World,
		[](AWoTRuntimeBSPWorld& Owner)
		{
			AddFloor(&Owner, false);
			AddPart(&Owner, TEXT("Wall"), FVector(300, 0, 300),
				FVector(200, 1500, 300), false);
			AddPart(&Owner, TEXT("CorridorLeft"), FVector(-1500, 2875, 300),
				FVector(400, 100, 300), false);
			AddPart(&Owner, TEXT("CorridorRight"), FVector(-1500, 3125, 300),
				FVector(400, 100, 300), false);
		});
	if (!TestNotNull(TEXT("runtime level"), Level)) return false;
	const FVector DecorationExtent(29.0f, 29.0f, 29.0f);

	// The caster stands against the wall: its 17-unit radius touches it.
	FCaster Caster = SpawnCaster(World, FVector(82, 0, 48), 0.0f, true);
	AWoTBreakableDecoration* Vase = World->SpawnActor<AWoTBreakableDecoration>(
		FVector(-600, 0, 29.5f), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("caster"), Caster.Pawn) || !TestNotNull(TEXT("breakable"), Vase))
		return false;
	TestEqual(TEXT("the breakable keeps the original's collision radius"),
		Vase->Collision->GetUnscaledCapsuleRadius(), 29.0f);
	const FVector CasterBefore = Caster.Pawn->GetActorLocation();
	const FVector VaseBefore = Vase->GetActorLocation();
	TestFalse(TEXT("the breakable would stand 12 units inside the wall where the caster is"),
		WoTUE1Collision::LevelPointCheckFits(*World, CasterBefore, DecorationExtent));
	AWoTSwapPlacesProjectile* Swap = SpawnSwap(World, Caster, VaseBefore + FVector(0, 0, 40));
	if (!TestNotNull(TEXT("swap projectile"), Swap)) return false;
	Swap->SetDestination(Vase);
	Swap->Explode(Swap->GetActorLocation(), FVector::UpVector);

	const FVector VaseAfter = Vase->GetActorLocation();
	TestTrue(FString::Printf(TEXT("the caster takes the breakable's place (now %s)"),
		*Caster.Pawn->GetActorLocation().ToString()),
		FVector::Dist2D(Caster.Pawn->GetActorLocation(), VaseBefore) < 1.0);
	TestTrue(FString::Printf(TEXT("the breakable fits the level where it stands (at %s)"),
		*VaseAfter.ToString()),
		WoTUE1Collision::LevelPointCheckFits(*World, VaseAfter, DecorationExtent));
	const double Nudge = CasterBefore.X - VaseAfter.X;
	TestTrue(FString::Printf(TEXT("it is nudged out of the wall by about its overlap, "
		"no further (%.1f units)"), Nudge), Nudge > 10.0 && Nudge < 15.0);
	TestTrue(TEXT("and otherwise stands where the caster stood"),
		FMath::IsNearlyEqual(VaseAfter.Y, CasterBefore.Y, 0.5)
		&& FMath::IsNearlyEqual(VaseAfter.Z, CasterBefore.Z, 0.5));
	TestTrue(TEXT("no refusal message"), Caster.Controller->CenterMessages.IsEmpty());

	// In the corridor the breakable fits nowhere, so its move fails and the
	// caster is put back.
	AWoTBreakableDecoration* Crate = World->SpawnActor<AWoTBreakableDecoration>(
		FVector(-600, 600, 29.5f), FRotator::ZeroRotator, AlwaysSpawn());
	if (!TestNotNull(TEXT("second breakable"), Crate)) return false;
	Caster.Pawn->SetActorLocation(FVector(-1500, 3000, 48), false, nullptr,
		ETeleportType::TeleportPhysics);
	const FVector InCorridor = Caster.Pawn->GetActorLocation();
	const FVector CrateBefore = Crate->GetActorLocation();
	TestTrue(TEXT("the caster fits in the corridor"),
		WoTUE1Collision::LevelPointCheckFits(*World, InCorridor,
			Caster.Pawn->GetSimpleCollisionCylinderExtent()));
	AWoTSwapPlacesProjectile* Refused = SpawnSwap(World, Caster, CrateBefore + FVector(0, 0, 40));
	if (!TestNotNull(TEXT("second swap projectile"), Refused)) return false;
	Refused->SetDestination(Crate);
	Refused->Explode(Refused->GetActorLocation(), FVector::UpVector);
	TestTrue(FString::Printf(TEXT("the caster is put back in the corridor (now %s)"),
		*Caster.Pawn->GetActorLocation().ToString()),
		Caster.Pawn->GetActorLocation().Equals(InCorridor, 1.0));
	TestTrue(FString::Printf(TEXT("the breakable stays where it was (now %s)"),
		*Crate->GetActorLocation().ToString()),
		Crate->GetActorLocation().Equals(CrateBefore, 0.5));
	TestEqual(TEXT("a refused swap shows no flashes"), Refused->LastSwapFlashCount, 0);
	ExpectLastMessage(*this, Caster, TEXT("DestinationFailMessage"),
		TEXT("the breakable does not fit in the corridor"));
	return !HasAnyErrors();
}

#endif
