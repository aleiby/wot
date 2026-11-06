#pragma once

#include "CoreMinimal.h"
#include "../AI/WoTPathNodeIterator.h"
#include "WoTGenericProjectile.h"
#include "WoTSeekingProjectile.generated.h"

// Port of Angreal.SeekingProjectile: accelerating pursuit of a Destination
// actor. Direct-line steering when visible; when blocked, steer via the
// navigation network. Spec: exports/script/Angreal/SeekingProjectile.uc.
// Retail path nodes are the default navigation backend; UE5 NavMesh remains a
// live wot.SeekNavBackend A/B arm. UE replicated movement replaces the manual 1Hz
// ServerLocation drift correction.
UCLASS(Abstract)
class AWoTSeekingProjectile : public AWoTGenericProjectile
{
	GENERATED_BODY()

public:
	AWoTSeekingProjectile();

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") float AccelPerSec = 1.0f;
	UPROPERTY(Replicated) float Acceleration = 16.0f;
	UPROPERTY(Replicated) TObjectPtr<AActor> Destination;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") float PathCheckResolution = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") bool bTakeShortcuts = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") bool bDesperateSeeker = false;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") float ReachTolerance = 12.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") bool bNotifiesDestination = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") bool bAlignToVelocity = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Seeking") bool bCollideWorldWhileSeeking = false;
	// Engine.Projectile's SeePlayer notification surface. The G18 autonomous
	// acquisition branch remains deferred; Personal Illusion drives this minimal
	// seam for projectiles whose class/instance opts in.
	UPROPERTY(EditAnywhere, Category = "WoT|Seeking") bool bNotifySeePlayer = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Seeking") float SeePlayerRadius = 0.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|Seeking") float SeePlayerHeight = 0.0f;

	UPROPERTY(ReplicatedUsing = OnRep_Seeking) bool bSeeking = false;
	UPROPERTY(Replicated) bool bDestExists = false;
	UPROPERTY(Replicated) bool bServerReachedDest = false;
	UPROPERTY(Replicated) FVector DestLoc = FVector::ZeroVector;

	virtual void SetDestination(AActor* Dest) override;
	virtual void NotifySeePawn(AActor* SeenPawn);
	virtual void DestinationLost();
	virtual void NotifyReachedDestination();
	virtual void CannotReachDestination();
	virtual void NotifySeekerBounced(AWoTPathNode* OnNode);
	virtual void CalcNextDirection(bool bDesperate = false);
	bool CanDirectlyReachDestination(const FVector& Dest) const;
	virtual bool CanReachDestination(const FVector& Dest);
	virtual bool DestinationExists();
	bool ReachedDestination();
	bool IsCalcNextDirectionDisabled() const { return bDisableCalcNextDirection; }
	// Actor.PreTeleport return value: true cancels Teleporter.Touch. The port's
	// nav point is a collision-free twin, so match the live pad by location.
	bool PreTeleport(const class AWoTTeleporter* Teleporter) const;

	// SeekingProjectile.uc:39-43. These are path-node twins, not gameplay
	// teleporter actors; SourceClass preserves the retail NavigationPoint type.
	UPROPERTY(Transient) TObjectPtr<AWoTPathNode> NavPointNode;
	UPROPERTY(Transient) TObjectPtr<AWoTPathNode> LastPoint;
	UPROPERTY(Transient) TObjectPtr<AWoTPathNode> RestrictedPoint;

	virtual void ProcessTouch(AActor* Other, FVector HitLocation) override;
	virtual void BeginPlay() override;
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other,
	                       UPrimitiveComponent* OtherComp, bool bSelfMoved,
	                       FVector HitLocation, FVector HitNormal,
	                       FVector NormalImpulse, const FHitResult& Hit) override;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	// Destroyed() (client explode-on-destroy) is inherited from AWoTGenericProjectile,
	// which runs it for every persistent-channel (!bNetTemporary) projectile.
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	void EnterSeeking();
	// Virtual: MashadarGuide never re-blocks world statics (bCollideWorld=False for life,
	// MashadarGuide.uc:1941) — the guide overrides the exit re-block.
	virtual void ExitSeeking();
	virtual void ApplySeekingCollisionState();
	UFUNCTION() void OnRep_Seeking();

	float CurrentSpeed = 0.0f;
	float PathCheckTimer = 0.0f;
	FVector NavPoint = FVector::ZeroVector;   // current steering waypoint
	bool bHasNavPoint = false;
	bool bWasSeeking = false;
	bool bDisableCalcNextDirection = false;
	FWoTPathNodeIterator PathNodeIterator;
};
