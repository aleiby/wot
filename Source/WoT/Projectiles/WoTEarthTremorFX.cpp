#include "WoTEarthTremorFX.h"
#include "../WoTRandom.h"
#include "../AI/WoTPathNodeIterator.h"

#include "../Inventory/WoTEarthTremorLeech.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../WoTCarcass.h"
#include "../WoTAmbientSound.h"
#include "../WoTCharacter.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTSound.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"

#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Engine/SkeletalMesh.h"

AWoTETSoundProxy::AWoTETSoundProxy()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	LoopAudio = CreateDefaultSubobject<UWoTAmbientSoundComponent>(TEXT("LoopET"));
	LoopAudio->SetupAttachment(RootComponent);
	LoopAudio->bAutoActivate = false;
	InitialLifeSpan = 15.0f;
	bReplicates = false;
}

void AWoTETSoundProxy::BeginPlay()
{
	Super::BeginPlay();
	SetLifeSpan(15.0f);
	FullLightIntensity = WoTUE1LightRegistry::Configure(
		Light, TEXT("ETSoundProxy")) ? Light->Intensity : 0.0f;
	if (USoundBase* Loop = WoTSound::LoadBySubPath(TEXT("Angreal/LoopET"), true))
	{
		LoopAudio->ConfigureUE1Ambient(Loop, 83, 255, 50);
		LoopAudio->Play();
	}
}

void AWoTETSoundProxy::AddRock(AWoTEarthTremorRock* Rock)
{
	if (IsValid(Rock) && Rocks.Num() < 32)
	{
		Rocks.Add(Rock);
		bHadRock = true;
	}
}

void AWoTETSoundProxy::RemoveRock(AWoTEarthTremorRock* Rock)
{
	Rocks.RemoveAll([Rock](const TWeakObjectPtr<AWoTEarthTremorRock>& R)
	{
		return !R.IsValid() || R.Get() == Rock;
	});
	if (bHadRock && Rocks.IsEmpty()) { Destroy(); }
}

void AWoTETSoundProxy::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Rocks.RemoveAll([](const TWeakObjectPtr<AWoTEarthTremorRock>& R)
	{
		return !R.IsValid();
	});
	if (bHadRock && Rocks.IsEmpty()) { Destroy(); return; }
	FadeScalar = FMath::Min(1.0f, GetLifeSpan() / 15.0f * 10.0f);
	Light->SetIntensity(FullLightIntensity * FadeScalar);
	LoopAudio->SetVolumeMultiplier(LoopAudio->BaseVolumeMultiplier * FadeScalar);
}

AWoTEarthTremorChunk::AWoTEarthTremorChunk()
{
	MinChunkSpeed = 400.0f;
	MaxChunkSpeed = 600.0f;
	SprayerConfig = TEXT("BlackSmoke01");
	bLavaRockVisual = true;
}

AWoTEarthTremorRock::AWoTEarthTremorRock()
{
	PrimaryActorTick.bCanEverTick = true;
	RockMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("RockMesh"));
	RockMesh->SetupAttachment(RootComponent);
	RockMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RockCollision = CreateDefaultSubobject<UCapsuleComponent>(TEXT("RockCollision"));
	RockCollision->SetupAttachment(RootComponent);
	RockCollision->InitCapsuleSize(30.0f, 46.0f);
	RockCollision->SetCollisionResponseToAllChannels(ECR_Overlap);
	RockCollision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
	RockCollision->OnComponentBeginOverlap.AddDynamic(this,
		&AWoTEarthTremorRock::OnRockBeginOverlap);
	RockCollision->OnComponentEndOverlap.AddDynamic(this,
		&AWoTEarthTremorRock::OnRockEndOverlap);
	ProjectileMovement->Deactivate();
	ProjectileMovement->bAutoActivate = false;
	bReplicates = false;
	InitialLifeSpan = 15.0f;
}

void AWoTEarthTremorRock::InitializeRock(bool bGameplay, bool bInnerRock,
	float Scale, AWoTETSoundProxy* InProxy)
{
	bAuthorityGameplay = bGameplay;
	bInner = bInnerRock;
	AuthoredScale = Scale;
	SoundProxy = InProxy;
}

void AWoTEarthTremorRock::BeginPlay()
{
	Super::BeginPlay();
	SetLifeSpan(15.0f);
	// EarthTremor.uc:123-130 registers the center rock before Rock.Go(), and
	// EarthTremorRock.uc:82-89 removes it from Destroyed. Register before the
	// ground trace so failed placement can tear the otherwise-empty proxy down.
	if (SoundProxy) { SoundProxy->AddRock(this); }
	RockCollision->SetCollisionEnabled(bAuthorityGameplay
		? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	const FName MeshName = bInner ? FName(TEXT("ETinner"))
		: FName(TEXT("EarthTremorRock"));
	if (RockMesh->SetVertexMesh(MeshName))
	{
		RockMesh->SetRelativeScale3D(FVector((bInner ? 3.0f : 4.0f) * AuthoredScale));
	}

	const FVector Up = GetActorUpVector();
	FHitResult Hit;
	// EarthTremorRock.uc:108 Trace(..., false): TRACE_VisBlocking sees only
	// level and movers.
	if (!WoTVisBlockingTrace(GetWorld(), GetActorLocation() + Up * 50.0f,
		GetActorLocation() - Up * 300.0f, this, Hit))
	{
		Destroy();
		return;
	}
	RelativeUp = Hit.ImpactNormal.GetSafeNormal();
	const FVector Forward = FVector::VectorPlaneProject(GetActorForwardVector(), RelativeUp)
		.GetSafeNormal();
	SetActorRotation(FRotationMatrix::MakeFromXZ(
		Forward.IsNearlyZero() ? FVector::ForwardVector : Forward, RelativeUp).Rotator());
	SetActorLocation(Hit.ImpactPoint);
	NextAnimationTime = GetWorld()->GetTimeSeconds() + WoTRandom::FRandRange(0.0f, 0.2f);
	NextSpewTime = GetWorld()->GetTimeSeconds() + WoTRandom::FRandRange(0.0f, 10.0f);
}

void AWoTEarthTremorRock::SpawnLavaChunks()
{
	AWoTBurningChunk::SpawnFan(*GetWorld(), AWoTEarthTremorChunk::StaticClass(),
		GetActorLocation(), RelativeUp, this);
}

void AWoTEarthTremorRock::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now >= NextAnimationTime && GetLifeSpan() > 37.0f / (30.0f * 0.9f))
	{
		RockMesh->PlayAnim(TEXT("All"), WoTRandom::FRandRange(0.9f, 1.2f));
		NextAnimationTime = Now + AnimationFrequency;
		AnimationFrequency += 0.1f;
	}
	if (Now >= NextSpewTime)
	{
		const float Probability = WoTRandom::FRand();
		if (Probability < 0.1f) { SpawnLavaChunks(); }
		else if (Probability < 0.5f)
		{
			WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("LavaSpew"),
				GetActorLocation() + RelativeUp * 8.0f, RelativeUp.Rotation());
		}
		NextSpewTime = Now + GetLifeSpan() + 100.0f;
	}
	if (!bAuthorityGameplay) { return; }
	DamageTimer -= DeltaSeconds;
	if (DamageTimer <= 0.0f)
	{
		DamageTimer += 0.2f;
		TArray<AActor*> Touching;
		GetOverlappingActors(Touching);
		for (AActor* Other : Touching)
		{
			if (!IsValid(Other) || Other == this || Other->IsA<APawn>()) { continue; }
			FWoTDamageEvent Ev;
			Ev.WoTDamageType = SourceAngreal
				? AWoTAngrealInventory::GetDamageType(SourceAngreal) : FName(TEXT("xEFxx"));
			Ev.UE1Momentum = RelativeUp * 10000.0f;
			Ev.HitInfo.ImpactPoint = GetActorLocation();
			Other->TakeDamage(5.0f, Ev,
				GetInstigator() ? GetInstigator()->GetController() : nullptr, this);
		}
	}
}

void AWoTEarthTremorRock::TouchActor(AActor* Other)
{
	APawn* Pawn = bAuthorityGameplay ? Cast<APawn>(Other) : nullptr;
	if (!IsValid(Pawn) || Pawn == IgnoredPawn) { return; }
	TArray<UWoTEarthTremorLeech*> Existing;
	Pawn->GetComponents<UWoTEarthTremorLeech>(Existing);
	for (UWoTEarthTremorLeech* Leech : Existing)
	{
		if (IsValid(Leech)) { Leech->Unattach(); }
	}
	UWoTEarthTremorLeech* Leech = NewObject<UWoTEarthTremorLeech>(Pawn);
	if (!Leech) { return; }
	Leech->InitializeWithProjectile(this);
	Leech->Lifespan = GetLifeSpan();
	Leech->Momentum = RelativeUp * 10000.0f;
	Leech->DamagePerHit = 5.0f;
	Leech->AttachTo(Pawn);
}

void AWoTEarthTremorRock::UnTouchActor(AActor* Other)
{
	APawn* Pawn = Cast<APawn>(Other);
	if (!IsValid(Pawn)) { return; }
	TArray<UWoTEarthTremorLeech*> Leeches;
	Pawn->GetComponents<UWoTEarthTremorLeech>(Leeches);
	for (UWoTEarthTremorLeech* Leech : Leeches)
	{
		if (IsValid(Leech) && Leech->SourceProjectile == this) { Leech->Unattach(); }
	}
}

void AWoTEarthTremorRock::OnRockBeginOverlap(UPrimitiveComponent*, AActor* Other,
	UPrimitiveComponent*, int32, bool, const FHitResult&) { TouchActor(Other); }
void AWoTEarthTremorRock::OnRockEndOverlap(UPrimitiveComponent*, AActor* Other,
	UPrimitiveComponent*, int32) { UnTouchActor(Other); }

void AWoTEarthTremorRock::Destroyed()
{
	if (SoundProxy) { SoundProxy->RemoveRock(this); }
	Super::Destroyed();
}

AWoTEarthTremor::AWoTEarthTremor()
{
	PrimaryActorTick.bCanEverTick = true;
	SetActorHiddenInGame(true);
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProjectileMovement->Deactivate();
	ProjectileMovement->bAutoActivate = false;
	bReplicates = true;
	bAlwaysRelevant = true;
	bNetTemporary = true;
	NetPriority = 6.0f;
	InitialLifeSpan = 15.0f;
}

void AWoTEarthTremor::BeginPlay()
{
	Super::BeginPlay();
	SetLifeSpan(15.0f);
	TArray<AWoTEarthTremorRock*> OldRocks;
	for (TActorIterator<AWoTEarthTremorRock> It(GetWorld()); It; ++It)
	{
		if (FVector::Dist(It->GetActorLocation(), GetActorLocation()) <= 280.0f)
		{
			OldRocks.Add(*It);
		}
	}
	for (AWoTEarthTremorRock* Rock : OldRocks)
	{
		if (IsValid(Rock)) { Rock->Destroy(); }
	}
	Go();
}

void AWoTEarthTremor::Go()
{
	if (bRunning) { return; }
	bRunning = true;
	FActorSpawnParameters P;
	P.Owner = this;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SoundProxy = GetWorld()->SpawnActor<AWoTETSoundProxy>(
		AWoTETSoundProxy::StaticClass(), GetActorLocation(), GetActorRotation(), P);
	SpawnCenter();
}

AWoTEarthTremorRock* AWoTEarthTremor::SpawnRock(const FVector& Location,
	const FRotator& Rotation, float Scale, bool bInner)
{
	const FTransform At(Rotation, Location);
	AWoTEarthTremorRock* Rock = GetWorld()->SpawnActorDeferred<AWoTEarthTremorRock>(
		AWoTEarthTremorRock::StaticClass(), At, this, GetInstigator(),
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Rock) { return nullptr; }
	Rock->SetSourceAngreal(SourceAngreal);
	Rock->InitializeRock(HasAuthority(), bInner, Scale, SoundProxy);
	Rock->FinishSpawning(At);
	if (!IsValid(Rock)) { return nullptr; }
	LocalRocks.Add(Rock);
	++SpawnedRockCount;
	return Rock;
}

void AWoTEarthTremor::SpawnCenter()
{
	SpawnRock(GetActorLocation(), GetActorRotation(), 1.0f, true);
}

bool AWoTEarthTremor::IsRingSlotClear(const FVector& Slot) const
{
	// EarthTremor.uc:74 Trace(..., false): pawn, projectile and carcass
	// capsules do not suppress a ring rock, including start overlaps.
	return !WoTVisBlockingTraceHits(GetWorld(), GetActorLocation(), Slot, this);
}

bool AWoTEarthTremor::IsRingSlotClearForTest(const FVector& Slot) const
{
	return IsRingSlotClear(Slot);
}

void AWoTEarthTremor::SpawnRing(float Scale, float StartAngle,
	float StepAngle, float Radius)
{
	const FVector X = GetActorForwardVector();
	const FVector Y = GetActorRightVector();
	const FVector Z = GetActorUpVector();
	for (float Angle = StartAngle; Angle < 360.0f; Angle += StepAngle)
	{
		const float Radians = FMath::DegreesToRadians(Angle);
		const FVector Axis = FMath::Sin(Radians) * Y + FMath::Cos(Radians) * X;
		const FVector Slot = GetActorLocation() + Axis * Radius;
		if (!IsRingSlotClear(Slot))
		{
			continue;
		}
		const FVector RingY = FVector::CrossProduct(Z, Axis);
		const FRotator Rotation = FRotationMatrix::MakeFromXY(Axis, RingY).Rotator();
		if (SpawnRock(Slot, Rotation, Scale, false))
		{
			SpawnedRingRadii.Add(Radius);
		}
	}
}

void AWoTEarthTremor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bRunning || NextRing >= 4) { return; }
	StepElapsed += DeltaSeconds;
	if (StepElapsed >= 0.2f)
	{
		// EarthTremor.uc:115/138/149 restarts each state's 0.2 s wait at
		// the actual transition. Discard hitch overrun so one tick can resume
		// at most one gameplay ring instead of stacking catch-up damage.
		StepElapsed = 0.0f;
		switch (NextRing)
		{
		case 0: SpawnRing(1.0f, 0.0f, 72.0f, 70.0f); break;
		case 1: SpawnRing(0.85f, 36.0f, 72.0f, 140.0f); break;
		case 2: SpawnRing(0.70f, 0.0f, 36.0f, 210.0f); break;
		case 3: SpawnRing(0.65f, 18.0f, 36.0f, 280.0f); break;
		default: break;
		}
		++NextRing;
		if (NextRing == 4)
		{
			Destroy();
			return;
		}
	}
}

AWoTEarthTremorRock* AWoTEarthTremor::GetAnyAuthorityRockForTest() const
{
	for (AWoTEarthTremorRock* Rock : LocalRocks)
	{
		if (IsValid(Rock) && Rock->IsAuthorityGameplayRock()) { return Rock; }
	}
	return nullptr;
}
