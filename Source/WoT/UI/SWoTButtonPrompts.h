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
	/** The in-game menu's Taunts page: A Taunt, B Back. */
	static TArray<FWoTButtonPrompt> Taunts();
	/** A live key capture on the Controls page: B Cancel. */
	static TArray<FWoTButtonPrompt> Capture();
	/** The on-screen keyboard panel (spec r3 section 3, as the owner's Steam
	 * Deck session ruled, 2026-11-09): A Type, B Cancel, X Backspace, Y Space,
	 * Menu Done. View does nothing there, and Shift is only the on-screen key. */
	static TArray<FWoTButtonPrompt> KeyboardPanel();
	/** Typing with Steam's keyboard: Y Keyboard panel, B Cancel. */
	static TArray<FWoTButtonPrompt> SteamKeyboard();
	/** The settings screens' "Save changes?" prompt: A Select, B Keep Editing. */
	static TArray<FWoTButtonPrompt> SaveChanges();
};

/**
 * The button prompt strip (spec r3 section 5): Slate-drawn controller glyphs,
 * each followed by its label. It shows only while the last input came from a
 * controller and is collapsed otherwise, so keyboard and mouse players see
 * the screen exactly as before. The host binds Prompts to its current window
 * (spec r3 section 1a). Labels draw in the bound face, raised where needed to
 * MinimumLabelPixels at the strip's own layout scale, so every host's strip
 * is legible on a Steam Deck. Exported so plugin windows can use it.
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
	/** The labels' floor: at least this many physical pixels tall at the
	 * strip's live layout scale (the owner's Steam Deck session, 2026-11-09:
	 * the chat line's prompts were far too small at 1280x800). */
	static constexpr float MinimumLabelPixels = 16.0f;
	/** A glyph's side per point of its label's size: about 1.7 times the
	 * label's cap height. */
	static constexpr float GlyphSizePerPoint = 1.6f;

	/** InFont raised to the smallest whole point size whose text is at least
	 * MinimumLabelPixels tall under LayoutScale (Size x 96/72 x LayoutScale,
	 * as Slate draws it); InFont itself where it already is. An invalid face
	 * becomes the engine's default face. */
	static FSlateFontInfo LegibleLabelFont(const FSlateFontInfo& InFont, float LayoutScale);
	/** A glyph's side, in layout units, beside a label in InLabelFont: 1.6
	 * per point of the label, and never too small for its letter's floor
	 * (LetterFontFor) under LayoutScale. */
	static float GlyphSizeFor(const FSlateFontInfo& InLabelFont, float LayoutScale = 1.0f);
	/** The letter on a face-button glyph Side units across: half the side,
	 * and never under FWoTMenuStyle::FloorLegiblePixels (12) physical pixels
	 * under LayoutScale. Labels at their 16-pixel floor already give a 13-pixel
	 * letter, so the floor only guards smaller hosts. */
	static FSlateFontInfo LetterFontFor(float Side, float LayoutScale);
	/** The whole point size a glyph letter needs to reach the floor under
	 * LayoutScale. */
	static int32 LetterFloorPoints(float LayoutScale);
	/** The strip's height, in layout units, for labels bound to InFont under
	 * LayoutScale: the taller of a glyph and a label line. */
	static float BarHeightFor(const FSlateFontInfo& InFont, float LayoutScale);

#if WITH_AUTOMATION_TESTS
	/** One glyph widget as the strip draws it, for paint specs. */
	static TSharedRef<SWidget> MakeGlyphForTesting(EWoTPromptButton Button, float Size);
	/** The label face the strip draws now, the floor applied. */
	FSlateFontInfo LabelFontForTesting() const { return LabelFont(); }
	/** The face the host binds, before the floor. */
	FSlateFontInfo BoundFontForTesting() const { return Font.Get(FSlateFontInfo()); }
#endif

private:
	TArray<FWoTButtonPrompt> CurrentPrompts() const;
	FSlateFontInfo LabelFont() const;
	EVisibility PromptVisibility(int32 Index) const;

	TAttribute<TArray<FWoTButtonPrompt>> Prompts;
	TAttribute<FSlateFontInfo> Font;
};
