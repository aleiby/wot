#include "WoTUIScreensTestFixture.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTFrontEnd.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTInGameSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTPlayerController.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Online/SWoTSharingNotice.h"
#include "Engine/UserInterfaceSettings.h"
#include "Engine/World.h"
#include "Layout/Geometry.h"
#include "Styling/CoreStyle.h"

namespace WoTUIScreensTest
{
namespace
{
class FWoTUIScreensFeedback final : public IWoTFrontEndFeedbackSink
{
public:
	virtual bool IsReady() const override { return true; }
	virtual void MoveSelector() override {}
	virtual void Select() override {}
};

/** The front end's menus build their entries from these labels (the UI
 * specs' own set, placeholder text). */
TSharedRef<FWoTFrontEndPresentationAuthority> WoTUIScreensAuthority()
{
	TMap<FName, FString> Labels;
	const TPair<const TCHAR*, int32> Entries[] = {
		{TEXT("menuMain.MenuList[2]"), 11}, {TEXT("menuMain.MenuList[3]"), 8},
		{TEXT("menuMain.MenuList[4]"), 8}, {TEXT("menuMain.MenuList[7]"), 4},
		{TEXT("menuMultiPlayer.HelpMessage[1]"), 20}, {TEXT("menuMultiPlayer.MenuList[2]"), 13},
		{TEXT("menuStartArenaServer.MenuTitle"), 12}, {TEXT("menuStartBattleServer.MenuTitle"), 14},
		{TEXT("menuStartArenaServer.MenuList[1]"), 10}, {TEXT("menuStartArenaServer.MenuList[2]"), 12},
		{TEXT("menuStartArenaServer.MenuList[3]"), 10}, {TEXT("menuStartArenaServer.MenuList[5]"), 11},
		{TEXT("menuStartArenaServer.MenuList[7]"), 11}, {TEXT("menuPlayer.MenuTitle"), 13},
		{TEXT("menuPlayer.MenuList[1]"), 5}, {TEXT("menuPlayer.MenuList[3]"), 9},
		{TEXT("menuOptions.MenuTitle"), 8}, {TEXT("menuOptions.MenuList[1]"), 18},
		{TEXT("menuOptions.MenuList[2]"), 12}, {TEXT("menuOptions.MenuList[3]"), 17},
		{TEXT("menuConfiguration.MenuTitle"), 8}};
	for (const TPair<const TCHAR*, int32>& Entry : Entries)
	{
		Labels.Add(FName(Entry.Key), FString::ChrN(Entry.Value, TEXT('M')));
	}
	return FWoTFrontEndPresentationAuthority::CreateForTesting(TEXT("ui-screens"),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
		MoveTemp(Labels), MakeShared<FWoTUIScreensFeedback>());
}

TSharedRef<FWoTRequiredMenuFonts> WoTUIScreensFonts()
{
	static const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Required = MakeShared<FWoTRequiredMenuFonts>();
	Required->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Required->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Required->Generation = Generation;
	return Required;
}

SWoTFrontEndRoot& WoTUIScreensRoot(const TSharedRef<SWidget>& FrontEnd)
{
	return static_cast<SWoTFrontEndRoot&>(FrontEnd.Get());
}
}

TSharedRef<SWidget> MakeFrontEnd()
{
	return SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(WoTUIScreensAuthority());
}

void SettleFrontEnd(const TSharedRef<SWidget>& FrontEnd, const FVector2D& Size,
	TFunctionRef<void()> Paint)
{
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		FrontEnd->Tick(Pass == 0 ? FGeometry::MakeRoot(Size, FSlateLayoutTransform())
			: FrontEnd->GetCachedGeometry(), 0.0, 0.0f);
		Paint();
	}
}

void OpenServerBrowser(const TSharedRef<SWidget>& FrontEnd)
{
	WoTUIScreensRoot(FrontEnd).ActivateMainMenuEntryForTesting(0);
}

void OpenPlayerSetup(const TSharedRef<SWidget>& FrontEnd)
{
	WoTUIScreensRoot(FrontEnd).OpenPlayerConfigForTesting();
}

TSharedPtr<SWidget> AddressField(const TSharedRef<SWidget>& FrontEnd)
{
	return WoTUIScreensRoot(FrontEnd).DirectAddressTextBoxForTesting();
}

TSharedPtr<SWidget> GamePasswordField(const TSharedRef<SWidget>& FrontEnd)
{
	return WoTUIScreensRoot(FrontEnd).GamePasswordTextBoxForTesting();
}

TSharedPtr<SWidget> PlayerNameField(const TSharedRef<SWidget>& FrontEnd)
{
	return WoTUIScreensRoot(FrontEnd).PlayerNameTextBoxForTesting();
}

FString PendingPlayerName(const TSharedRef<SWidget>& FrontEnd)
{
	return WoTUIScreensRoot(FrontEnd).ViewModelForTesting().GetPendingPlayerName();
}

TSharedRef<SWidget> MakeInGameMenu(APlayerController& Controller)
{
	return SNew(SWoTInGameMenuOverlay)
		.PlayerController(Cast<AWoTPlayerController>(&Controller))
		.RequiredFontsForTesting(WoTUIScreensFonts());
}

TSharedPtr<SWidget> InGameMenuFocus(const TSharedRef<SWidget>& Menu)
{
	return static_cast<SWoTInGameMenuOverlay&>(Menu.Get()).InitialFocusWidget();
}

TSharedRef<SWidget> MakeItemInfo(APlayerController& Controller)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTAngrealInvFireball* Item = Controller.GetWorld()
		? Controller.GetWorld()->SpawnActor<AWoTAngrealInvFireball>(Params) : nullptr;
	return SNew(SWoTItemInfoOverlay)
		.PlayerController(Cast<AWoTPlayerController>(&Controller))
		.Item(Item)
		.RequiredFontsForTesting(WoTUIScreensFonts());
}

TSharedRef<SWidget> MakeHelp(const FVector2D& PhysicalSize, TFunction<void()> OnClose)
{
	FWoTHelpContent Content;
	Content.Title = TEXT("Help");
	Content.SubTitle = TEXT("Press a key");
	Content.Content.Add(TEXT("Line"));
	// As AWoTPlayerController::CreateHelpWidget sizes it.
	const float DpiScale = GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(
		FIntPoint(FMath::TruncToInt32(PhysicalSize.X), FMath::TruncToInt32(PhysicalSize.Y)));
	return SNew(SWoTHelpOverlay)
		.Content(Content)
		.InitialPhysicalSize(PhysicalSize)
		.InitialDpiScale(DpiScale)
		.OnClose(FSimpleDelegate::CreateLambda([OnClose = MoveTemp(OnClose)]()
		{
			if (OnClose)
			{
				OnClose();
			}
		}))
		.RequiredFontsForTesting(WoTUIScreensFonts());
}

int32 HelpPixelScale(const TSharedRef<SWidget>& Help)
{
	return static_cast<SWoTHelpOverlay&>(Help.Get()).PixelScaleForTesting();
}

TSharedRef<SWidget> MakeNotice()
{
	return SNew(SWoTSharingNotice);
}

TSharedPtr<SWidget> OpenControllerChat(APlayerController& Controller)
{
	AWoTPlayerController* Gameplay = Cast<AWoTPlayerController>(&Controller);
	if (!Gameplay)
	{
		return nullptr;
	}
	Gameplay->EnableChatWidgetForTesting();
	Gameplay->ChatByController(/*bTeam*/ false);
	return Gameplay->GetChatInputWidget();
}

void CloseChat(APlayerController& Controller)
{
	if (AWoTPlayerController* Gameplay = Cast<AWoTPlayerController>(&Controller))
	{
		if (Gameplay->IsChatTyping())
		{
			Gameplay->CancelChatInput();
		}
	}
}

float ChatDpiScale(const TSharedRef<SWidget>& Chat)
{
	return static_cast<SWoTChatInputOverlay&>(Chat.Get()).DpiScaleForTesting();
}
}

#endif // WITH_AUTOMATION_TESTS
