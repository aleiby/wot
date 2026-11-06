// Automation specs for the texture-keyed footsteps: the MovementEffects
// registry + prefix keying (WoT.Footstep.{Tables,NameKey}) and the wired step/
// thud path (WoT.Footstep.Steps). Spec of record:
// exports/script/WOT/GenericTextureHelper.uc + the WoTPawns helper subclasses.
// NOTE unity rule: all namespace-scope helper names here are Footstep-prefixed
// (Tests/*.cpp share unity TUs).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTFootsteps.h"
#include "../WoTWeaveFX.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTSoundTable.h"
#include "../WoTZoneVolume.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	// The distinct wave subpaths of a table's rows matching (Group, Surface).
	TArray<FName> FootstepWavesFor(FName TableName, EWoTFootstepGroup Group,
	                               EWoTSurface Surface)
	{
		TArray<FName> Waves;
		for (const FWoTFootstepRow& Row : WoTFootsteps::Table(TableName))
		{
			if (Row.Group == Group &&
			    (Row.Surface == Surface || Row.Surface == EWoTSurface::All))
			{
				Waves.AddUnique(Row.SoundSubPath);
			}
		}
		return Waves;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFootstepTablesSpec,
	"WoT.Footstep.Tables", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFootstepTablesSpec::RunTest(const FString&)
{
	using namespace WoTFootsteps;

	// --- Generic table (GenericTextureHelper.uc:615-670): 4 RunWalk waves per
	// surface (Metal and Rug author only 2), one Landed wave each. ---
	struct FCase { EWoTSurface S; const TCHAR* First; int32 NumRun; const TCHAR* Landed; };
	const FCase Cases[] = {
	    {EWoTSurface::Deck, TEXT("WOT/DeckL"), 4, TEXT("WOT/Deck")},
	    {EWoTSurface::Dirt, TEXT("WOT/EarthL"), 4, TEXT("WOT/Earth")},
	    {EWoTSurface::Grass, TEXT("WOT/GrassL"), 4, TEXT("WOT/Grass")},
	    {EWoTSurface::Gravel, TEXT("WOT/GravelL"), 4, TEXT("WOT/Gravel")},
	    {EWoTSurface::Leaves, TEXT("WOT/LeavesL"), 4, TEXT("WOT/Leaves")},
	    {EWoTSurface::Marble, TEXT("WOT/MarbleL"), 4, TEXT("WOT/Marble")},
	    {EWoTSurface::Metal, TEXT("WOT/MetalL2"), 2, TEXT("WOT/Metal")},
	    {EWoTSurface::Mud, TEXT("WOT/MudL"), 4, TEXT("WOT/Mud")},
	    {EWoTSurface::Puddle, TEXT("WOT/PuddleL"), 4, TEXT("WOT/Puddle")},
	    {EWoTSurface::Rug, TEXT("WOT/RugL2"), 2, TEXT("WOT/Rug")},
	    {EWoTSurface::Stone, TEXT("WOT/StoneL"), 4, TEXT("WOT/Stone")},
	    {EWoTSurface::Wood, TEXT("WOT/WoodL"), 4, TEXT("WOT/Wood")},
	};
	for (const FCase& C : Cases)
	{
		const TArray<FName> Run =
		    FootstepWavesFor(TEXT("Generic"), EWoTFootstepGroup::RunWalk, C.S);
		TestEqual(FString::Printf(TEXT("%s RunWalk wave count"), C.First), Run.Num(),
		          C.NumRun);
		TestTrue(FString::Printf(TEXT("%s present"), C.First),
		         Run.Contains(FName(C.First)));
		const TArray<FName> Land =
		    FootstepWavesFor(TEXT("Generic"), EWoTFootstepGroup::Landed, C.S);
		TestEqual(FString::Printf(TEXT("%s Landed wave count"), C.Landed), Land.Num(), 1);
		TestTrue(FString::Printf(TEXT("%s landed present"), C.Landed),
		         Land.Contains(FName(C.Landed)));
	}

	// Marble authors Pitch=0.8 on every row (RunWalk + Landed); others 1.0.
	for (const FWoTFootstepRow& Row : Table(TEXT("Generic")))
	{
		TestEqual(FString::Printf(TEXT("pitch of %s"), *Row.SoundSubPath.ToString()),
		          Row.Pitch, Row.Surface == EWoTSurface::Marble ? 0.8f : 1.0f);
		TestEqual(FString::Printf(TEXT("volume of %s"), *Row.SoundSubPath.ToString()),
		          Row.Volume, 1.0f);
	}

	// --- Legion's own table (NPCLegionTextureHelper): ST_All rows whose shipped
	// export inherits Group=SG_RunWalk from the parent CDO per-index diff — a
	// naive read of the batchexport silences Legion. ---
	const TArray<FName> LegRun =
	    FootstepWavesFor(TEXT("Legion"), EWoTFootstepGroup::RunWalk, EWoTSurface::Stone);
	TestEqual(TEXT("Legion RunWalk rows (ST_All matches any surface)"), LegRun.Num(), 4);
	TestTrue(TEXT("Leg_RunTex1"), LegRun.Contains(FName(TEXT("WoTPawns/Leg_RunTex1"))));
	const TArray<FName> LegLand =
	    FootstepWavesFor(TEXT("Legion"), EWoTFootstepGroup::Landed, EWoTSurface::Wood);
	TestEqual(TEXT("Legion Landed row"), LegLand.Num(), 1);
	TestTrue(TEXT("Leg_LandTex1"), LegLand.Contains(FName(TEXT("WoTPawns/Leg_LandTex1"))));

	// --- PickRow: uniform among the <=4 matches; never null for a keyed surface;
	// null for a group/surface with no rows in the named table. ---
	TSet<FName> Picked;
	for (int32 I = 0; I < 200; ++I)
	{
		const FWoTFootstepRow* Row =
		    PickRow(TEXT("Generic"), EWoTFootstepGroup::RunWalk, EWoTSurface::Stone);
		if (TestNotNull(TEXT("stone pick"), Row))
		{
			Picked.Add(Row->SoundSubPath);
		}
	}
	TestEqual(TEXT("all 4 stone waves reachable over 200 picks"), Picked.Num(), 4);

	// --- Helper params (shipped TextureHelperClass defaults). ---
	TestEqual(TEXT("Generic pitch"), HelperForSoundTable(TEXT("Trolloc")).BasePitch, 1.0f);
	TestEqual(TEXT("Female pitch 1.2 (AesSedai)"),
	          HelperForSoundTable(TEXT("AesSedai")).BasePitch, 1.2f);
	TestEqual(TEXT("Minion pitch 2.0"),
	          HelperForSoundTable(TEXT("Minion")).BasePitch, 2.0f);
	TestEqual(TEXT("Minion volume 2.0"),
	          HelperForSoundTable(TEXT("Minion")).BaseVolume, 2.0f);
	TestEqual(TEXT("BATrolloc pitch 0.5"),
	          HelperForSoundTable(TEXT("BATrolloc")).BasePitch, 0.5f);
	TestEqual(TEXT("BATrolloc volume 2.0"),
	          HelperForSoundTable(TEXT("BATrolloc")).BaseVolume, 2.0f);
	const FWoTFootstepHelper& Legion = HelperForSoundTable(TEXT("Legion"));
	TestTrue(TEXT("Legion never walk-quiets"), Legion.bNeverWalkQuiet);
	TestEqual(TEXT("Legion own table"), Legion.OwnTable, FName(TEXT("Legion")));

	// --- Walk gate (RunningSoundSpeed=200 -> WalkVolumeMultiplier=0.5). ---
	TestEqual(TEXT("walk = half volume"), VolumeMultForSpeed(100.0f, false), 0.5f);
	TestEqual(TEXT("run = full volume"), VolumeMultForSpeed(300.0f, false), 1.0f);
	TestEqual(TEXT("Legion walk = full volume"), VolumeMultForSpeed(100.0f, true), 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFootstepNameKeySpec,
	"WoT.Footstep.NameKey", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFootstepNameKeySpec::RunTest(const FString&)
{
	using namespace WoTFootsteps;

	// --- ConvertTextureName prefix table (GenericTextureHelper.uc:257-327). ---
	struct FPrefixCase { const TCHAR* Name; EWoTSurface S; };
	const FPrefixCase Prefixes[] = {
	    {TEXT("STONWALL4030"), EWoTSurface::Stone},
	    {TEXT("ROCKFACE1"), EWoTSurface::Stone},
	    {TEXT("BLOKWALL4041V02"), EWoTSurface::Stone},
	    {TEXT("BRIKRED2"), EWoTSurface::Stone},
	    {TEXT("MOSAICFLR"), EWoTSurface::Stone},
	    {TEXT("SLTEROOF"), EWoTSurface::Stone},
	    {TEXT("SFLOOR12"), EWoTSurface::Stone},
	    {TEXT("COBBLE3"), EWoTSurface::Stone},
	    {TEXT("SDAVWALL"), EWoTSurface::Stone},   // Caps('sDav')
	    {TEXT("SILTFLOR4901"), EWoTSurface::Dirt},
	    {TEXT("DIRTROAD"), EWoTSurface::Dirt},
	    {TEXT("PATHWAY"), EWoTSurface::Dirt},
	    {TEXT("DIRTPEBBLE"), EWoTSurface::Gravel},   // 8-char special BEFORE DIRT
	    {TEXT("DIRTLEAVES"), EWoTSurface::Leaves},   // 8-char special BEFORE DIRT
	    {TEXT("GRAVELPIT"), EWoTSurface::Gravel},
	    {TEXT("LEAFBED"), EWoTSurface::Leaves},
	    {TEXT("MARBLSTEP2362"), EWoTSurface::Marble},
	    {TEXT("TILEFLOOR"), EWoTSurface::Marble},
	    {TEXT("METLGRIL4000V01"), EWoTSurface::Metal},
	    {TEXT("MUDDYBANK"), EWoTSurface::Mud},
	    {TEXT("WATRPOOL"), EWoTSurface::Puddle},
	    {TEXT("RUGRED"), EWoTSurface::Rug},          // 3-char rule
	    {TEXT("FABRIC1"), EWoTSurface::Rug},
	    {TEXT("TPSTRY2"), EWoTSurface::Rug},
	    {TEXT("GRASSFIELD"), EWoTSurface::Grass},
	    {TEXT("GRSSKNOLL"), EWoTSurface::Grass},
	    {TEXT("DECKPLANK"), EWoTSurface::Deck},
	    {TEXT("WOODFLOOR"), EWoTSurface::Wood},
	    {TEXT("XYZZY"), EWoTSurface::None},          // unknown -> caller Stones it
	    {TEXT(""), EWoTSurface::None},
	};
	for (const FPrefixCase& C : Prefixes)
	{
		TestEqual(FString::Printf(TEXT("surface of '%s'"), C.Name),
		          static_cast<int32>(SurfaceFromTextureName(C.Name)),
		          static_cast<int32>(C.S));
	}

	// --- Material-name -> texture-name recovery (bsp2obj qualkey + import_map
	// MIC naming; port-side scout examples from Content/WoT on disk). ---
	struct FMatCase { const TCHAR* Mat; const TCHAR* Tex; };
	const FMatCase Mats[] = {
	    {TEXT("MI_W_MordethT__SiltFlor4901"), TEXT("SILTFLOR4901")},
	    {TEXT("MordethT__StonWall4030"), TEXT("STONWALL4030")},        // raw slot name
	    {TEXT("MI_W_ForsakenT__MarblStep2362"), TEXT("MARBLSTEP2362")},
	    {TEXT("MI_JHPFireEng__SLWater02_T_2S"), TEXT("SLWATER02")},    // flag strip x2
	    {TEXT("MetlGril4000v01_M"), TEXT("METLGRIL4000V01")},          // masked flag
	    {TEXT("MI_W_BlokWall4041v02"), TEXT("BLOKWALL4041V02")},       // stale, no pkg
	    {TEXT("MI_Sky"), TEXT("")},
	    {TEXT("MI_Missing"), TEXT("")},
	    {TEXT(""), TEXT("")},
	};
	for (const FMatCase& C : Mats)
	{
		TestEqual(FString::Printf(TEXT("texture from '%s'"), C.Mat),
		          TextureNameFromMaterialName(C.Mat), FString(C.Tex));
	}

	// End-to-end: an imported floor MIC keys the shipped surface.
	TestEqual(TEXT("Arena_01 silt floor -> Dirt"),
	          static_cast<int32>(SurfaceFromTextureName(
	              TextureNameFromMaterialName(TEXT("MI_W_MordethT__SiltFlor4901")))),
	          static_cast<int32>(EWoTSurface::Dirt));
	TestEqual(TEXT("retail grouped texture identity keys its leaf name"),
	          static_cast<int32>(SurfaceFromTextureIdentity(
	              TEXT("MordethT.DetailArchitect.MetlGril4002v01"))),
	          static_cast<int32>(EWoTSurface::Metal));
	TestEqual(TEXT("null retail texture identity remains unclassified"),
	          static_cast<int32>(SurfaceFromTextureIdentity(TEXT("None"))),
	          static_cast<int32>(EWoTSurface::None));
	return true;
}

namespace
{
	// Force a run and tick (the WoTSoundTests 5d arrangement): the loop driver
	// keys on horizontal speed, notify fractions drop the steps.
	void RunFootstepPawn(UWorld* World, ACharacter* Pawn, int32 Ticks,
	                     const FVector& Velocity = FVector(300.0, 0.0, 0.0))
	{
		for (int32 I = 0; I < Ticks; ++I)
		{
			if (Pawn && !Velocity.IsZero())
			{
				// The retail player driver keys direction from acceleration
				// (SetMotionDirection), not residual velocity. Supply the same
				// movement intent that a real player input frame would provide.
				Pawn->AddMovementInput(Velocity.GetSafeNormal2D(), 1.0f, true);
				Pawn->GetCharacterMovement()->Velocity = Velocity;
			}
			World->Tick(LEVELTICK_All, 1.0f / 60.0f);
			++GFrameCounter;
		}
	}

	// Live replicated one-shot markers whose subpath starts with Prefix; Exact
	// matches the whole subpath (the Landed thud waves are the un-suffixed names).
	int32 CountFootstepMarkers(UWorld* World, const TCHAR* Prefix, bool bExact = false)
	{
		int32 N = 0;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			if (!IsValid(*It))
			{
				continue;
			}
			const FString Sub = It->SoundSubPath.ToString();
			if (bExact ? (Sub == Prefix) : Sub.StartsWith(Prefix))
			{
				++N;
			}
		}
		return N;
	}

	// A wide walkable cube floor whose slot-0 material carries an imported-MIC
	// name (a lightweight MID — only the NAME is read by the keying).
	AStaticMeshActor* SpawnFootstepFloor(UWorld* World, const FVector& Center,
	                                     const TCHAR* MaterialName)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
		    AStaticMeshActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		Floor->SetMobility(EComponentMobility::Movable);
		UStaticMesh* Cube = Cast<UStaticMesh>(
		    FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")).TryLoad());
		Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Floor->SetActorScale3D(FVector(40.0f, 40.0f, 1.0f));   // +-2000 x +-2000 x +-50
		UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(
		    UMaterial::GetDefaultMaterial(MD_Surface), Floor, FName(MaterialName));
		Floor->GetStaticMeshComponent()->SetMaterial(0, Mat);
		return Floor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFootstepStepsSpec,
	"WoT.Footstep.Steps", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFootstepStepsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// --- 1. No floor under the pawn: the ConvertTexture fallback is Stone
	// (GenericTextureHelper.uc:445-449) — and the FootStubA stand-in is retired. ---
	AWoTNPCCharacter* Trolloc = WoTCreatureData::SpawnCreature(
	    *World, TEXT("Trolloc"), FVector(0.0f, 0.0f, 100.0f), 1, /*bCombatReady*/ true,
	    AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Trolloc spawned"), Trolloc))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	if (AController* C = Trolloc->GetController())
	{
		C->UnPossess();
	}
	RunFootstepPawn(World, Trolloc, 240);
	TestTrue(TEXT("no floor -> WOT/Stone* fallback steps"),
	         CountFootstepMarkers(World, TEXT("WOT/Stone")) >= 1);
	TestEqual(TEXT("the FootStubA stand-in is retired"),
	          CountFootstepMarkers(World, TEXT("FootStubA")), 0);

	// --- 2. A floor whose material carries an imported marble MIC name keys
	// Marble (and the trace resolves it through the hit component). ---
	SpawnFootstepFloor(World, FVector(5000.0f, 0.0f, 0.0f),
	                   TEXT("MI_W_ForsakenT__MarblStep2362"));
	Trolloc->SetActorLocation(FVector(5000.0f, 0.0f, 120.0f));
	RunFootstepPawn(World, Trolloc, 240);
	TestTrue(TEXT("marble floor -> WOT/Marble* steps"),
	         CountFootstepMarkers(World, TEXT("WOT/Marble")) >= 1);

	// --- 3. NPC wading gate (WOTPawn.uc:1117-1124): feet in a shallow water
	// zone silence the NPC's steps entirely. ---
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTZoneVolume* Shallow = World->SpawnActor<AWoTZoneVolume>(
		    AWoTZoneVolume::StaticClass(), FVector(5000.0f, 0.0f, 30.0f),
		    FRotator::ZeroRotator, Params);
		Shallow->Collision->SetBoxExtent(FVector(1500.0f, 1500.0f, 40.0f));
		Shallow->bWaterZone = true;   // covers the feet (z<=70), not the center
	}
	RunFootstepPawn(World, Trolloc, 240);
	TestEqual(TEXT("a wading NPC is silent (WOTPawn.uc:1118)"),
	          CountFootstepMarkers(World, TEXT("WOT/")), 0);

	// --- 4. A wading PLAYER keeps stepping (WOTPlayer.uc:2428-2434 has no
	// water gate; the trace hits the marble beneath the sheet). ---
	FActorSpawnParameters PawnParams;
	PawnParams.SpawnCollisionHandlingOverride =
	    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAesSedai* Player = World->SpawnActor<AWoTAesSedai>(
	    AWoTAesSedai::StaticClass(), FVector(5000.0f, 300.0f, 120.0f),
	    FRotator::ZeroRotator, PawnParams);
	Player->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	RunFootstepPawn(World, Player, 240);
	TestTrue(TEXT("a wading player keeps marble steps"),
	         CountFootstepMarkers(World, TEXT("WOT/Marble")) >= 1);

	// --- 5. Landing thud: a drop onto the marble plays the SG_Landed wave —
	// exactly "WOT/Marble" (the step waves are the L/R-suffixed names). ---
	Player->SetActorLocation(FVector(5000.0f, 600.0f, 700.0f));
	Player->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	RunFootstepPawn(World, Player, 90, FVector::ZeroVector);   // free fall ~1.5s
	TestTrue(TEXT("landing thud (WOT/Marble, GenericTextureHelper.uc:578-592)"),
	         CountFootstepMarkers(World, TEXT("WOT/Marble"), /*bExact*/ true) >= 1);

	// --- 5b. Puddle steps splash: the Puddle rows carry the shipped
	// EffectName=ParticleSystems.WaterSplash (GenericTextureHelper.uc:645-648) —
	// a WATR-keyed floor spawns the droplet burst marker per step. ---
	{
		int32 PuddleEffectRows = 0;
		for (const FWoTFootstepRow& Row : WoTFootsteps::Table(TEXT("Generic")))
		{
			if (Row.Surface == EWoTSurface::Puddle)
			{
				TestEqual(TEXT("Puddle row carries the WaterSplash effect"),
				          Row.EffectName, FName(TEXT("WaterSplash")));
				++PuddleEffectRows;
			}
			else
			{
				TestTrue(TEXT("only Puddle rows carry an effect"),
				         Row.EffectName.IsNone());
			}
		}
		TestEqual(TEXT("5 Puddle rows (4 RunWalk + Landed)"), PuddleEffectRows, 5);

		// The WaterSplash bundle row (WaterSplash.uc chain-resolved).
		const FWoTSprayerBundleRow* Splash =
		    WoTWeaveFX::Find(FName(TEXT("WaterSplash")));
		if (TestNotNull(TEXT("WaterSplash bundle registered"), Splash))
		{
			TestEqual(TEXT("0.2s one-shot burst"), Splash->BurstSeconds, 0.2f);
			TestEqual(TEXT("Volume 200/s"), Splash->Volume, 200.0f);
			TestEqual(TEXT("Spread 125"), Splash->Spread, 125.0f);
			TestEqual(TEXT("gravity -280"), static_cast<float>(Splash->Gravity.Z),
			          -280.0f);
			TestEqual(TEXT("two droplet templates"), Splash->Templates.Num(), 2);
			TestEqual(TEXT("actor gone ~2.2s"), Splash->SelfDestroySeconds, 2.2f);
		}

		// End-to-end: a WATR floor -> Puddle waves + a burst marker per step.
		SpawnFootstepFloor(World, FVector(-8000.0f, 0.0f, 0.0f),
		                   TEXT("MI_W_MordethT__WatrPool01"));
		Trolloc->SetActorLocation(FVector(-8000.0f, 0.0f, 120.0f));
		RunFootstepPawn(World, Trolloc, 240);
		TestTrue(TEXT("puddle steps play Puddle waves"),
		         CountFootstepMarkers(World, TEXT("WOT/Puddle")) >= 1);
		int32 SplashBursts = 0;
		for (TActorIterator<AWoTWeaveBurst> It(World); It; ++It)
		{
			if (IsValid(*It) && It->Config == FName(TEXT("WaterSplash")))
			{
				++SplashBursts;
			}
		}
		TestTrue(TEXT("each wet step spawns the WaterSplash burst"),
		         SplashBursts >= 1);
	}

	// --- 6. Legion runs its own ST_All waves at any surface. ---
	AWoTNPCCharacter* Legion = WoTCreatureData::SpawnCreature(
	    *World, TEXT("Legion"), FVector(12000.0f, 0.0f, 100.0f), 1, true,
	    AWoTTestNPCCharacter::StaticClass());
	if (TestNotNull(TEXT("Legion spawned"), Legion))
	{
		if (AController* C = Legion->GetController())
		{
			C->UnPossess();
		}
		RunFootstepPawn(World, Legion, 240);
		TestTrue(TEXT("Legion steps its own Leg_RunTex waves"),
		         CountFootstepMarkers(World, TEXT("WoTPawns/Leg_RunTex")) >= 1);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif   // WITH_AUTOMATION_TESTS
