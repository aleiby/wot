#include "WoTCreatureChunk.h"
#include "WoTRandom.h"

#include "WoTBloodFX.h"
#include "WoTCarcass.h"
#include "WoTCarcassZoneBehavior.h"
#include "WoTDecal.h"
#include "WoTSoundTable.h"
#include "WoTVertexAnimComponent.h"
#include "WoTZoneVolume.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTGameMode.h"

#include "Components/CapsuleComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	void ZeroRotatorAxis(double& Degrees)
	{
		const double Wrapped = FRotator::ClampAxis(Degrees);
		Degrees = Wrapped < 90.0 || Wrapped >= 270.0 ? 0.0 : 180.0;
	}

	void TwoWallAdjust(const FVector& DesiredDirection, FVector& Delta,
		const FVector& HitNormal, const FVector& OldHitNormal, float HitTime)
	{
		if ((OldHitNormal | HitNormal) <= 0.0f)
		{
			const FVector NewDirection = FVector::CrossProduct(
				HitNormal, OldHitNormal).GetSafeNormal();
			Delta = (Delta | NewDirection) * (1.0f - HitTime) * NewDirection;
			if ((DesiredDirection | Delta) < 0.0f)
			{
				Delta *= -1.0f;
			}
		}
		else
		{
			Delta = (Delta - HitNormal * (Delta | HitNormal))
				* (1.0f - HitTime);
			if ((Delta | DesiredDirection) <= 0.0f)
			{
				Delta = FVector::ZeroVector;
			}
		}
	}

	TUniquePtr<FWoTCreatureChunkSet>& InstalledCreatureChunkRegistry()
	{
		static TUniquePtr<FWoTCreatureChunkSet> Installed;
		return Installed;
	}
}

void FWoTCreatureChunkBuilder::Install(FWoTCreatureChunkSet&& Set)
{
	TUniquePtr<FWoTCreatureChunkSet> Candidate =
		MakeUnique<FWoTCreatureChunkSet>(MoveTemp(Set));
	Swap(InstalledCreatureChunkRegistry(), Candidate);
}

#if WITH_AUTOMATION_TESTS
void FWoTCreatureChunkBuilder::Reset()
{
	InstalledCreatureChunkRegistry().Reset();
}
#endif

namespace WoTGoreData
{
	const TArray<FWoTBodyPart>* FindRecipe(FName CarcassTypeName)
	{
		const TUniquePtr<FWoTCreatureChunkSet>& Installed =
			InstalledCreatureChunkRegistry();
		if (!Installed) return nullptr;
		// WOTCarcass.uc leaves CarcassType unset for the base recipe.
		static const FName BaseCarcassClass(TEXT("WOT.WOTCarcass"));
		const FName Key = CarcassTypeName.IsNone() ? BaseCarcassClass : CarcassTypeName;
		const FName* RecipeClass = Installed->RecipeClassByCarcassClass.Find(Key);
		if (RecipeClass == nullptr && Installed->Recipes.Contains(Key))
		{
			RecipeClass = &Key;
		}
		return RecipeClass == nullptr ? nullptr : Installed->Recipes.Find(*RecipeClass);
	}

	const FWoTCreatureChunkDefaults* GetDefaults()
	{
		const TUniquePtr<FWoTCreatureChunkSet>& Installed =
			InstalledCreatureChunkRegistry();
		return Installed ? &Installed->Defaults : nullptr;
	}
}

AWoTCreatureChunk::AWoTCreatureChunk()
{
	PrimaryActorTick.bCanEverTick = true;

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	SetRootComponent(Capsule);
	Capsule->InitCapsuleSize(8.0f, 8.0f);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	Capsule->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);

	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("ChunkMesh"));
	Mesh->SetupAttachment(Capsule);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	bReplicates = true;
	SetReplicatingMovement(true);
}

void AWoTCreatureChunk::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTCreatureChunk, MeshName);
	DOREPLIFETIME(AWoTCreatureChunk, SkinName);
	DOREPLIFETIME(AWoTCreatureChunk, SkinSlot);
	DOREPLIFETIME(AWoTCreatureChunk, MeshScale);
	DOREPLIFETIME(AWoTCreatureChunk, bTrailBlood);
	DOREPLIFETIME(AWoTCreatureChunk, PresentationGoreLevel);
	DOREPLIFETIME(AWoTCreatureChunk, BleedDuration);
}

void AWoTCreatureChunk::BeginPlay()
{
	Super::BeginPlay();
	if (UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Zones->UpdateActorZones(this, true);
	}
	// Clients: the initial bunch applied bTrailBlood before BeginPlay — start the
	// local trail here. The authority starts its own in InitFor (which runs AFTER
	// its BeginPlay, once the flag is known).
	if (!HasAuthority() && bTrailBlood)
	{
		SpawnTrail();
	}
}

void AWoTCreatureChunk::SpawnTrail()
{
	if (!Trail.IsValid() && GetWorld())
	{
		Trail = WoTBloodFX::SpawnLocal(*GetWorld(), FName(TEXT("BloodSpray01")),
		                               GetActorLocation(), FRotator(-90.0f, 0, 0),
		                               1.0f, this);
		if (Trail.IsValid() && BleedDuration > 0.0f)
			BleedRate = Trail->Volume / BleedDuration;
	}
}

void AWoTCreatureChunk::Destroyed()
{
	ReleaseDeathZoneCount();
	// Gibs.uc drains emission and leaves existing motes to age out.
	if (Trail.IsValid())
	{
		Trail->Volume = 0.0f;
		Trail->bOn = false;
		Trail->FollowActor = nullptr;
		Trail->SetLifeSpan(15.0f);
	}
	Super::Destroyed();
}

void AWoTCreatureChunk::InitFor(const AWoTCarcass& Source, const FWoTBodyPart& Part,
                                float ScaleMultiplier)
{
	if (!HasAuthority())
	{
		return;
	}
	MeshName = Part.MeshName;
	SkinName = Part.SkinTexName;
	SkinSlot = Part.SkinSlot;
	bTakesDamage = !Part.bNoDamage;
	bTrailBlood = !Part.bNoBlood;
	PresentationGoreLevel = Part.GoreLevel;
	bBlackDecals = Source.IsBlackBloodFamily();
	BounceSound = Part.BounceSound;
	bZeroYaw = Part.bZeroYaw;
	bZeroRoll = Part.bZeroRoll;
	bZeroPitch = Part.bZeroPitch;
	bBounce = true;
	bFalling = true;
	if (const FWoTCreatureChunkDefaults* Defaults = WoTGoreData::GetDefaults())
	{
		MinBounceSpeed = Defaults->MinBounceSpeed;
		DeleteChunkInitialDelaySecs = Defaults->DeleteInitialDelay;
		DeleteChunkCheckAgainDelaySecs = Defaults->DeleteRecheckDelay;
		BaseSinkRatePerSecMin = Defaults->SinkRateMin;
		BaseSinkRatePerSecMax = Defaults->SinkRateMax;
		BaseInitForVelocity = Defaults->BaseInitVelocity;
		PainZoneDamageScale = Defaults->PainZoneDamageScale;
		bShouldSink = Defaults->bShouldSink;
		BleedDuration = WoTRandom::FRandRange(
			Defaults->MinBleedTime, Defaults->MaxBleedTime);
	}
	DeathZone = AWoTZoneVolume::EditZoneAt(GetWorld(), GetActorLocation());
	if (DeathZone.IsValid())
	{
		++DeathZone->NumCarcasses;
		bDeathZoneCounted = true;
	}
	else if (UWoTZoneIndex* Index = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		++Index->WorldCarcassCount;
		bDeathZoneCounted = true;
	}
	if (bTrailBlood)
	{
		SpawnTrail();
	}

	// CreateChunk's bent-leg swap (WOTCarcass.uc:296-298, BentLegOdds 0.5). The
	// clean-bone/skull skin swaps have authored odds 0 in shipped data (CleanBoneOdds/
	// CleanSkullOdds are never set) — dead branches, not ported.
	if (MeshName == FName(TEXT("Leg")) && WoTRandom::FRand() < 0.5f)
	{
		MeshName = FName(TEXT("LegBent"));
	}

	// Chunks are authored player-sized: scale by source height / the player's 46
	// half-height, mass by scale squared (InitFor, WOTCreatureChunk.uc:170-181).
	const float HeightScale = Source.GetSourceCollisionHeight() / 46.0f;
	MeshScale = ScaleMultiplier * HeightScale;
	Mass = 30.0f * ScaleMultiplier * FMath::Square(HeightScale);

	// Randomized toss (uc:191-192) + tumble (uc:186-189), minus the zeroed axes.
	Velocity = (0.2f + WoTRandom::FRand()) * BaseInitForVelocity * WoTRandom::VRand().GetSafeNormal();
	Velocity.Z = (0.2f + WoTRandom::FRand()) * BaseInitForVelocity;
	// WOTCreatureChunk.uc:193-205: a chunk born underwater loses half of its
	// complete launch velocity, then every chunk receives one of the two retail
	// buoyancy ratios.  This must precede the initial zone notification so the
	// water collision response observes the initialized mass and buoyancy.
	if (const AWoTZoneVolume* Zone = AWoTZoneVolume::ZoneAt(GetWorld(), GetActorLocation());
		Zone && Zone->bWaterZone)
	{
		Velocity *= 0.5f;
	}
	Buoyancy = (WoTRandom::FRand() < 0.3f ? 1.06f : 0.94f) * Mass;
	TumbleRate = FRotator(
	    Part.bZeroPitch ? 0.0f : WoTRandom::FRandRange(-550.0f, 550.0f),
	    Part.bZeroYaw ? 0.0f : WoTRandom::FRandRange(-550.0f, 550.0f),
	    Part.bZeroRoll ? 0.0f : WoTRandom::FRandRange(-550.0f, 550.0f));
	if (UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
	{
		Zones->UpdateActorZones(this, true);
	}

	OnRep_Display();

	// Dead-state cleanup: DeleteChunkInitialDelaySecs (+ up to ~50% slack), then
	// the shipped Q3A sink removes the part below the floor.
	GetWorldTimerManager().SetTimer(
	    CleanupTimer, FTimerDelegate::CreateWeakLambda(this, [this] { CheckCleanupGate(); }),
	    DeleteChunkInitialDelaySecs * (1.0f + 0.5f * WoTRandom::FRand()), false);
}

void AWoTCreatureChunk::OnRep_Display()
{
	if (Mesh->SetVertexMesh(MeshName))
	{
		Mesh->SetRelativeScale3D(FVector(MeshScale));
		if (!SkinName.IsNone() && SkinSlot >= 0)
		{
			const FString Skin=SkinName.ToString();
			Mesh->SetSkinOverride(SkinSlot, FSoftObjectPath(FString::Printf(
				TEXT("/Game/WoT/Textures/WOT/%s.%s"),*Skin,*Skin)));
		}
	}
	ApplyLocalGoreVisibility();
}

void AWoTCreatureChunk::ApplyLocalGoreVisibility()
{
	uint8 LocalGore = 2;
	if (const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr)
	{
		LocalGore = Preferences->GetGameSettings().GoreDetailLevel;
	}
	Mesh->SetVisibility(PresentationGoreLevel <= LocalGore);
}

void AWoTCreatureChunk::StartSinking()
{
	if (!HasAuthority()) return;
	ReleaseDeathZoneCount();
	if (!bShouldSink)
	{
		Destroy(); return;
	}
	if (AWoTZoneVolume::IsPointInWater(GetWorld(),GetActorLocation()))
	{
		Destroy(); return;
	}
	bFalling=false;bSinking=true;SinkRate=WoTRandom::FRandRange(
		BaseSinkRatePerSecMin,BaseSinkRatePerSecMax);SinkDistance=0.0f;
	Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (bTrailBlood && Trail.IsValid()) { Trail->Volume=0; Trail->bOn=false; }
}

void AWoTCreatureChunk::CheckCleanupGate()
{
	if (!HasAuthority()) return;
	const UWoTZoneIndex* Index = GetWorld()->GetSubsystem<UWoTZoneIndex>();
	const bool bOverDeathZoneCap = DeathZone.IsValid()
		? DeathZone->NumCarcasses > DeathZone->MaxCarcasses
		: Index && Index->WorldCarcassCount > Index->WorldMaxCarcasses;
	const bool bCombatOrNetworked = GetNetMode() != NM_Standalone
		|| Cast<AWoTGameMode>(GetWorld()->GetAuthGameMode()) != nullptr;
	if (bOverDeathZoneCap || bCombatOrNetworked)
	{
		StartSinking();
		return;
	}
	GetWorldTimerManager().SetTimer(CleanupTimer,
		FTimerDelegate::CreateWeakLambda(this, [this] { CheckCleanupGate(); }),
		FMath::Max(0.01f, DeleteChunkCheckAgainDelaySecs), false);
}

void AWoTCreatureChunk::ReleaseDeathZoneCount()
{
	if (!bDeathZoneCounted) return;
	if (AWoTZoneVolume* Zone = DeathZone.Get())
		Zone->NumCarcasses = FMath::Max(0, Zone->NumCarcasses - 1);
	else if (UWoTZoneIndex* Index = GetWorld()->GetSubsystem<UWoTZoneIndex>())
		Index->WorldCarcassCount = FMath::Max(0, Index->WorldCarcassCount - 1);
	bDeathZoneCounted = false;
}

void AWoTCreatureChunk::MulticastSpawnBounceDecal_Implementation(FName Type,
	FVector Location, FRotator Rotation, float Scale, FVector Normal)
{
	if (UWorld* World = GetWorld(); World && World->GetNetMode() != NM_DedicatedServer)
	{
		WoTDecalFX::SpawnAlignedFit(*World, Type, Location, Rotation, Scale,
			Normal, /*bReplicated*/ false);
	}
}

void AWoTCreatureChunk::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ApplyLocalGoreVisibility();
	if (Trail.IsValid() && Trail->bOn && BleedRate > 0.0f)
	{
		Trail->Volume = FMath::Max(0.0f,
			Trail->Volume - BleedRate * DeltaSeconds);
		if (Trail->Volume <= 0.0f) Trail->bOn = false;
	}
	if (!HasAuthority()) return;
	if (bSinking)
	{
		const float Step = SinkRate * DeltaSeconds;
		AddActorWorldOffset(FVector(0,0,-Step));
		SinkDistance += Step;
		if (SinkDistance > 2.0f*Capsule->GetUnscaledCapsuleHalfHeight()+20.0f) Destroy();
		return;
	}
	if (GetWorld()->GetTimeSeconds() >= NextZoneCheckTime)
	{
		AWoTZoneVolume* Zone = nullptr;
		if (const UWoTZoneIndex* Zones = GetWorld()->GetSubsystem<UWoTZoneIndex>())
		{
			Zone = Zones->GetRegion(this);
		}
		CheckZone(Zone, 1.0f);
		NextZoneCheckTime = GetWorld()->GetTimeSeconds() + 1.0f;
		if (!IsValid(this) || IsActorBeingDestroyed()) return;
	}
	if (!bFalling)
	{
		return;
	}
	// Engine physFalling time-slices long frames so
	// linear fluid drag never receives a hitch-sized delta.  The zone lookup is
	// deliberately inside the loop: a chunk can cross the water boundary during
	// one frame and the next slice must use the new region's formula.
	float RemainingTime = DeltaSeconds;
	int32 Iterations = 0;
	int32 NumBounces = 0;
	while (RemainingTime > 0.0f && Iterations < 8 && bFalling)
	{
		++Iterations;
		const float TimeTick = RemainingTime > 0.1f
			? FMath::Min(0.1f, RemainingTime * 0.5f)
			: RemainingTime;
		RemainingTime -= TimeTick;

		const FVector OldLocation = GetActorLocation();
		FVector OldVelocity = Velocity;
		const AWoTZoneVolume* Zone = AWoTZoneVolume::ZoneAt(
			GetWorld(), OldLocation);
		const FVector ZoneVelocity = Zone
			? Zone->ZoneVelocity : FVector::ZeroVector;
		if (Zone && Zone->bWaterZone)
		{
			Velocity = Velocity
				* (1.0f - 2.0f * Zone->ZoneFluidFriction * TimeTick)
				+ 0.5f * Zone->ZoneGravity
					* (1.0f - Buoyancy / FMath::Max(1.0f, Mass))
					* TimeTick;
		}
		else
		{
			Velocity += 0.5f * AWoTZoneVolume::GetGravityAt(
				GetWorld(), GetActorLocation()) * TimeTick;
		}

		const FVector Adjusted = (Velocity + ZoneVelocity) * TimeTick;
		FHitResult Hit;
		AddActorWorldOffset(Adjusted, /*bSweep*/ true, &Hit);
		SetActorRotation(GetActorRotation() + TumbleRate * TimeTick);
		if (Hit.bBlockingHit)
		{
			if (bBounce)
			{
				HitWall(Hit.Normal);
				if (!bFalling)
				{
					return;
				}
				if (NumBounces < 2)
				{
					RemainingTime += TimeTick * (1.0f - Hit.Time);
				}
				++NumBounces;
			}
			else if (Hit.Normal.Z > 0.7f)
			{
				RemainingTime += TimeTick * (1.0f - Hit.Time);
				if (Hit.Time > 0.1f && Hit.Time * TimeTick > 0.003f)
				{
					Velocity = (GetActorLocation() - OldLocation)
						/ (TimeTick * Hit.Time);
				}
				Landed(Hit.Normal);
				return;
			}
			else
			{
				if (Hit.bStartPenetrating)
				{
					// MoveActor resolves contact penetration before calculating its
					// tangent move. Direct actor sweeps need the same small depenetration.
					AddActorWorldOffset(Hit.Normal * 0.125f, false);
				}
				const FVector OldHitNormal = Hit.Normal;
				FVector Delta = (Adjusted
					- Hit.Normal * (Adjusted | Hit.Normal))
					* (1.0f - Hit.Time);
				if ((Delta | Adjusted) >= 0.0f)
				{
					FHitResult SecondHit;
					AddActorWorldOffset(Delta, true, &SecondHit);
					if (SecondHit.bBlockingHit)
					{
						if (SecondHit.Normal.Z > 0.7f)
						{
							Landed(SecondHit.Normal);
							return;
						}
						const FVector DesiredDirection = Adjusted.GetSafeNormal();
						TwoWallAdjust(DesiredDirection, Delta, SecondHit.Normal,
							OldHitNormal, SecondHit.Time);
						const bool bDitch = OldHitNormal.Z > 0.0f
							&& SecondHit.Normal.Z > 0.0f && Delta.Z == 0.0f
							&& (SecondHit.Normal | OldHitNormal) < 0.0f;
						FHitResult ThirdHit;
						const bool bThirdMoveAttempted = !Delta.IsZero();
						if (bThirdMoveAttempted)
						{
							AddActorWorldOffset(Delta, true, &ThirdHit);
						}
						if (bDitch || (ThirdHit.bBlockingHit
							&& ThirdHit.Normal.Z > 0.7f))
						{
							// A clear attempted move carries the default zero hit normal,
							// matching the retail FCheckResult. Only a skipped zero-delta
							// move retains the second wall's result.
							Landed(bThirdMoveAttempted
								? ThirdHit.Normal : SecondHit.Normal);
							return;
						}
					}
				}
				const double OldZ = OldVelocity.Z;
				OldVelocity = (GetActorLocation() - OldLocation) / TimeTick;
				OldVelocity.Z = OldZ;
			}
		}

		if (!bBounce)
		{
			Velocity = (GetActorLocation() - OldLocation) / TimeTick
				- ZoneVelocity;
			if (Velocity.Z < OldVelocity.Z || OldVelocity.Z >= 0.0f)
			{
				Velocity = 2.0f * Velocity - OldVelocity;
			}
			constexpr float ZoneTerminalVelocity = 2500.0f;
			Velocity = Velocity.GetClampedToMaxSize(ZoneTerminalVelocity);
		}
	}
}

void AWoTCreatureChunk::HandleZoneChange(AWoTZoneVolume* NewZone,
	AWoTZoneVolume* OldZone)
{
	WoTCarcassZoneBehavior::HandleZoneChange(*this, Capsule, Velocity,
		TumbleRate, Mass, Buoyancy, bTakesDamage, PainZoneDamageScale,
		bSplash, bWaterCollisionFlattened, NewZone, OldZone);
}

void AWoTCreatureChunk::CheckZone(AWoTZoneVolume* Zone, float DeltaSeconds)
{
	WoTCarcassZoneBehavior::CheckZone(
		*this, Zone, bTakesDamage, PainZoneDamageScale, DeltaSeconds);
}

void AWoTCreatureChunk::HitWall(const FVector& HitNormal)
{
#if WITH_AUTOMATION_TESTS
	++BlockingHitCountForTesting;
#endif
	// WOTCreatureChunk.uc:257-299: a bleeding chunk stamps a blood decal at every
	// bounce (replicated — only the authority sees the bounce), then reflects with
	// randomized 0.6-0.8 damping, caps Z at 700, and disables further bounce
	// response under 120 while falling physics continues.
	if (bTrailBlood && GetWorld())
	{
		const FName Type(bBlackDecals
			? TEXT("BloodDecalBlack") : TEXT("BloodDecalRed"));
		const FWoTDecalConfig* Config = WoTDecalFX::Find(Type);
		FVector Location;
		FRotator Rotation;
		float Scale = Config ? Config->DrawScale : 1.0f;
		if (Config && WoTDecalFX::FindAlignedFit(*GetWorld(), GetActorLocation(),
			HitNormal, Scale, Location, Rotation, Scale, this))
		{
			MulticastSpawnBounceDecal(Type, Location, Rotation, Scale, HitNormal);
		}
	}
	const float Damp = WoTRandom::FRandRange(0.6f, 0.8f);
	Velocity = Damp * (Velocity - 2.0f * HitNormal * (Velocity | HitNormal));
	Velocity.Z = FMath::Min(Velocity.Z * WoTRandom::FRandRange(0.6f, 0.8f), 700.0f);
	if (Velocity.Size() > 700.0f)
	{
		Velocity *= 0.8f;
	}
	// PlayGibSound (WOTCreatureChunk.uc:115-134): above MinGibSoundspeed 100, the
	// part's own bounce sound (bones) or a random GibBounce1-5, vol/pitch perturbed
	// +/-30% (sound 5c).
	if (Velocity.Size() > 100.0f && GetWorld())
	{
		const FName Pick = BounceSound.IsNone()
		    ? FName(*FString::Printf(TEXT("WOT/GibBounce%d"), WoTRandom::RandRange(1, 5)))
		    : BounceSound;
		WoTSound::PlayReplicated(*GetWorld(), Pick, GetActorLocation(), 1600.0f,
		                         WoTRandom::FRandRange(0.7f, 1.3f),
		                         WoTRandom::FRandRange(0.7f, 1.3f));
	}
	if (Velocity.Size() < MinBounceSpeed)
	{
		bBounce = false;
	}
}

void AWoTCreatureChunk::Landed(const FVector& HitNormal)
{
#if WITH_AUTOMATION_TESTS
	++LandedCountForTesting;
	LastPreLandedRotationForTesting = GetActorRotation();
	LastLandedNormalForTesting = HitNormal;
#endif
	FRotator FinalRotation = GetActorRotation();
	if (bZeroYaw) ZeroRotatorAxis(FinalRotation.Yaw);
	if (bZeroRoll) ZeroRotatorAxis(FinalRotation.Roll);
	if (bZeroPitch) ZeroRotatorAxis(FinalRotation.Pitch);
	const FVector SurfaceDirection =
		(FVector::UpVector - HitNormal).GetSafeNormal();
	if (!SurfaceDirection.IsNearlyZero())
	{
		FinalRotation += SurfaceDirection.Rotation();
	}
	SetActorRotation(FinalRotation);
	Velocity = FVector::ZeroVector;
	TumbleRate = FRotator::ZeroRotator;
	bFalling = false;
	bBounce = true;
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ForceNetUpdate();
}

float AWoTCreatureChunk::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                    AController* EventInstigator, AActor* DamageCauser)
{
	// WOTCreatureChunk.TakeDamage (:303): a solid hit (or accumulation past Mass)
	// destroys the chunk. (Chunk-shooting is disabled in shipped MP — the port keeps
	// chunks shootable server-side; revisit with the 4e decal parity, PORT_NOTES.)
	Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	if (!HasAuthority() || !bTakesDamage || DamageAmount <= 0.0f)
	{
		return 0.0f;
	}
	CumulativeDamage += DamageAmount;
	if (DamageAmount > FMath::Min(15.0f, Mass) || CumulativeDamage > Mass)
	{
		Destroy();
	}
	return DamageAmount;
}
