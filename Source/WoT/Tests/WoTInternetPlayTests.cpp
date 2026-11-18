// Internet play (EOS) specs. Everything EOS is reached through the
// IWoTInternetBackend seam: a fake drives the sign-in state machine and a
// second Null OSS instance stands in for the internet session provider, so
// no spec needs credentials, the EOS SDK, or a network.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#if WOT_WITH_EOS && WITH_EOS_SDK
#include "../Online/WoTEOSCachePolicy.h"
#include "IEOSSDKManager.h"
#include "WoTBootstrapCore/ProfileSession.h"
#endif
#include "../Online/WoTInternetPlay.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Testing/WoTNetTestActions.h"
#include "../Testing/WoTNetTestChannel.h"
#include "../WoTFrontEnd.h"
#include "../WoTFrontEndViewModel.h"
#include "../WoTBattleGameMode.h"
#include "../WoTCitadelGameState.h"
#include "../WoTGameEngine.h"
#include "../WoTGameMode.h"
#include "../WoTGameInstance.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTSessionSubsystem.h"
#include "WoTTestWorld.h"

#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/NetDriver.h"
#include "Engine/Player.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "IpNetDriver.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSubsystemUtils.h"

namespace
{
constexpr EAutomationTestFlags WoTInternetSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

const FName WoTInternetSpecNullInstance(TEXT("NULL:WoTInternetSpec"));

FWoTEOSArtifact WoTFakeArtifact()
{
	// Obviously fake values: shaped like portal ids, never real ones.
	FWoTEOSArtifact Artifact;
	Artifact.ArtifactName = TEXT("WoTTest");
	Artifact.ProductId = TEXT("fakeproduct00000000000000000000");
	Artifact.SandboxId = TEXT("fakesandbox00000000000000000000");
	Artifact.DeploymentId = TEXT("fakedeploy000000000000000000000");
	Artifact.ClientId = TEXT("fakeclient000000000000000000000");
	Artifact.ClientSecret = TEXT("fake+secret/value=0000000000000");
	return Artifact;
}

/** Deferred, call-counting stand-in for the EOS backend. */
class FWoTFakeInternetBackend final : public IWoTInternetBackend
{
public:
	FString Unavailability;
	FString StartResult;
	IOnlineSessionPtr SessionsForTest;
	bool bLoggedIn = false;
	int32 StartCalls = 0;
	int32 DeviceIdCalls = 0;
	int32 LoginCalls = 0;
	int32 ResetCalls = 0;
	TArray<EWoTRelayControl> RelayControls;
	EWoTRelayControl EffectiveRelayControl = EWoTRelayControl::AllowRelays;
	bool bRelayControlSucceeds = true;
	FString LastDisplayName;
	FDone PendingDeviceId;
	FDone PendingLogin;
	FDone PendingReset;
	TFunction<void(const FString&)> Lost;
	FTelemetry Telemetry;

	virtual FString QuickUnavailability() const override { return Unavailability; }
	virtual FString EnsureStarted(UWorld*) override
	{
		++StartCalls;
		return StartResult;
	}
	virtual IOnlineSessionPtr GetSessionInterface() const override
	{
		return StartCalls > 0 ? SessionsForTest : nullptr;
	}
	virtual bool IsLoggedIn() const override { return bLoggedIn; }
	virtual FString RedactedUserId() const override
	{
		return bLoggedIn ? FString(TEXT("...abc123")) : FString(TEXT("none"));
	}
	virtual void CreateDeviceId(FDone Done) override
	{
		++DeviceIdCalls;
		PendingDeviceId = MoveTemp(Done);
	}
	virtual void Login(const FString& DisplayName, FDone Done) override
	{
		++LoginCalls;
		LastDisplayName = DisplayName;
		PendingLogin = MoveTemp(Done);
	}
	virtual void ResetIdentity(FDone Done) override
	{
		++ResetCalls;
		PendingReset = MoveTemp(Done);
	}
	virtual void SetSignInLostHandler(TFunction<void(const FString&)> Handler) override
	{
		Lost = MoveTemp(Handler);
	}
	virtual void SetTelemetryHandler(FTelemetry Handler) override
	{
		Telemetry = MoveTemp(Handler);
	}
	virtual bool SetRelayControl(EWoTRelayControl Control) override
	{
		RelayControls.Add(Control);
		if (!bRelayControlSucceeds)
		{
			return false;
		}
		EffectiveRelayControl = Control;
		return true;
	}
	virtual FString Diagnostics() const override
	{
		return FString::Printf(TEXT("artifact=fake nat=unknown relay=%s sdk=fake"),
			LexToString(EffectiveRelayControl));
	}

	void FinishDeviceId(bool bOk)
	{
		FDone Done = MoveTemp(PendingDeviceId);
		PendingDeviceId = nullptr;
		if (Done)
		{
			Done(bOk, bOk ? TEXT("EOS_Success") : TEXT("EOS_NoConnection"));
		}
	}
	void FinishLogin(bool bOk)
	{
		bLoggedIn = bOk;
		FDone Done = MoveTemp(PendingLogin);
		PendingLogin = nullptr;
		if (Done)
		{
			Done(bOk, bOk ? TEXT("") : TEXT("EOS_InvalidParameters"));
		}
	}
	void FinishReset(bool bOk)
	{
		if (bOk)
		{
			bLoggedIn = false;
		}
		FDone Done = MoveTemp(PendingReset);
		PendingReset = nullptr;
		if (Done)
		{
			Done(bOk, bOk ? TEXT("EOS_Success") : TEXT("EOS_UnexpectedError"));
		}
	}

	void EmitTelemetry(const TCHAR* Event, const FString& Detail)
	{
		if (Telemetry)
		{
			Telemetry(Event, Detail);
		}
	}
};

UWorld* WoTInternetSpecs_CreateWorld()
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

void WoTInternetSpecs_DestroyWorld(UWorld*& World)
{
	if (World != nullptr)
	{
		WoTTest::DestroySpecWorld(World);
		World = nullptr;
	}
	ResetWoTPayloadActivationForTesting();
}

FWoTHostRequest WoTInternetSpecs_Request(bool bLAN)
{
	FWoTHostRequest Request =
		FWoTFrontEndModel::DefaultHostRequest(TEXT("/Game/WoT/Maps/Arena_01"));
	Request.ServerName = TEXT("Internet Spec");
	Request.bLAN = bLAN;
	return Request;
}
} // namespace

#if WOT_WITH_EOS && WITH_EOS_SDK
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetEOSCachePolicySpec,
	"WoT.Internet.EOSCacheStaysInData", WoTInternetSpecFlags)

bool FWoTInternetEOSCachePolicySpec::RunTest(const FString&)
{
	FString Fixture = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation/WoT EOS Policy Fixture"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FPaths::NormalizeDirectoryName(Fixture);
	const FString Root = FPaths::Combine(Fixture, TEXT("Data"));
	const FString ExpectedEOS = FPaths::Combine(Root, TEXT("Online/EOS"));
	IFileManager::Get().MakeDirectory(*Root, true);
	WoTResetProcessProfileSessionForTesting();
	ON_SCOPE_EXIT
	{
		WoTResetProcessProfileSessionForTesting();
		IFileManager::Get().DeleteDirectory(*Fixture, false, true);
	};
	FWoTBootstrapStatus Acquired =
		WoTAcquireProcessProfileSessionShared(Root);
	if (!TestFalse(TEXT("the process takes SHARED before EOS setup"),
		Acquired.IsError()))
	{
		if (Acquired.IsError()) AddError(Acquired.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("SHARED acquisition alone does not create EOS leaves"),
		IFileManager::Get().DirectoryExists(*ExpectedEOS));

	IEOSSDKManager* Manager = IEOSSDKManager::Get();
	if (!TestNotNull(TEXT("the real EOS SDK manager is registered"), Manager))
		return false;
	TUniquePtr<FWoTEOSCachePolicy> StartupPolicy;
	FDelegateHandle StartupRegistration;
	FWoTBootstrapStatus Registered = WoTRegisterEOSCachePolicyForStartup(
		Manager, Root, StartupPolicy, StartupRegistration);
	if (!TestFalse(TEXT("the exact startup registration path succeeds"),
		Registered.IsError()))
	{
		if (Registered.IsError()) AddError(Registered.GetError().ToString());
		return false;
	}
	ON_SCOPE_EXIT
	{
		Manager->OnPreCreatePlatform.Remove(StartupRegistration);
		StartupPolicy.Reset();
	};
	EOS_Platform_Options Options{};
	Options.CacheDirectory = "C:/Users/player/AppData/Local/WoT/old-cache";
	Manager->OnPreCreatePlatform.Broadcast(Options);
	const FString Applied = UTF8_TO_TCHAR(Options.CacheDirectory);
	TestTrue(TEXT("registration creates Data/Online/EOS after SHARED"),
		IFileManager::Get().DirectoryExists(*ExpectedEOS));
	TestEqual(TEXT("the startup delegate receives Data/Online/EOS"),
		Applied, ExpectedEOS);
	TestEqual(TEXT("the startup-owned policy stores Data/Online/EOS"),
		StartupPolicy->Directory(), ExpectedEOS);
	TestFalse(TEXT("the replaced path has no Epic or AppData component"),
		Applied.Contains(TEXT("Epic"), ESearchCase::IgnoreCase)
		|| Applied.Contains(TEXT("AppData"), ESearchCase::IgnoreCase));
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetConfigPolicySpec,
	"WoT.Internet.Config.ArtifactPolicy", WoTInternetSpecFlags)

bool FWoTInternetConfigPolicySpec::RunTest(const FString&)
{
	TestEqual(TEXT("Shipping uses only the public artifact"),
		FWoTEOSConfig::PreferredArtifactName(true, true), FString(TEXT("WoT")));
	TestEqual(TEXT("Test/dev prefers the test deployment"),
		FWoTEOSConfig::PreferredArtifactName(false, true), FString(TEXT("WoTTest")));
	TestEqual(TEXT("Test/dev falls back to the public artifact"),
		FWoTEOSConfig::PreferredArtifactName(false, false), FString(TEXT("WoT")));

	TestTrue(TEXT("fake but well-formed credentials validate"),
		FWoTEOSConfig::ValidateArtifact(WoTFakeArtifact()).IsEmpty());
	const TCHAR* Placeholders[] = {
		TEXT(""), TEXT("   "), TEXT("REPLACE_WITH_PRODUCT_ID"),
		TEXT("placeholder"), TEXT("<ClientId>"),
	};
	for (const TCHAR* Value : Placeholders)
	{
		FWoTEOSArtifact Artifact = WoTFakeArtifact();
		Artifact.ProductId = Value;
		TestEqual(FString::Printf(TEXT("placeholder '%s' is not configured"), Value),
			FWoTEOSConfig::ValidateArtifact(Artifact), FString(TEXT("not-configured")));
	}
	const TCHAR* Unsafe[] = {
		TEXT("has space"), TEXT("quote\"d"), TEXT("paren("), TEXT("comma,"),
		TEXT("back\\slash"),
	};
	for (const TCHAR* Value : Unsafe)
	{
		FWoTEOSArtifact Artifact = WoTFakeArtifact();
		Artifact.ClientSecret = Value;
		TestFalse(FString::Printf(TEXT("config-breaking value '%s' is refused"), Value),
			FWoTEOSConfig::ValidateArtifact(Artifact).IsEmpty());
	}
	FWoTEOSArtifact MissingSecret = WoTFakeArtifact();
	MissingSecret.ClientSecret.Reset();
	TestFalse(TEXT("every field is required"),
		FWoTEOSConfig::ValidateArtifact(MissingSecret).IsEmpty());

	TestEqual(TEXT("ids are redacted to six characters"),
		FWoTEOSConfig::RedactId(TEXT("0002abcdef0123456789")),
		FString(TEXT("...456789")));
	TestEqual(TEXT("short ids reveal nothing"),
		FWoTEOSConfig::RedactId(TEXT("abc")), FString(TEXT("...")));
	TestEqual(TEXT("no id"), FWoTEOSConfig::RedactId(FString()), FString(TEXT("none")));

	for (const TCHAR* Reason : {TEXT("not-compiled"), TEXT("disabled"),
		TEXT("not-configured"), TEXT("sdk-unavailable"), TEXT("init-failed")})
	{
		TestTrue(FString::Printf(TEXT("unavailable reason %s keeps LAN in view"), Reason),
			FWoTEOSConfig::DescribeReason(Reason).Contains(TEXT("LAN")));
	}
	for (const TCHAR* Reason : {TEXT("device-id-failed"), TEXT("login-failed"),
		TEXT("login-timeout"), TEXT("signed-out"), TEXT("reset-failed"),
		TEXT("not-acknowledged"), TEXT("anything-else")})
	{
		TestFalse(FString::Printf(TEXT("reason %s has player text"), Reason),
			FWoTEOSConfig::DescribeReason(Reason).IsEmpty());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetShippingCommandLineSpec,
	"WoT.Internet.Config.ShippingCommandLineStrip", WoTInternetSpecFlags)

bool FWoTInternetShippingCommandLineSpec::RunTest(const FString&)
{
	const FString Untouched = TEXT("-windowed -ResX=640 -NoEOS -wotnettest");
	TestEqual(TEXT("unrelated tokens and -NoEOS are kept byte for byte"),
		FWoTEOSConfig::StripShippingCommandLineOverrides(Untouched), Untouched);
	TestEqual(TEXT("every EOS / auth / bucket override is removed"),
		FWoTEOSConfig::StripShippingCommandLineOverrides(
			TEXT("-windowed -EpicApp=Other -EOSArtifactNameOverride=WoTTest ")
			TEXT("-EpicSandboxId=s1 -EpicSandboxIdOverride=s2 ")
			TEXT("-EpicDeploymentId=d1 -EpicDeploymentIdOverride=d2 ")
			TEXT("-AUTH_TYPE=deviceid:DeviceIdAccessToken -AUTH_LOGIN=x ")
			TEXT("-AUTH_PASSWORD=y -BuildIdOverride=7 -NoEOS")),
		FString(TEXT("-windowed -NoEOS")));
	TestEqual(TEXT("a quoted value with spaces goes with its token"),
		FWoTEOSConfig::StripShippingCommandLineOverrides(
			TEXT("-a \"-EpicApp=My App\" -b -epicapp=\"x y\" -c")),
		FString(TEXT("-a -b -c")));
	TestEqual(TEXT("an override hidden inside another value is removed too"),
		FWoTEOSConfig::StripShippingCommandLineOverrides(
			TEXT("-x=\"q EpicApp=evil\" -y")),
		FString(TEXT("-y")));
	TestEqual(TEXT("empty command line"),
		FWoTEOSConfig::StripShippingCommandLineOverrides(TEXT("")), FString());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetEngineConfigSpec,
	"WoT.Internet.Config.EngineConfig", WoTInternetSpecFlags)

bool FWoTInternetEngineConfigSpec::RunTest(const FString&)
{
	// The effective GameNetDriver is the FIRST matching definition on the
	// engine object actually used (UWoTGameEngine in a -game run).
	for (const UEngine* Engine : {static_cast<const UEngine*>(GetDefault<UWoTGameEngine>()),
		static_cast<const UEngine*>(GetDefault<UGameEngine>())})
	{
		const FString Label = Engine->GetClass()->GetName();
		const FNetDriverDefinition* Game = Engine->NetDriverDefinitions.FindByPredicate(
			[](const FNetDriverDefinition& Definition)
			{
				return Definition.DefName == NAME_GameNetDriver;
			});
		if (TestNotNull(Label + TEXT(" defines GameNetDriver"), Game))
		{
			TestEqual(Label + TEXT(" GameNetDriver is EOS P2P"),
				Game->DriverClassName.ToString(),
				FString(TEXT("/Script/SocketSubsystemEOS.NetDriverEOS")));
			TestEqual(Label + TEXT(" falls back to the IP driver"),
				Game->DriverClassNameFallback.ToString(),
				FString(TEXT("/Script/OnlineSubsystemUtils.IpNetDriver")));
		}
		TestTrue(Label + TEXT(" keeps the beacon driver"),
			Engine->NetDriverDefinitions.ContainsByPredicate(
				[](const FNetDriverDefinition& Definition)
				{
					return Definition.DefName == NAME_BeaconNetDriver;
				}));
		TestTrue(Label + TEXT(" keeps the demo driver"),
			Engine->NetDriverDefinitions.ContainsByPredicate(
				[](const FNetDriverDefinition& Definition)
				{
					return Definition.DefName == NAME_DemoNetDriver;
				}));
		TestFalse(Label + TEXT(" has no dormant SteamSockets driver"),
			Engine->NetDriverDefinitions.ContainsByPredicate(
				[](const FNetDriverDefinition& Definition)
				{
					return Definition.DriverClassName.ToString().Contains(TEXT("Steam"));
				}));
	}

	UClass* EOSDriver = StaticLoadClass(UNetDriver::StaticClass(), nullptr,
		TEXT("/Script/SocketSubsystemEOS.NetDriverEOS"), nullptr, LOAD_Quiet);
	UClass* IpDriver = StaticLoadClass(UNetDriver::StaticClass(), nullptr,
		TEXT("/Script/OnlineSubsystemUtils.IpNetDriver"), nullptr, LOAD_Quiet);
	if (TestNotNull(TEXT("NetDriverEOS is compiled into the editor"), EOSDriver)
		&& TestNotNull(TEXT("IpNetDriver class"), IpDriver))
	{
		// With no EOS OSS instance there is no EOS socket subsystem, so the EOS
		// driver reports unavailable and the engine creates the IP fallback.
		if (!IOnlineSubsystem::DoesInstanceExist(EOS_SUBSYSTEM))
		{
			TestFalse(TEXT("NetDriverEOS is unavailable without an EOS login"),
				EOSDriver->GetDefaultObject<UNetDriver>()->IsAvailable());
		}
		// Simulation parity: whichever driver carries a match (EOS P2P, its IP
		// pass-through, or the IpNetDriver fallback), every configured net
		// driver tunable (tick/client rates, timeouts) is the IP driver's.
		const UObject* IpDefaults = IpDriver->GetDefaultObject();
		const UObject* EOSDefaults = EOSDriver->GetDefaultObject();
		int32 Compared = 0;
		for (TFieldIterator<FProperty> It(IpDriver); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Config)
				|| Property->GetFName() == FName(TEXT("NetConnectionClassName")))
			{
				continue;
			}
			++Compared;
			FString IpValue;
			FString EOSValue;
			Property->ExportTextItem_InContainer(IpValue, IpDefaults, nullptr, nullptr, PPF_None);
			Property->ExportTextItem_InContainer(EOSValue, EOSDefaults, nullptr, nullptr, PPF_None);
			TestEqual(FString::Printf(TEXT("NetDriverEOS keeps the IP driver's %s"),
				*Property->GetName()), EOSValue, IpValue);
		}
		TestTrue(TEXT("net driver tunables were compared"), Compared > 5);
	}

	FString DefaultService;
	GConfig->GetString(TEXT("OnlineSubsystem"), TEXT("DefaultPlatformService"),
		DefaultService, GEngineIni);
	TestEqual(TEXT("the default OSS is Null (EOS is created by name only)"),
		DefaultService, FString(TEXT("Null")));
	bool bSteamEnabled = true;
	GConfig->GetBool(TEXT("OnlineSubsystemSteam"), TEXT("bEnabled"), bSteamEnabled,
		GEngineIni);
	TestFalse(TEXT("the Steam N7 layer is dormant"), bSteamEnabled);
	bool bUseBuildIdOverride = false;
	int32 BuildIdOverride = 0;
	GConfig->GetBool(TEXT("OnlineSubsystem"), TEXT("bUseBuildIdOverride"),
		bUseBuildIdOverride, GEngineIni);
	GConfig->GetInt(TEXT("OnlineSubsystem"), TEXT("BuildIdOverride"),
		BuildIdOverride, GEngineIni);
	TestTrue(TEXT("one pinned build id across engines"),
		bUseBuildIdOverride && BuildIdOverride != 0);
	FString RelayControl;
	GConfig->GetString(TEXT("SocketSubsystemEOS"), TEXT("RelayControl"),
		RelayControl, GEngineIni);
	TestEqual(TEXT("direct P2P preferred, relays allowed"), RelayControl,
		FString(TEXT("AllowRelays")));
	bool bUseEAS = true;
	bool bUseConnect = false;
	GConfig->GetBool(TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("bUseEAS"),
		bUseEAS, GEngineIni);
	GConfig->GetBool(TEXT("/Script/OnlineSubsystemEOS.EOSSettings"),
		TEXT("bUseEOSConnect"), bUseConnect, GEngineIni);
	TestTrue(TEXT("Connect only, no Epic Account Services"), !bUseEAS && bUseConnect);
	TArray<FString> Artifacts;
	GConfig->GetArray(TEXT("/Script/OnlineSubsystemEOS.EOSSettings"), TEXT("Artifacts"),
		Artifacts, GEngineIni);
	TestEqual(TEXT("the repository config carries no EOS credentials"),
		Artifacts.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetSettingsQuerySpec,
	"WoT.Internet.Session.SettingsAndQuery", WoTInternetSpecFlags)

bool FWoTInternetSettingsQuerySpec::RunTest(const FString&)
{
	UWorld* World = WoTInternetSpecs_CreateWorld();
	if (!TestNotNull(TEXT("spec world"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTInternetSpecs_DestroyWorld(World); };
	UWoTSessionSubsystem* Sessions =
		World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("session subsystem"), Sessions))
	{
		return false;
	}

	const FOnlineSessionSettings Internet =
		Sessions->BuildSessionSettingsForTesting(WoTInternetSpecs_Request(false));
	TestFalse(TEXT("internet host is not a LAN match"), Internet.bIsLANMatch);
	TestFalse(TEXT("internet lobby has presence off (no EAS)"), Internet.bUsesPresence);
	TestFalse(TEXT("internet lobby has presence joins off"), Internet.bAllowJoinViaPresence);
	TestTrue(TEXT("internet host creates an EOS lobby"), Internet.bUseLobbiesIfAvailable);
	TestTrue(TEXT("internet lobby is advertised"), Internet.bShouldAdvertise);
	bool bHostMigration = true;
	TestTrue(TEXT("host migration is set"),
		Internet.Get(SETTING_HOST_MIGRATION, bHostMigration));
	TestFalse(TEXT("host migration is OFF: the lobby dies with the listen host"),
		bHostMigration);
	if (const FOnlineSessionSetting* Migration = Internet.Settings.Find(SETTING_HOST_MIGRATION))
	{
		TestEqual(TEXT("host migration is not advertised"),
			static_cast<int32>(Migration->AdvertisementType),
			static_cast<int32>(EOnlineDataAdvertisementType::DontAdvertise));
	}
	for (const TCHAR* Key : {TEXT("WOT_SERVER_NAME"), TEXT("MAPNAME"),
		TEXT("GAMEMODE"), TEXT("WOT_NETVER")})
	{
		const FOnlineSessionSetting* Setting = Internet.Settings.Find(FName(Key));
		if (TestNotNull(FString::Printf(TEXT("internet lobby carries %s"), Key), Setting))
		{
			TestEqual(FString::Printf(TEXT("%s is published to the lobby"), Key),
				static_cast<int32>(Setting->AdvertisementType),
				static_cast<int32>(EOnlineDataAdvertisementType::ViaOnlineServiceAndPing));
		}
	}
	FString NetVersion;
	Internet.Get(FName(TEXT("WOT_NETVER")), NetVersion);
	TestEqual(TEXT("advertised version is the handshake version"),
		NetVersion, UWoTSessionSubsystem::LocalNetVersionString());

	const FOnlineSessionSettings Lan =
		Sessions->BuildSessionSettingsForTesting(WoTInternetSpecs_Request(true));
	TestTrue(TEXT("LAN host is a LAN match"), Lan.bIsLANMatch);
	TestTrue(TEXT("LAN keeps its presence-enabled beacon settings"),
		Lan.bUsesPresence && Lan.bAllowJoinViaPresence);
	TestNull(TEXT("LAN does not touch host migration"),
		Lan.Settings.Find(SETTING_HOST_MIGRATION));

	const TSharedRef<FOnlineSessionSearch> InternetSearch =
		Sessions->BuildSearchForTesting(false);
	bool bLobbies = false;
	TestFalse(TEXT("internet query is not a LAN query"), InternetSearch->bIsLanQuery);
	TestTrue(TEXT("internet query searches EOS LOBBIES"),
		InternetSearch->QuerySettings.Get(SEARCH_LOBBIES, bLobbies) && bLobbies);
	TestNull(TEXT("internet query has no PRESENCESEARCH filter no lobby carries"),
		InternetSearch->QuerySettings.SearchParams.Find(FName(TEXT("PRESENCESEARCH"))));
	TestEqual(TEXT("internet query asks for up to 100 hosts"),
		InternetSearch->MaxSearchResults, 100);
	const TSharedRef<FOnlineSessionSearch> LanSearch = Sessions->BuildSearchForTesting(true);
	TestTrue(TEXT("LAN query is unchanged: LAN"), LanSearch->bIsLanQuery);
	TestNotNull(TEXT("LAN query is unchanged: PRESENCESEARCH"),
		LanSearch->QuerySettings.SearchParams.Find(FName(TEXT("PRESENCESEARCH"))));
	TestEqual(TEXT("LAN query is unchanged: 32"), LanSearch->MaxSearchResults, 32);

	TestEqual(TEXT("LAN menu host URL keeps the IP listen driver"),
		FWoTFrontEndModel::BuildHostTravelURL(WoTInternetSpecs_Request(true),
			EWoTPlayerFaction::Hound),
		FString(TEXT("/Game/WoT/Maps/Arena_01?listen?bIsLanMatch?FragLimit=20"
			"?TimeLimit=0?Cycle=1?MaxPlayers=6?MaxClientTickRate=0"
			"?Class=WOTPawns.Hound")));
	TestEqual(TEXT("internet menu host URL listens on EOS P2P"),
		FWoTFrontEndModel::BuildHostTravelURL(WoTInternetSpecs_Request(false),
			EWoTPlayerFaction::Hound),
		FString(TEXT("/Game/WoT/Maps/Arena_01?listen?FragLimit=20"
			"?TimeLimit=0?Cycle=1?MaxPlayers=6?MaxClientTickRate=0"
			"?Class=WOTPawns.Hound")));
	TestEqual(TEXT("console LAN host appends the option"),
		UWoTSessionSubsystem::AppendLanMatchOptionForTesting(TEXT("?listen"), true),
		FString(TEXT("?listen?bIsLanMatch")));
	TestEqual(TEXT("console internet host leaves the URL alone"),
		UWoTSessionSubsystem::AppendLanMatchOptionForTesting(TEXT("?listen"), false),
		FString(TEXT("?listen")));
	TestEqual(TEXT("the option goes before a portal fragment, once"),
		UWoTSessionSubsystem::AppendLanMatchOptionForTesting(
			UWoTSessionSubsystem::AppendLanMatchOptionForTesting(
				TEXT("?listen#Start"), true), true),
		FString(TEXT("?listen?bIsLanMatch#Start")));
	FString Rejected;
	TestFalse(TEXT("an EOS address is never typed into Direct Address"),
		FWoTFrontEndModel::SanitizeDirectAddress(
			TEXT("EOS:0002abcdef0123456789"), Rejected));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetSignInStateMachineSpec,
	"WoT.Internet.Session.SignInStateMachine", WoTInternetSpecFlags)

bool FWoTInternetSignInStateMachineSpec::RunTest(const FString&)
{
	UWorld* World = WoTInternetSpecs_CreateWorld();
	if (!TestNotNull(TEXT("spec world"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTInternetSpecs_DestroyWorld(World); };
	UWoTSessionSubsystem* Sessions =
		World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("session subsystem"), Sessions))
	{
		return false;
	}
	const IOnlineSubsystem* LanOSS = Online::GetSubsystem(World, NULL_SUBSYSTEM);
	const IOnlineSubsystem* InternetOSS = IOnlineSubsystem::Get(WoTInternetSpecNullInstance);
	if (!TestNotNull(TEXT("LAN Null OSS"), LanOSS)
		|| !TestNotNull(TEXT("stand-in internet provider"), InternetOSS))
	{
		return false;
	}
	const IOnlineSessionPtr LanSessions = LanOSS->GetSessionInterface();
	const IOnlineSessionPtr InternetSessions = InternetOSS->GetSessionInterface();
	TestTrue(TEXT("providers are distinct"), LanSessions != InternetSessions);

	const TSharedRef<FWoTFakeInternetBackend> Fake = MakeShared<FWoTFakeInternetBackend>();
	Fake->SessionsForTest = InternetSessions;
	Sessions->SetInternetBackendForTesting(Fake);
	Sessions->SetOnlineIdentityAcknowledgedForTesting(false);
	Sessions->SetAlwaysUseEpicRelayForTesting(false);
	FString LastTravelURL;
	Sessions->SetServerTravelHookForTesting(
		[&LastTravelURL](UWorld*, const FString& URL, bool)
		{
			LastTravelURL = URL;
			return true;
		});
	int32 HostCompletions = 0;
	bool bLastHost = false;
	int32 SearchCompletions = 0;
	bool bLastSearch = true;
	int32 ResetCompletions = 0;
	bool bLastReset = false;
	Sessions->OnHostComplete().AddLambda([&](bool bOk) { ++HostCompletions; bLastHost = bOk; });
	Sessions->OnSearchComplete().AddLambda(
		[&](bool bOk, const TArray<FWoTSessionListEntry>&) { ++SearchCompletions; bLastSearch = bOk; });
	Sessions->OnOnlineIdentityResetComplete().AddLambda(
		[&](bool bOk) { ++ResetCompletions; bLastReset = bOk; });
	ON_SCOPE_EXIT
	{
		// Before the counters above leave scope: world teardown deactivates
		// the subsystem, which may publish terminal results.
		Sessions->OnHostComplete().Clear();
		Sessions->OnSearchComplete().Clear();
		Sessions->OnOnlineIdentityResetComplete().Clear();
		Sessions->SetServerTravelHookForTesting({});
		Sessions->SetOnlineIdentityAcknowledgedForTesting({});
		Sessions->SetAlwaysUseEpicRelayForTesting({});
		Sessions->SetRelayPreferenceSaveHookForTesting({});
		Sessions->SetInternetBackendForTesting(nullptr);
		if (InternetSessions.IsValid() && InternetSessions->GetNamedSession(NAME_GameSession))
		{
			InternetSessions->RemoveNamedSession(NAME_GameSession);
		}
	};

	// A. No disclosure accepted: nothing reaches EOS.
	TestFalse(TEXT("unacknowledged internet Host is refused"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestEqual(TEXT("refused Host publishes no completion"), HostCompletions, 0);
	TestEqual(TEXT("no EOS platform before the disclosure"), Fake->StartCalls, 0);
	TestEqual(TEXT("no Device ID before the disclosure"), Fake->DeviceIdCalls, 0);
	TestFalse(TEXT("unacknowledged internet Find is refused"), Sessions->Find(false));
	TestEqual(TEXT("refused Find completes exactly once"), SearchCompletions, 1);
	TestFalse(TEXT("refused Find reports failure"), bLastSearch);
	TestEqual(TEXT("still no EOS platform"), Fake->StartCalls, 0);
	TestFalse(TEXT("net-test internet Host respects the disclosure gate"),
		WoTNetTest::Host(World, TEXT("Arena_01"), TEXT("arena"),
			/*FragLimit*/ -1, /*TimeLimit*/ -1, /*bLan*/ false));
	TestEqual(TEXT("net-test internet refusal still creates no EOS platform"),
		Fake->StartCalls, 0);
	TestTrue(TEXT("net-test Host defaults to LAN for existing callers"),
		WoTNetTest::Host(World, TEXT("Battle2_01"), TEXT("battle")));
	TestNotNull(TEXT("default net-test Host uses the LAN provider"),
		LanSessions->GetNamedSession(NAME_GameSession));
	TestTrue(TEXT("absent net-test seal fields keep Battle defaults"),
		LastTravelURL.Contains(TEXT("?SealGoal=4?SealBudget=3")));
	Sessions->NotifySuccessfulHostDestinationExposure();
	TestTrue(TEXT("default net-test Host can leave"), Sessions->Leave());
	FWoTNetTestChannel HostChannel;
	TSharedRef<FJsonObject> HostRequest = MakeShared<FJsonObject>();
	HostRequest->SetStringField(TEXT("cmd"), TEXT("host"));
	HostRequest->SetStringField(TEXT("map"), TEXT("Battle2_01"));
	HostRequest->SetStringField(TEXT("game"), TEXT("battle"));
	HostRequest->SetNumberField(TEXT("sealgoal"), 3.0);
	HostRequest->SetNumberField(TEXT("sealbudget"), 2.0);
	TSharedRef<FJsonObject> HostResponse = MakeShared<FJsonObject>();
	TestTrue(TEXT("net-test host accepts seal fields"),
		HostChannel.DispatchCommandForTesting(World, HostRequest, HostResponse));
	TestTrue(TEXT("net-test seal fields reach the Battle travel URL"),
		LastTravelURL.Contains(TEXT("?SealGoal=3?SealBudget=2")));
	WoTTest::FSpecWorldOptions DestinationOptions;
	DestinationOptions.GameModeClass = AWoTBattleGameMode::StaticClass();
	DestinationOptions.URL = LastTravelURL;
	UWorld* DestinationWorld = WoTTest::CreateSpecWorld(DestinationOptions);
	AWoTBattleGameMode* DestinationMode = DestinationWorld
		? DestinationWorld->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	AWoTCitadelGameState* DestinationState = DestinationWorld
		? DestinationWorld->GetGameState<AWoTCitadelGameState>() : nullptr;
	if (TestNotNull(TEXT("rig destination uses the Battle game mode"), DestinationMode)
		&& TestNotNull(TEXT("rig destination has the Citadel game state"), DestinationState))
	{
		TestEqual(TEXT("rig seal budget reaches the game mode"),
			DestinationMode->SealBudget, 2);
		TestEqual(TEXT("rig seal goal reaches the game state"),
			DestinationState->SealGoal, 3);
	}
	WoTTest::DestroySpecWorld(DestinationWorld);
	Sessions->NotifySuccessfulHostDestinationExposure();
	TestTrue(TEXT("parameterized net-test Host can leave"), Sessions->Leave());
	const int32 HostCompletionsAfterNetTestDefault = HostCompletions;

	// B. Unconfigured build: refused with the reason, LAN untouched.
	Sessions->SetOnlineIdentityAcknowledgedForTesting(true);
	Fake->Unavailability = TEXT("not-configured");
	TestFalse(TEXT("unconfigured internet Host is refused"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestEqual(TEXT("unconfigured Host publishes no completion"), HostCompletions,
		HostCompletionsAfterNetTestDefault);
	TestEqual(TEXT("unconfigured: no EOS platform"), Fake->StartCalls, 0);
	TestFalse(TEXT("an unavailable backend gets no telemetry handler"),
		static_cast<bool>(Fake->Telemetry));
	const FWoTInternetStatus Unconfigured = Sessions->GetInternetStatus();
	TestTrue(TEXT("status is unavailable"),
		Unconfigured.State == EWoTInternetState::Unavailable);
	TestEqual(TEXT("status names the reason"), Unconfigured.Reason,
		FString(TEXT("not-configured")));
	TestTrue(TEXT("player text says not configured"),
		Unconfigured.UserText().Contains(TEXT("not configured")));
	TestTrue(TEXT("readout carries the internet state"),
		Sessions->HostDiagnostics().Contains(TEXT("internet=unavailable inet_reason=not-configured")));
	TestTrue(TEXT("LAN Host still works with EOS unavailable"),
		Sessions->Host(WoTInternetSpecs_Request(true)));
	TestEqual(TEXT("LAN Host completes"), HostCompletions,
		HostCompletionsAfterNetTestDefault + 1);
	TestTrue(TEXT("LAN Host succeeded"), bLastHost);
	TestTrue(TEXT("LAN host URL keeps the IP listen driver"),
		LastTravelURL.Contains(TEXT("?bIsLanMatch")));
	TestNotNull(TEXT("LAN session lives on the Null provider"),
		LanSessions->GetNamedSession(NAME_GameSession));
	TestNull(TEXT("LAN session never reaches the internet provider"),
		InternetSessions->GetNamedSession(NAME_GameSession));
	Sessions->NotifySuccessfulHostDestinationExposure();
	TestTrue(TEXT("LAN leave accepted"), Sessions->Leave());
	TestFalse(TEXT("LAN leave destroyed the session"), Sessions->HasSession());

	// C. Signed out: accepted, parked, signed in, then hosted on EOS.
	Fake->Unavailability.Reset();
	const int32 HostsBeforeSignIn = HostCompletions;
	TestTrue(TEXT("signed-out internet Host is accepted"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestEqual(TEXT("EOS started on the first internet action"), Fake->StartCalls, 1);
	TestEqual(TEXT("Device ID creation dispatched"), Fake->DeviceIdCalls, 1);
	TestTrue(TEXT("Host is parked behind the sign-in"),
		Sessions->IsInternetActionPendingForTesting());
	TestEqual(TEXT("no completion while signing in"), HostCompletions, HostsBeforeSignIn);
	TestTrue(TEXT("status shows the Device ID step"),
		Sessions->GetInternetStatus().State == EWoTInternetState::CreatingDeviceId);
	TestFalse(TEXT("a second Host is refused while signing in"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestFalse(TEXT("an internet Find is refused while signing in"), Sessions->Find(false));
	TestFalse(TEXT("identity reset is refused while signing in"),
		Sessions->ResetOnlineIdentity());
	TestEqual(TEXT("refusals publish nothing"), HostCompletions, HostsBeforeSignIn);
	TestEqual(TEXT("refused Find while signing in publishes nothing"), SearchCompletions, 1);
	Fake->FinishDeviceId(true);
	TestEqual(TEXT("login dispatched after the Device ID"), Fake->LoginCalls, 1);
	TestEqual(TEXT("Connect display name is fixed and non-identifying"),
		Fake->LastDisplayName, FString(TEXT("WoT Player")));
	TestTrue(TEXT("status shows the login step"),
		Sessions->GetInternetStatus().State == EWoTInternetState::LoggingIn);
	LastTravelURL.Reset();
	Fake->FinishLogin(true);
	TestEqual(TEXT("sign-in ready applies the default relay policy once"),
		Fake->RelayControls.Num(), 1);
	TestTrue(TEXT("default relay policy allows direct connections"),
		Fake->RelayControls[0] == EWoTRelayControl::AllowRelays);
	TestEqual(TEXT("signed-in Host completes exactly once"),
		HostCompletions, HostsBeforeSignIn + 1);
	TestTrue(TEXT("signed-in Host succeeded"), bLastHost);
	TestTrue(TEXT("status is ready"),
		Sessions->GetInternetStatus().State == EWoTInternetState::Ready);
	TestFalse(TEXT("internet host URL listens on EOS P2P"),
		LastTravelURL.Contains(TEXT("bIsLanMatch")));
	TestNotNull(TEXT("internet session lives on the internet provider"),
		InternetSessions->GetNamedSession(NAME_GameSession));
	TestNull(TEXT("internet session never reaches the LAN provider"),
		LanSessions->GetNamedSession(NAME_GameSession));
	TestTrue(TEXT("HasSession sees the internet provider"), Sessions->HasSession());
	TestTrue(TEXT("readout names the internet host provider"),
		Sessions->HostDiagnostics().Contains(TEXT("host_provider=internet")));
	TestTrue(TEXT("relay preference can be forced while signed in"),
		Sessions->SetAlwaysUseEpicRelay(true));
	TestEqual(TEXT("relay toggle reaches the backend"),
		Fake->RelayControls.Num(), 2);
	TestTrue(TEXT("relay toggle forces relays"),
		Fake->RelayControls.Last() == EWoTRelayControl::ForceRelays);
	TestTrue(TEXT("readout reports the effective forced-relay setting"),
		Sessions->HostDiagnostics().Contains(TEXT("relay=ForceRelays")));
	TestTrue(TEXT("relay preference can return to direct-preferred"),
		Sessions->SetAlwaysUseEpicRelay(false));
	TestTrue(TEXT("relay toggle allows relays again"),
		Fake->RelayControls.Last() == EWoTRelayControl::AllowRelays);
	Fake->bRelayControlSucceeds = false;
	TestFalse(TEXT("a runtime relay failure rejects the preference change"),
		Sessions->SetAlwaysUseEpicRelay(true));
	TestFalse(TEXT("a rejected runtime relay change restores the saved choice"),
		Sessions->AlwaysUseEpicRelay());
	TestTrue(TEXT("a rejected runtime relay change preserves the effective readout"),
		Sessions->HostDiagnostics().Contains(TEXT("relay=AllowRelays")));
	int32 RelayPreferenceSaveCalls = 0;
	Sessions->SetRelayPreferenceSaveHookForTesting(
		[&RelayPreferenceSaveCalls](bool)
		{
			return ++RelayPreferenceSaveCalls == 1;
		});
	TestFalse(TEXT("an EOS failure plus rollback failure rejects the toggle"),
		Sessions->SetAlwaysUseEpicRelay(true));
	TestTrue(TEXT("failed rollback leaves the requested persisted choice visible"),
		Sessions->AlwaysUseEpicRelay());
	TestTrue(TEXT("failed rollback blocks internet readiness"),
		Sessions->GetInternetStatus().State == EWoTInternetState::Failed);
	TestEqual(TEXT("failed rollback names the unapplied relay policy"),
		Sessions->GetInternetStatus().Reason,
		FString(TEXT("relay-control-failed")));
	Sessions->SetRelayPreferenceSaveHookForTesting({});
	Fake->bRelayControlSucceeds = true;
	TestTrue(TEXT("retry applies the persisted ForceRelays choice"),
		Sessions->SetAlwaysUseEpicRelay(true));
	TestTrue(TEXT("retry restores internet readiness"),
		Sessions->GetInternetStatus().State == EWoTInternetState::Ready);
	TestTrue(TEXT("retry makes ForceRelays effective"),
		Sessions->HostDiagnostics().Contains(TEXT("relay=ForceRelays")));
	TestTrue(TEXT("test returns to the default relay preference"),
		Sessions->SetAlwaysUseEpicRelay(false));
	Sessions->NotifySuccessfulHostDestinationExposure();
	TestTrue(TEXT("internet leave accepted"), Sessions->Leave());
	TestNull(TEXT("leave destroyed the internet session"),
		InternetSessions->GetNamedSession(NAME_GameSession));

	// D. Already signed in: no new sign-in round trip.
	const int32 DeviceIdsBefore = Fake->DeviceIdCalls;
	TestTrue(TEXT("signed-in Host dispatches immediately"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestEqual(TEXT("no Device ID round trip when signed in"),
		Fake->DeviceIdCalls, DeviceIdsBefore);
	TestTrue(TEXT("immediate internet Host succeeded"), bLastHost);
	Sessions->NotifySuccessfulHostDestinationExposure();
	TestTrue(TEXT("immediate host leave"), Sessions->Leave());

	// E. Sign-in lost (Connect expiry not refreshed): back to Idle, explained.
	Fake->bLoggedIn = false;
	if (TestTrue(TEXT("backend got a sign-in-lost handler"), static_cast<bool>(Fake->Lost)))
	{
		Fake->Lost(TEXT("signed-out"));
	}
	const FWoTInternetStatus Lost = Sessions->GetInternetStatus();
	TestTrue(TEXT("lost sign-in returns to idle"), Lost.State == EWoTInternetState::Idle);
	TestTrue(TEXT("lost sign-in is explained"), Lost.UserText().Contains(TEXT("Signed out")));

	// F. A relay-policy failure leaves EOS authenticated, but that authentication
	// alone must never let Host/Find bypass the requested privacy policy.
	Fake->bRelayControlSucceeds = false;
	const int32 HostsBeforeRelayFailure = HostCompletions;
	TestTrue(TEXT("Host is accepted for a relay-policy sign-in failure"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	Fake->FinishDeviceId(true);
	Fake->FinishLogin(true);
	TestEqual(TEXT("relay-policy sign-in failure completes Host once"),
		HostCompletions, HostsBeforeRelayFailure + 1);
	TestFalse(TEXT("relay-policy sign-in failure fails Host"), bLastHost);
	TestEqual(TEXT("relay-policy failure remains visible while EOS is logged in"),
		Sessions->GetInternetStatus().Reason,
		FString(TEXT("relay-control-failed")));
	TestFalse(TEXT("logged-in retry cannot Host with an unapplied relay policy"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	// Keep the deliberately failing pre-fix path from contaminating later cases.
	if (Sessions->HasSession())
	{
		Sessions->NotifySuccessfulHostDestinationExposure();
		Sessions->Leave();
	}
	const int32 SearchesBeforeRelayRetry = SearchCompletions;
	TestFalse(TEXT("logged-in retry cannot Find with an unapplied relay policy"),
		Sessions->Find(false));
	TestEqual(TEXT("blocked relay-policy Find completes exactly once"),
		SearchCompletions, SearchesBeforeRelayRetry + 1);
	TestFalse(TEXT("blocked relay-policy Find reports failure"), bLastSearch);
	Fake->bRelayControlSucceeds = true;
	TestTrue(TEXT("Host retries the relay policy and recovers"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestTrue(TEXT("recovered Host succeeds"), bLastHost);
	TestTrue(TEXT("recovery reports Ready"),
		Sessions->GetInternetStatus().State == EWoTInternetState::Ready);
	TestTrue(TEXT("recovery reapplies the requested AllowRelays policy"),
		Fake->RelayControls.Last() == EWoTRelayControl::AllowRelays);
	Sessions->NotifySuccessfulHostDestinationExposure();
	TestTrue(TEXT("recovered Host can leave"), Sessions->Leave());
	Fake->bLoggedIn = false;
	Fake->Lost(TEXT("signed-out"));

	// G. Login failure: exactly one terminal failure, then a clean retry.
	const int32 HostsBeforeFailure = HostCompletions;
	TestTrue(TEXT("Host accepted for the failing login"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	Fake->FinishDeviceId(true);
	Fake->FinishLogin(false);
	TestEqual(TEXT("failed login publishes exactly one completion"),
		HostCompletions, HostsBeforeFailure + 1);
	TestFalse(TEXT("failed login completion is a failure"), bLastHost);
	const FWoTInternetStatus Failed = Sessions->GetInternetStatus();
	TestTrue(TEXT("status is failed"), Failed.State == EWoTInternetState::Failed);
	TestEqual(TEXT("failure reason"), Failed.Reason, FString(TEXT("login-failed")));
	TestFalse(TEXT("no session after a failed sign-in"), Sessions->HasSession());

	// H. Device ID failure for a Find: one failed search completion, no login.
	const int32 SearchesBefore = SearchCompletions;
	const int32 LoginsBefore = Fake->LoginCalls;
	TestTrue(TEXT("signed-out internet Find is accepted"), Sessions->Find(false));
	TestEqual(TEXT("parked Find has not completed"), SearchCompletions, SearchesBefore);
	Fake->FinishDeviceId(false);
	TestEqual(TEXT("Device ID failure completes the Find once"),
		SearchCompletions, SearchesBefore + 1);
	TestFalse(TEXT("Device ID failure fails the Find"), bLastSearch);
	TestEqual(TEXT("no login after a Device ID failure"), Fake->LoginCalls, LoginsBefore);
	TestEqual(TEXT("Device ID failure reason"), Sessions->GetInternetStatus().Reason,
		FString(TEXT("device-id-failed")));

	// I. Timeout: one failure; the late result is stale.
	const int32 HostsBeforeTimeout = HostCompletions;
	TestTrue(TEXT("Host accepted for the timeout"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestFalse(TEXT("timeout ticker stops"),
		Sessions->TickInternetSignInTimeoutForTesting(FPlatformTime::Seconds() + 3600.0));
	TestEqual(TEXT("timeout publishes exactly one failure"),
		HostCompletions, HostsBeforeTimeout + 1);
	TestFalse(TEXT("timeout completion is a failure"), bLastHost);
	TestEqual(TEXT("timeout reason"), Sessions->GetInternetStatus().Reason,
		FString(TEXT("login-timeout")));
	const int32 LoginsBeforeStale = Fake->LoginCalls;
	Fake->FinishDeviceId(true);
	TestEqual(TEXT("a stale Device ID result starts no login"),
		Fake->LoginCalls, LoginsBeforeStale);
	TestEqual(TEXT("a stale result publishes nothing"),
		HostCompletions, HostsBeforeTimeout + 1);

	// J. Deactivation mid-login: the parked Host fails once, now.
	const int32 HostsBeforeRevoke = HostCompletions;
	TestTrue(TEXT("Host accepted before revocation"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	Fake->FinishDeviceId(true);
	Sessions->DeactivateForUnauthorizedWorld();
	TestEqual(TEXT("revocation publishes the parked Host failure once"),
		HostCompletions, HostsBeforeRevoke + 1);
	TestFalse(TEXT("revoked Host is a failure"), bLastHost);
	TestFalse(TEXT("nothing stays parked"), Sessions->IsInternetActionPendingForTesting());
	Fake->FinishLogin(true);
	TestEqual(TEXT("a late login after revocation publishes nothing"),
		HostCompletions, HostsBeforeRevoke + 1);
	TestNull(TEXT("a late login after revocation creates no lobby"),
		InternetSessions->GetNamedSession(NAME_GameSession));
	TestTrue(TEXT("authority readopts"), Sessions->ActivateForAdoptedWorldAuthority());
	Fake->bLoggedIn = false;
	const int32 SearchesBeforeRevoke = SearchCompletions;
	TestTrue(TEXT("Find accepted before revocation"), Sessions->Find(false));
	Sessions->DeactivateForUnauthorizedWorld();
	TestEqual(TEXT("a parked Find publishes exactly one terminal failure"),
		SearchCompletions, SearchesBeforeRevoke + 1);
	TestFalse(TEXT("the revoked Find is a failure"), bLastSearch);
	Fake->FinishDeviceId(true);
	TestEqual(TEXT("late Device ID after revocation starts no login"),
		Fake->PendingLogin ? 1 : 0, 0);
	TestEqual(TEXT("a late provider result cannot complete the revoked Find twice"),
		SearchCompletions, SearchesBeforeRevoke + 1);
	TestTrue(TEXT("authority readopts again"), Sessions->ActivateForAdoptedWorldAuthority());

	// K0. A failed reset (EOS delete or logout failed) keeps the identity and
	// the disclosure, and says so.
	TestTrue(TEXT("identity reset accepted"), Sessions->ResetOnlineIdentity());
	Fake->FinishReset(false);
	TestEqual(TEXT("failed reset completes once"), ResetCompletions, 1);
	TestFalse(TEXT("failed reset reports failure"), bLastReset);
	TestTrue(TEXT("failed reset keeps the disclosure"),
		Sessions->IsOnlineIdentityAcknowledged());
	TestEqual(TEXT("failed reset reason"), Sessions->GetInternetStatus().Reason,
		FString(TEXT("reset-failed")));

	// K. Reset online identity: logout + delete, then the disclosure again.
	TestTrue(TEXT("identity reset accepted when idle"), Sessions->ResetOnlineIdentity());
	TestTrue(TEXT("status shows the reset"),
		Sessions->GetInternetStatus().State == EWoTInternetState::Resetting);
	TestFalse(TEXT("internet Host refused during the reset"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	Fake->FinishReset(true);
	TestEqual(TEXT("reset completes once"), ResetCompletions, 2);
	TestTrue(TEXT("reset succeeded"), bLastReset);
	TestFalse(TEXT("reset forgets the disclosure"), Sessions->IsOnlineIdentityAcknowledged());
	TestTrue(TEXT("status idle after reset"),
		Sessions->GetInternetStatus().State == EWoTInternetState::Idle);
	TestFalse(TEXT("internet Host needs the disclosure again"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetNetTestHostLanParserSpec,
	"WoT.Internet.NetTest.HostLanParser", WoTInternetSpecFlags)

bool FWoTInternetNetTestHostLanParserSpec::RunTest(const FString&)
{
	const TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	TestTrue(TEXT("omitted lan remains LAN"),
		FWoTNetTestChannel::HostLanFromCommandForTesting(*Request));
	Request->SetBoolField(TEXT("lan"), true);
	TestTrue(TEXT("explicit true selects LAN"),
		FWoTNetTestChannel::HostLanFromCommandForTesting(*Request));
	Request->SetBoolField(TEXT("lan"), false);
	TestFalse(TEXT("explicit false selects internet"),
		FWoTNetTestChannel::HostLanFromCommandForTesting(*Request));
	Request->SetNumberField(TEXT("lan"), 0.0);
	TestTrue(TEXT("numeric lan cannot opt into internet"),
		FWoTNetTestChannel::HostLanFromCommandForTesting(*Request));
	Request->SetStringField(TEXT("lan"), TEXT("false"));
	TestTrue(TEXT("string lan cannot opt into internet"),
		FWoTNetTestChannel::HostLanFromCommandForTesting(*Request));
	Request->SetField(TEXT("lan"), MakeShared<FJsonValueNull>());
	TestTrue(TEXT("null lan cannot opt into internet"),
		FWoTNetTestChannel::HostLanFromCommandForTesting(*Request));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetJoinCleanupSpec,
	"WoT.Internet.Session.JoinCleanup", WoTInternetSpecFlags)

bool FWoTInternetJoinCleanupSpec::RunTest(const FString&)
{
	UWorld* World = WoTInternetSpecs_CreateWorld();
	if (!TestNotNull(TEXT("spec world"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTInternetSpecs_DestroyWorld(World); };
	UWoTSessionSubsystem* Sessions =
		World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	IOnlineSubsystem* RemoteOSS =
		IOnlineSubsystem::Get(FName(TEXT("NULL:WoTJoinSpecRemote")));
	const IOnlineSubsystem* LanOSS = Online::GetSubsystem(World, NULL_SUBSYSTEM);
	if (!TestNotNull(TEXT("session subsystem"), Sessions)
		|| !TestNotNull(TEXT("remote host provider"), RemoteOSS)
		|| !TestNotNull(TEXT("LAN provider"), LanOSS))
	{
		return false;
	}
	const IOnlineSessionPtr Remote = RemoteOSS->GetSessionInterface();
	const IOnlineSessionPtr Lan = LanOSS->GetSessionInterface();
	int32 Joins = 0;
	bool bLastJoin = true;
	int32 Travels = 0;
	Sessions->OnJoinCompleteForUI().AddLambda(
		[&Joins, &bLastJoin](bool bOk) { ++Joins; bLastJoin = bOk; });
	ON_SCOPE_EXIT
	{
		Sessions->OnJoinCompleteForUI().Clear();
		Sessions->SetClientTravelHookForTesting({});
		Sessions->SetClientTravelControllerForTesting(nullptr);
		for (const IOnlineSessionPtr& Provider : {Remote, Lan})
		{
			if (Provider.IsValid() && Provider->GetNamedSession(NAME_GameSession))
			{
				Provider->RemoveNamedSession(NAME_GameSession);
			}
		}
	};

	// A real Null host in a separate provider instance supplies a joinable
	// search row (its session info names the host address).
	TestTrue(TEXT("remote host session created"), Remote->CreateSession(
		0, NAME_GameSession,
		Sessions->BuildSessionSettingsForTesting(WoTInternetSpecs_Request(true))));
	const FNamedOnlineSession* Hosted = Remote->GetNamedSession(NAME_GameSession);
	if (!TestNotNull(TEXT("remote host session exists"), Hosted))
	{
		return false;
	}
	FOnlineSessionSearchResult Row;
	Row.Session = FOnlineSession(*Hosted);

	// Joined but not travelled (no local controller): the provider's named
	// session is left again BEFORE the one failure is published.
	Sessions->SetClientTravelControllerForTesting(nullptr);
	const FWoTSessionListEntry Entry =
		Sessions->InjectSearchResultForTesting(Lan, /*bInternet*/ false, Row);
	TestTrue(TEXT("join dispatched"), Sessions->Join(Entry));
	TestEqual(TEXT("one terminal join result"), Joins, 1);
	TestFalse(TEXT("an untravelled join is a failure"), bLastJoin);
	TestNull(TEXT("the joined session was left again"),
		Lan->GetNamedSession(NAME_GameSession));
	TestFalse(TEXT("the cleanup gate is released"),
		Sessions->IsJoinCleanupPendingForTesting());
	TestFalse(TEXT("no stale named session blocks the next action"),
		Sessions->HasSession());

	// A travelled join keeps its session until Leave.
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	Sessions->SetClientTravelControllerForTesting(Controller);
	Sessions->SetClientTravelHookForTesting(
		[&Travels](APlayerController*, const FString&, ETravelType) { ++Travels; });
	const FWoTSessionListEntry Again =
		Sessions->InjectSearchResultForTesting(Lan, /*bInternet*/ false, Row);
	TestTrue(TEXT("second join dispatched (gate open)"), Sessions->Join(Again));
	TestEqual(TEXT("second join completes once"), Joins, 2);
	TestTrue(TEXT("travelled join succeeds"), bLastJoin);
	TestEqual(TEXT("client travel issued once"), Travels, 1);
	TestTrue(TEXT("the joined session is kept while travelling"), Sessions->HasSession());
	TestTrue(TEXT("leave accepted"), Sessions->Leave());
	TestFalse(TEXT("leave destroyed the joined session"), Sessions->HasSession());
	Remote->DestroySession(NAME_GameSession);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetBackendTelemetrySpec,
	"WoT.Internet.Telemetry.BackendEvents", WoTInternetSpecFlags)

bool FWoTInternetBackendTelemetrySpec::RunTest(const FString&)
{
	FString Parent = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(),
		TEXT("wot-internet-telemetry-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FPaths::NormalizeDirectoryName(Parent);
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("telemetry fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	UWorld* World = WoTInternetSpecs_CreateWorld();
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
		WoTInternetSpecs_DestroyWorld(World);
	};
	if (!TestNotNull(TEXT("spec world"), World))
	{
		return false;
	}
	UWoTSessionSubsystem* Sessions =
		World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	const IOnlineSubsystem* InternetOSS = IOnlineSubsystem::Get(WoTInternetSpecNullInstance);
	if (!TestNotNull(TEXT("session subsystem"), Sessions)
		|| !TestNotNull(TEXT("stand-in internet provider"), InternetOSS))
	{
		return false;
	}
	const IOnlineSessionPtr InternetSessions = InternetOSS->GetSessionInterface();
	const TSharedRef<FWoTFakeInternetBackend> Fake = MakeShared<FWoTFakeInternetBackend>();
	Fake->SessionsForTest = InternetSessions;
	Sessions->SetInternetBackendForTesting(Fake);
	Sessions->SetOnlineIdentityAcknowledgedForTesting(true);
	Sessions->SetAlwaysUseEpicRelayForTesting(false);
	Sessions->SetServerTravelHookForTesting(
		[](UWorld*, const FString&, bool) { return true; });
	ON_SCOPE_EXIT
	{
		Sessions->SetServerTravelHookForTesting({});
		Sessions->SetOnlineIdentityAcknowledgedForTesting({});
		Sessions->SetAlwaysUseEpicRelayForTesting({});
		Sessions->SetInternetBackendForTesting(nullptr);
		if (InternetSessions.IsValid()
			&& InternetSessions->GetNamedSession(NAME_GameSession))
		{
			InternetSessions->RemoveNamedSession(NAME_GameSession);
		}
	};

	TestTrue(TEXT("internet Host starts the backend telemetry seam"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	Fake->FinishDeviceId(true);
	Fake->FinishLogin(true);
	Fake->EmitTelemetry(TEXT("eos-nat"), TEXT("nat=moderate"));
	Fake->EmitTelemetry(TEXT("eos-peer-established"),
		TEXT("connection=direct peer=...abc123 kind=new"));
	Fake->EmitTelemetry(TEXT("eos-peer-established"),
		TEXT("connection=relayed peer=...def456 kind=reconnection"));
	Fake->EmitTelemetry(TEXT("eos-peer-closed"),
		TEXT("connection=direct peer=...abc123 reason=closed-by-peer"));
	Fake->EmitTelemetry(TEXT("eos-peer-closed"),
		TEXT("connection=relayed peer=...def456 reason=timed-out"));

	FString Contents;
	if (!TestTrue(TEXT("telemetry heartbeat is readable"),
		FFileHelper::LoadFileToString(Contents, *FPaths::Combine(
			InstallRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName()))))
	{
		return false;
	}
	TArray<FString> Lines;
	Contents.ParseIntoArrayLines(Lines);
	auto CountEvent = [&Lines](const TCHAR* Event, const TCHAR* Detail) -> int32
	{
		const FString EventField = FString::Printf(TEXT("\"event\":\"%s\""), Event);
		int32 Count = 0;
		for (const FString& Line : Lines)
		{
			Count += Line.Contains(EventField) && Line.Contains(Detail) ? 1 : 0;
		}
		return Count;
	};
	TestEqual(TEXT("one NAT query completion is recorded"),
		CountEvent(TEXT("eos-nat"), TEXT("nat=moderate")), 1);
	TestEqual(TEXT("a direct peer establishment is recorded"),
		CountEvent(TEXT("eos-peer-established"),
			TEXT("connection=direct peer=...abc123 kind=new")), 1);
	TestEqual(TEXT("a relayed peer establishment is recorded"),
		CountEvent(TEXT("eos-peer-established"),
			TEXT("connection=relayed peer=...def456 kind=reconnection")), 1);
	TestEqual(TEXT("a peer close retains its established connection type"),
		CountEvent(TEXT("eos-peer-closed"),
			TEXT("connection=direct peer=...abc123 reason=closed-by-peer")), 1);
	TestEqual(TEXT("a relayed peer close retains its connection type"),
		CountEvent(TEXT("eos-peer-closed"),
			TEXT("connection=relayed peer=...def456 reason=timed-out")), 1);
	TestFalse(TEXT("telemetry never carries a full product user id"),
		Contents.Contains(TEXT("00000000000000000000000000abc123")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetListenClientTelemetrySpec,
	"WoT.Internet.Telemetry.ListenClientLifecycle", WoTInternetSpecFlags)

bool FWoTInternetListenClientTelemetrySpec::RunTest(const FString&)
{
	FString Parent = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(),
		TEXT("wot-client-lifecycle-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FPaths::NormalizeDirectoryName(Parent);
	const FString InstallRoot = FPaths::Combine(Parent, TEXT("WoT"));
	if (!TestTrue(TEXT("client telemetry fixture parent is created"),
		IFileManager::Get().MakeDirectory(*Parent, true)))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(InstallRoot);
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.GameModeClass = AWoTGameMode::StaticClass();
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	UIpNetDriver* Driver = nullptr;
	ON_SCOPE_EXIT
	{
		if (World && Driver)
		{
			World->SetNetDriver(nullptr);
			Driver->SetWorld(nullptr);
		}
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Parent, false, true);
		WoTTest::DestroySpecWorld(World);
		// Prove that this synthetic listen client leaves no invalid net object
		// for a later suite's garbage collection to discover.
		CollectGarbage(RF_NoFlags);
	};
	if (!TestNotNull(TEXT("listen telemetry world"), World))
	{
		return false;
	}
	Driver = NewObject<UIpNetDriver>(World);
	Driver->SetWorld(World);
	World->SetNetDriver(Driver);
	TestTrue(TEXT("synthetic server driver makes a listen world"),
		World->GetNetMode() == NM_ListenServer);
	UWoTSessionSubsystem* Sessions =
		World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	AWoTGameMode* Mode = World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("listen session subsystem"), Sessions)
		|| !TestNotNull(TEXT("listen game mode"), Mode))
	{
		return false;
	}
	Sessions->SetHostProviderForTesting(TEXT("internet"));
	World->SpawnActor<APlayerStart>(FVector(500.0, 0.0, 100.0),
		FRotator::ZeroRotator);
	AWoTPlayerController* Remote = World->SpawnActor<AWoTPlayerController>();
	if (!TestNotNull(TEXT("remote controller"), Remote))
	{
		return false;
	}
	UPlayer* RemotePlayer = NewObject<UPlayer>(Remote);
	RemotePlayer->PlayerController = Remote;
	RemotePlayer->CurrentNetSpeed = 10000;
	Remote->Player = RemotePlayer;
	if (!Remote->GetPlayerState<AWoTPlayerState>())
	{
		Remote->InitPlayerState();
	}
	AWoTPlayerState* RemoteState = Remote->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("remote player state"), RemoteState))
	{
		return false;
	}
	const FString FullPlayerId(TEXT("00000000000000000000000000abc123"));
	const FUniqueNetIdRef RemoteUniqueId =
		FUniqueNetIdString::Create(FullPlayerId, EOS_SUBSYSTEM);
	RemoteState->SetUniqueId(FUniqueNetIdRepl(RemoteUniqueId));
	Mode->PostLogin(Remote);
	Mode->Logout(Remote);

	FString Contents;
	if (!TestTrue(TEXT("client lifecycle heartbeat is readable"),
		FFileHelper::LoadFileToString(Contents, *FPaths::Combine(
			InstallRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName()))))
	{
		return false;
	}
	TestTrue(TEXT("listen host records the remote client join"),
		Contents.Contains(TEXT("\"event\":\"client-join\""))
		&& Contents.Contains(TEXT(
			"player=...abc123 provider=internet netdriver=IpNetDriver count=1")));
	TestTrue(TEXT("listen host records the remote client leave"),
		Contents.Contains(TEXT("\"event\":\"client-leave\""))
		&& Contents.Contains(TEXT(
			"player=...abc123 provider=internet netdriver=IpNetDriver count=0")));
	TestFalse(TEXT("client lifecycle never carries a full player id"),
		Contents.Contains(FullPlayerId));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetEOSAbsentSpec,
	"WoT.Internet.Session.EOSAbsentIsNoOp", WoTInternetSpecFlags)

bool FWoTInternetEOSAbsentSpec::RunTest(const FString&)
{
	UWorld* World = WoTInternetSpecs_CreateWorld();
	if (!TestNotNull(TEXT("spec world"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTInternetSpecs_DestroyWorld(World); };
	UWoTSessionSubsystem* Sessions =
		World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("session subsystem"), Sessions))
	{
		return false;
	}
	// The REAL backend, with the repository config (no credentials).
	Sessions->SetInternetBackendForTesting(nullptr);
	Sessions->SetOnlineIdentityAcknowledgedForTesting(true);
	ON_SCOPE_EXIT { Sessions->SetOnlineIdentityAcknowledgedForTesting({}); };
	// Automation never reads the operator's secrets file (the backend skips
	// it under GIsAutomationTesting), so this is the repository config: no
	// EOS artifact at all.
	const FWoTInternetStatus Status = Sessions->GetInternetStatus();
	TestTrue(TEXT("internet play is unavailable without credentials"),
		Status.State == EWoTInternetState::Unavailable);
	TestTrue(TEXT("unavailable for a known reason"),
		Status.Reason == TEXT("not-configured") || Status.Reason == TEXT("disabled")
		|| Status.Reason == TEXT("sdk-unavailable"));
	const bool bEOSBefore = IOnlineSubsystem::DoesInstanceExist(EOS_SUBSYSTEM);
	int32 HostCompletions = 0;
	Sessions->OnHostComplete().AddLambda([&HostCompletions](bool) { ++HostCompletions; });
	TestFalse(TEXT("internet Host is refused"),
		Sessions->Host(WoTInternetSpecs_Request(false)));
	TestEqual(TEXT("refusal publishes no completion"), HostCompletions, 0);
	TestEqual(TEXT("no EOS OSS instance is created by a refused action"),
		IOnlineSubsystem::DoesInstanceExist(EOS_SUBSYSTEM), bEOSBefore);
	TestFalse(TEXT("nothing parked"), Sessions->IsInternetActionPendingForTesting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetDisclosureMarkerSpec,
	"WoT.Internet.Preferences.OnlineIdentityMarker", WoTInternetSpecFlags)

bool FWoTInternetDisclosureMarkerSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), FString(TEXT("OnlineIdentity-")) + FGuid::NewGuid().ToString());
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ResetWoTPayloadActivationForTesting();
	ON_SCOPE_EXIT
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	UWoTGameInstance* Instance = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Preferences = NewObject<UWoTPreferencesSubsystem>(Instance);
	TArray<uint8> Bytes;
	const ANSICHAR* Text = "Schema=EarlyUI-v1\n";
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
		Bytes, FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
	if (Parsed.IsError())
	{
		AddError(Parsed.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("adopt EarlyUI"),
		Preferences->AdoptAuthenticatedEarlyUI(MoveTemp(Parsed).GetValue()).IsError());
	TestFalse(TEXT("dormant before activation: not acknowledged"),
		Preferences->HasAcknowledgedOnlineIdentity());
	TestTrue(TEXT("dormant before activation: refused"),
		Preferences->SetOnlineIdentityAcknowledged(true).IsError());
	TestFalse(TEXT("activate"),
		PublishWoTPayloadBootstrapActivationForTesting().IsError());
	TestFalse(TEXT("load full preferences"),
		Preferences->ApplyFullPreferencesAfterCacheActivation().IsError());
	TestFalse(TEXT("a fresh profile has not accepted"),
		Preferences->HasAcknowledgedOnlineIdentity());
	TestFalse(TEXT("accept"), Preferences->SetOnlineIdentityAcknowledged(true).IsError());
	TestTrue(TEXT("accepted"), Preferences->HasAcknowledgedOnlineIdentity());
	TestTrue(TEXT("the marker is a separate file"),
		IFileManager::Get().FileExists(*(Leaf + TEXT(".WoTOnlineIdentity"))));
	TestFalse(TEXT("the closed preference file is not written by the marker"),
		IFileManager::Get().FileExists(*Leaf));
	TestFalse(TEXT("forget"), Preferences->SetOnlineIdentityAcknowledged(false).IsError());
	TestFalse(TEXT("forgotten"), Preferences->HasAcknowledgedOnlineIdentity());
	TestFalse(TEXT("a fresh profile has not seen the local-network heads-up"),
		Preferences->HasSeenLocalNetworkHeadsUp());
	TestFalse(TEXT("record the local-network heads-up"),
		Preferences->SetLocalNetworkHeadsUpSeen().IsError());
	TestTrue(TEXT("the local-network heads-up is remembered"),
		Preferences->HasSeenLocalNetworkHeadsUp());
	TestTrue(TEXT("its marker is its own file"),
		IFileManager::Get().FileExists(*(Leaf + TEXT(".WoTLocalNetworkHeadsUp"))));
	TestFalse(TEXT("it is independent of the online identity"),
		Preferences->HasAcknowledgedOnlineIdentity());
	TestFalse(TEXT("forgetting twice is fine"),
		Preferences->SetOnlineIdentityAcknowledged(false).IsError());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetRelayPreferenceSpec,
	"WoT.Internet.Preferences.RelayPolicy", WoTInternetSpecFlags)

bool FWoTInternetRelayPreferenceSpec::RunTest(const FString&)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), FString(TEXT("RelayPolicy-")) + FGuid::NewGuid().ToString());
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ResetWoTPayloadActivationForTesting();
	ON_SCOPE_EXIT
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (PublishWoTPayloadBootstrapActivationForTesting().IsError())
	{
		AddError(TEXT("could not activate relay-preference fixture"));
		return false;
	}
	auto LoadPreferences = [this](UWoTGameInstance* Instance)
		-> UWoTPreferencesSubsystem*
	{
		UWoTPreferencesSubsystem* Preferences =
			NewObject<UWoTPreferencesSubsystem>(Instance);
		TArray<uint8> Bytes;
		const ANSICHAR* Text = "Schema=EarlyUI-v1\n";
		Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
			Bytes, FWoTEarlyUILimits::Production(),
			FWoTInventoryLocaleSet::Production());
		if (Parsed.IsError()
			|| Preferences->AdoptAuthenticatedEarlyUI(
				MoveTemp(Parsed).GetValue()).IsError()
			|| Preferences->ApplyFullPreferencesAfterCacheActivation().IsError())
		{
			AddError(TEXT("could not load relay-preference fixture"));
			return nullptr;
		}
		return Preferences;
	};

	UWoTGameInstance* FirstInstance = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* First = LoadPreferences(FirstInstance);
	if (!TestNotNull(TEXT("first preference owner"), First)) return false;
	TestFalse(TEXT("fresh profiles prefer direct P2P"),
		First->GetPreferences().bAlwaysUseEpicRelay);
	FWoTPlayerPreferences Forced = First->GetPreferences();
	Forced.bAlwaysUseEpicRelay = true;
	TestFalse(TEXT("forced-relay preference saves"), First->Save(Forced).IsError());

	UWoTGameInstance* ReloadedInstance = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Reloaded = LoadPreferences(ReloadedInstance);
	if (!TestNotNull(TEXT("reloaded preference owner"), Reloaded)) return false;
	TestTrue(TEXT("forced-relay preference round-trips"),
		Reloaded->GetPreferences().bAlwaysUseEpicRelay);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInternetViewModelSpec,
	"WoT.Internet.FrontEnd.ViewModel", WoTInternetSpecFlags)

bool FWoTInternetViewModelSpec::RunTest(const FString&)
{
	FWoTFrontEndViewModel Model;
	Model.ShowPlay({});
	TestFalse(TEXT("LAN is the default"), Model.IsInternet());
	Model.SetLAN(false);
	TestTrue(TEXT("Internet selectable in the model"), Model.IsInternet());
	Model.ReportNotice(TEXT("notice"), /*bError*/ true);
	TestEqual(TEXT("idle notice shows"), Model.GetStatusText(), FString(TEXT("notice")));
	TestTrue(TEXT("idle notice is an error"), Model.IsStatusError());
	TestTrue(TEXT("reset begins"), Model.BeginResetIdentity());
	Model.ReportNotice(TEXT("ignored"), false);
	TestEqual(TEXT("a notice never overwrites an operation"),
		Model.GetStatusText(), FString(TEXT("Resetting the online identity...")));
	Model.FinishResetIdentity(true);
	TestFalse(TEXT("a reset returns the choice to LAN"), Model.IsInternet());
	TestTrue(TEXT("reset success text"),
		Model.GetStatusText().StartsWith(TEXT("Online identity reset.")));
	TestFalse(TEXT("reset success is not an error"), Model.IsStatusError());
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
