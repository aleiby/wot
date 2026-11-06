// Pickup actors whose retail class supplies the mesh texture via
// Skin= (UE1 mesh GetTexture: actor Skin fills texture-less slots and slot 0)
// must carry that skin as a baked override material on the generated leaf BP.
// Without it the mesh's neutral typed MIC renders the master's parameter
// default — the "solid tan, black away from the light" Dart.

#include "CoreMinimal.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Engine/Texture.h"
#include "../Inventory/WoTInventory.h"
#include "../WoTVertexAnimComponent.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FWoTPickupSkinCase
	{
		const TCHAR* ClassName;   // BP_<ClassName> under Blueprints/Pickups
		const TCHAR* SkinTexture; // Skin= (exports/script/Angreal/<class>.uc)
	};

	// The six STY_Masked Skin= pickup classes. AngrealInvTracer also authors
	// Skin= but has no Style override, and retail's ordinary mesh path never
	// inherits texture flags (PORT_NOTES C02) — its embedded slot texture IS
	// the skin, so it stays on the mesh's own reconstruct-path MIC.
	const FWoTPickupSkinCase GWoTPickupSkinCases[] =
	{
		{ TEXT("WoTAngrealInvDart"),         TEXT("DartAesSedai") },
		{ TEXT("WoTAngrealInvFireShield"),   TEXT("ElShieldRED") },
		{ TEXT("WoTAngrealInvAirShield"),    TEXT("ElShieldWHITE") },
		{ TEXT("WoTAngrealInvWaterShield"),  TEXT("ElShieldBLUE") },
		{ TEXT("WoTAngrealInvEarthShield"),  TEXT("ElShieldGREEN") },
		{ TEXT("WoTAngrealInvSpiritShield"), TEXT("ElShieldGOLD") },
	};

	const UMaterialInterface* WoTPickupSkinRoot(const UMaterialInterface* Material)
	{
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
		while (Instance && Instance->Parent)
		{
			Material = Instance->Parent;
			Instance = Cast<UMaterialInstance>(Material);
		}
		return Material;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPickupSkinOverrideTest,
	"WoT.Inventory.PickupSkins.MaskedSkinOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPickupSkinOverrideTest::RunTest(const FString&)
{
	for (const FWoTPickupSkinCase& Case : GWoTPickupSkinCases)
	{
		const FString BPPath = FString::Printf(
			TEXT("/Game/WoT/Blueprints/Pickups/BP_%s.BP_%s_C"),
			Case.ClassName, Case.ClassName);
		UClass* PickupClass = LoadClass<AWoTInventory>(nullptr, *BPPath);
		if (!TestNotNull(*FString::Printf(TEXT("%s: pickup BP loads"),
			Case.ClassName), PickupClass))
		{
			continue;
		}
		const AWoTInventory* CDO = PickupClass->GetDefaultObject<AWoTInventory>();
		const UWoTVertexAnimComponent* Mesh = CDO ? CDO->Mesh.Get() : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("%s: CDO mesh component"),
			Case.ClassName), Mesh))
		{
			continue;
		}
		const UMaterialInterface* Override =
			Mesh->OverrideMaterials.IsValidIndex(0)
				? Mesh->OverrideMaterials[0] : nullptr;
		if (!TestNotNull(*FString::Printf(
			TEXT("%s: slot-0 skin override material present"),
			Case.ClassName), Override))
		{
			continue;
		}
		const UMaterialInterface* Root = WoTPickupSkinRoot(Override);
		TestTrue(*FString::Printf(
			TEXT("%s: skin override roots at the masked mesh master (got %s)"),
			Case.ClassName, Root ? *Root->GetName() : TEXT("null")),
			Root && Root->GetName() == TEXT("M_WoT_MeshLitMasked"));
		UTexture* BoundTexture = nullptr;
		Override->GetTextureParameterValue(
			FMaterialParameterInfo(TEXT("Tex")), BoundTexture);
		TestTrue(*FString::Printf(
			TEXT("%s: skin override binds Tex=%s (got %s)"),
			Case.ClassName, Case.SkinTexture,
			BoundTexture ? *BoundTexture->GetName() : TEXT("null")),
			BoundTexture && BoundTexture->GetName() == Case.SkinTexture);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
