// Retail destruction lifecycle regression specs. Inventory actors are independent
// UE actors, so destroying their pawn must explicitly end/destroy them.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTCharacter.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"
#include "../Inventory/WoTHandSet.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTShieldReflector.h"
#include "../Projectiles/WoTDartProjectile.h"

#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
FActorSpawnParameters DestructionLifecycleSpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AWoTCharacter* SpawnLifecyclePawn(UWorld* World)
{
	return World->SpawnActor<AWoTCharacter>(
		FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator,
		DestructionLifecycleSpawnParams());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDestroyPawnStopsAutofireTest,
	"WoT.DestructionLifecycle.DestroyPawnStopsAutofire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDestroyPawnStopsAutofireTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnLifecyclePawn(World);
	AWoTAngrealInvDart* Dart = World->SpawnActor<AWoTAngrealInvDart>(
		FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator,
		DestructionLifecycleSpawnParams());
	if (!TestNotNull(TEXT("real pawn"), Pawn)
		|| !TestNotNull(TEXT("real Dart launcher"), Dart))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Dart->CurCharges = 20;
	Dart->GiveTo(Pawn);
	Pawn->InventoryComponent->SelectItem(Dart);
	WoTTest::TickWorld(World, 0.1f);
	Pawn->InventoryComponent->UseAngreal();
	WoTTest::TickWorld(World, 0.25f);

	TSet<TWeakObjectPtr<AWoTDartProjectile>> ShotsBeforeDestroy;
	for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
	{
		ShotsBeforeDestroy.Add(*It);
	}
	TestTrue(TEXT("autofire emitted at least one real Dart projectile"),
		ShotsBeforeDestroy.Num() > 0);
	TestTrue(TEXT("launcher is casting before destruction"), Dart->bCasting);

	TWeakObjectPtr<AWoTAngrealInvDart> DartAfterDestroy = Dart;
	Pawn->Destroy();
	TestTrue(TEXT("destroying the pawn immediately destroys its inventory actor"),
		!DartAfterDestroy.IsValid()
		|| DartAfterDestroy->IsActorBeingDestroyed());
	WoTTest::TickWorld(World, 0.5f);

	int32 NewShots = 0;
	for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
	{
		if (!ShotsBeforeDestroy.Contains(*It))
		{
			++NewShots;
		}
	}
	TestEqual(TEXT("no projectile is emitted after the launcher owner is gone"),
		NewShots, 0);
	TestFalse(TEXT("the independent launcher does not survive the pawn"),
		DartAfterDestroy.IsValid());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDestroyPawnDropsSealTest,
	"WoT.DestructionLifecycle.DestroyPawnDropsSeal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDestroyPawnDropsSealTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnLifecyclePawn(World);
	AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(
		FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator,
		DestructionLifecycleSpawnParams());
	if (!TestNotNull(TEXT("real pawn"), Pawn)
		|| !TestNotNull(TEXT("real seal"), Seal))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Seal->Team = 0;
	Seal->GiveTo(Pawn);
	// Reproduce the interaction guard used by a Citadel editor palette. A
	// lifecycle drop must bypass it; otherwise destruction deletes the seal.
	Pawn->CurrentHandSet = NewObject<UWoTHandSet>(Pawn);
	TestTrue(TEXT("seal starts in the pawn inventory"),
		Pawn->InventoryComponent->Items.Contains(Seal));
	TWeakObjectPtr<AWoTSeal> SealAfterDestroy = Seal;

	Pawn->Destroy();
	WoTTest::TickWorld(World, 0.05f);
	TestTrue(TEXT("dropped seal remains alive"), SealAfterDestroy.IsValid());
	if (SealAfterDestroy.IsValid())
	{
		TestNull(TEXT("dropped seal no longer has the destroyed pawn as owner"),
			SealAfterDestroy->GetOwner());
		TestEqual(TEXT("seal is pickup state after the drop"),
			SealAfterDestroy->GetItemState(), EWoTInventoryState::Pickup);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDestroyPawnCleansEffectsTest,
	"WoT.DestructionLifecycle.DestroyPawnCleansEffectsAndInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDestroyPawnCleansEffectsTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnLifecyclePawn(World);
	AWoTAngrealInvDart* Dart = World->SpawnActor<AWoTAngrealInvDart>(
		FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator,
		DestructionLifecycleSpawnParams());
	if (!TestNotNull(TEXT("real pawn"), Pawn)
		|| !TestNotNull(TEXT("real inventory actor"), Dart))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Dart->CurCharges = 20;
	Dart->GiveTo(Pawn);
	UWoTDecreaseHealthLeech* Leech = NewObject<UWoTDecreaseHealthLeech>(Pawn);
	Leech->bRemovable = true;
	Leech->AttachTo(Pawn);
	UWoTShieldReflector* Reflector = NewObject<UWoTShieldReflector>(Pawn);
	Reflector->bRemovable = false; // retail RemoveAllReflectorsFrom still removes it
	Reflector->Install(Pawn);
	TestTrue(TEXT("removable leech is installed"), Leech->IsRegistered());
	TestTrue(TEXT("non-removable default reflector is installed"),
		Reflector->IsRegistered());

	TWeakObjectPtr<AWoTAngrealInvDart> DartAfterDestroy = Dart;
	Pawn->Destroy();
	TestFalse(TEXT("remaining inventory actor is destroyed"),
		DartAfterDestroy.IsValid()
		&& !DartAfterDestroy->IsActorBeingDestroyed());
	TestFalse(TEXT("removable leech is detached"), Leech->IsRegistered());
	TestFalse(TEXT("all reflectors are uninstalled, even non-removable defaults"),
		Reflector->IsRegistered());

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
