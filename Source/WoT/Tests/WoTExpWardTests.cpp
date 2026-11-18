// Ward arming and contact rules, checked against the shipped script:
// exports/script/Angreal/AngrealExpWardProjectile.uc, AngrealInvExpWard.uc and
// GenericProjectile.uc, exports/script/WOT/AngrealProjectile.uc.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvExpWard.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTDecal.h"
#include "../WoTEvents.h"
#include "../WoTMover.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerController.h"
#include "../WoTSoundTable.h"
#include "../WoTWeaveFX.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags WardSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters WardSpecSpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

// Level geometry the way imported maps carry it: a StaticMeshActor whose
// world-static box blocks every channel.
UBoxComponent* WardSpecLevelBox(UWorld& World, const FVector& Center,
	const FVector& Extent, const TCHAR* Name)
{
	AStaticMeshActor* Level = World.SpawnActor<AStaticMeshActor>(
		Center, FRotator::ZeroRotator, WardSpecSpawnParams());
	if (!Level)
	{
		return nullptr;
	}
	UBoxComponent* Box = NewObject<UBoxComponent>(Level, Name);
	Box->SetupAttachment(Level->GetRootComponent());
	Box->SetBoxExtent(Extent);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	return Box;
}

// A floor whose top face is z = 0.
void WardSpecFloor(UWorld& World)
{
	WardSpecLevelBox(World, FVector(0, 0, -50), FVector(1000, 1000, 50),
		TEXT("WardSpecFloor"));
}

struct FWardSpecCaster
{
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Pawn = nullptr;
	AWoTAngrealInvExpWard* Artifact = nullptr;
};

// A player pawn holding the real ward artifact.
FWardSpecCaster WardSpecCaster(UWorld& World, const FVector& Location, bool bWalking)
{
	FWardSpecCaster Caster;
	Caster.Controller = World.SpawnActor<AWoTPlayerController>(WardSpecSpawnParams());
	Caster.Pawn = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, WardSpecSpawnParams());
	if (!Caster.Controller || !Caster.Pawn)
	{
		return Caster;
	}
	Caster.Controller->SetAsLocalPlayerController();
	Caster.Controller->Possess(Caster.Pawn);
	if (!bWalking)
	{
		Caster.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	Caster.Artifact = World.SpawnActor<AWoTAngrealInvExpWard>(
		Location, FRotator::ZeroRotator, WardSpecSpawnParams());
	if (Caster.Artifact)
	{
		Caster.Artifact->GiveTo(Caster.Pawn);
		Caster.Artifact->CurCharges = FMath::Max(Caster.Artifact->CurCharges, 20);
	}
	WoTTest::TickWorld(&World, 0.05f);
	Caster.Pawn->Health = 100;
	return Caster;
}

// A pawn that stays where it is put.
AWoTCharacter* WardSpecStillPawn(UWorld& World, const FVector& Location)
{
	AWoTCharacter* Pawn = World.SpawnActor<AWoTCharacter>(
		Location, FRotator::ZeroRotator, WardSpecSpawnParams());
	if (Pawn)
	{
		Pawn->GetCharacterMovement()->DisableMovement();
		Pawn->Health = 100;
	}
	return Pawn;
}

void WardSpecAimAt(AWoTCharacter& Pawn, const FVector& Target)
{
	const FVector Eye = Pawn.GetActorLocation() + FVector(0, 0, Pawn.BaseEyeHeight);
	const FRotator Aim = (Target - Eye).Rotation();
	Pawn.SetActorRotation(FRotator(0, Aim.Yaw, 0));
	if (AController* Controller = Pawn.GetController())
	{
		Controller->SetControlRotation(Aim);
	}
}

// Casts the real artifact at a world point and returns the ward it placed.
AWoTExpWardProjectile* WardSpecCastAt(UWorld& World, const FWardSpecCaster& Caster,
	const FVector& Target)
{
	TSet<const AWoTExpWardProjectile*> Existing;
	for (TActorIterator<AWoTExpWardProjectile> It(&World); It; ++It)
	{
		Existing.Add(*It);
	}
	WardSpecAimAt(*Caster.Pawn, Target);
	Caster.Artifact->UnCastArtifact();
	Caster.Artifact->NextCastTime = -1.0f;
	Caster.Artifact->CastArtifact();
	Caster.Artifact->UnCastArtifact();
	for (TActorIterator<AWoTExpWardProjectile> It(&World); It; ++It)
	{
		if (!Existing.Contains(*It) && It->SourceAngreal == Caster.Artifact)
		{
			return *It;
		}
	}
	return nullptr;
}

// The ward's detonation burst (AngrealExpWardProjectile.uc:145).
int32 WardSpecBlasts(UWorld& World)
{
	int32 Count = 0;
	for (TActorIterator<AWoTExplosionEffect> It(&World); It; ++It)
	{
		Count += It->ConfigName == FName(TEXT("FireballExplode3")) ? 1 : 0;
	}
	return Count;
}

// One real world per case, destroyed with the case.
struct FWardSpecWorld
{
	UWorld* World = WoTTest::CreateSpecWorld();
	~FWardSpecWorld() { WoTTest::DestroySpecWorld(World); }
};

// Floor scene: a flying caster 120 units back from the cast point (0, 0, 0)
// and a still bystander inside the 120-unit splash, outside the 24-unit
// contact radius. The ward lands at (0, 0, 0.5).
struct FWardSpecFloorScene
{
	FWardSpecCaster Caster;
	AWoTCharacter* Bystander = nullptr;
	AWoTExpWardProjectile* Ward = nullptr;
};

bool WardSpecBuildFloorScene(FAutomationTestBase& Test, UWorld& World,
	FWardSpecFloorScene& Scene, bool bWalkingCaster = false)
{
	WardSpecFloor(World);
	Scene.Caster = WardSpecCaster(World, FVector(-120, 0, 48), bWalkingCaster);
	if (!Test.TestNotNull(TEXT("caster holds the real ward artifact"),
		Scene.Caster.Artifact))
	{
		return false;
	}
	Scene.Bystander = WardSpecStillPawn(World, FVector(0, 70, 48));
	Scene.Ward = WardSpecCastAt(World, Scene.Caster, FVector::ZeroVector);
	return Test.TestNotNull(TEXT("real artifact cast places a ward"), Scene.Ward)
		&& Test.TestNotNull(TEXT("bystander spawned"), Scene.Bystander)
		&& Test.TestTrue(TEXT("ward stuck to the floor"),
			Scene.Ward->GetActorLocation().Equals(FVector(0, 0, 0.5), 1.0f));
}

// Wall scene: the caster at eye height 142 fires along +X at a level wall
// whose face is x = 190; the ward lands at (189.5, 0, 142).
struct FWardSpecWallScene
{
	FWardSpecCaster Caster;
	AWoTExpWardProjectile* Ward = nullptr;
};

bool WardSpecBuildWallScene(FAutomationTestBase& Test, UWorld& World,
	FWardSpecWallScene& Scene)
{
	WardSpecLevelBox(World, FVector(200, 0, 150), FVector(10, 300, 300),
		TEXT("WardSpecWall"));
	Scene.Caster = WardSpecCaster(World, FVector(0, 0, 100), false);
	if (!Test.TestNotNull(TEXT("caster holds the real ward artifact"),
		Scene.Caster.Artifact))
	{
		return false;
	}
	Scene.Ward = WardSpecCastAt(World, Scene.Caster, FVector(190, 0, 142));
	return Test.TestNotNull(TEXT("real artifact cast places a wall ward"), Scene.Ward)
		&& Test.TestTrue(TEXT("ward stuck to the wall"),
			Scene.Ward->GetActorLocation().Equals(FVector(189.5, 0, 142), 1.0f));
}

// The caster walks forward until both wards are gone (or two seconds pass).
void WardSpecWalkUntilGone(UWorld& World, AWoTCharacter& Walker,
	const TWeakObjectPtr<AWoTExpWardProjectile>& First,
	const TWeakObjectPtr<AWoTExpWardProjectile>& Second)
{
	for (int32 Frame = 0; Frame < 120 && (First.IsValid() || Second.IsValid()); ++Frame)
	{
		Walker.AddMovementInput(FVector::ForwardVector, 1.0f, true);
		WoTTest::TickWorld(&World, 1, 1.0f / 60.0f);
	}
}

// Placement sounds the server spawned (WoTSound::PlayReplicatedAsset markers).
int32 WardSpecSounds(UWorld& World, const TCHAR* Wave)
{
	int32 Count = 0;
	for (TActorIterator<AWoTSoundBurst> It(&World); It; ++It)
	{
		Count += It->SoundSubPath.ToString().Contains(Wave) ? 1 : 0;
	}
	return Count;
}

const AWoTSoundBurst* WardSpecFindSound(UWorld& World, const TCHAR* Wave)
{
	for (TActorIterator<AWoTSoundBurst> It(&World); It; ++It)
	{
		if (It->SoundSubPath.ToString().Contains(Wave))
		{
			return *It;
		}
	}
	return nullptr;
}

// The ward's detonation presentation (AngrealExpWardProjectile.uc:135-146).
const AWoTExplosionEffect* WardSpecFindBlast(UWorld& World)
{
	for (TActorIterator<AWoTExplosionEffect> It(&World); It; ++It)
	{
		if (It->ConfigName == FName(TEXT("FireballExplode3")))
		{
			return *It;
		}
	}
	return nullptr;
}

const AWoTParticleSprayer* WardSpecFindBlastBits(UWorld& World)
{
	for (TActorIterator<AWoTParticleSprayer> It(&World); It; ++It)
	{
		if (It->ConfigName == FName(TEXT("BlastBits")))
		{
			return *It;
		}
	}
	return nullptr;
}

const AWoTDecal* WardSpecFindScorch(UWorld& World)
{
	for (TActorIterator<AWoTDecal> It(&World); It; ++It)
	{
		if (It->DecalType == FName(TEXT("ScorchMark")))
		{
			return *It;
		}
	}
	return nullptr;
}

// The wall ward at (189.5, 0, 142) has blown: the fireball and blast bits are
// at the ward, the bits and the scorch face the wall's normal (-1, 0, 0), and
// the scorch lies on the wall face x = 190.
void WardSpecExpectBlastOnWall(FAutomationTestBase& Test, UWorld& World,
	const TCHAR* Path)
{
	const FVector WardAt(189.5, 0, 142);
	const FVector WallNormal(-1, 0, 0);
	const AWoTExplosionEffect* Blast = WardSpecFindBlast(World);
	Test.TestTrue(FString::Printf(TEXT("%s: the fireball bursts at the ward (at %s)"),
		Path, Blast ? *Blast->GetActorLocation().ToString() : TEXT("none")),
		Blast && Blast->GetActorLocation().Equals(WardAt, 1.0f));
	const AWoTParticleSprayer* Bits = WardSpecFindBlastBits(World);
	Test.TestTrue(FString::Printf(TEXT("%s: the blast bits fly off the wall from the ward (at %s)"),
		Path, Bits ? *Bits->GetActorLocation().ToString() : TEXT("none")),
		Bits && Bits->GetActorLocation().Equals(WardAt, 1.0f)
			&& Bits->GetActorRotation().Equals(WallNormal.Rotation(), 0.01f));
	const AWoTDecal* Scorch = WardSpecFindScorch(World);
	Test.TestTrue(FString::Printf(TEXT("%s: the scorch lands on the wall (at %s)"),
		Path, Scorch ? *Scorch->GetActorLocation().ToString() : TEXT("none")),
		Scorch && Scorch->PlacedNormal.Equals(WallNormal, 0.01f)
			&& Scorch->GetActorLocation().Equals(FVector(189, 0, 142), 1.0f));
}

#if WITH_EDITOR
// Net test worlds load /Engine/Maps/Entry outside the normal bootstrap; a
// standalone spec world holds the gameplay activation while they run.
struct FWardSpecNetActivation
{
	UWorld* World = WoTTest::CreateSpecWorld();
	~FWardSpecNetActivation() { WoTTest::DestroySpecWorld(World); }
};

// A dedicated server with one client: the server places a ward on a
// server-only floor 300 units ahead of the client's pawn. Returns the
// client's copy once it has arrived.
AWoTExpWardProjectile* WardSpecNetPlaceWard(UE::Net::FTestWorlds& Worlds,
	AWoTExpWardProjectile*& OutServerWard)
{
	OutServerWard = nullptr;
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	APlayerController* Controller = Worlds.GetServerPlayerControllerOfClient(0);
	const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	const FVector Feet = Pawn
		? Pawn->GetActorLocation() - FVector(0, 0, Pawn->GetSimpleCollisionHalfHeight() + 2.0f)
		: FVector::ZeroVector;
	WardSpecLevelBox(*ServerWorld, Feet - FVector(0, 0, 50), FVector(1000, 1000, 50),
		TEXT("WardSpecNetFloor"));
	const FVector Spot = Feet + FVector(300, 0, 0);
	OutServerWard = ServerWorld->SpawnActor<AWoTExpWardProjectile>(
		Spot + FVector(0, 0, 100), FRotator(-90, 0, 0), WardSpecSpawnParams());
	if (!OutServerWard)
	{
		return nullptr;
	}
	OutServerWard->ForceNetUpdate();
	AWoTExpWardProjectile* ClientWard = nullptr;
	Worlds.TickUntil([&Worlds, &ClientWard]()
	{
		for (TActorIterator<AWoTExpWardProjectile> It(Worlds.Clients[0].GetWorld()); It; ++It)
		{
			ClientWard = *It;
		}
		return ClientWard != nullptr;
	}, 120);
	return ClientWard;
}
#endif
}

// Waiting ignores Touch, ProcessTouch, TakeDamage and Explode for the first
// second (AngrealExpWardProjectile.uc:179-188). No caller detonates an unarmed
// ward; an expiry in that state stays the plain destroy (uc:172-176).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardWaitingIgnoresDetonationSpec,
	"WoT.ExpWard.WaitingIgnoresDetonation", WardSpecFlags)
bool FWoTExpWardWaitingIgnoresDetonationSpec::RunTest(const FString&)
{
	// An explicit trigger during the first second does nothing; the same
	// trigger detonates the ward once it is armed (uc:125-128).
	{
		FWardSpecWorld Case;
		FWardSpecFloorScene Scene;
		if (WardSpecBuildFloorScene(*this, *Case.World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> Ward = Scene.Ward;
			TestFalse(TEXT("a fresh ward is still waiting"), Scene.Ward->bArmed);
			Scene.Ward->WoTTrigger(nullptr, nullptr);
			WoTTest::TickWorld(Case.World, 0.5f);
			if (Ward.IsValid())
			{
				Ward->WoTTrigger(nullptr, Scene.Caster.Pawn);
			}
			WoTTest::TickWorld(Case.World, 0.1f);
			TestTrue(TEXT("triggers during the first second leave the ward in place"),
				Ward.IsValid());
			TestEqual(TEXT("triggers during the first second raise no blast"),
				WardSpecBlasts(*Case.World), 0);
			TestEqual(TEXT("triggers during the first second splash nobody"),
				Scene.Bystander->Health, 100);
			WoTTest::TickWorld(Case.World, 0.5f);
			if (TestTrue(TEXT("ward arms after its activation time"),
				Ward.IsValid() && Ward->bArmed))
			{
				Ward->WoTTrigger(nullptr, nullptr);
				TestFalse(TEXT("a trigger detonates the armed ward"), Ward.IsValid());
				TestEqual(TEXT("the armed detonation raises the blast"),
					WardSpecBlasts(*Case.World), 1);
				TestTrue(TEXT("the armed detonation splashes the bystander"),
					Scene.Bystander->Health < 100);
			}
		}
	}

	// A level-scripted ward triggered through its Tag by the event system.
	{
		FWardSpecWorld Case;
		WardSpecFloor(*Case.World);
		const FName Tag(TEXT("WardSpecTag"));
		const FTransform At(FRotator(-90, 0, 0), FVector(300, 0, 100));
		AWoTExpWardProjectile* Tagged =
			Case.World->SpawnActorDeferred<AWoTExpWardProjectile>(
				AWoTExpWardProjectile::StaticClass(), At, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		UWoTEventSubsystem* Events = Case.World->GetSubsystem<UWoTEventSubsystem>();
		if (TestNotNull(TEXT("tagged ward spawned"), Tagged)
			&& TestNotNull(TEXT("event system"), Events))
		{
			Tagged->TagName = Tag;
			Tagged->FinishSpawning(At);
			TWeakObjectPtr<AWoTExpWardProjectile> Ward = Tagged;
			TestTrue(TEXT("tagged ward placed on the floor"), Tagged->bCorrectlyPlaced);
			Events->FireEvent(Tag, nullptr, nullptr);
			TestTrue(TEXT("an event during the first second leaves the ward in place"),
				Ward.IsValid());
			WoTTest::TickWorld(Case.World, 1.1f);
			if (TestTrue(TEXT("tagged ward arms"), Ward.IsValid() && Ward->bArmed))
			{
				Events->FireEvent(Tag, nullptr, nullptr);
				TestFalse(TEXT("the event detonates the armed ward"), Ward.IsValid());
			}
		}
	}

	// Explode itself is ignored while waiting, whatever calls it.
	{
		FWardSpecWorld Case;
		FWardSpecFloorScene Scene;
		if (WardSpecBuildFloorScene(*this, *Case.World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> Ward = Scene.Ward;
			Scene.Ward->Explode(Scene.Ward->GetActorLocation(), Scene.Ward->RelativeUp);
			TestTrue(TEXT("Explode during the first second leaves the ward in place"),
				Ward.IsValid());
			TestEqual(TEXT("Explode during the first second raises no blast"),
				WardSpecBlasts(*Case.World), 0);
			TestEqual(TEXT("Explode during the first second splashes nobody"),
				Scene.Bystander->Health, 100);
		}
	}

	// Damage is ignored while waiting.
	{
		FWardSpecWorld Case;
		FWardSpecFloorScene Scene;
		if (WardSpecBuildFloorScene(*this, *Case.World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> Ward = Scene.Ward;
			Scene.Ward->TakeDamage(10.0f, FDamageEvent(), nullptr, nullptr);
			TestTrue(TEXT("damage during the first second leaves the ward in place"),
				Ward.IsValid());
			TestEqual(TEXT("damage during the first second raises no blast"),
				WardSpecBlasts(*Case.World), 0);
		}
	}

	// A lifespan that runs out while waiting removes the ward without a blast.
	{
		FWardSpecWorld Case;
		FWardSpecFloorScene Scene;
		if (WardSpecBuildFloorScene(*this, *Case.World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> Ward = Scene.Ward;
			Scene.Ward->SetLifeSpan(0.2f);
			WoTTest::TickWorld(Case.World, 0.35f);
			TestFalse(TEXT("the expired waiting ward is gone"), Ward.IsValid());
			TestEqual(TEXT("the expiry while waiting raises no blast"),
				WardSpecBlasts(*Case.World), 0);
			TestEqual(TEXT("the expiry while waiting splashes nobody"),
				Scene.Bystander->Health, 100);
		}
	}
	return !HasAnyErrors();
}

// PHYS_None with bCollideWorld=False (AngrealExpWardProjectile.uc:279,289): the
// ward never moves itself and nothing in the world blocks against it, so a
// world contact is never a wall impact. It stays a projectile target (uc:290).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardWorldContactSpec,
	"WoT.ExpWard.WorldContactNeverDetonates", WardSpecFlags)
bool FWoTExpWardWorldContactSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	FWardSpecFloorScene Scene;
	if (!WardSpecBuildFloorScene(*this, *Case.World, Scene))
	{
		return false;
	}
	AWoTExpWardProjectile* Ward = Scene.Ward;
	TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Ward;
	TestFalse(TEXT("the placed ward runs no projectile movement"),
		Ward->ProjectileMovement->IsActive());

	WoTTest::TickWorld(Case.World, 1.1f);
	if (!TestTrue(TEXT("ward armed"), WardPtr.IsValid() && Ward->bArmed))
	{
		return false;
	}
	const UPrimitiveComponent* Cylinder = Ward->GetActiveSweptCollision();
	if (TestNotNull(TEXT("ward collision cylinder"), Cylinder))
	{
		const ECollisionChannel Channels[] = {ECC_WorldStatic, ECC_WorldDynamic,
			ECC_Pawn, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible,
			ECC_GameTraceChannel1};
		for (const ECollisionChannel Channel : Channels)
		{
			TestNotEqual(FString::Printf(TEXT("the armed ward blocks nothing (channel %d)"),
				static_cast<int32>(Channel)),
				static_cast<int32>(Cylinder->GetCollisionResponseToChannel(Channel)),
				static_cast<int32>(ECR_Block));
		}
	}

	// A slab of level geometry swept across the armed ward (clear of the
	// bystander and the caster).
	UBoxComponent* Slab = WardSpecLevelBox(*Case.World, FVector(200, 0, 55),
		FVector(20, 30, 50), TEXT("WardSpecSlab"));
	if (!TestNotNull(TEXT("level slab"), Slab))
	{
		return false;
	}
	FHitResult SweepHit;
	Slab->SetWorldLocation(FVector(0, 0, 55), true, &SweepHit);
	WoTTest::TickWorld(Case.World, 0.1f);
	TestTrue(TEXT("a world contact leaves the armed ward in place"), WardPtr.IsValid());
	TestEqual(TEXT("a world contact raises no blast"), WardSpecBlasts(*Case.World), 0);
	TestFalse(TEXT("the ward does not block world geometry"),
		SweepHit.bBlockingHit && SweepHit.GetActor() == Ward);
	TestTrue(TEXT("the slab passed onto the ward"),
		Slab->GetComponentLocation().Equals(FVector(0, 0, 55), 0.5f));

	if (WardPtr.IsValid())
	{
		Ward->TakeDamage(10.0f, FDamageEvent(), nullptr, nullptr);
		TestFalse(TEXT("the armed ward still detonates when shot"), WardPtr.IsValid());
	}
	return !HasAnyErrors();
}

// Touch qualifies only an actor with bProjTarget, or with both bBlockActors
// and bBlockPlayers (AngrealProjectile.uc:84), and only actors in the
// collision hash raise Touch at all. Level geometry and collision-disabled
// actors never trigger the ward, and eligibility is rechecked when an actor
// that was already inside at arming moves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardIneligibleContactSpec,
	"WoT.ExpWard.IneligibleContactsNeverTrigger", WardSpecFlags)
bool FWoTExpWardIneligibleContactSpec::RunTest(const FString&)
{
	// Level geometry beside a lift: the ward rides the lift past a static ledge
	// whose bounds reach its cylinder. Editor worlds also hold the default BSP
	// brush and the gameplay debugger's replicator (a render-only primitive with
	// world-sized bounds) at the origin; neither can raise Touch either.
	{
		FWardSpecWorld Case;
		UWorld& World = *Case.World;
		AWoTMover* Lift = World.SpawnActor<AWoTMover>(
			FVector::ZeroVector, FRotator::ZeroRotator, WardSpecSpawnParams());
		AStaticMeshActor* Ledge = World.SpawnActor<AStaticMeshActor>(
			FVector(60, 0, 10), FRotator::ZeroRotator, WardSpecSpawnParams());
		UStaticMesh* Cube = Cast<UStaticMesh>(
			FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad());
		if (TestNotNull(TEXT("lift"), Lift) && TestNotNull(TEXT("ledge"), Ledge)
			&& TestNotNull(TEXT("engine cube mesh"), Cube))
		{
			UBoxComponent* Deck = NewObject<UBoxComponent>(Lift, TEXT("WardSpecDeck"));
			Deck->SetupAttachment(Lift->GetRootComponent());
			Deck->SetBoxExtent(FVector(25, 25, 10));
			Deck->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Deck->SetCollisionObjectType(ECC_WorldDynamic);
			Deck->SetCollisionResponseToAllChannels(ECR_Block);
			Deck->RegisterComponent();
			Ledge->SetMobility(EComponentMobility::Movable);
			Ledge->GetStaticMeshComponent()->SetStaticMesh(Cube);
			Ledge->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
			Ledge->SetActorScale3D(FVector(0.6f, 0.6f, 0.2f));

			FWardSpecCaster Caster = WardSpecCaster(World, FVector(-120, 0, 60), false);
			AWoTExpWardProjectile* Ward = Caster.Artifact
				? WardSpecCastAt(World, Caster, FVector(0, 0, 10)) : nullptr;
			if (TestNotNull(TEXT("real artifact cast places a ward on the lift"), Ward))
			{
				TestTrue(TEXT("ward is based on the lift"),
					Ward->GetAttachParentActor() == Lift);
				TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Ward;
				WoTTest::TickWorld(&World, 1.1f);
				TestTrue(TEXT("ward armed beside the ledge"),
					WardPtr.IsValid() && Ward->bArmed);
				Lift->AddActorWorldOffset(FVector(0, 0, 5), false, nullptr,
					ETeleportType::TeleportPhysics);
				WoTTest::TickWorld(&World, 0.1f);
				TestTrue(TEXT("level geometry never triggers the ward"), WardPtr.IsValid());
				TestEqual(TEXT("level geometry raises no blast"), WardSpecBlasts(World), 0);
			}
		}
	}

	// A collision-disabled pawn inside the cylinder when the ward arms (wall
	// ward at chest height, the pawn's centre within the 24-unit radius).
	{
		FWardSpecWorld Case;
		UWorld& World = *Case.World;
		AWoTCharacter* Ghost = WardSpecStillPawn(World, FVector(170, 0, 142));
		FWardSpecWallScene Scene;
		if (TestNotNull(TEXT("ghost pawn"), Ghost))
		{
			Ghost->SetActorEnableCollision(false);
			if (WardSpecBuildWallScene(*this, World, Scene))
			{
				TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
				WoTTest::TickWorld(&World, 1.1f);
				TestTrue(TEXT("ward armed over the ghost"),
					WardPtr.IsValid() && Scene.Ward->bArmed);
				Ghost->SetActorLocation(Ghost->GetActorLocation() + FVector(-1, 0, 0));
				WoTTest::TickWorld(&World, 0.1f);
				TestTrue(TEXT("a collision-disabled actor never triggers the ward"),
					WardPtr.IsValid());
				TestEqual(TEXT("the ghost was not hit"), Ghost->Health, 100);
			}
		}
	}

	// A pawn inside the cylinder at arming loses its collision (as a dying
	// pawn does), moves, regains collision and moves again.
	{
		FWardSpecWorld Case;
		UWorld& World = *Case.World;
		AWoTCharacter* Occupant = WardSpecStillPawn(World, FVector(170, 0, 142));
		FWardSpecWallScene Scene;
		if (TestNotNull(TEXT("occupant pawn"), Occupant)
			&& WardSpecBuildWallScene(*this, World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
			UCapsuleComponent* Capsule = Occupant->GetCapsuleComponent();
			const ECollisionEnabled::Type Collision = Capsule->GetCollisionEnabled();
			WoTTest::TickWorld(&World, 1.1f);
			TestTrue(TEXT("ward armed around the occupant"),
				WardPtr.IsValid() && Scene.Ward->bArmed);
			TestTrue(TEXT("arming alone does not touch the occupant"), WardPtr.IsValid());

			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Occupant->SetActorLocation(Occupant->GetActorLocation() + FVector(-1, 0, 0));
			WoTTest::TickWorld(&World, 0.1f);
			TestTrue(TEXT("eligibility is rechecked: a collision-less move does not trigger"),
				WardPtr.IsValid());

			Capsule->SetCollisionEnabled(Collision);
			WoTTest::TickWorld(&World, 0.1f);
			TestTrue(TEXT("regaining collision alone does not touch"), WardPtr.IsValid());
			Occupant->SetActorLocation(Occupant->GetActorLocation() + FVector(-1, 0, 0));
			WoTTest::TickWorld(&World, 0.1f);
			TestFalse(TEXT("the occupant's next colliding move triggers the ward"),
				WardPtr.IsValid());
			TestTrue(TEXT("the triggering occupant takes the hit"), Occupant->Health < 100);
		}
	}
	return !HasAnyErrors();
}

// bHurtsOwner=True (AngrealExpWardProjectile.uc:273): the launcher leaves
// IgnoredPawn unset (LaunchProjectileEffect.uc:127-130) and the trigger rule
// excludes nobody else, so the caster can set off their own armed ward.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardCasterContactSpec,
	"WoT.ExpWard.CasterTriggersOwnWard", WardSpecFlags)
bool FWoTExpWardCasterContactSpec::RunTest(const FString&)
{
	auto WalkUntilGone = [](UWorld& World, AWoTCharacter& Walker,
		const TWeakObjectPtr<AWoTExpWardProjectile>& WardPtr)
	{
		for (int32 Frame = 0; Frame < 120 && WardPtr.IsValid(); ++Frame)
		{
			Walker.AddMovementInput(FVector::ForwardVector, 1.0f, true);
			WoTTest::TickWorld(&World, 1, 1.0f / 60.0f);
		}
	};

	// The caster walks onto their own ward after it arms.
	{
		FWardSpecWorld Case;
		FWardSpecFloorScene Scene;
		if (WardSpecBuildFloorScene(*this, *Case.World, Scene, true))
		{
			AWoTCharacter* Caster = Scene.Caster.Pawn;
			TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
			TestTrue(TEXT("the caster owns the placed ward"),
				Scene.Ward->GetOwner() == Caster);
			WoTTest::TickWorld(Case.World, 1.2f);
			TestTrue(TEXT("ward armed"), WardPtr.IsValid() && Scene.Ward->bArmed);
			TestEqual(TEXT("the caster is walking"),
				static_cast<int32>(Caster->GetCharacterMovement()->MovementMode.GetValue()),
				static_cast<int32>(MOVE_Walking));
			Caster->Health = 100;
			WalkUntilGone(*Case.World, *Caster, WardPtr);
			TestFalse(TEXT("the caster walking onto their armed ward detonates it"),
				WardPtr.IsValid());
			TestTrue(TEXT("the ward catches its own caster"), Caster->Health < 100);
		}
	}

	// The caster standing on the spot when the ward arms sets it off by moving:
	// both contact paths admit the caster.
	{
		FWardSpecWorld Case;
		UWorld& World = *Case.World;
		WardSpecFloor(World);
		FWardSpecCaster Caster = WardSpecCaster(World, FVector(0, 0, 48), true);
		AWoTExpWardProjectile* Ward = Caster.Artifact
			? WardSpecCastAt(World, Caster, FVector::ZeroVector) : nullptr;
		if (TestNotNull(TEXT("real artifact cast places a ward underfoot"), Ward))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Ward;
			WoTTest::TickWorld(&World, 1.2f);
			TestTrue(TEXT("arming under the caster does not touch them"), WardPtr.IsValid());
			WalkUntilGone(World, *Caster.Pawn, WardPtr);
			TestFalse(TEXT("the caster stepping off their armed ward detonates it"),
				WardPtr.IsValid());
		}
	}

	// IgnoredPawn remains the exclusion on both paths (GenericProjectile.uc:172).
	{
		FWardSpecWorld Case;
		FWardSpecFloorScene Scene;
		if (WardSpecBuildFloorScene(*this, *Case.World, Scene, true))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
			Scene.Ward->SetIgnoredPawn(Scene.Caster.Pawn);
			WoTTest::TickWorld(Case.World, 1.2f);
			WalkUntilGone(*Case.World, *Scene.Caster.Pawn, WardPtr);
			TestTrue(TEXT("the ignored pawn walks over the armed ward"), WardPtr.IsValid());
		}
	}
	return !HasAnyErrors();
}

// The armed ward is a projectile target (uc:290) and TakeDamage detonates it
// (uc:119-122); only the Waiting state ignores damage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardArmedDamageSpec,
	"WoT.ExpWard.ArmedDetonatesFromDamage", WardSpecFlags)
bool FWoTExpWardArmedDamageSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	FWardSpecWallScene Scene;
	if (!WardSpecBuildWallScene(*this, World, Scene))
	{
		return false;
	}
	TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
	Scene.Ward->TakeDamage(10.0f, FDamageEvent(), nullptr, nullptr);
	TestTrue(TEXT("damage while waiting is ignored"), WardPtr.IsValid());
	WoTTest::TickWorld(&World, 1.1f);
	if (!TestTrue(TEXT("ward armed"), WardPtr.IsValid() && Scene.Ward->bArmed))
	{
		return false;
	}

	AWoTCharacter* Shooter = World.SpawnActor<AWoTCharacter>(
		FVector(0, 80, 100), FRotator::ZeroRotator, WardSpecSpawnParams());
	AWoTPlayerController* ShooterController =
		World.SpawnActor<AWoTPlayerController>(WardSpecSpawnParams());
	AWoTAngrealInvDart* Dart = World.SpawnActor<AWoTAngrealInvDart>(
		FVector(0, 80, 100), FRotator::ZeroRotator, WardSpecSpawnParams());
	if (TestNotNull(TEXT("shooter"), Shooter) && TestNotNull(TEXT("dart artifact"), Dart)
		&& TestNotNull(TEXT("shooter controller"), ShooterController))
	{
		ShooterController->SetAsLocalPlayerController();
		ShooterController->Possess(Shooter);
		Shooter->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Dart->GiveTo(Shooter);
		Dart->CurCharges = FMath::Max(Dart->CurCharges, 20);
		WoTTest::TickWorld(&World, 0.05f);
		WardSpecAimAt(*Shooter, Scene.Ward->GetActorLocation());
		Dart->CastArtifact();
		for (int32 Frame = 0; Frame < 60 && WardPtr.IsValid(); ++Frame)
		{
			WoTTest::TickWorld(&World, 1, 1.0f / 60.0f);
		}
		Dart->UnCastArtifact();
		TestFalse(TEXT("a real dart detonates the armed ward"), WardPtr.IsValid());
		TestEqual(TEXT("the shot ward blasts once"), WardSpecBlasts(World), 1);
	}
	return !HasAnyErrors();
}

// AI support: bRequiresLeading=False, a downward trajectory helper (the
// direction to the target with its z set to -0.5) and a 600-unit maximum range
// (AngrealExpWardProjectile.uc:246-260,272); the minimum
// stays the splash radius (GenericProjectile.uc:429-432). The launcher aims
// straight at the target when the class is not leadable
// (ProjectileLauncher.uc:238-301).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardAIMetadataSpec,
	"WoT.ExpWard.AIRangeAndLeading", WardSpecFlags)
bool FWoTExpWardAIMetadataSpec::RunTest(const FString&)
{
	const AWoTExpWardProjectile* WardDefaults = GetDefault<AWoTExpWardProjectile>();
	TestFalse(TEXT("the ward class does not require leading"),
		WardDefaults->bRequiresLeading);
	TestEqual(TEXT("the ward class maximum range is 600"),
		AWoTGenericProjectile::GetMaxRange(AWoTExpWardProjectile::StaticClass()),
		600.0f);
	TestEqual(TEXT("the ward class minimum range is its splash radius"),
		AWoTGenericProjectile::GetMinRange(AWoTExpWardProjectile::StaticClass()),
		120.0f);

	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	FWardSpecCaster Caster = WardSpecCaster(World, FVector(0, 0, 100), false);
	AWoTCharacter* Target = WardSpecStillPawn(World, FVector(300, 400, 100));
	if (!TestNotNull(TEXT("caster holds the real artifact"), Caster.Artifact)
		|| !TestNotNull(TEXT("target"), Target))
	{
		return false;
	}
	AWoTAngrealInvExpWard* Artifact = Caster.Artifact;
	Artifact->SetVictim(Target);
	TestFalse(TEXT("the artifact is not leadable"), Artifact->IsLeadable());
	TestEqual(TEXT("the artifact has no lead speed"), Artifact->GetLeadSpeed(), 0.0f);
	float MinRange = 0.0f;
	float MaxRange = 0.0f;
	Artifact->GetEffectiveRange(MinRange, MaxRange);
	TestEqual(TEXT("the artifact's AI minimum range"), MinRange, 120.0f);
	TestEqual(TEXT("the artifact's AI maximum range"), MaxRange, 600.0f);
	const FRotator Direct =
		(Target->GetActorLocation() - Caster.Pawn->GetActorLocation()).Rotation();
	TestTrue(TEXT("the artifact aims straight at its target"),
		Artifact->GetBestTrajectory().Equals(Direct, 0.01f));

	// The class helper: horizontal direction to the target, then z = -0.5.
	FVector Down = (Target->GetActorLocation() - Caster.Pawn->GetActorLocation())
		.GetSafeNormal();
	Down.Z = -0.5f;
	const FRotator Helper = WardDefaults->CalculateTrajectory(
		Caster.Pawn, Target, WardDefaults->Speed);
	TestTrue(FString::Printf(TEXT("the class trajectory helper aims down (got %s)"),
		*Helper.ToString()), Helper.Equals(Down.Rotation(), 0.01f));
	TestTrue(TEXT("the helper points below the horizon"), Helper.Pitch < -20.0f);
	return !HasAnyErrors();
}

// Two wards cast at one spot 0.3 s apart (the artifact refires every 0.25 s).
// SetCollision(True) only switches an arming ward's collision on (uc:186) and
// Touch comes from movement, so neither ward's arming touches the other: both
// stay armed and nothing blows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardStackedArmingSpec,
	"WoT.ExpWard.StackedWardsArmQuietly", WardSpecFlags)
bool FWoTExpWardStackedArmingSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	FWardSpecFloorScene Scene;
	if (!WardSpecBuildFloorScene(*this, World, Scene))
	{
		return false;
	}
	TWeakObjectPtr<AWoTExpWardProjectile> First = Scene.Ward;
	WoTTest::TickWorld(&World, 0.3f);
	AWoTExpWardProjectile* SecondWard =
		WardSpecCastAt(World, Scene.Caster, FVector::ZeroVector);
	if (!TestNotNull(TEXT("a second real cast at the spot places a ward"), SecondWard))
	{
		return false;
	}
	TWeakObjectPtr<AWoTExpWardProjectile> Second = SecondWard;
	TestTrue(TEXT("the second ward stacks on the first"), First.IsValid()
		&& SecondWard->GetActorLocation().Equals(First->GetActorLocation(), 1.0f));
	WoTTest::TickWorld(&World, 2.2f);
	TestTrue(TEXT("the first ward is armed and in place"), First.IsValid() && First->bArmed);
	TestTrue(TEXT("the second ward is armed and in place"),
		Second.IsValid() && Second->bArmed);
	TestEqual(TEXT("arming the stacked wards raises no blast"), WardSpecBlasts(World), 0);
	TestEqual(TEXT("arming the stacked wards splashes nobody"),
		Scene.Bystander->Health, 100);
	return !HasAnyErrors();
}

// A ward placed beside an armed one. The newcomer is placed with its collision
// off (bCollideActors=False, uc:288; SetCollision(False) before SetLocation,
// uc:72-75) and its arming raises no Touch (uc:186), so the armed ward
// survives both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardPlacedBesideArmedSpec,
	"WoT.ExpWard.PlacedBesideArmedWard", WardSpecFlags)
bool FWoTExpWardPlacedBesideArmedSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	FWardSpecFloorScene Scene;
	if (!WardSpecBuildFloorScene(*this, World, Scene))
	{
		return false;
	}
	TWeakObjectPtr<AWoTExpWardProjectile> Armed = Scene.Ward;
	WoTTest::TickWorld(&World, 1.2f);
	if (!TestTrue(TEXT("the first ward armed"), Armed.IsValid() && Armed->bArmed))
	{
		return false;
	}
	AWoTExpWardProjectile* NewcomerWard =
		WardSpecCastAt(World, Scene.Caster, FVector(30, 0, 0));
	if (!TestNotNull(TEXT("a real cast places a ward beside the armed one"), NewcomerWard))
	{
		return false;
	}
	TWeakObjectPtr<AWoTExpWardProjectile> Newcomer = NewcomerWard;
	TestTrue(TEXT("the newcomer lands 30 units from the armed ward"),
		NewcomerWard->GetActorLocation().Equals(FVector(30, 0, 0.5), 1.0f));
	TestTrue(TEXT("the placement leaves the armed ward in place"), Armed.IsValid());
	TestEqual(TEXT("the placement raises no blast"), WardSpecBlasts(World), 0);
	WoTTest::TickWorld(&World, 1.2f);
	TestTrue(TEXT("the newcomer arms"), Newcomer.IsValid() && Newcomer->bArmed);
	TestTrue(TEXT("the newcomer's arming leaves the armed ward in place"),
		Armed.IsValid() && Armed->bArmed);
	TestEqual(TEXT("neither ward blasts"), WardSpecBlasts(World), 0);
	TestEqual(TEXT("nobody is splashed"), Scene.Bystander->Health, 100);
	return !HasAnyErrors();
}

// A pawn walking onto the stacked wards afterwards still sets them off as the
// script does: the ward it touches detonates and its splash detonates the
// other (TakeDamage, uc:119-122; bProjTarget, uc:290). The walker is hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardWalkerOnStackSpec,
	"WoT.ExpWard.WalkerSetsOffStackedWards", WardSpecFlags)
bool FWoTExpWardWalkerOnStackSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	FWardSpecFloorScene Scene;
	if (!WardSpecBuildFloorScene(*this, World, Scene, true))
	{
		return false;
	}
	TWeakObjectPtr<AWoTExpWardProjectile> First = Scene.Ward;
	WoTTest::TickWorld(&World, 0.3f);
	AWoTExpWardProjectile* SecondWard =
		WardSpecCastAt(World, Scene.Caster, FVector::ZeroVector);
	if (!TestNotNull(TEXT("a second real cast at the spot places a ward"), SecondWard))
	{
		return false;
	}
	TWeakObjectPtr<AWoTExpWardProjectile> Second = SecondWard;
	WoTTest::TickWorld(&World, 2.2f);
	TestTrue(TEXT("both stacked wards armed and wait for a toucher"),
		First.IsValid() && First->bArmed && Second.IsValid() && Second->bArmed);
	AWoTCharacter* Walker = Scene.Caster.Pawn;
	Walker->Health = 100;
	WardSpecWalkUntilGone(World, *Walker, First, Second);
	TestFalse(TEXT("both stacked wards are gone"), First.IsValid() || Second.IsValid());
	TestEqual(TEXT("each ward blasts once"), WardSpecBlasts(World), 2);
	TestTrue(TEXT("the walker takes the hit"), Walker->Health < 100);
	return !HasAnyErrors();
}

// An actor inside the cylinder with its collision off when the ward arms is
// still inside, unmoved, when its collision returns. Switching collision on
// raises no Touch (uc:186), so only its next move touches. The pawn stands
// beside the ward's centre, inside only through its own 17-unit radius.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardCollisionReturnSpec,
	"WoT.ExpWard.CollisionReturningInsideWaitsForAMove", WardSpecFlags)
bool FWoTExpWardCollisionReturnSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	FWardSpecFloorScene Scene;
	if (!WardSpecBuildFloorScene(*this, World, Scene))
	{
		return false;
	}
	AWoTCharacter* Occupant = WardSpecStillPawn(World, FVector(30, 0, 46));
	if (!TestNotNull(TEXT("occupant pawn"), Occupant))
	{
		return false;
	}
	Occupant->SetActorEnableCollision(false);
	TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
	WoTTest::TickWorld(&World, 1.2f);
	if (!TestTrue(TEXT("the ward armed over the collision-less pawn"),
		WardPtr.IsValid() && WardPtr->bArmed))
	{
		return false;
	}
	Occupant->SetActorEnableCollision(true);
	WoTTest::TickWorld(&World, 0.1f);
	TestTrue(TEXT("collision returning inside the cylinder does not touch"),
		WardPtr.IsValid());
	TestEqual(TEXT("collision returning raises no blast"), WardSpecBlasts(World), 0);
	Occupant->SetActorLocation(Occupant->GetActorLocation() + FVector(-1, 0, 0));
	WoTTest::TickWorld(&World, 0.1f);
	TestFalse(TEXT("the occupant's next move touches"), WardPtr.IsValid());
	TestTrue(TEXT("the occupant takes the hit"), Occupant->Health < 100);
	return !HasAnyErrors();
}

// A ward stuck to a mover (SetBase, uc:83-85) never counts its base as a
// contact: the original's Touch never pairs an actor with the actor it is
// based on. A ward on a door near its hinge rides the door's swing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardBaseMoverSpec,
	"WoT.ExpWard.BaseMoverNeverTriggers", WardSpecFlags)
bool FWoTExpWardBaseMoverSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	// A door slab turning about its own origin (100, 0, 100): 20 deep and
	// 200 wide and tall, its face at x = 90.
	AWoTMover* Door = World.SpawnActor<AWoTMover>(
		FVector(100, 0, 100), FRotator::ZeroRotator, WardSpecSpawnParams());
	UStaticMesh* Cube = Cast<UStaticMesh>(
		FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad());
	if (!TestNotNull(TEXT("door"), Door) || !TestNotNull(TEXT("engine cube mesh"), Cube))
	{
		return false;
	}
	Door->Mesh->SetStaticMesh(Cube);
	Door->SetActorScale3D(FVector(0.2f, 2.0f, 2.0f));
	// The caster's eye at z = 100, level with the door's hinge.
	FWardSpecCaster Caster = WardSpecCaster(World, FVector(-100, 0, 58), false);
	AWoTExpWardProjectile* Ward = Caster.Artifact
		? WardSpecCastAt(World, Caster, FVector(90, 0, 100)) : nullptr;
	if (!TestNotNull(TEXT("a real cast places a ward on the door"), Ward))
	{
		return false;
	}
	TestTrue(TEXT("the ward is stuck to the door"), Ward->GetAttachParentActor() == Door);
	TestTrue(TEXT("the ward sits on the door face beside the hinge"),
		Ward->GetActorLocation().Equals(FVector(89.5, 0, 100), 1.0f));
	TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Ward;
	WoTTest::TickWorld(&World, 1.2f);
	if (!TestTrue(TEXT("the ward armed on the door"), WardPtr.IsValid() && Ward->bArmed))
	{
		return false;
	}
	const FVector ArmedAt = Ward->GetActorLocation();
	for (int32 Step = 0; Step < 10 && WardPtr.IsValid(); ++Step)
	{
		Door->AddActorWorldRotation(FRotator(0, 3, 0), false, nullptr,
			ETeleportType::TeleportPhysics);
		WoTTest::TickWorld(&World, 0.05f);
	}
	for (int32 Step = 0; Step < 20 && WardPtr.IsValid(); ++Step)
	{
		Door->AddActorWorldRotation(FRotator(0, -3, 0), false, nullptr,
			ETeleportType::TeleportPhysics);
		WoTTest::TickWorld(&World, 0.05f);
	}
	TestTrue(TEXT("the ward survives the door's swing"), WardPtr.IsValid());
	TestEqual(TEXT("the swing raises no blast"), WardSpecBlasts(World), 0);
	if (WardPtr.IsValid())
	{
		TestFalse(TEXT("the ward rode the swing"),
			Ward->GetActorLocation().Equals(ArmedAt, 1.0f));
		TestTrue(TEXT("the ward stays stuck to the door"),
			Ward->GetAttachParentActor() == Door);
	}
	return !HasAnyErrors();
}

// Explode spawns the scorch, the fireball and the blast bits at the ward's own
// Location, aligned to its surface normal RelativeUp, whatever touched it
// (AngrealExpWardProjectile.uc:135-146). A wall ward's scorch lands on the wall.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardBlastAtWardSpec,
	"WoT.ExpWard.BlastAtWardSurface", WardSpecFlags)
bool FWoTExpWardBlastAtWardSpec::RunTest(const FString&)
{
	// A pawn inside the cylinder when the ward arms moves: the deferred touch.
	{
		FWardSpecWorld Case;
		UWorld& World = *Case.World;
		AWoTCharacter* Occupant = WardSpecStillPawn(World, FVector(170, 0, 142));
		FWardSpecWallScene Scene;
		if (TestNotNull(TEXT("occupant pawn"), Occupant)
			&& WardSpecBuildWallScene(*this, World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
			WoTTest::TickWorld(&World, 1.1f);
			TestTrue(TEXT("ward armed around the occupant"),
				WardPtr.IsValid() && Scene.Ward->bArmed);
			Occupant->SetActorLocation(Occupant->GetActorLocation() + FVector(-1, 0, 0));
			for (int32 Frame = 0; Frame < 6 && WardPtr.IsValid(); ++Frame)
			{
				WoTTest::TickWorld(&World, 1, 1.0f / 60.0f);
			}
			TestFalse(TEXT("the occupant's move detonates the ward"), WardPtr.IsValid());
			WardSpecExpectBlastOnWall(*this, World, TEXT("deferred touch"));
		}
	}
	// A pawn moved into the armed ward's cylinder: the overlap touch.
	{
		FWardSpecWorld Case;
		UWorld& World = *Case.World;
		AWoTCharacter* Visitor = WardSpecStillPawn(World, FVector(100, 0, 142));
		FWardSpecWallScene Scene;
		if (TestNotNull(TEXT("visiting pawn"), Visitor)
			&& WardSpecBuildWallScene(*this, World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
			WoTTest::TickWorld(&World, 1.1f);
			TestTrue(TEXT("ward armed"), WardPtr.IsValid() && Scene.Ward->bArmed);
			Visitor->SetActorLocation(FVector(170, 0, 142));
			for (int32 Frame = 0; Frame < 6 && WardPtr.IsValid(); ++Frame)
			{
				WoTTest::TickWorld(&World, 1, 1.0f / 60.0f);
			}
			TestFalse(TEXT("the visitor's arrival detonates the ward"), WardPtr.IsValid());
			WardSpecExpectBlastOnWall(*this, World, TEXT("overlap touch"));
		}
	}
	// A shot (TakeDamage, uc:119-122).
	{
		FWardSpecWorld Case;
		UWorld& World = *Case.World;
		FWardSpecWallScene Scene;
		if (WardSpecBuildWallScene(*this, World, Scene))
		{
			TWeakObjectPtr<AWoTExpWardProjectile> WardPtr = Scene.Ward;
			WoTTest::TickWorld(&World, 1.1f);
			if (TestTrue(TEXT("ward armed"), WardPtr.IsValid() && Scene.Ward->bArmed))
			{
				Scene.Ward->TakeDamage(10.0f, FDamageEvent(), nullptr, nullptr);
				TestFalse(TEXT("the shot detonates the ward"), WardPtr.IsValid());
				WardSpecExpectBlastOnWall(*this, World, TEXT("shot"));
			}
		}
	}
	return !HasAnyErrors();
}

// GenericProjectile.PreBeginPlay (ServerInit, which plays the SpawnSound) runs
// only after the ward's placement succeeded (uc:78-96): a cast that finds no
// surface is silent, and a placed ward sounds from where it was placed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardPlacementSoundSpec,
	"WoT.ExpWard.PlacementSoundOnlyWhenPlaced", WardSpecFlags)
bool FWoTExpWardPlacementSoundSpec::RunTest(const FString&)
{
	FWardSpecWorld Case;
	UWorld& World = *Case.World;
	FWardSpecCaster Caster = WardSpecCaster(World, FVector(0, 0, 100), false);
	if (!TestNotNull(TEXT("caster holds the real ward artifact"), Caster.Artifact))
	{
		return false;
	}
	const int32 Charges = Caster.Artifact->CurCharges;
	AWoTExpWardProjectile* Missed = WardSpecCastAt(World, Caster, FVector(1000, 0, 142));
	TestNull(TEXT("a cast that finds no surface places no ward"), Missed);
	TestEqual(TEXT("the failed cast uses no charge"), Caster.Artifact->CurCharges, Charges);
	TestEqual(TEXT("the failed cast plays no placement sound"),
		WardSpecSounds(World, TEXT("PlaceLM")), 0);

	WardSpecLevelBox(World, FVector(200, 0, 150), FVector(10, 300, 300),
		TEXT("WardSpecWall"));
	AWoTExpWardProjectile* Placed = WardSpecCastAt(World, Caster, FVector(190, 0, 142));
	if (TestNotNull(TEXT("a cast at the wall places a ward"), Placed))
	{
		TestEqual(TEXT("the placed ward plays its placement sound once"),
			WardSpecSounds(World, TEXT("PlaceLM")), 1);
		const AWoTSoundBurst* Sound = WardSpecFindSound(World, TEXT("PlaceLM"));
		TestTrue(FString::Printf(TEXT("the placement sound comes from the placed ward (at %s)"),
			Sound ? *Sound->GetActorLocation().ToString() : TEXT("none")),
			Sound && Sound->GetActorLocation().Equals(Placed->GetActorLocation(), 1.0f));
	}
	return !HasAnyErrors();
}

// The server alone decides a detonation, and every client shows the blast of
// one it performs, even within a frame of arming, before the arming state
// could reach the client.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardNetServerBlastSpec,
	"WoT.ExpWard.NetClientShowsServerBlast", WardSpecFlags)
bool FWoTExpWardNetServerBlastSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	FWardSpecNetActivation Activation;
	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("ward detonation reaches the client"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("a client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTExpWardProjectile* ServerWard = nullptr;
	AWoTExpWardProjectile* ClientWard = WardSpecNetPlaceWard(Worlds, ServerWard);
	if (!TestNotNull(TEXT("the server places its ward"), ServerWard)
		|| !TestNotNull(TEXT("the ward reaches the client"), ClientWard))
	{
		return false;
	}
	TestTrue(TEXT("the server's ward stuck to the floor"), ServerWard->bCorrectlyPlaced);
	TestFalse(TEXT("the client's copy is still waiting"), ClientWard->bArmed);
	// The server's ward arms and is set off in the same frame.
	ServerWard->Activate();
	ServerWard->WoTTrigger(nullptr, nullptr);
	TestFalse(TEXT("the server detonates its ward"), IsValid(ServerWard));
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	const TWeakObjectPtr<AWoTExpWardProjectile> ClientPtr = ClientWard;
	TestTrue(TEXT("the detonation removes the client's copy"),
		Worlds.TickUntil([&ClientPtr]() { return !ClientPtr.IsValid(); }, 120));
	TestEqual(TEXT("the client shows the server's blast"), WardSpecBlasts(*ClientWorld), 1);
#endif
	return !HasAnyErrors();
}

// A client's copy never detonates by itself. Here its own arming timer runs
// before the server's arming arrives and it sees a pawn the server does not
// (a client/server position mismatch): neither ward blows, and the client
// shows the blast once the server's ward really detonates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTExpWardNetClientContactSpec,
	"WoT.ExpWard.NetClientNeverDetonatesAlone", WardSpecFlags)
bool FWoTExpWardNetClientContactSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	FWardSpecNetActivation Activation;
	UE::Net::FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("ward client contact"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("a client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTExpWardProjectile* ServerWard = nullptr;
	AWoTExpWardProjectile* ClientWard = WardSpecNetPlaceWard(Worlds, ServerWard);
	if (!TestNotNull(TEXT("the server places its ward"), ServerWard)
		|| !TestNotNull(TEXT("the ward reaches the client"), ClientWard))
	{
		return false;
	}
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	ClientWard->Activate();
	TestTrue(TEXT("the client's copy armed on its own timer"), ClientWard->bArmed);
	TestFalse(TEXT("the server's ward is still waiting"), ServerWard->bArmed);
	ACharacter* Phantom = ClientWorld->SpawnActor<ACharacter>(
		ClientWard->GetActorLocation() + FVector(0, 0, 60), FRotator::ZeroRotator,
		WardSpecSpawnParams());
	if (TestNotNull(TEXT("a pawn only the client sees"), Phantom))
	{
		Phantom->GetCharacterMovement()->DisableMovement();
	}
	Worlds.TickAll(10);
	TestEqual(TEXT("a contact only the client sees raises no blast there"),
		WardSpecBlasts(*ClientWorld), 0);
	TestTrue(TEXT("the client's copy stays"), IsValid(ClientWard));
	TestTrue(TEXT("the server's ward stays"), IsValid(ServerWard));
	TestTrue(TEXT("the server's ward arms through its timer"),
		Worlds.TickUntil([ServerWard]() { return IsValid(ServerWard) && ServerWard->bArmed; },
			180));
	if (IsValid(ServerWard))
	{
		ServerWard->WoTTrigger(nullptr, nullptr);
	}
	const TWeakObjectPtr<AWoTExpWardProjectile> ClientPtr = ClientWard;
	TestTrue(TEXT("the server's detonation removes the client's copy"),
		Worlds.TickUntil([&ClientPtr]() { return !ClientPtr.IsValid(); }, 120));
	TestEqual(TEXT("the client shows the server's blast once"),
		WardSpecBlasts(*ClientWorld), 1);
#endif
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
