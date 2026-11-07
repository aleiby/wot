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
#include "GameFramework/PlayerState.h"

namespace
{
	FText WoTInGameSlate_Text(const TCHAR* Text)
	{
		return FText::FromString(Text);
	}

	enum class EWoTInGameMenuEntry : uint8
	{
		Resume, Scoreboard, Help, EditCitadel, PlayerConfig, Controls, Hardware, Quit
	};

	struct FWoTInGameMenuEntry
	{
		EWoTInGameMenuEntry Id;
		FString Label;
	};

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
				*WoTInGameSlate_KeyLabel(Settings, EWoTManagedBinding::ShowHelp))}
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

	// UE5 action delegates run after the platform has delivered the key's
	// character event, so the T/Y opener cannot arrive at this newly focused
	// widget. Accept the first event unconditionally; a timing heuristic would
	// instead discard a fast user's first actual message character.
	TSharedRef<SEditableText> Editable = SNew(SEditableText)
		.Text(FText::FromString(InArgs._InitialText))
		.Font(RequiredFonts->List)
		.ColorAndOpacity(FLinearColor::White)
		.IsCaretMovedWhenGainFocus(true)
		.SelectAllTextWhenFocused(false);
	Editable->GoTo(ETextLocation::EndOfDocument);
	Input = Editable;

	ChildSlot
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
					.Font(RequiredFonts->List)
					.ColorAndOpacity(FLinearColor::White)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					Editable
				]
			]
		]
	];
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
	const FReply Reply = HandleKey(InKeyEvent.GetKey());
	return Reply.IsEventHandled() ? Reply
		: SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTChatInputOverlay::OnKeyDown(
	const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
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
	return Input.IsValid() ? Input->GetText().ToString() : FString();
}

void SWoTChatInputOverlay::SetText(const FString& Text)
{
	if (Input.IsValid())
	{
		Input->SetText(FText::FromString(Text));
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

	ChildSlot
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
		ContentHost->SetContent(BuildSettingsPage(Page));
		if (FSlateApplication::IsInitialized() && ActiveSettingsScreen.IsValid())
		{
			FSlateApplication::Get().SetKeyboardFocus(
				ActiveSettingsScreen, EFocusCause::SetDirectly);
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
	if (ActiveSettingsScreen.IsValid() || bShowingPlayerConfig)
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
	if (IsHelpKey(InKeyEvent.GetKey())
		&& (!ActiveSettingsScreen.IsValid()
			|| !ActiveSettingsScreen->IsCapturingBinding()))
	{
		return ActivateHelp();
	}
	if (WoTIsMenuBackKey(InKeyEvent.GetKey()))
	{
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
	if (IsHelpKey(InKeyEvent.GetKey())
		&& (!ActiveSettingsScreen.IsValid()
			|| !ActiveSettingsScreen->IsCapturingBinding()))
	{
		return ActivateHelp();
	}
	const FKey Key = InKeyEvent.GetKey();
	if (IsShowingMainMenu()
		&& (Key == EKeys::Up || Key == EKeys::Down
			|| Key == EKeys::Gamepad_DPad_Up || Key == EKeys::Gamepad_DPad_Down))
	{
		const int32 Direction = Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up ? -1 : 1;
		return MoveMainMenuSelection(Direction)
			? FReply::Handled() : FReply::Unhandled();
	}
	if (WoTIsMenuBackKey(Key))
	{
		if (ActiveSettingsScreen.IsValid())
		{
			const TSharedPtr<SWoTSettingsScreen> Screen = ActiveSettingsScreen;
			Screen->HandleBack();
			return FReply::Handled();
		}
		if (bShowingPlayerConfig)
		{
			ReturnToMainMenu();
			return FReply::Handled();
		}
		return CanInteract() ? Resume() : FReply::Handled();
	}
	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FNavigationReply SWoTInGameMenuOverlay::OnNavigation(
	const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	// The stick navigates by analog events rather than keys; it moves the same
	// wrapping selection as the arrows and the D-pad.
	const EUINavigation Direction = InNavigationEvent.GetNavigationType();
	if (IsShowingMainMenu()
		&& (Direction == EUINavigation::Up || Direction == EUINavigation::Down)
		&& MoveMainMenuSelection(Direction == EUINavigation::Up ? -1 : 1))
	{
		return FNavigationReply::Stop();
	}
	return SCompoundWidget::OnNavigation(MyGeometry, InNavigationEvent);
}

bool SWoTInGameMenuOverlay::IsShowingMainMenu() const
{
	return !ActiveSettingsScreen.IsValid() && !bShowingPlayerConfig;
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
		&& !bShowingPlayerConfig;
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
