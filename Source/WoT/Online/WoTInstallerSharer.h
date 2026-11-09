// LAN installer sharing: the sharer (spec B1-B3, B5).
//
// The sharer never listens. OFFERs leave an unbound UDP socket (the OS picks
// the port), the only datagrams read are replies to those offers, and every
// transfer is an outward TCP connection. Every socket call goes through the
// seam below so specs can record the calls and prove that.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Templates/Atomic.h"
#include "WoTInstallerShareProtocol.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTBootstrapCore/Hash.h"

class FRunnableThread;
class FEvent;
struct FWoTLocalInstallerSearch;

enum class EWoTShareIo : uint8
{
	Done,
	WouldBlock,
	Failed,
};

/** One socket exactly as the sharer may use it. Addresses are IPv4 in host
 * byte order. Bind and Listen exist only so a recording seam can prove the
 * sharer never calls them. */
class IWoTInstallerShareSocket
{
public:
	virtual ~IWoTInstallerShareSocket() = default;
	virtual bool SetBroadcast() = 0;
	virtual bool Bind(uint32 Address, uint16 Port) = 0;
	virtual bool Listen(int32 Backlog) = 0;
	virtual EWoTShareIo SendTo(uint32 Address, uint16 Port,
		TConstArrayView<uint8> Bytes) = 0;
	/** Waits up to WaitSeconds for one datagram. WouldBlock when none came. */
	virtual EWoTShareIo RecvFrom(TArray<uint8>& OutBytes, uint32& OutAddress,
		uint16& OutPort, double WaitSeconds) = 0;
	/** Starts a non-blocking outward connection. */
	virtual bool BeginConnect(uint32 Address, uint16 Port) = 0;
	/** Done once connected, WouldBlock while in progress, Failed on error. */
	virtual EWoTShareIo PollConnect() = 0;
	/** Non-blocking: Done with OutSent > 0, WouldBlock when the send buffer is
	 * full, Failed when the connection is gone. */
	virtual EWoTShareIo Send(TConstArrayView<uint8> Bytes, int32& OutSent) = 0;
	/** Sleeps until the socket can take more bytes, at most WaitSeconds. */
	virtual void WaitForWrite(double WaitSeconds) = 0;
	virtual void Close() = 0;
};

class IWoTInstallerShareNetwork
{
public:
	virtual ~IWoTInstallerShareNetwork() = default;
	virtual TUniquePtr<IWoTInstallerShareSocket> CreateDatagram() = 0;
	virtual TUniquePtr<IWoTInstallerShareSocket> CreateStream() = 0;
	virtual TArray<FWoTShareAdapterAddress> EnumerateAdapters() = 0;
};

/** UE sockets; adapters from GetAdaptersAddresses (Windows, also under Wine)
 * or getifaddrs (Mac). */
TSharedRef<IWoTInstallerShareNetwork> WoTCreateProductionInstallerShareNetwork();

/** The found installer, held open for one transfer. */
class IWoTInstallerShareReader
{
public:
	virtual ~IWoTInstallerShareReader() = default;
	/** Up to Out.Num() bytes at Offset. Zero bytes or an error ends the transfer. */
	virtual TWoTBootstrapResult<uint64> ReadAt(uint64 Offset,
		TArrayView<uint8> Out) = 0;
	/** False once the held file is no longer the object that was found. */
	virtual bool IsUnchanged() = 0;
};

/** Where the sharer's installer comes from. */
class IWoTInstallerShareSource
{
public:
	virtual ~IWoTInstallerShareSource() = default;
	/** An Inspect-only search; never hashes. Empty when nothing was found,
	 * otherwise a token naming where (adjacent, remembered, ...). */
	virtual FString Find() = 0;
	/** Opens what the last Find found, or null. */
	virtual TUniquePtr<IWoTInstallerShareReader> Open() = 0;
};

/** The production source over WoTFindLocalInstallerForSharing. */
TSharedRef<IWoTInstallerShareSource> WoTCreateLocalInstallerShareSource(
	const FWoTLocalInstallerSearch& Search);

enum class EWoTShareTransferResult : uint8
{
	Sent,
	/** Sharing was switched off, the game is closing, or the receiver hung up. */
	Stopped,
	/** 15 s without send progress (a receiver that went to sleep). */
	Stalled,
	ConnectFailed,
	ReadFailed,
	/** The held file changed while it was being sent. */
	Changed,
	/** The whole file went out and its SHA-256 is not the pin. */
	BadHash,
};

const TCHAR* LexToString(EWoTShareTransferResult Result);

/** Posted from the sharer thread; the owner marshals it to the game thread. */
struct FWoTInstallerSharerEvent
{
	enum class EKind : uint8
	{
		/** The outward connection is up: "Sending ... to <label>...". */
		SendStarted,
		/** Follows a SendStarted: "... sent" or "... stopped". */
		SendEnded,
		/** A search finished; bFound says whether it found an installer. */
		SearchFinished,
	};

	EKind Kind = EKind::SearchFinished;
	/** A generic display label, never a peer-chosen string. */
	FString Label;
	EWoTShareTransferResult Result = EWoTShareTransferResult::Sent;
	bool bFound = false;
};

class FWoTInstallerSharer final
{
public:
	struct FConfig
	{
		uint16 DiscoveryPort = WoTInstallerShare::DiscoveryPort;
		/** This copy's generic label. */
		FString Label;
		/** The pinned installer: offered, and checked while streaming. */
		uint64 Size = 0;
		FWoTSha256 Sha256;
		double OfferIntervalSeconds = 2.0;
		double SearchIntervalSeconds = 60.0;
		double RequestIntervalSeconds = 1.0;
		double StallSeconds = 15.0;
		double ConnectTimeoutSeconds = 5.0;
		double OfferHeartbeatIntervalSeconds = 60.0;
		int32 MaxTransfers = 2;
		/** Tests and the test harness only: loopback peers pass the
		 * private-address rule. */
		bool bAllowLoopbackPeers = false;
		/** Tests and the test harness only: replaces the broadcast
		 * destinations (a loopback receiver never sees a broadcast). */
		TArray<uint32> DestinationsOverride;
	};

	struct FDependencies
	{
		TSharedPtr<IWoTInstallerShareNetwork> Network;
		TSharedPtr<IWoTInstallerShareSource> Source;
		TFunction<double()> NowSeconds;
		/** Called on the sharer's thread. */
		TFunction<void(const FWoTInstallerSharerEvent&)> OnEvent;
		/** Called on the sharer's thread; WoTRecordBootstrapHeartbeat when unset. */
		TFunction<void(const TCHAR* Event, const FString& Detail)> Heartbeat;
	};

	FWoTInstallerSharer(FConfig InConfig, FDependencies InDependencies);
	~FWoTInstallerSharer();
	FWoTInstallerSharer(const FWoTInstallerSharer&) = delete;
	FWoTInstallerSharer& operator=(const FWoTInstallerSharer&) = delete;

	/** One bounded step: search when due, offer when due, read requests,
	 * advance transfers. Returns how long the caller may sleep before the
	 * next step (0 = step again now). */
	double Pump();

	/** Ends every transfer as stopped and closes every socket. Idempotent;
	 * the destructor calls it. */
	void Shutdown();

	int32 ActiveTransferCount() const { return Transfers.Num(); }
	bool HasFoundInstaller() const { return bFound; }
	/** The streamed bytes did not hash to the pin: no more offers this session. */
	bool IsInstallerRejected() const { return bRejected; }
	const FWoTShareId& SessionId() const { return Session; }

private:
	struct FTransfer;

	void Search(double Now);
	void Offer(double Now);
	void ReceiveRequests(double Now, double WaitSeconds);
	void HandleRequest(double Now, const FWoTShareRequest& Request,
		uint32 SourceAddress);
	/** True when the transfer ended (it is then removed by the caller). */
	bool Advance(FTransfer& Transfer, double Now, bool& bOutBlocked);
	void End(FTransfer& Transfer, EWoTShareTransferResult Result);
	void Emit(const FWoTInstallerSharerEvent& Event) const;
	void Beat(const TCHAR* Event, const FString& Detail) const;
	void LoseInstaller(double Now);

	FConfig Config;
	FDependencies Deps;
	FWoTShareId Session;
	TUniquePtr<IWoTInstallerShareSocket> Datagram;
	TArray<TUniquePtr<FTransfer>> Transfers;
	TMap<uint32, double> LastRequestSeconds;
	TSet<FWoTShareId> UsedTokens;
	FString FoundWhere;
	double NextSearchSeconds = 0.0;
	double NextOfferSeconds = 0.0;
	double LastOfferHeartbeatSeconds = 0.0;
	bool bFound = false;
	bool bSearched = false;
	bool bRejected = false;
	bool bOfferHeartbeatSent = false;
	bool bShutdown = false;
};

/** Runs one FWoTInstallerSharer on its own thread until StopAndJoin. */
class FWoTInstallerSharerThread final : public FRunnable
{
public:
	explicit FWoTInstallerSharerThread(TUniquePtr<FWoTInstallerSharer> InCore);
	virtual ~FWoTInstallerSharerThread() override;

	bool Start();
	/** Stops the loop, ends every transfer as stopped and joins the thread. */
	void StopAndJoin();

	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	TUniquePtr<FWoTInstallerSharer> Core;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	TAtomic<bool> bStopRequested{false};
};
