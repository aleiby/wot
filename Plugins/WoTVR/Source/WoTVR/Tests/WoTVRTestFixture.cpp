#include "WoTVRTestFixture.h"

#if WITH_AUTOMATION_TESTS

#include "Tests/WoTTestWorld.h"
#include "WoTLocalPlayer.h"
#include "WoTVRModule.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
#include "Slate/SceneViewport.h"
#include "UObject/UnrealType.h"
#include "Widgets/SViewport.h"

namespace WoTVRTest
{
FScope::FScope(bool bActive, EWoTVRStartupMode Startup, TOptional<bool> bFlagVR, TOptional<bool> bFlagNoVR)
	: Session(MakeShared<FWoTVRSyntheticSession>())
{
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	bEnableVR = Settings.bEnableVR;
	StartupMode = Settings.StartupMode;
	Posture = Settings.Posture;
	StandingEyeHeightMeters = Settings.StandingEyeHeightMeters;
	TurnMode = Settings.TurnMode;
	SnapTurnDegrees = Settings.SnapTurnDegrees;
	SmoothTurnDegreesPerSecond = Settings.SmoothTurnDegreesPerSecond;
	ModeSwitchKey = Settings.ModeSwitchKey;
	RecentreKey = Settings.RecentreKey;
	TurnLeftKey = Settings.TurnLeftKey;
	TurnRightKey = Settings.TurnRightKey;
	// Known preferences, in memory only.
	Settings.StartupMode = Startup;
	Settings.Posture = EWoTVRPosture::Standing;
	Settings.StandingEyeHeightMeters = 0.0f;
	Settings.TurnMode = EWoTVRTurnMode::Snap;
	Settings.SnapTurnDegrees = 30.0f;
	Settings.SmoothTurnDegreesPerSecond = 90.0f;
	Settings.ModeSwitchKey = EKeys::F7;
	Settings.RecentreKey = EKeys::F8;
	Settings.TurnLeftKey = EKeys::C;
	Settings.TurnRightKey = EKeys::V;
	FWoTVRModule::FTestOverrides Overrides;
	Overrides.Session = Session;
	Overrides.bActive = bActive;
	Overrides.bFlagVR = bFlagVR.Get(false);
	Overrides.bFlagNoVR = bFlagNoVR.Get(false);
	FWoTVRModule::Get().SetTestOverrides(Overrides);
}

FScope::~FScope()
{
	FWoTVRModule::Get().ClearTestOverrides();
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	Settings.bEnableVR = bEnableVR;
	Settings.StartupMode = StartupMode;
	Settings.Posture = Posture;
	Settings.StandingEyeHeightMeters = StandingEyeHeightMeters;
	Settings.TurnMode = TurnMode;
	Settings.SnapTurnDegrees = SnapTurnDegrees;
	Settings.SmoothTurnDegreesPerSecond = SmoothTurnDegreesPerSecond;
	Settings.ModeSwitchKey = ModeSwitchKey;
	Settings.RecentreKey = RecentreKey;
	Settings.TurnLeftKey = TurnLeftKey;
	Settings.TurnRightKey = TurnRightKey;
}

UWoTVRPlayerSubsystem* Rig(const ULocalPlayer* Player)
{
	return Player ? Player->GetSubsystem<UWoTVRPlayerSubsystem>() : nullptr;
}

const FName FObserver::Owner(TEXT("WoTVRSpecObserver"));

bool FObserver::Attach(ULocalPlayer& InPlayer)
{
	Player = &InPlayer;
	FWoTLocalPlayerConsumer Consumer;
	Consumer.Lifecycle = [this](EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext&)
	{
		Events.Add(Event);
	};
	return FWoTLocalPlayerHooks::Attach(InPlayer, Owner, Consumer);
}

void FObserver::Detach()
{
	if (Player)
	{
		FWoTLocalPlayerHooks::Detach(*Player, Owner);
	}
}

int32 FObserver::Count(EWoTLocalPlayerEvent Event) const
{
	int32 Result = 0;
	for (const EWoTLocalPlayerEvent Seen : Events)
	{
		Result += Seen == Event ? 1 : 0;
	}
	return Result;
}

bool FObserver::Command(EWoTLocalPlayerCommand InCommand, int32 Index)
{
	return Player && FWoTLocalPlayerHooks::Command(*Player, Owner, InCommand, Index);
}

FObserver::~FObserver()
{
	Detach();
}

FMinimalViewInfo FinalView(ULocalPlayer& Player)
{
	// The game's local player (Config/DefaultEngine.ini LocalPlayerClassName):
	// its view point is the camera's, composed by the hooks' view owner. A
	// virtual call through the complete type, so no game symbol is linked.
	FMinimalViewInfo View;
	static_cast<UWoTLocalPlayer&>(Player).GetViewPoint(View);
	return View;
}

void PressKey(APlayerController& PC, const FKey& Key)
{
	if (PC.PlayerInput)
	{
		PC.PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.0f));
	}
}

void ReleaseKey(APlayerController& PC, const FKey& Key)
{
	if (PC.PlayerInput)
	{
		PC.PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f));
	}
}

void SetAxis(APlayerController& PC, const FKey& Key, float Value)
{
	if (PC.PlayerInput)
	{
		PC.PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Axis, Value, 1));
	}
}

UClass* GameClass(const TCHAR* Name)
{
	return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/WoT.%s"), Name));
}

bool SetFloatProperty(UObject* Object, const TCHAR* Name, float Value)
{
	FFloatProperty* Property = Object ? FindFProperty<FFloatProperty>(Object->GetClass(), Name) : nullptr;
	if (!Property)
	{
		return false;
	}
	Property->SetPropertyValue_InContainer(Object, Value);
	return true;
}

bool SetIntProperty(UObject* Object, const TCHAR* Name, int32 Value)
{
	FIntProperty* Property = Object ? FindFProperty<FIntProperty>(Object->GetClass(), Name) : nullptr;
	if (!Property)
	{
		return false;
	}
	Property->SetPropertyValue_InContainer(Object, Value);
	return true;
}

bool GetBoolProperty(const UObject* Object, const TCHAR* Name, bool& OutValue)
{
	const FBoolProperty* Property = Object ? FindFProperty<FBoolProperty>(Object->GetClass(), Name) : nullptr;
	if (!Property)
	{
		return false;
	}
	OutValue = Property->GetPropertyValue_InContainer(Object);
	return true;
}

bool CallFunction(UObject* Object, const TCHAR* Name, void* Params)
{
	UFunction* Function = Object ? Object->FindFunction(Name) : nullptr;
	if (!Function)
	{
		return false;
	}
	Object->ProcessEvent(Function, Params);
	return true;
}

AActor* SpawnBlock(UWorld* World, const FVector& Center, const FVector& Extent)
{
	if (!World)
	{
		return nullptr;
	}
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

bool FMatchWorld::Init(FAutomationTestBase& Test, const TCHAR* GameModeName, bool bWithController)
{
	UClass* ModeClass = GameClass(GameModeName);
	if (!Test.TestNotNull(TEXT("game mode class"), ModeClass))
	{
		return false;
	}
	World = WoTTest::CreateSpecWorld(TSubclassOf<AGameModeBase>(ModeClass));
	if (!Test.TestNotNull(TEXT("VR spec world"), World))
	{
		return false;
	}
	Context = GEngine->GetWorldContextFromWorld(World);
	UGameInstance* Instance = World->GetGameInstance();
	if (!Test.TestNotNull(TEXT("world context"), Context) || !Test.TestNotNull(TEXT("game instance"), Instance))
	{
		return false;
	}
	Viewport = NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass);
	Viewport->Init(*Context, Instance);
	Context->GameViewport = Viewport;
	Widget = SNew(SViewport);
	Scene = Viewport->CreateViewport(Widget);
	Scene->SetInitialSize(FIntPoint(640, 480));
	Viewport->AddAssociation(*Scene);
	FString Error;
	Player = Instance->CreateLocalPlayer(0, Error, false);
	if (!Test.TestNotNull(TEXT("local player"), Player))
	{
		return false;
	}
	Game = World->GetAuthGameMode();
	if (!SpawnBlock(World, FVector(0, 0, -10), FVector(6000, 6000, 10)))
	{
		return false;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (UClass* StartClass = GameClass(TEXT("WoTPlayerStart")))
	{
		World->SpawnActor<AActor>(StartClass, FVector(1000, 0, 48), FRotator::ZeroRotator, Params);
	}
	if (Game)
	{
		SetFloatProperty(Game, TEXT("RespawnDelay"), 0.0f);
	}
	if (!bWithController)
	{
		return true;
	}
	UClass* ControllerClass = GameClass(TEXT("WoTPlayerController"));
	if (!Test.TestNotNull(TEXT("gameplay controller class"), ControllerClass))
	{
		return false;
	}
	// As the game mode spawns a local player's controller: local from BeginPlay.
	PC = World->SpawnActorDeferred<APlayerController>(ControllerClass, FTransform::Identity, nullptr,
		nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Test.TestNotNull(TEXT("gameplay controller"), PC))
	{
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->FinishSpawning(FTransform::Identity);
	PC->SetPlayer(Player);
	PC->InitPlayerState();
	return true;
}

APawn* FMatchWorld::Restart(FAutomationTestBase& Test)
{
	if (!Game || !PC)
	{
		return nullptr;
	}
	Game->RestartPlayer(PC);
	APawn* Pawn = PC->GetPawn();
	Test.TestNotNull(TEXT("the game mode spawns and possesses a pawn"), Pawn);
	return Pawn;
}

void FMatchWorld::Frames(int32 Count)
{
	WoTTest::TickWorld(World, Count, 1.0f / 60.0f);
}

FMatchWorld::~FMatchWorld()
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
}

#endif // WITH_AUTOMATION_TESTS
