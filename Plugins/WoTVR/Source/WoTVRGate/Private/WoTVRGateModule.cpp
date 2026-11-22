#include "WoTVRGate.h"

#include "Features/IModularFeatures.h"
#include "IHeadMountedDisplayModule.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"
#include "Misc/Parse.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTVRGate, Log, All);

namespace
{
FWoTVRGateInputs GWoTVRGateInputs;
FWoTVRGateDecision GWoTVRGateDecision;
int32 GWoTVRGateWithheld = 0;

void WoTVRGateWithhold(const FName Type, IModularFeature* Feature)
{
	if (!Feature)
	{
		return;
	}
	const FString Name = static_cast<IHeadMountedDisplayModule*>(Feature)->GetModuleKeyName();
	// The engine's own HeadMountedDisplay module registers a placeholder under
	// this name: it has no runtime and creates no tracking system, and the
	// engine unregisters it itself when no HMD is chosen. It stays, so a
	// process without XR sees exactly what it sees without this plugin.
	if (Name == TEXT("Default"))
	{
		return;
	}
	// The registry's lock is recursive: unregistering from inside its
	// registration broadcast is safe, and the engine never sees the module.
	IModularFeatures::Get().UnregisterModularFeature(Type, Feature);
	++GWoTVRGateWithheld;
	UE_LOG(LogWoTVRGate, Log,
		TEXT("XR gate closed: withheld HMD module '%s' from the engine (no XR instance, runtime or system)"),
		*Name);
}
}

const TCHAR* FWoTVRGate::SettingsSection()
{
	return TEXT("/Script/WoTVR.WoTVRSettings");
}

const TCHAR* FWoTVRGate::MasterEnableKey()
{
	return TEXT("bEnableVR");
}

FWoTVRGateDecision FWoTVRGate::Decide(const FWoTVRGateInputs& Inputs)
{
	FWoTVRGateDecision Result;
	if (Inputs.bCommandlet)
	{
		Result.Reason = TEXT("a commandlet never creates an XR instance");
	}
	else if (Inputs.bDedicatedServer)
	{
		Result.Reason = TEXT("a dedicated server never creates an XR instance");
	}
	else if (Inputs.bFlagNoVR)
	{
		Result.Reason = Inputs.bFlagVR
			? TEXT("-novr wins over -vr: no VR this launch")
			: TEXT("-novr: no VR this launch");
	}
	else if (Inputs.bFlagVR)
	{
		Result.bXRAllowed = true;
		Result.Reason = TEXT("-vr");
	}
	else if (Inputs.bMasterEnable && !Inputs.bEditor)
	{
		Result.bXRAllowed = true;
		Result.Reason = TEXT("the VR master enable is on");
	}
	else if (Inputs.bMasterEnable)
	{
		Result.Reason = TEXT("an editor process honours only -vr, not the saved master enable");
	}
	else
	{
		Result.Reason = TEXT("the VR master enable is off and -vr was not given");
	}
	return Result;
}

FWoTVRGateInputs FWoTVRGate::ReadProcessInputs()
{
	FWoTVRGateInputs Inputs;
	if (GConfig && !GGameUserSettingsIni.IsEmpty())
	{
		GConfig->GetBool(SettingsSection(), MasterEnableKey(), Inputs.bMasterEnable,
			GGameUserSettingsIni);
	}
	const TCHAR* CommandLine = FCommandLine::Get();
	Inputs.bFlagVR = FParse::Param(CommandLine, TEXT("vr"));
	Inputs.bFlagNoVR = FParse::Param(CommandLine, TEXT("novr"));
	Inputs.bCommandlet = IsRunningCommandlet();
	Inputs.bDedicatedServer = IsRunningDedicatedServer();
	Inputs.bEditor = GIsEditor;
	return Inputs;
}

const FWoTVRGateInputs& FWoTVRGate::GetProcessInputs()
{
	return GWoTVRGateInputs;
}

const FWoTVRGateDecision& FWoTVRGate::GetProcessDecision()
{
	return GWoTVRGateDecision;
}

int32 FWoTVRGate::GetWithheldRegistrationCount()
{
	return GWoTVRGateWithheld;
}

class FWoTVRGateModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		GWoTVRGateInputs = FWoTVRGate::ReadProcessInputs();
		GWoTVRGateDecision = FWoTVRGate::Decide(GWoTVRGateInputs);
		if (GWoTVRGateDecision.bXRAllowed)
		{
			UE_LOG(LogWoTVRGate, Log, TEXT("XR gate open (%s): XR initialises at startup"),
				*GWoTVRGateDecision.Reason);
			return;
		}
		UE_LOG(LogWoTVRGate, Log, TEXT("XR gate closed (%s)"), *GWoTVRGateDecision.Reason);
		IModularFeatures& Features = IModularFeatures::Get();
		const FName Type = IHeadMountedDisplayModule::GetModularFeatureName();
		// A module of the same loading phase may have registered first.
		TArray<IModularFeature*> Registered;
		Features.LockModularFeatureList();
		const int32 Count = Features.GetModularFeatureImplementationCount(Type);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Registered.Add(Features.GetModularFeatureImplementation(Type, Index));
		}
		Features.UnlockModularFeatureList();
		for (IModularFeature* Feature : Registered)
		{
			WoTVRGateWithhold(Type, Feature);
		}
		RegisteredHandle = Features.OnModularFeatureRegistered().AddLambda(
			[Type](const FName& RegisteredType, IModularFeature* Feature)
			{
				if (RegisteredType == Type)
				{
					WoTVRGateWithhold(RegisteredType, Feature);
				}
			});
	}

	virtual void ShutdownModule() override
	{
		if (RegisteredHandle.IsValid())
		{
			IModularFeatures::Get().OnModularFeatureRegistered().Remove(RegisteredHandle);
			RegisteredHandle.Reset();
		}
	}

private:
	FDelegateHandle RegisteredHandle;
};

IMPLEMENT_MODULE(FWoTVRGateModule, WoTVRGate)
