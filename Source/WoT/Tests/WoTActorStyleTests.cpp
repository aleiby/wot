#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTPayloadActivationTestSupport.h"
#include "WoTTestWorld.h"

#include "../Inventory/WoTInventory.h"
#include "../Inventory/WoTSeal.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../RetailData/WoTRetailBuildContext.h"
#include "../Mesh/WoTRuntimeVertexMesh.h"
#include "../World/WoTRuntimeActorVisuals.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../World/WoTWorldProjector.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../WoTDecoration.h"
#include "../WoTFragment.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSpriteBillboard.h"

#include "HAL/FileManager.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/MeshAnimation.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/TextureSound.h"

namespace
{
constexpr EAutomationTestFlags WoTActorStyleSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

class FWoTActorStyleObjectWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit,
				TEXT("actor-style retail writer overflow")));
		}
		if (Schema == TEXT("wot-world-v3")
			|| Schema == TEXT("wot-class-default-v3")
			|| Schema == TEXT("wot-mesh-v3")
			|| Schema == TEXT("wot-animation-v3")
			|| Schema == TEXT("wot-texture-v3")
			|| Schema == TEXT("wot-procedural-v3"))
		{
			TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
			Copy.Append(Bytes.GetData(), Bytes.Num());
			Schemas.Add(FString(LogicalId), FString(Schema));
		}
		return FWoTBootstrapStatus::Success();
	}

	TMap<FString, TArray<uint8>> Objects;
	TMap<FString, FString> Schemas;
};

class FWoTActorStyleProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

const FWoTRuntimeWorldActor* WoTActorStyleFindActor(
	const FWoTRuntimeWorldResource& Resource, FStringView IdentityLeaf)
{
	return Resource.Actors().FindByPredicate(
		[IdentityLeaf](const FWoTRuntimeWorldActor& Actor)
		{
			return FStringView(Actor.Identity).EndsWith(
				IdentityLeaf, ESearchCase::IgnoreCase);
		});
}

struct FWoTActorStyleCacheRoot
{
	FWoTActorStyleCacheRoot()
	{
		Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(),
			TEXT("wot-actor-style-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache")));
		IFileManager::Get().MakeDirectory(*Path, true);
	}
	~FWoTActorStyleCacheRoot()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true);
	}
	FString Path;
};

struct FWoTActorStyleActivationScope
{
	FWoTActorStyleActivationScope()
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
		UWoTRuntimeAssets::SetPayloadTextureResolutionForTesting(true);
	}
	~FWoTActorStyleActivationScope()
	{
		UWoTRuntimeAssets::SetPayloadTextureResolutionForTesting(false);
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
	}
};

const UMaterialInterface* WoTActorStyleRootMaterial(
	const UMaterialInterface* Material)
{
	const UMaterialInterface* Current = Material;
	while (IsValid(Current))
	{
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
		if (!Instance || !IsValid(Instance->Parent)) return Current;
		Current = Instance->Parent;
	}
	return nullptr;
}

bool WoTActorStyleImportPackage(FAutomationTestBase& Test,
	const FString& RetailRoot, const FString& SourcePath,
	const FWoTRetailContract& Contract, FWoTActorStyleObjectWriter& Writer,
	FWoTActorStyleProgress& Progress, bool bMesh, bool bTexture,
	bool bProcedural)
{
	TArray<uint8> Bytes;
	if (!Test.TestTrue(SourcePath + TEXT(" loads"),
		FFileHelper::LoadFileToArray(Bytes, *(RetailRoot / SourcePath))))
	{
		return false;
	}
	FWoTRetailContract PackageContract = Contract;
	PackageContract.Facts.RemoveAll(
		[&SourcePath](const FWoTRequiredFact& Fact)
		{
			return Fact.SourcePath != SourcePath;
		});
	if (bMesh)
	{
		const TWoTBootstrapResult<TArray<FWoTCacheBlob>> Result =
			ImportMeshAnimationPackageBytesForTesting(
				Bytes, SourcePath, PackageContract, Writer, Progress);
		if (!Test.TestFalse(SourcePath + TEXT(" mesh import"), Result.IsError()))
		{
			if (Result.IsError()) Test.AddError(Result.GetError().ToString());
			return false;
		}
	}
	if (bTexture)
	{
		const TWoTBootstrapResult<TArray<FWoTCacheBlob>> Result =
			ImportTexturePackageBytesForTesting(
				Bytes, SourcePath, PackageContract, Writer, Progress);
		if (!Test.TestFalse(SourcePath + TEXT(" texture import"), Result.IsError()))
		{
			if (Result.IsError()) Test.AddError(Result.GetError().ToString());
			return false;
		}
	}
	if (bProcedural)
	{
		const TWoTBootstrapResult<TArray<FWoTCacheBlob>> Result =
			ImportFontProceduralPackageBytesForTesting(
				Bytes, SourcePath, PackageContract, Writer, Progress);
		if (!Test.TestFalse(SourcePath + TEXT(" procedural import"), Result.IsError()))
		{
			if (Result.IsError()) Test.AddError(Result.GetError().ToString());
			return false;
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTActorStyleRealRetailDecodeSpec,
	"WoT.World.ActorStyle.RealRetailDecode",
	WoTActorStyleSpecFlags)

bool FWoTActorStyleRealRetailDecodeSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString RetailRoot = WoTReferencePath(TEXT("original"));
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"),
		LoadedContract.IsError()))
	{
		if (LoadedContract.IsError()) AddError(LoadedContract.GetError().ToString());
		return false;
	}

	FWoTRetailContract WorldContract;
	FWoTRetailContract DefaultsContract;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
	TArray<FString> DefaultLogicalIds;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		const bool bMultiplayerWorld =
			(Fact.Domain == EWoTFactDomain::World
				|| Fact.Domain == EWoTFactDomain::BSP)
			&& !Fact.SourcePath.Equals(
				TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
		if (bMultiplayerWorld) WorldContract.Facts.Add(Fact);
		if (bMultiplayerWorld && Fact.Domain == EWoTFactDomain::World)
		{
			FWoTWorldPackageBytesForTesting& Source =
				WorldSources.AddDefaulted_GetRef();
			Source.SourcePath = Fact.SourcePath;
			if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
				FFileHelper::LoadFileToArray(
					Source.Bytes, *(RetailRoot / Fact.SourcePath))))
			{
				return false;
			}
		}
		if (Fact.Domain == EWoTFactDomain::ClassDefault)
		{
			DefaultsContract.Facts.Add(Fact);
			FWoTRemainingSourceBytesForTesting& Source =
				DefaultSources.AddDefaulted_GetRef();
			Source.SourcePath = Fact.SourcePath;
			if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
				FFileHelper::LoadFileToArray(
					Source.Bytes, *(RetailRoot / Fact.SourcePath))))
			{
				return false;
			}
			if (Fact.Outputs.Num() == 1)
			{
				DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
			}
		}
	}
	if (!TestEqual(TEXT("nineteen multiplayer map packages"),
		WorldSources.Num(), 19)
		|| !TestEqual(TEXT("twenty class-default packages"),
			DefaultSources.Num(), 20))
	{
		return false;
	}

	FWoTActorStyleObjectWriter Writer;
	FWoTActorStyleProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorlds =
		ImportRequiredWorldSetPackageBytesForTesting(
			WorldSources, WorldContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds retail world records"),
		ImportedWorlds.IsError()))
	{
		if (ImportedWorlds.IsError()) AddError(ImportedWorlds.GetError().ToString());
		return false;
	}
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
		ImportRegistryLocalizationDefaultsSourcesForTesting(
			DefaultSources, DefaultsContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds retail class defaults"),
		ImportedDefaults.IsError()))
	{
		if (ImportedDefaults.IsError()) AddError(ImportedDefaults.GetError().ToString());
		return false;
	}

	TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
	for (const FString& LogicalId : DefaultLogicalIds)
	{
		TArray<uint8>* Bytes = Writer.Objects.Find(LogicalId);
		if (!TestNotNull(LogicalId + TEXT(" object"), Bytes)) return false;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
				MoveTemp(*Bytes), TEXT("ue1-class-defaults-v2"),
				TEXT("class_default"));
		if (!TestFalse(LogicalId + TEXT(" decodes"), Decoded.IsError()))
		{
			if (Decoded.IsError()) AddError(Decoded.GetError().ToString());
			return false;
		}
		DefaultObjects.Add(MoveTemp(Decoded).GetValue());
	}
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("production class defaults load"),
		Defaults.LoadObjectsForTesting(MoveTemp(DefaultObjects), Error)))
	{
		AddError(Error);
		return false;
	}

	struct FDecodeCase
	{
		const TCHAR* Map;
		const TCHAR* ActorLeaf;
		uint8 ExpectedStyle;
	};
	const FDecodeCase Cases[] = {
		{TEXT("Battle3_01"), TEXT(".twotriangles48"), 4},
		{TEXT("Battle3_01"), TEXT(".GnarledTree0"), 2},
		{TEXT("Battle2_02"), TEXT(".BoxyLamp0"), 2},
		{TEXT("Arena_03"), TEXT(".AngrealInvFireShield0"), 2},
	};
	for (const FDecodeCase& DecodeCase : Cases)
	{
		TArray<uint8>* WorldBytes = Writer.Objects.Find(
			FString(TEXT("cache/world/")) + DecodeCase.Map);
		if (!TestNotNull(FString(DecodeCase.Map) + TEXT(" world object"),
			WorldBytes))
		{
			continue;
		}
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
				*WorldBytes, TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (!TestFalse(FString(DecodeCase.Map) + TEXT(" world decodes"),
			DecodedWorld.IsError()))
		{
			if (DecodedWorld.IsError()) AddError(DecodedWorld.GetError().ToString());
			continue;
		}
		FWoTRuntimeWorldResource Resource;
		if (!TestFalse(FString(DecodeCase.Map) + TEXT(" payload loads"),
			Resource.LoadObjectForTesting(
				MoveTemp(DecodedWorld).GetValue()).IsError()))
		{
			continue;
		}
		const FWoTRuntimeWorldActor* Actor =
			WoTActorStyleFindActor(Resource, DecodeCase.ActorLeaf);
		if (!TestNotNull(FString(DecodeCase.Map) + DecodeCase.ActorLeaf,
			Actor))
		{
			continue;
		}
		FWoTRuntimeActorVisualPresentation Visual;
		if (!TestTrue(FString(DecodeCase.Map) + DecodeCase.ActorLeaf
				+ TEXT(" visual decodes"),
			WoTRuntimeReadActorVisualPresentation(
				Defaults, *Actor, Visual, Error)))
		{
			AddError(Error);
			continue;
		}
		TestEqual(FString(DecodeCase.Map) + DecodeCase.ActorLeaf
			+ TEXT(" retains ERenderStyle byte"),
			static_cast<int32>(Visual.Style),
			static_cast<int32>(DecodeCase.ExpectedStyle));
		TestEqual(FString(DecodeCase.Map) + DecodeCase.ActorLeaf
			+ TEXT(" derives translucent compatibility bool"),
			Visual.bStyleTranslucent, DecodeCase.ExpectedStyle == 3);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTActorStyleRealRetailBindingsSpec,
	"WoT.World.ActorStyle.RealRetailBindings",
	WoTActorStyleSpecFlags)

bool FWoTActorStyleRealRetailBindingsSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString RetailRoot = WoTReferencePath(TEXT("original"));
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"),
		LoadedContract.IsError()))
	{
		if (LoadedContract.IsError()) AddError(LoadedContract.GetError().ToString());
		return false;
	}

	FWoTRetailContract WorldContract;
	FWoTRetailContract DefaultsContract;
	FWoTRetailContract ResourceContract;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
	TArray<FString> DefaultLogicalIds;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		const bool bMultiplayerWorld =
			(Fact.Domain == EWoTFactDomain::World
				|| Fact.Domain == EWoTFactDomain::BSP)
			&& !Fact.SourcePath.Equals(
				TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
		if (bMultiplayerWorld) WorldContract.Facts.Add(Fact);
		if (bMultiplayerWorld && Fact.Domain == EWoTFactDomain::World)
		{
			FWoTWorldPackageBytesForTesting& Source =
				WorldSources.AddDefaulted_GetRef();
			Source.SourcePath = Fact.SourcePath;
			if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
				FFileHelper::LoadFileToArray(
					Source.Bytes, *(RetailRoot / Fact.SourcePath)))) return false;
		}
		if (Fact.Domain == EWoTFactDomain::ClassDefault)
		{
			DefaultsContract.Facts.Add(Fact);
			FWoTRemainingSourceBytesForTesting& Source =
				DefaultSources.AddDefaulted_GetRef();
			Source.SourcePath = Fact.SourcePath;
			if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
				FFileHelper::LoadFileToArray(
					Source.Bytes, *(RetailRoot / Fact.SourcePath)))) return false;
			if (Fact.Outputs.Num() == 1)
				DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
		}

		const bool bDecorationMesh =
			(Fact.Domain == EWoTFactDomain::Mesh
				|| Fact.Domain == EWoTFactDomain::Animation)
			&& ((Fact.SourcePath == TEXT("System/WoTDecorations.u")
				&& (Fact.SourceExport == TEXT("WoTDecorations.twotriangles")
					|| Fact.SourceExport == TEXT("WoTDecorations.GnarledTree")
					|| Fact.SourceExport == TEXT("WoTDecorations.BoxyLamp")
					|| Fact.SourceExport == TEXT("WoTDecorations.BoxyLamp2")))
				|| (Fact.SourcePath == TEXT("System/Angreal.u")
					&& (Fact.SourceExport == TEXT("Angreal.AngrealElementalShield")
						|| Fact.SourceExport == TEXT("Angreal.AngrealGuardianPickup")))
				|| (Fact.SourcePath == TEXT("System/WOT.u")
					&& (Fact.SourceExport == TEXT("WOT.Seal")
						|| Fact.SourceExport == TEXT("WOT.wfrag1")
						|| Fact.SourceExport == TEXT("WOT.wfrag10")))
				|| (Fact.SourcePath == TEXT("System/WoTPawns.u")
					&& Fact.SourceExport == TEXT("WoTPawns.Minion")));
		const bool bTexture = Fact.Domain == EWoTFactDomain::Texture
			&& (Fact.SourceExport == TEXT("ParticleSystems.Decay.decaysplat")
				|| Fact.SourceExport.StartsWith(
					TEXT("ParticleSystems.SmokeBlack64."))
				|| Fact.SourceExport == TEXT("WoTDecorations.Skins.JTwoTriangles0")
				|| Fact.SourceExport == TEXT("WoTDecorations.Skins.JGnarledTree1")
				|| Fact.SourceExport == TEXT("WoTDecorations.Skins.JBoxyLamp0")
				|| Fact.SourceExport == TEXT("WoTDecorations.Skins.JBoxyLamp1")
				|| Fact.SourceExport.StartsWith(TEXT("Angreal.Skins.ElShield"))
				|| Fact.SourceExport == TEXT("Angreal.Skins.JAngrealGuardian1")
				|| Fact.SourceExport == TEXT("WOT.Skins.S_AesSedaiSeal")
				|| Fact.SourceExport == TEXT("WOT.Skins.WoodTex1")
				|| Fact.SourceExport == TEXT("WoTPawns.Skins.JMinion4")
				|| Fact.SourceExport == TEXT("WoTPawns.Skins.MinionEyes")
				|| Fact.SourceExport == TEXT("WorldT.SurfaceWall.TimbFace_900"));
		const bool bProcedural = Fact.Domain == EWoTFactDomain::Procedural
			&& Fact.SourceExport == TEXT("JHPFireEng.RiverWave01");
		if (bDecorationMesh || bTexture || bProcedural)
			ResourceContract.Facts.Add(Fact);
	}
	if (!TestEqual(TEXT("nineteen multiplayer map packages"),
		WorldSources.Num(), 19)
		|| !TestEqual(TEXT("twenty class-default packages"),
			DefaultSources.Num(), 20)) return false;

	FWoTActorStyleObjectWriter Writer;
	FWoTActorStyleProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorlds =
		ImportRequiredWorldSetPackageBytesForTesting(
			WorldSources, WorldContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds retail worlds"),
		ImportedWorlds.IsError()))
	{
		if (ImportedWorlds.IsError()) AddError(ImportedWorlds.GetError().ToString());
		return false;
	}
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
		ImportRegistryLocalizationDefaultsSourcesForTesting(
			DefaultSources, DefaultsContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds retail defaults"),
		ImportedDefaults.IsError()))
	{
		if (ImportedDefaults.IsError()) AddError(ImportedDefaults.GetError().ToString());
		return false;
	}
	if (!WoTActorStyleImportPackage(*this, RetailRoot,
			TEXT("System/WoTDecorations.u"), ResourceContract, Writer, Progress,
			true, true, false)
		|| !WoTActorStyleImportPackage(*this, RetailRoot,
			TEXT("System/Angreal.u"), ResourceContract, Writer, Progress,
			true, true, false)
		|| !WoTActorStyleImportPackage(*this, RetailRoot,
			TEXT("System/WOT.u"), ResourceContract, Writer, Progress,
			true, true, false)
		|| !WoTActorStyleImportPackage(*this, RetailRoot,
			TEXT("System/WoTPawns.u"), ResourceContract, Writer, Progress,
			true, true, false)
		|| !WoTActorStyleImportPackage(*this, RetailRoot,
			TEXT("Textures/WorldT.utx"), ResourceContract, Writer, Progress,
			false, true, false)
		|| !WoTActorStyleImportPackage(*this, RetailRoot,
			TEXT("System/ParticleSystems.u"), ResourceContract, Writer, Progress,
			false, true, false)
		|| !WoTActorStyleImportPackage(*this, RetailRoot,
			TEXT("Textures/JHPFireEng.utx"), ResourceContract, Writer, Progress,
			false, false, true)) return false;

	TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
	for (const FString& LogicalId : DefaultLogicalIds)
	{
		TArray<uint8>* Bytes = Writer.Objects.Find(LogicalId);
		if (!TestNotNull(LogicalId + TEXT(" object"), Bytes)) return false;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
				*Bytes, TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
		if (!TestFalse(LogicalId + TEXT(" decodes"), Decoded.IsError()))
		{
			if (Decoded.IsError()) AddError(Decoded.GetError().ToString());
			return false;
		}
		DefaultObjects.Add(MoveTemp(Decoded).GetValue());
	}
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("production class defaults load"),
		Defaults.LoadObjectsForTesting(MoveTemp(DefaultObjects), Error)))
	{
		AddError(Error);
		return false;
	}

	enum class EBindingKind : uint8
	{
		ModulatedMesh,
		MaskedDecoration,
		MaskedBoxyLamp,
		MaskedPickup,
		ModulatedSprite,
		ProceduralModulatedMesh,
	};
	struct FBindingCase
	{
		const TCHAR* Map;
		const TCHAR* ActorLeaf;
		EBindingKind Kind;
	};
	const FBindingCase Cases[] = {
		{TEXT("Battle3_01"), TEXT(".twotriangles48"),
			EBindingKind::ModulatedMesh},
		{TEXT("Battle2_01"), TEXT(".GnarledTree0"),
			EBindingKind::MaskedDecoration},
		{TEXT("Battle2_02"), TEXT(".BoxyLamp0"),
			EBindingKind::MaskedBoxyLamp},
		{TEXT("Arena_03"), TEXT(".AngrealInvFireShield0"),
			EBindingKind::MaskedPickup},
		{TEXT("Battle2_01"), TEXT(".TwoTriangles13"),
			EBindingKind::ModulatedSprite},
		{TEXT("Battle2_05"), TEXT(".TwoTriangles13"),
			EBindingKind::ProceduralModulatedMesh},
	};
	TArray<FWoTRuntimeWorldActor> Actors;
	TArray<FWoTRuntimeActorVisualPresentation> Visuals;
	for (const FBindingCase& BindingCase : Cases)
	{
		TArray<uint8>* WorldBytes = Writer.Objects.Find(
			FString(TEXT("cache/world/")) + BindingCase.Map);
		if (!TestNotNull(FString(BindingCase.Map) + TEXT(" world object"),
			WorldBytes)) return false;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
				*WorldBytes, TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (!TestFalse(FString(BindingCase.Map) + TEXT(" world decodes"),
			DecodedWorld.IsError())) return false;
		FWoTRuntimeWorldResource Resource;
		if (!TestFalse(FString(BindingCase.Map) + TEXT(" payload loads"),
			Resource.LoadObjectForTesting(
				MoveTemp(DecodedWorld).GetValue()).IsError())) return false;
		const FWoTRuntimeWorldActor* Actor =
			WoTActorStyleFindActor(Resource, BindingCase.ActorLeaf);
		if (!TestNotNull(FString(BindingCase.Map) + BindingCase.ActorLeaf,
			Actor)) return false;
		Actors.Add(*Actor);
		FWoTRuntimeActorVisualPresentation& Visual = Visuals.AddDefaulted_GetRef();
		if (!TestTrue(FString(BindingCase.Map) + BindingCase.ActorLeaf
				+ TEXT(" visual decodes"),
			WoTRuntimeReadActorVisualPresentation(
				Defaults, Actors.Last(), Visual, Error)))
		{
			AddError(Error);
			return false;
		}
	}
	TArray<FWoTRuntimeVertexMeshDataPtr> RuntimeMeshes;
	for (int32 CaseIndex = 0; CaseIndex < Visuals.Num(); ++CaseIndex)
	{
		const FString Suffix = Visuals[CaseIndex].MeshPackage.ToString()
			+ TEXT("__") + Visuals[CaseIndex].MeshName.ToString();
		const FString MeshLogicalId = TEXT("cache/mesh/") + Suffix;
		const FString AnimationLogicalId = TEXT("cache/animation/") + Suffix;
		auto FindWrittenObject = [&Writer](const FString& Wanted)
			-> const TArray<uint8>*
		{
			for (const TPair<FString, TArray<uint8>>& Pair : Writer.Objects)
			{
				if (Pair.Key.Equals(Wanted, ESearchCase::IgnoreCase))
					return &Pair.Value;
			}
			return nullptr;
		};
		const TArray<uint8>* MeshObject =
			FindWrittenObject(MeshLogicalId);
		const TArray<uint8>* AnimationObject =
			FindWrittenObject(AnimationLogicalId);
		if (!TestNotNull(MeshLogicalId + TEXT(" imported object"), MeshObject)
			|| !TestNotNull(AnimationLogicalId + TEXT(" imported object"),
				AnimationObject)) return false;
		TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr> DecodedMesh =
			FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
				*MeshObject, *AnimationObject,
				MeshLogicalId, AnimationLogicalId);
		if (!TestFalse(MeshLogicalId + TEXT(" production payload decodes"),
			DecodedMesh.IsError()))
		{
			if (DecodedMesh.IsError()) AddError(DecodedMesh.GetError().ToString());
			return false;
		}
		RuntimeMeshes.Add(MoveTemp(DecodedMesh).GetValue());
	}
	auto DecodeNamedRuntimeMesh = [this, &Writer](const TCHAR* PackageName,
		const TCHAR* MeshName)
		-> FWoTRuntimeVertexMeshDataPtr
	{
		const FString Suffix = FString(PackageName) + TEXT("__") + MeshName;
		const FString MeshLogicalId = TEXT("cache/mesh/") + Suffix;
		const FString AnimationLogicalId = TEXT("cache/animation/") + Suffix;
		auto FindWrittenObject = [&Writer](const FString& Wanted)
			-> const TArray<uint8>*
		{
			for (const TPair<FString, TArray<uint8>>& Pair : Writer.Objects)
			{
				if (Pair.Key.Equals(Wanted, ESearchCase::IgnoreCase))
					return &Pair.Value;
			}
			return nullptr;
		};
		const TArray<uint8>* MeshObject = FindWrittenObject(MeshLogicalId);
		const TArray<uint8>* AnimationObject = FindWrittenObject(AnimationLogicalId);
		if (!TestNotNull(MeshLogicalId + TEXT(" imported object"), MeshObject)
			|| !TestNotNull(AnimationLogicalId + TEXT(" imported object"),
				AnimationObject))
		{
			return nullptr;
		}
		auto Decoded = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
			*MeshObject, *AnimationObject, MeshLogicalId, AnimationLogicalId);
		if (!TestFalse(MeshLogicalId + TEXT(" production payload decodes"),
			Decoded.IsError()))
		{
			if (Decoded.IsError()) AddError(Decoded.GetError().ToString());
			return nullptr;
		}
		return MoveTemp(Decoded).GetValue();
	};
	const FWoTRuntimeVertexMeshDataPtr GuardianMesh =
		DecodeNamedRuntimeMesh(TEXT("Angreal"), TEXT("AngrealGuardianPickup"));
	const FWoTRuntimeVertexMeshDataPtr SealMesh =
		DecodeNamedRuntimeMesh(TEXT("WOT"), TEXT("Seal"));
	const FWoTRuntimeVertexMeshDataPtr MinionMesh =
		DecodeNamedRuntimeMesh(TEXT("WoTPawns"), TEXT("Minion"));
	const FWoTRuntimeVertexMeshDataPtr WoodFragmentEmbeddedMesh =
		DecodeNamedRuntimeMesh(TEXT("WOT"), TEXT("wfrag1"));
	const FWoTRuntimeVertexMeshDataPtr WoodFragmentSkinMesh =
		DecodeNamedRuntimeMesh(TEXT("WOT"), TEXT("wfrag10"));

	TArray<FWoTPayloadActivationTestObject> PublishObjects;
	for (const TPair<FString, FString>& Pair : Writer.Schemas)
	{
		if (Pair.Value == TEXT("wot-world-v3")
			|| Pair.Value == TEXT("wot-class-default-v3")) continue;
		const TArray<uint8>* Bytes = Writer.Objects.Find(Pair.Key);
		if (Bytes) PublishObjects.Add({Pair.Key, Pair.Value, *Bytes});
	}
	FWoTActorStyleCacheRoot CacheRoot;
	if (!TestTrue(TEXT("real mesh, animation, texture and procedural objects publish"),
		WoTPayloadActivationTest_PublishObjects(
			CacheRoot.Path, PublishObjects, Error)))
	{
		AddError(Error);
		return false;
	}

	FWoTActorStyleActivationScope ActivationScope;
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.GameInstanceClass = UWoTGameInstance::StaticClass();
	WorldOptions.bStartPlay = false;
	WorldOptions.bActivatePayload = false;
	WorldOptions.BeforeInitializeStandalone = [&CacheRoot](UGameInstance* GameInstance)
	{
		CastChecked<UWoTGameInstance>(GameInstance)->SetPayloadCacheDir(
			CacheRoot.Path, false);
	};
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	UWoTPayloadSubsystem* Payloads = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("payload-backed spec world exists"), World)
		|| !TestNotNull(TEXT("payload subsystem exists"), Payloads)
		|| !TestTrue(TEXT("real actor-style generation activates"),
			WoTPayloadActivationTest_ActivatePublished(
				Payloads, CacheRoot.Path, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	for (int32 CaseIndex = 0; CaseIndex < UE_ARRAY_COUNT(Cases); ++CaseIndex)
	{
		const FBindingCase& BindingCase = Cases[CaseIndex];
		const FWoTRuntimeVertexMeshDataPtr Mesh = RuntimeMeshes[CaseIndex];
		if (!TestTrue(FString(BindingCase.Map) + BindingCase.ActorLeaf
				+ TEXT(" real mesh payload loads"), Mesh.IsValid())) continue;
		const bool bAnyMaskedTriangle = Mesh->GetSlots().ContainsByPredicate(
			[](const FWoTRuntimeVertexMeshSlot& Slot)
			{
				return Slot.Style == EWoTRuntimeVertexMeshStyle::Masked;
			});
		if (BindingCase.Kind == EBindingKind::MaskedDecoration)
		{
			TestFalse(TEXT("GnarledTree triangles are not themselves masked"),
				bAnyMaskedTriangle);
		}
		if (BindingCase.Kind == EBindingKind::MaskedBoxyLamp)
		{
			TestTrue(TEXT("BoxyLamp carries an authored masked triangle slot"),
				bAnyMaskedTriangle);
		}
	}

	FWoTWorldProjectionSource ProjectionSource;
	ProjectionSource.MapIdentity = TEXT("ActorStyleRealRetail");
	ProjectionSource.Records = Actors;
	ProjectionSource.FindTexture = [Payloads](FStringView Identity,
		FWoTRetailTextureFacts& Facts)
	{
		return Payloads->FindRetailTextureFacts(Identity, Facts);
	};
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("production world projector accepts retail cases"),
		FWoTWorldProjector::ProjectDeferred(
			*World, ProjectionSource, Defaults, Projection, Error)))
	{
		AddError(Error);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	for (AActor* Deferred : Projection.DeferredActors)
	{
		if (IsValid(Deferred))
			Deferred->FinishSpawning(Deferred->GetActorTransform(), true);
	}

	UMaterialInterface* ModulateMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"));
	UMaterialInterface* MaskedMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshLitMasked.M_WoT_MeshLitMasked"));
	TestNotNull(TEXT("existing modulate master loads"), ModulateMaster);
	TestNotNull(TEXT("existing masked mesh master loads"), MaskedMaster);

	for (int32 CaseIndex = 0; CaseIndex < UE_ARRAY_COUNT(Cases); ++CaseIndex)
	{
		const FBindingCase& BindingCase = Cases[CaseIndex];
		const FString Rule = BindingCase.Kind == EBindingKind::MaskedPickup
			? TEXT("FixedPickup") : TEXT("Decoration");
		const FString Name = FString::Printf(
			TEXT("WoTProjected_%d_%s"), CaseIndex, *Rule);
		AActor* Projected = FindObject<AActor>(World->PersistentLevel, *Name);
		if (!TestNotNull(Name + TEXT(" projected actor"), Projected)) continue;

		UWoTVertexAnimComponent* MeshComponent = nullptr;
		if (AWoTDecoration* Decoration = Cast<AWoTDecoration>(Projected))
			MeshComponent = Decoration->Mesh;
		else if (AWoTInventory* Inventory = Cast<AWoTInventory>(Projected))
			MeshComponent = Inventory->Mesh;
		if (MeshComponent)
		{
			const FWoTRuntimeVertexMeshDataPtr RuntimeMesh =
				RuntimeMeshes[CaseIndex];
			if (TestTrue(Name + TEXT(" binds production runtime mesh payload"),
				RuntimeMesh.IsValid()
					&& MeshComponent->BindRuntimeVertexMeshForTesting(RuntimeMesh)))
			{
				MeshComponent->SetActorSkin(Visuals[CaseIndex].Skin);
				MeshComponent->SetRuntimeSkinOverrides(
					Visuals[CaseIndex].SkinOverrides);
			}
		}

		if (BindingCase.Kind == EBindingKind::ModulatedSprite)
		{
			AWoTDecoration* Decoration = Cast<AWoTDecoration>(Projected);
			const FWoTRuntimeWorldProperty* DrawType =
				Defaults.FindProperty(Actors[CaseIndex], TEXT("DrawType"));
			uint8 DrawTypeValue = 0;
			TestTrue(TEXT("Battle2_01 swarm DrawType decodes from retail bytes"),
				DrawType && WoTRuntimeWorldPropertyByte(*DrawType, DrawTypeValue));
			TestEqual(TEXT("Battle2_01 swarm is DT_Sprite"),
				static_cast<int32>(DrawTypeValue), 1);
			if (TestNotNull(TEXT("sprite remains a decoration actor"), Decoration)
				&& TestNotNull(TEXT("sprite uses existing billboard component"),
					Decoration->Sprite.Get()))
			{
				TestTrue(TEXT("sprite presentation flag is retained"),
					Decoration->IsRuntimeSprite());
				TestFalse(TEXT("DT_Sprite does not present its mesh"),
					Decoration->Mesh->IsVisible());
				TestTrue(TEXT("STY_Modulated billboard is modulate-only"),
					Decoration->Sprite->bModulateOnly);
				TestTrue(TEXT("real swarm texture resolves from payload"),
					Decoration->Sprite->HasResolvedFrames());
				TestEqual(TEXT("sprite uses decoded DrawScale"),
					Decoration->Sprite->DrawScale, Visuals[CaseIndex].DrawScale);
				TestEqual(TEXT("sprite uses decoded ScaleGlow"),
					Decoration->Sprite->ScaleGlow, Visuals[CaseIndex].ScaleGlow);
				TestEqual(TEXT("sprite modulate-only material root"),
					WoTActorStyleRootMaterial(
						Decoration->Sprite->GetModulateMaterialForTesting()),
					static_cast<const UMaterialInterface*>(ModulateMaster));
				FWoTRetailTextureFacts Facts;
				if (TestTrue(TEXT("swarm texture facts decode"),
					Payloads->FindRetailTextureFacts(
						TEXT("ParticleSystems.SmokeBlack64.Blk64_007"), Facts)))
				{
					TestEqual(TEXT("billboard width comes from decoded USize"),
						Decoration->Sprite->GetTextureWidthForTesting(),
						static_cast<float>(Facts.USize));
					TestEqual(TEXT("billboard height comes from decoded VSize"),
						Decoration->Sprite->GetTextureHeightForTesting(),
						static_cast<float>(Facts.VSize));
				}
			}
			continue;
		}

		if (!TestNotNull(Name + TEXT(" runtime mesh component"), MeshComponent))
			continue;
		const UMaterialInterface* ExpectedMaster =
			BindingCase.Kind == EBindingKind::MaskedDecoration
				|| BindingCase.Kind == EBindingKind::MaskedBoxyLamp
				|| BindingCase.Kind == EBindingKind::MaskedPickup
			? MaskedMaster : ModulateMaster;
		bool bSawExpectedMaskedBoxyLampSlot = false;
		for (int32 SlotIndex = 0;
			SlotIndex < MeshComponent->GetNumMaterials(); ++SlotIndex)
		{
			if (BindingCase.Kind == EBindingKind::MaskedBoxyLamp)
			{
				bSawExpectedMaskedBoxyLampSlot |= WoTActorStyleRootMaterial(
					MeshComponent->GetMaterial(SlotIndex)) == MaskedMaster;
				continue;
			}
			TestEqual(FString::Printf(TEXT("%s slot %d material root"),
				*Name, SlotIndex),
				WoTActorStyleRootMaterial(MeshComponent->GetMaterial(SlotIndex)),
				static_cast<const UMaterialInterface*>(ExpectedMaster));
		}
		if (BindingCase.Kind == EBindingKind::MaskedBoxyLamp)
		{
			TestTrue(TEXT("BoxyLamp authored masked slot stays masked"),
				bSawExpectedMaskedBoxyLampSlot);
		}
		if (BindingCase.Kind == EBindingKind::MaskedDecoration)
		{
			UMaterialInterface* Material = MeshComponent->GetMaterial(0);
			UTexture* Texture = nullptr;
			if (TestTrue(TEXT("masked GnarledTree material binds Tex"), Material
				&& Material->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), Texture)))
			{
				UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
				if (TestTrue(TEXT("masked GnarledTree Tex is materialized"),
					Texture2D && Texture2D->GetPlatformData()
						&& !Texture2D->GetPlatformData()->Mips.IsEmpty()))
				{
					FTexture2DMipMap& Mip = Texture2D->GetPlatformData()->Mips[0];
					const FColor* Pixels = reinterpret_cast<const FColor*>(
						Mip.BulkData.LockReadOnly());
					if (TestNotNull(TEXT("masked GnarledTree pixels are readable"), Pixels))
					{
						bool bSawTransparentKey = false;
						const int64 PixelCount = static_cast<int64>(Mip.SizeX)
							* static_cast<int64>(Mip.SizeY);
						for (int64 PixelIndex = 0;
							PixelIndex < PixelCount; ++PixelIndex)
						{
							bSawTransparentKey |= Pixels[PixelIndex].A == 0;
						}
						TestTrue(TEXT("actor Masked materializes palette-zero holes as transparent"),
							bSawTransparentKey);
					}
					Mip.BulkData.Unlock();
				}
			}
		}
		if (BindingCase.Kind == EBindingKind::ProceduralModulatedMesh)
		{
			FWoTRetailTextureFacts Facts;
			TestTrue(TEXT("RiverWave01 procedural facts decode"),
				Payloads->FindRetailTextureFacts(
					TEXT("JHPFireEng.RiverWave01"), Facts) && Facts.bProcedural);
			UTexture* Texture = nullptr;
			UMaterialInterface* Material = MeshComponent->GetMaterial(0);
			TestTrue(TEXT("RiverWave01 modulate material binds procedural placeholder"),
				Material && Material->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), Texture)
				&& Texture && Texture->GetName().StartsWith(TEXT("WoTProc_")));
		}
	}

	const FWoTRuntimeVertexMeshDataPtr* ElementalMesh = RuntimeMeshes.FindByPredicate(
		[](const FWoTRuntimeVertexMeshDataPtr& Mesh)
		{
			return Mesh.IsValid()
				&& Mesh->GetMeshName() == FName(TEXT("AngrealElementalShield"));
		});
	auto BoundTexture = [this](UWoTVertexAnimComponent& Component,
		const FString& Label) -> UTexture2D*
	{
		UTexture* Texture = nullptr;
		UMaterialInterface* Material = Component.GetMaterial(0);
		if (!TestTrue(Label + TEXT(" material binds Tex"), Material
			&& Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("Tex")), Texture)))
		{
			return nullptr;
		}
		return Cast<UTexture2D>(Texture);
	};
	auto BoundTextureAtMaterial = [this](UWoTVertexAnimComponent& Component,
		int32 MaterialIndex, const FString& Label) -> UTexture2D*
	{
		UTexture* Texture = nullptr;
		UMaterialInterface* Material = Component.GetMaterial(MaterialIndex);
		if (!TestTrue(Label + TEXT(" material binds Tex"), Material
			&& Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("Tex")), Texture)))
		{
			return nullptr;
		}
		return Cast<UTexture2D>(Texture);
	};
	auto HasAlphaZero = [this](UTexture2D* Texture, const FString& Label)
	{
		if (!TestTrue(Label + TEXT(" texture has a readable mip"), Texture
			&& Texture->GetPlatformData()
			&& !Texture->GetPlatformData()->Mips.IsEmpty()))
		{
			return false;
		}
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		const FColor* Pixels = reinterpret_cast<const FColor*>(
			Mip.BulkData.LockReadOnly());
		bool bSawAlphaZero = false;
		if (TestNotNull(Label + TEXT(" texture pixels are readable"), Pixels))
		{
			const int64 PixelCount = static_cast<int64>(Mip.SizeX) * Mip.SizeY;
			for (int64 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				bSawAlphaZero |= Pixels[PixelIndex].A == 0;
			}
		}
		Mip.BulkData.Unlock();
		TestTrue(Label + TEXT(" palette-zero texels are transparent"),
			bSawAlphaZero);
		return bSawAlphaZero;
	};

	if (MinionMesh.IsValid())
	{
		const int32 EyeMaterialIndex = MinionMesh->GetSlots().IndexOfByPredicate(
			[](const FWoTRuntimeVertexMeshSlot& Slot)
			{
				return Slot.TextureIndex == 4;
			});
		if (TestTrue(TEXT("real Minion payload has authored eye slot 4"),
			EyeMaterialIndex != INDEX_NONE))
		{
			TestEqual(TEXT("real Minion eye slot embeds its off texture"),
				MinionMesh->GetSlots()[EyeMaterialIndex].TextureName,
				FString(TEXT("JMinion4")));
			UWoTVertexAnimComponent* Minion =
				NewObject<UWoTVertexAnimComponent>(World);
			TestTrue(TEXT("real Minion runtime mesh binds"),
				Minion->BindRuntimeVertexMeshForTesting(MinionMesh));
			const FSoftObjectPath EyesOn(
				TEXT("/Game/WoT/Textures/WoTPawns/MinionEyes.MinionEyes"));
			TestTrue(TEXT("Minion eyes-on authors MultiSkins[4]"),
				Minion->SetSkinOverride(4, EyesOn));
			if (UTexture2D* Texture = BoundTextureAtMaterial(
				*Minion, EyeMaterialIndex, TEXT("Minion eyes on")))
			{
				TestEqual(TEXT("Minion eyes-on binds MinionEyes"),
					Texture->GetName(), FString(TEXT("MinionEyes")));
			}
			TestTrue(TEXT("Minion eyes-off clears MultiSkins[4]"),
				Minion->SetSkinOverride(4, FSoftObjectPath()));
			TestFalse(TEXT("Minion eyes-off erases the authored override"),
				Minion->GetRuntimeSkinOverrides().Contains(4));
			if (UTexture2D* Texture = BoundTextureAtMaterial(
				*Minion, EyeMaterialIndex, TEXT("Minion eyes off")))
			{
				TestEqual(TEXT("Minion eyes-off restores embedded JMinion4"),
					Texture->GetName(), FString(TEXT("JMinion4")));
			}
		}
	}

	if (WoodFragmentEmbeddedMesh.IsValid() && WoodFragmentSkinMesh.IsValid())
	{
		TestEqual(TEXT("wfrag1 renders authored texture index 1"),
			WoodFragmentEmbeddedMesh->GetSlots()[0].TextureIndex, 1);
		TestEqual(TEXT("wfrag1 embeds WoodTex1 at rendered index 1"),
			WoodFragmentEmbeddedMesh->GetSlots()[0].TextureName,
			FString(TEXT("WoodTex1")));
		TestEqual(TEXT("wfrag10 renders authored texture index 1"),
			WoodFragmentSkinMesh->GetSlots()[0].TextureIndex, 1);
		TestTrue(TEXT("wfrag10 rendered index 1 has no embedded texture"),
			WoodFragmentSkinMesh->GetSlots()[0].TextureName.IsEmpty());

		AWoTFragment* Fragment = World->SpawnActor<AWoTFragment>();
		const FSoftObjectPath ActorSkin(
			TEXT("/Game/WoT/Textures/WorldT/TimbFace_900.TimbFace_900"));
		Fragment->MeshName = TEXT("wfrag1");
		Fragment->SkinName = TEXT("TimbFace_900");
		Fragment->OnRep_Display();
		TestEqual(TEXT("fragment display retains mover texture as actor Skin"),
			Fragment->Mesh->GetRuntimeActorSkin(), ActorSkin);
		TestTrue(TEXT("fragment actor Skin does not synthesize MultiSkins"),
			Fragment->Mesh->GetRuntimeSkinOverrides().IsEmpty());
		TestTrue(TEXT("real wfrag1 runtime payload binds"),
			Fragment->Mesh->BindRuntimeVertexMeshForTesting(
				WoodFragmentEmbeddedMesh));
		Fragment->Mesh->SetActorSkin(ActorSkin);
		if (UTexture2D* Texture = BoundTexture(*Fragment->Mesh,
			TEXT("wfrag1 with actor Skin")))
		{
			TestEqual(TEXT("wfrag1 embedded index-1 texture beats actor Skin"),
				Texture->GetName(), FString(TEXT("WoodTex1")));
		}

		TestTrue(TEXT("real wfrag10 runtime payload binds"),
			Fragment->Mesh->BindRuntimeVertexMeshForTesting(WoodFragmentSkinMesh));
		Fragment->Mesh->SetActorSkin(ActorSkin);
		if (UTexture2D* Texture = BoundTexture(*Fragment->Mesh,
			TEXT("wfrag10 with actor Skin")))
		{
			TestEqual(TEXT("wfrag10 texture-less index 1 takes actor Skin"),
				Texture->GetName(), FString(TEXT("TimbFace_900")));
		}
	}

	struct FShieldBindingCase
	{
		const TCHAR* ClassIdentity;
		const TCHAR* TextureName;
	};
	const FShieldBindingCase ShieldCases[] = {
		{TEXT("Angreal.AngrealInvFireShield"), TEXT("ElShieldRED")},
		{TEXT("Angreal.AngrealInvWaterShield"), TEXT("ElShieldBLUE")},
		{TEXT("Angreal.AngrealInvAirShield"), TEXT("ElShieldWHITE")},
		{TEXT("Angreal.AngrealInvEarthShield"), TEXT("ElShieldGREEN")},
		{TEXT("Angreal.AngrealInvSpiritShield"), TEXT("ElShieldGOLD")},
	};
	TArray<FWoTRuntimeActorVisualPresentation> ShieldVisuals;
	for (const FShieldBindingCase& ShieldCase : ShieldCases)
	{
		FWoTRuntimeActorVisualPresentation& ShieldVisual =
			ShieldVisuals.AddDefaulted_GetRef();
		const FString Label(ShieldCase.ClassIdentity);
		if (!TestTrue(Label + TEXT(" real class presentation decodes"),
			WoTRuntimeReadClassVisualPresentation(
				Defaults, ShieldCase.ClassIdentity, ShieldVisual, Error))
			|| !TestTrue(Label + TEXT(" shares the real elemental-shield mesh"),
				ElementalMesh && ElementalMesh->IsValid()))
		{
			continue;
		}
		UWoTVertexAnimComponent* Component =
			NewObject<UWoTVertexAnimComponent>(World);
		Component->Style = ShieldVisual.Style;
		Component->bMeshCurvy = ShieldVisual.bMeshCurvy;
		FString ApplyError;
		if (!TestTrue(Label + TEXT(" real runtime mesh binds"),
			Component->BindRuntimeVertexMeshForTesting(*ElementalMesh))
			|| !TestTrue(Label + TEXT(" production actor visual applies"),
				WoTRuntimeApplyBoundActorVisualPresentationForTesting(
					*Component, ShieldVisual, ApplyError)))
		{
			AddError(ApplyError);
			continue;
		}
		UTexture2D* Texture = BoundTexture(*Component, Label);
		if (TestNotNull(Label + TEXT(" binds its actor Skin"), Texture))
		{
			TestEqual(Label + TEXT(" binds the correct shield color"),
				Texture->GetName(), FString(ShieldCase.TextureName));
			HasAlphaZero(Texture, Label);
		}
	}

	if (ElementalMesh && ElementalMesh->IsValid() && ShieldVisuals.Num() >= 2)
	{
		FWoTRuntimeActorVisualPresentation MultiSkinWins = ShieldVisuals[0];
		MultiSkinWins.SkinOverrides.Add(1,
			FSoftObjectPath(TEXT("/Game/WoT/Textures/Angreal/ElShieldBLUE.ElShieldBLUE")));
		UWoTVertexAnimComponent* Component =
			NewObject<UWoTVertexAnimComponent>(World);
		Component->Style = MultiSkinWins.Style;
		Component->bMeshCurvy = MultiSkinWins.bMeshCurvy;
		FString ApplyError;
		TestTrue(TEXT("MultiSkins precedence runtime mesh binds"),
			Component->BindRuntimeVertexMeshForTesting(*ElementalMesh));
		TestTrue(TEXT("MultiSkins precedence presentation applies"),
			WoTRuntimeApplyBoundActorVisualPresentationForTesting(
				*Component, MultiSkinWins, ApplyError));
		if (UTexture2D* Texture = BoundTexture(*Component,
			TEXT("MultiSkins precedence")))
		{
			TestEqual(TEXT("MultiSkins[1] beats actor Skin"),
				Texture->GetName(), FString(TEXT("ElShieldBLUE")));
		}
	}

	if (GuardianMesh.IsValid() && !ShieldVisuals.IsEmpty())
	{
		FWoTRuntimeActorVisualPresentation GuardianWithSkin = ShieldVisuals[0];
		GuardianWithSkin.MeshName = TEXT("AngrealGuardianPickup");
		GuardianWithSkin.MeshPackage = TEXT("Angreal");
		UWoTVertexAnimComponent* Component =
			NewObject<UWoTVertexAnimComponent>(World);
		Component->Style = GuardianWithSkin.Style;
		Component->bMeshCurvy = GuardianWithSkin.bMeshCurvy;
		FString ApplyError;
		TestTrue(TEXT("Guardian real runtime mesh binds"),
			Component->BindRuntimeVertexMeshForTesting(GuardianMesh));
		TestTrue(TEXT("Guardian with actor Skin presentation applies"),
			WoTRuntimeApplyBoundActorVisualPresentationForTesting(
				*Component, GuardianWithSkin, ApplyError));
		if (UTexture2D* Texture = BoundTexture(*Component, TEXT("Guardian")))
		{
			TestEqual(TEXT("Guardian nonzero embedded texture beats actor Skin"),
				Texture->GetName(), FString(TEXT("JAngrealGuardian1")));
		}
		TestEqual(TEXT("Guardian remains masked"),
			WoTActorStyleRootMaterial(Component->GetMaterial(0)),
			static_cast<const UMaterialInterface*>(MaskedMaster));
	}

	if (SealMesh.IsValid())
	{
		AWoTSeal* Seal = World->SpawnActor<AWoTSeal>();
		if (TestNotNull(TEXT("real Seal actor spawns"), Seal)
			&& TestTrue(TEXT("real Seal runtime mesh binds"),
				Seal->Mesh->BindRuntimeVertexMeshForTesting(SealMesh)))
		{
			Seal->ControllingFactionName = TEXT("AesSedai");
			Seal->ApplyControllingFactionForTesting();
			const FSoftObjectPath ExpectedSealSkin(
				TEXT("/Game/WoT/Textures/WOT/S_AesSedaiSeal.S_AesSedaiSeal"));
			TestEqual(TEXT("Seal faction uses retail MultiSkins[1]"),
				Seal->Mesh->GetRuntimeSkinOverrides().FindRef(1),
				ExpectedSealSkin);
			if (UTexture2D* Texture = BoundTexture(*Seal->Mesh,
				TEXT("Seal faction")))
			{
				TestEqual(TEXT("Seal faction binds its retail texture"),
					Texture->GetName(), FString(TEXT("S_AesSedaiSeal")));
			}
		}
	}

	const bool bSuccess = !HasAnyErrors();
	WoTTest::DestroySpecWorld(World);
	return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTActorStylePrecedenceSpec,
	"WoT.World.ActorStyle.Precedence",
	WoTActorStyleSpecFlags)

bool FWoTActorStylePrecedenceSpec::RunTest(const FString&)
{
	struct FPrecedenceCase
	{
		EWoTRuntimeVertexMeshStyle SlotStyle;
		uint8 ActorStyle;
		EWoTRuntimeVertexMeshStyle Expected;
		const TCHAR* Label;
	};
	const FPrecedenceCase Cases[] = {
		{EWoTRuntimeVertexMeshStyle::Masked, 4,
			EWoTRuntimeVertexMeshStyle::Modulated,
			TEXT("actor Modulated outranks triangle Masked")},
		{EWoTRuntimeVertexMeshStyle::Modulated, 2,
			EWoTRuntimeVertexMeshStyle::Modulated,
			TEXT("triangle Modulated outranks actor Masked")},
		{EWoTRuntimeVertexMeshStyle::Masked, 3,
			EWoTRuntimeVertexMeshStyle::Translucent,
			TEXT("actor Translucent outranks triangle Masked")},
		{EWoTRuntimeVertexMeshStyle::Translucent, 2,
			EWoTRuntimeVertexMeshStyle::Translucent,
			TEXT("triangle Translucent outranks actor Masked")},
		{EWoTRuntimeVertexMeshStyle::Modulated, 3,
			EWoTRuntimeVertexMeshStyle::Translucent,
			TEXT("actor Translucent outranks triangle Modulated")},
		{EWoTRuntimeVertexMeshStyle::Translucent, 4,
			EWoTRuntimeVertexMeshStyle::Translucent,
			TEXT("triangle Translucent outranks actor Modulated")},
	};
	for (const FPrecedenceCase& PrecedenceCase : Cases)
	{
		TestEqual(PrecedenceCase.Label,
			WoTRuntimeEffectiveMeshStyle(
				PrecedenceCase.SlotStyle, PrecedenceCase.ActorStyle),
			PrecedenceCase.Expected);
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
