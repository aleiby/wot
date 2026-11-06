#include "WoTNPCWeaponData.h"

#include "../Projectiles/WoTWeaponProjectile.h"

namespace
{
	FString BareName(FName Ref)
	{
		FString Name = Ref.ToString();
		int32 Dot = INDEX_NONE;
		if (Name.FindLastChar(TEXT('.'), Dot))
		{
			Name = Name.RightChop(Dot + 1);
		}
		return Name;
	}

	TUniquePtr<FWoTNPCWeaponSet>& InstalledWeaponRegistry()
	{
		static TUniquePtr<FWoTNPCWeaponSet> Installed;
		return Installed;
	}

	const TMap<FName, FWoTNPCWeaponConfig>& ActiveWeaponRegistry()
	{
		const TUniquePtr<FWoTNPCWeaponSet>& Installed = InstalledWeaponRegistry();
		if (Installed) return Installed->Rows;
		static const TMap<FName, FWoTNPCWeaponConfig> Empty;
		return Empty;
	}
}

void FWoTNPCWeaponDataBuilder::Install(FWoTNPCWeaponSet&& Set)
{
	TUniquePtr<FWoTNPCWeaponSet> Candidate =
		MakeUnique<FWoTNPCWeaponSet>(MoveTemp(Set));
	Swap(InstalledWeaponRegistry(), Candidate);
}

#if WITH_AUTOMATION_TESTS
void FWoTNPCWeaponDataBuilder::Reset()
{
	InstalledWeaponRegistry().Reset();
}
#endif

const FWoTNPCWeaponConfig* WoTNPCWeaponData::Find(FName WeaponType)
{
	return ActiveWeaponRegistry().Find(WeaponType);
}

void WoTNPCWeaponData::GetAllWeaponTypes(TArray<FName>& OutTypes)
{
	ActiveWeaponRegistry().GetKeys(OutTypes);
}

FSoftObjectPath WoTNPCWeaponData::ResolveMeshPath(FName MeshRef)
{
	if (MeshRef.IsNone())
	{
		return FSoftObjectPath();
	}
	const FString Name = BareName(MeshRef);
	// Weapon visuals use the same deterministic imported-mesh tree as creature
	// rows. The ref stays soft; AWoTNPCWeapon never performs a hard TryLoad.
	return FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Meshes/%s/%s/SkeletalMeshes/%s.%s"), *Name, *Name, *Name, *Name));
}

FSoftObjectPath WoTNPCWeaponData::ResolveTexturePath(FName TextureRef)
{
	if (TextureRef.IsNone())
	{
		return FSoftObjectPath();
	}
	const FString Name = BareName(TextureRef);
	return FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/WoTPawns/%s.%s"), *Name, *Name));
}

FSoftObjectPath WoTNPCWeaponData::ResolveSoundPath(FName SoundRef)
{
	if (SoundRef.IsNone())
	{
		return FSoftObjectPath();
	}
	const FString Name = BareName(SoundRef);
	return FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Sounds/WoTPawns/%s.%s"), *Name, *Name));
}
