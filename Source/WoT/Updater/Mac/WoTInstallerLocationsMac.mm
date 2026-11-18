#include "../WoTInstallerLocations.h"

#if PLATFORM_MAC

#include "Mac/MacSystemIncludes.h"

FString WoTPlatformDownloadsDirectory()
{
	@autoreleasepool
	{
		NSArray<NSString*>* Paths = NSSearchPathForDirectoriesInDomains(
			NSDownloadsDirectory, NSUserDomainMask, YES);
		NSString* Path = Paths.firstObject;
		return Path == nil ? FString()
			: FString(UTF8_TO_TCHAR(Path.fileSystemRepresentation));
	}
}

#endif
