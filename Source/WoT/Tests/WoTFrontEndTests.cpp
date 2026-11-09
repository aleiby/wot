// Automation specs for the nonvisual multiplayer front-end foundation.
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "../World/WoTWorldMaterializer.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTFrontEnd.h"
#include "../FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../Payload/WoTFrontEndFontOwner.h"
#include "../WoTMenuStyle.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTFrontEndSlate.h"
#include "../Online/WoTLocalNetworkAccess.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTFrontEndCatalog.h"
#include "../WoTFrontEndViewModel.h"
#include "../WoTGameInstance.h"
#include "../WoTLocalPlayer.h"
#include "../WoTMacComputerName.h"
#include "HAL/PlatformProcess.h"
#include "../WoTBootstrapSubsystem.h"
#include "../WoTInGameMenu.h"
#include "../WoTInGameSlate.h"
#include "../WoTPlayerController.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameMode.h"
#include "../WoTBattleGameMode.h"
#include "../WoTRetailText.h"
#include "../WoTBattleInfo.h"
#include "../WoTSessionSubsystem.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../Settings/WoTSettingsSlate.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../UI/SWoTListCombo.h"
#include "../UI/WoTControllerUIInput.h"
#include "WoTFrontEndTestData.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "Async/TaskGraphInterfaces.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Input/HittestGrid.h"
#include "InputCoreTypes.h"
#include "Layout/ArrangedChildren.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "Styling/CoreStyle.h"
#include "Rendering/SlateRenderer.h"
#include "Rendering/DrawElements.h"
#include "Types/PaintArgs.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	UWorld* WoTFrontEndTests_CreateWorld()
	{
		ResetWoTPayloadActivationForTesting();
		if (PublishWoTPayloadBootstrapActivationForTesting().IsError()
			|| PublishWoTBootstrapWorldReadyForTesting().IsError())
		{
			ResetWoTPayloadActivationForTesting();
			return nullptr;
		}
		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		UWorld* World = WoTTest::CreateSpecWorld(Options);
		if (World == nullptr)
		{
			ResetWoTPayloadActivationForTesting();
		}
		return World;
	}

	void WoTFrontEndTests_DestroyWorld(UWorld*& World)
	{
		if (!World)
		{
			return;
		}

		WoTTest::DestroySpecWorld(World);
		World = nullptr;
		ResetWoTPayloadActivationForTesting();
	}

	struct FWoTFrontEndTests_SessionState
	{
		UWorld* World = nullptr;
		UWoTGameInstance* GameInstance = nullptr;
		UWoTSessionSubsystem* Sessions = nullptr;
		int32 HostCompletionCount = 0;
		bool bHostSucceeded = false;
		int32 SearchCompletionCount = 0;
		bool bSearchSucceeded = false;
		TArray<FWoTSessionListEntry> SearchResults;
		double Deadline = 0.0;
	};

	class FWoTFrontEndPresentationFeedbackRecorder final
		: public IWoTFrontEndFeedbackSink
	{
	public:
		virtual bool IsReady() const override { return bReady; }
		virtual void MoveSelector() override { Events.Add(TEXT("MoveSelector")); }
		virtual void Select() override { Events.Add(TEXT("Select")); }

		TArray<FString> Events;
		bool bReady = true;
	};

	void CollectWidgetsOfType(const TSharedRef<SWidget>& Widget, FName Type,
		TArray<TSharedRef<SWidget>>& Out)
	{
		if (Widget->GetType() == Type) Out.Add(Widget);
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				CollectWidgetsOfType(Children->GetChildAt(Index), Type, Out);
			}
		}
	}

	void CollectArrangedGeometryOfType(const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry, FName Type, TArray<FGeometry>& Out)
	{
		if (Widget->GetType() == Type) Out.Add(Geometry);
		FArrangedChildren Children(EVisibility::Visible);
		Widget->ArrangeChildren(Geometry, Children);
		for (const FArrangedWidget& Child : Children.GetInternalArray())
			CollectArrangedGeometryOfType(Child.Widget, Child.Geometry, Type, Out);
	}

	FString FrontEndFixtureLabel(int32 Length, TCHAR Character)
	{
		return FString::ChrN(Length, Character);
	}

	TSharedRef<FWoTFrontEndPresentationAuthority>
	MakeInjectedPresentationAuthority(
		const TSharedRef<FWoTFrontEndPresentationFeedbackRecorder>& Feedback,
		FString SnapshotIdentity = TEXT("generation-input"), int32 BodyFontSize = 14)
	{
		TMap<FName, FString> Labels;
		Labels.Add(FName(TEXT("menuMain.MenuList[2]")), FrontEndFixtureLabel(11, TEXT('M')));
		Labels.Add(FName(TEXT("menuMain.MenuList[3]")), FrontEndFixtureLabel(8, TEXT('C')));
		Labels.Add(FName(TEXT("menuMain.MenuList[4]")), FrontEndFixtureLabel(8, TEXT('H')));
		Labels.Add(FName(TEXT("menuMain.MenuList[7]")), FrontEndFixtureLabel(4, TEXT('Q')));
		Labels.Add(FName(TEXT("menuMultiPlayer.HelpMessage[1]")), FrontEndFixtureLabel(26, TEXT('J')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuTitle")), FrontEndFixtureLabel(12, TEXT('A')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuTitle")), FrontEndFixtureLabel(14, TEXT('B')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[1]")), FrontEndFixtureLabel(10, TEXT('S')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[2]")), FrontEndFixtureLabel(22, TEXT('K')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[3]")), FrontEndFixtureLabel(10, TEXT('T')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[4]")), FrontEndFixtureLabel(12, TEXT('C')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[5]")), FrontEndFixtureLabel(18, TEXT('N')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[6]")), FrontEndFixtureLabel(20, TEXT('X')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[7]")), FrontEndFixtureLabel(11, TEXT('V')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[8]")), FrontEndFixtureLabel(14, TEXT('A')));
		Labels.Add(FName(TEXT("menuStartArenaServer.MenuList[9]")), FrontEndFixtureLabel(13, TEXT('G')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[4]")), FrontEndFixtureLabel(13, TEXT('L')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[6]")), FrontEndFixtureLabel(12, TEXT('W')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[7]")), FrontEndFixtureLabel(11, TEXT('B')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[8]")), FrontEndFixtureLabel(19, TEXT('U')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[9]")), FrontEndFixtureLabel(18, TEXT('P')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[10]")), FrontEndFixtureLabel(17, TEXT('Z')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[11]")), FrontEndFixtureLabel(20, TEXT('Y')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[13]")), FrontEndFixtureLabel(14, TEXT('D')));
		Labels.Add(FName(TEXT("menuStartBattleServer.MenuList[14]")), FrontEndFixtureLabel(13, TEXT('J')));
		Labels.Add(FName(TEXT("menuMultiPlayer.MenuList[2]")), FrontEndFixtureLabel(13, TEXT('P')));
		Labels.Add(FName(TEXT("menuPlayer.MenuTitle")), FrontEndFixtureLabel(13, TEXT('R')));
		Labels.Add(FName(TEXT("menuPlayer.MenuList[1]")), FrontEndFixtureLabel(5, TEXT('N')));
		Labels.Add(FName(TEXT("menuPlayer.MenuList[3]")), FrontEndFixtureLabel(9, TEXT('S')));
		Labels.Add(FName(TEXT("menuPlayer.HelpMessage[1]")), FrontEndFixtureLabel(84, TEXT('I')));
		Labels.Add(FName(TEXT("menuPlayer.HelpMessage[3]")), FrontEndFixtureLabel(31, TEXT('K')));
		Labels.Add(FName(TEXT("menuOptions.MenuTitle")), FrontEndFixtureLabel(8, TEXT('O')));
		Labels.Add(FName(TEXT("menuOptions.MenuList[1]")), FrontEndFixtureLabel(18, TEXT('U')));
		Labels.Add(FName(TEXT("menuOptions.MenuList[2]")), FrontEndFixtureLabel(12, TEXT('I')));
		Labels.Add(FName(TEXT("menuOptions.MenuList[3]")), FrontEndFixtureLabel(17, TEXT('E')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[1]")), FrontEndFixtureLabel(4, TEXT('F')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[2]")), FrontEndFixtureLabel(7, TEXT('J')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[3]")), FrontEndFixtureLabel(12, TEXT('W')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[4]")), FrontEndFixtureLabel(13, TEXT('X')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[5]")), FrontEndFixtureLabel(11, TEXT('L')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[6]")), FrontEndFixtureLabel(12, TEXT('R')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[7]")), FrontEndFixtureLabel(9, TEXT('L')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[8]")), FrontEndFixtureLabel(10, TEXT('R')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[9]")), FrontEndFixtureLabel(9, TEXT('N')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[10]")), FrontEndFixtureLabel(13, TEXT('P')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[11]")), FrontEndFixtureLabel(9, TEXT('D')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[13]")), FrontEndFixtureLabel(7, TEXT('U')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[14]")), FrontEndFixtureLabel(9, TEXT('W')));
		Labels.Add(FName(TEXT("menuKeyboard.MenuList[18]")), FrontEndFixtureLabel(17, TEXT('Z')));
		Labels.Add(FName(TEXT("menuConfiguration.MenuTitle")), FrontEndFixtureLabel(8, TEXT('G')));
		Labels.Add(FName(TEXT("menuConfiguration.MenuList[2]")), FrontEndFixtureLabel(22, TEXT('Y')));
		Labels.Add(FName(TEXT("menuConfiguration.MenuList[3]")), FrontEndFixtureLabel(17, TEXT('R')));
		Labels.Add(FName(TEXT("menuConfiguration.MenuList[5]")), FrontEndFixtureLabel(5, TEXT('V')));
		Labels.Add(FName(TEXT("menuConfiguration.MenuList[6]")), FrontEndFixtureLabel(21, TEXT('D')));
		Labels.Add(FName(TEXT("menuConfiguration.MenuList[15]")), FrontEndFixtureLabel(13, TEXT('A')));
		return FWoTFrontEndPresentationAuthority::CreateForTesting(
			MoveTemp(SnapshotIdentity),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), BodyFontSize),
			MoveTemp(Labels), Feedback);
	}

	FKeyEvent MakeFrontEndKeyEvent(FKey Key)
	{
		return FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0);
	}

	bool ClickWidgetThroughSlate(const TSharedRef<SWidget>& Widget)
	{
		FSlateApplication& Slate = FSlateApplication::Get();
		FWidgetPath WidgetPath;
		if (!Slate.GeneratePathToWidgetUnchecked(Widget, WidgetPath)) return false;
		const FGeometry Geometry = Widget->GetCachedGeometry();
		const FVector2D Position = Geometry.LocalToAbsolute(
			Geometry.GetLocalSize() * 0.5f);
		const TSet<FKey> NoButtons;
		TSet<FKey> LeftDown;
		LeftDown.Add(EKeys::LeftMouseButton);
		const FPointerEvent Down(0, Position, Position, LeftDown,
			EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
		const FPointerEvent Up(0, Position, Position, NoButtons,
			EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
		const FReply DownReply = Widget->OnMouseButtonDown(Geometry, Down);
		Slate.ProcessReply(WidgetPath, DownReply, &WidgetPath, &Down, 0);
		const FReply UpReply = Widget->OnMouseButtonUp(Geometry, Up);
		Slate.ReleaseAllPointerCapture(0);
		return DownReply.IsEventHandled() && UpReply.IsEventHandled();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndFactionConfigSpec,
	"WoT.FrontEnd.FactionConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndFactionConfigSpec::RunTest(const FString&)
{
	const TArray<FWoTFactionChoice>& Choices = FWoTFrontEndModel::FactionChoices();
	TestEqual(TEXT("four retail faction choices"), Choices.Num(), 4);
	if (Choices.Num() == 4)
	{
		const EWoTPlayerFaction ExpectedFactions[] = {
			EWoTPlayerFaction::AesSedai,
			EWoTPlayerFaction::Forsaken,
			EWoTPlayerFaction::Hound,
			EWoTPlayerFaction::Whitecloak
		};
		const TCHAR* ExpectedClasses[] = {
			TEXT("WOTPawns.AesSedai"),
			TEXT("WOTPawns.Forsaken"),
			TEXT("WOTPawns.Hound"),
			TEXT("WOTPawns.Whitecloak")
		};
		const TCHAR* ExpectedDescriptions[] = {
			TEXT("Aes Sedai"), TEXT("Forsaken"), TEXT("Hound"), TEXT("Whitecloak")
		};

		for (int32 Index = 0; Index < Choices.Num(); ++Index)
		{
			TestEqual(FString::Printf(TEXT("choice %d order"), Index),
				static_cast<int32>(Choices[Index].Faction),
				static_cast<int32>(ExpectedFactions[Index]));
			TestEqual(FString::Printf(TEXT("choice %d class"), Index),
				Choices[Index].ClassOption, FString(ExpectedClasses[Index]));
			TestEqual(FString::Printf(TEXT("choice %d description"), Index),
				Choices[Index].TeamDescription, FString(ExpectedDescriptions[Index]));
		}
	}

	const EWoTPlayerFaction InvalidFaction = static_cast<EWoTPlayerFaction>(255);
	TestEqual(TEXT("invalid faction normalizes to Aes Sedai"),
		static_cast<int32>(FWoTFrontEndModel::NormalizeFaction(InvalidFaction)),
		static_cast<int32>(EWoTPlayerFaction::AesSedai));
	TestEqual(TEXT("invalid faction class uses the retail default"),
		FWoTFrontEndModel::ClassOption(InvalidFaction), FString(TEXT("WOTPawns.AesSedai")));

	UWoTGameInstance* Saved = NewObject<UWoTGameInstance>();
	Saved->SetSelectedFaction(EWoTPlayerFaction::Hound, /*bSave*/ false);
	TestEqual(TEXT("GameInstance exposes the chosen class"), Saved->GetSelectedClassOption(),
		FString(TEXT("WOTPawns.Hound")));
	Saved->SetSelectedFaction(InvalidFaction, /*bSave*/ false);
	TestEqual(TEXT("GameInstance normalizes an invalid choice"),
		static_cast<int32>(Saved->GetSelectedFaction()),
		static_cast<int32>(EWoTPlayerFaction::AesSedai));

	TestNull(TEXT("selected faction is no longer a reflected config property"),
		UWoTGameInstance::StaticClass()->FindPropertyByName(
			FName(TEXT("SelectedFaction"))));
	TestNull(TEXT("payload cache path is no longer a reflected config property"),
		UWoTGameInstance::StaticClass()->FindPropertyByName(
			FName(TEXT("PayloadCacheDir"))));
	Saved->SetSelectedFaction(EWoTPlayerFaction::Whitecloak, /*bSave*/ false);
	TestEqual(TEXT("typed runtime selection maps to the package-qualified class"),
		Saved->GetSelectedClassOption(), FString(TEXT("WOTPawns.Whitecloak")));
	Saved->SetSelectedSkinColor(EWoTPlayerSkinColor::Green, /*bSave*/ false);
	TestEqual(TEXT("GameInstance retains the typed runtime skin color"),
		static_cast<int32>(Saved->GetSelectedSkinColor()),
		static_cast<int32>(EWoTPlayerSkinColor::Green));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityP1CPlayerConfigSkinSpec,
	"WoT.Parity.P1C.PlayerConfig.SkinsAndURL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTParityP1CPlayerConfigSkinSpec::RunTest(const FString&)
{
	const TArray<FWoTSkinChoice>& Choices = FWoTFrontEndModel::SkinChoices();
	TestEqual(TEXT("the retail menu exposes five skin colors"), Choices.Num(), 5);
	if (Choices.Num() == 5)
	{
		const EWoTPlayerSkinColor ExpectedColors[] = {
			EWoTPlayerSkinColor::Black, EWoTPlayerSkinColor::Blue,
			EWoTPlayerSkinColor::Green, EWoTPlayerSkinColor::Red,
			EWoTPlayerSkinColor::Base};
		const TCHAR* ExpectedNames[] = {
			TEXT("Black"), TEXT("Blue"), TEXT("Green"), TEXT("Red"),
			TEXT("Base")};
		for (int32 Index = 0; Index < Choices.Num(); ++Index)
		{
			TestEqual(FString::Printf(TEXT("skin %d order"), Index),
				static_cast<int32>(Choices[Index].Color),
				static_cast<int32>(ExpectedColors[Index]));
			TestEqual(FString::Printf(TEXT("skin %d label"), Index),
				Choices[Index].DisplayName, FString(ExpectedNames[Index]));
		}
	}

	TestEqual(TEXT("Aes Sedai blue skin is package-qualified"),
		FWoTFrontEndModel::SkinOption(EWoTPlayerFaction::AesSedai,
			EWoTPlayerSkinColor::Blue), FString(TEXT("AesSedaiSkins.Aes_Blue")));
	TestEqual(TEXT("Forsaken red skin is package-qualified"),
		FWoTFrontEndModel::SkinOption(EWoTPlayerFaction::Forsaken,
			EWoTPlayerSkinColor::Red), FString(TEXT("ForsakenSkins.For_Red")));
	TestEqual(TEXT("Hound green skin is package-qualified"),
		FWoTFrontEndModel::SkinOption(EWoTPlayerFaction::Hound,
			EWoTPlayerSkinColor::Green), FString(TEXT("HoundSkins.Hou_Green")));
	TestEqual(TEXT("Whitecloak black skin is package-qualified"),
		FWoTFrontEndModel::SkinOption(EWoTPlayerFaction::Whitecloak,
			EWoTPlayerSkinColor::Black), FString(TEXT("WhitecloakSkins.Whi_Black")));
	TestTrue(TEXT("a closed-list skin is valid"),
		FWoTFrontEndModel::IsValidSkinOption(TEXT("AesSedaiSkins.Aes_Red")));
	TestFalse(TEXT("a foreign object path is not a personal skin"),
		FWoTFrontEndModel::IsValidSkinOption(TEXT("Engine.Engine")));
	TestFalse(TEXT("URL option injection cannot ride in a skin"),
		FWoTFrontEndModel::IsValidSkinOption(
			TEXT("AesSedaiSkins.Aes_Red?Class=WOTPawns.Forsaken")));

	TestEqual(TEXT("join URL replaces hostile class and skin options"),
		FWoTFrontEndModel::AppendPlayerOptions(
			TEXT("host?Name=Lan?skin=Bad?CLASS=Old#Start"),
			EWoTPlayerFaction::Whitecloak, EWoTPlayerSkinColor::Red),
		FString(TEXT("host?Name=Lan?Class=WOTPawns.Whitecloak"
			"?Skin=WhitecloakSkins.Whi_Red#Start")));

	FWoTHostRequest Host = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Arena_01"));
	TestTrue(TEXT("host URL carries the selected class and skin"),
		FWoTFrontEndModel::BuildHostTravelURL(Host,
			EWoTPlayerFaction::Hound, EWoTPlayerSkinColor::Blue).EndsWith(
				TEXT("?Class=WOTPawns.Hound?Skin=HoundSkins.Hou_Blue")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityP1CPlayerConfigFrontEndSpec,
	"WoT.Parity.P1C.PlayerConfig.FrontEndSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTParityP1CPlayerConfigFrontEndSpec::RunTest(const FString&)
{
	TSharedRef<FWoTFrontEndPresentationFeedbackRecorder> Feedback =
		MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	TSharedRef<FWoTFrontEndPresentationAuthority> Authority =
		MakeInjectedPresentationAuthority(Feedback);
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority);
	TestTrue(TEXT("Player Config opens from the authenticated front end"),
		Root->OpenPlayerConfigForTesting().IsEventHandled());
	TestTrue(TEXT("Player Config screen is active"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::PlayerConfig);
	TArray<TSharedRef<SWidget>> TextWidgets;
	CollectWidgetsOfType(Root, FName(TEXT("STextBlock")), TextWidgets);
	TArray<FString> Text;
	for (const TSharedRef<SWidget>& Widget : TextWidgets)
	{
		Text.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
	}
	TestTrue(TEXT("skin row uses authenticated menuPlayer localization"),
		Text.Contains(FrontEndFixtureLabel(9, TEXT('S'))));
	TestTrue(TEXT("skin help uses authenticated menuPlayer localization"),
		Text.Contains(FrontEndFixtureLabel(31, TEXT('K'))));
	TestTrue(TEXT("skin row exposes previous color"), Text.Contains(TEXT("Red")));
	TestTrue(TEXT("skin row exposes current color"), Text.Contains(TEXT("Base")));
	TestTrue(TEXT("skin row exposes next color"), Text.Contains(TEXT("Black")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndTravelURLSpec,
	"WoT.FrontEnd.TravelURL",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndTravelURLSpec::RunTest(const FString&)
{
	TestTrue(TEXT("Arena package is playable"),
		FWoTFrontEndModel::IsArenaMap(TEXT("/Game/WoT/Maps/Arena_01")));
	TestTrue(TEXT("Battle package is playable"),
		FWoTFrontEndModel::IsBattleMap(TEXT("/Game/WoT/Maps/Battle2_03")));
	TestFalse(TEXT("map prefix is case-sensitive"),
		FWoTFrontEndModel::IsPlayableMap(TEXT("/Game/WoT/Maps/arena_01")));
	TestFalse(TEXT("map must live below the WoT map root"),
		FWoTFrontEndModel::IsPlayableMap(TEXT("/Game/Other/Arena_01")));
	TestEqual(TEXT("short map name strips the package path and options"),
		FWoTFrontEndModel::ShortMapName(TEXT("/Game/WoT/Maps/Battle2_03?listen")),
		FString(TEXT("Battle2_03")));
	TestEqual(TEXT("short map name also strips a portal fragment"),
		FWoTFrontEndModel::ShortMapName(TEXT("/Game/WoT/Maps/Arena_01#Start")),
		FString(TEXT("Arena_01")));

	const FWoTHostRequest ArenaDefaults =
		FWoTFrontEndModel::DefaultHostRequest(TEXT("/Game/WoT/Maps/Arena_01"));
	TestEqual(TEXT("Arena retail frag default"), ArenaDefaults.FragLimit, 20);
	TestEqual(TEXT("Arena retail time default"), ArenaDefaults.TimeLimit, 0);
	const FWoTHostRequest BattleDefaults =
		FWoTFrontEndModel::DefaultHostRequest(TEXT("/Game/WoT/Maps/Battle2_03"));
	TestEqual(TEXT("Battle retail frag default"), BattleDefaults.FragLimit, 0);
	TestEqual(TEXT("Battle retail time default"), BattleDefaults.TimeLimit, 0);
	TestEqual(TEXT("Battle retail seal-goal default"), BattleDefaults.SealGoal, 4);
	TestEqual(TEXT("Battle retail seal-budget default"), BattleDefaults.SealBudget, 3);

	FWoTHostRequest Arena = ArenaDefaults;
	Arena.FragLimit = 125;
	Arena.TimeLimit = -4;
	TestEqual(TEXT("Arena URL clamps rules and omits a redundant game option"),
		FWoTFrontEndModel::BuildHostTravelURL(Arena, EWoTPlayerFaction::Hound),
		FString(TEXT("/Game/WoT/Maps/Arena_01?listen?bIsLanMatch?FragLimit=100?TimeLimit=0"
			"?Cycle=1?MaxPlayers=6?MaxClientTickRate=0"
			"?Class=WOTPawns.Hound")));

	FWoTHostRequest InjectedArena = ArenaDefaults;
	InjectedArena.MapPackage = TEXT(
		"/Game/WoT/Maps/Arena_01?game=/Script/WoT.WoTBattleGameMode#Entry");
	TestEqual(TEXT("host construction discards caller map options and fragments"),
		FWoTFrontEndModel::BuildHostTravelURL(
			InjectedArena, EWoTPlayerFaction::Hound),
		FString(TEXT("/Game/WoT/Maps/Arena_01?listen?bIsLanMatch?FragLimit=20?TimeLimit=0"
			"?Cycle=1?MaxPlayers=6?MaxClientTickRate=0"
			"?Class=WOTPawns.Hound")));

	FWoTHostRequest Battle = BattleDefaults;
	Battle.FragLimit = -1;
	Battle.TimeLimit = 75;
	Battle.SealGoal = 99;
	Battle.SealBudget = -1;
	TestEqual(TEXT("Battle URL selects its game mode and clamps rules"),
		FWoTFrontEndModel::BuildHostTravelURL(Battle, EWoTPlayerFaction::Forsaken),
		FString(TEXT("/Game/WoT/Maps/Battle2_03?listen?bIsLanMatch"
			"?game=/Script/WoT.WoTBattleGameMode?FragLimit=0?TimeLimit=60"
			"?Cycle=1?MaxPlayers=6?MaxClientTickRate=0"
			"?SealGoal=4?SealBudget=2?AutoTeamSelect=0?MaxTeamSize=3"
			"?Class=WOTPawns.Forsaken")));

	FWoTHostRequest Invalid = ArenaDefaults;
	Invalid.MapPackage = TEXT("/Game/WoT/Maps/SinglePlayer_01");
	TestTrue(TEXT("non-playable host package is rejected"),
		FWoTFrontEndModel::BuildHostTravelURL(
			Invalid, EWoTPlayerFaction::AesSedai).IsEmpty());

	TestEqual(TEXT("join/direct URL replaces every case variant and preserves other options"),
		FWoTFrontEndModel::AppendClassOption(
			TEXT("127.0.0.1:7777?Name=Lan?class=Old?CLASS?Skin=Blue#Start"),
			EWoTPlayerFaction::Whitecloak),
		FString(TEXT("127.0.0.1:7777?Name=Lan?Skin=Blue"
			"?Class=WOTPawns.Whitecloak#Start")));
	TestEqual(TEXT("class is inserted before a portal fragment"),
		FWoTFrontEndModel::AppendClassOption(
			TEXT("127.0.0.1#Entry"), EWoTPlayerFaction::AesSedai),
		FString(TEXT("127.0.0.1?Class=WOTPawns.AesSedai#Entry")));
	TestEqual(TEXT("a post-fragment Class option cannot override the saved faction"),
		FWoTFrontEndModel::AppendClassOption(
			TEXT("lan-host#Entry?CLASS=WOTPawns.Forsaken?Name=Player"),
			EWoTPlayerFaction::Whitecloak),
		FString(TEXT("lan-host?Class=WOTPawns.Whitecloak#Entry?Name=Player")));

	FString SanitizedAddress;
	TestTrue(TEXT("direct IPv4 and numeric port are accepted"),
		FWoTFrontEndModel::SanitizeDirectAddress(
			TEXT("  192.0.2.25:7777  "), SanitizedAddress));
	TestEqual(TEXT("direct address whitespace is trimmed"), SanitizedAddress,
		FString(TEXT("192.0.2.25:7777")));
	TestTrue(TEXT("direct hostname without a port is accepted"),
		FWoTFrontEndModel::SanitizeDirectAddress(
			TEXT("lan-host.local"), SanitizedAddress));
	const TCHAR* InvalidDirectAddresses[] = {
		TEXT(""), TEXT(":7777"), TEXT("host:"), TEXT("host:0"),
		TEXT("host:65536"), TEXT("host:not-a-port"), TEXT("host:7777/path"),
		TEXT("host?Class=WOTPawns.Forsaken"), TEXT("host#Entry"),
		TEXT("bad_host:7777")
	};
	for (const TCHAR* InvalidAddress : InvalidDirectAddresses)
	{
		TestFalse(FString::Printf(TEXT("direct target '%s' is rejected"), InvalidAddress),
			FWoTFrontEndModel::SanitizeDirectAddress(
				InvalidAddress, SanitizedAddress));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityP1CHostControlsSpec,
	"WoT.Parity.P1C.HostControls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTParityP1CHostControlsSpec::RunTest(const FString&)
{
	FString Sanitized;
	TestTrue(TEXT("a bounded plain host password is accepted"),
		FWoTFrontEndModel::SanitizeHostPassword(TEXT("amyrlin-7"), Sanitized));
	TestEqual(TEXT("accepted password is unchanged"), Sanitized,
		FString(TEXT("amyrlin-7")));
	TestFalse(TEXT("URL-option injection is rejected as a password"),
		FWoTFrontEndModel::SanitizeHostPassword(
			TEXT("secret?MaxPlayers=16"), Sanitized));

	FWoTFrontEndViewModel Model;
	const TArray<FWoTFrontEndMapEntry> Maps = {
		{TEXT("/Game/WoT/Maps/Battle2_02"), TEXT("Battle2_02"), true}};
	Model.ShowPlay(Maps);
	Model.SetCycleLevels(false);
	Model.SetMaxClientTickRate(99);
	Model.SetAdminPassword(TEXT("admin"));
	Model.SetGamePassword(TEXT("game"));
	Model.SetAutoTeamSelect(true);
	Model.SetMaxTeamSize(4);
	const FWoTHostRequest& Request = Model.GetHostRequest();
	TestFalse(TEXT("host menu can disable map cycling"), Request.bCycleLevels);
	TestEqual(TEXT("host tick rate clamps to the retail maximum"),
		Request.MaxClientTickRate, 30);
	TestEqual(TEXT("host menu retains the admin password"),
		Request.AdminPassword, FString(TEXT("admin")));
	TestEqual(TEXT("host menu retains the game password"),
		Request.GamePassword, FString(TEXT("game")));
	TestTrue(TEXT("host menu enables automatic team selection"),
		Request.bAutoTeamSelect);
	TestEqual(TEXT("host menu selects four players per team"),
		Request.MaxTeamSize, 4);
	TestEqual(TEXT("automatic teams couple total player capacity"),
		Request.MaxPlayers, 8);
	const FString URL = FWoTFrontEndModel::BuildHostTravelURL(Request,
		EWoTPlayerFaction::AesSedai, EWoTPlayerSkinColor::Black);
	const TCHAR* RequiredOptions[] = {
		TEXT("?Cycle=0"), TEXT("?MaxPlayers=8"),
		TEXT("?MaxClientTickRate=30"), TEXT("?AutoTeamSelect=1"),
		TEXT("?MaxTeamSize=4"), TEXT("?AdminPassword=admin"),
		TEXT("?GamePassword=game")};
	for (const TCHAR* Option : RequiredOptions)
	{
		TestTrue(FString::Printf(TEXT("host URL carries %s"), Option),
			URL.Contains(Option, ESearchCase::CaseSensitive));
	}

	UWorld* World = WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass());
	AWoTBattleGameMode* Game = World
		? World->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	if (!TestNotNull(TEXT("real Battle game mode exists"), Game))
	{
		if (World) WoTTest::DestroySpecWorld(World);
		return false;
	}
	FString Error;
	Game->InitGame(TEXT("Battle2_02"),
		TEXT("?Cycle=0?MaxPlayers=8?MaxClientTickRate=30"
			"?AutoTeamSelect=1?MaxTeamSize=4"
			"?AdminPassword=admin?GamePassword=game"), Error);
	TestFalse(TEXT("authority applies cycle off"), Game->bChangeLevels);
	TestEqual(TEXT("authority applies total capacity"), Game->MaxPlayers, 8);
	TestEqual(TEXT("authority applies client tick rate"),
		Game->MaxClientTickRate, 30);
	TestTrue(TEXT("authority applies automatic teams"), Game->bAutoTeamSelect);
	TestEqual(TEXT("authority applies team capacity"), Game->MaxTeamSize, 4);
	TestTrue(TEXT("authority validates its admin password"),
		Game->IsAdminPassword(TEXT(
			"d1f7c49490b09d9cd27704ce37f7ec195a335ac19b961c5eda05c5f6cadd314e")));
	TestFalse(TEXT("authority rejects a wrong admin password"),
		Game->IsAdminPassword(TEXT(
			"36947e6df297c2bbdebf1a9779d3e701fce4e2dc63e57ed4f42643ee3cb2fbe4")));
	const FUniqueNetIdRepl InvalidId;
	FString WrongPasswordError;
	Game->PreLogin(TEXT("?PasswordToken="
		"36947e6df297c2bbdebf1a9779d3e701fce4e2dc63e57ed4f42643ee3cb2fbe4"),
		TEXT("127.0.0.1"), InvalidId,
		WrongPasswordError);
	TestFalse(TEXT("authority rejects a wrong game password"),
		WrongPasswordError.IsEmpty());
	FString CorrectPasswordError;
	Game->PreLogin(TEXT("?PasswordToken="
		"36061083b1809afb45a933837b1aa8af620859b48e15a716df2eede9f23d7702"),
		TEXT("127.0.0.1"), InvalidId,
		CorrectPasswordError);
	TestTrue(TEXT("authority accepts the correct game password"),
		CorrectPasswordError.IsEmpty());
	WoTTest::DestroySpecWorld(World);

	TSharedRef<FWoTFrontEndPresentationFeedbackRecorder> Feedback =
		MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(
			MakeInjectedPresentationAuthority(Feedback));
	TestTrue(TEXT("real host screen opens"),
		Root->OpenPlayForTesting().IsEventHandled());
	TArray<TSharedRef<SWidget>> TextWidgets;
	CollectWidgetsOfType(Root, FName(TEXT("STextBlock")), TextWidgets);
	TArray<FString> Labels;
	for (const TSharedRef<SWidget>& Widget : TextWidgets)
	{
		Labels.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
	}
	TestTrue(TEXT("host screen exposes Cycle Levels"),
		Labels.Contains(FrontEndFixtureLabel(12, TEXT('C'))));
	TestTrue(TEXT("host screen exposes Max Client Tick Rate"),
		Labels.Contains(FrontEndFixtureLabel(20, TEXT('X'))));
	TestTrue(TEXT("host screen exposes Admin Password"),
		Labels.Contains(FrontEndFixtureLabel(14, TEXT('A'))));
	TestTrue(TEXT("host screen exposes Game Password"),
		Labels.Contains(FrontEndFixtureLabel(13, TEXT('G'))));
	TestTrue(TEXT("host screen exposes Auto Team Selection"),
		Labels.Contains(FrontEndFixtureLabel(19, TEXT('U'))));
	TestTrue(TEXT("host screen exposes Maximum Team Size"),
		Labels.Contains(FrontEndFixtureLabel(17, TEXT('Z'))));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityP1CBattleDefaultsSpec,
	"WoT.Parity.P1C.BattleDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTParityP1CBattleDefaultsSpec::RunTest(const FString&)
{
	FWoTHostRequest Battle = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Battle2_02"));
	TestEqual(TEXT("Battle2 defaults to six total players"),
		Battle.MaxPlayers, 6);
	TestEqual(TEXT("Battle2 defaults to three players per team"),
		Battle.MaxTeamSize, 3);
	TestFalse(TEXT("automatic team selection defaults off"),
		Battle.bAutoTeamSelect);
	Battle.bAutoTeamSelect = true;
	Battle.MaxTeamSize = 6;
	FWoTFrontEndModel::UpdateBattlePlayerLimits(Battle, 2);
	TestEqual(TEXT("two citadels allow at most eight players per team"),
		Battle.MaxTeamSize, 6);
	TestEqual(TEXT("auto teams derive total capacity from team size"),
		Battle.MaxPlayers, 12);
	Battle.MaxTeamSize = 16;
	FWoTFrontEndModel::UpdateBattlePlayerLimits(Battle, 4);
	TestEqual(TEXT("four citadels clamp team size to four"),
		Battle.MaxTeamSize, 4);
	TestEqual(TEXT("four full teams clamp total capacity to sixteen"),
		Battle.MaxPlayers, 16);
	FWoTHostRequest Stale = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Battle2_02"));
	Stale.bAutoTeamSelect = true;
	Stale.MaxTeamSize = 4;
	Stale.MaxPlayers = 2;
	const FString CoupledURL = FWoTFrontEndModel::BuildHostTravelURL(
		Stale, EWoTPlayerFaction::AesSedai);
	TestTrue(TEXT("URL construction enforces auto-team player coupling"),
		CoupledURL.Contains(TEXT("?MaxPlayers=8?"),
			ESearchCase::CaseSensitive));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSealSettingsRuleSpec,
	"WoT.FrontEnd.SealSettingsRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSealSettingsRuleSpec::RunTest(const FString&)
{
	FWoTHostRequest Request;
	Request.SealGoal = 99;
	Request.SealBudget = -7;
	FWoTFrontEndModel::UpdateSealGoal(Request, Request.SealGoal, 2);
	TestEqual(TEXT("goal clamps to the retail maximum"), Request.SealGoal, 4);
	TestEqual(TEXT("budget clamps to ceil(goal/citadels)"), Request.SealBudget, 2);

	Request.SealBudget = 99;
	FWoTFrontEndModel::UpdateSealGoal(Request, -1, 4);
	TestEqual(TEXT("goal clamps to the retail minimum"), Request.SealGoal, 2);
	TestEqual(TEXT("budget clamps below the goal"), Request.SealBudget, 1);

	Request.SealGoal = 4;
	Request.SealBudget = 3;
	FWoTFrontEndModel::UpdateSealGoal(Request, 3, 3);
	TestEqual(TEXT("2 to 3 citadels sets goal 3"), Request.SealGoal, 3);
	TestEqual(TEXT("2 to 3 citadels clamps budget to 2"), Request.SealBudget, 2);
	FWoTFrontEndModel::UpdateSealGoal(Request, 2, 2);
	TestEqual(TEXT("3 to 2 citadels sets goal 2"), Request.SealGoal, 2);
	TestEqual(TEXT("3 to 2 citadels clamps budget to 1"), Request.SealBudget, 1);
	FWoTFrontEndModel::UpdateSealGoal(Request, 4, 4);
	TestEqual(TEXT("2 to 4 citadels sets goal 4"), Request.SealGoal, 4);
	TestEqual(TEXT("2 to 4 citadels keeps the current budget"), Request.SealBudget, 1);

	FWoTFrontEndModel::UpdateSealBudget(Request, 8, 4);
	TestEqual(TEXT("budget step helper clamps to goal minus one"), Request.SealBudget, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndLeaveStateSpec,
	"WoT.FrontEnd.LeaveState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndLeaveStateSpec::RunTest(const FString&)
{
	TestEqual(TEXT("development leave retains the generated front-end map"),
		FWoTLeaveStateMachine::FrontEndURLForBuild(false),
		FString(TEXT("/Game/WoT/Maps/WoTFrontEnd")));
	TestEqual(TEXT("guarded leave returns through the cooked bootstrap map"),
		FWoTLeaveStateMachine::FrontEndURLForBuild(true),
		FString(TEXT("/Game/WoT/Maps/WoTBootstrap")));
	FWoTLeaveStateMachine Leave;
	TestEqual(TEXT("leave starts idle"), static_cast<int32>(Leave.GetState()),
		static_cast<int32>(EWoTLeaveState::Idle));
	TestTrue(TEXT("named-session leave starts destroy"), Leave.Begin(true));
	TestEqual(TEXT("named-session leave waits in destroy state"),
		static_cast<int32>(Leave.GetState()),
		static_cast<int32>(EWoTLeaveState::DestroyingSession));
	TestTrue(TEXT("return URL is withheld before destroy completion"),
		Leave.TakeReturnURL().IsEmpty());
	TestFalse(TEXT("overlapping leave is refused"), Leave.Begin(false));
	TestTrue(TEXT("destroy callback advances leave"), Leave.CompleteDestroy());
	TestEqual(TEXT("destroy completion selects return state"),
		static_cast<int32>(Leave.GetState()),
		static_cast<int32>(EWoTLeaveState::ReturningToMenu));
	TestEqual(TEXT("leave chooses the active build's front-end map"),
		Leave.TakeReturnURL(), FWoTLeaveStateMachine::FrontEndURL());
	TestEqual(TEXT("consuming return URL restores idle"),
		static_cast<int32>(Leave.GetState()),
		static_cast<int32>(EWoTLeaveState::Idle));
	TestFalse(TEXT("late destroy callback is ignored"), Leave.CompleteDestroy());

	TestTrue(TEXT("direct-IP leave skips destroy"), Leave.Begin(false));
	TestEqual(TEXT("direct-IP leave selects the same return URL"),
		Leave.TakeReturnURL(), FWoTLeaveStateMachine::FrontEndURL());
	Leave.Reset();
	TestEqual(TEXT("shutdown reset is idle"), static_cast<int32>(Leave.GetState()),
		static_cast<int32>(EWoTLeaveState::Idle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndEscapeOverlaySpec,
	"WoT.FrontEnd.EscapeOverlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndEscapeOverlaySpec::RunTest(const FString&)
{
	FWoTInGameMenuModel Menu;
	TestFalse(TEXT("closed overlay cannot Resume"), Menu.Resume());
	TestTrue(TEXT("Escape opens the overlay model"), Menu.Open());
	TestFalse(TEXT("an open overlay cannot open twice"), Menu.Open());
	TestTrue(TEXT("Leave enters its terminal overlay state"), Menu.BeginLeave());
	TestFalse(TEXT("Resume is disabled while leaving"), Menu.Resume());
	TestTrue(TEXT("a synchronous Leave refusal restores the open menu"),
		Menu.CancelLeave());
	TestTrue(TEXT("Resume closes the overlay"), Menu.Resume());
	TestFalse(TEXT("closed overlay is not visible"), Menu.IsOpen());

	UWorld* World = WoTFrontEndTests_CreateWorld();
	if (!TestNotNull(TEXT("Escape teardown world exists"), World))
	{
		return false;
	}
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (TestNotNull(TEXT("gameplay controller exists"), Controller))
	{
		Controller->EditSessionTeam = 0;
		Controller->OwnerEditSessionTeam = 0;
		Controller->ToggleInGameMenu();
		TestFalse(TEXT("retail precedence makes first Escape end citadel editing"),
			Controller->IsInGameMenuOpen());
		TestFalse(TEXT("citadel editing ended before menu presentation"),
			Controller->IsOwnerEditingCitadel());
		Controller->bShowScores = true;
		Controller->ToggleInGameMenu();
		TestTrue(TEXT("automation can transition without a Slate widget"),
			Controller->IsInGameMenuOpen());
		TestTrue(TEXT("opening the overlay preserves scoreboard state"),
			Controller->bShowScores);
		Controller->LeaveToFrontEnd();
		TestTrue(TEXT("widget-free controller enters Leaving"),
			Controller->IsLeavingForMenu());
		Controller->Destroy();
		TestFalse(TEXT("controller EndPlay resets overlay state"),
			Controller->IsInGameMenuOpen());
	}
	// Includes GameInstance::Shutdown before world-context/world destruction.
	WoTFrontEndTests_DestroyWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInGameMenuSettingsSpec,
	"WoT.FrontEnd.InGameMenu.SettingsNavigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInGameMenuSettingsSpec::RunTest(const FString&)
{
	UWorld* World = WoTFrontEndTests_CreateWorld();
	if (!TestNotNull(TEXT("in-game menu world exists"), World))
	{
		return false;
	}
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("in-game menu controller exists"), Controller))
	{
		WoTFrontEndTests_DestroyWorld(World);
		return false;
	}
	Controller->ToggleInGameMenu();
	TestTrue(TEXT("controller model opens before Slate navigation"),
		Controller->IsInGameMenuOpen());

	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Fonts = MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	TSharedRef<FWoTFrontEndPresentationFeedbackRecorder> Feedback =
		MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	TSharedRef<FWoTFrontEndPresentationAuthority> Presentation =
		MakeInjectedPresentationAuthority(Feedback);
	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.PresentationAuthority(Presentation)
		.RequiredFontsForTesting(Fonts);
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);

	const TArray<FString> ExpectedLabels = {
		TEXT("Resume"), TEXT("Scoreboard (F4)"), TEXT("Help (F1)"),
		TEXT("Player Config"), TEXT("Controls"), TEXT("Hardware"),
		TEXT("Quit to Main Menu")};
	TestTrue(TEXT("menu items retain the required order and labels"),
		Overlay->MenuLabelsForTesting() == ExpectedLabels);
	FWoTGameSettings Rebound = FWoTGameSettings::Defaults();
	Rebound.FindBinding(EWoTManagedBinding::ShowScores)->Primary = EKeys::F6;
	Rebound.FindBinding(EWoTManagedBinding::ShowHelp)->Primary = EKeys::H;
	Overlay->SetGameSettingsForTesting(Rebound);
	const TArray<FString> ReboundLabels = Overlay->MenuLabelsForTesting();
	TestTrue(TEXT("pause labels read the live primary bindings"),
		ReboundLabels.Contains(TEXT("Scoreboard (F6)"))
		&& ReboundLabels.Contains(TEXT("Help (H)")));
	TestNotEqual(TEXT("heading and item fonts are visually distinct"),
		FMath::RoundToInt(Overlay->HeadingFontForTesting().Size),
		FMath::RoundToInt(Overlay->ItemFontForTesting().Size));
	const FSlateFontInfo FrontEndBody = Presentation->MultiplayerFont(
		FName(TEXT("F_WOTReg14")), 1.0f, 2);
	TestTrue(TEXT("game-menu items are at least the front-end readable body size"),
		Overlay->ItemFontForTesting().Size >= FrontEndBody.Size);
	TestTrue(TEXT("game-menu title uses the front-end heading face size"),
		FMath::IsNearlyEqual(Overlay->HeadingFontForTesting().Size,
			Presentation->MultiplayerFont(
				FName(TEXT("F_WOTReg30")), 1.0f).Size));
	TestTrue(TEXT("all main-menu content fits the 1280x720 panel without scrolling"),
		Overlay->MainMenuDesiredHeightForTesting() <= 620.0f);
	TestFalse(TEXT("gameplay-controller presentation factory fails gracefully without a lease"),
		FWoTFrontEndPresentationAuthority::Create(Controller).IsValid());

	Overlay->OpenSettingsForTesting(EWoTSettingsPage::Controls);
	TestNotNull(TEXT("Controls opens the existing settings screen"),
		Overlay->SettingsScreenForTesting().Get());
	const TCHAR* RequiredSettingsKeys[] = {
		TEXT("menuOptions.MenuTitle"), TEXT("menuOptions.MenuList[1]"),
		TEXT("menuOptions.MenuList[2]"), TEXT("menuOptions.MenuList[3]"),
		TEXT("menuKeyboard.MenuList[1]"), TEXT("menuKeyboard.MenuList[2]"),
		TEXT("menuKeyboard.MenuList[3]"), TEXT("menuKeyboard.MenuList[4]"),
		TEXT("menuKeyboard.MenuList[5]"), TEXT("menuKeyboard.MenuList[6]"),
		TEXT("menuKeyboard.MenuList[7]"), TEXT("menuKeyboard.MenuList[8]"),
		TEXT("menuKeyboard.MenuList[9]"), TEXT("menuKeyboard.MenuList[10]"),
		TEXT("menuKeyboard.MenuList[11]"),
		TEXT("menuKeyboard.MenuList[13]"), TEXT("menuKeyboard.MenuList[14]"),
		TEXT("menuKeyboard.MenuList[18]"),
		TEXT("menuConfiguration.MenuTitle"),
		TEXT("menuConfiguration.MenuList[2]"),
		TEXT("menuConfiguration.MenuList[3]"),
		TEXT("menuConfiguration.MenuList[5]"),
		TEXT("menuConfiguration.MenuList[6]"),
		TEXT("menuConfiguration.MenuList[15]")};
	if (Overlay->SettingsScreenForTesting().IsValid())
	{
		for (const TCHAR* Key : RequiredSettingsKeys)
		{
			const FString Resolved = Overlay->SettingsScreenForTesting()
				->ResolvedRetailTextForTesting(Key).ToString();
			TestFalse(*FString::Printf(TEXT("in-game setting resolves %s"), Key),
				Resolved.IsEmpty() || Resolved == Key);
		}
	}
	TestTrue(TEXT("settings keep gameplay menu suspension active"),
		Controller->IsInGameMenuOpen());
	Overlay->OnPreviewKeyDown(FGeometry(), FKeyEvent(EKeys::Escape,
		FModifierKeysState(), 0, false, 0, 0));
	TestTrue(TEXT("Escape from Controls returns one level to Game Menu"),
		Overlay->IsShowingMainMenuForTesting());
	TestTrue(TEXT("settings Back does not resume gameplay"),
		Controller->IsInGameMenuOpen());

	Overlay->OpenSettingsForTesting(EWoTSettingsPage::Hardware);
	TestNotNull(TEXT("Hardware opens the existing settings screen"),
		Overlay->SettingsScreenForTesting().Get());
	if (Overlay->SettingsScreenForTesting().IsValid())
	{
		Overlay->SettingsScreenForTesting()->HandleBack();
	}
	TestTrue(TEXT("Hardware Back returns to Game Menu"),
		Overlay->IsShowingMainMenuForTesting());
	Controller->bShowScores = false;
	Overlay->ActivateScoreboardForTesting();
	TestTrue(TEXT("pause Scoreboard toggles the scoreboard"), Controller->bShowScores);
	TestFalse(TEXT("pause Scoreboard closes the game menu"),
		Controller->IsInGameMenuOpen());
	Controller->ToggleInGameMenu();
	Overlay->ActivateHelpForTesting();
	TestTrue(TEXT("pause Help opens the help model"), Controller->IsHelpOpen());
	TestFalse(TEXT("pause Help closes the game menu"),
		Controller->IsInGameMenuOpen());
	Controller->ShowHelp();
	Controller->ToggleInGameMenu();
	Controller->ShowHelp();
	TestTrue(TEXT("ShowHelp replaces an open retail window"),
		Controller->IsHelpOpen());
	TestFalse(TEXT("ShowHelp removes the pause window before opening"),
		Controller->IsInGameMenuOpen());
	Controller->ShowHelp();
	const TSharedRef<SWoTInGameMenuOverlay> FallbackOverlay =
		SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller);
	TestTrue(TEXT("missing presentation lease selects readable fallback fonts"),
		FallbackOverlay->IsUsingFallbackFontsForTesting()
			&& FallbackOverlay->GetVisibility().IsVisible()
			&& FallbackOverlay->ItemFontForTesting().HasValidFont());
	FallbackOverlay->OpenSettingsForTesting(EWoTSettingsPage::Hardware);
	if (TestTrue(TEXT("settings remain available without presentation authority"),
		FallbackOverlay->SettingsScreenForTesting().IsValid()))
	{
		for (const TCHAR* Key : RequiredSettingsKeys)
		{
			const FString Resolved = FallbackOverlay->SettingsScreenForTesting()
				->ResolvedRetailTextForTesting(Key).ToString();
			TestFalse(*FString::Printf(TEXT("fallback setting resolves %s"), Key),
				Resolved.IsEmpty() || Resolved == Key);
		}
	}
	Overlay->ResumeForTesting();
	TestFalse(TEXT("Resume restores the controller gameplay state"),
		Controller->IsInGameMenuOpen());

	Controller->Destroy();
	WoTFrontEndTests_DestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInGameMenuKeyboardNavSpec,
	"WoT.FrontEnd.InGameMenu.KeyboardNav",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInGameMenuKeyboardNavSpec::RunTest(const FString&)
{
	if (!TestTrue(TEXT("Slate application is initialized"),
		FSlateApplication::IsInitialized()))
	{
		return false;
	}
	UWorld* World = WoTFrontEndTests_CreateWorld();
	if (!TestNotNull(TEXT("keyboard-nav world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTFrontEndTests_DestroyWorld(World); };
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("keyboard-nav controller exists"), Controller))
	{
		return false;
	}
	Controller->ToggleInGameMenu();

	// CreateInGameMenuWidget intentionally returns during automation. Mount the
	// real overlay and apply its InitialFocusWidget contract as the controller's
	// queued FInputModeUIOnly reply would on the next Slate pass.
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Fonts = MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.RequiredFontsForTesting(Fonts);
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(1024.0f, 768.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.CreateTitleBar(false)
		[Overlay];
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window, true);
	ON_SCOPE_EXIT
	{
		Window->RequestDestroyWindow();
		Slate.Tick();
	};
	Slate.Tick();
	Window->SlatePrepass(1.0f);
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	Window->SlatePrepass(1.0f);
	Slate.Tick();

	const TSharedPtr<SWidget> InitialFocus = Overlay->InitialFocusWidget();
	if (!TestTrue(TEXT("queued input-mode target is the Resume button"),
		InitialFocus.IsValid()
			&& InitialFocus == Overlay->MainMenuButtonForTesting(0)))
	{
		return false;
	}
	FWidgetPath OverlayPath;
	if (!TestTrue(TEXT("mounted overlay has a Slate widget path"),
		Slate.GeneratePathToWidgetUnchecked(Overlay, OverlayPath)))
	{
		return false;
	}
	const FReply QueuedInputModeReply = FReply::Handled().SetUserFocus(
		InitialFocus.ToSharedRef(), EFocusCause::SetDirectly);
	Slate.ProcessReply(OverlayPath, QueuedInputModeReply, nullptr, nullptr, 0);
	Slate.Tick();
	TestTrue(TEXT("Resume holds keyboard focus after the queued reply"),
		Slate.GetKeyboardFocusedWidget() == InitialFocus);
	TestEqual(TEXT("exactly the focused Resume entry is selected"),
		Overlay->SelectedMainMenuIndexForTesting(), 0);

	auto PressKey = [&Slate](FKey Key)
	{
		const FKeyEvent Event = MakeFrontEndKeyEvent(Key);
		const bool bHandled = Slate.ProcessKeyDownEvent(Event);
		Slate.ProcessKeyUpEvent(Event);
		Slate.Tick();
		return bHandled;
	};
	TestTrue(TEXT("Down is handled"), PressKey(EKeys::Down));
	TestTrue(TEXT("Down moves focus to the next entry"),
		Slate.GetKeyboardFocusedWidget()
			== Overlay->MainMenuButtonForTesting(1));
	TestTrue(TEXT("Up is handled"), PressKey(EKeys::Up));
	TestTrue(TEXT("Up moves focus back to Resume"),
		Slate.GetKeyboardFocusedWidget()
			== Overlay->MainMenuButtonForTesting(0));
	TestTrue(TEXT("Up wraps from first to last"), PressKey(EKeys::Up));
	TestTrue(TEXT("first-to-last wrap focuses Quit"),
		Slate.GetKeyboardFocusedWidget()
			== Overlay->MainMenuButtonForTesting(6));
	TestTrue(TEXT("Down wraps from last to first"), PressKey(EKeys::Down));
	TestTrue(TEXT("last-to-first wrap focuses Resume"),
		Slate.GetKeyboardFocusedWidget()
			== Overlay->MainMenuButtonForTesting(0));

	const TSharedPtr<SWoTMenuButton> Controls =
		Overlay->MainMenuButtonForTesting(4);
	if (TestTrue(TEXT("Controls button exists for hover"), Controls.IsValid()))
	{
		const FVector2D Position =
			Controls->GetCachedGeometry().GetAbsolutePosition();
		const FPointerEvent Hover(0, Position, Position, TSet<FKey>(), FKey(),
			0.0f, FModifierKeysState());
		Controls->OnMouseEnter(Controls->GetCachedGeometry(), Hover);
		TestTrue(TEXT("hover moves keyboard focus"),
			Slate.GetKeyboardFocusedWidget() == Controls);
		TestEqual(TEXT("hover leaves exactly Controls selected"),
			Overlay->SelectedMainMenuIndexForTesting(), 4);
	}
	TestTrue(TEXT("Enter is handled by the focused entry"), PressKey(EKeys::Enter));
	TestTrue(TEXT("Enter activates the focused Controls entry"),
		Overlay->SettingsScreenForTesting().IsValid());
	TestTrue(TEXT("Escape returns from Controls to the menu"),
		PressKey(EKeys::Escape));
	TestTrue(TEXT("returning from Controls restores Resume focus"),
		Slate.GetKeyboardFocusedWidget()
			== Overlay->MainMenuButtonForTesting(0));
	FWoTGameSettings HelpOnUp = FWoTGameSettings::Defaults();
	HelpOnUp.FindBinding(EWoTManagedBinding::ShowHelp)->Primary = EKeys::Up;
	Overlay->SetGameSettingsForTesting(HelpOnUp);
	TestTrue(TEXT("a rebound Help key keeps priority over menu navigation"),
		PressKey(EKeys::Up));
	TestTrue(TEXT("the rebound Help key still opens Help"),
		Controller->IsHelpOpen());
	TestFalse(TEXT("opening Help resumes out of the game menu"),
		Controller->IsInGameMenuOpen());
	Controller->ShowHelp();
	Controller->ToggleInGameMenu();
	Overlay->FocusResumeButton();
	TestTrue(TEXT("Escape from the main overlay is handled"),
		PressKey(EKeys::Escape));
	TestFalse(TEXT("Escape still resumes gameplay"),
		Controller->IsInGameMenuOpen());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInGameMenuControllerNavSpec,
	"WoT.FrontEnd.InGameMenu.ControllerNav",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInGameMenuControllerNavSpec::RunTest(const FString&)
{
	if (!TestTrue(TEXT("Slate application is initialized"),
		FSlateApplication::IsInitialized()))
	{
		return false;
	}
	UWorld* World = WoTFrontEndTests_CreateWorld();
	if (!TestNotNull(TEXT("controller-nav world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTFrontEndTests_DestroyWorld(World); };
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("controller-nav controller exists"), Controller))
	{
		return false;
	}
	Controller->ToggleInGameMenu();
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Fonts = MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay)
		.PlayerController(Controller)
		.RequiredFontsForTesting(Fonts);
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(1024.0f, 768.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.CreateTitleBar(false)
		[Overlay];
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window, true);
	ON_SCOPE_EXIT
	{
		Window->RequestDestroyWindow();
		Slate.Tick();
	};
	Slate.Tick();
	Window->SlatePrepass(1.0f);
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	Window->SlatePrepass(1.0f);
	Slate.Tick();
	Overlay->FocusResumeButton();
	Slate.Tick();
	if (!TestTrue(TEXT("Resume starts focused"),
		Slate.GetKeyboardFocusedWidget() == Overlay->MainMenuButtonForTesting(0)))
	{
		return false;
	}

	auto PressKey = [&Slate](FKey Key)
	{
		const FKeyEvent Event = MakeFrontEndKeyEvent(Key);
		const bool bHandled = Slate.ProcessKeyDownEvent(Event);
		Slate.ProcessKeyUpEvent(Event);
		Slate.Tick();
		return bHandled;
	};
	auto FocusedIs = [&Slate, &Overlay](int32 Index)
	{
		return Slate.GetKeyboardFocusedWidget() == Overlay->MainMenuButtonForTesting(Index)
			&& Overlay->SelectedMainMenuIndexForTesting() == Index;
	};
	// The D-pad moves the one highlighted entry and wraps like the arrow keys.
	PressKey(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("D-pad down selects the next entry"), FocusedIs(1));
	PressKey(EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("D-pad up returns to Resume"), FocusedIs(0));
	PressKey(EKeys::Gamepad_DPad_Up);
	TestTrue(TEXT("D-pad up wraps from first to last"), FocusedIs(6));
	PressKey(EKeys::Gamepad_DPad_Down);
	TestTrue(TEXT("D-pad down wraps from last to first"), FocusedIs(0));
	// The stick arrives as analog navigation, not keys; it wraps the same way.
	const FNavigationEvent StickUp(FModifierKeysState(), 0, EUINavigation::Up,
		ENavigationGenesis::Controller);
	Overlay->OnNavigation(Overlay->GetCachedGeometry(), StickUp);
	Slate.Tick();
	TestTrue(TEXT("stick up wraps from first to last"), FocusedIs(6));
	const FNavigationEvent StickDown(FModifierKeysState(), 0, EUINavigation::Down,
		ENavigationGenesis::Controller);
	Overlay->OnNavigation(Overlay->GetCachedGeometry(), StickDown);
	Slate.Tick();
	TestTrue(TEXT("stick down wraps from last to first"), FocusedIs(0));

	// B backs out one level, then resumes, like Escape.
	for (int32 Step = 0; Step < 4; ++Step)
	{
		PressKey(EKeys::Gamepad_DPad_Down);
	}
	TestTrue(TEXT("Controls is selected"), FocusedIs(4));
	PressKey(EKeys::Gamepad_FaceButton_Bottom);
	TestTrue(TEXT("A opens Controls"), Overlay->SettingsScreenForTesting().IsValid());
	TestTrue(TEXT("B is handled on Controls"), PressKey(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B returns from Controls to the menu"),
		Overlay->SettingsScreenForTesting().IsValid());
	TestTrue(TEXT("the menu is still open after backing out of Controls"),
		Controller->IsInGameMenuOpen());
	TestTrue(TEXT("B is handled on the menu"), PressKey(EKeys::Gamepad_FaceButton_Right));
	TestFalse(TEXT("B on the menu resumes gameplay"), Controller->IsInGameMenuOpen());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInGameMenuStableActionSpec,
	"WoT.FrontEnd.InGameMenu.StableAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTInGameMenuStableActionSpec::RunTest(const FString&)
{
	UWorld* World = WoTFrontEndTests_CreateWorld();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	Battle->NumTeams = 2;
	Battle->TeamClass[0] = AWoTCharacter::StaticClass();
	Battle->TeamClass[1] = AWoTCharacter::StaticClass();
	Battle->FinishSpawning(FTransform::Identity);
	Battle->bTeleportersDisabled = true;
	Controller->ToggleInGameMenu();
	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay).PlayerController(Controller);
	TestTrue(TEXT("edit-phase menu includes the citadel row"),
		Overlay->MenuLabelsForTesting().Contains(TEXT("Edit Citadel (F10)")));
	Battle->bTeleportersDisabled = false;
	Overlay->ActivateVisibleMainMenuEntryForTesting(6);
	TestTrue(TEXT("phase changes cannot retarget a visible Hardware row to Quit"),
		Overlay->SettingsScreenForTesting().IsValid());
	TestFalse(TEXT("stable Hardware dispatch does not begin leaving"),
		Controller->IsLeavingForMenu());
	if (Overlay->SettingsScreenForTesting().IsValid())
	{
		Overlay->SettingsScreenForTesting()->HandleBack();
	}
	TestEqual(TEXT("post-transition menu rebuild has seven rendered rows"),
		Overlay->RenderedMainMenuEntryCountForTesting(), 7);
	Battle->bTeleportersDisabled = true;
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	TestEqual(TEXT("entering edit phase atomically adds the rendered row"),
		Overlay->RenderedMainMenuEntryCountForTesting(), 8);
	Battle->bTeleportersDisabled = false;
	Overlay->Tick(FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	TestEqual(TEXT("leaving edit phase atomically removes the rendered row"),
		Overlay->RenderedMainMenuEntryCountForTesting(), 7);
	Controller->Destroy();
	WoTFrontEndTests_DestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityP1CPlayerConfigLiveSpec,
	"WoT.Parity.P1C.PlayerConfig.InGameLive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTParityP1CPlayerConfigLiveSpec::RunTest(const FString&)
{
	UFunction* ChangeSkin = AWoTPlayerController::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(AWoTPlayerController, ServerChangeSkin));
	TestNotNull(TEXT("ServerChangeSkin remains reflected"), ChangeSkin);
	if (ChangeSkin)
	{
		TestTrue(TEXT("skin change is a server RPC"),
			ChangeSkin->HasAnyFunctionFlags(FUNC_Net | FUNC_NetServer));
		TestTrue(TEXT("skin change is reliable"),
			ChangeSkin->HasAnyFunctionFlags(FUNC_NetReliable));
		TestTrue(TEXT("skin change is validated"),
			ChangeSkin->HasAnyFunctionFlags(FUNC_NetValidate));
	}

	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	if (!TestNotNull(TEXT("player-config world exists"), World))
	{
		return false;
	}
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("player-config controller exists"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->InitPlayerState();
	Controller->PlayerState->SetPlayerName(TEXT("Rand"));
	AWoTAesSedai* Character = World->SpawnActor<AWoTAesSedai>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("player-config character exists"), Character))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->Possess(Character);
	const TSharedRef<SWoTInGameMenuOverlay> Overlay =
		SNew(SWoTInGameMenuOverlay).PlayerController(Controller);
	TestTrue(TEXT("in-game menu exposes Player Config"),
		Overlay->MenuLabelsForTesting().Contains(TEXT("Player Config")));
	TestTrue(TEXT("closed-list skin request validates"),
		Controller->ServerChangeSkin_Validate(
			TEXT("AesSedaiSkins.Aes_Red"), TEXT(""), 255));
	TestFalse(TEXT("arbitrary skin package is rejected"),
		Controller->ServerChangeSkin_Validate(TEXT("Engine.Engine"), TEXT(""), 255));
	TestFalse(TEXT("unused face channel cannot carry input"),
		Controller->ServerChangeSkin_Validate(
			TEXT("AesSedaiSkins.Aes_Red"), TEXT("Injected"), 255));
	TestFalse(TEXT("unused team channel cannot carry input"),
		Controller->ServerChangeSkin_Validate(
			TEXT("AesSedaiSkins.Aes_Red"), TEXT(""), 0));
	TestTrue(TEXT("in-game Player Config page opens"),
		Overlay->OpenPlayerConfigForTesting().IsEventHandled());
	Overlay->SetPendingPlayerConfigForTesting(
		TEXT("Egwene"), EWoTPlayerSkinColor::Red);
	TestTrue(TEXT("in-game Player Config save is handled"),
		Overlay->SavePlayerConfigForTesting().IsEventHandled());
	TestEqual(TEXT("in-game save changes the live player name"),
		Controller->PlayerState->GetPlayerName(), FString(TEXT("Egwene")));
	TestEqual(TEXT("in-game save records the live skin URL"),
		Controller->PersonalSkinURL, FString(TEXT("AesSedaiSkins.Aes_Red")));
	TestEqual(TEXT("in-game save applies the live pawn skin"),
		Character->PersonalSkin, FName(TEXT("Aes_Red")));
	Controller->Destroy();
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHelpWindowSpec,
	"WoT.FrontEnd.HelpWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTHelpWindowSpec::RunTest(const FString&)
{
	if (!FWoTRetailText::EnsureLoaded())
	{
		AddWarning(TEXT("retail localization is unavailable; skipping help-content checks"));
		return true;
	}
	FWoTHelpContent Content;
	if (!TestTrue(TEXT("runtime help section loads"),
		FWoTHelpContent::LoadRuntime(Content)))
	{
		return false;
	}
	const FString* Title = FWoTRetailText::FindSectionValue(
		TEXT("WOTHelpInfo"), TEXT("Title"));
	const FString* ConsoleLine = FWoTRetailText::FindSectionValue(
		TEXT("WOTHelpInfo"), TEXT("Content[37]"));
	const FString* TabLine = FWoTRetailText::FindSectionValue(
		TEXT("WOTHelpInfo"), TEXT("Content[36]"));
	const FString* MovementHeading = FWoTRetailText::FindSectionValue(
		TEXT("WOTHelpInfo"), TEXT("Content[7]"));
	const FString* FunctionKeyLabel = FWoTRetailText::FindSectionValue(
		TEXT("WOTHelpInfo"), TEXT("Content[57]"));
	if (!TestNotNull(TEXT("help title is loaded"), Title)
		|| !TestNotNull(TEXT("console help line is loaded"), ConsoleLine)
		|| !TestNotNull(TEXT("TAB help line is loaded"), TabLine)
		|| !TestNotNull(TEXT("movement heading is loaded"), MovementHeading)
		|| !TestNotNull(TEXT("function-key label is loaded"), FunctionKeyLabel))
	{
		return false;
	}
	TestEqual(TEXT("help title is retail localized text"), Content.Title, *Title);
	TestEqual(TEXT("every contiguous retail help content entry is retained"),
		Content.Content.Num(), 73);
	TestEqual(TEXT("console prose comes from the loaded entry"),
		Content.Content[37], *ConsoleLine);
	TestEqual(TEXT("TAB prose comes from the loaded entry"),
		Content.Content[36], *TabLine);
	const TArray<FWoTHelpLayoutLine> Lines = FWoTHelpContent::Layout(Content);
	TestTrue(TEXT("font command selects the retail italic heading face"),
		Lines.ContainsByPredicate([MovementHeading](const FWoTHelpLayoutLine& Line)
		{
			return Line.Text == *MovementHeading && Line.FontIndex == 2;
		}));
	TestTrue(TEXT("absolute X/Y commands form the retail F-key columns"),
		Lines.ContainsByPredicate([FunctionKeyLabel](const FWoTHelpLayoutLine& Line)
		{
			return Line.Text == *FunctionKeyLabel
				&& FMath::IsNearlyEqual(Line.X, 320.0f)
				&& FMath::IsNearlyEqual(Line.Y, 325.0f);
		}));
	FWoTHelpContent Wrapping;
	Wrapping.Title = TEXT("Help");
	Wrapping.SubTitle = TEXT("Close");
	Wrapping.Content = {TEXT("%c=0"), FString::ChrN(60, TEXT('A'))};
	const TArray<FWoTHelpLayoutLine> WrappedLines = FWoTHelpContent::Layout(
		Wrapping, [](const FString& Text, int32)
		{
			return FVector2D(Text.Len() * 10.0f, 14.0f);
		});
	TestEqual(TEXT("retail 512-wide clip wraps long content"),
		WrappedLines.Num(), 2);
	TestFalse(TEXT("wrapped lines remain inside the 512x432 retail clip"),
		WrappedLines.ContainsByPredicate([](const FWoTHelpLayoutLine& Line)
		{
			return Line.Y < 14.0f || Line.Y + Line.Height > 432.0f
				|| Line.X < 0.0f || Line.X + Line.Width > 512.0f;
		}));

	int32 CloseCount = 0;
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Fonts = MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	const TSharedRef<SWoTHelpOverlay> Help = SNew(SWoTHelpOverlay)
		.Content(Content)
		.OnClose(FSimpleDelegate::CreateLambda([&CloseCount]() { ++CloseCount; }))
		.PrimaryCloseKey(EKeys::H)
		.SecondaryCloseKey(EKeys::J)
		.RequiredFontsForTesting(Fonts);
	TArray<TSharedRef<SWidget>> TextWidgets;
	CollectWidgetsOfType(Help, FName(TEXT("STextBlock")), TextWidgets);
	bool bHasTitle = false;
	bool bHasRetailBody = false;
	for (const TSharedRef<SWidget>& Widget : TextWidgets)
	{
		const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		bHasTitle |= Text == *Title;
		bHasRetailBody |= Text == *MovementHeading;
	}
	TestTrue(TEXT("help widget renders retail title and content"),
		bHasTitle && bHasRetailBody);
	TestTrue(TEXT("F1 closes the help widget"),
		Help->OnPreviewKeyDown(FGeometry(), MakeFrontEndKeyEvent(EKeys::F1))
			.IsEventHandled());
	TestEqual(TEXT("F1 executes one close"), CloseCount, 1);
	Help->OnPreviewKeyDown(FGeometry(), MakeFrontEndKeyEvent(EKeys::H));
	TestEqual(TEXT("rebound primary ShowHelp key closes UI-only help"), CloseCount, 2);
	Help->OnPreviewKeyDown(FGeometry(), MakeFrontEndKeyEvent(EKeys::J));
	TestEqual(TEXT("rebound secondary ShowHelp key closes UI-only help"), CloseCount, 3);
	Help->OnPreviewKeyDown(FGeometry(), MakeFrontEndKeyEvent(EKeys::Escape));
	TestEqual(TEXT("Escape closes help too"), CloseCount, 4);
	Help->OnPreviewKeyDown(FGeometry(), MakeFrontEndKeyEvent(EKeys::Gamepad_FaceButton_Right));
	TestEqual(TEXT("B closes help"), CloseCount, 5);
	Help->OnPreviewKeyDown(FGeometry(), MakeFrontEndKeyEvent(EKeys::Gamepad_Special_Left));
	TestEqual(TEXT("View closes help"), CloseCount, 6);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndCatalogSpec,
	"WoT.FrontEnd.MapCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndCatalogSpec::RunTest(const FString&)
{
	const TArray<FWoTMapAssetCandidate> Candidates = {
		{TEXT("/Game/WoT/Maps/Battle2_03"), TEXT("Battle2_03"), true, true},
		{TEXT("/Game/WoT/Maps/Arena_11"), TEXT("Arena_11"), true, true},
		{TEXT("/Game/WoT/Maps/Arena_02"), TEXT("Arena_02"), true, true},
		// A repeated Asset Registry record must not create a second menu row.
		{TEXT("/Game/WoT/Maps/Arena_02"), TEXT("Arena_02"), true, true},
		// Every remaining candidate violates one catalog condition.
		{TEXT("/Game/WoT/Maps/Arena_01"), TEXT("Arena_01"), false, true},
		{TEXT("/Game/WoT/Maps/Battle2_01"), TEXT("Battle2_01"), true, false},
		{TEXT("/Game/WoT/Maps/arena_03"), TEXT("arena_03"), true, true},
		{TEXT("/Game/Other/Arena_04"), TEXT("Arena_04"), true, true},
		{TEXT("/Game/WoT/Maps/WoTFrontEnd"), TEXT("WoTFrontEnd"), true, true},
		{TEXT("/Game/WoT/Maps/Arena_05"), TEXT("WrongAssetName"), true, true}
	};

	const TArray<FWoTFrontEndMapEntry> Maps =
		FWoTFrontEndMapCatalog::FilterAndSort(Candidates);
	TestEqual(TEXT("catalog keeps each playable top-level world once"), Maps.Num(), 3);
	if (Maps.Num() == 3)
	{
		TestEqual(TEXT("catalog sorts first Arena by short name"),
			Maps[0].ShortName, FString(TEXT("Arena_02")));
		TestEqual(TEXT("catalog sorts second Arena by short name"),
			Maps[1].ShortName, FString(TEXT("Arena_11")));
		TestEqual(TEXT("Battle follows the Arena names"),
			Maps[2].ShortName, FString(TEXT("Battle2_03")));
		TestFalse(TEXT("Arena entry is classified as Arena"), Maps[0].bIsBattle);
		TestTrue(TEXT("Battle entry is classified as Battle"), Maps[2].bIsBattle);
	}
	// Guarded builds list the adopted descriptor identities under the same
	// retail package convention the host travel URL and shell routing use.
	const FString Identities[] = {
		TEXT("Battle2_03"), TEXT("Arena_11"), TEXT("Arena_02"), TEXT("Arena_02"),
		TEXT("arena_03"), TEXT("WoTFrontEnd"), TEXT("Entry")};
	const TArray<FWoTFrontEndMapEntry> FromDescriptors =
		FWoTFrontEndMapCatalog::FromIdentities(MakeArrayView(Identities));
	TestEqual(TEXT("descriptor identities produce the same filtered catalog"),
		FromDescriptors.Num(), 3);
	if (FromDescriptors.Num() == 3)
	{
		TestEqual(TEXT("descriptor entries carry the retail package name"),
			FromDescriptors[0].PackageName, FString(TEXT("/Game/WoT/Maps/Arena_02")));
		TestTrue(TEXT("descriptor host request is playable"),
			FWoTFrontEndModel::IsPlayableMap(FromDescriptors[2].PackageName)
			&& FromDescriptors[2].bIsBattle);
		FWoTFrontEndMapEntry PickerEntry = FromDescriptors[0];
		PickerEntry.DisplayName = TEXT("Keep on the Borderlands (B2)");
		TestEqual(TEXT("host picker uses package ShortName rather than advertised title"),
			SWoTFrontEndRoot::HostMapPickerTextForTesting(&PickerEntry).ToString(),
			FString(TEXT("Arena_02")));
	}
	// The shell route is only travelable when its content root is mounted;
	// the engine's CanServerTravel rejects an unmounted long package name.
	TestTrue(TEXT("neutral-shell root resolves after the mount guarantee"),
		WoTEnsureNeutralShellMountPoint()
		&& FPackageName::IsValidLongPackageName(
			TEXT("/WoTNeutralShells/WoTMapShell01"), true));
	// The retail entry world shares the cache's World/BSP kinds: a generation
	// holding 19 maps plus the front-end world must still reconcile with the
	// 19 authenticated descriptors once the front-end pair is set aside, and
	// must not reconcile when nothing accounts for the extra world.
	TestTrue(TEXT("nineteen maps plus the attested front-end pair reconcile"),
		WoTWorldCensusReconciles(20, 20, 19, 19, 1, 1));
	TestFalse(TEXT("an unaccounted extra world fails the census"),
		WoTWorldCensusReconciles(20, 20, 19, 19, 0, 0));
	TestFalse(TEXT("a missing map fails the census"),
		WoTWorldCensusReconciles(19, 19, 19, 19, 1, 1));
	TestTrue(TEXT("a generation without a front-end world still reconciles"),
		WoTWorldCensusReconciles(19, 19, 19, 19, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlayerNameSanitizerSpec,
	"WoT.FrontEnd.PlayerName.Sanitizer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPlayerNameSanitizerSpec::RunTest(const FString&)
{
	FString Controls = TEXT("A");
	Controls.AppendChar(TEXT('\t'));
	Controls += TEXT("B");
	Controls.AppendChar(TEXT('\n'));
	Controls += TEXT("C");
	TestEqual(TEXT("control characters are stripped"),
		FWoTFrontEndModel::SanitizePlayerName(Controls), FString(TEXT("ABC")));
	TestEqual(TEXT("URL and serialization delimiters are stripped"),
		FWoTFrontEndModel::SanitizePlayerName(TEXT("A?B#C=D\"E")),
		FString(TEXT("ABCDE")));
	TestEqual(TEXT("interior spaces survive and edge spaces are trimmed"),
		FWoTFrontEndModel::SanitizePlayerName(TEXT("  Two Rivers  ")),
		FString(TEXT("Two Rivers")));
	FString NonASCII = TEXT("Lan");
	NonASCII.AppendChar(static_cast<TCHAR>(0x00e9));
	NonASCII += TEXT("Player");
	TestEqual(TEXT("non-ASCII characters are stripped"),
		FWoTFrontEndModel::SanitizePlayerName(NonASCII),
		FString(TEXT("LanPlayer")));
	TestEqual(TEXT("names are truncated to retail's twenty characters"),
		FWoTFrontEndModel::SanitizePlayerName(TEXT("1234567890123456789012345")),
		FString(TEXT("12345678901234567890")));
	TestEqual(TEXT("truncation cannot leave a trailing space"),
		FWoTFrontEndModel::SanitizePlayerName(TEXT("1234567890123456789 rest")),
		FString(TEXT("1234567890123456789")));
	TestTrue(TEXT("an empty name remains unset"),
		FWoTFrontEndModel::SanitizePlayerName(FString()).IsEmpty());
	TestTrue(TEXT("retail's underscore sentinel remains unset"),
		FWoTFrontEndModel::SanitizePlayerName(TEXT("_")).IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalPlayerNicknameSpec,
	"WoT.FrontEnd.PlayerName.ResolveNickname",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLocalPlayerNicknameSpec::RunTest(const FString&)
{
	TestEqual(TEXT("a preferred player name wins"),
		UWoTLocalPlayer::ResolveNickname(TEXT("Aviendha"), TEXT("DESKTOP-PC")),
		FString(TEXT("Aviendha")));
	TestEqual(TEXT("an empty preference uses the computer-name default"),
		UWoTLocalPlayer::ResolveNickname(FString(), TEXT("DESKTOP-PC")),
		FString(TEXT("DESKTOP-PC")));
	TestEqual(TEXT("a preferred name is sanitized on the way out"),
		UWoTLocalPlayer::ResolveNickname(TEXT("  Rand?al'Thor  "), TEXT("DESKTOP-PC")),
		FString(TEXT("Randal'Thor")));

	// The default is the computer name, not the Null subsystem's
	// "<hostname>-<LoginId>" nickname that showed as "DESKTOP-4KQ8ZT2-51E0".
	TestEqual(TEXT("a Windows computer name is used as is"),
		UWoTLocalPlayer::NormalizeComputerName(TEXT("DESKTOP-4KQ8ZT2")),
		FString(TEXT("DESKTOP-4KQ8ZT2")));
	TestEqual(TEXT("a macOS host name drops its .local suffix"),
		UWoTLocalPlayer::NormalizeComputerName(TEXT("Robins-MacBook-Pro.local")),
		FString(TEXT("Robins-MacBook-Pro")));
	TestEqual(TEXT("a long computer name is cut to twenty characters"),
		UWoTLocalPlayer::NormalizeComputerName(TEXT("WORKSTATION-OF-THE-AMYRLIN-SEAT")),
		FString(TEXT("WORKSTATION-OF-THE-A")));
	TestEqual(TEXT("an empty or unusable computer name falls back to Player"),
		UWoTLocalPlayer::NormalizeComputerName(TEXT("???")),
		FString(TEXT("Player")));
	TestFalse(TEXT("the live default is never empty"),
		UWoTLocalPlayer::DefaultPlayerName().IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalPlayerFriendlyNameSpec,
	"WoT.FrontEnd.PlayerName.FriendlyComputerName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLocalPlayerFriendlyNameSpec::RunTest(const FString&)
{
	// The macOS Sharing name players know beats the gethostname form; the
	// shared sanitizer drops its curly apostrophe.
	TestEqual(TEXT("a friendly computer name is preferred, then sanitized"),
		UWoTLocalPlayer::NormalizeComputerName(UWoTLocalPlayer::PreferComputerName(
			TEXT("Robin’s MacBook Pro"), TEXT("Robins-MacBook-Pro.local"))),
		FString(TEXT("Robins MacBook Pro")));
	TestEqual(TEXT("no friendly name falls back to the host name"),
		UWoTLocalPlayer::NormalizeComputerName(UWoTLocalPlayer::PreferComputerName(
			FString(), TEXT("Robins-MacBook-Pro.local"))),
		FString(TEXT("Robins-MacBook-Pro")));
	TestEqual(TEXT("a blank friendly name falls back to the host name"),
		UWoTLocalPlayer::PreferComputerName(TEXT("   "), TEXT("HOST-NAME")),
		FString(TEXT("HOST-NAME")));
	TestEqual(TEXT("a wholly non-ASCII friendly name falls back to the host name"),
		UWoTLocalPlayer::NormalizeComputerName(UWoTLocalPlayer::PreferComputerName(
			TEXT("田中のコンピュータ"),
			TEXT("tanaka-mac.local"))),
		FString(TEXT("tanaka-mac")));
#if PLATFORM_MAC
	// Live: the system store answers, and the default follows the Sharing name.
	const FString Friendly = WoTMacFriendlyComputerName();
	TestFalse(TEXT("the system store returns a Sharing computer name"),
		Friendly.IsEmpty());
	TestEqual(TEXT("the live Mac default prefers the Sharing name"),
		UWoTLocalPlayer::DefaultPlayerName(),
		UWoTLocalPlayer::NormalizeComputerName(UWoTLocalPlayer::PreferComputerName(
			Friendly, FPlatformProcess::ComputerName())));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndViewModelSpec,
	"WoT.FrontEnd.ViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndViewModelSpec::RunTest(const FString&)
{
	FWoTFrontEndViewModel Model;
	TestEqual(TEXT("front end starts on Main"),
		static_cast<int32>(Model.GetScreen()),
		static_cast<int32>(EWoTFrontEndScreen::Main));

	Model.ShowPlayerConfig(EWoTPlayerFaction::Hound, TEXT("Saved Name"),
		TEXT("DESKTOP-PC"), EWoTPlayerSkinColor::Red);
	TestEqual(TEXT("Settings starts from the saved faction"),
		static_cast<int32>(Model.GetPendingFaction()),
		static_cast<int32>(EWoTPlayerFaction::Hound));
	TestEqual(TEXT("Settings starts from the saved player name"),
		Model.GetPendingPlayerName(), FString(TEXT("Saved Name")));
	TestEqual(TEXT("Settings starts from the saved skin color"),
		static_cast<int32>(Model.GetPendingSkinColor()),
		static_cast<int32>(EWoTPlayerSkinColor::Red));
	Model.SelectPendingFaction(EWoTPlayerFaction::Whitecloak);
	Model.CyclePendingSkinColor(1);
	TestEqual(TEXT("skin cycle follows the retail color order"),
		static_cast<int32>(Model.GetPendingSkinColor()),
		static_cast<int32>(EWoTPlayerSkinColor::Base));
	Model.SelectPendingPlayerName(TEXT("Unsaved Name"));
	Model.CancelPlayerConfig();
	TestEqual(TEXT("Settings Back returns to Main"),
		static_cast<int32>(Model.GetScreen()),
		static_cast<int32>(EWoTFrontEndScreen::Main));
	Model.ShowPlayerConfig(EWoTPlayerFaction::Hound, TEXT("Saved Name"),
		TEXT("DESKTOP-PC"), EWoTPlayerSkinColor::Red);
	TestEqual(TEXT("Settings Back did not retain an unsaved selection"),
		static_cast<int32>(Model.GetPendingFaction()),
		static_cast<int32>(EWoTPlayerFaction::Hound));
	TestEqual(TEXT("Settings Back did not retain an unsaved player name"),
		Model.GetPendingPlayerName(), FString(TEXT("Saved Name")));
	TestEqual(TEXT("Settings Back did not retain an unsaved skin color"),
		static_cast<int32>(Model.GetPendingSkinColor()),
		static_cast<int32>(EWoTPlayerSkinColor::Red));
	Model.SelectPendingFaction(EWoTPlayerFaction::Forsaken);
	Model.CyclePendingSkinColor(-1);
	Model.SelectPendingPlayerName(TEXT("  Egwene?# al'Vere  "));
	const FWoTPlayerConfig Saved = Model.SavePlayerConfig();
	TestEqual(TEXT("Settings Save returns the selected faction"),
		static_cast<int32>(Saved.Faction),
		static_cast<int32>(EWoTPlayerFaction::Forsaken));
	TestEqual(TEXT("Settings Save returns the selected skin color"),
		static_cast<int32>(Saved.SkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Green));
	TestEqual(TEXT("Settings Save returns the sanitized player name"),
		Saved.PlayerName, FString(TEXT("Egwene al'Vere")));
	Model.ShowPlayerConfig(EWoTPlayerFaction::Hound, FString(),
		TEXT("DESKTOP-PC"));
	TestEqual(TEXT("an unset name displays the current platform nickname"),
		Model.GetPendingPlayerName(), FString(TEXT("DESKTOP-PC")));
	Model.SelectPendingPlayerName(TEXT("DESKTOP-PC"));
	TestTrue(TEXT("saving the platform nickname keeps the preference unset"),
		Model.SavePlayerConfig().PlayerName.IsEmpty());
	Model.ShowPlayerConfig(EWoTPlayerFaction::Hound, FString(),
		TEXT("DESKTOP-PC"));
	Model.SelectPendingPlayerName(TEXT("Two"));
	Model.SelectPendingPlayerName(Model.GetPendingPlayerName() + TEXT(" "));
	TestEqual(TEXT("a trailing space remains available during incremental input"),
		Model.GetPendingPlayerName(), FString(TEXT("Two ")));
	Model.SelectPendingPlayerName(Model.GetPendingPlayerName() + TEXT("Rivers"));
	TestEqual(TEXT("incremental typing can produce a spaced name"),
		Model.SavePlayerConfig().PlayerName, FString(TEXT("Two Rivers")));
	Model.ShowPlayerConfig(EWoTPlayerFaction::Hound, FString(),
		TEXT("DESKTOP-PC"));
	Model.SelectPendingPlayerName(TEXT("_"));
	TestEqual(TEXT("the underscore sentinel remains editable during input"),
		Model.GetPendingPlayerName(), FString(TEXT("_")));
	Model.SelectPendingPlayerName(Model.GetPendingPlayerName() + TEXT("A"));
	TestEqual(TEXT("a lone sentinel can grow into a valid name"),
		Model.SavePlayerConfig().PlayerName, FString(TEXT("_A")));

	Model.ShowPlay(TArray<FWoTFrontEndMapEntry>());
	TestFalse(TEXT("empty catalog disables Host"), Model.CanHost());
	TestTrue(TEXT("empty catalog explains why Host is disabled"),
		Model.GetStatusText().Contains(TEXT("No playable maps")));
	TestTrue(TEXT("Play Back works while idle"), Model.BackToMain());

	const TArray<FWoTFrontEndMapEntry> Maps = {
		{TEXT("/Game/WoT/Maps/Arena_02"), TEXT("Arena_02"), false},
		{TEXT("/Game/WoT/Maps/Battle3_01"), TEXT("Battle3_01"), true},
		{TEXT("/Game/WoT/Maps/Battle3_02"), TEXT("Battle3_02"), true},
		{TEXT("/Game/WoT/Maps/Battle2_03"), TEXT("Battle2_03"), true}
	};
	Model.ShowPlay(Maps);
	TestTrue(TEXT("non-empty catalog enables Host"), Model.CanHost());
	TestEqual(TEXT("Arena selection uses the authenticated Arena heading id"),
		Model.HostTitleLocalizationId(),
		FName(TEXT("menuStartArenaServer.MenuTitle")));
	TestEqual(TEXT("first map selects Arena defaults"),
		Model.GetHostRequest().FragLimit, 20);
	Model.SetServerName(TEXT("Keep This Name"));
	Model.SetLAN(false);
	Model.SetFragLimit(77);
	TestTrue(TEXT("map selection accepts a catalog package"),
		Model.SelectMap(TEXT("/Game/WoT/Maps/Battle3_01")));
	TestEqual(TEXT("Citadel selection uses the authenticated Battle heading id"),
		Model.HostTitleLocalizationId(),
		FName(TEXT("menuStartBattleServer.MenuTitle")));
	TestEqual(TEXT("Battle selection derives its frag default"),
		Model.GetHostRequest().FragLimit, 0);
	TestEqual(TEXT("Battle selection derives its time default"),
		Model.GetHostRequest().TimeLimit, 0);
	TestEqual(TEXT("selecting a three-citadel map applies the retail goal"),
		Model.GetHostRequest().SealGoal, 3);
	TestEqual(TEXT("selecting a three-citadel map clamps the budget"),
		Model.GetHostRequest().SealBudget, 2);
	TestEqual(TEXT("map changes preserve server name"),
		Model.GetHostRequest().ServerName, FString(TEXT("Keep This Name")));
	TestFalse(TEXT("map changes preserve LAN selection"),
		Model.GetHostRequest().bLAN);
	Model.SetSealGoal(4);
	Model.SetSealBudget(3);
	TestTrue(TEXT("same-count Battle map selection succeeds"),
		Model.SelectMap(TEXT("/Game/WoT/Maps/Battle3_02")));
	TestEqual(TEXT("same-count Battle map keeps the host seal goal"),
		Model.GetHostRequest().SealGoal, 4);
	TestEqual(TEXT("same-count Battle map keeps the host seal budget"),
		Model.GetHostRequest().SealBudget, 3);
	TestTrue(TEXT("switching to a two-citadel Battle map succeeds"),
		Model.SelectMap(TEXT("/Game/WoT/Maps/Battle2_03")));
	TestEqual(TEXT("three to two citadels applies goal 2"),
		Model.GetHostRequest().SealGoal, 2);
	TestEqual(TEXT("three to two citadels clamps budget to 1"),
		Model.GetHostRequest().SealBudget, 1);

	TestTrue(TEXT("Refresh enters the searching state"), Model.BeginSearch());
	TestFalse(TEXT("another search is refused"), Model.BeginSearch());
	TestTrue(TEXT("a running search leaves Host enabled"), Model.CanHost());
	// Back, Host and Direct Connect cut a search short (Play auto-fetches on
	// entry): see WoT.FrontEnd.BackDuringSearch and SwitchDuringSearch.
	FWoTSessionListEntry Session;
	Session.SearchGeneration = 41;
	Session.ResultIndex = 7;
	Session.ServerName = TEXT("Snapshot Host");
	const TArray<FWoTSessionListEntry> Sessions = {Session};
	Model.FinishSearch(true, Sessions);
	TestEqual(TEXT("search completion unlocks actions"),
		static_cast<int32>(Model.GetOperation()),
		static_cast<int32>(EWoTFrontEndAsyncOperation::None));
	TestEqual(TEXT("snapshot row retains its generation"),
		Model.GetSessions()[0].SearchGeneration, uint64(41));
	TestEqual(TEXT("snapshot row retains its raw-result index"),
		Model.GetSessions()[0].ResultIndex, 7);
	TestTrue(TEXT("successful search reports its result count"),
		Model.GetStatusText().Contains(TEXT("Found 1")));

	TestTrue(TEXT("Host enters its async state"), Model.BeginHost());
	TestFalse(TEXT("progress status is not an error"), Model.IsStatusError());
	Model.FinishHost(false);
	TestEqual(TEXT("Host failure unlocks actions"),
		static_cast<int32>(Model.GetOperation()),
		static_cast<int32>(EWoTFrontEndAsyncOperation::None));
	TestTrue(TEXT("Host failure is visible"),
		Model.GetStatusText().Contains(TEXT("Could not create")));
	TestFalse(TEXT("Host failure stays a short player message"),
		Model.GetStatusText().Contains(TEXT("=")));
	TestTrue(TEXT("Host failure is flagged as an error"), Model.IsStatusError());
	TestTrue(TEXT("Join enters its async state"),
		Model.BeginJoin(TEXT("Snapshot Host")));
	Model.FinishJoin(false);
	TestTrue(TEXT("Join failure is visible"),
		Model.GetStatusText().Contains(TEXT("Could not join")));
	TestTrue(TEXT("a second Join enters its async state"),
		Model.BeginJoin(TEXT("Snapshot Host")));
	Model.FinishJoin(true);
	TestEqual(TEXT("OSS Join acceptance enters Connecting"),
		static_cast<int32>(Model.GetOperation()),
		static_cast<int32>(EWoTFrontEndAsyncOperation::Connecting));
	TestFalse(TEXT("Connecting keeps Back disabled"), Model.BackToMain());
	TestFalse(TEXT("Connecting keeps Refresh disabled"), Model.BeginSearch());
	Model.ReportConnectionFailure(TEXT("Connection failed: timed out"));
	TestEqual(TEXT("network failure restores idle actions"),
		static_cast<int32>(Model.GetOperation()),
		static_cast<int32>(EWoTFrontEndAsyncOperation::None));
	TestTrue(TEXT("network failure reason is visible"),
		Model.GetStatusText().Contains(TEXT("timed out")));
	FWoTFrontEndViewModel ReturnedMenuModel;
	ReturnedMenuModel.ShowPlay(Maps);
	ReturnedMenuModel.ReportConnectionFailure(TEXT("Connection failed: rejected"));
	TestTrue(TEXT("a recreated menu is idle after retained failure recovery"),
		ReturnedMenuModel.CanStartAsyncAction());
	TestTrue(TEXT("a recreated menu can navigate after retained failure recovery"),
		ReturnedMenuModel.BackToMain());
	FWoTFrontEndViewModel InviteModel;
	InviteModel.ShowPlay(Maps);
	InviteModel.FinishJoin(true);
	TestEqual(TEXT("overlay Join acceptance also enters Connecting"),
		static_cast<int32>(InviteModel.GetOperation()),
		static_cast<int32>(EWoTFrontEndAsyncOperation::Connecting));

	TestTrue(TEXT("direct connect enters Connecting"), Model.BeginDirectConnect());
	TestFalse(TEXT("another direct connect is refused while Connecting"),
		Model.BeginDirectConnect());
	Model.RejectDirectConnect();
	TestEqual(TEXT("a rejected direct address restores idle actions"),
		static_cast<int32>(Model.GetOperation()),
		static_cast<int32>(EWoTFrontEndAsyncOperation::None));
	TestTrue(TEXT("a rejected direct address explains its accepted syntax"),
		Model.GetStatusText().Contains(TEXT("valid host name")));

	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FWoTFrontEndTests_WaitForSessionCompletion,
	TSharedRef<FWoTFrontEndTests_SessionState>, State,
	FAutomationTestBase*, Test);

bool FWoTFrontEndTests_WaitForSessionCompletion::Update()
{
	if (State->SearchCompletionCount == 0
		&& FPlatformTime::Seconds() < State->Deadline)
	{
		return false;
	}

	if (State->SearchCompletionCount == 0)
	{
		Test->AddError(TEXT("Null OSS Find did not invoke the completion delegate"));
		WoTFrontEndTests_DestroyWorld(State->World);
		return true;
	}

	Test->TestEqual(TEXT("Null OSS Find completed exactly once"),
		State->SearchCompletionCount, 1);
	Test->TestTrue(TEXT("Null OSS Find completed successfully"), State->bSearchSucceeded);
	Test->TestEqual(TEXT("delegate snapshot matches the stored snapshot"),
		State->SearchResults.Num(), State->Sessions->GetSearchResults().Num());
	for (int32 Index = 0; Index < State->SearchResults.Num(); ++Index)
	{
		const FWoTSessionListEntry& Entry = State->SearchResults[Index];
		Test->TestEqual(FString::Printf(TEXT("snapshot %d retains raw result index"), Index),
			Entry.ResultIndex, Index);
		Test->TestTrue(FString::Printf(TEXT("snapshot %d carries a search generation"), Index),
			Entry.SearchGeneration != 0);
		Test->TestTrue(FString::Printf(TEXT("snapshot %d has valid player counts"), Index),
			Entry.CurrentPlayers >= 0 && Entry.CurrentPlayers <= Entry.MaxPlayers);
	}

	WoTFrontEndTests_DestroyWorld(State->World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndPendingHostShutdownSpec,
	"WoT.ZSession.PendingHostShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndPendingHostShutdownSpec::RunTest(const FString&)
{
	UWorld* World = WoTFrontEndTests_CreateWorld();
	if (!TestNotNull(TEXT("pending-create shutdown world exists"), World))
		return false;
	UWoTGameInstance* GameInstance = Cast<UWoTGameInstance>(World->GetGameInstance());
	UWoTSessionSubsystem* Sessions = GameInstance
		? GameInstance->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	const IOnlineSubsystem* OSS = Online::GetSubsystem(World);
	if (!TestNotNull(TEXT("pending-create shutdown subsystem exists"), Sessions)
		|| !TestNotNull(TEXT("pending-create shutdown OSS exists"), OSS))
	{
		WoTFrontEndTests_DestroyWorld(World);
		return false;
	}
	if (OSS->GetSubsystemName() != FName(TEXT("NULL")))
	{
		AddInfo(TEXT("Skipping pending-create shutdown seam outside Null OSS"));
		WoTFrontEndTests_DestroyWorld(World);
		return true;
	}
	const IOnlineSessionPtr ExactSessions = OSS->GetSessionInterface();
	if (!TestTrue(TEXT("pending-create shutdown has exact session interface"),
		ExactSessions.IsValid()))
	{
		WoTFrontEndTests_DestroyWorld(World);
		return false;
	}

	TFunction<bool()> DelayedCreate;
	TFunction<bool()> DelayedDestroy;
	UWoTSessionSubsystem::SetDetachedDestroyDispatchHookForTesting(
		[&DelayedDestroy](TFunction<bool()>&& Dispatch)
		{
			DelayedDestroy = MoveTemp(Dispatch);
			return true;
		});
	ON_SCOPE_EXIT
	{
		UWoTSessionSubsystem::SetDetachedDestroyDispatchHookForTesting({});
	};
	Sessions->SetCreateSessionDispatchHookForTesting(
		[&DelayedCreate](TFunction<bool()>&& Dispatch)
		{
			DelayedCreate = MoveTemp(Dispatch);
			return true;
		});
	TestTrue(TEXT("pending create is accepted before subsystem shutdown"),
		Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	TestTrue(TEXT("pending provider dispatch survives only in the test harness"),
		static_cast<bool>(DelayedCreate));
	// Shut down the subsystem while its provider is still alive. Destroying the
	// entire world/OSS before invoking a captured provider operation would call
	// into a provider whose lifetime has already ended, which is not an OSS
	// completion scenario.
	Sessions->Deinitialize();
	ExactSessions->OnCreateSessionCompleteDelegates.Broadcast(
		FName(TEXT("UnrelatedSession")), /*bOk*/ true);
	if (DelayedCreate)
	{
		TestTrue(TEXT("provider may complete after subsystem shutdown"),
			DelayedCreate());
	}
	TestTrue(TEXT("late success transfers ownership to detached destroy"),
		static_cast<bool>(DelayedDestroy));
	ExactSessions->OnDestroySessionCompleteDelegates.Broadcast(
		FName(TEXT("UnrelatedSession")), /*bOk*/ false);
	TestTrue(TEXT("unrelated destroy completion cannot disarm cleanup"),
		ExactSessions->GetNamedSession(NAME_GameSession) != nullptr);
	ExactSessions->OnDestroySessionCompleteDelegates.Broadcast(
		NAME_GameSession, /*bOk*/ false);
	TestTrue(TEXT("detached shutdown cleanup removes a late named session"),
		ExactSessions->GetNamedSession(NAME_GameSession) == nullptr);
	WoTFrontEndTests_DestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndRootCompositionSpec,
	"WoT.FrontEnd.RootComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndRootCompositionSpec::RunTest(const FString&)
{
	TSharedRef<SWoTFrontEndRoot> UnavailableRoot =
		SNew(SWoTFrontEndRoot).PlayerController(nullptr);
	FChildren* RootChildren = UnavailableRoot->GetChildren();
	if (!TestNotNull(TEXT("front-end root exposes its compound child"), RootChildren))
	{
		return false;
	}
	if (!TestEqual(TEXT("front-end root has one compound child"),
		RootChildren->Num(), 1))
	{
		return false;
	}

	// Controller sub-project 3: the root's one child is the transparent focus
	// outline layer around a column of the content host and the controller
	// prompt strip (collapsed, with no prompts, while nothing is presented).
	const TSharedRef<SWidget> LayerChild = RootChildren->GetChildAt(0);
	if (!TestEqual(TEXT("front-end root child is the transparent focus outline layer"),
		LayerChild->GetType(), FName(TEXT("SWoTFocusOutlineLayer"))))
	{
		return false;
	}
	FChildren* LayerChildren = LayerChild->GetChildren();
	if (!TestTrue(TEXT("the layer holds one overlay"),
		LayerChildren != nullptr && LayerChildren->Num() == 1))
	{
		return false;
	}
	FChildren* ColumnChildren = LayerChildren->GetChildAt(0)->GetChildren();
	if (!TestTrue(TEXT("the overlay holds the content host and the prompt strip"),
		ColumnChildren != nullptr && ColumnChildren->Num() == 2))
	{
		return false;
	}
	TestEqual(TEXT("the footer is the controller prompt strip"),
		ColumnChildren->GetChildAt(1)->GetType(), FName(TEXT("SWoTButtonPrompts")));
	FWoTControllerUIInput::SetLastInputForTesting(EWoTLastInput::Controller);
	TestTrue(TEXT("the prompt strip shows nothing while nothing is presented"),
		StaticCastSharedRef<SWoTButtonPrompts>(ColumnChildren->GetChildAt(1))
			->GetShownPrompts().IsEmpty());
	FWoTControllerUIInput::ResetForTesting();
	const TSharedRef<SWidget> RootChild = ColumnChildren->GetChildAt(0);
	if (!TestEqual(TEXT("front-end root child is a transparent content host"),
		RootChild->GetType(), FName(TEXT("SBox"))))
	{
		return false;
	}
	FChildren* HostChildren = RootChild->GetChildren();
	TestNotNull(TEXT("transparent host exposes its projected content"), HostChildren);
	if (HostChildren)
	{
		TestEqual(TEXT("unavailable authenticated dependencies fail closed inside the transparent host"),
			HostChildren->Num(), 1);
		if (HostChildren->Num() == 1)
		{
			TestEqual(TEXT("fail-closed host contains no background, frame, or title widget"),
				HostChildren->GetChildAt(0)->GetType(),
				FName(TEXT("SNullWidgetContent")));
		}
	}

	TSharedRef<FWoTFrontEndPresentationFeedbackRecorder> Feedback =
		MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	TSharedRef<FWoTFrontEndPresentationAuthority> Authority =
		MakeInjectedPresentationAuthority(Feedback);
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority);
	Root->Tick(FGeometry::MakeRoot(
		FVector2D(1920.0f, 1080.0f), FSlateLayoutTransform()), 0.0, 0.0f);
	TestTrue(TEXT("injected authenticated presentation exposes the real root"),
		Authority->IsReady() && Root->GetVisibility().IsVisible());
	TArray<TSharedRef<SWidget>> TextWidgets;
	for (int32 EntryIndex = 0; EntryIndex < 4; ++EntryIndex)
	{
		const TSharedPtr<SWoTMenuButton> MenuButton =
			Root->MainMenuButtonForTesting(EntryIndex);
		if (MenuButton.IsValid())
		{
			CollectWidgetsOfType(MenuButton.ToSharedRef(),
				FName(TEXT("STextBlock")), TextWidgets);
		}
	}
	TArray<FString> MainLabels;
	for (const TSharedRef<SWidget>& Widget : TextWidgets)
	{
		const TSharedRef<STextBlock> Text =
			StaticCastSharedRef<STextBlock>(Widget);
		MainLabels.Add(Text->GetText().ToString());
		TestTrue(TEXT("main labels use the captured 30-point presentation face"),
			Text->GetFont().Size == 30);
	}
	const TArray<FString> ExpectedMainLabels = {
		FrontEndFixtureLabel(11, TEXT('M')), FrontEndFixtureLabel(8, TEXT('C')),
		FrontEndFixtureLabel(8, TEXT('H')), FrontEndFixtureLabel(4, TEXT('Q'))};
	TestEqual(TEXT("real main tree contains exactly the four authenticated labels"),
		MainLabels, ExpectedMainLabels);
	TestFalse(TEXT("real main tree contains no invented title"),
		MainLabels.Contains(TEXT("THE WHEEL OF TIME")));
	TArray<TSharedRef<SWidget>> MainFrames;
	for (int32 EntryIndex = 0; EntryIndex < 4; ++EntryIndex)
	{
		const TSharedPtr<SWoTMenuButton> MenuButton =
			Root->MainMenuButtonForTesting(EntryIndex);
		if (MenuButton.IsValid())
		{
			CollectWidgetsOfType(MenuButton.ToSharedRef(),
				FName(TEXT("SBorder")), MainFrames);
		}
	}
	TestEqual(TEXT("real main menu entries remain unframed"), MainFrames.Num(), 0);
	TArray<TSharedRef<SWidget>> BannerFrames;
	if (const TSharedPtr<SWidget> Banner = Root->UpdateBannerWidgetForTesting();
		Banner.IsValid())
	{
		CollectWidgetsOfType(Banner.ToSharedRef(), FName(TEXT("SBorder")),
			BannerFrames);
	}
	TestEqual(TEXT("update banner uses the two-layer browser frame"),
		BannerFrames.Num(), 2);
	TestTrue(TEXT("real main layout uses measured positive geometry"),
		Root->MainMenuListSizeForTesting().X > 0.0f
			&& Root->MainMenuListSizeForTesting().Y > 0.0f
			&& FMath::IsNearlyEqual(Root->MainMenuSafeWidthForTesting(), 1440.0f));

	TestTrue(TEXT("Controls activates the real Controls surface"),
		Root->ActivateMainMenuEntryForTesting(1).IsEventHandled());
	TestTrue(TEXT("Controls is its own screen"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Controls);
	TArray<TSharedRef<SWidget>> SettingsFrames;
	CollectWidgetsOfType(Root, FName(TEXT("SBorder")), SettingsFrames);
	TestTrue(TEXT("controls uses the framed browser chrome"), SettingsFrames.Num() > 0);
	const FKeyEvent Escape(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0);
	TestTrue(TEXT("preview Escape returns Settings to Main"),
		Root->OnPreviewKeyDown(FGeometry(), Escape).IsEventHandled());
	TestTrue(TEXT("preview Escape restored Main"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);
	TestTrue(TEXT("Multiplayer activates the real Play/browser surface"),
		Root->ActivateMainMenuEntryForTesting(0).IsEventHandled());
	TestTrue(TEXT("play surface is active"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	TArray<TSharedRef<SWidget>> PlayFrames;
	CollectWidgetsOfType(Root, FName(TEXT("SBorder")), PlayFrames);
	TestTrue(TEXT("browser/session chrome exists only below the Play panels"),
		PlayFrames.Num() > 0);

	const TConstArrayView<FWoTFrontEndMainMenuEntry> Entries =
		SWoTFrontEndRoot::RetailMainEntries();
	TestEqual(TEXT("main menu exposes exactly four actions"), Entries.Num(), 4);
	const EWoTFrontEndMainMenuAction ExpectedActions[] = {
		EWoTFrontEndMainMenuAction::Multiplayer,
		EWoTFrontEndMainMenuAction::Controls,
		EWoTFrontEndMainMenuAction::Hardware,
		EWoTFrontEndMainMenuAction::Quit
	};
	const FString ExpectedLabels[] = {
		FrontEndFixtureLabel(11, TEXT('M')), FrontEndFixtureLabel(8, TEXT('C')),
		FrontEndFixtureLabel(8, TEXT('H')), FrontEndFixtureLabel(4, TEXT('Q'))
	};
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("entry %d action order"), Index),
			static_cast<int32>(Entries[Index].Action),
			static_cast<int32>(ExpectedActions[Index]));
		TestEqual(FString::Printf(TEXT("entry %d authenticated localization"), Index),
			Authority->Text(Entries[Index].LocalizationId).ToString(),
			ExpectedLabels[Index]);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndPresentationAuthoritySpec,
	"WoT.FrontEnd.PresentationAuthority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndPresentationAuthoritySpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 3);
	FWoTFrontEndStageProductionFixture GenerationA;
	FString Error;
	if (!TestTrue(TEXT("real generation A fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(GenerationA, false, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	const FWoTFrontEndFontOwner* FontOwner =
		GenerationA.Payload->FrontEndFontOwnerForScene();
	if (!TestNotNull(TEXT("real generation A font owner"), FontOwner)) return false;
	FWoTFrontEndSceneMaterializer SceneA;
	auto StagedA = SceneA.Stage(*GenerationA.World, GenerationA.Descriptor,
		*GenerationA.WorldLease, *GenerationA.Payload, *FontOwner, {},
		[]() { return false; });
	if (!TestFalse(TEXT("real generation A scene stages"), StagedA.IsError()))
		return false;
	if (!TestFalse(TEXT("real generation A scene commits"),
		MoveTemp(StagedA).GetValue().Commit().IsError())) return false;
	AWoTFrontEndPlayerController* ControllerA =
		GenerationA.World->SpawnActor<AWoTFrontEndPlayerController>();
	if (!TestNotNull(TEXT("real generation A menu controller"), ControllerA))
		return false;
	TSharedPtr<FWoTFrontEndPresentationAuthority> AuthorityA =
		FWoTFrontEndPresentationAuthority::Create(ControllerA);
	if (!TestTrue(TEXT("production bundle captures generation A"),
		AuthorityA.IsValid() && AuthorityA->IsReady()
			&& AuthorityA->SnapshotIdentity()
				== GenerationA.WorldLease->GetManifestSha256().ToHex())) return false;
	AWoTPlayerController* GameplayControllerA =
		GenerationA.World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("generation A gameplay controller"),
		GameplayControllerA)) return false;
	TSharedPtr<FWoTFrontEndPresentationAuthority> GameplayAuthorityA =
		FWoTFrontEndPresentationAuthority::Create(GameplayControllerA);
	TestTrue(TEXT("gameplay controller creates the exact live presentation authority"),
		GameplayAuthorityA.IsValid() && GameplayAuthorityA->IsReady()
			&& GameplayAuthorityA->SnapshotIdentity()
				== GenerationA.WorldLease->GetManifestSha256().ToHex());
	GameplayAuthorityA.Reset();
	GameplayControllerA->Destroy();
	TSharedRef<SWoTFrontEndRoot> RootA = SNew(SWoTFrontEndRoot)
		.PlayerController(ControllerA)
		.PresentationAuthorityForTesting(AuthorityA);
	RootA->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	const FWoTRequiredMenuFonts* FontsA = AuthorityA->RequiredFonts();
	TestTrue(TEXT("production bundle exposes authenticated F_WOTReg30"),
		FontsA != nullptr && FontsA->Title.FontObject.Get() != nullptr
			&& FontsA->Title.FontObject.Get()
				== FontOwner->ResolveFontObject(FName(TEXT("F_WOTReg30"))));
	TestEqual(TEXT("production bundle exposes authenticated localization"),
		AuthorityA->Text(FName(TEXT("menuMain.MenuList[2]"))).ToString(),
		FString(TEXT("Multiplayer")));
	TestTrue(TEXT("production root is visible before revocation"),
		RootA->GetVisibility().IsVisible());

	TSharedRef<SWindow> GeometryWindow = SNew(SWindow)
		.ClientSize(FVector2D(1024.0f, 768.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.CreateTitleBar(false)
		[RootA];
	FSlateApplication& SlateApp = FSlateApplication::Get();
	const float PreviousApplicationScale = SlateApp.GetApplicationScale();
	SlateApp.AddWindow(GeometryWindow, true);
	struct FProductionGeometryCase
	{
		const TCHAR* Label;
		FVector2D PhysicalViewport;
		float DPIScale;
	};
	const FProductionGeometryCase GeometryCases[] = {
		{TEXT("1024x768"), FVector2D(1024.0f, 768.0f), 1.0f},
		{TEXT("1920x1080"), FVector2D(1920.0f, 1080.0f), 1.0f},
		{TEXT("1920x1200"), FVector2D(1920.0f, 1200.0f), 1.0f},
		{TEXT("3440x1440"), FVector2D(3440.0f, 1440.0f), 1.0f},
		{TEXT("1920x1080 at 200 percent DPI"),
			FVector2D(1920.0f, 1080.0f), 2.0f},
	};
	const TSharedRef<FSlateFontMeasure> FontMeasure =
		SlateApp.GetRenderer()->GetFontMeasureService();
	for (const FProductionGeometryCase& GeometryCase : GeometryCases)
	{
		SlateApp.SetApplicationScale(GeometryCase.DPIScale);
		GeometryWindow->Resize(GeometryCase.PhysicalViewport);
		const FVector2D LocalViewport =
			GeometryCase.PhysicalViewport / GeometryCase.DPIScale;
		const FGeometry RootGeometry = FGeometry::MakeRoot(LocalViewport,
			FSlateLayoutTransform(GeometryCase.DPIScale));
		// Two real layout/paint passes model the normal resize frame followed by
		// the settled frame after ScreenHost has current cached geometry.
		for (int32 LayoutPass = 0; LayoutPass < 2; ++LayoutPass)
		{
			RootA->Tick(RootGeometry, 0.0, 0.0f);
			RootA->SlatePrepass(GeometryCase.DPIScale);
			FHittestGrid HitTestGrid;
			FSlateWindowElementList DrawElements(GeometryWindow);
			const FPaintArgs PaintArgs(&GeometryWindow.Get(), HitTestGrid,
				FVector2f::ZeroVector, FApp::GetCurrentTime(), 0.0f);
			RootA->Paint(PaintArgs, RootGeometry,
				RootGeometry.GetLayoutBoundingRect(), DrawElements, 0,
				FWidgetStyle(), true);
		}

		float ExpectedLabelWidth = 0.0f;
		for (const FWoTFrontEndMainMenuEntry& Entry
			: SWoTFrontEndRoot::RetailMainEntries())
		{
			ExpectedLabelWidth = FMath::Max(ExpectedLabelWidth,
				FontMeasure->Measure(AuthorityA->Text(Entry.LocalizationId),
					FontsA->Title).X);
		}
		const FVector2D ListSize = RootA->MainMenuListSizeForTesting();
		AddInfo(FString::Printf(TEXT("%s geometry actual root=%s local=%s list=%s safe=%g dpi=%g"),
			GeometryCase.Label,
			*RootA->GetCachedGeometry().GetLocalSize().ToString(),
			*LocalViewport.ToString(), *ListSize.ToString(),
			RootA->MainMenuSafeWidthForTesting(), GeometryCase.DPIScale));
		TestTrue(FString::Printf(TEXT("%s production root uses physical/DPI viewport"),
			GeometryCase.Label),
			RootA->GetCachedGeometry().GetLocalSize().Equals(LocalViewport, 1.0f));
		TestTrue(FString::Printf(TEXT("%s list width equals authenticated renderer measurement"),
			GeometryCase.Label),
			FMath::IsNearlyEqual(ListSize.X, ExpectedLabelWidth, 1.0f));
		TestTrue(FString::Printf(TEXT("%s uses centered 4:3 safe width"),
			GeometryCase.Label), FMath::IsNearlyEqual(
				RootA->MainMenuSafeWidthForTesting(),
				FMath::Min(LocalViewport.X, LocalViewport.Y * 4.0f / 3.0f), 1.0f));

		FSlateRect ListBounds;
		bool bHasListBounds = false;
		for (int32 EntryIndex = 0; EntryIndex < 4; ++EntryIndex)
		{
			const TSharedPtr<SWoTMenuButton> Button =
				RootA->MainMenuButtonForTesting(EntryIndex);
			if (!Button.IsValid()) continue;
			const FSlateRect Bounds = Button->GetCachedGeometry().GetLayoutBoundingRect();
			if (!bHasListBounds)
			{
				ListBounds = Bounds;
				bHasListBounds = true;
			}
			else
			{
				ListBounds.Left = FMath::Min(ListBounds.Left, Bounds.Left);
				ListBounds.Top = FMath::Min(ListBounds.Top, Bounds.Top);
				ListBounds.Right = FMath::Max(ListBounds.Right, Bounds.Right);
				ListBounds.Bottom = FMath::Max(ListBounds.Bottom, Bounds.Bottom);
			}
		}
		TestTrue(FString::Printf(TEXT("%s has cached production button geometry"),
			GeometryCase.Label), bHasListBounds);
		if (bHasListBounds)
		{
			const FSlateRect RootBounds =
				RootA->GetCachedGeometry().GetLayoutBoundingRect();
			const FVector2D ListCenter(
				(ListBounds.Left + ListBounds.Right) * 0.5f,
				(ListBounds.Top + ListBounds.Bottom) * 0.5f);
			const FVector2D RootCenter(
				(RootBounds.Left + RootBounds.Right) * 0.5f,
				(RootBounds.Top + RootBounds.Bottom) * 0.5f);
			TestTrue(FString::Printf(TEXT("%s cached list is horizontally centered"),
				GeometryCase.Label), FMath::IsNearlyEqual(
					ListCenter.X, RootCenter.X, 1.0f));
			TestTrue(FString::Printf(TEXT("%s cached list is vertically centered"),
				GeometryCase.Label), FMath::IsNearlyEqual(
					ListCenter.Y, RootCenter.Y, 1.0f));
			const float CachedLayoutScale = RootA->GetCachedGeometry()
				.GetAccumulatedLayoutTransform().GetScale();
			TestTrue(FString::Printf(TEXT("%s cached list width matches computed list"),
				GeometryCase.Label), FMath::IsNearlyEqual(
					(ListBounds.Right - ListBounds.Left) / CachedLayoutScale,
					ListSize.X, 1.0f));
			TestTrue(FString::Printf(TEXT("%s cached list height matches computed list"),
				GeometryCase.Label), FMath::IsNearlyEqual(
					(ListBounds.Bottom - ListBounds.Top) / CachedLayoutScale,
					ListSize.Y, 1.0f));
		}
	}
	SlateApp.SetApplicationScale(PreviousApplicationScale);
	GeometryWindow->RequestDestroyWindow();
	SlateApp.Tick();

	SceneA.RevokeAuthorityForTesting();
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestFalse(TEXT("revocation invalidates the exact presentation bundle"),
		AuthorityA->IsReady());
	TestFalse(TEXT("revocation removes required-font access"),
		AuthorityA->RequiredFonts() != nullptr);
	TestTrue(TEXT("revocation removes localization access"),
		AuthorityA->Text(FName(TEXT("menuMain.MenuList[2]"))).IsEmpty());
	TestFalse(TEXT("revocation removes feedback readiness"),
		AuthorityA->IsReadyForFeedback());
	TestTrue(TEXT("revocation synchronously collapses the captured root"),
		RootA->GetVisibility() == EVisibility::Collapsed);
	TestTrue(TEXT("revocation synchronously tears down the scene"),
		SceneA.ObserveForTesting().ObservableActors == 0
			&& SceneA.ObserveForTesting().AudibleComponents == 0);
	SceneA.Shutdown();
	WoTFrontEndTest_DestroyProductionStageFixture(GenerationA);

	FWoTFrontEndStageProductionFixture GenerationB;
	Error.Empty();
	if (!TestTrue(TEXT("real generation B fixture activates after A"),
		WoTFrontEndTest_BuildProductionStageFixture(GenerationB, false, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	AWoTFrontEndPlayerController* ControllerB =
		GenerationB.World->SpawnActor<AWoTFrontEndPlayerController>();
	TSharedPtr<FWoTFrontEndPresentationAuthority> AuthorityB =
		FWoTFrontEndPresentationAuthority::Create(ControllerB);
	TSharedRef<SWoTFrontEndRoot> RootB = SNew(SWoTFrontEndRoot)
		.PlayerController(ControllerB)
		.PresentationAuthorityForTesting(AuthorityB);
	RootB->Tick(FGeometry::MakeRoot(FVector2D(1920.0f, 1080.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	TestTrue(TEXT("generation B creates a fresh working presentation"),
		AuthorityB.IsValid() && AuthorityB->IsReady()
			&& RootB->GetVisibility().IsVisible());
	TestFalse(TEXT("generation B cannot revive A authority"), AuthorityA->IsReady());
	TestTrue(TEXT("generation B cannot revive A widgets"),
		RootA->GetVisibility() == EVisibility::Collapsed);
	TestTrue(TEXT("generation B retains its exact live lease before teardown"),
		AuthorityB->HasLiveLeaseForTesting());
	TestTrue(TEXT("generation B owns both exact feedback bindings"),
		AuthorityB->HasCapturedFeedbackBindingsForTesting());
	AuthorityB->MoveSelector();
	TestTrue(TEXT("generation B starts an owned feedback component"),
		AuthorityB->ActiveFeedbackComponentCountForTesting() > 0);
	TSharedRef<SBox> TeardownSentinel = SNew(SBox);
	const int32 InitialBoundWidgetCount =
		AuthorityB->BoundWidgetCountForTesting();
	AuthorityB->BindWidget(TeardownSentinel);
	AuthorityB->BindWidget(TeardownSentinel);
	TestEqual(TEXT("duplicate widget binding occupies one live slot"),
		AuthorityB->BoundWidgetCountForTesting(), InitialBoundWidgetCount + 1);
	{
		TSharedPtr<SBox> ExpiredWidget = SNew(SBox);
		AuthorityB->BindWidget(ExpiredWidget.ToSharedRef());
		TestEqual(TEXT("temporary widget binding occupies one live slot"),
			AuthorityB->BoundWidgetCountForTesting(), InitialBoundWidgetCount + 2);
		ExpiredWidget.Reset();
	}
	TSharedRef<SBox> LaterSentinel = SNew(SBox);
	AuthorityB->BindWidget(LaterSentinel);
	TestEqual(TEXT("later binding compacts expired weak widget slots"),
		AuthorityB->BoundWidgetCountForTesting(), InitialBoundWidgetCount + 2);
	TestTrue(TEXT("normal teardown sentinel begins visible"),
		TeardownSentinel->GetVisibility().IsVisible());
	const FString GenerationBCacheRoot = GenerationB.CacheRoot;
	WoTFrontEndTest_DestroyProductionStageFixture(GenerationB);
	TestTrue(TEXT("normal payload teardown executes the subscribed barrier"),
		AuthorityB->DidRunRevocationBarrierForTesting());
	TestTrue(TEXT("normal payload teardown runs the presentation barrier"),
		TeardownSentinel->GetVisibility() == EVisibility::Collapsed);
	TestFalse(TEXT("normal payload teardown revokes retained authority"),
		AuthorityB->IsReady());
	TestFalse(TEXT("normal payload teardown revokes the retained lease state"),
		AuthorityB->HasLiveLeaseForTesting());
	TestFalse(TEXT("normal payload teardown removes exact feedback bindings"),
		AuthorityB->HasCapturedFeedbackBindingsForTesting());
	TestEqual(TEXT("normal payload teardown stops all feedback components"),
		AuthorityB->ActiveFeedbackComponentCountForTesting(), 0);
	TestFalse(TEXT("normal payload teardown releases snapshot directory locks"),
		IFileManager::Get().DirectoryExists(*GenerationBCacheRoot));

	FWoTFrontEndStageProductionFixture GenerationC;
	Error.Empty();
	if (!TestTrue(TEXT("generation C activates after normal B teardown"),
		WoTFrontEndTest_BuildProductionStageFixture(GenerationC, false, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		return false;
	}
	AWoTFrontEndPlayerController* ControllerC =
		GenerationC.World->SpawnActor<AWoTFrontEndPlayerController>();
	TSharedPtr<FWoTFrontEndPresentationAuthority> AuthorityC =
		FWoTFrontEndPresentationAuthority::Create(ControllerC);
	TestTrue(TEXT("generation C creates a fresh working presentation"),
		AuthorityC.IsValid() && AuthorityC->IsReady());
	TestFalse(TEXT("generation C cannot revive normal-teardown B authority"),
		AuthorityB->IsReady());
	TestTrue(TEXT("generation C cannot revive normal-teardown B widgets"),
		RootB->GetVisibility() == EVisibility::Collapsed
			&& TeardownSentinel->GetVisibility() == EVisibility::Collapsed);
	WoTFrontEndTest_DestroyProductionStageFixture(GenerationC);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSlateInputSpec,
	"WoT.FrontEnd.PresentationInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSlateInputSpec::RunTest(const FString&)
{
	// Entering Play auto-fetches: pin the platform path where that search
	// really starts (on a Mac the LAN heads-up would answer first), so Back
	// is exercised against an in-flight search on every host.
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	if (!TestTrue(TEXT("Slate application is initialized"),
		FSlateApplication::IsInitialized())) return false;
#if PLATFORM_MAC
	// Headless (-nullrhi) SWindows have no native NSWindow, so the macOS IME
	// bridge logs an Error on every text-context (de)activation. Engine noise
	// only; the spec's own assertions are unaffected.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, 0);
#endif
	UWorld* World = WoTFrontEndTests_CreateWorld();
	if (!TestNotNull(TEXT("input fixture world"), World)) return false;
	ON_SCOPE_EXIT { WoTFrontEndTests_DestroyWorld(World); };
	AWoTFrontEndPlayerController* Controller =
		World->SpawnActor<AWoTFrontEndPlayerController>();
	if (!TestNotNull(TEXT("input fixture uses a real front-end controller"),
		Controller)) return false;
	TSharedRef<FWoTFrontEndPresentationFeedbackRecorder> Feedback =
		MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	TSharedRef<FWoTFrontEndPresentationAuthority> Authority =
		MakeInjectedPresentationAuthority(Feedback);
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Controller)
		.PresentationAuthorityForTesting(Authority)
		.ActionObserverForTesting_Lambda([Feedback](EWoTFrontEndMainMenuAction)
		{
			Feedback->Events.Add(TEXT("Action"));
		});
	TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(1024.0f, 768.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.CreateTitleBar(false)
		[Root];
	FSlateApplication::Get().AddWindow(Window, true);
	ON_SCOPE_EXIT
	{
		Window->RequestDestroyWindow();
		FSlateApplication::Get().Tick();
	};
	FSlateApplication::Get().Tick();
	Window->SlatePrepass(1.0f);
	Root->Tick(FGeometry::MakeRoot(FVector2D(1024.0f, 768.0f),
		FSlateLayoutTransform()), 0.0, 0.0f);
	Window->SlatePrepass(1.0f);
	FSlateApplication::Get().Tick();
	TestTrue(TEXT("front-end queued focus target is its selected first control"),
		Root->InitialFocusWidget() == Root->MainMenuButtonForTesting(0));

	TSharedPtr<SWoTMenuButton> Hardware = Root->MainMenuButtonForTesting(2);
	if (!TestTrue(TEXT("real main Hardware button is reachable"),
		Hardware.IsValid())) return false;
	const FPointerEvent Hover(0,
		Hardware->GetCachedGeometry().GetAbsolutePosition(),
		Hardware->GetCachedGeometry().GetAbsolutePosition(), TSet<FKey>(),
		FKey(), 0.0f, FModifierKeysState());
	Hardware->OnMouseEnter(Hardware->GetCachedGeometry(), Hover);
	TestEqual(TEXT("real hover changes the one selection model once"),
		Feedback->Events, TArray<FString>{TEXT("MoveSelector")});
	TestTrue(TEXT("hover synchronizes the real Slate focus origin"),
		FSlateApplication::Get().GetKeyboardFocusedWidget() == Hardware);
	TestTrue(TEXT("real pointer down/up activates Hardware"),
		ClickWidgetThroughSlate(Hardware.ToSharedRef()));
	TestTrue(TEXT("Hardware click opens the real Hardware surface"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Hardware);
	TestEqual(TEXT("real hover/click orders Move, Select, action exactly once"),
		Feedback->Events,
		TArray<FString>{TEXT("MoveSelector"), TEXT("Select"), TEXT("Action")});

	auto AssertBackFromFocusedPlayChild =
		[this, &Root, &Feedback, &Authority](const TCHAR* Label,
			const TSharedPtr<SWidget>& Focused, FKey Key)
	{
		if (!TestTrue(FString::Printf(TEXT("%s focus target exists"), Label),
			Focused.IsValid())) return;
		FSlateApplication::Get().SetKeyboardFocus(
			Focused, EFocusCause::SetDirectly);
		const FString Snapshot = Authority->SnapshotIdentity();
		const uint64 Before = Root->ScreenRebuildCountForTesting();
		Feedback->Events.Reset();
		TestTrue(FString::Printf(TEXT("%s preview back is handled"), Label),
			FSlateApplication::Get().ProcessKeyDownEvent(
				MakeFrontEndKeyEvent(Key)));
		Root->Tick(Root->GetCachedGeometry(), 0.0, 0.0f);
		FSlateApplication::Get().Tick();
		TestTrue(FString::Printf(TEXT("%s back returns to Main"), Label),
			Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);
		TestEqual(FString::Printf(TEXT("%s causes one transition"), Label),
			Root->ScreenRebuildCountForTesting(), Before + 1);
		TestTrue(FString::Printf(TEXT("%s back emits no feedback"), Label),
			Feedback->Events.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s preserves presentation snapshot"), Label),
			Authority->SnapshotIdentity(), Snapshot);
		TestTrue(FString::Printf(TEXT("%s restores selected main focus"), Label),
			FSlateApplication::Get().GetKeyboardFocusedWidget()
				== Root->MainMenuButtonForTesting(0));
	};

	TestTrue(TEXT("preview Escape returns Settings before descendants"),
		FSlateApplication::Get().ProcessKeyDownEvent(
			MakeFrontEndKeyEvent(EKeys::Escape)));
	Feedback->Events.Reset();

	Root->ActivateMainMenuEntryForTesting(0);
	TArray<TSharedRef<SWidget>> Edits;
	CollectWidgetsOfType(Root, FName(TEXT("SEditableTextBox")), Edits);
	TestTrue(TEXT("Play exposes server-name and direct-address edit boxes"),
		Edits.Num() >= 2);
	if (Edits.Num() >= 2)
	{
		AssertBackFromFocusedPlayChild(TEXT("server-name edit"), Edits[0],
			EKeys::Escape);
		Root->ActivateMainMenuEntryForTesting(0);
		Edits.Reset();
		CollectWidgetsOfType(Root, FName(TEXT("SEditableTextBox")), Edits);
		AssertBackFromFocusedPlayChild(TEXT("direct-address edit"), Edits.Last(),
			EKeys::Gamepad_FaceButton_Right);
	}

	// The map list is the touch-friendly SWoTListCombo (spec r3 section 2).
	// With its dropdown open, Escape closes only the dropdown and a second
	// Escape backs the screen (owner ruling 2026-11-08, spec r3 [N1]).
	Root->ActivateMainMenuEntryForTesting(0);
	TSharedPtr<SWidget> Combo = Root->MapComboBoxForTesting();
	TSharedPtr<SWoTListCombo> TypedCombo = Root->MapListComboForTesting();
	TArray<TSharedRef<SWidget>> ComboButtons;
	CollectWidgetsOfType(Combo.ToSharedRef(), FName(TEXT("SButton")), ComboButtons);
	TestTrue(TEXT("map combo opens through a real pointer click"),
		ComboButtons.Num() > 0 && ClickWidgetThroughSlate(ComboButtons[0]));
	FSlateApplication::Get().Tick(ESlateTickType::All);
	TestTrue(TEXT("real pointer click leaves map combo popup open"),
		TypedCombo.IsValid() && TypedCombo->IsOpen());
	const TSharedPtr<SWidget> PopupFocus =
		FSlateApplication::Get().GetKeyboardFocusedWidget();
	AddInfo(FString::Printf(TEXT("combo popup focus type=%s same-anchor=%s"),
		PopupFocus.IsValid() ? *PopupFocus->GetType().ToString() : TEXT("<none>"),
		PopupFocus == Combo ? TEXT("true") : TEXT("false")));
	TestTrue(TEXT("open map combo genuinely focuses its popup list"),
		PopupFocus.IsValid() && PopupFocus != Combo && TypedCombo.IsValid()
			&& PopupFocus == TypedCombo->GetPopupWidget());
	const FString ComboSnapshot = Authority->SnapshotIdentity();
	const uint64 BeforeComboBack = Root->ScreenRebuildCountForTesting();
	Feedback->Events.Reset();
	TestTrue(TEXT("popup-list Escape is handled in one event"),
		FSlateApplication::Get().ProcessKeyDownEvent(
			MakeFrontEndKeyEvent(EKeys::Escape)));
	Root->Tick(Root->GetCachedGeometry(), 0.0, 0.0f);
	FSlateApplication::Get().Tick();
	TestFalse(TEXT("popup-list Escape closes the dropdown"),
		TypedCombo.IsValid() && TypedCombo->IsOpen());
	TestTrue(TEXT("popup-list Escape closes only the dropdown: still on Play"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	TestEqual(TEXT("closing the dropdown rebuilds nothing"),
		Root->ScreenRebuildCountForTesting(), BeforeComboBack);
	TestTrue(TEXT("focus returns from the dropdown to the map list"),
		FSlateApplication::Get().GetKeyboardFocusedWidget() == Combo);
	TestTrue(TEXT("a second Escape is handled"),
		FSlateApplication::Get().ProcessKeyDownEvent(
			MakeFrontEndKeyEvent(EKeys::Escape)));
	Root->Tick(Root->GetCachedGeometry(), 0.0, 0.0f);
	FSlateApplication::Get().Tick();
	TestTrue(TEXT("a second Escape returns Play to Main"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);
	TestEqual(TEXT("the two Escapes cause one transition"),
		Root->ScreenRebuildCountForTesting(), BeforeComboBack + 1);
	TestTrue(TEXT("popup-list Escape emits no feedback"),
		Feedback->Events.IsEmpty());
	TestEqual(TEXT("popup-list Escape preserves the presentation snapshot"),
		Authority->SnapshotIdentity(), ComboSnapshot);
	TestTrue(TEXT("popup-list Escape restores selected main focus"),
		FSlateApplication::Get().GetKeyboardFocusedWidget()
			== Root->MainMenuButtonForTesting(0));

	// The B/View half: B on the open dropdown closes it (consumed, spec r3
	// section 1a), and a second B backs Play to Main.
	Root->ActivateMainMenuEntryForTesting(0);
	TSharedPtr<SWidget> ControllerCombo = Root->MapComboBoxForTesting();
	TSharedPtr<SWoTListCombo> TypedControllerCombo = Root->MapListComboForTesting();
	TArray<TSharedRef<SWidget>> ControllerComboButtons;
	CollectWidgetsOfType(ControllerCombo.ToSharedRef(), FName(TEXT("SButton")),
		ControllerComboButtons);
	TestTrue(TEXT("controller-back case opens combo through a real pointer click"),
		ControllerComboButtons.Num() > 0
			&& ClickWidgetThroughSlate(ControllerComboButtons[0]));
	FSlateApplication::Get().Tick(ESlateTickType::All);
	const TSharedPtr<SWidget> ControllerPopupFocus =
		FSlateApplication::Get().GetKeyboardFocusedWidget();
	TestTrue(TEXT("controller-back case genuinely focuses popup list"),
		ControllerPopupFocus.IsValid() && ControllerPopupFocus != ControllerCombo
			&& TypedControllerCombo.IsValid()
			&& ControllerPopupFocus == TypedControllerCombo->GetPopupWidget());
	const uint64 BeforeControllerComboBack =
		Root->ScreenRebuildCountForTesting();
	Feedback->Events.Reset();
	TestTrue(TEXT("popup-list controller back is handled in one event"),
		FSlateApplication::Get().ProcessKeyDownEvent(
			MakeFrontEndKeyEvent(EKeys::Gamepad_FaceButton_Right)));
	Root->Tick(Root->GetCachedGeometry(), 0.0, 0.0f);
	FSlateApplication::Get().Tick();
	TestFalse(TEXT("popup-list controller back closes the dropdown"),
		TypedControllerCombo.IsValid() && TypedControllerCombo->IsOpen());
	TestTrue(TEXT("popup-list controller back never also backs the screen"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	TestEqual(TEXT("closing the dropdown by B rebuilds nothing"),
		Root->ScreenRebuildCountForTesting(), BeforeControllerComboBack);
	TestTrue(TEXT("B returns focus to the map list"),
		FSlateApplication::Get().GetKeyboardFocusedWidget() == ControllerCombo);
	TestTrue(TEXT("a second controller back is handled"),
		FSlateApplication::Get().ProcessKeyDownEvent(
			MakeFrontEndKeyEvent(EKeys::Gamepad_FaceButton_Right)));
	Root->Tick(Root->GetCachedGeometry(), 0.0, 0.0f);
	FSlateApplication::Get().Tick();
	TestTrue(TEXT("a second controller back returns Play to Main"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Main);
	TestEqual(TEXT("the two backs cause one transition"),
		Root->ScreenRebuildCountForTesting(), BeforeControllerComboBack + 1);
	TestTrue(TEXT("popup-list controller back emits no feedback"),
		Feedback->Events.IsEmpty());
	TestTrue(TEXT("popup-list controller back restores selected main focus"),
		FSlateApplication::Get().GetKeyboardFocusedWidget()
			== Root->MainMenuButtonForTesting(0));

	Root->ActivateMainMenuEntryForTesting(0);
	AssertBackFromFocusedPlayChild(TEXT("session list"),
		Root->SessionListViewForTesting(), EKeys::Gamepad_Special_Left);
	const uint64 AfterHandledBack = Root->ScreenRebuildCountForTesting();
	TestFalse(TEXT("back on Main is a no-op and unhandled"),
		FSlateApplication::Get().ProcessKeyDownEvent(
			MakeFrontEndKeyEvent(EKeys::Escape)));
	TestEqual(TEXT("back no-op cannot rebuild Main"),
		Root->ScreenRebuildCountForTesting(), AfterHandledBack);
	TestTrue(TEXT("back no-op emits no feedback"), Feedback->Events.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSessionSpec,
	"WoT.FrontEnd.Session",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSessionSpec::RunTest(const FString&)
{
	TSharedRef<FWoTFrontEndTests_SessionState> State =
		MakeShared<FWoTFrontEndTests_SessionState>();
	State->World = WoTFrontEndTests_CreateWorld();
	if (!TestNotNull(TEXT("front-end session test world exists"), State->World))
	{
		return false;
	}

	State->GameInstance = Cast<UWoTGameInstance>(State->World->GetGameInstance());
	if (!TestNotNull(TEXT("configured WoT GameInstance exists"), State->GameInstance))
	{
		WoTFrontEndTests_DestroyWorld(State->World);
		return false;
	}
	State->Sessions = State->GameInstance->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("session subsystem exists"), State->Sessions))
	{
		WoTFrontEndTests_DestroyWorld(State->World);
		return false;
	}

	const IOnlineSubsystem* OSS = Online::GetSubsystem(State->World);
	if (!TestNotNull(TEXT("headless OSS exists"), OSS))
	{
		WoTFrontEndTests_DestroyWorld(State->World);
		return false;
	}
	if (OSS->GetSubsystemName() != FName(TEXT("NULL")))
	{
		// Windows automation can inherit a live Steam client; the deterministic
		// Null seam remains covered by Mac/CI rather than asserting Steam is absent.
		AddInfo(FString::Printf(
			TEXT("Skipping Null OSS session seam under %s (Windows + Steam case)"),
			*OSS->GetSubsystemName().ToString()));
		WoTFrontEndTests_DestroyWorld(State->World);
		return true;
	}
	const IOnlineSessionPtr OnlineSessions = OSS->GetSessionInterface();
	if (!TestTrue(TEXT("Null OSS exposes its session interface"),
		OnlineSessions.IsValid()))
	{
		WoTFrontEndTests_DestroyWorld(State->World);
		return false;
	}
	TestFalse(TEXT("no named session exists initially"), State->Sessions->HasSession());

	// Keep all host coverage ahead of LAN discovery: a sandbox may synchronously
	// refuse the later Null broadcast with SE_EHOSTUNREACH.
	State->GameInstance->SetSelectedFaction(
		EWoTPlayerFaction::Whitecloak, /*bSave*/ false);
	State->GameInstance->SetSelectedSkinColor(
		EWoTPlayerSkinColor::Blue, /*bSave*/ false);
	FWoTHostRequest Request;
	Request.MapPackage = TEXT("/Game/WoT/Maps/Battle2_01");
	Request.MapTitle = FWoTFrontEndMapCatalog::ResolveRetailTitle(TEXT("Battle2_01"));
	Request.ServerName = TEXT("  Front-End Spec  ");
	Request.FragLimit = 101;
	Request.TimeLimit = -3;
	Request.MaxPlayers = 99;
	Request.bLAN = true;
	State->Sessions->OnHostComplete().AddLambda([State](bool bSucceeded)
	{
		++State->HostCompletionCount;
		State->bHostSucceeded = bSucceeded;
	});
	int32 SceneSuspensionsBeforeBrowse = 0;
	UWoTSessionSubsystem::SetSceneTravelObserverForTesting(
		[&SceneSuspensionsBeforeBrowse]() { ++SceneSuspensionsBeforeBrowse; });
	ON_SCOPE_EXIT
	{
		UWoTSessionSubsystem::SetSceneTravelObserverForTesting({});
	};

	const bool bHostAccepted = State->Sessions->Host(Request);
	TestTrue(TEXT("parameterized Host accepted"), bHostAccepted);
	if (bHostAccepted)
	{
		TestEqual(TEXT("Host captured the complete travel URL before callback"),
			State->Sessions->GetLastHostTravelURL(),
			FString(TEXT("/Game/WoT/Maps/Battle2_01?listen?bIsLanMatch"
				"?game=/Script/WoT.WoTBattleGameMode?FragLimit=100?TimeLimit=0"
				"?Cycle=1?MaxPlayers=16?MaxClientTickRate=0"
				"?SealGoal=4?SealBudget=3?AutoTeamSelect=0?MaxTeamSize=3"
				"?Class=WOTPawns.Whitecloak?Skin=WhitecloakSkins.Whi_Blue")));
		TestTrue(TEXT("Null OSS created the named session"),
			State->Sessions->HasSession());
		TestEqual(TEXT("parameterized Host completed exactly once"),
			State->HostCompletionCount, 1);
		TestTrue(TEXT("parameterized Host announced success"),
			State->bHostSucceeded);
		TestEqual(TEXT("scene suspension is the last gate before host browse"),
			SceneSuspensionsBeforeBrowse, 1);
		TestTrue(TEXT("successful host travel remains pending until exposure"),
			State->Sessions->IsHostTravelPendingForTesting());
		TestNotNull(TEXT("pending host travel retains its source world"),
			State->Sessions->HostTravelWorldForTesting());
		State->Sessions->NotifySuccessfulHostDestinationExposure();
		TestFalse(TEXT("host exposure retires pending host classification"),
			State->Sessions->IsHostTravelPendingForTesting());
		TestFalse(TEXT("host exposure clears handled-failure classification"),
			State->Sessions->WasHostTravelFailureHandledForTesting());
		TestNull(TEXT("host exposure releases the source world marker"),
			State->Sessions->HostTravelWorldForTesting());

		FNamedOnlineSession* NamedSession = OnlineSessions.IsValid()
			? OnlineSessions->GetNamedSession(NAME_GameSession) : nullptr;
		if (TestNotNull(TEXT("named session settings are available"), NamedSession))
		{
			FString ServerName;
			FString MapName;
			NamedSession->SessionSettings.Get(
				FName(TEXT("WOT_SERVER_NAME")), ServerName);
			NamedSession->SessionSettings.Get(SETTING_MAPNAME, MapName);
			TestEqual(TEXT("server name is trimmed and advertised"),
				ServerName, FString(TEXT("Front-End Spec")));
			TestEqual(TEXT("decoded retail map title is advertised"), MapName,
				Request.MapTitle);
			TestEqual(TEXT("advertised capacity is clamped"),
				NamedSession->SessionSettings.NumPublicConnections, 16);
		}

		TestTrue(TEXT("Leave accepted"), State->Sessions->Leave());
		TestFalse(TEXT("deliberate leave clears pending host classification"),
			State->Sessions->IsHostTravelPendingForTesting());
		TestFalse(TEXT("deliberate leave clears handled host failure state"),
			State->Sessions->WasHostTravelFailureHandledForTesting());
		TestNull(TEXT("deliberate leave releases the host source world"),
			State->Sessions->HostTravelWorldForTesting());
		TestFalse(TEXT("Leave destroyed the named session"),
			State->Sessions->HasSession());
		TestEqual(TEXT("Leave selected the front-end URL after destroy"),
			State->Sessions->GetLastReturnTravelURL(),
			FWoTLeaveStateMachine::FrontEndURL());
		TestEqual(TEXT("automation leave returned to idle after suppressing travel"),
			static_cast<int32>(State->Sessions->GetLeaveState()),
			static_cast<int32>(EWoTLeaveState::Idle));
	}

	const bool bCompatibilityHostAccepted =
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true);
	TestTrue(TEXT("compatibility Host accepted"), bCompatibilityHostAccepted);
	if (bCompatibilityHostAccepted)
	{
		TestEqual(TEXT("compatibility Host preserves current-map listen travel"),
			State->Sessions->GetLastHostTravelURL(),
			FString(TEXT("?listen?bIsLanMatch")));
		TestTrue(TEXT("compatibility Host cleanup accepted"),
			State->Sessions->Leave());
		TestFalse(TEXT("compatibility Host cleanup destroyed the named session"),
			State->Sessions->HasSession());
	}
	UWoTBootstrapSubsystem* Bootstrap =
		State->GameInstance->GetSubsystem<UWoTBootstrapSubsystem>();
	const bool bSceneWasCommitted = Bootstrap != nullptr
		&& Bootstrap->IsFrontEndSceneCommitted();
	State->Sessions->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return false; });
	const int32 SuspensionsBeforeFailedTravel = SceneSuspensionsBeforeBrowse;
	const bool bFailedTravelHostAccepted =
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true);
	TestTrue(TEXT("failed ServerTravel still accepts the async create request"),
		bFailedTravelHostAccepted);
	if (bFailedTravelHostAccepted)
	{
		TestFalse(TEXT("failed ServerTravel reports host completion failure"),
			State->bHostSucceeded);
		TestEqual(TEXT("failed ServerTravel crossed the suspension boundary"),
			SceneSuspensionsBeforeBrowse, SuspensionsBeforeFailedTravel + 1);
		if (bSceneWasCommitted)
		{
			TestTrue(TEXT("failed ServerTravel immediately restores the entry scene"),
				Bootstrap->IsFrontEndSceneCommitted());
			APlayerController* FailedTravelController =
				State->World->GetFirstPlayerController();
			TestTrue(TEXT("failed travel immediately restores an actual scene view"),
				FailedTravelController != nullptr
					&& FailedTravelController->GetViewTarget() != nullptr
					&& FailedTravelController->GetViewTarget()
						!= FailedTravelController);
		}
		TestFalse(TEXT("failed travel destroys the named session before completion"),
			State->Sessions->HasSession());
		const int32 HostCompletionsBeforeRetry = State->HostCompletionCount;
		State->Sessions->SetServerTravelHookForTesting(
			[](UWorld*, const FString&, bool) { return true; });
		TestTrue(TEXT("a second Host is accepted without a masking Leave"),
			State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
		TestEqual(TEXT("retry Host completes independently"),
			State->HostCompletionCount, HostCompletionsBeforeRetry + 1);
		TestTrue(TEXT("retry Host cleanup accepted"), State->Sessions->Leave());
	}
	State->Sessions->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return true; });
	const int32 HostCompletionsBeforeAsyncFailure = State->HostCompletionCount;
	TestTrue(TEXT("asynchronous-failure Host dispatches after create"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	TestTrue(TEXT("dispatched Host retains the named session until travel resolves"),
		State->Sessions->HasSession());
	GEngine->OnTravelFailure().Broadcast(State->World,
		ETravelFailure::ServerTravelFailure,
		TEXT("synthetic asynchronous host travel failure"));
	const FString RetainedHostFailure =
		State->Sessions->ConsumeConnectionFailure();
	TestTrue(TEXT("failed guarded host travel retains its recovery message"),
		RetainedHostFailure.Contains(
			TEXT("synthetic asynchronous host travel failure")));
	TestFalse(TEXT("asynchronous ServerTravel failure destroys the named session"),
		State->Sessions->HasSession());
	TestEqual(TEXT("asynchronous failure publishes one terminal host failure"),
		State->HostCompletionCount, HostCompletionsBeforeAsyncFailure + 2);
	TestFalse(TEXT("asynchronous failure terminal event is false"),
		State->bHostSucceeded);
	TestTrue(TEXT("asynchronous failure accepts retry without Leave"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	TestTrue(TEXT("asynchronous-failure retry cleanup accepted"),
		State->Sessions->Leave());
	for (const bool bDispatchResult : {true, false})
	{
		State->Sessions->SetServerTravelHookForTesting(
			[State, bDispatchResult](UWorld*, const FString&, bool)
			{
				GEngine->OnTravelFailure().Broadcast(State->World,
					ETravelFailure::ServerTravelFailure,
					TEXT("synthetic reentrant host travel failure"));
				return bDispatchResult;
			});
		const int32 BeforeReentrant = State->HostCompletionCount;
		TestTrue(TEXT("reentrant-failure Host accepts the create request"),
			State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
		TestEqual(TEXT("reentrant failure publishes one terminal completion"),
			State->HostCompletionCount, BeforeReentrant + 1);
		TestFalse(TEXT("reentrant failure cannot be overwritten by travel success"),
			State->bHostSucceeded);
		TestFalse(TEXT("reentrant failure removes the named session"),
			State->Sessions->HasSession());
	}
	State->Sessions->SetServerTravelHookForTesting(
		[State](UWorld*, const FString&, bool)
		{
			State->Sessions->DeactivateForUnauthorizedWorld();
			return false;
		});
	const int32 BeforeLostAuthority = State->HostCompletionCount;
	TestTrue(TEXT("authority-loss Host accepts the create request"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	TestEqual(TEXT("authority loss publishes one terminal failure"),
		State->HostCompletionCount, BeforeLostAuthority + 1);
	TestTrue(TEXT("session authority can be readopted after cleanup"),
		State->Sessions->ActivateForAdoptedWorldAuthority());
	TestFalse(TEXT("authority loss during dispatch still removes named session"),
		State->Sessions->HasSession());
	State->Sessions->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return true; });

	TFunction<bool()> DelayedCreate;
	State->Sessions->SetCreateSessionDispatchHookForTesting(
		[&DelayedCreate](TFunction<bool()>&& Dispatch)
		{
			DelayedCreate = MoveTemp(Dispatch);
			return true;
		});
	const int32 BeforeRevokedCreate = State->HostCompletionCount;
	TestTrue(TEXT("delayed provider create is accepted before revocation"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	const uint64 RevokedCreateSerial =
		State->Sessions->ActiveHostCreateSerialForTesting();
	TestTrue(TEXT("delayed create owns a nonzero operation serial"),
		RevokedCreateSerial != 0);
	TestTrue(TEXT("provider create dispatch was retained by the test seam"),
		static_cast<bool>(DelayedCreate));
	State->Sessions->DeactivateForUnauthorizedWorld();
	if (DelayedCreate)
	{
		TestTrue(TEXT("delayed Null provider accepts create after revocation"),
			DelayedCreate());
	}
	TestTrue(TEXT("revoked delayed success removes the provider named session"),
		OnlineSessions->GetNamedSession(NAME_GameSession) == nullptr);
	TestEqual(TEXT("revoked delayed success publishes one terminal failure"),
		State->HostCompletionCount, BeforeRevokedCreate + 1);
	TestFalse(TEXT("revoked delayed success cannot report host success"),
		State->bHostSucceeded);
	TestTrue(TEXT("authority readopts after revoked delayed success cleanup"),
		State->Sessions->ActivateForAdoptedWorldAuthority());
	State->Sessions->SetCreateSessionDispatchHookForTesting({});
	TestTrue(TEXT("Host retries after revoked delayed success without Leave"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	TestTrue(TEXT("revoked delayed success retry cleanup accepted"),
		State->Sessions->Leave());

	DelayedCreate = {};
	State->Sessions->SetCreateSessionDispatchHookForTesting(
		[&DelayedCreate](TFunction<bool()>&& Dispatch)
		{
			DelayedCreate = MoveTemp(Dispatch);
			return true;
		});
	const int32 BeforeRevokedFailure = State->HostCompletionCount;
	TestTrue(TEXT("delayed failure operation is accepted before revocation"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	const uint64 RevokedFailureSerial =
		State->Sessions->ActiveHostCreateSerialForTesting();
	TFunction<void(bool)> StaleCreateCompletion =
		State->Sessions->CaptureHostCreateCompletionForTesting();
	State->Sessions->DeactivateForUnauthorizedWorld();
	StaleCreateCompletion(/*bOk*/ false);
	TestEqual(TEXT("revoked delayed failure publishes exactly one terminal result"),
		State->HostCompletionCount, BeforeRevokedFailure + 1);
	TestFalse(TEXT("revoked delayed failure remains a failure"),
		State->bHostSucceeded);
	TestTrue(TEXT("revoked delayed failure leaves no provider named session"),
		OnlineSessions->GetNamedSession(NAME_GameSession) == nullptr);
	TestTrue(TEXT("authority readopts after revoked delayed failure"),
		State->Sessions->ActivateForAdoptedWorldAuthority());

	const uint64 StaleSerial = RevokedFailureSerial;
	const int32 BeforeCurrentCreate = State->HostCompletionCount;
	DelayedCreate = {};
	TestTrue(TEXT("a current delayed create follows the completed revoked failure"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	const uint64 CurrentSerial =
		State->Sessions->ActiveHostCreateSerialForTesting();
	TestTrue(TEXT("new delayed create has a distinct serial"),
		CurrentSerial != 0 && CurrentSerial != StaleSerial);
	StaleCreateCompletion(/*bOk*/ true);
	TestEqual(TEXT("stale completion cannot publish a terminal result"),
		State->HostCompletionCount, BeforeCurrentCreate);
	TestEqual(TEXT("stale completion cannot consume the current operation"),
		State->Sessions->ActiveHostCreateSerialForTesting(), CurrentSerial);
	State->Sessions->CaptureHostCreateCompletionForTesting()(/*bOk*/ false);
	TestEqual(TEXT("current delayed completion publishes once"),
		State->HostCompletionCount, BeforeCurrentCreate + 1);
	TestFalse(TEXT("current delayed completion reports failure"),
		State->bHostSucceeded);
	State->Sessions->SetCreateSessionDispatchHookForTesting({});
	TestTrue(TEXT("Host retries after stale completion without Leave"),
		State->Sessions->Host(/*MaxPlayers*/ 8, /*bLan*/ true));
	TestTrue(TEXT("stale-completion retry cleanup accepted"),
		State->Sessions->Leave());
	State->Sessions->SetServerTravelHookForTesting({});
	TestTrue(TEXT("Leave without a named session is accepted"),
		State->Sessions->Leave());
	TestEqual(TEXT("no-session Leave uses the same front-end URL"),
		State->Sessions->GetLastReturnTravelURL(),
		FWoTLeaveStateMachine::FrontEndURL());

	int32 RecoveryClientTravelCount = 0;
	FString LastRecoveryTravelURL;
	AWoTPlayerController* RecoveryController =
		State->World->SpawnActor<AWoTPlayerController>();
	TestNotNull(TEXT("post-fallback regression has a local travel controller"),
		RecoveryController);
	State->Sessions->SetClientTravelControllerForTesting(
		RecoveryController);
	State->Sessions->SetClientTravelHookForTesting(
		[&RecoveryClientTravelCount, &LastRecoveryTravelURL](
			APlayerController*, const FString& URL,
			ETravelType)
		{
			++RecoveryClientTravelCount;
			LastRecoveryTravelURL = URL;
		});
	TestTrue(TEXT("direct client travel is pending before fallback reconstruction"),
		State->Sessions->ConnectDirect(TEXT("127.0.0.1")));
	TestEqual(TEXT("direct travel carries the persisted class and skin"),
		LastRecoveryTravelURL,
		FString(TEXT("127.0.0.1?Class=WOTPawns.Whitecloak"
			"?Skin=WhitecloakSkins.Whi_Blue")));
	TestTrue(TEXT("fallback root initially consumes no pre-broadcast failure"),
		State->Sessions->ConsumeConnectionFailure().IsEmpty());
	GEngine->OnTravelFailure().Broadcast(State->World,
		ETravelFailure::ClientTravelFailure,
		TEXT("synthetic post-fallback client travel failure"));
	const FString PostFallbackFailure =
		State->Sessions->ConsumeConnectionFailure();
	TestTrue(TEXT("post-fallback client failure remains reportable"),
		PostFallbackFailure.Contains(
			TEXT("synthetic post-fallback client travel failure")));
	TestTrue(TEXT("post-fallback failure drives the authorized recovery route"),
		RecoveryClientTravelCount >= 2);
	struct FNetworkFailureCase
	{
		ENetworkFailure::Type Type;
		const TCHAR* Detail;
	};
	const FNetworkFailureCase NetworkFailures[] = {
		{ENetworkFailure::ConnectionTimeout, TEXT("synthetic timeout")},
		{ENetworkFailure::ConnectionLost, TEXT("synthetic connection loss")},
		{ENetworkFailure::FailureReceived, TEXT("synthetic refusal")},
		{ENetworkFailure::OutdatedClient, TEXT("synthetic version mismatch")},
	};
	for (const FNetworkFailureCase& Case : NetworkFailures)
	{
		TestTrue(FString(TEXT("client travel is pending for ")) + Case.Detail,
			State->Sessions->ConnectDirect(TEXT("127.0.0.1")));
		GEngine->OnNetworkFailure().Broadcast(
			State->World, nullptr, Case.Type, Case.Detail);
		const FString Failure = State->Sessions->ConsumeConnectionFailure();
		TestTrue(FString(TEXT("failure lands back on Multiplayer: ")) + Case.Detail,
			Failure.Contains(Case.Detail));
	}
	TestTrue(TEXT("client travel is pending for server-travel failure"),
		State->Sessions->ConnectDirect(TEXT("127.0.0.1")));
	GEngine->OnTravelFailure().Broadcast(State->World,
		ETravelFailure::ServerTravelFailure,
		TEXT("synthetic server travel failure"));
	TestTrue(TEXT("server-travel failure lands back on Multiplayer"),
		State->Sessions->ConsumeConnectionFailure().Contains(
			TEXT("synthetic server travel failure")));
	State->Sessions->SetClientTravelHookForTesting({});
	State->Sessions->SetClientTravelControllerForTesting(nullptr);

	State->Sessions->OnSearchComplete().AddLambda(
		[State](bool bSucceeded, const TArray<FWoTSessionListEntry>& Results)
		{
			++State->SearchCompletionCount;
			State->bSearchSucceeded = bSucceeded;
			State->SearchResults = Results;
		});
	// Host or Direct Connect cuts the menu's search short: the cancelled Find
	// publishes nothing, and the LAN beacon is free for the next search.
	if (State->Sessions->Find(/*bLan*/ true))
	{
		State->Sessions->CancelFind();
		TestFalse(TEXT("a cancelled Find is no longer in flight"),
			State->Sessions->IsFindInFlight());
		TestEqual(TEXT("a cancelled Find publishes no completion"),
			State->SearchCompletionCount, 0);
	}
	State->SearchCompletionCount = 0;
	State->Deadline = FPlatformTime::Seconds() + 8.0;
	if (!State->Sessions->Find(/*bLan*/ true))
	{
		// Sandboxed headless hosts can refuse the Null OSS LAN discovery
		// broadcast outright (SE_EHOSTUNREACH). The UI-safe contract still
		// holds either way: the completion delegate fires synchronously with
		// failure and an empty snapshot.
		TestEqual(TEXT("refused Find fired exactly one completion"),
			State->SearchCompletionCount, 1);
		TestFalse(TEXT("refused Find reports failure"), State->bSearchSucceeded);
		TestEqual(TEXT("refused Find yields no snapshot rows"),
			State->SearchResults.Num(), 0);
		WoTFrontEndTests_DestroyWorld(State->World);
		return true;
	}

	ADD_LATENT_AUTOMATION_COMMAND(
		FWoTFrontEndTests_WaitForSessionCompletion(State, this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSessionFailureHeartbeatSpec,
	"WoT.FrontEnd.SessionFailureHeartbeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSessionFailureHeartbeatSpec::RunTest(const FString&)
{
	// A host-join test once read a deliberate Leave as a dropped
	// LAN join because nothing durable said why the joiner returned to the
	// menu (the guarded build compiles logging out). Every owned engine
	// network/travel failure now lands in a `net-failure` heartbeat with its
	// engine type and message, and every Leave in a `session-leave` heartbeat
	// naming its cause.
	TestEqual(TEXT("net-failure detail names kind, type, role and message"),
		UWoTSessionSubsystem::NetFailureHeartbeatDetail(TEXT("network"),
			ENetworkFailure::ToString(ENetworkFailure::ConnectionLost),
			TEXT("client"), true, TEXT("  peer closed the connection \n")),
		FString(TEXT("kind=network type=ConnectionLost role=client ")
			TEXT("client_connection=1 msg=peer closed the connection")));
	TestEqual(TEXT("an empty engine message is spelled '-'"),
		UWoTSessionSubsystem::NetFailureHeartbeatDetail(TEXT("travel"),
			ETravelFailure::ToString(ETravelFailure::ClientTravelFailure),
			TEXT("travel"), false, TEXT("   ")),
		FString(TEXT("kind=travel type=ClientTravelFailure role=travel ")
			TEXT("client_connection=0 msg=-")));
	const FString LongMessage = FString::ChrN(1000, TEXT('x'));
	TestTrue(TEXT("a peer message is bounded to 400 characters"),
		UWoTSessionSubsystem::NetFailureHeartbeatDetail(TEXT("network"),
			TEXT("FailureReceived"), TEXT("pending"), true, LongMessage)
			.EndsWith(TEXT("msg=") + FString::ChrN(400, TEXT('x'))));
	TestEqual(TEXT("a failure-driven leave says so"),
		UWoTSessionSubsystem::SessionLeaveHeartbeatDetail(true, true),
		FString(TEXT("reason=connection-failure named_session=1")));
	TestEqual(TEXT("a requested leave says so"),
		UWoTSessionSubsystem::SessionLeaveHeartbeatDetail(false, false),
		FString(TEXT("reason=requested named_session=0")));

	FString Parent = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(),
		TEXT("wot-session-failure-heartbeat-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FPaths::NormalizeDirectoryName(Parent);
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("heartbeat fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	UWorld* World = WoTFrontEndTests_CreateWorld();
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
		WoTFrontEndTests_DestroyWorld(World);
	};
	if (!TestNotNull(TEXT("session failure test world exists"), World))
	{
		return false;
	}
	UWoTSessionSubsystem* Sessions = World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("session subsystem exists"), Sessions)
		|| !TestTrue(TEXT("session subsystem is authorized for the world"),
			Sessions->CanUseAuthorizedWorldForTesting()))
	{
		return false;
	}
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("a local travel controller exists"), Controller))
	{
		return false;
	}
	int32 ClientTravels = 0;
	Sessions->SetClientTravelControllerForTesting(Controller);
	Sessions->SetClientTravelHookForTesting(
		[&ClientTravels](APlayerController*, const FString&, ETravelType)
		{
			++ClientTravels;
		});
	ON_SCOPE_EXIT
	{
		Sessions->SetClientTravelHookForTesting({});
		Sessions->SetClientTravelControllerForTesting(nullptr);
	};
	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);

	TestTrue(TEXT("a direct connection is pending"),
		Sessions->ConnectDirect(TEXT("127.0.0.1:7777")));
	GEngine->OnNetworkFailure().Broadcast(World, nullptr,
		ENetworkFailure::ConnectionLost, TEXT("synthetic drop"));
	TestTrue(TEXT("the failure is still reported to the menu"),
		Sessions->ConsumeConnectionFailure().Contains(TEXT("synthetic drop")));
	TestTrue(TEXT("an explicit Leave is accepted"), Sessions->Leave());
	GEngine->OnTravelFailure().Broadcast(World,
		ETravelFailure::ClientTravelFailure, TEXT("synthetic travel refusal"));
	TestTrue(TEXT("failure recovery and both leaves traveled to the menu"),
		ClientTravels >= 3);

	FString Contents;
	if (!TestTrue(TEXT("the heartbeat log is readable"),
		FFileHelper::LoadFileToString(Contents, *FPaths::Combine(
			InstallRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName()))))
	{
		return false;
	}
	TArray<FString> Lines;
	Contents.ParseIntoArrayLines(Lines);
	auto IndexOf = [&Lines](const TCHAR* Event, const TCHAR* Detail) -> int32
	{
		const FString EventField = FString::Printf(TEXT("\"event\":\"%s\""), Event);
		const FString DetailField = FString::Printf(TEXT("\"detail\":\"%s\""), Detail);
		return Lines.IndexOfByPredicate([&](const FString& Line)
		{
			return Line.Contains(EventField, ESearchCase::CaseSensitive)
				&& Line.Contains(DetailField, ESearchCase::CaseSensitive);
		});
	};
	const int32 Dropped = IndexOf(TEXT("net-failure"),
		TEXT("kind=network type=ConnectionLost role=standalone ")
		TEXT("client_connection=1 msg=synthetic drop"));
	const int32 FailureLeave = IndexOf(TEXT("session-leave"),
		TEXT("reason=connection-failure named_session=0"));
	const int32 RequestedLeave = IndexOf(TEXT("session-leave"),
		TEXT("reason=requested named_session=0"));
	const int32 Travel = IndexOf(TEXT("net-failure"),
		TEXT("kind=travel type=ClientTravelFailure role=travel ")
		TEXT("client_connection=0 msg=synthetic travel refusal"));
	TestTrue(TEXT("the dropped connection is recorded with type and message"),
		Dropped != INDEX_NONE);
	TestTrue(TEXT("the failure-driven leave follows the failure record"),
		FailureLeave != INDEX_NONE && FailureLeave > Dropped);
	TestTrue(TEXT("the requested leave is recorded as requested"),
		RequestedLeave != INDEX_NONE && RequestedLeave > FailureLeave);
	TestTrue(TEXT("a travel failure is recorded even with no connection pending"),
		Travel != INDEX_NONE && Travel > RequestedLeave);
	if (HasAnyErrors())
	{
		for (const FString& Line : Lines)
		{
			AddInfo(Line);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityFix18MultiplayerLabelsSpec,
	"WoT.Parity.Fix18.MultiplayerLabelsFromRetailText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTParityFix18MultiplayerLabelsSpec::RunTest(const FString&)
{
	// The packaged host panel once showed the raw keys
	// menuStartArenaServer.MenuList[4], [6], [8] and [9]. The authenticated
	// WoT.int projection kept a fixed label list, and the editor-only menu
	// strings asset (absent from the neutral package) hid the gap from every
	// spec that injected labels straight into the authority. This drives the
	// real projection into the real screens with that asset hidden.
#if PLATFORM_MAC
	// Headless SWindows have no native NSWindow: as focus walks the host
	// panel's text fields, the macOS IME bridge logs "Activating/Deactivating
	// a context failed when its window couldn't be found". Engine noise only.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	FWoTMenuStyle::Get().SetEditorMenuStringsHiddenForTesting(true);
	ON_SCOPE_EXIT { FWoTMenuStyle::Get().SetEditorMenuStringsHiddenForTesting(false); };

	const TArray<uint8> Bytes = WoTFrontEndTest_PlaceholderMenuLocalization();
	TMap<FName, FString> Projected;
	if (!TestTrue(TEXT("the placeholder WoT.int projects"),
		FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(Bytes, Projected)))
	{
		return false;
	}
	TestEqual(TEXT("the host framerate label projects"),
		Projected.FindRef(FName(TEXT("menuStartArenaServer.MenuList[6]"))),
		FString(TEXT("Label 2 6")));
	TestEqual(TEXT("a quoted label loses its enclosing quotes"),
		Projected.FindRef(FName(TEXT("menuKeyboard.MenuList[12]"))),
		FString(TEXT("Label 6 12")));
	{
		// Only the four opening labels gate admission (fix18 review): an empty or
		// repeated optional label is skipped / first-wins, never fatal.
		const ANSICHAR Damaged[] =
			"[menuMain]\nMenuList[2]=Label A\nMenuList[3]=Label B\nMenuList[4]=Label C\n"
			"MenuList[7]=Label D\n[menuStartArenaServer]\nMenuTitle=\nMenuList[1]=First\n"
			"MenuList[1]=Second\n";
		TMap<FName, FString> Tolerant;
		TestTrue(TEXT("a damaged optional label does not reject the file"),
			FWoTMenuStyle::ParseAuthenticatedMainMenuLocalization(MakeArrayView(
				reinterpret_cast<const uint8*>(Damaged), static_cast<int32>(sizeof(Damaged) - 1)),
				Tolerant));
		TestEqual(TEXT("the first of a repeated optional label wins"),
			Tolerant.FindRef(FName(TEXT("menuStartArenaServer.MenuList[1]"))),
			FString(TEXT("First")));
		TestFalse(TEXT("an empty optional label is skipped"),
			Tolerant.Contains(FName(TEXT("menuStartArenaServer.MenuTitle"))));
	}

	const auto Feedback = MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	const auto Authority = FWoTFrontEndPresentationAuthority::CreateForTesting(
		TEXT("generation-input"), FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
		FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 20), MoveTemp(Projected),
		Feedback);
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority);
	Root->ActivateMainMenuEntryForTesting(0);
	if (!TestTrue(TEXT("multiplayer opens the real play screen"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play))
	{
		return false;
	}
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(1280.0f, 720.0f))
		.CreateTitleBar(false).SupportsMaximize(false).SupportsMinimize(false)
		[Root];
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window, false);
	ON_SCOPE_EXIT { Slate.RequestDestroyWindow(Window); };
	FGeometry Geometry = FGeometry::MakeRoot(
		FVector2D(1280.0f, 720.0f), FSlateLayoutTransform(1.0f));
	auto Settle = [&Root, &Window, &Geometry]()
	{
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			Root->Tick(Geometry, 0.0, 0.0f);
			Root->SlatePrepass(1.0f);
			FHittestGrid Grid;
			FSlateWindowElementList Elements(Window);
			const FPaintArgs Args(&Window.Get(), Grid,
				FVector2f::ZeroVector, FApp::GetCurrentTime(), 0.0f);
			Root->Paint(Args, Geometry, Geometry.GetLayoutBoundingRect(),
				Elements, 0, FWidgetStyle(), true);
		}
	};
	auto ExpectResolved = [this, &Root](const FString& Screen,
		std::initializer_list<const TCHAR*> Expected)
	{
		TArray<TSharedRef<SWidget>> Widgets;
		CollectWidgetsOfType(Root, FName(TEXT("STextBlock")), Widgets);
		TSet<FString> Shown;
		for (const TSharedRef<SWidget>& Widget : Widgets)
		{
			const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
			Shown.Add(Text);
			TestFalse(FString::Printf(TEXT("%s shows no raw localization key (%s)"),
				*Screen, *Text), WoTFrontEndTest_IsRawMenuKey(Text));
		}
		for (const TCHAR* Label : Expected)
		{
			TestTrue(FString::Printf(TEXT("%s shows the projected label %s"), *Screen, Label),
				Shown.Contains(Label));
		}
	};

	Settle();
	// Arena (section 2): cycle, framerate, admin and game password rows.
	ExpectResolved(TEXT("arena host panel"),
		{TEXT("Label 2 4"), TEXT("Label 2 6"), TEXT("Label 2 8"), TEXT("Label 2 9")});
	TArray<TSharedRef<SWidget>> AllText;
	CollectWidgetsOfType(Root, FName(TEXT("STextBlock")), AllText);
	for (const TSharedRef<SWidget>& Widget : AllText)
	{
		const FString Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		TestFalse(FString::Printf(TEXT("the host panel drops the mode/defaults line (%s)"), *Text),
			Text.StartsWith(TEXT("Mode: ")) || Text.StartsWith(TEXT("Retail defaults")));
	}

	const TSharedPtr<SWidget> Scroll = Root->HostScrollBoxForTesting();
	if (TestTrue(TEXT("the host panel scrolls"), Scroll.IsValid()))
	{
		TArray<TSharedRef<SWidget>> Bars;
		CollectWidgetsOfType(Scroll.ToSharedRef(), FName(TEXT("SScrollBar")), Bars);
		if (TestEqual(TEXT("the host panel owns one scrollbar"), Bars.Num(), 1))
		{
			TestTrue(TEXT("the host panel's scrollbar is drawn"),
				Bars[0]->GetVisibility() == EVisibility::Visible);
			TestTrue(FString::Printf(TEXT("the host panel's scrollbar is wide enough to find (%g)"),
				Bars[0]->GetCachedGeometry().GetLocalSize().X),
				Bars[0]->GetCachedGeometry().GetLocalSize().X >= 11.9f);
		}
	}

	{
		// Keyboard/gamepad (fix18 review): Down from the first host control walks
		// the rows to Host Match, which starts below the fold, and scrolls it in.
		const TSharedPtr<SScrollBox> HostScroll =
			StaticCastSharedPtr<SScrollBox>(Root->HostScrollBoxForTesting());
		const TSharedPtr<SWidget> HostMatch = Root->HostMatchButtonForTesting();
		if (TestTrue(TEXT("the host panel exposes its scroll box and Host Match"),
			HostScroll.IsValid() && HostMatch.IsValid()))
		{
			const FSlateRect View = HostScroll->GetCachedGeometry().GetLayoutBoundingRect();
			const float Overflow = HostScroll->GetScrollOffsetOfEnd();
			TestTrue(FString::Printf(TEXT("the host panel overflows at 1280x720 (scrollable %g, view %g-%g)"),
				Overflow, View.Top, View.Bottom), Overflow > 0.0f);
			TArray<TSharedRef<SWidget>> Edits;
			CollectWidgetsOfType(HostScroll.ToSharedRef(), FName(TEXT("SEditableTextBox")), Edits);
			TSharedPtr<SWidget> Focused;
			if (Edits.Num() > 0) Focused = Edits[0];
			bool bReached = false;
			for (int32 Step = 0; Focused.IsValid() && Step < 40 && !bReached; ++Step)
			{
				Slate.SetUserFocus(0, Focused, EFocusCause::Navigation);
				const FNavigationReply Reply = HostScroll->OnNavigation(
					HostScroll->GetCachedGeometry(),
					FNavigationEvent(FModifierKeysState(), 0, EUINavigation::Down,
						ENavigationGenesis::Keyboard));
				Focused = Reply.GetFocusRecipient();
				bReached = Focused.IsValid() && Focused == HostMatch;
			}
			TestTrue(TEXT("Down from the server name reaches Host Match"), bReached);
			if (bReached)
			{
				Slate.SetUserFocus(0, HostMatch, EFocusCause::Navigation);
				Settle();
				const FSlateRect Scrolled = HostScroll->GetCachedGeometry().GetLayoutBoundingRect();
				const FSlateRect Button = HostMatch->GetCachedGeometry().GetLayoutBoundingRect();
				TestTrue(FString::Printf(TEXT("Host Match is scrolled into view (button %g-%g, view %g-%g)"),
					Button.Top, Button.Bottom, Scrolled.Top, Scrolled.Bottom),
					Button.Top >= Scrolled.Top - 0.5f && Button.Bottom <= Scrolled.Bottom + 0.5f);
			}
			Slate.ClearUserFocus(0);
			HostScroll->ScrollToStart();
			Settle();
		}
	}

	{
		// Every host row sits label-left on one line, so the
		// Arena panel fits without scrolling at the game's own layout size: the
		// UI DPI curve lays out any 16:9 window as 1920x1080.
		Geometry = FGeometry::MakeRoot(FVector2D(1920.0f, 1080.0f), FSlateLayoutTransform(1.0f));
		Settle();
		const TSharedPtr<SScrollBox> FitScroll =
			StaticCastSharedPtr<SScrollBox>(Root->HostScrollBoxForTesting());
		if (TestTrue(TEXT("the host panel exposes its scroll box"), FitScroll.IsValid()))
		{
			TestTrue(FString::Printf(TEXT("the arena host panel fits at the 1920x1080 layout (scrollable %g)"),
				FitScroll->GetScrollOffsetOfEnd()), FitScroll->GetScrollOffsetOfEnd() <= 0.5f);
			// The controls share one column right of the widest label.
			TArray<TSharedRef<SWidget>> Controls;
			CollectWidgetsOfType(FitScroll.ToSharedRef(), FName(TEXT("SEditableTextBox")), Controls);
			CollectWidgetsOfType(FitScroll.ToSharedRef(), FName(TEXT("SWoTRetailSlider")), Controls);
			const float PanelLeft = FitScroll->GetCachedGeometry().GetLayoutBoundingRect().Left;
			int32 Placed = 0;
			float ColumnLeft = 0.0f;
			for (const TSharedRef<SWidget>& Control : Controls)
			{
				const FSlateRect Bounds = Control->GetCachedGeometry().GetLayoutBoundingRect();
				if (Bounds.GetSize().X <= 0.0f) continue; // a collapsed Battle-only row
				if (Placed++ == 0) ColumnLeft = Bounds.Left;
				TestTrue(FString::Printf(TEXT("a host control lines up with the column (left %g, column %g)"),
					Bounds.Left, ColumnLeft), FMath::IsNearlyEqual(Bounds.Left, ColumnLeft, 1.0f));
			}
			TestTrue(FString::Printf(TEXT("the host controls sit beside their labels (%d placed, column %g, panel %g)"),
				Placed, ColumnLeft, PanelLeft), Placed >= 6 && ColumnLeft > PanelLeft + 40.0f);
		}
		Geometry = FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f), FSlateLayoutTransform(1.0f));
		Settle();
	}

	if (TestTrue(TEXT("the catalog offers a Citadel map"),
		Root->SelectHostMapKindForTesting(true)))
	{
		Settle();
		// Citadel (section 3): cycle, team select, team size, framerate,
		// admin and game password rows.
		ExpectResolved(TEXT("citadel host panel"),
			{TEXT("Label 3 4"), TEXT("Label 3 8"), TEXT("Label 3 10"),
			 TEXT("Label 3 11"), TEXT("Label 3 13"), TEXT("Label 3 14")});
	}

	if (TestTrue(TEXT("player config opens"),
		Root->OpenPlayerConfigForTesting().IsEventHandled()))
	{
		Settle();
		// Player Config (section 4): the skin row and its help line.
		ExpectResolved(TEXT("player config"), {TEXT("Label 4 3"), TEXT("Help 4 3")});
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTParityFix18HelpWholePixelScaleSpec,
	"WoT.Parity.Fix18.HelpWholePixelScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTParityFix18HelpWholePixelScaleSpec::RunTest(const FString&)
{
	// The F1 help was hard to read when the help
	// window stretched its 640x480 layout to fit the screen, smearing the 1x
	// bitmap strikes by a fractional factor (2.25 at 1080p). It must draw at
	// a whole-number scale that fits, so glyphs meet their strike 1:1.
	FWoTHelpContent Content;
	Content.Title = TEXT("Title");
	Content.SubTitle = TEXT("Sub");
	Content.Content = {TEXT("%c=0"), TEXT("First line"), TEXT("Second line")};
	const TSharedRef<FWoTRequiredMenuFontGeneration> Generation =
		FWoTRequiredMenuFontGeneration::CreateForTesting();
	TSharedRef<FWoTRequiredMenuFonts> Fonts = MakeShared<FWoTRequiredMenuFonts>();
	Fonts->Title = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30);
	Fonts->List = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14);
	Fonts->Generation = Generation;
	struct FCase { FVector2D PhysicalSize; float Dpi; float ExpectedScale; };
	const FCase Cases[] = {
		{FVector2D(1920.0f, 1080.0f), 1.0f, 2.0f},
		{FVector2D(1280.0f, 720.0f), 1.0f, 1.0f},
		{FVector2D(3840.0f, 2160.0f), 2.0f, 2.0f},
		{FVector2D(1366.0f, 768.0f), 1.0f, 1.0f}};
	for (const FCase& Case : Cases)
	{
		const TSharedRef<SWoTHelpOverlay> Help = SNew(SWoTHelpOverlay)
			.Content(Content)
			.InitialPhysicalSize(Case.PhysicalSize)
			.InitialDpiScale(Case.Dpi)
			.RequiredFontsForTesting(Fonts);
		const TSharedRef<SWindow> Window = SNew(SWindow)
			.ClientSize(Case.PhysicalSize)
			.CreateTitleBar(false).SupportsMaximize(false).SupportsMinimize(false)
			[Help];
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.AddWindow(Window, false);
		ON_SCOPE_EXIT { Slate.RequestDestroyWindow(Window); };
		const FGeometry Geometry = FGeometry::MakeRoot(
			Case.PhysicalSize / Case.Dpi, FSlateLayoutTransform(Case.Dpi));
		{
			// Production order: Slate prepasses and arranges the new window before
			// it is ever ticked, so the first frame must already be at scale.
			Help->SlatePrepass(Case.Dpi);
			TArray<FGeometry> FirstFrame;
			CollectArrangedGeometryOfType(Help, Geometry,
				FName(TEXT("SConstraintCanvas")), FirstFrame);
			const float FirstScale = FirstFrame.Num() == 1
				? FirstFrame[0].GetAccumulatedLayoutTransform().GetScale() : 0.0f;
			TestTrue(FString::Printf(
				TEXT("%gx%g at DPI %g: the first frame already draws at %g (actual %g)"),
				Case.PhysicalSize.X, Case.PhysicalSize.Y, Case.Dpi, Case.ExpectedScale,
				FirstScale), FMath::IsNearlyEqual(FirstScale, Case.ExpectedScale, 0.001f));
		}
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			Help->Tick(Geometry, 0.0, 0.0f);
			Help->SlatePrepass(Case.Dpi);
			FHittestGrid Grid;
			FSlateWindowElementList Elements(Window);
			const FPaintArgs Args(&Window.Get(), Grid,
				FVector2f::ZeroVector, FApp::GetCurrentTime(), 0.0f);
			Help->Paint(Args, Geometry, Geometry.GetLayoutBoundingRect(),
				Elements, 0, FWidgetStyle(), true);
		}
		const FString Label = FString::Printf(TEXT("%gx%g at DPI %g"),
			Case.PhysicalSize.X, Case.PhysicalSize.Y, Case.Dpi);
		TArray<FGeometry> Canvases;
		CollectArrangedGeometryOfType(Help, Geometry,
			FName(TEXT("SConstraintCanvas")), Canvases);
		if (TestEqual(Label + TEXT(" draws one help canvas"), Canvases.Num(), 1))
		{
			const float Scale = Canvases[0].GetAccumulatedLayoutTransform().GetScale();
			TestTrue(FString::Printf(TEXT("%s draws the help window at %g screen pixels per unit (actual %g)"),
				*Label, Case.ExpectedScale, Scale),
				FMath::IsNearlyEqual(Scale, Case.ExpectedScale, 0.001f));
			const FVector2D Drawn = Canvases[0].GetLocalSize() * Scale + FVector2D(128.0f, 48.0f) * Scale;
			TestTrue(Label + TEXT(" keeps the whole window on screen"),
				Drawn.X <= Case.PhysicalSize.X + 0.5f && Drawn.Y <= Case.PhysicalSize.Y + 0.5f);
			// Each line sits at its retail position by its top-left corner: the
			// canvas's default centring cut half of every line off the window.
			const FSlateRect HelpRect = Canvases[0].GetLayoutBoundingRect();
			TArray<FGeometry> Lines;
			CollectArrangedGeometryOfType(Help, Geometry, FName(TEXT("STextBlock")), Lines);
			TestTrue(Label + TEXT(" arranges the title, body and footer"), Lines.Num() >= 4);
			for (const FGeometry& Line : Lines)
			{
				const FSlateRect Rect = Line.GetLayoutBoundingRect();
				TestTrue(FString::Printf(
					TEXT("%s keeps a help line inside the window (line %g,%g; window %g,%g)"),
					*Label, Rect.Left, Rect.Top, HelpRect.Left, HelpRect.Top),
					Rect.Left >= HelpRect.Left - 0.5f && Rect.Top >= HelpRect.Top - 0.5f);
			}
		}
		// A window resized below 1280x960 drops to 1x on its next Tick.
		Help->Tick(FGeometry::MakeRoot(FVector2D(1280.0f, 720.0f), FSlateLayoutTransform(1.0f)),
			0.0, 0.0f);
		TestEqual(Label + TEXT(" follows a resize to 1280x720"), Help->PixelScaleForTesting(), 1);
	}
	TestEqual(TEXT("a 640x480 screen draws at 1x"),
		SWoTHelpOverlay::PixelScaleFor(FVector2D(640.0f, 480.0f)), 1);
	TestEqual(TEXT("scale never exceeds the 2x bitmap strike"),
		SWoTHelpOverlay::PixelScaleFor(FVector2D(7680.0f, 4320.0f)), 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndMultiplayerGeometrySpec,
	"WoT.FrontEnd.MultiplayerGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndMultiplayerGeometrySpec::RunTest(const FString&)
{
	// Layout only: entering Play auto-fetches LAN, and on a Mac its Local
	// Network heads-up would replace the disclosure this spec measures.
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	// Catches using the retail label/value offsets as two whole-panel columns,
	// and shrinking already-small menu fonts a second time with viewport DPI.
	const auto Feedback = MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	// Include the larger browser face: the entry list's 14pt stand-in masked
	// vertical clipping from forcing retail's 26-unit heading row height.
	const auto Authority = MakeInjectedPresentationAuthority(Feedback, TEXT("generation-input"), 20);
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(Authority)
		.ShowResetIdentityForTesting(true)
		.ShowOnlineDisclosureForTesting(true)
		.InternetStatusTextForTesting(FText::FromString(TEXT("Internet ready (layout test)")));
	Root->ActivateMainMenuEntryForTesting(0);
	TestTrue(TEXT("multiplayer action opens the real play screen"),
		Root->CurrentScreenForTesting() == EWoTFrontEndScreen::Play);
	const auto Row = MakeShared<FWoTSessionListEntry>();
	Row->ServerName = TEXT("Descender gy server");
	Row->MapName = TEXT("Descender gy map");
	TArray<TSharedPtr<FWoTSessionListEntry>> Rows{Row};
	const auto List = StaticCastSharedPtr<SListView<TSharedPtr<FWoTSessionListEntry>>>(
		Root->SessionListViewForTesting());
	List->SetItemsSource(&Rows);
	ON_SCOPE_EXIT { List->ClearItemsSource(); };
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(1280.0f, 720.0f))
		.CreateTitleBar(false).SupportsMaximize(false).SupportsMinimize(false)
		[Root];
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.AddWindow(Window, false);
	ON_SCOPE_EXIT { Slate.RequestDestroyWindow(Window); };
	struct FCase { FVector2D PhysicalSize; float Scale; };
	const FCase Cases[] = {
		{FVector2D(1280.0f, 720.0f), 2.0f / 3.0f},
		{FVector2D(1920.0f, 1080.0f), 1.0f},
		{FVector2D(1280.0f, 720.0f), 1.0f}
	};
	float TallViewportListHeight = 0.0f;
	for (const FCase& Case : Cases)
	{
		const FGeometry Geometry = FGeometry::MakeRoot(
			Case.PhysicalSize / Case.Scale, FSlateLayoutTransform(Case.Scale));
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			Root->Tick(Geometry, 0.0, 0.0f);
			Root->SlatePrepass(Case.Scale);
			FHittestGrid Grid;
			FSlateWindowElementList Elements(Window);
			const FPaintArgs Args(&Window.Get(), Grid,
				FVector2f::ZeroVector, FApp::GetCurrentTime(), 0.0f);
			Root->Paint(Args, Geometry, Geometry.GetLayoutBoundingRect(),
				Elements, 0, FWidgetStyle(), true);
			List->Tick(List->GetCachedGeometry(), 0.0, 0.0f);
		}
		const FString Label = FString::Printf(TEXT("%gx%g scale %g"),
			Case.PhysicalSize.X, Case.PhysicalSize.Y, Case.Scale);
		const FSlateRect ListBounds = List->GetCachedGeometry().GetLayoutBoundingRect();
		TArray<TSharedRef<SWidget>> Borders;
		CollectWidgetsOfType(Root, FName(TEXT("SBorder")), Borders);
		// The notices wear the same gold frame; only the Play panels count.
		TArray<TSharedRef<SWidget>> NoticeBorders;
		for (const TSharedPtr<SWidget>& Notice : {Root->OnlineDisclosureWidgetForTesting(),
			Root->LocalNetworkNoticeWidgetForTesting(),
			Root->LocalNetworkHeadsUpWidgetForTesting(),
			Root->KeychainHeadsUpWidgetForTesting()})
		{
			if (Notice.IsValid())
			{
				CollectWidgetsOfType(Notice.ToSharedRef(), FName(TEXT("SBorder")),
					NoticeBorders);
			}
		}
		TArray<FSlateRect> Frames;
		for (const auto& Widget : Borders)
			if (StaticCastSharedRef<SBorder>(Widget)->GetBorderImage()
				== Authority->BrowserStyle()->WindowFrame()
				&& !NoticeBorders.Contains(Widget))
				Frames.Add(Widget->GetCachedGeometry().GetLayoutBoundingRect());
		if (TestEqual(Label + TEXT(" two framed panels"), Frames.Num(), 2))
		{
			TestTrue(Label + TEXT(" panels share top and bottom edges"),
				FMath::IsNearlyEqual(Frames[0].Top, Frames[1].Top, 1.0f)
				&& FMath::IsNearlyEqual(Frames[0].Bottom, Frames[1].Bottom, 1.0f));
		}
		TArray<FGeometry> Sliders;
		CollectArrangedGeometryOfType(Root, Geometry, FName(TEXT("SWoTRetailSlider")), Sliders);
		if (TestEqual(Label + TEXT(" four sliders"), Sliders.Num(), 4))
		{
			// This spec's stand-in labels leave no room for a control beside
			// them, so every row wraps its slider under the label (the compact
			// three-column variant collided labels with their values at 2x).
			for (int32 Index = 1; Index < Sliders.Num(); ++Index)
			{
				const float Gap = Sliders[Index].GetAbsolutePosition().Y
					- (Sliders[Index - 1].GetAbsolutePosition().Y
						+ Sliders[Index - 1].GetAbsoluteSize().Y);
				TestTrue(Label + FString::Printf(
					TEXT(" slider rows are stacked with room for a label (gap=%g)"), Gap),
					Gap >= 12.0f * Case.Scale);
			}
			// The host panel scrolls (an SScrollBox) when its content outgrows
			// the window. The injected test fonts are much taller than the
			// retail bitmap faces, so rows may land below the frame here; the
			// real 1280x720 fit is checked on a packaged build, not in this spec.
		}
		// The list gets the remaining height after title/connection controls,
		// not an arbitrary fraction of the viewport. At fixed DPI every extra
		// vertical pixel must go into the list, not an unused bottom margin.
		const float ListHeight = ListBounds.Bottom - ListBounds.Top;
		if (Case.PhysicalSize.Y == 1080.0f) TallViewportListHeight = ListHeight;
		if (Case.PhysicalSize.Y == 720.0f && Case.Scale == 1.0f)
			TestTrue(Label + TEXT(" browser absorbs the full viewport height difference"),
				FMath::IsNearlyEqual(TallViewportListHeight - ListHeight, 360.0f, 1.0f));
		TArray<TSharedRef<SWidget>> Edits;
		CollectWidgetsOfType(Root, FName(TEXT("SEditableTextBox")), Edits);
		TestTrue(Label + TEXT(" has the host and direct-address controls"), Edits.Num() >= 2);
		if (Edits.Num() >= 2)
		{
			const FSlateRect HostBounds = Edits[0]->GetCachedGeometry().GetLayoutBoundingRect();
			TestTrue(Label + TEXT(" host input has readable panel width"),
				HostBounds.Right - HostBounds.Left >= 260.0f);
			TestTrue(Label + TEXT(" host panel uses the left side instead of an empty half-screen"),
				HostBounds.Left < Case.PhysicalSize.X * 0.25f);
		}
		TArray<TSharedRef<SWidget>> Texts;
		CollectWidgetsOfType(Root, FName(TEXT("STextBlock")), Texts);
		int32 BrowserHeadingCount = 0;
		int32 BrowserDescenderCellCount = 0;
		bool bFoundBack = false;
		bool bFoundFooter = false;
		bool bFoundBrowserStatus = false;
		bool bFoundResetOnlineIdentity = false;
		bool bFoundHostButton = false;
		int32 VisibleInternetStatusCount = 0;
		int32 RelayCopyCount = 0;
		int32 VisibleRelayCopyCount = 0;
		int32 EOSBodyCopyCount = 0;
		const float ExpectedEOSBodySize = Root->LargeBodyFontForTesting().Size;
		auto TestEOSBodyFont = [this, &Label, ExpectedEOSBodySize](
			const TSharedRef<STextBlock>& Text, const FString& Description)
		{
			const float Actual = Text->GetFont().Size;
			return TestTrue(Label + TEXT(" ") + Description
				+ FString::Printf(TEXT(" uses the 2x body face (actual=%g expected=%g)"),
					Actual, ExpectedEOSBodySize),
				FMath::IsNearlyEqual(Actual, ExpectedEOSBodySize, 0.01f));
		};
		TestTrue(Label + TEXT(" internet status widget exists"),
			Root->InternetStatusTextWidgetForTesting().IsValid());
		if (Root->InternetStatusTextWidgetForTesting().IsValid())
		{
			TestEOSBodyFont(Root->InternetStatusTextWidgetForTesting().ToSharedRef(),
				TEXT("internet status"));
		}
		TestTrue(Label + TEXT(" Reset online identity widget exists"),
			Root->ResetIdentityTextWidgetForTesting().IsValid());
		if (Root->ResetIdentityTextWidgetForTesting().IsValid())
		{
			TestEOSBodyFont(Root->ResetIdentityTextWidgetForTesting().ToSharedRef(),
				TEXT("Reset online identity"));
		}
		TestTrue(Label + TEXT(" disclosure widget exists"),
			Root->OnlineDisclosureWidgetForTesting().IsValid());
		FSlateRect DisclosureBounds;
		if (Root->OnlineDisclosureWidgetForTesting().IsValid())
		{
			DisclosureBounds = Root->OnlineDisclosureWidgetForTesting()
				->GetCachedGeometry().GetLayoutBoundingRect();
			TestTrue(Label + TEXT(" disclosure is visible and fits on screen"),
				DisclosureBounds.GetSize().X > 0.0f
				&& DisclosureBounds.GetSize().Y > 0.0f
				&& DisclosureBounds.Left >= 8.0f && DisclosureBounds.Top >= 8.0f
				&& DisclosureBounds.Right <= Case.PhysicalSize.X - 8.0f
				&& DisclosureBounds.Bottom <= Case.PhysicalSize.Y - 8.0f);
		}
		for (const TSharedRef<SWidget>& Widget : Texts)
		{
			const TSharedRef<STextBlock> Text = StaticCastSharedRef<STextBlock>(Widget);
			const FString Value = Text->GetText().ToString();
			const FSlateRect Bounds = Text->GetCachedGeometry().GetLayoutBoundingRect();
			if (Value == TEXT("Reset online identity"))
			{
				bFoundResetOnlineIdentity = true;
			}
			if (Value == TEXT("Internet ready (layout test)"))
			{
				++VisibleInternetStatusCount;
				TestEOSBodyFont(Text, TEXT("populated internet status"));
				TestTrue(Label + TEXT(" populated internet status fits on screen"),
					Bounds.GetSize().X > 0.0f && Bounds.GetSize().Y > 0.0f
					&& Bounds.Left >= 8.0f && Bounds.Top >= 8.0f
					&& Bounds.Right <= Case.PhysicalSize.X - 8.0f
					&& Bounds.Bottom <= Case.PhysicalSize.Y - 8.0f);
			}
			if (Value == TEXT("Host Match"))
			{
				// Present in the host panel (reachable by its scroll box).
				bFoundHostButton = true;
			}
			if (Value.Contains(TEXT("Always use Epic's relay - keeps your IP address hidden "
				"from other players, but may add some lag.")))
			{
				++RelayCopyCount;
				if (Bounds.GetSize().X > 0.0f && Bounds.GetSize().Y > 0.0f)
				{
					TestEOSBodyFont(Text, TEXT("relay copy"));
					++VisibleRelayCopyCount;
					TestTrue(Label + FString::Printf(TEXT(
						" relay control stays inside 1280x720 (bounds=%g,%g,%g,%g)"),
						Bounds.Left, Bounds.Top, Bounds.Right, Bounds.Bottom),
						Bounds.Left >= 8.0f && Bounds.Top >= 8.0f
						&& Bounds.Right <= Case.PhysicalSize.X - 8.0f
						&& Bounds.Bottom <= Case.PhysicalSize.Y - 8.0f);
				}
			}
			// Font size is checked whether or not the row is scrolled into view.
			const bool bEOSBodyCopy = (Value == TEXT("LAN") || Value == TEXT("Internet")
				|| Value == TEXT("Refresh Sessions")
				|| Value.StartsWith(TEXT("Internet play uses EOS"))
				|| Value == TEXT("Accept") || Value == TEXT("Cancel")
				|| Value.StartsWith(TEXT("To list and join internet games")));
			if (bEOSBodyCopy)
			{
				++EOSBodyCopyCount;
				TestEOSBodyFont(Text, TEXT("EOS-added text: ") + Value.Left(32));
				if (Value.StartsWith(TEXT("Internet play uses EOS"))
					|| Value == TEXT("Accept") || Value == TEXT("Cancel")
					|| Value.StartsWith(TEXT("To list and join internet games")))
				{
					TestTrue(Label + TEXT(" disclosure text/control fits its panel: ")
						+ Value.Left(32) + FString::Printf(TEXT(" child=%g,%g,%g,%g panel=%g,%g,%g,%g"),
							Bounds.Left, Bounds.Top, Bounds.Right, Bounds.Bottom,
							DisclosureBounds.Left, DisclosureBounds.Top,
							DisclosureBounds.Right, DisclosureBounds.Bottom),
						Bounds.GetSize().X > 0.0f && Bounds.GetSize().Y > 0.0f
						&& Bounds.Left >= DisclosureBounds.Left
						&& Bounds.Top >= DisclosureBounds.Top
						&& Bounds.Right <= DisclosureBounds.Right
						&& Bounds.Bottom <= DisclosureBounds.Bottom);
				}
			}
			if (Value == TEXT("0 Servers, 0 Players"))
			{
				bFoundBrowserStatus = true;
				TestTrue(Label + TEXT(" browser reaches the bottom footer area"),
					Frames.Num() == 2 && Bounds.Bottom >= Frames[1].Bottom - 20.0f
					&& Bounds.Bottom <= Case.PhysicalSize.Y - 8.0f);
			}
			if (Value.StartsWith(TEXT("Descender gy")))
			{
				++BrowserDescenderCellCount;
				const float RequiredHeight = Slate.GetRenderer()->GetFontMeasureService()
					->Measure(Text->GetText(), Text->GetFont(), Case.Scale).Y;
				TestTrue(Label + TEXT(" populated browser cell has room for descenders: ") + Value,
					Bounds.Bottom - Bounds.Top + 1.0f >= RequiredHeight);
			}
			const bool bIsFooter = Value.StartsWith(TEXT("No playable maps"))
				|| Value == FrontEndFixtureLabel(26, TEXT('J'));
			bFoundFooter |= bIsFooter;
			if (Value == TEXT("Back") || bIsFooter)
			{
				if (Value == TEXT("Back"))
					TestTrue(Label + TEXT(" Back is in the lower left"),
						Bounds.Left < Case.PhysicalSize.X * 0.1f
						&& Bounds.Bottom >= Case.PhysicalSize.Y - 32.0f);
				bFoundBack |= Value == TEXT("Back");
				TestTrue(Label + TEXT(" control/footer font remains legible at viewport scale"),
					Text->GetFont().Size * Case.Scale >= 14.0f - 0.01f);
				TestTrue(Label + TEXT(" control/footer stays inside padded screen bounds"),
					Bounds.Top >= 8.0f && Bounds.Bottom <= Case.PhysicalSize.Y - 8.0f);
			}
			if (Value == TEXT("Server") || Value == TEXT("Ping")
				|| Value == TEXT("Map") || Value == TEXT("Players"))
			{
				++BrowserHeadingCount;
				const FVector2D RequiredSize = FSlateApplication::Get().GetRenderer()
					->GetFontMeasureService()->Measure(Text->GetText(), Text->GetFont(), Case.Scale);
				const float RequiredWidth = RequiredSize.X;
				TestTrue(Label + TEXT(" browser heading is not clipped: ") + Value
					+ FString::Printf(TEXT(" (allotted=%g required=%g)"), Bounds.Right - Bounds.Left, RequiredWidth),
					Bounds.Right - Bounds.Left + 1.0f >= RequiredWidth);
				TestTrue(Label + TEXT(" browser heading has room for descenders: ") + Value
					+ FString::Printf(TEXT(" (allotted=%g required=%g)"), Bounds.Bottom - Bounds.Top, RequiredSize.Y),
					Bounds.Bottom - Bounds.Top + 1.0f >= RequiredSize.Y);
				TestTrue(Label + TEXT(" browser heading stays on screen: ") + Value,
					Bounds.Right <= Case.PhysicalSize.X - 8.0f);
			}
		}
		TestTrue(Label + TEXT(" back control was examined"), bFoundBack);
		TestTrue(Label + TEXT(" status footer was examined"), bFoundFooter);
		TestTrue(Label + TEXT(" browser status was examined"), bFoundBrowserStatus);
		// No two visible text blocks inside the host panel may overlap: the
		// compact reflow once drew "Server Name" over its value at 2x.
		if (Frames.Num() == 2)
		{
			const FSlateRect Host = Frames[0].Left <= Frames[1].Left ? Frames[0] : Frames[1];
			TArray<TPair<FString, FSlateRect>> HostTexts;
			for (const TSharedRef<SWidget>& Widget : Texts)
			{
				const TSharedRef<STextBlock> Text = StaticCastSharedRef<STextBlock>(Widget);
				const FSlateRect Bounds = Text->GetCachedGeometry().GetLayoutBoundingRect();
				if (Bounds.GetSize().X <= 0.0f || Bounds.GetSize().Y <= 0.0f
					|| Text->GetText().IsEmpty()
					|| Bounds.Left < Host.Left || Bounds.Right > Host.Right
					|| Bounds.Top < Host.Top || Bounds.Bottom > Host.Bottom) continue;
				HostTexts.Emplace(Text->GetText().ToString(), Bounds);
			}
			TestTrue(Label + TEXT(" host panel texts were examined"), HostTexts.Num() >= 8);
			for (int32 A = 0; A < HostTexts.Num(); ++A)
				for (int32 B = A + 1; B < HostTexts.Num(); ++B)
				{
					const FSlateRect& RA = HostTexts[A].Value;
					const FSlateRect& RB = HostTexts[B].Value;
					const float OverlapX = FMath::Min(RA.Right, RB.Right) - FMath::Max(RA.Left, RB.Left);
					const float OverlapY = FMath::Min(RA.Bottom, RB.Bottom) - FMath::Max(RA.Top, RB.Top);
					TestFalse(Label + FString::Printf(TEXT(" host texts do not overlap: '%s' / '%s'"),
						*HostTexts[A].Key.Left(24), *HostTexts[B].Key.Left(24)),
						OverlapX > 1.0f && OverlapY > 1.0f);
				}
		}
		TestTrue(Label + TEXT(" Reset online identity was examined"),
			bFoundResetOnlineIdentity);
		// The status line and relay option live once, in the browser panel.
		TestEqual(Label + TEXT(" the populated internet status line was examined"),
			VisibleInternetStatusCount, 1);
		TestTrue(Label + TEXT(" Host Match is in the host panel"), bFoundHostButton);
		TestEqual(Label + TEXT(" all visible non-relay EOS-added text was examined"),
			EOSBodyCopyCount, 9);
		TestEqual(Label + TEXT(" relay copy appears in the browser and the disclosure"),
			RelayCopyCount, 2);
		TestEqual(Label + TEXT(" browser and disclosure relay copy is visible"),
			VisibleRelayCopyCount, 2);
		TestEqual(Label + TEXT(" all browser headings were examined"), BrowserHeadingCount, 4);
		TestEqual(Label + TEXT(" populated browser cells were examined"), BrowserDescenderCellCount, 2);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndLocalNetworkNoticeSpec,
	"WoT.FrontEnd.LocalNetworkNotice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndLocalNetworkNoticeSpec::RunTest(const FString&)
{
	EWoTLocalNetworkAccess ProbeResult = EWoTLocalNetworkAccess::Denied;
	int32 ProbeCount = 0;
	int32 SettingsOpened = 0;
	WoTLocalNetworkAccess::SetProbeOverrideForTesting([&]()
	{
		++ProbeCount;
		return ProbeResult;
	});
	WoTLocalNetworkAccess::SetOpenSettingsOverrideForTesting(
		[&SettingsOpened]() { ++SettingsOpened; });
	ON_SCOPE_EXIT
	{
		WoTLocalNetworkAccess::SetProbeOverrideForTesting({});
		WoTLocalNetworkAccess::SetOpenSettingsOverrideForTesting({});
	};

	const auto Feedback = MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(MakeInjectedPresentationAuthority(Feedback));
	Root->ActivateMainMenuEntryForTesting(0);
	TestTrue(TEXT("the notice is built with the play screen"),
		Root->LocalNetworkNoticeWidgetForTesting().IsValid());
	TestFalse(TEXT("the notice starts hidden"),
		Root->IsLocalNetworkNoticeVisibleForTesting());

	// A denied, empty LAN search: the masked "No sessions found." case.
	Root->BeginSearchForTesting();
	Root->CompleteSearchForTesting(true, {});
	TestEqual(TEXT("an empty LAN search probes once"), ProbeCount, 1);
	TestTrue(TEXT("a denial opens the notice"),
		Root->IsLocalNetworkNoticeVisibleForTesting());
	TestTrue(TEXT("the status line names the cause"),
		Root->ViewModelForTesting().IsStatusError()
			&& Root->ViewModelForTesting().GetStatusText().Contains(
				TEXT("Local network access is off")));
	Root->OpenLocalNetworkSettingsForTesting();
	TestEqual(TEXT("Open Settings launches the Local Network pane"),
		SettingsOpened, 1);
	TestFalse(TEXT("Open Settings closes the notice"),
		Root->IsLocalNetworkNoticeVisibleForTesting());

	// Results prove the network works: no probe, no notice.
	Root->BeginSearchForTesting();
	Root->CompleteSearchForTesting(true, {FWoTSessionListEntry()});
	TestEqual(TEXT("a non-empty search does not probe"), ProbeCount, 1);
	TestFalse(TEXT("found sessions never show the notice"),
		Root->IsLocalNetworkNoticeVisibleForTesting());

	// Allowed: a genuinely empty LAN keeps the plain message.
	ProbeResult = EWoTLocalNetworkAccess::Allowed;
	Root->BeginSearchForTesting();
	Root->CompleteSearchForTesting(true, {});
	TestFalse(TEXT("an allowed empty search stays quiet"),
		Root->IsLocalNetworkNoticeVisibleForTesting());

	// OK dismisses without opening anything.
	ProbeResult = EWoTLocalNetworkAccess::Denied;
	Root->BeginSearchForTesting();
	Root->CompleteSearchForTesting(true, {});
	TestTrue(TEXT("the notice reopens on the next denied search"),
		Root->IsLocalNetworkNoticeVisibleForTesting());
	Root->DismissLocalNetworkNoticeForTesting();
	TestFalse(TEXT("OK closes the notice"),
		Root->IsLocalNetworkNoticeVisibleForTesting());
	TestEqual(TEXT("OK does not open settings"), SettingsOpened, 1);

	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndLocalNetworkHeadsUpSpec,
	"WoT.FrontEnd.LocalNetworkHeadsUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndLocalNetworkHeadsUpSpec::RunTest(const FString&)
{
	int32 ProbeCount = 0;
	WoTLocalNetworkAccess::SetProbeOverrideForTesting([&ProbeCount]()
	{
		++ProbeCount;
		return EWoTLocalNetworkAccess::Allowed;
	});
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(true);
	ON_SCOPE_EXIT
	{
		WoTLocalNetworkAccess::SetProbeOverrideForTesting({});
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({});
	};

	const auto Feedback = MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(MakeInjectedPresentationAuthority(Feedback));
	Root->ActivateMainMenuEntryForTesting(0);
	TestTrue(TEXT("the heads-up is built with the play screen"),
		Root->LocalNetworkHeadsUpWidgetForTesting().IsValid());
	// Entering Play lists the LAN at once, so the first search is the
	// auto-fetch: the OS prompt is explained before anything is sent (the
	// denial probe is itself local traffic that would raise the prompt).
	TestTrue(TEXT("entering Play auto-fetches LAN and opens the heads-up"),
		Root->IsLocalNetworkHeadsUpVisibleForTesting());
	TestEqual(TEXT("nothing probes the network behind the heads-up"), ProbeCount, 0);
	Root->CancelLocalNetworkHeadsUpForTesting();
	TestFalse(TEXT("Cancel closes the heads-up"),
		Root->IsLocalNetworkHeadsUpVisibleForTesting());

	// Switching back to LAN lists it again, so the heads-up returns with it.
	Root->SetInternetModeForTesting();
	Root->SelectNetworkModeForTesting(false);
	TestTrue(TEXT("switching to LAN auto-fetches and explains again"),
		Root->IsLocalNetworkHeadsUpVisibleForTesting());
	Root->CancelLocalNetworkHeadsUpForTesting();

	// Cancel is not "seen": the next LAN action asks again.
	Root->HostForTesting();
	TestTrue(TEXT("a cancelled heads-up returns on the next LAN action"),
		Root->IsLocalNetworkHeadsUpVisibleForTesting());
	Root->ContinueLocalNetworkHeadsUpForTesting();
	TestFalse(TEXT("Continue closes the heads-up"),
		Root->IsLocalNetworkHeadsUpVisibleForTesting());

	Root->RefreshSessionsForTesting();
	Root->HostForTesting();
	TestFalse(TEXT("once continued it never shows again"),
		Root->IsLocalNetworkHeadsUpVisibleForTesting());

	// Platforms whose OS never asks show nothing.
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	const TSharedRef<SWoTFrontEndRoot> Quiet = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(MakeInjectedPresentationAuthority(Feedback));
	Quiet->ActivateMainMenuEntryForTesting(0);
	Quiet->RefreshSessionsForTesting();
	TestFalse(TEXT("no OS prompt, no heads-up"),
		Quiet->IsLocalNetworkHeadsUpVisibleForTesting());
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndKeychainHeadsUpSpec,
	"WoT.FrontEnd.KeychainHeadsUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndKeychainHeadsUpSpec::RunTest(const FString&)
{
	const auto Feedback = MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(MakeInjectedPresentationAuthority(Feedback));
	Root->ActivateMainMenuEntryForTesting(0);
	TestTrue(TEXT("the keychain heads-up is built with the play screen"),
		Root->KeychainHeadsUpWidgetForTesting().IsValid());

	// LAN never signs in to Epic: no keychain heads-up even when one is due.
	Root->SetKeychainAccessForTesting(EWoTKeychainAccess::WillPrompt);
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	Root->RefreshSessionsForTesting();
	TestFalse(TEXT("LAN refresh skips the keychain heads-up"),
		Root->IsKeychainHeadsUpVisibleForTesting());

	Root->SetInternetModeForTesting();
	Root->RefreshSessionsForTesting();
	TestTrue(TEXT("an internet refresh that will prompt explains it first"),
		Root->IsKeychainHeadsUpVisibleForTesting());
	Root->CancelKeychainHeadsUpForTesting();
	TestFalse(TEXT("Not Now closes it"), Root->IsKeychainHeadsUpVisibleForTesting());

	Root->HostForTesting();
	TestTrue(TEXT("an internet host is gated too"),
		Root->IsKeychainHeadsUpVisibleForTesting());
	Root->ContinueKeychainHeadsUpForTesting();
	TestFalse(TEXT("Continue closes it"), Root->IsKeychainHeadsUpVisibleForTesting());
	TestEqual(TEXT("Continue replays the gated action exactly once"),
		Root->KeychainReplaysForTesting(), 1);
	Root->ContinueKeychainHeadsUpForTesting();
	TestEqual(TEXT("a second Continue with nothing pending replays nothing"),
		Root->KeychainReplaysForTesting(), 1);

	// Each sign-in that would prompt is explained (plain Allow covers one read).
	Root->RefreshSessionsForTesting();
	TestTrue(TEXT("a later prompting sign-in is explained again"),
		Root->IsKeychainHeadsUpVisibleForTesting());
	Root->CancelKeychainHeadsUpForTesting();

	// Leaving Play drops the notice: Continue must not replay it later.
	Root->RefreshSessionsForTesting();
	TestTrue(TEXT("shown again before Back"), Root->IsKeychainHeadsUpVisibleForTesting());
	Root->BackFromPlayForTesting();
	TestFalse(TEXT("Back drops the pending heads-up"),
		Root->IsKeychainHeadsUpVisibleForTesting());
	Root->ContinueKeychainHeadsUpForTesting();
	TestEqual(TEXT("a dropped heads-up is never replayed"),
		Root->KeychainReplaysForTesting(), 1);
	Root->ActivateMainMenuEntryForTesting(0);
	Root->SetInternetModeForTesting();

	Root->SetKeychainAccessForTesting(EWoTKeychainAccess::NoPrompt);
	Root->RefreshSessionsForTesting();
	TestFalse(TEXT("no prompt coming, no heads-up"),
		Root->IsKeychainHeadsUpVisibleForTesting());
	Root->SetKeychainAccessForTesting(EWoTKeychainAccess::Unknown);
	Root->RefreshSessionsForTesting();
	TestFalse(TEXT("an undecided probe stays quiet"),
		Root->IsKeychainHeadsUpVisibleForTesting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndBackDuringSearchSpec,
	"WoT.FrontEnd.BackDuringSearch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndBackDuringSearchSpec::RunTest(const FString&)
{
	// Play auto-fetches on entry, so Back must not wait for the search.
	FWoTFrontEndViewModel Model;
	Model.ShowPlay({});
	TestTrue(TEXT("a search starts"), Model.BeginSearch());
	TestTrue(TEXT("Back leaves Play while only searching"), Model.BackToMain());
	TestTrue(TEXT("Back reached Main"), Model.GetScreen() == EWoTFrontEndScreen::Main);
	TestTrue(TEXT("the abandoned search no longer holds the screen"),
		Model.GetOperation() == EWoTFrontEndAsyncOperation::None);
	Model.FinishSearch(true, {FWoTSessionListEntry()});
	TestEqual(TEXT("a late result from the abandoned search is ignored"),
		Model.GetSessions().Num(), 0);

	Model.ShowPlay({});
	TestTrue(TEXT("a join starts"), Model.BeginJoin(TEXT("WoT Server")));
	TestFalse(TEXT("Back still waits for a join"), Model.BackToMain());
	TestTrue(TEXT("the join keeps Play"), Model.GetScreen() == EWoTFrontEndScreen::Play);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSwitchDuringSearchSpec,
	"WoT.FrontEnd.SwitchDuringSearch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSwitchDuringSearchSpec::RunTest(const FString&)
{
	// Play lists the LAN on entry: the network choice and Back must not wait
	// for that search, while host/join still hold them.
	FWoTFrontEndViewModel Model;
	Model.ShowPlay({});
	TestTrue(TEXT("idle allows switching"), Model.CanLeaveOrSwitchNetwork());
	TestTrue(TEXT("a search starts"), Model.BeginSearch());
	TestTrue(TEXT("a running search allows switching"), Model.CanLeaveOrSwitchNetwork());
	Model.AbandonSearch();
	TestTrue(TEXT("abandoning frees the screen"),
		Model.GetOperation() == EWoTFrontEndAsyncOperation::None);
	Model.FinishSearch(true, {FWoTSessionListEntry()});
	TestEqual(TEXT("the abandoned search's late result is ignored"),
		Model.GetSessions().Num(), 0);
	TestTrue(TEXT("a join starts"), Model.BeginJoin(TEXT("WoT Server")));
	TestFalse(TEXT("a join holds the network choice"), Model.CanLeaveOrSwitchNetwork());

	// Host and Direct Connect cut the running search short (otherwise Host
	// stayed disabled for seconds after entering Multiplayer).
	FWoTFrontEndViewModel HostModel;
	HostModel.ShowPlay({{TEXT("/Game/WoT/Maps/Arena_02"), TEXT("Arena_02"), false}});
	TestTrue(TEXT("the entry search starts"), HostModel.BeginSearch());
	TestTrue(TEXT("a running search leaves Host enabled"), HostModel.CanHost());
	TestTrue(TEXT("Host starts during a search"), HostModel.BeginHost());
	TestTrue(TEXT("hosting replaces the search"),
		HostModel.GetOperation() == EWoTFrontEndAsyncOperation::Hosting);
	HostModel.FinishSearch(true, {FWoTSessionListEntry()});
	TestTrue(TEXT("the cut search's late result leaves the host alone"),
		HostModel.GetOperation() == EWoTFrontEndAsyncOperation::Hosting
		&& HostModel.GetSessions().IsEmpty());
	HostModel.FinishHost(false);
	TestTrue(TEXT("a search starts again"), HostModel.BeginSearch());
	TestTrue(TEXT("a running search leaves Direct Connect enabled"),
		HostModel.IsIdleOrSearching());
	TestTrue(TEXT("Direct Connect starts during a search"),
		HostModel.BeginDirectConnect());
	TestTrue(TEXT("connecting replaces the search"),
		HostModel.GetOperation() == EWoTFrontEndAsyncOperation::Connecting);
	TestFalse(TEXT("a connect holds Host"), HostModel.CanHost());
	TestFalse(TEXT("a connect holds Direct Connect"), HostModel.BeginDirectConnect());

	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT { WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({}); };
	const auto Feedback = MakeShared<FWoTFrontEndPresentationFeedbackRecorder>();
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(MakeInjectedPresentationAuthority(Feedback));
	Root->ActivateMainMenuEntryForTesting(0);
	Root->SetInternetModeForTesting();
	Root->BeginSearchForTesting();
	Root->SelectNetworkModeForTesting(false);
	TestFalse(TEXT("switching to LAN mid-search takes effect at once"),
		Root->ViewModelForTesting().IsInternet());
	Root->BeginSearchForTesting();
	Root->CutSearchShortForTesting();
	TestTrue(TEXT("cutting a search short frees the screen"),
		Root->ViewModelForTesting().GetOperation() == EWoTFrontEndAsyncOperation::None);
	TestFalse(TEXT("a cut search is not restarted when it lands"),
		Root->IsSearchRestartPendingForTesting());
	return !HasAnyErrors();
}
#endif // WITH_AUTOMATION_TESTS
