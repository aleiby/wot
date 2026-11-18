// G29 headless specs: fragment skin override threading + the water/pain-zone and
// Flying.Touch legs (Engine.Fragment, ExplodingMover.uc:88-110).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTExplodingMover.h"
#include "../WoTFragment.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTZoneVolume.h"
#include "../WoTSoundTable.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "UObject/UnrealType.h"

namespace
{
	AWoTZoneVolume* SpawnG29Zone(UWorld* World, float Extent)
	{
		AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>();
		Zone->Collision->SetBoxExtent(FVector(Extent));
		return Zone;
	}

	AWoTFragment* SpawnG29Fragment(UWorld* World, FName Skin = NAME_None,
		const FVector& Location = FVector::ZeroVector)
	{
		AWoTFragment* Fragment = World->SpawnActor<AWoTFragment>(
			AWoTFragment::StaticClass(), Location, FRotator::ZeroRotator);
		Fragment->Init(EWoTFragmentType::Wall, 200.0f, FVector::ZeroVector, 1.0f, Skin);
		return Fragment;
	}

	// A fragment outside the origin zone (extent 1000), flying +X at 300 uu/s.
	// 60 ticks at 1/60s carries it from x=-1200 to x=-900 — across the boundary.
	AWoTFragment* SpawnG29Incoming(UWorld* World)
	{
		AWoTFragment* Fragment = SpawnG29Fragment(
			World, NAME_None, FVector(-1200.0f, 0.0f, 0.0f));
		Fragment->SetFragmentVelocity(FVector(300.0f, 0.0f, 0.0f));
		return Fragment;
	}
}

// Engine.Fragment state collision: Flying has bCollideActors=false; Dying enables
// non-blocking actor collision and its state-local TakeDamage destroys. Both
// assertions use a genuinely moving production zero-damage Swap Places projectile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33FragmentProjectileStateSpec,
	"WoT.RC33.R3.FragmentProjectileState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRC33FragmentProjectileStateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTFragment* Flying = SpawnG29Fragment(
		World, NAME_None, FVector(0.0f, 0.0f, 200.0f));
	Flying->SetFragmentVelocity(FVector::ZeroVector);
	World->SpawnActor<AWoTSwapPlacesProjectile>(AWoTSwapPlacesProjectile::StaticClass(),
		FVector(-100.0f, 0.0f, 200.0f), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("a moving projectile cannot contact a Flying fragment"),
		IsValid(Flying) && !Flying->IsDying());

	AWoTZoneVolume* Water = SpawnG29Zone(World, 1000.0f);
	Water->bWaterZone = true;
	Water->EntrySoundSubPath = FName(TEXT("AmbientA/DIVE"));
	AWoTFragment* Dying = SpawnG29Incoming(World);
	WoTTest::TickWorld(World, 1.0f);
	TestTrue(TEXT("water crossing enters Dying"),
		IsValid(Dying) && Dying->IsDying());
	Dying->SetFragmentVelocity(FVector::ZeroVector);
	const FVector Target = Dying->GetActorLocation();
	AWoTSwapPlacesProjectile* Swap =
		World->SpawnActor<AWoTSwapPlacesProjectile>(
			AWoTSwapPlacesProjectile::StaticClass(),
		Target - FVector(80.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	TestEqual(TEXT("production Swap Places projectile carries zero damage"),
		Swap->Damage, 0.0f);
	WoTTest::TickWorld(World, 0.1f);
	TestFalse(TEXT("a moving zero-damage projectile destroys a Dying fragment"),
		IsValid(Dying));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (a) The mover's authored WallTexture/WoodTexture is threaded into each spawned
// fragment's replicated SkinName; an unauthored family keeps NAME_None (the baked
// default look). The field is CPF_Net and the three MP-live names resolve to
// authenticated texture identities for the B3 slot override seam.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG29FragmentSkinSpec,
	"WoT.Mover.Exploding.FragmentSkin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG29FragmentSkinSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTExplodingMover* Mover = World->SpawnActorDeferred<AWoTExplodingMover>(
		AWoTExplodingMover::StaticClass(), FTransform::Identity);
	Mover->Health = 10;
	Mover->NumWallChunks = 1;
	Mover->NumWoodChunks = 1;
	Mover->NumGlassChunks = 0;
	Mover->WallTextureName = FName(TEXT("TileWall021"));
	// WoodTextureName intentionally left unauthored (NAME_None).
	Mover->KeyPos = {FVector::ZeroVector, FVector(100.0f, 0.0f, 0.0f)};
	Mover->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	Mover->FinishSpawning(FTransform::Identity);

	Mover->Explode(nullptr, FVector::ZeroVector);

	bool bSawWall = false;
	bool bSawWood = false;
	for (TActorIterator<AWoTFragment> It(World); It; ++It)
	{
		const FString Name = It->MeshName.ToString();
		if (Name.StartsWith(TEXT("wafr")))
		{
			bSawWall = true;
			TestEqual(TEXT("wall fragment carries the mover's WallTexture skin"),
				It->SkinName, FName(TEXT("TileWall021")));
		}
		else if (Name.StartsWith(TEXT("wfrag")))
		{
			bSawWood = true;
			TestTrue(TEXT("an unauthored wood texture keeps the default (no skin)"),
				It->SkinName.IsNone());
		}
	}
	TestTrue(TEXT("a wall fragment spawned"), bSawWall);
	TestTrue(TEXT("a wood fragment spawned"), bSawWood);

	const FProperty* SkinProperty = FindFProperty<FProperty>(
		AWoTFragment::StaticClass(), TEXT("SkinName"));
	TestTrue(TEXT("SkinName replicates"),
		SkinProperty && SkinProperty->HasAnyPropertyFlags(CPF_Net));

	TestEqual(TEXT("Arena_09 stone resolves to its retail texture identity"),
		AWoTFragment::ResolveSkinTexturePath(FName(TEXT("StonBrdg2735cw2"))).ToString(),
		FString(TEXT("/Game/WoT/Textures/ForsakenT/StonBrdg2735cw2.StonBrdg2735cw2")));
	TestEqual(TEXT("Whitecloak wood resolves to its retail texture identity"),
		AWoTFragment::ResolveSkinTexturePath(FName(TEXT("WoodFlor3670"))).ToString(),
		FString(TEXT("/Game/WoT/Textures/WhiteCloakT/WoodFlor3670.WoodFlor3670")));
	TestEqual(TEXT("world timber resolves to its retail texture identity"),
		AWoTFragment::ResolveSkinTexturePath(FName(TEXT("TimbFace_900"))).ToString(),
		FString(TEXT("/Game/WoT/Textures/WorldT/TimbFace_900.TimbFace_900")));
	TestTrue(TEXT("an unknown texture name keeps the baked default"),
		AWoTFragment::ResolveSkinTexturePath(FName(TEXT("NoSuchTexture999"))).IsNull());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (b) CROSSING into a water zone damps velocity to 0.2x, once EVER; spawning
// inside water does nothing (Engine.Fragment — retail's spawn-time
// ZoneChange predates state Flying, and post-entry the Dying state has no
// ZoneChange at all).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG29FragmentWaterSpec,
	"WoT.Mover.Exploding.FragmentWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG29FragmentWaterSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = SpawnG29Zone(World, 1000.0f);
	Water->bWaterZone = true;
	Water->EntrySoundSubPath = FName(TEXT("AmbientA/DIVE"));

	// Spawned INSIDE water: never damped (the retail no-op).
	AWoTFragment* Inside = SpawnG29Fragment(World);
	Inside->SetFragmentVelocity(FVector(300.0f, 0.0f, 0.0f));

	// Spawned outside, flying in: damped on the crossing.
	AWoTFragment* Incoming = SpawnG29Incoming(World);

	WoTTest::TickWorld(World, 60);
	TestFalse(TEXT("a fragment spawned inside water is never damped"),
		Inside->WasWaterDamped());
	TestEqual(TEXT("spawn-inside keeps full velocity"),
		Inside->GetFragmentVelocity().X, 300.0, 0.01);
	TestTrue(TEXT("crossing into water marks the fragment damped"),
		Incoming->WasWaterDamped());
	TestEqual(TEXT("the crossing damps velocity to 0.2x"),
		Incoming->GetFragmentVelocity().X, 60.0, 0.01);
	int32 EntryMarkers = 0;
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		if (IsValid(*It) && It->SoundSubPath == Water->EntrySoundSubPath)
		{
			++EntryMarkers;
		}
	}
	TestEqual(TEXT("fragment crossing publishes the zone EntrySound once"),
		EntryMarkers, 1);

	// Once EVER: pull the zone out from under the damped fragment, re-cover it
	// with fresh water AND a lethal pain zone — the retail Dying state handles
	// neither, so no re-damp and no post-water pain destroy.
	const double DampedX = Incoming->GetFragmentVelocity().X;
	Water->Destroy();
	WoTTest::TickWorld(World, 2);
	AWoTZoneVolume* Rewater = SpawnG29Zone(World, 8000.0f);
	Rewater->bWaterZone = true;
	AWoTZoneVolume* PostPain = SpawnG29Zone(World, 8000.0f);
	PostPain->bPainZone = true;
	PostPain->DamagePerSec = 10.0f;
	WoTTest::TickWorld(World, 2);
	TestTrue(TEXT("a damped fragment survives a later pain zone"),
		IsValid(Incoming));
	TestEqual(TEXT("re-entering water does not re-damp (once ever)"),
		Incoming->GetFragmentVelocity().X, DampedX, 0.01);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (c) CROSSING into a pain zone with DamagePerSec > 0 destroys the fragment;
// spawning inside one does not, and a zero-damage pain zone never does
// (Engine.Fragment's rate gate; spawn-time ZoneChange predates Flying).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG29FragmentPainSpec,
	"WoT.Mover.Exploding.FragmentPain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG29FragmentPainSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Pain = SpawnG29Zone(World, 1000.0f);
	Pain->bPainZone = true;
	Pain->DamagePerSec = 10.0f;

	// Spawned INSIDE the lethal zone: survives (the retail no-op).
	AWoTFragment* Inside = SpawnG29Fragment(World);
	// Spawned outside, flying in: destroyed on the crossing.
	AWoTFragment* Incoming = SpawnG29Incoming(World);

	WoTTest::TickWorld(World, 60);
	TestTrue(TEXT("a fragment spawned inside a pain zone survives"),
		IsValid(Inside));
	TestFalse(TEXT("crossing into a damaging pain zone destroys the fragment"),
		IsValid(Incoming));

	// A zero-damage pain zone does NOT destroy even on a crossing (the flag
	// alone is not enough — Engine.Fragment gates on DamagePerSec > 0).
	Pain->Destroy();
	AWoTZoneVolume* Harmless = SpawnG29Zone(World, 1000.0f);
	Harmless->bPainZone = true;
	Harmless->DamagePerSec = 0.0f;
	AWoTFragment* Crosser = SpawnG29Incoming(World);
	WoTTest::TickWorld(World, 60);
	TestTrue(TEXT("the crosser made it into the zone"),
		IsValid(Crosser) && Crosser->GetActorLocation().X > -1000.0f);
	TestTrue(TEXT("a zero-damage pain zone leaves the fragment alive"),
		IsValid(Crosser));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// (d) Flying.Touch destroys against a non-player pawn; players and non-pawns are
// left alone (Engine.Fragment).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG29FragmentTouchSpec,
	"WoT.Mover.Exploding.FragmentTouch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG29FragmentTouchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A non-pawn touch is ignored.
	AWoTFragment* AgainstProp = SpawnG29Fragment(World);
	AActor* Prop = World->SpawnActor<AActor>();
	AgainstProp->FlyingTouch(Prop);
	TestTrue(TEXT("a non-pawn touch leaves the fragment intact"),
		IsValid(AgainstProp));

	// A non-player pawn touch destroys.
	AWoTFragment* AgainstNPC = SpawnG29Fragment(World);
	APawn* NPC = World->SpawnActor<APawn>();
	AgainstNPC->FlyingTouch(NPC);
	TestFalse(TEXT("a non-player pawn touch destroys the fragment"),
		IsValid(AgainstNPC));

	// A player-controlled pawn touch is excluded.
	AWoTFragment* AgainstPlayer = SpawnG29Fragment(World);
	APawn* PlayerPawn = World->SpawnActor<APawn>();
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->Possess(PlayerPawn);
	TestTrue(TEXT("the possessed pawn is driven by a PlayerController"),
		Cast<APlayerController>(PlayerPawn->GetController()) != nullptr);
	AgainstPlayer->FlyingTouch(PlayerPawn);
	TestTrue(TEXT("a player pawn touch leaves the fragment intact"),
		IsValid(AgainstPlayer));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
