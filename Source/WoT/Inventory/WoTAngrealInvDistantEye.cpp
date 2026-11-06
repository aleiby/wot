#include "WoTAngrealInvDistantEye.h"

#include "../Projectiles/WoTDistantEyeProjectile.h"
#include "../WoTPlayerController.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

AWoTAngrealInvDistantEye::AWoTAngrealInvDistantEye()
{
	BindStatusIcons(TEXT("I_DistantEye"), TEXT("M_DistantEye"));
	SpawnOffset = FVector(48.0f, 0.0f, 96.0f);
	MovementTolerance = 10.0f;
	EyeClass = AWoTDistantEyeProjectile::StaticClass();
	DropSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/DropDE.DropDE"));
	ActivateSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/ActivateDE.ActivateDE"));
	DeactivateSound = FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/Angreal/DeactivateDE.DeactivateDE"));

	bElementAir = true;
	bElementEarth = true;
	bCommon = true;
	bCombat = true;
	// AngrealInvDistantEye.uc authors bInfo=True, but P29 proved that hint flag
	// retail-dead; the settled G37 ruling explicitly leaves it false.
	bInfo = false;
	MinInitialCharges = 3;
	MaxInitialCharges = 7;
	MaxCharges = 10;
	MinChargeGroupInterval = 10.0f;
	InventoryGroup = 58;
	PickupViewScale = 0.7f;
	DrawScale = 0.7f;
	Mesh->SetRelativeScale3D(FVector(DrawScale));
}

void AWoTAngrealInvDistantEye::CastArtifact()
{
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!HasAuthority() || !IsValid(OwnerPawn) || !GetWorld())
	{
		return;
	}

	if (!IsValid(Eye))
	{
		const FRotator ViewRotation = OwnerPawn->GetViewRotation();
		const FVector SpawnLocation = OwnerPawn->GetActorLocation()
			+ ViewRotation.RotateVector(SpawnOffset);
		// Retail swaps the normal Cast sound to DropDE before Super.Cast. Playing
		// that same replicated marker directly preserves the observable result
		// without temporarily mutating the native ActivateSound CDO field.
		WoTSound::PlayReplicated(*GetWorld(), TEXT("Angreal/DropDE"),
			OwnerPawn->GetActorLocation(), 1600.0f);
		// Retail placement is UNCONDITIONAL: the eye ships bCollideWorld=False
		// with no bCollideWhenPlacing, so UE1 SpawnActor never encroach-tests it
		// — an eye can legally sit inside geometry. AlwaysSpawn matches that;
		// the IsValid guard below is pure defense, not a placement gate.
		FActorSpawnParameters Params;
		Params.Owner = OwnerPawn;
		Params.Instigator = OwnerPawn;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Eye = GetWorld()->SpawnActor<AWoTDistantEyeProjectile>(EyeClass,
			SpawnLocation, ViewRotation, Params);
		if (IsValid(Eye))
		{
			Eye->SetSourceAngreal(this);
		}
		return;
	}

	if (bActive)
	{
		// AngrealInvDistantEye.uc:80-97 toggle-off body is shipped commented out.
		return;
	}
	AWoTPlayerController* PC = Cast<AWoTPlayerController>(OwnerPawn->GetController());
	if (!IsValid(PC))
	{
		return;
	}
	// UE1 ViewTarget==None is the normal self-view state. UE5 represents that as
	// the possessed pawn; do not steal a different external camera.
	if (PC->GetViewTarget() != OwnerPawn)
	{
		return;
	}

	Super::CastArtifact();
	PC->SetViewTarget(Eye);
	PC->ClientSetViewTarget(Eye, FViewTargetTransitionParams());
	const FRotator EyeRotation = Eye->GetActorRotation();
	PC->SetControlRotation(EyeRotation);
	PC->ClientSetRotation(EyeRotation, false);
	Eye->Open();
	InitialOwnerLocation = OwnerPawn->GetActorLocation();
	bActive = true;
	SetActorTickEnabled(true);
}

void AWoTAngrealInvDistantEye::UnCastArtifact()
{
	// Deliberately empty: shipped uc:102. Cease is consumed by the eye reflector.
}

void AWoTAngrealInvDistantEye::ExitViewing()
{
	if (!bActive)
	{
		return;
	}
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	Super::UnCastArtifact();
	if (IsValid(OwnerPawn))
	{
		if (AWoTPlayerController* PC =
			Cast<AWoTPlayerController>(OwnerPawn->GetController()))
		{
			// Restore ONLY if the eye still owns the view: the G32 end camera
			// retargets the controller itself at match end (SetEndCams), and an
			// eye destroyed after that must not stomp the end-camera target.
			if (PC->GetViewTarget() == Eye)
			{
				PC->SetViewTarget(OwnerPawn);
				PC->ClientSetViewTarget(OwnerPawn, FViewTargetTransitionParams());
			}
		}
	}
	if (IsValid(Eye))
	{
		Eye->Close();
	}
	bActive = false;
	if (GetItemState() == EWoTInventoryState::Held)
	{
		SetActorTickEnabled(false);
	}
}

void AWoTAngrealInvDistantEye::NotifyEyeDestroyed(
	AWoTDistantEyeProjectile* DestroyedEye)
{
	if (Eye != DestroyedEye)
	{
		return;
	}
	ExitViewing();
	Eye = nullptr;
}

void AWoTAngrealInvDistantEye::DestroyEye()
{
	if (IsValid(Eye))
	{
		Eye->Destroy();
	}
	Eye = nullptr;
}

void AWoTAngrealInvDistantEye::Reset()
{
	ExitViewing();
	DestroyEye();
	// AngrealInvDistantEye.uc:105-116 explicitly does not call Super.Reset.
}

void AWoTAngrealInvDistantEye::DropFrom(FVector StartLocation)
{
	ExitViewing();
	DestroyEye();
	Super::DropFrom(StartLocation);
}

void AWoTAngrealInvDistantEye::NotifyPutInBag(AWoTBagHolding*)
{
	// AngrealInvDistantEye.uc:186-190 -> Dropped(). A bag bypasses DropFrom,
	// but placing the eye in it must still forget and close the remote eye.
	ExitViewing();
	DestroyEye();
}

void AWoTAngrealInvDistantEye::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!HasAuthority() || !bActive)
	{
		return;
	}
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (!IsValid(OwnerPawn)
		|| FVector::Dist(OwnerPawn->GetActorLocation(), InitialOwnerLocation)
			> MovementTolerance)
	{
		ExitViewing();
	}
}

void AWoTAngrealInvDistantEye::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (HasAuthority())
	{
		ExitViewing();
		DestroyEye();
	}
	Super::EndPlay(EndPlayReason);
}
