// LAN installer sharing (spec B1-B6): codec, address and interface rules, the
// sharer against a recording socket seam, the finder against real files with
// a small fake pin, the Mac gate, and one loopback-only transfer through the
// production sockets. No spec sends anything beyond loopback.
#if WITH_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Text/STextBlock.h"
#include "../Online/SWoTSharingNotice.h"
#include "../Online/WoTInstallerShareProtocol.h"
#include "../Online/WoTInstallerSharer.h"
#include "../Online/WoTInstallerSharingSubsystem.h"
#include "../Online/WoTLocalNetworkAccess.h"
#include "../Updater/WoTLocalInstallerForSharing.h"
#include "../Updater/WoTRememberedInstaller.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTMenuStyle.h"
#include "../WoTPreferencesSubsystem.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
constexpr EAutomationTestFlags WoTInstallerShareSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

/** A host-order IPv4 address from its octets: specs never spell a private
 * address as a dotted quad. */
constexpr uint32 WoTShareSpecIp(uint32 A, uint32 B, uint32 C, uint32 D)
{
	return (A << 24) | (B << 16) | (C << 8) | D;
}

FWoTShareId WoTShareSpecId(uint8 Seed)
{
	FWoTShareId Id;
	for (int32 Index = 0; Index < FWoTShareId::ByteCount; ++Index)
	{
		Id.Bytes[Index] = static_cast<uint8>(Seed + Index);
	}
	return Id;
}

TArray<uint8> WoTShareSpecPayload(int32 Size, uint8 Salt = 0)
{
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Size);
	for (int32 Index = 0; Index < Size; ++Index)
	{
		Bytes[Index] = static_cast<uint8>((Index * 31 + Index / 251 + Salt) & 0xFF);
	}
	return Bytes;
}

TArray<uint8> WoTShareSpecAscii(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	return Bytes;
}

struct FWoTShareSpecDatagram
{
	uint32 Address = 0;
	uint16 Port = 0;
	TArray<uint8> Bytes;
};

struct FWoTShareSpecStream
{
	uint32 Address = 0;
	uint16 Port = 0;
	EWoTShareIo ConnectResult = EWoTShareIo::Done;
	TArray<uint8> Received;
	int32 AcceptPerSend = MAX_int32;
	/** The receiver stops reading once it holds this many bytes. */
	int32 BlockAfterBytes = MAX_int32;
	bool bBlocked = false;
	bool bBroken = false;
	bool bClosed = false;
};

/** Everything the fake sockets saw, shared with the spec. */
struct FWoTShareSpecNet
{
	int32 DatagramsCreated = 0;
	int32 StreamsCreated = 0;
	int32 Binds = 0;
	int32 Listens = 0;
	int32 Broadcasts = 0;
	int32 DatagramCloses = 0;
	TArray<FWoTShareSpecDatagram> Sent;
	TSet<uint32> FailingDestinations;
	TArray<FWoTShareSpecDatagram> Inbox;
	TArray<TSharedRef<FWoTShareSpecStream>> Streams;
	TArray<FWoTShareAdapterAddress> Adapters;
	/** Applied to each new stream. */
	EWoTShareIo NextConnect = EWoTShareIo::Done;
	int32 NextAcceptPerSend = MAX_int32;
	int32 NextBlockAfterBytes = MAX_int32;
	bool bNextBlocked = false;
};

class FWoTShareSpecSocket final : public IWoTInstallerShareSocket
{
public:
	FWoTShareSpecSocket(TSharedRef<FWoTShareSpecNet> InNet,
		TSharedPtr<FWoTShareSpecStream> InStream)
		: Net(MoveTemp(InNet))
		, Stream(MoveTemp(InStream))
	{
	}

	virtual bool SetBroadcast() override
	{
		++Net->Broadcasts;
		return true;
	}
	virtual bool Bind(uint32, uint16) override
	{
		++Net->Binds;
		return true;
	}
	virtual bool Listen(int32) override
	{
		++Net->Listens;
		return true;
	}
	virtual EWoTShareIo SendTo(uint32 Address, uint16 Port,
		TConstArrayView<uint8> Bytes) override
	{
		Net->Sent.Add({Address, Port, TArray<uint8>(Bytes.GetData(), Bytes.Num())});
		return Net->FailingDestinations.Contains(Address)
			? EWoTShareIo::Failed : EWoTShareIo::Done;
	}
	virtual EWoTShareIo RecvFrom(TArray<uint8>& OutBytes, uint32& OutAddress,
		uint16& OutPort, double) override
	{
		if (Net->Inbox.IsEmpty())
		{
			return EWoTShareIo::WouldBlock;
		}
		const FWoTShareSpecDatagram Next = Net->Inbox[0];
		Net->Inbox.RemoveAt(0);
		OutBytes = Next.Bytes;
		OutAddress = Next.Address;
		OutPort = Next.Port;
		return EWoTShareIo::Done;
	}
	virtual bool BeginConnect(uint32 Address, uint16 Port) override
	{
		if (!Stream.IsValid())
		{
			return false;
		}
		Stream->Address = Address;
		Stream->Port = Port;
		return true;
	}
	virtual EWoTShareIo PollConnect() override
	{
		return Stream.IsValid() ? Stream->ConnectResult : EWoTShareIo::Failed;
	}
	virtual EWoTShareIo Send(TConstArrayView<uint8> Bytes, int32& OutSent) override
	{
		OutSent = 0;
		if (!Stream.IsValid() || Stream->bBroken || Stream->bClosed)
		{
			return EWoTShareIo::Failed;
		}
		const int32 Room = Stream->BlockAfterBytes - Stream->Received.Num();
		if (Stream->bBlocked || Room <= 0)
		{
			return EWoTShareIo::WouldBlock;
		}
		OutSent = FMath::Min3(Bytes.Num(), Stream->AcceptPerSend, Room);
		Stream->Received.Append(Bytes.GetData(), OutSent);
		return EWoTShareIo::Done;
	}
	virtual void WaitForWrite(double) override {}
	virtual void Close() override
	{
		if (Stream.IsValid())
		{
			Stream->bClosed = true;
		}
		else
		{
			++Net->DatagramCloses;
		}
	}

private:
	TSharedRef<FWoTShareSpecNet> Net;
	TSharedPtr<FWoTShareSpecStream> Stream;
};

class FWoTShareSpecNetwork final : public IWoTInstallerShareNetwork
{
public:
	explicit FWoTShareSpecNetwork(TSharedRef<FWoTShareSpecNet> InNet)
		: Net(MoveTemp(InNet))
	{
	}
	virtual TUniquePtr<IWoTInstallerShareSocket> CreateDatagram() override
	{
		++Net->DatagramsCreated;
		return MakeUnique<FWoTShareSpecSocket>(Net, nullptr);
	}
	virtual TUniquePtr<IWoTInstallerShareSocket> CreateStream() override
	{
		++Net->StreamsCreated;
		TSharedRef<FWoTShareSpecStream> Stream = MakeShared<FWoTShareSpecStream>();
		Stream->ConnectResult = Net->NextConnect;
		Stream->AcceptPerSend = Net->NextAcceptPerSend;
		Stream->BlockAfterBytes = Net->NextBlockAfterBytes;
		Stream->bBlocked = Net->bNextBlocked;
		Net->Streams.Add(Stream);
		return MakeUnique<FWoTShareSpecSocket>(Net, Stream);
	}
	virtual TArray<FWoTShareAdapterAddress> EnumerateAdapters() override
	{
		return Net->Adapters;
	}

private:
	TSharedRef<FWoTShareSpecNet> Net;
};

struct FWoTShareSpecFile
{
	TArray<uint8> Bytes;
	bool bFound = true;
	bool bOpenFails = false;
	bool bChanged = false;
	int32 Finds = 0;
	int32 Opens = 0;
};

class FWoTShareSpecReader final : public IWoTInstallerShareReader
{
public:
	explicit FWoTShareSpecReader(TSharedRef<FWoTShareSpecFile> InFile)
		: File(MoveTemp(InFile))
	{
	}
	virtual TWoTBootstrapResult<uint64> ReadAt(uint64 Offset,
		TArrayView<uint8> Out) override
	{
		const uint64 Size = static_cast<uint64>(File->Bytes.Num());
		const uint64 Count = Offset >= Size ? 0
			: FMath::Min<uint64>(static_cast<uint64>(Out.Num()), Size - Offset);
		if (Count > 0)
		{
			FMemory::Memcpy(Out.GetData(), File->Bytes.GetData() + Offset, Count);
		}
		return TWoTBootstrapResult<uint64>::Success(Count);
	}
	virtual bool IsUnchanged() override
	{
		return !File->bChanged;
	}

private:
	TSharedRef<FWoTShareSpecFile> File;
};

class FWoTShareSpecSource final : public IWoTInstallerShareSource
{
public:
	explicit FWoTShareSpecSource(TSharedRef<FWoTShareSpecFile> InFile)
		: File(MoveTemp(InFile))
	{
	}
	virtual FString Find() override
	{
		++File->Finds;
		return File->bFound ? FString(TEXT("adjacent")) : FString();
	}
	virtual TUniquePtr<IWoTInstallerShareReader> Open() override
	{
		++File->Opens;
		if (File->bOpenFails)
		{
			return nullptr;
		}
		return MakeUnique<FWoTShareSpecReader>(File);
	}

private:
	TSharedRef<FWoTShareSpecFile> File;
};

struct FWoTShareSpecBeat
{
	FString Event;
	FString Detail;
};

/** One sharer on fake sockets, a memory file and a hand-driven clock. */
struct FWoTShareSpecRig
{
	TSharedRef<FWoTShareSpecNet> Net = MakeShared<FWoTShareSpecNet>();
	TSharedRef<FWoTShareSpecFile> File = MakeShared<FWoTShareSpecFile>();
	TSharedRef<double> Clock = MakeShared<double>(1000.0);
	TSharedRef<TArray<FWoTInstallerSharerEvent>> Events =
		MakeShared<TArray<FWoTInstallerSharerEvent>>();
	TSharedRef<TArray<FWoTShareSpecBeat>> Beats =
		MakeShared<TArray<FWoTShareSpecBeat>>();
	FWoTSha256 Pin;
	TUniquePtr<FWoTInstallerSharer> Sharer;

	explicit FWoTShareSpecRig(int32 Size = 600 * 1024)
	{
		File->Bytes = WoTShareSpecPayload(Size);
		Pin = Sha256(File->Bytes);
		Net->Adapters.Add({WoTShareSpecIp(192, 168, 4, 9), 24, true, false,
			false, false});
	}

	FWoTInstallerSharer::FConfig Config() const
	{
		FWoTInstallerSharer::FConfig Result;
		Result.Label = WoTInstallerShare::LabelWindowsPC;
		Result.Size = static_cast<uint64>(File->Bytes.Num());
		Result.Sha256 = Pin;
		return Result;
	}

	FWoTInstallerSharer::FDependencies Dependencies() const
	{
		FWoTInstallerSharer::FDependencies Result;
		Result.Network = MakeShared<FWoTShareSpecNetwork>(Net);
		Result.Source = MakeShared<FWoTShareSpecSource>(File);
		Result.NowSeconds = [ClockValue = Clock]() { return *ClockValue; };
		Result.OnEvent = [EventLog = Events](const FWoTInstallerSharerEvent& Event)
		{
			EventLog->Add(Event);
		};
		Result.Heartbeat = [BeatLog = Beats](const TCHAR* Event, const FString& Detail)
		{
			BeatLog->Add({Event, Detail});
		};
		return Result;
	}

	void Make()
	{
		Sharer = MakeUnique<FWoTInstallerSharer>(Config(), Dependencies());
	}

	void Pump(int32 Steps = 1)
	{
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Sharer->Pump();
		}
	}

	void Request(uint32 From, const FWoTShareId& Token, uint64 Offset = 0,
		uint16 TcpPort = 40000, const TCHAR* Label = WoTInstallerShare::LabelSteamDeck,
		const FWoTShareId* Session = nullptr)
	{
		FWoTShareRequest Message;
		Message.SessionId = Session != nullptr ? *Session : Sharer->SessionId();
		Message.Token = Token;
		Message.TcpPort = TcpPort;
		Message.ResumeOffset = Offset;
		Message.Label = Label;
		Net->Inbox.Add({From, 50000, WoTInstallerShare::EncodeRequest(Message)});
	}

	int32 CountBeats(const TCHAR* Event) const
	{
		int32 Count = 0;
		for (const FWoTShareSpecBeat& Beat : *Beats)
		{
			Count += Beat.Event == Event ? 1 : 0;
		}
		return Count;
	}

	const FWoTShareSpecBeat* LastBeat(const TCHAR* Event) const
	{
		for (int32 Index = Beats->Num() - 1; Index >= 0; --Index)
		{
			if ((*Beats)[Index].Event == Event)
			{
				return &(*Beats)[Index];
			}
		}
		return nullptr;
	}

	int32 CountEvents(FWoTInstallerSharerEvent::EKind Kind) const
	{
		int32 Count = 0;
		for (const FWoTInstallerSharerEvent& Event : *Events)
		{
			Count += Event.Kind == Kind ? 1 : 0;
		}
		return Count;
	}

	/** The header and payload a stream received, split and checked. */
	bool StreamCarries(const FWoTShareSpecStream& Stream, const FWoTShareId& Token,
		uint64 Offset) const
	{
		if (Stream.Received.Num() < WoTInstallerShare::DataHeaderBytes)
		{
			return false;
		}
		const TOptional<FWoTShareDataHeader> Header = WoTInstallerShare::DecodeDataHeader(
			TConstArrayView<uint8>(Stream.Received.GetData(),
				WoTInstallerShare::DataHeaderBytes));
		const uint64 Size = static_cast<uint64>(File->Bytes.Num());
		if (!Header.IsSet() || Header->Token != Token || Header->Offset != Offset
			|| Header->Length != Size - Offset
			|| static_cast<uint64>(Stream.Received.Num())
				!= WoTInstallerShare::DataHeaderBytes + Size - Offset)
		{
			return false;
		}
		return FMemory::Memcmp(Stream.Received.GetData()
			+ WoTInstallerShare::DataHeaderBytes,
			File->Bytes.GetData() + Offset, Size - Offset) == 0;
	}
};

FWoTShareAdapterAddress WoTShareSpecAdapter(uint32 Address, uint8 Prefix)
{
	FWoTShareAdapterAddress Adapter;
	Adapter.Address = Address;
	Adapter.PrefixLength = Prefix;
	Adapter.bUp = true;
	return Adapter;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareCodecSpec,
	"WoT.InstallerShare.Codec", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareCodecSpec::RunTest(const FString&)
{
	FWoTShareOffer Offer;
	Offer.SessionId = WoTShareSpecId(7);
	Offer.Sha256 = Sha256(WoTShareSpecAscii("installer"));
	Offer.Size = 1029509080ull;
	Offer.Label = WoTInstallerShare::LabelSteamDeck;
	const TArray<uint8> OfferBytes = WoTInstallerShare::EncodeOffer(Offer);
	TestEqual(TEXT("an OFFER is 64 fixed bytes plus its label"), OfferBytes.Num(),
		64 + FCString::Strlen(WoTInstallerShare::LabelSteamDeck));
	TestTrue(TEXT("an OFFER starts with WOTIS, version 1, type 1"),
		OfferBytes.Num() > 7 && OfferBytes[0] == 'W' && OfferBytes[4] == 'S'
		&& OfferBytes[5] == 1 && OfferBytes[6] == 1);
	const TOptional<FWoTShareOffer> DecodedOffer = WoTInstallerShare::DecodeOffer(OfferBytes);
	TestTrue(TEXT("an OFFER round-trips"), DecodedOffer.IsSet()
		&& DecodedOffer->SessionId == Offer.SessionId
		&& DecodedOffer->Sha256 == Offer.Sha256 && DecodedOffer->Size == Offer.Size
		&& DecodedOffer->Label == Offer.Label);
	TestTrue(TEXT("integers are big-endian"), OfferBytes.Num() > 63
		&& OfferBytes[55] == 0x00 && OfferBytes[58] == 0x00
		&& OfferBytes[59] == 0x3D && OfferBytes[60] == 0x5D
		&& OfferBytes[61] == 0x0F && OfferBytes[62] == 0xD8
		&& OfferBytes[63] == 12);

	FWoTShareRequest Request;
	Request.SessionId = WoTShareSpecId(7);
	Request.Token = WoTShareSpecId(90);
	Request.TcpPort = 46253;
	Request.ResumeOffset = 123456789ull;
	// "Cafe uber" with an e-acute and a u-umlaut: multi-byte UTF-8.
	Request.Label = FString(TEXT("Caf")) + static_cast<TCHAR>(0xE9)
		+ TEXT(" ") + static_cast<TCHAR>(0xFC) + TEXT("ber");
	const TArray<uint8> RequestBytes = WoTInstallerShare::EncodeRequest(Request);
	const TOptional<FWoTShareRequest> DecodedRequest =
		WoTInstallerShare::DecodeRequest(RequestBytes);
	TestTrue(TEXT("a REQUEST with a multi-byte label round-trips"),
		DecodedRequest.IsSet() && DecodedRequest->SessionId == Request.SessionId
		&& DecodedRequest->Token == Request.Token
		&& DecodedRequest->TcpPort == Request.TcpPort
		&& DecodedRequest->ResumeOffset == Request.ResumeOffset
		&& DecodedRequest->Label == Request.Label);
	TestFalse(TEXT("a REQUEST is not an OFFER"),
		WoTInstallerShare::DecodeOffer(RequestBytes).IsSet());
	TestFalse(TEXT("an OFFER is not a REQUEST"),
		WoTInstallerShare::DecodeRequest(OfferBytes).IsSet());

	FWoTShareDataHeader Header;
	Header.Token = WoTShareSpecId(90);
	Header.Offset = 4096;
	Header.Length = 1029509080ull - 4096;
	const TArray<uint8> HeaderBytes = WoTInstallerShare::EncodeDataHeader(Header);
	TestEqual(TEXT("the DATA header is 39 bytes"), HeaderBytes.Num(),
		WoTInstallerShare::DataHeaderBytes);
	const TOptional<FWoTShareDataHeader> DecodedHeader =
		WoTInstallerShare::DecodeDataHeader(HeaderBytes);
	TestTrue(TEXT("the DATA header round-trips"), DecodedHeader.IsSet()
		&& DecodedHeader->Token == Header.Token
		&& DecodedHeader->Offset == Header.Offset
		&& DecodedHeader->Length == Header.Length);

	auto Mutated = [](TArray<uint8> Bytes, int32 Index, uint8 Value)
	{
		Bytes[Index] = Value;
		return Bytes;
	};
	TestFalse(TEXT("a wrong magic is rejected"),
		WoTInstallerShare::DecodeOffer(Mutated(OfferBytes, 0, 'X')).IsSet());
	TestFalse(TEXT("a wrong version is rejected"),
		WoTInstallerShare::DecodeOffer(Mutated(OfferBytes, 5, 2)).IsSet());
	TestFalse(TEXT("a wrong type is rejected"),
		WoTInstallerShare::DecodeOffer(Mutated(OfferBytes, 6, 3)).IsSet());
	TestFalse(TEXT("a truncated OFFER is rejected"), WoTInstallerShare::DecodeOffer(
		TConstArrayView<uint8>(OfferBytes.GetData(), OfferBytes.Num() - 1)).IsSet());
	TArray<uint8> Longer = OfferBytes;
	Longer.Add('x');
	TestFalse(TEXT("an OFFER with a trailing byte is rejected"),
		WoTInstallerShare::DecodeOffer(Longer).IsSet());
	TestFalse(TEXT("a label length that disagrees with the size is rejected"),
		WoTInstallerShare::DecodeOffer(Mutated(OfferBytes, 63, 3)).IsSet());
	TestFalse(TEXT("a truncated DATA header is rejected"),
		WoTInstallerShare::DecodeDataHeader(TConstArrayView<uint8>(
			HeaderBytes.GetData(), HeaderBytes.Num() - 1)).IsSet());
	FWoTShareDataHeader Overflow;
	Overflow.Offset = MAX_uint64 - 1;
	Overflow.Length = 4;
	TestFalse(TEXT("a DATA range past 2^64 is rejected"),
		WoTInstallerShare::DecodeDataHeader(
			WoTInstallerShare::EncodeDataHeader(Overflow)).IsSet());

	// Label rules, both on encode and on decode.
	TestFalse(TEXT("an empty label is not encoded"), [&]()
	{
		FWoTShareOffer Empty = Offer;
		Empty.Label.Reset();
		return !WoTInstallerShare::EncodeOffer(Empty).IsEmpty();
	}());
	TestTrue(TEXT("48 bytes is the longest label"),
		WoTInstallerShare::IsValidLabel(FString::ChrN(48, TEXT('a'))));
	TestFalse(TEXT("49 bytes is too long"),
		WoTInstallerShare::IsValidLabel(FString::ChrN(49, TEXT('a'))));
	TestFalse(TEXT("a 25-character label of 2-byte characters is too long"),
		WoTInstallerShare::IsValidLabel(FString::ChrN(25, static_cast<TCHAR>(0xE9))));
	TestFalse(TEXT("a control character is refused"),
		WoTInstallerShare::IsValidLabel(TEXT("a\tb")));
	TestFalse(TEXT("a C1 control is refused"), WoTInstallerShare::IsValidLabel(
		FString(TEXT("a")) + static_cast<TCHAR>(0x85) + TEXT("b")));
	auto WithLabelBytes = [&OfferBytes](std::initializer_list<uint8> Label)
	{
		TArray<uint8> Bytes(OfferBytes.GetData(), 63);
		Bytes.Add(static_cast<uint8>(Label.size()));
		for (const uint8 Byte : Label)
		{
			Bytes.Add(Byte);
		}
		return Bytes;
	};
	TestTrue(TEXT("a crafted plain label decodes"),
		WoTInstallerShare::DecodeOffer(WithLabelBytes({'a', 'b'})).IsSet());
	TestFalse(TEXT("a zero-length label is rejected"),
		WoTInstallerShare::DecodeOffer(WithLabelBytes({})).IsSet());
	TestFalse(TEXT("a broken UTF-8 sequence is rejected"),
		WoTInstallerShare::DecodeOffer(WithLabelBytes({0xC3, 0x28})).IsSet());
	TestFalse(TEXT("an overlong UTF-8 form is rejected"),
		WoTInstallerShare::DecodeOffer(WithLabelBytes({0xC0, 0xAF})).IsSet());
	TestFalse(TEXT("an encoded surrogate is rejected"),
		WoTInstallerShare::DecodeOffer(WithLabelBytes({0xED, 0xA0, 0x80})).IsSet());
	TestFalse(TEXT("a truncated UTF-8 sequence is rejected"),
		WoTInstallerShare::DecodeOffer(WithLabelBytes({'a', 0xE2, 0x82})).IsSet());
	TestFalse(TEXT("an encoded control character is rejected"),
		WoTInstallerShare::DecodeOffer(WithLabelBytes({'a', 0x07})).IsSet());
	TArray<uint8> TooLong = WithLabelBytes({});
	TooLong[63] = 49;
	for (int32 Index = 0; Index < 49; ++Index)
	{
		TooLong.Add('a');
	}
	TestFalse(TEXT("a 49-byte label is rejected on decode"),
		WoTInstallerShare::DecodeOffer(TooLong).IsSet());
	TestEqual(TEXT("P is 14095"), static_cast<int32>(WoTInstallerShare::DiscoveryPort), 14095);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerSharePrivateAddressSpec,
	"WoT.InstallerShare.PrivateAddress", WoTInstallerShareSpecFlags)

bool FWoTInstallerSharePrivateAddressSpec::RunTest(const FString&)
{
	const uint32 Accepted[] = {
		WoTShareSpecIp(10, 0, 0, 1), WoTShareSpecIp(10, 255, 255, 254),
		WoTShareSpecIp(172, 16, 0, 1), WoTShareSpecIp(172, 31, 255, 254),
		WoTShareSpecIp(192, 168, 0, 1), WoTShareSpecIp(192, 168, 255, 254),
		WoTShareSpecIp(169, 254, 1, 1)};
	for (const uint32 Address : Accepted)
	{
		TestTrue(FString::Printf(TEXT("private %08x is accepted"), Address),
			WoTInstallerShare::IsPrivatePeerAddress(Address, false));
	}
	const uint32 Refused[] = {
		WoTShareSpecIp(8, 8, 8, 8), WoTShareSpecIp(172, 15, 255, 255),
		WoTShareSpecIp(172, 32, 0, 1), WoTShareSpecIp(192, 169, 0, 1),
		WoTShareSpecIp(100, 64, 0, 1), WoTShareSpecIp(224, 0, 0, 1),
		WoTShareSpecIp(255, 255, 255, 255), 0u, WoTShareSpecIp(11, 0, 0, 1),
		WoTShareSpecIp(169, 253, 0, 1)};
	for (const uint32 Address : Refused)
	{
		TestFalse(FString::Printf(TEXT("non-private %08x is refused"), Address),
			WoTInstallerShare::IsPrivatePeerAddress(Address, true));
	}
	const uint32 Loopback = WoTShareSpecIp(127, 0, 0, 1);
	TestFalse(TEXT("loopback is refused outside tests and the harness"),
		WoTInstallerShare::IsPrivatePeerAddress(Loopback, false));
	TestTrue(TEXT("loopback passes only when allowed"),
		WoTInstallerShare::IsPrivatePeerAddress(Loopback, true));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareInterfaceFilterSpec,
	"WoT.InstallerShare.InterfaceFilter", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareInterfaceFilterSpec::RunTest(const FString&)
{
	TArray<FWoTShareAdapterAddress> Adapters;
	// The two-adapter PC from Task 0: a /22 Ethernet and a /24 Wi-Fi.
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(192, 168, 5, 150), 22));
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(10, 35, 78, 4), 24));
	FWoTShareAdapterAddress Down = WoTShareSpecAdapter(WoTShareSpecIp(10, 1, 1, 5), 24);
	Down.bUp = false;
	Adapters.Add(Down);
	FWoTShareAdapterAddress Loopback = WoTShareSpecAdapter(WoTShareSpecIp(127, 0, 0, 1), 8);
	Loopback.bLoopback = true;
	Adapters.Add(Loopback);
	FWoTShareAdapterAddress Vpn = WoTShareSpecAdapter(WoTShareSpecIp(10, 8, 0, 2), 24);
	Vpn.bTunnel = true;
	Adapters.Add(Vpn);
	FWoTShareAdapterAddress Tentative = WoTShareSpecAdapter(WoTShareSpecIp(172, 20, 0, 9), 16);
	Tentative.bTentative = true;
	Adapters.Add(Tentative);
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(169, 254, 7, 7), 16));
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(73, 12, 34, 56), 24));
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(192, 168, 9, 1), 32));
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(192, 168, 9, 2), 31));
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(192, 168, 9, 3), 0));
	// A second address on the /22: the same broadcast, sent once.
	Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(192, 168, 6, 1), 22));
	const TArray<uint32> Directed = WoTInstallerShare::DirectedBroadcasts(Adapters);
	TestEqual(TEXT("only the eligible adapters have a directed broadcast"),
		Directed.Num(), 2);
	TestTrue(TEXT("the /22 broadcast is computed from OnLinkPrefixLength"),
		Directed.Contains(WoTShareSpecIp(192, 168, 7, 255)));
	TestTrue(TEXT("the /24 broadcast"),
		Directed.Contains(WoTShareSpecIp(10, 35, 78, 255)));
	const TArray<uint32> Destinations = WoTInstallerShare::OfferDestinations(Adapters);
	TestTrue(TEXT("offers go to the limited broadcast first"),
		Destinations.Num() == 3 && Destinations[0] == WoTInstallerShare::LimitedBroadcast);
	TestEqual(TEXT("no adapter still means the limited broadcast"),
		WoTInstallerShare::OfferDestinations(TArray<FWoTShareAdapterAddress>()).Num(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareLabelsSpec,
	"WoT.InstallerShare.Labels", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareLabelsSpec::RunTest(const FString&)
{
	TestEqual(TEXT("native Windows"), FString(WoTInstallerShare::LocalLabelFor(
		false, false, FStringView())), FString(TEXT("a Windows PC")));
	TestEqual(TEXT("a Mac build"), FString(WoTInstallerShare::LocalLabelFor(
		true, false, FStringView())), FString(TEXT("a Mac")));
	TestEqual(TEXT("Proton on Linux"), FString(WoTInstallerShare::LocalLabelFor(
		false, true, TEXT("Linux"))), FString(TEXT("a Steam Deck")));
	TestEqual(TEXT("CrossOver on macOS counts as a Mac"),
		FString(WoTInstallerShare::LocalLabelFor(false, true, TEXT("Darwin"))),
		FString(TEXT("a Mac")));
	TestTrue(TEXT("the local label is one of the three"),
		WoTInstallerShare::DisplayLabel(WoTInstallerShare::LocalLabel())
			!= WoTInstallerShare::LabelOtherComputer);
	TestEqual(TEXT("a peer-chosen label is never shown"),
		WoTInstallerShare::DisplayLabel(TEXT("Totally Legit Update")),
		FString(TEXT("another computer")));
	TestEqual(TEXT("labels match exactly"),
		WoTInstallerShare::DisplayLabel(TEXT("A Mac")),
		FString(TEXT("another computer")));
	TestEqual(TEXT("the sending notice"),
		WoTInstallerShare::SendingNotice(WoTInstallerShare::LabelSteamDeck),
		FString(TEXT("Sending the original game installer to a Steam Deck...")));
	TestEqual(TEXT("the sent notice"),
		WoTInstallerShare::SentNotice(WoTInstallerShare::LabelSteamDeck),
		FString(TEXT("Sending the original game installer to a Steam Deck... sent")));
	TestEqual(TEXT("the stopped notice"),
		WoTInstallerShare::StoppedNotice(WoTInstallerShare::LabelMac),
		FString(TEXT("Sending the original game installer to a Mac... stopped")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareOffersSpec,
	"WoT.InstallerShare.Sharer.Offers", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareOffersSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	Rig.Make();
	Rig.Pump();
	TestEqual(TEXT("one search at start"), Rig.File->Finds, 1);
	TestEqual(TEXT("one offer socket"), Rig.Net->DatagramsCreated, 1);
	TestEqual(TEXT("broadcast is enabled on it"), Rig.Net->Broadcasts, 1);
	TestEqual(TEXT("the first offer goes to the limited and the directed broadcast"),
		Rig.Net->Sent.Num(), 2);
	if (Rig.Net->Sent.Num() == 2)
	{
		TestEqual(TEXT("limited broadcast first"), Rig.Net->Sent[0].Address,
			WoTInstallerShare::LimitedBroadcast);
		TestEqual(TEXT("then the adapter's directed broadcast"),
			Rig.Net->Sent[1].Address, WoTShareSpecIp(192, 168, 4, 255));
		TestEqual(TEXT("to port P"), static_cast<int32>(Rig.Net->Sent[0].Port), 14095);
		const TOptional<FWoTShareOffer> Offer =
			WoTInstallerShare::DecodeOffer(Rig.Net->Sent[0].Bytes);
		TestTrue(TEXT("the OFFER carries the pin and this session"), Offer.IsSet()
			&& Offer->Sha256 == Rig.Pin
			&& Offer->Size == static_cast<uint64>(Rig.File->Bytes.Num())
			&& Offer->SessionId == Rig.Sharer->SessionId());
		TestTrue(TEXT("the OFFER's label is generic"), Offer.IsSet()
			&& Offer->Label == WoTInstallerShare::LabelWindowsPC);
	}
	TestEqual(TEXT("the first offer is recorded"), Rig.CountBeats(TEXT("lan-share-offer")), 1);
	const FWoTShareSpecBeat* Beat = Rig.LastBeat(TEXT("lan-share-offer"));
	TestTrue(TEXT("the offer heartbeat names the source and destinations"),
		Beat != nullptr && Beat->Detail.Contains(TEXT("source=adjacent"))
		&& Beat->Detail.Contains(TEXT("destinations=2")));

	*Rig.Clock += 1.0;
	Rig.Pump();
	TestEqual(TEXT("no second offer within 2 s"), Rig.Net->Sent.Num(), 2);
	*Rig.Clock += 1.0;
	Rig.Pump();
	TestEqual(TEXT("an offer every 2 s"), Rig.Net->Sent.Num(), 4);
	TestEqual(TEXT("still one socket"), Rig.Net->DatagramsCreated, 1);
	for (int32 Second = 0; Second < 56; Second += 2)
	{
		*Rig.Clock += 2.0;
		Rig.Pump();
	}
	TestEqual(TEXT("offer heartbeats are rate-limited to one a minute"),
		Rig.CountBeats(TEXT("lan-share-offer")), 1);
	*Rig.Clock += 4.0;
	Rig.Pump();
	TestEqual(TEXT("then one more after a minute"),
		Rig.CountBeats(TEXT("lan-share-offer")), 2);
	TestEqual(TEXT("a found installer is not searched again"), Rig.File->Finds, 1);
	TestEqual(TEXT("the sharer never binds"), Rig.Net->Binds, 0);
	TestEqual(TEXT("the sharer never listens"), Rig.Net->Listens, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNeverListensSpec,
	"WoT.InstallerShare.Sharer.NeverBindsOrListens", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNeverListensSpec::RunTest(const FString&)
{
	// Every path the sharer has, through the recording socket seam: offers,
	// junk datagrams, a finished transfer, a resume that stalls, a refused
	// connection, then stop. Not one Bind or Listen on any socket.
	FWoTShareSpecRig Rig;
	Rig.Make();
	Rig.Pump();
	Rig.Net->Inbox.Add({WoTShareSpecIp(192, 168, 4, 2), 50000,
		WoTShareSpecAscii("not a request")});
	Rig.Request(WoTShareSpecIp(192, 168, 4, 3), WoTShareSpecId(1));
	Rig.Pump(3);
	*Rig.Clock += 2.0;
	Rig.Net->NextBlockAfterBytes = 100;
	Rig.Request(WoTShareSpecIp(192, 168, 4, 4), WoTShareSpecId(2), 1000);
	Rig.Pump();
	*Rig.Clock += 16.0;
	Rig.Pump();
	Rig.Net->NextConnect = EWoTShareIo::Failed;
	Rig.Request(WoTShareSpecIp(192, 168, 4, 5), WoTShareSpecId(3));
	Rig.Pump(2);
	*Rig.Clock += 2.0;
	Rig.Pump();
	Rig.Sharer->Shutdown();
	TestTrue(TEXT("the scenario offered and opened three transfers"),
		Rig.Net->Sent.Num() > 2 && Rig.Net->StreamsCreated == 3);
	TestEqual(TEXT("one sent, one stalled, one refused"),
		Rig.CountBeats(TEXT("lan-share-send-end")), 3);
	TestEqual(TEXT("no socket was ever bound"), Rig.Net->Binds, 0);
	TestEqual(TEXT("no socket ever listened"), Rig.Net->Listens, 0);
	TestEqual(TEXT("one offer socket, broadcast-enabled"), Rig.Net->Broadcasts, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNothingFoundSpec,
	"WoT.InstallerShare.Sharer.NothingFound", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNothingFoundSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	Rig.File->bFound = false;
	Rig.Make();
	Rig.Pump(5);
	TestEqual(TEXT("searched once at start"), Rig.File->Finds, 1);
	TestEqual(TEXT("nothing found opens no socket"), Rig.Net->DatagramsCreated, 0);
	TestEqual(TEXT("nothing found sends nothing"), Rig.Net->Sent.Num(), 0);
	TestEqual(TEXT("the search result is reported"),
		Rig.CountEvents(FWoTInstallerSharerEvent::EKind::SearchFinished), 1);
	TestTrue(TEXT("as not found"), !Rig.Events->IsEmpty() && !(*Rig.Events)[0].bFound);
	*Rig.Clock += 59.0;
	Rig.Pump();
	TestEqual(TEXT("no new search before a minute"), Rig.File->Finds, 1);
	*Rig.Clock += 1.0;
	Rig.Pump();
	TestEqual(TEXT("a new search every minute"), Rig.File->Finds, 2);
	Rig.File->bFound = true;
	*Rig.Clock += 60.0;
	Rig.Pump();
	TestEqual(TEXT("a later find starts offering"), Rig.Net->DatagramsCreated, 1);
	TestTrue(TEXT("and offers at once"), Rig.Net->Sent.Num() >= 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareSendErrorsSpec,
	"WoT.InstallerShare.Sharer.SendErrors", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareSendErrorsSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	Rig.Net->Adapters.Add(WoTShareSpecAdapter(WoTShareSpecIp(10, 0, 3, 7), 24));
	// A Mac refusing the limited broadcast (EHOSTUNREACH) and a vanished adapter.
	Rig.Net->FailingDestinations.Add(WoTInstallerShare::LimitedBroadcast);
	Rig.Net->FailingDestinations.Add(WoTShareSpecIp(192, 168, 4, 255));
	Rig.Make();
	Rig.Pump();
	TestEqual(TEXT("every destination is tried despite earlier failures"),
		Rig.Net->Sent.Num(), 3);
	TestTrue(TEXT("the last destination still got its offer"),
		Rig.Net->Sent.Num() == 3
		&& Rig.Net->Sent[2].Address == WoTShareSpecIp(10, 0, 3, 255));
	const FWoTShareSpecBeat* Beat = Rig.LastBeat(TEXT("lan-share-offer"));
	TestTrue(TEXT("the failures are counted"),
		Beat != nullptr && Beat->Detail.Contains(TEXT("failed=2")));
	*Rig.Clock += 2.0;
	Rig.Pump();
	TestEqual(TEXT("the loop keeps offering after failures"), Rig.Net->Sent.Num(), 6);
	TestEqual(TEXT("on the same socket"), Rig.Net->DatagramsCreated, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareTransferSpec,
	"WoT.InstallerShare.Sharer.Transfer", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareTransferSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	Rig.Make();
	Rig.Pump();
	const uint32 Deck = WoTShareSpecIp(192, 168, 4, 127);
	const FWoTShareId Token = WoTShareSpecId(40);
	Rig.Request(Deck, Token, 0, 46253);
	Rig.Pump();
	TestEqual(TEXT("an accepted REQUEST opens one outward stream"),
		Rig.Net->StreamsCreated, 1);
	if (Rig.Net->Streams.Num() != 1)
	{
		return false;
	}
	const TSharedRef<FWoTShareSpecStream> Stream = Rig.Net->Streams[0];
	TestTrue(TEXT("it connects to the REQUEST's source and port"),
		Stream->Address == Deck && Stream->Port == 46253);
	TestEqual(TEXT("the installer is opened once per transfer"), Rig.File->Opens, 1);
	for (int32 Step = 0; Step < 10 && Rig.Sharer->ActiveTransferCount() > 0; ++Step)
	{
		Rig.Pump();
	}
	TestEqual(TEXT("the transfer finishes"), Rig.Sharer->ActiveTransferCount(), 0);
	TestTrue(TEXT("DATA carries the token, the range and every byte"),
		Rig.StreamCarries(*Stream, Token, 0));
	TestTrue(TEXT("the stream is closed"), Stream->bClosed);
	TestEqual(TEXT("one started notice"),
		Rig.CountEvents(FWoTInstallerSharerEvent::EKind::SendStarted), 1);
	TestEqual(TEXT("one ended notice"),
		Rig.CountEvents(FWoTInstallerSharerEvent::EKind::SendEnded), 1);
	for (const FWoTInstallerSharerEvent& Event : *Rig.Events)
	{
		if (Event.Kind == FWoTInstallerSharerEvent::EKind::SendEnded)
		{
			TestTrue(TEXT("it ended sent, to a Steam Deck"),
				Event.Result == EWoTShareTransferResult::Sent
				&& Event.Label == WoTInstallerShare::LabelSteamDeck);
			TestEqual(TEXT("the player sees ... sent"),
				UWoTInstallerSharingSubsystem::NoticeText(Event),
				FString(TEXT("Sending the original game installer to a Steam Deck... sent")));
		}
	}
	TestEqual(TEXT("send-start is recorded"), Rig.CountBeats(TEXT("lan-share-send-start")), 1);
	const FWoTShareSpecBeat* End = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("send-end records the result, the bytes and a good hash"),
		End != nullptr && End->Detail.Contains(TEXT("result=sent"))
		&& End->Detail.Contains(FString::Printf(TEXT("bytes=%d"), Rig.File->Bytes.Num()))
		&& End->Detail.Contains(TEXT("hash=good"))
		&& End->Detail.Contains(TEXT("peer=steam-deck")));
	TestFalse(TEXT("a good file is still offered"), Rig.Sharer->IsInstallerRejected());

	// A resume from another receiver gets only the tail, after a small accept
	// window per send so the bytes come in several steps.
	*Rig.Clock += 2.0;
	const uint32 Mac = WoTShareSpecIp(192, 168, 4, 30);
	const FWoTShareId ResumeToken = WoTShareSpecId(41);
	const uint64 ResumeAt = 300000;
	Rig.Net->NextAcceptPerSend = 50000;
	Rig.Request(Mac, ResumeToken, ResumeAt, 51000, WoTInstallerShare::LabelMac);
	Rig.Pump();
	if (Rig.Net->Streams.Num() != 2)
	{
		AddError(TEXT("the resume REQUEST did not open a stream"));
		return false;
	}
	for (int32 Step = 0; Step < 40 && Rig.Sharer->ActiveTransferCount() > 0; ++Step)
	{
		Rig.Pump();
	}
	TestTrue(TEXT("a resumed transfer sends exactly the tail"),
		Rig.StreamCarries(*Rig.Net->Streams[1], ResumeToken, ResumeAt));
	const FWoTShareSpecBeat* ResumeEnd = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("a resume cannot check the whole file"),
		ResumeEnd != nullptr && ResumeEnd->Detail.Contains(TEXT("result=sent"))
		&& ResumeEnd->Detail.Contains(TEXT("hash=not-checked"))
		&& ResumeEnd->Detail.Contains(TEXT("offset=300000")));
	TestEqual(TEXT("never a bind"), Rig.Net->Binds, 0);
	TestEqual(TEXT("never a listen"), Rig.Net->Listens, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareRequestLimitsSpec,
	"WoT.InstallerShare.Sharer.RequestLimits", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareRequestLimitsSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	Rig.Net->NextConnect = EWoTShareIo::WouldBlock;
	Rig.Make();
	Rig.Pump();
	const uint32 Peer = WoTShareSpecIp(10, 0, 0, 20);
	const FWoTShareId OtherSession = WoTShareSpecId(200);
	Rig.Request(Peer, WoTShareSpecId(1), 0, 40000, WoTInstallerShare::LabelSteamDeck,
		&OtherSession);
	Rig.Pump();
	TestEqual(TEXT("another session's REQUEST is ignored"), Rig.Net->StreamsCreated, 0);
	Rig.Request(WoTShareSpecIp(8, 8, 4, 4), WoTShareSpecId(2));
	Rig.Request(WoTShareSpecIp(127, 0, 0, 1), WoTShareSpecId(3));
	Rig.Pump();
	TestEqual(TEXT("public and loopback sources are dropped"), Rig.Net->StreamsCreated, 0);

	*Rig.Clock += 5.0;
	Rig.Request(Peer, WoTShareSpecId(4), 0, 80);
	Rig.Pump();
	TestEqual(TEXT("a well-known port is never connected to"), Rig.Net->StreamsCreated, 0);
	*Rig.Clock += 1.0;
	Rig.Request(Peer, WoTShareSpecId(5), static_cast<uint64>(Rig.File->Bytes.Num()));
	Rig.Pump();
	TestEqual(TEXT("an offset at the end is refused"), Rig.Net->StreamsCreated, 0);

	*Rig.Clock += 1.0;
	Rig.Request(Peer, WoTShareSpecId(6));
	Rig.Request(Peer, WoTShareSpecId(7));
	Rig.Pump();
	TestEqual(TEXT("one REQUEST per second per source"), Rig.Net->StreamsCreated, 1);
	*Rig.Clock += 0.5;
	Rig.Request(Peer, WoTShareSpecId(8));
	Rig.Pump();
	TestEqual(TEXT("still one within the second"), Rig.Net->StreamsCreated, 1);
	*Rig.Clock += 1.0;
	Rig.Request(Peer, WoTShareSpecId(6));
	Rig.Pump();
	TestEqual(TEXT("a token works only once"), Rig.Net->StreamsCreated, 1);
	*Rig.Clock += 1.0;
	Rig.Request(Peer, WoTShareSpecId(9));
	Rig.Pump();
	TestEqual(TEXT("after a second the same source is heard again"),
		Rig.Net->StreamsCreated, 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareTransferLimitSpec,
	"WoT.InstallerShare.Sharer.TransferLimit", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareTransferLimitSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	Rig.Net->NextConnect = EWoTShareIo::WouldBlock;
	Rig.Make();
	Rig.Pump();
	Rig.Request(WoTShareSpecIp(192, 168, 4, 21), WoTShareSpecId(10));
	Rig.Request(WoTShareSpecIp(192, 168, 4, 22), WoTShareSpecId(20));
	Rig.Request(WoTShareSpecIp(192, 168, 4, 23), WoTShareSpecId(30));
	Rig.Pump();
	TestEqual(TEXT("at most two transfers at once"), Rig.Sharer->ActiveTransferCount(), 2);
	TestEqual(TEXT("the third REQUEST opens nothing"), Rig.Net->StreamsCreated, 2);
	// The first connects and completes; its slot frees for the next REQUEST.
	Rig.Net->Streams[0]->ConnectResult = EWoTShareIo::Done;
	for (int32 Step = 0; Step < 10 && Rig.Sharer->ActiveTransferCount() > 1; ++Step)
	{
		Rig.Pump();
	}
	TestEqual(TEXT("one slot frees"), Rig.Sharer->ActiveTransferCount(), 1);
	*Rig.Clock += 1.0;
	Rig.Request(WoTShareSpecIp(192, 168, 4, 23), WoTShareSpecId(31));
	Rig.Pump();
	TestEqual(TEXT("a freed slot takes a new transfer"), Rig.Net->StreamsCreated, 3);
	// A receiver that never accepts the connection is given up after 5 s.
	*Rig.Clock += 5.0;
	Rig.Pump();
	TestEqual(TEXT("connects time out after 5 s"), Rig.Sharer->ActiveTransferCount(), 0);
	TestEqual(TEXT("a failed connect shows no notice"),
		Rig.CountEvents(FWoTInstallerSharerEvent::EKind::SendStarted), 1);
	const FWoTShareSpecBeat* End = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("but is recorded"),
		End != nullptr && End->Detail.Contains(TEXT("result=connect-failed")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareStallSpec,
	"WoT.InstallerShare.Sharer.Stall", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareStallSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	// A receiver that reads 1000 bytes and then stops (a Deck gone to sleep).
	Rig.Net->NextBlockAfterBytes = 1000;
	Rig.Make();
	Rig.Pump();
	Rig.Request(WoTShareSpecIp(172, 16, 0, 5), WoTShareSpecId(50));
	Rig.Pump();
	if (Rig.Net->Streams.Num() != 1)
	{
		AddError(TEXT("no stream"));
		return false;
	}
	const TSharedRef<FWoTShareSpecStream> Stream = Rig.Net->Streams[0];
	TestEqual(TEXT("the first 1000 bytes move"), Stream->Received.Num(), 1000);
	*Rig.Clock += 10.0;
	Rig.Pump();
	TestEqual(TEXT("10 s without progress is still a transfer"),
		Rig.Sharer->ActiveTransferCount(), 1);
	// More progress resets the timer.
	Stream->BlockAfterBytes = 2000;
	Rig.Pump();
	TestEqual(TEXT("progress resumes"), Stream->Received.Num(), 2000);
	*Rig.Clock += 14.0;
	Rig.Pump();
	TestEqual(TEXT("14 s without progress is still a transfer"),
		Rig.Sharer->ActiveTransferCount(), 1);
	*Rig.Clock += 1.5;
	Rig.Pump();
	TestEqual(TEXT("15 s without progress drops it"), Rig.Sharer->ActiveTransferCount(), 0);
	TestTrue(TEXT("its socket is closed"), Stream->bClosed);
	bool bStoppedNotice = false;
	for (const FWoTInstallerSharerEvent& Event : *Rig.Events)
	{
		bStoppedNotice = bStoppedNotice
			|| (Event.Kind == FWoTInstallerSharerEvent::EKind::SendEnded
				&& Event.Result == EWoTShareTransferResult::Stalled
				&& UWoTInstallerSharingSubsystem::NoticeText(Event).EndsWith(TEXT("... stopped")));
	}
	TestTrue(TEXT("the notice ends with ... stopped"), bStoppedNotice);
	const FWoTShareSpecBeat* End = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("recorded as stalled"),
		End != nullptr && End->Detail.Contains(TEXT("result=stalled")));
	TestFalse(TEXT("a stall does not reject the file"), Rig.Sharer->IsInstallerRejected());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareBadHashSpec,
	"WoT.InstallerShare.Sharer.BadHash", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareBadHashSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	// Same size, different bytes: Inspect alone would call it a Candidate.
	Rig.Pin = Sha256(WoTShareSpecPayload(Rig.File->Bytes.Num(), 1));
	Rig.Make();
	Rig.Pump();
	Rig.Request(WoTShareSpecIp(192, 168, 1, 50), WoTShareSpecId(60));
	for (int32 Step = 0; Step < 12; ++Step)
	{
		Rig.Pump();
	}
	TestTrue(TEXT("a wrong hash while streaming rejects the file"),
		Rig.Sharer->IsInstallerRejected());
	const FWoTShareSpecBeat* End = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("recorded as bad-hash"), End != nullptr
		&& End->Detail.Contains(TEXT("result=bad-hash"))
		&& End->Detail.Contains(TEXT("hash=bad")));
	TestTrue(TEXT("the offer socket is closed"), Rig.Net->DatagramCloses >= 1);
	const int32 SentBefore = Rig.Net->Sent.Num();
	for (int32 Second = 0; Second < 10; ++Second)
	{
		*Rig.Clock += 2.0;
		Rig.Pump();
	}
	TestEqual(TEXT("no more offers for the rest of the session"),
		Rig.Net->Sent.Num(), SentBefore);
	*Rig.Clock += 61.0;
	Rig.Pump();
	TestEqual(TEXT("and no new search for that file"), Rig.File->Finds, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareStopSpec,
	"WoT.InstallerShare.Sharer.StopEndsTransfer", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareStopSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	Rig.Net->bNextBlocked = true;
	Rig.Make();
	Rig.Pump();
	Rig.Request(WoTShareSpecIp(192, 168, 1, 51), WoTShareSpecId(70));
	Rig.Pump();
	if (Rig.Net->Streams.Num() != 1)
	{
		AddError(TEXT("no stream"));
		return false;
	}
	Rig.Pump();
	TestEqual(TEXT("a transfer is under way"), Rig.Sharer->ActiveTransferCount(), 1);
	Rig.Sharer->Shutdown();
	TestEqual(TEXT("stopping ends it"), Rig.Sharer->ActiveTransferCount(), 0);
	TestTrue(TEXT("and closes its socket"), Rig.Net->Streams[0]->bClosed);
	TestTrue(TEXT("and the offer socket"), Rig.Net->DatagramCloses == 1);
	const FWoTShareSpecBeat* End = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("recorded as stopped"),
		End != nullptr && End->Detail.Contains(TEXT("result=stopped")));
	const int32 Sent = Rig.Net->Sent.Num();
	*Rig.Clock += 10.0;
	Rig.Pump(3);
	TestEqual(TEXT("a stopped sharer sends nothing"), Rig.Net->Sent.Num(), Sent);

	// A receiver that hangs up (Cancel) ends the transfer as stopped too.
	FWoTShareSpecRig Hangup;
	Hangup.Net->bNextBlocked = true;
	Hangup.Make();
	Hangup.Pump();
	Hangup.Request(WoTShareSpecIp(192, 168, 1, 52), WoTShareSpecId(71));
	Hangup.Pump();
	if (Hangup.Net->Streams.Num() == 1)
	{
		Hangup.Net->Streams[0]->bBroken = true;
		Hangup.Net->Streams[0]->bBlocked = false;
		Hangup.Pump();
		TestEqual(TEXT("a broken connection ends the transfer"),
			Hangup.Sharer->ActiveTransferCount(), 0);
	}
	// A file that disappears is searched for again, not streamed.
	FWoTShareSpecRig Gone;
	Gone.File->bOpenFails = true;
	Gone.Make();
	Gone.Pump();
	Gone.Request(WoTShareSpecIp(192, 168, 1, 53), WoTShareSpecId(72));
	Gone.Pump();
	TestEqual(TEXT("an unopenable file starts no transfer"), Gone.Net->StreamsCreated, 0);
	TestFalse(TEXT("and is no longer offered"), Gone.Sharer->HasFoundInstaller());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareSendHeartbeatsSpec,
	"WoT.InstallerShare.Sharer.SendHeartbeatsThrottled", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareSendHeartbeatsSpec::RunTest(const FString&)
{
	FWoTShareSpecRig Rig;
	// A receiver whose inbound TCP is blocked: every connect fails, and it
	// sends a new REQUEST every second for good.
	Rig.Net->NextConnect = EWoTShareIo::Failed;
	Rig.Make();
	Rig.Pump();
	const uint32 Deck = WoTShareSpecIp(192, 168, 4, 127);
	for (int32 Second = 0; Second <= 130; ++Second)
	{
		Rig.Request(Deck, WoTShareSpecId(static_cast<uint8>(Second)));
		Rig.Pump();
		*Rig.Clock += 1.0;
	}
	TestEqual(TEXT("every retry still gets a connection attempt"),
		Rig.Net->StreamsCreated, 131);
	TestEqual(TEXT("send-start: the first, then one a minute"),
		Rig.CountBeats(TEXT("lan-share-send-start")), 3);
	TestEqual(TEXT("each recorded start has its end"),
		Rig.CountBeats(TEXT("lan-share-send-end")), 3);
	const FWoTShareSpecBeat* End = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("recorded as connect-failed"),
		End != nullptr && End->Detail.Contains(TEXT("result=connect-failed")));

	// Another receiver has its own minute.
	Rig.Request(WoTShareSpecIp(192, 168, 4, 30), WoTShareSpecId(200), 0, 40000,
		WoTInstallerShare::LabelMac);
	Rig.Pump();
	TestEqual(TEXT("a second receiver's first transfer is recorded"),
		Rig.CountBeats(TEXT("lan-share-send-start")), 4);

	// Inside the Deck's minute, its connection finally works: a finished
	// copy is always recorded, even when its start was not.
	*Rig.Clock += 1.0;
	Rig.Net->NextConnect = EWoTShareIo::Done;
	Rig.Request(Deck, WoTShareSpecId(201));
	for (int32 Step = 0; Step < 10; ++Step)
	{
		Rig.Pump();
	}
	TestEqual(TEXT("its start is still held back"),
		Rig.CountBeats(TEXT("lan-share-send-start")), 4);
	const FWoTShareSpecBeat* Sent = Rig.LastBeat(TEXT("lan-share-send-end"));
	TestTrue(TEXT("but its end is written, as sent"), Sent != nullptr
		&& Sent->Detail.Contains(TEXT("result=sent"))
		&& Sent->Detail.Contains(TEXT("peer=steam-deck"))
		&& Sent->Detail.Contains(TEXT("hash=good")));
	return !HasAnyErrors();
}

namespace
{
class FWoTInstallerShareFinderFixture final
{
public:
	FWoTInstallerShareFinderFixture()
	{
		Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(), TEXT("wot-installer-share-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		FPaths::NormalizeDirectoryName(Root);
		Pins.Name = WoT::Bootstrap::InstallerName;
		Pins.Size = 8;
		Pins.Sha256 = Sha256(WoTShareSpecAscii("abcdefgh"));
		for (const TCHAR* Leaf : {TEXT("adjacent"), TEXT("state"),
				TEXT("remembered"), TEXT("downloads")})
		{
			bReady = IFileManager::Get().MakeDirectory(
				*FPaths::Combine(Root, Leaf), true) && bReady;
		}
		bReady = IFileManager::Get().MakeDirectory(*FPaths::Combine(Root,
			TEXT("state"), WoTReceivedInstallerDirectoryLeaf), true) && bReady;
		AdjacentPath = FPaths::Combine(Root, TEXT("adjacent"), Pins.Name);
		RememberedPath = FPaths::Combine(Root, TEXT("remembered"),
			TEXT("my installer copy.exe"));
		ReceivedPath = WoTReceivedInstallerPathUnder(FPaths::Combine(Root, TEXT("state")));
		DownloadsPath = FPaths::Combine(Root, TEXT("downloads"),
			TEXT("setup_the_wheel_of_time_333b_(54846) (1).exe"));
	}

	~FWoTInstallerShareFinderFixture()
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}

	/** Pinned size, wrong bytes: anything found was found without hashing. */
	bool Put(const FString& Path) const
	{
		return FFileHelper::SaveArrayToFile(WoTShareSpecAscii("ABCDEFGH"), *Path);
	}

	bool Remember() const
	{
		TWoTBootstrapResult<FString> Canonical =
			WoTCanonicalizeInstallerPickPath(RememberedPath);
		return !Canonical.IsError() && !WoTWriteRememberedInstaller(
			FPaths::Combine(Root, TEXT("state")), nullptr,
			Canonical.GetValue()).IsError();
	}

	FWoTLocalInstallerSearch Search() const
	{
		FWoTLocalInstallerSearch Result;
		FWoTInstallerLocation Adjacent;
		Adjacent.CanonicalPath = AdjacentPath;
		Result.Adjacent = Adjacent;
		Result.UserStateRoot = FPaths::Combine(Root, TEXT("state"));
		Result.DownloadsDirectory = FPaths::Combine(Root, TEXT("downloads"));
		Result.PinsForTesting = Pins;
		return Result;
	}

	FString Root;
	FWoTInstallerAcquisitionPins Pins;
	FString AdjacentPath;
	FString RememberedPath;
	FString ReceivedPath;
	FString DownloadsPath;
	bool bReady = true;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareFinderSpec,
	"WoT.InstallerShare.Finder", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareFinderSpec::RunTest(const FString&)
{
	FWoTInstallerShareFinderFixture Fixture;
	if (!TestTrue(TEXT("fixture folders exist"), Fixture.bReady))
	{
		return false;
	}
	TestTrue(TEXT("fixture files are written"), Fixture.Put(Fixture.AdjacentPath)
		&& Fixture.Put(Fixture.RememberedPath) && Fixture.Put(Fixture.ReceivedPath)
		&& Fixture.Put(Fixture.DownloadsPath) && Fixture.Remember());
	auto Expect = [this, &Fixture](const TCHAR* What,
		TOptional<EWoTSharingInstallerSource> Source, const FString& Path,
		const FWoTLocalInstallerSearch* Override = nullptr)
	{
		const TOptional<FWoTLocalInstallerForSharing> Found =
			WoTFindLocalInstallerForSharing(Override ? *Override : Fixture.Search());
		if (!Source.IsSet())
		{
			TestFalse(What, Found.IsSet());
			return;
		}
		TestTrue(What, Found.IsSet() && Found->Source == Source.GetValue()
			&& FPaths::IsSamePath(Found->Location.CanonicalPath, Path)
			&& Found->Identity.Size == 8);
	};
	Expect(TEXT("adjacent comes first, found without hashing"),
		EWoTSharingInstallerSource::Adjacent, Fixture.AdjacentPath);

	// Opening a found installer reads its bytes through a held handle.
	if (const TOptional<FWoTLocalInstallerForSharing> Found =
			WoTFindLocalInstallerForSharing(Fixture.Search()))
	{
		const TSharedRef<IWoTInstallerShareSource> Source =
			WoTCreateLocalInstallerShareSource(Fixture.Search());
		TestEqual(TEXT("the production source names where it found it"),
			Source->Find(), FString(TEXT("adjacent")));
		TUniquePtr<IWoTInstallerShareReader> Reader = Source->Open();
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(16);
		const TWoTBootstrapResult<uint64> Read = Reader.IsValid()
			? Reader->ReadAt(2, Bytes) : TWoTBootstrapResult<uint64>::Success(0);
		TestTrue(TEXT("the held reader reads from an offset"), Reader.IsValid()
			&& !Read.IsError() && Read.GetValue() == 6
			&& FMemory::Memcmp(Bytes.GetData(), "CDEFGH", 6) == 0);
		TestTrue(TEXT("an untouched file is unchanged"),
			Reader.IsValid() && Reader->IsUnchanged());
		Reader.Reset();
		TestFalse(TEXT("a direct open succeeds"),
			WoTOpenLocalInstallerForSharing(Found.GetValue()).IsError());
	}

	TestTrue(TEXT("remove adjacent"), IFileManager::Get().Delete(*Fixture.AdjacentPath));
	Expect(TEXT("then the remembered Browse pick"),
		EWoTSharingInstallerSource::Remembered, Fixture.RememberedPath);
	FWoTLocalInstallerSearch SkipRemembered = Fixture.Search();
	SkipRemembered.SkipRememberedUnder = FPaths::Combine(Fixture.Root, TEXT("remembered"));
	Expect(TEXT("a remembered pick under the skipped folder is not searched"),
		EWoTSharingInstallerSource::Received, Fixture.ReceivedPath, &SkipRemembered);
	TestTrue(TEXT("remove remembered"), IFileManager::Get().Delete(*Fixture.RememberedPath));
	Expect(TEXT("then the received copy"),
		EWoTSharingInstallerSource::Received, Fixture.ReceivedPath);
	TestTrue(TEXT("remove received"), IFileManager::Get().Delete(*Fixture.ReceivedPath));
	Expect(TEXT("then Downloads, including a browser duplicate name"),
		EWoTSharingInstallerSource::Downloads, Fixture.DownloadsPath);
	FWoTLocalInstallerSearch NoDownloads = Fixture.Search();
	NoDownloads.DownloadsDirectory.Reset();
	Expect(TEXT("an empty Downloads folder setting skips Downloads (the Mac rule)"),
		TOptional<EWoTSharingInstallerSource>(), FString(), &NoDownloads);
	TestTrue(TEXT("a partial download is written"), FFileHelper::SaveArrayToFile(
		WoTShareSpecAscii("ABCD"), *Fixture.DownloadsPath));
	Expect(TEXT("a partial download is not offered"),
		TOptional<EWoTSharingInstallerSource>(), FString());
	TestTrue(TEXT("remove downloads"), IFileManager::Get().Delete(*Fixture.DownloadsPath));
	Expect(TEXT("nothing anywhere finds nothing"),
		TOptional<EWoTSharingInstallerSource>(), FString());
	TestTrue(TEXT("the finder created nothing"),
		!IFileManager::Get().FileExists(*Fixture.AdjacentPath)
		&& !IFileManager::Get().FileExists(*Fixture.ReceivedPath));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareReceivedPathSpec,
	"WoT.InstallerShare.ReceivedPath", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareReceivedPathSpec::RunTest(const FString&)
{
	const FString Root = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("received-root")));
	FString Expected = FPaths::Combine(Root, TEXT("OriginalGame"),
		WoT::Bootstrap::InstallerName);
	FPaths::NormalizeFilename(Expected);
	TestEqual(TEXT("<user data root>/OriginalGame/<pinned name>"),
		WoTReceivedInstallerPathUnder(Root), Expected);
	TestTrue(TEXT("no root, no path"), WoTReceivedInstallerPathUnder(FString()).IsEmpty());
	TestTrue(TEXT("a relative root is refused"),
		WoTReceivedInstallerPathUnder(TEXT("relative/root")).IsEmpty());
	return !HasAnyErrors();
}

namespace
{
/** Everything the spec presenter was asked to draw. */
struct FWoTShareSpecNoticeLog
{
	/** Every Show, in order. */
	TArray<FString> Shown;
	int32 Hides = 0;
	/** The widget the spec presenter draws, while it is shown. */
	TSharedPtr<SWoTSharingNotice> Widget;

	bool IsVisible() const { return Widget.IsValid(); }
	FString VisibleText() const
	{
		return Widget.IsValid() ? Widget->GetTextForTesting().ToString() : FString();
	}
};

/** Draws the real notice widget, just not into a viewport. */
class FWoTShareSpecPresenter final : public IWoTSharingNoticePresenter
{
public:
	explicit FWoTShareSpecPresenter(TSharedRef<FWoTShareSpecNoticeLog> InLog)
		: Log(MoveTemp(InLog))
	{
	}
	virtual void Show(const FText& Text) override
	{
		Log->Shown.Add(Text.ToString());
		if (!Log->Widget.IsValid())
		{
			Log->Widget = SNew(SWoTSharingNotice);
		}
		Log->Widget->SetText(Text);
	}
	virtual void Hide() override
	{
		++Log->Hides;
		Log->Widget.Reset();
	}

private:
	TSharedRef<FWoTShareSpecNoticeLog> Log;
};

/** A sharing subsystem in inline mode over a fresh rig. The notice's hide
 * timer runs on the rig's clock. */
struct FWoTInstallerShareSubsystemRig
{
	FWoTShareSpecRig Share;
	TSharedRef<bool> bReady = MakeShared<bool>(true);
	TSharedRef<bool> bHeadsUp = MakeShared<bool>(false);
	TSharedRef<FWoTShareSpecNoticeLog> Notice = MakeShared<FWoTShareSpecNoticeLog>();
	UGameInstance* Instance = nullptr;
	UWoTInstallerSharingSubsystem* Sharing = nullptr;

	FWoTInstallerShareSubsystemRig()
	{
		// A game-instance subsystem lives in a game instance (never
		// initialized here: no world, no local player, no viewport).
		Instance = NewObject<UGameInstance>();
		Instance->AddToRoot();
		Sharing = NewObject<UWoTInstallerSharingSubsystem>(Instance);
		Sharing->AddToRoot();
	}

	~FWoTInstallerShareSubsystemRig()
	{
		Sharing->ApplySharingSwitch(false);
		Sharing->RemoveFromRoot();
		Instance->RemoveFromRoot();
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({});
	}

	void Configure(bool bSwitchOn)
	{
		FWoTInstallerSharingTestConfig Config;
		Config.IsBootstrapReady = [Ready = bReady]() { return *Ready; };
		Config.HasSeenLocalNetworkHeadsUp = [Seen = bHeadsUp]() { return *Seen; };
		Config.SharerConfig = Share.Config();
		Config.MakeDependencies = [this]() { return Share.Dependencies(); };
		Config.MakeNoticePresenter = [Log = Notice]()
		{
			return TUniquePtr<IWoTSharingNoticePresenter>(
				MakeUnique<FWoTShareSpecPresenter>(Log));
		};
		Config.NoticeNowSeconds = [ClockValue = Share.Clock]() { return *ClockValue; };
		Sharing->ConfigureForTesting(MoveTemp(Config), bSwitchOn);
	}

	/** Configured, sharing and offering, with nothing on screen yet. */
	bool Start()
	{
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
		Configure(true);
		Sharing->EvaluateForTesting();
		Sharing->PumpForTesting();
		return Sharing->InlineSharerForTesting() != nullptr;
	}

	/** A REQUEST from From, as a receiver sends it. */
	void Request(uint32 From, uint8 TokenSeed,
		const TCHAR* Label = WoTInstallerShare::LabelSteamDeck)
	{
		FWoTShareRequest Message;
		Message.SessionId = Sharing->InlineSharerForTesting()->SessionId();
		Message.Token = WoTShareSpecId(TokenSeed);
		Message.TcpPort = 45000;
		Message.Label = Label;
		Share.Net->Inbox.Add({From, 50000, WoTInstallerShare::EncodeRequest(Message)});
	}

	/** Moves the clock, then one ticker step (the hide timer). */
	void Advance(double Seconds)
	{
		*Share.Clock += Seconds;
		Sharing->TickForTesting();
	}
};

const TCHAR* const WoTShareSpecSendingDeck =
	TEXT("Sending the original game installer to a Steam Deck...");
const TCHAR* const WoTShareSpecSendingMac =
	TEXT("Sending the original game installer to a Mac...");

class FWoTShareSpecFeedback final : public IWoTFrontEndFeedbackSink
{
public:
	virtual void MoveSelector() override {}
	virtual void Select() override {}
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareMacGateSpec,
	"WoT.InstallerShare.MacGate", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareMacGateSpec::RunTest(const FString&)
{
	{
		FWoTInstallerShareSubsystemRig Rig;
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
		Rig.Configure(true);
		Rig.Sharing->EvaluateForTesting();
		Rig.Sharing->PumpForTesting();
		TestTrue(TEXT("asks = false, marker unset: the sharer runs"),
			Rig.Sharing->IsSharerRunning());
		TestTrue(TEXT("and offers"), Rig.Share.Net->DatagramsCreated == 1
			&& Rig.Share.Net->Sent.Num() > 0);
		TestEqual(TEXT("its search result reaches the subsystem"),
			static_cast<int32>(Rig.Sharing->GetSearchState()),
			static_cast<int32>(EWoTInstallerSharingSearch::Found));
	}
	{
		FWoTInstallerShareSubsystemRig Rig;
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(true);
		Rig.Configure(true);
		Rig.Sharing->EvaluateForTesting();
		Rig.Sharing->PumpForTesting();
		TestFalse(TEXT("asks = true, marker unset: no sharer"),
			Rig.Sharing->IsSharerRunning());
		TestEqual(TEXT("and no socket opens"), Rig.Share.Net->DatagramsCreated
			+ Rig.Share.Net->StreamsCreated, 0);
		TestEqual(TEXT("and nothing is searched"), Rig.Share.File->Finds, 0);
		*Rig.bHeadsUp = true;
		Rig.Sharing->EvaluateForTesting();
		Rig.Sharing->PumpForTesting();
		TestTrue(TEXT("after the heads-up the Mac shares"),
			Rig.Sharing->IsSharerRunning() && Rig.Share.Net->Sent.Num() > 0);
	}
	{
		FWoTInstallerShareSubsystemRig Rig;
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
		*Rig.bReady = false;
		Rig.Configure(true);
		Rig.Sharing->EvaluateForTesting();
		TestFalse(TEXT("nothing runs before the bootstrap is Ready"),
			Rig.Sharing->IsSharerRunning());
		*Rig.bReady = true;
		Rig.Configure(false);
		Rig.Sharing->EvaluateForTesting();
		TestFalse(TEXT("nothing runs while the switch is off"),
			Rig.Sharing->IsSharerRunning());
		TestEqual(TEXT("no socket either way"), Rig.Share.Net->DatagramsCreated, 0);
	}
	TestTrue(TEXT("the start rule"),
		UWoTInstallerSharingSubsystem::ShouldShare(true, true, false, false)
		&& !UWoTInstallerSharingSubsystem::ShouldShare(true, true, true, false)
		&& UWoTInstallerSharingSubsystem::ShouldShare(true, true, true, true)
		&& !UWoTInstallerSharingSubsystem::ShouldShare(false, true, false, true)
		&& !UWoTInstallerSharingSubsystem::ShouldShare(true, false, false, true));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareSwitchStopsSpec,
	"WoT.InstallerShare.SwitchStopsTransfer", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareSwitchStopsSpec::RunTest(const FString&)
{
	FWoTInstallerShareSubsystemRig Rig;
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	Rig.Share.Net->bNextBlocked = true;
	Rig.Configure(true);
	Rig.Sharing->EvaluateForTesting();
	Rig.Sharing->PumpForTesting();
	FWoTInstallerSharer* Sharer = Rig.Sharing->InlineSharerForTesting();
	if (!TestNotNull(TEXT("an inline sharer runs"), Sharer))
	{
		return false;
	}
	FWoTShareRequest Request;
	Request.SessionId = Sharer->SessionId();
	Request.Token = WoTShareSpecId(80);
	Request.TcpPort = 45000;
	Request.Label = WoTInstallerShare::LabelSteamDeck;
	Rig.Share.Net->Inbox.Add({WoTShareSpecIp(192, 168, 4, 127), 50000,
		WoTInstallerShare::EncodeRequest(Request)});
	Rig.Sharing->PumpForTesting();
	if (Rig.Share.Net->Streams.Num() != 1)
	{
		AddError(TEXT("no stream"));
		return false;
	}
	Rig.Sharing->PumpForTesting();
	TestEqual(TEXT("the player sees the sending notice"), Rig.Notice->VisibleText(),
		FString(WoTShareSpecSendingDeck));
	Rig.Sharing->ApplySharingSwitch(false);
	TestFalse(TEXT("switching sharing off stops the sharer"),
		Rig.Sharing->IsSharerRunning());
	TestTrue(TEXT("and the transfer's connection"), Rig.Share.Net->Streams[0]->bClosed);
	TestFalse(TEXT("and hides the notice at once"), Rig.Notice->IsVisible());
	TestTrue(TEXT("without flashing a stopped line"), Rig.Notice->Shown.Num() == 1
		&& Rig.Notice->Shown[0] == WoTShareSpecSendingDeck);
	TestTrue(TEXT("the notice forgets the transfer"),
		Rig.Sharing->NoticeLineForTesting() != nullptr
		&& !Rig.Sharing->NoticeLineForTesting()->IsShown()
		&& Rig.Sharing->NoticeLineForTesting()->SendingCount() == 0);
	Rig.Sharing->ApplySharingSwitch(true);
	TestTrue(TEXT("switching it back on shares again"), Rig.Sharing->IsSharerRunning());
	TestFalse(TEXT("with nothing on screen until a transfer starts"),
		Rig.Notice->IsVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNoticeSendingSpec,
	"WoT.InstallerShare.Notice.SendingShows", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNoticeSendingSpec::RunTest(const FString&)
{
	FWoTInstallerShareSubsystemRig Rig;
	Rig.Share.Net->bNextBlocked = true;
	if (!TestTrue(TEXT("the sharer runs"), Rig.Start()))
	{
		return false;
	}
	TestFalse(TEXT("nothing shows before a transfer"), Rig.Notice->IsVisible());
	Rig.Request(WoTShareSpecIp(192, 168, 4, 127), 90);
	Rig.Sharing->PumpForTesting();
	TestTrue(TEXT("a connected transfer shows the notice"), Rig.Notice->IsVisible());
	TestEqual(TEXT("with the sending line"), Rig.Notice->VisibleText(),
		FString(WoTShareSpecSendingDeck));
	// It stays while the transfer is sending, however long that takes.
	Rig.Advance(10.0);
	TestTrue(TEXT("a sending line has no timeout"), Rig.Notice->IsVisible());
	if (Rig.Notice->Widget.IsValid())
	{
		const TSharedRef<SWoTSharingNotice> Widget = Rig.Notice->Widget.ToSharedRef();
		TestTrue(TEXT("the widget never takes a click"),
			Widget->GetVisibility() == EVisibility::HitTestInvisible);
		TestFalse(TEXT("nor focus"), Widget->SupportsKeyboardFocus());
		const TSharedPtr<STextBlock> Text = Widget->TextBlockForTesting();
		TestTrue(TEXT("its font meets the Deck legibility floor"), Text.IsValid()
			&& Text->GetFont().Size >= 10.0f);
	}
	TestTrue(TEXT("its viewport layer is above every game layer"),
		WoTSharingNoticeZOrder > 2200);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNoticeSentSpec,
	"WoT.InstallerShare.Notice.SentHidesAfterSixSeconds", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNoticeSentSpec::RunTest(const FString&)
{
	FWoTInstallerShareSubsystemRig Rig;
	Rig.Share.Net->bNextBlocked = true;
	if (!TestTrue(TEXT("the sharer runs"), Rig.Start()))
	{
		return false;
	}
	Rig.Request(WoTShareSpecIp(192, 168, 4, 127), 91);
	Rig.Sharing->PumpForTesting();
	if (Rig.Share.Net->Streams.Num() != 1)
	{
		AddError(TEXT("no stream"));
		return false;
	}
	Rig.Share.Net->Streams[0]->bBlocked = false;
	for (int32 Step = 0; Step < 10
		&& Rig.Sharing->InlineSharerForTesting()->ActiveTransferCount() > 0; ++Step)
	{
		Rig.Sharing->PumpForTesting();
	}
	TestEqual(TEXT("the copy finishes with ... sent"), Rig.Notice->VisibleText(),
		FString(WoTShareSpecSendingDeck) + TEXT(" sent"));
	Rig.Advance(5.9);
	TestTrue(TEXT("the sent line stays for 6 s"), Rig.Notice->IsVisible());
	Rig.Advance(0.2);
	TestFalse(TEXT("then hides"), Rig.Notice->IsVisible());
	TestEqual(TEXT("hidden once"), Rig.Notice->Hides, 1);
	Rig.Advance(30.0);
	TestFalse(TEXT("and stays hidden"), Rig.Notice->IsVisible());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNoticeStoppedSpec,
	"WoT.InstallerShare.Notice.StoppedHidesAfterSixSeconds", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNoticeStoppedSpec::RunTest(const FString&)
{
	FWoTInstallerShareSubsystemRig Rig;
	Rig.Share.Net->bNextBlocked = true;
	if (!TestTrue(TEXT("the sharer runs"), Rig.Start()))
	{
		return false;
	}
	Rig.Request(WoTShareSpecIp(192, 168, 4, 127), 92);
	Rig.Sharing->PumpForTesting();
	if (Rig.Share.Net->Streams.Num() != 1)
	{
		AddError(TEXT("no stream"));
		return false;
	}
	// The receiver hangs up (its Cancel).
	Rig.Share.Net->Streams[0]->bBlocked = false;
	Rig.Share.Net->Streams[0]->bBroken = true;
	Rig.Sharing->PumpForTesting();
	TestEqual(TEXT("the transfer ended"),
		Rig.Sharing->InlineSharerForTesting()->ActiveTransferCount(), 0);
	TestEqual(TEXT("the notice ends with ... stopped"), Rig.Notice->VisibleText(),
		FString(WoTShareSpecSendingDeck) + TEXT(" stopped"));
	Rig.Advance(5.9);
	TestTrue(TEXT("the stopped line stays for 6 s"), Rig.Notice->IsVisible());
	Rig.Advance(0.2);
	TestFalse(TEXT("then hides"), Rig.Notice->IsVisible());
	TestTrue(TEXT("sharing itself goes on"), Rig.Sharing->IsSharerRunning());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNoticeTwoTransfersSpec,
	"WoT.InstallerShare.Notice.TwoTransfers", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNoticeTwoTransfersSpec::RunTest(const FString&)
{
	FWoTInstallerShareSubsystemRig Rig;
	Rig.Share.Net->bNextBlocked = true;
	if (!TestTrue(TEXT("the sharer runs"), Rig.Start()))
	{
		return false;
	}
	Rig.Request(WoTShareSpecIp(192, 168, 4, 127), 93);
	Rig.Request(WoTShareSpecIp(192, 168, 4, 30), 94, WoTInstallerShare::LabelMac);
	Rig.Sharing->PumpForTesting();
	if (Rig.Share.Net->Streams.Num() != 2)
	{
		AddError(TEXT("two streams expected"));
		return false;
	}
	TestEqual(TEXT("two transfers are sending"),
		Rig.Sharing->NoticeLineForTesting() != nullptr
			? Rig.Sharing->NoticeLineForTesting()->SendingCount() : -1, 2);
	TestEqual(TEXT("the newest line shows"), Rig.Notice->VisibleText(),
		FString(WoTShareSpecSendingMac));
	// The Deck's copy finishes; the Mac's is still sending.
	Rig.Share.Net->Streams[0]->bBlocked = false;
	for (int32 Step = 0; Step < 10
		&& Rig.Sharing->InlineSharerForTesting()->ActiveTransferCount() > 1; ++Step)
	{
		Rig.Sharing->PumpForTesting();
	}
	TestEqual(TEXT("the newest line is the Deck's sent"), Rig.Notice->VisibleText(),
		FString(WoTShareSpecSendingDeck) + TEXT(" sent"));
	Rig.Advance(7.0);
	TestTrue(TEXT("the notice stays while the Mac's copy is sending"),
		Rig.Notice->IsVisible());
	Rig.Share.Net->Streams[1]->bBlocked = false;
	for (int32 Step = 0; Step < 10
		&& Rig.Sharing->InlineSharerForTesting()->ActiveTransferCount() > 0; ++Step)
	{
		Rig.Sharing->PumpForTesting();
	}
	TestEqual(TEXT("then shows the Mac's sent"), Rig.Notice->VisibleText(),
		FString(WoTShareSpecSendingMac) + TEXT(" sent"));
	Rig.Advance(5.9);
	TestTrue(TEXT("for 6 s after the last transfer ends"), Rig.Notice->IsVisible());
	Rig.Advance(0.2);
	TestFalse(TEXT("then hides"), Rig.Notice->IsVisible());
	TestEqual(TEXT("one widget the whole time"), Rig.Notice->Hides, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNoticeFrontEndSpec,
	"WoT.InstallerShare.Notice.FrontEndStatusUntouched", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNoticeFrontEndSpec::RunTest(const FString&)
{
	// The notice is its own viewport widget: the front end's status line
	// (and the main-menu banner it feeds) is never written by sharing.
	for (const TCHAR* Leaf : {TEXT("WoTFrontEndSlate.cpp"), TEXT("WoTFrontEndSlate.h")})
	{
		FString Source;
		if (TestTrue(FString::Printf(TEXT("%s is readable"), Leaf),
				FFileHelper::LoadFileToString(Source, *FPaths::Combine(
					FPaths::ProjectDir(), TEXT("Source/WoT"), Leaf))))
		{
			TestFalse(FString::Printf(TEXT("%s has no sharing route"), Leaf),
				Source.Contains(TEXT("InstallerSharing"), ESearchCase::CaseSensitive)
				|| Source.Contains(TEXT("SharingNotice"), ESearchCase::CaseSensitive));
		}
	}

	TMap<FName, FString> Labels;
	Labels.Add(FName(TEXT("menuMain.MenuList[2]")), TEXT("Multiplayer"));
	Labels.Add(FName(TEXT("menuMain.MenuList[7]")), TEXT("Quit"));
	const TSharedRef<SWoTFrontEndRoot> Root = SNew(SWoTFrontEndRoot)
		.PlayerController(nullptr)
		.PresentationAuthorityForTesting(FWoTFrontEndPresentationAuthority::CreateForTesting(
			TEXT("installer-share-notice-fixture"),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 30),
			FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14),
			MoveTemp(Labels), MakeShared<FWoTShareSpecFeedback>()));
	const FString StatusBefore = Root->ViewModelForTesting().GetStatusText();
	const bool bErrorBefore = Root->ViewModelForTesting().IsStatusError();

	FWoTInstallerShareSubsystemRig Rig;
	if (!TestTrue(TEXT("the sharer runs"), Rig.Start()))
	{
		return false;
	}
	Rig.Request(WoTShareSpecIp(192, 168, 4, 127), 95);
	for (int32 Step = 0; Step < 10; ++Step)
	{
		Rig.Sharing->PumpForTesting();
	}
	TestTrue(TEXT("a whole transfer showed its lines"), Rig.Notice->Shown.Num() == 2
		&& Rig.Notice->Shown[1] == FString(WoTShareSpecSendingDeck) + TEXT(" sent"));
	TestEqual(TEXT("the front end's status text is untouched"),
		Root->ViewModelForTesting().GetStatusText(), StatusBefore);
	TestEqual(TEXT("and so is its error state"),
		Root->ViewModelForTesting().IsStatusError(), bErrorBefore);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareNoticeNoViewportSpec,
	"WoT.InstallerShare.Notice.NoGameViewport", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareNoticeNoViewportSpec::RunTest(const FString&)
{
	// A game instance with no world context has no game viewport (a dedicated
	// server, or early boot before the window exists).
	UGameInstance* Instance = NewObject<UGameInstance>();
	Instance->AddToRoot();
	ON_SCOPE_EXIT
	{
		Instance->RemoveFromRoot();
	};
	TestNull(TEXT("no game viewport"), Instance->GetGameViewportClient());
	{
		TUniquePtr<IWoTSharingNoticePresenter> Presenter =
			WoTCreateViewportSharingNoticePresenter(Instance);
		Presenter->Show(FText::FromString(WoTShareSpecSendingDeck));
		Presenter->Show(FText::FromString(FString(WoTShareSpecSendingDeck) + TEXT(" sent")));
		Presenter->Hide();
		Presenter->Hide();
		Presenter->Show(FText::FromString(WoTShareSpecSendingDeck));
		// Destroyed while "showing": nothing to remove.
	}
	{
		TUniquePtr<IWoTSharingNoticePresenter> Orphan =
			WoTCreateViewportSharingNoticePresenter(nullptr);
		Orphan->Show(FText::FromString(WoTShareSpecSendingDeck));
		Orphan->Hide();
	}
	// The subsystem's own presenter (no spec presenter) over the same
	// viewport-less instance: the notice logic runs, nothing is drawn.
	UWoTInstallerSharingSubsystem* Sharing =
		NewObject<UWoTInstallerSharingSubsystem>(Instance);
	Sharing->AddToRoot();
	FWoTShareSpecRig Share;
	WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting(false);
	ON_SCOPE_EXIT
	{
		Sharing->ApplySharingSwitch(false);
		Sharing->RemoveFromRoot();
		WoTLocalNetworkAccess::SetOSAsksForAccessOverrideForTesting({});
	};
	FWoTInstallerSharingTestConfig Config;
	Config.IsBootstrapReady = []() { return true; };
	Config.SharerConfig = Share.Config();
	Config.MakeDependencies = [&Share]() { return Share.Dependencies(); };
	Config.NoticeNowSeconds = [ClockValue = Share.Clock]() { return *ClockValue; };
	Sharing->ConfigureForTesting(MoveTemp(Config), true);
	Sharing->EvaluateForTesting();
	Sharing->PumpForTesting();
	FWoTInstallerSharer* Sharer = Sharing->InlineSharerForTesting();
	if (!TestNotNull(TEXT("an inline sharer runs"), Sharer))
	{
		return false;
	}
	FWoTShareRequest Request;
	Request.SessionId = Sharer->SessionId();
	Request.Token = WoTShareSpecId(96);
	Request.TcpPort = 45000;
	Request.Label = WoTInstallerShare::LabelSteamDeck;
	Share.Net->Inbox.Add({WoTShareSpecIp(192, 168, 4, 127), 50000,
		WoTInstallerShare::EncodeRequest(Request)});
	for (int32 Step = 0; Step < 10; ++Step)
	{
		Sharing->PumpForTesting();
	}
	TestTrue(TEXT("the notice line ran"), Sharing->NoticeLineForTesting() != nullptr
		&& Sharing->NoticeLineForTesting()->IsShown());
	*Share.Clock += 7.0;
	Sharing->TickForTesting();
	TestTrue(TEXT("and hid on time"), Sharing->NoticeLineForTesting() != nullptr
		&& !Sharing->NoticeLineForTesting()->IsShown());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareSwitchFileSpec,
	"WoT.InstallerShare.SwitchFile", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareSwitchFileSpec::RunTest(const FString&)
{
	TestTrue(TEXT("the switch encodes to its schema"),
		UWoTInstallerSharingSubsystem::EncodeSharingSwitch(false)
			== WoTShareSpecAscii("Schema=WoTInstallerSharing-v1\noff\n"));
	TestTrue(TEXT("on round-trips"), UWoTInstallerSharingSubsystem::DecodeSharingSwitch(
		UWoTInstallerSharingSubsystem::EncodeSharingSwitch(true)) == TOptional<bool>(true));
	TestTrue(TEXT("CRLF is tolerated"), UWoTInstallerSharingSubsystem::DecodeSharingSwitch(
		WoTShareSpecAscii("Schema=WoTInstallerSharing-v1\r\noff\r\n"))
			== TOptional<bool>(false));
	for (const ANSICHAR* Invalid : {"", "off\n", "Schema=WoTInstallerSharing-v2\noff\n",
		"Schema=WoTInstallerSharing-v1\nOFF\n", "Schema=WoTInstallerSharing-v1\noff\non\n",
		"Schema=WoTInstallerSharing-v1\nmaybe\n"})
	{
		TestFalse(FString::Printf(TEXT("invalid switch file '%s'"), ANSI_TO_TCHAR(Invalid)),
			UWoTInstallerSharingSubsystem::DecodeSharingSwitch(
				WoTShareSpecAscii(Invalid)).IsSet());
	}

	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("wot-sharing-switch-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	ON_SCOPE_EXIT
	{
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	FString SidePath;
	TestTrue(TEXT("the side file has a path"),
		UWoTPreferencesSubsystem::SideFilePath(TEXT("InstallerSharing"), SidePath));
	TestTrue(TEXT("no side file means on"), UWoTInstallerSharingSubsystem::LoadSharingSwitch());
	TestTrue(TEXT("off is saved"), UWoTInstallerSharingSubsystem::SaveSharingSwitch(false));
	TestFalse(TEXT("off reads back"), UWoTInstallerSharingSubsystem::LoadSharingSwitch());
	TArray<uint8> Bytes;
	TestTrue(TEXT("the side file holds the schema and off"),
		FFileHelper::LoadFileToArray(Bytes, *SidePath)
		&& Bytes == WoTShareSpecAscii("Schema=WoTInstallerSharing-v1\noff\n"));
	// A preferences reload: the path is re-resolved and the file re-read.
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
	TestFalse(TEXT("off survives a preferences reload"),
		UWoTInstallerSharingSubsystem::LoadSharingSwitch());
	TestTrue(TEXT("a damaged file is written"),
		FFileHelper::SaveStringToFile(TEXT("garbage"), *SidePath));
	TestTrue(TEXT("an invalid side file means on"),
		UWoTInstallerSharingSubsystem::LoadSharingSwitch());
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
	TestTrue(TEXT("specs never read the player's real switch"),
		UWoTInstallerSharingSubsystem::LoadSharingSwitch());
	TestFalse(TEXT("nor write it"), UWoTInstallerSharingSubsystem::SaveSharingSwitch(false));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerShareLoopbackSpec,
	"WoT.InstallerShare.Loopback", WoTInstallerShareSpecFlags)

bool FWoTInstallerShareLoopbackSpec::RunTest(const FString&)
{
	// The production socket adapter end to end, on loopback only. The spec
	// plays the receiver (it binds and listens on loopback); the sharer never
	// binds and only ever sends to loopback here.
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!TestNotNull(TEXT("socket subsystem"), Sockets))
	{
		return false;
	}
	const uint32 Loopback = WoTShareSpecIp(127, 0, 0, 1);
	auto MakeLoopbackAddress = [Sockets, Loopback](uint16 Port)
	{
		TSharedRef<FInternetAddr> Address =
			Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Address->SetIp(Loopback);
		Address->SetPort(Port);
		return Address;
	};
	FSocket* Udp = Sockets->CreateSocket(NAME_DGram, TEXT("WoTShareSpecReceiver"),
		FNetworkProtocolTypes::IPv4);
	FSocket* Listener = Sockets->CreateSocket(NAME_Stream, TEXT("WoTShareSpecListener"),
		FNetworkProtocolTypes::IPv4);
	FSocket* Connection = nullptr;
	ON_SCOPE_EXIT
	{
		for (FSocket* Socket : {Udp, Listener, Connection})
		{
			if (Socket != nullptr)
			{
				Socket->Close();
				Sockets->DestroySocket(Socket);
			}
		}
	};
	if (!TestTrue(TEXT("loopback receiver sockets"), Udp != nullptr && Listener != nullptr
			&& Udp->Bind(*MakeLoopbackAddress(0)) && Listener->Bind(*MakeLoopbackAddress(0))
			&& Listener->Listen(1) && Listener->SetNonBlocking(true)))
	{
		return false;
	}
	const uint16 UdpPort = static_cast<uint16>(Udp->GetPortNo());
	const uint16 TcpPort = static_cast<uint16>(Listener->GetPortNo());

	TSharedRef<FWoTShareSpecFile> File = MakeShared<FWoTShareSpecFile>();
	File->Bytes = WoTShareSpecPayload(700 * 1024, 3);
	FWoTInstallerSharer::FConfig Config;
	Config.DiscoveryPort = UdpPort;
	Config.Label = WoTInstallerShare::LabelWindowsPC;
	Config.Size = static_cast<uint64>(File->Bytes.Num());
	Config.Sha256 = Sha256(File->Bytes);
	Config.bAllowLoopbackPeers = true;
	Config.DestinationsOverride = {Loopback};
	FWoTInstallerSharer::FDependencies Dependencies;
	Dependencies.Network = WoTCreateProductionInstallerShareNetwork();
	Dependencies.Source = MakeShared<FWoTShareSpecSource>(File);
	TArray<FWoTShareSpecBeat> Beats;
	Dependencies.Heartbeat = [&Beats](const TCHAR* Event, const FString& Detail)
	{
		Beats.Add({Event, Detail});
	};
	FWoTInstallerSharer Sharer(MoveTemp(Config), MoveTemp(Dependencies));

	const FWoTShareId Token = WoTShareSpecId(123);
	bool bRequested = false;
	TArray<uint8> Received;
	const int64 Expected = WoTInstallerShare::DataHeaderBytes + File->Bytes.Num();
	const double Deadline = FPlatformTime::Seconds() + 20.0;
	while (FPlatformTime::Seconds() < Deadline
		&& (Received.Num() < Expected || Sharer.ActiveTransferCount() > 0))
	{
		Sharer.Pump();
		uint32 Pending = 0;
		if (!bRequested && Udp->HasPendingData(Pending) && Pending > 0)
		{
			TArray<uint8> Datagram;
			Datagram.SetNumUninitialized(1024);
			TSharedRef<FInternetAddr> From = Sockets->CreateInternetAddr(
				FNetworkProtocolTypes::IPv4);
			int32 Read = 0;
			if (Udp->RecvFrom(Datagram.GetData(), Datagram.Num(), Read, *From) && Read > 0)
			{
				Datagram.SetNum(Read);
				const TOptional<FWoTShareOffer> Offer = WoTInstallerShare::DecodeOffer(Datagram);
				TestTrue(TEXT("a real OFFER arrives over loopback"), Offer.IsSet()
					&& Offer->Size == static_cast<uint64>(File->Bytes.Num()));
				if (Offer.IsSet())
				{
					FWoTShareRequest Request;
					Request.SessionId = Offer->SessionId;
					Request.Token = Token;
					Request.TcpPort = TcpPort;
					Request.Label = WoTInstallerShare::LabelSteamDeck;
					const TArray<uint8> RequestBytes = WoTInstallerShare::EncodeRequest(Request);
					int32 Sent = 0;
					bRequested = Udp->SendTo(RequestBytes.GetData(), RequestBytes.Num(),
						Sent, *From);
				}
			}
		}
		if (Connection == nullptr)
		{
			bool bPendingConnection = false;
			if (Listener->HasPendingConnection(bPendingConnection) && bPendingConnection)
			{
				Connection = Listener->Accept(TEXT("WoTShareSpecData"));
				if (Connection != nullptr)
				{
					Connection->SetNonBlocking(true);
				}
			}
		}
		if (Connection != nullptr)
		{
			uint8 Buffer[64 * 1024];
			int32 Read = 0;
			while (Connection->Recv(Buffer, static_cast<int32>(sizeof(Buffer)), Read)
				&& Read > 0)
			{
				Received.Append(Buffer, Read);
			}
		}
	}
	TestTrue(TEXT("the REQUEST went out"), bRequested);
	TestEqual(TEXT("every byte arrived"), static_cast<int64>(Received.Num()), Expected);
	if (Received.Num() == Expected)
	{
		const TOptional<FWoTShareDataHeader> Header = WoTInstallerShare::DecodeDataHeader(
			TConstArrayView<uint8>(Received.GetData(), WoTInstallerShare::DataHeaderBytes));
		TestTrue(TEXT("the DATA header carries the token and range"), Header.IsSet()
			&& Header->Token == Token && Header->Offset == 0
			&& Header->Length == static_cast<uint64>(File->Bytes.Num()));
		TestTrue(TEXT("the payload is the file"), FMemory::Memcmp(
			Received.GetData() + WoTInstallerShare::DataHeaderBytes,
			File->Bytes.GetData(), File->Bytes.Num()) == 0);
	}
	bool bSentGood = false;
	for (const FWoTShareSpecBeat& Beat : Beats)
	{
		bSentGood = bSentGood || (Beat.Event == TEXT("lan-share-send-end")
			&& Beat.Detail.Contains(TEXT("result=sent"))
			&& Beat.Detail.Contains(TEXT("hash=good")));
	}
	TestTrue(TEXT("the sharer recorded a good send"), bSentGood);
	Sharer.Shutdown();
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
