// Engine event parity: use actual world collision, noise emission and walk-off physics.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS
#include "WoTTestAIClasses.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Traps/WoTAlarm.h"
#include "../WoTPlayerState.h"
#include "../WoTGameMode.h"
#include "../WoTCarcass.h"
#include "../WoTZoneVolume.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

namespace
{
 constexpr auto PCFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

 AWoTTestNPCCharacter* PCNPC(UWorld* World, const FVector& At, uint8 Team, bool bObserveStates = false)
 {
  FActorSpawnParameters Params;
  Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  AWoTTestNPCCharacter* NPC = nullptr;
  if (bObserveStates)
  {
   const FTransform Transform(At);
   NPC = World->SpawnActorDeferred<AWoTTestNPCCharacter>(AWoTTestNPCCharacter::StaticClass(),
    Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
   NPC->AIControllerClass = AWoTTestStateObservationController::StaticClass();
   NPC->FinishSpawning(Transform);
  }
  else NPC = World->SpawnActor<AWoTTestNPCCharacter>(At, FRotator::ZeroRotator, Params);
  NPC->AssignedTeam = Team;
  NPC->Health = 100;
  NPC->SetInstigator(NPC);
  NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
  return NPC;
 }

 AWoTTestProjectileCollisionTarget* PCSolid(UWorld* World, const FVector& At, const FVector& Extent)
 {
  AWoTTestProjectileCollisionTarget* Solid = World->SpawnActor<AWoTTestProjectileCollisionTarget>(At, FRotator::ZeroRotator);
  Solid->Box->SetBoxExtent(Extent);
  Solid->Box->SetCollisionObjectType(ECC_WorldStatic);
  Solid->Box->SetCollisionResponseToAllChannels(ECR_Block);
  return Solid;
 }

 // The retail visibility query admits level geometry/movers, including real
 // AStaticMeshActor owners. A generic box actor would be deliberately ignored.
 AStaticMeshActor* PCWall(UWorld* World, const FVector& At, const FVector& Extent)
 {
  AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(At, FRotator::ZeroRotator);
  UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
  Wall->AddInstanceComponent(Box);
  Box->SetupAttachment(Wall->GetRootComponent());
  Box->SetBoxExtent(Extent);
  Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
  Box->SetCollisionObjectType(ECC_WorldStatic);
  Box->SetCollisionResponseToAllChannels(ECR_Block);
  Box->RegisterComponent();
  return Wall;
 }

 AWoTCharacter* PCPlayer(UWorld* World, const FVector& At)
 {
  FActorSpawnParameters Params;
  Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  AWoTCharacter* Player = World->SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, Params);
  APlayerController* PC = World->SpawnActor<APlayerController>();
  PC->InitPlayerState();
  if (AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>()) PS->Team = 2;
  PC->Possess(Player);
  Player->Health = 100000;
  Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
  return Player;
 }

 AWoTCharacter* PCActivityAnchor(UWorld* World, const FVector& At)
 {
  FActorSpawnParameters Params;
  Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
  AWoTCharacter* Anchor = World->SpawnActor<AWoTCharacter>(At, FRotator::ZeroRotator, Params);
  Anchor->Health = 0;
  Anchor->bDead = true;
  Anchor->SetActorHiddenInGame(true);
  Anchor->SetActorEnableCollision(false);
  return Anchor;
 }

 struct PCListener
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTTestNPCCharacter* Guard = PCNPC(World, FVector(0, 0, 100), 1);
  AWoTTestNPCCharacter* Source = PCNPC(World, FVector(450, 0, 100), 2);
  PCListener()
  {
   PCActivityAnchor(World, Guard->GetActorLocation());
   // No sight tick is delivered: any acquired goal came from EmitNoise, the
   // common production entry called by footsteps/landing/weapon noise.
   Source->GetController()->UnPossess();
  }
  ~PCListener() { WoTTest::DestroySpecWorld(World); }
 };

 struct PCLedgeEpisode
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTTestNPCCharacter* Guard;
  AWoTTestStateObservationController* Controller;
  UCharacterMovementComponent* Movement;
  AWoTTestProjectileCollisionTarget* Floor;
  PCLedgeEpisode(float Height = 800.0f, bool bMovableFloor = false)
  {
   PCSolid(World, FVector(0, 0, Height - 10), FVector(120, 200, 10));
   Floor = PCSolid(World, FVector(600, 0, -10), FVector(1500, 1500, 10));
   Floor->Box->SetMobility(bMovableFloor ? EComponentMobility::Movable : EComponentMobility::Static);
   Guard = PCNPC(World, FVector(100, 0, Height + 50), 1, true);
   PCActivityAnchor(World, Guard->GetActorLocation());
   Guard->AnimTableName = TEXT("Trolloc");
   Guard->ChanceOfHitAnim = 1.0f;
   Guard->HitReferenceHealth = 100.0f;
   Guard->Health = 1000;
   Controller = Cast<AWoTTestStateObservationController>(Guard->GetController());
   Movement = Guard->GetCharacterMovement();
   Movement->MaxWalkSpeed = 500.0f;
   Movement->SetMovementMode(MOVE_Walking);
   WoTTest::TickWorld(World, 0.1f);
  }
  ~PCLedgeEpisode() { WoTTest::DestroySpecWorld(World); }
  bool WalkOff()
  {
   for (int32 Frame = 0; Frame < 120 && !Movement->IsFalling(); ++Frame)
   {
    Guard->AddMovementInput(FVector::ForwardVector, 1.0f, true);
    WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   }
   return Movement->IsFalling();
  }
  bool Land()
  {
   for (int32 Frame = 0; Frame < 300 && !Movement->IsMovingOnGround(); ++Frame)
    WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
   return Movement->IsMovingOnGround();
  }
 };

 AWoTMashadarGuide* PCGuide(UWorld* World, const FVector& At,
  float Radius, bool bCitadel = false)
 {
  const FTransform Transform(FRotator::ZeroRotator, At);
  AWoTMashadarGuide* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
   AWoTMashadarGuide::StaticClass(), Transform, nullptr, nullptr,
   ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
  Guide->Team = 1;
  Guide->SearchRadius = Radius;
  Guide->TriggerRadius = Radius;
  Guide->SearchResolution = 100.0f;
  Guide->bCitadelPlaced = bCitadel;
  Guide->FinishSpawning(Transform);
  return Guide;
 }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCMashadarDetection,
 "WoT.Parity.C.Mashadar.DetectionCylinderAndReachability", PCFlags)
bool FWoTPCMashadarDetection::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTMashadarGuide* CornerGuide = PCGuide(World, FVector(0,0,100), 100.0f);
 AWoTCharacter* CornerPlayer = PCPlayer(World, FVector(80,0,180));
 for (int32 Frame=0; Frame<30 && CornerGuide->Destination!=CornerPlayer; ++Frame)
  WoTTest::TickWorld(World, 1, 1.0f/60.0f);
 TestEqual(TEXT("player inside 2D-radius plus absolute-Z cylinder is detected"),
  CornerGuide->Destination.Get(), static_cast<AActor*>(CornerPlayer));

 AWoTMashadarGuide* BlockedGuide = PCGuide(World, FVector(1000,0,100), 200.0f);
 AWoTCharacter* BlockedPlayer = PCPlayer(World, FVector(1120,0,100));
 PCWall(World, FVector(1060,0,100), FVector(10,200,200));
 for (int32 Frame=0; Frame<30; ++Frame)
  WoTTest::TickWorld(World, 1, 1.0f/60.0f);
 TestNotEqual(TEXT("no-LOS perception still rejects an unreachable destination"),
  BlockedGuide->Destination.Get(), static_cast<AActor*>(BlockedPlayer));
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCMashadarNativeCylinderOrigin,
 "WoT.Parity.C.Mashadar.NativeCylinderCurrentLocationAndVisibility", PCFlags)
bool FWoTPCMashadarNativeCylinderOrigin::RunTest(const FString&)
{
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTMashadarGuide* Guide = PCGuide(World, FVector(0,0,100), 150.0f);
  Guide->SetActorLocation(FVector(1000,0,100));
  AWoTCharacter* Player = PCPlayer(World, FVector(1050,0,100));
  for (int32 Frame=0; Frame<30 && Guide->Destination!=Player; ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f/60.0f);
  TestEqual(TEXT("native cylinder follows the projectile observer's current location"),
   Guide->Destination.Get(), static_cast<AActor*>(Player));
  WoTTest::DestroySpecWorld(World);
 }
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTMashadarGuide* Guide = PCGuide(World, FVector(0,0,100), 150.0f);
  AWoTCharacter* HiddenPlayer = PCPlayer(World, FVector(50,0,100));
  HiddenPlayer->SetActorHiddenInGame(true);
  WoTTest::TickWorld(World, 30, 1.0f/60.0f);
  TestNull(TEXT("native ShowSelf dispatch skips hidden player pawns"),
   Guide->Destination.Get());
  WoTTest::DestroySpecWorld(World);
 }
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTMashadarGuide* Guide = PCGuide(World, FVector(0,0,100), 100.0f);
  Guide->SetActorTickEnabled(false);
  PCPlayer(World, FVector(80,0,180));
  Guide->SearchAndChase();
  TestNull(TEXT("explicit GetClosestPawn remains a lair-centered 3D sphere"),
   Guide->Destination.Get());
  WoTTest::DestroySpecWorld(World);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCMashadarSeePlayerPolicy,
 "WoT.Parity.C.Mashadar.SeePlayerRateLimitAndClosestTarget", PCFlags)
bool FWoTPCMashadarSeePlayerPolicy::RunTest(const FString&)
{
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTMashadarGuide* Guide = PCGuide(World, FVector(0,0,100), 500.0f);
  PCPlayer(World, FVector(300,-100,100));
  AWoTCharacter* NearPlayer = PCPlayer(World, FVector(100,100,100));
  for (int32 Frame=0; Frame<30; ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f/60.0f);
  TestEqual(TEXT("player-controller iteration cannot replace a closer target"),
   Guide->Destination.Get(), static_cast<AActor*>(NearPlayer));
  WoTTest::DestroySpecWorld(World);
 }
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  AWoTMashadarGuide* Guide = PCGuide(World, FVector(0,0,100), 300.0f);
  // Keep the shipped two-second search cadence armed: it must not bypass the
  // native SeePlayer retry table and reacquire this pawn early.
  Guide->SearchResolution = 2.0f;
  AWoTCharacter* Player = PCPlayer(World, FVector(100,0,100));
  for (int32 Frame=0; Frame<30 && Guide->Destination!=Player; ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f/60.0f);
  TestEqual(TEXT("first native SeePlayer dispatch acquires immediately"),
   Guide->Destination.Get(), static_cast<AActor*>(Player));
  Player->SetActorLocation(FVector(500,0,100));
  WoTTest::TickWorld(World, 1, 1.0f/120.0f);
  TestNull(TEXT("per-tick leash releases the target"), Guide->Destination.Get());
  Player->SetActorLocation(FVector(100,0,100));
  WoTTest::TickWorld(World, 300, 1.0f/120.0f);
  TestNull(TEXT("same pawn remains suppressed before the three-second minimum"),
   Guide->Destination.Get());
  for (int32 Frame=0; Frame<450 && Guide->Destination!=Player; ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f/120.0f);
  TestEqual(TEXT("same pawn is eligible by the six-second maximum"),
   Guide->Destination.Get(), static_cast<AActor*>(Player));
  WoTTest::DestroySpecWorld(World);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCMashadarMover,
 "WoT.Parity.C.Mashadar.PassesMoverWhileSeeking", PCFlags)
bool FWoTPCMashadarMover::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AWoTMashadarGuide* Guide = PCGuide(World, FVector(0,0,100), 1000.0f);
 AWoTCharacter* Player = PCPlayer(World, FVector(700,0,100));
 for (int32 Frame=0; Frame<60 && Guide->Destination!=Player; ++Frame)
  WoTTest::TickWorld(World, 1, 1.0f/60.0f);
 TestEqual(TEXT("control acquires before the mover appears"),
  Guide->Destination.Get(), static_cast<AActor*>(Player));
 AStaticMeshActor* Mover = PCWall(World, FVector(300,0,100), FVector(10,200,200));
 TArray<UBoxComponent*> Boxes;
 Mover->GetComponents<UBoxComponent>(Boxes);
 for (UBoxComponent* Box : Boxes)
 {
  Box->SetMobility(EComponentMobility::Movable);
  Box->SetCollisionObjectType(ECC_WorldDynamic);
 }
 for (int32 Frame=0; Frame<360 && Guide->GetActorLocation().X<400; ++Frame)
  WoTTest::TickWorld(World, 1, 1.0f/60.0f);
 TestTrue(TEXT("bCollideWorld=false guide passes a dynamic mover"),
  Guide->GetActorLocation().X >= 400);
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCLegionCarcassBaseChange,
 "WoT.Parity.C.LegionCarcass.LandedAndBaseChange", PCFlags)
bool FWoTPCLegionCarcassBaseChange::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 AStaticMeshActor* Floor = PCWall(World, FVector(0,0,0), FVector(1000,1000,10));
 PCPlayer(World, FVector(300,0,100));
 AWoTNPCCharacter* Legion = WoTCreatureData::SpawnCreature(*World, TEXT("Legion"),
  FVector(0,0,100), 1, true, AWoTTestNPCCharacter::StaticClass());
 if (!TestNotNull(TEXT("Legion spawned"), Legion))
 {
  WoTTest::DestroySpecWorld(World);
  return false;
 }
 Legion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
 Legion->Health = 10;
 FWoTDamageEvent Damage;
 Damage.WoTDamageType = TEXT("Sliced");
 Legion->TakeDamage(20.0f, Damage, nullptr, Legion);
 AWoTCarcass* Carcass = nullptr;
 for (int32 Frame = 0; Frame < 720 && (!Carcass || !Carcass->IsSinkingForTest()); ++Frame)
 {
  for (TActorIterator<AWoTCarcass> It(World); It; ++It)
   if (It->CarcassTypeName.ToString().Contains(TEXT("Legion"))) Carcass = *It;
  WoTTest::TickWorld(World, 1, 1.0f/120.0f);
 }
 TestTrue(TEXT("real Landed event starts Legion sinking"), Carcass && Carcass->IsSinkingForTest());
 if (Carcass)
 {
  TestTrue(TEXT("active sink rate is the retail 50-percent perturbation of 3"),
   Carcass->GetSinkRateForTest() >= 1.5f && Carcass->GetSinkRateForTest() <= 4.5f);
  for (int32 Frame = 0; Frame < 2400 && Carcass->GetSinkDistanceForTest() < 30.0f; ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f/120.0f);
  TestTrue(TEXT("sink accumulated against the landed base"),
   Carcass->GetSinkDistanceForTest() >= 30.0f);
  const float Rate = Carcass->GetSinkRateForTest();
  AStaticMeshActor* Replacement = PCWall(World, FVector(0,0,0),
   FVector(1000,1000,10));
  TArray<UBoxComponent*> ReplacementBoxes;
  Replacement->GetComponents<UBoxComponent>(ReplacementBoxes);
  Replacement->GetRootComponent()->SetMobility(EComponentMobility::Movable);
  for (UBoxComponent* Box : ReplacementBoxes)
  {
   Box->SetMobility(EComponentMobility::Movable);
   Box->SetRelativeLocation(FVector(100,0,0));
  }
  Replacement->SetActorLocation(FVector(-100,0,0));
  TArray<UBoxComponent*> OldSupportBoxes;
  Floor->GetComponents<UBoxComponent>(OldSupportBoxes);
  for (UBoxComponent* Box : OldSupportBoxes)
   Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
  for (int32 Frame = 0; Frame < 10 && Carcass->GetSinkingBaseForTest() != Replacement; ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f/120.0f);
  TestTrue(TEXT("valid-to-valid support change resets BaseChange sink distance"),
   Carcass->GetSinkDistanceForTest() < 0.1f);
  TestEqual(TEXT("BaseChange does not reroll the active sink rate"),
   Carcass->GetSinkRateForTest(), Rate);
  WoTTest::TickWorld(World, 1, 1.0f/120.0f);
  TestEqual(TEXT("replacement support remains bound after a deep sink reset"),
   Carcass->GetSinkingBaseForTest(), static_cast<AActor*>(Replacement));
  const FVector RelativeToBase = Carcass->GetActorLocation()
   - Replacement->GetActorLocation();
  const float BeforeBaseYaw = Carcass->GetActorRotation().Yaw;
  Replacement->AddActorWorldRotation(FRotator(0,90,0));
  WoTTest::TickWorld(World, 1, 1.0f/120.0f);
  const FVector ExpectedAfterYaw = Replacement->GetActorLocation()
   + FRotator(0,90,0).RotateVector(RelativeToBase);
  TestTrue(TEXT("rotating support orbits the based sinking carcass"),
   FVector::Dist2D(Carcass->GetActorLocation(), ExpectedAfterYaw) < 1.0f);
  TestTrue(TEXT("rotating support carries the carcass yaw"),
   FMath::Abs(FMath::FindDeltaAngleDegrees(
    BeforeBaseYaw + 90.0f, Carcass->GetActorRotation().Yaw)) < 0.1f);
  const float BeforeBaseMoveZ = Carcass->GetActorLocation().Z;
  Replacement->AddActorWorldOffset(FVector(0,0,20));
  WoTTest::TickWorld(World, 1, 1.0f/120.0f);
  TestTrue(TEXT("sinking carcass remains based on a moving replacement support"),
   Carcass->GetActorLocation().Z > BeforeBaseMoveZ + 19.0f);
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// UE1's APawn::CanHear: the final TRACE_VisBlocking is mandatory for ordinary noise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCNoiseWall, "WoT.Parity.C.Hearing.WallBlocksFootstepNoise", PCFlags)
bool FWoTPCNoiseWall::RunTest(const FString&)
{
 PCListener Fixture;
 PCWall(Fixture.World, FVector(225, 0, 100), FVector(20, 200, 200));
 TestTrue(TEXT("real wall blocks the production retail visibility query"),
  WoTVisBlockingTraceHits(Fixture.World, Fixture.Guard->GetActorLocation(),
   Fixture.Source->GetActorLocation(), Fixture.Guard));
 Fixture.Source->EmitNoise(1.0f);
 TestFalse(TEXT("footstep noise behind the wall cannot acquire an enemy"),
  Fixture.Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCNoiseLOS, "WoT.Parity.C.Hearing.LineOfSightHears", PCFlags)
bool FWoTPCNoiseLOS::RunTest(const FString&)
{
 PCListener Fixture;
 Fixture.Source->EmitNoise(1.0f);
 TestEqual(TEXT("unobstructed audible noise acquires its instigator"),
  Fixture.Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(), static_cast<AActor*>(Fixture.Source));
 return !HasAnyErrors();
}

// Noise-slot dedup belongs to the emitter, not to the observer's acquired-state gate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCNoiseDedupe, "WoT.Parity.C.Hearing.RepeatWithinPointTwoIgnored", PCFlags)
bool FWoTPCNoiseDedupe::RunTest(const FString&)
{
 PCListener Fixture;
 // Native slots start at zero: move beyond the initial 0.2s before first emission.
 Fixture.Guard->Brain->SetNotifierEnabled(EWoTNotifierSlot::Visibility, false);
 WoTTest::TickWorld(Fixture.World, 0.25f);
 Fixture.Source->EmitNoise(1.0f);
 TestTrue(TEXT("first noise was heard"), Fixture.Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
 Fixture.Guard->Brain->InvalidateGoal(EWoTGoalIndex::Threat);
 Fixture.Source->EmitNoise(1.0f);
 TestFalse(TEXT("same-instigator same-area repeat inside0.2s is suppressed before dispatch"),
  Fixture.Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
 WoTTest::TickWorld(Fixture.World, 0.21f);
 Fixture.Source->EmitNoise(1.0f);
 TestTrue(TEXT("repeat becomes audible after0.2s"), Fixture.Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
 return !HasAnyErrors();
}

// Native Pawn HearingThreshold=1, Alertness=0, perceived=min(1200000/d2,2).
// At300uu and loudness0.3: hard radius600 passes, stimulus0.6 fails threshold1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCNoiseQuiet, "WoT.Parity.C.Hearing.QuietFailsStimulusThreshold", PCFlags)
bool FWoTPCNoiseQuiet::RunTest(const FString&)
{
 PCListener Fixture;
 Fixture.Source->SetActorLocation(FVector(300, 0, 100));
 Fixture.Source->EmitNoise(0.3f);
 TestFalse(TEXT("inside hard range is insufficient when perceived loudness is below1"),
  Fixture.Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
 Fixture.Source->EmitNoise(1.0f); // much-louder exception bypasses same-area dedup
 TestTrue(TEXT("loud control at the same distance is heard"), Fixture.Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
 return !HasAnyErrors();
}

// Grunt.uc:849-854,2168-2212: FallingState is entered by the engine's walk-off
// event, preserves the acquired threat, then Landed resumes the interrupted state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCWalkOff, "WoT.Parity.C.Falling.WalkOffIgnoresEventsAndResumes", PCFlags)
bool FWoTPCWalkOff::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 PCSolid(World, FVector(0, 0, 390), FVector(120, 200, 10)); // finite ledge, top400
 PCSolid(World, FVector(600, 0, -10), FVector(1500, 1500, 10)); // landing floor, top0
 AWoTTestNPCCharacter* Guard = PCNPC(World, FVector(60, 0, 450), 1, true);
 AWoTTestStateObservationController* Controller = Cast<AWoTTestStateObservationController>(Guard->GetController());
 UCharacterMovementComponent* Movement = Guard->GetCharacterMovement();
 Movement->SetMovementMode(MOVE_Walking);
 WoTTest::TickWorld(World, 0.1f);
 TestTrue(TEXT("fixture starts genuinely grounded on the ledge"), Movement->IsMovingOnGround());
 AWoTCharacter* OriginalThreat = PCPlayer(World, FVector(1600, 0, 450));
 for (int32 Frame = 0; Frame < 120 && Controller->GetBrainState() != EWoTBrainState::Tracking; ++Frame)
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
 TestEqual(TEXT("actual enemy acquisition enters distinguishable Tracking before walk-off"),
  Controller->GetBrainState(), EWoTBrainState::Tracking);
 TestEqual(TEXT("original acquired threat is the possessed enemy"),
  Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(), static_cast<AActor*>(OriginalThreat));
 TestTrue(TEXT("guard remains grounded when interrupted state is captured"), Movement->IsMovingOnGround());
 const EWoTBrainState Previous = Controller->GetBrainState();
 Controller->StateEntries.Reset();
 auto FirstStateAfterFalling = [&]() -> TOptional<EWoTBrainState>
 {
  bool bSawFalling = false;
  for (const EWoTBrainState Entry : Controller->StateEntries)
  {
   if (bSawFalling) return Entry;
   bSawFalling = Entry == EWoTBrainState::Falling;
  }
  return {};
 };
 bool bWalkedOff = false;
 for (int32 Frame = 0; Frame < 120 && !bWalkedOff; ++Frame)
 {
  Guard->AddMovementInput(FVector::ForwardVector, 1.0f, true);
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  bWalkedOff = Movement->IsFalling();
 }
 TestTrue(TEXT("movement ticks walked the capsule off the finite ledge"), bWalkedOff);
 if (bWalkedOff)
 {
  TestEqual(TEXT("actual walk-off entered the brain Falling state"), Controller->GetBrainState(), EWoTBrainState::Falling);
  AWoTTestNPCCharacter* Source = PCNPC(World, Guard->GetActorLocation() + FVector(300, 0, 0), 2);
  Source->GetController()->UnPossess();
  Source->EmitNoise(1.0f);
  AWoTCharacter* Player = PCPlayer(World, Guard->GetActorLocation() + FVector(200, 0, 0));
  WoTTest::TickWorld(World, 0.25f); // delivers the ordinary player sight sample
  TestEqual(TEXT("falling hearing/sight do not replace the original valid threat"),
   Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(), static_cast<AActor*>(OriginalThreat));
  Source->Destroy();
  Player->Destroy();
  for (int32 Frame = 0; Frame < 180 && !Movement->IsMovingOnGround(); ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  TestTrue(TEXT("real landing collision returned to walking"), Movement->IsMovingOnGround());
  // Retail FallingState.Landed calls FinishAnim before resuming NextState.
  // Observe the first state entry, not eventual goal re-selection. Recording
  // BeginState also catches an incorrect Falling->Waiting->Tracking in one tick.
  for (int32 Frame = 0; Frame < 180 && !FirstStateAfterFalling().IsSet(); ++Frame)
   WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  const TOptional<EWoTBrainState> FirstResumed = FirstStateAfterFalling();
  TestTrue(TEXT("fall lifecycle records a first transition out of Falling"), FirstResumed.IsSet());
  if (FirstResumed.IsSet())
   TestEqual(TEXT("first transition out of Falling restores saved Tracking without a Waiting detour"),
    FirstResumed.GetValue(), Previous);
  TestEqual(TEXT("original threat remains valid across falling and landing"),
   Guard->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(), static_cast<AActor*>(OriginalThreat));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// Grunt.FallingState must reject orders/alarm even when no occupied goal can
// mask the state-specific stubs. Keep this fixture separate from acquired resume.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCUnoccupiedWalkOff, "WoT.Parity.C.Falling.UnoccupiedWalkOffRejectsOrdersAndAlarm", PCFlags)
bool FWoTPCUnoccupiedWalkOff::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 PCSolid(World, FVector(0, 0, 390), FVector(120, 200, 10));
 PCSolid(World, FVector(600, 0, -10), FVector(1500, 1500, 10));
 AWoTTestNPCCharacter* Guard = PCNPC(World, FVector(60, 0, 450), 1);
 PCActivityAnchor(World, Guard->GetActorLocation());
 AWoTNPCController* Controller = Cast<AWoTNPCController>(Guard->GetController());
 UCharacterMovementComponent* Movement = Guard->GetCharacterMovement();
 Movement->SetMovementMode(MOVE_Walking);
 WoTTest::TickWorld(World, 0.1f);
 TestTrue(TEXT("unoccupied fixture starts grounded on the finite ledge"), Movement->IsMovingOnGround());
 TestFalse(TEXT("unoccupied guard has no acquired Threat before walk-off"), Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
 TestTrue(TEXT("unoccupied grounded guard ordinarily accepts new orders"), Controller->CanBeGivenNewOrders());
 bool bWalkedOff = false;
 for (int32 Frame = 0; Frame < 120 && !bWalkedOff; ++Frame)
 {
  Guard->AddMovementInput(FVector::ForwardVector, 1.0f, true);
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
  bWalkedOff = Movement->IsFalling();
 }
 TestTrue(TEXT("unoccupied guard genuinely walks off the finite ledge"), bWalkedOff);
 if (bWalkedOff)
 {
  TestFalse(TEXT("unoccupied falling guard still has no Threat"), Guard->Brain->IsGoalValid(EWoTGoalIndex::Threat));
  TestFalse(TEXT("unoccupied falling guard has no Guarding goal"), Guard->Brain->IsGoalValid(EWoTGoalIndex::Guarding));
  TestFalse(TEXT("unoccupied falling guard has no Refuge goal"), Guard->Brain->IsGoalValid(EWoTGoalIndex::Refuge));
  TestFalse(TEXT("unoccupied falling guard has no Intermediate before alarm"), Guard->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
  TestEqual(TEXT("unoccupied real walk-off enters brain Falling"), Controller->GetBrainState(), EWoTBrainState::Falling);
  TestFalse(TEXT("Falling alone refuses new orders with no occupied goal"), Controller->CanBeGivenNewOrders());
  AWoTAlarm* Alarm = World->SpawnActor<AWoTAlarm>(Guard->GetActorLocation() + FVector(400, 0, 0), FRotator::ZeroRotator);
  Alarm->ActivateAlarm(nullptr);
  TestTrue(TEXT("real alarm activation rang successfully"), Alarm->bRung);
  TestFalse(TEXT("Falling alone suppresses real alarm navigation without an occupied-goal mask"),
   Guard->Brain->IsGoalValid(EWoTGoalIndex::Intermediate));
 }
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

// FallingState's water-region edge takes Splash rather than waiting forever for
// a ground collision. Drive the same finite ledge with a real water zone below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCWalkOffSplash, "WoT.Parity.C.Falling.WalkOffSplashResumesSavedState", PCFlags)
bool FWoTPCWalkOffSplash::RunTest(const FString&)
{
 UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
 PCSolid(World, FVector(0, 0, 390), FVector(120, 200, 10));
 AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(FVector(600, 0, 0), FRotator::ZeroRotator);
 Water->Collision->SetBoxExtent(FVector(1500, 1500, 300));
 Water->bWaterZone = true;
 AWoTTestNPCCharacter* Guard = PCNPC(World, FVector(60, 0, 450), 1, true);
 PCActivityAnchor(World, Guard->GetActorLocation());
 AWoTTestStateObservationController* Controller = Cast<AWoTTestStateObservationController>(Guard->GetController());
 UCharacterMovementComponent* Movement = Guard->GetCharacterMovement();
 Movement->SetMovementMode(MOVE_Walking);
 WoTTest::TickWorld(World, 0.1f);
 TestTrue(TEXT("splash fixture starts grounded above the water"), Movement->IsMovingOnGround());
 TestFalse(TEXT("splash fixture starts outside water region"),
  AWoTZoneVolume::IsPointInWater(World, Guard->GetActorLocation()));
 const EWoTBrainState Previous = Controller->GetBrainState();
 Controller->StateEntries.Reset();
 for (int32 Frame = 0; Frame < 120 && !Movement->IsFalling(); ++Frame)
 {
  Guard->AddMovementInput(FVector::ForwardVector, 1.0f, true);
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
 }
 TestTrue(TEXT("splash fixture genuinely walks off the ledge"), Movement->IsFalling());
 TestEqual(TEXT("walk-off enters Falling before touching water"), Controller->GetBrainState(), EWoTBrainState::Falling);
 for (int32 Frame = 0; Frame < 180 && Controller->GetBrainState() == EWoTBrainState::Falling; ++Frame)
  WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
 TestTrue(TEXT("actual movement reaches the water region"),
  AWoTZoneVolume::IsPointInWater(World, Guard->GetActorLocation()));
 TestTrue(TEXT("water physics switches the NPC to swimming"), Movement->IsSwimming());
 TestFalse(TEXT("splash resumes without a ground landing"), Movement->IsMovingOnGround());
 TestEqual(TEXT("splash restores the actual saved brain state"), Controller->GetBrainState(), Previous);
 TestTrue(TEXT("splash recorded entry and first exit from Falling"), Controller->StateEntries.Num() >= 2);
 if (Controller->StateEntries.Num() >= 2)
  TestEqual(TEXT("first transition after Falling is the saved state"), Controller->StateEntries[1], Previous);
 WoTTest::DestroySpecWorld(World);
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCNestedTakeHitFall, "WoT.Parity.C.Falling.NestedTakeHitKeepsOuterContinuation", PCFlags)
bool FWoTPCNestedTakeHitFall::RunTest(const FString&)
{
 PCLedgeEpisode F;
 TestTrue(TEXT("soft-hit fixture starts grounded"), F.Movement->IsMovingOnGround());
 const EWoTBrainState Outer = F.Controller->GetBrainState();
 TestEqual(TEXT("outer continuation is Waiting"), Outer, EWoTBrainState::Waiting);
 F.Controller->StateEntries.Reset();
 F.Movement->Velocity = FVector(500, 0, 0);
 // The production soft-hit presentation preserves walking. Damage's independent
 // AddVelocity falling transition would prevent an honest walking loss of floor.
 F.Guard->PlayHitReactAnim(10, TEXT("Sliced"));
 TestEqual(TEXT("first soft reaction entered TakeHit"), F.Controller->GetBrainState(), EWoTBrainState::TakeHit);
 TestTrue(TEXT("soft reaction leaves walking physics active"), F.Movement->IsMovingOnGround());
 if (TestTrue(TEXT("soft reaction genuinely walks off"), F.WalkOff()))
 {
  TestEqual(TEXT("walking loss of floor interrupted the soft TakeHit"), F.Controller->GetBrainState(), EWoTBrainState::Falling);
  TestEqual(TEXT("TakeHit did not finish before the genuine walk-off"), F.Controller->StateEntries.Num(), 2);
  F.Controller->StateEntries.Reset();
  F.Guard->PlayHitReactAnim(10, TEXT("Sliced"));
  TestEqual(TEXT("second airborne hit enters a new TakeHit"), F.Controller->GetBrainState(), EWoTBrainState::TakeHit);
  for (int32 Frame = 0; Frame < 120 && F.Controller->GetBrainState() == EWoTBrainState::TakeHit; ++Frame)
   WoTTest::TickWorld(F.World, 1, 1.0f / 60.0f);
  TestTrue(TEXT("nested hit actually exits TakeHit rather than self-looping"), F.Controller->StateEntries.Num() >= 2);
  if (F.Controller->StateEntries.Num() >= 2)
   TestEqual(TEXT("first restored continuation is the outer Waiting state"), F.Controller->StateEntries[1], Outer);
  TestEqual(TEXT("nested hit returns to the outer state"), F.Controller->GetBrainState(), Outer);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCDamagingLanding, "WoT.Parity.C.Falling.SurvivingDamageStillPlaysLand", PCFlags)
bool FWoTPCDamagingLanding::RunTest(const FString&)
{
 PCLedgeEpisode F(1900.0f);
 // Champion's authored no-retreat capability isolates landing from the separate
 // heavy-hit sidestep operation. Damage, hit animation and physics remain live.
 F.Guard->bChampionOrders = true;
 F.Guard->FatalLandedDamageVelocity = -10000.0f;
 const int32 BeforeHealth = F.Guard->Health;
 const EWoTBrainState Outer = F.Controller->GetBrainState();
 if (TestTrue(TEXT("damaging fixture genuinely walks off"), F.WalkOff()))
 {
  TestEqual(TEXT("damaging walk-off enters Falling"), F.Controller->GetBrainState(), EWoTBrainState::Falling);
  TestTrue(TEXT("damaging fall physically lands"), F.Land());
  TestTrue(TEXT("real landing applied nonfatal damage"), F.Guard->Health > 0 && F.Guard->Health < BeforeHealth);
  const float Impact = F.Guard->GetLastLandingImpactVelocity();
  TestTrue(TEXT("impact crossed the native damage threshold"), Impact <= F.Guard->MinLandedDamageVelocity - 850.0f);
  const int32 ExpectedDamage = FMath::TruncToInt(-0.15f * (Impact + 1050.0f));
  TestEqual(TEXT("landing preserves the normal fall-damage amount"), BeforeHealth - F.Guard->Health, ExpectedDamage);
  TestEqual(TEXT("surviving damage still stamps the Land sequence"), F.Guard->RepAnim.Sequence, FName(TEXT("Landed")));
  TestFalse(TEXT("damaging landing plays the full Land animation"), F.Guard->RepAnim.bTweenOnly != 0);
  TestEqual(TEXT("Land continuation owns Falling after the damage detour"), F.Controller->GetBrainState(), EWoTBrainState::Falling);
  F.Controller->StateEntries.Reset();
  for (int32 Frame = 0; Frame < 180 && F.Controller->GetBrainState() == EWoTBrainState::Falling; ++Frame)
   WoTTest::TickWorld(F.World, 1, 1.0f / 60.0f);
  TestTrue(TEXT("Land animation exits to a recorded continuation"), !F.Controller->StateEntries.IsEmpty());
  if (!F.Controller->StateEntries.IsEmpty())
   TestEqual(TEXT("Land first restores the saved outer state"), F.Controller->StateEntries[0], Outer);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCMovableLanding, "WoT.Parity.C.Falling.MovablePlatformPreservesMovement", PCFlags)
bool FWoTPCMovableLanding::RunTest(const FString&)
{
 PCLedgeEpisode F(400.0f, true);
 if (TestTrue(TEXT("platform fixture genuinely walks off"), F.WalkOff()))
 {
  TestTrue(TEXT("airborne NPC retains horizontal movement"), F.Movement->Velocity.Size2D() > 10.0f);
  TestTrue(TEXT("platform fixture physically lands"), F.Land());
  TestEqual(TEXT("UE installed the actual movable landing component"), F.Guard->GetMovementBaseObject(), static_cast<UObject*>(F.Floor->Box));
  TestTrue(TEXT("movable-platform landing preserves horizontal movement through Land"), F.Movement->Velocity.Size2D() > 10.0f);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCNoiseCreaturePartition, "WoT.Parity.C.Hearing.CreatureFamilyPartition", PCFlags)
bool FWoTPCNoiseCreaturePartition::RunTest(const FString&)
{
 for (const bool bDerivedMaker : {false, true})
 {
  UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
  const FName MakerRow = bDerivedMaker ? TEXT("TrollocStrong") : TEXT("Trolloc");
  const FName RelatedRow = bDerivedMaker ? TEXT("Trolloc") : TEXT("TrollocStrong");
  AWoTNPCCharacter* Maker = WoTCreatureData::SpawnCreature(*World, MakerRow, FVector(450, 0, 100), 2, true, AWoTTestNPCCharacter::StaticClass());
  AWoTNPCCharacter* Related = WoTCreatureData::SpawnCreature(*World, RelatedRow, FVector(0, 0, 100), 1, true, AWoTTestNPCCharacter::StaticClass());
  AWoTNPCCharacter* Unrelated = WoTCreatureData::SpawnCreature(*World, TEXT("Soldier"), FVector(0, 100, 100), 1, true, AWoTTestNPCCharacter::StaticClass());
  if (TestNotNull(TEXT("authored maker row spawned"), Maker) && TestNotNull(TEXT("authored related row spawned"), Related)
   && TestNotNull(TEXT("authored unrelated row spawned"), Unrelated))
  {
   TestTrue(TEXT("related listener starts in a hearing state"), Cast<AWoTNPCController>(Related->GetController())->IsHearingLive());
   TestTrue(TEXT("unrelated listener starts in a hearing state"), Cast<AWoTNPCController>(Unrelated->GetController())->IsHearingLive());
   Maker->EmitNoise(1.0f);
   TestEqual(TEXT("Trolloc ancestry hears in either direction"), Related->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(), static_cast<AActor*>(Maker));
   TestFalse(TEXT("unrelated Soldier row cannot hear a non-player-related Trolloc"), Unrelated->Brain->IsGoalValid(EWoTGoalIndex::Threat));
   // A player-related instigator deliberately opens the ordinary all-NPC fanout.
   AWoTCharacter* Player = PCPlayer(World, FVector(8000, 0, 100));
   Maker->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Player);
   Maker->SetActorLocation(Maker->GetActorLocation() + FVector(60, 0, 0));
   Maker->EmitNoise(1.0f);
   TestEqual(TEXT("player-related noise reaches the unrelated row"), Unrelated->Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get(), static_cast<AActor*>(Maker));
  }
  WoTTest::DestroySpecWorld(World);
 }
 return !HasAnyErrors();
}

// Runtime level geometry keeps its retail level identity even when its UE
// collision component must be movable. Mobility alone cannot classify Base==Level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPCRuntimeWorldLanding, "WoT.Parity.C.Falling.RuntimeBSPWorldStopsMovement", PCFlags)
bool FWoTPCRuntimeWorldLanding::RunTest(const FString&)
{
 PCLedgeEpisode F(400.0f, true);
 F.Floor->Destroy();
 AWoTRuntimeBSPWorld* LevelOwner = F.World->SpawnActor<AWoTRuntimeBSPWorld>(FVector(600, 0, -10), FRotator::ZeroRotator);
 UBoxComponent* LevelCollision = NewObject<UBoxComponent>(LevelOwner);
 LevelOwner->AddInstanceComponent(LevelCollision);
 LevelCollision->SetupAttachment(LevelOwner->GetRootComponent());
 LevelCollision->SetMobility(EComponentMobility::Movable);
 LevelCollision->SetBoxExtent(FVector(1500, 1500, 10));
 LevelCollision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
 LevelCollision->SetCollisionObjectType(ECC_WorldStatic);
 LevelCollision->SetCollisionResponseToAllChannels(ECR_Block);
 LevelCollision->RegisterComponent();
 TestEqual(TEXT("runtime world collision is deliberately movable"), LevelCollision->Mobility, EComponentMobility::Movable);
 if (TestTrue(TEXT("runtime-world fixture genuinely walks off"), F.WalkOff()))
 {
  TestEqual(TEXT("walking loss of floor enters honest Falling"), F.Controller->GetBrainState(), EWoTBrainState::Falling);
  TestTrue(TEXT("runtime-world landing has horizontal movement to stop"), F.Movement->Velocity.Size2D() > 10.0f);
  TestTrue(TEXT("runtime-world fixture physically lands"), F.Land());
  TestEqual(TEXT("actual landing support belongs to runtime BSP world"), F.Guard->GetMovementBaseObject(), static_cast<UObject*>(LevelCollision));
  TestEqual(TEXT("real landing plays Land before the saved continuation"), F.Guard->RepAnim.Sequence, FName(TEXT("Landed")));
  TestTrue(TEXT("retail level landing stops horizontal movement despite movable component"), F.Movement->Velocity.Size2D() <= KINDA_SMALL_NUMBER);
 }
 return !HasAnyErrors();
}
#endif
