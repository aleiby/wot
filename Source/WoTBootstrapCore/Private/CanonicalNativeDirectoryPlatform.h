#pragma once

#include "WoTBootstrapCore/NativeHandles.h"

/** Platform open used only while canonicalizing a directory above an owned root. */
TWoTBootstrapResult<FWoTNativeDirectoryHandle>
	WoTOpenNativeDirectoryFollowingForCanonicalization(
		FStringView LaunchAbsolutePath);
