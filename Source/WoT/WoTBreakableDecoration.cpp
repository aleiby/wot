#include "WoTBreakableDecoration.h"
#include "WoTRandom.h"

#include "AI/WoTNPCController.h"
#include "Projectiles/WoTProjectile.h"
#include "WoTEvents.h"
#include "WoTZoneVolume.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	// Frag(fragmentClass, Momentum, fragmentSize, 12) — BreakableDecoration.uc:102.
	constexpr int32 BreakableNumFrags = 12;
	// Animate.HitWall (BreakableDecoration.uc:69-71): landing faster than
	// 200uu/s downward self-damages 100 'shattered' with HitNormal*10000.
	constexpr float ShatterVelocityZ = -200.0f;
	constexpr float ShatterDamage = 100.0f;
	// Resting tolerance for the support-loss scan (see ReleaseSupportedDecorations).
	constexpr float SupportGap = 4.0f;
}

AWoTBreakableDecoration::AWoTBreakableDecoration()
{
	bProjTarget = true;
	// The falling mode is authority-integrated in Tick; dormant until knocked
	// loose or dropped by a support loss.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	// BreakableDecoration overrides the inherited Actor cylinder to 29x29.
	ConfigureRuntimeCollision(true, true, true, 29.0f, 29.0f);
	ApplyBlockingCollision();

	// Placed on both sides from the umap like the wall; server-authoritative
	// damage, movement replication carries the knock/fall, and the break
	// replicates as actor teardown (Frag ends in Destroy) — the fragments
	// replicate themselves.
	bReplicates = true;
	SetReplicatingMovement(true);
}

void AWoTBreakableDecoration::ConfigureRuntimeCollision(bool bInCollideActors,
	bool bInBlockActors, bool bInBlockPlayers, float Radius, float HalfHeight)
{
	Super::ConfigureRuntimeCollision(bInCollideActors, bInBlockActors,
		bInBlockPlayers, Radius, HalfHeight);
	ApplyBlockingCollision();
}

void AWoTBreakableDecoration::ApplyBlockingCollision()
{
	// Retail WOTDecoration CDO bBlockActors/bBlockPlayers=True + bProjTarget=True
	// + bCollideWorld=True (BreakableDecoration.uc:121-126): pawns bump into the
	// prop, weave projectiles detonate on it, and it rests on world geometry
	// when falling. WorldDynamic covers stacked breakables and movers.
	if (bCollideActors && Collision->GetCollisionEnabled()
		!= ECollisionEnabled::NoCollision)
	{
		Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
		Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	}
}

void AWoTBreakableDecoration::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// Placed actors reload with the serialized collision-profile template over
	// the constructor's responses (the AWoTSkyBox clobber, PORT_NOTES finding
	// #3) — re-assert the blocking contract at init.
	ApplyBlockingCollision();
}

void AWoTBreakableDecoration::BeginPlay()
{
	Super::BeginPlay();
	if (bStartsFalling && GetNetMode() != NM_Client && HasAuthority())
	{
		StartFalling(FVector::ZeroVector);
	}
}

float AWoTBreakableDecoration::TakeDamage(float DamageAmount,
	const FDamageEvent& DamageEvent, AController* EventInstigator, AActor*)
{
	// Animate.TakeDamage (BreakableDecoration.uc:76-112). The uc:81 bStatic gate
	// is omitted: the CDO ships bStatic=False (uc:122) and no author re-enables
	// it, so placed breakables are never static in the port.
	LastDamageTaken = DamageAmount;
	if (!HasAuthority())
	{
		return 0.0f;
	}
	// uc:86-89 — the already-dead pre-check (the one leg the G35 wall does NOT
	// have): a bOnlyTriggerable prop whose Health has gone negative absorbs
	// further hits as no-ops.
	if (Health <= 0)
	{
		return 0.0f;
	}
	// uc:91-94 — MakeNoise(1.0) on EVERY accepted hit, not just the fatal one.
	// Same G18 seam call shape as the wall: the decoration is the noisemaker
	// (retail CheckNoiseHearing hears the prop, not the shooter).
	if (EventInstigator)
	{
		AWoTNPCController::BroadcastNoise(GetWorld(), this, 1.0f, EventInstigator->GetPawn());
	}

	FVector Momentum = FVector::ZeroVector;
	if (DamageEvent.IsOfType(FWoTDamageEvent::ClassID))
	{
		Momentum = static_cast<const FWoTDamageEvent&>(DamageEvent).UE1Momentum;
	}

	// uc:96 — NO damage-type filter (contrast the wall/mover ActivatedBy gates):
	// every damage name chips Health.
	const int32 AppliedDamage = FMath::TruncToInt(DamageAmount);
	Health -= AppliedDamage;
	if (Health <= 0)
	{
		// uc:97-104 — fatal: shed the authored fragment family.
		Frag(EventInstigator ? EventInstigator->GetPawn() : nullptr, Momentum);
	}
	else
	{
		// uc:105-111 — the non-fatal knock: PHYS_Falling + bBounce with
		// Momentum.Z REPLACED by 1000, then Velocity = Momentum*0.01 — i.e. the
		// scaled momentum XY plus a fixed Z=10 boost. bKnocked is the retail
		// bBounce: ONLY a knocked fall routes floor contact through HitWall
		// (UE1 physFalling); an unknocked fall Lands damage-free.
		bKnocked = true;
		Momentum.Z = 1000.0f;
		StartFalling(Momentum * 0.01f);
	}
	return static_cast<float>(AppliedDamage);
}

void AWoTBreakableDecoration::Frag(APawn* EventInstigator, const FVector& Momentum)
{
	// Decoration.Frag, authority-only, one-shot.
	if (!HasAuthority() || bFragged)
	{
		return;
	}
	// uc:199-200 — bOnlyTriggerable aborts BEFORE anything happens: no event,
	// no debris, no destroy (damage still drained Health in TakeDamage).
	if (bOnlyTriggerable)
	{
		return;
	}
	bFragged = true;

	// uc:201-203 — Event fan-out before the debris spawns.
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, this, EventInstigator);
	// uc:204-208 Region.Zone.bDestructive insta-Destroy leg NOTE-SKIPPED: the
	// port's zone model has no bDestructive (same citation-skip as G29/G35).

	// uc:209-214 — exactly NumFrags fragments AT the actor location (no VRand
	// spread — contrast the wall's ExplosionDimensions scatter), Owner passed
	// through, CalcVelocity(Momentum, 0) == Init ExplosionSize 0, per-fragment
	// randomized DrawScale (contrast the wall/mover's uniform ParticleSize),
	// and NO skin — Frag (not skinnedFrag) keeps the family's baked look.
	FActorSpawnParameters Params;
	Params.Owner = GetOwner();
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Location = GetActorLocation();
	for (int32 Index = 0; Index < BreakableNumFrags; ++Index)
	{
		if (AWoTFragment* Fragment = GetWorld()->SpawnActor<AWoTFragment>(
			AWoTFragment::StaticClass(), Location, FRotator::ZeroRotator, Params))
		{
			Fragment->Init(FragmentType, /*ExplosionSize*/ 0.0f, Momentum,
				FragmentSize * 0.5f + 0.7f * FragmentSize * WoTRandom::FRand(),
				NAME_None);
		}
	}

	ReleaseSupportedDecorations();
	// uc:216 — the prop is GONE; replicated teardown removes remote proxies.
	Destroy();
}

void AWoTBreakableDecoration::ReleaseSupportedDecorations()
{
	// Port fold of the retail Base/BaseChange support chain
	// (BreakableDecoration.uc:17-63): the engine cleared a stacked prop's Base
	// when the prop under it died, and the base==None + bPushable leg (uc:28-31)
	// dropped it into PHYS_Falling — Arena_06's two barrel piles collapse this
	// way. The port has no decoration base bookkeeping, so the dying prop scans
	// for others resting on top of it (XY inside combined radii, bottom within
	// ~4uu of this top) and starts them falling. The carried/pawn-base and
	// decoration-base damage-math legs (uc:32-60) are SP furniture mechanics —
	// NOTE-SKIPPED (PORT_NOTES G36).
	const FVector MyLocation = GetActorLocation();
	const float MyTop = MyLocation.Z
		+ Collision->GetUnscaledCapsuleHalfHeight() * Collision->GetComponentScale().Z;
	const float MyRadius = Collision->GetUnscaledCapsuleRadius()
		* FMath::Max(Collision->GetComponentScale().X, Collision->GetComponentScale().Y);
	for (TActorIterator<AWoTBreakableDecoration> It(GetWorld()); It; ++It)
	{
		AWoTBreakableDecoration* Other = *It;
		if (Other == this || !IsValid(Other) || Other->IsFallingLoose())
		{
			continue;
		}
		const FVector OtherLocation = Other->GetActorLocation();
		const float CombinedRadii =
			MyRadius + Other->Collision->GetUnscaledCapsuleRadius()
				* FMath::Max(Other->Collision->GetComponentScale().X,
					Other->Collision->GetComponentScale().Y);
		if ((OtherLocation - MyLocation).SizeSquared2D() >
			FMath::Square(CombinedRadii))
		{
			continue;
		}
		const float OtherBottom =
			OtherLocation.Z - Other->Collision->GetUnscaledCapsuleHalfHeight()
				* Other->Collision->GetComponentScale().Z;
		if (FMath::Abs(OtherBottom - MyTop) <= SupportGap)
		{
			Other->StartFalling(FVector::ZeroVector);
		}
	}
}

void AWoTBreakableDecoration::StartFalling(const FVector& InVelocity)
{
	if (GetNetMode() == NM_Client || !HasAuthority())
	{
		return;
	}
	// A repeated knock overwrites the velocity, matching retail's re-assignment
	// (uc:107-110 runs on every non-fatal hit).
	FallVelocity = InVelocity;
	bFallingLoose = true;
	SetActorTickEnabled(true);
}

void AWoTBreakableDecoration::NotifyHit(UPrimitiveComponent* MyComp,
	AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved,
	FVector HitLocation, FVector HitNormal, FVector NormalImpulse,
	const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation,
		HitNormal, NormalImpulse, Hit);
	APawn* Pusher = Cast<APawn>(Other);
	if (!HasAuthority() || !bPushable || !Pusher)
	{
		return;
	}
	float PusherMass = 0.0f;
	if (const ACharacter* Character = Cast<ACharacter>(Pusher))
	{
		if (const UCharacterMovementComponent* Move =
			Character->GetCharacterMovement())
		{
			PusherMass = Move->Mass;
		}
	}
	if (PusherMass <= 40.0f)
	{
		return;
	}
	const FVector PusherVelocity = Pusher->GetVelocity();
	const float Speed = PusherVelocity.Size();
	if (Speed <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	const float OldZ = FallVelocity.Z;
	FVector PushVelocity = PusherVelocity
		* (FMath::Min(120.0f, 20.0f + Speed) / Speed);
	PushVelocity.Z = bFallingLoose ? OldZ : 25.0f;
	SetInstigator(Pusher);
	StartFalling(PushVelocity);
}

void AWoTBreakableDecoration::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// The falling mode: authority manual gravity + swept root move, the
	// AWoTFragment::Tick pattern.
	if (!HasAuthority() || !bFallingLoose)
	{
		return;
	}
	FallVelocity += AWoTZoneVolume::GetGravityAt(
		GetWorld(), GetActorLocation()) * DeltaSeconds;
	FHitResult Hit;
	AddActorWorldOffset(FallVelocity * DeltaSeconds, true, &Hit);
	if (!Hit.IsValidBlockingHit())
	{
		return;
	}
	if (Cast<AWoTFragment>(Hit.GetActor()))
	{
		// Debris never carries a prop: retail fragments are bCollideActors=false
		// toward decorations. Ignore the chunk and keep falling.
		Collision->IgnoreActorWhenMoving(Hit.GetActor(), true);
		return;
	}
	// UE1 physFalling dispatch (review F1/F2 —
	// parent-verified):
	//  - WALL contact (Normal.Z <= 0.7): processHitWall for EVERY fall ->
	//    Animate.HitWall runs the Vz < -200 shatter gate (uc:69-71), then
	//    clears bBounce and zeroes Velocity; PHYS_Falling re-takes.
	//  - FLOOR contact: eventHitWall (the shatter gate) ONLY when bBounce is
	//    set — i.e. only after the non-fatal knock. An unknocked fall (the
	//    stack-collapse support loss) goes processLanded -> Decoration.Landed,
	//    which deals NO damage; the only unknocked landing damage is the
	//    BaseChange crush gate at Vz <= -MinDamageVelocityZ (uc:23-27,
	//    damage 1 - Vz/30 'crushed' — Base is set during the landing while the
	//    impact velocity is still live). Arena_06's pile tops fall ~43uu
	//    (~-290uu/s): in retail they land INTACT.
	const bool bFloor = Hit.ImpactNormal.Z > 0.7f;
	const float ImpactVz = FallVelocity.Z;
	if (!bFloor || bKnocked)
	{
		if (ImpactVz < ShatterVelocityZ)
		{
			FWoTDamageEvent Shatter;
			Shatter.WoTDamageType = FName(TEXT("shattered"));
			Shatter.UE1Momentum = Hit.ImpactNormal * 10000.0f;
			TakeDamage(ShatterDamage, Shatter, nullptr, this);
		}
	}
	else if (ImpactVz <= -static_cast<float>(MinDamageVelocityZ))
	{
		FWoTDamageEvent Crush;
		Crush.WoTDamageType = FName(TEXT("crushed"));
		TakeDamage(1.0f - ImpactVz / 30.0f, Crush, nullptr, this);
	}
	if (!IsValid(this))
	{
		return;
	}
	// uc:72-74 — HitWall clears bBounce and zeroes Velocity on ANY contact; a
	// wall graze keeps falling (gravity restarts next tick), a floor contact
	// settles (retail's next floor touch Lands into PHYS_None).
	bKnocked = false;
	FallVelocity = FVector::ZeroVector;
	if (bFloor)
	{
		bFallingLoose = false;
		SetActorTickEnabled(false);
	}
}
