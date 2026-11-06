#pragma once

#include "CoreMinimal.h"
#include "../WoTRetailProjectileTarget.h"
#include "GameFramework/Actor.h"
#include "WoTWeaponComponent.h"
#include "WoTNPCWeapon.generated.h"

class AWoTNPCCharacter;
class AWoTWeaponProjectile;
class UProjectileMovementComponent;
class UStaticMeshComponent;
class UStaticMesh;
class USkeletalMesh;
class UWoTVertexAnimComponent;
class UTexture;
class USoundBase;
struct FWoTNPCWeaponConfig;

DECLARE_MULTICAST_DELEGATE_OneParam(FWoTNPCWeaponDestroyed, class AWoTNPCWeapon*);

// A rendered, held WOT.WotWeapon actor. Configuration is applied from the
// UE1-name registry after spawn and replayed on clients through WeaponTypeName.
UCLASS()
class AWoTNPCWeapon : public AActor
{
	GENERATED_BODY()

public:
	AWoTNPCWeapon();

	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TObjectPtr<UStaticMeshComponent> WeaponMesh;
	// Imported UE1 vertex meshes are skeletal assets in this repo. The static root
	// is the required held-actor primitive; this no-collision child renders the
	// actual imported payload using the creature-row mesh layout.
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TObjectPtr<UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(ReplicatedUsing = OnRep_WeaponType) FName WeaponTypeName;
	UPROPERTY(ReplicatedUsing = OnRep_Equipped) bool bEquipped = false;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MinMeleeRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MaxMeleeRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MeleeEffectiveness = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MinProjectileRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MaxProjectileRange = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float ProjectileEffectiveness = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") TSubclassOf<AWoTWeaponProjectile> ProjectileClass;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") bool bRequireTargetVisibility = true;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float MissOdds = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") FName TypeCapableOfPreparation;

	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float FireNotifyTime = 0.4f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Weapon") float DrawTime = 0.6f;

	UPROPERTY(ReplicatedUsing = OnRep_Health, VisibleAnywhere, Category = "WoT|Weapon") int32 Health = 0;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") int32 DefaultHealth = 0;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TSoftObjectPtr<UTexture> CurrentSkin;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TSoftObjectPtr<UTexture> DamageSkin0;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TSoftObjectPtr<UTexture> DamageSkin1;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TSoftObjectPtr<UTexture> DamageSkin2;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TSoftObjectPtr<USoundBase> DestroyedSound;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TSoftObjectPtr<USkeletalMesh> WeaponMeshAsset;
	// WotWeapon leaves author MultiSkins by material index. CurrentSkin is the
	// damage-state Skin override and wins over these base slot textures (uc:256-281).
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") TArray<TSoftObjectPtr<UTexture>> SkinOverrides;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") bool bStyleMasked = false;
	UPROPERTY(ReplicatedUsing = OnRep_FadePresentation, VisibleAnywhere, Category = "WoT|Weapon") bool bStyleTranslucent = false;
	UPROPERTY(ReplicatedUsing = OnRep_FadePresentation, VisibleAnywhere, Category = "WoT|Weapon") float ScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") FName TossedDecorationType;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Weapon") float MPTossedLifeSpan = 60.0f;

	FWoTNPCWeaponDestroyed OnWeaponDestroyed;

	void ConfigureFromType(FName WeaponType);
	virtual void DetermineWeaponUsage(float GoalDistance, bool bGoalVisible,
		float Random01 = -1.0f);
	// RangeHandlerSniping evaluates continuously in the folded controller. Cache
	// only its stochastic input between offensive notifications; range/LOS remain live.
	void RefreshOddsRoll(float Random01 = -1.0f);
	float GetCachedOddsRoll() const { return CachedOddsRoll; }
	EWoTWeaponUsage GetWeaponUsage() const { return DeterminedWeaponUsage; }
	bool CanUseAsMeleeWeapon(float GoalDistance) const;
	bool CanUseAsProjectileWeapon(float GoalDistance) const;
	bool ShouldBePreparedBy(const AActor* PreparingActor) const;

	virtual void Equip(AWoTNPCCharacter* OwnerPawn);
	void Unequip();
	virtual bool IsInFlight() const { return false; }
	virtual void UseProjectileWeapon(APawn* Invoker, AActor* Target);
	virtual AWoTWeaponProjectile* FireProjectile(APawn* Invoker, const FVector& Source,
		const FRotator& Direction);
	virtual void DropFrom(const FVector& StartLocation);
	void ApplyFadePresentation();

	static float RandDistance(float Distance, float MinDistance, float MaxDistance,
		float MinOdds, float MaxOdds);

	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
		AController* EventInstigator, AActor* DamageCauser) override;
	virtual void PostInitializeComponents() override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UFUNCTION() void OnRep_WeaponType();
	UFUNCTION() void OnRep_Equipped();
	UFUNCTION() void OnRep_Health();
	UFUNCTION() void OnRep_FadePresentation();
	void ApplyConfig(const FWoTNPCWeaponConfig& Config);
	void ApplyVisuals();
	void ApplyDamageSkin();
	void ApplyCurrentSkinVisual();
	void GetAimParams(APawn* Invoker, AActor* Target, FVector& OutSource,
		FRotator& OutDirection) const;

	EWoTWeaponUsage DeterminedWeaponUsage = EWoTWeaponUsage::None;
	float CachedOddsRoll = 0.0f;
	bool bDestroyedEventSent = false;
};

UCLASS()
class AWoTNPCThrowableWeapon : public AWoTNPCWeapon
{
	GENERATED_BODY()

public:
	virtual AWoTWeaponProjectile* FireProjectile(APawn* Invoker, const FVector& Source,
		const FRotator& Direction) override;
	virtual bool IsInFlight() const override;

protected:
	UFUNCTION() void OnThrownProjectileDestroyed(AActor* DestroyedActor);
	UPROPERTY() TObjectPtr<AWoTWeaponProjectile> ActiveProjectile;
};

// Minimal BounceableDecoration equivalent used only for death tosses
// (WotWeapon.uc:101-157). It is replicated presentation, never an inventory item.
UCLASS()
class AWoTTossedWeapon : public AActor, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:
	AWoTTossedWeapon();
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UProjectileMovementComponent> Movement;
	UPROPERTY(EditAnywhere, Category = "WoT") float Mass = 1.0f;
	UPROPERTY(ReplicatedUsing = OnRep_TossedVisual, VisibleAnywhere) FName WeaponTypeName;
	UPROPERTY(ReplicatedUsing = OnRep_TossedVisual, VisibleAnywhere) TSoftObjectPtr<UTexture> SkinOverride;
	UPROPERTY(Replicated, VisibleAnywhere) bool bStyleMasked = false;
	void InitFromWeapon(const AWoTNPCWeapon& SourceWeapon);
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UFUNCTION() void OnRep_TossedVisual();
	void ApplyTossedVisual();
};
