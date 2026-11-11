#include "WoTFrontEndSlate.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Types/NavigationMetaData.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "WoTFrontEndCatalog.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTBootstrapSubsystem.h"
#include "WoTFrontEndPlayerController.h"
#include "WoTGameInstance.h"
#include "WoTLocalPlayer.h"
#include "WoTMenuStyle.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTRetailText.h"
#include "UI/WoTReleaseNotes.h"
#include "Updater/WoTReleaseIdentity.h"
#include "WoTSessionSubsystem.h"
#include "Online/WoTLocalNetworkAccess.h"
#include "Settings/WoTSettingsSlate.h"
#include "UI/SWoTFocusOutline.h"
#include "UI/SWoTListCombo.h"
#include "UI/SWoTOnScreenKeyboard.h"
#include "UI/SWoTTextFieldActivator.h"
#include "UI/WoTBackStack.h"
#include "UI/WoTControllerUIInput.h"
#include "UI/WoTMenuNavigation.h"
#include "UI/WoTTextEntry.h"

// Escape's per-window back path. B and View go through the back stack
// (FWoTBackStack) since controller sub-project 3; Escape keeps this path,
// except that with the keyboard panel open it cancels the panel first, and
// with a dropdown open it closes only the dropdown (owner ruling 2026-11-08,
// spec r3 [M3] [N1]).
class FWoTFrontEndBackInputProcessor final : public IInputProcessor
{
public:
	explicit FWoTFrontEndBackInputProcessor(
		const TSharedRef<SWoTFrontEndRoot>& InRoot)
		: Root(InRoot)
	{
	}

	virtual void Tick(float, FSlateApplication&,
		TSharedRef<ICursor>) override
	{
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp,
		const FKeyEvent& InKeyEvent) override
	{
		if (InKeyEvent.GetKey() != EKeys::Escape)
		{
			return false;
		}
		const TSharedPtr<SWoTFrontEndRoot> PinnedRoot = Root.Pin();
		if (!PinnedRoot.IsValid() || !PinnedRoot->IsPresentationReady())
		{
			return false;
		}
		const TSharedPtr<SWidget> Focused = SlateApp.GetKeyboardFocusedWidget();
		const TSharedPtr<SWindow> RootWindow =
			SlateApp.FindWidgetWindow(PinnedRoot.ToSharedRef());
		TSharedPtr<SWindow> FocusWindow = Focused.IsValid()
			? SlateApp.FindWidgetWindow(Focused.ToSharedRef()) : nullptr;
		if (!RootWindow.IsValid() || !FocusWindow.IsValid()) return false;
		bool bInsideRootWindowFamily = false;
		for (TSharedPtr<SWindow> Candidate = FocusWindow; Candidate.IsValid();
			Candidate = Candidate->GetParentWindow())
		{
			if (Candidate == RootWindow)
			{
				bInsideRootWindowFamily = true;
				break;
			}
		}
		if (!bInsideRootWindowFamily) return false;

		// An open keyboard panel cancels first, and no screen back happens.
		if (FWoTTextEntry::CancelWithin(PinnedRoot.ToSharedRef()))
		{
			return true;
		}
		if (PinnedRoot->ViewModel.GetScreen() == EWoTFrontEndScreen::Main)
		{
			return false;
		}
		// An open dropdown closes first; a second Escape backs the screen.
		if (SWoTListCombo::CloseOpenWithin(PinnedRoot.ToSharedRef()))
		{
			return true;
		}
		SlateApp.DismissAllMenus();
		return PinnedRoot->HandleBackInput().IsEventHandled();
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("WoTFrontEndBackInput");
	}

private:
	TWeakPtr<SWoTFrontEndRoot> Root;
};

namespace
{
	FText WoTFrontEndSlate_Text(const TCHAR* Text)
	{
		return FText::FromString(Text);
	}

	FText WoTFrontEndSlate_Text(const FString& Text)
	{
		return FText::FromString(Text);
	}

	// The host panel outgrows the screen once every retail row is shown, so
	// its bar is always drawn and its thumb wears the menu text colour: the
	// shared dark-grey menu thumb went unnoticed on the dark frame.
	const FScrollBarStyle* WoTFrontEndSlate_HostScrollBarStyle()
	{
		static const FScrollBarStyle Style = []()
		{
			FScrollBarStyle Result = *FWoTMenuStyle::Get().ScrollBarStyle();
			Result
				.SetNormalThumbImage(FSlateColorBrush(FWoTMenuStyle::UnselectedColor()))
				.SetHoveredThumbImage(FSlateColorBrush(FWoTMenuStyle::SelectedColor()))
				.SetDraggedThumbImage(FSlateColorBrush(FWoTMenuStyle::SelectedColor()))
				.SetThickness(12.0f);
			return Result;
		}();
		return &Style;
	}

	// Narrowest host control, in layout units, before it wraps under its label.
	constexpr float WoTFrontEndSlate_HostMinControlWidth = 240.0f;

	// Every label the host panel shows in either mode; the widest sets the
	// shared label column so the controls beside them line up.
	const TCHAR* const WoTFrontEndSlate_HostLabelIds[] = {
		TEXT("menuStartArenaServer.MenuList[7]"),
		TEXT("menuStartArenaServer.MenuList[1]"),
		TEXT("menuStartArenaServer.MenuList[2]"),
		TEXT("menuStartArenaServer.MenuList[3]"),
		TEXT("menuStartBattleServer.MenuList[6]"),
		TEXT("menuStartBattleServer.MenuList[7]"),
		TEXT("menuStartArenaServer.MenuList[5]"),
		TEXT("menuStartBattleServer.MenuList[10]"),
		TEXT("menuStartArenaServer.MenuList[6]"),
		TEXT("menuStartBattleServer.MenuList[11]"),
		TEXT("menuStartArenaServer.MenuList[8]"),
		TEXT("menuStartBattleServer.MenuList[13]"),
		TEXT("menuStartArenaServer.MenuList[9]"),
		TEXT("menuStartBattleServer.MenuList[14]")};

	FText WoTFrontEndSlate_SkinLabel(EWoTPlayerSkinColor Current, int32 Offset)
	{
		const TArray<FWoTSkinChoice>& Choices = FWoTFrontEndModel::SkinChoices();
		int32 Index = Choices.IndexOfByPredicate(
			[Current](const FWoTSkinChoice& Choice)
			{
				return Choice.Color == Current;
			});
		if (Choices.IsEmpty())
		{
			return FText::GetEmpty();
		}
		if (Index == INDEX_NONE)
		{
			Index = 0;
		}
		Index = (Index + Offset % Choices.Num() + Choices.Num()) % Choices.Num();
		return WoTFrontEndSlate_Text(Choices[Index].DisplayName);
	}

}

TConstArrayView<FWoTFrontEndMainMenuEntry>
SWoTFrontEndRoot::RetailMainEntries()
{
	static const FWoTFrontEndMainMenuEntry Entries[] = {
		{FName(TEXT("menuMain.MenuList[2]")),
			EWoTFrontEndMainMenuAction::Multiplayer},
		{FName(TEXT("menuMain.MenuList[3]")),
			EWoTFrontEndMainMenuAction::Controls},
		{FName(TEXT("menuMain.MenuList[4]")),
			EWoTFrontEndMainMenuAction::Hardware},
		{FName(TEXT("menuMain.MenuList[7]")),
			EWoTFrontEndMainMenuAction::Quit}
	};
	return MakeArrayView(Entries);
}

void SWoTFrontEndRoot::Construct(const FArguments& InArgs)
{
	BrowserSort = MakeUnique<FWoTBrowserSortState>();
	PlayCheckBoxStyle = *FWoTMenuStyle::Get().CheckBoxStyle();
	// Resized to the label font once the presentation measures the viewport.
	PlayCheckGlyphSize = 12.0f;
	FWoTMenuStyle::SizeCheckBoxImages(PlayCheckBoxStyle, PlayCheckGlyphSize);
	PlayerController = InArgs._PlayerController;
	if (AWoTFrontEndPlayerController* Controller = PlayerController.Get())
	{
		GameInstance = Cast<UWoTGameInstance>(Controller->GetGameInstance());
		if (UWoTGameInstance* Instance = GameInstance.Get())
		{
			SessionSubsystem = Instance->GetSubsystem<UWoTSessionSubsystem>();
		}
	}
#if WITH_AUTOMATION_TESTS
	PresentationAuthority = InArgs._PresentationAuthorityForTesting;
	ActionObserverForTesting = InArgs._ActionObserverForTesting;
	PreferencesForTesting = InArgs._PreferencesForTesting;
	BootstrapForTesting = InArgs._BootstrapForTesting;
	ReleasePageLaunchForTesting = InArgs._ReleasePageLaunchForTesting;
	bShowResetIdentityForTesting = InArgs._ShowResetIdentityForTesting;
	bShowOnlineDisclosure = InArgs._ShowOnlineDisclosureForTesting;
	InternetStatusTextOverrideForTesting = InArgs._InternetStatusTextForTesting;
#endif
	if (!PresentationAuthority.IsValid())
	{
		PresentationAuthority = FWoTFrontEndPresentationAuthority::Create(
			PlayerController.Get());
	}
	RunningHumanVersion = WoTReleaseIdentity::LoadRunningHumanVersion();
	VersionLabel = WoTReleaseIdentity::DisplayLabel(
		WoTReleaseIdentity::LoadRunning(), RunningHumanVersion);
	if (PresentationAuthority.IsValid())
	{
		PresentationAuthority->BindWidget(AsShared());
		MainMenuSelection = MakeUnique<FWoTFrontEndMenuSelection>(
			StaticCastSharedRef<IWoTFrontEndFeedbackSink>(
				PresentationAuthority.ToSharedRef()));
	}
	SetVisibility(TAttribute<EVisibility>::CreateLambda(
		[CapturedAuthority = PresentationAuthority]()
		{
			return CapturedAuthority.IsValid() && CapturedAuthority->IsReady()
				? EVisibility::Visible : EVisibility::Collapsed;
		}));
	BindSessionEvents();
	ViewModel.ShowMain();
	if (UWoTBootstrapSubsystem* Bootstrap = ResolveBootstrap())
	{
		ReleaseNotesCompletedHandle = Bootstrap->OnReleaseNotesCompleted().AddSP(
			this, &SWoTFrontEndRoot::HandleReleaseNotesCompleted);
	}
	if (UWoTSessionSubsystem* Sessions = SessionSubsystem.Get())
	{
		const FString Failure = Sessions->ConsumeConnectionFailure();
		if (Sessions->GetPasswordChallenge() != EWoTPasswordChallenge::None)
		{
			ViewModel.ShowPlay(FWoTFrontEndMapCatalog::Enumerate());
		}
		else if (!Failure.IsEmpty())
		{
			// Engine failure handling may have browsed back to the default menu
			// world. Reopen Play so the retained reason is visible and usable.
			ViewModel.ShowPlay(FWoTFrontEndMapCatalog::Enumerate());
			ViewModel.ReportConnectionFailure(Failure);
		}
	}

	// The focus outline layer draws the controller focus outline on every
	// screen. The prompt strip overlays the screens' bottom-right corner and
	// shows only in Controller mode; as an overlay it never resizes the screen
	// host, so the menus don't move when the input mode flips.
	ChildSlot
	[
		SAssignNew(FocusOutlineLayer, SWoTFocusOutlineLayer)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				// Hosts the keyboard panel over every screen (spec r3 section
				// 3). Outside the screen host, so a rebuild keeps the panel and
				// its draft; the prompt strip stays drawn above it.
				SAssignNew(TextEntryLayer, SWoTTextEntryLayer)
				.Font(this, &SWoTFrontEndRoot::PromptFont)
				// The dark strip behind the prompt strip below (10 under it).
				.PromptBarMargin(FMargin(0.0f, 10.0f, 0.0f, 10.0f))
				[
					SAssignNew(ScreenHost, SBox)
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
				]
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Bottom)
			.Padding(FMargin(24.0f, 0.0f, 28.0f, 10.0f))
			[
				SAssignNew(ButtonPrompts, SWoTButtonPrompts)
				.Prompts(this, &SWoTFrontEndRoot::ControllerPrompts)
				.Font(this, &SWoTFrontEndRoot::PromptFont)
			]
		]
	];
	// The front end is a modal surface: a controller hides the pointer.
	CursorLease = FWoTControllerUIInput::AcquireCursorLease();
	RegisterBackStackEntries();
	RebuildScreen();
	if (FSlateApplication::IsInitialized())
	{
		BackInputProcessor = MakeShared<FWoTFrontEndBackInputProcessor>(
			StaticCastSharedRef<SWoTFrontEndRoot>(AsShared()));
		FSlateApplication::Get().RegisterInputPreProcessor(BackInputProcessor,
			EInputPreProcessorType::Game);
	}
}

SWoTFrontEndRoot::~SWoTFrontEndRoot()
{
	if (UWoTBootstrapSubsystem* Bootstrap = ResolveBootstrap())
	{
		Bootstrap->OnReleaseNotesCompleted().Remove(
			ReleaseNotesCompletedHandle);
	}
	if (BackInputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(BackInputProcessor);
		BackInputProcessor.Reset();
	}
	if (UWoTSessionSubsystem* Sessions = SessionSubsystem.Get())
	{
		Sessions->OnHostComplete().Remove(HostCompleteHandle);
		Sessions->OnSearchComplete().Remove(SearchCompleteHandle);
		Sessions->OnJoinCompleteForUI().Remove(JoinCompleteHandle);
		Sessions->OnConnectionFailure().Remove(ConnectionFailureHandle);
		Sessions->OnOnlineIdentityResetComplete().Remove(IdentityResetHandle);
	}
}

void SWoTFrontEndRoot::BindSessionEvents()
{
	if (UWoTSessionSubsystem* Sessions = SessionSubsystem.Get())
	{
		HostCompleteHandle = Sessions->OnHostComplete().AddSP(
			this, &SWoTFrontEndRoot::HandleHostComplete);
		SearchCompleteHandle = Sessions->OnSearchComplete().AddSP(
			this, &SWoTFrontEndRoot::HandleSearchComplete);
		JoinCompleteHandle = Sessions->OnJoinCompleteForUI().AddSP(
			this, &SWoTFrontEndRoot::HandleJoinComplete);
		ConnectionFailureHandle = Sessions->OnConnectionFailure().AddSP(
			this, &SWoTFrontEndRoot::HandleConnectionFailure);
		IdentityResetHandle = Sessions->OnOnlineIdentityResetComplete().AddSP(
			this, &SWoTFrontEndRoot::HandleIdentityResetComplete);
	}
}

TSharedPtr<SWidget> SWoTFrontEndRoot::InitialFocusWidget() const
{
	if (!IsPresentationReady())
	{
		return nullptr;
	}
	// Initial focus follows the last input (owner ruling 2026-11-08):
	// Controller mode uses spec r3 section 1a, Pointer keeps today's.
	return FWoTControllerUIInput::IsControllerMode() && ControllerFocusWidget.IsValid()
		? ControllerFocusWidget : FirstFocusableWidget;
}

void SWoTFrontEndRoot::RegisterBackStackEntries()
{
	FWoTBackStack& Stack = FWoTBackStack::Get();
	const TSharedRef<SWidget> Self = AsShared();
	auto OnScreen = [this](EWoTFrontEndScreen Screen)
	{
		return IsPresentationReady() && ViewModel.GetScreen() == Screen;
	};
	// Bottom of the stack: the screen itself.
	BackStackEntries.Add(Stack.Register(Self,
		[this]() { return IsPresentationReady(); },
		[this]() { return HandleControllerBack(); }));
	// Main's notices: B dismisses the one showing.
	BackStackEntries.Add(Stack.Register(Self,
		[this, OnScreen]()
		{
			return OnScreen(EWoTFrontEndScreen::Main) && bShowReleaseNotesNotice;
		},
		[this]() { ContinueReleaseNotes(); return true; }));
	BackStackEntries.Add(Stack.Register(Self,
		[this, OnScreen]()
		{
			return OnScreen(EWoTFrontEndScreen::Main) && bShowUpdateFailedNotice;
		},
		[this]() { ContinueUpdateFailed(); return true; }));
	// Play's overlays, bottom to top as BuildPlayScreen stacks them: B takes
	// the top visible overlay's cancel and never also backs the screen.
	BackStackEntries.Add(Stack.Register(Self,
		[this, OnScreen]()
		{
			return OnScreen(EWoTFrontEndScreen::Play) && bShowOnlineDisclosure;
		},
		[this]() { CancelOnlineDisclosure(); return true; }));
	BackStackEntries.Add(Stack.Register(Self,
		[this, OnScreen]()
		{
			return OnScreen(EWoTFrontEndScreen::Play) && bShowLocalNetworkNotice;
		},
		[this]() { DismissLocalNetworkNotice(); return true; }));
	BackStackEntries.Add(Stack.Register(Self,
		[this, OnScreen]()
		{
			return OnScreen(EWoTFrontEndScreen::Play)
				&& PendingLocalNetworkAction != ELocalNetworkAction::None;
		},
		[this]() { CancelLocalNetworkHeadsUp(); return true; }));
	BackStackEntries.Add(Stack.Register(Self,
		[this, OnScreen]()
		{
			return OnScreen(EWoTFrontEndScreen::Play)
				&& PendingKeychainAction != EKeychainAction::None;
		},
		[this]() { CancelKeychainHeadsUp(); return true; }));
	BackStackEntries.Add(Stack.Register(Self,
		[this, OnScreen]()
		{
			return OnScreen(EWoTFrontEndScreen::Play) && IsPasswordPromptVisible();
		},
		[this]() { CancelServerPassword(); return true; }));
}

bool SWoTFrontEndRoot::HandleControllerBack()
{
	if (!IsPresentationReady())
	{
		return false;
	}
	if (ViewModel.GetScreen() == EWoTFrontEndScreen::Main)
	{
		// Nothing to back out of; the press is consumed and does nothing.
		return true;
	}
	if (FSlateApplication::IsInitialized())
	{
		// An open map list closes with the screen, as Escape does.
		FSlateApplication::Get().DismissAllMenus();
	}
	return HandleBackInput().IsEventHandled();
}

TArray<FWoTButtonPrompt> SWoTFrontEndRoot::ControllerPrompts() const
{
	if (!IsPresentationReady())
	{
		return {};
	}
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		// The keyboard panel's set, or Steam's keyboard's.
		return FWoTTextEntry::ControllerPrompts();
	}
	if (SWoTListCombo::IsAnyOpenWithin(AsShared()))
	{
		return FWoTButtonPromptSets::Dropdown();
	}
	switch (ViewModel.GetScreen())
	{
	case EWoTFrontEndScreen::Play:
		if (IsPasswordPromptVisible())
		{
			return FWoTButtonPromptSets::Password();
		}
		if (PendingKeychainAction != EKeychainAction::None
			|| PendingLocalNetworkAction != ELocalNetworkAction::None)
		{
			return FWoTButtonPromptSets::Overlay(
				WoTFrontEndSlate_Text(TEXT("Not Now")));
		}
		if (bShowLocalNetworkNotice)
		{
			return FWoTButtonPromptSets::Overlay(WoTFrontEndSlate_Text(TEXT("OK")));
		}
		if (bShowOnlineDisclosure)
		{
			return FWoTButtonPromptSets::Overlay(WoTFrontEndSlate_Text(TEXT("Cancel")));
		}
		return FWoTButtonPromptSets::SelectBack();
	case EWoTFrontEndScreen::PlayerConfig:
		return FWoTButtonPromptSets::SelectBack();
	case EWoTFrontEndScreen::Controls:
	case EWoTFrontEndScreen::Hardware:
		return ActiveSettingsScreen.IsValid()
			? ActiveSettingsScreen->ControllerPrompts() : FWoTButtonPromptSets::Settings();
	case EWoTFrontEndScreen::Main:
	default:
		return bShowReleaseNotesNotice || bShowUpdateFailedNotice
			? FWoTButtonPromptSets::Notice() : FWoTButtonPromptSets::Select();
	}
}

FSlateFontInfo SWoTFrontEndRoot::PromptFont() const
{
	// Never an empty face: that would draw Slate's last-resort font.
	return IsPresentationReady() ? LargeBodyFont()
		: FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
}

#if WITH_AUTOMATION_TESTS
TSharedPtr<SWidget> SWoTFrontEndRoot::MapComboBoxForTesting() const
{
	return MapComboBox;
}
#endif

bool SWoTFrontEndRoot::IsPasswordPromptVisible() const
{
	const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	return PasswordPromptWidget.IsValid() && Sessions != nullptr
		&& Sessions->GetPasswordChallenge() != EWoTPasswordChallenge::None;
}

bool SWoTFrontEndRoot::IsPlayOverlayVisible() const
{
	return bShowOnlineDisclosure || bShowLocalNetworkNotice
		|| PendingLocalNetworkAction != ELocalNetworkAction::None
		|| PendingKeychainAction != EKeychainAction::None
		|| IsPasswordPromptVisible();
}

TSharedPtr<SWidget> SWoTFrontEndRoot::PlayOverlayFocusTarget() const
{
	if (IsPasswordPromptVisible() && PasswordTextBox.IsValid())
	{
		return PasswordTextBox;
	}
	if (PendingKeychainAction != EKeychainAction::None)
	{
		return KeychainHeadsUpPrimaryWidget;
	}
	if (PendingLocalNetworkAction != ELocalNetworkAction::None)
	{
		return LocalNetworkHeadsUpPrimaryWidget;
	}
	if (bShowLocalNetworkNotice)
	{
		return LocalNetworkNoticePrimaryWidget;
	}
	if (bShowOnlineDisclosure)
	{
		return OnlineDisclosurePrimaryWidget;
	}
	return nullptr;
}

void SWoTFrontEndRoot::SyncPlayOverlayFocus()
{
	if (ViewModel.GetScreen() != EWoTFrontEndScreen::Play
		|| !FSlateApplication::IsInitialized())
	{
		LastPlayOverlayFocus.Reset();
		PrePlayOverlayFocus.Reset();
		return;
	}
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		// The keyboard panel keeps focus; overlays sync once the entry ends.
		return;
	}
	const TSharedPtr<SWidget> OverlayFocus = PlayOverlayFocusTarget();
	const TSharedPtr<SWidget> Previous = LastPlayOverlayFocus.Pin();
	if (OverlayFocus == Previous)
	{
		return;
	}
	LastPlayOverlayFocus = OverlayFocus;
	FSlateApplication& SlateApp = FSlateApplication::Get();
	if (OverlayFocus.IsValid())
	{
		// The panels behind it are disabled, so focus moves into the overlay.
		const TSharedPtr<SWidget> Focused = SlateApp.GetKeyboardFocusedWidget();
		if (!Previous.IsValid())
		{
			PrePlayOverlayFocus = Focused;
		}
		if (Focused != OverlayFocus)
		{
			SlateApp.SetKeyboardFocus(OverlayFocus, EFocusCause::SetDirectly);
		}
	}
	else if (Previous.IsValid())
	{
		const TSharedPtr<SWidget> Restore = PrePlayOverlayFocus.Pin();
		PrePlayOverlayFocus.Reset();
		if (FWoTControllerUIInput::IsControllerMode())
		{
			// The overlay closed: a controller needs focus back on the panels,
			// where Controller mode starts (spec r3 section 1a, Play).
			const TSharedPtr<SWidget> PanelFocus = MapComboBox.IsValid()
				? StaticCastSharedPtr<SWidget>(MapComboBox) : PlayBackButton;
			if (PanelFocus.IsValid())
			{
				SlateApp.SetKeyboardFocus(PanelFocus, EFocusCause::SetDirectly);
			}
		}
		else if (Restore.IsValid() && Restore->GetVisibility().IsVisible()
			&& Restore->IsEnabled())
		{
			// Keyboard and mouse get focus back where it was before the
			// overlay took it, never left on the collapsed overlay.
			SlateApp.SetKeyboardFocus(Restore, EFocusCause::SetDirectly);
		}
	}
}

void SWoTFrontEndRoot::FocusInitialWidget()
{
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		// A rebuild during entry keeps the entry and its draft [M6]: focus
		// stays on the panel, or returns to the rebuilt field for Steam's keys.
		FWoTTextEntry::RestoreFocus();
		return;
	}
	const TSharedPtr<SWidget> InitialFocus = InitialFocusWidget();
	if (FSlateApplication::IsInitialized() && InitialFocus.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			InitialFocus, EFocusCause::SetDirectly);
	}
}

void SWoTFrontEndRoot::Tick(
	const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	if (!IsPresentationReady())
	{
		FirstFocusableWidget.Reset();
		if (ScreenHost.IsValid())
		{
			ScreenHost->SetContent(SNullWidget::NullWidget);
		}
		MainMenuItemHeight = 0.0f;
		MainMenuSpacing = 0.0f;
		MainMenuListHeight = 0.0f;
		MainMenuListWidth = 0.0f;
		MainMenuSafeWidth = 0.0f;
		return;
	}
	if (bRestoreFocusOnNextTick)
	{
		bRestoreFocusOnNextTick = false;
		FocusInitialWidget();
	}
	RepairUpdateBannerFocus();
	SyncPlayOverlayFocus();
	FVector2D LiveSize = AllottedGeometry.GetLocalSize();
	if (ScreenHost.IsValid())
	{
		const FVector2D HostSize = ScreenHost->GetCachedGeometry().GetLocalSize();
		if (HostSize.X > 0.0f && HostSize.Y > 0.0f)
		{
			LiveSize = HostSize;
		}
	}

	const float PreviousWidth = LiveViewportWidth;
	const float PreviousHeight = LiveViewportHeight;
	const float PreviousScale = LiveViewportScale;
	const float PreviousItemHeight = MainMenuItemHeight;
	const float PreviousSpacing = MainMenuSpacing;
	const float PreviousListHeight = MainMenuListHeight;
	const float PreviousListWidth = MainMenuListWidth;
	const float PreviousSafeWidth = MainMenuSafeWidth;
	LiveViewportWidth = LiveSize.X;
	LiveViewportHeight = LiveSize.Y;
	LiveViewportScale = FMath::Max(
		AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), 0.01f);
	const float PreviousCheckGlyph = PlayCheckGlyphSize;
	PlayCheckGlyphSize = FWoTMenuStyle::CheckGlyphSizeFor(LargeBodyFont());
	const float PreviousLabelColumn = HostLabelColumnWidth;
	HostLabelColumnWidth = 0.0f;
	for (const TCHAR* LabelId : WoTFrontEndSlate_HostLabelIds)
	{
		HostLabelColumnWidth = FMath::Max(HostLabelColumnWidth,
			FWoTMenuLayout::MeasureText(RetailText(LabelId), LargeBodyFont()).X);
	}
	if (!FMath::IsNearlyEqual(PreviousCheckGlyph, PlayCheckGlyphSize))
	{
		FWoTMenuStyle::SizeCheckBoxImages(PlayCheckBoxStyle, PlayCheckGlyphSize);
	}
	TArray<FVector2D, TInlineAllocator<4>> Measurements;
	bool bHasMeasuredFace = true;
	for (const FWoTFrontEndMainMenuEntry& Entry : RetailMainEntries())
	{
		const FVector2D Measurement = FWoTMenuLayout::MeasureText(
			RetailText(Entry.LocalizationId), MenuFonts().Title);
		bHasMeasuredFace = bHasMeasuredFace
			&& Measurement.X > 0.0f && Measurement.Y > 0.0f;
		Measurements.Add(Measurement);
	}
	if (!bHasMeasuredFace)
	{
		FirstFocusableWidget.Reset();
		ScreenHost->SetContent(SNullWidget::NullWidget);
		MainMenuItemHeight = 0.0f;
		MainMenuSpacing = 0.0f;
		MainMenuListHeight = 0.0f;
		MainMenuListWidth = 0.0f;
		MainMenuSafeWidth = 0.0f;
		return;
	}
	const FWoTRetailMenuLayout Layout =
		FWoTMenuLayout::CalculateRetailMainLayout(LiveSize, Measurements);
	MainMenuItemHeight = Layout.ItemHeight;
	MainMenuSpacing = Layout.Spacing;
	MainMenuListHeight = Layout.ListSize.Y;
	MainMenuListWidth = Layout.ListSize.X;
	MainMenuSafeWidth = Layout.SafeRegion.Right - Layout.SafeRegion.Left;

	if (!FMath::IsNearlyEqual(PreviousWidth, LiveViewportWidth)
		|| !FMath::IsNearlyEqual(PreviousHeight, LiveViewportHeight)
		|| !FMath::IsNearlyEqual(PreviousScale, LiveViewportScale)
		|| !FMath::IsNearlyEqual(PreviousCheckGlyph, PlayCheckGlyphSize)
		|| !FMath::IsNearlyEqual(PreviousLabelColumn, HostLabelColumnWidth)
		|| !FMath::IsNearlyEqual(PreviousItemHeight, MainMenuItemHeight)
		|| !FMath::IsNearlyEqual(PreviousSpacing, MainMenuSpacing)
		|| !FMath::IsNearlyEqual(PreviousListHeight, MainMenuListHeight)
		|| !FMath::IsNearlyEqual(PreviousListWidth, MainMenuListWidth)
		|| !FMath::IsNearlyEqual(PreviousSafeWidth, MainMenuSafeWidth))
	{
		Invalidate(EInvalidateWidgetReason::Layout);
	}
}

bool SWoTFrontEndRoot::IsPresentationReady() const
{
	return PresentationAuthority.IsValid()
		&& PresentationAuthority->IsReady();
}

const FWoTRequiredMenuFonts& SWoTFrontEndRoot::MenuFonts() const
{
	check(IsPresentationReady());
	const FWoTRequiredMenuFonts* Fonts =
		PresentationAuthority->RequiredFonts();
	check(Fonts != nullptr);
	return *Fonts;
}

FSlateFontInfo SWoTFrontEndRoot::PlayBodyFont() const
{
	if (!IsPresentationReady()) return FSlateFontInfo();
	return PresentationAuthority->MultiplayerFont(FName(TEXT("F_WOTReg14")), LiveViewportScale);
}

FSlateFontInfo SWoTFrontEndRoot::PlayHeadingFont() const
{
	if (!IsPresentationReady()) return FSlateFontInfo();
	return PresentationAuthority->MultiplayerFont(FName(TEXT("F_WOTReg30")), LiveViewportScale);
}

FSlateFontInfo SWoTFrontEndRoot::LargeBodyFont() const
{
	if (!IsPresentationReady()) return FSlateFontInfo();
	return PresentationAuthority->MultiplayerFont(FName(TEXT("F_WOTReg14")), LiveViewportScale, 2);
}

FSlateFontInfo SWoTFrontEndRoot::PlayBrowserFont() const
{
	if (!IsPresentationReady() || !PresentationAuthority->BrowserStyle()) return FSlateFontInfo();
	return PresentationAuthority->MultiplayerFont(FName(TEXT("Tahoma20")), LiveViewportScale);
}

FText SWoTFrontEndRoot::RetailText(const TCHAR* Key) const
{
	return RetailText(FName(Key));
}

FText SWoTFrontEndRoot::RetailText(FName Key) const
{
	if (!IsPresentationReady()) return FText::GetEmpty();
	const FText Authenticated = PresentationAuthority->Text(Key);
	return !Authenticated.IsEmpty()
		? Authenticated : FWoTMenuStyle::Get().RetailText(Key);
}

FReply SWoTFrontEndRoot::HandleBackInput()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	switch (ViewModel.GetScreen())
	{
	case EWoTFrontEndScreen::PlayerConfig:
		return CancelPlayerConfig();
	case EWoTFrontEndScreen::Play:
		return BackFromPlay();
	case EWoTFrontEndScreen::Controls:
	case EWoTFrontEndScreen::Hardware:
		// The screen decides: a live key capture consumes Escape first.
		return ActiveSettingsScreen.IsValid() && ActiveSettingsScreen->HandleBack()
			? FReply::Handled() : FReply::Unhandled();
	case EWoTFrontEndScreen::Main:
	default:
		return FReply::Unhandled();
	}
}

EActiveTimerReturnType SWoTFrontEndRoot::RestoreFocusAfterBack(
	double /*CurrentTime*/, float /*DeltaTime*/)
{
	bRestoreFocusOnNextTick = false;
	FocusInitialWidget();
	return EActiveTimerReturnType::Stop;
}

FReply SWoTFrontEndRoot::OnPreviewKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	// Escape only: B and View reach the screen through the back stack.
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		// An open keyboard panel cancels first (spec r3 [M3]).
		if (FWoTTextEntry::CancelWithin(AsShared()))
		{
			return FReply::Handled();
		}
		// An open dropdown closes first (spec r3 [N1]).
		if (SWoTListCombo::CloseOpenWithin(AsShared()))
		{
			return FReply::Handled();
		}
		return HandleBackInput();
	}
	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTFrontEndRoot::OnKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

bool SWoTFrontEndRoot::RefreshTextureBrushes()
{
	return FWoTMenuStyle::Get().RefreshTextureBrushes();
}

TArray<UTexture2D*> SWoTFrontEndRoot::RequiredTexturePayloads() const
{
	return FWoTMenuStyle::Get().RequiredTexturePayloads();
}

void SWoTFrontEndRoot::RebuildScreen()
{
#if WITH_AUTOMATION_TESTS
	++ScreenRebuildCount;
#endif
	FirstFocusableWidget.Reset();
	ControllerFocusWidget.Reset();
	PlayBackButton.Reset();
	PlayerConfigSaveButton.Reset();
	OnlineDisclosureCancelWidget.Reset();
	HostNavigationRows.Reset();
	JoinNavigationRows.Reset();
	PlayerNameTextBox.Reset();
	ServerNameTextBox.Reset();
	AdminPasswordTextBox.Reset();
	GamePasswordTextBox.Reset();
	DirectAddressTextBox.Reset();
	MainMenuButtons.Reset();
	MapComboBox.Reset();
	SessionListView.Reset();
	ActiveSettingsScreen.Reset();
	PlayerConfigButton.Reset();
	OnlineDisclosurePrimaryWidget.Reset();
	LocalNetworkNoticePrimaryWidget.Reset();
	LocalNetworkHeadsUpPrimaryWidget.Reset();
	KeychainHeadsUpPrimaryWidget.Reset();
	ReleaseNotesContinueWidget.Reset();
	UpdateFailedContinueWidget.Reset();
	UpdateBannerWidget.Reset();
	UpdateBannerButton.Reset();
	if (!ScreenHost.IsValid())
	{
		return;
	}
	if (!IsPresentationReady())
	{
		ScreenHost->SetContent(SNullWidget::NullWidget);
		return;
	}

	switch (ViewModel.GetScreen())
	{
	case EWoTFrontEndScreen::PlayerConfig:
		ScreenHost->SetContent(BuildPlayerConfigScreen());
		break;
	case EWoTFrontEndScreen::Controls:
	case EWoTFrontEndScreen::Hardware:
		ScreenHost->SetContent(BuildSettingsPage(ViewModel.GetScreen()));
		break;
	case EWoTFrontEndScreen::Play:
		ScreenHost->SetContent(BuildPlayScreen());
		break;
	case EWoTFrontEndScreen::Main:
	default:
		ShowUpdateFailedIfNeeded();
		if (!bShowUpdateFailedNotice) ShowReleaseNotesIfNeeded();
		ScreenHost->SetContent(BuildMainScreen());
		break;
	}
	FocusInitialWidget();
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildMainScreen()
{
	TSharedRef<SVerticalBox> Entries = SNew(SVerticalBox);
	MainMenuButtons.Reserve(RetailMainEntries().Num());
	for (int32 EntryIndex = 0;
		EntryIndex < RetailMainEntries().Num(); ++EntryIndex)
	{
		const FWoTFrontEndMainMenuEntry Entry = RetailMainEntries()[EntryIndex];
		const FText Label = RetailText(Entry.LocalizationId);
		TSharedPtr<SWoTMenuButton> Button;
		Entries->AddSlot()
		.AutoHeight()
		.Padding(TAttribute<FMargin>::CreateLambda(
			[this, EntryIndex]() { return MainMenuEntryPadding(EntryIndex); }))
		[
			SNew(SBox)
			.HeightOverride_Lambda([this]() { return MainMenuItemHeight; })
			[
				SAssignNew(Button, SWoTMenuButton)
				.AccessibleText(Label)
				.ContentPadding(FMargin(0.0f))
				.HAlign(HAlign_Center)
				.IsSelected_Lambda([this, EntryIndex]()
				{
					return MainMenuSelection.IsValid()
						&& MainMenuSelection->GetSelectedIndex() == EntryIndex;
				})
				.OnSelected_Lambda([this, EntryIndex]()
				{
					SelectMainMenuEntry(EntryIndex);
				})
				.OnClicked_Lambda([this, EntryIndex]()
				{
					return ActivateMainMenuEntry(EntryIndex);
				})
				[
					SNew(SWoTMenuText)
					.PresentationAuthority(PresentationAuthority)
					.Text(Label)
					.Large(true)
					.Justification(ETextJustify::Center)
				]
			]
		];
		MainMenuButtons.Add(Button);
	}
	if (MainMenuSelection.IsValid()
		&& MainMenuButtons.IsValidIndex(MainMenuSelection->GetSelectedIndex()))
	{
		FirstFocusableWidget =
			MainMenuButtons[MainMenuSelection->GetSelectedIndex()];
	}

	const TSharedRef<SWidget> MenuList = SNew(SBox)
		.WidthOverride_Lambda([this]() { return MainMenuListWidth; })
		.HeightOverride_Lambda([this]() { return MainMenuListHeight; })
		[
			Entries
		];
	const TSharedRef<SWidget> UpdateBanner = BuildUpdateBanner();
	// The runtime entry list (onboarding rows included once they render
	// here) wraps top to bottom; after the last entry comes the update
	// banner, skipped while it is not interactive (spec r3 section 1a).
	TArray<TSharedPtr<SWidget>> MainNavigation;
	for (const TSharedPtr<SWoTMenuButton>& Button : MainMenuButtons)
	{
		MainNavigation.Add(Button);
	}
	MainNavigation.Add(UpdateBannerButton);
	WoTWrapVerticalNavigation(MainNavigation);
	const TSharedRef<SWidget> MainMenu = SNew(SBox)
		.WidthOverride_Lambda([this]() { return MainMenuSafeWidth; })
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			MenuList
		];
	const TSharedRef<SWidget> UpdateFailed = BuildUpdateFailedNotice();
	const TSharedRef<SWidget> ReleaseNotes = BuildReleaseNotesNotice();
	if (bShowUpdateFailedNotice
		&& &UpdateFailed.Get() == &SNullWidget::NullWidget.Get())
	{
		bShowUpdateFailedNotice = false;
		UpdateFailedContinueWidget.Reset();
	}
	if (bShowReleaseNotesNotice
		&& &ReleaseNotes.Get() == &SNullWidget::NullWidget.Get())
	{
		bShowReleaseNotesNotice = false;
		ReleaseNotesContinueWidget.Reset();
	}
	if (bShowReleaseNotesNotice && ReleaseNotesContinueWidget.IsValid())
	{
		FirstFocusableWidget = ReleaseNotesContinueWidget;
	}
	if (bShowUpdateFailedNotice && UpdateFailedContinueWidget.IsValid())
	{
		FirstFocusableWidget = UpdateFailedContinueWidget;
	}
	return SNew(SOverlay)
		+ SOverlay::Slot()[MainMenu]
		// Which build is running, small in the corner (the original has no
		// version label here; PORT_NOTES).
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.0f, 0.0f, 12.0f, 8.0f))
		[
			SAssignNew(VersionLabelWidget, STextBlock)
			.Text(WoTFrontEndSlate_Text(VersionLabel))
			.Font(this, &SWoTFrontEndRoot::PlayBodyFont)
			.ColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.55f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(TAttribute<FMargin>::CreateLambda([this]()
		{
			return FMargin(0.0f, 0.0f, 0.0f, UpdateBannerBottomMargin());
		}))
		[UpdateBanner]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[UpdateFailed]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[ReleaseNotes];
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildUpdateBanner()
{
	if (!PresentationAuthority.IsValid()
		|| !PresentationAuthority->PrepareBrowserResources())
	{
		return SNullWidget::NullWidget;
	}
	const FWoTRetailBrowserStyle* Browser =
		PresentationAuthority->BrowserStyle();
	if (Browser == nullptr) return SNullWidget::NullWidget;
	const FWoTRetailBrowserStyle& Style = *Browser;
	auto TextVisibility = [this](bool bReason)
	{
		const FWoTUpdateBannerPresentation Banner = UpdateBannerPresentation();
		const bool bHasText = bReason
			? !Banner.ReasonLine.IsEmpty() : !Banner.MultiplayerLine.IsEmpty();
		return bHasText ? EVisibility::Visible : EVisibility::Collapsed;
	};
	return SAssignNew(UpdateBannerWidget, SBox)
		.WidthOverride_Lambda([this]()
		{
			return UpdateBannerMaxWidth();
		})
		.Visibility_Lambda([this]()
		{
			return UpdateBannerPresentation().bVisible
				? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SBorder)
			.BorderImage(Style.WindowFrame())
			.Padding(FMargin(1.0f))
			[
				SNew(SBorder)
				.BorderImage(Style.FaceBackground())
				.Padding(FMargin(14.0f, 8.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Text_Lambda([this]()
							{
								return WoTFrontEndSlate_Text(
									UpdateBannerPresentation().PrimaryLine);
							})
							.Font(this, &SWoTFrontEndRoot::UpdateBannerBodyFont)
							.ColorAndOpacity(Style.BodyForeground())
							.WrapTextAt_Lambda([this]()
							{
								return UpdateBannerTextWrapWidth();
							})
						]
						+ SVerticalBox::Slot().AutoHeight()
						.Padding(0.0f, 2.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Visibility_Lambda([TextVisibility]()
							{
								return TextVisibility(/*bReason*/ true);
							})
							.Text_Lambda([this]()
							{
								return WoTFrontEndSlate_Text(
									UpdateBannerPresentation().ReasonLine);
							})
							.Font(this, &SWoTFrontEndRoot::UpdateBannerBodyFont)
							.ColorAndOpacity(Style.BodyForeground())
							.WrapTextAt_Lambda([this]()
							{
								return UpdateBannerTextWrapWidth();
							})
						]
						+ SVerticalBox::Slot().AutoHeight()
						.Padding(0.0f, 2.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Visibility_Lambda([TextVisibility]()
							{
								return TextVisibility(/*bReason*/ false);
							})
							.Text_Lambda([this]()
							{
								return WoTFrontEndSlate_Text(
									UpdateBannerPresentation().MultiplayerLine);
							})
							.Font(this, &SWoTFrontEndRoot::UpdateBannerBodyFont)
							.ColorAndOpacity(Style.BodyForeground())
							.WrapTextAt_Lambda([this]()
							{
								return UpdateBannerTextWrapWidth();
							})
						]
						+ SVerticalBox::Slot().AutoHeight()
						.Padding(0.0f, 2.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]()
							{
								return ViewModel.IsStatusError()
									&& !ViewModel.GetStatusText().IsEmpty()
									? EVisibility::Visible : EVisibility::Collapsed;
							})
							.Text_Lambda([this]()
							{
								return WoTFrontEndSlate_Text(ViewModel.GetStatusText());
							})
							.Font(this, &SWoTFrontEndRoot::UpdateBannerBodyFont)
							.ColorAndOpacity(FSlateColor(
								FLinearColor(1.0f, 0.35f, 0.3f, 1.0f)))
							.WrapTextAt_Lambda([this]()
							{
								return UpdateBannerTextWrapWidth();
							})
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 0.0f, 0.0f)
					[
						SAssignNew(UpdateBannerButton, SButton)
						.ButtonStyle(Style.ButtonStyle())
						.ContentPadding(FMargin(14.0f, 4.0f))
						.Visibility_Lambda([this]()
						{
							return UpdateBannerPresentation().Action
								!= EWoTUpdateBannerAction::None
								? EVisibility::Visible : EVisibility::Collapsed;
						})
						.IsEnabled_Lambda([this]()
						{
							return IsUpdateBannerInteractive();
						})
						.OnClicked(this, &SWoTFrontEndRoot::ActivateUpdateBanner)
						[
							SNew(STextBlock)
							.Text_Lambda([this]()
							{
								return WoTFrontEndSlate_Text(
									UpdateBannerPresentation().ButtonLabel);
							})
							.Font(this, &SWoTFrontEndRoot::UpdateBannerBodyFont)
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildPlayerConfigScreen()
{
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	Rows->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 5.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 14.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(RetailText(TEXT("menuPlayer.MenuList[1]")))
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.ColorAndOpacity(FSlateColor::UseForeground())
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			TextField(SAssignNew(PlayerNameTextBox, SEditableTextBox)
			.Style(FWoTMenuStyle::Get().EditableTextBoxStyle())
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.MaximumLength(20)
			.Text_Lambda([this]()
			{
				return WoTFrontEndSlate_Text(ViewModel.GetPendingPlayerName());
			})
			.OnTextChanged(this, &SWoTFrontEndRoot::SetPlayerName)
			.OnTextCommitted(this, &SWoTFrontEndRoot::CommitPlayerName),
			PlayerNameEntryRequest())
		]
	];
	Rows->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 2.0f, 0.0f, 13.0f)
	[
		SNew(STextBlock)
		.Text(RetailText(TEXT("menuPlayer.HelpMessage[1]")))
		.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
		.ColorAndOpacity(FWoTMenuStyle::SelectedColor())
		.AutoWrapText(true)
		.Justification(ETextJustify::Center)
	];
	TArray<TArray<TSharedPtr<SWidget>>> PlayerRows;
	PlayerRows.Add({PlayerNameTextBox});
	TSharedPtr<SWoTMenuButton> FirstFactionButton;
	for (const FWoTFactionChoice& Choice : FWoTFrontEndModel::FactionChoices())
	{
		const EWoTPlayerFaction Faction = Choice.Faction;
		TSharedPtr<SWoTMenuButton> FactionButton;
		Rows->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 5.0f)
		[
			SAssignNew(FactionButton, SWoTMenuButton)
			.ContentPadding(12.0f)
			.OnClicked_Lambda([this, Faction]()
			{
				ViewModel.SelectPendingFaction(Faction);
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(WoTFrontEndSlate_Text(Choice.TeamDescription))
					.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(18.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
					.Text_Lambda([this, Faction]()
					{
						return ViewModel.GetPendingFaction() == Faction
							? WoTFrontEndSlate_Text(TEXT("Current")) : FText::GetEmpty();
					})
				]
			]
		];
		if (!FirstFactionButton.IsValid())
		{
			FirstFactionButton = FactionButton;
		}
		PlayerRows.Add({FactionButton});
	}
	TSharedPtr<SWoTMenuButton> SkinPreviousButton;
	TSharedPtr<SWoTMenuButton> SkinNextButton;

	Rows->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 12.0f, 0.0f, 4.0f)
	[
		SNew(SBorder)
		.BorderImage(FWoTMenuStyle::Get().Blackout())
		.BorderBackgroundColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.52f))
		.Padding(FMargin(12.0f, 7.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 18.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(RetailText(TEXT("menuPlayer.MenuList[3]")))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			[
				SAssignNew(SkinPreviousButton, SWoTMenuButton)
				.ContentPadding(FMargin(12.0f, 4.0f))
				.OnClicked_Lambda([this]()
				{
					ViewModel.CyclePendingSkinColor(-1);
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return WoTFrontEndSlate_SkinLabel(
							ViewModel.GetPendingSkinColor(), -1);
					})
					.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Center)
			.VAlign(VAlign_Center).Padding(18.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return WoTFrontEndSlate_SkinLabel(
						ViewModel.GetPendingSkinColor(), 0);
				})
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity(FWoTMenuStyle::SelectedColor())
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			[
				SAssignNew(SkinNextButton, SWoTMenuButton)
				.ContentPadding(FMargin(12.0f, 4.0f))
				.OnClicked_Lambda([this]()
				{
					ViewModel.CyclePendingSkinColor(1);
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return WoTFrontEndSlate_SkinLabel(
							ViewModel.GetPendingSkinColor(), 1);
					})
					.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]
	];
	Rows->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 2.0f, 0.0f, 8.0f)
	[
		SNew(STextBlock)
		.Text(RetailText(TEXT("menuPlayer.HelpMessage[3]")))
		.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
		.ColorAndOpacity(FWoTMenuStyle::SelectedColor())
		.AutoWrapText(true)
		.Justification(ETextJustify::Center)
	];

	TSharedPtr<SWoTMenuButton> SaveButton;
	TSharedPtr<SWoTMenuButton> PlayerBackButton;
	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);
	Panel->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 20.0f)
	[
		SNew(SWoTMenuText)
		.PresentationAuthority(PresentationAuthority)
		.Text(RetailText(TEXT("menuPlayer.MenuTitle")))
		.Large(true)
	];
	Panel->AddSlot().AutoHeight()[Rows];
	Panel->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 18.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(RetailText(TEXT("menuPlayer.HelpMessage[2]")))
		.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
		.ColorAndOpacity(FWoTMenuStyle::SelectedColor())
		.AutoWrapText(true)
		.Justification(ETextJustify::Center)
	];
	Panel->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 22.0f, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SAssignNew(SaveButton, SWoTMenuButton)
			.ContentPadding(FMargin(18.0f, 8.0f))
			.OnClicked(this, &SWoTFrontEndRoot::SavePlayerConfig)
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("Save")))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SAssignNew(PlayerBackButton, SWoTMenuButton)
			.ContentPadding(FMargin(18.0f, 8.0f))
			.OnClicked(this, &SWoTFrontEndRoot::CancelPlayerConfig)
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("Back")))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
	];
	FirstFocusableWidget = PlayerNameTextBox.IsValid()
		? StaticCastSharedPtr<SWidget>(PlayerNameTextBox)
		: StaticCastSharedPtr<SWidget>(SaveButton);
	// Controller mode starts on Save (spec r3 section 1a, Player); the page
	// wraps top to bottom, the skin arrows and Save/Back side by side.
	PlayerConfigSaveButton = SaveButton;
	ControllerFocusWidget = SaveButton;
	PlayerRows.Add({SkinPreviousButton, SkinNextButton});
	PlayerRows.Add({SaveButton, PlayerBackButton});
	WoTWrapGridNavigation(PlayerRows);

	return SNew(SBox)
		.MaxDesiredWidth(700.0f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			Panel
		];
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildPlayScreen()
{
	// Retained connection failures can reopen Play without the main-menu action.
	const bool bBrowserReady = PresentationAuthority->PrepareBrowserResources();
	RebuildMapItems();
	if (BrowserSort)
	{
		*BrowserSort = FWoTBrowserSortState();
	}
	RebuildSessionItems();

	const TSharedRef<SWoTMenuButton> BackButton = SNew(SWoTMenuButton)
		.IsEnabled_Lambda([this]() { return ViewModel.CanLeaveOrSwitchNetwork(); })
		.OnClicked(this, &SWoTFrontEndRoot::BackFromPlay)
		[
			SNew(STextBlock)
			.Text(WoTFrontEndSlate_Text(TEXT("Back")))
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.ColorAndOpacity(FSlateColor::UseForeground())
		];
	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);
	Panel->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 12.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(STextBlock)
			.Text(RetailText(TEXT("menuMain.MenuList[2]")))
			.Font(this, &SWoTFrontEndRoot::PlayHeadingFont)
			.ColorAndOpacity(FWoTRetailBrowserStyle::GameTextColor())
		]
	];
	FirstFocusableWidget = BackButton;
	PlayBackButton = BackButton;
	if (!bBrowserReady)
	{
		Panel->AddSlot().FillHeight(1.0f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				// The Steam Deck legibility pass: the 2x body face, as the
				// rest of the multiplayer screens (was the 14-pixel strike).
				SNew(STextBlock)
				.Text(PresentationAuthority->BrowserResourceError())
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f)
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("Return to the main menu and try again.")))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(this, &SWoTFrontEndRoot::StatusText)
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.AutoWrapText(true)
			]
		];
		Panel->AddSlot().AutoHeight().HAlign(HAlign_Left)[BackButton];
		return SNew(SBox).Padding(24.0f)[Panel];
	}

	// Overlays fire no show or hide event, so the panels behind them are
	// disabled through a bound attribute: a disabled parent keeps every
	// control in it out of D-pad search (spec r3 [I6]).
	const TSharedRef<SWidget> HostPanel = BuildHostPanel();
	const TSharedRef<SWidget> JoinPanel = BuildJoinPanel();
	Panel->AddSlot()
	.FillHeight(1.0f)
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SHorizontalBox)
			.IsEnabled_Lambda([this]() { return !IsPlayOverlayVisible(); })
			+ SHorizontalBox::Slot()
			.FillWidth(0.36f)
			.Padding(0.0f, 0.0f, 20.0f, 0.0f)
			[
				HostPanel
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.64f)
			[
				JoinPanel
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			BuildOnlineDisclosure()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			BuildLocalNetworkNotice()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			BuildLocalNetworkHeadsUp()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			BuildKeychainHeadsUp()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			BuildPasswordPrompt()
		]
	];

	Panel->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 14.0f, 0.0f, 0.0f)
	[
		// The footer mirrors the panel columns: Back sits under the host panel's
		// left edge, Player Config against its right edge, and the status line
		// under the join panel.
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(0.36f)
		.Padding(0.0f, 0.0f, 20.0f, 0.0f)
		.VAlign(VAlign_Center)
		[
			// Disabled under an overlay with the panels (spec r3 [I6]).
			SNew(SHorizontalBox)
			.IsEnabled_Lambda([this]() { return !IsPlayOverlayVisible(); })
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[BackButton]
			+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right)
				.VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				// Retail keeps faction/name under Multiplayer as Player Config.
				SAssignNew(PlayerConfigButton, SWoTMenuButton)
				.IsEnabled_Lambda([this]() { return ViewModel.IsIdleOrSearching(); })
				.OnClicked(this, &SWoTFrontEndRoot::OpenPlayerConfig)
				[
					SNew(STextBlock)
					.Text(RetailText(TEXT("menuMultiPlayer.MenuList[2]")))
					.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(0.64f).VAlign(VAlign_Center)
		[
			// One fixed line: a longer message ellipsizes instead of growing the
			// footer and pushing the panels into a scroll.
			SNew(SBox)
			.HeightOverride(40.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SWoTFrontEndRoot::StatusText)
				// Readable at the 2x body face; failures are red so they cannot be missed.
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity_Lambda([this]()
				{
					return ViewModel.IsStatusError()
						? FSlateColor(FLinearColor(1.0f, 0.35f, 0.3f, 1.0f))
						: FSlateColor(FWoTMenuStyle::SelectedColor());
				})
				.AutoWrapText(false)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Justification(ETextJustify::Center)
			]
		]
	];
	// Two columns over the footer: Left/Right switch columns at the nearest
	// row (spec r3 section 1a, Play).
	const TArray<TSharedPtr<SWidget>> Footer = {BackButton, PlayerConfigButton};
	WoTTwoColumnNavigation(HostNavigationRows, JoinNavigationRows, Footer);
	// Controller mode starts on the host column's first non-text control, the
	// map list, unless an overlay is up (its own first control).
	const TSharedPtr<SWidget> OverlayFocus = PlayOverlayFocusTarget();
	ControllerFocusWidget = MapComboBox;
	if (OverlayFocus.IsValid())
	{
		ControllerFocusWidget = OverlayFocus;
	}
	return SNew(SBox).Padding(24.0f)[Panel];
}

FString SWoTFrontEndRoot::PasswordPromptText() const
{
	const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (!Sessions)
	{
		return FString();
	}
	const EWoTPasswordChallenge Challenge = Sessions->GetPasswordChallenge();
	const TCHAR* Key = Challenge == EWoTPasswordChallenge::WrongPassword
		? TEXT("WrongPassword") : TEXT("NeedPassword");
	FWoTRetailText::EnsureLoaded();
	if (const FString* Retail = FWoTRetailText::FindSectionValue(
		TEXT("GameInfo"), Key))
	{
		return *Retail;
	}
	return Challenge == EWoTPasswordChallenge::WrongPassword
		? FString(TEXT("That password did not work. Enter another password to continue."))
		: FString(TEXT("This server requires a password before you can join."));
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildPasswordPrompt()
{
	const FWoTRetailBrowserStyle* Browser = PresentationAuthority->BrowserStyle();
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (!Browser || !Sessions
		|| Sessions->GetPasswordChallenge() == EWoTPasswordChallenge::None)
	{
		PasswordPromptWidget.Reset();
		PasswordTextBox.Reset();
		return SNullWidget::NullWidget;
	}
	const FWoTRetailBrowserStyle& Style = *Browser;
	TSharedPtr<SWidget> PasswordConnectButton;
	TSharedPtr<SWidget> PasswordCancelButton;
	TSharedRef<SWidget> Prompt = SAssignNew(PasswordPromptWidget, SBox)
		.MaxDesiredWidth(680.0f)
		[
			SNew(SBorder)
			.BorderImage(Style.WindowFrame())
			.Padding(FMargin(1.0f))
			[
				SNew(SBorder)
				.BorderImage(Style.FaceBackground())
				.Padding(FMargin(22.0f, 18.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(WoTFrontEndSlate_Text(TEXT("Server Password")))
						.Font(this, &SWoTFrontEndRoot::PlayHeadingFont)
						.ColorAndOpacity(Style.BodyForeground())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f)
					[
						SNew(STextBlock)
						.Text(WoTFrontEndSlate_Text(PasswordPromptText()))
						.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
						.ColorAndOpacity(Style.BodyForeground())
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						TextField(SAssignNew(PasswordTextBox, SEditableTextBox)
						.Style(Style.EditableTextBoxStyle())
						.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
						.IsPassword(true)
						.OnTextCommitted(this,
							&SWoTFrontEndRoot::CommitServerPassword),
						ServerPasswordEntryRequest())
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
					.HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SAssignNew(PasswordConnectButton, SButton)
							.ButtonStyle(Style.ButtonStyle())
							.OnClicked(this,
								&SWoTFrontEndRoot::SubmitServerPassword)
							[
								SNew(STextBlock)
								.Text(WoTFrontEndSlate_Text(TEXT("Connect")))
								.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SAssignNew(PasswordCancelButton, SButton)
							.ButtonStyle(Style.ButtonStyle())
							.OnClicked(this,
								&SWoTFrontEndRoot::CancelServerPassword)
							[
								SNew(STextBlock)
								.Text(WoTFrontEndSlate_Text(TEXT("Cancel")))
								.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
							]
						]
					]
				]
			]
		];
	FirstFocusableWidget = PasswordTextBox;
	// Box above Connect and Cancel; focus stays in the prompt.
	TArray<TArray<TSharedPtr<SWidget>>> PasswordRows;
	PasswordRows.Add({PasswordTextBox});
	PasswordRows.Add({PasswordConnectButton, PasswordCancelButton});
	WoTWrapGridNavigation(PasswordRows);
	return Prompt;
}

FReply SWoTFrontEndRoot::SubmitServerPassword()
{
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (!Sessions || !PasswordTextBox.IsValid()
		|| !Sessions->SubmitServerPassword(
			PasswordTextBox->GetText().ToString()))
	{
		ViewModel.ReportNotice(
			TEXT("Enter a valid password before connecting."), /*bError*/ true);
		return FReply::Handled();
	}
	RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::CancelServerPassword()
{
	if (UWoTSessionSubsystem* Sessions = SessionSubsystem.Get())
	{
		Sessions->DismissPasswordChallenge();
	}
	RebuildScreen();
	return FReply::Handled();
}

void SWoTFrontEndRoot::CommitServerPassword(
	const FText&, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter)
	{
		SubmitServerPassword();
	}
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildHostPanel()
{
	// One scroll-box slot per row: Up/Down then walks the rows and scrolls each
	// into view, and any focus change scrolls its row into view. A single slot
	// holding every row left clipped rows, Host Match included, unreachable
	// without a mouse.
	TSharedRef<SScrollBox> Panel = SAssignNew(HostScrollBox, SScrollBox)
		.Style(FWoTMenuStyle::Get().ScrollBoxStyle())
		.ScrollBarStyle(WoTFrontEndSlate_HostScrollBarStyle())
		.ScrollBarAlwaysVisible(true)
		.ScrollBarThickness(FVector2D(12.0f, 12.0f))
		.ScrollBarPadding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
		.ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll);
	// Retail's UWindow controls draw the label left of the control on one line.
	// Each row stays one scroll-box slot so navigation still walks it, and every
	// label shares one column (HostLabelColumnWidth) so the controls line up.
	// The 2x body face keeps its pixel size, so a small window leaves less room
	// beside it: once the column and a usable control no longer fit, the
	// control wraps under its label instead of being squeezed to nothing.
	auto InlineRow = [this](const TAttribute<FText>& Label,
		const TSharedRef<SWidget>& Control) -> TSharedRef<SWidget>
	{
		return SNew(SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D(0.0f, 4.0f))
			// Filling applies only to the last slot on a line, so the label keeps
			// the column width inline and is held to the line once wrapped.
			+ SWrapBox::Slot().FillEmptySpace(true).VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride_Lambda([this]()
				{
					return HostLabelColumnWidth > 0.0f
						? FOptionalSize(HostLabelColumnWidth) : FOptionalSize();
				})
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				]
			]
			+ SWrapBox::Slot().FillEmptySpace(true).VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(WoTFrontEndSlate_HostMinControlWidth)
				[
					Control
				]
			];
	};
	auto SliderWithValue = [this](const TSharedRef<SWidget>& Slider,
		TFunction<FText()> Value) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				Slider
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f)
			[
				SNew(SWoTSliderValueLabel)
				.Text_Lambda(MoveTemp(Value))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			];
	};
	// Each control joins the Play focus graph's host column, in row order.
	auto HostControl = [this](const TSharedRef<SWidget>& Control) -> TSharedRef<SWidget>
	{
		HostNavigationRows.Add({Control});
		return Control;
	};
	// Battle-only rows hide whole, label and control together.
	auto BattleOnly = [this](const TSharedRef<SWidget>& Row) -> TSharedRef<SWidget>
	{
		Row->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]()
		{
			return ViewModel.IsBattleMapSelected()
				? EVisibility::Visible : EVisibility::Collapsed;
		}));
		return Row;
	};
	Panel->AddSlot().AutoSize().Padding(0.0f, 0.0f, 0.0f, 10.0f)
	[
		SNew(STextBlock)
		.Text(this, &SWoTFrontEndRoot::HostPanelTitle)
		.Font(this, &SWoTFrontEndRoot::PlayHeadingFont)
		.ColorAndOpacity(FWoTRetailBrowserStyle::GameTextColor())
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 0.0f, 0.0f, 9.0f)
	[
		InlineRow(RetailText(TEXT("menuStartArenaServer.MenuList[7]")),
			TextField(HostControl(SAssignNew(ServerNameTextBox, SEditableTextBox)
			.Style(FWoTMenuStyle::Get().EditableTextBoxStyle())
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.Text_Lambda([this]()
			{
				return WoTFrontEndSlate_Text(ViewModel.GetHostRequest().ServerName);
			})
			.OnTextChanged(this, &SWoTFrontEndRoot::SetServerName)),
			ServerNameEntryRequest()))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 0.0f, 0.0f, 5.0f)
	[
		InlineRow(RetailText(TEXT("menuStartArenaServer.MenuList[1]")),
			// The touch-friendly list (spec r3 section 2): D-pad Up/Down move
			// focus off it while closed, never the selected map.
			HostControl(SAssignNew(MapComboBox, SWoTListCombo)
			.ComboBoxStyle(FWoTMenuStyle::Get().ComboBoxStyle())
			// The live frame brush: the combo style holds a copy of it that
			// is not rebound when the menu textures are.
			.MenuBorderBrush(FWoTMenuStyle::Get().Frame())
			.ItemStyle(FWoTMenuStyle::Get().TableRowStyle())
			.ScrollBarStyle(FWoTMenuStyle::Get().ScrollBarStyle())
			.ForegroundColor(FWoTMenuStyle::MenuColor(false))
			.NumItems_Lambda([this]() { return MapItems.Num(); })
			.SelectedIndex_Lambda([this]() { return MapItems.IndexOfByKey(SelectedMapItem); })
			.OnGenerateItem_Lambda([this](int32 Index)
			{
				return GenerateMapWidget(MapItems.IsValidIndex(Index) ? MapItems[Index] : FMapItem());
			})
			.OnPicked_Lambda([this](int32 Index, ESelectInfo::Type SelectInfo)
			{
				if (MapItems.IsValidIndex(Index))
				{
					SelectMap(MapItems[Index], SelectInfo);
				}
			})
			[
				SNew(STextBlock)
				.Text(this, &SWoTFrontEndRoot::SelectedMapText)
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			]))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 0.0f)
	[
		HostControl(SNew(SCheckBox)
		.Style(FWoTMenuStyle::Get().CheckBoxStyle())
		.IsChecked_Lambda([this]()
		{
			return ViewModel.GetHostRequest().bCycleLevels
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			ViewModel.SetCycleLevels(State == ECheckBoxState::Checked);
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return RetailText(ViewModel.IsBattleMapSelected()
					? FName(TEXT("menuStartBattleServer.MenuList[4]"))
					: FName(TEXT("menuStartArenaServer.MenuList[4]")));
			})
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
		])
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		InlineRow(RetailText(TEXT("menuStartArenaServer.MenuList[2]")), SliderWithValue(
			HostControl(SNew(SWoTRetailSlider)
			.Value_Lambda([this]()
			{
				return ViewModel.GetHostRequest().FragLimit / 100.0f;
			})
			// menuStartArenaServer: 0-100 kills in steps of one.
			.Positions(101)
			.OnValueChanged_Lambda([this](float Value)
			{
				SetFragLimit(FMath::RoundToInt(Value * 100.0f));
			})),
			[this]() { return FText::AsNumber(ViewModel.GetHostRequest().FragLimit); }))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		InlineRow(RetailText(TEXT("menuStartArenaServer.MenuList[3]")), SliderWithValue(
			HostControl(SNew(SWoTRetailSlider)
			.Value_Lambda([this]()
			{
				return ViewModel.GetHostRequest().TimeLimit / 60.0f;
			})
			// menuStartArenaServer: 0-60 minutes in steps of one.
			.Positions(61)
			.OnValueChanged_Lambda([this](float Value)
			{
				SetTimeLimit(FMath::RoundToInt(Value * 60.0f));
			})),
			[this]() { return FText::AsNumber(ViewModel.GetHostRequest().TimeLimit); }))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		BattleOnly(InlineRow(RetailText(TEXT("menuStartBattleServer.MenuList[6]")),
			SliderWithValue(
			HostControl(SNew(SWoTRetailSlider)
			.Value_Lambda([this]()
			{
				return (ViewModel.GetHostRequest().SealGoal - 2) / 2.0f;
			})
			// menuStartBattleServer.uc:6,154-156: 2-4 in unit steps.
			.Positions(3)
			.OnValueChanged_Lambda([this](float Value)
			{
				SetSealGoal(2 + FMath::RoundToInt(Value * 2.0f));
			})),
			[this]() { return FText::AsNumber(ViewModel.GetHostRequest().SealGoal); })))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		BattleOnly(InlineRow(RetailText(TEXT("menuStartBattleServer.MenuList[7]")),
			SliderWithValue(
			HostControl(SNew(SWoTRetailSlider)
			.Value_Lambda([this]()
			{
				const int32 Min = ViewModel.GetMinSealBudget();
				const int32 Max = ViewModel.GetMaxSealBudget();
				return Max > Min
					? (ViewModel.GetHostRequest().SealBudget - Min)
						/ static_cast<float>(Max - Min)
					: 0.0f;
			})
			// menuStartBattleServer.uc:7,159-161: step only inside retail bounds.
			.Positions(3)
			.OnValueChanged_Lambda([this](float Value)
			{
				const int32 Min = ViewModel.GetMinSealBudget();
				const int32 Max = ViewModel.GetMaxSealBudget();
				SetSealBudget(Min + FMath::RoundToInt(Value * (Max - Min)));
			})),
			[this]() { return FText::AsNumber(ViewModel.GetHostRequest().SealBudget); })))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		InlineRow(RetailText(TEXT("menuStartArenaServer.MenuList[5]")), SliderWithValue(
			HostControl(SNew(SWoTRetailSlider)
			.Value_Lambda([this]()
			{
				return (ViewModel.GetHostRequest().MaxPlayers - 2) / 14.0f;
			})
			// menuStartArenaServer: 2-16 players in steps of one.
			.Positions(15)
			.OnValueChanged_Lambda([this](float Value)
			{
				SetMaxPlayers(2 + FMath::RoundToInt(Value * 14.0f));
			})),
			[this]() { return FText::AsNumber(ViewModel.GetHostRequest().MaxPlayers); }))
	];
	Panel->AddSlot().AutoSize()
	[
		HostControl(SNew(SCheckBox)
		.Visibility_Lambda([this]()
		{
			return ViewModel.IsBattleMapSelected()
				? EVisibility::Visible : EVisibility::Collapsed;
		})
		.Style(FWoTMenuStyle::Get().CheckBoxStyle())
		.IsChecked_Lambda([this]()
		{
			return ViewModel.GetHostRequest().bAutoTeamSelect
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			ViewModel.SetAutoTeamSelect(State == ECheckBoxState::Checked);
		})
		[
			SNew(STextBlock)
			.Text(RetailText(TEXT("menuStartBattleServer.MenuList[8]")))
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
		])
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		BattleOnly(InlineRow(RetailText(TEXT("menuStartBattleServer.MenuList[10]")),
			SliderWithValue(
			HostControl(SNew(SWoTRetailSlider)
			.Value_Lambda([this]()
			{
				const int32 Citadels = FMath::Max(2,
					FWoTFrontEndModel::BattleCitadelCount(
						ViewModel.GetHostRequest().MapPackage));
				const int32 Max = 16 / Citadels;
				return Max > 1 ? (ViewModel.GetHostRequest().MaxTeamSize - 1)
					/ static_cast<float>(Max - 1) : 0.0f;
			})
			// menuStartBattleServer.uc:130,196: 1 to 16 / citadels in unit steps,
			// one stop each. Eight fixed stops put the 3- and 4-citadel sizes on
			// cell boundaries, where Left or Right could not leave them.
			.Positions_Lambda([this]()
			{
				return 16 / FMath::Max(2, FWoTFrontEndModel::BattleCitadelCount(
					ViewModel.GetHostRequest().MapPackage));
			})
			.OnValueChanged_Lambda([this](float Value)
			{
				const int32 Citadels = FMath::Max(2,
					FWoTFrontEndModel::BattleCitadelCount(
						ViewModel.GetHostRequest().MapPackage));
				const int32 Max = 16 / Citadels;
				ViewModel.SetMaxTeamSize(1 + FMath::RoundToInt(Value * (Max - 1)));
			})),
			[this]() { return FText::AsNumber(ViewModel.GetHostRequest().MaxTeamSize); })))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		InlineRow(TAttribute<FText>::CreateLambda([this]()
		{
			return RetailText(ViewModel.IsBattleMapSelected()
				? FName(TEXT("menuStartBattleServer.MenuList[11]"))
				: FName(TEXT("menuStartArenaServer.MenuList[6]")));
		}),
			SliderWithValue(
			HostControl(SNew(SWoTRetailSlider)
			.Value_Lambda([this]()
			{
				const int32 TickRate = ViewModel.GetHostRequest().MaxClientTickRate;
				return TickRate == 0 ? 0.0f : (TickRate - 9) / 21.0f;
			})
			.Positions(22)
			.OnValueChanged_Lambda([this](float Value)
			{
				const int32 Index = FMath::RoundToInt(Value * 21.0f);
				ViewModel.SetMaxClientTickRate(Index == 0 ? 0 : Index + 9);
			})),
			[this]()
			{
				return FText::FromString(FString::Printf(TEXT("%d fps"),
					ViewModel.GetHostRequest().MaxClientTickRate));
			}))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		InlineRow(TAttribute<FText>::CreateLambda([this]()
		{
			return RetailText(ViewModel.IsBattleMapSelected()
				? FName(TEXT("menuStartBattleServer.MenuList[13]"))
				: FName(TEXT("menuStartArenaServer.MenuList[8]")));
		}),
			TextField(HostControl(SAssignNew(AdminPasswordTextBox, SEditableTextBox)
			.Style(FWoTMenuStyle::Get().EditableTextBoxStyle())
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.IsPassword(true)
			.Text_Lambda([this]()
			{
				return FText::FromString(ViewModel.GetHostRequest().AdminPassword);
			})
			.OnTextChanged_Lambda([this](const FText& Text)
			{
				ViewModel.SetAdminPassword(Text.ToString());
			})),
			HostPasswordEntryRequest(/*bAdmin*/ true)))
	];
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 6.0f)
	[
		InlineRow(TAttribute<FText>::CreateLambda([this]()
		{
			return RetailText(ViewModel.IsBattleMapSelected()
				? FName(TEXT("menuStartBattleServer.MenuList[14]"))
				: FName(TEXT("menuStartArenaServer.MenuList[9]")));
		}),
			TextField(HostControl(SAssignNew(GamePasswordTextBox, SEditableTextBox)
			.Style(FWoTMenuStyle::Get().EditableTextBoxStyle())
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.IsPassword(true)
			.Text_Lambda([this]()
			{
				return FText::FromString(ViewModel.GetHostRequest().GamePassword);
			})
			.OnTextChanged_Lambda([this](const FText& Text)
			{
				ViewModel.SetGamePassword(Text.ToString());
			})),
			HostPasswordEntryRequest(/*bAdmin*/ false)))
	];
	TArray<TSharedPtr<SWidget>> HostNetworkRow;
	Panel->AddSlot().AutoSize().Padding(0.0f, 4.0f, 0.0f, 8.0f)
	[
		BuildNetworkModeRow(/*bBrowser*/ false, &HostNetworkRow)
	];
	HostNavigationRows.Add(HostNetworkRow);
	TSharedPtr<SWidget> ResetIdentityButton;
	// The internet status line and the relay privacy option live once, in the
	// browser panel (BuildInternetOptions); the host panel pairs the identity
	// reset with Host Match on one row to keep the panel short.
	Panel->AddSlot().AutoSize()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SAssignNew(ResetIdentityButton, SButton)
			.ButtonStyle(PresentationAuthority->BrowserStyle()->ButtonStyle())
			.ContentPadding(FMargin(10.0f, 8.0f))
			.Visibility(this, &SWoTFrontEndRoot::ResetIdentityVisibility)
			.IsEnabled_Lambda([this]() { return ViewModel.CanStartAsyncAction(); })
			.OnClicked(this, &SWoTFrontEndRoot::ResetOnlineIdentity)
			[
				SAssignNew(ResetIdentityTextWidget, STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("Reset online identity")))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SAssignNew(HostMatchButton, SButton)
			.ButtonStyle(PresentationAuthority->BrowserStyle()->ButtonStyle())
			.ContentPadding(FMargin(18.0f, 8.0f))
			.HAlign(HAlign_Center)
			.IsEnabled_Lambda([this]() { return ViewModel.CanHost(); })
			.OnClicked(this, &SWoTFrontEndRoot::Host)
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("Host Match")))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
	];
	HostNavigationRows.Add({ResetIdentityButton, HostMatchButton});
	return SNew(SBorder)
		.BorderImage(PresentationAuthority->BrowserStyle()->WindowFrame())
		.Padding(14.0f)
		[
			Panel
		];
}

FText SWoTFrontEndRoot::HostPanelTitle() const
{
	return RetailText(ViewModel.HostTitleLocalizationId());
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildJoinPanel()
{
	const FWoTRetailBrowserStyle* Browser = PresentationAuthority->BrowserStyle();
	if (!Browser) return SNullWidget::NullWidget;
	const FWoTRetailBrowserStyle& Style = *Browser;
	auto MakeHeading =
		[this, StylePtr = &Style](const TCHAR* Label, EWoTBrowserSortColumn Column)
			-> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(StylePtr->HeadingStyle())
			.ContentPadding(FMargin(4.0f, 1.0f))
			.HAlign(HAlign_Left)
			.OnClicked_Lambda([this, Column]()
			{
				return SelectBrowserSortColumn(Column);
			})
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(Label))
				.Font(this, &SWoTFrontEndRoot::PlayBrowserFont)
				.ColorAndOpacity_Lambda([this, StylePtr, Column]()
				{
					return StylePtr->HeadingForeground(
						BrowserSort && BrowserSort->Column == Column);
				})
			];
	};

	// The Play focus graph's join column, row by row.
	TArray<TSharedPtr<SWidget>> JoinTopRow;
	TArray<TSharedPtr<SWidget>> JoinOptionsRow;
	TArray<TSharedPtr<SWidget>> JoinHeadingRow;
	TSharedPtr<SWidget> RefreshButton;
	TSharedPtr<SWidget> JoinSelectedButton;
	TSharedPtr<SWidget> DirectAddressBox;
	TSharedPtr<SWidget> DirectConnectButton;
	TSharedRef<SHorizontalBox> Headings = SNew(SHorizontalBox);
	const TSharedRef<SWidget> ServerHeading =
		MakeHeading(TEXT("Server"), EWoTBrowserSortColumn::Server);
	const TSharedRef<SWidget> PingHeading =
		MakeHeading(TEXT("Ping"), EWoTBrowserSortColumn::Ping);
	const TSharedRef<SWidget> MapHeading =
		MakeHeading(TEXT("Map"), EWoTBrowserSortColumn::Map);
	const TSharedRef<SWidget> PlayersHeading =
		MakeHeading(TEXT("Players"), EWoTBrowserSortColumn::Players);
	JoinHeadingRow = {ServerHeading, PingHeading, MapHeading, PlayersHeading};
	Headings->AddSlot().FillWidth(0.40f)
	[
		ServerHeading
	];
	Headings->AddSlot().FillWidth(0.15f)
	[
		PingHeading
	];
	Headings->AddSlot().FillWidth(0.25f)
	[
		MapHeading
	];
	Headings->AddSlot().FillWidth(0.20f)
	[
		PlayersHeading
	];
	// SListView reserves this slot for its 24px bar plus surrounding 2px
	// gutters. Mirror it so every heading boundary matches its row boundary.
	Headings->AddSlot().AutoWidth()
	[
		SNew(SBox)
		.WidthOverride(Style.ScrollBarReservation())
		[
			SNew(SBorder)
			.BorderImage(Style.FaceBackground())
			.Padding(0.0f)
		]
	];

	TSharedRef<SVerticalBox> Client = SNew(SVerticalBox);
	Client->AddSlot().AutoHeight()
	[
		SNew(SBorder)
		.BorderImage(Style.FaceBackground())
		.Padding(FMargin(4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SAssignNew(RefreshButton, SButton)
				.ButtonStyle(Style.ButtonStyle())
				.ContentPadding(FMargin(12.0f, 4.0f))
				.IsEnabled_Lambda([this]() { return ViewModel.CanStartAsyncAction(); })
				.OnClicked(this, &SWoTFrontEndRoot::RefreshSessions)
				[
					SNew(STextBlock)
						.Text(WoTFrontEndSlate_Text(TEXT("Refresh Sessions")))
						.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				BuildNetworkModeRow(/*bBrowser*/ true, &JoinTopRow)
			]
		]
	];
	JoinTopRow.Insert(RefreshButton, 0);
	Client->AddSlot().AutoHeight()
	[
		SNew(SBorder)
		.BorderImage(Style.FaceBackground())
		.Padding(FMargin(4.0f, 2.0f))
		[
			BuildInternetOptions(/*bBrowser*/ true, &JoinOptionsRow)
		]
	];
	Client->AddSlot().AutoHeight()
	[
		SNew(SBox)
		.MinDesiredHeight(Style.HeadingHeight())
		[
			Headings
		]
	];
	Client->AddSlot().FillHeight(1.0f)
	[
		SNew(SBorder)
		.BorderImage(Style.ClientBackground())
		.Padding(0.0f)
		[
			SNew(SBox)
			.MinDesiredHeight(240.0f)
			[
				SAssignNew(SessionListView, SListView<FSessionItem>)
				.ListViewStyle(Style.TableViewStyle())
				.ShadowBoxStyle(Style.ScrollBoxStyle())
				.ScrollBarStyle(Style.ScrollBarStyle())
				.ListItemsSource(&SessionItems)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SWoTFrontEndRoot::GenerateSessionRow)
				.OnSelectionChanged(this, &SWoTFrontEndRoot::SelectSession)
				.OnMouseButtonDoubleClick(
					this, &SWoTFrontEndRoot::DoubleClickSession)
			]
		]
	];
	Client->AddSlot().AutoHeight()
	[
		SNew(SBorder)
		.BorderImage(Style.FaceBackground())
		.Padding(FMargin(4.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(JoinSelectedButton, SButton)
				.ButtonStyle(Style.ButtonStyle())
				.ContentPadding(FMargin(12.0f, 4.0f))
				.HAlign(HAlign_Center)
				.IsEnabled_Lambda([this]()
				{
					return ViewModel.CanStartAsyncAction()
						&& SelectedSessionItem.IsValid();
				})
				.OnClicked(this, &SWoTFrontEndRoot::JoinSelectedSession)
				[
					SNew(STextBlock)
						.Text(WoTFrontEndSlate_Text(TEXT("Join Selected")))
						.Font(this, &SWoTFrontEndRoot::PlayBrowserFont)
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("Direct Address")))
				.Font(this, &SWoTFrontEndRoot::PlayBrowserFont)
				.ColorAndOpacity(Style.BodyForeground())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(
					0.0f, 0.0f, 4.0f, 0.0f)
				[
					TextField(SAssignNew(DirectAddressTextBox, SEditableTextBox)
						.Style(Style.EditableTextBoxStyle())
						.Font(this, &SWoTFrontEndRoot::PlayBrowserFont)
						.HintText(WoTFrontEndSlate_Text(
								TEXT("192.0.2.10:7777")))
						// Shows the address the root keeps, so a rebuild or
						// the keyboard panel's Done is reflected here.
						.Text_Lambda([this]() { return WoTFrontEndSlate_Text(DirectAddress); })
						.OnTextChanged(this, &SWoTFrontEndRoot::SetDirectAddress),
						DirectAddressEntryRequest())
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SAssignNew(DirectConnectButton, SButton)
						.ButtonStyle(Style.ButtonStyle())
						.ContentPadding(FMargin(10.0f, 3.0f))
						.IsEnabled_Lambda([this]()
						{
							return ViewModel.IsIdleOrSearching();
						})
						.OnClicked(this, &SWoTFrontEndRoot::ConnectDirect)
						[
							SNew(STextBlock)
								.Text(WoTFrontEndSlate_Text(TEXT("Connect")))
								.Font(this, &SWoTFrontEndRoot::PlayBrowserFont)
								.ColorAndOpacity(FSlateColor::UseForeground())
						]
				]
			]
		]
	];

	DirectAddressBox = DirectAddressTextBox;
	JoinNavigationRows.Add(JoinTopRow);
	JoinNavigationRows.Add(JoinOptionsRow);
	JoinNavigationRows.Add(JoinHeadingRow);
	JoinNavigationRows.Add({SessionListView});
	JoinNavigationRows.Add({JoinSelectedButton});
	JoinNavigationRows.Add({DirectAddressBox, DirectConnectButton});
	return SNew(SBorder)
		.BorderImage(Style.WindowFrame())
		.Padding(FMargin(4.0f, 0.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.MinDesiredHeight(Style.TitleBarHeight())
				.Padding(FMargin(12.0f, 10.0f))
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
						// WOTBrowserMainWindow.uc:10 +
						// UBrowserMainWindow.uc:107.
						.Text(WoTFrontEndSlate_Text(
							TEXT("WoT Reborn Server Browser")))
						.Font(this, &SWoTFrontEndRoot::PlayHeadingFont)
						.ColorAndOpacity(Style.TitleForeground())
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				Client
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.MinDesiredHeight(Style.StatusBarHeight())
				.Padding(FMargin(8.0f, 3.0f))
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
						.Text(this, &SWoTFrontEndRoot::BrowserStatusText)
						.Font(this, &SWoTFrontEndRoot::PlayBrowserFont)
						.ColorAndOpacity(Style.BodyForeground())
				]
			]
		];
}

FReply SWoTFrontEndRoot::OpenPlay()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	DirectAddress.Reset();
	ClearPendingNotices();
	ViewModel.ShowPlay(FWoTFrontEndMapCatalog::Enumerate());
	RebuildScreen();
	AutoFetchSessions();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::OpenPlayerConfig()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	EWoTPlayerFaction SavedFaction = EWoTPlayerFaction::AesSedai;
	EWoTPlayerSkinColor SavedSkinColor = EWoTPlayerSkinColor::Base;
	FString SavedPlayerName;
	if (UWoTGameInstance* Instance = GameInstance.Get())
	{
		SavedFaction = Instance->GetSelectedFaction();
		SavedSkinColor = Instance->GetSelectedSkinColor();
		SavedPlayerName = Instance->GetPlayerName();
	}
	// An empty saved name shows the computer-name default, the same name
	// UWoTLocalPlayer::GetNickname sends at login.
	ViewModel.ShowPlayerConfig(
		SavedFaction, SavedPlayerName, UWoTLocalPlayer::DefaultPlayerName(),
		SavedSkinColor);
	RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::OpenControls()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	ViewModel.ShowControls();
	RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::OpenHardware()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	ViewModel.ShowHardware();
	RebuildScreen();
	return FReply::Handled();
}

void SWoTFrontEndRoot::CloseSettingsPage()
{
	if (!IsPresentationReady()) return;
	if (ViewModel.BackToMain())
	{
		RebuildScreen();
		bRestoreFocusOnNextTick = true;
		RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(
			this, &SWoTFrontEndRoot::RestoreFocusAfterBack));
	}
}

UWoTPreferencesSubsystem* SWoTFrontEndRoot::ResolvePreferences() const
{
#if WITH_AUTOMATION_TESTS
	if (UWoTPreferencesSubsystem* Injected = PreferencesForTesting.Get())
	{
		return Injected;
	}
#endif
	UWoTGameInstance* Instance = GameInstance.Get();
	return Instance ? Instance->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
}

UWoTBootstrapSubsystem* SWoTFrontEndRoot::ResolveBootstrap() const
{
#if WITH_AUTOMATION_TESTS
	if (UWoTBootstrapSubsystem* Injected = BootstrapForTesting.Get())
	{
		return Injected;
	}
#endif
	UWoTGameInstance* Instance = GameInstance.Get();
	return Instance ? Instance->GetSubsystem<UWoTBootstrapSubsystem>() : nullptr;
}

FWoTUpdateState SWoTFrontEndRoot::ResolveUpdateState() const
{
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	if (TOptional<FWoTUpdateState> Fake =
		WoTFakeUpdateStateFromCommandLine(); Fake.IsSet())
	{
		return MoveTemp(Fake.GetValue());
	}
#endif
	const UWoTBootstrapSubsystem* Bootstrap = ResolveBootstrap();
	return Bootstrap ? Bootstrap->Snapshot().Update : FWoTUpdateState();
}

FWoTUpdateBannerPresentation SWoTFrontEndRoot::UpdateBannerPresentation() const
{
	return WoTBuildUpdateBannerPresentation(ResolveUpdateState());
}

float SWoTFrontEndRoot::UpdateBannerMaxWidth() const
{
	const float NoticeWidth = 1200.0f / FMath::Max(LiveViewportScale, 0.01f);
	const float ViewportCap = static_cast<float>(
		static_cast<int32>(LiveViewportWidth * 0.9f));
	return FMath::Max(120.0f, FMath::Min(NoticeWidth, ViewportCap));
}

float SWoTFrontEndRoot::UpdateBannerContentWidth() const
{
	// Two 1px frame edges plus the face's 14px horizontal padding.
	return FMath::Max(90.0f, UpdateBannerMaxWidth() - 30.0f);
}

float SWoTFrontEndRoot::UpdateBannerButtonColumnWidthForFont(
	const FSlateFontInfo& Font) const
{
	const FWoTUpdateBannerPresentation Banner = UpdateBannerPresentation();
	if (Banner.Action == EWoTUpdateBannerAction::None) return 0.0f;
	const FVector2D LabelSize = FWoTMenuLayout::MeasureText(
		WoTFrontEndSlate_Text(Banner.ButtonLabel), Font);
	const FWoTRetailBrowserStyle* Browser = PresentationAuthority.IsValid()
		? PresentationAuthority->BrowserStyle() : nullptr;
	const FMargin StylePadding = Browser != nullptr
		? Browser->ButtonStyle()->NormalPadding : FMargin(0.0f);
	// The slot's 12px gap, the button's explicit 14px content padding, and
	// the style padding all consume space beside the text column.
	return 12.0f + LabelSize.X + 28.0f
		+ StylePadding.Left + StylePadding.Right;
}

float SWoTFrontEndRoot::UpdateBannerTextWrapWidthForFont(
	const FSlateFontInfo& Font) const
{
	return FMath::Max(40.0f, UpdateBannerContentWidth()
		- UpdateBannerButtonColumnWidthForFont(Font));
}

float SWoTFrontEndRoot::UpdateBannerEstimatedHeightForFont(
	const FSlateFontInfo& Font) const
{
	const FWoTUpdateBannerPresentation Banner = UpdateBannerPresentation();
	const float WrapWidth = UpdateBannerTextWrapWidthForFont(Font);
	const float LineHeight = FMath::Max(
		1.0f, FWoTMenuLayout::MeasureFontHeight(Font));
	int32 TextLineCount = 0;
	int32 TextBlockCount = 0;
	const auto AddText = [&TextLineCount, &TextBlockCount, WrapWidth, &Font](
		const FString& Text)
	{
		if (Text.IsEmpty()) return;
		const float Width = FWoTMenuLayout::MeasureText(
			WoTFrontEndSlate_Text(Text), Font).X;
		int32 WrappedLines = FMath::Max(1, FMath::CeilToInt(Width / WrapWidth));
		// Word boundaries can leave unused room at the end of a wrapped line.
		// One conservative extra line makes the compact-font decision stable.
		if (WrappedLines > 1) ++WrappedLines;
		TextLineCount += WrappedLines;
		++TextBlockCount;
	};
	AddText(Banner.PrimaryLine);
	AddText(Banner.ReasonLine);
	AddText(Banner.MultiplayerLine);
	if (ViewModel.IsStatusError()) AddText(ViewModel.GetStatusText());
	const float TextHeight = TextLineCount * LineHeight
		+ FMath::Max(0, TextBlockCount - 1) * 2.0f;

	float ButtonHeight = 0.0f;
	if (Banner.Action != EWoTUpdateBannerAction::None)
	{
		const FWoTRetailBrowserStyle* Browser = PresentationAuthority.IsValid()
			? PresentationAuthority->BrowserStyle() : nullptr;
		const FMargin StylePadding = Browser != nullptr
			? Browser->ButtonStyle()->NormalPadding : FMargin(0.0f);
		ButtonHeight = LineHeight + 8.0f
			+ StylePadding.Top + StylePadding.Bottom;
	}
	// Two 1px frame edges and the face's 8px vertical padding surround the row.
	return 18.0f + FMath::Max(TextHeight, ButtonHeight);
}

bool SWoTFrontEndRoot::ShouldUseCompactUpdateBannerFont() const
{
	// The last resort only: the wrapped 2x banner overruns the room below the
	// menu. The Slate canvas is about 1080 units tall at every resolution, so
	// this never happens at 1280x720 or larger; it keeps the menu clear on the
	// smallest windows (640x480), where the 14-pixel face still passes the
	// 12-pixel floor. A long reason line on a wide screen wraps instead (the
	// Steam Deck legibility pass removed that 14-pixel case).
	constexpr float MenuGap = 6.0f;
	const float AvailableHeight = (LiveViewportHeight - MainMenuListHeight) * 0.5f
		- MenuGap;
	return UpdateBannerEstimatedHeightForFont(LargeBodyFont()) > AvailableHeight;
}

FSlateFontInfo SWoTFrontEndRoot::UpdateBannerBodyFont() const
{
	return ShouldUseCompactUpdateBannerFont() ? PlayBodyFont() : LargeBodyFont();
}

float SWoTFrontEndRoot::UpdateBannerTextWrapWidth() const
{
	return UpdateBannerTextWrapWidthForFont(UpdateBannerBodyFont());
}

float SWoTFrontEndRoot::UpdateBannerBottomMargin() const
{
	constexpr float MenuGap = 6.0f;
	const float Requested = static_cast<float>(
		static_cast<int32>(LiveViewportHeight * 0.08f));
	const float BannerHeight = UpdateBannerWidget.IsValid()
		? UpdateBannerWidget->GetDesiredSize().Y : 0.0f;
	const float SpaceBelowMenu = (LiveViewportHeight - MainMenuListHeight) * 0.5f
		- BannerHeight - MenuGap;
	const float NonOverlapping = static_cast<float>(
		static_cast<int32>(FMath::Max(0.0f, SpaceBelowMenu)));
	return FMath::Min(Requested, NonOverlapping);
}

bool SWoTFrontEndRoot::IsUpdateBannerInteractive() const
{
	const FWoTUpdateBannerPresentation Banner = UpdateBannerPresentation();
	return ViewModel.GetScreen() == EWoTFrontEndScreen::Main
		&& Banner.bVisible && Banner.Action != EWoTUpdateBannerAction::None
		&& !IsAnotherNoticeVisible();
}

TSharedPtr<SWidget> SWoTFrontEndRoot::VisibleModalPrimaryWidget() const
{
	if (bShowUpdateFailedNotice && UpdateFailedContinueWidget.IsValid())
	{
		return UpdateFailedContinueWidget;
	}
	if (bShowReleaseNotesNotice && ReleaseNotesContinueWidget.IsValid())
	{
		return ReleaseNotesContinueWidget;
	}
	if (bShowOnlineDisclosure && OnlineDisclosurePrimaryWidget.IsValid())
	{
		return OnlineDisclosurePrimaryWidget;
	}
	if (bShowLocalNetworkNotice && LocalNetworkNoticePrimaryWidget.IsValid())
	{
		return LocalNetworkNoticePrimaryWidget;
	}
	if (PendingLocalNetworkAction != ELocalNetworkAction::None
		&& LocalNetworkHeadsUpPrimaryWidget.IsValid())
	{
		return LocalNetworkHeadsUpPrimaryWidget;
	}
	if (PendingKeychainAction != EKeychainAction::None
		&& KeychainHeadsUpPrimaryWidget.IsValid())
	{
		return KeychainHeadsUpPrimaryWidget;
	}
	return nullptr;
}

void SWoTFrontEndRoot::RepairUpdateBannerFocus()
{
	if (!FSlateApplication::IsInitialized() || !UpdateBannerButton.IsValid()
		|| FSlateApplication::Get().GetKeyboardFocusedWidget() != UpdateBannerButton
		|| IsUpdateBannerInteractive())
	{
		return;
	}
	TSharedPtr<SWidget> FocusTarget = VisibleModalPrimaryWidget();
	if (!FocusTarget.IsValid() && !MainMenuButtons.IsEmpty())
	{
		FocusTarget = MainMenuButtons.Last();
	}
	if (FocusTarget.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			FocusTarget, EFocusCause::SetDirectly);
	}
}

FReply SWoTFrontEndRoot::ActivateUpdateBanner()
{
	if (!IsPresentationReady() || !IsUpdateBannerInteractive())
	{
		return FReply::Unhandled();
	}
	const FWoTUpdateBannerPresentation Banner = UpdateBannerPresentation();
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	if (WoTFakeUpdateStateFromCommandLine().IsSet())
	{
		const FString Detail = FString::Printf(TEXT("action=%d sequence=%llu"),
			static_cast<int32>(Banner.Action),
			static_cast<unsigned long long>(ResolveUpdateState().ReleaseSequence));
		(void)WoTRecordBootstrapHeartbeat(TEXT("fake-update-action"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, Detail);
		return FReply::Handled();
	}
#endif
	if (Banner.Action == EWoTUpdateBannerAction::Restart)
	{
		UWoTBootstrapSubsystem* Bootstrap = ResolveBootstrap();
		if (Bootstrap == nullptr || !Bootstrap->RequestRestartToUpdate())
		{
			ViewModel.ReportNotice(TEXT(
				"WoT Reborn could not quit. Close it, then open it again to finish installing."),
				/*bError*/ true);
		}
		return FReply::Handled();
	}
	if (Banner.Action == EWoTUpdateBannerAction::OpenReleasePage
		&& WoTIsHttpsReleasePageUrl(Banner.ReleasePageUrl))
	{
		LaunchReleasePage(Banner.ReleasePageUrl);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SWoTFrontEndRoot::LaunchReleasePage(const FString& Url)
{
#if WITH_AUTOMATION_TESTS
	if (ReleasePageLaunchForTesting.IsBound())
	{
		ReleasePageLaunchForTesting.Execute(Url);
		return;
	}
#endif
	FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildSettingsPage(EWoTFrontEndScreen Screen)
{
	// Same framed chrome as the browser; a missing frame degrades to no border.
	PresentationAuthority->PrepareBrowserResources();
	ActiveSettingsScreen = SNew(SWoTSettingsScreen)
		.Page(Screen == EWoTFrontEndScreen::Hardware
			? EWoTSettingsPage::Hardware : EWoTSettingsPage::Controls)
		.Preferences(ResolvePreferences())
		.PresentationAuthority(PresentationAuthority)
		.HeadingFont(this, &SWoTFrontEndRoot::PlayHeadingFont)
		.BodyFont(this, &SWoTFrontEndRoot::LargeBodyFont)
		.TextResolver(FWoTSettingsTextResolver::CreateLambda(
			[this](FName Key) { return RetailText(Key); }))
		.OnBack(FSimpleDelegate::CreateSP(this, &SWoTFrontEndRoot::CloseSettingsPage))
		.OnSaved(FSimpleDelegate::CreateSP(this, &SWoTFrontEndRoot::CloseSettingsPage));
	FirstFocusableWidget = ActiveSettingsScreen;
	// Controller mode starts on the page's first control (spec r3 section 1a).
	ControllerFocusWidget = ActiveSettingsScreen->InitialControllerFocusWidget();
	return ActiveSettingsScreen.ToSharedRef();
}

FReply SWoTFrontEndRoot::Quit()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	if (AWoTFrontEndPlayerController* Controller = PlayerController.Get())
	{
		UKismetSystemLibrary::QuitGame(
			Controller, Controller, EQuitPreference::Quit, /*bIgnoreRestrictions*/ false);
	}
	return FReply::Handled();
}

void SWoTFrontEndRoot::SelectMainMenuEntry(int32 EntryIndex)
{
	if (IsPresentationReady() && MainMenuSelection.IsValid())
	{
		MainMenuSelection->MoveTo(EntryIndex);
	}
}

FReply SWoTFrontEndRoot::ActivateMainMenuEntry(int32 EntryIndex)
{
	if (bShowReleaseNotesNotice || bShowUpdateFailedNotice
		|| !IsPresentationReady() || !MainMenuSelection.IsValid()
		|| !RetailMainEntries().IsValidIndex(EntryIndex))
	{
		return FReply::Unhandled();
	}
	const EWoTFrontEndMainMenuAction Action =
		RetailMainEntries()[EntryIndex].Action;
	const bool bActivated = MainMenuSelection->Activate(
		EntryIndex, [this, Action]()
		{
			if (!IsPresentationReady()) return;
#if WITH_AUTOMATION_TESTS
			ActionObserverForTesting.ExecuteIfBound(Action);
#endif
			switch (Action)
			{
			case EWoTFrontEndMainMenuAction::Multiplayer:
				OpenPlay();
				break;
			case EWoTFrontEndMainMenuAction::Controls:
				OpenControls();
				break;
			case EWoTFrontEndMainMenuAction::Hardware:
				OpenHardware();
				break;
			case EWoTFrontEndMainMenuAction::Quit:
				Quit();
				break;
			}
		});
	return bActivated ? FReply::Handled() : FReply::Unhandled();
}

FReply SWoTFrontEndRoot::SavePlayerConfig()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	const FWoTPlayerConfig Saved = ViewModel.SavePlayerConfig();
	if (UWoTGameInstance* Instance = GameInstance.Get())
	{
		Instance->SetSelectedFaction(Saved.Faction);
		Instance->SetSelectedSkinColor(Saved.SkinColor);
		Instance->SetPlayerName(Saved.PlayerName);
	}
	RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::CancelPlayerConfig()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	ViewModel.CancelPlayerConfig();
	RebuildScreen();
	bRestoreFocusOnNextTick = true;
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(
		this, &SWoTFrontEndRoot::RestoreFocusAfterBack));
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::BackFromPlay()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	// A notice belongs to the Play screen action that raised it.
	ClearPendingNotices();
	if (ViewModel.BackToMain())
	{
		RebuildScreen();
		bRestoreFocusOnNextTick = true;
		RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(
			this, &SWoTFrontEndRoot::RestoreFocusAfterBack));
	}
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::Host()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	if (!ViewModel.GetHostRequest().bLAN
		&& ShowKeychainHeadsUpIfNeeded(EKeychainAction::Host))
	{
		return FReply::Handled();
	}
	// Explain the OS prompt before the probe below can trigger it.
	if (ViewModel.GetHostRequest().bLAN
		&& ShowLocalNetworkHeadsUpIfNeeded(ELocalNetworkAction::Host))
	{
		return FReply::Handled();
	}
	if (!FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance.Get()))
	{
		return FReply::Handled();
	}
	// A LAN host the OS will not let anyone reach is no host at all, and the
	// travel would tear the notice down with the menu: stop here instead.
	if (ViewModel.GetHostRequest().bLAN
		&& ShowLocalNetworkNoticeIfDenied(TEXT("host")))
	{
		return FReply::Handled();
	}
	if (!ViewModel.CanHost())
	{
		return FReply::Handled();
	}
	CutSearchShort();
	if (!ViewModel.BeginHost())
	{
		return FReply::Handled();
	}
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	const bool bAccepted = Sessions && Sessions->Host(ViewModel.GetHostRequest());
	if (!bAccepted
		&& ViewModel.GetOperation() == EWoTFrontEndAsyncOperation::Hosting)
	{
		// Refused before any async work: keep the gate readout durably and
		// on screen, since the guarded client compiles logging out.
		const FString Detail = Sessions
			? Sessions->HostDiagnostics() : FString(TEXT("no session subsystem"));
		WoTRecordBootstrapHeartbeat(TEXT("host-refused"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, Detail.Left(512));
		ViewModel.FinishHost(false, InternetFailureText());
	}
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::RefreshSessions()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	if (!ViewModel.GetHostRequest().bLAN
		&& ShowKeychainHeadsUpIfNeeded(EKeychainAction::Refresh))
	{
		return FReply::Handled();
	}
	if (ViewModel.GetHostRequest().bLAN
		&& ShowLocalNetworkHeadsUpIfNeeded(ELocalNetworkAction::Refresh))
	{
		return FReply::Handled();
	}
	TryRefreshSessions();
	return FReply::Handled();
}

bool SWoTFrontEndRoot::TryRefreshSessions()
{
	if (!FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance.Get())
		|| !ViewModel.BeginSearch())
	{
		return false;
	}
	RebuildSessionItems();
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (Sessions && Sessions->IsFindInFlight())
	{
		// The player left Play mid-search and came back: wait for that
		// search to land, then search the network now selected.
		bRestartSearchWhenStaleLands = true;
		return true;
	}
	const bool bAccepted = Sessions && Sessions->Find(ViewModel.GetHostRequest().bLAN);
	if (!bAccepted
		&& ViewModel.GetOperation() == EWoTFrontEndAsyncOperation::Searching)
	{
		ViewModel.FinishSearch(false, TArray<FWoTSessionListEntry>(),
			InternetFailureText());
		RebuildSessionItems();
	}
	return bAccepted;
}

FReply SWoTFrontEndRoot::JoinSelectedSession()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	TryJoinSelectedSession();
	return FReply::Handled();
}

bool SWoTFrontEndRoot::TryJoinSelectedSession()
{
	if (!FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance.Get())
		|| !SelectedSessionItem.IsValid())
	{
		return false;
	}
	if (SelectedSessionItem->bVersionMismatch)
	{
		// The engine's version handshake would refuse the connection.
		ViewModel.ReportNotice(SessionVersionMismatchNotice(*SelectedSessionItem),
			/*bError*/ true);
		return false;
	}
	if (!ViewModel.BeginJoin(SelectedSessionItem->ServerName))
	{
		return false;
	}
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	const bool bAccepted = Sessions && Sessions->Join(*SelectedSessionItem);
	if (!bAccepted
		&& ViewModel.GetOperation() == EWoTFrontEndAsyncOperation::Joining)
	{
		ViewModel.FinishJoin(false);
	}
	return bAccepted;
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
bool SWoTFrontEndRoot::HarnessRefreshBrowser(bool bLan)
{
	if (!IsPresentationReady())
	{
		return false;
	}
	// The harness drives its own Refresh; the player-facing auto-fetch would
	// start a search it then collides with.
	TGuardValue<bool> NoAutoFetch(bAutoFetchSuppressed, true);
	if (ViewModel.GetScreen() != EWoTFrontEndScreen::Play)
	{
		OpenPlay();
	}
	SetLAN(bLan ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
	return TryRefreshSessions();
}

bool SWoTFrontEndRoot::HarnessJoinBrowserRow(int32 Row)
{
	if (!IsPresentationReady() || ViewModel.GetScreen() != EWoTFrontEndScreen::Play
		|| !SessionItems.IsValidIndex(Row) || !SessionItems[Row].IsValid())
	{
		return false;
	}
	// A row click then the Join button: the list selection feeds
	// SelectSession, which is what JoinSelectedSession reads.
	const FSessionItem Item = SessionItems[Row];
	if (SessionListView.IsValid())
	{
		SessionListView->SetSelection(Item, ESelectInfo::Direct);
	}
	SelectSession(Item, ESelectInfo::Direct);
	return TryJoinSelectedSession();
}

TArray<FWoTSessionListEntry> SWoTFrontEndRoot::HarnessBrowserRows() const
{
	TArray<FWoTSessionListEntry> Rows;
	for (const FSessionItem& Item : SessionItems)
	{
		if (Item.IsValid())
		{
			Rows.Add(*Item);
		}
	}
	return Rows;
}
#endif

FReply SWoTFrontEndRoot::ConnectDirect()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	// A direct address may be on this network (the usual LAN fallback).
	if (!DirectAddress.TrimStartAndEnd().IsEmpty()
		&& ShowLocalNetworkHeadsUpIfNeeded(ELocalNetworkAction::ConnectDirect))
	{
		return FReply::Handled();
	}
	if (FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance.Get())
		&& ViewModel.IsIdleOrSearching())
	{
		CutSearchShort();
	}
	if (FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance.Get())
		&& ViewModel.BeginDirectConnect())
	{
		UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
		if (!Sessions || !Sessions->ConnectDirect(DirectAddress))
		{
			ViewModel.RejectDirectConnect();
		}
	}
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::SelectBrowserSortColumn(
	EWoTBrowserSortColumn Column)
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	if (BrowserSort)
	{
		BrowserSort->Select(Column);
		SortSessionItems();
		if (SessionListView.IsValid())
		{
			SessionListView->RequestListRefresh();
		}
	}
	return FReply::Handled();
}

void SWoTFrontEndRoot::RebuildMapItems()
{
	MapItems.Reset();
	SelectedMapItem.Reset();
	const FWoTFrontEndMapEntry* Selected = ViewModel.GetSelectedMap();
	for (const FWoTFrontEndMapEntry& Entry : ViewModel.GetMaps())
	{
		FMapItem Item = MakeShared<FWoTFrontEndMapEntry>(Entry);
		MapItems.Add(Item);
		if (Selected && Entry.PackageName == Selected->PackageName)
		{
			SelectedMapItem = Item;
		}
	}
}

void SWoTFrontEndRoot::RebuildSessionItems()
{
	SessionItems.Reset();
	SelectedSessionItem.Reset();
	for (const FWoTSessionListEntry& Entry : ViewModel.GetSessions())
	{
		SessionItems.Add(MakeShared<FWoTSessionListEntry>(Entry));
	}
	SortSessionItems();
	if (SessionListView.IsValid())
	{
		SessionListView->ClearSelection();
		SessionListView->RequestListRefresh();
	}
}

TSharedRef<SWidget> SWoTFrontEndRoot::GenerateMapWidget(FMapItem Item) const
{
	return SNew(STextBlock).Text(HostMapPickerText(Item.Get()))
		.Font(this, &SWoTFrontEndRoot::LargeBodyFont);
}

void SWoTFrontEndRoot::SelectMap(FMapItem Item, ESelectInfo::Type /*SelectInfo*/)
{
	if (!IsPresentationReady()) return;
	if (Item.IsValid() && ViewModel.SelectMap(Item->PackageName))
	{
		SelectedMapItem = Item;
	}
}

bool SWoTFrontEndRoot::SelectFirstHostMapOfKind(bool bBattle)
{
	for (const FMapItem& Item : MapItems)
	{
		if (Item.IsValid() && Item->bIsBattle == bBattle)
		{
			SelectMap(Item, ESelectInfo::Direct);
			return SelectedMapItem == Item;
		}
	}
	return false;
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
float SWoTFrontEndRoot::HarnessHostPanelScrollable() const
{
	return HostScrollBox.IsValid()
		? StaticCastSharedPtr<SScrollBox>(HostScrollBox)->GetScrollOffsetOfEnd() : -1.0f;
}
#endif

TSharedRef<ITableRow> SWoTFrontEndRoot::GenerateSessionRow(
	FSessionItem Item, const TSharedRef<STableViewBase>& OwnerTable) const
{
	const FWoTRetailBrowserStyle* Browser = PresentationAuthority->BrowserStyle();
	if (!Browser) return SNew(STableRow<FSessionItem>, OwnerTable)[SNullWidget::NullWidget];
	const FWoTRetailBrowserStyle& Style = *Browser;
	auto MakeCell =
		[this, &Style](const FString& Text, ETextJustify::Type Justification)
			-> TSharedRef<SWidget>
	{
		return SNew(SBox).Padding(FMargin(4.0f, 0.0f))
		[
		SNew(STextBlock)
			.Text(WoTFrontEndSlate_Text(Text))
			.Font(this, &SWoTFrontEndRoot::PlayBrowserFont)
			.ColorAndOpacity(Style.CellForeground())
			.Justification(Justification)
			.Clipping(EWidgetClipping::ClipToBounds)
		];
	};

	const FString Server = Item.IsValid() ? SessionServerText(*Item) : FString();
	const FString Ping = Item.IsValid() ? SessionPingText(*Item) : FString();
	const FString Map = Item.IsValid() ? Item->MapName : FString();
	const FString Players = Item.IsValid()
		? FString::Printf(TEXT("%d/%d"), Item->CurrentPlayers, Item->MaxPlayers)
		: FString();
	return SNew(STableRow<FSessionItem>, OwnerTable)
		.Style(Style.TableRowStyle())
	[
		SNew(SBox)
		.MinDesiredHeight(FWoTRetailBrowserStyle::RowHeight)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.40f)
			[
				MakeCell(Server, ETextJustify::Left)
			]
			+ SHorizontalBox::Slot().FillWidth(0.15f)
			[
				MakeCell(Ping, ETextJustify::Right)
			]
			+ SHorizontalBox::Slot().FillWidth(0.25f)
			[
				MakeCell(Map, ETextJustify::Left)
			]
			+ SHorizontalBox::Slot().FillWidth(0.20f)
			[
				MakeCell(Players, ETextJustify::Right)
			]
		]
	];
}

void SWoTFrontEndRoot::SelectSession(
	FSessionItem Item, ESelectInfo::Type /*SelectInfo*/)
{
	if (!IsPresentationReady()) return;
	SelectedSessionItem = Item;
}

void SWoTFrontEndRoot::DoubleClickSession(FSessionItem Item)
{
	if (!IsPresentationReady() || !Item.IsValid())
	{
		return;
	}
	SelectedSessionItem = Item;
	if (SessionListView.IsValid())
	{
		SessionListView->SetSelection(Item, ESelectInfo::Direct);
	}
	JoinSelectedSession();
}

void SWoTFrontEndRoot::SortSessionItems()
{
	if (!BrowserSort)
	{
		return;
	}

	const FWoTBrowserSortState Sort = *BrowserSort;
	SessionItems.StableSort(
		[Sort](const FSessionItem& A, const FSessionItem& B)
		{
			if (!A.IsValid() || !B.IsValid())
			{
				return A.IsValid() && !B.IsValid();
			}

			// UBrowserServerGrid.uc:344-439 keeps unpinged entries at the tail
			// for every non-Ping comparator, even in descending order.
			if (Sort.Column != EWoTBrowserSortColumn::Ping)
			{
				const bool bAUnpinged = A->PingMs == 9999;
				const bool bBUnpinged = B->PingMs == 9999;
				if (bAUnpinged != bBUnpinged)
				{
					return !bAUnpinged;
				}
			}

			int32 Result = 0;
			switch (Sort.Column)
			{
			case EWoTBrowserSortColumn::Server:
				Result = FWoTBrowserSortState::CompareText(
					A->ServerName, B->ServerName);
				break;
			case EWoTBrowserSortColumn::Ping:
				Result = A->PingMs < B->PingMs ? -1
					: (A->PingMs > B->PingMs ? 1 : 0);
				break;
			case EWoTBrowserSortColumn::Map:
				Result = FWoTBrowserSortState::CompareText(
					A->MapName, B->MapName);
				if (Result == 0)
				{
					Result = A->PingMs - B->PingMs;
				}
				break;
			case EWoTBrowserSortColumn::Players:
				// Retail's non-descending Players comparator is fullest first.
				Result = A->CurrentPlayers > B->CurrentPlayers ? -1
					: (A->CurrentPlayers < B->CurrentPlayers ? 1 : 0);
				if (Result == 0)
				{
					Result = A->MaxPlayers > B->MaxPlayers ? -1
						: (A->MaxPlayers < B->MaxPlayers ? 1 : 0);
				}
				if (Result == 0)
				{
					Result = A->PingMs - B->PingMs;
				}
				break;
			}
			return (Sort.bDescending ? -Result : Result) < 0;
		});
}

TSharedRef<SWidget> SWoTFrontEndRoot::TextField(const TSharedRef<SWidget>& Field,
	const FWoTTextEntryRequest& Request)
{
	return SNew(SWoTTextFieldActivator)
		.Request(Request)
		[
			Field
		];
}

FWoTTextValidation SWoTFrontEndRoot::ValidateHostPassword(const FString& Text)
{
	FString Sanitized;
	if (FWoTFrontEndModel::SanitizeHostPassword(Text, Sanitized))
	{
		return FWoTTextValidation::Accept();
	}
	// SanitizeHostPassword rejects rather than trims, so the message names
	// the rule (spec r3 section 3).
	return FWoTTextValidation::Reject(WoTFrontEndSlate_Text(
		TEXT("Passwords are up to 32 characters: letters, digits and symbols, ")
		TEXT("with no spaces and none of ? # = \" \\")));
}

FWoTTextEntryRequest SWoTFrontEndRoot::PlayerNameEntryRequest()
{
	FWoTTextEntryRequest Request;
	Request.Title = RetailText(TEXT("menuPlayer.MenuList[1]"));
	Request.MaxLength = 20;
	Request.GetField = [this]() { return StaticCastSharedPtr<SWidget>(PlayerNameTextBox); };
	Request.GetText = [this]() { return ViewModel.GetPendingPlayerName(); };
	Request.SetText = [this](const FString& Text)
	{
		ViewModel.SelectPendingPlayerName(Text);
		// The box's own commit (CommitPlayerName) sets its text directly, which
		// ends its binding, so the entry sets it the same way.
		if (PlayerNameTextBox.IsValid())
		{
			PlayerNameTextBox->SetText(WoTFrontEndSlate_Text(ViewModel.GetPendingPlayerName()));
		}
	};
	Request.Filter = [](const FString& Text)
	{
		return FWoTFrontEndModel::FilterPlayerNameInput(Text);
	};
	Request.Commit = [this]()
	{
		CommitPlayerName(WoTFrontEndSlate_Text(ViewModel.GetPendingPlayerName()),
			ETextCommit::OnEnter);
	};
	return Request;
}

FWoTTextEntryRequest SWoTFrontEndRoot::ServerPasswordEntryRequest()
{
	FWoTTextEntryRequest Request;
	Request.Title = WoTFrontEndSlate_Text(TEXT("Server Password"));
	Request.MaxLength = 32;
	Request.bPassword = true;
	Request.GetField = [this]() { return StaticCastSharedPtr<SWidget>(PasswordTextBox); };
	Request.GetText = [this]()
	{
		return PasswordTextBox.IsValid() ? PasswordTextBox->GetText().ToString() : FString();
	};
	Request.SetText = [this](const FString& Text)
	{
		if (PasswordTextBox.IsValid())
		{
			PasswordTextBox->SetText(WoTFrontEndSlate_Text(Text));
		}
	};
	Request.Validate = &SWoTFrontEndRoot::ValidateHostPassword;
	Request.Commit = [this]() { SubmitServerPassword(); };
	return Request;
}

FWoTTextEntryRequest SWoTFrontEndRoot::ServerNameEntryRequest()
{
	FWoTTextEntryRequest Request;
	Request.Title = RetailText(TEXT("menuStartArenaServer.MenuList[7]"));
	// New for controller entry (spec r3 [M6]); the keyboard's box is unbounded.
	Request.MaxLength = 32;
	Request.GetField = [this]() { return StaticCastSharedPtr<SWidget>(ServerNameTextBox); };
	Request.GetText = [this]() { return ViewModel.GetHostRequest().ServerName; };
	Request.SetText = [this](const FString& Text) { SetServerName(WoTFrontEndSlate_Text(Text)); };
	return Request;
}

FWoTTextEntryRequest SWoTFrontEndRoot::HostPasswordEntryRequest(bool bAdmin)
{
	FWoTTextEntryRequest Request;
	Request.Title = TAttribute<FText>::CreateLambda([this, bAdmin]()
	{
		const bool bBattle = ViewModel.IsBattleMapSelected();
		return RetailText(bAdmin
			? (bBattle ? FName(TEXT("menuStartBattleServer.MenuList[13]"))
				: FName(TEXT("menuStartArenaServer.MenuList[8]")))
			: (bBattle ? FName(TEXT("menuStartBattleServer.MenuList[14]"))
				: FName(TEXT("menuStartArenaServer.MenuList[9]"))));
	});
	Request.MaxLength = 32;
	Request.bPassword = true;
	Request.GetField = [this, bAdmin]()
	{
		return StaticCastSharedPtr<SWidget>(bAdmin ? AdminPasswordTextBox : GamePasswordTextBox);
	};
	Request.GetText = [this, bAdmin]()
	{
		return bAdmin ? ViewModel.GetHostRequest().AdminPassword
			: ViewModel.GetHostRequest().GamePassword;
	};
	Request.SetText = [this, bAdmin](const FString& Text)
	{
		if (bAdmin)
		{
			ViewModel.SetAdminPassword(Text);
		}
		else
		{
			ViewModel.SetGamePassword(Text);
		}
	};
	Request.Validate = &SWoTFrontEndRoot::ValidateHostPassword;
	return Request;
}

FWoTTextEntryRequest SWoTFrontEndRoot::DirectAddressEntryRequest()
{
	FWoTTextEntryRequest Request;
	Request.Title = WoTFrontEndSlate_Text(TEXT("Direct Address"));
	// New for controller entry (spec r3 [M6]); the keyboard's box is unbounded.
	Request.MaxLength = 64;
	Request.GetField = [this]() { return StaticCastSharedPtr<SWidget>(DirectAddressTextBox); };
	Request.GetText = [this]() { return DirectAddress; };
	Request.SetText = [this](const FString& Text) { SetDirectAddress(WoTFrontEndSlate_Text(Text)); };
	// Done connects, as the Connect button beside the box does.
	Request.Commit = [this]() { ConnectDirect(); };
	return Request;
}

void SWoTFrontEndRoot::SetServerName(const FText& Text)
{
	if (!IsPresentationReady()) return;
	ViewModel.SetServerName(Text.ToString());
}

void SWoTFrontEndRoot::SetPlayerName(const FText& Text)
{
	if (!IsPresentationReady()) return;
	// Filter only the pending value here. SetText from inside OnTextChanged would
	// re-enter this callback and jump the caret to the end; the box caps length
	// itself (MaximumLength) and CommitPlayerName shows the sanitized text.
	ViewModel.SelectPendingPlayerName(Text.ToString());
}

void SWoTFrontEndRoot::CommitPlayerName(
	const FText& Text, ETextCommit::Type CommitType)
{
	(void)CommitType;
	if (!IsPresentationReady()) return;
	const FString Sanitized =
		FWoTFrontEndModel::SanitizePlayerName(Text.ToString());
	ViewModel.SelectPendingPlayerName(Sanitized);
	if (PlayerNameTextBox.IsValid())
	{
		PlayerNameTextBox->SetText(WoTFrontEndSlate_Text(Sanitized));
	}
}

void SWoTFrontEndRoot::SetDirectAddress(const FText& Text)
{
	if (!IsPresentationReady()) return;
	DirectAddress = Text.ToString();
}

void SWoTFrontEndRoot::SetFragLimit(int32 Value)
{
	if (!IsPresentationReady()) return;
	ViewModel.SetFragLimit(Value);
}

void SWoTFrontEndRoot::SetTimeLimit(int32 Value)
{
	if (!IsPresentationReady()) return;
	ViewModel.SetTimeLimit(Value);
}

void SWoTFrontEndRoot::SetSealGoal(int32 Value)
{
	if (!IsPresentationReady()) return;
	ViewModel.SetSealGoal(Value);
}

void SWoTFrontEndRoot::SetSealBudget(int32 Value)
{
	if (!IsPresentationReady()) return;
	ViewModel.SetSealBudget(Value);
}

void SWoTFrontEndRoot::SetMaxPlayers(int32 Value)
{
	if (!IsPresentationReady()) return;
	ViewModel.SetMaxPlayers(Value);
}

void SWoTFrontEndRoot::SetLAN(ECheckBoxState State)
{
	if (!IsPresentationReady()) return;
	SetNetworkMode(State != ECheckBoxState::Checked);
}

void SWoTFrontEndRoot::SetNetworkMode(bool bInternet)
{
	if (!IsPresentationReady() || !ViewModel.CanLeaveOrSwitchNetwork()) return;
	const bool bWasInternet = ViewModel.IsInternet();
	if (!bInternet)
	{
		bShowOnlineDisclosure = false;
		if (bWasInternet)
		{
			// Switching cuts the other network's search short; its late result
			// is dropped and this network is searched once it lands.
			ViewModel.AbandonSearch();
		}
		ViewModel.SetLAN(true);
		if (bWasInternet)
		{
			AutoFetchSessions();
		}
		return;
	}
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (Sessions == nullptr)
	{
		return;
	}
	const FWoTInternetStatus Status = Sessions->GetInternetStatus();
	if (Status.State == EWoTInternetState::Unavailable)
	{
		ViewModel.SetLAN(true);
		ViewModel.ReportNotice(Status.UserText(), /*bError*/ true);
		return;
	}
	if (!Sessions->IsOnlineIdentityAcknowledged())
	{
		// Nothing reaches Epic until the player accepts the notice.
		bShowOnlineDisclosure = true;
		return;
	}
	if (!bWasInternet)
	{
		ViewModel.AbandonSearch();
	}
	ViewModel.SetLAN(false);
	if (!bWasInternet)
	{
		AutoFetchSessions();
	}
}

void SWoTFrontEndRoot::CutSearchShort()
{
	// Play lists the network on entry. Host and Direct Connect must not wait
	// out that search (otherwise Host stays disabled for seconds after
	// entering Multiplayer), so they drop it, and the subsystem cancels the
	// provider's Find: a LAN host cannot advertise while the search holds the
	// LAN beacon.
	const bool bSearching =
		ViewModel.GetOperation() == EWoTFrontEndAsyncOperation::Searching;
	ViewModel.AbandonSearch();
	bRestartSearchWhenStaleLands = false;
	if (UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
		Sessions && Sessions->IsFindInFlight())
	{
		Sessions->CancelFind();
	}
	if (bSearching)
	{
		RebuildSessionItems();
	}
}

void SWoTFrontEndRoot::AutoFetchSessions()
{
	// Entering Play or switching LAN/Internet lists that network at once.
	// Through the Refresh button's path, so its heads-ups still explain any
	// macOS prompt before the search can raise it.
	if (bAutoFetchSuppressed)
	{
		return;
	}
	RefreshSessions();
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildNetworkModeRow(bool bBrowser,
	TArray<TSharedPtr<SWidget>>* OutNavigationRow)
{
	(void)bBrowser;
	const auto Font = &SWoTFrontEndRoot::LargeBodyFont;
	TSharedPtr<SWidget> LanRadio;
	TSharedPtr<SWidget> InternetRadio;
	const TSharedRef<SWidget> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 14.0f, 0.0f)
		[
			SAssignNew(LanRadio, SCheckBox)
			.Style(&PlayCheckBoxStyle)
			.IsChecked(this, &SWoTFrontEndRoot::LANState)
			.IsEnabled_Lambda([this]() { return ViewModel.CanLeaveOrSwitchNetwork(); })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState)
			{
				SetNetworkMode(/*bInternet*/ false);
			})
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("LAN")))
				.Font(this, Font)
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SAssignNew(InternetRadio, SCheckBox)
			.Style(&PlayCheckBoxStyle)
			.IsChecked(this, &SWoTFrontEndRoot::InternetCheckState)
			.IsEnabled(this, &SWoTFrontEndRoot::CanChooseInternet)
			.OnCheckStateChanged_Lambda([this](ECheckBoxState)
			{
				SetNetworkMode(/*bInternet*/ true);
			})
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(TEXT("Internet")))
				.Font(this, Font)
			]
		];
	if (OutNavigationRow != nullptr)
	{
		OutNavigationRow->Add(LanRadio);
		OutNavigationRow->Add(InternetRadio);
	}
	return Row;
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildInternetOptions(bool bBrowser,
	TArray<TSharedPtr<SWidget>>* OutNavigationRow)
{
	TSharedPtr<SWidget> RelayCheckBox;
	TSharedRef<SVerticalBox> Options = SNew(SVerticalBox);
	Options->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
	[
		SAssignNew(InternetStatusTextWidget, STextBlock)
		.Text(this, &SWoTFrontEndRoot::InternetStatusText)
		.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
		.AutoWrapText(false)
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
	];
	Options->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, bBrowser ? 0.0f : 4.0f)
	[
		SAssignNew(RelayCheckBox, SCheckBox)
		.Style(&PlayCheckBoxStyle)
		.IsChecked(this, &SWoTFrontEndRoot::AlwaysUseEpicRelayState)
		.IsEnabled_Lambda([this]() { return ViewModel.IsIdleOrSearching(); })
		.OnCheckStateChanged(this, &SWoTFrontEndRoot::SetAlwaysUseEpicRelay)
		[
			SNew(STextBlock)
			.Text(WoTFrontEndSlate_Text(TEXT(
				"Always use Epic's relay - keeps your IP address hidden from other players, "
				"but may add some lag.")))
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.AutoWrapText(true)
			.WrapTextAt_Lambda([this, bBrowser]()
			{
				// Keep a stable physical wrap width across window DPI scales.
				return (bBrowser ? 500.0f : 620.0f)
					/ FMath::Max(LiveViewportScale, 0.01f);
			})
		]
	];
	if (OutNavigationRow != nullptr)
	{
		OutNavigationRow->Add(RelayCheckBox);
	}
	return Options;
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildOnlineDisclosure()
{
	const FWoTRetailBrowserStyle* Browser = PresentationAuthority->BrowserStyle();
	if (!Browser) return SNullWidget::NullWidget;
	const FWoTRetailBrowserStyle& Style = *Browser;
	// Not the browser WindowFrame brush: the two framed Play panels are the
	// only window frames (WoT.FrontEnd.MultiplayerGeometry).
	const TSharedRef<SWidget> Disclosure = SAssignNew(OnlineDisclosureWidget, SBox)
		.MaxDesiredWidth_Lambda([this]()
		{
			// Match the menu's inverse-DPI font sizing: keep the notice near 1200
			// physical pixels wide so the 2x body copy also fits at 1280x720.
			return 1200.0f / FMath::Max(LiveViewportScale, 0.01f);
		})
		.Visibility_Lambda([this]()
		{
			return bShowOnlineDisclosure
				? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			// Gold browser frame around the notice face, like the Play panels:
			// a borderless face was hard to read against the screen behind it.
			SNew(SBorder)
			.BorderImage(Style.WindowFrame())
			.Padding(FMargin(1.0f)) // the game skin's 1px gold outline
			[
				SNew(SBorder)
				.BorderImage(Style.FaceBackground())
				.Padding(FMargin(18.0f, 14.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(STextBlock)
						.Text(WoTFrontEndSlate_Text(
							TEXT("Internet play uses EOS. Nothing is sent until you accept.")))
						.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
						.ColorAndOpacity(Style.BodyForeground())
						.AutoWrapText(true)
						.WrapTextAt_Lambda([this]()
						{
							return 1164.0f / FMath::Max(LiveViewportScale, 0.01f);
						})
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 18.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(WoTFrontEndSlate_Text(TEXT(
								"To list and join internet games, WoT sends Epic an anonymous ID, "
								"the name \"WoT Player\", your IP address, and the hosted server "
								"and map. No Epic account is needed.")))
							.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
							.ColorAndOpacity(Style.BodyForeground())
							.AutoWrapText(true)
							.WrapTextAt_Lambda([this]()
							{
								return 555.0f / FMath::Max(LiveViewportScale, 0.01f);
							})
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(STextBlock)
							.Text(WoTFrontEndSlate_Text(TEXT(
								"Always use Epic's relay - keeps your IP address hidden from other "
								"players, but may add some lag.\n\n"
								"Direct peers may see your IP. LAN and direct IP never use Epic. "
								"Reset online identity deletes the ID.")))
							.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
							.ColorAndOpacity(Style.BodyForeground())
							.AutoWrapText(true)
							.WrapTextAt_Lambda([this]()
							{
								return 555.0f / FMath::Max(LiveViewportScale, 0.01f);
							})
						]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SAssignNew(OnlineDisclosurePrimaryWidget, SButton)
							.ButtonStyle(Style.ButtonStyle())
							.ContentPadding(FMargin(14.0f, 4.0f))
							.OnClicked(this, &SWoTFrontEndRoot::AcceptOnlineDisclosure)
							[
								SNew(STextBlock)
								.Text(WoTFrontEndSlate_Text(TEXT("Accept")))
								.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
								.ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SAssignNew(OnlineDisclosureCancelWidget, SButton)
							.ButtonStyle(Style.ButtonStyle())
							.ContentPadding(FMargin(14.0f, 4.0f))
							.OnClicked(this, &SWoTFrontEndRoot::CancelOnlineDisclosure)
							[
								SNew(STextBlock)
								.Text(WoTFrontEndSlate_Text(TEXT("Cancel")))
								.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
								.ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
					]
				]
			]
		];
	// An overlay's buttons keep focus until it closes (spec r3 [I6]).
	const TArray<TSharedPtr<SWidget>> Buttons = {
		OnlineDisclosurePrimaryWidget, OnlineDisclosureCancelWidget};
	WoTWrapHorizontalNavigation(Buttons, /*bHoldVertical*/ true);
	return Disclosure;
}

bool SWoTFrontEndRoot::ShowLocalNetworkNoticeIfDenied(const TCHAR* Trigger)
{
	FString Detail;
	if (WoTLocalNetworkAccess::Probe(&Detail) != EWoTLocalNetworkAccess::Denied)
	{
		return false;
	}
	WoTRecordBootstrapHeartbeat(TEXT("local-network-denied"),
		EWoTBootstrapPhase::Ready, 0, {}, {},
		FString::Printf(TEXT("trigger=%s %s"), Trigger, *Detail).Left(512));
	bShowLocalNetworkNotice = true;
	ViewModel.ReportNotice(TEXT("Local network access is off for WoT Reborn."),
		/*bError*/ true);
	return true;
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildNoticePanel(
	TSharedPtr<SWidget>& OutWidget, TFunction<bool()> IsVisible,
	const TCHAR* Heading, const TCHAR* Body, const TCHAR* PrimaryLabel,
	FOnClicked OnPrimary, const TCHAR* SecondaryLabel, FOnClicked OnSecondary,
	bool bScrollableBody, TSharedPtr<SWidget>* OutPrimaryWidget)
{
	const FWoTRetailBrowserStyle* Browser = PresentationAuthority->BrowserStyle();
	if (!Browser) return SNullWidget::NullWidget;
	const FWoTRetailBrowserStyle& Style = *Browser;
	const auto Paragraph = [this, &Style](const TCHAR* Text)
	{
		return SNew(STextBlock)
			.Text(WoTFrontEndSlate_Text(Text))
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.ColorAndOpacity(Style.BodyForeground())
			.AutoWrapText(true)
			.WrapTextAt_Lambda([this]()
			{
				return 1164.0f / FMath::Max(LiveViewportScale, 0.01f);
			});
	};
	const auto Button = [this, &Style](const TCHAR* Label, FOnClicked OnClicked)
	{
		return SNew(SButton)
			.ButtonStyle(Style.ButtonStyle())
			.ContentPadding(FMargin(14.0f, 4.0f))
			.OnClicked(OnClicked)
			[
				SNew(STextBlock)
				.Text(WoTFrontEndSlate_Text(Label))
				.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	};
	TSharedPtr<SButton> PrimaryButton;
	TSharedPtr<SWidget> SecondaryButton;
	TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
	Buttons->AddSlot().AutoWidth()
	[
		SAssignNew(PrimaryButton, SButton)
		.ButtonStyle(Style.ButtonStyle())
		.ContentPadding(FMargin(14.0f, 4.0f))
		.OnClicked(OnPrimary)
		[
			SNew(STextBlock)
			.Text(WoTFrontEndSlate_Text(PrimaryLabel))
			.Font(this, &SWoTFrontEndRoot::LargeBodyFont)
			.ColorAndOpacity(FSlateColor::UseForeground())
		]
	];
	if (SecondaryLabel != nullptr && SecondaryLabel[0] != TEXT('\0')
		&& OnSecondary.IsBound())
	{
		SecondaryButton = Button(SecondaryLabel, OnSecondary);
		Buttons->AddSlot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SecondaryButton.ToSharedRef()
		];
	}
	if (OutPrimaryWidget != nullptr)
	{
		*OutPrimaryWidget = PrimaryButton;
	}
	// A notice's buttons keep focus until it closes (spec r3 section 1a).
	const TArray<TSharedPtr<SWidget>> NoticeButtons = {PrimaryButton, SecondaryButton};
	WoTWrapHorizontalNavigation(NoticeButtons, /*bHoldVertical*/ true);
	TSharedRef<SWidget> BodyWidget = Paragraph(Body);
	if (bScrollableBody)
	{
		BodyWidget = SNew(SBox)
			.MaxDesiredHeight_Lambda([this]()
			{
				return FMath::Max(120.0f, LiveViewportHeight * 0.5f);
			})
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()[Paragraph(Body)]
			];
	}
	// Same face, width rule and 2x body copy as the online-identity notice.
	return SAssignNew(OutWidget, SBox)
		.MaxDesiredWidth_Lambda([this]()
		{
			return 1200.0f / FMath::Max(LiveViewportScale, 0.01f);
		})
		.Visibility_Lambda([IsVisible = MoveTemp(IsVisible)]()
		{
			return IsVisible() ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			// Gold browser frame around the notice face, like the Play panels:
			// a borderless face was hard to read against the screen behind it.
			SNew(SBorder)
			.BorderImage(Style.WindowFrame())
			.Padding(FMargin(1.0f)) // the game skin's 1px gold outline
			[
				SNew(SBorder)
				.BorderImage(Style.FaceBackground())
				.Padding(FMargin(18.0f, 14.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						Paragraph(Heading)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
					[
						BodyWidget
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						Buttons
					]
				]
			]
		];
}

bool SWoTFrontEndRoot::IsAnotherNoticeVisible() const
{
	return bShowOnlineDisclosure || bShowLocalNetworkNotice
		|| bShowUpdateFailedNotice || bShowReleaseNotesNotice
		|| PendingLocalNetworkAction != ELocalNetworkAction::None
		|| PendingKeychainAction != EKeychainAction::None;
}

void SWoTFrontEndRoot::ShowUpdateFailedIfNeeded()
{
	if (bShowUpdateFailedNotice || IsAnotherNoticeVisible()) return;
	uint64 Failed = 0;
	uint64 Restored = 0;
	const UWoTPreferencesSubsystem* Preferences = ResolvePreferences();
	if (Preferences == nullptr
		|| !Preferences->GetPendingUpdateFailedNotice(Failed, Restored))
	{
		return;
	}
	if (Failed == DismissedFailedReleaseSequenceThisRun) return;
	if (!PresentationAuthority.IsValid()
		|| !PresentationAuthority->PrepareBrowserResources())
	{
		return;
	}
	ActiveFailedReleaseSequence = Failed;
	ActiveRestoredReleaseSequence = Restored;
	// The menu shows the running human version, so the notice names that one;
	// the failed release's own version was never installed here.
	const WoTReleaseIdentity::FIdentity Running = WoTReleaseIdentity::LoadRunning();
	ActiveUpdateFailedText = !RunningHumanVersion.IsEmpty() && Restored != 0
			&& Running.bStamped && Running.ReleaseSequence == Restored
		? FString::Printf(
			TEXT("The update could not start on this computer, so you are back on version %s."),
			*RunningHumanVersion)
		: Restored == 0
		? FString::Printf(
			TEXT("The update to version %llu could not start on this computer, so you are back on the previous version."),
			Failed)
		: FString::Printf(
			TEXT("The update to version %llu could not start on this computer, so you are back on version %llu."),
			Failed, Restored);
	bShowUpdateFailedNotice = true;
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildUpdateFailedNotice()
{
	return BuildNoticePanel(UpdateFailedNoticeWidget,
		[this]() { return bShowUpdateFailedNotice; },
		TEXT("Update could not start"), *ActiveUpdateFailedText,
		TEXT("Continue"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::ContinueUpdateFailed),
		nullptr, FOnClicked(), false, &UpdateFailedContinueWidget);
}

FReply SWoTFrontEndRoot::ContinueUpdateFailed()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	bShowUpdateFailedNotice = false;
	DismissedFailedReleaseSequenceThisRun = ActiveFailedReleaseSequence;
	if (UWoTPreferencesSubsystem* Preferences = ResolvePreferences())
	{
		(void)Preferences->SetUpdateFailedNoticeSeen(
			ActiveFailedReleaseSequence);
	}
	ShowReleaseNotesIfNeeded();
	RebuildScreen();
	return FReply::Handled();
}

void SWoTFrontEndRoot::ShowReleaseNotesIfNeeded()
{
	if (ViewModel.GetScreen() != EWoTFrontEndScreen::Main
		|| ViewModel.GetOperation() != EWoTFrontEndAsyncOperation::None
		|| bShowReleaseNotesNotice || IsAnotherNoticeVisible())
	{
		return;
	}
	const UWoTBootstrapSubsystem* Bootstrap = ResolveBootstrap();
	if (Bootstrap == nullptr || !Bootstrap->ReleaseNotesResult().IsSet())
	{
		return;
	}
	const FWoTReleaseNotesResult& Result =
		Bootstrap->ReleaseNotesResult().GetValue();
	if (Result.Outcome != EWoTReleaseNotesOutcome::Selected)
	{
		return;
	}
	const FString& Key = Result.ReleaseCommit;
	if (Key.IsEmpty() || Key == DismissedReleaseNotesKeyThisRun)
	{
		return;
	}
	if (const UWoTPreferencesSubsystem* Preferences = ResolvePreferences())
	{
		if (Preferences->HasSeenReleaseNotes(Key))
		{
			return;
		}
	}
	if (!PresentationAuthority.IsValid()
		|| !PresentationAuthority->PrepareBrowserResources())
	{
		return;
	}
	ActiveReleaseNotesKey = Key;
	ActiveReleaseNotesText = Result.Text;
	bShowReleaseNotesNotice = true;
}

void SWoTFrontEndRoot::HandleReleaseNotesCompleted(
	const FWoTReleaseNotesResult&)
{
	check(IsInGameThread());
	ShowReleaseNotesIfNeeded();
	if (bShowReleaseNotesNotice
		&& ViewModel.GetScreen() == EWoTFrontEndScreen::Main)
	{
		RebuildScreen();
	}
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildReleaseNotesNotice()
{
	return BuildNoticePanel(ReleaseNotesNoticeWidget,
		[this]() { return bShowReleaseNotesNotice; },
		TEXT("What's new in this version"), *ActiveReleaseNotesText,
		TEXT("Continue"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::ContinueReleaseNotes),
		nullptr, FOnClicked(), /*bScrollableBody*/ true,
		&ReleaseNotesContinueWidget);
}

FReply SWoTFrontEndRoot::ContinueReleaseNotes()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	const FString Key = ActiveReleaseNotesKey;
	bShowReleaseNotesNotice = false;
	DismissedReleaseNotesKeyThisRun = Key;
	if (UWoTPreferencesSubsystem* Preferences = ResolvePreferences())
	{
		Preferences->SetReleaseNotesSeen(Key);
	}
	if (MainMenuSelection.IsValid()
		&& MainMenuButtons.IsValidIndex(MainMenuSelection->GetSelectedIndex()))
	{
		FirstFocusableWidget = MainMenuButtons[MainMenuSelection->GetSelectedIndex()];
	}
	bRestoreFocusOnNextTick = true;
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(
		this, &SWoTFrontEndRoot::RestoreFocusAfterBack));
	return FReply::Handled();
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildLocalNetworkNotice()
{
	return BuildNoticePanel(LocalNetworkNoticeWidget,
		[this]() { return bShowLocalNetworkNotice; },
		TEXT("Local network access is off for WoT Reborn."),
		TEXT("macOS is blocking WoT from finding or hosting games on this "
			"network. Open System Settings > Privacy & Security > Local "
			"Network, turn on WoT Reborn, then try again. If games still do not "
			"appear, restart WoT."),
		TEXT("Open Settings"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::OpenLocalNetworkSettings),
		TEXT("OK"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::DismissLocalNetworkNotice),
		false, &LocalNetworkNoticePrimaryWidget);
}

bool SWoTFrontEndRoot::ShowLocalNetworkHeadsUpIfNeeded(ELocalNetworkAction Action)
{
	if (!WoTLocalNetworkAccess::OSAsksForAccess() || bLocalNetworkHeadsUpSeenThisRun)
	{
		return false;
	}
	if (const UWoTPreferencesSubsystem* Preferences = ResolvePreferences())
	{
		if (Preferences->HasSeenLocalNetworkHeadsUp())
		{
			return false;
		}
	}
	bShowOnlineDisclosure = false;
	bShowLocalNetworkNotice = false;
	PendingLocalNetworkAction = Action;
	return true;
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildLocalNetworkHeadsUp()
{
	return BuildNoticePanel(LocalNetworkHeadsUpWidget,
		[this]() { return PendingLocalNetworkAction != ELocalNetworkAction::None; },
		TEXT("macOS will ask about your local network."),
		TEXT("To find, host, and join games on this network, WoT needs "
			"macOS Local Network access. When macOS asks whether WoT Reborn "
			"may find devices on your local network, choose Allow. "
			"You can change this later in System Settings > Privacy & "
			"Security > Local Network."),
		TEXT("Continue"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::ContinueLocalNetworkHeadsUp),
		TEXT("Not Now"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::CancelLocalNetworkHeadsUp),
		false, &LocalNetworkHeadsUpPrimaryWidget);
}

FReply SWoTFrontEndRoot::ContinueLocalNetworkHeadsUp()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	const ELocalNetworkAction Action = PendingLocalNetworkAction;
	PendingLocalNetworkAction = ELocalNetworkAction::None;
	// Shown once per profile. A refused write (dormant preferences, unguarded
	// run) still counts for this process; it never blocks the action.
	bLocalNetworkHeadsUpSeenThisRun = true;
	if (UWoTPreferencesSubsystem* Preferences = ResolvePreferences())
	{
		const FWoTBootstrapStatus Recorded = Preferences->SetLocalNetworkHeadsUpSeen();
		WoTRecordBootstrapHeartbeat(TEXT("local-network-heads-up"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("recorded=%d"), Recorded.IsError() ? 0 : 1));
	}
	switch (Action)
	{
	case ELocalNetworkAction::Refresh: return RefreshSessions();
	case ELocalNetworkAction::Host: return Host();
	case ELocalNetworkAction::ConnectDirect: return ConnectDirect();
	case ELocalNetworkAction::None:
	default: return FReply::Handled();
	}
}

void SWoTFrontEndRoot::ClearPendingNotices()
{
	// The online-identity disclosure carries no action to replay, so it is
	// left to its own Accept/Cancel.
	bShowLocalNetworkNotice = false;
	PendingLocalNetworkAction = ELocalNetworkAction::None;
	PendingKeychainAction = EKeychainAction::None;
	bKeychainHeadsUpAccepted = false;
}

bool SWoTFrontEndRoot::ShowKeychainHeadsUpIfNeeded(EKeychainAction Action)
{
	if (bKeychainHeadsUpAccepted)
	{
		// Continue is re-running the action it gated.
		bKeychainHeadsUpAccepted = false;
		return false;
	}
	EWoTKeychainAccess Access = EWoTKeychainAccess::NoPrompt;
#if WITH_AUTOMATION_TESTS
	if (KeychainAccessForTesting.IsSet())
	{
		Access = KeychainAccessForTesting.GetValue();
	}
	else
#endif
	if (const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get())
	{
		Access = Sessions->ProbeSignInKeychainPrompt();
	}
	// Unknown stays quiet: a surprise prompt is better than a heads-up on
	// every sign-in for a keychain we cannot read.
	if (Access != EWoTKeychainAccess::WillPrompt)
	{
		return false;
	}
	bShowOnlineDisclosure = false;
	bShowLocalNetworkNotice = false;
	PendingLocalNetworkAction = ELocalNetworkAction::None;
	PendingKeychainAction = Action;
	return true;
}

TSharedRef<SWidget> SWoTFrontEndRoot::BuildKeychainHeadsUp()
{
	return BuildNoticePanel(KeychainHeadsUpWidget,
		[this]() { return PendingKeychainAction != EKeychainAction::None; },
		TEXT("macOS will ask about your keychain."),
		TEXT("WoT Reborn keeps its anonymous online ID in your Mac's "
			"keychain. This version of WoT is new to macOS, so it will ask "
			"before WoT can read that ID. Choose Always Allow; otherwise macOS "
			"asks again each time WoT signs in."),
		TEXT("Continue"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::ContinueKeychainHeadsUp),
		TEXT("Not Now"),
		FOnClicked::CreateSP(this, &SWoTFrontEndRoot::CancelKeychainHeadsUp),
		false, &KeychainHeadsUpPrimaryWidget);
}

FReply SWoTFrontEndRoot::ContinueKeychainHeadsUp()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	const EKeychainAction Action = PendingKeychainAction;
	PendingKeychainAction = EKeychainAction::None;
	if (Action == EKeychainAction::None)
	{
		return FReply::Handled();
	}
	bKeychainHeadsUpAccepted = true;
#if WITH_AUTOMATION_TESTS
	++KeychainReplayCountForTesting;
#endif
	FReply Reply = FReply::Handled();
	switch (Action)
	{
	case EKeychainAction::Refresh: Reply = RefreshSessions(); break;
	case EKeychainAction::Host: Reply = Host(); break;
	case EKeychainAction::None:
	default: break;
	}
	// Whatever path the action took, the next one is gated afresh.
	bKeychainHeadsUpAccepted = false;
	return Reply;
}

FReply SWoTFrontEndRoot::CancelKeychainHeadsUp()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	PendingKeychainAction = EKeychainAction::None;
	ShowReleaseNotesIfNeeded();
	if (bShowReleaseNotesNotice) RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::CancelLocalNetworkHeadsUp()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	PendingLocalNetworkAction = ELocalNetworkAction::None;
	ShowReleaseNotesIfNeeded();
	if (bShowReleaseNotesNotice) RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::OpenLocalNetworkSettings()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	bShowLocalNetworkNotice = false;
	WoTLocalNetworkAccess::OpenSettings();
	ShowReleaseNotesIfNeeded();
	if (bShowReleaseNotesNotice) RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::DismissLocalNetworkNotice()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	bShowLocalNetworkNotice = false;
	ShowReleaseNotesIfNeeded();
	if (bShowReleaseNotesNotice) RebuildScreen();
	return FReply::Handled();
}

ECheckBoxState SWoTFrontEndRoot::InternetCheckState() const
{
	if (!IsPresentationReady()) return ECheckBoxState::Unchecked;
	return ViewModel.IsInternet()
		? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

ECheckBoxState SWoTFrontEndRoot::AlwaysUseEpicRelayState() const
{
	const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	return Sessions != nullptr && Sessions->AlwaysUseEpicRelay()
		? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SWoTFrontEndRoot::SetAlwaysUseEpicRelay(ECheckBoxState State)
{
	if (!IsPresentationReady()) return;
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (Sessions == nullptr
		|| !Sessions->SetAlwaysUseEpicRelay(State == ECheckBoxState::Checked))
	{
		ViewModel.ReportNotice(TEXT("Could not apply the relay preference."),
			/*bError*/ true);
	}
}

bool SWoTFrontEndRoot::CanChooseInternet() const
{
	if (!IsPresentationReady() || !ViewModel.CanLeaveOrSwitchNetwork()) return false;
	const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	return Sessions != nullptr
		&& Sessions->GetInternetStatus().State != EWoTInternetState::Unavailable;
}

FText SWoTFrontEndRoot::InternetStatusText() const
{
	if (!IsPresentationReady()) return FText::GetEmpty();
#if WITH_AUTOMATION_TESTS
	if (!InternetStatusTextOverrideForTesting.IsEmpty())
	{
		return InternetStatusTextOverrideForTesting;
	}
#endif
	const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (Sessions == nullptr)
	{
		return FText::GetEmpty();
	}
	const FWoTInternetStatus Status = Sessions->GetInternetStatus();
	if (ViewModel.IsInternet() || Status.State == EWoTInternetState::Unavailable)
	{
		return WoTFrontEndSlate_Text(Status.UserText());
	}
	return WoTFrontEndSlate_Text(
		TEXT("LAN: this network only, or Direct Address. No online service is used."));
}

EVisibility SWoTFrontEndRoot::ResetIdentityVisibility() const
{
	if (!IsPresentationReady()) return EVisibility::Collapsed;
#if WITH_AUTOMATION_TESTS
	if (bShowResetIdentityForTesting) return EVisibility::Visible;
#endif
	const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	return Sessions != nullptr && Sessions->IsOnlineIdentityAcknowledged()
		&& Sessions->GetInternetStatus().State != EWoTInternetState::Unavailable
		? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SWoTFrontEndRoot::AcceptOnlineDisclosure()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	bShowOnlineDisclosure = false;
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (Sessions != nullptr && Sessions->AcknowledgeOnlineIdentity())
	{
		ViewModel.AbandonSearch();
		ViewModel.SetLAN(false);
		AutoFetchSessions();
	}
	else
	{
		ViewModel.SetLAN(true);
		ViewModel.ReportNotice(TEXT("Could not record your choice. Internet play stays off."),
			/*bError*/ true);
	}
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::CancelOnlineDisclosure()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	bShowOnlineDisclosure = false;
	ViewModel.SetLAN(true);
	ShowReleaseNotesIfNeeded();
	if (bShowReleaseNotesNotice) RebuildScreen();
	return FReply::Handled();
}

FReply SWoTFrontEndRoot::ResetOnlineIdentity()
{
	if (!IsPresentationReady()) return FReply::Unhandled();
	UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (Sessions == nullptr || !ViewModel.BeginResetIdentity())
	{
		return FReply::Handled();
	}
	if (!Sessions->ResetOnlineIdentity())
	{
		ViewModel.FinishResetIdentity(false);
	}
	return FReply::Handled();
}

void SWoTFrontEndRoot::HandleIdentityResetComplete(bool bSucceeded)
{
	if (!IsPresentationReady()) return;
	ViewModel.FinishResetIdentity(bSucceeded);
}

FString SWoTFrontEndRoot::InternetFailureText() const
{
	const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
	if (Sessions == nullptr || !ViewModel.IsInternet())
	{
		return FString();
	}
	if (!Sessions->IsOnlineIdentityAcknowledged())
	{
		return FWoTEOSConfig::DescribeReason(TEXT("not-acknowledged"));
	}
	const FWoTInternetStatus Status = Sessions->GetInternetStatus();
	return Status.State == EWoTInternetState::Unavailable
		|| Status.State == EWoTInternetState::Failed
		? Status.UserText() : FString();
}

FString SWoTFrontEndRoot::SessionPingText(const FWoTSessionListEntry& Entry) const
{
	// EOS lobby results carry the search round trip, not host latency.
	return Entry.bInternet ? FString(TEXT("-")) : FString::FromInt(Entry.PingMs);
}

FString SWoTFrontEndRoot::SessionServerText(const FWoTSessionListEntry& Entry) const
{
	if (!Entry.bVersionMismatch) return Entry.ServerName;
	const int32 RelativeSequence = CompareSessionReleaseSequence(Entry);
	if (RelativeSequence > 0)
	{
		return Entry.ServerName + TEXT(" (newer version)");
	}
	if (RelativeSequence < 0)
	{
		return Entry.ServerName + TEXT(" (older version)");
	}
	return Entry.ServerName + TEXT(" (other version)");
}

FString SWoTFrontEndRoot::SessionVersionMismatchNotice(
	const FWoTSessionListEntry& Entry) const
{
	const int32 RelativeSequence = CompareSessionReleaseSequence(Entry);
	if (RelativeSequence > 0)
	{
		return TEXT("That server is on a newer version. Update to join.");
	}
	if (RelativeSequence < 0)
	{
		return TEXT(
			"That server is on an older version and cannot be joined.");
	}
	return TEXT(
		"That server runs a different game version and cannot be joined.");
}

int32 SWoTFrontEndRoot::CompareSessionReleaseSequence(
	const FWoTSessionListEntry& Entry) const
{
	const UWoTBootstrapSubsystem* Bootstrap = ResolveBootstrap();
	const uint64 LocalSequence = Bootstrap
		? Bootstrap->Snapshot().RunningReleaseSequence : 0;
	if (Entry.HostReleaseSequence == 0 || LocalSequence == 0
		|| Entry.HostReleaseSequence == LocalSequence)
	{
		return 0;
	}
	return Entry.HostReleaseSequence > LocalSequence ? 1 : -1;
}

FText SWoTFrontEndRoot::SelectedMapText() const
{
	if (!IsPresentationReady()) return FText::GetEmpty();
	const FWoTFrontEndMapEntry* Selected = ViewModel.GetSelectedMap();
	return Selected ? HostMapPickerText(Selected)
		: WoTFrontEndSlate_Text(TEXT("No playable maps"));
}

FText SWoTFrontEndRoot::HostMapPickerText(const FWoTFrontEndMapEntry* Entry)
{
	return Entry ? WoTFrontEndSlate_Text(Entry->ShortName) : FText::GetEmpty();
}

FText SWoTFrontEndRoot::StatusText() const
{
	if (!IsPresentationReady()) return FText::GetEmpty();
	const FString Status = ViewModel.GetStatusText();
	return Status.IsEmpty()
		? RetailText(TEXT("menuMultiPlayer.HelpMessage[1]"))
		: WoTFrontEndSlate_Text(Status);
}

FText SWoTFrontEndRoot::BrowserStatusText() const
{
	if (!IsPresentationReady()) return FText::GetEmpty();
	int32 Players = 0;
	for (const FSessionItem& Item : SessionItems)
	{
		if (Item.IsValid())
		{
			Players += Item->CurrentPlayers;
		}
	}
	// UBrowserServerListWindow.uc:552-554 keeps the plural labels verbatim.
	return WoTFrontEndSlate_Text(FString::Printf(
		TEXT("%d Servers, %d Players"), SessionItems.Num(), Players));
}

ECheckBoxState SWoTFrontEndRoot::LANState() const
{
	if (!IsPresentationReady()) return ECheckBoxState::Unchecked;
	return ViewModel.GetHostRequest().bLAN
		? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

FMargin SWoTFrontEndRoot::MainMenuEntryPadding(int32 EntryIndex) const
{
	const float HalfSpacing = MainMenuSpacing * 0.5f;
	return FMargin(
		0.0f,
		EntryIndex == 0 ? 0.0f : HalfSpacing,
		0.0f,
		EntryIndex == 3 ? 0.0f : HalfSpacing);
}

void SWoTFrontEndRoot::HandleHostComplete(bool bSucceeded)
{
	if (!IsPresentationReady()) return;
	if (!bSucceeded)
	{
		UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
		const FString Detail = Sessions
			? Sessions->HostDiagnostics() : FString(TEXT("no session subsystem"));
		WoTRecordBootstrapHeartbeat(TEXT("host-failed"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, Detail.Left(512));
	}
	ViewModel.FinishHost(bSucceeded,
		bSucceeded ? FString() : InternetFailureText());
}

void SWoTFrontEndRoot::HandleSearchComplete(bool bSucceeded,
	const TArray<FWoTSessionListEntry>& Results)
{
	if (!IsPresentationReady()) return;
	const bool bLiveSearch = ViewModel.GetScreen() == EWoTFrontEndScreen::Play
		&& ViewModel.GetOperation() == EWoTFrontEndAsyncOperation::Searching;
	if (bRestartSearchWhenStaleLands)
	{
		// These results belong to a search abandoned by leaving Play.
		bRestartSearchWhenStaleLands = false;
		if (bLiveSearch)
		{
			UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
			if (!Sessions || !Sessions->Find(ViewModel.GetHostRequest().bLAN))
			{
				ViewModel.FinishSearch(false, TArray<FWoTSessionListEntry>(),
					InternetFailureText());
				RebuildSessionItems();
			}
		}
		return;
	}
	ViewModel.FinishSearch(bSucceeded, Results,
		bSucceeded ? FString() : InternetFailureText());
	RebuildSessionItems();
	// Only an empty LAN result can be the privacy block: a denied subnet
	// broadcast reports success, so the search itself looks clean. A search
	// abandoned by leaving Play says nothing about the screen now shown.
	if (bLiveSearch && ViewModel.GetHostRequest().bLAN && Results.IsEmpty())
	{
		ShowLocalNetworkNoticeIfDenied(TEXT("search"));
	}
}

void SWoTFrontEndRoot::HandleJoinComplete(bool bSucceeded)
{
	if (!IsPresentationReady()) return;
	ViewModel.FinishJoin(bSucceeded);
}

void SWoTFrontEndRoot::HandleConnectionFailure(const FString& Reason)
{
	if (!IsPresentationReady()) return;
	if (const UWoTSessionSubsystem* Sessions = SessionSubsystem.Get();
		Sessions
		&& Sessions->GetPasswordChallenge() != EWoTPasswordChallenge::None)
	{
		if (ViewModel.GetScreen() != EWoTFrontEndScreen::Play)
		{
			ViewModel.ShowPlay(FWoTFrontEndMapCatalog::Enumerate());
		}
		RebuildScreen();
		return;
	}
	const bool bOpenPlay = ViewModel.GetScreen() != EWoTFrontEndScreen::Play;
	if (bOpenPlay)
	{
		ViewModel.ShowPlay(FWoTFrontEndMapCatalog::Enumerate());
	}
	ViewModel.ReportConnectionFailure(Reason);
	if (bOpenPlay)
	{
		RebuildScreen();
	}
	// Do not consume the subsystem's retained copy here: UEngine schedules a
	// default-map travel after pending connection failure. The replacement menu
	// root consumes it during Construct, after the fallback world has loaded.
}
