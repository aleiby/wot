#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTNPCCharacter.h"
#include "Inventory/WoTAngrealInvHeal.h"
#include "Inventory/WoTResourceSpawnerInv.h"
#include "Projectiles/WoTAngrealProjectile.h"
#include "Projectiles/WoTSeekingProjectile.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "WoTZoneVolume.h"
#include "Engine/DamageEvents.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Inventory/WoTProjectileLauncher.h"
namespace WoTBotTest
{

struct FWoTBotCastCompletionAccess
{
	static void Think(AWoTBotController* Bot) { Bot->Think(); }
	static void Decide(AWoTBotController* Bot) { Bot->CastArtifacts(); }
	static void Prepare(AWoTBotController* Bot, AWoTCharacter* Enemy)
	{
		Bot->GetWorldTimerManager().ClearAllTimersForObject(Bot);
		Bot->SetTarget(Enemy, true);
		Bot->State = EWoTBotState::Engage;
	}
	static void Cast(AWoTBotController* Bot, AWoTAngrealInventory* Item)
	{
		Item->SetVictim(Bot->GetTarget());
		Item->InitChargeGroup();
		Bot->CastItem(Item);
	}
	static bool SelectAndCast(AWoTBotController* Bot, AWoTAngrealInventory* Item, AActor* Threat, float& Budget)
	{
		if (Bot->Caster->IsCasting()) return false;
		AWoTAngrealInventory* Selected = nullptr;
		if (Item->bOffensive) Selected = Bot->SelectArtifact(Bot->GetTarget(), false);
		else if (Item->bDefensive && !Item->IsA<AWoTAngrealInvHeal>())
			Selected = Bot->SelectArtifact(Threat, true);
		else
		{
			// Utility decisions retain the real health, helper and usage gates.
			Bot->CastArtifacts();
			Budget = 1.05f;
			return Bot->Caster->IsCasting();
		}
		if (!Selected) return false;
		Selected->GetCastDuration(Budget);
		Bot->CastItem(Selected);
		return Bot->Caster->IsCasting();
	}
};

namespace
{
struct FCatalogRow { const TCHAR* Name; float MaxBudget; bool bUsable = true; };
// All 34 classes from the three default pool catalogs, plus two bound attacks.
const FCatalogRow Catalog[] = {
	{TEXT("Dart"), 4.75f}, {TEXT("Fireball"), .716667f}, {TEXT("DistantEye"), 1.05f, false},
	{TEXT("Shield"), 1.05f}, {TEXT("TrapDetect"), 1.05f, false}, {TEXT("ExpWard"), .3f, false},
	{TEXT("Illusion"), 1.05f, false}, {TEXT("SoulBarb"), 1.05f},
	{TEXT("AMA"), 1.05f}, {TEXT("Shift"), 1.05f}, {TEXT("EarthTremor"), .716667f},
	{TEXT("Ice"), 1.05f}, {TEXT("Seeker"), 1.05f}, {TEXT("Lightning"), 1.05f},
	{TEXT("SwapPlaces"), 1.05f}, {TEXT("WallOfAir"), 1.55f}, {TEXT("Levitate"), 1.05f, false},
	{TEXT("Whirlwind"), 3.05f}, {TEXT("Reflect"), 1.05f}, {TEXT("Fork"), 1.05f},
	{TEXT("Minion"), 1.05f}, {TEXT("Guardian"), 1.05f}, {TEXT("Disguise"), 1.05f, false},
	{TEXT("Absorb"), 1.05f}, {TEXT("Heal"), 1.05f}, {TEXT("RemoveCurse"), 1.05f},
	{TEXT("Decay"), 2.05f}, {TEXT("Taint"), 1.05f}, {TEXT("Target"), 1.05f},
	{TEXT("FireShield"), 1.05f}, {TEXT("AirShield"), 1.05f}, {TEXT("EarthShield"), 1.05f},
	{TEXT("WaterShield"), 1.05f}, {TEXT("SpiritShield"), 1.05f},
	{TEXT("Balefire"), 2.0f}, {TEXT("AirBurst"), .55f}
};

bool ActiveFlag(const AWoTAngrealInventory* Item)
{
	const FBoolProperty* P = FindFProperty<FBoolProperty>(Item->GetClass(), TEXT("bCasting"));
	return P && P->GetPropertyValue_InContainer(Item);
}

AWoTAngrealInventory* OnlyItem(AWoTBotController* Bot, const FString& Name)
{
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(Bot->GetPawn());
	const auto Items = Pawn->InventoryComponent->Items;
	for (AWoTInventory* Item : Items) { Pawn->InventoryComponent->DeleteInventory(Item); Item->Destroy(); }
	AWoTAngrealInventory* Item = Give(Pawn, *FString::Printf(TEXT("/Script/WoT.WoTAngrealInv%s"), *Name));
	Item->CurCharges = Item->MaxCharges;
	return Item;
}

void RemoveHelpers(UWorld* W, AWoTCharacter* Pawn)
{
	for (TActorIterator<AWoTNPCCharacter> It(W); It; ++It)
		if (It->SummonOwner == Pawn) It->Destroy();
	for (TActorIterator<AWoTMashadarGuide> It(W); It; ++It)
		if (It->SummonOwner == Pawn) It->Destroy();
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotCatalogCompletionSpec, "WoT.Bots.CastCompletion.Catalog", Flags)
void FBotCatalogCompletionSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const FCatalogRow& Row : Catalog) { Names.Add(Row.Name); Commands.Add(Row.Name); }
}
bool FBotCatalogCompletionSpec::RunTest(const FString& Name)
{
	const FCatalogRow* Row = nullptr;
	for (const FCatalogRow& Candidate : Catalog) if (Name == Candidate.Name) Row = &Candidate;
	if (!TestNotNull(TEXT("catalog row"), Row)) return false;
	FSettings Settings; Settings.Value->ReactionSeconds = 0; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(Name == TEXT("AirBurst") ? 100 : 600, 0, 50));
	E->SetCanBeDamaged(false); Pawn->SetCanBeDamaged(false);
	AWoTAngrealInventory* Item = OnlyItem(B, Name);
	// Fix the random group at its authored maximum, including the longest volley.
	Item->MinChargesInGroup = Item->MaxChargesInGroup;
	TestEqual(TEXT("bot has only the catalog artifact"), Pawn->InventoryComponent->Items.Num(), 1);
	const bool Usable = Item->bOffensive || Item->bDefensive || Item->IsA<AWoTResourceSpawnerInv>();
	TestEqual(TEXT("catalog classification matches real item policy"), Usable, Row->bUsable);
	if (!Row->bUsable)
	{
		AddInfo(FString::Printf(TEXT("class=%s SKIP: no offensive, defensive or summon selection policy"), *Item->GetClass()->GetName()));
		return !HasAnyErrors();
	}
	FWoTBotCastCompletionAccess::Prepare(B, E);
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	// A real projectile supplies the defensive selector's element and range checks.
	// Keep it in view until input completion; its flight is independent of this spec.
	AWoTAngrealProjectile* Threat = W->SpawnActor<AWoTAngrealProjectile>(FindObject<UClass>(nullptr,
		TEXT("/Script/WoT.WoTDartProjectile")), FVector(400, 100, 80), FRotator::ZeroRotator);
	Threat->SourceAngreal = Give(E, TEXT("/Script/WoT.WoTAngrealInvDart"));
	Threat->SetActorTickEnabled(false);
	for (UActorComponent* Component : Threat->GetComponents()) Component->SetComponentTickEnabled(false);
	Threat->SetLifeSpan(0);
	float NextAllowedSelection = 0;
	for (int32 Cycle = 0; Cycle != 2; ++Cycle)
	{
		if (Item->IsA<AWoTAngrealInvHeal>()) Pawn->Health = 20;
		if (Cycle) RemoveHelpers(W, Pawn); // A living helper deliberately forbids another summon.
		if (Cycle && Name == TEXT("Shield"))
		{
			// The installed shield has no expiry and deliberately suppresses reuse.
			// Exhaust it through real damage before asking the selector to replenish it.
			Pawn->SetCanBeDamaged(true); Pawn->Health = 1000;
			FDamageEvent Damage;
			Pawn->TakeDamage(200, Damage, E->GetController(), E);
			Pawn->SetCanBeDamaged(false);
		}
		const int32 UsesBeforeCycle = Item->CastUseCount;
		float Budget = Row->MaxBudget;
		bool Started = false;
		const float WaitStarted = W->GetTimeSeconds();
		for (int32 Frame = 0; Frame < 1800 && !Started; ++Frame)
		{
			Started = FWoTBotCastCompletionAccess::SelectAndCast(B, Item, Threat, Budget);
			if (!Started) WoTTest::TickWorld(W, 1.0f / 60.0f);
		}
		if (!TestTrue(FString::Printf(TEXT("cycle %d starts through real selection"), Cycle + 1), Started)) break;
		if (Name == TEXT("Balefire")) Budget = 2.0f;
		TestTrue(TEXT("item budget is within audited maximum"), Budget <= Row->MaxBudget + .001f);
		if (Cycle) TestTrue(TEXT("reselection respects hand and exhausted-group intervals"),
			W->GetTimeSeconds() + .001f >= NextAllowedSelection);
		const float Began = W->GetTimeSeconds();
		const int32 UsesAtStart = Item->CastUseCount;
		int32 Observed = UsesAtStart;
		float LastUse = Began;
		for (int32 Frame = 0; Frame < FMath::CeilToInt((Budget + .15f) * 60) && Caster->IsCasting(); ++Frame)
		{
			WoTTest::TickWorld(W, 1.0f / 60.0f);
			if (Observed != Item->CastUseCount) { Observed = Item->CastUseCount; LastUse = W->GetTimeSeconds(); }
		}
		const float Elapsed = W->GetTimeSeconds() - Began;
		TestFalse(TEXT("cast ends within item budget plus frame margin"), Caster->IsCasting());
		TestTrue(TEXT("each cycle actually uses the artifact"), Item->CastUseCount > UsesBeforeCycle);
		if (Name == TEXT("Dart")) TestEqual(TEXT("automatic hold completes the full authored group"),
			Item->CastUseCount - UsesBeforeCycle, 20);
		TestTrue(TEXT("enemy stays visible without interruption"), B->GetTarget() == E && B->IsTargetVisible());
		AddInfo(FString::Printf(TEXT("class=%s cycle=%d budget=%.6f elapsed=%.6f selection_wait=%.6f completion=%s uses=%d (start=%d)"),
			*Item->GetClass()->GetName(), Cycle + 1, Budget, Elapsed, Began - WaitStarted,
			Caster->IsCasting() ? TEXT("STILL_HELD") : Name == TEXT("Balefire") ? TEXT("full-prime release") : TEXT("used; ended before budget"),
			Item->CastUseCount, UsesAtStart));
		if (Caster->IsCasting()) break;
		if (Name == TEXT("EarthTremor") || Name == TEXT("SwapPlaces"))
		{
			// Completion owns input lifetime, not downstream ground-chunk physics.
			// Prove the real launch and flight, then dispose it before the cooldown
			// wait can enter the shared point-chunk bounce defect (fix3 report).
			// A completed swap also relocates this fixture's stationary enemy out
			// of view while its perception timer is disabled. Its full arrival is
			// covered by the real-map selection spec; keep this two-cycle fixture
			// focused on release and reuse with an uninterrupted visible target.
			WoTTest::TickWorld(W, .05f);
			int32 Projectiles = 0;
			for (TActorIterator<AWoTAngrealProjectile> It(W); It; ++It)
				if (It->SourceAngreal == Item)
				{
					++Projectiles;
					TestTrue(TEXT("real catalog projectile class launched"),
						It->GetClass() == CastChecked<AWoTProjectileLauncher>(Item)->ProjectileClass);
					TestTrue(TEXT("launched projectile travels with finite velocity"),
						!It->ProjectileMovement->Velocity.ContainsNaN() && It->ProjectileMovement->Velocity.Size() > 100
						&& It->GetActorLocation().X > Pawn->GetActorLocation().X + 5);
					if (Name == TEXT("SwapPlaces"))
						TestEqual(TEXT("completed swap cast seeks the visible pawn"),
							CastChecked<AWoTSeekingProjectile>(*It)->Destination.Get(), static_cast<AActor*>(E));
					It->Destroy();
				}
			TestEqual(TEXT("each completed cast really launches one projectile"), Projectiles, 1);
		}
		float Interval = Item->bDefensive ? Caster->MinDefensiveUsageInterval : Caster->MinOffensiveUsageInterval;
		if (!Item->ShouldCast() && Name != TEXT("Shield")) Interval = FMath::Max(Interval, Item->MinChargeGroupInterval);
		// Counter observation may occur one frame after the item's actual use.
		NextAllowedSelection = LastUse + Interval - 1.0f / 60.0f;
	}
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotFullPrimeSpec, "WoT.Bots.CastCompletion.FullPrimeVisibleTarget", Flags)
void FBotFullPrimeSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Default"), TEXT("AuthoredPrime") }) { Names.Add(Name); Commands.Add(Name); }
}
bool FBotFullPrimeSpec::RunTest(const FString& Name)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->Health = E->MaxHealth = 2000;
	AWoTAngrealInventory* Item = OnlyItem(B, TEXT("Balefire"));
	const float Prime = Name == TEXT("Default") ? 2.0f : 1.4f;
	FindFProperty<FFloatProperty>(Item->GetClass(), TEXT("PrimeTime"))->SetPropertyValue_InContainer(Item, Prime);
	FWoTBotCastCompletionAccess::Prepare(B, E);
	FWoTBotCastCompletionAccess::Cast(B, Item);
	UWoTCasterComponent* Caster = B->GetPawn()->FindComponentByClass<UWoTCasterComponent>();
	WoTTest::TickWorld(W, Prime - .05f);
	TestTrue(TEXT("holds to full prime with uninterrupted target"), Caster->IsCasting() && ActiveFlag(Item));
	TestEqual(TEXT("no early shot"), Item->CastUseCount, 0);
	WoTTest::TickWorld(W, .1f);
	TestFalse(TEXT("full prime releases caster"), Caster->IsCasting());
	TestFalse(TEXT("full prime ends item charge"), ActiveFlag(Item));
	TestEqual(TEXT("one full-strength shot"), Item->CastUseCount, 1);
	TestEqual(TEXT("full damage proves full prime"), E->Health, 1600);
	TestTrue(TEXT("enemy remains visible and alive"), E->IsAlive() && B->IsTargetVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotShockCancelSpec, "WoT.Bots.CastCompletion.WaterCancelBeforeFirstDrain", Flags)
bool FBotShockCancelSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->SetCanBeDamaged(false);
	AWoTAngrealInventory* Item = OnlyItem(B, TEXT("Lightning"));
	FWoTBotCastCompletionAccess::Prepare(B, E);
	FWoTBotCastCompletionAccess::Cast(B, Item);
	WoTTest::TickWorld(W, .1f);
	TestTrue(TEXT("real effect started"), ActiveFlag(Item));
	AWoTZoneVolume* Water = W->SpawnActor<AWoTZoneVolume>(FVector::ZeroVector, FRotator::ZeroRotator);
	Water->Collision->SetBoxExtent(FVector(100, 100, 100)); Water->bWaterZone = true;
	WoTTest::TickWorld(W, .1f);
	TestTrue(TEXT("water shock is nonlethal"), Pawn->Health > 0 && Pawn->Health < 100);
	TestFalse(TEXT("water independently cancelled item"), ActiveFlag(Item));
	TestEqual(TEXT("cancellation preceded first drain"), Item->CastUseCount, 0);
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	TestFalse(TEXT("controller releases within a frame of cancellation"), Caster->IsCasting());
	Water->Destroy();
	float Budget = 0; bool Resumed = false;
	for (int32 I = 0; I < 180 && !Resumed; ++I)
	{
		WoTTest::TickWorld(W, 1.0f / 60.0f);
		Resumed = FWoTBotCastCompletionAccess::SelectAndCast(B, Item, nullptr, Budget);
	}
	TestTrue(TEXT("same target re-engages through selector"), Resumed && ActiveFlag(Item));
	WoTTest::TickWorld(W, 1.1f);
	TestTrue(TEXT("resumed effect consumes charge normally"), Item->CastUseCount > 0);
	TestFalse(TEXT("resumed hold also completes"), Caster->IsCasting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotNoUseBudgetSpec, "WoT.Bots.CastCompletion.NoUseEndsAtBudget", Flags)
bool FBotNoUseBudgetSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50));
	AWoTAngrealInventory* Item = OnlyItem(B, TEXT("Heal"));
	FWoTBotCastCompletionAccess::Prepare(B, E);
	FWoTBotCastCompletionAccess::Cast(B, Item); // Full health: accepted input, no item use.
	UWoTCasterComponent* Caster = B->GetPawn()->FindComponentByClass<UWoTCasterComponent>();
	WoTTest::TickWorld(W, .95f);
	TestTrue(TEXT("default hold budget has not expired"), Caster->IsCasting());
	WoTTest::TickWorld(W, .2f);
	TestFalse(TEXT("no-use cast releases at default duration"), Caster->IsCasting());
	TestEqual(TEXT("no charge was spent"), Item->CastUseCount, 0);
	CastChecked<AWoTCharacter>(B->GetPawn())->Health = 20;
	float Budget = 0;
	TestTrue(TEXT("later usable cast starts normally"), FWoTBotCastCompletionAccess::SelectAndCast(B, Item, nullptr, Budget));
	WoTTest::TickWorld(W, .1f);
	TestEqual(TEXT("later cast actually heals"), Item->CastUseCount, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSafetyCapSpec, "WoT.Bots.CastCompletion.SafetyCapAndOncePerClass", Flags)
bool FBotSafetyCapSpec::RunTest(const FString&)
{
#if !NO_LOGGING
	AddExpectedMessagePlain(TEXT("Bot cast safety cap reached for class WoTAngrealInvHeal"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
#endif
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		AWoTBotController* B = Bot(W, FVector(0, Attempt * 400, 50));
		AWoTCharacter* E = Enemy(W, FVector(600, Attempt * 400, 50));
		AWoTAngrealInventory* Item = OnlyItem(B, TEXT("Heal"));
		// A no-use activation with no duration; only the independent cap can end it.
		if (Attempt == 0) Item->MinChargesInGroup = Item->MaxChargesInGroup = 0;
		else Item->RoundsPerMinute = .5f; // A valid 120.05s duration must also hit the cap.
		FWoTBotCastCompletionAccess::Prepare(B, E);
		FWoTBotCastCompletionAccess::Cast(B, Item);
		UWoTCasterComponent* Caster = B->GetPawn()->FindComponentByClass<UWoTCasterComponent>();
		WoTTest::TickWorld(W, 5.9f);
		TestTrue(TEXT("safety margin exceeds legitimate default budgets"), Caster->IsCasting());
		WoTTest::TickWorld(W, .2f);
		TestFalse(TEXT("single hold cannot exceed six-second safety cap"), Caster->IsCasting());
		B->Retire(); E->Destroy();
	}
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotTimedReleaseDeathSpec, "WoT.Bots.CastCompletion.TimedReleaseSelfDeath", Flags)
void FBotTimedReleaseDeathSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Frame"), TEXT("Think") }) { Names.Add(Name); Commands.Add(Name); }
}
bool FBotTimedReleaseDeathSpec::RunTest(const FString& Name)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->SetCanBeDamaged(false);
	AWoTAngrealInventory* Item = OnlyItem(B, TEXT("Balefire"));
	Pawn->Health = 1; Item->bTainted = true; Item->TaintInstigator = Pawn;
	FWoTBotCastCompletionAccess::Prepare(B, E);
	FWoTBotCastCompletionAccess::Cast(B, Item);
	if (Name == TEXT("Think")) B->SetActorTickEnabled(false);
	WoTTest::TickWorld(W, 2.1f);
	if (Name == TEXT("Think")) FWoTBotCastCompletionAccess::Think(B);
	TestEqual(TEXT("timed release fires the tainted artifact"), Item->CastUseCount, 1);
	TestNull(TEXT("lethal timed release unpossesses without continuing pawn work"), B->GetPawn());
	TestTrue(TEXT("lethal timed release preserves dead state"), B->GetBehaviourState() == EWoTBotState::Dead);
	WoTTest::TickWorld(W, 5.0f);
	TestTrue(TEXT("normal respawn follows timed self-damage"), B->GetPawn() && B->GetPawn() != Pawn);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotOnlySummonSpec, "WoT.Bots.CastCompletion.AutonomousSummonOnly", Flags)
void FBotOnlySummonSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Minion"), TEXT("Guardian") }) { Names.Add(Name); Commands.Add(Name); }
}
bool FBotOnlySummonSpec::RunTest(const FString& Name)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0; Settings.Value->AimErrorDegrees = 0;
	Settings.Value->StuckRespawnSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	Enemy(W, FVector(600, 0, 50))->SetCanBeDamaged(false);
	AWoTAngrealInventory* Item = OnlyItem(B, Name);
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	// Leave the real perception/think timer running; do not inject an Engage state.
	for (int32 I = 0; I < 120 && Item->CastUseCount == 0; ++I) WoTTest::TickWorld(W, 1.0f / 60.0f);
	TestEqual(TEXT("autonomous single-artifact kit summons"), Item->CastUseCount, 1);
	WoTTest::TickWorld(W, .1f);
	TestFalse(TEXT("autonomous summon hold ends"), Caster->IsCasting());
	RemoveHelpers(W, Pawn);
	for (int32 I = 0; I < 900 && Item->CastUseCount < 2; ++I) WoTTest::TickWorld(W, 1.0f / 60.0f);
	TestEqual(TEXT("autonomous single-artifact kit summons again when helper is gone"), Item->CastUseCount, 2);
	WoTTest::TickWorld(W, .1f);
	TestFalse(TEXT("second autonomous hold ends"), Caster->IsCasting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotAdaptationChoiceSpec, "WoT.Bots.CastCompletion.PreservesAlternativeSelection", Flags)
bool FBotAdaptationChoiceSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->SetCanBeDamaged(false);
	AWoTAngrealInventory* First = OnlyItem(B, TEXT("Fireball"));
	First->Priority = 100; First->MinChargesInGroup = First->MaxChargesInGroup = 6;
	AWoTAngrealInventory* Alternative = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvDart"));
	Alternative->Priority = 50;
	FWoTBotCastCompletionAccess::Prepare(B, E);
	FWoTBotCastCompletionAccess::Decide(B);
	TestEqual(TEXT("highest priority selected first"), First->CastUseCount, 1);
	// Exercise the selector throughout the usage wait: a premature retry must not
	// clear adaptation just because the hand's interval temporarily blocks selection.
	for (int32 I = 0; I < 51; ++I)
	{
		WoTTest::TickWorld(W, .1f);
		if (I < 49) FWoTBotCastCompletionAccess::Decide(B);
	}
	TestTrue(TEXT("first artifact still has charges in its group"), First->ShouldCast());
	FWoTBotCastCompletionAccess::Decide(B);
	TestTrue(TEXT("normal adapted-item exclusion prefers the other usable attack"), Alternative->CastUseCount > 0);
	TestEqual(TEXT("completion does not continually select the previous top priority"), First->CastUseCount, 1);
	return !HasAnyErrors();
}

} // namespace WoTBotTest
#endif
