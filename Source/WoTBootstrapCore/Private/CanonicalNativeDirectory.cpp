#include "WoTBootstrapCore/NativeHandles.h"

#include "CanonicalNativeDirectoryPlatform.h"
#include "Misc/Paths.h"

namespace
{
bool WoTCanonicalDirectoryIsFilesystemRoot(const FString& Path)
{
	if (Path == TEXT("/"))
	{
		return true;
	}
	if (Path.Len() == 3 && FChar::IsAlpha(Path[0])
		&& Path[1] == TEXT(':') && Path[2] == TEXT('/'))
	{
		return true;
	}
	if (!Path.StartsWith(TEXT("//"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	const int32 ServerEnd = Path.Find(
		TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 2);
	return ServerEnd > 2
		&& Path.Find(TEXT("/"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, ServerEnd + 1) == INDEX_NONE;
}

TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
WoTOpenCanonicalNativeDirectory(
	FStringView LaunchAbsolutePath, bool bFollowLaunchFinal,
	TFunction<void()> BeforeCanonicalReopen)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalNativeDirectory>;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> First =
		bFollowLaunchFinal
			? WoTOpenNativeDirectoryFollowingForCanonicalization(
				LaunchAbsolutePath)
			: OpenNativeDirectoryNoFollow(LaunchAbsolutePath);
	if (First.IsError())
	{
		return FResult::Error(MoveTemp(First).GetError());
	}
	const FWoTNativeFileIdentity FirstIdentity =
		First.GetValue().GetIdentity();
	TWoTBootstrapResult<FString> Queried =
		QueryNativeDirectoryPath(First.GetValue());
	if (Queried.IsError())
	{
		return FResult::Error(MoveTemp(Queried).GetError());
	}
	FString CanonicalPath = MoveTemp(Queried).GetValue();
	CanonicalPath.ReplaceInline(TEXT("\\"), TEXT("/"),
		ESearchCase::CaseSensitive);
	const bool bPosixRoot = CanonicalPath == TEXT("/");
	const bool bDriveRoot = CanonicalPath.Len() == 3
		&& FChar::IsAlpha(CanonicalPath[0]) && CanonicalPath[1] == TEXT(':')
		&& CanonicalPath[2] == TEXT('/');
	FPaths::NormalizeDirectoryName(CanonicalPath);
	if (bPosixRoot)
	{
		CanonicalPath = TEXT("/");
	}
	else if (bDriveRoot && CanonicalPath.Len() == 2)
	{
		CanonicalPath += TEXT("/");
	}
	if (CanonicalPath.IsEmpty() || FPaths::IsRelative(CanonicalPath)
		|| !FPaths::CollapseRelativeDirectories(CanonicalPath))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the held directory reported an unsafe canonical path"),
			CanonicalPath));
	}

	if (BeforeCanonicalReopen)
	{
		BeforeCanonicalReopen();
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Reopened =
		WoTCanonicalDirectoryIsFilesystemRoot(CanonicalPath)
			? OpenNativeDirectoryNoFollow(CanonicalPath)
			: OpenNativeDirectoryPathNoFollow(CanonicalPath);
	if (Reopened.IsError())
	{
		return FResult::Error(MoveTemp(Reopened).GetError());
	}
	if (!Reopened.GetValue().GetIdentity().IsSameObject(FirstIdentity))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the directory identity changed between the following open and canonical no-follow re-open"),
			CanonicalPath));
	}

	FWoTCanonicalNativeDirectory Result;
	Result.CanonicalPath = MoveTemp(CanonicalPath);
	Result.Directory = MoveTemp(Reopened).GetValue();
	return FResult::Success(MoveTemp(Result));
}
} // namespace

TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
OpenCanonicalNativeDirectoryNoFollow(FStringView LaunchAbsolutePath)
{
	return WoTOpenCanonicalNativeDirectory(
		LaunchAbsolutePath, false, TFunction<void()>());
}

TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
OpenCanonicalNativeDirectoryFollowing(FStringView AncestorAbsolutePath)
{
	return WoTOpenCanonicalNativeDirectory(
		AncestorAbsolutePath, true, TFunction<void()>());
}

FWoTBootstrapStatus SplitNativePathParentAndLeaf(
	FStringView AbsolutePath, FString& OutParent, FString& OutLeaf)
{
	OutParent.Reset();
	OutLeaf.Reset();
	FString Path(AbsolutePath);
	Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	while (Path.Len() > 1 && Path.EndsWith(TEXT("/"), ESearchCase::CaseSensitive)
		&& !(Path.Len() == 3 && FChar::IsAlpha(Path[0])
			&& Path[1] == TEXT(':')))
	{
		Path.LeftChopInline(1, EAllowShrinking::No);
	}
	const bool bAbsolute = Path.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
		|| (Path.Len() >= 3 && FChar::IsAlpha(Path[0])
			&& Path[1] == TEXT(':') && Path[2] == TEXT('/'));
	if (Path.IsEmpty() || !bAbsolute
		|| WoTCanonicalDirectoryIsFilesystemRoot(Path))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("an absolute non-root directory path is required"), Path));
	}
	const int32 Slash = Path.Find(
		TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (Slash == INDEX_NONE || Slash == Path.Len() - 1)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the owned-root path has no safe leaf"), Path));
	}
	OutLeaf = Path.Mid(Slash + 1);
	OutParent = Slash == 0 ? TEXT("/") : Path.Left(Slash);
	if (Slash == 2 && Path.Len() >= 3 && FChar::IsAlpha(Path[0])
		&& Path[1] == TEXT(':'))
	{
		OutParent += TEXT("/");
	}
	if (FWoTBootstrapStatus Safe = RequireSafeRelativePath(OutLeaf);
		Safe.IsError())
	{
		OutParent.Reset();
		OutLeaf.Reset();
		return Safe;
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
OpenOrCreateCanonicalNativeDirectoryNoFollow(
	FStringView OwnedRootAbsolutePath, bool& bOutCreated)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalNativeDirectory>;
	bOutCreated = false;
	FString ParentPath;
	FString Leaf;
	if (FWoTBootstrapStatus Split = SplitNativePathParentAndLeaf(
			OwnedRootAbsolutePath, ParentPath, Leaf); Split.IsError())
	{
		return FResult::Error(MoveTemp(Split).GetError());
	}
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Parent =
		OpenCanonicalNativeDirectoryFollowing(ParentPath);
	if (Parent.IsError())
	{
		return FResult::Error(MoveTemp(Parent).GetError());
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenOrCreateNativeDirectoryAtNoFollow(
			Parent.GetValue().Directory, Leaf, bOutCreated);
	if (Root.IsError())
	{
		return FResult::Error(MoveTemp(Root).GetError());
	}
	TWoTBootstrapResult<FString> Canonical =
		QueryNativeDirectoryPath(Root.GetValue());
	if (Canonical.IsError())
	{
		return FResult::Error(MoveTemp(Canonical).GetError());
	}
	FWoTCanonicalNativeDirectory Result;
	Result.CanonicalPath = MoveTemp(Canonical).GetValue();
	Result.CanonicalPath.ReplaceInline(TEXT("\\"), TEXT("/"),
		ESearchCase::CaseSensitive);
	Result.Directory = MoveTemp(Root).GetValue();
	Result.ParentDirectory = MoveTemp(Parent.GetValue().Directory);
	return FResult::Success(MoveTemp(Result));
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<FWoTCanonicalNativeDirectory>
OpenCanonicalNativeDirectoryNoFollowForTesting(
	FStringView LaunchAbsolutePath, TFunction<void()> BeforeCanonicalReopen)
{
	return WoTOpenCanonicalNativeDirectory(
		LaunchAbsolutePath, false, MoveTemp(BeforeCanonicalReopen));
}
#endif
