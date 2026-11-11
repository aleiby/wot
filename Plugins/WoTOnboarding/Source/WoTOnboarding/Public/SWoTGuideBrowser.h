#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealGuide.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class FWoTOnboardingFaces;
class FWoTRetailArt;
class SBox;
class SScrollBox;
class SVerticalBox;
struct FWoTOnboardingWindowScale;

// The angreal field guide (plan Tasks 5 and 8): a window in the original
// item-info window's look. An icon grid of the guide rows in rarity groups
// fills the left; the right shows the selection as the item-info window
// shows an item (title, bevelled icon, rarity and charges, gem bar,
// description, divider, quote), then the rows that counter it, its hint line
// and the rows it counters, each other row as an icon that opens it. Title,
// Description and Quote are the purchaser's retail text, resolved at runtime
// through the onboarding retail-text seam and never stored or invented; a row
// whose text does not resolve shows its facts and omits the prose. The
// widget reads only the guide asset and the seams, so it works over the front
// end and over a match alike; the module hosts it on the game viewport the way
// the retail item-info window is hosted.
class WOTONBOARDING_API SWoTGuideBrowser : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTGuideBrowser)
		: _Guide(nullptr)
		, _InitialPhysicalSize(FVector2D::ZeroVector)
		, _InitialDpiScale(1.0f)
	{}
		SLATE_ARGUMENT(UWoTAngrealGuide*, Guide)
		/** The row the guide opens at; OpeningRow() picks it. None or a row
		    the guide does not carry opens at the first entry of the first
		    group. */
		SLATE_ARGUMENT(FName, InitialRow)
		/** Pressing any of these closes the browser (the live ShowGuide
		    binding, resolved by the module at open time; the retail help
		    window takes its close keys the same way). */
		SLATE_ARGUMENT(TArray<FKey>, CloseKeys)
		/** Display text by (section, property); defaults to the onboarding
		    retail-text seam. Specs inject fixture resolvers here. */
		SLATE_ARGUMENT(TFunction<FString(const FString& Section,
			const FString& Property)>, TextResolver)
		/** The viewport's size in pixels and its UI DPI scale. Slate lays a
		    widget out before its first tick, so without these the first
		    frame would draw at 1x. */
		SLATE_ARGUMENT(FVector2D, InitialPhysicalSize)
		SLATE_ARGUMENT(float, InitialDpiScale)
		SLATE_EVENT(FSimpleDelegate, OnClose)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The row to open at: CardRow while a card is up and the guide carries
	    it, else HandItem (the selected hand's item) when the guide carries
	    it, else the first entry of the first group. None for an empty
	    guide. */
	static FName OpeningRow(const UWoTAngrealGuide& InGuide, FName CardRow,
		FName HandItem);

	/** Shows ClassName's row in the detail pane (the deep link the counter
	    icons use). Unknown names leave the selection unchanged. */
	void SelectRow(FName ClassName);
	FName SelectedRowForTesting() const { return SelectedClassName; }
	int32 BaseStepForTesting() const;
	int32 ReadingStepForTesting() const;
	float WindowScaleForTesting() const;
	TSharedRef<FWoTOnboardingFaces> FacesForTesting() const { return Faces.ToSharedRef(); }
	FVector2D MeasureDetailForTesting(FName ClassName);
	static float DetailPaneHeightForTesting();
	int32 FitChecksForTesting() const { return FitChecks; }

	/** The grid's entries in navigation order: the groups in rarity order,
	    traps last, each in guide order. */
	TArray<FName> GridOrder() const;

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry,
		const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;

private:
	/** One rarity group of the grid, laid out in lines of eight. */
	struct FGridGroup
	{
		const TCHAR* Label;
		TArray<FName> Rows;
	};

	FString ResolveRowText(const FWoTAngrealGuideRow& Row,
		const TCHAR* Property) const;
	FString ResolveWindowText(const TCHAR* Key, const FString& Fallback) const;
	/** The row's resolved retail title, or its spaced class name while the
	    retail text is unavailable. */
	FString DisplayTitle(const FWoTAngrealGuideRow& Row) const;
	TSharedRef<SWidget> BuildGrid();
	TSharedRef<SWidget> BuildGridCell(FName ClassName);
	TSharedRef<SWidget> BuildDetail(const FWoTAngrealGuideRow& Row,
		TSharedPtr<SWidget>* OutFactsLeft = nullptr, TSharedPtr<SWidget>* OutFactsRight = nullptr);
	void FitReadingStep();
	TSharedRef<SWidget> BuildGemBar(const FWoTAngrealGuideRow& Row);
	/** A labelled run of icons that each open their row; nothing for no
	    targets. TagPrefix names the icons' buttons. Returns whether it added
	    the run. */
	bool AddIconRow(SVerticalBox& Into, float Top, const TCHAR* Label,
		const TCHAR* TagPrefix, const TArray<const FWoTAngrealGuideRow*>& Targets);
	void RebuildDetail();
	/** Moves the selection a step through the grid: left and right walk the
	    entries in order, up and down keep the column across lines. */
	void MoveSelection(int32 Columns, int32 Lines);
	void ScrollDetail(float Units);
	FReply Close();
	bool IsCloseKey(const FKey& Key) const;

	TStrongObjectPtr<UWoTAngrealGuide> Guide;
	TFunction<FString(const FString&, const FString&)> TextResolver;
	FSimpleDelegate OnClose;
	TArray<FKey> CloseKeys;
	TSharedPtr<FWoTOnboardingFaces> Faces;
	TSharedPtr<FWoTRetailArt> Art;
	TSharedPtr<FWoTOnboardingWindowScale> Scale;
	TArray<FGridGroup> Groups;
	/** The grid as laid out: each line a run of up to eight entries of one
	    group. */
	TArray<TArray<FName>> GridLines;
	TMap<FName, TSharedPtr<SWidget>> GridCells;
	TSharedPtr<SScrollBox> GridScroll;
	TSharedPtr<SScrollBox> DetailScroll;
	TSharedPtr<SBox> WindowBox;
	TSharedPtr<SBox> DetailHost;
	FName SelectedClassName;
	double LastScaleChangeTime = 0.0;
	bool bReadingFitPending = false;
	int32 FitChecks = 0;
};
