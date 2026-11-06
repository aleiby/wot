#pragma once

#include "CoreMinimal.h"

/** Resolves and identity-canonicalizes the current user's Downloads directory. */
FString WoTResolveDownloadsDirectory();

/** Pinned installer name followed by browser duplicate names (1) through (9). */
TArray<FString> WoTDownloadsInstallerCandidates(
	const FString& DownloadsDirectory);

/** Platform discovery before shared native canonicalization. */
FString WoTPlatformDownloadsDirectory();
