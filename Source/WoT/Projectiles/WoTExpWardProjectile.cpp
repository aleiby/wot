#include "WoTExpWardProjectile.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTSound.h"
#include "Components/PointLightComponent.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"

#include "../WoTCollisionBlocker.h"
#include "../WoTMover.h"
#include "../WoTMeshLight.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "Components/SphereComponent.h"
#include "Engine/Brush.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsEngine/BodyInstance.h"
#include "TimerManager.h"

namespace
{
	// In the collision hash: collision on (bCollideActors) and a collision shape
	// to touch with. A primitive with no body (a debug-draw root, a mesh
	// component without a mesh) never overlaps anything, whatever its bounds.
	bool WardContactHasCollision(const UPrimitiveComponent* Primitive)
	{
		if (!Primitive || !Primitive->IsQueryCollisionEnabled())
		{
			return false;
		}
		const FBodyInstance* Body = Primitive->GetBodyInstance();
		return Body && Body->IsValidBodyInstance();
	}

	// The cylinder Other presents to Touch while it collides. An actor whose
	// collision is off reports no cylinder, so take its root primitive's own:
	// the one that comes back with its collision.
	void WardContactCylinder(const AActor& Other, float& OutRadius, float& OutHalfHeight)
	{
		Other.GetSimpleCollisionCylinder(OutRadius, OutHalfHeight);
		if (OutRadius <= 0.0f && OutHalfHeight <= 0.0f)
		{
			if (const UPrimitiveComponent* Root =
				Cast<UPrimitiveComponent>(Other.GetRootComponent()))
			{
				Root->CalcBoundingCylinder(OutRadius, OutHalfHeight);
			}
		}
	}
}

AWoTExpWardProjectile::AWoTExpWardProjectile()
{
	ExplosionEffectName = FName(TEXT("FireballExplode3"));   // AngrealExpWardProjectile.uc:145
	PrimaryActorTick.bCanEverTick = true;
	// AngrealExpWardProjectile.uc:264,281-285 display defaults.
	ScaleGlow = 1.0f;
	ScaleGlowRate = 1.0f;
	bUnlit = true;
	bStyleTranslucent = true;
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	// Explode scorch (AngrealExpWardProjectile.uc:137-141: ScorchType
	// ScorchMark at DrawScale = ScorchSize 2.5, :268-269).
	ImpactDecalType = FName(TEXT("ScorchMark"));
	ImpactDecalDrawScale = 2.5f;
	// AngrealExpWardProjectile defaultproperties (simulation subset).
	Damage = 50.0f;
	DamageRadius = 120.0f;              // splash on detonation
	MomentumTransfer = 6000;
	DamageType = "xEFxx";              // Earth + Fire
	bHurtsOwner = true;               // the ward can catch its placer
	// AI support (uc:272): the ward is aimed straight at a target, not led.
	bRequiresLeading = false;
	TraceDist = 200.0f;
	ActivationTime = 1.0f;
	InitialLifeSpan = 60.0f;          // MP auto-detonate; standalone -> permanent

	// Divergence from the raw bNetTemporary default (PORT_NOTES "Explosive Ward uses a persistent channel"): a stationary,
	// long-lived, ≤16-on-the-field ward rides a PERSISTENT channel so a server-only
	// early detonation (proximity/shot/Trigger before its 60s) propagates its Destroy
	// to clients instead of leaving a ≤60s lingering ghost. It then inherits the base
	// AWoTGenericProjectile::Destroyed() client explode-on-destroy cosmetics, which
	// Explode ignores while the client's copy is still waiting.
	bNetTemporary = false;

	// AngrealExpWardProjectile.uc: CollisionRadius=24, CollisionHeight=8.
	SetUE1CollisionSize(24.0f, 8.0f);
	// bProjTarget=True (uc:290): shootable — a dart hit or splash detonates the
	// ward. The Waiting state's Touch/ProcessTouch/TakeDamage/Explode ignore
	// (uc:181) maps to disabled collision until armed + the bArmed gates below.
	bProjTarget = true;
	// bCollideWorld=False and no block flags among the collision defaults
	// (uc:286-290): the cylinder blocks nothing, so no world contact can reach
	// the generic wall-impact detonation. It overlaps every object channel;
	// projectiles still strike it and each actor that meets it raises the Touch
	// that IsEligibleContact filters.
	CylinderCollision->SetCollisionResponseToAllChannels(ECR_Overlap);
	CylinderCollision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	CylinderCollision->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	// bCollideActors=False (uc:288): no collision until the ward arms, so it
	// touches nothing while it is spawned and placed (PreBeginPlay switches
	// collision off before its SetLocation, uc:72-75) or while it waits.
	SetActorEnableCollision(false);

	// Physics=PHYS_None (uc:279): the ward never moves itself; it only rides a
	// mover it is attached to.
	ProjectileMovement->bAutoActivate = false;
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	ProjectileMovement->InitialSpeed = 0.0f;
	ProjectileMovement->MaxSpeed = 0.0f;

	// Sound'Angreal.LandMine.*' — bound by the sound pass (soft, safe if absent).
	SpawnSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/PlaceLM.PlaceLM"));
	ImpactSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ExplodeLM.ExplodeLM"));
	ActivateSound = FSoftObjectPath(TEXT("/Game/WoT/Sounds/Angreal/ActivateLM.ActivateLM"));
}

void AWoTExpWardProjectile::BeginPlay()
{
	// The base BeginPlay's ServerInit waits for the placement below.
	Super::BeginPlay();
	FullLightIntensity = WoTUE1LightRegistry::Configure(
		Light, TEXT("AngrealExpWardProjectile")) ? Light->Intensity : 0.0f;
	BindWeaveMesh(DisplayMesh, FName(TEXT("ExpWard")), 0.3f);
	ApplyVisualPresentation();

	if (HasAuthority())
	{
		// Trace forward to the nearest surface and stick to it (PreBeginPlay).
		// Retail traces ALL world geometry, not just static BSP
		// (AngrealExpWardProjectile.uc:68), so include movable world actors.
		// Collision stays off through the placement (SetCollision(False),
		// uc:72; off since construction), so its moves touch nothing.
		SetActorEnableCollision(false);
		const FVector Start = GetActorLocation();
		const FVector End = Start + GetActorForwardVector() * TraceDist;
		FHitResult Hit;
		// AngrealExpWardProjectile.uc:68 Trace(..., false): only level/movers
		// place the ward. The accepted hit supplies retail location and normal.
		if (WoTVisBlockingTrace(GetWorld(), Start, End, this, Hit))
		{
			RelativeUp = Hit.ImpactNormal;
			AlignPresentationToVelocity(RelativeUp);
			bCorrectlyPlaced = SetActorLocation(Hit.ImpactPoint + RelativeUp * 0.5f);
			if (bCorrectlyPlaced)
			{
				// Only a successful placement runs GenericProjectile.PreBeginPlay,
				// whose ServerInit plays the placement sound from the placed
				// ward (uc:78-81). A placement that fails is silent (uc:88-96).
				Super::ServerInit();
				// "Stick to movers": SetBase(HitActor) (uc:83-85) so the ward rides
				// a moving platform/door instead of hanging in the air.
				if (AWoTMover* M = Cast<AWoTMover>(Hit.GetActor()))
				{
					AttachToActor(M, FAttachmentTransformRules::KeepWorldTransform);
				}
			}
			// Retail SetLocation -> SetActorZone -> ZoneChange (UE1's FarMoveActor
			// and SetActorZone): a ward placed on a surface inside a water zone hits water
			// and is destroyed (AngrealExpWardProjectile.uc:165-169) during spawn,
			// so SpawnActor returns None and the cast fails without using a charge
			// (LaunchProjectileEffect.uc:133-141, ProjectileLauncher.uc:216-226).
			// The SetLocation itself succeeded, so the placement sound above
			// has played (uc:75-81); there is no fizzle: the ward authors no
			// HitWater class or sound.
			UpdateWaterZone();
			if (!IsValid(this) || IsActorBeingDestroyed())
			{
				return;
			}
		}
	}

	// Explosive wards are permanent in SP; timed in MP (LifeSpan default 60).
	if (GetNetMode() == NM_Standalone)
	{
		SetLifeSpan(0.0f);
	}

	// Register our Tag so a matching Trigger/Dispatcher can detonate us (retail
	// Trigger). Player-thrown wards have no Tag, so this is a no-op for them.
	if (HasAuthority() && !TagName.IsNone())
	{
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->RegisterTag(TagName, this);
		}
	}

	// Waiting: arm after ActivationTime (Sleep(ActivationTime) -> active). A
	// client's copy whose first update already carried the server's arming is
	// armed (OnRep_VisualPhase).
	if (!bActivated)
	{
		GetWorldTimerManager().SetTimer(ArmTimer, this,
			&AWoTExpWardProjectile::Activate, ActivationTime, false);
	}
}

void AWoTExpWardProjectile::WoTTrigger(AActor* /*Other*/, APawn* /*EventInstigator*/)
{
	// AngrealExpWardProjectile.uc:125 (singular Trigger): detonate in place.
	// While waiting, Explode ignores it (uc:181).
	if (!HasAuthority())
	{
		return;
	}
	Explode(GetActorLocation(), RelativeUp);
}

bool AWoTExpWardProjectile::IsPotentialContact(const AActor* Other) const
{
	if (!IsValid(Other) || Other == this || Other->IsActorBeingDestroyed())
	{
		return false;
	}
	// Level geometry is not an actor in the original's collision hash and never
	// raises Touch. The port's level is the runtime BSP world or an imported BSP
	// StaticMeshActor (the same split as WoTRainCydIsVisBlockingActor), plus any
	// engine BSP brush.
	if (Other->IsA<AWoTRuntimeBSPWorld>() || Other->IsA<AStaticMeshActor>()
		|| Other->IsA<ABrush>())
	{
		return false;
	}
	// The original's Touch never pairs an actor with the actor it is based on,
	// either way round: the mover the ward is stuck to (SetBase, uc:83-85)
	// turning or swinging under it is no contact.
	if (IsBasedOnActor(Other) || Other->IsBasedOnActor(this))
	{
		return false;
	}
	// The same owner rule as the overlap path: a ward hurts its owner, so the
	// caster is an ordinary contact (IgnoredPawn stays the exclusion).
	if (Other == GetOwner() && SkipsOwnerTouch())
	{
		return false;
	}
	// AngrealProjectile.uc:84: bProjTarget, or both bBlockActors and bBlockPlayers.
	return IsTouchTarget(Other);
}

bool AWoTExpWardProjectile::IsEligibleContact(const AActor* Other) const
{
	if (!IsPotentialContact(Other))
	{
		return false;
	}
	// Touch passes only between colliding actors: an actor whose collision is
	// off (bCollideActors=False) never touches. Its collision primitive is the
	// root wherever the root is one.
	if (const UPrimitiveComponent* Root =
		Cast<UPrimitiveComponent>(Other->GetRootComponent()))
	{
		return WardContactHasCollision(Root);
	}
	TInlineComponentArray<UPrimitiveComponent*> Primitives(Other);
	return Primitives.ContainsByPredicate(&WardContactHasCollision);
}

bool AWoTExpWardProjectile::MeetsCylinder(const AActor* Other) const
{
	// The ward's 24x8 cylinder against Other's, as WoTUE1Collision::
	// CylindersOverlap measures them, but sized as Other is while it collides.
	if (!IsValid(Other))
	{
		return false;
	}
	float OtherRadius = 0.0f;
	float OtherHalfHeight = 0.0f;
	WardContactCylinder(*Other, OtherRadius, OtherHalfHeight);
	const FVector Delta = Other->GetActorLocation() - GetActorLocation();
	return FVector2D(Delta.X, Delta.Y).SizeSquared()
			<= FMath::Square(UE1CollisionRadius + FMath::Max(0.0f, OtherRadius))
		&& FMath::Abs(Delta.Z) <= UE1CollisionHeight + FMath::Max(0.0f, OtherHalfHeight);
}

bool AWoTExpWardProjectile::CreationSucceeded()
{
	return bCorrectlyPlaced;
}

void AWoTExpWardProjectile::Activate()
{
	if (bActivated)
	{
		return;
	}
	bActivated = true;
	bArmed = true;
	GetWorldTimerManager().ClearTimer(ArmTimer);
	if (HasAuthority())
	{
		ArmingOccupants.Reset();
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			AActor* Other = *It;
			// Every actor inside that could raise Touch, colliding now or not:
			// one whose collision returns later is still inside without having
			// moved, so it too touches only by moving. Level geometry is never
			// a toucher.
			if (IsPotentialContact(Other) && MeetsCylinder(Other))
			{
				// Touch is generated by relative movement. Remember the separation,
				// not only the occupant's world position, so a mover carrying the ward
				// into a stationary pawn triggers exactly like retail.
				ArmingOccupants.Add(Other,
					Other->GetActorLocation() - GetActorLocation());
			}
		}
	}
	{
		// SetCollision(True) (uc:186) raises no Touch: overlaps that begin
		// while the collision switches on are occupants, not touches, here and
		// in every armed ward this one arms inside (ProcessTouch).
		TGuardValue<bool> SwitchingOn(bSwitchingCollisionOn, true);
		SetActorEnableCollision(true);   // proximity triggering goes live
	}
	if (!IsValid(this) || IsActorBeingDestroyed())
	{
		return;
	}
	if (VisualPhase == EWoTExpWardVisualPhase::Waiting)
	{
		SetVisualPhase(EWoTExpWardVisualPhase::FadeOut);
	}
	PlayActivateSoundOnce();
}

void AWoTExpWardProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bArmed && !ArmingOccupants.IsEmpty())
	{
		TArray<AActor*, TInlineAllocator<4>> Touchers;
		for (auto It = ArmingOccupants.CreateIterator(); It; ++It)
		{
			AActor* Other = It->Key.Get();
			// Inside by the cylinder Other has while it collides, so an occupant
			// whose collision is off keeps its place.
			if (!IsValid(Other) || !MeetsCylinder(Other))
			{
				It.RemoveCurrent();
				continue;
			}
			const FVector Separation = Other->GetActorLocation() - GetActorLocation();
			if (Separation.Equals(It->Value, KINDA_SMALL_NUMBER))
			{
				continue;
			}
			if (!IsEligibleContact(Other))
			{
				// Rechecked at the move: an actor without collision raises no
				// Touch now. It stays deferred from where it is, so its next move
				// once it collides (again) touches.
				It->Value = Separation;
				continue;
			}
			It.RemoveCurrent();
			Touchers.Add(Other);
		}
		// Occupants exist only on the server (Activate, ProcessTouch), so this
		// dispatch is the server's.
		for (AActor* Other : Touchers)
		{
			if (IsValid(Other))
			{
				Super::ProcessTouch(Other, Other->GetActorLocation());
				if (!IsValid(this) || IsActorBeingDestroyed()) { return; }
			}
		}
	}
	if (!bArmed || VisualPhase == EWoTExpWardVisualPhase::Waiting)
	{
		return;
	}

	if (VisualPhase == EWoTExpWardVisualPhase::FadeOut)
	{
		ScaleGlow = FMath::Max(0.0f, ScaleGlow - ScaleGlowRate * DeltaSeconds);
		if (ScaleGlow <= 0.0f)
		{
			SetVisualPhase(EWoTExpWardVisualPhase::FadeIn);
		}
	}
	else
	{
		ScaleGlow = FMath::Min(1.0f, ScaleGlow + ScaleGlowRate * DeltaSeconds);
		if (ScaleGlow >= 1.0f)
		{
			SetVisualPhase(EWoTExpWardVisualPhase::FadeOut);
		}
	}
	ApplyVisualPresentation();
}

void AWoTExpWardProjectile::SetVisualPhase(EWoTExpWardVisualPhase NewPhase)
{
	if (VisualPhase == NewPhase)
	{
		return;
	}
	VisualPhase = NewPhase;
	if (HasAuthority())
	{
		ForceNetUpdate();
	}
	ApplyVisualPresentation();
}

void AWoTExpWardProjectile::OnRep_VisualPhase()
{
	if (VisualPhase != EWoTExpWardVisualPhase::Waiting)
	{
		// The server armed: this copy arms too if its own timer has not run
		// yet (collision on, so other projectiles' local copies meet it).
		Activate();
	}
	ApplyVisualPresentation();
}

void AWoTExpWardProjectile::PlayActivateSoundOnce()
{
	if (bActivateSoundPlayed || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	bActivateSoundPlayed = true;
	// AngrealExpWardProjectile.uc:185: PlaySound in the simulated Waiting state
	// code — each machine's own play, volume 1, the engine's default radius.
	WoTSound::PlaySimulatedFrom(*this, WoTSound::Resolve(ActivateSound));
}

void AWoTExpWardProjectile::ApplyVisualPresentation()
{
	// At the FadeOut trough the translucent source is exactly black; hiding the
	// actor avoids leaving an opaque/depth-writing shell for that armed instant.
	// FadeIn unhides it on the next positive ScaleGlow step. A blown client copy
	// stays hidden until the server's teardown removes it.
	SetActorHiddenInGame(bDetonated || (bArmed && ScaleGlow <= KINDA_SMALL_NUMBER));
	if (IsValid(Light))
	{
		const float Glow = VisualPhase == EWoTExpWardVisualPhase::Waiting
			? 0.0f : FMath::Clamp(ScaleGlow, 0.0f, 1.0f);
		Light->SetIntensity(FullLightIntensity * Glow);
	}
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		// bUnlit makes this a cheap flat-color refresh: MLAmbient is
		// clamp(ScaleGlow*0.5), exactly the translucent-master source multiplier.
		MeshLight->ForceRuntimeBake();
	}
}

void AWoTExpWardProjectile::ProcessTouch(AActor* Other, FVector HitLocation)
{
	// The server decides every detonation; a client's copy shows the ones it
	// announces (MulticastDetonation) and never detonates on its own contact.
	if (!HasAuthority())
	{
		return;
	}
	// Waiting state ignores Touch and ProcessTouch (uc:181); only a live ward
	// detonates, and only for an actor that could raise Touch.
	if (!bArmed || !IsEligibleContact(Other))
	{
		return;
	}
	if (!WoTUE1Collision::CylindersOverlap(GetActorLocation(),
		UE1CollisionRadius, UE1CollisionHeight, Other))
	{
		return;
	}
	if (ArmingOccupants.Contains(Other))
	{
		return;
	}
	// An overlap that begins while either ward is switching its collision on
	// comes from no movement: SetCollision(True) raises no Touch (uc:186). The
	// other actor is inside without touching, an occupant like those found
	// when this ward armed, so only a later relative move touches.
	const AWoTExpWardProjectile* OtherWard = Cast<AWoTExpWardProjectile>(Other);
	if (bSwitchingCollisionOn || (OtherWard && OtherWard->bSwitchingCollisionOn))
	{
		ArmingOccupants.Add(Other, Other->GetActorLocation() - GetActorLocation());
		return;
	}
	Super::ProcessTouch(Other, HitLocation);
}

void AWoTExpWardProjectile::NotifyHit(UPrimitiveComponent* /*MyComp*/,
	AActor* /*Other*/, UPrimitiveComponent* /*OtherComp*/, bool /*bSelfMoved*/,
	FVector /*HitLocation*/, FVector /*HitNormal*/, FVector /*NormalImpulse*/,
	const FHitResult& /*Hit*/)
{
	// PHYS_None with bCollideWorld=False (uc:279,289): the original never calls
	// HitWall on a ward. Its cylinder blocks no channel, and a blocking contact
	// reported anyway is still not a wall impact.
}

void AWoTExpWardProjectile::Explode(FVector /*HitLocation*/, FVector /*HitNormal*/)
{
	// Waiting ignores Explode (uc:179-181): until the ward arms, no caller (a
	// trigger, a shot, a contact or an expiry) detonates it. The server alone
	// detonates; a client's copy shows only the detonation the server
	// announced, whatever its own arming state, and nothing for a removal
	// that was no detonation.
	if (HasAuthority() ? !bArmed : !bServerDetonated)
	{
		return;
	}
	// One detonation per ward: retail's TakeDamage/Trigger are `singular`
	// (uc:119,125), which is what stops two armed wards in mutual splash radius
	// from re-detonating each other in unbounded recursion (A -> B -> A -> ...).
	if (bDetonated)
	{
		return;
	}
	bDetonated = true;
	if (HasAuthority())
	{
		// Reliable, on the ward's own channel: it reaches each client before
		// the channel's teardown that follows from the Destroy below.
		MulticastDetonation();
	}
	// Every effect is at the ward's own Location, aligned to its surface
	// normal RelativeUp, whatever touched or hit it (uc:135-146): the
	// BlastBits here, the ScorchMark (Align(RelativeUp)), the FireballExplode3
	// and the impact sound in the base Explode.
	const FVector At = GetActorLocation();
	if (GetWorld())
	{
		WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("BlastBits"), At,
			RelativeUp.Rotation());
	}
	// TriggerOthers (AngrealExpWardProjectile.uc:154): fire our Event so any actor
	// tagged with it (e.g. a chained ward) is triggered. Empty for player-thrown wards.
	if (HasAuthority() && !EventName.IsNone())
	{
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->FireEvent(EventName, this, nullptr);
		}
	}
	Super::Explode(At, RelativeUp);   // splash (DamageRadius) + destroy
	if (!HasAuthority() && IsValid(this) && !IsActorBeingDestroyed())
	{
		// A client cannot destroy its copy of the persistent channel; the
		// server's teardown follows. Until then the blown ward shows nothing.
		SetActorEnableCollision(false);
		SetActorHiddenInGame(true);
	}
}

void AWoTExpWardProjectile::MulticastDetonation_Implementation()
{
	if (HasAuthority())
	{
		return;   // the server's own Explode is running this
	}
	bServerDetonated = true;
	Explode(GetActorLocation(), RelativeUp);
}

void AWoTExpWardProjectile::LifeSpanExpired()
{
	// Expired() -> Explode() (MP auto-detonate, uc:172-176), after which the
	// lifespan removes the actor. While waiting the Explode is ignored, so the
	// expiry is only that removal.
	Explode(GetActorLocation(), RelativeUp);
	if (IsValid(this) && !IsActorBeingDestroyed())
	{
		Super::LifeSpanExpired();
	}
}

float AWoTExpWardProjectile::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                        AController* EventInstigator, AActor* DamageCauser)
{
	const float Actual = Super::TakeDamage(DamageAmount, DamageEvent,
		EventInstigator, DamageCauser);
	// A live ward detonates when shot (uc:119-122; Waiting ignores TakeDamage).
	if (bArmed)
	{
		Explode(GetActorLocation(), RelativeUp);
	}
	return Actual;
}

FRotator AWoTExpWardProjectile::CalculateTrajectory(const AActor* Source,
	const AActor* Destination, float /*ProjectileSpeed*/) const
{
	// AngrealExpWardProjectile.uc:246-253: the unit direction to the destination
	// with its z replaced by -0.5, aiming the placement at the ground. The
	// launcher's AI aim does not use it (bRequiresLeading=False).
	if (!Source || !Destination)
	{
		return FRotator::ZeroRotator;
	}
	FVector Direction = (Destination->GetActorLocation()
		- Source->GetActorLocation()).GetSafeNormal();
	Direction.Z = -0.5f;
	return Direction.Rotation();
}

void AWoTExpWardProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Original: reliable if Role==Authority RelativeUp.
	DOREPLIFETIME(AWoTExpWardProjectile, RelativeUp);
	DOREPLIFETIME(AWoTExpWardProjectile, bArmed);
	DOREPLIFETIME(AWoTExpWardProjectile, VisualPhase);
}
