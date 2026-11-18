// macOS Local Network privacy probe: the pure classification over injected
// interfaces and sends (the platform probe feeds it getifaddrs + sendto).
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Online/WoTLocalNetworkAccess.h"

namespace
{
FWoTLocalNetworkInterface WoTLocalNetworkSpecInterface(
	const TCHAR* Name, uint32 Address, uint32 Netmask)
{
	FWoTLocalNetworkInterface Interface;
	Interface.Name = Name;
	Interface.Address = Address;
	Interface.Netmask = Netmask;
	return Interface;
}

constexpr uint32 WoTLocalNetworkSpecIp(uint32 A, uint32 B, uint32 C, uint32 D)
{
	return (A << 24) | (B << 16) | (C << 8) | D;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalNetworkAccessClassifySpec,
	"WoT.Internet.LocalNetwork.Classify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLocalNetworkAccessClassifySpec::RunTest(const FString&)
{
	// Synthetic interfaces use RFC 5737 documentation networks.
	const TArray<FWoTLocalNetworkInterface> Interfaces{
		WoTLocalNetworkSpecInterface(TEXT("en0"),
			WoTLocalNetworkSpecIp(192, 0, 2, 161), 0xFFFFFC00u), // /22
		WoTLocalNetworkSpecInterface(TEXT("bridge0"),
			WoTLocalNetworkSpecIp(203, 0, 113, 3), 0xFFFFFE00u)}; // /23

	TArray<uint32> Targets;
	FString Detail;
	const EWoTLocalNetworkAccess Denied = WoTLocalNetworkAccess::Classify(
		Interfaces, [&Targets](uint32 Target)
		{
			Targets.Add(Target);
			return EWoTLocalNetworkSend::HostUnreachable;
		}, &Detail);
	TestEqual(TEXT("every on-link probe unreachable is the privacy block"),
		Denied, EWoTLocalNetworkAccess::Denied);
	TestEqual(TEXT("one probe per eligible interface"), Targets.Num(), 2);
	if (Targets.Num() == 2)
	{
		TestEqual(TEXT("probe addresses the network's first host"),
			Targets[0], WoTLocalNetworkSpecIp(192, 0, 0, 1));
		TestEqual(TEXT("each interface is probed on its own subnet"),
			Targets[1], WoTLocalNetworkSpecIp(203, 0, 112, 1));
	}
	TestTrue(TEXT("detail names the interface outcomes"),
		Detail.Contains(TEXT("access=denied"))
			&& Detail.Contains(TEXT("en0:192.0.0.1=unreachable")));

	int32 Calls = 0;
	TestEqual(TEXT("any delivered probe means access is allowed"),
		WoTLocalNetworkAccess::Classify(Interfaces, [&Calls](uint32)
		{
			return ++Calls == 1 ? EWoTLocalNetworkSend::HostUnreachable
				: EWoTLocalNetworkSend::Sent;
		}), EWoTLocalNetworkAccess::Allowed);

	TestEqual(TEXT("other send errors never claim a denial"),
		WoTLocalNetworkAccess::Classify(Interfaces, [](uint32)
		{
			return EWoTLocalNetworkSend::OtherError;
		}), EWoTLocalNetworkAccess::Unknown);

	TArray<uint32> SelfTargets;
	WoTLocalNetworkAccess::Classify({WoTLocalNetworkSpecInterface(TEXT("en1"),
		WoTLocalNetworkSpecIp(10, 0, 0, 1), 0xFFFFFF00u)},
		[&SelfTargets](uint32 Target)
		{
			SelfTargets.Add(Target);
			return EWoTLocalNetworkSend::Sent;
		});
	TestTrue(TEXT("a router-addressed interface probes its neighbour instead"),
		SelfTargets.Num() == 1
			&& SelfTargets[0] == WoTLocalNetworkSpecIp(10, 0, 0, 2));

	int32 IneligibleCalls = 0;
	FString IneligibleDetail;
	const EWoTLocalNetworkAccess Ineligible = WoTLocalNetworkAccess::Classify({
		WoTLocalNetworkSpecInterface(TEXT("en5"),
			WoTLocalNetworkSpecIp(169, 254, 7, 9), 0xFFFF0000u),
		WoTLocalNetworkSpecInterface(TEXT("utun0"),
			WoTLocalNetworkSpecIp(10, 8, 0, 2), 0xFFFFFFFFu)},
		[&IneligibleCalls](uint32)
		{
			++IneligibleCalls;
			return EWoTLocalNetworkSend::HostUnreachable;
		}, &IneligibleDetail);
	TestEqual(TEXT("link-local and host-only interfaces are not a LAN"),
		Ineligible, EWoTLocalNetworkAccess::Unknown);
	TestEqual(TEXT("nothing is sent without an eligible interface"),
		IneligibleCalls, 0);
	TestTrue(TEXT("no-probe detail is explicit"),
		IneligibleDetail.Contains(TEXT("probes=none")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLocalNetworkAccessPlatformSpec,
	"WoT.Internet.LocalNetwork.PlatformProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLocalNetworkAccessPlatformSpec::RunTest(const FString&)
{
	// No assertion on the Mac verdict itself: it reflects the privacy rule of
	// whatever launched the editor (a GUI editor can legitimately be denied).
	FString Detail;
	const EWoTLocalNetworkAccess Access = WoTLocalNetworkAccess::Probe(&Detail);
	TestTrue(TEXT("the probe always explains itself"),
		Detail.StartsWith(FString(TEXT("access="))
			+ WoTLocalNetworkAccess::LexToString(Access)));
#if !PLATFORM_MAC
	TestEqual(TEXT("platforms without the gate report allowed"),
		Access, EWoTLocalNetworkAccess::Allowed);
#endif
	TestTrue(TEXT("the settings link targets the Local Network pane"),
		FString(WoTLocalNetworkAccess::SettingsURL()).EndsWith(
			TEXT("Privacy_LocalNetwork")));
	return !HasAnyErrors();
}

#endif
