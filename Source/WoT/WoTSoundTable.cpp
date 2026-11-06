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
		// SetupAndPlaySound (SoundTableImpl.uc:71-118): unset per-entry values fall
		// back (volume/pitch 1.0; radius 1600 — TransientSoundRadius ships unset),
		// then the pawn's multipliers scale them.
		const float Volume =
		    (Pick->Volume > KINDA_SMALL_NUMBER ? Pick->Volume : 1.0f) * VolumeMultiplier;
		const float Radius =
		    (Pick->Radius > KINDA_SMALL_NUMBER ? Pick->Radius : 1600.0f) * RadiusMultiplier;
		const float Pitch =
		    (Pick->Pitch > KINDA_SMALL_NUMBER ? Pick->Pitch : 1.0f) * PitchMultiplier;
		WoTSound::PlayReplicated(*World, Pick->SoundSubPath, Pawn.GetActorLocation(),
		                         Radius, Volume, Pitch);
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
		const float Volume =
		    (Pick->Volume > KINDA_SMALL_NUMBER ? Pick->Volume : 1.0f) * VolumeMultiplier;
		const float Radius =
		    (Pick->Radius > KINDA_SMALL_NUMBER ? Pick->Radius : 1600.0f) * RadiusMultiplier;
		const float Pitch =
		    (Pick->Pitch > KINDA_SMALL_NUMBER ? Pick->Pitch : 1.0f) * PitchMultiplier;
		WoTSound::PlayReplicated(*World, Pick->SoundSubPath, Actor.GetActorLocation(),
		                         Radius, Volume, Pitch);
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
	DOREPLIFETIME(AWoTSoundBurst, SlotOwner);
}

namespace
{
	// Per-owner UE1 SLOT_Misc channel: the latest slot sound's component per
	// machine. Weak values — components die with their attach parent or on
	// natural finish; entries are lazily replaced.
	TMap<FObjectKey, TWeakObjectPtr<UAudioComponent>> GWoTSlotSounds;
}

void AWoTSoundBurst::BeginPlay()
{
	Super::BeginPlay();
	// Every machine plays locally (the authority immediately, clients when the
	// marker replicates); dedicated servers have no listeners.
	if (GetWorld() && GetWorld()->GetNetMode() != NM_DedicatedServer)
	{
		if (USoundBase* S = WoTSound::LoadBySubPath(SoundSubPath))
		{
			AActor* Slot = SlotOwner;
			if (IsValid(Slot) && Slot->GetRootComponent())
			{
				// UE1 slot replacement: stop whatever this owner's misc slot
				// was playing before starting the new wave.
				if (TWeakObjectPtr<UAudioComponent>* Prev =
					GWoTSlotSounds.Find(FObjectKey(Slot)))
				{
					if (UAudioComponent* PrevComp = Prev->Get())
					{
						WoTSound::StopEffectVoice(PrevComp);
					}
				}
				UAudioComponent* Comp = WoTSound::PlayAttachedOneShot(
					this, S, Slot->GetRootComponent(), Slot->GetActorLocation(),
					Radius, Pitch, Volume);
				GWoTSlotSounds.Add(FObjectKey(Slot),
					TWeakObjectPtr<UAudioComponent>(Comp));
			}
			else
			{
				WoTSound::PlayOneShot(this, S, GetActorLocation(), Radius,
					Pitch, Volume);
			}
		}
	}
	SetLifeSpan(1.0f);
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
                                         float VolumeMult, float PitchMult,
                                         AActor* SlotOwner)
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
		Marker->Radius = RadiusUU;
		Marker->Volume = VolumeMult;
		Marker->Pitch = PitchMult;
		Marker->SlotOwner = SlotOwner;
		const float HearingRadius = RadiusUU > 0.0f
			? RadiusUU : WoTSound::DefaultPlaySoundRadius;
		Marker->SetNetCullDistanceSquared(
			FMath::Square(HearingRadius) / 1.3f);
		Marker->FinishSpawning(At);
	}
	return Marker;
}
