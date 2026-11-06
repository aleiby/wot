// Automation specs for the gore batch (presentation Slice 4a): the death-notify
// pawn->carcass swap, the carcass actor's fall/lie-flat/gib behavior, the player
// early-swap fallback, and the NPC branch of PlayerKilled (the recon latent bug).
// Needs imported content (creature meshes + DT_WoTAnimSequences with notifies).
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBloodFX.h"
#include "../WoTDecal.h"
#include "../WoTCollisionBlocker.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTAngrealProjectile.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "../WoTCarcass.h"
#include "../WoTCreatureChunk.h"
#include "../WoTParticleSprayer.h"
#include "../WoTCharacter.h"
#include "../WoTZoneVolume.h"
#include "../WoTFactionCharacters.h"
#include "../WoTVertexAnimComponent.h"
#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	// Minimal ticking Standalone world — copied per the test-file convention.
	// A big blocking slab under the arena so the falling carcass has ground to land
	// on (top surface at Z = 0).
	AActor* SpawnFloor(UWorld* World)
	{
		AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(
			FVector(0, 0, -50), FRotator::ZeroRotator);
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("Floor"));
		Box->SetupAttachment(Floor->GetRootComponent());
		Box->SetBoxExtent(FVector(4000, 4000, 50));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		// A rootless spawn drops the requested location — re-place so the slab's top
		// surface sits at Z = 0.
		return Floor;
	}

	// The decal's bound material family (its root UMaterial's name).
	FString DecalRootMaterial(const AWoTDecal* Decal)
	{
		const UMaterialInstanceDynamic* M = Decal ? Decal->GetDecalMaterial() : nullptr;
		const UMaterial* Root = M ? M->GetMaterial() : nullptr;
		return Root ? Root->GetName() : FString();
	}

	FString DecalSkinName(const AWoTDecal* Decal)
	{
		UTexture* Skin = nullptr;
		if (UMaterialInstanceDynamic* M = Decal ? Decal->GetDecalMaterial() : nullptr)
		{
			M->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), Skin);
		}
		return Skin ? Skin->GetName() : FString();
	}

	float DecalMLAmbient(const AWoTDecal* Decal)
	{
		FLinearColor Value(-1.0f, -1.0f, -1.0f, -1.0f);
		if (UMaterialInstanceDynamic* M = Decal ? Decal->GetDecalMaterial() : nullptr)
		{
			M->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("MLAmbient")), Value);
		}
		return Value.R;
	}

	int32 CountCarcasses(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTCarcass> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Count;
			}
		}
		return Count;
	}

	AWoTCarcass* FirstCarcass(UWorld* World)
	{
		for (TActorIterator<AWoTCarcass> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

// The pawn->carcass swap: a dead creature stamps its death variant, holds through the
// authored TransitionToCarcassNotification fraction, then swaps to a carcass that
// copies its display, falls to the floor, lies flat, and gibs past the damage
// threshold. NPCs never route through the player respawn path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGoreCarcassSpec,
	"WoT.Gore.Carcass", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGoreCarcassSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFloor(World);

	AWoTNPCCharacter* Trolloc = WoTCreatureData::SpawnCreature(
		*World, TEXT("Trolloc"), FVector(0, 0, 200), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!TestNotNull(TEXT("Trolloc spawned"), Trolloc))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Trolloc->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTNPCController* Ctl = Cast<AWoTNPCController>(Trolloc->GetController());
	const int32 PawnsBefore = [World] {
		int32 N = 0;
		for (TActorIterator<AWoTCharacter> It(World); It; ++It) { ++N; }
		return N;
	}();

	// ConfigureNPC bound the gore data.
	TestEqual(TEXT("row bound the gib threshold"), Trolloc->GibForSureFinalHealth, -40.0f);
	const FSoftObjectPath CorpseActorSkin(
		TEXT("/Game/WoT/Textures/Angreal/JDart.JDart"));
	const FSoftObjectPath CorpseMultiSkin(
		TEXT("/Game/WoT/Textures/WoTPawns/H_AesSedai0.H_AesSedai0"));
	TMap<int32, FSoftObjectPath> CorpseMultiSkins;
	if (UWoTVertexAnimComponent* Body =
		Cast<UWoTVertexAnimComponent>(Trolloc->GetMesh()))
	{
		Body->SetFatness(164);
		Body->SetActorSkin(CorpseActorSkin);
		Body->SetSkinOverride(1, CorpseMultiSkin);
		CorpseMultiSkins = Body->GetRuntimeSkinOverrides();
	}

	// Kill with a known overkill so the carcass's cumulative-damage seed is exact:
	// 150 hp - 160 = -10.
	WoTTest::TickWorld(World, 0.1f);
	Trolloc->TakeRawDamage(Trolloc->Health + 10, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	TestTrue(TEXT("the Trolloc died"), Trolloc->bDead);
	TestEqual(TEXT("no carcass before the death-anim notify"), CountCarcasses(World), 0);
	TestFalse(TEXT("the dying pawn is still visible (death anim playing)"),
	          Trolloc->IsHidden());

	// The NPC branch: the AI controller was NOT freed for a respawn (the latent-bug
	// fix — a routed NPC would get UnPossess + a default pawn at a PlayerStart).
	TestTrue(TEXT("the dead NPC keeps its AI controller (no player-respawn routing)"),
	         Trolloc->GetController() == Ctl);

	// Swap fires at the authored fraction of the picked variant: worst case DEATHF =
	// 0.90 x (28-1)/(15 x 0.35) = ~4.6s. Tick past it.
	WoTTest::TickWorld(World, 5.2f);
	AWoTCarcass* Carc = FirstCarcass(World);
	if (!TestNotNull(TEXT("the carcass spawned at the death-anim notify"), Carc))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("the dead pawn hid at the swap"), Trolloc->IsHidden());
	TestEqual(TEXT("the carcass copied the pawn's mesh"), Carc->MeshName, FName(TEXT("Trolloc")));
	TestEqual(TEXT("the carcass copied Util.CopyDisplaySettings Fatness"),
		Carc->MeshFatness, uint8(164));
	TestEqual(TEXT("the carcass applied copied Fatness to its mesh"),
		Carc->Mesh->Fatness, uint8(164));
	TestEqual(TEXT("the carcass copied actor Skin"),
		Carc->Mesh->GetRuntimeActorSkin(), CorpseActorSkin);
	TestTrue(TEXT("the carcass copied exact-index MultiSkins"),
		Carc->Mesh->GetRuntimeSkinOverrides().OrderIndependentCompareEqual(
			CorpseMultiSkins));
	AWoTCarcass* ProxyCarcass = World->SpawnActor<AWoTCarcass>();
	if (TestNotNull(TEXT("client carcass presentation fixture"), ProxyCarcass))
	{
		ProxyCarcass->MeshName = Carc->MeshName;
		ProxyCarcass->MeshScale = Carc->MeshScale;
		ProxyCarcass->MeshFatness = Carc->MeshFatness;
		ProxyCarcass->MeshActorSkin = Carc->MeshActorSkin;
		ProxyCarcass->MeshSkinIndices = Carc->MeshSkinIndices;
		ProxyCarcass->MeshSkinPaths = Carc->MeshSkinPaths;
		ProxyCarcass->OnRep_Display();
		TestEqual(TEXT("client carcass applies replicated actor Skin"),
			ProxyCarcass->Mesh->GetRuntimeActorSkin(), CorpseActorSkin);
		TestTrue(TEXT("client carcass applies replicated exact-index MultiSkins"),
			ProxyCarcass->Mesh->GetRuntimeSkinOverrides()
				.OrderIndependentCompareEqual(CorpseMultiSkins));
		ProxyCarcass->Destroy();
	}
	TestEqual(TEXT("the carcass froze the death variant pose"),
	          Carc->PoseSequence, Trolloc->GetReplicatedAnimName());
	TestTrue(TEXT("the pose fraction is the notify point (not 0)"), Carc->PoseFraction > 0.3f);
	TestEqual(TEXT("no rogue pawn spawned (PlayerKilled NPC branch)"), [World] {
		int32 N = 0;
		for (TActorIterator<AWoTCharacter> It(World); It; ++It) { ++N; }
		return N;
	}(), PawnsBefore);

	// It falls (spawned at Z~200 over the floor at 0), lands, and lies flat with the
	// squashed cylinder (0.4 x source RADIUS half-height — WOTCarcass.uc:554).
	WoTTest::TickWorld(World, 2.0f);
	TestTrue(TEXT("the carcass came to rest on the floor"), Carc->IsLyingStill());
	TestTrue(TEXT("the carcass settled near the floor"), Carc->GetActorLocation().Z < 60.0f);
	if (const UCapsuleComponent* Cap = Carc->Capsule)
	{
		// UE capsules clamp half-height >= radius (the flat-disc divergence noted in
		// AdjustCylinder): squashed means "below the source pawn's half-height".
		TestTrue(TEXT("the cylinder squashed on landing"),
		         Cap->GetUnscaledCapsuleHalfHeight() < 44.0f);
	}

	// Shootable END-TO-END (the review-caught fix): weaves must be able to REACH the
	// corpse — the capsule overlaps the WoTProjectile channel (bProjTarget=true,
	// WOTCarcass.uc:150) and the projectile touch gate accepts carcasses. The direct
	// TakeDamage below only proves the accumulation math.
	if (const UCapsuleComponent* Cap2 = Carc->Capsule)
	{
		TestEqual(TEXT("the corpse overlaps the weave channel (shootable, non-blocking)"),
		          Cap2->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		          ECR_Overlap);
	}
	TestTrue(TEXT("the projectile touch gate accepts a carcass"),
	         AWoTAngrealProjectile::IsTouchTarget(Carc));

	// Damage accumulates toward the scaled gib threshold — the death seeded
	// 10, a 50 hit crosses 40/50 x 40 = 32 -> ChunkUp spawns the recipe (4c).
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Sliced");
	Carc->TakeDamage(50.0f, Ev, nullptr, nullptr);
	TestTrue(TEXT("threshold damage gibs the carcass"), Carc->WasChunkedUp());
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("the gibbed carcass is gone"), CountCarcasses(World), 0);

	// The chunks (4c): a Trolloc uses the BASE recipe — the ChunkA replacement always
	// spawns, extras roll their 0.9/1.0 odds (6 parts max). All fly, land, and enter
	// cleanup on the decoded 8s timer with retail's +/-50% perturbation (4-12s).
	int32 Chunks = 0;
	AWoTCreatureChunk* AnyChunk = nullptr;
	AWoTCreatureChunk* BloodChunk = nullptr;
	for (TActorIterator<AWoTCreatureChunk> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			++Chunks;
			if (!AnyChunk) { AnyChunk = *It; }
			if (!BloodChunk && It->GetTrailForTest()) { BloodChunk = *It; }
		}
	}
	TestTrue(FString::Printf(TEXT("the gib spawned recipe chunks (got %d)"), Chunks),
	         Chunks >= 1 && Chunks <= 6);
	if (TestNotNull(TEXT("blooded production gib owns its following trail"), BloodChunk))
	{
		AWoTParticleSprayer* Trail = BloodChunk->GetTrailForTest();
		const float InitialVolume = Trail ? Trail->Volume : 0.0f;
		WoTTest::TickWorld(World, 0.1f);
		TestTrue(TEXT("decoded 2-5 second bleed drains volume every tick"),
			Trail && Trail->Volume > 0.0f && Trail->Volume < InitialVolume);
	}
	if (AnyChunk)
	{
		TestTrue(TEXT("chunks carry recipe meshes"), !AnyChunk->MeshName.IsNone());
		TestTrue(TEXT("chunks resolve real imported assets"),
		         AWoTCarcass::ResolveMeshByName(AnyChunk->MeshName) != nullptr);
		TestTrue(TEXT("production gib recipe carries its authored skin"),
			!AnyChunk->SkinName.IsNone());
		TestTrue(TEXT("chunk applies the authored skin to its runtime mesh"),
			!AnyChunk->Mesh->GetRuntimeSkinOverrides().IsEmpty());
		TestEqual(TEXT("chunk applies only the decoded authored skin slot"),
			AnyChunk->Mesh->GetRuntimeSkinOverrides().Num(), 1);
		TestTrue(TEXT("chunk runtime skin map contains the decoded SkinSlot"),
			AnyChunk->Mesh->GetRuntimeSkinOverrides().Contains(AnyChunk->SkinSlot));
		for (int32 Step = 0; Step < 260 && IsValid(AnyChunk)
			&& !AnyChunk->IsSinking(); ++Step)
		{
			WoTTest::TickWorld(World, 0.05f);
		}
		TestTrue(TEXT("multiplayer cleanup enters the retail Q3A sink"),
			IsValid(AnyChunk) && AnyChunk->IsSinking());
		if (IsValid(AnyChunk))
		{
			const float BeforeSinkZ = AnyChunk->GetActorLocation().Z;
			WoTTest::TickWorld(World, 0.2f);
			TestTrue(TEXT("sinking gib moves below the floor"), IsValid(AnyChunk)
				&& AnyChunk->GetSinkDistance() > 0.0f
				&& AnyChunk->GetActorLocation().Z < BeforeSinkZ);
		}
	}
	if (BloodChunk)
	{
		AWoTParticleSprayer* Trail = BloodChunk->GetTrailForTest();
		BloodChunk->Destroy();
		TestTrue(TEXT("destroy drains rather than destroys the blood trail"),
			IsValid(Trail) && !Trail->bOn && Trail->GetLifeSpan() > 0.0f);
	}

	// The pain-zone decay uses the authenticated production recipe and InitFor path.
	const TArray<FWoTBodyPart>* Recipe = WoTGoreData::FindRecipe(NAME_None);
	FActorSpawnParameters PainSpawn;
	PainSpawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FTransform PainTransform(FRotator::ZeroRotator,
		FVector(5000, 0, 100));
	AWoTZoneVolume* Pain = World->SpawnActorDeferred<AWoTZoneVolume>(
		AWoTZoneVolume::StaticClass(), PainTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Pain)
	{
		Pain->Collision->SetBoxExtent(FVector(1000));
		Pain->bPainZone = true;
		// Arm after the raw carcass has served as InitFor's display/recipe source.
		// WOTCarcassBase.ZoneChange now correctly performs an immediate 1-second
		// CheckZone, so arming before spawn would gib this fixture helper.
		Pain->DamagePerSec = 0.0f;
		Pain->FinishSpawning(PainTransform);
	}
	AWoTCarcass* PainSource = World->SpawnActor<AWoTCarcass>(
		FVector(5000, 0, 100), FRotator::ZeroRotator, PainSpawn);
	AWoTCreatureChunk* PainChunk = World->SpawnActor<AWoTCreatureChunk>(
		FVector(5000, 0, 100), FRotator::ZeroRotator, PainSpawn);
	if (TestTrue(TEXT("production base gib recipe is installed"), Recipe && !Recipe->IsEmpty())
		&& TestNotNull(TEXT("pain-zone volume"), Pain)
		&& TestNotNull(TEXT("pain-zone source carcass"), PainSource)
		&& TestNotNull(TEXT("pain-zone production chunk"), PainChunk))
	{
		PainChunk->InitFor(*PainSource, (*Recipe)[0]);
		PainChunk->SetVelocityForTest(FVector::ZeroVector);
		Pain->DamagePerSec = 1000.0f;
		WoTTest::TickWorld(World, 0.5f);
		TestFalse(TEXT("PainZoneDamageScale 0.1 decays a gib"), IsValid(PainChunk));
	}
	WoTTest::TickWorld(World, 50.0f);
	int32 ChunksLeft = 0;
	for (TActorIterator<AWoTCreatureChunk> It(World); It; ++It)
	{
		if (IsValid(*It)) { ++ChunksLeft; }
	}
	TestEqual(TEXT("chunks expired on the delete timer"), ChunksLeft, 0);

	// The hidden NPC actor + its controller clean up after DestroyDelaySecs (10s).
	WoTTest::TickWorld(World, 10.5f);
	TestFalse(TEXT("the hidden dead NPC actor is destroyed after the delay"),
	          IsValid(Trolloc));
	TestFalse(TEXT("the dead NPC's AI controller went with it"), IsValid(Ctl));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGoreChunkCleanupGateSpec,
	"WoT.Gore.ChunkCleanupGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGoreChunkCleanupGateSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AGameModeBase::StaticClass();
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	const TArray<FWoTBodyPart>* Recipe = WoTGoreData::FindRecipe(NAME_None);
	const FWoTCreatureChunkDefaults* Defaults = WoTGoreData::GetDefaults();
	if (!TestTrue(TEXT("production chunk family is installed"),
		Recipe && !Recipe->IsEmpty() && Defaults))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTBodyPart Part = (*Recipe)[0];
	Part.bNoBlood = true;
	FActorSpawnParameters AlwaysSpawn;
	AlwaysSpawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCarcass* RecheckSource = World->SpawnActor<AWoTCarcass>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn);
	AWoTCreatureChunk* RecheckChunk = World->SpawnActor<AWoTCreatureChunk>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawn);
	if (!TestNotNull(TEXT("standalone recheck source"), RecheckSource)
		|| !TestNotNull(TEXT("standalone recheck chunk"), RecheckChunk))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	UWoTZoneIndex* ZoneIndex = World->GetSubsystem<UWoTZoneIndex>();
	if (!TestNotNull(TEXT("world zone index"), ZoneIndex))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const int32 BaselineWorldCarcasses = ZoneIndex->WorldCarcassCount;
	ZoneIndex->WorldMaxCarcasses = BaselineWorldCarcasses + 1;
	RecheckChunk->InitFor(*RecheckSource, Part);
	TestEqual(TEXT("a no-volume chunk is accounted in the world carcass bucket"),
		ZoneIndex->WorldCarcassCount, BaselineWorldCarcasses + 1);
	RecheckChunk->CheckCleanupGateForTest();
	TestFalse(TEXT("no-volume chunk at the world cap does not sink"),
		RecheckChunk->IsSinking());
	TestTrue(TEXT("standalone chunk schedules the decoded cleanup recheck"),
		RecheckChunk->HasCleanupTimerForTest());
	TestEqual(TEXT("cleanup recheck uses the decoded family delay"),
		RecheckChunk->GetCleanupTimerRateForTest(),
		Defaults->DeleteRecheckDelay, 0.001f);

	AWoTCreatureChunk* WorldCapChunk = World->SpawnActor<AWoTCreatureChunk>(
		FVector(2500, 0, 100), FRotator::ZeroRotator, AlwaysSpawn);
	if (!TestNotNull(TEXT("world-cap chunk"), WorldCapChunk))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WorldCapChunk->InitFor(*RecheckSource, Part);
	TestEqual(TEXT("a second no-volume chunk exceeds the world cap"),
		ZoneIndex->WorldCarcassCount, BaselineWorldCarcasses + 2);
	WorldCapChunk->CheckCleanupGateForTest();
	TestTrue(TEXT("over-cap no-volume chunk starts the decoded sink path"),
		WorldCapChunk->IsSinking());
	TestEqual(TEXT("sinking releases the world carcass count"),
		ZoneIndex->WorldCarcassCount, BaselineWorldCarcasses + 1);

	const FVector CapLocation(5000, 0, 100);
	AWoTZoneVolume* DeathZone = World->SpawnActor<AWoTZoneVolume>(
		CapLocation, FRotator::ZeroRotator);
	DeathZone->Collision->SetBoxExtent(FVector(1000));
	DeathZone->MaxCarcasses = 0;
	RecheckSource->SetActorLocation(CapLocation, false, nullptr,
		ETeleportType::TeleportPhysics);
	AWoTCreatureChunk* CapChunk = World->SpawnActor<AWoTCreatureChunk>(
		CapLocation, FRotator::ZeroRotator, AlwaysSpawn);
	if (!TestNotNull(TEXT("DeathZone-cap chunk"), CapChunk))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	CapChunk->InitFor(*RecheckSource, Part);
	TestEqual(TEXT("InitFor accounts the chunk in its DeathZone"),
		DeathZone->NumCarcasses, 1);
	CapChunk->CheckCleanupGateForTest();
	TestTrue(TEXT("DeathZone cap starts the decoded sink path"),
		CapChunk->IsSinking());
	TestEqual(TEXT("sinking releases the DeathZone carcass count"),
		DeathZone->NumCarcasses, 0);

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// The early-swap fallback: a dead pawn torn down before its carcass notify (an early
// eligible player respawn can beat the authored fraction) still leaves a carcass behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGoreEarlySwapSpec,
	"WoT.Gore.EarlySwap", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGoreEarlySwapSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFloor(World);

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTCharacter* Pawn = World->SpawnActor<AWoTAesSedai>(
		FVector(0, 0, 200), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("faction pawn spawned"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	WoTTest::TickWorld(World, 0.1f);
	Pawn->TakeRawDamage(Pawn->Health + 5, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	TestTrue(TEXT("the pawn died"), Pawn->bDead);
	TestEqual(TEXT("no carcass yet (notify pending)"), CountCarcasses(World), 0);

	// Destroy the pawn actor NOW (what an early eligible respawn does) — the carcass
	// must still appear, posed however far the death anim got.
	Pawn->Destroy();
	WoTTest::TickWorld(World, 0.1f);
	AWoTCarcass* Carc = FirstCarcass(World);
	if (TestNotNull(TEXT("early pawn teardown still produced a carcass"), Carc))
	{
		TestEqual(TEXT("the carcass copied the faction mesh"),
		          Carc->MeshName, FName(TEXT("AesSedai")));
		TestTrue(TEXT("its pose fraction reflects the cut-short anim (< 0.5)"),
		         Carc->PoseFraction < 0.5f);
	}

	// A table-less bare pawn (no mesh, no death sequence) never swaps: nothing to copy.
	AWoTCharacter* Bare = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 200), FRotator::ZeroRotator, Params);
	WoTTest::TickWorld(World, 0.1f);
	Bare->TakeRawDamage(Bare->Health + 5, TEXT("Sliced"), nullptr, FVector::ZeroVector);
	WoTTest::TickWorld(World, 2.0f);
	Bare->Destroy();
	WoTTest::TickWorld(World, 0.1f);
	TestEqual(TEXT("a meshless pawn leaves no carcass"), CountCarcasses(World), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The blood layer (gore 4d): the 7 shipped configs resolve, the one-shot sprayer
// lifecycle runs (burst -> off -> self-destroy), and the damage hook spawns the
// replicated marker with the damage-scaled volume + the black-blood split.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGoreBloodSpec,
	"WoT.Gore.Blood", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGoreBloodSpec::RunTest(const FString&)
{
	// --- Registry: every shipped config, verbatim shape. ---
	for (const TCHAR* Name : {TEXT("BloodDamageRed1"), TEXT("BloodDamageBlack1"),
	                          TEXT("BloodBurstRed2"), TEXT("BloodBurstBlack2"),
	                          TEXT("BloodSprayRed"), TEXT("BloodSprayBlack"),
	                          TEXT("BloodSpray01")})
	{
		TestNotNull(FString::Printf(TEXT("config %s present"), Name),
		            WoTBloodFX::Find(FName(Name)));
	}
	const FWoTBloodConfig* Damage1 = WoTBloodFX::Find(TEXT("BloodDamageRed1"));
	if (TestNotNull(TEXT("BloodDamageRed1"), Damage1))
	{
		TestEqual(TEXT("4 templates (BloodDamageRed1.uc)"), Damage1->Templates.Num(), 4);
		TestEqual(TEXT("burst = TriggerTimed TimerDuration 0.1"), Damage1->BurstSeconds, 0.1f);
		TestEqual(TEXT("destroy = PreBeginPlay LifeSpan 2.1"), Damage1->SelfDestroySeconds, 2.1f);
		TestEqual(TEXT("Volume 800"), Damage1->Volume, 800.0f);
	}
	const FWoTBloodConfig* Burst2B = WoTBloodFX::Find(TEXT("BloodBurstBlack2"));
	if (TestNotNull(TEXT("BloodBurstBlack2"), Burst2B))
	{
		TestEqual(TEXT("7 templates (the gib explosion)"), Burst2B->Templates.Num(), 7);
		TestEqual(TEXT("black texture set"), Burst2B->TextureNames[0],
		          FName(TEXT("BloodBlack11")));
	}
	TestNull(TEXT("unknown config misses quietly"), WoTBloodFX::Find(TEXT("NoSuchBlood")));

	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFloor(World);

	// --- One-shot lifecycle: burst emits, drops bOn, self-destroys. ---
	AWoTParticleSprayer* S = WoTBloodFX::SpawnLocal(
	    *World, TEXT("BloodDamageRed1"), FVector(0, 0, 100), FRotator::ZeroRotator, 2.0f);
	if (TestNotNull(TEXT("SpawnLocal produced a sprayer"), S))
	{
		TestEqual(TEXT("volume scaled by the damage factor"), S->Volume, 1600.0f);
		TestTrue(TEXT("blood draws modulate-only (STY_Modulated)"), S->bModulateOnly);
		TestTrue(TEXT("materials resolved (import_gore_fx ran)"),
		         S->ModMaterials.Num() == 4 && S->ModMaterials[0] != nullptr);
		TestTrue(TEXT("burst starts on"), S->bOn);
		WoTTest::TickWorld(World, 0.3f);
		TestFalse(TEXT("burst is off after TimerDuration 0.1"), S->bOn);
		WoTTest::TickWorld(World, 2.2f);
		TestFalse(TEXT("the burst actor self-destroyed at LifeSpan 2.1"), IsValid(S));
	}

	// --- The damage hook: a hit spawns the replicated marker, red vs black. ---
	auto CountMarkers = [World](FName Config) {
		int32 N = 0;
		for (TActorIterator<AWoTBloodBurst> It(World); It; ++It)
		{
			if (IsValid(*It) && It->Config == Config)
			{
				++N;
			}
		}
		return N;
	};
	AWoTNPCCharacter* Trolloc = WoTCreatureData::SpawnCreature(
		*World, TEXT("Trolloc"), FVector(500, 0, 200), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	AWoTNPCCharacter* Myrddraal = WoTCreatureData::SpawnCreature(
		*World, TEXT("Myrddraal"), FVector(1000, 0, 200), 1, /*bCombatReady*/ true,
		AWoTTestNPCCharacter::StaticClass());
	if (!Trolloc || !Myrddraal)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("Trollocs bleed red (plan premise 6)"), Trolloc->bBlackBlood);
	TestTrue(TEXT("Myrddraal bleeds black (DamageHelperBlackBlood)"), Myrddraal->bBlackBlood);

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("Sliced");
	Trolloc->TakeDamage(50.0f, Ev, nullptr, nullptr);
	TestEqual(TEXT("a damaging hit spawns the red damage marker"),
	          CountMarkers(TEXT("BloodDamageRed1")), 1);
	Myrddraal->TakeDamage(50.0f, Ev, nullptr, nullptr);
	TestEqual(TEXT("a black-blooded hit spawns the black marker"),
	          CountMarkers(TEXT("BloodDamageBlack1")), 1);

	// The marker's BeginPlay fired a local sprayer scaled by damage/25.
	bool bSawScaledSprayer = false;
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (IsValid(*It) && FMath::IsNearlyEqual(It->Volume, 800.0f * 2.0f))
		{
			bSawScaledSprayer = true;   // 50 damage / BaseDamageLevel 25 = x2
		}
	}
	TestTrue(TEXT("the marker fired a local sprayer at damage/25 volume"), bSawScaledSprayer);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The decal layer (gore 4e): the class registry, the Align fit (place, edge-shrink,
// mid-air fail), and the sprayer-thrown flying path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGoreDecalSpec,
	"WoT.Gore.Decal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGoreDecalSpec::RunTest(const FString&)
{
	// --- Registry: the shipped decal classes, verbatim texture lists. ---
	for (const TCHAR* Name : {TEXT("BloodDecalRed"), TEXT("BloodDecalBlack"),
	                          TEXT("CrackDecal"), TEXT("ScorchMark"), TEXT("BurnDecal"),
	                          TEXT("DecaySplatter"), TEXT("MashadarCrack"),
	                          TEXT("BalefireDecal"), TEXT("BFHole")})
	{
		TestNotNull(FString::Printf(TEXT("decal config %s present"), Name),
		            WoTDecalFX::Find(FName(Name)));
	}
	// Only BloodDecal.uc:55 authors DrawScale 0.7; Crack/Scorch/Burn inherit it.
	for (const TCHAR* Name : {TEXT("CrackDecal"), TEXT("ScorchMark"), TEXT("BurnDecal")})
	{
		if (const FWoTDecalConfig* C = WoTDecalFX::Find(FName(Name)))
		{
			TestEqual(FString::Printf(TEXT("%s inherits BloodDecal DrawScale 0.7"), Name),
			          C->DrawScale, 0.7f);
			TestTrue(FString::Printf(TEXT("%s is STY_Modulated"), Name),
			         C->Style == EWoTDecalStyle::Modulated);
		}
	}
	const FWoTDecalConfig* Red = WoTDecalFX::Find(TEXT("BloodDecalRed"));
	if (TestNotNull(TEXT("BloodDecalRed"), Red))
	{
		TestEqual(TEXT("15-texture weighted list (BloodDecal.uc)"), Red->Textures.Num(), 15);
		TestEqual(TEXT("DrawScale 0.7"), Red->DrawScale, 0.7f);
		TestEqual(TEXT("BloodDecal retail DetailLevel"), Red->DetailLevel, uint8(2));
		TestEqual(TEXT("BloodDecal retail GoreLevel"), Red->GoreLevel, uint8(2));
		TestEqual(TEXT("BloodDecal retail visibility radius"),
			Red->VisibilityRadius, 2000.0f);
		TestEqual(TEXT("BloodDecal retail visibility height"),
			Red->VisibilityHeight, 2000.0f);
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFloor(World);

	// --- Align: an open-floor impact places at full size with a projected decal. ---
	AWoTDecal* D = WoTDecalFX::SpawnAligned(*World, TEXT("BloodDecalRed"),
	                                        FVector(0, 0, 5), FVector::UpVector);
	if (TestNotNull(TEXT("open-floor decal placed"), D))
	{
		TestTrue(TEXT("placed"), D->IsPlaced());
		TestEqual(TEXT("full size on open ground"), D->FinalScale, 0.7f);
		// Decal.uc: DT_Mesh Legend.DecalTRI, unlit, STY_Modulated -> the
		// DecalTRI quad on the 2x-modulate mesh master (it must show on the
		// MSM_Unlit world, which a deferred/DBuffer decal cannot touch).
		UWoTVertexAnimComponent* Quad = D->GetDecalMesh();
		if (TestNotNull(TEXT("the DecalTRI mesh component"), Quad))
		{
			TestTrue(TEXT("DecalTRI bound"), Quad->HasDisplayMesh());
			TestEqual(TEXT("DecalTRI mesh"), Quad->GetDisplayMeshName(),
			          FName(TEXT("DecalTRI")));
			TestTrue(TEXT("visible once placed"), Quad->IsVisible());
			TestEqual(TEXT("mesh scale = fitted DrawScale"),
			          static_cast<float>(Quad->GetRelativeScale3D().X), 0.7f);
			TestTrue(TEXT("decal material bound on slot 0"),
			         Quad->GetMaterial(0) == D->GetDecalMaterial());
			TestEqual(TEXT("UE1 pass-1 draw order (before translucent world)"),
			          Quad->TranslucencySortPriority, -1);
			TestTrue(TEXT("no collision (later Align traces pass through)"),
			         Quad->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
		}
		TestEqual(TEXT("STY_Modulated -> M_WoT_MeshModulate"),
		          DecalRootMaterial(D), FString(TEXT("M_WoT_MeshModulate")));
		TestTrue(TEXT("a BloodDecal skin"), DecalSkinName(D).StartsWith(TEXT("Blood")));
	}

	// --- Align: a mid-air spawn (no surface within the 16uu back-trace) dies. ---
	TestNull(TEXT("a mid-air decal destroys itself"),
	         WoTDecalFX::SpawnAligned(*World, TEXT("BloodDecalRed"),
	                                  FVector(0, 0, 500), FVector::UpVector));

	// --- Align: near the slab edge the corner traces fail until the x0.85 shrink
	// fits (Decal.uc's RetryScale loop). 15uu in from the 4000 edge needs the
	// 21uu half-width shrunk 2-3 times. ---
	AWoTDecal* Edge = WoTDecalFX::SpawnAligned(*World, TEXT("BloodDecalRed"),
	                                           FVector(3985, 0, 5), FVector::UpVector);
	if (TestNotNull(TEXT("edge decal fits after shrinking"), Edge))
	{
		TestTrue(FString::Printf(TEXT("shrunk to fit (scale %.2f)"), Edge->FinalScale),
		         Edge->FinalScale < 0.7f && Edge->FinalScale > 0.2f);
	}

	AWoTCollisionBlocker* BlockAll = World->SpawnActor<AWoTCollisionBlocker>(
		FVector(2000, 0, 50), FRotator::ZeroRotator);
	BlockAll->Configure(EWoTCollisionBlockerMode::All, 500.0f, 50.0f);
	TestNull(TEXT("Align rejects retail BlockAll surfaces"),
		WoTDecalFX::SpawnAligned(*World, TEXT("BloodDecalRed"),
			FVector(2000, 0, 105), FVector::UpVector));

	AActor* StaticImpact = SpawnFloor(World);
	TestFalse(TEXT("a WorldStatic actor is not retail Level geometry"),
		AWoTFireballProjectile::ShouldSpawnScorchForImpact(
			StaticImpact, StaticImpact->FindComponentByClass<UBoxComponent>()));
	TestFalse(TEXT("retail BlockAll is an actor, not Level geometry"),
		AWoTFireballProjectile::ShouldSpawnScorchForImpact(
			BlockAll, BlockAll->Capsule));
	AWoTCharacter* PawnImpact = World->SpawnActor<AWoTCharacter>(
		FVector(3000, 0, 100), FRotator::ZeroRotator);
	TestFalse(TEXT("fireball does not scorch pawn impacts"),
		AWoTFireballProjectile::ShouldSpawnScorchForImpact(
			PawnImpact, PawnImpact->GetCapsuleComponent()));
	AActor* DynamicImpact = World->SpawnActor<AActor>();
	UBoxComponent* DynamicBox = NewObject<UBoxComponent>(DynamicImpact);
	DynamicImpact->SetRootComponent(DynamicBox);
	DynamicBox->SetCollisionObjectType(ECC_WorldDynamic);
	DynamicBox->RegisterComponent();
	TestFalse(TEXT("fireball does not scorch non-Level dynamic actors"),
		AWoTFireballProjectile::ShouldSpawnScorchForImpact(DynamicImpact, DynamicBox));

	// --- The sprayer-thrown path: a flying decal falls and aligns where it lands. ---
	AWoTDecal* Flying = WoTDecalFX::SpawnFlying(*World, TEXT("ScorchMark"),
	                                            FVector(500, 500, 120),
	                                            FVector(0, 0, -100.0f));
	if (TestNotNull(TEXT("flying decal spawned"), Flying))
	{
		TestFalse(TEXT("not placed while airborne"), Flying->IsPlaced());
		WoTTest::TickWorld(World, 2.0f);
		TestTrue(TEXT("landed and placed"), IsValid(Flying) && Flying->IsPlaced());
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Decals as UE1 meshes: the STY_Translucent BalefireDecal (GUnlitColor from
// ScaleGlow/AmbientGlow in the original renderer), its 10-frame burn-in + fade
// (BalefireDecal.uc Tick), the BFHole it drops, the DecalManager exemption, and
// the caller-set DrawScale (AngrealFireballProjectile.uc:152).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGoreDecalStyleSpec,
	"WoT.Gore.DecalStyles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGoreDecalStyleSpec::RunTest(const FString&)
{
	TestEqual(TEXT("GUnlitColor clamps (0.5 + 180/256)"),
	          WoTDecalFX::UnlitColor(1.0f, 180), 1.0f);
	TestEqual(TEXT("GUnlitColor at AmbientGlow 0"),
	          WoTDecalFX::UnlitColor(0.5f, 0), 0.25f);
	const FWoTDecalConfig* Bale = WoTDecalFX::Find(TEXT("BalefireDecal"));
	if (TestNotNull(TEXT("BalefireDecal row"), Bale))
	{
		TestTrue(TEXT("STY_Translucent"), Bale->Style == EWoTDecalStyle::Translucent);
		TestEqual(TEXT("AmbientGlow 180"), static_cast<int32>(Bale->AmbientGlow), 180);
		TestEqual(TEXT("AnimTextures BFhole01-10"), Bale->Textures.Num(), 10);
		TestFalse(TEXT("never registers with the DecalManager"), Bale->bRegisters);
		TestEqual(TEXT("drops a BFHole"), Bale->CompanionDecal, FName(TEXT("BFHole")));
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnFloor(World);

	AWoTDecal* Burn = WoTDecalFX::SpawnAligned(*World, TEXT("BalefireDecal"),
	                                           FVector(0, 0, 5), FVector::UpVector);
	if (TestNotNull(TEXT("balefire decal placed"), Burn))
	{
		TestEqual(TEXT("STY_Translucent -> M_WoT_MeshTranslucent"),
		          DecalRootMaterial(Burn), FString(TEXT("M_WoT_MeshTranslucent")));
		TestEqual(TEXT("burn-in starts at BFhole01"), DecalSkinName(Burn),
		          FString(TEXT("BFhole01")));
		TestEqual(TEXT("unlit source = GUnlitColor 1.0"), DecalMLAmbient(Burn), 1.0f);
		TestEqual(TEXT("UE1 pass-2 draw order"),
		          Burn->GetDecalMesh()->TranslucencySortPriority, 90);
	}
	auto CountHoles = [World]() {
		int32 Count = 0;
		for (TActorIterator<AWoTDecal> It(World); It; ++It)
		{
			Count += It->DecalType == FName(TEXT("BFHole")) ? 1 : 0;
		}
		return Count;
	};
	// BalefireDecal.uc spawns its BFHole from its first Tick, not in Align.
	TestEqual(TEXT("no BFHole before the first Tick"), CountHoles(), 0);
	WoTTest::TickWorld(World, 1);
	int32 Holes = 0;
	for (TActorIterator<AWoTDecal> It(World); It; ++It)
	{
		if (It->DecalType == FName(TEXT("BFHole")) && It->IsPlaced())
		{
			++Holes;
			TestEqual(TEXT("BFHole is STY_Modulated"), DecalRootMaterial(*It),
			          FString(TEXT("M_WoT_MeshModulate")));
			TestEqual(TEXT("BFHole skin"), DecalSkinName(*It),
			          FString(TEXT("BalefireHole")));
		}
	}
	TestEqual(TEXT("one BFHole under the burn"), Holes, 1);

	if (Burn)
	{
		// 10 frames x FrameTime 0.1 (one step per tick at most), then the fade.
		WoTTest::TickWorld(World, 1.5f);
		if (TestTrue(TEXT("burn survives into its fade"), IsValid(Burn)))
		{
			TestEqual(TEXT("holds the last frame"), DecalSkinName(Burn),
			          FString(TEXT("BFhole10")));
			TestEqual(TEXT("AmbientGlow dropped to 0"),
			          static_cast<int32>(Burn->AmbientGlow), 0);
			const float Unlit = DecalMLAmbient(Burn);
			TestTrue(FString::Printf(TEXT("fading from 0.5 (got %.3f)"), Unlit),
			         Unlit < 0.5f && Unlit > 0.3f);
			TestTrue(TEXT("the authority expires it after FadeTime"),
			         Burn->GetLifeSpan() > 0.0f && Burn->GetLifeSpan() <= 5.0f);
		}
	}

	// The caller-set DrawScale (the fireball's BurnMark.DrawScale = 2.0).
	AWoTDecal* Scorch = WoTDecalFX::SpawnAligned(*World, TEXT("ScorchMark"),
	                                             FVector(1000, 1000, 5),
	                                             FVector::UpVector, false, 2.0f);
	if (TestNotNull(TEXT("scorch placed"), Scorch))
	{
		TestEqual(TEXT("override DrawScale 2.0 fits open ground"),
		          Scorch->FinalScale, 2.0f);
		TestEqual(TEXT("mesh at the fitted scale"),
		          static_cast<float>(Scorch->GetDecalMesh()->GetRelativeScale3D().X), 2.0f);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif   // WITH_AUTOMATION_TESTS
