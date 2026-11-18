// Classification of one path component the path walk opened as itself.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

/** What the path walk knows about one component it opened as itself. */
struct FWoTReparseComponentFacts
{
	bool bReparse = false;
	uint32 Tag = 0;
	bool bDirectory = false;
	/** The component is a drive root such as "Z:\". */
	bool bDriveRoot = false;
	/** The component's volume differs from its parent directory's. */
	bool bVolumeBoundary = false;
	/** The process runs under Wine (ntdll exports wine_get_version). */
	bool bWine = false;
};

/**
 * Directory, File or SymbolicLink for one opened path component.
 *
 * Every reparse point is a link, except under Wine: there the drive root and
 * the mount points Wine presents for its Unix filesystem (a directory at a drive
 * root or volume boundary carrying one of Wine's mapping tags) are the
 * directories they stand for. Wine resolves a Unix symbolic link before Windows
 * code sees it, so such a link is never visible here at all.
 */
WOTBOOTSTRAPCORE_API EWoTTreeEntryType WoTClassifyPathComponent(
	const FWoTReparseComponentFacts& Facts);

/** The reparse tags Wine reports on its drive roots and mount points. */
WOTBOOTSTRAPCORE_API TConstArrayView<uint32> WoTWineMappingReparseTags();
