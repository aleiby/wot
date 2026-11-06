// Regression coverage for the UE1 sprite style split: STY_Modulated is the
// single PF_Modulated (2*Tex*Dst) pass, while STY_Translucent remains the
// ordered particle-modulate + particle-add pair.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Projectiles/WoTSoulBarbProjectile.h"
#include "../Traps/WoTFireWall.h"
#include "../WoTBloodFX.h"
#include "../WoTDecal.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTWeaveFX.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

namespace
{
	constexpr const TCHAR* MeshModulate = TEXT("M_WoT_MeshModulate");
	constexpr const TCHAR* ParticleMod = TEXT("M_WoT_ParticleMod");
	constexpr const TCHAR* ParticleAdd = TEXT("M_WoT_ParticleAdd");
	constexpr const TCHAR* ParticleModFB = TEXT("M_WoT_ParticleModFB");
	constexpr const TCHAR* ParticleAddFB = TEXT("M_WoT_ParticleAddFB");

	FActorSpawnParameters AlwaysSpawn()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	FString RootMaterialName(const UMaterialInterface* Material)
	{
		const UMaterial* Root = Material ? Material->GetMaterial() : nullptr;
		return Root ? Root->GetName() : FString();
	}

	int32 CheckPassRoots(FAutomationTestBase& Test, const AActor* Owner,
		const TCHAR* ComponentMarker, const TCHAR* ExpectedRoot)
	{
		if (!Owner)
		{
			Test.AddError(FString::Printf(TEXT("%s owner is missing"), ComponentMarker));
			return 0;
		}
		TInlineComponentArray<UInstancedStaticMeshComponent*> Components(Owner);
		int32 Matches = 0;
		for (const UInstancedStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->GetName().Contains(ComponentMarker))
			{
				continue;
			}
			++Matches;
			Test.TestEqual(*FString::Printf(TEXT("%s/%s material root"),
				*Owner->GetName(), *Component->GetName()),
				RootMaterialName(Component->GetMaterial(0)), FString(ExpectedRoot));
		}
		Test.TestTrue(*FString::Printf(TEXT("%s has a %s component"),
			*Owner->GetName(), ComponentMarker), Matches > 0);
		return Matches;
	}

	void CheckNoPass(FAutomationTestBase& Test, const AActor* Owner,
		const TCHAR* ComponentMarker)
	{
		if (!Owner) return;
		TInlineComponentArray<UInstancedStaticMeshComponent*> Components(Owner);
		for (const UInstancedStaticMeshComponent* Component : Components)
		{
			Test.TestFalse(*FString::Printf(TEXT("%s has no %s pass"),
				*Owner->GetName(), ComponentMarker),
				Component && Component->GetName().Contains(ComponentMarker));
		}
	}

	UTexture* FindPassTexture(const AActor* Owner, const TCHAR* ComponentMarker)
	{
		if (!Owner) return nullptr;
		TInlineComponentArray<UInstancedStaticMeshComponent*> Components(Owner);
		for (const UInstancedStaticMeshComponent* Component : Components)
		{
			if (!Component || !Component->GetName().Contains(ComponentMarker))
			{
				continue;
			}
			UTexture* Texture = nullptr;
			UMaterialInterface* Material = Component->GetMaterial(0);
			if (Material && Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("Tex")), Texture))
			{
				return Texture;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTModulatedSpriteMasterSpec,
	"WoT.Rendering.ModulatedSprites.UseTwoXMaster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTModulatedSpriteMasterSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("standalone world"), World))
	{
		return false;
	}
	UMaterial* MeshModulateMaster = LoadObject<UMaterial>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"));
	if (TestNotNull(TEXT("2x modulate master"), MeshModulateMaster))
	{
		TestTrue(TEXT("2x master supports instanced static meshes"),
			MeshModulateMaster->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes));
	}

	// ParticleSprayer STY_Modulated: every shipped blood leaf uses this path.
	AWoTParticleSprayer* Blood = WoTBloodFX::SpawnLocal(*World,
		TEXT("BloodDamageRed1"), FVector(0, 0, 100), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("blood sprayer"), Blood))
	{
		CheckPassRoots(*this, Blood, TEXT("PS_Mod_"), MeshModulate);
		CheckNoPass(*this, Blood, TEXT("PS_Add_"));
	}
	AWoTDecal* DecayDrop = WoTDecalFX::SpawnFlying(*World,
		TEXT("DecaySplatter"), FVector(500, 0, 100), FVector::DownVector);
	if (TestNotNull(TEXT("DecaySplatter flying sprite"), DecayDrop))
	{
		CheckPassRoots(*this, DecayDrop, TEXT("FlyingSprite_Mod"), MeshModulate);
		CheckNoPass(*this, DecayDrop, TEXT("FlyingSprite_Add"));
	}

	// FireWallSprayer2 is modulated; Sprayer1/3 inherit STY_Translucent.
	AWoTFireWallSlab* FireWall = World->SpawnActor<AWoTFireWallSlab>(
		AWoTFireWallSlab::StaticClass(), FVector(1000, 0, 100),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("fire wall slab"), FireWall))
	{
		for (AWoTParticleSprayer* Sprayer : FireWall->FlameSprayers)
		{
			if (!TestNotNull(TEXT("fire wall sprayer"), Sprayer)) continue;
			if (Sprayer->bModulateOnly)
			{
				CheckPassRoots(*this, Sprayer, TEXT("PS_Mod_"), MeshModulate);
				CheckNoPass(*this, Sprayer, TEXT("PS_Add_"));
			}
			else
			{
				CheckPassRoots(*this, Sprayer, TEXT("PS_Mod_"), ParticleModFB);
				CheckPassRoots(*this, Sprayer, TEXT("PS_Add_"), ParticleAddFB);
			}
		}
	}

	// GenericSprite users: SoulBarb's Hole is STY_Modulated and its Flare is
	// STY_Translucent.
	AWoTSoulBarbProjectile* SoulBarb = World->SpawnActor<AWoTSoulBarbProjectile>(
		AWoTSoulBarbProjectile::StaticClass(), FVector(2000, 0, 100),
		FRotator::ZeroRotator, AlwaysSpawn());
	if (TestNotNull(TEXT("SoulBarb projectile"), SoulBarb))
	{
		CheckPassRoots(*this, SoulBarb, TEXT("Hole_Mod"), MeshModulate);
		CheckNoPass(*this, SoulBarb, TEXT("Hole_Add"));
		CheckPassRoots(*this, SoulBarb, TEXT("Flare_Mod"), ParticleModFB);
		CheckPassRoots(*this, SoulBarb, TEXT("Flare_Add"), ParticleAddFB);
	}

	// TaintVisual's procedural center is STY_Modulated; the inner glow retains
	// the translucent pair.
	AActor* Follow = World->SpawnActor<AActor>(
		AActor::StaticClass(), FVector(3000, 0, 100),
		FRotator::ZeroRotator, AlwaysSpawn());
	AWoTTaintVisual* Taint = WoTMeshFX::SpawnTaintVisual(*World, Follow);
	if (TestNotNull(TEXT("Taint visual"), Taint))
	{
		CheckPassRoots(*this, Taint, TEXT("Center_Mod"), MeshModulate);
		CheckNoPass(*this, Taint, TEXT("Center_Add"));
		CheckPassRoots(*this, Taint, TEXT("Glow_Mod"), ParticleMod);
		CheckPassRoots(*this, Taint, TEXT("Glow_Add"), ParticleAdd);

		UWoTFractalCatalog* Catalog = LoadObject<UWoTFractalCatalog>(nullptr,
			TEXT("/Game/WoT/Data/DA_WoTFractalCatalog.DA_WoTFractalCatalog"));
		const FWoTFractalTextureRecord* TaintRecord = Catalog
			? Catalog->Textures.FindByPredicate(
				[](const FWoTFractalTextureRecord& Record)
				{
					return Record.PackageName == TEXT("Angreal")
						&& Record.TextureName == TEXT("TntCntrA");
				})
			: nullptr;
		if (TestNotNull(TEXT("real TntCntrA catalog record"), TaintRecord))
		{
			TestEqual(TEXT("TntCntrA is a WetTexture record"),
				static_cast<int32>(TaintRecord->FractalClass),
				static_cast<int32>(EWoTFractalClass::Wet));
			TestFalse(TEXT("TntCntrA names its retail source texture"),
				TaintRecord->SourceTextureRef.IsEmpty());
			TestTrue(TEXT("TntCntrA catalog embeds real source pixels"),
				!TaintRecord->SourceIndices.IsEmpty());
			int32 NeutralSourcePixels = 0;
			if (TestEqual(TEXT("TntCntrA carries a complete retail palette"),
				TaintRecord->PaletteRGBA.Num(), 256 * 4))
			{
				for (const uint8 SourceIndex : TaintRecord->SourceIndices)
				{
					const int32 PaletteOffset = static_cast<int32>(SourceIndex) * 4;
					if (TaintRecord->PaletteRGBA[PaletteOffset + 0] == 128
						&& TaintRecord->PaletteRGBA[PaletteOffset + 1] == 128
						&& TaintRecord->PaletteRGBA[PaletteOffset + 2] == 128)
					{
						++NeutralSourcePixels;
					}
				}
				TestTrue(TEXT("TntCntrA source contains modulate-neutral pixels"),
					NeutralSourcePixels > 0);
			}
			AddInfo(FString::Printf(TEXT("TntCntrA SourceTextureRef=%s"),
				*TaintRecord->SourceTextureRef));

			UWoTFractalSubsystem* Fractals =
				World->GetSubsystem<UWoTFractalSubsystem>();
			UTexture2D* LiveTexture = Fractals
				? Fractals->FindLiveTexture(FName(*TaintRecord->QualifiedName()))
				: nullptr;
			if (TestNotNull(TEXT("TntCntrA live texture is activated"), LiveTexture))
			{
				TestTrue(TEXT("TntCntrA live texture has the live identity"),
					LiveTexture->GetName().StartsWith(TEXT("WoTLive_")));
				TestFalse(TEXT("TntCntrA live texture preserves raw display bytes"),
					LiveTexture->SRGB);
			}
			UTexture* BoundTexture = FindPassTexture(Taint, TEXT("Center_Mod"));
			AddInfo(FString::Printf(TEXT("Taint center bound Tex=%s"),
				BoundTexture ? *BoundTexture->GetPathName() : TEXT("None")));
			TestTrue(TEXT("Taint center samples the subsystem live texture"),
				LiveTexture && BoundTexture == LiveTexture);
		}
	}

	// The generic mesh-sprite registry and weave sprayer registry currently
	// contain only STY_Translucent rows. Exercise one real row through each
	// production factory so their audited darken+add behavior cannot collapse.
	AWoTWeaveSpriteEffect* RegistrySprite = WoTMeshFX::SpawnSpriteLocal(
		*World, TEXT("SoulBarbEffect"), FVector(4000, 0, 100));
	if (TestNotNull(TEXT("translucent sprite-registry effect"), RegistrySprite))
	{
		CheckPassRoots(*this, RegistrySprite, TEXT("Sprite_Mod"), ParticleMod);
		CheckPassRoots(*this, RegistrySprite, TEXT("Sprite_Add"), ParticleAdd);
	}
	AWoTParticleSprayer* WeaveSprayer = WoTWeaveFX::SpawnLocal(
		*World, TEXT("SoulBarbSmoke"), FVector(5000, 0, 100),
		FRotator::ZeroRotator);
	if (TestNotNull(TEXT("translucent weave sprayer"), WeaveSprayer))
	{
		TestFalse(TEXT("weave row is not modulate-only"),
			WeaveSprayer->bModulateOnly);
		CheckPassRoots(*this, WeaveSprayer, TEXT("PS_Mod_"), ParticleMod);
		CheckPassRoots(*this, WeaveSprayer, TEXT("PS_Add_"), ParticleAdd);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
