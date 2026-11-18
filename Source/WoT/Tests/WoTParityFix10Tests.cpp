#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

namespace
{
constexpr EAutomationTestFlags Fix10_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

void Fix10_SendViewportKey(FSceneViewport& Viewport, const FKey& Key,
	bool bPressed)
{
	const FKeyEvent Event(Key, FModifierKeysState(), 0,
		/*bInIsRepeat*/ false, 0, 0);
	if (bPressed)
	{
		Viewport.OnKeyDown(FGeometry(), Event);
	}
	else
	{
		Viewport.OnKeyUp(FGeometry(), Event);
	}
}

AActor* Fix10_SpawnNavigationBox(UWorld& World, const FVector& Location,
	const FVector& Extent)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(
		Location, FRotator::ZeroRotator, Params);
	if (!Actor)
	{
		return nullptr;
	}
	UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
	Box->SetupAttachment(Actor->GetRootComponent());
	Box->SetBoxExtent(Extent);
	Box->SetMobility(EComponentMobility::Static);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->SetCanEverAffectNavigation(true);
	Box->RegisterComponent();
	return Actor;
}

bool Fix10_BuildNavigationMesh(UWorld& World)
{
	Fix10_SpawnNavigationBox(World, FVector(0.0f, 0.0f, -50.0f),
		FVector(1200.0f, 1200.0f, 50.0f));
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ANavMeshBoundsVolume* Bounds = World.SpawnActor<ANavMeshBoundsVolume>(
		ANavMeshBoundsVolume::StaticClass(), FVector(0.0f, 0.0f, 200.0f),
		FRotator::ZeroRotator, Params);
	if (!Bounds)
	{
		return false;
	}
	UBoxComponent* BoundsBox = NewObject<UBoxComponent>(Bounds);
	BoundsBox->SetBoxExtent(FVector(1200.0f, 1200.0f, 400.0f));
	BoundsBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoundsBox->SetCanEverAffectNavigation(false);
	BoundsBox->SetupAttachment(Bounds->GetRootComponent());
	BoundsBox->RegisterComponent();

	if (!FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World))
	{
		FNavigationSystem::AddNavigationSystemToWorld(
			World, FNavigationSystemRunMode::GameMode);
	}
	UNavigationSystemV1* Nav =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World);
	if (!Nav)
	{
		return false;
	}
	Nav->OnNavigationBoundsUpdated(Bounds);
	Nav->Build();
	for (int32 Tick = 0; Tick < 600 && Nav->IsNavigationBuildInProgress(); ++Tick)
	{
		World.Tick(LEVELTICK_All, 1.0f / 60.0f);
		++GFrameCounter;
	}
	return Nav->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) != nullptr;
}

AWoTNPCCharacter* Fix10_SpawnGuard(UWorld& World, const FVector& Location)
{
	AWoTNPCCharacter* Guard = WoTCreatureData::SpawnCreature(World,
		TEXT("Trolloc"), Location, 1, true, AWoTTestNPCCharacter::StaticClass());
	if (Guard)
	{
		AWoTCharacter* ActivityAnchor = World.SpawnActor<AWoTCharacter>(Location,
			FRotator::ZeroRotator);
		if (ActivityAnchor)
		{
			ActivityAnchor->Health = 0;
			ActivityAnchor->SetActorEnableCollision(false);
			ActivityAnchor->SetActorHiddenInGame(true);
		}
	}
	return Guard;
}
}

// This fixture needs the editor PIE net-mode seam; the native
// WoTGuardedEngineTests target compiles this file without editor APIs.
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix10PausedChatReleasedKeysSpec,
	"WoT.Parity.Fix10.PausedChatRoutesReleasedKeys", Fix10_Flags)

bool FFix10PausedChatReleasedKeysSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.BeforeInitializeActorsForPlay = [](UWorld* World)
	{
		World->WorldType = EWorldType::PIE;
		World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	FWorldContext* Context = World
		? GEngine->GetWorldContextFromWorld(World) : nullptr;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UGameViewportClient* ViewportClient = Context && GameInstance
		? NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass)
		: nullptr;
	TSharedPtr<SViewport> ViewportWidget;
	TSharedPtr<FSceneViewport> SceneViewport;
	ON_SCOPE_EXIT
	{
		if (ViewportClient && SceneViewport)
		{
			ViewportClient->RemoveAssociation(*SceneViewport);
		}
		SceneViewport.Reset();
		ViewportWidget.Reset();
		if (Context)
		{
			Context->GameViewport = nullptr;
		}
		WoTTest::DestroySpecWorld(World);
	};
	if (!TestNotNull(TEXT("listen world"), World)
		|| !TestNotNull(TEXT("world context"), Context)
		|| !TestNotNull(TEXT("game instance"), GameInstance)
		|| !TestNotNull(TEXT("game viewport client"), ViewportClient))
	{
		return false;
	}
	ViewportClient->Init(*Context, GameInstance);
	Context->GameViewport = ViewportClient;
	ViewportWidget = SNew(SViewport);
	SceneViewport = ViewportClient->CreateViewport(ViewportWidget);
	SceneViewport->SetInitialSize(FIntPoint(640, 480));
	ViewportClient->AddAssociation(*SceneViewport);

	FString PlayerError;
	ULocalPlayer* LocalPlayer = GameInstance->CreateLocalPlayer(
		0, PlayerError, /*bSpawnPlayerController*/ false);
	AWoTGameMode* Game = World->GetAuthGameMode<AWoTGameMode>();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
	AWoTCharacter* Victim = World->SpawnActor<AWoTCharacter>(
		FVector(300.0f, 0.0f, 542.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("local player"), LocalPlayer)
		|| !TestNotNull(TEXT("listen rules"), Game)
		|| !TestNotNull(TEXT("local controller"), Controller)
		|| !TestNotNull(TEXT("caster"), Pawn)
		|| !TestNotNull(TEXT("balefire victim"), Victim))
	{
		return false;
	}
	Controller->SetPlayer(LocalPlayer);
	Controller->Possess(Pawn);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Victim->Health = 1000;
	Controller->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
	Controller->PlayerInput->AddActionMapping(
		FInputActionKeyMapping(TEXT("Fire"), EKeys::F));
	Controller->PlayerInput->AddAxisMapping(
		FInputAxisKeyMapping(TEXT("MoveForward"), EKeys::W, 1.0f));

	AWoTAngrealInvBalefire* Balefire = World->SpawnActor<AWoTAngrealInvBalefire>(
		FVector(0.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("balefire inventory"), Balefire))
	{
		return false;
	}
	Balefire->GiveTo(Pawn);
	Pawn->InventoryComponent->SelectItem(Balefire);
	WoTTest::TickWorld(World, 0.1f);

	Fix10_SendViewportKey(*SceneViewport, EKeys::F, true);
	Fix10_SendViewportKey(*SceneViewport, EKeys::W, true);
	WoTTest::TickWorld(World, 2.5f);
	TestEqual(TEXT("held balefire remains charged before pause"),
		Victim->Health, 1000);
	TestTrue(TEXT("real listen pause succeeds"),
		Game->TrySetPlayerPause(Controller, NM_ListenServer));
	Fix10_SendViewportKey(*SceneViewport, EKeys::T, true);
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestTrue(TEXT("Talk opens typing from the real viewport while paused"),
		Controller->IsChatTyping());

	Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE,
		EKeys::F, IE_Released, 0.0f, false, 0));
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestFalse(TEXT("the paused Fire release reaches gameplay immediately"),
		Controller->PlayerInput->IsPressed(EKeys::F));
	TestEqual(TEXT("the paused Fire release ends the charged cast"),
		Victim->Health, 600);
	TestTrue(TEXT("the separately held movement key remains held"),
		Controller->PlayerInput->IsPressed(EKeys::W));
	Controller->CancelChatInput();
	Game->ClearPause();
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestEqual(TEXT("chat close and unpause add no second Fire transition"),
		Victim->Health, 600);
	TestTrue(TEXT("the physically held movement key remains held after unpause"),
		Controller->PlayerInput->IsPressed(EKeys::W));
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix10NPCReturnValidStartSpec,
	"WoT.Parity.Fix10.NPCReturnRequiresValidNavStart", Fix10_Flags)

bool FFix10NPCReturnValidStartSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("AI world"), World)
		|| !TestTrue(TEXT("test navigation mesh builds"),
			Fix10_BuildNavigationMesh(*World)))
	{
		return false;
	}

	const FVector OriginalPost(600.0f, 0.0f, 60.0f);
	AWoTNPCCharacter* KnockedGuard = Fix10_SpawnGuard(
		*World, FVector(1500.0f, 0.0f, 300.0f));
	AWoTNPCController* KnockedController = KnockedGuard
		? Cast<AWoTNPCController>(KnockedGuard->GetController()) : nullptr;
	UWoTTestBrainComponent* KnockedBrain = KnockedGuard
		? Cast<UWoTTestBrainComponent>(KnockedGuard->Brain) : nullptr;
	if (!TestNotNull(TEXT("knocked guard"), KnockedGuard)
		|| !TestNotNull(TEXT("knocked guard controller"), KnockedController)
		|| !TestNotNull(TEXT("knocked guard brain"), KnockedBrain))
	{
		return false;
	}
	KnockedGuard->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	KnockedBrain->bForceReachable = false;
	KnockedBrain->bForcePathable = true;
	KnockedBrain->SetWaitingGoal(OriginalPost, FRotator::ZeroRotator);
	KnockedController->TransitionOnGoalPriority(true);
	WoTTest::TickWorld(World, 8, 1.0f / 60.0f);
	TestEqual(TEXT("falling off-mesh failures do not give up the guard post"),
		KnockedController->GetBrainState(), EWoTBrainState::Returning);
	TestTrue(TEXT("falling off-mesh failures preserve the authored post"),
		KnockedBrain->GetGoal(EWoTGoalIndex::Waiting).GoalVector.Equals(
			OriginalPost, 0.01f));

	KnockedGuard->SetActorLocation(FVector(-600.0f, 0.0f, 60.0f),
		false, nullptr, ETeleportType::TeleportPhysics);
	KnockedGuard->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	KnockedGuard->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	const float GroundedStartDistance = FVector::Dist2D(
		KnockedGuard->GetActorLocation(), OriginalPost);
	WoTTest::TickWorld(World, 120, 1.0f / 60.0f);
	TestTrue(TEXT("the grounded guard walks back toward the preserved post"),
		FVector::Dist2D(KnockedGuard->GetActorLocation(), OriginalPost)
			< GroundedStartDistance - 50.0f);

	AWoTNPCCharacter* StrandedGuard = Fix10_SpawnGuard(
		*World, FVector(-600.0f, 500.0f, 60.0f));
	AWoTNPCController* StrandedController = StrandedGuard
		? Cast<AWoTNPCController>(StrandedGuard->GetController()) : nullptr;
	UWoTTestBrainComponent* StrandedBrain = StrandedGuard
		? Cast<UWoTTestBrainComponent>(StrandedGuard->Brain) : nullptr;
	if (!TestNotNull(TEXT("stranded guard"), StrandedGuard)
		|| !TestNotNull(TEXT("stranded guard controller"), StrandedController)
		|| !TestNotNull(TEXT("stranded guard brain"), StrandedBrain))
	{
		return false;
	}
	StrandedGuard->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	StrandedBrain->bForceReachable = false;
	StrandedBrain->bForcePathable = true;
	StrandedBrain->SetWaitingGoal(
		FVector(5000.0f, 500.0f, 60.0f), FRotator::ZeroRotator);
	StrandedController->TransitionOnGoalPriority(true);
	WoTTest::TickWorld(World, 8, 1.0f / 60.0f);
	TestEqual(TEXT("grounded on-mesh failures still take UnableToReturn"),
		StrandedController->GetBrainState(), EWoTBrainState::Waiting);
	TestTrue(TEXT("a genuinely unreachable post adopts the grounded guard spot"),
		StrandedBrain->GetGoal(EWoTGoalIndex::Waiting).GoalVector.Equals(
			StrandedGuard->GetActorLocation(), 0.01f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix10AuthoritativeFactionSkinSpec,
	"WoT.Parity.Fix10.SkinRequestMatchesAuthoritativeFaction", Fix10_Flags)

bool FFix10AuthoritativeFactionSkinSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTBattleGameMode* Mode = World
		? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	AWoTBattleInfo* Battle = World
		? World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity) : nullptr;
	if (Battle)
	{
		Battle->NumTeams = 2;
		Battle->TeamClass[0] = AWoTHound::StaticClass();
		Battle->TeamClass[1] = AWoTForsaken::StaticClass();
		Battle->bTeleportersDisabled = false;
		Battle->FinishSpawning(FTransform::Identity);
	}
	AWoTPlayerStart* Start = World ? World->SpawnActor<AWoTPlayerStart>(
		FVector(500.0f, 0.0f, 100.0f), FRotator::ZeroRotator) : nullptr;
	AWoTPlayerController* Controller = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTAesSedai* Pawn = World ? World->SpawnActor<AWoTAesSedai>(
		FVector::ZeroVector, FRotator::ZeroRotator) : nullptr;
	if (!TestNotNull(TEXT("Battle rules"), Mode)
		|| !TestNotNull(TEXT("BattleInfo"), Battle)
		|| !TestNotNull(TEXT("player start"), Start)
		|| !TestNotNull(TEXT("authority controller"), Controller)
		|| !TestNotNull(TEXT("pre-team pawn"), Pawn))
	{
		return false;
	}
	Mode->Battle = Battle;
	Controller->Possess(Pawn);
	AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("authority player state"), State))
	{
		return false;
	}
	State->Team = 255;
	TestTrue(TEXT("real Battle team change succeeds"),
		Mode->ChangeTeam(Controller, 0));
	TestTrue(TEXT("team change establishes the authoritative Hound faction"),
		State->FactionClass == AWoTHound::StaticClass());
	Controller->ServerChangeSkin(
		TEXT("HoundSkins.Hou_Green"), FString(), 255);
	TestEqual(TEXT("the current-faction request is stored"),
		Controller->PersonalSkinURL, FString(TEXT("HoundSkins.Hou_Green")));
	TestEqual(TEXT("the current-faction request applies to the live pawn"),
		Pawn->PersonalSkin, FName(TEXT("Hou_Green")));

	// A stale menu was populated from the faction replicated before ChangeTeam.
	Controller->ServerChangeSkin(
		TEXT("AesSedaiSkins.Aes_Red"), FString(), 255);
	TestEqual(TEXT("a stale-faction request cannot replace the stored team skin"),
		Controller->PersonalSkinURL, FString(TEXT("HoundSkins.Hou_Green")));

	Controller->UnPossess();
	Pawn->Destroy();
	Mode->RestartPlayer(Controller);
	AWoTCharacter* Respawned = Cast<AWoTCharacter>(Controller->GetPawn());
	TestTrue(TEXT("the player respawns as the authoritative team faction"),
		Respawned && Respawned->IsA(AWoTHound::StaticClass()));
	if (Respawned)
	{
		TestEqual(TEXT("the respawn retains the team-faction colour"),
			Respawned->PersonalSkin, FName(TEXT("Hou_Green")));
	}
	return !HasAnyErrors();
}

#endif
