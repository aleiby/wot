#include "WoTMashadarSegment.h"
#include "../WoTRandom.h"
#include "Net/UnrealNetwork.h"
#include "../WoTWeaveFX.h"
#include "../WoTCarcass.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../WoTMeshParticleRenderer.h"
#include "../WoTVertexAnimComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture2D.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "../RetailData/WoTDevRetailFamilies.h"
#endif

#include "WoTMashadarGuide.h"

#include "../WoTSoundTable.h"
#include "WoTProjectile.h"   // FWoTDamageEvent
#include "../WoTCharacter.h"
#include "../Inventory/WoTAngrealInvLightning.h"
#include "Components/SphereComponent.h"

AWoTMashadarSegment::AWoTMashadarSegment()
{
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(RootComponent);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DisplayMesh->SetVisibility(false);
	if (!IsRunningDedicatedServer())
	{
		DisplayMesh->RequestCurrentPoseCPUAccess();
	}

	ParticleQuads = CreateDefaultSubobject<UWoTMeshParticleRenderer>(
		TEXT("ParticleQuads"));
	ParticleQuads->SetupAttachment(RootComponent);
	// The actor's quad update must see the frame morphs published by this
	// component tick, not the previous tick's pose.
	PrimaryActorTick.AddPrerequisite(
		DisplayMesh, DisplayMesh->PrimaryComponentTick);
	// MashadarSegment joints bend 40-50 degrees (stiffer than the base default).
	MinAngleExt = 40.0f;
	MaxAngleExt = 50.0f;
	Dist = 42.0f;

	// The damaging body volume — overlaps pawns, sweeps damage as the segment moves.
	// Hidden segments still collide (a hidden actor is invisible, not intangible).
	TouchVolume = CreateDefaultSubobject<USphereComponent>(TEXT("TouchVolume"));
	TouchVolume->SetupAttachment(GetRootComponent());
	TouchVolume->SetSphereRadius(TouchRadius);
	TouchVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TouchVolume->SetCollisionObjectType(ECC_WorldDynamic);
	TouchVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	TouchVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	// MashadarTrailer.uc authors bProjTarget=True.  A segment is a shootable,
	// non-blocking actor, so moving weave projectiles must overlap its body and
	// route damage through TakeDamage to the shared guide health.
	TouchVolume->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	TouchVolume->SetGenerateOverlapEvents(true);
	TouchVolume->OnComponentBeginOverlap.AddDynamic(this, &AWoTMashadarSegment::OnSegmentOverlap);
}

void AWoTMashadarSegment::BeginPlay()
{
	Super::BeginPlay();
	// Apply the (possibly guide-configured) collision reach.
	if (TouchVolume)
	{
		TouchVolume->SetSphereRadius(TouchRadius);
	}
	InitializeParticleRenderer();
}

void AWoTMashadarSegment::OnSegmentOverlap(UPrimitiveComponent* /*OverlappedComp*/, AActor* Other,
                                           UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/,
                                           bool /*bFromSweep*/, const FHitResult& /*Sweep*/)
{
	BiteTarget(Other);
}

void AWoTMashadarSegment::BiteTarget(AActor* Other)
{
	// MashadarTrailer.uc:52 Touch — server-authoritative; a guide-less segment is inert.
	AWoTMashadarGuide* G = Guide.Get();
	// Client-local RemoteRole=None actors can report local authority in UE5, so the
	// replicated guide's role is the decisive gameplay gate as well. This keeps the
	// locally-collidable visual chain inert on NM_Client (MashadarTrailer.uc:52-61).
	if (!HasAuthority() || !IsValid(G) || !G->HasAuthority())
	{
		return;
	}
	// MashadarTrailer.uc:61-68: Lightning shocks the whole tendril before the
	// active/enemy bite gates and never receives the ordinary Spirit bite.
	if (APawn* Pawn = Cast<APawn>(Other);
		Pawn && AWoTAngrealInvLightning::IsUsingLightning(Pawn))
	{
		G->Shock(Pawn);
		return;
	}
	// Only an active tendril bites, and only enemies (MashadarTrailer.uc:71).
	if (!G->bActive || !G->IsEnemyPawn(Other))
	{
		return;
	}
	const AWoTCharacter* Victim = Cast<AWoTCharacter>(Other);
	const int32 HealthBefore = Victim ? Victim->Health : 0;

	FWoTDamageEvent Ev;
	Ev.WoTDamageType = TEXT("xxxxS");   // Spirit
	Ev.UE1Momentum = FVector::ZeroVector;   // MashadarTrailer.uc:87
	Ev.HitInfo.ImpactPoint = GetActorLocation();
	Other->TakeDamage(static_cast<float>(Damage), Ev, G->GetInstigatorController(), this);

	// The bite taunts (MashadarTrailer.uc:95-102), routed through the guide's
	// table like shipped: a surviving victim draws MeleeHitEnemyTaunt (the row's
	// own 0.05 odds gate the rate), a kill MeleeKilledEnemyTaunt.
	if (Victim)
	{
		WoTSoundTable::PlaySlotSound(*G, G->VoiceTableName,
		                             FName(Victim->IsAlive()
		                                       ? TEXT("MeleeHitEnemyTaunt")
		                                       : TEXT("MeleeKilledEnemyTaunt")));
	}

	// A run of no-effect bites (a shielded/immune victim) gives up and pulls the tendril
	// back (MashadarTrailer.uc:75-88).
	if (Victim && Victim->Health == HealthBefore)
	{
		if (++NoDamageCounter > NoDamageTolerance)
		{
			NoDamageCounter = 0;
			G->Deactivate();
		}
	}
	else
	{
		NoDamageCounter = 0;
	}
}

float AWoTMashadarSegment::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
                                      AController* EventInstigator, AActor* DamageCauser)
{
	// MashadarTrailer.uc:43 — a hit on a segment hurts the whole tendril, routed through
	// the guide (which owns the shared health and the death/frag-credit).
	if (AWoTMashadarGuide* G = Guide.Get())
	{
		// HurtMashadar accepts damage only for an active trailer that is still
		// registered in this guide's production chain (the retail manager gate).
		if (!HasAuthority() || !G->HasAuthority() || !G->bActive
			|| !G->IsManagedSegment(this)
			|| !G->ConsumeSegmentDamageThisTick())
		{
			return 0.0f;
		}
		// HurtMashadar's pain burst at the HIT segment, aimed up (MashadarGuide.uc:900)
		// — a replicated one-shot so every machine sees the puff.
		if (GetWorld())
		{
			WoTWeaveFX::SpawnBurstMarker(*GetWorld(), TEXT("MashadarPainSprayer"),
			                             GetActorLocation(), FRotator(90, 0, 0));
		}
		const int32 Applied = FMath::TruncToInt(DamageAmount);
		if (G->MashadarHealth > 0 && G->MashadarHealth - Applied <= 0)
		{
			// Retail sends the hit segment's one-based index so each client severs its
			// private chain at the equivalent joint (MashadarGuide.uc:894-908).
			G->NotifyLethalSegmentHit(this);
		}
		return G->TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
	}
	return 0.0f;
}

void AWoTMashadarSegment::FadeAway()
{
	// MashadarTrailer.uc:182 — fade this segment and cascade to the rest of the chain.
	if (bFadingAway)
	{
		return;   // already cascading — Destroyed() must not re-walk the chain
	}
	bFadingAway = true;
	if (AWoTMashadarSegment* Child = Cast<AWoTMashadarSegment>(ChildTrailer))
	{
		Child->FadeAway();
	}
	Fade = EFade::Away;
}

void AWoTMashadarSegment::Destroyed()
{
	// MashadarTrailer.uc:118 — the master drags the rest of the chain down with it, and
	// the guide's segment tally drops. The FadingAway gate (MashadarTrailer.uc:120
	// `GetStateName() != 'FadingAway'`) keeps a FadeAway-initiated Destroy from
	// cascading a second time over the pending-kill chain.
	if (bMaster && !bFadingAway)
	{
		if (AWoTMashadarSegment* Child = Cast<AWoTMashadarSegment>(ChildTrailer))
		{
			Child->FadeAway();
		}
	}
	if (AWoTMashadarGuide* G = Guide.Get())
	{
		G->NotifySegmentDestroyed(this);
	}
	Super::Destroyed();
}

void AWoTMashadarSegment::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTMashadarSegment, bIsHead);
}

void AWoTMashadarSegment::BindDisplay()
{
	// Lazy (bIsHead lands post-spawn on the server, pre-BeginPlay on clients).
	bDisplayBound = true;
	BaseDrawScale = bIsHead ? 0.2f : 0.3f;   // MashadarTip.uc / MashadarSegment.uc
	DrawScaleNow = BaseDrawScale;
	if (GetNetMode() == NM_DedicatedServer)
	{
		// Preserve the existing fade/teardown timing while skipping every
		// rendering asset and instance.
		FadeIn();
		return;
	}
	if (DisplayMesh->SetVertexMesh(
		FName(bIsHead ? TEXT("MashTip") : TEXT("MashSeg"))))
	{
		// UnMesh.cpp:433,574 uses scale 1 for bParticles vertex positions.
		// DrawScale belongs only to the icons (UnMeshRn.cpp:443-444), otherwise
		// each cloud collapses between the 42/48-unit trailer joints.
		DisplayMesh->SetRelativeScale3D(
			UWoTMeshParticleRenderer::PoseDriverScale(true, BaseDrawScale));
		// Hidden, but still ticking: this supplies the current mesh-local pose.
		DisplayMesh->SetVisibility(false);
		DisplayMesh->SetComponentTickEnabled(true);
		DisplayMesh->LoopAnim(FName(TEXT("All")), WoTRandom::FRandRange(0.01f, 0.03f));
	}
#if !WOT_GUARDED_BOOTSTRAP
	if (bIsHead)
	{
		// Triangle-based MashTip imports omit a moving source vertex. Read its
		// ten authored frames once; do not invent a stationary mote at the origin.
		WoTDevRetailUnreferencedMeshFrames(TEXT("WoTPawns.MashTip"), UnreferencedVertexFrames);
	}
#endif
	FadeIn();
}

void AWoTMashadarSegment::InitializeParticleRenderer()
{
	if (GetNetMode() == NM_DedicatedServer) return;
	UWoTMeshParticleRenderer* Renderer = GetParticleRenderer();
	if (!Renderer) return;
	Renderer->InitializeParticleRenderer(DisplayMesh);
	UTexture2D* ImportedMyTex2 = UWoTRuntimeAssets::ResolveTexturePath(
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WoTPawns/MyTex2.MyTex2")));
	Renderer->SetParticleTexture(ImportedMyTex2);
}

void AWoTMashadarSegment::FadeIn()
{
	DesiredScaleGlow = WoTRandom::FRandRange(0.2f, 0.4f);
	// MashadarTrailer only enters FadingIn in standalone. Network segments
	// appear immediately at their desired glow.
	const bool bRamp = GetNetMode() == NM_Standalone;
	Fade = bRamp ? EFade::In : EFade::Hold;
	ScaleGlow = bRamp ? 0.0f : DesiredScaleGlow;
}

float AWoTMashadarSegment::ComputeRetailParticleColor(
	const float InScaleGlow, const uint8 AmbientGlow)
{
	return UWoTMeshParticleRenderer::ComputeRetailParticleColor(
		InScaleGlow, AmbientGlow);
}

void AWoTMashadarSegment::UpdateParticleQuads()
{
	TConstArrayView<FVector> AdditionalVertices;
#if !WOT_GUARDED_BOOTSTRAP
	// Runtime SourcePositions already contains every FrameVert. Only the asset
	// path needs the triangle-unreferenced vertices (UnMeshRn.cpp bParticles).
	if (!DisplayMesh->HasCompleteSourceVertexPose() && !UnreferencedVertexFrames.IsEmpty())
	{
		const float Frame = DisplayMesh->GetAnimFrame() * UnreferencedVertexFrames.Num();
		const int32 FrameA = FMath::FloorToInt(Frame) % UnreferencedVertexFrames.Num();
		const int32 FrameB = (FrameA + 1) % UnreferencedVertexFrames.Num();
		const float Alpha = FMath::Frac(Frame);
		UnreferencedPose.SetNumUninitialized(UnreferencedVertexFrames[FrameA].Num());
		for (int32 Vertex = 0; Vertex < UnreferencedPose.Num(); ++Vertex)
			UnreferencedPose[Vertex] = FMath::Lerp(UnreferencedVertexFrames[FrameA][Vertex],
				UnreferencedVertexFrames[FrameB][Vertex], Alpha);
		AdditionalVertices = UnreferencedPose;
	}
#endif
	if (UWoTMeshParticleRenderer* Renderer = GetParticleRenderer())
	{
		Renderer->UpdateParticleQuads(DrawScaleNow, ScaleGlow, 0, AdditionalVertices);
	}
}

UWoTMeshParticleRenderer* AWoTMashadarSegment::GetParticleRenderer() const
{
	return Cast<UWoTMeshParticleRenderer>(ParticleQuads);
}

UMaterialInstanceDynamic* AWoTMashadarSegment::GetParticleMaterial() const
{
	const UWoTMeshParticleRenderer* Renderer = GetParticleRenderer();
	return Renderer ? Renderer->GetParticleMaterial() : nullptr;
}

FIntPoint AWoTMashadarSegment::GetParticleTextureSize() const
{
	const UWoTMeshParticleRenderer* Renderer = GetParticleRenderer();
	return Renderer ? Renderer->GetParticleTextureSize() : FIntPoint(128, 128);
}

void AWoTMashadarSegment::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bDisplayBound)
	{
		BindDisplay();
	}
	// The shipped ramps ran per rendered frame (~30Hz): +0.01/frame in, -0.02/frame
	// out with +0.01/frame swell — expressed per-second at that cadence.
	const float Step30 = DeltaTime * 30.0f;
	switch (Fade)
	{
		case EFade::In:
			ScaleGlow += 0.01f * Step30;
			if (ScaleGlow >= DesiredScaleGlow)
			{
				ScaleGlow = DesiredScaleGlow;
				Fade = EFade::Hold;
			}
			break;
		case EFade::Away:
			ScaleGlow -= 0.02f * Step30;
			DrawScaleNow += 0.01f * Step30;
			if (ScaleGlow <= 0.0f && HasAuthority())
			{
				Destroy();
			}
			break;
		default:
			break;
	}
	UpdateParticleQuads();
}
