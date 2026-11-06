#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTAngrealInvWallOfAir.generated.h"

// Port of Angreal.AngrealInvWallOfAir ("Sever") — a non-launcher, sustained
// ter'angreal. Spec: exports/script/Angreal/AngrealInvWallOfAir.uc. On cast it
// finds the best target in view and plants an AngrealWallOfAirProjectile between
// caster and target; two DestroyOnDestroyLeeches (on caster + target) tie the
// wall's life to both pawns. While held it drains a charge every 60/RPM seconds
// (RPM 40 -> 1.5s); releasing, running out of charges, or either pawn dying ends
// the cast. The wall unravels weaves that strike it (WallOfAir projectile).
UCLASS()
class AWoTAngrealInvWallOfAir : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTAngrealInvWallOfAir();

	// Targeting cone half-angle (defaultproperties MaxAngle).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|WallOfAir") float MaxAngle = 22.5f;

	virtual void CastArtifact() override;     // Cast()
	virtual void UnCastArtifact() override;   // UnCast()  (UnrealScript 'singular')
	virtual void Tick(float DeltaTime) override;
	class AWoTWallOfAirProjectile* GetWallForTest() const { return WOA.Get(); }
	class UWoTDestroyOnDestroyLeech* GetCasterAnchorForTest() const { return CastorLeech.Get(); }
	class UWoTDestroyOnDestroyLeech* GetVictimAnchorForTest() const { return VictimLeech.Get(); }
	bool IsCastingForTest() const { return bCasting; }

private:
	// Spawn a DestroyOnDestroyLeech on Victim that will drag DestroyActor down when
	// removed; returns null if the victim's reflector chain refuses it.
	class UWoTDestroyOnDestroyLeech* InstallDestroyLeech(
		AActor* DestroyActor, AActor* Victim, bool bShowIcon);

	UPROPERTY() TObjectPtr<class AWoTWallOfAirProjectile> WOA;
	UPROPERTY() TObjectPtr<class UWoTDestroyOnDestroyLeech> CastorLeech;
	UPROPERTY() TObjectPtr<class UWoTDestroyOnDestroyLeech> VictimLeech;

	bool bCasting = false;
	bool bInUnCast = false;    // UnrealScript 'singular UnCast': no re-entrancy
	float CastTimer = 0.0f;
};
