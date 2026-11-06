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
