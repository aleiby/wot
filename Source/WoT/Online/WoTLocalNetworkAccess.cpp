#include "WoTLocalNetworkAccess.h"

#include "HAL/PlatformProcess.h"

#if PLATFORM_MAC
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
#if WITH_AUTOMATION_TESTS
TFunction<EWoTLocalNetworkAccess()>& WoTLocalNetworkProbeOverride()
{
	static TFunction<EWoTLocalNetworkAccess()> Override;
	return Override;
}

TFunction<void()>& WoTLocalNetworkOpenSettingsOverride()
{
	static TFunction<void()> Override;
	return Override;
}

TOptional<bool>& WoTLocalNetworkOSAsksOverride()
{
	static TOptional<bool> Override;
	return Override;
}
#endif

FString WoTLocalNetworkDottedQuad(uint32 HostOrder)
{
	return FString::Printf(TEXT("%u.%u.%u.%u"), (HostOrder >> 24) & 0xff,
		(HostOrder >> 16) & 0xff, (HostOrder >> 8) & 0xff, HostOrder & 0xff);
}

#if PLATFORM_MAC
TArray<FWoTLocalNetworkInterface> WoTLocalNetworkInterfaces()
{
	TArray<FWoTLocalNetworkInterface> Result;
	ifaddrs* Interfaces = nullptr;
	if (getifaddrs(&Interfaces) != 0 || Interfaces == nullptr)
	{
		return Result;
	}
	for (ifaddrs* Cursor = Interfaces; Cursor != nullptr;
		Cursor = Cursor->ifa_next)
	{
		const unsigned int Flags = Cursor->ifa_flags;
		if (Cursor->ifa_addr == nullptr || Cursor->ifa_netmask == nullptr
			|| Cursor->ifa_addr->sa_family != AF_INET
			|| (Flags & IFF_UP) == 0 || (Flags & IFF_RUNNING) == 0
			|| (Flags & IFF_LOOPBACK) != 0 || (Flags & IFF_BROADCAST) == 0)
		{
			continue;
		}
		FWoTLocalNetworkInterface Entry;
		Entry.Name = UTF8_TO_TCHAR(Cursor->ifa_name);
		Entry.Address = ntohl(reinterpret_cast<const sockaddr_in*>(
			Cursor->ifa_addr)->sin_addr.s_addr);
		Entry.Netmask = ntohl(reinterpret_cast<const sockaddr_in*>(
			Cursor->ifa_netmask)->sin_addr.s_addr);
		Result.Add(MoveTemp(Entry));
	}
	freeifaddrs(Interfaces);
	return Result;
}

EWoTLocalNetworkSend WoTLocalNetworkSendOneByte(uint32 TargetHostOrder)
{
	const int Socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (Socket < 0)
	{
		return EWoTLocalNetworkSend::OtherError;
	}
	sockaddr_in Target = {};
	Target.sin_family = AF_INET;
	Target.sin_port = htons(9); // discard
	Target.sin_addr.s_addr = htonl(TargetHostOrder);
	const char Byte = 0;
	const ssize_t Sent = sendto(Socket, &Byte, 1, 0,
		reinterpret_cast<const sockaddr*>(&Target), sizeof(Target));
	const int SendError = Sent < 0 ? errno : 0;
	close(Socket);
	if (Sent == 1)
	{
		return EWoTLocalNetworkSend::Sent;
	}
	return SendError == EHOSTUNREACH
		? EWoTLocalNetworkSend::HostUnreachable
		: EWoTLocalNetworkSend::OtherError;
}
#endif
}

namespace WoTLocalNetworkAccess
{
EWoTLocalNetworkAccess Classify(
	const TArray<FWoTLocalNetworkInterface>& Interfaces,
	const FSend& Send, FString* OutDetail)
{
	int32 Probed = 0;
	int32 Unreachable = 0;
	bool bAnySent = false;
	TArray<FString> Parts;
	for (const FWoTLocalNetworkInterface& Interface : Interfaces)
	{
		// The link-local range is never a player's LAN, and a host-only mask
		// has no on-link neighbour to address.
		if (Interface.Address == 0 || (Interface.Address >> 16) == 0xA9FE
			|| Interface.Netmask == 0xFFFFFFFFu || Interface.Netmask == 0)
		{
			continue;
		}
		const uint32 Network = Interface.Address & Interface.Netmask;
		uint32 Target = Network | 1u;
		if (Target == Interface.Address)
		{
			Target = Network | 2u;
		}
		const EWoTLocalNetworkSend Outcome = Send ? Send(Target)
			: EWoTLocalNetworkSend::OtherError;
		++Probed;
		const TCHAR* OutcomeText = TEXT("error");
		if (Outcome == EWoTLocalNetworkSend::Sent)
		{
			bAnySent = true;
			OutcomeText = TEXT("sent");
		}
		else if (Outcome == EWoTLocalNetworkSend::HostUnreachable)
		{
			++Unreachable;
			OutcomeText = TEXT("unreachable");
		}
		Parts.Add(FString::Printf(TEXT("%s:%s=%s"), *Interface.Name,
			*WoTLocalNetworkDottedQuad(Target), OutcomeText));
	}
	EWoTLocalNetworkAccess Access = EWoTLocalNetworkAccess::Unknown;
	if (bAnySent)
	{
		Access = EWoTLocalNetworkAccess::Allowed;
	}
	else if (Probed > 0 && Unreachable == Probed)
	{
		Access = EWoTLocalNetworkAccess::Denied;
	}
	if (OutDetail)
	{
		*OutDetail = FString::Printf(TEXT("access=%s probes=%s"),
			LexToString(Access),
			Parts.IsEmpty() ? TEXT("none") : *FString::Join(Parts, TEXT(",")));
	}
	return Access;
}

EWoTLocalNetworkAccess Probe(FString* OutDetail)
{
#if WITH_AUTOMATION_TESTS
	if (WoTLocalNetworkProbeOverride())
	{
		const EWoTLocalNetworkAccess Access = WoTLocalNetworkProbeOverride()();
		if (OutDetail)
		{
			*OutDetail = FString::Printf(TEXT("access=%s probes=override"),
				LexToString(Access));
		}
		return Access;
	}
#endif
#if PLATFORM_MAC
	return Classify(WoTLocalNetworkInterfaces(), &WoTLocalNetworkSendOneByte,
		OutDetail);
#else
	if (OutDetail)
	{
		*OutDetail = TEXT("access=allowed probes=not-applicable");
	}
	return EWoTLocalNetworkAccess::Allowed;
#endif
}

bool OSAsksForAccess()
{
#if WITH_AUTOMATION_TESTS
	if (WoTLocalNetworkOSAsksOverride().IsSet())
	{
		return WoTLocalNetworkOSAsksOverride().GetValue();
	}
#endif
	return PLATFORM_MAC != 0;
}

const TCHAR* SettingsURL()
{
	return TEXT("x-apple.systempreferences:com.apple.preference.security?Privacy_LocalNetwork");
}

void OpenSettings()
{
#if WITH_AUTOMATION_TESTS
	if (WoTLocalNetworkOpenSettingsOverride())
	{
		WoTLocalNetworkOpenSettingsOverride()();
		return;
	}
#endif
#if PLATFORM_MAC
	FPlatformProcess::LaunchURL(SettingsURL(), nullptr, nullptr);
#endif
}

const TCHAR* LexToString(EWoTLocalNetworkAccess Access)
{
	switch (Access)
	{
	case EWoTLocalNetworkAccess::Allowed: return TEXT("allowed");
	case EWoTLocalNetworkAccess::Denied: return TEXT("denied");
	case EWoTLocalNetworkAccess::Unknown:
	default: return TEXT("unknown");
	}
}

#if WITH_AUTOMATION_TESTS
void SetProbeOverrideForTesting(TFunction<EWoTLocalNetworkAccess()> Override)
{
	WoTLocalNetworkProbeOverride() = MoveTemp(Override);
}

void SetOpenSettingsOverrideForTesting(TFunction<void()> Override)
{
	WoTLocalNetworkOpenSettingsOverride() = MoveTemp(Override);
}

void SetOSAsksForAccessOverrideForTesting(TOptional<bool> Override)
{
	WoTLocalNetworkOSAsksOverride() = Override;
}
#endif
}
