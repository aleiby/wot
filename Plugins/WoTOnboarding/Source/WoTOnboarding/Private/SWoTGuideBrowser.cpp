#include "SWoTGuideBrowser.h"

#include "SWoTRetailArt.h"
#include "WoTAngrealGuide.h"
#include "WoTOnboardingChrome.h"
#include "WoTOnboardingHints.h"
#include "WoTOnboardingSeams.h"

#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCanvas.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
using WoTOnboardingChrome::EFace;

// The window in window units (plan Task 8): the frame's content area starts
// 48 in and 28 down, the grid fills the left, and the detail column runs to
// 18 past the content area's right edge, where the gem bar's ends reach, as
// the item-info window widens its gem bar past its text.
constexpr float WoTGuideBrowserInsetLeft = 48.0f;
constexpr float WoTGuideBrowserInsetTop = 28.0f;
constexpr float WoTGuideBrowserInsetRight = 30.0f;
constexpr float WoTGuideBrowserInsetBottom = 28.0f;
constexpr float WoTGuideBrowserGemReach = 18.0f;
constexpr int32 WoTGuideBrowserColumns = 8;
constexpr float WoTGuideBrowserIcon = 50.0f;
constexpr float WoTGuideBrowserPitch = 56.0f;
constexpr float WoTGuideBrowserGridTop = 10.0f;
constexpr float WoTGuideBrowserPaneGap = 12.0f;
/** The detail column, the gem bar's reach included on both sides. */
constexpr float WoTGuideBrowserDetailWidth = 422.0f;
constexpr float WoTGuideBrowserCounterIcon = 40.0f;
constexpr float WoTGuideBrowserDetailStep = 42.0f;
/** F_Element's height; InventoryInfoWindow lets each label overlap its gem
    by ElementStrOffsetY (2). */
constexpr float WoTGuideBrowserGemLabelHeight = 19.0f;
constexpr float WoTGuideBrowserGemLabelRise = WoTGuideBrowserGemLabelHeight - 2.0f;

/** Rarity order, the placeable traps (no angreal rarity) last. */
int32 WoTGuideBrowserGroupIndex(const FWoTAngrealGuideRow& Row)
{
	if (Row.Rarity == FName(TEXT("Common"))) return 0;
	if (Row.Rarity == FName(TEXT("Uncommon"))) return 1;
	if (Row.Rarity == FName(TEXT("Rare"))) return 2;
	return 3;
}

const TCHAR* WoTGuideBrowserRarityKey(const FWoTAngrealGuideRow& Row)
{
	if (Row.Rarity == FName(TEXT("Rare"))) return TEXT("RarityRareStr");
	if (Row.Rarity == FName(TEXT("Uncommon"))) return TEXT("RarityUncommonStr");
	return TEXT("RarityCommonStr");
}

FString WoTGuideBrowserRarityFallback(const FWoTAngrealGuideRow& Row)
{
	if (Row.Rarity == FName(TEXT("Rare"))) return TEXT("Rare artifact");
	if (Row.Rarity == FName(TEXT("Uncommon"))) return TEXT("Uncommon artifact");
	return TEXT("Common artifact");
}

/** A button that draws nothing of its own and never moves its content, so
    an icon sits exactly where the layout puts it. */
const FButtonStyle& WoTGuideBrowserIconButtonStyle()
{
	static const FButtonStyle Style = FButtonStyle(
		FCoreStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("NoBorder")))
		.SetNormalPadding(FMargin(0.0f))
		.SetPressedPadding(FMargin(0.0f));
	return Style;
}

const FSlateBrush* WoTGuideBrowserWhite()
{
	return FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
}

/** An icon slot whose icon did not resolve. */
FLinearColor WoTGuideBrowserEmptySlotColor()
{
	return FLinearColor(0.10f, 0.09f, 0.08f, 1.0f);
}

FName WoTGuideBrowserTag(const TCHAR* Prefix, FName ClassName)
{
	return FName(*FString::Printf(TEXT("%s.%s"), Prefix, *ClassName.ToString()));
}
} // namespace

FName SWoTGuideBrowser::OpeningRow(const UWoTAngrealGuide& InGuide, FName CardRow,
	FName HandItem)
{
	for (const FName Candidate : {CardRow, HandItem})
	{
		if (!Candidate.IsNone() && InGuide.FindByClassName(Candidate))
		{
			return Candidate;
		}
	}
	const FWoTAngrealGuideRow* First = nullptr;
	for (const FWoTAngrealGuideRow& Row : InGuide.Rows)
	{
		if (!First || WoTGuideBrowserGroupIndex(Row) < WoTGuideBrowserGroupIndex(*First))
		{
			First = &Row;
		}
	}
	return First ? First->ClassName : NAME_None;
}

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

	Faces = MakeShared<FWoTOnboardingFaces>(WoTOnboardingChrome::Resolve());
	Art = FWoTRetailArt::Resolve();
	Scale = MakeShared<FWoTOnboardingWindowScale>();
	Scale->Initialize(InArgs._InitialPhysicalSize, InArgs._InitialDpiScale);
	Faces->Refresh(Scale->GetPixelScale());

	if (!Guide.IsValid() || Guide->Rows.IsEmpty())
	{
		ChildSlot[SNullWidget::NullWidget];
		return;
	}

	Groups = {{TEXT("COMMON"), {}}, {TEXT("UNCOMMON"), {}}, {TEXT("RARE"), {}},
		{TEXT("TRAPS"), {}}};
	for (const FWoTAngrealGuideRow& Row : Guide->Rows)
	{
		Groups[WoTGuideBrowserGroupIndex(Row)].Rows.Add(Row.ClassName);
	}
	for (const FGridGroup& Group : Groups)
	{
		for (int32 Start = 0; Start < Group.Rows.Num(); Start += WoTGuideBrowserColumns)
		{
			GridLines.Emplace(Group.Rows.GetData() + Start,
				FMath::Min(WoTGuideBrowserColumns, Group.Rows.Num() - Start));
		}
	}
	SelectedClassName = OpeningRow(*Guide, InArgs._InitialRow, NAME_None);

	const TSharedRef<FWoTOnboardingFaces> FacesRef = Faces.ToSharedRef();
	const TSharedRef<FWoTOnboardingWindowScale> ScaleRef = Scale.ToSharedRef();
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(WoTGuideBrowserWhite())
			.ColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.82f))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			// The window draws at whole screen pixels per unit, with the
			// matching bitmap strikes, as the game's own windows do.
			SNew(SScaleBox)
			.Stretch(EStretch::UserSpecified)
			.UserSpecifiedScale_Lambda([ScaleRef]() { return ScaleRef->SlateScale(); })
			[
				SNew(SBox)
				.WidthOverride(WoTOnboardingChrome::WindowWidth)
				.HeightOverride(WoTOnboardingChrome::WindowHeight)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SWoTRetailArt)
						.Piece(EWoTRetailArtPiece::Window)
						.Art(Art)
					]
					+ SOverlay::Slot()
					.Padding(FMargin(WoTGuideBrowserInsetLeft, WoTGuideBrowserInsetTop,
						WoTGuideBrowserInsetRight, WoTGuideBrowserInsetBottom))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FMargin(0.0f, 0.0f, WoTGuideBrowserGemReach, 0.0f))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								WoTOnboardingChrome::MakeText(FacesRef, TEXT("FIELD GUIDE"),
									EFace::Body, WoTOnboardingChrome::TextColor(EFace::Body))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Tag(FName(TEXT("Close")))
								.ButtonStyle(&WoTGuideBrowserIconButtonStyle())
								.ContentPadding(FMargin(0.0f))
								.IsFocusable(false)
								.OnClicked(this, &SWoTGuideBrowser::Close)
								[
									WoTOnboardingChrome::MakeText(FacesRef, TEXT("Esc  Close"),
										EFace::Body, WoTOnboardingChrome::LabelColor(EFace::Body))
								]
							]
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(FMargin(0.0f, WoTGuideBrowserGridTop, 0.0f, 0.0f))
							[
								SNew(SBox)
								.WidthOverride(WoTGuideBrowserColumns * WoTGuideBrowserPitch)
								[
									SAssignNew(GridScroll, SScrollBox)
									.ScrollBarVisibility(EVisibility::Collapsed)
									+ SScrollBox::Slot()
									[
										BuildGrid()
									]
								]
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(FMargin(WoTGuideBrowserPaneGap, 0.0f, 0.0f, 0.0f))
							[
								SAssignNew(DetailScroll, SScrollBox)
								.ScrollBarVisibility(EVisibility::Collapsed)
								+ SScrollBox::Slot()
								[
									SAssignNew(DetailHost, SBox)
									.Tag(FName(TEXT("Guide.Detail")))
									.WidthOverride(WoTGuideBrowserDetailWidth)
								]
							]
						]
					]
				]
			]
		]
	];
	RebuildDetail();
	if (const TSharedPtr<SWidget>* Cell = GridCells.Find(SelectedClassName))
	{
		// Applied after the first layout.
		GridScroll->ScrollDescendantIntoView(*Cell, false);
	}
	if (Faces->GetChrome().BindWidget)
	{
		Faces->GetChrome().BindWidget(AsShared());
	}
}

TArray<FName> SWoTGuideBrowser::GridOrder() const
{
	TArray<FName> Order;
	for (const TArray<FName>& Line : GridLines)
	{
		Order.Append(Line);
	}
	return Order;
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

FString SWoTGuideBrowser::ResolveWindowText(const TCHAR* Key,
	const FString& Fallback) const
{
	// The item-info window's own display strings.
	const FString Text = TextResolver
		? TextResolver(TEXT("InventoryInfoWindow"), FString(Key)) : FString();
	return Text.IsEmpty() ? Fallback : Text;
}

FString SWoTGuideBrowser::DisplayTitle(const FWoTAngrealGuideRow& Row) const
{
	const FString Title = ResolveRowText(Row, TEXT("Title"));
	return Title.IsEmpty()
		? WoTOnboardingHints::SpacedClassName(Row.ClassName) : Title;
}

TSharedRef<SWidget> SWoTGuideBrowser::BuildGrid()
{
	const TSharedRef<FWoTOnboardingFaces> FacesRef = Faces.ToSharedRef();
	TSharedRef<SVerticalBox> Grid = SNew(SVerticalBox);
	bool bFirstGroup = true;
	for (const FGridGroup& Group : Groups)
	{
		if (Group.Rows.IsEmpty())
		{
			continue;
		}
		Grid->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, bFirstGroup ? 0.0f : 2.0f, 0.0f, 2.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, Group.Label, EFace::Body,
				WoTOnboardingChrome::LabelColor(EFace::Body))
		];
		bFirstGroup = false;
		for (int32 Start = 0; Start < Group.Rows.Num(); Start += WoTGuideBrowserColumns)
		{
			TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox);
			const int32 End = FMath::Min(Start + WoTGuideBrowserColumns, Group.Rows.Num());
			for (int32 Index = Start; Index < End; ++Index)
			{
				Line->AddSlot().AutoWidth()[BuildGridCell(Group.Rows[Index])];
			}
			Grid->AddSlot().AutoHeight()[Line];
		}
	}
	return Grid;
}

TSharedRef<SWidget> SWoTGuideBrowser::BuildGridCell(FName ClassName)
{
	TSharedRef<SOverlay> Face = SNew(SOverlay);
	// Grid icons draw at 50 of their 64 texels, so they sample smoothly.
	if (const FSlateBrush* Icon = Art->ItemIcon(ClassName, true))
	{
		Face->AddSlot()[SNew(SWoTRetailImage).Image(Icon)];
	}
	else
	{
		Face->AddSlot()
		[
			SNew(SImage)
			.Image(WoTGuideBrowserWhite())
			.ColorAndOpacity(WoTGuideBrowserEmptySlotColor())
		];
	}
	// The selection wears the HUD's gold selected-slot frame; without the art,
	// a flat highlight.
	const FSlateBrush* Frame = Art->SelectionFrame();
	Face->AddSlot()
	[
		SNew(SWoTRetailImage)
		.Image(Frame ? Frame : WoTGuideBrowserWhite())
		.ColorAndOpacity(Frame ? FLinearColor::White
		                       : FLinearColor(1.0f, 0.85f, 0.45f, 0.35f))
		.Visibility_Lambda([this, ClassName]()
		{
			return SelectedClassName == ClassName
				? EVisibility::HitTestInvisible : EVisibility::Hidden;
		})
	];

	TSharedRef<SWidget> Cell = SNew(SBox)
		.WidthOverride(WoTGuideBrowserPitch)
		.HeightOverride(WoTGuideBrowserPitch)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		[
			SNew(SButton)
			.Tag(WoTGuideBrowserTag(TEXT("Grid"), ClassName))
			.ButtonStyle(&WoTGuideBrowserIconButtonStyle())
			.ContentPadding(FMargin(0.0f))
			.IsFocusable(false)
			.OnClicked_Lambda([this, ClassName]()
			{
				SelectRow(ClassName);
				return FReply::Handled();
			})
			[
				SNew(SBox)
				.WidthOverride(WoTGuideBrowserIcon)
				.HeightOverride(WoTGuideBrowserIcon)
				[
					Face
				]
			]
		];
	GridCells.Add(ClassName, Cell);
	return Cell;
}

TSharedRef<SWidget> SWoTGuideBrowser::BuildGemBar(const FWoTAngrealGuideRow& Row)
{
	// InventoryInfoWindow.DrawAngrealGem: each label centred over its gem,
	// nudged by its own offset, its bottom 2 units into the bar.
	const TSharedRef<FWoTOnboardingFaces> FacesRef = Faces.ToSharedRef();
	TSharedRef<SCanvas> Labels = SNew(SCanvas);
	const TConstArrayView<FWoTRetailGem> Gems = SWoTRetailArt::Gems();
	for (int32 Index = 0; Index < Gems.Num(); ++Index)
	{
		const FWoTRetailGem& Gem = Gems[Index];
		Labels->AddSlot()
		.Position(FVector2D(SWoTRetailArt::GemX(WoTGuideBrowserDetailWidth, Index)
			+ Gem.LabelOffsetX, 0.0f))
		.Size(FVector2D(SWoTRetailArt::GemWidth, WoTGuideBrowserGemLabelHeight))
		[
			WoTOnboardingChrome::MakeText(FacesRef,
				ResolveWindowText(Gem.LabelKey, Gem.Element.ToString().ToUpper()),
				EFace::Element, WoTOnboardingChrome::TextColor(EFace::Element),
				ETextJustify::Center)
		];
	}
	return SNew(SBox)
		.WidthOverride(WoTGuideBrowserDetailWidth)
		.HeightOverride(WoTGuideBrowserGemLabelRise + SWoTRetailArt::GemBarHeight)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			.Padding(FMargin(0.0f, WoTGuideBrowserGemLabelRise, 0.0f, 0.0f))
			[
				SNew(SWoTRetailArt)
				.Tag(FName(TEXT("Guide.Detail.GemBar")))
				.Piece(EWoTRetailArtPiece::GemBar)
				.Art(Art)
				.Elements(Row.Elements)
			]
			+ SOverlay::Slot()
			[
				Labels
			]
		];
}

bool SWoTGuideBrowser::AddIconRow(SVerticalBox& Into, float Top, const TCHAR* Label,
	const TCHAR* TagPrefix, const TArray<const FWoTAngrealGuideRow*>& Targets)
{
	if (Targets.IsEmpty())
	{
		return false;
	}
	// The label then the icons, centred, wrapping onto a second line for a
	// long list. The label keeps 12 units from the first icon.
	TSharedRef<SWrapBox> Run = SNew(SWrapBox)
		.UseAllottedSize(true)
		.HAlign(HAlign_Center)
		.InnerSlotPadding(FVector2D(6.0f, 6.0f));
	Run->AddSlot()
	.VAlign(VAlign_Center)
	.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
	[
		WoTOnboardingChrome::MakeText(Faces.ToSharedRef(), Label, EFace::Body,
			WoTOnboardingChrome::LabelColor(EFace::Body))
	];
	for (const FWoTAngrealGuideRow* Target : Targets)
	{
		const FName ClassName = Target->ClassName;
		const FSlateBrush* Icon = Art->ItemIcon(ClassName, true);
		Run->AddSlot()
		[
			SNew(SButton)
			.Tag(WoTGuideBrowserTag(TagPrefix, ClassName))
			.ButtonStyle(&WoTGuideBrowserIconButtonStyle())
			.ContentPadding(FMargin(0.0f))
			.IsFocusable(false)
			.OnClicked_Lambda([this, ClassName]()
			{
				SelectRow(ClassName);
				return FReply::Handled();
			})
			[
				SNew(SBox)
				.WidthOverride(WoTGuideBrowserCounterIcon)
				.HeightOverride(WoTGuideBrowserCounterIcon)
				[
					SNew(SWoTRetailImage)
					.Image(Icon ? Icon : WoTGuideBrowserWhite())
					.ColorAndOpacity(Icon ? FLinearColor::White
					                      : WoTGuideBrowserEmptySlotColor())
				]
			]
		];
	}
	Into.AddSlot()
	.AutoHeight()
	.Padding(FMargin(WoTGuideBrowserGemReach, Top, WoTGuideBrowserGemReach, 0.0f))
	[
		Run
	];
	return true;
}

TSharedRef<SWidget> SWoTGuideBrowser::BuildDetail(const FWoTAngrealGuideRow& Row)
{
	const TSharedRef<FWoTOnboardingFaces> FacesRef = Faces.ToSharedRef();
	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);
	const auto AddLine = [&Body, &FacesRef](float Top, const FString& Line,
		EFace Face)
	{
		Body->AddSlot()
		.AutoHeight()
		.Padding(FMargin(WoTGuideBrowserGemReach, Top, WoTGuideBrowserGemReach, 0.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, Line, Face,
				WoTOnboardingChrome::TextColor(Face), ETextJustify::Center, true)
		];
	};

	// InventoryInfoWindow.Draw positions the header from the window's top.
	const bool bAngreal = !Row.Rarity.IsNone();
	TSharedRef<SOverlay> Header = SNew(SOverlay);
	Header->AddSlot()
	.VAlign(VAlign_Top)
	.Padding(FMargin(WoTGuideBrowserGemReach, 0.0f, WoTGuideBrowserGemReach, 0.0f))
	[
		WoTOnboardingChrome::MakeText(FacesRef, DisplayTitle(Row), EFace::Title,
			WoTOnboardingChrome::TextColor(EFace::Title), ETextJustify::Center, true)
	];
	TSharedRef<SOverlay> IconFace = SNew(SOverlay);
	if (const FSlateBrush* Icon = Art->ItemIcon(Row.ClassName, false))
	{
		IconFace->AddSlot()[SNew(SWoTRetailImage).Image(Icon)];
	}
	else
	{
		IconFace->AddSlot()
		[
			SNew(SImage)
			.Image(WoTGuideBrowserWhite())
			.ColorAndOpacity(WoTGuideBrowserEmptySlotColor())
		];
	}
	if (const FSlateBrush* Mask = Art->IconMask())
	{
		IconFace->AddSlot()[SNew(SWoTRetailImage).Image(Mask)];
	}
	Header->AddSlot()
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Top)
	.Padding(FMargin(0.0f, 28.0f, 0.0f, 0.0f))
	[
		SNew(SBox)
		.Tag(FName(TEXT("Guide.Detail.Icon")))
		.WidthOverride(64.0f)
		.HeightOverride(64.0f)
		[
			IconFace
		]
	];

	// The facts line: rarity (or the trap group's own tag) and the charge
	// envelope, the line the item-info window leads with.
	const FString RarityText = bAngreal
		? ResolveWindowText(WoTGuideBrowserRarityKey(Row),
			WoTGuideBrowserRarityFallback(Row))
		: FString(TEXT("Trap"));
	TSharedRef<SHorizontalBox> Facts = SNew(SHorizontalBox)
		.Tag(FName(TEXT("Guide.Detail.Facts")))
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			WoTOnboardingChrome::MakeText(FacesRef, RarityText, EFace::Body,
				WoTOnboardingChrome::TextColor(EFace::Body))
		];
	const FString ChargesLabel = ResolveWindowText(TEXT("ChargesStr"),
		TEXT("Charges:"));
	if (bAngreal && Row.MaxCharges > 0 && Row.ChargeCost == 0)
	{
		// InventoryInfoWindow.Draw: an angreal that spends no charge shows
		// the charge face's ':' glyph, its infinity mark. An engine stand-in
		// has no such glyph, so it draws the infinity sign itself.
		Facts->AddSlot()
		.AutoWidth()
		[
			WoTOnboardingChrome::MakeText(FacesRef, ChargesLabel, EFace::Body,
				WoTOnboardingChrome::TextColor(EFace::Body))
		];
		Facts->AddSlot()
		.AutoWidth()
		.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Font_Lambda([FacesRef]() { return FacesRef->Get(EFace::Charge); })
			.Text_Lambda([FacesRef]()
			{
				return FText::FromString(FacesRef->IsPublished(EFace::Charge)
					? FString(TEXT(":")) : FString(TEXT("\u221E")));
			})
			.ColorAndOpacity(WoTOnboardingChrome::TextColor(EFace::Charge))
		];
	}
	else if (bAngreal && Row.MaxCharges > 0)
	{
		Facts->AddSlot()
		.AutoWidth()
		[
			WoTOnboardingChrome::MakeText(FacesRef, ChargesLabel, EFace::Body,
				WoTOnboardingChrome::TextColor(EFace::Body))
		];
		Facts->AddSlot()
		.AutoWidth()
		.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, FString::FromInt(Row.MaxInitialCharges),
				EFace::Charge, WoTOnboardingChrome::TextColor(EFace::Charge))
		];
		Facts->AddSlot()
		.AutoWidth()
		.Padding(FMargin(3.0f, 0.0f))
		[
			WoTOnboardingChrome::MakeText(FacesRef, TEXT("/"), EFace::Body,
				WoTOnboardingChrome::TextColor(EFace::Body))
		];
		Facts->AddSlot()
		.AutoWidth()
		[
			WoTOnboardingChrome::MakeText(FacesRef, FString::FromInt(Row.MaxCharges),
				EFace::Charge, WoTOnboardingChrome::TextColor(EFace::Charge))
		];
	}
	Header->AddSlot()
	.VAlign(VAlign_Top)
	.Padding(FMargin(WoTGuideBrowserGemReach, 48.0f, WoTGuideBrowserGemReach, 0.0f))
	[
		Facts
	];

	float Gap = 10.0f;
	if (bAngreal)
	{
		Header->AddSlot()
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.0f, 96.0f - WoTGuideBrowserGemLabelRise, 0.0f, 0.0f))
		[
			BuildGemBar(Row)
		];
		Gap = 8.0f;
	}

	Body->AddSlot()
	.AutoHeight()
	[
		SNew(SBox)
		.WidthOverride(WoTGuideBrowserDetailWidth)
		.HeightOverride(bAngreal ? 96.0f + SWoTRetailArt::GemBarHeight : 28.0f + 64.0f)
		[
			Header
		]
	];

	// Prose only where the retail text resolves; a row without it shows its
	// facts and nothing else, never placeholder text.
	const FString Description = ResolveRowText(Row, TEXT("Description"));
	const FString Quote = ResolveRowText(Row, TEXT("Quote"));
	if (!Description.IsEmpty())
	{
		AddLine(Gap, Description, EFace::Body);
		Gap = 8.0f;
	}
	if (!Description.IsEmpty() && !Quote.IsEmpty())
	{
		Body->AddSlot()
		.AutoHeight()
		.Padding(FMargin(WoTGuideBrowserGemReach, 2.0f, WoTGuideBrowserGemReach, 0.0f))
		[
			SNew(SWoTRetailArt)
			.Piece(EWoTRetailArtPiece::Divider)
			.Art(Art)
			.Indent(32.0f)
		];
		Gap = 4.0f;
	}
	if (!Quote.IsEmpty())
	{
		AddLine(Gap, Quote, EFace::Italic);
	}
	Gap = 10.0f;

	// What counters this row, the hint that teaches it, then what this row
	// counters.
	if (AddIconRow(*Body, Gap, TEXT("COUNTERED BY"), TEXT("CounteredBy"),
		Guide->CounteredBy(Row.ClassName)))
	{
		Gap = 8.0f;
	}
	const FString HintLine = WoTOnboardingHints::ResolveHintLine(*Guide, Row,
		[this](const FWoTAngrealGuideRow& HintRow)
		{
			return ResolveRowText(HintRow, TEXT("Title"));
		});
	if (!HintLine.IsEmpty())
	{
		AddLine(Gap, HintLine, EFace::Body);
		Gap = 10.0f;
	}
	TArray<const FWoTAngrealGuideRow*> Counters;
	for (const FName& Countered : Row.Counters)
	{
		if (const FWoTAngrealGuideRow* Target = Guide->FindByClassName(Countered))
		{
			Counters.Add(Target);
		}
	}
	AddIconRow(*Body, Gap, TEXT("COUNTERS"), TEXT("Counters"), Counters);
	Body->AddSlot()
	.AutoHeight()
	[
		SNew(SBox).HeightOverride(10.0f)
	];
	return Body;
}

void SWoTGuideBrowser::RebuildDetail()
{
	if (!DetailHost.IsValid())
	{
		return;
	}
	const FWoTAngrealGuideRow* Row = Guide.IsValid()
		? Guide->FindByClassName(SelectedClassName) : nullptr;
	DetailHost->SetContent(Row ? BuildDetail(*Row) : SNullWidget::NullWidget);
	if (DetailScroll.IsValid())
	{
		DetailScroll->ScrollToStart();
	}
}

void SWoTGuideBrowser::SelectRow(FName ClassName)
{
	if (!Guide.IsValid() || !Guide->FindByClassName(ClassName))
	{
		return;
	}
	SelectedClassName = ClassName;
	RebuildDetail();
	if (GridScroll.IsValid())
	{
		if (const TSharedPtr<SWidget>* Cell = GridCells.Find(ClassName))
		{
			GridScroll->ScrollDescendantIntoView(*Cell, false,
				EDescendantScrollDestination::IntoView, 4.0f);
		}
	}
}

void SWoTGuideBrowser::MoveSelection(int32 Columns, int32 Lines)
{
	int32 LineIndex = INDEX_NONE;
	int32 Column = INDEX_NONE;
	for (int32 Index = 0; Index < GridLines.Num() && LineIndex == INDEX_NONE; ++Index)
	{
		Column = GridLines[Index].IndexOfByKey(SelectedClassName);
		if (Column != INDEX_NONE)
		{
			LineIndex = Index;
		}
	}
	if (LineIndex == INDEX_NONE)
	{
		return;
	}
	FName Target;
	if (Columns != 0)
	{
		const TArray<FName> Order = GridOrder();
		const int32 Next = Order.IndexOfByKey(SelectedClassName) + Columns;
		if (Order.IsValidIndex(Next))
		{
			Target = Order[Next];
		}
	}
	else if (GridLines.IsValidIndex(LineIndex + Lines))
	{
		const TArray<FName>& Line = GridLines[LineIndex + Lines];
		Target = Line[FMath::Min(Column, Line.Num() - 1)];
	}
	if (!Target.IsNone())
	{
		SelectRow(Target);
	}
}

void SWoTGuideBrowser::ScrollDetail(float Units)
{
	if (DetailScroll.IsValid())
	{
		DetailScroll->SetScrollOffset(FMath::Clamp(
			DetailScroll->GetScrollOffset() + Units, 0.0f,
			DetailScroll->GetScrollOffsetOfEnd()));
	}
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
	const FKey Key = InKeyEvent.GetKey();
	if (IsCloseKey(Key))
	{
		return Close();
	}
	// The arrow keys, the d-pad and the left stick walk the grid; page keys
	// and the right stick scroll a long detail pane.
	struct FWoTGuideBrowserStep
	{
		FKey Keys[3];
		int32 Columns;
		int32 Lines;
	};
	const FWoTGuideBrowserStep Steps[] = {
		{{EKeys::Left, EKeys::Gamepad_DPad_Left, EKeys::Gamepad_LeftStick_Left}, -1, 0},
		{{EKeys::Right, EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right}, 1, 0},
		{{EKeys::Up, EKeys::Gamepad_DPad_Up, EKeys::Gamepad_LeftStick_Up}, 0, -1},
		{{EKeys::Down, EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down}, 0, 1},
	};
	for (const FWoTGuideBrowserStep& Step : Steps)
	{
		for (const FKey& StepKey : Step.Keys)
		{
			if (Key == StepKey)
			{
				MoveSelection(Step.Columns, Step.Lines);
				return FReply::Handled();
			}
		}
	}
	if (Key == EKeys::PageUp || Key == EKeys::Gamepad_RightStick_Up)
	{
		ScrollDetail(-WoTGuideBrowserDetailStep);
		return FReply::Handled();
	}
	if (Key == EKeys::PageDown || Key == EKeys::Gamepad_RightStick_Down)
	{
		ScrollDetail(WoTGuideBrowserDetailStep);
		return FReply::Handled();
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

void SWoTGuideBrowser::Tick(const FGeometry& AllottedGeometry,
	const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	if (Scale.IsValid() && Faces.IsValid() && Scale->Update(AllottedGeometry))
	{
		Faces->Refresh(Scale->GetPixelScale());
	}
}
