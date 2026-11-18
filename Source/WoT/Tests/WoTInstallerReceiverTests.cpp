// LAN installer sharing, the receiving side (spec B1, B3, B5, B6): the loopback
// transfer matrix against the production sockets (through a recording wrapper),
// the trigger rules, the byte-progress stages, and the bootstrap screen's copy
// line and buttons. No spec sends anything beyond loopback.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "../Online/WoTInstallerShareProtocol.h"
#include "../Online/WoTInstallerSharer.h"
#include "../UI/SWoTBootstrapScreen.h"
#include "../Updater/WoTInstallerReceiver.h"
#include "../WoTBootstrapViewModel.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTInstallerReceiverTestSupport.h"

namespace
{
constexpr EAutomationTestFlags WoTReceiverSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
constexpr double WoTReceiverSpecWaitSeconds = 20.0;

struct FWoTReceiverSpecBeat
{
	FString Event;
	FString Detail;
};

/** Heartbeats, from whichever thread wrote them. */
struct FWoTReceiverSpecBeats
{
	void Add(const TCHAR* Event, const FString& Detail)
	{
		FScopeLock Lock(&Guard);
		Lines.Add({Event, Detail});
	}

	bool Has(const TCHAR* Event, const TCHAR* Fragment = TEXT("")) const
	{
		FScopeLock Lock(&Guard);
		for (const FWoTReceiverSpecBeat& Line : Lines)
		{
			if (Line.Event == Event && Line.Detail.Contains(Fragment))
			{
				return true;
			}
		}
		return false;
	}

	mutable FCriticalSection Guard;
	TArray<FWoTReceiverSpecBeat> Lines;
};

/** The sharer's file, served from memory. */
class FWoTReceiverSpecReader final : public IWoTInstallerShareReader
{
public:
	explicit FWoTReceiverSpecReader(TSharedRef<TArray<uint8>> InBytes)
		: Bytes(MoveTemp(InBytes))
	{
	}

	virtual TWoTBootstrapResult<uint64> ReadAt(uint64 Offset,
		TArrayView<uint8> Out) override
	{
		const uint64 Size = static_cast<uint64>(Bytes->Num());
		const uint64 Count = Offset >= Size ? 0
			: FMath::Min<uint64>(static_cast<uint64>(Out.Num()), Size - Offset);
		if (Count > 0)
		{
			FMemory::Memcpy(Out.GetData(), Bytes->GetData() + Offset, Count);
		}
		return TWoTBootstrapResult<uint64>::Success(Count);
	}

	virtual bool IsUnchanged() override
	{
		return true;
	}

private:
	TSharedRef<TArray<uint8>> Bytes;
};

class FWoTReceiverSpecSource final : public IWoTInstallerShareSource
{
public:
	explicit FWoTReceiverSpecSource(TSharedRef<TArray<uint8>> InBytes)
		: Bytes(MoveTemp(InBytes))
	{
	}

	virtual FString Find() override
	{
		return TEXT("adjacent");
	}

	virtual TUniquePtr<IWoTInstallerShareReader> Open() override
	{
		return MakeUnique<FWoTReceiverSpecReader>(Bytes);
	}

private:
	TSharedRef<TArray<uint8>> Bytes;
};

/** The production sharer over loopback, offering Bytes under Pin. */
struct FWoTReceiverSpecSharer
{
	TSharedRef<FWoTReceiverSpecBeats> Beats = MakeShared<FWoTReceiverSpecBeats>();
	TUniquePtr<FWoTInstallerSharer> Sharer;

	void Make(uint16 ReceiverPort, const FWoTSha256& Pin, TArray<uint8> Bytes,
		const TCHAR* Label = WoTInstallerShare::LabelWindowsPC)
	{
		FWoTInstallerSharer::FConfig Config;
		Config.DiscoveryPort = ReceiverPort;
		Config.Label = Label;
		Config.Size = static_cast<uint64>(Bytes.Num());
		Config.Sha256 = Pin;
		Config.bAllowLoopbackPeers = true;
		Config.DestinationsOverride = {WoTReceiverSpec::Loopback};
		Config.OfferIntervalSeconds = 0.05;
		FWoTInstallerSharer::FDependencies Dependencies;
		Dependencies.Network = WoTCreateProductionInstallerShareNetwork();
		Dependencies.Source = MakeShared<FWoTReceiverSpecSource>(
			MakeShared<TArray<uint8>>(MoveTemp(Bytes)));
		Dependencies.Heartbeat = [BeatLog = Beats](const TCHAR* Event,
			const FString& Detail)
		{
			BeatLog->Add(Event, Detail);
		};
		Sharer = MakeUnique<FWoTInstallerSharer>(MoveTemp(Config),
			MoveTemp(Dependencies));
	}

	void Pump()
	{
		if (Sharer.IsValid())
		{
			Sharer->Pump();
		}
	}
};

/** One receiver over the recording network, a real OriginalGame folder and a
 * hand-driven clock, pumped inline by the spec. */
struct FWoTReceiverSpecRig
{
	FString Root;
	FString Folder;
	FWoTNativeFileIdentity FolderIdentity;
	TArray<uint8> Payload;
	FWoTSha256 Pin;
	TSharedRef<WoTReceiverSpec::FLog> Log = MakeShared<WoTReceiverSpec::FLog>();
	TSharedRef<double> Clock = MakeShared<double>(1000.0);
	TSharedRef<FWoTReceiverSpecBeats> Beats = MakeShared<FWoTReceiverSpecBeats>();
	TSharedRef<TAtomic<int32>> PlacedCalls = MakeShared<TAtomic<int32>>(0);
	TSharedRef<TOptional<uint64>> FreeBytes = MakeShared<TOptional<uint64>>();
	TUniquePtr<FWoTInstallerReceiver> Receiver;
	bool bAllowLoopback = true;

	explicit FWoTReceiverSpecRig(int32 Size = 300 * 1024)
	{
		Payload = WoTReceiverSpec::Payload(Size);
		Pin = Sha256(Payload);
		Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(), TEXT("wot-installer-receiver-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()));
		FPaths::NormalizeDirectoryName(Root);
		Folder = FPaths::Combine(Root, TEXT("OriginalGame"));
	}

	~FWoTReceiverSpecRig()
	{
		if (Receiver.IsValid())
		{
			Receiver->Shutdown();
			Receiver.Reset();
		}
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	}

	bool Initialize()
	{
		if (!IFileManager::Get().MakeDirectory(*Folder, true))
		{
			return false;
		}
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Opened =
			OpenNativeDirectoryNoFollow(Folder);
		if (Opened.IsError())
		{
			return false;
		}
		FolderIdentity = Opened.GetValue().GetIdentity();
		return true;
	}

	FWoTInstallerReceiver::FConfig Config() const
	{
		FWoTInstallerReceiver::FConfig Result;
		Result.DiscoveryPort = 0;
		Result.BindAddress = WoTReceiverSpec::Loopback;
		Result.bAllowLoopbackPeers = bAllowLoopback;
		Result.Label = WoTInstallerShare::LabelSteamDeck;
		Result.Size = static_cast<uint64>(Payload.Num());
		Result.Sha256 = Pin;
		Result.PinnedLeaf = WoT::Bootstrap::InstallerName;
		Result.Directory = Folder;
		Result.DirectoryIdentity = FolderIdentity;
		Result.SocketWaitSeconds = 0.005;
		return Result;
	}

	FWoTInstallerReceiver::FDependencies Dependencies() const
	{
		FWoTInstallerReceiver::FDependencies Result;
		Result.Network = MakeShared<WoTReceiverSpec::FRecordingNetwork>(Log);
		Result.NowSeconds = [ClockValue = Clock]() { return *ClockValue; };
		Result.FreeBytes = [Free = FreeBytes]() { return *Free; };
		Result.OnPlaced = [Calls = PlacedCalls]() { ++(*Calls); };
		Result.Heartbeat = [BeatLog = Beats](const TCHAR* Event, const FString& Detail)
		{
			BeatLog->Add(Event, Detail);
		};
		return Result;
	}

	void Make()
	{
		Receiver = MakeUnique<FWoTInstallerReceiver>(Config(), Dependencies());
		Receiver->Pump();
	}

	uint16 Port() const
	{
		return Receiver.IsValid() ? Receiver->Status().DiscoveryPort : 0;
	}

	void Pump()
	{
		if (Receiver.IsValid())
		{
			Receiver->Pump();
		}
	}

	bool PumpUntil(TFunctionRef<bool()> Done, TFunctionRef<void()> Also,
		double Seconds = WoTReceiverSpecWaitSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Pump();
			Also();
			if (Done())
			{
				return true;
			}
		}
		return Done();
	}

	bool PumpUntil(TFunctionRef<bool()> Done)
	{
		return PumpUntil(Done, []() {});
	}

	/** Pumps a while and reports whether a REQUEST reached Peer. */
	bool RequestArrives(WoTReceiverSpec::FPeer& Peer, int32 Pumps = 50)
	{
		for (int32 Step = 0; Step < Pumps; ++Step)
		{
			Pump();
			if (Peer.PollRequest().IsSet())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return false;
	}

	EWoTInstallerReceiveState State() const
	{
		return Receiver->Status().State;
	}

	FString LeafPath() const
	{
		return FPaths::Combine(Folder, WoT::Bootstrap::InstallerName);
	}

	TArray<FString> PartFiles() const
	{
		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *FPaths::Combine(Folder, TEXT("*.part")),
			true, false);
		return Found;
	}

	int64 PartBytes() const
	{
		const TArray<FString> Parts = PartFiles();
		return Parts.Num() == 1
			? IFileManager::Get().FileSize(*FPaths::Combine(Folder, Parts[0])) : -1;
	}

	bool LeafHolds(const TArray<uint8>& Expected) const
	{
		TArray<uint8> Actual;
		return FFileHelper::LoadFileToArray(Actual, *LeafPath()) && Actual == Expected;
	}
};

TArray<uint8> WoTReceiverSpecSlice(const TArray<uint8>& Bytes, int32 From, int32 To)
{
	return TArray<uint8>(Bytes.GetData() + From, To - From);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverHappyPathSpec,
	"WoT.InstallerShare.Receiver.HappyPath", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverHappyPathSpec::RunTest(const FString&)
{
	// The production sharer and receiver end to end over loopback.
	FWoTReceiverSpecRig Rig;
	if (!TestTrue(TEXT("rig"), Rig.Initialize()))
	{
		return false;
	}
	Rig.Make();
	if (!TestTrue(TEXT("the receiver binds its discovery port"), Rig.Port() != 0))
	{
		return false;
	}
	TestEqual(TEXT("listening"), Rig.State(), EWoTInstallerReceiveState::Listening);
	FWoTReceiverSpecSharer Sharer;
	Sharer.Make(Rig.Port(), Rig.Pin, Rig.Payload);
	const bool bPlaced = Rig.PumpUntil(
		[&Rig]() { return Rig.State() == EWoTInstallerReceiveState::Placed; },
		[&Sharer]() { Sharer.Pump(); });
	if (!TestTrue(TEXT("the copy is placed"), bPlaced))
	{
		return false;
	}
	// Let the sharer see its end of the transfer.
	for (int32 Step = 0; Step < 20 && Sharer.Sharer->ActiveTransferCount() > 0; ++Step)
	{
		Sharer.Pump();
	}
	TestTrue(TEXT("the pinned leaf holds the installer"), Rig.LeafHolds(Rig.Payload));
	TestEqual(TEXT("no .part is left"), Rig.PartFiles().Num(), 0);
	TestEqual(TEXT("the owner is told once"), Rig.PlacedCalls->Load(), 1);
	const FWoTInstallerReceiveStatus Status = Rig.Receiver->Status();
	TestEqual(TEXT("one copy attempt"), Status.Attempt, 1);
	TestEqual(TEXT("all bytes"), Status.CompletedBytes,
		static_cast<uint64>(Rig.Payload.Num()));
	TestEqual(TEXT("the sharer's generic label"), Status.PeerLabel,
		FString(WoTInstallerShare::LabelWindowsPC));
	TestEqual(TEXT("one REQUEST"), Rig.Log->RequestsSent.Load(), 1);
	TestEqual(TEXT("one listener"), Rig.Log->Listens.Load(), 1);
	TestEqual(TEXT("one connection accepted"), Rig.Log->Accepted.Load(), 1);
	TestEqual(TEXT("the receiver never writes to the data connection"),
		Rig.Log->ConnectionSends.Load(), 0);
	TestEqual(TEXT("every socket is closed once placed"), Rig.Log->Open.Load(), 0);
	TestTrue(TEXT("lan-receive-listen"), Rig.Beats->Has(TEXT("lan-receive-listen"),
		TEXT("result=ok")));
	TestTrue(TEXT("lan-receive-start"), Rig.Beats->Has(TEXT("lan-receive-start"),
		TEXT("peer=windows-pc offset=0")));
	TestTrue(TEXT("lan-receive-verified"), Rig.Beats->Has(TEXT("lan-receive-verified")));
	TestTrue(TEXT("the sharer finished a good send"), Sharer.Beats->Has(
		TEXT("lan-share-send-end"), TEXT("result=sent")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverNeverWritesSpec,
	"WoT.InstallerShare.Receiver.NeverWritesToTheDataConnection",
	WoTReceiverSpecFlags)

bool FWoTInstallerReceiverNeverWritesSpec::RunTest(const FString&)
{
	// The sharer closes right after its last byte; anything the receiver sent
	// would make that close a reset. Seen from the sharer's own socket.
	FWoTReceiverSpecRig Rig;
	WoTReceiverSpec::FPeer Peer(1);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	Rig.Make();
	auto Pump = [&Rig]() { Rig.Pump(); };
	const TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
		Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("a REQUEST answers the offer"), Request.IsSet())
		|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
			Rig.Payload.Num(), Pump))
		|| !TestTrue(TEXT("payload"), Peer.Stream(Rig.Payload, Pump)))
	{
		return false;
	}
	TestTrue(TEXT("placed"), Rig.PumpUntil(
		[&Rig]() { return Rig.State() == EWoTInstallerReceiveState::Placed; }));
	bool bClosed = false;
	TestEqual(TEXT("no byte came back on the data connection"),
		Peer.BytesFromReceiver(bClosed, 2.0), 0);
	TestTrue(TEXT("the receiver closed it cleanly after the last byte"), bClosed);
	TestEqual(TEXT("and the seam saw no send on it"),
		Rig.Log->ConnectionSends.Load(), 0);
	TestEqual(TEXT("the REQUEST carried this copy's label"), Request->Label,
		FString(WoTInstallerShare::LabelSteamDeck));
	TestTrue(TEXT("the receiver's port is not a well-known one"),
		Request->TcpPort >= WoTInstallerShare::MinimumTcpPort);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverOfferRulesSpec,
	"WoT.InstallerShare.Receiver.OfferRules", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverOfferRulesSpec::RunTest(const FString&)
{
	// Only this build's exact pin pairs; loopback passes the private-address
	// rule only under the test harness.
	{
		FWoTReceiverSpecRig Rig;
		WoTReceiverSpec::FPeer Peer(2);
		if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
		{
			return false;
		}
		Rig.Make();
		FWoTSha256 Other = Rig.Pin;
		Other.Bytes[0] ^= 0xFF;
		Peer.Offer(Rig.Port(), Rig.Payload.Num(), Other);
		TestFalse(TEXT("another hash gets no REQUEST"), Rig.RequestArrives(Peer));
		Peer.Offer(Rig.Port(), Rig.Payload.Num() + 1, Rig.Pin);
		TestFalse(TEXT("another size gets no REQUEST"), Rig.RequestArrives(Peer));
		Peer.Offer(Rig.Port(), Rig.Payload.Num(), Rig.Pin);
		TestTrue(TEXT("the pin gets a REQUEST"), Rig.RequestArrives(Peer));
		TestEqual(TEXT("nothing listened before the pin matched"),
			Rig.Log->Listens.Load(), 1);
	}
	{
		FWoTReceiverSpecRig Rig;
		Rig.bAllowLoopback = false;
		WoTReceiverSpec::FPeer Peer(3);
		if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
		{
			return false;
		}
		Rig.Make();
		Peer.Offer(Rig.Port(), Rig.Payload.Num(), Rig.Pin);
		TestFalse(TEXT("loopback is not a private address outside the harness"),
			Rig.RequestArrives(Peer));
		TestEqual(TEXT("no REQUEST went out"), Rig.Log->RequestsSent.Load(), 0);
	}
	TestTrue(TEXT("RFC 1918 is private"), WoTInstallerShare::IsPrivatePeerAddress(
		WoTReceiverSpec::Ip(192, 168, 1, 20), false));
	TestFalse(TEXT("a public address is not"), WoTInstallerShare::IsPrivatePeerAddress(
		WoTReceiverSpec::Ip(8, 8, 4, 4), false));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverWrongHashSpec,
	"WoT.InstallerShare.Receiver.WrongHashIgnoresThatSession", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverWrongHashSpec::RunTest(const FString&)
{
	FWoTReceiverSpecRig Rig;
	WoTReceiverSpec::FPeer Peer(4);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	Rig.Make();
	auto Pump = [&Rig]() { Rig.Pump(); };
	const TArray<uint8> Wrong = WoTReceiverSpec::Payload(Rig.Payload.Num(), 99);
	const TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
		Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("REQUEST"), Request.IsSet())
		|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
			Wrong.Num(), Pump))
		|| !TestTrue(TEXT("the wrong bytes"), Peer.Stream(Wrong, Pump)))
	{
		return false;
	}
	TestTrue(TEXT("the damaged copy is seen"), Rig.PumpUntil(
		[&Rig]() { return Rig.Receiver->Status().bLastCopyDamaged; }));
	const FWoTInstallerReceiveStatus Status = Rig.Receiver->Status();
	TestEqual(TEXT("one damaged copy"), Status.DamagedCopies, 1);
	TestEqual(TEXT("still listening"), Status.State, EWoTInstallerReceiveState::Listening);
	TestEqual(TEXT("the damaged .part is deleted"), Rig.PartFiles().Num(), 0);
	TestFalse(TEXT("nothing is placed"), IFileManager::Get().FileExists(*Rig.LeafPath()));
	TestEqual(TEXT("not handed to the owner"), Rig.PlacedCalls->Load(), 0);
	TestTrue(TEXT("lan-receive-failed reason=damaged"),
		Rig.Beats->Has(TEXT("lan-receive-failed"), TEXT("reason=damaged")));
	Peer.CloseData();
	Peer.Offer(Rig.Port(), Rig.Payload.Num(), Rig.Pin);
	TestFalse(TEXT("that sharer's session is ignored"), Rig.RequestArrives(Peer));
	const FWoTShareId Fresh = WoTReceiverSpec::Id(40);
	Peer.Offer(Rig.Port(), Rig.Payload.Num(), Rig.Pin,
		WoTInstallerShare::LabelWindowsPC, &Fresh);
	TestTrue(TEXT("another session still pairs"), Rig.RequestArrives(Peer));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverThreeStrikesSpec,
	"WoT.InstallerShare.Receiver.ThreeDamagedCopiesStopListening",
	WoTReceiverSpecFlags)

bool FWoTInstallerReceiverThreeStrikesSpec::RunTest(const FString&)
{
	FWoTReceiverSpecRig Rig(64 * 1024);
	WoTReceiverSpec::FPeer Peer(5);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	Rig.Make();
	const uint16 Port = Rig.Port();
	auto Pump = [&Rig]() { Rig.Pump(); };
	const TArray<uint8> Wrong = WoTReceiverSpec::Payload(Rig.Payload.Num(), 77);
	for (uint8 Strike = 0; Strike < 3; ++Strike)
	{
		const FWoTShareId Session = WoTReceiverSpec::Id(static_cast<uint8>(60 + Strike));
		const TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
			Port, Rig.Payload.Num(), Rig.Pin, Pump, 10.0, &Session);
		if (!TestTrue(TEXT("REQUEST"), Request.IsSet())
			|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
			|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
				Wrong.Num(), Pump))
			|| !TestTrue(TEXT("bytes"), Peer.Stream(Wrong, Pump)))
		{
			return false;
		}
		const int32 Expected = Strike + 1;
		TestTrue(TEXT("strike counted"), Rig.PumpUntil(
			[&Rig, Expected]() { return Rig.Receiver->Status().DamagedCopies == Expected; }));
		Peer.CloseData();
	}
	TestEqual(TEXT("three damaged copies stop the receiver"), Rig.State(),
		EWoTInstallerReceiveState::GaveUp);
	TestEqual(TEXT("nothing listens any more"), Rig.Log->Open.Load(), 0);
	TestTrue(TEXT("the discovery port is free again"), WoTReceiverSpec::PortIsFree(Port));
	const FWoTShareId Fourth = WoTReceiverSpec::Id(90);
	Peer.Offer(Port, Rig.Payload.Num(), Rig.Pin, WoTInstallerShare::LabelWindowsPC,
		&Fourth);
	TestFalse(TEXT("a fourth sharer gets no REQUEST"), Rig.RequestArrives(Peer));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverResumeSpec,
	"WoT.InstallerShare.Receiver.TruncatedThenResumedFromAnotherSharer",
	WoTReceiverSpecFlags)

bool FWoTInstallerReceiverResumeSpec::RunTest(const FString&)
{
	FWoTReceiverSpecRig Rig;
	WoTReceiverSpec::FPeer Peer(6);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	Rig.Make();
	auto Pump = [&Rig]() { Rig.Pump(); };
	const int32 Cut = Rig.Payload.Num() * 2 / 5;
	const TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
		Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("REQUEST"), Request.IsSet())
		|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
			Rig.Payload.Num(), Pump))
		|| !TestTrue(TEXT("the first part"), Peer.Stream(
			WoTReceiverSpecSlice(Rig.Payload, 0, Cut), Pump)))
	{
		return false;
	}
	TestTrue(TEXT("the first part lands"), Rig.PumpUntil([&Rig, Cut]()
	{
		return Rig.Receiver->Status().CompletedBytes == static_cast<uint64>(Cut);
	}));
	// The first sharer goes away mid-copy.
	Peer.CloseData();
	TestTrue(TEXT("the short stream ends the attempt"), Rig.PumpUntil(
		[&Rig]() { return Rig.State() == EWoTInstallerReceiveState::Listening; }));
	TestEqual(TEXT("the .part is kept for a resume"), Rig.PartBytes(),
		static_cast<int64>(Cut));
	TestTrue(TEXT("lan-receive-failed reason=truncated"),
		Rig.Beats->Has(TEXT("lan-receive-failed"), TEXT("reason=truncated")));
	// Another sharer (the production one) finishes it from where it stopped.
	FWoTReceiverSpecSharer Second;
	Second.Make(Rig.Port(), Rig.Pin, Rig.Payload, WoTInstallerShare::LabelMac);
	TestTrue(TEXT("the second sharer completes the copy"), Rig.PumpUntil(
		[&Rig]() { return Rig.State() == EWoTInstallerReceiveState::Placed; },
		[&Second]() { Second.Pump(); }));
	TestTrue(TEXT("the second sharer streamed only the rest"), Second.Beats->Has(
		TEXT("lan-share-send-start"),
		*FString::Printf(TEXT("offset=%d"), Cut)));
	TestTrue(TEXT("the resumed file is the installer"), Rig.LeafHolds(Rig.Payload));
	TestEqual(TEXT("no .part is left"), Rig.PartFiles().Num(), 0);
	TestEqual(TEXT("two copy attempts"), Rig.Receiver->Status().Attempt, 2);
	TestEqual(TEXT("the second sharer's label"), Rig.Receiver->Status().PeerLabel,
		FString(WoTInstallerShare::LabelMac));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverStrangersSpec,
	"WoT.InstallerShare.Receiver.WrongTokenOrAddressClosedUnread",
	WoTReceiverSpecFlags)

bool FWoTInstallerReceiverStrangersSpec::RunTest(const FString&)
{
	FWoTReceiverSpecRig Rig(128 * 1024);
	WoTReceiverSpec::FPeer Peer(7);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	Rig.Make();
	auto Pump = [&Rig]() { Rig.Pump(); };
	const TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
		Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("REQUEST"), Request.IsSet()))
	{
		return false;
	}
	// 1) A connection from another address: closed without reading a byte.
	Rig.Log->FakePeerAddress.Store(WoTReceiverSpec::Ip(192, 168, 9, 9));
	Rig.Log->FakePeerAccepts.Store(1);
	if (!TestTrue(TEXT("connect from the wrong address"), Peer.Connect(Request->TcpPort)))
	{
		return false;
	}
	// The receiver may close it before or after these bytes arrive.
	(void)Peer.SendHeader(Request->Token, 0, Rig.Payload.Num(), Pump);
	TestTrue(TEXT("the stranger is closed"), Rig.PumpUntil(
		[&Rig]() { return Rig.Log->FakedPeerCloses.Load() == 1; }));
	TestEqual(TEXT("closed unread"), Rig.Log->FakedPeerRecvs.Load(), 0);
	TestEqual(TEXT("no copy started"), Rig.Receiver->Status().Attempt, 0);
	// 2) The right address with the wrong token: closed, no copy.
	FWoTShareId WrongToken = Request->Token;
	WrongToken.Bytes[3] ^= 0x5A;
	if (!TestTrue(TEXT("connect with the wrong token"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("wrong token"), Peer.SendHeader(WrongToken, 0,
			Rig.Payload.Num(), Pump)))
	{
		return false;
	}
	bool bClosed = false;
	TestTrue(TEXT("the wrong token is closed"), Rig.PumpUntil(
		[&Peer, &bClosed]()
		{
			Peer.BytesFromReceiver(bClosed, 0.01);
			return bClosed;
		}));
	TestEqual(TEXT("still no copy"), Rig.Receiver->Status().Attempt, 0);
	TestTrue(TEXT("refusals are recorded"),
		Rig.Beats->Has(TEXT("lan-receive-refused"), TEXT("reason=token")));
	// 3) The paired sharer with its token, still inside the connect window.
	if (!TestTrue(TEXT("connect for real"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
			Rig.Payload.Num(), Pump))
		|| !TestTrue(TEXT("bytes"), Peer.Stream(Rig.Payload, Pump)))
	{
		return false;
	}
	TestTrue(TEXT("the paired sharer's copy is placed"), Rig.PumpUntil(
		[&Rig]() { return Rig.State() == EWoTInstallerReceiveState::Placed; }));
	TestTrue(TEXT("the right bytes"), Rig.LeafHolds(Rig.Payload));
	TestEqual(TEXT("one copy"), Rig.Receiver->Status().Attempt, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverCancelSpec,
	"WoT.InstallerShare.Receiver.Cancel", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverCancelSpec::RunTest(const FString&)
{
	FWoTReceiverSpecRig Rig;
	WoTReceiverSpec::FPeer Peer(8);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	Rig.Make();
	auto Pump = [&Rig]() { Rig.Pump(); };
	const int32 Cut = Rig.Payload.Num() / 3;
	TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
		Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("REQUEST"), Request.IsSet())
		|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
			Rig.Payload.Num(), Pump))
		|| !TestTrue(TEXT("a third"), Peer.Stream(
			WoTReceiverSpecSlice(Rig.Payload, 0, Cut), Pump)))
	{
		return false;
	}
	TestTrue(TEXT("a third lands"), Rig.PumpUntil([&Rig, Cut]()
	{
		return Rig.Receiver->Status().CompletedBytes == static_cast<uint64>(Cut);
	}));
	TestEqual(TEXT("copying"), Rig.State(), EWoTInstallerReceiveState::Copying);
	Rig.Receiver->RequestCancelCopy();
	Rig.Pump();
	TestEqual(TEXT("Cancel returns to waiting"), Rig.State(),
		EWoTInstallerReceiveState::Listening);
	bool bClosed = false;
	TestEqual(TEXT("nothing is written back"), Peer.BytesFromReceiver(bClosed, 2.0), 0);
	TestTrue(TEXT("the copy's connection is closed"), bClosed);
	TestEqual(TEXT("the .part is kept"), Rig.PartBytes(), static_cast<int64>(Cut));
	Peer.CloseData();
	Peer.Offer(Rig.Port(), Rig.Payload.Num(), Rig.Pin);
	TestFalse(TEXT("that sharer is ignored for a while"), Rig.RequestArrives(Peer));
	*Rig.Clock += 31.0;
	Request = Peer.OfferAndAwaitRequest(Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("after 30 s it pairs again"), Request.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("and resumes the kept bytes"), Request->ResumeOffset,
		static_cast<uint64>(Cut));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverDiskFullSpec,
	"WoT.InstallerShare.Receiver.NotEnoughSpaceSendsNoRequest", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverDiskFullSpec::RunTest(const FString&)
{
	FWoTReceiverSpecRig Rig;
	WoTReceiverSpec::FPeer Peer(9);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	const uint64 Margin = 64ull * 1024ull * 1024ull;
	const uint64 Needed = static_cast<uint64>(Rig.Payload.Num()) + Margin;
	*Rig.FreeBytes = Needed - 1;
	Rig.Make();
	Peer.Offer(Rig.Port(), Rig.Payload.Num(), Rig.Pin);
	TestFalse(TEXT("no REQUEST without room"), Rig.RequestArrives(Peer));
	TestEqual(TEXT("nothing listened"), Rig.Log->StreamsCreated.Load(), 0);
	const FWoTInstallerReceiveStatus Status = Rig.Receiver->Status();
	TestTrue(TEXT("the needed space is reported"), Status.NeededBytes.IsSet()
		&& Status.NeededBytes.GetValue() == Needed);
	TestTrue(TEXT("lan-receive-failed reason=space"),
		Rig.Beats->Has(TEXT("lan-receive-failed"), TEXT("reason=space")));
	*Rig.FreeBytes = Needed;
	Peer.Offer(Rig.Port(), Rig.Payload.Num(), Rig.Pin);
	TestTrue(TEXT("with room the offer pairs"), Rig.RequestArrives(Peer));
	TestFalse(TEXT("the space note clears"), Rig.Receiver->Status().NeededBytes.IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverConnectTimeoutSpec,
	"WoT.InstallerShare.Receiver.ConnectAndIdleTimeouts", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverConnectTimeoutSpec::RunTest(const FString&)
{
	FWoTReceiverSpecRig Rig;
	WoTReceiverSpec::FPeer Peer(10);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	Rig.Make();
	auto Pump = [&Rig]() { Rig.Pump(); };
	// A sharer that never connects: the pairing ends after 5 s.
	TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
		Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("REQUEST"), Request.IsSet()))
	{
		return false;
	}
	*Rig.Clock += 4.0;
	Rig.Pump();
	TestEqual(TEXT("still paired at 4 s"), Rig.Log->Open.Load(), 2);
	*Rig.Clock += 1.5;
	Rig.Pump();
	TestEqual(TEXT("the listener closes after 5 s"), Rig.Log->Open.Load(), 1);
	TestTrue(TEXT("lan-receive-failed reason=connect"),
		Rig.Beats->Has(TEXT("lan-receive-failed"), TEXT("reason=connect")));
	// A sharer that stops sending: the copy ends after 15 s without bytes.
	Request = Peer.OfferAndAwaitRequest(Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
	if (!TestTrue(TEXT("second REQUEST"), Request.IsSet())
		|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
			Rig.Payload.Num(), Pump))
		|| !TestTrue(TEXT("a few bytes"), Peer.Stream(
			WoTReceiverSpecSlice(Rig.Payload, 0, 1000), Pump)))
	{
		return false;
	}
	TestTrue(TEXT("copying"), Rig.PumpUntil(
		[&Rig]() { return Rig.Receiver->Status().CompletedBytes == 1000; }));
	*Rig.Clock += 14.0;
	Rig.Pump();
	TestEqual(TEXT("still copying at 14 s"), Rig.State(),
		EWoTInstallerReceiveState::Copying);
	*Rig.Clock += 1.5;
	Rig.Pump();
	TestEqual(TEXT("idle for 15 s ends the copy"), Rig.State(),
		EWoTInstallerReceiveState::Listening);
	TestEqual(TEXT("its bytes are kept"), Rig.PartBytes(), static_cast<int64>(1000));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverStopSpec,
	"WoT.InstallerShare.Receiver.StopClosesEverythingAndKeepsThePart",
	WoTReceiverSpecFlags)

bool FWoTInstallerReceiverStopSpec::RunTest(const FString&)
{
	// The thread form, as the worker runs it: StopAndJoin mid-copy.
	FWoTReceiverSpecRig Rig;
	WoTReceiverSpec::FPeer Peer(11);
	if (!TestTrue(TEXT("rig"), Rig.Initialize()) || !TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	FWoTInstallerReceiver::FDependencies Dependencies = Rig.Dependencies();
	Dependencies.NowSeconds = {};
	FWoTInstallerReceiverThread Thread(MakeUnique<FWoTInstallerReceiver>(
		Rig.Config(), MoveTemp(Dependencies)));
	if (!TestTrue(TEXT("the thread starts"), Thread.Start()))
	{
		return false;
	}
	const double Deadline = FPlatformTime::Seconds() + WoTReceiverSpecWaitSeconds;
	while (FPlatformTime::Seconds() < Deadline && Thread.Status().DiscoveryPort == 0)
	{
		FPlatformProcess::Sleep(0.005f);
	}
	const uint16 Port = Thread.Status().DiscoveryPort;
	auto Wait = []() { FPlatformProcess::Sleep(0.001f); };
	const int32 Cut = Rig.Payload.Num() / 4;
	const TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
		Port, Rig.Payload.Num(), Rig.Pin, Wait);
	if (!TestTrue(TEXT("REQUEST from the thread"), Request.IsSet())
		|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
			Rig.Payload.Num(), Wait))
		|| !TestTrue(TEXT("a quarter"), Peer.Stream(
			WoTReceiverSpecSlice(Rig.Payload, 0, Cut), Wait)))
	{
		Thread.StopAndJoin();
		return false;
	}
	const double CopyDeadline = FPlatformTime::Seconds() + WoTReceiverSpecWaitSeconds;
	while (FPlatformTime::Seconds() < CopyDeadline
		&& Thread.Status().CompletedBytes != static_cast<uint64>(Cut))
	{
		FPlatformProcess::Sleep(0.005f);
	}
	TestEqual(TEXT("copying on its thread"), Thread.Status().State,
		EWoTInstallerReceiveState::Copying);
	Thread.StopAndJoin();
	TestEqual(TEXT("every socket is closed"), Rig.Log->Open.Load(), 0);
	TestTrue(TEXT("the discovery port is free"), WoTReceiverSpec::PortIsFree(Port));
	TestEqual(TEXT("the .part stays for a later resume"), Rig.PartBytes(),
		static_cast<int64>(Cut));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverLeftoverSpec,
	"WoT.InstallerShare.Receiver.ResumesALeftoverPart", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverLeftoverSpec::RunTest(const FString&)
{
	// A .part an earlier run left (the game closed mid-copy) is resumed, and a
	// complete leftover is checked without asking anyone.
	{
		FWoTReceiverSpecRig Rig;
		if (!TestTrue(TEXT("rig"), Rig.Initialize()))
		{
			return false;
		}
		const int32 Half = Rig.Payload.Num() / 2;
		TestTrue(TEXT("leftover written"), FFileHelper::SaveArrayToFile(
			WoTReceiverSpecSlice(Rig.Payload, 0, Half),
			*FPaths::Combine(Rig.Folder, TEXT("leftover.part"))));
		Rig.Make();
		WoTReceiverSpec::FPeer Peer(12);
		if (!TestTrue(TEXT("peer"), Peer.Open()))
		{
			return false;
		}
		auto Pump = [&Rig]() { Rig.Pump(); };
		const TOptional<FWoTShareRequest> Request = Peer.OfferAndAwaitRequest(
			Rig.Port(), Rig.Payload.Num(), Rig.Pin, Pump);
		if (!TestTrue(TEXT("REQUEST"), Request.IsSet()))
		{
			return false;
		}
		TestEqual(TEXT("the leftover is resumed"), Request->ResumeOffset,
			static_cast<uint64>(Half));
		if (!TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
			|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, Half,
				Rig.Payload.Num() - Half, Pump))
			|| !TestTrue(TEXT("the rest"), Peer.Stream(
				WoTReceiverSpecSlice(Rig.Payload, Half, Rig.Payload.Num()), Pump)))
		{
			return false;
		}
		TestTrue(TEXT("placed"), Rig.PumpUntil(
			[&Rig]() { return Rig.State() == EWoTInstallerReceiveState::Placed; }));
		TestTrue(TEXT("the joined file is the installer"), Rig.LeafHolds(Rig.Payload));
	}
	{
		FWoTReceiverSpecRig Rig;
		if (!TestTrue(TEXT("rig"), Rig.Initialize()))
		{
			return false;
		}
		TestTrue(TEXT("complete leftover written"), FFileHelper::SaveArrayToFile(
			Rig.Payload, *FPaths::Combine(Rig.Folder, TEXT("complete.part"))));
		Rig.Make();
		TestTrue(TEXT("a complete leftover is placed without a sharer"), Rig.PumpUntil(
			[&Rig]() { return Rig.State() == EWoTInstallerReceiveState::Placed; }));
		TestTrue(TEXT("its bytes"), Rig.LeafHolds(Rig.Payload));
		TestEqual(TEXT("no REQUEST was needed"), Rig.Log->RequestsSent.Load(), 0);
	}
	{
		FWoTReceiverSpecRig Rig;
		if (!TestTrue(TEXT("rig"), Rig.Initialize()))
		{
			return false;
		}
		TestTrue(TEXT("damaged leftover written"), FFileHelper::SaveArrayToFile(
			WoTReceiverSpec::Payload(Rig.Payload.Num(), 5),
			*FPaths::Combine(Rig.Folder, TEXT("damaged.part"))));
		Rig.Make();
		TestTrue(TEXT("a damaged leftover is deleted"), Rig.PumpUntil(
			[&Rig]() { return Rig.PartFiles().Num() == 0; }));
		TestEqual(TEXT("and is no strike"), Rig.Receiver->Status().DamagedCopies, 0);
		TestEqual(TEXT("still listening"), Rig.State(),
			EWoTInstallerReceiveState::Listening);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverFolderSpec,
	"WoT.InstallerShare.Receiver.ChangedFolderIsUnavailable", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverFolderSpec::RunTest(const FString&)
{
	// The owner pins the folder's identity; a different folder at that path
	// is never written into.
	FWoTReceiverSpecRig Rig;
	if (!TestTrue(TEXT("rig"), Rig.Initialize()))
	{
		return false;
	}
	TestTrue(TEXT("folder removed"),
		IFileManager::Get().DeleteDirectory(*Rig.Folder, false, true));
	TestTrue(TEXT("and recreated"), IFileManager::Get().MakeDirectory(*Rig.Folder, true));
	Rig.Make();
	TestEqual(TEXT("unavailable"), Rig.State(), EWoTInstallerReceiveState::Unavailable);
	TestEqual(TEXT("no socket"), Rig.Log->DatagramsCreated.Load(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerReceiverTriggerSpec,
	"WoT.InstallerShare.Receiver.TriggerRules", WoTReceiverSpecFlags)

bool FWoTInstallerReceiverTriggerSpec::RunTest(const FString&)
{
	using namespace WoTInstallerShare;
	TestTrue(TEXT("Proton on Linux starts by itself"),
		ReceiverStartsAutomaticallyFor(true, true, TEXT("Linux")));
	TestFalse(TEXT("CrossOver on macOS counts as a Mac"),
		ReceiverStartsAutomaticallyFor(true, true, TEXT("Darwin")));
	TestFalse(TEXT("an unknown Wine host waits for the button"),
		ReceiverStartsAutomaticallyFor(true, true, TEXT("")));
	TestFalse(TEXT("native Windows waits for the button"),
		ReceiverStartsAutomaticallyFor(true, false, TEXT("")));
	TestFalse(TEXT("a Mac build waits for the button"),
		ReceiverStartsAutomaticallyFor(false, false, TEXT("")));
	ON_SCOPE_EXIT
	{
		WoTSetRunningUnderWineForTesting(TOptional<bool>());
		SetWineHostSysnameForTesting(TOptional<FString>());
	};
	WoTSetRunningUnderWineForTesting(true);
	SetWineHostSysnameForTesting(FString(TEXT("Linux")));
	TestEqual(TEXT("under the Wine seam on Linux it starts by itself"),
		ReceiverStartsAutomatically(), PLATFORM_WINDOWS != 0);
	SetWineHostSysnameForTesting(FString(TEXT("Darwin")));
	TestFalse(TEXT("under the Wine seam on macOS it needs the button"),
		ReceiverStartsAutomatically());
	WoTSetRunningUnderWineForTesting(false);
	SetWineHostSysnameForTesting(FString(TEXT("Linux")));
	TestFalse(TEXT("outside Wine it needs the button"), ReceiverStartsAutomatically());
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	const FString Previous =
		FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_TEST_LAN_RECEIVER_OFF"));
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("WOT_TEST_LAN_RECEIVER_OFF"), *Previous);
	};
	FPlatformMisc::SetEnvironmentVar(TEXT("WOT_TEST_LAN_RECEIVER_OFF"), TEXT("1"));
	TestTrue(TEXT("the Test-build switch turns receiving off"), HarnessReceiverOff());
	FPlatformMisc::SetEnvironmentVar(TEXT("WOT_TEST_LAN_RECEIVER_OFF"), TEXT("0"));
	TestFalse(TEXT("anything but 1 leaves it on"), HarnessReceiverOff());
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerCopyByteStagesSpec,
	"WoT.Bootstrap.InstallerCopy.ByteStages", WoTReceiverSpecFlags)

bool FWoTInstallerCopyByteStagesSpec::RunTest(const FString&)
{
	FWoTBootstrapViewModel Model;
	Model.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	Model.TransitionTo(EWoTBootstrapPhase::CheckingOriginalGameData);
	Model.TransitionTo(EWoTBootstrapPhase::OriginalGameNeeded);
	const uint64 Epoch = Model.Snapshot().OperationEpoch;
	auto Event = [Epoch](const TCHAR* Stage, TOptional<uint64> Completed,
		EWoTInstallerCopyStatus Status)
	{
		FWoTBootstrapProgressEvent Result;
		Result.OperationEpoch = Epoch;
		Result.StageId = Stage;
		if (Completed.IsSet())
		{
			Result.CompletedBytes = Completed;
			Result.TotalBytes = 100;
		}
		Result.OriginalGameNeededReason = EWoTOriginalGameNeededReason::Missing;
		FWoTInstallerCopyState Copy;
		Copy.Status = Status;
		Copy.PeerLabel = WoTInstallerShare::LabelWindowsPC;
		Result.InstallerCopy = Copy;
		return Result;
	};
	TestTrue(TEXT("IsBytePhase is relaxed for the copy stage only"),
		FWoTBootstrapViewModel::IsBytePhase(EWoTBootstrapPhase::OriginalGameNeeded,
			TEXT("lan-copy-1"))
		&& !FWoTBootstrapViewModel::IsBytePhase(EWoTBootstrapPhase::OriginalGameNeeded,
			TEXT("other"))
		&& !FWoTBootstrapViewModel::IsBytePhase(EWoTBootstrapPhase::OriginalGameNeeded));
	TestTrue(TEXT("a waiting event"), Model.UpdateProgress(Event(TEXT(""), {},
		EWoTInstallerCopyStatus::Waiting)));
	TestEqual(TEXT("its copy state"), Model.Snapshot().InstallerCopy.Status,
		EWoTInstallerCopyStatus::Waiting);
	TestTrue(TEXT("a copy's bytes"), Model.UpdateProgress(Event(TEXT("lan-copy-1"),
		uint64(90), EWoTInstallerCopyStatus::Copying)));
	TestEqual(TEXT("90%"), Model.Snapshot().PhaseFraction.Get(-1.0f), 0.9f);
	TestFalse(TEXT("bytes never go back within one copy"), Model.UpdateProgress(
		Event(TEXT("lan-copy-1"), uint64(80), EWoTInstallerCopyStatus::Copying)));
	TestFalse(TEXT("bytes under any other stage are refused"), Model.UpdateProgress(
		Event(TEXT("other"), uint64(95), EWoTInstallerCopyStatus::Copying)));
	TestTrue(TEXT("the damaged copy's event"), Model.UpdateProgress(Event(TEXT(""), {},
		EWoTInstallerCopyStatus::Damaged)));
	TestFalse(TEXT("the bar empties between copies"),
		Model.Snapshot().PhaseFraction.IsSet());
	TestFalse(TEXT("no bytes between copies"), Model.Snapshot().CompletedBytes.IsSet());
	TestTrue(TEXT("the next copy starts again from zero"), Model.UpdateProgress(
		Event(TEXT("lan-copy-2"), uint64(5), EWoTInstallerCopyStatus::Copying)));
	TestEqual(TEXT("5%"), Model.Snapshot().PhaseFraction.Get(-1.0f), 0.05f);
	TestTrue(TEXT("a new stage id restarts the bytes even without a gap"),
		Model.UpdateProgress(Event(TEXT("lan-copy-3"), uint64(1),
			EWoTInstallerCopyStatus::Copying)));
	TestEqual(TEXT("the stage reads as a copy"), Model.Snapshot().StageText.ToString(),
		FString(TEXT("Copying the original game installer")));
	TestEqual(TEXT("the copy state follows"), Model.Snapshot().InstallerCopy.Status,
		EWoTInstallerCopyStatus::Copying);
	Model.TransitionTo(EWoTBootstrapPhase::ExtractingOriginalGame);
	TestEqual(TEXT("leaving the phase resets the copy state"),
		Model.Snapshot().InstallerCopy.Status, EWoTInstallerCopyStatus::Unknown);

	// A download keeps its old rule: a lower byte count is refused whatever
	// its stage id.
	FWoTBootstrapViewModel Download;
	Download.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	Download.TransitionTo(EWoTBootstrapPhase::DownloadingUpdate);
	FWoTBootstrapProgressEvent First;
	First.OperationEpoch = Download.Snapshot().OperationEpoch;
	First.StageId = TEXT("a");
	First.CompletedBytes = 50;
	First.TotalBytes = 100;
	TestTrue(TEXT("download bytes"), Download.UpdateProgress(MoveTemp(First)));
	FWoTBootstrapProgressEvent Lower;
	Lower.OperationEpoch = Download.Snapshot().OperationEpoch;
	Lower.StageId = TEXT("b");
	Lower.CompletedBytes = 10;
	Lower.TotalBytes = 100;
	TestFalse(TEXT("a download never goes back"), Download.UpdateProgress(MoveTemp(Lower)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTInstallerCopyScreenSpec,
	"WoT.Bootstrap.InstallerCopy.Screen", WoTReceiverSpecFlags)

bool FWoTInstallerCopyScreenSpec::RunTest(const FString&)
{
	FWoTBootstrapSnapshot Snapshot;
	Snapshot.Phase = EWoTBootstrapPhase::OriginalGameNeeded;
	Snapshot.bMandatoryWork = true;
	int32 Copies = 0;
	int32 Cancels = 0;
	int32 Browses = 0;
	int32 Exits = 0;
	TSharedRef<SWoTBootstrapScreen> Screen = SNew(SWoTBootstrapScreen)
		.Snapshot_Lambda([&Snapshot]() { return Snapshot; })
		.OnExitRequested(FSimpleDelegate::CreateLambda([&Exits]() { ++Exits; }))
		.OnBrowseInstaller(FSimpleDelegate::CreateLambda([&Browses]() { ++Browses; }))
		.OnCopyFromAnotherComputer(FSimpleDelegate::CreateLambda([&Copies]() { ++Copies; }))
		.OnCancelInstallerCopy(FSimpleDelegate::CreateLambda([&Cancels]() { ++Cancels; }));
	auto Visible = [](const TSharedPtr<SWidget>& Widget)
	{
		return Widget.IsValid() && Widget->GetVisibility() == EVisibility::Visible;
	};
	auto Refresh = [&Screen]()
	{
		Screen->CopyFromAnotherComputerWidgetForTesting()->UpdateAllAttributes();
		Screen->CancelInstallerCopyWidgetForTesting()->UpdateAllAttributes();
		Screen->BrowseInstallerWidgetForTesting()->UpdateAllAttributes();
	};

	// Before the worker's first copy state: no line, no button.
	Refresh();
	TestTrue(TEXT("no line before the first state"), Screen->InstallerCopyText().IsEmpty());
	TestFalse(TEXT("no button before the first state"),
		Visible(Screen->CopyFromAnotherComputerWidgetForTesting()));

	// Windows and Mac, not listening: the hint and the button.
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::Off;
	Refresh();
	TestEqual(TEXT("the Windows hint"),
		SWoTBootstrapScreen::InstallerCopyTextFor(Snapshot, true).ToString(),
		FString(TEXT("Have WoT Reborn on another computer on this network? Open it "
			"there, then click Copy from another computer. If Windows asks, click "
			"Allow.")));
	TestEqual(TEXT("the Mac hint has no firewall line"),
		SWoTBootstrapScreen::InstallerCopyTextFor(Snapshot, false).ToString(),
		FString(TEXT("Have WoT Reborn on another computer on this network? Open it "
			"there, then click Copy from another computer.")));
	TestTrue(TEXT("the button shows"), Visible(Screen->CopyFromAnotherComputerWidgetForTesting()));
	TestFalse(TEXT("no Cancel while nothing copies"),
		Visible(Screen->CancelInstallerCopyWidgetForTesting()));
	TestTrue(TEXT("Browse stays"), Visible(Screen->BrowseInstallerWidgetForTesting()));
	TestTrue(TEXT("the button is a focusable SButton"),
		Screen->CopyFromAnotherComputerWidgetForTesting()->GetType() == FName(TEXT("SButton"))
		&& Screen->CopyFromAnotherComputerWidgetForTesting()->SupportsKeyboardFocus());
	TestTrue(TEXT("Cancel is a focusable SButton"),
		Screen->CancelInstallerCopyWidgetForTesting()->GetType() == FName(TEXT("SButton"))
		&& Screen->CancelInstallerCopyWidgetForTesting()->SupportsKeyboardFocus());
	TestTrue(TEXT("Browse keeps the initial focus"),
		Screen->InitialFocusWidgetForTesting() == Screen->BrowseInstallerWidgetForTesting());
	Screen->CopyFromAnotherComputerForTesting();
	TestEqual(TEXT("the click reaches the controller"), Copies, 1);

	// Listening after the click.
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::Waiting;
	Refresh();
	TestFalse(TEXT("the button goes while listening"),
		Visible(Screen->CopyFromAnotherComputerWidgetForTesting()));
	Screen->CopyFromAnotherComputerForTesting();
	TestEqual(TEXT("a hidden button does nothing"), Copies, 1);
	TestEqual(TEXT("listening on Windows"),
		SWoTBootstrapScreen::InstallerCopyTextFor(Snapshot, true).ToString(),
		FString(TEXT("Looking for WoT Reborn on another computer on this network… "
			"If Windows asks, click Allow.")));

	// Proton: listening by itself, never a button.
	Snapshot.InstallerCopy.bAutomatic = true;
	Refresh();
	TestEqual(TEXT("the Proton line"),
		SWoTBootstrapScreen::InstallerCopyTextFor(Snapshot, true).ToString(),
		FString(TEXT("Or open WoT Reborn (version 0.1.4 or later) on a computer on "
			"this network that has the installer; it is copied here automatically.")));
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::GaveUp;
	Refresh();
	TestFalse(TEXT("never a button under Proton"),
		Visible(Screen->CopyFromAnotherComputerWidgetForTesting()));
	Snapshot.InstallerCopy.bAutomatic = false;
	Refresh();
	TestTrue(TEXT("after three strikes the button is back"),
		Visible(Screen->CopyFromAnotherComputerWidgetForTesting()));
	TestEqual(TEXT("three strikes"),
		SWoTBootstrapScreen::InstallerCopyTextFor(Snapshot, true).ToString(),
		FString(TEXT("Copies from this network keep arriving damaged. Use Browse, or "
			"put the installer in Downloads.")));

	// Copying: the status line, the bar, Cancel.
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::Copying;
	Snapshot.InstallerCopy.PeerLabel = WoTInstallerShare::LabelWindowsPC;
	Snapshot.StageId = TEXT("lan-copy-1");
	Snapshot.CompletedBytes = 42;
	Snapshot.TotalBytes = 100;
	Snapshot.PhaseFraction = 0.42f;
	Refresh();
	TestEqual(TEXT("the copy line"), Screen->ProgressText().ToString(),
		FString(TEXT("Copying the original game installer from a Windows PC… 42%")));
	TestEqual(TEXT("the bar follows the copy"), Screen->PhaseProgress().Get(-1.0f), 0.42f);
	TestTrue(TEXT("no second line while copying"), Screen->InstallerCopyText().IsEmpty());
	TestTrue(TEXT("Cancel shows"), Visible(Screen->CancelInstallerCopyWidgetForTesting()));
	TestFalse(TEXT("no Copy button while copying"),
		Visible(Screen->CopyFromAnotherComputerWidgetForTesting()));
	TestTrue(TEXT("Browse stays during a copy"), Visible(Screen->BrowseInstallerWidgetForTesting()));
	Screen->CancelInstallerCopyForTesting();
	TestEqual(TEXT("Cancel reaches the controller"), Cancels, 1);

	// Damaged, and not enough space.
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::Damaged;
	Snapshot.CompletedBytes.Reset();
	Snapshot.TotalBytes.Reset();
	Snapshot.PhaseFraction.Reset();
	Refresh();
	TestEqual(TEXT("damaged"), Screen->InstallerCopyText().ToString(),
		FString(TEXT("The copy from a Windows PC was damaged. Trying again…")));
	TestFalse(TEXT("no Cancel after the copy"),
		Visible(Screen->CancelInstallerCopyWidgetForTesting()));
	Screen->CancelInstallerCopyForTesting();
	TestEqual(TEXT("a hidden Cancel does nothing"), Cancels, 1);
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::NotEnoughSpace;
	Snapshot.InstallerCopy.NeededBytes = 1029509080ull + 64ull * 1024ull * 1024ull;
	TestEqual(TEXT("not enough space, in GB rounded up"),
		Screen->InstallerCopyText().ToString(),
		FString(TEXT("Not enough free space for the original installer: 1.1 GB needed.")));
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::Disabled;
	TestTrue(TEXT("nothing where receiving is off"), Screen->InstallerCopyText().IsEmpty());

	// Quit and Escape are unchanged: the waiting screen has no Quit button and
	// Escape takes the exit-only path.
	Screen->ExitWidgetForTesting()->UpdateAllAttributes();
	TestFalse(TEXT("no Quit button"), Visible(Screen->ExitWidgetForTesting()));
	const FKeyEvent Escape(EKeys::Escape, FModifierKeysState(), 0, false, 0, 0);
	Screen->OnKeyDown(FGeometry(), Escape);
	TestEqual(TEXT("Escape exits"), Exits, 1);
	TestEqual(TEXT("and starts nothing else"), Copies + Cancels + Browses, 2);

	// Other phases never show it.
	Snapshot.Phase = EWoTBootstrapPhase::BuildingContent;
	Snapshot.InstallerCopy.Status = EWoTInstallerCopyStatus::Off;
	Refresh();
	TestTrue(TEXT("not outside the phase"), Screen->InstallerCopyText().IsEmpty());
	TestFalse(TEXT("no button outside the phase"),
		Visible(Screen->CopyFromAnotherComputerWidgetForTesting()));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
