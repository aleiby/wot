#include "WoTTrapBurst.h"
#include "../WoTRandom.h"
#include "../WoTVertexAnimComponent.h"
#include "../AI/WoTNPCController.h"
#include "WoTTrap.h"

AWoTTrapBurst::AWoTTrapBurst()
{
	VisualMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(RootComponent);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	// The self-destruct (PlayAnim 'Explo' → FinishAnim → Destroy). Set on the CDO so the spawn path
	// starts the lifespan timer — SetLifeSpan inside BeginPlay no-ops (HasActorBegunPlay() is still
	// false there, so the timer would never arm).
	InitialLifeSpan = BurstLifeSpan;

	// A minimal root; the burst mesh (DrawType=DT_Mesh, Mesh'WoTTraps.TrapBurst') binds onto
	// the vertex-anim component in BeginPlay (BindTrapVisual "Trapburst").
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void AWoTTrapBurst::BeginPlay()
{
	Super::BeginPlay();

	// TrapBurst.Explode (TrapBurst.uc:19-28), on every machine — the actor replicates:
	// random facing (rotator(VRand()) — pitch+yaw), the AI hearing stimulus, and the
	// 6-frame 'Explo' play at 0.6 (~0.56s; the shipped PlaySound(EffectSound1) is a
	// no-op — no default sound). Shipped destroyed on FinishAnim; the ctor's 1s
	// InitialLifeSpan holds the last frame briefly instead (minor divergence).
	SetActorRotation(WoTRandom::VRand().Rotation());
	MakeNoise(1.0f);
	// CheckNoiseHearing uses the burst as maker/origin, with the pawn that
	// tripped/placed it supplying the per-instigator history and fanout context.
	if (HasAuthority())
	{
		if (APawn* NoiseInstigator = GetInstigator())
		{
			AWoTNPCController::BroadcastNoise(GetWorld(), this, 1.0f, NoiseInstigator);
		}
	}
	AWoTTrap::BindTrapVisual(VisualMesh, FName(TEXT("Trapburst")), FVector(1.0f));
	VisualMesh->PlayAnim(FName(TEXT("Explo")), 0.6f);
}
