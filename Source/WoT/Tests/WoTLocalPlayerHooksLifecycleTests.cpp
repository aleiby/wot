#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "../WoTCharacter.h"
#include "../WoTFrontEndGameMode.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTGameMode.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTTeleporter.h"

#include "Components/BoxComponent.h"
#include "Components/InputComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/ScopeExit.h"
#include "Slate/SceneViewport.h"
#include "UObject/UObjectHash.h"
#include "Widgets/SViewport.h"

#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags HookLifecycleFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName HookLifecycleOwner(TEXT("HookLifecycleSpec"));

FActorSpawnParameters HookLifecycleAlwaysSpawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

bool HookLifecycleHasAction(UInputComponent* Input, FName Action)
{
	if (!Input) return false;
	for (int32 Index = 0; Index < Input->GetNumActionBindings(); ++Index)
	{
		if (Input->GetActionBinding(Index).GetActionName() == Action)
			return true;
	}
	return false;
}

int32 HookLifecycleActorCount(UWorld& World)
{
	int32 Count = 0;
	for (TActorIterator<AActor> It(&World); It; ++It) ++Count;
	return Count;
}

struct FHookLifecycleRecord
{
	EWoTLocalPlayerEvent Event;
	FWoTLocalPlayerContext Context;
	FVector PawnLocation = FVector::ZeroVector;
	FRotator ControlRotation = FRotator::ZeroRotator;
};

struct FHookInputRecord
{
	TWeakObjectPtr<ULocalPlayer> Player;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<UInputComponent> Input;
	bool bHasFire = false;
	bool bHasJump = false;
};

struct FHookLifecycleLog
{
	TArray<FHookLifecycleRecord> Events;
	TArray<FHookInputRecord> Inputs;

	FWoTLocalPlayerConsumer Consumer()
	{
		FWoTLocalPlayerConsumer Result;
		Result.Lifecycle = [this](EWoTLocalPlayerEvent Event,
			const FWoTLocalPlayerContext& Context)
		{
			FHookLifecycleRecord Record{Event, Context};
			if (const APawn* Pawn = Context.Pawn.Get())
				Record.PawnLocation = Pawn->GetActorLocation();
			if (const APlayerController* Controller = Context.Controller.Get())
				Record.ControlRotation = Controller->GetControlRotation();
			Events.Add(Record);
		};
		Result.RegisterInput = [this](ULocalPlayer& Player, UInputComponent* Input)
		{
			FHookInputRecord Record;
			Record.Player = &Player;
			Record.Controller = Player.PlayerController;
			Record.Input = Input;
			Record.bHasFire = HookLifecycleHasAction(Input, TEXT("Fire"));
			Record.bHasJump = HookLifecycleHasAction(Input, TEXT("Jump"));
			Inputs.Add(Record);
		};
		return Result;
	}

	int32 Count(EWoTLocalPlayerEvent Event) const
	{
		int32 Result = 0;
		for (const FHookLifecycleRecord& Record : Events)
			if (Record.Event == Event) ++Result;
		return Result;
	}

	const FHookLifecycleRecord* Last(EWoTLocalPlayerEvent Event) const
	{
		for (int32 Index = Events.Num() - 1; Index >= 0; --Index)
			if (Events[Index].Event == Event) return &Events[Index];
		return nullptr;
	}
};

// Match the input-ready viewport used by WoTLocalPlayerHooksTests.cpp. The
// viewport belongs to the game instance and remains valid when its world changes.
struct FHookLifecycleViewport
{
	FWorldContext* Context = nullptr;
	UGameViewportClient* Viewport = nullptr;
	TSharedPtr<SViewport> Widget;
	TSharedPtr<FSceneViewport> Scene;

	bool Init(FAutomationTestBase& Test, UGameInstance& Instance)
	{
		Context = Instance.GetWorldContext();
		if (!Test.TestNotNull(TEXT("local-player world context"), Context)) return false;
		Viewport = NewObject<UGameViewportClient>(GEngine, GEngine->GameViewportClientClass);
		if (!Test.TestNotNull(TEXT("local-player viewport"), Viewport)) return false;
		Viewport->Init(*Context, &Instance);
		Context->GameViewport = Viewport;
		Widget = SNew(SViewport);
		Scene = Viewport->CreateViewport(Widget);
		Scene->SetInitialSize(FIntPoint(640, 480));
		Viewport->AddAssociation(*Scene);
		return true;
	}

	void Release()
	{
		if (Viewport && Scene) Viewport->RemoveAssociation(*Scene);
		Scene.Reset();
		Widget.Reset();
		if (Context) Context->GameViewport = nullptr;
		Context = nullptr;
		Viewport = nullptr;
	}

	~FHookLifecycleViewport() { Release(); }
};

struct FHookLifecycleWorld
{
	UWorld* World = nullptr;
	ULocalPlayer* Player = nullptr;
	FHookLifecycleViewport Viewport;

	bool Init(FAutomationTestBase& Test, TSubclassOf<AGameModeBase> Mode)
	{
		World = WoTTest::CreateSpecWorld(Mode);
		if (!Test.TestNotNull(TEXT("lifecycle world"), World)) return false;
		UGameInstance* Instance = World->GetGameInstance();
		if (!Test.TestNotNull(TEXT("lifecycle game instance"), Instance)
			|| !Viewport.Init(Test, *Instance)) return false;
		FString Error;
		Player = Instance->CreateLocalPlayer(0, Error, false);
		return Test.TestNotNull(TEXT("local player"), Player)
			&& Test.TestTrue(TEXT("local-player creation has no error"), Error.IsEmpty());
	}

	~FHookLifecycleWorld()
	{
		if (Player) FWoTLocalPlayerHooks::DetachAll(*Player);
		Viewport.Release();
		WoTTest::DestroySpecWorld(World);
	}
};

bool HookLifecyclePrepareRespawn(FAutomationTestBase& Test, UWorld& World)
{
	AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(
		FVector(0, 0, -10), FRotator::ZeroRotator, HookLifecycleAlwaysSpawn());
	if (!Test.TestNotNull(TEXT("respawn floor"), Floor)) return false;
	UBoxComponent* Box = NewObject<UBoxComponent>(Floor);
	Box->SetBoxExtent(FVector(6000, 6000, 10));
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->SetupAttachment(Floor->GetRootComponent());
	Box->RegisterComponent();
	return Test.TestNotNull(TEXT("respawn start"), World.SpawnActor<AWoTPlayerStart>(
		FVector(1000, 0, 48), FRotator::ZeroRotator, HookLifecycleAlwaysSpawn()));
}

#if WITH_EDITOR
bool HookLifecycleInitTravelInstance(FAutomationTestBase& Test,
	UE::Net::FTestWorldInstance& Instance)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!Test.TestNotNull(TEXT("travel game instance"), GameInstance)) return false;
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_Standalone;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	int32 PIEInstance = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
	UE::Net::FScopedNetTestPIERestoration Restoration;
	UE::SetPlayInEditorID(-1);
	const FGameInstancePIEResult Initialized =
		GameInstance->InitializeForPlayInEditor(PIEInstance + 1, Parameters);
	if (!Test.TestTrue(TEXT("travel instance initializes"), Initialized.IsSuccess()))
	{
		GameInstance->Shutdown();
		GameInstance->RemoveFromRoot();
		return false;
	}
	Instance.GameInstance = GameInstance;
	return true;
}

bool HookLifecycleTravel(FAutomationTestBase& Test,
	UE::Net::FTestWorldInstance& Instance, const TCHAR* URL, UClass* ModeClass)
{
	const TWeakObjectPtr<UWorld> PreviousWorld = Instance.GetWorld();
	UE::Net::FScopedNetTestPIERestoration Restoration;
	UE::SetPlayInEditorID(-1);
	if (!Test.TestTrue(TEXT("server travel accepted"),
		PreviousWorld.IsValid() && PreviousWorld->ServerTravel(URL, true))) return false;
	for (int32 Frame = 0; Frame < 180; ++Frame)
	{
		Instance.Tick(1.0f / 60.0f);
		++GFrameCounter;
		UWorld* CurrentWorld = Instance.GetWorld();
		if (CurrentWorld && CurrentWorld != PreviousWorld.Get()
			&& CurrentWorld->HasBegunPlay() && CurrentWorld->GetAuthGameMode()
			&& CurrentWorld->GetAuthGameMode()->IsA(ModeClass)) return true;
	}
	Test.AddError(TEXT("travel did not replace the world with the requested game mode"));
	return false;
}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFrontEndInputLifecycleSpec,
	"WoT.VR.Hooks.H1.FrontEndControllerInputOrderAndDetach", HookLifecycleFlags)

bool FWoTHookFrontEndInputLifecycleSpec::RunTest(const FString&)
{
	FHookLifecycleLog Log;
	FHookLifecycleWorld Fixture;
	if (!Fixture.Init(*this, AWoTFrontEndGameMode::StaticClass())) return false;
	TestFalse(TEXT("creating a local player leaves the registry empty"),
		FWoTLocalPlayerHooks::HasConsumers(Fixture.Player));
	TestNull(TEXT("the local player starts without a controller"),
		Fixture.Player->PlayerController.Get());
	TArray<UObject*> OriginalPlayerObjects;
	GetObjectsWithOuter(Fixture.Player, OriginalPlayerObjects, EGetObjectsFlags::IncludeNestedObjects);
	const int32 OriginalActorCount = HookLifecycleActorCount(*Fixture.World);
	TSharedPtr<int32> ConsumerLifetime = MakeShared<int32>(0);
	const TWeakPtr<int32> WeakConsumerLifetime = ConsumerLifetime;
	FWoTLocalPlayerConsumer Consumer = Log.Consumer();
	Consumer.AimRotation = [ConsumerLifetime](FRotator&) { return false; };
	if (!TestTrue(TEXT("attach before controller or pawn exists"),
		FWoTLocalPlayerHooks::Attach(*Fixture.Player, HookLifecycleOwner, Consumer)))
		return false;
	TestTrue(TEXT("Attached is the first event"),
		Log.Events.Num() > 0 && Log.Events[0].Event == EWoTLocalPlayerEvent::Attached);
	Consumer = FWoTLocalPlayerConsumer();
	ConsumerLifetime.Reset();
	TestTrue(TEXT("registry retains the attached consumer's resources"), WeakConsumerLifetime.IsValid());
	TestEqual(TEXT("attaching a consumer creates no world actors"),
		HookLifecycleActorCount(*Fixture.World), OriginalActorCount);
	TArray<UObject*> AttachedPlayerObjects;
	GetObjectsWithOuter(Fixture.Player, AttachedPlayerObjects, EGetObjectsFlags::IncludeNestedObjects);
	TestEqual(TEXT("attaching creates no local-player-owned objects"),
		AttachedPlayerObjects.Num(), OriginalPlayerObjects.Num());
	TestTrue(TEXT("unbound attachment registers input with a null component"),
		Log.Inputs.Num() == 1 && !Log.Inputs[0].Input.IsValid()
		&& Log.Inputs[0].Player.Get() == Fixture.Player);

	AWoTFrontEndPlayerController* FrontEnd =
		Fixture.World->SpawnActor<AWoTFrontEndPlayerController>(HookLifecycleAlwaysSpawn());
	if (!TestNotNull(TEXT("real front-end controller"), FrontEnd)) return false;
	FrontEnd->SetPlayer(Fixture.Player);
	TestNull(TEXT("the front end has no pawn"), FrontEnd->GetPawn());
	TestNull(TEXT("the front end has no HUD"), FrontEnd->GetHUD());
	const FHookLifecycleRecord* FrontEndEvent = Log.Last(EWoTLocalPlayerEvent::FrontEnd);
	TestTrue(TEXT("front-end notification identifies the pawnless local controller"),
		FrontEndEvent && FrontEndEvent->Context.Player.Get() == Fixture.Player
		&& FrontEndEvent->Context.Controller.Get() == FrontEnd
		&& FrontEndEvent->Context.World.Get() == Fixture.World
		&& !FrontEndEvent->Context.Pawn.IsValid());

	AWoTPlayerController* Controller =
		Fixture.World->SpawnActor<AWoTPlayerController>(HookLifecycleAlwaysSpawn());
	if (!TestNotNull(TEXT("replacement gameplay controller"), Controller)) return false;
	Controller->SetPlayer(Fixture.Player);
	bool bSawEarlyGameplayInput = false;
	for (const FHookInputRecord& Input : Log.Inputs)
	{
		if (Input.Controller.Get() == Controller && Input.Input.IsValid())
		{
			bSawEarlyGameplayInput = true;
			TestFalse(TEXT("consumer runs before the gameplay Fire bindings"), Input.bHasFire);
			TestFalse(TEXT("consumer runs before the gameplay Jump bindings"), Input.bHasJump);
		}
	}
	TestTrue(TEXT("replacement creates an early input-registration opportunity"), bSawEarlyGameplayInput);
	TestTrue(TEXT("ordinary Fire is bound after the consumer returns"),
		HookLifecycleHasAction(Controller->InputComponent, TEXT("Fire")));
	TestTrue(TEXT("ordinary Jump is bound after the consumer returns"),
		HookLifecycleHasAction(Controller->InputComponent, TEXT("Jump")));
	const FHookLifecycleRecord* Changed = Log.Last(EWoTLocalPlayerEvent::ControllerChanged);
	TestTrue(TEXT("replacement retains the original local-player registration"),
		Changed && Changed->Context.Player.Get() == Fixture.Player
		&& Changed->Context.Controller.Get() == Controller
		&& FWoTLocalPlayerHooks::HasConsumers(Fixture.Player));
	FrontEnd->Destroy();
	TestTrue(TEXT("destroying the replaced controller preserves the registration"),
		FWoTLocalPlayerHooks::HasConsumers(Fixture.Player));

	TArray<UObject*> ObjectsBeforeDetach;
	GetObjectsWithOuter(Fixture.Player, ObjectsBeforeDetach, EGetObjectsFlags::IncludeNestedObjects);
	const int32 ActorsBeforeDetach = HookLifecycleActorCount(*Fixture.World);
	FWoTLocalPlayerHooks::Detach(*Fixture.Player, HookLifecycleOwner);
	TestEqual(TEXT("one detach callback"), Log.Count(EWoTLocalPlayerEvent::Detached), 1);
	TestFalse(TEXT("last detach removes the registration"),
		FWoTLocalPlayerHooks::HasConsumers(Fixture.Player));
	TestFalse(TEXT("last detach releases every captured consumer resource"), WeakConsumerLifetime.IsValid());
	TestEqual(TEXT("detach preserves the ordinary world actors"),
		HookLifecycleActorCount(*Fixture.World), ActorsBeforeDetach);
	TArray<UObject*> RemainingPlayerObjects;
	GetObjectsWithOuter(Fixture.Player, RemainingPlayerObjects, EGetObjectsFlags::IncludeNestedObjects);
	TestEqual(TEXT("hooks leave no extra local-player-owned objects"),
		RemainingPlayerObjects.Num(), ObjectsBeforeDetach.Num());
	for (UObject* Object : RemainingPlayerObjects)
		TestTrue(TEXT("detach preserves ordinary local-player objects"), ObjectsBeforeDetach.Contains(Object));
	const int32 DetachedEventCount = Log.Events.Num();
	Controller->PreClientTravel(TEXT("/Engine/Maps/Entry"), TRAVEL_Absolute, false);
	TestEqual(TEXT("detached consumer receives no later travel callback"),
		Log.Events.Num(), DetachedEventCount);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookGameplayLifecycleSpec,
	"WoT.VR.Hooks.H1.PossessionDeathRespawnTeleportAndMatchEnd", HookLifecycleFlags)

bool FWoTHookGameplayLifecycleSpec::RunTest(const FString&)
{
	FHookLifecycleLog Log;
	FHookLifecycleLog DeadObserver;
	FHookLifecycleWorld Fixture;
	if (!Fixture.Init(*this, AWoTGameMode::StaticClass())
		|| !HookLifecyclePrepareRespawn(*this, *Fixture.World)) return false;
	AWoTGameMode* Game = Fixture.World->GetAuthGameMode<AWoTGameMode>();
	AWoTPlayerController* Controller =
		Fixture.World->SpawnActor<AWoTPlayerController>(HookLifecycleAlwaysSpawn());
	if (!TestNotNull(TEXT("game mode"), Game)
		|| !TestNotNull(TEXT("gameplay controller"), Controller)) return false;
	Controller->SetPlayer(Fixture.Player);
	Controller->InitPlayerState();
	if (!TestTrue(TEXT("attach before gameplay possession"),
		FWoTLocalPlayerHooks::Attach(*Fixture.Player, HookLifecycleOwner, Log.Consumer())))
		return false;
	Game->RestartPlayer(Controller);
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Controller->GetPawn());
	if (!TestNotNull(TEXT("game rules spawn and possess the first pawn"), Pawn)) return false;
	const FHookLifecycleRecord* Possessed = Log.Last(EWoTLocalPlayerEvent::Possessed);
	TestTrue(TEXT("possession supplies the real gameplay pawn"),
		Possessed && Possessed->Context.Pawn.Get() == Pawn
		&& Possessed->Context.Controller.Get() == Controller);
	Controller->UnPossess();
	TestTrue(TEXT("unpossession is observed"), Log.Count(EWoTLocalPlayerEvent::Unpossessed) > 0);
	Controller->Possess(Pawn);

	const FVector Destination(1700, 300, 100);
	const int32 TeleportsBefore = Log.Count(EWoTLocalPlayerEvent::Teleported);
	TestTrue(TEXT("teleport collision probe succeeds"),
		Pawn->TeleportTo(Destination, FRotator(0, 90, 0), true, true));
	WoTTest::TickWorld(Fixture.World, 1);
	TestEqual(TEXT("a teleport probe is not a relocation notification"),
		Log.Count(EWoTLocalPlayerEvent::Teleported), TeleportsBefore);
	// The engine's probe places the pawn at the destination it tests; the real
	// teleport goes on from there.
	const FVector Arrival = Pawn->GetActorLocation() + FVector(0, 1200, 0);
	TestTrue(TEXT("real teleport succeeds"),
		Pawn->TeleportTo(Arrival, FRotator(0, 90, 0), false, true));
	TestEqual(TEXT("the relocation is reported after it completes, not inside it"),
		Log.Count(EWoTLocalPlayerEvent::Teleported), TeleportsBefore);
	WoTTest::TickWorld(Fixture.World, 1);
	const FHookLifecycleRecord* Teleported = Log.Last(EWoTLocalPlayerEvent::Teleported);
	TestEqual(TEXT("the next player tick reports the teleport once"),
		Log.Count(EWoTLocalPlayerEvent::Teleported), TeleportsBefore + 1);
	TestTrue(TEXT("teleport notification observes the pawn at its new anchor"),
		Teleported && Teleported->Context.Pawn.Get() == Pawn
		&& Teleported->PawnLocation.Equals(Arrival, 5.0));

	// A map teleporter relocates through its own path and may turn the player;
	// the report comes after the turn, so a consumer reads the exit heading.
	AWoTTeleporter* Exit = Fixture.World->SpawnActor<AWoTTeleporter>(
		FVector(2600, 300, 100), FRotator(0, 135, 0), HookLifecycleAlwaysSpawn());
	if (!TestNotNull(TEXT("real teleporter"), Exit)) return false;
	Exit->bChangesYaw = true;
	TestTrue(TEXT("the teleporter accepts the pawn"), Exit->Accept(Pawn));
	WoTTest::TickWorld(Fixture.World, 1);
	const FHookLifecycleRecord* Arrived = Log.Last(EWoTLocalPlayerEvent::Teleported);
	TestEqual(TEXT("a teleporter relocation is reported once"),
		Log.Count(EWoTLocalPlayerEvent::Teleported), TeleportsBefore + 2);
	TestTrue(TEXT("the teleporter report sees the pawn at the exit"),
		Arrived && Arrived->PawnLocation.Equals(Exit->GetActorLocation(), 5.0));
	TestTrue(TEXT("the teleporter report sees the exit heading"),
		Arrived && FMath::IsNearlyEqual(FRotator::NormalizeAxis(Arrived->ControlRotation.Yaw), 135.0, 0.01));

	Game->RespawnDelay = 0.0f;
	Pawn->Health = 25;
	Pawn->MaxHealth = 100;
	const int32 DeathsBefore = Log.Count(EWoTLocalPlayerEvent::Death);
	const int32 RespawnsBefore = Log.Count(EWoTLocalPlayerEvent::Respawn);
	Pawn->TakeDamage(26.0f, FDamageEvent(), nullptr, nullptr);
	TestFalse(TEXT("ordinary damage kills the pawn"), Pawn->IsAlive());
	TestNull(TEXT("death releases possession"), Controller->GetPawn());
	TestEqual(TEXT("death produces one lifecycle boundary"),
		Log.Count(EWoTLocalPlayerEvent::Death), DeathsBefore + 1);
	TestTrue(TEXT("a second observer can attach while dead"),
		FWoTLocalPlayerHooks::Attach(*Fixture.Player, TEXT("DeadObserver"), DeadObserver.Consumer()));
	FWoTLocalPlayerHooks::Detach(*Fixture.Player, TEXT("DeadObserver"));
	TestTrue(TEXT("detaching while dead retains the first observer"),
		FWoTLocalPlayerHooks::HasConsumers(Fixture.Player));
	TestEqual(TEXT("dead observer receives its detach"), DeadObserver.Count(EWoTLocalPlayerEvent::Detached), 1);

	// A press comes at least a frame after the death (the respawn gate compares
	// the world time against a float stamp taken at the death).
	WoTTest::TickWorld(Fixture.World, 1);
	Controller->Fire();
	AWoTCharacter* Respawned = Cast<AWoTCharacter>(Controller->GetPawn());
	if (!TestNotNull(TEXT("existing Fire command respawns the player"), Respawned)) return false;
	TestTrue(TEXT("respawn installs a replacement pawn"), Respawned != Pawn);
	TestEqual(TEXT("respawn produces one lifecycle boundary"),
		Log.Count(EWoTLocalPlayerEvent::Respawn), RespawnsBefore + 1);
	const FHookLifecycleRecord* RespawnEvent = Log.Last(EWoTLocalPlayerEvent::Respawn);
	TestTrue(TEXT("respawn provides the replacement anchor"),
		RespawnEvent && RespawnEvent->Context.Pawn.Get() == Respawned
		&& RespawnEvent->Context.Player.Get() == Fixture.Player);
	Game->EndGame(TEXT("lifecycle spec"));
	TestTrue(TEXT("game rules end the match"), Game->bGameEnded);
	TestEqual(TEXT("match end reaches the local consumer once"),
		Log.Count(EWoTLocalPlayerEvent::MatchEnded), 1);
	Game->EndGame(TEXT("lifecycle spec repeated"));
	TestEqual(TEXT("repeated match-end requests do not duplicate the boundary"),
		Log.Count(EWoTLocalPlayerEvent::MatchEnded), 1);
	FWoTLocalPlayerHooks::Detach(*Fixture.Player, HookLifecycleOwner);
	TestFalse(TEXT("detach after match end removes the registry"),
		FWoTLocalPlayerHooks::HasConsumers(Fixture.Player));
	return !HasAnyErrors();
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookFrontEndRoundTripTravelSpec,
	"WoT.VR.Hooks.H1.FrontEndMatchRoundTripTravel", HookLifecycleFlags)

bool FWoTHookFrontEndRoundTripTravelSpec::RunTest(const FString&)
{
	FHookLifecycleLog Log;
	UE::Net::FScopedTestSettings Settings;
	UE::Net::FTestWorldInstance Instance(true);
	if (!HookLifecycleInitTravelInstance(*this, Instance)) return false;
	FHookLifecycleViewport Viewport;
	if (!Viewport.Init(*this, *Instance.GameInstance)) return false;
	FString PlayerError;
	ULocalPlayer* Player = Instance.GameInstance->CreateLocalPlayer(0, PlayerError, false);
	if (!TestNotNull(TEXT("persistent travel local player"), Player)) return false;
	ON_SCOPE_EXIT { FWoTLocalPlayerHooks::DetachAll(*Player); };
	{
		UE::Net::FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FString Error;
		const FURL URL(nullptr,
			TEXT("/Engine/Maps/Entry?Game=/Script/WoT.WoTFrontEndGameMode"), TRAVEL_Absolute);
		Instance.bIsLoaded = GEngine->Browse(*Instance.GetWorldContext(), URL, Error)
			== EBrowseReturnVal::Success;
		if (!TestTrue(FString::Printf(TEXT("front end loads: %s"), *Error), Instance.IsLoaded()))
			return false;
	}
	AWoTFrontEndPlayerController* FrontEnd = Cast<AWoTFrontEndPlayerController>(Player->PlayerController);
	if (!TestNotNull(TEXT("loaded front-end controller"), FrontEnd)) return false;
	TestNull(TEXT("loaded front end is pawnless"), FrontEnd->GetPawn());
	TestNull(TEXT("loaded front end is HUD-less"), FrontEnd->GetHUD());
	if (!TestTrue(TEXT("attach in the loaded front end"),
		FWoTLocalPlayerHooks::Attach(*Player, HookLifecycleOwner, Log.Consumer()))) return false;
	TestEqual(TEXT("front-end attachment is announced once"), Log.Count(EWoTLocalPlayerEvent::Attached), 1);
	TestTrue(TEXT("front-end state is immediately available"), Log.Count(EWoTLocalPlayerEvent::FrontEnd) > 0);

	if (!HookLifecycleTravel(*this, Instance,
		TEXT("/Engine/Maps/Entry?Game=/Script/WoT.WoTGameMode"), AWoTGameMode::StaticClass())) return false;
	AWoTPlayerController* Controller = Cast<AWoTPlayerController>(Player->PlayerController);
	if (!TestNotNull(TEXT("travel replaces the controller with the gameplay class"), Controller)) return false;
	TestTrue(TEXT("game-instance local-player identity survives the first travel"),
		Instance.GameInstance->GetFirstGamePlayer() == Player && Controller->GetLocalPlayer() == Player);
	TestEqual(TEXT("leaving the front end is announced once"), Log.Count(EWoTLocalPlayerEvent::Travel), 1);
	TestTrue(TEXT("registration survives front-end world destruction"), FWoTLocalPlayerHooks::HasConsumers(Player));
	if (!HookLifecyclePrepareRespawn(*this, *Instance.GetWorld())) return false;
	AWoTGameMode* Game = Instance.GetWorld()->GetAuthGameMode<AWoTGameMode>();
	AWoTCharacter* Pawn = Cast<AWoTCharacter>(Controller->GetPawn());
	if (!Pawn)
	{
		// Entry supplies no authored game start. Install one and use the real
		// restart path if login could not seat this game's character there.
		Game->RestartPlayer(Controller);
		Pawn = Cast<AWoTCharacter>(Controller->GetPawn());
	}
	if (!TestNotNull(TEXT("travel destination possesses a gameplay pawn"), Pawn)) return false;
	const FHookLifecycleRecord* Possessed = Log.Last(EWoTLocalPlayerEvent::Possessed);
	TestTrue(TEXT("new possession context belongs to the destination world"),
		Possessed && Possessed->Context.World.Get() == Instance.GetWorld()
		&& Possessed->Context.Controller.Get() == Controller && Possessed->Context.Pawn.Get() == Pawn);
	const int32 TeleportsBefore = Log.Count(EWoTLocalPlayerEvent::Teleported);
	TestTrue(TEXT("the traveled player can teleport"),
		Pawn->TeleportTo(Pawn->GetActorLocation() + FVector(1500, 0, 0), FRotator::ZeroRotator, false, true));
	auto TickTraveled = [&Instance]()
	{
		UE::Net::FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		Instance.Tick(1.0f / 60.0f);
		++GFrameCounter;
	};
	TickTraveled();
	TestEqual(TEXT("the front-end consumer observes a gameplay teleport"),
		Log.Count(EWoTLocalPlayerEvent::Teleported), TeleportsBefore + 1);
	Game->RespawnDelay = 0.0f;
	Pawn->Health = 25;
	Pawn->MaxHealth = 100;
	const int32 DeathsBefore = Log.Count(EWoTLocalPlayerEvent::Death);
	const int32 RespawnsBefore = Log.Count(EWoTLocalPlayerEvent::Respawn);
	Pawn->TakeDamage(26.0f, FDamageEvent(), nullptr, nullptr);
	TestTrue(TEXT("the front-end consumer observes death in the new world"),
		Log.Count(EWoTLocalPlayerEvent::Death) > DeathsBefore);
	// A press comes at least a frame after the death (the respawn gate compares
	// the world time against a float stamp taken at the death).
	TickTraveled();
	Controller->Fire();
	TestTrue(TEXT("the existing respawn command replaces the traveled player's pawn"),
		Controller->GetPawn() && Controller->GetPawn() != Pawn);
	TestTrue(TEXT("the front-end consumer observes the replacement anchor"),
		Log.Count(EWoTLocalPlayerEvent::Respawn) > RespawnsBefore);
	const int32 TravelsBeforeReturn = Log.Count(EWoTLocalPlayerEvent::Travel);
	const int32 FrontEndsBeforeReturn = Log.Count(EWoTLocalPlayerEvent::FrontEnd);
	if (!HookLifecycleTravel(*this, Instance,
		TEXT("/Engine/Maps/Entry?Game=/Script/WoT.WoTFrontEndGameMode"), AWoTFrontEndGameMode::StaticClass())) return false;
	AWoTFrontEndPlayerController* Returned = Cast<AWoTFrontEndPlayerController>(Player->PlayerController);
	if (!TestNotNull(TEXT("return travel restores the front-end controller class"), Returned)) return false;
	TestTrue(TEXT("the same local player owns the returned front end"),
		Instance.GameInstance->GetFirstGamePlayer() == Player && Returned->GetLocalPlayer() == Player);
	TestNull(TEXT("return discards the gameplay pawn"), Returned->GetPawn());
	TestNull(TEXT("return does not create a HUD"), Returned->GetHUD());
	TestEqual(TEXT("the return travel is announced once"),
		Log.Count(EWoTLocalPlayerEvent::Travel), TravelsBeforeReturn + 1);
	TestTrue(TEXT("return front-end state is observed"), Log.Count(EWoTLocalPlayerEvent::FrontEnd) > FrontEndsBeforeReturn);
	const FHookLifecycleRecord* ReturnedEvent = Log.Last(EWoTLocalPlayerEvent::FrontEnd);
	TestTrue(TEXT("returned context has only the new neutral anchor's world and controller"),
		ReturnedEvent && ReturnedEvent->Context.World.Get() == Instance.GetWorld()
		&& ReturnedEvent->Context.Controller.Get() == Returned && !ReturnedEvent->Context.Pawn.IsValid());
	TestEqual(TEXT("round trip never reattaches the consumer"), Log.Count(EWoTLocalPlayerEvent::Attached), 1);
	TestEqual(TEXT("world replacement never detaches the consumer"), Log.Count(EWoTLocalPlayerEvent::Detached), 0);
	FWoTLocalPlayerHooks::Detach(*Player, HookLifecycleOwner);
	TestFalse(TEXT("detach in the returned front end removes registration"), FWoTLocalPlayerHooks::HasConsumers(Player));
	return !HasAnyErrors();
}
#endif // WITH_EDITOR

#endif // WITH_AUTOMATION_TESTS
