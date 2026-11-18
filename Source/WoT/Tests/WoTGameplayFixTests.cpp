// Gameplay fixes: the Aura of
// Unraveling's retail touch radius, Shift's actors-only destination test, the
// Personal Shield recast pool, and never sending a correction against a movement
// base the client cannot resolve.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Assets/WoTRetailIdentity.h"
#include "../Inventory/WoTAngrealInvAMA.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTAngrealInvShift.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTShieldReflector.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTAngrealAMAProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTUE1Collision.h"
#include "../Projectiles/WoTProjectile.h"
#include "../WoTPlayerController.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "../WoTCollisionBlocker.h"
#include "Components/SphereComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	FActorSpawnParameters GameplayFixSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	int32 GameplayFixPearls(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTAMAPearl> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	// A plain actor whose root is a runtime-created, blocking, MOVABLE box: the
	// shape of the guarded world BSP (AWoTRuntimeBSPWorld::RuntimeBSPComponent_NNN),
	// which no remote machine can name.
	AActor* GameplayFixRuntimeFloor(UWorld* World, const FVector& Center,
		const FVector& Extent)
	{
		AActor* Floor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, GameplayFixSpawnParams());
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("RuntimeFloor"));
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		Floor->SetActorLocation(Center);
		return Floor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAMARetailTouchRadiusSpec,
	"WoT.AMA.RetailTouchRadius",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAMARetailTouchRadiusSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FActorSpawnParameters SP = GameplayFixSpawnParams();
	AWoTAngrealInvAMA* Inv = World->SpawnActor<AWoTAngrealInvAMA>(
		AWoTAngrealInvAMA::StaticClass(), FVector(0, 0, -2000), FRotator::ZeroRotator, SP);

	// 1) Waiting.BeginPlay's SetCollisionSize(TriggerRadius=150) never runs
	//    (BeginPlay is spawn-only; the mine reaches Waiting via Priming), so the
	//    shipped mine trips on CollisionRadius 12 (AngrealAMAProjectile.uc:108-116,
	//    :169, :180). The packaged matrix's caster ~76uu away used to trip it.
	AWoTAngrealAMAProjectile* Mine = World->SpawnActor<AWoTAngrealAMAProjectile>(
		AWoTAngrealAMAProjectile::StaticClass(), FVector(3000, 0, 300),
		FRotator::ZeroRotator, SP);
	if (!TestNotNull(TEXT("mine spawned"), Mine))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Mine->SetSourceAngreal(Inv);
	Mine->Place();
	WoTTest::TickWorld(World, 0.05f);
	TestTrue(TEXT("the mine primes into Waiting"), Mine->IsArmed());
	TestEqual(TEXT("Waiting keeps the retail cylinder radius 12"),
		Mine->CylinderCollision->GetUnscaledCapsuleRadius(), 12.0f);
	TestEqual(TEXT("Waiting keeps the retail cylinder half-height 10"),
		Mine->CylinderCollision->GetUnscaledCapsuleHalfHeight(), 10.0f);
	TestEqual(TEXT("the former sphere collision is disabled"),
		Mine->Collision->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
	TestEqual(TEXT("no proximity poll radius is applied"), Mine->ProjCollisionRadius, 0.0f);
	const int32 Pearls0 = GameplayFixPearls(World);
	AWoTCharacter* Near = World->SpawnActor<AWoTCharacter>(
		FVector(3076, 0, 300), FRotator::ZeroRotator, SP);   // the matrix's caster offset
	Near->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.3f);
	TestEqual(TEXT("a pawn 76uu away does not trip the mine"),
		GameplayFixPearls(World), Pearls0);
	TestFalse(TEXT("the mine is still waiting"), Mine->IsFading());
	Near->SetActorLocation(FVector(3000, 0, 300));   // walk onto it
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("touching the mine plants one dome"), GameplayFixPearls(World), Pearls0 + 1);
	TestTrue(TEXT("the tripped mine fades"), !IsValid(Mine) || Mine->IsFading());

	// 2) The global Touch is Pawn OR Projectile, in any state: a weave flown into
	//    a placed mine trips it too (AngrealAMAProjectile.uc:37).
	AWoTAngrealAMAProjectile* Mine2 = World->SpawnActor<AWoTAngrealAMAProjectile>(
		AWoTAngrealAMAProjectile::StaticClass(), FVector(6000, 0, 300),
		FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("second mine"), Mine2))
	{
		Mine2->SetSourceAngreal(Inv);
		Mine2->Place();
		WoTTest::TickWorld(World, 0.05f);
		const int32 Pearls1 = GameplayFixPearls(World);
		AWoTFireballProjectile* Weave = World->SpawnActor<AWoTFireballProjectile>(
			AWoTFireballProjectile::StaticClass(), FVector(6000, 300, 300),
			FRotator::ZeroRotator, SP);
		if (TestNotNull(TEXT("weave spawned"), Weave))
		{
			Weave->SetActorLocation(FVector(6000, 0, 300));
			WoTTest::TickWorld(World, 0.05f);
			TestEqual(TEXT("a projectile touching the mine plants a dome"),
				GameplayFixPearls(World), Pearls1 + 1);
		}
	}

	// 3) A spawn-time overlap is not a touch (UE1 sends no Touch at spawn and a
	//    cylinder exit is not a hit): a mine launched from inside its caster does
	//    not trip on him; walking back onto it later does.
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
		FVector(9000, 0, 300), FRotator::ZeroRotator, SP);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.05f);
	AWoTAngrealAMAProjectile* Mine3 = World->SpawnActor<AWoTAngrealAMAProjectile>(
		AWoTAngrealAMAProjectile::StaticClass(), FVector(9000, 0, 300),
		FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("third mine"), Mine3))
	{
		Mine3->SetSourceAngreal(Inv);
		Mine3->Place();
		const int32 Pearls2 = GameplayFixPearls(World);
		WoTTest::TickWorld(World, 0.2f);
		TestEqual(TEXT("the caster the mine spawned inside does not trip it"),
			GameplayFixPearls(World), Pearls2);
		Caster->SetActorLocation(FVector(9000, 400, 300));
		WoTTest::TickWorld(World, 0.05f);
		Caster->SetActorLocation(FVector(9000, 0, 300));
		WoTTest::TickWorld(World, 0.05f);
		TestEqual(TEXT("walking back onto it trips it"),
			GameplayFixPearls(World), Pearls2 + 1);
	}

	// 4) Only who overlapped AT spawn is exempt: a pawn that steps onto a fresh
	//    mine in the same frame (no tick in between, no sweep) trips it.
	AWoTAngrealAMAProjectile* Mine4 = World->SpawnActor<AWoTAngrealAMAProjectile>(
		AWoTAngrealAMAProjectile::StaticClass(), FVector(12000, 0, 300),
		FRotator::ZeroRotator, SP);
	AWoTCharacter* Stepper = World->SpawnActor<AWoTCharacter>(
		FVector(12000, 500, 300), FRotator::ZeroRotator, SP);
	if (TestNotNull(TEXT("fourth mine"), Mine4) && TestNotNull(TEXT("stepper"), Stepper))
	{
		Stepper->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Mine4->SetSourceAngreal(Inv);
		Mine4->Place();
		const int32 Pearls3 = GameplayFixPearls(World);
		Stepper->SetActorLocation(FVector(12000, 0, 300));
		TestEqual(TEXT("a same-frame, non-swept entrant trips a fresh mine"),
			GameplayFixPearls(World), Pearls3 + 1);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShiftActorsOnlySpec,
	"WoT.Inventory.ShiftActorsOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTShiftActorsOnlySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FActorSpawnParameters SP = GameplayFixSpawnParams();
	// A level-geometry floor (dev maps are StaticMeshActors; guarded maps the
	// runtime BSP world): not an actor to Util.ActorFits.
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		FVector(0, 0, 0), FRotator::ZeroRotator, SP);
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("FloorBox"));
	Box->SetBoxExtent(FVector(2000, 2000, 10));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->RegisterComponent();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 200), FRotator::ZeroRotator, SP);
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("pawn"), Pawn) || !TestNotNull(TEXT("controller"), PC))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	Pawn->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	WoTTest::TickWorld(World, 1.0f);   // land on the floor
	const float HalfHeight = Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	TestTrue(TEXT("standing on the floor"),
		FMath::IsNearlyEqual(Pawn->GetActorLocation().Z, 10.0f + HalfHeight, 3.0f));

	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;
	World->SpawnActor<AWoTAngrealInvShift>(Pawn->GetActorLocation(), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.2f);
	AWoTAngrealInvShift* Shift = Cast<AWoTAngrealInvShift>(Inv->SelectedItem.Get());
	if (!TestNotNull(TEXT("shift held"), Shift))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Shift->CurCharges = 5;

	// ShiftEffect.uc:127-141: 160uu along a view pitched 10 degrees down ends ~28uu
	// into the floor. Util.ActorFits ignores the world, SetLocation's FindSpot lifts
	// the cylinder out: the cast succeeds (the port used to fail it).
	PC->SetControlRotation(FRotator(-10.0f, 0.0f, 0.0f));
	const FVector Start = Pawn->GetActorLocation();
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("an aimed-down shift spends a charge"), Shift->CurCharges, 4);
	const FVector End = Pawn->GetActorLocation();
	TestTrue(FString::Printf(TEXT("moved forward along the view (dx %.1f)"), End.X - Start.X),
		End.X - Start.X > 150.0f);
	TestTrue(FString::Printf(TEXT("not embedded in the floor (z %.1f)"), End.Z),
		End.Z >= 10.0f + HalfHeight - 1.0f);

	// A blocking ACTOR at the destination still fails the cast (no charge).
	WoTTest::TickWorld(World, 1.2f);
	PC->SetControlRotation(FRotator::ZeroRotator);
	const FVector Here = Pawn->GetActorLocation();
	AWoTCharacter* Blocker = World->SpawnActor<AWoTCharacter>(
		Here + FVector(160, 0, 0), FRotator::ZeroRotator, SP);
	TestNotNull(TEXT("blocker"), Blocker);
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("a pawn at the destination fails the cast"), Shift->CurCharges, 4);
	TestTrue(TEXT("and does not move the caster"),
		FVector::Dist(Pawn->GetActorLocation(), Here) < 1.0f);

	// Util.ActorFits over the same world: the floor actor is not counted, a pawn is.
	TestTrue(TEXT("level geometry is not an ActorFits blocker"),
		UWoTWhirlwindLeech::ActorFits(Pawn, Here + FVector(0, 400, 20), 1024.0f));
	TestFalse(TEXT("a blocking pawn is"),
		UWoTWhirlwindLeech::ActorFits(Pawn, Blocker->GetActorLocation(), 1024.0f));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRC33TeleportWorldOnlySpec,
	"WoT.RC33.G21.TeleportWorldOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRC33TeleportWorldOnlySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	const FActorSpawnParameters SP = GameplayFixSpawnParams();
	AWoTCharacter* Traveler = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, SP);
	AWoTCharacter* Occupant = World->SpawnActor<AWoTCharacter>(
		FVector(400, 0, 100), FRotator::ZeroRotator, SP);
	const ECollisionResponse Before = Traveler->GetCapsuleComponent()
		->GetCollisionResponseToChannel(ECC_Pawn);
	TestTrue(TEXT("retail world-only placement ignores an occupying pawn"),
		WoTUE1Collision::TeleportWorldOnly(*Traveler,
			Occupant->GetActorLocation(), Traveler->GetActorRotation()));
	TestTrue(TEXT("pawn overlap does not adjust the requested destination"),
		Traveler->GetActorLocation().Equals(Occupant->GetActorLocation(), 0.01f));
	TestEqual(TEXT("the production capsule response is restored after placement"),
		Traveler->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Pawn),
		Before);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPersonalShieldRecastSpec,
	"WoT.Inventory.PersonalShieldRecast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPersonalShieldRecastSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, GameplayFixSpawnParams());
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	auto Shields = [&]()
	{
		TArray<UWoTShieldReflector*> Found;
		Pawn->GetComponents<UWoTShieldReflector>(Found);
		return Found;
	};
	auto Install = [&]()
	{
		UWoTShieldReflector* Shield = NewObject<UWoTShieldReflector>(Pawn);
		Shield->Install(Pawn);
		return Shield;
	};
	auto Hurt = [&](int32 Amount)
	{
		FWoTDamageEvent Event;
		Event.WoTDamageType = TEXT("xxxxx");
		Event.Damage = Amount;
		Pawn->TakeDamage(static_cast<float>(Amount), Event, nullptr, nullptr);
	};

	// A first shield fills the pool.
	Install();
	TestEqual(TEXT("first shield: full pool"), Shields().Num() == 1
		? Shields()[0]->ShieldHitPoints : -1, 50);
	Hurt(10);   // 5 to the player, 5 to the pool
	TestEqual(TEXT("worn to 45"), Shields()[0]->ShieldHitPoints, 45);

	// ShieldReflector.Install (uc:33-44) fills the PAWN pool, then Reflector.Install's
	// bRemoveExisting (Reflector.uc:233-245) UnInstalls the worn shield, whose
	// UnInstall (uc:61-70) zeroes that same pool: the recast shield holds 0.
	Install();
	TestEqual(TEXT("the recast replaced the worn shield"), Shields().Num(), 1);
	TestEqual(TEXT("recast over a worn shield leaves 0 points"),
		Shields().Num() == 1 ? Shields()[0]->ShieldHitPoints : -1, 0);
	TestEqual(TEXT("its status duration reads 0"),
		Shields().Num() == 1 ? Shields()[0]->GetDuration() : -1.0f, 0.0f);

	// With 0 points the next hit lands in full and breaks it (uc:84-137).
	const int32 Health0 = Pawn->Health;
	Hurt(10);
	TestEqual(TEXT("the empty shield absorbs nothing"), Pawn->Health, Health0 - 10);
	TestEqual(TEXT("and breaks"), Shields().Num(), 0);

	// With no shield worn, a cast fills the pool again.
	Install();
	TestEqual(TEXT("a cast with no shield worn fills the pool"),
		Shields().Num() == 1 ? Shields()[0]->ShieldHitPoints : -1, 50);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUnresolvableBaseCorrectionSpec,
	"WoT.Net.UnresolvableBaseCorrection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTUnresolvableBaseCorrectionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AActor* Floor = GameplayFixRuntimeFloor(World, FVector(1000, 0, 0), FVector(3000, 3000, 10));
	UPrimitiveComponent* FloorBox = Cast<UPrimitiveComponent>(Floor->GetRootComponent());
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 200), FRotator::ZeroRotator, GameplayFixSpawnParams());
	if (!TestNotNull(TEXT("pawn"), Pawn) || !TestNotNull(TEXT("floor"), FloorBox))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTCharacterMovement* Move = Cast<UWoTCharacterMovement>(Pawn->GetCharacterMovement());
	Move->bRunPhysicsWithNoController = true;
	Move->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(World, 1.0f);
	TestEqual(TEXT("walking"), static_cast<int32>(Move->MovementMode.GetValue()),
		static_cast<int32>(MOVE_Walking));
	TestTrue(TEXT("based on the runtime floor"), Move->GetMovementBaseObject() == FloorBox);

	// Which bases a remote machine can name.
	TestTrue(TEXT("no base is fine"), UWoTCharacterMovement::IsNetResolvableMovementBase(nullptr));
	TestFalse(TEXT("a runtime-created component of a local actor is not"),
		UWoTCharacterMovement::IsNetResolvableMovementBase(FloorBox));
	TestTrue(TEXT("a native subobject of a replicated actor is"),
		UWoTCharacterMovement::IsNetResolvableMovementBase(Pawn->GetCapsuleComponent()));

	// The pure rewrite.
	FClientAdjustment Relative;
	Relative.bBaseRelativePosition = true;
	Relative.bBaseRelativeVelocity = true;
	Relative.NewLoc = FVector(1, 2, 3);
	TestTrue(TEXT("an unresolvable relative correction is rewritten"),
		UWoTCharacterMovement::AbsolutizeUnresolvableAdjustment(Relative, FloorBox,
			FVector(10, 20, 30), FVector(4, 5, 6)));
	TestFalse(TEXT("to an absolute location"), Relative.bBaseRelativePosition);
	TestEqual(TEXT("absolute location"), Relative.NewLoc, FVector(10, 20, 30));
	TestEqual(TEXT("world velocity"), Relative.NewVel, FVector(4, 5, 6));
	FClientAdjustment Resolvable;
	Resolvable.bBaseRelativePosition = true;
	Resolvable.NewLoc = FVector(1, 2, 3);
	TestFalse(TEXT("a resolvable base keeps its relative correction"),
		UWoTCharacterMovement::AbsolutizeUnresolvableAdjustment(Resolvable,
			Pawn->GetCapsuleComponent(), FVector::ZeroVector, FVector::ZeroVector));
	TestTrue(TEXT("unchanged"), Resolvable.bBaseRelativePosition);

	// The real server path: a client that reports itself 545uu away (the
	// SwapPlaces desync) gets an ABSOLUTE correction it can apply.
	FNetworkPredictionData_Server_Character* ServerData =
		Move->GetPredictionData_Server_Character();
	const FVector ServerLocation = Pawn->GetActorLocation();
	const uint8 Mode = Move->PackNetworkMovementMode();
	FMovementBaseInterfaceData* const NoClientBase = nullptr;   // unmapped NetGUID
	Move->ServerMoveHandleClientError(1.0f, 0.016f, FVector::ZeroVector,
		ServerLocation - FloorBox->GetComponentLocation() + FVector(545, 0, 0),
		NoClientBase, NAME_None, Mode);
	TestEqual(TEXT("a correction was staged"), ServerData->PendingAdjustment.TimeStamp, 1.0f);
	TestFalse(TEXT("it is a correction, not an ack"), ServerData->PendingAdjustment.bAckGoodMove);
	TestFalse(TEXT("sent absolute, not relative to the unnameable floor"),
		ServerData->PendingAdjustment.bBaseRelativePosition);
	TestTrue(TEXT("carrying the server's world location"),
		ServerData->PendingAdjustment.NewLoc.Equals(ServerLocation, 0.5f));

	// Client -> server: the client's floor-relative location (its base arrives
	// null) reads through the server's copy of the same local geometry, so an
	// in-sync client is not corrected.
	Move->ServerMoveHandleClientError(2.0f, 0.016f, FVector::ZeroVector,
		ServerLocation - FloorBox->GetComponentLocation(), NoClientBase, NAME_None, Mode);
	// (A good move is acknowledged: TimeStamp advances with bAckGoodMove set.)
	TestEqual(TEXT("the in-sync move was answered"),
		ServerData->PendingAdjustment.TimeStamp, 2.0f);
	TestTrue(TEXT("an in-sync client on local geometry is not corrected"),
		ServerData->PendingAdjustment.bAckGoodMove);

	// Only a WALKING client with an unmapped base is read through the server's
	// copy; an airborne client's null base means an absolute location, and a
	// nameable server base (a mover) is left to UE.
	TestTrue(TEXT("walking + unmapped base + unnameable dynamic server base"),
		UWoTCharacterMovement::ShouldReadClientLocationThroughServerBase(
			MOVE_Walking, false, FloorBox, true));
	TestFalse(TEXT("an airborne client is not reinterpreted"),
		UWoTCharacterMovement::ShouldReadClientLocationThroughServerBase(
			MOVE_Falling, false, FloorBox, true));
	TestFalse(TEXT("a client whose base arrived is not reinterpreted"),
		UWoTCharacterMovement::ShouldReadClientLocationThroughServerBase(
			MOVE_Walking, true, FloorBox, true));
	TestFalse(TEXT("a nameable server base is left to UE"),
		UWoTCharacterMovement::ShouldReadClientLocationThroughServerBase(
			MOVE_Walking, false, Pawn->GetCapsuleComponent(), true));
	TestFalse(TEXT("a static server base means absolute locations"),
		UWoTCharacterMovement::ShouldReadClientLocationThroughServerBase(
			MOVE_Walking, false, FloorBox, false));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDerivedMaterialExactIdentitySpec,
	"WoT.Assets.DerivedMaterialExactIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDerivedMaterialExactIdentitySpec::RunTest(const FString&)
{
	// A particle sprayer on a colliding texture (ParticleSystems ships Blood01 in
	// both the Blood and ModulatedBlood groups): the production contract holds
	// only the exact grouped identities, no plain alias.
	FWoTRetailObjectPath Blood;
	TestTrue(TEXT("sprayer texture identity parses"),
		WoTParseRetailObjectPath(TEXT("ParticleSystems.Blood.Blood01"), Blood));
	FWoTRetailObjectPath Modulated;
	WoTParseRetailObjectPath(TEXT("ParticleSystems.ModulatedBlood.Blood01"), Modulated);
	auto ContractHolds = [](const FString& LogicalId)
	{
		return LogicalId == TEXT("cache/texture/ParticleSystems.Blood__Blood01")
			|| LogicalId == TEXT("cache/texture/ParticleSystems.ModulatedBlood__Blood01");
	};
	const FString SubPath = WoTRetailAssetSubPath(
		EWoTNeutralResourceKind::Texture, Blood, ContractHolds);
	TestEqual(TEXT("the texture routes to the exact grouped identity"),
		SubPath, FString(TEXT("ParticleSystems/Blood/Blood01")));
	const FString Suffix = WoTRetailDerivedMaterialSuffix(Blood, /*bGrouped*/ true);
	TestEqual(TEXT("reversible grouped derived-material suffix"),
		Suffix, FString(TEXT("ParticleSystems-2EBlood__Blood01")));
	TestNotEqual(TEXT("the two colliding exports stay distinct"),
		WoTRetailDerivedMaterialSuffix(Modulated, true), Suffix);
	TestTrue(TEXT("MI_PSAdd_<suffix> is a valid object name"),
		FName::IsValidXName(TEXT("MI_PSAdd_") + Suffix, INVALID_OBJECTNAME_CHARACTERS));
	FString Decoded;
	TestTrue(TEXT("the derived-material resolver decodes the suffix"),
		WoTRetailTextureSubPathFromDerivedSuffix(Suffix, Decoded));
	TestEqual(TEXT("back to the same grouped texture sub path"), Decoded, SubPath);
	FString Base;
	TestTrue(TEXT("which is the exact cache identity"),
		WoTRetailLogicalBaseFromSubPath(Decoded, Base)
		&& Base == TEXT("ParticleSystems.Blood__Blood01"));

	// A unique name keeps the plain spelling the generated dev instances use.
	TestEqual(TEXT("plain suffix unchanged"),
		WoTRetailDerivedMaterialSuffix(Blood, /*bGrouped*/ false),
		FString(TEXT("ParticleSystems__Blood01")));
	TestTrue(TEXT("plain suffix decodes"),
		WoTRetailTextureSubPathFromDerivedSuffix(TEXT("ParticleSystems__Sparks01"), Decoded)
		&& Decoded == TEXT("ParticleSystems/Sparks01"));

	// Escaped group names round-trip (FootStubA.(All).METL).
	FWoTRetailObjectPath Escaped;
	WoTParseRetailObjectPath(TEXT("FootStubA.(All).METL"), Escaped);
	const FString EscapedSuffix = WoTRetailDerivedMaterialSuffix(Escaped, true);
	TestEqual(TEXT("escaped group suffix"), EscapedSuffix,
		FString(TEXT("FootStubA-2E-28All-29__METL")));
	TestTrue(TEXT("escaped group decodes"),
		WoTRetailTextureSubPathFromDerivedSuffix(EscapedSuffix, Decoded)
		&& Decoded == TEXT("FootStubA/(All)/METL"));

	// Malformed suffixes fail closed.
	for (const TCHAR* Bad : {TEXT("__Blood01"), TEXT("ParticleSystems__"),
		TEXT("ParticleSystems-2E__Blood01"), TEXT("ParticleSystems-2EBl-2__Blood01"),
		TEXT("Particle.Systems__Blood01"), TEXT("ParticleSystems-2EB-2Food__Blood01")})
	{
		TestFalse(FString::Printf(TEXT("malformed suffix %s is rejected"), Bad),
			WoTRetailTextureSubPathFromDerivedSuffix(Bad, Decoded));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWhirlwindRealFloorSpec,
	"WoT.Inventory.WhirlwindRealFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTWhirlwindRealFloorSpec::RunTest(const FString&)
{
	// A real map: level geometry under the victim (a StaticMeshActor, as the dev
	// world meshes are). Util.ActorFits must not count it (the packaged matrix
	// victim never moved), and Owner.SetLocation's FindSpot must keep a victim
	// dragged toward a point below the floor on top of it (it never sinks).
	UWorld* World = WoTTest::CreateSpecWorld();
	const FActorSpawnParameters SP = GameplayFixSpawnParams();
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, SP);
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("FloorBox"));
	Box->SetBoxExtent(FVector(2000, 2000, 10));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->RegisterComponent();
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 200), FRotator::ZeroRotator, SP);
	Victim->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	WoTTest::TickWorld(World, 1.0f);
	const float HalfHeight = Victim->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Start = Victim->GetActorLocation();
	TestTrue(TEXT("the victim stands on the floor"),
		FMath::IsNearlyEqual(Start.Z, 10.0f + HalfHeight, 3.0f));

	UWoTWhirlwindLeech* Leech = NewObject<UWoTWhirlwindLeech>(Victim);
	Leech->AttachTo(Victim);
	// AngrealInvWhirlwind.uc:149 with the caster's view pitched down: the desired
	// point is 40uu under the floor, 200uu ahead.
	Leech->SetDesiredLocation(FVector(200, 0, -30));
	WoTTest::TickWorld(World, 2.0f);
	const FVector End = Victim->GetActorLocation();
	TestTrue(FString::Printf(TEXT("the whirlwind drags the victim (dx %.1f)"), End.X - Start.X),
		End.X - Start.X > 50.0f);
	TestTrue(FString::Printf(TEXT("the victim never sinks into the floor (z %.1f)"), End.Z),
		End.Z >= 10.0f + HalfHeight - 2.0f);

	// A point above the floor lifts it (the retail whirlwind raises its victim).
	Leech->SetDesiredLocation(FVector(End.X, 0, 400));
	WoTTest::TickWorld(World, 2.0f);
	TestTrue(FString::Printf(TEXT("the whirlwind lifts the victim (z %.1f)"),
		Victim->GetActorLocation().Z), Victim->GetActorLocation().Z > End.Z + 100.0f);
	Leech->Unattach();
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
