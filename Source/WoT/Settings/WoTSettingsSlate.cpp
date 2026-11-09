#include "WoTSettingsSlate.h"

#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "../WoTMenuStyle.h"
#include "../WoTOnboardingSeams.h"
#include "../WoTPreferencesSubsystem.h"
#include "../UI/SWoTFocusOutline.h"
#include "../UI/SWoTListCombo.h"
#include "../UI/WoTBackStack.h"
#include "../UI/WoTControllerUIInput.h"
#include "../UI/WoTMenuNavigation.h"

namespace
{
// Eight retail slider cells cover the 0.1-10 sensitivity range; 1x is a cell.
// SWoTSliderValueLabel::AllValueLabels lists these steps' labels, and the
// controller's below, too.
constexpr float SensitivitySteps[8] = {0.25f, 0.5f, 0.75f, 1.0f, 2.0f, 4.0f, 7.0f, 10.0f};
// The controller stick already turns at tuned rates, so its scale stops at 4x.
constexpr float ControllerSensitivitySteps[8] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr uint16 FrameRateSteps[] = {0, 30, 60, 90, 120, 144, 165, 240};
constexpr uint16 CommonResolutions[][2] = {
	{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 800}, {1366, 768},
	{1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200},
	{2560, 1440}, {3440, 1440}, {3840, 2160}};

int32 SensitivityIndex(float Sensitivity, const float (&Steps)[8] = SensitivitySteps)
{
	int32 Best = 0;
	for (int32 Index = 1; Index < UE_ARRAY_COUNT(Steps); ++Index)
	{
		if (FMath::Abs(Steps[Index] - Sensitivity)
			< FMath::Abs(Steps[Best] - Sensitivity))
		{
			Best = Index;
		}
	}
	return Best;
}

int32 CellIndexFromSliderValue(float Value)
{
	// SWoTRetailSlider reports MinValue + StepSize * 8 * (cell - 1) / 7.
	return FMath::Clamp(FMath::RoundToInt(Value * 7.0f), 0, 7);
}

FString ResolutionLabel(uint16 Width, uint16 Height)
{
	return FString::Printf(TEXT("%u x %u"), Width, Height);
}

FText Literal(const TCHAR* Text)
{
	return FText::FromString(Text);
}
} // namespace

void SWoTSettingsScreen::Construct(const FArguments& InArgs)
{
	Page = InArgs._Page;
	Preferences = InArgs._Preferences;
	PresentationAuthority = InArgs._PresentationAuthority;
	HeadingFont = InArgs._HeadingFont;
	BodyFont = InArgs._BodyFont;
	TextResolver = InArgs._TextResolver;
	OnBack = InArgs._OnBack;
	OnSaved = InArgs._OnSaved;
	if (UWoTPreferencesSubsystem* Owner = Preferences.Get())
	{
		Confirmed = Owner->GetGameSettings();
	}
	else
	{
		Confirmed = FWoTGameSettings::Defaults();
	}
	Draft = Confirmed;
	// The page itself holds focus only during a key capture; the binding's
	// "Press a key..." is the cue, so no outline is drawn around the page.
	AddMetadata<FWoTSelfDrawnFocusMetaData>(MakeShared<FWoTSelfDrawnFocusMetaData>());

	const FText Title = Retail(Page == EWoTSettingsPage::Controls
		? TEXT("menuOptions.MenuTitle") : TEXT("menuConfiguration.MenuTitle"));
	const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	BodyCheckBoxStyle = *Style.CheckBoxStyle();
	BodyCheckBoxGlyphSize = FWoTMenuStyle::CheckGlyphSizeFor(Body());
	FWoTMenuStyle::SizeCheckBoxImages(BodyCheckBoxStyle, BodyCheckBoxGlyphSize);

	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);
	Panel->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
	[
		SNew(STextBlock)
		.Text(Title)
		.Font(this, &SWoTSettingsScreen::Heading)
		.ColorAndOpacity(FWoTRetailBrowserStyle::GameTextColor())
	];
	// One scroll-box slot per row (the host panel's pattern): D-pad focus can
	// reach every row, onboarding rows included, and each row scrolls into
	// view as it takes focus (spec r3 [I5]).
	Panel->AddSlot().FillHeight(1.0f)
	[
		SAssignNew(BodyScroll, SScrollBox)
		.Style(Style.ScrollBoxStyle())
		.ScrollBarStyle(Style.ScrollBarStyle())
		.ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
	];
	BuildBody();
	// Video confirmation strip: visible only while a preview awaits Keep.
	Panel->AddSlot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		.Visibility_Lambda([this]()
		{
			return VideoConfirmDeadline > 0.0 ? EVisibility::Visible : EVisibility::Collapsed;
		})
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(this, &SWoTSettingsScreen::Body)
			.ColorAndOpacity(FWoTMenuStyle::SelectedColor())
			.AutoWrapText(true)
			.Text_Lambda([this]()
			{
				const int32 Remaining = FMath::Max(0,
					FMath::CeilToInt(VideoConfirmDeadline - LastTickTime));
				return FText::FromString(FString::Printf(
					TEXT("Keep these display settings? Reverting in %d s."), Remaining));
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f)
		[
			SAssignNew(KeepButton, SWoTMenuButton)
			.ContentPadding(FMargin(18.0f, 8.0f))
			.OnClicked(this, &SWoTSettingsScreen::KeepVideo)
			[
				SNew(STextBlock).Text(Literal(TEXT("Keep")))
				.Font(this, &SWoTSettingsScreen::Body)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SAssignNew(RevertButton, SWoTMenuButton)
			.ContentPadding(FMargin(18.0f, 8.0f))
			.OnClicked(this, &SWoTSettingsScreen::RevertVideo)
			[
				SNew(STextBlock).Text(Literal(TEXT("Revert")))
				.Font(this, &SWoTSettingsScreen::Body)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
	];
	// Fixed footer: Save and Back stay visible whatever the body height.
	Panel->AddSlot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SAssignNew(SaveButton, SWoTMenuButton)
			.ContentPadding(FMargin(18.0f, 8.0f))
			.IsEnabled_Lambda([this]()
			{
				return Preferences.IsValid() && VideoConfirmDeadline <= 0.0;
			})
			.OnClicked(this, &SWoTSettingsScreen::Save)
			[
				SNew(STextBlock).Text(Literal(TEXT("Save")))
				.Font(this, &SWoTSettingsScreen::Body)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SAssignNew(BackButton, SWoTMenuButton)
			.ContentPadding(FMargin(18.0f, 8.0f))
			.OnClicked(this, &SWoTSettingsScreen::Back)
			[
				SNew(STextBlock).Text(Literal(TEXT("Back")))
				.Font(this, &SWoTSettingsScreen::Body)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(18.0f, 0.0f)
		[
			SNew(STextBlock)
			.Font(this, &SWoTSettingsScreen::Body)
			.AutoWrapText(true)
			.ColorAndOpacity_Lambda([this]()
			{
				return ErrorText.IsEmpty()
					? FSlateColor(FWoTMenuStyle::SelectedColor())
					: FSlateColor(FLinearColor(1.0f, 0.45f, 0.35f, 1.0f));
			})
			.Text_Lambda([this]()
			{
				return ErrorText.IsEmpty() ? StatusText : ErrorText;
			})
		]
	];

	ChildSlot
	[
		SNew(SBox).Padding(24.0f)
		[
			SNew(SBorder)
			.BorderImage(PresentationAuthority.IsValid() && PresentationAuthority->BrowserStyle()
				? PresentationAuthority->BrowserStyle()->WindowFrame()
				: FCoreStyle::Get().GetBrush("NoBorder"))
			.Padding(14.0f)
			[
				Panel
			]
		]
	];

	// The page's focus graph: every body row in order, then Save and Back,
	// wrapping top to bottom (spec r3 section 1a, Controls / Hardware).
	NavigationRows.Add({SaveButton, BackButton});
	WoTWrapGridNavigation(NavigationRows);
	// The video confirmation strip keeps focus until Keep or Revert.
	const TArray<TSharedPtr<SWidget>> StripButtons = {KeepButton, RevertButton};
	WoTWrapHorizontalNavigation(StripButtons, /*bHoldVertical*/ true);
	// Registered above the host window's entry: B cancels a live key capture
	// first, and on the video strip B reverts and returns focus to Save.
	CaptureBackEntry = FWoTBackStack::Get().Register(AsShared(),
		[this]() { return IsCapturingBinding(); },
		[this]() { CancelCapture(); return true; });
	VideoStripBackEntry = FWoTBackStack::Get().Register(AsShared(),
		[this]() { return VideoConfirmDeadline > 0.0; },
		[this]()
		{
			RevertVideo();
			FocusSaveAfterStrip(/*bAlways*/ true);
			return true;
		});
}

TSharedPtr<SWidget> SWoTSettingsScreen::InitialControllerFocusWidget() const
{
	return !NavigationRows.IsEmpty() && !NavigationRows[0].IsEmpty()
		? NavigationRows[0][0] : SaveButton;
}

TArray<FWoTButtonPrompt> SWoTSettingsScreen::ControllerPrompts() const
{
	if (SWoTListCombo::IsAnyOpenWithin(AsShared()))
	{
		return FWoTButtonPromptSets::Dropdown();
	}
	if (IsCapturingBinding())
	{
		return FWoTButtonPromptSets::Capture();
	}
	if (VideoConfirmDeadline > 0.0)
	{
		return FWoTButtonPromptSets::KeepRevert();
	}
	return FWoTButtonPromptSets::Settings();
}

void SWoTSettingsScreen::FocusSaveAfterStrip(bool bAlways)
{
	if (!FSlateApplication::IsInitialized() || !SaveButton.IsValid())
	{
		return;
	}
	FSlateApplication& SlateApp = FSlateApplication::Get();
	const TSharedPtr<SWidget> Focused = SlateApp.GetKeyboardFocusedWidget();
	if (bAlways || (Focused.IsValid()
		&& (Focused == KeepButton || Focused == RevertButton)))
	{
		SlateApp.SetKeyboardFocus(SaveButton, EFocusCause::SetDirectly);
	}
}

SWoTSettingsScreen::~SWoTSettingsScreen()
{
	const bool bBrightnessPreviewLive =
		!FMath::IsNearlyEqual(Draft.Brightness, Confirmed.Brightness);
	// A screen torn down mid-preview never leaves the display unconfirmed.
	if (bVideoPreviewLive)
	{
		FWoTGameSettingsAdapter::RevertVideo(Confirmed);
		bVideoPreviewLive = false;
	}
	else if (bBrightnessPreviewLive)
	{
		FWoTGameSettingsAdapter::ApplyBrightness(Confirmed.Brightness);
	}
}

FText SWoTSettingsScreen::Retail(const TCHAR* Key) const
{
	if (TextResolver.IsBound())
	{
		const FText Resolved = TextResolver.Execute(FName(Key));
		if (!Resolved.IsEmpty()) return Resolved;
	}
	if (PresentationAuthority.IsValid())
	{
		const FText Authenticated = PresentationAuthority->Text(FName(Key));
		if (!Authenticated.IsEmpty()) return Authenticated;
	}
	return FWoTMenuStyle::Get().RetailText(FName(Key));
}

FText SWoTSettingsScreen::ActionLabel(EWoTManagedBinding Action) const
{
	// Retail menuKeyboard names where the original had the action; the
	// remaining DefaultInput.ini actions carry plain names, never identifiers.
	switch (Action)
	{
	case EWoTManagedBinding::Fire: return Retail(TEXT("menuKeyboard.MenuList[1]"));
	case EWoTManagedBinding::Jump: return Retail(TEXT("menuKeyboard.MenuList[2]"));
	case EWoTManagedBinding::MoveForward: return Retail(TEXT("menuKeyboard.MenuList[3]"));
	case EWoTManagedBinding::MoveBackward: return Retail(TEXT("menuKeyboard.MenuList[4]"));
	case EWoTManagedBinding::MoveLeft: return Retail(TEXT("menuKeyboard.MenuList[5]"));
	case EWoTManagedBinding::MoveRight: return Retail(TEXT("menuKeyboard.MenuList[6]"));
	case EWoTManagedBinding::TurnLeft: return Retail(TEXT("menuKeyboard.MenuList[7]"));
	case EWoTManagedBinding::TurnRight: return Retail(TEXT("menuKeyboard.MenuList[8]"));
	case EWoTManagedBinding::NextItem: return Literal(TEXT("Next Item"));
	case EWoTManagedBinding::PrevItem: return Literal(TEXT("Previous Item"));
	case EWoTManagedBinding::NextHand: return Retail(TEXT("menuKeyboard.MenuList[9]"));
	case EWoTManagedBinding::PrevHand: return Retail(TEXT("menuKeyboard.MenuList[10]"));
	case EWoTManagedBinding::Drop: return Retail(TEXT("menuKeyboard.MenuList[11]"));
	case EWoTManagedBinding::DropTainted: return Literal(TEXT("Drop Tainted Items"));
	case EWoTManagedBinding::Look: return Retail(TEXT("menuKeyboard.MenuList[12]"));
	case EWoTManagedBinding::LookUp: return Retail(TEXT("menuKeyboard.MenuList[13]"));
	case EWoTManagedBinding::LookDown: return Retail(TEXT("menuKeyboard.MenuList[14]"));
	case EWoTManagedBinding::CenterView: return Retail(TEXT("menuKeyboard.MenuList[15]"));
	case EWoTManagedBinding::Walking: return Retail(TEXT("menuKeyboard.MenuList[16]"));
	case EWoTManagedBinding::Strafe: return Retail(TEXT("menuKeyboard.MenuList[17]"));
	case EWoTManagedBinding::FreeLook: return Literal(TEXT("Free Look"));
	case EWoTManagedBinding::LookToggle: return Literal(TEXT("Toggle Mouse Look"));
	case EWoTManagedBinding::ViewUp: return Literal(TEXT("HUD Detail Up"));
	case EWoTManagedBinding::ViewDown: return Literal(TEXT("HUD Detail Down"));
	case EWoTManagedBinding::ShowScores: return Literal(TEXT("Scoreboard"));
	case EWoTManagedBinding::ShowInventoryInfo: return Literal(TEXT("Item Info"));
	case EWoTManagedBinding::InGameMenu: return Literal(TEXT("Game Menu"));
	case EWoTManagedBinding::Type: return Literal(TEXT("Type Chat"));
	case EWoTManagedBinding::Talk: return Literal(TEXT("All Chat"));
	case EWoTManagedBinding::TeamTalk: return Literal(TEXT("Team Chat"));
	case EWoTManagedBinding::SShot: return Literal(TEXT("Screenshot"));
	case EWoTManagedBinding::ToggleFullscreen: return Literal(TEXT("Toggle Fullscreen"));
	case EWoTManagedBinding::ServerEditCitadel: return Literal(TEXT("Edit Citadel"));
	case EWoTManagedBinding::ShowHelp: return Literal(TEXT("Help"));
	case EWoTManagedBinding::PlayTaunt1: return Literal(TEXT("Taunt 1"));
	case EWoTManagedBinding::PlayTaunt2: return Literal(TEXT("Taunt 2"));
	case EWoTManagedBinding::PlayTaunt3: return Literal(TEXT("Taunt 3"));
	case EWoTManagedBinding::PlayTaunt4: return Literal(TEXT("Taunt 4"));
	case EWoTManagedBinding::PlayTaunt5: return Literal(TEXT("Taunt 5"));
	case EWoTManagedBinding::PlayTaunt6: return Literal(TEXT("Taunt 6"));
	case EWoTManagedBinding::PlayTauntRandom: return Literal(TEXT("Random Taunt"));
	case EWoTManagedBinding::Brightness: return Literal(TEXT("Brightness"));
	case EWoTManagedBinding::Pause: return Literal(TEXT("Pause"));
	case EWoTManagedBinding::SelectHand1: return Literal(TEXT("Select Hand 1"));
	case EWoTManagedBinding::SelectHand2: return Literal(TEXT("Select Hand 2"));
	case EWoTManagedBinding::SelectHand3: return Literal(TEXT("Select Hand 3"));
	case EWoTManagedBinding::SelectHand4: return Literal(TEXT("Select Hand 4"));
	case EWoTManagedBinding::SelectHand5: return Literal(TEXT("Select Hand 5"));
	case EWoTManagedBinding::SelectHand6: return Literal(TEXT("Select Hand 6"));
	case EWoTManagedBinding::SelectHand7: return Literal(TEXT("Select Hand 7"));
	case EWoTManagedBinding::SelectHand8: return Literal(TEXT("Select Hand 8"));
	case EWoTManagedBinding::SelectHand9: return Literal(TEXT("Select Hand 9"));
	case EWoTManagedBinding::SelectHand0: return Literal(TEXT("Select Hand 10"));
	default: return FText::FromString(FWoTGameSettings::ActionName(Action));
	}
}

FText SWoTSettingsScreen::BindingText(EWoTManagedBinding Action, EWoTBindingSlot Slot) const
{
	if (CaptureAction.IsSet() && CaptureAction.GetValue() == Action && CaptureSlot == Slot)
	{
		return Literal(TEXT("Press a key..."));
	}
	const FWoTManagedKeyBinding* Binding = Draft.FindBinding(Action);
	if (!Binding) return FText::GetEmpty();
	const FKey& Key = Slot == EWoTBindingSlot::Primary ? Binding->Primary : Binding->Secondary;
	// "None" rather than "-": the Hyphen key's own display name is "-".
	return Key.IsValid() ? Key.GetDisplayName(false) : Literal(TEXT("None"));
}

TSharedRef<SWidget> SWoTSettingsScreen::BuildLabel(const FText& Text)
{
	return SNew(STextBlock)
		.Text(Text)
		.Font(this, &SWoTSettingsScreen::Body)
		.ColorAndOpacity(FWoTRetailBrowserStyle::GameTextColor());
}

TSharedRef<SWidget> SWoTSettingsScreen::BuildChoiceRow(
	const FText& Label, TSharedRef<SWidget> Control,
	TSharedPtr<SWidget>* LabelCapture)
{
	const TSharedRef<SWidget> LabelWidget = BuildLabel(Label);
	if (LabelCapture != nullptr) *LabelCapture = LabelWidget;
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.60f).VAlign(VAlign_Center)
		[
			LabelWidget
		]
		+ SHorizontalBox::Slot().FillWidth(0.40f).VAlign(VAlign_Center)
		[
			Control
		];
}

void SWoTSettingsScreen::BuildBody()
{
	if (!Preferences.IsValid())
	{
		BodyScroll->AddSlot()
		[
			SNew(STextBlock)
			.Text(Literal(TEXT("Settings are unavailable until the game has finished loading.")))
			.Font(this, &SWoTSettingsScreen::Body)
			.AutoWrapText(true)
		];
		return;
	}
	if (Page == EWoTSettingsPage::Controls)
	{
		BuildControlsBody();
	}
	else
	{
		BuildHardwareBody();
	}
}

TSharedRef<SWidget> SWoTSettingsScreen::BuildBindingButton(
	EWoTManagedBinding Action, EWoTBindingSlot Slot)
{
	return SNew(SWoTMenuButton)
		.ContentPadding(FMargin(10.0f, 4.0f))
		.HAlign(HAlign_Center)
		.OnClicked_Lambda([this, Action, Slot]()
		{
			if (IsCapturingBinding()) { CancelCapture(); }
			BeginCapture(Action, Slot);
			return FReply::Handled();
		})
		[
			SNew(STextBlock)
			.Font(this, &SWoTSettingsScreen::Body)
			.ColorAndOpacity(FSlateColor::UseForeground())
			.Text_Lambda([this, Action, Slot]() { return BindingText(Action, Slot); })
		];
}

TSharedRef<SWidget> SWoTSettingsScreen::BuildBindingRow(EWoTManagedBinding Action)
{
	const TSharedRef<SWidget> PrimaryButton =
		BuildBindingButton(Action, EWoTBindingSlot::Primary);
	const TSharedRef<SWidget> SecondaryButton =
		BuildBindingButton(Action, EWoTBindingSlot::Secondary);
	NavigationRows.Add({PrimaryButton, SecondaryButton});
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.44f).VAlign(VAlign_Center)
		[
			BuildLabel(ActionLabel(Action))
		]
		+ SHorizontalBox::Slot().FillWidth(0.28f).Padding(4.0f, 0.0f)
		[
			PrimaryButton
		]
		+ SHorizontalBox::Slot().FillWidth(0.28f).Padding(4.0f, 0.0f)
		[
			SecondaryButton
		];
}

void SWoTSettingsScreen::BuildControlsBody()
{
	const TSharedRef<SScrollBox> Rows = BodyScroll.ToSharedRef();
	TSharedPtr<SWidget> MouseSensitivitySlider;
	// Mouse sensitivity: retail slider over eight authored steps.
	Rows->AddSlot()[BuildLabel(Retail(TEXT("menuOptions.MenuList[3]")))];
	Rows->AddSlot().Padding(0.0f, 8.0f, 0.0f, 22.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SAssignNew(MouseSensitivitySlider, SWoTRetailSlider)
			.MinValue(0.0f)
			.StepSize(0.125f)
			.Value_Lambda([this]()
			{
				return SensitivityIndex(Draft.MouseSensitivity) / 7.0f;
			})
			.OnValueChanged_Lambda([this](float Value)
			{
				Draft.MouseSensitivity = SensitivitySteps[CellIndexFromSliderValue(Value)];
				SetError(FText::GetEmpty());
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f)
		[
			SNew(SWoTSliderValueLabel)
			.Font(this, &SWoTSettingsScreen::Body)
			.Text_Lambda([this]()
			{
				return FText::FromString(FString::Printf(TEXT("%gx"), Draft.MouseSensitivity));
			})
		]
	];
	NavigationRows.Add({MouseSensitivitySlider});
	TSharedPtr<SWidget> InvertMouseCheckBox;
	Rows->AddSlot().Padding(0.0f, 0.0f, 0.0f, 22.0f)
	[
		SAssignNew(InvertMouseCheckBox, SCheckBox)
		.Style(&BodyCheckBoxStyle)
		.IsChecked_Lambda([this]()
		{
			return Draft.bInvertMouseY ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			Draft.bInvertMouseY = State == ECheckBoxState::Checked;
		})
		[
			BuildLabel(Retail(TEXT("menuOptions.MenuList[2]")))
		]
	];
	NavigationRows.Add({InvertMouseCheckBox});
	auto AddFlag = [this, &Rows](const TCHAR* RetailKey,
		bool FWoTGameSettings::* Field,
		bool FWoTGameSettings::* Exclusive = nullptr)
	{
		TSharedPtr<SWidget> FlagCheckBox;
		Rows->AddSlot().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SAssignNew(FlagCheckBox, SCheckBox).Style(&BodyCheckBoxStyle)
			.IsChecked_Lambda([this, Field]() { return Draft.*Field
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Field, Exclusive](ECheckBoxState State)
			{
				Draft.*Field = State == ECheckBoxState::Checked;
				if (Draft.*Field && Exclusive) { Draft.*Exclusive = false; }
			})
			[ BuildLabel(Retail(RetailKey)) ]
		];
		NavigationRows.Add({FlagCheckBox});
	};
	AddFlag(TEXT("menuOptions.MenuList[4]"), &FWoTGameSettings::bAlwaysMouseLook,
		&FWoTGameSettings::bAutoSlopeLook);
	AddFlag(TEXT("menuOptions.MenuList[5]"), &FWoTGameSettings::bAutoSlopeLook,
		&FWoTGameSettings::bAlwaysMouseLook);
	AddFlag(TEXT("menuOptions.MenuList[6]"), &FWoTGameSettings::bLookSpring);
	AddFlag(TEXT("menuOptions.MenuList[9]"), &FWoTGameSettings::bJoystickEnabled);
	// Controller look: the mouse's sensitivity steps and its own invert.
	Rows->AddSlot()[BuildLabel(Literal(TEXT("Controller Sensitivity")))];
	Rows->AddSlot().Padding(0.0f, 8.0f, 0.0f, 22.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SAssignNew(ControllerSensitivitySlider, SWoTRetailSlider)
			.MinValue(0.0f)
			.StepSize(0.125f)
			.Value_Lambda([this]()
			{
				return SensitivityIndex(Draft.ControllerLookSensitivity,
					ControllerSensitivitySteps) / 7.0f;
			})
			.OnValueChanged_Lambda([this](float Value)
			{
				Draft.ControllerLookSensitivity =
					ControllerSensitivitySteps[CellIndexFromSliderValue(Value)];
				SetError(FText::GetEmpty());
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f)
		[
			SNew(SWoTSliderValueLabel)
			.Font(this, &SWoTSettingsScreen::Body)
			.Text_Lambda([this]()
			{
				return FText::FromString(FString::Printf(TEXT("%gx"), Draft.ControllerLookSensitivity));
			})
		]
	];
	NavigationRows.Add({ControllerSensitivitySlider});
	Rows->AddSlot().Padding(0.0f, 0.0f, 0.0f, 12.0f)
	[
		SAssignNew(ControllerInvertCheckBox, SCheckBox)
		.Style(&BodyCheckBoxStyle)
		.IsChecked_Lambda([this]()
		{
			return Draft.bControllerInvertY ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			Draft.bControllerInvertY = State == ECheckBoxState::Checked;
		})
		[
			BuildLabel(Literal(TEXT("Invert Controller Look")))
		]
	];
	NavigationRows.Add({ControllerInvertCheckBox});
	TSharedPtr<SWidget> ViewBobSlider;
	Rows->AddSlot()[BuildLabel(Retail(TEXT("menuOptions.MenuList[8]")))];
	Rows->AddSlot().Padding(0.0f, 6.0f, 0.0f, 16.0f)
	[
		SAssignNew(ViewBobSlider, SWoTRetailSlider).MinValue(0.0f).StepSize(0.125f)
		.Value_Lambda([this]() { return Draft.ViewBob / 0.032f; })
		.OnValueChanged_Lambda([this](float Value)
			{ Draft.ViewBob = FMath::RoundToFloat(Value * 8.0f) * 0.004f; })
	];
	NavigationRows.Add({ViewBobSlider});
	TSharedPtr<SWidget> CrosshairSlider;
	Rows->AddSlot()[BuildLabel(Retail(TEXT("menuOptions.MenuList[7]")))];
	Rows->AddSlot().Padding(0.0f, 6.0f, 0.0f, 16.0f)
	[
		SAssignNew(CrosshairSlider, SWoTRetailSlider).MinValue(0.0f).StepSize(0.05f)
		.Value_Lambda([this]() { return Draft.Crosshair / 20.0f; })
		.OnValueChanged_Lambda([this](float Value)
			{ Draft.Crosshair = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 20.0f), 0, 20)); })
	];
	NavigationRows.Add({CrosshairSlider});
	TSharedPtr<SWidget> GoreSlider;
	Rows->AddSlot()[BuildLabel(Retail(TEXT("menuOptions.MenuList[10]")))];
	Rows->AddSlot().Padding(0.0f, 6.0f, 0.0f, 18.0f)
	[
		SAssignNew(GoreSlider, SWoTRetailSlider).MinValue(0.0f).StepSize(1.0f / 3.0f)
		.Value_Lambda([this]() { return Draft.GoreDetailLevel / 3.0f; })
		.OnValueChanged_Lambda([this](float Value)
			{ Draft.GoreDetailLevel = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 3.0f), 0, 3)); })
	];
	NavigationRows.Add({GoreSlider});
	AddFlag(TEXT("menuOptions.MenuList[11]"), &FWoTGameSettings::bPlayerIDEnabled);
	Rows->AddSlot().Padding(0.0f, 0.0f, 0.0f, 8.0f)
	[
		BuildLabel(Retail(TEXT("menuOptions.MenuList[1]")))
	];
	for (const EWoTManagedBinding Action : FWoTGameSettings::ManagedActions())
	{
		Rows->AddSlot().Padding(0.0f, 3.0f)[BuildBindingRow(Action)];
	}
	TSharedPtr<SWidget> ResetBindingsButton;
	Rows->AddSlot().Padding(0.0f, 14.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
	[
		SAssignNew(ResetBindingsButton, SWoTMenuButton)
		.ContentPadding(FMargin(18.0f, 8.0f))
		.OnClicked(this, &SWoTSettingsScreen::ResetBindings)
		[
			SNew(STextBlock)
			.Text(Retail(TEXT("menuKeyboard.MenuList[18]")))
			.Font(this, &SWoTSettingsScreen::Body)
			.ColorAndOpacity(FSlateColor::UseForeground())
		]
	];
	NavigationRows.Add({ResetBindingsButton});
	// Extension rows from the onboarding seam registry (the optional plugin's
	// settings toggles). Registered rows render as one appended group; an
	// empty registry appends nothing, so a build without the plugin renders
	// exactly the rows above. Labels resolve lazily — a row's text follows
	// its live setting — and the callbacks are copied at build time so the
	// screen never reads a registry that changed under it.
	const TArray<FWoTOnboardingRow>& ExtensionRows =
		FWoTOnboardingSeams::Get().SettingsRows;
	if (!ExtensionRows.IsEmpty())
	{
		Rows->AddSlot().Padding(0.0f, 22.0f, 0.0f, 8.0f)
		[
			BuildLabel(Literal(TEXT("ONBOARDING")))
		];
		for (const FWoTOnboardingRow& Row : ExtensionRows)
		{
			TSharedPtr<SWidget> ExtensionButton;
			Rows->AddSlot().Padding(0.0f, 3.0f).HAlign(HAlign_Left)
			[
				SAssignNew(ExtensionButton, SWoTMenuButton)
				.ContentPadding(FMargin(10.0f, 4.0f))
				.OnClicked_Lambda([Invoke = Row.Invoke]()
				{
					if (Invoke)
					{
						Invoke();
					}
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Font(this, &SWoTSettingsScreen::Body)
					.ColorAndOpacity(FSlateColor::UseForeground())
					.Text_Lambda([Label = Row.Label]()
					{
						return Label ? FText::FromString(Label())
						             : FText::GetEmpty();
					})
				]
			];
			NavigationRows.Add({ExtensionButton});
		}
	}
}

void SWoTSettingsScreen::BuildHardwareBody()
{
	const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	const FWoTEarlyUILimits Limits = FWoTEarlyUILimits::Production();
	ResolutionItems.Reset();
	bool bHasDraftResolution = false;
	for (const auto& Candidate : CommonResolutions)
	{
		if (Candidate[0] < Limits.MinWidth || Candidate[0] > Limits.MaxWidth
			|| Candidate[1] < Limits.MinHeight || Candidate[1] > Limits.MaxHeight)
		{
			continue;
		}
		ResolutionItems.Add(MakeShared<FString>(ResolutionLabel(Candidate[0], Candidate[1])));
		bHasDraftResolution = bHasDraftResolution
			|| (Candidate[0] == Draft.Width && Candidate[1] == Draft.Height);
	}
	if (!bHasDraftResolution)
	{
		ResolutionItems.Insert(MakeShared<FString>(ResolutionLabel(Draft.Width, Draft.Height)), 0);
	}
	WindowModeItems = {MakeShared<FString>(TEXT("Windowed")),
		MakeShared<FString>(TEXT("Windowed Fullscreen")), MakeShared<FString>(TEXT("Fullscreen"))};
	QualityItems = {MakeShared<FString>(TEXT("Low")), MakeShared<FString>(TEXT("Medium")),
		MakeShared<FString>(TEXT("High")), MakeShared<FString>(TEXT("Epic"))};
	FrameRateItems.Reset();
	for (const uint16 Step : FrameRateSteps)
	{
		FrameRateItems.Add(MakeShared<FString>(Step == 0
			? FString(TEXT("Unlimited")) : FString::Printf(TEXT("%u"), Step)));
	}
	BrightnessItems.Reset();
	for (int32 Step = 0; Step < 10; ++Step)
	{
		BrightnessItems.Add(MakeShared<FString>(FString::Printf(
			TEXT("Display brightness: %d of 10"), Step + 1)));
	}

	// The shared dropdown: the touch-friendly list (spec r3 section 2). D-pad
	// Up/Down move focus off it while closed and never change the value, so
	// a controller can no longer apply a brightness change by passing over it.
	auto MakeCombo = [this, &Style](TArray<TSharedPtr<FString>>* Items,
		TFunction<int32()> Selected, TFunction<void(int32)> Commit) -> TSharedRef<SWidget>
	{
		return SNew(SWoTListCombo)
			.ComboBoxStyle(Style.ComboBoxStyle())
			// The live frame brush: the combo style holds a copy of it that
			// is not rebound when the menu textures are.
			.MenuBorderBrush(Style.Frame())
			.ItemStyle(Style.TableRowStyle())
			.ScrollBarStyle(Style.ScrollBarStyle())
			.ForegroundColor(FWoTMenuStyle::MenuColor(false))
			.NumItems_Lambda([Items]() { return Items->Num(); })
			.SelectedIndex_Lambda([Selected]() { return Selected(); })
			.OnGenerateItem_Lambda([this, Items](int32 Index) -> TSharedRef<SWidget>
			{
				const TSharedPtr<FString> Item =
					Items->IsValidIndex(Index) ? (*Items)[Index] : nullptr;
				return SNew(STextBlock)
					.Text(FText::FromString(Item.IsValid() ? *Item : FString()))
					.Font(this, &SWoTSettingsScreen::Body);
			})
			.OnPicked_Lambda([Items, Commit](int32 Index, ESelectInfo::Type)
			{
				if (Items->IsValidIndex(Index)) Commit(Index);
			})
			[
				SNew(STextBlock)
				.Font(this, &SWoTSettingsScreen::Body)
				.Text_Lambda([Items, Selected]()
				{
					const int32 Index = Selected();
					return FText::FromString(Items->IsValidIndex(Index) ? *(*Items)[Index] : FString());
				})
			];
	};

	const TSharedRef<SScrollBox> Rows = BodyScroll.ToSharedRef();
	auto Navigable = [this](const TSharedRef<SWidget>& Control) -> TSharedRef<SWidget>
	{
		NavigationRows.Add({Control});
		return Control;
	};
	Rows->AddSlot().Padding(0.0f, 4.0f)
	[
		BuildChoiceRow(Retail(TEXT("menuConfiguration.MenuList[1]")),
			Navigable(MakeCombo(&BrightnessItems,
				[this]() { return FMath::Clamp(FMath::RoundToInt(Draft.Brightness * 10.0f), 0, 9); },
				[this](int32 Index)
				{
					Draft.Brightness = static_cast<float>(Index) / 10.0f;
					FWoTGameSettingsAdapter::ApplyBrightness(Draft.Brightness);
				})))
	];
	WindowModeControl = MakeCombo(&WindowModeItems,
		[this]() { return static_cast<int32>(Draft.WindowMode); },
		[this](int32 Index) { Draft.WindowMode = static_cast<EWoTWindowMode>(Index); });
	Rows->AddSlot().Padding(0.0f, 4.0f)
	[
		BuildChoiceRow(Retail(TEXT("menuConfiguration.MenuList[3]")),
			Navigable(MakeCombo(&ResolutionItems,
				[this]()
				{
					const FString Current = ResolutionLabel(Draft.Width, Draft.Height);
					return ResolutionItems.IndexOfByPredicate(
						[&Current](const TSharedPtr<FString>& Item) { return *Item == Current; });
				},
				[this](int32 Index)
				{
					FString Width, Height;
					if ((*ResolutionItems[Index]).Split(TEXT(" x "), &Width, &Height))
					{
						Draft.Width = static_cast<uint16>(FCString::Atoi(*Width));
						Draft.Height = static_cast<uint16>(FCString::Atoi(*Height));
					}
				})))
	];
	Rows->AddSlot().Padding(0.0f, 4.0f)
	[
		BuildChoiceRow(Retail(TEXT("menuConfiguration.MenuList[2]")),
			Navigable(WindowModeControl.ToSharedRef()), &WindowModeLabel)
	];
	TSharedPtr<SWidget> VSyncCheckBox;
	Rows->AddSlot().Padding(0.0f, 8.0f)
	[
		SAssignNew(VSyncCheckBox, SCheckBox)
		.Style(&BodyCheckBoxStyle)
		.IsChecked_Lambda([this]()
		{
			return Draft.bVSync ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
		{
			Draft.bVSync = State == ECheckBoxState::Checked;
		})
		[
			BuildLabel(Retail(TEXT("menuConfiguration.MenuList[5]")))
		]
	];
	NavigationRows.Add({VSyncCheckBox});
	Rows->AddSlot().Padding(0.0f, 4.0f)
	[
		BuildChoiceRow(Retail(TEXT("menuConfiguration.MenuList[6]")),
			Navigable(MakeCombo(&QualityItems,
				[this]() { return static_cast<int32>(Draft.GraphicsQuality); },
				[this](int32 Index) { Draft.GraphicsQuality = static_cast<EWoTGraphicsQuality>(Index); })))
	];
	Rows->AddSlot().Padding(0.0f, 4.0f)
	[
		BuildChoiceRow(Literal(TEXT("Frame Rate Limit")),
			Navigable(MakeCombo(&FrameRateItems,
				[this]()
				{
					for (int32 Index = 0; Index < UE_ARRAY_COUNT(FrameRateSteps); ++Index)
						if (FrameRateSteps[Index] == Draft.FrameRateLimit) return Index;
					return 0;
				},
				[this](int32 Index) { Draft.FrameRateLimit = FrameRateSteps[Index]; })))
	];
	Rows->AddSlot().Padding(0.0f, 14.0f, 0.0f, 0.0f)
	[
		BuildLabel(Retail(TEXT("menuConfiguration.MenuList[15]")))
	];
	TSharedPtr<SWidget> VolumeSlider;
	Rows->AddSlot().Padding(0.0f, 8.0f, 0.0f, 12.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SAssignNew(VolumeSlider, SWoTRetailSlider)
			.MinValue(0.0f)
			.StepSize(0.125f)
			.Value_Lambda([this]() { return Draft.MasterVolume; })
			.OnValueChanged_Lambda([this](float Value)
			{
				Draft.MasterVolume = FMath::Clamp(Value, 0.0f, 1.0f);
				// Audible preview; Back restores the confirmed volume.
				FWoTGameSettingsAdapter::ApplyAudio(Draft);
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f)
		[
			SNew(SWoTSliderValueLabel)
			.Font(this, &SWoTSettingsScreen::Body)
			.Text_Lambda([this]()
			{
				return FText::FromString(FString::Printf(TEXT("%d%%"),
					FMath::RoundToInt(Draft.MasterVolume * 100.0f)));
			})
		]
	];
	NavigationRows.Add({VolumeSlider});
}

void SWoTSettingsScreen::BeginCapture(EWoTManagedBinding Action, EWoTBindingSlot Slot)
{
	CaptureAction = Action;
	CaptureSlot = Slot;
	SetError(FText::GetEmpty());
	SetStatus(Literal(TEXT("Press a keyboard or mouse button. Escape cancels.")));
	if (BodyScroll.IsValid()) BodyScroll->SetConsumeMouseWheel(EConsumeMouseWheel::Never);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(AsShared(), EFocusCause::SetDirectly);
	}
}

void SWoTSettingsScreen::CancelCapture()
{
	CaptureAction.Reset();
	if (BodyScroll.IsValid()) BodyScroll->SetConsumeMouseWheel(EConsumeMouseWheel::WhenScrollingPossible);
	SetStatus(FText::GetEmpty());
}

bool SWoTSettingsScreen::CommitCapturedKey(const FKey& Key)
{
	if (!CaptureAction.IsSet()) return false;
	if (!FWoTGameSettings::IsEditableKey(Key))
	{
		SetError(Literal(TEXT("Choose a keyboard or mouse button for this action.")));
		return true;
	}
	FWoTManagedKeyBinding* Binding = Draft.FindBinding(CaptureAction.GetValue());
	if (!Binding) { CancelCapture(); return true; }
	FKey& Target = CaptureSlot == EWoTBindingSlot::Primary ? Binding->Primary : Binding->Secondary;
	const FKey Previous = Target;
	Target = Key;
	FString Error;
	if (!Draft.Validate(Error))
	{
		Target = Previous;
		SetError(FText::FromString(Error));
		CancelCapture();
		return true;
	}
	CancelCapture();
	SetError(FText::GetEmpty());
	return true;
}

FReply SWoTSettingsScreen::ResetBindings()
{
	if (IsCapturingBinding()) CancelCapture();
	Draft.Bindings = FWoTGameSettings::Defaults().Bindings;
	SetError(FText::GetEmpty());
	return FReply::Handled();
}

bool SWoTSettingsScreen::VideoDiffersFromConfirmed() const
{
	return Draft.Width != Confirmed.Width || Draft.Height != Confirmed.Height
		|| Draft.Brightness != Confirmed.Brightness
		|| Draft.WindowMode != Confirmed.WindowMode || Draft.bVSync != Confirmed.bVSync
		|| Draft.GraphicsQuality != Confirmed.GraphicsQuality
		|| Draft.FrameRateLimit != Confirmed.FrameRateLimit;
}

void SWoTSettingsScreen::RevertDraftVideoToConfirmed()
{
	Draft.Width = Confirmed.Width;
	Draft.Brightness = Confirmed.Brightness;
	Draft.Height = Confirmed.Height;
	Draft.WindowMode = Confirmed.WindowMode;
	Draft.bVSync = Confirmed.bVSync;
	Draft.GraphicsQuality = Confirmed.GraphicsQuality;
	Draft.FrameRateLimit = Confirmed.FrameRateLimit;
}

bool SWoTSettingsScreen::CommitDraft()
{
	UWoTPreferencesSubsystem* Owner = Preferences.Get();
	if (!Owner)
	{
		SetError(Literal(TEXT("Settings are unavailable until the game has finished loading.")));
		return false;
	}
	FString Error;
	if (!Draft.Validate(Error))
	{
		SetError(FText::FromString(Error));
		return false;
	}
	const FWoTBootstrapStatus Saved = Owner->SaveGameSettings(Draft);
	if (Saved.IsError())
	{
		// The draft survives so the user can retry; the owner kept its state.
		SetError(Literal(TEXT("Settings could not be saved. Your changes are still shown; try again.")));
		return false;
	}
	Confirmed = Draft;
	SetError(FText::GetEmpty());
	return true;
}

FReply SWoTSettingsScreen::Save()
{
	if (IsCapturingBinding()) CancelCapture();
	if (VideoConfirmDeadline > 0.0) return FReply::Handled();
	if (Page == EWoTSettingsPage::Hardware && VideoDiffersFromConfirmed())
	{
		FString Error;
		if (!FWoTGameSettingsAdapter::PreviewVideo(Draft, Error))
		{
			SetError(FText::FromString(Error));
			return FReply::Handled();
		}
		bVideoPreviewLive = true;
		VideoConfirmDeadline = LastTickTime + VideoConfirmationSeconds;
		SetError(FText::GetEmpty());
		// The strip appears: a controller player lands on Keep.
		if (FWoTControllerUIInput::IsControllerMode() && KeepButton.IsValid()
			&& FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetKeyboardFocus(KeepButton, EFocusCause::SetDirectly);
		}
		return FReply::Handled();
	}
	if (CommitDraft())
	{
		OnSaved.ExecuteIfBound();
	}
	return FReply::Handled();
}

FReply SWoTSettingsScreen::KeepVideo()
{
	if (VideoConfirmDeadline <= 0.0) return FReply::Handled();
	VideoConfirmDeadline = 0.0;
	if (CommitDraft())
	{
		bVideoPreviewLive = false;
		// The strip collapses: focus on Keep or Revert returns to Save. Before
		// OnSaved, which may leave the page.
		FocusSaveAfterStrip(/*bAlways*/ false);
		OnSaved.ExecuteIfBound();
		return FReply::Handled();
	}
	// A failed save never leaves the display on unconfirmed settings.
	FWoTGameSettingsAdapter::RevertVideo(Confirmed);
	bVideoPreviewLive = false;
	RevertDraftVideoToConfirmed();
	FocusSaveAfterStrip(/*bAlways*/ false);
	return FReply::Handled();
}

FReply SWoTSettingsScreen::RevertVideo()
{
	if (VideoConfirmDeadline <= 0.0 && !bVideoPreviewLive) return FReply::Handled();
	VideoConfirmDeadline = 0.0;
	FWoTGameSettingsAdapter::RevertVideo(Confirmed);
	bVideoPreviewLive = false;
	RevertDraftVideoToConfirmed();
	SetStatus(Literal(TEXT("Display settings reverted.")));
	// The strip collapses; focus inside it moves to Save.
	FocusSaveAfterStrip(/*bAlways*/ false);
	return FReply::Handled();
}

bool SWoTSettingsScreen::HandleBack()
{
	if (IsCapturingBinding())
	{
		CancelCapture();
		return true;
	}
	const bool bBrightnessPreviewLive =
		!FMath::IsNearlyEqual(Draft.Brightness, Confirmed.Brightness);
	const bool bRevertVideoPreview =
		VideoConfirmDeadline > 0.0 || bVideoPreviewLive;
	if (bRevertVideoPreview)
	{
		RevertVideo();
	}
	else if (bBrightnessPreviewLive)
	{
		FWoTGameSettingsAdapter::ApplyBrightness(Confirmed.Brightness);
	}
	if (Page == EWoTSettingsPage::Hardware
		&& !FMath::IsNearlyEqual(Draft.MasterVolume, Confirmed.MasterVolume))
	{
		FWoTGameSettingsAdapter::ApplyAudio(Confirmed);
	}
	Draft = Confirmed;
	OnBack.ExecuteIfBound();
	return true;
}

FReply SWoTSettingsScreen::Back()
{
	HandleBack();
	return FReply::Handled();
}

FReply SWoTSettingsScreen::OnPreviewKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (IsCapturingBinding())
	{
		if (InKeyEvent.GetKey() == EKeys::Escape)
		{
			CancelCapture();
			return FReply::Handled();
		}
		CommitCapturedKey(InKeyEvent.GetKey());
		return FReply::Handled();
	}
	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTSettingsScreen::OnKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (IsCapturingBinding())
	{
		if (InKeyEvent.GetKey() == EKeys::Escape) { CancelCapture(); }
		else { CommitCapturedKey(InKeyEvent.GetKey()); }
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FNavigationReply SWoTSettingsScreen::OnNavigation(
	const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	// A live key capture holds focus: a held D-pad's repeat cannot wander off.
	if (IsCapturingBinding())
	{
		return FNavigationReply::Stop();
	}
	return SCompoundWidget::OnNavigation(MyGeometry, InNavigationEvent);
}

FReply SWoTSettingsScreen::OnPreviewMouseButtonDown(
	const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (IsCapturingBinding())
	{
		CommitCapturedKey(MouseEvent.GetEffectingButton());
		return FReply::Handled();
	}
	return SCompoundWidget::OnPreviewMouseButtonDown(MyGeometry, MouseEvent);
}

FReply SWoTSettingsScreen::OnMouseWheel(
	const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (IsCapturingBinding())
	{
		CommitCapturedKey(MouseEvent.GetWheelDelta() > 0.0f
			? EKeys::MouseScrollUp : EKeys::MouseScrollDown);
		return FReply::Handled();
	}
	return SCompoundWidget::OnMouseWheel(MyGeometry, MouseEvent);
}

void SWoTSettingsScreen::Tick(
	const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	LastTickTime = InCurrentTime;
	if (VideoConfirmDeadline > 0.0 && InCurrentTime >= VideoConfirmDeadline)
	{
		RevertVideo();
	}
}

void SWoTSettingsScreen::SetError(const FText& Text)
{
	ErrorText = Text;
}

void SWoTSettingsScreen::SetStatus(const FText& Text)
{
	StatusText = Text;
}

#if WITH_AUTOMATION_TESTS
TArray<float> SWoTSettingsScreen::SensitivityStepsForTesting()
{
	return TArray<float>(SensitivitySteps, UE_ARRAY_COUNT(SensitivitySteps));
}

TArray<float> SWoTSettingsScreen::ControllerSensitivityStepsForTesting()
{
	return TArray<float>(ControllerSensitivitySteps, UE_ARRAY_COUNT(ControllerSensitivitySteps));
}
#endif
