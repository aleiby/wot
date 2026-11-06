// Automation specs for the networked test harness.
// Covers the test-only Source/WoT/Testing layer: world-state snapshot
// serializer, scripted player actions, and the TCP command channel.
// These are single-process specs; the multiprocess smoke loop is driven
// by a separate Python orchestrator.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"
#include "../WoTSessionSubsystem.h"

#if WITH_AUTOMATION_TESTS

#include "../Testing/WoTNetTestSnapshot.h"
#include "../Testing/WoTNetTestActions.h"
#include "../Testing/WoTNetTestChannel.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTAppearEffect.h"
#include "../WoTCharacterMovement.h"
#include "../WoTParticleSprayer.h"
#include "../WoTMeshFX.h"
#include "../WoTPlayerController.h"
#include "../WoTFactionCharacters.h"
#include "../WoTZoneVolume.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../Projectiles/WoTAngrealAMAProjectile.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTDistantEyeProjectile.h"
#include "../Projectiles/WoTAngrealIllusionProjectile.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTFireworksProjectile.h"
#include "../Projectiles/WoTIceProjectile.h"
#include "../Projectiles/WoTSeekerProjectile.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTDecreaseHealthLeech.h"
#include "../Inventory/WoTIceLeech.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Inventory/WoTReflector.h"
#include "../Inventory/WoTShieldReflector.h"
#include "../Inventory/WoTReturnToSenderReflector.h"
#include "../Inventory/WoTInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTResourceSpawnerInv.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvAMA.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTAngrealInvChampion.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "../Inventory/WoTAngrealInvExpWard.h"
#include "../Inventory/WoTAngrealInvFireworks.h"
#include "../Inventory/WoTAngrealInvGuardian.h"
#include "../Inventory/WoTAngrealInvHeal.h"
#include "../Inventory/WoTAngrealInvLevitate.h"
#include "../Inventory/WoTAngrealInvLightGlobe.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "../Inventory/WoTAngrealInvMinion.h"
#include "../Inventory/WoTAngrealInvIce.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTAngrealInvShields.h"
#include "../Inventory/WoTIgnoreElementReflector.h"
#include "../Inventory/WoTAngrealInvShift.h"
#include "../Inventory/WoTAngrealInvWhirlwind.h"
#include "../WoTBattleInfo.h"
#include "../WoTBattleGameMode.h"
#include "../WoTPlayerState.h"
#include "WoTTestBattleClasses.h"
#include "../WoTBudgetInfo.h"
#include "../WoTTeleporter.h"
#include "../WoTEvents.h"
#include "../WoTMover.h"
#include "../WoTLightGlobe.h"
#include "WoTTestLeech.h"
#include "Net/UnrealNetwork.h"
#include "Dom/JsonObject.h"
#include "Components/BoxComponent.h"
#include "UObject/UnrealType.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTNetWarningCaptureSpec, Log, All);

namespace
{
	// The minimal ticking game world with the project game mode, matching
	// NM_Standalone -> ROLE_Authority; items take the non-respawn path.
	// Tick the world NumTicks fixed 1/60 steps (the harness scripts in integer
	// frames, unlike the inventory tests' duration-based TickWorld).
	// The repo's known spawn gotcha: collision-less test world needs AlwaysSpawn.
	FActorSpawnParameters AlwaysSpawnParams()
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return P;
	}

	AActor* SpawnHarnessFloor(UWorld* World)
	{
		AActor* Floor = World->SpawnActor<AActor>(
			FVector(0, 0, -50), FRotator::ZeroRotator, AlwaysSpawnParams());
		if (!Floor) return nullptr;
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
		Box->SetBoxExtent(FVector(5000, 5000, 50));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Floor->SetRootComponent(Box);
		Box->RegisterComponent();
		Floor->SetActorLocation(FVector(0, 0, -50));
		return Floor;
	}

	AActor* SpawnGateToolingStaticBox(UWorld* World, const FVector& Center,
		const FVector& Extent)
	{
		AActor* Actor = World->SpawnActor<AActor>(
			Center, FRotator::ZeroRotator, AlwaysSpawnParams());
		if (!Actor) return nullptr;
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Actor->SetRootComponent(Box);
		Box->RegisterComponent();
		Actor->SetActorLocation(Center);
		return Actor;
	}

	AActor* SpawnRetailVisBlockingBox(UWorld* World, const FVector& Center,
		const FVector& Extent)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			Center, FRotator::ZeroRotator, AlwaysSpawnParams());
		if (!Actor) return nullptr;
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Box->SetupAttachment(Actor->GetRootComponent());
		Box->SetBoxExtent(Extent);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		return Actor;
	}

	struct FGateToolingAssistedJumpOutcome
	{
		bool bCommandAccepted = false;
		float PawnZ = 0.0f;
		int32 AssistedJumpCount = 0;
		int32 AssistedJumpCountAfterReset = 0;
		// The first assisted jump's recorded blocker, if any.
		bool bBlockerRecorded = false;
		double BlockerHeight = 0.0;
		double BlockerNormalX = 0.0;
	};

	FGateToolingAssistedJumpOutcome RunGateToolingAssistedJumpScenario(
		bool bJumpWhenBlocked)
	{
		FGateToolingAssistedJumpOutcome Outcome;
		UWorld* World = WoTTest::CreateSpecWorld();
		SpawnGateToolingStaticBox(World, FVector(0, 0, -10),
			FVector(1000, 1000, 10));
		SpawnGateToolingStaticBox(World, FVector(150, 0, 20),
			FVector(75, 100, 20));
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 48), FRotator::ZeroRotator, AlwaysSpawnParams());
		AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
		Controller->SetAsLocalPlayerController();
		Controller->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);

		FWoTNetTestChannel Channel;
		TSharedRef<FJsonObject> Reset = MakeShared<FJsonObject>();
		Reset->SetStringField(TEXT("cmd"), TEXT("probe_reset"));
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Channel.DispatchCommandForTesting(World, Reset, Response);

		TSharedRef<FJsonObject> MoveTo = MakeShared<FJsonObject>();
		MoveTo->SetStringField(TEXT("cmd"), TEXT("moveto"));
		MoveTo->SetNumberField(TEXT("tol"), 20.0);
		if (bJumpWhenBlocked)
			MoveTo->SetBoolField(TEXT("jump_when_blocked"), true);
		TArray<TSharedPtr<FJsonValue>> Target = {
			MakeShared<FJsonValueNumber>(150.0),
			MakeShared<FJsonValueNumber>(0.0),
			MakeShared<FJsonValueNumber>(88.0)};
		MoveTo->SetArrayField(TEXT("path"), {
			MakeShared<FJsonValueArray>(Target)});
		Response = MakeShared<FJsonObject>();
		Outcome.bCommandAccepted = Channel.DispatchCommandForTesting(
			World, MoveTo, Response);
		for (int32 Tick = 0; Tick < 240; ++Tick)
		{
			Channel.TickDirector(World);
			WoTTest::TickWorld(World, 1);
		}
		Outcome.PawnZ = Pawn->GetActorLocation().Z;

		auto ReadAssistedJumpCount = [&]()
		{
			TSharedRef<FJsonObject> ResultsRequest = MakeShared<FJsonObject>();
			ResultsRequest->SetStringField(TEXT("cmd"), TEXT("probe_results"));
			TSharedRef<FJsonObject> ResultsResponse = MakeShared<FJsonObject>();
			Channel.DispatchCommandForTesting(World, ResultsRequest, ResultsResponse);
			const TArray<TSharedPtr<FJsonValue>>* AssistedJumps = nullptr;
			const TSharedPtr<FJsonObject> Data =
				ResultsResponse->GetObjectField(TEXT("data"));
			if (!Data->TryGetArrayField(TEXT("assisted_jumps"), AssistedJumps)
				|| !AssistedJumps)
				return -1;
			const TSharedPtr<FJsonObject>* Blocker = nullptr;
			if (!AssistedJumps->IsEmpty()
				&& (*AssistedJumps)[0]->AsObject()->TryGetObjectField(
					TEXT("blocker"), Blocker))
			{
				Outcome.bBlockerRecorded = true;
				Outcome.BlockerHeight = (*Blocker)->GetNumberField(TEXT("height"));
				Outcome.BlockerNormalX = (*Blocker)->GetArrayField(TEXT("normal"))[0]
					->AsNumber();
			}
			return AssistedJumps->Num();
		};
		Outcome.AssistedJumpCount = ReadAssistedJumpCount();
		Response = MakeShared<FJsonObject>();
		Channel.DispatchCommandForTesting(World, Reset, Response);
		Outcome.AssistedJumpCountAfterReset = ReadAssistedJumpCount();

		WoTTest::DestroySpecWorld(World);
		return Outcome;
	}

	AWoTCharacter* SpawnProbeCaster(UWorld* World, const FVector& Location,
		EWoTCreatureFaction Faction = EWoTCreatureFaction::Whitecloak,
		TSubclassOf<AWoTCharacter> CharacterClass = AWoTCharacter::StaticClass())
	{
		APlayerController* Controller = World->SpawnActor<APlayerController>();
		Controller->InitPlayerState();
		AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
			CharacterClass, Location, FRotator::ZeroRotator, AlwaysSpawnParams());
		Caster->SummonFaction = Faction;
		Controller->Possess(Caster);
		Caster->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		return Caster;
	}

	void ResetProbeHarness(FWoTNetTestChannel& Channel, UWorld* World)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("probe_reset"));
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Channel.DispatchCommandForTesting(World, Request, Response);
	}

	bool BeginWeaveProbe(FWoTNetTestChannel& Channel, UWorld* World,
		const FString& Label, const FString& Weave, const AWoTCharacter* Caster,
		const FString& AnimationTarget = FString())
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("probe_begin"));
		Request->SetStringField(TEXT("label"), Label);
		Request->SetStringField(TEXT("kind"), TEXT("weave"));
		Request->SetStringField(TEXT("weave"), Weave);
		Request->SetStringField(TEXT("caster"), Caster->GetName());
		if (!AnimationTarget.IsEmpty())
		{
			Request->SetArrayField(TEXT("animation_targets"), {
				MakeShared<FJsonValueString>(AnimationTarget)});
		}
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		return Channel.DispatchCommandForTesting(World, Request, Response);
	}

	TSharedPtr<FJsonObject> FinishWeaveProbe(FWoTNetTestChannel& Channel,
		UWorld* World, const FString& Label)
	{
		Channel.TickDirector(World);
		TSharedRef<FJsonObject> End = MakeShared<FJsonObject>();
		End->SetStringField(TEXT("cmd"), TEXT("probe_end"));
		End->SetStringField(TEXT("label"), Label);
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		if (!Channel.DispatchCommandForTesting(World, End, Response)) return nullptr;

		TSharedRef<FJsonObject> Results = MakeShared<FJsonObject>();
		Results->SetStringField(TEXT("cmd"), TEXT("probe_results"));
		Response = MakeShared<FJsonObject>();
		if (!Channel.DispatchCommandForTesting(World, Results, Response)) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>& Windows =
			Response->GetObjectField(TEXT("data"))->GetArrayField(TEXT("windows"));
		return Windows.IsEmpty() ? nullptr : Windows.Last()->AsObject();
	}

	bool EvidenceContains(const TSharedPtr<FJsonObject>& Window,
		const TCHAR* Field, const FString& Needle)
	{
		if (!Window) return false;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Window->TryGetArrayField(Field, Values) || !Values) return false;
		return Values->ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Value)
		{
			return Value.IsValid()
				&& Value->AsString().Contains(Needle, ESearchCase::IgnoreCase);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetSnapshotSpec,
	"WoT.Net.Snapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetSnapshotSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);

	TSharedPtr<FJsonObject> Snap = WoTNetTest::BuildSnapshot(World);
	TestNotNull(TEXT("snapshot"), Snap.Get());
	const TArray<TSharedPtr<FJsonValue>>* Pawns = nullptr;
	TestTrue(TEXT("has pawns array"),
		Snap->TryGetArrayField(TEXT("pawns"), Pawns) && Pawns->Num() == 1);
	if (Pawns && Pawns->Num() == 1)
	{
		const TSharedPtr<FJsonObject> P0 = (*Pawns)[0]->AsObject();
		TestEqual(TEXT("role is authority"),
			(int32)P0->GetNumberField(TEXT("role")), (int32)ROLE_Authority);
		TestEqual(TEXT("health"), (int32)P0->GetNumberField(TEXT("health")), 100);
		TestFalse(TEXT("not dead"), P0->GetBoolField(TEXT("dead")));
	}
	TestTrue(TEXT("netmode field"), Snap->HasField(TEXT("netmode")));
	TestTrue(TEXT("projectiles field"), Snap->HasField(TEXT("projectiles")));
	TestTrue(TEXT("pickups field"), Snap->HasField(TEXT("pickups")));
	TestTrue(TEXT("players field"), Snap->HasField(TEXT("players")));

	// The condensed single-line form is JSONL-safe (no embedded newline).
	const FString Line = WoTNetTest::BuildSnapshotJson(World);
	TestTrue(TEXT("json line non-empty"), Line.Len() > 0);
	TestFalse(TEXT("json line has no newline"), Line.Contains(TEXT("\n")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetActionsSpec,
	"WoT.Net.Actions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetActionsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	// Real controller class — the cast/fire input path lives on AWoTPlayerController now.
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	// APlayerController::IsLocalController() gates on the cached
	// bIsLocalPlayerController, which is only set when a real ULocalPlayer is
	// attached. A bare spawned PC in this headless world has none, so the pawn
	// reads as not-locally-controlled and the CMC ignores AddMovementInput. Force
	// the flag (the client's genuine ULocalPlayer does this for real; the
	// multiprocess smoke exercises that path).
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	// No floor in the test world (matches WoTInventoryTests): fly so the held
	// move produces clean horizontal displacement.
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	const FVector Start = Pawn->GetActorLocation();
	WoTNetTest::FMoveState Move;
	Move.Axis = FVector2D(1, 0);
	for (int32 i = 0; i < 30; ++i)
	{
		WoTNetTest::ApplyMoveTick(World, Move);
		WoTTest::TickWorld(World, 1);
	}
	TestTrue(TEXT("pawn moved +X"), (Pawn->GetActorLocation() - Start).X > 10.f);

	const int32 H0 = Pawn->Health;
	TestTrue(TEXT("damage applied"), WoTNetTest::DamagePawn(World, Pawn->GetName(), 25.f));
	WoTTest::TickWorld(World, 2);
	TestEqual(TEXT("health dropped 25"), Pawn->Health, H0 - 25);

	// cast/next route through the character's server-RPC input path; here they
	// just confirm the possessed pawn accepts them (no-op-safe without a target).
	TestTrue(TEXT("cast press ok"), WoTNetTest::CastPress(World));
	TestTrue(TEXT("cast release ok"), WoTNetTest::CastRelease(World));
	TestTrue(TEXT("next artifact ok"), WoTNetTest::NextArtifact(World));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetGiveSpec,
	"WoT.Net.Give", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetGiveSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Grant a projectile-launcher artifact by bare native class name (authority).
	const bool bGave = WoTNetTest::GiveArtifact(
		World, Pawn->GetName(), TEXT("WoTAngrealInvFireball"), 10);
	TestTrue(TEXT("give returned true"), bGave);
	WoTTest::TickWorld(World, 2);

	// It should now show up as a held inventory actor in the snapshot.
	const TSharedPtr<FJsonObject> Snap = WoTNetTest::BuildSnapshot(World);
	const TArray<TSharedPtr<FJsonValue>>* Pickups = nullptr;
	bool bFound = false;
	if (Snap->TryGetArrayField(TEXT("pickups"), Pickups))
	{
		for (const TSharedPtr<FJsonValue>& V : *Pickups)
		{
			if (V->AsObject()->GetStringField(TEXT("cls")).Contains(TEXT("Fireball")))
			{
				bFound = true;
			}
		}
	}
	TestTrue(TEXT("granted artifact present as inventory actor"), bFound);

	// give selects what it grants (so a subsequent castpress fires it), even
	// though the pawn already had a spawn-selected artifact.
	const TArray<TSharedPtr<FJsonValue>>* SnapPawns = nullptr;
	if (Snap->TryGetArrayField(TEXT("pawns"), SnapPawns) && SnapPawns->Num() >= 1)
	{
		const TSharedPtr<FJsonObject> P0 = (*SnapPawns)[0]->AsObject();
		TestTrue(TEXT("granted artifact is selected"),
			P0->GetStringField(TEXT("selected")).Contains(TEXT("Fireball")));
	}

	// An unknown class name resolves to nothing and is rejected (no crash).
	TestFalse(TEXT("unknown class rejected"),
		WoTNetTest::GiveArtifact(World, Pawn->GetName(), TEXT("WoTAngrealInvNoSuchThing"), 1));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetGiveSealEditingPhaseSpec,
	"WoT.Net.GiveSealEditingPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetGiveSealEditingPhaseSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	AWoTPlayerController* Controller =
		World->SpawnActor<AWoTPlayerController>(AlwaysSpawnParams());
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	if (Controller)
	{
		Controller->InitPlayerState();
		if (AWoTPlayerState* PS = Controller->GetPlayerState<AWoTPlayerState>())
		{
			PS->Team = 1;
		}
		Controller->Possess(Pawn);
	}
	if (!TestNotNull(TEXT("battle mode"), Mode)
		|| !TestNotNull(TEXT("battle info"), Battle)
		|| !TestNotNull(TEXT("team pawn"), Pawn))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->bTeleportersDisabled = true; // real pre-play/editing pickup gate
	Battle->FinishSpawning(FTransform::Identity);
	Mode->Battle = Battle;

	TestTrue(TEXT("guarded command grants a seal during the editing phase"),
		WoTNetTest::GiveSeal(World, Pawn->GetName()));
	AWoTSeal* CarriedSeal = nullptr;
	for (AWoTInventory* Item : Pawn->InventoryComponent->Items)
	{
		if (AWoTSeal* Seal = Cast<AWoTSeal>(Item))
		{
			CarriedSeal = Seal;
			break;
		}
	}
	if (TestNotNull(TEXT("real seal is present in production inventory"), CarriedSeal))
	{
		TestTrue(TEXT("pawn owns the carried seal"),
			CarriedSeal->GetOwner() == Pawn);
		TestEqual(TEXT("seal is compatible with its carrier's team"),
			static_cast<int32>(CarriedSeal->Team), 1);
		TestEqual(TEXT("seal is in the real Held state"),
			CarriedSeal->GetItemState(), EWoTInventoryState::Held);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProbeSummonOutputsSpec,
	"WoT.Net.Probes.WeaveSummonOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProbeSummonOutputsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("WoT game mode"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTNetTestChannel Channel;
	ResetProbeHarness(Channel, World);

	auto Verify = [&](AWoTCharacter* Caster, TSubclassOf<AWoTResourceSpawnerInv> Type,
		const FString& Weave)
	{
		Game->GivePawnAngreal(Caster, Type, 5);
		AWoTResourceSpawnerInv* Item = Cast<AWoTResourceSpawnerInv>(
			Caster->InventoryComponent->FindInventoryType(Type));
		if (!TestNotNull(*(Weave + TEXT(" item")), Item)) return;
		TestTrue(*(Weave + TEXT(" probe began")),
			BeginWeaveProbe(Channel, World, Weave, Weave, Caster));
		Item->CastArtifact();
		const TSharedPtr<FJsonObject> Window = FinishWeaveProbe(Channel, World, Weave);
		TestTrue(*(Weave + TEXT(" records its real helper")), Window.IsValid()
			&& Window->GetBoolField(TEXT("projectile_or_effect"))
			&& EvidenceContains(Window, TEXT("effect_evidence"), TEXT("WoTNPCCharacter:")));
	};

	Verify(SpawnProbeCaster(World, FVector(0, 0, 100)),
		AWoTAngrealInvChampion::StaticClass(), TEXT("WoTAngrealInvChampion"));
	Verify(SpawnProbeCaster(World, FVector(0, 500, 100)),
		AWoTAngrealInvGuardian::StaticClass(), TEXT("WoTAngrealInvGuardian"));
	Verify(SpawnProbeCaster(World, FVector(0, 1000, 100)),
		AWoTAngrealInvMinion::StaticClass(), TEXT("WoTAngrealInvMinion"));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProbeStateOutputsSpec,
	"WoT.Net.Probes.WeaveStateOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProbeStateOutputsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("WoT game mode"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTNetTestChannel Channel;
	ResetProbeHarness(Channel, World);
	auto FinishAndExpect = [&](const FString& Label, const FString& Evidence)
	{
		const TSharedPtr<FJsonObject> Window = FinishWeaveProbe(Channel, World, Label);
		TestTrue(*(Label + TEXT(" records its direct output")), Window.IsValid()
			&& Window->GetBoolField(TEXT("projectile_or_effect"))
			&& EvidenceContains(Window, TEXT("effect_evidence"), Evidence));
	};

	// Disguise mutates the caster and also emits its retail AppearEffect.
	AWoTCharacter* Disguiser = SpawnProbeCaster(World, FVector(0, 0, 100));
	World->SpawnActor<AWoTCharacter>(FVector(100, 0, 100),
		FRotator::ZeroRotator, AlwaysSpawnParams());
	Game->GivePawnAngreal(Disguiser, AWoTAngrealInvDisguise::StaticClass(), 5);
	AWoTAngrealInvDisguise* Disguise = Cast<AWoTAngrealInvDisguise>(
		Disguiser->InventoryComponent->FindInventoryType(
			AWoTAngrealInvDisguise::StaticClass()));
	BeginWeaveProbe(Channel, World, TEXT("disguise"),
		TEXT("WoTAngrealInvDisguise"), Disguiser);
	Disguise->CastArtifact();
	FinishAndExpect(TEXT("disguise"), TEXT("state:disguised:"));

	AWoTCharacter* Healer = SpawnProbeCaster(World, FVector(0, 500, 100));
	Game->GivePawnAngreal(Healer, AWoTAngrealInvHeal::StaticClass(), 5);
	AWoTAngrealInvHeal* Heal = Cast<AWoTAngrealInvHeal>(
		Healer->InventoryComponent->FindInventoryType(AWoTAngrealInvHeal::StaticClass()));
	Healer->Health = 50;
	BeginWeaveProbe(Channel, World, TEXT("heal"), TEXT("WoTAngrealInvHeal"), Healer);
	Heal->CastArtifact();
	FinishAndExpect(TEXT("heal"), TEXT("state:health-increased:"));

	AWoTCharacter* Levitator = SpawnProbeCaster(World, FVector(0, 1000, 100));
	Levitator->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Game->GivePawnAngreal(Levitator, AWoTAngrealInvLevitate::StaticClass(), 5);
	AWoTAngrealInvLevitate* Levitate = Cast<AWoTAngrealInvLevitate>(
		Levitator->InventoryComponent->FindInventoryType(
			AWoTAngrealInvLevitate::StaticClass()));
	BeginWeaveProbe(Channel, World, TEXT("levitate"),
		TEXT("WoTAngrealInvLevitate"), Levitator);
	Levitate->CastArtifact();
	WoTTest::TickWorld(World, 1);
	FinishAndExpect(TEXT("levitate"), TEXT("state:levitating:"));

	AWoTCharacter* Shifter = SpawnProbeCaster(World, FVector(0, 1500, 100));
	Game->GivePawnAngreal(Shifter, AWoTAngrealInvShift::StaticClass(), 5);
	AWoTAngrealInvShift* Shift = Cast<AWoTAngrealInvShift>(
		Shifter->InventoryComponent->FindInventoryType(AWoTAngrealInvShift::StaticClass()));
	BeginWeaveProbe(Channel, World, TEXT("shift"), TEXT("WoTAngrealInvShift"), Shifter);
	Shift->CastArtifact();
	FinishAndExpect(TEXT("shift"), TEXT("state:shifted:"));

	AWoTCharacter* WindCaster = SpawnProbeCaster(World, FVector(0, 2000, 100));
	World->SpawnActor<AWoTCharacter>(FVector(400, 2000, 100),
		FRotator::ZeroRotator, AlwaysSpawnParams());
	Game->GivePawnAngreal(WindCaster, AWoTAngrealInvWhirlwind::StaticClass(), 5);
	AWoTAngrealInvWhirlwind* Whirlwind = Cast<AWoTAngrealInvWhirlwind>(
		WindCaster->InventoryComponent->FindInventoryType(
			AWoTAngrealInvWhirlwind::StaticClass()));
	BeginWeaveProbe(Channel, World, TEXT("whirlwind"),
		TEXT("WoTAngrealInvWhirlwind"), WindCaster);
	Whirlwind->CastArtifact();
	FinishAndExpect(TEXT("whirlwind"), TEXT("WoTWhirlwindLeech:"));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProbeLightGlobeOutputsSpec,
	"WoT.Net.Probes.LightGlobeOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProbeLightGlobeOutputsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("WoT game mode"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTNetTestChannel Channel;
	ResetProbeHarness(Channel, World);
	struct FLightGlobeCase
	{
		const TCHAR* Label;
		TSubclassOf<AWoTCharacter> CasterClass;
		const TCHAR* GlobeClass;
	};
	// In retail the LG* classes are map-placement helpers which replace themselves
	// with the base inventory class. Once carried, GiveTo derives the visible color
	// from the owner's real faction/PlayerColor. Exercise that production seam.
	const TArray<FLightGlobeCase> Cases = {
		{TEXT("WoTAngrealInvLightGlobe"), AWoTAesSedai::StaticClass(),
			TEXT("WoTLightGlobeBlue")},
		{TEXT("WoTAngrealInvLGBlue"), AWoTAesSedai::StaticClass(),
			TEXT("WoTLightGlobeBlue")},
		{TEXT("WoTAngrealInvLGGreen"), AWoTCharacter::StaticClass(),
			TEXT("WoTLightGlobeGreen")},
		{TEXT("WoTAngrealInvLGPurple"), AWoTHound::StaticClass(),
			TEXT("WoTLightGlobePurple")},
		{TEXT("WoTAngrealInvLGRed"), AWoTForsaken::StaticClass(),
			TEXT("WoTLightGlobeRed")},
		{TEXT("WoTAngrealInvLGYellow"), AWoTWhitecloak::StaticClass(),
			TEXT("WoTLightGlobeYellow")}};
	for (int32 CaseIndex = 0; CaseIndex < Cases.Num(); ++CaseIndex)
	{
		const FLightGlobeCase& Case = Cases[CaseIndex];
		AWoTCharacter* Caster = World->SpawnActor<AWoTCharacter>(
			Case.CasterClass, FVector(0, CaseIndex * 300.0, 100),
			FRotator::ZeroRotator, AlwaysSpawnParams());
		if (!TestNotNull(*(FString(Case.Label) + TEXT(" real faction caster")), Caster))
		{
			continue;
		}
		Game->GivePawnAngreal(Caster, AWoTAngrealInvLightGlobe::StaticClass(), 20);
		AWoTAngrealInvLightGlobe* Item = Cast<AWoTAngrealInvLightGlobe>(
			Caster->InventoryComponent->FindInventoryType(
				AWoTAngrealInvLightGlobe::StaticClass()));
		if (!TestNotNull(*(FString(Case.Label) + TEXT(" real base item")), Item))
		{
			continue;
		}
		BeginWeaveProbe(Channel, World, Case.Label, Case.Label, Caster);
		Item->CastArtifact();
		const TSharedPtr<FJsonObject> Window = FinishWeaveProbe(Channel, World, Case.Label);
		TestTrue(*(FString(Case.Label) + TEXT(" records its following colored globe")),
			Window.IsValid() && Window->GetBoolField(TEXT("projectile_or_effect"))
			&& EvidenceContains(Window, TEXT("effect_evidence"), Case.GlobeClass));
		Item->CastArtifact(); // toggle the real globe off before the next color cast
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProbeUnrelatedActorSpec,
	"WoT.Net.Probes.UnrelatedActorDoesNotCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProbeUnrelatedActorSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = SpawnProbeCaster(World, FVector::ZeroVector);
	FWoTNetTestChannel Channel;
	ResetProbeHarness(Channel, World);
	BeginWeaveProbe(Channel, World, TEXT("negative"), TEXT("WoTAngrealInvHeal"), Caster);
	World->SpawnActor<AWoTLightGlobe>(FVector(500, 0, 100),
		FRotator::ZeroRotator, AlwaysSpawnParams());
	const TSharedPtr<FJsonObject> Window = FinishWeaveProbe(Channel, World, TEXT("negative"));
	TestTrue(TEXT("negative probe window exists"), Window.IsValid());
	if (Window)
	{
		TestFalse(TEXT("an unrelated real actor is not weave evidence"),
			Window->GetBoolField(TEXT("projectile_or_effect")));
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProbeImpactOutputsSpec,
	"WoT.Net.Probes.WeaveImpactOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProbeImpactOutputsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("WoT game mode"), Game))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTNetTestChannel Channel;
	TestTrue(TEXT("impact probe channel started"),
		Channel.Start(0, TEXT("127.0.0.1"), TEXT("impact-spec")));
	ResetProbeHarness(Channel, World);
	auto ExpectImpact = [&](const FString& Label, const FString& Evidence)
	{
		const TSharedPtr<FJsonObject> Window = FinishWeaveProbe(Channel, World, Label);
		TestTrue(*(Label + TEXT(" records its attributed retail impact")),
			Window.IsValid() && Window->GetBoolField(TEXT("impact_or_decal"))
			&& EvidenceContains(Window, TEXT("impact_evidence"), Evidence));
	};

	AWoTCharacter* AMACaster = SpawnProbeCaster(World, FVector(0, 0, 100));
	AWoTCharacter* AMAVictim = World->SpawnActor<AWoTCharacter>(
		FVector(300, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	Game->GivePawnAngreal(AMACaster, AWoTAngrealInvAMA::StaticClass(), 5);
	AWoTAngrealInvAMA* AMA = Cast<AWoTAngrealInvAMA>(
		AMACaster->InventoryComponent->FindInventoryType(AWoTAngrealInvAMA::StaticClass()));
	BeginWeaveProbe(Channel, World, TEXT("ama"), TEXT("WoTAngrealInvAMA"), AMACaster);
	WoTTest::TickWorld(World, 1); // retail casts happen after the world-time epoch
	AMA->CastArtifact();
	Channel.TickDirector(World); // cache the real launch before its touch output
	AWoTAngrealAMAProjectile* Mine = nullptr;
	for (TActorIterator<AWoTAngrealAMAProjectile> It(World); It; ++It)
	{
		if (It->SourceAngreal == AMA) { Mine = *It; break; }
	}
	if (TestNotNull(TEXT("AMA real cast launched its mine"), Mine))
	{
		Mine->Place();
		Mine->Touch(AMAVictim);
	}
	ExpectImpact(TEXT("ama"), TEXT("WoTAMAPearl:"));
	// The pearl expands far beyond the AMA victim. Remove that completed case so
	// it cannot absorb the independently staged ExpWard below.
	for (TActorIterator<AWoTAMAPearl> It(World); It; ++It)
	{
		It->Destroy();
	}

	AWoTCharacter* WardCaster = SpawnProbeCaster(World, FVector(0, 500, 100));
	AWoTCharacter* WardVictim = World->SpawnActor<AWoTCharacter>(
		FVector(300, 500, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	SpawnRetailVisBlockingBox(World, FVector(150, 500, 100), FVector(10, 100, 100));
	Game->GivePawnAngreal(WardCaster, AWoTAngrealInvExpWard::StaticClass(), 5);
	AWoTAngrealInvExpWard* ExpWard = Cast<AWoTAngrealInvExpWard>(
		WardCaster->InventoryComponent->FindInventoryType(
			AWoTAngrealInvExpWard::StaticClass()));
	BeginWeaveProbe(Channel, World, TEXT("expward"),
		TEXT("WoTAngrealInvExpWard"), WardCaster);
	WoTTest::TickWorld(World, 1);
	ExpWard->CastArtifact();
	Channel.TickDirector(World);
	AWoTExpWardProjectile* Ward = nullptr;
	for (TActorIterator<AWoTExpWardProjectile> It(World); It; ++It)
	{
		if (It->SourceAngreal == ExpWard) { Ward = *It; break; }
	}
	if (TestNotNull(TEXT("ExpWard real cast placed its ward"), Ward))
	{
		WoTTest::TickWorld(World, 1.1f);
		TestTrue(TEXT("ExpWard armed through its retail timer"), Ward->bArmed);
		// Cross the armed retail cylinder through real movement; direct handler
		// injection bypasses both the collision shape and arming trigger.
		WardVictim->SetActorLocation(Ward->GetActorLocation());
		WoTTest::TickWorld(World, 1, 1.0f / 60.0f);
	}
	ExpectImpact(TEXT("expward"), TEXT("WoTExplosionEffect:"));

	AWoTCharacter* FireworksCaster = SpawnProbeCaster(World, FVector(0, 1000, 100));
	Game->GivePawnAngreal(FireworksCaster, AWoTAngrealInvFireworks::StaticClass(), 5);
	AWoTAngrealInvFireworks* Fireworks = Cast<AWoTAngrealInvFireworks>(
		FireworksCaster->InventoryComponent->FindInventoryType(
			AWoTAngrealInvFireworks::StaticClass()));
	BeginWeaveProbe(Channel, World, TEXT("fireworks"),
		TEXT("WoTAngrealInvFireworks"), FireworksCaster);
	WoTTest::TickWorld(World, 1);
	Fireworks->CastArtifact();
	Channel.TickDirector(World);
	AWoTFireworksProjectile* Rocket = nullptr;
	for (TActorIterator<AWoTFireworksProjectile> It(World); It; ++It)
	{
		if (It->SourceAngreal == Fireworks) { Rocket = *It; break; }
	}
	if (TestNotNull(TEXT("Fireworks real cast launched its projectile"), Rocket))
	{
		// Retail chooses a 2..6 second fuse. Observe each frame so the short-lived
		// burst is latched without calling the detonation helper from the spec.
		for (int32 Frame = 0; Frame < 7 * 60 && IsValid(Rocket); ++Frame)
		{
			WoTTest::TickWorld(World, 1);
			Channel.TickDirector(World);
		}
		TestFalse(TEXT("Fireworks detonated through its retail fuse"), IsValid(Rocket));
	}
	ExpectImpact(TEXT("fireworks"), TEXT("WoTParticleSprayer:"));

	AWoTCharacter* SeekerCaster = SpawnProbeCaster(World, FVector(0, 1250, 100));
	AWoTCharacter* SeekerVictim = World->SpawnActor<AWoTCharacter>(
		FVector(300, 1250, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	Game->GivePawnAngreal(SeekerCaster, AWoTAngrealInvSeeker::StaticClass(), 5);
	AWoTAngrealInvSeeker* Seeker = Cast<AWoTAngrealInvSeeker>(
		SeekerCaster->InventoryComponent->FindInventoryType(
			AWoTAngrealInvSeeker::StaticClass()));
	Seeker->Target = SeekerVictim;
	BeginWeaveProbe(Channel, World, TEXT("seeker"),
		TEXT("WoTAngrealInvSeeker"), SeekerCaster);
	WoTTest::TickWorld(World, 1);
	Seeker->CastArtifact();
	Channel.TickDirector(World);
	AWoTSeekerProjectile* SeekerShot = nullptr;
	for (TActorIterator<AWoTSeekerProjectile> It(World); It; ++It)
	{
		if (It->SourceAngreal == Seeker) { SeekerShot = *It; break; }
	}
	if (TestNotNull(TEXT("Seeker real cast launched its homing projectile"),
		SeekerShot))
	{
		SeekerShot->ProcessTouch(SeekerVictim, SeekerVictim->GetActorLocation());
	}
	ExpectImpact(TEXT("seeker"), TEXT("projectile-contact:WoTSeekerProjectile"));

	AWoTCharacter* BalefireCaster = SpawnProbeCaster(World, FVector(0, 1500, 100));
	const float BeamZ = BalefireCaster->GetActorLocation().Z
		+ BalefireCaster->BaseEyeHeight - 30.0f;
	SpawnRetailVisBlockingBox(World, FVector(500, 1500, BeamZ),
		FVector(10, 100, 100));
	Game->GivePawnAngreal(BalefireCaster, AWoTAngrealInvBalefire::StaticClass(), 5);
	AWoTAngrealInvBalefire* Balefire = Cast<AWoTAngrealInvBalefire>(
		BalefireCaster->InventoryComponent->FindInventoryType(
			AWoTAngrealInvBalefire::StaticClass()));
	Balefire->PrimeTime = 0.1f;
	BeginWeaveProbe(Channel, World, TEXT("balefire"),
		TEXT("WoTAngrealInvBalefire"), BalefireCaster);
	Balefire->CastArtifact();
	WoTTest::TickWorld(World, 12);
	Balefire->UnCastArtifact();
	ExpectImpact(TEXT("balefire"), TEXT("WoTDecal:"));

	Channel.Stop();
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProbeWaterShieldRealCastSpec,
	"WoT.Net.Probes.WaterShieldRealCastAnimation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProbeWaterShieldRealCastSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTCharacter* Defender = SpawnProbeCaster(World, FVector(0, 0, 100));
	AWoTCharacter* Attacker = SpawnProbeCaster(World, FVector(300, 0, 100));
	if (!TestNotNull(TEXT("WoT game mode"), Game)
		|| !TestNotNull(TEXT("water-shield defender"), Defender)
		|| !TestNotNull(TEXT("Ice attacker"), Attacker))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	FWoTNetTestChannel Channel;
	ResetProbeHarness(Channel, World);
	Game->GivePawnAngreal(Defender, AWoTAngrealInvWaterShield::StaticClass(), 5);
	AWoTAngrealInvWaterShield* Shield = Cast<AWoTAngrealInvWaterShield>(
		Defender->InventoryComponent->FindInventoryType(
			AWoTAngrealInvWaterShield::StaticClass()));
	Game->GivePawnAngreal(Attacker, AWoTAngrealInvIce::StaticClass(), 5);
	AWoTAngrealInvIce* Ice = Cast<AWoTAngrealInvIce>(
		Attacker->InventoryComponent->FindInventoryType(
			AWoTAngrealInvIce::StaticClass()));
	if (!TestNotNull(TEXT("real Water Shield item"), Shield)
		|| !TestNotNull(TEXT("real Ice item"), Ice))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	BeginWeaveProbe(Channel, World, TEXT("water-shield-real"),
		TEXT("WoTAngrealInvWaterShield"), Defender,
		TEXT("Angreal.Elemental.Water_A"));
	Shield->CastArtifact();
	TestNotNull(TEXT("Water Shield installed its production reflector"),
		Defender->FindComponentByClass<UWoTIgnoreWaterElementReflector>());
	Ice->Target = Defender;
	WoTTest::TickWorld(World, 1);
	Ice->CastArtifact();
	AWoTIceProjectile* IceShot = nullptr;
	for (TActorIterator<AWoTIceProjectile> It(World); It; ++It)
	{
		if (It->SourceAngreal == Ice) { IceShot = *It; break; }
	}
	if (TestNotNull(TEXT("real Ice cast launched its projectile"), IceShot))
	{
		IceShot->ProcessTouch(Defender, Defender->GetActorLocation());
	}

	AWoTMeshEffect* WaterEffect = nullptr;
	for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
	{
		if (It->ConfigName == TEXT("WaterShieldVisual"))
		{
			WaterEffect = *It;
			break;
		}
	}
	if (TestNotNull(TEXT("blocking Ice spawned the retail WaterShieldVisual"),
		WaterEffect))
	{
		for (int32 Frame = 0; Frame < 24 && IsValid(WaterEffect); ++Frame)
		{
			WaterEffect->MarkDrawnForTesting();
			WoTTest::TickWorld(World, 1);
			Channel.TickDirector(World);
		}
	}
	const TSharedPtr<FJsonObject> Window = FinishWeaveProbe(
		Channel, World, TEXT("water-shield-real"));
	const TSharedPtr<FJsonObject>* Advances = nullptr;
	bool bAdvanced = false;
	if (Window && Window->TryGetObjectField(TEXT("animation_advances"), Advances)
		&& Advances && Advances->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Advances)->Values)
		{
			bAdvanced |= Pair.Key.Contains(TEXT("Water_A"))
				&& Pair.Value.IsValid() && Pair.Value->AsNumber() > 0.0;
		}
	}
	TestTrue(TEXT("real Water Shield feedback advances its visible Water_A texture"),
		bAdvanced);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetProbeVisibleAnimationOutputsSpec,
	"WoT.Net.Probes.VisibleAnimationOutputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetProbeVisibleAnimationOutputsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Caster = SpawnProbeCaster(World, FVector::ZeroVector,
		EWoTCreatureFaction::Whitecloak, AWoTWhitecloak::StaticClass());
	AWoTCharacter* LightningVictim = World->SpawnActor<AWoTAesSedai>(
		FVector(100, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	FWoTNetTestChannel Channel;
	ResetProbeHarness(Channel, World);
	auto Advanced = [](const TSharedPtr<FJsonObject>& Window)
	{
		if (!Window) return false;
		const TSharedPtr<FJsonObject>* Values = nullptr;
		if (!Window->TryGetObjectField(TEXT("animation_advances"), Values)
			|| !Values || !Values->IsValid()) return false;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Values)->Values)
		{
			if (Pair.Value.IsValid() && Pair.Value->AsNumber() > 0.0) return true;
		}
		return false;
	};
	struct FShieldCase
	{
		const TCHAR* Weave;
		const TCHAR* Config;
		const TCHAR* Texture;
	};
	const FShieldCase Shields[] = {
		{TEXT("WoTAngrealInvEarthShield"), TEXT("EarthShieldVisual"),
			TEXT("Angreal.Elemental.Earth_A")},
		{TEXT("WoTAngrealInvSpiritShield"), TEXT("SpiritShieldVisual"),
			TEXT("Angreal.Elemental.Spirit_A")},
		{TEXT("WoTAngrealInvWaterShield"), TEXT("WaterShieldVisual"),
			TEXT("Angreal.Elemental.Water_A")},
	};
	for (const FShieldCase& Case : Shields)
	{
		const FString Label = FString(TEXT("animation-")) + Case.Config;
		BeginWeaveProbe(Channel, World, Label, Case.Weave, Caster, Case.Texture);
		AWoTMeshEffect* Effect = WoTMeshFX::SpawnLocal(*World, Case.Config,
			Caster->GetActorLocation(), FRotator::ZeroRotator, Caster);
		if (TestNotNull(*(Label + TEXT(" spawned through WoTMeshFX")), Effect))
		{
			for (int32 Frame = 0; Frame < 24 && IsValid(Effect); ++Frame)
			{
				Effect->MarkDrawnForTesting();
				WoTTest::TickWorld(World, 1);
				Channel.TickDirector(World);
			}
		}
		TestTrue(*(Label + TEXT(" records visible TextureSet changes")),
			Advanced(FinishWeaveProbe(Channel, World, Label)));
	}

	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	Game->GivePawnAngreal(Caster, AWoTAngrealInvLightning::StaticClass(), 10);
	AWoTAngrealInvLightning* Lightning = Cast<AWoTAngrealInvLightning>(
		Caster->InventoryComponent->FindInventoryType(
			AWoTAngrealInvLightning::StaticClass()));
	if (TestNotNull(TEXT("Lightning item"), Lightning))
	{
		BeginWeaveProbe(Channel, World, TEXT("animation-lightning"),
			TEXT("WoTAngrealInvLightning"), Caster, TEXT("Angreal.Effects.LSE_A"));
		Lightning->CastArtifact();
		for (int32 Frame = 0; Frame < 8; ++Frame)
		{
			WoTTest::TickWorld(World, 1);
			Channel.TickDirector(World);
		}
		TestTrue(TEXT("Lightning real cast latched its nearby victim"),
			LightningVictim && Lightning->Leeches.ContainsByPredicate(
				[LightningVictim](const UWoTDecreaseHealthLeech* Leech)
				{
					return IsValid(Leech) && Leech->GetOwner() == LightningVictim;
				}));
		TestTrue(TEXT("Lightning records its visible LSE_A skin changes"),
			Advanced(FinishWeaveProbe(Channel, World, TEXT("animation-lightning"))));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetDeathReactionSpec,
	"WoT.Net.DeathReaction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetDeathReactionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	// No floor in the test world (matches the Actions spec): fly so the held move
	// produces clean horizontal velocity, then kill the MOVING pawn.
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTNetTest::FMoveState Move;
	Move.Axis = FVector2D(1, 0);
	for (int32 i = 0; i < 20; ++i)
	{
		WoTNetTest::ApplyMoveTick(World, Move);
		WoTTest::TickWorld(World, 1);
	}
	TestFalse(TEXT("pawn is moving before death"),
		Pawn->GetCharacterMovement()->Velocity.IsNearlyZero());

	// Simulate the simulated-proxy death path: the replicated value arrives and its
	// OnRep fires. On a proxy Died() never runs, so only OnRep_bDead can shut the
	// corpse down.
	Pawn->bDead = true;
	Pawn->OnRep_bDead();

	TestTrue(TEXT("movement stopped on proxy"),
		Pawn->GetCharacterMovement()->Velocity.IsNearlyZero());
	TestEqual(TEXT("capsule collision disabled on proxy"),
		Pawn->GetCapsuleComponent()->GetCollisionEnabled(), ECollisionEnabled::NoCollision);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetAnimChannelSpec,
	"WoT.Net.AnimChannel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetAnimChannelSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	// Authority stamps an intent: the replicated channel carries the sequence name,
	// and the re-fire token advances (a fresh trigger of the same anim).
	const int32 Token0 = (int32)Pawn->RepAnim.PlayToken;
	Pawn->SetAnimIntent(FName("Run"), 1.0f, true);
	TestEqual(TEXT("replicated anim name is Run"),
		Pawn->GetReplicatedAnimName().ToString(), FString(TEXT("Run")));
	TestNotEqual(TEXT("play token bumped"), (int32)Pawn->RepAnim.PlayToken, Token0);

	// A proxy that received a Death intent must replay without crashing when it has
	// no vertex-anim component (the headless / pre-mesh-bind case).
	AWoTCharacter* Proxy = World->SpawnActor<AWoTCharacter>(
		FVector(500, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("proxy spawned"), Proxy);
	if (Proxy)
	{
		Proxy->RepAnim.Sequence = FName("Death");
		Proxy->OnRep_Anim();   // no component present -> must be a safe no-op
		TestEqual(TEXT("proxy anim name reads Death"),
			Proxy->GetReplicatedAnimName().ToString(), FString(TEXT("Death")));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetHarnessActionsSpec,
	"WoT.Net.HarnessActions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetHarnessActionsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);

	// FacePoint aims player 0's control yaw at a world point.
	const FVector Point(100.f, 100.f, 100.f);
	TestTrue(TEXT("face point ok"), WoTNetTest::FacePoint(World, Point));
	const float WantYaw = (Point - Pawn->GetActorLocation()).Rotation().Yaw;
	TestTrue(TEXT("control yaw faces point within 1 deg"),
		FMath::Abs(FRotator::NormalizeAxis(PC->GetControlRotation().Yaw - WantYaw)) < 1.0f);

	// FaceTarget aims at another pawn resolved by name.
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(-200.f, 300.f, 100.f), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("target spawned"), Target);
	if (Target)
	{
		TestTrue(TEXT("face target ok"), WoTNetTest::FaceTarget(World, Target->GetName()));
		const float WantT = (Target->GetActorLocation() - Pawn->GetActorLocation()).Rotation().Yaw;
		TestTrue(TEXT("control yaw faces target within 1 deg"),
			FMath::Abs(FRotator::NormalizeAxis(PC->GetControlRotation().Yaw - WantT)) < 1.0f);
	}

	// SpawnZone (authority) creates a current-bearing zone with the requested velocity.
	const FVector ZoneVel(300.f, 0.f, 0.f);
	TestTrue(TEXT("spawn zone ok"),
		WoTNetTest::SpawnZone(World, FVector(0, 0, 100), ZoneVel));
	AWoTZoneVolume* Zone = nullptr;
	if (TActorIterator<AWoTZoneVolume> It(World); It) { Zone = *It; }
	TestNotNull(TEXT("zone volume exists"), Zone);
	if (Zone)
	{
		TestTrue(TEXT("zone velocity matches request"), Zone->ZoneVelocity.Equals(ZoneVel));
	}

	// A client / invalid world is rejected (mirrors DamagePawn's NM_Client gate; an
	// in-process spec can't fabricate an NM_Client world, so the null-world false path
	// stands in for the authority guard here — live clients prove the netmode gate).
	TestFalse(TEXT("null world rejected"),
		WoTNetTest::SpawnZone(nullptr, FVector(0, 0, 100), ZoneVel));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelSpec,
	"WoT.Net.Channel.Loopback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelSpec::RunTest(const FString&)
{
	FString ArenaTravel;
	TestTrue(TEXT("play-through travel accepts an arena/map pairing"),
		WoTNetTest::BuildTravelURL(TEXT("Arena_01"), TEXT("arena"), ArenaTravel));
	TestTrue(TEXT("play-through travel remains listen/LAN"),
		ArenaTravel.Contains(TEXT("?listen"))
		&& ArenaTravel.Contains(TEXT("?bIsLanMatch")));
	FString RejectedTravel;
	TestFalse(TEXT("play-through travel rejects a mismatched game kind"),
		WoTNetTest::BuildTravelURL(TEXT("Arena_01"), TEXT("battle"), RejectedTravel));
	TestFalse(TEXT("play-through travel rejects URL injection"),
		WoTNetTest::BuildTravelURL(TEXT("Arena_01?game=bad"), TEXT("arena"),
			RejectedTravel));

	UWorld* World = WoTTest::CreateSpecWorld();
	// a pawn so the snapshot verb has something to report
	World->SpawnActor<AWoTCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	AWoTParticleSprayer* CloseupSprayer = World->SpawnActor<AWoTParticleSprayer>(
		FVector(200, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("close-up sprayer spawned"), CloseupSprayer);
	if (CloseupSprayer) CloseupSprayer->bOn = false;

	FWoTNetTestChannel Channel;
	TestTrue(TEXT("channel started"), Channel.Start(0, TEXT("127.0.0.1"), TEXT("sekrit")));
	const uint16 Port = Channel.GetBoundPort();
	TestTrue(TEXT("ephemeral port assigned"), Port != 0);

	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	bool bValidIp = false;
	Addr->SetIp(TEXT("127.0.0.1"), bValidIp);
	Addr->SetPort(Port);

	auto Connect = [&](const TCHAR* Name) -> FSocket*
	{
		FSocket* S = FTcpSocketBuilder(Name).AsBlocking().Build();
		if (S) { S->Connect(*Addr); }
		return S;
	};
	auto SendLine = [](FSocket* S, const FString& L)
	{
		const FString Wire = L + TEXT("\n");
		FTCHARToUTF8 U(*Wire);
		int32 Sent = 0;
		S->Send(reinterpret_cast<const uint8*>(U.Get()), U.Length(), Sent);
	};
	auto PumpAndRead = [&](FSocket* S, FString& Buf, int32 Times)
	{
		for (int32 k = 0; k < Times; ++k)
		{
			Channel.PumpOnGameThread(World);
			FPlatformProcess::Sleep(0.02f);
			uint32 Pending = 0;
			while (S->HasPendingData(Pending) && Pending > 0)
			{
				uint8 B[4096];
				int32 R = 0;
				if (!S->Recv(B, sizeof(B), R) || R <= 0) { break; }
				FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(B), R);
				Buf.Append(Conv.Get(), Conv.Length());
			}
		}
	};

	FSocket* Client = Connect(TEXT("WoTNetTestClient"));
	TestNotNull(TEXT("client socket built"), Client);
	if (Client)
	{
		FString Buf;
		SendLine(Client, TEXT("{\"token\":\"sekrit\"}"));
		SendLine(Client, TEXT("{\"id\":1,\"cmd\":\"ping\"}"));
		PumpAndRead(Client, Buf, 12);
		TestTrue(TEXT("ping id echoed"), Buf.Contains(TEXT("\"id\":1")));
		TestTrue(TEXT("ping ok true"), Buf.Contains(TEXT("\"ok\":true")));

		SendLine(Client, TEXT("{\"id\":2,\"cmd\":\"snapshot\"}"));
		PumpAndRead(Client, Buf, 12);
		TestTrue(TEXT("snapshot data has pawns"), Buf.Contains(TEXT("\"pawns\"")));

		SendLine(Client, TEXT("{\"id\":3,\"cmd\":\"status\"}"));
		PumpAndRead(Client, Buf, 12);
		TestTrue(TEXT("status reports front-end readiness"),
			Buf.Contains(TEXT("\"front_end_ready\"")));
		TestTrue(TEXT("status reports world identity"),
			Buf.Contains(TEXT("\"world\"")));
		TestTrue(TEXT("status reports net mode"),
			Buf.Contains(TEXT("\"net_mode\"")));
		TestTrue(TEXT("status reports battle phase"),
			Buf.Contains(TEXT("\"battle_phase\"")));
		TestTrue(TEXT("status reports team and seals"),
			Buf.Contains(TEXT("\"team\"")) && Buf.Contains(TEXT("\"seals\"")));
		TestTrue(TEXT("status reports a travel generation"),
			Buf.Contains(TEXT("\"world_generation\"")));

		SendLine(Client, TEXT("{\"id\":4,\"cmd\":\"probe_reset\"}"));
		SendLine(Client, TEXT("{\"id\":5,\"cmd\":\"probe_begin\","
			"\"label\":\"spec-window\",\"kind\":\"weave\"}"));
		PumpAndRead(Client, Buf, 12);
		Channel.TickDirector(World);
		SendLine(Client, TEXT("{\"id\":6,\"cmd\":\"probe_end\","
			"\"label\":\"spec-window\"}"));
		SendLine(Client, TEXT("{\"id\":7,\"cmd\":\"probe_results\"}"));
		PumpAndRead(Client, Buf, 12);
		TestTrue(TEXT("probe verbs return the labelled observation window"),
			Buf.Contains(TEXT("\"label\":\"spec-window\""))
			&& Buf.Contains(TEXT("\"projectile_or_effect\""))
			&& Buf.Contains(TEXT("\"animation_start\"")));
		TestTrue(TEXT("probe results expose all automatic-check collections"),
			Buf.Contains(TEXT("\"materialized\""))
			&& Buf.Contains(TEXT("\"late_joins\""))
			&& Buf.Contains(TEXT("\"anomalies\""))
			&& Buf.Contains(TEXT("\"route_snaps\""))
			&& Buf.Contains(TEXT("\"assisted_jumps\""))
			&& Buf.Contains(TEXT("\"falls\""))
			&& Buf.Contains(TEXT("\"log_warnings\""))
			&& Buf.Contains(TEXT("\"log_overflow\""))
			&& Buf.Contains(TEXT("\"seeker_samples\"")));

		if (CloseupSprayer)
		{
			SendLine(Client, FString::Printf(
				TEXT("{\"id\":8,\"cmd\":\"sprayer_activate\",\"actor\":\"%s\"}"),
				*CloseupSprayer->GetName()));
			PumpAndRead(Client, Buf, 12);
			TestTrue(TEXT("play-through close-up activates the exact sprayer"),
				CloseupSprayer->bOn);
		}

		Client->Close();
		SS->DestroySocket(Client);
	}

	// Wrong token: the first line drops the connection; the follow-up cmd is
	// never answered.
	FSocket* Bad = Connect(TEXT("WoTNetTestBad"));
	if (Bad)
	{
		FString BadBuf;
		SendLine(Bad, TEXT("{\"token\":\"wrong\"}"));
		SendLine(Bad, TEXT("{\"id\":9,\"cmd\":\"ping\"}"));
		PumpAndRead(Bad, BadBuf, 12);
		TestFalse(TEXT("wrong token gets no reply"), BadBuf.Contains(TEXT("\"ok\"")));
		Bad->Close();
		SS->DestroySocket(Bad);
	}

	Channel.Stop();
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelLogWarningsSpec,
	"WoT.Net.Channel.LogWarnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelLogWarningsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AddExpectedError(TEXT("Pre-start warning"),
		EAutomationExpectedErrorFlags::Contains, 1);
	GLog->Log(TEXT("LogWoTNetWarningCaptureSpec"), ELogVerbosity::Warning,
		TEXT("Pre-start warning 123"));
	// UE_LOG routes warnings/errors through GWarn, which the automation
	// framework replaces during a test (expected errors stop there). The packaged
	// game's GWarn forwards to GLog, so drive GLog directly: that is the stream
	// the channel's output device subscribes to.
	FWoTNetTestChannel Channel;
	if (!TestTrue(TEXT("channel started for log capture"),
		Channel.Start(0, TEXT("127.0.0.1"), TEXT("log-spec"))))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	auto Dispatch = [&](const TCHAR* Command)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), Command);
		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		TestTrue(FString::Printf(TEXT("%s accepted"), Command),
			Channel.DispatchCommandForTesting(World, Request, Response));
		return Response;
	};
	auto FindCaptured = [](const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Verbosity) -> TSharedPtr<FJsonObject>
	{
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		if (!Data.IsValid()
			|| !Data->TryGetArrayField(TEXT("log_warnings"), Warnings))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Warnings)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid()
				? Value->AsObject() : nullptr;
			if (Entry.IsValid()
				&& Entry->GetStringField(TEXT("category"))
					== TEXT("LogWoTNetWarningCaptureSpec")
				&& Entry->GetStringField(TEXT("verbosity")) == Verbosity)
			{
				return Entry;
			}
		}
		return nullptr;
	};

	TSharedPtr<FJsonObject> Data =
		Dispatch(TEXT("probe_results"))->GetObjectField(TEXT("data"));
	TestNull(TEXT("GLog backlog from before channel start is not captured"),
		FindCaptured(Data, TEXT("Warning")).Get());
	Dispatch(TEXT("probe_reset"));
	AddExpectedError(TEXT("Repeated warning actor"),
		EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("Repeated error code"),
		EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedError(TEXT("Worker-thread warning"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Unable to load SocketSubsystem module EOS"),
		EAutomationExpectedErrorFlags::Contains, 1);
	GLog->Log(TEXT("LogWoTNetWarningCaptureSpec"), ELogVerbosity::Warning,
		TEXT("Repeated warning actor 17 at 0x1234"));
	TFuture<void> WorkerLog = Async(EAsyncExecution::ThreadPool, []()
	{
		GLog->Log(TEXT("LogWoTNetWarningCaptureSpec"), ELogVerbosity::Warning,
		TEXT("Worker-thread warning"));
	});
	WorkerLog.Wait();
	GLog->Log(TEXT("LogWoTNetWarningCaptureSpec"), ELogVerbosity::Error,
		TEXT("Repeated error code 500"));
	GLog->Log(TEXT("LogSockets"), ELogVerbosity::Warning,
		TEXT("Unable to load SocketSubsystem module EOS"));
	Dispatch(TEXT("probe_results"));
	WoTTest::TickWorld(World, 2);
	GLog->Log(TEXT("LogWoTNetWarningCaptureSpec"), ELogVerbosity::Warning,
		TEXT("Repeated warning actor 90210 at 0xABCDEF"));
	GLog->Log(TEXT("LogWoTNetWarningCaptureSpec"), ELogVerbosity::Error,
		TEXT("Repeated error code 501"));

	Data = Dispatch(TEXT("probe_results"))->GetObjectField(TEXT("data"));
	const TSharedPtr<FJsonObject> Warning = FindCaptured(Data, TEXT("Warning"));
	const TSharedPtr<FJsonObject> Error = FindCaptured(Data, TEXT("Error"));
	if (TestNotNull(TEXT("deduped warning appears in probe results"), Warning.Get()))
	{
		TestEqual(TEXT("warning address and digit runs are normalized"),
			Warning->GetStringField(TEXT("message")),
			FString(TEXT("Repeated warning actor # at 0x#")));
		TestEqual(TEXT("warning repeats increment the count"),
			static_cast<int32>(Warning->GetNumberField(TEXT("count"))), 2);
		TestTrue(TEXT("warning timestamps preserve repeat time"),
			Warning->GetNumberField(TEXT("first_t"))
				< Warning->GetNumberField(TEXT("last_t")));
	}
	if (TestNotNull(TEXT("deduped error appears in probe results"), Error.Get()))
	{
		TestEqual(TEXT("error digit runs are normalized"),
			Error->GetStringField(TEXT("message")),
			FString(TEXT("Repeated error code #")));
		TestEqual(TEXT("error repeats increment the count"),
			static_cast<int32>(Error->GetNumberField(TEXT("count"))), 2);
		TestTrue(TEXT("error timestamps preserve repeat time"),
			Error->GetNumberField(TEXT("first_t"))
				< Error->GetNumberField(TEXT("last_t")));
	}
	const TArray<TSharedPtr<FJsonValue>>& Captured =
		Data->GetArrayField(TEXT("log_warnings"));
	TestTrue(TEXT("worker-thread warning reaches the game-thread drain"),
		Captured.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid()
				? Value->AsObject() : nullptr;
			return Entry.IsValid()
				&& Entry->GetStringField(TEXT("category"))
					== TEXT("LogWoTNetWarningCaptureSpec")
				&& Entry->GetStringField(TEXT("message"))
					== TEXT("Worker-thread warning");
		}));
	TestFalse(TEXT("known-benign socket startup warning is excluded"),
		Captured.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid()
				? Value->AsObject() : nullptr;
			return Entry.IsValid()
				&& Entry->GetStringField(TEXT("category")) == TEXT("LogSockets")
				&& Entry->GetStringField(TEXT("message")).Contains(
					TEXT("Unable to load SocketSubsystem module EOS"));
		}));
	TestEqual(TEXT("no distinct-entry overflow"),
		static_cast<int32>(Data->GetNumberField(TEXT("log_overflow"))), 0);

	Dispatch(TEXT("probe_reset"));
	Data = Dispatch(TEXT("probe_results"))->GetObjectField(TEXT("data"));
	TestEqual(TEXT("probe_reset clears captured log warnings"),
		Data->GetArrayField(TEXT("log_warnings")).Num(), 0);
	TestEqual(TEXT("probe_reset clears log overflow"),
		static_cast<int32>(Data->GetNumberField(TEXT("log_overflow"))), 0);

	Channel.Stop();
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelMoveToSnapSpec,
	"WoT.Net.Channel.MoveToSnap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelMoveToSnapSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(1000, 0, 200), FRotator::ZeroRotator, AlwaysSpawnParams());
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	Pawn->GetCharacterMovement()->Velocity = FVector(200, 0, 0);

	FWoTNetTestChannel Channel;
	auto Dispatch = [&](const TSharedRef<FJsonObject>& Request)
	{
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		TestTrue(TEXT("command accepted"),
			Channel.DispatchCommandForTesting(World, Request, Response));
		return Response;
	};
	TSharedRef<FJsonObject> Reset = MakeShared<FJsonObject>();
	Reset->SetStringField(TEXT("cmd"), TEXT("probe_reset"));
	Dispatch(Reset);
	auto MoveTo = [&](double X)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("moveto"));
		Request->SetNumberField(TEXT("snap"), 100.0);
		TArray<TSharedPtr<FJsonValue>> Coordinates = {
			MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(0.0),
			MakeShared<FJsonValueNumber>(100.0)};
		TArray<TSharedPtr<FJsonValue>> Path = {
			MakeShared<FJsonValueArray>(Coordinates)};
		Request->SetArrayField(TEXT("path"), Path);
		Dispatch(Request);
	};

	MoveTo(0.0);
	TestTrue(TEXT("far pawn is safely re-stationed at the first path point"),
		FMath::Abs(Pawn->GetActorLocation().X) < 1.0
		&& Pawn->GetActorLocation().Z > 0.0);
	TestTrue(TEXT("snap zeroes retained movement velocity"),
		Pawn->GetVelocity().IsNearlyZero());
	TSharedRef<FJsonObject> ResultsRequest = MakeShared<FJsonObject>();
	ResultsRequest->SetStringField(TEXT("cmd"), TEXT("probe_results"));
	TSharedRef<FJsonObject> Results = Dispatch(ResultsRequest);
	const TArray<TSharedPtr<FJsonValue>>* Snaps = nullptr;
	const TSharedPtr<FJsonObject> Data = Results->GetObjectField(TEXT("data"));
	TestTrue(TEXT("far moveto records one route snap"),
		Data->TryGetArrayField(TEXT("route_snaps"), Snaps) && Snaps->Num() == 1);
	if (Snaps && Snaps->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Snap = (*Snaps)[0]->AsObject();
		TestEqual(TEXT("route snap names the pawn"),
			Snap->GetStringField(TEXT("pawn")), Pawn->GetName());
		TestTrue(TEXT("route snap carries from/to vectors and threshold distance"),
			Snap->GetArrayField(TEXT("from")).Num() == 3
			&& Snap->GetArrayField(TEXT("to")).Num() == 3
			&& Snap->GetNumberField(TEXT("distance")) > 100.0);
	}

	Pawn->SetActorLocation(FVector(10, 0, Pawn->GetActorLocation().Z), false,
		nullptr, ETeleportType::TeleportPhysics);
	MoveTo(0.0);
	Results = Dispatch(ResultsRequest);
	Snaps = nullptr;
	TestTrue(TEXT("near pawn does not add a route snap"),
		Results->GetObjectField(TEXT("data"))->TryGetArrayField(
			TEXT("route_snaps"), Snaps) && Snaps->Num() == 1);

	auto Command = [&](const TCHAR* Cmd)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), Cmd);
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		return Channel.DispatchCommandForTesting(World, Request, Response);
	};
	TestTrue(TEXT("the near moveto is active"), Command(TEXT("moveto_status")));
	TestTrue(TEXT("stop is accepted"), Command(TEXT("stop")));
	TestFalse(TEXT("stop ends the active moveto"), Command(TEXT("moveto_status")));

	// No floor exists out here: the re-station fails, the walk still starts.
	const FVector BeforeFailedSnap = Pawn->GetActorLocation();
	MoveTo(20000.0);
	TestTrue(TEXT("a moveto whose snap fails is still active"),
		Command(TEXT("moveto_status")));
	TestTrue(TEXT("a failed snap leaves the pawn where it stood"),
		Pawn->GetActorLocation().Equals(BeforeFailedSnap, 1.0));
	Results = Dispatch(ResultsRequest);
	Snaps = nullptr;
	TestTrue(TEXT("a failed snap is recorded"),
		Results->GetObjectField(TEXT("data"))->TryGetArrayField(
			TEXT("route_snaps"), Snaps) && Snaps->Num() == 2);
	if (Snaps && Snaps->Num() == 2)
	{
		TestTrue(TEXT("the first snap succeeded"),
			!(*Snaps)[0]->AsObject()->GetBoolField(TEXT("failed")));
		TestTrue(TEXT("the second snap is marked failed"),
			(*Snaps)[1]->AsObject()->GetBoolField(TEXT("failed")));
	}

	TSharedRef<FJsonObject> TourPoint = MakeShared<FJsonObject>();
	TourPoint->SetStringField(TEXT("cmd"), TEXT("tourpoint"));
	TourPoint->SetStringField(TEXT("pawn"), Pawn->GetName());
	TourPoint->SetNumberField(TEXT("x"), 0.0);
	TourPoint->SetNumberField(TEXT("y"), 0.0);
	TourPoint->SetNumberField(TEXT("z"), 100.0);
	Dispatch(TourPoint);
	TestFalse(TEXT("a tour point ends the pawn's active moveto"),
		Command(TEXT("moveto_status")));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelMoveToInWalkRecoverySpec,
	"WoT.Net.Channel.MoveToInWalkRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelMoveToInWalkRecoverySpec::RunTest(const FString&)
{
	// One moveto now spans a whole sweep leg, so a stalled or off-route walker
	// is re-stationed during the walk rather than at the next moveto.
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);

	FWoTNetTestChannel Channel;
	auto Dispatch = [&](const TSharedRef<FJsonObject>& Request)
	{
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		TestTrue(TEXT("command accepted"),
			Channel.DispatchCommandForTesting(World, Request, Response));
		return Response;
	};
	auto Snaps = [&]()
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("probe_results"));
		// Keep the response alive while its array is copied out.
		const TSharedRef<FJsonObject> Response = Dispatch(Request);
		const TSharedPtr<FJsonObject> Data = Response->GetObjectField(TEXT("data"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("route_snaps"), Values)
			|| Values == nullptr)
			return TArray<TSharedPtr<FJsonValue>>();
		return *Values;
	};
	auto Place = [&](const FVector& Location)
	{
		Pawn->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	};
	TSharedRef<FJsonObject> Reset = MakeShared<FJsonObject>();
	Reset->SetStringField(TEXT("cmd"), TEXT("probe_reset"));
	Dispatch(Reset);

	const float StandZ = Pawn->GetActorLocation().Z;
	TSharedRef<FJsonObject> MoveTo = MakeShared<FJsonObject>();
	MoveTo->SetStringField(TEXT("cmd"), TEXT("moveto"));
	MoveTo->SetNumberField(TEXT("snap"), 256.0);
	MoveTo->SetBoolField(TEXT("smooth"), true);
	TArray<TSharedPtr<FJsonValue>> Path;
	for (const double X : {0.0, 1000.0, 2000.0, 3000.0})
	{
		Path.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(0.0),
			MakeShared<FJsonValueNumber>(100.0)}));
	}
	MoveTo->SetArrayField(TEXT("path"), Path);
	Dispatch(MoveTo);
	Channel.TickDirector(World);
	TestEqual(TEXT("a walker at the route start is not re-stationed"),
		Snaps().Num(), 0);

	// Progress along the route keeps the walk going without a snap. (A jump
	// longer than a tick's walk counts as a teleport, not progress.)
	for (double X = 50.0; X <= 400.0; X += 50.0)
	{
		Place(FVector(X, 10, StandZ));
		Channel.TickDirector(World);
	}
	TestEqual(TEXT("a walker advancing on the route is left alone"), Snaps().Num(), 0);

	// No progress for the stall window: re-stationed 200 uu further along.
	WoTTest::TickWorld(World, 2.2f);
	Place(FVector(400, 10, StandZ));
	Channel.TickDirector(World);
	TArray<TSharedPtr<FJsonValue>> Recorded = Snaps();
	if (TestEqual(TEXT("a stalled walker is re-stationed once"), Recorded.Num(), 1))
	{
		const TSharedPtr<FJsonObject> Snap = Recorded[0]->AsObject();
		TestEqual(TEXT("the stall is labelled"), Snap->GetStringField(TEXT("reason")),
			FString(TEXT("stalled")));
		TestFalse(TEXT("the stall re-station succeeded"), Snap->GetBoolField(TEXT("failed")));
	}
	TestTrue(TEXT("the stalled walker now stands further along the route"),
		Pawn->GetActorLocation().X > 500.0 && Pawn->GetActorLocation().X < 700.0
		&& FMath::Abs(Pawn->GetActorLocation().Y) < 32.0);
	Channel.TickDirector(World);
	TestEqual(TEXT("the re-station restarts the stall window"), Snaps().Num(), 1);

	// Knocked well off the route (a fall to another floor, a push, a map
	// teleporter): re-stationed 200 uu ahead of its last route progress, once
	// the re-station grace has passed, not wherever it landed.
	WoTTest::TickWorld(World, 0.6f);
	Place(FVector(1500, 700, StandZ));
	Channel.TickDirector(World);
	Recorded = Snaps();
	if (TestEqual(TEXT("an off-route walker is re-stationed"), Recorded.Num(), 2))
	{
		const TSharedPtr<FJsonObject> Snap = Recorded[1]->AsObject();
		TestEqual(TEXT("the off-route snap is labelled"),
			Snap->GetStringField(TEXT("reason")), FString(TEXT("off_route")));
		TestTrue(TEXT("the off-route distance is recorded"),
			Snap->GetNumberField(TEXT("distance")) > 256.0);
	}
	TestTrue(TEXT("the off-route walker is back on the route, past its last progress"),
		Pawn->GetActorLocation().X > 850.0 && Pawn->GetActorLocation().X < 950.0
		&& FMath::Abs(Pawn->GetActorLocation().Y) < 32.0);

	// A walker displaced again after every re-station (Arena_06: the leg ended
	// in a teleporter trigger) is re-stationed at most three times, a grace
	// apart, and then the walk is given up instead of re-stationing per tick.
	Dispatch(Reset);
	Place(FVector(0, 0, StandZ));
	Dispatch(MoveTo);
	Channel.TickDirector(World);
	const FVector Courtyard(1500, 900, StandZ);
	Place(Courtyard);
	Channel.TickDirector(World);
	TestEqual(TEXT("the first displacement is re-stationed"), Snaps().Num(), 1);
	Place(Courtyard);
	Channel.TickDirector(World);
	TestEqual(TEXT("no re-station inside the grace"), Snaps().Num(), 1);
	for (int32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		WoTTest::TickWorld(World, 0.6f);
		Place(Courtyard);
		Channel.TickDirector(World);
	}
	TestEqual(TEXT("three re-stations without on-route progress"), Snaps().Num(), 3);
	WoTTest::TickWorld(World, 0.6f);
	Place(Courtyard);
	Channel.TickDirector(World);
	Recorded = Snaps();
	if (TestEqual(TEXT("the fourth displacement gives the walk up"), Recorded.Num(), 4))
	{
		const TSharedPtr<FJsonObject> Snap = Recorded[3]->AsObject();
		TestEqual(TEXT("the give-up is labelled"),
			Snap->GetStringField(TEXT("reason")), FString(TEXT("abandoned")));
		TestTrue(TEXT("the give-up is a failure"), Snap->GetBoolField(TEXT("failed")));
	}
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		WoTTest::TickWorld(World, 0.6f);
		Place(Courtyard);
		Channel.TickDirector(World);
	}
	TestEqual(TEXT("an abandoned walk records nothing more"), Snaps().Num(), 4);
	TestTrue(TEXT("the abandoned walker stays where it was put"),
		FVector::Dist2D(Pawn->GetActorLocation(), Courtyard) < 1.0);

	// A walk without snap keeps the old behaviour: no in-walk recovery.
	Dispatch(Reset);
	MoveTo->SetNumberField(TEXT("snap"), 0.0);
	Place(FVector(0, 0, StandZ));
	Dispatch(MoveTo);
	WoTTest::TickWorld(World, 2.2f);
	Place(FVector(0, 0, StandZ));
	Channel.TickDirector(World);
	TestEqual(TEXT("snap 0 disables in-walk recovery"), Snaps().Num(), 0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelMoveToRetracedRouteSpec,
	"WoT.Net.Channel.MoveToRetracedRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelMoveToRetracedRouteSpec::RunTest(const FString&)
{
	// A sweep leg doubles back through corridors it has already walked. The
	// walker's progress stays on the pass it is walking: an earlier search
	// jumped to a later pass, ended the walk early and froze the walker.
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);

	FWoTNetTestChannel Channel;
	auto Dispatch = [&](const TSharedRef<FJsonObject>& Request)
	{
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		TestTrue(TEXT("command accepted"),
			Channel.DispatchCommandForTesting(World, Request, Response));
		return Response;
	};
	auto Request = [](const TCHAR* Command)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("cmd"), Command);
		return Object;
	};
	auto Progress = [&]()
	{
		return Dispatch(Request(TEXT("moveto_status")))->GetNumberField(TEXT("curve_distance"));
	};
	auto Place = [&](const FVector& Location)
	{
		Pawn->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	};
	Dispatch(Request(TEXT("probe_reset")));
	const float StandZ = Pawn->GetActorLocation().Z;

	TSharedRef<FJsonObject> MoveTo = Request(TEXT("moveto"));
	MoveTo->SetNumberField(TEXT("snap"), 256.0);
	MoveTo->SetNumberField(TEXT("lead"), 40.0);
	MoveTo->SetBoolField(TEXT("smooth"), true);
	TArray<TSharedPtr<FJsonValue>> Path;
	// Out along y = 0, back along y = 8, then off along x = 0.
	for (const FVector2D& Point : {FVector2D(0, 0), FVector2D(500, 0), FVector2D(1000, 0),
		FVector2D(1000, 8), FVector2D(500, 8), FVector2D(0, 8), FVector2D(0, 1000)})
	{
		Path.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(Point.X), MakeShared<FJsonValueNumber>(Point.Y),
			MakeShared<FJsonValueNumber>(StandZ)}));
	}
	MoveTo->SetArrayField(TEXT("path"), Path);
	Place(FVector(0, 6, StandZ));
	Dispatch(MoveTo);
	TestTrue(TEXT("the walk starts on the outbound pass"), Progress() < 16.0);

	// Walk out at 7 uu a tick, nearer the return pass than the outbound one.
	for (int32 Step = 1; Step <= 20; ++Step)
	{
		Place(FVector(7.0 * Step, 6, StandZ));
		Channel.TickDirector(World);
	}
	const double OutboundProgress = Progress();
	TestTrue(TEXT("progress follows the outbound pass"),
		OutboundProgress > 120.0 && OutboundProgress < 160.0);
	TestFalse(TEXT("the walk is not over"),
		Dispatch(Request(TEXT("moveto_status")))->GetBoolField(TEXT("done")));

	// Walked distance per probe window: steps count, a teleport does not.
	TSharedRef<FJsonObject> Begin = Request(TEXT("probe_begin"));
	Begin->SetStringField(TEXT("label"), TEXT("sweep:0001"));
	Dispatch(Begin);
	for (int32 Step = 1; Step <= 10; ++Step)
	{
		Place(FVector(140.0 + 10.0 * Step, 6, StandZ));
		Channel.TickDirector(World);
	}
	Place(FVector(900, 6, StandZ));
	Channel.TickDirector(World);
	TSharedRef<FJsonObject> End = Request(TEXT("probe_end"));
	End->SetStringField(TEXT("label"), TEXT("sweep:0001"));
	Dispatch(End);
	const TSharedPtr<FJsonObject> Data = Dispatch(Request(TEXT("probe_results")))
		->GetObjectField(TEXT("data"));
	const TArray<TSharedPtr<FJsonValue>>* Windows = nullptr;
	if (TestTrue(TEXT("one probe window"), Data.IsValid()
		&& Data->TryGetArrayField(TEXT("windows"), Windows) && Windows->Num() == 1))
	{
		const double Walked = (*Windows)[0]->AsObject()->GetNumberField(TEXT("walked_xy"));
		TestTrue(TEXT("the window counts the walked steps, not the teleport"),
			FMath::IsNearlyEqual(Walked, 100.0, 1.0));
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelMoveToHairpinNoJumpSpec,
	"WoT.Net.Channel.MoveToHairpinNoJump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelMoveToHairpinNoJumpSpec::RunTest(const FString&)
{
	// A sweep route turns straight back at a dead-end spur (Arena_03's
	// PathNode140). The walker barely moves while it turns but still advances
	// along the route, so the blocked-walker jump must not fire; a walker
	// making no route progress either still gets it.
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(990, 2, 48), FRotator::ZeroRotator, AlwaysSpawnParams());
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	WoTTest::TickWorld(World, 0.2f);
	const float StandZ = Pawn->GetActorLocation().Z;

	FWoTNetTestChannel Channel;
	auto Dispatch = [&](const TSharedRef<FJsonObject>& Request)
	{
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		TestTrue(TEXT("command accepted"),
			Channel.DispatchCommandForTesting(World, Request, Response));
		return Response;
	};
	auto Request = [](const TCHAR* Command)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("cmd"), Command);
		return Object;
	};
	auto AssistedJumps = [&]()
	{
		const TSharedPtr<FJsonObject> Data = Dispatch(Request(TEXT("probe_results")))
			->GetObjectField(TEXT("data"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Data.IsValid() && Data->TryGetArrayField(TEXT("assisted_jumps"), Values)
			&& Values ? Values->Num() : -1;
	};
	auto Place = [&](double X, double Y)
	{
		Pawn->SetActorLocation(FVector(X, Y, StandZ), false, nullptr,
			ETeleportType::TeleportPhysics);
		Pawn->GetCharacterMovement()->StopMovementImmediately();
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	};
	Dispatch(Request(TEXT("probe_reset")));

	TSharedRef<FJsonObject> MoveTo = Request(TEXT("moveto"));
	MoveTo->SetNumberField(TEXT("lead"), 40.0);
	MoveTo->SetBoolField(TEXT("smooth"), true);
	MoveTo->SetBoolField(TEXT("jump_when_blocked"), true);
	TArray<TSharedPtr<FJsonValue>> Path;
	// Out along y = 0 to the spur's end, back along y = 8.
	for (const FVector2D& Point : {FVector2D(0, 0), FVector2D(500, 0),
		FVector2D(1000, 0), FVector2D(1000, 8), FVector2D(500, 8), FVector2D(0, 8)})
	{
		Path.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(Point.X), MakeShared<FJsonValueNumber>(Point.Y),
			MakeShared<FJsonValueNumber>(StandZ)}));
	}
	MoveTo->SetArrayField(TEXT("path"), Path);
	Place(990, 2);
	Dispatch(MoveTo);

	// Half a second later the walker stands 7 uu away, on the way back.
	WoTTest::TickWorld(World, 0.6f);
	Place(985, 7);
	Channel.TickDirector(World);
	TestEqual(TEXT("a walker turning at a hairpin is not blocked"), AssistedJumps(), 0);

	// Another half second with no movement and no route progress: blocked.
	WoTTest::TickWorld(World, 0.6f);
	Place(985, 7);
	Channel.TickDirector(World);
	TestEqual(TEXT("a walker making no progress still gets the jump"), AssistedJumps(), 1);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelMoveToDeadEndTipSpec,
	"WoT.Net.Channel.MoveToDeadEndTip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelMoveToDeadEndTipSpec::RunTest(const FString&)
{
	// Real dead-end spurs as the gate walks them (lead 40, smoothed, snap
	// 256) on a flat floor, heights relative to the standing walker. An earlier
	// walker parked short of each tip until the blocked-walker jump fired:
	// - Arena_03 route points 652-670 to WotPathNode8 / PlayerStart4 and
	//   straight back: its steering point landed on itself half a lead short
	//   of the tip.
	// - Arena_04 route points 284-302 to InventorySpot42: the outbound pass
	//   climbs, the return runs level 8 uu aside, both ~35 uu above the walker.
	// - Battle2_04's zig-zag to PlayerStart16 (a 215.6 s stall).
	// (label, route, tip, the route point before the tip (the tip itself when
	// the route runs on past it), facing)
	const TTuple<const TCHAR*, TArray<FVector>, FVector2D, FVector2D, FRotator> Routes[] = {
		MakeTuple(TEXT("Arena_03 WotPathNode8"), TArray<FVector>{
			FVector(0.0, 0.0, 2.0), FVector(-12.1, -38.4, 1.9), FVector(-24.1, -76.8, 1.9),
			FVector(-57.2, -101.3, 1.9), FVector(-90.2, -125.8, 1.9),
			FVector(-123.2, -150.3, 1.9), FVector(-156.3, -174.8, 1.9),
			FVector(-172.1, -213.4, 2.5), FVector(-187.9, -252.1, 3.1),
			FVector(-203.7, -290.8, 3.7), FVector(-219.5, -329.5, 4.3),
			FVector(-235.3, -368.2, 4.9), FVector(-235.5, -366.1, -8.1),
			FVector(-235.3, -368.2, 4.9), FVector(-219.5, -329.5, 4.3),
			FVector(-203.7, -290.8, 3.7), FVector(-187.9, -252.1, 3.1),
			FVector(-172.1, -213.4, 2.5), FVector(-156.3, -174.8, 1.9),
			FVector(-190.2, -155.8, 1.9), FVector(-224.2, -136.9, 1.9),
			FVector(-258.2, -117.9, 1.9)},
			FVector2D(-235.3, -368.2), FVector2D(-219.5, -329.5), FRotator(0.0, -110.0, 0.0)),
		MakeTuple(TEXT("Arena_04 InventorySpot42"), TArray<FVector>{
			FVector(0.0, 0.0, 41.0), FVector(-0.9, -40.3, 34.5), FVector(-1.9, -80.7, 28.0),
			FVector(-2.8, -121.0, 21.6), FVector(-3.7, -161.4, 15.1),
			FVector(-4.6, -201.7, 8.6), FVector(38.0, -210.0, 12.9),
			FVector(80.7, -218.4, 17.3), FVector(123.3, -226.7, 21.6),
			FVector(166.0, -235.0, 26.0), FVector(208.6, -243.3, 30.3),
			FVector(251.2, -251.7, 34.7), FVector(204.5, -252.2, 34.7),
			FVector(157.7, -252.7, 34.7), FVector(110.9, -253.2, 34.7),
			FVector(64.2, -253.7, 34.7), FVector(17.4, -254.2, 34.7),
			FVector(-29.4, -254.7, 34.7), FVector(-76.2, -255.2, 34.7)},
			FVector2D(251.2, -251.7), FVector2D(208.6, -243.3), FRotator(0.0, -90.0, 0.0)),
		// Battle2_04 route points 154-171: a 20 uu zig-zag back to
		// PlayerStart16, 26 uu below its PathNodes, then down a return line
		// 15 uu aside.
		MakeTuple(TEXT("Battle2_04 PlayerStart16"), TArray<FVector>{
			FVector(0.0, 0.0, 17.9), FVector(-15.2, 42.6, 17.9), FVector(-30.3, 85.3, 17.9),
			FVector(-45.5, 127.9, 17.9), FVector(-60.7, 170.6, 17.9),
			FVector(-75.8, 213.2, 17.9), FVector(-91.0, 255.9, 17.9),
			FVector(-106.2, 298.5, 18.0), FVector(-100.8, 278.4, -8.1),
			FVector(-106.2, 298.5, 18.0), FVector(-106.6, 251.6, 17.8),
			FVector(-107.1, 204.7, 17.7), FVector(-107.5, 157.8, 17.6),
			FVector(-108.0, 110.9, 17.4), FVector(-108.4, 63.9, 17.3),
			FVector(-108.9, 17.0, 17.2), FVector(-109.3, -29.9, 17.1),
			FVector(-109.8, -76.8, 17.0)},
			// (Not a dead end: the route runs on past PlayerStart16, so no
			// overshoot direction.)
			FVector2D(-100.8, 278.4), FVector2D(-100.8, 278.4), FRotator(0.0, 110.0, 0.0)),
		// Battle3_02 route points 130-146: up the tower stair, along the narrow
		// curved walkway and into its 24 uu dead end at PathNode140, where
		// an earlier walker ran past the tip and fell ~260 uu.
		MakeTuple(TEXT("Battle3_02 PathNode140"), TArray<FVector>{
			FVector(0.0, 0.0, 0.0), FVector(7.4, 32.0, 20.4), FVector(14.8, 63.9, 40.8),
			FVector(-24.6, 47.2, 37.8), FVector(-63.9, 30.5, 34.7),
			FVector(-103.2, 13.8, 31.7), FVector(-142.5, -2.8, 28.6),
			FVector(-181.9, -19.5, 25.6), FVector(-221.2, -36.2, 22.5),
			FVector(-233.9, -15.6, 27.5), FVector(-246.7, 5.1, 32.5),
			FVector(-233.9, -15.6, 27.5), FVector(-221.2, -36.2, 22.5),
			FVector(-260.8, -59.1, 23.6), FVector(-300.4, -82.1, 24.7),
			FVector(-340.0, -105.0, 25.8), FVector(-379.6, -127.9, 27.0)},
			FVector2D(-246.7, 5.1), FVector2D(-233.9, -15.6), FRotator(0.0, 75.0, 0.0)),
	};
	for (const auto& [RouteLabel, RoutePoints, RouteTip, RouteBeforeTip, RouteFacing] : Routes)
	{
		const FString Label(RouteLabel);
		UWorld* World = WoTTest::CreateSpecWorld();
		SpawnHarnessFloor(World);
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 48), RouteFacing, AlwaysSpawnParams());
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->SetAsLocalPlayerController();
		PC->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		WoTTest::TickWorld(World, 0.2f);
		const float StandZ = Pawn->GetActorLocation().Z;

		FWoTNetTestChannel Channel;
		auto Dispatch = [&](const TSharedRef<FJsonObject>& Request)
		{
			const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			TestTrue(Label + TEXT(" command accepted"),
				Channel.DispatchCommandForTesting(World, Request, Response));
			return Response;
		};
		auto Request = [](const TCHAR* Command)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("cmd"), Command);
			return Object;
		};
		auto ProbeCount = [&](const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject> Data = Dispatch(Request(TEXT("probe_results")))
				->GetObjectField(TEXT("data"));
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			return Data.IsValid() && Data->TryGetArrayField(Field, Values)
				&& Values ? Values->Num() : -1;
		};
		Dispatch(Request(TEXT("probe_reset")));

		TSharedRef<FJsonObject> MoveTo = Request(TEXT("moveto"));
		MoveTo->SetNumberField(TEXT("snap"), 256.0);
		MoveTo->SetNumberField(TEXT("lead"), 40.0);
		MoveTo->SetNumberField(TEXT("tol"), 48.0);
		MoveTo->SetBoolField(TEXT("smooth"), true);
		MoveTo->SetBoolField(TEXT("jump_when_blocked"), true);
		TArray<TSharedPtr<FJsonValue>> Path;
		for (const FVector& Point : RoutePoints)
		{
			Path.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueNumber>(Point.X), MakeShared<FJsonValueNumber>(Point.Y),
				MakeShared<FJsonValueNumber>(StandZ + Point.Z)}));
		}
		MoveTo->SetArrayField(TEXT("path"), Path);
		Dispatch(MoveTo);

		double Nearest = TNumericLimits<double>::Max();
		// How far past the tip the walker runs: a dead end is often a ledge.
		const FVector2D IntoTip = (RouteTip - RouteBeforeTip).GetSafeNormal();
		double Overshoot = -TNumericLimits<double>::Max();
		bool bDone = false;
		for (int32 Tick = 0; Tick < 360 && !bDone; ++Tick)
		{
			Channel.TickDirector(World);
			WoTTest::TickWorld(World, 1);
			const FVector2D At(Pawn->GetActorLocation());
			Nearest = FMath::Min(Nearest, FVector2D::Distance(At, RouteTip));
			Overshoot = FMath::Max(Overshoot, FVector2D::DotProduct(At - RouteTip, IntoTip));
			bDone = Dispatch(Request(TEXT("moveto_status")))->GetBoolField(TEXT("done"));
		}
		AddInfo(FString::Printf(TEXT("%s: nearest the tip %.1f uu, past it %.1f uu"),
			*Label, Nearest, Overshoot));
		TestTrue(Label + TEXT(": the walker reaches the tip's neighbourhood"), Nearest < 40.0);
		TestTrue(Label + TEXT(": and never runs past the tip"), Overshoot < 8.0);
		TestTrue(Label + TEXT(": and walks the route back out to its end"), bDone);
		TestEqual(Label + TEXT(": without a blocked-walker jump"),
			ProbeCount(TEXT("assisted_jumps")), 0);
		TestEqual(Label + TEXT(": or a re-station"), ProbeCount(TEXT("route_snaps")), 0);
		WoTTest::DestroySpecWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelMoveToPaceSpec,
	"WoT.Net.Channel.MoveToPace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelMoveToPaceSpec::RunTest(const FString&)
{
	// A gate leg's chapters share its time in proportion to distance, so a
	// walker given a duration keeps to it instead of arriving early and
	// standing still; without one it walks as fast as it goes.
	auto Walk = [this](const TArray<FVector2D>& Route, double Duration,
		double& OutHalfwayX, double& OutDoneSeconds, int32& OutJumps,
		FVector2D Probe = FVector2D::ZeroVector, double* OutProbeNearest = nullptr)
	{
		UWorld* World = WoTTest::CreateSpecWorld();
		SpawnHarnessFloor(World);
		AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0, 0, 48), FRotator::ZeroRotator, AlwaysSpawnParams());
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
		PC->SetAsLocalPlayerController();
		PC->Possess(Pawn);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		WoTTest::TickWorld(World, 0.2f);
		const float StandZ = Pawn->GetActorLocation().Z;
		FWoTNetTestChannel Channel;
		auto Dispatch = [&](const TSharedRef<FJsonObject>& Request)
		{
			const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			TestTrue(TEXT("command accepted"),
				Channel.DispatchCommandForTesting(World, Request, Response));
			return Response;
		};
		auto Request = [](const TCHAR* Command)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("cmd"), Command);
			return Object;
		};
		Dispatch(Request(TEXT("probe_reset")));
		TSharedRef<FJsonObject> MoveTo = Request(TEXT("moveto"));
		MoveTo->SetNumberField(TEXT("snap"), 256.0);
		MoveTo->SetNumberField(TEXT("lead"), 40.0);
		MoveTo->SetNumberField(TEXT("tol"), 48.0);
		MoveTo->SetBoolField(TEXT("smooth"), true);
		MoveTo->SetBoolField(TEXT("jump_when_blocked"), true);
		if (Duration > 0.0) MoveTo->SetNumberField(TEXT("duration"), Duration);
		TArray<TSharedPtr<FJsonValue>> Path;
		for (const FVector2D& Point : Route)
		{
			Path.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueNumber>(Point.X), MakeShared<FJsonValueNumber>(Point.Y),
				MakeShared<FJsonValueNumber>(StandZ)}));
		}
		MoveTo->SetArrayField(TEXT("path"), Path);
		Dispatch(MoveTo);
		OutHalfwayX = 0.0;
		OutDoneSeconds = -1.0;
		for (int32 Tick = 1; Tick <= 600 && OutDoneSeconds < 0.0; ++Tick)
		{
			Channel.TickDirector(World);
			WoTTest::TickWorld(World, 1);
			if (Tick == 90) OutHalfwayX = Pawn->GetActorLocation().X;
			if (OutProbeNearest)
			{
				*OutProbeNearest = FMath::Min(*OutProbeNearest,
					FVector2D::Distance(FVector2D(Pawn->GetActorLocation()), Probe));
			}
			if (Dispatch(Request(TEXT("moveto_status")))->GetBoolField(TEXT("done")))
				OutDoneSeconds = Tick / 60.0;
		}
		const TSharedPtr<FJsonObject> Data = Dispatch(Request(TEXT("probe_results")))
			->GetObjectField(TEXT("data"));
		const TArray<TSharedPtr<FJsonValue>>* Jumps = nullptr;
		OutJumps = Data.IsValid() && Data->TryGetArrayField(TEXT("assisted_jumps"), Jumps)
			&& Jumps ? Jumps->Num() : -1;
		WoTTest::DestroySpecWorld(World);
	};
	const TArray<FVector2D> Straight = {FVector2D(0, 0), FVector2D(400, 0),
		FVector2D(800, 0), FVector2D(1200, 0)};
	double FreeX = 0.0, FreeDone = 0.0, PacedX = 0.0, PacedDone = 0.0;
	int32 FreeJumps = 0, PacedJumps = 0;
	Walk(Straight, 0.0, FreeX, FreeDone, FreeJumps);
	Walk(Straight, 6.0, PacedX, PacedDone, PacedJumps);
	// Arena_09's short leg (InventorySpot28 -> 26 -> 28 -> 29 -> 27) starts on
	// a 71 uu spur back through its own start, allotted 1.24 s. The walk must
	// begin at its start and walk the spur: an earlier walk began it on the return
	// pass, skipped the spur, and crawled 110 of its 290 uu.
	const TArray<FVector2D> Spur = {FVector2D(0, 0), FVector2D(-0.3, 35.7),
		FVector2D(-0.5, 71.3), FVector2D(-0.3, 35.7), FVector2D(0, 0),
		FVector2D(-23.3, -31.5), FVector2D(-46.6, -63.0), FVector2D(-81.3, -62.8),
		FVector2D(-115.9, -62.6)};
	double SpurX = 0.0, SpurDone = 0.0, SpurTipNearest = TNumericLimits<double>::Max();
	int32 SpurJumps = 0;
	Walk(Spur, 1.24, SpurX, SpurDone, SpurJumps, FVector2D(-0.5, 71.3), &SpurTipNearest);
	AddInfo(FString::Printf(TEXT("short spur leg: done %.2f s of 1.24 s, tip within %.0f uu"),
		SpurDone, SpurTipNearest));
	TestTrue(TEXT("a leg starting on a spur walks the spur"), SpurTipNearest < 40.0);
	TestTrue(TEXT("and keeps to its time"), SpurDone > 0.5 && SpurDone < 1.24 + 0.6);
	TestEqual(TEXT("without a blocked-walker jump"), SpurJumps, 0);
	AddInfo(FString::Printf(TEXT("free: x %.0f at 1.5 s, done %.2f s; paced: x %.0f, done %.2f s"),
		FreeX, FreeDone, PacedX, PacedDone));
	TestTrue(TEXT("without a duration the walk runs at full speed"),
		FreeDone > 0.0 && FreeDone < 4.0);
	TestTrue(TEXT("a 6 s walk is not ahead of its schedule at 1.5 s"),
		PacedX < 450.0);
	TestTrue(TEXT("and arrives about on time, not seconds early"),
		PacedDone > 5.0 && PacedDone < 6.8);
	TestEqual(TEXT("an eased walker is never taken for blocked"), PacedJumps, 0);
	TestEqual(TEXT("nor is a free one"), FreeJumps, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelQuietDamageSpec,
	"WoT.Net.Channel.QuietDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelQuietDamageSpec::RunTest(const FString&)
{
	// The weave reel stages Heal by taking a survival-pool walker to 50 Health.
	// Ordinary damage shakes the view by the damage dealt (retail TakeDamage);
	// quiet damage only lowers Health.
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	FWoTNetTestChannel Channel;
	auto Damage = [&](double Amount, bool bQuiet)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("damage"));
		Request->SetStringField(TEXT("pawn"), Pawn->GetName());
		Request->SetNumberField(TEXT("amount"), Amount);
		if (bQuiet) Request->SetBoolField(TEXT("quiet"), true);
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		return Channel.DispatchCommandForTesting(World, Request, Response);
	};
	Pawn->Health = 100000;
	const int32 ShakesBefore = PC->GetShakeRequestCountForTest();
	TestTrue(TEXT("quiet damage is accepted"), Damage(99950.0, true));
	TestEqual(TEXT("quiet damage lowers Health"), Pawn->Health, 50);
	TestEqual(TEXT("without shaking the view"), PC->GetShakeRequestCountForTest(), ShakesBefore);
	TestTrue(TEXT("ordinary damage is accepted"), Damage(10.0, false));
	TestTrue(TEXT("ordinary damage shakes the view"),
		PC->GetShakeRequestCountForTest() > ShakesBefore);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelInvulnerableSpec,
	"WoT.Net.Channel.Invulnerable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelInvulnerableSpec::RunTest(const FString&)
{
	// The gate walker survives its tour; UE's god flag stops every damage
	// entry, and clearing it restores ordinary damage.
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	FWoTNetTestChannel Channel;
	auto Invulnerable = [&](bool bOn)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("invulnerable"));
		Request->SetBoolField(TEXT("on"), bOn);
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		return Channel.DispatchCommandForTesting(World, Request, Response);
	};
	const int32 Before = Pawn->Health;
	TestTrue(TEXT("invulnerable is accepted"), Invulnerable(true));
	TestFalse(TEXT("the walker can no longer be damaged"), Pawn->CanBeDamaged());
	Pawn->TakeDamage(40.0f, FDamageEvent(), nullptr, nullptr);
	Pawn->TakeRawDamage(40, TEXT("Balefire"), nullptr, Pawn->GetActorLocation(),
		FVector::ZeroVector);
	TestEqual(TEXT("no damage reaches an invulnerable walker"), Pawn->Health, Before);
	TestTrue(TEXT("invulnerable can be cleared"), Invulnerable(false));
	Pawn->TakeDamage(40.0f, FDamageEvent(), nullptr, nullptr);
	TestTrue(TEXT("ordinary damage applies again"), Pawn->Health < Before);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelMoveToCornerEaseSpec,
	"WoT.Net.Channel.MoveToCornerEase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelMoveToCornerEaseSpec::RunTest(const FString&)
{
	// A route walk eases off into a sharp turn (Battle2_03's narrow rooftop
	// ramp: the walker swung wide at full speed and fell off) and keeps full
	// speed on a straight.
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 48), FRotator::ZeroRotator, AlwaysSpawnParams());
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	WoTTest::TickWorld(World, 0.2f);
	const float StandZ = Pawn->GetActorLocation().Z;
	FWoTNetTestChannel Channel;
	auto Place = [&](double X, double Y)
	{
		Pawn->SetActorLocation(FVector(X, Y, StandZ), false, nullptr,
			ETeleportType::TeleportPhysics);
	};
	auto InputAt = [&](double X, double Y)
	{
		Place(X, Y);
		Pawn->ConsumeMovementInputVector();
		Channel.TickDirector(World);
		return Pawn->ConsumeMovementInputVector().Size();
	};
	TSharedRef<FJsonObject> MoveTo = MakeShared<FJsonObject>();
	MoveTo->SetStringField(TEXT("cmd"), TEXT("moveto"));
	MoveTo->SetNumberField(TEXT("snap"), 256.0);
	MoveTo->SetNumberField(TEXT("lead"), 40.0);
	MoveTo->SetBoolField(TEXT("smooth"), true);
	TArray<TSharedPtr<FJsonValue>> Path;
	// East 500 uu, then a right-angle turn north.
	for (const FVector2D& Point : {FVector2D(0, 0), FVector2D(250, 0), FVector2D(500, 0),
		FVector2D(500, 250), FVector2D(500, 500)})
	{
		Path.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(Point.X), MakeShared<FJsonValueNumber>(Point.Y),
			MakeShared<FJsonValueNumber>(StandZ)}));
	}
	MoveTo->SetArrayField(TEXT("path"), Path);
	Place(0, 0);
	const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	TestTrue(TEXT("moveto accepted"),
		Channel.DispatchCommandForTesting(World, MoveTo, Response));
	float Straight = 0.0f;
	for (double X = 40.0; X <= 120.0; X += 40.0) Straight = InputAt(X, 0);
	TestTrue(TEXT("full speed on a straight"), FMath::IsNearlyEqual(Straight, 1.0f, 0.01f));
	float Corner = 1.0f;
	for (double X = 160.0; X <= 480.0; X += 40.0) Corner = InputAt(X, 0);
	TestTrue(TEXT("eased off just before a right-angle turn"), Corner < 0.6f);
	TestTrue(TEXT("but never stopped"), Corner >= 0.39f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelTourPointRestoreGateToolingSpec,
	"WoT.Net.Channel.TourPointRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelTourPointRestoreGateToolingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnGateToolingStaticBox(World, FVector(0, 0, -10),
		FVector(1000, 1000, 10));
	const FVector PriorLocation(500, 0, 48);
	const FRotator PriorRotation(0, 37, 0);
	const FVector PriorVelocity(80, -15, 5);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		PriorLocation, PriorRotation, AlwaysSpawnParams());
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Pawn->GetCharacterMovement()->Velocity = PriorVelocity;

	FWoTNetTestChannel Channel;
	Channel.ForceNextTourPointPostCheckFailureForTesting();
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("cmd"), TEXT("tourpoint"));
	Request->SetStringField(TEXT("pawn"), Pawn->GetName());
	Request->SetNumberField(TEXT("x"), 0.0);
	Request->SetNumberField(TEXT("y"), 0.0);
	Request->SetNumberField(TEXT("z"), 0.0);
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	TestFalse(TEXT("a failed post-check keeps reporting the tour point error"),
		Channel.DispatchCommandForTesting(World, Request, Response));
	TestEqual(TEXT("a failed post-check reports the station error"),
		Response->GetStringField(TEXT("err")),
		FString(TEXT("no safe walkable station near retail coverage point")));
	TestTrue(TEXT("a failed post-check restores the pawn location"),
		Pawn->GetActorLocation().Equals(PriorLocation, 0.1f));
	TestTrue(TEXT("a failed post-check restores the pawn rotation"),
		Pawn->GetActorRotation().Equals(PriorRotation, 0.1f));
	TestTrue(TEXT("a failed post-check restores the pawn velocity"),
		Pawn->GetCharacterMovement()->Velocity.Equals(PriorVelocity, 0.1f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelNearestFloorGateToolingSpec,
	"WoT.Net.Channel.NearestFloorBelow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelNearestFloorGateToolingSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnGateToolingStaticBox(World, FVector(0, 0, -15),
		FVector(500, 500, 5));
	SpawnGateToolingStaticBox(World, FVector(0, 0, 95),
		FVector(500, 500, 5));
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(750, 0, 48), FRotator::ZeroRotator, AlwaysSpawnParams());

	FWoTNetTestChannel Channel;
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("cmd"), TEXT("tourpoint"));
	Request->SetStringField(TEXT("pawn"), Pawn->GetName());
	Request->SetNumberField(TEXT("x"), 0.0);
	Request->SetNumberField(TEXT("y"), 0.0);
	Request->SetNumberField(TEXT("z"), 0.0);
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	TestTrue(TEXT("tour point accepts the nearest lower floor"),
		Channel.DispatchCommandForTesting(World, Request, Response));
	TestTrue(TEXT("tour point lands on node minus ten, not node plus one hundred"),
		FMath::IsNearlyEqual(Pawn->GetActorLocation().Z, 38.0f, 1.0f));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelAssistedJumpGateToolingSpec,
	"WoT.Net.Channel.AssistedJump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelAssistedJumpGateToolingSpec::RunTest(const FString&)
{
	const FGateToolingAssistedJumpOutcome Assisted =
		RunGateToolingAssistedJumpScenario(true);
	TestTrue(TEXT("jump-assisted moveto is accepted"), Assisted.bCommandAccepted);
	TestTrue(TEXT("jump-assisted moveto reaches the top of the 40 uu step"),
		Assisted.PawnZ > 75.0f);
	TestTrue(TEXT("jump-assisted moveto records at least one assisted jump"),
		Assisted.AssistedJumpCount >= 1);
	// The 40 uu step's face is what blocked it: a wall facing back along -X,
	// hit below the step's 40 uu top.
	TestTrue(TEXT("the jump records its blocker"), Assisted.bBlockerRecorded);
	TestTrue(TEXT("the blocker is the step's face"), Assisted.BlockerNormalX < -0.9);
	TestTrue(TEXT("hit below the step's top"),
		Assisted.BlockerHeight > 0.0 && Assisted.BlockerHeight <= 40.5);
	TestEqual(TEXT("probe reset clears assisted jumps"),
		Assisted.AssistedJumpCountAfterReset, 0);

	const FGateToolingAssistedJumpOutcome Unassisted =
		RunGateToolingAssistedJumpScenario(false);
	TestTrue(TEXT("default moveto is accepted"), Unassisted.bCommandAccepted);
	TestTrue(TEXT("default moveto stays below the 40 uu step"),
		Unassisted.PawnZ < 70.0f);
	TestEqual(TEXT("default moveto records no assisted jumps"),
		Unassisted.AssistedJumpCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelCaptureEverySpec,
	"WoT.Net.Channel.CaptureEvery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelCaptureEverySpec::RunTest(const FString&)
{
	IConsoleVariable* DumpingMovie =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.DumpingMovie"));
	if (!TestNotNull(TEXT("r.DumpingMovie exists"), DumpingMovie)) return false;
	UWorld* World = WoTTest::CreateSpecWorld();
	FWoTNetTestChannel Channel;
	auto Capture = [&](bool bOn, double Every)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("capture"));
		Request->SetBoolField(TEXT("on"), bOn);
		if (Every > 0.0) Request->SetNumberField(TEXT("every"), Every);
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		return Channel.DispatchCommandForTesting(World, Request, Response);
	};
	const uint64 SavedFrameCounter = GFrameCounter;
	TArray<int32> Armed;

	TestTrue(TEXT("capture every 2 is accepted"), Capture(true, 2.0));
	Armed.Add(DumpingMovie->GetInt());
	for (int32 Frame = 1; Frame <= 4; ++Frame)
	{
		++GFrameCounter;
		Channel.TickDirector(World);
		Armed.Add(DumpingMovie->GetInt());
	}
	TestTrue(FString::Printf(
		TEXT("every 2 arms the start frame and each second frame after (got %s)"),
		*FString::JoinBy(Armed, TEXT(","), [](int32 Value) { return FString::FromInt(Value); })),
		Armed == TArray<int32>{1, 0, 1, 0, 1});
	TestTrue(TEXT("capture off is accepted"), Capture(false, 0.0));
	++GFrameCounter;
	Channel.TickDirector(World);
	TestEqual(TEXT("capture off stays off"), DumpingMovie->GetInt(), 0);

	TestTrue(TEXT("plain capture is accepted"), Capture(true, 0.0));
	++GFrameCounter;
	Channel.TickDirector(World);
	TestEqual(TEXT("plain capture dumps every frame"), DumpingMovie->GetInt(), -1);
	TestFalse(TEXT("a fractional every is rejected"), Capture(true, 1.5));
	TestFalse(TEXT("every below 1 is rejected"), Capture(true, 0.5));
	TestEqual(TEXT("a rejected capture leaves dumping off"), DumpingMovie->GetInt(), 0);

	DumpingMovie->Set(0, ECVF_SetByCode);
	GFrameCounter = SavedFrameCounter;
	Channel.Stop();
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelBattlePlaySpec,
	"WoT.Net.Channel.BattlePlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelBattlePlaySpec::RunTest(const FString&)
{
	auto BattlePlay = [](FWoTNetTestChannel& Channel, UWorld* World,
		const TArray<double>& Teams)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("battle_play"));
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const double Team : Teams) Values.Add(MakeShared<FJsonValueNumber>(Team));
		Request->SetArrayField(TEXT("teams"), Values);
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		return Channel.DispatchCommandForTesting(World, Request, Response);
	};

	UWorld* ArenaWorld = WoTTest::CreateSpecWorld();
	{
		FWoTNetTestChannel Channel;
		TestFalse(TEXT("battle_play is refused outside a Battle game"),
			BattlePlay(Channel, ArenaWorld, {0.0}));
		Channel.Stop();
	}
	WoTTest::DestroySpecWorld(ArenaWorld);

	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	AWoTPlayerController* Controller =
		World->SpawnActor<AWoTPlayerController>(AlwaysSpawnParams());
	if (Controller) Controller->InitPlayerState();
	AWoTPlayerState* PlayerState = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("battle mode"), Mode) || !TestNotNull(TEXT("battle info"), Battle)
		|| !TestNotNull(TEXT("p0 player state"), PlayerState))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
	Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	Mode->Battle = Battle;
	Mode->ResetBattle();
	PlayerState->Team = 255;
	TestTrue(TEXT("the edit phase starts with teleporters disabled"),
		Battle->bTeleportersDisabled);

	FWoTNetTestChannel Channel;
	TestFalse(TEXT("an empty team list is refused"), BattlePlay(Channel, World, {}));
	TestFalse(TEXT("a fractional team is refused"), BattlePlay(Channel, World, {0.5}));
	TestFalse(TEXT("a team the map lacks is refused"), BattlePlay(Channel, World, {2.0}));
	TestEqual(TEXT("a refused team leaves p0 neutral"), (int32)PlayerState->Team, 255);
	TestTrue(TEXT("a refused battle_play keeps the edit phase"),
		Battle->bTeleportersDisabled);

	TestTrue(TEXT("battle_play puts p0 on team 1 and opens play"),
		BattlePlay(Channel, World, {1.0}));
	TestEqual(TEXT("p0 joined team 1"), (int32)PlayerState->Team, 1);
	TestFalse(TEXT("teleporters are on"), Battle->bTeleportersDisabled);
	TestFalse(TEXT("p0 is no longer frozen"), Controller->bTeleportingDisabled);
	TestTrue(TEXT("battle_play is idempotent"), BattlePlay(Channel, World, {1.0}));

	Channel.Stop();
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelSprayerByLocationSpec,
	"WoT.Net.Channel.SprayerByLocation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelSprayerByLocationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTParticleSprayer* Near = World->SpawnActor<AWoTParticleSprayer>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	AWoTParticleSprayer* Far = World->SpawnActor<AWoTParticleSprayer>(
		FVector(100, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	if (!TestNotNull(TEXT("first sprayer"), Near) || !TestNotNull(TEXT("second sprayer"), Far))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Near->bOn = false;
	Far->bOn = false;
	FWoTNetTestChannel Channel;
	FString Chosen;
	auto Activate = [&](const FVector& At, double Radius)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("sprayer_activate"));
		Request->SetNumberField(TEXT("x"), At.X);
		Request->SetNumberField(TEXT("y"), At.Y);
		Request->SetNumberField(TEXT("z"), At.Z);
		if (Radius != 0.0) Request->SetNumberField(TEXT("radius"), Radius);
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Chosen.Reset();
		const bool bAccepted = Channel.DispatchCommandForTesting(World, Request, Response);
		Response->TryGetStringField(TEXT("actor"), Chosen);
		return bAccepted;
	};

	TestTrue(TEXT("a point 10 uu from the second sprayer is accepted"),
		Activate(FVector(90, 0, 100), 0.0));
	TestEqual(TEXT("the nearest sprayer is chosen"), Chosen, Far->GetName());
	TestTrue(TEXT("the chosen sprayer is on"), Far->bOn);
	TestFalse(TEXT("the other sprayer stays off"), Near->bOn);
	TestFalse(TEXT("no sprayer within the default 64 uu is refused"),
		Activate(FVector(0, 0, 300), 0.0));
	TestFalse(TEXT("the refused point leaves the first sprayer off"), Near->bOn);
	TestTrue(TEXT("an explicit radius widens the search"),
		Activate(FVector(0, 0, 300), 256.0));
	TestEqual(TEXT("the widened search picks the nearest"), Chosen, Near->GetName());
	TestFalse(TEXT("a non-positive radius is refused"), Activate(FVector(0, 0, 100), -5.0));

	Channel.Stop();
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelFallWatchSpec,
	"WoT.Net.Channel.FallWatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelFallWatchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	SpawnHarnessFloor(World);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 200), FRotator::ZeroRotator, AlwaysSpawnParams());
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	FWoTNetTestChannel Channel;
	auto Dispatch = [&](const TSharedRef<FJsonObject>& Request)
	{
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		TestTrue(TEXT("command accepted"),
			Channel.DispatchCommandForTesting(World, Request, Response));
		return Response;
	};
	auto SetFallWatch = [&](double MinZ, double Seconds)
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("fallwatch"));
		Request->SetBoolField(TEXT("on"), true);
		Request->SetNumberField(TEXT("min_z"), MinZ);
		Request->SetNumberField(TEXT("seconds"), Seconds);
		Dispatch(Request);
	};
	auto FallResults = [&]()
	{
		TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("cmd"), TEXT("probe_results"));
		return Dispatch(Request)->GetObjectField(TEXT("data"));
	};

	TSharedRef<FJsonObject> Reset = MakeShared<FJsonObject>();
	Reset->SetStringField(TEXT("cmd"), TEXT("probe_reset"));
	Dispatch(Reset);
	SetFallWatch(0.0, 10.0);
	Pawn->SetActorLocation(FVector(0, 0, -100), false, nullptr,
		ETeleportType::TeleportPhysics);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	Channel.TickDirector(World);
	Pawn->SetActorLocation(FVector(0, 0, 200), false, nullptr,
		ETeleportType::TeleportPhysics);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Channel.TickDirector(World);

	SetFallWatch(-10000.0, 0.0);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	Channel.TickDirector(World);
	WoTTest::TickWorld(World, 1);
	Channel.TickDirector(World);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Channel.TickDirector(World);
	const TArray<TSharedPtr<FJsonValue>>* Falls = nullptr;
	TSharedPtr<FJsonObject> Data = FallResults();
	TestTrue(TEXT("below-min and long-fall episodes are both recorded"),
		Data->TryGetArrayField(TEXT("falls"), Falls) && Falls->Num() == 2);
	if (Falls && Falls->Num() == 2)
	{
		TestTrue(TEXT("first event is below-min-z"),
			(*Falls)[0]->AsObject()->GetBoolField(TEXT("below_min_z")));
		TestFalse(TEXT("second event is a long fall warning"),
			(*Falls)[1]->AsObject()->GetBoolField(TEXT("below_min_z")));
	}

	SetFallWatch(-10000.0, 10.0);
	Pawn->SetActorLocation(FVector(500, 0, 200), false, nullptr,
		ETeleportType::TeleportPhysics);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	Channel.TickDirector(World);
	TSharedRef<FJsonObject> Tour = MakeShared<FJsonObject>();
	Tour->SetStringField(TEXT("cmd"), TEXT("tourpoint"));
	Tour->SetStringField(TEXT("pawn"), Pawn->GetName());
	Tour->SetNumberField(TEXT("x"), 500.0);
	Tour->SetNumberField(TEXT("y"), 0.0);
	Tour->SetNumberField(TEXT("z"), 100.0);
	Dispatch(Tour);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Channel.TickDirector(World);
	Falls = nullptr;
	Data = FallResults();
	TestTrue(TEXT("tourpoint closes an unreported episode without recording it"),
		Data->TryGetArrayField(TEXT("falls"), Falls) && Falls->Num() == 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetChannelResetStateSpec,
	"WoT.Net.Channel.ResetStateLeftovers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetChannelResetStateSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 200), FRotator::ZeroRotator, AlwaysSpawnParams());
	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	auto Spawn = [&](UClass* Class, double X)
	{
		return World->SpawnActor<AActor>(Class, FVector(X, 0, 200),
			FRotator::ZeroRotator, AlwaysSpawnParams());
	};
	AActor* AmaMine = Spawn(AWoTAngrealAMAProjectile::StaticClass(), 100);
	AActor* AmaBubble = Spawn(AWoTAMAPearl::StaticClass(), 200);
	AActor* Ward = Spawn(AWoTExpWardProjectile::StaticClass(), 300);
	AWoTDistantEyeProjectile* Eye = Cast<AWoTDistantEyeProjectile>(
		Spawn(AWoTDistantEyeProjectile::StaticClass(), 400));
	AActor* Illusion = Spawn(AWoTAngrealIllusionProjectile::StaticClass(), 500);
	AWoTLightGlobe* Light = Cast<AWoTLightGlobe>(
		Spawn(AWoTLightGlobe::StaticClass(), 600));
	AActor* TremorProjectile = Spawn(AWoTEarthTremorProjectile::StaticClass(), 700);
	AActor* TremorRing = Spawn(AWoTEarthTremor::StaticClass(), 800);
	AActor* TremorRock = Spawn(AWoTEarthTremorRock::StaticClass(), 900);
	AActor* TremorChunk = Spawn(AWoTEarthTremorChunk::StaticClass(), 1000);
	AActor* TremorSound = Spawn(AWoTETSoundProxy::StaticClass(), 1100);
	if (Light) Light->Follow(Pawn);
	if (Eye) PC->SetViewTarget(Eye);
	UWoTIceLeech* Ice = NewObject<UWoTIceLeech>(Pawn);
	Ice->AttachTo(Pawn);
	Pawn->GetCharacterMovement()->Velocity = FVector(300, 20, 100);

	FWoTNetTestChannel Channel;
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("cmd"), TEXT("resetstate"));
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	TestTrue(TEXT("resetstate accepted"),
		Channel.DispatchCommandForTesting(World, Request, Response));
	const TArray<AActor*> Leftovers = {AmaMine, AmaBubble, Ward, Eye, Illusion,
		Light, TremorProjectile, TremorRing, TremorRock, TremorChunk, TremorSound};
	for (AActor* Leftover : Leftovers)
	{
		TestFalse(FString::Printf(TEXT("%s was cleared"), *GetNameSafe(Leftover)),
			IsValid(Leftover));
	}
	TestFalse(TEXT("ice freeze pin was unattached"), IsValid(Ice));
	TestTrue(TEXT("every player pawn velocity is zeroed"),
		Pawn->GetVelocity().IsNearlyZero());
	TestEqual(TEXT("distant-eye remote view is ended"), PC->GetViewTarget(),
		static_cast<AActor*>(Pawn));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The zone current is applied by UWoTCharacterMovement's Phys* transport —
// the PREDICTED path — so an owned pawn in a current computes the same drift on client
// and server. This single-process spec is the gate: the transport hook is role-agnostic
// (it runs identically in the client's replayed saved moves and the server's authority),
// so proving the CMC applies the current HERE proves the client predicts it (matching
// the authority) by construction. The volume no longer writes the current in its Tick
// (relocated to the CMC), and the CMC reads the zone via ContainsPoint (math, not the
// volume's physics-overlap poll), so this is deterministic and floor-free.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetZonePredictSpec,
	"WoT.Net.ZonePredict", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetZonePredictSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn) { WoTTest::DestroySpecWorld(World); return false; }

	// The pawn's movement component is our subclass (installed in the AWoTCharacter ctor).
	TestTrue(TEXT("CMC is UWoTCharacterMovement"),
		Cast<UWoTCharacterMovement>(Pawn->GetCharacterMovement()) != nullptr);

	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	// No floor: fly so the current makes clean horizontal drift (no ground friction or
	// floor to confound), and hold NO input — the ONLY horizontal force is the zone.
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);

	// A +X current zone enclosing the pawn (a wide box so it stays inside as it drifts).
	AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("zone spawned"), Zone);
	if (Zone)
	{
		Zone->ZoneVelocity = FVector(600, 0, 0);
		if (Zone->Collision) { Zone->Collision->SetBoxExtent(FVector(1000.0f)); }
	}

	const FVector Start = Pawn->GetActorLocation();
	WoTTest::TickWorld(World, 30);   // 0.5s
	const float DriftX = Pawn->GetActorLocation().X - Start.X;

	// GREEN: the CMC folds the current into CalcVelocity -> the pawn drifts +X. RED (stub
	// CalcVelocity): the current is applied nowhere (the volume's write is gone) -> ~0.
	TestTrue(
		FString::Printf(TEXT("owned pawn drifts +X under the predicted zone current (drift=%.1f)"), DriftX),
		DriftX > 25.0f);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Hardening: the player Server RPCs carry WithValidation. The only one with
// a parameter is ServerUseAngreal(FRotator) — the RPC framework calls _Validate on the
// authority and disconnects a client that fails. Verify it rejects a non-finite aim (a
// compromised client) and accepts a finite one; the parameterless RPCs validate trivially.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetRpcValidationSpec,
	"WoT.Net.RpcValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetRpcValidationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn) { WoTTest::DestroySpecWorld(World); return false; }

	// A guaranteed quiet-NaN bit pattern (portable, unlike sqrt(-1) which a compiler may fold).
	float NaNf;
	const uint32 NaNBits = 0x7FC00000u;
	FMemory::Memcpy(&NaNf, &NaNBits, sizeof(float));

	TestTrue(TEXT("finite aim validates"), Pawn->ServerUseAngreal_Validate(FRotator(10.f, 20.f, 0.f)));
	TestFalse(TEXT("NaN aim is rejected"), Pawn->ServerUseAngreal_Validate(FRotator(NaNf, 0.f, 0.f)));
	TestTrue(TEXT("parameterless cast RPCs validate"),
		Pawn->ServerCeaseUsingAngreal_Validate() && Pawn->ServerNextInventory_Validate()
			&& Pawn->ServerPrevInventory_Validate());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The zone GROUND FRICTION swap and SWIM-MODE entry are folded into
// UWoTCharacterMovement's predicted path too. Friction + the swim enter/exit decision live in
// UpdateCharacterStateBeforeMovement, which runs IDENTICALLY on the authority and in the
// client's replayed saved moves; PhysSwimming is overridden so the base's self-exit (it forces
// MOVE_Falling because there is no real APhysicsVolume — GetPhysicsVolume()->bWaterVolume is
// false) cannot oscillate the mode. Like ZonePredict, this single-process spec is the gate: the
// hooks are role-agnostic, so proving the CMC applies the friction + swim HERE proves the client
// predicts them by construction. The volume no longer writes friction/swim in its Tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetZoneSwimFrictionSpec,
	"WoT.Net.ZoneSwimFriction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetZoneSwimFrictionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("pawn spawned"), Pawn);
	if (!Pawn) { WoTTest::DestroySpecWorld(World); return false; }

	UWoTCharacterMovement* Move = Cast<UWoTCharacterMovement>(Pawn->GetCharacterMovement());
	TestNotNull(TEXT("CMC is UWoTCharacterMovement"), Move);
	if (!Move) { WoTTest::DestroySpecWorld(World); return false; }

	APlayerController* PC = World->SpawnActor<APlayerController>();
	PC->SetAsLocalPlayerController();
	PC->Possess(Pawn);
	Move->SetMovementMode(MOVE_Falling);

	// Capture the pawn's OWN ground friction BEFORE any zone exists — the first movement update
	// captures the base (so a zone can't clobber it).
	WoTTest::TickWorld(World, 2);
	const float BaseFriction = Move->GroundFriction;

	// A water zone with a slippery custom friction (some retail water zones author 0.6),
	// wide enough to enclose the pawn.
	AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("zone spawned"), Zone);
	if (Zone)
	{
		Zone->bWaterZone = true;
		Zone->ZoneGroundFriction = 0.6f;
		if (Zone->Collision) { Zone->Collision->SetBoxExtent(FVector(1000.0f)); }
	}
	WoTTest::TickWorld(World, 5);

	// GREEN: the CMC's predicted path put the pawn in MOVE_Swimming and swapped in the zone's
	// ground friction. RED (server-only Tick writes removed, CMC hooks absent): neither happens.
	TestEqual(TEXT("owned pawn enters MOVE_Swimming in the water zone (predicted path)"),
		(int32)Move->MovementMode.GetValue(), (int32)MOVE_Swimming);
	TestEqual(TEXT("owned pawn takes the zone's ground friction (predicted path)"),
		Move->GroundFriction, 0.6f);

	// Leaving the zone reverts both — back to falling, back to the pawn's own friction.
	Pawn->SetActorLocation(FVector(50000, 0, 100));
	WoTTest::TickWorld(World, 5);
	TestEqual(TEXT("leaving the water drops back to MOVE_Falling"),
		(int32)Move->MovementMode.GetValue(), (int32)MOVE_Falling);
	TestEqual(TEXT("leaving restores the pawn's own ground friction"),
		Move->GroundFriction, BaseFriction);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Runtime-attached leeches replicate on 5.8's dynamic-subobject path — the SAME path the
// reflector was diagnostic-verified on (base-ctor SetIsReplicatedByDefault, NO
// AddReplicatedSubObject anywhere) — AND the AffectHost effect dispatch is authority-only. It
// used to run on ALL roles (a real client-side hazard for the gameplay leeches: DecreaseCharges
// mutated CurCharges and DestroyOnDestroy self-destructed the leech every client tick, since
// DestroyActor is not replicated). This single-process spec is the gate: Part 1 locks the
// replication contract (a regression guard for the cross-process result the live net harness proved and
// CI can't cheaply re-run); Part 2 proves the authority gate by forcing the host — hence the
// component's GetOwnerRole() — to a simulated proxy (the in-repo SetRole idiom).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetLeechReplicationSpec,
	"WoT.Net.LeechReplication", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetLeechReplicationSpec::RunTest(const FString&)
{
	// A replicated UPROPERTY carries CPF_Net (robust equivalent of decoding GetLifetimeReplicatedProps).
	auto PropReplicated = [](UClass* Cls, const TCHAR* Name) -> bool
	{
		const FProperty* P = FindFProperty<FProperty>(Cls, Name);
		return P && (P->PropertyFlags & CPF_Net) != 0;
	};

	// --- Part 1: replication registration (CDO/reflection; no world needed) ---
	// SetIsReplicatedByDefault(true) in the base ctor -> every leech replicates on the dynamic-
	// subobject path, identical to the verified reflector.
	TestTrue(TEXT("UWoTLeech CDO replicates by default"), GetDefault<UWoTLeech>()->GetIsReplicated());
	TestTrue(TEXT("UWoTIceLeech CDO replicates by default"), GetDefault<UWoTIceLeech>()->GetIsReplicated());
	TestTrue(TEXT("UWoTWhirlwindLeech CDO replicates by default"), GetDefault<UWoTWhirlwindLeech>()->GetIsReplicated());
	// Base Source* refs replicate (Leech.uc:94-99).
	TestTrue(TEXT("SourceAngreal replicated"), PropReplicated(UWoTLeech::StaticClass(), TEXT("SourceAngreal")));
	TestTrue(TEXT("SourceProjectile replicated"), PropReplicated(UWoTLeech::StaticClass(), TEXT("SourceProjectile")));
	TestTrue(TEXT("SourceProjClass replicated"), PropReplicated(UWoTLeech::StaticClass(), TEXT("SourceProjClass")));
	// Visual-leech state must reach clients (Ice pin origin, Whirlwind drag target).
	TestTrue(TEXT("Ice OriginalLocation replicated"), PropReplicated(UWoTIceLeech::StaticClass(), TEXT("OriginalLocation")));
	TestTrue(TEXT("Whirlwind DesiredLocation replicated"), PropReplicated(UWoTWhirlwindLeech::StaticClass(), TEXT("DesiredLocation")));
	// Parity mirror: the reflector Source* replicates on the same path (leech ≡ reflector).
	TestTrue(TEXT("Reflector SourceAngreal replicated"), PropReplicated(UWoTReflector::StaticClass(), TEXT("SourceAngreal")));

	// --- Part 2: AffectHost dispatch is authority-only ---
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("host spawned"), Host);
	if (!Host) { WoTTest::DestroySpecWorld(World); return false; }

	UWoTTestLeech* Leech = NewObject<UWoTTestLeech>(Host);
	Leech->AffectResolution = 0.05f;   // fires roughly every 3rd 1/60s tick
	Leech->RegisterComponent();        // attach to the authority host + start ticking

	WoTTest::TickWorld(World, 12);
	TestTrue(FString::Printf(TEXT("AffectHost runs on the authority (calls=%d)"), Leech->AffectHostCalls),
		Leech->AffectHostCalls > 0);

	// Force the host (hence the component's GetOwnerRole()) to a simulated proxy; the gated
	// dispatch must now be suppressed while timing keeps accumulating harmlessly.
	Host->SetRole(ROLE_SimulatedProxy);
	const int32 CallsAtSwitch = Leech->AffectHostCalls;
	WoTTest::TickWorld(World, 12);
	TestEqual(FString::Printf(TEXT("AffectHost suppressed on a simulated proxy (stayed %d)"), CallsAtSwitch),
		Leech->AffectHostCalls, CallsAtSwitch);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// Reflectors were already diagnostic-verified replicating to all 3 views and are already
// fully authority-split (no AffectHost-style all-roles path — every fold runs on the server
// damage/effect path). This spec is the regression guard: Part 1 locks the replication contract,
// Part 2 the authority-gated lifespan tick (WoTReflector.cpp:84 — same gate shape as the leech).
// No reflector production code change is needed (PORT_NOTES records the parity split).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetReflectorReplSpec,
	"WoT.Net.ReflectorRepl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetReflectorReplSpec::RunTest(const FString&)
{
	auto PropReplicated = [](UClass* Cls, const TCHAR* Name) -> bool
	{
		const FProperty* P = FindFProperty<FProperty>(Cls, Name);
		return P && (P->PropertyFlags & CPF_Net) != 0;
	};

	TestTrue(TEXT("UWoTReflector CDO replicates by default"), GetDefault<UWoTReflector>()->GetIsReplicated());
	TestTrue(TEXT("Reflector SourceAngreal replicated"),
		PropReplicated(UWoTReflector::StaticClass(), TEXT("SourceAngreal")));
	TestTrue(TEXT("Shield ShieldHitPoints replicated"),
		PropReplicated(UWoTShieldReflector::StaticClass(), TEXT("ShieldHitPoints")));

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTCharacter* Host = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("host spawned"), Host);
	if (!Host) { WoTTest::DestroySpecWorld(World); return false; }

	UWoTReturnToSenderReflector* Refl = NewObject<UWoTReturnToSenderReflector>(Host);
	Refl->Lifespan = 5.0f;
	Refl->RegisterComponent();

	WoTTest::TickWorld(World, 12);
	TestTrue(FString::Printf(TEXT("reflector lifespan decrements on the authority (%.2f < 5)"), Refl->Lifespan),
		Refl->Lifespan < 5.0f);

	Host->SetRole(ROLE_SimulatedProxy);
	const float AtSwitch = Refl->Lifespan;
	WoTTest::TickWorld(World, 12);
	TestEqual(TEXT("reflector lifespan frozen on a simulated proxy"), Refl->Lifespan, AtSwitch);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The trigger / dispatcher / counter event system is authority-scoped. Every touch/trigger/
// count path in WoTEvents.cpp ran on ALL roles — a server-spawned Trigger replicates as a simulated
// proxy, so each client simulated its own pawn overlaps and re-fired the whole event chain locally
// (flipping bSpent, decrementing Counters per-client — the world-state analog of the leech
// AffectHost all-roles bug). The shipped .uc trigger/dispatch/count functions are all non-simulated
// → server-only; effects reach clients via the replicated actors they drive (movers). Fix: four
// HasAuthority() guards (Counter/Dispatcher/Trigger-relay WoTTrigger + Trigger OnOverlap). This
// spec proves each source refuses off-authority via a terminal AWoTMover (itself kept on the
// authority, so it CAN open): the door opens when the source fires on the authority and stays shut
// when the source is a ROLE_SimulatedProxy (the in-repo SetRole idiom). RED before the guards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetEventAuthoritySpec,
	"WoT.Net.EventAuthority", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetEventAuthoritySpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A terminal mover registered under `Tag` that rises +Z100 over 0.3s once its WoTTrigger fires.
	// Deferred so KeyPos/TagName are set before BeginPlay caches BaseLoc + registers the tag.
	auto SpawnDoor = [&](const TCHAR* Tag) -> AWoTMover*
	{
		const FTransform X(FRotator::ZeroRotator, FVector(0, 0, 100));
		AWoTMover* M = World->SpawnActorDeferred<AWoTMover>(AWoTMover::StaticClass(), X);
		M->InitialState = EWoTMoverState::TriggerOpenTimed;   // opens on a fired event (not a bump)
		M->KeyPos = { FVector::ZeroVector, FVector(0, 0, 100) };
		M->KeyRot = { FRotator::ZeroRotator, FRotator::ZeroRotator };
		M->MoveTime = 0.3f;
		M->StayOpenTime = 30.0f;
		M->TagName = FName(Tag);
		M->FinishSpawning(X);
		return M;
	};
	auto Opened = [](AWoTMover* M, double BaseZ) { return M->GetActorLocation().Z > BaseZ + 50.0; };

	// Fire `Src` (already role-set + configured to fire event `Tag`) via `Fire`, tick past MoveTime,
	// and report whether the tagged door opened. Fresh door per call so cases don't cross-fire.
	auto DoorOpensWhen = [&](const TCHAR* Tag, TFunctionRef<void(AWoTMover*)> ConfigDoor,
	                         TFunctionRef<void()> Fire) -> bool
	{
		AWoTMover* Door = SpawnDoor(Tag);
		ConfigDoor(Door);
		const double BaseZ = Door->GetActorLocation().Z;
		Fire();
		WoTTest::TickWorld(World, 30);   // 0.5s > MoveTime
		return Opened(Door, BaseZ);
	};
	auto NoConfig = [](AWoTMover*) {};

	// A pawn to hand the Trigger's OnOverlap.
	APawn* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());

	// ---- Counter (NumToCount default 1 → BeginPlay sets Remaining=1 → first WoTTrigger fires) ----
	AWoTCounter* CAuth = World->SpawnActor<AWoTCounter>(AlwaysSpawnParams());
	CAuth->EventName = TEXT("cnt_auth");
	TestTrue(TEXT("counter fires on the authority → door opens"),
		DoorOpensWhen(TEXT("cnt_auth"), NoConfig, [&]{ CAuth->WoTTrigger(nullptr, nullptr); }));
	AWoTCounter* CProxy = World->SpawnActor<AWoTCounter>(AlwaysSpawnParams());
	CProxy->EventName = TEXT("cnt_proxy");
	CProxy->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("counter on a simulated proxy does not fire → door stays shut"),
		DoorOpensWhen(TEXT("cnt_proxy"), NoConfig, [&]{ CProxy->WoTTrigger(nullptr, nullptr); }));

	// ---- Dispatcher ----
	AWoTDispatcher* DAuth = World->SpawnActor<AWoTDispatcher>(AlwaysSpawnParams());
	DAuth->OutEvents = { FName(TEXT("dsp_auth")) }; DAuth->OutDelays = { 0.0f };
	TestTrue(TEXT("dispatcher fires on the authority → door opens"),
		DoorOpensWhen(TEXT("dsp_auth"), NoConfig, [&]{ DAuth->WoTTrigger(nullptr, nullptr); }));
	AWoTDispatcher* DProxy = World->SpawnActor<AWoTDispatcher>(AlwaysSpawnParams());
	DProxy->OutEvents = { FName(TEXT("dsp_proxy")) }; DProxy->OutDelays = { 0.0f };
	DProxy->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("dispatcher on a simulated proxy does not fire → door stays shut"),
		DoorOpensWhen(TEXT("dsp_proxy"), NoConfig, [&]{ DProxy->WoTTrigger(nullptr, nullptr); }));

	// ---- Trigger: relay (WoTTrigger) AND the spontaneous OnOverlap entry ----
	AWoTTrigger* TRelayA = World->SpawnActor<AWoTTrigger>(AlwaysSpawnParams());
	TRelayA->EventName = TEXT("trly_auth");
	TestTrue(TEXT("trigger relay fires on the authority → door opens"),
		DoorOpensWhen(TEXT("trly_auth"), NoConfig, [&]{ TRelayA->WoTTrigger(nullptr, nullptr); }));
	AWoTTrigger* TRelayP = World->SpawnActor<AWoTTrigger>(AlwaysSpawnParams());
	TRelayP->EventName = TEXT("trly_proxy");
	TRelayP->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("trigger relay on a simulated proxy does not fire → door stays shut"),
		DoorOpensWhen(TEXT("trly_proxy"), NoConfig, [&]{ TRelayP->WoTTrigger(nullptr, nullptr); }));

	// The overlap cases touch from inside the trigger's UE1 cylinder (the
	// pawn stands at (0,0,100)), so only the role gate decides the outcome.
	AWoTTrigger* TOverA = World->SpawnActor<AWoTTrigger>(AlwaysSpawnParams());
	WoTTest::PlaceWithoutOverlapEvents(TOverA, Pawn->GetActorLocation());
	TOverA->EventName = TEXT("tovr_auth");
	TOverA->TriggerType = EWoTTriggerType::PawnProximity;
	TestTrue(TEXT("trigger overlap on the authority → door opens"),
		DoorOpensWhen(TEXT("tovr_auth"), NoConfig,
			[&]{ TOverA->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult()); }));
	AWoTTrigger* TOverP = World->SpawnActor<AWoTTrigger>(AlwaysSpawnParams());
	WoTTest::PlaceWithoutOverlapEvents(TOverP, Pawn->GetActorLocation());
	TOverP->EventName = TEXT("tovr_proxy");
	TOverP->TriggerType = EWoTTriggerType::PawnProximity;
	TOverP->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("trigger overlap on a simulated proxy does not fire → door stays shut"),
		DoorOpensWhen(TEXT("tovr_proxy"), NoConfig,
			[&]{ TOverP->OnOverlap(nullptr, Pawn, nullptr, 0, false, FHitResult()); }));

	WoTTest::DestroySpecWorld(World);
	return true;
}

// AWoTInventory::ItemState now replicates (ReplicatedUsing=OnRep_ItemState). It was a plain
// member, so a client's enum stayed frozen at the default Pickup — held weapons read as world
// pickups on clients (UE5 does not re-hide on the native bHidden bit the way UE1 did, so an OnRep
// must re-derive rendering/collision). Part 1 locks the replication contract + the OnRep wiring
// (hard for CI to re-run cross-process). Part 2 exercises the
// presentation the OnRep applies (ApplyItemStatePresentation, shared with the public GotoItemState):
// a held item hides, a pickup shows. The cross-process "a client reads Held" proof
// lives in the live net harness.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetItemStateReplSpec,
	"WoT.Net.ItemStateRepl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetItemStateReplSpec::RunTest(const FString&)
{
	// --- Part 1: replication registration + OnRep wiring ---
	const FProperty* P = FindFProperty<FProperty>(AWoTInventory::StaticClass(), TEXT("ItemState"));
	TestTrue(TEXT("ItemState replicates (CPF_Net)"), P && (P->PropertyFlags & CPF_Net) != 0);
	TestEqual(TEXT("ItemState RepNotify is OnRep_ItemState"),
		P ? P->RepNotifyFunc : NAME_None, FName(TEXT("OnRep_ItemState")));

	// --- Part 2: the presentation OnRep applies (shared with GotoItemState) hides a held item ---
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvDart* Item = World->SpawnActor<AWoTAngrealInvDart>(
		AWoTAngrealInvDart::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, AlwaysSpawnParams());
	TestNotNull(TEXT("item spawned"), Item);
	if (!Item) { WoTTest::DestroySpecWorld(World); return false; }

	// PostInitializeComponents set it Pickup (visible, collidable).
	TestEqual(TEXT("spawns in Pickup"), (int32)Item->GetItemState(), (int32)EWoTInventoryState::Pickup);
	TestFalse(TEXT("a world pickup is not hidden"), Item->IsHidden());

	// Held → hidden (OnRep_ItemState invokes exactly this via ApplyItemStatePresentation).
	Item->GotoItemState(EWoTInventoryState::Held);
	TestEqual(TEXT("held state set"), (int32)Item->GetItemState(), (int32)EWoTInventoryState::Held);
	TestTrue(TEXT("a held item is hidden"), Item->IsHidden());

	// Back to Pickup → visible again.
	Item->GotoItemState(EWoTInventoryState::Pickup);
	TestFalse(TEXT("dropping to a pickup shows it again"), Item->IsHidden());

	WoTTest::DestroySpecWorld(World);
	return true;
}

// AWoTMover ports the original's CLIENT-SIMULATION keyframe model — a client interpolates the
// door locally from the replicated FSM Phase (SimulateClientInterp) instead of receiving raw
// transform snapshots (the old bReplicateMovement, steppy). Part 1 locks the contract: Phase is a
// replicated UPROPERTY (RepNotify OnRep_Phase), transform-snapshot replication is OFF, and the
// parity net props (NetPriority=2.7, bAlwaysRelevant) are restored (they were missing). Part 2
// proves the client-sim path: a mover forced to ROLE_SimulatedProxy interpolates OPEN under Tick
// (its authority FSM is gated off) — where the old code left a proxy frozen (Tick early-returned).
// RED before the port.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetMoverReplSpec,
	"WoT.Net.MoverRepl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetMoverReplSpec::RunTest(const FString&)
{
	// --- Part 1: replication contract ---
	const AWoTMover* CDO = GetDefault<AWoTMover>();
	TestFalse(TEXT("mover no longer streams transform snapshots"), CDO->IsReplicatingMovement());
	TestEqual(TEXT("mover NetPriority parity (2.7)"), CDO->NetPriority, 2.7f);
	TestTrue(TEXT("mover is bAlwaysRelevant"), CDO->bAlwaysRelevant);
	const FProperty* PP = FindFProperty<FProperty>(AWoTMover::StaticClass(), TEXT("Phase"));
	TestTrue(TEXT("Phase replicates (CPF_Net)"), PP && (PP->PropertyFlags & CPF_Net) != 0);
	TestEqual(TEXT("Phase RepNotify is OnRep_Phase"),
		PP ? PP->RepNotifyFunc : NAME_None, FName(TEXT("OnRep_Phase")));

	// --- Part 2: a simulated-proxy mover interpolates open locally ---
	UWorld* World = WoTTest::CreateSpecWorld();
	const FTransform X(FRotator::ZeroRotator, FVector(0, 0, 100));
	AWoTMover* M = World->SpawnActorDeferred<AWoTMover>(AWoTMover::StaticClass(), X);
	M->InitialState = EWoTMoverState::TriggerOpenTimed;   // opens on a fired event
	M->KeyPos = { FVector::ZeroVector, FVector(0, 0, 100) };
	M->KeyRot = { FRotator::ZeroRotator, FRotator::ZeroRotator };
	M->MoveTime = 0.5f;
	M->StayOpenTime = 30.0f;
	M->TagName = TEXT("mv");
	M->FinishSpawning(X);
	const double BaseZ = M->GetActorLocation().Z;

	M->WoTTrigger(nullptr, nullptr);       // authority: enter the Opening phase
	M->SetRole(ROLE_SimulatedProxy);       // now a client proxy (authority Tick FSM gated off)
	WoTTest::TickWorld(World, 15);           // 0.25s (< MoveTime 0.5) — expect mid-interpolation
	const double MidZ = M->GetActorLocation().Z;
	TestTrue(FString::Printf(
		TEXT("a simulated-proxy mover interpolates open locally (Z %.0f -> %.0f)"), BaseZ, MidZ),
		MidZ > BaseZ + 20.0 && MidZ < BaseZ + 90.0);   // moving, not frozen, not snapped-to-end

	// --- Part 3: PhaseStartServerTime replicates (the SimInterpolate/PhysAlpha analogue) ---
	const FProperty* PS = FindFProperty<FProperty>(AWoTMover::StaticClass(),
		TEXT("PhaseStartServerTime"));
	TestTrue(TEXT("PhaseStartServerTime replicates (CPF_Net)"),
		PS && (PS->PropertyFlags & CPF_Net) != 0);
	TestEqual(TEXT("PhaseStartServerTime shares OnRep_Phase"),
		PS ? PS->RepNotifyFunc : NAME_None, FName(TEXT("OnRep_Phase")));

	// --- Part 4: TriggerToggle honors Engine.Mover InterpolateTo — alpha 0 from rest,
	// PhysAlpha = 1 - PhysAlpha on a mid-move reversal (no jump either way) ---
	AWoTMover* T = World->SpawnActorDeferred<AWoTMover>(AWoTMover::StaticClass(), X);
	T->InitialState = EWoTMoverState::TriggerToggle;
	T->KeyPos = { FVector::ZeroVector, FVector(0, 0, 100) };
	T->KeyRot = { FRotator::ZeroRotator, FRotator::ZeroRotator };
	T->MoveTime = 0.5f;
	T->TagName = TEXT("tg");
	T->FinishSpawning(X);
	const double TBaseZ = T->GetActorLocation().Z;

	T->WoTTrigger(nullptr, nullptr);       // toggle from Closed (at rest)
	WoTTest::TickWorld(World, 1);            // one 1/60 step
	const double T1Z = T->GetActorLocation().Z;
	TestTrue(FString::Printf(
		TEXT("toggle from rest opens from alpha 0, not instantly (Z %.0f -> %.1f)"), TBaseZ, T1Z),
		T1Z < TBaseZ + 15.0);              // the old MoveTime-PhaseTime math snapped to +100

	WoTTest::TickWorld(World, 14);           // ~0.25s total — mid-open
	const double TUpZ = T->GetActorLocation().Z;
	T->WoTTrigger(nullptr, nullptr);       // reverse mid-open → Closing
	WoTTest::TickWorld(World, 1);
	const double TRevZ = T->GetActorLocation().Z;
	TestTrue(FString::Printf(
		TEXT("mid-open reversal continues from the current alpha (Z %.1f -> %.1f)"), TUpZ, TRevZ),
		FMath::Abs(TRevZ - TUpZ) < 10.0);  // the old StartClose reset jumped to +100 first
	WoTTest::TickWorld(World, 40);           // ride the close out
	TestTrue(TEXT("reversal closes back to the base keyframe"),
		FMath::Abs(T->GetActorLocation().Z - TBaseZ) < 1.0);

	// --- Part 5: TriggerToggle ignores bTriggerOnceOnly — Engine.Mover gives every timed/button
	// state a bTriggerOnceOnly dormancy check, but the TriggerToggle state has none: a toggle
	// mover toggles forever. Regression guard: open fully, close fully, then the THIRD trigger
	// must reopen (a bSpent gate on the from-rest path left the door dead here). ---
	T->bTriggerOnceOnly = true;
	T->WoTTrigger(nullptr, nullptr);       // 1st: from rest → open
	WoTTest::TickWorld(World, 40);           // > MoveTime — open completes (once-only would latch)
	TestTrue(TEXT("once-only toggle opens fully"),
		FMath::Abs(T->GetActorLocation().Z - (TBaseZ + 100.0)) < 1.0);
	T->WoTTrigger(nullptr, nullptr);       // 2nd: open → close
	WoTTest::TickWorld(World, 40);
	TestTrue(TEXT("once-only toggle closes fully"),
		FMath::Abs(T->GetActorLocation().Z - TBaseZ) < 1.0);
	T->WoTTrigger(nullptr, nullptr);       // 3rd: must reopen — TriggerToggle never goes dormant
	WoTTest::TickWorld(World, 40);
	TestTrue(TEXT("third trigger reopens a bTriggerOnceOnly toggle (no dormancy in Mover.uc)"),
		FMath::Abs(T->GetActorLocation().Z - (TBaseZ + 100.0)) < 1.0);

	WoTTest::DestroySpecWorld(World);
	return true;
}

// The legacy citadel-editor read-surface + edit-phase replication contract. BudgetInfo stays
// server-only. WOTInventory.Count retains its shipped owner-only parity channel for real inventory
// instances; the actor-free palette uses the controller budget-row mirror covered by
// WoT.Citadel.EditorPalette.NetOwnerOnlyReplication. Edit-phase flags + teleporter state replicate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetCitadelEditSurfaceSpec,
	"WoT.Net.CitadelEditSurface", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetCitadelEditSurfaceSpec::RunTest(const FString&)
{
	auto IsNet = [](UClass* Cls, const TCHAR* Name) -> bool
	{
		const FProperty* P = FindFProperty<FProperty>(Cls, Name);
		return P && (P->PropertyFlags & CPF_Net) != 0;
	};

	// BudgetInfo is bStatic/server-only per BudgetInfo.uc — it must NOT be set to replicate
	// (regression guard vs a well-meaning future DOREPLIFETIME). GetIsReplicated() reads bReplicates.
	TestFalse(TEXT("BudgetInfo does not replicate (bStatic/server-only per BudgetInfo.uc)"),
		GetDefault<AWoTBudgetInfo>()->GetIsReplicated());

	// The legacy resource-count channel on real hand items. It replicates COND_OwnerOnly — set by the
	// DOREPLIFETIME_CONDITION macro (code-reviewed; re-calling GetLifetimeReplicatedProps on a CDO
	// to read the runtime condition trips the engine's push-model assertion, so assert CPF_Net here).
	TestTrue(TEXT("WOTInventory Count replicates"), IsNet(AWoTInventory::StaticClass(), TEXT("Count")));

	// Edit-phase gates + teleporter state already replicate (all-clients status surface) — verify.
	TestTrue(TEXT("BattleInfo bEditing replicates"), IsNet(AWoTBattleInfo::StaticClass(), TEXT("bEditing")));
	TestTrue(TEXT("BattleInfo bEditingComplete replicates"), IsNet(AWoTBattleInfo::StaticClass(), TEXT("bEditingComplete")));
	TestTrue(TEXT("BattleInfo bTeleportersDisabled replicates"), IsNet(AWoTBattleInfo::StaticClass(), TEXT("bTeleportersDisabled")));
	TestTrue(TEXT("Teleporter bEnabled replicates"), IsNet(AWoTTeleporter::StaticClass(), TEXT("bEnabled")));

	return true;
}

// The session seam over the OSS Sessions API. Headless, this
// exercises the NULL platform service (the config's Steam service fails init
// without a running Steam client and the OSS falls back) — the interface the
// Steam path uses is identical by construction; the live-Steam lobby/SDR match
// needs a live two-machine run.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTNetSessionSpec,
	"WoT.Net.Session", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTNetSessionSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTSessionSubsystem* S =
	    World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("the session subsystem exists"), S))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("no session yet"), S->HasSession());

	// Host: the named GameSession lands via the (possibly deferred) OSS callback.
	// GIsAutomationTesting suppresses the listen ServerTravel (the MapRotation
	// pattern — travel breaks the standalone test world).
	TestTrue(TEXT("Host accepted"), S->Host(8, /*bLan*/ true));
	WoTTest::TickWorld(World, 5);
	TestTrue(TEXT("the named session exists"), S->HasSession());

	TestTrue(TEXT("Leave accepted"), S->Leave());
	WoTTest::TickWorld(World, 5);
	TestFalse(TEXT("the session is destroyed"), S->HasSession());

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
