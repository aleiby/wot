#include "WoTMetalLayerRefresh.h"

#if PLATFORM_MAC && !WITH_MULTI_VIEW_SLATE_WINDOW_SUPPORT

#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Mac/CocoaThread.h"
#include "Misc/App.h"
#include "Misc/CoreMisc.h"
#include "Misc/ScopeLock.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"

#include <atomic>

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <dispatch/dispatch.h>

DEFINE_LOG_CATEGORY_STATIC(LogWoTMetalLayerRefresh, Log, All);

static TAutoConsoleVariable<int32> CVarWoTMacMetalLayerRefresh(
	TEXT("wot.MacMetalLayerRefresh"), 1,
	TEXT("0 = disable Mac Metal layer recovery after resizes or frame stalls."));

namespace
{
// The lock covers answers and cancellation, including a swap already in progress.
FCriticalSection GWoTMetalLayerRefreshLock;
bool GWoTMetalLayerRefreshActive = false;
bool GWoTMetalLayerRefreshEnabled = false;
uint64 GWoTMetalLayerRefreshGeneration = 0;
bool GWoTMetalLayerRefreshHasAnswer = false;
EWoTMetalLayerRefreshResult GWoTMetalLayerRefreshAnswer = EWoTMetalLayerRefreshResult::Unavailable;
std::atomic<int32> GWoTMetalLayerRefreshCount{0};

// Only the game thread uses the policy, clock and ticker handle.
FWoTMetalLayerRefreshPolicy GWoTMetalLayerRefreshPolicy;
FTSTicker::FDelegateHandle GWoTMetalLayerRefreshTicker;
double GWoTMetalLayerRefreshPreviousSeconds = 0.0;
bool GWoTMetalLayerRefreshHasPreviousTick = false;

EWoTMetalLayerRefreshResult WoTMacRefreshGameMetalLayer(void* NativeWindowHandle)
{
	// Keep the handle unowned until AppKit confirms that the window still exists.
	NSWindow* GameNativeWindow = nil;
	for (NSWindow* CandidateWindow in [NSApp windows])
	{
		if (static_cast<void*>(CandidateWindow) == NativeWindowHandle)
		{
			GameNativeWindow = CandidateWindow;
			break;
		}
	}
	if (GameNativeWindow == nil)
	{
		return EWoTMetalLayerRefreshResult::Unavailable;
	}
	if ([GameNativeWindow inLiveResize])
	{
		return EWoTMetalLayerRefreshResult::Deferred;
	}

	NSView* GameContentView = [GameNativeWindow contentView];
	CALayer* ContentLayer = [GameContentView layer];
	if (![ContentLayer isKindOfClass:[CAMetalLayer class]])
	{
		return EWoTMetalLayerRefreshResult::Unavailable;
	}

	CAMetalLayer* PreviousMetalLayer = static_cast<CAMetalLayer*>(ContentLayer);
	CAMetalLayer* ReplacementMetalLayer = [[CAMetalLayer alloc] init];
	if (ReplacementMetalLayer == nil)
	{
		return EWoTMetalLayerRefreshResult::Unavailable;
	}
	ReplacementMetalLayer.device = PreviousMetalLayer.device;
	ReplacementMetalLayer.pixelFormat = PreviousMetalLayer.pixelFormat;
	ReplacementMetalLayer.colorspace = PreviousMetalLayer.colorspace;
	ReplacementMetalLayer.wantsExtendedDynamicRangeContent = PreviousMetalLayer.wantsExtendedDynamicRangeContent;
	if (PreviousMetalLayer.EDRMetadata != nil)
	{
		ReplacementMetalLayer.EDRMetadata = PreviousMetalLayer.EDRMetadata;
	}
	ReplacementMetalLayer.framebufferOnly = PreviousMetalLayer.framebufferOnly;
	ReplacementMetalLayer.presentsWithTransaction = PreviousMetalLayer.presentsWithTransaction;
	ReplacementMetalLayer.displaySyncEnabled = PreviousMetalLayer.displaySyncEnabled;
	ReplacementMetalLayer.allowsNextDrawableTimeout = PreviousMetalLayer.allowsNextDrawableTimeout;
	ReplacementMetalLayer.maximumDrawableCount = PreviousMetalLayer.maximumDrawableCount;
	ReplacementMetalLayer.opaque = PreviousMetalLayer.opaque;
	ReplacementMetalLayer.contentsScale = PreviousMetalLayer.contentsScale;
	ReplacementMetalLayer.contentsGravity = PreviousMetalLayer.contentsGravity;
	ReplacementMetalLayer.magnificationFilter = PreviousMetalLayer.magnificationFilter;
	ReplacementMetalLayer.minificationFilter = PreviousMetalLayer.minificationFilter;
	ReplacementMetalLayer.edgeAntialiasingMask = PreviousMetalLayer.edgeAntialiasingMask;
	ReplacementMetalLayer.masksToBounds = PreviousMetalLayer.masksToBounds;
	ReplacementMetalLayer.backgroundColor = PreviousMetalLayer.backgroundColor;
	ReplacementMetalLayer.anchorPoint = PreviousMetalLayer.anchorPoint;
	ReplacementMetalLayer.bounds = PreviousMetalLayer.bounds;
	ReplacementMetalLayer.position = PreviousMetalLayer.position;
	ReplacementMetalLayer.autoresizingMask = PreviousMetalLayer.autoresizingMask;
	ReplacementMetalLayer.hidden = PreviousMetalLayer.hidden;
	ReplacementMetalLayer.opacity = PreviousMetalLayer.opacity;
	// Last, after the geometry, so no bounds or scale change can touch it.
	ReplacementMetalLayer.drawableSize = PreviousMetalLayer.drawableSize;
	[ReplacementMetalLayer removeAllAnimations];

	// Rendering may still hold a borrowed pointer to the previous layer.
	[PreviousMetalLayer retain];
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	[GameContentView setLayer:ReplacementMetalLayer];
	[CATransaction commit];
	[ReplacementMetalLayer release];

	// The delayed block balances our retain; the view owns the replacement.
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64>(10ULL * NSEC_PER_SEC)),
		dispatch_get_main_queue(), ^{
			[PreviousMetalLayer release];
		});

	GWoTMetalLayerRefreshCount.fetch_add(1, std::memory_order_relaxed);
	UE_LOG(LogWoTMetalLayerRefresh, Log, TEXT("Refreshed the game window's Metal layer."));
	return EWoTMetalLayerRefreshResult::Refreshed;
}

bool WoTMacTickMetalLayerRefresh(float)
{
	const double NowSeconds = FPlatformTime::Seconds();
	const double FrameSeconds = GWoTMetalLayerRefreshHasPreviousTick
		? NowSeconds - GWoTMetalLayerRefreshPreviousSeconds : 0.0;
	GWoTMetalLayerRefreshPreviousSeconds = NowSeconds;
	GWoTMetalLayerRefreshHasPreviousTick = true;
	const bool bRecoveryEnabled = CVarWoTMacMetalLayerRefresh.GetValueOnGameThread() != 0;
	uint64 RequestGeneration;
	{
		FScopeLock StateLock(&GWoTMetalLayerRefreshLock);
		if (GWoTMetalLayerRefreshHasAnswer)
		{
			GWoTMetalLayerRefreshPolicy.OnResult(NowSeconds, GWoTMetalLayerRefreshAnswer);
			GWoTMetalLayerRefreshHasAnswer = false;
		}
		GWoTMetalLayerRefreshEnabled = bRecoveryEnabled;
		if (!bRecoveryEnabled)
		{
			// Cancel queued requests so re-enabling starts from a fresh baseline.
			++GWoTMetalLayerRefreshGeneration;
			GWoTMetalLayerRefreshPolicy = FWoTMetalLayerRefreshPolicy();
			return true;
		}
		RequestGeneration = GWoTMetalLayerRefreshGeneration;
	}

	FIntPoint GameViewportSize = FIntPoint::ZeroValue;
	TSharedPtr<SWindow> GameSlateWindow;
	UGameViewportClient* GameViewportClient = GEngine ? GEngine->GameViewport.Get() : nullptr;
	if (GameViewportClient)
	{
		GameSlateWindow = GameViewportClient->GetWindow();
		if (GameViewportClient->Viewport)
		{
			GameViewportSize = GameViewportClient->Viewport->GetSizeXY();
		}
	}
	const bool bStallWatchAllowed = GameSlateWindow.IsValid()
		&& GameSlateWindow->IsVisible() && !GameSlateWindow->IsWindowMinimized()
		&& FPlatformApplicationMisc::IsThisApplicationForeground();
	const EWoTMetalLayerRefreshReason RefreshReason = GWoTMetalLayerRefreshPolicy.Tick(
		NowSeconds, FrameSeconds, GameViewportSize, bStallWatchAllowed);
	if (RefreshReason == EWoTMetalLayerRefreshReason::None)
	{
		return true;
	}

	const TSharedPtr<FGenericWindow> GameNativeWindow = GameSlateWindow.IsValid()
		? GameSlateWindow->GetNativeWindow() : nullptr;
	void* const NativeWindowHandle = GameNativeWindow.IsValid()
		? GameNativeWindow->GetOSWindowHandle() : nullptr;
	if (!NativeWindowHandle)
	{
		GWoTMetalLayerRefreshPolicy.OnResult(NowSeconds, EWoTMetalLayerRefreshResult::Unavailable);
		return true;
	}

	// MainThreadCall can also dispatch during event tracking. Restrict the swap itself
	// to the default run-loop mode, without waiting on the game thread.
	MainThreadCall(^{
		[[NSRunLoop mainRunLoop] performInModes:@[NSDefaultRunLoopMode] block:^{
			FScopeLock StateLock(&GWoTMetalLayerRefreshLock);
			if (!GWoTMetalLayerRefreshActive || !GWoTMetalLayerRefreshEnabled
				|| RequestGeneration != GWoTMetalLayerRefreshGeneration)
			{
				return;
			}
			GWoTMetalLayerRefreshAnswer = WoTMacRefreshGameMetalLayer(NativeWindowHandle);
			GWoTMetalLayerRefreshHasAnswer = true;
		}];
	}, false, NSDefaultRunLoopMode);
	return true;
}
}

void WoTInstallMetalLayerRefresh()
{
	if (IsRunningCommandlet() || IsRunningDedicatedServer() || GIsEditor || !FApp::CanEverRender())
	{
		return;
	}
	if (GWoTMetalLayerRefreshTicker.IsValid())
	{
		return;
	}
	{
		FScopeLock StateLock(&GWoTMetalLayerRefreshLock);
		++GWoTMetalLayerRefreshGeneration;
		GWoTMetalLayerRefreshActive = true;
		GWoTMetalLayerRefreshEnabled = CVarWoTMacMetalLayerRefresh.GetValueOnGameThread() != 0;
		GWoTMetalLayerRefreshHasAnswer = false;
	}
	GWoTMetalLayerRefreshPolicy = FWoTMetalLayerRefreshPolicy();
	GWoTMetalLayerRefreshHasPreviousTick = false;
	GWoTMetalLayerRefreshTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateStatic(&WoTMacTickMetalLayerRefresh));
}

void WoTShutdownMetalLayerRefresh()
{
	{
		FScopeLock StateLock(&GWoTMetalLayerRefreshLock);
		GWoTMetalLayerRefreshActive = false;
		GWoTMetalLayerRefreshEnabled = false;
		++GWoTMetalLayerRefreshGeneration;
		GWoTMetalLayerRefreshHasAnswer = false;
	}
	if (GWoTMetalLayerRefreshTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GWoTMetalLayerRefreshTicker);
		GWoTMetalLayerRefreshTicker.Reset();
	}
}

int32 WoTGetMetalLayerRefreshCount()
{
	return GWoTMetalLayerRefreshCount.load(std::memory_order_relaxed);
}

#endif // PLATFORM_MAC && !WITH_MULTI_VIEW_SLATE_WINDOW_SUPPORT
