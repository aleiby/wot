#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "WoTFrontEndPlayerController.generated.h"

class SWoTFrontEndRoot;
class SWoTBootstrapScreen;
class UGameViewportClient;
struct FWoTBootstrapSnapshot;

enum class EWoTFrontEndHandoffPath : uint8
{
	DirectAuthorized,
	BootstrapSwap,
};

#if WITH_AUTOMATION_TESTS
struct FWoTFrontEndHandoffTestAdapter
{
	bool bPayloadSubsystemAvailable = true;
	TFunction<bool()> PrepareTextureConsumers;
	TFunction<bool()> BindTextureConsumers;
	TFunction<bool(EWoTFrontEndHandoffPath)> PrepareWidget;
	TFunction<bool()> RebindLoadedPayloads;
	TFunction<bool()> RefreshTextureBrushes;
	TFunction<void(FStringView)> ObserveEvent;
};

void SetWoTFrontEndHandoffTestAdapter(
	FWoTFrontEndHandoffTestAdapter* Adapter);
#endif

UCLASS()
class AWoTFrontEndPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// The session console verbs, front-end edition. The in-game verbs live
	// on AWoTPlayerController, which does not exist at the authorized front
	// end — where the funnel's host-join cell (and any console-driven Play
	// screen use) needs them. WoTHost mirrors the Play screen's host action:
	// default rules on the canonical first arena.
	UFUNCTION(Exec) void WoTHost(bool bLan = false);
	UFUNCTION(Exec) void WoTFind(bool bLan = false);
	UFUNCTION(Exec) void WoTJoin(int32 Index = 0);
	UFUNCTION(Exec) void WoTLeave();

#if WITH_AUTOMATION_TESTS
	bool RunFrontEndHandoffForTesting(EWoTFrontEndHandoffPath Path,
		const FWoTBootstrapSnapshot& ReadySnapshot);
#endif
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	/** The live front-end root, for the net-test browser verbs only. */
	TSharedPtr<SWoTFrontEndRoot> GetFrontEndRootForHarness() const
		{ return RootWidget; }
#endif

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	void RemoveFrontEndWidget();
	void ConstructAuthorizedWidget();
	void HandleBootstrapReady();
	void HandleBootstrapExit();
	void HandleBrowseInstaller();
	void HandleGetOriginalGame();
	void HandleCopyFromAnotherComputer();
	void HandleCancelInstallerCopy();
	bool RunFrontEndHandoff(EWoTFrontEndHandoffPath Path,
		const FWoTBootstrapSnapshot& ReadySnapshot, FStringView Detail);
	bool PrepareFrontEndWidget(EWoTFrontEndHandoffPath Path);
	bool CompletePostWidgetLoad(
		const FWoTBootstrapSnapshot& ReadySnapshot, FStringView Detail);
	// Once the main-menu scene is up, capture it (blurred) as the setup
	// screen's backdrop when the saved one is missing or predates this build.
	void CaptureSetupBackdrop();

	TSharedPtr<SWoTFrontEndRoot> RootWidget;
	TSharedPtr<SWoTBootstrapScreen> BootstrapWidget;
	TWeakObjectPtr<UGameViewportClient> WidgetViewport;
	bool bFrontEndHandoffPublished = false;
	FTimerHandle SetupBackdropTimer;
};
