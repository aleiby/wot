#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTNPCCharacter.h"
#include "Inventory/WoTAngrealInvHeal.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "Inventory/WoTResourceSpawnerInv.h"
#include "Projectiles/WoTAngrealProjectile.h"
#include "Projectiles/WoTProjectile.h"
#include "Engine/DamageEvents.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "TimerManager.h"
namespace WoTBotTest
{

struct FWoTBotBehaviourAccess
{
	static void SetThinkPhase(AWoTBotController* Bot, float Phase)
	{
		Bot->GetWorldTimerManager().SetTimer(Bot->ThinkTimer, Bot, &AWoTBotController::Think,
			GetDefault<UWoTBotSettings>()->ThinkInterval, true, Phase);
	}
	static void SetAimDisk(AWoTBotController* Bot, FVector2D Disk)
	{
		Bot->AimDisk = Disk;
		Bot->UpdateAim();
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSightSpec, "WoT.Bots.Perception.ConeRangeAndHeadLOS", Flags)
bool FBotSightSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(800, 0, 50));
	TestTrue(TEXT("in front in range"), B->CanSee(E));
	E->SetActorLocation(FVector(-800, 0, 50));
	TestFalse(TEXT("outside cone"), B->CanSee(E));
	E->SetActorLocation(FVector(4500, 0, 50));
	TestFalse(TEXT("outside range"), B->CanSee(E));
	E->SetActorLocation(FVector(800, 0, 50));
	float Radius, HalfHeight; E->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const FVector HeadTraceMidpoint = FMath::Lerp(B->GetPawn()->GetPawnViewLocation(),
		E->GetActorLocation() + FVector(0, 0, HalfHeight * .8f), .5f);
	AActor* HeadCover = Box(W, HeadTraceMidpoint, FVector(20, 300, 3));
	TestFalse(TEXT("head cover rejects even when center and feet are exposed"), B->CanSee(E));
	HeadCover->Destroy();
	AActor* Wall = Box(W, FVector(400, 0, 100), FVector(30, 300, 150));
	TestFalse(TEXT("real wall occludes"), B->CanSee(E));
	WoTTest::TickWorld(W, .2f);
	TestNull(TEXT("occluded enemy never acquired"), B->GetTarget());
	Wall->Destroy(); WoTTest::TickWorld(W, .2f);
	TestEqual(TEXT("visible enemy acquired by think"), B->GetTarget(), E);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDamageMemorySpec, "WoT.Bots.Perception.DamageRevealAndMemoryExpiry", Flags)
bool FBotDamageMemorySpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100; Settings.Value->TargetMemorySeconds = .5f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(-800, 0, 50));
	Box(W, FVector(-400, 0, 100), FVector(30, 300, 150));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	FDamageEvent Event;
	Pawn->TakeDamage(5, Event, E->GetController(), E);
	TestEqual(TEXT("damage immediately identifies instigator"), B->GetTarget(), E);
	TestTrue(TEXT("turns toward attack"), FVector::DotProduct(B->GetControlRotation().Vector(), FVector(-1, 0, 0)) > .9f);
	const FVector Known = B->GetLastKnownPosition();
	E->SetActorLocation(FVector(-1200, 600, 50));
	WoTTest::TickWorld(W, .25f);
	TestTrue(TEXT("memory keeps observed position without tracking through wall"), B->GetLastKnownPosition().Equals(Known));
	TestEqual(TEXT("memory still live"), B->GetTarget(), E);
	WoTTest::TickWorld(W, .4f);
	TestNull(TEXT("memory expires"), B->GetTarget());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotHysteresisSpec, "WoT.Bots.Perception.HysteresisAndRecentAttacker", Flags)
bool FBotHysteresisSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* First = Enemy(W, FVector(800, 0, 50));
	AWoTCharacter* Second = Enemy(W, FVector(850, 100, 50), 2);
	WoTTest::TickWorld(W, .2f);
	TestEqual(TEXT("nearest acquired"), B->GetTarget(), First);
	Second->SetActorLocation(FVector(750, 100, 50));
	WoTTest::TickWorld(W, .2f);
	TestEqual(TEXT("small distance change does not switch"), B->GetTarget(), First);
	Second->SetActorLocation(FVector(1300, 100, 50));
	FDamageEvent Event;
	CastChecked<AWoTCharacter>(B->GetPawn())->TakeDamage(1, Event, Second->GetController(), Second);
	WoTTest::TickWorld(W, .2f);
	TestEqual(TEXT("recent attacker wins"), B->GetTarget(), Second);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotDisguiseSpec, "WoT.Bots.Perception.Disguise", Flags)
void FBotDisguiseSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("BeforeFirstThink.Early"), TEXT("BeforeFirstThink.Late"),
		TEXT("AfterAcquisition.Early"), TEXT("AfterAcquisition.Late") })
	{ Names.Add(Name); Commands.Add(Name); }
}
bool FBotDisguiseSpec::RunTest(const FString& Name)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 0; Settings.Value->TargetMemorySeconds = .5f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(400, 0, 50));
	E->SetCanBeDamaged(false);
	FWoTBotBehaviourAccess::SetThinkPhase(B, Name.EndsWith(TEXT("Early")) ? .01f : Settings.Value->ThinkInterval);
	AWoTAngrealInventory* Attack = Kit(B);
	Attack->MinChargesInGroup = Attack->MaxChargesInGroup;
	UWoTCasterComponent* Caster = B->GetPawn()->FindComponentByClass<UWoTCasterComponent>();
	AWoTAngrealInventory* Item = Give(E, TEXT("/Script/WoT.WoTAngrealInvDisguise"));
	TestTrue(TEXT("enemy initially visible"), B->CanSee(E));
	const bool Acquired = Name.StartsWith(TEXT("After"));
	if (Acquired)
	{
		WoTTest::TickWorld(W, .2f); // Beyond every possible initial think phase.
		TestEqual(TEXT("hostile target acquired before disguise"), B->GetTarget(), E);
		TestTrue(TEXT("real volley active before disguise"), Caster->IsCasting() && Attack->CastUseCount > 0);
	}
	else TestEqual(TEXT("disguise precedes any think"), B->GetThinkCount(), uint64(0));
	const int32 Uses = Attack->CastUseCount;
	const FVector Known = B->GetLastKnownPosition();
	E->InventoryComponent->SelectItem(Item);
	E->AuthorityUseAngreal(FRotator::ZeroRotator);
	TestFalse(TEXT("real disguise adopts friendly apparent team"), B->CanSee(E));
	E->SetActorLocation(FVector(600, 100, 50));
	WoTTest::TickWorld(W, .2f);
	TestFalse(TEXT("friendly apparent team is not a visible enemy"), B->IsTargetVisible());
	TestFalse(TEXT("disguise interrupts offensive hold"), Caster->IsCasting());
	TestEqual(TEXT("no further uses while disguised"), Attack->CastUseCount, Uses);
	if (Acquired)
	{
		// The shared NPC brain retains an existing threat goal when faction changes.
		// Bots retain only their last observation, subject to the normal memory limit.
		TestEqual(TEXT("previous threat remains in memory"), B->GetTarget(), E);
		TestTrue(TEXT("disguise cannot refresh remembered position"), B->GetLastKnownPosition().Equals(Known));
		WoTTest::TickWorld(W, .5f);
		TestNull(TEXT("friendly threat memory expires normally"), B->GetTarget());
	}
	else TestNull(TEXT("disguised player never acquired"), B->GetTarget());
	E->AuthorityCeaseUsingAngreal();
	TestTrue(TEXT("removing disguise restores hostility"), B->CanSee(E));
	WoTTest::TickWorld(W, .2f);
	TestEqual(TEXT("removing disguise permits acquisition again"), B->GetTarget(), E);
	TestTrue(TEXT("restored target is visible"), B->IsTargetVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotReactionReacquireSpec, "WoT.Bots.Casting.ReactionAndStartingKitReacquisition", Flags)
bool FBotReactionReacquireSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = .4f;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(1000, 0, 50)); E->SetCanBeDamaged(false);
	AWoTAngrealInventory* Item = Kit(B);
	WoTTest::TickWorld(W, .3f);
	TestEqual(TEXT("reaction holds starting projectile"), Item->CastUseCount, 0);
	WoTTest::TickWorld(W, 1.0f);
	TestTrue(TEXT("real projectile fired after reaction"), Item->CastUseCount > 0);
	const int32 Shots = Item->CastUseCount;
	AActor* Wall = Box(W, FVector(500, 0, 100), FVector(30, 300, 150));
	WoTTest::TickWorld(W, .3f);
	TestFalse(TEXT("lost sight mid-volley"), B->IsTargetVisible());
	Wall->Destroy();
	WoTTest::TickWorld(W, 8.0f);
	TestTrue(TEXT("same starting artifact fires again after reacquisition"), Item->CastUseCount > Shots);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotAimSpec, "WoT.Bots.Casting.AimErrorBoundedAndShrinking", Flags)
void FBotAimSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Center"), TEXT("Edge") }) { Names.Add(Name); Commands.Add(Name); }
}
bool FBotAimSpec::RunTest(const FString& Name)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(1000, 0, 50));
	WoTTest::TickWorld(W, .2f);
	// A valid random disk draw can be exactly zero. Exercise that boundary and
	// maximum error explicitly instead of requiring every draw to shrink strictly.
	FWoTBotBehaviourAccess::SetAimDisk(B, Name == TEXT("Center") ? FVector2D::ZeroVector : FVector2D(.6, .8));
	const auto Error = [B, E]()
	{
		const FVector Direct = (E->GetActorLocation() - B->GetPawn()->GetPawnViewLocation()).GetSafeNormal();
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Direct, B->GetControlRotation().Vector()), -1.0, 1.0)));
	};
	const double First = Error();
	TestTrue(TEXT("initial actual angular error bounded"), First <= Settings.Value->AimErrorDegrees + .01f);
	WoTTest::TickWorld(W, 2.0f);
	if (Name == TEXT("Center")) TestTrue(TEXT("zero error remains zero"), First < .001 && Error() < .001);
	else TestTrue(TEXT("nonzero actual angular error shrinks"), First > 1.0 && Error() < First);
	TestTrue(TEXT("actual angular error within shrinking limit"), Error() <= B->GetAimErrorLimit() + .01f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotHealSpec, "WoT.Bots.Casting.HealThresholdAndDefaultSelectorPolicy", Flags)
bool FBotHealSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTAngrealInventory* Heal = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvHeal"));
	WoTTest::TickWorld(W, 1.1f);
	TestEqual(TEXT("full health never heals"), Heal->CastUseCount, 0);
	Pawn->Health = 50; WoTTest::TickWorld(W, .2f);
	TestEqual(TEXT("at threshold never heals"), Heal->CastUseCount, 0);
	AWoTAngrealProjectile* Offender = W->SpawnActor<AWoTAngrealProjectile>(FindObject<UClass>(nullptr,
		TEXT("/Script/WoT.WoTDartProjectile")), FVector(100, 0, 50), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("incoming projectile"), Offender)) return false;
	Offender->SourceAngreal = Heal;
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	TestEqual(TEXT("unfiltered existing policy still selects Heal"), Caster->SelectDefensiveWeave(Offender), Heal);
	TestNull(TEXT("optional filter excludes Heal"), Caster->SelectDefensiveWeave(Offender,
		[](const AWoTAngrealInventory* Item) { return !Item->IsA<AWoTAngrealInvHeal>(); }));
	Offender->Destroy();
	Pawn->Health = 40; WoTTest::TickWorld(W, 6.2f);
	TestEqual(TEXT("below threshold real heal restores health"), Pawn->Health, 50);
	TestEqual(TEXT("one heal charge used"), Heal->CastUseCount, 1);
	WoTTest::TickWorld(W, 2.0f);
	TestEqual(TEXT("no further healing at threshold"), Heal->CastUseCount, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotSummonSpec, "WoT.Bots.Casting.SummonWithoutLiveHelper", Flags)
bool FBotSummonSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	Enemy(W, FVector(1000, 0, 50))->SetCanBeDamaged(false);
	AWoTResourceSpawnerInv* Item = CastChecked<AWoTResourceSpawnerInv>(Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvMinion")));
	WoTTest::TickWorld(W, 7.0f);
	TestEqual(TEXT("summoning artifact actually used"), Item->CastUseCount, 1);
	TestTrue(TEXT("real live helper spawned"), Item->CurrentArenaHelper.IsValid());
	WoTTest::TickWorld(W, 2.0f);
	TestEqual(TEXT("live helper prevents further summons"), Item->CastUseCount, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotGatesSpec, "WoT.Bots.Casting.UsageHardLandingAndFrozen", Flags)
bool FBotGatesSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	Enemy(W, FVector(1000, 0, 50))->SetCanBeDamaged(false);
	AWoTAngrealInventory* Item = Kit(B);
	Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::LandingFreeze);
	WoTTest::TickWorld(W, .5f);
	TestEqual(TEXT("hard landing prevents casts"), Item->CastUseCount, 0);
	Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	Caster->MinOffensiveUsageInterval = 5.0f;
	WoTTest::TickWorld(W, 1.0f);
	const int32 First = Item->CastUseCount;
	TestTrue(TEXT("real cast after landing"), First > 0);
	AActor* Wall = Box(W, FVector(500, 0, 100), FVector(30, 300, 150));
	WoTTest::TickWorld(W, .2f); Wall->Destroy();
	WoTTest::TickWorld(W, 1.0f);
	TestEqual(TEXT("reacquisition cannot bypass usage interval"), Item->CastUseCount, First);
	W->GetAuthGameMode<AWoTGameMode>()->EndGame(NAME_None);
	const uint64 Thinks = B->GetThinkCount(); const FVector Position = Pawn->GetActorLocation();
	WoTTest::TickWorld(W, 2.0f);
	TestEqual(TEXT("frozen no cast"), Item->CastUseCount, First);
	TestEqual(TEXT("frozen no think"), B->GetThinkCount(), Thinks);
	TestTrue(TEXT("frozen no movement"), Position.Equals(Pawn->GetActorLocation(), .1f));
	TestTrue(TEXT("frozen state"), B->GetBehaviourState() == EWoTBotState::Frozen);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDamageHookSpec, "WoT.Bots.Hooks.DamageNotificationLeavesHumansAndNPCsUnchanged", Flags)
bool FBotDamageHookSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTCharacter* Human = Enemy(W, FVector(0, 0, 50));
	AWoTNPCCharacter* NPC = W->SpawnActor<AWoTNPCCharacter>(FVector(800, 0, 50), FRotator::ZeroRotator);
	for (AWoTCharacter* Pawn : { Human, static_cast<AWoTCharacter*>(NPC) })
	{
		FDamageEvent Event;
		const int32 Before = Pawn->Health;
		const float Without = Pawn->TakeDamage(7, Event, nullptr, nullptr);
		TestEqual(TEXT("unbound damage still applies"), Pawn->Health, Before - 7);
		int32 Notices = 0; float Amount = 0;
		const FDelegateHandle Handle = Pawn->OnDamageApplied.AddLambda([&](AWoTCharacter* Victim, AController*, AActor*, float Applied)
		{ ++Notices; Amount = Applied; TestEqual(TEXT("notification follows health change"), Victim->Health, Before - 14); });
		const float With = Pawn->TakeDamage(7, Event, nullptr, nullptr);
		TestEqual(TEXT("observer does not change damage result"), With, Without);
		TestEqual(TEXT("one real damage notification"), Notices, 1);
		TestEqual(TEXT("amount after reduction"), Amount, 7.0f);
		Pawn->SetCanBeDamaged(false); Pawn->TakeDamage(7, Event, nullptr, nullptr);
		TestEqual(TEXT("rejected damage does not reveal"), Notices, 1);
		Pawn->OnDamageApplied.Remove(Handle);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDefenceSpec, "WoT.Bots.Casting.IncomingProjectileUsesDefenceAndExcludesHeal", Flags)
bool FBotDefenceSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn());
	AWoTCharacter* E = Enemy(W, FVector(800, 0, 50));
	Pawn->Health = 75;
	AWoTAngrealInventory* Heal = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvHeal"));
	AWoTAngrealInventory* Source = Give(E, TEXT("/Script/WoT.WoTAngrealInvDart"));
	FActorSpawnParameters Params; Params.Owner = E; Params.Instigator = E;
	AWoTAngrealProjectile* Incoming = W->SpawnActor<AWoTAngrealProjectile>(FindObject<UClass>(nullptr,
		TEXT("/Script/WoT.WoTDartProjectile")), FVector(100, 0, 60), FRotator(0, 180, 0), Params);
	if (!TestNotNull(TEXT("real incoming projectile"), Incoming)) return false;
	Incoming->SourceAngreal = Source;
	Incoming->ProjectileMovement->Velocity = FVector(-100, 0, 0);
	WoTTest::TickWorld(W, .2f);
	TestEqual(TEXT("above threshold incoming fire never spends Heal"), Heal->CastUseCount, 0);
	AWoTAngrealInventory* Shield = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvShield"));
	WoTTest::TickWorld(W, .2f);
	TestTrue(TEXT("bot detects projectile and activates held defence"), Shield->CastUseCount > 0);
	TestTrue(TEXT("defence cannot heal above threshold"), Pawn->Health <= 75);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotNPCFilterSpec, "WoT.Bots.Hooks.UnfilteredNPCSelectorUnchanged", Flags)
bool FBotNPCFilterSpec::RunTest(const FString&)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTNPCCharacter* NPC = W->SpawnActor<AWoTNPCCharacter>(FVector(0, 0, 50), FRotator::ZeroRotator);
	AWoTAngrealInventory* Heal = Give(NPC, TEXT("/Script/WoT.WoTAngrealInvHeal"));
	UWoTCasterComponent* Caster = NPC->FindComponentByClass<UWoTCasterComponent>();
	if (!Caster)
	{
		Caster = NewObject<UWoTCasterComponent>(NPC); Caster->bBuildLoadout = false;
		NPC->AddInstanceComponent(Caster); Caster->RegisterComponent();
	}
	AWoTAngrealProjectile* Incoming = W->SpawnActor<AWoTAngrealProjectile>(FindObject<UClass>(nullptr,
		TEXT("/Script/WoT.WoTDartProjectile")), FVector(100, 0, 60), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("projectile"), Incoming)) return false;
	Incoming->SourceAngreal = Heal;
	TestEqual(TEXT("NPC default selector retains existing full-health Heal policy"), Caster->SelectDefensiveWeave(Incoming), Heal);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotLeadSpec, "WoT.Bots.Casting.MovingTargetUsesLauncherLead", Flags)
bool FBotLeadSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* E = Enemy(W, FVector(1000, 0, 50)); E->SetCanBeDamaged(false);
	AWoTProjectileLauncher* Item = CastChecked<AWoTProjectileLauncher>(Kit(B));
	CastChecked<APlayerController>(E->GetController())->SetAsLocalPlayerController();
	for (int32 I = 0; I < 45; ++I)
	{
		E->AddMovementInput(FVector(0, 1, 0));
		WoTTest::TickWorld(W, 1.0f / 60.0f);
	}
	const FVector Aim = B->GetControlRotation().Vector();
	const FVector Led = Item->GetBestTrajectory().Vector();
	const FVector Direct = (E->GetActorLocation() - B->GetPawn()->GetActorLocation()).GetSafeNormal();
	TestTrue(TEXT("real starting launcher fired"), Item->CastUseCount > 0);
	TestTrue(TEXT("aim follows item's predicted trajectory"), FVector::DotProduct(Aim, Led) > .999);
	TestTrue(TEXT("target actually moves"), E->GetVelocity().Size2D() > 100);
	TestTrue(TEXT("moving target requires nonzero lead"), FVector::DotProduct(Aim, Direct) < .99999);
	return !HasAnyErrors();
}
} // namespace WoTBotTest
#endif
