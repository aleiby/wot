#include "SWoTGuideBrowser.h"

#include "WoTAngrealGuide.h"
#include "WoTOnboardingChrome.h"
#include "WoTOnboardingHints.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
const TCHAR* WoTGuideBrowserRarityKey(const FWoTAngrealGuideRow& Row)
{
	if (Row.Rarity == FName(TEXT("Rare")))
	{
		return TEXT("RarityRareStr");
	}
	if (Row.Rarity == FName(TEXT("Uncommon")))
	{
		return TEXT("RarityUncommonStr");
	}
	return TEXT("RarityCommonStr");
}

FString WoTGuideBrowserRarityFallback(const FWoTAngrealGuideRow& Row)
{
	if (Row.Rarity == FName(TEXT("Rare")))
	{
		return TEXT("Rare artifact");
	}
	if (Row.Rarity == FName(TEXT("Uncommon")))
	{
		return TEXT("Uncommon artifact");
	}
	return TEXT("Common artifact");
}

FString WoTGuideBrowserJoinNames(const TArray<FName>& Names)
{
	FString Joined;
	for (const FName& Name : Names)
	{
		if (!Joined.IsEmpty())
		{
			Joined += TEXT(", ");
		}
		Joined += Name.ToString();
	}
	return Joined;
}
} // namespace

void SWoTGuideBrowser::Construct(const FArguments& InArgs)
{
	Guide = TStrongObjectPtr<UWoTAngrealGuide>(InArgs._Guide);
	TextResolver = InArgs._TextResolver;
	if (!TextResolver)
	{
		TextResolver = [](const FString& Section, const FString& Property)
		{
			// The resolver derives the owning file from the section, so the
			// file argument carries no address here.
			return FWoTOnboardingSeams::Get().ResolveText(
				FString(), Section, Property);
		};
	}
	CloseKeys = InArgs._CloseKeys;
	OnClose = InArgs._OnClose;

	Chrome = WoTOnboardingChrome::Resolve();

	if (!Guide.IsValid() || Guide->Rows.IsEmpty())
	{
		ChildSlot[SNullWidget::NullWidget];
		return;
	}
	SelectedClassName = Guide->Rows[0].ClassName;

	const bool bRetailChrome = WoTOnboardingChrome::IsRetail(Chrome);
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(Chrome.Blackout)
			.ColorAndOpacity(bRetailChrome
				? FLinearColor(1.0f, 1.0f, 1.0f, 0.88f)
				: FLinearColor(0.0f, 0.0f, 0.0f, 0.85f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBorder)
			.BorderImage(Chrome.Background)
			.BorderBackgroundColor(bRetailChrome
				? FLinearColor::White : FLinearColor(0.06f, 0.05f, 0.04f))
			[
				SNew(SBorder)
				.BorderImage(Chrome.Frame)
				.BorderBackgroundColor(bRetailChrome
					? FLinearColor::White : FLinearColor(0.12f, 0.11f, 0.09f))
				.ForegroundColor(Chrome.Foreground)
				.Padding(FMargin(28.0f, 22.0f))
				[
					SNew(SBox)
					.WidthOverride(1000.0f)
					.HeightOverride(620.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 10.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("Field Guide")))
								.Font(Chrome.TitleFont)
								.ColorAndOpacity(Chrome.Foreground)
							]
							+ SHorizontalBox::Slot().AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(SButton)
								.ButtonStyle(&FCoreStyle::Get()
									.GetWidgetStyle<FButtonStyle>(TEXT("NoBorder")))
								.OnClicked(this, &SWoTGuideBrowser::Close)
								[
									SNew(STextBlock)
									.Text(FText::FromString(TEXT("Close")))
									.Font(Chrome.BodyFont)
									.ColorAndOpacity(Chrome.Accent)
								]
							]
						]
						+ SVerticalBox::Slot().AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 12.0f)
						[
							SNew(SSeparator)
							.SeparatorImage(Chrome.Separator)
						]
						+ SVerticalBox::Slot().FillHeight(1.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SNew(SBox)
								.WidthOverride(280.0f)
								[
									BuildListPane()
								]
							]
							+ SHorizontalBox::Slot().AutoWidth()
							.Padding(14.0f, 0.0f)
							[
								SNew(SSeparator)
								.Orientation(Orient_Vertical)
								.SeparatorImage(Chrome.Separator)
							]
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							[
								SAssignNew(DetailHost, SBox)
							]
						]
					]
				]
			]
		]
	];
	RebuildDetail();
	if (Chrome.BindWidget)
	{
		Chrome.BindWidget(AsShared());
	}
}

FString SWoTGuideBrowser::ResolveRowText(const FWoTAngrealGuideRow& Row,
	const TCHAR* Property) const
{
	// The retail localization address: section = the shipped class name, the
	// same key the rows carry; the .int sections hold Title, Description and
	// Quote per class.
	return TextResolver
		? TextResolver(Row.ClassName.ToString(), FString(Property))
		: FString();
}

FString SWoTGuideBrowser::DisplayTitle(const FWoTAngrealGuideRow& Row) const
{
	const FString Title = ResolveRowText(Row, TEXT("Title"));
	return Title.IsEmpty()
		? WoTOnboardingHints::SpacedClassName(Row.ClassName) : Title;
}

TSharedRef<SWidget> SWoTGuideBrowser::BuildListPane()
{
	struct FWoTGuideBrowserGroup
	{
		const TCHAR* Header;
		TArray<const FWoTAngrealGuideRow*> Rows;
	};
	// Rarity groups in rarity order; rows without an angreal rarity are the
	// placeable traps, their own group at the end. Rows keep asset order
	// within a group.
	FWoTGuideBrowserGroup Groups[] = {
		{TEXT("COMMON"), {}}, {TEXT("UNCOMMON"), {}}, {TEXT("RARE"), {}},
		{TEXT("TRAPS"), {}}};
	for (const FWoTAngrealGuideRow& Row : Guide->Rows)
	{
		if (Row.Rarity == FName(TEXT("Common")))
		{
			Groups[0].Rows.Add(&Row);
		}
		else if (Row.Rarity == FName(TEXT("Uncommon")))
		{
			Groups[1].Rows.Add(&Row);
		}
		else if (Row.Rarity == FName(TEXT("Rare")))
		{
			Groups[2].Rows.Add(&Row);
		}
		else
		{
			Groups[3].Rows.Add(&Row);
		}
	}

	TSharedRef<SScrollBox> List = SNew(SScrollBox);
	for (const FWoTGuideBrowserGroup& Group : Groups)
	{
		if (Group.Rows.IsEmpty())
		{
			continue;
		}
		List->AddSlot().Padding(0.0f, 10.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Group.Header))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Accent)
		];
		for (const FWoTAngrealGuideRow* Row : Group.Rows)
		{
			const FName ClassName = Row->ClassName;
			List->AddSlot().Padding(8.0f, 2.0f)
			[
				SNew(SButton)
				.ButtonStyle(&FCoreStyle::Get()
					.GetWidgetStyle<FButtonStyle>(TEXT("NoBorder")))
				.ContentPadding(FMargin(2.0f, 1.0f))
				.OnClicked_Lambda([this, ClassName]()
				{
					SelectRow(ClassName);
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(DisplayTitle(*Row)))
					.Font(Chrome.BodyFont)
					.ColorAndOpacity_Lambda([this, ClassName]()
					{
						return SelectedClassName == ClassName
							? Chrome.Accent : Chrome.Foreground;
					})
				]
			];
		}
	}
	return List;
}

void SWoTGuideBrowser::AddLinkRows(SVerticalBox& Into, const TCHAR* Heading,
	const TArray<const FWoTAngrealGuideRow*>& Targets)
{
	if (Targets.IsEmpty())
	{
		return;
	}
	Into.AddSlot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 2.0f)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Heading))
		.Font(Chrome.BodyFont)
		.ColorAndOpacity(Chrome.Foreground)
	];
	for (const FWoTAngrealGuideRow* Target : Targets)
	{
		const FName ClassName = Target->ClassName;
		Into.AddSlot().AutoHeight().Padding(10.0f, 1.0f)
		.HAlign(HAlign_Left)
		[
			SNew(SButton)
			.ButtonStyle(&FCoreStyle::Get()
				.GetWidgetStyle<FButtonStyle>(TEXT("NoBorder")))
			.ContentPadding(FMargin(2.0f, 1.0f))
			.OnClicked_Lambda([this, ClassName]()
			{
				SelectRow(ClassName);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(DisplayTitle(*Target)))
				.Font(Chrome.BodyFont)
				.ColorAndOpacity(Chrome.Accent)
			]
		];
	}
}

TSharedRef<SWidget> SWoTGuideBrowser::BuildDetailPane(
	const FWoTAngrealGuideRow& Row)
{
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
	Body->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
	[
		SNew(STextBlock)
		.Text(FText::FromString(DisplayTitle(Row)))
		.Font(Chrome.TitleFont)
		.ColorAndOpacity(Chrome.Foreground)
		.Justification(ETextJustify::Center)
	];

	// The facts line: rarity (or the trap group's own tag) and the charge
	// envelope, the same row the retail item-info window leads with.
	FString RarityText;
	if (Row.Rarity.IsNone())
	{
		RarityText = TEXT("Trap");
	}
	else
	{
		RarityText = TextResolver
			? TextResolver(TEXT("InventoryInfoWindow"),
				WoTGuideBrowserRarityKey(Row))
			: FString();
		if (RarityText.IsEmpty())
		{
			RarityText = WoTGuideBrowserRarityFallback(Row);
		}
	}
	const FString ChargesText = Row.MaxCharges > 0
		? FString::Printf(TEXT("Charges:  %d / %d"), Row.MaxInitialCharges,
			Row.MaxCharges)
		: FString();
	Body->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(RarityText))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Foreground)
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right)
		[
			SNew(STextBlock)
			.Text(FText::FromString(ChargesText))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Foreground)
		]
	];

	if (!Row.Elements.IsEmpty())
	{
		// The shipped element readout order is Earth, Air, Fire, Water,
		// Spirit (InventoryInfoWindow.uc:146-150), as the item-info window
		// renders it.
		const TCHAR* ElementNames[] = {TEXT("Earth"), TEXT("Air"),
			TEXT("Fire"), TEXT("Water"), TEXT("Spirit")};
		TSharedRef<SHorizontalBox> Gems = SNew(SHorizontalBox);
		for (const TCHAR* ElementName : ElementNames)
		{
			const bool bActive = Row.Elements.Contains(FName(ElementName));
			Gems->AddSlot().FillWidth(1.0f).HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Font(Chrome.BodyFont)
				.Text(FText::FromString(FString::Printf(TEXT("%s %s"),
					bActive ? TEXT("[ON]") : TEXT("[--]"),
					*FString(ElementName).ToUpper())))
				.ColorAndOpacity(bActive
					? FSlateColor(FLinearColor(0.45f, 0.9f, 1.0f))
					: FSlateColor(FLinearColor(0.45f, 0.45f, 0.45f)))
			];
		}
		Body->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			Gems
		];
	}

	if (!Row.Roles.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(WoTGuideBrowserJoinNames(Row.Roles)))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Foreground)
			.Justification(ETextJustify::Center)
		];
	}

	// Prose panes only where the retail text resolves; a row without it shows
	// its facts and nothing else, never placeholder text.
	const FString Description = ResolveRowText(Row, TEXT("Description"));
	if (!Description.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 14.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Description))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Foreground)
			.AutoWrapText(true)
			.Justification(ETextJustify::Center)
		];
	}
	const FString Quote = ResolveRowText(Row, TEXT("Quote"));
	if (!Quote.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(24.0f, 0.0f, 24.0f, 12.0f)
		[
			SNew(SSeparator)
			.SeparatorImage(Chrome.Separator)
		];
		Body->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Quote))
			.Font(Chrome.ItalicFont)
			.ColorAndOpacity(Chrome.Foreground)
			.AutoWrapText(true)
			.Justification(ETextJustify::Center)
		];
	}

	const FString HintLine = WoTOnboardingHints::ResolveHintLine(*Guide, Row,
		[this](const FWoTAngrealGuideRow& HintRow)
		{
			return ResolveRowText(HintRow, TEXT("Title"));
		});
	if (!HintLine.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 6.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(HintLine))
			.Font(Chrome.BodyFont)
			.ColorAndOpacity(Chrome.Accent)
			.AutoWrapText(true)
			.Justification(ETextJustify::Center)
		];
	}

	TArray<const FWoTAngrealGuideRow*> Counters;
	for (const FName& Countered : Row.Counters)
	{
		if (const FWoTAngrealGuideRow* Target =
			Guide->FindByClassName(Countered))
		{
			Counters.Add(Target);
		}
	}
	AddLinkRows(*Body, TEXT("COUNTERS"), Counters);
	AddLinkRows(*Body, TEXT("COUNTERED BY"),
		Guide->CounteredBy(Row.ClassName));

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(10.0f, 0.0f)
		[
			Body
		];
}

void SWoTGuideBrowser::RebuildDetail()
{
	if (!DetailHost.IsValid())
	{
		return;
	}
	const FWoTAngrealGuideRow* Row = Guide.IsValid()
		? Guide->FindByClassName(SelectedClassName) : nullptr;
	DetailHost->SetContent(
		Row ? BuildDetailPane(*Row) : SNullWidget::NullWidget);
}

void SWoTGuideBrowser::SelectRow(FName ClassName)
{
	if (!Guide.IsValid() || !Guide->FindByClassName(ClassName))
	{
		return;
	}
	SelectedClassName = ClassName;
	RebuildDetail();
}

FReply SWoTGuideBrowser::Close()
{
	OnClose.ExecuteIfBound();
	return FReply::Handled();
}

bool SWoTGuideBrowser::IsCloseKey(const FKey& Key) const
{
	// Escape and the controller back keys mirror the retail windows' back
	// handling; the ShowGuide binding's own keys arrive through CloseKeys so
	// the opening key is also the closing key, whatever it is bound to.
	if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right
		|| Key == EKeys::Gamepad_Special_Left)
	{
		return true;
	}
	return CloseKeys.Contains(Key);
}

FReply SWoTGuideBrowser::OnKeyDown(const FGeometry& MyGeometry,
	const FKeyEvent& InKeyEvent)
{
	if (IsCloseKey(InKeyEvent.GetKey()))
	{
		return Close();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoTGuideBrowser::OnPreviewKeyDown(const FGeometry& MyGeometry,
	const FKeyEvent& InKeyEvent)
{
	if (IsCloseKey(InKeyEvent.GetKey()))
	{
		return Close();
	}
	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}
