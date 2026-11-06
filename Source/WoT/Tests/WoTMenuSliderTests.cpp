#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTMenuStyle.h"
#include "../Settings/WoTSettingsSlate.h"
#include "Styling/CoreStyle.h"

#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/HittestGrid.h"
#include "InputCoreTypes.h"
#include "Layout/ArrangedChildren.h"
#include "Misc/App.h"
#include "Rendering/DrawElements.h"
#include "Types/PaintArgs.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SWindow.h"

namespace
{
struct FWoTMenuSliderTests_ScopedArt
{
	FWoTMenuSliderTests_ScopedArt()
	{
		const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
		Brushes[0] = const_cast<FSlateBrush*>(Style.SliderLeftCap());
		Brushes[1] = const_cast<FSlateBrush*>(Style.SliderKnob());
		Brushes[2] = const_cast<FSlateBrush*>(Style.SliderRightCap());
		Brushes[3] = const_cast<FSlateBrush*>(Style.SliderEmptyCell());
		// Actual authenticated WOT.Slide1..4 are 8x8, not tall stand-ins.
		const FVector2D Sizes[4] = {
			FVector2D(8.0f, 8.0f), FVector2D(8.0f, 8.0f),
			FVector2D(8.0f, 8.0f), FVector2D(8.0f, 8.0f)};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			SavedSizes[Index] = Brushes[Index]->GetImageSize();
			SavedResources[Index] = Brushes[Index]->GetResourceObject();
			Textures[Index] = TStrongObjectPtr<UTexture2D>(
				UTexture2D::CreateTransient(
					static_cast<int32>(Sizes[Index].X),
					static_cast<int32>(Sizes[Index].Y)));
			Brushes[Index]->SetImageSize(Sizes[Index]);
			Brushes[Index]->SetResourceObject(Textures[Index].Get());
		}
	}

	~FWoTMenuSliderTests_ScopedArt()
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Brushes[Index]->SetImageSize(SavedSizes[Index]);
			Brushes[Index]->SetResourceObject(SavedResources[Index]);
		}
	}

	bool IsValid() const
	{
		for (const TStrongObjectPtr<UTexture2D>& Texture : Textures)
		{
			if (!Texture.IsValid())
			{
				return false;
			}
		}
		return true;
	}

	FSlateBrush* Brushes[4] = {};
	FVector2D SavedSizes[4];
	UObject* SavedResources[4] = {};
	TStrongObjectPtr<UTexture2D> Textures[4];
};

void WoTMenuSliderTests_CollectImages(const TSharedRef<SWidget>& Widget,
	TArray<TSharedRef<SImage>>& Images)
{
	if (Widget->GetType() == FName(TEXT("SImage")))
	{
		Images.Add(StaticCastSharedRef<SImage>(Widget));
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
	{
		WoTMenuSliderTests_CollectImages(Children->GetChildAt(Index), Images);
	}
}

bool WoTMenuSliderTests_ArrangeCells(const TSharedRef<SWoTRetailSlider>& Slider,
	const FGeometry& Geometry, FArrangedChildren& Cells)
{
	Slider->SlatePrepass(1.0f);
	FArrangedChildren SliderChildren(EVisibility::Visible);
	Slider->ArrangeChildren(Geometry, SliderChildren);
	if (SliderChildren.Num() != 1)
	{
		return false;
	}
	SliderChildren[0].Widget->ArrangeChildren(
		SliderChildren[0].Geometry, Cells);
	return Cells.Num() == 10;
}

FPointerEvent WoTMenuSliderTests_PointerEvent(const FVector2D& Position, bool bPressed)
{
	TSet<FKey> PressedButtons;
	if (bPressed)
	{
		PressedButtons.Add(EKeys::LeftMouseButton);
	}
	return FPointerEvent(0, Position, Position, PressedButtons,
		EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailSliderWidePaintSpec,
	"WoT.Menu.Slider.WidePaint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailSliderWidePaintSpec::RunTest(const FString&)
{
	FWoTMenuSliderTests_ScopedArt Art;
	if (!TestTrue(TEXT("slider paint fixture has transient art"), Art.IsValid()))
	{
		return false;
	}
	const FWoTMenuStyle& Style = FWoTMenuStyle::Get();
	const FVector2D LeftSize = Style.SliderLeftCap()->GetImageSize();
	const FVector2D KnobSize = Style.SliderKnob()->GetImageSize();
	const FVector2D RightSize = Style.SliderRightCap()->GetImageSize();
	const FVector2D TrackSize = Style.SliderEmptyCell()->GetImageSize();
	if (!TestTrue(TEXT("slider art has nonzero dimensions"),
		LeftSize.X > 0.0f && LeftSize.Y > 0.0f
		&& KnobSize.X > 0.0f && KnobSize.Y > 0.0f
		&& RightSize.X > 0.0f && RightSize.Y > 0.0f
		&& TrackSize.X > 0.0f && TrackSize.Y > 0.0f))
	{
		return false;
	}
	const TSharedRef<SWoTRetailSlider> Slider =
		SNew(SWoTRetailSlider).Value(0.4f);
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(480.0f, 32.0f))
		.CreateTitleBar(false)
		[Slider];
	for (const float Width : {320.0f, 480.0f})
	{
		const FGeometry Geometry = FGeometry::MakeRoot(
			FVector2D(Width, 32.0f), FSlateLayoutTransform(1.0f));
		FArrangedChildren Cells(EVisibility::Visible);
		if (!TestTrue(TEXT("slider arranges two caps and eight cells"),
			WoTMenuSliderTests_ArrangeCells(Slider, Geometry, Cells)))
		{
			return false;
		}
		const FSlateRect RightCap = Cells[9].Geometry.GetLayoutBoundingRect();
		TestTrue(*FString::Printf(TEXT("%g-wide track reaches its right cap"), Width),
			FMath::IsNearlyEqual(RightCap.Right, Width, 1.0f));
		for (int32 Index = 1; Index <= 8; ++Index)
		{
			const float CellWidth = Cells[Index].Geometry.GetLocalSize().X;
			TestTrue(*FString::Printf(TEXT("%g-wide cell %d receives track space"), Width, Index),
				CellWidth > KnobSize.X);
		}
		FHittestGrid Grid;
		FSlateWindowElementList Elements(Window);
		const FPaintArgs Args(&Window.Get(), Grid,
			FVector2f::ZeroVector, FApp::GetCurrentTime(), 0.0f);
		Slider->Paint(Args, Geometry, Geometry.GetLayoutBoundingRect(),
			Elements, 0, FWidgetStyle(), true);
		for (const int32 CapIndex : {0, 9})
		{
			TArray<TSharedRef<SImage>> CapImages;
			WoTMenuSliderTests_CollectImages(Cells[CapIndex].Widget, CapImages);
			const FVector2D AuthoredSize = CapIndex == 0 ? LeftSize : RightSize;
			bool bCapAspectPreserved = false;
			for (const TSharedRef<SImage>& Image : CapImages)
			{
				const FVector2D PaintedSize = Image->GetCachedGeometry().GetLocalSize();
				bCapAspectPreserved |= PaintedSize.X > 0.0f
					&& FMath::IsNearlyEqual(PaintedSize.Y / PaintedSize.X,
						AuthoredSize.Y / AuthoredSize.X, 0.02f)
					&& FMath::IsNearlyEqual(PaintedSize.Y, 24.0f, 0.01f);
			}
			TestTrue(*FString::Printf(TEXT("%g-wide cap %d paints at native aspect ratio"),
				Width, CapIndex), bCapAspectPreserved);
		}
		for (const int32 CellIndex : {1, 4, 8})
		{
			TArray<TSharedRef<SImage>> CellImages;
			WoTMenuSliderTests_CollectImages(Cells[CellIndex].Widget, CellImages);
			bool bTrackKeepsAuthoredHeight = false;
			for (const TSharedRef<SImage>& Image : CellImages)
			{
				const FVector2D PaintedSize = Image->GetCachedGeometry().GetLocalSize();
				bTrackKeepsAuthoredHeight |= FMath::IsNearlyEqual(
					PaintedSize.X, Cells[CellIndex].Geometry.GetLocalSize().X, 1.0f)
					&& FMath::IsNearlyEqual(PaintedSize.Y, 24.0f, 0.01f);
			}
			TestTrue(*FString::Printf(TEXT("%g-wide track cell %d has readable uniform 3x art height"),
				Width, CellIndex), bTrackKeepsAuthoredHeight);
		}
		// The knob is painted by the slider itself, at the native 3x size.
		bool bNativeKnobPainted = false;
		for (const FSlateBoxElement& Element : Elements.GetUncachedDrawElements()
			.Get<(uint8)EElementType::ET_Box>())
		{
			const FVector2D PaintedSize(Element.GetLocalSize());
			bNativeKnobPainted |= FMath::IsNearlyEqual(PaintedSize.X, 24.0f, 0.01f)
				&& FMath::IsNearlyEqual(PaintedSize.Y / PaintedSize.X,
					KnobSize.Y / KnobSize.X, 0.02f);
		}
		TestTrue(*FString::Printf(TEXT("%g-wide knob paints at native aspect ratio"), Width),
			bNativeKnobPainted);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailSliderInputSpec,
	"WoT.Menu.Slider.WideInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailSliderInputSpec::RunTest(const FString&)
{
	FWoTMenuSliderTests_ScopedArt Art;
	if (!TestTrue(TEXT("slider input fixture has transient art"), Art.IsValid()))
	{
		return false;
	}
	float Value = 0.0f;
	// Authored 8px caps are displayed uniformly at 3x (24 logical pixels).
	const float LeftCapWidth = 24.0f;
	const float RightCapWidth = 24.0f;
	const TSharedRef<SWoTRetailSlider> Slider = SNew(SWoTRetailSlider)
		.Value_Lambda([&Value]() { return Value; })
		.OnValueChanged_Lambda([&Value](float NewValue) { Value = NewValue; });
	for (const float Width : {320.0f, 480.0f})
	{
		const FGeometry Geometry = FGeometry::MakeRoot(
			FVector2D(Width, 32.0f), FSlateLayoutTransform(1.0f));
		FArrangedChildren Cells(EVisibility::Visible);
		if (!TestTrue(TEXT("slider input fixture arranges eight cells"),
			WoTMenuSliderTests_ArrangeCells(Slider, Geometry, Cells)))
		{
			return false;
		}
		for (const int32 Index : {1, 4, 8})
		{
			// Expected hit regions come from the allocated track, independent of
			// the widget's current child geometry and native cell width.
			const float TrackWidth = Width - LeftCapWidth - RightCapWidth;
			const FVector2D Position = Geometry.LocalToAbsolute(FVector2D(
				LeftCapWidth + TrackWidth * (Index - 0.5f) / 8.0f, 16.0f));
			TestTrue(TEXT("left press is handled"),
				Slider->OnMouseButtonDown(Geometry,
					WoTMenuSliderTests_PointerEvent(Position, true)).IsEventHandled());
			TestTrue(TEXT("left release is handled"),
				Slider->OnMouseButtonUp(Geometry,
					WoTMenuSliderTests_PointerEvent(Position, false)).IsEventHandled());
			const float Expected = static_cast<float>(Index - 1) / 7.0f;
			TestTrue(*FString::Printf(TEXT("%g-wide visual cell %d selects its value"), Width, Index),
				FMath::IsNearlyEqual(Value, Expected, KINDA_SMALL_NUMBER));
		}
		// A press picks the stop whose knob centre is nearest: just past the
		// enlarged left cap is still the first stop.
		const FVector2D PastLeftCap = Geometry.LocalToAbsolute(FVector2D(
			LeftCapWidth + 4.0f, 16.0f));
		Slider->OnMouseButtonDown(Geometry,
			WoTMenuSliderTests_PointerEvent(PastLeftCap, true));
		Slider->OnMouseButtonUp(Geometry,
			WoTMenuSliderTests_PointerEvent(PastLeftCap, false));
		TestTrue(TEXT("enlarged left cap is accounted for at the first stop"),
			FMath::IsNearlyZero(Value));
	}
	const FGeometry Geometry = FGeometry::MakeRoot(
		FVector2D(480.0f, 32.0f), FSlateLayoutTransform(1.0f));
	auto PressKey = [&](FKey Key)
	{
		const FKeyEvent Event(Key, FModifierKeysState(), 0, false, 0, 0);
		return Slider->OnKeyDown(Geometry, Event).IsEventHandled();
	};
	TestTrue(TEXT("Home key is handled"), PressKey(EKeys::Home));
	TestTrue(TEXT("Home selects the first position"),
		FMath::IsNearlyZero(Value));
	TestTrue(TEXT("Right key is handled"), PressKey(EKeys::Right));
	TestTrue(TEXT("Right advances one of eight positions"),
		FMath::IsNearlyEqual(Value, 1.0f / 7.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("End key is handled"), PressKey(EKeys::End));
	TestTrue(TEXT("End selects the eighth position"),
		FMath::IsNearlyEqual(Value, 1.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("Left key is handled"), PressKey(EKeys::Left));
	TestTrue(TEXT("Left retreats one of eight positions"),
		FMath::IsNearlyEqual(Value, 6.0f / 7.0f, KINDA_SMALL_NUMBER));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailSliderFlushStopsSpec,
	"WoT.Menu.Slider.FlushStops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailSliderFlushStopsSpec::RunTest(const FString&)
{
	FWoTMenuSliderTests_ScopedArt Art;
	if (!TestTrue(TEXT("slider stop fixture has transient art"), Art.IsValid()))
	{
		return false;
	}
	// Caps and knob are authored 8px art shown at 3x (24 logical pixels). Each
	// cap's last 3 source columns (9 logical) are track interior, so the
	// visible track ends 15 logical pixels in from either edge.
	const float TrackEndInset = 15.0f;
	const float KnobWidth = 24.0f;
	for (const int32 Positions : {8, 15, 61})
	{
		float Value = 0.0f;
		const TSharedRef<SWoTRetailSlider> Slider = SNew(SWoTRetailSlider)
			.Positions(Positions)
			.Value_Lambda([&Value]() { return Value; })
			.OnValueChanged_Lambda([&Value](float NewValue) { Value = NewValue; });
		for (const float Width : {320.0f, 480.0f})
		{
			Value = 0.0f;
			TestTrue(*FString::Printf(
				TEXT("%d stops, %g wide: the minimum knob meets the left cap's end border"),
				Positions, Width),
				FMath::IsNearlyEqual(Slider->KnobLeft(Width), TrackEndInset, 0.01f));
			Value = 1.0f;
			TestTrue(*FString::Printf(
				TEXT("%d stops, %g wide: the maximum knob meets the right cap's end border"),
				Positions, Width),
				FMath::IsNearlyEqual(Slider->KnobLeft(Width) + KnobWidth,
					Width - TrackEndInset, 0.01f));
		}
		const FGeometry Geometry = FGeometry::MakeRoot(
			FVector2D(480.0f, 32.0f), FSlateLayoutTransform(1.0f));
		auto Press = [&](float LocalX)
		{
			const FVector2D Position = Geometry.LocalToAbsolute(FVector2D(LocalX, 16.0f));
			Slider->OnMouseButtonDown(Geometry,
				WoTMenuSliderTests_PointerEvent(Position, true));
			Slider->OnMouseButtonUp(Geometry,
				WoTMenuSliderTests_PointerEvent(Position, false));
		};
		Press(1.0f);
		TestTrue(*FString::Printf(TEXT("%d stops: a press on the left cap selects the minimum"),
			Positions), FMath::IsNearlyZero(Value));
		Press(479.0f);
		TestTrue(*FString::Printf(TEXT("%d stops: a press on the right cap selects the maximum"),
			Positions), FMath::IsNearlyEqual(Value, 1.0f, KINDA_SMALL_NUMBER));
		const FKeyEvent Home(EKeys::Home, FModifierKeysState(), 0, false, 0, 0);
		const FKeyEvent Right(EKeys::Right, FModifierKeysState(), 0, false, 0, 0);
		Slider->OnKeyDown(Geometry, Home);
		Slider->OnKeyDown(Geometry, Right);
		TestTrue(*FString::Printf(TEXT("%d stops: Right advances exactly one stop"), Positions),
			FMath::IsNearlyEqual(Value, 1.0f / static_cast<float>(Positions - 1),
				KINDA_SMALL_NUMBER));
	}

	// The host time limit: 61 stops reach every whole minute 0..60.
	float Minutes = 0.0f;
	const TSharedRef<SWoTRetailSlider> TimeLimit = SNew(SWoTRetailSlider)
		.Positions(61)
		.Value_Lambda([&Minutes]() { return Minutes / 60.0f; })
		.OnValueChanged_Lambda([&Minutes](float NewValue)
		{
			Minutes = static_cast<float>(FMath::RoundToInt(NewValue * 60.0f));
		});
	const FGeometry Geometry = FGeometry::MakeRoot(
		FVector2D(480.0f, 32.0f), FSlateLayoutTransform(1.0f));
	const FKeyEvent Right(EKeys::Right, FModifierKeysState(), 0, false, 0, 0);
	bool bEveryMinute = true;
	for (int32 Minute = 1; Minute <= 60; ++Minute)
	{
		TimeLimit->OnKeyDown(Geometry, Right);
		bEveryMinute &= FMath::IsNearlyEqual(Minutes, static_cast<float>(Minute));
	}
	TestTrue(TEXT("time limit steps through every minute 1..60"), bEveryMinute);
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTSliderValueLabelSpec,
	"WoT.Menu.Slider.ValueLabels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTSliderValueLabelSpec::RunTest(const FString&)
{
	// Sliders used to change width while dragging because
	// each value label sized to its text. Every menu slider's label now shares
	// one width: the widest value any of them can show.
	const TArray<FString>& Labels = SWoTSliderValueLabel::AllValueLabels();
	for (int32 Value = 0; Value <= 100; ++Value)
	{
		TestTrue(*FString::Printf(TEXT("whole number %d is a known label"), Value),
			Labels.Contains(FString::FromInt(Value)));
		TestTrue(*FString::Printf(TEXT("volume %d%% is a known label"), Value),
			Labels.Contains(FString::Printf(TEXT("%d%%"), Value)));
	}
	for (const float Step : SWoTSettingsScreen::SensitivityStepsForTesting())
	{
		TestTrue(*FString::Printf(TEXT("sensitivity %gx is a known label"), Step),
			Labels.Contains(FString::Printf(TEXT("%gx"), Step)));
	}

	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 20);
	const float Width = SWoTSliderValueLabel::SharedWidth(Font);
	if (!TestTrue(TEXT("the shared label width is measurable"), Width > 0.0f))
	{
		return false;
	}
	bool bAllFit = true;
	for (const FString& Label : Labels)
	{
		bAllFit &= FWoTMenuLayout::MeasureText(FText::FromString(Label), Font).X
			<= Width + KINDA_SMALL_NUMBER;
	}
	TestTrue(TEXT("every slider value fits the shared width"), bAllFit);

	auto DesiredWidth = [&Font](const TCHAR* Text)
	{
		const TSharedRef<SWoTSliderValueLabel> Label = SNew(SWoTSliderValueLabel)
			.Text(FText::FromString(Text))
			.Font(Font);
		Label->SlatePrepass(1.0f);
		return static_cast<float>(Label->GetDesiredSize().X);
	};
	TestTrue(TEXT("a one-digit label takes the shared width"),
		FMath::IsNearlyEqual(DesiredWidth(TEXT("0")), Width, 0.01f));
	TestTrue(TEXT("a three-digit label takes the same width"),
		FMath::IsNearlyEqual(DesiredWidth(TEXT("100")), Width, 0.01f));
	return !HasAnyErrors();
}

#endif
