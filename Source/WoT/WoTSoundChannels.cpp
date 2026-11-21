#include "WoTSoundChannels.h"

#include "WoTAmbientSound.h"
#include "WoTSound.h"
#include "WoTZoneReverbSubsystem.h"

#include "AudioDefines.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"

namespace
{
	// When the engine voice of a non-looping sound ends, in world seconds. UE
	// clamps a voice's pitch to [MIN_PITCH, MAX_PITCH], which sets its length.
	double VoiceEndTime(const USoundBase& Sound, double Now, float Pitch)
	{
		const float Duration = Sound.GetDuration();
		const float EnginePitch = FMath::Clamp(Pitch, MIN_PITCH, MAX_PITCH);
		return Now + static_cast<double>(Duration / EnginePitch);
	}

	// UGameplayStatics::SpawnSoundAtLocation for a world context, without its
	// Play (GameplayStatics.cpp): the caller adds the room reverb send first,
	// so the voice reaches the reverb from its first sample. Like the engine
	// call it creates no voice for a short sound out of every listener's range.
	UAudioComponent* CreateVoiceAt(UWorld* World, USoundBase* Sound,
		const FVector& Location, float Volume, float Pitch, USoundAttenuation* Attenuation)
	{
		if (!Sound || !GEngine || !GEngine->UseSound() || !World
			|| !World->bAllowAudioPlayback || World->IsNetMode(NM_DedicatedServer))
		{
			return nullptr;
		}
		FAudioDevice::FCreateComponentParams Params(World, nullptr);
		Params.SetLocation(Location);
		Params.AttenuationSettings = Attenuation;
		UAudioComponent* Voice = FAudioDevice::CreateComponent(Sound, Params);
		if (Voice)
		{
			Voice->SetWorldLocationAndRotation(Location, FRotator::ZeroRotator);
			Voice->SetVolumeMultiplier(Volume);
			Voice->SetPitchMultiplier(Pitch);
			Voice->bAllowSpatialization = Params.ShouldUseAttenuation();
			Voice->bIsUISound = !World->IsGameWorld();
			Voice->bAutoDestroy = true;
			Voice->SubtitlePriority = Sound->GetSubtitlePriority();
			Voice->bStopWhenOwnerDestroyed = false;
		}
		return Voice;
	}
}

UWoTSoundChannels* UWoTSoundChannels::Get(const UWorld* World)
{
	return World ? World->GetSubsystem<UWoTSoundChannels>() : nullptr;
}

bool UWoTSoundChannels::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UWoTSoundChannels::Deinitialize()
{
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		StopChannel(Index);
	}
	Ambients.Reset();
	Super::Deinitialize();
}

TStatId UWoTSoundChannels::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWoTSoundChannels, STATGROUP_Tickables);
}

bool UWoTSoundChannels::IsTickable() const
{
	if (Ambients.Num() > 0)
	{
		return true;
	}
	for (const FChannel& Channel : Channels)
	{
		if (Channel.bBusy)
		{
			return true;
		}
	}
	return false;
}

UWoTSoundChannels::EAdmission UWoTSoundChannels::Admit(const WoTSound::FPlayParams& Params,
	float& OutVolume, float& OutRadius, float& OutPitch, float& OutPriority,
	int32& OutChannel) const
{
	OutChannel = INDEX_NONE;
	const UWorld* World = GetWorld();
	FVector View;
	if (!World || World->GetNetMode() == NM_DedicatedServer
		|| !WoTSound::ListenerLocation(*World, View))
	{
		return EAdmission::NoListener;
	}
	// PlaySound's hearing test, on the raw radius (0 is the default).
	const float Distance = FVector::Distance(View, Params.Location);
	const float RawRadius = FMath::Max(Params.Radius, 0.0f);
	if (!WoTSound::PassesHearingGate(Distance,
		RawRadius > 0.0f ? RawRadius : WoTSound::DefaultPlaySoundRadius))
	{
		return EAdmission::OutsideHearingRadius;
	}
	// The listener's ClientHearSound may scale the raw values, and only then
	// does playback give an unset radius the default.
	OutVolume = Params.Volume;
	OutRadius = RawRadius;
	OutPitch = Params.Pitch;
	WoTSound::ApplyUnderwater(*World, Params.Source, Params.Location, Params.Slot,
		Params.Sound, OutVolume, OutRadius, OutPitch);
	if (OutRadius <= 0.0f)
	{
		OutRadius = WoTSound::DefaultPlaySoundRadius;
	}
	OutPriority = WoTSound::EffectsPriority(Distance, OutRadius, OutVolume);
	const FObjectKey Key = Params.Source ? FObjectKey(Params.Source) : FObjectKey();
	OutChannel = ChooseChannel(Key, TakesClientOwnSlot(Params.Caller), Params.Slot,
		Params.bNoOverride, OutPriority);
	return OutChannel != INDEX_NONE ? EAdmission::Admitted : EAdmission::Rejected;
}

bool UWoTSoundChannels::TakesClientOwnSlot(WoTSound::ECaller Caller) const
{
	// The original named a slot by the actor's object index on the machine
	// that called PlaySound. A network client's own plays therefore never
	// matched the ones the server sent it; a listen host or standalone game
	// is one machine and keeps one set of slots.
	const UWorld* World = GetWorld();
	return Caller == WoTSound::ECaller::Local && World
		&& World->GetNetMode() == NM_Client;
}

int32 UWoTSoundChannels::ChooseChannel(const FObjectKey& Key, bool bClientOwnSlot,
	WoTSound::ESlot Slot, bool bNoOverride, float Priority) const
{
	// The sound already in this actor's slot is replaced, or the newcomer
	// yields to it under bNoOverride. Otherwise the last of the quietest
	// channels (an idle channel counts 0) at or below the newcomer wins.
	const bool bKeyed = Slot != WoTSound::ESlot::None && Key != FObjectKey();
	int32 Chosen = INDEX_NONE;
	float Best = Priority;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		const FChannel& Channel = Channels[Index];
		if (bKeyed && Channel.bBusy && Channel.Slot == Slot && Channel.SourceKey == Key
			&& Channel.bClientOwnSlot == bClientOwnSlot)
		{
			return bNoOverride ? INDEX_NONE : Index;
		}
		const float ChannelPriority = Channel.bBusy ? Channel.Priority : 0.0f;
		if (ChannelPriority <= Best)
		{
			Chosen = Index;
			Best = ChannelPriority;
		}
	}
	return Chosen;
}

UAudioComponent* UWoTSoundChannels::Play(const WoTSound::FPlayParams& Params)
{
	return Start(Params, /*bInterface*/ false);
}

UAudioComponent* UWoTSoundChannels::PlayInterface(const WoTSound::FPlayParams& Params)
{
	return Start(Params, /*bInterface*/ true);
}

UAudioComponent* UWoTSoundChannels::Start(const WoTSound::FPlayParams& Params, bool bInterface)
{
	float Volume = 1.0f, Radius = 0.0f, Pitch = 1.0f, Priority = 0.0f;
	int32 Index = INDEX_NONE;
	if (Admit(Params, Volume, Radius, Pitch, Priority, Index) != EAdmission::Admitted)
	{
		return nullptr;
	}
	StopChannel(Index);
	if (!Params.Sound)
	{
		return nullptr;   // PlaySound(None, Slot): the slot is now silent
	}
	UWorld* World = GetWorld();
	UAudioComponent* Voice = bInterface
		? UGameplayStatics::CreateSound2D(World, Params.Sound,
			WoTSound::NormalizedUISoundVoiceGain(Volume), Pitch)
		: CreateVoiceAt(World, Params.Sound, Params.Location,
			WoTSound::VoiceVolume(Volume), Pitch,
			WoTSound::LinearAttenuation(Radius, Volume));
	if (Voice)
	{
		// Every effects voice, interface sounds included, feeds the room reverb.
		UWoTZoneReverbSubsystem::AddSend(World, *Voice);
		Voice->Play();
	}
	if (!Voice || !Voice->IsPlaying())
	{
		// The engine started no voice (no audio device, audio off in this
		// world, or a short sound it culls as out of every listener's range):
		// the channel stays free rather than holding a sound nobody hears.
		if (IsValid(Voice))
		{
			Voice->DestroyComponent();
		}
		return nullptr;
	}
	FChannel& Channel = Channels[Index];
	Channel.bBusy = true;
	Channel.SourceKey = Params.Source ? FObjectKey(Params.Source) : FObjectKey();
	Channel.Slot = Params.Slot;
	Channel.bClientOwnSlot = TakesClientOwnSlot(Params.Caller);
	Channel.Source = Params.Source;
	Channel.bHadSource = Params.Source != nullptr;
	Channel.bInterface = bInterface;
	Channel.SoundName = Params.Sound->GetFName();
	Channel.Location = Params.Location;
	Channel.Volume = Volume;
	Channel.Radius = Radius;
	Channel.Pitch = Pitch;
	Channel.Priority = Priority;
	Channel.bLooping = Params.Sound->IsLooping();
	Channel.EndTime = Channel.bLooping ? TNumericLimits<double>::Max()
		: VoiceEndTime(*Params.Sound, World->GetTimeSeconds(), Pitch);
	Channel.Voice = Voice;
	Channel.bVoicePlayed = true;
	return Voice;
}

void UWoTSoundChannels::StopChannel(int32 Index)
{
	FChannel& Channel = Channels[Index];
	if (Channel.bBusy)
	{
		if (UWoTAmbientSoundComponent* Ambient = Channel.Ambient.Get())
		{
			Ambient->StopChannelVoice();
		}
		else if (UAudioComponent* Voice = Channel.Voice.Get())
		{
			Voice->Stop();
		}
	}
	Channel = FChannel();
}

void UWoTSoundChannels::AddAmbient(UWoTAmbientSoundComponent& Ambient)
{
	Ambients.AddUnique(&Ambient);
}

void UWoTSoundChannels::RemoveAmbient(UWoTAmbientSoundComponent& Ambient)
{
	Ambients.Remove(&Ambient);
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		if (Channels[Index].bBusy && Channels[Index].Ambient.Get() == &Ambient)
		{
			StopChannel(Index);
		}
	}
}

bool UWoTSoundChannels::HoldsChannel(const UWoTAmbientSoundComponent& Ambient) const
{
	for (const FChannel& Channel : Channels)
	{
		if (Channel.bBusy && Channel.Ambient.Get() == &Ambient)
		{
			return true;
		}
	}
	return false;
}

void UWoTSoundChannels::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FVector View;
	const bool bHasView = World->GetNetMode() != NM_DedicatedServer
		&& WoTSound::ListenerLocation(*World, View);
	if (bHasView)
	{
		StartAmbience(View);
		UpdateAmbience(View);
	}
	UpdateChannels(View, bHasView);
}

void UWoTSoundChannels::StartAmbience(const FVector& View)
{
	// An ambient loop within its full radius that holds no channel asks for
	// one every frame, at its start volume (the ambient gain x SoundVolume/255;
	// the doubled, lit volume applies from the next update).
	Ambients.RemoveAll([](const TWeakObjectPtr<UWoTAmbientSoundComponent>& Weak)
	{
		return !Weak.IsValid();
	});
	for (const TWeakObjectPtr<UWoTAmbientSoundComponent>& Weak : Ambients)
	{
		UWoTAmbientSoundComponent* Ambient = Weak.Get();
		if (!Ambient || !Ambient->WantsToPlay() || !Ambient->GetSound()
			|| !Ambient->IsRegistered() || HoldsChannel(*Ambient))
		{
			continue;
		}
		const FVector Location = Ambient->GetComponentLocation();
		const float Radius = Ambient->BaseRadiusUU;
		const float DistSquared = FVector::DistSquared(View, Location);
		if (Radius <= 0.0f || DistSquared > FMath::Square(Radius))
		{
			continue;
		}
		const float Volume = Ambient->GetStartVolume();
		const float Priority =
			WoTSound::EffectsPriority(FMath::Sqrt(DistSquared), Radius, Volume);
		// Every machine plays its own ambient loops.
		const bool bClientOwnSlot = TakesClientOwnSlot(WoTSound::ECaller::Local);
		const int32 Index = ChooseChannel(FObjectKey(Ambient), bClientOwnSlot,
			WoTSound::ESlot::Ambient, /*bNoOverride*/ false, Priority);
		if (Index == INDEX_NONE)
		{
			continue;
		}
		// Only a loop whose engine voice starts takes the channel; one that
		// does not asks again next frame and leaves the channel's sound alone.
		Ambient->StartChannelVoice();
		if (!Ambient->IsPlaying())
		{
			continue;
		}
		StopChannel(Index);
		FChannel& Channel = Channels[Index];
		Channel.bBusy = true;
		Channel.SourceKey = FObjectKey(Ambient);
		Channel.Slot = WoTSound::ESlot::Ambient;
		Channel.bClientOwnSlot = bClientOwnSlot;
		Channel.Source = Ambient->GetOwner();
		Channel.bHadSource = true;
		Channel.bAmbientLoop = true;
		Channel.Ambient = Ambient;
		Channel.Voice = Ambient;
		Channel.bVoicePlayed = true;
		Channel.SoundName = Ambient->GetSound()->GetFName();
		Channel.Location = Location;
		Channel.Volume = Volume;
		Channel.Radius = Radius;
		Channel.Pitch = Ambient->GetChannelPitch();
		Channel.Priority = Priority;
		Channel.bLooping = true;
		Channel.EndTime = TNumericLimits<double>::Max();
	}
}

void UWoTSoundChannels::UpdateAmbience(const FVector& View)
{
	// A loop stops when its listener leaves its full radius, its owner stops
	// it or its sound changes; otherwise it takes its current (lit, muffled)
	// volume, radius and pitch.
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		FChannel& Channel = Channels[Index];
		if (!Channel.bBusy || !Channel.bAmbientLoop)
		{
			continue;
		}
		UWoTAmbientSoundComponent* Ambient = Channel.Ambient.Get();
		const USoundBase* Sound = Ambient ? Ambient->GetSound() : nullptr;
		if (!Ambient || !Ambient->WantsToPlay() || !Sound
			|| Sound->GetFName() != Channel.SoundName
			|| FVector::DistSquared(View, Ambient->GetComponentLocation())
				> FMath::Square(Ambient->BaseRadiusUU))
		{
			StopChannel(Index);
			continue;
		}
		Channel.Volume = Ambient->GetChannelVolume();
		Channel.Radius = Ambient->GetChannelRadius();
		Channel.Pitch = Ambient->GetChannelPitch();
	}
}

void UWoTSoundChannels::UpdateChannels(const FVector& View, bool bHasView)
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		FChannel& Channel = Channels[Index];
		if (!Channel.bBusy)
		{
			continue;
		}
		// An ambient loop ends with its component.
		if (Channel.bAmbientLoop && !Channel.Ambient.IsValid())
		{
			Channel = FChannel();
			continue;
		}
		// A sound is finished when its sample ends or its engine voice stops.
		UAudioComponent* Voice = Channel.Voice.Get();
		const bool bVoicePlaying = Voice && Voice->IsPlaying();
		Channel.bVoicePlayed |= bVoicePlaying;
		if ((Channel.bVoicePlayed && !bVoicePlaying) || Now >= Channel.EndTime)
		{
			// Freed without a stop: a voice still sounding past its sample's
			// end drains its last samples rather than being cut.
			Channel = FChannel();
			continue;
		}
		// Follow the playing actor. When it is gone an ordinary sound goes on
		// from its last location; a looping one (which would never end) stops
		// with its actor.
		if (UWoTAmbientSoundComponent* Ambient = Channel.Ambient.Get())
		{
			Channel.Location = Ambient->GetComponentLocation();
		}
		else if (const AActor* Source = Channel.Source.Get())
		{
			Channel.Location = Source->GetActorLocation();
			if (Voice && !Channel.bInterface)
			{
				Voice->SetWorldLocation(Channel.Location);
			}
		}
		else if (Channel.bHadSource && Channel.bLooping)
		{
			StopChannel(Index);
			continue;
		}
		if (!bHasView)
		{
			continue;
		}
		Channel.Priority = WoTSound::EffectsPriority(
			FVector::Distance(View, Channel.Location), Channel.Radius, Channel.Volume);
		// Ambient loops take the source-only Doppler shift.
		if (UWoTAmbientSoundComponent* Ambient = Channel.Ambient.Get())
		{
			const AActor* Owner = Ambient->GetOwner();
			Ambient->SetDopplerFactor(Owner
				? WoTSound::DopplerFactor(Owner->GetVelocity(), Channel.Location, View)
				: 1.0f);
		}
	}
}

#if WITH_AUTOMATION_TESTS
WoTSound::EEffectAdmissionResult UWoTSoundChannels::PrepareForTesting(
	const WoTSound::FPlayParams& Params)
{
	float Volume = 1.0f, Radius = 0.0f, Pitch = 1.0f, Priority = 0.0f;
	int32 Index = INDEX_NONE;
	switch (Admit(Params, Volume, Radius, Pitch, Priority, Index))
	{
	case EAdmission::Admitted:
		return WoTSound::EEffectAdmissionResult::Admitted;
	case EAdmission::NoListener:
		return WoTSound::EEffectAdmissionResult::NoListener;
	case EAdmission::OutsideHearingRadius:
		return WoTSound::EEffectAdmissionResult::OutsideHearingRadius;
	default:
		return WoTSound::EEffectAdmissionResult::PoolPriorityRejected;
	}
}

int32 UWoTSoundChannels::SeedForTesting(
	TConstArrayView<WoTSound::FEffectVoicePrioritySample> Voices)
{
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		StopChannel(Index);
	}
	FVector View = FVector::ZeroVector;
	const bool bHasView = GetWorld() && WoTSound::ListenerLocation(*GetWorld(), View);
	int32 Count = 0;
	for (const WoTSound::FEffectVoicePrioritySample& Sample : Voices)
	{
		if (Count >= WoTSound::EffectsChannelCount)
		{
			break;
		}
		FChannel& Channel = Channels[Count++];
		Channel.bBusy = true;
		Channel.Slot = WoTSound::ESlot::None;
		Channel.Location = Sample.SourceLocation;
		Channel.Volume = Sample.Volume;
		Channel.Radius = Sample.RadiusUU;
		Channel.bLooping = true;
		Channel.EndTime = TNumericLimits<double>::Max();
		Channel.Priority = bHasView ? WoTSound::EffectsPriority(
			FVector::Distance(View, Sample.SourceLocation), Sample.RadiusUU,
			Sample.Volume) : Sample.Volume;
	}
	return Count;
}

TArray<WoTSound::FChannelSnapshot> UWoTSoundChannels::SnapshotForTesting() const
{
	TArray<WoTSound::FChannelSnapshot> Out;
	for (int32 Index = 0; Index < WoTSound::EffectsChannelCount; ++Index)
	{
		const FChannel& Channel = Channels[Index];
		if (!Channel.bBusy)
		{
			continue;
		}
		WoTSound::FChannelSnapshot& Snap = Out.AddDefaulted_GetRef();
		Snap.Index = Index;
		Snap.SoundName = Channel.SoundName;
		Snap.SourceKey = Channel.SourceKey;
		Snap.Slot = Channel.Slot;
		Snap.bClientOwnSlot = Channel.bClientOwnSlot;
		Snap.Location = Channel.Location;
		Snap.Volume = Channel.Volume;
		Snap.Radius = Channel.Radius;
		Snap.Pitch = Channel.Pitch;
		Snap.Priority = Channel.Priority;
		Snap.bAmbient = Channel.Ambient.IsValid();
	}
	return Out;
}
#endif
