#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "UObject/Object.h"
#include "WoTVRSettings.generated.h"

/** The mode a launch starts in (design section 5.3, decision F). */
UENUM()
enum class EWoTVRStartupMode : uint8
{
	/** VR when a usable headset session starts, flat otherwise. */
	Auto,
	Flat,
	/** VR, explicitly: a failure is reported and play continues flat. */
	VR
};

/** Calibration posture (design section 6). Both map the confirmed neutral pose
 * to the avatar's standing simulation eye; capsule size and eye height never
 * change with it. */
UENUM()
enum class EWoTVRPosture : uint8
{
	Standing,
	Seated
};

UENUM()
enum class EWoTVRTurnMode : uint8
{
	/** One deflection turns by SnapTurnDegrees. */
	Snap,
	/** Held input turns at SmoothTurnDegreesPerSecond. */
	Smooth
};

/** The plugin's own keys (WoT.VR.Key rebinds them). */
UENUM()
enum class EWoTVRKeyAction : uint8
{
	ModeSwitch,
	Recentre,
	TurnLeft,
	TurnRight
};

// Persisted VR preferences. The plugin keeps its own GameUserSettings section,
// like the onboarding aids, so a build without the plugin carries no trace of
// it. The XR gate (WoTVRGate.h) reads the master enable straight from this
// section before UObjects exist.
//
// Guarded player builds never read or write the GameUserSettings hierarchy
// (the guarded-bootstrap engine patch), so there these preferences keep their
// compiled defaults for every launch and -vr is the way to play in VR.
UCLASS(Config=GameUserSettings)
class WOTVR_API UWoTVRSettings : public UObject
{
	GENERATED_BODY()

public:
	/** Master enable, default off: the owner rules on the shipped default.
	 * While it is off the game creates no XR instance unless -vr is given, and
	 * the plugin attaches nothing to the game. It takes effect for XR at the
	 * next launch (XR can only start before the renderer does). */
	UPROPERTY(Config)
	bool bEnableVR = false;

	UPROPERTY(Config)
	EWoTVRStartupMode StartupMode = EWoTVRStartupMode::Auto;

	UPROPERTY(Config)
	EWoTVRPosture Posture = EWoTVRPosture::Standing;

	/** Optional calibration: the user's measured standing eye-to-floor height
	 * in metres (0 = not measured). Reported against the avatar's eye height;
	 * the eye-level tracking origin cannot find the real floor, so it never
	 * moves the view. */
	UPROPERTY(Config)
	float StandingEyeHeightMeters = 0.0f;

	UPROPERTY(Config)
	EWoTVRTurnMode TurnMode = EWoTVRTurnMode::Snap;

	/** The approved snap angle (decision C). */
	UPROPERTY(Config)
	float SnapTurnDegrees = 30.0f;

	/** The initial smooth turn rate (decision C). */
	UPROPERTY(Config)
	float SmoothTurnDegreesPerSecond = 90.0f;

	/** Switch between VR and flat, anywhere (menus, front end, while dead).
	 * F7: no game binding, managed binding default or retail action uses it. */
	UPROPERTY(Config)
	FKey ModeSwitchKey = EKeys::F7;

	/** Recentre: confirm the current pose as neutral. F8, unused likewise. */
	UPROPERTY(Config)
	FKey RecentreKey = EKeys::F8;

	/** Keyboard turning in VR (the gamepad's right stick X turns too). C and V
	 * sit beside the movement keys and no binding uses them; a turn key the
	 * player binds to a game action stays that action's. */
	UPROPERTY(Config)
	FKey TurnLeftKey = EKeys::C;

	UPROPERTY(Config)
	FKey TurnRightKey = EKeys::V;

	/** The live, persisted settings: the config-backed class default object. */
	static UWoTVRSettings& Get();

	/** Write one change through SaveConfig and broadcast OnChanged; setting the
	 * current value is a no-op. While automation tests run nothing is written
	 * to disk: a spec must never leave the master enable on for the next
	 * launch, which would start a headset runtime. */
	void SetEnableVR(bool bValue);
	void SetStartupMode(EWoTVRStartupMode Value);
	void SetPosture(EWoTVRPosture Value);
	void SetStandingEyeHeightMeters(float Value);
	void SetTurnMode(EWoTVRTurnMode Value);
	/** Rebinds one of the plugin's keys (an invalid key unbinds it). Every use
	 * reads the setting, so the new key acts at once. */
	void SetKey(EWoTVRKeyAction Action, const FKey& Key);
	const FKey& GetKey(EWoTVRKeyAction Action) const;

	FSimpleMulticastDelegate OnChanged;

private:
	void Persist();
};
