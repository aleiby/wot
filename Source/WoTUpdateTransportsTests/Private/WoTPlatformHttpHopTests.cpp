#ifndef WOT_BOOTSTRAP_TEST_ENDPOINTS
	#define WOT_BOOTSTRAP_TEST_ENDPOINTS 0
#endif

#if WITH_AUTOMATION_TESTS
#if WOT_BOOTSTRAP_TEST_ENDPOINTS

#include "Async/Async.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "WoTUpdateTransports/WoTPlatformHttpHop.h"
#include "WoTBootstrapCore/ArtifactTypes.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"

#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
constexpr EAutomationTestFlags WoTPlatformHttpHopSpecFlags =
	EAutomationTestFlags::ProgramContext
	| EAutomationTestFlags::ProductFilter;
constexpr TCHAR WoTPlatformHttpHopSpecAnnouncementOverrideEnv[] =
	TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL");
constexpr TCHAR WoTPlatformHttpHopSpecReleaseBaseOverrideEnv[] =
	TEXT("WOT_BOOTSTRAP_TEST_RELEASE_BASE_URL");

class FWoTPlatformHttpHopSpecEnvironment final
{
public:
	explicit FWoTPlatformHttpHopSpecEnvironment(const FString& Value,
		const TCHAR* InName)
		: Previous(FPlatformMisc::GetEnvironmentVariable(
			InName)), Name(InName)
	{
		FPlatformMisc::SetEnvironmentVar(Name, *Value);
	}

	~FWoTPlatformHttpHopSpecEnvironment()
	{
		FPlatformMisc::SetEnvironmentVar(Name, *Previous);
	}

	FWoTPlatformHttpHopSpecEnvironment(
		const FWoTPlatformHttpHopSpecEnvironment&) = delete;
	FWoTPlatformHttpHopSpecEnvironment& operator=(
		const FWoTPlatformHttpHopSpecEnvironment&) = delete;

private:
	FString Previous;
	const TCHAR* Name;
};

#if PLATFORM_WINDOWS
using WoTPlatformHttpHopSpecSocket = SOCKET;
using WoTPlatformHttpHopSpecSocklen = int;
constexpr WoTPlatformHttpHopSpecSocket WoTPlatformHttpHopSpecInvalidSocket =
	INVALID_SOCKET;

bool WoTPlatformHttpHopSpecStartSockets()
{
	WSADATA Data = {};
	return WSAStartup(MAKEWORD(2, 2), &Data) == 0;
}

void WoTPlatformHttpHopSpecStopSockets()
{
	WSACleanup();
}

void WoTPlatformHttpHopSpecCloseSocket(WoTPlatformHttpHopSpecSocket Socket)
{
	if (Socket != WoTPlatformHttpHopSpecInvalidSocket)
	{
		closesocket(Socket);
	}
}
#else
using WoTPlatformHttpHopSpecSocket = int;
using WoTPlatformHttpHopSpecSocklen = socklen_t;
constexpr WoTPlatformHttpHopSpecSocket WoTPlatformHttpHopSpecInvalidSocket = -1;

bool WoTPlatformHttpHopSpecStartSockets()
{
	return true;
}

void WoTPlatformHttpHopSpecStopSockets()
{
}

void WoTPlatformHttpHopSpecCloseSocket(WoTPlatformHttpHopSpecSocket Socket)
{
	if (Socket != WoTPlatformHttpHopSpecInvalidSocket)
	{
		close(Socket);
	}
}
#endif

class FWoTPlatformHttpHopSpecServer
{
public:
	explicit FWoTPlatformHttpHopSpecServer(
		std::string InResponse, bool bInStall = false,
		std::vector<size_t> InSendChunkSizes = {})
		: Response(MoveTemp(InResponse))
		, bStall(bInStall)
		, SendChunkSizes(MoveTemp(InSendChunkSizes))
	{
		if (!WoTPlatformHttpHopSpecStartSockets())
		{
			return;
		}
		bSocketsStarted = true;
		ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (ListenSocket == WoTPlatformHttpHopSpecInvalidSocket)
		{
			return;
		}

		sockaddr_in Address = {};
		Address.sin_family = AF_INET;
		Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		Address.sin_port = 0;
		if (bind(ListenSocket, reinterpret_cast<const sockaddr*>(&Address),
				sizeof(Address)) != 0
			|| listen(ListenSocket, 8) != 0)
		{
			WoTPlatformHttpHopSpecCloseSocket(ListenSocket);
			ListenSocket = WoTPlatformHttpHopSpecInvalidSocket;
			return;
		}

		WoTPlatformHttpHopSpecSocklen AddressSize = sizeof(Address);
		if (getsockname(ListenSocket, reinterpret_cast<sockaddr*>(&Address),
				&AddressSize) != 0)
		{
			WoTPlatformHttpHopSpecCloseSocket(ListenSocket);
			ListenSocket = WoTPlatformHttpHopSpecInvalidSocket;
			return;
		}
		Port = ntohs(Address.sin_port);
		Thread = std::thread([this]() { Run(); });
	}

	~FWoTPlatformHttpHopSpecServer()
	{
		bStop.store(true);
		WoTPlatformHttpHopSpecCloseSocket(ListenSocket);
		if (Thread.joinable())
		{
			Thread.join();
		}
		ListenSocket = WoTPlatformHttpHopSpecInvalidSocket;
		if (bSocketsStarted)
		{
			WoTPlatformHttpHopSpecStopSockets();
		}
	}

	bool IsReady() const
	{
		return ListenSocket != WoTPlatformHttpHopSpecInvalidSocket && Port != 0;
	}

	FString Url(const TCHAR* Path = TEXT("/object")) const
	{
		return FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, Path);
	}

	int32 ConnectionCount() const
	{
		return Connections.load();
	}

	FString RawRequest() const
	{
		std::lock_guard<std::mutex> Lock(RequestMutex);
		return FString(UTF8_TO_TCHAR(Request.c_str()));
	}

	void SetResponse(std::string InResponse)
	{
		std::lock_guard<std::mutex> Lock(ResponseMutex);
		Response = MoveTemp(InResponse);
	}

private:
	void Run()
	{
		while (!bStop.load())
		{
			fd_set ReadSet;
			FD_ZERO(&ReadSet);
			FD_SET(ListenSocket, &ReadSet);
			timeval Timeout = {};
			Timeout.tv_usec = 100000;
#if PLATFORM_WINDOWS
			const int Ready = select(0, &ReadSet, nullptr, nullptr, &Timeout);
#else
			const int Ready = select(ListenSocket + 1, &ReadSet, nullptr, nullptr, &Timeout);
#endif
			if (Ready <= 0)
			{
				continue;
			}

			sockaddr_in Peer = {};
			WoTPlatformHttpHopSpecSocklen PeerSize = sizeof(Peer);
			const WoTPlatformHttpHopSpecSocket Client = accept(ListenSocket,
				reinterpret_cast<sockaddr*>(&Peer), &PeerSize);
			if (Client == WoTPlatformHttpHopSpecInvalidSocket)
			{
				continue;
			}
			Connections.fetch_add(1);

			std::string Incoming;
			char Buffer[1024];
			while (Incoming.find("\r\n\r\n") == std::string::npos
				&& Incoming.size() < 64 * 1024)
			{
#if PLATFORM_WINDOWS
				const int Count = recv(Client, Buffer, sizeof(Buffer), 0);
#else
				const int Count = static_cast<int>(
					recv(Client, Buffer, sizeof(Buffer), 0));
#endif
				if (Count <= 0)
				{
					break;
				}
				Incoming.append(Buffer, static_cast<size_t>(Count));
			}
			{
				std::lock_guard<std::mutex> Lock(RequestMutex);
				Request = MoveTemp(Incoming);
			}

			if (bStall)
			{
				while (!bStop.load())
				{
					FPlatformProcess::SleepNoStats(0.01f);
				}
			}
			else
			{
				std::string ScriptedResponse;
				{
					std::lock_guard<std::mutex> Lock(ResponseMutex);
					ScriptedResponse = Response;
				}
				size_t Sent = 0;
				size_t ChunkIndex = 0;
				while (Sent < ScriptedResponse.size())
				{
					const size_t Remaining = ScriptedResponse.size() - Sent;
					const size_t ChunkSize = SendChunkSizes.empty()
						? Remaining
						: std::min(Remaining, SendChunkSizes[
							ChunkIndex++ % SendChunkSizes.size()]);
#if PLATFORM_WINDOWS
					const int Count = send(Client,
						ScriptedResponse.data() + Sent,
						static_cast<int>(ChunkSize), 0);
#else
					const int Count = static_cast<int>(send(Client,
						ScriptedResponse.data() + Sent,
						ChunkSize, 0));
#endif
					if (Count <= 0)
					{
						break;
					}
					Sent += static_cast<size_t>(Count);
				}
			}
			WoTPlatformHttpHopSpecCloseSocket(Client);
		}
	}

	std::string Response;
	std::mutex ResponseMutex;
	const bool bStall = false;
	const std::vector<size_t> SendChunkSizes;
	bool bSocketsStarted = false;
	WoTPlatformHttpHopSpecSocket ListenSocket =
		WoTPlatformHttpHopSpecInvalidSocket;
	uint16 Port = 0;
	std::atomic<bool> bStop{false};
	std::atomic<int32> Connections{0};
	mutable std::mutex RequestMutex;
	std::string Request;
	std::thread Thread;
};

bool WoTPlatformHttpHopSpecWaitForConnections(
	const FWoTPlatformHttpHopSpecServer& Server, int32 Expected)
{
	const double Deadline = FPlatformTime::Seconds() + 1.0;
	while (Server.ConnectionCount() < Expected
		&& FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.01f);
	}
	return Server.ConnectionCount() >= Expected;
}

FWoTBootstrapStatus WoTPlatformHttpHopSpecExecute(
	FWoTPlatformHttpHopSpecServer& Server,
	FWoTHttpHopResponse& OutResponse,
	const FString& RangeHeader = FString(),
	double DeadlineFromNow = 5.0,
	const TCHAR* Path = TEXT("/object"),
	IWoTHttpBodySink* BodySink = nullptr)
{
	TUniquePtr<IWoTHttpHopExecutor> Executor =
		WoTCreatePlatformHttpHopExecutor();
	FWoTHttpHopRequest Request;
	Request.Url = Server.Url(Path);

	Request.AbsoluteDeadlineSeconds =
		FPlatformTime::Seconds() + DeadlineFromNow;
	Request.BodySink = BodySink;
	Request.bAllowPlainHttpForLoopbackTesting = true;
	return Executor->ExecuteHop(Request, OutResponse);
}

class FWoTPlatformHttpHopSpecCollectingSink final : public IWoTHttpBodySink
{
public:
	virtual bool ShouldStreamResponseBody(int32 Status) const override
	{
		return bStreamAllStatuses
			|| IWoTHttpBodySink::ShouldStreamResponseBody(Status);
	}

	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse& HeadersOnly) override
	{
		++ResponseStartCount;
		ResponseStart = HeadersOnly;
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus Write(
		TConstArrayView<uint8> Bytes) override
	{
		if (ResponseStartCount != 1)
		{
			++WritesBeforeResponseStart;
		}
		Data.Append(Bytes.GetData(), Bytes.Num());
		++WriteCount;
		return FWoTBootstrapStatus::Success();
	}

	FWoTHttpHopResponse ResponseStart;
	TArray<uint8> Data;
	int32 ResponseStartCount = 0;
	int32 WriteCount = 0;
	int32 WritesBeforeResponseStart = 0;
	bool bStreamAllStatuses = false;
};

class FWoTPlatformHttpHopSpecFailingSink final : public IWoTHttpBodySink
{
public:
	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse&) override
	{
		++ResponseStartCount;
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8>) override
	{
		++WriteCount;
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("spec sink refused the bytes"),
			TEXT("sink-spec")));
	}

	int32 ResponseStartCount = 0;
	int32 WriteCount = 0;
};

class FWoTPlatformHttpHopSpecRejectingStartSink final
	: public IWoTHttpBodySink
{
public:
	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse& HeadersOnly) override
	{
		++ResponseStartCount;
		ResponseStart = HeadersOnly;
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("spec rejected the response start"),
			TEXT("response-start-spec")));
	}

	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8>) override
	{
		++WriteCount;
		return FWoTBootstrapStatus::Success();
	}

	FWoTHttpHopResponse ResponseStart;
	int32 ResponseStartCount = 0;
	int32 WriteCount = 0;
};

class FWoTPlatformHttpHopSpecFailAfterPrefixSink final
	: public IWoTHttpBodySink
{
public:
	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse&) override
	{
		++ResponseStartCount;
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus Write(
		TConstArrayView<uint8> Bytes) override
	{
		++WriteCount;
		if (WriteCount == 1)
		{
			Prefix.Append(Bytes.GetData(), Bytes.Num());
			return FWoTBootstrapStatus::Success();
		}
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("spec sink failed after its prefix"),
			TEXT("fail-after-prefix-spec")));
	}

	TArray<uint8> Prefix;
	int32 ResponseStartCount = 0;
	int32 WriteCount = 0;
};

class FWoTPlatformHttpHopSpecCooperativeCancelSink final
	: public IWoTHttpBodySink
{
public:
	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse&) override
	{
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8> Bytes) override
	{
		const int32 ThisWrite = ++WriteCount;
		if (ThisWrite == 1)
		{
			Prefix.Append(Bytes.GetData(), Bytes.Num());
			return FWoTBootstrapStatus::Success();
		}
		bWaitingForCancel.store(true);
		while (!bCancelled.load())
		{
			FPlatformProcess::SleepNoStats(0.001f);
		}
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::ServiceUnavailable,
			TEXT("spec cooperative cancellation")));
	}

	virtual bool ShouldPreserveResponseOnError() const override
	{
		return bCancelled.load();
	}

	TArray<uint8> Prefix;
	std::atomic<int32> WriteCount{0};
	std::atomic<bool> bWaitingForCancel{false};
	std::atomic<bool> bCancelled{false};
};

class FWoTPlatformHttpHopSpecBlockingSink final : public IWoTHttpBodySink
{
public:
	explicit FWoTPlatformHttpHopSpecBlockingSink(
		double InAbsoluteDeadlineSeconds)
		: AbsoluteDeadlineSeconds(InAbsoluteDeadlineSeconds)
	{
	}

	virtual FWoTBootstrapStatus OnResponseStart(
		const FWoTHttpHopResponse&) override
	{
		ResponseStartCount.fetch_add(1);
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus Write(TConstArrayView<uint8>) override
	{
		WriteCount.fetch_add(1);
		if (bExecuteReturned.load())
		{
			WritesAfterReturn.fetch_add(1);
		}
		bWriteInProgress.store(true);
		while (FPlatformTime::Seconds() < AbsoluteDeadlineSeconds + 0.25)
		{
			FPlatformProcess::SleepNoStats(0.01f);
		}
		bFinishedPastDeadline.store(
			FPlatformTime::Seconds() >= AbsoluteDeadlineSeconds);
		bWriteInProgress.store(false);
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("blocking sink deadline error"),
			TEXT("blocking-sink-spec")));
	}

	void MarkExecuteReturned()
	{
		bExecuteReturned.store(true);
	}

	const double AbsoluteDeadlineSeconds;
	std::atomic<int32> ResponseStartCount{0};
	std::atomic<int32> WriteCount{0};
	std::atomic<int32> WritesAfterReturn{0};
	std::atomic<bool> bWriteInProgress{false};
	std::atomic<bool> bFinishedPastDeadline{false};
	std::atomic<bool> bExecuteReturned{false};
};

// HTTP field names are case-insensitive; native stacks may recase them. Find
// the value for Name in the raw request text without assuming field-name case,
// then let the caller compare the VALUE exactly.
bool WoTPlatformHttpHopSpecHeaderValue(
	const FString& RawRequest, const FString& Name, FString& OutValue)
{
	TArray<FString> Lines;
	RawRequest.ParseIntoArray(Lines, TEXT("\r\n"), true);
	for (const FString& Line : Lines)
	{
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), Colon))
		{
			continue;
		}
		if (Line.Left(Colon).Equals(Name, ESearchCase::IgnoreCase))
		{
			OutValue = Line.Mid(Colon + 1).TrimStartAndEnd();
			return true;
		}
	}
	return false;
}

bool WoTPlatformHttpHopSpecAssertTransportFailure(
	FAutomationTestBase& Test,
	const FWoTBootstrapStatus& Status,
	const FWoTHttpHopResponse& Response)
{
	bool bOkay = Test.TestTrue(TEXT("the transport status is an error"),
		Status.IsError());
	if (Status.IsError())
	{
		bOkay &= Test.TestEqual(TEXT("the error is ServiceUnavailable"),
			Status.GetError().Code, EWoTBootstrapError::ServiceUnavailable);
	}
	bOkay &= Test.TestEqual(TEXT("the HTTP status is zero"), Response.Status, 0);
	bOkay &= Test.TestTrue(TEXT("the failed response body is empty"),
		Response.Body.IsEmpty());
	bOkay &= Test.TestTrue(TEXT("the failed response Location is empty"),
		Response.Location.IsEmpty());
	bOkay &= Test.TestEqual(TEXT("the failed response declared length is zero"),
		Response.BodyLength, uint64(0));
	bOkay &= Test.TestEqual(TEXT("the failed response streamed length is zero"),
		Response.StreamedBodyLength, uint64(0));
	return bOkay;
}

// Reserves an ephemeral port that REFUSES connections for as long as the
// object lives: bound but never listening, so a connect gets RST while no
// other process can rebind it (closing the socket before the request would
// open a reuse race).
class FWoTPlatformHttpHopSpecRefusingPort
{
public:
	FWoTPlatformHttpHopSpecRefusingPort()
	{
		if (!WoTPlatformHttpHopSpecStartSockets())
		{
			return;
		}
		bSocketsStarted = true;
		Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (Socket == WoTPlatformHttpHopSpecInvalidSocket)
		{
			return;
		}
		sockaddr_in Address = {};
		Address.sin_family = AF_INET;
		Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		Address.sin_port = 0;
		if (bind(Socket, reinterpret_cast<const sockaddr*>(&Address),
				sizeof(Address)) == 0)
		{
			WoTPlatformHttpHopSpecSocklen AddressSize = sizeof(Address);
			if (getsockname(Socket, reinterpret_cast<sockaddr*>(&Address),
					&AddressSize) == 0)
			{
				Port = ntohs(Address.sin_port);
			}
		}
	}

	~FWoTPlatformHttpHopSpecRefusingPort()
	{
		WoTPlatformHttpHopSpecCloseSocket(Socket);
		if (bSocketsStarted)
		{
			WoTPlatformHttpHopSpecStopSockets();
		}
	}

	FWoTPlatformHttpHopSpecRefusingPort(
		const FWoTPlatformHttpHopSpecRefusingPort&) = delete;
	FWoTPlatformHttpHopSpecRefusingPort& operator=(
		const FWoTPlatformHttpHopSpecRefusingPort&) = delete;

	uint16 Get() const
	{
		return Port;
	}

private:
	WoTPlatformHttpHopSpecSocket Socket = WoTPlatformHttpHopSpecInvalidSocket;
	bool bSocketsStarted = false;
	uint16 Port = 0;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopRoundTripSpec,
	"WoT.Update.PlatformHop.RoundTrip",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopRoundTripSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\n"
		"ETag: \"opaque-etag\"\r\n"
		"Last-Modified: Tue, 28 Jul 2026 12:34:56 GMT\r\n"
		"Content-Length: 7\r\n"
		"Content-Range: bytes 42-48/100\r\n"
		"Connection: close\r\n"
		"\r\n"
		"payload");
	if (!TestTrue(TEXT("the loopback server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status =
		WoTPlatformHttpHopSpecExecute(Server, Response);
	TestFalse(TEXT("the request succeeds"), Status.IsError());
	TestEqual(TEXT("the status surfaces"), Response.Status, 200);
	TestEqual(TEXT("Location is absent"), Response.Location, FString());
	TestEqual(TEXT("declared Content-Length surfaces"),
		Response.BodyLength, uint64(7));
	const TArray<uint8> Expected = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
	TestTrue(TEXT("the delivered body surfaces"), Response.Body == Expected);
	return !HasAnyErrors();
}


#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopConnectRetriesUnsupportedSpec,
	"WoT.Update.PlatformHop.ConnectRetriesOptionUnsupported",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopConnectRetriesUnsupportedSpec::RunTest(const FString&)
{
	// Wine's WinHttp refuses WINHTTP_OPTION_CONNECT_RETRIES; the hop must still
	// complete inside its deadline instead of failing the update check.
	WoTPlatformHttpHopWindowsFailConnectRetriesForTesting(true);
	ON_SCOPE_EXIT { WoTPlatformHttpHopWindowsFailConnectRetriesForTesting(false); };
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: 7\r\n"
		"Connection: close\r\n"
		"\r\n"
		"payload");
	if (!TestTrue(TEXT("the loopback server starts"), Server.IsReady()))
	{
		return false;
	}
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(Server, Response);
	if (!TestFalse(TEXT("the hop succeeds without the connect-retries bound"),
		Status.IsError()))
	{
		AddError(Status.GetError().ToString());
	}
	TestEqual(TEXT("the status surfaces"), Response.Status, 200);
	const TArray<uint8> Expected = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
	TestTrue(TEXT("the body surfaces"), Response.Body == Expected);
	return !HasAnyErrors();
}
#endif


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopRedirectSpec,
	"WoT.Update.PlatformHop.RedirectNotFollowed",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopRedirectSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server("");
	if (!TestTrue(TEXT("the redirect loopback server starts"), Server.IsReady()))
	{
		return false;
	}
	const FString Location = Server.Url(TEXT("/followed"));
	const FTCHARToUTF8 LocationUtf8(*Location);
	const std::string Reply =
		"HTTP/1.1 302 Found\r\nLocation: "
		+ std::string(LocationUtf8.Get(), LocationUtf8.Length())
		+ "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	Server.SetResponse(Reply);

	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status =
		WoTPlatformHttpHopSpecExecute(Server, Response);
	TestFalse(TEXT("the 302 is a successful HTTP exchange"), Status.IsError());
	TestEqual(TEXT("the 302 surfaces to the caller"), Response.Status, 302);
	TestEqual(TEXT("Location surfaces verbatim"), Response.Location, Location);
	TestTrue(TEXT("the redirect request reaches the server"),
		WoTPlatformHttpHopSpecWaitForConnections(Server, 1));
	TestEqual(TEXT("the redirect is never auto-followed"),
		Server.ConnectionCount(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopDeadlineSpec,
	"WoT.Update.PlatformHop.Deadline",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopDeadlineSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server("", true);
	if (!TestTrue(TEXT("the stalling loopback server starts"), Server.IsReady()))
	{
		return false;
	}

	const double Started = FPlatformTime::Seconds();
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status =
		WoTPlatformHttpHopSpecExecute(Server, Response, FString(), 2.0);
	const double Elapsed = FPlatformTime::Seconds() - Started;
	WoTPlatformHttpHopSpecAssertTransportFailure(*this, Status, Response);
	TestTrue(TEXT("the stalling connection was actually accepted"),
		WoTPlatformHttpHopSpecWaitForConnections(Server, 1));
	TestTrue(TEXT("the absolute deadline prevents a hang"), Elapsed < 8.0);
	// A guard against second/millisecond unit regressions: the executor must
	// actually WAIT toward the 2s deadline, not fail after 2ms.
	TestTrue(TEXT("the deadline is honored in SECONDS"), Elapsed >= 1.0);
	return !HasAnyErrors();
}

#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopMacCancelStallSpec,
	"WoT.Update.PlatformHop.MacCancelNeverCompletes",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopMacCancelStallSpec::RunTest(const FString&)
{
	// B10: if NSURLSession never delivered didCompleteWithError: after the
	// deadline cancel, the executor must still return (bounded by its grace)
	// instead of waiting forever.
	FWoTPlatformHttpHopSpecServer Server("", true);
	if (!TestTrue(TEXT("the stalling loopback server starts"), Server.IsReady()))
	{
		return false;
	}
	WoTPlatformHttpHopMacSetCancelStallForTesting(true, 0.3);
	ON_SCOPE_EXIT { WoTPlatformHttpHopMacSetCancelStallForTesting(false, 10.0); };
	FWoTPlatformHttpHopSpecCollectingSink Sink;
	const double Started = FPlatformTime::Seconds();
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 1.0, TEXT("/object"), &Sink);
	const double Elapsed = FPlatformTime::Seconds() - Started;
	TestTrue(TEXT("a never-completing cancel still fails the hop"), Status.IsError());
	if (Status.IsError())
	{
		TestTrue(TEXT("the failure names the missing completion"),
			Status.GetError().ToString().Contains(TEXT("did not complete")));
	}
	TestTrue(TEXT("the executor returns within deadline + grace"), Elapsed < 4.0);
	TestTrue(TEXT("the deadline is still honored first"), Elapsed >= 0.9);

	WoTPlatformHttpHopMacSetCancelStallForTesting(false, 10.0);
	FPlatformProcess::SleepNoStats(0.5f);
	FWoTPlatformHttpHopSpecServer Healthy(
		"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
	if (TestTrue(TEXT("a healthy server starts"), Healthy.IsReady()))
	{
		FWoTHttpHopResponse After;
		TestFalse(TEXT("the next hop after a detached stall still succeeds"),
			WoTPlatformHttpHopSpecExecute(Healthy, After).IsError());
	}
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopConnectionRefusedSpec,
	"WoT.Update.PlatformHop.ConnectionRefused",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopConnectionRefusedSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecRefusingPort Reserved;
	if (!TestNotEqual(TEXT("an ephemeral refusing port is reserved"),
			Reserved.Get(), uint16(0)))
	{
		return false;
	}

	TUniquePtr<IWoTHttpHopExecutor> Executor =
		WoTCreatePlatformHttpHopExecutor();
	FWoTHttpHopRequest Request;
	Request.Url = FString::Printf(
		TEXT("http://127.0.0.1:%u/refused"), Reserved.Get());
	Request.AbsoluteDeadlineSeconds = FPlatformTime::Seconds() + 3.0;
	Request.bAllowPlainHttpForLoopbackTesting = true;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = Executor->ExecuteHop(Request, Response);
	WoTPlatformHttpHopSpecAssertTransportFailure(*this, Status, Response);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopCredentialsSpec,
	"WoT.Update.PlatformHop.NoCredentials",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopCredentialsSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	if (!TestTrue(TEXT("the credential loopback server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status =
		WoTPlatformHttpHopSpecExecute(Server, Response);
	TestFalse(TEXT("the credential-observation request succeeds"),
		Status.IsError());
	const FString Raw = Server.RawRequest();
	TestFalse(TEXT("Authorization is absent"),
		Raw.Contains(TEXT("\r\nAuthorization:"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Proxy-Authorization is absent"),
		Raw.Contains(TEXT("\r\nProxy-Authorization:"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Cookie is absent"),
		Raw.Contains(TEXT("\r\nCookie:"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Cookie2 is absent"),
		Raw.Contains(TEXT("\r\nCookie2:"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("API-key headers are absent"),
		Raw.Contains(TEXT("\r\nX-Api-Key:"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("auth-token headers are absent"),
		Raw.Contains(TEXT("\r\nX-Auth-Token:"), ESearchCase::IgnoreCase));
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopStatus304Spec,
	"WoT.Update.PlatformHop.Status304KeepsDeclaredLength",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopStatus304Spec::RunTest(const FString&)
{
	// A 304 carries no body by definition, but its Content-Length may describe
	// the corresponding 200 representation (RFC 9110 s8.6). The executor must
	// surface Status=304 with the DECLARED length and an empty body - not fail
	// the exchange because delivered != declared.
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 304 Not Modified\r\n"
		"ETag: \"opaque-etag\"\r\n"
		"Content-Length: 8\r\n"
		"Connection: close\r\n"
		"\r\n");
	if (!TestTrue(TEXT("the 304 loopback server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status =
		WoTPlatformHttpHopSpecExecute(Server, Response);
	TestFalse(TEXT("the 304 is a successful HTTP exchange"), Status.IsError());
	TestEqual(TEXT("the 304 surfaces"), Response.Status, 304);
	TestEqual(TEXT("the declared representation length surfaces"),
		Response.BodyLength, uint64(8));
	TestTrue(TEXT("the delivered body is empty"), Response.Body.IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopStreamed200Spec,
	"WoT.Update.PlatformHop.Streamed200",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopStreamed200Spec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\npayload");
	if (!TestTrue(TEXT("the streamed-200 server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTPlatformHttpHopSpecCollectingSink Sink;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 5.0, TEXT("/object"), &Sink);
	TestFalse(TEXT("the streamed 200 succeeds"), Status.IsError());
	TestEqual(TEXT("the streamed status surfaces"), Response.Status, 200);
	TestEqual(TEXT("the declared Content-Length keeps its meaning"),
		Response.BodyLength, uint64(7));
	TestEqual(TEXT("the delivered streamed byte count surfaces separately"),
		Response.StreamedBodyLength, uint64(7));
	TestTrue(TEXT("the response body stays empty"), Response.Body.IsEmpty());
	const TArray<uint8> Expected = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
	TestTrue(TEXT("the sink receives the body byte-exact"), Sink.Data == Expected);
	TestEqual(TEXT("response start is invoked exactly once"),
		Sink.ResponseStartCount, 1);
	TestEqual(TEXT("response start carries the status"),
		Sink.ResponseStart.Status, 200);
	TestEqual(TEXT("response start carries the declared length"),
		Sink.ResponseStart.BodyLength, uint64(7));
	TestEqual(TEXT("response start has no delivered bytes"),
		Sink.ResponseStart.StreamedBodyLength, uint64(0));
	TestTrue(TEXT("response start has no buffered body"),
		Sink.ResponseStart.Body.IsEmpty());
	TestTrue(TEXT("the sink is invoked"), Sink.WriteCount > 0);
	TestEqual(TEXT("no Write precedes response start"),
		Sink.WritesBeforeResponseStart, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopStreamedErrorOptInSpec,
	"WoT.Update.PlatformHop.StreamedErrorOptIn",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopStreamedErrorOptInSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 404 Not Found\r\nContent-Length: 7\r\n"
		"Connection: close\r\n\r\npayload");
	if (!TestTrue(TEXT("the streamed-error server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTPlatformHttpHopSpecCollectingSink Sink;
	Sink.bStreamAllStatuses = true;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 5.0, TEXT("/missing"), &Sink);
	TestFalse(TEXT("the opted-in error response completes"), Status.IsError());
	TestEqual(TEXT("the error status surfaces"), Response.Status, 404);
	TestTrue(TEXT("the error body is not buffered"), Response.Body.IsEmpty());
	TestEqual(TEXT("the error body is streamed exactly once"),
		Sink.ResponseStartCount, 1);
	const TArray<uint8> Expected = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
	TestTrue(TEXT("the sink receives the error body byte-exact"),
		Sink.Data == Expected);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopChunkedStreamSpec,
	"WoT.Update.PlatformHop.ChunkedStreamWithoutContentLength",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopChunkedStreamSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
		"Connection: close\r\n\r\n"
		"3\r\npay\r\n4\r\nload\r\n0\r\n\r\n");
	if (!TestTrue(TEXT("the chunked-stream server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTPlatformHttpHopSpecCollectingSink Sink;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 5.0, TEXT("/chunked"), &Sink);
	TestFalse(TEXT("the chunked stream succeeds"), Status.IsError());
	TestEqual(TEXT("the chunked status surfaces"), Response.Status, 200);
	TestEqual(TEXT("absent Content-Length remains unknown"),
		Response.BodyLength, uint64(0));
	TestEqual(TEXT("chunked delivered bytes surface separately"),
		Response.StreamedBodyLength, uint64(7));
	TestEqual(TEXT("response start sees unknown declared length"),
		Sink.ResponseStart.BodyLength, uint64(0));
	TestEqual(TEXT("chunked response start occurs exactly once"),
		Sink.ResponseStartCount, 1);
	TestEqual(TEXT("no chunked Write precedes response start"),
		Sink.WritesBeforeResponseStart, 0);
	TestTrue(TEXT("the chunked response body stays empty"),
		Response.Body.IsEmpty());
	const TArray<uint8> Expected = {'p', 'a', 'y', 'l', 'o', 'a', 'd'};
	TestTrue(TEXT("decoded chunked bytes arrive byte-exact"),
		Sink.Data == Expected);
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopSinkErrorSpec,
	"WoT.Update.PlatformHop.SinkErrorAborts",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopSinkErrorSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\npayload");
	if (!TestTrue(TEXT("the sink-error server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTPlatformHttpHopSpecFailingSink Sink;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 5.0, TEXT("/object"), &Sink);
	TestTrue(TEXT("the sink error aborts the hop"), Status.IsError());
	if (Status.IsError())
	{
		TestEqual(TEXT("the sink error code is preserved"),
			Status.GetError().Code, EWoTBootstrapError::Io);
		TestEqual(TEXT("the sink error message is preserved"),
			Status.GetError().Message, FString(TEXT("spec sink refused the bytes")));
		TestEqual(TEXT("the sink error path is preserved"),
			Status.GetError().Path, FString(TEXT("sink-spec")));
	}
	TestEqual(TEXT("response start precedes the failing Write"),
		Sink.ResponseStartCount, 1);
	TestEqual(TEXT("the failing sink is called once"), Sink.WriteCount, 1);
	TestEqual(TEXT("no partial HTTP response is published"), Response.Status, 0);
	TestTrue(TEXT("no partial body is published"), Response.Body.IsEmpty());
	TestEqual(TEXT("no partial length is published"), Response.BodyLength, uint64(0));
	TestEqual(TEXT("no partial streamed length is published"),
		Response.StreamedBodyLength, uint64(0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopFailAfterPrefixSpec,
	"WoT.Update.PlatformHop.SinkFailsAfterPrefix",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopFailAfterPrefixSpec::RunTest(const FString&)
{
	constexpr size_t BodySize = 128 * 1024 + 17;
	std::string Body(BodySize, 'x');
	std::string Reply = "HTTP/1.1 200 OK\r\nContent-Length: "
		+ std::to_string(Body.size())
		+ "\r\nConnection: close\r\n\r\n" + Body;
	FWoTPlatformHttpHopSpecServer Server(MoveTemp(Reply));
	if (!TestTrue(TEXT("the fail-after-prefix server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTPlatformHttpHopSpecFailAfterPrefixSink Sink;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 5.0, TEXT("/prefix"), &Sink);
	TestTrue(TEXT("the mid-stream sink error aborts the hop"), Status.IsError());
	if (Status.IsError())
	{
		TestEqual(TEXT("the mid-stream sink code is preserved"),
			Status.GetError().Code, EWoTBootstrapError::Io);
		TestEqual(TEXT("the mid-stream sink message is preserved"),
			Status.GetError().Message,
			FString(TEXT("spec sink failed after its prefix")));
		TestEqual(TEXT("the mid-stream sink path is preserved"),
			Status.GetError().Path,
			FString(TEXT("fail-after-prefix-spec")));
	}
	TestEqual(TEXT("response start occurs once before the prefix"),
		Sink.ResponseStartCount, 1);
	TestEqual(TEXT("one Write succeeds and the next fails"), Sink.WriteCount, 2);
	TestTrue(TEXT("a non-empty prefix was accepted"), !Sink.Prefix.IsEmpty());
	TestTrue(TEXT("the accepted prefix is shorter than the body"),
		Sink.Prefix.Num() < static_cast<int32>(BodySize));
	TestEqual(TEXT("no failed partial response is published"), Response.Status, 0);
	TestEqual(TEXT("no failed partial streamed length is published"),
		Response.StreamedBodyLength, uint64(0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopNativeCancelSpec,
	"WoT.Update.PlatformHop.NativeCancelUnblocksHeaders",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopNativeCancelSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(std::string(), true);
	if (!TestTrue(TEXT("the stalled-header server starts"), Server.IsReady()))
	{
		return false;
	}
	TUniquePtr<IWoTHttpHopExecutor> Executor =
		WoTCreatePlatformHttpHopExecutor();
	FWoTHttpHopRequest Request;
	Request.Url = Server.Url(TEXT("/stalled-headers"));
	Request.AbsoluteDeadlineSeconds = FPlatformTime::Seconds() + 3.0;
	Request.bAllowPlainHttpForLoopbackTesting = true;
	FWoTHttpHopResponse Response;
	TFuture<FWoTBootstrapStatus> Executing = Async(EAsyncExecution::Thread,
		[&Executor, &Request, &Response]()
		{
			return Executor->ExecuteHop(Request, Response);
		});
	if (!TestTrue(TEXT("the native request reaches the stalled server"),
			WoTPlatformHttpHopSpecWaitForConnections(Server, 1)))
	{
		Executor->CancelCurrentRequest();
		Executing.Wait();
		return false;
	}
	const double CancelStarted = FPlatformTime::Seconds();
	Executor->CancelCurrentRequest();
	const bool bPrompt = Executing.WaitFor(FTimespan::FromSeconds(1.0));
	Executing.Wait();
	const FWoTBootstrapStatus& Status = Executing.Get();
	TestTrue(TEXT("native cancellation unblocks the header wait promptly"),
		bPrompt && FPlatformTime::Seconds() - CancelStarted < 1.0);
	TestTrue(TEXT("native cancellation returns a transport error"),
		Status.IsError());
	TestEqual(TEXT("header cancellation publishes no terminal response"),
		Response.Status, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopCooperativeCancelSpec,
	"WoT.Update.PlatformHop.CooperativeCancelPreservesHeaders",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopCooperativeCancelSpec::RunTest(const FString&)
{
	constexpr size_t BodySize = 256 * 1024 + 17;
	std::string Body(BodySize, 'c');
	std::string Reply = "HTTP/1.1 200 OK\r\nContent-Length: "
		+ std::to_string(Body.size())
		+ "\r\nETag: \"cancel-etag\"\r\nConnection: close\r\n\r\n" + Body;
	FWoTPlatformHttpHopSpecServer Server(MoveTemp(Reply), false,
		{4096, 32768, 8192});
	if (!TestTrue(TEXT("the cooperative-cancel server starts"), Server.IsReady()))
	{
		return false;
	}
	TUniquePtr<IWoTHttpHopExecutor> Executor =
		WoTCreatePlatformHttpHopExecutor();
	FWoTPlatformHttpHopSpecCooperativeCancelSink Sink;
	FWoTHttpHopRequest Request;
	Request.Url = Server.Url(TEXT("/cancel-body"));
	Request.AbsoluteDeadlineSeconds = FPlatformTime::Seconds() + 5.0;
	Request.BodySink = &Sink;
	Request.bAllowPlainHttpForLoopbackTesting = true;
	FWoTHttpHopResponse Response;
	TFuture<FWoTBootstrapStatus> Executing = Async(EAsyncExecution::Thread,
		[&Executor, &Request, &Response]()
		{
			return Executor->ExecuteHop(Request, Response);
		});
	const double WaitDeadline = FPlatformTime::Seconds() + 2.0;
	while (!Sink.bWaitingForCancel.load()
		&& FPlatformTime::Seconds() < WaitDeadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	TestTrue(TEXT("the sink accepts a prefix before cancellation"),
		Sink.bWaitingForCancel.load() && !Sink.Prefix.IsEmpty());
	Sink.bCancelled.store(true);
	Executor->CancelCurrentRequest();
	Executing.Wait();
	const FWoTBootstrapStatus& Status = Executing.Get();
	TestTrue(TEXT("cooperative body cancellation returns an error"),
		Status.IsError());
	TestEqual(TEXT("cooperative cancellation preserves terminal status"),
		Response.Status, 200);
	TestEqual(TEXT("cooperative cancellation preserves declared length"),
		Response.BodyLength, static_cast<uint64>(BodySize));
	TestEqual(TEXT("cooperative cancellation preserves accepted prefix length"),
		Response.StreamedBodyLength,
		static_cast<uint64>(Sink.Prefix.Num()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopBlockingSinkDeadlineSpec,
	"WoT.Update.PlatformHop.BlockingSinkDeadlineQuiesces",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopBlockingSinkDeadlineSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
		"Connection: close\r\n\r\npayload");
	if (!TestTrue(TEXT("the blocking-sink server starts"), Server.IsReady()))
	{
		return false;
	}

	const double Started = FPlatformTime::Seconds();
	const double SinkDeadline = Started + 1.0;
	FWoTPlatformHttpHopSpecBlockingSink Sink(SinkDeadline);
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 1.0, TEXT("/blocking"), &Sink);
	Sink.MarkExecuteReturned();
	const double Elapsed = FPlatformTime::Seconds() - Started;
	TestTrue(TEXT("the blocking sink error aborts the hop"), Status.IsError());
	if (Status.IsError())
	{
		TestEqual(TEXT("the blocking sink error beats the deadline error"),
			Status.GetError().Code, EWoTBootstrapError::Io);
		TestEqual(TEXT("the blocking sink message is preserved"),
			Status.GetError().Message,
			FString(TEXT("blocking sink deadline error")));
		TestEqual(TEXT("the blocking sink path is preserved"),
			Status.GetError().Path, FString(TEXT("blocking-sink-spec")));
	}
	TestTrue(TEXT("the sink Write actually crossed the deadline"),
		Sink.bFinishedPastDeadline.load());
	TestTrue(TEXT("ExecuteHop waited beyond its deadline for sink quiescence"),
		Elapsed >= 1.0);
	TestFalse(TEXT("no sink Write remains active after ExecuteHop returns"),
		Sink.bWriteInProgress.load());
	FPlatformProcess::SleepNoStats(0.10f);
	TestEqual(TEXT("no sink Write begins after ExecuteHop returns"),
		Sink.WritesAfterReturn.load(), 0);
	TestEqual(TEXT("response start occurs once for the blocking sink"),
		Sink.ResponseStartCount.load(), 1);
	TestEqual(TEXT("the blocking sink receives one Write"),
		Sink.WriteCount.load(), 1);
	TestEqual(TEXT("no blocking partial response is published"),
		Response.Status, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopSinkNonSuccessSpec,
	"WoT.Update.PlatformHop.SinkNonSuccessStillBuffers",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopSinkNonSuccessSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer RedirectServer(
		"HTTP/1.1 302 Found\r\nLocation: https://example.invalid/next\r\n"
		"Content-Length: 8\r\nConnection: close\r\n\r\nredirect");
	FWoTPlatformHttpHopSpecServer MissingServer(
		"HTTP/1.1 404 Not Found\r\nContent-Length: 7\r\n"
		"Connection: close\r\n\r\nmissing");
	if (!TestTrue(TEXT("the buffered-redirect server starts"),
			RedirectServer.IsReady())
		|| !TestTrue(TEXT("the buffered-404 server starts"), MissingServer.IsReady()))
	{
		return false;
	}

	FWoTPlatformHttpHopSpecCollectingSink Sink;
	FWoTHttpHopResponse RedirectResponse;
	FWoTBootstrapStatus RedirectStatus = WoTPlatformHttpHopSpecExecute(
		RedirectServer, RedirectResponse, FString(), 5.0, TEXT("/object"), &Sink);
	TestFalse(TEXT("the redirect exchange succeeds"), RedirectStatus.IsError());
	TestEqual(TEXT("the redirect status surfaces"), RedirectResponse.Status, 302);
	const TArray<uint8> RedirectBody =
		{'r', 'e', 'd', 'i', 'r', 'e', 'c', 't'};
	TestTrue(TEXT("the redirect body remains buffered"),
		RedirectResponse.Body == RedirectBody);

	FWoTHttpHopResponse MissingResponse;
	FWoTBootstrapStatus MissingStatus = WoTPlatformHttpHopSpecExecute(
		MissingServer, MissingResponse, FString(), 5.0, TEXT("/object"), &Sink);
	TestFalse(TEXT("the 404 exchange succeeds"), MissingStatus.IsError());
	TestEqual(TEXT("the 404 status surfaces"), MissingResponse.Status, 404);
	const TArray<uint8> MissingBody = {'m', 'i', 's', 's', 'i', 'n', 'g'};
	TestTrue(TEXT("the 404 body remains buffered"),
		MissingResponse.Body == MissingBody);
	TestTrue(TEXT("non-200/206 bodies never reach the sink"), Sink.Data.IsEmpty());
	TestEqual(TEXT("non-200/206 responses are never judged by the sink"),
		Sink.ResponseStartCount, 0);
	TestEqual(TEXT("non-200/206 responses have no streamed length"),
		RedirectResponse.StreamedBodyLength, uint64(0));
	TestEqual(TEXT("the 404 has no streamed length"),
		MissingResponse.StreamedBodyLength, uint64(0));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPlatformHttpHopLargeStreamSpec,
	"WoT.Update.PlatformHop.LargeMultiChunkStream",
	WoTPlatformHttpHopSpecFlags)

bool FWoTPlatformHttpHopLargeStreamSpec::RunTest(const FString&)
{
	constexpr size_t BodySize = 8 * 1024 * 1024;
	std::string Body;
	Body.resize(BodySize);
	for (size_t Index = 0; Index < Body.size(); ++Index)
	{
		Body[Index] = static_cast<char>((Index * 31 + Index / 251) & 0xff);
	}
	std::string Reply = "HTTP/1.1 200 OK\r\nContent-Length: "
		+ std::to_string(Body.size())
		+ "\r\nConnection: close\r\n\r\n";
	Reply += Body;
	FWoTPlatformHttpHopSpecServer Server(
		MoveTemp(Reply), false, {17, 4093, 65521, 257, 131071});
	if (!TestTrue(TEXT("the large-stream server starts"), Server.IsReady()))
	{
		return false;
	}

	FWoTPlatformHttpHopSpecCollectingSink Sink;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = WoTPlatformHttpHopSpecExecute(
		Server, Response, FString(), 20.0, TEXT("/large"), &Sink);
	TestFalse(TEXT("the large stream succeeds"), Status.IsError());
	TestEqual(TEXT("the large declared length is exact"),
		Response.BodyLength, static_cast<uint64>(BodySize));
	TestEqual(TEXT("the large streamed length is exact"),
		Response.StreamedBodyLength, static_cast<uint64>(BodySize));
	TestTrue(TEXT("the large response body stays empty"), Response.Body.IsEmpty());
	TestEqual(TEXT("the sink receives every large-body byte"),
		Sink.Data.Num(), static_cast<int32>(BodySize));
	if (Sink.Data.Num() == static_cast<int32>(BodySize))
	{
		TestTrue(TEXT("the large body is byte-exact"),
			FMemory::Memcmp(Sink.Data.GetData(), Body.data(), BodySize) == 0);
	}
	TestTrue(TEXT("the large body arrives through multiple sink calls"),
		Sink.WriteCount > 1);
	TestEqual(TEXT("large response start occurs exactly once"),
		Sink.ResponseStartCount, 1);
	TestEqual(TEXT("no large Write precedes response start"),
		Sink.WritesBeforeResponseStart, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTAnnouncementLoopbackOverrideSpec,
	"WoT.Update.PlatformHop.AnnouncementLoopbackOverride",
	WoTPlatformHttpHopSpecFlags)

bool FWoTAnnouncementLoopbackOverrideSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
		"Connection: close\r\n\r\n");
	if (!TestTrue(TEXT("the announcement loopback server starts"),
		Server.IsReady()))
	{
		return false;
	}
	FWoTPlatformHttpHopSpecEnvironment Override(
		Server.Url(TEXT("/announcement.json")),
		WoTPlatformHttpHopSpecAnnouncementOverrideEnv);
	TUniquePtr<IWoTHttpHopExecutor> Executor =
		WoTCreatePlatformHttpHopExecutor();
	FWoTHttpHopRequest Request;
	Request.Url = FString(WoT::Bootstrap::Trust::AnnouncementUrl) + TEXT(".sig");
	Request.AbsoluteDeadlineSeconds = FPlatformTime::Seconds() + 5.0;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = Executor->ExecuteHop(Request, Response);
	TestFalse(TEXT("the exact announcement signature request reaches loopback"),
		Status.IsError());
	TestEqual(TEXT("the loopback announcement status is retained"),
		Response.Status, 404);
	TestTrue(TEXT("the announcement signature suffix is preserved"),
		Server.RawRequest().StartsWith(
			TEXT("GET /announcement.json.sig HTTP/1.1\r\n")));
	return !HasAnyErrors();
}

#if PLATFORM_WINDOWS || PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseLoopbackOverrideSpec,
	"WoT.Update.PlatformHop.ReleaseLoopbackOverride",
	WoTPlatformHttpHopSpecFlags)

bool FWoTReleaseLoopbackOverrideSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
		"Connection: close\r\n\r\n");
	if (!TestTrue(TEXT("the release loopback server starts"), Server.IsReady()))
	{
		return false;
	}
	FWoTPlatformHttpHopSpecEnvironment Override(
		Server.Url(TEXT("/test-release-base")),
		WoTPlatformHttpHopSpecReleaseBaseOverrideEnv);
	TUniquePtr<IWoTHttpHopExecutor> Executor =
		WoTCreatePlatformHttpHopExecutor();
	FWoTHttpHopRequest Request;
	Request.Url =
		TEXT("https://github.com/aleiby/wot/releases/download/v7/WoT-Win64.zip");
	Request.AbsoluteDeadlineSeconds = FPlatformTime::Seconds() + 5.0;
	Request.bAllowPlainHttpForLoopbackTesting = false;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = Executor->ExecuteHop(Request, Response);
	TestFalse(TEXT("the exact release request reaches loopback"), Status.IsError());
	TestEqual(TEXT("the release loopback status is retained"),
		Response.Status, 200);
	TestTrue(TEXT("tag and asset are appended below the test base"),
		Server.RawRequest().StartsWith(TEXT(
			"GET /test-release-base/releases/download/v7/WoT-Win64.zip HTTP/1.1\r\n")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseNotesLoopbackOverrideSpec,
	"WoT.Update.PlatformHop.ReleaseNotesLoopbackOverride",
	WoTPlatformHttpHopSpecFlags)

bool FWoTReleaseNotesLoopbackOverrideSpec::RunTest(const FString&)
{
	FWoTPlatformHttpHopSpecServer Server(
		"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
		"Connection: close\r\n\r\n");
	if (!TestTrue(TEXT("the release-notes loopback server starts"),
		Server.IsReady()))
	{
		return false;
	}
	FWoTPlatformHttpHopSpecEnvironment Override(
		Server.Url(TEXT("/test-release-base")),
		WoTPlatformHttpHopSpecReleaseBaseOverrideEnv);
	constexpr TCHAR NotesUrl[] = TEXT(
		"https://github.com/aleiby/wot/releases/latest/download/WoT-release-notes-v1.json");
	FString Rewritten;
	TestTrue(TEXT("the one exact latest notes URL rewrites"),
		WoTPlatformHttpHopRewriteReleaseUrlForTesting(NotesUrl, Rewritten));
	TestEqual(TEXT("the exact notes route is appended below the test base"),
		Rewritten, Server.Url(TEXT(
			"/test-release-base/releases/latest/download/WoT-release-notes-v1.json")));
	for (const TCHAR* Refused : {
		TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-release-notes-v1.json?x=1"),
		TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-release-notes-v1.json#fragment"),
		TEXT("https://github.com/aleiby/wot/releases/latest/download/another.json"),
		TEXT("https://github.com/aleiby/wot/releases/latest/download/WoT-Win64.zip"),
	})
	{
		TestFalse(FString::Printf(TEXT("latest route is refused: %s"), Refused),
			WoTPlatformHttpHopRewriteReleaseUrlForTesting(Refused, Rewritten));
	}

	TUniquePtr<IWoTHttpHopExecutor> Executor =
		WoTCreatePlatformHttpHopExecutor();
	FWoTHttpHopRequest Request;
	Request.Url = NotesUrl;
	Request.AbsoluteDeadlineSeconds = FPlatformTime::Seconds() + 5.0;
	FWoTHttpHopResponse Response;
	FWoTBootstrapStatus Status = Executor->ExecuteHop(Request, Response);
	TestFalse(TEXT("the exact release-notes request reaches loopback"),
		Status.IsError());
	TestEqual(TEXT("the release-notes loopback status is retained"),
		Response.Status, 200);
	TestTrue(TEXT("the native request uses only the exact notes path"),
		Server.RawRequest().StartsWith(TEXT(
			"GET /test-release-base/releases/latest/download/WoT-release-notes-v1.json HTTP/1.1\r\n")));
	return !HasAnyErrors();
}
#endif

#endif // WOT_BOOTSTRAP_TEST_ENDPOINTS
#endif // WITH_AUTOMATION_TESTS
