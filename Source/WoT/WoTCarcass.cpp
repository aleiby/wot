#include "WoTCarcass.h"
#include "WoTRandom.h"
#include "WoTCarcassZoneBehavior.h"
#include "WoTPreferencesSubsystem.h"

#include "WoTAmbientSound.h"
#include "WoTAnimTable.h"
#include "WoTBloodFX.h"
#include "WoTSound.h"
#include "WoTSoundTable.h"
#include "WoTCharacter.h"
#include "WoTCreatureChunk.h"
#include "WoTPlayerController.h"
#include "WoTVertexAnimComponent.h"
#include "WoTZoneVolume.h"
#include "Projectiles/WoTProjectile.h"
#include "AI/WoTSight.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

// The imported-mesh layout (PORT_NOTES "Mesh binding pass"): the same deterministic
// path ResolveBodyMesh walks for creature rows, keyed by the bare asset name.
USkeletalMesh* AWoTCarcass::ResolveMeshByName(FName MeshName)
{
#if WOT_GUARDED_BOOTSTRAP
	// Runtime display binding is name-based through UWoTVertexAnimComponent.
	// This pointer helper exists only for development automation fixtures.
	return nullptr;
#else
	if (MeshName.IsNone())
	{
		return nullptr;
	}
	const FString Name = MeshName.ToString();
	const FSoftObjectPath Path(FString::Printf(
		TEXT("/Game/WoT/Meshes/%s/%s/SkeletalMeshes/%s.%s"), *Name, *Name, *Name, *Name));
	return Cast<USkeletalMesh>(Path.TryLoad());
#endif
}

AWoTCarcass::AWoTCarcass()
{
	PrimaryActorTick.bCanEverTick = true;

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	SetRootComponent(Capsule);
	// The falling corpse collides with the world only; pawns/projectiles overlap it
	// (shipped carcasses never block play — Landed drops blocking entirely).
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	Capsule->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	// bProjTarget=true (WOTCarcass.uc:150): weaves TOUCH the corpse (never blocked
	// by it) so the shot-corpse spray + gib path is reachable — the review-caught
	// fix; ECC_GameTraceChannel1 is the WoTProjectile object channel.
	Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);

	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("CarcassMesh"));
	Mesh->SetupAttachment(Capsule);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// WOTCarcass defaults: NetPriority 6; the actor + its one-shot display payload
	// replicate, movement rides the standard replicated-movement snapshots.
	bReplicates = true;
	SetReplicatingMovement(true);
	NetPriority = 6.0f;
}

void AWoTCarcass::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTCarcass, MeshName);
	DOREPLIFETIME(AWoTCarcass, PoseSequence);
	DOREPLIFETIME(AWoTCarcass, PoseFraction);
	DOREPLIFETIME(AWoTCarcass, MeshScale);
	DOREPLIFETIME(AWoTCarcass, MeshFatness);
	DOREPLIFETIME(AWoTCarcass, MeshActorSkin);
	DOREPLIFETIME(AWoTCarcass, MeshSkinIndices);
	DOREPLIFETIME(AWoTCarcass, MeshSkinPaths);
	DOREPLIFETIME(AWoTCarcass, MeshRelativeOffset);
	DOREPLIFETIME(AWoTCarcass, AmbientHumPath);
	DOREPLIFETIME(AWoTCarcass, bSinking);
}

void AWoTCarcass::OnRep_MeshOffset()
{
	Mesh->SetRelativeLocation(MeshRelativeOffset);
}

void AWoTCarcass::InitFor(AWoTCharacter& Dead)
{
	if (!HasAuthority())
	{
		return;
	}
	// WOTCarcass.uc:114 explicitly performs `Tag = Other.Tag`. UE5's Actor.Tag
	// seam is the Tags array, so carry the marker through the real death path for
	// Balefire's tagged-carcass sweep (and preserve any non-Balefire tag identity).
	Tags = Dead.Tags;

	// CopyDisplaySettings (Legend/Util.uc:122-147) as a replicated payload: the bound
	// mesh asset, scale, and the frozen death pose (sequence + frame fraction).
	if (const UWoTVertexAnimComponent* Body =
		Cast<UWoTVertexAnimComponent>(Dead.GetMesh()))
	{
		if (Body->HasDisplayMesh())
		{
			MeshName = Body->GetDisplayMeshName();
		}
		MeshScale = Body->GetRelativeScale3D().X;
		MeshFatness = Body->Fatness;
		MeshActorSkin = Body->GetRuntimeActorSkin();
		MeshSkinIndices.Reset();
		MeshSkinPaths.Reset();
		TArray<int32> SkinIndices;
		Body->GetRuntimeSkinOverrides().GetKeys(SkinIndices);
		SkinIndices.Sort();
		for (const int32 SkinIndex : SkinIndices)
		{
			MeshSkinIndices.Add(SkinIndex);
			MeshSkinPaths.Add(
				Body->GetRuntimeSkinOverrides().FindChecked(SkinIndex));
		}
	}
	// Time-through-the-death-anim, not the local mesh frame (a headless authority
	// never plays it; clients are exactly this far in when the swap replicates).
	PoseFraction = Dead.GetDeathPoseFraction();
	PoseSequence = Dead.GetReplicatedAnimName();
	CarcassTypeName = Dead.CarcassTypeName;
	AnimTableName = Dead.AnimTableName;
	AmbientHumPath = Dead.AmbientHumPath;   // WOTCarcassLegion keeps the hum

	// Physics seeds: the pawn's death-instant velocity (captured before the corpse
	// halt — the tumble the original kept), source cylinder, and the gib bookkeeping
	// (deep-negative death health starts the carcass near its gib threshold,
	// WOTCarcass.uc:127; the threshold is the pawn's -GibForSureFinalHealth).
	Velocity = Dead.DeathVelocity;
	if (const UCapsuleComponent* Cap = Dead.GetCapsuleComponent())
	{
		SourceCollisionRadius = Cap->GetUnscaledCapsuleRadius();
		SourceCollisionHeight = Cap->GetUnscaledCapsuleHalfHeight();
	}
	Capsule->SetCapsuleSize(SourceCollisionRadius, SourceCollisionHeight);
	CumulativeDamage = FMath::Max(0, -Dead.Health);
	GibForSureCumulativeDamage = -Dead.GibForSureFinalHealth;
	// WOTCarcass.uc:113 — Mass = Other.Mass, read from the dead pawn's CMC (the same
	// draw-in mass source). The Legion's carcass is the only one >= 500 (LandThump).
	if (const UCharacterMovementComponent* Move = Dead.GetCharacterMovement())
	{
		Mass = Move->Mass;
	}
	if (Buoyancy < 0.8f * Mass) Buoyancy = 0.9f * Mass;

	// WOTCarcass.uc:154-164 — a balefired pawn's corpse arms INERT: PHYS_None, no
	// launch velocity, no cylinder adjust, closing the 1-tick window before the
	// Balefire sweep removes it. Every other corpse tumbles from its death velocity.
	if (IsBalefired())
	{
		Velocity = FVector::ZeroVector;
		bFalling = false;
	}
	else
	{
		bFalling = true;
	}

	OnRep_Display();   // authority applies its own presentation
	if (UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Zones->UpdateActorZones(this, true);
	}
}

void AWoTCarcass::BeginPlay()
{
	Super::BeginPlay();
	// WOTCarcass.uc:53-59 PostBeginPlay — account this corpse to the zone it spawned
	// in (its death zone; the world bucket if outside every authored zone), then run
	// eviction + arm the zone-scoped cleanup cadence. Authority-only (clients neither
	// evict nor time out corpses — the server's Destroy replicates the removal).
	if (HasAuthority())
	{
		DeathZone = AWoTZoneVolume::EditZoneAt(GetWorld(), GetActorLocation());
		AdjustZoneCount(+1);
		bCountedInZone = true;
		ScheduleCleanup();
	}
	if (UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Zones->UpdateActorZones(this, true);
	}
}

void AWoTCarcass::EndPlay(const EEndPlayReason::Type Reason)
{
	// WOTCarcass.uc:169-177 Destroyed() — decrement the zone's live carcass count.
	if (bCountedInZone)
	{
		AdjustZoneCount(-1);
		bCountedInZone = false;
	}
	Super::EndPlay(Reason);
}

bool AWoTCarcass::SameZoneBucket(const AWoTCarcass& Other) const
{
	// Two corpses share a bucket when they died in the same authored zone, or both
	// died outside every zone (the world-global bucket).
	return DeathZone.Get() == Other.DeathZone.Get();
}

void AWoTCarcass::AddUE1Velocity(const FVector& DeltaVelocity)
{
	if (!HasAuthority() || bChunkedUp || bSinking)
	{
		return;
	}
	Velocity += DeltaVelocity;
	bFalling = true;
	SetActorTickEnabled(true);
	ForceNetUpdate();
}

void AWoTCarcass::AdjustZoneCount(int32 Delta)
{
	if (AWoTZoneVolume* Zone = DeathZone.Get())
	{
		Zone->NumCarcasses += Delta;
	}
	else if (UWoTZoneIndex* Index = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Index->WorldCarcassCount += Delta;
	}
}

int32 AWoTCarcass::GetZoneCarcassCount() const
{
	if (const AWoTZoneVolume* Zone = DeathZone.Get())
	{
		return Zone->NumCarcasses;
	}
	if (const UWoTZoneIndex* Index = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		return Index->WorldCarcassCount;
	}
	return 0;
}

int32 AWoTCarcass::GetZoneMaxCarcasses() const
{
	if (const AWoTZoneVolume* Zone = DeathZone.Get())
	{
		return Zone->MaxCarcasses;
	}
	if (const UWoTZoneIndex* Index = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		return Index->WorldMaxCarcasses;
	}
	return 3;   // Engine.ZoneInfo.MaxCarcasses default
}

void AWoTCarcass::CheckZoneCarcasses()
{
	// WOTCarcass.uc:433-458 Dead.CheckZoneCarcasses: while the zone holds more than
	// MaxCarcasses corpses, evict the best candidate — a corpse no player can see is
	// preferred, otherwise the oldest. The evicted corpse dies through the normal
	// Destroy path (its zone count decrements in EndPlay).
	// DIVERGENCE (PORT_NOTES): retail's PlayerCanSeeMe not-visible preference has no
	// faithful headless authority analog (no local viewer); it degrades to IsHidden()
	// (the port keeps no lingering hidden corpse — ChunkUp destroys at once), so the
	// effective rule is oldest-by-creation, exactly what retail's AllActors fallback
	// (Best = Self) lands on when nothing is off-screen.
	if (!HasAuthority() || GetZoneCarcassCount() <= GetZoneMaxCarcasses())
	{
		return;
	}
	AWoTCarcass* Hidden = nullptr;
	AWoTCarcass* Oldest = nullptr;
	for (TActorIterator<AWoTCarcass> It(GetWorld()); It; ++It)
	{
		AWoTCarcass* C = *It;
		if (!IsValid(C) || !SameZoneBucket(*C))
		{
			continue;
		}
		if (C->IsHidden())
		{
			Hidden = C;
			break;
		}
		if (!Oldest || C->CreationTime < Oldest->CreationTime)
		{
			Oldest = C;
		}
	}
	if (AWoTCarcass* Victim = Hidden ? Hidden : Oldest)
	{
		Victim->Destroy();
	}
}

void AWoTCarcass::OnRep_Display()
{
	if (Mesh->SetVertexMesh(MeshName))
	{
		Mesh->SetRelativeScale3D(FVector(MeshScale));
		Mesh->SetFatness(MeshFatness);
		Mesh->SetActorSkin(MeshActorSkin);
		TMap<int32, FSoftObjectPath> SkinOverrides;
		const int32 SkinCount = FMath::Min(
			MeshSkinIndices.Num(), MeshSkinPaths.Num());
		for (int32 Skin = 0; Skin < SkinCount; ++Skin)
		{
			if (MeshSkinIndices[Skin] >= 0 && !MeshSkinPaths[Skin].IsNull())
			{
				SkinOverrides.Add(MeshSkinIndices[Skin], MeshSkinPaths[Skin]);
			}
		}
		Mesh->SetRuntimeSkinOverrides(SkinOverrides);
		if (!PoseSequence.IsNone())
		{
			Mesh->SetFrameAndStop(PoseSequence, PoseFraction);
		}
	}
	// WOTCarcassLegion.uc:113 — the corpse keeps humming, at Actor-default bytes
	// 32/128/64 (it authors no SoundRadius, unlike the live pawn's 255).
	if (!AmbientHumPath.IsNone() && !AmbientHum &&
	    GetNetMode() != NM_DedicatedServer)
	{
		if (USoundBase* Wave = WoTSound::LoadBySubPath(AmbientHumPath, true))
		{
			AmbientHum = NewObject<UWoTAmbientSoundComponent>(this);
			AmbientHum->SetupAttachment(GetRootComponent());
			AmbientHum->RegisterComponent();
			AmbientHum->ConfigureUE1Ambient(Wave, 32, 128, 64);
			AmbientHum->Play();
		}
	}
}

void AWoTCarcass::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority() && GetWorld()->GetTimeSeconds() >= NextZoneCheckTime)
	{
		AWoTZoneVolume* Zone = nullptr;
		if (const UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
		{
			Zone = Zones->GetRegion(this);
		}
		CheckZone(Zone, 1.0f);
		NextZoneCheckTime = GetWorld()->GetTimeSeconds()
			+ FMath::Max(TimeBetweenZoneChecks, 0.01f);
	}
	if (HasAuthority() && bSinking)
	{
		// UE1 PHYS_None remains based: carry the carcass with a moving support,
		// then reproduce BaseChange by resolving the support under it every tick.
		AActor* PreviousBase = SinkingBase.Get();
		if (IsValid(PreviousBase))
		{
			const FVector BaseLocation = PreviousBase->GetActorLocation();
			const FRotator BaseRotation = PreviousBase->GetActorRotation();
			const float DeltaYaw = FMath::FindDeltaAngleDegrees(
				LastSinkingBaseRotation.Yaw, BaseRotation.Yaw);
			const FVector RelativeToOldBase =
				GetActorLocation() - LastSinkingBaseLocation;
			const FVector CarriedLocation = BaseLocation
				+ FRotator(0.0f, DeltaYaw, 0.0f).RotateVector(RelativeToOldBase);
			FRotator CarriedRotation = GetActorRotation();
			CarriedRotation.Yaw += DeltaYaw;
			SetActorLocationAndRotation(CarriedLocation, CarriedRotation,
				false, nullptr, ETeleportType::TeleportPhysics);
			LastSinkingBaseLocation = BaseLocation;
			LastSinkingBaseRotation = BaseRotation;
		}
		FHitResult SupportHit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(LegionCarcassBase), false, this);
		// StartSinking shifts the actor root four logical half-heights above the
		// already-adjusted capsule. Probe only around the expected support plane;
		// adding PhysicalSinkDistance cancels the manual downward sink. This detects a
		// real BaseChange without snapping to unrelated geometry far below.
		const float RootToSupport = 4.0f * LogicalLandedHalfHeight
			+ (Capsule ? Capsule->GetUnscaledCapsuleHalfHeight() : 0.0f);
		const FVector SupportPlane = GetActorLocation() + FVector(0.0f, 0.0f,
			PhysicalSinkDistance - RootToSupport);
		constexpr float SupportTolerance = 8.0f;
		const bool bFoundSupport = GetWorld()->LineTraceSingleByChannel(
			SupportHit, SupportPlane + FVector(0.0f, 0.0f, SupportTolerance),
			SupportPlane - FVector(0.0f, 0.0f, SupportTolerance),
			ECC_Visibility, Query);
		AActor* CurrentBase = bFoundSupport ? SupportHit.GetActor() : nullptr;
		if (CurrentBase != PreviousBase || (bHadSinkingBase && !PreviousBase))
		{
			// WOTCarcassLegion.BaseChange restarts only the distance budget. It does
			// not rewind the body or reroll the once-per-corpse sink rate.
			ResetLegionSinkForBaseChange();
		}
		const bool bLostBase = bHadSinkingBase && !IsValid(CurrentBase);
		SinkingBase = CurrentBase;
		bHadSinkingBase = IsValid(CurrentBase);
		if (bHadSinkingBase)
		{
			LastSinkingBaseLocation = CurrentBase->GetActorLocation();
			LastSinkingBaseRotation = CurrentBase->GetActorRotation();
		}
		if (bLostBase)
		{
			// Decoration.BaseChange puts an unbased
			// PHYS_None carcass into PHYS_Falling. StartSinking already disabled
			// collision/world collision, so gravity carries it through the world
			// while WOTCarcassLegion.Tick continues the authored sink budget.
			bFalling = true;
			Velocity = FVector::ZeroVector;
		}
		const float Step = ActiveSinkRatePerSec * DeltaSeconds;
		FVector SinkAndFall(0.0f, 0.0f, -Step);
		if (bFalling)
		{
			Velocity += AWoTZoneVolume::GetGravityAt(
				GetWorld(), GetActorLocation()) * DeltaSeconds;
			SinkAndFall += Velocity * DeltaSeconds;
		}
		AddActorWorldOffset(SinkAndFall, false);
		PhysicalSinkDistance += Step;
		TotalSinkDistance += Step;
		if (TotalSinkDistance > 3.5f * LogicalLandedHalfHeight)
		{
			Destroy();
		}
		return;
	}
	// PHYS_Falling on the authority (clients follow replicated movement): integrate
	// gravity, sweep-move, and dispatch Landed/HitWall like the original Decoration
	// physics. A landed carcass stops simulating entirely.
	if (!HasAuthority() || !bFalling)
	{
		return;
	}
	Velocity += AWoTZoneVolume::GetGravityAt(
		GetWorld(), GetActorLocation()) * DeltaSeconds;
	FHitResult Hit;
	AddActorWorldOffset(Velocity * DeltaSeconds, /*bSweep*/ true, &Hit);
	if (Hit.IsValidBlockingHit())
	{
		if (Hit.ImpactNormal.Z > 0.7f)
		{
			Landed(Hit);
		}
		else
		{
			HitWall(Hit.ImpactNormal);
		}
	}
}

void AWoTCarcass::HandleZoneChange(AWoTZoneVolume* NewZone,
	AWoTZoneVolume* OldZone)
{
	WoTCarcassZoneBehavior::HandleZoneChange(*this, Capsule, Velocity,
		RotationRate, Mass, Buoyancy, bTakesDamage, PainZoneDamageScale,
		bSplash, bWaterCollisionFlattened, NewZone, OldZone);
}

void AWoTCarcass::CheckZone(AWoTZoneVolume* Zone, float DeltaSeconds)
{
	WoTCarcassZoneBehavior::CheckZone(
		*this, Zone, bTakesDamage, PainZoneDamageScale, DeltaSeconds);
}

void AWoTCarcass::HitWall(const FVector& HitNormal)
{
	// WOTCarcass.uc:575-584: reflect at 0.7, damp Z by 0.9, stop bouncing under 120.
	Velocity = 0.7f * (Velocity - 2.0f * HitNormal * (Velocity | HitNormal));
	Velocity.Z *= 0.9f;
	if (FMath::Abs(Velocity.Z) < 120.0f)
	{
		Velocity = FVector::ZeroVector;
	}
}

void AWoTCarcass::Landed(const FHitResult& Hit)
{
	// WOTCarcass.uc:505-532: a hard enough fall gibs outright; otherwise lie flat
	// (roll/pitch zeroed), drop blocking, and squash/shift the cylinder.
	if (GibForSureCumulativeDamage > 0.0f && Velocity.Z < -1000.0f)
	{
		ChunkUp(200.0f);
		return;
	}
	if (FMath::Abs(Velocity.Z) >= 120.0f)
	{
		// Still bouncing: treat the floor like a wall until the bounce dies out.
		HitWall(Hit.ImpactNormal);
		return;
	}
	Velocity = FVector::ZeroVector;
	bFalling = false;
	FRotator Flat = GetActorRotation();
	Flat.Roll = 0.0f;
	Flat.Pitch = 0.0f;
	SetActorRotation(Flat);
	// LieStill (WOTCarcass.uc:588-598): thump BEFORE AdjustCylinder — the Mass >= 500
	// LandThump gate reads the un-reduced Mass (AdjustCylinder's x0.8 comes after).
	LandThump();
	AdjustCylinder();
	WoTSound::PlayReplicated(*GetWorld(), TEXT("WOT/Landed1"),
		GetActorLocation(), WoTSound::DefaultPlaySoundRadius);
	if (CarcassTypeName == TEXT("WoTPawns.WOTCarcassLegion"))
	{
		StartLegionSinking(Hit.GetActor());
	}
}

void AWoTCarcass::StartLegionSinking(AActor* LandedBase)
{
	if (!HasAuthority() || bSinking) return;
	// StartSinking runs after Super.Landed. The original cylinder can be flatter
	// than a UE capsule, so its authored logical height—not UE's clamped capsule
	// half-height—drives both the initial counter-shift and destruction depth.
	LogicalLandedHalfHeight = CarcassCCHeightMultiplier * SourceCollisionRadius;
	const FVector CollisionShift(0.0f, 0.0f,
		4.0f * LogicalLandedHalfHeight);
	AddActorWorldOffset(CollisionShift, false);
	Mesh->AddWorldOffset(-CollisionShift);
	MeshRelativeOffset = Mesh->GetRelativeLocation();
	Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// PerturbFloatPercent(default.SinkRatePerSec=3, 50).
	ActiveSinkRatePerSec = WoTRandom::FRandRange(1.5f, 4.5f);
	PhysicalSinkDistance = 0.0f;
	TotalSinkDistance = 0.0f;
	SinkingBase = LandedBase;
	bHadSinkingBase = IsValid(LandedBase);
	LastSinkingBaseLocation = bHadSinkingBase
		? LandedBase->GetActorLocation() : FVector::ZeroVector;
	LastSinkingBaseRotation = bHadSinkingBase
		? LandedBase->GetActorRotation() : FRotator::ZeroRotator;
	bSinking = true;
	OnRep_Sinking();
	ForceNetUpdate();
}

void AWoTCarcass::ResetLegionSinkForBaseChange()
{
	TotalSinkDistance = 0.0f;
}

void AWoTCarcass::OnRep_Sinking()
{
	if (bSinking && Capsule)
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AWoTCarcass::LandThump()
{
	// WOTCarcass.uc:241-260: fires once, authority-side; only a Mass >= 500 corpse
	// (the Legion) throws nearby players.
	if (bThumped)
	{
		return;
	}
	bThumped = true;
	if (HasAuthority() && Mass >= 500.0f)
	{
		ThrowOthers();
	}
}

void AWoTCarcass::ThrowOthers()
{
	// WOTCarcass.uc:212-237, ported verbatim. Every human player gets a distance-
	// scaled view shake; walking players within 1500uu also take an AddVelocity kick
	// whose vertical component is 7000000 / ((0.4*dist + 350) * playerMass).
	const FVector Here = GetActorLocation();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
	     It; ++It)
	{
		AWoTPlayerController* PC = Cast<AWoTPlayerController>(It->Get());
		if (!PC)
		{
			continue;
		}
		AWoTCharacter* Player = Cast<AWoTCharacter>(PC->GetPawn());
		if (!Player)
		{
			continue;
		}
		const float Dist = FVector::Dist(Here, Player->GetActorLocation());
		// shake = FMax(500, 1500 - dist); ShakeView( FMax(0, 0.35 - dist/20000),
		// shake, 0.015 * shake ) — (time, roll, vert).
		const float Shake = FMath::Max(500.0f, 1500.0f - Dist);
		PC->ClientShakeView(FMath::Max(0.0f, 0.35f - Dist / 20000.0f), Shake,
		                    0.015f * Shake);

		const UCharacterMovementComponent* Move = Player->GetCharacterMovement();
		const bool bWalking = Move && (Move->MovementMode == MOVE_Walking ||
		                               Move->MovementMode == MOVE_NavWalking);
		if (bWalking && Dist < 1500.0f)
		{
			const float PlayerMass = (Move->Mass > 0.0f) ? Move->Mass : 175.0f;
			// Momentum = -0.5 * Velocity + 100 * VRand(); Momentum.Z overwritten.
			FVector Momentum = -0.5f * Player->GetVelocity() + 100.0f * WoTRandom::VRand();
			Momentum.Z = 7000000.0f / ((0.4f * Dist + 350.0f) * PlayerMass);
			Player->AddUE1Velocity(Momentum);
		}
	}
}

void AWoTCarcass::AdjustCylinder()
{
	// AdjustCylinder (WOTCarcass.uc:536-571): squash the cylinder flat (radius x1.2
	// +4, height 0.4 x source RADIUS — verbatim, the height keys off the radius) and
	// nudge the CAPSULE toward where the death anim left the body — the anim table's
	// end vector in units of half collision radii, rotated by facing — while the mesh
	// stays put (counter-offset).
	// DIVERGENCE (engine invariant): UE capsules clamp half-height >= radius, so the
	// "flat disc" the original made (height 0.4 x radius) bottoms out at the new
	// radius instead — the corpse's collision puck is taller than UE1's (PORT_NOTES).
	const float NewRadius = CarcassCCRadiusMultiplier * SourceCollisionRadius + 4.0f;
	const float NewHalfHeight = CarcassCCHeightMultiplier * SourceCollisionRadius;
	const FVector OldBottom =
	    GetActorLocation() - FVector(0, 0, Capsule->GetUnscaledCapsuleHalfHeight());
	Capsule->SetCapsuleSize(NewRadius, FMath::Max(NewHalfHeight, 4.0f));
	FVector Shift = OldBottom + FVector(0, 0, Capsule->GetUnscaledCapsuleHalfHeight()) -
	                GetActorLocation();

	const FVector EndVector =
	    WoTAnimTable::FindAnimEndVector(AnimTableName, PoseSequence);
	if (!EndVector.IsNearlyZero())
	{
		FRotator YawOnly = GetActorRotation();
		YawOnly.Roll = 0.0f;
		YawOnly.Pitch = 0.0f;
		Shift += SourceCollisionRadius / 2.0f * YawOnly.RotateVector(EndVector);
	}
	if (!Shift.IsNearlyZero())
	{
		AddActorWorldOffset(Shift, /*bSweep*/ false);
		Mesh->AddWorldOffset(-Shift);
		MeshRelativeOffset = Mesh->GetRelativeLocation();
	}
}

void AWoTCarcass::ScheduleCleanup()
{
	// R3a (WOTCarcass.uc:433-469): first evict if this corpse pushed its zone over
	// budget, then arm the cleanup timer on the ZONE-scoped cadence — more corpses in
	// the zone clean faster: max(12, 30 - 2 * zone count).
	CheckZoneCarcasses();
	GetWorldTimerManager().SetTimer(
	    CleanupTimer, this, &AWoTCarcass::CleanupTimerFired,
	    FMath::Max(12.0f, 30.0f - 2.0f * GetZoneCarcassCount()), false);
}

void AWoTCarcass::CleanupTimerFired()
{
	// Engine.Carcass.Dead.Timer: over-budget corpses go immediately.  Otherwise a
	// visible corpse survives and is reconsidered on the two-second cadence.
	if (GetZoneCarcassCount() > GetZoneMaxCarcasses() || !PlayerCanSeeMe())
	{
		Destroy();
		return;
	}
	GetWorldTimerManager().SetTimer(
		CleanupTimer, this, &AWoTCarcass::CleanupTimerFired, 2.0f, false);
}

bool AWoTCarcass::PlayerCanSeeMe() const
{
	if (!GetWorld()) return false;
	float Radius = 0.0f, HalfHeight = 0.0f;
	GetSimpleCollisionCylinder(Radius, HalfHeight);
	const float MaxDistanceSq = 100000.0f * (Radius + 3.6f);
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PC = It->Get();
		const APawn* Viewer = PC ? PC->GetPawn() : nullptr;
		if (!Viewer) continue;
		// UE1's TestCanSeeMe: ViewTarget wins before range; behind-view
		// cameras skip the cone; otherwise the dot product is squared, accepting
		// both the front and rear 60-degree cones.
		if (PC->GetViewTarget() == this) return true;
		const FVector Delta = GetActorLocation() - Viewer->GetActorLocation();
		const float DistSq = Delta.SizeSquared();
		if (DistSq >= MaxDistanceSq) continue;
		const AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(PC);
		const bool bBehindView = WoTPC && WoTPC->IsMatchEnded();
		const float Dot = FVector::DotProduct(
			Viewer->GetViewRotation().Vector(), Delta);
		if ((bBehindView || FMath::Square(Dot) >= 0.25f * DistSq)
			&& WoTSight::PawnCanSeeActor(Viewer, this, -1.0f, false))
		{
			return true;
		}
	}
	return false;
}

float AWoTCarcass::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                              AController* EventInstigator, AActor* DamageCauser)
{
	// WOTCarcassBase TakeDamage (:183-208): accumulate toward the damage-scaled gib
	// threshold. The blood spurt + momentum add land with 4d (momentum isn't carried
	// in the damage event yet — same note as the pawn chain).
	Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	// WOTCarcass.uc:185 — a balefired corpse takes no damage (no blood, no gib).
	if (!HasAuthority() || GibForSureCumulativeDamage <= 0.0f || DamageAmount <= 0.0f ||
	    bChunkedUp || IsBalefired())
	{
		return 0.0f;
	}
	// SpawnBlood(DamageCarcassBloodClassStr) — the shot-corpse spray (WOTCarcass.uc:337;
	// black family swaps the set, WOTCarcassBlack.uc:1296).
	WoTBloodFX::SpawnBurstMarker(
	    *GetWorld(),
	    FName(IsBlackBloodFamily() ? TEXT("BloodSprayBlack") : TEXT("BloodSprayRed")),
	    GetActorLocation() + FVector(0, 0, 0.5f * SourceCollisionHeight),
	    FRotator(60.0f, WoTRandom::FRandRange(0.0f, 360.0f), 0.0f));
	CumulativeDamage += DamageAmount;
	if (CumulativeDamage > BaseGibDamage / DamageAmount * GibForSureCumulativeDamage)
	{
		ChunkUp(DamageAmount);
	}
	return DamageAmount;
}

void AWoTCarcass::ChunkUp(float /*Damage*/)
{
	// WOTCarcass.ChunkUp (:339-369) + CreateReplacement/ClientExtraChunks (:390-384):
	// the recipe's part 0 becomes the persistent replacement chunk (DrawScale x2,
	// WOTReplacementChunk defaults), parts 1..N spawn odds-gated, offset by
	// ZOffset x CollisionHeight. The port spawns ALL chunks authority-side and
	// replicates them (shipped spawned the extras client-side — a bandwidth
	// optimization traded for simplicity; PORT_NOTES). Gib blood burst = 4d;
	// Gore detail affects standalone only; retail multiplayer always gibs.
	// WOTCarcass.uc:343 — a balefired corpse never gibs (the Balefire sweep removes
	// it whole).
	if (bChunkedUp || !HasAuthority() || IsBalefired())
	{
		return;
	}
	if (GetNetMode() == NM_Standalone)
	{
		const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
		if (Preferences && Preferences->GetGameSettings().GoreDetailLevel <= 1)
		{
			return;
		}
	}
	bChunkedUp = true;
	// GibSound (WOTCarcass.uc:481-495): GibExplode1/2 at 50/50 (the WOT bank).
	WoTSound::PlayReplicated(
	    *GetWorld(),
	    FName(WoTRandom::FRand() < 0.5f ? TEXT("WOT/GibExplode1") : TEXT("WOT/GibExplode2")),
	    GetActorLocation(), 1600.0f);
	// SpawnBlood(GibbedBloodClassStr) — the gib explosion (WOTCarcass.uc:350).
	WoTBloodFX::SpawnBurstMarker(
	    *GetWorld(),
	    FName(IsBlackBloodFamily() ? TEXT("BloodBurstBlack2") : TEXT("BloodBurstRed2")),
	    GetActorLocation() + FVector(0, 0, 0.5f * SourceCollisionHeight),
	    GetActorRotation());
	if (const TArray<FWoTBodyPart>* Recipe = WoTGoreData::FindRecipe(CarcassTypeName))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		for (int32 i = 0; i < Recipe->Num(); ++i)
		{
			const FWoTBodyPart& Body = (*Recipe)[i];
			if (Body.MeshName.IsNone())
			{
				continue;
			}
			// Index 0 always spawns (the replacement); the rest roll their odds.
			if (i > 0 && WoTRandom::FRand() >= Body.Odds)
			{
				continue;
			}
			const FVector At = GetActorLocation() +
			                   FVector(0, 0, Body.ZOffset * SourceCollisionHeight);
			if (AWoTCreatureChunk* Chunk =
			        GetWorld()->SpawnActor<AWoTCreatureChunk>(At, GetActorRotation(), Params))
			{
				Chunk->InitFor(*this, Body, i == 0 ? 2.0f : 1.0f);
			}
		}
	}
	Destroy();
}
