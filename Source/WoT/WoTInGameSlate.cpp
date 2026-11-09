#include "WoTInGameSlate.h"
#include "Assets/WoTRuntimeAssets.h"

#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "InputCoreTypes.h"
#include "Rendering/SlateRenderer.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Text/STextBlock.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTInventory.h"
#include "WoTMenuStyle.h"
#include "WoTPlayerController.h"
#include "WoTPlayerState.h"
#include "WoTCharacter.h"
#include "WoTCitadelGameState.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTGameInstance.h"
#include "WoTFactionCharacters.h"
#include "WoTRetailText.h"
#include "Settings/WoTSettingsSlate.h"
#include "UI/SWoTFocusOutline.h"
#include "UI/SWoTListCombo.h"
#include "UI/SWoTOnScreenKeyboard.h"
#include "UI/SWoTTextFieldActivator.h"
#include "UI/WoTBackStack.h"
#include "UI/WoTControllerUIInput.h"
#include "UI/WoTTextEntry.h"
#include "GameFramework/PlayerState.h"

namespace
{
	FText WoTInGameSlate_Text(const TCHAR* Text)
	{
		return FText::FromString(Text);
	}

	struct FWoTInGameMenuEntry
	{
		EWoTInGameMenuEntry Id;
		FString Label;
	};

	/** The Taunts page: PlayTaunt's argument per entry (-1 picks at random). */
	struct FWoTInGameTauntEntry
	{
		const TCHAR* Label;
		int32 TauntIndex;
	};
	const FWoTInGameTauntEntry WoTInGameSlate_TauntEntries[] = {
		{TEXT("Taunt 1"), 1}, {TEXT("Taunt 2"), 2}, {TEXT("Taunt 3"), 3},
		{TEXT("Taunt 4"), 4}, {TEXT("Taunt 5"), 5}, {TEXT("Taunt 6"), 6},
		{TEXT("Random"), -1}
	};

	/** One wrapping step through a list of Count entries. */
	int32 WoTInGameSlate_WrapStep(int32 Current, int32 Count, int32 Direction)
	{
		const int32 From = Current >= 0 && Current < Count ? Current : 0;
		return (From + (Direction < 0 ? -1 : 1) + Count) % Count;
	}

	FString WoTInGameSlate_KeyLabel(const FWoTGameSettings& Settings,
		EWoTManagedBinding Binding)
	{
		const FWoTManagedKeyBinding* Found = Settings.FindBinding(Binding);
		return Found && Found->Primary.IsValid()
			? Found->Primary.GetDisplayName(false).ToString() : FString(TEXT("-"));
	}

	EWoTPlayerFaction WoTInGameSlate_FactionForController(
		const AWoTPlayerController* Controller)
	{
		const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		const UClass* FactionClass = Pawn ? Pawn->GetClass() : nullptr;
		if (const AWoTPlayerState* State = Controller
			? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
			State && State->FactionClass)
		{
			FactionClass = *State->FactionClass;
		}
		else if (const AWoTCharacter* Character = Cast<AWoTCharacter>(Pawn);
			Character && Character->AppearanceClass)
		{
			FactionClass = *Character->AppearanceClass;
		}
		if (FactionClass && FactionClass->IsChildOf(AWoTForsaken::StaticClass()))
		{
			return EWoTPlayerFaction::Forsaken;
		}
		if (FactionClass && FactionClass->IsChildOf(AWoTHound::StaticClass()))
		{
			return EWoTPlayerFaction::Hound;
		}
		if (FactionClass && FactionClass->IsChildOf(AWoTWhitecloak::StaticClass()))
		{
			return EWoTPlayerFaction::Whitecloak;
		}
		return EWoTPlayerFaction::AesSedai;
	}

	// Talk's and TeamTalk's retail prefills (UE1 Console.Talk / TeamTalk).
	const TCHAR* const WoTInGameSlate_SayPrefix = TEXT("Say ");
	const TCHAR* const WoTInGameSlate_TeamSayPrefix = TEXT("TeamSay ");

	/** Splits a chat line into its Say/TeamSay prefix and the typed text. */
	FString WoTInGameSlate_SplitChatPrefix(const FString& Line, FString& OutPrefix)
	{
		for (const TCHAR* Candidate : {WoTInGameSlate_SayPrefix, WoTInGameSlate_TeamSayPrefix})
		{
			if (Line.StartsWith(Candidate, ESearchCase::IgnoreCase))
			{
				const int32 Length = FCString::Strlen(Candidate);
				OutPrefix = Line.Left(Length);
				return Line.Mid(Length);
			}
		}
		OutPrefix.Reset();
		return Line;
	}

	FText WoTInGameSlate_SkinLabel(EWoTPlayerSkinColor Current, int32 Offset)
	{
		const TArray<FWoTSkinChoice>& Choices = FWoTFrontEndModel::SkinChoices();
		if (Choices.IsEmpty()) return FText::GetEmpty();
		int32 Index = Choices.IndexOfByPredicate(
			[Current](const FWoTSkinChoice& Choice)
			{
				return Choice.Color == Current;
			});
		if (Index == INDEX_NONE) Index = 0;
		Index = (Index + Offset % Choices.Num() + Choices.Num()) % Choices.Num();
		return FText::FromString(Choices[Index].DisplayName);
	}

	TArray<FWoTInGameMenuEntry> WoTInGameSlate_MenuEntries(
		const AWoTPlayerController* Controller,
		const UWoTPreferencesSubsystem* Preferences,
		const FWoTGameSettings* Override = nullptr)
	{
		const FWoTGameSettings Defaults = FWoTGameSettings::Defaults();
		const FWoTGameSettings& Settings = Override ? *Override
			: (Preferences ? Preferences->GetGameSettings() : Defaults);
		TArray<FWoTInGameMenuEntry> Entries = {
			{EWoTInGameMenuEntry::Resume, TEXT("Resume")},
			{EWoTInGameMenuEntry::Scoreboard, FString::Printf(TEXT("Scoreboard (%s)"),
				*WoTInGameSlate_KeyLabel(Settings, EWoTManagedBinding::ShowScores))},
			{EWoTInGameMenuEntry::Help, FString::Printf(TEXT("Help (%s)"),
				*WoTInGameSlate_KeyLabel(Settings, EWoTManagedBinding::ShowHelp))},
			// Chat and taunts by menu (spec r3 section 4): after Help, before
			// any onboarding seam rows and Quit [I9].
			{EWoTInGameMenuEntry::Chat, TEXT("Chat")},
			{EWoTInGameMenuEntry::TeamChat, TEXT("Team Chat")},
			{EWoTInGameMenuEntry::Taunts, TEXT("Taunts")}
		};
		if (Controller && Controller->CanRequestCitadelEdit())
		{
			Entries.Add({EWoTInGameMenuEntry::EditCitadel,
				FString::Printf(TEXT("Edit Citadel (%s)"),
					*WoTInGameSlate_KeyLabel(Settings,
						EWoTManagedBinding::ServerEditCitadel))});
		}
		Entries.Append({
			{EWoTInGameMenuEntry::PlayerConfig, TEXT("Player Config")},
			{EWoTInGameMenuEntry::Controls, TEXT("Controls")},
			{EWoTInGameMenuEntry::Hardware, TEXT("Hardware")},
			{EWoTInGameMenuEntry::Quit, TEXT("Quit to Main Menu")}
		});
		return Entries;
	}
}

void SWoTChatInputOverlay::Construct(const FArguments& InArgs)
{
	PlayerController = InArgs._PlayerController;
	const TOptional<FWoTRequiredMenuFonts> ResolvedFonts =
		FWoTMenuStyle::Get().ResolveRequiredFonts();
	if (ResolvedFonts.IsSet())
	{
		RequiredFonts = MakeShared<FWoTRequiredMenuFonts>(ResolvedFonts.GetValue());
		RequiredFonts->BindWidget(AsShared());
	}
	else
	{
		TSharedRef<FWoTRequiredMenuFonts> Fallback =
			MakeShared<FWoTRequiredMenuFonts>();
		Fallback->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
		RequiredFonts = Fallback;
	}

	// The Say/TeamSay prefill shows as a label; the line keeps it (GetText).
	const FString Typed = WoTInGameSlate_SplitChatPrefix(InArgs._InitialText, Prefix);

	// UE5 action delegates run after the platform has delivered the key's
	// character event, so the T/Y opener cannot arrive at this newly focused
	// widget. Accept the first event unconditionally; a timing heuristic would
	// instead discard a fast user's first actual message character.
	TSharedRef<SEditableText> Editable = SNew(SEditableText)
		.Text(FText::FromString(Typed))
		.Font(RequiredFonts->List)
		.ColorAndOpacity(FLinearColor::White)
		.IsCaretMovedWhenGainFocus(true)
		.SelectAllTextWhenFocused(false)
		.OnKeyCharHandler_Lambda([this](const FGeometry&, const FCharacterEvent& Event)
		{
			// Backspace on an empty line takes the label back into the text,
			// as retail's Backspace over the "Say " prefill did ("Say").
			if (Event.GetCharacter() == TCHAR(8) && !Prefix.IsEmpty() && Input.IsValid()
				&& Input->GetText().IsEmpty())
			{
				const FString Unfolded = Prefix.LeftChop(1);
				Prefix.Reset();
				Input->SetText(FText::FromString(Unfolded));
				Input->GoTo(ETextLocation::EndOfDocument);
				return FReply::Handled();
			}
			return FReply::Unhandled();
		});
	Editable->GoTo(ETextLocation::EndOfDocument);
	Input = Editable;
	const FSlateFontInfo LineFont = RequiredFonts->List;

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			// Hosts the keyboard panel over chat (spec r3 section 3 [I7]).
			SAssignNew(EntryLayer, SWoTTextEntryLayer)
			.Font(LineFont)
			.PanelPadding(FMargin(16.0f, 16.0f, 16.0f, 44.0f))
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				.VAlign(VAlign_Bottom)
				.Padding(2.0f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
					.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f))
					.Padding(FMargin(3.0f, 1.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(WoTInGameSlate_Text(TEXT("(> ")))
							.Font(LineFont)
							.ColorAndOpacity(FLinearColor::White)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							// "Say:" / "Team say:" (owner ruling 2026-11-08).
							SNew(STextBlock)
							.Text(this, &SWoTChatInputOverlay::GetLabel)
							.Visibility_Lambda([this]()
							{
								return Prefix.IsEmpty() ? EVisibility::Collapsed
									: EVisibility::HitTestInvisible;
							})
							.Font(LineFont)
							.ColorAndOpacity(FLinearColor(0.94f, 0.77f, 0.36f, 1.0f))
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SAssignNew(Activator, SWoTTextFieldActivator)
							.Request(MakeEntryRequest())
							[
								Editable
							]
						]
					]
				]
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.0f, 0.0f, 16.0f, 8.0f))
		[
			// The keyboard's prompts while it is open over chat.
			SAssignNew(ButtonPrompts, SWoTButtonPrompts)
			.Prompts_Lambda([this]()
			{
				return FWoTTextEntry::IsActiveWithin(AsShared())
					? FWoTTextEntry::ControllerPrompts() : TArray<FWoTButtonPrompt>();
			})
			.Font(LineFont)
		]
	];
}

FText SWoTChatInputOverlay::GetLabel() const
{
	if (Prefix.Equals(WoTInGameSlate_SayPrefix, ESearchCase::IgnoreCase))
	{
		return WoTInGameSlate_Text(TEXT("Say:"));
	}
	if (Prefix.Equals(WoTInGameSlate_TeamSayPrefix, ESearchCase::IgnoreCase))
	{
		return WoTInGameSlate_Text(TEXT("Team say:"));
	}
	return FText::GetEmpty();
}

FWoTTextEntryRequest SWoTChatInputOverlay::MakeEntryRequest()
{
	FWoTTextEntryRequest Request;
	Request.Title = TAttribute<FText>::CreateLambda([this]()
	{
		const FText Label = GetLabel();
		return Label.IsEmpty() ? WoTInGameSlate_Text(TEXT("Chat"))
			: FText::FromString(Label.ToString().LeftChop(1));
	});
	// The typed text after the prefix; the line sent keeps the prefix.
	Request.MaxLength = AWoTPlayerController::MaxChatMessageLength;
	Request.GetText = [this]() { return TypedText(); };
	Request.SetText = [this](const FString& Text)
	{
		if (Input.IsValid())
		{
			Input->SetText(FText::FromString(Text));
			Input->GoTo(ETextLocation::EndOfDocument);
		}
	};
	const TWeakObjectPtr<AWoTPlayerController> WeakController = PlayerController;
	Request.Commit = [this, WeakController]()
	{
		if (AWoTPlayerController* Controller = WeakController.Get())
		{
			Controller->SubmitChatInput(GetText());
		}
	};
	// Controller chat is not a parity path: the panel suspends chat's input
	// router and routes UI-only; closing restores chat's input mode [I7].
	Request.OnPanelOpened = [WeakController](const TSharedRef<SWidget>& Panel)
	{
		if (AWoTPlayerController* Controller = WeakController.Get())
		{
			Controller->SuspendChatInputForTextEntry(Panel);
		}
	};
	Request.OnPanelClosed = [WeakController]()
	{
		if (AWoTPlayerController* Controller = WeakController.Get())
		{
			Controller->ResumeChatInputAfterTextEntry();
		}
	};
	return Request;
}

bool SWoTChatInputOverlay::BeginTextEntry(int32 UserIndex)
{
	return Activator.IsValid() && Activator->BeginEntry(UserIndex);
}

FString SWoTChatInputOverlay::TypedText() const
{
	return Input.IsValid() ? Input->GetText().ToString() : FString();
}

FReply SWoTChatInputOverlay::HandleKey(const FKey& Key)
{
	AWoTPlayerController* Controller = PlayerController.Get();
	if (!Controller)
	{
		return FReply::Unhandled();
	}
	if (Key == EKeys::Escape)
	{
		Controller->CancelChatInput();
		return FReply::Handled();
	}
	if (Key == EKeys::Enter)
	{
		const FString Line = GetText();
		Controller->SubmitChatInput(Line);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SWoTChatInputOverlay::OnPreviewKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		// The keyboard panel (or Steam's keyboard) takes the keys; Escape
		// cancels it first and chat stays open (spec r3 [M3]).
		if (InKeyEvent.GetKey() == EKeys::Escape && FWoTTextEntry::CancelWithin(AsShared()))
		{
			return FReply::Handled();
		}
		return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}
	const FReply Reply = HandleKey(InKeyEvent.GetKey());
	return Reply.IsEventHandled() ? Reply
		: SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTChatInputOverlay::OnKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}
	const FReply Reply = HandleKey(InKeyEvent.GetKey());
	return Reply.IsEventHandled() ? Reply
		: SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

TSharedPtr<SWidget> SWoTChatInputOverlay::TextWidget() const
{
	return Input;
}

FString SWoTChatInputOverlay::GetText() const
{
	return Prefix + TypedText();
}

void SWoTChatInputOverlay::SetText(const FString& Text)
{
	if (Input.IsValid())
	{
		Input->SetText(FText::FromString(WoTInGameSlate_SplitChatPrefix(Text, Prefix)));
		Input->GoTo(ETextLocation::EndOfDocument);
	}
}

void SWoTInGameMenuOverlay::Construct(const FArguments& InArgs)
{
	PlayerController = InArgs._PlayerController;
	Preferences = InArgs._Preferences;
	PresentationAuthority = InArgs._PresentationAuthority;
	if (PresentationAuthority.IsValid()
		&& !PresentationAuthority->IsReady())
	{
		PresentationAuthority.Reset();
	}
	if (!Preferences.IsValid())
	{
		if (AWoTPlayerController* Controller = PlayerController.Get())
		{
			Preferences = Controller->GetGameInstance()
				? Controller->GetGameInstance()
					->GetSubsystem<UWoTPreferencesSubsystem>()
				: nullptr;
		}
	}
#if WITH_AUTOMATION_TESTS
	RequiredFonts = InArgs._RequiredFontsForTesting;
#endif
	if (!RequiredFonts.IsValid() && PresentationAuthority.IsValid())
	{
		if (const FWoTRequiredMenuFonts* AuthorityFonts =
			PresentationAuthority->RequiredFonts())
		{
			RequiredFonts = MakeShared<FWoTRequiredMenuFonts>(*AuthorityFonts);
		}
	}
	if (!RequiredFonts.IsValid())
	{
		const TOptional<FWoTRequiredMenuFonts> ResolvedFonts =
			FWoTMenuStyle::Get().ResolveRequiredFonts();
		if (!ResolvedFonts.IsSet())
		{
			TSharedRef<FWoTRequiredMenuFonts> Fallback =
				MakeShared<FWoTRequiredMenuFonts>();
			Fallback->Title = FCoreStyle::GetDefaultFontStyle(
				TEXT("Regular"), 30);
			Fallback->List = FCoreStyle::GetDefaultFontStyle(
				TEXT("Regular"), 14);
			RequiredFonts = Fallback;
			bUsingFallbackFonts = true;
		}
		else
		{
			RequiredFonts =
				MakeShared<FWoTRequiredMenuFonts>(ResolvedFonts.GetValue());
		}
	}
	if (!bUsingFallbackFonts)
	{
		RequiredFonts->BindWidget(AsShared());
	}
	if (PresentationAuthority.IsValid())
	{
		PresentationAuthority->BindWidget(AsShared());
	}
	// A modal surface: the controller hides the pointer while it is up, and
	// B/View back out one page at a time like Escape.
	CursorLease = FWoTControllerUIInput::AcquireCursorLease();
	BackStackEntry = FWoTBackStack::Get().Register(AsShared(), nullptr,
		[this]() { return BackOneLevel(); });

	// The focus outline layer draws the controller focus outline on every
	// page; the prompt strip shows in Controller mode only. The text entry
	// layer hosts the keyboard panel over the pages, under the prompt strip.
	ChildSlot
	[
		SNew(SWoTFocusOutlineLayer)
		[
		SNew(SOverlay)
		.Visibility_Lambda([this]()
		{
			return HasUsableFonts()
				? EVisibility::Visible : EVisibility::Collapsed;
		})
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(FWoTMenuStyle::Get().Blackout())
			.ColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.72f))
		]
		+ SOverlay::Slot()
		[
			SAssignNew(TextEntryLayer, SWoTTextEntryLayer)
			.Font(this, &SWoTInGameMenuOverlay::ItemFont)
			[
			SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
			SNew(SBorder)
			.BorderImage(FWoTMenuStyle::Get().Background())
			[
			SNew(SBorder)
			.BorderImage(FWoTMenuStyle::Get().Frame())
			.ForegroundColor(FWoTMenuStyle::MenuColor(false))
			.Padding(36.0f)
			[
				SNew(SBox)
				.WidthOverride_Lambda([this]()
				{
					return ActiveSettingsScreen.IsValid() || bShowingPlayerConfig
						? 760.0f : 420.0f;
				})
				.MaxDesiredHeight(620.0f)
				[
					SAssignNew(ContentHost, SBox)
				]
			]
			]
			]
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(24.0f, 0.0f, 32.0f, 24.0f))
		[
			SAssignNew(ButtonPrompts, SWoTButtonPrompts)
			.Prompts(this, &SWoTInGameMenuOverlay::ControllerPrompts)
			.Font(this, &SWoTInGameMenuOverlay::ItemFont)
		]
		]
	];
	ReturnToMainMenu();
}

TSharedRef<SWidget> SWoTInGameMenuOverlay::BuildMainMenu()
{
	TSharedRef<SVerticalBox> Entries = SNew(SVerticalBox);
	MainMenuButtons.Reset();
	MainMenuEntryIds.Reset();
	MainMenuLabels.Reset();
	const TArray<FWoTInGameMenuEntry> MenuEntries = WoTInGameSlate_MenuEntries(
		PlayerController.Get(), Preferences.Get()
#if WITH_AUTOMATION_TESTS
		, GameSettingsForTesting.IsSet() ? &GameSettingsForTesting.GetValue() : nullptr
#endif
	);
	for (int32 EntryIndex = 0; EntryIndex < MenuEntries.Num(); ++EntryIndex)
	{
		const FWoTInGameMenuEntry Entry = MenuEntries[EntryIndex];
		const uint8 EntryId = static_cast<uint8>(Entry.Id);
		MainMenuEntryIds.Add(EntryId);
		MainMenuLabels.Add(Entry.Label);
		TSharedPtr<SWoTMenuButton> Button;
		Entries->AddSlot().AutoHeight()
		.Padding(TAttribute<FMargin>::CreateLambda(
			[this, EntryIndex]() { return EntryPadding(EntryIndex); }))
		[
			SNew(SBox).HeightOverride_Lambda([this]() { return EntryHeight; })
			[
				SAssignNew(Button, SWoTMenuButton)
				.IsEnabled_Lambda([this]() { return CanInteract(); })
				.ContentPadding(FMargin(28.0f, 0.0f))
				.HAlign(HAlign_Center)
				.IsSelected_Lambda([this, EntryIndex]()
				{
					return SelectedMainMenuIndex == EntryIndex;
				})
				.OnSelected_Lambda([this, EntryIndex]()
				{
					SelectMainMenuEntry(EntryIndex);
				})
				.OnClicked_Lambda([this, EntryId]()
				{
					return ActivateMainMenuEntryById(EntryId);
				})
				[
					SNew(STextBlock)
					.Font(this, &SWoTInGameMenuOverlay::ItemFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
					.Text_Lambda([WeakController = PlayerController, Entry]()
					{
						if (Entry.Id == EWoTInGameMenuEntry::Quit)
						{
							const AWoTPlayerController* Controller =
								WeakController.Get();
							if (Controller && Controller->IsLeavingForMenu())
							{
								return WoTInGameSlate_Text(TEXT("Leaving..."));
							}
						}
						return FText::FromString(Entry.Label);
					})
				]
			]
		];
		MainMenuButtons.Add(Button);
		if (EntryIndex == 0)
		{
			ResumeButton = Button;
		}
	}
	SelectedMainMenuIndex = MainMenuButtons.IsEmpty() ? INDEX_NONE : 0;

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 24.0f)
		[
			SNew(STextBlock)
			.Text(WoTInGameSlate_Text(TEXT("Game Menu")))
			.Font(this, &SWoTInGameMenuOverlay::HeadingFont)
			.ColorAndOpacity(FSlateColor::UseForeground())
			.Justification(ETextJustify::Center)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).HeightOverride_Lambda([this]() { return ListHeight; })
			[
				Entries
			]
		];
}

TSharedRef<SWidget> SWoTInGameMenuOverlay::BuildSettingsPage(
	EWoTSettingsPage Page)
{
	return SAssignNew(ActiveSettingsScreen, SWoTSettingsScreen)
		.Page(Page)
		.Preferences(Preferences.Get())
		.PresentationAuthority(PresentationAuthority)
		.HeadingFont(this, &SWoTInGameMenuOverlay::HeadingFont)
		.BodyFont(this, &SWoTInGameMenuOverlay::ItemFont)
		.TextResolver(FWoTSettingsTextResolver::CreateSP(
			this, &SWoTInGameMenuOverlay::ResolveRetailText))
		.OnBack(FSimpleDelegate::CreateSP(
			this, &SWoTInGameMenuOverlay::ReturnToMainMenu))
		.OnSaved(FSimpleDelegate::CreateSP(
			this, &SWoTInGameMenuOverlay::ReturnToMainMenu));
}

TSharedRef<SWidget> SWoTInGameMenuOverlay::BuildPlayerConfigPage()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 22.0f)
		[
			SNew(STextBlock)
			.Text(ResolveRetailText(FName(TEXT("menuPlayer.MenuTitle"))))
			.Font(this, &SWoTInGameMenuOverlay::HeadingFont)
			.ColorAndOpacity(FSlateColor::UseForeground())
			.Justification(ETextJustify::Center)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 14.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(ResolveRetailText(FName(TEXT("menuPlayer.MenuList[1]"))))
				.Font(this, &SWoTInGameMenuOverlay::ItemFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SWoTTextFieldActivator)
				.Request([this]()
				{
					// The page's Save path commits the pending name, as for
					// keyboard typing (spec r3 section 3).
					FWoTTextEntryRequest Request;
					Request.Title = ResolveRetailText(FName(TEXT("menuPlayer.MenuList[1]")));
					Request.MaxLength = 20;
					Request.GetField = [this]() { return StaticCastSharedPtr<SWidget>(PlayerNameInput); };
					Request.GetText = [this]() { return PendingPlayerName; };
					Request.SetText = [this](const FString& Text)
					{
						PendingPlayerName = FWoTFrontEndModel::FilterPlayerNameInput(Text);
						if (PlayerNameInput.IsValid())
						{
							PlayerNameInput->SetText(FText::FromString(PendingPlayerName));
						}
					};
					Request.Filter = [](const FString& Text)
					{
						return FWoTFrontEndModel::FilterPlayerNameInput(Text);
					};
					return Request;
				}())
				[
					SAssignNew(PlayerNameInput, SEditableText)
					.Text(FText::FromString(PendingPlayerName))
					.Font(this, &SWoTInGameMenuOverlay::ItemFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
					.OnTextChanged_Lambda([this](const FText& Text)
					{
						PendingPlayerName =
							FWoTFrontEndModel::FilterPlayerNameInput(Text.ToString());
					})
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 5.0f)
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
					.Text(ResolveRetailText(FName(TEXT("menuPlayer.MenuList[3]"))))
					.Font(this, &SWoTInGameMenuOverlay::ItemFont)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right)
				[
					SNew(SWoTMenuButton)
					.OnClicked_Lambda([this]()
					{
						const TArray<FWoTSkinChoice>& Choices =
							FWoTFrontEndModel::SkinChoices();
						int32 Current = Choices.IndexOfByPredicate(
							[this](const FWoTSkinChoice& Choice)
							{
								return Choice.Color == PendingSkinColor;
							});
						if (Current == INDEX_NONE) Current = 0;
						PendingSkinColor = Choices[
							(Current - 1 + Choices.Num()) % Choices.Num()].Color;
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return WoTInGameSlate_SkinLabel(
							PendingSkinColor, -1); })
						.Font(this, &SWoTInGameMenuOverlay::ItemFont)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(18.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return WoTInGameSlate_SkinLabel(
						PendingSkinColor, 0); })
					.Font(this, &SWoTInGameMenuOverlay::ItemFont)
					.ColorAndOpacity(FWoTMenuStyle::SelectedColor())
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Left)
				[
					SNew(SWoTMenuButton)
					.OnClicked_Lambda([this]()
					{
						const TArray<FWoTSkinChoice>& Choices =
							FWoTFrontEndModel::SkinChoices();
						int32 Current = Choices.IndexOfByPredicate(
							[this](const FWoTSkinChoice& Choice)
							{
								return Choice.Color == PendingSkinColor;
							});
						if (Current == INDEX_NONE) Current = 0;
						PendingSkinColor = Choices[
							(Current + 1) % Choices.Num()].Color;
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return WoTInGameSlate_SkinLabel(
							PendingSkinColor, 1); })
						.Font(this, &SWoTInGameMenuOverlay::ItemFont)
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 18.0f)
		[
			SNew(STextBlock)
			.Text(ResolveRetailText(FName(TEXT("menuPlayer.HelpMessage[3]"))))
			.Font(this, &SWoTInGameMenuOverlay::ItemFont)
			.ColorAndOpacity(FWoTMenuStyle::SelectedColor())
			.AutoWrapText(true)
			.Justification(ETextJustify::Center)
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SWoTMenuButton)
				.OnClicked(this, &SWoTInGameMenuOverlay::SavePlayerConfig)
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Save")))
					.Font(this, &SWoTInGameMenuOverlay::ItemFont)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SWoTMenuButton)
				.OnClicked_Lambda([this]()
				{
					ReturnToMainMenu();
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(TEXT("Back")))
					.Font(this, &SWoTInGameMenuOverlay::ItemFont)
				]
			]
		];
}

TSharedRef<SWidget> SWoTInGameMenuOverlay::BuildTauntsPage()
{
	TSharedRef<SVerticalBox> Entries = SNew(SVerticalBox);
	TauntButtons.Reset();
	TauntLabels.Reset();
	const int32 EntryCount = static_cast<int32>(UE_ARRAY_COUNT(WoTInGameSlate_TauntEntries));
	for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
	{
		const FWoTInGameTauntEntry& Entry = WoTInGameSlate_TauntEntries[EntryIndex];
		const int32 TauntIndex = Entry.TauntIndex;
		TauntLabels.Add(Entry.Label);
		TSharedPtr<SWoTMenuButton> Button;
		Entries->AddSlot().AutoHeight().Padding(0.0f, 4.0f)
		[
			SAssignNew(Button, SWoTMenuButton)
			.IsEnabled_Lambda([this]() { return CanInteract(); })
			.ContentPadding(FMargin(28.0f, 0.0f))
			.HAlign(HAlign_Center)
			.IsSelected_Lambda([this, EntryIndex]()
			{
				return SelectedTauntIndex == EntryIndex;
			})
			.OnSelected_Lambda([this, EntryIndex]()
			{
				SelectTauntEntry(EntryIndex);
			})
			.OnClicked_Lambda([this, TauntIndex]()
			{
				return ActivateTaunt(TauntIndex);
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(Entry.Label))
				.Font(this, &SWoTInGameMenuOverlay::ItemFont)
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
		TauntButtons.Add(Button);
	}
	SelectedTauntIndex = TauntButtons.IsEmpty() ? INDEX_NONE : 0;

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 24.0f)
		[
			SNew(STextBlock)
			.Text(WoTInGameSlate_Text(TEXT("Taunts")))
			.Font(this, &SWoTInGameMenuOverlay::HeadingFont)
			.ColorAndOpacity(FSlateColor::UseForeground())
			.Justification(ETextJustify::Center)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			Entries
		];
}

FReply SWoTInGameMenuOverlay::OpenTaunts()
{
	if (!CanInteract() || !ContentHost.IsValid()) return FReply::Handled();
	ActiveSettingsScreen.Reset();
	PlayerNameInput.Reset();
	bShowingPlayerConfig = false;
	bShowingTaunts = true;
	ResumeButton.Reset();
	ContentHost->SetContent(BuildTauntsPage());
	Invalidate(EInvalidateWidgetReason::Layout);
	// Taunt 1 first (spec r3 section 1a) for every input: a new page has no
	// earlier focus to keep, and the arrows need a focused entry.
	if (FSlateApplication::IsInitialized() && TauntButtons.IsValidIndex(0))
	{
		SelectTauntEntry(0);
		FSlateApplication::Get().SetKeyboardFocus(
			TauntButtons[0], EFocusCause::SetDirectly);
	}
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::ActivateChat(bool bTeam)
{
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->ChatFromInGameMenu(bTeam);
	}
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::ActivateTaunt(int32 TauntIndex)
{
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->TauntFromInGameMenu(TauntIndex);
	}
	return FReply::Handled();
}

bool SWoTInGameMenuOverlay::MoveTauntSelection(int32 Direction)
{
	if (!CanInteract() || TauntButtons.IsEmpty() || Direction == 0)
	{
		return false;
	}
	const int32 NewIndex = WoTInGameSlate_WrapStep(
		SelectedTauntIndex, TauntButtons.Num(), Direction);
	SelectTauntEntry(NewIndex);
	if (FSlateApplication::IsInitialized() && TauntButtons[NewIndex].IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			TauntButtons[NewIndex], EFocusCause::Navigation);
	}
	return true;
}

void SWoTInGameMenuOverlay::SelectTauntEntry(int32 EntryIndex)
{
	if (TauntButtons.IsValidIndex(EntryIndex))
	{
		SelectedTauntIndex = EntryIndex;
	}
}

FReply SWoTInGameMenuOverlay::OpenPlayerConfig()
{
	if (!CanInteract() || !ContentHost.IsValid()) return FReply::Handled();
	AWoTPlayerController* Controller = PlayerController.Get();
	PendingPlayerName = Controller && Controller->PlayerState
		? Controller->PlayerState->GetPlayerName() : FString();
	const bool bCitadelGame = Controller && Controller->GetWorld()
		&& Cast<AWoTCitadelGameState>(
			Controller->GetWorld()->GetGameState()) != nullptr;
	PendingSkinColor = Preferences.IsValid()
		? FWoTFrontEndModel::NormalizeSkinColor(
			bCitadelGame
				? Preferences->GetPreferences().CitadelSkinColor
				: Preferences->GetPreferences().SelectedSkinColor)
		: EWoTPlayerSkinColor::Base;
	ActiveSettingsScreen.Reset();
	bShowingTaunts = false;
	TauntButtons.Reset();
	bShowingPlayerConfig = true;
	ResumeButton.Reset();
	ContentHost->SetContent(BuildPlayerConfigPage());
	if (FSlateApplication::IsInitialized() && PlayerNameInput.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			PlayerNameInput, EFocusCause::SetDirectly);
	}
	Invalidate(EInvalidateWidgetReason::Layout);
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::SavePlayerConfig()
{
	AWoTPlayerController* Controller = PlayerController.Get();
	if (!Controller)
	{
		return FReply::Handled();
	}
	const FString Name = FWoTFrontEndModel::SanitizePlayerName(PendingPlayerName);
	if (!Name.IsEmpty())
	{
		Controller->SetName(Name);
	}
	const bool bCitadelGame = Controller->GetWorld()
		&& Cast<AWoTCitadelGameState>(
			Controller->GetWorld()->GetGameState()) != nullptr;
	const EWoTPlayerFaction Faction =
		WoTInGameSlate_FactionForController(Controller);
	const EWoTPlayerSkinColor Color =
		FWoTFrontEndModel::NormalizeSkinColor(PendingSkinColor);
	Controller->ServerChangeSkin(
		FWoTFrontEndModel::SkinOption(Faction, Color), FString(), 255);
	if (GEngine)
	{
		if (FWorldContext* Context =
			GEngine->GetWorldContextFromWorld(Controller->GetWorld()))
		{
			if (!Name.IsEmpty())
			{
				Context->LastURL.RemoveOption(TEXT("Name"));
				Context->LastURL.AddOption(
					*FString::Printf(TEXT("Name=%s"), *Name));
			}
			if (!bCitadelGame)
			{
				Context->LastURL.RemoveOption(TEXT("Skin"));
				if (Color != EWoTPlayerSkinColor::Base)
				{
					Context->LastURL.AddOption(*FString::Printf(TEXT("Skin=%s"),
						*FWoTFrontEndModel::SkinOption(Faction, Color)));
				}
			}
		}
	}
	if (UWoTGameInstance* Instance =
		Cast<UWoTGameInstance>(Controller->GetGameInstance()))
	{
		if (bCitadelGame)
		{
			Instance->SetCitadelSkinColor(Color);
		}
		else
		{
			Instance->SetSelectedFaction(Faction);
			Instance->SetSelectedSkinColor(Color);
		}
		Instance->SetPlayerName(Name);
	}
	ReturnToMainMenu();
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::OpenSettings(EWoTSettingsPage Page)
{
	if (CanInteract() && ContentHost.IsValid())
	{
		ResumeButton.Reset();
		bShowingTaunts = false;
		TauntButtons.Reset();
		ContentHost->SetContent(BuildSettingsPage(Page));
		if (FSlateApplication::IsInitialized() && ActiveSettingsScreen.IsValid())
		{
			// Controller mode starts on the page's first control (spec r3
			// section 1a); the pointer keeps today's focus on the page.
			const TSharedPtr<SWidget> InitialFocus =
				FWoTControllerUIInput::IsControllerMode()
					? ActiveSettingsScreen->InitialControllerFocusWidget()
					: StaticCastSharedPtr<SWidget>(ActiveSettingsScreen);
			FSlateApplication::Get().SetKeyboardFocus(
				InitialFocus, EFocusCause::SetDirectly);
		}
		Invalidate(EInvalidateWidgetReason::Layout);
	}
	return FReply::Handled();
}

void SWoTInGameMenuOverlay::ReturnToMainMenu()
{
	if (!ContentHost.IsValid())
	{
		return;
	}
	ActiveSettingsScreen.Reset();
	PlayerNameInput.Reset();
	bShowingPlayerConfig = false;
	bShowingTaunts = false;
	TauntButtons.Reset();
	TauntLabels.Reset();
	SelectedTauntIndex = INDEX_NONE;
	ContentHost->SetContent(BuildMainMenu());
	Invalidate(EInvalidateWidgetReason::Layout);
	FocusResumeButton();
}

void SWoTInGameMenuOverlay::Tick(
	const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	if (!HasUsableFonts())
	{
		SetVisibility(EVisibility::Collapsed);
		ResumeButton.Reset();
		ChildSlot.AttachWidget(SNullWidget::NullWidget);
		return;
	}
	const float PreviousViewportScale = LiveViewportScale;
	LiveViewportScale = FMath::Max(
		AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), 0.01f);
	if (ActiveSettingsScreen.IsValid() || bShowingPlayerConfig || bShowingTaunts)
	{
		if (!FMath::IsNearlyEqual(PreviousViewportScale, LiveViewportScale))
		{
			Invalidate(EInvalidateWidgetReason::Layout);
		}
		return;
	}
	const TArray<FWoTInGameMenuEntry> LiveEntries = WoTInGameSlate_MenuEntries(
		PlayerController.Get(), Preferences.Get()
#if WITH_AUTOMATION_TESTS
		, GameSettingsForTesting.IsSet() ? &GameSettingsForTesting.GetValue() : nullptr
#endif
	);
	bool bSnapshotMatches = LiveEntries.Num() == MainMenuEntryIds.Num()
		&& LiveEntries.Num() == MainMenuLabels.Num();
	for (int32 EntryIndex = 0;
		bSnapshotMatches && EntryIndex < LiveEntries.Num(); ++EntryIndex)
	{
		bSnapshotMatches = MainMenuEntryIds[EntryIndex]
			== static_cast<uint8>(LiveEntries[EntryIndex].Id)
			&& MainMenuLabels[EntryIndex] == LiveEntries[EntryIndex].Label;
	}
	if (!bSnapshotMatches)
	{
		ReturnToMainMenu();
	}
	const float PreviousEntryHeight = EntryHeight;
	const float PreviousSpacing = EntrySpacing;
	const float PreviousListHeight = ListHeight;
	EntryHeight =
		FWoTMenuLayout::MeasureFontHeight(ItemFont());
	const float TitleArea =
		FWoTMenuLayout::MeasureFontHeight(HeadingFont())
		+ 24.0f;
	const int32 EntryCount = MainMenuEntryCount();
	EntrySpacing = FWoTMenuLayout::CalculateSpacing(
		EntryCount, EntryHeight, AllottedGeometry.GetLocalSize().Y - TitleArea);
	ListHeight = FWoTMenuLayout::CalculateListHeight(
		EntryCount, EntryHeight, EntrySpacing);
	if (!FMath::IsNearlyEqual(PreviousViewportScale, LiveViewportScale)
		|| !FMath::IsNearlyEqual(PreviousEntryHeight, EntryHeight)
		|| !FMath::IsNearlyEqual(PreviousSpacing, EntrySpacing)
		|| !FMath::IsNearlyEqual(PreviousListHeight, ListHeight))
	{
		Invalidate(EInvalidateWidgetReason::Layout);
	}
}

TArray<FWoTButtonPrompt> SWoTInGameMenuOverlay::ControllerPrompts() const
{
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		return FWoTTextEntry::ControllerPrompts();
	}
	if (SWoTListCombo::IsAnyOpenWithin(AsShared()))
	{
		return FWoTButtonPromptSets::Dropdown();
	}
	if (ActiveSettingsScreen.IsValid())
	{
		return ActiveSettingsScreen->ControllerPrompts();
	}
	if (bShowingTaunts)
	{
		return FWoTButtonPromptSets::Taunts();
	}
	return FWoTButtonPromptSets::SelectBack();
}

TSharedPtr<SWidget> SWoTInGameMenuOverlay::InitialFocusWidget() const
{
	return ResumeButton;
}

void SWoTInGameMenuOverlay::FocusResumeButton()
{
	const TSharedPtr<SWidget> InitialFocus = InitialFocusWidget();
	if (FSlateApplication::IsInitialized() && InitialFocus.IsValid())
	{
		SelectMainMenuEntry(0);
		FSlateApplication::Get().SetKeyboardFocus(
			InitialFocus, EFocusCause::SetDirectly);
	}
}

FReply SWoTInGameMenuOverlay::OnKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		// The keyboard panel owns every key; Escape cancels it first [M3].
		if (InKeyEvent.GetKey() == EKeys::Escape)
		{
			FWoTTextEntry::CancelWithin(AsShared());
			return FReply::Handled();
		}
		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}
	if (IsHelpKey(InKeyEvent.GetKey())
		&& (!ActiveSettingsScreen.IsValid()
			|| !ActiveSettingsScreen->IsCapturingBinding()))
	{
		return ActivateHelp();
	}
	// Escape keeps its own path; B and View arrive through the back stack.
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		// An open dropdown closes first (spec r3 [N1]).
		if (SWoTListCombo::CloseOpenWithin(AsShared()))
		{
			return FReply::Handled();
		}
		if (CanInteract())
		{
			return Resume();
		}
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTInGameMenuOverlay::OnPreviewKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (FWoTTextEntry::IsActiveWithin(AsShared()))
	{
		// The keyboard panel owns every key: physical typing never opens Help.
		// Escape cancels the panel first, and the page stays (spec r3 [M3]).
		if (InKeyEvent.GetKey() == EKeys::Escape)
		{
			FWoTTextEntry::CancelWithin(AsShared());
			return FReply::Handled();
		}
		return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}
	if (IsHelpKey(InKeyEvent.GetKey())
		&& (!ActiveSettingsScreen.IsValid()
			|| !ActiveSettingsScreen->IsCapturingBinding()))
	{
		return ActivateHelp();
	}
	const FKey Key = InKeyEvent.GetKey();
	if ((IsShowingMainMenu() || bShowingTaunts)
		&& (Key == EKeys::Up || Key == EKeys::Down
			|| Key == EKeys::Gamepad_DPad_Up || Key == EKeys::Gamepad_DPad_Down))
	{
		const int32 Direction = Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up ? -1 : 1;
		const bool bMoved = bShowingTaunts
			? MoveTauntSelection(Direction) : MoveMainMenuSelection(Direction);
		return bMoved ? FReply::Handled() : FReply::Unhandled();
	}
	if (Key == EKeys::Escape)
	{
		// With a dropdown open, Escape closes only the dropdown; a second
		// Escape backs out as before (owner ruling 2026-11-08, spec r3 [N1]).
		if (!SWoTListCombo::CloseOpenWithin(AsShared()))
		{
			BackOneLevel();
		}
		return FReply::Handled();
	}
	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

bool SWoTInGameMenuOverlay::BackOneLevel()
{
	if (ActiveSettingsScreen.IsValid())
	{
		const TSharedPtr<SWoTSettingsScreen> Screen = ActiveSettingsScreen;
		Screen->HandleBack();
		return true;
	}
	if (bShowingPlayerConfig || bShowingTaunts)
	{
		// Escape, B and View on the Taunts page return to the menu [M3].
		ReturnToMainMenu();
		return true;
	}
	if (CanInteract())
	{
		Resume();
	}
	return true;
}

FNavigationReply SWoTInGameMenuOverlay::OnNavigation(
	const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	// The stick navigates by analog events rather than keys; it moves the same
	// wrapping selection as the arrows and the D-pad.
	const EUINavigation Direction = InNavigationEvent.GetNavigationType();
	if (Direction == EUINavigation::Up || Direction == EUINavigation::Down)
	{
		const int32 Step = Direction == EUINavigation::Up ? -1 : 1;
		if ((IsShowingMainMenu() && MoveMainMenuSelection(Step))
			|| (bShowingTaunts && MoveTauntSelection(Step)))
		{
			return FNavigationReply::Stop();
		}
	}
	return SCompoundWidget::OnNavigation(MyGeometry, InNavigationEvent);
}

bool SWoTInGameMenuOverlay::IsShowingMainMenu() const
{
	return !ActiveSettingsScreen.IsValid() && !bShowingPlayerConfig && !bShowingTaunts;
}

FReply SWoTInGameMenuOverlay::Resume()
{
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->ResumeFromInGameMenu();
	}
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::ActivateScoreboard()
{
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->ResumeFromInGameMenu();
		Controller->ShowScores();
	}
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::ActivateHelp()
{
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->ResumeFromInGameMenu();
		Controller->ShowHelp();
	}
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::ActivateCitadel()
{
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->ResumeFromInGameMenu();
		Controller->ServerEditCitadel();
	}
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::ActivateMainMenuEntryById(uint8 EntryId)
{
	switch (static_cast<EWoTInGameMenuEntry>(EntryId))
	{
	case EWoTInGameMenuEntry::Resume: return Resume();
	case EWoTInGameMenuEntry::Scoreboard: return ActivateScoreboard();
	case EWoTInGameMenuEntry::Help: return ActivateHelp();
	case EWoTInGameMenuEntry::Chat: return ActivateChat(/*bTeam*/ false);
	case EWoTInGameMenuEntry::TeamChat: return ActivateChat(/*bTeam*/ true);
	case EWoTInGameMenuEntry::Taunts: return OpenTaunts();
	case EWoTInGameMenuEntry::EditCitadel: return ActivateCitadel();
	case EWoTInGameMenuEntry::PlayerConfig: return OpenPlayerConfig();
	case EWoTInGameMenuEntry::Controls:
		return OpenSettings(EWoTSettingsPage::Controls);
	case EWoTInGameMenuEntry::Hardware:
		return OpenSettings(EWoTSettingsPage::Hardware);
	case EWoTInGameMenuEntry::Quit:
	default: return LeaveToMenu();
	}
}

bool SWoTInGameMenuOverlay::MoveMainMenuSelection(int32 Direction)
{
	if (!CanInteract() || MainMenuButtons.IsEmpty() || Direction == 0)
	{
		return false;
	}
	const int32 EntryCount = MainMenuButtons.Num();
	const int32 CurrentIndex = MainMenuButtons.IsValidIndex(SelectedMainMenuIndex)
		? SelectedMainMenuIndex : 0;
	const int32 NewIndex = (CurrentIndex + (Direction < 0 ? -1 : 1)
		+ EntryCount) % EntryCount;
	SelectMainMenuEntry(NewIndex);
	if (FSlateApplication::IsInitialized()
		&& MainMenuButtons[NewIndex].IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(
			MainMenuButtons[NewIndex], EFocusCause::Navigation);
	}
	return true;
}

void SWoTInGameMenuOverlay::SelectMainMenuEntry(int32 EntryIndex)
{
	if (MainMenuButtons.IsValidIndex(EntryIndex))
	{
		SelectedMainMenuIndex = EntryIndex;
	}
}

bool SWoTInGameMenuOverlay::IsHelpKey(const FKey& Key) const
{
	const FWoTGameSettings Defaults = FWoTGameSettings::Defaults();
	const FWoTGameSettings& Settings =
#if WITH_AUTOMATION_TESTS
		GameSettingsForTesting.IsSet() ? GameSettingsForTesting.GetValue() :
#endif
		(Preferences.IsValid() ? Preferences->GetGameSettings() : Defaults);
	const FWoTManagedKeyBinding* Binding =
		Settings.FindBinding(EWoTManagedBinding::ShowHelp);
	return Binding && Key.IsValid()
		&& (Key == Binding->Primary || Key == Binding->Secondary);
}

FReply SWoTInGameMenuOverlay::LeaveToMenu()
{
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->LeaveToFrontEnd();
	}
	return FReply::Handled();
}

bool SWoTInGameMenuOverlay::CanInteract() const
{
	const AWoTPlayerController* Controller = PlayerController.Get();
	return Controller && !Controller->IsLeavingForMenu();
}

int32 SWoTInGameMenuOverlay::MainMenuEntryCount() const
{
	return MainMenuEntryIds.Num();
}

FMargin SWoTInGameMenuOverlay::EntryPadding(int32 EntryIndex) const
{
	const float HalfSpacing = EntrySpacing * 0.5f;
	return FMargin(
		0.0f,
		EntryIndex == 0 ? 0.0f : HalfSpacing,
		0.0f,
		EntryIndex == MainMenuEntryCount() - 1 ? 0.0f : HalfSpacing);
}

FSlateFontInfo SWoTInGameMenuOverlay::HeadingFont() const
{
	if (PresentationAuthority.IsValid()
		&& PresentationAuthority->IsReady())
	{
		return PresentationAuthority->MultiplayerFont(
			FName(TEXT("F_WOTReg30")), LiveViewportScale);
	}
	if (!RequiredFonts.IsValid()) return FSlateFontInfo();
	const int32 PixelScale = FMath::Clamp(
		FMath::RoundToInt(LiveViewportScale), 1, 2);
	FSlateFontInfo Font = RequiredFonts->Title;
	Font.Size *= static_cast<float>(PixelScale) * 0.75f / LiveViewportScale;
	return Font;
}

FSlateFontInfo SWoTInGameMenuOverlay::ItemFont() const
{
	if (PresentationAuthority.IsValid()
		&& PresentationAuthority->IsReady())
	{
		return PresentationAuthority->MultiplayerFont(
			FName(TEXT("F_WOTReg14")), LiveViewportScale, 2);
	}
	if (!RequiredFonts.IsValid()) return FSlateFontInfo();
	const int32 PixelScale = FMath::Clamp(
		FMath::Max(FMath::RoundToInt(LiveViewportScale), 2), 1, 2);
	FSlateFontInfo Font = RequiredFonts->List;
	Font.Size *= static_cast<float>(PixelScale) * 0.75f / LiveViewportScale;
	return Font;
}

FText SWoTInGameMenuOverlay::ResolveRetailText(FName Id) const
{
	if (PresentationAuthority.IsValid())
	{
		const FText Authenticated = PresentationAuthority->Text(Id);
		if (!Authenticated.IsEmpty()) return Authenticated;
	}
	return FWoTMenuStyle::Get().RetailText(Id);
}

bool SWoTInGameMenuOverlay::HasUsableFonts() const
{
	return RequiredFonts.IsValid()
		&& (bUsingFallbackFonts
			? RequiredFonts->Title.HasValidFont()
				&& RequiredFonts->List.HasValidFont()
			: RequiredFonts->IsValid());
}

#if WITH_AUTOMATION_TESTS
void SWoTInGameMenuOverlay::OpenSettingsForTesting(EWoTSettingsPage Page)
{
	OpenSettings(Page);
}

bool SWoTInGameMenuOverlay::IsShowingMainMenuForTesting() const
{
	return ContentHost.IsValid() && !ActiveSettingsScreen.IsValid()
		&& !bShowingPlayerConfig && !bShowingTaunts;
}

FReply SWoTInGameMenuOverlay::ActivateTauntEntryForTesting(int32 EntryIndex)
{
	const int32 EntryCount = static_cast<int32>(UE_ARRAY_COUNT(WoTInGameSlate_TauntEntries));
	return bShowingTaunts && EntryIndex >= 0 && EntryIndex < EntryCount
		? ActivateTaunt(WoTInGameSlate_TauntEntries[EntryIndex].TauntIndex)
		: FReply::Handled();
}

TArray<FString> SWoTInGameMenuOverlay::MenuLabelsForTesting() const
{
	return MainMenuLabels;
}

FReply SWoTInGameMenuOverlay::ActivateScoreboardForTesting()
{
	return ActivateScoreboard();
}

FReply SWoTInGameMenuOverlay::ActivateHelpForTesting()
{
	return ActivateHelp();
}

TSharedPtr<SWidget> SWoTInGameMenuOverlay::PlayerNameInputForTesting() const
{
	return PlayerNameInput;
}

FReply SWoTInGameMenuOverlay::ActivateCitadelForTesting()
{
	return ActivateCitadel();
}

FReply SWoTInGameMenuOverlay::ActivateVisibleMainMenuEntryForTesting(
	int32 EntryIndex)
{
	return MainMenuEntryIds.IsValidIndex(EntryIndex)
		? ActivateMainMenuEntryById(MainMenuEntryIds[EntryIndex])
		: FReply::Handled();
}

FSlateFontInfo SWoTInGameMenuOverlay::HeadingFontForTesting() const
{
	return HeadingFont();
}

FSlateFontInfo SWoTInGameMenuOverlay::ItemFontForTesting() const
{
	return ItemFont();
}

float SWoTInGameMenuOverlay::MainMenuDesiredHeightForTesting() const
{
	return FWoTMenuLayout::MeasureFontHeight(HeadingFont())
		+ 24.0f + ListHeight;
}
#endif

bool FWoTHelpContent::ParseRetailInt(TConstArrayView<uint8> Bytes,
	FWoTHelpContent& Out)
{
	FWoTIntFile File;
	if (!FWoTRetailText::ParseIntBytes(Bytes, File)) return false;
	const FString* TitleValue = File.Find(TEXT("WOTHelpInfo"), TEXT("Title"));
	const FString* SubTitleValue = File.Find(TEXT("WOTHelpInfo"), TEXT("SubTitle"));
	if (!TitleValue || !SubTitleValue) return false;
	FWoTHelpContent Candidate;
	Candidate.Title = *TitleValue;
	Candidate.SubTitle = *SubTitleValue;
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const FString Key = FString::Printf(TEXT("Content[%d]"), Index);
		const FString* Value = File.Find(TEXT("WOTHelpInfo"), Key);
		if (!Value || Value->IsEmpty()) break;
		Candidate.Content.Add(*Value);
	}
	if (Candidate.Content.IsEmpty()) return false;
	Out = MoveTemp(Candidate);
	return true;
}

bool FWoTHelpContent::LoadRuntime(FWoTHelpContent& Out)
{
	if (!FWoTRetailText::EnsureLoaded()) return false;
	const FString* TitleValue = FWoTRetailText::FindSectionValue(
		TEXT("WOTHelpInfo"), TEXT("Title"));
	const FString* SubTitleValue = FWoTRetailText::FindSectionValue(
		TEXT("WOTHelpInfo"), TEXT("SubTitle"));
	if (!TitleValue || !SubTitleValue) return false;
	FWoTHelpContent Candidate;
	Candidate.Title = *TitleValue;
	Candidate.SubTitle = *SubTitleValue;
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const FString Key = FString::Printf(TEXT("Content[%d]"), Index);
		const FString* Value = FWoTRetailText::FindSectionValue(
			TEXT("WOTHelpInfo"), Key);
		if (!Value || Value->IsEmpty()) break;
		Candidate.Content.Add(*Value);
	}
	if (Candidate.Content.IsEmpty()) return false;
	Out = MoveTemp(Candidate);
	return true;
}

TArray<FWoTHelpLayoutLine> FWoTHelpContent::Layout(
	const FWoTHelpContent& In)
{
	return Layout(In, [](const FString& Text, int32 Index)
	{
		const float Height = (Index == 0 || Index == 1) ? 8.0f
			: ((Index == 4 || Index == 5) ? 30.0f : 14.0f);
		const float AverageWidth = (Index == 0 || Index == 1) ? 5.0f
			: ((Index == 4 || Index == 5) ? 18.0f : 8.0f);
		return FVector2D(Text.Len() * AverageWidth, Height);
	});
}

TArray<FWoTHelpLayoutLine> FWoTHelpContent::Layout(
	const FWoTHelpContent& In,
	const TFunction<FVector2D(const FString&, int32)>& Measure)
{
	constexpr float ClipWidth = 512.0f; // uiWindow: 640 - 2*WindowBorderX(64)
	TArray<FWoTHelpLayoutLine> Lines;
	int32 FontIndex = 3;
	bool bCentered = true;
	float ForcedX = -1.0f;
	float ForcedY = -1.0f;
	// DrawTextAt(Title, bCR=false) still advances CurY through WrappedPrint.
	float CurrentY = FMath::Max(1.0f, Measure(In.Title, 3).Y);
	for (const FString& Command : In.Content)
	{
		if (Command.StartsWith(TEXT("%f=")))
		{
			FontIndex = FCString::Atoi(*Command.Mid(3));
			continue;
		}
		if (Command.StartsWith(TEXT("%s=")))
		{
			CurrentY += FCString::Atoi(*Command.Mid(3));
			continue;
		}
		if (Command.StartsWith(TEXT("%c=")))
		{
			bCentered = FCString::Atoi(*Command.Mid(3)) != 0;
			continue;
		}
		if (Command.StartsWith(TEXT("%x=")))
		{
			ForcedX = FCString::Atoi(*Command.Mid(3));
			continue;
		}
		if (Command.StartsWith(TEXT("%y=")))
		{
			ForcedY = FCString::Atoi(*Command.Mid(3));
			continue;
		}
		if (Command.StartsWith(TEXT("%d=")))
		{
			const int32 SeparatorType = FCString::Atoi(*Command.Mid(3));
			CurrentY += SeparatorType == 1 ? 40.0f : 24.0f;
			continue;
		}

		FString Remaining = Command.StartsWith(TEXT("%b "))
			? Command.Mid(3) : Command;
		float LineX = ForcedX < 0.0f ? 0.0f : ForcedX;
		float LineY = ForcedY < 0.0f ? CurrentY : ForcedY;
		while (!Remaining.IsEmpty())
		{
			int32 CleanEnd = 0;
			bool bGotWord = false;
			for (int32 Index = 0; Index < Remaining.Len()
				&& Remaining[Index] != TEXT('\n'); ++Index)
			{
				const int32 TestEnd = Index + 1;
				if (LineX + Measure(Remaining.Left(TestEnd), FontIndex).X
					> ClipWidth)
				{
					break;
				}
				const bool bWordBreak = TestEnd == Remaining.Len()
					|| Remaining[TestEnd] == TEXT(' ')
					|| Remaining[TestEnd] == TEXT('\n');
				if (bWordBreak || !bGotWord)
				{
					CleanEnd = TestEnd;
					bGotWord |= bWordBreak;
				}
			}
			if (CleanEnd == 0)
			{
				break;
			}

			FWoTHelpLayoutLine& Line = Lines.AddDefaulted_GetRef();
			Line.Text = Remaining.Left(CleanEnd);
			Line.FontIndex = FontIndex;
			Line.bCentered = bCentered;
			const FVector2D Size = Measure(Line.Text, FontIndex);
			Line.Width = FMath::Max(1.0f, Size.X);
			Line.Height = FMath::Max(1.0f, Size.Y);
			Line.X = bCentered
				? LineX + (ClipWidth - Line.Width) * 0.5f : LineX;
			Line.Y = LineY;
			CurrentY = Line.Y + Line.Height;

			Remaining.RightChopInline(CleanEnd, EAllowShrinking::No);
			while (!Remaining.IsEmpty()
				&& (Remaining[0] == TEXT(' ') || Remaining[0] == TEXT('\n')))
			{
				Remaining.RightChopInline(1, EAllowShrinking::No);
			}
			LineX = 0.0f; // UCanvas::WrappedPrint resets CurX after each row.
			LineY = CurrentY;
		}
	}
	return Lines;
}

void SWoTHelpOverlay::Construct(const FArguments& InArgs)
{
	HelpContent = InArgs._Content;
	OnClose = InArgs._OnClose;
	PrimaryCloseKey = InArgs._PrimaryCloseKey;
	SecondaryCloseKey = InArgs._SecondaryCloseKey;
	PresentationAuthority = InArgs._PresentationAuthority;
	if (InArgs._InitialPhysicalSize.X > 0.0f && InArgs._InitialPhysicalSize.Y > 0.0f)
	{
		// Slate prepasses before it ticks: without this the first frame drew
		// at 1x and jumped to 2x on the next.
		DpiScale = FMath::Max(InArgs._InitialDpiScale, 0.01f);
		PixelScale = PixelScaleFor(InArgs._InitialPhysicalSize);
	}
#if WITH_AUTOMATION_TESTS
	RequiredFonts = InArgs._RequiredFontsForTesting;
#endif
	if (!RequiredFonts.IsValid() && PresentationAuthority.IsValid())
	{
		if (const FWoTRequiredMenuFonts* Fonts =
			PresentationAuthority->RequiredFonts())
		{
			RequiredFonts = MakeShared<FWoTRequiredMenuFonts>(*Fonts);
		}
	}
	if (!RequiredFonts.IsValid())
	{
		TSharedRef<FWoTRequiredMenuFonts> Fallback =
			MakeShared<FWoTRequiredMenuFonts>();
		Fallback->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
		Fallback->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
		RequiredFonts = Fallback;
	}
	if (PresentationAuthority.IsValid())
	{
		PresentationAuthority->BindWidget(AsShared());
	}
	else if (RequiredFonts->Generation.IsValid())
	{
		RequiredFonts->BindWidget(AsShared());
	}
	// B and View close Help through the back stack (held B closes it once).
	BackStackEntry = FWoTBackStack::Get().Register(AsShared(), nullptr,
		[this]() { Close(); return true; });

	TFunction<FVector2D(const FString&, int32)> Measure =
		[this](const FString& Text, int32 Index)
		{
			const FSlateFontInfo Font = FontForIndex(Index);
			if (FSlateApplication::IsInitialized())
			{
				if (FSlateRenderer* Renderer =
					FSlateApplication::Get().GetRenderer())
				{
					return FVector2D(Renderer->GetFontMeasureService()
						->Measure(Text, Font));
				}
			}
			return FVector2D(Text.Len() * FMath::Max(1, Font.Size / 2),
				FMath::Max(1, Font.Size));
		};
	const TArray<FWoTHelpLayoutLine> Layout =
		FWoTHelpContent::Layout(HelpContent, Measure);
	const float TitleHeight = FMath::Max(1.0f, Measure(HelpContent.Title, 3).Y);
	float ContentEndY = TitleHeight;
	for (const FWoTHelpLayoutLine& Line : Layout)
	{
		ContentEndY = Line.Y + Line.Height;
	}
	const float SubTitleY = FMath::Max(ContentEndY + 8.0f, 432.0f - 16.0f);
	const float SubTitleHeight = FMath::Max(1.0f,
		Measure(HelpContent.SubTitle, 3).Y);

	// Every slot is placed by its top-left corner, as the retail Canvas drew
	// at CurX/CurY. SConstraintCanvas defaults to a (0.5, 0.5) alignment,
	// which centred each line on its position: half of every left-aligned
	// line fell off the window, headings and the F-key columns shifted and
	// overlapped, and the title was cut in half.
	const FVector2D TopLeft(0.0f, 0.0f);
	TSharedRef<SConstraintCanvas> Canvas = SNew(SConstraintCanvas)
		.Clipping(EWidgetClipping::ClipToBounds);
	Canvas->AddSlot()
	.Anchors(FAnchors(0.0f, 0.0f, 1.0f, 0.0f))
	.Offset(FMargin(0.0f, 0.0f, 0.0f, TitleHeight))
	.Alignment(TopLeft)
	[
		SNew(STextBlock)
		.Text(FText::FromString(HelpContent.Title))
		.Font_Lambda([this]() { return FontForIndex(3); })
		.Justification(ETextJustify::Center)
	];
	for (const FWoTHelpLayoutLine& Line : Layout)
	{
		const int32 LineFont = Line.FontIndex;
		Canvas->AddSlot()
		.Anchors(FAnchors(0.0f, 0.0f))
		.Offset(FMargin(Line.X, Line.Y, Line.Width, Line.Height))
		.Alignment(TopLeft)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Line.Text))
			.Font_Lambda([this, LineFont]() { return FontForIndex(LineFont); })
		];
	}
	Canvas->AddSlot()
	.Anchors(FAnchors(0.0f, 0.0f, 1.0f, 0.0f))
	.Offset(FMargin(0.0f, SubTitleY, 0.0f, SubTitleHeight))
	.Alignment(TopLeft)
	[
		SNew(STextBlock)
		.Text(FText::FromString(HelpContent.SubTitle))
		.Font_Lambda([this]() { return FontForIndex(3); })
		.Justification(ETextJustify::Center)
	];

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(FWoTMenuStyle::Get().Blackout())
			.ColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.82f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			// ScaleToFit stretched the 1x bitmap strikes by a fractional
			// factor (2.25 at 1080p) and smeared every glyph. The window now
			// draws at the largest whole-number scale that fits, with the
			// matching bitmap strike, so text lands 1:1 on screen pixels as
			// the HUD and in-game menus do.
			SNew(SScaleBox)
			.Stretch(EStretch::UserSpecified)
			.UserSpecifiedScale_Lambda([this]() { return WindowScale(); })
			[
				SNew(SBox)
				.WidthOverride(640.0f)
				.HeightOverride(480.0f)
				[
					SNew(SBorder)
					.BorderImage(FWoTMenuStyle::Get().Frame())
					.ForegroundColor(FWoTMenuStyle::MenuColor(false))
					.Padding(FMargin(64.0f, 24.0f))
					[
						SNew(SBox)
						.WidthOverride(512.0f)
						.HeightOverride(432.0f)
						[
							Canvas
						]
					]
				]
			]
		]
		// Controller mode only: B closes Help (spec r3 section 1a).
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(24.0f, 0.0f, 32.0f, 24.0f))
		[
			SAssignNew(ButtonPrompts, SWoTButtonPrompts)
			.Prompts(FWoTButtonPromptSets::Close())
			.Font_Lambda([this]() { return FontForIndex(3); })
		]
	];
}

int32 SWoTHelpOverlay::PixelScaleFor(const FVector2D& PhysicalSize)
{
	const float Fit = FMath::Min(PhysicalSize.X / 640.0f, PhysicalSize.Y / 480.0f);
	return FMath::Clamp(FMath::FloorToInt(Fit), 1, 2);
}

void SWoTHelpOverlay::Tick(const FGeometry& AllottedGeometry,
	const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	DpiScale = FMath::Max(
		AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), 0.01f);
	PixelScale = PixelScaleFor(AllottedGeometry.GetLocalSize() * DpiScale);
}

FSlateFontInfo SWoTHelpOverlay::FontForIndex(int32 FontIndex) const
{
	const FName Face = (FontIndex == 0 || FontIndex == 1)
		? FName(TEXT("F_WOTReg08"))
		: (FontIndex == 2 ? FName(TEXT("F_WOTIta14"))
			: ((FontIndex == 4 || FontIndex == 5)
				? FName(TEXT("F_WOTReg30")) : FName(TEXT("F_WOTReg14"))));
	if (PresentationAuthority.IsValid() && PresentationAuthority->IsReady())
	{
		// The PixelScale strike at the window's own units: the box is drawn at
		// PixelScale screen pixels per unit, so each glyph meets its strike 1:1.
		return PresentationAuthority->MultiplayerFont(Face,
			static_cast<float>(PixelScale));
	}
	FSlateFontInfo Font = (FontIndex == 4 || FontIndex == 5)
		? RequiredFonts->Title : RequiredFonts->List;
	if (FontIndex == 0 || FontIndex == 1) Font.Size = 8;
	else if (FontIndex == 2) Font.TypefaceFontName = TEXT("Italic");
	return Font;
}

FReply SWoTHelpOverlay::Close()
{
	OnClose.ExecuteIfBound();
	return FReply::Handled();
}

FReply SWoTHelpOverlay::OnKeyDown(const FGeometry& MyGeometry,
	const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (WoTIsMenuBackKey(Key) || Key == EKeys::F1
		|| (PrimaryCloseKey.IsValid() && Key == PrimaryCloseKey)
		|| (SecondaryCloseKey.IsValid() && Key == SecondaryCloseKey))
	{
		return Close();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTHelpOverlay::OnPreviewKeyDown(const FGeometry& MyGeometry,
	const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (WoTIsMenuBackKey(Key) || Key == EKeys::F1
		|| (PrimaryCloseKey.IsValid() && Key == PrimaryCloseKey)
		|| (SecondaryCloseKey.IsValid() && Key == SecondaryCloseKey))
	{
		return Close();
	}
	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FWoTItemInfoContent FWoTItemInfoContent::FromItem(const AWoTInventory* InItem)
{
	FWoTItemInfoContent Result;
	if (!IsValid(InItem))
	{
		return Result;
	}

	Result.Title = InItem->Title;
	Result.Icon = InItem->StatusIcon;
	Result.Description = InItem->Description;
	Result.Quote = InItem->Quote;
	const AWoTAngrealInventory* Angreal = Cast<AWoTAngrealInventory>(InItem);
	if (!IsValid(Angreal))
	{
		return Result;
	}

	Result.bAngreal = true;
	const TCHAR* RarityKey = Angreal->bRare ? TEXT("RarityRareStr")
		: (Angreal->bUncommon ? TEXT("RarityUncommonStr") : TEXT("RarityCommonStr"));
	Result.Rarity = Angreal->bRare ? TEXT("Rare artifact")
		: (Angreal->bUncommon ? TEXT("Uncommon artifact") : TEXT("Common artifact"));
	if (FWoTRetailText::EnsureLoaded())
	{
		if (const FString* Value = FWoTRetailText::FindSectionValue(
			TEXT("InventoryInfoWindow"), RarityKey))
		{
			Result.Rarity = *Value;
		}
	}
	Result.Charges = Angreal->ChargeCost == 0
		? FString(TEXT("Charges:  :"))
		: FString::Printf(TEXT("Charges:  %d"), Angreal->CurCharges);
	// InventoryInfoWindow.uc:146-150. The shipped order is Earth, Air, Fire,
	// Water, Spirit (the prose brief swaps the final two; shipped script wins).
	Result.Elements = {
		{TEXT("EARTH"), Angreal->bElementEarth},
		{TEXT("AIR"), Angreal->bElementAir},
		{TEXT("FIRE"), Angreal->bElementFire},
		{TEXT("WATER"), Angreal->bElementWater},
		{TEXT("SPIRIT"), Angreal->bElementSpirit}
	};
	return Result;
}

void SWoTItemInfoOverlay::Construct(const FArguments& InArgs)
{
	PlayerController = InArgs._PlayerController;
	Item = InArgs._Item;
	TOptional<FWoTRequiredMenuFonts> RequiredFonts;
#if WITH_AUTOMATION_TESTS
	if (InArgs._RequiredFontsForTesting.IsValid())
	{
		RequiredFonts = *InArgs._RequiredFontsForTesting;
	}
	else
#endif
	{
		RequiredFonts = FWoTMenuStyle::Get().ResolveRequiredFonts();
	}
	if (!RequiredFonts.IsSet() || !RequiredFonts->IsValid())
	{
		SetVisibility(VisibilityForFonts(false));
		ChildSlot[SNullWidget::NullWidget];
		return;
	}
	const FWoTRequiredMenuFonts CapturedFonts = RequiredFonts.GetValue();
	// A static visibility value is intentional. Bound Slate attributes retain
	// their previous cached value until an attribute update; this overlay must
	// become non-interactive immediately when it is constructed. Revocation is
	// still live because the font generation collapses every bound widget.
	SetVisibility(VisibilityForFonts(true));
	CapturedFonts.BindWidget(AsShared());
	Content = FWoTItemInfoContent::FromItem(InArgs._Item);
	if (UTexture2D* IconTexture = UWoTRuntimeAssets::ResolveTexturePath(
		Content.Icon.ToSoftObjectPath()))
	{
		IconBrush.SetResourceObject(IconTexture);
		IconBrush.SetImageSize(FVector2D(64.0f, 64.0f));
		IconBrush.DrawAs = ESlateBrushDrawType::Image;
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
	Body->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 14.0f)
	[
		SNew(SWoTMenuText)
		.Text(FText::FromString(Content.Title))
		.Large(true)
		.Justification(ETextJustify::Center)
	];
	Body->AddSlot()
	.AutoHeight()
	.HAlign(HAlign_Center)
	.Padding(0.0f, 0.0f, 0.0f, 12.0f)
	[
		SNew(SBox)
		.WidthOverride(64.0f)
		.HeightOverride(64.0f)
		[
			SNew(SImage).Image(&IconBrush)
		]
	];

	if (Content.bAngreal)
	{
		Body->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 16.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Content.Rarity))
				.Font(RequiredFonts->List)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Right)
			[
				SNew(STextBlock)
				.Font(RequiredFonts->List)
				.Text_Lambda([WeakItem = Item]()
				{
					return FText::FromString(
						FWoTItemInfoContent::FromItem(WeakItem.Get()).Charges);
				})
			]
		];

		TSharedRef<SHorizontalBox> Gems = SNew(SHorizontalBox);
		for (int32 ElementIndex = 0;
			ElementIndex < Content.Elements.Num(); ++ElementIndex)
		{
			Gems->AddSlot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Font(FWoTMenuStyle::Get().SmallFont())
				.Text_Lambda([WeakItem = Item, ElementIndex]()
				{
					const FWoTItemInfoContent Live =
						FWoTItemInfoContent::FromItem(WeakItem.Get());
					if (!Live.Elements.IsValidIndex(ElementIndex))
					{
						return FText::GetEmpty();
					}
					const FWoTItemInfoElement& Element = Live.Elements[ElementIndex];
					return FText::FromString(FString::Printf(TEXT("%s %s"),
						Element.bActive ? TEXT("[ON]") : TEXT("[--]"),
						*Element.Label));
				})
				.ColorAndOpacity_Lambda([WeakItem = Item, ElementIndex]()
				{
					const FWoTItemInfoContent Live =
						FWoTItemInfoContent::FromItem(WeakItem.Get());
					const bool bActive = Live.Elements.IsValidIndex(ElementIndex)
						&& Live.Elements[ElementIndex].bActive;
					return bActive ? FSlateColor(FLinearColor(0.45f, 0.9f, 1.0f))
						: FSlateColor(FLinearColor(0.45f, 0.45f, 0.45f));
				})
			];
		}
		Body->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 18.0f)
		[
			Gems
		];
	}

	Body->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 16.0f)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Content.Description))
		.Font(RequiredFonts->List)
		.AutoWrapText(true)
		.WrapTextAt(600.0f)
		.Justification(ETextJustify::Center)
	];
	Body->AddSlot()
	.AutoHeight()
	.Padding(24.0f, 0.0f, 24.0f, 16.0f)
	[
		SNew(SSeparator)
		.SeparatorImage(FWoTMenuStyle::Get().Separator())
	];
	Body->AddSlot()
	.AutoHeight()
	[
		SNew(STextBlock)
		.Text(FText::FromString(Content.Quote))
		.Font(FWoTMenuStyle::Get().ItalicFont())
		.AutoWrapText(true)
		.WrapTextAt(600.0f)
		.Justification(ETextJustify::Center)
	];

	ChildSlot
	[
		SNew(SOverlay)
		.Visibility(EVisibility::HitTestInvisible)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(FWoTMenuStyle::Get().Blackout())
			.ColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 0.88f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBorder)
			.BorderImage(FWoTMenuStyle::Get().Background())
			[
			SNew(SBorder)
			.BorderImage(FWoTMenuStyle::Get().Frame())
			.ForegroundColor(FWoTMenuStyle::MenuColor(false))
			.Padding(40.0f, 32.0f)
			[
				SNew(SBox)
				.WidthOverride(720.0f)
				[
					Body
				]
			]
		]
		]
	];
}

EVisibility SWoTItemInfoOverlay::VisibilityForFonts(bool bFontsValid)
{
	return bFontsValid ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
}
