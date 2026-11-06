#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WoTLeech.generated.h"

// Port of WOT.Leech (LegendActorComponent : Actor), providing the shared
// component machinery for effects that remain attached over time (Leech.uc:7).
//
// UE5 mapping (first-of-kind framework; PORT_NOTES):
//  - Leech "attached to a host pawn" -> a UActorComponent on that pawn, matching
//    the ratified Inventory->UWoTInventoryComponent collapse. The per-host
//    singly-linked list (WOTPlayer/WOTPawn.FirstLeech) and LeechIterator become
//    the host's own UWoTLeech components (GetComponents<UWoTLeech>); WOTPlayer +
//    WOTPawn both collapse to the pawn (the PlayerPawn split).
//  - Subclasses implement their periodic host mutation in AffectHost(); Lifespan
//    (0 = permanent) and AffectResolution retain the source contract
//    (Leech.uc:415-416).
//  - bDisplayIcon/StatusIconFrame feed the owning character's status-icon list.
//    NotifyOwnerExpiring's shipped body only plays its halving-volume sound; the
//    nearby "flash our icon" text is comment-only. InitializeWithReflector/
//    Invokable remain omitted until those frameworks are ported.
UCLASS(Abstract, Blueprintable)
class UWoTLeech : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoTLeech();

	// Artifact that produced this effect (Leech.uc:35).
	UPROPERTY(Replicated) TObjectPtr<class AWoTAngrealInventory> SourceAngreal;
	// Projectile source when installation came through one; otherwise null
	// (Invokable.uc:28).
	UPROPERTY(Replicated) TObjectPtr<class AWoTAngrealProjectile> SourceProjectile;
	UPROPERTY(Replicated) TSubclassOf<class AWoTAngrealProjectile> SourceProjClass;

	// UE1 Actor.Instigator (UActorComponent has none) — the pawn responsible.
	UPROPERTY() TObjectPtr<APawn> LeechInstigator;

	// How long this leech lasts; live remaining seconds, 0 = permanent
	// (Actor.LifeSpan in the original; the engine decremented it there).
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Leech") float Lifespan = 0.0f;

	// Retail leeches live in a prepend linked list (newest first) that
	// LeechIterator walks; Absorb's install steal takes the FIRST eligible one.
	// Stamped from a process-monotonic counter by AttachTo().
	UPROPERTY() uint64 AttachSeq = 0;

	// Timer support — AffectHost gets called every AffectResolution seconds.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") float AffectResolution = 0.0f;
	float AffectTimer = 0.0f;
	// After each AffectHost pass, multiply the next interval by this value;
	// values above one slow the cadence and values below one hasten it
	// (Leech.uc:18-19).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") float AffectResolutionScale = 1.0f;

	// Marks an effect as harmful (Leech.uc:55).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") bool bDeleterious = false;
	// When enabled, show SourceAngreal's status icon on the host HUD
	// (Leech.uc:57-58).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") bool bDisplayIcon = false;
	// May clients remove us?
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") bool bRemovable = true;
	// If set, AttachTo fails when the host already carries a leech of this class.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") bool bSingular = false;
	// Before attaching, remove every existing leech of the same class when set
	// (Leech.uc:68).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") bool bRemoveExisting = false;
	// True if this leech came from a projectile (shift/swap eligibility).
	UPROPERTY() bool bFromProjectile = false;
	// Set only while Swap Places MOVES this leech to a new host: retail Leech.AttachTo
	// re-links without re-running effect blocking, so a transfer must skip the
	// IsEffectBlocked check (SwapPlacesEffect.uc:310-320; Leech.uc AttachTo).
	bool bTransferring = false;

	// Expiry notification sound (there is no shipped icon-flash statement).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") float NotifyExpireTime = 2.0f;
	float NotifyExpireTimer = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") float NotifyExpireResolution = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech") float NotifyExpireVolume = 1.0f;
	// Original identifier retained for data parity; NotifyOwnerExpiring plays the
	// imported WOT/DefaultExpiring owner-only UI sound directly.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech")
	TSoftObjectPtr<class USoundBase> NotifyExpireSound;
	// Optional status-frame override; otherwise AddIconInfo uses SourceAngreal's.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Leech")
	TSoftObjectPtr<class UTexture2D> StatusIconFrame;

	// UE1 leech ambient hum on the victim: LightningLeech/WhirlwindLeech carried
	// an AmbientSound; DecreaseCommonChargesLeech spawned an AttachedSound
	// follower (AttachedSound.uc — a DT_None leech pinning AmbientSound to the
	// Owner). The port loops it via a UWoTAmbientSoundComponent on the host,
	// immediately on authority attach and lazily from Tick on replicated clients
	// (no AttachTo runs there). Replicated because the
	// Lightning aura stamps its per-victim health leeches at latch time.
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Sound")
	FName AmbientSoundPath;
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Sound")
	uint8 AmbientSoundRadius = 32;
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Sound")
	uint8 AmbientSoundVolume = 128;
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Sound")
	uint8 AmbientSoundPitch = 64;
	UPROPERTY() TObjectPtr<class UWoTAmbientSoundComponent> AmbientHum;

	// --- lifecycle / support (spec names) ---

	// Attach to a host pawn: honor bSingular/bRemoveExisting, register (start
	// ticking), and arm the expiry-notify timer. The component must have been
	// created with NewHost as its owner (NewObject<...>(NewHost)). Virtual so
	// concretes (e.g. UWoTIceLeech) can hook host state on attach/detach; they
	// must call Super. Unattach() dispatches from Expired(), so it too is virtual.
	virtual void AttachTo(APawn* NewHost);
	// Remove only this producer's HUD entry from its current host. Swap Places
	// needs the unlink half of retail UnAttach without destroying the component.
	void RemoveStatusIconFromCurrentHost();
	// Remove ourselves from the host (== UE1 Unattach + Destroy).
	virtual void Unattach();
	// Undo any host-state changes WITHOUT destroying the component. Retail's swap
	// transfer runs L.UnAttach() before AttachTo (SwapPlacesEffect.uc:294) and
	// concrete leeches restore host state there (WhirlwindLeech.uc:83-97 restores
	// physics); our Unattach() destroys, so a transfer calls this on the OLD host
	// just before reparenting. Concretes that mutate their host override it (and
	// route their own Unattach through it).
	virtual void OnHostDetached(APawn* OldHost) {}

	// Expiry hook invoked when Lifespan runs out (Leech.uc:106).
	virtual void Expired();

	virtual float GetInitialDuration() const { return GetDuration(); }
	virtual float GetDuration() const { return Lifespan; }

	// These must be called when the leech is created (original contract).
	void SetSourceAngreal(class AWoTAngrealInventory* Source);
	void SetSourceProjectile(class AWoTAngrealProjectile* Source);
	void InitializeWithLeech(UWoTLeech* Source);
	void InitializeWithProjectile(class AWoTAngrealProjectile* Source);

	// Subclass hook for each scheduled host update; Iterations accounts for
	// missed AffectResolution intervals (Leech.uc:415-416).
	virtual void AffectHost(int32 Iterations = 0) {}

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	// Play the fading notification as we near expiry (authority).
	void NotifyOwnerExpiring();
	// Lazily start (or re-home after a swap transfer) the victim ambient hum.
	void UpdateAmbientHum();
	// Explicitly stop and destroy the victim-owned audio sibling. Detach/expiry
	// must silence it immediately, before replicated component teardown completes.
	void StopAmbientHum();
};
