#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTUE1Collision.h"
#include "WoTTeleporter.generated.h"

// Port of Engine.Teleporter (the original engine class — retail
// Engine ScriptText is stripped) as placed in the Battle maps via
// WOT.TeleporterWithEffects (whose enter/exit effects and sounds are bound by
// PresentEffect). Simulation subset:
// - URL empty -> destination-only (collision off, Teleporter.PostBeginPlay).
// - Touch: bEnabled gate, then teleport the toucher to a
//   RANDOM same-level teleporter whose Tag matches URL (Dest.Accept). The
//   cross-level "LevelName/Tele" and "unreal://" URL forms defer to §7 online.
//   A successful pawn teleport through Engine.Teleporter also broadcasts
//   Incoming.MakeNoise(1.0) through the shared AI-hearing seam. Legend's
//   TeleporterWithEffects override does not call Super and remains silent.
// - Accept: SetLocation at the destination, optional yaw
//   snap + velocity rules (bChangesVelocity/TargetVelocity, bChangesYaw
//   re-aims velocity, bReverses* flip components).
// - WOT Battle gating: retail giMPBattle Disable/EnableTeleporters set
//   per-player WOTPlayer.bTeleportingDisabled, consulted via PreTeleport; the
//   port checks the owning controller's replicated per-player bit, retaining a
//   mode-wide CanTeleport fallback for non-player pawns.
UCLASS()
class AWoTTeleporter : public AActor
{
	GENERATED_BODY()

public:
	AWoTTeleporter();

	// Teleporter.URL — local form: the destination teleporter's Tag.
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Teleporter") FString URL;
	// UE1 Actor.Tag (destination matching key — UE5 AActor has no FName Tag).
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") FName TeleTag;
	UPROPERTY(EditAnywhere, Replicated, Category = "WoT|Teleporter") bool bEnabled = true;
	// TeleporterWithEffects overrides PlayTeleportEffect without calling Super.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter") bool bMakesTeleportNoise = true;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") bool bChangesVelocity = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") bool bChangesYaw = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") bool bReversesX = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") bool bReversesY = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") bool bReversesZ = false;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") FVector TargetVelocity = FVector::ZeroVector;
	// Actor.Event: fired through the event subsystem when a pawn teleports OUT.
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") FName EventName;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") FName EnterEffectConfig;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") FName ExitEffectConfig;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") FName EnterEffectSoundPath;
	UPROPERTY(EditAnywhere, Category = "WoT|Teleporter") FName ExitEffectSoundPath;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter") FName LastEnterEffectConfig;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter") FName LastExitEffectConfig;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter") FName LastEnterEffectSound;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter") FName LastExitEffectSound;

	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter")
	TObjectPtr<class UCapsuleComponent> Collision;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter")
	float UE1CollisionRadius = 18.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Teleporter")
	float UE1CollisionHalfHeight = 40.0f;
	void SetUE1CollisionSize(float Radius, float HalfHeight);

	virtual void BeginPlay() override;
	virtual void NotifyActorBeginOverlap(AActor* Other) override;
	virtual void NotifyActorEndOverlap(AActor* Other) override;

	// Teleporter.Accept: land Incoming here. Returns false if the spot is bad.
	bool Accept(AActor* Incoming);

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	void PresentEffect(bool bExit);
	// The pawn Accept() just placed on us — its arrival begin-overlap must not
	// bounce it straight back (retail's Disable('Touch') window; UE1 also only
	// re-fires Touch after an untouch, which EndOverlap mirrors here).
	// (Protected — access-only widening — so the Pit pair's overlap overrides
	// honor the same guard.)
	UPROPERTY() TWeakObjectPtr<AActor> JustAccepted;
	// Exact flat-cylinder test against the authored UE1 size.
	bool IsWithinUE1Cylinder(const AActor* Other) const;
	// True when Other meets the exact cylinder now. Otherwise Other is tracked
	// while it stays in the containing capsule and NotifyActorBeginOverlap runs
	// again on the frame it meets the cylinder (UE1 Touch).
	bool AdmitUE1Touch(AActor* Other);
	WoTUE1Collision::FTouchTracker UE1Touches;
};
