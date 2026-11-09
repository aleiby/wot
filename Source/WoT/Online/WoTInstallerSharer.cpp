#include "WoTInstallerSharer.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "WoTBootstrapHeartbeat.h"
#include "Updater/WoTLocalInstallerForSharing.h"
#include "WoTBootstrapCore/NativeHandles.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC || PLATFORM_LINUX
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace
{
/** One file read and the most one transfer sends per step, so the offers,
 * the requests and a second transfer keep moving. */
constexpr int32 WoTShareChunkBytes = 256 * 1024;
constexpr int32 WoTShareStepBudgetBytes = 4 * 1024 * 1024;
/** Bounds one step's request reading. */
constexpr int32 WoTShareMaxDatagramsPerStep = 16;
/** Larger than any valid REQUEST; a longer datagram is truncated and then
 * fails decoding. */
constexpr int32 WoTShareMaxDatagramBytes = 512;
/** How long an idle sharer waits on its offer socket per step. */
constexpr double WoTShareIdleWaitSeconds = 0.25;
/** How long a step waits for a full send buffer to drain. */
constexpr double WoTShareWriteWaitSeconds = 0.02;

class FWoTUeShareSocket final : public IWoTInstallerShareSocket
{
public:
	FWoTUeShareSocket(ISocketSubsystem& InSubsystem, FSocket* InSocket)
		: Subsystem(InSubsystem)
		, Socket(InSocket)
	{
	}

	virtual ~FWoTUeShareSocket() override
	{
		Close();
	}

	virtual bool SetBroadcast() override
	{
		return Socket != nullptr && Socket->SetBroadcast(true);
	}

	virtual bool Bind(uint32 Address, uint16 Port) override
	{
		return Socket != nullptr && Socket->Bind(*MakeAddress(Address, Port));
	}

	virtual bool Listen(int32 Backlog) override
	{
		return Socket != nullptr && Socket->Listen(Backlog);
	}

	virtual EWoTShareIo SendTo(uint32 Address, uint16 Port,
		TConstArrayView<uint8> Bytes) override
	{
		if (Socket == nullptr)
		{
			return EWoTShareIo::Failed;
		}
		int32 Sent = 0;
		if (Socket->SendTo(Bytes.GetData(), Bytes.Num(), Sent,
				*MakeAddress(Address, Port))
			&& Sent == Bytes.Num())
		{
			return EWoTShareIo::Done;
		}
		return LastErrorWouldBlock() ? EWoTShareIo::WouldBlock
			: EWoTShareIo::Failed;
	}

	virtual EWoTShareIo RecvFrom(TArray<uint8>& OutBytes, uint32& OutAddress,
		uint16& OutPort, double WaitSeconds) override
	{
		OutBytes.Reset();
		if (Socket == nullptr)
		{
			return EWoTShareIo::Failed;
		}
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead,
				FTimespan::FromSeconds(FMath::Max(0.0, WaitSeconds))))
		{
			return EWoTShareIo::WouldBlock;
		}
		OutBytes.SetNumUninitialized(WoTShareMaxDatagramBytes);
		const TSharedRef<FInternetAddr> From =
			Subsystem.CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		int32 Read = 0;
		if (!Socket->RecvFrom(OutBytes.GetData(), OutBytes.Num(), Read, *From)
			|| Read < 0)
		{
			OutBytes.Reset();
			return LastErrorWouldBlock() ? EWoTShareIo::WouldBlock
				: EWoTShareIo::Failed;
		}
		OutBytes.SetNum(Read, EAllowShrinking::No);
		From->GetIp(OutAddress);
		OutPort = static_cast<uint16>(From->GetPort());
		return EWoTShareIo::Done;
	}

	virtual bool BeginConnect(uint32 Address, uint16 Port) override
	{
		return Socket != nullptr && Socket->SetNonBlocking(true)
			&& Socket->Connect(*MakeAddress(Address, Port));
	}

	virtual EWoTShareIo PollConnect() override
	{
		if (Socket == nullptr
			|| Socket->GetConnectionState() == SCS_ConnectionError)
		{
			return EWoTShareIo::Failed;
		}
		if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::Zero()))
		{
			return EWoTShareIo::WouldBlock;
		}
		const TSharedRef<FInternetAddr> Peer =
			Subsystem.CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		return Socket->GetPeerAddress(*Peer) ? EWoTShareIo::Done
			: EWoTShareIo::Failed;
	}

	virtual EWoTShareIo Send(TConstArrayView<uint8> Bytes, int32& OutSent) override
	{
		OutSent = 0;
		if (Socket == nullptr)
		{
			return EWoTShareIo::Failed;
		}
		int32 Sent = 0;
		if (!Socket->Send(Bytes.GetData(), Bytes.Num(), Sent))
		{
			return LastErrorWouldBlock() ? EWoTShareIo::WouldBlock
				: EWoTShareIo::Failed;
		}
		OutSent = Sent;
		return Sent > 0 ? EWoTShareIo::Done : EWoTShareIo::WouldBlock;
	}

	virtual void WaitForWrite(double WaitSeconds) override
	{
		if (Socket != nullptr)
		{
			(void)Socket->Wait(ESocketWaitConditions::WaitForWrite,
				FTimespan::FromSeconds(FMath::Max(0.0, WaitSeconds)));
		}
	}

	virtual void Close() override
	{
		if (Socket != nullptr)
		{
			Socket->Close();
			Subsystem.DestroySocket(Socket);
			Socket = nullptr;
		}
	}

private:
	TSharedRef<FInternetAddr> MakeAddress(uint32 Address, uint16 Port) const
	{
		TSharedRef<FInternetAddr> Result =
			Subsystem.CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Result->SetIp(Address);
		Result->SetPort(Port);
		return Result;
	}

	bool LastErrorWouldBlock() const
	{
		return Subsystem.GetLastErrorCode() == SE_EWOULDBLOCK;
	}

	ISocketSubsystem& Subsystem;
	FSocket* Socket = nullptr;
};

#if PLATFORM_MAC || PLATFORM_LINUX
/** The prefix of a contiguous netmask; 0 for a non-contiguous one. */
uint8 WoTSharePrefixLength(uint32 Netmask)
{
	uint8 Length = 0;
	while (Length < 32 && (Netmask & (0x80000000u >> Length)) != 0)
	{
		++Length;
	}
	const uint32 Expected = Length == 0 ? 0u : (0xFFFFFFFFu << (32 - Length));
	return Netmask == Expected ? Length : 0;
}
#endif

TArray<FWoTShareAdapterAddress> WoTShareEnumerateAdapters()
{
	TArray<FWoTShareAdapterAddress> Result;
#if PLATFORM_WINDOWS
	// Native Windows and Wine both implement OnLinkPrefixLength and DadState.
	const ULONG Flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
		| GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
	ULONG Size = 16 * 1024;
	TArray<uint8> Buffer;
	ULONG Status = ERROR_BUFFER_OVERFLOW;
	for (int32 Attempt = 0; Attempt < 3 && Status == ERROR_BUFFER_OVERFLOW;
		++Attempt)
	{
		Buffer.SetNumUninitialized(static_cast<int32>(Size));
		Status = ::GetAdaptersAddresses(AF_INET, Flags, nullptr,
			reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.GetData()), &Size);
	}
	if (Status != NO_ERROR)
	{
		return Result;
	}
	for (const IP_ADAPTER_ADDRESSES* Adapter =
			reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(Buffer.GetData());
		Adapter != nullptr; Adapter = Adapter->Next)
	{
		const bool bUp = Adapter->OperStatus == IfOperStatusUp;
		const bool bLoopback = Adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
		// VPN adapters: tunnels, PPP, and the proprietary virtual type that
		// WireGuard and OpenVPN's data-channel-offload adapter report.
		const bool bTunnel = Adapter->IfType == IF_TYPE_TUNNEL
			|| Adapter->IfType == IF_TYPE_PPP
			|| Adapter->IfType == IF_TYPE_PROP_VIRTUAL;
		for (const IP_ADAPTER_UNICAST_ADDRESS* Unicast =
				Adapter->FirstUnicastAddress;
			Unicast != nullptr; Unicast = Unicast->Next)
		{
			const SOCKADDR* Raw = Unicast->Address.lpSockaddr;
			if (Raw == nullptr || Raw->sa_family != AF_INET)
			{
				continue;
			}
			FWoTShareAdapterAddress Entry;
			Entry.Address = ntohl(
				reinterpret_cast<const sockaddr_in*>(Raw)->sin_addr.s_addr);
			Entry.PrefixLength = Unicast->OnLinkPrefixLength;
			Entry.bUp = bUp;
			Entry.bLoopback = bLoopback;
			Entry.bTunnel = bTunnel;
			Entry.bTentative = Unicast->DadState != IpDadStatePreferred
				&& Unicast->DadState != IpDadStateDeprecated;
			Result.Add(Entry);
		}
	}
#elif PLATFORM_MAC || PLATFORM_LINUX
	// The same skips as the LANBeacon WoT patch: up, broadcast-capable, not
	// loopback; point-to-point links (utun VPNs, PPP) count as tunnels.
	ifaddrs* Interfaces = nullptr;
	if (getifaddrs(&Interfaces) != 0 || Interfaces == nullptr)
	{
		return Result;
	}
	for (const ifaddrs* Cursor = Interfaces; Cursor != nullptr;
		Cursor = Cursor->ifa_next)
	{
		if (Cursor->ifa_addr == nullptr || Cursor->ifa_addr->sa_family != AF_INET)
		{
			continue;
		}
		const unsigned int IfFlags = Cursor->ifa_flags;
		FWoTShareAdapterAddress Entry;
		Entry.Address = ntohl(reinterpret_cast<const sockaddr_in*>(
			Cursor->ifa_addr)->sin_addr.s_addr);
		Entry.PrefixLength = Cursor->ifa_netmask == nullptr ? 0
			: WoTSharePrefixLength(ntohl(reinterpret_cast<const sockaddr_in*>(
				Cursor->ifa_netmask)->sin_addr.s_addr));
		Entry.bUp = (IfFlags & IFF_UP) != 0 && (IfFlags & IFF_RUNNING) != 0;
		Entry.bLoopback = (IfFlags & IFF_LOOPBACK) != 0;
		Entry.bTunnel = (IfFlags & IFF_POINTOPOINT) != 0
			|| (IfFlags & IFF_BROADCAST) == 0;
		Result.Add(Entry);
	}
	freeifaddrs(Interfaces);
#endif
	return Result;
}

class FWoTUeShareNetwork final : public IWoTInstallerShareNetwork
{
public:
	virtual TUniquePtr<IWoTInstallerShareSocket> CreateDatagram() override
	{
		return Create(NAME_DGram, TEXT("WoTInstallerShareOffer"));
	}

	virtual TUniquePtr<IWoTInstallerShareSocket> CreateStream() override
	{
		return Create(NAME_Stream, TEXT("WoTInstallerShareData"));
	}

	virtual TArray<FWoTShareAdapterAddress> EnumerateAdapters() override
	{
		return WoTShareEnumerateAdapters();
	}

private:
	static TUniquePtr<IWoTInstallerShareSocket> Create(
		const FName& Type, const TCHAR* Description)
	{
		ISocketSubsystem* Subsystem =
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Subsystem == nullptr)
		{
			return nullptr;
		}
		FSocket* Socket = Subsystem->CreateSocket(Type, Description,
			FNetworkProtocolTypes::IPv4);
		if (Socket == nullptr)
		{
			return nullptr;
		}
		return MakeUnique<FWoTUeShareSocket>(*Subsystem, Socket);
	}
};

class FWoTHeldInstallerShareReader final : public IWoTInstallerShareReader
{
public:
	FWoTHeldInstallerShareReader(FWoTNativeFileHandle&& InHandle,
		const FWoTNativeFileIdentity& InIdentity)
		: Handle(MoveTemp(InHandle))
		, Identity(InIdentity)
	{
	}

	virtual TWoTBootstrapResult<uint64> ReadAt(uint64 Offset,
		TArrayView<uint8> Out) override
	{
		return ReadNativeFileAt(Handle, Offset, Out);
	}

	virtual bool IsUnchanged() override
	{
		const TWoTBootstrapResult<FWoTNativeFileIdentity> Current =
			Handle.QueryCurrentIdentity();
		return !Current.IsError() && Current.GetValue() == Identity;
	}

private:
	FWoTNativeFileHandle Handle;
	FWoTNativeFileIdentity Identity;
};

class FWoTLocalInstallerShareSource final : public IWoTInstallerShareSource
{
public:
	explicit FWoTLocalInstallerShareSource(const FWoTLocalInstallerSearch& InSearch)
		: Search(InSearch)
	{
	}

	virtual FString Find() override
	{
		Found = WoTFindLocalInstallerForSharing(Search);
		return Found.IsSet() ? FString(LexToString(Found->Source)) : FString();
	}

	virtual TUniquePtr<IWoTInstallerShareReader> Open() override
	{
		if (!Found.IsSet())
		{
			return nullptr;
		}
		TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
			WoTOpenLocalInstallerForSharing(Found.GetValue());
		if (Opened.IsError())
		{
			return nullptr;
		}
		return MakeUnique<FWoTHeldInstallerShareReader>(
			MoveTemp(Opened).GetValue(), Found->Identity);
	}

private:
	FWoTLocalInstallerSearch Search;
	TOptional<FWoTLocalInstallerForSharing> Found;
};
} // namespace

TSharedRef<IWoTInstallerShareNetwork> WoTCreateProductionInstallerShareNetwork()
{
	return MakeShared<FWoTUeShareNetwork>();
}

TSharedRef<IWoTInstallerShareSource> WoTCreateLocalInstallerShareSource(
	const FWoTLocalInstallerSearch& Search)
{
	return MakeShared<FWoTLocalInstallerShareSource>(Search);
}

const TCHAR* LexToString(EWoTShareTransferResult Result)
{
	switch (Result)
	{
	case EWoTShareTransferResult::Sent: return TEXT("sent");
	case EWoTShareTransferResult::Stopped: return TEXT("stopped");
	case EWoTShareTransferResult::Stalled: return TEXT("stalled");
	case EWoTShareTransferResult::ConnectFailed: return TEXT("connect-failed");
	case EWoTShareTransferResult::ReadFailed: return TEXT("read-failed");
	case EWoTShareTransferResult::Changed: return TEXT("changed");
	case EWoTShareTransferResult::BadHash: return TEXT("bad-hash");
	default: return TEXT("unknown");
	}
}

struct FWoTInstallerSharer::FTransfer
{
	TUniquePtr<IWoTInstallerShareSocket> Socket;
	TUniquePtr<IWoTInstallerShareReader> Reader;
	FWoTShareId Token;
	uint64 Offset = 0;
	uint64 Length = 0;
	/** A generic display label. */
	FString Label;
	const TCHAR* Peer = TEXT("other");
	double StartedSeconds = 0.0;
	double LastProgressSeconds = 0.0;
	/** Bytes waiting for the socket: the DATA header, then file chunks. */
	TArray<uint8> Pending;
	int32 PendingSent = 0;
	uint64 NextRead = 0;
	uint64 BytesOnWire = 0;
	FWoTSha256Hasher Hasher;
	/** Only a transfer from offset 0 sees the whole file, so only it can
	 * check the file against the pin. */
	bool bHashing = false;
	bool bConnected = false;
	bool bAnnounced = false;
	bool bEnded = false;
};

FWoTInstallerSharer::FWoTInstallerSharer(
	FConfig InConfig, FDependencies InDependencies)
	: Config(MoveTemp(InConfig))
	, Deps(MoveTemp(InDependencies))
	, Session(FWoTShareId::Random())
{
	if (!Deps.NowSeconds)
	{
		Deps.NowSeconds = []()
		{
			return FPlatformTime::Seconds();
		};
	}
}

FWoTInstallerSharer::~FWoTInstallerSharer()
{
	Shutdown();
}

double FWoTInstallerSharer::Pump()
{
	if (bShutdown)
	{
		return 1.0;
	}
	const double Now = Deps.NowSeconds();
	if (!bFound && !bRejected && (!bSearched || Now >= NextSearchSeconds))
	{
		Search(Now);
	}
	if (bFound && !bRejected && Now >= NextOfferSeconds)
	{
		Offer(Now);
	}
	if (Datagram.IsValid())
	{
		const double Wait = Transfers.IsEmpty()
			? FMath::Clamp(NextOfferSeconds - Now, 0.0, WoTShareIdleWaitSeconds)
			: 0.0;
		ReceiveRequests(Now, Wait);
	}

	bool bAllBlocked = !Transfers.IsEmpty();
	IWoTInstallerShareSocket* Blocked = nullptr;
	for (int32 Index = 0; Index < Transfers.Num();)
	{
		bool bBlocked = false;
		if (Advance(*Transfers[Index], Deps.NowSeconds(), bBlocked))
		{
			Transfers.RemoveAt(Index);
			continue;
		}
		if (bBlocked)
		{
			if (Blocked == nullptr)
			{
				Blocked = Transfers[Index]->Socket.Get();
			}
		}
		else
		{
			bAllBlocked = false;
		}
		++Index;
	}
	if (bRejected)
	{
		// The file is not the installer: every other copy of it is junk too.
		for (const TUniquePtr<FTransfer>& Transfer : Transfers)
		{
			End(*Transfer, EWoTShareTransferResult::Stopped);
		}
		Transfers.Reset();
		if (Datagram.IsValid())
		{
			Datagram->Close();
			Datagram.Reset();
		}
	}
	if (!Transfers.IsEmpty())
	{
		if (bAllBlocked && Blocked != nullptr)
		{
			Blocked->WaitForWrite(WoTShareWriteWaitSeconds);
		}
		return 0.0;
	}
	if (Datagram.IsValid())
	{
		// The step already waited on the offer socket.
		return 0.0;
	}
	if (bRejected || bFound)
	{
		return 1.0;
	}
	return FMath::Clamp(NextSearchSeconds - Now, 0.0, 1.0);
}

void FWoTInstallerSharer::Search(double Now)
{
	bSearched = true;
	NextSearchSeconds = Now + Config.SearchIntervalSeconds;
	FoundWhere = Deps.Source.IsValid() ? Deps.Source->Find() : FString();
	bFound = !FoundWhere.IsEmpty();
	if (bFound)
	{
		NextOfferSeconds = Now;
	}
	FWoTInstallerSharerEvent Event;
	Event.Kind = FWoTInstallerSharerEvent::EKind::SearchFinished;
	Event.bFound = bFound;
	Emit(Event);
}

void FWoTInstallerSharer::LoseInstaller(double Now)
{
	bFound = false;
	FoundWhere.Reset();
	// Search again on the normal cadence: a file that keeps failing to open
	// never turns into a search on every offer.
	NextSearchSeconds = Now + Config.SearchIntervalSeconds;
	// Nothing to offer: no socket either, until a search finds it again.
	if (Datagram.IsValid())
	{
		Datagram->Close();
		Datagram.Reset();
	}
	FWoTInstallerSharerEvent Event;
	Event.Kind = FWoTInstallerSharerEvent::EKind::SearchFinished;
	Event.bFound = false;
	Emit(Event);
}

void FWoTInstallerSharer::Offer(double Now)
{
	NextOfferSeconds = Now + Config.OfferIntervalSeconds;
	if (!Deps.Network.IsValid())
	{
		return;
	}
	if (!Datagram.IsValid())
	{
		// Never bound: the OS picks the port on the first send.
		Datagram = Deps.Network->CreateDatagram();
		if (!Datagram.IsValid())
		{
			return;
		}
		if (!Datagram->SetBroadcast())
		{
			Datagram->Close();
			Datagram.Reset();
			return;
		}
	}
	FWoTShareOffer Message;
	Message.SessionId = Session;
	Message.Sha256 = Config.Sha256;
	Message.Size = Config.Size;
	Message.Label = Config.Label;
	const TArray<uint8> Bytes = WoTInstallerShare::EncodeOffer(Message);
	if (Bytes.IsEmpty())
	{
		return;
	}
	const TArray<uint32> Destinations = Config.DestinationsOverride.IsEmpty()
		? WoTInstallerShare::OfferDestinations(Deps.Network->EnumerateAdapters())
		: Config.DestinationsOverride;
	int32 Failed = 0;
	for (const uint32 Destination : Destinations)
	{
		// A refused destination (an interface that went away, a Mac refusing
		// the limited broadcast) never stops the others.
		if (Datagram->SendTo(Destination, Config.DiscoveryPort, Bytes)
			!= EWoTShareIo::Done)
		{
			++Failed;
		}
	}
	if (!bOfferHeartbeatSent || Now - LastOfferHeartbeatSeconds
			>= Config.OfferHeartbeatIntervalSeconds)
	{
		bOfferHeartbeatSent = true;
		LastOfferHeartbeatSeconds = Now;
		Beat(TEXT("lan-share-offer"), FString::Printf(
			TEXT("source=%s destinations=%d failed=%d"), *FoundWhere,
			Destinations.Num(), Failed));
	}
}

void FWoTInstallerSharer::ReceiveRequests(double Now, double WaitSeconds)
{
	TArray<uint8> Bytes;
	for (int32 Count = 0;
		Count < WoTShareMaxDatagramsPerStep && Datagram.IsValid(); ++Count)
	{
		uint32 From = 0;
		uint16 FromPort = 0;
		if (Datagram->RecvFrom(Bytes, From, FromPort,
				Count == 0 ? WaitSeconds : 0.0) != EWoTShareIo::Done)
		{
			break;
		}
		if (const TOptional<FWoTShareRequest> Request =
				WoTInstallerShare::DecodeRequest(Bytes))
		{
			HandleRequest(Now, Request.GetValue(), From);
		}
	}
	for (auto It = LastRequestSeconds.CreateIterator(); It; ++It)
	{
		if (Now - It.Value() > Config.RequestIntervalSeconds * 10.0)
		{
			It.RemoveCurrent();
		}
	}
}

void FWoTInstallerSharer::HandleRequest(double Now,
	const FWoTShareRequest& Request, uint32 SourceAddress)
{
	// Nothing outside the local network ever gets a connection.
	if (!WoTInstallerShare::IsPrivatePeerAddress(
			SourceAddress, Config.bAllowLoopbackPeers)
		|| Request.SessionId != Session)
	{
		return;
	}
	if (const double* Last = LastRequestSeconds.Find(SourceAddress);
		Last != nullptr && Now - *Last < Config.RequestIntervalSeconds)
	{
		return;
	}
	LastRequestSeconds.Add(SourceAddress, Now);
	if (!bFound || bRejected || Transfers.Num() >= Config.MaxTransfers
		|| UsedTokens.Contains(Request.Token)
		|| Request.TcpPort < WoTInstallerShare::MinimumTcpPort
		|| Request.ResumeOffset >= Config.Size || !Deps.Network.IsValid())
	{
		return;
	}
	UsedTokens.Add(Request.Token);
	TUniquePtr<IWoTInstallerShareReader> Reader =
		Deps.Source.IsValid() ? Deps.Source->Open() : nullptr;
	if (!Reader.IsValid())
	{
		LoseInstaller(Now);
		return;
	}
	TUniquePtr<FTransfer> Transfer = MakeUnique<FTransfer>();
	Transfer->Reader = MoveTemp(Reader);
	Transfer->Token = Request.Token;
	Transfer->Offset = Request.ResumeOffset;
	Transfer->Length = Config.Size - Request.ResumeOffset;
	Transfer->NextRead = Request.ResumeOffset;
	Transfer->bHashing = Request.ResumeOffset == 0;
	Transfer->Label = WoTInstallerShare::DisplayLabel(Request.Label);
	Transfer->Peer = WoTInstallerShare::PeerToken(Transfer->Label);
	Transfer->StartedSeconds = Now;
	Transfer->LastProgressSeconds = Now;
	Beat(TEXT("lan-share-send-start"), FString::Printf(
		TEXT("peer=%s offset=%llu length=%llu"), Transfer->Peer,
		static_cast<unsigned long long>(Transfer->Offset),
		static_cast<unsigned long long>(Transfer->Length)));
	// Outward only: the receiver listens, the sharer connects.
	Transfer->Socket = Deps.Network->CreateStream();
	if (!Transfer->Socket.IsValid()
		|| !Transfer->Socket->BeginConnect(SourceAddress, Request.TcpPort))
	{
		End(*Transfer, EWoTShareTransferResult::ConnectFailed);
		return;
	}
	Transfers.Add(MoveTemp(Transfer));
}

bool FWoTInstallerSharer::Advance(FTransfer& Transfer, double Now,
	bool& bOutBlocked)
{
	bOutBlocked = false;
	if (!Transfer.bConnected)
	{
		const EWoTShareIo Connected = Transfer.Socket->PollConnect();
		if (Connected == EWoTShareIo::Failed
			|| (Connected == EWoTShareIo::WouldBlock
				&& Now - Transfer.StartedSeconds >= Config.ConnectTimeoutSeconds))
		{
			End(Transfer, EWoTShareTransferResult::ConnectFailed);
			return true;
		}
		if (Connected == EWoTShareIo::WouldBlock)
		{
			bOutBlocked = true;
			return false;
		}
		Transfer.bConnected = true;
		Transfer.LastProgressSeconds = Now;
		FWoTShareDataHeader Header;
		Header.Token = Transfer.Token;
		Header.Offset = Transfer.Offset;
		Header.Length = Transfer.Length;
		Transfer.Pending = WoTInstallerShare::EncodeDataHeader(Header);
		Transfer.PendingSent = 0;
		Transfer.bAnnounced = true;
		FWoTInstallerSharerEvent Event;
		Event.Kind = FWoTInstallerSharerEvent::EKind::SendStarted;
		Event.Label = Transfer.Label;
		Emit(Event);
	}
	int32 Budget = WoTShareStepBudgetBytes;
	while (Budget > 0)
	{
		if (Transfer.PendingSent >= Transfer.Pending.Num())
		{
			if (Transfer.NextRead >= Config.Size)
			{
				if (!Transfer.Reader->IsUnchanged())
				{
					End(Transfer, EWoTShareTransferResult::Changed);
					LoseInstaller(Now);
					return true;
				}
				if (Transfer.bHashing && Transfer.Hasher.Finish() != Config.Sha256)
				{
					// The right size but not the installer: stop offering it
					// for the rest of this session.
					bRejected = true;
					End(Transfer, EWoTShareTransferResult::BadHash);
					return true;
				}
				End(Transfer, EWoTShareTransferResult::Sent);
				return true;
			}
			const int32 Want = static_cast<int32>(FMath::Min<uint64>(
				WoTShareChunkBytes, Config.Size - Transfer.NextRead));
			Transfer.Pending.SetNumUninitialized(Want, EAllowShrinking::No);
			const TWoTBootstrapResult<uint64> Read = Transfer.Reader->ReadAt(
				Transfer.NextRead, TArrayView<uint8>(Transfer.Pending.GetData(), Want));
			if (Read.IsError() || Read.GetValue() == 0
				|| Read.GetValue() > static_cast<uint64>(Want))
			{
				End(Transfer, EWoTShareTransferResult::ReadFailed);
				LoseInstaller(Now);
				return true;
			}
			Transfer.Pending.SetNum(static_cast<int32>(Read.GetValue()),
				EAllowShrinking::No);
			if (Transfer.bHashing)
			{
				Transfer.Hasher.Update(Transfer.Pending);
			}
			Transfer.PendingSent = 0;
			Transfer.NextRead += Read.GetValue();
		}
		int32 Sent = 0;
		const EWoTShareIo Io = Transfer.Socket->Send(TConstArrayView<uint8>(
			Transfer.Pending.GetData() + Transfer.PendingSent,
			Transfer.Pending.Num() - Transfer.PendingSent), Sent);
		if (Io == EWoTShareIo::Failed)
		{
			// The receiver hung up (Cancel, a damaged copy) or the link died.
			End(Transfer, EWoTShareTransferResult::Stopped);
			return true;
		}
		if (Io != EWoTShareIo::Done || Sent <= 0)
		{
			bOutBlocked = true;
			break;
		}
		Sent = FMath::Min(Sent, Transfer.Pending.Num() - Transfer.PendingSent);
		Transfer.PendingSent += Sent;
		Transfer.BytesOnWire += static_cast<uint64>(Sent);
		Transfer.LastProgressSeconds = Now;
		Budget -= Sent;
	}
	if (bOutBlocked && Now - Transfer.LastProgressSeconds >= Config.StallSeconds)
	{
		End(Transfer, EWoTShareTransferResult::Stalled);
		return true;
	}
	return false;
}

void FWoTInstallerSharer::End(FTransfer& Transfer, EWoTShareTransferResult Result)
{
	if (Transfer.bEnded)
	{
		return;
	}
	Transfer.bEnded = true;
	if (Transfer.Socket.IsValid())
	{
		Transfer.Socket->Close();
	}
	Transfer.Reader.Reset();
	const uint64 HeaderBytes = WoTInstallerShare::DataHeaderBytes;
	const uint64 FileBytes = Transfer.BytesOnWire > HeaderBytes
		? Transfer.BytesOnWire - HeaderBytes : 0;
	const TCHAR* Hash = Result == EWoTShareTransferResult::BadHash ? TEXT("bad")
		: (Result == EWoTShareTransferResult::Sent && Transfer.bHashing)
			? TEXT("good") : TEXT("not-checked");
	Beat(TEXT("lan-share-send-end"), FString::Printf(
		TEXT("result=%s peer=%s offset=%llu bytes=%llu hash=%s"),
		LexToString(Result), Transfer.Peer,
		static_cast<unsigned long long>(Transfer.Offset),
		static_cast<unsigned long long>(FileBytes), Hash));
	if (Transfer.bAnnounced)
	{
		FWoTInstallerSharerEvent Event;
		Event.Kind = FWoTInstallerSharerEvent::EKind::SendEnded;
		Event.Label = Transfer.Label;
		Event.Result = Result;
		Emit(Event);
	}
}

void FWoTInstallerSharer::Shutdown()
{
	if (bShutdown)
	{
		return;
	}
	bShutdown = true;
	for (const TUniquePtr<FTransfer>& Transfer : Transfers)
	{
		End(*Transfer, EWoTShareTransferResult::Stopped);
	}
	Transfers.Reset();
	if (Datagram.IsValid())
	{
		Datagram->Close();
		Datagram.Reset();
	}
}

void FWoTInstallerSharer::Emit(const FWoTInstallerSharerEvent& Event) const
{
	if (Deps.OnEvent)
	{
		Deps.OnEvent(Event);
	}
}

void FWoTInstallerSharer::Beat(const TCHAR* Event, const FString& Detail) const
{
	if (Deps.Heartbeat)
	{
		Deps.Heartbeat(Event, Detail);
		return;
	}
	(void)WoTRecordBootstrapHeartbeat(Event, EWoTBootstrapPhase::Ready, 0,
		{}, {}, Detail);
}

FWoTInstallerSharerThread::FWoTInstallerSharerThread(
	TUniquePtr<FWoTInstallerSharer> InCore)
	: Core(MoveTemp(InCore))
	, WakeEvent(FPlatformProcess::GetSynchEventFromPool(false))
{
}

FWoTInstallerSharerThread::~FWoTInstallerSharerThread()
{
	StopAndJoin();
	if (WakeEvent != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

bool FWoTInstallerSharerThread::Start()
{
	if (Thread != nullptr)
	{
		return true;
	}
	if (!Core.IsValid())
	{
		return false;
	}
	bStopRequested = false;
	Thread = FRunnableThread::Create(this, TEXT("WoTInstallerSharer"), 0,
		TPri_BelowNormal);
	return Thread != nullptr;
}

void FWoTInstallerSharerThread::StopAndJoin()
{
	Stop();
	if (Thread != nullptr)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (Core.IsValid())
	{
		Core->Shutdown();
	}
}

uint32 FWoTInstallerSharerThread::Run()
{
	// The search probes player-chosen paths (a remembered pick on a card
	// reader): fail those probes instead of showing a "No Disk" dialog.
	WoTFailCriticalErrorsOnThisThread();
	while (!bStopRequested.Load())
	{
		const double Wait = Core->Pump();
		if (Wait > 0.0 && !bStopRequested.Load() && WakeEvent != nullptr)
		{
			WakeEvent->Wait(static_cast<uint32>(Wait * 1000.0));
		}
	}
	Core->Shutdown();
	return 0;
}

void FWoTInstallerSharerThread::Stop()
{
	bStopRequested = true;
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
	}
}
