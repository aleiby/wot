// LAN installer sharing: the sharer's "Sending..." notice (spec B1, B4).
//
// The notice is the consent mitigation for sharing that is on by default, so
// the sharer's player sees it on every screen: one widget drawn as game
// viewport content above everything else (front end, Settings and matches
// alike). It never takes focus or input.
#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class UGameInstance;

/** Where the notice line is drawn. Specs supply a recording one. */
class IWoTSharingNoticePresenter
{
public:
	virtual ~IWoTSharingNoticePresenter() = default;
	/** Shows Text, replacing whatever line is already shown. */
	virtual void Show(const FText& Text) = 0;
	virtual void Hide() = 0;
};

/**
 * Which line shows and for how long. Every transfer start and end shows its
 * line, so with two transfers the newest line wins. The notice stays while any
 * transfer is still sending and hides HideDelaySeconds after the last one
 * ends ("... sent" or "... stopped").
 */
class FWoTSharingNoticeLine final
{
public:
	static constexpr double HideDelaySeconds = 6.0;

	/** NowSeconds drives the hide timer; FPlatformTime::Seconds when unset. */
	FWoTSharingNoticeLine(TUniquePtr<IWoTSharingNoticePresenter> InPresenter,
		TFunction<double()> InNowSeconds = {});
	~FWoTSharingNoticeLine();
	FWoTSharingNoticeLine(const FWoTSharingNoticeLine&) = delete;
	FWoTSharingNoticeLine& operator=(const FWoTSharingNoticeLine&) = delete;

	/** "Sending the original game installer to <label>..." */
	void TransferStarted(const FString& Text);
	/** "... sent" or "... stopped". */
	void TransferEnded(const FString& Text);
	/** Hides the line once its time is up. */
	void Tick();
	/** Hides the line now and forgets every transfer (sharing stopped). */
	void Reset();

	bool IsShown() const { return bShown; }
	int32 SendingCount() const { return Sending; }

private:
	void Present(const FString& Text);

	TUniquePtr<IWoTSharingNoticePresenter> Presenter;
	TFunction<double()> NowSeconds;
	TOptional<double> HideAtSeconds;
	int32 Sending = 0;
	bool bShown = false;
};

/** The notice itself: ivory text on a translucent dark backing in the
 * bottom-left corner. Hit-test invisible, so it never blocks a click. */
class SWoTSharingNotice final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTSharingNotice) {}
	SLATE_END_ARGS()

	/** FCoreStyle Regular at this size draws about 12 px tall at 1280x800
	 * (the Steam Deck), above the 9 px legibility floor. */
	static constexpr int32 FontSize = 12;
	static constexpr float ScreenMargin = 16.0f;

	void Construct(const FArguments& InArgs);
	void SetText(const FText& Text);
	virtual bool SupportsKeyboardFocus() const override { return false; }

#if WITH_AUTOMATION_TESTS
	FText GetTextForTesting() const;
	TSharedPtr<STextBlock> TextBlockForTesting() const { return TextBlock; }
#endif

private:
	TSharedPtr<STextBlock> TextBlock;
	FSlateBrush BackingBrush;
};

/** The notice's place in the game viewport's overlay: above every menu,
 * overlay and HUD layer the game adds (the highest of those is 2200). */
inline constexpr int32 WoTSharingNoticeZOrder = 10000;

/**
 * The production presenter: SWoTSharingNotice as content of the instance's
 * game viewport, added on the first Show and removed on Hide. Without a game
 * viewport (a dedicated server, early boot, a headless spec) it does nothing.
 */
TUniquePtr<IWoTSharingNoticePresenter> WoTCreateViewportSharingNoticePresenter(
	TWeakObjectPtr<UGameInstance> Instance);
