// G17: the NPC angreal respawn-leech. When an NPC empties a weave,
// AngrealInventory.GoEmpty rerolls its charges and arms a UWoTLatentGiveToLeech
// on the caster that hands the weave back after NPCRespawnTime. A player's
// emptied weave never respawns (the retail LegendPawn gate = NPC-only), and if
// the caster dies before the leech fires the orphaned weave is destroyed.
// Spec: exports/script/WOT/AngrealInventory.uc:687-727,
// exports/script/WOT/LatentGiveToLeech.uc.
//
// Fixed 1/60 world steps (the G20 convention): UWorld clamps timers at frame
// granularity, so exact boundaries are not portable across the Mac/Windows lanes.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTLatentGiveToLeech.h"
#include "WoTTestDamageDispatch.h"   // AWoTTestFixedChargeAngreal

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"

namespace
{
	template <typename T>
	T* SpawnG17Pawn(UWorld* World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return World->SpawnActor<T>(T::StaticClass(), Location, FRotator::ZeroRotator,
			Params);
	}

	// A weave with a quick respawn and a real charge range, so the reroll is
	// observable. NPCRespawnTime/Min/Max are set on the instance.
	AWoTTestFixedChargeAngreal* SpawnG17Weave(UWorld* World)
	{
		AWoTTestFixedChargeAngreal* Weave =
			World->SpawnActor<AWoTTestFixedChargeAngreal>(
				FVector(0, 5000, 500), FRotator::ZeroRotator);
		Weave->NPCRespawnTime = 0.5f;
		Weave->MinInitialCharges = 2;
		Weave->MaxInitialCharges = 5;
		Weave->ChargeCost = 1;
		return Weave;
	}

	// Drop a single charge to force GoEmpty, exercising the real UseCharge path.
	void EmptyG17Weave(AWoTAngrealInventory* Weave)
	{
		Weave->CurCharges = 1;
		Weave->UseCharge();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG17NPCRespawnCycleTest,
	"WoT.Angreal.G17.NPCRespawnCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG17NPCRespawnCycleTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTNPCCharacter* NPC = SpawnG17Pawn<AWoTNPCCharacter>(World, FVector::ZeroVector);
	if (AController* C = NPC->GetController())
	{
		C->UnPossess();   // no AI casting the weave mid-cycle
	}

	AWoTTestFixedChargeAngreal* Weave = SpawnG17Weave(World);
	Weave->GiveTo(NPC);
	TestEqual(TEXT("the NPC holds the weave before it empties"),
		NPC->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(Weave));

	EmptyG17Weave(Weave);

	// The emptied weave leaves the inventory immediately, and the respawn leech
	// is armed on the caster.
	TestNull(TEXT("the emptied weave leaves the NPC inventory"),
		NPC->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()));
	TestNotNull(TEXT("a respawn leech is armed on the caster"),
		NPC->FindComponentByClass<UWoTLatentGiveToLeech>());
	TestTrue(TEXT("the weave survives while the leech holds it"), IsValid(Weave));

	// After NPCRespawnTime the leech hands the weave back with rerolled charges.
	WoTTest::TickWorld(World, 0.6f);

	TestEqual(TEXT("the weave is back in the NPC inventory"),
		NPC->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()),
		static_cast<AWoTInventory*>(Weave));
	TestTrue(TEXT("charges are rerolled into [MinInitial, MaxInitial]"),
		Weave->CurCharges >= Weave->MinInitialCharges &&
		Weave->CurCharges <= Weave->MaxInitialCharges);
	TestNull(TEXT("the leech self-destroys after handing the weave back"),
		NPC->FindComponentByClass<UWoTLatentGiveToLeech>());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG17PlayerNoRespawnTest,
	"WoT.Angreal.G17.PlayerNoRespawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG17PlayerNoRespawnTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A plain AWoTCharacter is a player pawn (IsNPCPawn() == false) — the retail
	// LegendPawn gate excludes it, so its emptied weave must not respawn.
	AWoTCharacter* Player = SpawnG17Pawn<AWoTCharacter>(World, FVector::ZeroVector);

	AWoTTestFixedChargeAngreal* Weave = SpawnG17Weave(World);
	Weave->GiveTo(Player);
	EmptyG17Weave(Weave);

	TestNull(TEXT("no respawn leech is armed on a player"),
		Player->FindComponentByClass<UWoTLatentGiveToLeech>());

	WoTTest::TickWorld(World, 0.6f);   // well past NPCRespawnTime

	TestNull(TEXT("a player's emptied weave never returns to the inventory"),
		Player->InventoryComponent->FindInventoryType(
			AWoTTestFixedChargeAngreal::StaticClass()));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG17DeadCasterDestroysWeaveTest,
	"WoT.Angreal.G17.DeadCasterDestroysWeave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTG17DeadCasterDestroysWeaveTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTNPCCharacter* NPC = SpawnG17Pawn<AWoTNPCCharacter>(World, FVector::ZeroVector);
	if (AController* C = NPC->GetController())
	{
		C->UnPossess();
	}

	AWoTTestFixedChargeAngreal* Weave = SpawnG17Weave(World);
	Weave->GiveTo(NPC);
	EmptyG17Weave(Weave);
	TestNotNull(TEXT("the leech is armed before the caster dies"),
		NPC->FindComponentByClass<UWoTLatentGiveToLeech>());

	// The caster dies before NPCRespawnTime elapses. The leech component is
	// destroyed with its pawn, so the orphaned weave is destroyed, not granted.
	NPC->Destroy();
	WoTTest::TickWorld(World, 0.1f);

	TestFalse(TEXT("the orphaned weave is destroyed when its caster dies"),
		IsValid(Weave));

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif  // WITH_AUTOMATION_TESTS
