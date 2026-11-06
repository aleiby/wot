#include "WoTLeech.h"
#include "../WoTPlayerController.h"

#include "WoTAngrealInventory.h"
#include "../WoTAmbientSound.h"
#include "../WoTCharacter.h"
#include "../WoTSound.h"
#include "../Projectiles/WoTAngrealProjectile.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

UWoTLeech::UWoTLeech()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

void UWoTLeech::TickComponent(float DeltaTime, ELevelTick TickType,
                              FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Victim ambient hum — lazily from Tick on every machine with listeners: a
	// client's leech appears via subobject replication (no AttachTo there), and
	// the Lightning aura's per-victim stamp also lands after creation.
	UpdateAmbientHum();

	// AffectHost timing. The timer accumulates on ALL roles (cheap; mirrors the original's
	// all-roles Leech.Tick accumulation, Leech.uc:117-141), but the effect DISPATCH is
	// authority-only (N3): every shipped `AffectHost` override is a server `function`
	// (DecreaseHealth/Decrease*Charges/DestroyOnDestroy/EarthTremor gameplay + Whirlwind's
	// dead-host detach), so on a replicated visual leech the original ran a client no-op. Gating
	// the dispatch reproduces that AND fixes real client hazards our uniform replication exposed
	// (DecreaseCharges mutated CurCharges; DestroyOnDestroy self-destructed every client tick,
	// its DestroyActor being non-replicated → always null on clients). TrapDetect's
	// owner-client detection/presentation now runs in its subclass TickComponent;
	// AffectHost remains the standalone/authority heartbeat cadence.
	if (AffectResolution > 0.0f)
	{
		AffectTimer += DeltaTime;
		if (AffectTimer >= AffectResolution)
		{
			int32 Iterations = 0;
			while (AffectTimer >= AffectResolution)
			{
				if (AffectResolutionScale != 1.0f)
				{
					AffectResolution *= AffectResolutionScale;
				}
				AffectTimer -= AffectResolution;
				++Iterations;
			}
			if (GetOwnerRole() == ROLE_Authority)
			{
				AffectHost(Iterations);
			}
		}
	}

	// Lifespan / expiry-notify — authority only, and only if not permanent.
	// UE1 let the engine decrement Actor.LifeSpan and call Expired(); we do it
	// here (UActorComponent has no LifeSpan).
	if (GetOwnerRole() == ROLE_Authority && Lifespan > 0.0f)
	{
		Lifespan -= DeltaTime;
		if (Lifespan <= 0.0f)
		{
			Expired();
			return;
		}

		NotifyExpireTimer -= DeltaTime;
		if (NotifyExpireTimer < 0.0f)
		{
			NotifyExpireTimer += NotifyExpireResolution;
			NotifyOwnerExpiring();
		}
	}
}

void UWoTLeech::UpdateAmbientHum()
{
	if (AmbientSoundPath.IsNone() || !GetOwner() ||
	    GetNetMode() == NM_DedicatedServer)
	{
		StopAmbientHum();
		return;
	}
	// A swap transfer reparents this leech to a new host — re-home the hum.
	if (AmbientHum && AmbientHum->GetOwner() != GetOwner())
	{
		AmbientHum->DestroyComponent();
		AmbientHum = nullptr;
	}
	if (!AmbientHum)
	{
		if (USoundBase* Wave = WoTSound::LoadBySubPath(AmbientSoundPath, true))
		{
			AmbientHum = NewObject<UWoTAmbientSoundComponent>(GetOwner());
			AmbientHum->SetupAttachment(GetOwner()->GetRootComponent());
			AmbientHum->RegisterComponent();
			AmbientHum->ConfigureUE1Ambient(Wave, AmbientSoundRadius,
			                                AmbientSoundVolume, AmbientSoundPitch);
			AmbientHum->Play();
		}
		else
		{
			AmbientSoundPath = NAME_None;   // unresolved: don't retry every tick
		}
	}
}

void UWoTLeech::StopAmbientHum()
{
	if (AmbientHum)
	{
		AmbientHum->Stop();
		AmbientHum->DestroyComponent();
		AmbientHum = nullptr;
	}
}

void UWoTLeech::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// UE1 Unattach == Destroy tore the leech actor (and AttachedSound follower)
	// down on every machine; the audio component follows this leech's teardown.
	StopAmbientHum();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UWoTLeech::AttachTo(APawn* NewHost)
{
	if (!IsValid(NewHost))
	{
		return;
	}

	// A reflector on the host may throw away the incoming effect — an element shield
	// blocks same-element weaves; an anti-magic dome blocks every deleterious one.
	// A fire leech never lands on a fire-shielded victim; no bad leech lands inside
	// an Aura of Unraveling.
	// (Skipped when Swap Places is MOVING us — retail Leech.AttachTo doesn't block.)
	if (!bTransferring)
	{
		if (AWoTCharacter* Ch = Cast<AWoTCharacter>(NewHost))
		{
			if (Ch->IsEffectBlocked(SourceAngreal, bDeleterious,
				/*bEmitFeedback*/ true))
			{
				DestroyComponent();
				return;
			}
		}
	}

	// Don't attach if singular and the host already carries our class. (We are
	// not registered yet, so GetComponents won't return `this`.)
	if (bSingular)
	{
		TArray<UWoTLeech*> Existing;
		NewHost->GetComponents<UWoTLeech>(Existing);
		for (UWoTLeech* L : Existing)
		{
			if (L != this && IsValid(L) && L->GetClass() == GetClass())
			{
				DestroyComponent();   // == UE1 Destroy()
				return;
			}
		}
	}

	// Remove existing of the same class.
	if (bRemoveExisting)
	{
		TArray<UWoTLeech*> Existing;
		NewHost->GetComponents<UWoTLeech>(Existing);
		for (UWoTLeech* L : Existing)
		{
			if (L != this && IsValid(L) && L->GetClass() == GetClass())
			{
				L->Unattach();
			}
		}
	}

	// Register on the host (adds to its components + starts ticking).
	if (!IsRegistered())
	{
		RegisterComponent();
	}
	{
		static uint64 GWoTLeechAttachSequence = 0;
		AttachSeq = ++GWoTLeechAttachSequence;
	}

	// Leech.uc:254-257: no StatusIcon gate here; AddIconInfo resolves our frame
	// override or falls back to SourceAngreal.
	if (bDisplayIcon)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(NewHost);
			IsValid(Character) && !Character->IsNPCPawn())
		{
			Character->AddIconInfo(this, !bDeleterious, GetFName());
		}
	}

	// Arm the expiry-notify timer.
	NotifyExpireTimer = Lifespan - NotifyExpireTime;
	// Authority-created leeches can start immediately; replicated client leeches
	// still enter through Tick once their instance sound fields arrive.
	UpdateAmbientHum();
}

void UWoTLeech::Unattach()
{
	RemoveStatusIconFromCurrentHost();
	StopAmbientHum();

	// Removing a leech from its host == destroying its component (the UE1 list
	// removal + SetOwner(None) + the caller's Destroy() collapse to this).
	DestroyComponent();
}

void UWoTLeech::RemoveStatusIconFromCurrentHost()
{
	if (bDisplayIcon)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner());
			IsValid(Character) && !Character->IsNPCPawn())
		{
			Character->RemoveIconInfo(GetFName());
		}
	}
}

void UWoTLeech::Expired()
{
	Unattach();
}

void UWoTLeech::NotifyOwnerExpiring()
{
	// The shipped body has no icon-flash call: it only plays the fading expiry ping
	// (Leech.uc:165-181 -> WOT.Notification.DefaultExpiring via
	// Owner.PlaySound at NotifyExpireVolume, halving each repeat) — owner-only.
	if (bDisplayIcon)
	{
		if (APawn* Host = Cast<APawn>(GetOuter()))
		{
			if (AWoTPlayerController* PC =
			        Cast<AWoTPlayerController>(Host->GetController()))
			{
				PC->ClientPlayUISound(FName(TEXT("WOT/DefaultExpiring")),
				                      NotifyExpireVolume);
			}
		}
		NotifyExpireVolume *= 0.5f;
	}
}

void UWoTLeech::SetSourceAngreal(AWoTAngrealInventory* Source)
{
	SourceAngreal = Source;
	if (Source)
	{
		LeechInstigator = Cast<APawn>(Source->GetOwner());
	}
}

void UWoTLeech::SetSourceProjectile(AWoTAngrealProjectile* Source)
{
	SourceProjectile = Source;
	SourceProjClass = Source ? Source->GetClass() : nullptr;
	// AttachLeechEffect.uc:94-95: bFromProjectile = (SourceProjectile != None). A
	// projectile-installed leech is NOT swap/cleanse-transferable.
	bFromProjectile = (Source != nullptr);
}

void UWoTLeech::InitializeWithLeech(UWoTLeech* Source)
{
	if (!Source)
	{
		return;
	}
	SourceAngreal = Source->SourceAngreal;
	SourceProjectile = Source->SourceProjectile;
	SourceProjClass = Source->SourceProjClass;
	LeechInstigator = Source->LeechInstigator;
}

void UWoTLeech::InitializeWithProjectile(AWoTAngrealProjectile* Source)
{
	if (!Source)
	{
		return;
	}
	SourceAngreal = Source->SourceAngreal;
	SourceProjectile = Source;
	SourceProjClass = Source->GetClass();
	LeechInstigator = Source->SourcePawn;
	bFromProjectile = true;   // AttachLeechEffect.uc:94-95 (installed by a projectile)
}

void UWoTLeech::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Original: reliable if Role==Authority (server->client) — the default.
	DOREPLIFETIME(UWoTLeech, SourceAngreal);
	DOREPLIFETIME(UWoTLeech, SourceProjectile);
	DOREPLIFETIME(UWoTLeech, SourceProjClass);
	// Presentation-owning leeches (Ice) need the accepted instance's actual
	// remaining lifetime, rather than guessing from the class default.
	DOREPLIFETIME(UWoTLeech, Lifespan);
	// Victim ambient hum: class defaults match both sides, but the Lightning aura
	// stamps its per-victim health leeches at latch time — instance values travel.
	DOREPLIFETIME(UWoTLeech, AmbientSoundPath);
	DOREPLIFETIME(UWoTLeech, AmbientSoundRadius);
	DOREPLIFETIME(UWoTLeech, AmbientSoundVolume);
	DOREPLIFETIME(UWoTLeech, AmbientSoundPitch);
}
