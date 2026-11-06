#include "WoTAlarm.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTSoundTable.h"
#include "../WoTSound.h"

#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTNPCController.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "TimerManager.h"

AWoTAlarm::AWoTAlarm()
{
	// Alarm.uc defaultproperties. bReplicates: the player-spawned trap family (the gong is
	// client-facing presentation). bProjTarget=True: weave hits ring it through TakeDamage;
	// the capsule stays the base query-only touch volume. The 'Waiting' loop is a single
	// frame (a static bind); the ActivateAM gong plays in EnterSoundWindow.
	VisualMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(RootComponent);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	bReplicates = true;
	HeightAboveFloor = 80;
	SeparationDistance = 32;
	TrapRadius = 56;
	// Neither Alarm.uc nor Trap.uc sets a collision size, so the gong keeps the
	// UE1 Actor default cylinder (22 x 22): its touch volume and the extent the
	// captain's goal tests and reach thresholds measure.
	if (Capsule)
	{
		Capsule->InitCapsuleSize(22.0f, 22.0f);
	}
}


void AWoTAlarm::BeginPlay()
{
	Super::BeginPlay();
	BindTrapVisual(VisualMesh, FName(TEXT("Alarm")), FVector(1.0f));
}

bool AWoTAlarm::CalcLocation(FVector& Loc, FVector& Normal)
{
	if (FMath::Abs(Normal.Z) > KINDA_SMALL_NUMBER)
	{
		return false;
	}
	return Super::CalcLocation(Loc, Normal);
}

bool AWoTAlarm::DeployResource(FVector Loc, FVector Normal)
{
	if (!CalcLocation(Loc, Normal))
	{
		return false;
	}
	SetActorLocationAndRotation(Loc, Normal.Rotation());
	// Alarm.uc:47 returns ValidateTrap(): Trap.ValidateTrap (Trap.uc:185-204) does
	// SetOwner(None) on every final placement (owner drives net relevancy — retail cleared
	// it for exactly that reason; the tutorial-trigger poll is deferred, as in Pit).
	if (!IsPreviewMode())
	{
		SetOwner(nullptr);
	}
	return true;
}

void AWoTAlarm::ApplyEditingCollision(bool bEditingNow)
{
	if (!Capsule)
	{
		return;
	}
	if (bEditingNow)
	{
		// Keep the gong a query-only touch volume for pawns, but make it a blocking hit for
		// the editor's ECC_Visibility cursor while resources are being edited.
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	}
	else
	{
		// Alarm.uc has no edit-collision change; the edit half added an ECC_Visibility
		// block for editor targeting. Undo it at play so it does not leak into runtime,
		// but keep the query-only Pawn-overlap touch volume the gong triggers on.
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	}
}

void AWoTAlarm::ActivateAlarm(AActor* Ringer)
{
	if (IsPreviewMode())
	{
		return;
	}
	// Waiting-state ActivateAlarm notifies every pawn within SoundRadius*16 before entering
	// the sound phase (Alarm.uc:52-56). This direct AWoTNPCController::AlarmSounded fan-out
	// replaces the source TriggerRadiusInstances chain that reaches Grunt.uc:809-812.
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	bRung = true;

	const float Reach = SoundRadius * 16.0f;
	const float ReachSquared = Reach * Reach;
	const FVector Origin = GetActorLocation();
	for (TActorIterator<AWoTNPCCharacter> It(World); It; ++It)
	{
		AWoTNPCCharacter* NPC = *It;
		if (NPC == Ringer)
		{
			continue;
		}
		if (FVector::DistSquared(NPC->GetActorLocation(), Origin) > ReachSquared)
		{
			continue;
		}
		if (AWoTNPCController* Controller = Cast<AWoTNPCController>(NPC->GetController()))
		{
			Controller->AlarmSounded(this);
		}
	}

	EnterSoundWindow();
}

void AWoTAlarm::Touch(AActor* Other)
{
	// Waiting.Touch is authority-only world state (Alarm.uc:68-84). Passing no toucher to
	// IsInactive bypasses the team filter, leaving only the editing-state gate.
	if (!Other || !HasAuthority())
	{
		return;
	}
	if (IsInactive(nullptr))
	{
		return;
	}
	APawn* P = Cast<APawn>(Other);
	if (!P)
	{
		return;
	}
	if (P->IsA<AWoTNPCCharacter>())
	{
		// NPC overlap does not ring the gong; the source permits only player touch here
		// (Alarm.uc:79-81). Captains use ActivateAlarm when they arrive.
		return;
	}
	ActivateAlarm(Other);
}

void AWoTAlarm::Bump(AActor* Other)
{
	// Waiting.Bump → Touch (Alarm.uc:63-66); the Sound window Disables 'Bump' (Alarm.uc:91).
	if (bBumpEnabled)
	{
		Touch(Other);
	}
}

float AWoTAlarm::TakeDamage(float Damage, const FDamageEvent& DamageEvent,
                            AController* EventInstigator, AActor* DamageCauser)
{
	if (IsPreviewMode())
	{
		return 0.0f;
	}
	// Waiting.TakeDamage → Touch(InstigatedBy) (Alarm.uc:58-61): the alarm takes no damage itself.
	Touch(EventInstigator ? EventInstigator->GetPawn() : nullptr);
	return 0.0f;
}

void AWoTAlarm::NotifyActorBeginOverlap(AActor* Other)
{
	// UE1 Touch = overlap entry on the trap capsule.
	Super::NotifyActorBeginOverlap(Other);
	Touch(Other);
}

void AWoTAlarm::EnterSoundWindow()
{
	// The sound phase suppresses bumps, plays the gong, waits AlarmDelay, then restores bumps
	// (Alarm.uc:90-94). Replacing the timer on another ring restarts that wait, matching a
	// fresh entry to the source state label.
	bBumpEnabled = false;
	// Alarm.uc:92 supplies no PlaySound radius, so UE1 falls back to the actor
	// transient-sound default (1600uu). SoundRadius=255 is ambient/NPC fan-out only.
	WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/ActivateAM")),
		GetActorLocation(), WoTSound::DefaultPlaySoundRadius);
	GetWorldTimerManager().SetTimer(SoundTimer, this, &AWoTAlarm::ReenableBump,
	                                static_cast<float>(AlarmDelay), false);
}

void AWoTAlarm::ReenableBump()
{
	bBumpEnabled = true;
}

bool AWoTAlarm::HasActiveSoundTimerForTest() const
{
	return GetWorld() && GetWorld()->GetTimerManager().IsTimerActive(SoundTimer);
}

AWoTAlarmInventory::AWoTAlarmInventory()
{
	// AlarmInventory.uc defaultproperties (player-spawned).
	ResourceClass = AWoTAlarm::StaticClass();
	Count = 2;
	// AlarmInventory.uc:20,23: StatusIconFrame=Texture'WoTTraps.UI.M_SAlarm' /
	// StatusIcon=Texture'WoTTraps.UI.I_SAlarm'.
	BindStatusIcons(TEXT("WoTTraps"), TEXT("I_SAlarm"), TEXT("M_SAlarm"));
}
