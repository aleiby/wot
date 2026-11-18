#include "WoTLocalPlayerHooksTestFixture.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "../FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../WoTCharacter.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTOnboardingSeams.h"
#include "../WoTPlayerController.h"
#include "../WoTPreferencesSubsystem.h"
#include "WoTEarlyUI.h"
#include "WoTLaunchEngineHooks.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/AutomationTest.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

namespace WoTLocalPlayerHooksTest
{
bool FPreferencesScope::Init(FAutomationTestBase& Test)
{
	// The startup order WoTGameSettingsTests.cpp uses: creation enabled, then a
	// validated early-UI record published and projected before the world exists.
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	FWoTEarlyUIAuthority& Authority = GetEarlyUIAuthority();
	Authority.ResetForTesting();
	bActive = true;
	static const ANSICHAR Record[] = "Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n";
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Record), FCStringAnsi::Strlen(Record));
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> EarlyUI = ParseEarlyUIV1(
		Bytes, FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
	if (!Test.TestFalse(TEXT("early-UI fixture parses"), EarlyUI.IsError())
		|| !Test.TestFalse(TEXT("early-UI fixture publishes"),
			Authority.PublishOnce(MoveTemp(EarlyUI).GetValue()).IsError()))
	{
		return false;
	}
	const auto Apply = [](uint16, uint16, EWoTWindowMode) -> FWoTBootstrapStatus
	{
		return FWoTBootstrapStatus::Success();
	};
	const FWoTEarlyUINativeProjectionSink Sink{Apply};
	FString Error;
	const bool bProjected =
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(Sink, &Error);
	return Test.TestTrue(FString::Printf(TEXT("early-UI fixture projects (%s)"), *Error), bProjected);
}

FPreferencesScope::~FPreferencesScope()
{
	if (bActive)
	{
		GetEarlyUIAuthority().ResetForTesting();
		UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false);
	}
}

bool FPlayerWorld::Init(FAutomationTestBase& Test, const FVector& PawnLocation,
	EMovementMode Mode, TSubclassOf<AGameModeBase> GameMode)
{
	World = WoTTest::CreateSpecWorld(GameMode);
	if (!Test.TestNotNull(TEXT("hook spec world"), World))
	{
		return false;
	}
	Context = GEngine->GetWorldContextFromWorld(World);
	UGameInstance* Instance = World->GetGameInstance();
	if (!Test.TestNotNull(TEXT("hook world context"), Context)
		|| !Test.TestNotNull(TEXT("hook game instance"), Instance))
	{
		return false;
	}
	// An input-ready game viewport, so the local player's controller builds its
	// real input stack and ticks as a local player.
	Viewport = NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass);
	Viewport->Init(*Context, Instance);
	Context->GameViewport = Viewport;
	Widget = SNew(SViewport);
	Scene = Viewport->CreateViewport(Widget);
	Scene->SetInitialSize(FIntPoint(640, 480));
	Viewport->AddAssociation(*Scene);
	FString Error;
	Player = Instance->CreateLocalPlayer(0, Error, false);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// As the game mode spawns a local player's controller: local from BeginPlay,
	// which installs the controller's own window and overlay seams.
	PC = World->SpawnActorDeferred<AWoTPlayerController>(AWoTPlayerController::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (PC)
	{
		PC->SetAsLocalPlayerController();
		PC->FinishSpawning(FTransform::Identity);
	}
	Pawn = World->SpawnActor<AWoTCharacter>(PawnLocation, FRotator::ZeroRotator, Params);
	if (!Test.TestNotNull(TEXT("hook local player"), Player)
		|| !Test.TestNotNull(TEXT("hook gameplay controller"), PC)
		|| !Test.TestNotNull(TEXT("hook character"), Pawn))
	{
		return false;
	}
	PC->SetPlayer(Player);
	PC->Possess(Pawn);
	Pawn->GetCharacterMovement()->SetMovementMode(Mode);
	Frames(2);
	return true;
}

void FPlayerWorld::Frames(int32 Count)
{
	WoTTest::TickWorld(World, Count, 1.0f / 60.0f);
}

AActor* FPlayerWorld::SpawnBlock(const FVector& Center, const FVector& Extent)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Block = World->SpawnActor<AStaticMeshActor>(Center, FRotator::ZeroRotator, Params);
	if (!Block)
	{
		return nullptr;
	}
	UBoxComponent* Box = NewObject<UBoxComponent>(Block);
	Box->SetBoxExtent(Extent);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->SetupAttachment(Block->GetRootComponent());
	Box->RegisterComponent();
	return Block;
}

const FInputActionBinding* FPlayerWorld::FindBinding(FName Name, EInputEvent Edge) const
{
	UInputComponent* Inputs[] = {
		PC ? PC->InputComponent.Get() : nullptr,
		Pawn ? Pawn->InputComponent.Get() : nullptr
	};
	for (UInputComponent* Input : Inputs)
	{
		if (!Input)
		{
			continue;
		}
		for (int32 Index = 0; Index < Input->GetNumActionBindings(); ++Index)
		{
			const FInputActionBinding& Action = Input->GetActionBinding(Index);
			if (Action.GetActionName() == Name && Action.KeyEvent == Edge)
			{
				return &Action;
			}
		}
	}
	return nullptr;
}

bool FPlayerWorld::Binding(FName Name, EInputEvent Edge)
{
	const FInputActionBinding* Action = FindBinding(Name, Edge);
	if (!Action)
	{
		return false;
	}
	Action->ActionDelegate.Execute(FKey());
	return true;
}

bool FPlayerWorld::SetOverlayInput(bool bOpen)
{
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	if (!Seams.SetOverlayInput)
	{
		return false;
	}
	Seams.SetOverlayInput(bOpen, nullptr);
	return true;
}

bool FPlayerWorld::ViewportIgnoresInput() const
{
	return Viewport && Viewport->IgnoreInput();
}

FPlayerWorld::~FPlayerWorld()
{
	if (Player)
	{
		FWoTLocalPlayerHooks::DetachAll(*Player);
	}
	if (Viewport && Scene)
	{
		Viewport->RemoveAssociation(*Scene);
	}
	Scene.Reset();
	Widget.Reset();
	if (Context)
	{
		Context->GameViewport = nullptr;
	}
	WoTTest::DestroySpecWorld(World);
}

FFrontEndScene::FFrontEndScene() = default;

FFrontEndScene::~FFrontEndScene()
{
	if (Materializer)
	{
		Materializer->Shutdown();
	}
}

bool FFrontEndScene::Commit(FAutomationTestBase& Test, UWorld& World, APlayerController& ViewOwner,
	const FVector& Eye, const FRotator& Rotation)
{
	// The synthetic entry scene the materializer's own specs stage
	// (WoTFrontEndSceneMaterializerTests.cpp), its camera at the given eye.
	FWoTFrontEndDescriptor Descriptor;
	Descriptor.MapName = TEXT("SyntheticEntry");
	Descriptor.BspLogicalId = TEXT("cache/bsp/SyntheticEntry/SyntheticEntry");
	Descriptor.WorldLogicalId = TEXT("cache/world/SyntheticEntry");
	Descriptor.CameraLocation = Eye;
	Descriptor.CameraRotation = Rotation;
	Descriptor.HorizontalFov = 90.0f;
	Descriptor.Census.Lights = 39;
	Descriptor.Census.AmbientSounds = 5;
	Descriptor.Census.SplashEmitters = 4;
	Descriptor.Census.RainCylinders = 6;
	Descriptor.Census.LightningActors = 5;
	Descriptor.Census.RandomLightningControllers = 1;
	FWoTFrontEndSceneTestFixture Fixture;
	Fixture.SnapshotIdentity = TEXT("generation-a");
	Fixture.FontSnapshotIdentity = TEXT("generation-a");
	Fixture.BSPSurfaceCount = 8;
	Fixture.WaterSurfaceCount = 1;
	Fixture.PositionalAmbienceCount = 5;
	FWoTFrontEndScenePreferences Preferences;
	Preferences.AudioVolume = 0;
	Materializer = MakeUnique<FWoTFrontEndSceneMaterializer>();
	auto Staged = Materializer->StageForTesting(World, Descriptor, Fixture, Preferences);
	if (Staged.IsError())
	{
		Test.AddError(FString::Printf(TEXT("front-end scene stages: %s"), *Staged.GetError().ToString()));
		return false;
	}
	const FWoTBootstrapStatus Committed = MoveTemp(Staged).GetValue().Commit(&ViewOwner);
	if (Committed.IsError())
	{
		Test.AddError(FString::Printf(TEXT("front-end scene commits: %s"), *Committed.GetError().ToString()));
		return false;
	}
	Root = ViewOwner.GetViewTarget();
	if (Root && Root->ActorHasTag(TEXT("WoTAuthenticatedFrontEndScene")))
	{
		TInlineComponentArray<UCameraComponent*> Cameras;
		Root->GetComponents(Cameras);
		for (UCameraComponent* Candidate : Cameras)
		{
			if (Candidate && Candidate->GetFName() == TEXT("AuthenticatedFrontEndCamera"))
			{
				Camera = Candidate;
			}
		}
	}
	return Test.TestNotNull(TEXT("the committed scene is the view owner's view, with its scene camera"), Camera);
}
}

#endif // WITH_AUTOMATION_TESTS
