#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"

class UWorld;

/**
 * Internet play (Epic Online Services, OSS v1 "EOS", Connect Device ID).
 *
 * LAN and direct IP never consult anything here: they stay on the Null OSS and
 * the IpNetDriver whether EOS is compiled, configured, reachable or not.
 */
enum class EWoTInternetState : uint8
{
	/** Not compiled / -NoEOS / not configured / SDK missing / init failed. */
	Unavailable,
	/** Usable, not signed in. The next internet action signs in. */
	Idle,
	/** EOS_Connect_CreateDeviceId in flight (EOS_DuplicateNotAllowed = ok). */
	CreatingDeviceId,
	/** IOnlineIdentity::Login("...:DeviceIdAccessToken") in flight. */
	LoggingIn,
	/** Signed in: lobbies can be created/searched, P2P can listen/connect. */
	Ready,
	/** The last sign-in failed; the next internet action retries. */
	Failed,
	/** Logout + EOS_Connect_DeleteDeviceId in flight. */
	Resetting
};

const TCHAR* LexToString(EWoTInternetState State);

/** The two player-facing EOS P2P policies. */
enum class EWoTRelayControl : uint8
{
	AllowRelays,
	ForceRelays
};

const TCHAR* LexToString(EWoTRelayControl Control);

/** One EOS "artifact": a Developer Portal client + product/sandbox/deployment. */
struct FWoTEOSArtifact
{
	FString ArtifactName;
	FString ProductId;
	FString SandboxId;
	FString DeploymentId;
	FString ClientId;
	FString ClientSecret;
};

/** Pure policy helpers (no config, SDK, or OSS access): spec-covered. */
struct FWoTEOSConfig
{
	/** Public Shipping deployment. */
	static const TCHAR* ShippingArtifactName() { return TEXT("WoT"); }
	/** Test/dev deployment, kept out of the public lobby list. */
	static const TCHAR* TestArtifactName() { return TEXT("WoTTest"); }

	/**
	 * Shipping uses only the public artifact. Every other configuration prefers
	 * the test artifact and falls back to the public one when it is absent.
	 */
	static FString PreferredArtifactName(bool bShippingBuild,
		bool bTestArtifactPresent);

	/** Empty or an obvious template value ("REPLACE_WITH_...", "<...>"). */
	static bool IsPlaceholder(const FString& Value);

	/**
	 * Empty when every field is a plausible credential. Otherwise the reason
	 * token "not-configured". Values that could break the +Artifacts config
	 * line (quotes, parentheses, commas, whitespace) are rejected too.
	 */
	static FString ValidateArtifact(const FWoTEOSArtifact& Artifact);

	/**
	 * Guarded Shipping treats the command line as untrusted. OnlineSubsystemEOS
	 * honours artifact / sandbox / deployment / auth / build-id overrides even in
	 * Shipping (EOSSettings.cpp GetSelectedArtifactSettings, UserManagerEOS.cpp
	 * AutoLogin, OnlineSubsystem.cpp GetBuildUniqueId): remove every
	 * whitespace-delimited token that names one. -NoEOS (disable only) is kept.
	 */
	static FString StripShippingCommandLineOverrides(FStringView CommandLine);

	/** Never logs a whole id: "..." + the last six characters. */
	static FString RedactId(const FString& Id);

	/** Player-facing sentence for a reason token (unavailable / failure). */
	static FString DescribeReason(const FString& Reason);
};

/**
 * Seam over everything EOS so the session state machine is spec-driven
 * without a network or credentials. All callbacks complete exactly once,
 * possibly synchronously, on the game thread.
 */
class IWoTInternetBackend
{
public:
	virtual ~IWoTInternetBackend() = default;

	/**
	 * Cheap and side-effect free: compiled in, not disabled, credentials
	 * configured, SDK initialized, no earlier init failure. Empty = usable.
	 * Never creates the EOS platform (UI polls this).
	 */
	virtual FString QuickUnavailability() const = 0;

	/**
	 * Creates the EOS OSS instance (and with it the EOS platform) on first
	 * use. Called only from authorized internet actions - never at startup or
	 * before the bootstrap gate. Empty = started.
	 */
	virtual FString EnsureStarted(UWorld* World) = 0;

	virtual IOnlineSessionPtr GetSessionInterface() const = 0;
	virtual bool IsLoggedIn() const = 0;
	virtual FString RedactedUserId() const = 0;

	using FDone = TFunction<void(bool /*bOk*/, const FString& /*Detail*/)>;
	virtual void CreateDeviceId(FDone Done) = 0;
	virtual void Login(const FString& DisplayName, FDone Done) = 0;
	/** Logout (when signed in) + delete the local Device ID credential. */
	virtual void ResetIdentity(FDone Done) = 0;

	/** An established sign-in was lost (expiry not refreshed, logout). */
	virtual void SetSignInLostHandler(TFunction<void(const FString&)> Handler) = 0;

	/**
	 * Permanent field telemetry emitted by the EOS backend. Event names and
	 * details are already bounded and privacy-safe (redacted ids, no IPs).
	 */
	using FTelemetry = TFunction<void(const TCHAR* /*Event*/, const FString& /*Detail*/)>;
	virtual void SetTelemetryHandler(FTelemetry Handler) = 0;

	/** Apply the relay policy to new P2P connections. */
	virtual bool SetRelayControl(EWoTRelayControl Control) = 0;

	/** artifact, NAT type, effective relay policy: appended to the session readout. */
	virtual FString Diagnostics() const = 0;

	/**
	 * The account the SDK files its Device ID credential under in the macOS
	 * keychain (the selected artifact's ProductId); empty when unknown.
	 */
	virtual FString KeychainAccountId() const { return FString(); }
};

/** The production backend (reports "not-compiled" when WOT_WITH_EOS=0). */
TSharedRef<IWoTInternetBackend> MakeWoTEOSInternetBackend();
