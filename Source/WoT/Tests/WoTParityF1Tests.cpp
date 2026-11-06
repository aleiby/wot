// Retail AI parity, through possessed controllers and the normal world tick.
// Navigation is the existing empty-world fixture; selection and dispatch stay live.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS
#include "WoTTestAIClasses.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTNPCWeapon.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvIce.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTLegionInvSeeker.h"
#include "../Inventory/WoTAbsorbReflector.h"
#include "../Inventory/WoTAMAReflectors.h"
#include "../Inventory/WoTReturnToSenderReflector.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTIceProjectile.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "../Traps/WoTAlarm.h"
#include "../WoTGameMode.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTMeshFX.h"
#include "../WoTUE1Collision.h"
#include "../WoTPlayerState.h"
#include "../WoTSealAltar.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Components/BoxComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "UObject/UnrealType.h"

namespace
{
 constexpr auto F1Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

 AWoTNPCCharacter* F1Creature(UWorld* World, FName Row, const FVector& At = FVector(0, 0, 100))
 {
  AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(*World, Row, At, 1, true,
   AWoTTestNPCCharacter::StaticClass());
  if (NPC) NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
  return NPC;
 }

 AWoTNPCController* F1Controller(AWoTNPCCharacter* NPC)
 {
  return NPC ? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
 }

 // The Fireball fixture's authored MaxChargeUsedInterval=.67 releases each
 // use immediately. Dart has a continuous real volley (interval0), needed to
 // observe per-tick re-aim and an impact interrupt while input remains held.
 AWoTTestCasterNPC* F1VolleyNPC(UWorld* World,
  TSubclassOf<AWoTAngrealInventory> WeaveClass = AWoTAngrealInvDart::StaticClass())
 {
  const FTransform Transform(FVector(0, 0, 100));
  AWoTTestCasterNPC* NPC = World->SpawnActorDeferred<AWoTTestCasterNPC>(
   AWoTTestCasterNPC::StaticClass(), Transform, nullptr, nullptr,
   ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
  NPC->AssignedTeam = 1;
  NPC->Caster->HoldRange = 2000;
  NPC->Caster->Weaves[0].WeaveClass = WeaveClass;
  // A deterministic long group proves Ice contact, rather than ordinary group
  // exhaustion, ends a Dart volley.
  NPC->Caster->Weaves[0].MinChargesInGroup = 20;
  NPC->Caster->Weaves[0].MaxChargesInGroup = 20;
  NPC->FinishSpawning(Transform);
  NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
  return NPC;
 }

 AWoTCharacter* F1Player(UWorld* World, const FVector& At, uint8 Team = 2)
 {
  FActorSpawnParameters Params;
  Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  APlayerController* PC = World->SpawnActor<APlayerController>();
  PC->InitPlayerState();
  if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) PS->Team = Team;
  AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, Params);
  PC->Possess(Player);
  Player->Health = 100000;
  Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
  return Player;
 }

 void F1ActivityAnchor(UWorld* World, const FVector& At)
 {
  FActorSpawnParameters Params;
  Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  AWoTCharacter* Anchor = World->SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, Params);
  Anchor->Health = 0;
  Anchor->bDead = true;
  Anchor->SetActorHiddenInGame(true);
  Anchor->SetActorEnableCollision(false);
 }

 // Bounded polling observes transient states/animations without invoking an AI handler.
 bool F1Until(UWorld* World, float Seconds, TFunctionRef<bool()> Predicate)
 {
  for (int32 Frame = 0; Frame < FMath::CeilToInt(Seconds * 60); ++Frame)
  {
   if (Predicate()) return true;
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  }
  return Predicate();
 }

 // Production cast/flight/contact fixture. It deliberately observes the
 // accepted leech rather than attaching one directly.
 UWoTIceLeech* F1FreezeWithRealIce(UWorld* World, AWoTCharacter* Caster,
  AWoTNPCCharacter* Victim, float Timeout = 1.5f)
 {
  if (!World || !Caster || !Victim) return nullptr;
  Caster->GetController()->SetControlRotation(
   (Victim->GetActorLocation() - Caster->GetActorLocation()).Rotation());
  const FTransform At(Caster->GetActorLocation());
  AWoTAngrealInvIce* Ice = World->SpawnActorDeferred<AWoTAngrealInvIce>(
   AWoTAngrealInvIce::StaticClass(), At, Caster, Caster,
   ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
  if (!Ice) return nullptr;
  Ice->bHeldItem = true;
  Ice->FinishSpawning(At);
  Ice->CurCharges = 10;
  Ice->GiveTo(Caster);
  Caster->InventoryComponent->SelectItem(Ice);
  // DefaultCastReflector uses a strict world-time gate.
  if (World->GetTimeSeconds() <= 0.0f) WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  Caster->InventoryComponent->UseAngreal();
  UWoTIceLeech* Accepted = nullptr;
  F1Until(World, Timeout, [&]
  {
   Accepted = Victim->FindComponentByClass<UWoTIceLeech>();
   return Accepted != nullptr;
  });
  return Accepted;
 }

}

// MovementPatternTrolloc0.uc:12; LegendPawn.TrackGoal -> InitMovement -> PerformMove.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1PatternPursue, "WoT.Parity.F1.MovementPattern.Pursue425", F1Flags)
bool FWoTF1PatternPursue::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 // Put the target beyond every valid stationary axe choice so this spec tests
 // MovementPatternTrolloc0's Pursue speed rather than a lucky attack roll.
 F1Player(World, FVector(2000, 0, 100));
 if (TestNotNull(TEXT("possessed Trolloc"), F1Controller(NPC)))
 {
  TestTrue(TEXT("real sight acquires the player"), F1Until(World, 1, [&] {
   return NPC->Brain->IsGoalValid(EWoTGoalIndex::Threat); }));
  TestTrue(TEXT("authored425 supplies effective340 with NPC desired-speed fraction0.8"),
   F1Until(World, 2, [&] { return FMath::IsNearlyEqual(NPC->GetCharacterMovement()->MaxWalkSpeed, 340.0f)
    && FMath::IsNearlyEqual(F1Controller(NPC)->GetMovementManager().SuggestedSpeed, 425.0f); }));
  TestTrue(TEXT("pursuit physically advances"), F1Until(World,1,[&]{return NPC->GetActorLocation().X>5;}));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Trolloc BackOff1/2: interval 2/3s, diagonal waypoint, pre-hint AttemptAttack.
// Disable only the ordinary timed cadence, so a swing must come from the hint.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1PatternHints, "WoT.Parity.F1.MovementPattern.BackOffAttackHint", F1Flags)
bool FWoTF1PatternHints::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 AWoTCharacter* Player = F1Player(World, FVector(50, 0, 100));
 if (TestNotNull(TEXT("controller"), F1Controller(NPC)))
 {
  FWoTDurationNotifier& Offensive = NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
  Offensive.Period = 1000;
  Offensive.bRandomizePhase = false;
  Offensive.Elapsed = 0;
  bool bSawDiagonal = false;
  bool bSawHintSwing = false;
  for (int32 Frame = 0; Frame < 360; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   bSawDiagonal |= FMath::Abs(NPC->GetActorLocation().Y) > 4;
   bSawHintSwing |= !F1Controller(NPC)->GetLastSwingSlot().IsNone();
  }
  TestEqual(TEXT("target remains alive during the choreography"), Player->Health > 0, true);
  TestTrue(TEXT("contact range selects an offset rather than standing planted"), bSawDiagonal);
  TestTrue(TEXT("AttemptAttack hint starts a swing before the 1000s ordinary pulse"), bSawHintSwing);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1PatternCycle, "WoT.Parity.F1.MovementPattern.FixedVectorAndHintOrder", F1Flags)
bool FWoTF1PatternCycle::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 auto* NPC = F1Creature(World,TEXT("Trolloc"));
 if (NPC && NPC->GetController()) { auto* Previous=NPC->GetController(); Previous->UnPossess(); Previous->Destroy(); }
 auto* Controller = NPC ? World->SpawnActor<AWoTTestMovementObservationController>() : nullptr;
 if (Controller) Controller->Possess(NPC);
 if (TestNotNull(TEXT("real possessed creature observer"),Controller))
 {
  NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
  FWoTMovementPattern Pattern; Pattern.ClassIdentity=TEXT("TimingFixture");
  auto& E=Pattern.Elements[0]; E.bEnabled=true; E.bApplySpeed=true; E.Speed=200;
  E.PostHint=TEXT("AttemptAttack");
  E.Waypoint.bApplyDirection=true; E.Waypoint.Direction=FVector(0,1,0);
  E.Waypoint.bApplyDistance=true; E.Waypoint.Distance=128;
  Controller->SetPatternFixture(Pattern);
  auto& Offensive=NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
  Offensive.Period=1000; Offensive.Elapsed=0; Offensive.bRandomizePhase=false;
  auto* Player=F1Player(World,FVector(1000,0,100));
  const bool bPre=F1Until(World,2,[&]{return Controller->GetMovementManager().States.Contains(21);});
  TestTrue(TEXT("real sight initializes the movement cycle"),bPre);
  if (!bPre)
  {
   AddInfo(FString::Printf(TEXT("timing fixture state=%d goal=%d row=%s pattern=%s handler=%d elapsed=%f pulses=%d"),static_cast<int32>(Controller->GetBrainState()),static_cast<int32>(NPC->Brain->CurrentGoalIndex),*NPC->CreatureRowName.ToString(),*Controller->GetTrackingBands().Band(EWoTGoalProximity::Reachable).MovementBinding.Pattern.ClassIdentity.ToString(),Controller->GetTrackingBands().Band(EWoTGoalProximity::Reachable).MovementBinding.HandlerIndex,Offensive.Elapsed,Controller->OffensivePulseLocations.Num()));
   WoTTest::DestroySpecWorld(World); return false;
  }
  const FVector Destination=Controller->GetMovementManager().Destination;
  const float SelectionTime=Controller->GetMovementManager().States.FindChecked(21).LastSelectionTimes[0];
  TestEqual(TEXT("no pre hint is authored"),Controller->OffensivePulseLocations.Num(),0);
  Player->SetActorLocation(FVector(1400,100,100));
  WoTTest::TickWorld(World,10,1.0f/60.0f);
  TestTrue(TEXT("actor motion cannot replace a fixed vector waypoint"),Controller->GetMovementManager().Destination.Equals(Destination));
  TestEqual(TEXT("tracking does not reselect each frame"),Controller->GetMovementManager().States.FindChecked(21).LastSelectionTimes[0],SelectionTime);
  TestEqual(TEXT("post hint waits while moving"),Controller->OffensivePulseLocations.Num(),0);
  TestTrue(TEXT("post hint eventually fires"),F1Until(World,3,[&]{return Controller->OffensivePulseLocations.Num()>=1;}));
  if (Controller->OffensivePulseLocations.Num()>=1)
  {
   TestTrue(TEXT("post hint follows arrival"),FVector::Dist2D(Controller->OffensivePulseLocations[0],Destination)<16.1f);
   TestTrue(TEXT("turn/move precedes post hint"),Controller->OffensivePulseTimes[0]>SelectionTime+.1f);
  }
  TestTrue(TEXT("hint notify preserves ordinary cadence clock"),Offensive.Elapsed>.1f && Offensive.Elapsed<10);
 }
 WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1PreAttackAbandonsMove, "WoT.Parity.F1.MovementPattern.PreAttackDiscardsCycleWhenOutOfRange", F1Flags)
bool FWoTF1PreAttackAbandonsMove::RunTest(const FString&)
{
 UWorld* World=WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 // Expire the authored cooldown before spawning the NPC; its idle turns must
 // not change the initial sight fixture while waiting for the clock.
 WoTTest::TickWorld(World,5.1f,1.0f/60.0f);
 auto* NPC=F1Creature(World,TEXT("Trolloc"));
 if (NPC && NPC->GetController()) {auto* Old=NPC->GetController(); Old->UnPossess(); Old->Destroy();}
 auto* C=NPC ? World->SpawnActor<AWoTTestMovementObservationController>() : nullptr;
 if(C) C->Possess(NPC);
 if(TestNotNull(TEXT("possessed pre-hint observer"),C))
 {
  FWoTMovementPattern P; P.ClassIdentity=TEXT("PreAttackFixture");
  auto& E=P.Elements[0]; E.bEnabled=true; E.bApplySpeed=true; E.Speed=800;
  E.IntervalRelation=EWoTMovementRelation::GreaterOrEqual; E.Interval=5;
  E.PreHint=E.PostHint=TEXT("AttemptAttack"); E.Waypoint.bApplyDirection=true;
  E.Waypoint.Direction=FVector(-1,1,0); E.Waypoint.bApplyDistance=true; E.Waypoint.Distance=128;
  C->SetPatternFixture(P);
  auto& N=NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
  N.Period=1000; N.Elapsed=0; N.bRandomizePhase=false;
  F1Player(World,FVector(1000,0,100));
  const bool bHint=F1Until(World,2,[&]{return C->OffensivePulseLocations.Num()>0;});
  TestTrue(TEXT("enabled pre hint notifies before ordinary cadence"),bHint);
  if(bHint && C->GetMovementManager().States.Contains(21))
  {
   const float Stamp=C->GetMovementManager().States.FindChecked(21).LastSelectionTimes[0];
   const float InitialY=NPC->GetActorLocation().Y;
   TestTrue(TEXT("selected candidate is a diagonal"),C->GetMovementManager().Destination.Y>InitialY+10);
   TestEqual(TEXT("pre hint consumes cursor"),C->GetMovementManager().States.FindChecked(21).Cursor,1);
   TestTrue(TEXT("pre hint consumes nonzero selection time"),Stamp>0);
   TestTrue(TEXT("distant target prevents an actual attack"),C->GetLastSwingSlot().IsNone());
   WoTTest::TickWorld(World,2.0f,1.0f/60.0f);
   TestTrue(TEXT("discarded diagonal is never traversed"),FMath::Abs(NPC->GetActorLocation().Y-InitialY)<.1f);
   TestEqual(TEXT("discarded cycle emits no post hint"),C->OffensivePulseLocations.Num(),1);
   TestEqual(TEXT("cooldown timestamp survives discarded cycle"),C->GetMovementManager().States.FindChecked(21).LastSelectionTimes[0],Stamp);
   TestTrue(TEXT("failed attempt keeps ordinary notifier clock"),N.Elapsed>.5f && N.Elapsed<10);
  }
 }
 WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1Snipe, "WoT.Parity.F1.MovementPattern.MyrddraalSnipeSelectsValidNode", F1Flags)
bool FWoTF1Snipe::RunTest(const FString&)
{
 for(bool bAddValid : {false,true})
 {
  UWorld* World=WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  auto* NPC=F1Creature(World,TEXT("Myrddraal"));
  if(NPC && NPC->GetController()){auto* Old=NPC->GetController(); Old->UnPossess(); Old->Destroy();}
  auto* C=NPC ? World->SpawnActor<AWoTTestMovementObservationController>() : nullptr;
  if(C) C->Possess(NPC);
  if(TestNotNull(TEXT("possessed Myrddraal"),C))
  {
   FWoTMovementPattern P; P.ClassIdentity=TEXT("SnipeFixture");
   P.Elements[0].bEnabled=true; P.Elements[0].PreHint=TEXT("Snipe");
   P.Elements[0].Waypoint.bRequireReachable=false; C->SetPatternFixture(P);
   auto& N=NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
   N.Period=1000; N.Elapsed=0; N.bRandomizePhase=false;
   auto Node=[&](FVector At,bool bFlag){auto* V=World->SpawnActor<AWoTPathNode>(At,FRotator::ZeroRotator); V->bSnipe=bFlag; V->SourceClass=TEXT("WOT.WotPathNode"); return V;};
   Node(FVector(50,-50,100),false);
   auto* Blocked=Node(FVector(100,200,100),true);
   auto* Far=Node(FVector(2400,0,100),true);
   auto* Best=bAddValid ? Node(FVector(600,400,100),true) : nullptr;
   if(bAddValid) Node(FVector(800,800,100),true);
   auto* Wall=World->SpawnActor<AStaticMeshActor>();
   auto* Box=NewObject<UBoxComponent>(Wall); Wall->SetRootComponent(Box);
   Box->SetBoxExtent(FVector(25,25,200)); Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
   Box->SetCollisionObjectType(ECC_WorldStatic); Box->SetCollisionResponseToAllChannels(ECR_Block);
   Box->RegisterComponent(); Wall->SetActorLocation(FVector(500,111,130));
   auto* Player=F1Player(World,FVector(1000,0,100)); FHitResult Hit;
   TestTrue(TEXT("blocked node ray hits actual wall"),WoTRetailActorTrace(World,Blocked->GetActorLocation()+FVector(0,0,60),Player->GetActorLocation(),Blocked,Hit) && Hit.GetActor()==Wall);
   TestTrue(TEXT("far node is outside2250"),FVector::Dist(Far->GetActorLocation(),NPC->GetActorLocation())>2250);
   if(Best) TestTrue(TEXT("valid node actor trace hits target"),WoTRetailActorTrace(World,Best->GetActorLocation()+FVector(0,0,60),Player->GetActorLocation(),Best,Hit) && Hit.GetActor()==Player);
   TestTrue(TEXT("real tracking executes Snipe cell"),F1Until(World,2,[&]{return C->GetMovementManager().States.Contains(21);}));
   if(bAddValid) TestEqual(TEXT("nearest flagged visible node becomes Intermediate"),NPC->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(),static_cast<AActor*>(Best));
   else TestFalse(TEXT("blocked distant and unflagged nodes cannot become Intermediate"),NPC->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
  }
  WoTTest::DestroySpecWorld(World);
 }
 return !HasAnyErrors();
}

// LegendPawn.uc:1816-1831: Movement notification re-accelerates every melee tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1MovingMelee, "WoT.Parity.F1.Melee.FollowsRetreatDuringSwing", F1Flags)
bool FWoTF1MovingMelee::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 AWoTCharacter* Player = F1Player(World, FVector(65, 0, 100));
 AWoTNPCController* Controller = F1Controller(NPC);
 if (TestNotNull(TEXT("controller"), Controller))
 {
  const bool bSwing = F1Until(World, 3, [&] { return Controller->GetBrainState() == EWoTBrainState::PerformMelee; });
  TestTrue(TEXT("normal offensive selection starts melee"), bSwing);
  if (bSwing)
  {
   const FVector Before = NPC->GetActorLocation();
   Player->SetActorLocation(Player->GetActorLocation() + FVector(50, 0, 0));
   WoTTest::TickWorld(World, 6, 1.0f / 60.0f);
   TestTrue(TEXT("active swing accelerates toward the retreating goal"),
    NPC->GetCharacterMovement()->GetCurrentAcceleration().X > 0);
   TestTrue(TEXT("the attacker closes during the swing"), NPC->GetActorLocation().X > Before.X + 1);
  }
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Warder.PlayStateAnimation and Minion grab explicitly disable the inherited
// movement pulse for their rooted attacks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1StationaryMeleeExceptions,
 "WoT.Parity.F1.Melee.StationaryAttackExceptions", F1Flags)
bool FWoTF1StationaryMeleeExceptions::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* Warder = F1Creature(World, TEXT("Warder"));
 AWoTCharacter* Player = F1Player(World, FVector(45, 0, 100));
 AWoTNPCController* Controller = F1Controller(Warder);
 if (TestNotNull(TEXT("Warder controller"), Controller))
 {
  Warder->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Player);
  Warder->Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
  bool bSawRootedSequence = false;
  for (int32 Attempt = 0; Attempt < 64 && !bSawRootedSequence; ++Attempt)
  {
   Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
   Controller->SetBrainStateForG20Test(EWoTBrainState::PerformMelee);
   const FName Sequence = Warder->GetReplicatedAnimName();
   if (Sequence == TEXT("SkewRip") || Sequence == TEXT("DbleSwipe"))
   {
    bSawRootedSequence = true;
    TestFalse(TEXT("Warder rooted swing disables the movement notifier"),
     Warder->Brain->IsNotifierEnabled(EWoTNotifierSlot::Movement));
    WoTTest::TickWorld(World, 2, 1.0f / 60.0f);
    TestTrue(TEXT("Warder rooted swing remains stopped"),
     Warder->GetCharacterMovement()->GetCurrentAcceleration().IsNearlyZero(0.1f));
   }
  }
  TestTrue(TEXT("real Warder attack table selected a rooted sequence"), bSawRootedSequence);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Both Waiting/Returning items: LegendPawn.uc:603-609,2376-2457; authored facing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1ReturnPost, "WoT.Parity.F1.Waiting.ReturnsToPostAndFacing", F1Flags)
bool FWoTF1ReturnPost::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 F1ActivityAnchor(World, NPC->GetActorLocation());
 AWoTCharacter* Player = F1Player(World, FVector(1000, 0, 100));
 if (TestNotNull(TEXT("controller"), F1Controller(NPC)))
 {
  const FVector Post = NPC->GetActorLocation();
  NPC->Brain->SetWaitingGoal(Post, FRotator(0, 90, 0));
  const bool bLeftPost = F1Until(World, 4, [&] {
   return FVector::Dist2D(NPC->GetActorLocation(), Post) > 100; });
  TestTrue(FString::Printf(TEXT("real sight and pursuit leave the post (state %d, distance %.1f, threat %s)"),
   static_cast<int32>(F1Controller(NPC)->GetBrainState()),
   FVector::Dist2D(NPC->GetActorLocation(), Post),
   *GetNameSafe(NPC->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get())), bLeftPost);
  Player->Destroy(); // actual goal-actor lifetime invalidation, no state injection
  TestTrue(TEXT("lost threat returns the guard to its placed post"), F1Until(World, 10, [&] {
   return FVector::Dist2D(NPC->GetActorLocation(), Post) <= 20; }));
  WoTTest::TickWorld(World, 1.0f);
  TestTrue(TEXT("returned guard restores authored yaw"),
   FMath::Abs(FMath::FindDeltaAngleDegrees(NPC->GetActorRotation().Yaw, 90.0f)) < 5);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1ReturnRestartAndUnable,
 "WoT.Parity.F1.Waiting.ChangedPostAndUnableReseat", F1Flags)
bool FWoTF1ReturnRestartAndUnable::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 F1ActivityAnchor(World, NPC->GetActorLocation());
 if (TestNotNull(TEXT("returning NPC"), NPC) && TestNotNull(TEXT("controller"), F1Controller(NPC)))
 {
  NPC->Brain->SetWaitingGoal(FVector(500, 0, 100), FRotator(0, 45, 0));
  TestTrue(TEXT("changed waiting goal starts a real return leg"), F1Until(World, 2, [&] {
   return NPC->GetActorLocation().X > 60; }));
  NPC->Brain->SetWaitingGoal(FVector(0, 500, 100), FRotator(0, 180, 0));
  TestTrue(TEXT("a second waiting goal restarts toward the new post"), F1Until(World, 8, [&] {
   return FVector::Dist2D(NPC->GetActorLocation(), FVector(0, 500, 100)) < 20; }));
  TestTrue(TEXT("restarted return adopts the second facing"), F1Until(World, 2, [&] {
   return FMath::Abs(FMath::FindDeltaAngleDegrees(NPC->GetActorRotation().Yaw, 180)) < 5; }));

  UWoTTestBrainComponent* TestBrain = Cast<UWoTTestBrainComponent>(NPC->Brain);
  if (TestNotNull(TEXT("forced navigation brain"), TestBrain))
  {
   TestBrain->bForceReachable = false;
   TestBrain->bForcePathable = false;
   const FVector Stranded = NPC->GetActorLocation();
   const FRotator StrandedFacing = NPC->GetActorRotation();
   NPC->Brain->SetWaitingGoal(Stranded + FVector(600, 0, 0), FRotator(0, 15, 0));
   TestTrue(TEXT("unreachable return re-seats the waiting post"), F1Until(World, 1, [&] {
    const FWoTGoal& Waiting = NPC->Brain->GetGoal(EWoTGoalIndex::Waiting);
    return FVector::Dist(Waiting.GoalVector, Stranded) < 1
     && FMath::Abs(FMath::FindDeltaAngleDegrees(
      Waiting.AssociatedRotation.Yaw, StrandedFacing.Yaw)) < 1;
   }));
  }
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Returning expands Tracking in retail: it retains HearNoise and continuously
// polls higher-priority goals while walking home. Exercise all three event
// sources after a real chase/lost-threat transition, never by entering Returning
// directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1ReturningInterruptions,
 "WoT.Parity.F1.Waiting.ReturningRetainsTrackingInterruptions", F1Flags)
bool FWoTF1ReturningInterruptions::RunTest(const FString&)
{
 // Audible enemy: an NPC source is Dormant so only its real EmitNoise path can
 // reveal it (no show-self visibility pulse races the hearing assertion).
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTNPCCharacter* Guard = F1Creature(World, TEXT("Trolloc"));
  Guard->AssignedTeam = 1;
  // Keep the return leg long regardless of which valid ranged decision the
  // deterministic gameplay stream chooses before the interruption.
  AWoTCharacter* First = F1Player(World, FVector(2000, 0, 100), 2);
  AWoTNPCController* Controller = F1Controller(Guard);
  const FVector Post = Guard->GetActorLocation();
  if (TestNotNull(TEXT("hearing-return controller"), Controller))
  {
   Guard->Brain->SetWaitingGoal(Post, FRotator::ZeroRotator);
   TestTrue(TEXT("hearing control enters real pursuit away from post"), F1Until(World, 4, [&] {
    return FVector::Dist2D(Guard->GetActorLocation(), Post) > 100
     && Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == First
     && Controller->GetBrainState() == EWoTBrainState::Tracking; }));
   First->Destroy();
   TestTrue(TEXT("lost threat enters real Returning before hearing"), F1Until(World, 1, [&] {
    return Controller->GetBrainState() == EWoTBrainState::Returning
     && FVector::Dist2D(Guard->GetActorLocation(), Post) > 100; }));
   if (UWoTTestBrainComponent* TestBrain = Cast<UWoTTestBrainComponent>(Guard->Brain))
   {
    TestBrain->bForceVisible = false;
   }
   AWoTNPCCharacter* NoiseEnemy = F1Creature(World, TEXT("Trolloc"),
    Guard->GetActorLocation() + FVector(300, 0, 0));
   NoiseEnemy->AssignedTeam = 2;
   if (AWoTNPCController* NoiseController = F1Controller(NoiseEnemy))
   {
    NoiseController->ForceDormant();
   }
   NoiseEnemy->EmitNoise(1.0f);
   TestTrue(TEXT("Returning inherits live HearNoise and promptly adopts threat"),
    F1Until(World, 0.5f, [&] {
     return Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == NoiseEnemy
      && Controller->GetBrainState() == EWoTBrainState::Tracking; }));
  }
  WoTTest::DestroySpecWorld(World);
 }

 // Visible enemy while InitialAcquisition is still inside its 60-second gate:
 // the shout is suppressed, but ordinary Threat priority must still interrupt
 // Returning before the pawn reaches its post.
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTNPCCharacter* Guard = F1Creature(World, TEXT("Trolloc"));
  Guard->AssignedTeam = 1;
  AWoTCharacter* First = F1Player(World, FVector(2000, 0, 100), 2);
  AWoTNPCController* Controller = F1Controller(Guard);
  const FVector Post = Guard->GetActorLocation();
  if (TestNotNull(TEXT("visibility-return controller"), Controller))
  {
   Guard->Brain->SetWaitingGoal(Post, FRotator::ZeroRotator);
   TestTrue(TEXT("visibility control enters real pursuit away from post"), F1Until(World, 4, [&] {
    return FVector::Dist2D(Guard->GetActorLocation(), Post) > 100
     && Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == First
     && Controller->GetBrainState() == EWoTBrainState::Tracking; }));
   First->Destroy();
   TestTrue(TEXT("lost first target starts return inside acquisition cooldown"),
    F1Until(World, 1, [&] {
     return Controller->GetBrainState() == EWoTBrainState::Returning
      && FVector::Dist2D(Guard->GetActorLocation(), Post) > 100; }));
   AWoTCharacter* Second = F1Player(World,
    Guard->GetActorLocation() + FVector(500, 100, 0), 2);
   TestTrue(TEXT("visible threat during cooldown interrupts return before home"),
    F1Until(World, 1, [&] {
     return Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == Second
      && Guard->Brain->CurrentGoalIndex == EWoTGoalIndex::Threat
      && Controller->GetBrainState() == EWoTBrainState::Tracking
      && FVector::Dist2D(Guard->GetActorLocation(), Post) > 20; }));
  }
  WoTTest::DestroySpecWorld(World);
 }

 // Alarm/help Intermediate outranks Waiting and is polled by the inherited
 // Tracking operation during the return leg.
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTNPCCharacter* Guard = F1Creature(World, TEXT("Trolloc"));
  Guard->AssignedTeam = 1;
  AWoTCharacter* First = F1Player(World, FVector(2000, 0, 100), 2);
  AWoTNPCController* Controller = F1Controller(Guard);
  const FVector Post = Guard->GetActorLocation();
  if (TestNotNull(TEXT("alarm-return controller"), Controller))
  {
   Guard->Brain->SetWaitingGoal(Post, FRotator::ZeroRotator);
   TestTrue(TEXT("alarm control enters real pursuit away from post"), F1Until(World, 4, [&] {
    return FVector::Dist2D(Guard->GetActorLocation(), Post) > 100
     && Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == First
     && Controller->GetBrainState() == EWoTBrainState::Tracking; }));
   First->Destroy();
   TestTrue(TEXT("lost target starts return before alarm"), F1Until(World, 1, [&] {
    return Controller->GetBrainState() == EWoTBrainState::Returning; }));
   AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
    Guard->GetActorLocation() + FVector(500, 0, 0), FRotator::ZeroRotator);
   if (TestNotNull(TEXT("real alarm interruption"), Alarm))
   {
    Alarm->ActivateAlarm(nullptr);
    TestTrue(TEXT("alarm Intermediate promptly interrupts Returning"),
     F1Until(World, 0.5f, [&] {
      return Guard->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get() == Alarm
       && Guard->Brain->CurrentGoalIndex == EWoTGoalIndex::Intermediate
       && Controller->GetBrainState() == EWoTBrainState::Tracking; }));
   }
  }
  WoTTest::DestroySpecWorld(World);
 }
 return !HasAnyErrors();
}

// WOTPawn.ChangedWeapon -> PrepareInventoryItem; ArcherSword preparation capability.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1Preparation, "WoT.Parity.F1.Preparation.ArcherDrawBeforeEquip", F1Flags)
bool FWoTF1Preparation::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Archer"));
 F1Player(World, FVector(80, 0, 100));
 if (TestNotNull(TEXT("Archer controller"), F1Controller(NPC)))
 {
  bool bSawPreparation = false;
  bool bSawDraw = false;
  bool bSawDelayedWeapon = false;
  for (int32 Frame = 0; Frame < 300; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   if (F1Controller(NPC)->GetBrainState() == EWoTBrainState::PrepareWeapon)
   {
    bSawPreparation = true;
    bSawDelayedWeapon |= NPC->GetHeldWeapon() != NPC->GetMeleeWeapon();
    TestTrue(TEXT("draw state stops movement"), NPC->GetVelocity().IsNearlyZero(0.1f));
   }
   bSawDraw |= NPC->GetReplicatedAnimName() == FName(TEXT("DRAWSWRD"));
  }
  TestTrue(TEXT("automatic weapon selection enters preparation"), bSawPreparation);
  TestTrue(TEXT("Archer plays authored DRAWSWRD"), bSawDraw);
  TestTrue(TEXT("sword waits for the weapon-switch notify"), bSawDelayedWeapon);
  TestEqual(TEXT("draw finishes with melee weapon held"), NPC->GetHeldWeapon(), NPC->GetMeleeWeapon());
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1MyrddraalPreparation,
 "WoT.Parity.F1.Preparation.MyrddraalBowNotifySwitch", F1Flags)
bool FWoTF1MyrddraalPreparation::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Myrddraal"));
 F1Player(World, FVector(1000, 0, 100));
 AWoTNPCController* Controller = F1Controller(NPC);
 if (TestNotNull(TEXT("Myrddraal controller"), Controller)
  && TestNotNull(TEXT("Myrddraal bow"), NPC ? NPC->GetRangedWeapon() : nullptr))
 {
  bool bSawPrepare = false;
  bool bSawBowUp = false;
  bool bHeldOldWeaponDuringDraw = false;
  bool bSawNotifySwitch = false;
  for (int32 Frame = 0; Frame < 360; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   if (Controller->GetBrainState() == EWoTBrainState::PrepareWeapon)
   {
    bSawPrepare = true;
    bSawBowUp |= NPC->GetReplicatedAnimName() == TEXT("BowUp");
    bHeldOldWeaponDuringDraw |= NPC->GetHeldWeapon() != NPC->GetRangedWeapon();
    bSawNotifySwitch |= NPC->GetHeldWeapon() == NPC->GetRangedWeapon();
   }
   else if (bSawPrepare && NPC->GetHeldWeapon() == NPC->GetRangedWeapon())
   {
    // Stop observing this episode before normal tracking is free to change
    // weapon preference again.
    break;
   }
  }
  TestTrue(TEXT("ranged routing enters preparation"), bSawPrepare);
  TestTrue(TEXT("Myrddraal plays authored BowUp"), bSawBowUp);
  TestTrue(TEXT("bow remains hidden until its mesh notify"), bHeldOldWeaponDuringDraw);
  TestTrue(TEXT("mesh notify switches to the bow during preparation"), bSawNotifySwitch);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// The offensive notifier runs before the ordinary Tracking tick. If its range
// routing starts a preparation episode, that same pulse must not overwrite the
// draw/put-away state with an attack.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1PreparationOnOffensivePulse,
 "WoT.Parity.F1.Preparation.OffensivePulsePreservesPreparation", F1Flags)
bool FWoTF1PreparationOnOffensivePulse::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Myrddraal"));
 AWoTCharacter* Player = F1Player(World, FVector(1000, 0, 100));
 AWoTNPCController* Controller = F1Controller(NPC);
 UWoTTestBrainComponent* TestBrain = NPC ? Cast<UWoTTestBrainComponent>(NPC->Brain) : nullptr;
 AWoTNPCWeapon* Bow = NPC ? NPC->GetRangedWeapon() : nullptr;
 AWoTNPCWeapon* Sword = NPC ? NPC->GetMeleeWeapon() : nullptr;
 if (TestNotNull(TEXT("offensive-prep controller"), Controller)
  && TestNotNull(TEXT("offensive-prep brain"), TestBrain)
  && TestNotNull(TEXT("offensive-prep bow"), Bow)
  && TestNotNull(TEXT("offensive-prep sword"), Sword))
 {
  TestBrain->bForceReachable = false;
  TestBrain->bForcePathable = false;
  TestTrue(TEXT("far target first equips the real bow"), F1Until(World, 5, [&] {
   return NPC->GetHeldWeapon() == Bow
    && Controller->GetBrainState() == EWoTBrainState::Tracking; }));

  TestBrain->bForceReachable = true;
  Player->SetActorLocation(NPC->GetActorLocation() + FVector(80, 0, 0));
  FWoTDurationNotifier& Offensive =
   NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
  Offensive.Period = 0.0f;
  Offensive.Elapsed = 0.0f;
  Offensive.bRandomizePhase = false;
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);

  TestEqual(TEXT("offensive pulse leaves the synchronous route-owned state intact"),
   static_cast<int32>(Controller->GetBrainState()),
   static_cast<int32>(EWoTBrainState::PrepareWeapon));
  TestEqual(TEXT("outgoing bow remains held before its real notify"),
   NPC->GetHeldWeapon(), Bow);

  bool bPrematureAttack = false;
  bool bSawNotifySwitch = false;
  bool bSawAttackAfterPreparation = false;
  for (int32 Frame = 0; Frame < 360; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   const bool bAttacking = Controller->GetBrainState() == EWoTBrainState::PerformMelee
    || Controller->GetBrainState() == EWoTBrainState::PerformProjectile;
   bPrematureAttack |= bAttacking && NPC->GetHeldWeapon() != Sword;
   bSawNotifySwitch |= NPC->GetHeldWeapon() == Sword;
   bSawAttackAfterPreparation |= bSawNotifySwitch && bAttacking;
   if (bSawAttackAfterPreparation) break;
  }
  TestFalse(TEXT("no attack overwrites outgoing/incoming preparation"), bPrematureAttack);
  TestTrue(TEXT("real weapon notify eventually installs the sword"), bSawNotifySwitch);
  TestTrue(TEXT("offensive cadence resumes only after preparation"),
   bSawAttackAfterPreparation);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// WotWeapon.DownWeapon.TweenDown calls the same PrepareInventoryItem path as
// bring-up. Retail therefore replays the bow's authored BowUp preparation even
// while putting it down; that notify removes/switches it before the plain sword.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1MyrddraalBowToSword,
 "WoT.Parity.F1.Preparation.MyrddraalBowToSwordTwoPhase", F1Flags)
bool FWoTF1MyrddraalBowToSword::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Myrddraal"));
 AWoTCharacter* Player = F1Player(World, FVector(1000, 0, 100));
 AWoTNPCController* Controller = F1Controller(NPC);
 UWoTTestBrainComponent* TestBrain = NPC ? Cast<UWoTTestBrainComponent>(NPC->Brain) : nullptr;
 AWoTNPCWeapon* Bow = NPC ? NPC->GetRangedWeapon() : nullptr;
 AWoTNPCWeapon* Sword = NPC ? NPC->GetMeleeWeapon() : nullptr;
 if (TestNotNull(TEXT("Myrddraal controller"), Controller)
  && TestNotNull(TEXT("controllable navigation brain"), TestBrain)
  && TestNotNull(TEXT("Myrddraal bow"), Bow)
  && TestNotNull(TEXT("Myrddraal sword"), Sword))
 {
  TestBrain->bForceReachable = false;
  TestBrain->bForcePathable = false;
  TestTrue(TEXT("far target brings the real bow up first"), F1Until(World, 5, [&] {
   return NPC->GetHeldWeapon() == Bow
    && Controller->GetBrainState() == EWoTBrainState::Tracking; }));
  FWoTDurationNotifier& Offensive =
   NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
  Offensive.Period = 1000.0f;
  Offensive.Elapsed = 0.0f;
  Offensive.bRandomizePhase = false;
  TestBrain->bForceReachable = true;
  Player->SetActorLocation(NPC->GetActorLocation() + FVector(80, 0, 0));

  bool bSawOutgoingBowPrepHeld = false;
  bool bSawOutgoingBowPrepSwitched = false;
  for (int32 Frame = 0; Frame < 480; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   const FName Anim = NPC->GetReplicatedAnimName();
   if (Anim == TEXT("BOWUP"))
   {
    bSawOutgoingBowPrepHeld |= NPC->GetHeldWeapon() == Bow;
    bSawOutgoingBowPrepSwitched |= NPC->GetHeldWeapon() == Sword;
   }
   if (bSawOutgoingBowPrepHeld && bSawOutgoingBowPrepSwitched)
   {
    break;
   }
  }
  if (!bSawOutgoingBowPrepHeld || !bSawOutgoingBowPrepSwitched)
  {
   AddInfo(FString::Printf(TEXT("Myr transition state=%d proximity=%d anim=%s held=%s bowFlight=%d swordFlight=%d gap=%f"),
    static_cast<int32>(Controller->GetBrainState()),
    static_cast<int32>(Controller->GetTrackingProximity()),
    *NPC->GetReplicatedAnimName().ToString(),
    NPC->GetHeldWeapon() ? *NPC->GetHeldWeapon()->GetName() : TEXT("none"),
    Bow->IsInFlight() ? 1 : 0, Sword->IsInFlight() ? 1 : 0,
    FVector::Dist(NPC->GetActorLocation(), Player->GetActorLocation())));
  }
  TestTrue(TEXT("range transition prepares the held bow before switching"),
   bSawOutgoingBowPrepHeld);
  TestTrue(TEXT("outgoing bow's real notify switches to the plain sword"),
   bSawOutgoingBowPrepSwitched);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Archer's component bow has no held actor. A genuine post-shot melee commit
// and later range opening must still prepare the held sword with DRAWSWRD's
// notify before the behavior returns to the component-backed bow.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1ArcherSwordToBow,
 "WoT.Parity.F1.Preparation.ArcherSwordToComponentBow", F1Flags)
bool FWoTF1ArcherSwordToBow::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Archer"));
 AWoTCharacter* Player = F1Player(World, FVector(900, 0, 100));
 AWoTNPCController* Controller = F1Controller(NPC);
 UWoTArcherBowBehavior* BowBehavior =
  NPC ? NPC->FindComponentByClass<UWoTArcherBowBehavior>() : nullptr;
 AWoTNPCWeapon* Sword = NPC ? NPC->GetMeleeWeapon() : nullptr;
 if (TestNotNull(TEXT("Archer controller"), Controller)
  && TestNotNull(TEXT("Archer bow behavior"), BowBehavior)
  && TestNotNull(TEXT("Archer sword"), Sword))
 {
  TestTrue(TEXT("far target enters the real kneel-fire state"), F1Until(World, 4, [&] {
   return Controller->GetBrainState() == EWoTBrainState::PerformProjectile; }));
  WoTTest::TickWorld(World, 320, 1.0f / 60.0f);
  Player->SetActorLocation(NPC->GetActorLocation() + FVector(80, 0, 0));
  bool bSawPostShotDrawBeforeNotify = false;
  bool bSawPostShotSwordAfterNotify = false;
  bool bAttackedBeforeSwordReady = false;
  for (int32 Frame = 0; Frame < 360; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   if (NPC->GetReplicatedAnimName() == TEXT("DRAWSWRD"))
   {
    bSawPostShotDrawBeforeNotify |= NPC->GetHeldWeapon() != Sword;
    bSawPostShotSwordAfterNotify |= NPC->GetHeldWeapon() == Sword;
   }
   bAttackedBeforeSwordReady |= !bSawPostShotSwordAfterNotify
    && Controller->GetBrainState() == EWoTBrainState::PerformMelee;
   if (BowBehavior->IsCommittedToMelee()
    && bSawPostShotDrawBeforeNotify && bSawPostShotSwordAfterNotify)
   {
    break;
   }
  }
  TestTrue(TEXT("post-shot melee commit plays DRAWSWRD before its notify"),
   bSawPostShotDrawBeforeNotify);
  TestTrue(TEXT("post-shot DRAWSWRD notify equips the sword"),
   bSawPostShotSwordAfterNotify);
  TestFalse(TEXT("post-shot attacks wait for the sword notify"),
   bAttackedBeforeSwordReady);

  FWoTDurationNotifier& Offensive =
   NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
  Offensive.Period = 1000.0f;
  Offensive.Elapsed = 0.0f;
  Offensive.bRandomizePhase = false;
  WoTTest::TickWorld(World, 310, 1.0f / 60.0f); // pass the authored switch gate
  Player->SetActorLocation(NPC->GetActorLocation() + FVector(900, 0, 0));

  bool bSawOutgoingSwordPrepHeld = false;
  bool bSawOutgoingSwordPrepRemoved = false;
  bool bBowRearmed = false;
  for (int32 Frame = 0; Frame < 360; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   if (NPC->GetReplicatedAnimName() == TEXT("DRAWSWRD"))
   {
    bSawOutgoingSwordPrepHeld |= NPC->GetHeldWeapon() == Sword;
    bSawOutgoingSwordPrepRemoved |= NPC->GetHeldWeapon() != Sword;
   }
   bBowRearmed |= !BowBehavior->IsCommittedToMelee()
    && NPC->GetHeldWeapon() != Sword;
   if (bSawOutgoingSwordPrepHeld && bSawOutgoingSwordPrepRemoved && bBowRearmed) break;
  }
  TestTrue(TEXT("range opening prepares the outgoing sword with DRAWSWRD"),
   bSawOutgoingSwordPrepHeld);
  TestTrue(TEXT("DRAWSWRD notify removes the held sword"),
   bSawOutgoingSwordPrepRemoved);
  TestTrue(TEXT("component bow re-arms only after the put-down path"), bBowRearmed);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Grunt.InitialAcquisition: first unexpected sight stops and plays SeeEnemy, not Acquired.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1InitialAcquisition, "WoT.Parity.F1.Acquisition.FirstSightStopsAndShouts", F1Flags)
bool FWoTF1InitialAcquisition::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 F1Player(World, FVector(700, 250, 100));
 bool bSawSeeEnemy = false;
 bool bSawTurnPause = false;
 if (TestNotNull(TEXT("controller"), F1Controller(NPC)))
 {
  for (int32 Frame = 0; Frame < 60; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   if (NPC->Brain->IsGoalValid(EWoTGoalIndex::Threat))
    bSawTurnPause |= !NPC->Brain->IsNotifierEnabled(EWoTNotifierSlot::Offensive)
     && NPC->GetVelocity().IsNearlyZero(0.1f);
   for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
    bSawSeeEnemy |= It->SoundSubPath.ToString().Contains(TEXT("Tro_SeeEnemy"));
  }
  TestTrue(TEXT("sight acquired a real player"), NPC->Brain->IsGoalValid(EWoTGoalIndex::Threat));
  TestTrue(TEXT("initial acquisition disables attacks while stopped for the turn"), bSawTurnPause);
  TestTrue(TEXT("first sight emits the retail SeeEnemy voice"), bSawSeeEnemy);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1AcquisitionCadence,
 "WoT.Parity.F1.Acquisition.EntryIntervalAndAcquiredDelay", F1Flags)
bool FWoTF1AcquisitionCadence::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 F1ActivityAnchor(World, NPC->GetActorLocation());
 AWoTCharacter* First = F1Player(World, FVector(700, 400, 100));
  AWoTNPCController* Controller = F1Controller(NPC);
  UWoTTestBrainComponent* TestBrain = NPC ? Cast<UWoTTestBrainComponent>(NPC->Brain) : nullptr;
  if (TestNotNull(TEXT("controller"), Controller)
   && TestNotNull(TEXT("controllable navigation brain"), TestBrain))
 {
   // Hold the target in one folded acquired handler. PlayAcquiredSound is a
   // BeginState action: the first handler entry arms five seconds, while simply
   // remaining in that handler must never poll the deadline or bark.
   TestBrain->bForceVisible = true;
   TestBrain->bForceReachable = false;
   TestBrain->bForcePathable = false;
   FWoTDurationNotifier& Offensive =
    NPC->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
   Offensive.Period = 1000.0f;
   Offensive.Elapsed = 0.0f;
   Offensive.bRandomizePhase = false;
   // The retail Trolloc timer gives the first Acquired attempt 1% odds, then
   // switches to timed mode whether that check succeeds. Put only that slot into timed
   // mode so the spec deterministically observes the controller's five-second
   // gate rather than testing random sound-table selection.
   NPC->SoundSlotsSwitchedToTimed.Add(TEXT("Acquired"));
   TSet<TWeakObjectPtr<AWoTSoundBurst>> SeenSounds;
   int32 SeeEnemyCount = 0;
   int32 AcquiredCount = 0;
   auto ScanSounds = [&]
   {
    for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
    {
     TWeakObjectPtr<AWoTSoundBurst> Sound(*It);
     if (SeenSounds.Contains(Sound)) continue;
     SeenSounds.Add(Sound);
     const FString Path = It->SoundSubPath.ToString();
     SeeEnemyCount += Path.Contains(TEXT("Tro_SeeEnemy")) ? 1 : 0;
     if (Path.Contains(TEXT("Tro_Acquired")))
     {
      ++AcquiredCount;
     }
    }
   };
  TestTrue(TEXT("first visible enemy enters acquisition"), F1Until(World, 1, [&] {
    ScanSounds();
    return SeeEnemyCount == 1
     && Controller->GetBrainState() == EWoTBrainState::InitialAcquisition; }));
   // Once the visible InitialAcquisition component has been selected, move the
   // still-live goal into one stable nonvisible handler before ordinary Tracking
   // begins. This isolates Acquired.BeginState timing from proximity churn.
   TestBrain->bForceVisible = false;
   for (int32 Frame = 0; Frame < 390; ++Frame)
   {
    WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
    ScanSounds();
   }
  TestEqual(TEXT("stable acquired handler never polls or barks after five seconds"),
    AcquiredCount, 0);
   const int32 BeforeLaterEntry = AcquiredCount;
   First->SetActorLocation(First->GetActorLocation() + FVector(100, 0, 0));
   TestBrain->bForceVisible = false;
   TestBrain->bForcePathable = true; // Visible -> Pathable: genuine handler entry
   const bool bAcquiredBark = F1Until(World, 1, [&] {
    ScanSounds();
    return AcquiredCount > BeforeLaterEntry; });
   TestTrue(TEXT("later acquired-handler entry after five seconds may bark"),
    bAcquiredBark);

  First->Destroy();
  NPC->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
  WoTTest::TickWorld(World, 5, 1.0f / 60.0f);
  TestBrain->bForceVisible = true;
  AWoTCharacter* Second = F1Player(World, FVector(650, -100, 100));
  TestTrue(TEXT("second enemy is acquired during the cooldown"), F1Until(World, 1, [&] {
    ScanSounds();
   return NPC->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == Second; }));
   for (int32 Frame = 0; Frame < 30; ++Frame)
   {
    WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
    ScanSounds();
   }
  TestEqual(TEXT("non-reentrant sixty-second gate suppresses a second SeeEnemy"),
    SeeEnemyCount, 1);

  Second->Destroy();
  NPC->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
   for (int32 Frame = 0; Frame < 610; ++Frame)
   {
    WoTTest::TickWorld(World, 1, 0.1f);
    ScanSounds();
   }
  TestBrain->bForceVisible = true;
  AWoTCharacter* Third = F1Player(World, FVector(600, 100, 100));
  TestTrue(TEXT("visible entry is eligible again after sixty seconds"), F1Until(World, 1, [&] {
    ScanSounds();
   return NPC->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == Third
     && SeeEnemyCount == 2; }));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// InitialAcquisition is a visible Threat handler, not a raw perception callback.
// These controls enter through real hearing, damage, friendly threat transfer,
// and Captain priority rather than calling the acquisition episode directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1AcquisitionHandlerEligibility,
 "WoT.Parity.F1.Acquisition.EligibleVisibleThreatHandler", F1Flags)
bool FWoTF1AcquisitionHandlerEligibility::RunTest(const FString&)
{
 // A real audible but nonvisible enemy becomes a Threat without SeeEnemy. The
 // same live Threat then becomes visible through the ordinary tracking tick:
 // entering that visible handler must run InitialAcquisition exactly once.
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTNPCCharacter* Guard = F1Creature(World, TEXT("Trolloc"));
  Guard->AssignedTeam = 1;
  UWoTTestBrainComponent* Brain = Cast<UWoTTestBrainComponent>(Guard->Brain);
  AWoTNPCCharacter* Enemy = F1Creature(World, TEXT("Trolloc"), FVector(300, 0, 100));
  Enemy->AssignedTeam = 2;
  if (AWoTNPCController* EnemyController = F1Controller(Enemy)) EnemyController->ForceDormant();
  if (TestNotNull(TEXT("auditory visibility control"), Brain))
  {
   Brain->bForceVisible = false;
   Brain->bForceReachable = false;
   Brain->bForcePathable = false;
  }
  FWoTDurationNotifier& Offensive =
   Guard->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Offensive)];
  Offensive.Period = 1000.0f;
  Offensive.Elapsed = 0.0f;
  Offensive.bRandomizePhase = false;
  Enemy->EmitNoise(1.0f);
  TestTrue(TEXT("real nonvisible noise still installs and selects Threat"),
   F1Until(World, 1, [&] {
    return Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == Enemy
     && F1Controller(Guard)->GetBrainState() == EWoTBrainState::Tracking; }));
  TSet<TWeakObjectPtr<AWoTSoundBurst>> SeenSounds;
  int32 SeeEnemyCount = 0;
  auto ScanSeeEnemy = [&]
  {
   for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
   {
    TWeakObjectPtr<AWoTSoundBurst> Sound(*It);
    if (SeenSounds.Contains(Sound)) continue;
    SeenSounds.Add(Sound);
    SeeEnemyCount += It->SoundSubPath.ToString().Contains(TEXT("Tro_SeeEnemy")) ? 1 : 0;
   }
  };
  ScanSeeEnemy();
  TestEqual(TEXT("auditory nonvisible acquisition does not run visible handler"),
   SeeEnemyCount, 0);

  Brain->bForceVisible = true;
  // Visible cache currency is location-based. Let the same live enemy move into
  // view so ordinary goal classification refreshes the cached nonvisible answer.
  Enemy->SetActorLocation(Enemy->GetActorLocation() + FVector(0, 100, 0));
  bool bSawInitialAcquisition = false;
  F1Until(World, 2, [&]
  {
   ScanSeeEnemy();
   bSawInitialAcquisition |= F1Controller(Guard)->GetBrainState()
    == EWoTBrainState::InitialAcquisition;
   return bSawInitialAcquisition && SeeEnemyCount == 1;
  });
  TestTrue(TEXT("same auditory Threat enters InitialAcquisition when it becomes visible"),
   bSawInitialAcquisition);
  TestEqual(TEXT("same auditory Threat plays SeeEnemy on visible-handler entry"),
   SeeEnemyCount, 1);

  TestTrue(TEXT("visible acquisition returns to ordinary Tracking"),
   F1Until(World, 1, [&] {
    return F1Controller(Guard)->GetBrainState() == EWoTBrainState::Tracking; }));
  bool bTimerReenteredInitialAcquisition = false;
  for (int32 Frame = 0; Frame < 610; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 0.1f);
   ScanSeeEnemy();
   bTimerReenteredInitialAcquisition |= F1Controller(Guard)->GetBrainState()
    == EWoTBrainState::InitialAcquisition;
  }
  TestFalse(TEXT("stable visible Tracking does not timer-reenter InitialAcquisition"),
   bTimerReenteredInitialAcquisition);
  TestEqual(TEXT("stable visible Tracking does not replay SeeEnemy after sixty seconds"),
   SeeEnemyCount, 1);
  WoTTest::DestroySpecWorld(World);
 }

 // Friendly threat transfer reaches the normal visible handler and therefore
 // remains eligible for first-entry InitialAcquisition.
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTNPCCharacter* Guard = F1Creature(World, TEXT("Trolloc"));
  AWoTNPCCharacter* Friend = F1Creature(World, TEXT("Trolloc"), FVector(100, 0, 100));
  AWoTNPCCharacter* Enemy = F1Creature(World, TEXT("Trolloc"), FVector(500, 0, 100));
  Guard->AssignedTeam = Friend->AssignedTeam = 1;
  Enemy->AssignedTeam = 2;
  Friend->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Enemy);
  if (AWoTNPCController* FriendController = F1Controller(Friend)) FriendController->ForceDormant();
  if (AWoTNPCController* EnemyController = F1Controller(Enemy)) EnemyController->ForceDormant();
  Friend->EmitNoise(1.0f);
  TestTrue(TEXT("visible transferred Threat enters InitialAcquisition handler"),
   F1Until(World, 1, [&] {
    for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
     if (It->SoundSubPath.ToString().Contains(TEXT("Tro_SeeEnemy"))) return true;
    return false;
   }));
  TestEqual(TEXT("friendly transfer preserves the enemy actor"),
   Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(),
   static_cast<AActor*>(Enemy));
  WoTTest::DestroySpecWorld(World);
 }

 // A real damage-installed visible Threat reaches the same handler selection.
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTNPCCharacter* Guard = F1Creature(World, TEXT("Trolloc"));
  Guard->AssignedTeam = 1;
  AWoTCharacter* Attacker = F1Player(World, FVector(600, 0, 100), 2);
  Attacker->SetActorHiddenInGame(true); // prevent the independent sight sampler
  FDamageEvent DamageEvent;
  Guard->TakeDamage(1.0f, DamageEvent, Attacker->GetController(), Attacker);
  TestTrue(TEXT("visible damage-installed Threat enters InitialAcquisition handler"),
   F1Until(World, 1, [&] {
    for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
     if (It->SoundSubPath.ToString().Contains(TEXT("Tro_SeeEnemy"))) return true;
    return false;
   }));
  WoTTest::DestroySpecWorld(World);
 }

 // Captain sight first evaluates orders. Its valid Intermediate is priority85,
 // so raw perception must not force the lower-priority Threat/InitialAcquisition.
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTNPCCharacter* Captain = F1Creature(World, TEXT("Sister"));
  Captain->AssignedTeam = 1;
  AWoTNPCController* Controller = F1Controller(Captain);
  AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(FVector(1200, 0, 100), FRotator::ZeroRotator);
  if (TestNotNull(TEXT("captain priority controller"), Controller)
   && TestNotNull(TEXT("captain priority alarm"), Alarm))
  {
   Controller->ChangeOrders(EWoTOrders::Automatic);
   AWoTCharacter* Intruder = F1Player(World, FVector(700, 100, 100), 2);
   TestTrue(TEXT("real sight initializes the Captain threat"), F1Until(World, 1, [&] {
    return Captain->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == Intruder; }));
   TestTrue(TEXT("Captain priority selection runs after sight initialization"),
    F1Until(World, 1, [&] {
     return Captain->Brain->CurrentGoalIndex == EWoTGoalIndex::Intermediate
      && Controller->GetBrainState() == EWoTBrainState::Tracking; }));
   TestEqual(TEXT("orders keep the higher-priority alarm Intermediate"),
    Captain->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(),
    static_cast<AActor*>(Alarm));
   TestEqual(TEXT("Captain selects Intermediate instead of raw acquisition"),
    Captain->Brain->CurrentGoalIndex, EWoTGoalIndex::Intermediate);
   TestEqual(TEXT("Captain stays in normal navigation Tracking"),
    static_cast<int32>(Controller->GetBrainState()),
    static_cast<int32>(EWoTBrainState::Tracking));
  }
  WoTTest::DestroySpecWorld(World);
 }
 return !HasAnyErrors();
}

// NPC show-self visibility is an external observer callback, so it must honor
// the same state/health perception gates as the observer's own player sampler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1ExternalSightStateGates,
 "WoT.Parity.F1.Acquisition.ExternalNPCSightRespectsStateGates", F1Flags)
bool FWoTF1ExternalSightStateGates::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* Frozen = F1Creature(World, TEXT("Trolloc"), FVector(0, 0, 100));
 AWoTNPCCharacter* Dormant = F1Creature(World, TEXT("Trolloc"), FVector(0, 300, 100));
 AWoTNPCCharacter* Dead = F1Creature(World, TEXT("Trolloc"), FVector(0, -300, 100));
 Frozen->AssignedTeam = Dormant->AssignedTeam = Dead->AssignedTeam = 1;
 AWoTCharacter* IceCaster = F1Player(World, FVector(-500, 0, 100), 1);
 UWoTIceLeech* Leech = F1FreezeWithRealIce(World, IceCaster, Frozen);
 AWoTNPCController* FrozenController = F1Controller(Frozen);
 AWoTNPCController* DormantController = F1Controller(Dormant);
 AWoTNPCController* DeadController = F1Controller(Dead);
 if (TestNotNull(TEXT("real accepted Ice for external-sight gate"), Leech)
  && TestNotNull(TEXT("frozen observer controller"), FrozenController)
  && TestNotNull(TEXT("explicit Dormant observer controller"), DormantController)
  && TestNotNull(TEXT("dead observer controller"), DeadController))
 {
  DormantController->ForceDormant();
  Dead->Health = 0;
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  const FName FrozenAnim = Frozen->GetReplicatedAnimName();

  AWoTNPCCharacter* Enemy = F1Creature(World, TEXT("Trolloc"), FVector(500, 0, 100));
  Enemy->AssignedTeam = 2;
  // Use the production visibility notifier and inverted ShowSelfToNPCs route,
  // only make its cadence deterministic for this control.
  FWoTDurationNotifier& Visibility =
   Enemy->Brain->Notifiers[static_cast<int32>(EWoTNotifierSlot::Visibility)];
  Visibility.Period = 0.0f;
  Visibility.Elapsed = 0.0f;
  Visibility.bRandomizePhase = false;
  WoTTest::TickWorld(World, 15, 1.0f / 60.0f);

  TestEqual(TEXT("Ice-suspended external observer remains Dormant"),
   static_cast<int32>(FrozenController->GetBrainState()),
   static_cast<int32>(EWoTBrainState::Dormant));
  TestFalse(TEXT("Ice-suspended observer does not initialize Threat"),
   Frozen->Brain->IsGoalValid(EWoTGoalIndex::Threat));
  bool bAnyNotifierEnabled = false;
  for (const FWoTDurationNotifier& Notifier : Frozen->Brain->Notifiers)
  {
   bAnyNotifierEnabled |= Notifier.bEnabled;
  }
  TestFalse(TEXT("Ice-suspended observer keeps every notifier disabled"),
   bAnyNotifierEnabled);
  TestEqual(TEXT("external sight cannot replace the Ice-held animation"),
   Frozen->GetReplicatedAnimName(), FrozenAnim);
  TestEqual(TEXT("explicit Dormant observer remains Dormant"),
   static_cast<int32>(DormantController->GetBrainState()),
   static_cast<int32>(EWoTBrainState::Dormant));
  TestFalse(TEXT("explicit Dormant observer ignores external sight"),
   Dormant->Brain->IsGoalValid(EWoTGoalIndex::Threat));
  TestEqual(TEXT("dead observer remains Dying"),
   static_cast<int32>(DeadController->GetBrainState()),
   static_cast<int32>(EWoTBrainState::Dying));
  TestFalse(TEXT("dead observer ignores external sight"),
   Dead->Brain->IsGoalValid(EWoTGoalIndex::Threat));

  Leech->Lifespan = 0.1f;
  TestTrue(TEXT("natural thaw releases the external-sight gate"),
   F1Until(World, 0.5f, [&] {
    return Frozen->FindComponentByClass<UWoTIceLeech>() == nullptr; }));
  TestTrue(TEXT("the same real NPC visibility pulse acquires after thaw"),
   F1Until(World, 1, [&] {
    return Frozen->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get() == Enemy; }));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Grunt.WaitingCower: friendly player cylinder gap <256, ReactP -> ReactPLoop.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1Cower, "WoT.Parity.F1.Waiting.FriendlyPlayerRespect", F1Flags)
bool FWoTF1Cower::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 AWoTCharacter* Player = F1Player(World, FVector(150, 0, 100), 1);
 if (TestNotNull(TEXT("controller"), F1Controller(NPC)))
 {
  TestTrue(TEXT("friendly sight starts ReactP"), F1Until(World, 2, [&] {
   return NPC->GetReplicatedAnimName() == FName(TEXT("ReactP")); }));
  TestTrue(TEXT("respect intro finishes in ReactPLoop"), F1Until(World, 3, [&] {
   return NPC->GetReplicatedAnimName() == FName(TEXT("ReactPLoop")); }));
  Player->SetActorLocation(FVector(1200, 0, 100));
  TestTrue(TEXT("leaving 256uu cower radius restores idle"), F1Until(World, 2, [&] {
   return NPC->GetReplicatedAnimName() == FName(TEXT("Breath")); }));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1CowerBoundary,
 "WoT.Parity.F1.Waiting.RespectStrictGapAndNoRestart", F1Flags)
bool FWoTF1CowerBoundary::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 AWoTCharacter* Player = F1Player(World, FVector(1200, 0, 100), 1);
 if (TestNotNull(TEXT("controller"), F1Controller(NPC)))
 {
  const float Contact = NPC->GetSimpleCollisionRadius() + Player->GetSimpleCollisionRadius();
  Player->SetActorLocation(FVector(Contact + 255.0f, 0, 100));
  TestTrue(TEXT("gap below256 enters respect intro"), F1Until(World, 2, [&] {
   return NPC->GetReplicatedAnimName() == TEXT("ReactP"); }));
  TestTrue(TEXT("intro advances once to the loop"), F1Until(World, 3, [&] {
   return NPC->GetReplicatedAnimName() == TEXT("ReactPLoop"); }));
  WoTTest::TickWorld(World, 90, 1.0f / 60.0f);
  TestEqual(TEXT("waiting pulses do not restart ReactP"),
   NPC->GetReplicatedAnimName(), FName(TEXT("ReactPLoop")));
  Player->SetActorLocation(FVector(Contact + 256.0f, 0, 100));
  TestTrue(TEXT("exact gap256 is outside the strict retail threshold"), F1Until(World, 2, [&] {
   return NPC->GetReplicatedAnimName() != TEXT("ReactP")
    && NPC->GetReplicatedAnimName() != TEXT("ReactPLoop"); }));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1CowerNotifyAudio,
 "WoT.Parity.F1.Waiting.RespectAudioUsesIntroAndLoopNotifies", F1Flags)
bool FWoTF1CowerNotifyAudio::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 // Minion's shipped ShowRespect sound row is 1.0 odds, so every authored
 // PlayAnimSound notify is observable without changing sound-table policy.
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Minion"));
 AWoTCharacter* Player = F1Player(World, FVector(1200, 0, 100), 1);
 if (TestNotNull(TEXT("Minion controller"), F1Controller(NPC)))
 {
  TSet<TWeakObjectPtr<AWoTSoundBurst>> SeenSounds;
  int32 RespectCount = 0;
  TArray<float> RespectTimes;
  auto ScanSounds = [&]
  {
   for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
   {
    TWeakObjectPtr<AWoTSoundBurst> Sound(*It);
    if (SeenSounds.Contains(Sound)) continue;
    SeenSounds.Add(Sound);
    if (It->SoundSubPath.ToString().Contains(TEXT("Min_ShowRespect")))
    {
     ++RespectCount;
     RespectTimes.Add(World->GetTimeSeconds());
    }
   }
  };
  Player->SetActorLocation(FVector(150, 0, 100));
  TestTrue(TEXT("friendly sight enters the real Minion respect intro"),
   F1Until(World, 2, [&] { ScanSounds(); return NPC->GetReplicatedAnimName() == TEXT("ReactP"); }));
  ScanSounds();
  TestEqual(TEXT("respect voice is not played immediately on cower begin"),
   RespectCount, 0);
  TestTrue(TEXT("ReactP PlayAnimSound notify emits the 1.0-odds voice"),
   F1Until(World, 2, [&] { ScanSounds(); return RespectCount == 1; }));
  TestTrue(TEXT("respect intro advances to its authored loop"),
   F1Until(World, 3, [&] { ScanSounds(); return NPC->GetReplicatedAnimName() == TEXT("ReactPLoop"); }));
  const int32 AtLoopEntry = RespectCount;
  TestTrue(TEXT("ReactPLoop's first authored notify emits another voice"),
   F1Until(World, 3, [&] { ScanSounds(); return RespectCount > AtLoopEntry; }));
  const int32 AfterFirstLoop = RespectCount;
  TestTrue(TEXT("each subsequent ReactPLoop cycle re-fires PlayAnimSound"),
   F1Until(World, 5, [&] { ScanSounds(); return RespectCount > AfterFirstLoop; }));
  if (RespectTimes.Num() >= 3)
  {
   // Exported Minion REACTPLOOP is 16 frames at the imported 30 fps and its
   // ShowRespectLoop table row plays at 0.4: one loop is 16/(30*0.4)=4/3s.
   const float ObservedLoopPeriod = RespectTimes.Last()
    - RespectTimes[RespectTimes.Num() - 2];
   TestTrue(TEXT("multi-frame respect notify cadence uses the full loop wrap"),
    FMath::IsNearlyEqual(ObservedLoopPeriod, 4.0f / 3.0f, 0.04f));
  }
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1OneFrameCowerNotifyAudio,
 "WoT.Parity.F1.Waiting.OneFrameRespectLoopNotifyCadence", F1Flags)
bool FWoTF1OneFrameCowerNotifyAudio::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 // Archer ships a one-frame REACTPLOOP and a 1.0-odds ShowRespect sound row.
 // Enter through ordinary friendly sight so this covers the real cower episode,
 // not a direct scheduler/helper call.
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Archer"));
 AWoTCharacter* Player = F1Player(World, FVector(1200, 0, 100), 1);
 if (TestNotNull(TEXT("Archer controller"), F1Controller(NPC)))
 {
  const UWoTVertexAnimComponent* Anim = NPC
   ? Cast<UWoTVertexAnimComponent>(NPC->GetMesh()) : nullptr;
  const FWoTAnimSequence* Loop = Anim
   ? Anim->FindSequence(TEXT("ReactPLoop")) : nullptr;
  if (TestNotNull(TEXT("real Archer ReactPLoop sequence"), Loop))
  {
   TestEqual(TEXT("positive control is the shipped one-frame loop"),
    Loop->NumFrames, 1);
  }

  TSet<TWeakObjectPtr<AWoTSoundBurst>> SeenSounds;
  TArray<float> RespectTimes;
  auto ScanSounds = [&]
  {
   for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
   {
    TWeakObjectPtr<AWoTSoundBurst> Sound(*It);
    if (SeenSounds.Contains(Sound)) continue;
    SeenSounds.Add(Sound);
    if (It->SoundSubPath.ToString().Contains(TEXT("Arc_ShowRespect")))
    {
     RespectTimes.Add(World->GetTimeSeconds());
    }
   }
  };

  Player->SetActorLocation(FVector(150, 0, 100));
  TestTrue(TEXT("friendly sight enters Archer's real respect loop"),
   F1Until(World, 4, [&]
   {
    ScanSounds();
    return NPC->GetReplicatedAnimName() == TEXT("ReactPLoop");
   }));
  ScanSounds();
  const int32 AtLoopEntry = RespectTimes.Num();
  TestTrue(TEXT("one-frame ReactPLoop fires on successive authored wraps"),
   F1Until(World, 1, [&]
   {
    ScanSounds();
    return RespectTimes.Num() >= AtLoopEntry + 3;
   }));
  if (RespectTimes.Num() >= AtLoopEntry + 3)
  {
   // Exported Archer REACTPLOOP is one frame at 30 fps; its table rate is 0.4,
   // so successive PlayAnimSound crossings are 1/(30*0.4)=1/12s apart.
   const float FirstPeriod = RespectTimes[AtLoopEntry + 1]
    - RespectTimes[AtLoopEntry];
   const float SecondPeriod = RespectTimes[AtLoopEntry + 2]
    - RespectTimes[AtLoopEntry + 1];
   TestTrue(TEXT("first one-frame loop interval matches the animator wrap"),
    FMath::IsNearlyEqual(FirstPeriod, 1.0f / 12.0f, 0.025f));
   TestTrue(TEXT("one-frame loop cadence remains stable"),
    FMath::IsNearlyEqual(SecondPeriod, 1.0f / 12.0f, 0.025f));
  }
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1WarderCowerFade,
 "WoT.Parity.F1.Waiting.WarderRespectFadeCoordination", F1Flags)
bool FWoTF1WarderCowerFade::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* Warder = F1Creature(World, TEXT("Warder"));
 AWoTCharacter* Player = F1Player(World, FVector(150, 0, 100), 1);
 UWoTWarderFadeCloakBehavior* Fade = Warder
  ? Warder->FindComponentByClass<UWoTWarderFadeCloakBehavior>() : nullptr;
 if (TestNotNull(TEXT("Warder fade behavior"), Fade))
 {
  TestTrue(TEXT("respect cower disables camouflage fade-out"), F1Until(World, 2, [&] {
   return Warder->GetReplicatedAnimName() == TEXT("ReactP")
    && FMath::IsNearlyEqual(Fade->GetActiveFadeChangeTime(), Fade->FadeInChangeTime); }));
  Player->SetActorLocation(FVector(1200, 0, 100));
  TestTrue(TEXT("leaving respect re-enables waiting fade-out"), F1Until(World, 2, [&] {
   return FMath::IsNearlyEqual(Fade->GetActiveFadeChangeTime(), Fade->FadeOutChangeTime); }));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// WotGoalAbstracter radius160: real alarm ring -> Intermediate -> arrival.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1AlarmGeometry, "WoT.Parity.F1.GoalGeometry.AlarmArrivalAt160", F1Flags)
bool FWoTF1AlarmGeometry::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Trolloc"));
 AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(FVector(150, 0, 100), FRotator::ZeroRotator);
 if (TestNotNull(TEXT("controller"), F1Controller(NPC)) && TestNotNull(TEXT("alarm"), Alarm))
 {
  Alarm->ActivateAlarm(nullptr);
  TestEqual(TEXT("ring dispatch selected alarm goal"),
   NPC->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(), static_cast<AActor*>(Alarm));
  TestTrue(TEXT("normal tracking consumes alarm arrival without walking to its 22uu cylinder"),
   F1Until(World, 0.4f, [&] { return !NPC->Brain->IsGoalValid(EWoTGoalIndex::Intermediate); }));
  TestTrue(TEXT("arrival keeps the guard outside 100uu"),
   FVector::Dist2D(NPC->GetActorLocation(), Alarm->GetActorLocation()) > 100);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// WotGoalAbstracter.DetermineGoalActorParams: all four special branches and
// the ordinary-actor controls. The asymmetric team bytes prove friendliness is
// asked from the TARGET grunt, as shipped, rather than from the invoker.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1GoalGeometryBranches,
 "WoT.Parity.F1.GoalGeometry.ActorSpecificBranches", F1Flags)
bool FWoTF1GoalGeometryBranches::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* Invoker = F1Creature(World, TEXT("Trolloc"), FVector(0, 0, 100));
 if (!TestNotNull(TEXT("goal invoker"), Invoker))
 {
  WoTTest::DestroySpecWorld(World);
  return false;
 }
 FVector Origin;
 float Radius = 0, HalfHeight = 0;

 AWoTSealAltar* Altar = World->SpawnActor<AWoTSealAltar>(
  FVector(500, 0, 100), FRotator::ZeroRotator);
 Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Altar);
 TestTrue(TEXT("SealAltar goal params resolve"),
  Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
 TestEqual(TEXT("SealAltar goal radius is32"), Radius, 32.0f);
 TestEqual(TEXT("SealAltar goal half-height is32"), HalfHeight, 32.0f);
 float InvokerRadius = 0, InvokerHalfHeight = 0;
 Invoker->GetSimpleCollisionCylinder(InvokerRadius, InvokerHalfHeight);
 Altar->SetActorLocation(Invoker->GetActorLocation()
  + FVector((InvokerRadius * 32.0f / 15.0f) + 0.5f, 0, 0));
 TestFalse(TEXT("SealAltar is outside the exact32 reached boundary"),
  Invoker->Brain->IsGoalReached(EWoTGoalIndex::Threat));
 Altar->SetActorLocation(Invoker->GetActorLocation()
  + FVector((InvokerRadius * 32.0f / 15.0f) - 0.5f, 0, 0));
 TestTrue(TEXT("SealAltar is reached inside the exact32 boundary"),
  Invoker->Brain->IsGoalReached(EWoTGoalIndex::Threat));

 AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(
  FVector(700, 0, 100), FRotator::ZeroRotator);
 float AlarmNativeRadius = 0, AlarmNativeHalfHeight = 0;
 Alarm->GetSimpleCollisionCylinder(AlarmNativeRadius, AlarmNativeHalfHeight);
 Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Alarm);
 TestTrue(TEXT("Alarm goal params resolve"),
  Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
 TestEqual(TEXT("Alarm goal radius is160"), Radius, 160.0f);
 TestEqual(TEXT("Alarm keeps its native half-height"), HalfHeight, AlarmNativeHalfHeight);

 // Target Any regards an invoker on None as friendly, while the invoker regards
 // nobody as friendly. The target-side answer must win.
 Invoker->AssignedTeam = WoTTeam::None;
 AWoTNPCCharacter* Captain = F1Creature(World, TEXT("Sister"), FVector(1000, 0, 100));
 Captain->AssignedTeam = WoTTeam::Any;
 AWoTNPCController* CaptainController = F1Controller(Captain);
 if (TestNotNull(TEXT("captain target/controller"), CaptainController))
 {
  TestFalse(TEXT("invoker does not regard captain friendly"), Invoker->IsFriendly(Captain));
  TestTrue(TEXT("captain target regards invoker friendly"), Captain->IsFriendly(Invoker));
  Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Captain);
  TestTrue(TEXT("captain goal params resolve"),
   Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
  TestEqual(TEXT("Captain FindHelpRadius defaults to320"), Radius, 320.0f);

  // Captain.uc owns this editable value on the target pawn, not on its
  // transient controller. Reflection keeps this ownership assertion stable
  // across the RED state where the port still stores it on the controller.
  FFloatProperty* PawnHelpRadius = FindFProperty<FFloatProperty>(
   Captain->GetClass(), TEXT("FindHelpRadius"));
  FFloatProperty* ControllerHelpRadius = FindFProperty<FFloatProperty>(
   CaptainController->GetClass(), TEXT("FindHelpRadius"));
  TestNotNull(TEXT("FindHelpRadius is target-pawn data"), PawnHelpRadius);
  TestNull(TEXT("controller has no duplicate FindHelpRadius owner"), ControllerHelpRadius);
  FFloatProperty* ExistingHelpRadius = PawnHelpRadius ? PawnHelpRadius : ControllerHelpRadius;
  if (TestNotNull(TEXT("RED/green fixture can customize the runtime owner"), ExistingHelpRadius))
  {
   void* RadiusOwner = PawnHelpRadius
    ? static_cast<void*>(Captain) : static_cast<void*>(CaptainController);
   ExistingHelpRadius->SetPropertyValue_InContainer(RadiusOwner, 333.0f);
  }
  Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Captain);
  TestTrue(TEXT("custom captain goal params resolve while possessed"),
   Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
  TestEqual(TEXT("friendly captain uses target FindHelpRadius"), Radius, 333.0f);
  Captain->SetActorLocation(Invoker->GetActorLocation()
   + FVector(InvokerRadius * 326.5f / 15.0f, 0, 0));
  TestTrue(TEXT("possessed custom333 reaches beyond default320"),
   Invoker->Brain->IsGoalReached(EWoTGoalIndex::Threat));

  CaptainController->UnPossess();
  Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Captain);
  TestTrue(TEXT("unpossessed captain goal params resolve"),
   Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
  TestEqual(TEXT("unpossession preserves target's custom FindHelpRadius"), Radius, 333.0f);
  TestTrue(TEXT("unpossessed custom333 retains the custom-only arrival"),
   Invoker->Brain->IsGoalReached(EWoTGoalIndex::Threat));

  float CaptainNativeRadius = 0, CaptainNativeHalfHeight = 0;
  Captain->GetSimpleCollisionCylinder(CaptainNativeRadius, CaptainNativeHalfHeight);
  APlayerController* CaptainPlayerController = World->SpawnActor<APlayerController>();
  if (TestNotNull(TEXT("real player controller for Grunt bIsPlayer control"),
   CaptainPlayerController))
  {
   CaptainPlayerController->Possess(Captain);
   TestTrue(TEXT("Captain NPC body is now player-controlled"), Captain->IsPlayerControlled());
   Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Captain);
   TestTrue(TEXT("player-controlled Grunt goal params resolve"),
    Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
   TestEqual(TEXT("friendly player-controlled Grunt keeps native radius"),
    Radius, CaptainNativeRadius);
   TestEqual(TEXT("friendly player-controlled Grunt keeps native half-height"),
    HalfHeight, CaptainNativeHalfHeight);
   CaptainPlayerController->UnPossess();
   CaptainController->Possess(Captain);
  }
 }

 AWoTNPCCharacter* Grunt = F1Creature(World, TEXT("Trolloc"), FVector(1300, 0, 100));
 Grunt->AssignedTeam = WoTTeam::Any;
 float GruntRadius = 0, GruntHalfHeight = 0;
 Grunt->GetSimpleCollisionCylinder(GruntRadius, GruntHalfHeight);
 Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Grunt);
 TestTrue(TEXT("friendly grunt goal params resolve"),
  Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
 TestEqual(TEXT("friendly non-player grunt uses four times its radius"),
  Radius, GruntRadius * 4.0f);
 TestEqual(TEXT("friendly grunt keeps native half-height"), HalfHeight, GruntHalfHeight);

 // Reverse the asymmetry: the invoker's Any says friendly, but the target's
 // None says not friendly. A Captain must therefore retain its ordinary body.
 Invoker->AssignedTeam = WoTTeam::Any;
 Captain->AssignedTeam = WoTTeam::None;
 float CaptainNativeRadius = 0, CaptainNativeHalfHeight = 0;
 Captain->GetSimpleCollisionCylinder(CaptainNativeRadius, CaptainNativeHalfHeight);
 TestTrue(TEXT("invoker regards hostile-side captain friendly"), Invoker->IsFriendly(Captain));
 TestFalse(TEXT("captain target rejects invoker"), Captain->IsFriendly(Invoker));
 Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Captain);
 TestTrue(TEXT("hostile captain goal params resolve"),
  Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
 TestEqual(TEXT("target-side hostile captain keeps native radius"), Radius, CaptainNativeRadius);
 TestEqual(TEXT("target-side hostile captain keeps native half-height"),
  HalfHeight, CaptainNativeHalfHeight);

 AWoTCharacter* Player = F1Player(World, FVector(1600, 0, 100), 1);
 float PlayerRadius = 0, PlayerHalfHeight = 0;
 Player->GetSimpleCollisionCylinder(PlayerRadius, PlayerHalfHeight);
 Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Player);
 TestTrue(TEXT("player goal params resolve"),
  Invoker->Brain->GetGoalCylinder(EWoTGoalIndex::Threat, Origin, Radius, HalfHeight));
 TestEqual(TEXT("player target keeps native radius"), Radius, PlayerRadius);
 TestEqual(TEXT("player target keeps native half-height"), HalfHeight, PlayerHalfHeight);

 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// The enlarged friendly-NPC cylinders must reach the normal Intermediate
// arrival dispatch; this is the player-visible Captain/help timing, not merely
// a helper-property assertion.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1FriendlyGoalGeometryArrival,
 "WoT.Parity.F1.GoalGeometry.FriendlyNPCArrivals", F1Flags)
bool FWoTF1FriendlyGoalGeometryArrival::RunTest(const FString&)
{
 // Friendly Captain: 250uu is outside ordinary 16x16 contact but inside the
 // default 16x320 FindHelpRadius threshold.
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* Invoker = F1Creature(World, TEXT("Trolloc"), FVector(0, 0, 100));
 AWoTNPCCharacter* Captain = F1Creature(World, TEXT("Sister"), FVector(250, 0, 100));
 Invoker->AssignedTeam = WoTTeam::None;
 Captain->AssignedTeam = WoTTeam::Any;
 Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Intermediate, Captain);
 TestTrue(TEXT("friendly captain is reached at FindHelp distance"),
  Invoker->Brain->IsGoalReached(EWoTGoalIndex::Intermediate));
 TestTrue(TEXT("normal controller consumes friendly-captain arrival"),
  F1Until(World, 0.6f, [&]
  {
   return !Invoker->Brain->IsGoalValid(EWoTGoalIndex::Intermediate);
  }));
 WoTTest::DestroySpecWorld(World);

 // Friendly grunt: 60uu is outside ordinary contact but inside the target's
 // four-radius engagement threshold.
 World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 Invoker = F1Creature(World, TEXT("Trolloc"), FVector(0, 0, 100));
 AWoTNPCCharacter* Grunt = F1Creature(World, TEXT("Trolloc"), FVector(60, 0, 100));
 Invoker->AssignedTeam = 1;
 Grunt->AssignedTeam = 1;
 Invoker->Brain->InitGoalWithActor(EWoTGoalIndex::Intermediate, Grunt);
 TestTrue(TEXT("friendly grunt is reached at four-radius distance"),
  Invoker->Brain->IsGoalReached(EWoTGoalIndex::Intermediate));
 TestTrue(TEXT("normal controller consumes friendly-grunt arrival"),
  F1Until(World, 0.6f, [&]
  {
   return !Invoker->Brain->IsGoalValid(EWoTGoalIndex::Intermediate);
  }));
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// AngrealUser.AddDefaultInventoryItems applies Sister group5..8 to group, stock20..70.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1CasterDefaults, "WoT.Parity.F1.Caster.SisterStockAndGroup", F1Flags)
bool FWoTF1CasterDefaults::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Sister"));
 AWoTAngrealInvDart* Dart = NPC ? Cast<AWoTAngrealInvDart>(
  NPC->InventoryComponent->FindInventoryType(AWoTAngrealInvDart::StaticClass())) : nullptr;
 if (TestNotNull(TEXT("spawn-time real inventory Dart"), Dart))
 {
  TestEqual(TEXT("Dart class minimum stock remains20"), Dart->MinInitialCharges, 20);
  TestEqual(TEXT("Dart class maximum stock remains70"), Dart->MaxInitialCharges, 70);
  TestEqual(TEXT("minimum group is Sister's5"), Dart->MinChargesInGroup, 5);
  TestEqual(TEXT("maximum group is Sister's8"), Dart->MaxChargesInGroup, 8);
  TestTrue(TEXT("stock remains Dart20..70 rather than volley5..8"), Dart->CurCharges >= 20 && Dart->CurCharges <= 70);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1DefensiveInterval, "WoT.Parity.F1.Caster.AuthoredDefensiveInterval", F1Flags)
bool FWoTF1DefensiveInterval::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 for (const FName Row : {FName(TEXT("Sister")), FName(TEXT("Sitter"))})
 {
  AWoTNPCCharacter* NPC = F1Creature(World, Row);
  UWoTCasterComponent* Caster = NPC ? NPC->FindComponentByClass<UWoTCasterComponent>() : nullptr;
  if (TestNotNull(*Row.ToString(), Caster))
   TestEqual(TEXT("spawn/ConfigureNPC copies retail defensive2s gate"), Caster->MinDefensiveUsageInterval, 2.0f);
 }

 // Drive the real Sister inventory selection/use/callback chain. A stationary
 // projectile is sufficient here: SelectDefensiveWeave consumes the same
 // incoming AngrealProjectile victim that the live 0.05s detector supplies.
 AWoTNPCCharacter* Sister = F1Creature(World, TEXT("Sister"), FVector(1000, 0, 100));
 UWoTCasterComponent* Caster = Sister ? Sister->FindComponentByClass<UWoTCasterComponent>() : nullptr;
 AWoTAngrealInvShield* Shield = Sister ? Cast<AWoTAngrealInvShield>(
  Sister->InventoryComponent->FindInventoryType(AWoTAngrealInvShield::StaticClass())) : nullptr;
 AWoTAngrealInvAirBurst* Source = World->SpawnActor<AWoTAngrealInvAirBurst>(
  FVector(9000, 9000, 9000), FRotator::ZeroRotator);
 AWoTTestSeeker* Offender = World->SpawnActor<AWoTTestSeeker>(
  Sister ? Sister->GetActorLocation() + FVector(100, 0, 0) : FVector::ZeroVector,
  FRotator::ZeroRotator);
 if (TestNotNull(TEXT("real Sister caster"), Caster)
  && TestNotNull(TEXT("real Sister personal shield"), Shield)
  && TestNotNull(TEXT("defensive source"), Source)
  && TestNotNull(TEXT("incoming defensive victim"), Offender))
 {
  Offender->SetSourceAngreal(Source);
  Offender->ProjectileMovement->StopMovementImmediately();
  Offender->ProjectileMovement->Deactivate();
  // Isolate the adapter's two-second hand gate from this weave's independent
  // four-second group gate; the live selection/use/callback path remains intact.
  Shield->MinChargeGroupInterval = 0.0f;
  Shield->RoundsPerMinute = 6000.0f;
  WoTTest::TickWorld(World, 0.1f);
  AWoTAngrealInventory* First = Caster->SelectDefensiveWeave(Offender);
  TestEqual(TEXT("first normal defensive selection chooses the personal shield"),
   First, static_cast<AWoTAngrealInventory*>(Shield));
  const int32 ChargesBefore = Shield->CurCharges;
  Caster->BeginCast(First);
  TestTrue(TEXT("normal BeginCast spends a defensive charge"),
   F1Until(World, 0.5f, [&] { return Shield->CurCharges < ChargesBefore; }));
  WoTTest::TickWorld(World, 2, 1.0f / 60.0f); // let ChargeUsed stamp the hand
  Caster->Release();
  TestNull(TEXT("a second defensive selection is gated immediately"),
   Caster->SelectDefensiveWeave(Offender));
  WoTTest::TickWorld(World, 1.8f);
  TestNull(TEXT("the authored gate remains closed before two seconds"),
   Caster->SelectDefensiveWeave(Offender));
  WoTTest::TickWorld(World, 0.3f);
  TestEqual(TEXT("a later normal selection opens after the authored interval"),
   Caster->SelectDefensiveWeave(Offender), static_cast<AWoTAngrealInventory*>(Shield));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Real Dart launcher: source aim must lead target velocity and reverse each tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1LeadAim, "WoT.Parity.F1.Caster.ProjectileLeadingAndVolleyReaim", F1Flags)
bool FWoTF1LeadAim::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTTestCasterNPC* NPC = F1VolleyNPC(World);
 AWoTCharacter* Player = F1Player(World, FVector(1000, 0, 100));
 Player->GetCharacterMovement()->BrakingDecelerationFlying = 0;
 Player->GetCharacterMovement()->Velocity = FVector(0, 250, 0);
 AWoTNPCController* Controller = F1Controller(NPC);
 AWoTAngrealInvDart* Dart = NPC ? Cast<AWoTAngrealInvDart>(
  NPC->InventoryComponent->FindInventoryType(AWoTAngrealInvDart::StaticClass())) : nullptr;
 if (TestNotNull(TEXT("caster controller"), Controller))
 {
  TestTrue(TEXT("normal controller selects/casts the inventory weave"), F1Until(World, 3, [&] { return NPC->Caster->IsCasting(); }));
  bool bSawProjectile = false;
  bool bProjectileLed = false;
  for (int32 Frame = 0; Frame < 180 && !bSawProjectile; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
   {
    if (It->GetInstigator() != NPC) continue;
    bSawProjectile = true;
    const FVector ToTarget = Player->GetActorLocation() - It->GetActorLocation();
    bProjectileLed |= It->GetVelocity().GetSafeNormal().Y > ToTarget.GetSafeNormal().Y + 0.05f;
   }
  }
  TestTrue(TEXT("actual Dart projectile observed"), bSawProjectile);
  TestTrue(TEXT("launched velocity leads the moving player"), bProjectileLed);
  TSet<TWeakObjectPtr<AWoTDartProjectile>> PreReversalDarts;
  for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
   if (It->GetInstigator() == NPC) PreReversalDarts.Add(*It);
  const int32 UsesBeforeReversal = Dart ? Dart->CastUseCount : INDEX_NONE;
  Player->GetCharacterMovement()->Velocity = FVector(0, -250, 0);
  bool bSawLaterDart = false;
  bool bLaterDartUsesReversedLead = false;
  for (int32 Frame = 0; Frame < 60 && !bLaterDartUsesReversedLead; ++Frame)
  {
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
   {
    if (It->GetInstigator() != NPC || PreReversalDarts.Contains(*It)) continue;
    bSawLaterDart = true;
    const FVector Direct = (Player->GetActorLocation() - It->GetActorLocation()).GetSafeNormal();
    bLaterDartUsesReversedLead |= It->GetVelocity().GetSafeNormal().Y < Direct.Y - 0.05f;
   }
  }
  TestTrue(TEXT("velocity reversal produces an actual later Dart"), bSawLaterDart);
  TestTrue(TEXT("later Dart trajectory uses the reversed target lead"), bLaterDartUsesReversedLead);
  TestTrue(TEXT("later Dart came from a later real charge use"),
   Dart && Dart->CastUseCount > UsesBeforeReversal);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Leading belongs only to an NPC adapter with a leadable projectile. A seeker
// retains direct aim and a player's real inventory cast retains their authored view.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1LeadAimControls,
 "WoT.Parity.F1.Caster.ProjectileLeadingControls", F1Flags)
bool FWoTF1LeadAimControls::RunTest(const FString&)
{
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTTestCasterNPC* NPC = F1VolleyNPC(World, AWoTLegionInvSeeker::StaticClass());
  AWoTCharacter* Player = F1Player(World, FVector(1000, 0, 100));
  Player->GetCharacterMovement()->Velocity = FVector(0, 300, 0);
  AWoTNPCController* Controller = F1Controller(NPC);
  bool bSawSeeker = false;
  if (TestNotNull(TEXT("non-leadable caster controller"), Controller))
  {
   bSawSeeker = F1Until(World, 4.0f, [&]
   {
    for (TActorIterator<AWoTLegionProjectile> It(World); It; ++It)
     if (It->GetInstigator() == NPC) return true;
    return false;
   });
   const FRotator Direct = (Player->GetActorLocation() - NPC->GetActorLocation()).Rotation();
   TestTrue(TEXT("real non-leadable seeker launched"), bSawSeeker);
   TestTrue(TEXT("non-leadable NPC keeps direct target aim"),
    FMath::Abs(FMath::FindDeltaAngleDegrees(
     Direct.Yaw, Controller->GetControlRotation().Yaw)) < 3.0f);
  }
  WoTTest::DestroySpecWorld(World);
 }

 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTCharacter* PlayerCaster = F1Player(World, FVector(0, 0, 100), 1);
  AWoTCharacter* MovingTarget = F1Player(World, FVector(1000, 500, 100), 2);
  MovingTarget->GetCharacterMovement()->Velocity = FVector(0, 400, 0);
  AController* PC = PlayerCaster ? PlayerCaster->GetController() : nullptr;
  const FRotator AuthoredAim(0, 12, 0);
  if (PC) PC->SetControlRotation(AuthoredAim);
  const FTransform At(PlayerCaster ? PlayerCaster->GetActorLocation() : FVector::ZeroVector);
  AWoTAngrealInvDart* Dart = World->SpawnActorDeferred<AWoTAngrealInvDart>(
   AWoTAngrealInvDart::StaticClass(), At, PlayerCaster, PlayerCaster,
   ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
  if (TestNotNull(TEXT("player Dart inventory"), Dart) && TestNotNull(TEXT("player caster"), PlayerCaster))
  {
   Dart->bHeldItem = true;
   Dart->FinishSpawning(At);
   Dart->CurCharges = 20;
   Dart->GiveTo(PlayerCaster);
   Dart->SetVictim(MovingTarget);
   PlayerCaster->InventoryComponent->SelectItem(Dart);
   // DefaultCastReflector's strict Level.TimeSeconds > NextFireTime gate rejects
   // a cast on the exact world-zero frame.
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   PlayerCaster->InventoryComponent->UseAngreal();
   AWoTDartProjectile* Shot = nullptr;
   F1Until(World, 0.5f, [&]
   {
    for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
     if (It->GetInstigator() == PlayerCaster) { Shot = *It; return true; }
    return false;
   });
   if (TestNotNull(TEXT("player launched a real Dart"), Shot))
   {
    TestTrue(TEXT("player projectile follows authored view rather than adapter lead"),
     FMath::Abs(FMath::FindDeltaAngleDegrees(
      AuthoredAim.Yaw, Shot->GetVelocity().Rotation().Yaw)) < 2.0f);
   }
   TestTrue(TEXT("player control rotation is not rewritten by NPC lead support"),
    PC && PC->GetControlRotation().Equals(AuthoredAim, 0.1f));
  }
  WoTTest::DestroySpecWorld(World);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1RejectedCastTransactional,
 "WoT.Parity.F1.Caster.RejectedCastIsTransactional", F1Flags)
bool FWoTF1RejectedCastTransactional::RunTest(const FString&)
{
 // The strict DefaultCastReflector time gate rejects this world-zero request.
 // BeginCast must not expose an active adapter or AttackRun when no use started.
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTTestCasterNPC* NPC = F1VolleyNPC(World);
 AWoTCharacter* Target = F1Player(World, FVector(1000, 0, 100));
 AWoTAngrealInvDart* Dart = NPC ? Cast<AWoTAngrealInvDart>(
  NPC->InventoryComponent->FindInventoryType(AWoTAngrealInvDart::StaticClass())) : nullptr;
 if (TestNotNull(TEXT("world-zero Dart"), Dart))
 {
  Dart->SetVictim(Target);
  Dart->InitChargeGroup();
  NPC->InventoryComponent->SelectItem(Dart);
  NPC->Caster->BeginCast(Dart);
  TestFalse(TEXT("time-rejected use leaves no active caster adapter"), NPC->Caster->IsCasting());
  TestEqual(TEXT("time-rejected use spends no real charge"), Dart->CastUseCount, 0);
  TestTrue(TEXT("time-rejected use never owns AttackRun"),
   NPC->GetReplicatedAnimName() != FName(TEXT("ATTACKRUN")));
 }
 WoTTest::DestroySpecWorld(World);

 // NoCast is the real highest-priority cast interceptor. Repeated normal AI
 // attack attempts must stay rejected without latching a phantom volley.
 World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 NPC = F1VolleyNPC(World);
 UWoTNoCastReflector* NoCast = NewObject<UWoTNoCastReflector>(NPC);
 NoCast->Install(NPC);
 F1Player(World, FVector(350, 0, 100));
 Dart = Cast<AWoTAngrealInvDart>(
  NPC->InventoryComponent->FindInventoryType(AWoTAngrealInvDart::StaticClass()));
 WoTTest::TickWorld(World, 4.0f);
 int32 FiredDarts = 0;
 for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
  FiredDarts += It->GetInstigator() == NPC ? 1 : 0;
 TestFalse(TEXT("NoCast-rejected NPC never latches CasterUse"), NPC->Caster->IsCasting());
 TestTrue(TEXT("NoCast-rejected NPC never owns AttackRun"),
  NPC->GetReplicatedAnimName() != FName(TEXT("ATTACKRUN")));
 TestEqual(TEXT("NoCast-rejected NPC spends no Dart charge"), Dart ? Dart->CastUseCount : -1, 0);
 TestEqual(TEXT("NoCast-rejected NPC launches no Dart"), FiredDarts, 0);
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1CastingAnim, "WoT.Parity.F1.Caster.CastingAttackRun", F1Flags)
bool FWoTF1CastingAnim::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* NPC = F1Creature(World, TEXT("Sister"));
 F1Player(World, FVector(350, 0, 100));
 UWoTCasterComponent* Caster = NPC ? NPC->FindComponentByClass<UWoTCasterComponent>() : nullptr;
 if (TestNotNull(TEXT("caster"), Caster))
 {
  TestTrue(TEXT("real controller starts a volley"), F1Until(World, 4, [&] { return Caster->IsCasting(); }));
  TestTrue(TEXT("active weave animates ATTACKRUN instead of Breath"), F1Until(World, 1, [&] {
   return Caster->IsCasting() && NPC->GetReplicatedAnimName() == FName(TEXT("ATTACKRUN")); }));
  Caster->Release();
  TestTrue(TEXT("ending the continuous cast releases AttackRun back to locomotion"),
   F1Until(World, 0.5f, [&] {
    return NPC->GetReplicatedAnimName() != FName(TEXT("ATTACKRUN")); }));
 }
 WoTTest::DestroySpecWorld(World);

 World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTTestCasterNPC* FireCaster = F1VolleyNPC(World, AWoTAngrealInvFireball::StaticClass());
 F1Player(World, FVector(350, 0, 100));
 bool bSawFireballProjectile = false;
 bool bSawChargeStamp = false;
 for (int32 Frame = 0; Frame < 240 && !(bSawFireballProjectile && bSawChargeStamp); ++Frame)
 {
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  for (TActorIterator<AWoTFireballProjectile> It(World); It; ++It)
   bSawFireballProjectile |= It->GetInstigator() == FireCaster;
  bSawChargeStamp |= FireCaster
   && FireCaster->GetReplicatedAnimName() == FName(TEXT("ATTACKRUN"));
 }
 TestTrue(TEXT("real Fireball cast launched its production projectile"), bSawFireballProjectile);
 TestTrue(TEXT("Fireball charge callback stamps ATTACKRUN once"), bSawChargeStamp);
 TestTrue(TEXT("non-continuous Fireball does not retain the AttackRun loop"),
  F1Until(World, 0.5f, [&] {
   return FireCaster && !FireCaster->Caster->IsCasting()
    && FireCaster->GetReplicatedAnimName() != FName(TEXT("ATTACKRUN")); }));
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1IceReflectInterrupt,
 "WoT.Parity.F1.Caster.IceContactInterruptsBeforeReflectors", F1Flags)
bool FWoTF1IceReflectInterrupt::RunTest(const FString&)
{
 auto RunContact = [this](bool bAbsorb)
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTTestCasterNPC* NPC = F1VolleyNPC(World);
  AWoTCharacter* Player = F1Player(World, FVector(180, 0, 100));
  Player->GetController()->SetControlRotation(FRotator(0, 180, 0));
  TestTrue(bAbsorb ? TEXT("absorbed case starts a live Dart volley")
                   : TEXT("reflected case starts a live Dart volley"),
   F1Until(World, 4.0f, [&] { return NPC->Caster->IsCasting(); }));
  if (bAbsorb)
  {
   UWoTAbsorbReflector* Absorb = NewObject<UWoTAbsorbReflector>(NPC);
   Absorb->Install(NPC);
  }
  else
  {
   UWoTReturnToSenderReflector* Reflect = NewObject<UWoTReturnToSenderReflector>(NPC);
   Reflect->Install(NPC);
  }
  const FTransform At(Player->GetActorLocation());
  AWoTAngrealInvIce* Ice = World->SpawnActorDeferred<AWoTAngrealInvIce>(
   AWoTAngrealInvIce::StaticClass(), At, Player, Player,
   ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
  if (Ice)
  {
   Ice->bHeldItem = true; Ice->FinishSpawning(At); Ice->CurCharges = 10;
   Ice->GiveTo(Player); Player->InventoryComponent->SelectItem(Ice);
   Player->InventoryComponent->UseAngreal();
  }
  bool bReturnedIce = false;
  const bool bReleased = F1Until(World, 1.2f, [&]
  {
   for (TActorIterator<AWoTIceProjectile> It(World); It; ++It)
    bReturnedIce |= It->GetInstigator() == NPC;
   return !NPC->Caster->IsCasting();
  });
  TestTrue(bAbsorb ? TEXT("absorbed Ice contact releases the charge group")
                   : TEXT("reflected Ice contact releases the charge group"), bReleased);
  TestNull(bAbsorb ? TEXT("absorbed Ice installs no leech")
                   : TEXT("reflected Ice installs no leech"),
   NPC->FindComponentByClass<UWoTIceLeech>());
  if (!bAbsorb)
   TestTrue(TEXT("real reflector returned the Ice projectile"), bReturnedIce);
  WoTTest::DestroySpecWorld(World);
 };
 RunContact(false);
 RunContact(true);
 return !HasAnyErrors();
}

// AngrealUser.NotifyHitByAngrealProjectile + IceLeech.FreezeHost, real player cast/flight.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1IceInterrupt, "WoT.Parity.F1.Caster.IceImpactInterruptsVolley", F1Flags)
bool FWoTF1IceInterrupt::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTTestCasterNPC* NPC = F1VolleyNPC(World);
 // At180uu, the300uu/s Ice reaches this stationary target before Dart's
 // minimum five-shot group can finish (four320RPM intervals =0.75s).
 AWoTCharacter* Player = F1Player(World, FVector(180, 0, 100));
 Player->GetController()->SetControlRotation(FRotator(0, 180, 0));
 NPC->SetActorRotation(FRotator::ZeroRotator);
 TestTrue(TEXT("controller has an active volley before Ice"), F1Until(World, 4, [&] { return NPC->Caster->IsCasting(); }));
 const FTransform Transform(Player->GetActorLocation());
 AWoTAngrealInvIce* Ice = World->SpawnActorDeferred<AWoTAngrealInvIce>(
  AWoTAngrealInvIce::StaticClass(), Transform, Player, Player, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
 Ice->bHeldItem = true;
 Ice->FinishSpawning(Transform);
 Ice->CurCharges = 10;
 Ice->GiveTo(Player);
 Player->InventoryComponent->SelectItem(Ice);
 Player->InventoryComponent->UseAngreal();
 const float FlightAllowance = FVector::Dist(Player->GetActorLocation(), NPC->GetActorLocation()) / 300.0f + 0.25f;
 bool bIceContact = false;
 bool bVolleyContinuousUntilImpact = true;
 bool bCastingImmediatelyBeforeImpact = false;
 for (int32 Frame = 0; Frame < FMath::CeilToInt(FlightAllowance * 60); ++Frame)
 {
  const bool bCastingBeforeTick = NPC->Caster->IsCasting();
  bVolleyContinuousUntilImpact &= bCastingBeforeTick;
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  if (NPC->FindComponentByClass<UWoTIceLeech>())
  {
   bIceContact = true;
   bCastingImmediatelyBeforeImpact = bCastingBeforeTick;
   break;
  }
 }
 TestTrue(TEXT("real Ice flight installs freeze on the NPC within the speed-based allowance"), bIceContact);
 TestTrue(TEXT("Dart volley remains active continuously until real Ice impact"), bVolleyContinuousUntilImpact);
 TestTrue(TEXT("caster is actively casting immediately before the impact tick"), bCastingImmediatelyBeforeImpact);
 TestFalse(TEXT("Ice impact releases the active charge group"), NPC->Caster->IsCasting());
 AWoTNPCController* Controller = F1Controller(NPC);
 TestTrue(TEXT("accepted Ice suspends the active AI episode in the dormant shell"),
  Controller && Controller->GetBrainState() == EWoTBrainState::Dormant);
 bool bAnyNotifier = false;
 for (const FWoTDurationNotifier& Notifier : NPC->Brain->Notifiers)
  bAnyNotifier |= Notifier.bEnabled;
 TestFalse(TEXT("accepted Ice disables every NPC notifier"), bAnyNotifier);
 int32 CastsAtFreeze = 0;
 for (AWoTInventory* Item : NPC->InventoryComponent->Items)
  if (AWoTAngrealInventory* Weave = Cast<AWoTAngrealInventory>(Item)) CastsAtFreeze += Weave->CastUseCount;
 WoTTest::TickWorld(World, 1.0f);
 int32 CastsAfter = 0;
 for (AWoTInventory* Item : NPC->InventoryComponent->Items)
  if (AWoTAngrealInventory* Weave = Cast<AWoTAngrealInventory>(Item)) CastsAfter += Weave->CastUseCount;
 TestEqual(TEXT("frozen NPC spends no further shots"), CastsAfter, CastsAtFreeze);
 TestFalse(TEXT("frozen NPC cannot restart its volley"), NPC->Caster->IsCasting());
 if (UWoTIceLeech* Leech = NPC->FindComponentByClass<UWoTIceLeech>())
  Leech->Lifespan = 0.1f;
 TestTrue(TEXT("natural leech expiry runs the thaw path"), F1Until(World, 0.5f, [&] {
  return NPC->FindComponentByClass<UWoTIceLeech>() == nullptr; }));
 TestTrue(TEXT("thaw reselects an active NPC goal instead of resuming stale cast state"),
  F1Until(World, 1.0f, [&] {
   return Controller && Controller->GetBrainState() != EWoTBrainState::Dormant; }));
 WoTTest::DestroySpecWorld(World);

 // An NPC already in explicit Dormant stays Dormant after a real
 // freeze/thaw instead of being woken by IceLeech's goal-priority transition.
 World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 NPC = F1VolleyNPC(World);
 Controller = F1Controller(NPC);
 Player = F1Player(World, FVector(180, 0, 100));
 Player->GetController()->SetControlRotation(FRotator(0, 180, 0));
 if (Controller) Controller->ForceDormant();
 const FTransform DormantAt(Player->GetActorLocation());
 AWoTAngrealInvIce* DormantIce = World->SpawnActorDeferred<AWoTAngrealInvIce>(
  AWoTAngrealInvIce::StaticClass(), DormantAt, Player, Player,
  ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
 if (DormantIce)
 {
  DormantIce->bHeldItem = true; DormantIce->FinishSpawning(DormantAt);
  DormantIce->CurCharges = 10; DormantIce->GiveTo(Player);
  Player->InventoryComponent->SelectItem(DormantIce);
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  Player->InventoryComponent->UseAngreal();
 }
 UWoTIceLeech* DormantLeech = nullptr;
 TestTrue(TEXT("real Ice flight freezes the already-dormant NPC"),
  F1Until(World, 1.2f, [&] {
   DormantLeech = NPC->FindComponentByClass<UWoTIceLeech>();
   return DormantLeech != nullptr; }));
 if (DormantLeech) DormantLeech->Lifespan = 0.1f;
 F1Until(World, 0.5f, [&] {
  return NPC->FindComponentByClass<UWoTIceLeech>() == nullptr; });
 TestTrue(TEXT("thaw does not wake a preexisting explicit Dormant pawn"),
  Controller && Controller->GetBrainState() == EWoTBrainState::Dormant);
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1IceSwapTransfer,
 "WoT.Parity.F1.Caster.IceSwapTransferLifecycle", F1Flags)
bool FWoTF1IceSwapTransfer::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTTestCasterNPC* Frozen = F1VolleyNPC(World);
 // Equal-size endpoints bypass SwapPlaces' intentionally unrelated asymmetric
 // fit gate; this spec isolates the accepted-leech transfer lifecycle.
 AWoTTestCasterNPC* Replacement = F1VolleyNPC(World);
 Replacement->SetActorLocation(FVector(600, 0, 100));
 AWoTCharacter* Player = F1Player(World, FVector(180, 0, 100));
 UWoTIceLeech* Ice = F1FreezeWithRealIce(World, Player, Frozen);
 AWoTNPCController* OldController = F1Controller(Frozen);
 AWoTNPCController* NewController = F1Controller(Replacement);
 if (TestNotNull(TEXT("real accepted Ice leech before swap"), Ice))
 {
  TestTrue(TEXT("old host begins suspended"),
   OldController && OldController->IsSuspendedForIceFreeze());
  TestEqual(TEXT("old host begins movement-disabled"),
   (int32)Frozen->GetCharacterMovement()->MovementMode.GetValue(), (int32)MOVE_None);

  FActorSpawnParameters Params;
  Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  AWoTSwapPlacesProjectile* Swap = World->SpawnActor<AWoTSwapPlacesProjectile>(
   AWoTSwapPlacesProjectile::StaticClass(), FVector(300, 0, 300),
   FRotator::ZeroRotator, Params);
  Swap->PerformSwap(Frozen, Replacement);

  TestEqual(TEXT("real Swap Places moves the same Ice leech to the replacement"),
   Ice->GetOwner(), static_cast<AActor*>(Replacement));
  TestTrue(TEXT("old host controller is released during transfer"),
   OldController && !OldController->IsSuspendedForIceFreeze());
  TestTrue(TEXT("old host movement is restored during transfer"),
   Frozen->GetCharacterMovement()->MovementMode != MOVE_None);
  TestFalse(TEXT("old host animation ownership is restored during transfer"),
   Frozen->IsNPCAnimationSuspendedForIce());
  TestTrue(TEXT("replacement controller owns the transferred freeze"),
   NewController && NewController->IsSuspendedForIceFreeze()
    && NewController->GetBrainState() == EWoTBrainState::Dormant);
  TestEqual(TEXT("replacement movement is disabled"),
   (int32)Replacement->GetCharacterMovement()->MovementMode.GetValue(), (int32)MOVE_None);
  TestTrue(TEXT("transferred shell is rehomed to the replacement"),
   IsValid(Ice->FrozenShell.Get()) && Ice->FrozenShell->FollowActor.Get() == Replacement);
  TestTrue(TEXT("transferred collision block is rehomed to the replacement"),
   IsValid(Ice->IceBlock.Get()) && Ice->IceBlock->GetAttachParentActor() == Replacement);

  Ice->Lifespan = 0.1f;
  TestTrue(TEXT("transferred Ice naturally thaws on its new host"), F1Until(World, 0.5f, [&]
  {
   return Replacement->FindComponentByClass<UWoTIceLeech>() == nullptr;
  }));
  TestTrue(TEXT("new host movement is restored on transferred thaw"),
   Replacement->GetCharacterMovement()->MovementMode != MOVE_None);
  TestTrue(TEXT("new host reselects a goal on transferred thaw"), F1Until(World, 1.0f, [&]
  {
   return NewController && NewController->GetBrainState() != EWoTBrainState::Dormant;
  }));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1FrozenDamageSuppression,
 "WoT.Parity.F1.Caster.IceSuppressesHitDetoursButNotDeath", F1Flags)
bool FWoTF1FrozenDamageSuppression::RunTest(const FString&)
{
 auto ExplodeFireball = [](UWorld* World, AWoTCharacter* Shooter, AWoTNPCCharacter* Victim)
 {
  FActorSpawnParameters Params;
  Params.Owner = Shooter;
  Params.Instigator = Shooter;
  Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  const FVector At = Victim->GetActorLocation() + FVector(0, 0, 100);
  AWoTFireballProjectile* Fireball = World->SpawnActor<AWoTFireballProjectile>(
   AWoTFireballProjectile::StaticClass(), At, FRotator::ZeroRotator, Params);
  if (Fireball) Fireball->Explode(At, FVector::UpVector);
 };

 // A real accepted freeze must survive ordinary splash damage without letting
 // TakeHit steal the Dormant state or frozen animation channel.
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTTestCasterNPC* NPC = F1VolleyNPC(World);
 AWoTCharacter* Player = F1Player(World, FVector(180, 0, 100));
 UWoTIceLeech* Ice = F1FreezeWithRealIce(World, Player, NPC);
 if (TestNotNull(TEXT("accepted Ice for surviving damage"), Ice))
 {
  NPC->Health = 200;
  NPC->ChanceOfHitAnim = 1.0f;
  const int32 HealthBefore = NPC->Health;
  const FName FrozenSequence = NPC->RepAnim.Sequence;
  const uint8 FrozenToken = NPC->RepAnim.PlayToken;
  ExplodeFireball(World, Player, NPC);
  TestTrue(TEXT("real Fireball splash still damages the frozen NPC"), NPC->Health < HealthBefore);
  TestTrue(TEXT("surviving splash cannot leave the frozen Dormant shell"),
   F1Controller(NPC) && F1Controller(NPC)->GetBrainState() == EWoTBrainState::Dormant);
  TestEqual(TEXT("surviving splash cannot replace the frozen sequence"),
   NPC->RepAnim.Sequence, FrozenSequence);
  TestEqual(TEXT("surviving splash cannot stamp a hit animation token"),
   NPC->RepAnim.PlayToken, FrozenToken);
 }
 WoTTest::DestroySpecWorld(World);

 // The suppression is only for nonfatal hit detours: lethal splash still runs
 // the ordinary death path and removes the accepted Ice effect.
 World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 NPC = F1VolleyNPC(World);
 Player = F1Player(World, FVector(180, 0, 100));
 Ice = F1FreezeWithRealIce(World, Player, NPC);
 if (TestNotNull(TEXT("accepted Ice for lethal damage"), Ice))
 {
  NPC->Health = 10;
  ExplodeFireball(World, Player, NPC);
  TestTrue(TEXT("lethal splash is not suppressed by the Ice shell"), NPC->bDead && NPC->Health <= 0);
  TestNull(TEXT("death removes the accepted Ice leech"),
   NPC->FindComponentByClass<UWoTIceLeech>());
  TestTrue(TEXT("frozen lethal control reaches Dying"), F1Until(World, 0.2f, [&]
  {
   return F1Controller(NPC) && F1Controller(NPC)->GetBrainState() == EWoTBrainState::Dying;
  }));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1IceControllerReplacement,
 "WoT.Parity.F1.Caster.IceControllerReplacement", F1Flags)
bool FWoTF1IceControllerReplacement::RunTest(const FString&)
{
 auto ReplaceController = [](UWorld* World, AWoTNPCCharacter* NPC)
 {
  AWoTNPCController* Old = F1Controller(NPC);
  if (Old) Old->UnPossess();
  AWoTNPCController* Fresh = World->SpawnActor<AWoTNPCController>();
  if (Fresh) Fresh->Possess(NPC);
  return TPair<AWoTNPCController*, AWoTNPCController*>(Old, Fresh);
 };

 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTTestCasterNPC* NPC = F1VolleyNPC(World);
 AWoTCharacter* Player = F1Player(World, FVector(180, 0, 100));
 UWoTIceLeech* Ice = F1FreezeWithRealIce(World, Player, NPC);
 if (TestNotNull(TEXT("accepted Ice before active controller replacement"), Ice))
 {
  const auto Controllers = ReplaceController(World, NPC);
  TestTrue(TEXT("replaced controller releases its Ice ownership latch"),
   Controllers.Key && !Controllers.Key->IsSuspendedForIceFreeze());
  TestTrue(TEXT("replacement controller immediately adopts the live Ice suspension"),
   Controllers.Value && Controllers.Value->IsSuspendedForIceFreeze()
    && Controllers.Value->GetBrainState() == EWoTBrainState::Dormant);
  Ice->Lifespan = 0.1f;
  F1Until(World, 0.5f, [&] { return NPC->FindComponentByClass<UWoTIceLeech>() == nullptr; });
  TestTrue(TEXT("replacement controller reselects after thaw"), F1Until(World, 1.0f, [&]
  {
   return Controllers.Value
    && Controllers.Value->GetBrainState() != EWoTBrainState::Dormant;
  }));
 }
 WoTTest::DestroySpecWorld(World);

 // Preserve original Dormant provenance across the same controller handoff.
 World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 NPC = F1VolleyNPC(World);
 Player = F1Player(World, FVector(180, 0, 100));
 if (F1Controller(NPC)) F1Controller(NPC)->ForceDormant();
 Ice = F1FreezeWithRealIce(World, Player, NPC);
 if (TestNotNull(TEXT("accepted Ice before dormant controller replacement"), Ice))
 {
  const auto Controllers = ReplaceController(World, NPC);
  TestTrue(TEXT("replacement controller holds pre-Dormant freeze in Dormant"),
   Controllers.Value && Controllers.Value->IsSuspendedForIceFreeze()
    && Controllers.Value->GetBrainState() == EWoTBrainState::Dormant);
  Ice->Lifespan = 0.1f;
  F1Until(World, 0.5f, [&] { return NPC->FindComponentByClass<UWoTIceLeech>() == nullptr; });
  TestTrue(TEXT("pre-Dormant provenance survives replacement and thaw"),
   Controllers.Value && Controllers.Value->GetBrainState() == EWoTBrainState::Dormant);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF1IceSuspendsCreatureBehavior,
 "WoT.Parity.F1.Caster.IceSuspendsCreatureBehavior", F1Flags)
bool FWoTF1IceSuspendsCreatureBehavior::RunTest(const FString&)
{
 // A real Myrddraal behavior tick must not let its passive close-seeker
 // perimeter bypass accepted Ice's state, animation, or teleport presentation
 // suspension. The same offender remains live and must be observed after thaw.
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTNPCCharacter* Myrddraal = F1Creature(World, TEXT("Myrddraal"));
 AWoTNPCController* Controller = F1Controller(Myrddraal);
 UWoTMyrddraalTeleportBehavior* Teleport = Myrddraal
  ? Myrddraal->FindComponentByClass<UWoTMyrddraalTeleportBehavior>() : nullptr;
 AWoTCharacter* Player = F1Player(World, FVector(-80, 0, 100));
 UWoTIceLeech* Ice = F1FreezeWithRealIce(World, Player, Myrddraal);
 if (TestNotNull(TEXT("accepted Ice on behavior-bearing Myrddraal"), Ice)
  && TestNotNull(TEXT("Myrddraal controller"), Controller)
  && TestNotNull(TEXT("Myrddraal teleport behavior"), Teleport))
 {
  AWoTPathNode* DarkNode = World->SpawnActor<AWoTPathNode>(
   Myrddraal->GetActorLocation(), FRotator::ZeroRotator);
  if (DarkNode) DarkNode->bDark = true;
  AWoTTestSeeker* Seeker = World->SpawnActor<AWoTTestSeeker>(
   Myrddraal->GetActorLocation() + FVector(1000, 0, 0), FRotator::ZeroRotator);
  if (Seeker)
  {
   Seeker->SetActorTickEnabled(false);
   Seeker->SetActorEnableCollision(false);
   Seeker->SetDestination(Myrddraal);
   Seeker->SetActorLocation(Myrddraal->GetActorLocation() + FVector(5, 0, 0));
  }
  float MyrddraalRadius = 0.0f;
  float MyrddraalHalfHeight = 0.0f;
  Myrddraal->GetSimpleCollisionCylinder(MyrddraalRadius, MyrddraalHalfHeight);
  TestTrue(TEXT("targeting seeker breaches the real passive perimeter fixture"),
   Seeker && WoTUE1Collision::CylindersOverlap(Myrddraal->GetActorLocation(),
    MyrddraalRadius * Teleport->DefensivePerimeterRadiusFactor,
    MyrddraalHalfHeight * Teleport->DefensivePerimeterRadiusFactor, Seeker));
  const FName FrozenSequence = Myrddraal->RepAnim.Sequence;
  const uint8 FrozenToken = Myrddraal->RepAnim.PlayToken;
  const uint16 FrozenPresentation = Myrddraal->GetTeleportPresentationSerialForTest();
  WoTTest::TickWorld(World, 12, 1.0f / 60.0f);
  TestFalse(TEXT("frozen behavior cannot start a passive teleport episode"),
   Controller->IsTeleportEpisodeActive());
  TestEqual(TEXT("frozen behavior cannot replace the Dormant shell"),
   static_cast<int32>(Controller->GetBrainState()),
   static_cast<int32>(EWoTBrainState::Dormant));
  TestEqual(TEXT("frozen behavior cannot replace the held animation sequence"),
   Myrddraal->RepAnim.Sequence, FrozenSequence);
  TestEqual(TEXT("frozen behavior cannot stamp an animation token"),
   Myrddraal->RepAnim.PlayToken, FrozenToken);
  TestEqual(TEXT("frozen behavior cannot publish teleport presentation"),
   Myrddraal->GetTeleportPresentationSerialForTest(), FrozenPresentation);

  Ice->Lifespan = 0.1f;
  TestTrue(TEXT("Myrddraal naturally thaws"), F1Until(World, 0.5f, [&]
  {
   return Myrddraal->FindComponentByClass<UWoTIceLeech>() == nullptr;
  }));
  TestTrue(TEXT("the same passive seeker resumes the real behavior after thaw"),
   F1Until(World, 0.5f, [&] { return Controller->IsTeleportEpisodeActive(); }));
 }
 WoTTest::DestroySpecWorld(World);

 // Damage and health remain live while frozen, but the distinct BATrolloc
 // TakeDamage behavior callback must not arm or enter Recovering until thaw.
 World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 WoTTest::TickWorld(World, 5.1f);
 AWoTNPCCharacter* BATrolloc = F1Creature(World, TEXT("BATrolloc"));
 Controller = F1Controller(BATrolloc);
 UWoTBATrollocRecoveryBehavior* Recovery = BATrolloc
  ? BATrolloc->FindComponentByClass<UWoTBATrollocRecoveryBehavior>() : nullptr;
 Player = F1Player(World, FVector(-80, 0, 100));
 Ice = F1FreezeWithRealIce(World, Player, BATrolloc);
 if (TestNotNull(TEXT("accepted Ice on behavior-bearing BATrolloc"), Ice)
  && TestNotNull(TEXT("BATrolloc controller"), Controller)
  && TestNotNull(TEXT("BATrolloc recovery behavior"), Recovery))
 {
  Recovery->bNoInitialRecovery = false;
  BATrolloc->Health = 200;
  const int32 HealthBefore = BATrolloc->Health;
  const FName FrozenSequence = BATrolloc->RepAnim.Sequence;
  const uint8 FrozenToken = BATrolloc->RepAnim.PlayToken;
  FWoTDamageEvent Damage;
  Damage.WoTDamageType = TEXT("direct");
  BATrolloc->TakeDamage(20.0f, Damage, Player->GetController(), Player);
  TestTrue(TEXT("frozen BATrolloc still takes surviving damage"),
   BATrolloc->Health < HealthBefore);
  TestFalse(TEXT("frozen damage cannot enter the recovery behavior"),
   Recovery->IsRecovering());
  TestEqual(TEXT("frozen damage cannot stamp the recovery cooldown"),
   Recovery->GetLastRecoveryTime(), 0.0f);
  TestEqual(TEXT("frozen damage callback cannot replace the held sequence"),
   BATrolloc->RepAnim.Sequence, FrozenSequence);
  TestEqual(TEXT("frozen damage callback cannot stamp an animation token"),
   BATrolloc->RepAnim.PlayToken, FrozenToken);

  Ice->Lifespan = 0.1f;
  TestTrue(TEXT("BATrolloc naturally thaws"), F1Until(World, 0.5f, [&]
  {
   return BATrolloc->FindComponentByClass<UWoTIceLeech>() == nullptr;
  }));
  BATrolloc->TakeDamage(20.0f, Damage, Player->GetController(), Player);
  TestTrue(TEXT("the real recovery callback resumes after thaw"),
   Recovery->IsRecovering());
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}
#endif
