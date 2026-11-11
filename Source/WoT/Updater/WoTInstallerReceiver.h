// LAN installer sharing: the receiver (spec B1, B3, B5).
//
// A copy waiting at the original-game-needed screen listens for OFFERs on UDP
// port P, answers one whose hash and size are its own compiled pin with a
// REQUEST, and accepts exactly one inbound TCP connection: from the paired
// sharer's address, carrying the REQUEST's token. The file streams into
// OriginalGame/<random>.part. A complete copy whose SHA-256 is the pin is
// flushed, renamed onto the pinned leaf without replacing anything, and the
// folder flushed; the bootstrap sweep then finds it like any other installer.
//
// The receiver only ever READS the data connection. The sharer closes right
// after its last byte, and a close with unread inbound data turns into a
// reset that can cut off the tail of the file; the socket seam below has no
// way to write to an accepted connection, and the specs assert none happens.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Templates/Atomic.h"
#include "Online/WoTInstallerShareProtocol.h"
#include "Online/WoTInstallerSharer.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"

class FRunnableThread;
class FEvent;

/** One socket exactly as the receiver may use it. Addresses are IPv4 in host
 * byte order. Datagram sockets bind, receive OFFERs and send REQUESTs; a
 * stream socket binds and listens; an accepted connection is only read. */
class IWoTInstallerReceiveSocket
{
public:
	virtual ~IWoTInstallerReceiveSocket() = default;
	virtual bool Bind(uint32 Address, uint16 Port) = 0;
	/** The bound local port (an OS-chosen one after a bind to port 0). */
	virtual uint16 LocalPort() const = 0;
	virtual bool Listen(int32 Backlog) = 0;
	/** Datagram sockets only: one REQUEST. */
	virtual EWoTShareIo SendTo(uint32 Address, uint16 Port,
		TConstArrayView<uint8> Bytes) = 0;
	/** Datagram sockets only: waits up to WaitSeconds for one datagram. */
	virtual EWoTShareIo RecvFrom(TArray<uint8>& OutBytes, uint32& OutAddress,
		uint16& OutPort, double WaitSeconds) = 0;
	/** Listening sockets only: waits up to WaitSeconds for one inbound
	 * connection; null when none came. */
	virtual TUniquePtr<IWoTInstallerReceiveSocket> Accept(
		uint32& OutPeerAddress, double WaitSeconds) = 0;
	/** Accepted connections only: Done with OutRead > 0, WouldBlock when
	 * nothing arrived within WaitSeconds, Failed once the peer closed or the
	 * connection broke. */
	virtual EWoTShareIo Recv(TArrayView<uint8> Out, int32& OutRead,
		double WaitSeconds) = 0;
	virtual void Close() = 0;
};

class IWoTInstallerReceiveNetwork
{
public:
	virtual ~IWoTInstallerReceiveNetwork() = default;
	virtual TUniquePtr<IWoTInstallerReceiveSocket> CreateDatagram() = 0;
	virtual TUniquePtr<IWoTInstallerReceiveSocket> CreateStream() = 0;
};

/** UE sockets. */
TSharedRef<IWoTInstallerReceiveNetwork> WoTCreateProductionInstallerReceiveNetwork();

enum class EWoTInstallerReceiveState : uint8
{
	/** The discovery port is not bound yet (or binding is being retried). */
	Starting,
	/** Waiting for an offer, or pairing with one. */
	Listening,
	/** A copy is streaming in. */
	Copying,
	/** The verified copy is at the pinned leaf: done, nothing listens. */
	Placed,
	/** Three copies arrived damaged: nothing listens until a new receiver. */
	GaveUp,
	/** The received-installer folder could not be used: nothing listens. */
	Unavailable,
};

const TCHAR* LexToString(EWoTInstallerReceiveState State);

/** A copy of the receiver's state; any thread may read it. */
struct FWoTInstallerReceiveStatus
{
	EWoTInstallerReceiveState State = EWoTInstallerReceiveState::Starting;
	/** Counts copies that started streaming (a valid DATA header arrived). */
	int32 Attempt = 0;
	/** Bytes of the installer held in the .part file (a resumed prefix
	 * included), or the full size once placed. */
	uint64 CompletedBytes = 0;
	uint64 TotalBytes = 0;
	/** The generic display label of the current or last sharer. */
	FString PeerLabel;
	int32 DamagedCopies = 0;
	/** The last copy failed its check and no copy has started since. */
	bool bLastCopyDamaged = false;
	/** Set while the last offer was refused for space: the free bytes the
	 * copy needs. */
	TOptional<uint64> NeededBytes;
	/** The bound discovery port; 0 until bound. */
	uint16 DiscoveryPort = 0;
};

class FWoTInstallerReceiver final
{
public:
	struct FConfig
	{
		/** 0 lets the OS choose (specs read Status().DiscoveryPort). */
		uint16 DiscoveryPort = WoTInstallerShare::DiscoveryPort;
		/** Host order; 0 binds every interface. */
		uint32 BindAddress = 0;
		/** Tests and the test harness only: loopback sharers pass the
		 * private-address rule. */
		bool bAllowLoopbackPeers = false;
		/** This copy's generic label, sent in each REQUEST. */
		FString Label;
		/** The compiled pin: an OFFER must carry exactly this hash and size. */
		uint64 Size = 0;
		FWoTSha256 Sha256;
		/** The pinned installer name the verified copy is renamed to. */
		FString PinnedLeaf;
		/** <user data root>/OriginalGame, created by the owner before the
		 * receiver starts, and its native identity. */
		FString Directory;
		FWoTNativeFileIdentity DirectoryIdentity;
		/** From the REQUEST to an accepted connection with its DATA header. */
		double ConnectTimeoutSeconds = 5.0;
		/** A copy that receives nothing for this long has stopped. */
		double IdleTimeoutSeconds = 15.0;
		/** After Cancel, that sharer's session is ignored this long. */
		double CancelIgnoreSeconds = 30.0;
		double BindRetrySeconds = 2.0;
		/** How long one step waits on a socket for something to arrive. */
		double SocketWaitSeconds = 0.1;
		/** lan-receive-failed/refused: the first per reason, then at most one
		 * per this interval (a sharer whose connection never arrives pairs
		 * every 2 s). */
		double FailureHeartbeatIntervalSeconds = 60.0;
		int32 MaxDamagedCopies = 3;
		/** Free space needed beyond the bytes still to copy. */
		uint64 FreeSpaceMarginBytes = 64ull * 1024ull * 1024ull;
	};

	struct FDependencies
	{
		TSharedPtr<IWoTInstallerReceiveNetwork> Network;
		/** FPlatformTime::Seconds when unset. */
		TFunction<double()> NowSeconds;
		/** Free bytes on the folder's volume; the native query when unset. An
		 * unset answer skips the check (a full disk then fails the write). */
		TFunction<TOptional<uint64>()> FreeBytes;
		/** Called on the receiver's thread once the copy is placed. */
		TFunction<void()> OnPlaced;
		/** Called on the receiver's thread; WoTRecordBootstrapHeartbeat when
		 * unset. */
		TFunction<void(const TCHAR* Event, const FString& Detail)> Heartbeat;
	};

	FWoTInstallerReceiver(FConfig InConfig, FDependencies InDependencies);
	~FWoTInstallerReceiver();
	FWoTInstallerReceiver(const FWoTInstallerReceiver&) = delete;
	FWoTInstallerReceiver& operator=(const FWoTInstallerReceiver&) = delete;

	/** One bounded step. Returns how long the caller may sleep before the next
	 * step (0 = step again now; socket waits already happened inside). */
	double Pump();

	/** The player's Cancel, from any thread: the copy in progress (or the
	 * pairing) stops, its .part is kept for a resume, and that sharer's
	 * session is ignored for CancelIgnoreSeconds. Listening continues. */
	void RequestCancelCopy();

	/** Closes every socket and the .part handle (the file stays for a later
	 * resume). Idempotent; the destructor calls it. */
	void Shutdown();

	/** Any thread. */
	FWoTInstallerReceiveStatus Status() const;

private:
	enum class EStep : uint8
	{
		Waiting,
		Paired,
		Copying,
		/** Verified; placing it onto the pinned leaf. */
		Placing,
		Done,
	};

	bool OpenFolder();
	bool StartListening(double Now);
	void ReadOffers(double Now);
	void HandleOffer(double Now, const FWoTShareOffer& Offer, uint32 From,
		uint16 FromPort);
	void Pair(double Now);
	void Copy(double Now);
	void CatchUpHash();
	void Verify(double Now);
	void Place(double Now);
	void FailAttempt(double Now, const TCHAR* Reason);
	void CancelCopy(double Now);
	void DrainOffers();
	bool EnsurePart();
	void DiscardPart();
	void DeleteLeftoverParts();
	void CloseTransfer();
	void CloseAll();
	void Finish(EWoTInstallerReceiveState FinalState);
	TOptional<uint64> QueryFreeBytes() const;
	void Beat(const TCHAR* Event, const FString& Detail) const;
	void BeatLimited(double Now, const TCHAR* Event, const TCHAR* Key,
		const FString& Detail);
	void Publish(TFunctionRef<void(FWoTInstallerReceiveStatus&)> Change);

	FConfig Config;
	FDependencies Deps;
	EStep Step = EStep::Waiting;
	TUniquePtr<IWoTInstallerReceiveSocket> Datagram;
	TUniquePtr<IWoTInstallerReceiveSocket> Listener;
	TUniquePtr<IWoTInstallerReceiveSocket> Connection;
	FWoTNativeDirectoryHandle Folder;
	bool bFolderOpen = false;
	FWoTNativeFileHandle Part;
	FString PartLeaf;
	uint64 PartBytes = 0;
	/** The hasher covers exactly the first HashedBytes of the .part. */
	FWoTSha256Hasher Hasher;
	uint64 HashedBytes = 0;
	/** The pairing: the offer's session, its source address and the token. */
	FWoTShareId PairedSession;
	FWoTShareId Token;
	uint32 PairedAddress = 0;
	FString PairedLabel;
	bool bPairedSessionSet = false;
	double PairDeadline = 0.0;
	TArray<uint8> Header;
	double LastBytesSeconds = 0.0;
	double NextBindSeconds = 0.0;
	double NextPlaceSeconds = 0.0;
	TSet<FWoTShareId> DamagedSessions;
	TMap<FWoTShareId, double> IgnoredUntil;
	TMap<FString, double> LastLimitedBeat;
	TArray<uint8> Buffer;
	int32 DamagedCount = 0;
	bool bShutdown = false;
	TAtomic<bool> bCancelRequested{false};
	mutable FCriticalSection StatusLock;
	FWoTInstallerReceiveStatus Published;
};

/** Runs one FWoTInstallerReceiver on its own thread until StopAndJoin. */
class FWoTInstallerReceiverThread final : public FRunnable
{
public:
	explicit FWoTInstallerReceiverThread(TUniquePtr<FWoTInstallerReceiver> InCore);
	virtual ~FWoTInstallerReceiverThread() override;

	bool Start();
	/** Stops the loop, joins the thread and shuts the receiver down. */
	void StopAndJoin();
	FWoTInstallerReceiveStatus Status() const;
	void RequestCancelCopy();

	virtual uint32 Run() override;
	/** Any thread: asks the loop to end without joining. */
	virtual void Stop() override;

private:
	TUniquePtr<FWoTInstallerReceiver> Core;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	TAtomic<bool> bStopRequested{false};
};

#if WITH_AUTOMATION_TESTS
/** Worker-level receiver overrides for specs: loopback, an OS-chosen
 * discovery port, a recording network, a free-space answer. */
struct FWoTInstallerReceiverOverridesForTesting
{
	/** The production network when unset. */
	TSharedPtr<IWoTInstallerReceiveNetwork> Network;
	uint16 DiscoveryPort = 0;
	uint32 BindAddress = 0x7F000001u;
	bool bAllowLoopbackPeers = true;
	/** Replaces the platform trigger rule (Wine on Linux starts by itself). */
	TOptional<bool> bStartsAutomatically;
	TFunction<TOptional<uint64>()> FreeBytes;
	double ConnectTimeoutSeconds = 5.0;
	double IdleTimeoutSeconds = 15.0;
	/** How often the worker republishes the copy state while one runs. */
	double PublishIntervalSeconds = 0.25;
};
#endif
