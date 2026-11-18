// Automation specs for the anim foundation: the generated
// sequence table (meta.json → DT_WoTAnimSequences) feeding UWoTVertexAnimComponent, the pawn
// mesh component being a vertex-anim component, and the replicated anim channel driving real
// playback on a proxy. Needs imported content (run Tools/import/import_animdata.py once).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTNPCCharacter.h"
#include "../WoTAnimData.h"
#include "../WoTAnimTable.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTDecoration.h"
#include "../WoTFactionCharacters.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTVertexAnimComponent.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
	// Minimal ticking standalone world supplied by the shared fixture.
	// Fixed 1/60 steps; GFrameCounter must advance per step (the repo's timer-test gotcha).
}

// The generated table row resolves, the component self-feeds on mesh bind, playback runs, and
// the replicated intent path drives a simulated proxy's playback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnimSequenceFeedSpec,
	"WoT.Anim.SequenceFeed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAnimSequenceFeedSpec::RunTest(const FString&)
{
	// --- The generated table (import_animdata.py) has the creature's full sequence set ---
	const TArray<FWoTAnimSequence>* Seqs = WoTAnimData::FindSequences(FName("Trolloc"));
	if (!TestNotNull(TEXT("Trolloc row in DT_WoTAnimSequences (run Tools/import/import_animdata.py)"),
	                 reinterpret_cast<const void*>(Seqs)))
	{
		return false;
	}
	TestTrue(TEXT("full Trolloc sequence set (24 in the .uc)"), Seqs->Num() >= 20);
	auto Has = [Seqs](const TCHAR* Name)
	{
		for (const FWoTAnimSequence& S : *Seqs)
		{
			if (S.Name == FName(Name))
			{
				return true;
			}
		}
		return false;
	};
	TestTrue(TEXT("RUN present"), Has(TEXT("RUN")));
	TestTrue(TEXT("BREATH present"), Has(TEXT("BREATH")));
	TestTrue(TEXT("DEATHF present"), Has(TEXT("DEATHF")));

	// --- The feed also carries the #exec MESH NOTIFY rows: Trolloc DEATHF's
	// carcass swap fires at 0.90 (Trolloc.uc:30-32), AttackRun's damage at 0.75. ---
	auto NotifyFraction = [Seqs](const TCHAR* Seq, const TCHAR* Func) -> float
	{
		for (const FWoTAnimSequence& S : *Seqs)
		{
			if (S.Name == FName(Seq))
			{
				for (const FWoTAnimNotify& N : S.Notifies)
				{
					if (N.Function == FName(Func))
					{
						return N.Fraction;
					}
				}
			}
		}
		return -1.0f;
	};
	TestEqual(TEXT("DEATHF carcass notify at 0.90 (re-run import_animdata.py if missing)"),
	          NotifyFraction(TEXT("DEATHF"), TEXT("TransitionToCarcassNotification")), 0.9f);
	TestEqual(TEXT("AttackRun damage notify at 0.75"),
	          NotifyFraction(TEXT("ATTACKRUN"), TEXT("AttackRunDamageTarget")), 0.75f);

	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Params);

	// --- The pawn mesh component is the vertex-anim surface (the subobject swap) ---
	UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(Pawn->GetMesh());
	if (!TestNotNull(TEXT("pawn mesh component is a UWoTVertexAnimComponent"), Anim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// --- Binding an imported mesh self-populates Sequences (the SetSkeletalMesh feed) ---
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(FSoftObjectPath(
		TEXT("/Game/WoT/Meshes/Trolloc/Trolloc/SkeletalMeshes/Trolloc.Trolloc")).TryLoad());
	if (!TestNotNull(TEXT("imported Trolloc skeletal mesh"), Mesh))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Anim->SetSkeletalMeshAsset(Mesh);
	TestTrue(TEXT("sequences self-populated on mesh bind"), Anim->Sequences.Num() >= 20);

	// --- Playback: the locomotion driver's "Run" matches the sheet's "RUN" (FName is
	// case-insensitive) and frames advance ---
	Anim->LoopAnim(FName("Run"));
	TestEqual(TEXT("Run is playing"), Anim->GetCurrentSequence(), FName("Run"));
	WoTTest::TickWorld(World, 6);
	TestTrue(TEXT("frames advance"), Anim->GetAnimFrame() > 0.0f);

	// --- The replicated intent path now drives real playback on a simulated proxy ---
	AWoTCharacter* Proxy = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, Params);
	UWoTVertexAnimComponent* ProxyAnim = Cast<UWoTVertexAnimComponent>(Proxy->GetMesh());
	ProxyAnim->SetSkeletalMeshAsset(Mesh);
	Proxy->SetRole(ROLE_SimulatedProxy);
	Proxy->RepAnim.Sequence = FName("Run");
	Proxy->RepAnim.Rate = 1.0f;
	Proxy->RepAnim.bLoop = true;
	Proxy->RepAnim.PlayToken++;
	Proxy->OnRep_Anim();
	TestEqual(TEXT("proxy replays the intent on its vertex-anim component"),
	          ProxyAnim->GetCurrentSequence(), FName("Run"));
	WoTTest::TickWorld(World, 6);
	TestTrue(TEXT("proxy frames advance"), ProxyAnim->GetAnimFrame() > 0.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Player faction identity: the four thin faction classes carry the
// shipped WOTPlayer subclass appearance on their CDOs; spawning one binds mesh+scale+sequences;
// the replicated AppearanceClass morph (giMPBattle ServerChangeClass/ChangeClassModelStuff)
// re-skins an existing pawn on any role; Battle spawns TeamClass[Team] and morphs on JoinTeam.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFactionMeshBindingSpec,
	"WoT.Faction.MeshBinding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFactionMeshBindingSpec::RunTest(const FString&)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	{
		UWorld* World = WoTTest::CreateSpecWorld();

		// --- Spawning a faction class binds its shipped appearance (AesSedai.uc:96-104) ---
		AWoTAesSedai* Aes = World->SpawnActor<AWoTAesSedai>(
			FVector(0, 0, 100), FRotator::ZeroRotator, Params);
		UWoTVertexAnimComponent* Anim = Cast<UWoTVertexAnimComponent>(Aes->GetMesh());
		if (!TestNotNull(TEXT("faction pawn has the vertex-anim mesh component"), Anim))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		USkeletalMesh* Body = Anim->GetSkeletalMeshAsset();
		if (TestNotNull(TEXT("AesSedai body mesh bound at spawn"), Body))
		{
			TestEqual(TEXT("bound mesh is the imported AesSedai"),
			          Body->GetFName(), FName("AesSedai"));
		}
		TestEqual(TEXT("UE1 DrawScale 0.95 applied to the MESH component"),
		          static_cast<float>(Anim->GetRelativeScale3D().X), 0.95f);
		TestEqual(TEXT("TeamDescription from the class"), Aes->TeamDescription,
		          FString(TEXT("Aes Sedai")));
		TestTrue(TEXT("bIsFemale carried (AesSedai.uc:97)"), Aes->bIsFemale);
		TestTrue(TEXT("sequence table fed on the faction bind (AesSedai has 31)"),
		         Anim->Sequences.Num() >= 25);

		// --- Authority morph: the mid-match faction change re-skins the EXISTING pawn ---
		Aes->SetAppearanceClass(AWoTHound::StaticClass());
		TestEqual(TEXT("morphed mesh is the Hound"),
		          Anim->GetSkeletalMeshAsset()->GetFName(), FName("Hound"));
		TestEqual(TEXT("morphed DrawScale 1.44708"),
		          static_cast<float>(Anim->GetRelativeScale3D().X), 1.44708f);
		TestEqual(TEXT("morphed TeamDescription"), Aes->TeamDescription,
		          FString(TEXT("Hound")));

		// --- Proxy morph: OnRep applies the replicated class on a simulated proxy ---
		AWoTCharacter* Proxy = World->SpawnActor<AWoTCharacter>(
			FVector(500, 0, 100), FRotator::ZeroRotator, Params);
		Proxy->SetRole(ROLE_SimulatedProxy);
		Proxy->AppearanceClass = AWoTWhitecloak::StaticClass();
		Proxy->OnRep_AppearanceClass();
		UWoTVertexAnimComponent* ProxyAnim = Cast<UWoTVertexAnimComponent>(Proxy->GetMesh());
		if (TestNotNull(TEXT("proxy morphed a body mesh"), ProxyAnim->GetSkeletalMeshAsset()))
		{
			TestEqual(TEXT("proxy morphed to the Whitecloak"),
			          ProxyAnim->GetSkeletalMeshAsset()->GetFName(), FName("Whitecloak"));
		}
		TestEqual(TEXT("proxy morphed DrawScale 1.08"),
		          static_cast<float>(ProxyAnim->GetRelativeScale3D().X), 1.08f);

		// --- Arena fallback: missing/invalid ?Class= retains the canonical default ---
		TestEqual(TEXT("Arena/base fallback pawn class is AesSedai"),
		          GetDefault<AWoTGameMode>()->DefaultPawnClass.Get(),
		          static_cast<UClass*>(AWoTAesSedai::StaticClass()));

		WoTTest::DestroySpecWorld(World);
	}

	// --- Battle: TeamClass[Team] IS the spawn class, and joining a team morphs the pawn ---
	{
		UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());

		AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
		AWoTBattleInfo* BI = World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity);
		BI->NumTeams = 2;
		BI->TeamClass[0] = AWoTForsaken::StaticClass();
		BI->TeamClass[1] = AWoTWhitecloak::StaticClass();
		BI->FinishSpawning(FTransform::Identity);
		Mode->Battle = BI;   // the mode located no map-placed BattleInfo in this bare world

		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->InitPlayerState();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 100), FRotator::ZeroRotator, Params);
		PC->Possess(Pawn);

		Mode->ChangeTeam(PC, 0);
		TestEqual(TEXT("joining team 0 morphs the pawn to its faction (Forsaken)"),
		          Pawn->AppearanceClass.Get(),
		          static_cast<UClass*>(AWoTForsaken::StaticClass()));
		TestEqual(TEXT("morph bound the Forsaken mesh"),
		          Pawn->GetMesh()->GetSkeletalMeshAsset()->GetFName(), FName("Forsaken"));

		TestEqual(TEXT("a respawn would spawn the faction class itself"),
		          Mode->GetDefaultPawnClassForController(PC),
		          static_cast<UClass*>(AWoTForsaken::StaticClass()));

		WoTTest::DestroySpecWorld(World);
	}

	return true;
}

// The AnimationTable registry: the 16 shipped tables as verbatim
// rows, PickRandomSlotAnim's weighted selection, the exact-anim (bNotSlot) path, the
// rate/tween fallbacks, the geometry-filter API, and the shipped-data anomalies
// preserved byte-for-byte.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnimCombatTableSpec,
	"WoT.Anim.CombatTable", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAnimCombatTableSpec::RunTest(const FString&)
{
	// --- Registry shape: all 16 shipped AnimationTable*.uc present. ---
	TArray<FName> Tables;
	WoTAnimTable::GetAllTableNames(Tables);
	TestEqual(TEXT("16 anim tables registered"), Tables.Num(), 16);
	for (const TCHAR* Expected : {TEXT("Trolloc"), TEXT("TrollocSideArm"), TEXT("BATrolloc"),
	                              TEXT("Myrddraal"), TEXT("Hound"), TEXT("Minion"),
	                              TEXT("Soldier"), TEXT("Archer"), TEXT("Warder"),
	                              TEXT("Whitecloak"), TEXT("AesSedai"), TEXT("Forsaken"),
	                              TEXT("Sister"), TEXT("BlackAjahBoss"), TEXT("Legion"),
	                              TEXT("MashFocusPawn")})
	{
		TestNotNull(FString::Printf(TEXT("table %s present"), Expected),
		            WoTAnimTable::Find(FName(Expected)));
	}

	// Trolloc verbatim: 28 authored rows; the Death slot holds the F/B/C variants at
	// 0.05/0.85/0.10 with the authored fall vectors (AnimationTableTrolloc.uc:16-18).
	const TArray<FWoTAnimTableEntry>* Trolloc = WoTAnimTable::Find(TEXT("Trolloc"));
	if (TestNotNull(TEXT("Trolloc table"), Trolloc))
	{
		TestEqual(TEXT("Trolloc has 28 authored rows"), Trolloc->Num(), 28);
		int32 DeathRows = 0;
		for (const FWoTAnimTableEntry& E : *Trolloc)
		{
			if (E.Slot == FName(TEXT("Death")))
			{
				++DeathRows;
				if (E.AnimName == FName(TEXT("DEATHB")))
				{
					TestEqual(TEXT("DEATHB odds 0.85"), E.Odds, 0.85f);
					TestEqual(TEXT("DEATHB falls backward (X=-4.5)"),
					          static_cast<float>(E.EndVector.X), -4.5f);
					TestEqual(TEXT("DEATHB rate 0.35"), E.Rate, 0.35f);
				}
			}
		}
		TestEqual(TEXT("Trolloc Death slot has 3 variants"), DeathRows, 3);
	}

	// The MashFocusPawn table is deliberately EMPTY (all plays no-op — Mashadar
	// animates nothing), and unknown names miss quietly.
	const TArray<FWoTAnimTableEntry>* Mash = WoTAnimTable::Find(TEXT("MashFocusPawn"));
	if (TestNotNull(TEXT("MashFocusPawn table resolves"), Mash))
	{
		TestEqual(TEXT("MashFocusPawn table is empty"), Mash->Num(), 0);
	}
	TestNull(TEXT("unknown table misses quietly"), WoTAnimTable::Find(TEXT("NoSuchTable")));
	TestNull(TEXT("NAME_None misses quietly"), WoTAnimTable::Find(NAME_None));

	// TrollocSideArm = Trolloc with ONLY index 30 overridden — UnrealScript
	// defaultproperties merge in place, so the row keeps the parent's slot/odds/rate
	// (both .uc files pin the ProjectileAttack index for exactly this reason).
	const TArray<FWoTAnimTableEntry>* SideArm = WoTAnimTable::Find(TEXT("TrollocSideArm"));
	if (TestNotNull(TEXT("TrollocSideArm table"), SideArm))
	{
		TestEqual(TEXT("SideArm row count matches Trolloc"), SideArm->Num(), 28);
		for (const FWoTAnimTableEntry& E : *SideArm)
		{
			if (E.Index == 30)
			{
				TestEqual(TEXT("SideArm 30 overrides the throw anim"),
				          E.AnimName, FName(TEXT("ATTACKTHROW1")));
				TestEqual(TEXT("SideArm 30 keeps the parent slot"),
				          E.Slot, FName(TEXT("ProjectileAttack")));
				TestEqual(TEXT("SideArm 30 keeps the parent odds"), E.Odds, 1.0f);
				TestEqual(TEXT("SideArm 30 keeps the parent rate"), E.Rate, 0.5f);
			}
		}
	}

	// Shipped data bug preserved verbatim: Hound index 41's AnimName is the fused
	// "DodgeRightAnimRate" (no such sequence), with no rate (-> 1.0 at play).
	const TArray<FWoTAnimTableEntry>* Hound = WoTAnimTable::Find(TEXT("Hound"));
	if (TestNotNull(TEXT("Hound table"), Hound))
	{
		bool bSawAnomaly = false;
		for (const FWoTAnimTableEntry& E : *Hound)
		{
			if (E.Index == 41)
			{
				bSawAnomaly = true;
				TestEqual(TEXT("Hound 41 keeps the fused shipped name"),
				          E.AnimName, FName(TEXT("DodgeRightAnimRate")));
				TestEqual(TEXT("Hound 41 rate resolves to DefaultAnimRate 1.0"),
				          WoTAnimTable::ResolveRate(&E), 1.0f);
			}
		}
		TestTrue(TEXT("Hound 41 anomaly present"), bSawAnomaly);
	}

	// --- Selection semantics. ---
	// A weighted slot only ever returns its own candidates (Trolloc MeleeAttack:
	// AttackRun 0.66 / ATTACKRUNB 0.33), and both arms are reachable.
	bool bSawRun = false, bSawRunB = false;
	for (int32 i = 0; i < 64; ++i)
	{
		const FWoTAnimTableEntry* Pick =
		    WoTAnimTable::PickSlotAnim(TEXT("Trolloc"), TEXT("MeleeAttack"));
		if (!TestNotNull(TEXT("a multi-row slot always picks"), Pick))
		{
			break;
		}
		bSawRun |= Pick->AnimName == FName(TEXT("AttackRun"));
		bSawRunB |= Pick->AnimName == FName(TEXT("ATTACKRUNB"));
		TestTrue(TEXT("pick stays inside the slot"),
		         Pick->AnimName == FName(TEXT("AttackRun")) ||
		         Pick->AnimName == FName(TEXT("ATTACKRUNB")));
	}
	TestTrue(TEXT("both weighted arms reachable (0.66/0.33 over 64 rolls)"), bSawRun && bSawRunB);

	// bNotSlot picks an exact anim by name, ignoring its odds (SCRATCH odds 0.005).
	const FWoTAnimTableEntry* Exact = WoTAnimTable::PickSlotAnim(
	    TEXT("Trolloc"), TEXT("SCRATCH"), nullptr, /*bNotSlot*/ true);
	if (TestNotNull(TEXT("bNotSlot resolves the exact anim"), Exact))
	{
		TestEqual(TEXT("bNotSlot ignores the odds gate"), Exact->Odds, 0.005f);
	}
	TestNull(TEXT("empty slot returns null"),
	         WoTAnimTable::PickSlotAnim(TEXT("Trolloc"), TEXT("NoSuchSlot")));

	// Tween fallback chain: caller > entry > 0.2 default.
	TestEqual(TEXT("tween falls back to DefaultAnimTweenToTime"),
	          WoTAnimTable::ResolveTweenTime(nullptr), 0.2f);

	// --- Death geometry filter (AnimCanBeUsed): a zero end-vector is never filtered;
	// in open space an authored fall vector fits. (The blocked case rides the death
	// pick in WoT.AI.CombatAnims, where a real floored world exists.) ---
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		    FVector(0, 0, 100), FRotator::ZeroRotator);
		if (TestNotNull(TEXT("filter pawn spawned"), Pawn))
		{
			FWoTAnimTableEntry Forward;
			Forward.EndVector = FVector(2.0f, 0.0f, 0.0f);
			TestTrue(TEXT("open space: the death anim fits"),
			         WoTAnimTable::AnimCanBeUsed(*Pawn, Forward));
			FWoTAnimTableEntry NoVector;
			TestTrue(TEXT("zero end-vector is never filtered"),
			         WoTAnimTable::AnimCanBeUsed(*Pawn, NoVector));
		}
		WoTTest::DestroySpecWorld(World);
	}

	// --- The faction classes carry their shipped AnimationTableClass refs (players
	// pick death/hit variants from their own tables). ---
	TestEqual(TEXT("AesSedai faction table"),
	          AWoTAesSedai::StaticClass()->GetDefaultObject<AWoTCharacter>()->AnimTableName,
	          FName(TEXT("AesSedai")));
	TestEqual(TEXT("Forsaken faction table"),
	          AWoTForsaken::StaticClass()->GetDefaultObject<AWoTCharacter>()->AnimTableName,
	          FName(TEXT("Forsaken")));

	return true;
}

// Decoration ambient animation — the WOTDecoration.uc bAutoAnimate loop
// (DragonFly's hardcoded LoopAnim override folds to stamped data) and the
// level-placed corpse props' held pose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnimDecoAmbientSpec,
	"WoT.Anim.DecoAmbient", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTAnimDecoAmbientSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	USkeletalMesh* Fly = Cast<USkeletalMesh>(FSoftObjectPath(TEXT(
	    "/Game/WoT/Meshes/DragonFly/DragonFly/SkeletalMeshes/DragonFly.DragonFly"))
	    .TryLoad());
	if (TestNotNull(TEXT("DragonFly mesh imported"), Fly))
	{
		AWoTDecoration* D = World->SpawnActorDeferred<AWoTDecoration>(
		    AWoTDecoration::StaticClass(), FTransform::Identity);
		D->Mesh->SetSkeletalMeshAsset(Fly);   // self-feeds Sequences
		D->bAutoAnimate = true;
		D->AnimationSequence = TEXT("DRAGONFLY");
		D->MinAnimRate = 1.5f;
		D->MaxAnimRate = 2.0f;
		D->FinishSpawning(FTransform::Identity);
		WoTTest::TickWorld(World, 5);
		TestEqual(TEXT("wing loop playing"), D->Mesh->GetCurrentSequence(),
		          FName(TEXT("DRAGONFLY")));
		TestTrue(TEXT("wing loop advances"), D->Mesh->GetAnimFrame() > 0.0f);
	}

	USkeletalMesh* Hound = Cast<USkeletalMesh>(FSoftObjectPath(TEXT(
	    "/Game/WoT/Meshes/Hound/Hound/SkeletalMeshes/Hound.Hound")).TryLoad());
	if (TestNotNull(TEXT("Hound mesh imported"), Hound))
	{
		// Battle4_01's WOTCarcassHound prop: AnimSequence=Swim held at frame 0.
		AWoTDecoration* C = World->SpawnActorDeferred<AWoTDecoration>(
		    AWoTDecoration::StaticClass(), FTransform::Identity);
		C->Mesh->SetSkeletalMeshAsset(Hound);
		C->PoseSequence = TEXT("SWIM");
		C->PoseFrame = 0.0f;
		C->FinishSpawning(FTransform::Identity);
		const float Held = C->Mesh->GetAnimFrame();
		WoTTest::TickWorld(World, 10);
		TestEqual(TEXT("pose is not playing"), C->Mesh->GetCurrentSequence(),
		          FName(NAME_None));
		TestEqual(TEXT("pose frame holds"), C->Mesh->GetAnimFrame(), Held,
		          KINDA_SMALL_NUMBER);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFirstPersonBodyHiddenSpec,
	"WoT.Pawn.FirstPersonBodyHidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFirstPersonBodyHiddenSpec::RunTest(const FString&)
{
	// Retail first person never draws the viewer's own pawn; the owning client
	// must not see its own body mesh (other clients are unaffected — owner-only
	// visibility). Regression pins the "full body in first person" bug and
	// the shot-harness spawn-start photobomb.
	const AWoTCharacter* Default = GetDefault<AWoTCharacter>();
	const USkeletalMeshComponent* Body = Default->GetMesh();
	if (!TestNotNull(TEXT("pawn CDO has a mesh component"), Body))
	{
		return false;
	}
	TestTrue(TEXT("own body mesh is owner-no-see (first-person parity)"),
	         Body->bOwnerNoSee);
	// A citadel troop is owned by the player who placed it; owner-no-see would hide
	// it from that player's first-person view while its weapon still drew.
	const USkeletalMeshComponent* NPCBody = GetDefault<AWoTNPCCharacter>()->GetMesh();
	if (TestNotNull(TEXT("NPC CDO has a mesh component"), NPCBody))
	{
		TestFalse(TEXT("NPC body stays visible to the player who owns it"),
		          NPCBody->bOwnerNoSee);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP18GroundedSelectionSpec,
	"WoT.P18.PlayerAnimation.GroundedSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP18GroundedSelectionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAesSedai* Pawn = World->SpawnActor<AWoTAesSedai>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator, Params);
	UWoTCharacterMovement* Move = Cast<UWoTCharacterMovement>(
		Pawn ? Pawn->GetCharacterMovement() : nullptr);
	UWoTVertexAnimComponent* Anim = Pawn
		? Cast<UWoTVertexAnimComponent>(Pawn->GetMesh()) : nullptr;
	if (!TestNotNull(TEXT("P18 Aes Sedai pawn"), Pawn)
		|| !TestNotNull(TEXT("P18 retail movement component"), Move)
		|| !TestNotNull(TEXT("P18 vertex animation component"), Anim))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto Drive = [Pawn, Move](const FVector& Velocity,
		const FVector& Acceleration, bool bWalking)
	{
		Move->SetMovementMode(MOVE_Walking);
		Move->Velocity = Velocity;
		Move->SetAccelerationForTesting(Acceleration);
		Move->SetWalkingRequested(bWalking);
		Pawn->Tick(0.0f);
	};

	Drive(FVector(200.0f, 0.0f, 0.0f),
		FVector(2048.0f, 0.0f, 0.0f), false);
	TestEqual(TEXT("forward run sequence"), Pawn->RepAnim.Sequence,
		FName(TEXT("Run")));
	TestEqual(TEXT("run rate uses current/default GroundSpeed, not velocity"),
		Pawn->RepAnim.Rate, 0.5f, KINDA_SMALL_NUMBER);
	TestFalse(TEXT("waiting-to-run enters through TweenToRunning"),
		Pawn->RepAnim.bLoop != 0);
	TestFalse(TEXT("TweenToRunning plays rather than holds the target pose"),
		Pawn->RepAnim.bTweenOnly != 0);
	TestEqual(TEXT("waiting-to-run uses the retail 0.1 second tween"),
		Pawn->RepAnim.TweenTime, 0.1f, KINDA_SMALL_NUMBER);
	TestEqual(TEXT("authority immediately drives the component run sequence"),
		Anim->GetCurrentSequence(), FName(TEXT("Run")));
	const uint8 ForwardToken = Pawn->RepAnim.PlayToken;
	Pawn->Tick(0.0f);
	TestEqual(TEXT("unchanged run state stamps once"),
		Pawn->RepAnim.PlayToken, ForwardToken);
	Pawn->Tick(1.2f);
	TestTrue(TEXT("TweenToRunning completes into the looping run"),
		Pawn->RepAnim.bLoop != 0);
	TestEqual(TEXT("completed run keeps the setting/default rate"),
		Pawn->RepAnim.Rate, 0.5f, KINDA_SMALL_NUMBER);

	Move->MaxWalkSpeed = 300.0f;
	Pawn->Tick(0.0f);
	TestEqual(TEXT("a changed GroundSpeed restamps current/default rate"),
		Pawn->RepAnim.Rate, 0.375f, KINDA_SMALL_NUMBER);

	Drive(FVector(0.0f, -200.0f, 0.0f),
		FVector(0.0f, -2048.0f, 0.0f), false);
	TestEqual(TEXT("left strafe run sequence"), Pawn->RepAnim.Sequence,
		FName(TEXT("RunL")));
	Drive(FVector(0.0f, 200.0f, 0.0f),
		FVector(0.0f, 2048.0f, 0.0f), false);
	TestEqual(TEXT("right strafe run sequence"), Pawn->RepAnim.Sequence,
		FName(TEXT("RunR")));
	Drive(FVector(-200.0f, 0.0f, 0.0f),
		FVector(-2048.0f, 0.0f, 0.0f), false);
	TestEqual(TEXT("backpedal uses the shipped forward run slot"),
		Pawn->RepAnim.Sequence, FName(TEXT("Run")));

	Drive(FVector(100.0f, 0.0f, 0.0f),
		FVector(2048.0f, 0.0f, 0.0f), true);
	TestEqual(TEXT("walking sequence"), Pawn->RepAnim.Sequence,
		FName(TEXT("Walk")));
	TestEqual(TEXT("walk rate is authored 0.6 times 300/400 GroundSpeed"),
		Pawn->RepAnim.Rate, 0.45f, KINDA_SMALL_NUMBER);

	Drive(FVector::ZeroVector, FVector::ZeroVector, false);
	TestEqual(TEXT("movement exit tweens to retail breathing pose"),
		Pawn->RepAnim.Sequence, FName(TEXT("Breath")));
	TestTrue(TEXT("movement exit uses TweenToWaiting's held pose"),
		Pawn->RepAnim.bTweenOnly != 0);
	TestEqual(TEXT("movement exit uses the retail 0.2 second tween"),
		Pawn->RepAnim.TweenTime, 0.2f, KINDA_SMALL_NUMBER);
	Pawn->Tick(0.21f);
	TestTrue(TEXT("TweenToWaiting completes into a shipped Breath/Look idle"),
		Pawn->RepAnim.Sequence == FName(TEXT("Breath"))
			|| Pawn->RepAnim.Sequence == FName(TEXT("Look")));
	TestEqual(TEXT("completed waiting pose loops"), Pawn->RepAnim.bLoop, uint8(1));

	Pawn->SetPlayerAnimFireHeldForTesting(true);
	Drive(FVector(200.0f, 0.0f, 0.0f),
		FVector(2048.0f, 0.0f, 0.0f), false);
	Pawn->Tick(1.2f);
	Drive(FVector::ZeroVector, FVector::ZeroVector, false);
	TestEqual(TEXT("TweenToWaiting while firing selects Attack"),
		Pawn->RepAnim.Sequence, FName(TEXT("Attack")));
	TestTrue(TEXT("waiting fire pose remains a TweenAnim hold"),
		Pawn->RepAnim.bTweenOnly != 0);
	TestEqual(TEXT("waiting fire pose uses the retail 0.2 second tween"),
		Pawn->RepAnim.TweenTime, 0.2f, KINDA_SMALL_NUMBER);
	Pawn->Tick(0.21f);
	TestEqual(TEXT("held fire retains Attack after TweenToWaiting completes"),
		Pawn->RepAnim.Sequence, FName(TEXT("Attack")));
	TestTrue(TEXT("held Attack remains the tweened pose rather than an idle loop"),
		Pawn->RepAnim.bTweenOnly != 0);
	TestEqual(TEXT("held Attack remains active on the component"),
		Anim->GetCurrentSequence(), FName(TEXT("Attack")));
	Pawn->SetPlayerAnimFireHeldForTesting(false);
	Pawn->Tick(0.0f);
	TestTrue(TEXT("fire release returns to a shipped idle loop"),
		Pawn->RepAnim.Sequence == FName(TEXT("Breath"))
			|| Pawn->RepAnim.Sequence == FName(TEXT("Look")));

	const TArray<FWoTAnimTableEntry>* Table = WoTAnimTable::Find(TEXT("AesSedai"));
	if (TestNotNull(TEXT("activated Aes Sedai animation table"), Table))
	{
		auto HasSlot = [Table](FName Slot)
		{
			return Table->ContainsByPredicate(
				[Slot](const FWoTAnimTableEntry& Entry) { return Entry.Slot == Slot; });
		};
		TestFalse(TEXT("shipped player table has no invented turn slot"),
			HasSlot(TEXT("Turn")));
		TestFalse(TEXT("shipped player table has no invented crouch slot"),
			HasSlot(TEXT("Crouch")));
		TestFalse(TEXT("shipped player table has no invented crawl slot"),
			HasSlot(TEXT("Crawl")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP18AirWaterLandingSelectionSpec,
	"WoT.P18.PlayerAnimation.AirWaterLandingSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP18AirWaterLandingSelectionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto SpawnPawn = [&]()
	{
		return World->SpawnActor<AWoTAesSedai>(
			FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator, Params);
	};

	AWoTAesSedai* FallPawn = SpawnPawn();
	UCharacterMovementComponent* FallMove = FallPawn->GetCharacterMovement();
	FallMove->SetMovementMode(MOVE_Walking);
	FallMove->SetMovementMode(MOVE_Falling);
	TestEqual(TEXT("walking off a ledge selects Fall"),
		FallPawn->RepAnim.Sequence, FName(TEXT("Fall")));
	TestEqual(TEXT("Fall is a held tween pose"),
		FallPawn->RepAnim.bTweenOnly, uint8(1));
	TestEqual(TEXT("Fall uses its authored tween"),
		FallPawn->RepAnim.TweenTime, 0.2f, KINDA_SMALL_NUMBER);
	const uint8 FallToken = FallPawn->RepAnim.PlayToken;
	FallPawn->Tick(0.0f);
	TestEqual(TEXT("falling locomotion does not stomp Fall"),
		FallPawn->RepAnim.PlayToken, FallToken);

	AWoTAesSedai* JumpPawn = SpawnPawn();
	JumpPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	JumpPawn->OnJumped_Implementation();
	TestEqual(TEXT("a real jump selects Jump"), JumpPawn->RepAnim.Sequence,
		FName(TEXT("Jump")));
	TestEqual(TEXT("Jump is a held tween pose"),
		JumpPawn->RepAnim.bTweenOnly, uint8(1));
	TestEqual(TEXT("Jump uses its authored tween"),
		JumpPawn->RepAnim.TweenTime, 0.4f, KINDA_SMALL_NUMBER);

	AWoTAesSedai* SwimPawn = SpawnPawn();
	UWoTVertexAnimComponent* SwimAnim = Cast<UWoTVertexAnimComponent>(
		SwimPawn->GetMesh());
	SwimPawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	TestEqual(TEXT("entering water selects Swim"), SwimPawn->RepAnim.Sequence,
		FName(TEXT("Swim")));
	const uint8 SwimToken = SwimPawn->RepAnim.PlayToken;
	SwimPawn->Tick(0.0f);
	TestEqual(TEXT("treading water does not become Breath"),
		SwimPawn->RepAnim.Sequence, FName(TEXT("Swim")));
	TestEqual(TEXT("unchanged swim entry stamps once"),
		SwimPawn->RepAnim.PlayToken, SwimToken);
	WoTTest::TickWorld(World, 30);
	TestEqual(TEXT("swim entry completes into the component loop"),
		SwimAnim ? SwimAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Swim")));
	TestEqual(TEXT("swim loop uses the authored table rate"),
		SwimPawn->RepAnim.Rate, 0.34f, KINDA_SMALL_NUMBER);
	if (SwimAnim)
	{
		TestEqual(TEXT("component retains the authored swim loop rate"),
			SwimAnim->GetPlayRateForTesting(), 0.34f, KINDA_SMALL_NUMBER);
	}

	AWoTAesSedai* LandPawn = SpawnPawn();
	LandPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	LandPawn->GetCharacterMovement()->Velocity = FVector(0.0f, 0.0f, -300.0f);
	FHitResult FloorHit;
	FloorHit.Normal = FVector::UpVector;
	LandPawn->Landed(FloorHit);
	TestEqual(TEXT("minor landing selects Land"), LandPawn->RepAnim.Sequence,
		FName(TEXT("Landed")));
	TestEqual(TEXT("minor Land is a held tween pose"),
		LandPawn->RepAnim.bTweenOnly, uint8(1));
	TestEqual(TEXT("Land uses its authored tween"),
		LandPawn->RepAnim.TweenTime, 0.1f, KINDA_SMALL_NUMBER);
	// CharacterMovement normally leaves MOVE_Falling before delivering Landed.
	// This direct-call fixture must model that edge explicitly; flying avoids
	// the floorless spec world immediately re-entering Fall while time advances.
	LandPawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 16);
	TestTrue(TEXT("minor Land completion routes through TweenToWaiting"),
		LandPawn->RepAnim.bTweenOnly != 0);
	TestEqual(TEXT("minor Land exit uses the retail 0.2 second waiting tween"),
		LandPawn->RepAnim.TweenTime, 0.2f, KINDA_SMALL_NUMBER);
	TestTrue(TEXT("minor Land exit selects a shipped waiting pose"),
		LandPawn->RepAnim.Sequence == FName(TEXT("Breath"))
			|| LandPawn->RepAnim.Sequence == FName(TEXT("Look")));
	WoTTest::TickWorld(World, 20);
	TestEqual(TEXT("minor Land waiting tween completes into its loop"),
		LandPawn->RepAnim.bLoop, uint8(1));

	AWoTAesSedai* HardLandPawn = SpawnPawn();
	UWoTVertexAnimComponent* HardLandAnim = Cast<UWoTVertexAnimComponent>(
		HardLandPawn->GetMesh());
	// Close the NPC-style probability gate so an accidental application of
	// WOTPawn hit eligibility to this WOTPlayer landing always adds a second
	// token. Retail's 22-damage fall is below WOTPlayer.PlayHit's strict >25%
	// threshold, so the Land presentation remains the only valid stamp.
	HardLandPawn->ChanceOfHitAnim = 1.0f;
	HardLandPawn->SetPlayerAnimFireHeldForTesting(true);
	HardLandPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	HardLandPawn->GetCharacterMovement()->Velocity = FVector(0.0f, 0.0f, -1200.0f);
	const uint8 BeforeHardLandToken = HardLandPawn->RepAnim.PlayToken;
	HardLandPawn->Landed(FloorHit);
	TestEqual(TEXT("one hard-landing edge stamps exactly one animation intent"),
		HardLandPawn->RepAnim.PlayToken,
		static_cast<uint8>(BeforeHardLandToken + 1));
	TestEqual(TEXT("hard landing immediately plays on the authority component"),
		HardLandAnim ? HardLandAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Landed")));
	TestFalse(TEXT("hard landing clears the authority/local fire latch"),
		HardLandPawn->IsPlayerAnimFireHeldForTesting());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP18NetworkPlaybackRolesSpec,
	"WoT.P18.PlayerAnimation.NetworkPlaybackRoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP18NetworkPlaybackRolesSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("P18 authority owner and simulated animation playback"));
	if (!TestTrue(TEXT("P18 dedicated server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(
		FVector(-2000.0f, 0.0f, 5000.0f), FRotator::ZeroRotator);
	ServerWorld->SpawnActor<AWoTPlayerStart>(
		FVector(2000.0f, 0.0f, 5000.0f), FRotator::ZeroRotator);
	if (!TestTrue(TEXT("P18 autonomous owner connects"),
			Worlds.CreateAndConnectClient())
		|| !TestTrue(TEXT("P18 simulated observer connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* AuthorityPawn = ServerPC
		? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
	AWoTPlayerController* OwnerPC = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTCharacter* OwnerPawn = OwnerPC
		? Cast<AWoTCharacter>(OwnerPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("P18 authority pawn"), AuthorityPawn)
		|| !TestNotNull(TEXT("P18 autonomous owner pawn"), OwnerPawn))
	{
		return false;
	}
	TestEqual(TEXT("P18 owner uses the autonomous role"),
		OwnerPawn->GetLocalRole(), ROLE_AutonomousProxy);
	const int32 OwnerPlayerId = OwnerPawn->GetPlayerState()
		? OwnerPawn->GetPlayerState()->GetPlayerId() : INDEX_NONE;
	AWoTCharacter* SimulatedPawn = nullptr;
	TestTrue(TEXT("P18 observer resolves the owner's simulated proxy"),
		Worlds.TickUntil([&Worlds, &SimulatedPawn, OwnerPlayerId]()
		{
			for (TActorIterator<AWoTCharacter> It(
				Worlds.Clients[1].GetWorld()); It; ++It)
			{
				if (It->GetLocalRole() == ROLE_SimulatedProxy
					&& It->GetPlayerState()
					&& It->GetPlayerState()->GetPlayerId() == OwnerPlayerId)
				{
					SimulatedPawn = *It;
					return true;
				}
			}
			return false;
		}, 180));
	if (!SimulatedPawn)
	{
		return false;
	}
	auto DriveRun = [](AWoTCharacter* Pawn)
	{
		UWoTCharacterMovement* Move = CastChecked<UWoTCharacterMovement>(
			Pawn->GetCharacterMovement());
		// Entry has no floor. Flying keeps the same directional selector inputs
		// stable while network worlds tick instead of replacing Run with Fall.
		Move->SetMovementMode(MOVE_Flying);
		Move->Velocity = FVector(200.0f, 0.0f, 0.0f);
		Move->SetAccelerationForTesting(FVector(2048.0f, 0.0f, 0.0f));
		Pawn->Tick(0.0f);
	};
	DriveRun(AuthorityPawn);
	DriveRun(OwnerPawn);
	AuthorityPawn->HoldLocomotionFor(3600.0f);
	OwnerPawn->HoldLocomotionFor(3600.0f);
	UWoTVertexAnimComponent* AuthorityAnim =
		Cast<UWoTVertexAnimComponent>(AuthorityPawn->GetMesh());
	UWoTVertexAnimComponent* OwnerAnim =
		Cast<UWoTVertexAnimComponent>(OwnerPawn->GetMesh());
	UWoTVertexAnimComponent* SimulatedAnim =
		Cast<UWoTVertexAnimComponent>(SimulatedPawn->GetMesh());
	TestEqual(TEXT("authority component plays selected Run"),
		AuthorityAnim ? AuthorityAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Run")));
	TestEqual(TEXT("authority publishes selected Run intent"),
		AuthorityPawn->RepAnim.Sequence, FName(TEXT("Run")));
	TestEqual(TEXT("autonomous owner component predicts selected Run"),
		OwnerAnim ? OwnerAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Run")));
	AuthorityPawn->ForceNetUpdate();
	const bool bSimulatedReplayed = Worlds.TickUntil([SimulatedAnim]()
		{
			return SimulatedAnim
				&& SimulatedAnim->GetCurrentSequence() == FName(TEXT("Run"));
		}, 180);
	TestEqual(TEXT("simulated proxy receives replicated Run intent"),
		SimulatedPawn->RepAnim.Sequence, FName(TEXT("Run")));
	TestNotNull(TEXT("simulated proxy has the Run sequence bound"),
		SimulatedAnim ? SimulatedAnim->FindSequence(FName(TEXT("Run"))) : nullptr);
	TestTrue(TEXT("simulated proxy component replays replicated Run"),
		bSimulatedReplayed);

	auto AnimOf = [](AWoTCharacter* Pawn)
	{
		return Pawn ? Cast<UWoTVertexAnimComponent>(Pawn->GetMesh()) : nullptr;
	};
	auto PrepareTransition = [&AnimOf](AWoTCharacter* Pawn)
	{
		CastChecked<UCharacterMovementComponent>(Pawn->GetCharacterMovement())
			->SetMovementMode(MOVE_Flying);
		if (UWoTVertexAnimComponent* Anim = AnimOf(Pawn))
		{
			Anim->LoopAnim(FName(TEXT("Breath")), 1.0f);
		}
	};
	auto ReplicateSequence = [this, &Worlds, AuthorityPawn, SimulatedPawn,
		SimulatedAnim](FName Sequence, const TCHAR* Label)
	{
		AuthorityPawn->ForceNetUpdate();
		TestTrue(FString::Printf(TEXT("simulated proxy replays replicated %s"), Label),
			Worlds.TickUntil([SimulatedAnim, Sequence]()
			{
				return SimulatedAnim
					&& SimulatedAnim->GetCurrentSequence() == Sequence;
			}, 180));
		TestEqual(FString::Printf(TEXT("simulated proxy receives %s intent"), Label),
			SimulatedPawn->RepAnim.Sequence, Sequence);
	};
	UWoTVertexAnimComponent* OwnerSpecialAnim = AnimOf(OwnerPawn);

	// Fall: a local movement-mode callback drives authority and owner, while a
	// simulated proxy remains on its baseline until the authority intent arrives.
	for (AWoTCharacter* Pawn : {AuthorityPawn, OwnerPawn, SimulatedPawn})
	{
		PrepareTransition(Pawn);
	}
	SimulatedPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	TestEqual(TEXT("simulated proxy does not locally drive Fall"),
		SimulatedAnim ? SimulatedAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Breath")));
	OwnerPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	TestEqual(TEXT("autonomous owner locally drives Fall"),
		OwnerSpecialAnim ? OwnerSpecialAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Fall")));
	const uint8 BeforeAuthorityFall = AuthorityPawn->RepAnim.PlayToken;
	AuthorityPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	TestEqual(TEXT("authority Fall stamps exactly once"),
		AuthorityPawn->RepAnim.PlayToken,
		static_cast<uint8>(BeforeAuthorityFall + 1));
	TestEqual(TEXT("authority locally drives Fall"),
		AuthorityAnim ? AuthorityAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Fall")));
	ReplicateSequence(FName(TEXT("Fall")), TEXT("Fall"));

	// Minor Land uses the same role contract through Landed/PlayPlayerTweenPose.
	FHitResult FloorHit;
	FloorHit.Normal = FVector::UpVector;
	for (AWoTCharacter* Pawn : {AuthorityPawn, OwnerPawn, SimulatedPawn})
	{
		PrepareTransition(Pawn);
		Pawn->GetCharacterMovement()->Velocity = FVector(0.0f, 0.0f, -300.0f);
	}
	SimulatedPawn->Landed(FloorHit);
	TestEqual(TEXT("simulated proxy does not locally drive Land"),
		SimulatedAnim ? SimulatedAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Breath")));
	OwnerPawn->Landed(FloorHit);
	TestEqual(TEXT("autonomous owner locally drives Land"),
		OwnerSpecialAnim ? OwnerSpecialAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Landed")));
	const uint8 BeforeAuthorityLand = AuthorityPawn->RepAnim.PlayToken;
	AuthorityPawn->Landed(FloorHit);
	TestEqual(TEXT("authority Land stamps exactly once"),
		AuthorityPawn->RepAnim.PlayToken,
		static_cast<uint8>(BeforeAuthorityLand + 1));
	TestEqual(TEXT("authority locally drives Land"),
		AuthorityAnim ? AuthorityAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Landed")));
	ReplicateSequence(FName(TEXT("Landed")), TEXT("Land"));

	// Swim entry and its callback are likewise local only for authority/owner.
	for (AWoTCharacter* Pawn : {AuthorityPawn, OwnerPawn, SimulatedPawn})
	{
		PrepareTransition(Pawn);
	}
	SimulatedPawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	TestEqual(TEXT("simulated proxy does not locally drive Swim"),
		SimulatedAnim ? SimulatedAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Breath")));
	OwnerPawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	TestEqual(TEXT("autonomous owner locally drives Swim"),
		OwnerSpecialAnim ? OwnerSpecialAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Swim")));
	const uint8 BeforeAuthoritySwim = AuthorityPawn->RepAnim.PlayToken;
	AuthorityPawn->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	TestEqual(TEXT("authority Swim stamps exactly once"),
		AuthorityPawn->RepAnim.PlayToken,
		static_cast<uint8>(BeforeAuthoritySwim + 1));
	TestEqual(TEXT("authority locally drives Swim"),
		AuthorityAnim ? AuthorityAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Swim")));
	ReplicateSequence(FName(TEXT("Swim")), TEXT("Swim"));

	// RepAnim is COND_SkipOwner, so the landing edge itself must clear the
	// autonomous owner's locally predicted fire latch.
	PrepareTransition(OwnerPawn);
	OwnerPawn->SetPlayerAnimFireHeldForTesting(true);
	OwnerPawn->GetCharacterMovement()->Velocity = FVector(0.0f, 0.0f, -1200.0f);
	OwnerPawn->Landed(FloorHit);
	TestFalse(TEXT("autonomous hard landing clears the local fire latch"),
		OwnerPawn->IsPlayerAnimFireHeldForTesting());
	TestEqual(TEXT("autonomous landing while firing ends Attack with Land"),
		OwnerSpecialAnim ? OwnerSpecialAnim->GetCurrentSequence() : NAME_None,
		FName(TEXT("Landed")));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP18FireAndGuardSelectionSpec,
	"WoT.P18.PlayerAnimation.FireAndGuardSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTP18FireAndGuardSelectionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAesSedai* Pawn = World->SpawnActor<AWoTAesSedai>(
		FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator, Params);
	UWoTCharacterMovement* Move = CastChecked<UWoTCharacterMovement>(
		Pawn->GetCharacterMovement());
	Move->SetMovementMode(MOVE_Walking);
	Move->Velocity = FVector(0.0f, -200.0f, 0.0f);
	Move->SetAccelerationForTesting(FVector(0.0f, -2048.0f, 0.0f));
	Pawn->Tick(0.0f);

	Pawn->ServerUseAngreal_Implementation(FRotator::ZeroRotator);
	Pawn->Tick(0.0f);
	TestEqual(TEXT("firing while strafing left selects AttackRunL"),
		Pawn->RepAnim.Sequence, FName(TEXT("AttackRunL")));
	TestEqual(TEXT("AttackRunL preserves the authored/speed-scaled rate"),
		Pawn->RepAnim.Rate, 0.5f, KINDA_SMALL_NUMBER);
	const uint8 FiringToken = Pawn->RepAnim.PlayToken;
	Pawn->Tick(0.0f);
	TestEqual(TEXT("unchanged firing movement stamps once"),
		Pawn->RepAnim.PlayToken, FiringToken);

	Pawn->ServerCeaseUsingAngreal_Implementation();
	Pawn->Tick(0.0f);
	TestEqual(TEXT("ceasing fire restores RunL"), Pawn->RepAnim.Sequence,
		FName(TEXT("RunL")));

	Pawn->HoldLocomotionFor(1.0f);
	const uint8 HeldToken = Pawn->RepAnim.PlayToken;
	Move->Velocity = FVector(0.0f, 200.0f, 0.0f);
	Move->SetAccelerationForTesting(FVector(0.0f, 2048.0f, 0.0f));
	Pawn->Tick(0.0f);
	TestEqual(TEXT("one-shot hold blocks locomotion restamp"),
		Pawn->RepAnim.PlayToken, HeldToken);

	Pawn->bDead = true;
	Pawn->HoldLocomotionFor(0.0f);
	const uint8 DeadToken = Pawn->RepAnim.PlayToken;
	Pawn->Tick(0.0f);
	TestEqual(TEXT("death blocks locomotion restamp"),
		Pawn->RepAnim.PlayToken, DeadToken);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDeathAnimPlaysOnAuthoritySpec,
	"WoT.Pawn.DeathAnimPlaysOnAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTDeathAnimPlaysOnAuthoritySpec::RunTest(const FString&)
{
	// Killed pawns stood frozen in their last pose until
	// the carcass swap snapped them to the floor. The death path stamped only RepAnim,
	// and authority never receives its own OnRep_Anim, so a listen host or standalone
	// game never played the death sequence on its own display. Both the literal
	// stand-in (no table) and a table's weighted variants must play on the body.
	for (const FName Table : {FName(NAME_None), FName(TEXT("Trolloc"))})
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0.0f, 0.0f, 200.0f), FRotator::ZeroRotator, Params);
		UWoTVertexAnimComponent* Anim =
			Pawn ? Cast<UWoTVertexAnimComponent>(Pawn->GetMesh()) : nullptr;
		if (!TestNotNull(TEXT("pawn"), Pawn) || !TestNotNull(TEXT("vertex-anim body"), Anim))
		{
			WoTTest::DestroySpecWorld(World);
			return false;
		}
		Pawn->AnimTableName = Table;
		TArray<FName> DeathNames = {FName(TEXT("Death"))};
		if (const TArray<FWoTAnimTableEntry>* Rows = WoTAnimTable::Find(Table))
		{
			for (const FWoTAnimTableEntry& Row : *Rows)
			{
				if (Row.Slot == FName(TEXT("Death")))
				{
					DeathNames.AddUnique(Row.AnimName);
				}
			}
		}
		for (const FName Name : DeathNames)
		{
			FWoTAnimSequence Sequence;
			Sequence.Name = Name;
			Sequence.NumFrames = 12;
			Sequence.Rate = 15.0f;
			Anim->Sequences.Add(Sequence);
		}

		// Keep this an ordinary death so the test observes the animation path;
		// killing-blow gibs are covered separately by the Parity E death specs.
		Pawn->Health = 1;
		Pawn->TakeRawDamage(1, TEXT("Sliced"), nullptr, FVector::ZeroVector);
		const FString Label = Table.IsNone() ? TEXT("no table") : Table.ToString();
		const FName Picked = Pawn->GetReplicatedAnimName();
		TestTrue(FString::Printf(TEXT("[%s] the pawn died"), *Label), Pawn->bDead);
		TestTrue(FString::Printf(TEXT("[%s] the replicated pick is a death sequence (%s)"),
			*Label, *Picked.ToString()), DeathNames.Contains(Picked));
		TestEqual(FString::Printf(TEXT("[%s] the authority's own body plays the pick"), *Label),
			Anim->GetCurrentSequence(), Picked);
		WoTTest::TickWorld(World, 0.4f);
		TestEqual(FString::Printf(TEXT("[%s] still playing it 0.4 s later"), *Label),
			Anim->GetCurrentSequence(), Picked);
		TestTrue(FString::Printf(TEXT("[%s] and advancing through it (frame %.2f)"), *Label,
			Anim->GetAnimFrame()), Anim->GetAnimFrame() > 0.0f);
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
