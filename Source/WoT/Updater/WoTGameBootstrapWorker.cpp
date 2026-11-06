#include "WoTGameBootstrapWorker.h"

#include "Containers/Queue.h"
#include "HAL/Event.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Templates/Atomic.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Ed25519.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/KeyRing.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTBootstrapCore/ReleaseManifest.h"
#include "WoTBootstrapCore/TestOverrides.h"
#include "WoTUserDataRoot.h"
#include "WoTRetailReader/Package.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTUpdaterCore/AnnouncementPolicy.h"
#include "WoTUpdaterCore/ReleaseStaging.h"
#include "WoTUpdaterCore/UpdateFailureNotice.h"
#include "WoTUpdaterCore/UpdateState.h"
#include "../WoTBootstrapHeartbeat.h"
#include "WoTUpdaterCore/UpdaterPaths.h"
#include "WoTUpdaterCore/UpdaterPlatform.h"

#include "../Payload/WoTCacheStore.h"
#include "../UI/WoTReleaseNotes.h"
#include "WoTCacheBootstrapOperation.h"
#include "WoTGameUpdateTransports.h"
#include "WoTInstallerLocations.h"
#include "WoTRememberedInstaller.h"
#include "WoTUpdateTransports/WoTPlatformHttpHop.h"

#include <cstdlib>

namespace
{
constexpr double WoTGameBootstrapWorkerUnboundedDurationSeconds = 1.0e9;

FCriticalSection GWoTResolvedUserStateRootLock;
FString GWoTResolvedUserStateRootPath;
FWoTNativeFileIdentity GWoTResolvedUserStateRootIdentity;
bool GWoTHasResolvedUserStateRoot = false;

class FWoTGameBootstrapCacheProgress final : public IWoTProgressSink
{
public:
	FWoTGameBootstrapCacheProgress(FWoTBootstrapProgressCallback& InProgress,
		uint64 InEpoch, const TAtomic<bool>& InCancelled,
		double InPhaseStartedSeconds, const TFunction<double()>& InNowSeconds)
		: Progress(InProgress)
		, Epoch(InEpoch)
		, Cancelled(InCancelled)
		, PhaseStartedSeconds(InPhaseStartedSeconds)
		, NowSeconds(InNowSeconds)
	{
	}

	virtual FWoTBootstrapStatus ReportProgress(FStringView Stage,
		uint64 Completed, uint64 Total) override
	{
		if (Progress && !Cancelled.Load())
		{
			FWoTBootstrapProgressEvent Event;
			Event.OperationEpoch = Epoch;
			Event.StageId = FString(Stage);
			Event.CompletedUnits = Completed;
			Event.TotalUnits = Total;
			Event.ElapsedSeconds = FMath::Max(0.0,
				NowSeconds() - PhaseStartedSeconds);
			Progress(MoveTemp(Event));
		}
		return FWoTBootstrapStatus::Success();
	}

	virtual bool IsCancellationRequested() const override
	{
		return Cancelled.Load();
	}

private:
	FWoTBootstrapProgressCallback& Progress;
	uint64 Epoch = 0;
	const TAtomic<bool>& Cancelled;
	double PhaseStartedSeconds = 0.0;
	const TFunction<double()>& NowSeconds;
};

FWoTBootstrapStatus WoTGameBootstrapWorkerError(
	EWoTBootstrapError Code, const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(Code, Message));
}

const TCHAR* WoTGameBootstrapWorkerUpdateStatusName(EWoTUpdateStatus Status)
{
	switch (Status)
	{
	case EWoTUpdateStatus::None: return TEXT("None");
	case EWoTUpdateStatus::Checking: return TEXT("Checking");
	case EWoTUpdateStatus::Downloading: return TEXT("Downloading");
	case EWoTUpdateStatus::Ready: return TEXT("Ready");
	case EWoTUpdateStatus::Available: return TEXT("Available");
	case EWoTUpdateStatus::KnownBad: return TEXT("KnownBad");
	default: return TEXT("Unknown");
	}
}

FString WoTGameBootstrapWorkerReleasePageUrl(FStringView ReleaseAssetUrl)
{
	const FString Url(ReleaseAssetUrl);
	const FString Marker = TEXT("/releases/download/");
	const int32 MarkerAt = Url.Find(Marker, ESearchCase::CaseSensitive);
	if (MarkerAt == INDEX_NONE)
	{
		return FString();
	}
	const int32 TagAt = MarkerAt + Marker.Len();
	const int32 AssetAt = Url.Find(
		TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TagAt);
	if (AssetAt == INDEX_NONE || AssetAt == TagAt)
	{
		return FString();
	}
	return Url.Left(MarkerAt) + TEXT("/releases/tag/")
		+ Url.Mid(TagAt, AssetAt - TagAt);
}

EWoTBootstrapDiagnosticCode WoTGameBootstrapWorkerDiagnostic(
	const FWoTBootstrapError& Error)
{
	switch (Error.Code)
	{
	case EWoTBootstrapError::Crypto:
		return EWoTBootstrapDiagnosticCode::SignatureInvalid;
	case EWoTBootstrapError::UnsupportedProtocol:
	case EWoTBootstrapError::UnknownField:
	case EWoTBootstrapError::InvalidNumber:
	case EWoTBootstrapError::InvalidUtf8:
	case EWoTBootstrapError::NonCanonicalJson:
	case EWoTBootstrapError::DuplicateField:
	case EWoTBootstrapError::SizeLimit:
		return EWoTBootstrapDiagnosticCode::ConfigInvalid;
	default:
		return EWoTBootstrapDiagnosticCode::BootstrapFailure;
	}
}

TWoTBootstrapResult<FWoTEd25519Signature>
WoTGameBootstrapWorkerSignature(
	TConstArrayView<uint8> Bytes, const TCHAR* What)
{
	using FResult = TWoTBootstrapResult<FWoTEd25519Signature>;
	if (Bytes.Num() != FWoTEd25519Signature::ByteCount)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Crypto,
			FString::Printf(TEXT("%s signature has the wrong size"), What)));
	}
	FWoTEd25519Signature Signature;
	FMemory::Memcpy(Signature.Bytes, Bytes.GetData(),
		FWoTEd25519Signature::ByteCount);
	return FResult::Success(Signature);
}

TWoTBootstrapResult<FWoTKeyRing> WoTGameBootstrapWorkerAnnouncementRing(
	FStringView PublicKeyHex)
{
	using FResult = TWoTBootstrapResult<FWoTKeyRing>;
	TWoTBootstrapResult<FWoTEd25519PublicKey> PublicKey =
		FWoTEd25519PublicKey::FromHex(PublicKeyHex);
	if (PublicKey.IsError())
	{
		return FResult::Error(MoveTemp(PublicKey).GetError());
	}
	FWoTKeyRing Ring;
	Ring.Format = WoTReleaseKeyRingFormat;
	Ring.Epoch = 1;
	FWoTReleaseKey Key;
	Key.KeyId = TEXT("compiled-announcement-key");
	Key.PublicKey = MoveTemp(PublicKey).GetValue();
	Key.FirstSequence = 1;
	Key.LastSequence = MAX_uint64;
	Ring.Keys.Add(MoveTemp(Key));
	return FResult::Success(MoveTemp(Ring));
}

bool WoTGameBootstrapWorkerVerifyWithKey(
	const FWoTEd25519PublicKey& Key, EWoTSignatureContext Context,
	TConstArrayView<uint8> Bytes, const FWoTEd25519Signature& Signature,
	TOptional<FWoTBootstrapError>& OutError)
{
	TWoTBootstrapResult<bool> Verified =
		VerifyEd25519(Key, Context, Bytes, Signature);
	if (Verified.IsError())
	{
		OutError = MoveTemp(Verified).GetError();
		return false;
	}
	return MoveTemp(Verified).GetValue();
}

struct FWoTGameBootstrapWorkerVerifiedMetadata
{
	FWoTKeyRing ReleaseRing;
	FWoTReleaseManifestV2 Manifest;
};

TWoTBootstrapResult<FWoTGameBootstrapWorkerVerifiedMetadata>
WoTGameBootstrapWorkerVerifyMetadata(
	const FWoTAnnouncementV1& Announcement,
	const FWoTReleaseStagingMetadata& Metadata,
	uint64 MinimumValidatedRingEpoch)
{
	using FResult =
		TWoTBootstrapResult<FWoTGameBootstrapWorkerVerifiedMetadata>;
	if (static_cast<uint64>(Metadata.KeyRingBytes.Num())
			!= Announcement.KeyRing.Size
		|| Sha256(Metadata.KeyRingBytes) != Announcement.KeyRing.Sha256
		|| static_cast<uint64>(Metadata.ReleaseManifestBytes.Num())
			!= Announcement.Manifest.Size
		|| Sha256(Metadata.ReleaseManifestBytes)
			!= Announcement.Manifest.Sha256)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Crypto,
			TEXT("prefetched metadata disagrees with the authenticated announcement")));
	}

	TWoTBootstrapResult<FWoTEd25519Signature> RingSignature =
		WoTGameBootstrapWorkerSignature(
			Metadata.KeyRingSignature, TEXT("release key-ring"));
	if (RingSignature.IsError())
	{
		return FResult::Error(MoveTemp(RingSignature).GetError());
	}
	const FWoTEd25519Signature AcceptedRingSignature =
		MoveTemp(RingSignature).GetValue();
	TWoTBootstrapResult<FWoTEd25519Signature> ManifestSignature =
		WoTGameBootstrapWorkerSignature(
			Metadata.ReleaseManifestSignature, TEXT("release manifest"));
	if (ManifestSignature.IsError())
	{
		return FResult::Error(MoveTemp(ManifestSignature).GetError());
	}
	const FWoTEd25519Signature AcceptedManifestSignature =
		MoveTemp(ManifestSignature).GetValue();

	TWoTBootstrapResult<FWoTCanonicalJsonValue> RingDocument =
		ParseCanonicalJson(Metadata.KeyRingBytes);
	if (RingDocument.IsError())
	{
		return FResult::Error(MoveTemp(RingDocument).GetError());
	}
	FWoTCanonicalJsonValue RingJson =
		MoveTemp(RingDocument).GetValue();
	TWoTBootstrapResult<FWoTKeyRing> ReleaseRing =
		ParseKeyRing(RingJson, WoTReleaseKeyRingFormat);
	if (ReleaseRing.IsError())
	{
		return FResult::Error(MoveTemp(ReleaseRing).GetError());
	}
	FWoTKeyRing AcceptedReleaseRing =
		MoveTemp(ReleaseRing).GetValue();
	if (AcceptedReleaseRing.Epoch != Announcement.KeyRingEpoch)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Rollback,
			TEXT("the release key-ring epoch disagrees with the announcement")));
	}
	if (FWoTBootstrapStatus Epoch = RequireRingEpochAtLeast(
			AcceptedReleaseRing, MinimumValidatedRingEpoch);
		Epoch.IsError())
	{
		return FResult::Error(MoveTemp(Epoch).GetError());
	}

	const TConstArrayView<uint8> RootBytes(
		WoT::Bootstrap::Trust::OfflineRootKeyRingCanonicalBytes,
		WoT::Bootstrap::Trust::OfflineRootKeyRingSize);
	TWoTBootstrapResult<FWoTCanonicalJsonValue> RootDocument =
		ParseCanonicalJson(RootBytes);
	if (RootDocument.IsError())
	{
		return FResult::Error(MoveTemp(RootDocument).GetError());
	}
	FWoTCanonicalJsonValue RootJson =
		MoveTemp(RootDocument).GetValue();
	TWoTBootstrapResult<FWoTKeyRing> RootRing =
		ParseKeyRing(RootJson, WoTOfflineRootRingFormat);
	if (RootRing.IsError())
	{
		return FResult::Error(MoveTemp(RootRing).GetError());
	}
	FWoTKeyRing AcceptedRootRing =
		MoveTemp(RootRing).GetValue();

	bool bRingAuthenticated = false;
	TOptional<FWoTBootstrapError> VerificationError;
	for (const FWoTReleaseKey& RootKey : AcceptedRootRing.Keys)
	{
		bRingAuthenticated |= WoTGameBootstrapWorkerVerifyWithKey(
			RootKey.PublicKey, EWoTSignatureContext::ReleaseKeyRingV1,
			Metadata.KeyRingBytes, AcceptedRingSignature,
			VerificationError);
	}
#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
	if (!bRingAuthenticated && !VerificationError.IsSet()
		&& WoTShouldAcceptTestUpdateIdentities())
	{
		// Public half of the deterministic 0x91 seed used by the public-API
		// synthetic v1/v2 updater fixture.
		TWoTBootstrapResult<FWoTEd25519PublicKey> TestRoot =
			FWoTEd25519PublicKey::FromHex(
				WoTTestReleaseKeyRingRootHex());
		if (!TestRoot.IsError())
		{
			const FWoTEd25519PublicKey TestRootKey =
				MoveTemp(TestRoot).GetValue();
			bRingAuthenticated = WoTGameBootstrapWorkerVerifyWithKey(
				TestRootKey, EWoTSignatureContext::ReleaseKeyRingV1,
				Metadata.KeyRingBytes, AcceptedRingSignature,
				VerificationError);
		}
	}
#endif
	if (VerificationError.IsSet())
	{
		return FResult::Error(MoveTemp(VerificationError.GetValue()));
	}
	if (!bRingAuthenticated)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Crypto,
			TEXT("the release key ring is not signed by an embedded root")));
	}

	TWoTBootstrapResult<FWoTReleaseManifestV2> Manifest =
		VerifyReleaseManifestV2(Metadata.ReleaseManifestBytes,
			AcceptedManifestSignature, Announcement,
			AcceptedReleaseRing);
	if (Manifest.IsError())
	{
		return FResult::Error(MoveTemp(Manifest).GetError());
	}
	FWoTReleaseManifestV2 AcceptedManifest =
		MoveTemp(Manifest).GetValue();
	if (!SupportsManifestProtocols(AcceptedManifest))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the release manifest requires unsupported protocols")));
	}

	FWoTGameBootstrapWorkerVerifiedMetadata Verified;
	Verified.ReleaseRing = MoveTemp(AcceptedReleaseRing);
	Verified.Manifest = MoveTemp(AcceptedManifest);
	return FResult::Success(MoveTemp(Verified));
}

bool WoTGameBootstrapWorkerAddBudget(uint64 Value, uint64& Total)
{
	if (Value > MAX_uint64 - Total)
	{
		return false;
	}
	Total += Value;
	return true;
}

FString WoTGameBootstrapWorkerDefaultUserStateRoot()
{
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	// Guarded packaged-Test harness only: give each process its own complete
	// updater/cache root. Shipping does not compile this override.
	FString HarnessRoot;
	if (FParse::Value(FCommandLine::Get(), TEXT("wottestinstallroot="),
		HarnessRoot))
	{
		HarnessRoot = FPaths::ConvertRelativePathToFull(HarnessRoot);
		FPaths::NormalizeDirectoryName(HarnessRoot);
		if (FPaths::IsRelative(HarnessRoot)
			|| !FPaths::CollapseRelativeDirectories(HarnessRoot))
		{
			return FString();
		}
		return HarnessRoot;
	}
#endif

#if WOT_GUARDED_BOOTSTRAP
	return WoTUserDataRoot();
#else
	// Non-guarded editor/development oracle only. Guarded Test and Shipping
	// builds resolve the single WoT Reborn root above and never enter here.
	const FString Base = FPlatformProcess::UserSettingsDir();
	if (Base.IsEmpty())
	{
		return FString();
	}
	FString Root = FPaths::Combine(Base, TEXT("WoT"));
	Root = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(Root);
	if (!FPaths::CollapseRelativeDirectories(Root)
		|| FPaths::IsRelative(Root))
	{
		return FString();
	}
	return Root;
#endif
}

using FWoTGameBootstrapNativeTreePathQuery =
	TFunction<TWoTBootstrapResult<FString>(
		const FWoTNativeDirectoryHandle&)>;

#if WITH_AUTOMATION_TESTS
TAtomic<int32> GWoTExecutableLayoutResolutionCount{0};
TAtomic<int32> GWoTAdjacentParentOpenCount{0};
#endif

bool WoTGameBootstrapWorkerIsSupportedAbsolutePath(const FString& Path)
{
#if PLATFORM_WINDOWS
	const bool bDrivePath = Path.Len() >= 3 && FChar::IsAlpha(Path[0])
		&& Path[1] == TEXT(':') && Path[2] == TEXT('/');
	if (bDrivePath)
	{
		return true;
	}
	if (!Path.StartsWith(TEXT("//"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	const int32 ServerEnd = Path.Find(
		TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 2);
	const int32 ShareEnd = ServerEnd == INDEX_NONE ? INDEX_NONE
		: Path.Find(TEXT("/"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, ServerEnd + 1);
	return ServerEnd > 2 && Path.Len() > ServerEnd + 1
		&& (ShareEnd == INDEX_NONE || ShareEnd > ServerEnd + 1);
#else
	return Path.StartsWith(TEXT("/"), ESearchCase::CaseSensitive);
#endif
}

TWoTBootstrapResult<FString> WoTGameBootstrapWorkerCanonicalOwnedRoot(
	FString Root, const TCHAR* Label,
	FWoTNativeFileIdentity* OutIdentity = nullptr)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (Root.IsEmpty())
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("the %s is unavailable"), Label), Root));
	}
	Root = FPaths::ConvertRelativePathToFull(MoveTemp(Root));
	Root.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	FPaths::NormalizeDirectoryName(Root);
	if (Root.IsEmpty() || FPaths::IsRelative(Root)
		|| !FPaths::CollapseRelativeDirectories(Root))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("the %s is not an absolute safe path"), Label),
			Root));
	}
	bool bCreated = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Opened =
		OpenOrCreateCanonicalNativeDirectoryNoFollow(Root, bCreated);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	(void)bCreated;
	if (OutIdentity != nullptr)
	{
		*OutIdentity = Opened.GetValue().Directory.GetIdentity();
	}
	return FResult::Success(MoveTemp(Opened).GetValue().CanonicalPath);
}
// FPaths::NormalizeDirectoryName strips the slash from the POSIX root ("/"
// becomes ""), though it keeps "D:/". A volume-root install needs "/" intact.
static void WoTGameBootstrapWorkerNormalizeRootAware(FString& Path)
{
	const bool bPosixRoot = Path == TEXT("/");
	FPaths::NormalizeDirectoryName(Path);
	if (bPosixRoot)
	{
		Path = TEXT("/");
	}
}


void WoTGameBootstrapWorkerResolveUpdateLayout(
	FWoTResolvedExecutableLayout& Layout,
	const FWoTGameBootstrapNativeTreePathQuery& NativeTreePathQuery,
	bool bFailCanonicalIdentityForTesting)
{
#if !WITH_AUTOMATION_TESTS
	(void)bFailCanonicalIdentityForTesting;
#endif
	TWoTBootstrapResult<FString> Queried =
		TWoTBootstrapResult<FString>::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io,
			TEXT("the live-tree canonical query did not run")));
	if (NativeTreePathQuery)
	{
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Live =
			OpenNativeDirectoryNoFollow(Layout.InstalledTreeRoot);
		if (!Live.IsError())
		{
			Queried = NativeTreePathQuery(Live.GetValue());
		}
	}
	else
	{
#if WITH_AUTOMATION_TESTS
		TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Live =
			bFailCanonicalIdentityForTesting
				? TWoTBootstrapResult<FWoTCanonicalNativeDirectory>::Error(
					MakeWoTBootstrapError(
						EWoTBootstrapError::CorruptInstallation,
						TEXT("injected canonical directory identity mismatch")))
				: OpenCanonicalNativeDirectoryNoFollow(
					Layout.InstalledTreeRoot);
#else
		TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Live =
			OpenCanonicalNativeDirectoryNoFollow(Layout.InstalledTreeRoot);
#endif
		if (Live.IsError())
		{
			if (Live.GetError().Code
				== EWoTBootstrapError::CorruptInstallation)
			{
				Layout.UpdateDisableReason = TEXT("live-tree-identity-mismatch");
				return;
			}
		}
		else
		{
			Queried = TWoTBootstrapResult<FString>::Success(
				MoveTemp(Live).GetValue().CanonicalPath);
		}
	}
	if (Queried.IsError())
	{
		Layout.UpdateDisableReason = TEXT("live-tree-name-unresolved");
		return;
	}

	FString NativeTreeRoot = MoveTemp(Queried).GetValue();
	NativeTreeRoot.ReplaceInline(TEXT("\\"), TEXT("/"),
		ESearchCase::CaseSensitive);
	FPaths::NormalizeDirectoryName(NativeTreeRoot);
	if (!WoTGameBootstrapWorkerIsSupportedAbsolutePath(NativeTreeRoot)
		|| !FPaths::CollapseRelativeDirectories(NativeTreeRoot))
	{
		Layout.UpdateDisableReason = TEXT("live-tree-name-unresolved");
		return;
	}

	FString LiveLeaf = FPaths::GetCleanFilename(NativeTreeRoot);
	if (LiveLeaf.IsEmpty() || LiveLeaf.Contains(TEXT("/"))
		|| LiveLeaf.Contains(TEXT("\\"))
		|| RequireSafeRelativePath(LiveLeaf).IsError())
	{
		Layout.UpdateDisableReason = TEXT("live-tree-name-unresolved");
		return;
	}
	if (LiveLeaf.Equals(TEXT(".wot-update"), ESearchCase::IgnoreCase))
	{
		Layout.UpdateDisableReason = TEXT("reserved-live-leaf");
		return;
	}
#if PLATFORM_MAC
	if (!LiveLeaf.EndsWith(TEXT(".app"), ESearchCase::CaseSensitive))
	{
		Layout.UpdateDisableReason = TEXT("live-tree-name-unresolved");
		return;
	}
#endif

	FString InstallRoot = FPaths::GetPath(NativeTreeRoot);
#if PLATFORM_MAC
	if (InstallRoot.IsEmpty()
		&& NativeTreeRoot.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		InstallRoot = TEXT("/");
	}
#endif
#if PLATFORM_WINDOWS
	if (InstallRoot.Len() == 2 && InstallRoot[1] == TEXT(':'))
	{
		InstallRoot += TEXT("/");
	}
#endif
	WoTGameBootstrapWorkerNormalizeRootAware(InstallRoot);
	if (!WoTGameBootstrapWorkerIsSupportedAbsolutePath(InstallRoot)
		|| !FPaths::CollapseRelativeDirectories(InstallRoot))
	{
		Layout.UpdateDisableReason = TEXT("install-root-unsupported");
		return;
	}

	Layout.InstallRoot = MoveTemp(InstallRoot);
	Layout.LiveLeaf = MoveTemp(LiveLeaf);
	Layout.InstalledTreeRoot = MoveTemp(NativeTreeRoot);
	Layout.CanonicalAdjacentInstallerPath = FPaths::Combine(
		Layout.InstallRoot, WoT::Bootstrap::InstallerName);
	FPaths::NormalizeFilename(Layout.CanonicalAdjacentInstallerPath);
}

FWoTResolvedExecutableLayout
WoTGameBootstrapWorkerResolveExecutableLayout(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot,
	FWoTGameBootstrapNativeTreePathQuery NativeTreePathQuery = {},
	bool bFailCanonicalIdentityForTesting = false,
	bool bSkipInstallRootIdentityRebindForTesting = false,
	TFunction<bool(const FString&)> RunningTreeTranslocatedForTesting = {})
{
#if WITH_AUTOMATION_TESTS
	++GWoTExecutableLayoutResolutionCount;
#endif
	FWoTResolvedExecutableLayout Layout;
	FString Path = FPaths::ConvertRelativePathToFull(FString(ExecutablePath));
	FPaths::NormalizeFilename(Path);
	if (!FPaths::CollapseRelativeDirectories(Path)
		|| !WoTGameBootstrapWorkerIsSupportedAbsolutePath(Path))
	{
		return Layout;
	}
	TWoTBootstrapResult<FString> ResolvedUserState =
		WoTGameBootstrapWorkerCanonicalOwnedRoot(
			FString(DefaultUserStateRoot), TEXT("user state root"),
			&Layout.UserStateRootIdentity);
	if (ResolvedUserState.IsError())
	{
		return Layout;
	}
	Layout.UserStateRoot = MoveTemp(ResolvedUserState).GetValue();
	Layout.bHasUserStateRootIdentity = true;
	Layout.ContentCacheRoot = FPaths::Combine(
		Layout.UserStateRoot, TEXT("ContentCache"));
	Layout.ContentCacheRoot = FPaths::ConvertRelativePathToFull(
		Layout.ContentCacheRoot);
	FPaths::NormalizeDirectoryName(Layout.ContentCacheRoot);
	if (!FPaths::CollapseRelativeDirectories(Layout.ContentCacheRoot)
		|| FPaths::IsRelative(Layout.ContentCacheRoot)
		|| FPaths::GetPath(Layout.ContentCacheRoot) != Layout.UserStateRoot)
	{
		return FWoTResolvedExecutableLayout{};
	}

#if PLATFORM_MAC
	// Bundle helpers may live in nested app bundles. The outermost enclosing app
	// is the one canonical live tree swapped as a unit.
	FString Directory = FPaths::GetPath(Path);
	while (!Directory.IsEmpty())
	{
		const FString Leaf = FPaths::GetCleanFilename(Directory);
		if (Leaf.Len() > 4
			&& Leaf.EndsWith(TEXT(".app"), ESearchCase::CaseSensitive))
		{
			Layout.InstalledTreeRoot = Directory;
		}
		const FString Parent = FPaths::GetPath(Directory);
		if (Parent.IsEmpty() || Parent == Directory)
		{
			break;
		}
		Directory = Parent;
	}
	if (Layout.InstalledTreeRoot.IsEmpty())
		return FWoTResolvedExecutableLayout{};
	Layout.ExecutableTreeRelativePath = Path.RightChop(
		Layout.InstalledTreeRoot.Len() + 1);
#elif PLATFORM_WINDOWS
	// The guarded Win64 layout is
	// <InstallRoot>/<safe-live-leaf>/WoT/Binaries/Win64/<exe>.
	const FString Leaf = FPaths::GetCleanFilename(Path);
	const FString BinDirectory = FPaths::GetPath(Path);
	const FString BinariesDirectory = FPaths::GetPath(BinDirectory);
	const FString ProjectDirectory = FPaths::GetPath(BinariesDirectory);
	const FString TreeRoot = FPaths::GetPath(ProjectDirectory);
	if (!FPaths::GetCleanFilename(BinDirectory).Equals(
			TEXT("Win64"), ESearchCase::IgnoreCase)
		|| !FPaths::GetCleanFilename(BinariesDirectory).Equals(
			TEXT("Binaries"), ESearchCase::IgnoreCase)
		|| !FPaths::GetCleanFilename(ProjectDirectory).Equals(
			TEXT("WoT"), ESearchCase::IgnoreCase))
	{
		return FWoTResolvedExecutableLayout{};
	}
	Layout.InstalledTreeRoot = TreeRoot;
	Layout.ExecutableTreeRelativePath = TEXT("WoT/Binaries/Win64/") + Leaf;
#else
#error "installed-tree resolution is defined only for Win64 and Mac"
	#endif
	FPaths::NormalizeDirectoryName(Layout.InstalledTreeRoot);
	FPaths::NormalizeFilename(Layout.ExecutableTreeRelativePath);
	if (Layout.InstalledTreeRoot.IsEmpty()
		|| Layout.ExecutableTreeRelativePath.IsEmpty()
		|| !WoTGameBootstrapWorkerIsSupportedAbsolutePath(
			Layout.InstalledTreeRoot)
		|| !FPaths::CollapseRelativeDirectories(Layout.InstalledTreeRoot)
		|| RequireSafeRelativePath(
			Layout.ExecutableTreeRelativePath).IsError())
	{
		return FWoTResolvedExecutableLayout{};
	}
	const bool bRunningTreeTranslocated = RunningTreeTranslocatedForTesting
		? RunningTreeTranslocatedForTesting(Layout.InstalledTreeRoot)
		: WoTIsRunningTreeTranslocated(Layout.InstalledTreeRoot);
	if (bRunningTreeTranslocated)
	{
		// The translocated tree is not adjacent to the installed copy. Do not open,
		// canonicalize, or retain authority over its ephemeral parent.
		Layout.UpdateDisableReason = TEXT("translocated");
		return Layout;
	}
	FString LaunchInstallRoot = FPaths::GetPath(Layout.InstalledTreeRoot);
#if PLATFORM_MAC
	if (LaunchInstallRoot.IsEmpty()
		&& Layout.InstalledTreeRoot.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		LaunchInstallRoot = TEXT("/");
	}
#endif
#if PLATFORM_WINDOWS
	if (LaunchInstallRoot.Len() == 2 && LaunchInstallRoot[1] == TEXT(':'))
		LaunchInstallRoot += TEXT("/");
#endif
	LaunchInstallRoot = FPaths::ConvertRelativePathToFull(LaunchInstallRoot);
	WoTGameBootstrapWorkerNormalizeRootAware(LaunchInstallRoot);
	if (!WoTGameBootstrapWorkerIsSupportedAbsolutePath(LaunchInstallRoot)
		|| !FPaths::CollapseRelativeDirectories(LaunchInstallRoot))
		return FWoTResolvedExecutableLayout{};
	#if WITH_AUTOMATION_TESTS
	++GWoTAdjacentParentOpenCount;
	#endif
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> CanonicalInstallRoot =
		OpenCanonicalNativeDirectoryNoFollow(LaunchInstallRoot);
	if (CanonicalInstallRoot.IsError())
	{
		if (CanonicalInstallRoot.GetError().Code
			== EWoTBootstrapError::UnsupportedProtocol)
		{
			// The loader-reported tree/member still defines the guarded game
			// layout. Only the optional updater needs a DOS/UNC final name.
			Layout.UpdateDisableReason = TEXT("live-tree-name-unresolved");
			return Layout;
		}
		return FWoTResolvedExecutableLayout{};
	}
	LaunchInstallRoot = CanonicalInstallRoot.GetValue().CanonicalPath;
	const FWoTNativeFileIdentity LaunchInstallRootIdentity =
		CanonicalInstallRoot.GetValue().Directory.GetIdentity();
	Layout.AdjacentInstallerParentIdentity = LaunchInstallRootIdentity;
	Layout.bHasAdjacentInstallerParentIdentity = true;
	Layout.CanonicalAdjacentInstallerPath = FPaths::Combine(
		LaunchInstallRoot, WoT::Bootstrap::InstallerName);
	FPaths::NormalizeFilename(Layout.CanonicalAdjacentInstallerPath);
	if (!WoTGameBootstrapWorkerIsSupportedAbsolutePath(
			Layout.CanonicalAdjacentInstallerPath)
		|| !FPaths::CollapseRelativeDirectories(
			Layout.CanonicalAdjacentInstallerPath))
	{
		return FWoTResolvedExecutableLayout{};
	}
	WoTGameBootstrapWorkerResolveUpdateLayout(
		Layout, NativeTreePathQuery, bFailCanonicalIdentityForTesting);
	Layout.bHasInstallRootIdentity = false;
	if (Layout.UpdateDisableReason.IsEmpty() && !Layout.InstallRoot.IsEmpty())
	{
#if WITH_AUTOMATION_TESTS
		if (bSkipInstallRootIdentityRebindForTesting)
		{
			// Pure path-projection specs may inject a non-local UNC spelling.
			// Production and identity specs always reopen the final root below.
			Layout.InstallRootIdentity = LaunchInstallRootIdentity;
			Layout.bHasInstallRootIdentity = true;
			return Layout;
		}
#else
		(void)bSkipInstallRootIdentityRebindForTesting;
#endif
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> FinalInstallRoot =
			OpenNativeDirectoryNoFollow(Layout.InstallRoot);
		if (FinalInstallRoot.IsError())
		{
			Layout.UpdateDisableReason = TEXT("install-root-identity-unresolved");
			return Layout;
		}
		Layout.InstallRootIdentity =
			FinalInstallRoot.GetValue().GetIdentity();
		Layout.bHasInstallRootIdentity = true;
	}
	return Layout;
}

FString WoTGameBootstrapWorkerResolveProductionContentCacheRoot(
	FStringView ExecutablePath,
	FStringView DefaultUserStateRoot)
{
	const FWoTResolvedExecutableLayout Layout =
		WoTGameBootstrapWorkerResolveExecutableLayout(
			ExecutablePath, DefaultUserStateRoot);
	if (!Layout.IsValid())
	{
		return FString();
	}
	return Layout.ContentCacheRoot;
}

class FWoTGameBootstrapFreeSpaceProbe final : public IWoTFreeSpaceProbe
{
public:
	explicit FWoTGameBootstrapFreeSpaceProbe(
		FWoTNativeDirectoryHandle&& InStateDirectory)
		: StateDirectory(MoveTemp(InStateDirectory))
	{
	}

	virtual bool QueryAvailableBytes(
		EWoTBootstrapPhase, uint64& OutAvailableBytes) override
	{
		TWoTBootstrapResult<uint64> Available =
			QueryNativeDirectoryAvailableBytes(StateDirectory);
		if (Available.IsError())
		{
			return false;
		}
		OutAvailableBytes = Available.GetValue();
		return true;
	}

private:
	FWoTNativeDirectoryHandle StateDirectory;
};
} // namespace

struct FWoTGameBootstrapWorker::FImpl final : public FRunnable
{
	struct FWork
	{
		enum class EKind : uint8
		{
			Phase,
			ReleaseNotes,
			BackgroundUpdate,
		};

		EKind Kind = EKind::Phase;
		FWoTBootstrapWorkerRequest Request;
		FWoTBootstrapProgressCallback Progress;
		FWoTBootstrapCompletionCallback Completion;
		double PhaseStartedSeconds = 0.0;
		bool bReleaseNotesSeen = false;
		FWoTReleaseNotesCallback ReleaseNotes;
		FWoTUpdateStateCallback UpdateState;
	};

	struct FTrackedInstallerLocation
	{
		FWoTInstallerLocation Location;
		TOptional<FWoTNativeFileIdentity> PreviousCandidateIdentity;
		TOptional<FWoTNativeFileIdentity> KnownWrongIdentity;
	};

	struct FWoTInstallerSweep
	{
		TUniquePtr<FWoTVerifiedInstaller> Verified;
		EWoTOriginalGameNeededReason Reason =
			EWoTOriginalGameNeededReason::Missing;
		FString Detail;
		EWoTInstallerSource FoundSource = EWoTInstallerSource::Adjacent;
		/** The held installer's canonical path when Verified is set. */
		FString FoundCanonicalPath;
		bool bNeedsStabilityPass = false;
	};

	class FCancellableHops final : public IWoTHttpHopExecutor
	{
	public:
		FCancellableHops(
			IWoTHttpHopExecutor& InInner, TAtomic<bool>& InCancelled)
			: Inner(InInner), bCancelled(InCancelled)
		{
		}

		virtual FWoTBootstrapStatus ExecuteHop(
			const FWoTHttpHopRequest& Request,
			FWoTHttpHopResponse& OutResponse) override
		{
			if (bCancelled.Load())
			{
				OutResponse = FWoTHttpHopResponse();
				return WoTGameBootstrapWorkerError(
					EWoTBootstrapError::ServiceUnavailable,
					TEXT("the bootstrap worker was cancelled"));
			}
			class FCancellableBodySink final : public IWoTHttpBodySink
			{
			public:
				FCancellableBodySink(IWoTHttpBodySink& InInner,
					TAtomic<bool>& InCancelled)
					: InnerSink(InInner), Cancelled(InCancelled)
				{
				}

				virtual FWoTBootstrapStatus OnResponseStart(
					const FWoTHttpHopResponse& HeadersOnly) override
				{
					return Cancelled.Load()
						? WoTGameBootstrapWorkerError(
							EWoTBootstrapError::ServiceUnavailable,
							TEXT("the bootstrap worker was cancelled"))
						: InnerSink.OnResponseStart(HeadersOnly);
				}

				virtual bool ShouldStreamResponseBody(
					int32 StatusCode) const override
				{
					return InnerSink.ShouldStreamResponseBody(StatusCode);
				}

				virtual FWoTBootstrapStatus Write(
					TConstArrayView<uint8> Bytes) override
				{
					if (Cancelled.Load())
					{
						return WoTGameBootstrapWorkerError(
							EWoTBootstrapError::ServiceUnavailable,
							TEXT("the bootstrap worker was cancelled"));
					}
					FWoTBootstrapStatus Written = InnerSink.Write(Bytes);
					if (Written.IsError()) return Written;
					// A successful inner write accepted this entire chunk. Let the
					// platform count it; native abort or the next callback observes
					// cancellation without understating the durable resume prefix.
					return FWoTBootstrapStatus::Success();
				}

				virtual bool ShouldPreserveResponseOnError() const override
				{
					return Cancelled.Load();
				}

			private:
				IWoTHttpBodySink& InnerSink;
				TAtomic<bool>& Cancelled;
			};

			TOptional<FCancellableBodySink> CancellableSink;
			FWoTHttpHopRequest CancellableRequest = Request;
			if (Request.BodySink != nullptr)
			{
				CancellableSink.Emplace(*Request.BodySink, bCancelled);
				CancellableRequest.BodySink = &CancellableSink.GetValue();
			}
			FWoTBootstrapStatus Status =
				Inner.ExecuteHop(CancellableRequest, OutResponse);
			if (bCancelled.Load())
			{
				// Preserve terminal headers and any body prefix already delivered to
				// the sink. ReleaseStaging durably records that prefix before it
				// returns the cancellation error.
				return WoTGameBootstrapWorkerError(
					EWoTBootstrapError::ServiceUnavailable,
					TEXT("the bootstrap worker was cancelled"));
			}
			return Status;
		}

		virtual void CancelCurrentRequest() override
		{
			Inner.CancelCurrentRequest();
		}

	private:
		IWoTHttpHopExecutor& Inner;
		TAtomic<bool>& bCancelled;
	};

	class FRecordingAnnouncementTransport final
		: public IWoTAnnouncementTransport
	{
	public:
		explicit FRecordingAnnouncementTransport(
			FWoTGameAnnouncementTransport& InInner)
			: Inner(InInner)
		{
		}

		virtual FWoTBootstrapStatus FetchPair(
			TArray<uint8>& OutAnnouncement,
			TArray<uint8>& OutSignature) override
		{
			if (bFetched)
			{
				OutAnnouncement = Announcement;
				OutSignature = Signature;
				return FWoTBootstrapStatus::Success();
			}
			FWoTBootstrapStatus Status =
				Inner.FetchPair(OutAnnouncement, OutSignature);
			if (!Status.IsError())
			{
				Announcement = OutAnnouncement;
				Signature = OutSignature;
				bFetched = true;
			}
			return Status;
		}

		const TArray<uint8>& AnnouncementBytes() const
		{
			return Announcement;
		}
		const TArray<uint8>& SignatureBytes() const
		{
			return Signature;
		}

	private:
		FWoTGameAnnouncementTransport& Inner;
		TArray<uint8> Announcement;
		TArray<uint8> Signature;
		bool bFetched = false;
	};

	explicit FImpl(FDependencies&& InDeps)
		: Deps(MoveTemp(InDeps))
		, WorkEvent(FPlatformProcess::GetSynchEventFromPool(false))
		, OriginalGameRecheckEvent(
			FPlatformProcess::GetSynchEventFromPool(false))
	{
		if (Deps.DownloadsDirectory.IsEmpty())
		{
			Deps.DownloadsDirectory = WoTResolveDownloadsDirectory();
		}
		AdjacentInstaller.Location.Source = EWoTInstallerSource::Adjacent;
		AdjacentInstaller.Location.CanonicalPath =
			Deps.CanonicalAdjacentInstallerPath;
		AdjacentInstaller.Location.bRequirePinnedName = true;
		if (Deps.bHasAdjacentInstallerParentIdentity)
		{
			AdjacentInstaller.Location.ParentIdentity =
				Deps.AdjacentInstallerParentIdentity;
		}
		for (FString& Candidate :
			WoTDownloadsInstallerCandidates(Deps.DownloadsDirectory))
		{
			FTrackedInstallerLocation& Tracked =
				DownloadsInstallers.AddDefaulted_GetRef();
			Tracked.Location.Source = EWoTInstallerSource::Downloads;
			Tracked.Location.CanonicalPath = MoveTemp(Candidate);
			Tracked.Location.bRequirePinnedName = false;
		}
		if (!Deps.NowSeconds)
		{
			Deps.NowSeconds = []()
			{
				return FPlatformTime::Seconds();
			};
		}
		if (!Deps.RequestRestartExit)
		{
			Deps.RequestRestartExit = []()
			{
				FPlatformMisc::RequestExit(false);
			};
		}
		if (Deps.DownloadOperationDeadlineSeconds == 0.0)
		{
			Deps.DownloadOperationDeadlineSeconds =
				WoTGameBootstrapWorkerUnboundedDurationSeconds;
		}
		if (Deps.Hops.IsValid())
		{
			CancellableHops =
				MakeUnique<FCancellableHops>(*Deps.Hops, bCancelled);
		}
	}

	virtual ~FImpl() override
	{
		CancelAndJoin();
		if (WorkEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(WorkEvent);
			WorkEvent = nullptr;
		}
		if (OriginalGameRecheckEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(
				OriginalGameRecheckEvent);
			OriginalGameRecheckEvent = nullptr;
		}
	}

	void Start(FWork&& Work)
	{
		FScopeLock Lock(&ThreadLock);
		if (bCancelled.Load())
		{
			return;
		}
		if (Thread == nullptr)
		{
			Thread = FRunnableThread::Create(
				this, TEXT("WoTGameBootstrapWorker"));
			if (Thread == nullptr)
			{
				return;
			}
		}
		WorkQueue.Enqueue(MakeUnique<FWork>(MoveTemp(Work)));
		WorkEvent->Trigger();
	}

	virtual uint32 Run() override
	{
		while (!bCancelled.Load())
		{
			WorkEvent->Wait();
			if (bCancelled.Load())
			{
				break;
			}
			TUniquePtr<FWork> Work;
			while (!bCancelled.Load() && WorkQueue.Dequeue(Work))
			{
				Work->PhaseStartedSeconds = Deps.NowSeconds();
				if (Work->Kind == FWork::EKind::ReleaseNotes)
				{
					ExecuteReleaseNotes(*Work);
					Work.Reset();
					continue;
				}
				if (Work->Kind == FWork::EKind::BackgroundUpdate)
				{
					FRunnableThread* Current = FRunnableThread::GetRunnableThread();
					if (Current != nullptr)
					{
						Current->SetThreadPriority(TPri_BelowNormal);
					}
					ExecuteBackgroundUpdate(*Work);
					if (Current != nullptr)
					{
						Current->SetThreadPriority(TPri_Normal);
					}
					Work.Reset();
					continue;
				}
				FWoTBootstrapWorkerCompletion Result = Execute(*Work);
				{
					// The delivery gate: CancelAndJoin flips bCancelled under
					// this same lock, so no completion can START after the
					// cancel store is visible, and an in-flight delivery
					// finishes before the store proceeds.
					FScopeLock DeliveryScope(&DeliveryLock);
					if (!bCancelled.Load() && Work->Completion)
					{
						Work->Completion(MoveTemp(Result));
					}
				}
				Work.Reset();
			}
		}
		ResetCacheActivationState();
		return 0;
	}

	void CancelAndJoin()
	{
		// Serialize concurrent teardown: a second caller must not return (or
		// drain the queue) while the first is still joining the run thread.
		FScopeLock CancelScope(&CancelLock);
		bAcceptPickedInstaller.Store(false);
		{
			// Paired with the run loop's delivery gate (see Run): after this
			// store no completion or restart-exit can begin.
			FScopeLock DeliveryScope(&DeliveryLock);
			bCancelled.Store(true);
		}
		if (CancellableHops != nullptr)
		{
			CancellableHops->CancelCurrentRequest();
		}
		if (WorkEvent != nullptr)
		{
			WorkEvent->Trigger();
		}
		if (OriginalGameRecheckEvent != nullptr)
		{
			OriginalGameRecheckEvent->Trigger();
		}
		FRunnableThread* ThreadToJoin = nullptr;
		{
			FScopeLock Lock(&ThreadLock);
			ThreadToJoin = Thread;
		}
		if (ThreadToJoin != nullptr)
		{
			ThreadToJoin->WaitForCompletion();
			FScopeLock Lock(&ThreadLock);
			if (Thread == ThreadToJoin)
			{
				delete ThreadToJoin;
				Thread = nullptr;
			}
		}
		ResetCacheActivationState();
		ResetContentOperationState();
		TUniquePtr<FWork> Discarded;
		while (WorkQueue.Dequeue(Discarded))
		{
			Discarded.Reset();
		}
	}

	FWoTBootstrapWorkerCompletion Completion(
		const FWork& Work, EWoTBootstrapPhase NextPhase) const
	{
		FWoTBootstrapWorkerCompletion Result;
		Result.OperationEpoch = Work.Request.OperationEpoch;
		Result.CompletedPhase = Work.Request.Phase;
		Result.NextPhase = NextPhase;
		Result.bSucceeded = true;
		return Result;
	}

	FWoTBootstrapWorkerCompletion CompleteGameUpdateCheck(
		const FWork& Work, bool bAnnouncementUnavailable = false,
		const FWoTAnnouncementV1* NewerAnnouncement = nullptr) const
	{
		FWoTBootstrapWorkerCompletion Result = Completion(
			Work, EWoTBootstrapPhase::CheckingOriginalGameData);
		Result.bAnnouncementUnavailable = bAnnouncementUnavailable;
		if (NotifyOnlyReason.IsEmpty())
		{
			return Result;
		}
		FString Detail = TEXT("notify-only reason=") + NotifyOnlyReason;
		if (!NotifyOnlyDetail.IsEmpty())
		{
			Detail += TEXT(" detail=") + NotifyOnlyDetail;
		}
		WoTRecordBootstrapHeartbeat(TEXT("update-mode"),
			EWoTBootstrapPhase::CheckingGameUpdates,
			Work.Request.OperationEpoch, {}, {},
			Detail);
		Result.bUpdateNotifyOnly = true;
		Result.NotifyOnlyReason = NotifyOnlyReason;
		if (NewerAnnouncement != nullptr)
		{
			Result.NotifyOnlyReleaseSequence =
				NewerAnnouncement->ReleaseSequence;
			Result.NotifyOnlyReleasePageUrl =
				WoTGameBootstrapWorkerReleasePageUrl(
					NewerAnnouncement->Manifest.Url);
		}
		return Result;
	}

	FWoTBootstrapWorkerCompletion Failure(
		const FWork& Work, EWoTBootstrapDiagnosticCode Diagnostic) const
	{
		FWoTBootstrapWorkerCompletion Result;
		Result.OperationEpoch = Work.Request.OperationEpoch;
		Result.CompletedPhase = Work.Request.Phase;
		Result.NextPhase = EWoTBootstrapPhase::Fatal;
		Result.DiagnosticCode = Diagnostic;
		return Result;
	}

	FWoTBootstrapWorkerCompletion Failure(
		const FWork& Work, const FWoTBootstrapError& Error) const
	{
		FWoTBootstrapWorkerCompletion Result =
			Failure(Work, WoTGameBootstrapWorkerDiagnostic(Error));
		Result.FailureDetail = Error.ToString();
		return Result;
	}

	FWoTBootstrapWorkerCompletion CacheFailure(
		const FWork& Work, const FWoTBootstrapError& Error) const
	{
		FWoTBootstrapWorkerCompletion Result = Failure(Work, Error);
		Result.FailureDetail += FString::Printf(TEXT(" native=%u"), Error.NativeCode);
		FString Message;
#if PLATFORM_WINDOWS
		const bool bContended = Error.NativeCode == 33 || Error.NativeCode == 32;
#else
		const bool bContended = Error.NativeCode == EWOULDBLOCK || Error.NativeCode == EAGAIN;
#endif
		if (Error.Code == EWoTBootstrapError::ServiceUnavailable && bContended)
		{
			Message = TEXT("Close other copies of WoT Reborn and relaunch.");
		}
		else
		{
#if PLATFORM_WINDOWS
			const bool bDiskFull = Error.NativeCode == 112 || Error.NativeCode == 39;
			const bool bAccessDenied = Error.NativeCode == 5;
#else
			const bool bDiskFull = Error.NativeCode == ENOSPC || Error.NativeCode == EDQUOT;
			const bool bAccessDenied = Error.NativeCode == EACCES || Error.NativeCode == EPERM;
#endif
			if (bDiskFull)
			{
				Message = TEXT("There is not enough disk space for the content cache. Free space on this drive and relaunch WoT Reborn.");
			}
			else if (bAccessDenied)
			{
				Message = TEXT("WoT Reborn cannot access its content cache. Check that you have permission to read and write this folder, then relaunch.");
			}
			else
			{
				Message = TEXT("WoT Reborn could not prepare its content cache. Check that this drive is available and the folder is writable, then relaunch.");
			}
		}
		Result.FailureMessage = Message + TEXT("\nContent cache: ")
			+ FPaths::ConvertRelativePathToFull(Deps.ContentCacheRoot);
		return Result;
	}

	FWoTBootstrapWorkerCompletion InvalidInstallerFailure(
		const FWork& Work) const
	{
		return Failure(Work, EWoTBootstrapDiagnosticCode::InstallerInvalid);
	}

	TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
		InspectInstallerLocation(FWoTInstallerLocation& Location,
			const FWoTHeldInstallerParent* Parent = nullptr) const
	{
#if WITH_AUTOMATION_TESTS
		if (Deps.InstallerLocationFaultForTesting)
		{
			TOptional<FWoTBootstrapError> Fault =
				Deps.InstallerLocationFaultForTesting(
					Location.Source, Location.CanonicalPath);
			if (Fault.IsSet())
			{
				return TWoTBootstrapResult<FWoTAdjacentInstallerInspection>::Error(
					MoveTemp(Fault.GetValue()));
			}
		}
		if (Deps.InstallerPinsForTesting.IsSet())
		{
			return Parent != nullptr
				? WoTInspectInstallerLocationAtParentForTesting(
					Location, *Parent,
					Deps.InstallerPinsForTesting.GetValue())
				: WoTInspectInstallerLocationForTesting(
					Location, Deps.InstallerPinsForTesting.GetValue());
		}
#endif
		return Parent != nullptr
			? WoTInspectInstallerLocationAtParent(Location, *Parent)
			: WoTInspectInstallerLocation(Location);
	}

	TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
		AcquireInstallerLocation(FWoTInstallerLocation& Location,
			const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr,
			const FWoTHeldInstallerParent* Parent = nullptr) const
	{
		TFunction<bool()> IsCancelled = [this]()
		{
			return bCancelled.Load();
		};
#if WITH_AUTOMATION_TESTS
		if (Deps.InstallerPinsForTesting.IsSet())
		{
			return Parent != nullptr
				? WoTTryAcquireInstallerLocationAtParentForTesting(
					Location, *Parent,
					Deps.InstallerPinsForTesting.GetValue(),
					MoveTemp(IsCancelled), Deps.InstallerHooksForTesting,
					ExpectedInstallerIdentity)
				: WoTTryAcquireInstallerLocationForTesting(
					Location,
					Deps.InstallerPinsForTesting.GetValue(),
					MoveTemp(IsCancelled), Deps.InstallerHooksForTesting,
					ExpectedInstallerIdentity);
		}
#endif
		return Parent != nullptr
			? WoTTryAcquireInstallerLocationAtParent(Location, *Parent,
				MoveTemp(IsCancelled), ExpectedInstallerIdentity)
			: WoTTryAcquireInstallerLocation(
				Location, MoveTemp(IsCancelled), ExpectedInstallerIdentity);
	}

	TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
		AcquireAdjacentInstaller(
			const FWoTNativeFileIdentity* ExpectedInstallerIdentity = nullptr)
	{
		return AcquireInstallerLocation(
			AdjacentInstaller.Location, ExpectedInstallerIdentity);
	}

	bool RunningTreeTranslocated() const
	{
#if WITH_AUTOMATION_TESTS
		if (Deps.RunningTreeTranslocatedForTesting)
		{
			return Deps.RunningTreeTranslocatedForTesting(
				Deps.InstalledTreeRoot);
		}
#endif
		return WoTIsRunningTreeTranslocated(Deps.InstalledTreeRoot);
	}

	void RequestOriginalGameRecheck()
	{
		if (OriginalGameRecheckEvent != nullptr)
		{
			OriginalGameRecheckEvent->Trigger();
		}
	}

	void RequestPickedInstaller(FString CanonicalPath)
	{
		if (!bAcceptPickedInstaller.Load() || CanonicalPath.IsEmpty())
		{
			return;
		}
		PickedInstallerRequests.Enqueue(MoveTemp(CanonicalPath));
		RequestOriginalGameRecheck();
	}

	static const TCHAR* InstallerSourceName(EWoTInstallerSource Source)
	{
		switch (Source)
		{
		case EWoTInstallerSource::Downloads:
			return TEXT("downloads");
		case EWoTInstallerSource::Picked:
			return TEXT("picked");
		case EWoTInstallerSource::Remembered:
			return TEXT("remembered");
		case EWoTInstallerSource::Adjacent:
		default:
			return TEXT("adjacent");
		}
	}

	static int32 InstallerReasonPriority(EWoTOriginalGameNeededReason Reason)
	{
		switch (Reason)
		{
		case EWoTOriginalGameNeededReason::OnlineOnly:
			return 5;
		case EWoTOriginalGameNeededReason::UnsupportedReparsePoint:
			return 4;
		case EWoTOriginalGameNeededReason::PickedWrongFile:
			return 3;
		case EWoTOriginalGameNeededReason::Downloading:
			return 2;
		case EWoTOriginalGameNeededReason::NotTheInstaller:
			return 1;
		case EWoTOriginalGameNeededReason::Missing:
		default:
			return 0;
		}
	}

	static void MergeInstallerReason(FWoTInstallerSweep& Sweep,
		EWoTOriginalGameNeededReason Reason, FString Detail = FString())
	{
		if (InstallerReasonPriority(Reason)
			> InstallerReasonPriority(Sweep.Reason))
		{
			Sweep.Reason = Reason;
			Sweep.Detail = MoveTemp(Detail);
		}
	}

	void AdoptLatestPickedInstallerRequest()
	{
		FString RequestedPath;
		FString LatestPath;
		while (PickedInstallerRequests.Dequeue(RequestedPath))
		{
			LatestPath = MoveTemp(RequestedPath);
		}
		if (LatestPath.IsEmpty())
		{
			return;
		}
		if (PickedInstaller.IsSet()
			&& FPaths::IsSamePath(
				PickedInstaller->Location.CanonicalPath, LatestPath))
		{
			return;
		}
		FTrackedInstallerLocation Replacement;
		Replacement.Location.Source = EWoTInstallerSource::Picked;
		Replacement.Location.CanonicalPath = MoveTemp(LatestPath);
		Replacement.Location.bRequirePinnedName = false;
		PickedInstaller = MoveTemp(Replacement);
	}

	static FString InstallerUnavailableSlotKey(EWoTInstallerSource Source,
		const FString& CandidatePath)
	{
		return FString::Printf(TEXT("%d|%s"), static_cast<int32>(Source),
			*FPaths::GetCleanFilename(CandidatePath));
	}

	void RecordInstallerLocationUnavailable(const FWork& Work,
		EWoTInstallerSource Source, const FString& CandidatePath,
		const FWoTBootstrapError& Error,
		TSet<FString>& UnavailableThisSweep)
	{
		const FString Leaf = FPaths::GetCleanFilename(CandidatePath);
		const FString DedupeKey = FString::Printf(TEXT("%s|%d"),
			*InstallerUnavailableSlotKey(Source, CandidatePath),
			static_cast<int32>(Error.Code));
		UnavailableThisSweep.Add(DedupeKey);
		if (RecordedInstallerUnavailable.Contains(DedupeKey))
		{
			return;
		}
		RecordedInstallerUnavailable.Add(DedupeKey);
		WoTRecordBootstrapHeartbeat(TEXT("installer-location-unavailable"),
			Work.Request.Phase, Work.Request.OperationEpoch, {}, {},
			FString::Printf(TEXT("source=%s leaf=%s detail=%s"),
				InstallerSourceName(Source), *Leaf, LexToString(Error.Code)));
	}

	/** Reads the remembered Browse pick once per content flow. The record is
	 * untrusted: anything but a well-formed v1 record is ignored, and a stale
	 * one is kept on disk (see TrackRememberedInstaller). */
	void LoadRememberedInstallerRecord(const FWork& Work)
	{
		RememberedInstaller.Reset();
		RememberedInstallerRecordPath.Reset();
		bRememberedInstallerRejected = false;
		FWoTRememberedInstallerRead Read = WoTReadRememberedInstaller(
			Deps.UserStateRoot,
			Deps.bHasUserStateRootIdentity ? &Deps.UserStateRootIdentity
				: nullptr);
		if (!Read.IgnoredReason.IsEmpty())
		{
			WoTRecordBootstrapHeartbeat(TEXT("remembered-installer-ignored"),
				Work.Request.Phase, Work.Request.OperationEpoch, {}, {},
				TEXT("reason=") + Read.IgnoredReason);
		}
		if (Read.CanonicalInstallerPath.IsSet())
		{
			RememberedInstallerRecordPath =
				MoveTemp(Read.CanonicalInstallerPath.GetValue());
		}
	}

	/**
	 * Admits the remembered path to this sweep after exactly the
	 * canonicalization a Browse pick gets. A record whose path no longer
	 * canonicalizes to itself (a link or rename above the installer since it
	 * was recorded) is skipped for the rest of the flow; an unreachable parent
	 * (an unplugged drive, an offline share) is retried on the next sweep.
	 *
	 * Nothing here deletes the record. A missing or unreachable installer is
	 * most often a drive that is temporarily away, and deleting would make the
	 * player browse again once it is back. A wrong or changed file is never
	 * used, and the record is replaced the next time a Browse pick builds the
	 * cache, which the player must do anyway in that case. Reads stay
	 * side-effect free.
	 */
	void TrackRememberedInstaller(const FWork& Work,
		TSet<FString>& UnavailableThisSweep,
		TSet<FString>& ProbedUnavailableSlots)
	{
		if (RememberedInstaller.IsSet() || bRememberedInstallerRejected
			|| RememberedInstallerRecordPath.IsEmpty())
		{
			return;
		}
		ProbedUnavailableSlots.Add(InstallerUnavailableSlotKey(
			EWoTInstallerSource::Remembered, RememberedInstallerRecordPath));
		TWoTBootstrapResult<FString> Canonical =
			WoTCanonicalizeInstallerPickPath(RememberedInstallerRecordPath);
		if (Canonical.IsError())
		{
			RecordInstallerLocationUnavailable(Work,
				EWoTInstallerSource::Remembered, RememberedInstallerRecordPath,
				Canonical.GetError(), UnavailableThisSweep);
			return;
		}
		if (!Canonical.GetValue().Equals(
				RememberedInstallerRecordPath, ESearchCase::CaseSensitive))
		{
			bRememberedInstallerRejected = true;
			WoTRecordBootstrapHeartbeat(TEXT("remembered-installer-ignored"),
				Work.Request.Phase, Work.Request.OperationEpoch, {}, {},
				TEXT("reason=not-canonical"));
			return;
		}
		FTrackedInstallerLocation Tracked;
		Tracked.Location.Source = EWoTInstallerSource::Remembered;
		Tracked.Location.CanonicalPath = MoveTemp(Canonical).GetValue();
		Tracked.Location.bRequirePinnedName = false;
		RememberedInstaller = MoveTemp(Tracked);
	}

	/** A remembered location whose parent changed identity mid-flow is
	 * dropped silently instead of failing the bootstrap like a pick: the
	 * player never chose it in this session, so the normal search continues. */
	void RejectRememberedInstaller(const FWork& Work,
		const FWoTBootstrapError& Error, TSet<FString>& UnavailableThisSweep)
	{
		bRememberedInstallerRejected = true;
		if (RememberedInstaller.IsSet())
		{
			RecordInstallerLocationUnavailable(Work,
				EWoTInstallerSource::Remembered,
				RememberedInstaller->Location.CanonicalPath, Error,
				UnavailableThisSweep);
		}
	}

	/** Records the Browse pick that just built the cache. Best effort: a
	 * failure is a heartbeat, never a failed build. */
	void RememberPickedInstaller(const FWork& Work,
		const FString& CanonicalPath)
	{
		FWoTBootstrapStatus Saved = WoTWriteRememberedInstaller(
			Deps.UserStateRoot,
			Deps.bHasUserStateRootIdentity ? &Deps.UserStateRootIdentity
				: nullptr,
			CanonicalPath);
		WoTRecordBootstrapHeartbeat(TEXT("installer-remembered"),
			Work.Request.Phase, Work.Request.OperationEpoch, {}, {},
			Saved.IsError()
				? FString::Printf(TEXT("result=failed detail=%s"),
					LexToString(Saved.GetError().Code))
				: FString(TEXT("result=saved")));
	}

	TWoTBootstrapResult<FWoTInstallerSweep> SweepInstallerLocations(
		FWork& Work)
	{
		using FResult = TWoTBootstrapResult<FWoTInstallerSweep>;
		AdoptLatestPickedInstallerRequest();
		FWoTInstallerSweep Sweep;
		TSet<FString> UnavailableThisSweep;
		TSet<FString> ProbedUnavailableSlots;
		TrackRememberedInstaller(Work, UnavailableThisSweep,
			ProbedUnavailableSlots);
		TArray<FTrackedInstallerLocation*> Locations;
		if (PickedInstaller.IsSet())
		{
			Locations.Add(&PickedInstaller.GetValue());
		}
		if (!bOriginalGameTranslocated)
		{
			Locations.Add(&AdjacentInstaller);
		}
		// The remembered Browse pick goes right after the installer beside the
		// game and before Downloads, unless an earlier location is that file.
		if (RememberedInstaller.IsSet() && !bRememberedInstallerRejected
			&& !Locations.ContainsByPredicate(
				[this](const FTrackedInstallerLocation* Earlier)
				{
					return FPaths::IsSamePath(Earlier->Location.CanonicalPath,
						RememberedInstaller->Location.CanonicalPath);
				}))
		{
			Locations.Add(&RememberedInstaller.GetValue());
		}
		for (FTrackedInstallerLocation& Downloads : DownloadsInstallers)
		{
			Locations.Add(&Downloads);
		}

		TUniquePtr<FWoTHeldInstallerParent> DownloadsParent;
		bool bDownloadsUnavailable = false;
		for (FTrackedInstallerLocation* Tracked : Locations)
		{
			if (Tracked == nullptr
				|| (Tracked->Location.Source == EWoTInstallerSource::Downloads
					&& bDownloadsUnavailable))
			{
				continue;
			}
			const FWoTHeldInstallerParent* Parent = nullptr;
			if (Tracked->Location.Source == EWoTInstallerSource::Downloads)
			{
				if (!DownloadsParent.IsValid())
				{
					ProbedUnavailableSlots.Add(InstallerUnavailableSlotKey(
						EWoTInstallerSource::Downloads, Deps.DownloadsDirectory));
			#if WITH_AUTOMATION_TESTS
					if (Deps.InstallerLocationFaultForTesting)
					{
						TOptional<FWoTBootstrapError> ParentFault =
							Deps.InstallerLocationFaultForTesting(
								EWoTInstallerSource::Downloads,
								Deps.DownloadsDirectory);
						if (ParentFault.IsSet())
						{
							RecordInstallerLocationUnavailable(Work,
								EWoTInstallerSource::Downloads,
								Deps.DownloadsDirectory,
								ParentFault.GetValue(), UnavailableThisSweep);
							bDownloadsUnavailable = true;
							continue;
						}
					}
			#endif
					TWoTBootstrapResult<FWoTHeldInstallerParent> Opened =
						WoTOpenInstallerParent(
							Deps.DownloadsDirectory, DownloadsParentIdentity);
					if (Opened.IsError())
					{
						FWoTBootstrapError Error = MoveTemp(Opened).GetError();
						if (WoTInstallerAcquisitionIsParentIdentityMismatch(Error))
						{
							RecordedInstallerUnavailable.Reset();
							return FResult::Error(MoveTemp(Error));
						}
						RecordInstallerLocationUnavailable(Work,
							EWoTInstallerSource::Downloads,
							Deps.DownloadsDirectory, Error, UnavailableThisSweep);
						bDownloadsUnavailable = true;
						continue;
					}
					DownloadsParent = MakeUnique<FWoTHeldInstallerParent>(
						MoveTemp(Opened).GetValue());
					for (FTrackedInstallerLocation& Downloads : DownloadsInstallers)
					{
						Downloads.Location.ParentIdentity = DownloadsParentIdentity;
					}
				}
				Parent = DownloadsParent.Get();
			}
			ProbedUnavailableSlots.Add(InstallerUnavailableSlotKey(
				Tracked->Location.Source, Tracked->Location.CanonicalPath));
			const bool bRemembered =
				Tracked->Location.Source == EWoTInstallerSource::Remembered;
			// The remembered location is a silent shortcut: whatever it finds,
			// the reason shown to the player comes only from the normal search.
			auto MergeReason = [&Sweep, bRemembered](
				EWoTOriginalGameNeededReason Reason, FString Detail = FString())
			{
				if (!bRemembered)
				{
					MergeInstallerReason(Sweep, Reason, MoveTemp(Detail));
				}
			};
			TWoTBootstrapResult<FWoTAdjacentInstallerInspection> Inspected =
				InspectInstallerLocation(Tracked->Location, Parent);
			if (Inspected.IsError())
			{
				FWoTBootstrapError Error = MoveTemp(Inspected).GetError();
				if (WoTInstallerAcquisitionIsParentIdentityMismatch(Error))
				{
					if (bRemembered)
					{
						RejectRememberedInstaller(
							Work, Error, UnavailableThisSweep);
						continue;
					}
					RecordedInstallerUnavailable.Reset();
					return FResult::Error(MoveTemp(Error));
				}
				RecordInstallerLocationUnavailable(Work,
					Tracked->Location.Source, Tracked->Location.CanonicalPath,
					Error, UnavailableThisSweep);
				continue;
			}
			const FWoTAdjacentInstallerInspection& Observation =
				Inspected.GetValue();
			const EWoTOriginalGameNeededReason WrongReason =
				Tracked->Location.Source == EWoTInstallerSource::Picked
					? EWoTOriginalGameNeededReason::PickedWrongFile
					: EWoTOriginalGameNeededReason::NotTheInstaller;
			if (Observation.State == EWoTPinnedAdjacentInstallerProbe::Missing)
			{
				Tracked->PreviousCandidateIdentity.Reset();
				if (Tracked->Location.Source == EWoTInstallerSource::Picked)
				{
					MergeReason(WrongReason);
				}
				continue;
			}
			if (Observation.State
				== EWoTPinnedAdjacentInstallerProbe::Downloading)
			{
				Tracked->PreviousCandidateIdentity.Reset();
				MergeReason(
					Tracked->Location.Source == EWoTInstallerSource::Picked
						? WrongReason
						: EWoTOriginalGameNeededReason::Downloading);
				continue;
			}
			if (Observation.State
				== EWoTPinnedAdjacentInstallerProbe::OnlineOnly)
			{
				Tracked->PreviousCandidateIdentity.Reset();
				MergeReason(EWoTOriginalGameNeededReason::OnlineOnly,
					Observation.Message);
				continue;
			}
			if (Observation.State
				== EWoTPinnedAdjacentInstallerProbe::UnsupportedReparsePoint)
			{
				Tracked->PreviousCandidateIdentity.Reset();
				MergeReason(
					EWoTOriginalGameNeededReason::UnsupportedReparsePoint);
				continue;
			}
			if (Observation.State
				== EWoTPinnedAdjacentInstallerProbe::NotTheInstaller)
			{
				Tracked->PreviousCandidateIdentity.Reset();
				MergeReason(WrongReason);
				continue;
			}
			if (Observation.State != EWoTPinnedAdjacentInstallerProbe::Candidate
				|| !Observation.Identity.IsSet())
			{
				continue;
			}
			const FWoTNativeFileIdentity Identity =
				Observation.Identity.GetValue();
			if (Tracked->KnownWrongIdentity.IsSet()
				&& Tracked->KnownWrongIdentity.GetValue() == Identity)
			{
				MergeReason(WrongReason);
				continue;
			}
			if (!Tracked->PreviousCandidateIdentity.IsSet()
				|| Tracked->PreviousCandidateIdentity.GetValue() != Identity)
			{
				Tracked->PreviousCandidateIdentity = Identity;
				Sweep.bNeedsStabilityPass = true;
				continue;
			}
			TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult> Acquired =
				AcquireInstallerLocation(Tracked->Location, &Identity, Parent);
			if (Acquired.IsError())
			{
				FWoTBootstrapError Error = MoveTemp(Acquired).GetError();
				if (WoTInstallerAcquisitionIsIdentityMutation(Error))
				{
					Tracked->PreviousCandidateIdentity.Reset();
					continue;
				}
				if (WoTInstallerAcquisitionIsParentIdentityMismatch(Error))
				{
					if (bRemembered)
					{
						RejectRememberedInstaller(
							Work, Error, UnavailableThisSweep);
						continue;
					}
					RecordedInstallerUnavailable.Reset();
					return FResult::Error(MoveTemp(Error));
				}
				RecordInstallerLocationUnavailable(Work,
					Tracked->Location.Source, Tracked->Location.CanonicalPath,
					Error, UnavailableThisSweep);
				continue;
			}
			FWoTPinnedAdjacentInstallerResult Result =
				MoveTemp(Acquired).GetValue();
			if (Result.State == EWoTPinnedAdjacentInstallerProbe::Valid
				&& Result.Verified.IsValid())
			{
				Sweep.FoundSource = Tracked->Location.Source;
				Sweep.FoundCanonicalPath = Tracked->Location.CanonicalPath;
				Sweep.Verified = MoveTemp(Result.Verified);
				RecordedInstallerUnavailable.Reset();
				return FResult::Success(MoveTemp(Sweep));
			}
			if (Result.State
				== EWoTPinnedAdjacentInstallerProbe::NotTheInstaller)
			{
				Tracked->KnownWrongIdentity = Identity;
				MergeReason(WrongReason);
			}
			else if (Result.State
				== EWoTPinnedAdjacentInstallerProbe::Downloading)
			{
				MergeReason(
					Tracked->Location.Source == EWoTInstallerSource::Picked
						? WrongReason
						: EWoTOriginalGameNeededReason::Downloading);
			}
			else if (Result.State
				== EWoTPinnedAdjacentInstallerProbe::OnlineOnly)
			{
				MergeReason(EWoTOriginalGameNeededReason::OnlineOnly,
					Result.Message);
			}
			else if (Result.State
				== EWoTPinnedAdjacentInstallerProbe::UnsupportedReparsePoint)
			{
				MergeReason(
					EWoTOriginalGameNeededReason::UnsupportedReparsePoint);
			}
			Tracked->PreviousCandidateIdentity.Reset();
		}
		for (auto It = RecordedInstallerUnavailable.CreateIterator(); It; ++It)
		{
			int32 ErrorSeparator = INDEX_NONE;
			if (It->FindLastChar(TEXT('|'), ErrorSeparator)
				&& ProbedUnavailableSlots.Contains(It->Left(ErrorSeparator))
				&& !UnavailableThisSweep.Contains(*It))
			{
				It.RemoveCurrent();
			}
		}
		return FResult::Success(MoveTemp(Sweep));
	}

	TWoTBootstrapResult<FWoTCacheGenerationBuildIdentity>
		ExpectedCacheIdentity() const
	{
		using FResult = TWoTBootstrapResult<FWoTCacheGenerationBuildIdentity>;
		TWoTBootstrapResult<FWoTSha256> InstallerSha =
			FWoTSha256::FromHex(WoT::Bootstrap::InstallerSha256);
#if WITH_AUTOMATION_TESTS
		if (Deps.InstallerPinsForTesting.IsSet())
		{
			InstallerSha = TWoTBootstrapResult<FWoTSha256>::Success(
				Deps.InstallerPinsForTesting.GetValue().Sha256);
		}
#endif
		TWoTBootstrapResult<FWoTSha256> ContentLabel = GetCacheContentLabel();
		TWoTBootstrapResult<FWoTRetailContract> Contract =
			WoTLoadProductionRetailContract();
		if (InstallerSha.IsError())
		{
			return FResult::Error(MoveTemp(InstallerSha).GetError());
		}
		if (ContentLabel.IsError())
		{
			return FResult::Error(MoveTemp(ContentLabel).GetError());
		}
		if (Contract.IsError())
		{
			return FResult::Error(MoveTemp(Contract).GetError());
		}
		FWoTCacheGenerationBuildIdentity Identity;
		Identity.bHasBuildIdentity = true;
		Identity.InstallerSha256 = InstallerSha.GetValue();
		Identity.ContractSha256 = ComputeRetailContractIdentity(Contract.GetValue());
		Identity.ProductionDescriptorSha256 = ContentLabel.GetValue();
		return FResult::Success(MoveTemp(Identity));
	}

	TWoTBootstrapResult<FWoTCacheStore> OpenCacheStore() const
	{
#if WITH_AUTOMATION_TESTS
		if (Deps.bUseSemanticCacheFixture)
		{
			return FWoTCacheStore::ForTesting(Deps.ContentCacheRoot);
		}
#endif
		return FWoTCacheStore::Open(Deps.ContentCacheRoot);
	}

	FWoTBootstrapStatus OpenCurrentCache(FWork& Work)
	{
		ValidatedCacheSnapshot.Reset();
#if WITH_AUTOMATION_TESTS
		if (Deps.InitialCacheSnapshotForTesting.IsSet())
		{
			ValidatedCacheSnapshot.Emplace(
				MoveTemp(Deps.InitialCacheSnapshotForTesting.GetValue()));
			Deps.InitialCacheSnapshotForTesting.Reset();
			bCacheProbedForCurrentFlow = true;
			return FWoTBootstrapStatus::Success();
		}
#endif
		TWoTBootstrapResult<FWoTCacheGenerationBuildIdentity> Identity =
			ExpectedCacheIdentity();
		if (Identity.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Identity).GetError());
		}
		TWoTBootstrapResult<FWoTCacheStore> Opened = OpenCacheStore();
		if (Opened.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Opened).GetError());
		}
		FWoTGameBootstrapCacheProgress Progress(
			Work.Progress, Work.Request.OperationEpoch, bCancelled,
			Work.PhaseStartedSeconds, Deps.NowSeconds);
		TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>> Current =
			Opened.GetValue().TryOpenCurrent(Identity.GetValue(), &Progress);
		if (Current.IsError())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Current).GetError());
		}
		if (Current.GetValue().IsSet())
		{
#if WITH_AUTOMATION_TESTS
			if (Deps.bUseSemanticCacheFixture)
			{
				TWoTBootstrapResult<FWoTRetailContract> Contract =
					WoTLoadProductionRetailContract();
				if (Contract.IsError())
				{
					return FWoTBootstrapStatus::Error(
						MoveTemp(Contract).GetError());
				}
				FWoTBootstrapStatus Verified = Current.GetValue().GetValue()
					.VerifyCriticalClosureForTesting(
						Contract.GetValue().StartupCriticalLogicalIds, &Progress);
				if (Verified.IsError())
				{
					Current.GetValue().Reset();
				}
			}
#endif
		}
		if (Current.GetValue().IsSet())
		{
			ValidatedCacheSnapshot.Emplace(MoveTemp(Current.GetValue().GetValue()));
		}
		bCacheProbedForCurrentFlow = true;
		return FWoTBootstrapStatus::Success();
	}

	FWoTBootstrapWorkerCompletion Execute(FWork& Work)
	{
		switch (Work.Request.Phase)
		{
		case EWoTBootstrapPhase::EstablishingIdentity:
			return EstablishIdentity(Work);
		case EWoTBootstrapPhase::CheckingGameUpdates:
			return CheckGameUpdates(Work);
		case EWoTBootstrapPhase::DownloadingUpdate:
			return DownloadUpdate(Work);
		case EWoTBootstrapPhase::CheckingOriginalGameData:
			return CheckOriginalGameData(Work);
		case EWoTBootstrapPhase::OriginalGameNeeded:
			return OriginalGameNeeded(Work);
		case EWoTBootstrapPhase::ExtractingOriginalGame:
			return ExtractOriginalGame(Work);
		case EWoTBootstrapPhase::BuildingContent:
			return BuildContent(Work);
		case EWoTBootstrapPhase::VerifyingContent:
			return VerifyContent(Work);
		case EWoTBootstrapPhase::StartingGame:
			return StartGame(Work);
		default:
			return Failure(
				Work, EWoTBootstrapDiagnosticCode::UnexpectedPhase);
		}
	}

	FWoTBootstrapWorkerCompletion EstablishIdentity(FWork& Work)
	{
		WoTRecordBootstrapHeartbeat(TEXT("release-identity"),
			EWoTBootstrapPhase::EstablishingIdentity,
			Work.Request.OperationEpoch, {}, {},
			FString::Printf(TEXT("sequence=%llu stamped=%d"),
				static_cast<unsigned long long>(
					Deps.RunningIdentity.ReleaseSequence),
				Deps.RunningIdentity.bStamped ? 1 : 0));
		NotifyOnlyReason = Deps.UpdateDisableReason;
		if (Deps.bHasUserStateRootIdentity)
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> ReopenedUserState =
				OpenNativeDirectoryNoFollow(Deps.UserStateRoot);
			if (ReopenedUserState.IsError()
				|| !ReopenedUserState.GetValue().GetIdentity().IsSameObject(
					Deps.UserStateRootIdentity))
			{
				NotifyOnlyReason = TEXT("state-root-identity-changed");
				if (ReopenedUserState.IsError())
				{
					NotifyOnlyDetail = ReopenedUserState.GetError().ToString();
				}
				bRecoveryClean.Store(false);
				return Completion(
					Work, EWoTBootstrapPhase::CheckingGameUpdates);
			}
		}
		// This retired directory is user-state-owned, not updater-layout-owned.
		// Remove it immediately after authenticating that root so notify-only and
		// otherwise update-disabled starts perform the migration too.
		WoTRemoveLegacyInstallerStateBestEffort(
			FPaths::Combine(Deps.UserStateRoot, TEXT("UpdaterState")));
		if (!NotifyOnlyReason.IsEmpty())
		{
			// A resolved notify-only reason is terminal for updater work. The
			// Guarded game/content phases do not need updater metadata, so update
			// state cannot delay a playable launch.
			bRecoveryClean.Store(false);
			return Completion(Work, EWoTBootstrapPhase::CheckingGameUpdates);
		}
		if (Deps.bHasInstallRootIdentity)
		{
			TWoTBootstrapResult<FWoTNativeDirectoryHandle> ReopenedInstallRoot =
				OpenNativeDirectoryNoFollow(Deps.InstallRoot);
			if (ReopenedInstallRoot.IsError()
				|| !ReopenedInstallRoot.GetValue().GetIdentity().IsSameObject(
					Deps.InstallRootIdentity))
			{
				NotifyOnlyReason = TEXT("install-root-identity-changed");
				if (ReopenedInstallRoot.IsError())
				{
					NotifyOnlyDetail =
						ReopenedInstallRoot.GetError().ToString();
				}
				bRecoveryClean.Store(false);
				return Completion(
					Work, EWoTBootstrapPhase::CheckingGameUpdates);
			}
		}
		// Establish notify-only mode before choosing the updater-state root. In
		// particular, a failed-closed App Translocation query must not let an
		// inaccessible volume-root .wot-update turn a playable launch into a
		// bootstrap failure. A layout that is already known unsuitable for
		// updates also has no outer-app question left to ask.
		if (NotifyOnlyReason.IsEmpty())
		{
			bool bRunningTreeTranslocated = false;
#if WITH_AUTOMATION_TESTS
			bRunningTreeTranslocated = Deps.RunningTreeTranslocatedForTesting
				? Deps.RunningTreeTranslocatedForTesting(Deps.InstalledTreeRoot)
				: WoTIsRunningTreeTranslocated(Deps.InstalledTreeRoot);
#else
			bRunningTreeTranslocated =
				WoTIsRunningTreeTranslocated(Deps.InstalledTreeRoot);
#endif
			if (bRunningTreeTranslocated)
			{
				NotifyOnlyReason = TEXT("translocated");
			}
		}
		if (!NotifyOnlyReason.IsEmpty())
		{
			bRecoveryClean.Store(false);
			return Completion(Work, EWoTBootstrapPhase::CheckingGameUpdates);
		}
		TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved =
			ResolveUpdaterStateRoot(
				Deps.UserStateRoot, Deps.InstallRoot, Deps.LiveLeaf);
		if (Resolved.IsError())
		{
			NotifyOnlyReason = TEXT("state-root-unresolved");
			NotifyOnlyDetail = Resolved.GetError().ToString();
			bRecoveryClean.Store(false);
			return Completion(Work, EWoTBootstrapPhase::CheckingGameUpdates);
		}
		Root = MoveTemp(Resolved).GetValue();
		FWoTBootstrapStatus Rollback = WoTConsumeUpdateRollback(Root.GetValue(), Deps.RunningIdentity.ReleaseSequence);
		if (Rollback.IsError())
		{
			NotifyOnlyReason = TEXT("rollback-notice-unavailable");
			NotifyOnlyDetail = Rollback.GetError().ToString();
			bRecoveryClean.Store(false);
			return Completion(Work, EWoTBootstrapPhase::CheckingGameUpdates);
		}
		RefreshUpdateState(EWoTBootstrapPhase::EstablishingIdentity,
			Work.Request.OperationEpoch);
		bRecoveryClean.Store(true);


		return Completion(Work, EWoTBootstrapPhase::CheckingGameUpdates);
	}

	void RefreshUpdateState(EWoTBootstrapPhase Phase, uint64 OperationEpoch)
	{
		if (!Root.IsSet())
		{
			return;
		}
		FWoTUpdateStateRecordReadResult ReadState =
			ReadWoTUpdateState(Root.GetValue());
		UpdateStateRecord.KeyRevocationFloor = FMath::Max(
			UpdateStateRecord.KeyRevocationFloor,
			ReadState.State.KeyRevocationFloor);
		UpdateStateRecord.NewestAnnouncementSequence = FMath::Max(
			UpdateStateRecord.NewestAnnouncementSequence,
			ReadState.State.NewestAnnouncementSequence);
		if (!ReadState.Diagnostic.IsEmpty())
		{
			WoTRecordBootstrapHeartbeat(TEXT("update-state-defaulted"), Phase,
				OperationEpoch, {}, {}, ReadState.Diagnostic.Left(1024));
		}
	}

	FWoTBootstrapWorkerCompletion CheckGameUpdates(
		FWork& Work, bool bBackgroundUpdate = false)
	{
		if (!bBackgroundUpdate)
		{
			if (!NotifyOnlyReason.IsEmpty())
			{
				return CompleteGameUpdateCheck(Work, true);
			}
			// Bootstrap remains local-only. Adjacent original-game content is
			// inspected only by CheckingOriginalGameData so there is exactly one
			// stability and hashing policy.
			ResetContentOperationState();
			ResetCacheActivationState();
			// Critical cache verification belongs to CheckingOriginalGameData,
			// inside the measured warm-start content interval.
			return CompleteGameUpdateCheck(Work);
		}
		if (CancellableHops == nullptr || !Root.IsSet())
		{
			return Failure(Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
		}
		if (Work.Request.AnnouncementLogicalObjects.Num() != 2
			|| Work.Request.AnnouncementLogicalObjects[0] != Deps.AnnouncementJsonUrl
			|| Work.Request.AnnouncementLogicalObjects[1]
				!= Deps.AnnouncementJsonUrl + TEXT(".sig"))
		{
			return Failure(Work, EWoTBootstrapDiagnosticCode::ConfigInvalid);
		}
		TWoTBootstrapResult<FWoTKeyRing> AnnouncementRing =
			WoTGameBootstrapWorkerAnnouncementRing(
				WoT::Bootstrap::Trust::AnnouncementPublicKeyHex);
		if (AnnouncementRing.IsError())
		{
			if (!NotifyOnlyReason.IsEmpty())
			{
				return CompleteGameUpdateCheck(Work, true);
			}
			return Failure(
				Work, MoveTemp(AnnouncementRing).GetError());
		}
		FWoTKeyRing AssessmentRing =
			MoveTemp(AnnouncementRing).GetValue();

		const double AbsoluteDeadline =
			Deps.NowSeconds() + Deps.AnnouncementDeadlineSeconds;
		FWoTGameAnnouncementTransport GameTransport(*CancellableHops,
			Deps.AnnouncementJsonUrl, AbsoluteDeadline, Deps.NowSeconds);
		FRecordingAnnouncementTransport Recording(GameTransport);
		// Recording makes both policy assessments below consume the same
		// authenticated bytes.
		TArray<uint8> PrefetchedAnnouncement;
		TArray<uint8> PrefetchedSignature;
		if (FWoTBootstrapStatus Prefetched = Recording.FetchPair(
				PrefetchedAnnouncement, PrefetchedSignature);
			Prefetched.IsError())
		{
			return Failure(Work, MoveTemp(Prefetched).GetError());
		}
		RefreshUpdateState(EWoTBootstrapPhase::CheckingGameUpdates,
			Work.Request.OperationEpoch);
		TWoTBootstrapResult<FWoTUpdateAssessment> Assessed =
			AssessAnnouncement(
				Recording, AssessmentRing, UpdateStateRecord, Root.GetValue(),
				Deps.RunningIdentity.ReleaseSequence,
				true);
#if WITH_AUTOMATION_TESTS || WOT_BOOTSTRAP_TEST_IDENTITIES
		if (Assessed.IsError())
		{
			FWoTBootstrapError AssessmentError =
				MoveTemp(Assessed).GetError();
			if (AssessmentError.Code == EWoTBootstrapError::Crypto
				&& WoTShouldAcceptTestUpdateIdentities())
			{
				TWoTBootstrapResult<FWoTKeyRing> TestRing =
					WoTGameBootstrapWorkerAnnouncementRing(
						WoTTestAnnouncementPublicKeyHex());
				if (!TestRing.IsError())
				{
					AssessmentRing = MoveTemp(TestRing).GetValue();
					Assessed = AssessAnnouncement(
						Recording, AssessmentRing, UpdateStateRecord,
						Root.GetValue(),
						Deps.RunningIdentity.ReleaseSequence,
						true);
				}
				else
				{
					if (!NotifyOnlyReason.IsEmpty())
					{
						return CompleteGameUpdateCheck(Work, true);
					}
					return Failure(
						Work, MoveTemp(TestRing).GetError());
				}
			}
			else
			{
				if (!NotifyOnlyReason.IsEmpty())
				{
					return CompleteGameUpdateCheck(Work, true);
				}
				return Failure(Work, AssessmentError);
			}
		}
#endif
		if (Assessed.IsError())
		{
			if (!NotifyOnlyReason.IsEmpty())
			{
				return CompleteGameUpdateCheck(Work, true);
			}
			return Failure(Work, MoveTemp(Assessed).GetError());
		}

		FWoTUpdateAssessment Assessment = MoveTemp(Assessed).GetValue();
		if (!Assessment.NotifyOnlyDetail.IsEmpty())
		{
			WoTRecordBootstrapHeartbeat(TEXT("update-state-save-failed"),
				EWoTBootstrapPhase::CheckingGameUpdates,
				Work.Request.OperationEpoch, {}, {},
				Assessment.NotifyOnlyDetail.Left(1024));
		}
		auto FailedSequence = ReadUpdateFailedSequence(Root.GetValue());
		if (FailedSequence.IsError()) return Failure(Work, MoveTemp(FailedSequence).GetError());
		if (Assessment.Announcement.IsSet())
		{
			const uint64 Sequence = Assessment.Announcement->ReleaseSequence;
			if (Sequence == FailedSequence.GetValue())
			{
				FWoTUpdateState Suppressed;
				Suppressed.Status = EWoTUpdateStatus::KnownBad;
				Suppressed.ReleaseSequence = Sequence;
				Suppressed.Reason = TEXT("failed-release");
				BackgroundPolicyResult.Emplace(MoveTemp(Suppressed));
				BackgroundSkipReason = TEXT("failed-release");
				return CompleteGameUpdateCheck(Work, true);
			}
			auto Cleared = ClearUpdateFailedSequenceForNewerRelease(Root.GetValue(), Sequence);
			if (Cleared.IsError()) return Failure(Work, MoveTemp(Cleared).GetError());
		}
		if (Assessment.Decision == EWoTUpdateDecision::Current)
		{
			return CompleteGameUpdateCheck(Work);
		}
		if (Assessment.Decision
			== EWoTUpdateDecision::ContinueCurrentAndLogUnavailable)
		{
			if (!Assessment.NotifyOnlyReason.IsEmpty())
			{
				NotifyOnlyReason = Assessment.NotifyOnlyReason;
				NotifyOnlyDetail = Assessment.NotifyOnlyDetail;
				return CompleteGameUpdateCheck(Work, false,
					Assessment.Announcement.IsSet()
						? &Assessment.Announcement.GetValue() : nullptr);
			}
			return CompleteGameUpdateCheck(Work, true);
		}
		if (Assessment.Decision == EWoTUpdateDecision::StayBlocked
			|| !Assessment.Announcement.IsSet())
		{
			if (!NotifyOnlyReason.IsEmpty())
			{
				return CompleteGameUpdateCheck(Work, true);
			}
			return Failure(
				Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
		}

		Announcement = Assessment.Announcement.GetValue();
		if (NotifyOnlyReason.IsEmpty())
		{
			FWoTBootstrapStatus Writable = FWoTBootstrapStatus::Success();
#if WITH_AUTOMATION_TESTS
			if (Deps.InstallWritabilityProbeForTesting)
			{
				Writable = Deps.InstallWritabilityProbeForTesting();
			}
			else
#endif
			{
				Writable = ProbeUpdaterInstallWritable(Root.GetValue(), [this]()
				{
					return CheckBackgroundCancellation();
				});
			}
			if (Writable.IsError())
			{
				NotifyOnlyReason = TEXT("install-root-not-writable");
				NotifyOnlyDetail = Writable.GetError().ToString();
			}
		}
		if (!NotifyOnlyReason.IsEmpty())
		{
			return CompleteGameUpdateCheck(
				Work, false, &Announcement.GetValue());
		}
		RefreshUpdateState(EWoTBootstrapPhase::CheckingGameUpdates,
			Work.Request.OperationEpoch);
		const uint64 AnnouncementHighWater = FMath::Max(
			UpdateStateRecord.NewestAnnouncementSequence,
			Deps.RunningIdentity.ReleaseSequence);
		Assessed = AssessAnnouncement(Recording, AssessmentRing,
			UpdateStateRecord, Root.GetValue(),
			Deps.RunningIdentity.ReleaseSequence, false);
		if (Assessed.IsError())
		{
			return Failure(Work, MoveTemp(Assessed).GetError());
		}
		Assessment = MoveTemp(Assessed).GetValue();
		if (!Assessment.NotifyOnlyDetail.IsEmpty())
		{
			WoTRecordBootstrapHeartbeat(TEXT("update-state-save-failed"),
				EWoTBootstrapPhase::CheckingGameUpdates,
				Work.Request.OperationEpoch, {}, {},
				Assessment.NotifyOnlyDetail.Left(1024));
		}
		if (Assessment.Decision != EWoTUpdateDecision::MustStage
			|| !Assessment.Announcement.IsSet()
			|| Assessment.Announcement->ReleaseSequence
				< AnnouncementHighWater)
		{
			BackgroundSkipReason = TEXT("ordering-superseded");
			return CompleteGameUpdateCheck(Work, true);
		}
		Announcement = Assessment.Announcement.GetValue();
		Download = MakeUnique<FWoTGameReleaseDownload>(
			*CancellableHops, Announcement.GetValue(),
			Recording.AnnouncementBytes(), Recording.SignatureBytes(),
			Deps.DownloadOperationDeadlineSeconds, Deps.NowSeconds,
			[this](uint64 CompletedBytes)
			{
				if (bCancelled.Load() || ActiveDownloadProgress == nullptr
					|| !(*ActiveDownloadProgress))
				{
					return;
				}
				FWoTBootstrapProgressEvent Event;
				Event.OperationEpoch = ActiveDownloadEpoch;
				Event.CompletedBytes = FMath::Min(CompletedBytes,
					ActiveDownloadTotal);
				Event.TotalBytes = ActiveDownloadTotal;
				Event.ElapsedSeconds = FMath::Max(0.0,
					Deps.NowSeconds() - ActiveDownloadPhaseStartedSeconds);
				(*ActiveDownloadProgress)(MoveTemp(Event));
			});

		FWoTReleaseStagingMetadata Metadata;
		FWoTBootstrapStatus Fetched = Download->FetchSignedMetadata(Metadata);
		if (Fetched.IsError())
		{
			return Failure(Work, MoveTemp(Fetched).GetError());
		}
		RefreshUpdateState(EWoTBootstrapPhase::CheckingGameUpdates,
			Work.Request.OperationEpoch);
		TWoTBootstrapResult<FWoTGameBootstrapWorkerVerifiedMetadata> Verified =
			WoTGameBootstrapWorkerVerifyMetadata(
				Announcement.GetValue(), Metadata,
				UpdateStateRecord.KeyRevocationFloor);
		if (Verified.IsError())
		{
			return Failure(Work, MoveTemp(Verified).GetError());
		}

		FWoTGameBootstrapWorkerVerifiedMetadata Accepted =
			MoveTemp(Verified).GetValue();
		Download->SetVerifiedArchiveSource(
			Accepted.Manifest.Archive.Url,
			Accepted.Manifest.Archive.Size);

		uint64 UpdateDownloadBytes = 0;
		if (!WoTGameBootstrapWorkerAddBudget(
				Accepted.Manifest.Archive.Size, UpdateDownloadBytes)
			|| !WoTGameBootstrapWorkerAddBudget(
				Announcement.GetValue().KeyRing.Size,
				UpdateDownloadBytes)
			|| !WoTGameBootstrapWorkerAddBudget(
				Announcement.GetValue().Manifest.Size,
				UpdateDownloadBytes))
		{
			return Failure(
				Work, EWoTBootstrapDiagnosticCode::ConfigInvalid);
		}
		DiskBudget = FWoTDiskBudgetInputs();
		DiskBudget.UpdateDownloadBytes = UpdateDownloadBytes;
		DiskBudget.CandidateInstallBytes =
			Accepted.Manifest.InstalledTreeRoot.Size;
		DiskBudget.ExtractionBytes =
			Accepted.Manifest.InstalledTreeRoot.Size;

		if (Accepted.ReleaseRing.Epoch > UpdateStateRecord.KeyRevocationFloor)
		{
			UpdateStateRecord.KeyRevocationFloor = Accepted.ReleaseRing.Epoch;
			FString StateDiagnostic;
			FWoTBootstrapStatus Saved = MergeAndWriteWoTUpdateState(
				Root.GetValue(), UpdateStateRecord, &StateDiagnostic);
			if (Saved.IsError() || !StateDiagnostic.IsEmpty())
			{
				const FString Detail = !StateDiagnostic.IsEmpty()
					? StateDiagnostic : Saved.GetError().ToString();
				WoTRecordBootstrapHeartbeat(TEXT("update-state-save-failed"),
					EWoTBootstrapPhase::CheckingGameUpdates,
					Work.Request.OperationEpoch, {}, {}, Detail.Left(1024));
			}
		}

		ReleaseRing = MoveTemp(Accepted.ReleaseRing);
		Manifest = MoveTemp(Accepted.Manifest);
		FWoTBootstrapWorkerCompletion Result =
			Completion(Work, EWoTBootstrapPhase::DownloadingUpdate);
		Result.bMandatoryWork = true;
		Result.NextPhaseDiskBudget = DiskBudget;
		return Result;
	}

	FWoTBootstrapWorkerCompletion DownloadUpdate(FWork& Work)
	{
		if (!Root.IsSet() || !Announcement.IsSet() || !ReleaseRing.IsSet()
			|| !Manifest.IsSet() || !Download.IsValid())
		{
			return Failure(
				Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
		}
		ActiveDownloadProgress = &Work.Progress;
		ActiveDownloadEpoch = Work.Request.OperationEpoch;
		ActiveDownloadTotal = Manifest.GetValue().Archive.Size;
		ActiveDownloadPhaseStartedSeconds = Work.PhaseStartedSeconds;
		TWoTBootstrapResult<FWoTStagedRelease> StagedResult =
			StageRelease(Announcement.GetValue(), ReleaseRing.GetValue(),
				*Download, Root.GetValue(), [this]()
				{
					return CheckBackgroundCancellation();
				});
		ActiveDownloadProgress = nullptr;
		if (StagedResult.IsError())
		{
			// Keep the staging error: a bare INSTALL-FAILED made an intermittent
			// staging failure on an intact archive undiagnosable.
			FWoTBootstrapWorkerCompletion Failed = Failure(
				Work, EWoTBootstrapDiagnosticCode::InstallFailed);
			Failed.FailureDetail = StagedResult.GetError().ToString();
			return Failed;
		}
		Staged = MakeUnique<FWoTStagedRelease>(
			MoveTemp(StagedResult).GetValue());
		FWoTBootstrapWorkerCompletion Result =
			Completion(Work, EWoTBootstrapPhase::CheckingOriginalGameData);
		Result.bMandatoryWork = true;
		Result.NextPhaseDiskBudget = DiskBudget;
		return Result;
	}



	FWoTBootstrapStatus CheckBackgroundCancellation()
	{
		if (bCancelled.Load())
		{
			return WoTGameBootstrapWorkerError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("the bootstrap worker was cancelled"));
		}
#if WITH_AUTOMATION_TESTS
		if (Deps.StagingYieldForTesting)
		{
			Deps.StagingYieldForTesting();
		}
		else
#endif
		{
			FPlatformProcess::SleepNoStats(0.0f);
		}
		return bCancelled.Load()
			? WoTGameBootstrapWorkerError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("the bootstrap worker was cancelled"))
			: FWoTBootstrapStatus::Success();
	}

	class FReleaseNotesSink final : public IWoTHttpBodySink
	{
	public:
		virtual FWoTBootstrapStatus OnResponseStart(
			const FWoTHttpHopResponse& HeadersOnly) override
		{
			Bytes.Reset();
			if (HeadersOnly.BodyLength > WoTReleaseNotes::MaximumFileBytes)
			{
				bExceeded = true;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::SizeLimit,
					TEXT("release notes exceeds the streaming size limit")));
			}
			return FWoTBootstrapStatus::Success();
		}

		virtual bool ShouldStreamResponseBody(int32) const override
		{
			return true;
		}

		virtual FWoTBootstrapStatus Write(TConstArrayView<uint8> Chunk) override
		{
			const uint64 NewSize = static_cast<uint64>(Bytes.Num())
				+ static_cast<uint64>(Chunk.Num());
			if (NewSize > WoTReleaseNotes::MaximumFileBytes)
			{
				bExceeded = true;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::SizeLimit,
					TEXT("release notes exceeds the streaming size limit")));
			}
			Bytes.Append(Chunk.GetData(), Chunk.Num());
			return FWoTBootstrapStatus::Success();
		}

		TArray<uint8> Bytes;
		bool bExceeded = false;
	};

	void FinishReleaseNotes(FWork& Work, FWoTReleaseNotesResult Result)
	{
		const FString Detail = FString::Printf(
			TEXT("outcome=%s sequence=%llu commit=%s"),
			LexToString(Result.Outcome),
			static_cast<unsigned long long>(Result.ReleaseSequence),
			*Result.ReleaseCommit);
		WoTRecordBootstrapHeartbeat(TEXT("release-notes-result"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, Detail);
		FScopeLock DeliveryScope(&DeliveryLock);
		if (!bCancelled.Load() && Work.ReleaseNotes)
		{
			Work.ReleaseNotes(MoveTemp(Result));
		}
	}

	void ExecuteReleaseNotes(FWork& Work)
	{
		FWoTReleaseNotesResult Result;
		Result.ReleaseCommit = Deps.RunningIdentity.ReleaseCommit;
		Result.ReleaseSequence = Deps.RunningIdentity.ReleaseSequence;
		Result.Outcome = EWoTReleaseNotesOutcome::NotAttempted;
		// Only identity, seen and transport gate the notes. The update
		// mechanics deliberately do not: the notify-only reasons, the updater
		// state root, the activation lease and the staged record. The first
		// launch after an update runs with activation-recovery-pending, and
		// read-only or translocated installs never resolve a state root, yet
		// those launches still deserve their notes. A read-only GET touches no
		// update state.
		if (!Deps.RunningIdentity.bStamped
			|| Deps.RunningIdentity.ReleaseSequence < 1
			|| Deps.RunningIdentity.ReleaseCommit.IsEmpty()
			|| Work.bReleaseNotesSeen
			|| CancellableHops == nullptr)
		{
			FinishReleaseNotes(Work, MoveTemp(Result));
			return;
		}

		const double Deadline = Deps.NowSeconds() + 15.0;
		TArray<uint8> Buffered;
		FWoTHttpHopResponse Terminal;
		FReleaseNotesSink Sink;
		FWoTBootstrapStatus Fetched = WoTFetchReleaseObject(
			*CancellableHops, WoTReleaseNotes::LatestUrl, Deadline,
			Deps.NowSeconds, Buffered, Terminal, &Sink);
		if (Sink.bExceeded
			|| Buffered.Num() > static_cast<int32>(WoTReleaseNotes::MaximumFileBytes))
		{
			Result.Outcome = EWoTReleaseNotesOutcome::Invalid;
			FinishReleaseNotes(Work, MoveTemp(Result));
			return;
		}
		if (Fetched.IsError() || Terminal.Status != 200)
		{
			Result.Outcome = EWoTReleaseNotesOutcome::Unavailable;
			FinishReleaseNotes(Work, MoveTemp(Result));
			return;
		}
		const TArray<uint8>& Bytes = Sink.Bytes.IsEmpty() ? Buffered : Sink.Bytes;
		FString SelectedText;
		TWoTBootstrapResult<bool> Selected = WoTReleaseNotes::ParseAndSelect(
			Bytes, Deps.RunningIdentity.ReleaseSequence, SelectedText);
		if (Selected.IsError())
		{
			Result.Outcome = EWoTReleaseNotesOutcome::Invalid;
		}
		else if (Selected.GetValue())
		{
			Result.Outcome = EWoTReleaseNotesOutcome::Selected;
			Result.Text = MoveTemp(SelectedText);
		}
		else
		{
			Result.Outcome = EWoTReleaseNotesOutcome::NoEntry;
		}
		FinishReleaseNotes(Work, MoveTemp(Result));
	}

	void PublishUpdateState(FWork& Work, FWoTUpdateState State)
	{
		FScopeLock DeliveryScope(&DeliveryLock);
		if (!bCancelled.Load() && Work.UpdateState)
		{
			Work.UpdateState(MoveTemp(State));
		}
	}

	void FinishBackgroundUpdate(FWork& Work, FStringView Result,
		FWoTUpdateState State = FWoTUpdateState())
	{
		// The published state rides on the result so a packaged run can tell
		// an Available or KnownBad assessment from a skipped one.
		FString Detail(Result);
		if (State.Status != EWoTUpdateStatus::None)
		{
			Detail += FString::Printf(TEXT(" status=%s sequence=%llu reason=%s"),
				WoTGameBootstrapWorkerUpdateStatusName(State.Status),
				static_cast<unsigned long long>(State.ReleaseSequence),
				*State.Reason);
		}
		WoTRecordBootstrapHeartbeat(TEXT("update-check-result"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, Detail);
		PublishUpdateState(Work, MoveTemp(State));
	}

	void ExecuteBackgroundUpdate(FWork& Work)
	{
		WoTRecordBootstrapHeartbeat(TEXT("update-check-begin"),
			EWoTBootstrapPhase::Ready, 0);
		BackgroundSkipReason.Reset();
		BackgroundSkipDetail.Reset();
		BackgroundPolicyResult.Reset();
		FWoTUpdateState Checking;
		Checking.Status = EWoTUpdateStatus::Checking;
		PublishUpdateState(Work, MoveTemp(Checking));
		if (!Root.IsSet() || CancellableHops == nullptr)
		{
			WoTRecordBootstrapHeartbeat(TEXT("update-check-skipped"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				TEXT("reason=guarded-session-absent"));
			FinishBackgroundUpdate(Work, TEXT("skipped"));
			return;
		}
		if (NotifyOnlyReason == TEXT("rollback-notice-unavailable"))
		{
			FinishBackgroundUpdate(Work, TEXT("rollback-notice-unavailable"));
			return;
		}
		auto Trial = ReadWoTUpdateMarker(Root.GetValue(), TEXT("trial"), true);
		auto ReadyMarker = ReadWoTUpdateMarker(Root.GetValue(), TEXT("ready"));
		if (Trial.IsError() || ReadyMarker.IsError() || Trial.GetValue().IsSet())
		{
			FinishBackgroundUpdate(Work, TEXT("trial-or-marker-unavailable"));
			return;
		}
		if (ReadyMarker.GetValue().IsSet())
		{
			FWoTUpdateState Ready;
			Ready.Status = EWoTUpdateStatus::Ready;
			Ready.ReleaseSequence = ReadyMarker.GetValue()->Sequence;
			Ready.ReleasePageUrl = WoTGameBootstrapWorkerReleasePageUrl(Deps.AnnouncementJsonUrl);
			{
				FScopeLock Lock(&UpdateLock);
				ReadyStaged = MakeUnique<FWoTStagedRelease>();
				ReadyStaged->ReleaseSequence = Ready.ReleaseSequence;
				ReadyStaged->StagedTreeDir = Root->StagedTreeDir;
			}
			FinishBackgroundUpdate(Work, TEXT("ready"), MoveTemp(Ready));
			return;
		}
		Work.Request.Phase = EWoTBootstrapPhase::CheckingGameUpdates;
		Work.Request.OperationEpoch = 0;
		Work.Request.AnnouncementLogicalObjects = {
			Deps.AnnouncementJsonUrl, Deps.AnnouncementJsonUrl + TEXT(".sig")};
		Announcement.Reset();
		ReleaseRing.Reset();
		Manifest.Reset();
		Download.Reset();
		Staged.Reset();
		FWoTBootstrapWorkerCompletion Checked = CheckGameUpdates(
			Work, true);
		if (!Checked.bSucceeded
			|| Checked.NextPhase != EWoTBootstrapPhase::DownloadingUpdate)
		{
			if (!BackgroundSkipReason.IsEmpty())
			{
				FString Detail = TEXT("reason=") + BackgroundSkipReason;
				if (!BackgroundSkipDetail.IsEmpty())
				{
					Detail += TEXT(" detail=") + BackgroundSkipDetail;
				}
				WoTRecordBootstrapHeartbeat(TEXT("update-check-skipped"),
					EWoTBootstrapPhase::Ready, 0, {}, {},
					Detail);
				if (BackgroundPolicyResult.IsSet())
				{
					FinishBackgroundUpdate(Work, TEXT("suppressed"),
						MoveTemp(BackgroundPolicyResult.GetValue()));
					BackgroundPolicyResult.Reset();
				}
				else
				{
					FinishBackgroundUpdate(Work, TEXT("skipped"));
				}
				return;
			}
			if (Announcement.IsSet() && !NotifyOnlyReason.IsEmpty())
			{
				FWoTUpdateState Available;
				Available.Status = NotifyOnlyReason == TEXT("known-bad")
					? EWoTUpdateStatus::KnownBad : EWoTUpdateStatus::Available;
				Available.ReleaseSequence = Announcement->ReleaseSequence;
				Available.Reason = NotifyOnlyReason;
				Available.ReleasePageUrl = WoTGameBootstrapWorkerReleasePageUrl(
					Announcement->Manifest.Url);
				FinishBackgroundUpdate(Work, TEXT("available"), MoveTemp(Available));
			}
			else if (!Checked.bSucceeded)
			{
				// A failed check used to read as "none", so a packaged run could
				// not tell a rejected or unreachable release from no update (a
				// release metadata fetch could fail silently).
				WoTRecordBootstrapHeartbeat(TEXT("update-check-failed"),
					EWoTBootstrapPhase::Ready, 0, {}, {},
					FString::Printf(TEXT("diagnostic=%s%s%s"),
						*FWoTBootstrapViewModel::DiagnosticCodeText(
							Checked.DiagnosticCode).ToString(),
						Checked.FailureDetail.IsEmpty() ? TEXT("") : TEXT(" detail="),
						*Checked.FailureDetail));
				FinishBackgroundUpdate(Work, TEXT("failed"));
			}
			else
			{
				FinishBackgroundUpdate(Work, TEXT("none"));
			}
			return;
		}

		FWoTUpdateState Downloading;
		Downloading.Status = EWoTUpdateStatus::Downloading;
		Downloading.ReleaseSequence = Announcement->ReleaseSequence;
		Downloading.TotalBytes = Manifest->Archive.Size;
		PublishUpdateState(Work, MoveTemp(Downloading));
		double LastProgressHeartbeat = -1.0e30;
		Work.Progress = [this, &Work, &LastProgressHeartbeat](
			FWoTBootstrapProgressEvent&& Event)
		{
			FWoTUpdateState Progress;
			Progress.Status = EWoTUpdateStatus::Downloading;
			Progress.ReleaseSequence = Announcement.IsSet()
				? Announcement->ReleaseSequence : 0;
			Progress.CompletedBytes = Event.CompletedBytes.Get(0);
			Progress.TotalBytes = Event.TotalBytes.Get(0);
			const double Now = Deps.NowSeconds();
			if (Now - LastProgressHeartbeat >= 10.0)
			{
				LastProgressHeartbeat = Now;
				WoTRecordBootstrapHeartbeat(TEXT("update-download-progress"),
					EWoTBootstrapPhase::Ready, 0, Progress.CompletedBytes,
					Progress.TotalBytes);
			}
			PublishUpdateState(Work, MoveTemp(Progress));
		};
		Work.Request.Phase = EWoTBootstrapPhase::DownloadingUpdate;
		FWoTBootstrapWorkerCompletion Downloaded = DownloadUpdate(Work);
		if (!Downloaded.bSucceeded || !Staged.IsValid())
		{
			// Say why: an intact archive whose staging failed read only as
			// "download-failed".
			WoTRecordBootstrapHeartbeat(TEXT("update-check-failed"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				FString::Printf(TEXT("stage=download diagnostic=%s%s%s"),
					*FWoTBootstrapViewModel::DiagnosticCodeText(
						Downloaded.DiagnosticCode).ToString(),
					Downloaded.FailureDetail.IsEmpty()
						? (Downloaded.bSucceeded
							? TEXT(" detail=no staged release") : TEXT(""))
						: TEXT(" detail="),
					*Downloaded.FailureDetail));
			// Tell the player instead of letting the Downloading banner vanish:
			// the current version stays playable and the next start retries.
			FWoTUpdateState Failed;
			Failed.Status = EWoTUpdateStatus::Available;
			Failed.ReleaseSequence = Announcement.IsSet()
				? Announcement->ReleaseSequence : 0;
			Failed.Reason = TEXT("download-failed");
			Failed.ReleasePageUrl = Announcement.IsSet()
				? WoTGameBootstrapWorkerReleasePageUrl(Announcement->Manifest.Url)
				: FString();
			FinishBackgroundUpdate(Work, TEXT("download-failed"), MoveTemp(Failed));
			return;
		}
		FWoTUpdateState Ready;
		Ready.Status = EWoTUpdateStatus::Ready;
		Ready.ReleaseSequence = Staged->ReleaseSequence;
		Ready.ReleasePageUrl = WoTGameBootstrapWorkerReleasePageUrl(Announcement->Manifest.Url);
		const FString StagedDetail = FString::Printf(
			TEXT("sequence=%llu"),
			static_cast<unsigned long long>(Ready.ReleaseSequence));
		WoTRecordBootstrapHeartbeat(TEXT("update-staged"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, StagedDetail);
		{
			FScopeLock Lock(&UpdateLock);
			ReadyStaged = MoveTemp(Staged);
		}
		FinishBackgroundUpdate(Work, TEXT("ready"), MoveTemp(Ready));
	}

	void StartBackgroundUpdate(FWoTUpdateStateCallback&& Callback)
	{
		if (bBackgroundStarted.Exchange(true) || bCancelled.Load())
		{
			return;
		}
		FWork Work;
		Work.Kind = FWork::EKind::BackgroundUpdate;
		Work.UpdateState = MoveTemp(Callback);
		Start(MoveTemp(Work));
	}

	void StartReleaseNotes(bool bSeen, FWoTReleaseNotesCallback&& Callback)
	{
		if (bReleaseNotesStarted.Exchange(true) || bCancelled.Load())
		{
			return;
		}
		FWork Work;
		Work.Kind = FWork::EKind::ReleaseNotes;
		Work.bReleaseNotesSeen = bSeen;
		Work.ReleaseNotes = MoveTemp(Callback);
		Start(MoveTemp(Work));
	}

	bool LaunchReadyUpdate()
	{
		FScopeLock Lock(&UpdateLock);
		if (bApplyLaunched || bCancelled.Load() || !ReadyStaged.IsValid()
			|| !Root.IsSet() || !NotifyOnlyReason.IsEmpty())
		{
			return false;
		}
		bApplyLaunched = true;
		WoTRecordBootstrapHeartbeat(
			TEXT("update-restart"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, TEXT("result=quit-requested"));
		{
			FScopeLock DeliveryScope(&DeliveryLock);
			if (!bCancelled.Load())
			{
				Deps.RequestRestartExit();
			}
		}
		return true;
	}

	FWoTBootstrapWorkerCompletion CheckOriginalGameData(FWork& Work)
	{
		ResetContentOperationState();
		if (!bCacheProbedForCurrentFlow)
		{
			FWoTBootstrapStatus Cache = OpenCurrentCache(Work);
			if (Cache.IsError())
			{
				return CacheFailure(Work, MoveTemp(Cache).GetError());
			}
		}
		if (ValidatedCacheSnapshot.IsSet())
		{
			return Completion(
				Work, EWoTBootstrapPhase::VerifyingContent);
		}
		bOriginalGameTranslocated = RunningTreeTranslocated();
		LoadRememberedInstallerRecord(Work);
		TWoTBootstrapResult<FWoTInstallerSweep> Swept =
			SweepInstallerLocations(Work);
		if (Swept.IsError())
		{
			return Failure(Work, MoveTemp(Swept).GetError());
		}
		FWoTInstallerSweep InstallerSweep = MoveTemp(Swept).GetValue();
		if (!InstallerSweep.Verified.IsValid()
			&& InstallerSweep.bNeedsStabilityPass)
		{
			const uint32 WaitMilliseconds = static_cast<uint32>(FMath::Clamp(
				Deps.OriginalGameProbeIntervalSeconds * 1000.0, 1.0,
				static_cast<double>(MAX_uint32)));
			if (OriginalGameRecheckEvent != nullptr)
			{
				OriginalGameRecheckEvent->Wait(WaitMilliseconds);
			}
			if (bCancelled.Load())
			{
				return Failure(Work,
					EWoTBootstrapDiagnosticCode::BootstrapFailure);
			}
			Swept = SweepInstallerLocations(Work);
			if (Swept.IsError())
			{
				return Failure(Work, MoveTemp(Swept).GetError());
			}
			InstallerSweep = MoveTemp(Swept).GetValue();
		}
		if (InstallerSweep.Verified.IsValid())
		{
			AdoptContentInstaller(InstallerSweep);
			WoTRecordBootstrapHeartbeat(TEXT("original-game-detected"),
				EWoTBootstrapPhase::CheckingOriginalGameData,
				Work.Request.OperationEpoch, {}, {},
				FString::Printf(TEXT("source=%s"),
					InstallerSourceName(InstallerSweep.FoundSource)));
			FWoTBootstrapWorkerCompletion Result = Completion(
				Work, EWoTBootstrapPhase::ExtractingOriginalGame);
			Result.NextPhaseDiskBudget.CacheConstructionBytes =
				WoTBootstrapCacheConstructionBudgetBytes;
			return Result;
		}

		OriginalGameNeededReason = InstallerSweep.Reason;
		OriginalGameNeededDetail = InstallerSweep.Detail;
		bAcceptPickedInstaller.Store(true);
		FWoTBootstrapWorkerCompletion Result = Completion(
			Work, EWoTBootstrapPhase::OriginalGameNeeded);
		Result.bMandatoryWork = true;
		Result.OriginalGameNeededReason = InstallerSweep.Reason;
		Result.OriginalGameNeededDetail = InstallerSweep.Detail;
		return Result;
	}

	/** Takes the sweep's held installer for extraction and notes a Browse pick,
	 * which is remembered only once the cache it builds has been published. */
	void AdoptContentInstaller(FWoTInstallerSweep& InstallerSweep)
	{
		ContentInstaller = MoveTemp(InstallerSweep.Verified);
		PickedInstallerPathToRemember =
			InstallerSweep.FoundSource == EWoTInstallerSource::Picked
				? InstallerSweep.FoundCanonicalPath : FString();
	}

	void PublishOriginalGameNeededState(FWork& Work,
		EWoTOriginalGameNeededReason Reason, const FString& Detail)
	{
		OriginalGameNeededReason = Reason;
		OriginalGameNeededDetail = Detail;
		if (!Work.Progress || bCancelled.Load())
		{
			return;
		}
		FWoTBootstrapProgressEvent Event;
		Event.OperationEpoch = Work.Request.OperationEpoch;
		Event.ElapsedSeconds = FMath::Max(0.0,
			Deps.NowSeconds() - Work.PhaseStartedSeconds);
		Event.OriginalGameNeededReason = Reason;
		Event.OriginalGameNeededDetail = Detail;
		Work.Progress(MoveTemp(Event));
	}

	FWoTBootstrapWorkerCompletion OriginalGameNeeded(FWork& Work)
	{
		WoTRecordBootstrapHeartbeat(TEXT("original-game-needed"),
			EWoTBootstrapPhase::OriginalGameNeeded,
			Work.Request.OperationEpoch, {}, {},
			FString::Printf(TEXT("reason=%s translocated=%d downloads=%d"),
				LexToString(OriginalGameNeededReason),
				bOriginalGameTranslocated ? 1 : 0,
				Deps.DownloadsDirectory.IsEmpty() ? 0 : 1));
		PublishOriginalGameNeededState(Work, OriginalGameNeededReason,
			OriginalGameNeededDetail);
		bool bFirstProbe = true;
		while (!bCancelled.Load())
		{
			if (!bFirstProbe && OriginalGameRecheckEvent != nullptr)
			{
				const uint32 WaitMilliseconds = static_cast<uint32>(FMath::Clamp(
					Deps.OriginalGameProbeIntervalSeconds * 1000.0, 1.0,
					static_cast<double>(MAX_uint32)));
				OriginalGameRecheckEvent->Wait(WaitMilliseconds);
			}
			bFirstProbe = false;
			if (bCancelled.Load()) break;
			TWoTBootstrapResult<FWoTInstallerSweep> Swept =
				SweepInstallerLocations(Work);
			if (Swept.IsError())
			{
				bAcceptPickedInstaller.Store(false);
				return Failure(Work, MoveTemp(Swept).GetError());
			}
			FWoTInstallerSweep InstallerSweep = MoveTemp(Swept).GetValue();
			PublishOriginalGameNeededState(Work, InstallerSweep.Reason,
				InstallerSweep.Detail);
			if (InstallerSweep.Verified.IsValid())
			{
				bAcceptPickedInstaller.Store(false);
				AdoptContentInstaller(InstallerSweep);
				WoTRecordBootstrapHeartbeat(TEXT("original-game-detected"),
					EWoTBootstrapPhase::OriginalGameNeeded,
					Work.Request.OperationEpoch, {}, {},
					FString::Printf(TEXT("source=%s"),
						InstallerSourceName(InstallerSweep.FoundSource)));
				FWoTBootstrapWorkerCompletion Result = Completion(
					Work, EWoTBootstrapPhase::ExtractingOriginalGame);
				Result.NextPhaseDiskBudget.CacheConstructionBytes =
					WoTBootstrapCacheConstructionBudgetBytes;
				return Result;
			}
		}
		bAcceptPickedInstaller.Store(false);
		return Failure(Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
	}

	void ResetContentOperationState()
	{
		ContentOperation.Reset();
		ContentCache.Reset();
		ContentInstaller.Reset();
		ContentPackage.Reset();
		bAcceptPickedInstaller.Store(false);
		AdjacentInstaller.PreviousCandidateIdentity.Reset();
		AdjacentInstaller.KnownWrongIdentity.Reset();
		for (FTrackedInstallerLocation& Downloads : DownloadsInstallers)
		{
			Downloads.PreviousCandidateIdentity.Reset();
			Downloads.KnownWrongIdentity.Reset();
		}
		PickedInstaller.Reset();
		RememberedInstaller.Reset();
		RememberedInstallerRecordPath.Reset();
		bRememberedInstallerRejected = false;
		PickedInstallerPathToRemember.Reset();
		RecordedInstallerUnavailable.Reset();
		FString DiscardedPickedPath;
		while (PickedInstallerRequests.Dequeue(DiscardedPickedPath)) {}
	}

	FWoTBootstrapWorkerCompletion ExtractOriginalGame(FWork& Work)
	{
		ContentOperation.Reset();
		ContentCache.Reset();
		ContentPackage.Reset();
		PublishedCacheReceipt.Reset();
		if (!ContentInstaller.IsValid())
		{
			TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult> Acquired =
				AcquireAdjacentInstaller();
			if (Acquired.IsError())
			{
				return Failure(Work, MoveTemp(Acquired).GetError());
			}
			FWoTPinnedAdjacentInstallerResult Adjacent =
				MoveTemp(Acquired).GetValue();
			if (Adjacent.State != EWoTPinnedAdjacentInstallerProbe::Valid
				|| !Adjacent.Verified.IsValid())
			{
				return InvalidInstallerFailure(Work);
			}
			ContentInstaller = MoveTemp(Adjacent.Verified);
			PickedInstallerPathToRemember.Reset();
		}
		TWoTBootstrapResult<FWoTRetailPackage> Package = FWoTRetailPackage::Open(
			MoveTemp(ContentInstaller->Snapshot), FWoTArchiveLimits());
		ContentInstaller.Reset();
		if (Package.IsError())
		{
			return InvalidInstallerFailure(Work);
		}
		TWoTBootstrapResult<FWoTRetailContract> Contract =
			WoTLoadProductionRetailContract();
		if (Contract.IsError())
		{
			return Failure(Work, MoveTemp(Contract).GetError());
		}
		if (FWoTBootstrapStatus Identity =
			Package.GetValue().ReverifyHeldSnapshotAfterExtraction();
			Identity.IsError())
		{
			return InvalidInstallerFailure(Work);
		}
		TSet<FString> Sources;
		for (const FWoTRequiredFact& Fact : Contract.GetValue().Facts)
		{
			Sources.Add(Fact.SourcePath);
		}
		for (const FString& Source : Sources)
		{
			if (Package.GetValue().FindExact(Source).IsError())
			{
				return InvalidInstallerFailure(Work);
			}
		}
		FWoTBootstrapStatus Reverified =
			Package.GetValue().ReverifyHeldSnapshotAfterExtraction();
		if (Reverified.IsError())
		{
			return InvalidInstallerFailure(Work);
		}
		TWoTBootstrapResult<FWoTCacheStore> OpenedCache = OpenCacheStore();
		if (OpenedCache.IsError())
		{
			return CacheFailure(Work, MoveTemp(OpenedCache).GetError());
		}
		ContentPackage = MakeUnique<FWoTRetailPackage>(
			MoveTemp(Package).GetValue());
		ContentCache = MakeUnique<FWoTCacheStore>(
			MoveTemp(OpenedCache).GetValue());
#if WITH_AUTOMATION_TESTS
		if (Deps.CacheBuildForTesting)
		{
			ContentOperation = MakeUnique<FWoTCacheBootstrapOperation>(
				Deps.CacheBuildForTesting);
		}
		else
#endif
		{
			ContentOperation = MakeUnique<FWoTCacheBootstrapOperation>();
		}
		FWoTBootstrapWorkerCompletion Result =
			Completion(Work, EWoTBootstrapPhase::BuildingContent);
		Result.NextPhaseDiskBudget.CacheConstructionBytes =
			WoTBootstrapCacheConstructionBudgetBytes;
		Result.bMandatoryWork = true;
		return Result;
	}

	FWoTBootstrapWorkerCompletion BuildContent(FWork& Work)
	{
		if (!ContentPackage.IsValid() || !ContentCache.IsValid()
			|| !ContentOperation.IsValid())
		{
			ResetContentOperationState();
			return Failure(Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
		}
		FWoTGameBootstrapCacheProgress Progress(
			Work.Progress, Work.Request.OperationEpoch, bCancelled,
			Work.PhaseStartedSeconds, Deps.NowSeconds);
		// Reset below clears the flow's installer state; keep the Browse pick
		// (if this build came from one) until publication has succeeded.
		const FString PickedPathToRemember =
			MoveTemp(PickedInstallerPathToRemember);
		TWoTBootstrapResult<FWoTPublishedCacheReceipt> Published =
			ContentOperation->BuildAndPublish(
				MoveTemp(*ContentPackage), *ContentCache, Progress);
		ResetContentOperationState();
		if (WoTGetProcessProfileSession() == nullptr)
		{
#if WITH_AUTOMATION_TESTS
			if (GIsAutomationTesting)
			{
				const FWoTBootstrapError LostSession = MakeWoTBootstrapError(
					EWoTBootstrapError::UnsafePath,
					TEXT("cache construction could not restore the process profile session"));
				return CacheFailure(Work, LostSession);
			}
#endif
			// Deferred shutdown could write profile children after another
			// process wins the exclusive lock. Once the shared hold is lost,
			// terminate before any later game or teardown work can run.
			std::_Exit(EXIT_FAILURE);
		}
		if (Published.IsError())
		{
			return CacheFailure(Work, MoveTemp(Published).GetError());
		}
		PublishedCacheReceipt = MoveTemp(Published).GetValue();
		if (!PickedPathToRemember.IsEmpty())
		{
			RememberPickedInstaller(Work, PickedPathToRemember);
		}
		return Completion(Work, EWoTBootstrapPhase::VerifyingContent);
	}

	FWoTBootstrapWorkerCompletion VerifyContent(FWork& Work)
	{
		if (!ValidatedCacheSnapshot.IsSet())
		{
			FWoTBootstrapStatus Cache = OpenCurrentCache(Work);
			if (Cache.IsError())
			{
				PublishedCacheReceipt.Reset();
				return CacheFailure(Work, MoveTemp(Cache).GetError());
			}
		}
		if (!ValidatedCacheSnapshot.IsSet())
		{
			PublishedCacheReceipt.Reset();
			return Failure(
				Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
		}
		if (PublishedCacheReceipt.IsSet())
		{
			const FWoTPublishedCacheReceipt& Receipt =
				PublishedCacheReceipt.GetValue();
			const FWoTCacheGenerationBuildIdentity& Identity =
				ValidatedCacheSnapshot->GetBuildIdentity();
			if (ValidatedCacheSnapshot->GetManifestSha256()
					!= Receipt.ManifestSha256
				|| Identity.InstallerSha256 != Receipt.InstallerSha256
				|| Identity.ContractSha256 != Receipt.ContractSha256
				|| Identity.ProductionDescriptorSha256
					!= Receipt.ProductionDescriptorSha256)
			{
				PublishedCacheReceipt.Reset();
				ValidatedCacheSnapshot.Reset();
				return Failure(
					Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
			}
		}
		PublishedCacheReceipt.Reset();
		return Completion(Work, EWoTBootstrapPhase::StartingGame);
	}

	FWoTBootstrapWorkerCompletion StartGame(FWork& Work)
	{
		if (!ValidatedCacheSnapshot.IsSet()
			|| !ValidatedCacheSnapshot->IsValid())
		{
			ResetCacheActivationState();
			return Failure(
				Work, EWoTBootstrapDiagnosticCode::BootstrapFailure);
		}
		FWoTBootstrapWorkerCompletion Result =
			Completion(Work, EWoTBootstrapPhase::Ready);
		Result.CacheSnapshot.Emplace(
			MoveTemp(ValidatedCacheSnapshot.GetValue()));
		ValidatedCacheSnapshot.Reset();
		return Result;
	}

	void ResetCacheActivationState()
	{
		ValidatedCacheSnapshot.Reset();
		bCacheProbedForCurrentFlow = false;
	}

	bool PersistVerifiedResumeState() const
	{
		return bRecoveryClean.Load();
	}

	FDependencies Deps;
	FEvent* WorkEvent = nullptr;
	FEvent* OriginalGameRecheckEvent = nullptr;
	FCriticalSection ThreadLock;
	// Serializes whole-teardown (CancelAndJoin) and gates callback/exit
	// delivery against the cancel store, respectively.
	FCriticalSection CancelLock;
	FCriticalSection DeliveryLock;
	FCriticalSection UpdateLock;
	FRunnableThread* Thread = nullptr;
	TQueue<TUniquePtr<FWork>, EQueueMode::Mpsc> WorkQueue;
	TAtomic<bool> bCancelled{false};
	TAtomic<bool> bRecoveryClean{false};
	TAtomic<bool> bBackgroundStarted{false};
	TAtomic<bool> bReleaseNotesStarted{false};
	bool bApplyLaunched = false;
	TUniquePtr<FCancellableHops> CancellableHops;

	TOptional<FWoTUpdaterStateRoot> Root;
	FWoTUpdateStateRecord UpdateStateRecord;
	TOptional<FWoTAnnouncementV1> Announcement;
	FString NotifyOnlyReason;
	FString NotifyOnlyDetail;
	FString BackgroundSkipReason;
	FString BackgroundSkipDetail;
	TOptional<FWoTUpdateState> BackgroundPolicyResult;
	TOptional<FWoTKeyRing> ReleaseRing;
	TOptional<FWoTReleaseManifestV2> Manifest;
	TUniquePtr<FWoTGameReleaseDownload> Download;
	TUniquePtr<FWoTStagedRelease> Staged;
	TUniquePtr<FWoTStagedRelease> ReadyStaged;
	FWoTDiskBudgetInputs DiskBudget;

	// One acquired handle survives Check/Download -> Extract, then the package
	// holding that same handle survives Extract -> Build. Verification receives
	// only the immutable publication receipt.
	TUniquePtr<FWoTVerifiedInstaller> ContentInstaller;
	EWoTOriginalGameNeededReason OriginalGameNeededReason =
		EWoTOriginalGameNeededReason::Missing;
	FString OriginalGameNeededDetail;
	bool bOriginalGameTranslocated = false;
	TAtomic<bool> bAcceptPickedInstaller{false};
	FTrackedInstallerLocation AdjacentInstaller;
	TArray<FTrackedInstallerLocation> DownloadsInstallers;
	TOptional<FWoTNativeFileIdentity> DownloadsParentIdentity;
	TOptional<FTrackedInstallerLocation> PickedInstaller;
	TQueue<FString, EQueueMode::Mpsc> PickedInstallerRequests;
	/** Well-formed path read from the remembered-installer record this flow. */
	FString RememberedInstallerRecordPath;
	/** The remembered location once it has canonicalized to its record. */
	TOptional<FTrackedInstallerLocation> RememberedInstaller;
	/** Skips the remembered location for the rest of this flow. */
	bool bRememberedInstallerRejected = false;
	/** Canonical path of the Browse pick feeding this flow's build, if any. */
	FString PickedInstallerPathToRemember;
	TSet<FString> RecordedInstallerUnavailable;
	TUniquePtr<FWoTRetailPackage> ContentPackage;
	TUniquePtr<FWoTCacheStore> ContentCache;
	TUniquePtr<FWoTCacheBootstrapOperation> ContentOperation;
	TOptional<FWoTPublishedCacheReceipt> PublishedCacheReceipt;
	TOptional<FWoTCacheGenerationLease> ValidatedCacheSnapshot;
	bool bCacheProbedForCurrentFlow = false;

	FWoTBootstrapProgressCallback* ActiveDownloadProgress = nullptr;
	uint64 ActiveDownloadEpoch = 0;
	uint64 ActiveDownloadTotal = 0;
	double ActiveDownloadPhaseStartedSeconds = 0.0;
};

FWoTGameBootstrapWorker::FWoTGameBootstrapWorker(
	FDependencies&& InDeps)
	: Impl(MakeUnique<FImpl>(MoveTemp(InDeps)))
{
}

FWoTGameBootstrapWorker::~FWoTGameBootstrapWorker()
{
	CancelAndJoin();
}

void FWoTGameBootstrapWorker::StartPhase(
	const FWoTBootstrapWorkerRequest& Request,
	FWoTBootstrapProgressCallback&& Progress,
	FWoTBootstrapCompletionCallback&& Completion)
{
	if (!Impl.IsValid())
	{
		return;
	}
	FImpl::FWork Work;
	Work.Request = Request;
	Work.Progress = MoveTemp(Progress);
	Work.Completion = MoveTemp(Completion);
	Impl->Start(MoveTemp(Work));
}

bool FWoTGameBootstrapWorker::PersistVerifiedResumeState()
{
	return Impl.IsValid() && Impl->PersistVerifiedResumeState();
}

void FWoTGameBootstrapWorker::StartReleaseNotes(bool bSeen,
	FWoTReleaseNotesCallback&& Callback)
{
	if (Impl.IsValid())
	{
		Impl->StartReleaseNotes(bSeen, MoveTemp(Callback));
	}
}

void FWoTGameBootstrapWorker::StartBackgroundUpdate(
	FWoTUpdateStateCallback&& Callback)
{
	if (Impl.IsValid())
	{
		Impl->StartBackgroundUpdate(MoveTemp(Callback));
	}
}

bool FWoTGameBootstrapWorker::RequestRestartToUpdate()
{
	return Impl.IsValid() && Impl->LaunchReadyUpdate();
}

void FWoTGameBootstrapWorker::RequestOriginalGameRecheck()
{
	if (Impl.IsValid())
	{
		Impl->RequestOriginalGameRecheck();
	}
}

void FWoTGameBootstrapWorker::RequestPickedInstaller(FString CanonicalPath)
{
	if (Impl.IsValid())
	{
		Impl->RequestPickedInstaller(MoveTemp(CanonicalPath));
	}
}

#if WITH_AUTOMATION_TESTS
void FWoTGameBootstrapWorker::SetRunningReleaseSequenceForTesting(
	uint64 ReleaseSequence)
{
	if (Impl.IsValid())
	{
		Impl->Deps.RunningIdentity.ReleaseSequence = ReleaseSequence;
		Impl->Deps.RunningIdentity.ReleaseCommit = FString::Printf(
			TEXT("worker-spec-release-%llu"),
			static_cast<unsigned long long>(ReleaseSequence));
	}
}

void FWoTGameBootstrapWorker::SetRunningIdentityForTesting(bool bStamped,
	uint64 ReleaseSequence, FString ReleaseCommit)
{
	if (Impl.IsValid())
	{
		Impl->Deps.RunningIdentity.bStamped = bStamped;
		Impl->Deps.RunningIdentity.ReleaseSequence = ReleaseSequence;
		Impl->Deps.RunningIdentity.ReleaseCommit = MoveTemp(ReleaseCommit);
	}
}

void FWoTGameBootstrapWorker::SetInitialCacheSnapshotForTesting(
	FWoTCacheGenerationLease&& Snapshot)
{
	if (Impl.IsValid())
	{
		Impl->Deps.InitialCacheSnapshotForTesting.Emplace(MoveTemp(Snapshot));
	}
}
#endif

void FWoTGameBootstrapWorker::CancelAndJoin()
{
	if (Impl.IsValid())
	{
		Impl->CancelAndJoin();
	}
}

TUniquePtr<IWoTBootstrapWorker> WoTCreateProductionBootstrapWorker(
	const FWoTResolvedExecutableLayout& Layout)
{
	if (!Layout.IsValid())
	{
		return nullptr;
	}
	TUniquePtr<IWoTHttpHopExecutor> Hops =
		WoTCreatePlatformHttpHopExecutor();
	if (!Hops.IsValid())
	{
		return nullptr;
	}
	FWoTGameBootstrapWorker::FDependencies Deps;
	Deps.Hops = MoveTemp(Hops);
	Deps.UserStateRoot = Layout.UserStateRoot;
	Deps.UserStateRootIdentity = Layout.UserStateRootIdentity;
	Deps.bHasUserStateRootIdentity = Layout.bHasUserStateRootIdentity;
	Deps.InstallRoot = Layout.InstallRoot;
	Deps.AdjacentInstallerParentIdentity =
		Layout.AdjacentInstallerParentIdentity;
	Deps.bHasAdjacentInstallerParentIdentity =
		Layout.bHasAdjacentInstallerParentIdentity;
	Deps.InstallRootIdentity = Layout.InstallRootIdentity;
	Deps.bHasInstallRootIdentity = Layout.bHasInstallRootIdentity;
	Deps.LiveLeaf = Layout.LiveLeaf;
	Deps.InstalledTreeRoot = Layout.InstalledTreeRoot;
	Deps.CanonicalAdjacentInstallerPath =
		Layout.CanonicalAdjacentInstallerPath;
	Deps.DownloadsDirectory = WoTResolveDownloadsDirectory();
	Deps.ContentCacheRoot = Layout.ContentCacheRoot;
	Deps.UpdateDisableReason = Layout.UpdateDisableReason;
	Deps.AnnouncementJsonUrl =
		WoT::Bootstrap::Trust::AnnouncementUrl;
	Deps.RunningIdentity = WoTReleaseIdentity::LoadRunning();
	return MakeUnique<FWoTGameBootstrapWorker>(MoveTemp(Deps));
}

TUniquePtr<IWoTFreeSpaceProbe> WoTCreateProductionFreeSpaceProbe(
	const FWoTResolvedExecutableLayout& Layout)
{
	if (!Layout.IsValid())
	{
		return nullptr;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> UserState =
		OpenNativeDirectoryNoFollow(Layout.UserStateRoot);
	if (UserState.IsError()
		|| (Layout.bHasUserStateRootIdentity
			&& !UserState.GetValue().GetIdentity().IsSameObject(
				Layout.UserStateRootIdentity)))
	{
		return nullptr;
	}
	bool bStateCreated = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> State =
		OpenOrCreateNativeDirectoryAtNoFollow(
			UserState.GetValue(), TEXT("UpdaterState"), bStateCreated);
	if (State.IsError())
	{
		return nullptr;
	}
	(void)bStateCreated;
	return MakeUnique<FWoTGameBootstrapFreeSpaceProbe>(
		MoveTemp(State).GetValue());
}

FString WoTResolveInstallRootFromExecutable()
{
	return WoTResolveExecutableLayout().InstallRoot;
}

FString WoTResolveUserStateRoot()
{
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Root =
		WoTReopenResolvedUserStateRoot();
	return Root.IsError() ? FString() : MoveTemp(Root).GetValue().CanonicalPath;
}

FWoTResolvedExecutableLayout WoTResolveExecutableLayoutFromPath(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot)
{
	return WoTGameBootstrapWorkerResolveExecutableLayout(
		ExecutablePath, DefaultUserStateRoot);
}

FWoTResolvedExecutableLayout WoTResolveExecutableLayout()
{
	return WoTResolveExecutableLayoutForProfileSession(
		WoTGameBootstrapWorkerDefaultUserStateRoot());
}

FString WoTResolveProfileSessionRootForStartup()
{
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	FString HarnessRoot;
	if (FParse::Value(FCommandLine::Get(), TEXT("wottestinstallroot="),
			HarnessRoot))
	{
		HarnessRoot = FPaths::ConvertRelativePathToFull(MoveTemp(HarnessRoot));
		FPaths::NormalizeDirectoryName(HarnessRoot);
		return !FPaths::IsRelative(HarnessRoot)
			&& FPaths::CollapseRelativeDirectories(HarnessRoot)
				? HarnessRoot : FString();
	}
#endif
#if PLATFORM_MAC
	// ApplePlatformProcess appends its historical "Epic" component to the same
	// NSSearchPath Application Support directory used by WoTUserDataRoot. Remove
	// that fixed component without invoking the engine resolver, which creates all
	// five profile leaves and therefore cannot run before the session gate.
	FString ApplicationSupport = FPlatformProcess::UserSettingsDir();
	FPaths::NormalizeDirectoryName(ApplicationSupport);
	if (ApplicationSupport.IsEmpty())
	{
		return FString();
	}
	if (FPaths::GetCleanFilename(ApplicationSupport).Equals(
			TEXT("Epic"), ESearchCase::CaseSensitive))
	{
		ApplicationSupport = FPaths::GetPath(ApplicationSupport);
		FPaths::NormalizeDirectoryName(ApplicationSupport);
	}
	FString Root = FPaths::Combine(
		ApplicationSupport, WoTUserDataRootLeafName());
	Root = FPaths::ConvertRelativePathToFull(MoveTemp(Root));
	FPaths::NormalizeDirectoryName(Root);
	return !Root.IsEmpty() && !FPaths::IsRelative(Root)
		&& FPaths::CollapseRelativeDirectories(Root) ? Root : FString();
#else
	return WoTUserDataRoot();
#endif
}

FWoTResolvedExecutableLayout WoTResolveExecutableLayoutForProfileSession(
	FStringView UserStateRoot)
{
	FString PathResolutionFailureReason;
	TWoTBootstrapResult<FWoTNativeExecutableIdentity> RunningImage =
		OpenCurrentExecutableIdentityNoFollow(&PathResolutionFailureReason);
	if (RunningImage.IsError())
	{
		return FWoTResolvedExecutableLayout{};
	}
	FWoTResolvedExecutableLayout Layout = WoTResolveExecutableLayoutFromPath(
			RunningImage.GetValue().GetReportedPath(),
			UserStateRoot);
	if (Layout.IsValid() && !PathResolutionFailureReason.IsEmpty())
	{
		Layout.UpdateDisableReason = MoveTemp(PathResolutionFailureReason);
	}
	return Layout;
}

void WoTAdoptResolvedExecutableLayout(
	const FWoTResolvedExecutableLayout& Layout)
{
	FScopeLock Lock(&GWoTResolvedUserStateRootLock);
	GWoTHasResolvedUserStateRoot = Layout.IsValid()
		&& Layout.bHasUserStateRootIdentity;
	GWoTResolvedUserStateRootPath = GWoTHasResolvedUserStateRoot
		? Layout.UserStateRoot : FString();
	GWoTResolvedUserStateRootIdentity = GWoTHasResolvedUserStateRoot
		? Layout.UserStateRootIdentity : FWoTNativeFileIdentity{};
}

TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
WoTReopenResolvedUserStateRoot()
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalNativeDirectory>;
	FString Path;
	FWoTNativeFileIdentity ExpectedIdentity;
	{
		FScopeLock Lock(&GWoTResolvedUserStateRootLock);
		if (!GWoTHasResolvedUserStateRoot)
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the canonical user state root was not adopted")));
		}
		Path = GWoTResolvedUserStateRootPath;
		ExpectedIdentity = GWoTResolvedUserStateRootIdentity;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened =
		OpenNativeDirectoryNoFollow(Path);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	if (!Opened.GetValue().GetIdentity().IsSameObject(ExpectedIdentity))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the canonical user state root identity changed"), Path));
	}
	FWoTCanonicalNativeDirectory Result;
	Result.CanonicalPath = MoveTemp(Path);
	Result.Directory = MoveTemp(Opened).GetValue();
	return FResult::Success(MoveTemp(Result));
}

FString WoTDefaultUserStateRootPathForDiagnostics()
{
	return WoTGameBootstrapWorkerDefaultUserStateRoot();
}

FString WoTResolveInstalledTreeRootFromExecutablePath(
	FStringView ExecutablePath)
{
	return WoTResolveExecutableLayoutFromPath(
		ExecutablePath,
		WoTGameBootstrapWorkerDefaultUserStateRoot()).InstalledTreeRoot;
}

FString WoTResolveInstalledTreeRootFromExecutable()
{
	return WoTResolveExecutableLayout().InstalledTreeRoot;
}

FString WoTResolveProductionContentCacheRoot()
{
	const FString UserStateRoot = WoTResolveUserStateRoot();
	if (UserStateRoot.IsEmpty())
	{
		return FString();
	}
	TWoTBootstrapResult<FWoTUpdaterStateRoot> Root =
		ResolveUpdaterUserStateRoot(UserStateRoot);
	return Root.IsError() ? FString()
		: FPaths::Combine(Root.GetValue().UserStateRoot, TEXT("ContentCache"));
}

FString WoTResolveProductionContentCacheRoot(
	const FWoTResolvedExecutableLayout& Layout)
{
	return Layout.IsValid() ? Layout.ContentCacheRoot : FString();
}

#if WITH_AUTOMATION_TESTS
FString WoTGameBootstrapWorkerSpecResolveInstallRootFromExecutablePath(
	FStringView ExecutablePath,
	FStringView DefaultUserStateRoot)
{
	return WoTGameBootstrapWorkerResolveExecutableLayout(
		ExecutablePath, DefaultUserStateRoot).InstallRoot;
}

FString WoTGameBootstrapWorkerSpecResolveProductionContentCacheRootFromExecutablePath(
	FStringView ExecutablePath,
	FStringView DefaultUserStateRoot)
{
	return WoTGameBootstrapWorkerResolveProductionContentCacheRoot(
		ExecutablePath, DefaultUserStateRoot);
}

FWoTResolvedExecutableLayout
WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithNativeTreePath(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot,
	FStringView NativeTreePath, bool bFailNameQuery,
	bool bRunningTreeTranslocated)
{
	const FString InjectedPath(NativeTreePath);
	return WoTGameBootstrapWorkerResolveExecutableLayout(
		ExecutablePath, DefaultUserStateRoot,
		[InjectedPath, bFailNameQuery](const FWoTNativeDirectoryHandle&)
			-> TWoTBootstrapResult<FString>
		{
			if (bFailNameQuery)
			{
				return TWoTBootstrapResult<FString>::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("injected native live-tree name query failure")));
			}
			return TWoTBootstrapResult<FString>::Success(InjectedPath);
			}, false, true,
			[bRunningTreeTranslocated](const FString&)
			{
				return bRunningTreeTranslocated;
			});
}

FWoTResolvedExecutableLayout
WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithReboundNativeTreePath(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot,
	FStringView NativeTreePath)
{
	const FString InjectedPath(NativeTreePath);
	return WoTGameBootstrapWorkerResolveExecutableLayout(
		ExecutablePath, DefaultUserStateRoot,
		[InjectedPath](const FWoTNativeDirectoryHandle&)
			-> TWoTBootstrapResult<FString>
		{
			return TWoTBootstrapResult<FString>::Success(InjectedPath);
		});
}

FWoTResolvedExecutableLayout
WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithIdentityMismatch(
	FStringView ExecutablePath, FStringView DefaultUserStateRoot)
{
	return WoTGameBootstrapWorkerResolveExecutableLayout(
		ExecutablePath, DefaultUserStateRoot, {}, true);
}

void WoTGameBootstrapWorkerSpecResetLayoutResolutionCount()
{
	GWoTExecutableLayoutResolutionCount.Store(0);
}

int32 WoTGameBootstrapWorkerSpecLayoutResolutionCount()
{
	return GWoTExecutableLayoutResolutionCount.Load();
}

void WoTGameBootstrapWorkerSpecResetAdjacentParentOpenCount()
{
	GWoTAdjacentParentOpenCount.Store(0);
}

int32 WoTGameBootstrapWorkerSpecAdjacentParentOpenCount()
{
	return GWoTAdjacentParentOpenCount.Load();
}
#endif
