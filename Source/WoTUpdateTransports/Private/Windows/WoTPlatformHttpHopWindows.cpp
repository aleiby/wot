#include "WoTUpdateTransports/WoTPlatformHttpHop.h"

#if PLATFORM_WINDOWS

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"
#include "WoTBootstrapCore/TestOverrides.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <winhttp.h>
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTPlatformHttpHop, Log, All);

namespace
{
#if WITH_AUTOMATION_TESTS
TAtomic<bool> GWoTPlatformHttpHopFailConnectRetriesForTesting{false};
#endif
// The connect-retries diagnostic is written once per process, not once per hop.
TAtomic<bool> GWoTPlatformHttpHopConnectRetriesReported{false};

class FWoTPlatformHttpHopWindowsHandle
{
public:
	explicit FWoTPlatformHttpHopWindowsHandle(HINTERNET InHandle = nullptr)
		: Handle(InHandle)
	{
	}

	~FWoTPlatformHttpHopWindowsHandle()
	{
		Close();
	}

	FWoTPlatformHttpHopWindowsHandle(
		const FWoTPlatformHttpHopWindowsHandle&) = delete;
	FWoTPlatformHttpHopWindowsHandle& operator=(
		const FWoTPlatformHttpHopWindowsHandle&) = delete;

	HINTERNET Get() const
	{
		return Handle.Load();
	}

	void Close()
	{
		if (HINTERNET ToClose = Handle.Exchange(nullptr); ToClose != nullptr)
		{
			::WinHttpCloseHandle(ToClose);
		}
	}

private:
	TAtomic<HINTERNET> Handle{nullptr};
};

FWoTBootstrapStatus WoTPlatformHttpHopWindowsFailure(
	FWoTHttpHopResponse& OutResponse, const TCHAR* Message)
{
	OutResponse = FWoTHttpHopResponse();
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::ServiceUnavailable, Message));
}

bool WoTPlatformHttpHopWindowsRemainingMilliseconds(
	double AbsoluteDeadlineSeconds, int32& OutMilliseconds)
{
	const double RemainingSeconds =
		AbsoluteDeadlineSeconds - FPlatformTime::Seconds();
	if (RemainingSeconds <= 0.0)
	{
		return false;
	}
	const int64 RemainingMilliseconds = FMath::CeilToInt64(
		RemainingSeconds * 1000.0);
	OutMilliseconds = static_cast<int32>(FMath::Clamp<int64>(
		RemainingMilliseconds, 1, MAX_int32));
	return true;
}

bool WoTPlatformHttpHopWindowsSetRemainingTimeouts(
	HINTERNET Handle, double AbsoluteDeadlineSeconds)
{
	int32 RemainingMilliseconds = 0;
	return WoTPlatformHttpHopWindowsRemainingMilliseconds(
			AbsoluteDeadlineSeconds, RemainingMilliseconds)
		&& ::WinHttpSetTimeouts(Handle,
			RemainingMilliseconds,
			RemainingMilliseconds,
			RemainingMilliseconds,
			RemainingMilliseconds) != 0;
}

bool WoTPlatformHttpHopWindowsDeadlineRemains(
	double AbsoluteDeadlineSeconds)
{
	int32 IgnoredMilliseconds = 0;
	return WoTPlatformHttpHopWindowsRemainingMilliseconds(
		AbsoluteDeadlineSeconds, IgnoredMilliseconds);
}

#if WOT_BOOTSTRAP_TEST_ENDPOINTS
bool WoTPlatformHttpHopWindowsRewriteReleaseUrl(
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

enum class EWoTPlatformHttpHopWindowsHeaderResult : uint8
{
	Found,
	Absent,
	Failure,
};

EWoTPlatformHttpHopWindowsHeaderResult
WoTPlatformHttpHopWindowsQueryHeader(
	HINTERNET RequestHandle, const wchar_t* Name, FString& OutValue)
{
	OutValue.Reset();
	DWORD ByteCount = 0;
	::SetLastError(ERROR_SUCCESS);
	if (::WinHttpQueryHeaders(RequestHandle, WINHTTP_QUERY_CUSTOM,
			Name, WINHTTP_NO_OUTPUT_BUFFER, &ByteCount,
			WINHTTP_NO_HEADER_INDEX) != 0)
	{
		return EWoTPlatformHttpHopWindowsHeaderResult::Found;
	}
	const DWORD QueryError = ::GetLastError();
	if (QueryError == ERROR_WINHTTP_HEADER_NOT_FOUND)
	{
		return EWoTPlatformHttpHopWindowsHeaderResult::Absent;
	}
	if (QueryError != ERROR_INSUFFICIENT_BUFFER || ByteCount == 0)
	{
		return EWoTPlatformHttpHopWindowsHeaderResult::Failure;
	}

	TArray<wchar_t> Buffer;
	Buffer.SetNumZeroed(
		static_cast<int32>((ByteCount + sizeof(wchar_t) - 1) / sizeof(wchar_t)));
	if (::WinHttpQueryHeaders(RequestHandle, WINHTTP_QUERY_CUSTOM,
			Name, Buffer.GetData(), &ByteCount,
			WINHTTP_NO_HEADER_INDEX) == 0)
	{
		return EWoTPlatformHttpHopWindowsHeaderResult::Failure;
	}
	OutValue = Buffer.GetData();
	return EWoTPlatformHttpHopWindowsHeaderResult::Found;
}

class FWoTPlatformHttpHopWindowsExecutor final
	: public IWoTHttpHopExecutor
{
public:
	virtual void CancelCurrentRequest() override
	{
		FScopeLock Lock(&ActiveHandleLock);
		bAbortRequested.Store(true);
		if (ActiveRequest != nullptr)
		{
			ActiveRequest->Close();
		}
		if (ActiveConnection != nullptr)
		{
			ActiveConnection->Close();
		}
	}

	virtual FWoTBootstrapStatus ExecuteHop(
		const FWoTHttpHopRequest& Request,
		FWoTHttpHopResponse& OutResponse) override
	{
		OutResponse = FWoTHttpHopResponse();
		if (bAbortRequested.Load())
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("HTTP hop was cancelled before it began"));
		}
		if (!WoTPlatformHttpHopWindowsDeadlineRemains(
				Request.AbsoluteDeadlineSeconds))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("HTTP hop deadline expired before URL parsing"));
		}

		FString EffectiveUrl = Request.Url;
		bool bTestEndpointOverride = false;
#if WOT_BOOTSTRAP_TEST_ENDPOINTS
		if (Request.Url.Equals(WoT::Bootstrap::Trust::AnnouncementUrl,
				ESearchCase::CaseSensitive)
			|| Request.Url.Equals(
				FString(WoT::Bootstrap::Trust::AnnouncementUrl) + TEXT(".sig"),
				ESearchCase::CaseSensitive))
		{
			const FString Override = FPlatformMisc::GetEnvironmentVariable(
				TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"));
			if (!Override.IsEmpty())
			{
				EffectiveUrl = Override
					+ (Request.Url.EndsWith(TEXT(".sig"),
						ESearchCase::CaseSensitive) ? TEXT(".sig") : TEXT(""));
				bTestEndpointOverride = true;
			}
		}
		else if (WoTPlatformHttpHopWindowsRewriteReleaseUrl(
			Request.Url, EffectiveUrl))
		{
			bTestEndpointOverride = true;
		}
#endif

		URL_COMPONENTS Components = {};
		Components.dwStructSize = sizeof(Components);
		Components.dwSchemeLength = static_cast<DWORD>(-1);
		Components.dwHostNameLength = static_cast<DWORD>(-1);
		Components.dwUserNameLength = static_cast<DWORD>(-1);
		Components.dwPasswordLength = static_cast<DWORD>(-1);
		Components.dwUrlPathLength = static_cast<DWORD>(-1);
		Components.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (::WinHttpCrackUrl(*EffectiveUrl,
				static_cast<DWORD>(EffectiveUrl.Len()), 0, &Components) == 0
			|| Components.dwHostNameLength == 0
			|| Components.dwUserNameLength != 0
			|| Components.dwPasswordLength != 0)
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("HTTP hop URL is invalid or contains userinfo"));
		}

		const bool bSecure = Components.nScheme == INTERNET_SCHEME_HTTPS;
		const FString Host = FString::ConstructFromPtrSize(
			Components.lpszHostName,
			static_cast<int32>(Components.dwHostNameLength));
		if (bTestEndpointOverride
			&& (Components.nScheme != INTERNET_SCHEME_HTTP
				|| Host != TEXT("127.0.0.1")))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse,
				TEXT("the Test installer override must use loopback HTTP"));
		}
		if (!bSecure)
		{
#if WOT_BOOTSTRAP_TEST_ENDPOINTS
			if (Components.nScheme != INTERNET_SCHEME_HTTP
				|| (!Request.bAllowPlainHttpForLoopbackTesting
					&& !bTestEndpointOverride)
				|| Host != TEXT("127.0.0.1"))
			{
				return WoTPlatformHttpHopWindowsFailure(
					OutResponse, TEXT("plain HTTP is restricted to guarded loopback tests"));
			}
#else
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("HTTP hop URL must use HTTPS"));
#endif
		}

		FString ObjectName;
		if (Components.dwUrlPathLength != 0)
		{
			ObjectName.AppendChars(Components.lpszUrlPath,
				static_cast<int32>(Components.dwUrlPathLength));
		}
		if (Components.dwExtraInfoLength != 0)
		{
			ObjectName.AppendChars(Components.lpszExtraInfo,
				static_cast<int32>(Components.dwExtraInfoLength));
		}
		if (ObjectName.IsEmpty())
		{
			ObjectName = TEXT("/");
		}
		INTERNET_PORT Port = Components.nPort;
		if (Port == 0)
		{
			Port = bSecure
				? INTERNET_DEFAULT_HTTPS_PORT
				: INTERNET_DEFAULT_HTTP_PORT;
		}

		FWoTPlatformHttpHopWindowsHandle Session(::WinHttpOpen(
			L"WoT-Updater/1.0",
			WINHTTP_ACCESS_TYPE_NO_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0));
		if (Session.Get() == nullptr
			|| !WoTPlatformHttpHopWindowsSetRemainingTimeouts(
				Session.Get(), Request.AbsoluteDeadlineSeconds))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("could not open the WinHttp session"));
		}
		// Keep the synchronous connect phase to one native attempt. Otherwise
		// WinHttp can apply the full remaining connect timeout repeatedly.
		DWORD ConnectAttempts = 1;
		bool bConnectRetriesBounded = ::WinHttpSetOption(Session.Get(),
			WINHTTP_OPTION_CONNECT_RETRIES,
			&ConnectAttempts, sizeof(ConnectAttempts)) != 0;
		DWORD ConnectRetriesError = bConnectRetriesBounded ? 0 : ::GetLastError();
#if WITH_AUTOMATION_TESTS
		if (GWoTPlatformHttpHopFailConnectRetriesForTesting.Load())
		{
			bConnectRetriesBounded = false;
			ConnectRetriesError = ERROR_WINHTTP_INVALID_OPTION;
		}
#endif
		if (!bConnectRetriesBounded)
		{
			// Best effort: Wine's WinHttp (Proton) does not implement this option.
			// The option exists because native WinHttp can repeat the connect
			// timeout once per retry; Wine's WinHttp makes a single connect
			// attempt, so under Proton the remaining-timeouts call below (still
			// mandatory) keeps the hop inside its deadline.
			if (!GWoTPlatformHttpHopConnectRetriesReported.Exchange(true))
			{
				UE_LOG(LogWoTPlatformHttpHop, Warning,
					TEXT("WinHttp refused WINHTTP_OPTION_CONNECT_RETRIES (error %u); "
						"the per-phase deadline still bounds every connection attempt"),
					static_cast<uint32>(ConnectRetriesError));
			}
		}

		if (!WoTPlatformHttpHopWindowsSetRemainingTimeouts(
				Session.Get(), Request.AbsoluteDeadlineSeconds))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("HTTP hop deadline expired before connect"));
		}
		FWoTPlatformHttpHopWindowsHandle Connection(
			::WinHttpConnect(Session.Get(), *Host, Port, 0));
		{
			FScopeLock Lock(&ActiveHandleLock);
			ActiveConnection = &Connection;
			if (bAbortRequested.Load())
			{
				Connection.Close();
			}
		}
		ON_SCOPE_EXIT
		{
			FScopeLock Lock(&ActiveHandleLock);
			if (ActiveConnection == &Connection)
			{
				ActiveConnection = nullptr;
			}
		};
		if (Connection.Get() == nullptr
			|| !WoTPlatformHttpHopWindowsDeadlineRemains(
				Request.AbsoluteDeadlineSeconds))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("WinHttp connect failed or exceeded the deadline"));
		}

		DWORD OpenFlags = WINHTTP_FLAG_ESCAPE_DISABLE;
		if (bSecure)
		{
			OpenFlags |= WINHTTP_FLAG_SECURE;
		}
		FWoTPlatformHttpHopWindowsHandle RequestHandle(
			::WinHttpOpenRequest(Connection.Get(), L"GET", *ObjectName,
				nullptr, WINHTTP_NO_REFERER,
				WINHTTP_DEFAULT_ACCEPT_TYPES, OpenFlags));
		{
			FScopeLock Lock(&ActiveHandleLock);
			ActiveRequest = &RequestHandle;
			if (bAbortRequested.Load())
			{
				RequestHandle.Close();
			}
		}
		ON_SCOPE_EXIT
		{
			FScopeLock Lock(&ActiveHandleLock);
			if (ActiveRequest == &RequestHandle)
			{
				ActiveRequest = nullptr;
			}
		};
		if (RequestHandle.Get() == nullptr)
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("could not create the WinHttp request"));
		}

		DWORD RedirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		DWORD DisabledFeatures =
			WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
		DWORD AutologonPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
		if (::WinHttpSetOption(RequestHandle.Get(),
				WINHTTP_OPTION_REDIRECT_POLICY,
				&RedirectPolicy, sizeof(RedirectPolicy)) == 0
			|| ::WinHttpSetOption(RequestHandle.Get(),
				WINHTTP_OPTION_DISABLE_FEATURE,
				&DisabledFeatures, sizeof(DisabledFeatures)) == 0
			|| ::WinHttpSetOption(RequestHandle.Get(),
				WINHTTP_OPTION_AUTOLOGON_POLICY,
				&AutologonPolicy, sizeof(AutologonPolicy)) == 0)
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("could not apply WinHttp request isolation"));
		}



		if (!WoTPlatformHttpHopWindowsSetRemainingTimeouts(
				RequestHandle.Get(), Request.AbsoluteDeadlineSeconds)
			|| ::WinHttpSendRequest(RequestHandle.Get(),
				WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == 0
			|| !WoTPlatformHttpHopWindowsDeadlineRemains(
				Request.AbsoluteDeadlineSeconds))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("WinHttp send failed or exceeded the deadline"));
		}

		if (!WoTPlatformHttpHopWindowsSetRemainingTimeouts(
				RequestHandle.Get(), Request.AbsoluteDeadlineSeconds)
			|| ::WinHttpReceiveResponse(RequestHandle.Get(), nullptr) == 0
			|| !WoTPlatformHttpHopWindowsDeadlineRemains(
				Request.AbsoluteDeadlineSeconds))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("WinHttp response failed or exceeded the deadline"));
		}

		FWoTHttpHopResponse Response;
		DWORD StatusCode = 0;
		DWORD StatusSize = sizeof(StatusCode);
		if (::WinHttpQueryHeaders(RequestHandle.Get(),
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &StatusCode, &StatusSize,
				WINHTTP_NO_HEADER_INDEX) == 0)
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("could not read the HTTP status"));
		}
		Response.Status = static_cast<int32>(StatusCode);
		if (WoTPlatformHttpHopWindowsQueryHeader(
				RequestHandle.Get(), L"Location", Response.Location)
				== EWoTPlatformHttpHopWindowsHeaderResult::Failure)
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("could not read HTTP response headers"));
		}
		FString ContentLength;
		const EWoTPlatformHttpHopWindowsHeaderResult ContentLengthResult =
			WoTPlatformHttpHopWindowsQueryHeader(
				RequestHandle.Get(), L"Content-Length", ContentLength);
		if (ContentLengthResult
				== EWoTPlatformHttpHopWindowsHeaderResult::Failure
			|| (ContentLengthResult
					== EWoTPlatformHttpHopWindowsHeaderResult::Found
				&& (ContentLength.IsEmpty()
					|| !LexTryParseString(
						Response.BodyLength, *ContentLength))))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("HTTP Content-Length is invalid"));
		}
		const bool bHasContentLength = ContentLengthResult
			== EWoTPlatformHttpHopWindowsHeaderResult::Found;
		const uint64 DeclaredBodyLength = Response.BodyLength;
		const bool bStreamBody = Request.BodySink != nullptr
			&& Request.BodySink->ShouldStreamResponseBody(Response.Status);
		uint64 StreamedBodyLength = 0;
		auto StreamFailure = [this, &Request, &Response,
			&StreamedBodyLength, &OutResponse](const TCHAR* Message)
		{
			if (bAbortRequested.Load() && Request.BodySink != nullptr
				&& Request.BodySink->ShouldPreserveResponseOnError())
			{
				Response.StreamedBodyLength = StreamedBodyLength;
				OutResponse = MoveTemp(Response);
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::ServiceUnavailable, Message));
			}
			return WoTPlatformHttpHopWindowsFailure(OutResponse, Message);
		};
		TArray<uint8> StreamBuffer;
		if (bStreamBody)
		{
			FWoTBootstrapStatus Judged =
				Request.BodySink->OnResponseStart(Response);
			if (Judged.IsError())
			{
				if (Request.BodySink->ShouldPreserveResponseOnError())
				{
					OutResponse = MoveTemp(Response);
				}
				else
				{
					OutResponse = FWoTHttpHopResponse();
				}
				return FWoTBootstrapStatus::Error(
					MoveTemp(Judged).GetError());
			}
			StreamBuffer.SetNumUninitialized(64 * 1024);
		}

		for (;;)
		{
			if (!WoTPlatformHttpHopWindowsSetRemainingTimeouts(
					RequestHandle.Get(), Request.AbsoluteDeadlineSeconds))
			{
				return StreamFailure(
					TEXT("HTTP hop deadline expired before a body read"));
			}
			DWORD Available = 0;
			if (::WinHttpQueryDataAvailable(
					RequestHandle.Get(), &Available) == 0
				|| !WoTPlatformHttpHopWindowsDeadlineRemains(
					Request.AbsoluteDeadlineSeconds))
			{
				return StreamFailure(
					TEXT("WinHttp body availability read failed"));
			}
			if (Available == 0)
			{
				break;
			}
			if (!bStreamBody && Available > static_cast<DWORD>(
					MAX_int32 - Response.Body.Num()))
			{
				return WoTPlatformHttpHopWindowsFailure(
					OutResponse, TEXT("HTTP response body exceeds the container bound"));
			}

			const DWORD ReadCapacity = bStreamBody
				? FMath::Min<DWORD>(
					Available, static_cast<DWORD>(StreamBuffer.Num()))
				: Available;
			const int32 Offset = bStreamBody ? 0
				: Response.Body.AddUninitialized(
					static_cast<int32>(ReadCapacity));
			if (!WoTPlatformHttpHopWindowsSetRemainingTimeouts(
					RequestHandle.Get(), Request.AbsoluteDeadlineSeconds))
			{
				return StreamFailure(
					TEXT("HTTP hop deadline expired during a body read"));
			}
			DWORD Read = 0;
			if (::WinHttpReadData(RequestHandle.Get(),
					bStreamBody ? StreamBuffer.GetData()
						: Response.Body.GetData() + Offset,
					ReadCapacity, &Read) == 0
				|| Read == 0
				|| !WoTPlatformHttpHopWindowsDeadlineRemains(
					Request.AbsoluteDeadlineSeconds))
			{
				return StreamFailure(
					TEXT("WinHttp body read failed or was cut short"));
			}
			if (bStreamBody)
			{
				if (StreamedBodyLength > MAX_uint64 - static_cast<uint64>(Read))
				{
					return WoTPlatformHttpHopWindowsFailure(
						OutResponse, TEXT("HTTP streamed body length overflowed"));
				}
				FWoTBootstrapStatus Written = Request.BodySink->Write(
					MakeArrayView(StreamBuffer.GetData(), static_cast<int32>(Read)));
				if (Written.IsError())
				{
					if (Request.BodySink->ShouldPreserveResponseOnError())
					{
						Response.StreamedBodyLength = StreamedBodyLength;
						OutResponse = MoveTemp(Response);
					}
					else
					{
						OutResponse = FWoTHttpHopResponse();
					}
					return FWoTBootstrapStatus::Error(
						MoveTemp(Written).GetError());
				}
				StreamedBodyLength += static_cast<uint64>(Read);
			}
			else
			{
				Response.Body.SetNum(Offset + static_cast<int32>(Read),
					EAllowShrinking::No);
			}
		}

		// 204/304 are DEFINED to carry no body while their Content-Length may
		// describe the corresponding representation (RFC 9110 §8.6), so the
		// declared-vs-delivered completeness check must not apply to them.
		const bool bStatusHasNoBody =
			Response.Status == 204 || Response.Status == 304;
		if (bHasContentLength && !bStatusHasNoBody
			&& DeclaredBodyLength != (bStreamBody
				? StreamedBodyLength
				: static_cast<uint64>(Response.Body.Num())))
		{
			return WoTPlatformHttpHopWindowsFailure(
				OutResponse, TEXT("HTTP response ended before Content-Length"));
		}
		if (bStreamBody)
		{
			Response.StreamedBodyLength = StreamedBodyLength;
		}
		OutResponse = MoveTemp(Response);
		return FWoTBootstrapStatus::Success();
	}

private:
	FCriticalSection ActiveHandleLock;
	FWoTPlatformHttpHopWindowsHandle* ActiveConnection = nullptr;
	FWoTPlatformHttpHopWindowsHandle* ActiveRequest = nullptr;
	TAtomic<bool> bAbortRequested{false};
};
} // namespace

#if WITH_AUTOMATION_TESTS
void WoTPlatformHttpHopWindowsFailConnectRetriesForTesting(bool bFail)
{
	GWoTPlatformHttpHopFailConnectRetriesForTesting.Store(bFail);
}
#endif

#if WOT_BOOTSTRAP_TEST_ENDPOINTS
bool WoTPlatformHttpHopRewriteReleaseUrlForTesting(
	const FString& OriginalUrl, FString& OutUrl)
{
	return WoTPlatformHttpHopWindowsRewriteReleaseUrl(OriginalUrl, OutUrl);
}
#endif

TUniquePtr<IWoTHttpHopExecutor> WoTCreatePlatformHttpHopExecutor()
{
	return MakeUnique<FWoTPlatformHttpHopWindowsExecutor>();
}

#endif // PLATFORM_WINDOWS
