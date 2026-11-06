// Automation specs for water breathing: the head-submersion breath countdown,
// the 5HP/2s drowning loop ('Drowned', killer-less), pain-zone interaction,
// gasp + Drown voice + no-blood, the HUD mirror, and the underwater muffle.
// Spec sources: UE1's Engine.Pawn zone/PainTimer events + level tick (engine
// mechanics; retail Engine is source-stripped), exports/script/WOT/WOTPlayer.uc +
// WOTPawn.uc (WoT overrides + defaults).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS


#include "../WoTAmbientSound.h"
#include "../WoTBubbles.h"
#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTDecal.h"
#include "../WoTHUD.h"
#include "../WoTSound.h"
#include "../WoTZoneVolume.h"
#include "../WoTBloodFX.h"
#include "../WoTSoundTable.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTCreatureData.h"
#include "../Inventory/WoTIgnoreElementReflector.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTBurningChunk.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvExpWard.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/PlayerController.h"
#include "UObject/SoftObjectPath.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	// 1/60s steps: UWorld::Tick clamps big deltas (MaxUndilatedFrameTime).
	void TickWaterWorld(UWorld* World, float Duration)
	{
		const float Step = 1.0f / 60.0f;
		while (Duration > 0.0f)
		{
			const float TickStep = FMath::Min(Step, Duration);
			WoTTest::TickWorld(World, 1, TickStep);
			Duration -= Step;
		}
	}

	template <typename T>
	T* SpawnWaterPawn(UWorld* World, const FVector& Loc)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		T* Pawn = World->SpawnActor<T>(T::StaticClass(), Loc, FRotator::ZeroRotator, Params);
		Pawn->GetCharacterMovement()->bRunPhysicsWithNoController = true;
		return Pawn;
	}

	AWoTZoneVolume* SpawnWaterZone(UWorld* World, const FVector& Loc, const FVector& Extent)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(
		    AWoTZoneVolume::StaticClass(), Loc, FRotator::ZeroRotator, Params);
		Zone->Collision->SetBoxExtent(Extent);
		return Zone;
	}

	// The last live replicated one-shot marker's subpath ("" if none live).
	FString WaterLastMarker(UWorld* World)
	{
		FName Found = NAME_None;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				Found = It->SoundSubPath;
			}
		}
		return Found.ToString();
	}

	int32 CountWaterMarkers(UWorld* World)
	{
		int32 N = 0;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++N;
			}
		}
		return N;
	}

	int32 CountWaterMarkersContaining(UWorld* World, const TCHAR* Text)
	{
		int32 N = 0;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			N += IsValid(*It) && It->SoundSubPath.ToString().Contains(Text) ? 1 : 0;
		}
		return N;
	}

	int32 CountWaterBlood(UWorld* World)
	{
		int32 N = 0;
		for (TActorIterator<AWoTBloodBurst> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++N;
			}
		}
		return N;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterBreathSpec,
	"WoT.Water.Breath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterBreathSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// CDO parity: WOTPlayer.uc:7283 / WOTPawn.uc:1577 (base Engine.Pawn never set it).
	TestEqual(TEXT("player UnderWaterTime 30 (WOTPlayer.uc:7283)"),
	          GetDefault<AWoTCharacter>()->UnderWaterTime, 30.0f);
	TestEqual(TEXT("NPC UnderWaterTime 20 (WOTPawn.uc:1577)"),
	          GetDefault<AWoTNPCCharacter>()->UnderWaterTime, 20.0f);

	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(2000.0f));
	Water->bWaterZone = true;

	AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, FVector::ZeroVector);
	Pawn->UnderWaterTime = 1.0f;   // spec-speed breath
	const int32 HP0 = Pawn->Health;

	// The countdown itself deals nothing.
	TickWaterWorld(World, 0.9f);
	TestEqual(TEXT("no damage during the countdown"), Pawn->Health, HP0);
	// Breath out: the first drown tick (5 HP, Engine.Pawn PainTimer).
	TickWaterWorld(World, 0.2f);
	TestEqual(TEXT("first drown tick = 5 HP"), Pawn->Health, HP0 - 5);
	// The 2.0s cadence (Engine.Pawn PainTimer).
	TickWaterWorld(World, 2.0f);
	TestEqual(TEXT("second drown tick after 2s"), Pawn->Health, HP0 - 10);

	// Surfacing disarms (HeadZoneChange head-exit -> PainTime -1).
	Pawn->SetActorLocation(FVector(50000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 3.0f);
	TestEqual(TEXT("no drown ticks out of water"), Pawn->Health, HP0 - 10);

	// Re-submerging restarts the FULL countdown (fresh PainTime = UnderWaterTime).
	Pawn->SetActorLocation(FVector::ZeroVector);
	TickWaterWorld(World, 0.9f);
	TestEqual(TEXT("fresh breath on re-entry"), Pawn->Health, HP0 - 10);

	// Drowning kills: the killer-less 'Drowned' suicide path (giWOT.ScoreKill).
	Pawn->Health = 4;
	TickWaterWorld(World, 0.3f);
	TestTrue(TEXT("drowned to death"), Pawn->bDead);

	// Shipped creatures mechanically drown too (WOTPawn.uc:1577; same engine loop).
	AWoTNPCCharacter* NPC = SpawnWaterPawn<AWoTNPCCharacter>(World, FVector(0.0f, 300.0f, 0.0f));
	NPC->UnderWaterTime = 0.5f;
	const int32 NPCHP0 = NPC->Health;
	TickWaterWorld(World, 0.8f);
	TestEqual(TEXT("NPC drown tick"), NPC->Health, NPCHP0 - 5);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterBreathPainZoneSpec,
	"WoT.Water.BreathPainZone", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterBreathPainZoneSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// Water everywhere; a flag-only pain zone (DamagePerSec 0) around the origin.
	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(5000.0f));
	Water->bWaterZone = true;
	AWoTZoneVolume* Pain = SpawnWaterZone(World, FVector::ZeroVector, FVector(500.0f));
	Pain->bPainZone = true;   // DamagePerSec stays 0: isolates the breath gates

	AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, FVector::ZeroVector);
	Pawn->UnderWaterTime = 1.0f;
	const int32 HP0 = Pawn->Health;

	// Submerged with the feet in a pain zone: the breath countdown never arms
	// (HeadZoneChange gate) and the drown branch never fires (PainTimer's pain-branch
	// priority) — well past UnderWaterTime.
	TickWaterWorld(World, 2.5f);
	TestEqual(TEXT("no drown damage while the feet are in a pain zone"), Pawn->Health, HP0);

	// Leaving the pain zone underwater: UE1's shared timer keeps cycling, so drowning
	// starts within ~1s WITHOUT a fresh breath (FootZoneChange resets only when
	// the head is dry — the transit-a-pain-zone-underwater quirk).
	Pawn->SetActorLocation(FVector(3000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 1.2f);
	TestEqual(TEXT("drown tick ~1s after leaving the pain zone (no fresh breath)"),
	          Pawn->Health, HP0 - 5);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterSoundsSpec,
	"WoT.Water.Sounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterSoundsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(2000.0f));
	Water->bWaterZone = true;

	// --- Drown voice + no blood on the drown tick (WOTPawn.uc:1002-1005;
	// GenericDamageHelper.uc:247 maps Drowned to NO blood effect). ---
	AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, FVector::ZeroVector);
	Pawn->SoundTableName = FName(TEXT("AesSedai"));   // real Drown/Gasp waves
	Pawn->UnderWaterTime = 0.5f;

	// --- Water shield eats exactly 'drowned' (IgnoreWaterElementReflector.uc:94-105);
	// no other element shield does. Pure fold asserts; the components are outered
	// to a world actor so they die with the world — but NOT to the drowning pawn:
	// an actor-outered component joins its OwnedComponents, and the pawn's own
	// damage chain would then fold the water shield and eat the drown ticks. ---
	UWoTIgnoreWaterElementReflector* WaterShield =
	    NewObject<UWoTIgnoreWaterElementReflector>(Water);
	TestEqual(TEXT("water shield eats Drowned"),
	          WaterShield->ModifyDamage(5, FName(TEXT("Drowned"))), 0);
	TestEqual(TEXT("water shield passes untyped hurt"),
	          WaterShield->ModifyDamage(5, FName(TEXT("hurt"))), 5);
	UWoTIgnoreFireElementReflector* FireShield =
	    NewObject<UWoTIgnoreFireElementReflector>(Water);
	TestEqual(TEXT("fire shield does NOT eat Drowned"),
	          FireShield->ModifyDamage(5, FName(TEXT("Drowned"))), 5);
	TickWaterWorld(World, 0.8f);   // first drown tick at ~0.5s
	TestTrue(TEXT("drown tick voices the Drown slot (Aes_Drown1)"),
	         WaterLastMarker(World).Contains(TEXT("Drown")));
	TestEqual(TEXT("Drowned sheds no blood"), CountWaterBlood(World), 0);
	// Freeze this pawn (UpdateBreath's bDead gate) so its 2s drown cadence and
	// surfacing gasp can't race the marker-count asserts below.
	Pawn->bDead = true;

	// --- Gasp: surfacing with <8s of breath left, players only
	// (HeadZoneChange: bIsPlayer && 0 < PainTime < 8). ---
	AWoTCharacter* Gasper = SpawnWaterPawn<AWoTCharacter>(World, FVector(0.0f, 300.0f, 0.0f));
	Gasper->SoundTableName = FName(TEXT("AesSedai"));
	Gasper->UnderWaterTime = 6.0f;   // any mid-countdown surfacing is in the window
	TickWaterWorld(World, 1.0f);
	Gasper->SetActorLocation(FVector(50000.0f, 300.0f, 0.0f));
	TickWaterWorld(World, 0.1f);
	TestTrue(TEXT("gasp on surfacing inside the window"),
	         WaterLastMarker(World).Contains(TEXT("Gasp")));

	// Above the window (29s left of 30): no gasp.
	AWoTCharacter* Deep = SpawnWaterPawn<AWoTCharacter>(World, FVector(0.0f, 600.0f, 0.0f));
	Deep->SoundTableName = FName(TEXT("AesSedai"));
	TickWaterWorld(World, 1.0f);   // UnderWaterTime stays 30
	const int32 Before = CountWaterMarkers(World);
	Deep->SetActorLocation(FVector(50000.0f, 600.0f, 0.0f));
	TickWaterWorld(World, 0.1f);
	TestEqual(TEXT("no gasp with >8s of breath left"), CountWaterMarkers(World), Before);

	// NPCs never gasp (bIsPlayer is player-only in shipped WoT — WOTPawn.Gasp is
	// unreachable in shipped play).
	AWoTNPCCharacter* NPC = SpawnWaterPawn<AWoTNPCCharacter>(World, FVector(0.0f, 900.0f, 0.0f));
	NPC->SoundTableName = FName(TEXT("Trolloc"));
	NPC->UnderWaterTime = 6.0f;
	TickWaterWorld(World, 1.0f);
	const int32 BeforeNPCGasp = CountWaterMarkersContaining(World, TEXT("Gasp"));
	NPC->SetActorLocation(FVector(50000.0f, 900.0f, 0.0f));
	TickWaterWorld(World, 0.1f);
	TestEqual(TEXT("NPCs never gasp"),
	          CountWaterMarkersContaining(World, TEXT("Gasp")), BeforeNPCGasp);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterHUDBreathSpec,
	"WoT.Water.HUDBreath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterHUDBreathSpec::RunTest(const FString&)
{
	// Replication surface: CPF_Net only — never walk the lifetime list off a CDO
	// (push-model assert).
	const FProperty* Prop =
	    FindFProperty<FProperty>(AWoTCharacter::StaticClass(), TEXT("BreathDisplay"));
	TestTrue(TEXT("BreathDisplay replicates (CPF_Net; COND_OwnerOnly by review)"),
	         Prop && Prop->HasAnyPropertyFlags(CPF_Net));

	// Formatting: countdown line, DROWNING banner, absent when dry.
	FWoTHUDReadout R;
	R.bHasPawn = true;
	R.Breath = 7;
	bool bSaw = false;
	for (const FString& L : AWoTHUD::FormatLines(R))
	{
		bSaw |= L.Contains(TEXT("Breath: 7"));
	}
	TestTrue(TEXT("countdown line"), bSaw);
	R.Breath = 0;
	bSaw = false;
	for (const FString& L : AWoTHUD::FormatLines(R))
	{
		bSaw |= L.Contains(TEXT("DROWNING"));
	}
	TestTrue(TEXT("drowning banner"), bSaw);
	R.Breath = -1;
	bSaw = false;
	for (const FString& L : AWoTHUD::FormatLines(R))
	{
		bSaw |= L.Contains(TEXT("Breath"));
	}
	TestFalse(TEXT("no line when dry"), bSaw);

	// Live stamp: a submerged pawn's owner-only mirror counts the 30s pool down.
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(2000.0f));
	Water->bWaterZone = true;
	AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, FVector::ZeroVector);
	TickWaterWorld(World, 1.0f);
	TestTrue(TEXT("BreathDisplay counts down while submerged (30s pool)"),
	         Pawn->BreathDisplay >= 28 && Pawn->BreathDisplay <= 30);
	Pawn->SetActorLocation(FVector(50000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.1f);
	TestEqual(TEXT("BreathDisplay -1 when dry"), Pawn->BreathDisplay, -1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterMuffleSpec,
	"WoT.Water.Muffle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterMuffleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A local listener: PC possessing a pawn (the ZoneSwimFriction arrangement).
	AWoTCharacter* Listener = SpawnWaterPawn<AWoTCharacter>(World, FVector::ZeroVector);
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Listener);

	// A configured ambient loop far from any water (the emitter's own position is
	// irrelevant — the muffle keys on the LISTENER's head).
	AWoTAmbientSound* Amb = World->SpawnActorDeferred<AWoTAmbientSound>(
	    AWoTAmbientSound::StaticClass(),
	    FTransform(FVector(500.0f, 0.0f, 0.0f)));
	Amb->SoundSubPath = FName(TEXT("FootStubA/STON"));
	Amb->FinishSpawning(FTransform(FVector(500.0f, 0.0f, 0.0f)));
	const float BaseVol = Amb->Ambient->VolumeMultiplier;
	const float BasePitch = Amb->Ambient->PitchMultiplier;
	TestTrue(TEXT("ambient configured"), BaseVol > 0.0f);

	// Submerge the LISTENER's head: ambient loops muffle x0.90 vol / x0.77 pitch /
	// x0.80 radius (the original Galaxy audio driver).
	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(300.0f));
	Water->bWaterZone = true;
	TickWaterWorld(World, 0.3f);   // > the subsystem's 0.1s poll
	TestEqual(TEXT("muffled volume x0.90"), Amb->Ambient->VolumeMultiplier,
	          BaseVol * 0.90f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("muffled pitch x0.77"), Amb->Ambient->PitchMultiplier,
	          BasePitch * 0.77f, KINDA_SMALL_NUMBER);
	if (TestNotNull(TEXT("attenuation live"), Amb->Ambient->AttenuationSettings.Get()))
	{
		TestEqual(TEXT("muffled falloff x0.80"),
		          Amb->Ambient->AttenuationSettings->Attenuation.FalloffDistance,
		          Amb->Ambient->BaseRadiusUU * 0.80f, KINDA_SMALL_NUMBER);
	}

	// Surface: restored.
	Listener->SetActorLocation(FVector(50000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.3f);
	TestEqual(TEXT("restored volume"), Amb->Ambient->VolumeMultiplier, BaseVol,
	          KINDA_SMALL_NUMBER);
	TestEqual(TEXT("restored pitch"), Amb->Ambient->PitchMultiplier, BasePitch,
	          KINDA_SMALL_NUMBER);

	// One-shot rule (WOTPlayer.uc:1394-1439): x0.90/x0.80/x0.85, listener-head OR
	// source underwater, DROWN/GASP waves exempt.
	Listener->SetActorLocation(FVector::ZeroVector);   // head back underwater
	TickWaterWorld(World, 0.1f);
	float V = 1.0f, R = 1000.0f, P = 1.0f;
	TestTrue(TEXT("one-shot muffles for a submerged listener"),
	         WoTSound::ApplyUnderwaterOneShot(World, FVector(50000.0f, 0.0f, 0.0f),
	                                          FName(TEXT("STON")), V, R, P));
	TestEqual(TEXT("one-shot volume x0.90"), V, 0.90f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("one-shot radius x0.80"), R, 800.0f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("one-shot pitch x0.85"), P, 0.85f, KINDA_SMALL_NUMBER);
	V = 1.0f; R = 1000.0f; P = 1.0f;
	TestFalse(TEXT("DROWN waves exempt"),
	          WoTSound::ApplyUnderwaterOneShot(World, FVector(50000.0f, 0.0f, 0.0f),
	                                           FName(TEXT("Aes_Drown1")), V, R, P));
	TestFalse(TEXT("GASP waves exempt"),
	          WoTSound::ApplyUnderwaterOneShot(World, FVector(50000.0f, 0.0f, 0.0f),
	                                           FName(TEXT("Whi_Gasp1")), V, R, P));
	// A dry listener still hears an underwater SOURCE muffled.
	Listener->SetActorLocation(FVector(50000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.1f);
	TestTrue(TEXT("source-underwater also muffles"),
	         WoTSound::ApplyUnderwaterOneShot(World, FVector::ZeroVector,
	                                          FName(TEXT("STON")), V, R, P));

	WoTTest::DestroySpecWorld(World);
	return true;
}

namespace
{
	// A bare walkable cube (the shallow-splash occluder floor).
	AStaticMeshActor* SpawnWaterFloor(UWorld* World, const FVector& Center)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		    AStaticMeshActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->GetStaticMeshComponent()->SetStaticMesh(Cast<UStaticMesh>(
		    FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad()));
		Floor->SetActorScale3D(FVector(15.0f, 15.0f, 1.0f));
		return Floor;
	}

	int32 CountWaterMarkersLike(UWorld* World, const TCHAR* Contains)
	{
		int32 N = 0;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (IsValid(*It) && It->SoundSubPath.ToString().Contains(Contains))
			{
				++N;
			}
		}
		return N;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterBubblesSpec,
	"WoT.Water.Bubbles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterBubblesSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(2000.0f));
	Water->bWaterZone = true;

	// ANY damaging hit with the head underwater bubbles from the mouth
	// (GenericDamageHelper.uc:131-134) — Drowned included (it just sheds no blood).
	AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, FVector::ZeroVector);
	TickWaterWorld(World, 0.1f);
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Sliced");
	Pawn->TakeDamage(10.0f, Ev, nullptr, nullptr);
	TickWaterWorld(World, 0.1f);
	int32 Markers = 0, Groups = 0;
	for (TActorIterator<AWoTBubbleBurst> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++Markers; }
	}
	for (TActorIterator<AWoTBubbles> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++Groups; }
	}
	TestEqual(TEXT("underwater damage spawns the bubble marker"), Markers, 1);
	TestEqual(TEXT("the marker ran the local group"), Groups, 1);

	// Bubbles die when the water goes away (WOTBubble.ZoneChange), and the
	// group actor dies with its bubbles (bDestroy=True). A high-rolled group is
	// still SPAWNING for up to 0.8s (8 at the 0.1s cadence) — each newborn dies
	// on its next dry tick; wait out the whole cadence.
	Water->Destroy();
	TickWaterWorld(World, 1.2f);
	Groups = 0;
	for (TActorIterator<AWoTBubbles> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++Groups; }
	}
	TestEqual(TEXT("bubbles die out of water; the group self-destroys"), Groups, 0);

	// A dry pawn's damage never bubbles.
	AWoTCharacter* Dry = SpawnWaterPawn<AWoTCharacter>(World, FVector(30000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.1f);
	Dry->TakeDamage(10.0f, Ev, nullptr, nullptr);
	TickWaterWorld(World, 0.1f);
	Markers = 0;
	for (TActorIterator<AWoTBubbleBurst> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++Markers; }
	}
	TestEqual(TEXT("dry damage never bubbles (only the expiring first marker)"),
	          Markers, 1);   // the 3s-LifeSpan marker from the first hit persists

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterSplashSpec,
	"WoT.Water.Splash", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterSplashSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// An authored deep-water zone (the Arena_12 shape: DIVE in, Emerge2 out).
	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(800.0f));
	Water->bWaterZone = true;
	Water->EntrySoundSubPath = FName(TEXT("AmbientA/DIVE"));
	Water->ExitSoundSubPath = FName(TEXT("AmbientA/Emerge2"));

	AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, FVector(5000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.1f);   // settle the dry state

	// Entry: the DIVE at volume 2*splashSize (>=2 — splashSize clamps at 1).
	Pawn->SetActorLocation(FVector::ZeroVector);
	TickWaterWorld(World, 0.05f);
	TestEqual(TEXT("entry plays the zone's DIVE"), CountWaterMarkersLike(World, TEXT("DIVE")), 1);
	for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
	{
		if (IsValid(*It) && It->SoundSubPath == FName(TEXT("AmbientA/DIVE")))
		{
			TestTrue(TEXT("entry volume 2*splashSize (>=2, <=8)"),
			         It->Volume >= 1.99f && It->Volume <= 8.01f);
		}
	}

	// Exit inside the 0.25s throttle window: silent (the pawn SplashTime gate).
	Pawn->SetActorLocation(FVector(5000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.05f);
	TestEqual(TEXT("re-cross inside 0.25s is throttled"),
	          CountWaterMarkersLike(World, TEXT("Emerge")), 0);

	// Past the throttle: enter + exit both sound.
	TickWaterWorld(World, 0.3f);
	Pawn->SetActorLocation(FVector::ZeroVector);
	TickWaterWorld(World, 0.3f);
	Pawn->SetActorLocation(FVector(5000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.05f);
	TestEqual(TEXT("exit plays the zone's Emerge2"),
	          CountWaterMarkersLike(World, TEXT("Emerge")), 1);

	// Shallow entry: a floor within 40uu below the feet mutes the dive (the
	// shipped WaterStep fallback is None everywhere -> silent; Engine.Pawn FootZoneChange).
	AWoTZoneVolume* Shallows =
	    SpawnWaterZone(World, FVector(10000.0f, 0.0f, 0.0f), FVector(800.0f, 800.0f, 200.0f));
	Shallows->bWaterZone = true;
	Shallows->EntrySoundSubPath = FName(TEXT("AmbientA/DIVE"));
	SpawnWaterFloor(World, FVector(10000.0f, 0.0f, 0.0f));   // top at z=50
	TickWaterWorld(World, 0.3f);   // clear the throttle
	const int32 DivesBefore = CountWaterMarkersLike(World, TEXT("DIVE"));
	Pawn->SetActorLocation(FVector(10000.0f, 0.0f, 130.0f));   // feet 84, 34uu over the floor
	TickWaterWorld(World, 0.05f);
	TestEqual(TEXT("shallow entry is silent (occluded feet)"),
	          CountWaterMarkersLike(World, TEXT("DIVE")), DivesBefore);

	// An unauthored zone (most MP water): no sounds either way.
	AWoTZoneVolume* Mute =
	    SpawnWaterZone(World, FVector(20000.0f, 0.0f, 0.0f), FVector(800.0f));
	Mute->bWaterZone = true;
	TickWaterWorld(World, 1.1f);   // expire the earlier markers (LifeSpan 1.0)
	Pawn->SetActorLocation(FVector(20000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.3f);
	Pawn->SetActorLocation(FVector(25000.0f, 0.0f, 0.0f));
	TickWaterWorld(World, 0.05f);
	TestEqual(TEXT("unauthored zone splashes silently"),
	          CountWaterMarkersLike(World, TEXT("AmbientA")), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

namespace
{
	// Real zone + blocking floor; no PhysicsVolume and no injected velocity.
	AWoTCharacter* SpawnSwimmingPlayer(UWorld* World, const FVector& Location,
		float SurfaceZ = 1000.0f)
	{
		AWoTZoneVolume* Water = SpawnWaterZone(World,
			FVector(0.0f, 0.0f, SurfaceZ - 1000.0f), FVector(1000.0f));
		Water->bWaterZone = true;
		SpawnWaterFloor(World, FVector(0.0f, 0.0f, -50.0f));
		AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, Location);
		// The existing breath fixtures are unpossessed. Initialize live physics
		// here; CMC otherwise remains MOVE_None from controller-less spawning.
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		return Pawn;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimFloorSpec,
	"WoT.Water.Swim.RestingOnFloorHasNoBuildUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimFloorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 46.5f));
	UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
	double PeakSinkSpeed = 0.0;
	for (int32 Tick = 0; Tick < 300; ++Tick)
	{
		WoTTest::TickWorld(World, 1);
		PeakSinkSpeed = FMath::Max(PeakSinkSpeed, -Move->Velocity.Z);
	}
	TestEqual(TEXT("zone keeps a resting pawn swimming"), Move->MovementMode, MOVE_Swimming);
	// UE1 calcVelocity caps at WaterSpeed; physSwimming uses actual travel.
	TestTrue(TEXT("never sinks faster than retail WaterSpeed 200"), PeakSinkSpeed <= 200.01);
	TestTrue(TEXT("five seconds on the floor leaves no vertical buildup"),
		FMath::Abs(Move->Velocity.Z) < 1.0);
	TestTrue(TEXT("pawn actually rests on the floor"),
		FMath::Abs(Pawn->GetActorLocation().Z - 46.0) < 1.0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimBuoyancySpec,
	"WoT.Water.Swim.SinkRateIsBuoyant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimBuoyancySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 500.0f));
	UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
	constexpr float Step = 1.0f / 60.0f;
	// WOTPlayer.uc:7289-7290 (175/150); Engine.ZoneInfo defaults (-950/1.2).
	// UE1 calcVelocity: turn damping, fluid, THEN buoyancy.
	const double ExpectedFirst = -950.0 * Step * (1.0 - 150.0 / 175.0);
	WoTTest::TickWorld(World, 1, Step);
	TestEqual(TEXT("first movement tick enters swimming"), Move->MovementMode, MOVE_Swimming);
	TestTrue(TEXT("first tick sinks with retail buoyant gravity"),
		FMath::IsNearlyEqual(Move->Velocity.Z, ExpectedFirst, 0.001));
	TestTrue(TEXT("first tick displacement follows buoyant velocity"),
		FMath::IsNearlyEqual(Pawn->GetActorLocation().Z, 500.0 + ExpectedFirst * Step, 0.001));
	const double ExpectedSecond = ExpectedFirst * (1.0 - 1.2 * Step)
		* (1.0 - 1.2 * Step) + ExpectedFirst;
	WoTTest::TickWorld(World, 1, Step);
	TestTrue(TEXT("existing sink velocity gets friction before buoyant gravity"),
		FMath::IsNearlyEqual(Move->Velocity.Z, ExpectedSecond, 0.001));
	// Authored friction below one still uses max(1, friction) for turning,
	// and arbitrary zone gravity replaces the default in all three axes.
	for (TActorIterator<AWoTZoneVolume> It(World); It; ++It)
	{
		It->ZoneFluidFriction = 0.6f;
		It->bOverrideGravity = true;
		It->ZoneGravity = FVector(280.0f, -140.0f, -1900.0f);
	}
	const FVector ExpectedThird = FVector(0.0, 0.0, ExpectedSecond)
		* (1.0 - Step) * (1.0 - 0.6 * Step)
		+ FVector(280.0, -140.0, -1900.0) * Step / 7.0;
	WoTTest::TickWorld(World, 1, Step);
	TestTrue(TEXT("authored vector gravity and fluid friction drive swimming"),
		Move->Velocity.Equals(ExpectedThird, 0.001));
	for (TActorIterator<AWoTZoneVolume> It(World); It; ++It)
	{
		It->ZoneGravity = FVector(0.0f, 0.0f, -1000000.0f);
	}
	WoTTest::TickWorld(World, 1, Step);
	TestTrue(TEXT("WaterSpeed caps the result AFTER buoyant gravity"),
		FMath::IsNearlyEqual(Move->Velocity.Size(), 200.0, 0.001));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimUpSpec,
	"WoT.Water.Swim.SwimUpFromFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimUpSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 46.5f));
	WoTTest::TickWorld(World, 300);
	const double FloorZ = Pawn->GetActorLocation().Z;
	// WOTPlayer.uc:3124-3127 ignores walk input while swimming.
	CastChecked<UWoTCharacterMovement>(Pawn->GetCharacterMovement())->SetWalkingRequested(true);
	bool bRose = false;
	for (int32 Tick = 0; Tick < 30; ++Tick)
	{
		Pawn->AddMovementInput(FVector::UpVector, 1.0f, true);
		WoTTest::TickWorld(World, 1);
		bRose |= Pawn->GetCharacterMovement()->Velocity.Z > 0.0;
	}
	TestTrue(TEXT("up input overcomes sinking within half a second"), bRose);
	TestTrue(TEXT("up input lifts the pawn off the floor"), Pawn->GetActorLocation().Z > FloorZ + 1.0);
	TestTrue(TEXT("walk request does not impose the ground 0.3 speed cap in water"),
		Pawn->GetCharacterMovement()->Velocity.Z > 60.0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimLeaveSpec,
	"WoT.Water.Swim.LeaveWaterNoFallDeath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimLeaveSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 46.5f));
	SpawnWaterFloor(World, FVector(3000.0f, 0.0f, -50.0f));
	const int32 InitialHealth = Pawn->Health;
	WoTTest::TickWorld(World, 300);
	// Arena_11 repro: teleport preserves the real accumulated swim velocity.
	Pawn->SetActorLocation(FVector(3000.0f, 0.0f, 100.0f), false,
		nullptr, ETeleportType::TeleportPhysics);
	WoTTest::TickWorld(World, 60);
	TestEqual(TEXT("pawn landed outside the water"), Pawn->GetCharacterMovement()->MovementMode, MOVE_Walking);
	TestEqual(TEXT("landing after a swim does not take falling damage"), Pawn->Health, InitialHealth);
	TestFalse(TEXT("leaving water cannot trigger the guaranteed-gib fall"), Pawn->bDead);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimWaterlineSpec,
	"WoT.Water.Swim.ExitAtWaterline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimWaterlineSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 145.0f), 160.0f);
	UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
	bool bExited = false;
	// Shorter than retail's 0.01s continuation gate (:1736), so the exit tick
	// exposes the bob itself, without subsequent falling gravity obscuring it.
	for (int32 Tick = 0; Tick < 240; ++Tick)
	{
		Pawn->AddMovementInput(FVector::UpVector, 0.3f, true);
		WoTTest::TickWorld(World, 1, 1.0f / 120.0f);
		if (!AWoTZoneVolume::IsPointInWater(World, Pawn->GetActorLocation()))
		{
			bExited = true;
			break;
		}
	}
	TestTrue(TEXT("swim input crosses the surface"), bExited);
	TestEqual(TEXT("surface exit immediately changes physics"), Move->MovementMode, MOVE_Falling);
	// UE1 physSwimming surface exit: 40 + 0.4*horizontal speed, here horizontal=0.
	TestTrue(TEXT("surface exit applies retail bobbing Z"),
		FMath::IsNearlyEqual(Move->Velocity.Z, 40.0, 0.001));
	TestTrue(TEXT("exit remains within the retail one-unit waterline tolerance"),
		Pawn->GetActorLocation().Z > 160.0 && Pawn->GetActorLocation().Z < 161.0);
	// Re-enter preserving the velocity earned above. A 0.1s tick traverses
	// several units and leaves >0.01s in air, exercising the binary search and
	// StartNewPhysics rather than only sub-unit crossings.
	Pawn->SetActorLocation(FVector(0.0f, 0.0f, 159.75f), false,
		nullptr, ETeleportType::TeleportPhysics);
	Pawn->AddMovementInput(FVector::UpVector, 0.3f, true);
	WoTTest::TickWorld(World, 1, 0.1f);
	TestEqual(TEXT("long surface crossing continues in falling"), Move->MovementMode, MOVE_Falling);
	TestTrue(TEXT("remaining air time applies falling gravity after the bob"), Move->Velocity.Z < 39.0);
	TestTrue(TEXT("waterline rewind prevents retaining the full swim displacement in air"),
		Pawn->GetActorLocation().Z < 164.0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimHeadDampingSpec,
	"WoT.Water.Swim.HeadOutDampsRise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimHeadDampingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 500.0f));
	for (int32 Tick = 0; Tick < 15; ++Tick)
	{
		Pawn->AddMovementInput(FVector::UpVector, 1.0f, true);
		WoTTest::TickWorld(World, 1);
	}
	UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
	TestTrue(TEXT("input earns upward speed above the head damping threshold"), Move->Velocity.Z > 100.0);
	Pawn->SetActorLocation(FVector(0.0f, 0.0f, 990.0f), false,
		nullptr, ETeleportType::TeleportPhysics);
	constexpr float Step = 1.0f / 60.0f;
	// UE1 physSwimming's out-of-water Z damping, then calcVelocity, with input released.
	const double Expected = Move->Velocity.Z * (1.0 - Step)
		* (1.0 - 1.2 * Step) * (1.0 - 1.2 * Step) - 950.0 * Step / 7.0;
	WoTTest::TickWorld(World, 1, Step);
	TestEqual(TEXT("body is still swimming while head is dry"), Move->MovementMode, MOVE_Swimming);
	TestTrue(TEXT("head-zone damping precedes fluid friction and buoyancy"),
		FMath::IsNearlyEqual(Move->Velocity.Z, Expected, 0.001));
	// Replay the same earned movement state with the two presentation tween
	// heights. At z=974 the steady swim head is wet; the tween head is dry.
	// Prediction must not change its physics result when animation advances.
	const FVector ReplayVelocity = Move->Velocity;
	const FVector ReplayLocation(0.0f, 0.0f, 974.0f);
	Pawn->SetActorLocation(ReplayLocation, false, nullptr, ETeleportType::TeleportPhysics);
	Pawn->BaseEyeHeight = 0.8f * Pawn->GetRetailStandingBaseEyeHeight();
	Move->TickComponent(Step, LEVELTICK_All, nullptr);
	const FVector FirstResult = Move->Velocity;
	const FVector FirstPosition = Pawn->GetActorLocation();
	Pawn->SetActorLocation(ReplayLocation, false, nullptr, ETeleportType::TeleportPhysics);
	Move->Velocity = ReplayVelocity; // restore captured state, as a correction replay does.
	Pawn->BaseEyeHeight = 0.5f * Pawn->GetRetailStandingBaseEyeHeight();
	Move->TickComponent(Step, LEVELTICK_All, nullptr);
	TestTrue(TEXT("head damping replays independently of the animation tween"),
		Move->Velocity.Equals(FirstResult, 0.001)
		&& Pawn->GetActorLocation().Equals(FirstPosition, 0.001));
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimCreatureBuoyancySpec,
	"WoT.Water.Swim.CreatureBuoyancy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimCreatureBuoyancySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTZoneVolume* Water = SpawnWaterZone(World, FVector::ZeroVector, FVector(1000.0f));
	Water->bWaterZone = true;
	SpawnWaterFloor(World, FVector(0.0f, 0.0f, -50.0f));
	// SisterAssets.uc:67 (Mass 120) rises and Legion.uc:528 (Mass 3000) sinks,
	// both with Grunt.uc:2434's Buoyancy 150: ConfigureNPC applies the row Mass.
	for (const float CreatureMass : {120.0f, 3000.0f})
	{
		const FTransform Transform(FVector(0.0f, 0.0f, 500.0f));
		AWoTNPCCharacter* Pawn = World->SpawnActorDeferred<AWoTNPCCharacter>(
			AWoTNPCCharacter::StaticClass(), Transform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Pawn->AutoPossessAI = EAutoPossessAI::Disabled;
		Pawn->FinishSpawning(Transform);
		FWoTCreatureRow Row;
		Row.Mass = CreatureMass;
		Row.ConfigureNPC(*Pawn, nullptr);
		UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
		Move->bRunPhysicsWithNoController = true;
		Move->SetMovementMode(MOVE_Falling);
		WoTTest::TickWorld(World, 1);
		const double Expected = -950.0 / 60.0 * (1.0 - 150.0 / CreatureMass);
		TestTrue(TEXT("creature row controls buoyant vertical movement"),
			FMath::IsNearlyEqual(Move->Velocity.Z, Expected, 0.001));
		Pawn->Destroy();
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

namespace
{
	// A solid bank beside the pool: a 1500 x 1500 cube scaled to TopZ.
	void SpawnBank(UWorld* World, float NearX, float TopZ)
	{
		AStaticMeshActor* Bank = SpawnWaterFloor(World,
			FVector(NearX + 750.0f, 0.0f, 0.5f * TopZ));
		Bank->SetActorScale3D(FVector(15.0f, 15.0f, TopZ / 100.0f));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimWaterJumpSpec,
	"WoT.Water.Swim.WaterJumpOntoBank",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimWaterJumpSpec::RunTest(const FString&)
{
	// Waterline 160, a vertical bank 20 above it: higher than a swimming pawn's
	// step-up can reach (its feet ride a full half-height below the waterline),
	// within the 1.1*MaxStepHeight lip test (Engine.Pawn.CheckWaterJump).
	constexpr float BankX = 200.0f;
	constexpr float BankTop = 180.0f;
	for (const bool bForward : {true, false})
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 120.0f), 160.0f);
		SpawnBank(World, BankX, BankTop);
		const float Radius = Pawn->GetSimpleCollisionRadius();
		const float HalfHeight = Pawn->GetSimpleCollisionHalfHeight();
		Pawn->SetActorLocation(FVector(BankX - Radius - 2.0f, 0.0f, 120.0f), false,
			nullptr, ETeleportType::TeleportPhysics);
		UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
		constexpr float Step = 1.0f / 120.0f;
		double PeakZ = -1.0e9;
		for (int32 Tick = 0; Tick < 360; ++Tick)
		{
			// Engine.PlayerPawn PlayerSwimming bUpAndOut: forward along the view and up.
			Pawn->AddMovementInput(bForward ? FVector(1.0f, 0.0f, 1.0f).GetSafeNormal()
				: FVector::UpVector, 1.0f, true);
			WoTTest::TickWorld(World, 1, Step);
			PeakZ = FMath::Max(PeakZ, Move->Velocity.Z);
		}
		const FVector End = Pawn->GetActorLocation();
		if (bForward)
		{
			// Retail sets 330 + 2*CollisionRadius on the exit tick; the remaining
			// air time of that tick (<= one Step) then falls under gravity.
			const double JumpZ = 330.0 + 2.0 * Radius;
			TestTrue(TEXT("leaving the water against the bank sets the retail water-jump Z"),
				PeakZ <= JumpZ + 0.001 && PeakZ >= JumpZ - 2000.0 * Step);
			TestTrue(TEXT("the water jump lands the pawn on the bank"),
				End.X > BankX && FMath::Abs(End.Z - (BankTop + HalfHeight)) < 3.0);
			TestEqual(TEXT("the pawn walks on the bank"), Move->MovementMode, MOVE_Walking);
		}
		else
		{
			TestTrue(TEXT("no forward push: no water jump"), PeakZ < 300.0);
			TestTrue(TEXT("no forward push: the pawn stays in the pool"), End.X < BankX);
		}
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Weaves and shallow water (Arena_01's stage pool, WaterZone0: 24uu deep over
// the floor). UE1 ULevel::SetActorZone is point-based and runs after spawn,
// after every move (before the caller's HitWall) and after SetLocation,
// whatever the collision.
// ---------------------------------------------------------------------------
namespace
{
	// A floor with its top at z=50 and a 24uu water layer on it (z 50..74).
	void SpawnShallowPool(UWorld* World, const FVector& At)
	{
		SpawnWaterFloor(World, At);
		AWoTZoneVolume* Pool = SpawnWaterZone(World,
			At + FVector(0.0f, 0.0f, 62.0f), FVector(700.0f, 700.0f, 12.0f));
		Pool->bWaterZone = true;
	}
}

// AngrealExpWardProjectile.uc:68-90 places the ward with SetLocation; inside a
// water zone that fires ZoneChange -> HitWater -> Destroy (:165-169), so
// SpawnActor returns None and LaunchProjectileEffect reports failure: no ward,
// no charge used (LaunchProjectileEffect.uc:133-141, ProjectileLauncher.uc:216-226).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterWardPlacementSpec,
	"WoT.Water.WardPlacedUnderwater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterWardPlacementSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	World->GetAuthGameMode<AWoTGameMode>()->Difficulty = 0;
	SpawnShallowPool(World, FVector(0.0f, 0.0f, 0.0f));
	SpawnWaterFloor(World, FVector(5000.0f, 0.0f, 0.0f));   // dry control, top z=50

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FRotator Down(-90.0f, 0.0f, 0.0f);

	// 1) The ward itself: aimed at the pool floor it never exists.
	AWoTExpWardProjectile* Wet = World->SpawnActor<AWoTExpWardProjectile>(
		AWoTExpWardProjectile::StaticClass(), FVector(0.0f, 0.0f, 150.0f), Down, Params);
	TestFalse(TEXT("a ward placed on an underwater floor is destroyed at placement"),
		IsValid(Wet));
	if (Wet != nullptr)
	{
		TestFalse(TEXT("an underwater ward reports a failed creation"),
			Wet->CreationSucceeded());
	}
	AWoTExpWardProjectile* Dry = World->SpawnActor<AWoTExpWardProjectile>(
		AWoTExpWardProjectile::StaticClass(), FVector(5000.0f, 0.0f, 150.0f), Down, Params);
	if (TestTrue(TEXT("a ward on a dry floor is placed"), IsValid(Dry)))
	{
		TestTrue(TEXT("a dry ward reports success"), Dry->CreationSucceeded());
		TickWaterWorld(World, 0.1f);
		TestTrue(TEXT("a dry ward survives the per-frame water pass"), IsValid(Dry));
	}

	// 2) Through the artifact: the failed cast uses no charge; a dry cast does.
	AWoTCharacter* Caster = SpawnWaterPawn<AWoTCharacter>(World, FVector(0.0f, 0.0f, 150.0f));
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvExpWard* Item = World->SpawnActor<AWoTAngrealInvExpWard>(
		FVector(0.0f, 300.0f, 150.0f), FRotator::ZeroRotator, Params);
	TickWaterWorld(World, 0.1f);
	Caster->SetActorLocation(Item->GetActorLocation());
	TickWaterWorld(World, 0.1f);
	UWoTInventoryComponent* Inv = Caster->InventoryComponent;
	if (!TestTrue(TEXT("the caster picked up the ward artifact"),
		Inv->SelectedItem.Get() == Item))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	auto CountWards = [World]()
	{
		int32 N = 0;
		for (TActorIterator<AWoTExpWardProjectile> It(World); It; ++It)
		{
			N += IsValid(*It) && !It->IsActorBeingDestroyed() ? 1 : 0;
		}
		return N;
	};
	const int32 WardsBefore = CountWards();
	const int32 Charges = Item->CurCharges;
	Caster->SetActorRotation(Down);
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("casting at the pool floor places no ward"), CountWards(), WardsBefore);
	TestEqual(TEXT("casting at the pool floor uses no charge"), Item->CurCharges, Charges);

	TickWaterWorld(World, 0.5f);   // clear any cast gate
	Caster->SetActorLocation(FVector(5000.0f, 300.0f, 150.0f));
	Caster->SetActorRotation(Down);
	Inv->UseAngreal();
	Inv->CeaseUsingAngreal();
	TestEqual(TEXT("casting at a dry floor places a ward"), CountWards(), WardsBefore + 1);
	TestEqual(TEXT("casting at a dry floor uses one charge"), Item->CurCharges, Charges - 1);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// A Fireball that crosses the surface and strikes the pool floor in ONE move
// fizzles: UE1 sets the zone at the end of MoveActor before
// physProjectile hands the hit to HitWall, and AngrealFireballProjectile.uc:190
// destroys it. No explosion, so no splash damage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterShallowFloorHitSpec,
	"WoT.Water.ShallowFloorHitFizzles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterShallowFloorHitSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnShallowPool(World, FVector(0.0f, 0.0f, 0.0f));
	SpawnWaterFloor(World, FVector(5000.0f, 0.0f, 0.0f));   // dry control

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FRotator Down(-90.0f, 0.0f, 0.0f);

	// A bystander inside the 220uu splash radius but off the flight line.
	auto Bystander = [&](const FVector& At)
	{
		AWoTCharacter* Pawn = SpawnWaterPawn<AWoTCharacter>(World, At);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		return Pawn;
	};
	AWoTCharacter* WetWitness = Bystander(FVector(120.0f, 0.0f, 130.0f));
	AWoTCharacter* DryWitness = Bystander(FVector(5120.0f, 0.0f, 130.0f));
	TickWaterWorld(World, 0.1f);
	const int32 WetHealth = WetWitness->Health;
	const int32 DryHealth = DryWitness->Health;

	// 6uu above the surface, 1000uu/s straight down, one 1/30s step: the move
	// crosses the 24uu layer and hits the floor in the same step.
	AWoTFireballProjectile* Wet = World->SpawnActor<AWoTFireballProjectile>(
		AWoTFireballProjectile::StaticClass(), FVector(0.0f, 0.0f, 80.0f), Down, Params);
	AWoTFireballProjectile* DryShot = World->SpawnActor<AWoTFireballProjectile>(
		AWoTFireballProjectile::StaticClass(), FVector(5000.0f, 0.0f, 80.0f), Down, Params);
	TestNotNull(TEXT("wet fireball spawned"), Wet);
	TestNotNull(TEXT("dry fireball spawned"), DryShot);
	WoTTest::TickWorld(World, 1, 1.0f / 30.0f);
	WoTTest::TickWorld(World, 3, 1.0f / 30.0f);

	TestFalse(TEXT("the wet fireball is gone"), IsValid(Wet));
	TestEqual(TEXT("a fireball that hits the pool floor fizzles: no splash damage"),
		WetWitness->Health, WetHealth);
	TestTrue(TEXT("control: the same shot on a dry floor explodes and splashes"),
		DryWitness->Health < DryHealth);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// Earth Tremor has no HitWater override: it fizzles at the surface
// (HitWaterClass=FireballFizzle, offset Z=56, AngrealEarthTremorProjectile.uc:158-161),
// keeps falling and still erupts on the bottom.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWaterEarthTremorSpec,
	"WoT.Water.EarthTremorErupts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWaterEarthTremorSpec::RunTest(const FString&)
{
	const AWoTEarthTremorProjectile* CDO = GetDefault<AWoTEarthTremorProjectile>();
	TestEqual(TEXT("Earth Tremor fizzles with FireballFizzle"),
		CDO->HitWaterEffectName, FName(TEXT("FireballFizzle")));
	TestEqual(TEXT("Earth Tremor fizzle offset is Z=56"),
		CDO->HitWaterEffectOffset, FVector(0.0f, 0.0f, 56.0f));

	UWorld* World = WoTTest::CreateSpecWorld();
	// A deeper pool here (z 50..150) so the lob is visibly in the water before
	// it reaches the bottom.
	SpawnWaterFloor(World, FVector::ZeroVector);
	AWoTZoneVolume* Pool = SpawnWaterZone(World, FVector(0.0f, 0.0f, 100.0f),
		FVector(700.0f, 700.0f, 50.0f));
	Pool->bWaterZone = true;

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTEarthTremorProjectile* Lob = World->SpawnActor<AWoTEarthTremorProjectile>(
		AWoTEarthTremorProjectile::StaticClass(), FVector(0.0f, 0.0f, 220.0f),
		FRotator(-90.0f, 0.0f, 0.0f), Params);
	if (!TestNotNull(TEXT("tremor lob spawned"), Lob))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TWeakObjectPtr<AWoTEarthTremorProjectile> LobPtr = Lob;
	bool bSawWater = false;
	for (int32 Step = 0; Step < 60 && LobPtr.IsValid(); ++Step)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		if (LobPtr.IsValid() && LobPtr->bHitWater)
		{
			bSawWater = true;
		}
	}
	TestTrue(TEXT("the lob hit water on the way down and was not destroyed by it"),
		bSawWater);
	int32 Tremors = 0;
	for (TActorIterator<AWoTEarthTremor> It(World); It; ++It)
	{
		++Tremors;
	}
	TestTrue(TEXT("the tremor still erupts on the bottom of the pool"), Tremors > 0);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// PlayerSwimming.PlayerMove (Engine.PlayerPawn): in water a
// player's forward input follows the full ViewRotation (look down to dive, up
// to rise) at aForward*0.2 = 1200 against AccelRate 2048; strafe and the held
// Jump key's aUp add 600 each. Walking keeps yaw-only, full-scale input. This
// drives a POSSESSED player through the real input functions (the older swim
// specs inject AddMovementInput directly, which hid the yaw-only bug that made
// Arena_12's basement water unswimmable).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSwimPlayerInputSpec,
	"WoT.Water.Swim.PlayerInputFollowsView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTSwimPlayerInputSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = SpawnSwimmingPlayer(World, FVector(0.0f, 0.0f, 500.0f));
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>(Params);
	// Locally controlled, so its own input drives the character's physics.
	PC->SetAsLocalPlayerController();
	PC->InitPlayerState();
	PC->Possess(Pawn);
	TickWaterWorld(World, 0.1f);
	UCharacterMovementComponent* Move = Pawn->GetCharacterMovement();
	if (!TestEqual(TEXT("the possessed player swims"),
		static_cast<int32>(Move->MovementMode.GetValue()),
		static_cast<int32>(MOVE_Swimming)))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const float Accel = Move->GetMaxAcceleration();
	TestEqual(TEXT("player AccelRate is 2048"), Accel, 2048.0f);

	// Look down 60 degrees and press forward.
	PC->SetControlRotation(FRotator(-60.0f, 0.0f, 0.0f));
	Pawn->ConsumeMovementInputVector();
	Pawn->AddForwardInputForTest(1.0f);
	FVector Input = Pawn->GetPendingMovementInputVector();
	TestTrue(TEXT("forward input dives along the view"), Input.Z < -0.45);
	TestTrue(TEXT("forward input is aForward*0.2 = 1200 of 2048"),
		FMath::IsNearlyEqual(Input.Size() * Accel, 1200.0, 1.0));
	const double StartZ = Pawn->GetActorLocation().Z;
	for (int32 Tick = 0; Tick < 30; ++Tick)
	{
		Pawn->AddForwardInputForTest(1.0f);
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	}
	TestTrue(TEXT("looking down and swimming forward descends"),
		Pawn->GetActorLocation().Z < StartZ - 20.0);

	// Look up 60 degrees and press forward.
	PC->SetControlRotation(FRotator(60.0f, 0.0f, 0.0f));
	const double LowZ = Pawn->GetActorLocation().Z;
	for (int32 Tick = 0; Tick < 30; ++Tick)
	{
		Pawn->AddForwardInputForTest(1.0f);
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	}
	TestTrue(TEXT("looking up and swimming forward rises"),
		Pawn->GetActorLocation().Z > LowZ + 20.0);

	// Strafe and held Jump are 600 of 2048.
	PC->SetControlRotation(FRotator::ZeroRotator);
	Pawn->ConsumeMovementInputVector();
	Pawn->AddStrafeInputForTest(1.0f);
	Input = Pawn->ConsumeMovementInputVector();
	TestTrue(TEXT("swim strafe is aStrafe*0.1 = 600 of 2048, horizontal"),
		FMath::IsNearlyEqual(Input.Size() * Accel, 600.0, 1.0)
			&& FMath::IsNearlyZero(Input.Z, 1e-4));
	Pawn->AddSwimVerticalInputForTest();
	TestTrue(TEXT("no swim-up input while Jump is not held"),
		Pawn->ConsumeMovementInputVector().IsNearlyZero());
	Pawn->SetSwimUpHeld(true);
	Pawn->AddSwimVerticalInputForTest();
	Input = Pawn->ConsumeMovementInputVector();
	TestTrue(TEXT("held Jump in water is aUp*0.1 = 600 of 2048, straight up"),
		FMath::IsNearlyEqual(Input.Z * Accel, 600.0, 1.0)
			&& FMath::IsNearlyZero(Input.X, 1e-4) && FMath::IsNearlyZero(Input.Y, 1e-4));
	const double HeldStartZ = Pawn->GetActorLocation().Z;
	for (int32 Tick = 0; Tick < 30; ++Tick)
	{
		Pawn->AddSwimVerticalInputForTest();
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	}
	TestTrue(TEXT("holding Jump in water rises"),
		Pawn->GetActorLocation().Z > HeldStartZ + 5.0);
	Pawn->SetSwimUpHeld(false);

	// Out of the water, walking input stays yaw-only and full scale.
	Pawn->SetActorLocation(FVector(5000.0f, 0.0f, 500.0f));
	Move->SetMovementMode(MOVE_Falling);
	TickWaterWorld(World, 0.05f);
	PC->SetControlRotation(FRotator(-60.0f, 0.0f, 0.0f));
	Pawn->ConsumeMovementInputVector();
	Pawn->AddForwardInputForTest(1.0f);
	Input = Pawn->ConsumeMovementInputVector();
	TestTrue(TEXT("out of water, forward input ignores pitch and is full scale"),
		FMath::IsNearlyZero(Input.Z, 1e-4) && FMath::IsNearlyEqual(Input.Size(), 1.0, 1e-3));
	Pawn->SetSwimUpHeld(true);
	Pawn->AddSwimVerticalInputForTest();
	TestTrue(TEXT("held Jump adds no vertical input out of water"),
		Pawn->ConsumeMovementInputVector().IsNearlyZero());

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityABurningChunksSpec,
	"WoT.Parity.A.BurningChunks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTParityABurningChunksSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnWaterFloor(World, FVector::ZeroVector);
	FActorSpawnParameters SP; SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(FVector(0,0,150), FRotator(-90,0,0), SP);
	Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	World->SpawnActor<AWoTAngrealInvFireball>(Caster->GetActorLocation(), FRotator::ZeroRotator, SP);
	WoTTest::TickWorld(World, 0.1f);
	Caster->SetActorRotation(FRotator(-90,0,0));
	Caster->InventoryComponent->UseAngreal();

	TArray<AWoTBurningChunk*> Chunks;
	for (int32 Frame = 0; Frame < 60 && Chunks.Num() < 5; ++Frame)
	{
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
		Chunks.Reset();
		for (TActorIterator<AWoTBurningChunk> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetClass() == AWoTBurningChunk::StaticClass()) { Chunks.Add(*It); }
		}
	}
	TestEqual(TEXT("a real artifact cast and Fireball impact spawn the retail five-chunk fan"), Chunks.Num(), 5);
	if (Chunks.Num() >= 3)
	{
		for (AWoTBurningChunk* Chunk : Chunks)
		{
			TestTrue(TEXT("retail rotator addition launches floor-impact chunks outward"),
				Chunk->GetVelocity().Z >= -KINDA_SMALL_NUMBER);
		}
		for (AWoTGenericProjectile* Chunk : Chunks) { Chunk->SetLifeSpan(10.0f); }
		AStaticMeshActor* Wall = SpawnWaterFloor(World, FVector(1000,0,500));
		Wall->SetActorRotation(FRotator(90,0,0));
		Chunks[0]->SetActorLocation(FVector(940,0,500)); Chunks[0]->ProjectileMovement->Velocity = FVector(500,0,0);
		WoTTest::TickWorld(World, 2, 1.0f / 60.0f);
		TestTrue(TEXT("a wall hit keeps the component-wise retail bounce instead of UE5 reflection"),
			IsValid(Chunks[0]) && Chunks[0]->GetVelocity().X > 5000.0f);

		auto CountBurnDecals = [&]()
		{
			int32 Count = 0;
			for (TActorIterator<AWoTDecal> It(World); It; ++It)
			{
				Count += IsValid(*It) && It->DecalType == TEXT("BurnDecal") ? 1 : 0;
			}
			return Count;
		};
		const int32 DecalsBeforeLanding = CountBurnDecals();
		Chunks[1]->SetActorLocation(FVector(0,0,120)); Chunks[1]->ProjectileMovement->Velocity = FVector(0,0,-400);
		WoTTest::TickWorld(World, 30, 1.0f / 60.0f);
		TestTrue(TEXT("a landed chunk survives long enough to scorch and settle"), IsValid(Chunks[1]) && Chunks[1]->GetVelocity().IsNearlyZero(5.0f));
		TestTrue(TEXT("landing places the retail BurnDecal"), CountBurnDecals() > DecalsBeforeLanding);

		AWoTZoneVolume* Water = SpawnWaterZone(World, FVector(3000,0,100), FVector(300,300,50)); Water->bWaterZone = true;
		SpawnWaterFloor(World, FVector(3000,0,0));
		Chunks[2]->SetActorLocation(FVector(3000,0,200)); Chunks[2]->ProjectileMovement->Velocity = FVector(0,0,-400);
		Chunks[2]->ProjectileMovement->Activate(true);
		WoTTest::TickWorld(World, 30, 1.0f / 60.0f);
		TestTrue(TEXT("a chunk entering water takes the GenericProjectile water path"), IsValid(Chunks[2]) && Chunks[2]->bHitWater);
		TestTrue(TEXT("water extinguishes the chunk light"), IsValid(Chunks[2]) && !Chunks[2]->Light->IsVisible());
		TestNull(TEXT("water stops and releases the chunk sprayer"), Chunks[2]->Sprayer.Get());
		TestTrue(TEXT("an extinguished chunk still lands and settles"), IsValid(Chunks[2]) && Chunks[2]->GetVelocity().IsNearlyZero(5.0f));
		bool bScaledFizzle = false;
		for (TActorIterator<AWoTExplosionEffect> It(World); It; ++It)
		{
			bScaledFizzle |= It->ConfigName == TEXT("FireballFizzle")
				&& FMath::IsNearlyEqual(It->Sprite->DrawScale, 0.7f)
				&& FMath::IsNearlyEqual(It->GetActorLocation().Z, 189.0f, 20.0f);
		}
		TestTrue(TEXT("water spawns the retail 0.7-scale fizzle 39uu above entry"), bScaledFizzle);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif   // WITH_AUTOMATION_TESTS
