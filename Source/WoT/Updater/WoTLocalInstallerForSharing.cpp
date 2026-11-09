#include "WoTLocalInstallerForSharing.h"

#include "Misc/Paths.h"
#include "WoTGameBootstrapWorker.h"
#include "WoTInstallerLocations.h"
#include "WoTRememberedInstaller.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
TWoTBootstrapResult<FWoTAdjacentInstallerInspection> WoTSharingInspect(
	FWoTInstallerLocation& Location, const FWoTLocalInstallerSearch& Search)
{
#if WITH_AUTOMATION_TESTS
	if (Search.PinsForTesting.IsSet())
	{
		return WoTInspectInstallerLocationForTesting(
			Location, Search.PinsForTesting.GetValue());
	}
#else
	(void)Search;
#endif
	return WoTInspectInstallerLocation(Location);
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection> WoTSharingInspectAtParent(
	FWoTInstallerLocation& Location, const FWoTHeldInstallerParent& Parent,
	const FWoTLocalInstallerSearch& Search)
{
#if WITH_AUTOMATION_TESTS
	if (Search.PinsForTesting.IsSet())
	{
		return WoTInspectInstallerLocationAtParentForTesting(
			Location, Parent, Search.PinsForTesting.GetValue());
	}
#else
	(void)Search;
#endif
	return WoTInspectInstallerLocationAtParent(Location, Parent);
}

/** Only a complete-looking, singly linked, fully local file counts. Any
 * error (a missing folder, a changed parent) just skips the location. */
TOptional<FWoTLocalInstallerForSharing> WoTSharingCandidate(
	EWoTSharingInstallerSource Source, const FWoTInstallerLocation& Location,
	const TWoTBootstrapResult<FWoTAdjacentInstallerInspection>& Inspected)
{
	if (Inspected.IsError()
		|| Inspected.GetValue().State
			!= EWoTPinnedAdjacentInstallerProbe::Candidate
		|| !Inspected.GetValue().Identity.IsSet())
	{
		return {};
	}
	FWoTLocalInstallerForSharing Found;
	Found.Source = Source;
	Found.Location = Location;
	Found.Identity = Inspected.GetValue().Identity.GetValue();
	return Found;
}

bool WoTSharingIsUnder(const FString& Path, const FString& Folder)
{
	if (Folder.IsEmpty())
	{
		return false;
	}
	FString NormalizedPath = Path;
	FString NormalizedFolder = Folder;
	FPaths::NormalizeFilename(NormalizedPath);
	FPaths::NormalizeDirectoryName(NormalizedFolder);
	return NormalizedPath.StartsWith(NormalizedFolder + TEXT("/"),
		ESearchCase::IgnoreCase);
}
} // namespace

const TCHAR* LexToString(EWoTSharingInstallerSource Source)
{
	switch (Source)
	{
	case EWoTSharingInstallerSource::Remembered:
		return TEXT("remembered");
	case EWoTSharingInstallerSource::Received:
		return TEXT("received");
	case EWoTSharingInstallerSource::Downloads:
		return TEXT("downloads");
	case EWoTSharingInstallerSource::Adjacent:
	default:
		return TEXT("adjacent");
	}
}

FWoTLocalInstallerSearch WoTProductionLocalInstallerSearch(
	TOptional<FWoTInstallerLocation> Adjacent)
{
	FWoTLocalInstallerSearch Search;
	Search.Adjacent = MoveTemp(Adjacent);
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Root =
		WoTReopenResolvedUserStateRoot();
	if (!Root.IsError())
	{
		Search.UserStateRoot = Root.GetValue().CanonicalPath;
		Search.UserStateRootIdentity = Root.GetValue().Directory.GetIdentity();
	}
#if PLATFORM_MAC
	// A path lookup only: Downloads itself is never opened here.
	Search.SkipRememberedUnder = WoTPlatformDownloadsDirectory();
#else
	Search.DownloadsDirectory = WoTResolveDownloadsDirectory();
#endif
	return Search;
}

TOptional<FWoTLocalInstallerForSharing> WoTFindLocalInstallerForSharing(
	const FWoTLocalInstallerSearch& Search)
{
	if (Search.Adjacent.IsSet() && !Search.Adjacent->CanonicalPath.IsEmpty())
	{
		FWoTInstallerLocation Location = Search.Adjacent.GetValue();
		Location.Source = EWoTInstallerSource::Adjacent;
		Location.bRequirePinnedName = true;
		if (TOptional<FWoTLocalInstallerForSharing> Found = WoTSharingCandidate(
				EWoTSharingInstallerSource::Adjacent, Location,
				WoTSharingInspect(Location, Search)))
		{
			return Found;
		}
	}
	if (!Search.UserStateRoot.IsEmpty())
	{
		const FWoTNativeFileIdentity* RootIdentity =
			Search.UserStateRootIdentity.IsSet()
				? &Search.UserStateRootIdentity.GetValue() : nullptr;
		const FWoTRememberedInstallerRead Read =
			WoTReadRememberedInstaller(Search.UserStateRoot, RootIdentity);
		if (Read.CanonicalInstallerPath.IsSet())
		{
			const FString& Recorded = Read.CanonicalInstallerPath.GetValue();
			const bool bSkipped = (Search.Adjacent.IsSet()
					&& FPaths::IsSamePath(Search.Adjacent->CanonicalPath, Recorded))
				|| WoTSharingIsUnder(Recorded, Search.SkipRememberedUnder);
			// The sweep's own rule: a record that no longer canonicalizes to
			// itself (a link or rename above it) is not searched.
			TWoTBootstrapResult<FString> Canonical = bSkipped
				? TWoTBootstrapResult<FString>::Success(FString())
				: WoTCanonicalizeInstallerPickPath(Recorded);
			if (!bSkipped && !Canonical.IsError() && Canonical.GetValue().Equals(
					Recorded, ESearchCase::CaseSensitive))
			{
				FWoTInstallerLocation Location;
				Location.Source = EWoTInstallerSource::Remembered;
				Location.CanonicalPath = Recorded;
				Location.bRequirePinnedName = false;
				if (TOptional<FWoTLocalInstallerForSharing> Found =
						WoTSharingCandidate(EWoTSharingInstallerSource::Remembered,
							Location, WoTSharingInspect(Location, Search)))
				{
					return Found;
				}
			}
		}
		const FString ReceivedPath =
			WoTReceivedInstallerPathUnder(Search.UserStateRoot);
		if (!ReceivedPath.IsEmpty())
		{
			// EWoTInstallerSource only labels acquisition results; Inspect and
			// the streaming open ignore it. The receiver accepts the pinned
			// leaf only, like the adjacent location.
			FWoTInstallerLocation Location;
			Location.Source = EWoTInstallerSource::Adjacent;
			Location.CanonicalPath = ReceivedPath;
			Location.bRequirePinnedName = true;
			if (TOptional<FWoTLocalInstallerForSharing> Found =
					WoTSharingCandidate(EWoTSharingInstallerSource::Received,
						Location, WoTSharingInspect(Location, Search)))
			{
				return Found;
			}
		}
	}
	if (!Search.DownloadsDirectory.IsEmpty())
	{
		TOptional<FWoTNativeFileIdentity> ParentIdentity;
		TWoTBootstrapResult<FWoTHeldInstallerParent> Parent =
			WoTOpenInstallerParent(Search.DownloadsDirectory, ParentIdentity);
		if (!Parent.IsError())
		{
			for (const FString& CandidatePath :
				WoTDownloadsInstallerCandidates(Search.DownloadsDirectory))
			{
				FWoTInstallerLocation Location;
				Location.Source = EWoTInstallerSource::Downloads;
				Location.CanonicalPath = CandidatePath;
				Location.bRequirePinnedName = false;
				if (TOptional<FWoTLocalInstallerForSharing> Found =
						WoTSharingCandidate(EWoTSharingInstallerSource::Downloads,
							Location, WoTSharingInspectAtParent(
								Location, Parent.GetValue(), Search)))
				{
					return Found;
				}
			}
		}
	}
	return {};
}

TWoTBootstrapResult<FWoTNativeFileHandle> WoTOpenLocalInstallerForSharing(
	const FWoTLocalInstallerForSharing& Found)
{
	FWoTInstallerLocation Location = Found.Location;
	return WoTOpenInspectedInstallerForStreaming(Location, Found.Identity);
}

FString WoTReceivedInstallerPathUnder(const FString& UserStateRoot)
{
	if (UserStateRoot.IsEmpty() || FPaths::IsRelative(UserStateRoot))
	{
		return FString();
	}
	FString Path = FPaths::Combine(UserStateRoot,
		WoTReceivedInstallerDirectoryLeaf, WoT::Bootstrap::InstallerName);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString WoTReceivedInstallerPath()
{
	return WoTReceivedInstallerPathUnder(WoTResolveUserStateRoot());
}
