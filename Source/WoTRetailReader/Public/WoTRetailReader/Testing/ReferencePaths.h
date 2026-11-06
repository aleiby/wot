#pragma once

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

// The repository root. Game and editor targets run with ProjectDir there, but an
// engine-free Program (WoTRetailReaderTests) gets the engine-relative
// <...>/Engine/Programs/<Name>/ instead, where no fixture exists. So when
// ProjectDir holds no WoT.uproject, walk up from the executable
// (<repo>/Binaries/<Platform>/) to the directory that does. Nothing absolute is
// compiled in.
inline FString WoTRepositoryRoot()
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (FPaths::FileExists(ProjectDir / TEXT("WoT.uproject")))
	{
		return ProjectDir;
	}
	FString Directory = FPaths::ConvertRelativePathToFull(
		FString(FPlatformProcess::BaseDir()));
	FPaths::NormalizeDirectoryName(Directory);
	for (int32 Level = 0; Level < 6 && !Directory.IsEmpty(); ++Level)
	{
		if (FPaths::FileExists(Directory / TEXT("WoT.uproject")))
		{
			return Directory + TEXT("/");
		}
		const FString Parent = FPaths::GetPath(Directory);
		if (Parent.IsEmpty() || Parent == Directory)
		{
			break;
		}
		Directory = Parent;
	}
	return ProjectDir;
}

inline FString WoTReferenceRoot()
{
	const FString Configured = FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_REFERENCE_ROOT"));
	return FPaths::ConvertRelativePathToFull(
		Configured.IsEmpty() ? WoTRepositoryRoot() : Configured);
}

inline bool WoTIsPrivateReferencePath(const FStringView Relative)
{
	// Retail-derived fixtures the public export omits (Tools/public/
	// export_public_tree.py RETAIL_DERIVED_ARTIFACTS) are private reference
	// inputs too: a public clone reaches them through WOT_REFERENCE_ROOT, and
	// without one the real-retail specs that need them skip.
	if (Relative.Equals(TEXT("Tools/dist/fixtures/bsp-native-parity-v1.json"),
		ESearchCase::IgnoreCase))
	{
		return true;
	}
	return Relative.Equals(TEXT("original"), ESearchCase::IgnoreCase)
		|| Relative.StartsWith(TEXT("original/"), ESearchCase::IgnoreCase)
		|| Relative.Equals(TEXT("devtree"), ESearchCase::IgnoreCase)
		|| Relative.StartsWith(TEXT("devtree/"), ESearchCase::IgnoreCase)
		|| Relative.Equals(TEXT("exports"), ESearchCase::IgnoreCase)
		|| Relative.StartsWith(TEXT("exports/"), ESearchCase::IgnoreCase);
}

inline FString WoTReferencePath(const FStringView Relative)
{
	return FPaths::ConvertRelativePathToFull(
		WoTReferenceRoot() / FString(Relative));
}

inline FString WoTRepositoryOrReferencePath(const FStringView Relative)
{
	return WoTIsPrivateReferencePath(Relative)
		? WoTReferencePath(Relative)
		: FPaths::ConvertRelativePathToFull(WoTRepositoryRoot() / FString(Relative));
}

inline bool WoTReferenceTreeExists()
{
	return FPaths::DirectoryExists(WoTReferencePath(TEXT("original")));
}

inline FString WoTReferenceMissingWarning(const FStringView Relative)
{
	return FString::Printf(TEXT(
		"private reference input %.*s is absent; set WOT_REFERENCE_ROOT; oracle skipped"),
		Relative.Len(), Relative.GetData());
}
