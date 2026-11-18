// LAN installer sharing, protocol v1 (spec B3): the pure frame codec, the
// label rules, the private-address rule and the offer-destination filter.
// Nothing here touches a socket, a file or a clock, so specs drive it directly.
#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapCore/ArtifactTypes.h"

/** A 16-byte random sharer session id or transfer token. */
struct FWoTShareId
{
	static constexpr int32 ByteCount = 16;

	uint8 Bytes[ByteCount] = {};

	bool operator==(const FWoTShareId& Other) const
	{
		return FMemory::Memcmp(Bytes, Other.Bytes, ByteCount) == 0;
	}
	bool operator!=(const FWoTShareId& Other) const { return !(*this == Other); }
	friend uint32 GetTypeHash(const FWoTShareId& Id)
	{
		return FCrc::MemCrc32(Id.Bytes, ByteCount);
	}

	/** A fresh random id (not a secret: the pin is the only acceptance test). */
	static FWoTShareId Random();
};

/** OFFER (type 1): sharer -> broadcast, every 2 s while sharing is on. */
struct FWoTShareOffer
{
	FWoTShareId SessionId;
	FWoTSha256 Sha256;
	uint64 Size = 0;
	FString Label;
};

/** REQUEST (type 2): receiver -> the OFFER's source address, unicast. */
struct FWoTShareRequest
{
	FWoTShareId SessionId;
	FWoTShareId Token;
	uint16 TcpPort = 0;
	uint64 ResumeOffset = 0;
	FString Label;
};

/** DATA (type 3) header: the sharer's first bytes on the outward connection,
 * followed by exactly Length bytes of the file from Offset. */
struct FWoTShareDataHeader
{
	FWoTShareId Token;
	uint64 Offset = 0;
	uint64 Length = 0;
};

/** One IPv4 address of one local network adapter, as the platform reports it.
 * Addresses are host byte order. */
struct FWoTShareAdapterAddress
{
	uint32 Address = 0;
	uint8 PrefixLength = 0;
	bool bUp = false;
	bool bLoopback = false;
	/** A tunnel, PPP or other point-to-point/VPN adapter. */
	bool bTunnel = false;
	/** Duplicate-address detection has not finished (or failed). */
	bool bTentative = false;
};

namespace WoTInstallerShare
{
	/** UDP port P: OFFERs go to it and receivers bind it. Unassigned at IANA
	 * and distinct from 7777 (game), 14001 (LAN beacon) and 17777 (net tests). */
	inline constexpr uint16 DiscoveryPort = 14095;
	inline constexpr uint8 ProtocolVersion = 1;
	inline constexpr int32 MagicByteCount = 5;
	inline constexpr int32 MaxLabelBytes = 48;
	/** The limited broadcast address, host byte order. */
	inline constexpr uint32 LimitedBroadcast = 0xFFFFFFFFu;
	/** Receivers listen on an OS-chosen port; the well-known ports below this
	 * are never a receiver's, so the sharer never connects to one. */
	inline constexpr uint16 MinimumTcpPort = 1024;

	enum class EMessageType : uint8
	{
		Offer = 1,
		Request = 2,
		Data = 3,
	};

	/** "WOTIS" + version + type + 16-byte token + offset + length. */
	inline constexpr int32 DataHeaderBytes = MagicByteCount + 2
		+ FWoTShareId::ByteCount + 8 + 8;

	/** Labels are 1-48 bytes of strict UTF-8 with no control characters. */
	bool IsValidLabel(FStringView Label);

	/** Each encoder returns an empty array when the message's label breaks the
	 * label rules; a valid message always encodes. */
	TArray<uint8> EncodeOffer(const FWoTShareOffer& Offer);
	TArray<uint8> EncodeRequest(const FWoTShareRequest& Request);
	TArray<uint8> EncodeDataHeader(const FWoTShareDataHeader& Header);

	/** Each decoder accepts exactly one well-formed message of its type: the
	 * magic, the version, the type, the exact length and the label rules are
	 * all checked. Anything else is unset. */
	TOptional<FWoTShareOffer> DecodeOffer(TConstArrayView<uint8> Bytes);
	TOptional<FWoTShareRequest> DecodeRequest(TConstArrayView<uint8> Bytes);
	TOptional<FWoTShareDataHeader> DecodeDataHeader(TConstArrayView<uint8> Bytes);

	/** RFC 1918 (10/8, 172.16/12, 192.168/16) and link-local (169.254/16).
	 * Loopback (127/8) only when the caller is a test or the test harness. */
	bool IsPrivatePeerAddress(uint32 HostOrderAddress, bool bAllowLoopback);

	/** The directed broadcast of every eligible adapter address, deduplicated:
	 * skips adapters that are down, loopback or tunnels (VPNs, PPP), tentative
	 * addresses, link-local and non-private addresses, and prefixes with no
	 * broadcast (/0, /31, /32). */
	TArray<uint32> DirectedBroadcasts(
		TConstArrayView<FWoTShareAdapterAddress> Adapters);

	/** 255.255.255.255 first, then DirectedBroadcasts(Adapters). */
	TArray<uint32> OfferDestinations(
		TConstArrayView<FWoTShareAdapterAddress> Adapters);

	/** The three generic labels. Never the OS device name, which would put
	 * something like a person's name on the network every 2 s. */
	inline constexpr TCHAR LabelWindowsPC[] = TEXT("a Windows PC");
	inline constexpr TCHAR LabelMac[] = TEXT("a Mac");
	inline constexpr TCHAR LabelSteamDeck[] = TEXT("a Steam Deck");
	/** Shown instead of any peer label that is not one of the three above. */
	inline constexpr TCHAR LabelOtherComputer[] = TEXT("another computer");

	/** Pure label choice: a Mac build, or a Windows build under Wine whose host
	 * is Darwin (CrossOver), is "a Mac"; any other Wine host (Proton on Linux)
	 * is "a Steam Deck"; native Windows is "a Windows PC". */
	const TCHAR* LocalLabelFor(bool bPlatformMac, bool bUnderWine,
		FStringView HostSysname);

	/** This copy's label. */
	FString LocalLabel();

	/** Wine's host OS name ("Linux", "Darwin") from ntdll's
	 * wine_get_host_version; empty on native Windows and on a Mac build. */
	FString WineHostSysname();
#if WITH_AUTOMATION_TESTS
	/** Replaces WineHostSysname's answer; unset restores detection. */
	void SetWineHostSysnameForTesting(TOptional<FString> Sysname);
#endif

	/** Pure receiver trigger rule (spec B1): a receiver starts listening by
	 * itself only in a Windows build under Wine on Linux (Proton on a Steam
	 * Deck), where nothing prompts. Wine on macOS (CrossOver) counts as a Mac,
	 * and native Windows and Mac wait for the player's click, because a
	 * listening socket is what raises the firewall or Local Network prompt. */
	bool ReceiverStartsAutomaticallyFor(bool bPlatformWindows, bool bUnderWine,
		FStringView HostSysname);

	/** ReceiverStartsAutomaticallyFor for this copy. */
	bool ReceiverStartsAutomatically();

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	/** Test harness only (never compiled into guarded Shipping): with
	 * -wottestlanloopback the receiver binds 127.0.0.1 and the sharer offers to
	 * 127.0.0.1 only, and both accept loopback peers, so a funnel cell can pair
	 * two processes on one PC without raising a firewall prompt. */
	bool HarnessLoopback();
	/** Test harness only: WOT_TEST_LAN_RECEIVER_OFF=1 in the environment keeps
	 * the receiver from ever starting, so a sharer elsewhere on the LAN cannot
	 * feed a cell that judges where the installer came from. */
	bool HarnessReceiverOff();
#endif

	/** What the player sees for a peer: one of the generic labels, never a
	 * peer-chosen string. */
	FString DisplayLabel(FStringView PeerLabel);

	/** A space-free heartbeat token for a peer label. */
	const TCHAR* PeerToken(FStringView PeerLabel);

	/** The sharer's notice lines (spec B1). */
	FString SendingNotice(FStringView DisplayedLabel);
	FString SentNotice(FStringView DisplayedLabel);
	FString StoppedNotice(FStringView DisplayedLabel);
}
