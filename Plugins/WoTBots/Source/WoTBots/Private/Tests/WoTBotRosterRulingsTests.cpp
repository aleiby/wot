#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTBotBehaviourFixture.h"
#include "WoTBotRosterTestTypes.h"
#include "WoTBotRoster.h"
#include "WoTCarcass.h"
#include "AI/WoTCasterComponent.h"
#include "EngineUtils.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Engine.h"
#include "Engine/Player.h"
#include "GameFramework/GameStateBase.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "Inventory/WoTAngrealInvBalefire.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "Inventory/WoTLeech.h"
#include "WoTSoundTable.h"

namespace WoTBotTest
{
struct FWoTBotRosterAccess
{
	static bool Thinking(AWoTBotController* B) { return B->GetWorldTimerManager().IsTimerActive(B->ThinkTimer); }
	static void PauseDecisions(AWoTBotController* B)
	{
		B->GetWorldTimerManager().ClearTimer(B->ThinkTimer);
		B->SetActorTickEnabled(false);
	}
	static void BeginCast(AWoTBotController* B, AWoTAngrealInventory* Item, AWoTCharacter* Enemy)
	{
		B->SetTarget(Enemy, true);
		Item->SetVictim(Enemy);
		Item->InitChargeGroup();
		B->CastItem(Item);
	}
};

namespace
{
UWorld* RosterWorld(int32 Fill = 2, TFunction<void(UWorld*)> BeforeBots = {})
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.GameModeClass = AWoTBotRosterTestGame::StaticClass();
	Options.URL = FString::Printf(TEXT("/Engine/Maps/Entry?BotFill=%d?FragLimit=0?TimeLimit=0"), Fill);
	Options.BeforeInitializeActorsForPlay = [](UWorld* W)
	{
		Box(W, FVector(0, 0, -50), FVector(3000, 2000, 50));
		W->SpawnActor<APlayerStart>(FVector(0, 0, 50), FRotator::ZeroRotator);
		W->SpawnActor<APlayerStart>(FVector(-2000, 0, 50), FRotator::ZeroRotator);
	};
	UWorld* W = WoTTest::CreateSpecWorld(Options);
	if (BeforeBots) BeforeBots(W);
	WoTTest::TickWorld(W, 1.2f);
	int32 Index = 0;
	for (TActorIterator<AWoTBotController> It(W); It; ++It)
	{
		It->GetPlayerState<AWoTPlayerState>()->Kills = Index * 100;
		It->GetPlayerState<AWoTPlayerState>()->Team = 0;
		if (It->GetPawn()) It->GetPawn()->SetActorLocation(FVector(Index++ * -2000, 0, 50));
	}
	return W;
}
AWoTBotController* FirstBot(UWorld* W)
{
	for (TActorIterator<AWoTBotController> It(W); It; ++It) return *It;
	return nullptr;
}
AWoTBotRosterTestViewer* Viewer(UWorld* W, bool Notify = true)
{
	auto* V = W->SpawnActor<AWoTBotRosterTestViewer>();
	V->Player = NewObject<UPlayer>(W->GetGameInstance());
	V->GetPlayerState<AWoTPlayerState>()->Team = 1;
	if (Notify) W->GetAuthGameMode()->PostLogin(V);
	return V;
}
int32 CorpseCount(UWorld* W)
{
	int32 Count = 0;
	for (TActorIterator<AWoTCarcass> It(W); It; ++It) ++Count;
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDepartingActsSpec, "WoT.Bots.Rulings.DepartingKeepsActing", Flags)
bool FBotDepartingActsSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = RosterWorld(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	BuildNav(W);
	AWoTBotController* B = FirstBot(W);
	if (!TestNotNull(TEXT("bot exists"), B) || !TestNotNull(TEXT("body exists"), B->GetPawn<APawn>())) return false;
	auto* V = Viewer(W, false);
	AWoTCharacter* E = Body(W, FVector(600, 0, 50)); V->Possess(E);
	V->ViewLocation = FVector(600, 0, 90); V->ViewRotation = FRotator(0, 180, 0);
	W->GetAuthGameMode()->PostLogin(V);
	const FVector Before = B->GetPawn<APawn>()->GetActorLocation();
	const uint64 Thoughts = B->GetThinkCount();
	WoTTest::TickWorld(W, .7f);
	if (!TestTrue(TEXT("seen bot remains"), IsValid(B))) return false;
	TestTrue(TEXT("marked bot keeps thinking"), B->GetThinkCount() > Thoughts);
	TestEqual(TEXT("marked bot retains the visible target"), B->GetTarget(), E);
	TestTrue(TEXT("marked bot keeps engaging"), B->GetBehaviourState() == EWoTBotState::Engage);
	TestTrue(TEXT("marked bot moves"), FVector::Dist(Before, B->GetPawn<APawn>()->GetActorLocation()) > 5);
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn<APawn>());
	Pawn->Died(nullptr, NAME_None, Pawn->GetActorLocation());
	TestFalse(TEXT("marked death retires at once"), IsValid(B));
	WoTTest::TickWorld(W, 2.2f);
	TestEqual(TEXT("marked bot never respawns"), W->GetSubsystem<UWoTBotRoster>()->GetBotCount(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotUnseenSpec, "WoT.Bots.Rulings.UnseenDeparture", Flags)
void FBotUnseenSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Wall"), TEXT("BehindViewer"), TEXT("Hysteresis"), TEXT("SecondViewer"),
		TEXT("HeadOnly"), TEXT("CentreOnly"), TEXT("FeetOnly"), TEXT("Attachment") })
	{ Names.Add(Name); Commands.Add(Name); }
}
bool FBotUnseenSpec::RunTest(const FString& Case)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100;
	UWorld* W = RosterWorld(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = FirstBot(W);
	if (!TestNotNull(TEXT("body exists"), B ? B->GetPawn<APawn>() : nullptr)) return false;
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn<APawn>());
	const TWeakObjectPtr<AWoTCharacter> BodyBefore = Pawn;
	TArray<TWeakObjectPtr<AWoTInventory>> Held;
	for (AWoTInventory* Item : Pawn->InventoryComponent->Items) Held.Add(Item);
	auto* V = Viewer(W, false);
	const bool SampleCase = Case == TEXT("HeadOnly") || Case == TEXT("CentreOnly") || Case == TEXT("FeetOnly") || Case == TEXT("Attachment");
	if (Case == TEXT("Attachment"))
	{
		AActor* Attached = Box(W, FVector(-250, 0, 100), FVector(5, 500, 500), false);
		Attached->GetRootComponent()->SetMobility(EComponentMobility::Movable);
		Attached->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
	}
	else if (SampleCase)
	{
		const float Heights[] = {float(Pawn->GetPawnViewLocation().Z), float(Pawn->GetActorLocation().Z),
			float(Pawn->GetActorLocation().Z - Pawn->GetSimpleCollisionHalfHeight())};
		const int32 OpenSample = Case == TEXT("HeadOnly") ? 0 : Case == TEXT("CentreOnly") ? 1 : 2;
		for (int32 Index = 0; Index < 3; ++Index)
			if (Index != OpenSample) Box(W, FVector(-250, 0, (V->ViewLocation.Z + Heights[Index]) / 2), FVector(2, 100, 3), false);
	}
	if (Case == TEXT("Wall")) Box(W, FVector(-250, 0, 200), FVector(10, 500, 500), false);
	if (Case == TEXT("BehindViewer") || Case == TEXT("SecondViewer")) V->ViewRotation.Yaw = 180;
	auto* Game = W->GetAuthGameMode<AWoTBotRosterTestGame>();
	Game->PostLogin(V);
	if (SampleCase)
	{
		WoTTest::TickWorld(W, .3f);
		TestTrue(TEXT("one clear sample or ignored attachment keeps the bot seen"), IsValid(B));
		V->ViewRotation.Yaw = 180;
		WoTTest::TickWorld(W, .8f);
	}
	else if (Case == TEXT("SecondViewer"))
	{
		auto* Second = Viewer(W, false);
		Second->PlayerState->SetIsOnlyASpectator(true);
		Game->PostLogin(Second);
		WoTTest::TickWorld(W, .3f);
		TestTrue(TEXT("a second dead spectator viewer preserves the bot"), IsValid(B));
		Second->ViewRotation.Yaw = 180;
		WoTTest::TickWorld(W, .6f);
	}
	else if (Case == TEXT("Hysteresis"))
	{
		WoTTest::TickWorld(W, .26f);
		TestTrue(TEXT("visible at first tick"), IsValid(B));
		V->ViewRotation.Yaw = 180;
		WoTTest::TickWorld(W, .2f);
		TestTrue(TEXT("seen 0.2 seconds ago still counts"), IsValid(B));
		WoTTest::TickWorld(W, .08f);
		TestTrue(TEXT("next visibility tick preserves recent sight"), IsValid(B));
		WoTTest::TickWorld(W, .51f);
	}
	else WoTTest::TickWorld(W, .26f);
	TestFalse(TEXT("unseen bot retired within one eligible tick"), IsValid(B));
	TestFalse(TEXT("live body removed"), BodyBefore.IsValid());
	for (const auto& Item : Held) TestFalse(TEXT("held item destroyed, never dropped"), Item.IsValid());
	TestEqual(TEXT("no corpse created"), CorpseCount(W), 0);
	TestTrue(TEXT("ordinary leave message delivered"), V->LeaveMessages > 0);
	TestEqual(TEXT("no kill or death announcement"), Game->Messages.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotDepartureSafetyCapSpec, "WoT.Bots.Rulings.SafetyCap", Flags)
bool FBotDepartureSafetyCapSpec::RunTest(const FString&)
{
	TestEqual(TEXT("safety cap raised to sixty seconds"), GetDefault<UWoTBotSettings>()->LeaveTimeoutSeconds, 60.0f);
	UWorld* W = RosterWorld(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = FirstBot(W);
	auto* V = Viewer(W);
	const TWeakObjectPtr<AWoTBotController> Pending = B;
	// A dead viewer still sees the bot. No opponents or navigation move it away.
	WoTTest::TickWorld(W, 59.7f);
	TestTrue(TEXT("seen living bot survives until safety cap"), Pending.IsValid());
	WoTTest::TickWorld(W, .6f);
	TestFalse(TEXT("safety cap converges even while seen"), Pending.IsValid());
	TestTrue(TEXT("timeout uses ordinary leave message"), V->LeaveMessages > 0);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotIdleSpec, "WoT.Bots.Rulings.IdleWhenEmpty", Flags)
void FBotIdleSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Human"), TEXT("Spectator"), TEXT("InactiveTeam"), TEXT("LocalHost") })
	{ Names.Add(Name); Commands.Add(Name); }
}
bool FBotIdleSpec::RunTest(const FString& Case)
{
	UWorld* W = RosterWorld(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	UWoTBotRoster* R = W->GetSubsystem<UWoTBotRoster>();
	AWoTBotController* B = FirstBot(W);
	if (!TestNotNull(TEXT("idle roster still has bodies"), B ? B->GetPawn<APawn>() : nullptr)) return false;
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn<APawn>());
	TestEqual(TEXT("empty fill remains two"), R->GetBotCount(), 2);
	TestFalse(TEXT("think timer stopped with no connected human"), FWoTBotRosterAccess::Thinking(B));
	const FVector Before = Pawn->GetActorLocation();
	const uint64 Thoughts = B->GetThinkCount();
	AWoTAngrealInventory* Pickup = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvFireball"));
	Pawn->InventoryComponent->DeleteInventory(Pickup);
	Pickup->SetOwner(nullptr);
	TestFalse(TEXT("idle body rejects pickup"), Pickup->TryPickupBy(Pawn));
	WoTTest::TickWorld(W, 2.1f);
	TestEqual(TEXT("idle does not think"), B->GetThinkCount(), Thoughts);
	TestTrue(TEXT("idle stands still"), Pawn->GetActorLocation().Equals(Before, .1f));
	TestFalse(TEXT("idle does not cast"), Pawn->FindComponentByClass<UWoTCasterComponent>()->IsCasting());
	B->SilentRespawn();
	TestEqual(TEXT("idle forbids silent respawn"), B->GetPawn<APawn>(), static_cast<APawn*>(Pawn));
	if (!IsValid(Pawn)) return false;
	auto* V = Viewer(W, false);
	if (Case == TEXT("Spectator")) { V->PlayerState->SetIsOnlyASpectator(true); V->PlayerState->SetIsSpectator(true); }
	if (Case == TEXT("InactiveTeam")) { V->PlayerState->SetIsInactive(true); V->GetPlayerState<AWoTPlayerState>()->Team = 255; }
	if (Case == TEXT("LocalHost")) V->Player = NewObject<ULocalPlayer>(GEngine);
	W->GetAuthGameMode()->PostLogin(V);
	TestTrue(TEXT("connection wakes timer immediately"), FWoTBotRosterAccess::Thinking(B));
	TestNull(TEXT("wake begins without stale targets"), B->GetTarget());
	if (Case == TEXT("Spectator") || Case == TEXT("InactiveTeam"))
		TestEqual(TEXT("non-playing viewer does not consume fill slot"), R->GetDesiredBotCount(), 2);
	// Cancel the departure before testing the empty-server death path.
	V->Destroy();
	TestFalse(TEXT("last logout stops timer immediately"), FWoTBotRosterAccess::Thinking(B));
	Pawn->Died(nullptr, NAME_None, Pawn->GetActorLocation());
	WoTTest::TickWorld(W, 3.0f);
	TestNull(TEXT("dead idle bot waits to respawn"), B->GetPawn<APawn>());
	V = Viewer(W, false); V->PlayerState->SetIsOnlyASpectator(true);
	W->GetAuthGameMode()->PostLogin(V);
	WoTTest::TickWorld(W, 1.2f);
	TestNotNull(TEXT("dead bot respawns on wake"), B->GetPawn<APawn>());
	R->Freeze();
	V->Destroy(); Viewer(W);
	TestFalse(TEXT("frozen roster never wakes"), FWoTBotRosterAccess::Thinking(B));
	TestTrue(TEXT("freeze wins"), B->IsFrozen());
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotIdleCastSpec, "WoT.Bots.Rulings.IdleCancelsCasting", Flags)
void FBotIdleCastSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("Pending"), TEXT("Held") }) { Names.Add(Name); Commands.Add(Name); }
}
bool FBotIdleCastSpec::RunTest(const FString& Case)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = RosterWorld(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* V = Viewer(W, false); V->PlayerState->SetIsOnlyASpectator(true); V->PlayerState->SetIsInactive(true);
	W->GetAuthGameMode()->PostLogin(V);
	AWoTBotController* B = FirstBot(W);
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn<APawn>());
	AWoTCharacter* E = Body(W, FVector(600, 0, 50));
	AWoTAngrealInventory* Item = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvBalefire"));
	FWoTBotRosterAccess::BeginCast(B, Item, E);
	UWoTCasterComponent* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	if (Case == TEXT("Pending")) Caster->Release();
	const FBoolProperty* Casting = FindFProperty<FBoolProperty>(Item->GetClass(), TEXT("bCasting"));
	if (!TestNotNull(TEXT("charged cast flag"), Casting)) return false;
	TestTrue(TEXT("activation is pending before idle"), Casting->GetPropertyValue_InContainer(Item));
	auto* PoolGoal = Pool(W, FVector(900, 900, 30), TEXT("/Script/WoT.WoTAngrealInvFireball"));
	UWoTBotRoster* R = W->GetSubsystem<UWoTBotRoster>(); R->ClaimPool(B, PoolGoal);
	const int32 Uses = Item->CastUseCount, Charges = Item->CurCharges;
	V->Destroy();
	TestFalse(TEXT("idle releases pool claims"), R->IsPoolClaimed(PoolGoal, nullptr));
	TestFalse(TEXT("idle cancels pending activation"), Casting->GetPropertyValue_InContainer(Item));
	TestFalse(TEXT("idle releases caster"), Caster->IsCasting());
	WoTTest::TickWorld(W, 1.5f);
	TestEqual(TEXT("no delayed shot while idle"), Item->CastUseCount, Uses);
	TestEqual(TEXT("no delayed charge spent while idle"), Item->CurCharges, Charges);
	V = Viewer(W, false); V->PlayerState->SetIsOnlyASpectator(true); V->PlayerState->SetIsInactive(true);
	W->GetAuthGameMode()->PostLogin(V);
	TestNull(TEXT("wake clears stale target"), B->GetTarget());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotIdleInitialPickupSpec, "WoT.Bots.Rulings.IdleInitialSpawnCannotCollect", Flags)
bool FBotIdleInitialPickupSpec::RunTest(const FString&)
{
	TArray<TWeakObjectPtr<AWoTAngrealInventory>> Pickups;
	UWorld* W = RosterWorld(2, [&](UWorld* World)
	{
		UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTAngrealInvFireball"));
		for (const FVector& Location : {FVector(0, 0, 50), FVector(-2000, 0, 50)})
			Pickups.Add(World->SpawnActor<AWoTAngrealInventory>(Class, Location, FRotator::ZeroRotator));
	});
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	TestEqual(TEXT("idle roster still spawns its two bodies"), W->GetSubsystem<UWoTBotRoster>()->GetBotCount(), 2);
	for (const auto& Pickup : Pickups)
		TestTrue(TEXT("initial body overlap leaves world pickup untouched"), Pickup.IsValid() && !Pickup->GetOwner());
	for (TActorIterator<AWoTBotController> It(W); It; ++It)
	{
		AWoTCharacter* Pawn = Cast<AWoTCharacter>(It->GetPawn());
		if (TestNotNull(TEXT("idle initial body exists"), Pawn))
			TestEqual(TEXT("idle initial body still receives the normal kit"), Pawn->InventoryComponent->Items.Num(), 2);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBotQuietChargedDepartureSpec, "WoT.Bots.Rulings.QuietDepartureCancelsChargedCast", Flags)
bool FBotQuietChargedDepartureSpec::RunTest(const FString&)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = RosterWorld(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* V = Viewer(W);
	AWoTBotController* B = FirstBot(W);
	const TWeakObjectPtr<AWoTBotController> Pending = B;
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn<APawn>());
	AWoTCharacter* E = Body(W, FVector(600, 0, 50));
	AWoTAngrealInventory* Item = Give(Pawn, TEXT("/Script/WoT.WoTAngrealInvBalefire"));
	FWoTBotRosterAccess::BeginCast(B, Item, E);
	FWoTBotRosterAccess::PauseDecisions(B);
	WoTTest::TickWorld(W, 1.1f);
	TestTrue(TEXT("departure still has a primed held cast"), Pawn->FindComponentByClass<UWoTCasterComponent>()->IsCasting());
	V->ViewRotation.Yaw = 180;
	WoTTest::TickWorld(W, 1.0f);
	TestFalse(TEXT("unseen primed bot retires"), Pending.IsValid());
	TestEqual(TEXT("quiet retirement never fires the primed shot"), E->Health, 100);
	TestEqual(TEXT("quiet retirement has no kill announcement"), W->GetAuthGameMode<AWoTBotRosterTestGame>()->Messages.Num(), 0);
	return !HasAnyErrors();
}

namespace
{
int32 FailureSounds(UWorld* W, FName SubPath)
{
	int32 Count = 0;
	UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/WoT.WoTSoundBurst"));
	for (TActorIterator<AActor> It(W, Class); It; ++It)
		if (static_cast<AWoTSoundBurst*>(*It)->SoundSubPath == SubPath) ++Count;
	return Count;
}
int32 Shots(UWorld* W)
{
	int32 Count = 0;
	for (const TCHAR* Path : { TEXT("/Script/WoT.WoTProjectile"), TEXT("/Script/WoT.WoTBeamEffect") })
		for (TActorIterator<AActor> It(W, FindObject<UClass>(nullptr, Path)); It; ++It) ++Count;
	return Count;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotQuietCancellationSpec, "WoT.Bots.Rulings.QuietCancellation", Flags)
void FBotQuietCancellationSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Action : { TEXT("Retire"), TEXT("Respawn"), TEXT("Idle"), TEXT("Freeze") })
		for (const TCHAR* Cast : { TEXT("Primed"), TEXT("Pending"), TEXT("Launcher") })
		{
			const FString Name = FString(Action) + TEXT(".") + Cast;
			Names.Add(Name); Commands.Add(Name);
		}
}
bool FBotQuietCancellationSpec::RunTest(const FString& Case)
{
	FSettings Settings; Settings.Value->ReactionSeconds = 100; Settings.Value->AimErrorDegrees = 0;
	UWorld* W = RosterWorld(1); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	auto* V = Viewer(W, false);
	V->PlayerState->SetIsOnlyASpectator(true);
	W->GetAuthGameMode()->PostLogin(V);
	AWoTBotController* B = FirstBot(W);
	if (!TestNotNull(TEXT("living bot exists"), B ? B->GetPawn<APawn>() : nullptr)) return false;
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn<APawn>());
	AWoTCharacter* E = Body(W, FVector(600, 0, 50));
	const bool Launcher = Case.EndsWith(TEXT("Launcher"));
	const bool Pending = Case.EndsWith(TEXT("Pending"));
	const TCHAR* ItemClass = Launcher ? TEXT("/Script/WoT.WoTAngrealInvFireball") : TEXT("/Script/WoT.WoTAngrealInvBalefire");
	AWoTAngrealInventory* Item = Give(Pawn, ItemClass);
	const FName FailPath(TEXT("Tests/QuietCancelFailure"));
	Item->FailSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Tests/QuietCancelFailure.QuietCancelFailure"));
	if (Launcher) static_cast<AWoTProjectileLauncher*>(Item)->NextCastTime = W->GetTimeSeconds() + 20;
	FWoTBotRosterAccess::BeginCast(B, Item, E);
	FWoTBotRosterAccess::PauseDecisions(B);
	auto* Caster = Pawn->FindComponentByClass<UWoTCasterComponent>();
	TestTrue(TEXT("cast is held before cancellation"), Caster->IsCasting());
	if (Pending) Caster->Release();
	else if (!Launcher) WoTTest::TickWorld(W, 1.1f);
	if (Launcher) TestTrue(TEXT("launcher is mid-cooldown"), static_cast<AWoTProjectileLauncher*>(Item)->NextCastTime > W->GetTimeSeconds());
	else TestTrue(TEXT("release cast is armed"), static_cast<AWoTAngrealInvBalefire*>(Item)->bCasting);
	const int32 Charges = Item->CurCharges;
	const int32 SoundsBefore = FailureSounds(W, FailPath);
	const int32 MessagesBefore = V->ClientMessages.Num();
	TestEqual(TEXT("setup has no projectile or beam"), Shots(W), 0);
	if (Case.StartsWith(TEXT("Retire")))
	{
		V->PlayerState->SetIsOnlyASpectator(false);
		V->ViewRotation.Yaw = 180;
		W->GetAuthGameMode()->PostLogin(V);
		WoTTest::TickWorld(W, .3f);
		TestFalse(TEXT("unseen bot retires"), IsValid(B));
	}
	else if (Case.StartsWith(TEXT("Respawn"))) B->SilentRespawn();
	else if (Case.StartsWith(TEXT("Idle"))) B->SetIdle(true);
	else W->GetSubsystem<UWoTBotRoster>()->Freeze();
	TestEqual(TEXT("cancellation emits no replicated failure sound"), FailureSounds(W, FailPath), SoundsBefore);
	TestEqual(TEXT("cancellation emits no failure message"), V->ClientMessages.Num(), MessagesBefore);
	TestEqual(TEXT("cancellation has no public failure or death message"), W->GetAuthGameMode<AWoTBotRosterTestGame>()->Messages.Num(), 0);
	TestEqual(TEXT("release emits no projectile or beam"), Shots(W), 0);
	if (IsValid(Item))
	{
		TestFalse(TEXT("caster released"), Caster->IsCasting());
		if (Launcher) TestTrue(TEXT("cooldown cleared"), static_cast<AWoTProjectileLauncher*>(Item)->NextCastTime <= W->GetTimeSeconds());
		else
		{
			TestFalse(TEXT("charge cancelled"), static_cast<AWoTAngrealInvBalefire*>(Item)->bCasting);
			TestNull(TEXT("gather effect released"), static_cast<AWoTAngrealInvBalefire*>(Item)->GatherEffect.Get());
		}
		WoTTest::TickWorld(W, 1.2f);
		TestEqual(TEXT("pending shot never spends a charge"), Item->CurCharges, Charges);
		TestEqual(TEXT("pending timer emits no projectile or beam"), Shots(W), 0);
	}
	TestEqual(TEXT("cancelled cast never damages its target"), E->Health, 100);

	// The same item failure still performs cleanup and sends owner feedback.
	V->PlayerState->SetIsOnlyASpectator(false);
	AWoTCharacter* Human = Body(W, FVector(0, 700, 50)); V->Possess(Human);
	TestEqual(TEXT("human owns the control body"), Human->GetController(), static_cast<AController*>(V));
	AWoTAngrealInventory* HumanItem = Give(Human, ItemClass);
	HumanItem->FailSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Tests/QuietCancelFailure.QuietCancelFailure"));
	if (Launcher) static_cast<AWoTProjectileLauncher*>(HumanItem)->NextCastTime = W->GetTimeSeconds() + 20;
	HumanItem->CastArtifact();
	if (Pending) HumanItem->UnCastArtifact();
	else if (!Launcher) WoTTest::TickWorld(W, 1.1f);
	const int32 HumanCharges = HumanItem->CurCharges;
	const int32 HumanSounds = FailureSounds(W, FailPath);
	const int32 HumanMessages = V->ClientMessages.Num();
	HumanItem->Failed();
	TestEqual(TEXT("human failure plays one replicated sound"), FailureSounds(W, FailPath), HumanSounds + 1);
	TestEqual(TEXT("human failure sends one owner message"), V->ClientMessages.Num(), HumanMessages + 1);
	if (V->ClientMessages.Num() > HumanMessages)
		TestEqual(TEXT("human failure retains its message"), V->ClientMessages.Last(), HumanItem->Title + TEXT(" ") + HumanItem->FailMessage + TEXT("."));
	if (Launcher) TestEqual(TEXT("human failure clears cooldown"), static_cast<AWoTProjectileLauncher*>(HumanItem)->NextCastTime, float(W->GetTimeSeconds()));
	else
	{
		TestFalse(TEXT("human failure cancels charging"), static_cast<AWoTAngrealInvBalefire*>(HumanItem)->bCasting);
		TestNull(TEXT("human failure removes gather effect"), static_cast<AWoTAngrealInvBalefire*>(HumanItem)->GatherEffect.Get());
	}
	HumanItem->UnCastArtifact();
	WoTTest::TickWorld(W, 1.2f);
	TestEqual(TEXT("human failed release spends no charge"), HumanItem->CurCharges, HumanCharges);
	TestEqual(TEXT("human failed release emits no shot"), Shots(W), 0);
	return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FBotWakeAfterHoldSpec, "WoT.Bots.Rulings.WakeAfterHold", Flags)
void FBotWakeAfterHoldSpec::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	Names.Add(TEXT("Lift")); Commands.Add(TEXT("/Script/WoT.WoTWhirlwindLeech"));
	Names.Add(TEXT("Ice")); Commands.Add(TEXT("/Script/WoT.WoTIceLeech"));
}
bool FBotWakeAfterHoldSpec::RunTest(const FString& EffectPath)
{
	UWorld* W = World(); ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(W); };
	AWoTBotController* B = Bot(W, FVector(0, 0, 50));
	AWoTCharacter* Pawn = CastChecked<AWoTCharacter>(B->GetPawn<APawn>());
	auto* Movement = Pawn->GetCharacterMovement();
	UClass* EffectClass = FindObject<UClass>(nullptr, *EffectPath);
	if (!TestNotNull(TEXT("hold class exists"), EffectClass)) return false;
	for (bool EndWhileIdle : { false, true })
	{
		Movement->SetMovementMode(MOVE_Walking);
		auto* Effect = static_cast<UWoTLeech*>(NewObject<UActorComponent>(Pawn, EffectClass));
		Effect->AttachTo(Pawn);
		TestTrue(TEXT("real hold attached"), Effect->IsRegistered());
		TestEqual(TEXT("hold disables movement"), int32(Movement->MovementMode), int32(MOVE_None));
		B->SetIdle(true);
		if (EndWhileIdle) Effect->Unattach();
		B->SetIdle(false);
		if (!EndWhileIdle)
		{
			TestEqual(TEXT("wake preserves an active hold"), int32(Movement->MovementMode), int32(MOVE_None));
			Effect->Unattach();
			continue;
		}
		TestTrue(TEXT("wake restores movement after expired hold"), Movement->MovementMode != MOVE_None);
		FWoTBotRosterAccess::PauseDecisions(B);
		const FVector Before = Pawn->GetActorLocation();
		for (int32 Step = 0; Step < 15; ++Step)
		{
			Pawn->AddMovementInput(FVector::ForwardVector);
			WoTTest::TickWorld(W, .02f);
		}
		TestTrue(TEXT("woken body moves again"), Pawn->GetActorLocation().X > Before.X + 5);
	}
	return !HasAnyErrors();
}
}
#endif
