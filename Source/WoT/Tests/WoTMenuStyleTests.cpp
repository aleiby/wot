// menuWOT-family and UWindow browser font/string/style/layout parity.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTHUDFonts.h"
#include "../WoTMenuStrings.h"
#include "../WoTMenuStyle.h"
#include "../WoTRetailText.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "../Payload/WoTFrontEndFontOwner.h"
#include "../FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "WoTFrontEndTestData.h"

#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "InputCoreTypes.h"
#include "UObject/Package.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"
#include "Fonts/FontCache.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

namespace
{
	template <typename T>
	FWoTRuntimeWorldProperty BrowserDefaultValue(const TCHAR* Name,
		EWoTRuntimeWorldPropertyType Type, T Value, uint32 ArrayIndex = 0)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = Type;
		Property.ArrayIndex = ArrayIndex;
		Property.Value.SetNumUninitialized(sizeof(T));
		FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(T));
		return Property;
	}

	FWoTRuntimeWorldProperty BrowserRegion(const TCHAR* Name, uint32 Index,
		int32 Width, int32 Height)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.ArrayIndex = Index;
		Property.StructName = TEXT("Region");
		Property.StructMembers.Add(BrowserDefaultValue(TEXT("X"),
			EWoTRuntimeWorldPropertyType::Integer, int32(0)));
		Property.StructMembers.Add(BrowserDefaultValue(TEXT("Y"),
			EWoTRuntimeWorldPropertyType::Integer, int32(0)));
		Property.StructMembers.Add(BrowserDefaultValue(TEXT("W"),
			EWoTRuntimeWorldPropertyType::Integer, Width));
		Property.StructMembers.Add(BrowserDefaultValue(TEXT("H"),
			EWoTRuntimeWorldPropertyType::Integer, Height));
		return Property;
	}

	FWoTRuntimeWorldProperty BrowserColor(const TCHAR* Name, uint8 R,
		uint8 G, uint8 B)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.StructName = TEXT("Color");
		Property.StructMembers.Add(BrowserDefaultValue(TEXT("R"),
			EWoTRuntimeWorldPropertyType::Byte, R));
		Property.StructMembers.Add(BrowserDefaultValue(TEXT("G"),
			EWoTRuntimeWorldPropertyType::Byte, G));
		Property.StructMembers.Add(BrowserDefaultValue(TEXT("B"),
			EWoTRuntimeWorldPropertyType::Byte, B));
		return Property;
	}

	bool LoadBrowserDefaults(FWoTRuntimeClassDefaults& Defaults,
		bool bOmitActiveColor, FString& OutError,
		FColor InactiveHeading = FColor(21, 43, 65),
		FColor EditText = FColor(76, 54, 32),
		bool bOmitImplicitColors = false)
	{
		FWoTRuntimeClassDefaultRecord BaseRecord;
		BaseRecord.Identity = TEXT("UWindow.UWindowLookAndFeel");
		if (!bOmitImplicitColors)
		{
			BaseRecord.Properties.Add(BrowserColor(
				TEXT("HeadingInActiveTitleColor"), InactiveHeading.R,
				InactiveHeading.G, InactiveHeading.B));
			BaseRecord.Properties.Add(BrowserColor(TEXT("EditBoxTextColor"),
				EditText.R, EditText.G, EditText.B));
		}
		FWoTRuntimeClassDefaultRecord Record;
		Record.Identity = TEXT("UWindow.UWindowWin95LookAndFeel");
		Record.SuperclassIdentity = BaseRecord.Identity;
		const TPair<const TCHAR*, uint32> Regions[] = {
			{TEXT("BevelUpTL"),0},{TEXT("BevelUpT"),0},{TEXT("BevelUpTR"),0},
			{TEXT("BevelUpL"),0},{TEXT("BevelUpArea"),0},{TEXT("BevelUpR"),0},
			{TEXT("BevelUpBL"),0},{TEXT("BevelUpB"),0},{TEXT("BevelUpBR"),0},
			{TEXT("FrameTL"),0},{TEXT("FrameT"),0},{TEXT("FrameTR"),0},
			{TEXT("FrameL"),0},{TEXT("FrameR"),0},{TEXT("FrameSBL"),0},
			{TEXT("FrameSB"),0},{TEXT("FrameSBR"),0},
			{TEXT("MiscBevelTL"),1},{TEXT("MiscBevelT"),1},
			{TEXT("MiscBevelTR"),1},{TEXT("MiscBevelL"),1},
			{TEXT("MiscBevelArea"),1},{TEXT("MiscBevelR"),1},
			{TEXT("MiscBevelBL"),1},{TEXT("MiscBevelB"),1},
			{TEXT("MiscBevelBR"),1},{TEXT("MiscBevelArea"),2},
			{TEXT("MiscBevelTL"),2},{TEXT("SBUpUp"),0},
			{TEXT("SBDownUp"),0},{TEXT("SBLeftUp"),0},
			{TEXT("SBRightUp"),0},{TEXT("SBBackground"),0}};
		for (const auto& Region : Regions)
		{
			const int32 Height = FCString::Strcmp(Region.Key, TEXT("FrameT")) == 0
				? 7 : 1;
			Record.Properties.Add(BrowserRegion(
				Region.Key, Region.Value, 1, Height));
		}
		if (!bOmitActiveColor)
			Record.Properties.Add(BrowserColor(
				TEXT("FrameActiveTitleColor"), 123, 45, 67));
		Record.Properties.Add(BrowserColor(
			TEXT("FrameInactiveTitleColor"), 12, 34, 56));
		Record.Properties.Add(BrowserColor(
			TEXT("HeadingActiveTitleColor"), 9, 87, 165));
		Record.Properties.Add(BrowserDefaultValue(TEXT("Size_ScrollbarWidth"),
			EWoTRuntimeWorldPropertyType::Float, 17.0f));
		Record.Properties.Add(BrowserDefaultValue(TEXT("ColumnHeadingHeight"),
			EWoTRuntimeWorldPropertyType::Integer, int32(19)));
		TArray<FWoTRuntimeClassDefaultRecord> Records;
		Records.Add(MoveTemp(BaseRecord));
		Records.Add(MoveTemp(Record));
		return Defaults.LoadFixture(MoveTemp(Records), OutError);
	}

	class FWoTMenuFeedbackRecorder final : public IWoTFrontEndFeedbackSink
	{
	public:
		virtual bool IsReady() const override { return bReady; }

		virtual void MoveSelector() override
		{
			Events.Add(TEXT("MoveSelector"));
		}

		virtual void Select() override
		{
			Events.Add(TEXT("Select"));
		}

		TArray<FString> Events;
		bool bReady = true;
	};
}

namespace
{
	struct FWoTMenuStyleTestWorld
	{
		FWoTMenuStyleTestWorld()
			: World(WoTTest::CreateSpecWorld())
		{
		}

		~FWoTMenuStyleTestWorld()
		{
			WoTTest::DestroySpecWorld(World);
		}

		UWorld* World = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTMenuFontSelectionSpec, "WoT.Menu.Style.FontSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMenuFontSelectionSpec::RunTest(const FString&)
{
	const FSlateFontInfo Missing = FWoTMenuFontSelection::Select(
		nullptr, 30.0f, FName(TEXT("Regular")));
	TestNull(TEXT("null fallback remains independent of a retail FontObject"),
		Missing.FontObject.Get());
	TestTrue(TEXT("optional face retains Core composite fallback"),
		Missing.CompositeFont.IsValid());
	TestEqual(TEXT("null fallback preserves requested typeface"),
		Missing.TypefaceFontName, FName(TEXT("Regular")));
	TestEqual(TEXT("null fallback preserves requested size"),
		Missing.Size, 30.0f);

	const FSlateFontInfo Optional = FWoTMenuFontSelection::Select(
		nullptr, 8.0f, FName(TEXT("Italic")));
	TestTrue(TEXT("non-required guarded faces retain the existing engine fallback"),
		Optional.CompositeFont.IsValid());
	TestEqual(TEXT("optional fallback preserves its requested typeface"),
		Optional.TypefaceFontName, FName(TEXT("Italic")));

	UFont* Retail = NewObject<UFont>(GetTransientPackage());
	const FSlateFontInfo Preserved = FWoTMenuFontSelection::Select(
		Retail, 14.0f, FName(TEXT("Regular")));
	TestTrue(TEXT("non-null branch must not clobber the retail FontObject"),
		Preserved.FontObject.Get() == Retail);
	TestFalse(TEXT("non-null retail object-font path remains non-composite"),
		Preserved.CompositeFont.IsValid());
	TestEqual(TEXT("non-null retail input preserves requested size"),
		Preserved.Size, 14.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTMenuFontRegistrySpec, "WoT.Menu.Style.FontRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMenuFontRegistrySpec::RunTest(const FString&)
{
	FWoTMenuStyleTestWorld Fixture;
	if (!TestNotNull(TEXT("WoTTestWorld fixture"), Fixture.World))
	{
		return false;
	}

	UWoTHUDFonts* Registry = LoadObject<UWoTHUDFonts>(
		nullptr, TEXT("/Game/WoT/UI/Fonts/DA_WoTHUDFonts.DA_WoTHUDFonts"));
	if (!TestNotNull(TEXT("HUD font registry"), Registry))
	{
		return false;
	}

	const FName OldFontIds[] = {
		FName(TEXT("F_Charge")), FName(TEXT("F_Charge_S")),
		FName(TEXT("F_Key")), FName(TEXT("F_Key_S")),
		FName(TEXT("F_KeySelected")), FName(TEXT("F_KeySelected_S")),
		FName(TEXT("F_WOTReg14")), FName(TEXT("F_WOTReg14_S"))
	};
	for (const FName Id : OldFontIds)
	{
		UFont* Font = Registry->Resolve(Id);
		TestNotNull(
			*FString::Printf(TEXT("registry resolves %s"), *Id.ToString()),
			Font);
		if (Font)
		{
			TestEqual(TEXT("resolved font preserves asset id"), Font->GetFName(), Id);
		}
	}

	const FName NewFontIds[] = {
		FName(TEXT("F_WOTReg30")), FName(TEXT("F_WOTReg30_S")),
		FName(TEXT("F_WOTReg08")), FName(TEXT("F_WOTIta14"))
	};
	for (const FName Id : NewFontIds)
	{
		UFont* Font = Registry->Resolve(Id);
		TestNotNull(
			*FString::Printf(TEXT("registry resolves committed %s"), *Id.ToString()),
			Font);
		if (Font)
		{
			TestEqual(TEXT("resolved font preserves asset id"), Font->GetFName(), Id);
		}
	}

	const FName TahomaFontIds[] = {
		FName(TEXT("Tahoma10")), FName(TEXT("Tahoma20")),
		FName(TEXT("Tahoma30")), FName(TEXT("TahomaB10")),
		FName(TEXT("TahomaB20")), FName(TEXT("TahomaB30"))
	};
	for (const FName Id : TahomaFontIds)
	{
		UFont* Font = Registry->Resolve(Id);
		TestNotNull(
			*FString::Printf(TEXT("registry resolves committed %s"), *Id.ToString()),
			Font);
		if (Font)
		{
			TestEqual(TEXT("resolved Tahoma preserves asset id"), Font->GetFName(), Id);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTMenuStringAssetSpec, "WoT.Menu.Style.StringAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMenuStringAssetSpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping menu asset pins"));
		return true;
	}
	const struct
	{
		const TCHAR* Section;
		const TCHAR* Key;
		const TCHAR* AssetKey;
	} Pins[] = {
		{TEXT("menuMain"), TEXT("MenuList[2]"), TEXT("menuMain.MenuList[2]")},
		{TEXT("menuMain"), TEXT("MenuList[7]"), TEXT("menuMain.MenuList[7]")},
		{TEXT("menuMultiPlayer"), TEXT("HelpMessage[1]"),
			TEXT("menuMultiPlayer.HelpMessage[1]")},
		{TEXT("menuLoad"), TEXT("RestartString"), TEXT("menuLoad.RestartString")},
	};
	FWoTMenuStyleTestWorld Fixture;
	if (!TestNotNull(TEXT("WoTTestWorld fixture"), Fixture.World))
	{
		return false;
	}

	UWoTMenuStrings* Strings = LoadObject<UWoTMenuStrings>(
		nullptr,
		TEXT("/Game/WoT/UI/Menus/DA_WoTMenuStrings.DA_WoTMenuStrings"));
	if (!TestNotNull(TEXT("committed menu string asset"), Strings))
	{
		return false;
	}

	for (const auto& Pin : Pins)
	{
		const FString* Expected = FWoTRetailText::FindSectionValue(Pin.Section, Pin.Key);
		if (TestNotNull(Pin.AssetKey, Expected))
		{
			TestEqual(Pin.AssetKey, Strings->Resolve(FName(Pin.AssetKey)).ToString(),
				*Expected);
		}
	}
	TestTrue(TEXT("all retail menu sections imported"),
		Strings->Sections.Num() >= 15);

	const ANSICHAR SyntheticLocalization[] =
		"[menuMain]\r\n"
		"MenuList[2]=MMMMMMMMMMM\r\n"
		"MenuList[3]=CCCCCCCC\r\n"
		"MenuList[4]=HHHHHHHH\r\n"
		"MenuList[7]=QQQQ\r\n";
	TMap<FName, FString> Projected;
	TestTrue(TEXT("authenticated WoT.int bytes project the bounded four labels"),
		FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(
			MakeArrayView(reinterpret_cast<const uint8*>(SyntheticLocalization),
				static_cast<int32>(sizeof(SyntheticLocalization) - 1)), Projected));
	TestEqual(TEXT("bounded localization projection has exactly four entries"),
		Projected.Num(), 4);
	TestEqual(TEXT("bounded projection keeps Controls"),
		Projected.FindRef(FName(TEXT("menuMain.MenuList[3]"))),
		FString(TEXT("CCCCCCCC")));
	const ANSICHAR MissingQuit[] =
		"[menuMain]\nMenuList[2]=MMMMMMMMMMM\nMenuList[3]=CCCCCCCC\n"
		"MenuList[4]=HHHHHHHH\n";
	TestFalse(TEXT("a missing required localized action fails closed"),
		FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(
			MakeArrayView(reinterpret_cast<const uint8*>(MissingQuit),
				static_cast<int32>(sizeof(MissingQuit) - 1)), Projected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTMenuLayoutSpec, "WoT.Menu.Style.LayoutMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMenuLayoutSpec::RunTest(const FString&)
{
	TestEqual(TEXT("single entry has no spacing"),
		FWoTMenuLayout::CalculateSpacing(1, 30.0f, 480.0f), 0.0f);
	// Keep the explicit KINDA_SMALL tolerance on every float parity pin so the
	// contract does not depend on AutomationTest's/default float tolerance.
	TestTrue(TEXT("five 14px entries truncate ideal 88.667 and final 4.5"),
		FMath::IsNearlyEqual(
			FWoTMenuLayout::CalculateSpacing(5, 14.0f, 480.0f),
			4.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("SizeY 360 still uses the Slate high-res branch"),
		FMath::IsNearlyEqual(
			FWoTMenuLayout::CalculateSpacing(3, 30.0f, 360.0f),
			10.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("no-fit branch overlaps rows with negative spacing"),
		FMath::IsNearlyEqual(
			FWoTMenuLayout::CalculateSpacing(5, 20.0f, 80.0f),
			-5.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("both integer coercions remain visible with 13px rows"),
		FMath::IsNearlyEqual(
			FWoTMenuLayout::CalculateSpacing(5, 13.0f, 480.0f),
			4.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("list height consumes truncated spacing"),
		FMath::IsNearlyEqual(
			FWoTMenuLayout::CalculateListHeight(5, 14.0f, 4.0f),
			86.0f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("center-left column start"),
		FMath::IsNearlyEqual(
			FWoTMenuLayout::LeftColumnStart(800.0f), 240.0f,
			KINDA_SMALL_NUMBER));
	TestTrue(TEXT("center-right column start"),
		FMath::IsNearlyEqual(
			FWoTMenuLayout::RightColumnStart(800.0f), 480.0f,
			KINDA_SMALL_NUMBER));

	const FVector2D PhysicalMeasurements[] = {
		FVector2D(212.0f, 30.0f), FVector2D(156.0f, 30.0f),
		FVector2D(174.0f, 30.0f), FVector2D(70.0f, 30.0f)
	};
	const struct
	{
		const TCHAR* Label;
		FVector2D PhysicalViewport;
		float DPIScale;
		FSlateRect PhysicalSafeRegion;
	} Cases[] = {
		{TEXT("4:3 1024x768"), FVector2D(1024.0f, 768.0f), 1.0f,
			FSlateRect(0.0f, 0.0f, 1024.0f, 768.0f)},
		{TEXT("16:9 1920x1080"), FVector2D(1920.0f, 1080.0f), 1.0f,
			FSlateRect(240.0f, 0.0f, 1680.0f, 1080.0f)},
		{TEXT("16:10 1920x1200"), FVector2D(1920.0f, 1200.0f), 1.0f,
			FSlateRect(160.0f, 0.0f, 1760.0f, 1200.0f)},
		{TEXT("ultrawide 3440x1440"), FVector2D(3440.0f, 1440.0f), 1.0f,
			FSlateRect(760.0f, 0.0f, 2680.0f, 1440.0f)},
		{TEXT("16:9 at 200 percent DPI"), FVector2D(1920.0f, 1080.0f), 2.0f,
			FSlateRect(240.0f, 0.0f, 1680.0f, 1080.0f)}
	};
	for (const auto& Case : Cases)
	{
		TArray<FVector2D> LocalMeasurements;
		for (const FVector2D& Physical : PhysicalMeasurements)
		{
			LocalMeasurements.Add(Physical / Case.DPIScale);
		}
		const FWoTRetailMenuLayout Layout =
			FWoTMenuLayout::CalculateRetailMainLayout(
				Case.PhysicalViewport / Case.DPIScale, LocalMeasurements);
		const FSlateRect PhysicalSafe(
			Layout.SafeRegion.Left * Case.DPIScale,
			Layout.SafeRegion.Top * Case.DPIScale,
			Layout.SafeRegion.Right * Case.DPIScale,
			Layout.SafeRegion.Bottom * Case.DPIScale);
		TestTrue(FString::Printf(TEXT("%s uses the centered 4:3 safe region"),
			Case.Label), PhysicalSafe == Case.PhysicalSafeRegion);
		const FVector2D PhysicalListCenter =
			(Layout.ListPosition + Layout.ListSize * 0.5f) * Case.DPIScale;
		TestTrue(FString::Printf(TEXT("%s centers the measured list horizontally"),
			Case.Label), FMath::IsNearlyEqual(
				PhysicalListCenter.X, Case.PhysicalViewport.X * 0.5f));
		TestTrue(FString::Printf(TEXT("%s centers the measured list vertically"),
			Case.Label), FMath::IsNearlyEqual(
				PhysicalListCenter.Y, Case.PhysicalViewport.Y * 0.5f));
		TestTrue(FString::Printf(TEXT("%s uses the measured longest label"),
			Case.Label), FMath::IsNearlyEqual(
				Layout.ListSize.X * Case.DPIScale, 212.0f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTMenuFeedbackSpec, "WoT.Menu.Style.RetailFeedback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMenuFeedbackSpec::RunTest(const FString&)
{
	TestEqual(TEXT("production MoveSelector uses the authenticated closure ID"),
		FString(FWoTFrontEndFeedbackContract::MoveSelectorLogicalId()),
		FString(TEXT("cache/sound/WOT__MoveSelector")));
	TestEqual(TEXT("production Select uses the authenticated closure ID"),
		FString(FWoTFrontEndFeedbackContract::SelectLogicalId()),
		FString(TEXT("cache/sound/WOT__Select")));
	TestTrue(TEXT("typed authenticated and app volume admit feedback"),
		FWoTFrontEndFeedbackContract::ShouldPlay(0.5f, 0.75f));
	TestFalse(TEXT("typed muted preference suppresses feedback"),
		FWoTFrontEndFeedbackContract::ShouldPlay(0.0f, 1.0f));
	TestFalse(TEXT("muted app multiplier suppresses feedback"),
		FWoTFrontEndFeedbackContract::ShouldPlay(1.0f, 0.0f));

	TSharedRef<FWoTMenuFeedbackRecorder> Feedback =
		MakeShared<FWoTMenuFeedbackRecorder>();
	FWoTFrontEndMenuSelection Selection(Feedback);
	TestEqual(TEXT("retail selection starts on Multiplayer"),
		Selection.GetSelectedIndex(), 0);
	TestFalse(TEXT("programmatic focus on the current item is a no-op"),
		Selection.MoveTo(0));
	TestTrue(TEXT("keyboard/controller navigation changes the shared index"),
		Selection.MoveTo(1));
	TestFalse(TEXT("repeated focus cannot replay MoveSelector"),
		Selection.MoveTo(1));
	TestTrue(TEXT("mouse hover changes the same shared index"),
		Selection.MoveTo(2));
	TestFalse(TEXT("an invalid index is rejected"), Selection.MoveTo(4));

	int32 ActivationCount = 0;
	TestTrue(TEXT("a valid click/keyboard activation is accepted"),
		Selection.Activate(2, [&ActivationCount, &Feedback]()
		{
			Feedback->Events.Add(TEXT("Action"));
			++ActivationCount;
		}));
	TestEqual(TEXT("the valid action ran exactly once"), ActivationCount, 1);
	TestFalse(TEXT("invalid activation is rejected"),
		Selection.Activate(INDEX_NONE,
			[&ActivationCount]() { ++ActivationCount; }));
	TestEqual(TEXT("invalid activation did not run an action"), ActivationCount, 1);
	Feedback->bReady = false;
	TestFalse(TEXT("revoked authenticated feedback fails activation closed"),
		Selection.Activate(1,
			[&ActivationCount]() { ++ActivationCount; }));
	TestEqual(TEXT("revoked feedback did not run an action"), ActivationCount, 1);

	const TArray<FString> Expected = {
		TEXT("MoveSelector"), TEXT("MoveSelector"), TEXT("Select"),
		TEXT("Action")};
	TestEqual(TEXT("MoveSelector occurs once per change and Select immediately before activation"),
		Feedback->Events, Expected);
	const FText AccessibleLabel = FText::FromString(TEXT("MMMMMMMMMMM"));
	const TSharedRef<SWoTMenuButton> AccessibleButton =
		SNew(SWoTMenuButton)
		.AccessibleText(AccessibleLabel)
		[
			SNullWidget::NullWidget
		];
	TestTrue(TEXT("retail entries participate in keyboard/controller focus"),
		AccessibleButton->SupportsKeyboardFocus());
#if WITH_ACCESSIBILITY
	TestEqual(TEXT("retail entry exposes its localized accessible name"),
		AccessibleButton->GetAccessibleText().ToString(),
		FString(TEXT("MMMMMMMMMMM")));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTMenuColorSpec, "WoT.Menu.Style.Colors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMenuColorSpec::RunTest(const FString&)
{
	const FLinearColor Selected = FWoTMenuStyle::SelectedColor();
	const FLinearColor Unselected = FWoTMenuStyle::UnselectedColor();
	TestTrue(TEXT("selected is pure white"),
		Selected.Equals(FLinearColor::White));
	const float RetailDefault = 127.0f / 255.0f;
	TestTrue(TEXT("unselected restores Canvas default 127 gray"),
		Unselected.Equals(FLinearColor(
			RetailDefault, RetailDefault, RetailDefault, 1.0f)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailBrowserDecodedLookSpec,
	"WoT.Menu.Style.DecodedLookAndFeelMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailBrowserDecodedLookSpec::RunTest(const FString&)
{
	FString Error;
	FWoTRuntimeClassDefaults Defaults;
	if (!TestTrue(TEXT("mutated look-and-feel fixture loads"),
		LoadBrowserDefaults(Defaults, false, Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTBrowserLookAndFeel Look;
	if (!TestTrue(TEXT("complete typed look-and-feel decodes"),
		FWoTRetailBrowserStyle::DecodeLookAndFeelForTesting(
			Defaults, Look, Error)))
	{
		AddError(Error);
		return false;
	}
	const TSharedRef<FWoTRetailBrowserStyle> Style =
		FWoTRetailBrowserStyle::CreateForTesting(FSlateFontInfo(), &Look);
	TestEqual(TEXT("mutated region drives produced title geometry"),
		Style->TitleBarHeight(), 14.0f);
	TestTrue(TEXT("mutated color drives produced title style"),
		Style->TitleForeground().Equals(
			FLinearColor(FColor(123, 45, 67, 255))));
	TestEqual(TEXT("mutated size drives produced scrollbar reservation"),
		Style->ScrollBarReservation(), 38.0f);
	TestEqual(TEXT("mutated column heading height drives produced geometry"),
		Style->HeadingHeight(), 38.0f);
	TestTrue(TEXT("mutated inherited heading color drives produced heading style"),
		Style->HeadingForeground().Equals(
			FLinearColor(FColor(9, 87, 165, 255))));
	const FLinearColor FirstInactive(FColor(21, 43, 65, 255));
	const FLinearColor FirstEdit(FColor(76, 54, 32, 255));
	TestTrue(TEXT("mutated inherited inactive heading color drives inactive headings"),
		Style->HeadingForeground(false).Equals(FirstInactive));
	TestTrue(TEXT("mutated inherited edit color drives editable text"),
		Style->EditableTextBoxStyle()->ForegroundColor.GetSpecifiedColor().Equals(
			FirstEdit));

	FWoTRuntimeClassDefaults InactiveMutationDefaults;
	TestTrue(TEXT("independent inactive-heading mutation fixture loads"),
		LoadBrowserDefaults(InactiveMutationDefaults, false, Error,
			FColor(151, 31, 73), FColor(76, 54, 32)));
	FWoTBrowserLookAndFeel InactiveMutation;
	TestTrue(TEXT("independent inactive-heading mutation decodes"),
		FWoTRetailBrowserStyle::DecodeLookAndFeelForTesting(
			InactiveMutationDefaults, InactiveMutation, Error));
	const TSharedRef<FWoTRetailBrowserStyle> InactiveStyle =
		FWoTRetailBrowserStyle::CreateForTesting(
			FSlateFontInfo(), &InactiveMutation);
	TestTrue(TEXT("only the inactive heading source mutation changes inactive rendering"),
		InactiveStyle->HeadingForeground(false).Equals(
			FLinearColor(FColor(151, 31, 73, 255))));
	TestTrue(TEXT("inactive heading mutation leaves edit rendering unchanged"),
		InactiveStyle->EditableTextBoxStyle()->ForegroundColor
			.GetSpecifiedColor().Equals(FirstEdit));

	FWoTRuntimeClassDefaults EditMutationDefaults;
	TestTrue(TEXT("independent edit-text mutation fixture loads"),
		LoadBrowserDefaults(EditMutationDefaults, false, Error,
			FColor(21, 43, 65), FColor(17, 149, 203)));
	FWoTBrowserLookAndFeel EditMutation;
	TestTrue(TEXT("independent edit-text mutation decodes"),
		FWoTRetailBrowserStyle::DecodeLookAndFeelForTesting(
			EditMutationDefaults, EditMutation, Error));
	const TSharedRef<FWoTRetailBrowserStyle> EditStyle =
		FWoTRetailBrowserStyle::CreateForTesting(FSlateFontInfo(), &EditMutation);
	TestTrue(TEXT("only the edit source mutation changes editable-text rendering"),
		EditStyle->EditableTextBoxStyle()->ForegroundColor.GetSpecifiedColor()
			.Equals(FLinearColor(FColor(17, 149, 203, 255))));
	TestTrue(TEXT("edit mutation leaves inactive heading rendering unchanged"),
		EditStyle->HeadingForeground(false).Equals(FirstInactive));

	FWoTRuntimeClassDefaults ImplicitZeroDefaults;
	TestTrue(TEXT("implicit-zero inherited color fixture loads"),
		LoadBrowserDefaults(ImplicitZeroDefaults, false, Error,
			FColor::Black, FColor::Black, true));
	FWoTBrowserLookAndFeel ImplicitZeroLook;
	TestTrue(TEXT("implicit-zero inherited colors decode as zero"),
		FWoTRetailBrowserStyle::DecodeLookAndFeelForTesting(
			ImplicitZeroDefaults, ImplicitZeroLook, Error));
	TestTrue(TEXT("implicit-zero inactive heading is black"),
		ImplicitZeroLook.HeadingInActiveTitleColor.Equals(FLinearColor::Black));
	TestTrue(TEXT("implicit-zero edit text is black"),
		ImplicitZeroLook.EditBoxTextColor.Equals(FLinearColor::Black));

	FWoTRuntimeClassDefaults Malformed;
	TestTrue(TEXT("malformed fixture itself loads"),
		LoadBrowserDefaults(Malformed, true, Error));
	FWoTBrowserLookAndFeel Rejected;
	TestFalse(TEXT("missing decoded field fails closed"),
		FWoTRetailBrowserStyle::DecodeLookAndFeelForTesting(
			Malformed, Rejected, Error));
	TestTrue(TEXT("failure names the missing retail skin field"),
		Error.Contains(TEXT("title color")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTRetailBrowserContentSpec, "WoT.Menu.Style.RetailBrowserContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailBrowserContentSpec::RunTest(const FString&)
{
	FWoTMenuStyleTestWorld Fixture;
	if (!TestNotNull(TEXT("WoTTestWorld fixture"), Fixture.World))
	{
		return false;
	}

	const TCHAR* ChromeAssets[] = {
		TEXT("/Game/WoT/UI/UWindow/Misc.Misc"),
		TEXT("/Game/WoT/UI/UWindow/ActiveFrame.ActiveFrame"),
		TEXT("/Game/WoT/UI/UWindow/ActiveFrameS.ActiveFrameS"),
		TEXT("/Game/WoT/UI/UWindow/Highlight.Highlight"),
		TEXT("/Game/WoT/UI/UWindow/RaisedBevel.RaisedBevel"),
		TEXT("/Game/WoT/UI/UWindow/RecessedBevel.RecessedBevel"),
		TEXT("/Game/WoT/UI/UWindow/SunkenFrame.SunkenFrame"),
		TEXT("/Game/WoT/UI/UWindow/WindowFrame.WindowFrame"),
		TEXT("/Game/WoT/UI/UWindow/ScrollUp.ScrollUp"),
		TEXT("/Game/WoT/UI/UWindow/ScrollDown.ScrollDown"),
		TEXT("/Game/WoT/UI/UWindow/ScrollLeft.ScrollLeft"),
		TEXT("/Game/WoT/UI/UWindow/ScrollRight.ScrollRight"),
		TEXT("/Game/WoT/UI/UWindow/ScrollBackground.ScrollBackground")
	};
	for (const TCHAR* AssetPath : ChromeAssets)
	{
		TestNotNull(
			*FString::Printf(TEXT("retail chrome exists: %s"), AssetPath),
			LoadObject<UTexture2D>(nullptr, AssetPath));
	}

	const FWoTRetailBrowserStyle& Style = FWoTRetailBrowserStyle::Get();
	const struct
	{
		const TCHAR* Label;
		const FSlateFontInfo* Font;
		FName Face;
		int32 Size;
	} FontCases[] = {
		{TEXT("normal"), &Style.NormalFont(), FName(TEXT("Tahoma20")), 20},
		{TEXT("bold"), &Style.BoldFont(), FName(TEXT("TahomaB20")), 20},
		{TEXT("large"), &Style.LargeFont(), FName(TEXT("Tahoma30")), 30},
		{TEXT("large bold"), &Style.LargeBoldFont(), FName(TEXT("TahomaB30")), 30}
	};
	for (const auto& FontCase : FontCases)
	{
		const UObject* Face = FontCase.Font->FontObject.Get();
		TestNotNull(
			*FString::Printf(TEXT("%s GUIScale==2 face resolves"), FontCase.Label),
			Face);
		if (Face)
		{
			TestEqual(
				*FString::Printf(TEXT("%s GUIScale==2 face id"), FontCase.Label),
				Face->GetFName(), FontCase.Face);
		}
		TestEqual(
			*FString::Printf(TEXT("%s GUIScale==2 point size"), FontCase.Label),
			static_cast<int32>(FontCase.Font->Size), FontCase.Size);
	}

	TestTrue(TEXT("WoT browser client is solid black"),
		FWoTRetailBrowserStyle::ClientColor().Equals(FLinearColor::Black));
	TestTrue(TEXT("DrawCell inherits white from child paint"),
		FWoTRetailBrowserStyle::CellTextColor().Equals(FLinearColor::White));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTRetailBrowserSortSpec, "WoT.Menu.Style.RetailBrowserSort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailBrowserSortSpec::RunTest(const FString&)
{
	FWoTBrowserSortState Sort;
	TestTrue(TEXT("retail default sort column is Ping"),
		Sort.Column == EWoTBrowserSortColumn::Ping);
	TestFalse(TEXT("retail default Ping direction is ascending"),
		Sort.bDescending);
	TestTrue(TEXT("retail appStrcmp distinguishes case"),
		FWoTBrowserSortState::CompareText(
			TEXT("Alpha"), TEXT("alpha")) < 0);

	const EWoTBrowserSortColumn Columns[] = {
		EWoTBrowserSortColumn::Server,
		EWoTBrowserSortColumn::Ping,
		EWoTBrowserSortColumn::Map,
		EWoTBrowserSortColumn::Players
	};
	for (const EWoTBrowserSortColumn Column : Columns)
	{
		FWoTBrowserSortState Case;
		Case.Select(Column);
		TestTrue(TEXT("clicked column becomes active"), Case.Column == Column);
		const bool bFirstClickDescending =
			Column == EWoTBrowserSortColumn::Ping;
		TestEqual(TEXT("new column resets; active column reverses"),
			Case.bDescending, bFirstClickDescending);
		Case.Select(Column);
		TestEqual(TEXT("re-click reverses the current direction"),
			Case.bDescending, !bFirstClickDescending);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMultiplayerLocalizationSpec,
	"WoT.Menu.Style.AuthenticatedMultiplayerLocalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMultiplayerLocalizationSpec::RunTest(const FString&)
{
	// Catches the opening-menu-only projection dropping valid host labels.
	const ANSICHAR Input[] = "[menuMain]\nMenuList[2]=MMMMMMMMMMM\nMenuList[3]=CCCCCCCC\n"
		"MenuList[4]=HHHHHHHH\nMenuList[7]=QQQQ\n[menuStartArenaServer]\n"
		"MenuTitle=AAAAAAAAAAAA\nMenuList[1]=SSSSSSSSSS\nMenuList[2]=KKKKKKKKKKKKKKKKKKKKKK\n"
		"MenuList[3]=TTTTTTTTTT\nMenuList[5]=NNNNNNNNNNNNNNNNNN\nMenuList[7]=VVVVVVVVVVV\n"
		"[menuStartBattleServer]\nMenuTitle=BBBBBBBBBBBBBB\n"
		"MenuList[6]=WWWWWWWWWWWW\nMenuList[7]=BBBBBBBBBBB\n"
		"[menuMultiPlayer]\nHelpMessage[1]=JJJJJJJJJJJJJJJJJJJJ\n";
	TMap<FName, FString> Projected;
	TestTrue(TEXT("authenticated front-end labels parse"),
		FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(
			MakeArrayView(reinterpret_cast<const uint8*>(Input), sizeof(Input) - 1), Projected));
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("MenuTitle"), TEXT("AAAAAAAAAAAA")},
		{TEXT("MenuList[1]"), TEXT("SSSSSSSSSS")},
		{TEXT("MenuList[2]"), TEXT("KKKKKKKKKKKKKKKKKKKKKK")},
		{TEXT("MenuList[3]"), TEXT("TTTTTTTTTT")},
		{TEXT("MenuList[5]"), TEXT("NNNNNNNNNNNNNNNNNN")},
		{TEXT("MenuList[7]"), TEXT("VVVVVVVVVVV")}};
	for (const auto& Case : Cases)
		TestEqual(Case.Key, Projected.FindRef(FName(*(FString(TEXT("menuStartArenaServer.")) + Case.Key))),
			FString(Case.Value));
	TestEqual(TEXT("battle host title is authenticated retail text"),
		Projected.FindRef(FName(TEXT("menuStartBattleServer.MenuTitle"))),
		FString(TEXT("BBBBBBBBBBBBBB")));
	TestEqual(TEXT("battle win target is authenticated retail text"),
		Projected.FindRef(FName(TEXT("menuStartBattleServer.MenuList[6]"))),
		FString(TEXT("WWWWWWWWWWWW")));
	TestEqual(TEXT("battle budget is authenticated retail text"),
		Projected.FindRef(FName(TEXT("menuStartBattleServer.MenuList[7]"))),
		FString(TEXT("BBBBBBBBBBB")));
	TestEqual(TEXT("idle multiplayer status is authenticated"),
		Projected.FindRef(FName(TEXT("menuMultiPlayer.HelpMessage[1]"))),
		FString(TEXT("JJJJJJJJJJJJJJJJJJJJ")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMalformedBrowserLookFallbackSpec,
	"WoT.Menu.Style.MalformedBrowserLookFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMalformedBrowserLookFallbackSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	Fixture.bIncludeBrowserResources = true;
	Fixture.bMalformedBrowserLookAndFeel = true;
	FString Error;
	if (!TestTrue(TEXT("malformed authenticated look fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
	{
		AddError(Error);
		return false;
	}
	const FWoTFrontEndFontOwner* FontOwner =
		Fixture.Payload->FrontEndFontOwnerForScene();
	if (!TestNotNull(TEXT("authenticated browser font owner exists"), FontOwner))
		return false;
	FWoTFrontEndSceneMaterializer Scene;
	auto Staged = Scene.Stage(*Fixture.World, Fixture.Descriptor,
		*Fixture.WorldLease, *Fixture.Payload, *FontOwner, {}, []() { return false; });
	if (!TestFalse(TEXT("malformed-look opening scene stages"), Staged.IsError()))
		return false;
	if (!TestFalse(TEXT("malformed-look opening scene commits"),
		MoveTemp(Staged).GetValue().Commit().IsError())) return false;
	auto* Controller = Fixture.World->SpawnActor<AWoTFrontEndPlayerController>();
	auto Authority = FWoTFrontEndPresentationAuthority::Create(Controller);
	if (!TestTrue(TEXT("production presentation authority captures its lease"),
		Authority.IsValid() && Authority->IsReady())) return false;
	TestTrue(TEXT("malformed authenticated look falls back to an available browser"),
		Authority->PrepareBrowserResources());
	const FWoTRetailBrowserStyle* Browser = Authority->BrowserStyle();
	TestTrue(TEXT("fallback publishes the existing neutral game skin"),
		Browser && Browser->IsGameSkinForTesting());
	TestTrue(TEXT("successful neutral fallback exposes no unavailable message"),
		Authority->BrowserResourceError().IsEmpty());
	Scene.Shutdown();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOutOfBoundsBrowserLookFallbackSpec,
	"WoT.Menu.Style.OutOfBoundsBrowserLookFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTOutOfBoundsBrowserLookFallbackSpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	Fixture.bIncludeBrowserResources = true;
	FString Error;
	if (!TestTrue(TEXT("authenticated browser fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTRuntimeClassDefaults* Defaults =
		Fixture.Payload->RuntimeClassDefaults(Error);
	if (!TestNotNull(TEXT("authenticated look defaults load"), Defaults))
	{
		AddError(Error);
		return false;
	}
	const struct { const TCHAR* Member; int32 Value; } Mutations[] = {
		{TEXT("X"), 128}, {TEXT("Y"), 127},
		{TEXT("W"), 1}, {TEXT("H"), 1}};
	for (const auto& Mutation : Mutations)
	{
		TestTrue(FString(TEXT("authenticated BevelUpArea mutation: "))
			+ Mutation.Member,
			Defaults->SetClassStructIntegerForTesting(
				TEXT("UWindow.UWindowWin95LookAndFeel"),
				TEXT("BevelUpArea"), 0, Mutation.Member, Mutation.Value));
	}
	const FWoTFrontEndFontOwner* FontOwner =
		Fixture.Payload->FrontEndFontOwnerForScene();
	if (!TestNotNull(TEXT("authenticated browser font owner exists"), FontOwner))
		return false;
	FWoTFrontEndSceneMaterializer Scene;
	auto Staged = Scene.Stage(*Fixture.World, Fixture.Descriptor,
		*Fixture.WorldLease, *Fixture.Payload, *FontOwner, {}, []() { return false; });
	if (!TestFalse(TEXT("out-of-bounds-look opening scene stages"), Staged.IsError()))
		return false;
	if (!TestFalse(TEXT("out-of-bounds-look opening scene commits"),
		MoveTemp(Staged).GetValue().Commit().IsError())) return false;
	auto* Controller = Fixture.World->SpawnActor<AWoTFrontEndPlayerController>();
	auto Authority = FWoTFrontEndPresentationAuthority::Create(Controller);
	if (!TestTrue(TEXT("production presentation authority captures its lease"),
		Authority.IsValid() && Authority->IsReady())) return false;
	TestTrue(TEXT("out-of-bounds authenticated look keeps the browser available"),
		Authority->PrepareBrowserResources());
	const FWoTRetailBrowserStyle* Browser = Authority->BrowserStyle();
	TestTrue(TEXT("out-of-bounds look routes to the neutral skin without sampling"),
		Browser && Browser->IsGameSkinForTesting());
	TestTrue(TEXT("neutral fallback exposes no unavailable-browser message"),
		Authority->BrowserResourceError().IsEmpty());
	const struct { const TCHAR* Member; int32 Value; } Restores[] = {
		{TEXT("X"), 8}, {TEXT("Y"), 20},
		{TEXT("W"), 1}, {TEXT("H"), 1}};
	for (const auto& Restore : Restores)
	{
		TestTrue(FString(TEXT("authenticated BevelUpArea restore: "))
			+ Restore.Member,
			Defaults->SetClassStructIntegerForTesting(
				TEXT("UWindow.UWindowWin95LookAndFeel"),
				TEXT("BevelUpArea"), 0, Restore.Member, Restore.Value));
	}
	Scene.Shutdown();
	return !HasAnyErrors();
}

namespace
{
void CollectMultiplayerWidgets(const TSharedRef<SWidget>& Widget,
	TArray<TSharedRef<STextBlock>>& Texts, TArray<TSharedRef<SBorder>>& Borders)
{
	if (Widget->GetType() == FName(TEXT("STextBlock")))
		Texts.Add(StaticCastSharedRef<STextBlock>(Widget));
	if (Widget->GetType() == FName(TEXT("SBorder")) || Widget->GetType() == FName(TEXT("SButton")))
		Borders.Add(StaticCastSharedRef<SBorder>(Widget));
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		CollectMultiplayerWidgets(Children->GetChildAt(Index), Texts, Borders);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInactiveHeadingLiveWidgetSpec,
	"WoT.Menu.Style.InactiveHeadingRoutesThroughLiveWidget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInactiveHeadingLiveWidgetSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	const FColor InactiveColor(151, 31, 73);
	if (!TestTrue(TEXT("distinct browser heading colours decode"),
		LoadBrowserDefaults(Defaults, false, Error, InactiveColor)))
	{
		AddError(Error);
		return false;
	}
	FWoTBrowserLookAndFeel Look;
	if (!TestTrue(TEXT("the decoded look is accepted by the browser style"),
		FWoTRetailBrowserStyle::DecodeLookAndFeelForTesting(
			Defaults, Look, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("the fixture's active and inactive heading colours differ"),
		Look.HeadingActiveTitleColor.Equals(Look.HeadingInActiveTitleColor));

	FWoTMenuStyleTestWorld Fixture;
	AWoTFrontEndPlayerController* Controller =
		Fixture.World->SpawnActor<AWoTFrontEndPlayerController>();
	if (!TestNotNull(TEXT("front-end controller fixture"), Controller))
		return false;
	TSharedRef<FWoTMenuFeedbackRecorder> Feedback =
		MakeShared<FWoTMenuFeedbackRecorder>();
	TMap<FName, FString> Localization;
	Localization.Add(TEXT("heading.one"), TEXT("one"));
	Localization.Add(TEXT("heading.two"), TEXT("two"));
	Localization.Add(TEXT("heading.three"), TEXT("three"));
	Localization.Add(TEXT("heading.four"), TEXT("four"));
	TSharedRef<FWoTFrontEndPresentationAuthority> Authority =
		FWoTFrontEndPresentationAuthority::CreateForTesting(
			TEXT("inactive-heading-live-widget"),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
			MoveTemp(Localization), Feedback);
	Authority->SetBrowserLookAndFeelForTesting(Look);
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Controller)
		.PresentationAuthorityForTesting(Authority);
	TestTrue(TEXT("the real root accepts opening its Play screen"),
		Root->OpenPlayForTesting().IsEventHandled());
	Root->SlatePrepass(1.0f);
	TestEqual(TEXT("the real root opens its Play screen"),
		Root->CurrentScreenForTesting(), EWoTFrontEndScreen::Play);

	TArray<TSharedRef<STextBlock>> Texts;
	TArray<TSharedRef<SBorder>> Borders;
	CollectMultiplayerWidgets(Root, Texts, Borders);
	TMap<FString, TSharedPtr<STextBlock>> Headings;
	for (const TSharedRef<STextBlock>& Text : Texts)
	{
		const FString Label = Text->GetText().ToString();
		if (Label == TEXT("Server") || Label == TEXT("Ping")
			|| Label == TEXT("Map") || Label == TEXT("Players"))
		{
			Headings.Add(Label, Text);
		}
	}
	if (!TestEqual(TEXT("all four real join-panel headings are present"),
		Headings.Num(), 4))
	{
		return false;
	}
	auto HasColour = [&Headings](const TCHAR* Label, const FLinearColor& Expected)
	{
		const TSharedPtr<STextBlock>* Found = Headings.Find(Label);
		return Found && Found->IsValid()
			&& (*Found)->GetColorAndOpacity().GetSpecifiedColor().Equals(Expected);
	};
	TestTrue(TEXT("default sorted Ping heading uses the active colour"),
		HasColour(TEXT("Ping"), Look.HeadingActiveTitleColor));
	for (const TCHAR* Label : {TEXT("Server"), TEXT("Map"), TEXT("Players")})
	{
		TestTrue(FString(Label) + TEXT(" uses the inactive heading colour"),
			HasColour(Label, Look.HeadingInActiveTitleColor));
	}

	TestTrue(TEXT("the real Server heading sort handler accepts the selection"),
		Root->SelectBrowserSortColumnForTesting(EWoTBrowserSortColumn::Server)
			.IsEventHandled());
	Root->SlatePrepass(1.0f);
	TestTrue(TEXT("newly sorted Server heading swaps to the active colour"),
		HasColour(TEXT("Server"), Look.HeadingActiveTitleColor));
	TestTrue(TEXT("formerly sorted Ping heading swaps to the inactive colour"),
		HasColour(TEXT("Ping"), Look.HeadingInActiveTitleColor));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAuthenticatedMultiplayerResourcesSpec,
	"WoT.Menu.Style.AuthenticatedMultiplayerResources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTAuthenticatedMultiplayerResourcesSpec::RunTest(const FString&)
{
	// Exercise the real Play consumer so a disconnected materializer cannot pass.
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	Fixture.bIncludeBrowserResources = true;
	FString Error;
	if (!TestTrue(TEXT("authenticated fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
	{
		AddError(Error);
		return false;
	}
	const FWoTFrontEndFontOwner* FontOwner = Fixture.Payload->FrontEndFontOwnerForScene();
	if (!TestNotNull(TEXT("font owner exists"), FontOwner)) return false;
	TestNull(TEXT("browser font is not required during opening-scene startup"),
		FontOwner->ResolveResource(FName(TEXT("Tahoma20"))));
	FWoTFrontEndSceneMaterializer Scene;
	auto Staged = Scene.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *FontOwner, {}, []() { return false; });
	if (!TestFalse(TEXT("scene stages"), Staged.IsError())) return false;
	if (!TestFalse(TEXT("scene commits"), MoveTemp(Staged).GetValue().Commit().IsError())) return false;
	auto* Controller = Fixture.World->SpawnActor<AWoTFrontEndPlayerController>();
	auto Authority = FWoTFrontEndPresentationAuthority::Create(Controller);
	if (!TestTrue(TEXT("presentation captures lease"), Authority.IsValid() && Authority->IsReady())) return false;
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Controller).PresentationAuthorityForTesting(Authority);
	Root->ActivateMainMenuEntryForTesting(0);
	Root->SlatePrepass(1.0f);
	TestTrue(TEXT("real multiplayer screen opens"), Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	if (!Authority->BrowserStyle())
	{
		AddError(FString(TEXT("authenticated browser style failed: "))
			+ Authority->BrowserResourceError().ToString());
	}
	TArray<TSharedRef<STextBlock>> Texts;
	TArray<TSharedRef<SBorder>> Borders;
	CollectMultiplayerWidgets(Root, Texts, Borders);
	bool bServerLabel = false;
	bool bAuthenticatedBrowserFace = false;
	const auto NativeBrowser = Fixture.Payload->FrontEndFontOwnerForScene()
		->ResolveIntegerScaled(FName(TEXT("Tahoma20")), 1);
	if (!TestFalse(TEXT("browser has an authenticated native pixel face"), NativeBrowser.IsError()))
		return false;
	for (const TSharedRef<STextBlock>& Text : Texts)
	{
		const FString Label = Text->GetText().ToString();
		bServerLabel |= Label == TEXT("Server Name");
		if (Label == TEXT("Server"))
			TestTrue(TEXT("browser heading uses the dark skin's light game text, not Win95 black"),
				Text->GetColorAndOpacity().GetSpecifiedColor().GetLuminance() > 0.3f);
		TestFalse(TEXT("host controls never display localization identifiers"), Label.Contains(TEXT("menuStartArenaServer.")));
		if (Label != TEXT("Server")) continue;
		const FSlateFontInfo Info = Text->GetFont();
		const UObject* Font = Info.FontObject.Get();
		bAuthenticatedBrowserFace = Font && Font == NativeBrowser.GetValue().FontObject.Get()
			&& Font->GetOutermost() == GetTransientPackage()
			&& Info.FontFallback == EFontFallback::FF_NoFallback;
		if (bAuthenticatedBrowserFace)
		{
			FCharacterList& Characters = FSlateApplication::Get().GetRenderer()->GetFontCache()->GetCharacterList(Info, 1.0f);
			const FCharacterEntry& Character = Characters.GetCharacter(TEXT('A'), EFontFallback::FF_NoFallback);
			TestTrue(TEXT("browser text rasterizes its authenticated face"), Character.Valid && Character.GlyphIndex != 0);
		}
	}
	TestTrue(TEXT("host label is authenticated localized text"), bServerLabel);
	TestTrue(TEXT("browser uses transient authenticated Tahoma without engine fallback"), bAuthenticatedBrowserFace);
	const FWoTRetailBrowserStyle* GameStyle = Authority->BrowserStyle();
	if (!TestNotNull(TEXT("browser has a live presentation style"), GameStyle)) return false;
	// The live multiplayer menus wear the dark game skin, not the
	// retail Win95 frame (the retail decode below stays covered on its own).
	TestTrue(TEXT("live multiplayer browser uses the dark game skin"),
		GameStyle->IsGameSkinForTesting());
	TestNull(TEXT("live multiplayer browser frame is not a Win95 atlas texture"),
		GameStyle->WindowFrame()->GetResourceObject());
	// Reconstruct a second authenticated style to inspect the source pixels
	// independently of the live Slate widget tree.
	FString RetailError;
	const auto RetailStyle = FWoTRetailBrowserStyle::CreateAuthenticated(*Fixture.Payload,
		*Fixture.WorldLease, GameStyle->NormalFont(), RetailError);
	if (!TestTrue(TEXT("original authenticated retail skin remains available"), RetailStyle.IsValid()))
	{
		AddError(RetailError);
		return false;
	}
	const FLinearColor FaceTint =
		RetailStyle->FaceBackground()->TintColor.GetSpecifiedColor();
	const FLinearColor ExpectedFace(FColor(69, 137, 211, 255));
	TestTrue(TEXT("face brush carries its deliberately non-grey authenticated atlas pixel"),
		FaceTint.Equals(ExpectedFace));
	TestFalse(TEXT("face fixture is not the transcribed 192 grey"),
		FMath::IsNearlyEqual(FaceTint.R, 192.0f / 255.0f)
			&& FMath::IsNearlyEqual(FaceTint.G, 192.0f / 255.0f)
			&& FMath::IsNearlyEqual(FaceTint.B, 192.0f / 255.0f));
	const FSlateBrush* Raised = nullptr;
	bool bFrameBorderMatchesTitle = false;
	for (const FSlateBrush* Brush : {RetailStyle->WindowFrame(), RetailStyle->RaisedBevel()})
	{
		UTexture2D* Texture = Brush ? Cast<UTexture2D>(Brush->GetResourceObject()) : nullptr;
		if (Texture && Texture->HasAnyFlags(RF_Transient) && Brush->Margin.Top > 0.45f)
		{
			// Slate's nine-slice batcher uses actual texture dimensions, not
			// brush ImageSize, to set its fixed border geometry.
			bFrameBorderMatchesTitle = FMath::IsNearlyEqual(
				Texture->GetSizeY() * Brush->Margin.Top, 32.0f);
			TestTrue(TEXT("rendered frame top border fills the 32-unit title bar"), bFrameBorderMatchesTitle);
		}
		if (Texture && Texture->HasAnyFlags(RF_Transient)
			&& (Texture->GetSizeX() == 5 || Texture->GetSizeX() == 10)
			&& Texture->GetSizeX() == Texture->GetSizeY())
		{
			Raised = Brush;
			TestEqual(TEXT("GUIScale two expands actual bevel width"), Texture->GetSizeX(), 10);
			TestEqual(TEXT("GUIScale two expands actual bevel height"), Texture->GetSizeY(), 10);
			FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
			const FColor* Pixels = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
			if (TestNotNull(TEXT("raised bevel has authenticated pixels"), Pixels))
			{
				TestEqual(TEXT("raised corner samples Misc(4,16)"), Pixels[0].R, static_cast<uint8>(53));
				TestEqual(TEXT("expanded raised center samples Misc(8,20)"),
					Pixels[4 * Texture->GetSizeX() + 4].R, static_cast<uint8>(69));
			}
			Mip.BulkData.Unlock();
		}
	}
	TestNotNull(TEXT("retail reconstruction preserves the transient stitched bevel"), Raised);
	TestTrue(TEXT("browser frame retains a title-sized fixed border"), bFrameBorderMatchesTitle);
	Scene.RevokeAuthorityForTesting();
	TestTrue(TEXT("lease revocation collapses multiplayer synchronously"), Root->GetVisibility() == EVisibility::Collapsed);
	TestTrue(TEXT("revocation removes multiplayer localization"), Authority->Text(FName(TEXT("menuStartArenaServer.MenuTitle"))).IsEmpty());
	RetailStyle->ReleaseAuthenticatedResources();
	if (Raised) TestNull(TEXT("revocation unbinds retained browser brushes"), Raised->GetResourceObject());
	Scene.Shutdown();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUnavailableMultiplayerResourcesSpec,
	"WoT.Menu.Style.UnavailableMultiplayerResources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTUnavailableMultiplayerResourcesSpec::RunTest(const FString&)
{
	// A valid opening scene deliberately lacks the optional browser font.
	// Both direct navigation and a live connection failure must remain escapable.
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("opening-only fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error)))
	{
		AddError(Error);
		return false;
	}
	const FWoTFrontEndFontOwner* FontOwner = Fixture.Payload->FrontEndFontOwnerForScene();
	if (!TestNotNull(TEXT("opening font owner exists"), FontOwner)) return false;
	FWoTFrontEndSceneMaterializer Scene;
	auto Staged = Scene.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *FontOwner, {}, []() { return false; });
	if (!TestFalse(TEXT("opening scene stages"), Staged.IsError())) return false;
	if (!TestFalse(TEXT("opening scene commits"), MoveTemp(Staged).GetValue().Commit().IsError())) return false;
	auto* Controller = Fixture.World->SpawnActor<AWoTFrontEndPlayerController>();
	auto Authority = FWoTFrontEndPresentationAuthority::Create(Controller);
	if (!TestTrue(TEXT("opening presentation remains ready"), Authority.IsValid() && Authority->IsReady())) return false;
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Controller).PresentationAuthorityForTesting(Authority);
	auto CheckRecovery = [this, &Root, &Authority](const TCHAR* Context)
	{
		// Bound Slate font attributes are evaluated during the real layout
		// prepass; querying the freshly constructed text reads its default.
		Root->SlatePrepass(1.0f);
		TestTrue(FString(Context) + TEXT(" stays in Multiplayer"),
			Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
		TArray<TSharedRef<STextBlock>> Texts;
		TArray<TSharedRef<SBorder>> Borders;
		CollectMultiplayerWidgets(Root, Texts, Borders);
		bool bError = false, bBack = false, bUnavailableBrowser = false;
		for (const TSharedRef<STextBlock>& Text : Texts)
		{
			const FString Label = Text->GetText().ToString();
			bBack |= Label == TEXT("Back");
			bUnavailableBrowser |= Label == TEXT("Refresh Sessions");
			if (Label != TEXT("Multiplayer resources are unavailable.")) continue;
			bError = true;
			// The Steam Deck legibility pass (SP3 PR 7): the failure lines draw
			// the authenticated body face at its 2x strike (LargeBodyFont, 28
			// physical pixels), as every other multiplayer screen does; they
			// were the only 14-pixel text there.
			TestTrue(TEXT("recovery uses the authenticated menu font's 2x strike"),
				Text->GetFont().FontObject == Authority->MultiplayerFont(
					FName(TEXT("F_WOTReg14")), 1.0f, 2).FontObject
				&& Text->GetFont().FontFallback == EFontFallback::FF_NoFallback);
		}
		TestTrue(FString(Context) + TEXT(" shows the resource failure"), bError);
		TestTrue(FString(Context) + TEXT(" exposes Back"), bBack);
		TestFalse(FString(Context) + TEXT(" does not publish the unavailable browser"), bUnavailableBrowser);
		const FKeyEvent Escape(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0);
		TestTrue(FString(Context) + TEXT(" handles Escape"), Root->OnPreviewKeyDown(FGeometry(), Escape).IsEventHandled());
		TestTrue(FString(Context) + TEXT(" returns to Main"), Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);
	};
	Root->ActivateMainMenuEntryForTesting(0);
	CheckRecovery(TEXT("direct activation"));
	UWoTSessionSubsystem* Sessions = Fixture.World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	if (TestNotNull(TEXT("real session event source exists"), Sessions))
	{
		Sessions->OnConnectionFailure().Broadcast(TEXT("synthetic connection failure"));
		CheckRecovery(TEXT("connection-failure reentry"));
	}
	Scene.Shutdown();
	return !HasAnyErrors();
}

#endif
