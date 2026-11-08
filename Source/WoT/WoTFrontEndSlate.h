#pragma once

#include "CoreMinimal.h"
#include "Online/WoTKeychainAccess.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "UI/WoTUpdateBanner.h"
#include "WoTFrontEndViewModel.h"

class AWoTFrontEndPlayerController;
class SBox;
class SButton;
class SEditableTextBox;
class STextBlock;
class SWoTSettingsScreen;
class UWoTPreferencesSubsystem;
class STableViewBase;
class SWoTMenuButton;
class UWoTGameInstance;
class UWoTSessionSubsystem;
class UWoTBootstrapSubsystem;
class UTexture2D;
enum class EWoTBrowserSortColumn : uint8;
struct FWoTBrowserSortState;
struct FWoTRequiredMenuFonts;
struct FWoTReleaseNotesResult;
class FWoTFrontEndMenuSelection;
class FWoTFrontEndPresentationAuthority;
class FWoTFrontEndBackInputProcessor;
class FWoTBackStackEntryHandle;
class FWoTCursorLease;

enum class EWoTFrontEndMainMenuAction : uint8
{
	Multiplayer,
	Controls,
	Hardware,
	Quit
};

#if WITH_AUTOMATION_TESTS
DECLARE_DELEGATE_OneParam(FWoTFrontEndActionObserver,
	EWoTFrontEndMainMenuAction);
DECLARE_DELEGATE_OneParam(FWoTReleasePageLaunchObserver, const FString&);
#endif

struct FWoTFrontEndMainMenuEntry
{
	FName LocalizationId;
	EWoTFrontEndMainMenuAction Action;
};

// One controller-owned Slate root. Screen widgets are rebuilt from the pure
// view model, so no widget or world reference survives menu-world EndPlay.
class SWoTFrontEndRoot : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTFrontEndRoot) {}
		SLATE_ARGUMENT(AWoTFrontEndPlayerController*, PlayerController)
#if WITH_AUTOMATION_TESTS
		SLATE_ARGUMENT(TSharedPtr<FWoTFrontEndPresentationAuthority>,
			PresentationAuthorityForTesting)
		SLATE_EVENT(FWoTFrontEndActionObserver,
			ActionObserverForTesting)
		SLATE_ARGUMENT_DEFAULT(UWoTPreferencesSubsystem*, PreferencesForTesting) = nullptr;
		SLATE_ARGUMENT_DEFAULT(UWoTBootstrapSubsystem*, BootstrapForTesting) = nullptr;
		SLATE_EVENT(FWoTReleasePageLaunchObserver, ReleasePageLaunchForTesting)
		SLATE_ARGUMENT_DEFAULT(bool, ShowResetIdentityForTesting) = false;
		SLATE_ARGUMENT_DEFAULT(bool, ShowOnlineDisclosureForTesting) = false;
		SLATE_ARGUMENT_DEFAULT(FText, InternetStatusTextForTesting) = FText::GetEmpty();
#endif
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SWoTFrontEndRoot() override;
	TSharedPtr<SWidget> InitialFocusWidget() const;
	void FocusInitialWidget();
	bool RefreshTextureBrushes();
	TArray<UTexture2D*> RequiredTexturePayloads() const;
	static TConstArrayView<FWoTFrontEndMainMenuEntry> RetailMainEntries();
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnPreviewKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyDown(
		const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void Tick(
		const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

#if WITH_AUTOMATION_TESTS
	FReply ActivateMainMenuEntryForTesting(int32 EntryIndex)
		{ return ActivateMainMenuEntry(EntryIndex); }
	EWoTFrontEndScreen CurrentScreenForTesting() const
		{ return ViewModel.GetScreen(); }
	FVector2D MainMenuListSizeForTesting() const
		{ return FVector2D(MainMenuListWidth, MainMenuListHeight); }
	float MainMenuSafeWidthForTesting() const { return MainMenuSafeWidth; }
	TSharedPtr<SWoTMenuButton> MainMenuButtonForTesting(int32 Index) const
		{ return MainMenuButtons.IsValidIndex(Index) ? MainMenuButtons[Index] : nullptr; }
	TSharedPtr<SWidget> MapComboBoxForTesting() const { return MapComboBox; }
	TSharedPtr<SWidget> HostScrollBoxForTesting() const { return HostScrollBox; }
	TSharedPtr<SWidget> HostMatchButtonForTesting() const { return HostMatchButton; }
	/** Selects the first catalog map of the requested kind, as the map picker does. */
	bool SelectHostMapKindForTesting(bool bBattle) { return SelectFirstHostMapOfKind(bBattle); }
	static FText HostMapPickerTextForTesting(const FWoTFrontEndMapEntry* Entry)
		{ return HostMapPickerText(Entry); }
	TSharedPtr<SWidget> SessionListViewForTesting() const { return SessionListView; }
	uint64 ScreenRebuildCountForTesting() const { return ScreenRebuildCount; }
	TSharedPtr<SWoTSettingsScreen> SettingsScreenForTesting() const { return ActiveSettingsScreen; }
	TSharedPtr<SWidget> PlayerConfigButtonForTesting() const { return PlayerConfigButton; }
	TSharedPtr<STextBlock> InternetStatusTextWidgetForTesting() const
		{ return InternetStatusTextWidget; }
	TSharedPtr<STextBlock> ResetIdentityTextWidgetForTesting() const
		{ return ResetIdentityTextWidget; }
	TSharedPtr<SWidget> OnlineDisclosureWidgetForTesting() const
		{ return OnlineDisclosureWidget; }
	FSlateFontInfo LargeBodyFontForTesting() const { return LargeBodyFont(); }
	FSlateFontInfo PlayBodyFontForTesting() const { return PlayBodyFont(); }
	FSlateFontInfo PlayHeadingFontForTesting() const { return PlayHeadingFont(); }
	/** The Internet choice exactly as the radio's check-state handler runs it. */
	void SelectNetworkModeForTesting(bool bInternet) { SetNetworkMode(bInternet); }
	bool IsOnlineDisclosureVisibleForTesting() const { return bShowOnlineDisclosure; }
	FReply AcceptOnlineDisclosureForTesting() { return AcceptOnlineDisclosure(); }
	FReply CancelOnlineDisclosureForTesting() { return CancelOnlineDisclosure(); }
	bool IsLocalNetworkNoticeVisibleForTesting() const { return bShowLocalNetworkNotice; }
	TSharedPtr<SWidget> LocalNetworkNoticeWidgetForTesting() const
		{ return LocalNetworkNoticeWidget; }
	FReply OpenLocalNetworkSettingsForTesting() { return OpenLocalNetworkSettings(); }
	FReply DismissLocalNetworkNoticeForTesting() { return DismissLocalNetworkNotice(); }
	bool IsLocalNetworkHeadsUpVisibleForTesting() const
		{ return PendingLocalNetworkAction != ELocalNetworkAction::None; }
	TSharedPtr<SWidget> LocalNetworkHeadsUpWidgetForTesting() const
		{ return LocalNetworkHeadsUpWidget; }
	FReply ContinueLocalNetworkHeadsUpForTesting() { return ContinueLocalNetworkHeadsUp(); }
	FReply CancelLocalNetworkHeadsUpForTesting() { return CancelLocalNetworkHeadsUp(); }
	bool IsKeychainHeadsUpVisibleForTesting() const
		{ return PendingKeychainAction != EKeychainAction::None; }
	TSharedPtr<SWidget> KeychainHeadsUpWidgetForTesting() const
		{ return KeychainHeadsUpWidget; }
	FReply ContinueKeychainHeadsUpForTesting() { return ContinueKeychainHeadsUp(); }
	FReply CancelKeychainHeadsUpForTesting() { return CancelKeychainHeadsUp(); }
	bool IsReleaseNotesNoticeVisibleForTesting() const
		{ return bShowReleaseNotesNotice; }
	TSharedPtr<SWidget> ReleaseNotesNoticeWidgetForTesting() const
		{ return ReleaseNotesNoticeWidget; }
	TSharedPtr<SWidget> ReleaseNotesContinueWidgetForTesting() const
		{ return ReleaseNotesContinueWidget; }
	FReply ContinueReleaseNotesForTesting() { return ContinueReleaseNotes(); }
	bool IsUpdateFailedNoticeVisibleForTesting() const
		{ return bShowUpdateFailedNotice; }
	TSharedPtr<SWidget> UpdateFailedNoticeWidgetForTesting() const
		{ return UpdateFailedNoticeWidget; }
	TSharedPtr<SWidget> UpdateFailedContinueWidgetForTesting() const
		{ return UpdateFailedContinueWidget; }
	void ShowUpdateFailedNoticeForFocusTesting()
		{ bShowUpdateFailedNotice = true; }
	FReply ContinueUpdateFailedForTesting() { return ContinueUpdateFailed(); }
	/** What the session subsystem's keychain probe answers (Online/WoTKeychainAccess.h). */
	void SetKeychainAccessForTesting(TOptional<EWoTKeychainAccess> Access)
		{ KeychainAccessForTesting = Access; }
	void SetInternetModeForTesting() { ViewModel.SetLAN(false); }
	int32 KeychainReplaysForTesting() const { return KeychainReplayCountForTesting; }
	FReply BackFromPlayForTesting() { return BackFromPlay(); }
	/** The Refresh / Host / Direct Address buttons' handlers. */
	FReply RefreshSessionsForTesting() { return RefreshSessions(); }
	FReply HostForTesting() { return Host(); }
	void CutSearchShortForTesting() { CutSearchShort(); }
	bool IsSearchRestartPendingForTesting() const { return bRestartSearchWhenStaleLands; }
	FReply ConnectDirectForTesting() { return ConnectDirect(); }
	FReply OpenPlayForTesting() { return OpenPlay(); }
	FReply CancelServerPasswordForTesting() { return CancelServerPassword(); }
	FReply OpenPlayerConfigForTesting() { return OpenPlayerConfig(); }
	FReply SelectBrowserSortColumnForTesting(EWoTBrowserSortColumn Column)
		{ return SelectBrowserSortColumn(Column); }
	/** The state a Refresh puts the browser in before its search lands. */
	void BeginSearchForTesting() { ViewModel.BeginSearch(); }
	/** The browser's search completion exactly as the session delegate runs it. */
	void CompleteSearchForTesting(bool bSucceeded,
		const TArray<FWoTSessionListEntry>& Results)
		{ HandleSearchComplete(bSucceeded, Results); }
	const FWoTFrontEndViewModel& ViewModelForTesting() const { return ViewModel; }
	FText InternetStatusTextForTesting() const { return InternetStatusText(); }
	FString SessionPingTextForTesting(const FWoTSessionListEntry& Entry) const
		{ return SessionPingText(Entry); }
	FString SessionServerTextForTesting(const FWoTSessionListEntry& Entry) const
		{ return SessionServerText(Entry); }
	FString SessionVersionMismatchNoticeForTesting(
		const FWoTSessionListEntry& Entry) const
		{ return SessionVersionMismatchNotice(Entry); }
	FWoTUpdateBannerPresentation UpdateBannerPresentationForTesting() const
		{ return UpdateBannerPresentation(); }
	TSharedPtr<SWidget> UpdateBannerWidgetForTesting() const
		{ return UpdateBannerWidget; }
	TSharedPtr<SButton> UpdateBannerButtonForTesting() const
		{ return UpdateBannerButton; }
	FReply ActivateUpdateBannerForTesting() { return ActivateUpdateBanner(); }
	FString VersionLabelForTesting() const { return VersionLabel; }
	TSharedPtr<SWidget> VersionLabelWidgetForTesting() const
		{ return VersionLabelWidget; }
	/** True while a Play overlay covers the panels (they are disabled). */
	bool IsPlayOverlayVisibleForTesting() const { return IsPlayOverlayVisible(); }
	TSharedPtr<SWidget> PlayerConfigSaveButtonForTesting() const
		{ return PlayerConfigSaveButton; }
	TSharedPtr<SWidget> PlayBackButtonForTesting() const { return PlayBackButton; }
	TSharedPtr<SWidget> OnlineDisclosureCancelWidgetForTesting() const
		{ return OnlineDisclosureCancelWidget; }
	TSharedPtr<SWidget> OnlineDisclosurePrimaryWidgetForTesting() const
		{ return OnlineDisclosurePrimaryWidget; }
	TSharedPtr<SWidget> PasswordTextBoxForTesting() const { return PasswordTextBox; }
	/** The Play focus graph's columns, as last built. */
	const TArray<TArray<TSharedPtr<SWidget>>>& HostNavigationRowsForTesting() const
		{ return HostNavigationRows; }
	const TArray<TArray<TSharedPtr<SWidget>>>& JoinNavigationRowsForTesting() const
		{ return JoinNavigationRows; }
#endif

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	// Net-test harness seam over the REAL multiplayer browser (never compiled
	// into guarded Shipping): open the Play screen, set the LAN box and press
	// Refresh; select a listed row and press Join. Both run the exact handlers
	// the buttons bind, so a packaged two-instance run covers the browser join
	// (Find -> JoinSession -> resolved connect string -> travel) end to end.
	bool HarnessRefreshBrowser(bool bLan);
	bool HarnessJoinBrowserRow(int32 Row);
	/** Rows in the order the browser lists them (its current sort). */
	TArray<FWoTSessionListEntry> HarnessBrowserRows() const;
	EWoTFrontEndAsyncOperation HarnessBrowserOperation() const
		{ return ViewModel.GetOperation(); }
	FString HarnessBrowserStatus() const { return ViewModel.GetStatusText(); }
	// Pick the host panel's first Arena or Citadel map through the map list's
	// own SelectMap, so a packaged screenshot can show either layout.
	bool HarnessSelectHostMapKind(bool bBattle) { return SelectFirstHostMapOfKind(bBattle); }
	/** How far the host panel can scroll as last laid out: 0 when every row fits, -1 when it is not built. */
	float HarnessHostPanelScrollable() const;
#endif

private:
	/** Selects the first catalog map of the requested kind, as the map picker does. */
	bool SelectFirstHostMapOfKind(bool bBattle);
	friend class FWoTFrontEndBackInputProcessor;
	using FMapItem = TSharedPtr<FWoTFrontEndMapEntry>;
	using FSessionItem = TSharedPtr<FWoTSessionListEntry>;

	void BindSessionEvents();
	void RebuildScreen();
	void RebuildMapItems();
	void RebuildSessionItems();
	TSharedRef<SWidget> BuildMainScreen();
	TSharedRef<SWidget> BuildUpdateBanner();
	TSharedRef<SWidget> BuildPlayerConfigScreen();
	TSharedRef<SWidget> BuildSettingsPage(EWoTFrontEndScreen Screen);
	TSharedRef<SWidget> BuildPlayScreen();
	TSharedRef<SWidget> BuildHostPanel();
	TSharedRef<SWidget> BuildJoinPanel();
	TSharedRef<SWidget> BuildPasswordPrompt();

	FReply OpenPlay();
	FReply OpenPlayerConfig();
	FReply OpenControls();
	FReply OpenHardware();
	void CloseSettingsPage();
	UWoTPreferencesSubsystem* ResolvePreferences() const;
	UWoTBootstrapSubsystem* ResolveBootstrap() const;
	FWoTUpdateState ResolveUpdateState() const;
	FWoTUpdateBannerPresentation UpdateBannerPresentation() const;
	float UpdateBannerMaxWidth() const;
	float UpdateBannerContentWidth() const;
	float UpdateBannerButtonColumnWidthForFont(
		const FSlateFontInfo& Font) const;
	float UpdateBannerTextWrapWidthForFont(const FSlateFontInfo& Font) const;
	float UpdateBannerEstimatedHeightForFont(const FSlateFontInfo& Font) const;
	bool ShouldUseCompactUpdateBannerFont() const;
	FSlateFontInfo UpdateBannerBodyFont() const;
	float UpdateBannerTextWrapWidth() const;
	float UpdateBannerBottomMargin() const;
	bool IsUpdateBannerInteractive() const;
	TSharedPtr<SWidget> VisibleModalPrimaryWidget() const;
	void RepairUpdateBannerFocus();
	FReply ActivateUpdateBanner();
	void LaunchReleasePage(const FString& Url);
	FReply Quit();
	FReply ActivateMainMenuEntry(int32 EntryIndex);
	void SelectMainMenuEntry(int32 EntryIndex);
	FReply SavePlayerConfig();
	FReply CancelPlayerConfig();
	FReply BackFromPlay();
	FReply Host();
	FReply RefreshSessions();
	FReply JoinSelectedSession();
	/** The Refresh/Join button bodies; true when the operation was accepted. */
	bool TryRefreshSessions();
	bool TryJoinSelectedSession();
	FReply ConnectDirect();
	FReply SubmitServerPassword();
	FReply CancelServerPassword();
	void CommitServerPassword(const FText& Text,
		ETextCommit::Type CommitType);
	FString PasswordPromptText() const;
	FReply SelectBrowserSortColumn(EWoTBrowserSortColumn Column);

	TSharedRef<SWidget> GenerateMapWidget(FMapItem Item) const;
	void SelectMap(FMapItem Item, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> GenerateSessionRow(
		FSessionItem Item, const TSharedRef<STableViewBase>& OwnerTable) const;
	void SelectSession(FSessionItem Item, ESelectInfo::Type SelectInfo);
	void DoubleClickSession(FSessionItem Item);
	void SortSessionItems();
	void SetServerName(const FText& Text);
	void SetPlayerName(const FText& Text);
	void CommitPlayerName(const FText& Text, ETextCommit::Type CommitType);
	void SetDirectAddress(const FText& Text);
	void SetFragLimit(int32 Value);
	void SetTimeLimit(int32 Value);
	void SetSealGoal(int32 Value);
	void SetSealBudget(int32 Value);
	void SetMaxPlayers(int32 Value);
	void SetLAN(ECheckBoxState State);
	// Internet (EOS) vs LAN. Internet needs the one-time online-identity
	// disclosure; while it is unavailable (not configured, -NoEOS, SDK
	// missing) the radio stays on LAN and the status line says why.
	void SetNetworkMode(bool bInternet);
	/** The Refresh a player would press on entering Play or switching network. */
	void AutoFetchSessions();
	/** Drops a running search so Host or Direct Connect can go ahead. */
	void CutSearchShort();
	/** The LAN/Internet radios; their two check boxes join OutNavigationRow. */
	TSharedRef<SWidget> BuildNetworkModeRow(bool bBrowser,
		TArray<TSharedPtr<SWidget>>* OutNavigationRow = nullptr);
	TSharedRef<SWidget> BuildInternetOptions(bool bBrowser,
		TArray<TSharedPtr<SWidget>>* OutNavigationRow = nullptr);
	TSharedRef<SWidget> BuildOnlineDisclosure();
	ECheckBoxState InternetCheckState() const;
	ECheckBoxState AlwaysUseEpicRelayState() const;
	void SetAlwaysUseEpicRelay(ECheckBoxState State);
	bool CanChooseInternet() const;
	FText InternetStatusText() const;
	EVisibility ResetIdentityVisibility() const;
	FReply AcceptOnlineDisclosure();
	FReply CancelOnlineDisclosure();
	// macOS Local Network privacy (Online/WoTLocalNetworkAccess.h): probes after
	// an empty LAN search and before a LAN host; a denial opens the notice.
	bool ShowLocalNetworkNoticeIfDenied(const TCHAR* Trigger);
	TSharedRef<SWidget> BuildLocalNetworkNotice();
	FReply OpenLocalNetworkSettings();
	FReply DismissLocalNetworkNotice();
	// The one-time heads-up before the first local-network action where the
	// OS asks permission (macOS): explains the system prompt before it
	// appears, then runs the action the player pressed.
	enum class ELocalNetworkAction : uint8 { None, Refresh, Host, ConnectDirect };
	// The macOS keychain prompt an internet sign-in can raise (the game thread
	// blocks behind it, so nothing drawn after the request is ever seen):
	// explained first, and only when the probe says it is coming. Continue is
	// what starts the sign-in and so triggers the system dialog.
	// An internet Join never signs in (Join refuses internet-signed-out), so
	// only Refresh and Host can raise the prompt.
	enum class EKeychainAction : uint8 { None, Refresh, Host };
	/** Leaving or entering Play drops each notice that would replay an action. */
	void ClearPendingNotices();
	bool ShowKeychainHeadsUpIfNeeded(EKeychainAction Action);
	TSharedRef<SWidget> BuildKeychainHeadsUp();
	FReply ContinueKeychainHeadsUp();
	FReply CancelKeychainHeadsUp();
	bool ShowLocalNetworkHeadsUpIfNeeded(ELocalNetworkAction Action);
	TSharedRef<SWidget> BuildLocalNetworkHeadsUp();
	FReply ContinueLocalNetworkHeadsUp();
	FReply CancelLocalNetworkHeadsUp();
	/** Evaluate bundled notes only on a clear main-menu entry. */
	void ShowUpdateFailedIfNeeded();
	TSharedRef<SWidget> BuildUpdateFailedNotice();
	FReply ContinueUpdateFailed();
	void ShowReleaseNotesIfNeeded();
	void HandleReleaseNotesCompleted(const FWoTReleaseNotesResult& Result);
	bool IsAnotherNoticeVisible() const;
	TSharedRef<SWidget> BuildReleaseNotesNotice();
	FReply ContinueReleaseNotes();
	/** A centered notice face with an optional secondary button and scroll body. */
	TSharedRef<SWidget> BuildNoticePanel(TSharedPtr<SWidget>& OutWidget,
		TFunction<bool()> IsVisible, const TCHAR* Heading, const TCHAR* Body,
		const TCHAR* PrimaryLabel, FOnClicked OnPrimary,
		const TCHAR* SecondaryLabel, FOnClicked OnSecondary,
		bool bScrollableBody = false,
		TSharedPtr<SWidget>* OutPrimaryWidget = nullptr);
	FReply ResetOnlineIdentity();
	void HandleIdentityResetComplete(bool bSucceeded);
	/** Why an internet Host/Find failed, or empty for the generic line. */
	FString InternetFailureText() const;
	FString SessionPingText(const FWoTSessionListEntry& Entry) const;
	FString SessionServerText(const FWoTSessionListEntry& Entry) const;
	FString SessionVersionMismatchNotice(
		const FWoTSessionListEntry& Entry) const;
	int32 CompareSessionReleaseSequence(
		const FWoTSessionListEntry& Entry) const;

	FText SelectedMapText() const;
	static FText HostMapPickerText(const FWoTFrontEndMapEntry* Entry);
	FText StatusText() const;
	FText BrowserStatusText() const;
	FText HostPanelTitle() const;
	ECheckBoxState LANState() const;
	FMargin MainMenuEntryPadding(int32 EntryIndex) const;
	bool IsPresentationReady() const;
	const FWoTRequiredMenuFonts& MenuFonts() const;
	FSlateFontInfo PlayBodyFont() const;
	FSlateFontInfo LargeBodyFont() const;
	FSlateFontInfo PlayHeadingFont() const;
	FSlateFontInfo PlayBrowserFont() const;
	FText RetailText(const TCHAR* Key) const;
	FText RetailText(FName Key) const;
	FReply HandleBackInput();
	/** B/View on the screen itself (spec r3 section 1a). Overlays and
	 * notices register their own entries above it. */
	bool HandleControllerBack();
	void RegisterBackStackEntries();
	/** A Play overlay is up: the panels and the footer behind it are disabled. */
	bool IsPlayOverlayVisible() const;
	bool IsPasswordPromptVisible() const;
	/** The top visible Play overlay's first control, or null. */
	TSharedPtr<SWidget> PlayOverlayFocusTarget() const;
	/** Moves focus into an overlay that just appeared, and back out of one
	 * that just closed (Controller mode), since no event fires for either. */
	void SyncPlayOverlayFocus();
	EActiveTimerReturnType RestoreFocusAfterBack(
		double CurrentTime, float DeltaTime);

	void HandleHostComplete(bool bSucceeded);
	void HandleSearchComplete(bool bSucceeded,
		const TArray<FWoTSessionListEntry>& Results);
	void HandleJoinComplete(bool bSucceeded);
	void HandleConnectionFailure(const FString& Reason);

	TWeakObjectPtr<AWoTFrontEndPlayerController> PlayerController;
	TWeakObjectPtr<UWoTGameInstance> GameInstance;
	TWeakObjectPtr<UWoTSessionSubsystem> SessionSubsystem;
	FDelegateHandle HostCompleteHandle;
	FDelegateHandle SearchCompleteHandle;
	FDelegateHandle JoinCompleteHandle;
	FDelegateHandle ConnectionFailureHandle;
	FDelegateHandle IdentityResetHandle;
	FDelegateHandle ReleaseNotesCompletedHandle;
	bool bShowOnlineDisclosure = false;
	bool bShowLocalNetworkNotice = false;
	bool bShowReleaseNotesNotice = false;
	bool bShowUpdateFailedNotice = false;
	ELocalNetworkAction PendingLocalNetworkAction = ELocalNetworkAction::None;
	EKeychainAction PendingKeychainAction = EKeychainAction::None;
	// Set by Continue so the action it re-runs is not gated a second time.
	bool bKeychainHeadsUpAccepted = false;
	// Set while the net-test harness drives the browser itself.
	bool bAutoFetchSuppressed = false;
	// A search started before the player left Play is still running; when it
	// lands, its results are dropped and the current network is searched.
	bool bRestartSearchWhenStaleLands = false;
#if WITH_AUTOMATION_TESTS
	TOptional<EWoTKeychainAccess> KeychainAccessForTesting;
	int32 KeychainReplayCountForTesting = 0;
#endif
	// Unguarded development runs have no preference owner: the heads-up is
	// remembered for this process only.
	bool bLocalNetworkHeadsUpSeenThisRun = false;
	FString DismissedReleaseNotesKeyThisRun;
	uint64 DismissedFailedReleaseSequenceThisRun = 0;
	FString ActiveReleaseNotesKey;
	FString ActiveReleaseNotesText;
	uint64 ActiveFailedReleaseSequence = 0;
	uint64 ActiveRestoredReleaseSequence = 0;
	FString ActiveUpdateFailedText;
	/** The running release's human version ("" when unversioned) and its menu label. */
	FString RunningHumanVersion;
	FString VersionLabel;

	FWoTFrontEndViewModel ViewModel;
	TSharedPtr<SBox> ScreenHost;
	TSharedPtr<SWoTSettingsScreen> ActiveSettingsScreen;
	TSharedPtr<SWidget> PlayerConfigButton;
	TSharedPtr<STextBlock> InternetStatusTextWidget;
	TSharedPtr<STextBlock> ResetIdentityTextWidget;
	TSharedPtr<SWidget> OnlineDisclosureWidget;
	TSharedPtr<SButton> OnlineDisclosurePrimaryWidget;
	TSharedPtr<SWidget> LocalNetworkNoticeWidget;
	TSharedPtr<SWidget> LocalNetworkNoticePrimaryWidget;
	TSharedPtr<SWidget> LocalNetworkHeadsUpWidget;
	TSharedPtr<SWidget> LocalNetworkHeadsUpPrimaryWidget;
	TSharedPtr<SWidget> KeychainHeadsUpWidget;
	TSharedPtr<SWidget> KeychainHeadsUpPrimaryWidget;
	TSharedPtr<SWidget> ReleaseNotesNoticeWidget;
	TSharedPtr<SWidget> ReleaseNotesContinueWidget;
	TSharedPtr<SWidget> UpdateFailedNoticeWidget;
	TSharedPtr<SWidget> UpdateFailedContinueWidget;
	TSharedPtr<SWidget> UpdateBannerWidget;
	TSharedPtr<SWidget> VersionLabelWidget;
	TSharedPtr<SButton> UpdateBannerButton;
	TSharedPtr<SEditableTextBox> PlayerNameTextBox;
	TSharedPtr<SWidget> PasswordPromptWidget;
	TSharedPtr<SEditableTextBox> PasswordTextBox;
	TSharedPtr<SWidget> FirstFocusableWidget;
	/** Initial focus in Controller mode where spec r3 section 1a differs from
	 * today's (Pointer keeps FirstFocusableWidget). */
	TSharedPtr<SWidget> ControllerFocusWidget;
	TSharedPtr<SWidget> PlayBackButton;
	TSharedPtr<SWidget> PlayerConfigSaveButton;
	TSharedPtr<SWidget> OnlineDisclosureCancelWidget;
	TWeakPtr<SWidget> LastPlayOverlayFocus;
	TArray<TArray<TSharedPtr<SWidget>>> HostNavigationRows;
	TArray<TArray<TSharedPtr<SWidget>>> JoinNavigationRows;
	TArray<TSharedPtr<FWoTBackStackEntryHandle>> BackStackEntries;
	TSharedPtr<FWoTCursorLease> CursorLease;
	TSharedPtr<FWoTFrontEndPresentationAuthority> PresentationAuthority;
	TSharedPtr<FWoTFrontEndBackInputProcessor> BackInputProcessor;
	TUniquePtr<FWoTFrontEndMenuSelection> MainMenuSelection;
	TArray<TSharedPtr<SWoTMenuButton>> MainMenuButtons;
	TArray<FMapItem> MapItems;
	FMapItem SelectedMapItem;
	TSharedPtr<SComboBox<FMapItem>> MapComboBox;
	TSharedPtr<class SScrollBox> HostScrollBox;
	TSharedPtr<SWidget> HostMatchButton;
	TArray<FSessionItem> SessionItems;
	FSessionItem SelectedSessionItem;
	TSharedPtr<SListView<FSessionItem>> SessionListView;
	TUniquePtr<FWoTBrowserSortState> BrowserSort;
	FString DirectAddress;
	float LiveViewportWidth = 0.0f;
	float LiveViewportHeight = 0.0f;
	float LiveViewportScale = 1.0f;
	// The Play screen's check boxes, sized to LargeBodyFont at the live
	// viewport scale (SCheckBox reads its style by pointer).
	FCheckBoxStyle PlayCheckBoxStyle;
	float PlayCheckGlyphSize = 0.0f;
	// Widest host-panel label in LargeBodyFont: the shared label column.
	float HostLabelColumnWidth = 0.0f;
	float MainMenuSafeWidth = 0.0f;
	float MainMenuListWidth = 0.0f;
	float MainMenuItemHeight = 0.0f;
	float MainMenuSpacing = 0.0f;
	float MainMenuListHeight = 0.0f;
	bool bRestoreFocusOnNextTick = false;
#if WITH_AUTOMATION_TESTS
	FWoTFrontEndActionObserver ActionObserverForTesting;
	TWeakObjectPtr<UWoTPreferencesSubsystem> PreferencesForTesting;
	TWeakObjectPtr<UWoTBootstrapSubsystem> BootstrapForTesting;
	FWoTReleasePageLaunchObserver ReleasePageLaunchForTesting;
	uint64 ScreenRebuildCount = 0;
	bool bShowResetIdentityForTesting = false;
	FText InternetStatusTextOverrideForTesting;
#endif
};
