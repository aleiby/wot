#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvHeal.h"
#include "../Inventory/WoTZoneLeeches.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTRandom.h"
#include "../WoTZoneVolume.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif

namespace
{
constexpr EAutomationTestFlags Fix4_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FActorSpawnParameters Fix4_Spawn()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

int32 Fix4_CountSmoothFOVLeeches(const AWoTCharacter* Character)
{
	if (!Character) return 0;
	TArray<UWoTSmoothRandFOVLeech*> Leeches;
	Character->GetComponents<UWoTSmoothRandFOVLeech>(Leeches);
	int32 Count = 0;
	for (const UWoTSmoothRandFOVLeech* Leech : Leeches)
	{
		if (IsValid(Leech)) ++Count;
	}
	return Count;
}

AWoTZoneVolume* Fix4_SpawnFOVZone(UWorld& World, const FVector& Center,
	const FVector& Extent)
{
	AWoTZoneVolume* Zone = World.SpawnActor<AWoTZoneVolume>(
		Center, FRotator::ZeroRotator, Fix4_Spawn());
	if (Zone)
	{
		Zone->Collision->SetBoxExtent(Extent);
		Zone->bFOVZone = true;
		Zone->ZoneFOV = 120.0f;
		Zone->bLeechZone = true;
		Zone->LeechClasses.Add(UWoTSmoothRandFOVLeech::StaticClass());
	}
	return Zone;
}

#if WITH_EDITOR
UE::Net::FTestWorldInstance Fix4_CreateListenServer()
{
	using namespace UE::Net;
	FTestWorldInstance Instance(/*bDelayedInit*/ true);
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->AddToRoot();
	FGameInstancePIEParameters Parameters;
	Parameters.bSimulateInEditor = false;
	Parameters.bAnyBlueprintErrors = false;
	Parameters.bStartInSpectatorMode = false;
	Parameters.bRunAsDedicated = false;
	Parameters.bIsPrimaryPIEClient = false;
	Parameters.WorldFeatureLevel = ERHIFeatureLevel::Num;
	Parameters.EditorPlaySettings = nullptr;
	Parameters.NetMode = PIE_ListenServer;
	Parameters.OverrideMapURL = TEXT("/Engine/Maps/Entry");
	int32 PIEInstance = INDEX_NONE;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		PIEInstance = FMath::Max(PIEInstance, Context.PIEInstance);
	}
	++PIEInstance;
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized =
			GameInstance->InitializeForPlayInEditor(PIEInstance, Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	FWorldContext* WorldContext = GameInstance->GetWorldContext();
	UGameViewportClient* ViewportClient = NewObject<UGameViewportClient>(
		GEngine, GEngine->GameViewportClientClass);
	ViewportClient->Init(*WorldContext, GameInstance);
	WorldContext->GameViewport = ViewportClient;
	FString CreatePlayerError;
	ViewportClient->SetupInitialLocalPlayer(CreatePlayerError);
	{
		FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FURL URL(nullptr,
			TEXT("/Engine/Maps/Entry?listen?Game=/Script/WoT.WoTGameMode"),
			TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(
			*GameInstance->GetWorldContext(), URL, Error)
			== EBrowseReturnVal::Success;
	}
	return Instance;
}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix4RandomInclusiveMaximum,
	"WoT.Parity.Fix4.Random.InclusiveMaximumCharge", Fix4_Flags)

bool FWoTFix4RandomInclusiveMaximum::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// FRandomStream's LCG maps this seed to 0xffffffff on its next draw.
	// The gameplay float conversion must retain that retail-inclusive endpoint.
	WoTRandom::Seed(0x734454c4);
	const float Endpoint = WoTRandom::FRand();
	TestTrue(FString::Printf(
		TEXT("the gameplay random source reaches the exact inclusive 1.0 endpoint (%.9g)"),
		Endpoint), Endpoint == 1.0f);

#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Fix4 inclusive artifact charge endpoint"));
	if (TestTrue(TEXT("real multiplayer authority world loads"),
		Worlds.Server.IsLoaded()))
	{
		UWorld* World = Worlds.Server.GetWorld();
		const FTransform At(FVector(0.0, 0.0, 5000.0));
		AWoTAngrealInvHeal* Artifact =
			World->SpawnActorDeferred<AWoTAngrealInvHeal>(
				AWoTAngrealInvHeal::StaticClass(), At, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (TestNotNull(TEXT("real artifact instance"), Artifact))
		{
			Artifact->MinInitialCharges = 10;
			Artifact->MaxInitialCharges = 20;
			Artifact->MaxCharges = 20;
			WoTRandom::Seed(0x734454c4);
			Artifact->FinishSpawning(At);
			TestEqual(TEXT("multiplayer SetCharges can truncate to the authored maximum"),
				Artifact->CurCharges, 20);
		}
	}
#endif

	WoTRandom::Seed(0x7051);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix4SmoothRandFOVListenHost,
	"WoT.Parity.Fix4.SmoothRandFOV.ListenHostLifecycle", Fix4_Flags)

bool FWoTFix4SmoothRandFOVListenHost::RunTest(const FString& Parameters)
{
	(void)Parameters;
#if WITH_EDITOR
	bool bOwnsActivation = false;
	if (!IsWoTPayloadBootstrapActive())
	{
		bOwnsActivation =
			!PublishWoTPayloadBootstrapActivationForTesting().IsError()
			&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
	}
	ON_SCOPE_EXIT
	{
		if (bOwnsActivation) ResetWoTPayloadActivationForTesting();
	};
	UE::Net::FTestWorlds Worlds;
	Worlds.Server = Fix4_CreateListenServer();
	Worlds.SetDescription(TEXT("Fix4 SmoothRandFOV listen-host lifecycle"));
	if (!TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded()))
	{
		return false;
	}
	UWorld* World = Worlds.Server.GetWorld();
	TestEqual(TEXT("host fixture is a listen server"), World->GetNetMode(), NM_ListenServer);
	AWoTPlayerController* PC = Cast<AWoTPlayerController>(World->GetFirstPlayerController());
	if (!TestNotNull(TEXT("listen host controller"), PC))
	{
		return false;
	}
	TestNotNull(TEXT("listen host controller is backed by the real local player"),
		PC->GetLocalPlayer());
	if (APawn* InitialPawn = PC->GetPawn())
	{
		PC->UnPossess();
		InitialPawn->Destroy();
	}
	AWoTZoneVolume* Zone = Fix4_SpawnFOVZone(*World,
		FVector::ZeroVector, FVector(500));
	AWoTCharacter* Inside = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, Fix4_Spawn());
	Worlds.TickAll(2);
	PC->Possess(Inside);
	Worlds.TickAll(2);
	TestEqual(TEXT("spawn-inside host possession creates one SmoothRand FOV leech"),
		Fix4_CountSmoothFOVLeeches(Inside), 1);
	TestTrue(FString::Printf(TEXT("spawn-inside host applies the authored FOV band (%.3f)"),
		PC->GetCameraFOVForTest()),
		PC->GetCameraFOVForTest() >= 117.0f
			&& PC->GetCameraFOVForTest() <= 123.0f);

	Inside->SetActorLocation(FVector(1200, 0, 0));
	UWoTZoneIndex::RefreshAfterRelocation(Inside);
	Worlds.TickAll(10);
	TestEqual(TEXT("host region leave removes the SmoothRand FOV leech"),
		Fix4_CountSmoothFOVLeeches(Inside), 0);
	TestEqual(TEXT("host region leave restores the saved camera FOV"),
		PC->GetCameraFOVForTest(), 90.0f);
	Inside->SetActorLocation(FVector::ZeroVector);
	UWoTZoneIndex::RefreshAfterRelocation(Inside);
	Worlds.TickAll(10);
	TestEqual(TEXT("host region re-entry remains singular"),
		Fix4_CountSmoothFOVLeeches(Inside), 1);

	PC->UnPossess();
	Worlds.TickAll(2);
	TestEqual(TEXT("host unpossession inside detaches the old pawn leech"),
		Fix4_CountSmoothFOVLeeches(Inside), 0);
	TestEqual(TEXT("host unpossession inside restores the camera"),
		PC->GetCameraFOVForTest(), 90.0f);
	AWoTCharacter* Outside = World->SpawnActor<AWoTCharacter>(
		FVector(1200, 0, 0), FRotator::ZeroRotator, Fix4_Spawn());
	PC->Possess(Outside);
	Worlds.TickAll(10);
	TestEqual(TEXT("possession change to an outside pawn retains the restored FOV"),
		PC->GetCameraFOVForTest(), 90.0f);

	PC->UnPossess();
	AWoTCharacter* DyingInside = World->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, Fix4_Spawn());
	Worlds.TickAll(2);
	PC->Possess(DyingInside);
	Worlds.TickAll(10);
	TestEqual(TEXT("second spawn-inside possession reconciles before death"),
		Fix4_CountSmoothFOVLeeches(DyingInside), 1);
	DyingInside->Destroy();
	Worlds.TickAll(10);
	TestEqual(TEXT("host pawn destruction inside restores the camera"),
		PC->GetCameraFOVForTest(), 90.0f);
	TestNotNull(TEXT("listen-host test uses its real FOV zone"), Zone);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFix4SmoothRandFOVRemoteClient,
	"WoT.Parity.Fix4.SmoothRandFOV.RemoteClientLifecycle", Fix4_Flags)

bool FWoTFix4SmoothRandFOVRemoteClient::RunTest(const FString& Parameters)
{
	(void)Parameters;
#if WITH_EDITOR
	bool bOwnsActivation = false;
	if (!IsWoTPayloadBootstrapActive())
	{
		bOwnsActivation =
			!PublishWoTPayloadBootstrapActivationForTesting().IsError()
			&& !PublishWoTBootstrapWorldReadyForTesting().IsError();
	}
	ON_SCOPE_EXIT
	{
		if (bOwnsActivation) ResetWoTPayloadActivationForTesting();
	};
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("Fix4 SmoothRandFOV remote-client lifecycle"));
	UWorld* ServerWorld = Worlds.Server.GetWorld();
	ServerWorld->SpawnActor<AWoTPlayerStart>(
		FVector(1200, 0, 0), FRotator::ZeroRotator, Fix4_Spawn());
	if (!TestTrue(TEXT("real remote owner connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	UWorld* ClientWorld = Worlds.Clients[0].GetWorld();
	AWoTPlayerController* ClientPC = Cast<AWoTPlayerController>(
		ClientWorld->GetFirstPlayerController());
	AWoTCharacter* ServerPawn = ServerPC
		? Cast<AWoTCharacter>(ServerPC->GetPawn()) : nullptr;
	AWoTCharacter* ClientPawn = ClientPC
		? Cast<AWoTCharacter>(ClientPC->GetPawn()) : nullptr;
	if (!TestNotNull(TEXT("remote authority controller"), ServerPC)
		|| !TestNotNull(TEXT("remote owner controller"), ClientPC)
		|| !TestNotNull(TEXT("remote authority pawn"), ServerPawn)
		|| !TestNotNull(TEXT("remote owner pawn"), ClientPawn))
	{
		return false;
	}
	AWoTZoneVolume* ServerZone = Fix4_SpawnFOVZone(
		*ServerWorld, FVector::ZeroVector, FVector(500));
	AWoTZoneVolume* ClientZone = Fix4_SpawnFOVZone(
		*ClientWorld, FVector::ZeroVector, FVector(500));
	Worlds.TickAll(10);

	ServerPawn->SetActorLocation(FVector::ZeroVector);
	UWoTZoneIndex::RefreshAfterRelocation(ServerPawn);
	TestTrue(TEXT("remote owner enters and receives one SmoothRand FOV leech"),
		Worlds.TickUntil([ClientPawn]()
		{
			return Fix4_CountSmoothFOVLeeches(ClientPawn) == 1;
		}, 120));
	ServerPawn->SetActorLocation(FVector(1200, 0, 0));
	UWoTZoneIndex::RefreshAfterRelocation(ServerPawn);
	TestTrue(TEXT("remote owner leave detaches and restores FOV"),
		Worlds.TickUntil([ClientPawn, ClientPC]()
		{
			return Fix4_CountSmoothFOVLeeches(ClientPawn) == 0
				&& ClientPC->GetCameraFOVForTest() == 90.0f;
		}, 120));
	ServerPawn->SetActorLocation(FVector::ZeroVector);
	UWoTZoneIndex::RefreshAfterRelocation(ServerPawn);
	TestTrue(TEXT("remote owner re-entry remains singular"),
		Worlds.TickUntil([ClientPawn]()
		{
			return Fix4_CountSmoothFOVLeeches(ClientPawn) == 1;
		}, 120));

	ServerPC->UnPossess();
	ServerPawn->Destroy();
	AWoTCharacter* Replacement = ServerWorld->SpawnActor<AWoTCharacter>(
		FVector::ZeroVector, FRotator::ZeroRotator, Fix4_Spawn());
	ServerPC->Possess(Replacement);
	ServerPC->ForceNetUpdate();
	AWoTCharacter* ClientReplacement = nullptr;
	TestTrue(TEXT("remote spawn-inside replacement reaches its owner"),
		Worlds.TickUntil([&]()
		{
			ClientReplacement = Worlds.FindReplicatedObjectOnClient(Replacement, 0);
			return ClientPC->GetPawn() == ClientReplacement
				&& Fix4_CountSmoothFOVLeeches(ClientReplacement) == 1;
		}, 180));
	TestTrue(TEXT("remote spawn-inside replacement applies the authored FOV band"),
		ClientPC->GetCameraFOVForTest() >= 117.0f
			&& ClientPC->GetCameraFOVForTest() <= 123.0f);

	ServerPC->UnPossess();
	Replacement->Destroy();
	TestTrue(TEXT("remote death/destruction inside restores the old camera"),
		Worlds.TickUntil([ClientPC]()
		{
			return ClientPC->GetPawn() == nullptr
				&& ClientPC->GetCameraFOVForTest() == 90.0f;
		}, 180));
	AWoTCharacter* Outside = ServerWorld->SpawnActor<AWoTCharacter>(
		FVector(1200, 0, 0), FRotator::ZeroRotator, Fix4_Spawn());
	ServerPC->Possess(Outside);
	ServerPC->ForceNetUpdate();
	AWoTCharacter* ClientOutside = nullptr;
	TestTrue(TEXT("remote possession change outside stays restored and leech-free"),
		Worlds.TickUntil([&]()
		{
			ClientOutside = Worlds.FindReplicatedObjectOnClient(Outside, 0);
			return ClientPC->GetPawn() == ClientOutside
				&& Fix4_CountSmoothFOVLeeches(ClientOutside) == 0
				&& ClientPC->GetCameraFOVForTest() == 90.0f;
		}, 180));
	TestNotNull(TEXT("remote authority uses its FOV zone"), ServerZone);
	TestNotNull(TEXT("remote owner uses its matching local FOV zone"), ClientZone);
#endif
	return !HasAnyErrors();
}

#endif
