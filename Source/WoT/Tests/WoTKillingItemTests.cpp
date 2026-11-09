#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvGuardian.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInvSoulBarb.h"
#include "../Inventory/WoTAngrealInvTaint.h"
#include "../Inventory/WoTAngrealInvWhirlwind.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Traps/WoTFireWall.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTSpear.h"
#include "../Traps/WoTWall.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTKillingItem.h"
#include "../WoTMover.h"
#include "../WoTOnboardingSeams.h"
#include "../WoTPlayerController.h"

#include "GameFramework/PlayerStart.h"
#include "Misc/ScopeExit.h"

// The item behind a killing blow (WoTKillingItem) and its delivery to the
// victim for the optional death recap. The resolver specs spawn the real
// item, projectile and trap classes and only read class and ownership; the
// delivery spec kills through the real damage chain and game mode. Helper
// names carry the WoTKillingItemSpec prefix for the shared unity unit.

namespace
{
constexpr EAutomationTestFlags WoTKillingItemSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters WoTKillingItemSpecParams(AActor* Owner = nullptr)
{
	FActorSpawnParameters Params;
	Params.Owner = Owner;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

template <typename T>
T* WoTKillingItemSpecSpawn(UWorld* World, AActor* Owner, const FVector& Location)
{
	return World->SpawnActor<T>(T::StaticClass(), Location, FRotator::ZeroRotator,
		WoTKillingItemSpecParams(Owner));
}

FWoTDamageEvent WoTKillingItemSpecEvent(const TCHAR* DamageType)
{
	FWoTDamageEvent Event;
	Event.WoTDamageType = DamageType;
	return Event;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTKillingItemResolveSpec,
	"WoT.Onboarding.KillingItemResolve", WoTKillingItemSpecFlags)

bool FWoTKillingItemResolveSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Victim = WoTKillingItemSpecSpawn<AWoTCharacter>(
		World, nullptr, FVector(0, 0, 100));
	AWoTCharacter* Attacker = WoTKillingItemSpecSpawn<AWoTCharacter>(
		World, nullptr, FVector(400, 0, 100));
	if (!TestNotNull(TEXT("victim spawned"), Victim)
		|| !TestNotNull(TEXT("attacker spawned"), Attacker))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	using WoTKillingItem::Resolve;
	const FName Fire(TEXT("xxFxx"));
	const FName SelfInflicted(TEXT("SelfInflicted"));

	AWoTAngrealInvLightning* Lightning = WoTKillingItemSpecSpawn<AWoTAngrealInvLightning>(
		World, Attacker, FVector(400, 0, 300));
	TestEqual(TEXT("an item striking directly names itself"),
		Resolve(*Victim, Fire, Lightning), FName(TEXT("AngrealInvLightning")));

	AWoTAngrealInvFireball* Fireball = WoTKillingItemSpecSpawn<AWoTAngrealInvFireball>(
		World, Attacker, FVector(400, 0, 500));
	AWoTFireballProjectile* Shot = WoTKillingItemSpecSpawn<AWoTFireballProjectile>(
		World, Attacker, FVector(200, 0, 300));
	Shot->SetSourceAngreal(Fireball);
	TestEqual(TEXT("a projectile names its source angreal"),
		Resolve(*Victim, Fire, Shot), FName(TEXT("AngrealInvFireball")));
	AWoTFireballProjectile* Orphan = WoTKillingItemSpecSpawn<AWoTFireballProjectile>(
		World, Attacker, FVector(200, 0, 500));
	TestEqual(TEXT("a projectile with no source names nothing"),
		Resolve(*Victim, Fire, Orphan), NAME_None);

	// AngrealInventory.uc:306/315: a tainted item hurts its own holder.
	AWoTAngrealInvFireball* Tainted = WoTKillingItemSpecSpawn<AWoTAngrealInvFireball>(
		World, Victim, FVector(0, 0, 300));
	Tainted->bTainted = true;
	TestEqual(TEXT("a tainted item's blow at its holder names Taint"),
		Resolve(*Victim, SelfInflicted, Tainted), FName(TEXT("AngrealInvTaint")));
	TestEqual(TEXT("the same item's ordinary blow names the item"),
		Resolve(*Victim, Fire, Tainted), FName(TEXT("AngrealInvFireball")));

	// Soul Barb's blow carries the same damage type but comes from the
	// attacker's item, tainted or not.
	AWoTAngrealInvSoulBarb* SoulBarb = WoTKillingItemSpecSpawn<AWoTAngrealInvSoulBarb>(
		World, Attacker, FVector(400, 0, 700));
	TestEqual(TEXT("Soul Barb names itself"),
		Resolve(*Victim, SelfInflicted, SoulBarb), FName(TEXT("AngrealInvSoulBarb")));
	SoulBarb->bTainted = true;
	TestEqual(TEXT("an attacker's tainted Soul Barb still names Soul Barb"),
		Resolve(*Victim, SelfInflicted, SoulBarb), FName(TEXT("AngrealInvSoulBarb")));

	TestEqual(TEXT("Balefire's causer-less blow names Balefire"),
		Resolve(*Victim, FName(TEXT("Balefire")), nullptr),
		FName(TEXT("AngrealInvBalefire")));
	TestEqual(TEXT("no causer names nothing"), Resolve(*Victim, Fire, nullptr),
		NAME_None);
	TestEqual(TEXT("a pawn's own blow names nothing"),
		Resolve(*Victim, Fire, Attacker), NAME_None);

	AWoTMashadarGuide* Guide = WoTKillingItemSpecSpawn<AWoTMashadarGuide>(
		World, Attacker, FVector(-400, 0, 100));
	if (TestNotNull(TEXT("Mashadar guide spawned"), Guide))
	{
		TestEqual(TEXT("the Mashadar names the Guardian that summons it"),
			Resolve(*Victim, Fire, Guide), FName(TEXT("AngrealInvGuardian")));
		if (TestNotNull(TEXT("the guide grew its head segment"),
			Guide->GetMasterSegment()))
		{
			TestEqual(TEXT("a segment's bite reaches the Guardian through its guide"),
				Resolve(*Victim, Fire, Guide->GetMasterSegment()),
				FName(TEXT("AngrealInvGuardian")));
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTKillingItemTrapsSpec,
	"WoT.Onboarding.KillingItemTraps", WoTKillingItemSpecFlags)

bool FWoTKillingItemTrapsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Victim = WoTKillingItemSpecSpawn<AWoTCharacter>(
		World, nullptr, FVector(0, 0, 100));
	if (!TestNotNull(TEXT("victim spawned"), Victim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	using WoTKillingItem::Resolve;
	const FName Fire(TEXT("xxFxx"));

	// The placing items name the very pieces the resolver maps back to them,
	// so the resolver's table cannot drift from the items silently.
	TestTrue(TEXT("FireWallInventory places the fire wall"),
		GetDefault<AWoTFireWallInventory>()->ResourceClass == AWoTFireWall::StaticClass());
	TestTrue(TEXT("SpearInventory places the spear"),
		GetDefault<AWoTSpearInventory>()->ResourceClass == AWoTSpear::StaticClass());
	TestTrue(TEXT("PortcullisInventory places the portcullis"),
		GetDefault<AWoTPortcullisInventory>()->ResourceClass
			== AWoTPortcullis::StaticClass());

	AWoTFireWall* FireWall = WoTKillingItemSpecSpawn<AWoTFireWall>(
		World, nullptr, FVector(500, 800, 0));
	AWoTFireWallSlab* Slab = WoTKillingItemSpecSpawn<AWoTFireWallSlab>(
		World, FireWall, FVector(500, 800, 0));
	TestEqual(TEXT("a fire wall's slab names the item that places the wall"),
		Resolve(*Victim, Fire, Slab), FName(TEXT("FireWallInventory")));

	AWoTSpear* Spear = WoTKillingItemSpecSpawn<AWoTSpear>(
		World, nullptr, FVector(500, 0, 0));
	TestEqual(TEXT("a spear names the item that places it"),
		Resolve(*Victim, FName(TEXT("killed")), Spear),
		FName(TEXT("SpearInventory")));

	// A portcullis takes ownership of its map mover (Portcullis.PreBeginPlay);
	// the mover is what crushes.
	AWoTPortcullis* Portcullis = WoTKillingItemSpecSpawn<AWoTPortcullis>(
		World, nullptr, FVector(500, 400, 0));
	AWoTPortcullisMover* Gate = WoTKillingItemSpecSpawn<AWoTPortcullisMover>(
		World, Portcullis, FVector(500, 400, 200));
	TestEqual(TEXT("a portcullis's mover names the item that places the gate"),
		Resolve(*Victim, FName(TEXT("Crushed")), Gate),
		FName(TEXT("PortcullisInventory")));

	AWoTWall* PlainWall = WoTKillingItemSpecSpawn<AWoTWall>(
		World, nullptr, FVector(500, 600, 0));
	AWoTWallSlab* PlainSlab = WoTKillingItemSpecSpawn<AWoTWallSlab>(
		World, PlainWall, FVector(500, 600, 0));
	TestEqual(TEXT("a plain wall's slab names nothing"),
		Resolve(*Victim, Fire, PlainSlab), NAME_None);
	AWoTMover* MapMover = WoTKillingItemSpecSpawn<AWoTMover>(
		World, nullptr, FVector(-500, 0, 0));
	TestEqual(TEXT("a map mover no trap owns names nothing"),
		Resolve(*Victim, FName(TEXT("Crushed")), MapMover), NAME_None);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTKillingItemDeliverySpec,
	"WoT.Onboarding.KillingItemDelivery", WoTKillingItemSpecFlags)

bool FWoTKillingItemDeliverySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->SpawnActor<APlayerStart>(FVector(500, 500, 100), FRotator::ZeroRotator);
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("test world has the WoT game mode"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto SpawnPlayer = [World, Game](const FVector& Location)
	{
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
		if (!Controller->PlayerState)
		{
			Controller->InitPlayerState();
		}
		Game->ChangeTeam(Controller, 255);   // free-for-all: everyone an enemy
		Controller->Possess(World->SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator));
		return Controller;
	};
	auto PawnOf = [](AWoTPlayerController* Controller)
	{
		return Cast<AWoTCharacter>(Controller->GetPawn());
	};

	AWoTPlayerController* KillerController = SpawnPlayer(FVector(400, 0, 100));
	AWoTCharacter* Killer = PawnOf(KillerController);
	if (!TestNotNull(TEXT("killer possessed"), Killer))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTAngrealInvLightning* Lightning = WoTKillingItemSpecSpawn<AWoTAngrealInvLightning>(
		World, Killer, FVector(400, 0, 300));
	AWoTAngrealInvWhirlwind* Whirlwind = WoTKillingItemSpecSpawn<AWoTAngrealInvWhirlwind>(
		World, Killer, FVector(400, 0, 500));
	const FWoTDamageEvent Fire = WoTKillingItemSpecEvent(TEXT("xxFxx"));

	// Nothing bound (as on a client without the onboarding aids): the server
	// sends anyway and the message is dropped.
	AWoTCharacter* Unheard = PawnOf(SpawnPlayer(FVector(0, 0, 100)));
	Unheard->TakeDamage(1000.0f, Fire, KillerController, Lightning);
	TestTrue(TEXT("a kill with nothing listening still lands"), Unheard->bDead);

	TArray<FName> Delivered;
	const FDelegateHandle Listener = FWoTOnboardingSeams::Get().ClientKilledBy.AddLambda(
		[&Delivered](FName ClassName) { Delivered.Add(ClassName); });
	ON_SCOPE_EXIT
	{
		FWoTOnboardingSeams::Get().ClientKilledBy.Remove(Listener);
	};

	// Both controllers are local here and share the one seam, so a single
	// message per kill shows it went to the victim alone.
	AWoTCharacter* Struck = PawnOf(SpawnPlayer(FVector(0, 200, 100)));
	Struck->TakeDamage(1000.0f, Fire, KillerController, Lightning);
	TestTrue(TEXT("the struck player died"), Struck->bDead);
	if (TestEqual(TEXT("one message per kill, to the victim"), Delivered.Num(), 1))
	{
		TestEqual(TEXT("naming the item that killed"), Delivered[0],
			FName(TEXT("AngrealInvLightning")));
	}

	AWoTCharacter* Punched = PawnOf(SpawnPlayer(FVector(0, -200, 100)));
	Punched->TakeDamage(1000.0f, Fire, KillerController, Killer);
	TestEqual(TEXT("a kill no item dealt sends nothing"), Delivered.Num(), 1);

	// A killer-less death soon after a displacement names the displacing
	// angreal, on the terms ScoreKill credits its caster.
	AWoTCharacter* Flung = PawnOf(SpawnPlayer(FVector(-200, 0, 100)));
	Flung->SetSuicideInstigator(Killer, Whirlwind);
	Flung->TakeDamage(1000.0f, Fire, nullptr, nullptr);
	if (TestEqual(TEXT("a displacement death sends one message"), Delivered.Num(), 2))
	{
		TestEqual(TEXT("naming the displacing angreal"), Delivered[1],
			FName(TEXT("AngrealInvWhirlwind")));
	}

	AWoTCharacter* Stale = PawnOf(SpawnPlayer(FVector(-200, 200, 100)));
	Stale->SetSuicideInstigator(Killer, Whirlwind);
	Stale->SuicideInstigationTime -= Game->SuicideInstigationRelevance + 1.0f;
	Stale->TakeDamage(1000.0f, Fire, nullptr, nullptr);
	TestEqual(TEXT("a stale displacement names nothing"), Delivered.Num(), 2);

	// The blow's own item outranks a displacement: a spear after a whirlwind
	// names the spear.
	AWoTSpear* Spear = WoTKillingItemSpecSpawn<AWoTSpear>(
		World, nullptr, FVector(-200, -200, 0));
	AWoTCharacter* Skewered = PawnOf(SpawnPlayer(FVector(-200, -200, 100)));
	Skewered->SetSuicideInstigator(Killer, Whirlwind);
	Skewered->TakeDamage(1000.0f, WoTKillingItemSpecEvent(TEXT("killed")),
		nullptr, Spear);
	if (TestEqual(TEXT("a trap kill sends one message"), Delivered.Num(), 3))
	{
		TestEqual(TEXT("naming the trap's item over the displacement"),
			Delivered[2], FName(TEXT("SpearInventory")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
