// Safe-archive policy: what an archive is allowed to contain.
//
// An archive index is attacker-controlled input that is about to become filesystem
// operations, so it is validated as a whole BEFORE anything is written. Every rule
// here is a way an archive could otherwise write outside its destination, or make
// one logical file out of two entries, or make extraction never terminate.
//
// Paths are treated as UTF-8 POSIX logical names and validated in that form, before
// any platform mapping. Validating after mapping would mean validating something
// the archive did not actually say.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

/** The two shapes a release archive may have. There is no generic layout. */
enum class EWoTArchiveLayout : uint8
{
	/** A single top-level immutable version tree. Windows permits NO links at all. */
	Win64VersionTree,

	/** A single top-level `.app` bundle. Links are allowed only inside it. */
	MacAppBundle,
};

WOTBOOTSTRAPCORE_API const TCHAR* LexToString(EWoTArchiveLayout Layout);

struct FWoTArchiveEntry
{
	FString Path;
	EWoTTreeEntryType Type = EWoTTreeEntryType::File;
	uint64 Size = 0;
	uint32 Mode = 0;
	FString LinkTarget;
};

/**
 * Bounds applied before extraction.
 *
 * They exist so a malicious index cannot ask for unbounded work: a zip bomb is
 * refused by arithmetic rather than discovered by filling the disk.
 */
struct FWoTArchiveLimits
{
	uint32 MaxEntries = 65536;
	uint64 MaxTotalBytes = 8ull * 1024 * 1024 * 1024;
	uint64 MaxEntryBytes = 4ull * 1024 * 1024 * 1024;
	uint16 MaxDepth = 32;
};

/** An index that has been proven safe, plus what it will cost to extract. */
struct FWoTSafeArchivePlan
{
	EWoTArchiveLayout Layout = EWoTArchiveLayout::Win64VersionTree;
	FString RootName;
	TArray<FWoTArchiveEntry> Entries;
	uint64 TotalBytes = 0;
};

/**
 * Validate an entire index, or refuse it.
 *
 * All-or-nothing on purpose: a partially validated archive would be extracted
 * partially, and a half-extracted release is a broken installation rather than a
 * refused one.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTSafeArchivePlan> ValidateArchiveIndex(
	TConstArrayView<FWoTArchiveEntry> Entries, const FWoTArchiveLimits& Limits,
	EWoTArchiveLayout Layout);
