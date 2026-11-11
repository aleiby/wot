#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "Settings/WoTGameSettings.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/UObjectGlobals.h"
#include "WoTGameViewportClient.h"
#include <limits>

namespace
{
constexpr EAutomationTestFlags WoTWindowSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDefaultWindowSizingSpec,
	"WoT.Preferences.WindowSizingTable", WoTWindowSpecFlags)

bool FWoTDefaultWindowSizingSpec::RunTest(const FString&)
{
	struct FWindowSizingCase
	{
		FIntPoint WorkArea;
		float Scale;
		FIntPoint Expected;
	};
	const FWindowSizingCase Cases[] = {
		{{3024, 1890}, 2.0f, {2560, 1440}},
		{{2560, 1550}, 2.0f, {2304, 1296}},
		{{1920, 1040}, 1.0f, {1280, 720}},
		{{3840, 2080}, 1.5f, {1920, 1080}},
		{{1366, 728}, 1.0f, {1280, 720}},
		{{0, 0}, 2.0f, {1280, 720}},
		{{3024, 1890}, 0.0f, {1280, 720}},
		{{3600, 2260}, 2.0f, {2560, 1440}},
		{{4000, 1200}, 2.0f, {1920, 1080}},
		{{3024, 1890}, 1.01f, {1292, 726}},
		{{3024, 1890}, 0.5f, {1280, 720}},
		{{-1, 1890}, 2.0f, {1280, 720}},
		{{3024, 0}, 2.0f, {1280, 720}},
		{{3024, 1890}, -1.0f, {1280, 720}},
		{{3024, 1890}, std::numeric_limits<float>::infinity(), {1280, 720}},
		{{3024, 1890}, std::numeric_limits<float>::quiet_NaN(), {1280, 720}},
		{{MAX_int32, MAX_int32}, std::numeric_limits<float>::max(), {7680, 4320}},
	};
	for (const FWindowSizingCase& Entry : Cases)
	{
		const FString Label = FString::Printf(TEXT("%dx%d at %.3g"),
			Entry.WorkArea.X, Entry.WorkArea.Y, Entry.Scale);
		TestEqual(Label, FWoTGameSettings::DefaultWindowSize(Entry.WorkArea, Entry.Scale),
			Entry.Expected);
		FWoTGameSettings Settings = FWoTGameSettings::Defaults(Entry.WorkArea, Entry.Scale);
		FString Error;
		TestTrue(Label + TEXT(" validates"), Settings.Validate(Error));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWindowSettingsMigrationSpec,
	"WoT.Preferences.WindowSizeMigration", WoTWindowSpecFlags)

bool FWoTWindowSettingsMigrationSpec::RunTest(const FString&)
{
	const FIntPoint WorkArea(2560, 1550);
	struct FWindowMigrationCase
	{
		int32 Version;
		uint16 SavedWidth;
		uint16 SavedHeight;
		EWoTWindowMode Mode;
		FIntPoint Expected;
	};
	const FWindowMigrationCase Cases[] = {
		{10, 1280, 720, EWoTWindowMode::Windowed, {2304, 1296}},
		{10, 1920, 1080, EWoTWindowMode::Windowed, {1920, 1080}},
		{10, 1280, 720, EWoTWindowMode::Fullscreen, {1280, 720}},
		{10, 1280, 720, EWoTWindowMode::WindowedFullscreen, {1280, 720}},
		{10, 1280, 800, EWoTWindowMode::Windowed, {1280, 800}},
		{11, 1280, 720, EWoTWindowMode::Windowed, {1280, 720}},
	};
	FWoTGameSettings Parsed;
	FString Error;
	for (const FWindowMigrationCase& Entry : Cases)
	{
		FWoTGameSettings Saved = FWoTGameSettings::Defaults();
		Saved.Width = Entry.SavedWidth;
		Saved.Height = Entry.SavedHeight;
		Saved.WindowMode = Entry.Mode;
		FString Record = FWoTGameSettingsCodec::Serialize(Saved);
		Record.ReplaceInline(TEXT("GameSettingsVersion=11"),
			*FString::Printf(TEXT("GameSettingsVersion=%d"), Entry.Version));
		const FTCHARToUTF8 Encoded(*Record);
		TestTrue(TEXT("saved record parses with injected display"),
			FWoTGameSettingsCodec::Parse(MakeArrayView(
				reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()),
				Parsed, Error, WorkArea, 2.0f));
		TestEqual(TEXT("only the old windowed default migrates"),
			FIntPoint(Parsed.Width, Parsed.Height), Entry.Expected);
		TestTrue(TEXT("window mode is retained"), Parsed.WindowMode == Entry.Mode);
		TestTrue(TEXT("written record is version 11"),
			FWoTGameSettingsCodec::Serialize(Parsed).StartsWith(TEXT("GameSettingsVersion=11\n")));
	}
	const ANSICHAR FreshRecord[] = "Schema=EarlyUI-v1\n";
	TestTrue(TEXT("fresh profile parses without a game record"),
		FWoTGameSettingsCodec::Parse(MakeArrayView(
			reinterpret_cast<const uint8*>(FreshRecord), UE_ARRAY_COUNT(FreshRecord) - 1),
			Parsed, Error, WorkArea, 2.0f));
	TestEqual(TEXT("fresh profile uses the display default"),
		FIntPoint(Parsed.Width, Parsed.Height), FIntPoint(2304, 1296));
	TestTrue(TEXT("missing preference file parses"),
		FWoTGameSettingsCodec::Parse({}, Parsed, Error, WorkArea, 2.0f));
	TestEqual(TEXT("missing preference file uses the display default"),
		FIntPoint(Parsed.Width, Parsed.Height), FIntPoint(2304, 1296));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTViewportClickActivationSpec,
	"WoT.Viewport.ClickActivationCapture", WoTWindowSpecFlags)

bool FWoTViewportClickActivationSpec::RunTest(const FString&)
{
#if WITH_EDITORONLY_DATA || USING_CODE_ANALYSIS
	TGuardValue<bool> GameContext(GIsEditor, false);
#endif
	UWoTGameViewportClient* Client = NewObject<UWoTGameViewportClient>();
	Client->SetMouseCaptureMode(EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
	TestTrue(TEXT("gameplay normally captures"), Client->IsInPermanentCapture());
	TestTrue(TEXT("a click activation suppresses capture"),
		Client->SuppressClickActivation(FWindowActivateEvent::EA_Activate, true));
	TestFalse(TEXT("the activation query does not capture"), Client->IsInPermanentCapture());
	TestTrue(TEXT("the next query captures normally"), Client->IsInPermanentCapture());
	Client->SuppressClickActivation(FWindowActivateEvent::EA_Activate, true);
	Client->Tick(0.0f);
	TestTrue(TEXT("unused suppression expires on tick"), Client->IsInPermanentCapture());
	TestFalse(TEXT("keyboard activation does not suppress"),
		Client->SuppressClickActivation(FWindowActivateEvent::EA_Activate, false));
	TestTrue(TEXT("keyboard activation keeps capture"), Client->IsInPermanentCapture());
	TestFalse(TEXT("native mouse activation needs no suppression"),
		Client->SuppressClickActivation(FWindowActivateEvent::EA_ActivateByMouse, true));
	TestFalse(TEXT("deactivation does not suppress"),
		Client->SuppressClickActivation(FWindowActivateEvent::EA_Deactivate, true));
	Client->SuppressClickActivation(FWindowActivateEvent::EA_Activate, true);
	Client->SuppressClickActivation(FWindowActivateEvent::EA_Activate, false);
	TestTrue(TEXT("a later activation clears unused suppression"), Client->IsInPermanentCapture());
	return !HasAnyErrors();
}

#endif
