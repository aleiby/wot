// Focused parity regressions.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "WoTEarlyUI.h"
#include "../WoTBootstrapHeartbeat.h"
#include "WoTLaunchEngineHooks.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTGameMode.h"
#include "../WoTMenuStyle.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTRetailText.h"
#include "../WoTSessionSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/ChildConnection.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Net/OnlineEngineInterface.h"
#include "OnlineSubsystemNames.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "UObject/UnrealType.h"

namespace
{
	class FFix6_PresentationFeedback final : public IWoTFrontEndFeedbackSink
	{
	public:
		virtual bool IsReady() const override { return true; }
		virtual void MoveSelector() override {}
		virtual void Select() override {}
	};

	TSharedRef<FWoTFrontEndPresentationAuthority>
	Fix6_MakePresentationAuthority()
	{
		TMap<FName, FString> Labels;
		Labels.Add(FName(TEXT("menuMain.MenuList[2]")), TEXT("Multiplayer"));
		Labels.Add(FName(TEXT("menuMain.MenuList[3]")), TEXT("Controls"));
		Labels.Add(FName(TEXT("menuMain.MenuList[4]")), TEXT("Hardware"));
		Labels.Add(FName(TEXT("menuMain.MenuList[7]")), TEXT("Quit"));
		return FWoTFrontEndPresentationAuthority::CreateForTesting(
			TEXT("fix6-password"),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
			MoveTemp(Labels), MakeShared<FFix6_PresentationFeedback>());
	}

	void Fix6_CollectWidgetsOfType(const TSharedRef<SWidget>& Widget,
		FName Type, TArray<TSharedRef<SWidget>>& Out)
	{
		if (Widget->GetType() == Type)
		{
			Out.Add(Widget);
		}
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				Fix6_CollectWidgetsOfType(Children->GetChildAt(Index), Type, Out);
			}
		}
	}

	bool Fix6_PublishEarlyUI()
	{
		FWoTEarlyUIAuthority& Authority = GetEarlyUIAuthority();
		Authority.ResetForTesting();
		static constexpr ANSICHAR Source[] =
			"Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"
			"ReducedMotion=0\nHighContrast=0\nScreenReader=0\n"
			"BootstrapVolume=1.0\nInputSensitivity=1.0\n";
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Source),
			UE_ARRAY_COUNT(Source) - 1);
		TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
			Bytes, FWoTEarlyUILimits::Production(),
			FWoTInventoryLocaleSet::Production());
		if (Parsed.IsError()
			|| Authority.PublishOnce(MoveTemp(Parsed).GetValue()).IsError())
		{
			return false;
		}
		const auto Apply = [](uint16, uint16, EWoTWindowMode)
			-> FWoTBootstrapStatus
		{
			return FWoTBootstrapStatus::Success();
		};
		const FWoTEarlyUINativeProjectionSink Sink{Apply};
		FString Error;
		return WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			Sink, &Error);
	}

	struct FFix6_PasswordFixture
	{
		FString Directory;
		FString PreferenceLeaf;
		UWorld* World = nullptr;
		AWoTFrontEndPlayerController* Controller = nullptr;
		UWoTSessionSubsystem* Sessions = nullptr;
		TArray<FString> ClientTravels;

		bool Create(FAutomationTestBase& Test, bool bWithSavedPassword)
		{
			Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
				FString(TEXT("Fix6Password-")) + FGuid::NewGuid().ToString());
			PreferenceLeaf = FPaths::Combine(
				Directory, TEXT("WoTUserPreferences"));
			IFileManager::Get().MakeDirectory(*Directory, true);
			UWoTPreferencesSubsystem::SetPreferencePathForTesting(PreferenceLeaf);
			UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
			if (!Fix6_PublishEarlyUI())
			{
				Test.AddError(TEXT("Fix6 EarlyUI fixture publication failed"));
				return false;
			}

			FWoTPlayerPreferences Value;
			TArray<uint8> Bytes =
				UWoTPreferencesSubsystem::SerializeForTesting(Value);
			if (bWithSavedPassword)
			{
				static constexpr ANSICHAR Saved[] =
					"SavedPassword0Server=127.0.0.1:7777\n"
					"SavedPassword0Value=memory\n";
				Bytes.Append(reinterpret_cast<const uint8*>(Saved),
					UE_ARRAY_COUNT(Saved) - 1);
			}
			if (!FFileHelper::SaveArrayToFile(Bytes, *PreferenceLeaf))
			{
				Test.AddError(TEXT("Fix6 isolated preference fixture write failed"));
				return false;
			}

			WoTTest::FSpecWorldOptions Options;
			Options.GameInstanceClass = UWoTGameInstance::StaticClass();
			World = WoTTest::CreateSpecWorld(Options);
			if (!World)
			{
				Test.AddError(TEXT("Fix6 password world creation failed"));
				return false;
			}
			UGameInstance* Instance = World->GetGameInstance();
			UWoTPreferencesSubsystem* Preferences = Instance
				? Instance->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
			if (!Preferences || Preferences->ApplyFullPreferencesAfterCacheActivation().IsError())
			{
				Test.AddError(TEXT("Fix6 isolated preferences did not load"));
				return false;
			}
			Sessions = Instance->GetSubsystem<UWoTSessionSubsystem>();
			Controller = World->SpawnActor<AWoTFrontEndPlayerController>();
			if (!Sessions || !Controller)
			{
				Test.AddError(TEXT("Fix6 password client fixture is incomplete"));
				return false;
			}
			Sessions->SetClientTravelControllerForTesting(Controller);
			Sessions->SetClientTravelHookForTesting(
				[this](APlayerController*, const FString& URL, ETravelType)
				{
					ClientTravels.Add(URL);
				});
			return true;
		}

		void Destroy()
		{
			if (Sessions)
			{
				Sessions->SetClientTravelHookForTesting({});
				Sessions->SetClientTravelControllerForTesting(nullptr);
			}
			if (World)
			{
				WoTTest::DestroySpecWorld(World);
				World = nullptr;
			}
			Sessions = nullptr;
			Controller = nullptr;
			UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false);
			UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
			GetEarlyUIAuthority().ResetForTesting();
			if (!Directory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Directory, false, true);
			}
		}
	};

	bool Fix6_WidgetTreeContainsText(const TSharedRef<SWidget>& Root,
		const FString& Expected)
	{
		TArray<TSharedRef<SWidget>> TextWidgets;
		Fix6_CollectWidgetsOfType(Root, FName(TEXT("STextBlock")), TextWidgets);
		for (const TSharedRef<SWidget>& Widget : TextWidgets)
		{
			if (StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString()
				== Expected)
			{
				return true;
			}
		}
		return false;
	}

	bool Fix6_ReadAdminFlag(FAutomationTestBase& Test,
		const AWoTPlayerState* State, bool& OutValue)
	{
		OutValue = false;
		if (!State)
		{
			Test.AddError(TEXT("Fix6 admin player state is absent"));
			return false;
		}
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(
			AWoTPlayerState::StaticClass(), FName(TEXT("bAdmin")));
		if (!Property)
		{
			Test.AddError(TEXT("Fix6 replicated bAdmin property is absent"));
			return false;
		}
		OutValue = Property->GetPropertyValue_InContainer(State);
		return true;
	}

	AWoTPlayerController* Fix6_SpawnRemotePlayer(UWorld* World,
		const TCHAR* Name, const TCHAR* StableId)
	{
		AWoTPlayerController* Controller = World
			? World->SpawnActor<AWoTPlayerController>() : nullptr;
		AWoTPlayerState* State = Controller
			? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
		if (!Controller || !State)
		{
			return nullptr;
		}
		State->SetPlayerName(Name);
		const FUniqueNetIdRef UniqueId =
			FUniqueNetIdString::Create(StableId,
				UOnlineEngineInterface::Get()->GetDefaultOnlineSubsystemName());
		State->SetUniqueId(FUniqueNetIdRepl(UniqueId));
		UChildConnection* Connection = NewObject<UChildConnection>(GEngine);
		Connection->PlayerController = Controller;
		Controller->Player = Connection;
		return Controller;
	}

	FUniqueNetIdRepl Fix6_UniqueId(const TCHAR* StableId)
	{
		const FUniqueNetIdRef UniqueId =
			FUniqueNetIdString::Create(StableId,
				UOnlineEngineInterface::Get()->GetDefaultOnlineSubsystemName());
		return FUniqueNetIdRepl(UniqueId);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_PasswordAdmissionSpec,
	"WoT.Parity.Fix6.Password.Admission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_PasswordAdmissionSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTGameMode::StaticClass();
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!TestNotNull(TEXT("password authority exists"), Game))
	{
		return false;
	}
	FString InitError;
	Game->InitGame(TEXT("Fix6Password"), TEXT("?GamePassword=secret"),
		InitError);
	TestTrue(TEXT("password authority accepts the host option"),
		InitError.IsEmpty());
	const FUniqueNetIdRepl InvalidId;
	FString EmptyError;
	Game->PreLogin(FString(), TEXT("127.0.0.1"), InvalidId, EmptyError);
	TestEqual(TEXT("an omitted game password returns the retail machine reason"),
		EmptyError, FString(TEXT("NEEDPW")));
	FString WrongError;
	Game->PreLogin(TEXT("?PasswordToken="
		"36947e6df297c2bbdebf1a9779d3e701fce4e2dc63e57ed4f42643ee3cb2fbe4"),
		TEXT("127.0.0.1"),
		InvalidId, WrongError);
	TestEqual(TEXT("a wrong game password returns the retail machine reason"),
		WrongError, FString(TEXT("WRONGPW")));
	FString CorrectError;
	Game->PreLogin(TEXT("?PasswordToken="
		"63cc3e764149aecf431941146613bef1f866fceba0824e30199b022e808f50cb"),
		TEXT("127.0.0.1"),
		InvalidId, CorrectError);
	TestTrue(TEXT("the correct game password admits the client"),
		CorrectError.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_PasswordPromptSpec,
	"WoT.Parity.Fix6.Password.Prompt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_PasswordPromptSpec::RunTest(const FString&)
{
	FFix6_PasswordFixture Fixture;
	ON_SCOPE_EXIT { Fixture.Destroy(); };
	if (!Fixture.Create(*this, /*bWithSavedPassword*/ false))
	{
		return false;
	}
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Fixture.Controller)
		.PresentationAuthorityForTesting(Fix6_MakePresentationAuthority());
	TestTrue(TEXT("the real direct-connect path starts"),
		Fixture.Sessions->ConnectDirect(TEXT("127.0.0.1:7777")));
	GEngine->OnNetworkFailure().Broadcast(Fixture.World, nullptr,
		ENetworkFailure::FailureReceived, TEXT("NEEDPW"));
	TestEqual(TEXT("the session retains the NEEDPW challenge"),
		static_cast<int32>(Fixture.Sessions->GetPasswordChallenge()),
		static_cast<int32>(EWoTPasswordChallenge::NeedPassword));
	TestEqual(TEXT("the password challenge remains on Multiplayer"),
		static_cast<int32>(Root->CurrentScreenForTesting()),
		static_cast<int32>(EWoTFrontEndScreen::Play));
	const TSharedPtr<SWidget> Focus = Root->InitialFocusWidget();
	TestTrue(TEXT("the password entry receives initial focus"),
		Focus.IsValid() && Focus->GetType() == FName(TEXT("SEditableTextBox")));
	FWoTRetailText::EnsureLoaded();
	const FString* NeedPassword = FWoTRetailText::FindSectionValue(
		TEXT("GameInfo"), TEXT("NeedPassword"));
	if (TestNotNull(TEXT("runtime Engine.int NEEDPW prompt exists"), NeedPassword))
	{
		TestTrue(TEXT("the prompt displays the runtime retail text"),
			Fix6_WidgetTreeContainsText(Root, *NeedPassword));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_PasswordWrongRepromptSpec,
	"WoT.Parity.Fix6.Password.WrongReprompt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_PasswordWrongRepromptSpec::RunTest(const FString&)
{
	FFix6_PasswordFixture Fixture;
	ON_SCOPE_EXIT { Fixture.Destroy(); };
	if (!Fixture.Create(*this, /*bWithSavedPassword*/ false))
	{
		return false;
	}
	TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(Fixture.Controller)
		.PresentationAuthorityForTesting(Fix6_MakePresentationAuthority());
	TestTrue(TEXT("the wrong-password client attempt starts"),
		Fixture.Sessions->ConnectDirect(TEXT("127.0.0.1:7777")));
	GEngine->OnNetworkFailure().Broadcast(Fixture.World, nullptr,
		ENetworkFailure::FailureReceived, TEXT("NEEDPW"));
	TestTrue(TEXT("the player can submit a password through the retained join"),
		Fixture.Sessions->SubmitServerPassword(TEXT("wrong")));
	if (Fixture.ClientTravels.Num() >= 2)
	{
		TestTrue(TEXT("the password prompt sends the derived token"),
			Fixture.ClientTravels[1].Contains(
				TEXT("?PasswordToken="
					"36947e6df297c2bbdebf1a9779d3e701fce4e2dc63e57ed4f42643ee3cb2fbe4"),
				ESearchCase::CaseSensitive));
		TestFalse(TEXT("the password prompt never sends plaintext"),
			Fixture.ClientTravels[1].Contains(TEXT("wrong"),
				ESearchCase::CaseSensitive));
		TestFalse(TEXT("the password prompt omits the legacy option"),
			Fixture.ClientTravels[1].Contains(TEXT("?Password="),
				ESearchCase::IgnoreCase));
	}
	GEngine->OnNetworkFailure().Broadcast(Fixture.World, nullptr,
		ENetworkFailure::FailureReceived, TEXT("WRONGPW"));
	TestEqual(TEXT("the session retains the WRONGPW challenge"),
		static_cast<int32>(Fixture.Sessions->GetPasswordChallenge()),
		static_cast<int32>(EWoTPasswordChallenge::WrongPassword));
	const TSharedPtr<SWidget> Focus = Root->InitialFocusWidget();
	TestTrue(TEXT("a wrong password reopens the focused password prompt"),
		Focus.IsValid() && Focus->GetType() == FName(TEXT("SEditableTextBox")));
	FWoTRetailText::EnsureLoaded();
	const FString* WrongPassword = FWoTRetailText::FindSectionValue(
		TEXT("GameInfo"), TEXT("WrongPassword"));
	if (TestNotNull(TEXT("runtime Engine.int WRONGPW prompt exists"), WrongPassword))
	{
		TestTrue(TEXT("the re-prompt displays the runtime retail text"),
			Fix6_WidgetTreeContainsText(Root, *WrongPassword));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_BaseSkinSpec,
	"WoT.Parity.Fix6.BaseSkin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_BaseSkinSpec::RunTest(const FString&)
{
	const EWoTPlayerSkinColor Base = static_cast<EWoTPlayerSkinColor>(4);
	const TArray<FWoTSkinChoice>& Choices = FWoTFrontEndModel::SkinChoices();
	TestEqual(TEXT("the retail skin list contains five entries"),
		Choices.Num(), 5);
	const int32 ExpectedOrder[] = {0, 1, 3, 2, 4};
	for (int32 Index = 0;
		Index < Choices.Num() && Index < UE_ARRAY_COUNT(ExpectedOrder); ++Index)
	{
		TestEqual(FString::Printf(TEXT("skin entry %d follows .int order"), Index),
			static_cast<int32>(Choices[Index].Color), ExpectedOrder[Index]);
	}
	TestEqual(TEXT("invalid skin records normalize to Base"),
		static_cast<int32>(FWoTFrontEndModel::NormalizeSkinColor(
			static_cast<EWoTPlayerSkinColor>(255))), 4);
	TestEqual(TEXT("new typed preferences default to Base"),
		static_cast<int32>(FWoTPlayerPreferences().SelectedSkinColor), 4);
	UWoTGameInstance* Instance = NewObject<UWoTGameInstance>();
	TestEqual(TEXT("the game-instance fallback defaults to Base"),
		static_cast<int32>(Instance->GetSelectedSkinColor()), 4);

	FWoTFrontEndViewModel ViewModel;
	ViewModel.ShowPlayerConfig(EWoTPlayerFaction::Hound, FString(),
		TEXT("Player"));
	TestEqual(TEXT("Player Config opens on Base by default"),
		static_cast<int32>(ViewModel.GetPendingSkinColor()), 4);
	ViewModel.CyclePendingSkinColor(1);
	TestEqual(TEXT("cycling forward wraps Base to Black"),
		static_cast<int32>(ViewModel.GetPendingSkinColor()), 0);
	ViewModel.CyclePendingSkinColor(-1);
	TestEqual(TEXT("cycling backward wraps Black to Base"),
		static_cast<int32>(ViewModel.GetPendingSkinColor()), 4);

	TestEqual(TEXT("Base remains a valid live skin option"),
		FWoTFrontEndModel::SkinOption(EWoTPlayerFaction::Hound, Base),
		FString(TEXT("HoundSkins.Hou_Base")));
	TestTrue(TEXT("the Base live skin option validates"),
		FWoTFrontEndModel::IsValidSkinOption(TEXT("HoundSkins.Hou_Base")));
	const FString Travel = FWoTFrontEndModel::AppendPlayerOptions(
		TEXT("127.0.0.1:7777?Skin=HoundSkins.Hou_Black"),
		EWoTPlayerFaction::Hound, Base);
	TestEqual(TEXT("Base travel omits Skin and preserves the class default"),
		Travel, FString(TEXT("127.0.0.1:7777?Class=WOTPawns.Hound")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_AdminLoginSpec,
	"WoT.Parity.Fix6.Admin.Login",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_AdminLoginSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!TestNotNull(TEXT("admin authority exists"), Game))
	{
		return false;
	}
	FString InitError;
	Game->InitGame(TEXT("Fix6Admin"),
		TEXT("?MaxPlayers=2?AdminPassword=AdminSecret"
			"?GamePassword=GameSecret"), InitError);
	TestTrue(TEXT("admin host options initialize"), InitError.IsEmpty());
	AWoTPlayerController* AdminPlayer =
		World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerController* GamePlayer =
		World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("admin login player exists"), AdminPlayer)
		|| !TestNotNull(TEXT("game login player exists"), GamePlayer)
		|| !TestNotNull(TEXT("admin login has a player state"),
			AdminPlayer ? AdminPlayer->GetPlayerState<AWoTPlayerState>() : nullptr)
		|| !TestNotNull(TEXT("game login has a player state"),
			GamePlayer ? GamePlayer->GetPlayerState<AWoTPlayerState>() : nullptr))
	{
		return false;
	}

	const FUniqueNetIdRepl InvalidId;
	FString AdminCapacityError;
	Game->PreLogin(TEXT("?PasswordToken="
		"3c102d489000460464d5c773b83c9d3db9b54689a44926ced5a2b5a45c802c71"),
		TEXT("127.0.0.1"),
		InvalidId, AdminCapacityError);
	TestTrue(TEXT("the admin password admits at capacity ignoring case"),
		AdminCapacityError.IsEmpty());
	FString GameCapacityError;
	Game->PreLogin(TEXT("?PasswordToken="
		"87e900ca7da6b140155d69757948093ef80615c0f980879d8c6ccd52dbc0d9ba"),
		TEXT("127.0.0.2"),
		InvalidId, GameCapacityError);
	TestTrue(TEXT("the game password also admits at capacity ignoring case"),
		GameCapacityError.IsEmpty());
	TestTrue(TEXT("admin password lookup ignores case at login"),
		Game->IsAdminPassword(TEXT(
			"3c102d489000460464d5c773b83c9d3db9b54689a44926ced5a2b5a45c802c71")));

	Game->InitNewPlayer(AdminPlayer, InvalidId,
		TEXT("?PasswordToken="
			"3c102d489000460464d5c773b83c9d3db9b54689a44926ced5a2b5a45c802c71"),
		FString());
	Game->InitNewPlayer(GamePlayer, InvalidId,
		TEXT("?PasswordToken="
			"87e900ca7da6b140155d69757948093ef80615c0f980879d8c6ccd52dbc0d9ba"),
		FString());
	bool bAdmin = false;
	if (Fix6_ReadAdminFlag(*this,
		AdminPlayer->GetPlayerState<AWoTPlayerState>(), bAdmin))
	{
		TestTrue(TEXT("the admin-password login grants replicated admin"), bAdmin);
	}
	bool bGameAdmin = true;
	if (Fix6_ReadAdminFlag(*this,
		GamePlayer->GetPlayerState<AWoTPlayerState>(), bGameAdmin))
	{
		TestFalse(TEXT("the game-password login does not grant admin"),
			bGameAdmin);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_AdminCommandsSpec,
	"WoT.Parity.Fix6.Admin.Commands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_AdminCommandsSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	if (!TestNotNull(TEXT("admin command authority exists"), Game))
	{
		return false;
	}
	FString InitError;
	Game->InitGame(TEXT("Fix6AdminCommands"),
		TEXT("?AdminPassword=AdminSecret"), InitError);
	AWoTPlayerController* Caller = World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerController* Victim = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("admin command caller exists"), Caller)
		|| !TestNotNull(TEXT("admin command victim exists"), Victim))
	{
		return false;
	}
	if (Caller->PlayerState) Caller->PlayerState->SetPlayerName(TEXT("Caller"));
	if (Victim->PlayerState) Victim->PlayerState->SetPlayerName(TEXT("Victim"));
	Caller->SetPlayer(NewObject<ULocalPlayer>(GEngine));
	bool bAdmin = true;
	if (Fix6_ReadAdminFlag(*this,
		Caller->GetPlayerState<AWoTPlayerState>(), bAdmin))
	{
		TestFalse(TEXT("the command caller starts non-admin"), bAdmin);
	}
	TestFalse(TEXT("the authority rejects a differently-cased explicit login"),
		Game->AdminLogin(Caller, TEXT("ADMINSECRET")));

	Caller->ConsoleCommand(TEXT("Kick Victim"), true);
	TestTrue(TEXT("a non-admin Kick does nothing"), IsValid(Victim));
	Caller->ConsoleCommand(TEXT("AdminLogin ADMINSECRET"), true);
	if (Fix6_ReadAdminFlag(*this,
		Caller->GetPlayerState<AWoTPlayerState>(), bAdmin))
	{
		TestFalse(TEXT("AdminLogin uses the retail exact comparison"), bAdmin);
	}
	Caller->ConsoleCommand(TEXT("AdminLogin AdminSecret"), true);
	if (Fix6_ReadAdminFlag(*this,
		Caller->GetPlayerState<AWoTPlayerState>(), bAdmin))
	{
		TestTrue(TEXT("the exact AdminLogin password grants admin"), bAdmin);
	}
	Caller->ConsoleCommand(TEXT("AdminLogout"), true);
	if (Fix6_ReadAdminFlag(*this,
		Caller->GetPlayerState<AWoTPlayerState>(), bAdmin))
	{
		TestFalse(TEXT("AdminLogout revokes replicated admin"), bAdmin);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_BanVoteSpec,
	"WoT.Parity.Fix6.Bans.Vote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_BanVoteSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix6VoteBan-")) + FGuid::NewGuid().ToString());
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	UWorld* World = nullptr;
	ON_SCOPE_EXIT
	{
		if (World) WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};

	World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* Victim = Fix6_SpawnRemotePlayer(
		World, TEXT("VoteVictim"), TEXT("fix6-vote-ban"));
	AWoTPlayerController* VoterA = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* VoterB = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	if (!TestNotNull(TEXT("vote-ban authority exists"), Game)
		|| !TestNotNull(TEXT("vote-ban victim exists"), Victim)
		|| !TestNotNull(TEXT("vote-ban first voter exists"), VoterA)
		|| !TestNotNull(TEXT("vote-ban second voter exists"), VoterB))
	{
		return false;
	}
	bool bRemoved = false;
	Game->SetPlayerRemovalHookForTesting(
		[&bRemoved](APlayerController* Player, const FText&)
		{
			bRemoved = true;
			Player->Destroy();
			return true;
		});
	Game->VoteRequest(VoterA, TEXT("KICKBAN VoteVictim"));
	Game->VoteRequest(VoterB, TEXT("KICKBAN VoteVictim"));
	World->Tick(ELevelTick::LEVELTICK_All, 0.01f);
	TestTrue(TEXT("a successful KICKBAN vote removes the player"), bRemoved);
	FString SameRunError;
	Game->PreLogin(FString(), TEXT("203.0.113.10"),
		Fix6_UniqueId(TEXT("fix6-vote-ban")), SameRunError);
	TestFalse(TEXT("the voted ban refuses the same stable identity"),
		SameRunError.IsEmpty());

	WoTTest::DestroySpecWorld(World);
	World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	FString ReloadError;
	if (TestNotNull(TEXT("persisted-ban replacement authority exists"), Game))
	{
		Game->PreLogin(FString(), TEXT("203.0.113.10"),
			Fix6_UniqueId(TEXT("fix6-vote-ban")), ReloadError);
		TestFalse(TEXT("the host ban persists across authority replacement"),
			ReloadError.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_BanAdminSpec,
	"WoT.Parity.Fix6.Bans.Admin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_BanAdminSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix6AdminBan-")) + FGuid::NewGuid().ToString());
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT
	{
		WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* AdminPlayer = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* Victim = Fix6_SpawnRemotePlayer(
		World, TEXT("AdminVictim"), TEXT("fix6-admin-ban"));
	if (!TestNotNull(TEXT("admin-ban authority exists"), Game)
		|| !TestNotNull(TEXT("admin-ban caller exists"), AdminPlayer)
		|| !TestNotNull(TEXT("admin-ban victim exists"), Victim))
	{
		return false;
	}
	bool bRemoved = false;
	Game->SetPlayerRemovalHookForTesting(
		[&bRemoved](APlayerController* Player, const FText&)
		{
			bRemoved = true;
			Player->Destroy();
			return true;
		});
	AdminPlayer->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
	AdminPlayer->SetPlayer(NewObject<ULocalPlayer>(GEngine));
	AdminPlayer->ConsoleCommand(TEXT("KickBan AdminVictim"), true);
	World->Tick(ELevelTick::LEVELTICK_All, 0.01f);
	TestTrue(TEXT("an admin KickBan removes the player"), bRemoved);
	FString Error;
	Game->PreLogin(FString(), TEXT("203.0.113.20"),
		Fix6_UniqueId(TEXT("fix6-admin-ban")), Error);
	TestFalse(TEXT("the admin ban refuses the same stable identity"),
		Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_KickReconnectSpec,
	"WoT.Parity.Fix6.Bans.KickReconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_KickReconnectSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("Fix6Kick-")) + FGuid::NewGuid().ToString());
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT
	{
		WoTTest::DestroySpecWorld(World);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* AdminPlayer = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* Victim = Fix6_SpawnRemotePlayer(
		World, TEXT("KickVictim"), TEXT("fix6-kick-only"));
	if (!TestNotNull(TEXT("kick authority exists"), Game)
		|| !TestNotNull(TEXT("kick caller exists"), AdminPlayer)
		|| !TestNotNull(TEXT("kick victim exists"), Victim))
	{
		return false;
	}
	bool bRemoved = false;
	Game->SetPlayerRemovalHookForTesting(
		[&bRemoved](APlayerController* Player, const FText&)
		{
			bRemoved = true;
			Player->Destroy();
			return true;
		});
	AdminPlayer->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
	AdminPlayer->SetPlayer(NewObject<ULocalPlayer>(GEngine));
	AdminPlayer->ConsoleCommand(TEXT("Kick KickVictim"), true);
	World->Tick(ELevelTick::LEVELTICK_All, 0.01f);
	TestTrue(TEXT("an admin Kick removes the player"), bRemoved);
	WoTTest::DestroySpecWorld(World);
	World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	FString Error;
	if (TestNotNull(TEXT("kick replacement authority exists"), Game))
	{
		Game->PreLogin(FString(), TEXT("203.0.113.30"),
			Fix6_UniqueId(TEXT("fix6-kick-only")), Error);
		TestEqual(TEXT("Kick without ban lets the same identity reconnect"),
			Error, FString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_DiagnosticPasswordRedactionSpec,
	"WoT.Parity.Fix6.Diagnostics.PasswordRedaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_DiagnosticPasswordRedactionSpec::RunTest(const FString&)
{
	FFix6_PasswordFixture Fixture;
	ON_SCOPE_EXIT { Fixture.Destroy(); };
	if (!Fixture.Create(*this, /*bWithSavedPassword*/ false))
	{
		return false;
	}
	const FString HeartbeatRoot = FPaths::Combine(
		Fixture.Directory, TEXT("HeartbeatRoot"));
	SetWoTBootstrapHeartbeatRootForTesting(HeartbeatRoot);
	ON_SCOPE_EXIT { SetWoTBootstrapHeartbeatRootForTesting(FString()); };
	Fixture.Sessions->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return true; });
	Fixture.Sessions->SetCreateSessionDispatchHookForTesting(
		[](TFunction<bool()>&&) { return false; });

	FWoTHostRequest Request;
	Request.MapPackage = TEXT("/Game/WoT/Maps/Arena_01");
	Request.ServerName = TEXT("Fix6 Diagnostic Host");
	Request.AdminPassword = TEXT("fix6-admin-secret");
	Request.GamePassword = TEXT("fix6-game-secret");
	Fixture.Sessions->Host(Request);

	FString Heartbeat;
	const FString HeartbeatPath = FPaths::Combine(HeartbeatRoot,
		TEXT("UpdaterState"), WoTBootstrapHeartbeatFileName());
	if (!TestTrue(TEXT("the real host path records a heartbeat"),
		FFileHelper::LoadFileToString(Heartbeat, *HeartbeatPath)))
	{
		return false;
	}
	TestFalse(TEXT("the heartbeat omits the admin password value"),
		Heartbeat.Contains(TEXT("fix6-admin-secret")));
	TestFalse(TEXT("the heartbeat omits the game password value"),
		Heartbeat.Contains(TEXT("fix6-game-secret")));
	TestFalse(TEXT("retained host diagnostics omit the admin password value"),
		Fixture.Sessions->GetLastHostTravelURL().Contains(
			TEXT("fix6-admin-secret")));
	TestFalse(TEXT("retained host diagnostics omit the game password value"),
		Fixture.Sessions->GetLastHostTravelURL().Contains(
			TEXT("fix6-game-secret")));
	TestTrue(TEXT("a direct connection starts before token diagnostic capture"),
		Fixture.Sessions->ConnectDirect(TEXT("127.0.0.1:7777")));
	const FString DiagnosticToken(
		TEXT("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
	GEngine->OnNetworkFailure().Broadcast(Fixture.World, nullptr,
		ENetworkFailure::FailureReceived,
		TEXT("refused ?PasswordToken=") + DiagnosticToken);
	TestFalse(TEXT("retained connection diagnostics omit the password token"),
		Fixture.Sessions->RetainedConnectionFailure().Contains(DiagnosticToken));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_SpectatorVoteSpec,
	"WoT.Parity.Fix6.Vote.Spectators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_SpectatorVoteSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld(AWoTGameMode::StaticClass());
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTGameMode* Game = World ? World->GetAuthGameMode<AWoTGameMode>() : nullptr;
	AWoTPlayerController* ActiveA = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* ActiveB = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* ActiveC = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	AWoTPlayerController* Spectator = World
		? World->SpawnActor<AWoTPlayerController>() : nullptr;
	if (!TestNotNull(TEXT("spectator-vote authority exists"), Game)
		|| !TestNotNull(TEXT("first active voter exists"), ActiveA)
		|| !TestNotNull(TEXT("second active voter exists"), ActiveB)
		|| !TestNotNull(TEXT("third active voter exists"), ActiveC)
		|| !TestNotNull(TEXT("spectator voter exists"), Spectator))
	{
		return false;
	}
	Spectator->PlayerState->SetIsOnlyASpectator(true);
	Spectator->Vote(TEXT("RESTARTSERVER"));
	TestEqual(TEXT("a spectator cannot open a vote"),
		Game->ActiveVoteRequestCountForTesting(), 0);
	ActiveA->Vote(TEXT("RESTARTSERVER"));
	ActiveB->Vote(TEXT("RESTARTSERVER"));
	TestEqual(TEXT("spectators are absent from the two-thirds denominator"),
		Game->LastExecutedVoteRequestForTesting(),
		FString(TEXT("RESTARTSERVER")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix6_PasswordSavedFirstSpec,
	"WoT.Parity.Fix6.Password.SavedFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFix6_PasswordSavedFirstSpec::RunTest(const FString&)
{
	FFix6_PasswordFixture Fixture;
	ON_SCOPE_EXIT { Fixture.Destroy(); };
	if (!Fixture.Create(*this, /*bWithSavedPassword*/ true))
	{
		return false;
	}
	TestTrue(TEXT("the saved-password client attempt starts"),
		Fixture.Sessions->ConnectDirect(TEXT("127.0.0.1:7777")));
	TestEqual(TEXT("the first attempt omits a password"),
		Fixture.ClientTravels.Num(), 1);
	GEngine->OnNetworkFailure().Broadcast(Fixture.World, nullptr,
		ENetworkFailure::FailureReceived, TEXT("NEEDPW"));
	TestEqual(TEXT("NEEDPW automatically retries once with the saved password"),
		Fixture.ClientTravels.Num(), 2);
	if (Fixture.ClientTravels.Num() >= 2)
	{
		TestTrue(TEXT("the automatic retry derives a token from the saved password"),
			Fixture.ClientTravels[1].Contains(TEXT("?PasswordToken="
				"1379a15ea680c44d90d7d43628b0f6c02346922224f6126328bb4012fc41a867"),
				ESearchCase::CaseSensitive));
		TestFalse(TEXT("the automatic retry does not expose the saved password"),
			Fixture.ClientTravels[1].Contains(TEXT("memory"),
				ESearchCase::CaseSensitive));
		TestFalse(TEXT("the automatic retry omits the legacy password option"),
			Fixture.ClientTravels[1].Contains(TEXT("?Password="),
				ESearchCase::IgnoreCase));
	}
	return true;
}

#endif
