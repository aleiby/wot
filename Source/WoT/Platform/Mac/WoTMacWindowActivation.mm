#include "WoTWindowActivation.h"

#if PLATFORM_MAC

#include "HAL/PlatformTime.h"
#include "Mac/CocoaThread.h"
#import <AppKit/AppKit.h>
#include <atomic>

namespace
{
std::atomic<double> GWoTLastWindowMouseDown{-1.0};
id GWoTWindowActivationMonitor = nil;
constexpr double WoTWindowClickActivationSeconds = 0.5;
}

void WoTInstallWindowActivationMonitor()
{
	MainThreadCall(^{
		if (GWoTWindowActivationMonitor == nil)
		{
			GWoTWindowActivationMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:
				NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown
				handler:^NSEvent*(NSEvent* MouseEvent)
				{
					GWoTLastWindowMouseDown.store(FPlatformTime::Seconds(), std::memory_order_relaxed);
					return MouseEvent;
				}];
		}
	}, false, NSDefaultRunLoopMode);
}

void WoTShutdownWindowActivationMonitor()
{
	MainThreadCall(^{
		if (GWoTWindowActivationMonitor != nil)
		{
			[NSEvent removeMonitor:GWoTWindowActivationMonitor];
			GWoTWindowActivationMonitor = nil;
		}
		GWoTLastWindowMouseDown.store(-1.0, std::memory_order_relaxed);
	}, true, NSDefaultRunLoopMode);
}

bool WoTWasWindowActivatedByClick()
{
	if ([NSEvent pressedMouseButtons] != 0)
	{
		return true;
	}
	const double LastMouseDown = GWoTLastWindowMouseDown.load(std::memory_order_relaxed);
	const double ElapsedSeconds = FPlatformTime::Seconds() - LastMouseDown;
	return LastMouseDown >= 0.0 && ElapsedSeconds >= 0.0
		&& ElapsedSeconds <= WoTWindowClickActivationSeconds;
}

#endif
