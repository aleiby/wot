#include "UI/SWoTButtonPrompts.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "UI/WoTControllerUIInput.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "WoTButtonPrompts"

namespace
{
// The face buttons wear the colours players know them by.
FLinearColor WoTPromptGlyph_FaceColor(EWoTPromptButton Button)
{
	switch (Button)
	{
	case EWoTPromptButton::A: return FLinearColor::FromSRGBColor(FColor(96, 176, 72));
	case EWoTPromptButton::B: return FLinearColor::FromSRGBColor(FColor(214, 66, 56));
	case EWoTPromptButton::X: return FLinearColor::FromSRGBColor(FColor(62, 120, 222));
	case EWoTPromptButton::Y: return FLinearColor::FromSRGBColor(FColor(236, 192, 48));
	default: return FLinearColor::White;
	}
}

const TCHAR* WoTPromptGlyph_Letter(EWoTPromptButton Button)
{
	switch (Button)
	{
	case EWoTPromptButton::A: return TEXT("A");
	case EWoTPromptButton::B: return TEXT("B");
	case EWoTPromptButton::X: return TEXT("X");
	case EWoTPromptButton::Y: return TEXT("Y");
	default: return nullptr;
	}
}

const FSlateBrush* WoTPromptGlyph_Disc()
{
	// Half-height rounding: a circle in a square box.
	static const FSlateRoundedBoxBrush Brush(FLinearColor::White);
	return &Brush;
}

const FSlateBrush* WoTPromptGlyph_Bar()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 1.5f);
	return &Brush;
}

const FSlateBrush* WoTPromptGlyph_Edge()
{
	// A plain fill for a frame's edges. FSlateDrawElement::MakeBox colours a
	// box, rounded or not, with its tint argument and never the brush's own
	// tint (SlateCore DrawElementTypes.cpp, MakeBoxInternal's SetTint(InTint);
	// ElementBatcher.cpp fills with the element's tint), so a "transparent"
	// rounded frame drawn with an opaque tint is a solid box. A hollow frame is
	// therefore drawn as four edge bars.
	static const FSlateColorBrush Brush(FLinearColor::White);
	return &Brush;
}

/** One controller button, drawn with Slate boxes and text: no textures. */
class SWoTPromptGlyph final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTPromptGlyph) : _Size(24.0f) {}
		SLATE_ATTRIBUTE(EWoTPromptButton, Button)
		SLATE_ATTRIBUTE(float, Size)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Button = InArgs._Button;
		Size = InArgs._Size;
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		const float Side = FMath::Max(Size.Get(24.0f), 4.0f);
		return FVector2D(Side, Side);
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& AllottedGeometry,
		const FSlateRect&, FSlateWindowElementList& OutDrawElements, int32 LayerId,
		const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const FVector2f Local = AllottedGeometry.GetLocalSize();
		const float Side = FMath::Min(Local.X, Local.Y);
		if (Side <= 0.0f)
		{
			return LayerId;
		}
		const FVector2f Origin((Local.X - Side) * 0.5f, (Local.Y - Side) * 0.5f);
		const ESlateDrawEffect Effect = ShouldBeEnabled(bParentEnabled)
			? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;
		const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();
		auto Box = [&](float X, float Y, float W, float H, const FSlateBrush* Brush,
			const FLinearColor& Color, int32 Layer)
		{
			FSlateDrawElement::MakeBox(OutDrawElements, Layer,
				AllottedGeometry.ToPaintGeometry(FVector2f(W, H),
					FSlateLayoutTransform(Origin + FVector2f(X, Y))),
				Brush, Effect, Color * Tint);
		};
		const EWoTPromptButton Which = Button.Get(EWoTPromptButton::A);
		const FLinearColor Light(0.92f, 0.90f, 0.85f, 1.0f);
		const FLinearColor Dim(0.45f, 0.43f, 0.40f, 1.0f);
		// A hollow frame: four edge bars, so the lines inside stay visible.
		auto Frame = [&](float X, float Y, float W, float H, int32 Layer)
		{
			const float Edge = FMath::Clamp(Side * 0.06f, 1.0f, FMath::Min(W, H) * 0.5f);
			Box(X, Y, W, Edge, WoTPromptGlyph_Edge(), Light, Layer);
			Box(X, Y + H - Edge, W, Edge, WoTPromptGlyph_Edge(), Light, Layer);
			Box(X, Y + Edge, Edge, H - 2.0f * Edge, WoTPromptGlyph_Edge(), Light, Layer);
			Box(X + W - Edge, Y + Edge, Edge, H - 2.0f * Edge, WoTPromptGlyph_Edge(),
				Light, Layer);
		};
		if (const TCHAR* Letter = WoTPromptGlyph_Letter(Which))
		{
			Box(0.0f, 0.0f, Side, Side, WoTPromptGlyph_Disc(),
				WoTPromptGlyph_FaceColor(Which), LayerId);
			const FSlateFontInfo LetterFont = FCoreStyle::GetDefaultFontStyle(
				TEXT("Bold"), FMath::Max(6, FMath::RoundToInt(Side * 0.5f)));
			FVector2f Measured(Side * 0.4f, Side * 0.6f);
			if (FSlateApplication::IsInitialized()
				&& FSlateApplication::Get().GetRenderer() != nullptr)
			{
				Measured = FVector2f(FSlateApplication::Get().GetRenderer()
					->GetFontMeasureService()->Measure(FString(Letter), LetterFont));
			}
			const FLinearColor Ink = Which == EWoTPromptButton::Y
				? FLinearColor(0.05f, 0.05f, 0.05f, 1.0f) : FLinearColor::White;
			FSlateDrawElement::MakeText(OutDrawElements, LayerId + 1,
				AllottedGeometry.ToPaintGeometry(Measured, FSlateLayoutTransform(
					Origin + (FVector2f(Side, Side) - Measured) * 0.5f)),
				FString(Letter), LetterFont, Effect, Ink * Tint);
			return LayerId + 1;
		}
		switch (Which)
		{
		case EWoTPromptButton::View:
		{
			// A small frame with two overlapping windows.
			Frame(Side * 0.08f, Side * 0.2f, Side * 0.84f, Side * 0.6f, LayerId);
			Frame(Side * 0.26f, Side * 0.32f, Side * 0.3f, Side * 0.22f, LayerId + 1);
			Frame(Side * 0.42f, Side * 0.44f, Side * 0.3f, Side * 0.22f, LayerId + 1);
			return LayerId + 1;
		}
		case EWoTPromptButton::Menu:
		{
			// A small frame with three lines.
			Frame(Side * 0.08f, Side * 0.2f, Side * 0.84f, Side * 0.6f, LayerId);
			for (int32 Line = 0; Line < 3; ++Line)
			{
				Box(Side * 0.28f, Side * (0.33f + 0.13f * Line), Side * 0.44f,
					FMath::Max(Side * 0.06f, 1.0f), WoTPromptGlyph_Bar(), Light, LayerId + 1);
			}
			return LayerId + 1;
		}
		case EWoTPromptButton::DPadLeftRight:
		case EWoTPromptButton::DPadUp:
		default:
		{
			// The D-pad cross; the arms that act are drawn bright.
			const float Arm = Side * 0.32f;
			const float Mid = (Side - Arm) * 0.5f;
			const bool bHorizontal = Which == EWoTPromptButton::DPadLeftRight;
			Box(Mid, 0.0f, Arm, Side, WoTPromptGlyph_Bar(), Dim, LayerId);
			Box(0.0f, Mid, Side, Arm, WoTPromptGlyph_Bar(),
				bHorizontal ? Light : Dim, LayerId + 1);
			if (!bHorizontal)
			{
				Box(Mid, 0.0f, Arm, Mid + Arm * 0.5f, WoTPromptGlyph_Bar(), Light, LayerId + 1);
			}
			return LayerId + 1;
		}
		}
	}

private:
	TAttribute<EWoTPromptButton> Button;
	TAttribute<float> Size;
};
}

// ---------------------------------------------------------------------------
// Prompts and prompt sets

FString FWoTButtonPrompt::ButtonName(EWoTPromptButton Button)
{
	switch (Button)
	{
	case EWoTPromptButton::A: return TEXT("A");
	case EWoTPromptButton::B: return TEXT("B");
	case EWoTPromptButton::X: return TEXT("X");
	case EWoTPromptButton::Y: return TEXT("Y");
	case EWoTPromptButton::View: return TEXT("View");
	case EWoTPromptButton::Menu: return TEXT("Menu");
	case EWoTPromptButton::DPadLeftRight: return TEXT("D-pad Left/Right");
	case EWoTPromptButton::DPadUp: return TEXT("D-pad Up");
	default: return TEXT("?");
	}
}

FString FWoTButtonPrompt::ToString() const
{
	return ButtonName(Button) + TEXT(" ") + Label.ToString();
}

FString FWoTButtonPrompt::Describe(TConstArrayView<FWoTButtonPrompt> Prompts)
{
	TArray<FString> Parts;
	for (const FWoTButtonPrompt& Prompt : Prompts)
	{
		Parts.Add(Prompt.ToString());
	}
	return FString::Join(Parts, TEXT(" | "));
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Select()
{
	return {{EWoTPromptButton::A, LOCTEXT("Select", "Select")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Notice()
{
	return {{EWoTPromptButton::A, LOCTEXT("OK", "OK")},
		{EWoTPromptButton::B, LOCTEXT("Close", "Close")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::SelectBack()
{
	return {{EWoTPromptButton::A, LOCTEXT("Select", "Select")},
		{EWoTPromptButton::B, LOCTEXT("Back", "Back")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Overlay(const FText& DismissLabel)
{
	return {{EWoTPromptButton::A, LOCTEXT("Select", "Select")},
		{EWoTPromptButton::B, DismissLabel}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Password()
{
	return {{EWoTPromptButton::A, LOCTEXT("Type", "Type")},
		{EWoTPromptButton::B, LOCTEXT("Cancel", "Cancel")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Settings()
{
	return {{EWoTPromptButton::A, LOCTEXT("Select", "Select")},
		{EWoTPromptButton::B, LOCTEXT("Back", "Back")},
		{EWoTPromptButton::DPadLeftRight, LOCTEXT("Adjust", "Adjust")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::KeepRevert()
{
	return {{EWoTPromptButton::A, LOCTEXT("Keep", "Keep")},
		{EWoTPromptButton::B, LOCTEXT("Revert", "Revert")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Dropdown()
{
	return {{EWoTPromptButton::A, LOCTEXT("Pick", "Pick")},
		{EWoTPromptButton::B, LOCTEXT("Close", "Close")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Close()
{
	return {{EWoTPromptButton::B, LOCTEXT("Close", "Close")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Taunts()
{
	return {{EWoTPromptButton::A, LOCTEXT("Taunt", "Taunt")},
		{EWoTPromptButton::B, LOCTEXT("Back", "Back")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Capture()
{
	return {{EWoTPromptButton::B, LOCTEXT("Cancel", "Cancel")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::KeyboardPanel()
{
	// The owner's Steam Deck ruling (2026-11-09); View is deliberately absent.
	return {{EWoTPromptButton::A, LOCTEXT("Type", "Type")},
		{EWoTPromptButton::B, LOCTEXT("Cancel", "Cancel")},
		{EWoTPromptButton::X, LOCTEXT("Backspace", "Backspace")},
		{EWoTPromptButton::Y, LOCTEXT("Space", "Space")},
		{EWoTPromptButton::Menu, LOCTEXT("Done", "Done")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::SteamKeyboard()
{
	return {{EWoTPromptButton::Y, LOCTEXT("KeyboardPanel", "Keyboard panel")},
		{EWoTPromptButton::B, LOCTEXT("Cancel", "Cancel")}};
}

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::SaveChanges()
{
	return {{EWoTPromptButton::A, LOCTEXT("Select", "Select")},
		{EWoTPromptButton::B, LOCTEXT("KeepEditing", "Keep Editing")}};
}

// ---------------------------------------------------------------------------
// The strip

void SWoTButtonPrompts::Construct(const FArguments& InArgs)
{
	Prompts = InArgs._Prompts;
	Font = InArgs._Font;
	SetVisibility(TAttribute<EVisibility>::CreateLambda([this]()
	{
		// Never in the way of the pointer, and no room taken in Pointer mode.
		return CurrentPrompts().IsEmpty()
			? EVisibility::Collapsed : EVisibility::HitTestInvisible;
	}));
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (int32 Index = 0; Index < MaxPrompts; ++Index)
	{
		Row->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(Index == 0 ? 0.0f : 18.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this, Index]() { return PromptVisibility(Index); })
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SWoTPromptGlyph)
				.Button_Lambda([this, Index]()
				{
					const TArray<FWoTButtonPrompt> Shown = CurrentPrompts();
					return Shown.IsValidIndex(Index) ? Shown[Index].Button : EWoTPromptButton::A;
				})
				.Size_Lambda([this]()
				{
					return GlyphSizeFor(LabelFont());
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(this, &SWoTButtonPrompts::LabelFont)
				.ColorAndOpacity(InArgs._LabelColor)
				.Text_Lambda([this, Index]()
				{
					const TArray<FWoTButtonPrompt> Shown = CurrentPrompts();
					return Shown.IsValidIndex(Index) ? Shown[Index].Label : FText::GetEmpty();
				})
			]
		];
	}
	ChildSlot
	[
		Row
	];
}

bool SWoTButtonPrompts::IsShownForCurrentInput()
{
	return FWoTControllerUIInput::IsControllerMode();
}

TArray<FWoTButtonPrompt> SWoTButtonPrompts::CurrentPrompts() const
{
	if (!IsShownForCurrentInput())
	{
		return {};
	}
	return Prompts.Get(TArray<FWoTButtonPrompt>());
}

TArray<FWoTButtonPrompt> SWoTButtonPrompts::GetShownPrompts() const
{
	TArray<FWoTButtonPrompt> Shown = CurrentPrompts();
	if (Shown.Num() > MaxPrompts)
	{
		Shown.SetNum(MaxPrompts);
	}
	return Shown;
}

FSlateFontInfo SWoTButtonPrompts::LabelFont() const
{
	// The prepass hands every widget its accumulated layout scale (the game
	// viewport's DPI scale included) before its children measure their text.
	return LegibleLabelFont(Font.Get(FSlateFontInfo()), GetPrepassLayoutScaleMultiplier());
}

FSlateFontInfo SWoTButtonPrompts::LegibleLabelFont(const FSlateFontInfo& InFont,
	float LayoutScale)
{
	// An empty face would fall back to the last-resort font.
	FSlateFontInfo Result = InFont.HasValidFont()
		? InFont : FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	const float PixelsPerPoint = static_cast<float>(FontConstants::RenderDPI) / 72.0f
		* FMath::Max(LayoutScale, 0.01f);
	// The tolerance keeps a size that lands exactly on the floor from rounding
	// up a whole point through float error.
	const float Floor = FMath::CeilToFloat(
		MinimumLabelPixels / PixelsPerPoint - UE_KINDA_SMALL_NUMBER);
	Result.Size = FMath::Max(Result.Size, Floor);
	return Result;
}

float SWoTButtonPrompts::GlyphSizeFor(const FSlateFontInfo& InLabelFont)
{
	return FMath::Max(14.0f, InLabelFont.Size * GlyphSizePerPoint);
}

float SWoTButtonPrompts::BarHeightFor(const FSlateFontInfo& InFont, float LayoutScale)
{
	const FSlateFontInfo Label = LegibleLabelFont(InFont, LayoutScale);
	float LineHeight = Label.Size * static_cast<float>(FontConstants::RenderDPI) / 72.0f;
	if (FSlateApplication::IsInitialized())
	{
		if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer())
		{
			LineHeight = Renderer->GetFontMeasureService()->GetMaxCharacterHeight(Label);
		}
	}
	return FMath::Max(GlyphSizeFor(Label), LineHeight);
}

#if WITH_AUTOMATION_TESTS
TSharedRef<SWidget> SWoTButtonPrompts::MakeGlyphForTesting(EWoTPromptButton Button, float Size)
{
	return SNew(SWoTPromptGlyph).Button(Button).Size(Size);
}
#endif

EVisibility SWoTButtonPrompts::PromptVisibility(int32 Index) const
{
	return CurrentPrompts().IsValidIndex(Index) ? EVisibility::HitTestInvisible
		: EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
