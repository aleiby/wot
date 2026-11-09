#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingSeams.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

class SBox;
class SVerticalBox;

// The angreal field-guide browser (plan Task 5): a list pane of guide rows
// grouped by rarity and a detail pane whose Title, Description and Quote are
// the purchaser's retail text, resolved at runtime through the onboarding
// retail-text seam and never stored or invented. A row whose text does not
// resolve shows its facts and omits the prose panes. The widget reads only
// the guide asset and the seams, so it works over the front end and over a
// match alike; the module hosts it on the game viewport the way the retail
// item-info window is hosted.
class WOTONBOARDING_API SWoTGuideBrowser : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTGuideBrowser)
		: _Guide(nullptr)
	{}
		SLATE_ARGUMENT(UWoTAngrealGuide*, Guide)
		/** Pressing any of these closes the browser (the live ShowGuide
		    binding, resolved by the module at open time; the retail help
		    window takes its close keys the same way). */
		SLATE_ARGUMENT(TArray<FKey>, CloseKeys)
		/** Display text by (section, property); defaults to the onboarding
		    retail-text seam. Specs inject fixture resolvers here. */
		SLATE_ARGUMENT(TFunction<FString(const FString& Section,
			const FString& Property)>, TextResolver)
		SLATE_EVENT(FSimpleDelegate, OnClose)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows ClassName's row in the detail pane (the deep link the counter
	    rows use). Unknown names leave the selection unchanged. */
	void SelectRow(FName ClassName);
	FName SelectedRowForTesting() const { return SelectedClassName; }

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;

private:
	FString ResolveRowText(const FWoTAngrealGuideRow& Row,
		const TCHAR* Property) const;
	/** The row's resolved retail title, or its spaced class name while the
	    retail text is unavailable, so list rows and deep links stay
	    navigable either way. */
	FString DisplayTitle(const FWoTAngrealGuideRow& Row) const;
	TSharedRef<SWidget> BuildListPane();
	TSharedRef<SWidget> BuildDetailPane(const FWoTAngrealGuideRow& Row);
	void AddLinkRows(SVerticalBox& Into, const TCHAR* Heading,
		const TArray<const FWoTAngrealGuideRow*>& Targets);
	void RebuildDetail();
	FReply Close();
	bool IsCloseKey(const FKey& Key) const;

	TStrongObjectPtr<UWoTAngrealGuide> Guide;
	TFunction<FString(const FString&, const FString&)> TextResolver;
	FSimpleDelegate OnClose;
	TArray<FKey> CloseKeys;
	FWoTOnboardingMenuChrome Chrome;
	TSharedPtr<SBox> DetailHost;
	FName SelectedClassName;
};
