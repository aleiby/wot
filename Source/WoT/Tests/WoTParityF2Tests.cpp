#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestAIClasses.h"
#include "WoTTestBattleClasses.h"
#include "../AI/WoTCreatureBehaviors.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTPathNode.h"
#include "../Inventory/WoTAngrealInvGuardian.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTLegionInvSeeker.h"
#include "../Projectiles/WoTArcherArrow.h"
#include "../Projectiles/WoTLegionProjectile.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTProjectile.h"
#include "../WoTAppearEffect.h"
#include "../WoTCarcass.h"
#include "../WoTBattleGameMode.h"
#include "../Projectiles/WoTWeaponProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTDecal.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerState.h"
#include "../WoTRandom.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags F2_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters F2_AlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AWoTNPCCharacter* F2_SpawnStillNPC(UWorld& World, FName Row,
	const FVector& Location, uint8 Team = 2)
{
	AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
		World, Row, Location, Team, true);
	if (NPC)
	{
		if (AController* Controller = NPC->GetController()) Controller->UnPossess();
		NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	return NPC;
}

AStaticMeshActor* F2_SpawnWall(UWorld& World, const FVector& Location)
{
	AStaticMeshActor* Wall = World.SpawnActor<AStaticMeshActor>(
		Location, FRotator::ZeroRotator, F2_AlwaysSpawn());
	if (!Wall) return nullptr;
	UBoxComponent* Box = NewObject<UBoxComponent>(Wall, TEXT("F2Wall"));
	Box->SetupAttachment(Wall->GetRootComponent());
	Box->SetBoxExtent(FVector(20.0f, 200.0f, 200.0f));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	return Wall;
}

AStaticMeshActor* F2_SpawnFloor(UWorld& World, const FVector& Location)
{
	AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(
		Location, FRotator::ZeroRotator, F2_AlwaysSpawn());
	if (!Floor) return nullptr;
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("F2Floor"));
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->SetBoxExtent(FVector(1000.0f, 1000.0f, 10.0f));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	return Floor;
}

template <typename Predicate>
bool F2_TickUntil(UWorld* World, float Timeout, Predicate&& Done)
{
	for (int32 Frame = 0; Frame < FMath::CeilToInt(Timeout * 120.0f); ++Frame)
	{
		if (Done()) return true;
		WoTTest::TickWorld(World, 1.0f / 120.0f);
	}
	return Done();
}

AWoTParticleSprayer* F2_FindSprayer(UWorld* World, FName Config)
{
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (IsValid(*It) && It->ConfigName == Config) return *It;
	}
	return nullptr;
}

AWoTExplosionEffect* F2_FindExplosion(UWorld* World, FName Config)
{
	for (TActorIterator<AWoTExplosionEffect> It(World); It; ++It)
	{
		if (IsValid(*It) && It->ConfigName == Config) return *It;
	}
	return nullptr;
}

AWoTMeshEffect* F2_FindMeshEffect(UWorld* World, FName Config)
{
	for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
	{
		if (IsValid(*It) && It->ConfigName == Config) return *It;
	}
	return nullptr;
}

int32 F2_CountDecals(UWorld* World, FName Config)
{
	int32 Count = 0;
	for (TActorIterator<AWoTDecal> It(World); It; ++It)
	{
		if (IsValid(*It) && It->DecalType == Config) ++Count;
	}
	return Count;
}

AWoTNPCCharacter* F2_SpawnCombatNPC(UWorld& World, FName Row,
	const FVector& Location, uint8 Team = 1)
{
	AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(
		World, Row, Location, Team, true, AWoTTestNPCCharacter::StaticClass());
	if (NPC) NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	return NPC;
}

AWoTCharacter* F2_SpawnPlayer(UWorld& World, const FVector& Location,
	uint8 Team = 2)
{
	APlayerController* Controller = World.SpawnActor<APlayerController>();
	if (!Controller) return nullptr;
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
	{
		State->Team = Team;
	}
	AWoTCharacter* Player = World.SpawnActor<AWoTCharacter>(Location,
		FRotator::ZeroRotator, F2_AlwaysSpawn());
	if (!Player) return nullptr;
	Controller->Possess(Player);
	Player->Health = 100000;
	Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	return Player;
}

void F2_DestroySoundMarkers(UWorld* World)
{
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		if (IsValid(*It)) It->Destroy();
	}
}

int32 F2_CountSoundMarkers(UWorld* World, const TCHAR* Fragment)
{
	int32 Count = 0;
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		Count += IsValid(*It) && It->SoundSubPath.ToString().Contains(
			Fragment, ESearchCase::IgnoreCase) ? 1 : 0;
	}
	return Count;
}

bool F2_SelectMeleeSequence(AWoTNPCController* Controller,
	AWoTNPCCharacter* NPC, FName Sequence, int32 MaxAttempts = 128)
{
	if (!Controller || !NPC) return false;
	for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
	{
		F2_DestroySoundMarkers(NPC->GetWorld());
		Controller->SetBrainStateForG20Test(EWoTBrainState::Tracking);
		Controller->SetBrainStateForG20Test(EWoTBrainState::PerformMelee);
		if (NPC->GetReplicatedAnimName() == Sequence) return true;
	}
	return false;
}

AWoTLegionInvSeeker* F2_FindLegionLauncher(AWoTNPCCharacter* Legion)
{
	const UWoTInventoryComponent* Inventory = Legion
		? Legion->FindComponentByClass<UWoTInventoryComponent>() : nullptr;
	if (!Inventory) return nullptr;
	for (const TObjectPtr<AWoTInventory>& Item : Inventory->Items)
	{
		if (AWoTLegionInvSeeker* Launcher = Cast<AWoTLegionInvSeeker>(Item))
		{
			return Launcher;
		}
	}
	return nullptr;
}

AWoTAngrealInvGuardian* F2_CastGuardian(UWorld& World)
{
	AWoTCharacter* Caster = F2_SpawnPlayer(World, FVector(0.0f, 0.0f, 100.0f), 1);
	if (!Caster) return nullptr;
	Caster->SummonFaction = EWoTCreatureFaction::Hound;
	Caster->SetActorRotation(FRotator::ZeroRotator);
	if (AController* Controller = Caster->GetController())
	{
		Controller->SetControlRotation(FRotator::ZeroRotator);
	}
	F2_SpawnWall(World, FVector(80.0f, 0.0f, 100.0f));
	AWoTAngrealInvGuardian* Guardian = World.SpawnActor<AWoTAngrealInvGuardian>(
		F2_AlwaysSpawn());
	if (!Guardian) return nullptr;
	Guardian->SetOwner(Caster);
	Guardian->CurCharges = 3;
	Guardian->CastArtifact();
	return Guardian;
}

AWoTMashadarGuide* F2_SpawnMashadarGuide(UWorld& World,
	const FVector& Location, uint8 Team, float Radius, bool bCitadel)
{
	const FTransform Transform(FRotator::ZeroRotator, Location);
	AWoTMashadarGuide* Guide = World.SpawnActorDeferred<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Guide) return nullptr;
	Guide->Team = Team;
	Guide->SearchRadius = Radius;
	Guide->TriggerRadius = Radius;
	Guide->SearchResolution = 100.0f;
	Guide->bCitadelPlaced = bCitadel;
	Guide->FinishSpawning(Transform);
	return Guide;
}

AWoTCharacter* F2_SpawnCapturePlayer(UWorld& World, const FVector& Location,
	uint8 Team, const FString& Name, AWoTTestMessageCaptureController*& OutController)
{
	OutController = World.SpawnActor<AWoTTestMessageCaptureController>(F2_AlwaysSpawn());
	if (!OutController) return nullptr;
	OutController->InitPlayerState();
	if (AWoTPlayerState* State = OutController->GetPlayerState<AWoTPlayerState>())
	{
		State->Team = Team;
		State->SetPlayerName(Name);
	}
	AWoTCharacter* Player = World.SpawnActor<AWoTCharacter>(Location,
		FRotator::ZeroRotator, F2_AlwaysSpawn());
	OutController->Possess(Player);
	Player->Health = 100000;
	Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	return Player;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MyrddraalBoltActorImpactSpec,
	"WoT.Parity.F2.MyrddraalBolt.TrailsAndActorImpact", F2_Flags)
bool FWoTF2MyrddraalBoltActorImpactSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Target = F2_SpawnStillNPC(*World, TEXT("Soldier"),
		FVector(350.0f, 0.0f, 100.0f));
	const int32 InitialHealth = Target ? Target->Health : 0;
	AWoTMyrddraalBolt* Bolt = World->SpawnActor<AWoTMyrddraalBolt>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator, F2_AlwaysSpawn());
	AWoTParticleSprayer* TrailA = F2_FindSprayer(World, TEXT("MyrdSprayerA"));
	AWoTParticleSprayer* TrailB = F2_FindSprayer(World, TEXT("MyrdSprayerB"));
	TestNotNull(TEXT("real bolt flight spawns MyrdSprayerA"), TrailA);
	TestNotNull(TEXT("real bolt flight spawns MyrdSprayerB"), TrailB);
	TestTrue(TEXT("both flight trails follow the bolt"), TrailA && TrailB
		&& TrailA->FollowActor.Get() == Bolt && TrailB->FollowActor.Get() == Bolt);
	TestTrue(TEXT("real bolt damages the NPC and resolves"), Target &&
		F2_TickUntil(World, 1.0f, [&]() { return Target->Health < InitialHealth; }));
	TestTrue(TEXT("resolved trails stop emitting"), TrailA && TrailB
		&& !TrailA->bOn && !TrailB->bOn);
	TestNotNull(TEXT("actor impact spawns MyrdSprayerExp"),
		F2_FindSprayer(World, TEXT("MyrdSprayerExp")));
	AWoTExplosionEffect* Electric =
		F2_FindExplosion(World, TEXT("MyrddraalElectric"));
	if (TestNotNull(TEXT("actor impact spawns MyrddraalElectric"), Electric))
	{
		TestFalse(TEXT("MyrddraalElectric honors its explicit LT_None"),
			Electric->Light->IsVisible());
	}
	TestEqual(TEXT("actor impact does not leave a wall crack"),
		F2_CountDecals(World, TEXT("CrackDecal")), 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MyrddraalBoltWallImpactSpec,
	"WoT.Parity.F2.MyrddraalBolt.WallImpactCrack", F2_Flags)
bool FWoTF2MyrddraalBoltWallImpactSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	F2_SpawnWall(*World, FVector(350.0f, 0.0f, 100.0f));
	World->SpawnActor<AWoTMyrddraalBolt>(FVector(0.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, F2_AlwaysSpawn());
	TestTrue(TEXT("wall hit emits the full bolt impact bundle"),
		F2_TickUntil(World, 1.0f, [&]()
		{
			return F2_FindSprayer(World, TEXT("MyrdSprayerExp")) != nullptr
				&& F2_FindExplosion(World, TEXT("MyrddraalElectric")) != nullptr;
		}));
	TestEqual(TEXT("wall hit aligns one CrackDecal"),
		F2_CountDecals(World, TEXT("CrackDecal")), 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2ArcherArrowTrailSpec,
	"WoT.Parity.F2.ArcherArrow.ExplodeTrail", F2_Flags)
bool FWoTF2ArcherArrowTrailSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Target = F2_SpawnStillNPC(*World, TEXT("Soldier"),
		FVector(650.0f, 0.0f, 100.0f));
	const int32 InitialHealth = Target ? Target->Health : 0;
	World->SpawnActor<AWoTArcherArrow>(FVector(0.0f, 0.0f, 100.0f),
		FRotator::ZeroRotator, F2_AlwaysSpawn());
	TestTrue(TEXT("real arrow damages the NPC"), Target &&
		F2_TickUntil(World, 0.5f, [&]() { return Target->Health < InitialHealth; }));
	TestNotNull(TEXT("arrow actor impact spawns ArrowTrail"),
		F2_FindMeshEffect(World, TEXT("ArrowTrail")));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2ShieldBashSoundSpec,
	"WoT.Parity.F2.ShieldUser.ShieldBashSound", F2_Flags)
bool FWoTF2ShieldBashSoundSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Soldier = F2_SpawnCombatNPC(*World, TEXT("Soldier"),
		FVector(0.0f, 0.0f, 100.0f));
	AWoTCharacter* Player = F2_SpawnPlayer(*World, FVector(45.0f, 0.0f, 100.0f));
	AWoTNPCController* Controller = Soldier
		? Cast<AWoTNPCController>(Soldier->GetController()) : nullptr;
	if (TestNotNull(TEXT("real Soldier controller"), Controller)
		&& TestNotNull(TEXT("possessed player target"), Player))
	{
		Soldier->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Player);
		Soldier->Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		const bool bShield = F2_SelectMeleeSequence(Controller, Soldier,
			TEXT("AttackRunShield"));
		TestTrue(TEXT("random real attack table eventually selects the shield bash"),
			bShield);
		if (bShield)
		{
			TestEqual(TEXT("shield bash has no sword-swish fallback"),
				F2_CountSoundMarkers(World, TEXT("Sword_Swipe")), 0);
			const int32 InitialHealth = Player->Health;
			TestTrue(TEXT("real shield notify and damage path complete"),
				F2_TickUntil(World, 1.5f,
					[&]() { return Player->Health < InitialHealth; }));
			TestTrue(TEXT("notify and landed bash both use Shield_HitPawn"),
				F2_CountSoundMarkers(World, TEXT("Shield_HitPawn")) >= 2);
			TestEqual(TEXT("landed shield bash has no sword-on-flesh cue"),
				F2_CountSoundMarkers(World, TEXT("Sword_HitPawn")), 0);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

bool F2_RunWarderMissAbortSpec(FAutomationTestBase& Test, FName Sequence)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Warder = F2_SpawnCombatNPC(*World, TEXT("Warder"),
		FVector(0.0f, 0.0f, 100.0f));
	AWoTCharacter* Player = F2_SpawnPlayer(*World, FVector(45.0f, 0.0f, 100.0f));
	AWoTNPCController* Controller = Warder
		? Cast<AWoTNPCController>(Warder->GetController()) : nullptr;
	if (Test.TestNotNull(TEXT("real Warder controller"), Controller)
		&& Test.TestNotNull(TEXT("possessed player target"), Player))
	{
		Warder->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Player);
		Warder->Brain->CurrentGoalIndex = EWoTGoalIndex::Threat;
		// The runtime schedule must follow notify time, not incidental decoder row
		// order. Put this sequence in ascending row order so a reverse-index walk
		// would incorrectly attempt the later notify first.
		TArray<int32> SequenceRows;
		for (int32 Index = 0; Index < Warder->MeleeAttacks.Num(); ++Index)
		{
			if (Warder->MeleeAttacks[Index].AnimName == Sequence)
			{
				SequenceRows.Add(Index);
			}
		}
		if (SequenceRows.Num() >= 2
			&& Warder->MeleeAttacks[SequenceRows[0]].NotifyFraction
				> Warder->MeleeAttacks[SequenceRows[1]].NotifyFraction)
		{
			Warder->MeleeAttacks.Swap(SequenceRows[0], SequenceRows[1]);
		}
		const bool bSelected = F2_SelectMeleeSequence(Controller, Warder, Sequence);
		Test.TestTrue(TEXT("random real Warder table selects requested attack"),
			bSelected);
		if (bSelected)
		{
			float EarliestNotify = TNumericLimits<float>::Max();
			for (const FWoTMeleeAttack& Attack : Warder->MeleeAttacks)
			{
				if (Attack.AnimName == Sequence)
				{
					EarliestNotify = FMath::Min(EarliestNotify, Attack.NotifyFraction);
				}
			}
			Player->SetActorLocation(FVector(1200.0f, 0.0f, 100.0f));
			F2_DestroySoundMarkers(World);
			// One hitch crosses every notify in either retail sequence.
			Controller->TickMeleeForTest(3.0f);
			Test.TestEqual(TEXT("same-frame traversal attempts the earliest notify first"),
				Controller->FirstMeleeNotifyFractionForTest, EarliestNotify);
			Test.TestEqual(TEXT("same-frame notify traversal stops after the missed opener"),
				Controller->MeleeNotifyAttemptsForTest, 1);
			const int32 ExpectedPreOpenerSlashes = Sequence == TEXT("DbleSwipe") ? 1 : 0;
			Test.TestEqual(TEXT("same-frame abort emits only slash notifies before the opener"),
				F2_CountSoundMarkers(World, TEXT("Sword_Slash")), ExpectedPreOpenerSlashes);
			Test.TestEqual(TEXT("first missed notify aborts rooted swing immediately"),
				Controller->GetBrainState(), EWoTBrainState::Tracking);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !Test.HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2WarderSkewMissSpec,
	"WoT.Parity.F2.Warder.SkewFirstMissAborts", F2_Flags)
bool FWoTF2WarderSkewMissSpec::RunTest(const FString&)
{
	return F2_RunWarderMissAbortSpec(*this, TEXT("SkewRip"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2WarderDoubleSwipeMissSpec,
	"WoT.Parity.F2.Warder.DoubleSwipeFirstMissAborts", F2_Flags)
bool FWoTF2WarderDoubleSwipeMissSpec::RunTest(const FString&)
{
	return F2_RunWarderMissAbortSpec(*this, TEXT("DbleSwipe"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2LegionShootSpiritSilenceSpec,
	"WoT.Parity.F2.Legion.ShootSpiritSlotMismatchSilent", F2_Flags)
bool FWoTF2LegionShootSpiritSilenceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Legion = F2_SpawnCombatNPC(*World, TEXT("Legion"),
		FVector(0.0f, 0.0f, 100.0f));
	AWoTCharacter* Victim = F2_SpawnPlayer(*World,
		FVector(1500.0f, 0.0f, 100.0f));
	AWoTLegionInvSeeker* Launcher = F2_FindLegionLauncher(Legion);
	if (TestNotNull(TEXT("configured Legion launcher"), Launcher)
		&& TestNotNull(TEXT("possessed launch target"), Victim))
	{
		WoTTest::TickWorld(World, 0.2f);
		F2_DestroySoundMarkers(World);
		Launcher->SetVictim(Victim);
		Launcher->CastArtifact();
		Launcher->UnCastArtifact();
		bool bSpiritLaunched = false;
		for (TActorIterator<AWoTLegionProjectile> It(World); It; ++It)
		{
			bSpiritLaunched |= IsValid(*It);
		}
		TestTrue(TEXT("real Legion launcher still fires a spirit"), bSpiritLaunched);
		TestEqual(TEXT("retail slot-name mismatch leaves the launch silent"),
			F2_CountSoundMarkers(World, TEXT("ShootSpirit")), 0);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2GuardianMultiplayerSpeedSpec,
	"WoT.Parity.F2.MashadarArenaSpawner.MultiplayerMaxSpeed", F2_Flags)
bool FWoTF2GuardianMultiplayerSpeedSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"), TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("F2 Guardian multiplayer speed"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("real player connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	UWorld* World = Worlds.Server.GetWorld();
	APlayerController* Controller = Worlds.GetServerPlayerControllerOfClient(0);
	AWoTCharacter* Caster = Controller ? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;
	if (TestNotNull(TEXT("server owns the connected caster"), Caster))
	{
		Caster->SetActorLocation(FVector(0.0f, 0.0f, 100.0f));
		Caster->SetActorRotation(FRotator::ZeroRotator);
		Controller->SetControlRotation(FRotator::ZeroRotator);
		Caster->SummonFaction = EWoTCreatureFaction::Hound;
		F2_SpawnWall(*World, FVector(80.0f, 0.0f, 100.0f));
		AWoTAngrealInvGuardian* Guardian = World->SpawnActor<AWoTAngrealInvGuardian>(
			F2_AlwaysSpawn());
		Guardian->SetOwner(Caster);
		Guardian->CurCharges = 3;
		Guardian->CastArtifact();
		AWoTMashadarGuide* Guide = Guardian->CurrentGuide.Get();
		if (TestNotNull(TEXT("real multiplayer Guardian cast spawns a tendril"), Guide))
		{
			TestEqual(TEXT("non-standalone Guardian uses retail MPMaxSpeed"),
				Guide->MaxSpeed, 400.0f);
		}
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarArenaHealthBoundsSpec,
	"WoT.Parity.F2.MashadarArenaSpawner.HealthUpperBoundExclusive", F2_Flags)
bool FWoTF2MashadarArenaHealthBoundsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	WoTRandom::Seed(0x7051);
	AWoTMashadarGuide* MapCrack = F2_SpawnMashadarGuide(*World,
		FVector(2000.0f, 0.0f, 100.0f), 1, 1000.0f, true);
	if (TestNotNull(TEXT("map-authored citadel crack"), MapCrack))
	{
		TestEqual(TEXT("an unclaimed map crack consumes no Mashadar health roll"),
			MapCrack->MashadarHealth, 0);
	}
	AWoTAngrealInvGuardian* Guardian = F2_CastGuardian(*World);
	AWoTMashadarGuide* Guide = Guardian ? Guardian->CurrentGuide.Get() : nullptr;
	if (TestNotNull(TEXT("real Guardian cast spawns its arena guide"), Guide))
	{
		AWoTCharacter* Enemy = F2_SpawnPlayer(*World,
			Guide->HidingPlaceLocation + Guide->GetActorForwardVector() * 100.0f, 2);
		// Seed 17's first fraction is 0.98836: inclusive integer RandRange would
		// produce 200, while retail float RandRange then truncation produces 199.
		WoTRandom::Seed(17);
		Guide->NotifySeePawn(Enemy);
		TestTrue(TEXT("real first-live chase assigns retail health in [150, 200)"),
			Guide->bActive && Guide->MashadarHealth >= 150
				&& Guide->MashadarHealth < 200);
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarCrackSparkleSpec,
	"WoT.Parity.F2.MashadarCrack.AppearSparkle", F2_Flags)
bool FWoTF2MashadarCrackSparkleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvGuardian* Guardian = F2_CastGuardian(*World);
	AWoTDecal* Crack = Guardian ? Guardian->LastSpawnedCrackForTest.Get() : nullptr;
	AWoTAppearEffect* Sparkle = nullptr;
	for (TActorIterator<AWoTAppearEffect> It(World); It; ++It)
	{
		if (IsValid(*It) && It->AppearTarget == Crack) Sparkle = *It;
	}
	if (TestNotNull(TEXT("real Guardian cast places its crack"), Crack)
		&& TestNotNull(TEXT("crack first appearance spawns AppearEffect"), Sparkle))
	{
		TestEqual(TEXT("crack top sparkle is APurpleCorona"),
			Sparkle->TopSprite, FName(TEXT("APurpleCorona")));
		TestEqual(TEXT("crack bottom sparkle is AWhiteCorona"),
			Sparkle->BottomSprite, FName(TEXT("AWhiteCorona")));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarCrackLateRelevantSparkleSpec,
	"WoT.Parity.F2.MashadarCrack.LateRelevantSparkle", F2_Flags)
bool FWoTF2MashadarCrackLateRelevantSparkleSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("F2 late-relevant Mashadar crack sparkle"));
	if (!TestTrue(TEXT("dedicated server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	const FWoTDecalConfig* Config = WoTDecalFX::Find(TEXT("MashadarCrack"));
	AWoTDecal* ServerCrack = Config ? WoTDecalFX::SpawnAlignedFit(*ServerWorld,
		TEXT("MashadarCrack"), FVector(0,0,5000), FRotator::ZeroRotator,
		Config->DrawScale, FVector::UpVector, true) : nullptr;
	if (!TestNotNull(TEXT("persistent authority crack"), ServerCrack))
	{
		return false;
	}
	ServerCrack->bAlwaysRelevant = true;
	ServerCrack->ForceNetUpdate();
	// The original replicated effect actor would already be gone before this
	// client first receives the still-live crack.
	Worlds.TickAll(180);
	if (!TestTrue(TEXT("late presentation client connects"),
		Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTDecal* ClientCrack = nullptr;
	AWoTAppearEffect* ClientSparkle = nullptr;
	TestTrue(TEXT("late client creates its first-tick local sparkle from the crack"),
		Worlds.TickUntil([&]()
		{
			for (TActorIterator<AWoTDecal> It(Worlds.Clients[0].GetWorld()); It; ++It)
			{
				if (It->DecalType == TEXT("MashadarCrack")) ClientCrack = *It;
			}
			for (TActorIterator<AWoTAppearEffect> It(
				Worlds.Clients[0].GetWorld()); It; ++It)
			{
				if (It->AppearTarget == ClientCrack) ClientSparkle = *It;
			}
			return ClientCrack && ClientSparkle;
		}, 180));
	if (ClientSparkle)
	{
		TestFalse(TEXT("late sparkle is local rather than a stale replicated channel"),
			ClientSparkle->GetIsReplicated());
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MinionEyesDeathResetSpec,
	"WoT.Parity.F2.Minion.DeathResetsEyes", F2_Flags)
bool FWoTF2MinionEyesDeathResetSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Minion = F2_SpawnCombatNPC(*World, TEXT("Minion"),
		FVector(0.0f, 0.0f, 100.0f));
	UWoTVertexAnimComponent* Body = Minion
		? Cast<UWoTVertexAnimComponent>(Minion->GetMesh()) : nullptr;
	WoTTest::TickWorld(World, 0.05f);
	const FSoftObjectPath* LitEyes = Body
		? Body->GetRuntimeSkinOverrides().Find(4) : nullptr;
	if (TestTrue(TEXT("real Minion blink turns the eye skin on"), LitEyes
		&& LitEyes->ToString().Contains(TEXT("MinionEyes"))))
	{
		Minion->Health = 10;
		FWoTDamageEvent Damage;
		Damage.WoTDamageType = TEXT("Sliced");
		Minion->TakeDamage(11.0f, Damage, nullptr, Minion);
		const FSoftObjectPath* DeadEyes = Body->GetRuntimeSkinOverrides().Find(4);
		TestTrue(TEXT("real Died path restores the authored eyes-off skin"),
			!DeadEyes || !DeadEyes->ToString().Contains(TEXT("MinionEyes")));
		AWoTCarcass* Carcass = nullptr;
		TestTrue(TEXT("death animation reaches the real carcass transition"),
			F2_TickUntil(World, 5.0f, [&]()
			{
				for (TActorIterator<AWoTCarcass> It(World); It; ++It)
				{
					if (IsValid(*It) && It->CarcassTypeName.ToString().Contains(
						TEXT("Minion"))) Carcass = *It;
				}
				return Carcass != nullptr;
			}));
		if (Carcass)
		{
			bool bCarcassEyesLit = false;
			for (int32 Index = 0; Index < Carcass->MeshSkinIndices.Num(); ++Index)
			{
				bCarcassEyesLit |= Carcass->MeshSkinIndices[Index] == 4
					&& Carcass->MeshSkinPaths.IsValidIndex(Index)
					&& Carcass->MeshSkinPaths[Index].ToString().Contains(TEXT("MinionEyes"));
			}
			TestFalse(TEXT("carcass does not copy glowing Minion eyes"), bCarcassEyesLit);
		}
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2LegionCarcassSinkSpec,
	"WoT.Parity.F2.LegionCarcass.LandedSinkRate", F2_Flags)
bool FWoTF2LegionCarcassSinkSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	F2_SpawnFloor(*World, FVector(0.0f, 0.0f, 0.0f));
	F2_SpawnPlayer(*World, FVector(300.0f, 0.0f, 100.0f));
	AWoTNPCCharacter* Legion = F2_SpawnCombatNPC(*World, TEXT("Legion"),
		FVector(0.0f, 0.0f, 100.0f));
	Legion->Health = 10;
	FWoTDamageEvent Damage;
	Damage.WoTDamageType = TEXT("Sliced");
	Legion->TakeDamage(20.0f, Damage, nullptr, Legion);
	AWoTCarcass* Carcass = nullptr;
	TestTrue(TEXT("real Legion death reaches its configured carcass"),
		F2_TickUntil(World, 6.0f, [&]()
		{
			for (TActorIterator<AWoTCarcass> It(World); It; ++It)
			{
				if (IsValid(*It) && It->CarcassTypeName.ToString().Contains(
					TEXT("Legion"))) Carcass = *It;
			}
			return Carcass != nullptr && Carcass->IsLyingStill();
		}));
	if (Carcass)
	{
		const FVector Before = Carcass->GetActorLocation();
		WoTTest::TickWorld(World, 0.5f);
		const float Drop = Before.Z - Carcass->GetActorLocation().Z;
		TestTrue(TEXT("landed Legion carcass sinks at perturbed default rate"),
			Drop >= 0.75f && Drop <= 2.25f);
		TestEqual(TEXT("sinking corpse disables collision"),
			Carcass->Capsule->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
		TestFalse(TEXT("sinking corpse is no longer a projectile target"),
			Carcass->IsRetailProjectileTarget());
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarPerTickLeashSpec,
	"WoT.Parity.F2.Mashadar.PerTickLeash", F2_Flags)
bool FWoTF2MashadarPerTickLeashSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTMashadarGuide* Guide = F2_SpawnMashadarGuide(*World,
		FVector(0,0,100), 1, 300.0f, false);
	AWoTCharacter* Player = F2_SpawnPlayer(*World, FVector(100,0,100), 2);
	TestTrue(TEXT("normal player dispatch acquires a nearby enemy"),
		F2_TickUntil(World, 1.0f, [&]() { return Guide->Destination == Player; }));
	Player->SetActorLocation(FVector(500,0,100));
	WoTTest::TickWorld(World, 1, 1.0f/120.0f);
	TestNull(TEXT("one seeking tick drops a target outside the lair leash"),
		Guide->Destination.Get());
	TestFalse(TEXT("lost target deactivates the tendril immediately"), Guide->bActive);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarSharedPoolSpec,
	"WoT.Parity.F2.Mashadar.SharedTeamPool", F2_Flags)
bool FWoTF2MashadarSharedPoolSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTMashadarGuide* First = F2_SpawnMashadarGuide(*World,
		FVector(0,0,100), 1, 500.0f, true);
	AWoTMashadarGuide* Second = F2_SpawnMashadarGuide(*World,
		FVector(1500,0,100), 1, 500.0f, true);
	First->Deploy();
	AWoTCharacter* FirstTarget = F2_SpawnPlayer(*World, FVector(100,0,100), 2);
	TestTrue(TEXT("deployed crack creates and claims a team slot"),
		F2_TickUntil(World, 1.0f, [&]() { return First->bActive; }));
	const int32 InitialHealth = First->MashadarHealth;
	FWoTDamageEvent Damage;
	Damage.WoTDamageType = TEXT("Sliced");
	if (AWoTMashadarSegment* Segment = First->GetMasterSegment())
	{
		Segment->TakeDamage(100.0f, Damage, nullptr, FirstTarget);
	}
	TestEqual(TEXT("real segment damage drains the claimed slot"),
		First->MashadarHealth, InitialHealth - 100);
	FirstTarget->SetActorLocation(FVector(900,0,100));
	TestTrue(TEXT("leash releases the slot"), F2_TickUntil(World, 1.0f,
		[&]() { return !First->bActive; }));
	AWoTCharacter* SecondTarget = F2_SpawnPlayer(*World, FVector(1600,0,100), 2);
	TestTrue(TEXT("an undeployed same-team crack claims the freed slot"),
		F2_TickUntil(World, 1.0f, [&]() { return Second->Destination == SecondTarget; }));
	TestEqual(TEXT("the shared slot carries its damaged health to the new crack"),
		Second->MashadarHealth, InitialHealth - 100);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarDeploymentAddsPoolLifeSpec,
	"WoT.Parity.F2.Mashadar.EachDeploymentAddsPoolLife", F2_Flags)
bool FWoTF2MashadarDeploymentAddsPoolLifeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	auto ActivateThenRelease = [&](float X)
	{
		AWoTMashadarGuide* Guide = F2_SpawnMashadarGuide(*World,
			FVector(X,0,100), 1, 250.0f, true);
		Guide->Deploy();
		AWoTCharacter* Target = F2_SpawnPlayer(*World, FVector(X + 100,0,100), 2);
		const bool bActivated = F2_TickUntil(World, 1.0f,
			[&]() { return Guide->bActive; });
		Target->Destroy();
		const bool bReleased = F2_TickUntil(World, 1.0f,
			[&]() { return !Guide->bActive; });
		return bActivated && bReleased;
	};
	TestTrue(TEXT("first deployed focus adds then releases one team life"),
		ActivateThenRelease(0.0f));
	TestTrue(TEXT("second deployed focus adds its own life before claiming"),
		ActivateThenRelease(1000.0f));

	AWoTMashadarGuide* FirstCrack = F2_SpawnMashadarGuide(*World,
		FVector(2000,0,100), 1, 250.0f, true);
	AWoTMashadarGuide* SecondCrack = F2_SpawnMashadarGuide(*World,
		FVector(3000,0,100), 1, 250.0f, true);
	F2_SpawnPlayer(*World, FVector(2100,0,100), 2);
	F2_SpawnPlayer(*World, FVector(3100,0,100), 2);
	TestTrue(TEXT("two undeployed cracks can claim the two released lives together"),
		F2_TickUntil(World, 1.0f,
			[&]() { return FirstCrack->bActive && SecondCrack->bActive; }));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarPainDeathSpec,
	"WoT.Parity.F2.Mashadar.PainDeathBroadcastAndFocusConsumed", F2_Flags)
bool FWoTF2MashadarPainDeathSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTBattleGameMode* Battle = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTNPCCharacter* Focus = F2_SpawnCombatNPC(*World, TEXT("MashadarFocusPawn"),
		FVector(-300,0,100), 1);
	const FString FocusDisplayName = Focus ? Focus->SummonDisplayName : FString();
	TestFalse(TEXT("focus has a runtime-localized synthetic victim name"),
		FocusDisplayName.IsEmpty());
	Battle->RegisterCitadelDeployedTroop(Focus, 1);
	AWoTMashadarGuide* Guide = F2_SpawnMashadarGuide(*World,
		FVector(0,0,100), 1, 500.0f, true);
	Battle->SetCitadelTroopMashadarGuide(Focus, Guide);
	Guide->Deploy();
	AWoTTestMessageCaptureController* KillerController = nullptr;
	AWoTCharacter* Killer = F2_SpawnCapturePlayer(*World, FVector(100,0,100),
		2, TEXT("Rand"), KillerController);
	TestTrue(TEXT("real focus-bound guide activates"), F2_TickUntil(World, 1.0f,
		[&]() { return Guide->bActive && Guide->GetMasterSegment(); }));
	FWoTDamageEvent Damage;
	Damage.WoTDamageType = TEXT("Sliced");
	auto Hurt = [&](float Amount)
	{
		Guide->GetMasterSegment()->TakeDamage(Amount, Damage, KillerController, Killer);
		WoTTest::TickWorld(World, 1, 1.0f/120.0f);
	};
	F2_DestroySoundMarkers(World);
	Hurt(10.0f);
	TestTrue(TEXT("soft hit plays HitSoft"),
		F2_CountSoundMarkers(World, TEXT("HitSoft")) > 0);
	Hurt(25.0f);
	TestTrue(TEXT("25-point hit plays HitHard"),
		F2_CountSoundMarkers(World, TEXT("HitHard")) > 0);
	const int32 Remaining = Guide->MashadarHealth;
	Hurt(static_cast<float>(Remaining));
	TestTrue(TEXT("post-damage health at or below 25 plays DieHard"),
		F2_CountSoundMarkers(World, TEXT("DieHard")) > 0);
	TestFalse(TEXT("lethal tendril hit consumes its citadel focus"), IsValid(Focus));
	TestEqual(TEXT("synthetic focus identity records its retail death"),
		Guide->DefenderDeaths, 1);
	AWoTPlayerState* KillerState = KillerController->GetPlayerState<AWoTPlayerState>();
	TestEqual(TEXT("enemy killer receives one defender frag"), KillerState->Kills, 1);
	TestTrue(TEXT("kill broadcasts the focus's runtime victim name as a typed death message"),
		KillerController->CapturedTypes.Contains(TEXT("WOTDeathMessage"))
		&& KillerController->CapturedMessages.ContainsByPredicate([&](const FString& Message)
		{
			return Message.Contains(FocusDisplayName)
				&& Message.Contains(TEXT("Rand"));
		}));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MashadarFocusInstigatorSpec,
	"WoT.Parity.F2.Mashadar.FocusInstigatorScoresVictim", F2_Flags)
bool FWoTF2MashadarFocusInstigatorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTBattleGameMode* Battle = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTNPCCharacter* Focus = F2_SpawnCombatNPC(*World, TEXT("MashadarFocusPawn"),
		FVector(-300,0,100), 1);
	const FString FocusDisplayName = Focus ? Focus->SummonDisplayName : FString();
	Battle->RegisterCitadelDeployedTroop(Focus, 1);
	AWoTMashadarGuide* Guide = F2_SpawnMashadarGuide(*World,
		FVector(0,0,100), 1, 500.0f, true);
	Battle->SetCitadelTroopMashadarGuide(Focus, Guide);
	Guide->Deploy();
	AWoTTestMessageCaptureController* VictimController = nullptr;
	AWoTCharacter* Victim = F2_SpawnCapturePlayer(*World, FVector(100,0,100),
		2, TEXT("BiteVictim"), VictimController);
	Victim->Health = 1;
	TestTrue(TEXT("focus-bound tendril activates through real sight and pool claim"),
		F2_TickUntil(World, 1.0f,
			[&]() { return Guide->bActive && Guide->GetMasterSegment(); }));
	TestEqual(TEXT("activation assigns the first eligible team focus as instigator"),
		Guide->GetInstigator(), static_cast<APawn*>(Focus));
	if (AWoTMashadarSegment* Segment = Guide->GetMasterSegment())
	{
		Segment->BiteTarget(Victim);
	}
	AWoTPlayerState* VictimState = VictimController->GetPlayerState<AWoTPlayerState>();
	TestEqual(TEXT("focus-instigated bite records an ordinary victim death"),
		VictimState->Deaths, 1);
	TestEqual(TEXT("focus-instigated bite is not an environment suicide"),
		VictimState->Suicides, 0);
	TestTrue(TEXT("typed death message names the runtime-localized focus killer"),
		VictimController->CapturedMessages.ContainsByPredicate(
			[&](const FString& Message) { return Message.Contains(FocusDisplayName); }));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2MyrddraalSnipeSpec,
	"WoT.Parity.F2.Myrddraal.SnipeSelectsValidPerch", F2_Flags)
bool FWoTF2MyrddraalSnipeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* Myrddraal = F2_SpawnCombatNPC(*World, TEXT("Myrddraal"),
		FVector(0.0f, 0.0f, 100.0f));
	if (Myrddraal && Myrddraal->GetController())
	{
		AController* OldController = Myrddraal->GetController();
		OldController->UnPossess();
		OldController->Destroy();
	}
	AWoTTestMovementObservationController* Controller = Myrddraal
		? World->SpawnActor<AWoTTestMovementObservationController>() : nullptr;
	if (Controller) Controller->Possess(Myrddraal);
	if (TestNotNull(TEXT("real Myrddraal NPC controller"), Controller))
	{
		FWoTMovementPattern Pattern;
		Pattern.ClassIdentity = TEXT("F2SnipeFixture");
		Pattern.Elements[0].bEnabled = true;
		Pattern.Elements[0].PreHint = TEXT("Snipe");
		Pattern.Elements[0].Waypoint.bRequireReachable = false;
		Controller->SetPatternFixture(Pattern);
		auto& Notifier = Myrddraal->Brain->Notifiers[
			static_cast<int32>(EWoTNotifierSlot::Offensive)];
		Notifier.Period = 1000.0f;
		Notifier.Elapsed = 0.0f;
		Notifier.bRandomizePhase = false;

		AWoTPathNode* Perch = World->SpawnActor<AWoTPathNode>(
			FVector(600.0f, 400.0f, 100.0f), FRotator::ZeroRotator);
		if (Perch)
		{
			Perch->bSnipe = true;
			Perch->SourceClass = TEXT("WOT.WotPathNode");
		}
		F2_SpawnPlayer(*World, FVector(1000.0f, 0.0f, 100.0f));
		TestTrue(TEXT("real tracking executes the Myrddraal Snipe hint"),
			F2_TickUntil(World, 2.0f, [&]
			{
				return Controller->GetMovementManager().States.Contains(21);
			}));
		TestEqual(TEXT("valid flagged visible perch becomes Intermediate"),
			Myrddraal->Brain->GetGoal(EWoTGoalIndex::Intermediate).GoalActor.Get(),
			static_cast<AActor*>(Perch));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTF2WarderRespectFadeSpec,
	"WoT.Parity.F2.Warder.FriendlyPlayerUncloaksAndSalutes", F2_Flags)
bool FWoTF2WarderRespectFadeSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTNPCCharacter* Warder = F2_SpawnCombatNPC(*World, TEXT("Warder"),
		FVector(0.0f, 0.0f, 100.0f), 1);
	AWoTCharacter* FriendlyPlayer = F2_SpawnPlayer(*World,
		FVector(150.0f, 0.0f, 100.0f), 1);
	UWoTWarderFadeCloakBehavior* Fade = Warder
		? Warder->FindComponentByClass<UWoTWarderFadeCloakBehavior>() : nullptr;
	if (TestNotNull(TEXT("Warder fade behavior"), Fade))
	{
		TestTrue(TEXT("friendly player starts the real ReactP respect path and fade-in"),
			F2_TickUntil(World, 2.0f, [&]
			{
				return Warder->GetReplicatedAnimName() == TEXT("ReactP")
					&& FMath::IsNearlyEqual(Fade->GetActiveFadeChangeTime(),
						Fade->FadeInChangeTime);
			}));
		FriendlyPlayer->SetActorLocation(FVector(1200.0f, 0.0f, 100.0f));
		TestTrue(TEXT("leaving cower range returns waiting camouflage fade-out"),
			F2_TickUntil(World, 2.0f, [&]
			{
				return FMath::IsNearlyEqual(Fade->GetActiveFadeChangeTime(),
					Fade->FadeOutChangeTime);
			}));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
