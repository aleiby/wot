#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "Widgets/SCompoundWidget.h"

/** A controller button a prompt names. Its glyph is drawn by Slate. */
enum class EWoTPromptButton : uint8
{
	A,
	B,
	X,
	Y,
	View,
	Menu,
	/** D-pad left and right (spec r3 section 1a's "◀▶"). */
	DPadLeftRight,
	DPadUp
};

/** One "button + what it does" entry of a prompt strip. */
struct WOT_API FWoTButtonPrompt
{
	EWoTPromptButton Button = EWoTPromptButton::A;
	FText Label;

	FWoTButtonPrompt() = default;
	FWoTButtonPrompt(EWoTPromptButton InButton, const FText& InLabel)
		: Button(InButton), Label(InLabel)
	{
	}

	bool operator==(const FWoTButtonPrompt& Other) const
	{
		return Button == Other.Button && Label.ToString() == Other.Label.ToString();
	}
	bool operator!=(const FWoTButtonPrompt& Other) const { return !(*this == Other); }

	/** The button's short name ("A", "B", "View", "D-pad Left/Right"). */
	static FString ButtonName(EWoTPromptButton Button);
	/** "A Select", for logs and specs. */
	FString ToString() const;
	/** "A Select | B Back". */
	static FString Describe(TConstArrayView<FWoTButtonPrompt> Prompts);
};

/** The prompt sets of spec r3 section 1a, one per window row. */
struct WOT_API FWoTButtonPromptSets
{
	/** Front-end Main, the bootstrap screen: A Select. */
	static TArray<FWoTButtonPrompt> Select();
	/** Release notes and update-failed notices: A OK, B Close. */
	static TArray<FWoTButtonPrompt> Notice();
	/** Play, Player and the in-game menu: A Select, B Back. */
	static TArray<FWoTButtonPrompt> SelectBack();
	/** A Play overlay: A Select, B and the overlay's own dismiss. */
	static TArray<FWoTButtonPrompt> Overlay(const FText& DismissLabel);
	/** The password challenge: A Type, B Cancel. */
	static TArray<FWoTButtonPrompt> Password();
	/** Controls and Hardware: A Select, B Back, D-pad Left/Right Adjust. */
	static TArray<FWoTButtonPrompt> Settings();
	/** The video Keep/Revert strip: A Keep, B Revert. */
	static TArray<FWoTButtonPrompt> KeepRevert();
	/** An open dropdown: A Pick, B Close. */
	static TArray<FWoTButtonPrompt> Dropdown();
	/** Help: B Close. */
	static TArray<FWoTButtonPrompt> Close();
	/** A live key capture on the Controls page: B Cancel. */
	static TArray<FWoTButtonPrompt> Capture();
};

/**
 * The button prompt strip (spec r3 section 5): Slate-drawn controller glyphs,
 * each followed by its label. It shows only while the last input came from a
 * controller and is collapsed otherwise, so keyboard and mouse players see
 * the screen exactly as before. The host binds Prompts to its current window
 * (spec r3 section 1a). Exported so plugin windows can use it.
 */
class WOT_API SWoTButtonPrompts : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTButtonPrompts)
		: _LabelColor(FSlateColor(FLinearColor(0.88f, 0.84f, 0.73f)))
	{}
		SLATE_ATTRIBUTE(TArray<FWoTButtonPrompt>, Prompts)
		/** The label face; the engine's default face when unbound. */
		SLATE_ATTRIBUTE(FSlateFontInfo, Font)
		SLATE_ATTRIBUTE(FSlateColor, LabelColor)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The prompts on screen now: the bound set in Controller mode, else none. */
	TArray<FWoTButtonPrompt> GetShownPrompts() const;
	/** The strip shows only while the last input is a controller. */
	static bool IsShownForCurrentInput();

	/** Prompts past this many are not drawn. */
	static constexpr int32 MaxPrompts = 6;

private:
	TArray<FWoTButtonPrompt> CurrentPrompts() const;
	FSlateFontInfo LabelFont() const;
	EVisibility PromptVisibility(int32 Index) const;

	TAttribute<TArray<FWoTButtonPrompt>> Prompts;
	TAttribute<FSlateFontInfo> Font;
};
