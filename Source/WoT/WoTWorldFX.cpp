#include "WoTWorldFX.h"
#include "WoTRandom.h"

#include "WoTParticleSprayer.h"
#include "WoTSound.h"
#include "WoTSoundTable.h"
#include "WoTVertexAnimComponent.h"
#include "WoTZoneVolume.h"
#include "Components/PointLightComponent.h"
#include "Components/CapsuleComponent.h"
#include "Assets/WoTRetailIdentity.h"
#include "Scalability.h"
#include "Engine/PointLight.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AWoTLightning::AWoTLightning()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("LightningLight"));
	SetRootComponent(Light);
	Light->SetCastShadows(false);
	Light->SetUseInverseSquaredFalloff(false);
	Light->SetAttenuationRadius(6400.0f);
	Light->SetIntensity(0.0f);
}

void AWoTLightning::BeginPlay()
{
	Super::BeginPlay();
	if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		Events->RegisterTag(TagName, this);
	Light->SetVisibility(false);
	if (UPointLightComponent* Presentation = ResolvePresentationLight())
		Presentation->SetVisibility(false);
}

UPointLightComponent* AWoTLightning::ResolvePresentationLight()
{
	if (LightTag.IsNone()) return Light;
	if (TaggedLight.IsValid()) return TaggedLight.Get();
	for (TActorIterator<APointLight> It(GetWorld()); It; ++It)
		if (It->ActorHasTag(LightTag) && IsValid(It->PointLightComponent))
		{
			TaggedLight = It->PointLightComponent;
			PresentationPeakIntensity = It->PointLightComponent->Intensity;
			return TaggedLight.Get();
		}
	return Light;
}

void AWoTLightning::WoTTrigger(AActor* Other, APawn* InstigatorPawn)
{
	if (!EventName.IsNone())
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
			Events->FireEvent(EventName, Other, InstigatorPawn);
	if (HasAuthority()) { ++StrikeSerial; ForceNetUpdate(); }
	StartStrike();
}

void AWoTLightning::StartStrike()
{
	StrikeAge = 0.0f; bThundered = false; bLightFinished = false;
	if (UPointLightComponent* Presentation = ResolvePresentationLight())
		if (GetNetMode() != NM_DedicatedServer) Presentation->SetVisibility(true);
}

void AWoTLightning::OnRep_Strike() { StartStrike(); }

void AWoTLightning::Tick(float Dt)
{
	Super::Tick(Dt);
	if (StrikeAge < 0.0f) return;
	StrikeAge += Dt;
	UPointLightComponent* Presentation = ResolvePresentationLight();
	if (!bLightFinished && Presentation)
	{
		if (!LightTag.IsNone())
		{
			// Lightning.uc AltStage1: reuse the first tagged authored light as a
			// flicker for LightningLength seconds, then restore LT_None.
			if (StrikeAge < LightningLength)
				Presentation->SetIntensity(PresentationPeakIntensity
					* WoTRandom::FRandRange(0.25f, 1.0f));
			else bLightFinished = true;
		}
		else
		{
			// Lightning.uc Stage1..4: half flash, decay, full flash, decay.
			const float Unit = FMath::Max(0.001f, 255.0f / BrightnessPerSecond);
			float Byte = 0.0f;
			if (StrikeAge < Unit * HalfLightPct) Byte = BrightnessPerSecond * StrikeAge;
			else if (StrikeAge < Unit * (HalfLightPct * 2.0f))
				Byte = 255.0f * HalfLightPct - BrightnessPerSecond
					* (StrikeAge - Unit * HalfLightPct);
			else if (StrikeAge < Unit * (HalfLightPct * 2.0f + 1.0f))
				Byte = BrightnessPerSecond
					* (StrikeAge - Unit * HalfLightPct * 2.0f);
			else Byte = 255.0f - BrightnessPerSecond
				* (StrikeAge - Unit * (HalfLightPct * 2.0f + 1.0f));
			Presentation->SetIntensity(FMath::Square(
				FMath::Clamp(Byte, 0.0f, 255.0f) / 255.0f)
				* PresentationPeakIntensity);
			bLightFinished = Byte <= 0.0f
				&& StrikeAge > Unit * (HalfLightPct * 2.0f + 1.0f);
		}
		if (bLightFinished)
		{
			Presentation->SetIntensity(0.0f);
			Presentation->SetVisibility(false);
		}
	}
	if (!bThundered && StrikeAge >= ThunderDelay)
	{
		bThundered = true;
		if (GetNetMode() != NM_DedicatedServer)
		if (USoundBase* Sound = WoTSound::LoadBySubPath(ThunderSound))
			WoTSound::PlayOneShot(this, Sound, GetActorLocation(), ThunderRadius);
	}
	if (bLightFinished && bThundered) StrikeAge = -1.0f;
}

void AWoTLightning::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTLightning, StrikeSerial);
	DOREPLIFETIME(AWoTLightning, ThunderSound);
	DOREPLIFETIME(AWoTLightning, ThunderDelay);
	DOREPLIFETIME(AWoTLightning, HalfLightPct);
	DOREPLIFETIME(AWoTLightning, LightTag);
}

AWoTRandomLightning::AWoTRandomLightning() { SetReplicates(true); }
void AWoTRandomLightning::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority()) NextStrike = GetWorld()->GetTimeSeconds()
		+ WoTRandom::FRandRange(MinLightningTime, MaxLightningTime);
}

AWoTDistanceLightning::AWoTDistanceLightning()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true; bAlwaysRelevant = true;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("DistanceLightningLight"));
	SetRootComponent(Light); Light->SetCastShadows(false);
	Light->SetUseInverseSquaredFalloff(false); Light->SetAttenuationRadius(5600.0f);
	Light->SetIntensity(0.0f);
}
void AWoTDistanceLightning::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode()==NM_DedicatedServer) Light->SetVisibility(false);
	if (HasAuthority()) ChangeAt=GetWorld()->GetTimeSeconds()+WoTRandom::FRandRange(5.0f,15.0f);
	OnRep_Flash();
}
void AWoTDistanceLightning::Tick(float)
{
	if (HasAuthority() && GetWorld()->GetTimeSeconds()>=ChangeAt)
	{
		bFlashing=!bFlashing; ChangeAt=GetWorld()->GetTimeSeconds()
			+(bFlashing?WoTRandom::FRandRange(0.4f,0.45f):WoTRandom::FRandRange(9.0f,29.0f));
		OnRep_Flash(); ForceNetUpdate();
	}
}
void AWoTDistanceLightning::OnRep_Flash()
{
	Light->SetIntensity(bFlashing?5.0f:0.0f);
	if(GetNetMode()!=NM_DedicatedServer) Light->SetVisibility(bFlashing);
}
void AWoTDistanceLightning::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTDistanceLightning,bFlashing);
}
void AWoTRandomLightning::Tick(float Dt)
{
	AWoTLightning::Tick(Dt);
	if (HasAuthority() && GetWorld()->GetTimeSeconds() >= NextStrike)
	{
		ThunderDelay = WoTRandom::FRandRange(MinThunderDelay, MaxThunderDelay);
		HalfLightPct = WoTRandom::FRandRange(0.1f, 0.7f);
		NextStrike = GetWorld()->GetTimeSeconds()
			+ WoTRandom::FRandRange(MinLightningTime, MaxLightningTime);
		static const FName ThunderSounds[] = {
			TEXT("Environment/Thunder9"), TEXT("Environment/Thunder3"),
			TEXT("Environment/Thunder10"), TEXT("Environment/Thunder2"),
			TEXT("Environment/Thunder4"), TEXT("Environment/Thunder8") };
		int32 SoundIndex = 0;
		for (; SoundIndex < UE_ARRAY_COUNT(ThunderSounds) - 1; ++SoundIndex)
		{
			const float DelayBoundary = MinThunderDelay
				+ static_cast<float>(SoundIndex + 1)
				* (MaxThunderDelay - MinThunderDelay)
				/ static_cast<float>(UE_ARRAY_COUNT(ThunderSounds));
			if (DelayBoundary >= ThunderDelay) break;
		}
		const FName SelectedSound = ThunderSounds[SoundIndex];
		if (bSpawnsLightning)
		{
			const FTransform Transform(GetActorRotation(), GetActorLocation());
			AWoTLightning* Spawned = GetWorld()->SpawnActorDeferred<AWoTLightning>(
				AWoTLightning::StaticClass(), Transform, this, GetInstigator(),
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Spawned)
			{
				Spawned->TagName = EventName;
				Spawned->LightTag = LightTag;
				Spawned->ThunderDelay = ThunderDelay;
				Spawned->HalfLightPct = HalfLightPct;
				Spawned->ThunderSound = SelectedSound;
				Spawned->SetLifeSpan(ThunderDelay + 30.0f);
				Spawned->FinishSpawning(Transform);
			}
		}
		for (TActorIterator<AWoTLightning> It(GetWorld()); It; ++It)
			if (*It != this && It->TagName == EventName)
			{
				It->ThunderDelay = ThunderDelay;
				It->HalfLightPct = HalfLightPct;
				It->ThunderSound = SelectedSound;
			}
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
			Events->FireEvent(EventName, this, GetInstigator());
	}
}

AWoTWind::AWoTWind() { PrimaryActorTick.bCanEverTick = true; bReplicates = true; }
void AWoTWind::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority()) bOn = bInitiallyOn;
	NextImpulse = 0.0f;
	if (UWoTEventSubsystem* E = GetWorld()->GetSubsystem<UWoTEventSubsystem>()) E->RegisterTag(TagName, this);
}
void AWoTWind::ApplySpeed(float Speed)
{
	const FVector NewWind = GetActorForwardVector() * Speed;
	for (TActorIterator<AWoTParticleSprayer> It(GetWorld()); It; ++It)
		if (It->TagName == EventName && !It->bIsWindResistant)
			It->Gravity += NewWind - LastWind;
	LastWind = NewWind;
}
void AWoTWind::SetEnabled(bool bNewOn)
{
	if (bOn == bNewOn) return;
	bOn = bNewOn;
	OnRep_Enabled();
	if (HasAuthority()) ForceNetUpdate();
}
void AWoTWind::OnRep_Enabled()
{
	// TriggerTimed is an edge+server-time protocol. Its intermediate bOn level
	// may coalesce with the restore and must not shorten or erase the client pulse.
	if (!HasAuthority() && InitialState == TEXT("TriggerTimed"))
	{
		if (!bOn)
		{
			if (bHoldExpiredTimedPulse)
			{
				// An already-expired edge is still presented for one update. A
				// coalesced authoritative restore cannot overwrite it at receipt.
				bOn = !bInitiallyOn;
			}
			else
			{
				RestoreAt = -1.0f;
				NextImpulse = 0.0f;
				ApplySpeed(0.0f);
			}
		}
		return;
	}
	// Wind.uc runs the impulse timer independently on every renderer. Replication
	// carries only the authoritative edge; each client retains its own random
	// cadence and speed picks once enabled.
	NextImpulse = 0.0f;
	if (!bOn) ApplySpeed(0.0f);
}
void AWoTWind::OnRep_TimedTrigger()
{
	ApplyTimedTriggerReplication();
}

void AWoTWind::ApplyTimedTriggerReplication()
{
	if (HasAuthority() || InitialState != TEXT("TriggerTimed")
		|| TimedTriggerSerial == 0) return;
	if (bHasAppliedTimedTriggerPair
		&& LastAppliedTimedTriggerSerial == TimedTriggerSerial
		&& LastAppliedTimedTriggerServerTime == TimedTriggerServerTime)
	{
		return;
	}
	bHasAppliedTimedTriggerPair = true;
	LastAppliedTimedTriggerSerial = TimedTriggerSerial;
	LastAppliedTimedTriggerServerTime = TimedTriggerServerTime;
#if WITH_AUTOMATION_TESTS
	++TimedTriggerObserveCountForTesting;
#endif
	const AGameStateBase* GameState = GetWorld()->GetGameState();
	const float ServerNow = GameState
		? GameState->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
	const float Remaining = FMath::Clamp(
		TimerDuration - FMath::Max(0.0f, ServerNow - TimedTriggerServerTime),
		0.0f, TimerDuration);
	bOn = !bInitiallyOn;
	NextImpulse = 0.0f;
	if (!bOn) ApplySpeed(0.0f);
	bHoldExpiredTimedPulse = Remaining <= 0.0f;
	ExpiredTimedPulseUpdatesRemaining = bHoldExpiredTimedPulse ? 1 : 0;
	RestoreAt = bHoldExpiredTimedPulse
		? -1.0f : GetWorld()->GetTimeSeconds() + Remaining;
}

void AWoTWind::RestoreTimedClientState()
{
	bHoldExpiredTimedPulse = false;
	ExpiredTimedPulseUpdatesRemaining = 0;
	RestoreAt = -1.0f;
	bOn = bInitiallyOn;
	NextImpulse = 0.0f;
	if (!bOn) ApplySpeed(0.0f);
}
void AWoTWind::Tick(float)
{
	const float Now = GetWorld()->GetTimeSeconds();
	if (!HasAuthority() && InitialState == TEXT("TriggerTimed")
		&& bHoldExpiredTimedPulse)
	{
		if (ExpiredTimedPulseUpdatesRemaining == 0)
		{
			RestoreTimedClientState();
		}
		else
		{
			--ExpiredTimedPulseUpdatesRemaining;
		}
	}
	else if (RestoreAt >= 0 && Now >= RestoreAt)
	{
		RestoreAt = -1;
		if (!HasAuthority() && InitialState == TEXT("TriggerTimed"))
		{
			RestoreTimedClientState();
		}
		else
		{
			SetEnabled(bInitiallyOn);
		}
	}
	if (bOn && Now >= NextImpulse)
	{
		ApplySpeed(WoTRandom::FRandRange(MinWindspeed, MaxWindspeed));
		NextImpulse = Now + FMath::Max(0.01f, WoTRandom::FRandRange(MinImpulseTime, MaxImpulseTime));
	}
}
void AWoTWind::WoTTrigger(AActor*, APawn*)
{
	if (!HasAuthority()) return;
	if (InitialState == TEXT("TriggerControl"))
	{
		SetEnabled(!bInitiallyOn);
	}
	else if (InitialState == TEXT("TriggerTimed"))
	{
		SetEnabled(!bInitiallyOn);
		RestoreAt = GetWorld()->GetTimeSeconds() + TimerDuration;
		const AGameStateBase* GameState = GetWorld()->GetGameState();
		TimedTriggerServerTime = GameState
			? GameState->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
		// Zero is the CDO's untriggered sentinel. Timed edges wrap 255 -> 1 so a
		// late joiner can never confuse a live pulse with untouched defaults.
		TimedTriggerSerial = TimedTriggerSerial == MAX_uint8
			? 1 : static_cast<uint8>(TimedTriggerSerial + 1);
		ForceNetUpdate();
	}
	else
	{
		SetEnabled(!bOn);
	}
}
void AWoTWind::WoTUnTrigger(AActor*, APawn*) { if (InitialState == TEXT("TriggerControl")) SetEnabled(bInitiallyOn); }
void AWoTWind::EndPlay(EEndPlayReason::Type R) { ApplySpeed(0); Super::EndPlay(R); }
void AWoTWind::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTWind, TagName);
	DOREPLIFETIME(AWoTWind, EventName);
	DOREPLIFETIME(AWoTWind, InitialState);
	DOREPLIFETIME(AWoTWind, MinImpulseTime);
	DOREPLIFETIME(AWoTWind, MaxImpulseTime);
	DOREPLIFETIME(AWoTWind, MinWindspeed);
	DOREPLIFETIME(AWoTWind, MaxWindspeed);
	DOREPLIFETIME(AWoTWind, TimerDuration);
	DOREPLIFETIME(AWoTWind, bInitiallyOn);
	DOREPLIFETIME(AWoTWind, bOn);
	DOREPLIFETIME(AWoTWind, TimedTriggerSerial);
	DOREPLIFETIME(AWoTWind, TimedTriggerServerTime);
}

AWoTSpewChunk::AWoTSpewChunk()
{
	PrimaryActorTick.bCanEverTick = true; bReplicates = true; SetReplicatingMovement(true);
	CollisionCapsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CollisionCapsule"));
	SetRootComponent(CollisionCapsule);
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(CollisionCapsule);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}
void AWoTSpewChunk::BeginPlay()
{
	Super::BeginPlay(); DisplayMesh->SetVertexMesh(MeshName); DisplayMesh->SetRelativeScale3D(FVector(MeshScale));
	CollisionCapsule->SetCapsuleSize(CollisionRadius, CollisionHeight);
	CollisionCapsule->SetCollisionEnabled(bCollision
		? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	// BounceableDecoration.uc:183-190 collides with the world while leaving
	// bBlockActors/bBlockPlayers false.  A bare UE5 capsule's inherited profile
	// is not a stable equivalent, so spell out that retail response matrix.
	CollisionCapsule->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionCapsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionCapsule->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	for (int32 Slot = 0; Slot < SkinIdentities.Num(); ++Slot)
	{
		FWoTRetailObjectPath Path;
		if (WoTParseRetailObjectPath(SkinIdentities[Slot].ToString(), Path))
		{
			DisplayMesh->SetSkinOverride(Slot, FSoftObjectPath(FString::Printf(
				TEXT("/Game/WoT/Textures/%s.%s"),
				*WoTRetailTextureSubPath(Path), *Path.Name)));
		}
	}
	const int32 LocalDetail = Scalability::GetQualityLevels().EffectsQuality;
	DisplayMesh->SetVisibility(GetNetMode() != NM_DedicatedServer
		&& DetailLevel <= LocalDetail);
	Velocity = InitialVelocity; TumbleRate = FRotator(WoTRandom::FRandRange(-700.0f,700.0f),WoTRandom::FRandRange(-700.0f,700.0f),WoTRandom::FRandRange(-700.0f,700.0f));
	if (!bTumble) TumbleRate = FRotator::ZeroRotator;
	if (AuthoredLifeSpan > 0.0f) SetLifeSpan(AuthoredLifeSpan);
}
void AWoTSpewChunk::Tick(float Dt)
{
	Super::Tick(Dt); if (!HasAuthority()) return;
	Velocity += AWoTZoneVolume::GetGravityAt(GetWorld(), GetActorLocation()) * Dt;
	FHitResult Hit; AddActorWorldOffset(Velocity * Dt, true, &Hit);
	AddActorLocalRotation(TumbleRate * Dt);
	if (Hit.bBlockingHit && bBounce)
	{
		const float Speed = Velocity.Size();
#if WITH_AUTOMATION_TESTS
		++BlockingHitCountForTesting;
		LastPreImpactVelocityForTesting = Velocity;
		LastImpactNormalForTesting = Hit.ImpactNormal;
#endif
		if (!bSilent && (bFirstHit || Speed > MinBounceSoundSpeed)
			&& !LandSound.IsNone())
		{
#if WITH_AUTOMATION_TESTS
			++BounceSoundCountForTesting;
#endif
			FString SoundPath = LandSound.ToString();
			SoundPath.ReplaceCharInline(TEXT('.'), TEXT('/'));
			const float Volume = BaseBounceVolume * WoTRandom::FRandRange(
				1.0f - VolumePerturbPercent / 100.0f,
				1.0f + VolumePerturbPercent / 100.0f);
			const float Pitch = BaseBouncePitch * WoTRandom::FRandRange(
				1.0f - PitchPerturbPercent / 100.0f,
				1.0f + PitchPerturbPercent / 100.0f);
			WoTSound::PlayReplicated(*GetWorld(), FName(*SoundPath),
				Hit.ImpactPoint, 1600.0f, Volume, Pitch);
		}
		if (bFirstHit && Speed < MinFirstHitSpeed)
		{
			bFirstHit = false;
			TumbleRate = FRotator::ZeroRotator;
			SetActorRotation(FRotator(0.0f,
				WoTRandom::FRandRange(0.0f, 360.0f), 0.0f));
		}
		TumbleRate *= RotationDampeningRatio;
		Velocity = SpeedDampeningRatio * Velocity.MirrorByVector(Hit.ImpactNormal);
#if WITH_AUTOMATION_TESTS
		LastPostImpactVelocityForTesting = Velocity;
#endif
		if (Velocity.Size() < MinBounceSpeed)
		{
			bBounce = false;
			Velocity = FVector::ZeroVector;
		}
	}
}
void AWoTSpewChunk::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTSpewChunk, MeshName);
	DOREPLIFETIME(AWoTSpewChunk, ClassIdentity);
	DOREPLIFETIME(AWoTSpewChunk, SkinIdentities);
	DOREPLIFETIME(AWoTSpewChunk, MeshScale);
	DOREPLIFETIME(AWoTSpewChunk, InitialVelocity);
	DOREPLIFETIME(AWoTSpewChunk, bTumble);
	DOREPLIFETIME(AWoTSpewChunk, bCollision);
	DOREPLIFETIME(AWoTSpewChunk, bSilent);
	DOREPLIFETIME(AWoTSpewChunk, DetailLevel);
	DOREPLIFETIME(AWoTSpewChunk, CollisionRadius);
	DOREPLIFETIME(AWoTSpewChunk, CollisionHeight);
	DOREPLIFETIME(AWoTSpewChunk, AuthoredLifeSpan);
	DOREPLIFETIME(AWoTSpewChunk, MinBounceSoundSpeed);
	DOREPLIFETIME(AWoTSpewChunk, MinFirstHitSpeed);
	DOREPLIFETIME(AWoTSpewChunk, MinBounceSpeed);
	DOREPLIFETIME(AWoTSpewChunk, SpeedDampeningRatio);
	DOREPLIFETIME(AWoTSpewChunk, RotationDampeningRatio);
	DOREPLIFETIME(AWoTSpewChunk, BaseBounceVolume);
	DOREPLIFETIME(AWoTSpewChunk, BaseBouncePitch);
	DOREPLIFETIME(AWoTSpewChunk, VolumePerturbPercent);
	DOREPLIFETIME(AWoTSpewChunk, PitchPerturbPercent);
	DOREPLIFETIME(AWoTSpewChunk, LandSound);
}

AWoTSpewTrigger::AWoTSpewTrigger() { PrimaryActorTick.bCanEverTick = true; bReplicates = true; }
void AWoTSpewTrigger::BeginPlay()
{
	Super::BeginPlay(); Remaining = SpewCount; if (UWoTEventSubsystem* E=GetWorld()->GetSubsystem<UWoTEventSubsystem>()) E->RegisterTag(TagName,this);
}
void AWoTSpewTrigger::SetEnabled(bool V) { bEnabled=V; Remaining=SpewCount; NextSpew=0; }
void AWoTSpewTrigger::WoTTrigger(AActor*, APawn*)
{
	if (InitialState==TEXT("TriggerTurnsOn")) SetEnabled(true); else if (InitialState==TEXT("TriggerTurnsOff")) SetEnabled(false); else SetEnabled(!bEnabled);
}
void AWoTSpewTrigger::WoTUnTrigger(AActor*, APawn*) { if (InitialState==TEXT("TriggerControl")) SetEnabled(!bEnabled); }
void AWoTSpewTrigger::Tick(float)
{
	if (!HasAuthority() || !bEnabled || GetWorld()->GetTimeSeconds() < NextSpew) return;
	NextSpew=GetWorld()->GetTimeSeconds()+TimeBetweenSpewsSecs; Spew();
	if (SpewCount != 0 && --Remaining <= 0) bEnabled=false;
}
void AWoTSpewTrigger::Spew()
{
	for (const FWoTSpewThing& Thing : Things) for (int32 I=0; I<Thing.Count; ++I)
	{
		const FRotator R=bInheritVelocity ? GetActorRotation()+FRotator(WoTRandom::FRandRange(-SpewRotationSpread.Pitch,SpewRotationSpread.Pitch),WoTRandom::FRandRange(-SpewRotationSpread.Yaw,SpewRotationSpread.Yaw),WoTRandom::FRandRange(-SpewRotationSpread.Roll,SpewRotationSpread.Roll)) : WoTRandom::VRand().Rotation();
		FTransform X(R,GetActorLocation()); AWoTSpewChunk* C=GetWorld()->SpawnActorDeferred<AWoTSpewChunk>(AWoTSpewChunk::StaticClass(),X,this,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (C) { C->ClassIdentity=Thing.ClassIdentity; C->MeshName=Thing.MeshName;
			C->SkinIdentities=Thing.SkinIdentities; C->MeshScale=Thing.DrawScale==0?1:Thing.DrawScale;
			C->InitialVelocity=R.Vector()*WoTRandom::FRandRange(Thing.MinSpeed,Thing.MaxSpeed);
			C->bTumble=bTumble; C->bCollision=bCollision; C->bSilent=bSilent;
			C->DetailLevel=Thing.DetailLevel; C->CollisionRadius=Thing.CollisionRadius;
			C->CollisionHeight=Thing.CollisionHeight; C->AuthoredLifeSpan=Thing.LifeSpan;
			C->MinBounceSoundSpeed=Thing.MinBounceSoundSpeed;
			C->MinFirstHitSpeed=Thing.MinFirstHitSpeed; C->MinBounceSpeed=Thing.MinBounceSpeed;
			C->SpeedDampeningRatio=Thing.SpeedDampeningRatio;
			C->RotationDampeningRatio=Thing.RotationDampeningRatio; C->LandSound=Thing.LandSound;
			C->BaseBounceVolume=Thing.BaseBounceVolume; C->BaseBouncePitch=Thing.BaseBouncePitch;
			C->VolumePerturbPercent=Thing.VolumePerturbPercent;
			C->PitchPerturbPercent=Thing.PitchPerturbPercent;
			C->SetNetCullDistanceSquared(FMath::Square(FMath::Max(SpewVisibilityHeight,SpewVisibilityRadius))); C->FinishSpawning(X); }
	}
}
