#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/CanonicalJson.h"

enum class EWoTReleaseNotesOutcome : uint8
{
	Selected,
	NoEntry,
	Unavailable,
	Invalid,
	NotAttempted,
};

struct FWoTReleaseNotesResult
{
	EWoTReleaseNotesOutcome Outcome = EWoTReleaseNotesOutcome::NotAttempted;
	FString ReleaseCommit;
	uint64 ReleaseSequence = 0;
	FString Text;
};

inline const TCHAR* LexToString(EWoTReleaseNotesOutcome Outcome)
{
	switch (Outcome)
	{
	case EWoTReleaseNotesOutcome::Selected: return TEXT("selected");
	case EWoTReleaseNotesOutcome::NoEntry: return TEXT("no-entry");
	case EWoTReleaseNotesOutcome::Unavailable: return TEXT("unavailable");
	case EWoTReleaseNotesOutcome::Invalid: return TEXT("invalid");
	case EWoTReleaseNotesOutcome::NotAttempted: return TEXT("not-attempted");
	}
	return TEXT("not-attempted");
}

namespace WoTReleaseNotes
{
	inline constexpr TCHAR LatestUrl[] =
		TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-release-notes-v1.json");
	inline constexpr uint32 MaximumFileBytes = 256 * 1024;
	inline constexpr uint32 MaximumEntryBytes = 16 * 1024;
	inline constexpr int32 MaximumEntries = 64;

	/** Strictly parse the cumulative canonical asset and select one sequence. */
	TWoTBootstrapResult<bool> ParseAndSelect(TConstArrayView<uint8> Bytes,
		uint64 ReleaseSequence, FString& OutText);
}
