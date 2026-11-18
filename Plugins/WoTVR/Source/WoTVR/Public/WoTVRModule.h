#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "WoTVRSettings.h"

class IWoTVRSession;
class FWoTVRInputProcessor;
class UWoTVRPlayerSubsystem;
class UWorld;
struct FKeyEvent;

/** What the startup mode is chosen from (design section 5.3, decision F). */
struct FWoTVRStartupInputs
{
	/** The plugin participates this launch (WoTVRModule's activation). */
	bool bActive = false;
	EWoTVRStartupMode Preference = EWoTVRStartupMode::Auto;
	bool bFlagVR = false;
	bool bFlagNoVR = false;
};

enum class EWoTVRStartupRequest : uint8
{
	Flat,
	/** VR if a usable session starts; flat otherwise, quietly. */
	AutoVR,
	/** VR explicitly (-vr or the VR preference): a failure is logged as an
	 * error and play continues flat. */
	ExplicitVR
};

struct FWoTVRStartupDecision
{
	EWoTVRStartupRequest Request = EWoTVRStartupRequest::Flat;
	FString Reason;
};

/**
 * The optional VR play plugin's runtime module. It owns the XR session every
 * local player's rig uses, decides whether the plugin participates in this
 * launch, routes the mode-switch and recentre shortcuts, and resolves the
 * startup mode. Each local player's rig, aim, view and switching live in
 * UWoTVRPlayerSubsystem.
 */
class WOTVR_API FWoTVRModule final : public IModuleInterface
{
public:
	static FWoTVRModule& Get();
	static bool IsAvailable();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** The session: stock OpenXR, or a spec's synthetic session. */
	IWoTVRSession& GetSession() const;

	/** The plugin participates: the XR gate opened for this launch, and the
	 * master enable is on (live) or -vr was given. While inactive the plugin
	 * attaches nothing to the game and registers no shortcut. */
	bool IsActive() const;
	/** Broadcast when IsActive() changes (the master enable toggled live). */
	FSimpleMulticastDelegate OnActivationChanged;

	/** -vr / -novr for this launch. */
	bool HasVRFlag() const;
	bool HasNoVRFlag() const;

	/** The frame-rate cap (t.MaxFPS) as configured when the module started,
	 * before the engine could start stereo for -vr and uncap it. */
	float GetStartupMaxFPS() const { return StartupMaxFPS; }

	/** Precedence: an inactive plugin starts flat; -novr wins over everything;
	 * then -vr; then the saved preference (VR explicit, Auto, Flat). */
	static FWoTVRStartupDecision ResolveStartup(const FWoTVRStartupInputs& Inputs);
	FWoTVRStartupDecision ResolveProcessStartup() const;

	/** The first active local player's subsystem in a game or play world, the
	 * one the keyboard and console commands drive. */
	static UWoTVRPlayerSubsystem* FindPrimarySubsystem(const UWorld* PreferredWorld = nullptr);

	/** The shortcut keys, routed from Slate ahead of any widget so they work in
	 * menus, the front end and while dead. True when the key was handled: the
	 * switch key in VR or while a headset can start VR, the recentre key in VR;
	 * any other time the key stays the game's. */
	bool HandleShortcutKey(const FKeyEvent& Event);

	/** Active subsystems hold the shortcut processor registered. */
	void AcquireShortcuts();
	void ReleaseShortcuts();
	bool AreShortcutsRegistered() const;

#if WITH_AUTOMATION_TESTS
	/** Specs only: a synthetic session in place of OpenXR, and the activation
	 * and launch flags in place of this process's. Nothing is persisted. */
	struct FTestOverrides
	{
		TSharedPtr<IWoTVRSession> Session;
		TOptional<bool> bActive;
		TOptional<bool> bFlagVR;
		TOptional<bool> bFlagNoVR;
	};
	void SetTestOverrides(const FTestOverrides& Overrides);
	void ClearTestOverrides();
#endif

private:
	void HandleSettingsChanged();

	TSharedPtr<IWoTVRSession> OpenXRSession;
	TSharedPtr<FWoTVRInputProcessor> InputProcessor;
	int32 ShortcutUsers = 0;
	bool bLastActive = false;
	float StartupMaxFPS = 0.0f;
	FDelegateHandle SettingsChangedHandle;
#if WITH_AUTOMATION_TESTS
	FTestOverrides TestOverrides;
#endif
};
