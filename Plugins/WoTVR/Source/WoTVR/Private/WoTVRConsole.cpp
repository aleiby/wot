#include "WoTVRLog.h"
#include "WoTVRModule.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRRig.h"
#include "WoTVRSettings.h"

#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"

// Console commands for the first headset sessions: the switch, recentre and
// diagnostics each have a key too; the preferences have no menu rows yet (the
// headset menus are a later wave).
namespace
{
UWoTVRPlayerSubsystem* WoTVRConsoleTarget(UWorld* World)
{
	UWoTVRPlayerSubsystem* Subsystem = FWoTVRModule::IsAvailable()
		? FWoTVRModule::FindPrimarySubsystem(World) : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogWoTVR, Display, TEXT("WoTVR: no active local player (VR is not active this launch, or no game is running)"));
	}
	return Subsystem;
}

void WoTVRConsoleMode(const TArray<FString>& Args, UWorld* World)
{
	if (UWoTVRPlayerSubsystem* Subsystem = WoTVRConsoleTarget(World))
	{
		const FString Wanted = Args.Num() > 0 ? Args[0] : FString(TEXT("Toggle"));
		if (Wanted.Equals(TEXT("VR"), ESearchCase::IgnoreCase))
		{
			Subsystem->RequestMode(EWoTVRMode::VR, TEXT("WoT.VR.Mode"));
		}
		else if (Wanted.Equals(TEXT("Flat"), ESearchCase::IgnoreCase))
		{
			Subsystem->RequestMode(EWoTVRMode::Flat, TEXT("WoT.VR.Mode"));
		}
		else
		{
			Subsystem->ToggleMode(TEXT("WoT.VR.Mode"));
		}
	}
}

void WoTVRConsoleRecentre(const TArray<FString>&, UWorld* World)
{
	if (UWoTVRPlayerSubsystem* Subsystem = WoTVRConsoleTarget(World))
	{
		Subsystem->Recentre(TEXT("WoT.VR.Recentre"));
	}
}

void WoTVRConsoleDiagnostics(const TArray<FString>&, UWorld* World)
{
	if (UWoTVRPlayerSubsystem* Subsystem = WoTVRConsoleTarget(World))
	{
		Subsystem->LogDiagnostics(TEXT("requested"));
	}
}

void WoTVRConsoleStatus(const TArray<FString>&, UWorld* World)
{
	if (UWoTVRPlayerSubsystem* Subsystem = WoTVRConsoleTarget(World))
	{
		UE_LOG(LogWoTVR, Display, TEXT("%s"), *Subsystem->DescribeStatus());
	}
}

void WoTVRConsoleEnable(const TArray<FString>& Args, UWorld*)
{
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	if (Args.Num() > 0)
	{
		Settings.SetEnableVR(Args[0].ToBool());
	}
	UE_LOG(LogWoTVR, Display, TEXT("VR master enable %s (XR itself starts only at launch)"),
		Settings.bEnableVR ? TEXT("on") : TEXT("off"));
}

void WoTVRConsoleStartupMode(const TArray<FString>& Args, UWorld*)
{
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	if (Args.Num() > 0)
	{
		if (Args[0].Equals(TEXT("VR"), ESearchCase::IgnoreCase))
		{
			Settings.SetStartupMode(EWoTVRStartupMode::VR);
		}
		else if (Args[0].Equals(TEXT("Flat"), ESearchCase::IgnoreCase))
		{
			Settings.SetStartupMode(EWoTVRStartupMode::Flat);
		}
		else if (Args[0].Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
		{
			Settings.SetStartupMode(EWoTVRStartupMode::Auto);
		}
	}
	UE_LOG(LogWoTVR, Display, TEXT("VR startup mode %s"), *UEnum::GetValueAsString(Settings.StartupMode));
}

void WoTVRConsolePosture(const TArray<FString>& Args, UWorld*)
{
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	if (Args.Num() > 0)
	{
		Settings.SetPosture(Args[0].Equals(TEXT("Seated"), ESearchCase::IgnoreCase)
			? EWoTVRPosture::Seated : EWoTVRPosture::Standing);
	}
	UE_LOG(LogWoTVR, Display, TEXT("VR posture %s; confirm the neutral pose with Recentre"),
		*UEnum::GetValueAsString(Settings.Posture));
}

void WoTVRConsoleTurnMode(const TArray<FString>& Args, UWorld*)
{
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	if (Args.Num() > 0)
	{
		Settings.SetTurnMode(Args[0].Equals(TEXT("Smooth"), ESearchCase::IgnoreCase)
			? EWoTVRTurnMode::Smooth : EWoTVRTurnMode::Snap);
	}
	UE_LOG(LogWoTVR, Display, TEXT("VR turn mode %s"), *UEnum::GetValueAsString(Settings.TurnMode));
}

bool WoTVRConsoleKeyAction(const FString& Name, EWoTVRKeyAction& OutAction)
{
	struct FNamed { const TCHAR* Name; EWoTVRKeyAction Action; };
	static const FNamed Names[] = {
		{TEXT("ModeSwitch"), EWoTVRKeyAction::ModeSwitch}, {TEXT("Switch"), EWoTVRKeyAction::ModeSwitch},
		{TEXT("Recentre"), EWoTVRKeyAction::Recentre}, {TEXT("Recenter"), EWoTVRKeyAction::Recentre},
		{TEXT("TurnLeft"), EWoTVRKeyAction::TurnLeft}, {TEXT("TurnRight"), EWoTVRKeyAction::TurnRight}};
	for (const FNamed& Named : Names)
	{
		if (Name.Equals(Named.Name, ESearchCase::IgnoreCase))
		{
			OutAction = Named.Action;
			return true;
		}
	}
	return false;
}

void WoTVRConsoleKey(const TArray<FString>& Args, UWorld*)
{
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	if (Args.Num() >= 2)
	{
		EWoTVRKeyAction Action = EWoTVRKeyAction::ModeSwitch;
		const bool bUnbind = Args[1].Equals(TEXT("None"), ESearchCase::IgnoreCase);
		const FKey Key = bUnbind ? EKeys::Invalid : FKey(*Args[1]);
		if (!WoTVRConsoleKeyAction(Args[0], Action))
		{
			UE_LOG(LogWoTVR, Display, TEXT("WoT.VR.Key: unknown action '%s' (ModeSwitch, Recentre, TurnLeft or TurnRight)"), *Args[0]);
		}
		else if (!bUnbind && !Key.IsValid())
		{
			UE_LOG(LogWoTVR, Display, TEXT("WoT.VR.Key: unknown key '%s'"), *Args[1]);
		}
		else
		{
			Settings.SetKey(Action, Key);
		}
	}
	auto Name = [](const FKey& Key) { return Key.IsValid() ? Key.ToString() : FString(TEXT("(unbound)")); };
	UE_LOG(LogWoTVR, Display, TEXT("VR keys: switch %s, recentre %s, turn left %s, turn right %s"),
		*Name(Settings.ModeSwitchKey), *Name(Settings.RecentreKey), *Name(Settings.TurnLeftKey),
		*Name(Settings.TurnRightKey));
}

void WoTVRConsoleEyeHeight(const TArray<FString>& Args, UWorld* World)
{
	UWoTVRSettings& Settings = UWoTVRSettings::Get();
	if (Args.Num() > 0)
	{
		Settings.SetStandingEyeHeightMeters(FCString::Atof(*Args[0]));
	}
	FString Avatar;
	if (const UWoTVRPlayerSubsystem* Subsystem = FWoTVRModule::IsAvailable()
		? FWoTVRModule::FindPrimarySubsystem(World) : nullptr)
	{
		const ULocalPlayer* LocalPlayer = Subsystem->GetLocalPlayer();
		const APlayerController* PC = LocalPlayer ? LocalPlayer->PlayerController.Get() : nullptr;
		if (const APawn* Pawn = PC ? PC->GetPawn() : nullptr)
		{
			float Radius = 0.0f;
			float HalfHeight = 0.0f;
			Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
			const double EyeMeters = (Pawn->GetPawnViewLocation().Z
				- (Pawn->GetActorLocation().Z - HalfHeight)) / WoTVR::UnitsPerMeter;
			Avatar = FString::Printf(TEXT("; the avatar's eye is %.2f m above its feet"), EyeMeters);
			if (Settings.StandingEyeHeightMeters > 0.0f)
			{
				Avatar += FString::Printf(TEXT(", %+.2f m from yours"),
					EyeMeters - Settings.StandingEyeHeightMeters);
			}
		}
	}
	UE_LOG(LogWoTVR, Display, TEXT("VR standing eye height %s%s"),
		Settings.StandingEyeHeightMeters > 0.0f
			? *FString::Printf(TEXT("%.2f m"), Settings.StandingEyeHeightMeters)
			: TEXT("not measured"),
		*Avatar);
}

FAutoConsoleCommandWithWorldAndArgs GWoTVRModeCommand(TEXT("WoT.VR.Mode"),
	TEXT("Switch in place between VR and flat: WoT.VR.Mode VR|Flat|Toggle (no argument toggles)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleMode));
FAutoConsoleCommandWithWorldAndArgs GWoTVRRecentreCommand(TEXT("WoT.VR.Recentre"),
	TEXT("Confirm the current head pose as neutral, keeping the pawn's position and the aim."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleRecentre));
FAutoConsoleCommandWithWorldAndArgs GWoTVRDiagnosticsCommand(TEXT("WoT.VR.Diagnostics"),
	TEXT("Log the XR runtime and version, the system, the enabled OpenXR extensions and each hand's interaction profile."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleDiagnostics));
FAutoConsoleCommandWithWorldAndArgs GWoTVRStatusCommand(TEXT("WoT.VR.Status"),
	TEXT("Log the VR mode, session, gate, preferences and keys."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleStatus));
FAutoConsoleCommandWithWorldAndArgs GWoTVREnableCommand(TEXT("WoT.VR.Enable"),
	TEXT("VR master enable: WoT.VR.Enable 1|0 (saved; XR starts at the next launch)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleEnable));
FAutoConsoleCommandWithWorldAndArgs GWoTVRStartupModeCommand(TEXT("WoT.VR.StartupMode"),
	TEXT("VR startup mode: WoT.VR.StartupMode Auto|Flat|VR (saved)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleStartupMode));
FAutoConsoleCommandWithWorldAndArgs GWoTVRPostureCommand(TEXT("WoT.VR.Posture"),
	TEXT("VR calibration posture: WoT.VR.Posture Standing|Seated (saved)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsolePosture));
FAutoConsoleCommandWithWorldAndArgs GWoTVRTurnModeCommand(TEXT("WoT.VR.TurnMode"),
	TEXT("VR turning: WoT.VR.TurnMode Snap|Smooth (saved)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleTurnMode));
FAutoConsoleCommandWithWorldAndArgs GWoTVRKeyCommand(TEXT("WoT.VR.Key"),
	TEXT("Rebind a VR key: WoT.VR.Key ModeSwitch|Recentre|TurnLeft|TurnRight <Key>|None (saved; acts at once). No argument lists the keys."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleKey));
FAutoConsoleCommandWithWorldAndArgs GWoTVREyeHeightCommand(TEXT("WoT.VR.EyeHeight"),
	TEXT("Optional calibration: your standing eye-to-floor height in metres (0 clears), reported against the avatar's."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&WoTVRConsoleEyeHeight));
}
