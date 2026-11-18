#include "WoTSoundTable.h"
#include "WoTRandom.h"

#include "WoTBattleGameMode.h"
#include "WoTCharacter.h"
#include "WoTSound.h"
#include "AI/WoTNPCController.h"

#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"

#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"

// Activated sound and timer rows are payload-owned. Playback semantics remain
// native and are cited at their call sites below.
namespace
{
	TUniquePtr<FWoTSoundSet>& InstalledSoundRegistry()
	{
		static TUniquePtr<FWoTSoundSet> Installed;
		return Installed;
	}

	const TMap<FName, TArray<FWoTSoundTableEntry>>& ActiveTables()
	{
		const TUniquePtr<FWoTSoundSet>& Installed = InstalledSoundRegistry();
		if (Installed) return Installed->Tables;
		static const TMap<FName, TArray<FWoTSoundTableEntry>> Empty;
		return Empty;
	}

	const TMap<FName, TArray<FWoTSoundSlotTimer>>& ActiveTimerLists()
	{
		const TUniquePtr<FWoTSoundSet>& Installed = InstalledSoundRegistry();
		if (Installed) return Installed->TimerLists;
		static const TMap<FName, TArray<FWoTSoundSlotTimer>> Empty;
		return Empty;
	}

	// SoundIsValidForGameType (SoundTableWOT.uc:77-113) in the MP-only port: All and
	// MP always pass, SP and None never, Battle only under the Battle mode, and
	// Arena rides the shipped duplicate-case bug (unknowns warn + return TRUE).
	bool ValidForGameType(uint8 GameType, const UWorld* World)
	{
		switch (GameType)
		{
			case 0: return true;                    // GT_All
			case 1: return false;                   // GT_SinglePlayer (port is MP)
			case 2: return true;                    // GT_MultiPlayer
			case 3:                                  // GT_Battle
				return World && World->GetAuthGameMode<AWoTBattleGameMode>() != nullptr;
			case 4: return true;                    // GT_Arena — dead case in shipped
			default: return false;                  // GT_None
		}
	}

	// UnrealScript's "~=": equal within 0.0001.
	bool UnsetSoundValue(float Value)
	{
		return FMath::Abs(Value) < 0.0001f;
	}

	// SetupAndPlaySound (SoundTableImpl.uc:71-118): unset entry values take the
	// source's TransientSoundVolume (1) and TransientSoundRadius (0) and the
	// table's DefaultPitch (1); the multipliers then scale them, so an unset
	// radius stays 0 and the engine default applies only at playback. The
	// source plays it in the row's slot with the row's bNoOverride.
	void PlayPickedSound(AActor& Source, const FWoTSoundTableEntry& Pick,
		float VolumeMultiplier, float RadiusMultiplier, float PitchMultiplier)
	{
		const float Volume =
			(UnsetSoundValue(Pick.Volume) ? 1.0f : Pick.Volume) * VolumeMultiplier;
		const float Radius =
			(UnsetSoundValue(Pick.Radius) ? 0.0f : Pick.Radius) * RadiusMultiplier;
		const float Pitch =
			(UnsetSoundValue(Pick.Pitch) ? 1.0f : Pick.Pitch) * PitchMultiplier;
		WoTSound::PlayReplicatedFrom(Source, Pick.SoundSubPath,
			static_cast<WoTSound::ESlot>(Pick.EngineSlot), Volume, Pick.bNoOverride,
			Radius, Pitch);
	}
}

void FWoTSoundTableBuilder::Install(FWoTSoundSet&& Set)
{
	TUniquePtr<FWoTSoundSet> Candidate = MakeUnique<FWoTSoundSet>(MoveTemp(Set));
	Swap(InstalledSoundRegistry(), Candidate);
}

#if WITH_AUTOMATION_TESTS
void FWoTSoundTableBuilder::Reset()
{
	InstalledSoundRegistry().Reset();
}
#endif

namespace WoTSoundTable
{
	const TArray<FWoTSoundTableEntry>* Find(FName TableName)
	{
		return TableName.IsNone() ? nullptr : ActiveTables().Find(TableName);
	}

	const TArray<FWoTSoundSlotTimer>* FindTimers(FName TableName)
	{
		return TableName.IsNone() ? nullptr : ActiveTimerLists().Find(TableName);
	}

	void InitializeSlotTimers(AWoTCharacter& Pawn)
	{
		const TArray<FWoTSoundSlotTimer>* List = FindTimers(Pawn.SoundTableName);
		UWorld* World = Pawn.GetWorld();
		if (!List || !World) return;
		const float Now = World->GetTimeSeconds();
		for (const FWoTSoundSlotTimer& Timer : *List)
		{
			Pawn.SoundSlotCurrentOdds.Add(Timer.Slot, Timer.Odds);
			Pawn.SoundSlotNextTime.Add(Timer.Slot, Now + WoTRandom::FRandRange(
				Timer.MinSecs, Timer.MaxSecs));
		}
	}

	const FWoTSoundTableEntry* PickSlotSound(FName TableName, FName Slot,
	                                         const UWorld* World)
	{
		const TArray<FWoTSoundTableEntry>* Table = Find(TableName);
		if (!Table || Slot.IsNone())
		{
			return nullptr;
		}
		// PlayRandomSlotSound (SoundTableImpl.uc:141-220): collect + weighted pick;
		// a single match plays only if FRand() < its odds.
		TArray<const FWoTSoundTableEntry*, TInlineAllocator<10>> Matches;
		float TotalOdds = 0.0f;
		for (const FWoTSoundTableEntry& Entry : *Table)
		{
			if (Entry.Slot == Slot && ValidForGameType(Entry.GameType, World))
			{
				Matches.Add(&Entry);
				TotalOdds += Entry.Odds;
			}
		}
		if (Matches.Num() == 0)
		{
			return nullptr;
		}
		if (Matches.Num() == 1)
		{
			return WoTRandom::FRand() < TotalOdds ? Matches[0] : nullptr;
		}
		const float RandVal = WoTRandom::FRand() * TotalOdds;
		float Accum = 0.0f;
		for (const FWoTSoundTableEntry* Entry : Matches)
		{
			Accum += Entry->Odds;
			if (Accum >= RandVal)
			{
				return Entry;
			}
		}
		return Matches.Last();
	}

	bool PlaySlotSound(AWoTCharacter& Pawn, FName Slot, float VolumeMultiplier,
	                   float RadiusMultiplier, float PitchMultiplier)
	{
		UWorld* World = Pawn.GetWorld();
		if (!World || World->bIsTearingDown || !Pawn.HasAuthority())
		{
			return false;
		}
		// The timer-list gate (TimeToDoSound, SoundSlotTimerListImpl.uc:105-152):
		// unlisted slots are uncontrolled; odds mode gates by FRand (bReset flips the
		// slot to timed after its first check). A non-reset successful roll during
		// cooldown changes its mutable odds to 1, forcing the next eligible attempt.
		const FWoTSoundSlotTimer* Timer = nullptr;
		if (const TArray<FWoTSoundSlotTimer>* List = FindTimers(Pawn.SoundTableName))
		{
			for (const FWoTSoundSlotTimer& Row : *List)
			{
				if (Row.Slot == Slot)
				{
					Timer = &Row;
					break;
				}
			}
		}
		const float Now = World->GetTimeSeconds();
		if (Timer)
		{
			float* StoredOdds = Pawn.SoundSlotCurrentOdds.Find(Slot);
			if (!StoredOdds)
			{
				StoredOdds = &Pawn.SoundSlotCurrentOdds.Add(Slot, Timer->Odds);
			}
			if (Pawn.SoundSlotsSwitchedToTimed.Contains(Slot))
			{
				*StoredOdds = 0.0f;
			}
			if (*StoredOdds != 0.0f)
			{
				const bool bOddsSucceeded = WoTRandom::FRand() < *StoredOdds;
				if (Timer->bReset)
				{
					*StoredOdds = 0.0f;
					Pawn.SoundSlotsSwitchedToTimed.Add(Slot);
				}
				if (!bOddsSucceeded)
				{
					return false;
				}
				if (!Timer->bReset && Timer->MinSecs != 0.0f)
				{
					if (const float* Next = Pawn.SoundSlotNextTime.Find(Slot);
						Next && Now < *Next)
					{
						*StoredOdds = 1.0f;
						return false;
					}
					*StoredOdds = Timer->Odds;
				}
			}
			else if (const float* Next = Pawn.SoundSlotNextTime.Find(Slot);
			         Next && Now < *Next)
			{
				return false;
			}
		}

		if (Timer)
		{
			Pawn.SoundSlotNextTime.Add(
			    Slot, Now + WoTRandom::FRandRange(Timer->MinSecs, Timer->MaxSecs));
		}
		const FWoTSoundTableEntry* Pick = PickSlotSound(Pawn.SoundTableName, Slot, World);
		if (!Pick)
		{
			return false;
		}
		PlayPickedSound(Pawn, *Pick, VolumeMultiplier, RadiusMultiplier, PitchMultiplier);
		AWoTNPCController::BroadcastNoise(World, &Pawn, 1.0f, &Pawn);
		return true;
	}

	bool PlaySlotSound(AActor& Actor, FName TableName, FName Slot,
	                   float VolumeMultiplier, float RadiusMultiplier,
	                   float PitchMultiplier)
	{
		UWorld* World = Actor.GetWorld();
		if (!World || World->bIsTearingDown || !Actor.HasAuthority())
		{
			return false;
		}
		const FWoTSoundTableEntry* Pick = PickSlotSound(TableName, Slot, World);
		if (!Pick)
		{
			return false;
		}
		PlayPickedSound(Actor, *Pick, VolumeMultiplier, RadiusMultiplier, PitchMultiplier);
		Actor.MakeNoise(1.0f, Actor.GetInstigator());
		if (APawn* Instigator = Actor.GetInstigator())
		{
			AWoTNPCController::BroadcastNoise(World, &Actor, 1.0f, Instigator);
		}
		return true;
	}
}

AWoTSoundBurst::AWoTSoundBurst()
{
	bReplicates = true;
	bNetTemporary = true;
	// Just under the default, so a playing actor spawned in the same frame
	// (a projectile and its launch sound) reaches clients first and the
	// marker's Source resolves there.
	NetPriority = 0.9f;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AWoTSoundBurst::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTSoundBurst, SoundSubPath);
	DOREPLIFETIME(AWoTSoundBurst, Radius);
	DOREPLIFETIME(AWoTSoundBurst, Volume);
	DOREPLIFETIME(AWoTSoundBurst, Pitch);
	DOREPLIFETIME(AWoTSoundBurst, Source);
	DOREPLIFETIME(AWoTSoundBurst, Slot);
	DOREPLIFETIME(AWoTSoundBurst, bNoOverride);
}

void AWoTSoundBurst::BeginPlay()
{
	Super::BeginPlay();
	// Every machine plays locally (the authority immediately, clients when the
	// marker replicates); dedicated servers have no listeners.
	UWorld* World = GetWorld();
	if (World && World->GetNetMode() != NM_DedicatedServer)
	{
		const AActor* Player = LocalSource.IsValid() ? LocalSource.Get() : Source.Get();
		USoundBase* Wave = WoTSound::LoadBySubPath(SoundSubPath);
		// A missing wave plays nothing; a None one stops its slot.
		if (Wave || (SoundSubPath.IsNone() && Player))
		{
			WoTSound::FPlayParams Params;
			Params.Sound = Wave;
			Params.Source = Player;
			// Where the server heard it start; the source is followed from here.
			Params.Location = GetActorLocation();
			Params.Slot = static_cast<WoTSound::ESlot>(Slot);
			Params.bNoOverride = bNoOverride;
			Params.Volume = Volume;
			Params.Radius = Radius;
			Params.Pitch = Pitch;
			// The server's play: on a client it takes the server's slots, apart
			// from the sounds the client plays itself.
			Params.Caller = WoTSound::ECaller::Server;
			WoTSound::PlaySound(this, Params);
		}
	}
	SetLifeSpan(1.0f);
}

namespace
{
	AWoTSoundBurst* SpawnSoundBurst(UWorld& World, AActor* PlayingActor,
		FName SoundSubPath, const FVector& Location, WoTSound::ESlot Slot,
		float Volume, bool bNoOverride, float Radius, float Pitch)
	{
		if (World.bIsTearingDown || World.GetNetMode() == NM_Client)
		{
			return nullptr;
		}
		const FTransform At(Location);
		AWoTSoundBurst* Marker = World.SpawnActorDeferred<AWoTSoundBurst>(
			AWoTSoundBurst::StaticClass(), At, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Marker)
		{
			Marker->SoundSubPath = SoundSubPath;
			Marker->Radius = Radius;
			Marker->Volume = Volume;
			Marker->Pitch = Pitch;
			Marker->Slot = static_cast<uint8>(Slot);
			Marker->bNoOverride = bNoOverride;
			Marker->LocalSource = PlayingActor;
			// Sent as a plain reference. A client resolves it only when the
			// actor replicates to that client or was loaded with the map;
			// otherwise it arrives empty and the client plays the sound where
			// it started, outside any slot.
			Marker->Source = PlayingActor;
			// Only players inside PlaySound's hearing radius receive it.
			const float HearingRadius = Radius > 0.0f
				? Radius : WoTSound::DefaultPlaySoundRadius;
			Marker->SetNetCullDistanceSquared(FMath::Square(HearingRadius) / 1.3f);
			Marker->FinishSpawning(At);
		}
		return Marker;
	}
}

AWoTSoundBurst* WoTSound::PlayReplicatedAsset(UWorld& World,
                                         const TSoftObjectPtr<USoundBase>& Sound,
                                         const FVector& Location, float RadiusUU,
                                         float VolumeMult, float PitchMult)
{
	const FName SubPath = WoTSound::SubPathOf(Sound);
	return SubPath.IsNone() ? nullptr
		: PlayReplicated(World, SubPath, Location, RadiusUU, VolumeMult, PitchMult);
}

AWoTSoundBurst* WoTSound::PlayReplicated(UWorld& World, FName SoundSubPath,
                                         const FVector& Location, float RadiusUU,
                                         float VolumeMult, float PitchMult)
{
	return SoundSubPath.IsNone() ? nullptr
		: SpawnSoundBurst(World, nullptr, SoundSubPath, Location, ESlot::None,
			VolumeMult, /*bNoOverride*/ false, RadiusUU, PitchMult);
}

AWoTSoundBurst* WoTSound::PlayReplicatedFrom(AActor& Source, FName SoundSubPath,
                                             ESlot Slot, float Volume, bool bNoOverride,
                                             float Radius, float Pitch)
{
	UWorld* World = Source.GetWorld();
	if (!World || (SoundSubPath.IsNone() && Slot == ESlot::None))
	{
		return nullptr;
	}
	return SpawnSoundBurst(*World, &Source, SoundSubPath, Source.GetActorLocation(),
		Slot, Volume, bNoOverride, Radius, Pitch);
}

AWoTSoundBurst* WoTSound::PlayReplicatedAssetFrom(AActor& Source,
                                                  const TSoftObjectPtr<USoundBase>& Sound,
                                                  ESlot Slot, float Volume,
                                                  bool bNoOverride, float Radius,
                                                  float Pitch)
{
	const FName SubPath = WoTSound::SubPathOf(Sound);
	return SubPath.IsNone() ? nullptr
		: PlayReplicatedFrom(Source, SubPath, Slot, Volume, bNoOverride, Radius, Pitch);
}
