#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTCreatureData.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "../WoTCarcass.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCreatureChunk.h"
#include "../WoTGameMode.h"
#include "../WoTGameEngine.h"
#include "../WoTGameInstance.h"
#include "../WoTFrontEnd.h"
#include "../WoTFactionCharacters.h"
#include "../WoTInGameSlate.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTRandom.h"
#include "../WoTSessionSubsystem.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Payload/WoTPayloadActivation.h"
#include "WoTLaunchEngineHooks.h"
#include "WoTTestAIClasses.h"

#include "Components/BoxComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Engine.h"
#include "Engine/ChildConnection.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "Net/OnlineEngineInterface.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSubsystemUtils.h"

#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags Fix7_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters Fix7_Spawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

AStaticMeshActor* Fix7_SpawnBox(UWorld& World, const FVector& Location,
	const FVector& Extent, const FRotator& Rotation)
{
	AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(
		Location, Rotation, Fix7_Spawn());
	if (!Actor) return nullptr;
	UBoxComponent* Box = NewObject<UBoxComponent>(Actor, TEXT("Fix7_Box"));
	Box->SetBoxExtent(Extent);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Actor->SetRootComponent(Box);
	Box->RegisterComponent();
	Actor->SetActorLocationAndRotation(Location, Rotation, false, nullptr,
		ETeleportType::TeleportPhysics);
	return Actor;
}

AWoTCreatureChunk* Fix7_ChunkUpPart(UWorld& World, const FVector& Location)
{
	TSet<TWeakObjectPtr<AWoTCreatureChunk>> Before;
	for (TActorIterator<AWoTCreatureChunk> It(&World); It; ++It)
	{
		Before.Add(*It);
	}
	AWoTCarcass* Source = World.SpawnActor<AWoTCarcass>(
		Location, FRotator::ZeroRotator, Fix7_Spawn());
	if (!Source) return nullptr;
	Source->CarcassTypeName = NAME_None;
	WoTRandom::Seed(0xF107);
	Source->ChunkUp(100.0f);
	AWoTCreatureChunk* Selected = nullptr;
	TArray<AWoTCreatureChunk*> NewChunks;
	for (TActorIterator<AWoTCreatureChunk> It(&World); It; ++It)
	{
		if (Before.Contains(*It)) continue;
		NewChunks.Add(*It);
		if (!Selected || It->Mass > Selected->Mass) Selected = *It;
	}
	for (AWoTCreatureChunk* Chunk : NewChunks)
	{
		if (Chunk != Selected) Chunk->Destroy();
	}
	return Selected;
}

void Fix7_ZeroRotatorAxis(double& Degrees)
{
	const double Wrapped = FRotator::ClampAxis(Degrees);
	Degrees = Wrapped < 90.0 || Wrapped >= 270.0 ? 0.0 : 180.0;
}

bool Fix7_PublishWorldAuthority()
{
	ResetWoTPayloadActivationForTesting();
	return !PublishWoTPayloadBootstrapActivationForTesting().IsError()
		&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
}

FString Fix7_OptionsForURL(const FURL& URL)
{
	FString Options;
	for (const FString& Option : URL.Op)
	{
		Options += TEXT("?") + Option;
	}
	return Options;
}

class FFix7_TravelLogCapture final : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type,
		const FName& Category) override
	{
		Text += Category.ToString();
		Text += TEXT(": ");
		Text += Message;
		Text += TEXT("\n");
	}

	FString Text;
};

struct FFix7_GameEngineScope
{
	FFix7_GameEngineScope()
		: PriorEngine(GEngine), PriorWorld(GWorld), bPriorPIE(GIsPlayInEditorWorld)
	{
		Engine = NewObject<UWoTGameEngine>(GetTransientPackage());
		Engine->AddToRoot();
		Engine->WorldSettingsClass = PriorEngine->WorldSettingsClass;
		// A NewObject engine never runs InitializeObjectReferences. Pawns spawned
		// while it is GEngine create physics bodies, and FBodyInstance resolves
		// their material through GEngine->DefaultPhysMaterial, so borrow the
		// editor engine's defaults.
		Engine->DefaultPhysMaterial = PriorEngine->DefaultPhysMaterial;
		Engine->DefaultDestructiblePhysMaterial =
			PriorEngine->DefaultDestructiblePhysMaterial;
		for (const FWorldContext& PriorContext : PriorEngine->GetWorldContexts())
		{
			UWorld* World = PriorContext.World();
			if (!World) continue;
			FWorldContext& Borrowed =
				Engine->CreateNewWorldContext(PriorContext.WorldType);
			Borrowed.PIEInstance = PriorContext.PIEInstance;
			Borrowed.SetCurrentWorld(World);
			BorrowedWorlds.Add(World);
		}
		GEngine = Engine;
	}

	~FFix7_GameEngineScope()
	{
		if (Engine)
		{
			TArray<FName> Handles;
			for (const FWorldContext& Context : Engine->GetWorldContexts())
			{
				Handles.Add(Context.ContextHandle);
			}
			for (const FName Handle : Handles)
			{
				FWorldContext* Context =
					Engine->GetWorldContextFromHandle(Handle);
				if (!Context) continue;
				UWorld* World = Context->World();
				if (World && BorrowedWorlds.Contains(World))
				{
					Engine->DestroyWorldContext(World);
					continue;
				}
				if (World && World->HasBegunPlay())
				{
					World->BeginTearingDown();
					World->EndPlay(EEndPlayReason::Quit);
				}
				if (Context->OwningGameInstance)
				{
					Context->OwningGameInstance->Shutdown();
				}
				if (World && !World->HasAnyFlags(
					RF_BeginDestroyed | RF_FinishDestroyed))
				{
					Engine->ShutdownWorldNetDriver(World);
					Engine->DestroyWorldContext(World);
					World->DestroyWorld(false);
				}
			}
			// UEngine::FinishDestroy unconditionally clears the process GEngine,
			// even after this scope restores the editor engine. Keep this empty
			// test engine rooted for the automation process lifetime so a later
			// collection cannot invalidate unrelated network-world specs.
		}
		GEngine = PriorEngine;
		GWorld = PriorWorld;
		GIsPlayInEditorWorld = bPriorPIE;
	}

	UWoTGameEngine* Engine = nullptr;
	UEngine* PriorEngine = nullptr;
	UWorld* PriorWorld = nullptr;
	bool bPriorPIE = false;
	TSet<UWorld*> BorrowedWorlds;
};

int32 Fix7_UnusedPIEInstance()
{
	static int32 LastInstance = INDEX_NONE;
	int32 Maximum = LastInstance;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		Maximum = FMath::Max(Maximum, Context.PIEInstance);
	}
	LastInstance = Maximum + 1;
	return LastInstance;
}

FUniqueNetIdRepl Fix7_UniqueId(const TCHAR* StableId)
{
	const FUniqueNetIdRef UniqueId = FUniqueNetIdString::Create(StableId,
		UOnlineEngineInterface::Get()->GetDefaultOnlineSubsystemName());
	return FUniqueNetIdRepl(UniqueId);
}

AWoTPlayerController* Fix7_SpawnRemotePlayer(UWorld& World,
	const TCHAR* Name, const FUniqueNetIdRepl& UniqueId)
{
	AWoTPlayerController* Controller =
		World.SpawnActor<AWoTPlayerController>();
	AWoTPlayerState* State = Controller
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!Controller || !State) return nullptr;
	State->SetPlayerName(Name);
	State->SetUniqueId(UniqueId);
	UChildConnection* Connection = NewObject<UChildConnection>(GEngine);
	Connection->PlayerController = Controller;
	Controller->Player = Connection;
	return Controller;
}

AWoTNPCCharacter* Fix7_SpawnCreature(UWorld& World, FName Row,
	const FVector& Location)
{
	AWoTNPCCharacter* NPC = WoTCreatureData::SpawnCreature(World, Row, Location,
		1, true, AWoTTestNPCCharacter::StaticClass());
	if (NPC)
	{
		NPC->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
	return NPC;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7NPCReturnPathFailureSpec,
	"WoT.Parity.Fix7.NPCReturnPathFailure", Fix7_Flags)
bool FFix7NPCReturnPathFailureSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* NPC = World
		? Fix7_SpawnCreature(*World, TEXT("Trolloc"), FVector(0, 0, 100))
		: nullptr;
	AWoTNPCController* Controller = NPC
		? Cast<AWoTNPCController>(NPC->GetController()) : nullptr;
	UWoTTestBrainComponent* Brain = NPC
		? Cast<UWoTTestBrainComponent>(NPC->Brain) : nullptr;
	if (TestNotNull(TEXT("production NPC"), NPC)
		&& TestNotNull(TEXT("real NPC controller"), Controller)
		&& TestNotNull(TEXT("forced navigation brain"), Brain))
	{
		Brain->bForceReachable = false;
		Brain->bForcePathable = true;
		Brain->SetWaitingGoal(FVector(1000, 0, 100), FRotator::ZeroRotator);
		Controller->TransitionOnGoalPriority(true);
		Controller->Tick(0.05f);

		TestEqual(TEXT("a classified path failure remains in the return state"),
			Controller->GetBrainState(), EWoTBrainState::Returning);
		TestTrue(TEXT("a classified path failure preserves the waiting post"),
			Brain->GetGoal(EWoTGoalIndex::Waiting).GoalVector.Equals(
				FVector(1000, 0, 100), 0.01f));
		TestFalse(TEXT("failed path request leaves no locomotion intent"),
			Controller->HasActiveMovementIntentForAnimation());
		NPC->Tick(0.0f);
		TestTrue(TEXT("the failed return does not animate locomotion"),
			NPC->RepAnim.Sequence != TEXT("Walk")
			&& NPC->RepAnim.Sequence != TEXT("Run"));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7CreatureChunkDitchSpec,
	"WoT.Parity.Fix7.CreatureChunkDitchClearThirdSweep", Fix7_Flags)
bool FFix7CreatureChunkDitchSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	// Two steep floor slabs form a trough along Y. The first two sweeps hit
	// opposing upward normals; their two-wall adjustment then moves freely down
	// the open trough, exercising the clear third-sweep ditch continuation.
	Fix7_SpawnBox(*World, FVector(-100, 0, 173), FVector(200, 1000, 5),
		FRotator(-60, 0, 0));
	Fix7_SpawnBox(*World, FVector(100, 0, 173), FVector(200, 1000, 5),
		FRotator(60, 0, 0));
	AWoTCreatureChunk* Chunk = Fix7_ChunkUpPart(*World, FVector(-55, -120, 180));
	if (TestNotNull(TEXT("real ChunkUp replacement"), Chunk))
	{
		WoTTest::PlaceWithoutOverlapEvents(Chunk, FVector(-55, -120, 180));
		Chunk->SetActorRotation(FRotator(22, 31, 44));
		Chunk->SetTumbleRateForTest(FRotator::ZeroRotator);
		Chunk->SetBouncingForTest(false);
		Chunk->SetVelocityForTest(FVector(420, 360, -700));
		for (int32 Step = 0; Step < 100 && !Chunk->IsResting(); ++Step)
		{
			Chunk->Tick(0.02f);
		}
		TestTrue(TEXT("the V-shaped ditch reaches the landing continuation"),
			Chunk->IsResting());
		TestTrue(TEXT("a clear third sweep supplies the zero hit normal"),
			Chunk->GetLastLandedNormalForTest().IsNearlyZero());
		FRotator Expected = Chunk->GetLastPreLandedRotationForTest();
		if (Chunk->HasZeroYawForTest()) Fix7_ZeroRotatorAxis(Expected.Yaw);
		if (Chunk->HasZeroRollForTest()) Fix7_ZeroRotatorAxis(Expected.Roll);
		if (Chunk->HasZeroPitchForTest()) Fix7_ZeroRotatorAxis(Expected.Pitch);
		const FVector SurfaceDirection = (FVector::UpVector
			- Chunk->GetLastLandedNormalForTest()).GetSafeNormal();
		if (!SurfaceDirection.IsNearlyZero())
		{
			Expected += SurfaceDirection.Rotation();
		}
		TestTrue(*FString::Printf(TEXT("the rest rotation is derived from the clear "
			"third sweep (actual=%s expected=%s normal=%s zero=%d/%d/%d)"),
			*Chunk->GetActorRotation().ToCompactString(), *Expected.ToCompactString(),
			*Chunk->GetLastLandedNormalForTest().ToCompactString(),
			Chunk->HasZeroPitchForTest() ? 1 : 0,
			Chunk->HasZeroYawForTest() ? 1 : 0,
			Chunk->HasZeroRollForTest() ? 1 : 0),
			Chunk->GetActorQuat().Equals(Expected.Quaternion(), 0.001f));
	}
	WoTRandom::Seed(0x7051);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7HostBanIsolationSpec,
	"WoT.Parity.Fix7.HostBanAutomationIsolation", Fix7_Flags)
bool FFix7HostBanIsolationSpec::RunTest(const FString&)
{
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	FString ResolvedHostBanPath;
	TestTrue(TEXT("the production host-ban path resolves"),
		UWoTPreferencesSubsystem::SideFilePath(
			TEXT("WoTHostBans"), ResolvedHostBanPath));
	TestFalse(TEXT("the resolved production host-ban path is not test scratch"),
		ResolvedHostBanPath.Contains(FPaths::AutomationTransientDir(),
			ESearchCase::IgnoreCase));
	TArray<TPair<FString, bool>> Accesses;
	UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting(
		[&Accesses](const FString& Path, bool bWrite) -> TOptional<bool>
		{
			Accesses.Emplace(Path, bWrite);
			return false;
		});
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting({});
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	};

	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (TestNotNull(TEXT("real host GameMode"), Game))
	{
		TestEqual(TEXT("automation without a preference override starts empty"),
			Game->HostBanCountForTesting(), 0);
	}
	const bool bTouchedResolvedPath = Accesses.ContainsByPredicate(
		[&ResolvedHostBanPath](const TPair<FString, bool>& Access)
		{
			return FPaths::IsSamePath(Access.Key, ResolvedHostBanPath);
		});
	TestFalse(TEXT("world creation never accesses the resolved real host-ban file"),
		bTouchedResolvedPath);
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7PasswordDismissSessionSpec,
	"WoT.Parity.Fix7.PasswordDismissLeavesSession", Fix7_Flags)
bool FFix7PasswordDismissSessionSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7PasswordDismiss-")) + FGuid::NewGuid().ToString());
	const FString PreferenceLeaf =
		FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(PreferenceLeaf);
	UWorld* World = nullptr;
	IOnlineSessionPtr Remote;
	IOnlineSessionPtr Lan;
	ON_SCOPE_EXIT
	{
		for (const IOnlineSessionPtr& Provider : {Remote, Lan})
		{
			if (Provider.IsValid() && Provider->GetNamedSession(NAME_GameSession))
			{
				Provider->RemoveNamedSession(NAME_GameSession);
			}
		}
		if (World) WoTTest::DestroySpecWorld(World);
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestTrue(TEXT("authenticated world authority publishes"),
		Fix7_PublishWorldAuthority()))
	{
		return false;
	}
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	World = WoTTest::CreateSpecWorld(Options);
	UWoTSessionSubsystem* Sessions = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	IOnlineSubsystem* RemoteOSS =
		IOnlineSubsystem::Get(FName(TEXT("NULL:Fix7PasswordRemote")));
	const IOnlineSubsystem* LanOSS = World
		? Online::GetSubsystem(World, NULL_SUBSYSTEM) : nullptr;
	if (!TestNotNull(TEXT("session subsystem"), Sessions)
		|| !TestNotNull(TEXT("remote Null provider"), RemoteOSS)
		|| !TestNotNull(TEXT("local Null provider"), LanOSS))
	{
		return false;
	}
	Remote = RemoteOSS->GetSessionInterface();
	Lan = LanOSS->GetSessionInterface();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	int32 ClientTravels = 0;
	Sessions->SetClientTravelControllerForTesting(Controller);
	Sessions->SetClientTravelHookForTesting(
		[&ClientTravels](APlayerController*, const FString&, ETravelType)
		{
			++ClientTravels;
		});
	Sessions->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return true; });
	ON_SCOPE_EXIT
	{
		Sessions->SetClientTravelHookForTesting({});
		Sessions->SetClientTravelControllerForTesting(nullptr);
		Sessions->SetServerTravelHookForTesting({});
	};

	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	Request.ServerName = TEXT("Fix7 Password Remote");
	Request.bLAN = true;
	TestTrue(TEXT("remote session is created"), Remote->CreateSession(
		0, NAME_GameSession, Sessions->BuildSessionSettingsForTesting(Request)));
	const FNamedOnlineSession* Hosted =
		Remote->GetNamedSession(NAME_GameSession);
	if (!TestNotNull(TEXT("remote named session exists"), Hosted)) return false;
	FOnlineSessionSearchResult Row;
	Row.Session = FOnlineSession(*Hosted);
	auto JoinRemote = [&]()
	{
		const FWoTSessionListEntry Entry = Sessions->InjectSearchResultForTesting(
			Lan, false, Row);
		return Sessions->Join(Entry);
	};

	TestTrue(TEXT("first real join dispatches"), JoinRemote());
	TestTrue(TEXT("first join retains a local named session"), Sessions->HasSession());
	GEngine->OnNetworkFailure().Broadcast(World, nullptr,
		ENetworkFailure::FailureReceived, TEXT("NEEDPW"));
	TestEqual(TEXT("the joined connection opens the password challenge"),
		static_cast<int32>(Sessions->GetPasswordChallenge()),
		static_cast<int32>(EWoTPasswordChallenge::NeedPassword));
	Sessions->DismissPasswordChallenge();
	TestFalse(TEXT("dismiss destroys the joined named session"), Sessions->HasSession());
	TestTrue(TEXT("a second real join is accepted after dismiss"), JoinRemote());
	TestTrue(TEXT("the second join also retains its session"), Sessions->HasSession());
	TestTrue(TEXT("the second join can leave"), Sessions->Leave());
	TestFalse(TEXT("leave clears the second named session"), Sessions->HasSession());
	Request.ServerName = TEXT("Fix7 Host After Dismiss");
	TestTrue(TEXT("hosting is accepted after the dismissed join"),
		Sessions->Host(Request));
	TestTrue(TEXT("hosting owns a fresh named session"), Sessions->HasSession());
	TestTrue(TEXT("the fixture exercised client travel"), ClientTravels >= 3);
	return !HasAnyErrors();
}

// These specs switch the world to a listen server through the editor PIE
// net-mode seam (UWorld::SetPlayInEditorInitialNetMode is WITH_EDITOR only);
// the native WoTGuardedEngineTests target compiles this file without the
// editor (precedent: WoTDecorationPhysicsTests.cpp).
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7CapacityPasswordLoginSpec,
	"WoT.Parity.Fix7.CapacityPasswordRealLogin", Fix7_Flags)
bool FFix7CapacityPasswordLoginSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7CapacityLogin-")) + FGuid::NewGuid().ToString());
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.URL = TEXT("/Game/WoT/Maps/Arena_01?MaxPlayers=1"
		"?AdminPassword=AdminSecret?GamePassword=GameSecret");
	Options.BeforeInitializeActorsForPlay = [](UWorld* World)
	{
		World->WorldType = EWorldType::PIE;
		World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT
	{
		if (World) WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* Existing = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	if (!TestNotNull(TEXT("capacity authority"), Game)
		|| !TestNotNull(TEXT("existing capacity player"), Existing))
	{
		return false;
	}
	TestEqual(TEXT("fixture runs as a listen server"),
		static_cast<int32>(World->GetNetMode()),
		static_cast<int32>(NM_ListenServer));
	TestEqual(TEXT("fixture is at its configured capacity"),
		Game->GetNumPlayers(), 1);
	const FUniqueNetIdRepl InvalidId;
	auto LoginWithPasswordToken = [&](const TCHAR* PasswordToken, FString& Error)
	{
		UChildConnection* Connection = NewObject<UChildConnection>(GEngine);
		const FString LoginOptions = FString::Printf(
			TEXT("?PasswordToken=%s?Name=Fix7Guest"), PasswordToken);
		return Game->Login(Connection, ROLE_AutonomousProxy, FString(),
			LoginOptions, InvalidId, Error);
	};
	FString GamePasswordError;
	APlayerController* GamePasswordPlayer =
		LoginWithPasswordToken(TEXT(
			"87e900ca7da6b140155d69757948093ef80615c0f980879d8c6ccd52dbc0d9ba"),
			GamePasswordError);
	TestNotNull(TEXT("real Login admits the game password at capacity"),
		GamePasswordPlayer);
	TestTrue(TEXT("game-password Login returns no capacity error"),
		GamePasswordError.IsEmpty());
	FString AdminPasswordError;
	APlayerController* AdminPasswordPlayer =
		LoginWithPasswordToken(TEXT(
			"3c102d489000460464d5c773b83c9d3db9b54689a44926ced5a2b5a45c802c71"),
			AdminPasswordError);
	TestNotNull(TEXT("real Login admits the admin password at capacity"),
		AdminPasswordPlayer);
	TestTrue(TEXT("admin-password Login returns no capacity error"),
		AdminPasswordError.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7AdminPauseSpec,
	"WoT.Parity.Fix7.AdminNetworkPause", Fix7_Flags)
bool FFix7AdminPauseSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7AdminPause-")) + FGuid::NewGuid().ToString());
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.BeforeInitializeActorsForPlay = [](UWorld* World)
	{
		World->WorldType = EWorldType::PIE;
		World->SetPlayInEditorInitialNetMode(NM_ListenServer);
	};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT
	{
		if (World) WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* Admin = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* NonAdmin = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerState* AdminState = Admin
		? Admin->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("pause authority"), Game)
		|| !TestNotNull(TEXT("admin controller"), Admin)
		|| !TestNotNull(TEXT("non-admin controller"), NonAdmin)
		|| !TestNotNull(TEXT("admin player state"), AdminState))
	{
		return false;
	}
	AdminState->bAdmin = true;
	TestFalse(TEXT("non-admin cannot pause a network match"),
		Game->TrySetPlayerPause(NonAdmin, NM_ListenServer));
	TestFalse(TEXT("non-admin attempt leaves the world running"), World->IsPaused());
	TestTrue(TEXT("admin pauses through the engine pause gate"),
		Game->TrySetPlayerPause(Admin, NM_ListenServer));
	TestTrue(TEXT("the listen-server world is actually paused"), World->IsPaused());
	TestTrue(TEXT("admin unpauses through the same request path"),
		Game->TrySetPlayerPause(Admin, NM_ListenServer));
	TestFalse(TEXT("the listen-server world resumes"), World->IsPaused());
	return !HasAnyErrors();
}

#endif // WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7InGameTravelOptionsSpec,
	"WoT.Parity.Fix7.InGameConfigPersistsTravelOptions", Fix7_Flags)
bool FFix7InGameTravelOptionsSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7InGameConfig-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	FFix7_GameEngineScope Scope;
	UWoTGameInstance* Instance = NewObject<UWoTGameInstance>(Scope.Engine);
	Instance->InitializeStandalone(FName(TEXT("Fix7InGameConfig")));
	FWorldContext& Context =
		Scope.Engine->GetWorldContextFromWorldChecked(Instance->GetWorld());
	Context.PIEInstance = Fix7_UnusedPIEInstance();
	FString TravelError;
	const FURL InitialURL(nullptr,
		TEXT("/Engine/Maps/Entry?game=/Script/WoT.WoTGameMode"
			"?Name=Rand?Skin=AesSedaiSkins.Aes_Blue"), TRAVEL_Absolute);
	if (!TestTrue(TEXT("initial real game-engine travel succeeds"),
		Scope.Engine->Browse(Context, InitialURL, TravelError)
			== EBrowseReturnVal::Success))
	{
		return false;
	}
	const FUniqueNetIdRepl InvalidId;
	auto LoginCurrentWorld = [&](const TCHAR* Label) -> AWoTPlayerController*
	{
		UWorld* CurrentWorld = Instance->GetWorld();
		AWoTGameMode* CurrentGame = CurrentWorld
			? CurrentWorld->GetAuthGameMode<AWoTGameMode>() : nullptr;
		if (!CurrentGame)
		{
			AddError(FString::Printf(TEXT("%s authority is absent"), Label));
			return nullptr;
		}
		UChildConnection* Connection = NewObject<UChildConnection>(Scope.Engine);
		FString Error;
		AWoTPlayerController* Result = Cast<AWoTPlayerController>(
			CurrentGame->Login(Connection, ROLE_AutonomousProxy, FString(),
				Fix7_OptionsForURL(Context.LastURL), InvalidId, Error));
		if (!Result)
		{
			AddError(FString::Printf(TEXT("%s real Login failed: %s"),
				Label, *Error));
		}
		return Result;
	};
	AWoTPlayerController* Controller = LoginCurrentWorld(TEXT("initial-map"));
	UWorld* World = Instance->GetWorld();
	AWoTAesSedai* Pawn = World ? World->SpawnActor<AWoTAesSedai>() : nullptr;
	if (!TestNotNull(TEXT("initial in-game controller"), Controller)
		|| !TestNotNull(TEXT("initial Aes Sedai pawn"), Pawn)) return false;
	Controller->Possess(Pawn);
	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay).PlayerController(Controller);
	Overlay->SetPendingPlayerConfigForTesting(
		TEXT("Egwene"), EWoTPlayerSkinColor::Red);
	Overlay->SavePlayerConfigForTesting();

	Context.PIEInstance = Fix7_UnusedPIEInstance();
	const FURL NextURL(&Context.LastURL,
		TEXT("/Engine/Maps/Entry"), TRAVEL_Relative);
	TravelError.Reset();
	TestTrue(TEXT("relative travel to the next map succeeds through Browse/LoadMap"),
		Scope.Engine->Browse(Context, NextURL, TravelError)
			== EBrowseReturnVal::Success);
	AWoTPlayerController* NextController =
		LoginCurrentWorld(TEXT("next-map"));
	if (!TestNotNull(TEXT("next-map real Login controller"), NextController))
	{
		return false;
	}
	TestEqual(TEXT("next-map Login receives the saved skin"),
		NextController->PersonalSkinURL,
		FString(TEXT("AesSedaiSkins.Aes_Red")));
	TestEqual(TEXT("next-map Login receives the saved name"),
		NextController->PlayerState->GetPlayerName(), FString(TEXT("Egwene")));

	World = Instance->GetWorld();
	AWoTAesSedai* NextPawn = World
		? World->SpawnActor<AWoTAesSedai>() : nullptr;
	if (!TestNotNull(TEXT("next-map Aes Sedai pawn"), NextPawn)) return false;
	NextController->Possess(NextPawn);
	const TSharedRef<SWoTInGameMenuOverlay> NextOverlay =
		SNew(SWoTInGameMenuOverlay).PlayerController(NextController);
	NextOverlay->SetPendingPlayerConfigForTesting(
		TEXT("Nynaeve"), EWoTPlayerSkinColor::Base);
	NextOverlay->SavePlayerConfigForTesting();
	Context.PIEInstance = Fix7_UnusedPIEInstance();
	const FURL AfterBaseURL(&Context.LastURL,
		TEXT("/Engine/Maps/Entry"), TRAVEL_Relative);
	TravelError.Reset();
	TestTrue(TEXT("Base selection travels through another real map load"),
		Scope.Engine->Browse(Context, AfterBaseURL, TravelError)
			== EBrowseReturnVal::Success);
	AWoTPlayerController* BaseController =
		LoginCurrentWorld(TEXT("base-next-map"));
	if (!TestNotNull(TEXT("Base next-map real Login controller"),
		BaseController)) return false;
	TestTrue(TEXT("Base removes Skin from the destination Login"),
		BaseController->PersonalSkinURL.IsEmpty());
	TestEqual(TEXT("the later name reaches the destination Login"),
		BaseController->PlayerState->GetPlayerName(), FString(TEXT("Nynaeve")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7BattleMorphSkinSpec,
	"WoT.Parity.Fix7.BattleMorphUsesFactionSkin", Fix7_Flags)
bool FFix7BattleMorphSkinSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7BattleMorph-")) + FGuid::NewGuid().ToString());
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	ON_SCOPE_EXIT
	{
		if (World) WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
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
		Battle->FinishSpawning(FTransform::Identity);
	}
	AWoTPlayerController* Controller = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTAesSedai* Pawn = World
		? World->SpawnActor<AWoTAesSedai>() : nullptr;
	if (!TestNotNull(TEXT("Battle authority"), Mode)
		|| !TestNotNull(TEXT("BattleInfo"), Battle)
		|| !TestNotNull(TEXT("Battle controller"), Controller)
		|| !TestNotNull(TEXT("concrete Aes Sedai pawn"), Pawn))
	{
		return false;
	}
	Mode->Battle = Battle;
	Controller->Possess(Pawn);
	AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("Battle player state"), State)) return false;
	State->Team = 255;
	TestTrue(TEXT("real Battle team change succeeds"),
		Mode->ChangeTeam(Controller, 0));
	TestTrue(TEXT("team change records the Hound faction identity"),
		State->FactionClass == AWoTHound::StaticClass());
	TestTrue(TEXT("team change morphs the existing Aes pawn to Hound"),
		Pawn->AppearanceClass == AWoTHound::StaticClass());

	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay).PlayerController(Controller);
	Overlay->SetPendingPlayerConfigForTesting(
		TEXT("Perrin"), EWoTPlayerSkinColor::Red);
	Overlay->SavePlayerConfigForTesting();
	TestEqual(TEXT("pre-respawn save uses the morphed Hound skin URL"),
		Controller->PersonalSkinURL, FString(TEXT("HoundSkins.Hou_Red")));
	TestEqual(TEXT("the morphed pawn applies the Hound skin immediately"),
		Pawn->PersonalSkin, FName(TEXT("Hou_Red")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7RemoteAdminAllAmmoSpec,
	"WoT.Parity.Fix7.RemoteAdminAllAmmo", Fix7_Flags)
bool FFix7RemoteAdminAllAmmoSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Fix7 remote admin inventory grants"));
	if (!TestTrue(TEXT("dedicated authority loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	Worlds.Server.GetWorld()->SpawnActor<AWoTPlayerStart>(
		FVector(0.0f, 0.0f, 5000.0f), FRotator::ZeroRotator);
	if (!TestTrue(TEXT("remote client connects"),
		Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* ServerController = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTCharacter* ServerPawn = ServerController
		? Cast<AWoTCharacter>(ServerController->GetPawn()) : nullptr;
	AWoTPlayerController* ClientController = Cast<AWoTPlayerController>(
		Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
	AWoTCharacter* ClientPawn = ClientController
		? Cast<AWoTCharacter>(ClientController->GetPawn()) : nullptr;
	AWoTPlayerState* ServerState = ServerController
		? ServerController->GetPlayerState<AWoTPlayerState>() : nullptr;
	AWoTPlayerState* ClientState = ClientController
		? ClientController->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("authority pawn"), ServerPawn)
		|| !TestNotNull(TEXT("remote autonomous pawn"), ClientPawn)
		|| !TestNotNull(TEXT("authority player state"), ServerState)
		|| !TestNotNull(TEXT("remote player state"), ClientState)
		|| !TestNotNull(TEXT("authority inventory"),
			ServerPawn ? ServerPawn->InventoryComponent.Get() : nullptr))
	{
		return false;
	}

	ServerState->bAdmin = true;
	ServerState->ForceNetUpdate();
	TestTrue(TEXT("admin status reaches the remote owner"),
		Worlds.TickUntil([ClientState]() { return ClientState->bAdmin; }, 120));
	const int32 BeforeGrant = ServerPawn->InventoryComponent->Items.Num();
	ClientPawn->AllAmmo();
	TestTrue(TEXT("remote AllAmmo grants inventory on the authority"),
		Worlds.TickUntil([ServerPawn, BeforeGrant]()
		{
			return ServerPawn->InventoryComponent
				&& ServerPawn->InventoryComponent->Items.Num() > BeforeGrant;
		}, 120));
	bool bFoundInfiniteAuthorityItem = false;
	for (AWoTInventory* Item : ServerPawn->InventoryComponent->Items)
	{
		if (const AWoTAngrealInventory* Angreal =
			Cast<AWoTAngrealInventory>(Item))
		{
			bFoundInfiniteAuthorityItem = Angreal->CurCharges == 999
				&& Angreal->ChargeCost == 0;
			if (bFoundInfiniteAuthorityItem) break;
		}
	}
	TestTrue(TEXT("the authority applies infinite charges and zero cost"),
		bFoundInfiniteAuthorityItem);

	ServerState->bAdmin = false;
	ServerState->ForceNetUpdate();
	TestTrue(TEXT("admin revocation reaches the remote owner"),
		Worlds.TickUntil([ClientState]() { return !ClientState->bAdmin; }, 120));
	const int32 BeforeRejectedGrant =
		ServerPawn->InventoryComponent->Items.Num();
	ClientPawn->AllAngreal(false);
	Worlds.TickAll(8);
	TestEqual(TEXT("authority rejects a grant after admin revocation"),
		ServerPawn->InventoryComponent->Items.Num(), BeforeRejectedGrant);
#else
	AddError(TEXT("RemoteAdminAllAmmo requires WITH_EDITOR network worlds"));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7EngineTravelPasswordLogsSpec,
	"WoT.Parity.Fix7.EngineTravelPasswordLogs", Fix7_Flags)
bool FFix7EngineTravelPasswordLogsSpec::RunTest(const FString&)
{
	FFix7_GameEngineScope Scope;
	UGameInstance* Instance = NewObject<UGameInstance>(Scope.Engine);
	Instance->InitializeStandalone(FName(TEXT("Fix7TravelPasswordLogs")));
	FWorldContext& Context =
		Scope.Engine->GetWorldContextFromWorldChecked(Instance->GetWorld());
	Context.PIEInstance = Fix7_UnusedPIEInstance();
	const FString UserSecret(TEXT("Fix7UserSecret-71"));
	const FString UserToken(TEXT(
		"6f1839889700fb0fe3ecc4a39e501d8966b64f4b7923a729fdb1cda70342060f"));
	const FString AdminSecret(TEXT("Fix7AdminSecret-83"));
	const FString GameSecret(TEXT("Fix7GameSecret-97"));
	const FString Travel = FString::Printf(
		TEXT("/Engine/Maps/Entry?PasswordToken=%s?AdminPassword=%s?GamePassword=%s"),
		*UserToken, *AdminSecret, *GameSecret);
	const FURL URL(&Context.LastURL, *Travel, TRAVEL_Absolute);
	FFix7_TravelLogCapture Capture;
	GLog->AddOutputDevice(&Capture);
	FString Error;
	const EBrowseReturnVal::Type Result = Scope.Engine->Browse(Context, URL, Error);
	GLog->RemoveOutputDevice(&Capture);
	TestTrue(TEXT("real game-engine Browse reaches LoadMap"),
		Result == EBrowseReturnVal::Success);
	TestEqual(TEXT("the user password token still reaches the destination URL"),
		FString(Context.LastURL.GetOption(TEXT("PasswordToken="), TEXT(""))),
		UserToken);
	TestTrue(TEXT("the destination URL omits the legacy user password option"),
		Context.LastURL.GetOption(TEXT("Password="), nullptr) == nullptr);
	TestEqual(TEXT("the admin password still reaches the destination URL"),
		FString(Context.LastURL.GetOption(TEXT("AdminPassword="), TEXT(""))),
		AdminSecret);
	TestEqual(TEXT("the game password still reaches the destination URL"),
		FString(Context.LastURL.GetOption(TEXT("GamePassword="), TEXT(""))),
		GameSecret);
	TestFalse(TEXT("Browse and LoadMap logs omit the user password token"),
		Capture.Text.Contains(UserToken));
	TestFalse(TEXT("Browse and LoadMap logs omit the user password"),
		Capture.Text.Contains(UserSecret));
	TestFalse(TEXT("Browse and LoadMap logs omit the admin password"),
		Capture.Text.Contains(AdminSecret));
	TestFalse(TEXT("Browse and LoadMap logs omit the game password"),
		Capture.Text.Contains(GameSecret));

	const FString TokenOnlyTravel =
		TEXT("/Engine/Maps/Entry?PasswordToken=") + UserToken;
	const FURL TokenOnlyURL(nullptr, *TokenOnlyTravel, TRAVEL_Absolute);
	FFix7_TravelLogCapture TokenOnlyCapture;
	GLog->AddOutputDevice(&TokenOnlyCapture);
	FString TokenOnlyError;
	const EBrowseReturnVal::Type TokenOnlyResult =
		Scope.Engine->Browse(Context, TokenOnlyURL, TokenOnlyError);
	GLog->RemoveOutputDevice(&TokenOnlyCapture);
	TestTrue(TEXT("token-only game-engine Browse reaches LoadMap"),
		TokenOnlyResult == EBrowseReturnVal::Success);
	TestFalse(TEXT("token-only Browse and LoadMap logs omit the password token"),
		TokenOnlyCapture.Text.Contains(UserToken));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7AmpersandPasswordRedactionSpec,
	"WoT.Parity.Fix7.AmpersandPasswordRedaction", Fix7_Flags)
bool FFix7AmpersandPasswordRedactionSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7AmpersandPassword-")) + FGuid::NewGuid().ToString());
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	UWorld* World = nullptr;
	ON_SCOPE_EXIT
	{
		if (World) WoTTest::DestroySpecWorld(World);
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestTrue(TEXT("authenticated world authority publishes"),
		Fix7_PublishWorldAuthority()))
	{
		return false;
	}
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	World = WoTTest::CreateSpecWorld(Options);
	UWoTSessionSubsystem* Sessions = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	AWoTPlayerController* Controller = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	if (!TestNotNull(TEXT("session subsystem"), Sessions)
		|| !TestNotNull(TEXT("local travel controller"), Controller))
	{
		return false;
	}
	Sessions->SetCreateSessionDispatchHookForTesting(
		[](TFunction<bool()>&&) { return false; });
	Sessions->SetClientTravelControllerForTesting(Controller);
	Sessions->SetClientTravelHookForTesting(
		[](APlayerController*, const FString&, ETravelType) {});
	ON_SCOPE_EXIT
	{
		Sessions->SetCreateSessionDispatchHookForTesting({});
		Sessions->SetClientTravelControllerForTesting(nullptr);
		Sessions->SetClientTravelHookForTesting({});
	};

	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	Request.bLAN = true;
	Request.AdminPassword = TEXT("Fix7AdminPrefix&Fix7AdminSuffix");
	Request.GamePassword = TEXT("Fix7GamePrefix&Fix7GameSuffix");
	TestFalse(TEXT("the injected create refusal retains host diagnostics"),
		Sessions->Host(Request));
	TestFalse(TEXT("host diagnostics redact the complete admin password"),
		Sessions->GetLastHostTravelURL().Contains(TEXT("Fix7AdminSuffix")));
	TestFalse(TEXT("host diagnostics redact the complete game password"),
		Sessions->GetLastHostTravelURL().Contains(TEXT("Fix7GameSuffix")));

	TestTrue(TEXT("a real direct client travel becomes pending"),
		Sessions->ConnectDirect(TEXT("127.0.0.1:7777")));
	GEngine->OnNetworkFailure().Broadcast(World, nullptr,
		ENetworkFailure::FailureReceived,
		TEXT("refused ?Password=Fix7LegacyPrefix&Fix7LegacySuffix"
			"?PasswordToken=Fix7TokenPrefix&Fix7TokenSuffix"));
	TestFalse(TEXT("retained legacy diagnostics still redact through ampersand"),
		Sessions->RetainedConnectionFailure().Contains(TEXT("Fix7LegacySuffix")));
	TestFalse(TEXT("retained token diagnostics redact through ampersand"),
		Sessions->RetainedConnectionFailure().Contains(TEXT("Fix7TokenSuffix")));
	return !HasAnyErrors();
}

// These specs switch the world to a listen server through the editor PIE
// net-mode seam (UWorld::SetPlayInEditorInitialNetMode is WITH_EDITOR only);
// the native WoTGuardedEngineTests target compiles this file without the
// editor (precedent: WoTDecorationPhysicsTests.cpp).
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7BanPrecedesCapacitySpec,
	"WoT.Parity.Fix7.BanPrecedesCapacity", Fix7_Flags)
bool FFix7BanPrecedesCapacitySpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7BanCapacity-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT
	{
		if (World) WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	const FUniqueNetIdRepl BannedId = Fix7_UniqueId(TEXT("fix7-ban-capacity"));
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* Admin = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* Victim = World
		? Fix7_SpawnRemotePlayer(*World, TEXT("CapacityVictim"), BannedId)
		: nullptr;
	if (!TestNotNull(TEXT("ban authority"), Game)
		|| !TestNotNull(TEXT("ban admin"), Admin)
		|| !TestNotNull(TEXT("ban victim"), Victim))
	{
		return false;
	}
	Admin->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
	bool bRemoved = false;
	Game->SetPlayerRemovalHookForTesting(
		[&bRemoved](APlayerController* Player, const FText&)
		{
			bRemoved = true;
			Player->Destroy();
			return true;
		});
	TestTrue(TEXT("real admin command accepts the stable-id ban"),
		Game->ExecuteAdminCommand(Admin, TEXT("KICKBAN CapacityVictim")));
	TestTrue(TEXT("the ban removes the target"), bRemoved);
	TestEqual(TEXT("one ban is persisted"), Game->HostBanCountForTesting(), 1);

	WoTTest::DestroySpecWorld(World);
	World = nullptr;
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	Options.URL = TEXT("/Game/WoT/Maps/Arena_01?MaxPlayers=1"
		"?GamePassword=GameSecret");
	Options.BeforeInitializeActorsForPlay = [](UWorld* NewWorld)
	{
		NewWorld->WorldType = EWorldType::PIE;
		NewWorld->SetPlayInEditorInitialNetMode(NM_ListenServer);
	};
	World = WoTTest::CreateSpecWorld(Options);
	Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!TestNotNull(TEXT("replacement capacity authority"), Game)) return false;
	FString BanError;
	const FString CorrectToken = TEXT("?PasswordToken="
		"87e900ca7da6b140155d69757948093ef80615c0f980879d8c6ccd52dbc0d9ba");
	Game->PreLogin(CorrectToken, TEXT("203.0.113.141"), BannedId, BanError);
	TestFalse(TEXT("the persisted ban refuses below capacity"),
		BanError.IsEmpty());
	AWoTPlayerController* Filler = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("capacity filler"), Filler)) return false;
	TestEqual(TEXT("the replacement server is full"), Game->GetNumPlayers(), 1);
	FString FullBanError;
	Game->PreLogin(CorrectToken, TEXT("203.0.113.142"), BannedId,
		FullBanError);
	TestEqual(TEXT("the ban result overrides the full-server result"),
		FullBanError, BanError);
	return !HasAnyErrors();
}

#endif // WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix7SharedAddressIdentityBansSpec,
	"WoT.Parity.Fix7.SharedAddressIdentityBans", Fix7_Flags)
bool FFix7SharedAddressIdentityBansSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix7SharedAddressBans-")) + FGuid::NewGuid().ToString());
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(
		FPaths::Combine(Directory, TEXT("WoTUserPreferences")));
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT
	{
		if (World) WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	AWoTGameMode* Game = World
		? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!TestNotNull(TEXT("shared-address ban authority"), Game)) return false;
	const FString SharedAddress(TEXT("198.51.100.77"));
	const FUniqueNetIdRepl FirstId =
		Fix7_UniqueId(TEXT("fix7-shared-address-first"));
	const FUniqueNetIdRepl SecondId =
		Fix7_UniqueId(TEXT("fix7-shared-address-second"));
	auto LoginFromSharedAddress = [&](const TCHAR* Name,
		const FUniqueNetIdRepl& UniqueId) -> AWoTPlayerController*
	{
		FString Error;
		Game->PreLogin(FString(), SharedAddress, UniqueId, Error);
		if (!Error.IsEmpty())
		{
			AddError(FString::Printf(
				TEXT("shared-address PreLogin for %s failed: %s"), Name, *Error));
			return nullptr;
		}
		AWoTPlayerController* Controller =
			Fix7_SpawnRemotePlayer(*World, Name, UniqueId);
		if (!Controller) return nullptr;
		Game->InitNewPlayer(Controller, UniqueId,
			FString::Printf(TEXT("?Name=%s"), Name), FString());
		Controller->PlayerState->SetPlayerName(Name);
		return Controller;
	};
	AWoTPlayerController* First = LoginFromSharedAddress(
		TEXT("SharedFirst"), FirstId);
	AWoTPlayerController* Second = LoginFromSharedAddress(
		TEXT("SharedSecond"), SecondId);
	AWoTPlayerController* Admin = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	if (!TestNotNull(TEXT("first shared-address player"), First)
		|| !TestNotNull(TEXT("second shared-address player"), Second)
		|| !TestNotNull(TEXT("shared-address admin"), Admin))
	{
		return false;
	}
	Admin->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
	int32 RemovedPlayers = 0;
	Game->SetPlayerRemovalHookForTesting(
		[&RemovedPlayers](APlayerController* Player, const FText&)
		{
			++RemovedPlayers;
			Player->Destroy();
			return true;
		});
	TestTrue(TEXT("first identity ban command is accepted"),
		Game->ExecuteAdminCommand(Admin, TEXT("KICKBAN SharedFirst")));
	TestTrue(TEXT("second identity ban command is accepted"),
		Game->ExecuteAdminCommand(Admin, TEXT("KICKBAN SharedSecond")));
	TestEqual(TEXT("both shared-address players are removed"),
		RemovedPlayers, 2);
	TestEqual(TEXT("distinct stable identities retain two ban records"),
		Game->HostBanCountForTesting(), 2);
	FString FirstReconnectError;
	Game->PreLogin(FString(), TEXT("203.0.113.151"), FirstId,
		FirstReconnectError);
	FString SecondReconnectError;
	Game->PreLogin(FString(), TEXT("203.0.113.152"), SecondId,
		SecondReconnectError);
	TestFalse(TEXT("the first stable identity remains banned"),
		FirstReconnectError.IsEmpty());
	TestFalse(TEXT("the second stable identity remains banned"),
		SecondReconnectError.IsEmpty());
	return !HasAnyErrors();
}

#endif
