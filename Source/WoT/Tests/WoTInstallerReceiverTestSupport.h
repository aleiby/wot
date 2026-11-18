// LAN installer receiver specs: a recording wrapper over the production
// receive sockets, and a loopback "spec sharer" built from raw sockets that can
// misbehave on purpose (wrong bytes, wrong token, a short stream). Nothing here
// sends beyond 127.0.0.1. Shared by the receiver and worker specs, so every
// name lives in its own namespace and every function is inline.
#pragma once

#if WITH_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Templates/Atomic.h"
#include "../Online/WoTInstallerShareProtocol.h"
#include "../Updater/WoTInstallerReceiver.h"
#include "WoTBootstrapCore/Hash.h"

namespace WoTReceiverSpec
{
/** A host-order IPv4 address from its octets: specs never spell a private
 * address as a dotted quad. */
constexpr uint32 Ip(uint32 A, uint32 B, uint32 C, uint32 D)
{
	return (A << 24) | (B << 16) | (C << 8) | D;
}

inline constexpr uint32 Loopback = Ip(127, 0, 0, 1);

inline FWoTShareId Id(uint8 Seed)
{
	FWoTShareId Result;
	for (int32 Index = 0; Index < FWoTShareId::ByteCount; ++Index)
	{
		Result.Bytes[Index] = static_cast<uint8>(Seed + Index * 7);
	}
	return Result;
}

inline TArray<uint8> Payload(int32 Size, uint8 Salt = 0)
{
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Size);
	for (int32 Index = 0; Index < Size; ++Index)
	{
		Bytes[Index] = static_cast<uint8>((Index * 13 + Index / 509 + Salt) & 0xFF);
	}
	return Bytes;
}

/** Everything the recording sockets saw. Counters are atomic: worker specs
 * read them while the receiver's own thread runs. */
struct FLog
{
	TAtomic<int32> DatagramsCreated{0};
	TAtomic<int32> StreamsCreated{0};
	TAtomic<int32> Binds{0};
	TAtomic<int32> Listens{0};
	TAtomic<int32> Accepted{0};
	TAtomic<int32> Open{0};
	/** REQUESTs: SendTo on a datagram socket. */
	TAtomic<int32> RequestsSent{0};
	/** Anything sent on an accepted data connection. Must stay 0. */
	TAtomic<int32> ConnectionSends{0};
	/** Recv calls on connections whose peer address was faked. */
	TAtomic<int32> FakedPeerRecvs{0};
	TAtomic<int32> FakedPeerCloses{0};
	/** The last bound datagram port. */
	TAtomic<int32> DiscoveryPort{0};
	/** The next accepted connections report this peer address instead. */
	TAtomic<uint32> FakePeerAddress{0};
	TAtomic<int32> FakePeerAccepts{0};
};

class FRecordingSocket final : public IWoTInstallerReceiveSocket
{
public:
	enum class ERole : uint8
	{
		Datagram,
		Listener,
		Connection,
	};

	FRecordingSocket(TSharedRef<FLog> InLog,
		TUniquePtr<IWoTInstallerReceiveSocket> InInner, ERole InRole,
		bool bInFakedPeer = false)
		: Log(MoveTemp(InLog))
		, Inner(MoveTemp(InInner))
		, Role(InRole)
		, bFakedPeer(bInFakedPeer)
	{
		++Log->Open;
	}

	virtual ~FRecordingSocket() override
	{
		Close();
	}

	virtual bool Bind(uint32 Address, uint16 Port) override
	{
		++Log->Binds;
		const bool bBound = Inner.IsValid() && Inner->Bind(Address, Port);
		if (bBound && Role == ERole::Datagram)
		{
			Log->DiscoveryPort.Store(Inner->LocalPort());
		}
		return bBound;
	}

	virtual uint16 LocalPort() const override
	{
		return Inner.IsValid() ? Inner->LocalPort() : 0;
	}

	virtual bool Listen(int32 Backlog) override
	{
		++Log->Listens;
		return Inner.IsValid() && Inner->Listen(Backlog);
	}

	virtual EWoTShareIo SendTo(uint32 Address, uint16 Port,
		TConstArrayView<uint8> Bytes) override
	{
		if (Role == ERole::Connection)
		{
			++Log->ConnectionSends;
		}
		else
		{
			++Log->RequestsSent;
		}
		return Inner.IsValid() ? Inner->SendTo(Address, Port, Bytes)
			: EWoTShareIo::Failed;
	}

	virtual EWoTShareIo RecvFrom(TArray<uint8>& OutBytes, uint32& OutAddress,
		uint16& OutPort, double WaitSeconds) override
	{
		return Inner.IsValid()
			? Inner->RecvFrom(OutBytes, OutAddress, OutPort, WaitSeconds)
			: EWoTShareIo::Failed;
	}

	virtual TUniquePtr<IWoTInstallerReceiveSocket> Accept(
		uint32& OutPeerAddress, double WaitSeconds) override
	{
		if (!Inner.IsValid())
		{
			return nullptr;
		}
		TUniquePtr<IWoTInstallerReceiveSocket> Connection =
			Inner->Accept(OutPeerAddress, WaitSeconds);
		if (!Connection.IsValid())
		{
			return nullptr;
		}
		++Log->Accepted;
		bool bFaked = false;
		if (Log->FakePeerAccepts.Load() > 0)
		{
			--Log->FakePeerAccepts;
			OutPeerAddress = Log->FakePeerAddress.Load();
			bFaked = true;
		}
		return MakeUnique<FRecordingSocket>(Log, MoveTemp(Connection),
			ERole::Connection, bFaked);
	}

	virtual EWoTShareIo Recv(TArrayView<uint8> Out, int32& OutRead,
		double WaitSeconds) override
	{
		if (bFakedPeer)
		{
			++Log->FakedPeerRecvs;
		}
		return Inner.IsValid() ? Inner->Recv(Out, OutRead, WaitSeconds)
			: EWoTShareIo::Failed;
	}

	virtual void Close() override
	{
		if (Inner.IsValid())
		{
			Inner->Close();
			Inner.Reset();
			--Log->Open;
			if (bFakedPeer)
			{
				++Log->FakedPeerCloses;
			}
		}
	}

private:
	TSharedRef<FLog> Log;
	TUniquePtr<IWoTInstallerReceiveSocket> Inner;
	ERole Role = ERole::Datagram;
	bool bFakedPeer = false;
};

/** The production receive network, recorded. */
class FRecordingNetwork final : public IWoTInstallerReceiveNetwork
{
public:
	explicit FRecordingNetwork(TSharedRef<FLog> InLog)
		: Log(MoveTemp(InLog))
		, Inner(WoTCreateProductionInstallerReceiveNetwork())
	{
	}

	virtual TUniquePtr<IWoTInstallerReceiveSocket> CreateDatagram() override
	{
		++Log->DatagramsCreated;
		TUniquePtr<IWoTInstallerReceiveSocket> Socket = Inner->CreateDatagram();
		return Socket.IsValid() ? MakeUnique<FRecordingSocket>(Log,
			MoveTemp(Socket), FRecordingSocket::ERole::Datagram) : nullptr;
	}

	virtual TUniquePtr<IWoTInstallerReceiveSocket> CreateStream() override
	{
		++Log->StreamsCreated;
		TUniquePtr<IWoTInstallerReceiveSocket> Socket = Inner->CreateStream();
		return Socket.IsValid() ? MakeUnique<FRecordingSocket>(Log,
			MoveTemp(Socket), FRecordingSocket::ERole::Listener) : nullptr;
	}

private:
	TSharedRef<FLog> Log;
	TSharedRef<IWoTInstallerReceiveNetwork> Inner;
};

/**
 * Plays a sharer over raw loopback sockets so a spec controls every byte: an
 * OFFER for any hash or session, the DATA header with any token, and the file
 * in measured pieces. Sends never block the spec thread: the receiver (or the
 * worker's receiver thread) reads in between.
 */
class FPeer final
{
public:
	explicit FPeer(uint8 Seed)
		: Session(Id(Seed))
	{
	}

	~FPeer()
	{
		CloseData();
		if (Udp != nullptr)
		{
			Udp->Close();
			Sockets->DestroySocket(Udp);
			Udp = nullptr;
		}
	}

	FPeer(const FPeer&) = delete;
	FPeer& operator=(const FPeer&) = delete;

	bool Open()
	{
		Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Sockets == nullptr)
		{
			return false;
		}
		Udp = Sockets->CreateSocket(NAME_DGram, TEXT("WoTReceiverSpecPeerOffer"),
			FNetworkProtocolTypes::IPv4);
		return Udp != nullptr && Udp->Bind(*Address(0)) && Udp->SetNonBlocking(true);
	}

	bool Offer(uint16 ReceiverPort, uint64 Size, const FWoTSha256& Sha256,
		const TCHAR* Label = WoTInstallerShare::LabelWindowsPC,
		const FWoTShareId* SessionOverride = nullptr)
	{
		FWoTShareOffer Message;
		Message.SessionId = SessionOverride != nullptr ? *SessionOverride : Session;
		Message.Sha256 = Sha256;
		Message.Size = Size;
		Message.Label = Label;
		const TArray<uint8> Bytes = WoTInstallerShare::EncodeOffer(Message);
		int32 Sent = 0;
		return Udp != nullptr && !Bytes.IsEmpty()
			&& Udp->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *Address(ReceiverPort))
			&& Sent == Bytes.Num();
	}

	/** A REQUEST that arrived for this peer, if any (never waits). */
	TOptional<FWoTShareRequest> PollRequest()
	{
		if (Udp == nullptr)
		{
			return {};
		}
		uint8 Buffer[512];
		int32 Read = 0;
		const TSharedRef<FInternetAddr> From =
			Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		if (!Udp->RecvFrom(Buffer, static_cast<int32>(UE_ARRAY_COUNT(Buffer)), Read,
				*From) || Read <= 0)
		{
			return {};
		}
		return WoTInstallerShare::DecodeRequest(
			TConstArrayView<uint8>(Buffer, Read));
	}

	/** Offers, then pumps until a REQUEST arrives or WaitSeconds pass. */
	TOptional<FWoTShareRequest> OfferAndAwaitRequest(uint16 ReceiverPort,
		uint64 Size, const FWoTSha256& Sha256, TFunctionRef<void()> Pump,
		double WaitSeconds = 10.0, const FWoTShareId* SessionOverride = nullptr)
	{
		if (!Offer(ReceiverPort, Size, Sha256, WoTInstallerShare::LabelWindowsPC,
				SessionOverride))
		{
			return {};
		}
		const double Deadline = FPlatformTime::Seconds() + WaitSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Pump();
			if (TOptional<FWoTShareRequest> Request = PollRequest())
			{
				return Request;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return {};
	}

	bool Connect(uint16 TcpPort)
	{
		CloseData();
		Data = Sockets->CreateSocket(NAME_Stream, TEXT("WoTReceiverSpecPeerData"),
			FNetworkProtocolTypes::IPv4);
		if (Data == nullptr || !Data->Connect(*Address(TcpPort)))
		{
			return false;
		}
		return Data->SetNonBlocking(true);
	}

	bool SendHeader(const FWoTShareId& Token, uint64 Offset, uint64 Length,
		TFunctionRef<void()> Pump)
	{
		FWoTShareDataHeader Header;
		Header.Token = Token;
		Header.Offset = Offset;
		Header.Length = Length;
		const TArray<uint8> Bytes = WoTInstallerShare::EncodeDataHeader(Header);
		return Stream(Bytes, Pump);
	}

	/** Sends every byte, pumping the receiver whenever the socket is full. */
	bool Stream(TConstArrayView<uint8> Bytes, TFunctionRef<void()> Pump,
		double WaitSeconds = 20.0)
	{
		if (Data == nullptr)
		{
			return false;
		}
		int32 Offset = 0;
		const double Deadline = FPlatformTime::Seconds() + WaitSeconds;
		while (Offset < Bytes.Num() && FPlatformTime::Seconds() < Deadline)
		{
			int32 Sent = 0;
			const int32 Want = FMath::Min(Bytes.Num() - Offset, 64 * 1024);
			if (!Data->Send(Bytes.GetData() + Offset, Want, Sent))
			{
				if (Sockets->GetLastErrorCode() != SE_EWOULDBLOCK)
				{
					return false;
				}
				Sent = 0;
			}
			Offset += FMath::Max(0, Sent);
			Pump();
		}
		return Offset == Bytes.Num();
	}

	/** What the receiver sent back on the data connection: never anything.
	 * Waits up to WaitSeconds; bOutClosed tells whether it closed (FIN). */
	int32 BytesFromReceiver(bool& bOutClosed, double WaitSeconds)
	{
		bOutClosed = false;
		if (Data == nullptr)
		{
			return 0;
		}
		int32 Total = 0;
		const double Deadline = FPlatformTime::Seconds() + WaitSeconds;
		while (FPlatformTime::Seconds() < Deadline && !bOutClosed)
		{
			if (!Data->Wait(ESocketWaitConditions::WaitForRead,
					FTimespan::FromSeconds(0.05)))
			{
				continue;
			}
			uint8 Buffer[256];
			int32 Read = 0;
			if (!Data->Recv(Buffer, static_cast<int32>(UE_ARRAY_COUNT(Buffer)), Read))
			{
				bOutClosed = true;
				break;
			}
			Total += FMath::Max(0, Read);
		}
		return Total;
	}

	void CloseData()
	{
		if (Data != nullptr)
		{
			Data->Close();
			Sockets->DestroySocket(Data);
			Data = nullptr;
		}
	}

	FWoTShareId Session;

private:
	TSharedRef<FInternetAddr> Address(uint16 Port) const
	{
		TSharedRef<FInternetAddr> Result =
			Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Result->SetIp(Loopback);
		Result->SetPort(Port);
		return Result;
	}

	ISocketSubsystem* Sockets = nullptr;
	FSocket* Udp = nullptr;
	FSocket* Data = nullptr;
};

/** True when the UDP port can be bound again on loopback: nothing holds it. */
inline bool PortIsFree(uint16 Port)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (Sockets == nullptr || Port == 0)
	{
		return false;
	}
	FSocket* Probe = Sockets->CreateSocket(NAME_DGram,
		TEXT("WoTReceiverSpecPortProbe"), FNetworkProtocolTypes::IPv4);
	if (Probe == nullptr)
	{
		return false;
	}
	TSharedRef<FInternetAddr> Address =
		Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	Address->SetIp(Loopback);
	Address->SetPort(Port);
	const bool bBound = Probe->Bind(*Address);
	Probe->Close();
	Sockets->DestroySocket(Probe);
	return bBound;
}
} // namespace WoTReceiverSpec

#endif // WITH_AUTOMATION_TESTS
