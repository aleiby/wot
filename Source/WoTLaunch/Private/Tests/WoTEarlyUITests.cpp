#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTEarlyUI.h"
#include "WoTLaunchEngineHooks.h"

namespace
{
constexpr EAutomationTestFlags WoTEarlyUISpecFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter;

TArray<uint8> EarlyUIBytes(const ANSICHAR* Text)
{
	const int32 Length = FCStringAnsi::Strlen(Text);
	TArray<uint8> Result;
	Result.Append(reinterpret_cast<const uint8*>(Text), Length);
	return Result;
}

TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> ParseEarlyUI(
	TConstArrayView<uint8> Bytes)
{
	return ParseEarlyUIV1(Bytes, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEarlyUICompleteParserSpec,
	"WoT.Launch.EarlyUI.CompleteParser", WoTEarlyUISpecFlags)

bool FWoTEarlyUICompleteParserSpec::RunTest(const FString&)
{
	TArray<uint8> Bytes = EarlyUIBytes(
		"Schema=EarlyUI-v1\n"
		"Width=1920\nHeight=1080\nWindowMode=WindowedFullscreen\n"
		"UIScale=1.25\nLocale=fr-FR\nReducedMotion=1\n"
		"HighContrast=1\nScreenReader=1\nBootstrapVolume=0.4\n"
		"InputSensitivity=2.5\n");
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUI(Bytes);
	if (!TestFalse(TEXT("complete EarlyUI parses"), Parsed.IsError()))
	{
		return false;
	}
	const FWoTEarlyUIV1& Value = Parsed.GetValue().Get();
	TestEqual(TEXT("bounded width"), static_cast<int32>(Value.Width), 1920);
	TestEqual(TEXT("bounded height"), static_cast<int32>(Value.Height), 1080);
	TestEqual(TEXT("closed window mode"), static_cast<int32>(Value.WindowMode),
		static_cast<int32>(EWoTWindowMode::WindowedFullscreen));
	TestEqual(TEXT("bounded UI scale"), Value.UIScale, 1.25f);
	TestEqual(TEXT("inventory-bound locale"), static_cast<int32>(Value.Locale),
		static_cast<int32>(EWoTShippedLocale::FrFR));
	TestTrue(TEXT("reduced motion"), Value.bReducedMotion);
	TestTrue(TEXT("high contrast"), Value.bHighContrast);
	TestTrue(TEXT("screen reader"), Value.bScreenReader);
	TestEqual(TEXT("bootstrap volume"), Value.BootstrapVolume, 0.4f);
	TestEqual(TEXT("input sensitivity"), Value.InputSensitivity, 2.5f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEarlyUIDefaultBoundarySpec,
	"WoT.Launch.EarlyUI.SignedDefaults", WoTEarlyUISpecFlags)

bool FWoTEarlyUIDefaultBoundarySpec::RunTest(const FString&)
{
	const FWoTEarlyUIV1 Defaults = FWoTEarlyUILimits::Production().Defaults;
	TArray<uint8> Bytes = EarlyUIBytes(
		"Schema=EarlyUI-v1\n"
		"Width=99999\nWidth=1920\nHeight=-1\nWindowMode=ConsoleCommand\n"
		"UIScale=1.5e0\nLocale=../../Object\nReducedMotion=yes\n"
		"HighContrast=2\nScreenReader=/Script/Evil.Class\n"
		"BootstrapVolume=.5\nInputSensitivity=+2\n"
		"Object=/Game/Evil\nPath=../../Engine.ini\nCommand=quit\n"
		"CVar=r.Foo=1\nRedirect=Old.New\nModule=Injected\nPlugin=Injected\n"
		"Mount=/Injected\nURL=127.0.0.1?game=Evil\n[LegacyIni]=1\n");
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUI(Bytes);
	TestFalse(TEXT("invalid individual records are non-fatal"), Parsed.IsError());
	if (!Parsed.IsError())
	{
		TestTrue(TEXT("invalid, duplicate, and extension-shaped records use defaults"),
			Parsed.GetValue().Get() == Defaults);
	}

	TArray<uint8> WrongSchema = EarlyUIBytes(
		"Schema=EarlyUI-v2\nWidth=1920\n");
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Wrong =
		ParseEarlyUI(WrongSchema);
	TestFalse(TEXT("schema mismatch safely authenticates defaults"), Wrong.IsError());
	if (!Wrong.IsError())
	{
		TestTrue(TEXT("schema mismatch changes nothing"),
			Wrong.GetValue().Get() == Defaults);
	}

	TArray<uint8> Torn = EarlyUIBytes(
		"Schema=EarlyUI-v1\nWidth=1600");
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> TornResult = ParseEarlyUI(Torn);
	TestFalse(TEXT("torn final record safely parses"), TornResult.IsError());
	if (!TornResult.IsError())
	{
		TestEqual(TEXT("torn width remains default"),
			static_cast<int32>(TornResult.GetValue().Get().Width),
			static_cast<int32>(Defaults.Width));
	}

	FWoTEarlyUILimits Small = FWoTEarlyUILimits::Production();
	Small.MaxSerializedBytes = 32;
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Oversized =
		ParseEarlyUIV1(Bytes, Small, FWoTInventoryLocaleSet::Production());
	TestFalse(TEXT("oversized input safely parses"), Oversized.IsError());
	if (!Oversized.IsError())
	{
		TestTrue(TEXT("oversized input uses signed defaults"),
			Oversized.GetValue().Get() == Defaults);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEarlyUILocaleInventorySpec,
	"WoT.Launch.EarlyUI.LocaleInventory", WoTEarlyUISpecFlags)

bool FWoTEarlyUILocaleInventorySpec::RunTest(const FString&)
{
	FWoTInventoryLocaleSet EnglishOnly;
	EnglishOnly.Locales.Add(EWoTShippedLocale::EnUS);
	TArray<uint8> Bytes = EarlyUIBytes(
		"Schema=EarlyUI-v1\nLocale=fr-FR\n");
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
		Bytes, FWoTEarlyUILimits::Production(), EnglishOnly);
	TestFalse(TEXT("unshipped locale is a per-record default"), Parsed.IsError());
	if (!Parsed.IsError())
	{
		TestEqual(TEXT("unshipped locale cannot resolve"),
			static_cast<int32>(Parsed.GetValue().Get().Locale),
			static_cast<int32>(EWoTShippedLocale::EnUS));
	}
	FWoTEarlyUILimits Invalid = FWoTEarlyUILimits::Production();
	Invalid.Defaults.Locale = EWoTShippedLocale::FrFR;
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> InvalidInventory =
		ParseEarlyUIV1(Bytes, Invalid, EnglishOnly);
	TestTrue(TEXT("signed defaults outside inventory are fatal"),
		InvalidInventory.IsError());

	Invalid = FWoTEarlyUILimits::Production();
	Invalid.Defaults.WindowMode = static_cast<EWoTWindowMode>(99);
	TestTrue(TEXT("unknown signed default window modes are fatal"),
		ParseEarlyUIV1(Bytes, Invalid, EnglishOnly).IsError());

	FWoTInventoryLocaleSet DuplicateInventory =
		FWoTInventoryLocaleSet::Production();
	DuplicateInventory.Locales.Add(EWoTShippedLocale::EnUS);
	TestTrue(TEXT("duplicate locale inventory entries are fatal"),
		ParseEarlyUIV1(Bytes, FWoTEarlyUILimits::Production(),
			DuplicateInventory).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEarlyUIAuthoritySpec,
	"WoT.Launch.EarlyUI.Authority", WoTEarlyUISpecFlags)

bool FWoTEarlyUIAuthoritySpec::RunTest(const FString&)
{
	FWoTEarlyUIAuthority& Authority = GetEarlyUIAuthority();
	Authority.ResetForTesting();
	TestTrue(TEXT("take before publication fails"), Authority.TakeOnce().IsError());

	TArray<uint8> Bytes = EarlyUIBytes(
		"Schema=EarlyUI-v1\nWidth=1600\nHeight=900\nWindowMode=Fullscreen\n");
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUI(Bytes);
	if (Parsed.IsError())
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("one publication succeeds"),
		Authority.PublishOnce(MoveTemp(Parsed).GetValue()).IsError());
	TestTrue(TEXT("take before native projection fails"),
		Authority.TakeOnce().IsError());

	uint16 AppliedWidth = 0;
	uint16 AppliedHeight = 0;
	EWoTWindowMode AppliedMode = EWoTWindowMode::Windowed;
	auto Apply = [&AppliedWidth, &AppliedHeight, &AppliedMode](
		uint16 Width, uint16 Height, EWoTWindowMode Mode)
		-> FWoTBootstrapStatus
	{
		AppliedWidth = Width;
		AppliedHeight = Height;
		AppliedMode = Mode;
		return FWoTBootstrapStatus::Success();
	};
	const FWoTEarlyUINativeProjectionSink Sink{Apply};
	FString Error;
	TestTrue(TEXT("native projection succeeds once"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			Sink, &Error));
	TestEqual(TEXT("projection width came from retained value"),
		static_cast<int32>(AppliedWidth), 1600);
	TestEqual(TEXT("projection height came from retained value"),
		static_cast<int32>(AppliedHeight), 900);
	TestEqual(TEXT("projection mode came from retained value"),
		static_cast<int32>(AppliedMode),
		static_cast<int32>(EWoTWindowMode::Fullscreen));
	TestFalse(TEXT("native projection replay fails"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			Sink, &Error));

	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Taken = Authority.TakeOnce();
	TestFalse(TEXT("projection does not consume the handoff"), Taken.IsError());
	if (!Taken.IsError())
	{
		TestEqual(TEXT("take observes the same width"),
			static_cast<int32>(Taken.GetValue().Get().Width), 1600);
	}
	TestTrue(TEXT("second take fails"), Authority.TakeOnce().IsError());
	TestFalse(TEXT("shutdown closes taken authority"), Authority.Shutdown().IsError());
	TestTrue(TEXT("shutdown replay fails"), Authority.Shutdown().IsError());

	Authority.ResetForTesting();
	Parsed = ParseEarlyUI(Bytes);
	if (Parsed.IsError())
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("failure case publishes once"),
		Authority.PublishOnce(MoveTemp(Parsed).GetValue()).IsError());
	auto FailProjection = [](uint16, uint16, EWoTWindowMode)
		-> FWoTBootstrapStatus
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("injected projection failure")));
	};
	const FWoTEarlyUINativeProjectionSink FailingSink{FailProjection};
	TestFalse(TEXT("native projection failure is reported"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			FailingSink, &Error));
	TestTrue(TEXT("failed native projection cannot be taken"),
		Authority.TakeOnce().IsError());
	TestFalse(TEXT("failed native projection cannot be replayed"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			FailingSink, &Error));
	TestTrue(TEXT("failed native projection already closed authority"),
		Authority.Shutdown().IsError());
	Authority.ResetForTesting();
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
