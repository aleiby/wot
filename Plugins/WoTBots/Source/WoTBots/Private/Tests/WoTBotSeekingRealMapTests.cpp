#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "World/WoTRuntimeWorldProperties.h"
#include "AI/WoTCasterComponent.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "Inventory/WoTIceLeech.h"
#include "Projectiles/WoTDartProjectile.h"
#include "Projectiles/WoTFireballProjectile.h"
#include "Projectiles/WoTSeekerProjectile.h"
#include "Projectiles/WoTLegionProjectile.h"
#include "Projectiles/WoTSwapPlacesProjectile.h"
#include "Projectiles/WoTExpWardProjectile.h"
#include "Testing/WoTSoakEvents.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "TimerManager.h"

namespace WoTBotTest
{
struct FWoTBotSeekingAccess
{
	static void StopThinking(AWoTBotController* Bot)
	{
		Bot->GetWorldTimerManager().ClearTimer(Bot->ThinkTimer);
		Bot->SetActorTickEnabled(false);
	}
	static void Cast(AWoTBotController* Bot) { Bot->Perceive(); Bot->CastArtifacts(); }
};
}

namespace WoTBotSeekingRealMap
{
using namespace WoTBotTest;
constexpr float Step = 1.0f / 240.0f;

struct FFixture
{
	UWorld* W = nullptr;
	FVector Start, Direction;
	FSettings Settings;
	~FFixture() { WoTTest::DestroySpecWorld(W); }
	bool Load(FAutomationTestBase& Test)
	{
		Settings.Value->AimErrorDegrees = 0;
		FMapSet Maps; FString Error;
		if (!Maps.Load(Error)) { Test.AddError(Error); return false; }
		const auto* Map = Maps.Descriptors.FindByPredicate([](const auto& D) { return D.MapIdentity == TEXT("Arena_06"); });
		if (!Test.TestNotNull(TEXT("real Arena_06 descriptor"), Map)) return false;
		WoTTest::FSpecWorldOptions Options;
		Options.bStartPlay = false; Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		W = WoTTest::CreateSpecWorld(Options);
		if (!Materialize(W, Maps, *Map, Error)) { Test.AddError(Error); return false; }
		W->GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
		FURL URL(nullptr, TEXT("/Engine/Maps/Entry?BotFill=0?FragLimit=0?TimeLimit=0"), TRAVEL_Absolute);
		if (!Test.TestTrue(TEXT("real game mode"), W->SetGameMode(URL))) return false;
		W->InitializeActorsForPlay(URL); W->BeginPlay();
		WoTTest::TickWorld(W, 12.0f);
		// Pick a clear lane from the map's actual spawn pool; no synthetic geometry.
		for (TActorIterator<APlayerStart> It(W); It; ++It)
		{
			const FVector P = It->GetActorLocation() + FVector(0, 0, 40);
			for (int32 Yaw = 0; Yaw < 360; Yaw += 45)
			{
				const FVector D = FRotator(0, Yaw, 0).Vector();
				FHitResult Hit;
				if (!W->SweepSingleByChannel(Hit, P, P + D * 700,
					FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(45)))
				{
					Start = P; Direction = D;
					Test.AddInfo(FString::Printf(TEXT("Arena_06 lane start=%s direction=%s"), *P.ToString(), *D.ToString()));
					return true;
				}
			}
		}
		Test.AddError(TEXT("no clear real-map lane")); return false;
	}
	void Tick(float Seconds) { WoTTest::TickWorld(W, Seconds, Step); }
};

AWoTGenericProjectile* Fire(AWoTCharacter* Pawn, const TCHAR* Class, FVector Aim)
{
	AWoTProjectileLauncher* Item = CastChecked<AWoTProjectileLauncher>(Give(Pawn, Class));
	Pawn->GetController()->SetControlRotation(Aim.Rotation());
	Pawn->InventoryComponent->SelectItem(Item);
	Pawn->AuthorityUseAngreal(Aim.Rotation());
	Pawn->AuthorityCeaseUsingAngreal();
	for (TActorIterator<AWoTGenericProjectile> It(Pawn->GetWorld()); It; ++It)
		if (It->SourceAngreal == Item) return *It;
	return nullptr;
}

bool Selection(FAutomationTestBase& Test, const TCHAR* Class, bool bEligibleEnemy, bool bSpecialProjectile = false)
{
	FFixture F; if (!F.Load(Test)) return false;
	AWoTBotController* B = Bot(F.W, F.Start);
	FWoTBotSeekingAccess::StopThinking(B);
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	Pawn->GetCharacterMovement()->DisableMovement();
	AWoTCharacter* E = Enemy(F.W, F.Start + F.Direction * 600);
	E->GetCharacterMovement()->DisableMovement();
	B->SetControlRotation(F.Direction.Rotation());
	AWoTProjectileLauncher* Item = CastChecked<AWoTProjectileLauncher>(Give(Pawn, Class));
	AWoTGenericProjectile* Dart = Fire(E, TEXT("/Script/WoT.WoTAngrealInvDart"), -F.Direction);
	if (!Test.TestNotNull(TEXT("incoming dart fired by a real artifact"), Dart)) return false;
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	for (int32 I = 0; I < 240 && Caster->FindIncomingWeave() != Dart && IsValid(Dart); ++I) F.Tick(Step);
	if (!Test.TestEqual(TEXT("real defensive detector finds the live dart"), Caster->FindIncomingWeave(), static_cast<AActor*>(Dart))) return false;
	E->SetActorHiddenInGame(!bEligibleEnemy);
	Test.TestEqual(TEXT("enemy eligibility matches case"), Item->IsTargetable(E), bEligibleEnemy);
	if (bEligibleEnemy) Test.TestTrue(TEXT("enemy is visible on the real map"), B->CanSee(E));
	if (bSpecialProjectile)
	{
		FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Spirit = F.W->SpawnActor<AWoTLegionProjectile>(Item->GetTrajectorySource() + F.Direction * 400, (-F.Direction).Rotation(), Params);
		Test.TestEqual(TEXT("normal player acquisition admits the special projectile"),
			Item->FindBestTarget(Item->GetTrajectorySource(), F.Direction.Rotation(), Item->ProjectileClass.GetDefaultObject()->MaxTargetAngle), Spirit);
	}
	const int32 Uses = Item->CastUseCount, Charges = Item->CurCharges;
	int32 DartSwaps = 0, PawnSwaps = 0;
	const auto Handle = WoTSoakEvents::BodyEvents().AddLambda([&](AActor* A, const TCHAR* Event)
	{
		if (FString(Event) == TEXT("swap destination placed"))
		{
			if (A == Dart) ++DartSwaps;
			if (A == E) ++PawnSwaps;
		}
	});
	ON_SCOPE_EXIT { WoTSoakEvents::BodyEvents().Remove(Handle); };
	FWoTBotSeekingAccess::Cast(B);
	AWoTSeekingProjectile* Launched = nullptr;
	for (TActorIterator<AWoTSeekingProjectile> It(F.W); It; ++It)
		if (It->SourceAngreal == Item) Launched = *It;
	if (bEligibleEnemy)
	{
		if (Test.TestNotNull(TEXT("bot launches its defensive seeker"), Launched))
		{
			Test.TestEqual(TEXT("launched destination is the enemy"), Launched->Destination.Get(), static_cast<AActor*>(E));
			Test.TestTrue(TEXT("launched destination is never the dart"), Launched->Destination != Dart);
		}
		Test.TestEqual(TEXT("one cast consumes a charge"), Item->CastUseCount, Uses + 1);
	}
	else
	{
		Test.TestNull(TEXT("no eligible destination means no launched seeker"), Launched);
		Test.TestEqual(TEXT("no destination means no cast"), Item->CastUseCount, Uses);
		Test.TestEqual(TEXT("no destination preserves charges"), Item->CurCharges, Charges);
		Test.TestFalse(TEXT("no destination leaves no active cast"), Caster->IsCasting());
		Test.TestNull(TEXT("no destination clears the incoming victim"), Item->Target.Get());
	}
	F.Tick(2.0f);
	Test.TestEqual(TEXT("normal flight never swaps with the dart"), DartSwaps, 0);
	if (bEligibleEnemy && Item->ProjectileClass->IsChildOf(AWoTSwapPlacesProjectile::StaticClass()))
		Test.TestEqual(TEXT("normal flight positively swaps with the pawn"), PawnSwaps, 1);
	if (bEligibleEnemy && FString(Class).EndsWith(TEXT("WoTAngrealInvIce")))
		Test.TestNotNull(TEXT("normal ice flight freezes the enemy"), E->FindComponentByClass<UWoTIceLeech>());
	return !Test.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSwapDestinationSpec, "WoT.Bots.Seeking.RealMap.SwapEnemy", WoTBotTest::Flags)
bool FBotSwapDestinationSpec::RunTest(const FString&) { return WoTBotSeekingRealMap::Selection(*this, TEXT("/Script/WoT.WoTAngrealInvSwapPlaces"), true); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotIceDestinationSpec, "WoT.Bots.Seeking.RealMap.IceEnemy", WoTBotTest::Flags)
bool FBotIceDestinationSpec::RunTest(const FString&) { return WoTBotSeekingRealMap::Selection(*this, TEXT("/Script/WoT.WoTAngrealInvIce"), true); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSwapNoDestinationSpec, "WoT.Bots.Seeking.RealMap.SwapNoEnemy", WoTBotTest::Flags)
bool FBotSwapNoDestinationSpec::RunTest(const FString&) { return WoTBotSeekingRealMap::Selection(*this, TEXT("/Script/WoT.WoTAngrealInvSwapPlaces"), false); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotIceNoDestinationSpec, "WoT.Bots.Seeking.RealMap.IceNoEnemy", WoTBotTest::Flags)
bool FBotIceNoDestinationSpec::RunTest(const FString&) { return WoTBotSeekingRealMap::Selection(*this, TEXT("/Script/WoT.WoTAngrealInvIce"), false); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSwapSpecialDestinationSpec, "WoT.Bots.Seeking.RealMap.SwapProjectileOnly", WoTBotTest::Flags)
bool FBotSwapSpecialDestinationSpec::RunTest(const FString&) { return WoTBotSeekingRealMap::Selection(*this, TEXT("/Script/WoT.WoTAngrealInvSwapPlaces"), false, true); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotIceSpecialDestinationSpec, "WoT.Bots.Seeking.RealMap.IceProjectileOnly", WoTBotTest::Flags)
bool FBotIceSpecialDestinationSpec::RunTest(const FString&) { return WoTBotSeekingRealMap::Selection(*this, TEXT("/Script/WoT.WoTAngrealInvIce"), false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotProjectileCrossingSpec, "WoT.Bots.Seeking.RealMap.Crossings", WoTBotTest::Flags)
bool FBotProjectileCrossingSpec::RunTest(const FString&)
{
	using namespace WoTBotSeekingRealMap;
	FFixture F; if (!F.Load(*this)) return false;
	const FVector Center = F.Start + F.Direction * 350;
	const TPair<UClass*, UClass*> Pairs[] = {
		{AWoTDartProjectile::StaticClass(), AWoTDartProjectile::StaticClass()},
		{AWoTFireballProjectile::StaticClass(), AWoTSeekerProjectile::StaticClass()},
		{AWoTSwapPlacesProjectile::StaticClass(), AWoTDartProjectile::StaticClass()}};
	for (const auto& Pair : Pairs)
	{
		FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const float ASpeed = Pair.Key->GetDefaultObject<AWoTGenericProjectile>()->Speed;
		const float BSpeed = Pair.Value->GetDefaultObject<AWoTGenericProjectile>()->Speed;
		AWoTGenericProjectile* A = F.W->SpawnActor<AWoTGenericProjectile>(Pair.Key, Center - F.Direction * ASpeed * .08f, F.Direction.Rotation(), Params);
		AWoTGenericProjectile* B = F.W->SpawnActor<AWoTGenericProjectile>(Pair.Value, Center + F.Direction * BSpeed * .08f, (-F.Direction).Rotation(), Params);
		int32 PairImpacts = 0, DartSwaps = 0;
		const auto Swap = WoTSoakEvents::BodyEvents().AddLambda([&](AActor* Actor, const TCHAR* Event)
		{
			if (Actor->IsA<AWoTDartProjectile>() && FString(Event) == TEXT("swap destination placed")) ++DartSwaps;
		});
		for (int32 Tick = 0; Tick < 39; ++Tick)
		{
			F.Tick(Step);
			// All three pairs explode on accepted contact; neither may retire.
			if (!IsValid(A) || !IsValid(B) || A->IsActorBeingDestroyed() || B->IsActorBeingDestroyed()) { ++PairImpacts; break; }
		}
		WoTSoakEvents::BodyEvents().Remove(Swap);
		TestEqual(TEXT("no projectile-pair impact"), PairImpacts, 0);
		TestEqual(TEXT("no dart swap"), DartSwaps, 0);
		TestTrue(TEXT("both live projectiles crossed through their shared midpoint"), IsValid(A) && IsValid(B)
			&& FVector::DotProduct(A->GetActorLocation() - Center, F.Direction) > 0
			&& FVector::DotProduct(B->GetActorLocation() - Center, F.Direction) < 0);
		if (auto* S = Cast<AWoTSwapPlacesProjectile>(A)) TestFalse(TEXT("crossing swap never strikes"), S->bHasStruck);
		A->Destroy(); B->Destroy();
	}
	// Positive collision control: a normally placed ward arms on the real floor.
	FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTExpWardProjectile* Ward = F.W->SpawnActor<AWoTExpWardProjectile>(Center, FRotator(-90, 0, 0), Params);
	if (!TestTrue(TEXT("ward finds the real map floor"), Ward->CreationSucceeded())) return false;
	F.Tick(1.1f);
	if (!TestTrue(TEXT("ward arms through its timer"), Ward->bArmed)) return false;
	AWoTDartProjectile* Dart = F.W->SpawnActor<AWoTDartProjectile>(Ward->GetActorLocation() + FVector(0, 0, 12) - F.Direction * 150, F.Direction.Rotation(), Params);
	F.Tick(.2f);
	TestTrue(TEXT("moving dart detonates the armed ward"), !IsValid(Ward) || Ward->IsActorBeingDestroyed());
	if (IsValid(Dart)) Dart->Destroy();
	return !HasAnyErrors();
}

// Evidence for keeping spawn behavior unchanged: inspect authored records and
// inherited defaults, rather than manufacturing a tilted spawn fixture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSpawnRotationAuditSpec, "WoT.Bots.Seeking.RealMap.AuthoredSpawnRotationAudit", WoTBotTest::Flags)
bool FBotSpawnRotationAuditSpec::RunTest(const FString&)
{
	WoTBotTest::FMapSet Maps; FString Error;
	if (!Maps.Load(Error)) { AddError(Error); return false; }
	int32 Starts = 0, Creatures = 0, Tilted = 0;
	int32 SpawnedTiltedStarts = 0;
	for (const auto& Map : Maps.Descriptors)
	{
		bool bHasTiltedStart = false;
		auto Object = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(Maps.Writer.Objects.FindChecked(Map.WorldLogicalId), TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (Object.IsError()) { AddError(Object.GetError().ToString()); return false; }
		FWoTRuntimeWorldResource Resource;
		const auto Loaded = Resource.LoadObjectForTesting(MoveTemp(Object).GetValue());
		if (Loaded.IsError()) { AddError(Loaded.GetError().ToString()); return false; }
		for (const auto& Actor : Resource.Actors())
		{
			const bool bStart = Maps.Defaults.ClassIsA(Actor.ClassIdentity, TEXT("Engine.PlayerStart"));
			const bool bCreature = Maps.Defaults.ClassIsA(Actor.ClassIdentity, TEXT("WOT.Grunt"));
			if (!bStart && !bCreature) continue;
			Starts += bStart; Creatures += bCreature;
			FRotator Rotation = FRotator::ZeroRotator;
			if (const auto* Property = Maps.Defaults.FindProperty(Actor, TEXT("Rotation")))
				if (!TestTrue(TEXT("authored spawn rotation decodes"), WoTRuntimeWorldPropertyRotator(*Property, Rotation))) return false;
			if (!FMath::IsNearlyZero(FRotator::NormalizeAxis(Rotation.Pitch)) || !FMath::IsNearlyZero(FRotator::NormalizeAxis(Rotation.Roll)))
			{
				++Tilted;
				bHasTiltedStart |= bStart;
				AddInfo(FString::Printf(TEXT("tilted spawn map=%s actor=%s rotation=%s"), *Map.MapIdentity, *Actor.Identity, *Rotation.ToString()));
			}
		}
		if (bHasTiltedStart)
		{
			WoTTest::FSpecWorldOptions Options; Options.bStartPlay = false;
			UWorld* W = WoTTest::CreateSpecWorld(Options);
			ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
			if (!WoTBotTest::Materialize(W, Maps, Map, Error)) { AddError(Error); return false; }
			for (TActorIterator<APlayerStart> It(W); It; ++It)
			{
				const FRotator Rotation = It->GetActorRotation();
				if (FMath::IsNearlyZero(Rotation.Pitch) && FMath::IsNearlyZero(Rotation.Roll)) continue;
				APawn* Pawn = AWoTGameMode::SpawnPawnAtRetailStart(*W, AWoTCharacter::StaticClass(), It->GetActorTransform(), nullptr, true);
				if (TestNotNull(TEXT("real tilted map start spawns a character"), Pawn))
				{
					++SpawnedTiltedStarts;
					TestTrue(TEXT("real map spawn gives the body only authored yaw"), Pawn->GetActorRotation().Equals(FRotator(0, Rotation.Yaw, 0), 0.001));
					Pawn->Destroy();
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("spawn audit maps=%d starts=%d creatures=%d tilted=%d"), Maps.Descriptors.Num(), Starts, Creatures, Tilted));
	TestEqual(TEXT("all eleven tilted real starts exercised through production spawn"), SpawnedTiltedStarts, 11);
	return !HasAnyErrors();
}
#endif
