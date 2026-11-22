#include "WoTVRModule.h"

#include "WoTVRGate.h"
#include "WoTVRInputProcessor.h"
#include "WoTVRLog.h"
#include "WoTVROpenXRSession.h"
#include "WoTVRPlayerSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Input/Events.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogWoTVR);

void FWoTVRInputProcessor::Tick(const float, FSlateApplication&, TSharedRef<ICursor>)
{
}

bool FWoTVRInputProcessor::HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& InKeyEvent)
{
	return FWoTVRModule::IsAvailable() && FWoTVRModule::Get().HandleShortcutKey(InKeyEvent);
}

FWoTVRModule& FWoTVRModule::Get()
{
	return FModuleManager::LoadModuleChecked<FWoTVRModule>(TEXT("WoTVR"));
}

bool FWoTVRModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("WoTVR"));
}

void FWoTVRModule::StartupModule()
{
	// Before the engine can start stereo for -vr (UEngine::InitializeHMDDevice
	// runs after the Default loading phase): the flat frame-rate cap as
	// configured, which a stereo start the engine made itself has replaced.
	if (const IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
	{
		StartupMaxFPS = MaxFPS->GetFloat();
	}
	OpenXRSession = MakeShared<FWoTVROpenXRSession>();
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	SettingsChangedHandle = Settings.OnChanged.AddRaw(this, &FWoTVRModule::HandleSettingsChanged);
	bLastActive = IsActive();

	const FWoTVRGateDecision& Gate = FWoTVRGate::GetProcessDecision();
	if (Gate.bXRAllowed)
	{
		// A runtime that ends the session (the runtime quit, or its own UI asked
		// to leave VR) would otherwise make OpenXR quit the game. The rig holds
		// the player still instead and starts a new session when the runtime
		// offers one; the mode switch continues flat.
		if (IConsoleVariable* ExitOnSessionExit = IConsoleManager::Get().FindConsoleVariable(
			TEXT("xr.OpenXRExitAppOnRuntimeDrivenSessionExit")))
		{
			ExitOnSessionExit->Set(0, ECVF_SetByCode);
		}
	}
	UE_LOG(LogWoTVR, Log, TEXT("WoTVR %s: XR gate %s (%s); master enable %s; -vr %s, -novr %s"),
		bLastActive ? TEXT("active") : TEXT("inactive"),
		Gate.bXRAllowed ? TEXT("open") : TEXT("closed"), *Gate.Reason,
		Settings.bEnableVR ? TEXT("on") : TEXT("off"),
		HasVRFlag() ? TEXT("given") : TEXT("absent"),
		HasNoVRFlag() ? TEXT("given") : TEXT("absent"));
}

void FWoTVRModule::ShutdownModule()
{
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	ShortcutUsers = 0;
	if (SettingsChangedHandle.IsValid() && UObjectInitialized())
	{
		UWoTVRSettings::Get().OnChanged.Remove(SettingsChangedHandle);
	}
	SettingsChangedHandle.Reset();
	OpenXRSession.Reset();
#if WITH_AUTOMATION_TESTS
	TestOverrides = FTestOverrides();
#endif
}

IWoTVRSession& FWoTVRModule::GetSession() const
{
#if WITH_AUTOMATION_TESTS
	if (TestOverrides.Session.IsValid())
	{
		return *TestOverrides.Session;
	}
#endif
	return *OpenXRSession;
}

bool FWoTVRModule::HasVRFlag() const
{
#if WITH_AUTOMATION_TESTS
	if (TestOverrides.bFlagVR.IsSet())
	{
		return *TestOverrides.bFlagVR;
	}
#endif
	return FWoTVRGate::GetProcessInputs().bFlagVR;
}

bool FWoTVRModule::HasNoVRFlag() const
{
#if WITH_AUTOMATION_TESTS
	if (TestOverrides.bFlagNoVR.IsSet())
	{
		return *TestOverrides.bFlagNoVR;
	}
#endif
	return FWoTVRGate::GetProcessInputs().bFlagNoVR;
}

bool FWoTVRModule::IsActive() const
{
#if WITH_AUTOMATION_TESTS
	if (TestOverrides.bActive.IsSet())
	{
		return *TestOverrides.bActive;
	}
#endif
	// XR can only start before the renderer does: a launch whose gate closed
	// stays inactive whatever the setting becomes.
	if (!FWoTVRGate::GetProcessDecision().bXRAllowed)
	{
		return false;
	}
	return FWoTVRGate::GetProcessInputs().bFlagVR || UWoTVRSettings::Get().bEnableVR;
}

FWoTVRStartupDecision FWoTVRModule::ResolveStartup(const FWoTVRStartupInputs& Inputs)
{
	FWoTVRStartupDecision Result;
	if (!Inputs.bActive)
	{
		Result.Reason = TEXT("VR is not active this launch");
	}
	else if (Inputs.bFlagNoVR)
	{
		Result.Reason = TEXT("-novr");
	}
	else if (Inputs.bFlagVR)
	{
		Result.Request = EWoTVRStartupRequest::ExplicitVR;
		Result.Reason = TEXT("-vr");
	}
	else if (Inputs.Preference == EWoTVRStartupMode::VR)
	{
		Result.Request = EWoTVRStartupRequest::ExplicitVR;
		Result.Reason = TEXT("the VR startup preference");
	}
	else if (Inputs.Preference == EWoTVRStartupMode::Auto)
	{
		Result.Request = EWoTVRStartupRequest::AutoVR;
		Result.Reason = TEXT("the Auto startup preference");
	}
	else
	{
		Result.Reason = TEXT("the Flat startup preference");
	}
	return Result;
}

FWoTVRStartupDecision FWoTVRModule::ResolveProcessStartup() const
{
	FWoTVRStartupInputs Inputs;
	Inputs.bActive = IsActive();
	Inputs.Preference = UWoTVRSettings::Get().StartupMode;
	Inputs.bFlagVR = HasVRFlag();
	Inputs.bFlagNoVR = HasNoVRFlag();
	return ResolveStartup(Inputs);
}

UWoTVRPlayerSubsystem* FWoTVRModule::FindPrimarySubsystem(const UWorld* PreferredWorld)
{
	auto FromInstance = [](const UGameInstance* GameInstance) -> UWoTVRPlayerSubsystem*
	{
		if (!GameInstance)
		{
			return nullptr;
		}
		for (ULocalPlayer* LocalPlayer : GameInstance->GetLocalPlayers())
		{
			UWoTVRPlayerSubsystem* Subsystem = LocalPlayer
				? LocalPlayer->GetSubsystem<UWoTVRPlayerSubsystem>() : nullptr;
			if (Subsystem && Subsystem->IsActive())
			{
				return Subsystem;
			}
		}
		return nullptr;
	};
	if (PreferredWorld)
	{
		if (UWoTVRPlayerSubsystem* Subsystem = FromInstance(PreferredWorld->GetGameInstance()))
		{
			return Subsystem;
		}
	}
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
		{
			continue;
		}
		if (UWoTVRPlayerSubsystem* Subsystem = FromInstance(Context.OwningGameInstance))
		{
			return Subsystem;
		}
	}
	return nullptr;
}

bool FWoTVRModule::HandleShortcutKey(const FKeyEvent& Event)
{
	if (Event.IsRepeat() || Event.IsAltDown() || Event.IsControlDown() || Event.IsCommandDown())
	{
		return false;
	}
	const UWoTVRSettings& Settings = UWoTVRSettings::Get();
	const FKey Key = Event.GetKey();
	const bool bSwitch = Settings.ModeSwitchKey.IsValid() && Key == Settings.ModeSwitchKey;
	const bool bRecentre = Settings.RecentreKey.IsValid() && Key == Settings.RecentreKey;
	if (!bSwitch && !bRecentre)
	{
		return false;
	}
	UWoTVRPlayerSubsystem* Subsystem = FindPrimarySubsystem();
	if (!Subsystem)
	{
		return false;
	}
	// A shortcut is taken from the game only while it can act: the switch in VR
	// or while a headset can start VR, recentre in VR. Otherwise the key is the
	// game's as if the plugin were absent.
	if (bSwitch)
	{
		if (!Subsystem->CanSwitchMode())
		{
			return false;
		}
		Subsystem->ToggleMode(TEXT("mode switch key"));
		return true;
	}
	if (!Subsystem->IsVR())
	{
		return false;
	}
	Subsystem->Recentre(TEXT("recentre key"));
	return true;
}

void FWoTVRModule::AcquireShortcuts()
{
	if (ShortcutUsers++ > 0)
	{
		return;
	}
	if (!InputProcessor.IsValid())
	{
		InputProcessor = MakeShared<FWoTVRInputProcessor>();
	}
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	}
}

void FWoTVRModule::ReleaseShortcuts()
{
	if (ShortcutUsers == 0 || --ShortcutUsers > 0)
	{
		return;
	}
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
}

bool FWoTVRModule::AreShortcutsRegistered() const
{
	return ShortcutUsers > 0 && InputProcessor.IsValid() && FSlateApplication::IsInitialized()
		&& FSlateApplication::Get().FindInputPreProcessor(InputProcessor, EInputPreProcessorType::Game) != INDEX_NONE;
}

void FWoTVRModule::HandleSettingsChanged()
{
	const bool bActive = IsActive();
	if (bActive != bLastActive)
	{
		bLastActive = bActive;
		OnActivationChanged.Broadcast();
	}
	else if (UWoTVRSettings::Get().bEnableVR && !FWoTVRGate::GetProcessDecision().bXRAllowed)
	{
		UE_LOG(LogWoTVR, Display,
			TEXT("VR master enable on: XR starts at the next launch (this launch has none: %s)"),
			*FWoTVRGate::GetProcessDecision().Reason);
	}
}

#if WITH_AUTOMATION_TESTS
void FWoTVRModule::SetTestOverrides(const FTestOverrides& Overrides)
{
	TestOverrides = Overrides;
	HandleSettingsChanged();
}

void FWoTVRModule::ClearTestOverrides()
{
	TestOverrides = FTestOverrides();
	HandleSettingsChanged();
}
#endif

IMPLEMENT_MODULE(FWoTVRModule, WoTVR)
