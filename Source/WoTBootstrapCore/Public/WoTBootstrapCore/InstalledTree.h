// The signed inventory of an installed tree, and its derived root digest.
//
// The question this answers is "is this installation still exactly what was
// signed", so the inventory must describe shape as well as content: a tree with an
// extra file, a file turned into a symlink, or a mode quietly widened is not the
// tree that was signed even if every listed digest still matches.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

inline constexpr TCHAR WoTInstalledTreeV1Format[] = TEXT("wot-installed-tree-v1");
inline constexpr uint32 WoTInstalledTreeVersion = 1;

/**
 * Sidecars are never inventory entries.
 *
 * The inventory cannot describe itself: including it would make its own digest an
 * input to itself. These names are therefore excluded by rule, and an inventory
 * that lists one is refused rather than filtered — a signer that emitted it did
 * something the format does not mean.
 */
WOTBOOTSTRAPCORE_API bool IsInstalledTreeSidecarPath(FStringView Path);

/** True for the exact, case-sensitive AppleDouble component spellings. */
WOTBOOTSTRAPCORE_API bool IsAppleDoublePathComponent(FStringView Component);

WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTInstalledTreeManifest> ParseInstalledTreeV1(
	TConstArrayView<uint8> CanonicalBytes);

/**
 * Derive the tree's root digest.
 *
 * A full lower-case SHA-256 over a canonical projection of every entry, computed
 * by the verifier — never a value read from the document, and never a path
 * fragment supplied by the network.
 */
WOTBOOTSTRAPCORE_API TWoTBootstrapResult<FWoTSha256> ComputeInstalledTreeRoot(
	const FWoTInstalledTreeManifest& Inventory);
