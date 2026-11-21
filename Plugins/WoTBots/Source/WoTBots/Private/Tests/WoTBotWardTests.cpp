#include "Misc/AutomationTest.h"
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "WoTBotMapFixture.h"
#include "Projectiles/WoTExpWardProjectile.h"
#include "Inventory/WoTProjectileLauncher.h"
#include "AI/WoTCasterComponent.h"
#include "Navigation/PathFollowingComponent.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "WoTBotWardObserver.h"
#include "WoTBotRoster.h"
#include "UObject/StrongObjectPtr.h"
#include "AI/WoTPathNodeIterator.h"
#include "AI/WoTAIUtil.h"

namespace WoTBotTest
{
struct FWoTBotWardAccess
{
 static void Quiet(AWoTBotController* B) { B->GetWorldTimerManager().ClearAllTimersForObject(B); B->SetActorTickEnabled(false); }
 static void Sense(AWoTBotController* B) { B->PerceiveWards(); }
 static bool Known(AWoTBotController* B, AActor* W) { const auto* K=B->WardBeliefs.Find(W); return K && K->bKnown; }
 static void Clear(AWoTBotController* B) { B->WardBeliefs.Reset(); }
 static void Seed(AWoTBotController* B, int32 S) { B->WardRandom.Initialize(S); }
 static bool Route(AWoTBotController* B, FVector P) { return B->NavigateTo(P); }
 static bool Guard(AWoTBotController* B) { return B->GuardWardRoute(); }
 static bool Visible(AWoTBotController* B, AActor* W) { return B->CanSeeStimulus(W); }
 static bool AimingAt(AWoTBotController* B, AActor* W) { return B->AimingWard == W; }
 static void Cast(AWoTBotController* B) { B->CastArtifacts(); }
 static void EnemyTarget(AWoTBotController* B, AWoTCharacter* E) { B->SetTarget(E,true); B->VisibleSince-=1; }
 // Normal controller thinking and ticking, as a roster-spawned bot runs.
 static void Wake(AWoTBotController* B) { B->SetActorTickEnabled(true); B->StartThinking(); }
 static bool Waiting(AWoTBotController* B) { return B->bWardWaiting; }
 static void Forget(AWoTBotController* B, AActor* W) { B->WardBeliefs.Remove(W); }
 static AActor* Blocking(AWoTBotController* B) { return B->BlockingWard.Get(); }
};
}
namespace BotWardSpec
{
using namespace WoTBotTest;
constexpr float Step=1.f/60;
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
		UNavigationSystemConfig* Config = NewObject<UNavigationSystemConfig>(W->GetWorldSettings());
		Config->NavigationSystemClass = FSoftClassPath(UWoTBotNavigationSystem::StaticClass());
		W->GetWorldSettings()->SetNavigationSystemConfigOverride(Config);
		FNavigationSystem::AddNavigationSystemToWorld(*W, FNavigationSystemRunMode::GameMode, Config);
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


struct FWardFixture : FFixture
{
 AWoTBotController* B = nullptr;
 AWoTCharacter* Placer = nullptr;
 AWoTExpWardProjectile* Ward = nullptr;
 FVector End;
 bool Setup(FAutomationTestBase& Test, uint8 Team=1, bool Own=false)
 {
  if (!Load(Test)) return false;
  auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
  if (!Test.TestNotNull(TEXT("real map navigation"), Nav)) return false;
  Nav->Build(); for (ANavigationData* Data : Nav->NavDataSet) if (Data) Data->EnsureBuildCompletion();
  B=Bot(W,Start); FWoTBotWardAccess::Quiet(B);
  FWoTBotWardAccess::Seed(B,9127);
  Placer=Own ? CastChecked<AWoTCharacter>(B->GetPawn()) : Enemy(W,Start+Direction*350,Team);
  Placer->GetCharacterMovement()->DisableMovement();
  if (Own) Placer->SetActorLocation(Start+Direction*350);
  auto* Item=Give(Placer,TEXT("/Script/WoT.WoTAngrealInvExpWard"));
  Placer->InventoryComponent->SelectItem(Item);
  Placer->GetController()->SetControlRotation(FRotator(-90,0,0));
  Placer->AuthorityUseAngreal(FRotator(-90,0,0)); Placer->AuthorityCeaseUsingAngreal();
  UClass* WardClass=FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTExpWardProjectile"));
  for (TActorIterator<AActor> It(W,WardClass); It; ++It) Ward=static_cast<AWoTExpWardProjectile*>(*It);
  if (!Test.TestNotNull(TEXT("ward placed through real artifact cast"),Ward)) return false;
  if (!Test.TestTrue(TEXT("ward attached to real map surface"),Ward->bCorrectlyPlaced)) return false;
  Placer->SetActorLocation(Own ? Start : Start-Direction*300);
  if(Own) Placer->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
  Tick(1.1f);
  Test.TestTrue(TEXT("real activation timer armed ward"),Ward->bArmed);
  B->SetControlRotation((Ward->GetActorLocation()-B->GetPawn()->GetPawnViewLocation()).Rotation());
  End=Ward->GetActorLocation()+Direction*350;
  return true;
 }
 void Look() { B->SetControlRotation((Ward->GetActorLocation()-B->GetPawn()->GetPawnViewLocation()).Rotation()); }
 bool Notice()
 {
  for(int I=0;I<50 && !FWoTBotWardAccess::Known(B,Ward);++I) { Look(); Ward->ScaleGlow=1; FWoTBotWardAccess::Sense(B); Tick(.1f); }
  return FWoTBotWardAccess::Known(B,Ward);
 }
 // Another enemy casts a second real ward straight down at At, then steps away
 // before it arms (it would otherwise touch it on its next move).
 AWoTExpWardProjectile* PlaceAnother(FVector At)
 {
  UClass* WardClass=FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTExpWardProjectile"));
  TSet<AActor*> Before; for(TActorIterator<AActor> It(W,WardClass);It;++It) Before.Add(*It);
  auto* Caster=Enemy(W,At,1); Caster->GetCharacterMovement()->DisableMovement();
  auto* Item=Give(Caster,TEXT("/Script/WoT.WoTAngrealInvExpWard"));
  Caster->InventoryComponent->SelectItem(Item);
  Caster->GetController()->SetControlRotation(FRotator(-90,0,0));
  Caster->AuthorityUseAngreal(FRotator(-90,0,0)); Caster->AuthorityCeaseUsingAngreal();
  Caster->SetActorLocation(Start-Direction*300+FVector(0,0,200));
  for(TActorIterator<AActor> It(W,WardClass);It;++It) if(!Before.Contains(*It)) return static_cast<AWoTExpWardProjectile*>(*It);
  return nullptr;
 }
};

// A flat floor split by walls into two halves joined by one corridor 200 wide:
// narrower than a ward's avoidance zone (blast 120 + body 17 + 8 on each side),
// so a ward at its middle blocks every route between the halves. A teammate
// casts a real ward there; the bot saw it placed. The only pool is beyond it.
struct FCorridor
{
 UWorld* W = nullptr;
 AWoTBotController* B = nullptr;
 AWoTCharacter* Placer = nullptr;
 AWoTExpWardProjectile* Ward = nullptr;
 FSettings Settings;
 ~FCorridor() { WoTTest::DestroySpecWorld(W); }
 bool Setup(FAutomationTestBase& Test)
 {
  SeedDecisions(4721);
  W=World();
  Box(W,FVector(0,-1100,150),FVector(200,1000,150));
  Box(W,FVector(0,1100,150),FVector(200,1000,150));
  if(!Test.TestNotNull(TEXT("corridor navigation"),BuildNav(W))) return false;
  Pool(W,FVector(800,0,30),TEXT("/Script/WoT.WoTAngrealInvFireball"));
  Placer=Enemy(W,FVector(0,0,50),0); Placer->GetCharacterMovement()->DisableMovement();
  auto* Item=Give(Placer,TEXT("/Script/WoT.WoTAngrealInvExpWard"));
  Placer->InventoryComponent->SelectItem(Item);
  Placer->GetController()->SetControlRotation(FRotator(-90,0,0));
  Placer->AuthorityUseAngreal(FRotator(-90,0,0)); Placer->AuthorityCeaseUsingAngreal();
  UClass* WardClass=FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTExpWardProjectile"));
  for(TActorIterator<AActor> It(W,WardClass);It;++It) Ward=static_cast<AWoTExpWardProjectile*>(*It);
  if(!Test.TestNotNull(TEXT("ward placed in the corridor by a real cast"),Ward)) return false;
  if(!Test.TestTrue(TEXT("ward attached to the corridor floor"),Ward->bCorrectlyPlaced)) return false;
  Placer->SetActorLocation(FVector(-1500,-1500,50));
  WoTTest::TickWorld(W,1.1f);
  if(!Test.TestTrue(TEXT("real activation timer armed ward"),Ward->bArmed)) return false;
  B=Bot(W,FVector(-450,0,50)); FWoTBotWardAccess::Seed(B,9127);
  return true;
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardAvoidSpec,"WoT.Bots.Wards.SeenRoute",WoTBotTest::Flags)
bool FWardAvoidSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this)) return false;
 TestTrue(TEXT("complete real navigation route exists"),FWoTBotWardAccess::Route(F.B,F.End));
 TestEqual(TEXT("unaware bot starts following real path"),F.B->GetMoveStatus(),EPathFollowingStatus::Moving);
 F.B->StopMovement();
 TestTrue(TEXT("enemy ward noticed through sight"),F.Notice());
 FWoTBotWardAccess::Route(F.B,F.End); FWoTBotWardAccess::Guard(F.B);
 float Closest=1e9f;
 for(int I=0;I<180;++I) { FWoTBotWardAccess::Guard(F.B); F.Tick(Step); Closest=FMath::Min(Closest,float(FVector::Dist2D(F.B->GetPawn()->GetActorLocation(),F.Ward->GetActorLocation()))); }
 TestTrue(TEXT("real route stays outside blast plus body radius"),Closest>=137);
 TestTrue(TEXT("ward survives bot approach"),IsValid(F.Ward)); return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardUnseenSpec,"WoT.Bots.Wards.Unseen",WoTBotTest::Flags)
bool FWardUnseenSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this)) return false;
 F.B->SetControlRotation((-F.Direction).Rotation());
 for(int I=0;I<20;++I) { FWoTBotWardAccess::Sense(F.B); F.Tick(.1f); }
 TestFalse(TEXT("behind bot supplies no knowledge"),FWoTBotWardAccess::Known(F.B,F.Ward));
 auto* Disguise=Give(F.Placer,TEXT("/Script/WoT.WoTAngrealInvDisguise"));
 F.Placer->InventoryComponent->SelectItem(Disguise); F.Placer->AuthorityUseAngreal(FRotator::ZeroRotator);
 TestEqual(TEXT("real enemy disguise presents the bot's team"),WoTAIUtil::ApparentTeamOf(F.Placer),uint8(0));
 FWoTBotWardAccess::Sense(F.B);
 TestFalse(TEXT("disguise cannot disclose unseen enemy placements"),FWoTBotWardAccess::Known(F.B,F.Ward));
 F.Placer->AuthorityCeaseUsingAngreal();
 F.Look(); F.Settings.Value->SightRange=100;
 for(int I=0;I<20;++I) { FWoTBotWardAccess::Sense(F.B); F.Tick(.1f); }
 TestFalse(TEXT("out of sight range supplies no knowledge"),FWoTBotWardAccess::Known(F.B,F.Ward));
 F.Settings.Value->SightRange=4000;
 const FVector Saved=F.B->GetPawn()->GetActorLocation();
 bool Hidden=false;
 for(TActorIterator<APlayerStart> It(F.W);It;++It) {
  F.B->GetPawn()->SetActorLocation(It->GetActorLocation()+FVector(0,0,40)); F.Look();
  if(WoTVisBlockingTraceHits(F.W,F.B->GetPawn()->GetPawnViewLocation(),F.Ward->GetActorLocation(),F.B->GetPawn())) { Hidden=true; break; }
 }
 TestTrue(TEXT("map wall supplies an occluded viewpoint"),Hidden);
 for(int I=0;I<20;++I) { FWoTBotWardAccess::Sense(F.B); F.Tick(.1f); }
 TestFalse(TEXT("occlusion supplies no knowledge"),FWoTBotWardAccess::Known(F.B,F.Ward));
 F.B->GetPawn()->SetActorLocation(Saved); TestTrue(TEXT("positive sight control notices the same ward"),F.Notice()); return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardChanceSpec,"WoT.Bots.Wards.DistanceAndGlow",WoTBotTest::Flags)
bool FWardChanceSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this)) return false;
 F.Settings.Value->ReactionSeconds=0;
 int Counts[4]={};
 const FVector Close=F.B->GetPawn()->GetActorLocation(); FVector Far=Close;
 auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(F.W);
 const auto* Data=Nav->GetNavDataForProps(F.B->GetPawn()->GetNavAgentPropertiesRef());
 bool FarVisible=false;
 for(int Angle=0;Angle<360;Angle+=15) {
  FNavLocation P;
  if(!Nav->ProjectPointToNavigation(F.Ward->GetActorLocation()+FRotator(0,Angle,0).Vector()*800,P,FVector(80,80,180),Data)) continue;
  F.B->GetPawn()->SetActorLocation(P.Location+FVector(0,0,46)); F.Look();
  if(FWoTBotWardAccess::Visible(F.B,F.Ward)) { Far=F.B->GetPawn()->GetActorLocation(); FarVisible=true; break; }
 }
 if(!TestTrue(TEXT("distant real-map projection has clear sight"),FarVisible)) return false;
 for(int K=0;K<4;++K) {
  F.B->GetPawn()->SetActorLocation(K>=2?Far:Close); F.Look();
  TestTrue(TEXT("both distances have unobstructed sight"),FWoTBotWardAccess::Visible(F.B,F.Ward));
  for(int I=0;I<400;++I) { FWoTBotWardAccess::Clear(F.B); F.Ward->ScaleGlow=(K%2)? .05f:1.f; FWoTBotWardAccess::Sense(F.B); Counts[K]+=FWoTBotWardAccess::Known(F.B,F.Ward); }
 }
 AddInfo(FString::Printf(TEXT("400 trials each: close bright=%d dim=%d; far bright=%d dim=%d"),Counts[0],Counts[1],Counts[2],Counts[3]));
 TestTrue(TEXT("distance changes notices at fixed brightness"),Counts[0]>Counts[2]+40);
 TestTrue(TEXT("glow changes notices at fixed distance"),Counts[0]>Counts[1]+40);
 TestTrue(TEXT("close bright beats far dim"),Counts[0]>Counts[3]+100); return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardMemorySpec,"WoT.Bots.Wards.MemoryAndReaction",WoTBotTest::Flags)
bool FWardMemorySpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this)) return false;
 FWoTBotWardAccess::Sense(F.B); TestFalse(TEXT("first glimpse must wait reaction time"),FWoTBotWardAccess::Known(F.B,F.Ward));
 TestTrue(TEXT("continuous sight eventually notices"),F.Notice());
 F.B->SetControlRotation((-F.Direction).Rotation()); F.Tick(13); FWoTBotWardAccess::Sense(F.B);
 TestFalse(TEXT("unseen memory expires"),FWoTBotWardAccess::Known(F.B,F.Ward)); return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardFriendlySpec,"WoT.Bots.Wards.OwnAndTeam",WoTBotTest::Flags)
bool FWardFriendlySpec::RunTest(const FString&)
{
 using namespace BotWardSpec;
 for(int Own=0;Own<2;++Own) { FWardFixture F; if(!F.Setup(*this,0,Own!=0)) return false;
 F.B->SetControlRotation((-F.Direction).Rotation()); FWoTBotWardAccess::Sense(F.B);
 TestTrue(TEXT("own and team placement known behind view"),FWoTBotWardAccess::Known(F.B,F.Ward));
 FWoTBotWardAccess::Route(F.B,F.End); FWoTBotWardAccess::Guard(F.B);
 TestTrue(TEXT("friendly ward stops unsafe route"),F.B->GetMoveStatus()!=EPathFollowingStatus::Moving);
 } return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardShootSpec,"WoT.Bots.Wards.ShootBlockingAndEnemy",WoTBotTest::Flags)
bool FWardShootSpec::RunTest(const FString&)
{
 using namespace BotWardSpec;
 for(int Opportunity=0;Opportunity<2;++Opportunity) { FWardFixture F; if(!F.Setup(*this)) return false;
 TestTrue(TEXT("ward first noticed normally"),F.Notice());
 F.Settings.Value->AimErrorDegrees=4;
 Give(CastChecked<AWoTCharacter>(F.B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvDart"));
 if(Opportunity) { F.Placer->SetActorLocation(F.Ward->GetActorLocation()+F.Direction*80+FVector(0,0,46)); FWoTBotWardAccess::EnemyTarget(F.B,F.Placer); }
 else { FWoTBotWardAccess::Route(F.B,F.End); FWoTBotWardAccess::Guard(F.B); }
 TestTrue(TEXT("shooter outside blast plus body"),FVector::Dist2D(F.B->GetPawn()->GetActorLocation(),F.Ward->GetActorLocation())>137);
 TStrongObjectPtr<UWoTBotWardObserver> Observer(NewObject<UWoTBotWardObserver>()); Observer->Watch(F.Ward);
 TestEqual(TEXT("observer counts successful real placement"),Observer->Placed,1);
 bool Deliberate=false;
 for(int I=0;I<180 && IsValid(F.Ward);++I) { F.Look(); FWoTBotWardAccess::Sense(F.B); FWoTBotWardAccess::Cast(F.B); Deliberate |= FWoTBotWardAccess::AimingAt(F.B,F.Ward); F.Tick(.1f); }
 TestTrue(TEXT("bot deliberately aims at ward through cast code"),Deliberate);
 TestEqual(TEXT("damage observer counts actual bot shot once"),Observer->ShotByBots,1);
 TestFalse(TEXT("real damaging cast detonates the ward"),IsValid(F.Ward));
 } return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardCounterSpec,"WoT.Bots.Soak.WardCounters",WoTBotTest::Flags)
bool FWardCounterSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this)) return false;
 TStrongObjectPtr<UWoTBotWardObserver> Observer(NewObject<UWoTBotWardObserver>());
 Observer->Watch(F.Ward); Observer->Watch(F.B->GetPawn());
 auto* Victim=CastChecked<AWoTCharacter>(F.B->GetPawn()); Victim->Health=1;
 Victim->SetActorLocation(F.Ward->GetActorLocation()+FVector(0,0,46)); F.Tick(.1f);
 TestEqual(TEXT("real ward placement counted"),Observer->Placed,1);
 TestFalse(TEXT("real touch detonates ward"),IsValid(F.Ward));
 TestFalse(TEXT("ward blast kills the bot"),Victim->IsAlive());
 TestEqual(TEXT("lethal ward damage counted once"),Observer->BotDeaths,1);
 TestEqual(TEXT("touch does not count as bot shooting"),Observer->ShotByBots,0);
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardDistantExitSpec,"WoT.Bots.Wards.DistantExit",WoTBotTest::Flags)
bool FWardDistantExitSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this,0)) return false;
 auto* Roster=F.W->GetSubsystem<UWoTBotRoster>(); Roster->BuildLinks();
 auto* Nav=FNavigationSystem::GetCurrent<UNavigationSystemV1>(F.W);
 for(ANavigationData* Data:Nav->NavDataSet) if(Data) Data->EnsureBuildCompletion();
 if(!TestTrue(TEXT("real map has distant teleporter routes"),!Roster->GetDistantTeleports().IsEmpty())) return false;
 auto Via=Roster->GetDistantTeleports()[0];
 const auto* Data=Nav->GetNavDataForProps(F.B->GetPawn()->GetNavAgentPropertiesRef());
 bool Approach=false;
 for(int Angle=0;Angle<360;Angle+=30) {
  FNavLocation P;
  if(!Nav->ProjectPointToNavigation(Via.Entry+FRotator(0,Angle,0).Vector()*(Via.TouchRadius+200),P,FVector(50,50,150),Data)) continue;
  F.B->GetPawn()->SetActorLocation(P.Location+FVector(0,0,46));
  if(FWoTBotWardAccess::Route(F.B,Via.Entry) && F.B->GetMoveStatus()==EPathFollowingStatus::Moving) { Approach=true; break; }
 }
 F.B->StopMovement();
 if(!TestTrue(TEXT("real route can approach the entrance"),Approach)) return false;
 F.Ward->Destroy();
 // Arrive through the real pad so its ordinary arrival guard keeps the
 // caster at the exit while placing the ward.
 F.Placer->SetActorLocation(Via.Pad);
 const FVector Arrival=F.Placer->GetActorLocation();
 if(const auto* Actual=Roster->GetDistantTeleports().FindByPredicate([&](const auto& T) { return T.Teleporter==Via.Teleporter && FVector::Dist2D(T.Exit,Arrival)<120; })) Via=*Actual;
 if(!TestTrue(TEXT("caster rides the real distant teleporter"),FVector::Dist2D(Arrival,Via.Exit)<120)) return false;
 auto* Item=Give(F.Placer,TEXT("/Script/WoT.WoTAngrealInvExpWard"));
 F.Placer->InventoryComponent->SelectItem(Item); F.Placer->GetController()->SetControlRotation(FRotator(-90,0,0));
 F.Placer->AuthorityUseAngreal(FRotator(-90,0,0)); F.Placer->AuthorityCeaseUsingAngreal();
 F.Ward=nullptr;
 UClass* WardClass=FindObject<UClass>(nullptr,TEXT("/Script/WoT.WoTExpWardProjectile"));
 for(TActorIterator<AActor> It(F.W,WardClass);It;++It) F.Ward=static_cast<AWoTExpWardProjectile*>(*It);
 if(!TestNotNull(TEXT("real cast places ward near mapped exit"),F.Ward)) return false;
 AddInfo(FString::Printf(TEXT("entry=%s exit=%s caster=%s ward=%s"),*Via.Entry.ToString(),*Via.Exit.ToString(),*Arrival.ToString(),*F.Ward->GetActorLocation().ToString()));
 if(!TestTrue(TEXT("ward covers the actual mapped exit"),FVector::Dist2D(F.Ward->GetActorLocation(),Via.Exit)<120)) return false;
 F.Placer->SetActorLocation(F.B->GetPawn()->GetActorLocation()+FVector(0,0,150)); F.Tick(1.1f);
 FWoTBotWardAccess::Sense(F.B);
 TestTrue(TEXT("team placement is known at the remote exit"),FWoTBotWardAccess::Known(F.B,F.Ward));
 FWoTBotWardAccess::Route(F.B,Via.Entry);
 TestTrue(TEXT("bot refuses entry to a known explosive exit"),F.B->GetMoveStatus()!=EPathFollowingStatus::Moving);
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardLeaveZoneSpec,"WoT.Bots.Wards.LeavesZoneItStandsIn",WoTBotTest::Flags)
bool FWardLeaveZoneSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this)) return false;
 // The caster leaves the match: only the ward is left to react to.
 F.Placer->Destroy();
 auto* Body=CastChecked<AWoTCharacter>(F.B->GetPawn());
 const FVector WardAt=F.Ward->GetActorLocation();
 Body->SetActorLocation(FVector(WardAt.X,WardAt.Y,Body->GetActorLocation().Z)-F.Direction*100);
 TestTrue(TEXT("bot stands inside the ward's blast"),FVector::Dist2D(Body->GetActorLocation(),WardAt)<120);
 TestTrue(TEXT("armed ward about 100 units away noticed through sight"),F.Notice());
 FWoTBotWardAccess::Wake(F.B);
 // Shorter than any wait for a blocked route: a bot inside the zone leaves at once.
 float Farthest=0, OutAt=-1; bool Jump=false; FVector Last=Body->GetActorLocation();
 for(int I=0;I<150;++I)
 {
  F.Tick(Step);
  if(F.B->GetPawn()!=Body) break;
  const FVector Here=Body->GetActorLocation(); Jump|=FVector::Dist(Here,Last)>100; Last=Here;
  Farthest=FMath::Max(Farthest,float(FVector::Dist2D(Here,WardAt)));
  if(OutAt<0 && Farthest>=150) OutAt=(I+1)*Step;
 }
 AddInfo(FString::Printf(TEXT("farthest from ward %.0f; out of the zone after %.2f s"),Farthest,OutAt));
 TestTrue(TEXT("same body throughout: no silent respawn"),F.B->GetPawn()==Body);
 TestFalse(TEXT("walked, never teleported"),Jump);
 TestTrue(TEXT("ticking bot retreats out of the known ward's zone"),Farthest>=150);
 TestTrue(TEXT("ward survives the retreat"),IsValid(F.Ward)); return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardChainSpec,"WoT.Bots.Wards.ShotChecksKnownChain",WoTBotTest::Flags)
bool FWardChainSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FWardFixture F; if(!F.Setup(*this)) return false;
 AWoTExpWardProjectile* Far=F.Ward;
 AWoTExpWardProjectile* Near=F.PlaceAnother(F.Start+F.Direction*240);
 if(!TestNotNull(TEXT("second ward placed by a real cast"),Near)) return false;
 F.Tick(1.1f);
 if(!TestTrue(TEXT("second ward armed"),Near->bArmed)) return false;
 const float Apart=FVector::Dist(Far->GetActorLocation(),Near->GetActorLocation());
 TestTrue(TEXT("the far ward's blast reaches the near ward"),Apart<Far->DamageRadius);
 auto* Body=CastChecked<AWoTCharacter>(F.B->GetPawn());
 const FVector FarAt=Far->GetActorLocation();
 Body->SetActorLocation(FVector(FarAt.X,FarAt.Y,Body->GetActorLocation().Z)-F.Direction*170);
 const float ToFar=FVector::Dist(Body->GetActorLocation(),FarAt), ToNear=FVector::Dist(Body->GetActorLocation(),Near->GetActorLocation());
 AddInfo(FString::Printf(TEXT("wards %.0f apart; shooter %.0f from far, %.0f from near"),Apart,ToFar,ToNear));
 TestTrue(TEXT("far ward alone is outside blast plus body"),ToFar>Far->DamageRadius+Body->GetSimpleCollisionRadius()+8);
 TestTrue(TEXT("near ward's blast reaches the shooter"),ToNear<Near->DamageRadius);
 for(int I=0;I<80 && !(FWoTBotWardAccess::Known(F.B,Far) && FWoTBotWardAccess::Known(F.B,Near));++I)
 { F.Look(); Far->ScaleGlow=Near->ScaleGlow=1; FWoTBotWardAccess::Sense(F.B); F.Tick(.1f); }
 TestTrue(TEXT("both wards noticed through sight"),FWoTBotWardAccess::Known(F.B,Far) && FWoTBotWardAccess::Known(F.B,Near));
 Give(Body,TEXT("/Script/WoT.WoTAngrealInvDart"));
 // The bot wants to go on past both: its real route check stops it, and the
 // far ward, outside blast plus body on its own, is the one in its way.
 FWoTBotWardAccess::Route(F.B,F.End);
 if(!TestTrue(TEXT("route check records the far ward as blocking"),FWoTBotWardAccess::Blocking(F.B)==Far)) return false;
 TStrongObjectPtr<UWoTBotWardObserver> Observer(NewObject<UWoTBotWardObserver>()); Observer->Watch(Far); Observer->Watch(Near);
 const int32 Health=Body->Health;
 const auto Aiming=[&](AActor* W) { return IsValid(W) && FWoTBotWardAccess::AimingAt(F.B,W); };
 bool Deliberate=false;
 for(int I=0;I<80;++I) { F.Look(); FWoTBotWardAccess::Sense(F.B); FWoTBotWardAccess::Cast(F.B); Deliberate|=Aiming(Far)||Aiming(Near); F.Tick(.1f); }
 TestFalse(TEXT("no deliberate shot whose known chain reaches the shooter"),Deliberate);
 TestEqual(TEXT("no ward shot by the bot"),Observer->ShotByBots,0);
 TestTrue(TEXT("both wards intact"),IsValid(Far) && IsValid(Near));
 TestEqual(TEXT("shooter unhurt"),Body->Health,Health);
 // Beliefs only: with the near ward unknown, the far one is a fair shot.
 FWoTBotWardAccess::Forget(F.B,Near);
 bool Shot=false;
 for(int I=0;I<80 && IsValid(Far);++I) { F.Look(); FWoTBotWardAccess::Cast(F.B); Shot|=Aiming(Far); F.Tick(.1f); }
 AddInfo(FString::Printf(TEXT("with only the far ward known: aimed=%d bot shots=%d far=%d near=%d shooter health %d"),Shot,Observer->ShotByBots,IsValid(Far),IsValid(Near),Body->Health));
 TestTrue(TEXT("with only the far ward known the bot may shoot it"),Shot); return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardTeammateSpec,"WoT.Bots.Wards.TeammateInBlastVetoesShot",WoTBotTest::Flags)
bool FWardTeammateSpec::RunTest(const FString&)
{
 using namespace BotWardSpec;
 for(int Mate=1;Mate>=0;--Mate) { FCorridor F; if(!F.Setup(*this)) return false;
  Give(CastChecked<AWoTCharacter>(F.B->GetPawn()),TEXT("/Script/WoT.WoTAngrealInvDart"));
  if(Mate)
  {
   F.Placer->SetActorLocation(F.Ward->GetActorLocation()+FVector(80,0,46));
   TestTrue(TEXT("teammate beside the ward is in the bot's view"),FWoTBotWardAccess::Visible(F.B,F.Placer));
  }
  bool Deliberate=false;
  for(int I=0;I<600 && IsValid(F.Ward);++I) { WoTTest::TickWorld(F.W,Step); Deliberate|=IsValid(F.Ward) && FWoTBotWardAccess::AimingAt(F.B,F.Ward); }
  if(Mate)
  {
   TestFalse(TEXT("no deliberate detonation whose blast reaches a visible teammate"),Deliberate);
   TestTrue(TEXT("ward intact beside the teammate"),IsValid(F.Ward));
  }
  else TestTrue(TEXT("the same blocked bot shoots the ward with no teammate near it"),Deliberate);
 } return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWardCorridorWaitSpec,"WoT.Bots.Wards.CorridorWaitIsBounded",WoTBotTest::Flags)
bool FWardCorridorWaitSpec::RunTest(const FString&)
{
 using namespace BotWardSpec; FCorridor F; if(!F.Setup(*this)) return false;
 // Nothing that could detonate the ward: the wait can end only by leaving.
 auto* Body=CastChecked<AWoTCharacter>(F.B->GetPawn());
 const auto Held=Body->InventoryComponent->Items;
 for(AWoTInventory* Item:Held) { Body->InventoryComponent->DeleteInventory(Item); Item->Destroy(); }
 const FVector WardAt=F.Ward->GetActorLocation();
 float Closest=1e9f, Run=0, Longest=0, Moved=0; bool Waited=false, Resumed=false, Jump=false;
 FVector Last=Body->GetActorLocation(), WaitedAt=Last;
 for(int I=0;I<720;++I)
 {
  WoTTest::TickWorld(F.W,Step);
  if(F.B->GetPawn()!=Body) break;
  const FVector Here=Body->GetActorLocation(); Jump|=FVector::Dist(Here,Last)>100; Last=Here;
  Closest=FMath::Min(Closest,float(FVector::Dist2D(Here,WardAt)));
  if(FWoTBotWardAccess::Waiting(F.B)) { if(!Waited) WaitedAt=Here; Waited=true; Run+=Step; Longest=FMath::Max(Longest,Run); }
  else { Resumed|=Run>0; Run=0; }
  if(Resumed) Moved=FMath::Max(Moved,float(FVector::Dist2D(Here,WaitedAt)));
 }
 AddInfo(FString::Printf(TEXT("longest wait %.2f s; moved %.0f after the first wait; closest %.0f"),Longest,Moved,Closest));
 TestTrue(TEXT("same body throughout: no silent respawn"),F.B->GetPawn()==Body);
 TestFalse(TEXT("walked, never teleported"),Jump);
 TestTrue(TEXT("bot stops short of the known ward"),Waited);
 TestTrue(TEXT("route stays outside blast plus body radius"),Closest>=137);
 TestTrue(TEXT("ward intact"),IsValid(F.Ward));
 TestTrue(TEXT("waiting before the ward is bounded"),Longest<=3.5f);
 TestTrue(TEXT("after waiting the bot takes another route"),Moved>=100); return !HasAnyErrors();
}

#endif

