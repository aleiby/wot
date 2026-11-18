#include "SWoTSharingNotice.h"

#include "Brushes/SlateColorBrush.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreMisc.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "../WoTMenuStyle.h"

namespace
{
/** The multiplayer skin's dark panel, translucent so the screen shows through. */
const FLinearColor WoTSharingNoticeBacking(0.018f, 0.022f, 0.025f, 0.82f);
const FMargin WoTSharingNoticePadding(10.0f, 6.0f);

class FWoTViewportSharingNoticePresenter final : public IWoTSharingNoticePresenter
{
public:
	explicit FWoTViewportSharingNoticePresenter(TWeakObjectPtr<UGameInstance> InInstance)
		: Instance(MoveTemp(InInstance))
	{
	}

	virtual ~FWoTViewportSharingNoticePresenter() override
	{
		Hide();
	}

	virtual void Show(const FText& Text) override
	{
		UGameViewportClient* Viewport = ResolveViewport();
		if (Viewport == nullptr)
		{
			// No viewport (any more): nothing to draw on.
			Hide();
			return;
		}
		if (Widget.IsValid() && ShownOn.Get() != Viewport)
		{
			Hide();
		}
		if (!Widget.IsValid())
		{
			Widget = SNew(SWoTSharingNotice);
			Viewport->AddViewportWidgetContent(Widget.ToSharedRef(),
				WoTSharingNoticeZOrder);
			ShownOn = Viewport;
		}
		Widget->SetText(Text);
	}

	virtual void Hide() override
	{
		if (Widget.IsValid())
		{
			if (UGameViewportClient* Viewport = ShownOn.Get())
			{
				Viewport->RemoveViewportWidgetContent(Widget.ToSharedRef());
			}
			Widget.Reset();
		}
		ShownOn.Reset();
	}

private:
	UGameViewportClient* ResolveViewport() const
	{
		if (IsRunningDedicatedServer() || !FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		const UGameInstance* Owner = Instance.Get();
		UGameViewportClient* Viewport = Owner != nullptr
			? Owner->GetGameViewportClient() : nullptr;
		// The overlay that holds viewport content is set together with the
		// window; before that (early boot) adding content would assert.
		return Viewport != nullptr && Viewport->GetWindow().IsValid()
			? Viewport : nullptr;
	}

	TWeakObjectPtr<UGameInstance> Instance;
	TWeakObjectPtr<UGameViewportClient> ShownOn;
	TSharedPtr<SWoTSharingNotice> Widget;
};
} // namespace

FWoTSharingNoticeLine::FWoTSharingNoticeLine(
	TUniquePtr<IWoTSharingNoticePresenter> InPresenter,
	TFunction<double()> InNowSeconds)
	: Presenter(MoveTemp(InPresenter))
	, NowSeconds(MoveTemp(InNowSeconds))
{
	if (!NowSeconds)
	{
		NowSeconds = []()
		{
			return FPlatformTime::Seconds();
		};
	}
}

FWoTSharingNoticeLine::~FWoTSharingNoticeLine()
{
	Reset();
}

void FWoTSharingNoticeLine::TransferStarted(const FString& Text)
{
	++Sending;
	HideAtSeconds.Reset();
	Present(Text);
}

void FWoTSharingNoticeLine::TransferEnded(const FString& Text)
{
	Sending = FMath::Max(0, Sending - 1);
	Present(Text);
	if (Sending == 0)
	{
		HideAtSeconds = NowSeconds() + HideDelaySeconds;
	}
}

void FWoTSharingNoticeLine::Tick()
{
	if (HideAtSeconds.IsSet() && NowSeconds() >= HideAtSeconds.GetValue())
	{
		HideAtSeconds.Reset();
		if (bShown)
		{
			bShown = false;
			if (Presenter.IsValid())
			{
				Presenter->Hide();
			}
		}
	}
}

void FWoTSharingNoticeLine::Reset()
{
	Sending = 0;
	HideAtSeconds.Reset();
	if (bShown)
	{
		bShown = false;
		if (Presenter.IsValid())
		{
			Presenter->Hide();
		}
	}
}

void FWoTSharingNoticeLine::Present(const FString& Text)
{
	bShown = true;
	if (Presenter.IsValid())
	{
		Presenter->Show(FText::AsCultureInvariant(Text));
	}
}

void SWoTSharingNotice::Construct(const FArguments& InArgs)
{
	(void)InArgs;
	BackingBrush = FSlateColorBrush(WoTSharingNoticeBacking);
	SetVisibility(EVisibility::HitTestInvisible);
	SetCanTick(false);
	ChildSlot
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Bottom)
	.Padding(ScreenMargin)
	[
		SNew(SBorder)
		.BorderImage(&BackingBrush)
		.Padding(WoTSharingNoticePadding)
		[
			SAssignNew(TextBlock, STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), FontSize))
			.ColorAndOpacity(FWoTRetailBrowserStyle::GameTextColor())
		]
	];
}

void SWoTSharingNotice::SetText(const FText& Text)
{
	if (TextBlock.IsValid())
	{
		TextBlock->SetText(Text);
	}
}

#if WITH_AUTOMATION_TESTS
FText SWoTSharingNotice::GetTextForTesting() const
{
	return TextBlock.IsValid() ? TextBlock->GetText() : FText::GetEmpty();
}
#endif

TUniquePtr<IWoTSharingNoticePresenter> WoTCreateViewportSharingNoticePresenter(
	TWeakObjectPtr<UGameInstance> Instance)
{
	return MakeUnique<FWoTViewportSharingNoticePresenter>(MoveTemp(Instance));
}
