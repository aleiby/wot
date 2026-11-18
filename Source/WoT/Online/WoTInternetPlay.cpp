#include "WoTInternetPlay.h"

#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSubsystemUtils.h"

#if WOT_WITH_EOS
#include "EOSSettings.h"
#include "EOSShared.h"
#include "EOSSharedTypes.h"
#include "IEOSSDKManager.h"
#include "IOnlineSubsystemEOS.h"
#include "eos_connect.h"
#include "eos_p2p.h"
#include "eos_sdk.h"
#endif

const TCHAR* LexToString(EWoTInternetState State)
{
	switch (State)
	{
	case EWoTInternetState::Unavailable: return TEXT("unavailable");
	case EWoTInternetState::Idle: return TEXT("idle");
	case EWoTInternetState::CreatingDeviceId: return TEXT("creating-device-id");
	case EWoTInternetState::LoggingIn: return TEXT("logging-in");
	case EWoTInternetState::Ready: return TEXT("ready");
	case EWoTInternetState::Failed: return TEXT("failed");
	case EWoTInternetState::Resetting: return TEXT("resetting");
	default: return TEXT("unknown");
	}
}

const TCHAR* LexToString(EWoTRelayControl Control)
{
	switch (Control)
	{
	case EWoTRelayControl::AllowRelays: return TEXT("AllowRelays");
	case EWoTRelayControl::ForceRelays: return TEXT("ForceRelays");
	default: return TEXT("AllowRelays");
	}
}

FString FWoTEOSConfig::PreferredArtifactName(bool bShippingBuild,
	bool bTestArtifactPresent)
{
	if (bShippingBuild)
	{
		return ShippingArtifactName();
	}
	return bTestArtifactPresent ? TestArtifactName() : ShippingArtifactName();
}

bool FWoTEOSConfig::IsPlaceholder(const FString& Value)
{
	const FString Trimmed = Value.TrimStartAndEnd();
	return Trimmed.IsEmpty()
		|| Trimmed.StartsWith(TEXT("REPLACE_WITH"), ESearchCase::IgnoreCase)
		|| Trimmed.StartsWith(TEXT("PLACEHOLDER"), ESearchCase::IgnoreCase)
		|| Trimmed.Contains(TEXT("<")) || Trimmed.Contains(TEXT(">"));
}

FString FWoTEOSConfig::ValidateArtifact(const FWoTEOSArtifact& Artifact)
{
	const FString* Fields[] = {
		&Artifact.ProductId, &Artifact.SandboxId, &Artifact.DeploymentId,
		&Artifact.ClientId, &Artifact.ClientSecret,
	};
	for (const FString* Field : Fields)
	{
		if (IsPlaceholder(*Field) || Field->Len() > 128)
		{
			return TEXT("not-configured");
		}
		for (const TCHAR Char : *Field)
		{
			// Printable ASCII only, and nothing that could end the quoted
			// +Artifacts value or the struct literal around it.
			if (Char <= TEXT(' ') || Char > TEXT('~') || Char == TEXT('"')
				|| Char == TEXT('(') || Char == TEXT(')') || Char == TEXT(',')
				|| Char == TEXT('\\'))
			{
				return TEXT("not-configured");
			}
		}
	}
	return FString();
}

namespace
{
	/**
	 * Substrings OnlineSubsystemEOS / OnlineSubsystem read with FParse::Value
	 * (which matches anywhere in the stream, not only at a token start).
	 */
	const TCHAR* const WoTEOSOverrideKeys[] = {
		TEXT("EpicApp="),
		TEXT("EOSArtifactNameOverride="),
		TEXT("EpicSandboxId"),
		TEXT("EpicDeploymentId"),
		TEXT("AUTH_TYPE="),
		TEXT("AUTH_LOGIN="),
		TEXT("AUTH_PASSWORD="),
		TEXT("BuildIdOverride="),
	};
}

FString FWoTEOSConfig::StripShippingCommandLineOverrides(FStringView CommandLine)
{
	TArray<FString> Kept;
	bool bChanged = false;
	int32 Cursor = 0;
	while (Cursor < CommandLine.Len())
	{
		while (Cursor < CommandLine.Len() && FChar::IsWhitespace(CommandLine[Cursor]))
		{
			++Cursor;
		}
		if (Cursor >= CommandLine.Len())
		{
			break;
		}
		const int32 Start = Cursor;
		bool bInsideQuotes = false;
		while (Cursor < CommandLine.Len()
			&& (bInsideQuotes || !FChar::IsWhitespace(CommandLine[Cursor])))
		{
			if (CommandLine[Cursor] == TEXT('"'))
			{
				bInsideQuotes = !bInsideQuotes;
			}
			++Cursor;
		}
		const FString Token(CommandLine.Mid(Start, Cursor - Start));
		bool bOverride = false;
		for (const TCHAR* Key : WoTEOSOverrideKeys)
		{
			if (Token.Contains(Key, ESearchCase::IgnoreCase))
			{
				bOverride = true;
				break;
			}
		}
		if (bOverride)
		{
			bChanged = true;
			continue;
		}
		Kept.Add(Token);
	}
	return bChanged ? FString::Join(Kept, TEXT(" ")) : FString(CommandLine);
}

FString FWoTEOSConfig::RedactId(const FString& Id)
{
	if (Id.IsEmpty())
	{
		return TEXT("none");
	}
	return Id.Len() <= 6 ? FString(TEXT("...")) : TEXT("...") + Id.Right(6);
}

FString FWoTEOSConfig::DescribeReason(const FString& Reason)
{
	if (Reason == TEXT("not-compiled"))
	{
		return TEXT("Internet play is not available on this platform. LAN and direct IP still work.");
	}
	if (Reason == TEXT("disabled"))
	{
		return TEXT("Internet play is disabled for this run. LAN and direct IP still work.");
	}
	if (Reason == TEXT("not-configured"))
	{
		return TEXT("Internet play is not configured in this build. LAN and direct IP still work.");
	}
	if (Reason == TEXT("sdk-unavailable"))
	{
		return TEXT("The Epic Online Services library could not be loaded. LAN and direct IP still work.");
	}
	if (Reason == TEXT("init-failed"))
	{
		return TEXT("Epic Online Services failed to start. LAN and direct IP still work.");
	}
	if (Reason == TEXT("device-id-failed"))
	{
		return TEXT("Could not create an anonymous online identity. Check your internet connection and try again.");
	}
	if (Reason == TEXT("login-failed"))
	{
		return TEXT("Could not sign in to Epic Online Services. Check your internet connection and try again.");
	}
	if (Reason == TEXT("login-timeout"))
	{
		return TEXT("Signing in to Epic Online Services timed out. Try again.");
	}
	if (Reason == TEXT("relay-control-failed"))
	{
		return TEXT("Could not apply the Epic relay privacy setting. Internet play stays off.");
	}
	if (Reason == TEXT("signed-out"))
	{
		return TEXT("Signed out of Epic Online Services. The next internet action signs in again.");
	}
	if (Reason == TEXT("reset-failed"))
	{
		return TEXT("Could not reset the online identity. Try again.");
	}
	if (Reason == TEXT("not-acknowledged"))
	{
		return TEXT("Internet play needs your OK first: choose Internet and accept the notice.");
	}
	return TEXT("Internet play is unavailable. LAN and direct IP still work.");
}

#if WOT_WITH_EOS
namespace
{
	FWoTEOSArtifact WoTToArtifact(const FArtifactSettings& Settings)
	{
		FWoTEOSArtifact Artifact;
		Artifact.ArtifactName = Settings.ArtifactName;
		Artifact.ProductId = Settings.ProductId;
		Artifact.SandboxId = Settings.SandboxId;
		Artifact.DeploymentId = Settings.DeploymentId;
		Artifact.ClientId = Settings.ClientId;
		Artifact.ClientSecret = Settings.ClientSecret;
		return Artifact;
	}

	const FArtifactSettings* WoTFindArtifact(const UEOSSettings& Settings,
		const FString& Name)
	{
		return Settings.Artifacts.FindByPredicate(
			[&Name](const FArtifactSettings& Candidate)
			{
				return Candidate.ArtifactName == Name;
			});
	}

	const TCHAR* WoTNatTypeName(EOS_ENATType Type)
	{
		switch (Type)
		{
		case EOS_ENATType::EOS_NAT_Open: return TEXT("open");
		case EOS_ENATType::EOS_NAT_Moderate: return TEXT("moderate");
		case EOS_ENATType::EOS_NAT_Strict: return TEXT("strict");
		default: return TEXT("unknown");
		}
	}

	const TCHAR* WoTNetworkTypeName(EOS_ENetworkConnectionType Type)
	{
		switch (Type)
		{
		case EOS_ENetworkConnectionType::EOS_NCT_DirectConnection:
			return TEXT("direct");
		case EOS_ENetworkConnectionType::EOS_NCT_RelayedConnection:
			return TEXT("relayed");
		default:
			return TEXT("unknown");
		}
	}

	const TCHAR* WoTEstablishedKindName(EOS_EConnectionEstablishedType Type)
	{
		return Type == EOS_EConnectionEstablishedType::EOS_CET_Reconnection
			? TEXT("reconnection") : TEXT("new");
	}

	const TCHAR* WoTClosedReasonName(EOS_EConnectionClosedReason Reason)
	{
		switch (Reason)
		{
		case EOS_EConnectionClosedReason::EOS_CCR_ClosedByLocalUser:
			return TEXT("closed-by-local-user");
		case EOS_EConnectionClosedReason::EOS_CCR_ClosedByPeer:
			return TEXT("closed-by-peer");
		case EOS_EConnectionClosedReason::EOS_CCR_TimedOut:
			return TEXT("timed-out");
		case EOS_EConnectionClosedReason::EOS_CCR_TooManyConnections:
			return TEXT("too-many-connections");
		case EOS_EConnectionClosedReason::EOS_CCR_InvalidMessage:
			return TEXT("invalid-message");
		case EOS_EConnectionClosedReason::EOS_CCR_InvalidData:
			return TEXT("invalid-data");
		case EOS_EConnectionClosedReason::EOS_CCR_ConnectionFailed:
			return TEXT("connection-failed");
		case EOS_EConnectionClosedReason::EOS_CCR_ConnectionClosed:
			return TEXT("connection-closed");
		case EOS_EConnectionClosedReason::EOS_CCR_NegotiationFailed:
			return TEXT("negotiation-failed");
		case EOS_EConnectionClosedReason::EOS_CCR_UnexpectedError:
			return TEXT("unexpected-error");
		case EOS_EConnectionClosedReason::EOS_CCR_ConnectionIgnored:
			return TEXT("connection-ignored");
		default:
			return TEXT("unknown");
		}
	}

	FString WoTProductUserIdString(EOS_ProductUserId UserId)
	{
		char Buffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
		int32_t BufferLength = UE_ARRAY_COUNT(Buffer);
		return UserId != nullptr
			&& EOS_ProductUserId_ToString(UserId, Buffer, &BufferLength)
				== EOS_EResult::EOS_Success
			? FString(UTF8_TO_TCHAR(Buffer)) : FString();
	}

	FString WoTPeerConnectionKey(EOS_ProductUserId RemoteUserId,
		const EOS_P2P_SocketId* SocketId)
	{
		return WoTProductUserIdString(RemoteUserId) + TEXT("|")
			+ (SocketId != nullptr
				? FString(UTF8_TO_TCHAR(SocketId->SocketName)) : FString());
	}

	const TCHAR* WoTDeviceModel()
	{
#if PLATFORM_WINDOWS
		return TEXT("PC Windows");
#elif PLATFORM_MAC
		return TEXT("Mac");
#else
		return TEXT("Desktop");
#endif
	}
}

class FWoTEOSInternetBackend final
	: public IWoTInternetBackend
	, public TSharedFromThis<FWoTEOSInternetBackend>
{
public:
	FWoTEOSInternetBackend()
	{
#if !UE_BUILD_SHIPPING && !WOT_GUARDED_BOOTSTRAP
		ApplyLocalCredentialFile();
#endif
	}

	virtual ~FWoTEOSInternetBackend() override
	{
		ClearPeerConnectionNotifications();
		if (IEOSSDKManager* Manager = IEOSSDKManager::Get();
			Manager != nullptr && NetworkStatusHandle.IsValid())
		{
			Manager->OnNetworkStatusChanged.Remove(NetworkStatusHandle);
		}
		if (Identity.IsValid())
		{
			if (LoginCompleteHandle.IsValid())
			{
				Identity->ClearOnLoginCompleteDelegate_Handle(0, LoginCompleteHandle);
			}
			if (LoginStatusHandle.IsValid())
			{
				Identity->ClearOnLoginStatusChangedDelegate_Handle(0, LoginStatusHandle);
			}
			if (LogoutCompleteHandle.IsValid())
			{
				Identity->ClearOnLogoutCompleteDelegate_Handle(0, LogoutCompleteHandle);
			}
		}
	}

	virtual FString QuickUnavailability() const override
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("NoEOS"))
			|| !IOnlineSubsystem::IsEnabled(EOS_SUBSYSTEM))
		{
			return TEXT("disabled");
		}
		FWoTEOSArtifact Artifact;
		if (!SelectArtifact(Artifact)
			|| !FWoTEOSConfig::ValidateArtifact(Artifact).IsEmpty())
		{
			return TEXT("not-configured");
		}
		const IEOSSDKManager* Manager = IEOSSDKManager::Get();
		if (Manager == nullptr || !Manager->IsInitialized())
		{
			return TEXT("sdk-unavailable");
		}
		if (bInitFailed)
		{
			return TEXT("init-failed");
		}
		return FString();
	}

	virtual FString EnsureStarted(UWorld* World) override
	{
		if (Subsystem != nullptr)
		{
			return FString();
		}
		const FString Problem = QuickUnavailability();
		if (!Problem.IsEmpty())
		{
			return Problem;
		}
		FWoTEOSArtifact Artifact;
		SelectArtifact(Artifact);
		// The OSS selects its artifact from DefaultArtifactName (EOSSettings.cpp
		// GetSelectedArtifactSettings); Shipping strips the command-line
		// overrides that could redirect it (FWoTEOSConfig).
		GetMutableDefault<UEOSSettings>()->DefaultArtifactName = Artifact.ArtifactName;
		SelectedArtifact = Artifact.ArtifactName;
		IOnlineSubsystem* OSS = Online::GetSubsystem(World, EOS_SUBSYSTEM);
		if (OSS == nullptr || OSS->GetSubsystemName() != EOS_SUBSYSTEM
			|| !OSS->GetIdentityInterface().IsValid()
			|| !OSS->GetSessionInterface().IsValid())
		{
			bInitFailed = true;
			return TEXT("init-failed");
		}
		Subsystem = OSS;
		Identity = OSS->GetIdentityInterface();
		LoginStatusHandle = Identity->AddOnLoginStatusChangedDelegate_Handle(0,
			FOnLoginStatusChangedDelegate::CreateSP(this,
				&FWoTEOSInternetBackend::HandleLoginStatusChanged));
		if (IEOSSDKManager* Manager = IEOSSDKManager::Get();
			Manager != nullptr && !NetworkStatusHandle.IsValid())
		{
			NetworkStatusHandle = Manager->OnNetworkStatusChanged.AddSP(this,
				&FWoTEOSInternetBackend::HandleNetworkStatusChanged);
		}
		if (IsLoggedIn())
		{
			QueryNatType();
			RegisterPeerConnectionNotifications();
		}
		return FString();
	}

	virtual IOnlineSessionPtr GetSessionInterface() const override
	{
		return Subsystem != nullptr ? Subsystem->GetSessionInterface() : nullptr;
	}

	virtual bool IsLoggedIn() const override
	{
		return Identity.IsValid()
			&& Identity->GetLoginStatus(0) == ELoginStatus::LoggedIn;
	}

	virtual FString RedactedUserId() const override
	{
		if (!IsLoggedIn())
		{
			return TEXT("none");
		}
		const FUniqueNetIdPtr UserId = Identity->GetUniquePlayerId(0);
		return FWoTEOSConfig::RedactId(UserId.IsValid() ? UserId->ToString() : FString());
	}

	virtual void CreateDeviceId(FDone Done) override
	{
		const EOS_HPlatform Platform = PlatformHandle();
		if (Platform == nullptr)
		{
			Done(false, TEXT("no-platform"));
			return;
		}
		EOS_Connect_CreateDeviceIdOptions Options = {};
		Options.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
		const FTCHARToUTF8 Model(WoTDeviceModel());
		Options.DeviceModel = Model.Get();
		using FCallback = TEOSCallback<EOS_Connect_OnCreateDeviceIdCallback,
			EOS_Connect_CreateDeviceIdCallbackInfo, FWoTEOSInternetBackend>;
		FCallback* Callback = new FCallback(AsWeak());
		Callback->CallbackLambda =
			[Done](const EOS_Connect_CreateDeviceIdCallbackInfo* Data)
			{
				// eos_connect.h: an existing Device ID answers
				// EOS_DuplicateNotAllowed and the caller proceeds to login.
				const bool bOk = Data->ResultCode == EOS_EResult::EOS_Success
					|| Data->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed;
				Done(bOk, LexToString(Data->ResultCode));
			};
		EOS_Connect_CreateDeviceId(EOS_Platform_GetConnectInterface(Platform),
			&Options, Callback, Callback->GetCallbackPtr());
	}

	virtual void Login(const FString& DisplayName, FDone Done) override
	{
		if (!Identity.IsValid())
		{
			Done(false, TEXT("no-identity"));
			return;
		}
		if (IsLoggedIn())
		{
			QueryNatType();
			RegisterPeerConnectionNotifications();
			Done(true, TEXT("already-signed-in"));
			return;
		}
		if (PendingLogin)
		{
			Done(false, TEXT("login-in-flight"));
			return;
		}
		PendingLogin = MoveTemp(Done);
		LoginCompleteHandle = Identity->AddOnLoginCompleteDelegate_Handle(0,
			FOnLoginCompleteDelegate::CreateSP(this,
				&FWoTEOSInternetBackend::HandleLoginComplete));
		// Any prefix; "DeviceIdAccessToken" after the colon selects
		// EOS_ECT_DEVICEID_ACCESS_TOKEN (UserManagerEOS.cpp CallEOSConnectLogin
		// + EOSShared.cpp LexFromString). Id is the Connect display name; the
		// guarded engine patch sends it as UserLoginInfo with a NULL token.
		const FOnlineAccountCredentials Credentials(
			TEXT("deviceid:DeviceIdAccessToken"), DisplayName, FString());
		if (!Identity->Login(0, Credentials) && PendingLogin)
		{
			ClearLoginDelegate();
			FDone Failed = MoveTemp(PendingLogin);
			PendingLogin = nullptr;
			Failed(false, TEXT("login-refused"));
		}
	}

	virtual void ResetIdentity(FDone Done) override
	{
		if (Subsystem == nullptr || !Identity.IsValid())
		{
			Done(false, TEXT("not-started"));
			return;
		}
		if (PendingReset)
		{
			Done(false, TEXT("reset-in-flight"));
			return;
		}
		PendingReset = MoveTemp(Done);
		if (IsLoggedIn())
		{
			LogoutCompleteHandle = Identity->AddOnLogoutCompleteDelegate_Handle(0,
				FOnLogoutCompleteDelegate::CreateSP(this,
					&FWoTEOSInternetBackend::HandleLogoutComplete));
			if (Identity->Logout(0) || !PendingReset)
			{
				return;
			}
			Identity->ClearOnLogoutCompleteDelegate_Handle(0, LogoutCompleteHandle);
			LogoutCompleteHandle.Reset();
		}
		DeleteDeviceId();
	}

	virtual void SetSignInLostHandler(
		TFunction<void(const FString&)> Handler) override
	{
		SignInLost = MoveTemp(Handler);
	}

	virtual void SetTelemetryHandler(FTelemetry Handler) override
	{
		Telemetry = MoveTemp(Handler);
		if (Telemetry && IsLoggedIn())
		{
			RegisterPeerConnectionNotifications();
		}
	}

	virtual bool SetRelayControl(EWoTRelayControl Control) override
	{
		const EOS_HPlatform Platform = PlatformHandle();
		EOS_HP2P P2P = Platform != nullptr
			? EOS_Platform_GetP2PInterface(Platform) : nullptr;
		if (P2P == nullptr)
		{
			return false;
		}
		EOS_P2P_SetRelayControlOptions Options = {};
		Options.ApiVersion = EOS_P2P_SETRELAYCONTROL_API_LATEST;
		Options.RelayControl = Control == EWoTRelayControl::ForceRelays
			? EOS_ERelayControl::EOS_RC_ForceRelays
			: EOS_ERelayControl::EOS_RC_AllowRelays;
		// EOS applies this only to new P2P connections (and reconnects after a
		// temporary outage); it deliberately does not reroute an existing match.
		const EOS_EResult Result = EOS_P2P_SetRelayControl(P2P, &Options);
		if (Result != EOS_EResult::EOS_Success
			&& Result != EOS_EResult::EOS_NoChange)
		{
			return false;
		}
		EffectiveRelayControl = Control;
		return true;
	}

	virtual FString KeychainAccountId() const override
	{
		FWoTEOSArtifact Artifact;
		return SelectArtifact(Artifact) ? Artifact.ProductId : FString();
	}

	virtual FString Diagnostics() const override
	{
		return FString::Printf(TEXT("artifact=%s nat=%s relay=%s sdk=%s"),
			SelectedArtifact.IsEmpty() ? TEXT("none") : *SelectedArtifact,
			*NatType, LexToString(EffectiveRelayControl),
			IEOSSDKManager::Get() && IEOSSDKManager::Get()->IsInitialized()
				? TEXT("initialized") : TEXT("absent"));
	}

private:
	bool SelectArtifact(FWoTEOSArtifact& Out) const
	{
		const UEOSSettings* Settings = GetDefault<UEOSSettings>();
		if (Settings == nullptr)
		{
			return false;
		}
		const FArtifactSettings* Test =
			WoTFindArtifact(*Settings, FWoTEOSConfig::TestArtifactName());
		const bool bTestPresent = Test != nullptr
			&& FWoTEOSConfig::ValidateArtifact(WoTToArtifact(*Test)).IsEmpty();
		const FString Name = FWoTEOSConfig::PreferredArtifactName(
			UE_BUILD_SHIPPING != 0, bTestPresent);
		const FArtifactSettings* Chosen = WoTFindArtifact(*Settings, Name);
		if (Chosen == nullptr)
		{
			return false;
		}
		Out = WoTToArtifact(*Chosen);
		return true;
	}

#if !UE_BUILD_SHIPPING && !WOT_GUARDED_BOOTSTRAP
	/**
	 * Unpackaged development runs only (editor / -game; compiled out of every
	 * guarded Test and Shipping build, which use the staged config): read the
	 * same local file Tools/dist/eos_credentials.py injects at packaging time -
	 * %WOT_EOS_CREDENTIALS%, else %USERPROFILE%/.wot-secrets/eos-credentials.json
	 * - and add its Test deployment as the "WoTTest" artifact. Values are
	 * never logged; a missing or malformed file leaves internet play disabled.
	 */
	void ApplyLocalCredentialFile()
	{
		if (GIsAutomationTesting)
		{
			// Specs run with whatever credentials the repository config has
			// (none): the operator's secrets file is never read by a test.
			return;
		}
		FString Path = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_EOS_CREDENTIALS"));
		if (Path.IsEmpty())
		{
			FString Profile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
			if (Profile.IsEmpty())
			{
				Profile = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
			}
			if (Profile.IsEmpty())
			{
				return;
			}
			Path = FPaths::Combine(Profile, TEXT(".wot-secrets"), TEXT("eos-credentials.json"));
		}
		FString Text;
		if (IFileManager::Get().FileSize(*Path) <= 0
			|| IFileManager::Get().FileSize(*Path) > 16 * 1024
			|| !FFileHelper::LoadFileToString(Text, *Path))
		{
			return;
		}
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object)
			|| !Object.IsValid() || Object->Values.Num() != 6)
		{
			return;
		}
		FArtifactSettings Settings;
		Settings.ArtifactName = FWoTEOSConfig::TestArtifactName();
		if (!Object->TryGetStringField(TEXT("ProductId"), Settings.ProductId)
			|| !Object->TryGetStringField(TEXT("SandboxId"), Settings.SandboxId)
			|| !Object->TryGetStringField(TEXT("DeploymentIdTest"), Settings.DeploymentId)
			|| !Object->HasTypedField<EJson::String>(TEXT("DeploymentIdPublic"))
			|| !Object->TryGetStringField(TEXT("ClientId"), Settings.ClientId)
			|| !Object->TryGetStringField(TEXT("ClientSecret"), Settings.ClientSecret)
			|| !FWoTEOSConfig::ValidateArtifact(WoTToArtifact(Settings)).IsEmpty())
		{
			return;
		}
		UEOSSettings* Mutable = GetMutableDefault<UEOSSettings>();
		Mutable->Artifacts.RemoveAll([](const FArtifactSettings& Existing)
		{
			return Existing.ArtifactName == FWoTEOSConfig::TestArtifactName();
		});
		Mutable->Artifacts.Add(Settings);
	}
#endif

	EOS_HPlatform PlatformHandle() const
	{
		if (Subsystem == nullptr || Subsystem->GetSubsystemName() != EOS_SUBSYSTEM)
		{
			return nullptr;
		}
		const IEOSPlatformHandlePtr Handle =
			static_cast<IOnlineSubsystemEOS*>(Subsystem)->GetEOSPlatformHandle();
		return Handle.IsValid() ? static_cast<EOS_HPlatform>(*Handle) : nullptr;
	}

	void ClearLoginDelegate()
	{
		if (Identity.IsValid() && LoginCompleteHandle.IsValid())
		{
			Identity->ClearOnLoginCompleteDelegate_Handle(0, LoginCompleteHandle);
		}
		LoginCompleteHandle.Reset();
	}

	void HandleLoginComplete(int32 /*LocalUserNum*/, bool bWasSuccessful,
		const FUniqueNetId& /*UserId*/, const FString& Error)
	{
		ClearLoginDelegate();
		if (!PendingLogin)
		{
			return;
		}
		FDone Done = MoveTemp(PendingLogin);
		PendingLogin = nullptr;
		if (bWasSuccessful)
		{
			QueryNatType();
			RegisterPeerConnectionNotifications();
		}
		Done(bWasSuccessful, bWasSuccessful ? FString() : Error.Left(200));
	}

	void HandleLoginStatusChanged(int32 /*LocalUserNum*/,
		ELoginStatus::Type OldStatus, ELoginStatus::Type NewStatus,
		const FUniqueNetId& /*UserId*/)
	{
		if (OldStatus == ELoginStatus::LoggedIn
			&& NewStatus != ELoginStatus::LoggedIn)
		{
			ClearPeerConnectionNotifications();
			if (SignInLost)
			{
				SignInLost(TEXT("signed-out"));
			}
		}
	}

	void HandleLogoutComplete(int32 /*LocalUserNum*/, bool bWasSuccessful)
	{
		if (Identity.IsValid() && LogoutCompleteHandle.IsValid())
		{
			Identity->ClearOnLogoutCompleteDelegate_Handle(0, LogoutCompleteHandle);
		}
		LogoutCompleteHandle.Reset();
		if (!PendingReset)
		{
			return;
		}
		if (!bWasSuccessful || IsLoggedIn())
		{
			// Never report a reset while the old product user is still signed
			// in: deleting the credential under a live login would leave the
			// game on an identity the player asked to discard.
			FDone Done = MoveTemp(PendingReset);
			PendingReset = nullptr;
			Done(false, TEXT("logout-failed"));
			return;
		}
		DeleteDeviceId();
	}

	void DeleteDeviceId()
	{
		const EOS_HPlatform Platform = PlatformHandle();
		if (Platform == nullptr)
		{
			FDone Done = MoveTemp(PendingReset);
			PendingReset = nullptr;
			if (Done)
			{
				Done(false, TEXT("no-platform"));
			}
			return;
		}
		EOS_Connect_DeleteDeviceIdOptions Options = {};
		Options.ApiVersion = EOS_CONNECT_DELETEDEVICEID_API_LATEST;
		using FCallback = TEOSCallback<EOS_Connect_OnDeleteDeviceIdCallback,
			EOS_Connect_DeleteDeviceIdCallbackInfo, FWoTEOSInternetBackend>;
		FCallback* Callback = new FCallback(AsWeak());
		Callback->CallbackLambda =
			[this](const EOS_Connect_DeleteDeviceIdCallbackInfo* Data)
			{
				FDone Done = MoveTemp(PendingReset);
				PendingReset = nullptr;
				NatType = TEXT("unknown");
				// No credential to delete is also a clean slate.
				const bool bOk = Data->ResultCode == EOS_EResult::EOS_Success
					|| Data->ResultCode == EOS_EResult::EOS_NotFound;
				if (Done)
				{
					Done(bOk, LexToString(Data->ResultCode));
				}
			};
		EOS_Connect_DeleteDeviceId(EOS_Platform_GetConnectInterface(Platform),
			&Options, Callback, Callback->GetCallbackPtr());
	}

	using FPeerEstablishedCallback = TEOSGlobalCallback<
		EOS_P2P_OnPeerConnectionEstablishedCallback,
		EOS_P2P_OnPeerConnectionEstablishedInfo, FWoTEOSInternetBackend>;
	using FPeerClosedCallback = TEOSGlobalCallback<
		EOS_P2P_OnRemoteConnectionClosedCallback,
		EOS_P2P_OnRemoteConnectionClosedInfo, FWoTEOSInternetBackend>;

	void ClearPeerConnectionNotifications()
	{
		const EOS_HPlatform Platform = PlatformHandle();
		EOS_HP2P P2P = Platform != nullptr
			? EOS_Platform_GetP2PInterface(Platform) : nullptr;
		if (P2P != nullptr)
		{
			if (PeerEstablishedNotifyId != EOS_INVALID_NOTIFICATIONID)
			{
				EOS_P2P_RemoveNotifyPeerConnectionEstablished(
					P2P, PeerEstablishedNotifyId);
			}
			if (PeerClosedNotifyId != EOS_INVALID_NOTIFICATIONID)
			{
				EOS_P2P_RemoveNotifyPeerConnectionClosed(P2P, PeerClosedNotifyId);
			}
		}
		PeerEstablishedNotifyId = EOS_INVALID_NOTIFICATIONID;
		PeerClosedNotifyId = EOS_INVALID_NOTIFICATIONID;
		delete PeerEstablishedCallback;
		PeerEstablishedCallback = nullptr;
		delete PeerClosedCallback;
		PeerClosedCallback = nullptr;
		PeerConnectionTypes.Reset();
	}

	void RegisterPeerConnectionNotifications()
	{
		if (!Telemetry || !IsLoggedIn())
		{
			return;
		}
		const EOS_HPlatform Platform = PlatformHandle();
		EOS_HP2P P2P = Platform != nullptr
			? EOS_Platform_GetP2PInterface(Platform) : nullptr;
		EOS_HConnect Connect = Platform != nullptr
			? EOS_Platform_GetConnectInterface(Platform) : nullptr;
		if (P2P == nullptr || Connect == nullptr
			|| EOS_Connect_GetLoggedInUsersCount(Connect) <= 0)
		{
			return;
		}
		const EOS_ProductUserId LocalUserId =
			EOS_Connect_GetLoggedInUserByIndex(Connect, 0);
		if (LocalUserId == nullptr)
		{
			return;
		}

		if (PeerEstablishedNotifyId == EOS_INVALID_NOTIFICATIONID)
		{
			EOS_P2P_AddNotifyPeerConnectionEstablishedOptions Options = {};
			Options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
			Options.LocalUserId = LocalUserId;
			Options.SocketId = nullptr;
			PeerEstablishedCallback = new FPeerEstablishedCallback(AsWeak());
			PeerEstablishedCallback->CallbackLambda =
				[this](const EOS_P2P_OnPeerConnectionEstablishedInfo* Data)
				{
					const FString RawRemoteId =
						WoTProductUserIdString(Data->RemoteUserId);
					const FString ConnectionType =
						WoTNetworkTypeName(Data->NetworkType);
					PeerConnectionTypes.Add(
						WoTPeerConnectionKey(Data->RemoteUserId, Data->SocketId),
						ConnectionType);
					if (Telemetry)
					{
						Telemetry(TEXT("eos-peer-established"), FString::Printf(
							TEXT("connection=%s peer=%s kind=%s"),
							*ConnectionType,
							*FWoTEOSConfig::RedactId(RawRemoteId),
							WoTEstablishedKindName(Data->ConnectionType)));
					}
				};
			PeerEstablishedNotifyId =
				EOS_P2P_AddNotifyPeerConnectionEstablished(P2P, &Options,
					PeerEstablishedCallback,
					PeerEstablishedCallback->GetCallbackPtr());
			if (PeerEstablishedNotifyId == EOS_INVALID_NOTIFICATIONID)
			{
				delete PeerEstablishedCallback;
				PeerEstablishedCallback = nullptr;
			}
		}

		if (PeerClosedNotifyId == EOS_INVALID_NOTIFICATIONID)
		{
			EOS_P2P_AddNotifyPeerConnectionClosedOptions Options = {};
			Options.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
			Options.LocalUserId = LocalUserId;
			Options.SocketId = nullptr;
			PeerClosedCallback = new FPeerClosedCallback(AsWeak());
			PeerClosedCallback->CallbackLambda =
				[this](const EOS_P2P_OnRemoteConnectionClosedInfo* Data)
				{
					const FString RawRemoteId =
						WoTProductUserIdString(Data->RemoteUserId);
					const FString Key =
						WoTPeerConnectionKey(Data->RemoteUserId, Data->SocketId);
					FString ConnectionType = PeerConnectionTypes.FindRef(Key);
					if (ConnectionType.IsEmpty())
					{
						ConnectionType = TEXT("unknown");
					}
					PeerConnectionTypes.Remove(Key);
					if (Telemetry)
					{
						Telemetry(TEXT("eos-peer-closed"), FString::Printf(
							TEXT("connection=%s peer=%s reason=%s"),
							*ConnectionType,
							*FWoTEOSConfig::RedactId(RawRemoteId),
							WoTClosedReasonName(Data->Reason)));
					}
				};
			PeerClosedNotifyId = EOS_P2P_AddNotifyPeerConnectionClosed(
				P2P, &Options, PeerClosedCallback,
				PeerClosedCallback->GetCallbackPtr());
			if (PeerClosedNotifyId == EOS_INVALID_NOTIFICATIONID)
			{
				delete PeerClosedCallback;
				PeerClosedCallback = nullptr;
			}
		}
	}

	void HandleNetworkStatusChanged(EOS_ENetworkStatus OldStatus,
		EOS_ENetworkStatus NewStatus)
	{
		if (OldStatus == EOS_ENetworkStatus::EOS_NS_Online
			&& NewStatus != EOS_ENetworkStatus::EOS_NS_Online)
		{
			// EOS has already unbound every P2P notification. Mirror the engine's
			// SocketEOS cleanup; the next online edge installs fresh registrations.
			PeerEstablishedNotifyId = EOS_INVALID_NOTIFICATIONID;
			PeerClosedNotifyId = EOS_INVALID_NOTIFICATIONID;
			PeerEstablishedCallback = nullptr;
			PeerClosedCallback = nullptr;
			PeerConnectionTypes.Reset();
		}
		else if (OldStatus != EOS_ENetworkStatus::EOS_NS_Online
			&& NewStatus == EOS_ENetworkStatus::EOS_NS_Online)
		{
			RegisterPeerConnectionNotifications();
		}
	}

	void QueryNatType()
	{
		const EOS_HPlatform Platform = PlatformHandle();
		if (Platform == nullptr)
		{
			return;
		}
		EOS_P2P_QueryNATTypeOptions Options = {};
		Options.ApiVersion = EOS_P2P_QUERYNATTYPE_API_LATEST;
		using FCallback = TEOSCallback<EOS_P2P_OnQueryNATTypeCompleteCallback,
			EOS_P2P_OnQueryNATTypeCompleteInfo, FWoTEOSInternetBackend>;
		FCallback* Callback = new FCallback(AsWeak());
		Callback->CallbackLambda =
			[this](const EOS_P2P_OnQueryNATTypeCompleteInfo* Data)
			{
				NatType = Data->ResultCode == EOS_EResult::EOS_Success
					? FString(WoTNatTypeName(Data->NATType))
					: FString::Printf(TEXT("error-%s"), *LexToString(Data->ResultCode));
				if (Telemetry)
				{
					Telemetry(TEXT("eos-nat"),
						FString::Printf(TEXT("nat=%s"), *NatType));
				}
			};
		EOS_P2P_QueryNATType(EOS_Platform_GetP2PInterface(Platform), &Options,
			Callback, Callback->GetCallbackPtr());
	}

	IOnlineSubsystem* Subsystem = nullptr;
	IOnlineIdentityPtr Identity;
	FDelegateHandle LoginCompleteHandle;
	FDelegateHandle LoginStatusHandle;
	FDelegateHandle LogoutCompleteHandle;
	FDelegateHandle NetworkStatusHandle;
	FDone PendingLogin;
	FDone PendingReset;
	TFunction<void(const FString&)> SignInLost;
	FTelemetry Telemetry;
	FPeerEstablishedCallback* PeerEstablishedCallback = nullptr;
	FPeerClosedCallback* PeerClosedCallback = nullptr;
	EOS_NotificationId PeerEstablishedNotifyId = EOS_INVALID_NOTIFICATIONID;
	EOS_NotificationId PeerClosedNotifyId = EOS_INVALID_NOTIFICATIONID;
	TMap<FString, FString> PeerConnectionTypes;
	FString SelectedArtifact;
	FString NatType = TEXT("unknown");
	EWoTRelayControl EffectiveRelayControl = EWoTRelayControl::AllowRelays;
	bool bInitFailed = false;
};
#endif // WOT_WITH_EOS

#if !WOT_WITH_EOS
namespace
{
	/** WOT_WITH_EOS=0 (Linux server, Program targets): always unavailable. */
	class FWoTUnavailableInternetBackend final : public IWoTInternetBackend
	{
	public:
		virtual FString QuickUnavailability() const override
		{
			return TEXT("not-compiled");
		}
		virtual FString EnsureStarted(UWorld*) override
		{
			return TEXT("not-compiled");
		}
		virtual IOnlineSessionPtr GetSessionInterface() const override
		{
			return nullptr;
		}
		virtual bool IsLoggedIn() const override { return false; }
		virtual FString RedactedUserId() const override { return TEXT("none"); }
		virtual void CreateDeviceId(FDone Done) override
		{
			Done(false, TEXT("not-compiled"));
		}
		virtual void Login(const FString&, FDone Done) override
		{
			Done(false, TEXT("not-compiled"));
		}
		virtual void ResetIdentity(FDone Done) override
		{
			Done(false, TEXT("not-compiled"));
		}
		virtual void SetSignInLostHandler(TFunction<void(const FString&)>) override {}
		virtual void SetTelemetryHandler(FTelemetry) override {}
		virtual bool SetRelayControl(EWoTRelayControl) override { return false; }
		virtual FString Diagnostics() const override
		{
			return TEXT("artifact=none nat=unknown relay=none sdk=not-compiled");
		}
	};
}
#endif // !WOT_WITH_EOS

TSharedRef<IWoTInternetBackend> MakeWoTEOSInternetBackend()
{
#if WOT_WITH_EOS
	return MakeShared<FWoTEOSInternetBackend>();
#else
	return MakeShared<FWoTUnavailableInternetBackend>();
#endif
}
