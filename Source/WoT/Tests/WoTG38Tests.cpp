// G38: functional Andilay Root pickup + singular healing leech.
// Specs: exports/script/Angreal/AndilayRoot.uc and AndilayRootLeech.uc.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestG38Classes.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTHUD.h"
#include "../Inventory/WoTAndilayRoot.h"
#include "../Inventory/WoTAndilayRootLeech.h"
#include "../Inventory/WoTInventoryComponent.h"

#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	AWoTCharacter* SpawnG38Character(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), Location, FRotator::ZeroRotator, Params);
		Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		return Character;
	}

	AWoTAndilayRoot* SpawnG38Root(UWorld* World, const FVector& Location)
	{
		// These legacy pickup specs explicitly assert the shipped Easy charge
		// table. Production now defaults to Normal, so select Easy in the fixture.
		if (AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>())
		{
			Game->Difficulty = 0;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<AWoTAndilayRoot>(
			AWoTAndilayRoot::StaticClass(), Location, FRotator::ZeroRotator, Params);
	}

	TArray<UWoTAndilayRootLeech*> GetG38RootLeeches(AWoTCharacter* Host)
	{
		TArray<UWoTAndilayRootLeech*> Leeches;
		Host->GetComponents<UWoTAndilayRootLeech>(Leeches);
		Leeches.RemoveAll([](const UWoTAndilayRootLeech* Leech)
		{
			return !IsValid(Leech) || !Leech->IsRegistered();
		});
		return Leeches;
	}

	UWoTAndilayRootLeech* AttachG38RootLeech(
		AWoTCharacter* Host, int32 Charges)
	{
		UWoTAndilayRootLeech* Leech =
			NewObject<UWoTAndilayRootLeech>(Host);
		// Production leeches resolve this through SourceAngreal; this simulation
		// helper has no pickup actor, so provide the authored frame directly.
		Leech->StatusIconFrame = FSoftObjectPath(
			TEXT("/Game/WoT/Textures/Angreal/M_AndilayRoot.M_AndilayRoot"));
		Leech->AttachTo(Host);
		Leech->GiveCharges(Charges);
		return Leech;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG38StatusCountdownTest,
	"WoT.Inventory.G38.AndilayRoot.StatusCountdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG38StatusCountdownTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Host = SpawnG38Character(World, FVector(0, 0, 300));
	Host->Health = 50;
	AWoTAndilayRoot* Root = SpawnG38Root(World, FVector(3000, 0, 300));
	TestTrue(TEXT("root pickup installs the countdown producer"),
		Root->TryPickupBy(Host));

	TArray<UWoTAndilayRootLeech*> Leeches = GetG38RootLeeches(Host);
	UWoTAndilayRootLeech* Leech = Leeches.Num() == 1 ? Leeches[0] : nullptr;
	TestTrue(TEXT("pickup shows a full 10000-unit icon"),
		Leech && Leech->PeakCharges == Leech->NumCharges &&
		Leech->GetInitialDuration() == 10000.0f &&
		Leech->GetDuration() == 10000.0f &&
		Host->StatusIcons.Num() == 1 &&
		Host->StatusIcons[0].InitialDuration == 10000.0f);

	if (Leech)
	{
		const int32 Half = Leech->NumCharges / 2;
		while (Leech->NumCharges > Half)
		{
			Leech->GetCharges();
		}
		Host->Tick(0.0f);
		TestTrue(TEXT("half the peak reports half the HUD duration"),
			FMath::IsNearlyEqual(Leech->GetDuration(), 5000.0f));
		TestEqual(TEXT("half charge advances the 19-frame timer proportionally"),
			AWoTHUD::StatusTimerIndex(Leech->GetInitialDuration(),
				Leech->GetDuration()), 9);

		Leech->GiveCharges(30);
		TestEqual(TEXT("GiveCharges raises PeakCharges"),
			Leech->PeakCharges, Leech->NumCharges);
		TestEqual(TEXT("a new peak restores the full HUD duration"),
			Leech->GetDuration(), 10000.0f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG38PickupGateTest,
	"WoT.Inventory.G38.AndilayRoot.PickupGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG38PickupGateTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Full = SpawnG38Character(World, FVector(0, 0, 300));
	Full->Health = 100;

	AWoTAndilayRoot* First = SpawnG38Root(World, FVector(3000, 0, 300));
	TestTrue(TEXT("a full-health pawn may take exactly one root"),
		First->TryPickupBy(Full));
	TArray<UWoTAndilayRootLeech*> Leeches = GetG38RootLeeches(Full);
	TestEqual(TEXT("the first full-health root installs one leech"),
		Leeches.Num(), 1);
	const int32 StoredCharges = Leeches.Num() == 1 ? Leeches[0]->NumCharges : 0;
	// WoTAngrealInventory.cpp:347: standalone Easy deterministically chooses Max.
	// The multiplayer 15-20 roll remains an untested seam in this standalone spec.
	TestEqual(TEXT("standalone root carries MaxInitialCharges"), StoredCharges, 20);

	AWoTAndilayRoot* Second = SpawnG38Root(World, FVector(6000, 0, 300));
	TestFalse(TEXT("stored charges make a second full-health root decline"),
		Second->TryPickupBy(Full));
	Leeches = GetG38RootLeeches(Full);
	TestTrue(TEXT("a declined pickup leaves the original leech unchanged"),
		Leeches.Num() == 1 && Leeches[0]->NumCharges == StoredCharges);

	AWoTCharacter* Wounded = SpawnG38Character(World, FVector(0, 3000, 300));
	Wounded->Health = 50;
	AWoTAndilayRoot* WoundRoot =
		SpawnG38Root(World, FVector(3000, 3000, 300));
	TestTrue(TEXT("a wounded pawn picks up a root"),
		WoundRoot->TryPickupBy(Wounded));
	TestEqual(TEXT("the wounded pawn receives the healing leech"),
		GetG38RootLeeches(Wounded).Num(), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG38AutoCastTopUpTest,
	"WoT.Inventory.G38.AndilayRoot.AutoCastTopUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG38AutoCastTopUpTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Host = SpawnG38Character(World, FVector(0, 0, 300));
	Host->Health = 60;
	AWoTAndilayRoot* First = SpawnG38Root(World, FVector(3000, 0, 300));
	TestTrue(TEXT("the first root auto-casts on pickup"), First->TryPickupBy(Host));

	TArray<UWoTAndilayRootLeech*> Leeches = GetG38RootLeeches(Host);
	TestEqual(TEXT("auto-cast installs one leech"), Leeches.Num(), 1);
	UWoTAndilayRootLeech* Original = Leeches.Num() == 1 ? Leeches[0] : nullptr;
	// WoTAngrealInventory.cpp:347: standalone Easy deterministically chooses Max;
	// the multiplayer random roll is outside this fixture's coverage.
	TestEqual(TEXT("standalone auto-cast transfers MaxInitialCharges"),
		Original ? Original->NumCharges : 0, 20);
	TestEqual(TEXT("the auto-cast root does not remain in inventory"),
		Host->InventoryComponent->Items.Num(), 0);

	// Keep potential health <= the class-default 100 while forcing the cap path.
	Host->Health = 1;
	if (Original)
	{
		Original->NumCharges = 95;
	}
	AWoTAndilayRoot* Second = SpawnG38Root(World, FVector(6000, 0, 300));
	TestTrue(TEXT("a wounded host can take a second root"),
		Second->TryPickupBy(Host));
	Leeches = GetG38RootLeeches(Host);
	TestEqual(TEXT("bSingular top-up keeps exactly one component"),
		Leeches.Num(), 1);
	TestTrue(TEXT("the existing leech instance is topped up"),
		Leeches.Num() == 1 && Leeches[0] == Original);
	TestEqual(TEXT("top-up clamps to MaxCharges"),
		Original ? Original->NumCharges : 0, 100);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG38HealCadenceTest,
	"WoT.Inventory.G38.AndilayRoot.HealCadence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG38HealCadenceTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Stationary = SpawnG38Character(World, FVector(0, 0, 300));
	Stationary->Health = 90;
	UWoTAndilayRootLeech* Slow = AttachG38RootLeech(Stationary, 3);

	WoTTest::TickWorld(World, 0.18f);
	TestEqual(TEXT("stationary healing waits for the 0.2-second window"),
		Stationary->Health, 90);
	WoTTest::TickWorld(World, 0.05f);
	TestEqual(TEXT("one window heals exactly one HP"), Stationary->Health, 91);
	TestEqual(TEXT("one window drains exactly one charge"), Slow->NumCharges, 2);

	AWoTCharacter* Fast = SpawnG38Character(World, FVector(3000, 0, 300));
	Fast->Health = 90;
	UWoTAndilayRootLeech* FastLeech = AttachG38RootLeech(Fast, 3);
	Fast->GetCharacterMovement()->Velocity = FVector(800.0f, 0.0f, 0.0f);
	FastLeech->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("speed above 400 produces an unclamped interval above 0.8s"),
		FastLeech->AffectResolution > 0.8f);
	TestTrue(TEXT("800 speed computes the retail 1.4-second interval"),
		FMath::IsNearlyEqual(FastLeech->AffectResolution, 1.4f, 0.01f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG38TeardownAndPreservationTest,
	"WoT.Inventory.G38.AndilayRoot.TeardownAndPreservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG38TeardownAndPreservationTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Full = SpawnG38Character(World, FVector(0, 0, 300));
	Full->Health = 100;
	UWoTAndilayRootLeech* Preserved = AttachG38RootLeech(Full, 5);
	WoTTest::TickWorld(World, 0.7f);
	TestEqual(TEXT("full health consumes no charges"), Preserved->NumCharges, 5);
	TestEqual(TEXT("the preserved leech remains attached"),
		GetG38RootLeeches(Full).Num(), 1);

	AWoTCharacter* Depleted = SpawnG38Character(World, FVector(3000, 0, 300));
	Depleted->Health = 50;
	AttachG38RootLeech(Depleted, 1);
	WoTTest::TickWorld(World, 0.22f);
	TestEqual(TEXT("the last charge heals one HP"), Depleted->Health, 51);
	TestEqual(TEXT("an exhausted leech detaches and destroys itself"),
		GetG38RootLeeches(Depleted).Num(), 0);

	AWoTCharacter* Dead = SpawnG38Character(World, FVector(6000, 0, 300));
	Dead->Health = 0;
	AttachG38RootLeech(Dead, 5);
	WoTTest::TickWorld(World, 0.22f);
	TestEqual(TEXT("a dead host tears the leech down"),
		GetG38RootLeeches(Dead).Num(), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG38RespawnLifecycleTest,
	"WoT.Inventory.G38.AndilayRoot.RespawnLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG38RespawnLifecycleTest::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTTestG38GameMode::StaticClass();
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	AWoTCharacter* Wounded = SpawnG38Character(World, FVector(0, 0, 300));
	Wounded->Health = 50;
	AWoTAndilayRoot* Root = SpawnG38Root(World, FVector(3000, 0, 300));
	Root->RespawnTime = 0.2f;   // authored instance value, shortened for the spec
	TestTrue(TEXT("the wounded pawn takes the respawning root through TryPickupBy"),
		Root->TryPickupBy(Wounded));
	TestEqual(TEXT("ShouldRespawn gate puts the taken root to sleep"), Root->GetItemState(),
		EWoTInventoryState::Sleeping);
	TestTrue(TEXT("a sleeping root is hidden"), Root->IsHidden());
	TestFalse(TEXT("a sleeping root cannot be touched"),
		Root->GetActorEnableCollision());

	WoTTest::TickWorld(World, 0.3f);
	TestEqual(TEXT("the authored RespawnTime re-arms Pickup"),
		Root->GetItemState(), EWoTInventoryState::Pickup);
	TestFalse(TEXT("the respawned root is visible"), Root->IsHidden());
	TestTrue(TEXT("the respawned root overlaps pawns again"),
		Root->GetActorEnableCollision());
	TestTrue(TEXT("the CDO carries the retail 10-unit collision radius"),
		FMath::IsNearlyEqual(Root->Collision->GetUnscaledSphereRadius(), 10.0f));
	TestFalse(TEXT("the root does not spin"), Root->bRotatingPickup);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif  // WITH_AUTOMATION_TESTS
