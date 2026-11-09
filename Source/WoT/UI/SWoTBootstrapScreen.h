#pragma once

#include "Widgets/SCompoundWidget.h"
#include "../WoTBootstrapViewModel.h"

struct FSlateDynamicImageBrush;
class SButton;
class SProgressBar;
class STextBlock;

#ifndef WOT_BOOTSTRAP_TEST_IDENTITIES
#define WOT_BOOTSTRAP_TEST_IDENTITIES 0
#endif

/** Payload-free code-only Slate projection of one immutable snapshot. */
class SWoTBootstrapScreen final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTBootstrapScreen) {}
		SLATE_ATTRIBUTE(FWoTBootstrapSnapshot, Snapshot)
		SLATE_EVENT(FSimpleDelegate, OnContinueRequested)
		SLATE_EVENT(FSimpleDelegate, OnSkipRequested)
		SLATE_EVENT(FSimpleDelegate, OnExitRequested)
		SLATE_EVENT(FSimpleDelegate, OnBrowseInstaller)
		SLATE_EVENT(FSimpleDelegate, OnGetOriginalGame)
		/** "Copy from another computer" (Windows and Mac): start listening
		 * for WoT Reborn on another computer on this network. */
		SLATE_EVENT(FSimpleDelegate, OnCopyFromAnotherComputer)
		/** Cancel during a LAN copy. */
		SLATE_EVENT(FSimpleDelegate, OnCancelInstallerCopy)
		SLATE_EVENT(FSimpleDelegate, OnReady)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry,
		double InCurrentTime, float InDeltaTime) override;
	TSharedPtr<SWidget> InitialFocusWidget() const;
	void FocusInitialWidget();

	FText PhaseText() const;
	FText ProgressText() const;
	FText DiagnosticText() const;
	FText OriginalGameText() const;
	// The step-by-step GOG instructions, shown after "Get it on GOG".
	FText GogStepsText() const;
	EVisibility GogStepsVisibility() const;
	/** The LAN copy line (spec B1): the hint, waiting, damaged, three
	 * strikes or not enough space. Empty while a copy runs (the status line
	 * shows it) and where receiving is unavailable. */
	FText InstallerCopyText() const;
	EVisibility InstallerCopyTextVisibility() const;
	EVisibility CopyFromAnotherComputerVisibility() const;
	EVisibility CancelInstallerCopyVisibility() const;
	/** Pure text rules; bWindows adds the firewall line ("click Allow"). */
	static FText InstallerCopyTextFor(const FWoTBootstrapSnapshot& Snapshot,
		bool bWindows);
	/** "Copying the original game installer from <label>... NN%". */
	static FText InstallerCopyProgressTextFor(const FWoTBootstrapSnapshot& Snapshot);
	TOptional<float> PhaseProgress() const;
	EVisibility OriginalGameTextVisibility() const;
	EVisibility OriginalGameActionVisibility() const;
	EVisibility ContinueVisibility() const;
	EVisibility SkipVisibility() const;
	EVisibility ExitVisibility() const;
	bool CanContinueOptionalWork() const;
	bool CanSkipOptionalWork() const;
	bool CanExit() const;
	TSharedPtr<SWidget> InitialFocusWidgetForTesting() const
		{ return InitialFocusWidget(); }
	/** The phase title, "<step> failed" on the failure screen. */
	TSharedPtr<STextBlock> TitleWidgetForTesting() const;
	TSharedPtr<SWidget> ContinueWidgetForTesting() const;
	TSharedPtr<SWidget> SkipWidgetForTesting() const;
	TSharedPtr<SWidget> ExitWidgetForTesting() const;
	TSharedPtr<SWidget> BrowseInstallerWidgetForTesting() const;
	TSharedPtr<SWidget> GetOriginalGameWidgetForTesting() const;
	TSharedPtr<SWidget> CopyFromAnotherComputerWidgetForTesting() const;
	TSharedPtr<SWidget> CancelInstallerCopyWidgetForTesting() const;
#if WITH_AUTOMATION_TESTS
	FReply BrowseInstallerForTesting() { return BrowseInstaller(); }
	FReply GetOriginalGameForTesting() { return GetOriginalGame(); }
	FReply CopyFromAnotherComputerForTesting() { return CopyFromAnotherComputer(); }
	FReply CancelInstallerCopyForTesting() { return CancelInstallerCopy(); }
#endif
	/** The content column width for the last arranged screen width. */
	float ContentWidthForTesting(float ScreenWidth) const;
	bool HasBackdropBrushForTesting() const { return BackdropBrush.IsValid(); }

private:
	FWoTBootstrapSnapshot GetSnapshot() const;
	FReply Continue();
	FReply Skip();
	FReply Exit();
	FReply BrowseInstaller();
	FReply GetOriginalGame();
	FReply CopyFromAnotherComputer();
	FReply CancelInstallerCopy();

	TAttribute<FWoTBootstrapSnapshot> SnapshotAttribute;
	FSimpleDelegate ContinueRequested;
	FSimpleDelegate SkipRequested;
	FSimpleDelegate ExitRequested;
	FSimpleDelegate BrowseInstallerRequested;
	FSimpleDelegate GetOriginalGameRequested;
	FSimpleDelegate CopyFromAnotherComputerRequested;
	FSimpleDelegate CancelInstallerCopyRequested;
	FSimpleDelegate Ready;
	TSharedPtr<SButton> ContinueButton;
	TSharedPtr<SButton> SkipButton;
	TSharedPtr<SButton> ExitButton;
	TSharedPtr<SButton> BrowseInstallerButton;
	TSharedPtr<SButton> GetOriginalGameButton;
	TSharedPtr<SButton> CopyFromAnotherComputerButton;
	TSharedPtr<SButton> CancelInstallerCopyButton;
	TSharedPtr<STextBlock> TitleText;
	TSharedPtr<SProgressBar> ProgressBar;
	// The backdrop behind everything: the saved blurred main-menu scene, else
	// the shipped default, else the generated gradient (WoTSetupBackdrop).
	TSharedPtr<FSlateDynamicImageBrush> BackdropBrush;
	float ArrangedWidth = 0.0f;
	bool bReadyAnnounced = false;
	bool bShowGogSteps = false;
#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
	EWoTBootstrapPhase LastAutoBrowsePhase = EWoTBootstrapPhase::Ready;
	bool bAutoBrowseInvoked = false;
#endif
};
