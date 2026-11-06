#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTZoneVolume.h"
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
constexpr EAutomationTestFlags Fix11_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FKeyEvent Fix11_KeyEvent(const FKey& Key)
{
	return FKeyEvent(Key, FModifierKeysState(), 0,
		/*bInIsRepeat*/ false, 0, 0);
}

AActor* Fix11_SpawnNavigationBox(UWorld& World, const FVector& Location,
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

bool Fix11_BuildNavigationMesh(UWorld& World)
{
	Fix11_SpawnNavigationBox(World, FVector(0.0f, 0.0f, -50.0f),
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
	for (int32 Tick = 0;
		Tick < 600 && Nav->IsNavigationBuildInProgress(); ++Tick)
	{
		World.Tick(LEVELTICK_All, 1.0f / 60.0f);
		++GFrameCounter;
	}
	return Nav->GetDefaultNavDataInstance(
		FNavigationSystem::DontCreate) != nullptr;
}

AWoTNPCCharacter* Fix11_SpawnGuard(UWorld& World, const FVector& Location)
{
	AWoTNPCCharacter* Guard = WoTCreatureData::SpawnCreature(World,
		TEXT("Trolloc"), Location, 1, true,
		AWoTTestNPCCharacter::StaticClass());
	if (Guard)
	{
		AWoTCharacter* ActivityAnchor = World.SpawnActor<AWoTCharacter>(
			Location, FRotator::ZeroRotator);
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

// The PIE net-mode seam is editor-only; the guarded native target still
// compiles this translation unit without the fixture.
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix11PausedChatSlateReleaseSpec,
	"WoT.Parity.Fix11.PausedChatSlateRelease", Fix11_Flags)

bool FFix11PausedChatSlateReleaseSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless (-nullrhi) Slate focus on an editable chat widget has no native
	// NSWindow, so the macOS text-input-method system logs an Error that a real
	// game window never produces. Ignore it here; the assertions are unaffected.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
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
	TSharedPtr<SWindow> Window;
	UGameViewportClient* PreviousEngineViewport = GEngine
		? GEngine->GameViewport.Get() : nullptr;
	ON_SCOPE_EXIT
	{
		if (GEngine)
		{
			GEngine->GameViewport = PreviousEngineViewport;
		}
		if (Window.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			FSlateApplication::Get().Tick();
		}
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
		|| !TestNotNull(TEXT("game viewport client"), ViewportClient)
		|| !TestTrue(TEXT("Slate application is initialized"),
			FSlateApplication::IsInitialized()))
	{
		return false;
	}

	ViewportClient->Init(*Context, GameInstance);
	Context->GameViewport = ViewportClient;
	GEngine->GameViewport = ViewportClient;
	ViewportWidget = SNew(SViewport);
	SceneViewport = ViewportClient->CreateViewport(ViewportWidget);
	SceneViewport->SetInitialSize(FIntPoint(640, 480));
	ViewportClient->AddAssociation(*SceneViewport);
	Window = SNew(SWindow)
		.ClientSize(FVector2D(640.0f, 480.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.CreateTitleBar(false)
		[ViewportWidget.ToSharedRef()];
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window.ToSharedRef(), false);
	Slate.Tick();
	Window->SlatePrepass(1.0f);
	Slate.SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
	if (!TestTrue(TEXT("real scene viewport owns keyboard focus"),
		Slate.GetKeyboardFocusedWidget() == ViewportWidget))
	{
		return false;
	}

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
	Controller->EnableChatWidgetForTesting();
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

	Slate.ProcessKeyDownEvent(Fix11_KeyEvent(EKeys::F));
	Slate.ProcessKeyDownEvent(Fix11_KeyEvent(EKeys::W));
	WoTTest::TickWorld(World, 2.5f);
	TestEqual(TEXT("held balefire remains charged before pause"),
		Victim->Health, 1000);
	TestTrue(TEXT("real listen pause succeeds"),
		Game->TrySetPlayerPause(Controller, NM_ListenServer));
	Slate.ProcessKeyDownEvent(Fix11_KeyEvent(EKeys::T));
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestTrue(TEXT("Talk opens the actual chat overlay while paused"),
		Controller->IsChatTyping());
	TestTrue(TEXT("actual chat focus leaves FSceneViewport"),
		Slate.GetKeyboardFocusedWidget().IsValid()
			&& Slate.GetKeyboardFocusedWidget() != ViewportWidget);
	TestFalse(TEXT("focus loss clears FSceneViewport's physical-key cache"),
		SceneViewport->KeyState(EKeys::F));
	TestTrue(TEXT("deliberate chat handoff preserves gameplay's held Fire"),
		Controller->PlayerInput->IsPressed(EKeys::F));
	TestTrue(TEXT("deliberate chat handoff preserves a held movement key"),
		Controller->PlayerInput->IsPressed(EKeys::W));

	Slate.ProcessKeyUpEvent(Fix11_KeyEvent(EKeys::F));
	Slate.ProcessKeyUpEvent(Fix11_KeyEvent(EKeys::T));
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestFalse(TEXT("Slate forwards Fire release while chat is focused"),
		Controller->PlayerInput->IsPressed(EKeys::F));
	TestEqual(TEXT("the Fire release ends the cast while paused"),
		Victim->Health, 600);
	Slate.ProcessKeyDownEvent(Fix11_KeyEvent(EKeys::Escape));
	TestFalse(TEXT("real Escape path closes chat"), Controller->IsChatTyping());
	Game->ClearPause();
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	TestEqual(TEXT("unpause adds no second Fire transition"),
		Victim->Health, 600);
	TestTrue(TEXT("a key still held through chat stays held"),
		Controller->PlayerInput->IsPressed(EKeys::W));
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix11SwimmingReturnBoundSpec,
	"WoT.Parity.Fix11.SwimmingReturnBound", Fix11_Flags)

bool FFix11SwimmingReturnBoundSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	if (!TestNotNull(TEXT("AI world"), World)
		|| !TestTrue(TEXT("test navigation mesh builds"),
			Fix11_BuildNavigationMesh(*World)))
	{
		return false;
	}

	AWoTZoneVolume* Water = World->SpawnActor<AWoTZoneVolume>(
		FVector(-600.0f, 0.0f, 100.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("real water zone"), Water))
	{
		return false;
	}
	Water->Collision->SetBoxExtent(FVector(400.0f, 350.0f, 300.0f));
	Water->bWaterZone = true;

	AWoTNPCCharacter* Swimmer = Fix11_SpawnGuard(
		*World, FVector(-600.0f, 0.0f, 60.0f));
	AWoTNPCController* SwimController = Swimmer
		? Cast<AWoTNPCController>(Swimmer->GetController()) : nullptr;
	UWoTTestBrainComponent* SwimBrain = Swimmer
		? Cast<UWoTTestBrainComponent>(Swimmer->Brain) : nullptr;
	UCharacterMovementComponent* SwimMovement = Swimmer
		? Swimmer->GetCharacterMovement() : nullptr;
	if (!TestNotNull(TEXT("swimming guard"), Swimmer)
		|| !TestNotNull(TEXT("swimming guard controller"), SwimController)
		|| !TestNotNull(TEXT("swimming guard brain"), SwimBrain)
		|| !TestNotNull(TEXT("swimming guard movement"), SwimMovement))
	{
		return false;
	}
	SwimMovement->SetMovementMode(MOVE_Walking);
	WoTTest::TickWorld(World, 2, 1.0f / 60.0f);
	TestTrue(TEXT("swimmer is inside the real water zone"),
		AWoTZoneVolume::IsPointInWater(World, Swimmer->GetActorLocation()));
	TestTrue(TEXT("water physics establishes stable swimming locomotion"),
		SwimMovement->IsSwimming());
	TestFalse(TEXT("the swimmer is not classified as grounded"),
		SwimMovement->IsMovingOnGround());

	const FVector UnsubmittableSwimPost(5000.0f, 0.0f, 60.0f);
	SwimBrain->bForceReachable = false;
	SwimBrain->bForcePathable = true;
	SwimBrain->SetWaitingGoal(
		UnsubmittableSwimPost, FRotator::ZeroRotator);
	SwimController->TransitionOnGoalPriority(true);
	TestEqual(TEXT("swimmer begins the real return path"),
		SwimController->GetBrainState(), EWoTBrainState::Returning);
	WoTTest::TickWorld(World, 8, 1.0f / 60.0f);
	TestEqual(TEXT("stable swimmer takes UnableToReturn after the bound"),
		SwimController->GetBrainState(), EWoTBrainState::Waiting);
	const FVector AdoptedSwimPost =
		SwimBrain->GetGoal(EWoTGoalIndex::Waiting).GoalVector;
	TestTrue(TEXT("UnableToReturn adopts the swimmer's stranded spot"),
		!AdoptedSwimPost.Equals(UnsubmittableSwimPost, 0.01f)
		&& FVector::Dist(AdoptedSwimPost,
			Swimmer->GetActorLocation()) < 5.0f);

	AWoTNPCCharacter* FallingGuard = Fix11_SpawnGuard(
		*World, FVector(-600.0f, 800.0f, 200.0f));
	AWoTNPCController* FallingController = FallingGuard
		? Cast<AWoTNPCController>(FallingGuard->GetController()) : nullptr;
	UWoTTestBrainComponent* FallingBrain = FallingGuard
		? Cast<UWoTTestBrainComponent>(FallingGuard->Brain) : nullptr;
	UCharacterMovementComponent* FallingMovement = FallingGuard
		? FallingGuard->GetCharacterMovement() : nullptr;
	if (!TestNotNull(TEXT("falling guard"), FallingGuard)
		|| !TestNotNull(TEXT("falling guard controller"), FallingController)
		|| !TestNotNull(TEXT("falling guard brain"), FallingBrain)
		|| !TestNotNull(TEXT("falling guard movement"), FallingMovement))
	{
		return false;
	}
	const FVector UnsubmittableFallingPost(5000.0f, 800.0f, 200.0f);
	FallingMovement->SetMovementMode(MOVE_Falling);
	FallingBrain->bForceReachable = false;
	FallingBrain->bForcePathable = true;
	FallingBrain->SetWaitingGoal(
		UnsubmittableFallingPost, FRotator::ZeroRotator);
	FallingController->TransitionOnGoalPriority(true);
	WoTTest::TickWorld(World, 8, 1.0f / 60.0f);
	TestTrue(TEXT("falling fixture remains in transient locomotion"),
		FallingMovement->IsFalling());
	TestEqual(TEXT("falling failures do not consume the return bound"),
		FallingController->GetBrainState(), EWoTBrainState::Returning);
	TestTrue(TEXT("falling failures preserve the authored post"),
		FallingBrain->GetGoal(EWoTGoalIndex::Waiting).GoalVector.Equals(
			UnsubmittableFallingPost, 0.01f));
	return !HasAnyErrors();
}

#endif
