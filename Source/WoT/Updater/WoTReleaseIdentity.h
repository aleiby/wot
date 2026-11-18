#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/CanonicalJson.h"

namespace WoTReleaseIdentity
{
	struct FIdentity
	{
		uint64 ReleaseSequence = 0;
		FString ReleaseCommit;
		bool bStamped = false;
	};

	/** Parse the exact closed canonical identity document. */
	TWoTBootstrapResult<FIdentity> ParseCanonical(TConstArrayView<uint8> Bytes);

	/** Load the identity beside the running project; invalid input is unstamped. */
	FIdentity LoadRunning();

	/**
	 * Parse the exact closed canonical release-version document. Its human
	 * version is "" for an unversioned package, else a short version such as
	 * 0.1.2. A separate file from the identity: the Windows launcher outside
	 * Game/ matches release-identity.json byte-for-byte, so that file can never
	 * gain a member.
	 */
	TWoTBootstrapResult<FString> ParseHumanVersion(TConstArrayView<uint8> Bytes);

	/** The human version stamped beside the running identity; "" when absent or invalid. */
	FString LoadRunningHumanVersion();

	/** The human version stamped in a staged release tree (its live leaf); "" when absent or invalid. */
	FString LoadStagedHumanVersion(const FString& StagedTreeDir);

	/** A human version safe to show: "", or a digit then up to 31 of [0-9A-Za-z.+-]. */
	bool IsDisplayableHumanVersion(FStringView HumanVersion);

	/** The main-menu label: "v0.1.2"; "dev <commit>" for an unversioned stamped package; else "dev". */
	FString DisplayLabel(const FIdentity& Identity, const FString& HumanVersion);

#if WITH_AUTOMATION_TESTS
	class FScopedPathOverrideForTesting
	{
	public:
		explicit FScopedPathOverrideForTesting(FString InPath);
		~FScopedPathOverrideForTesting();

		FScopedPathOverrideForTesting(const FScopedPathOverrideForTesting&) = delete;
		FScopedPathOverrideForTesting& operator=(
			const FScopedPathOverrideForTesting&) = delete;

	private:
		TOptional<FString> PreviousPath;
	};
#endif
}
