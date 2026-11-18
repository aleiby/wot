#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "WoTVRTestFixture.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRSyntheticSession.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Tests/NetTestHelpers.h"

// A listen server and a remote client (design sections 14 and 15): the
// client's mid-match switches are invisible to the server, which keeps the
// same controller, body and position and receives head aim as ordinary aim
// through the existing transport.

namespace
{
UE::Net::FTestWorldInstance WoTVRNetCreateListenServer()
{
	UE::Net::FTestWorldInstance Instance(/*bDelayedInit*/ true);
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
		UE::Net::FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		const FGameInstancePIEResult Initialized = GameInstance->InitializeForPlayInEditor(PIEInstance, Parameters);
		if (!Initialized.IsSuccess())
		{
			GameInstance->Shutdown();
			GameInstance->RemoveFromRoot();
			return Instance;
		}
	}
	Instance.GameInstance = GameInstance;
	{
		UE::Net::FScopedNetTestPIERestoration Restoration;
		UE::SetPlayInEditorID(-1);
		FURL URL(nullptr, TEXT("/Engine/Maps/Entry?listen?Game=/Script/WoT.WoTGameMode"), TRAVEL_Absolute);
		FString Error;
		Instance.bIsLoaded = GEngine->Browse(*GameInstance->GetWorldContext(), URL, Error) == EBrowseReturnVal::Success;
	}
	return Instance;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRNetworkSwitchSpec, "WoT.VR.Network.ClientSwitchInvisibleToServer", WoTVRTest::Flags)
bool FWoTVRNetworkSwitchSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	UE::Net::FTestWorlds Worlds;
	Worlds.Server = WoTVRNetCreateListenServer();
	Worlds.SetDescription(TEXT("WoTVR client switch over the ordinary transport"));
	if (!TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("real remote client connects"), Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	APlayerController* ServerController = Worlds.GetServerPlayerControllerOfClient(0);
	APlayerController* ClientController = Worlds.Clients[0].GetWorld()->GetFirstPlayerController();
	ACharacter* ServerPawn = ServerController ? Cast<ACharacter>(ServerController->GetPawn()) : nullptr;
	ACharacter* ClientPawn = ClientController ? Cast<ACharacter>(ClientController->GetPawn()) : nullptr;
	ULocalPlayer* ClientPlayer = ClientController ? ClientController->GetLocalPlayer() : nullptr;
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(ClientPlayer);
	if (!TestNotNull(TEXT("server pawn"), ServerPawn) || !TestNotNull(TEXT("client pawn"), ClientPawn)
		|| !TestNotNull(TEXT("the client's rig"), Rig))
	{
		return false;
	}
	TestNull(TEXT("the server's copy of the client has no local player, so no rig"),
		ServerController->GetLocalPlayer());
	// Rest both bodies in the air, as the hooks' own transport specs do.
	Worlds.TickAll(2);
	const FVector Start(0.0, 5000.0, 5000.0);
	for (ACharacter* Pawn : {ServerPawn, ClientPawn})
	{
		Pawn->SetActorLocation(Start, false, nullptr, ETeleportType::TeleportPhysics);
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Pawn->GetCharacterMovement()->StopMovementImmediately();
	}
	if (FNetworkPredictionData_Client_Character* Prediction =
		ClientPawn->GetCharacterMovement()->GetPredictionData_Client_Character())
	{
		Prediction->SavedMoves.Empty();
		Prediction->PendingMove.Reset();
	}
	ServerPawn->ForceNetUpdate();
	Worlds.TickAll(10);
	const FVector Rest = ServerPawn->GetActorLocation();
	UClass* const ServerControllerClass = ServerController->GetClass();

	auto ExpectServerUnchanged = [&](const FString& Label)
	{
		TestTrue(Label + TEXT(": the server keeps the same controller for the client"),
			Worlds.GetServerPlayerControllerOfClient(0) == ServerController
			&& ServerController->GetClass() == ServerControllerClass);
		TestTrue(Label + TEXT(": the server keeps the same body, still possessed"),
			ServerController->GetPawn() == ServerPawn && IsValid(ServerPawn));
		TestTrue(Label + TEXT(": the body did not move"), ServerPawn->GetActorLocation().Equals(Rest, 1.0));
	};

	for (int32 Cycle = 0; Cycle < 2; ++Cycle)
	{
		const FString Label = FString::Printf(TEXT("cycle %d"), Cycle + 1);
		Scope.Session->SetHead(FVector::ZeroVector, FRotator::ZeroRotator);
		TestTrue(Label + TEXT(": the client switches to VR mid-match"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")));
		Worlds.TickAll(10);
		ExpectServerUnchanged(Label + TEXT(" VR"));
		// Head aim reaches the server as ordinary aim.
		Scope.Session->SetHead(FVector::ZeroVector, FRotator(23.0, 41.0, 0.0));
		const FRotator* ClientAim = nullptr;
		FRotator Expected;
		Worlds.TickAll(2);
		Expected = ClientController->GetControlRotation();
		ClientAim = &Expected;
		TestEqual(Label + TEXT(": the client's aim pitch is the head's"), ClientAim->Pitch, 23.0, 0.05);
		TestTrue(Label + TEXT(": head aim arrives at the server as ordinary aim"),
			Worlds.TickUntil([&]() { return ServerController->GetControlRotation().Equals(Expected, 0.5); }, 180));
		ExpectServerUnchanged(Label + TEXT(" aim"));
		TestTrue(Label + TEXT(": the client switches back to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
		Worlds.TickAll(30);
		ExpectServerUnchanged(Label + TEXT(" flat"));
		TestTrue(Label + TEXT(": the server keeps the aim the client continues from"),
			ServerController->GetControlRotation().Equals(Expected, 0.5));
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS && WITH_EDITOR
