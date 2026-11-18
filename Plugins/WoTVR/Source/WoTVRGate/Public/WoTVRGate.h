#pragma once

#include "CoreMinimal.h"

// The XR gate (design section 5.3). Creating an OpenXR instance starts the
// installed runtime, and on a PC with SteamVR installed that launches SteamVR,
// so a process that is not going to play in VR must never create one. The
// engine reaches an HMD module only through the "HMD" modular feature: its
// pre-initialisation (FEngineLoop::PreInitHMDDevice, which on Windows creates
// and destroys an instance), the RHI's adapter query, the Slate renderer's
// stereo-only check, the audio and voice device queries and
// UEngine::InitializeHMDDevice. This module loads at PostConfigInit, before any
// of them, and while the gate is closed it withholds every HMD module from that
// feature list: modules registered before it are unregistered at startup and
// later registrations are unregistered as they arrive. Nothing then creates an
// XR instance, starts a runtime or creates an XR system.

/** What decides the gate, read before the engine pre-initialises HMD modules. */
struct FWoTVRGateInputs
{
	/** The saved master enable: GameUserSettings [/Script/WoTVR.WoTVRSettings] bEnableVR. */
	bool bMasterEnable = false;
	/** -vr on the command line: VR for this launch, even with the master enable off. */
	bool bFlagVR = false;
	/** -novr on the command line: no VR and no XR for this launch; wins over -vr. */
	bool bFlagNoVR = false;
	bool bCommandlet = false;
	bool bDedicatedServer = false;
	/** An editor process (UnrealEditor without -game, including automation and
	 * tool runs). It honours -vr only, never the saved master enable, so a
	 * developer's saved preference cannot start a headset runtime under the
	 * editor's specs and tools. */
	bool bEditor = false;
};

struct FWoTVRGateDecision
{
	bool bXRAllowed = false;
	/** Plain-language reason, for the log and the status command. */
	FString Reason;
};

struct WOTVRGATE_API FWoTVRGate
{
	/** The GameUserSettings section and key of the master enable (UWoTVRSettings). */
	static const TCHAR* SettingsSection();
	static const TCHAR* MasterEnableKey();

	/** The gate rule: never for commandlets and dedicated servers; never with
	 * -novr; with -vr; otherwise only with the saved master enable on, in a game
	 * process. */
	static FWoTVRGateDecision Decide(const FWoTVRGateInputs& Inputs);

	/** This process's inputs: the command line, the saved master enable and
	 * the process kind. */
	static FWoTVRGateInputs ReadProcessInputs();

	/** The inputs and decision taken when the gate module started. */
	static const FWoTVRGateInputs& GetProcessInputs();
	static const FWoTVRGateDecision& GetProcessDecision();

	/** HMD module registrations withheld from the engine since startup. */
	static int32 GetWithheldRegistrationCount();
};
