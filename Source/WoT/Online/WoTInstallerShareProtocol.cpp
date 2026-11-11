#include "WoTInstallerShareProtocol.h"

#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "WoTBootstrapCore/NativeHandles.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
constexpr uint8 WoTShareMagic[WoTInstallerShare::MagicByteCount] =
	{'W', 'O', 'T', 'I', 'S'};
constexpr int32 WoTShareHeaderBytes = WoTInstallerShare::MagicByteCount + 2;
// Fixed bytes before the label: header, session id, SHA-256, size, length.
constexpr int32 WoTShareOfferFixedBytes = WoTShareHeaderBytes
	+ FWoTShareId::ByteCount + FWoTSha256::ByteCount + 8 + 1;
// Header, session id, token, TCP port, resume offset, label length.
constexpr int32 WoTShareRequestFixedBytes = WoTShareHeaderBytes
	+ FWoTShareId::ByteCount * 2 + 2 + 8 + 1;

void WoTShareAppendHeader(TArray<uint8>& Out, WoTInstallerShare::EMessageType Type)
{
	Out.Append(WoTShareMagic, WoTInstallerShare::MagicByteCount);
	Out.Add(WoTInstallerShare::ProtocolVersion);
	Out.Add(static_cast<uint8>(Type));
}

void WoTShareAppendU16(TArray<uint8>& Out, uint16 Value)
{
	Out.Add(static_cast<uint8>(Value >> 8));
	Out.Add(static_cast<uint8>(Value));
}

void WoTShareAppendU64(TArray<uint8>& Out, uint64 Value)
{
	for (int32 Shift = 56; Shift >= 0; Shift -= 8)
	{
		Out.Add(static_cast<uint8>(Value >> Shift));
	}
}

uint16 WoTShareReadU16(const uint8* Bytes)
{
	return static_cast<uint16>((static_cast<uint16>(Bytes[0]) << 8) | Bytes[1]);
}

uint64 WoTShareReadU64(const uint8* Bytes)
{
	uint64 Value = 0;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Value = (Value << 8) | Bytes[Index];
	}
	return Value;
}

bool WoTShareHasHeader(TConstArrayView<uint8> Bytes,
	WoTInstallerShare::EMessageType Type)
{
	return Bytes.Num() >= WoTShareHeaderBytes
		&& FMemory::Memcmp(Bytes.GetData(), WoTShareMagic,
			WoTInstallerShare::MagicByteCount) == 0
		&& Bytes[WoTInstallerShare::MagicByteCount]
			== WoTInstallerShare::ProtocolVersion
		&& Bytes[WoTInstallerShare::MagicByteCount + 1]
			== static_cast<uint8>(Type);
}

bool WoTShareIsControl(uint32 CodePoint)
{
	return CodePoint < 0x20 || (CodePoint >= 0x7F && CodePoint <= 0x9F);
}

/** Strict UTF-8 (no overlong forms, surrogates or values past U+10FFFF) with
 * no control characters. */
bool WoTShareIsCleanUtf8(const uint8* Bytes, int32 Count)
{
	int32 Index = 0;
	while (Index < Count)
	{
		const uint8 Lead = Bytes[Index];
		int32 Extra = 0;
		uint32 Minimum = 0;
		uint32 CodePoint = 0;
		if (Lead < 0x80)
		{
			CodePoint = Lead;
		}
		else if ((Lead & 0xE0) == 0xC0)
		{
			Extra = 1;
			Minimum = 0x80;
			CodePoint = Lead & 0x1F;
		}
		else if ((Lead & 0xF0) == 0xE0)
		{
			Extra = 2;
			Minimum = 0x800;
			CodePoint = Lead & 0x0F;
		}
		else if ((Lead & 0xF8) == 0xF0)
		{
			Extra = 3;
			Minimum = 0x10000;
			CodePoint = Lead & 0x07;
		}
		else
		{
			return false;
		}
		if (Count - Index - 1 < Extra)
		{
			return false;
		}
		for (int32 Continuation = 1; Continuation <= Extra; ++Continuation)
		{
			const uint8 Byte = Bytes[Index + Continuation];
			if ((Byte & 0xC0) != 0x80)
			{
				return false;
			}
			CodePoint = (CodePoint << 6) | (Byte & 0x3F);
		}
		if ((Extra > 0 && CodePoint < Minimum) || CodePoint > 0x10FFFF
			|| (CodePoint >= 0xD800 && CodePoint <= 0xDFFF)
			|| WoTShareIsControl(CodePoint))
		{
			return false;
		}
		Index += Extra + 1;
	}
	return true;
}

/** The label's UTF-8 bytes, or false when it breaks the label rules. */
bool WoTShareLabelBytes(FStringView Label, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	if (Label.IsEmpty())
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(Label.GetData(), Label.Len());
	const int32 Count = Utf8.Length();
	if (Count < 1 || Count > WoTInstallerShare::MaxLabelBytes)
	{
		return false;
	}
	const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
	if (!WoTShareIsCleanUtf8(Bytes, Count))
	{
		return false;
	}
	OutBytes.Append(Bytes, Count);
	return true;
}

bool WoTShareDecodeLabel(const uint8* Bytes, int32 Count, FString& OutLabel)
{
	if (Count < 1 || Count > WoTInstallerShare::MaxLabelBytes
		|| !WoTShareIsCleanUtf8(Bytes, Count))
	{
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes), Count);
	OutLabel = FString(Converted.Length(), Converted.Get());
	return !OutLabel.IsEmpty();
}

#if WITH_AUTOMATION_TESTS
FCriticalSection GWoTShareWineHostSysnameLock;
TOptional<FString> GWoTShareWineHostSysnameForTesting;
#endif

#if PLATFORM_WINDOWS
/** Wine's host OS name ("Linux", "Darwin"), or empty on native Windows. */
FString WoTShareWineHostSysname()
{
	if (!WoTIsRunningUnderWine())
	{
		return FString();
	}
	using FWineGetHostVersion = void(__cdecl*)(const char**, const char**);
	const HMODULE Ntdll = ::GetModuleHandleW(L"ntdll.dll");
	if (Ntdll == nullptr)
	{
		return FString();
	}
	const FWineGetHostVersion GetHostVersion = reinterpret_cast<FWineGetHostVersion>(
		reinterpret_cast<void*>(::GetProcAddress(Ntdll, "wine_get_host_version")));
	if (GetHostVersion == nullptr)
	{
		return FString();
	}
	const char* Sysname = nullptr;
	const char* Release = nullptr;
	GetHostVersion(&Sysname, &Release);
	return Sysname == nullptr ? FString() : FString(UTF8_TO_TCHAR(Sysname));
}
#endif
} // namespace

FWoTShareId FWoTShareId::Random()
{
	const FGuid Guid = FGuid::NewGuid();
	const uint32 Parts[4] = {Guid.A, Guid.B, Guid.C, Guid.D};
	FWoTShareId Id;
	for (int32 Part = 0; Part < 4; ++Part)
	{
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Id.Bytes[Part * 4 + Byte] =
				static_cast<uint8>(Parts[Part] >> (24 - Byte * 8));
		}
	}
	return Id;
}

namespace WoTInstallerShare
{
bool IsValidLabel(FStringView Label)
{
	TArray<uint8> Bytes;
	return WoTShareLabelBytes(Label, Bytes);
}

TArray<uint8> EncodeOffer(const FWoTShareOffer& Offer)
{
	TArray<uint8> Label;
	if (!WoTShareLabelBytes(Offer.Label, Label))
	{
		return {};
	}
	TArray<uint8> Out;
	Out.Reserve(WoTShareOfferFixedBytes + Label.Num());
	WoTShareAppendHeader(Out, EMessageType::Offer);
	Out.Append(Offer.SessionId.Bytes, FWoTShareId::ByteCount);
	Out.Append(Offer.Sha256.Bytes, FWoTSha256::ByteCount);
	WoTShareAppendU64(Out, Offer.Size);
	Out.Add(static_cast<uint8>(Label.Num()));
	Out.Append(Label);
	return Out;
}

TArray<uint8> EncodeRequest(const FWoTShareRequest& Request)
{
	TArray<uint8> Label;
	if (!WoTShareLabelBytes(Request.Label, Label))
	{
		return {};
	}
	TArray<uint8> Out;
	Out.Reserve(WoTShareRequestFixedBytes + Label.Num());
	WoTShareAppendHeader(Out, EMessageType::Request);
	Out.Append(Request.SessionId.Bytes, FWoTShareId::ByteCount);
	Out.Append(Request.Token.Bytes, FWoTShareId::ByteCount);
	WoTShareAppendU16(Out, Request.TcpPort);
	WoTShareAppendU64(Out, Request.ResumeOffset);
	Out.Add(static_cast<uint8>(Label.Num()));
	Out.Append(Label);
	return Out;
}

TArray<uint8> EncodeDataHeader(const FWoTShareDataHeader& Header)
{
	TArray<uint8> Out;
	Out.Reserve(DataHeaderBytes);
	WoTShareAppendHeader(Out, EMessageType::Data);
	Out.Append(Header.Token.Bytes, FWoTShareId::ByteCount);
	WoTShareAppendU64(Out, Header.Offset);
	WoTShareAppendU64(Out, Header.Length);
	return Out;
}

TOptional<FWoTShareOffer> DecodeOffer(TConstArrayView<uint8> Bytes)
{
	if (!WoTShareHasHeader(Bytes, EMessageType::Offer)
		|| Bytes.Num() < WoTShareOfferFixedBytes)
	{
		return {};
	}
	const uint8* Cursor = Bytes.GetData() + WoTShareHeaderBytes;
	FWoTShareOffer Offer;
	FMemory::Memcpy(Offer.SessionId.Bytes, Cursor, FWoTShareId::ByteCount);
	Cursor += FWoTShareId::ByteCount;
	FMemory::Memcpy(Offer.Sha256.Bytes, Cursor, FWoTSha256::ByteCount);
	Cursor += FWoTSha256::ByteCount;
	Offer.Size = WoTShareReadU64(Cursor);
	Cursor += 8;
	const int32 LabelBytes = *Cursor++;
	if (Bytes.Num() != WoTShareOfferFixedBytes + LabelBytes
		|| !WoTShareDecodeLabel(Cursor, LabelBytes, Offer.Label))
	{
		return {};
	}
	return Offer;
}

TOptional<FWoTShareRequest> DecodeRequest(TConstArrayView<uint8> Bytes)
{
	if (!WoTShareHasHeader(Bytes, EMessageType::Request)
		|| Bytes.Num() < WoTShareRequestFixedBytes)
	{
		return {};
	}
	const uint8* Cursor = Bytes.GetData() + WoTShareHeaderBytes;
	FWoTShareRequest Request;
	FMemory::Memcpy(Request.SessionId.Bytes, Cursor, FWoTShareId::ByteCount);
	Cursor += FWoTShareId::ByteCount;
	FMemory::Memcpy(Request.Token.Bytes, Cursor, FWoTShareId::ByteCount);
	Cursor += FWoTShareId::ByteCount;
	Request.TcpPort = WoTShareReadU16(Cursor);
	Cursor += 2;
	Request.ResumeOffset = WoTShareReadU64(Cursor);
	Cursor += 8;
	const int32 LabelBytes = *Cursor++;
	if (Bytes.Num() != WoTShareRequestFixedBytes + LabelBytes
		|| !WoTShareDecodeLabel(Cursor, LabelBytes, Request.Label))
	{
		return {};
	}
	return Request;
}

TOptional<FWoTShareDataHeader> DecodeDataHeader(TConstArrayView<uint8> Bytes)
{
	if (!WoTShareHasHeader(Bytes, EMessageType::Data)
		|| Bytes.Num() != DataHeaderBytes)
	{
		return {};
	}
	const uint8* Cursor = Bytes.GetData() + WoTShareHeaderBytes;
	FWoTShareDataHeader Header;
	FMemory::Memcpy(Header.Token.Bytes, Cursor, FWoTShareId::ByteCount);
	Cursor += FWoTShareId::ByteCount;
	Header.Offset = WoTShareReadU64(Cursor);
	Header.Length = WoTShareReadU64(Cursor + 8);
	if (Header.Offset + Header.Length < Header.Offset)
	{
		return {};
	}
	return Header;
}

bool IsPrivatePeerAddress(uint32 HostOrderAddress, bool bAllowLoopback)
{
	const uint32 A = HostOrderAddress >> 24;
	const uint32 B = (HostOrderAddress >> 16) & 0xFF;
	if (A == 10 || (A == 172 && (B & 0xF0) == 16) || (A == 192 && B == 168)
		|| (A == 169 && B == 254))
	{
		return true;
	}
	return A == 127 && bAllowLoopback;
}

TArray<uint32> DirectedBroadcasts(
	TConstArrayView<FWoTShareAdapterAddress> Adapters)
{
	TArray<uint32> Result;
	for (const FWoTShareAdapterAddress& Adapter : Adapters)
	{
		const uint32 Address = Adapter.Address;
		const bool bLinkLocal = (Address >> 16) == 0xA9FE;
		if (!Adapter.bUp || Adapter.bLoopback || Adapter.bTunnel
			|| Adapter.bTentative || Address == 0 || (Address >> 24) == 127
			|| bLinkLocal || !IsPrivatePeerAddress(Address, false)
			|| Adapter.PrefixLength == 0 || Adapter.PrefixLength >= 31)
		{
			continue;
		}
		const uint32 Mask = 0xFFFFFFFFu << (32 - Adapter.PrefixLength);
		Result.AddUnique((Address & Mask) | ~Mask);
	}
	return Result;
}

TArray<uint32> OfferDestinations(
	TConstArrayView<FWoTShareAdapterAddress> Adapters)
{
	TArray<uint32> Result;
	Result.Add(LimitedBroadcast);
	for (const uint32 Directed : DirectedBroadcasts(Adapters))
	{
		Result.AddUnique(Directed);
	}
	return Result;
}

const TCHAR* LocalLabelFor(bool bPlatformMac, bool bUnderWine,
	FStringView HostSysname)
{
	if (bPlatformMac)
	{
		return LabelMac;
	}
	if (!bUnderWine)
	{
		return LabelWindowsPC;
	}
	return FStringView(TEXT("Darwin")).Equals(HostSysname,
		ESearchCase::IgnoreCase) ? LabelMac : LabelSteamDeck;
}

FString LocalLabel()
{
#if PLATFORM_WINDOWS
	const FString Sysname = WineHostSysname();
	return LocalLabelFor(false, WoTIsRunningUnderWine(), Sysname);
#else
	return LocalLabelFor(PLATFORM_MAC != 0, false, FStringView());
#endif
}

FString WineHostSysname()
{
#if WITH_AUTOMATION_TESTS
	{
		FScopeLock Lock(&GWoTShareWineHostSysnameLock);
		if (GWoTShareWineHostSysnameForTesting.IsSet())
		{
			return GWoTShareWineHostSysnameForTesting.GetValue();
		}
	}
#endif
#if PLATFORM_WINDOWS
	return WoTShareWineHostSysname();
#else
	return FString();
#endif
}

#if WITH_AUTOMATION_TESTS
void SetWineHostSysnameForTesting(TOptional<FString> Sysname)
{
	FScopeLock Lock(&GWoTShareWineHostSysnameLock);
	GWoTShareWineHostSysnameForTesting = MoveTemp(Sysname);
}
#endif

bool ReceiverStartsAutomaticallyFor(bool bPlatformWindows, bool bUnderWine,
	FStringView HostSysname)
{
	return bPlatformWindows && bUnderWine
		&& FStringView(TEXT("Linux")).Equals(HostSysname, ESearchCase::IgnoreCase);
}

bool ReceiverStartsAutomatically()
{
#if PLATFORM_WINDOWS
	const bool bUnderWine = WoTIsRunningUnderWine();
	return ReceiverStartsAutomaticallyFor(true, bUnderWine,
		bUnderWine ? WineHostSysname() : FString());
#else
	return false;
#endif
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
bool HarnessLoopback()
{
	return FParse::Param(FCommandLine::Get(), TEXT("wottestlanloopback"));
}

bool HarnessReceiverOff()
{
	return FPlatformMisc::GetEnvironmentVariable(
		TEXT("WOT_TEST_LAN_RECEIVER_OFF")).TrimStartAndEnd().Equals(TEXT("1"));
}
#endif

FString DisplayLabel(FStringView PeerLabel)
{
	const TCHAR* const KnownLabels[] = {LabelWindowsPC, LabelMac, LabelSteamDeck};
	for (const TCHAR* Known : KnownLabels)
	{
		if (PeerLabel.Equals(Known, ESearchCase::CaseSensitive))
		{
			return Known;
		}
	}
	return LabelOtherComputer;
}

const TCHAR* PeerToken(FStringView PeerLabel)
{
	if (PeerLabel.Equals(LabelWindowsPC, ESearchCase::CaseSensitive))
	{
		return TEXT("windows-pc");
	}
	if (PeerLabel.Equals(LabelMac, ESearchCase::CaseSensitive))
	{
		return TEXT("mac");
	}
	if (PeerLabel.Equals(LabelSteamDeck, ESearchCase::CaseSensitive))
	{
		return TEXT("steam-deck");
	}
	return TEXT("other");
}

FString SendingNotice(FStringView DisplayedLabel)
{
	return FString(TEXT("Sending the original game installer to "))
		+ FString(DisplayedLabel) + TEXT("...");
}

FString SentNotice(FStringView DisplayedLabel)
{
	return SendingNotice(DisplayedLabel) + TEXT(" sent");
}

FString StoppedNotice(FStringView DisplayedLabel)
{
	return SendingNotice(DisplayedLabel) + TEXT(" stopped");
}
}
