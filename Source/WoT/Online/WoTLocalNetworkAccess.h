#pragma once

#include "CoreMinimal.h"

/**
 * Whether the OS lets this process reach hosts on the local network.
 *
 * macOS Local Network privacy blocks an app's LAN traffic once the player has
 * answered "Don't Allow" (or never been asked). The block is silent where it
 * matters most: a subnet-directed broadcast still "succeeds" and is dropped,
 * so the LAN browser just says "No sessions found." There is no API to query
 * the setting; a one-byte UDP send to an on-link host fails with
 * EHOSTUNREACH only while the block is active (measured 2026-10-03).
 */
enum class EWoTLocalNetworkAccess : uint8
{
	/** Nothing to probe (no local interface) or not decidable. */
	Unknown,
	Allowed,
	Denied,
};

/** One local IPv4 interface the probe may test (host byte order). */
struct FWoTLocalNetworkInterface
{
	FString Name;
	uint32 Address = 0;
	uint32 Netmask = 0;
};

enum class EWoTLocalNetworkSend : uint8
{
	Sent,
	/** EHOSTUNREACH: the privacy block (or no route at all). */
	HostUnreachable,
	/** Any other failure; it says nothing about the privacy setting. */
	OtherError,
};

namespace WoTLocalNetworkAccess
{
	using FSend = TFunction<EWoTLocalNetworkSend(uint32 TargetHostOrder)>;

	/**
	 * Pure decision over the given interfaces. Each interface is probed at an
	 * on-link address (its network + 1, or + 2 when that is the interface
	 * itself). Denied only when at least one interface was probed and every
	 * probe came back HostUnreachable; any successful send means Allowed.
	 * OutDetail names each interface's outcome for the heartbeat.
	 */
	EWoTLocalNetworkAccess Classify(
		const TArray<FWoTLocalNetworkInterface>& Interfaces,
		const FSend& Send, FString* OutDetail = nullptr);

	/** The platform probe. Mac: real interfaces and a one-byte UDP send to
	 * the discard port. Other platforms have no such gate: Allowed. */
	EWoTLocalNetworkAccess Probe(FString* OutDetail = nullptr);

	/** Whether the OS asks the player before WoT's first local-network
	 * traffic (macOS Local Network privacy). The game explains the prompt
	 * before triggering it. */
	bool OSAsksForAccess();

	/** System Settings > Privacy & Security > Local Network. */
	const TCHAR* SettingsURL();

	/** Opens SettingsURL() (Mac only; a no-op elsewhere). */
	void OpenSettings();

	const TCHAR* LexToString(EWoTLocalNetworkAccess Access);

#if WITH_AUTOMATION_TESTS
	void SetProbeOverrideForTesting(TFunction<EWoTLocalNetworkAccess()> Override);
	void SetOpenSettingsOverrideForTesting(TFunction<void()> Override);
	void SetOSAsksForAccessOverrideForTesting(TOptional<bool> Override);
#endif
}
