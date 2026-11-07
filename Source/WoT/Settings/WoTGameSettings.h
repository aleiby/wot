#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "WoTEarlyUI.h"

class UPlayerInput;
class UWorld;

/** Only bindings already present in DefaultInput.ini are editable. */
enum class EWoTManagedBinding : uint8
{
	MoveForward, MoveBackward, MoveRight, MoveLeft,
	TurnLeft, TurnRight, LookUp, LookDown,
	Jump, Fire, NextItem, PrevItem, ShowScores, InGameMenu,
	Type, Talk, TeamTalk, ShowInventoryInfo, ViewUp, ViewDown,
	NextHand, PrevHand, Drop, DropTainted,
	SShot, ToggleFullscreen, ServerEditCitadel, ShowHelp,
	Walking, Look, LookToggle, FreeLook, CenterView, Strafe,
	PlayTaunt1, PlayTaunt2, PlayTaunt3, PlayTaunt4, PlayTaunt5,
	PlayTaunt6, PlayTauntRandom, Brightness, Pause,
	SelectHand1, SelectHand2, SelectHand3, SelectHand4, SelectHand5,
	SelectHand6, SelectHand7, SelectHand8, SelectHand9, SelectHand0,
};

enum class EWoTGraphicsQuality : uint8 { Low, Medium, High, Epic };

struct FWoTManagedKeyBinding
{
	EWoTManagedBinding Action = EWoTManagedBinding::Jump;
	FKey Primary;
	FKey Secondary;

	bool operator==(const FWoTManagedKeyBinding& Other) const
	{
		return Action == Other.Action && Primary == Other.Primary
			&& Secondary == Other.Secondary;
	}
};

/** Draftable post-activation settings. This never replaces authenticated EarlyUI. */
struct FWoTGameSettings
{
	// original/System/{Default,WoT}.ini: SoundVolume=200, normalized for UE5's
	// main-submix [0,1] gain.
	inline static constexpr float RetailMasterVolume = 200.0f / 255.0f;

	float MouseSensitivity = 1.0f;
	float Brightness = 0.6f;
	bool bInvertMouseY = false;
	uint16 Width = 1280;
	uint16 Height = 720;
	EWoTWindowMode WindowMode = EWoTWindowMode::Windowed;
	bool bVSync = true;
	EWoTGraphicsQuality GraphicsQuality = EWoTGraphicsQuality::Epic;
	uint16 FrameRateLimit = 0;
	float MasterVolume = RetailMasterVolume;
	// Retail menuOptions options 6-10 / PlayerPawn ViewBob default.
	// PlayerPawn defaultproperties and shipped User.ini both enable this option.
	bool bAlwaysMouseLook = true;
	// PlayerPawn.bLookUpStairs and BaseHUD.bPlayerIDEnabled, exposed by the
	// retail Auto Slope Look and Player ID rows.
	bool bAutoSlopeLook = false;
	bool bPlayerIDEnabled = true;
	bool bLookSpring = false;
	uint8 Crosshair = 13;
	float ViewBob = 0.016f;
	bool bJoystickEnabled = true;
	// Controller layer (version 10). The dead zone is radial, as a fraction of
	// full deflection; sensitivity multiplies the look rates.
	float ControllerLookSensitivity = 1.0f;
	bool bControllerInvertY = false;
	float ControllerDeadZone = 0.20f;
	uint8 GoreDetailLevel = 2;
	TArray<FWoTManagedKeyBinding> Bindings;

	static FWoTGameSettings Defaults();
	bool Validate(FString& OutError) const;
	bool operator==(const FWoTGameSettings& Other) const;
	FWoTManagedKeyBinding* FindBinding(EWoTManagedBinding Action);
	const FWoTManagedKeyBinding* FindBinding(EWoTManagedBinding Action) const;
	/** Replace only the game's managed local mappings and mouse axis properties. */
	void ApplyToInput(UPlayerInput& Input) const;

	/** Closed catalog in retail Controls order; the UI never invents actions. */
	static TConstArrayView<EWoTManagedBinding> ManagedActions();
	/** DefaultInput.ini action/axis identity for a managed binding. */
	static const TCHAR* ActionName(EWoTManagedBinding Action);
	/** Keyboard or mouse keys only; gamepad/touch keys are refused. */
	static bool IsEditableKey(const FKey& Key);
};

/** Closed preference-file codec; input is bounded to EarlyUI's 4096-byte limit. */
struct FWoTGameSettingsCodec
{
	static bool Parse(TConstArrayView<uint8> Bytes, FWoTGameSettings& Out,
		FString& OutError);
	static FString Serialize(const FWoTGameSettings& Settings);
};

/** Runtime projection only. None of these methods persists engine config. */
struct FWoTGameSettingsAdapter
{
	static bool PreviewVideo(const FWoTGameSettings& Settings,
		FString& OutError);
	static void RevertVideo(const FWoTGameSettings& Confirmed);
	static void ApplyVideoOnStartup(const FWoTGameSettings& Settings);
	static float AdvanceBrightness(float Brightness);
	static FString BrightnessMessage(float Brightness);
	/** Exponent for exact display-space D^(1/g), where g is retail's Glide ramp. */
	static float RampInvGammaForBrightness(float Brightness);
	static void ApplyBrightness(float Brightness);
	static void ApplyAudio(const FWoTGameSettings& Settings);
	/** The master volume ApplyAudio last projected onto every audio device. */
	static float GetAppliedMasterVolume();
	static bool HasReviewDisplayOverrides();
#if WITH_AUTOMATION_TESTS
	using FDisplayProjection = TFunction<void(
		const FWoTGameSettings&, bool, bool)>;
	static void SetDisplayProjectionForTesting(
		FDisplayProjection Projection);
	using FBrightnessProjection = TFunction<void(float)>;
	static void SetBrightnessProjectionForTesting(
		FBrightnessProjection Projection);
	/** Headless-world target for exercising the real display MPC projection. */
	static void SetBrightnessWorldForTesting(UWorld* World);
	/** Observes each device's main-submix master-volume write. */
	using FMasterVolumeProjection = TFunction<void(
		class FAudioDevice*, class USoundSubmix*, float)>;
	static void SetMasterVolumeProjectionForTesting(
		FMasterVolumeProjection Projection);
	static bool HasCommandLineFrameRateOverrideForTesting(
		bool bExecCmdsCanRun);
#endif
};
