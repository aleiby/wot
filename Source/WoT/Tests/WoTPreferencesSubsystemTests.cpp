#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTGameInstance.h"
#include "../WoTPreferencesSubsystem.h"
#include "../Payload/WoTPayloadActivation.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
constexpr EAutomationTestFlags WoTPreferencesSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> PreferencesEarlyUI(
	const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	return ParseEarlyUIV1(Bytes, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPreferencesTypedAdoptionSpec,
	"WoT.Preferences.TypedAdoption", WoTPreferencesSpecFlags)

bool FWoTPreferencesTypedAdoptionSpec::RunTest(const FString&)
{
	TestNull(TEXT("selected faction is no longer a reflected config property"),
		UWoTGameInstance::StaticClass()->FindPropertyByName(
			FName(TEXT("SelectedFaction"))));
	TestNull(TEXT("payload cache path is no longer a reflected config property"),
		UWoTGameInstance::StaticClass()->FindPropertyByName(
			FName(TEXT("PayloadCacheDir"))));
	UWoTGameInstance* Owner = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Preferences =
		NewObject<UWoTPreferencesSubsystem>(Owner);
	const FString PreviousCulture =
		FInternationalization::Get().GetCurrentCulture()->GetName();
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = PreferencesEarlyUI(
		"Schema=EarlyUI-v1\nUIScale=1.25\nLocale=fr-FR\n"
		"ReducedMotion=1\nHighContrast=1\nScreenReader=1\n"
		"BootstrapVolume=0.5\nInputSensitivity=1.75\n");
	if (Parsed.IsError())
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	FWoTBootstrapStatus Adopted = Preferences->AdoptAuthenticatedEarlyUI(
		MoveTemp(Parsed).GetValue());
	TestFalse(TEXT("authenticated adoption succeeds"), Adopted.IsError());
	TestTrue(TEXT("adoption state is observable"),
		Preferences->HasAdoptedAuthenticatedEarlyUI());
	const FWoTEarlyUIV1& UI = Preferences->GetPreferences().EarlyUI;
	TestTrue(TEXT("typed reduced motion retained"), UI.bReducedMotion);
	TestTrue(TEXT("typed high contrast retained"), UI.bHighContrast);
	TestTrue(TEXT("typed screen-reader intent retained"), UI.bScreenReader);
	TestEqual(TEXT("typed bootstrap input retained"), UI.InputSensitivity, 1.75f);
	TestEqual(TEXT("real application volume changed"),
		FApp::GetVolumeMultiplier(), 0.5f);
	TestEqual(TEXT("real internationalization culture changed"),
		FInternationalization::Get().GetCurrentCulture()->GetName(),
		FString(TEXT("fr-FR")));
	if (FSlateApplication::IsInitialized())
	{
		TestEqual(TEXT("real Slate application scale changed"),
			FSlateApplication::Get().GetApplicationScale(), 1.25f);
	}

	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Replay = PreferencesEarlyUI(
		"Schema=EarlyUI-v1\n");
	TestFalse(TEXT("replay fixture parses"), Replay.IsError());
	if (!Replay.IsError())
	{
		TestTrue(TEXT("second adoption is rejected"),
			Preferences->AdoptAuthenticatedEarlyUI(
				MoveTemp(Replay).GetValue()).IsError());
	}
	TestTrue(TEXT("full preference reopen is dormant before activation"),
		Preferences->ApplyFullPreferencesAfterCacheActivation().IsError());
	const EWoTPlayerFaction BeforeFaction =
		Preferences->GetPreferences().SelectedFaction;
	TestTrue(TEXT("unsaved faction mutation is dormant before activation"),
		Preferences->SetSelectedFaction(
			EWoTPlayerFaction::Hound, /*bSave*/ false).IsError());
	TestEqual(TEXT("rejected unsaved faction mutation preserves state"),
		static_cast<int32>(Preferences->GetPreferences().SelectedFaction),
		static_cast<int32>(BeforeFaction));
	TestTrue(TEXT("saved faction mutation is dormant before activation"),
		Preferences->SetSelectedFaction(
			EWoTPlayerFaction::Whitecloak, /*bSave*/ true).IsError());
	TestEqual(TEXT("rejected saved faction mutation preserves state"),
		static_cast<int32>(Preferences->GetPreferences().SelectedFaction),
		static_cast<int32>(BeforeFaction));
	TestTrue(TEXT("player-name mutation is dormant before activation"),
		Preferences->SetPlayerName(TEXT("Egwene"), /*bSave*/ false).IsError());
	TestTrue(TEXT("rejected player-name mutation preserves state"),
		Preferences->GetPreferences().PlayerName.IsEmpty());
	FApp::SetVolumeMultiplier(1.0f);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetApplicationScale(1.0f);
	}
	TestTrue(TEXT("prior internationalization culture restores"),
		FInternationalization::Get().SetCurrentCulture(PreviousCulture));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPreferencesTypedSerializationSpec,
	"WoT.Preferences.TypedSerialization", WoTPreferencesSpecFlags)

bool FWoTPreferencesTypedSerializationSpec::RunTest(const FString&)
{
	FWoTPlayerPreferences Value;
	Value.EarlyUI.Width = 2560;
	Value.EarlyUI.Height = 1440;
	Value.EarlyUI.WindowMode = EWoTWindowMode::WindowedFullscreen;
	Value.EarlyUI.UIScale = 1.5f;
	Value.EarlyUI.Locale = EWoTShippedLocale::DeDE;
	Value.EarlyUI.bReducedMotion = true;
	Value.EarlyUI.BootstrapVolume = 0.25f;
	Value.EarlyUI.InputSensitivity = 3.0f;
	Value.SelectedFaction = EWoTPlayerFaction::Whitecloak;
	Value.SelectedSkinColor = EWoTPlayerSkinColor::Red;
	Value.CitadelSkinColor = EWoTPlayerSkinColor::Blue;
	Value.PlayerName = TEXT("Lan Player");
	TArray<uint8> Bytes =
		UWoTPreferencesSubsystem::SerializeForTesting(Value);
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
		Bytes, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
	TestFalse(TEXT("typed persistence bytes parse through the launch grammar"),
		Parsed.IsError());
	if (!Parsed.IsError())
	{
		TestTrue(TEXT("typed EarlyUI round-trips exactly"),
			Parsed.GetValue().Get() == Value.EarlyUI);
	}
	FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		Bytes.Num());
	const FString Text(Converted.Length(), Converted.Get());
	TestTrue(TEXT("typed faction has one closed record"),
		Text.Contains(TEXT("SelectedFaction=Whitecloak\n"),
			ESearchCase::CaseSensitive));
	TestTrue(TEXT("typed player skin has one closed record"),
		Text.Contains(TEXT("PlayerSkin=Red\n"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("typed citadel skin has one closed record"),
		Text.Contains(TEXT("CitadelSkin=Blue\n"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("sanitized player name follows the relay preference"),
		Text.Contains(TEXT("AlwaysUseEpicRelay=0\nPlayerName=Lan Player\n"),
			ESearchCase::CaseSensitive));
	const TCHAR* Forbidden[] = {
		TEXT("Object="), TEXT("Path="), TEXT("Command="), TEXT("CVar="),
		TEXT("Redirect="), TEXT("Module="), TEXT("Plugin="), TEXT("Mount="),
		TEXT("URL="), TEXT("[/Script/"),
	};
	for (const TCHAR* Token : Forbidden)
	{
		TestFalse(FString::Printf(TEXT("serialization excludes %s"), Token),
			Text.Contains(Token, ESearchCase::CaseSensitive));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPreferencesPlayerNameSpec,
	"WoT.Preferences.PlayerName", WoTPreferencesSpecFlags)

bool FWoTPreferencesPlayerNameSpec::RunTest(const FString&)
{
	const FString PreviousCulture =
		FInternationalization::Get().GetCurrentCulture()->GetName();
	const float PreviousScale = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetApplicationScale() : 1.0f;
	const float PreviousVolume = FApp::GetVolumeMultiplier();
	const float PreviousMaster =
		FWoTGameSettingsAdapter::GetAppliedMasterVolume();
	const FString Directory = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString(TEXT("PlayerNamePreferences-")) + FGuid::NewGuid().ToString());
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ON_SCOPE_EXIT
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		FInternationalization::Get().SetCurrentCulture(PreviousCulture);
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetApplicationScale(PreviousScale);
		}
		FApp::SetVolumeMultiplier(PreviousVolume);
		FWoTGameSettings Restore = FWoTGameSettings::Defaults();
		Restore.MasterVolume = PreviousMaster;
		FWoTGameSettingsAdapter::ApplyAudio(Restore);
	};

	auto Load = [this, &Leaf](const TArray<uint8>& Bytes)
	{
		ResetWoTPayloadActivationForTesting();
		TestTrue(TEXT("write isolated player-name preference fixture"),
			FFileHelper::SaveArrayToFile(Bytes, *Leaf));
		UWoTGameInstance* Instance = NewObject<UWoTGameInstance>();
		UWoTPreferencesSubsystem* Preferences =
			NewObject<UWoTPreferencesSubsystem>(Instance);
		TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Early = PreferencesEarlyUI(
			"Schema=EarlyUI-v1\n");
		if (Early.IsError())
		{
			AddError(Early.GetError().ToString());
			return FWoTPlayerPreferences();
		}
		TestFalse(TEXT("adopt isolated player-name EarlyUI"),
			Preferences->AdoptAuthenticatedEarlyUI(
				MoveTemp(Early).GetValue()).IsError());
		TestFalse(TEXT("activate isolated player-name preferences"),
			PublishWoTPayloadBootstrapActivationForTesting().IsError());
		TestFalse(TEXT("load isolated player-name preferences"),
			Preferences->ApplyFullPreferencesAfterCacheActivation().IsError());
		return Preferences->GetPreferences();
	};

	FWoTPlayerPreferences Value;
	Value.SelectedFaction = EWoTPlayerFaction::Hound;
	Value.SelectedSkinColor = EWoTPlayerSkinColor::Red;
	Value.CitadelSkinColor = EWoTPlayerSkinColor::Green;
	Value.PlayerName = TEXT("Two Rivers");
	const TArray<uint8> Serialized =
		UWoTPreferencesSubsystem::SerializeForTesting(Value);
	TestEqual(TEXT("serialized player name round-trips"),
		Load(Serialized).PlayerName, FString(TEXT("Two Rivers")));
	TestEqual(TEXT("serialized skin color round-trips"),
		static_cast<int32>(Load(Serialized).SelectedSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Red));
	TestEqual(TEXT("serialized citadel skin color round-trips"),
		static_cast<int32>(Load(Serialized).CitadelSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Green));

	const auto Bytes = [](const ANSICHAR* Text)
	{
		TArray<uint8> Result;
		Result.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		return Result;
	};
	TestTrue(TEXT("a legacy file without PlayerName uses the platform default"),
		Load(Bytes("Schema=EarlyUI-v1\nSelectedFaction=Hound\n")).PlayerName.IsEmpty());
	TestEqual(TEXT("a legacy file without PlayerSkin uses Base"),
		static_cast<int32>(Load(Bytes(
			"Schema=EarlyUI-v1\nSelectedFaction=Hound\n")).SelectedSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Base));
	TestEqual(TEXT("an invalid PlayerSkin uses Base"),
		static_cast<int32>(Load(Bytes(
			"Schema=EarlyUI-v1\nSelectedFaction=Hound\nPlayerSkin=Purple\n"))
			.SelectedSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Base));
	TestEqual(TEXT("duplicate PlayerSkin records use Base"),
		static_cast<int32>(Load(Bytes(
			"Schema=EarlyUI-v1\nSelectedFaction=Hound\n"
			"PlayerSkin=Blue\nPlayerSkin=Green\n")).SelectedSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Base));
	TestEqual(TEXT("a legacy file without CitadelSkin uses Base"),
		static_cast<int32>(Load(Bytes(
			"Schema=EarlyUI-v1\nSelectedFaction=Hound\n")).CitadelSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Base));
	TestEqual(TEXT("an invalid CitadelSkin uses Base"),
		static_cast<int32>(Load(Bytes(
			"Schema=EarlyUI-v1\nSelectedFaction=Hound\nCitadelSkin=Purple\n"))
			.CitadelSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Base));
	TestEqual(TEXT("duplicate CitadelSkin records use Base"),
		static_cast<int32>(Load(Bytes(
			"Schema=EarlyUI-v1\nSelectedFaction=Hound\n"
			"CitadelSkin=Blue\nCitadelSkin=Green\n")).CitadelSkinColor),
		static_cast<int32>(EWoTPlayerSkinColor::Base));
	TestTrue(TEXT("duplicate PlayerName records are ignored"),
		Load(Bytes("Schema=EarlyUI-v1\nSelectedFaction=Hound\n"
			"PlayerName=One\nPlayerName=Two\n")).PlayerName.IsEmpty());
	TestTrue(TEXT("a torn final PlayerName record is ignored"),
		Load(Bytes("Schema=EarlyUI-v1\nSelectedFaction=Hound\n"
			"PlayerName=Tor")).PlayerName.IsEmpty());

	TArray<uint8> NonASCII = Bytes(
		"Schema=EarlyUI-v1\nSelectedFaction=Hound\nPlayerName=Valid");
	NonASCII.Add(0x80);
	NonASCII.Add('\n');
	const FWoTPlayerPreferences Rejected = Load(NonASCII);
	TestTrue(TEXT("a non-ASCII preference file rejects the player name"),
		Rejected.PlayerName.IsEmpty());
	TestEqual(TEXT("non-ASCII affects faction parsing only as it already did"),
		static_cast<int32>(Rejected.SelectedFaction),
		static_cast<int32>(EWoTPlayerFaction::AesSedai));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPreferencesSaveBoundarySpec,
	"WoT.Preferences.SaveBoundary", WoTPreferencesSpecFlags)

bool FWoTPreferencesSaveBoundarySpec::RunTest(const FString&)
{
	UWoTGameInstance* Owner = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Preferences =
		NewObject<UWoTPreferencesSubsystem>(Owner);
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = PreferencesEarlyUI(
		"Schema=EarlyUI-v1\n");
	if (Parsed.IsError())
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("default adoption succeeds"),
		Preferences->AdoptAuthenticatedEarlyUI(
			MoveTemp(Parsed).GetValue()).IsError());
	FWoTPlayerPreferences Replaced = Preferences->GetPreferences();
	Replaced.EarlyUI.Width = 9000;
	TestTrue(TEXT("out-of-range replacement is refused before I/O"),
		Preferences->Save(Replaced).IsError());
	Replaced = Preferences->GetPreferences();
	Replaced.EarlyUI.Width = 1920;
	TestTrue(TEXT("a file cannot replace authenticated EarlyUI"),
		Preferences->Save(Replaced).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPreferencesDirectorySpec,
	"WoT.Preferences.Directory", WoTPreferencesSpecFlags)

bool FWoTPreferencesDirectorySpec::RunTest(const FString&)
{
	const FString Root = FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("WoT Root With Space"));
	const FString PreferencesDirectory =
		FPaths::Combine(Root, TEXT("Preferences"));
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FPaths::Combine(
		PreferencesDirectory, TEXT("WoTUserPreferences")));
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	};
	FString Marker;
	TestTrue(TEXT("a side-file path resolves under the preference seam"),
		UWoTPreferencesSubsystem::SideFilePath(
			TEXT("WoTSetupBackdrop.png"), Marker));
	TestEqual(TEXT("the preference test seam keeps ordinary side files isolated"),
		Marker,
		FPaths::Combine(PreferencesDirectory,
			TEXT("WoTUserPreferences.WoTSetupBackdrop.png")));
	TestTrue(TEXT("the update notice resolves under the preference seam"),
		UWoTPreferencesSubsystem::SideFilePath(
			TEXT("WoTUpdateFailedNotice"), Marker));
	TestEqual(TEXT("the shared update marker stays in R/Preferences"),
		Marker,
		FPaths::Combine(PreferencesDirectory, TEXT("WoTUpdateFailedNotice")));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
