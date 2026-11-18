#pragma once

#include "CoreMinimal.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "Templates/SubclassOf.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTNPCWeaponData.generated.h"

class AWoTNPCWeapon;
class AWoTWeaponProjectile;
class FWoTRuntimeClassDefaults;

// One WotWeapon leaf CDO represented as data. The registry is keyed by the
// shipped UE1 class name (for example WoTPawns.WarderSword), exactly like the
// creature registry's weapon columns.
USTRUCT()
struct FWoTNPCWeaponConfig
{
	GENERATED_BODY()

	UPROPERTY() FName WeaponType;
	UPROPERTY() float MinMeleeRange = 0.0f;
	UPROPERTY() float MaxMeleeRange = 0.0f;
	UPROPERTY() float MeleeEffectiveness = 0.0f;
	UPROPERTY() float MinProjectileRange = 0.0f;
	UPROPERTY() float MaxProjectileRange = 0.0f;
	UPROPERTY() float ProjectileEffectiveness = 0.0f;
	UPROPERTY() TSubclassOf<AWoTWeaponProjectile> ProjectileClass;
	UPROPERTY() bool bRequireTargetVisibility = true;
	UPROPERTY() float MissOdds = 0.0f;
	UPROPERTY() FName TypeCapableOfPreparation;

	UPROPERTY() FName Mesh;
	UPROPERTY() float DrawScale = 1.0f;
	UPROPERTY() TArray<FName> SkinOverrides;
	UPROPERTY() bool bStyleMasked = false;
	UPROPERTY() float ScaleGlow = 1.0f;

	UPROPERTY() int32 Health = 0;
	UPROPERTY() FName DamageSkin0;
	UPROPERTY() FName DamageSkin1;
	UPROPERTY() FName DamageSkin2;
	UPROPERTY() FName DestroyedSound;
	UPROPERTY() FName TossedDecorationType;
	UPROPERTY() float MPTossedLifeSpan = 60.0f;

	UPROPERTY() bool bThrowable = false;
	UPROPERTY() bool bQuestionerOddsGate = false;
};

struct FWoTNPCWeaponSet
{
	TMap<FName, FWoTNPCWeaponConfig> Rows;
};

struct FWoTNPCWeaponDataBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("npc-weapons");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTNPCWeaponSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTNPCWeaponSet& Set);
	static void Install(FWoTNPCWeaponSet&& Set);
	static void SerializeCanonical(const FWoTNPCWeaponSet& Set,
		TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTNPCWeaponSet& Built,
		const FWoTNPCWeaponSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

namespace WoTNPCWeaponData
{
	const FWoTNPCWeaponConfig* Find(FName WeaponType);
	void GetAllWeaponTypes(TArray<FName>& OutTypes);
	FSoftObjectPath ResolveMeshPath(FName MeshRef);
	FSoftObjectPath ResolveTexturePath(FName TextureRef);
	FSoftObjectPath ResolveSoundPath(FName SoundRef);
}
