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
#include "Widgets/Layout/SSpacer.h"
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

	// The game menu's chrome around its pages: the background border's padding
	// (SBorder's default, written out) and the frame's.
	constexpr float WoTInGameMenu_BorderPadding = 2.0f;
	constexpr float WoTInGameMenu_FramePadding = 36.0f;
	// The least room kept between the menu and the screen's top and bottom.
	constexpr float WoTInGameMenu_ScreenMargin = 24.0f;
	// The settings, player and taunt pages' panel height, as before.
	constexpr float WoTInGameMenu_PageHeight = 620.0f;
	// The heading's gap above the entries.
	constexpr float WoTInGameMenu_HeadingGap = 24.0f;

	// Seam extension rows (FWoTOnboardingSeams::MenuRows) sit after the other
	// entries, just above Quit; their ids carry the row index offset past the
	// retail enum, so the Tick snapshot/live comparison treats them like any
	// other entry and rebuilds when the registry changes under the open menu.
	constexpr uint8 WoTInGameSlate_ExtensionIdBase = 0x80;

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
			{EWoTInGameMenuEntry::Hardware, TEXT("Hardware")}
		});
		// Seam extension rows, just above Quit, which stays last [I9]; an
		// empty registry adds nothing, so a build without the onboarding
		// plugin renders exactly the retail list. Labels resolve here
		// (snapshot time) so Tick's live/snapshot comparison follows label
		// changes too.
		const TArray<FWoTOnboardingRow>& ExtensionRows =
			FWoTOnboardingSeams::Get().MenuRows;
		const int32 ExtensionCapacity = 0xFF - WoTInGameSlate_ExtensionIdBase;
		for (int32 RowIndex = 0;
			RowIndex < ExtensionRows.Num() && RowIndex < ExtensionCapacity;
			++RowIndex)
		{
			const FWoTOnboardingRow& Row = ExtensionRows[RowIndex];
			Entries.Add({static_cast<EWoTInGameMenuEntry>(
					WoTInGameSlate_ExtensionIdBase + RowIndex),
				Row.Label ? Row.Label() : FString()});
		}
		Entries.Add({EWoTInGameMenuEntry::Quit, TEXT("Quit to Main Menu")});
		return Entries;
	}
}

void SWoTChatInputOverlay::Construct(const FArguments& InArgs)
{
	PlayerController = InArgs._PlayerController;
	PresentationAuthority = InArgs._PresentationAuthority;
	if (InArgs._InitialPhysicalSize.X > 0.0f && InArgs._InitialPhysicalSize.Y > 0.0f)
	{
		// The game layer's scale is the DPI curve's (UIScale aside); Tick
		// takes the live values from the first frame on.
		DpiScale = FWoTMenuStyle::ViewportDpiScale(InArgs._InitialPhysicalSize);
		LayoutScale = DpiScale;
	}
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
		.Font(this, &SWoTChatInputOverlay::LineFont)
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

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			// Hosts the keyboard panel over chat (spec r3 section 3 [I7]).
			SAssignNew(EntryLayer, SWoTTextEntryLayer)
			.Font(this, &SWoTChatInputOverlay::LineFont)
			// The dark strip behind the prompt strip below: its 8-unit
			// bottom padding, and as much again above it.
			.PromptBarMargin(FMargin(0.0f, 8.0f, 0.0f, 8.0f))
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				.VAlign(VAlign_Bottom)
				.Padding(2.0f)
				[
					SAssignNew(LineStrip, SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
					.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f))
					.Padding(FMargin(3.0f, 1.0f))
					[
						SNew(SBox)
						.MinDesiredHeight(this, &SWoTChatInputOverlay::LineStripMinHeight)
						.VAlign(VAlign_Center)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(WoTInGameSlate_Text(TEXT("(> ")))
								.Font(this, &SWoTChatInputOverlay::LineFont)
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
								.Font(this, &SWoTChatInputOverlay::LineFont)
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
							+ SHorizontalBox::Slot().AutoWidth()
							[
								// Keeps the typed text clear of the prompt strip.
								SNew(SSpacer)
								.Size(this, &SWoTChatInputOverlay::PromptRoom)
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
			// The keyboard's prompts while it is open over chat; on the bare
			// line, A types and B closes chat.
			SAssignNew(ButtonPrompts, SWoTButtonPrompts)
			.Prompts_Lambda([this]()
			{
				return FWoTTextEntry::IsActiveWithin(AsShared())
					? FWoTTextEntry::ControllerPrompts() : FWoTButtonPromptSets::Password();
			})
			.Font(this, &SWoTChatInputOverlay::LineFont)
		]
	];
	RegisterControllerBack();
}

void SWoTChatInputOverlay::Tick(const FGeometry& AllottedGeometry,
	const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// The overlay fills the game viewport, so its size at the accumulated
	// layout scale is the viewport's pixel size.
	const float Scale = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
	const FVector2D Physical = AllottedGeometry.GetLocalSize() * Scale;
	if (Physical.X > 0.0 && Physical.Y > 0.0)
	{
		LayoutScale = FMath::Max(Scale, 0.01f);
		DpiScale = FWoTMenuStyle::ViewportDpiScale(Physical);
	}
}

FSlateFontInfo SWoTChatInputOverlay::LineFont() const
{
	if (!RequiredFonts.IsValid())
	{
		return FSlateFontInfo();
	}
	if (FWoTMenuStyle::IsCompactScreen(DpiScale))
	{
		// The Steam Deck legibility pass: the 14-pixel strike scaled by Slate
		// drew 13.8 soft pixels at 1280x800; the 2x strike is 28, crisp.
		return FWoTMenuStyle::LegibleStrikeFont(PresentationAuthority.Get(),
			FName(TEXT("F_WOTReg14")), RequiredFonts->List, LayoutScale);
	}
	return RequiredFonts->List;
}

FOptionalSize SWoTChatInputOverlay::LineStripMinHeight() const
{
	if (!FWoTMenuStyle::IsCompactScreen(DpiScale) || !ButtonPrompts.IsValid()
		|| ButtonPrompts->GetShownPrompts().IsEmpty())
	{
		return FOptionalSize();
	}
	// The prompt strip stands 8 units above the screen's bottom edge and the
	// line's strip 2 (plus 1 of padding): this keeps the prompts inside the
	// strip with about 6 units of it below them and 7 above.
	return FOptionalSize(
		SWoTButtonPrompts::BarHeightFor(LineFont(), LayoutScale) + 12.0f);
}

FVector2D SWoTChatInputOverlay::PromptRoom() const
{
	if (!FWoTMenuStyle::IsCompactScreen(DpiScale) || !ButtonPrompts.IsValid()
		|| ButtonPrompts->GetShownPrompts().IsEmpty())
	{
		return FVector2D::ZeroVector;
	}
	// The strip's width, its 16-unit right margin less the line's own 5, and
	// a 12-unit gap before it.
	return FVector2D(ButtonPrompts->GetDesiredSize().X + 23.0f, 0.0f);
}

void SWoTChatInputOverlay::RegisterControllerBack()
{
	// A controller backs out of chat one level per press, as Escape does from
	// the keyboard: the on-screen keyboard closes first (its own session entry
	// sits above this one), then B or View on the bare line closes chat
	// without sending anything.
	BackStackEntry = FWoTBackStack::Get().Register(AsShared(),
		[this]() { return !FWoTTextEntry::IsActiveWithin(AsShared()); },
		[WeakController = PlayerController]()
		{
			if (AWoTPlayerController* Controller = WeakController.Get())
			{
				Controller->CancelChatInput();
			}
			return true;
		});
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
			// The dark strip behind the prompt strip below (24 under it).
			.PromptBarMargin(FMargin(0.0f, 12.0f, 0.0f, 24.0f))
			[
			SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
			SAssignNew(MenuBackground, SBorder)
			.BorderImage(FWoTMenuStyle::Get().Background())
			.Padding(WoTInGameMenu_BorderPadding)
			[
			SNew(SBorder)
			.BorderImage(FWoTMenuStyle::Get().Frame())
			.ForegroundColor(FWoTMenuStyle::MenuColor(false))
			.Padding(WoTInGameMenu_FramePadding)
			[
				// The main menu sizes to its entries, which Tick fits to the
				// screen; the pages keep their fixed panel.
				SNew(SBox)
				.WidthOverride_Lambda([this]()
				{
					return ActiveSettingsScreen.IsValid() || bShowingPlayerConfig
						? FOptionalSize(760.0f) : FOptionalSize();
				})
				.MinDesiredWidth_Lambda([this]()
				{
					return ActiveSettingsScreen.IsValid() || bShowingPlayerConfig
						? FOptionalSize() : FOptionalSize(420.0f);
				})
				.MaxDesiredHeight_Lambda([this]()
				{
					return FOptionalSize(PageMaxHeight());
				})
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
	// Captured with the labels: extension activation must run the invoke that
	// belongs to the rendered row even if the registry changes before Tick's
	// snapshot comparison rebuilds the list.
	ExtensionMenuRows = FWoTOnboardingSeams::Get().MenuRows;
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
		.Padding(0.0f, 0.0f, 0.0f, WoTInGameMenu_HeadingGap)
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
	const float PreviousScreenHeight = LiveScreenHeight;
	LiveViewportScale = FMath::Max(
		AllottedGeometry.GetAccumulatedLayoutTransform().GetScale(), 0.01f);
	LiveScreenHeight = AllottedGeometry.GetLocalSize().Y;
	if (ActiveSettingsScreen.IsValid() || bShowingPlayerConfig || bShowingTaunts)
	{
		if (!FMath::IsNearlyEqual(PreviousViewportScale, LiveViewportScale)
			|| !FMath::IsNearlyEqual(PreviousScreenHeight, LiveScreenHeight))
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
		+ WoTInGameMenu_HeadingGap;
	const int32 EntryCount = MainMenuEntryCount();
	// menuWOT's spacing over the room the entries really have: the screen
	// less its margins, the menu's chrome and the heading. Where the ideal
	// spacing does not fit (a Battle edit phase has the most entries), the
	// spacing compacts and the text keeps its size, so the background always
	// encloses the last entry on screen.
	EntrySpacing = FWoTMenuLayout::CalculateSpacing(
		EntryCount, EntryHeight, MainMenuContentRoom() - TitleArea);
	ListHeight = FWoTMenuLayout::CalculateListHeight(
		EntryCount, EntryHeight, EntrySpacing);
	if (!FMath::IsNearlyEqual(PreviousViewportScale, LiveViewportScale)
		|| !FMath::IsNearlyEqual(PreviousScreenHeight, LiveScreenHeight)
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

FReply SWoTInGameMenuOverlay::ActivateExtensionEntry(int32 RowIndex)
{
	if (!ExtensionMenuRows.IsValidIndex(RowIndex))
	{
		return FReply::Handled();
	}
	// Copy before Resume: leaving the menu releases the controller's reference
	// to this overlay, so members are not touched after it. Activation mirrors
	// ActivateHelp: leave the menu first, then run the row's action.
	const FWoTOnboardingRow Row = ExtensionMenuRows[RowIndex];
	if (AWoTPlayerController* Controller = PlayerController.Get())
	{
		Controller->ResumeFromInGameMenu();
	}
	if (Row.Invoke)
	{
		Row.Invoke();
	}
	return FReply::Handled();
}

FReply SWoTInGameMenuOverlay::ActivateMainMenuEntryById(uint8 EntryId)
{
	// Extension ids sit past the retail enum; they must never reach the switch
	// below, whose default is the Quit entry.
	if (EntryId >= WoTInGameSlate_ExtensionIdBase)
	{
		return ActivateExtensionEntry(EntryId - WoTInGameSlate_ExtensionIdBase);
	}
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

float SWoTInGameMenuOverlay::MainMenuContentRoom() const
{
	// The screen less a margin top and bottom and the menu's chrome.
	constexpr float Chrome = 2.0f * (WoTInGameMenu_BorderPadding + WoTInGameMenu_FramePadding);
	return FMath::Max(0.0f, LiveScreenHeight - 2.0f * WoTInGameMenu_ScreenMargin - Chrome);
}

FOptionalSize SWoTInGameMenuOverlay::PageMaxHeight() const
{
	const bool bPage = ActiveSettingsScreen.IsValid() || bShowingPlayerConfig || bShowingTaunts;
	if (LiveScreenHeight <= 0.0f)
	{
		// Before the first Tick: the pages' panel, the main menu unbounded.
		return bPage ? FOptionalSize(WoTInGameMenu_PageHeight) : FOptionalSize();
	}
	const float Room = MainMenuContentRoom();
	return FOptionalSize(bPage ? FMath::Min(WoTInGameMenu_PageHeight, Room) : Room);
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
		+ WoTInGameMenu_HeadingGap + ListHeight;
}
#endif

namespace
{
	/** WOTTextWindow's inline command interpreter (WOTTextWindow.uc): the
	 * state one command leaves for the next, and the lines it lays out in a
	 * clip ClipWidth wide. */
	struct FWoTHelpInterpreter
	{
		int32 FontIndex = 3;
		bool bCentered = true;
		float ForcedX = -1.0f;
		float ForcedY = -1.0f;
		float CurrentY = 0.0f;

		void Run(TConstArrayView<FString> Commands, float ClipWidth,
			const TFunction<FVector2D(const FString&, int32)>& Measure,
			TArray<FWoTHelpLayoutLine>& Out)
		{
			for (const FString& Command : Commands)
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

					FWoTHelpLayoutLine& Line = Out.AddDefaulted_GetRef();
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
		}
	};

	/** The lowest line bottom of Lines from index First on; Top when none. */
	float WoTHelp_LinesBottom(const TArray<FWoTHelpLayoutLine>& Lines,
		int32 First, float Top)
	{
		float Bottom = Top;
		for (int32 Index = First; Index < Lines.Num(); ++Index)
		{
			Bottom = FMath::Max(Bottom, Lines[Index].Y + Lines[Index].Height);
		}
		return Bottom;
	}
}

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

FName FWoTHelpContent::FaceFor(int32 FontIndex, int32 PixelScale)
{
	// WOTTextWindow.uc:57-78 (GetStrFont).
	if (FontIndex == 0 || FontIndex == 1)
	{
		// Steam Deck legibility: the 8-pixel face at a 1x strike is under the
		// 12-pixel floor, and a fractional strike would blur it. F_WOTReg14 is
		// the next face of the family; its letters are about a tenth wider and
		// its lines 6 px taller, which only matters for help text that selects
		// fonts 0/1 (the shipped English help never does).
		const FName SmallFace(TEXT("F_WOTReg08"));
		return RetailLineHeight(SmallFace) * FMath::Max(PixelScale, 1)
				< FWoTMenuStyle::FloorLegiblePixels
			? FName(TEXT("F_WOTReg14")) : SmallFace;
	}
	if (FontIndex == 2) return FName(TEXT("F_WOTIta14"));
	if (FontIndex == 4 || FontIndex == 5) return FName(TEXT("F_WOTReg30"));
	return FName(TEXT("F_WOTReg14"));
}

int32 FWoTHelpContent::RetailLineHeight(FName Face)
{
	// Tallest glyph cell of each shipped WOT.u face, which the runtime font
	// owner uses as the line height (WoT.UI.Legibility.HelpRetailGlyphs pins
	// these against the converted retail faces).
	return FWoTMenuStyle::RetailFaceLineHeight(Face);
}

FVector2D FWoTHelpContent::MeasureRetail(const FString& Text, int32 FontIndex,
	int32 PixelScale)
{
	const FName Face = FaceFor(FontIndex, PixelScale);
	// Rough advances kept from the original spec metric; the shipped faces
	// average about 5.2 (Reg08), 5.8 (Reg14), 6.1 (Ita14) and 9.7 (Reg30)
	// pixels a glyph. WoT.UI.Legibility.HelpRetailGlyphs lays the page out
	// in the real advances.
	const float AverageWidth = Face == FName(TEXT("F_WOTReg08")) ? 5.0f
		: (Face == FName(TEXT("F_WOTReg30")) ? 18.0f : 8.0f);
	return FVector2D(Text.Len() * AverageWidth,
		static_cast<float>(RetailLineHeight(Face)));
}

TArray<FWoTHelpLayoutLine> FWoTHelpContent::Layout(
	const FWoTHelpContent& In, int32 PixelScale)
{
	return Layout(In, [PixelScale](const FString& Text, int32 Index)
	{
		return MeasureRetail(Text, Index, PixelScale);
	});
}

FWoTHelpPageLayout FWoTHelpContent::LayoutPage(const FWoTHelpContent& In,
	int32 PixelScale)
{
	return LayoutPage(In, [PixelScale](const FString& Text, int32 Index)
	{
		return MeasureRetail(Text, Index, PixelScale);
	});
}

FWoTHelpPageLayout FWoTHelpContent::LayoutReflow(const FWoTHelpContent& In,
	int32 PixelScale, const FVector2D& ClipSize)
{
	return LayoutReflow(In, [PixelScale](const FString& Text, int32 Index)
	{
		return MeasureRetail(Text, Index, PixelScale);
	}, ClipSize);
}

FWoTHelpPageLayout FWoTHelpContent::LayoutReflow(const FWoTHelpContent& In,
	const TFunction<FVector2D(const FString&, int32)>& Measure,
	const FVector2D& ClipSize)
{
	FWoTHelpPageLayout Page;
	Page.ClipSize = ClipSize;
	Page.TitleSize = Measure(In.Title, 3);
	Page.TitleSize.Y = FMath::Max(1.0f, Page.TitleSize.Y);
	const TConstArrayView<FString> Commands(In.Content);

	// The absolutely placed block starts at the first %y= that places a line
	// (the retail F-key table, %y=325); everything before it flows.
	int32 BlockStart = Commands.Num();
	for (int32 Index = 0; Index < Commands.Num(); ++Index)
	{
		if (Commands[Index].StartsWith(TEXT("%y="))
			&& FCString::Atoi(*Commands[Index].Mid(3)) >= 0)
		{
			BlockStart = Index;
			break;
		}
	}
	// Each headed section opens with the italic heading face (%f=2).
	TArray<int32> SectionStarts;
	for (int32 Index = 0; Index < BlockStart; ++Index)
	{
		if (Commands[Index].StartsWith(TEXT("%f="))
			&& FCString::Atoi(*Commands[Index].Mid(3)) == 2)
		{
			SectionStarts.Add(Index);
		}
	}
	const bool bColumns = SectionStarts.Num() >= 2;

	// DrawTextAt(Title, bCR=false) still advances CurY through WrappedPrint.
	FWoTHelpInterpreter Interpreter;
	Interpreter.CurrentY = Page.TitleSize.Y;
	const int32 IntroEnd = bColumns ? SectionStarts[0] : BlockStart;
	Interpreter.Run(Commands.Slice(0, IntroEnd), ClipSize.X, Measure, Page.Lines);
	float FlowBottom = WoTHelp_LinesBottom(Page.Lines, 0, Page.TitleSize.Y);
	if (bColumns)
	{
		// Two columns: the first Split sections on the left, the rest on the
		// right, split where the taller column is shortest.
		const float ColumnTop = FlowBottom;
		const float ColumnWidth = FMath::Max(1.0f, (ClipSize.X - ReflowColumnGap) * 0.5f);
		TArray<FWoTHelpLayoutLine> BestLines;
		FWoTHelpInterpreter BestState;
		float BestBottom = TNumericLimits<float>::Max();
		for (int32 Split = 1; Split < SectionStarts.Num(); ++Split)
		{
			TArray<FWoTHelpLayoutLine> Lines;
			FWoTHelpInterpreter Column = Interpreter;
			Column.CurrentY = ColumnTop;
			Column.Run(Commands.Slice(IntroEnd, SectionStarts[Split] - IntroEnd),
				ColumnWidth, Measure, Lines);
			const float LeftBottom = WoTHelp_LinesBottom(Lines, 0, ColumnTop);
			const int32 RightFirst = Lines.Num();
			Column.CurrentY = ColumnTop;
			Column.Run(Commands.Slice(SectionStarts[Split],
				BlockStart - SectionStarts[Split]), ColumnWidth, Measure, Lines);
			for (int32 Index = RightFirst; Index < Lines.Num(); ++Index)
			{
				Lines[Index].X += ColumnWidth + ReflowColumnGap;
			}
			const float Bottom = FMath::Max(LeftBottom,
				WoTHelp_LinesBottom(Lines, RightFirst, ColumnTop));
			if (Bottom < BestBottom)
			{
				BestBottom = Bottom;
				BestLines = MoveTemp(Lines);
				BestState = Column;
			}
		}
		Page.Lines.Append(MoveTemp(BestLines));
		Interpreter = BestState;
		FlowBottom = BestBottom;
	}

	// The block keeps its own %x offsets and line order, moved up to sit one
	// gap below the flowing text.
	if (BlockStart < Commands.Num())
	{
		TArray<FWoTHelpLayoutLine> Block;
		Interpreter.Run(Commands.Slice(BlockStart, Commands.Num() - BlockStart),
			ClipSize.X, Measure, Block);
		float BlockTop = TNumericLimits<float>::Max();
		for (const FWoTHelpLayoutLine& Line : Block)
		{
			BlockTop = FMath::Min(BlockTop, Line.Y);
		}
		const float Offset = FlowBottom + ReflowBlockGap - BlockTop;
		for (FWoTHelpLayoutLine& Line : Block)
		{
			Line.Y += Offset;
		}
		Page.Lines.Append(MoveTemp(Block));
	}

	const float ContentEndY = WoTHelp_LinesBottom(Page.Lines, 0, Page.TitleSize.Y);
	Page.SubTitleSize = Measure(In.SubTitle, 3);
	Page.SubTitleSize.Y = FMath::Max(1.0f, Page.SubTitleSize.Y);
	// WOTTextWindow.uc:334: Max(CurY + SubTitleMarginY, ClipY - SubTitleOffsetY),
	// the offset never less than the subtitle's own height, so a face taller
	// than the retail 14 pixels still ends inside the clip.
	Page.SubTitleY = FMath::Max(ContentEndY + 8.0f,
		ClipSize.Y - FMath::Max(16.0f, Page.SubTitleSize.Y + 2.0f));
	return Page;
}

FWoTHelpPageLayout FWoTHelpContent::LayoutPage(const FWoTHelpContent& In,
	const TFunction<FVector2D(const FString&, int32)>& Measure)
{
	FWoTHelpPageLayout Page;
	Page.TitleSize = Measure(In.Title, 3);
	Page.TitleSize.Y = FMath::Max(1.0f, Page.TitleSize.Y);
	Page.Lines = Layout(In, Measure);
	float ContentEndY = Page.TitleSize.Y;
	for (const FWoTHelpLayoutLine& Line : Page.Lines)
	{
		ContentEndY = Line.Y + Line.Height;
	}
	// WOTTextWindow.uc:334: Max(CurY + SubTitleMarginY, ClipY - SubTitleOffsetY).
	Page.SubTitleY = FMath::Max(ContentEndY + 8.0f, ClipHeight - 16.0f);
	Page.SubTitleSize = Measure(In.SubTitle, 3);
	Page.SubTitleSize.Y = FMath::Max(1.0f, Page.SubTitleSize.Y);
	return Page;
}

bool FWoTHelpPageLayout::FitsClip() const
{
	const auto Inside = [this](float Left, float Top, float Width, float Height)
	{
		constexpr float Tolerance = 0.5f;
		return Left >= -Tolerance && Top >= -Tolerance
			&& Left + Width <= ClipSize.X + Tolerance
			&& Top + Height <= ClipSize.Y + Tolerance;
	};
	// The title and subtitle are centred across the clip's full width.
	if (!Inside((ClipSize.X - TitleSize.X) * 0.5f, 0.0f,
			TitleSize.X, TitleSize.Y)
		|| !Inside((ClipSize.X - SubTitleSize.X) * 0.5f,
			SubTitleY, SubTitleSize.X, SubTitleSize.Y))
	{
		return false;
	}
	for (const FWoTHelpLayoutLine& Line : Lines)
	{
		if (!Inside(Line.X, Line.Y, Line.Width, Line.Height)) return false;
	}
	return true;
}

float FWoTHelpPageLayout::ContentBottom() const
{
	float Bottom = FMath::Max(TitleSize.Y, SubTitleY + SubTitleSize.Y);
	for (const FWoTHelpLayoutLine& Line : Lines)
	{
		Bottom = FMath::Max(Bottom, Line.Y + Line.Height);
	}
	return Bottom;
}

TArray<FWoTHelpLayoutLine> FWoTHelpContent::Layout(
	const FWoTHelpContent& In,
	const TFunction<FVector2D(const FString&, int32)>& Measure)
{
	TArray<FWoTHelpLayoutLine> Lines;
	FWoTHelpInterpreter Interpreter;
	// DrawTextAt(Title, bCR=false) still advances CurY through WrappedPrint.
	Interpreter.CurrentY = FMath::Max(1.0f, Measure(In.Title, 3).Y);
	Interpreter.Run(In.Content, ClipWidth, Measure, Lines);
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
		LivePhysicalSize = InArgs._InitialPhysicalSize;
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

	SAssignNew(Canvas, SConstraintCanvas)
		.Clipping(EWidgetClipping::ClipToBounds);
	RebuildPage();

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
			// the HUD and in-game menus do. Where that scale would be 1, the
			// window fills the screen at 2x with its page reflowed.
			SNew(SScaleBox)
			.Stretch(EStretch::UserSpecified)
			.UserSpecifiedScale_Lambda([this]() { return WindowScale(); })
			[
				SNew(SBox)
				.WidthOverride_Lambda([this]() { return FOptionalSize(WindowSize().X); })
				.HeightOverride_Lambda([this]() { return FOptionalSize(WindowSize().Y); })
				[
					SNew(SBorder)
					.BorderImage(FWoTMenuStyle::Get().Frame())
					.ForegroundColor(FWoTMenuStyle::MenuColor(false))
					.Padding_Lambda([this]() { return WindowPadding(); })
					[
						SNew(SBox)
						.WidthOverride_Lambda([this]()
						{
							return FOptionalSize(PageLayout.ClipSize.X);
						})
						.HeightOverride_Lambda([this]()
						{
							return FOptionalSize(PageLayout.ClipSize.Y);
						})
						[
							Canvas.ToSharedRef()
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
			.Font_Lambda([this]() { return PromptFont(); })
		]
	];
}

bool SWoTHelpOverlay::ReflowsOn(const FVector2D& PhysicalSize)
{
	return PhysicalSize.X > 0.0 && PhysicalSize.Y > 0.0
		&& PixelScaleFor(PhysicalSize) == 1;
}

FVector2D SWoTHelpOverlay::ReflowWindowSizeFor(const FVector2D& PhysicalSize)
{
	return FVector2D(
		FMath::FloorToDouble(FMath::Max(PhysicalSize.X, 0.0) / ReflowDrawScale),
		FMath::FloorToDouble(FMath::Max(PhysicalSize.Y, 0.0) / ReflowDrawScale));
}

FVector2D SWoTHelpOverlay::ReflowClipSizeFor(const FVector2D& PhysicalSize)
{
	const FVector2D Window = ReflowWindowSizeFor(PhysicalSize);
	return FVector2D(FMath::Max(Window.X - 2.0 * ReflowPaddingX, 0.0),
		FMath::Max(Window.Y - 2.0 * ReflowPaddingY, 0.0));
}

FVector2D SWoTHelpOverlay::WindowSize() const
{
	return bReflowed ? ReflowWindowSizeFor(PageLayoutPhysicalSize)
		: FVector2D(640.0, 480.0);
}

FMargin SWoTHelpOverlay::WindowPadding() const
{
	// uiWindow's WindowBorderX/Y around the retail 512x432 clip.
	return bReflowed ? FMargin(ReflowPaddingX, ReflowPaddingY)
		: FMargin(64.0f, 24.0f);
}

FSlateFontInfo SWoTHelpOverlay::PromptFont() const
{
	if (PixelScale == 1 && RequiredFonts.IsValid())
	{
		// The strip sits outside the window's scale box, at the layout scale:
		// the list face at its 2x strike, 1:1 there (28 physical pixels).
		return FWoTMenuStyle::LegibleStrikeFont(PresentationAuthority.Get(),
			FName(TEXT("F_WOTReg14")), RequiredFonts->List, DpiScale);
	}
	return FontForIndex(3);
}

void SWoTHelpOverlay::RebuildPage()
{
	PageLayoutPixelScale = PixelScale;
	PageLayoutPhysicalSize = LivePhysicalSize;
	const auto MeasureAt = [this](int32 Scale)
	{
		return [this, Scale](const FString& Text, int32 Index)
		{
			const FSlateFontInfo Font = FontForIndexAt(Index, Scale);
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
	};
	// The Steam Deck legibility pass: where the 640x480 window would draw at
	// 1x, the page reflows across the whole screen at 2x when it fits there;
	// a page that does not (none shipped) keeps the retail window at 1x.
	bReflowed = false;
	if (ReflowsOn(LivePhysicalSize))
	{
		FWoTHelpPageLayout Reflowed = FWoTHelpContent::LayoutReflow(HelpContent,
			MeasureAt(ReflowDrawScale), ReflowClipSizeFor(LivePhysicalSize));
		if (Reflowed.FitsClip())
		{
			PageLayout = MoveTemp(Reflowed);
			bReflowed = true;
		}
	}
	if (!bReflowed)
	{
		PageLayout = FWoTHelpContent::LayoutPage(HelpContent, MeasureAt(PixelScale));
	}

	// Every slot is placed by its top-left corner, as the retail Canvas drew
	// at CurX/CurY. SConstraintCanvas defaults to a (0.5, 0.5) alignment,
	// which centred each line on its position: half of every left-aligned
	// line fell off the window, headings and the F-key columns shifted and
	// overlapped, and the title was cut in half.
	const FVector2D TopLeft(0.0f, 0.0f);
	Canvas->ClearChildren();
	Canvas->AddSlot()
	.Anchors(FAnchors(0.0f, 0.0f, 1.0f, 0.0f))
	.Offset(FMargin(0.0f, 0.0f, 0.0f, PageLayout.TitleSize.Y))
	.Alignment(TopLeft)
	[
		SNew(STextBlock)
		.Text(FText::FromString(HelpContent.Title))
		.Font_Lambda([this]() { return FontForIndex(3); })
		.Justification(ETextJustify::Center)
	];
	for (const FWoTHelpLayoutLine& Line : PageLayout.Lines)
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
	.Offset(FMargin(0.0f, PageLayout.SubTitleY, 0.0f, PageLayout.SubTitleSize.Y))
	.Alignment(TopLeft)
	[
		SNew(STextBlock)
		.Text(FText::FromString(HelpContent.SubTitle))
		.Font_Lambda([this]() { return FontForIndex(3); })
		.Justification(ETextJustify::Center)
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
	LivePhysicalSize = AllottedGeometry.GetLocalSize() * DpiScale;
	PixelScale = PixelScaleFor(LivePhysicalSize);
	// The reflowed page's clip follows the screen.
	if (PixelScale != PageLayoutPixelScale
		|| (ReflowsOn(LivePhysicalSize)
			&& !LivePhysicalSize.Equals(PageLayoutPhysicalSize, 0.5)))
	{
		RebuildPage();
	}
}

FSlateFontInfo SWoTHelpOverlay::FontForIndex(int32 FontIndex) const
{
	return FontForIndexAt(FontIndex, DrawScale());
}

FSlateFontInfo SWoTHelpOverlay::FontForIndexAt(int32 FontIndex, int32 Scale) const
{
	const FName Face = FWoTHelpContent::FaceFor(FontIndex, Scale);
	if (PresentationAuthority.IsValid() && PresentationAuthority->IsReady())
	{
		// The Scale strike at the window's own units: the box is drawn at
		// Scale screen pixels per unit, so each glyph meets its strike 1:1.
		return PresentationAuthority->MultiplayerFont(Face,
			static_cast<float>(Scale));
	}
	FSlateFontInfo Font = Face == FName(TEXT("F_WOTReg30"))
		? RequiredFonts->Title : RequiredFonts->List;
	if (Face == FName(TEXT("F_WOTReg08")))
	{
		Font.Size = static_cast<float>(FWoTHelpContent::RetailLineHeight(Face));
	}
	else if (Face == FName(TEXT("F_WOTIta14")))
	{
		Font.TypefaceFontName = TEXT("Italic");
	}
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
	PresentationAuthority = InArgs._PresentationAuthority;
	if (InArgs._InitialPhysicalSize.X > 0.0f && InArgs._InitialPhysicalSize.Y > 0.0f)
	{
		DpiScale = FWoTMenuStyle::ViewportDpiScale(InArgs._InitialPhysicalSize);
		LayoutScale = DpiScale;
		ViewportWidthUnits = InArgs._InitialPhysicalSize.X / DpiScale;
	}
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
	MenuFonts = MakeShared<FWoTRequiredMenuFonts>(CapturedFonts);
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
		// SWoTMenuText's large face, through TitleFont.
		SNew(STextBlock)
		.Text(FText::FromString(Content.Title))
		.Font(this, &SWoTItemInfoOverlay::TitleFont)
		.ColorAndOpacity(FSlateColor::UseForeground())
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
				.Font(this, &SWoTItemInfoOverlay::BodyFont)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Right)
			[
				SNew(STextBlock)
				.Font(this, &SWoTItemInfoOverlay::BodyFont)
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
				// Size 8 is 7.1 to 7.9 pixels on a 720p or 800p screen, under
				// the Steam Deck floors; GemFont follows live resizes.
				.Font(this, &SWoTItemInfoOverlay::GemFont)
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
		.Font(this, &SWoTItemInfoOverlay::BodyFont)
		.AutoWrapText(true)
		.WrapTextAt(this, &SWoTItemInfoOverlay::WrapWidth)
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
		.Font(this, &SWoTItemInfoOverlay::QuoteFont)
		.AutoWrapText(true)
		.WrapTextAt(this, &SWoTItemInfoOverlay::WrapWidth)
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
				.WidthOverride(this, &SWoTItemInfoOverlay::ContentWidth)
				[
					Body
				]
			]
		]
		]
	];
}

void SWoTItemInfoOverlay::Tick(const FGeometry& AllottedGeometry,
	const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// The overlay fills the game viewport, so its size at the accumulated
	// layout scale is the viewport's pixel size.
	const float Scale = AllottedGeometry.GetAccumulatedLayoutTransform().GetScale();
	const FVector2D PhysicalSize = AllottedGeometry.GetLocalSize() * Scale;
	if (PhysicalSize.X > 0.0 && PhysicalSize.Y > 0.0)
	{
		DpiScale = FWoTMenuStyle::ViewportDpiScale(PhysicalSize);
		LayoutScale = FMath::Max(Scale, 0.01f);
		ViewportWidthUnits = AllottedGeometry.GetLocalSize().X;
	}
}

bool SWoTItemInfoOverlay::IsCompact() const
{
	return FWoTMenuStyle::IsCompactScreen(DpiScale);
}

FSlateFontInfo SWoTItemInfoOverlay::BodyFont() const
{
	if (!MenuFonts.IsValid()) return FSlateFontInfo();
	return IsCompact()
		? FWoTMenuStyle::LegibleStrikeFont(PresentationAuthority.Get(),
			FName(TEXT("F_WOTReg14")), MenuFonts->List, LayoutScale)
		: MenuFonts->List;
}

FSlateFontInfo SWoTItemInfoOverlay::QuoteFont() const
{
	const FSlateFontInfo& Italic = FWoTMenuStyle::Get().ItalicFont();
	return IsCompact()
		? FWoTMenuStyle::LegibleStrikeFont(PresentationAuthority.Get(),
			FName(TEXT("F_WOTIta14")), Italic, LayoutScale)
		: Italic;
}

FSlateFontInfo SWoTItemInfoOverlay::TitleFont() const
{
	if (!MenuFonts.IsValid()) return FSlateFontInfo();
	// F_WOTReg30 already passes 16 pixels at 1x: crisp at 30, never fractional.
	return IsCompact()
		? FWoTMenuStyle::LegibleStrikeFont(PresentationAuthority.Get(),
			FName(TEXT("F_WOTReg30")), MenuFonts->Title, LayoutScale)
		: MenuFonts->Title;
}

FSlateFontInfo SWoTItemInfoOverlay::GemFont() const
{
	// On Deck-class screens the element names take the body's 2x strike
	// (replacing the floored small face); from 1080p up the retail size 8.
	return IsCompact() ? BodyFont() : FWoTMenuStyle::Get().LegibleSmallFont(DpiScale);
}

FOptionalSize SWoTItemInfoOverlay::ContentWidth() const
{
	if (!IsCompact())
	{
		return FOptionalSize(RetailContentWidth);
	}
	// The frame's 40-unit padding and the background's 2 each side, and a
	// 24-unit margin to the screen's edges.
	const float Room = ViewportWidthUnits > 0.0f
		? ViewportWidthUnits - 2.0f * (40.0f + 2.0f + 24.0f) : TNumericLimits<float>::Max();
	return FOptionalSize(FMath::Max(RetailContentWidth,
		FMath::Min(CompactContentPixels / FMath::Max(LayoutScale, 0.01f), Room)));
}

float SWoTItemInfoOverlay::WrapWidth() const
{
	if (!IsCompact())
	{
		return RetailWrapWidth;
	}
	return ContentWidth().Get()
		* (CompactWrapPixels / CompactContentPixels);
}

EVisibility SWoTItemInfoOverlay::VisibilityForFonts(bool bFontsValid)
{
	return bFontsValid ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
}
