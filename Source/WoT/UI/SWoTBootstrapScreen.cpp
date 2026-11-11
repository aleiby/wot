#include "SWoTBootstrapScreen.h"

#include "WoTSetupBackdrop.h"
#include "WoTBackStack.h"
#include "WoTControllerUIInput.h"
#include "WoTMenuNavigation.h"
#include "WoTBootstrapCore/TestOverrides.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Brushes/SlateColorBrush.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "WoTBootstrapScreen"

namespace
{
// This screen runs BEFORE any game content exists, so it cannot use
// FWoTMenuStyle -- those are retail fonts under /Game/WoT/UI and the payload is
// still downloading. Everything here must come from the engine-embedded core
// style. Sizes are therefore explicit: the Slate default is ~9pt, which is
// unreadable at desktop resolutions.
// Everything is drawn at twice its original size.
constexpr float WoTBootstrapScale = 2.0f;
constexpr float WoTBootstrapTitleFontSize = 24.0f * WoTBootstrapScale;
constexpr float WoTBootstrapStatusFontSize = 16.0f * WoTBootstrapScale;
constexpr float WoTBootstrapDetailFontSize = 14.0f * WoTBootstrapScale;
constexpr float WoTBootstrapButtonFontSize = 15.0f * WoTBootstrapScale;
// Long status and installer-path lines stay readable instead of stretching the
// full width of an arbitrarily large window, and narrower landscape windows
// (4:3 is ~1440 units, since Slate scales by window height) shrink the column
// instead of clipping it. Portrait slivers under ~500 units would still clip
// the 2x buttons; no supported window is that narrow.
constexpr float WoTBootstrapContentWidth = 720.0f * WoTBootstrapScale;
constexpr float WoTBootstrapOuterPadding = 48.0f;
constexpr float WoTBootstrapProgressBarHeight = 14.0f * WoTBootstrapScale;
constexpr float WoTBootstrapButtonGap = 12.0f * WoTBootstrapScale;
const FMargin WoTBootstrapButtonPadding(16.0f * WoTBootstrapScale, 8.0f * WoTBootstrapScale);
// Gold, matching the menus, rather than the stock blue on a warm backdrop.
// The stock fill image is itself blue, so the colour is the brush, not a tint.
// Given in sRGB: a linear literal displayed washed-out pale.
const FLinearColor WoTBootstrapProgressFill =
	FLinearColor::FromSRGBColor(FColor(196, 152, 64));

FSlateFontInfo WoTBootstrapFont(const TCHAR* Style, float Size)
{
	return FCoreStyle::GetDefaultFontStyle(Style, Size);
}

const FProgressBarStyle& WoTBootstrapProgressStyle()
{
	static const FProgressBarStyle Style =
		FProgressBarStyle(FCoreStyle::Get().GetWidgetStyle<FProgressBarStyle>(
			"ProgressBar"))
		.SetBackgroundImage(FSlateColorBrush(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f)))
		.SetFillImage(FSlateColorBrush(WoTBootstrapProgressFill));
	return Style;
}

const FTextBlockStyle& WoTBootstrapButtonTextStyle()
{
	static const FTextBlockStyle Style =
		FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(
			"NormalText"))
		.SetFont(WoTBootstrapFont(TEXT("Regular"),
			WoTBootstrapButtonFontSize));
	return Style;
}
}

void SWoTBootstrapScreen::Construct(const FArguments& InArgs)
{
	SnapshotAttribute = InArgs._Snapshot;
	ContinueRequested = InArgs._OnContinueRequested;
	SkipRequested = InArgs._OnSkipRequested;
	ExitRequested = InArgs._OnExitRequested;
	BrowseInstallerRequested = InArgs._OnBrowseInstaller;
	GetOriginalGameRequested = InArgs._OnGetOriginalGame;
	Ready = InArgs._OnReady;

	BackdropBrush = WoTSetupBackdrop::MakeBrush(WoTSetupBackdrop::Current());

	// Centred in both axes and width-limited: the slots are all AutoHeight, so
	// filling the window stranded the whole screen in the top-left corner and
	// left the rest as dead black space.
	// The backdrop fills the window behind it.
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(BackdropBrush.Get())
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(FMargin(WoTBootstrapOuterPadding))
		[
		SNew(SBox)
		.WidthOverride_Lambda([this]()
		{
			return FOptionalSize(ContentWidthForTesting(ArrangedWidth));
		})
		[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 18.0f * WoTBootstrapScale)
		[
			// Wraps: at 48 pt a long step name ("Building textures, sounds,
			// meshes, and maps failed") is wider than the column.
			SAssignNew(TitleText, STextBlock)
			.Text(this, &SWoTBootstrapScreen::PhaseText)
			.Font(WoTBootstrapFont(TEXT("Bold"), WoTBootstrapTitleFontSize))
			.AutoWrapText(true)
			.AccessibleText(this, &SWoTBootstrapScreen::PhaseText)
		]
		+ SVerticalBox::Slot().AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 10.0f * WoTBootstrapScale)
		[
			SNew(SBox)
			.HeightOverride(WoTBootstrapProgressBarHeight)
			[
				SAssignNew(ProgressBar, SProgressBar)
				.Style(&WoTBootstrapProgressStyle())
				.Percent(this, &SWoTBootstrapScreen::PhaseProgress)
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 12.0f * WoTBootstrapScale)
		[
			SNew(STextBlock)
			.Text(this, &SWoTBootstrapScreen::ProgressText)
			.Font(WoTBootstrapFont(TEXT("Regular"),
				WoTBootstrapStatusFontSize))
			.AutoWrapText(true)
			.AccessibleText(this, &SWoTBootstrapScreen::ProgressText)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(this, &SWoTBootstrapScreen::DiagnosticText)
			.Font(WoTBootstrapFont(TEXT("Regular"),
				WoTBootstrapDetailFontSize))
			.AutoWrapText(true)
			.AccessibleText(this, &SWoTBootstrapScreen::DiagnosticText)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(this, &SWoTBootstrapScreen::OriginalGameText)
			.Font(WoTBootstrapFont(TEXT("Regular"),
				WoTBootstrapDetailFontSize))
			.AutoWrapText(true)
			.AccessibleText(this, &SWoTBootstrapScreen::OriginalGameText)
			.Visibility(this,
				&SWoTBootstrapScreen::OriginalGameTextVisibility)
		]
		+ SVerticalBox::Slot().AutoHeight()
			.Padding(0.0f, 12.0f * WoTBootstrapScale, 0.0f, 0.0f)
		[
			// Shown once "Get it on GOG" was clicked: GOG steers buyers to its
			// Galaxy app, and the offline installer is easy to miss.
			SNew(STextBlock)
			.Text(this, &SWoTBootstrapScreen::GogStepsText)
			.Font(WoTBootstrapFont(TEXT("Regular"),
				WoTBootstrapDetailFontSize))
			.AutoWrapText(true)
			.AccessibleText(this, &SWoTBootstrapScreen::GogStepsText)
			.Visibility(this, &SWoTBootstrapScreen::GogStepsVisibility)
		]
		+ SVerticalBox::Slot().AutoHeight()
			.Padding(0.0f, 18.0f * WoTBootstrapScale, 0.0f, 0.0f)
		[
			// Each button pads its right side, so whichever button is visible
			// first sits flush with the column (hidden ones take no space).
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
				.Padding(0.0f, 0.0f, WoTBootstrapButtonGap, 0.0f)
			[
				SAssignNew(ContinueButton, SButton)
				.Text(LOCTEXT("Continue", "Continue"))
				.TextStyle(&WoTBootstrapButtonTextStyle())
				.ContentPadding(WoTBootstrapButtonPadding)
				.AccessibleText(LOCTEXT("ContinueAccessible",
					"Continue optional bootstrap work"))
				.IsEnabled(this,
					&SWoTBootstrapScreen::CanContinueOptionalWork)
				.Visibility(this, &SWoTBootstrapScreen::ContinueVisibility)
				.OnClicked(this, &SWoTBootstrapScreen::Continue)
			]
			+ SHorizontalBox::Slot().AutoWidth()
				.Padding(0.0f, 0.0f, WoTBootstrapButtonGap, 0.0f)
			[
				SAssignNew(SkipButton, SButton)
				.Text(LOCTEXT("Skip", "Skip"))
				.TextStyle(&WoTBootstrapButtonTextStyle())
				.ContentPadding(WoTBootstrapButtonPadding)
				.AccessibleText(LOCTEXT("SkipAccessible",
					"Skip optional bootstrap work"))
				.IsEnabled(this, &SWoTBootstrapScreen::CanSkipOptionalWork)
				.Visibility(this, &SWoTBootstrapScreen::SkipVisibility)
				.OnClicked(this, &SWoTBootstrapScreen::Skip)
			]
			+ SHorizontalBox::Slot().AutoWidth()
				.Padding(0.0f, 0.0f, WoTBootstrapButtonGap, 0.0f)
			[
				SAssignNew(ExitButton, SButton)
				.Text(LOCTEXT("Quit", "Quit"))
				.TextStyle(&WoTBootstrapButtonTextStyle())
				.ContentPadding(WoTBootstrapButtonPadding)
				.AccessibleText(LOCTEXT("QuitAccessible",
					"Progress is saved. You can continue next time."))
				.IsEnabled(this, &SWoTBootstrapScreen::CanExit)
				.Visibility(this, &SWoTBootstrapScreen::ExitVisibility)
				.OnClicked(this, &SWoTBootstrapScreen::Exit)
			]
			+ SHorizontalBox::Slot().AutoWidth()
				.Padding(0.0f, 0.0f, WoTBootstrapButtonGap, 0.0f)
			[
				SAssignNew(BrowseInstallerButton, SButton)
				.Text(LOCTEXT("BrowseInstaller", "Browse…"))
				.TextStyle(&WoTBootstrapButtonTextStyle())
				.ContentPadding(WoTBootstrapButtonPadding)
				.AccessibleText(LOCTEXT("BrowseInstallerAccessible",
					"Browse for the GOG installer"))
				.Visibility(this,
					&SWoTBootstrapScreen::OriginalGameActionVisibility)
				.OnClicked(this,
					&SWoTBootstrapScreen::BrowseInstaller)
			]
			+ SHorizontalBox::Slot().AutoWidth()
				.Padding(0.0f, 0.0f, WoTBootstrapButtonGap, 0.0f)
			[
				SAssignNew(GetOriginalGameButton, SButton)
				.Text(LOCTEXT("GetOriginalGame", "Get it on GOG"))
				.TextStyle(&WoTBootstrapButtonTextStyle())
				.ContentPadding(WoTBootstrapButtonPadding)
				.AccessibleText(LOCTEXT("GetOriginalGameAccessible",
					"Get The Wheel of Time (1999) on GOG"))
				.Visibility(this,
					&SWoTBootstrapScreen::OriginalGameActionVisibility)
				.OnClicked(this, &SWoTBootstrapScreen::GetOriginalGame)
			]
		]
		]
		]
	];

	// Only the visible buttons take focus; Left/Right wrap through them and
	// focus cannot leave the row.
	const TArray<TSharedPtr<SWidget>> Buttons = {ContinueButton, SkipButton,
		ExitButton, BrowseInstallerButton, GetOriginalGameButton};
	WoTWrapHorizontalNavigation(Buttons, /*bHoldVertical*/ true);
	CursorLease = FWoTControllerUIInput::AcquireCursorLease();
	// B and View are consumed and do nothing: B must never quit the game
	// mid-setup. Escape keeps its own path (OnKeyDown).
	BackStackEntry = FWoTBackStack::Get().Register(AsShared(), nullptr,
		[]() { return true; });

#if WITH_ACCESSIBILITY
	SetAccessibleBehavior(EAccessibleBehavior::Summary,
		TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(
			this, &SWoTBootstrapScreen::PhaseText)));
	SetCanChildrenBeAccessible(true);
#endif
}

FWoTBootstrapSnapshot SWoTBootstrapScreen::GetSnapshot() const
{
	return SnapshotAttribute.IsSet()
		? SnapshotAttribute.Get() : FWoTBootstrapSnapshot();
}

FText SWoTBootstrapScreen::PhaseText() const
{
	const FWoTBootstrapSnapshot Snapshot = GetSnapshot();
	if (Snapshot.Phase == EWoTBootstrapPhase::Fatal
		&& Snapshot.FailedPhase.IsSet())
	{
		return FText::Format(LOCTEXT("FailedPhase", "{0} failed"),
			FWoTBootstrapViewModel::PhaseText(Snapshot.FailedPhase.GetValue()));
	}
	return FWoTBootstrapViewModel::PhaseText(Snapshot.Phase);
}

FText SWoTBootstrapScreen::ProgressText() const
{
	const FWoTBootstrapSnapshot Snapshot = GetSnapshot();
	auto WithStage = [&Snapshot](const FText& Progress)
	{
		return Snapshot.StageText.IsEmpty() ? Progress : FText::Format(
			LOCTEXT("StageAndProgress", "{0}\n{1}"), Snapshot.StageText,
			Progress);
	};
	if (Snapshot.bDiskPreflightVisible && Snapshot.RequiredBytes.IsSet()
		&& Snapshot.AvailableBytes.IsSet())
	{
		if (Snapshot.RequiredBytes.GetValue() == 0)
		{
			return WithStage(LOCTEXT("NoDiskSpaceRequired",
				"No additional disk space required"));
		}
		return WithStage(FText::FromString(FString::Printf(
			TEXT("Required: %llu bytes. Available: %llu bytes."),
			static_cast<unsigned long long>(Snapshot.RequiredBytes.GetValue()),
			static_cast<unsigned long long>(Snapshot.AvailableBytes.GetValue()))));
	}
	if (Snapshot.CompletedBytes.IsSet() && Snapshot.TotalBytes.IsSet())
	{
		const uint64 Total = Snapshot.TotalBytes.GetValue();
		const uint64 Completed = Snapshot.CompletedBytes.GetValue();
		const uint64 Percentage = Total == 0 ? 0 : (Completed * 100) / Total;
		return WithStage(FText::FromString(FString::Printf(
			TEXT("%llu / %llu bytes (%llu%%)"),
			static_cast<unsigned long long>(Completed),
			static_cast<unsigned long long>(Total),
			static_cast<unsigned long long>(Percentage))));
	}
	if (Snapshot.TotalUnits != 0)
	{
		return WithStage(FText::FromString(FString::Printf(TEXT("%llu / %llu completed"),
			static_cast<unsigned long long>(Snapshot.CompletedUnits),
			static_cast<unsigned long long>(Snapshot.TotalUnits))));
	}
	return WithStage(FText::GetEmpty());
}

FText SWoTBootstrapScreen::DiagnosticText() const
{
	const FWoTBootstrapSnapshot Snapshot = GetSnapshot();
	if (!Snapshot.FatalMessage.IsEmpty())
		return FText::FromString(Snapshot.FatalMessage);
	const FText Code = FWoTBootstrapViewModel::DiagnosticCodeText(
		Snapshot.DiagnosticCode);
	return Code.IsEmpty() ? FText::GetEmpty()
		: FText::Format(LOCTEXT("Diagnostic", "Diagnostic code: {0}"),
			Code);
}

FText SWoTBootstrapScreen::OriginalGameText() const
{
	const FWoTBootstrapSnapshot Snapshot = GetSnapshot();
	if (Snapshot.Phase != EWoTBootstrapPhase::OriginalGameNeeded)
	{
		return FText::GetEmpty();
	}
	switch (Snapshot.OriginalGameNeededReason)
	{
	case EWoTOriginalGameNeededReason::Downloading:
		return LOCTEXT("OriginalGameDownloading",
			"Found the GOG installer in your Downloads folder. Waiting for the "
			"download to finish…");
	case EWoTOriginalGameNeededReason::NotTheInstaller:
		return LOCTEXT("OriginalGameWrongInstaller",
			"A file with the installer's name is there, but it isn't the expected "
			"GOG installer (its size or checksum differs). Download it again from "
			"your GOG library, or click Browse.");
	case EWoTOriginalGameNeededReason::PickedWrongFile:
		return LOCTEXT("OriginalGamePickedWrongFile",
			"That file isn't the GOG installer for The Wheel of Time (1999). "
			"Choose setup_the_wheel_of_time_333b_(54846).exe.");
	case EWoTOriginalGameNeededReason::OnlineOnly:
		return FText::FromString(Snapshot.OriginalGameNeededDetail);
	case EWoTOriginalGameNeededReason::UnsupportedReparsePoint:
		return LOCTEXT("OriginalGameUnsupportedReparsePoint",
			"That installer is stored through an unsupported Windows reparse "
			"point. Move or copy it to a normal local file, then click Browse.");
	case EWoTOriginalGameNeededReason::HardLinked:
		return LOCTEXT("OriginalGameHardLinked",
			"That installer is a hard link (one file with more than one name), "
			"which WoT Reborn won't read. Copy it to an ordinary file instead of "
			"linking it, then click Browse.");
	case EWoTOriginalGameNeededReason::Missing:
	default:
		return LOCTEXT("OriginalGameInstructions",
			"WoT Reborn builds its game data from your copy of The Wheel of Time "
			"(1999). Download the offline installer from your GOG library — WoT "
			"Reborn finds it in your Downloads folder automatically. If it's "
			"somewhere else, click Browse.");
	}
}

TOptional<float> SWoTBootstrapScreen::PhaseProgress() const
{
	const FWoTBootstrapSnapshot Snapshot = GetSnapshot();
	if (Snapshot.Phase == EWoTBootstrapPhase::Ready)
	{
		return 1.0f;
	}
	// An unknown total shows an EMPTY bar, never SProgressBar's marquee (an
	// unset Percent): the guarded package stages no Slate marquee image
	// (Starship/CoreWidgets/ProgressBar/ProgressMarquee), so the marquee drew
	// as a solid full bar and every new phase appeared to start at 100%
	// before dropping to 0. The status text carries the activity instead.
	if ((FWoTBootstrapViewModel::IsBytePhase(Snapshot.Phase)
			&& (!Snapshot.TotalBytes.IsSet()
				|| Snapshot.TotalBytes.GetValue() == 0))
		|| (FWoTBootstrapViewModel::IsItemPhase(Snapshot.Phase)
			&& Snapshot.TotalUnits == 0))
	{
		return 0.0f;
	}
	return Snapshot.PhaseFraction.Get(0.0f);
}

EVisibility SWoTBootstrapScreen::OriginalGameTextVisibility() const
{
	return GetSnapshot().Phase == EWoTBootstrapPhase::OriginalGameNeeded
		? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SWoTBootstrapScreen::OriginalGameActionVisibility() const
{
	return GetSnapshot().Phase == EWoTBootstrapPhase::OriginalGameNeeded
		? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SWoTBootstrapScreen::ContinueVisibility() const
{
	return CanContinueOptionalWork()
		? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SWoTBootstrapScreen::SkipVisibility() const
{
	return CanSkipOptionalWork()
		? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SWoTBootstrapScreen::ExitVisibility() const
{
	return CanExit()
		&& GetSnapshot().Phase != EWoTBootstrapPhase::OriginalGameNeeded
		? EVisibility::Visible : EVisibility::Collapsed;
}

bool SWoTBootstrapScreen::CanContinueOptionalWork() const
{
	return !GetSnapshot().bMandatoryWork && ContinueRequested.IsBound();
}

bool SWoTBootstrapScreen::CanSkipOptionalWork() const
{
	return !GetSnapshot().bMandatoryWork && SkipRequested.IsBound();
}

bool SWoTBootstrapScreen::CanExit() const
{
	return GetSnapshot().bCanExit && ExitRequested.IsBound();
}

FReply SWoTBootstrapScreen::Continue()
{
	if (CanContinueOptionalWork())
	{
		ContinueRequested.ExecuteIfBound();
	}
	return FReply::Handled();
}

FReply SWoTBootstrapScreen::Skip()
{
	if (CanSkipOptionalWork())
	{
		SkipRequested.ExecuteIfBound();
	}
	return FReply::Handled();
}

FReply SWoTBootstrapScreen::Exit()
{
	if (CanExit())
	{
		ExitRequested.ExecuteIfBound();
	}
	return FReply::Handled();
}

FReply SWoTBootstrapScreen::BrowseInstaller()
{
	if (OriginalGameActionVisibility() == EVisibility::Visible)
	{
		BrowseInstallerRequested.ExecuteIfBound();
	}
	return FReply::Handled();
}

FReply SWoTBootstrapScreen::GetOriginalGame()
{
	if (OriginalGameActionVisibility() == EVisibility::Visible)
	{
		bShowGogSteps = true;
		GetOriginalGameRequested.ExecuteIfBound();
	}
	return FReply::Handled();
}

FText SWoTBootstrapScreen::GogStepsText() const
{
	return LOCTEXT("OriginalGameGogSteps",
		"GOG pushes you toward its GOG Galaxy app, but WoT Reborn needs the "
		"offline installer file. A game installed through Galaxy isn't enough "
		"on its own.\n"
		"On gog.com: sign in, open your games library and click The Wheel of "
		"Time. Under \"Download offline backup game installers\", download the "
		"game's installer (about 1 GB).\n"
		"In GOG Galaxy: select The Wheel of Time, click More next to Play, "
		"choose \"Backups & goodies\" and download the installer under \"Game "
		"Backup Copy\".\n"
		"Leave setup_the_wheel_of_time_333b_(54846).exe in your Downloads "
		"folder and WoT Reborn picks it up, or click Browse.");
}

EVisibility SWoTBootstrapScreen::GogStepsVisibility() const
{
	return bShowGogSteps
		&& GetSnapshot().Phase == EWoTBootstrapPhase::OriginalGameNeeded
		? EVisibility::Visible : EVisibility::Collapsed;
}

FReply SWoTBootstrapScreen::OnKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		return CanExit() ? Exit() : FReply::Unhandled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SWoTBootstrapScreen::Tick(const FGeometry& AllottedGeometry,
	double InCurrentTime, float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	ArrangedWidth = static_cast<float>(AllottedGeometry.GetLocalSize().X);
	#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
	const EWoTBootstrapPhase CurrentPhase = GetSnapshot().Phase;
	if (!bAutoBrowseInvoked
		&& CurrentPhase == EWoTBootstrapPhase::OriginalGameNeeded
		&& LastAutoBrowsePhase != EWoTBootstrapPhase::OriginalGameNeeded
		&& FParse::Param(FCommandLine::Get(), TEXT("wottestautobrowse")))
	{
		bAutoBrowseInvoked = true;
		BrowseInstaller();
	}
	LastAutoBrowsePhase = CurrentPhase;
	#endif
	if (!bReadyAnnounced
		&& GetSnapshot().Phase == EWoTBootstrapPhase::Ready)
	{
		bReadyAnnounced = true;
		Ready.ExecuteIfBound();
	}
}

void SWoTBootstrapScreen::FocusInitialWidget()
{
	const TSharedPtr<SWidget> InitialFocus = InitialFocusWidget();
	if (InitialFocus.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			InitialFocus, EFocusCause::SetDirectly);
	}
}

TSharedPtr<SWidget> SWoTBootstrapScreen::InitialFocusWidget() const
{
	if (FWoTControllerUIInput::IsControllerMode())
	{
		// Controller mode: the first visible button, left to right.
		const TSharedPtr<SButton> Ordered[] = {ContinueButton, SkipButton,
			ExitButton, BrowseInstallerButton, GetOriginalGameButton};
		for (const TSharedPtr<SButton>& Button : Ordered)
		{
			if (Button.IsValid() && Button->GetVisibility().IsVisible())
			{
				return Button;
			}
		}
	}
	if (OriginalGameActionVisibility() == EVisibility::Visible)
	{
		return BrowseInstallerButton;
	}
	if (CanExit())
	{
		return ExitButton;
	}
	if (CanContinueOptionalWork())
	{
		return ContinueButton;
	}
	if (CanSkipOptionalWork())
	{
		return SkipButton;
	}
	return nullptr;
}

TSharedPtr<STextBlock> SWoTBootstrapScreen::TitleWidgetForTesting() const
{
	return TitleText;
}

TSharedPtr<SWidget> SWoTBootstrapScreen::ContinueWidgetForTesting() const
{
	return ContinueButton;
}

TSharedPtr<SWidget> SWoTBootstrapScreen::SkipWidgetForTesting() const
{
	return SkipButton;
}

TSharedPtr<SWidget> SWoTBootstrapScreen::ExitWidgetForTesting() const
{
	return ExitButton;
}

TSharedPtr<SWidget>
SWoTBootstrapScreen::BrowseInstallerWidgetForTesting() const
{
	return BrowseInstallerButton;
}

TSharedPtr<SWidget> SWoTBootstrapScreen::GetOriginalGameWidgetForTesting() const
{
	return GetOriginalGameButton;
}

float SWoTBootstrapScreen::ContentWidthForTesting(float ScreenWidth) const
{
	// Not yet arranged: the full design width.
	if (ScreenWidth <= 0.0f)
	{
		return WoTBootstrapContentWidth;
	}
	// Never wider than the window inside its margins.
	return FMath::Clamp(ScreenWidth - 2.0f * WoTBootstrapOuterPadding,
		0.0f, WoTBootstrapContentWidth);
}

#undef LOCTEXT_NAMESPACE
