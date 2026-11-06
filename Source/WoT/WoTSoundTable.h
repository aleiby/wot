#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "GameFramework/Actor.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTSoundTable.generated.h"

class AWoTCharacter;
class FWoTRuntimeClassDefaults;
class USoundBase;

// One SoundList entry of a shipped SoundTable*.uc (SoundTableInterf.uc:24-43): the
// WOT category slot, weighted odds (single-entry = play-probability, multi-entry =
// weighted always-fires — SoundTableImpl.uc:176-204), the game-type filter, and the
// engine play parameters (0 = unset -> TransientSound*/1.0 fallbacks). The engine
// mixing channel (SLOT_Talk etc.) is dropped — UE5 has no per-actor sound slots.
USTRUCT()
struct FWoTSoundTableEntry
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName Slot;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Odds = 0.0f;
	// EGameType: 0 All, 1 SP, 2 MP, 3 Battle, 4 Arena, 5 None. GT_Arena is DEAD in
	// shipped (a duplicate GT_Battle case shadows it; unknowns warn + return TRUE,
	// SoundTableWOT.uc:95-109) — the port preserves "Arena always plays".
	UPROPERTY(EditDefaultsOnly, Category = "WoT") uint8 GameType = 0;
	// Imported asset subpath under /Game/WoT/Sounds ("TrollocA/Tro_DieHard1" —
	// the UE1 Pkg.Group.Name group folds away at import).
	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName SoundSubPath;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Volume = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Radius = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Pitch = 0.0f;
};

// One SlotTimers row (SoundSlotTimerListInterf.uc:45-53): a per-slot cooldown —
// odds-gated while Odds != 0 (bReset flips it to timed after the first check),
// else a MinSecs-MaxSecs random window. Checked BEFORE selection.
USTRUCT()
struct FWoTSoundSlotTimer
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "WoT") FName Slot;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float Odds = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float MinSecs = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") float MaxSecs = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT") bool bReset = false;
};

/** Complete sound-entry and slot-timer families built from retail CDOs. */
struct FWoTSoundSet
{
	TMap<FName, TArray<FWoTSoundTableEntry>> Tables;
	TMap<FName, TArray<FWoTSoundSlotTimer>> TimerLists;
};

/** Pure construction and explicit publication boundary used by payload activation. */
struct FWoTSoundTableBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("sound-tables");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTSoundSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTSoundSet& Set);
	static void Install(FWoTSoundSet&& Set);

	// Stable canonical bytes and structural equivalence support for fixtures.
	static void SerializeCanonical(const FWoTSoundSet& Set, TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTSoundSet& Built,
		const FWoTSoundSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

// Activated sound/timer rows and the PlaySlotSound resolution:
// timer gate -> game-type filter -> weighted pick -> networked one-shot.
namespace WoTSoundTable
{
	const TArray<FWoTSoundTableEntry>* Find(FName TableName);
	const TArray<FWoTSoundSlotTimer>* FindTimers(FName TableName);
	// SoundSlotTimerListImpl.PreBeginPlay: seed every listed cooldown at spawn.
	void InitializeSlotTimers(AWoTCharacter& Pawn);

	// PlayRandomSlotSound's selection only (no timer gate, no play) — for specs.
	const FWoTSoundTableEntry* PickSlotSound(FName TableName, FName Slot,
	                                         const UWorld* World);

	// The full shipped path (SoundTableImpl.PlaySlotSound): the pawn's timer list
	// gates, the table picks, per-entry values fall back (volume/pitch 1.0, radius
	// 1600 — UE1 left TransientSoundRadius unset), multipliers scale, and the sound
	// plays networked (every machine). False when gated/missing/skipped.
	bool PlaySlotSound(AWoTCharacter& Pawn, FName Slot, float VolumeMultiplier = 1.0f,
	                   float RadiusMultiplier = 1.0f, float PitchMultiplier = 1.0f);

	// The non-pawn voice path (MashadarGuide.uc:1706-1710: the guide routes its
	// table's slots with a None timer list — no shipped non-pawn caller authors
	// one, so this overload is timer-free): authority gate -> pick -> fallbacks ->
	// networked play at the actor.
	bool PlaySlotSound(AActor& Actor, FName TableName, FName Slot,
	                   float VolumeMultiplier = 1.0f, float RadiusMultiplier = 1.0f,
	                   float PitchMultiplier = 1.0f);
}

// The replicated one-shot sound event (gore-4d marker pattern): UE1's PlaySound
// replicated natively; the port's WoTSound::PlayOneShot is local-only, so
// authority-observed sounds ride this thin bNetTemporary marker — every machine's
// BeginPlay plays the sound locally with the UE1 linear-falloff attenuation.
UCLASS()
class AWoTSoundBurst : public AActor
{
	GENERATED_BODY()

public:
	AWoTSoundBurst();

	UPROPERTY(Replicated) FName SoundSubPath;
	UPROPERTY(Replicated) float Radius = 1600.0f;
	UPROPERTY(Replicated) float Volume = 1.0f;
	UPROPERTY(Replicated) float Pitch = 1.0f;
	// UE1 Owner.PlaySound slot semantics (SLOT_Misc): when set, each machine
	// plays the wave ATTACHED to this actor and a later slot sound on the same
	// owner STOPS the previous one. Both bounds retail relied on to end
	// looping one-shots (smpl-chunk waves like LaunchLS): EndLS replaced the
	// loop in the slot, and pawn teardown freed the channel. Fire-and-forget
	// playback (SlotOwner unset) leaves a looping wave immortal (a stuck
	// cast hum).
	UPROPERTY(Replicated) AActor* SlotOwner = nullptr;

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};

namespace WoTSound
{
	// Authority: spawn the replicated sound marker (deferred so the payload lands
	// before BeginPlay). SubPath is under /Game/WoT/Sounds. No-op off authority.
	// SlotOwner engages the UE1 pawn-slot semantics documented on AWoTSoundBurst.
	AWoTSoundBurst* PlayReplicated(UWorld& World, FName SoundSubPath,
	                               const FVector& Location, float RadiusUU,
	                               float VolumeMult = 1.0f, float PitchMult = 1.0f,
	                               AActor* SlotOwner = nullptr);

	// The same for an asset reference under /Game/WoT/Sounds (no-op for a null or
	// out-of-tree reference): a UE1 server-side Actor.PlaySound.
	AWoTSoundBurst* PlayReplicatedAsset(UWorld& World,
	                                    const TSoftObjectPtr<USoundBase>& Sound,
	                                    const FVector& Location, float RadiusUU,
	                                    float VolumeMult = 1.0f, float PitchMult = 1.0f);
}
