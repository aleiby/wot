#include "WoTInstallerAcquisition.h"

#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
constexpr TCHAR WoTInstallerParentIdentityChanged[] =
	TEXT("the canonical installer parent identity changed");
constexpr TCHAR WoTInstallerIdentityMutated[] =
	TEXT("the adjacent installer identity changed");
constexpr TCHAR WoTInstallerUnsafePathChanged[] =
	TEXT("the adjacent installer canonical path changed");

struct FWoTInstallerPins
{
	FString Name;
	uint64 Size = 0;
	FWoTSha256 Sha256;
};

struct FWoTInstallerHooks
{
	TFunction<void(uint64)> FullHash;
	TFunction<void()> BeforeHeldOpen;
	TFunction<void()> BeforeSnapshot;
};

struct FWoTInstallerParent
{
	FWoTNativeDirectoryHandle Directory;
	FString Path;
	FString Leaf;
};

struct FWoTInstallerFoundEntry
{
	EWoTTreeEntryType Type = EWoTTreeEntryType::File;
	bool bUnsupportedReparsePoint = false;
};

FWoTBootstrapError InstallerError(EWoTBootstrapError Code, FString Message,
	FString Path = FString())
{
	return MakeWoTBootstrapError(Code, MoveTemp(Message), MoveTemp(Path));
}

FWoTBootstrapError CancelledError()
{
	return InstallerError(EWoTBootstrapError::ServiceUnavailable,
		TEXT("installer inspection was cancelled"));
}

TWoTBootstrapResult<FWoTInstallerPins> ProductionPins()
{
	using FResult = TWoTBootstrapResult<FWoTInstallerPins>;
	TWoTBootstrapResult<FWoTSha256> Parsed =
		FWoTSha256::FromHex(WoT::Bootstrap::InstallerSha256);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	FWoTInstallerPins Pins;
	Pins.Name = WoT::Bootstrap::InstallerName;
	Pins.Size = WoT::Bootstrap::InstallerSize;
	Pins.Sha256 = MoveTemp(Parsed).GetValue();
	return FResult::Success(MoveTemp(Pins));
}

TWoTBootstrapResult<FWoTInstallerParent> OpenParent(
	FWoTInstallerLocation& Location, const FWoTInstallerPins& Pins)
{
	using FResult = TWoTBootstrapResult<FWoTInstallerParent>;
	if (Pins.Name.IsEmpty() || Pins.Size == 0
		|| Pins.Size > static_cast<uint64>(TNumericLimits<int64>::Max())
		|| FPaths::IsRelative(Location.CanonicalPath)
		|| (Location.bRequirePinnedName
			&& !FPaths::GetCleanFilename(Location.CanonicalPath).Equals(
				Pins.Name, ESearchCase::IgnoreCase)))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			TEXT("the installer pins or canonical location path are invalid"),
			Location.CanonicalPath));
	}
	FWoTInstallerParent Parent;
	if (FWoTBootstrapStatus Split = SplitNativePathParentAndLeaf(
			Location.CanonicalPath, Parent.Path, Parent.Leaf); Split.IsError())
	{
		return FResult::Error(MoveTemp(Split).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened =
		OpenNativeDirectoryNoFollow(Parent.Path);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	if (Location.ParentIdentity.IsSet()
		&& !Opened.GetValue().GetIdentity().IsSameObject(
			Location.ParentIdentity.GetValue()))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerParentIdentityChanged, Parent.Path));
	}
	if (!Location.ParentIdentity.IsSet())
	{
		Location.ParentIdentity = Opened.GetValue().GetIdentity();
	}
	Parent.Directory = MoveTemp(Opened).GetValue();
	return FResult::Success(MoveTemp(Parent));
}

TWoTBootstrapResult<TOptional<FWoTInstallerFoundEntry>> FindEntry(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
	using FResult = TWoTBootstrapResult<TOptional<FWoTInstallerFoundEntry>>;
#if PLATFORM_WINDOWS
	TWoTBootstrapResult<TOptional<FWoTInstallerSourceEntryInfo>> Found =
		QueryNativeInstallerSourceEntryAtNoFollow(Directory, Leaf);
	if (Found.IsError()) return FResult::Error(MoveTemp(Found).GetError());
	if (!Found.GetValue().IsSet())
		return FResult::Success(TOptional<FWoTInstallerFoundEntry>());
	const FWoTInstallerSourceEntryInfo& Info = Found.GetValue().GetValue();
	return FResult::Success(TOptional<FWoTInstallerFoundEntry>(
		FWoTInstallerFoundEntry{Info.Type, Info.bRefusedReparsePoint}));
#else
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
		QueryNativeEntryTypeAtNoFollow(Directory, Leaf);
	if (Found.IsError()) return FResult::Error(MoveTemp(Found).GetError());
	if (!Found.GetValue().IsSet())
		return FResult::Success(TOptional<FWoTInstallerFoundEntry>());
	return FResult::Success(TOptional<FWoTInstallerFoundEntry>(
		FWoTInstallerFoundEntry{Found.GetValue().GetValue(), false}));
#endif
}

FString OnlineOnlyInstallerMessage(const FString& Path)
{
	return FString::Printf(TEXT("The installer at %s is an online-only file. Make it "
		"available on this device (in File Explorer, right-click it and choose "
		"'Always keep on this device'), then start WoT Reborn again."), *Path);
}

bool PopulateOnlineOnlyState(const FWoTNativeFileHandle& File,
	const FString& Path, FWoTAdjacentInstallerInspection& Result,
	FWoTBootstrapError& OutError)
{
#if PLATFORM_WINDOWS
	TWoTBootstrapResult<uint32> Attributes =
		QueryNativeInstallerSourceFileAttributes(File);
	if (Attributes.IsError())
	{
		OutError = MoveTemp(Attributes).GetError();
		return false;
	}
	if (!WoTInstallerSourceAttributesAreFullyLocal(Attributes.GetValue()))
	{
		Result.State = EWoTPinnedAdjacentInstallerProbe::OnlineOnly;
		Result.Message = OnlineOnlyInstallerMessage(Path);
	}
#else
	(void)File;
	(void)Path;
	(void)Result;
	(void)OutError;
#endif
	return true;
}

TWoTBootstrapResult<FWoTNativeFileHandle> OpenInstallerSource(
	const FWoTNativeDirectoryHandle& Directory, FStringView Leaf)
{
#if PLATFORM_WINDOWS
	return OpenNativeInstallerSourceFileAtReadNoFollow(Directory, Leaf);
#else
	return OpenNativeFileAtReadNoFollow(Directory, Leaf);
#endif
}

TWoTBootstrapResult<FString> BindHeldParent(
	FWoTInstallerLocation& Location, const FWoTInstallerPins& Pins,
	const FWoTHeldInstallerParent& Parent)
{
	using FResult = TWoTBootstrapResult<FString>;
	FString LocationParent;
	FString Leaf;
	if (Pins.Name.IsEmpty() || Pins.Size == 0
		|| Pins.Size > static_cast<uint64>(TNumericLimits<int64>::Max())
		|| FPaths::IsRelative(Location.CanonicalPath)
		|| (Location.bRequirePinnedName
			&& !FPaths::GetCleanFilename(Location.CanonicalPath).Equals(
				Pins.Name, ESearchCase::IgnoreCase)))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			TEXT("the installer pins or canonical location path are invalid"),
			Location.CanonicalPath));
	}
	if (FWoTBootstrapStatus Split = SplitNativePathParentAndLeaf(
			Location.CanonicalPath, LocationParent, Leaf); Split.IsError())
	{
		return FResult::Error(MoveTemp(Split).GetError());
	}
	if (!FPaths::IsSamePath(LocationParent, Parent.CanonicalPath))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			TEXT("the installer candidate does not belong to the held parent"),
			Location.CanonicalPath));
	}
	const FWoTNativeFileIdentity& HeldIdentity = Parent.Directory.GetIdentity();
	if (Location.ParentIdentity.IsSet()
		&& !HeldIdentity.IsSameObject(Location.ParentIdentity.GetValue()))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerParentIdentityChanged, LocationParent));
	}
	if (!Location.ParentIdentity.IsSet())
	{
		Location.ParentIdentity = HeldIdentity;
	}
	return FResult::Success(MoveTemp(Leaf));
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection> InspectAtParent(
	FWoTInstallerLocation& Location, const FWoTInstallerPins& Pins,
	const FWoTHeldInstallerParent& Parent)
{
	using FResult = TWoTBootstrapResult<FWoTAdjacentInstallerInspection>;
	TWoTBootstrapResult<FString> Bound = BindHeldParent(Location, Pins, Parent);
	if (Bound.IsError())
	{
		return FResult::Error(MoveTemp(Bound).GetError());
	}
	TWoTBootstrapResult<TOptional<FWoTInstallerFoundEntry>> Found =
		FindEntry(Parent.Directory, Bound.GetValue());
	if (Found.IsError())
	{
		return FResult::Error(MoveTemp(Found).GetError());
	}
	FWoTAdjacentInstallerInspection Result;
	if (!Found.GetValue().IsSet())
	{
		return FResult::Success(MoveTemp(Result));
	}
	if (Found.GetValue()->bUnsupportedReparsePoint)
	{
		Result.State = EWoTPinnedAdjacentInstallerProbe::UnsupportedReparsePoint;
		return FResult::Success(MoveTemp(Result));
	}
	if (Found.GetValue()->Type != EWoTTreeEntryType::File)
	{
		Result.State = EWoTPinnedAdjacentInstallerProbe::NotTheInstaller;
		return FResult::Success(MoveTemp(Result));
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
		OpenInstallerSource(Parent.Directory, Bound.GetValue());
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	FWoTBootstrapError AttributeError;
	if (!PopulateOnlineOnlyState(Opened.GetValue(), Location.CanonicalPath,
		Result, AttributeError))
	{
		return FResult::Error(MoveTemp(AttributeError));
	}
	if (Result.State == EWoTPinnedAdjacentInstallerProbe::OnlineOnly)
	{
		return FResult::Success(MoveTemp(Result));
	}
	const FWoTNativeFileIdentity& Identity = Opened.GetValue().GetIdentity();
	Result.Identity = Identity;
	if (Identity.Type != EWoTTreeEntryType::File || Identity.LinkCount != 1
		|| Identity.Size > Pins.Size)
	{
		Result.State = EWoTPinnedAdjacentInstallerProbe::NotTheInstaller;
		return FResult::Success(MoveTemp(Result));
	}
	if (Identity.Size < Pins.Size)
	{
		Result.State = EWoTPinnedAdjacentInstallerProbe::Downloading;
		return FResult::Success(MoveTemp(Result));
	}
	Result.State = EWoTPinnedAdjacentInstallerProbe::Candidate;
	return FResult::Success(MoveTemp(Result));
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection> Inspect(
	FWoTInstallerLocation& Location, const FWoTInstallerPins& Pins)
{
	using FResult = TWoTBootstrapResult<FWoTAdjacentInstallerInspection>;
	TWoTBootstrapResult<FWoTInstallerParent> Opened = OpenParent(Location, Pins);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	FWoTHeldInstallerParent Parent;
	Parent.CanonicalPath = Opened.GetValue().Path;
	Parent.Directory = MoveTemp(Opened).GetValue().Directory;
	return InspectAtParent(Location, Pins, Parent);
}

TWoTBootstrapResult<FWoTSha256> HashHeld(
	const FWoTNativeFileHandle& File, const FWoTNativeFileIdentity& Identity,
	const FString& DiagnosticPath, const TFunction<bool()>& IsCancelled,
	const FWoTInstallerHooks& Hooks)
{
	using FResult = TWoTBootstrapResult<FWoTSha256>;
	FWoTSha256Hasher Hasher;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(1024 * 1024);
	uint64 Offset = 0;
	while (Offset < Identity.Size)
	{
		if (IsCancelled && IsCancelled())
		{
			return FResult::Error(CancelledError());
		}
		const int32 Requested = static_cast<int32>(FMath::Min<uint64>(
			static_cast<uint64>(Buffer.Num()), Identity.Size - Offset));
		TWoTBootstrapResult<uint64> Read = ReadNativeFileAt(File, Offset,
			TArrayView<uint8>(Buffer.GetData(), Requested));
		if (Read.IsError())
		{
			return FResult::Error(MoveTemp(Read).GetError());
		}
		if (Read.GetValue() == 0
			|| Read.GetValue() > static_cast<uint64>(Requested))
		{
			return FResult::Error(InstallerError(EWoTBootstrapError::Io,
				TEXT("the held installer ended before its pinned size"),
				DiagnosticPath));
		}
		Hasher.Update(TConstArrayView<uint8>(Buffer.GetData(),
			static_cast<int32>(Read.GetValue())));
		Offset += Read.GetValue();
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> After =
		File.QueryCurrentIdentity();
	if (After.IsError())
	{
		return FResult::Error(MoveTemp(After).GetError());
	}
	if (After.GetValue() != Identity)
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerIdentityMutated, DiagnosticPath));
	}
	if (Hooks.FullHash)
	{
		Hooks.FullHash(Identity.Size);
	}
	return FResult::Success(Hasher.Finish());
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult> AcquireAtParent(
	FWoTInstallerLocation& Location, const FWoTInstallerPins& Pins,
	const FWoTHeldInstallerParent& Parent,
	const TFunction<bool()>& IsCancelled, const FWoTInstallerHooks& Hooks,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	using FResult = TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>;
	if (IsCancelled && IsCancelled())
	{
		return FResult::Error(CancelledError());
	}
	TWoTBootstrapResult<FWoTAdjacentInstallerInspection> Inspected =
		InspectAtParent(Location, Pins, Parent);
	if (Inspected.IsError())
	{
		return FResult::Error(MoveTemp(Inspected).GetError());
	}
	FWoTPinnedAdjacentInstallerResult Result;
	Result.State = Inspected.GetValue().State;
	Result.Message = Inspected.GetValue().Message;
	if (Result.State != EWoTPinnedAdjacentInstallerProbe::Candidate
		|| !Inspected.GetValue().Identity.IsSet())
	{
		return FResult::Success(MoveTemp(Result));
	}
	const FWoTNativeFileIdentity StableIdentity =
		Inspected.GetValue().Identity.GetValue();
	if (ExpectedInstallerIdentity != nullptr
		&& StableIdentity != *ExpectedInstallerIdentity)
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerIdentityMutated, Location.CanonicalPath));
	}
	if (Hooks.BeforeHeldOpen)
	{
		Hooks.BeforeHeldOpen();
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Held = OpenInstallerSource(
		Parent.Directory, FPaths::GetCleanFilename(Location.CanonicalPath));
	if (Held.IsError())
	{
		return FResult::Error(MoveTemp(Held).GetError());
	}
	if (Held.GetValue().GetIdentity() != StableIdentity)
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerIdentityMutated, Location.CanonicalPath));
	}
	FWoTAdjacentInstallerInspection Locality;
	FWoTBootstrapError AttributeError;
	if (!PopulateOnlineOnlyState(Held.GetValue(), Location.CanonicalPath,
		Locality, AttributeError))
	{
		return FResult::Error(MoveTemp(AttributeError));
	}
	if (Locality.State == EWoTPinnedAdjacentInstallerProbe::OnlineOnly)
	{
		Result.State = Locality.State;
		Result.Message = MoveTemp(Locality.Message);
		return FResult::Success(MoveTemp(Result));
	}
	TWoTBootstrapResult<FWoTSha256> Digest = HashHeld(Held.GetValue(),
		StableIdentity, Location.CanonicalPath, IsCancelled, Hooks);
	if (Digest.IsError())
	{
		return FResult::Error(MoveTemp(Digest).GetError());
	}
	if (Digest.GetValue() != Pins.Sha256)
	{
		Result.State = EWoTPinnedAdjacentInstallerProbe::NotTheInstaller;
		return FResult::Success(MoveTemp(Result));
	}
	if (Hooks.BeforeSnapshot)
	{
		Hooks.BeforeSnapshot();
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> FinalIdentity =
		Held.GetValue().QueryCurrentIdentity();
	TWoTBootstrapResult<FString> FinalPath = QueryNativeFilePath(Held.GetValue());
	if (FinalIdentity.IsError())
	{
		return FResult::Error(MoveTemp(FinalIdentity).GetError());
	}
	if (FinalPath.IsError())
	{
		return FResult::Error(MoveTemp(FinalPath).GetError());
	}
	FString HeldCanonicalPath = FinalPath.GetValue();
	FString RequestedCanonicalPath = Location.CanonicalPath;
	FPaths::NormalizeFilename(HeldCanonicalPath);
	FPaths::NormalizeFilename(RequestedCanonicalPath);
	// The direct parent-relative open already applied the volume's case rules
	// and the held identity is checked above and below. Preserve that result on
	// case-insensitive volumes while still refusing any non-case path change.
	if (!HeldCanonicalPath.Equals(
			RequestedCanonicalPath, ESearchCase::IgnoreCase))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerUnsafePathChanged, Location.CanonicalPath));
	}
	if (FinalIdentity.GetValue() != StableIdentity)
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerIdentityMutated, Location.CanonicalPath));
	}
	Location.CanonicalPath = MoveTemp(HeldCanonicalPath);
	FWoTHeldInstallerSnapshot Snapshot;
	Snapshot.Handle = MoveTemp(Held).GetValue();
	Snapshot.Size = Pins.Size;
	Snapshot.Sha256 = Pins.Sha256;
	Snapshot.Identity = StableIdentity;
	Result.State = EWoTPinnedAdjacentInstallerProbe::Valid;
	Result.Verified = MakeUnique<FWoTVerifiedInstaller>();
	Result.Verified->Source = Location.Source;
	Result.Verified->Snapshot = MoveTemp(Snapshot);
	return FResult::Success(MoveTemp(Result));
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult> Acquire(
	FWoTInstallerLocation& Location, const FWoTInstallerPins& Pins,
	const TFunction<bool()>& IsCancelled, const FWoTInstallerHooks& Hooks,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	using FResult = TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>;
	TWoTBootstrapResult<FWoTInstallerParent> Opened = OpenParent(Location, Pins);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	FWoTHeldInstallerParent Parent;
	Parent.CanonicalPath = Opened.GetValue().Path;
	Parent.Directory = MoveTemp(Opened).GetValue().Directory;
	return AcquireAtParent(Location, Pins, Parent, IsCancelled, Hooks,
		ExpectedInstallerIdentity);
}
}

bool WoTInstallerAcquisitionIsParentIdentityMismatch(
	const FWoTBootstrapError& Error)
{
	return Error.Code == EWoTBootstrapError::UnsafePath
		&& Error.Message.StartsWith(
			WoTInstallerParentIdentityChanged, ESearchCase::CaseSensitive);
}

bool WoTInstallerAcquisitionIsIdentityMutation(const FWoTBootstrapError& Error)
{
	return Error.Code == EWoTBootstrapError::UnsafePath
		&& Error.Message.StartsWith(
			WoTInstallerIdentityMutated, ESearchCase::CaseSensitive);
}

TWoTBootstrapResult<FString> WoTCanonicalizeInstallerPickPath(
	const FString& AbsolutePath)
{
	using FResult = TWoTBootstrapResult<FString>;
	if (AbsolutePath.IsEmpty() || FPaths::IsRelative(AbsolutePath))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			TEXT("an installer pick must be an absolute path"), AbsolutePath));
	}
	FString ParentPath;
	FString Leaf;
	if (FWoTBootstrapStatus Split = SplitNativePathParentAndLeaf(
			AbsolutePath, ParentPath, Leaf); Split.IsError())
	{
		return FResult::Error(MoveTemp(Split).GetError());
	}
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Parent =
		OpenCanonicalNativeDirectoryFollowing(ParentPath);
	if (Parent.IsError())
	{
		return FResult::Error(MoveTemp(Parent).GetError());
	}
	FString CanonicalPath = FPaths::Combine(
		Parent.GetValue().CanonicalPath, Leaf);
	FPaths::NormalizeFilename(CanonicalPath);
	return FResult::Success(MoveTemp(CanonicalPath));
}

TWoTBootstrapResult<FWoTHeldInstallerParent> WoTOpenInstallerParent(
	const FString& CanonicalParentPath,
	TOptional<FWoTNativeFileIdentity>& ParentIdentity)
{
	using FResult = TWoTBootstrapResult<FWoTHeldInstallerParent>;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened =
		OpenNativeDirectoryNoFollow(CanonicalParentPath);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	const FWoTNativeFileIdentity& OpenedIdentity =
		Opened.GetValue().GetIdentity();
	if (ParentIdentity.IsSet()
		&& !OpenedIdentity.IsSameObject(ParentIdentity.GetValue()))
	{
		return FResult::Error(InstallerError(EWoTBootstrapError::UnsafePath,
			WoTInstallerParentIdentityChanged, CanonicalParentPath));
	}
	if (!ParentIdentity.IsSet())
	{
		ParentIdentity = OpenedIdentity;
	}
	FWoTHeldInstallerParent Parent;
	Parent.CanonicalPath = CanonicalParentPath;
	Parent.Directory = MoveTemp(Opened).GetValue();
	return FResult::Success(MoveTemp(Parent));
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocation(FWoTInstallerLocation& Location)
{
	using FResult = TWoTBootstrapResult<FWoTAdjacentInstallerInspection>;
	TWoTBootstrapResult<FWoTInstallerPins> Pins = ProductionPins();
	return Pins.IsError() ? FResult::Error(MoveTemp(Pins).GetError())
		: Inspect(Location, Pins.GetValue());
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocationAtParent(FWoTInstallerLocation& Location,
	const FWoTHeldInstallerParent& Parent)
{
	using FResult = TWoTBootstrapResult<FWoTAdjacentInstallerInspection>;
	TWoTBootstrapResult<FWoTInstallerPins> Pins = ProductionPins();
	return Pins.IsError() ? FResult::Error(MoveTemp(Pins).GetError())
		: InspectAtParent(Location, Pins.GetValue(), Parent);
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocation(FWoTInstallerLocation& Location,
	TFunction<bool()> IsCancelled,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	using FResult = TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>;
	TWoTBootstrapResult<FWoTInstallerPins> Pins = ProductionPins();
	return Pins.IsError() ? FResult::Error(MoveTemp(Pins).GetError())
		: Acquire(Location, Pins.GetValue(), IsCancelled,
			FWoTInstallerHooks{}, ExpectedInstallerIdentity);
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocationAtParent(FWoTInstallerLocation& Location,
	const FWoTHeldInstallerParent& Parent, TFunction<bool()> IsCancelled,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	using FResult = TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>;
	TWoTBootstrapResult<FWoTInstallerPins> Pins = ProductionPins();
	return Pins.IsError() ? FResult::Error(MoveTemp(Pins).GetError())
		: AcquireAtParent(Location, Pins.GetValue(), Parent, IsCancelled,
			FWoTInstallerHooks{}, ExpectedInstallerIdentity);
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectPinnedAdjacentInstaller(const FString& CanonicalAdjacentPath,
	const FWoTNativeFileIdentity* ExpectedParentIdentity)
{
	FWoTInstallerLocation Location;
	Location.CanonicalPath = CanonicalAdjacentPath;
	if (ExpectedParentIdentity != nullptr)
	{
		Location.ParentIdentity = *ExpectedParentIdentity;
	}
	return WoTInspectInstallerLocation(Location);
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquirePinnedAdjacentInstaller(const FString& CanonicalAdjacentPath,
	TFunction<bool()> IsCancelled,
	const FWoTNativeFileIdentity* ExpectedParentIdentity,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	FWoTInstallerLocation Location;
	Location.CanonicalPath = CanonicalAdjacentPath;
	if (ExpectedParentIdentity != nullptr)
	{
		Location.ParentIdentity = *ExpectedParentIdentity;
	}
	return WoTTryAcquireInstallerLocation(
		Location, MoveTemp(IsCancelled), ExpectedInstallerIdentity);
}

namespace
{
TWoTBootstrapResult<bool> ProbeAdjacentInstallerParentWritable(
	const FString& CanonicalAdjacentPath,
	const FWoTNativeFileIdentity* ExpectedParentIdentity,
	const TFunction<void(FStringView)>& AfterCreate)
{
	using FResult = TWoTBootstrapResult<bool>;
	TWoTBootstrapResult<FWoTInstallerPins> Pins = ProductionPins();
	if (Pins.IsError())
	{
		return FResult::Error(MoveTemp(Pins).GetError());
	}
	FWoTInstallerLocation Location;
	Location.CanonicalPath = CanonicalAdjacentPath;
	if (ExpectedParentIdentity != nullptr)
	{
		Location.ParentIdentity = *ExpectedParentIdentity;
	}
	TWoTBootstrapResult<FWoTInstallerParent> Parent = OpenParent(
		Location, Pins.GetValue());
	if (Parent.IsError())
	{
		return FResult::Error(MoveTemp(Parent).GetError());
	}
	const FString Leaf = TEXT(".wot-write-probe-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
#if WITH_AUTOMATION_TESTS
	if (AfterCreate)
	{
		return ProbeNativeDirectoryWritableNoFollowForTesting(
			Parent.GetValue().Directory, Leaf, AfterCreate);
	}
#else
	(void)AfterCreate;
#endif
	return ProbeNativeDirectoryWritableNoFollow(
		Parent.GetValue().Directory, Leaf);
}
} // namespace

TWoTBootstrapResult<bool> WoTProbeAdjacentInstallerParentWritable(
	const FString& CanonicalAdjacentPath,
	const FWoTNativeFileIdentity* ExpectedParentIdentity)
{
	return ProbeAdjacentInstallerParentWritable(
		CanonicalAdjacentPath, ExpectedParentIdentity, {});
}

void WoTRemoveLegacyInstallerStateBestEffort(const FString& StateDir)
{
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> State =
		OpenNativeDirectoryNoFollow(StateDir);
	if (State.IsError()) return;
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
		QueryNativeEntryTypeAtNoFollow(State.GetValue(), TEXT("installer"));
	if (Found.IsError() || !Found.GetValue().IsSet()) return;
	if (Found.GetValue().GetValue() != EWoTTreeEntryType::Directory)
	{
		(void)RemoveNativeEntryAtNoFollow(State.GetValue(), TEXT("installer"),
			Found.GetValue().GetValue());
		return;
	}
	{
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Legacy =
			OpenNativeDirectoryAtNoFollow(State.GetValue(), TEXT("installer"));
		if (Legacy.IsError()) return;
		TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
			EnumerateNativeDirectory(Legacy.GetValue());
		if (Entries.IsError()) return;
		for (const FWoTNativeDirectoryEntry& Entry : Entries.GetValue())
		{
			(void)RemoveNativeEntryAtNoFollow(
				Legacy.GetValue(), Entry.Name, Entry.Type);
		}
	}
	(void)RemoveNativeEntryAtNoFollow(State.GetValue(), TEXT("installer"),
		EWoTTreeEntryType::Directory);
}

#if WITH_AUTOMATION_TESTS
FWoTInstallerAcquisitionPins::FWoTInstallerAcquisitionPins()
	: Name(WoT::Bootstrap::InstallerName)
	, Size(WoT::Bootstrap::InstallerSize)
{
	TWoTBootstrapResult<FWoTSha256> Parsed =
		FWoTSha256::FromHex(WoT::Bootstrap::InstallerSha256);
	check(!Parsed.IsError());
	Sha256 = MoveTemp(Parsed).GetValue();
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocationForTesting(
	FWoTInstallerLocation& Location,
	const FWoTInstallerAcquisitionPins& Pins)
{
	FWoTInstallerPins Resolved{Pins.Name, Pins.Size, Pins.Sha256};
	return Inspect(Location, Resolved);
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectInstallerLocationAtParentForTesting(
	FWoTInstallerLocation& Location, const FWoTHeldInstallerParent& Parent,
	const FWoTInstallerAcquisitionPins& Pins)
{
	FWoTInstallerPins Resolved{Pins.Name, Pins.Size, Pins.Sha256};
	return InspectAtParent(Location, Resolved, Parent);
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocationForTesting(
	FWoTInstallerLocation& Location,
	const FWoTInstallerAcquisitionPins& Pins, TFunction<bool()> IsCancelled,
	const FWoTInstallerAcquisitionTestHooks& TestHooks,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	FWoTInstallerPins Resolved{Pins.Name, Pins.Size, Pins.Sha256};
	FWoTInstallerHooks Hooks{
		TestHooks.FullHash, TestHooks.BeforeHeldOpen, TestHooks.BeforeSnapshot};
	return Acquire(Location, Resolved, IsCancelled, Hooks,
		ExpectedInstallerIdentity);
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquireInstallerLocationAtParentForTesting(
	FWoTInstallerLocation& Location, const FWoTHeldInstallerParent& Parent,
	const FWoTInstallerAcquisitionPins& Pins, TFunction<bool()> IsCancelled,
	const FWoTInstallerAcquisitionTestHooks& TestHooks,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	FWoTInstallerPins Resolved{Pins.Name, Pins.Size, Pins.Sha256};
	FWoTInstallerHooks Hooks{
		TestHooks.FullHash, TestHooks.BeforeHeldOpen, TestHooks.BeforeSnapshot};
	return AcquireAtParent(Location, Resolved, Parent, IsCancelled, Hooks,
		ExpectedInstallerIdentity);
}

TWoTBootstrapResult<FWoTAdjacentInstallerInspection>
WoTInspectPinnedAdjacentInstallerForTesting(
	const FString& CanonicalAdjacentPath,
	const FWoTInstallerAcquisitionPins& Pins,
	const FWoTNativeFileIdentity* ExpectedParentIdentity)
{
	FWoTInstallerLocation Location;
	Location.CanonicalPath = CanonicalAdjacentPath;
	if (ExpectedParentIdentity != nullptr)
	{
		Location.ParentIdentity = *ExpectedParentIdentity;
	}
	return WoTInspectInstallerLocationForTesting(Location, Pins);
}

TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult>
WoTTryAcquirePinnedAdjacentInstallerForTesting(
	const FString& CanonicalAdjacentPath,
	const FWoTInstallerAcquisitionPins& Pins, TFunction<bool()> IsCancelled,
	const FWoTInstallerAcquisitionTestHooks& TestHooks,
	const FWoTNativeFileIdentity* ExpectedParentIdentity,
	const FWoTNativeFileIdentity* ExpectedInstallerIdentity)
{
	FWoTInstallerLocation Location;
	Location.CanonicalPath = CanonicalAdjacentPath;
	if (ExpectedParentIdentity != nullptr)
	{
		Location.ParentIdentity = *ExpectedParentIdentity;
	}
	return WoTTryAcquireInstallerLocationForTesting(Location, Pins,
		MoveTemp(IsCancelled), TestHooks, ExpectedInstallerIdentity);
}

TWoTBootstrapResult<bool> WoTProbeAdjacentInstallerParentWritableForTesting(
	const FString& CanonicalAdjacentPath,
	const FWoTNativeFileIdentity* ExpectedParentIdentity,
	TFunction<void(FStringView)> AfterCreate)
{
	return ProbeAdjacentInstallerParentWritable(
		CanonicalAdjacentPath, ExpectedParentIdentity, AfterCreate);
}
#endif
