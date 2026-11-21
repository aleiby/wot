#include "WoTZoneReverbSubsystem.h"

#include "WoTMapLightTable.h"
#include "WoTMeshLight.h"

#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Sound/SoundSubmix.h"
#include "UObject/Package.h"

UWoTZoneReverbSubmix::UWoTZoneReverbSubmix(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsDynamic = true;
}

UWoTZoneReverbSubsystem* UWoTZoneReverbSubsystem::Get(const UWorld* World)
{
	return World ? World->GetSubsystem<UWoTZoneReverbSubsystem>() : nullptr;
}

bool UWoTZoneReverbSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UWoTZoneReverbSubsystem::IsLocalPresentation() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_DedicatedServer && !IsRunningDedicatedServer();
}

void UWoTZoneReverbSubsystem::AddSend(const UWorld* World, UAudioComponent& Voice)
{
	UWoTZoneReverbSubsystem* Reverb = Get(World);
	if (!Reverb || !Reverb->IsLocalPresentation() || !Reverb->EnsureSubmix()) return;
	Voice.SetSubmixSend(Reverb->Submix, WoTZoneReverb::SendLevel);
}

bool UWoTZoneReverbSubsystem::EnsureSubmix()
{
	if (Submix) return true;
	UWorld* World = GetWorld();
	FAudioDevice* Device = World && World->bAllowAudioPlayback ? World->GetAudioDeviceRaw() : nullptr;
	if (!Device) return false;
	// Both live outside the world: a voice still sounding as the world goes
	// keeps its send's submix alive for a moment, and that must not keep the
	// world.
	Preset = NewObject<UWoTZoneReverbPreset>(GetTransientPackage(), NAME_None, RF_Transient);
	Preset->SetSettings(Current);
	Submix = NewObject<UWoTZoneReverbSubmix>(GetTransientPackage(), NAME_None, RF_Transient);
	// Dynamic submixes are excluded from other devices' InitSoundSubmixes.
	// Connect only to this device's main output, under its master volume.
	Submix->SubmixEffectChain.Add(Preset);
	// Never switched off by the engine as silent: the reverb's tail outlives
	// its input.
	Submix->bAutoDisable = false;
	OwningAudioDevice = World->GetAudioDevice();
	if (!Submix->DynamicConnect(OwningAudioDevice, &Device->GetMainSubmixObject()))
	{
		Submix = nullptr;
		Preset = nullptr;
		OwningAudioDevice.Reset();
		return false;
	}
	// The submix plays only what the reverb writes; none of its input (the
	// sends) passes through.
	Device->SetSubmixDryLevel(Submix, 0.0f);
	return true;
}

void UWoTZoneReverbSubsystem::Deinitialize()
{
	if (Submix && OwningAudioDevice.IsValid())
	{
		Submix->DynamicDisconnect(OwningAudioDevice);
	}
	OwningAudioDevice.Reset();
	Submix = nullptr;
	Preset = nullptr;
	Super::Deinitialize();
}

TStatId UWoTZoneReverbSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWoTZoneReverbSubsystem, STATGROUP_Tickables);
}

bool UWoTZoneReverbSubsystem::IsTickable() const
{
	return IsLocalPresentation();
}

void UWoTZoneReverbSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World || !IsLocalPresentation()) return;
	// The listener: the local player's view target (else the player), at the
	// actor's own location. The zone is the one its centre is in, not the
	// camera's or the eyes'.
	FWoTZoneReverbSettings Wanted;
	FVector Location;
	ListenerZone = INDEX_NONE;
	APlayerController* LocalController = nullptr;
	for (auto It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* Controller = It->Get(); Controller && Controller->IsLocalController())
		{
			LocalController = Controller;
			break;
		}
	}
	if (LocalController)
	{
		const AActor* ViewTarget = LocalController->GetViewTarget();
		Location = ViewTarget ? ViewTarget->GetActorLocation() : LocalController->GetFocalLocation();
		const UWoTMapLightTable* Table = UWoTMeshLight::FindMapLightTable(World);
		int32 Zone = INDEX_NONE;
		int32 Leaf = INDEX_NONE;
		if (Table && UWoTMeshLight::ClassifyRegion(Table, Location, Zone, Leaf))
		{
			ListenerZone = Zone;
			if (WoTZoneReverb::IsRoomReverbEnabled() && Table->ZoneReverbs.IsValidIndex(Zone))
			{
				Wanted = WoTZoneReverb::MakeSettings(Table->ZoneReverbs[Zone]);
			}
		}
	}
	// Only a different reverb reaches the effect, and it restarts it; the same
	// reverb in another zone carries its tail on.
	if (Wanted == Current) return;
	Current = Wanted;
	++Changes;
	if (Current.IsActive()) EnsureSubmix();
	if (Preset) Preset->SetSettings(Current);
}
