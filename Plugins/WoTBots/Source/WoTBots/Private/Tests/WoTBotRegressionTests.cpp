#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "AI/WoTCasterComponent.h"
#include "Engine/DamageEvents.h"
#include "Testing/WoTSoakEvents.h"
#include "UObject/UnrealType.h"
#include "TimerManager.h"
namespace WoTBotTest
{

// Invoke the real decision/cast boundaries without depending on timer phase.
struct FWoTBotRegressionAccess
{
	static void Think(AWoTBotController* Bot)
	{
		Bot->NextRepathTime = 0;
		Bot->Think();
	}
	static void CastItem(AWoTBotController* Bot, AWoTAngrealInventory* Item, AWoTCharacter* Victim)
	{
		Bot->GetWorldTimerManager().ClearAllTimersForObject(Bot);
		Bot->SetTarget(Victim, true);
		Item->SetVictim(Victim);
		Item->InitChargeGroup();
		Bot->CastItem(Item);
	}
	static bool WantsMovement(const AWoTBotController* Bot) { return Bot->bWantsMovement; }
};

namespace
{
bool ItemFlag(const AWoTAngrealInventory* Item, const TCHAR* Name)
{
	const FBoolProperty* Property = FindFProperty<FBoolProperty>(Item->GetClass(), Name);
	// Non-replicated held effects enable their actor tick only during the cast.
	return Property ? Property->GetPropertyValue_InContainer(Item) : Item->IsActorTickEnabled();
}

bool HasEffect(AActor* Actor, const TCHAR* ClassName)
{
	UClass* Class = FindObject<UClass>(nullptr, ClassName);
	check(Class);
	for (UActorComponent* Component : Actor->GetComponents())
		if (IsValid(Component) && Component->IsRegistered() && Component->IsA(Class)) return true;
	return false;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotHeldPolicySpec, "WoT.Bots.Regression.HeldArtifactPolicy", Flags)
void FBotHeldPolicySpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Lightning"), TEXT("Whirlwind"), TEXT("Levitate"), TEXT("Disguise"), TEXT("WallOfAir") })
	{ Names.Add(Name); Commands.Add(Name); }
}
bool FBotHeldPolicySpec::RunTest(const FString& Name)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(150, 0, 50));
	AWoTAngrealInventory* Item = Give(Pawn, *FString::Printf(TEXT("/Script/WoT.WoTAngrealInv%s"), *Name));
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	FWoTBotRegressionAccess::CastItem(B, Item, E);
	const TCHAR* Flag = Name == TEXT("Levitate") ? TEXT("bLevitating") : TEXT("bCasting");
	TestTrue(TEXT("cast stays held until the caster observes item policy"), Caster->IsCasting());
	TestTrue(TEXT("held artifact remains active after BeginCast"), ItemFlag(Item, Flag));
	if (Name == TEXT("Disguise")) TestNotNull(TEXT("appearance applied while held"), Pawn->ApparentClass.Get());
	if (Name == TEXT("WallOfAir")) TestTrue(TEXT("wall lifetime link remains installed"),
		HasEffect(E, TEXT("/Script/WoT.WoTDestroyOnDestroyLeech")));
	if (Name == TEXT("Lightning") || Name == TEXT("Whirlwind"))
	{
		WoTTest::TickWorld(W, .25f);
		TestTrue(TEXT("held effect survives initial frames"), Caster->IsCasting() && ItemFlag(Item, Flag));
		if (Name == TEXT("Lightning"))
		{
			TestTrue(TEXT("latch tick attaches damaging effect"), HasEffect(E, TEXT("/Script/WoT.WoTDecreaseHealthLeech")));
			TestTrue(TEXT("latched effect damages victim"), E->Health < 100);
		}
		else
		{
			TestTrue(TEXT("lift effect remains attached"), HasEffect(E, TEXT("/Script/WoT.WoTWhirlwindLeech")));
			WoTTest::TickWorld(W, .5f);
			TestTrue(TEXT("lift lasts while the item is held"), Caster->IsCasting()
				&& HasEffect(E, TEXT("/Script/WoT.WoTWhirlwindLeech")));
		}
	}
	WoTTest::TickWorld(W, 3.3f);
	TestTrue(TEXT("real item consumed a charge"), Item->CastUseCount > 0);
	TestFalse(TEXT("caster ends hold after charge policy requests release"), Caster->IsCasting());
	TestFalse(TEXT("held artifact deactivates on policy release"), ItemFlag(Item, Flag));
	if (Name == TEXT("Whirlwind")) TestFalse(TEXT("release removes lift effect"), HasEffect(E, TEXT("/Script/WoT.WoTWhirlwindLeech")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotCastCompletionSpec, "WoT.Bots.Regression.ShotReleaseAndTargetLoss", Flags)
bool FBotCastCompletionSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->SetCanBeDamaged(false);
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	AWoTAngrealInventory* Shot = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvFireball"));
	FWoTBotRegressionAccess::CastItem(B, Shot, E);
	TestEqual(TEXT("single shot fires immediately"), Shot->CastUseCount, 1);
	TestTrue(TEXT("single shot completion belongs to caster tick"), Caster->IsCasting());
	WoTTest::TickWorld(W, .1f);
	TestFalse(TEXT("single shot releases after usage observation"), Caster->IsCasting());
	TestEqual(TEXT("no repeated shot"), Shot->CastUseCount, 1);
	WoTTest::TickWorld(W, 1.0f); // Let the real shared input cadence expire.
	AWoTAngrealInventory* Held = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvWhirlwind"));
	FWoTBotRegressionAccess::CastItem(B, Held, E);
	TestTrue(TEXT("hold begins"), Caster->IsCasting());
	E->SetActorLocation(FVector(-600, 0, 50));
	B->Tick(1.0f / 60.0f);
	TestFalse(TEXT("target loss releases immediately"), Caster->IsCasting());
	TestFalse(TEXT("target loss removes held effect"), HasEffect(E, TEXT("/Script/WoT.WoTWhirlwindLeech")));
	WoTTest::TickWorld(W, .1f);
	E->SetActorLocation(FVector(600, 0, 50));
	AWoTAngrealInventory* Charged = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvBalefire"));
	FWoTBotRegressionAccess::CastItem(B, Charged, E);
	WoTTest::TickWorld(W, 1.2f);
	TestEqual(TEXT("release-fired artifact holds without premature shot"), Charged->CastUseCount, 0);
	E->SetActorLocation(FVector(-600, 0, 50));
	B->Tick(1.0f / 60.0f);
	TestEqual(TEXT("target loss fires charged artifact through real release"), Charged->CastUseCount, 1);
	TestFalse(TEXT("release-fired cast does not linger"), Caster->IsCasting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotCloseRangeSpec, "WoT.Bots.Regression.EmptyRangedKitApproachesAndCasts", Flags)
bool FBotCloseRangeSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->SetCanBeDamaged(false);
	Kit(B)->CurCharges = 0;
	AWoTAngrealInventory* Close = CastChecked<AWoTAngrealInventory>(Pawn->InventoryComponent->FindInventoryType(Pawn->DefaultAngrealInventory));
	float Nearest = 600;
	for (int32 I = 0; I < 600 && Close->CastUseCount == 0; ++I)
	{
		WoTTest::TickWorld(W, 1.0f / 60.0f);
		if (!B->GetPawn()) break;
		Nearest = FMath::Min(Nearest, float(FVector::Dist(Pawn->GetActorLocation(), E->GetActorLocation())));
	}
	AddInfo(FString::Printf(TEXT("closest_distance=%.2f close_uses=%d"), Nearest, Close->CastUseCount));
	TestTrue(TEXT("empty ranged kit approaches into remaining artifact range"), Nearest <= Close->GetMaxRange());
	TestTrue(TEXT("real selector casts close artifact after approaching"), Close->CastUseCount > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotNoOffenceSpec, "WoT.Bots.Regression.NoOffenceSeeksDespiteVisibleEnemy", Flags)
bool FBotNoOffenceSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	const auto Items = Pawn->InventoryComponent->Items;
	for (AWoTInventory* Item : Items) { Pawn->InventoryComponent->DeleteInventory(Item); Item->Destroy(); }
	Enemy(W, FVector(600, 0, 50));
	AWoTAngrealSpawnPool* Goal = Pool(W, FVector(-400, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	FWoTBotRegressionAccess::Think(B);
	TestTrue(TEXT("visible enemy does not prevent unarmed replenishment"), B->GetBehaviourState() == EWoTBotState::SeekItem);
	WoTTest::TickWorld(W, 2.0f);
	TestNull(TEXT("unarmed bot physically collects offence"), Goal->PickupItem.Get());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotLandingResumeSpec, "WoT.Bots.Regression.HeldVolleyResumesAfterHardLanding", Flags)
bool FBotLandingResumeSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->SetCanBeDamaged(false);
	AWoTAngrealInventory* Item = Kit(B);
	Item->MinChargesInGroup = Item->MaxChargesInGroup; // Interrupt a known multi-shot volley.
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	for (int32 I = 0; I < 90 && Item->CastUseCount == 0; ++I) WoTTest::TickWorld(W, 1.0f / 60.0f);
	TestTrue(TEXT("real starting volley active"), Caster->IsCasting() && Item->CastUseCount > 0);
	const int32 Before = Item->CastUseCount;
	Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::LandingFreeze);
	WoTTest::TickWorld(W, .2f);
	TestFalse(TEXT("landing interrupts held volley"), Caster->IsCasting());
	TestEqual(TEXT("landing prevents further shots"), Item->CastUseCount, Before);
	TestTrue(TEXT("same target remains visible throughout interruption"), B->GetTarget() == E && B->IsTargetVisible());
	Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	WoTTest::TickWorld(W, 5.0f);
	TestTrue(TEXT("charged starting offence resumes after landing without losing target"), Item->CastUseCount > Before);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotLethalCastSpec, "WoT.Bots.Regression.LethalSelfCastDuringRepathRespawns", Flags)
bool FBotLethalCastSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	Enemy(W, FVector(100, 0, 50))->SetCanBeDamaged(false);
	Kit(B)->CurCharges = 0;
	AWoTAngrealInventory* Close = CastChecked<AWoTAngrealInventory>(Pawn->InventoryComponent->FindInventoryType(Pawn->DefaultAngrealInventory));
	Close->bTainted = true;
	Close->TaintInstigator = Pawn;
	Pawn->Health = 1;
	W->GetAuthGameMode<AWoTGameMode>()->RespawnDelay = .1f;
	AddInfo(TEXT("entering repath-due think with lethal tainted cast"));
	FWoTBotRegressionAccess::Think(B);
	TestFalse(TEXT("real cast killed owner"), Pawn->IsAlive());
	TestNull(TEXT("death unpossessed during think"), B->GetPawn());
	TestTrue(TEXT("death state retained after cast"), B->GetBehaviourState() == EWoTBotState::Dead);
	WoTTest::TickWorld(W, 1.3f);
	TestTrue(TEXT("normal respawn creates living replacement"), B->GetPawn() && B->GetPawn() != Pawn
		&& CastChecked<AWoTCharacter>(B->GetPawn())->IsAlive());
	TestEqual(TEXT("self-inflicted death scored once"), B->GetPlayerState<AWoTPlayerState>()->Suicides, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotLethalReleaseSpec, "WoT.Bots.Regression.LethalReleaseDuringThinkAndFrame", Flags)
bool FBotLethalReleaseSpec::RunTest(const FString&)
{
	for (bool InThink : { true, false })
	{
		FSettings Settings; Settings.Value->AimErrorDegrees = 0;
		UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
		BuildNav(W);
		AWoTBotController* B = Bot(W, FVector(0, 0, 50));
		AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
		AWoTCharacter* E = Enemy(W, FVector(600, 0, 50)); E->SetCanBeDamaged(false);
		AWoTAngrealInventory* Item = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvBalefire"));
		FWoTBotRegressionAccess::CastItem(B, Item, E);
		WoTTest::TickWorld(W, 1.2f);
		Item->bTainted = true; Item->TaintInstigator = Pawn;
		Pawn->Health = 1;
		W->GetAuthGameMode<AWoTGameMode>()->RespawnDelay = .1f;
		E->SetActorLocation(FVector(-600, 0, 50));
		if (InThink) FWoTBotRegressionAccess::Think(B);
		else B->Tick(1.0f / 60.0f);
		TestFalse(TEXT("release applies lethal self-damage"), Pawn->IsAlive());
		TestNull(TEXT("release unpossesses before update resumes"), B->GetPawn());
		TestTrue(TEXT("update preserves death state"), B->GetBehaviourState() == EWoTBotState::Dead);
		TestNull(TEXT("target loss cannot restore target after death"), B->GetTarget());
		WoTTest::TickWorld(W, 1.3f);
		TestTrue(TEXT("release death also respawns normally"), B->GetPawn() && B->GetPawn() != Pawn);
	}
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotHoldInterruptionSpec, "WoT.Bots.Regression.HoldInterruptions", Flags)
void FBotHoldInterruptionSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("TargetSwitch"), TEXT("ItemSwitch"), TEXT("StateChange"), TEXT("Departure"), TEXT("Freeze"), TEXT("Death") })
	{ Names.Add(Name); Commands.Add(Name); }
}
bool FBotHoldInterruptionSpec::RunTest(const FString& Name)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50));
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	AWoTAngrealInventory* Item = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvWhirlwind"));
	FWoTBotRegressionAccess::Think(B);
	FWoTBotRegressionAccess::CastItem(B, Item, E);
	WoTTest::TickWorld(W, .1f);
	TestTrue(TEXT("held effect established before interruption"), HasEffect(E, TEXT("/Script/WoT.WoTWhirlwindLeech")));
	if (Name == TEXT("TargetSwitch"))
	{
		AWoTCharacter* Other = Enemy(W, FVector(500, 200, 50), 2);
		FDamageEvent Event;
		Pawn->TakeDamage(1, Event, Other->GetController(), Other);
		TestEqual(TEXT("damage switches observed target"), B->GetTarget(), Other);
	}
	else if (Name == TEXT("ItemSwitch"))
	{
		AWoTAngrealInventory* Shot = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvFireball"));
		FWoTBotRegressionAccess::CastItem(B, Shot, E);
		TestEqual(TEXT("replacement item fires"), Shot->CastUseCount, 1);
		WoTTest::TickWorld(W, .1f);
	}
	else if (Name == TEXT("StateChange"))
	{
		Kit(B)->CurCharges = 0; Item->CurCharges = 2;
		E->SetActorLocation(FVector(2000, 0, 50));
		Pool(W, FVector(-400, 0, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
		FWoTBotRegressionAccess::Think(B);
		TestTrue(TEXT("low charges switch engagement to seeking"), B->GetBehaviourState() == EWoTBotState::SeekItem);
	}
	else if (Name == TEXT("Departure")) B->SetDeparturePending(true);
	else if (Name == TEXT("Freeze")) W->GetAuthGameMode<AWoTGameMode>()->EndGame(NAME_None);
	else
	{
		FDamageEvent Event;
		Pawn->TakeDamage(1000, Event, E->GetController(), E);
		TestNull(TEXT("death disposes pawn"), B->GetPawn());
	}
	TestFalse(TEXT("interruption releases held cast"), Caster->IsCasting());
	TestFalse(TEXT("interruption removes held effect"), HasEffect(E, TEXT("/Script/WoT.WoTWhirlwindLeech")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotPendingFreezeSpec, "WoT.Bots.Regression.FreezeCancelsPendingAndHeldActivation", Flags)
bool FBotPendingFreezeSpec::RunTest(const FString&)
{
	for (bool Pending : { true, false })
	{
		FSettings Settings; Settings.Value->AimErrorDegrees = 0;
		UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
		AWoTBotController* B = Bot(W, FVector(0, 0, 50));
		AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
		AWoTCharacter* E = Enemy(W, FVector(600, 0, 50));
		AWoTAngrealInventory* Item = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvBalefire"));
		UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
		FWoTBotRegressionAccess::CastItem(B, Item, E);
		if (Pending) Caster->Release(); // Early release arms the artifact's own auto-prime timer.
		TestTrue(TEXT("charged activation exists before freeze"), ItemFlag(Item, TEXT("bCasting")));
		TestEqual(TEXT("shot has not fired yet"), Item->CastUseCount, 0);
		const int32 Charges = Item->CurCharges;
		W->GetAuthGameMode<AWoTGameMode>()->EndGame(NAME_None);
		TestFalse(TEXT("freeze cancels activation before release"), ItemFlag(Item, TEXT("bCasting")));
		WoTTest::TickWorld(W, 1.5f);
		TestEqual(TEXT("pending shot never fires after freeze"), Item->CastUseCount, 0);
		TestEqual(TEXT("freeze does not spend a charge"), Item->CurCharges, Charges);
		TestEqual(TEXT("pending beam does not hurt victim"), E->Health, 100);
		TestFalse(TEXT("caster released after cancellation"), Caster->IsCasting());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotWhirlwindSpinSpec, "WoT.Bots.Movement.WhirlwindSpinsBot", Flags)
bool FBotWhirlwindSpinSpec::RunTest(const FString&)
{
	// The bot does not cast, and three seconds without progress would retire a
	// body held still, so the hold below outlasts the stuck rule.
	FSettings Settings; Settings.Value->ReactionSeconds = 100; Settings.Value->AimErrorDegrees = 0;
	Settings.Value->StuckRespawnSeconds = 3;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	SeedDecisions(0x7051);
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	const TWeakObjectPtr<AWoTCharacter> Body = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(600, 0, 50));
	int32 StuckEvents = 0;
	const FDelegateHandle Handle = WoTSoakEvents::Events().AddLambda([&](WoTSoakEvents::EKind Kind, AActor* Subject, AActor*)
	{
		if (Subject == B && (Kind == WoTSoakEvents::EKind::Recovery || Kind == WoTSoakEvents::EKind::SilentRespawn)) ++StuckEvents;
	});
	ON_SCOPE_EXIT { WoTSoakEvents::Events().Remove(Handle); };
	WoTTest::TickWorld(W, .5f);
	if (!TestTrue(TEXT("the bot is under way when the lift begins"), FWoTBotRegressionAccess::WantsMovement(B))) return false;
	// A player holds a Whirlwind on the bot, through the player's own artifact path.
	AWoTAngrealInventory* Whirlwind = Give(E, TEXT("/Script/WoT.WoTAngrealInvWhirlwind"));
	E->InventoryComponent->SelectItem(Whirlwind);
	E->AuthorityUseAngreal((Body->GetActorLocation() - E->GetActorLocation()).Rotation());
	const TCHAR* Lift = TEXT("/Script/WoT.WoTWhirlwindLeech");
	if (!TestTrue(TEXT("the lift holds the bot"), HasEffect(Body.Get(), Lift))) return false;
	// WhirlwindLeech.uc Tick turns its owner int(RotationPerSecond * DeltaTime) + 1
	// units a tick (RotationPerSecond 120000); 1/64 s ticks are exact in floats.
	constexpr float Step = 1.0f / 64.0f;
	constexpr int32 Frames = 320; // Five seconds.
	const double Turn = (FMath::TruncToInt(120000.0f * Step) + 1) * 360.0 / 65536.0;
	double LastBody = Body->GetActorRotation().Yaw, LastView = B->GetControlRotation().Yaw;
	double BodyTurned = 0, ViewTurned = 0, Apart = 0;
	int32 Held = 0;
	for (; Held < Frames; ++Held)
	{
		WoTTest::TickWorld(W, Step, Step);
		if (!Body.IsValid() || B->GetPawn() != Body.Get()) break;
		const double BodyYaw = Body->GetActorRotation().Yaw, ViewYaw = B->GetControlRotation().Yaw;
		BodyTurned += FMath::UnwindDegrees(BodyYaw - LastBody);
		ViewTurned += FMath::UnwindDegrees(ViewYaw - LastView);
		Apart = FMath::Max(Apart, FMath::Abs(FMath::UnwindDegrees(ViewYaw - BodyYaw)));
		LastBody = BodyYaw;
		LastView = ViewYaw;
	}
	AddInfo(FString::Printf(TEXT("WHIRLWIND frames=%d body_turned=%.3f view_turned=%.3f expected=%.3f max_apart=%.3f stuck_events=%d"),
		Held, BodyTurned, ViewTurned, Frames * Turn, Apart, StuckEvents));
	if (!TestEqual(TEXT("the lifted body is still the bot's"), Held, Frames)) return false;
	TestTrue(TEXT("the body turns at the lift's rate"), FMath::IsNearlyEqual(BodyTurned, Frames * Turn, .5));
	// The view follows the body, at most one tick behind it.
	TestTrue(TEXT("the view turns with the body"), FMath::IsNearlyEqual(ViewTurned, BodyTurned, Turn + .01) && Apart <= Turn + .01);
	TestEqual(TEXT("a lifted body is not stuck"), StuckEvents, 0);
	TestTrue(TEXT("the lift still holds the bot"), HasEffect(Body.Get(), Lift));
	E->AuthorityCeaseUsingAngreal();
	TestFalse(TEXT("release ends the lift"), HasEffect(Body.Get(), Lift));
	WoTTest::TickWorld(W, .5f);
	if (!TestTrue(TEXT("the released body is still the bot's"), Body.IsValid() && B->GetPawn() == Body.Get())) return false;
	const double View = B->GetControlRotation().Yaw;
	const double ToCaster = (E->GetActorLocation() - Body->GetPawnViewLocation()).Rotation().Yaw;
	AddInfo(FString::Printf(TEXT("WHIRLWIND released view=%.3f body=%.3f to_caster=%.3f"), View, Body->GetActorRotation().Yaw, ToCaster));
	TestTrue(TEXT("released, the bot aims at its target again"), FMath::Abs(FMath::UnwindDegrees(View - ToCaster)) < 2.0);
	TestTrue(TEXT("and faces where it aims"), FMath::Abs(FMath::UnwindDegrees(Body->GetActorRotation().Yaw - View)) < .01);
	TestEqual(TEXT("no stuck recovery after release"), StuckEvents, 0);
	return !HasAnyErrors();
}
} // namespace WoTBotTest
#endif
