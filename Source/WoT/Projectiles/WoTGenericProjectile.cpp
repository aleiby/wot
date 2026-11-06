#include "WoTGenericProjectile.h"
#include "../WoTWeaveFX.h"

#include "../WoTDecal.h"

#include "../WoTMover.h"
#include "../WoTCharacter.h"
#include "../AI/WoTNPCCharacter.h"
#include "../WoTZoneVolume.h"
#include "../Inventory/WoTReflector.h"
#include "../Inventory/WoTShieldReturnToSenderReflector.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "WoTSeekingProjectile.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

AWoTGenericProjectile::AWoTGenericProjectile()
{
}

void AWoTGenericProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// replication { reliable if(Role==ROLE_Authority) IgnoredPawn, bExplode }
	DOREPLIFETIME(AWoTGenericProjectile, IgnoredPawn);
}

void AWoTGenericProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		// ServerInit(): SpawnSound + SetVelocity(vector(Rotation) * Speed)
		WoTSound::PlayReplicatedAsset(*GetWorld(), SpawnSound,
			GetActorLocation(), SpawnSoundRadius, 1.0f, SpawnSoundPitch);
		ProjectileMovement->Velocity = GetActorRotation().Vector() * Speed;
		ProjectileMovement->InitialSpeed = Speed;
		ProjectileMovement->MaxSpeed = MaxSpeed;
	}
	else if (bNetTemporary)
	{
		// UE1 ROLE_SimulatedProxy + PHYS_Projectile: the client flew locally on the
		// natively-replicated Velocity (GenericProjectile.uc:449, ServerInit's
		// authority-only SetVelocity at :121-129). UE5 delivers that velocity in the
		// bNetTemporary initial burst as ReplicatedMovement.LinearVelocity but never
		// feeds the PMC, so the client copy sits inert at the muzzle.
		// Seed the PMC so it simulates the same trajectory
		// and can reach a locally-detected impact. bNetTemporary-only: the persistent
		// seeker/wall families are driven by the ongoing replicated stream and must
		// NOT be locally simulated (that would fight replication).
		ApplyReplicatedInitialVelocity();
	}
}

void AWoTGenericProjectile::ApplyReplicatedInitialVelocity()
{
	const FVector V = GetReplicatedMovement().LinearVelocity;
	ProjectileMovement->Velocity = V;
	ProjectileMovement->InitialSpeed = V.Size();
	ProjectileMovement->MaxSpeed = MaxSpeed;
}

void AWoTGenericProjectile::ProcessTouch(AActor* Other, FVector HitLocation)
{
	// A dead projectile touches nothing: UE1 never delivers Touch to a destroyed
	// actor, but UE5 can run a queued/manual ProcessTouch on a pending-kill copy —
	// which would skip every interception below (IsProjectileAffectable rejects a
	// dying projectile) yet still deal its damage a second time.
	if (!IsValid(this) || IsActorBeingDestroyed())
	{
		return;
	}

	// Contact with the excluded pawn has no effect (GenericProjectile.uc:172).
	if (Other == IgnoredPawn)
	{
		return;
	}
	// GenericProjectile.ProcessTouch notifies WOTPawn before its reflector chain.
	// AngrealUser's Ice specialization releases the active charge group even when
	// a later Absorb/Reflect interceptor cancels or redirects this impact.
	if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Other))
	{
		NPC->NotifyHitByAngrealProjectile(this);
	}
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	if (HasAuthority())
	{
		// Read-only production contact evidence for guarded proof/play-through
		// harnesses. This fires before reflect/absorb/fork can retire or redirect
		// the projectile, and carries the actual source angreal through `this`.
		const FHitResult ProbeHit(Other, nullptr, HitLocation, FVector::UpVector);
		OnImpactProbe.Broadcast(this, ProbeHit);
	}
#endif

	// Retail runs ProcessTouch's reflector chain on simulated copies too. The
	// compact ordered descriptor on the pawn carries every client-side decision
	// input; Absorb/Reflect retire silently here while authority alone performs
	// artifact transfer, damage, feedback, and replacement spawning. Fork leaves
	// the incoming copy alone and the replicated authority fork arrives normally.
	if (!HasAuthority())
	{
		if (const AWoTCharacter* Character = Cast<AWoTCharacter>(Other);
			Character && Character->ShouldSilentlyRetireProjectile(this))
		{
			bExplode = false;
			Destroy();
			return;
		}
	}

	// The pawn's damage-interception reflectors (Absorb/Reflect/Fork) fire in
	// retail Priority order: Absorb (170) > Reflect (160) > Fork (150). A pawn
	// usually carries at most one, but a pawn with several must resolve them in
	// that order. Authority-side.
	if (HasAuthority())
	{
		if (APawn* ReflectPawn = Cast<APawn>(Other))
		{
			AWoTCharacter* Ch = Cast<AWoTCharacter>(ReflectPawn);

			// Reflect/Fork destination-gate ONLY a seeker that is actually homing;
			// a pass-by seeker (targeting someone else) is left alone. Direct
			// weaves — including a SeekingProjectile that isn't seeking yet —
			// always qualify (ReturnToSenderReflector.uc:207-221). Absorb has
			// no such gate — it snatches any affectable weave that touches you
			// (AbsorbReflector.uc:32).
			AWoTSeekingProjectile* Seek = Cast<AWoTSeekingProjectile>(this);
			const bool bAfterThisPawn =
				!Seek || !Seek->bSeeking || Seek->Destination == Other;

			// Ordered touch dispatch replaces the old hand-coded Absorb -> Reflect ->
			// Fork checks. Shipped priorities (170 > 160 > 150) are equivalent, while
			// equal priorities now honor newest-install-first. Each concrete owns its
			// retail IsAffectable list; the ExpWard entry remains load-bearing because
			// reflecting a ward would leave it permanently inert against its victim.
			TArray<UWoTReflector*> TouchReflectors;
			if (Ch)
			{
				Ch->GetOrderedReflectors(TouchReflectors);
			}
			for (UWoTReflector* TouchReflector : TouchReflectors)
			{
				if (!IsValid(TouchReflector)
					|| !TouchReflector->IsProjectileAffectable(this))
				{
					continue;
				}

				// AbsorbReflector: snatch the firing artifact into the host's inventory
				// and cancel the hit (one-shot).
				if (TouchReflector->OnHitByProjectile(this))
				{
					Destroy();
					return;
				}

				// ReturnToSenderReflector: bounce the weave back at its originator; the
				// host takes no hit ("You are not affected").
				if (bAfterThisPawn && TouchReflector->ReflectsProjectiles())
				{
				TouchReflector->OnProjectileFeedback();
				APawn* OldSender = SourcePawn;
				APawn* Attacker = GetInstigator();
				FVector ForcedReturnVelocity = FVector::ZeroVector;
				AActor* ForcedReturnDestination = nullptr;
				// AdjustVelocity (ReturnToSenderReflector.uc:160-187): a seeker (or a
				// weave with no instigator) just reverses; otherwise the returned weave
				// is re-aimed at the attacker WITH leading (DoAdjustVelocity, uc:150-153
				// — retail respawns the projectile as a UE1 physics workaround; we
				// re-aim in place, preserving speed).
				if ((Seek && Seek->bSeeking) || !Attacker)
				{
					// Reverse direction.
					ForcedReturnVelocity = -ProjectileMovement->Velocity;
					// A seeker after this pawn now homes on the originator.
					if (Seek && Seek->bSeeking && Seek->Destination == Other)
					{
						ForcedReturnDestination = OldSender;
					}
				}
				else if (UWoTShieldReturnToSenderReflector* DeflectShield =
					         Cast<UWoTShieldReturnToSenderReflector>(TouchReflector);
					     DeflectShield && DeflectShield->bDeflect)
				{
					// Soldier shield deflect-away (ShieldReturnToSenderReflector.uc:79-92,
					// bShieldReflectorDeflect=True, Soldier.uc:50): knock the weave back
					// toward the side it came from, blended toward the reverse-velocity
					// vector by the MP difficulty weights — NOT a led re-aim at the shooter.
					// The Questioner keeps bDeflect false and falls to the lead path below.
					ForcedReturnVelocity = DeflectShield->ComputeDeflectVelocity(
						GetActorLocation(), ProjectileMovement->Velocity,
						ReflectPawn->GetActorLocation(),
						UWoTShieldReturnToSenderReflector::MPDifficulty());
				}
				{
				const FRotator Aim = ForcedReturnVelocity.IsNearlyZero()
					? CalculateTrajectory(ReflectPawn, Attacker, Speed)
					: ForcedReturnVelocity.Rotation();
				const FVector ReturnVelocity = ForcedReturnVelocity.IsNearlyZero()
					? Speed * Aim.Vector()
					: ForcedReturnVelocity;
				// ReturnToSenderReflector.uc:160-187 destroys a direct projectile and
				// spawns a replacement with the returned trajectory.  That is not just
				// a UE1 physics workaround: these projectiles are net-temporary, so an
				// in-place server velocity change is never sent after the initial bunch.
				// A replacement gives the caster and every observer the reflected weave.
				const FTransform ReturnTM(Aim, GetActorLocation());
				AWoTGenericProjectile* Returned =
					GetWorld()->SpawnActorDeferred<AWoTGenericProjectile>(
						GetClass(), ReturnTM, GetOwner(), ReflectPawn,
						ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
				if (Returned)
				{
					Returned->SetSourceAngreal(SourceAngreal);
					Returned->SourcePawn = ReflectPawn;
					Returned->SetInstigator(ReflectPawn);
					Returned->bIsForkCopy = bIsForkCopy;
					// A seeking weave has accelerated beyond its CDO Speed by now.
					// EnterSeeking seeds CurrentSpeed from Speed, so carry the live
					// magnitude across the network-visible replacement.
					if (Seek && Seek->bSeeking)
					{
						Returned->Speed = ReturnVelocity.Size();
					}
					if (!Returned->bHurtsOwner)
					{
						Returned->SetIgnoredPawn(ReflectPawn);
					}
					Returned->FinishSpawning(ReturnTM);
					Returned->ProjectileMovement->Velocity = ReturnVelocity;
					Returned->SetActorRotation(ReturnVelocity.Rotation());
					if (ForcedReturnDestination)
					{
						Returned->SetDestination(ForcedReturnDestination);
					}
					// Preserve the reflected state on the retiring UE1 projectile object
					// until end-of-frame. Existing gameplay observers can inspect the
					// same ownership/trajectory transition even though replication uses
					// the replacement actor for remote visibility.
					ProjectileMovement->Velocity = ReturnVelocity;
					SetActorRotation(ReturnVelocity.Rotation());
					SourcePawn = ReflectPawn;
					SetInstigator(ReflectPawn);
					if (!bHurtsOwner) SetIgnoredPawn(ReflectPawn);
					if (UWoTShieldReturnToSenderReflector* Shield =
						Cast<UWoTShieldReturnToSenderReflector>(TouchReflector))
					{
						Shield->RedirectImpactDamage(this);
					}
					Returned->Reflected();
					bExplode = false;
					Destroy();
					return;
				}

					// A reflected direct projectile may never continue in place: if the
					// replacement cannot be allocated, retire the incoming actor cleanly.
					bExplode = false;
					Destroy();
					return;
				}
				// The projectile now belongs to the reflecting pawn (uc:72-83) —
				// after the re-aim, which targets the ORIGINAL instigator.
				// Deflect damages the shield + fires the 'Reflected' voice hook
				// (RedirectImpactDamage, uc:47-61). Only a shield reflector routes damage;
				// the plain Reflect/Fork effects do not. May destroy the shield (which
				// uninstalls this reflector via OnWeaponDestroyed — no further use below).
			}

			// ForkRTSReflector preserves the incoming hit and sends one copy toward
			// its source; an existing fork copy cannot split again (AngrealInvFork.uc:96).
			if (bAfterThisPawn && TouchReflector->ForksProjectiles() && !bIsForkCopy)
			{
				TouchReflector->OnProjectileFeedback();
				// Aim the copy back at the attacker: the fork routes the return
				// through RTS AdjustVelocity (ForkRTSReflector.uc:39), so a seeker
				// (or a weave with no instigator) reverses; a direct weave gets the
				// same led re-aim as Reflect.
				APawn* Attacker = GetInstigator();
				const FRotator BackRot =
					((Seek && Seek->bSeeking) || !Attacker)
						? (-ProjectileMovement->Velocity).Rotation()
						: CalculateTrajectory(ReflectPawn, Attacker, Speed);
				// Deferred spawn: the copy spawns overlapping its host, and
				// begin-overlap fires DURING a plain SpawnActor (PORT_NOTES
				// Ice/EarthTremor gotcha) — the copy's ProcessTouch(host) ran before
				// bIsForkCopy/IgnoredPawn were set, re-forking unbounded and hitting
				// the host once per level. Retail guards with IgnoredProj
				// (ForkRTSReflector.uc:27-43); here all guards land pre-FinishSpawning.
				const FTransform ForkTM(BackRot, GetActorLocation());
				if (AWoTGenericProjectile* ForkCopy =
					GetWorld()->SpawnActorDeferred<AWoTGenericProjectile>(
						GetClass(), ForkTM, /*Owner*/ nullptr, ReflectPawn,
						ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
				{
					ForkCopy->bIsForkCopy = true;
					ForkCopy->SetSourceAngreal(SourceAngreal);
					ForkCopy->SourcePawn = ReflectPawn;
					// SetSourceAngreal restored the attacker as Instigator
					// (AngrealProjectile.uc:110); the fork host is shooting the copy,
					// so retail re-fixes it to the reflecting pawn
					// (ReturnToSenderReflector.uc:72 via ForkRTSReflector.uc:39).
					ForkCopy->SetInstigator(ReflectPawn);
					if (AWoTSeekingProjectile* FSeek = Cast<AWoTSeekingProjectile>(ForkCopy))
					{
						if (Seek && Seek->bSeeking)
						{
							FSeek->Speed = ProjectileMovement->Velocity.Size();
						}
						if (SourcePawn) { FSeek->SetDestination(SourcePawn); }
					}
					if (!ForkCopy->bHurtsOwner) { ForkCopy->SetIgnoredPawn(ReflectPawn); }
					ForkCopy->FinishSpawning(ForkTM);
				}
				// fall through — the original still hits the owner.
			}
			}
		}
	}

	// Reflector NotifyHitByAngrealProjectile dispatch has already run above.
	if (bExplode)
	{
		HitActor = Other;
		HitPrimitive = nullptr;
		CaptureImpactContext(Other, nullptr);
		// Damage stays authority-only (cosmetics stay local): UE1 routed
		// pawn/player damage through the authority-scoped DamageEffect/ProcessEffect
		// pipeline (GenericProjectile.uc:241-262); the port collapsed that to a direct
		// TakeDamage, so the authority gate must be explicit here (PORT_NOTES). The
		// client copy still runs Explode below (teardown + cosmetics), just no damage.
		if (HasAuthority())
		{
			ProcessDamage(Other, Damage, GetInstigator(), HitLocation,
			              MomentumTransfer * ProjectileMovement->Velocity.GetSafeNormal());
		}
		Explode(HitLocation, FVector::UpVector);
		if (Cast<APawn>(Other))
		{
			if (USoundBase* S = WoTSound::Resolve(HitPawnSound))
			{
				WoTSound::PlayOneShot(this, S, HitLocation,
					HitPawnSoundRadius, HitPawnSoundPitch);
			}
		}
	}
}

AWoTGenericProjectile::FWoTProjectileImpactProbe
	AWoTGenericProjectile::OnImpactProbe;

void AWoTGenericProjectile::NotifyHit(UPrimitiveComponent*, AActor* Wall,
	                                  UPrimitiveComponent* WallComponent, bool, FVector HitLocation,
                                      FVector HitNormal, FVector, const FHitResult& Hit)
{
	// UE1 MoveActor sets the zone at the end of the move
	// before physProjectile hands the blocking hit to HitWall. A weave that
	// crosses a shallow water surface and strikes the floor in one move fizzles
	// first (a Fireball is destroyed and never explodes).
	UpdateWaterZone();
	if (!IsValid(this) || IsActorBeingDestroyed())
	{
		return;
	}
	OnImpactProbe.Broadcast(this, Hit);
	// HitWall: store so splash doesn't damage the wall actor twice, explode.
	HitActor = Wall;
	HitPrimitive = WallComponent ? WallComponent : Hit.GetComponent();
	CaptureImpactContext(Wall, HitPrimitive);
	if (HasAuthority())
	{
		// Projectile.HitWall (AngrealProjectile.uc via engine): a damage-triggered
		// mover takes the projectile's direct damage — splash then excludes it as
		// HitActor, so without this a shoot-to-open mover would never be triggered.
		// Mover-damage (+ the original's MakeNoise) stays authority-only.
		if (const AWoTMover* M = Cast<AWoTMover>(Wall))
		{
			if (M->bDamageTriggered)
			{
				// Retail: Wall.TakeDamage(..., instigator, Location, ...) —
				// the projectile's stopped position, not the impact point.
				ProcessDamage(Wall, Damage, GetInstigator(), GetActorLocation(),
				              MomentumTransfer * ProjectileMovement->Velocity.GetSafeNormal());
			}
		}
	}
	// UE1 simulated HitWall runs Explode OUTSIDE the authority gate (only the
	// mover-damage/noise are gated) in Engine.Projectile. So the bNetTemporary
	// client copy self-destructs locally on a wall (root-cause H2 fix): the server
	// Destroy can't propagate over the closed bNetTemporary channel, so teardown
	// must be client-simulated. Explode's damage (splash) is itself authority-gated.
	// POSITION: retail HitWall explodes at `Location + ExploWallOut*HitNormal`
	// where Location is the projectile's STOPPED position (already its
	// CollisionRadius off the wall) and no shipped class authors ExploWallOut.
	// UE5's NotifyHit HitLocation is the wall-surface ImpactPoint — spawning
	// the explosion sprite THERE buries half the billboard in the wall.
	Explode(GetActorLocation(), HitNormal);
}

void AWoTGenericProjectile::ProcessDamage(AActor* Other, float GivenDamage,
                                          APawn* GivenInstigator, FVector HitLocation,
                                          FVector GivenMomentum)
{
	const FName ProjectileDamageType = SourceAngreal
		? AWoTAngrealInventory::GetDamageType(SourceAngreal) : DamageType;
	// ProcessDamage (GenericProjectile.uc:227-261) delivers pawn damage as a
	// DamageEffect routed through ProcessEffect — where the anti-magic dome
	// (NoBadEffects) and the element shields throw deleterious weave effects away.
	// Both the direct hit and the splash loop pass through here, so this single
	// gate is the ProcessEffect-interception fold for weave damage. (Direct
	// TakeDamage sources — zone pain, falling — never come this way.)
	if (AWoTCharacter* Ch = Cast<AWoTCharacter>(Other))
	{
		FWoTDamageEffectSpec Effect;
		Effect.Damage = FMath::TruncToInt(GivenDamage);
		Effect.InstigatorPawn = GivenInstigator;
		Effect.HitLocation = HitLocation;
		Effect.UE1Momentum = GivenMomentum;
		Effect.DamageType = ProjectileDamageType;
		Effect.SourceProjectile = this;
		Effect.SourceProjClass = GetClass();
		Effect.SourceAngreal = SourceAngreal;
		Effect.bDeleterious = true;
		Ch->ProcessEffect(Effect);
		return;
	}

	// Non-character actors do not own the pawn reflector chain; deliver their
	// direct TakeDamage with the same type and authored momentum.
	FWoTDamageEvent Ev;
	Ev.WoTDamageType = ProjectileDamageType;
	Ev.UE1Momentum = GivenMomentum;   // GenericProjectile.uc:200/318
	Ev.HitInfo.ImpactPoint = HitLocation;
	Ev.Damage = GivenDamage;
	Other->TakeDamage(GivenDamage, Ev,
	                  GivenInstigator ? GivenInstigator->GetController() : nullptr,
	                  this);
}

void AWoTGenericProjectile::Explode(FVector HitLocation, FVector HitNormal)
{
	// The impact decal (gore 4e): local per machine — every machine runs Explode.
	if (!ImpactDecalType.IsNone() && GetWorld())
	{
		WoTDecalFX::SpawnAligned(*GetWorld(), ImpactDecalType, HitLocation, HitNormal,
		                         /*bReplicated*/ false, ImpactDecalDrawScale);
	}
	if (!ExplosionEffectName.IsNone() && GetWorld())
	{
		WoTWeaveFX::SpawnExplosion(*GetWorld(), ExplosionEffectName, HitLocation);
	}
	if (USoundBase* S = WoTSound::Resolve(ImpactSound))
	{
		WoTSound::PlayOneShot(this, S, HitLocation,
			ImpactSoundRadius, ImpactSoundPitch);
	}
	if (HasAuthority() && !ImpactSound.IsNull())
	{
		if (AWoTCharacter* ImpactedCharacter = Cast<AWoTCharacter>(HitActor))
		{
			ImpactedCharacter->EmitNoise(1.0f);
		}
		else if (APawn* ImpactedPawn = Cast<APawn>(HitActor))
		{
			ImpactedPawn->MakeNoise(1.0f, ImpactedPawn, HitLocation);
		}
	}
	// Splash damage stays authority-only: a client-simulated detonation
	// is teardown + cosmetics, never damage — so it can't double-apply or race the
	// replicated Health. This also gates the seeker's client-side Destroyed()->
	// Explode() teardown (WoTSeekingProjectile.cpp:249-257), which previously ran
	// splash on the client.
	if (HasAuthority() && DamageRadius > 0.0f)
	{
		ProcessSplashDamage();
	}
	if (bDestroyOnExplode)
	{
		Destroy();
	}
}

void AWoTGenericProjectile::Destroyed()
{
	// Client-side explode-on-destroy for PERSISTENT-channel projectiles (seekers,
	// and the ExpWard which opts out of bNetTemporary): the server's Destroy()
	// propagates over the still-open channel, and the client mirrors the detonation
	// cosmetics locally (splash stays authority-only inside Explode). bNetTemporary
	// weaves are excluded — their channel closes after the initial burst, so they
	// self-destruct via the simulated NotifyHit/ProcessTouch impact path (H2), not
	// channel-close. (Factored up from AWoTSeekingProjectile so the ExpWard's flipped
	// channel shares one hook.)
	if (!HasAuthority() && !bNetTemporary && bExplode)
	{
		Explode(GetActorLocation(), FVector::UpVector);
	}
	Super::Destroyed();
}

void AWoTGenericProjectile::ProcessSplashDamage()
{
	// foreach RadiusActors(DamageRadius): linear falloff, verbatim formula:
	//   CurDamage = Damage - (Damage * dist / DamageRadius)
	// Target filter per original (GenericProjectile.uc:307-312): damage-triggered
	// movers, projectile targets (pawns + bProjTarget actors, e.g. an armed ward —
	// splash chain-detonates it), and actors carrying both retail block flags.
	// RadiusActors enumerates actors independently of their collision state.
	// Port mapping: bProjTarget and the paired bBlockActors/bBlockPlayers flags
	// are exposed by IWoTRetailProjectileTarget; bDamageTriggered stays on movers.
	const FVector Origin = GetActorLocation();
	const float RadiusSq = FMath::Square(DamageRadius);
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* A = *It;
		if (!IsValid(A) || A == HitActor || A == this
			|| FVector::DistSquared(A->GetActorLocation(), Origin) > RadiusSq)
		{
			continue;
		}
		const AWoTMover* M = Cast<AWoTMover>(A);
		// GenericProjectile.uc:307 is an OR: a mover may qualify through
		// bDamageTriggered OR through the ordinary bProjTarget filter.  The old
		// ternary accidentally excluded projectile-target movers that were not
		// damage-triggered (notably ExplodingMover).
		if (!((M && M->bDamageTriggered) || IsTouchTarget(A)))
		{
			continue;
		}
		const float Dist = FVector::Dist(A->GetActorLocation(), Origin);
		const int32 CurDamage = FMath::TruncToInt(
			Damage - (Damage * Dist / DamageRadius));
		if (CurDamage > 0)
		{
			const FVector SplashMomentum =
				MomentumTransfer * static_cast<float>(CurDamage) *
				(A->GetActorLocation() - Origin).GetSafeNormal();
			ProcessDamage(A, static_cast<float>(CurDamage), GetInstigator(), A->GetActorLocation(),
			              SplashMomentum);
		}
	}
}

void AWoTGenericProjectile::UpdateWaterZone()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !IsValid(this) || IsActorBeingDestroyed())
	{
		return;
	}
	AWoTZoneVolume* Now = AWoTZoneVolume::WaterZoneAt(World, GetActorLocation());
	const bool bEnteredWater = Now != nullptr && Now != CurrentWaterZone.Get();
	CurrentWaterZone = Now;
	if (bEnteredWater)
	{
		HitWater();
	}
}

void AWoTGenericProjectile::HitWater()
{
	// GenericProjectile.uc:329-343: each machine spawns its local fizzle and
	// plays the splash. The base does not destroy here; retail subclasses that
	// fizzle out override and tear down their own local copy.
	bHitWater = true;
	if (!HitWaterEffectName.IsNone() && GetWorld())
	{
		const FVector At = GetActorLocation() + HitWaterEffectOffset;
		if (WoTWeaveFX::FindExplosion(HitWaterEffectName))
		{
			WoTWeaveFX::SpawnExplosion(*GetWorld(), HitWaterEffectName, At);
		}
		else
		{
			WoTWeaveFX::SpawnLocal(*GetWorld(), HitWaterEffectName, At,
				FVector::UpVector.Rotation());
		}
	}
	if (USoundBase* S = WoTSound::Resolve(HitWaterSound))
	{
		WoTSound::PlayOneShot(this, S, GetActorLocation(),
			HitWaterSoundRadius, HitWaterSoundPitch);
	}
}

float AWoTGenericProjectile::GetMaxRange(TSubclassOf<AWoTGenericProjectile> Class)
{
	// SeekingProjectile.uc:740 — a seeker chases its target, so its range is
	// effectively infinite (it overrides GetMaxRange to MaxInt).
	if (Class && Class->IsChildOf(AWoTSeekingProjectile::StaticClass()))
	{
		return static_cast<float>(MAX_int32);
	}
	const AWoTGenericProjectile* CDO =
		Class ? Class.GetDefaultObject() : nullptr;
	if (!CDO)
	{
		return 0.0f;
	}
	// PHYS_Projectile (no gravity): Speed * LifeSpan, unbounded when immortal.
	if (CDO->ProjectileMovement->ProjectileGravityScale == 0.0f)
	{
		return (CDO->InitialLifeSpan > 0.0f) ? CDO->Speed * CDO->InitialLifeSpan
		                                     : static_cast<float>(MAX_int32);
	}
	// PHYS_Falling retains Speed ** 4 / (2*950); the source flags the derivation
	// as uncertain, but parity requires the same result (GenericProjectile.uc:414).
	return FMath::Pow(CDO->Speed, 4.0f) / (2.0f * 950.0f);
}

float AWoTGenericProjectile::GetMinRange(TSubclassOf<AWoTGenericProjectile> Class)
{
	const AWoTGenericProjectile* CDO =
		Class ? Class.GetDefaultObject() : nullptr;
	return CDO ? CDO->DamageRadius : 0.0f;
}

FRotator AWoTGenericProjectile::CalculateTrajectory(const AActor* Source,
                                                    const AActor* Destination,
                                                    float ProjectileSpeed) const
{
	if (!Source || !Destination || ProjectileSpeed <= 0.0f)
	{
		return FRotator::ZeroRotator;
	}
	const float Diff = FVector::Dist(Destination->GetActorLocation(),
	                                 Source->GetActorLocation());
	const float Time = Diff / ProjectileSpeed;
	const FVector PredictedLoc = Destination->GetActorLocation()
	                             + Destination->GetVelocity() * Time;
	return (PredictedLoc - Source->GetActorLocation()).Rotation();
}
