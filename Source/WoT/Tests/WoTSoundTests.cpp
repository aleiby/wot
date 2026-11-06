// Automation specs for the sound layer: the SoundTable/timer-list
// registries, selection + game-type filtering, the networked one-shot marker, and
// the pawn voice call sites (death cry / pain). Needs imported voice banks.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTAmbientSound.h"
#include "../WoTCarcass.h"
#include "../WoTMover.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTZoneVolume.h"
#include "../Settings/WoTGameSettings.h"
#include "../Inventory/WoTAngrealInvHeal.h"
#include "../Inventory/WoTAngrealInvWallOfAir.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../WoTCharacter.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "WoTTestAIClasses.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/BoxComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

namespace
{
	// The last-spawned live sound marker's subpath (NAME_None if none).
	FName LastMarker(UWorld* World)
	{
		FName Found = NAME_None;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				Found = It->SoundSubPath;
			}
		}
		return Found;
	}

	int32 CountMarkers(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Count;
			}
		}
		return Count;
	}

	void TickSoundWorld(UWorld* World, int32 Frames = 2)
	{
		WoTTest::TickWorld(World, Frames, 0.02f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAngrealReplicatedCastSoundSpec,
	"WoT.Sound.AngrealReplicatedCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAngrealReplicatedCastSoundSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* OwnerPawn = World->SpawnActor<AWoTCharacter>(
		AWoTCharacter::StaticClass(), FVector(0, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	AWoTAngrealInvHeal* Heal = World->SpawnActor<AWoTAngrealInvHeal>(
		AWoTAngrealInvHeal::StaticClass(), FVector(0, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("heal owner spawned"), OwnerPawn)
		|| !TestNotNull(TEXT("heal angreal spawned"), Heal))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	OwnerPawn->Health = 50;
	Heal->SetOwner(OwnerPawn);
	Heal->CastArtifact();
	TestEqual(TEXT("CastArtifact spawns the replicated ActivateHL marker"),
		LastMarker(World), FName(TEXT("Angreal/ActivateHL")));

	AWoTAngrealInvWallOfAir* Wall = World->SpawnActor<AWoTAngrealInvWallOfAir>(
		AWoTAngrealInvWallOfAir::StaticClass(), FVector(0, 0, 100),
		FRotator::ZeroRotator, SpawnParams);
	if (TestNotNull(TEXT("Wall of Air angreal spawned"), Wall))
	{
		Wall->SetOwner(OwnerPawn);
		// Exercise the base seam directly; Wall of Air's virtual override only
		// forwards while an actual wall cast is active.
		Wall->AWoTAngrealInventory::UnCastArtifact();
		TestEqual(TEXT("UnCastArtifact spawns the replicated DeActivateWA marker"),
			LastMarker(World), FName(TEXT("Angreal/DeActivateWA")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTZoneHealSoundSpec,
	"WoT.Sound.ZoneHeal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTZoneHealSoundSpec::RunTest(const FString&)
{
	auto SpawnHealFixture = [](UWorld* World, bool bPlayerControlled, int32 Health)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTZoneVolume* Zone = World->SpawnActorDeferred<AWoTZoneVolume>(
			AWoTZoneVolume::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Zone->bHealZone = true;
		Zone->HealPerSec = 5;
		Zone->HealSoundPath = FName(TEXT("Angreal/ActivateHL"));
		Zone->Collision->SetBoxExtent(FVector(200.0f), false);
		Zone->FinishSpawning(FTransform::Identity);

		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			AWoTCharacter::StaticClass(), FVector(0, 0, 100),
			FRotator::ZeroRotator, SpawnParams);
		Pawn->Health = Health;
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		if (bPlayerControlled)
		{
			APlayerController* Controller = World->SpawnActor<APlayerController>(
				APlayerController::StaticClass(), FVector::ZeroVector,
				FRotator::ZeroRotator, SpawnParams);
			Controller->Possess(Pawn);
		}
		return Pawn;
	};

	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTCharacter* Pawn = SpawnHealFixture(World, true, 50);
		TickSoundWorld(World);
		TestTrue(TEXT("damaged player heals on the armed zone tick"), Pawn->Health > 50);
		TestEqual(TEXT("damaged player heal spawns ActivateHL marker"),
			LastMarker(World), FName(TEXT("Angreal/ActivateHL")));
		WoTTest::DestroySpecWorld(World);
	}

	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTCharacter* Pawn = SpawnHealFixture(World, true, 100);
		TickSoundWorld(World);
		TestEqual(TEXT("full-health player stays full"), Pawn->Health, 100);
		TestEqual(TEXT("full-health player produces no heal marker"),
			CountMarkers(World), 0);
		WoTTest::DestroySpecWorld(World);
	}

	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTCharacter* Pawn = SpawnHealFixture(World, false, 50);
		TickSoundWorld(World);
		TestTrue(TEXT("controller-less NPC still heals"), Pawn->Health > 50);
		TestEqual(TEXT("controller-less NPC produces no heal marker"),
			CountMarkers(World), 0);
		WoTTest::DestroySpecWorld(World);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundTableSpec,
	"WoT.Sound.Table", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSoundTableSpec::RunTest(const FString&)
{
	// --- Registries: all 23 tables + 9 timer lists, verbatim spot-checks. ---
	for (const TCHAR* Name : {TEXT("Trolloc"), TEXT("Soldier"), TEXT("Myrddraal"),
	                          TEXT("AesSedai"), TEXT("Whitecloak"), TEXT("Mashadar2")})
	{
		TestNotNull(FString::Printf(TEXT("sound table %s present"), Name),
		            WoTSoundTable::Find(FName(Name)));
	}
	const TArray<FWoTSoundTableEntry>* Trolloc = WoTSoundTable::Find(TEXT("Trolloc"));
	if (TestNotNull(TEXT("Trolloc table"), Trolloc))
	{
		bool bSawBreath = false, bSawRespect = false;
		for (const FWoTSoundTableEntry& E : *Trolloc)
		{
			if (E.Slot == FName(TEXT("Breath")))
			{
				bSawBreath = true;
				TestEqual(TEXT("Breath radius 768 (SoundTableTrolloc.uc)"), E.Radius, 768.0f);
			}
			if (E.Slot == FName(TEXT("ShowRespect")))
			{
				bSawRespect = true;
				TestEqual(TEXT("ShowRespect odds 0.05 (single-entry probability)"),
				          E.Odds, 0.05f);
			}
		}
		TestTrue(TEXT("Breath + ShowRespect rows present"), bSawBreath && bSawRespect);
	}
	const TArray<FWoTSoundSlotTimer>* Timers = WoTSoundTable::FindTimers(TEXT("Trolloc"));
	if (TestNotNull(TEXT("Trolloc timer list"), Timers))
	{
		bool bSawAcquired = false;
		for (const FWoTSoundSlotTimer& T : *Timers)
		{
			if (T.Slot == FName(TEXT("Acquired")))
			{
				bSawAcquired = true;
				TestEqual(TEXT("Acquired odds 0.01"), T.Odds, 0.01f);
				TestTrue(TEXT("Acquired bReset flips odds->timed"), T.bReset);
			}
		}
		TestTrue(TEXT("Acquired timer row present"), bSawAcquired);
	}
	TestNull(TEXT("pawns without a timer list are uncontrolled"),
	         WoTSoundTable::FindTimers(TEXT("Minion")));

	UWorld* World = WoTTest::CreateSpecWorld();

	// --- Game-type filter: Trolloc 'Acquired' carries one GT_SinglePlayer entry
	// (Tro_Acquired2) that must NEVER pick in the MP port. ---
	for (int32 i = 0; i < 40; ++i)
	{
		if (const FWoTSoundTableEntry* Pick =
		        WoTSoundTable::PickSlotSound(TEXT("Trolloc"), TEXT("Acquired"), World))
		{
			TestTrue(TEXT("GT_SinglePlayer entries filter out in MP"),
			         Pick->SoundSubPath != FName(TEXT("TrollocA/Tro_Acquired2")));
		}
	}

	// --- The imported bank resolves (5a). ---
	TestNotNull(TEXT("TrollocA voice bank imported (run import_sounds.py)"),
	            Cast<USoundBase>(FSoftObjectPath(
	                TEXT("/Game/WoT/Sounds/TrollocA/Tro_DieHard1.Tro_DieHard1")).TryLoad()));

	// --- PlaySlotSound end-to-end on a row-bound pawn. ---
	AWoTNPCCharacter* Pawn = WoTCreatureData::SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Trolloc spawned"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	TestEqual(TEXT("ConfigureNPC bound the voice table"),
	          Pawn->SoundTableName, FName(TEXT("Trolloc")));

	TestTrue(TEXT("an uncontrolled slot plays"), Pawn->PlayVoiceSlot(TEXT("DieHard")));
	TestEqual(TEXT("the networked marker carries the picked wave"),
	          LastMarker(World), FName(TEXT("TrollocA/Tro_DieHard1")));

	// Spawn-time randomization blocks the timed WaitingRandom slot until its
	// initial 60-180s deadline. Force that deadline due, then verify the next
	// successful play schedules a fresh cooldown.
	TestFalse(TEXT("WaitingRandom initial delay is seeded"),
	          Pawn->PlayVoiceSlot(TEXT("WaitingRandom")));
	Pawn->SoundSlotNextTime.Add(TEXT("WaitingRandom"), 0.0f);
	TestTrue(TEXT("WaitingRandom plays after its deadline"),
	         Pawn->PlayVoiceSlot(TEXT("WaitingRandom")));
	TestFalse(TEXT("WaitingRandom second play is cooldown-gated"),
	          Pawn->PlayVoiceSlot(TEXT("WaitingRandom")));

	// --- The pawn call sites: pain + death cry. ---
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Sliced");
	Pawn->NextPainSoundTime = 0.0f;
	Pawn->TakeDamage(20.0f, Ev, nullptr, nullptr);   // soft: 20 < 0.25 x 150
	TestEqual(TEXT("a surviving soft hit cries HitSoft"),
	          LastMarker(World).ToString().Contains(TEXT("Tro_HitSoft")), true);

	// --- 5d: the loop-notify scheduler — a running pawn drops footsteps at the
	// authored PlayMovementSound fractions (Run @0.33/0.87). No floor under it,
	// so the texture-keyed pass resolves the ConvertTexture Stone fallback
	// (WOT/Stone* waves; the full keying is spec'd in WoT.Footstep.Steps). ---
	if (AController* C = Pawn->GetController()) { C->UnPossess(); }
	int32 Steps = 0;
	for (int32 i = 0; i < 240; ++i)   // 4s of running
	{
		Pawn->GetCharacterMovement()->Velocity = FVector(300, 0, 0);
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		GFrameCounter++;
	}
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		if (IsValid(*It) && It->SoundSubPath.ToString().StartsWith(TEXT("WOT/Stone")))
		{
			++Steps;
		}
	}
	TestTrue(FString::Printf(TEXT("running drops footsteps on the loop cadence (got %d)"),
	                         Steps),
	         Steps >= 1);

	// A mild overkill (-10 > GibSometimes -30) dies with the SOFT cry (not
	// exceptional: -10 > -0.25 x 150); Trolloc's DieSoft reuses Tro_DieHard2.
	Pawn->TakeRawDamage(Pawn->Health + 10, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	TestTrue(TEXT("the death cry fired"),
	         LastMarker(World).ToString().Contains(TEXT("Tro_DieHard")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The UE1 ambient-loop foundation — byte-param math on the component, the
// placed-emitter actor resolving + configuring at BeginPlay, and the
// DynamicAmbientSound keypoint's shipped pick/schedule semantics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundAmbientSpec,
	"WoT.Sound.Ambient", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSoundAmbientSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// --- Placed emitter: BeginPlay resolves the wave and applies the UE1 math. ---
	AWoTAmbientSound* Emitter = World->SpawnActorDeferred<AWoTAmbientSound>(
	    AWoTAmbientSound::StaticClass(), FTransform::Identity);
	Emitter->SoundSubPath = FName(TEXT("FootStubA/STON"));
	Emitter->SoundRadius = 64;
	Emitter->SoundVolume = 190;
	Emitter->SoundPitch = 128;
	Emitter->LightType = 1;
	Emitter->LightBrightness = 128;
	Emitter->FinishSpawning(FTransform::Identity);

	TestNotNull(TEXT("emitter resolved its wave"), Emitter->Ambient->Sound.Get());
	TestEqual(TEXT("steady-state volume 2*0.7*(V/255)*light"),
	          Emitter->Ambient->VolumeMultiplier,
	          2.0f * WoTSound::AmbientFactor * (190.0f / 255.0f) * (128.0f / 255.0f),
	          KINDA_SMALL_NUMBER);
	Emitter->LightType = 2;
	Emitter->LightBrightness = 255;
	Emitter->LightPeriod = 32;
	Emitter->LightPhase = 0;
	Emitter->UpdateLightVolumeAtTime(0.0f, 0);
	const float PulseAtZero = Emitter->Ambient->BaseVolumeMultiplier;
	Emitter->UpdateLightVolumeAtTime(8.0f / 35.0f, 8);
	TestNotEqual(TEXT("light-carried ambient follows the live Pulse waveform"),
		Emitter->Ambient->BaseVolumeMultiplier, PulseAtZero);
	TestEqual(TEXT("pitch byte/64"), Emitter->Ambient->PitchMultiplier, 2.0f,
	          KINDA_SMALL_NUMBER);
	if (TestNotNull(TEXT("linear attenuation set"),
	                Emitter->Ambient->AttenuationSettings.Get()))
	{
		const FSoundAttenuationSettings& S =
		    Emitter->Ambient->AttenuationSettings->Attenuation;
		TestEqual(TEXT("falloff = 25*(r+1)"), S.FalloffDistance, 1625.0f,
		          KINDA_SMALL_NUMBER);
		TestTrue(TEXT("linear model"),
		         S.DistanceAlgorithm == EAttenuationDistanceModel::Linear);
	}

	// An unset subpath stays silent without crashing.
	AWoTAmbientSound* Empty = World->SpawnActor<AWoTAmbientSound>(
	    AWoTAmbientSound::StaticClass(), FTransform::Identity);
	TestNull(TEXT("no subpath -> no sound"), Empty->Ambient->Sound.Get());

	// --- DynamicAmbientSound keypoint: shipped count/pick/schedule semantics. ---
	AWoTDynamicAmbientSound* Dyn = World->SpawnActorDeferred<AWoTDynamicAmbientSound>(
	    AWoTDynamicAmbientSound::StaticClass(), FTransform::Identity);
	Dyn->SoundSubPaths = {FName(TEXT("FootStubA/STON")), FName(TEXT("FootStubA/STON")),
	                      NAME_None, FName(TEXT("FootStubA/STON"))};
	Dyn->bDontRepeat = true;
	Dyn->bInitiallyOn = true;
	Dyn->FinishSpawning(FTransform::Identity);

	TestEqual(TEXT("count stops at the first empty slot"), Dyn->CountSounds(), 2);
	TestNotEqual(TEXT("bInitiallyOn played at BeginPlay"), Dyn->LastSound, -1);
	const float Remaining = World->GetTimerManager().GetTimerRemaining(Dyn->RecheckTimer);
	TestTrue(FString::Printf(TEXT("recheck scheduled in (0, max] (got %f)"), Remaining),
	         Remaining > 0.0f && Remaining <= Dyn->MaxReCheckTime + KINDA_SMALL_NUMBER);
	Dyn->LastSound = 0;
	for (int32 i = 0; i < 50; ++i)
	{
		TestEqual(TEXT("bDontRepeat never re-picks the last sound"),
		          Dyn->PickSoundIndex(2), 1);
		Dyn->LastSound = 0;
	}

	// --- Mover sounds (8b): edge one-shots + the move loop off the phase FSM. ---
	const FTransform MX(FRotator::ZeroRotator, FVector(0, 0, 500));
	AWoTMover* M = World->SpawnActorDeferred<AWoTMover>(AWoTMover::StaticClass(), MX);
	M->InitialState = EWoTMoverState::TriggerOpenTimed;
	M->KeyPos = {FVector::ZeroVector, FVector(0, 0, 100)};
	M->KeyRot = {FRotator::ZeroRotator, FRotator::ZeroRotator};
	M->MoveTime = 0.2f;
	M->StayOpenTime = 30.0f;
	M->TagName = TEXT("snd_mv");
	M->OpeningSoundPath = FName(TEXT("FootStubA/STON"));
	M->OpenedSoundPath = FName(TEXT("FootStubA/DIRT"));
	M->MoveAmbientSoundPath = FName(TEXT("FootStubA/MARB"));
	M->FinishSpawning(MX);

	M->WoTTrigger(nullptr, nullptr);   // authority: -> Opening (DoOpen)
	TestEqual(TEXT("DoOpen played OpeningSound"), M->LastPhaseSound,
	          M->OpeningSoundPath);
	TestTrue(TEXT("move loop on while opening"), M->bMoveLoopOn);
	TestNotNull(TEXT("move loop configured"), M->MoveAmbient->Sound.Get());
	for (int32 i = 0; i < 20; ++i)   // 0.33s > MoveTime 0.2
	{
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		GFrameCounter++;
	}
	TestEqual(TEXT("FinishedOpening played OpenedSound"), M->LastPhaseSound,
	          M->OpenedSoundPath);
	TestFalse(TEXT("move loop off once Open"), M->bMoveLoopOn);

	// --- Projectile flight hum (8c): the class-default AmbientSound loop rides
	// the shared component with the projectile's own bytes. ---
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
	    AWoTDartProjectile::StaticClass(), FTransform::Identity);
	if (TestNotNull(TEXT("dart flight hum attached"), Dart->AmbientHum.Get()))
	{
		TestNotNull(TEXT("dart hum resolved LoopDT"), Dart->AmbientHum->Sound.Get());
		TestEqual(TEXT("dart hum volume 2*0.7*(V/255)"),
		          Dart->AmbientHum->VolumeMultiplier,
		          2.0f * WoTSound::AmbientFactor * (Dart->SoundVolume / 255.0f),
		          KINDA_SMALL_NUMBER);
	}

	// --- Leech victim hum (8c): lazily attached to the host from Tick, torn
	// down with the leech. ---
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(
	    AWoTCharacter::StaticClass(), FTransform(FVector(0, 0, 300)));
	UWoTWhirlwindLeech* WW = NewObject<UWoTWhirlwindLeech>(Host);
	WW->AttachTo(Host);
	World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	GFrameCounter++;
	TWeakObjectPtr<UWoTAmbientSoundComponent> Hum = WW->AmbientHum;
	if (TestTrue(TEXT("whirlwind hum on the victim"), Hum.IsValid()))
	{
		TestEqual(TEXT("hum lives on the host"), Hum->GetOwner(),
		          static_cast<AActor*>(Host));
		TestNotNull(TEXT("LoopWW resolved"), Hum->Sound.Get());
	}
	WW->Unattach();
	TestTrue(TEXT("hum torn down with the leech"),
	         !Hum.IsValid() || Hum->IsBeingDestroyed());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The Mashadar guide's voice: the actor-generic PlaySlotSound
// overload (the guide is a projectile, not a pawn; no timer list — matching the
// shipped None SoundSlotTimerList) + the Mashadar2 rows the call sites use.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundMashadarVoiceSpec,
	"WoT.Sound.MashadarVoice", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSoundMashadarVoiceSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AActor* Caller = World->SpawnActor<AActor>();

	// MP game-type filter: SeeEnemy2/3 are GT_SinglePlayer rows — the port always
	// picks Mas_SeeEnemy1 (SoundTableWOT.uc ValidForGameType).
	TestTrue(TEXT("the actor-generic overload plays"),
	         WoTSoundTable::PlaySlotSound(*Caller, TEXT("Mashadar2"), TEXT("SeeEnemy")));
	TestEqual(TEXT("MP picks Mas_SeeEnemy1"), LastMarker(World),
	          FName(TEXT("MashadarA/Mas_SeeEnemy1")));

	// The kill taunt: two rows at odds 0.5 each — multi-entry weighted always-fires.
	TestTrue(TEXT("the kill taunt plays"),
	         WoTSoundTable::PlaySlotSound(*Caller, TEXT("Mashadar2"),
	                                      TEXT("MeleeKilledEnemyTaunt")));
	TestTrue(TEXT("a Mas_MeleeKilledEnemyTaunt wave"),
	         LastMarker(World).ToString().Contains(TEXT("Mas_MeleeKilledEnemyTaunt")));

	// The growth creak row (radius 256 authored).
	TestTrue(TEXT("the Breath creak plays"),
	         WoTSoundTable::PlaySlotSound(*Caller, TEXT("Mashadar2"), TEXT("Breath")));
	TestEqual(TEXT("Mas_Breath1"), LastMarker(World), FName(TEXT("MashadarA/Mas_Breath1")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The Legion hum: the creature row's AmbientSound surface —
// replicated path/bytes on the pawn, the local loop lazily attached, and the
// carcass keeping the sound at Actor-default bytes (WOTCarcassLegion.uc:113).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSoundLegionHumSpec,
	"WoT.Sound.LegionHum", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSoundLegionHumSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* Legion = WoTCreatureData::SpawnCreature(
	    *World, TEXT("Legion"), FVector(0, 0, 100), 1, /*bCombatReady*/ true,
	    AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Legion spawned"), Legion))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Legion->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	TestEqual(TEXT("row stamped the hum path"), Legion->AmbientHumPath,
	          FName(TEXT("WoTPawns/Leg_Ambient")));

	// The lazy Tick attach (Legion.uc:524-525: radius byte 255 -> 6400uu falloff,
	// volume 2 * AmbientFactor * 128/255).
	for (int32 i = 0; i < 5; ++i)
	{
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		GFrameCounter++;
	}
	if (TestNotNull(TEXT("pawn hum attached"), Legion->AmbientHum.Get()))
	{
		TestEqual(TEXT("hum volume 2*0.7*(128/255)"),
		          Legion->AmbientHum->VolumeMultiplier,
		          2.0f * WoTSound::AmbientFactor * (128.0f / 255.0f), KINDA_SMALL_NUMBER);
		if (TestNotNull(TEXT("hum attenuation"),
		                Legion->AmbientHum->AttenuationSettings.Get()))
		{
			TestEqual(TEXT("falloff 25*(255+1)"),
			          Legion->AmbientHum->AttenuationSettings->Attenuation.FalloffDistance,
			          6400.0f, KINDA_SMALL_NUMBER);
		}
	}

	// Death: the corpse hands the hum to the carcass at Actor-DEFAULT bytes
	// (WOTCarcassLegion authors no SoundRadius — 25*(32+1)=825uu falloff).
	Legion->TakeRawDamage(Legion->Health + 10, TEXT("Sliced"), nullptr,
	                      FVector::ZeroVector);
	// The swap fires at the death-anim notify — Legion's variants run slow (the
	// gore spec's worst case is ~4.6s for a Trolloc; give the giant 15s).
	for (int32 i = 0; i < 900 && !TActorIterator<AWoTCarcass>(World); ++i)
	{
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
		GFrameCounter++;
	}
	AWoTCarcass* Carc = nullptr;
	for (TActorIterator<AWoTCarcass> It(World); It; ++It)
	{
		if (IsValid(*It)) { Carc = *It; }
	}
	if (TestNotNull(TEXT("carcass spawned"), Carc))
	{
		TestEqual(TEXT("carcass inherits the hum path"), Carc->AmbientHumPath,
		          FName(TEXT("WoTPawns/Leg_Ambient")));
		if (TestNotNull(TEXT("carcass hum attached"), Carc->AmbientHum.Get()))
		{
			TestEqual(TEXT("carcass falloff 25*(32+1)=825 (Actor-default bytes)"),
			          Carc->AmbientHum->AttenuationSettings->Attenuation.FalloffDistance,
			          825.0f, KINDA_SMALL_NUMBER);
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP16HearingGatePrioritySpec,
	"WoT.P16.Audio.HearingGatePriority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP16HearingGatePrioritySpec::RunTest(const FString&)
{
	constexpr float Radius = 1300.0f;
	const float RetailBoundary = Radius / FMath::Sqrt(1.3f);
	TestTrue(TEXT("a listener just inside R/sqrt(1.3) hears the effect"),
		WoTSound::PassesHearingGate(RetailBoundary - 0.01f, Radius));
	TestFalse(TEXT("the retail hearing comparison is strict at R/sqrt(1.3)"),
		WoTSound::PassesHearingGate(RetailBoundary, Radius));
	TestFalse(TEXT("a listener beyond the retail hearing radius is rejected"),
		WoTSound::PassesHearingGate(RetailBoundary + 0.01f, Radius));
	TestFalse(TEXT("a non-positive sound radius never admits an effect"),
		WoTSound::PassesHearingGate(0.0f, 0.0f));

	TestEqual(TEXT("priority is volume times one minus normalized distance"),
		WoTSound::EffectsPriority(250.0f, 1000.0f, 0.8f), 0.6f,
		KINDA_SMALL_NUMBER);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP16DynamicAmbientEffectsPoolSpec,
	"WoT.P16.Audio.DynamicAmbientEffectsPool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP16DynamicAmbientEffectsPoolSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* Controller = World->SpawnActor<APlayerController>(
		APlayerController::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, SpawnParams);
	APawn* Listener = World->SpawnActor<APawn>(
		APawn::StaticClass(), FVector::ZeroVector,
		FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("dynamic ambient listener controller"), Controller)
		|| !TestNotNull(TEXT("dynamic ambient listener pawn"), Listener))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Listener);

	AWoTDynamicAmbientSound* Dynamic =
		World->SpawnActorDeferred<AWoTDynamicAmbientSound>(
			AWoTDynamicAmbientSound::StaticClass(), FTransform::Identity);
	Dynamic->bInitiallyOn = false;
	Dynamic->SoundSubPaths = {FName(TEXT("FootStubA/STON"))};
	Dynamic->SoundRadius = 32;
	Dynamic->SoundVolume = 42;
	Dynamic->FinishSpawning(FTransform::Identity);
	USoundBase* Wave = WoTSound::LoadBySubPath(FName(TEXT("FootStubA/STON")));
	if (!TestNotNull(TEXT("dynamic ambient fixture wave loads"), Wave))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const float Radius = static_cast<float>(Dynamic->SoundRadius) * 25.0f;
	Dynamic->SetActorLocation(
		FVector(Radius / FMath::Sqrt(1.3f) + 1.0f, 0.0f, 0.0f));
	TestEqual(TEXT("prepare rejects DynamicAmbient outside R/sqrt(1.3) before spawn"),
		WoTSound::PrepareEffectForTesting(Dynamic, Wave,
			Dynamic->GetActorLocation(), Radius, 1.0f,
			static_cast<float>(Dynamic->SoundVolume) / 128.0f),
		WoTSound::EEffectAdmissionResult::OutsideHearingRadius);
	TestNull(TEXT("DynamicAmbientSound rejects a chirp outside R/sqrt(1.3)"),
		Dynamic->PlayIndexForTesting(0));
	TestEqual(TEXT("the outside-gate actor reached its chirp attempt"),
		Dynamic->LastSound, 0);

	TArray<WoTSound::FEffectVoicePrioritySample> FullPool;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		FullPool.Add({FVector::ZeroVector, 1000.0f, 1.0f});
	}
	TestEqual(TEXT("headless fixture fills all retail effects channels"),
		WoTSound::SeedEffectsPoolForTesting(*World, FullPool),
		WoTSound::EffectsChannelCount);
	Dynamic->SetActorLocation(FVector(100.0f, 0.0f, 0.0f));
	TestEqual(TEXT("prepare rejects lower-priority DynamicAmbient in full pool"),
		WoTSound::PrepareEffectForTesting(Dynamic, Wave,
			Dynamic->GetActorLocation(), Radius, 1.0f,
			static_cast<float>(Dynamic->SoundVolume) / 128.0f),
		WoTSound::EEffectAdmissionResult::PoolPriorityRejected);
	TestNull(TEXT("DynamicAmbientSound low-priority chirp is rejected by a full pool"),
		Dynamic->PlayIndexForTesting(0));
	TestEqual(TEXT("a rejected DynamicAmbientSound attempt still stamps LastSound"),
		Dynamic->LastSound, 0);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP16EffectsVoiceStealingSpec,
	"WoT.P16.Audio.EffectsVoiceStealing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP16EffectsVoiceStealingSpec::RunTest(const FString&)
{
	TArray<float> FifteenVoices;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount - 1; ++Index)
	{
		FifteenVoices.Add(0.1f + static_cast<float>(Index));
	}
	TestEqual(TEXT("an unoccupied retail effect channel is appended"),
		WoTSound::ChooseEffectsVoice(FifteenVoices, 0.01f), FifteenVoices.Num());

	TArray<float> FullVoices;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		FullVoices.Add(0.2f + 0.1f * static_cast<float>(Index));
	}
	TestEqual(TEXT("the 17th voice steals the lowest-priority channel"),
		WoTSound::ChooseEffectsVoice(FullVoices, 0.9f), 0);
	TestEqual(TEXT("a 17th voice below every active priority is rejected"),
		WoTSound::ChooseEffectsVoice(FullVoices, 0.1f), INDEX_NONE);

	FullVoices[2] = 0.05f;
	FullVoices[11] = 0.05f;
	TestEqual(TEXT("equal lowest priorities preserve Galaxy's last-slot tie rule"),
		WoTSound::ChooseEffectsVoice(FullVoices, 0.05f), 11);

	TArray<WoTSound::FEffectVoicePrioritySample> MovingVoices;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		WoTSound::FEffectVoicePrioritySample& Voice =
			MovingVoices.AddDefaulted_GetRef();
		Voice.SourceLocation = Index == 0
			? FVector::ZeroVector : FVector(900.0f, 0.0f, 0.0f);
		Voice.RadiusUU = 1000.0f;
		Voice.Volume = 1.0f;
	}
	TestEqual(TEXT("listener movement refreshes the formerly-near voice priority"),
		WoTSound::ChooseEffectsVoiceForListener(
			MovingVoices, FVector(900.0f, 0.0f, 0.0f), 0.2f), 0);

	for (WoTSound::FEffectVoicePrioritySample& Voice : MovingVoices)
	{
		Voice.SourceLocation = FVector::ZeroVector;
	}
	MovingVoices[6].SourceLocation = FVector(900.0f, 0.0f, 0.0f);
	TestEqual(TEXT("attached-source movement refreshes its live priority"),
		WoTSound::ChooseEffectsVoiceForListener(
			MovingVoices, FVector::ZeroVector, 0.2f), 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP16UIStackMasterDefaultSpec,
	"WoT.P16.Audio.UIStackMasterDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP16UIStackMasterDefaultSpec::RunTest(const FString&)
{
	TestEqual(TEXT("ClientPlaySound overlaps four retail slots"),
		WoTSound::ClientUISoundVoiceCount, 4);
	TestEqual(TEXT("UE5 receives one normalized gain per retail UI slot"),
		WoTSound::NormalizedUISoundVoiceGain(16.0f), 1.0f,
		KINDA_SMALL_NUMBER);
	TestEqual(TEXT("four normalized slots sum to four effective voices"),
		WoTSound::StackedUISoundEffectiveGain(16.0f), 4.0f,
		KINDA_SMALL_NUMBER);
	TestEqual(TEXT("shipped SoundVolume=200 becomes the default master gain"),
		FWoTGameSettings::Defaults().MasterVolume, 200.0f / 255.0f,
		KINDA_SMALL_NUMBER);
	return true;
}

#endif   // WITH_AUTOMATION_TESTS
