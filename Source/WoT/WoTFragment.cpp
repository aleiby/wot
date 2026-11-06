#include "WoTFragment.h"
#include "WoTRandom.h"

#include "WoTSound.h"
#include "WoTSoundTable.h"
#include "WoTVertexAnimComponent.h"
#include "WoTZoneVolume.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

#include <initializer_list>

namespace
{
	FWoTFragmentRecipe MakeFragmentRecipe(
		std::initializer_list<const TCHAR*> Meshes, const TCHAR* Impact,
		const TCHAR* Misc, float Radius, float Height, bool bUpwardBoost)
	{
		FWoTFragmentRecipe R;
		for (const TCHAR* Name : Meshes)
		{
			R.MeshNames.Add(FName(Name));
		}
		R.ImpactSound = FName(Impact);
		R.MiscSound = FName(Misc);
		R.CollisionRadius = Radius;
		R.CollisionHeight = Height;
		R.bUpwardBoost = bUpwardBoost;
		return R;
	}

	const TMap<EWoTFragmentType, FWoTFragmentRecipe>& FragmentRecipes()
	{
		static const TMap<EWoTFragmentType, FWoTFragmentRecipe> Data = {
			{EWoTFragmentType::Wall,
			 MakeFragmentRecipe({TEXT("wafr1"), TEXT("wafr2"), TEXT("wafr4"), TEXT("wafr5"),
			         TEXT("wafr6"), TEXT("wafr7"), TEXT("wafr8"), TEXT("wafr9"),
			         TEXT("wafr10"), TEXT("wafr11"), TEXT("wafr10")},
			        TEXT("WOT/WallFragment1"), TEXT("WOT/WallFragment1"),
			        18.0f, 4.0f, false)},
			{EWoTFragmentType::Wood,
			 MakeFragmentRecipe({TEXT("wfrag1"), TEXT("wfrag2"), TEXT("wfrag3"), TEXT("wfrag4"),
			         TEXT("wfrag5"), TEXT("wfrag6"), TEXT("wfrag7"), TEXT("wfrag8"),
			         TEXT("wfrag9"), TEXT("wfrag10"), TEXT("wfrag11")},
			        TEXT("WOT/WoodFragment1"), TEXT("WOT/WoodFragment2"),
			        12.0f, 2.0f, true)},
			{EWoTFragmentType::Glass,
			 MakeFragmentRecipe({TEXT("Glass1"), TEXT("Glass2"), TEXT("Glass3"), TEXT("Glass4"),
			         TEXT("Glass5"), TEXT("Glass6"), TEXT("Glass7"), TEXT("Glass8"),
			         TEXT("Glass9"), TEXT("Glass10"), TEXT("Glass11")},
			        TEXT("WOT/GlassFragment1"), TEXT("WOT/GlassFragment2"),
			        10.0f, 2.0f, true)},
		};
		return Data;
	}
}

namespace WoTFragmentData
{
	const FWoTFragmentRecipe* FindRecipe(EWoTFragmentType Type)
	{
		return FragmentRecipes().Find(Type);
	}
}

AWoTFragment::AWoTFragment()
{
	PrimaryActorTick.bCanEverTick = true;

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	SetRootComponent(Capsule);
	Capsule->InitCapsuleSize(18.0f, 4.0f);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	Capsule->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);

	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("FragmentMesh"));
	Mesh->SetupAttachment(Capsule);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	bReplicates = true;
	bNetTemporary = false;
	SetReplicatingMovement(true);
	NetPriority = 1.4f;
	InitialLifeSpan = 20.0f;
}

void AWoTFragment::Init(EWoTFragmentType Type, float ExplosionSize,
	const FVector& Momentum, float DrawScale, FName InSkinName)
{
	if (!HasAuthority())
	{
		return;
	}
	const FWoTFragmentRecipe* R = WoTFragmentData::FindRecipe(Type);
	if (!R || R->MeshNames.IsEmpty())
	{
		return;
	}

	MeshName = R->MeshNames[WoTRandom::RandRange(0, R->MeshNames.Num() - 1)];
	MeshScale = DrawScale;
	SkinName = InSkinName;
	ImpactSound = R->ImpactSound;
	MiscSound = R->MiscSound;
	Capsule->SetCapsuleSize(R->CollisionRadius, R->CollisionHeight);

	Velocity = WoTRandom::VRand() *
		(ExplosionSize + WoTRandom::FRand() * 150.0f + 100.0f + Momentum.Size() / 80.0f);
	if (R->bUpwardBoost)
	{
		Velocity.Z += ExplosionSize * 0.5f;
	}
	// Engine Fragment: RandSpin(125000)
	// draws each axis in +/-125000 UE1 rotation units/s, then Pitch/Roll inside
	// (-10000, 10000) snap to +10000. 65536 units = 360 deg, so +/-686.6455 deg/s
	// with a 54.9316 deg/s Pitch/Roll floor.
	constexpr float TumbleRange = 125000.0f / 65536.0f * 360.0f;   // 686.6455 deg/s
	constexpr float TumbleFloor = 10000.0f / 65536.0f * 360.0f;    // 54.9316 deg/s
	TumbleRate = FRotator(
		WoTRandom::FRandRange(-TumbleRange, TumbleRange),
		WoTRandom::FRandRange(-TumbleRange, TumbleRange),
		WoTRandom::FRandRange(-TumbleRange, TumbleRange));
	if (FMath::Abs(TumbleRate.Pitch) < TumbleFloor)
	{
		TumbleRate.Pitch = TumbleFloor;
	}
	if (FMath::Abs(TumbleRate.Roll) < TumbleFloor)
	{
		TumbleRate.Roll = TumbleFloor;
	}
	bFalling = true;
	bDying = false;
	Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Ignore);
	OnRep_Display();
}

FSoftObjectPath AWoTFragment::ResolveSkinTexturePath(FName InSkinName)
{
	const FString Name = InSkinName.ToString();
	const TCHAR* Package = Name.Equals(TEXT("StonBrdg2735cw2"), ESearchCase::IgnoreCase)
		? TEXT("ForsakenT")
		: Name.Equals(TEXT("WoodFlor3670"), ESearchCase::IgnoreCase)
			? TEXT("WhiteCloakT")
			: Name.Equals(TEXT("TimbFace_900"), ESearchCase::IgnoreCase)
				? TEXT("WorldT") : nullptr;
	return Package ? FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/%s/%s.%s"), Package, *Name, *Name))
		: FSoftObjectPath();
}

void AWoTFragment::OnRep_Display()
{
	Mesh->SetRelativeScale3D(FVector(MeshScale));
	// Binding resets prior material overrides, so the skin is reapplied after it.
	Mesh->SetVertexMesh(MeshName);
	if (const FSoftObjectPath SkinPath = ResolveSkinTexturePath(SkinName);
		!SkinPath.IsNull())
	{
		Mesh->SetActorSkin(SkinPath);
	}
}

void AWoTFragment::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || !bFalling)
	{
		return;
	}
	Velocity += AWoTZoneVolume::GetGravityAt(
		GetWorld(), GetActorLocation()) * DeltaSeconds;
	FHitResult Hit;
	AddActorWorldOffset(Velocity * DeltaSeconds, true, &Hit);
	SetActorRotation(GetActorRotation() + TumbleRate * DeltaSeconds);
	if (Hit.IsValidBlockingHit())
	{
		HitWall(Hit.ImpactNormal);
	}
	HandleZoneTransition();
}

void AWoTFragment::BeginPlay()
{
	Super::BeginPlay();
	// Prime the zone edge detectors from the spawn point. Retail's spawn-time
	// ZoneChange dispatches before the fragment enters `auto state Flying`
	// (SetActorZone runs before eventSetInitialState), landing on
	// the empty global Actor.ZoneChange — so a fragment spawned inside a water
	// or pain zone is NOT damped/destroyed; only a later crossing counts.
	if (UWorld* W = GetWorld())
	{
		const FVector Loc = GetActorLocation();
		bInWater = AWoTZoneVolume::IsPointInWater(W, Loc);
		bInPain = AWoTZoneVolume::PainZoneAt(W, Loc) != nullptr;
	}
}

void AWoTFragment::HandleZoneTransition()
{
	// Engine Fragment's Flying.ZoneChange. ZoneChange fires on crossing into a
	// new zone; the port's zone model is the AWoTZoneVolume registry, so this
	// polls the fragment's current point each tick and edge-detects entry.
	// After a water entry the retail fragment lives in state Dying, which has
	// no ZoneChange at all — bWaterDamped disarms this
	// leg permanently (no re-damp on re-entry, no post-water pain destroy).
	UWorld* W = GetWorld();
	if (!W || bWaterDamped)
	{
		return;
	}
	const FVector Loc = GetActorLocation();

	// Water entry: damp velocity and spin to 0.2x on the rising edge,
	// then hand off to the dying/cleanup path.
	const AWoTZoneVolume* WaterZone = AWoTZoneVolume::WaterZoneAt(W, Loc);
	const bool bNowWater = WaterZone != nullptr;
	if (bNowWater && !bInWater)
	{
		// Entry effects: the shipped EntryActor is a mesh-less shell, but
		// EntrySound is audible. The replicated marker reaches listen hosts and
		// every rendering client; a dedicated server renders nothing.
		if (!WaterZone->EntrySoundSubPath.IsNone())
		{
			WoTSound::PlayReplicated(*W, WaterZone->EntrySoundSubPath,
				Loc, 1600.0f, 1.0f);
		}
		Velocity *= 0.2f;
		TumbleRate *= 0.2f;
		bWaterDamped = true;
		StartCleanup();
	}
	bInWater = bNowWater;

	// Pain zone: CROSSING into a pain zone with DamagePerSec > 0
	// destroys the fragment (the bare-flag test is not enough — retail gates on
	// the rate, so PainZoneAt is used rather than IsPointInPainZone). Runs on
	// the water-entry tick too: ZoneChange's GotoState does not abort the
	// function, so retail's pain check still executes for a water+pain zone.
	const AWoTZoneVolume* Pain = AWoTZoneVolume::PainZoneAt(W, Loc);
	if (Pain && !bInPain && Pain->DamagePerSec > 0.0f)
	{
		Destroy();
		return;
	}
	bInPain = Pain != nullptr;
}

void AWoTFragment::NotifyActorBeginOverlap(AActor* OtherActor)
{
	Super::NotifyActorBeginOverlap(OtherActor);
	FlyingTouch(OtherActor);
}

void AWoTFragment::FlyingTouch(AActor* Other)
{
	// Engine Fragment's Flying.Touch destroys the fragment when a NON-player
	// pawn touches it; non-pawns and player pawns are ignored. Gated on the
	// airborne (Flying) phase — a rested fragment no longer reacts.
	//
	// Retail parity note: Fragment authors bCollideActors=false in Flying and
	// defines no Touch in the Dying state, so this handler is dormant against
	// live actor overlaps in the original. The capsule likewise ignores ECC_Pawn
	// here (no overlap events are generated for pawns), so the ported leg stays
	// dormant in-game and is exercised directly by the spec; the logic is kept
	// faithful should a future collision profile enable it. bWaterDamped also
	// gates it off: retail's post-water Dying state defines no Touch.
	if (!HasAuthority() || !bFalling || bWaterDamped)
	{
		return;
	}
	const APawn* Pawn = Cast<APawn>(Other);
	if (!Pawn)
	{
		return;
	}
	// UE1 Pawn.bIsPlayer -> a pawn driven by a PlayerController. (IsPlayerControlled
	// keys on PlayerState, which AI/headless pawns may lack; the controller cast is
	// the faithful "is this a human's pawn" test.)
	if (!Cast<APlayerController>(Pawn->GetController()))
	{
		Destroy();
	}
}

void AWoTFragment::HitWall(const FVector& HitNormal)
{
	Velocity = 0.5f * (Velocity - 2.0f * HitNormal * (Velocity | HitNormal));
	TumbleRate *= 0.75f;
	const float Speed = Velocity.Size();
	if (Speed < 60.0f && HitNormal.Z > 0.7f)
	{
		Velocity = FVector::ZeroVector;
		TumbleRate = FRotator::ZeroRotator;
		bFalling = false;
		StartCleanup();
	}
	else if (Speed > 50.0f && GetWorld())
	{
		const FName Pick = WoTRandom::FRand() < 0.5f ? ImpactSound : MiscSound;
		WoTSound::PlayReplicated(*GetWorld(), Pick, GetActorLocation(), 300.0f,
			WoTRandom::FRandRange(0.5f, 1.0f), WoTRandom::FRandRange(0.85f, 1.15f));
	}
}

void AWoTFragment::StartCleanup()
{
	if (!bDying)
	{
		bDying = true;
		// Engine Fragment's Dying.BeginState SetCollision(true,false,false).  The
		// port's projectile object channel is the actor-touch surface corresponding
		// to bCollideActors=true; overlap preserves bBlockActors=false.
		Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	}
	// Engine Fragment: the Dying state
	// runs SetTimer(1.5, True) and each fire destroys ONLY when !PlayerCanSeeMe(),
	// so debris in view stays put until every player looks away (InitialLifeSpan
	// still caps it at 20s). WasRecentlyRendered is the UE5 analog of that
	// visibility test — render-feedback rather than a per-player LOS trace; on a
	// dedicated server nothing renders, so the first check destroys (PORT_NOTES).
	GetWorldTimerManager().SetTimer(
		CleanupTimer, FTimerDelegate::CreateWeakLambda(this, [this]
		{
			if (!WasRecentlyRendered(0.2f))
			{
				Destroy();
			}
		}),
		1.5f, true);
}

float AWoTFragment::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent,
	AController* EventInstigator, AActor* DamageCauser)
{
	// Engine Fragment: only the Dying state owns TakeDamage, and every hit
	// destroys it. Flying has no actor collision and inherits the inert handler.
	if (HasAuthority() && bDying)
	{
		Destroy();
		return DamageAmount;
	}
	return 0.0f;
}

void AWoTFragment::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTFragment, MeshName);
	DOREPLIFETIME(AWoTFragment, MeshScale);
	DOREPLIFETIME(AWoTFragment, SkinName);
}
