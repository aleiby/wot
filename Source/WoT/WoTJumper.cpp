#include "WoTJumper.h"

#include "AI/WoTNPCCharacter.h"
#include "WoTUE1Collision.h"

#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PawnMovementComponent.h"
#include "TimerManager.h"

AWoTJumper::AWoTJumper()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	RootComponent = Capsule;
	Capsule->InitCapsuleSize(40.0f, 40.0f);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetCollisionObjectType(ECC_WorldDynamic);
	Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Capsule->SetGenerateOverlapEvents(true);
	Capsule->OnComponentBeginOverlap.AddDynamic(this, &AWoTJumper::OnOverlap);
}

void AWoTJumper::SetUE1CollisionSize(float Radius, float HalfHeight)
{
	UE1CollisionRadius = FMath::Max(0.0f, Radius);
	UE1CollisionHalfHeight = FMath::Max(0.0f, HalfHeight);
	Capsule->SetCapsuleSize(UE1CollisionRadius,
		WoTUE1Collision::ContainingCapsuleHalfHeight(
			UE1CollisionRadius, UE1CollisionHalfHeight), false);
}

void AWoTJumper::BeginPlay()
{
	Super::BeginPlay();
	// Jumper is a non-simulated UE1 trigger.  A locally projected client copy
	// must not inherit ROLE_Authority merely because it is non-replicated.
	if (!GetIsReplicated() && GetNetMode() == NM_Client)
	{
		SetRole(ROLE_None);
		Capsule->SetGenerateOverlapEvents(false);
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

bool AWoTJumper::AcceptsPawn(const APawn* Pawn) const
{
	if (!IsValid(Pawn) || LimitedToClassIdentity.IsNone()) return IsValid(Pawn);
	FString Identity = LimitedToClassIdentity.ToString();
	int32 Export = INDEX_NONE;
	if (Identity.FindLastChar(TEXT('#'), Export)
		&& FStringView(Identity).Mid(Export).StartsWith(
			TEXT("#export-"), ESearchCase::IgnoreCase))
	{
		const FStringView Digits = FStringView(Identity).Mid(Export + 8);
		bool bNumeric = !Digits.IsEmpty();
		for (const TCHAR Character : Digits)
			bNumeric = bNumeric && FChar::IsDigit(Character);
		if (bNumeric) Identity.LeftInline(Export);
	}
	if (Identity.EndsWith(TEXT(".WOTPawn"), ESearchCase::IgnoreCase)
		|| Identity.Equals(TEXT("WOTPawn"), ESearchCase::IgnoreCase))
	{
		return Pawn->IsA<AWoTNPCCharacter>();
	}
	if (Identity.EndsWith(TEXT(".Grunt"), ESearchCase::IgnoreCase)
		|| Identity.Equals(TEXT("Grunt"), ESearchCase::IgnoreCase))
	{
		// Every currently ported retail creature row derives through Grunt;
		// players do not. CreatureRowName retains the concrete retail type.
		return Pawn->IsA<AWoTNPCCharacter>();
	}
	return false;
}

void AWoTJumper::OnOverlap(UPrimitiveComponent*, AActor* Other,
	UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	if (!HasAuthority()) return;
	APawn* Pawn = Cast<APawn>(Other);
	if (!AcceptsPawn(Pawn)) return;
	const auto Inside = [this](const AActor* Candidate)
		{
			return WoTUE1Collision::CylindersOverlap(GetActorLocation(),
				UE1CollisionRadius, UE1CollisionHalfHeight, Candidate);
		};
	if (!Inside(Pawn))
	{
		UE1Touches.DeferOutside(*this, Capsule, Pawn, Inside,
			[this](AActor* Candidate)
			{ OnOverlap(Capsule, Candidate, nullptr, 0, false, FHitResult()); });
		return;
	}
	Pending = Pawn;
	GetWorldTimerManager().SetTimer(
		LaunchTimer, this, &AWoTJumper::LaunchPending, 0.01f, false);
	if (bOnceOnly)
	{
		Capsule->SetGenerateOverlapEvents(false);
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AWoTJumper::LaunchPending()
{
	APawn* Pawn = Pending.Get();
	Pending.Reset();
	if (!IsValid(Pawn) || !HasAuthority()) return;

	Pawn->SetActorRotation(GetActorRotation());
	float GroundSpeed = 0.0f;
	float VerticalSpeed = JumpZ;
	if (ACharacter* Character = Cast<ACharacter>(Pawn))
	{
		if (UCharacterMovementComponent* Movement =
			Character->GetCharacterMovement())
		{
			GroundSpeed = Movement->MaxWalkSpeed;
			if (VerticalSpeed == 0.0f)
				VerticalSpeed = FMath::Max(100.0f, Movement->JumpZVelocity);
			Movement->SetMovementMode(MOVE_Falling);
		}
	}
	else if (UPawnMovementComponent* Movement = Pawn->GetMovementComponent())
	{
		GroundSpeed = Movement->GetMaxSpeed();
	}
	if (VerticalSpeed == 0.0f) VerticalSpeed = 100.0f;
	FVector Velocity = GetActorForwardVector() * GroundSpeed;
	Velocity.Z = VerticalSpeed;
	if (UPawnMovementComponent* Movement = Pawn->GetMovementComponent())
		Movement->Velocity = Velocity;
}
