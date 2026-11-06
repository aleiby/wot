#include "WoTIceLeech.h"

#include "../WoTMeshFX.h"
#include "../WoTPlayerController.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTCharacter.h"
#include "../AI/WoTNPCController.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

UWoTIceLeech::UWoTIceLeech()
{
	// IceLeech defaultproperties (simulation-relevant subset).
	MeltTime = 2.0f;
	bDeleterious = true;
	bDisplayIcon = true;       // displayed as a deleterious status icon
	bRemoveExisting = true;    // a fresh freeze replaces an existing one
	// Lifespan is set from the projectile's Duration at install time.
}

void UWoTIceLeech::AttachTo(APawn* NewHost)
{
	// Original clears bFromProjectile before Super so the victim can still be
	// shifted / swap-placed out of the ice (IceLeech.uc:84).
	bFromProjectile = false;

	Super::AttachTo(NewHost);

	// Super may have refused/destroyed us (bSingular) — bail if unregistered.
	if (!IsRegistered())
	{
		return;
	}

	if (APawn* Host = Cast<APawn>(GetOwner()))
	{
		FreezeHost(Host);
		EnsureFrozenPresentation();
	}
	else
	{
		Unattach();   // original: Owner == None -> Destroy()
	}
}

void UWoTIceLeech::FreezeHost(APawn* Host)
{
	SuspendedController = nullptr;
	bHostWasDormantBeforeFreeze = false;
	OriginalLocation = Host->GetActorLocation();
	Host->SetActorLocation(OriginalLocation);   // parity no-op (Owner.Location)
	if (AWoTNPCController* NPCController =
		Cast<AWoTNPCController>(Host->GetController()))
	{
		bHostWasDormantBeforeFreeze =
			NPCController->GetBrainState() == EWoTBrainState::Dormant;
		SuspendedController = NPCController;
		NPCController->SuspendForIceFreeze(bHostWasDormantBeforeFreeze);
	}

	// Stop and disable movement (UE1 StopMovement + GotoState('') + zeroed
	// AnimRate). Restored verbatim on Unattach.
	if (ACharacter* Char = Cast<ACharacter>(Host))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			SavedMovementMode = Move->MovementMode;
			SavedCustomMovementMode = Move->CustomMovementMode;
			Move->StopMovementImmediately();
			Move->DisableMovement();   // MOVE_None
		}
	}

	// Telefrag guard: stop the host blocking other pawns while pinned (UE1
	// SetCollision(true,false,false)). The separate retail-sized ice cylinder
	// below blocks pawns and intercepts projectiles before they reach the host.
	if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Host->GetRootComponent()))
	{
		SavedPawnResponse = Root->GetCollisionResponseToChannel(ECC_Pawn);
		Root->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	bFrozeHost = true;
	EnsureIceBlock();
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Host->GetController()))
	{
		// IceLeech.uc:107, defaults GlowScale=-.39 and
		// GlowFog=(312.5,468.75,468.75), routed to the owning client.
		PC->ClientAdjustGlow(-0.39f, FVector(312.5f, 468.75f, 468.75f));
	}
}

void UWoTIceLeech::RefreshNPCControllerSuspension()
{
	if (!bFrozeHost)
	{
		return;
	}
	APawn* Host = Cast<APawn>(GetOwner());
	AWoTNPCController* Current = Host
		? Cast<AWoTNPCController>(Host->GetController()) : nullptr;
	if (Current == SuspendedController.Get())
	{
		return;
	}
	if (AWoTNPCController* Previous = SuspendedController.Get())
	{
		Previous->ReleaseIceFreezeOwnership();
	}
	SuspendedController = Current;
	if (Current)
	{
		Current->SuspendForIceFreeze(bHostWasDormantBeforeFreeze);
	}
}

void UWoTIceLeech::ThawHost(APawn* Host)
{
	if (!bFrozeHost)
	{
		return;
	}
	if (!IsValid(Host))
	{
		bFrozeHost = false;
		if (AWoTNPCController* Previous = SuspendedController.Get())
		{
			Previous->ReleaseIceFreezeOwnership();
		}
		SuspendedController = nullptr;
		bHostWasDormantBeforeFreeze = false;
		return;
	}
	// The pawn may have changed controllers without ticking the leech between the
	// possession edge and this detach/expiry.
	RefreshNPCControllerSuspension();
	bFrozeHost = false;

	if (ACharacter* Char = Cast<ACharacter>(Host))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			Move->SetMovementMode(SavedMovementMode, SavedCustomMovementMode);
		}
	}

	if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(Host->GetRootComponent()))
	{
		Root->SetCollisionResponseToChannel(ECC_Pawn, SavedPawnResponse);
	}
	if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(Host->GetController()))
	{
		PC->ClientAdjustGlow(0.39f, FVector(-312.5f, -468.75f, -468.75f));
	}
	if (AWoTNPCController* NPCController = SuspendedController.Get())
	{
		NPCController->ResumeFromIceFreeze();
	}
	else if (AWoTCharacter* Character = Cast<AWoTCharacter>(Host))
	{
		Character->ResumeNPCAnimationAfterIce();
	}
	SuspendedController = nullptr;
	bHostWasDormantBeforeFreeze = false;
}

void UWoTIceLeech::OnHostDetached(APawn* OldHost)
{
	// Swap Places performs the non-destructive equivalent of retail UnAttach
	// before reparenting this same leech object. Retire objects owned by the old
	// host and restore all host/controller state; AttachTo will establish a fresh
	// snapshot and presentation on the new host.
	RetireFrozenPresentation();
	RetireIceBlock();
	ThawHost(OldHost);
}

void UWoTIceLeech::Unattach()
{
	// Restore host state before the component is destroyed (Super::Unattach).
	OnHostDetached(Cast<APawn>(GetOwner()));
	Super::Unattach();
}

void UWoTIceLeech::EnsureIceBlock()
{
	APawn* Host = Cast<APawn>(GetOwner());
	if (IsValid(IceBlock) || !IsRegistered() || !IsValid(Host) || !GetWorld())
	{
		return;
	}
	const FTransform At(Host->GetActorRotation(), Host->GetActorLocation());
	IceBlock = GetWorld()->SpawnActorDeferred<AWoTCollisionBlocker>(
		AWoTCollisionBlocker::StaticClass(), At, Host, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!IceBlock)
	{
		return;
	}
	const FWoTMeshEffectRow* Frozen = WoTMeshFX::Find(TEXT("FrozenPresentation"));
	const float DrawScale = Frozen
		? WoTMeshFX::ResolveDrawScale(*Frozen, Host) : 3.0f;
	const float CollisionScale = DrawScale / 3.0f;
	// IceLeech.uc ScaleForExceptions applies the same class DrawScale to both
	// Frozen and its ordinary actor collision cylinder.
	IceBlock->Configure(EWoTCollisionBlockerMode::Actor,
		40.0f * CollisionScale, 48.0f * CollisionScale);
	IceBlock->Capsule->IgnoreActorWhenMoving(Host, true);
	if (UPrimitiveComponent* HostRoot =
		Cast<UPrimitiveComponent>(Host->GetRootComponent()))
	{
		HostRoot->IgnoreActorWhenMoving(IceBlock, true);
	}
	IceBlock->FinishSpawning(At);
	IceBlock->AttachToActor(Host,
		FAttachmentTransformRules::KeepWorldTransform);
}

void UWoTIceLeech::RetireIceBlock()
{
	if (IsValid(IceBlock))
	{
		if (APawn* Host = Cast<APawn>(GetOwner()))
		{
			if (UPrimitiveComponent* HostRoot =
				Cast<UPrimitiveComponent>(Host->GetRootComponent()))
			{
				HostRoot->IgnoreActorWhenMoving(IceBlock, false);
			}
		}
		IceBlock->Destroy();
	}
	IceBlock = nullptr;
}

void UWoTIceLeech::EnsureFrozenPresentation()
{
	if (IsValid(FrozenShell) || !IsRegistered() || !IsValid(GetOwner())
		|| !GetWorld() || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	FrozenShell = WoTMeshFX::SpawnLocal(*GetWorld(), TEXT("FrozenPresentation"),
		GetOwner()->GetActorLocation(), GetOwner()->GetActorRotation(), GetOwner());
	if (IsValid(FrozenShell) && Lifespan > 0.0f)
	{
		// The projectile stamps Duration on the accepted leech. Mirror that live
		// instance value so custom durations cannot leave a stale seven-second shell.
		FrozenShell->SetLifeSpan(Lifespan);
	}
}

void UWoTIceLeech::RetireFrozenPresentation()
{
	if (IsValid(FrozenShell))
	{
		FrozenShell->Destroy();
	}
	FrozenShell = nullptr;
}

void UWoTIceLeech::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// Also covers replicated component teardown on clients, where Unattach is not
	// called locally when authority removes/cleanses/replaces the leech.
	OnHostDetached(Cast<APawn>(GetOwner()));
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UWoTIceLeech::TickComponent(float DeltaTime, ELevelTick TickType,
                                 FActorComponentTickFunction* ThisTickFunction)
{
	// Pin the host in place, exactly as IceLeech.uc:193
	// (Owner.SetLocation(OriginalLocation)). Do this before Super, which may
	// expire us this frame (Expired -> Unattach -> DestroyComponent).
	if (bFrozeHost)
	{
		RefreshNPCControllerSuspension();
		if (APawn* Host = Cast<APawn>(GetOwner()))
		{
			Host->SetActorLocation(OriginalLocation);
		}
	}
	// Replicated leech components do not execute AttachTo on simulated proxies;
	// their first component tick is the presentation lifecycle entry point.
	EnsureFrozenPresentation();
	EnsureIceBlock();

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UWoTIceLeech::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Original: reliable if Role==Authority OriginalLocation.
	DOREPLIFETIME(UWoTIceLeech, OriginalLocation);
}
