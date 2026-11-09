#include "WoTInstallerReceiver.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTBootstrapViewModel.h"

namespace
{
// Every name here is prefixed: WoT sources share unity translation units.
/** One socket read and one file write. */
constexpr int32 WoTReceiveChunkBytes = 256 * 1024;
/** The most one step streams to disk, so a cancel or a stop is seen soon. */
constexpr int32 WoTReceiveStepBudgetBytes = 8 * 1024 * 1024;
/** The most one step hashes of an adopted .part before its first REQUEST. */
constexpr uint64 WoTReceiveHashStepBytes = 32ull * 1024ull * 1024ull;
constexpr int32 WoTReceiveMaxDatagramsPerStep = 16;
/** Larger than any valid OFFER; a longer datagram is truncated and then fails
 * decoding. */
constexpr int32 WoTReceiveMaxDatagramBytes = 512;
constexpr double WoTReceivePlaceRetrySeconds = 1.0;
constexpr TCHAR WoTReceivePartSuffix[] = TEXT(".part");

bool WoTReceiveIsPartLeaf(const FString& Name)
{
	constexpr int32 SuffixChars =
		static_cast<int32>(UE_ARRAY_COUNT(WoTReceivePartSuffix)) - 1;
	return Name.Len() > SuffixChars
		&& Name.EndsWith(WoTReceivePartSuffix, ESearchCase::IgnoreCase);
}

FString WoTReceiveFormatBytes(uint64 Bytes)
{
	return FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Bytes));
}

class FWoTReceiverUeSocket final : public IWoTInstallerReceiveSocket
{
public:
	FWoTReceiverUeSocket(ISocketSubsystem& InSubsystem, FSocket* InSocket)
		: Subsystem(InSubsystem)
		, Socket(InSocket)
	{
	}

	virtual ~FWoTReceiverUeSocket() override
	{
		Close();
	}

	virtual bool Bind(uint32 Address, uint16 Port) override
	{
		return Socket != nullptr && Socket->Bind(*MakeAddress(Address, Port));
	}

	virtual uint16 LocalPort() const override
	{
		return Socket != nullptr ? static_cast<uint16>(Socket->GetPortNo()) : 0;
	}

	virtual bool Listen(int32 Backlog) override
	{
		// Non-blocking, so an Accept after a stale readiness never blocks.
		return Socket != nullptr && Socket->SetNonBlocking(true)
			&& Socket->Listen(Backlog);
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
		return Subsystem.GetLastErrorCode() == SE_EWOULDBLOCK
			? EWoTShareIo::WouldBlock : EWoTShareIo::Failed;
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
		OutBytes.SetNumUninitialized(WoTReceiveMaxDatagramBytes);
		const TSharedRef<FInternetAddr> From =
			Subsystem.CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		int32 Read = 0;
		if (!Socket->RecvFrom(OutBytes.GetData(), OutBytes.Num(), Read, *From)
			|| Read < 0)
		{
			OutBytes.Reset();
			return Subsystem.GetLastErrorCode() == SE_EWOULDBLOCK
				? EWoTShareIo::WouldBlock : EWoTShareIo::Failed;
		}
		OutBytes.SetNum(Read, EAllowShrinking::No);
		From->GetIp(OutAddress);
		OutPort = static_cast<uint16>(From->GetPort());
		return EWoTShareIo::Done;
	}

	virtual TUniquePtr<IWoTInstallerReceiveSocket> Accept(
		uint32& OutPeerAddress, double WaitSeconds) override
	{
		OutPeerAddress = 0;
		if (Socket == nullptr
			|| !Socket->Wait(ESocketWaitConditions::WaitForRead,
				FTimespan::FromSeconds(FMath::Max(0.0, WaitSeconds))))
		{
			return nullptr;
		}
		const TSharedRef<FInternetAddr> Peer =
			Subsystem.CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		FSocket* Accepted = Socket->Accept(*Peer, TEXT("WoTInstallerReceiveData"));
		if (Accepted == nullptr)
		{
			return nullptr;
		}
		TUniquePtr<FWoTReceiverUeSocket> Wrapped =
			MakeUnique<FWoTReceiverUeSocket>(Subsystem, Accepted);
		Peer->GetIp(OutPeerAddress);
		Accepted->SetNonBlocking(true);
		return Wrapped;
	}

	virtual EWoTShareIo Recv(TArrayView<uint8> Out, int32& OutRead,
		double WaitSeconds) override
	{
		OutRead = 0;
		if (Socket == nullptr)
		{
			return EWoTShareIo::Failed;
		}
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead,
				FTimespan::FromSeconds(FMath::Max(0.0, WaitSeconds))))
		{
			return EWoTShareIo::WouldBlock;
		}
		int32 Read = 0;
		// A stream Recv is false only for an orderly close or an error; a
		// would-block is true with nothing read.
		if (!Socket->Recv(Out.GetData(), Out.Num(), Read))
		{
			return EWoTShareIo::Failed;
		}
		if (Read <= 0)
		{
			return EWoTShareIo::WouldBlock;
		}
		OutRead = Read;
		return EWoTShareIo::Done;
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

	ISocketSubsystem& Subsystem;
	FSocket* Socket = nullptr;
};

class FWoTReceiverUeNetwork final : public IWoTInstallerReceiveNetwork
{
public:
	virtual TUniquePtr<IWoTInstallerReceiveSocket> CreateDatagram() override
	{
		return Create(NAME_DGram, TEXT("WoTInstallerReceiveOffer"));
	}

	virtual TUniquePtr<IWoTInstallerReceiveSocket> CreateStream() override
	{
		return Create(NAME_Stream, TEXT("WoTInstallerReceiveListen"));
	}

private:
	static TUniquePtr<IWoTInstallerReceiveSocket> Create(
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
		return MakeUnique<FWoTReceiverUeSocket>(*Subsystem, Socket);
	}
};
} // namespace

TSharedRef<IWoTInstallerReceiveNetwork> WoTCreateProductionInstallerReceiveNetwork()
{
	return MakeShared<FWoTReceiverUeNetwork>();
}

const TCHAR* LexToString(EWoTInstallerReceiveState State)
{
	switch (State)
	{
	case EWoTInstallerReceiveState::Starting: return TEXT("starting");
	case EWoTInstallerReceiveState::Listening: return TEXT("listening");
	case EWoTInstallerReceiveState::Copying: return TEXT("copying");
	case EWoTInstallerReceiveState::Placed: return TEXT("placed");
	case EWoTInstallerReceiveState::GaveUp: return TEXT("gave-up");
	case EWoTInstallerReceiveState::Unavailable: return TEXT("unavailable");
	default: return TEXT("unknown");
	}
}

FWoTInstallerReceiver::FWoTInstallerReceiver(
	FConfig InConfig, FDependencies InDependencies)
	: Config(MoveTemp(InConfig))
	, Deps(MoveTemp(InDependencies))
{
	if (!Deps.NowSeconds)
	{
		Deps.NowSeconds = []()
		{
			return FPlatformTime::Seconds();
		};
	}
	Published.TotalBytes = Config.Size;
}

FWoTInstallerReceiver::~FWoTInstallerReceiver()
{
	Shutdown();
}

FWoTInstallerReceiveStatus FWoTInstallerReceiver::Status() const
{
	FScopeLock Lock(&StatusLock);
	return Published;
}

void FWoTInstallerReceiver::Publish(
	TFunctionRef<void(FWoTInstallerReceiveStatus&)> Change)
{
	FScopeLock Lock(&StatusLock);
	Change(Published);
}

void FWoTInstallerReceiver::RequestCancelCopy()
{
	bCancelRequested.Store(true);
}

double FWoTInstallerReceiver::Pump()
{
	if (bShutdown || Step == EStep::Done)
	{
		return 0.25;
	}
	const double Now = Deps.NowSeconds();
	if (!bFolderOpen && !OpenFolder())
	{
		Beat(TEXT("lan-receive-listen"), TEXT("result=folder-unavailable"));
		Finish(EWoTInstallerReceiveState::Unavailable);
		return 0.25;
	}
	if (!Datagram.IsValid())
	{
		if (Now < NextBindSeconds)
		{
			return FMath::Clamp(NextBindSeconds - Now, 0.0, 0.25);
		}
		if (!StartListening(Now))
		{
			NextBindSeconds = Now + Config.BindRetrySeconds;
			return 0.25;
		}
	}
	if (bCancelRequested.Exchange(false))
	{
		CancelCopy(Now);
	}
	switch (Step)
	{
	case EStep::Waiting:
		if (HashedBytes < PartBytes)
		{
			// An adopted .part from an earlier run: hash its prefix (in steps)
			// before any REQUEST resumes it.
			CatchUpHash();
		}
		else if (Config.Size > 0 && PartBytes == Config.Size)
		{
			Verify(Now);
		}
		else
		{
			ReadOffers(Now);
		}
		return 0.0;
	case EStep::Paired:
		DrainOffers();
		Pair(Now);
		return 0.0;
	case EStep::Copying:
		DrainOffers();
		Copy(Now);
		return 0.0;
	case EStep::Placing:
		if (Now >= NextPlaceSeconds)
		{
			Place(Now);
		}
		return Step == EStep::Done ? 0.25 : 0.1;
	case EStep::Done:
	default:
		return 0.25;
	}
}

bool FWoTInstallerReceiver::OpenFolder()
{
	if (Config.Directory.IsEmpty() || Config.PinnedLeaf.IsEmpty()
		|| Config.Size == 0)
	{
		return false;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened =
		OpenNativeDirectoryNoFollow(Config.Directory);
	if (Opened.IsError()
		|| !Opened.GetValue().GetIdentity().IsSameObject(Config.DirectoryIdentity))
	{
		return false;
	}
	Folder = MoveTemp(Opened).GetValue();
	bFolderOpen = true;
	// A .part an earlier run left behind is resumed (the largest one that is
	// not longer than the installer); its prefix is hashed before any REQUEST.
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
		EnumerateNativeDirectory(Folder);
	if (!Entries.IsError())
	{
		FString BestLeaf;
		uint64 BestBytes = 0;
		for (const FWoTNativeDirectoryEntry& Entry : Entries.GetValue())
		{
			if (Entry.Type != EWoTTreeEntryType::File
				|| !WoTReceiveIsPartLeaf(Entry.Name))
			{
				continue;
			}
			TWoTBootstrapResult<FWoTNativeFileHandle> Probe =
				OpenNativeFileAtReadNoFollow(Folder, Entry.Name);
			if (Probe.IsError())
			{
				continue;
			}
			const FWoTNativeFileIdentity& Identity = Probe.GetValue().GetIdentity();
			if (Identity.Type == EWoTTreeEntryType::File && Identity.LinkCount == 1
				&& Identity.Size > BestBytes && Identity.Size <= Config.Size)
			{
				BestLeaf = Entry.Name;
				BestBytes = Identity.Size;
			}
		}
		if (!BestLeaf.IsEmpty())
		{
			PartLeaf = BestLeaf;
			if (EnsurePart())
			{
				PartBytes = FMath::Min(Part.GetIdentity().Size, Config.Size);
				HashedBytes = 0;
				Hasher = FWoTSha256Hasher();
				const uint64 Adopted = PartBytes;
				Publish([Adopted](FWoTInstallerReceiveStatus& Out)
				{
					Out.CompletedBytes = Adopted;
				});
			}
			else
			{
				PartLeaf.Reset();
			}
		}
	}
	return true;
}

bool FWoTInstallerReceiver::StartListening(double Now)
{
	if (!Deps.Network.IsValid())
	{
		return false;
	}
	TUniquePtr<IWoTInstallerReceiveSocket> Socket = Deps.Network->CreateDatagram();
	if (!Socket.IsValid()
		|| !Socket->Bind(Config.BindAddress, Config.DiscoveryPort))
	{
		if (Socket.IsValid())
		{
			Socket->Close();
		}
		BeatLimited(Now, TEXT("lan-receive-listen"), TEXT("bind"),
			FString::Printf(TEXT("result=bind-failed port=%u"),
				static_cast<uint32>(Config.DiscoveryPort)));
		return false;
	}
	const uint16 Bound = Socket->LocalPort();
	Datagram = MoveTemp(Socket);
	Beat(TEXT("lan-receive-listen"), FString::Printf(TEXT("result=ok port=%u"),
		static_cast<uint32>(Bound)));
	Publish([Bound](FWoTInstallerReceiveStatus& Out)
	{
		Out.DiscoveryPort = Bound;
		if (Out.State == EWoTInstallerReceiveState::Starting)
		{
			Out.State = EWoTInstallerReceiveState::Listening;
		}
	});
	return true;
}

void FWoTInstallerReceiver::ReadOffers(double Now)
{
	TArray<uint8> Bytes;
	for (int32 Count = 0; Count < WoTReceiveMaxDatagramsPerStep
		&& Datagram.IsValid() && Step == EStep::Waiting; ++Count)
	{
		uint32 From = 0;
		uint16 FromPort = 0;
		if (Datagram->RecvFrom(Bytes, From, FromPort,
				Count == 0 ? Config.SocketWaitSeconds : 0.0) != EWoTShareIo::Done)
		{
			break;
		}
		if (const TOptional<FWoTShareOffer> Offer =
				WoTInstallerShare::DecodeOffer(Bytes))
		{
			HandleOffer(Now, Offer.GetValue(), From, FromPort);
		}
	}
	for (auto It = IgnoredUntil.CreateIterator(); It; ++It)
	{
		if (Now >= It.Value())
		{
			It.RemoveCurrent();
		}
	}
}

void FWoTInstallerReceiver::DrainOffers()
{
	// Offers keep arriving every 2 s while a copy runs; read and drop them so
	// the socket's buffer never fills.
	TArray<uint8> Bytes;
	for (int32 Count = 0; Count < WoTReceiveMaxDatagramsPerStep
		&& Datagram.IsValid(); ++Count)
	{
		uint32 From = 0;
		uint16 FromPort = 0;
		if (Datagram->RecvFrom(Bytes, From, FromPort, 0.0) != EWoTShareIo::Done)
		{
			break;
		}
	}
}

void FWoTInstallerReceiver::HandleOffer(double Now, const FWoTShareOffer& Offer,
	uint32 From, uint16 FromPort)
{
	// Nothing outside the local network is ever answered, and only a sharer
	// offering this build's exact pin pairs.
	if (!WoTInstallerShare::IsPrivatePeerAddress(From, Config.bAllowLoopbackPeers)
		|| FromPort == 0 || Offer.Size != Config.Size
		|| Offer.Sha256 != Config.Sha256
		|| DamagedSessions.Contains(Offer.SessionId))
	{
		return;
	}
	if (const double* Until = IgnoredUntil.Find(Offer.SessionId);
		Until != nullptr && Now < *Until)
	{
		return;
	}
	// Free space first: no REQUEST is sent without room for the rest of the
	// file plus a margin.
	const uint64 Remaining = Config.Size - FMath::Min(PartBytes, Config.Size);
	const uint64 Needed = Remaining + Config.FreeSpaceMarginBytes;
	const TOptional<uint64> Free = QueryFreeBytes();
	if (Free.IsSet() && Free.GetValue() < Needed)
	{
		Publish([Needed](FWoTInstallerReceiveStatus& Out)
		{
			Out.NeededBytes = Needed;
		});
		BeatLimited(Now, TEXT("lan-receive-failed"), TEXT("space"),
			FString::Printf(TEXT("reason=space needed=%s free=%s"),
				*WoTReceiveFormatBytes(Needed),
				*WoTReceiveFormatBytes(Free.GetValue())));
		return;
	}
	Publish([](FWoTInstallerReceiveStatus& Out)
	{
		Out.NeededBytes.Reset();
	});
	if (!EnsurePart())
	{
		BeatLimited(Now, TEXT("lan-receive-failed"), TEXT("part"),
			TEXT("reason=part-open"));
		return;
	}
	// One listener per pairing, on a port the OS picks (always >= 1024, which
	// SteamOS's firewall allows).
	TUniquePtr<IWoTInstallerReceiveSocket> NewListener =
		Deps.Network.IsValid() ? Deps.Network->CreateStream() : nullptr;
	if (!NewListener.IsValid() || !NewListener->Bind(Config.BindAddress, 0)
		|| !NewListener->Listen(1))
	{
		if (NewListener.IsValid())
		{
			NewListener->Close();
		}
		BeatLimited(Now, TEXT("lan-receive-failed"), TEXT("listen"),
			TEXT("reason=listen"));
		return;
	}
	const uint16 TcpPort = NewListener->LocalPort();
	if (TcpPort < WoTInstallerShare::MinimumTcpPort)
	{
		NewListener->Close();
		return;
	}
	FWoTShareRequest Request;
	Request.SessionId = Offer.SessionId;
	Request.Token = FWoTShareId::Random();
	Request.TcpPort = TcpPort;
	Request.ResumeOffset = PartBytes;
	Request.Label = Config.Label;
	const TArray<uint8> Encoded = WoTInstallerShare::EncodeRequest(Request);
	if (Encoded.IsEmpty() || !Datagram.IsValid()
		|| Datagram->SendTo(From, FromPort, Encoded) != EWoTShareIo::Done)
	{
		NewListener->Close();
		return;
	}
	Listener = MoveTemp(NewListener);
	PairedSession = Offer.SessionId;
	bPairedSessionSet = true;
	Token = Request.Token;
	PairedAddress = From;
	PairedLabel = WoTInstallerShare::DisplayLabel(Offer.Label);
	PairDeadline = Now + Config.ConnectTimeoutSeconds;
	Header.Reset();
	Step = EStep::Paired;
}

void FWoTInstallerReceiver::Pair(double Now)
{
	if (!Connection.IsValid() && Listener.IsValid())
	{
		uint32 Peer = 0;
		TUniquePtr<IWoTInstallerReceiveSocket> Accepted =
			Listener->Accept(Peer, Config.SocketWaitSeconds);
		if (Accepted.IsValid())
		{
			if (Peer != PairedAddress)
			{
				// Only the paired sharer may connect: anything else is closed
				// without reading a byte.
				Accepted->Close();
				BeatLimited(Now, TEXT("lan-receive-refused"), TEXT("address"),
					TEXT("reason=address"));
			}
			else
			{
				Connection = MoveTemp(Accepted);
				Header.Reset();
			}
		}
	}
	if (Connection.IsValid())
	{
		// Exactly the DATA header, never a byte more: the payload is read only
		// once the token is known to be right.
		uint8 HeaderBytes[WoTInstallerShare::DataHeaderBytes];
		const int32 Want = WoTInstallerShare::DataHeaderBytes - Header.Num();
		int32 Read = 0;
		const EWoTShareIo Io = Connection->Recv(
			TArrayView<uint8>(HeaderBytes, Want), Read, Config.SocketWaitSeconds);
		if (Io == EWoTShareIo::Failed)
		{
			Connection->Close();
			Connection.Reset();
			Header.Reset();
		}
		else if (Io == EWoTShareIo::Done)
		{
			Header.Append(HeaderBytes, Read);
		}
		if (Connection.IsValid()
			&& Header.Num() == WoTInstallerShare::DataHeaderBytes)
		{
			const TOptional<FWoTShareDataHeader> Data =
				WoTInstallerShare::DecodeDataHeader(Header);
			if (!Data.IsSet() || Data->Token != Token)
			{
				Connection->Close();
				Connection.Reset();
				Header.Reset();
				BeatLimited(Now, TEXT("lan-receive-refused"), TEXT("token"),
					TEXT("reason=token"));
			}
			else if (Data->Offset != PartBytes
				|| Data->Length != Config.Size - PartBytes)
			{
				FailAttempt(Now, TEXT("range"));
				return;
			}
			else
			{
				// Paired: no other connection is accepted for this copy.
				if (Listener.IsValid())
				{
					Listener->Close();
					Listener.Reset();
				}
				Step = EStep::Copying;
				LastBytesSeconds = Now;
				const uint64 Offset = PartBytes;
				int32 AttemptNumber = 0;
				Publish([this, &AttemptNumber](FWoTInstallerReceiveStatus& Out)
				{
					Out.State = EWoTInstallerReceiveState::Copying;
					Out.Attempt += 1;
					AttemptNumber = Out.Attempt;
					Out.PeerLabel = PairedLabel;
					Out.bLastCopyDamaged = false;
					Out.CompletedBytes = PartBytes;
				});
				BeatLimited(Now, TEXT("lan-receive-start"), TEXT("start"),
					FString::Printf(TEXT("peer=%s offset=%s attempt=%d"),
						WoTInstallerShare::PeerToken(PairedLabel),
						*WoTReceiveFormatBytes(Offset), AttemptNumber));
				return;
			}
		}
	}
	if (Step == EStep::Paired && Now >= PairDeadline)
	{
		FailAttempt(Now, TEXT("connect"));
	}
}

void FWoTInstallerReceiver::Copy(double Now)
{
	if (!Connection.IsValid())
	{
		FailAttempt(Now, TEXT("truncated"));
		return;
	}
	if (Buffer.Num() != WoTReceiveChunkBytes)
	{
		Buffer.SetNumUninitialized(WoTReceiveChunkBytes);
	}
	int32 Budget = WoTReceiveStepBudgetBytes;
	bool bFirst = true;
	while (Budget > 0 && PartBytes < Config.Size)
	{
		// Never more than the file: the receiver reads exactly what the sharer
		// sends and nothing past it.
		const int32 Want = static_cast<int32>(FMath::Min<uint64>(
			static_cast<uint64>(Buffer.Num()), Config.Size - PartBytes));
		int32 Read = 0;
		const EWoTShareIo Io = Connection->Recv(
			TArrayView<uint8>(Buffer.GetData(), Want), Read,
			bFirst ? Config.SocketWaitSeconds : 0.0);
		bFirst = false;
		if (Io == EWoTShareIo::WouldBlock)
		{
			break;
		}
		if (Io == EWoTShareIo::Failed || Read <= 0 || Read > Want)
		{
			// The sharer stopped early: keep the .part for a resume.
			FailAttempt(Now, TEXT("truncated"));
			return;
		}
		const TConstArrayView<uint8> Chunk(Buffer.GetData(), Read);
		if (WriteNativeFileAt(Part, PartBytes, Chunk).IsError())
		{
			// Drop whatever part of the chunk may have landed; the hash still
			// covers exactly PartBytes.
			(void)TruncateNativeFile(Part, PartBytes);
			FailAttempt(Now, TEXT("write"));
			return;
		}
		Hasher.Update(Chunk);
		PartBytes += static_cast<uint64>(Read);
		HashedBytes = PartBytes;
		Budget -= Read;
		LastBytesSeconds = Deps.NowSeconds();
		const uint64 Completed = PartBytes;
		Publish([Completed](FWoTInstallerReceiveStatus& Out)
		{
			Out.CompletedBytes = Completed;
		});
	}
	if (PartBytes >= Config.Size)
	{
		// Every byte the sharer sends has been read, so this close is a clean
		// one on both ends.
		CloseTransfer();
		Verify(Now);
		return;
	}
	if (Deps.NowSeconds() - LastBytesSeconds >= Config.IdleTimeoutSeconds)
	{
		FailAttempt(Now, TEXT("idle"));
	}
}

void FWoTInstallerReceiver::CatchUpHash()
{
	if (Buffer.Num() != WoTReceiveChunkBytes)
	{
		Buffer.SetNumUninitialized(WoTReceiveChunkBytes);
	}
	uint64 Budget = WoTReceiveHashStepBytes;
	while (Budget > 0 && HashedBytes < PartBytes)
	{
		const int32 Want = static_cast<int32>(FMath::Min<uint64>(
			static_cast<uint64>(Buffer.Num()), PartBytes - HashedBytes));
		const TWoTBootstrapResult<uint64> Read = ReadNativeFileAt(Part,
			HashedBytes, TArrayView<uint8>(Buffer.GetData(), Want));
		if (Read.IsError() || Read.GetValue() == 0
			|| Read.GetValue() > static_cast<uint64>(Want))
		{
			// An unreadable leftover is not worth resuming.
			DiscardPart();
			return;
		}
		Hasher.Update(TConstArrayView<uint8>(Buffer.GetData(),
			static_cast<int32>(Read.GetValue())));
		HashedBytes += Read.GetValue();
		Budget -= FMath::Min(Budget, Read.GetValue());
	}
}

void FWoTInstallerReceiver::Verify(double Now)
{
	const FWoTSha256 Digest = Hasher.Finish();
	Hasher = FWoTSha256Hasher();
	HashedBytes = 0;
	if (PartBytes != Config.Size || Digest != Config.Sha256)
	{
		// Damaged: delete it, or every resume would reuse the bad bytes, and
		// never pair with that sharer's session again.
		DiscardPart();
		if (!bPairedSessionSet)
		{
			// A complete leftover from an earlier run: no sharer to blame and
			// no strike, just copy it again.
			Beat(TEXT("lan-receive-failed"), TEXT("reason=leftover-damaged"));
			Step = EStep::Waiting;
			return;
		}
		++DamagedCount;
		DamagedSessions.Add(PairedSession);
		const int32 Count = DamagedCount;
		Publish([Count](FWoTInstallerReceiveStatus& Out)
		{
			Out.DamagedCopies = Count;
			Out.bLastCopyDamaged = true;
			Out.CompletedBytes = 0;
			Out.State = EWoTInstallerReceiveState::Listening;
		});
		Beat(TEXT("lan-receive-failed"), FString::Printf(
			TEXT("reason=damaged peer=%s count=%d"),
			WoTInstallerShare::PeerToken(PairedLabel), Count));
		bPairedSessionSet = false;
		if (DamagedCount >= Config.MaxDamagedCopies)
		{
			// Copies from this network keep arriving damaged: stop listening
			// until the player acts again.
			Finish(EWoTInstallerReceiveState::GaveUp);
			return;
		}
		Step = EStep::Waiting;
		return;
	}
	if (FlushNativeFile(Part).IsError())
	{
		BeatLimited(Now, TEXT("lan-receive-failed"), TEXT("flush"),
			TEXT("reason=flush"));
	}
	Part = FWoTNativeFileHandle();
	Step = EStep::Placing;
	NextPlaceSeconds = Now;
	Place(Now);
}

void FWoTInstallerReceiver::Place(double Now)
{
	// Onto the pinned leaf without replacing anything, then the folder is
	// flushed (the remembered-installer record's pattern).
	FWoTBootstrapStatus Renamed = RenameNativeEntryAtNoFollow(
		Folder, PartLeaf, Folder, Config.PinnedLeaf, /*bReplaceExisting*/ false);
	if (Renamed.IsError())
	{
		NextPlaceSeconds = Now + WoTReceivePlaceRetrySeconds;
		BeatLimited(Now, TEXT("lan-receive-failed"), TEXT("place"),
			TEXT("reason=place"));
		return;
	}
	(void)FlushNativeDirectory(Folder);
	PartLeaf.Reset();
	PartBytes = 0;
	DeleteLeftoverParts();
	const uint64 Total = Config.Size;
	Publish([Total](FWoTInstallerReceiveStatus& Out)
	{
		Out.CompletedBytes = Total;
	});
	Beat(TEXT("lan-receive-verified"), FString::Printf(TEXT("peer=%s bytes=%s"),
		WoTInstallerShare::PeerToken(PairedLabel), *WoTReceiveFormatBytes(Total)));
	Finish(EWoTInstallerReceiveState::Placed);
	if (Deps.OnPlaced)
	{
		Deps.OnPlaced();
	}
}

void FWoTInstallerReceiver::FailAttempt(double Now, const TCHAR* Reason)
{
	CloseTransfer();
	Step = EStep::Waiting;
	const uint64 Kept = PartBytes;
	Publish([Kept](FWoTInstallerReceiveStatus& Out)
	{
		if (Out.State == EWoTInstallerReceiveState::Copying)
		{
			Out.State = EWoTInstallerReceiveState::Listening;
		}
		Out.CompletedBytes = Kept;
	});
	BeatLimited(Now, TEXT("lan-receive-failed"), Reason, FString::Printf(
		TEXT("reason=%s peer=%s kept=%s"), Reason,
		WoTInstallerShare::PeerToken(PairedLabel), *WoTReceiveFormatBytes(Kept)));
	bPairedSessionSet = false;
}

void FWoTInstallerReceiver::CancelCopy(double Now)
{
	if (Step != EStep::Paired && Step != EStep::Copying)
	{
		return;
	}
	if (bPairedSessionSet)
	{
		IgnoredUntil.Add(PairedSession, Now + Config.CancelIgnoreSeconds);
	}
	FailAttempt(Now, TEXT("cancelled"));
}

bool FWoTInstallerReceiver::EnsurePart()
{
	if (Part.IsValid())
	{
		return true;
	}
	if (!bFolderOpen)
	{
		return false;
	}
	const bool bFresh = PartLeaf.IsEmpty();
	if (bFresh)
	{
		PartLeaf = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()
			+ WoTReceivePartSuffix;
	}
	bool bCreated = false;
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
		OpenOrCreateNativeFileAtReadWriteNoFollow(Folder, PartLeaf, bCreated);
	if (Opened.IsError())
	{
		if (bFresh)
		{
			PartLeaf.Reset();
		}
		return false;
	}
	Part = MoveTemp(Opened).GetValue();
	if (bCreated)
	{
		PartBytes = 0;
		HashedBytes = 0;
		Hasher = FWoTSha256Hasher();
	}
	return true;
}

void FWoTInstallerReceiver::DiscardPart()
{
	Part = FWoTNativeFileHandle();
	if (bFolderOpen && !PartLeaf.IsEmpty())
	{
		(void)RemoveNativeEntryAtNoFollow(Folder, PartLeaf, EWoTTreeEntryType::File);
	}
	PartLeaf.Reset();
	PartBytes = 0;
	HashedBytes = 0;
	Hasher = FWoTSha256Hasher();
}

void FWoTInstallerReceiver::DeleteLeftoverParts()
{
	// Once an installer is accepted, no partial copy is worth keeping.
	TWoTBootstrapResult<TArray<FWoTNativeDirectoryEntry>> Entries =
		EnumerateNativeDirectory(Folder);
	if (Entries.IsError())
	{
		return;
	}
	for (const FWoTNativeDirectoryEntry& Entry : Entries.GetValue())
	{
		if (Entry.Type == EWoTTreeEntryType::File && WoTReceiveIsPartLeaf(Entry.Name))
		{
			(void)RemoveNativeEntryAtNoFollow(Folder, Entry.Name,
				EWoTTreeEntryType::File);
		}
	}
}

void FWoTInstallerReceiver::CloseTransfer()
{
	if (Connection.IsValid())
	{
		Connection->Close();
		Connection.Reset();
	}
	if (Listener.IsValid())
	{
		Listener->Close();
		Listener.Reset();
	}
	Header.Reset();
}

void FWoTInstallerReceiver::CloseAll()
{
	CloseTransfer();
	if (Datagram.IsValid())
	{
		Datagram->Close();
		Datagram.Reset();
	}
}

void FWoTInstallerReceiver::Finish(EWoTInstallerReceiveState FinalState)
{
	CloseAll();
	Step = EStep::Done;
	Publish([FinalState](FWoTInstallerReceiveStatus& Out)
	{
		Out.State = FinalState;
		Out.DiscoveryPort = 0;
	});
}

void FWoTInstallerReceiver::Shutdown()
{
	if (bShutdown)
	{
		return;
	}
	bShutdown = true;
	CloseAll();
	// The .part stays on disk for a resume; only the handle goes.
	Part = FWoTNativeFileHandle();
	Folder = FWoTNativeDirectoryHandle();
	bFolderOpen = false;
}

TOptional<uint64> FWoTInstallerReceiver::QueryFreeBytes() const
{
	if (Deps.FreeBytes)
	{
		return Deps.FreeBytes();
	}
	if (!bFolderOpen)
	{
		return {};
	}
	const TWoTBootstrapResult<uint64> Available =
		QueryNativeDirectoryAvailableBytes(Folder);
	return Available.IsError() ? TOptional<uint64>()
		: TOptional<uint64>(Available.GetValue());
}

void FWoTInstallerReceiver::Beat(const TCHAR* Event, const FString& Detail) const
{
	if (Deps.Heartbeat)
	{
		Deps.Heartbeat(Event, Detail);
		return;
	}
	(void)WoTRecordBootstrapHeartbeat(Event, EWoTBootstrapPhase::OriginalGameNeeded,
		0, {}, {}, Detail);
}

void FWoTInstallerReceiver::BeatLimited(double Now, const TCHAR* Event,
	const TCHAR* Key, const FString& Detail)
{
	const FString LimitKey = FString(Event) + TEXT("|") + Key;
	if (const double* Last = LastLimitedBeat.Find(LimitKey);
		Last != nullptr && Now - *Last < Config.FailureHeartbeatIntervalSeconds)
	{
		return;
	}
	LastLimitedBeat.Add(LimitKey, Now);
	Beat(Event, Detail);
}

FWoTInstallerReceiverThread::FWoTInstallerReceiverThread(
	TUniquePtr<FWoTInstallerReceiver> InCore)
	: Core(MoveTemp(InCore))
	, WakeEvent(FPlatformProcess::GetSynchEventFromPool(false))
{
}

FWoTInstallerReceiverThread::~FWoTInstallerReceiverThread()
{
	StopAndJoin();
	if (WakeEvent != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

bool FWoTInstallerReceiverThread::Start()
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
	Thread = FRunnableThread::Create(this, TEXT("WoTInstallerReceiver"), 0,
		TPri_BelowNormal);
	return Thread != nullptr;
}

void FWoTInstallerReceiverThread::StopAndJoin()
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

FWoTInstallerReceiveStatus FWoTInstallerReceiverThread::Status() const
{
	return Core.IsValid() ? Core->Status() : FWoTInstallerReceiveStatus();
}

void FWoTInstallerReceiverThread::RequestCancelCopy()
{
	if (Core.IsValid())
	{
		Core->RequestCancelCopy();
	}
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
	}
}

uint32 FWoTInstallerReceiverThread::Run()
{
	// The receiver writes into the player's data folder: fail an I/O probe of
	// a vanished drive instead of showing a "No Disk" dialog.
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

void FWoTInstallerReceiverThread::Stop()
{
	bStopRequested = true;
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
	}
}
