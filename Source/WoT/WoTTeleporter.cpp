#include "WoTTeleporter.h"
#include "WoTRandom.h"
#include "WoTCharacter.h"
#include "Projectiles/WoTSeekingProjectile.h"
#include "Projectiles/WoTWeaponProjectile.h"

#include "WoTBattleGameMode.h"
#include "WoTEvents.h"
#include "WoTPlayerController.h"
#include "WoTWeaveFX.h"
#include "WoTSoundTable.h"
#include "WoTUE1Collision.h"
#include "WoTZoneVolume.h"
#include "AI/WoTNPCController.h"

#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

AWoTTeleporter::AWoTTeleporter()
{
	// UE1 Teleporter defaultproperties: CollisionRadius=18, CollisionHeight=40
	// (T3D overrides carry the Battle maps' 48/90 pads).
	Collision = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Collision"));
	Collision->InitCapsuleSize(18.0f, 40.0f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	// Seeking projectiles ignore WorldStatic/WorldDynamic while homing. Classify
	// the pad on the projectile channel so their swept collision can still Touch it.
	Collision->SetCollisionObjectType(ECC_GameTraceChannel1);
	Collision->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	RootComponent = Collision;
	bReplicates = true;
}

void AWoTTeleporter::SetUE1CollisionSize(float Radius, float HalfHeight)
{
	UE1CollisionRadius = FMath::Max(0.0f, Radius);
	UE1CollisionHalfHeight = FMath::Max(0.0f, HalfHeight);
	Collision->SetCapsuleSize(UE1CollisionRadius,
		WoTUE1Collision::ContainingCapsuleHalfHeight(
			UE1CollisionRadius, UE1CollisionHalfHeight), false);
}

bool AWoTTeleporter::IsWithinUE1Cylinder(const AActor* Other) const
{
	return WoTUE1Collision::CylindersOverlap(GetActorLocation(),
		UE1CollisionRadius, UE1CollisionHalfHeight, Other);
}

bool AWoTTeleporter::AdmitUE1Touch(AActor* Other)
{
	if (IsWithinUE1Cylinder(Other))
	{
		return true;
	}
	UE1Touches.DeferOutside(*this, Collision, Other,
		[this](const AActor* Candidate) { return IsWithinUE1Cylinder(Candidate); },
		[this](AActor* Candidate) { NotifyActorBeginOverlap(Candidate); });
	return false;
}

void AWoTTeleporter::BeginPlay()
{
	Super::BeginPlay();
	// Teleporter.PostBeginPlay turns a destination-only (empty-URL) teleporter's
	// collision off as an optimization; the Touch handler's URL.IsEmpty() early-out
	// carries the semantics here instead — a spawn-then-configure caller (the
	// import, the specs) would otherwise latch collision off before URL lands.
}

void AWoTTeleporter::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Authority supplies bEnabled and URL reliably (Teleporter replication).
	DOREPLIFETIME(AWoTTeleporter, bEnabled);
	DOREPLIFETIME(AWoTTeleporter, URL);
}

void AWoTTeleporter::NotifyActorBeginOverlap(AActor* Other)
{
	Super::NotifyActorBeginOverlap(Other);
	if (!HasAuthority() || !bEnabled || URL.IsEmpty())
	{
		return;
	}
	APawn* P = Cast<APawn>(Other);
	AWoTSeekingProjectile* Seeker = Cast<AWoTSeekingProjectile>(Other);
	AWoTWeaponProjectile* WeaponProjectile = Cast<AWoTWeaponProjectile>(Other);
	if ((!P && !Seeker && !WeaponProjectile)
		|| Other == JustAccepted.Get() || !AdmitUE1Touch(Other))
	{
		return;
	}
	if (Seeker && Seeker->PreTeleport(this))
	{
		return;
	}

	// WOTPlayer.bTeleportingDisabled (WOTPlayer.uc:4295 PreTeleport): the per-player
	// freeze — true for everyone during the citadel-editing phase and for NEUTRAL
	// players even after play begins (JoinTeam/LeaveTeam/Enable/DisableTeleporters
	// drive it). The mode-phase check below stays as the non-player-pawn fold
	// (retail only gated WOTPlayers; MP Battle has no other teleporting pawns).
	if (P)
	{
		if (AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(P->GetController()))
		{
			if (WoTPC->PreTeleport()) { return; }
		}
		if (const AWoTBattleGameMode* Battle = GetWorld()->GetAuthGameMode<AWoTBattleGameMode>())
		{
			if (!Battle->CanTeleport()) { return; }
		}
	}

	// Cross-level / cross-server URL forms wait on §7 online (Level.Game.SendPlayer).
	if (URL.Contains(TEXT("/")) || URL.Contains(TEXT("#")))
	{
		return;
	}

	// For a local URL, choose uniformly among the other teleporters whose tag
	// matches it (Teleporter.Touch).
	const FName Want(*URL);
	TArray<AWoTTeleporter*> Dests;
	for (TActorIterator<AWoTTeleporter> It(GetWorld()); It; ++It)
	{
		if (*It != this && IsValid(*It) && It->TeleTag == Want)
		{
			Dests.Add(*It);
		}
	}
	if (Dests.Num() == 0)
	{
		return;
	}
	AWoTTeleporter* Dest = Dests[WoTRandom::RandHelper(Dests.Num())];
	// PlayTeleportEffect(false) calls Pawn.MakeNoise while the pawn is still at
	// the source. Hearing therefore keys on the entry
	// pad, not the destination reached by Accept.
	if (P && bMakesTeleportNoise)
	{
		AWoTNPCController::BroadcastNoise(GetWorld(), P, 1.0f, P);
	}
	if (!Dest->Accept(Other))
	{
		return;
	}
	if (P) { PresentEffect(false); }
	// Engine GameInfo.PlayTeleportEffect is empty in the shipped game; the
	// TeleporterWithEffects presentation is already supplied by PresentEffect.
	if (P && !EventName.IsNone())
	{
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->FireEvent(EventName, P, P);
		}
	}
}

void AWoTTeleporter::NotifyActorEndOverlap(AActor* Other)
{
	Super::NotifyActorEndOverlap(Other);
	UE1Touches.Forget(Other);
	if (Other == JustAccepted.Get())
	{
		JustAccepted = nullptr;   // walked off — re-arm for this pawn
	}
}

bool AWoTTeleporter::Accept(AActor* Incoming)
{
	// Teleporter.Accept, pawn path.
	if (!IsValid(Incoming))
	{
		return false;
	}
	JustAccepted = Incoming;   // suppress the arrival begin-overlap
	APawn* IncomingPawn = Cast<APawn>(Incoming);
	if (AWoTCharacter* Character = Cast<AWoTCharacter>(Incoming);
		Character && Character->HasAuthority())
	{
		Character->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	}

	const FRotator OldRot = Incoming->GetActorRotation();
	FRotator NewRot = OldRot;
	if (bChangesYaw)
	{
		NewRot.Yaw = GetActorRotation().Yaw;
	}

	const bool bMoved = Incoming->SetActorLocation(GetActorLocation(), false, nullptr,
		ETeleportType::TeleportPhysics);
	if (!IncomingPawn && !bMoved)
	{
		JustAccepted = nullptr;
		return false;
	}
	if (bMoved)
	{
		UWoTZoneIndex::RefreshAfterRelocation(Incoming);
	}
	if (bChangesYaw)
	{
		Incoming->SetActorRotation(NewRot);
		if (AController* C = IncomingPawn ? IncomingPawn->GetController() : nullptr)
		{
			C->SetControlRotation(NewRot);
		}
	}

	FVector V = Incoming->GetVelocity();
	if (bChangesVelocity)
	{
		V = TargetVelocity;
	}
	else
	{
		if (bChangesYaw)
		{
			// Re-aim the along-facing velocity component.
			FRotator OldDirectionRotation = OldRot;
			if (const UCharacterMovementComponent* CharacterMove =
				Incoming->FindComponentByClass<UCharacterMovementComponent>();
				CharacterMove && CharacterMove->MovementMode == MOVE_Walking)
			{
				// Retail flattens pitch only for PHYS_Walking. Flying seekers retain it.
				OldDirectionRotation.Pitch = 0.0f;
			}
			const FVector OldDir = OldDirectionRotation.Vector();
			const float Mag = FVector::DotProduct(V, OldDir);
			V = V - Mag * OldDir + Mag * NewRot.Vector();
		}
		if (bReversesX) { V.X *= -1.0f; }
		if (bReversesY) { V.Y *= -1.0f; }
		if (bReversesZ) { V.Z *= -1.0f; }
	}
	if (UMovementComponent* Move = Incoming->FindComponentByClass<UMovementComponent>())
	{
		Move->Velocity = V;
	}
	if (IncomingPawn) { PresentEffect(true); }
	return true;
}

void AWoTTeleporter::PresentEffect(bool bExit)
{
	if (!HasAuthority() || !GetWorld()) { return; }
	const FName Config = bExit ? ExitEffectConfig : EnterEffectConfig;
	const FName Sound = bExit ? ExitEffectSoundPath : EnterEffectSoundPath;
	if (bExit) { LastExitEffectConfig=Config; LastExitEffectSound=Sound; }
	else { LastEnterEffectConfig=Config; LastEnterEffectSound=Sound; }
	if (!Config.IsNone())
	{
		WoTWeaveFX::SpawnBurstMarker(*GetWorld(), Config, GetActorLocation());
	}
	if (!Sound.IsNone())
	{
		WoTSound::PlayReplicated(*GetWorld(), Sound, GetActorLocation(), 1600.0f);
	}
}
