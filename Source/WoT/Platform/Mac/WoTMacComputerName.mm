#include "../../WoTMacComputerName.h"

#include "Mac/MacSystemIncludes.h"
#include <SystemConfiguration/SystemConfiguration.h>

FString WoTMacFriendlyComputerName()
{
	// A null store asks the default dynamic store; a null encoding pointer
	// means the caller does not want the legacy encoding.
	CFStringRef Name = SCDynamicStoreCopyComputerName(nullptr, nullptr);
	if (Name == nullptr)
	{
		return FString();
	}
	FString Result(Name);
	CFRelease(Name);
	return Result;
}
