#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

#include "../WoTCharacter.h"
#include "../WoTCharacterMovement.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTPlayerController.h"
#include "../WoTTeleporter.h"
#include "../Inventory/WoTWhirlwindLeech.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Tests/NetTestHelpers.h"

namespace
{
constexpr EAutomationTestFlags LocalPlayerHooksNetworkFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

UE::Net::FTestWorldInstance CreateLocalPlayerHooksListenServer()
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
	{
		UE::Net::FScopedNetTestPIERestoration Restoration;
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

struct FLocalPlayerHooksNetworkWorlds
{
	UE::Net::FTestWorlds Worlds;
	AWoTPlayerController* ServerController = nullptr;
	AWoTPlayerController* ClientController = nullptr;
	AWoTCharacter* ServerPawn = nullptr;
	AWoTCharacter* ClientPawn = nullptr;
	UWoTCharacterMovement* ServerMovement = nullptr;
	UWoTCharacterMovement* ClientMovement = nullptr;
	ULocalPlayer* LocalPlayer = nullptr;

	bool Initialize(FAutomationTestBase& Test)
	{
		Worlds.Server = CreateLocalPlayerHooksListenServer();
		Worlds.SetDescription(TEXT("local-player hooks ordinary network transport"));
		if (!Test.TestTrue(TEXT("real listen server loads"), Worlds.Server.IsLoaded())
			|| !Test.TestTrue(TEXT("real remote client connects"),
				Worlds.CreateAndConnectClient()))
		{
			return false;
		}
		ServerController = Cast<AWoTPlayerController>(
			Worlds.GetServerPlayerControllerOfClient(0));
		ClientController = Cast<AWoTPlayerController>(
			Worlds.Clients[0].GetWorld()->GetFirstPlayerController());
		ServerPawn = ServerController
			? Cast<AWoTCharacter>(ServerController->GetPawn()) : nullptr;
		ClientPawn = ClientController
			? Cast<AWoTCharacter>(ClientController->GetPawn()) : nullptr;
		ServerMovement = ServerPawn
			? Cast<UWoTCharacterMovement>(ServerPawn->GetCharacterMovement()) : nullptr;
		ClientMovement = ClientPawn
			? Cast<UWoTCharacterMovement>(ClientPawn->GetCharacterMovement()) : nullptr;
		LocalPlayer = ClientController ? ClientController->GetLocalPlayer() : nullptr;
		if (!Test.TestNotNull(TEXT("authority character movement"), ServerMovement)
			|| !Test.TestNotNull(TEXT("autonomous character movement"), ClientMovement)
			|| !Test.TestNotNull(TEXT("remote client's actual local player"), LocalPlayer))
		{
			return false;
		}
		Test.TestEqual(TEXT("server fixture is listening"),
			Worlds.Server.GetWorld()->GetNetMode(), NM_ListenServer);
		Test.TestEqual(TEXT("remote world is a client"),
			Worlds.Clients[0].GetWorld()->GetNetMode(), NM_Client);
		Test.TestEqual(TEXT("owning pawn uses autonomous prediction"),
			ClientPawn->GetLocalRole(), ROLE_AutonomousProxy);
		Test.TestNull(TEXT("remote authority controller has no local player provider"),
			ServerController->GetLocalPlayer());
		ResetToRest();
		return !Test.HasAnyErrors();
	}

	void ResetToRest()
	{
		// Finish old saved moves before fixture placement. During measured ticks
		// only ordinary client input and the real net driver advance these pawns.
		ClientMovement->FlushServerMoves();
		Worlds.TickAll(2);
		const FVector Start(0.0, 5000.0, 5000.0);
		for (AWoTCharacter* Pawn : { ServerPawn, ClientPawn })
		{
			Pawn->ConsumeMovementInputVector();
			Pawn->SetActorLocation(Start, false, nullptr, ETeleportType::TeleportPhysics);
			Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Pawn->GetCharacterMovement()->StopMovementImmediately();
		}
		// Forget the client's unacknowledged moves from before the placement: the
		// server's answer to one of them is a correction of the whole placement
		// jump, which the hooks rightly report as a teleport. The engine ignores
		// a correction whose timestamp matches no saved move.
		if (FNetworkPredictionData_Client_Character* ClientData =
			ClientMovement->GetPredictionData_Client_Character())
		{
			ClientData->SavedMoves.Empty();
			ClientData->PendingMove.Reset();
		}
		ServerPawn->ForceNetUpdate();
		Worlds.TickAll(6);
	}

	double TickMovement(bool bStick, int32 Frames)
	{
		double PeakAuthoritySpeed = 0.0;
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			if (bStick) ClientPawn->AddMovementInput(FVector::ForwardVector, 1.0f);
			Worlds.TickAll();
			PeakAuthoritySpeed = FMath::Max(PeakAuthoritySpeed,
				ServerMovement->Velocity.Size());
		}
		return PeakAuthoritySpeed;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalPlayerHooksClientMovementTransportSpec,
	"WoT.VR.Hooks.H3.ClientMovementTransport", LocalPlayerHooksNetworkFlags)

bool FWoTLocalPlayerHooksClientMovementTransportSpec::RunTest(const FString&)
{
	FLocalPlayerHooksNetworkWorlds Fixture;
	if (!Fixture.Initialize(*this)) return false;
	const FVector IdleStart = Fixture.ServerPawn->GetActorLocation();
	Fixture.Worlds.TickAll(20);
	TestTrue(TEXT("no consumer and no input leave the authority stationary"),
		Fixture.ServerPawn->GetActorLocation().Equals(IdleStart, 0.5));

	// Flying avoids Entry's floor and fall hazards while retaining the same
	// ConsumeInputVector, acceleration clamp, saved moves and server simulation.
	const double StickSpeed = Fixture.TickMovement(/*bStick*/ true, 90);
	TestTrue(TEXT("ordinary client stick input moves the authority"),
		Fixture.ServerPawn->GetActorLocation().X > IdleStart.X + 50.0);
	TestTrue(TEXT("ordinary movement reaches a useful speed baseline"), StickSpeed > 50.0);
	Fixture.ResetToRest();

	FVector ExtraInput = FVector::ForwardVector;
	int32 Samples = 0;
	FWoTLocalPlayerConsumer Consumer;
	Consumer.ExtraMovement = [&ExtraInput, &Samples]()
	{
		++Samples;
		return ExtraInput;
	};
	const FName Owner(TEXT("HooksNetworkMovement"));
	if (!TestTrue(TEXT("client attaches the extra movement consumer"),
		FWoTLocalPlayerHooks::Attach(*Fixture.LocalPlayer, Owner, Consumer)))
	{
		return false;
	}
	bool bAttached = true;
	ON_SCOPE_EXIT
	{
		if (bAttached) FWoTLocalPlayerHooks::Detach(*Fixture.LocalPlayer, Owner);
	};

	const FVector ExtraStart = Fixture.ServerPawn->GetActorLocation();
	const double ExtraSpeed = Fixture.TickMovement(/*bStick*/ false, 90);
	TestTrue(TEXT("client samples its registered extra movement source"), Samples > 0);
	TestTrue(TEXT("extra input alone crosses the net driver and moves the authority"),
		Fixture.ServerPawn->GetActorLocation().X > ExtraStart.X + 50.0);
	TestTrue(TEXT("authority receives ordinary positive movement acceleration"),
		Fixture.ServerMovement->GetCurrentAcceleration().X > 0.0);
	TestTrue(TEXT("extra input alone stays within the ordinary input speed"),
		ExtraSpeed <= StickSpeed + 1.0);

	ExtraInput = FVector(8.0, 0.0, 0.0);
	const double CombinedSpeed = Fixture.TickMovement(/*bStick*/ true, 90);
	TestTrue(TEXT("stick plus oversized extra input cannot increase authority speed"),
		CombinedSpeed <= StickSpeed + 1.0);
	TestTrue(TEXT("combined authority speed obeys the existing legal maximum"),
		CombinedSpeed <= Fixture.ServerMovement->GetMaxSpeed() + 1.0);
	TestTrue(TEXT("combined authority acceleration obeys the existing legal clamp"),
		Fixture.ServerMovement->GetCurrentAcceleration().Size()
			<= Fixture.ServerMovement->GetMaxAcceleration() + 1.0);

	FWoTLocalPlayerHooks::Detach(*Fixture.LocalPlayer, Owner);
	bAttached = false;
	const int32 DetachedSamples = Samples;
	TestTrue(TEXT("detaching returns authority acceleration to ordinary idle input"),
		Fixture.Worlds.TickUntil([&Fixture]()
		{
			return Fixture.ServerMovement->GetCurrentAcceleration().IsNearlyZero();
		}, 120));
	Fixture.Worlds.TickAll(8);
	TestEqual(TEXT("detached provider is no longer sampled"), Samples, DetachedSamples);
	const FVector ResumedStart = Fixture.ServerPawn->GetActorLocation();
	Fixture.TickMovement(/*bStick*/ true, 30);
	TestTrue(TEXT("ordinary client input still moves the server after detach"),
		Fixture.ServerPawn->GetActorLocation().X > ResumedStart.X + 20.0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalPlayerHooksClientRelocationSpec,
	"WoT.VR.Hooks.H1.ClientHearsServerTeleport", LocalPlayerHooksNetworkFlags)

bool FWoTLocalPlayerHooksClientRelocationSpec::RunTest(const FString&)
{
	FLocalPlayerHooksNetworkWorlds Fixture;
	if (!Fixture.Initialize(*this)) return false;
	int32 Reports = 0;
	FVector ReportedAt = FVector::ZeroVector;
	FWoTLocalPlayerConsumer Consumer;
	Consumer.Lifecycle = [&Reports, &ReportedAt](EWoTLocalPlayerEvent Event,
		const FWoTLocalPlayerContext& Context)
	{
		if (Event == EWoTLocalPlayerEvent::Teleported)
		{
			++Reports;
			ReportedAt = Context.Pawn.IsValid() ? Context.Pawn->GetActorLocation() : FVector::ZeroVector;
		}
	};
	const FName Owner(TEXT("HooksNetworkRelocation"));
	if (!TestTrue(TEXT("the client attaches a lifecycle consumer"),
		FWoTLocalPlayerHooks::Attach(*Fixture.LocalPlayer, Owner, Consumer)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		FWoTLocalPlayerHooks::Detach(*Fixture.LocalPlayer, Owner);
	};
	Fixture.Worlds.TickAll(30);
	TestEqual(TEXT("ordinary play reports no relocation"), Reports, 0);

	// The server moves the client's body through a real teleporter, as a map
	// teleporter does; the owning client learns of it only by correction.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Exit = Fixture.ServerPawn->GetActorLocation() + FVector(1500.0, 0.0, 0.0);
	AWoTTeleporter* Teleporter = Fixture.Worlds.Server.GetWorld()->SpawnActor<AWoTTeleporter>(
		Exit, FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("server teleporter"), Teleporter)) return false;
	TestTrue(TEXT("the server teleporter accepts the client's body"), Teleporter->Accept(Fixture.ServerPawn));
	TestTrue(TEXT("the owning client hears the server's relocation"),
		Fixture.Worlds.TickUntil([&Reports]() { return Reports > 0; }, 180));
	TestTrue(TEXT("the report finds the client's body at the exit"), ReportedAt.Equals(Exit, 50.0));
	Fixture.Worlds.TickAll(60);
	TestEqual(TEXT("one relocation is reported once"), Reports, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalPlayerHooksClientContinuousMotionSpec,
	"WoT.VR.Hooks.H1.ClientContinuousMotionIsNotTeleport", LocalPlayerHooksNetworkFlags)

bool FWoTLocalPlayerHooksClientContinuousMotionSpec::RunTest(const FString&)
{
	FLocalPlayerHooksNetworkWorlds Fixture;
	if (!Fixture.Initialize(*this)) return false;
	int32 Reports = 0;
	int32 ForcedTurns = 0;
	FWoTLocalPlayerConsumer Consumer;
	Consumer.Lifecycle = [&Reports, &ForcedTurns](EWoTLocalPlayerEvent Event,
		const FWoTLocalPlayerContext&)
	{
		Reports += Event == EWoTLocalPlayerEvent::Teleported ? 1 : 0;
		ForcedTurns += Event == EWoTLocalPlayerEvent::ForcedTurnStarted ? 1 : 0;
	};
	const FName Owner(TEXT("HooksNetworkContinuousMotion"));
	if (!TestTrue(TEXT("the client attaches a lifecycle consumer"),
		FWoTLocalPlayerHooks::Attach(*Fixture.LocalPlayer, Owner, Consumer)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		FWoTLocalPlayerHooks::Detach(*Fixture.LocalPlayer, Owner);
	};
	Fixture.Worlds.TickAll(30);

	// Knockback: the server throws the client's body, and the owning client
	// learns it only through corrections.
	const FVector Thrown = Fixture.ClientPawn->GetActorLocation();
	Fixture.ServerPawn->LaunchCharacter(FVector(0.0, 900.0, 300.0), true, true);
	Fixture.Worlds.TickAll(60);
	TestTrue(TEXT("the knockback reaches the owning client"),
		FVector::Dist(Fixture.ClientPawn->GetActorLocation(), Thrown) > 100.0);
	TestEqual(TEXT("knockback corrections are not teleports"), Reports, 0);

	// The spinning artifact's lift: the server moves the body every tick.
	Fixture.ResetToRest();
	UWoTWhirlwindLeech* Leech = NewObject<UWoTWhirlwindLeech>(Fixture.ServerPawn);
	Leech->AttachTo(Fixture.ServerPawn);
	Leech->SetDesiredLocation(Fixture.ServerPawn->GetActorLocation() + FVector(0.0, 0.0, 400.0));
	const FVector Lifted = Fixture.ClientPawn->GetActorLocation();
	Fixture.Worlds.TickAll(120);
	TestEqual(TEXT("the owning client hears the forced turn start"), ForcedTurns, 1);
	TestTrue(TEXT("the lift reaches the owning client"),
		Fixture.ClientPawn->GetActorLocation().Z > Lifted.Z + 100.0);
	TestEqual(TEXT("lift corrections are not teleports"), Reports, 0);
	Leech->Unattach();
	Fixture.Worlds.TickAll(30);
	TestEqual(TEXT("ending the lift is not a teleport"), Reports, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalPlayerHooksStationaryAimTransportSpec,
	"WoT.VR.Hooks.H2.StationaryAimTransport", LocalPlayerHooksNetworkFlags)

bool FWoTLocalPlayerHooksStationaryAimTransportSpec::RunTest(const FString&)
{
	FLocalPlayerHooksNetworkWorlds Fixture;
	if (!Fixture.Initialize(*this)) return false;
	FRotator Aim(23.0, 41.0, 0.0);
	FWoTLocalPlayerConsumer Consumer;
	Consumer.AimRotation = [&Aim](FRotator& Out)
	{
		Out = Aim;
		return true;
	};
	const FName Owner(TEXT("HooksNetworkAim"));
	if (!TestTrue(TEXT("stationary client attaches the aim provider"),
		FWoTLocalPlayerHooks::Attach(*Fixture.LocalPlayer, Owner, Consumer)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		FWoTLocalPlayerHooks::Detach(*Fixture.LocalPlayer, Owner);
	};
	const FVector Start = Fixture.ServerPawn->GetActorLocation();
	auto AuthorityHasCurrentAim = [&Fixture, &Aim]()
	{
		return Fixture.ServerController->GetControlRotation().Equals(Aim, 0.5);
	};
	TestTrue(TEXT("provider aim reaches the authority without movement or a cast RPC"),
		Fixture.Worlds.TickUntil(AuthorityHasCurrentAim, 180));
	Aim = FRotator(-17.0, 130.0, 0.0);
	TestTrue(TEXT("changing stationary aim keeps using existing view transport"),
		Fixture.Worlds.TickUntil(AuthorityHasCurrentAim, 180));
	Fixture.Worlds.TickAll(90);
	TestTrue(TEXT("held stationary provider pose remains the current server aim"),
		AuthorityHasCurrentAim());
	TestTrue(TEXT("stationary aim transport never displaces the authority"),
		Fixture.ServerPawn->GetActorLocation().Equals(Start, 0.5));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS && WITH_EDITOR
