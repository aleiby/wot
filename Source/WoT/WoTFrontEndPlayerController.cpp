#include "WoTFrontEndPlayerController.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/Paths.h"
#include "Platform/WoTInstallerPicker.h"
#include "UI/SWoTBootstrapScreen.h"
#include "UI/WoTSetupBackdrop.h"
#include "TimerManager.h"
#include "Engine/GameInstance.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTBootstrapSubsystem.h"
#include "WoTFrontEnd.h"
#include "WoTPayloadSubsystem.h"
#include "World/WoTWorldMaterializer.h"
#include "WoTMenuStyle.h"
#include "WoTSessionSubsystem.h"
#include "WoTFrontEndSlate.h"
#include "Updater/WoTInstallerLocations.h"
#include "Updater/WoTGameBootstrapWorker.h"
#include "Updater/WoTReleaseIdentity.h"
#include "WoTUpdaterCore/UpdaterPaths.h"

#include <cstdlib>

namespace
{
#if WITH_AUTOMATION_TESTS
FWoTFrontEndHandoffTestAdapter* GWoTFrontEndHandoffTestAdapter = nullptr;
#endif

void WoTObserveFrontEndHandoffForTesting(FStringView Event)
{
#if WITH_AUTOMATION_TESTS
	if (GWoTFrontEndHandoffTestAdapter != nullptr
		&& GWoTFrontEndHandoffTestAdapter->ObserveEvent)
	{
		GWoTFrontEndHandoffTestAdapter->ObserveEvent(Event);
	}
#endif
}

bool WoTRecordFrontEndHandoffResult(const FWoTBootstrapSnapshot& Snapshot,
	bool bSucceeded, FStringView Detail)
{
#if WOT_GUARDED_BOOTSTRAP
	// Reaching the menu completes an update trial whether or not the
	// diagnostic log write below succeeds; otherwise a full disk at this
	// moment would make the next launch roll back a healthy update.
	if (bSucceeded)
	{
		const auto Layout = WoTResolveExecutableLayout();
		auto Root = ResolveUpdaterStateRoot(Layout.UserStateRoot, Layout.InstallRoot, Layout.LiveLeaf);
		if (!Root.IsError())
		{
			auto Completed = WoTCompleteUpdateTrial(Root.GetValue(), WoTReleaseIdentity::LoadRunning().ReleaseSequence);
			if (Completed.IsError())
				WoTRecordBootstrapHeartbeat(TEXT("update-trial-cleanup-failed"), Snapshot.Phase,
					Snapshot.OperationEpoch, {}, {}, Completed.GetError().ToString());
		}
	}
#endif
	(void)WoTRecordBootstrapHeartbeat(
		TEXT("span-end"), Snapshot.Phase,
		Snapshot.OperationEpoch, {}, {},
		bSucceeded
			? TEXT("name=scene-preparation-handoff result=success")
			: TEXT("name=scene-preparation-handoff result=failure"));
	(void)WoTRecordBootstrapHeartbeat(
		bSucceeded ? TEXT("frontend-handoff")
			: TEXT("frontend-handoff-refused"), Snapshot.Phase,
		Snapshot.OperationEpoch, {}, {}, Detail);
	WoTObserveFrontEndHandoffForTesting(bSucceeded
		? TEXT("success-heartbeat") : TEXT("refused-heartbeat"));
	return bSucceeded;
}
}

#if WITH_AUTOMATION_TESTS
void SetWoTFrontEndHandoffTestAdapter(
	FWoTFrontEndHandoffTestAdapter* Adapter)
{
	GWoTFrontEndHandoffTestAdapter = Adapter;
}
#endif

void AWoTFrontEndPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// Automation and server/headless worlds must never initialize Slate. Explicit
	// map URLs used by the net-test harness select gameplay modes/controllers, but
	// these guards also make an accidental menu-world launch harmless.
	if (!IsLocalController() || GIsAutomationTesting || IsRunningDedicatedServer()
		|| GetNetMode() == NM_DedicatedServer || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	WidgetViewport = GEngine->GameViewport;
	ConstructAuthorizedWidget();
	if (!RootWidget.IsValid() && !BootstrapWidget.IsValid())
	{
		return;
	}

	bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	const TSharedPtr<SWidget> InitialFocus = RootWidget.IsValid()
		? RootWidget->InitialFocusWidget()
		: BootstrapWidget->InitialFocusWidget();
	InputMode.SetWidgetToFocus(InitialFocus.IsValid()
		? InitialFocus
		: (RootWidget.IsValid()
			? StaticCastSharedPtr<SWidget>(RootWidget)
			: StaticCastSharedPtr<SWidget>(BootstrapWidget)));
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	if (RootWidget.IsValid())
	{
		RootWidget->FocusInitialWidget();
	}
	else
	{
		BootstrapWidget->FocusInitialWidget();
	}
}

void AWoTFrontEndPlayerController::ConstructAuthorizedWidget()
{
	UGameInstance* Instance = GetGameInstance();
	if (Instance != nullptr)
	{
		if (UWoTBootstrapSubsystem* Bootstrap =
			Instance->GetSubsystem<UWoTBootstrapSubsystem>())
		{
			if (!Bootstrap->EnsureFrontEndSceneForWorld(*GetWorld(), this))
			{
				UE_LOG(LogTemp, Error,
					TEXT("Authenticated front-end scene rematerialization failed"));
				const UWoTSessionSubsystem* Sessions =
					Instance->GetSubsystem<UWoTSessionSubsystem>();
				Bootstrap->HandleRuntimeCacheIntegrityFailure(
					Sessions != nullptr
						? Sessions->RetainedConnectionFailure()
						: FString(TEXT("Some of WoT Reborn's game data is damaged. It will be rebuilt the next time you start the game.")),
					false);
			}
		}
	}
	if (FWoTFrontEndModel::CanConstructNormalFrontEnd(Instance))
	{
		const UWoTBootstrapSubsystem* Bootstrap =
			Instance->GetSubsystem<UWoTBootstrapSubsystem>();
		const FWoTBootstrapSnapshot Snapshot = Bootstrap != nullptr
			? Bootstrap->Snapshot() : FWoTBootstrapSnapshot();
		(void)RunFrontEndHandoff(EWoTFrontEndHandoffPath::DirectAuthorized,
			Snapshot,
			TEXT("widget=1 authorized=1 ready=1 world_authority=1"));
		return;
	}
	UWoTBootstrapSubsystem* Bootstrap = Instance
		? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr;
	if (Bootstrap == nullptr || Bootstrap->HasInitializationFailed())
	{
		// Dependency/session failure is deliberately fail-closed with no worker
		// and no bootstrap or normal-front-end projection.
		return;
	}
	BootstrapWidget = SNew(SWoTBootstrapScreen)
		.Snapshot_Lambda([WeakBootstrap = TWeakObjectPtr<UWoTBootstrapSubsystem>(Bootstrap)]
		{
			return WeakBootstrap.IsValid()
				? WeakBootstrap->Snapshot() : FWoTBootstrapSnapshot();
		})
		.OnExitRequested(FSimpleDelegate::CreateUObject(
			this, &AWoTFrontEndPlayerController::HandleBootstrapExit))
		.OnBrowseInstaller(FSimpleDelegate::CreateUObject(
			this, &AWoTFrontEndPlayerController::HandleBrowseInstaller))
		.OnGetOriginalGame(FSimpleDelegate::CreateUObject(
			this, &AWoTFrontEndPlayerController::HandleGetOriginalGame))
		.OnCopyFromAnotherComputer(FSimpleDelegate::CreateUObject(
			this, &AWoTFrontEndPlayerController::HandleCopyFromAnotherComputer))
		.OnCancelInstallerCopy(FSimpleDelegate::CreateUObject(
			this, &AWoTFrontEndPlayerController::HandleCancelInstallerCopy))
		.OnReady(FSimpleDelegate::CreateUObject(
			this, &AWoTFrontEndPlayerController::HandleBootstrapReady));
	WidgetViewport->AddViewportWidgetContent(
		BootstrapWidget.ToSharedRef(), /*ZOrder*/ 1000);
#if WITH_AUTOMATION_TESTS
	Bootstrap->RecordScreenConstructionForTesting();
#endif
}

void AWoTFrontEndPlayerController::HandleBootstrapReady()
{
	const bool bHasWidget = BootstrapWidget.IsValid();
	const bool bAuthorized =
		FWoTFrontEndModel::CanConstructNormalFrontEnd(GetGameInstance());
	// The packaged Test build compiles logging out, so a refused hand-off is
	// invisible: the bootstrap screen simply stays up and the player sees a
	// black panel over the world. Record which
	// half of the gate refused.
	const UGameInstance* Instance = GetGameInstance();
	const UWoTBootstrapSubsystem* Bootstrap = Instance
		? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr;
	const FWoTBootstrapSnapshot Snapshot = Bootstrap != nullptr
		? Bootstrap->Snapshot() : FWoTBootstrapSnapshot();
	const FString HandoffDetail = FString::Printf(
		TEXT("widget=%d authorized=%d ready=%d world_authority=%d"),
		bHasWidget ? 1 : 0, bAuthorized ? 1 : 0,
		Bootstrap != nullptr && Bootstrap->IsReady() ? 1 : 0,
		IsWoTNormalWorldAuthorityAdopted() ? 1 : 0);
	if (!bHasWidget || !bAuthorized)
	{
		(void)WoTRecordFrontEndHandoffResult(
			Snapshot, false, HandoffDetail);
		return;
	}
	(void)RunFrontEndHandoff(EWoTFrontEndHandoffPath::BootstrapSwap,
		Snapshot, HandoffDetail);
}

bool AWoTFrontEndPlayerController::PrepareFrontEndWidget(
	EWoTFrontEndHandoffPath Path)
{
#if WITH_AUTOMATION_TESTS
	if (GWoTFrontEndHandoffTestAdapter != nullptr)
	{
		return GWoTFrontEndHandoffTestAdapter->PrepareTextureConsumers
			&& GWoTFrontEndHandoffTestAdapter->PrepareTextureConsumers()
			&& GWoTFrontEndHandoffTestAdapter->BindTextureConsumers
			&& GWoTFrontEndHandoffTestAdapter->BindTextureConsumers()
			&& GWoTFrontEndHandoffTestAdapter->PrepareWidget
			&& GWoTFrontEndHandoffTestAdapter->PrepareWidget(Path);
	}
#endif
	UGameViewportClient* Viewport = WidgetViewport.Get();
	if (Viewport == nullptr)
	{
		return false;
	}
#if WOT_GUARDED_BOOTSTRAP
	UGameInstance* OwnerInstance = GetGameInstance();
	UWoTPayloadSubsystem* Payload = OwnerInstance != nullptr
		? OwnerInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (Payload == nullptr)
	{
		return false;
	}
	TWoTBootstrapResult<TArray<UTexture2D*>> PreparedTextures =
		Payload->PrepareFrontEndMenuTextures();
	if (PreparedTextures.IsError()
		|| PreparedTextures.GetValue().Num() != 6
		|| !FWoTMenuStyle::Get().BindTextureConsumers(
			PreparedTextures.GetValue()))
	{
		return false;
	}
#endif
	if (Path == EWoTFrontEndHandoffPath::BootstrapSwap)
	{
		if (!BootstrapWidget.IsValid())
		{
			return false;
		}
		Viewport->RemoveViewportWidgetContent(BootstrapWidget.ToSharedRef());
		BootstrapWidget.Reset();
	}
	RootWidget = SNew(SWoTFrontEndRoot).PlayerController(this);
	Viewport->AddViewportWidgetContent(RootWidget.ToSharedRef(), 1000);
	if (Path == EWoTFrontEndHandoffPath::BootstrapSwap)
	{
		RootWidget->FocusInitialWidget();
	}
	return true;
}

bool AWoTFrontEndPlayerController::CompletePostWidgetLoad(
	const FWoTBootstrapSnapshot& ReadySnapshot, FStringView Detail)
{
	bool bPayloadAvailable = false;
	bool bRebound = false;
	bool bRefreshed = false;
#if WITH_AUTOMATION_TESTS
	if (GWoTFrontEndHandoffTestAdapter != nullptr)
	{
		bPayloadAvailable =
			GWoTFrontEndHandoffTestAdapter->bPayloadSubsystemAvailable;
		bRebound = bPayloadAvailable
			&& GWoTFrontEndHandoffTestAdapter->RebindLoadedPayloads
			&& GWoTFrontEndHandoffTestAdapter->RebindLoadedPayloads();
		if (bRebound)
		{
			WoTObserveFrontEndHandoffForTesting(TEXT("payload-rebind"));
		}
		bRefreshed = bRebound
			&& GWoTFrontEndHandoffTestAdapter->RefreshTextureBrushes
			&& GWoTFrontEndHandoffTestAdapter->RefreshTextureBrushes();
		if (bRefreshed)
		{
			WoTObserveFrontEndHandoffForTesting(TEXT("brush-refresh"));
		}
	}
	else
#endif
	{
		UGameInstance* OwnerInstance = GetGameInstance();
		UWoTPayloadSubsystem* Payload = OwnerInstance != nullptr
			? OwnerInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
		bPayloadAvailable = Payload != nullptr;
		bRebound = bPayloadAvailable && RootWidget.IsValid()
			&& Payload->RebindLoadedPayloads(
				RootWidget->RequiredTexturePayloads());
		bRefreshed = bRebound && RootWidget.IsValid()
			&& RootWidget->RefreshTextureBrushes();
	}
	if (!bPayloadAvailable || !bRebound || !bRefreshed)
	{
		FString RefusalDetail(Detail);
		RefusalDetail += FString::Printf(
			TEXT(" payload=%d rebound=%d refreshed=%d"),
			bPayloadAvailable ? 1 : 0, bRebound ? 1 : 0,
			bRefreshed ? 1 : 0);
		(void)WoTRecordFrontEndHandoffResult(
			ReadySnapshot, false, RefusalDetail);
		return false;
	}
	return WoTRecordFrontEndHandoffResult(
		ReadySnapshot, true, Detail);
}

bool AWoTFrontEndPlayerController::RunFrontEndHandoff(
	EWoTFrontEndHandoffPath Path,
	const FWoTBootstrapSnapshot& ReadySnapshot, FStringView Detail)
{
	if (bFrontEndHandoffPublished)
	{
		return true;
	}
	if (!PrepareFrontEndWidget(Path))
	{
		FString RefusalDetail(Detail);
		RefusalDetail += TEXT(" reason=widget-preparation-failed");
		(void)WoTRecordFrontEndHandoffResult(
			ReadySnapshot, false, RefusalDetail);
		return false;
	}
	bFrontEndHandoffPublished =
		CompletePostWidgetLoad(ReadySnapshot, Detail);
	if (bFrontEndHandoffPublished && GetWorld() != nullptr)
	{
		// The scene's sky and weather settle in the first moments.
		GetWorldTimerManager().SetTimer(SetupBackdropTimer, this,
			&AWoTFrontEndPlayerController::CaptureSetupBackdrop, 3.0f, false);
	}
	return bFrontEndHandoffPublished;
}

void AWoTFrontEndPlayerController::CaptureSetupBackdrop()
{
	WoTSetupBackdrop::CaptureIfStale(this);
}

#if WITH_AUTOMATION_TESTS
bool AWoTFrontEndPlayerController::RunFrontEndHandoffForTesting(
	EWoTFrontEndHandoffPath Path,
	const FWoTBootstrapSnapshot& ReadySnapshot)
{
	return RunFrontEndHandoff(Path, ReadySnapshot,
		TEXT("widget=1 authorized=1 ready=1 world_authority=1"));
}
#endif

void AWoTFrontEndPlayerController::WoTHost(bool bLan)
{
	UWoTSessionSubsystem* S = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	WoTRecordBootstrapHeartbeat(TEXT("console-verb"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("WoTHost frontend lan=%d subsystem=%d"),
			bLan ? 1 : 0, S != nullptr ? 1 : 0));
	if (S == nullptr)
	{
		return;
	}
	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	Request.bLAN = bLan;
	// NEVER call inside UE_LOG arguments: the guarded packaged build compiles
	// logging out (NO_LOGGING) and the arguments are not evaluated - the
	// host-join funnel cell failed exactly this way.
	const bool bHosting = S->Host(Request);
	UE_LOG(LogNet, Display, TEXT("WoTHost (front end) -> %s"),
		bHosting ? TEXT("creating") : TEXT("refused"));
}

void AWoTFrontEndPlayerController::WoTFind(bool bLan)
{
	UWoTSessionSubsystem* S = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (S == nullptr)
	{
		return;
	}
	const bool bFinding = S->Find(bLan);
	// Durable: the joiner half is invisible otherwise, and the packaged build
	// compiles logging out.
	WoTRecordBootstrapHeartbeat(TEXT("console-verb"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("WoTFind frontend lan=%d searching=%d"),
			bLan ? 1 : 0, bFinding ? 1 : 0));
	UE_LOG(LogNet, Display, TEXT("WoTFind (front end) -> %s"),
		bFinding ? TEXT("searching") : TEXT("refused"));
}

void AWoTFrontEndPlayerController::WoTJoin(int32 Index)
{
	UWoTSessionSubsystem* S = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (S == nullptr)
	{
		return;
	}
	const bool bJoining = S->Join(Index);
	WoTRecordBootstrapHeartbeat(TEXT("console-verb"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("WoTJoin frontend idx=%d joining=%d"),
			Index, bJoining ? 1 : 0));
	UE_LOG(LogNet, Display, TEXT("WoTJoin (front end) %d -> %s"), Index,
		bJoining ? TEXT("joining") : TEXT("refused"));
}

void AWoTFrontEndPlayerController::WoTLeave()
{
	UWoTSessionSubsystem* S = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (S == nullptr)
	{
		return;
	}
	const bool bLeaving = S->Leave();
	UE_LOG(LogNet, Display, TEXT("WoTLeave (front end) -> %s"),
		bLeaving ? TEXT("leaving") : TEXT("refused"));
}

void AWoTFrontEndPlayerController::HandleBootstrapExit()
{
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (UWoTBootstrapSubsystem* Bootstrap =
			Instance->GetSubsystem<UWoTBootstrapSubsystem>())
		{
			if (Bootstrap->ShouldExitWithFailure())
			{
				std::_Exit(EXIT_FAILURE);
			}
			Bootstrap->RequestResumeAndExit();
		}
	}
	UKismetSystemLibrary::QuitGame(
		this, this, EQuitPreference::Quit, /*bIgnoreRestrictions*/ false);
}

void AWoTFrontEndPlayerController::HandleBrowseInstaller()
{
	UGameInstance* Instance = GetGameInstance();
	UWoTBootstrapSubsystem* Bootstrap = Instance
		? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr;
	if (Bootstrap == nullptr
		|| Bootstrap->Snapshot().Phase
			!= EWoTBootstrapPhase::OriginalGameNeeded)
	{
		return;
	}
	TWeakObjectPtr<UWoTBootstrapSubsystem> WeakBootstrap(Bootstrap);
	WoTShowInstallerPicker(WoTResolveDownloadsDirectory(),
		[WeakBootstrap](const FString& Path)
		{
			if (WeakBootstrap.IsValid())
			{
				WeakBootstrap->RequestPickedInstaller(Path);
			}
		});
}

void AWoTFrontEndPlayerController::HandleGetOriginalGame()
{
	FPlatformProcess::LaunchURL(
		TEXT("https://www.gog.com/en/game/the_wheel_of_time"), nullptr, nullptr);
}

void AWoTFrontEndPlayerController::HandleCopyFromAnotherComputer()
{
	UGameInstance* Instance = GetGameInstance();
	if (UWoTBootstrapSubsystem* Bootstrap = Instance
			? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
	{
		Bootstrap->RequestInstallerCopy();
	}
}

void AWoTFrontEndPlayerController::HandleCancelInstallerCopy()
{
	UGameInstance* Instance = GetGameInstance();
	if (UWoTBootstrapSubsystem* Bootstrap = Instance
			? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr)
	{
		Bootstrap->CancelInstallerCopy();
	}
}

void AWoTFrontEndPlayerController::EndPlay(const EEndPlayReason::Type Reason)
{
	if (GetWorld() != nullptr)
	{
		GetWorldTimerManager().ClearTimer(SetupBackdropTimer);
	}
	WoTSetupBackdrop::CancelPendingCapture();
	RemoveFrontEndWidget();
	Super::EndPlay(Reason);
}

void AWoTFrontEndPlayerController::RemoveFrontEndWidget()
{
	if (RootWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = WidgetViewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(RootWidget.ToSharedRef());
		}
		RootWidget.Reset();
	}
	if (BootstrapWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = WidgetViewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(BootstrapWidget.ToSharedRef());
		}
		BootstrapWidget.Reset();
	}
	WidgetViewport.Reset();
	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
}
