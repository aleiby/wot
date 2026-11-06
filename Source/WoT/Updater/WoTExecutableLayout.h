// One immutable interpretation of the running executable's guarded and update
// layouts. The guarded fields are required to start; update fields are optional.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

struct FWoTResolvedExecutableLayout
{
	FString UserStateRoot;
	FString ContentCacheRoot;
	FString InstallRoot;
	FString LiveLeaf;
	FString InstalledTreeRoot;
	FString ExecutableTreeRelativePath;
	FString CanonicalAdjacentInstallerPath;
	/** Empty when updates are available for this copy. */
	FString UpdateDisableReason;
	FWoTNativeFileIdentity UserStateRootIdentity;
	/** Canonical parent of the launched executable tree, retained even when the
	 * optional updater layout is disabled. All adjacent-installer operations
	 * authenticate against this identity. */
	FWoTNativeFileIdentity AdjacentInstallerParentIdentity;
	FWoTNativeFileIdentity InstallRootIdentity;
	bool bHasUserStateRootIdentity = false;
	bool bHasAdjacentInstallerParentIdentity = false;
	bool bHasInstallRootIdentity = false;

	/** The pre-existing guarded layout required for the game itself. */
	bool IsValid() const
	{
		return !UserStateRoot.IsEmpty() && !ContentCacheRoot.IsEmpty()
			&& !InstalledTreeRoot.IsEmpty()
			&& !ExecutableTreeRelativePath.IsEmpty();
	}

	/** Optional per-copy layout required only by the in-place updater. */
	bool CanUpdate() const
	{
		return IsValid() && UpdateDisableReason.IsEmpty()
			&& !InstallRoot.IsEmpty() && !LiveLeaf.IsEmpty()
			&& !CanonicalAdjacentInstallerPath.IsEmpty()
			&& bHasInstallRootIdentity;
	}
};
