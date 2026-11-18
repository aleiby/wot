#include "WoTUpdateTransports/WoTPlatformHttpHop.h"

#if PLATFORM_MAC

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Templates/Atomic.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"
#include "WoTBootstrapCore/TestOverrides.h"

// Foundation pulls CarbonCore's legacy `struct FVector` (NumberFormatting.h),
// which collides with UE's FVector alias; the engine's own MacSystemIncludes.h
// hides the alias around Apple system headers the same way.
#pragma push_macro("FVector")
#define FVector FVectorWorkaround
#import <Foundation/Foundation.h>
#pragma pop_macro("FVector")
#include <dispatch/dispatch.h>
#include <os/lock.h>
#include <atomic>

namespace
{
#if WOT_BOOTSTRAP_TEST_ENDPOINTS
// Test builds only: map exactly
// the per-tag release route plus the one exact latest-notes URL onto the strict
// loopback WOT_BOOTSTRAP_TEST_RELEASE_BASE_URL, the same rule as Win64.
bool WoTPlatformHttpHopMacRewriteReleaseUrl(
	const FString& OriginalUrl, FString& OutUrl)
{
	constexpr FStringView Prefix(
		TEXTVIEW("https://github.com/aleiby/wot/releases/download/"));
	constexpr FStringView NotesUrl(TEXTVIEW(
		"https://github.com/aleiby/wot/releases/latest/download/WoT-release-notes-v1.json"));
	FString Route;
	if (FStringView(OriginalUrl).Equals(NotesUrl, ESearchCase::CaseSensitive))
	{
		Route = TEXT("/releases/latest/download/WoT-release-notes-v1.json");
	}
	else if (!OriginalUrl.StartsWith(Prefix, ESearchCase::CaseSensitive))
	{
		return false;
	}
	else
	{
		const FStringView Suffix =
			FStringView(OriginalUrl).RightChop(Prefix.Len());
		int32 Separator = INDEX_NONE;
		if (!Suffix.FindChar(TEXT('/'), Separator) || Separator <= 0
			|| Separator == Suffix.Len() - 1
			|| Suffix.RightChop(Separator + 1).Contains(TEXTVIEW("/"))
			|| Suffix.Contains(TEXTVIEW("?")) || Suffix.Contains(TEXTVIEW("#")))
		{
			return false;
		}
		Route = TEXT("/releases/download/") + FString(Suffix);
	}
	FString BaseUrl;
	if (!WoTTryGetStrictTestLoopbackOverride(
			TEXT("WOT_BOOTSTRAP_TEST_RELEASE_BASE_URL"), BaseUrl))
	{
		return false;
	}
	while (BaseUrl.EndsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		BaseUrl.LeftChopInline(1, EAllowShrinking::No);
	}
	OutUrl = BaseUrl + Route;
	return true;
}
#endif

FString WoTPlatformHttpHopMacString(NSString* Value)
{
	if (Value == nil)
	{
		return FString();
	}
	const char* Utf8 = [Value UTF8String];
	return Utf8 != nullptr ? FString(UTF8_TO_TCHAR(Utf8)) : FString();
}

NSString* WoTPlatformHttpHopMacHeader(
	NSDictionary* Headers, NSString* WantedName)
{
	for (id Key in Headers)
	{
		if ([[Key description] caseInsensitiveCompare:WantedName]
			== NSOrderedSame)
		{
			id Value = [Headers objectForKey:Key];
			return Value != nil ? [Value description] : nil;
		}
	}
	return nil;
}

bool WoTPlatformHttpHopMacMapHeaders(
	NSHTTPURLResponse* NativeResponse,
	FWoTHttpHopResponse& OutResponse,
	bool& bOutHasContentLength,
	bool& bOutBodyLengthIsWireComparable)
{
	OutResponse = FWoTHttpHopResponse();
	OutResponse.Status = static_cast<int32>([NativeResponse statusCode]);
	NSDictionary* Headers = [NativeResponse allHeaderFields];
	OutResponse.Location = WoTPlatformHttpHopMacString(
		WoTPlatformHttpHopMacHeader(Headers, @"Location"));

	NSString* ContentLengthValue =
		WoTPlatformHttpHopMacHeader(Headers, @"Content-Length");
	const FString ContentLength =
		WoTPlatformHttpHopMacString(ContentLengthValue);
	bOutHasContentLength = ContentLengthValue != nil;
	if (bOutHasContentLength
		&& (ContentLength.IsEmpty()
			|| !LexTryParseString(OutResponse.BodyLength, *ContentLength)))
	{
		return false;
	}
	const FString ContentEncoding = WoTPlatformHttpHopMacString(
		WoTPlatformHttpHopMacHeader(Headers, @"Content-Encoding"));
	bOutBodyLengthIsWireComparable = ContentEncoding.IsEmpty()
		|| ContentEncoding.Equals(TEXT("identity"), ESearchCase::IgnoreCase);
	return true;
}

// A sink may intentionally stop a streamed response after retaining
// a resumable prefix. Keep the already-observed terminal headers available to
// core even though NSURLSession reports the sink-driven cancellation as an
// error. Invalid response headers remain fail-closed and yield an empty result.
void WoTPlatformHttpHopMacPreserveStreamedResponse(
	NSHTTPURLResponse* NativeResponse,
	uint64 StreamedBodyLength,
	FWoTHttpHopResponse& OutResponse)
{
	if (NativeResponse == nil)
	{
		return;
	}
	FWoTHttpHopResponse Response;
	bool bHasContentLength = false;
	bool bBodyLengthIsWireComparable = false;
	if (WoTPlatformHttpHopMacMapHeaders(NativeResponse, Response,
			bHasContentLength, bBodyLengthIsWireComparable))
	{
		Response.StreamedBodyLength = StreamedBodyLength;
		OutResponse = MoveTemp(Response);
	}
}
} // namespace

namespace
{
// After [task cancel], how long the executor waits for
// URLSession:task:didCompleteWithError: before detaching the delegate and
// returning. Apple always delivers it, so this only bounds a pathological
// stall (B10): the worker and shutdown through CancelAndJoin can never hang.
double GWoTPlatformHttpHopMacCancelGraceSeconds = 10.0;
#if WITH_AUTOMATION_TESTS
std::atomic<bool> GWoTPlatformHttpHopMacSuppressCompletionForTesting{false};
#endif
}

@interface FWoTPlatformHttpHopMacDelegate
	: NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
{
	dispatch_semaphore_t Completion;
	NSMutableData* BodyData;
	NSHTTPURLResponse* HttpResponse;
	NSError* TransportError;
	IWoTHttpBodySink* BodySink;
	FWoTBootstrapError* SinkError;
	uint64 StreamedBodyLength;
	bool bStreamBody;
	bool bResponseStartInvoked;
	bool bPreserveStreamedResponse; // Cooperative cancellation only.
	// Every callback runs under CallbackLock and does nothing once detached, so
	// a callback that arrives after the executor gave up never touches the
	// caller-owned BodySink (the session keeps this delegate alive meanwhile).
	os_unfair_lock CallbackLock;
	bool bDetached;
}

- (id)initWithBodySink:(IWoTHttpBodySink*)InBodySink;
- (dispatch_semaphore_t)completion;
- (NSData*)bodyData;
- (NSHTTPURLResponse*)httpResponse;
- (NSError*)transportError;
- (FWoTBootstrapError*)sinkError;
- (uint64)streamedBodyLength;
- (bool)shouldPreserveStreamedResponse;
- (void)detach;

@end

@implementation FWoTPlatformHttpHopMacDelegate

- (id)initWithBodySink:(IWoTHttpBodySink*)InBodySink
{
	self = [super init];
	if (self != nil)
	{
		Completion = dispatch_semaphore_create(0);
		BodyData = [[NSMutableData alloc] init];
		HttpResponse = nil;
		TransportError = nil;
		BodySink = InBodySink;
		SinkError = nullptr;
		StreamedBodyLength = 0;
		bStreamBody = false;
		bResponseStartInvoked = false;
		bPreserveStreamedResponse = false;
		CallbackLock = OS_UNFAIR_LOCK_INIT;
		bDetached = false;
	}
	return self;
}

- (void)dealloc
{
	[BodyData release];
	[HttpResponse release];
	[TransportError release];
	delete SinkError;
	dispatch_release(Completion);
	[super dealloc];
}

- (dispatch_semaphore_t)completion
{
	return Completion;
}

- (NSData*)bodyData
{
	return BodyData;
}

- (NSHTTPURLResponse*)httpResponse
{
	return HttpResponse;
}

- (NSError*)transportError
{
	return TransportError;
}

- (FWoTBootstrapError*)sinkError
{
	return SinkError;
}

- (uint64)streamedBodyLength
{
	return StreamedBodyLength;
}

- (bool)shouldPreserveStreamedResponse
{
	return bPreserveStreamedResponse;
}

- (void)detach
{
	os_unfair_lock_lock(&CallbackLock);
	bDetached = true;
	BodySink = nullptr;
	os_unfair_lock_unlock(&CallbackLock);
}

- (void)URLSession:(NSURLSession*)Session
	dataTask:(NSURLSessionDataTask*)DataTask
	didReceiveResponse:(NSURLResponse*)Response
	completionHandler:(void (^)(NSURLSessionResponseDisposition))CompletionHandler
{
	(void)Session;
	(void)DataTask;
	os_unfair_lock_lock(&CallbackLock);
	const NSURLSessionResponseDisposition Disposition = bDetached
		? NSURLSessionResponseCancel : [self lockedReceiveResponse:Response];
	os_unfair_lock_unlock(&CallbackLock);
	CompletionHandler(Disposition);
}

- (NSURLSessionResponseDisposition)lockedReceiveResponse:(NSURLResponse*)Response
{
	[HttpResponse release];
	HttpResponse = [(NSHTTPURLResponse*)Response retain];
	const NSInteger StatusCode = [HttpResponse statusCode];
	bStreamBody = BodySink != nullptr
		&& BodySink->ShouldStreamResponseBody(static_cast<int32>(StatusCode));
	if (bStreamBody)
	{
		FWoTHttpHopResponse HeadersOnly;
		bool bHasContentLength = false;
		bool bBodyLengthIsWireComparable = false;
		if (bResponseStartInvoked
			|| !WoTPlatformHttpHopMacMapHeaders(HttpResponse, HeadersOnly,
				bHasContentLength, bBodyLengthIsWireComparable))
		{
			SinkError = new FWoTBootstrapError(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("HTTP Content-Length is invalid")));
			return NSURLSessionResponseCancel;
		}
		bResponseStartInvoked = true;
		FWoTBootstrapStatus Judged = BodySink->OnResponseStart(HeadersOnly);
		if (Judged.IsError())
		{
			bPreserveStreamedResponse =
				BodySink->ShouldPreserveResponseOnError();
			SinkError = new FWoTBootstrapError(
				MoveTemp(Judged).GetError());
			return NSURLSessionResponseCancel;
		}
	}
	return NSURLSessionResponseAllow;
}

- (void)URLSession:(NSURLSession*)Session
	dataTask:(NSURLSessionDataTask*)DataTask
	didReceiveData:(NSData*)Data
{
	(void)Session;
	os_unfair_lock_lock(&CallbackLock);
	if (!bDetached)
	{
		[self lockedReceiveData:Data task:DataTask];
	}
	os_unfair_lock_unlock(&CallbackLock);
}

- (void)lockedReceiveData:(NSData*)Data task:(NSURLSessionDataTask*)DataTask
{
	if (!bStreamBody)
	{
		[BodyData appendData:Data];
		return;
	}
	if (SinkError != nullptr)
	{
		return;
	}

	const uint8* Bytes = static_cast<const uint8*>([Data bytes]);
	NSUInteger Remaining = [Data length];
	while (Remaining != 0)
	{
		const int32 Count = static_cast<int32>(FMath::Min<NSUInteger>(
			Remaining, static_cast<NSUInteger>(64 * 1024)));
		if (StreamedBodyLength > MAX_uint64 - static_cast<uint64>(Count))
		{
			SinkError = new FWoTBootstrapError(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("HTTP streamed body length overflowed")));
			[DataTask cancel];
			return;
		}
		FWoTBootstrapStatus Written = BodySink->Write(
			MakeArrayView(Bytes, Count));
		if (Written.IsError())
		{
			bPreserveStreamedResponse =
				BodySink->ShouldPreserveResponseOnError();
			SinkError = new FWoTBootstrapError(
				MoveTemp(Written).GetError());
			[DataTask cancel];
			return;
		}
		StreamedBodyLength += static_cast<uint64>(Count);
		Bytes += Count;
		Remaining -= static_cast<NSUInteger>(Count);
	}
}

- (void)URLSession:(NSURLSession*)Session
	task:(NSURLSessionTask*)Task
	willPerformHTTPRedirection:(NSHTTPURLResponse*)Response
	newRequest:(NSURLRequest*)RedirectRequest
	completionHandler:(void (^)(NSURLRequest*))CompletionHandler
{
	(void)Session;
	(void)Task;
	(void)Response;
	(void)RedirectRequest;
	// Load-bearing: surface the 3xx response itself; never follow it.
	CompletionHandler(nil);
}

- (void)URLSession:(NSURLSession*)Session
	task:(NSURLSessionTask*)Task
	didCompleteWithError:(NSError*)Error
{
	(void)Session;
	(void)Task;
	os_unfair_lock_lock(&CallbackLock);
	[TransportError release];
	TransportError = [Error retain];
	os_unfair_lock_unlock(&CallbackLock);
#if WITH_AUTOMATION_TESTS
	if (GWoTPlatformHttpHopMacSuppressCompletionForTesting.load())
	{
		return;
	}
#endif
	dispatch_semaphore_signal(Completion);
}

// Intentionally no didReceiveChallenge override: OS trust evaluation remains
// the NSURLSession default and cannot be weakened by this executor.

@end

namespace
{
FWoTBootstrapStatus WoTPlatformHttpHopMacFailure(
	FWoTHttpHopResponse& OutResponse, const TCHAR* Message)
{
	OutResponse = FWoTHttpHopResponse();
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::ServiceUnavailable, Message));
}
class FWoTPlatformHttpHopMacExecutor final
	: public IWoTHttpHopExecutor
{
public:
	virtual void CancelCurrentRequest() override
	{
		// NSURLSessionTask cancellation is safe from the game thread
		// while ExecuteHop waits on its private delegate queue.
		FScopeLock Lock(&ActiveTaskLock);
		bAbortRequested.Store(true);
		if (ActiveTask != nil)
		{
			[ActiveTask cancel];
		}
	}

	virtual FWoTBootstrapStatus ExecuteHop(
		const FWoTHttpHopRequest& Request,
		FWoTHttpHopResponse& OutResponse) override
	{
		OutResponse = FWoTHttpHopResponse();
		if (bAbortRequested.Load())
		{
			return WoTPlatformHttpHopMacFailure(
				OutResponse, TEXT("HTTP hop was cancelled before it began"));
		}
		@autoreleasepool
		{
			double RemainingSeconds =
				Request.AbsoluteDeadlineSeconds - FPlatformTime::Seconds();
			if (RemainingSeconds <= 0.0)
			{
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP hop deadline expired before URL parsing"));
			}

			FString EffectiveUrl = Request.Url;
			bool bTestEndpointOverride = false;
#if WOT_BOOTSTRAP_TEST_ENDPOINTS
			if (Request.Url.Equals(
					WoT::Bootstrap::Trust::AnnouncementUrl,
					ESearchCase::CaseSensitive)
				|| Request.Url.Equals(
					FString(WoT::Bootstrap::Trust::AnnouncementUrl) + TEXT(".sig"),
					ESearchCase::CaseSensitive))
			{
				const FString Override =
					FPlatformMisc::GetEnvironmentVariable(
						TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"));
				if (!Override.IsEmpty())
				{
					EffectiveUrl = Override
						+ (Request.Url.EndsWith(TEXT(".sig"),
							ESearchCase::CaseSensitive) ? TEXT(".sig") : TEXT(""));
					bTestEndpointOverride = true;
				}
			}
			else if (Request.Url.StartsWith(
					TEXT("https://github.com/aleiby/wot/releases/download/"),
					ESearchCase::CaseSensitive)
				|| Request.Url.Equals(TEXT(
					"https://github.com/aleiby/wot/releases/latest/download/WoT-release-notes-v1.json"),
					ESearchCase::CaseSensitive))
			{
				FString Rewritten;
				if (WoTPlatformHttpHopMacRewriteReleaseUrl(Request.Url, Rewritten))
				{
					EffectiveUrl = MoveTemp(Rewritten);
					bTestEndpointOverride = true;
				}
			}
#endif
			const FTCHARToUTF8 UrlUtf8(*EffectiveUrl);
			NSString* UrlString = [NSString stringWithUTF8String:UrlUtf8.Get()];
			NSURL* Url = UrlString != nil
				? [NSURL URLWithString:UrlString]
				: nil;
			if (Url == nil || [Url host] == nil
				|| [[Url user] length] != 0 || [[Url password] length] != 0)
			{
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP hop URL is invalid or contains userinfo"));
			}
			// Modern Foundation auto-percent-encodes characters it considers
			// invalid, silently REWRITING the request target and breaking the
			// byte-for-byte fidelity the policy layer signed off on. Any URL the
			// framework would not transmit verbatim is refused, never repaired.
			NSString* AbsoluteString = [Url absoluteString];
			if (AbsoluteString == nil
				|| ![AbsoluteString isEqualToString:UrlString])
			{
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("the URL would be rewritten by NSURL encoding"));
			}

			const bool bSecure =
				[[Url scheme] caseInsensitiveCompare:@"https"] == NSOrderedSame;
			if (bTestEndpointOverride
				&& ([[Url scheme] caseInsensitiveCompare:@"http"]
						!= NSOrderedSame
					|| ![[Url host] isEqualToString:@"127.0.0.1"]))
			{
				return WoTPlatformHttpHopMacFailure(
					OutResponse,
					TEXT("the Test installer override must use loopback HTTP"));
			}
			if (!bSecure)
			{
#if WOT_BOOTSTRAP_TEST_ENDPOINTS
				if ([[Url scheme] caseInsensitiveCompare:@"http"] != NSOrderedSame
					|| (!Request.bAllowPlainHttpForLoopbackTesting
						&& !bTestEndpointOverride)
					|| ![[Url host] isEqualToString:@"127.0.0.1"])
				{
					return WoTPlatformHttpHopMacFailure(
						OutResponse,
						TEXT("plain HTTP is restricted to guarded loopback tests"));
				}
#else
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP hop URL must use HTTPS"));
#endif
			}

			// Native timeouts derive from the remaining budget measured AFTER
			// URL parsing, not from the stale pre-parse value.
			RemainingSeconds =
				Request.AbsoluteDeadlineSeconds - FPlatformTime::Seconds();
			if (RemainingSeconds <= 0.0)
			{
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP hop deadline expired before the session"));
			}
			NSURLSessionConfiguration* Configuration =
				[NSURLSessionConfiguration ephemeralSessionConfiguration];
			[Configuration setURLCache:nil];
			[Configuration setHTTPCookieStorage:nil];
			[Configuration setURLCredentialStorage:nil];
			[Configuration setHTTPShouldSetCookies:NO];
			[Configuration setRequestCachePolicy:
				NSURLRequestReloadIgnoringLocalCacheData];
			[Configuration setWaitsForConnectivity:NO];
			// ATS exceptions, if raw loopback HTTP requires one, must be verified
			// on macOS; production HTTPS never receives an exception.
			const NSTimeInterval Timeout =
				static_cast<NSTimeInterval>(FMath::Max(0.001, RemainingSeconds));
			[Configuration setTimeoutIntervalForRequest:Timeout];
			[Configuration setTimeoutIntervalForResource:Timeout];

			NSMutableURLRequest* NativeRequest =
				[[NSMutableURLRequest alloc] initWithURL:Url
					cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
					timeoutInterval:Timeout];
			[NativeRequest setHTTPMethod:@"GET"];


			FWoTPlatformHttpHopMacDelegate* Delegate =
				[[FWoTPlatformHttpHopMacDelegate alloc]
					initWithBodySink:Request.BodySink];
			NSOperationQueue* DelegateQueue = [[NSOperationQueue alloc] init];
			[DelegateQueue setMaxConcurrentOperationCount:1];
			NSURLSession* Session =
				[NSURLSession sessionWithConfiguration:Configuration
					delegate:Delegate
					delegateQueue:DelegateQueue];
			NSURLSessionDataTask* Task =
				[Session dataTaskWithRequest:NativeRequest];
			{
				FScopeLock Lock(&ActiveTaskLock);
				ActiveTask = Task;
				if (bAbortRequested.Load())
				{
					[Task cancel];
				}
			}
			ON_SCOPE_EXIT
			{
				FScopeLock Lock(&ActiveTaskLock);
				if (ActiveTask == Task)
				{
					ActiveTask = nil;
				}
			};
			// The last check before any network I/O can begin: a request whose
			// deadline lapsed during setup is never resumed.
			if (FPlatformTime::Seconds() >= Request.AbsoluteDeadlineSeconds)
			{
				[Session invalidateAndCancel];
				[DelegateQueue waitUntilAllOperationsAreFinished];
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP hop deadline expired before resume"));
			}
			[Task resume];

			RemainingSeconds =
				Request.AbsoluteDeadlineSeconds - FPlatformTime::Seconds();
			bool bCompletionObserved = false;
			bool bTimedOut = RemainingSeconds <= 0.0;
			if (!bTimedOut)
			{
				const double MaximumDispatchSeconds =
					static_cast<double>(MAX_int64) /
					static_cast<double>(NSEC_PER_SEC);
				const double BoundedSeconds = FMath::Min(
					RemainingSeconds, MaximumDispatchSeconds);
				const int64 WaitNanoseconds = FMath::CeilToInt64(
					BoundedSeconds * static_cast<double>(NSEC_PER_SEC));
				const dispatch_time_t WaitUntil =
					dispatch_time(DISPATCH_TIME_NOW, WaitNanoseconds);
				bCompletionObserved = dispatch_semaphore_wait(
					[Delegate completion], WaitUntil) == 0;
				bTimedOut = !bCompletionObserved;
			}

			if (bTimedOut
				|| FPlatformTime::Seconds()
					>= Request.AbsoluteDeadlineSeconds)
			{
				[Task cancel];
				[Session finishTasksAndInvalidate];
				if (!bCompletionObserved)
				{
					const int64 GraceNanoseconds = FMath::CeilToInt64(
						FMath::Clamp(GWoTPlatformHttpHopMacCancelGraceSeconds,
							0.0, 3600.0) * static_cast<double>(NSEC_PER_SEC));
					bCompletionObserved = dispatch_semaphore_wait(
						[Delegate completion],
						dispatch_time(DISPATCH_TIME_NOW, GraceNanoseconds)) == 0;
				}
				if (!bCompletionObserved)
				{
					// B10: the cancelled task never reported completion. Detach
					// so a late callback cannot reach the caller's sink, then
					// return without waiting on the delegate queue. The session
					// retains the delegate and queue until the task ends.
					[Delegate detach];
					[NativeRequest release];
					[Delegate release];
					[DelegateQueue release];
					return WoTPlatformHttpHopMacFailure(OutResponse,
						TEXT("NSURLSession did not complete after cancellation"));
				}
				[DelegateQueue waitUntilAllOperationsAreFinished];
				FWoTBootstrapError* RecordedSinkError = [Delegate sinkError];
				TOptional<FWoTBootstrapError> SinkError;
				if (RecordedSinkError != nullptr)
				{
					SinkError.Emplace(MoveTemp(*RecordedSinkError));
					if ([Delegate shouldPreserveStreamedResponse])
					{
						// Retain only cooperative-cancel headers.
						WoTPlatformHttpHopMacPreserveStreamedResponse(
							[Delegate httpResponse], [Delegate streamedBodyLength],
							OutResponse);
					}
				}
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				if (SinkError.IsSet())
				{
					return FWoTBootstrapStatus::Error(
						MoveTemp(SinkError.GetValue()));
				}
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("NSURLSession request exceeded the deadline"));
			}
			[Session finishTasksAndInvalidate];
			[DelegateQueue waitUntilAllOperationsAreFinished];
			if ([Delegate sinkError] != nullptr)
			{
				FWoTBootstrapError SinkError = MoveTemp(*[Delegate sinkError]);
				if ([Delegate shouldPreserveStreamedResponse])
				{
					// Retain only cooperative-cancel headers.
					WoTPlatformHttpHopMacPreserveStreamedResponse(
						[Delegate httpResponse], [Delegate streamedBodyLength],
						OutResponse);
				}
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				return FWoTBootstrapStatus::Error(MoveTemp(SinkError));
			}
			if ([Delegate transportError] != nil
				|| [Delegate httpResponse] == nil)
			{
				const bool bPreserveCancelledPrefix = bAbortRequested.Load()
					&& Request.BodySink != nullptr
					&& Request.BodySink->ShouldPreserveResponseOnError();
				if (bPreserveCancelledPrefix)
				{
					// Native task cancellation may bypass the sink; keep
					// the headers for the prefix already accepted before cancellation.
					WoTPlatformHttpHopMacPreserveStreamedResponse(
						[Delegate httpResponse], [Delegate streamedBodyLength],
						OutResponse);
				}
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				if (bPreserveCancelledPrefix && OutResponse.Status != 0)
				{
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::ServiceUnavailable,
						TEXT("NSURLSession request was cancelled")));
				}
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("NSURLSession transport failed"));
			}

			FWoTHttpHopResponse Response;
			NSHTTPURLResponse* NativeResponse = [Delegate httpResponse];
			bool bHasContentLength = false;
			bool bBodyLengthIsWireComparable = false;
			if (!WoTPlatformHttpHopMacMapHeaders(NativeResponse, Response,
					bHasContentLength, bBodyLengthIsWireComparable))
			{
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP Content-Length is invalid"));
			}

			const uint64 DeclaredBodyLength = Response.BodyLength;
			const bool bStreamBody = Request.BodySink != nullptr
				&& Request.BodySink->ShouldStreamResponseBody(Response.Status);
			NSData* Data = [Delegate bodyData];
			const NSUInteger BodyLength = [Data length];
			if (BodyLength > static_cast<NSUInteger>(MAX_int32))
			{
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP response body exceeds the container bound"));
			}
			Response.Body.Append(
				static_cast<const uint8*>([Data bytes]),
				static_cast<int32>(BodyLength));
			const uint64 DeliveredBodyLength = bStreamBody
				? [Delegate streamedBodyLength]
				: static_cast<uint64>(Response.Body.Num());
			// NSURLSession transparently decodes encoded representations. Their
			// declared wire length remains observable, but only identity bodies
			// can be length-compared to the delivered NSData.
			// 204/304 are DEFINED to carry no body while their Content-Length
			// may describe the corresponding representation (RFC 9110 §8.6).
			const bool bStatusHasNoBody =
				Response.Status == 204 || Response.Status == 304;
			if (bHasContentLength && bBodyLengthIsWireComparable
				&& !bStatusHasNoBody
				&& DeclaredBodyLength != DeliveredBodyLength)
			{
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP response ended before Content-Length"));
			}
			if (FPlatformTime::Seconds()
				>= Request.AbsoluteDeadlineSeconds)
			{
				[NativeRequest release];
				[Delegate release];
				[DelegateQueue release];
				return WoTPlatformHttpHopMacFailure(
					OutResponse, TEXT("HTTP response mapping exceeded the deadline"));
			}
			if (bStreamBody)
			{
				Response.StreamedBodyLength = DeliveredBodyLength;
			}

			[NativeRequest release];
			[Delegate release];
			[DelegateQueue release];
			OutResponse = MoveTemp(Response);
			return FWoTBootstrapStatus::Success();
		}
	}

private:
	FCriticalSection ActiveTaskLock;
	NSURLSessionDataTask* ActiveTask = nil;
	TAtomic<bool> bAbortRequested{false};
};
} // namespace

#if WOT_BOOTSTRAP_TEST_ENDPOINTS
bool WoTPlatformHttpHopRewriteReleaseUrlForTesting(
	const FString& OriginalUrl, FString& OutUrl)
{
	return WoTPlatformHttpHopMacRewriteReleaseUrl(OriginalUrl, OutUrl);
}
#endif

TUniquePtr<IWoTHttpHopExecutor> WoTCreatePlatformHttpHopExecutor()
{
	return MakeUnique<FWoTPlatformHttpHopMacExecutor>();
}

#if WITH_AUTOMATION_TESTS
void WoTPlatformHttpHopMacSetCancelStallForTesting(
	bool bSuppressCompletion, double GraceSeconds)
{
	GWoTPlatformHttpHopMacSuppressCompletionForTesting.store(bSuppressCompletion);
	GWoTPlatformHttpHopMacCancelGraceSeconds = GraceSeconds;
}
#endif

#endif // PLATFORM_MAC
