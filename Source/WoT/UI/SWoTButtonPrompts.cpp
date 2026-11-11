#include "UI/SWoTButtonPrompts.h"

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

const FSlateBrush* WoTPromptGlyph_Frame()
{
	static const FSlateRoundedBoxBrush Brush(FLinearColor::Transparent, 4.0f,
		FLinearColor::White, 1.5f);
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
			// A small rounded frame with two overlapping windows.
			Box(Side * 0.08f, Side * 0.2f, Side * 0.84f, Side * 0.6f,
				WoTPromptGlyph_Frame(), Light, LayerId);
			Box(Side * 0.26f, Side * 0.32f, Side * 0.3f, Side * 0.22f,
				WoTPromptGlyph_Frame(), Light, LayerId + 1);
			Box(Side * 0.42f, Side * 0.44f, Side * 0.3f, Side * 0.22f,
				WoTPromptGlyph_Frame(), Light, LayerId + 1);
			return LayerId + 1;
		}
		case EWoTPromptButton::Menu:
		{
			// A small rounded frame with three lines.
			Box(Side * 0.08f, Side * 0.2f, Side * 0.84f, Side * 0.6f,
				WoTPromptGlyph_Frame(), Light, LayerId);
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

TArray<FWoTButtonPrompt> FWoTButtonPromptSets::Capture()
{
	return {{EWoTPromptButton::B, LOCTEXT("Cancel", "Cancel")}};
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
					// About the label's cap height plus its ascender room.
					return FMath::Max(14.0f, LabelFont().Size * 1.4f);
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
	const FSlateFontInfo Bound = Font.Get(FSlateFontInfo());
	// An empty face would fall back to the last-resort font.
	return Bound.HasValidFont() ? Bound : FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
}

EVisibility SWoTButtonPrompts::PromptVisibility(int32 Index) const
{
	return CurrentPrompts().IsValidIndex(Index) ? EVisibility::HitTestInvisible
		: EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
