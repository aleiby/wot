#include "WoTBootstrapCore/ReparseClassification.h"

namespace
{
// Measured with Tools/deck/probe under Proton 11.0 (Wine 11.0) on SteamOS 3.9:
// Wine reports IO_REPARSE_TAG_MOUNT_POINT on Z:\ and on every mounted
// filesystem beneath it (for example /home), each with its own volume serial.
constexpr uint32 GWoTWineMappingReparseTags[] = {0xA0000003u};
}

TConstArrayView<uint32> WoTWineMappingReparseTags()
{
	return TConstArrayView<uint32>(GWoTWineMappingReparseTags);
}

EWoTTreeEntryType WoTClassifyPathComponent(const FWoTReparseComponentFacts& Facts)
{
	if (!Facts.bReparse)
	{
		return Facts.bDirectory ? EWoTTreeEntryType::Directory : EWoTTreeEntryType::File;
	}
	if (Facts.bWine && Facts.bDirectory && (Facts.bDriveRoot || Facts.bVolumeBoundary)
		&& WoTWineMappingReparseTags().Contains(Facts.Tag))
	{
		return EWoTTreeEntryType::Directory;
	}
	return EWoTTreeEntryType::SymbolicLink;
}
