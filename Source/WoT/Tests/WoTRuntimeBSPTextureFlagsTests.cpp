#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTPayloadActivationTestSupport.h"
#include "WoTTestWorld.h"

#include "../Assets/WoTRuntimeAssets.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeMapSky.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTWorldProjector.h"
#include "../WoTGameInstance.h"
#include "../WoTMover.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSkyBox.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/TextureSound.h"

namespace
{
constexpr EAutomationTestFlags WoTRuntimeBSPTextureFlagsSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

class FWoTRuntimeBSPTextureFlagsWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit,
				TEXT("texture-only masked retail writer overflow")));
		}
		const bool bArena05 = LogicalId == TEXT("cache/world/Arena_05")
			|| LogicalId.StartsWith(
				TEXT("cache/bsp/Arena_05/"), ESearchCase::CaseSensitive);
		const bool bMountain2 =
			LogicalId == TEXT("cache/texture/ScottT__mountain2");
		if (bArena05 || bMountain2)
		{
			TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
			Copy.Append(Bytes.GetData(), Bytes.Num());
		}
		return FWoTBootstrapStatus::Success();
	}

	TMap<FString, TArray<uint8>> Objects;
};

class FWoTRuntimeBSPTextureFlagsProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

struct FWoTRuntimeBSPTextureFlagsCacheRoot
{
	FWoTRuntimeBSPTextureFlagsCacheRoot()
	{
		Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(),
			TEXT("wot-bmasked-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
			TEXT("ContentCache")));
		IFileManager::Get().MakeDirectory(*Path, true);
	}
	~FWoTRuntimeBSPTextureFlagsCacheRoot()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true);
	}

	FString Path;
};

struct FWoTRuntimeBSPTextureFlagsActivationScope
{
	FWoTRuntimeBSPTextureFlagsActivationScope()
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
	}
	~FWoTRuntimeBSPTextureFlagsActivationScope()
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
	}
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPTextureFlagPrecedenceSpec,
	"WoT.RuntimeBSP.TextureFlags.Precedence",
	WoTRuntimeBSPTextureFlagsSpecFlags)

bool FWoTRuntimeBSPTextureFlagPrecedenceSpec::RunTest(const FString&)
{
	FWoTRuntimeBSPSectionStyle Style;
	WoTRuntimeBSPApplyTextureFlags(Style, true);
	TestEqual(TEXT("masked texture upgrades opaque"),
		Style.Blend, EWoTRuntimeBSPBlend::Masked);
	Style.Blend = EWoTRuntimeBSPBlend::Translucent;
	WoTRuntimeBSPApplyTextureFlags(Style, true);
	TestEqual(TEXT("translucent surface outranks masked texture"),
		Style.Blend, EWoTRuntimeBSPBlend::Translucent);
	Style.Blend = EWoTRuntimeBSPBlend::Modulated;
	WoTRuntimeBSPApplyTextureFlags(Style, true);
	TestEqual(TEXT("modulated surface outranks masked texture"),
		Style.Blend, EWoTRuntimeBSPBlend::Modulated);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPRealRetailArena05MaskedSpec,
	"WoT.World.TextureOnlyMasked.RealRetailArena05",
	WoTRuntimeBSPTextureFlagsSpecFlags)

bool FWoTRuntimeBSPRealRetailArena05MaskedSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString RetailRoot = WoTReferencePath(TEXT("original"));
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
	{
		AddError(LoadedContract.GetError().ToString());
		return false;
	}

	FWoTRetailContract WorldContract;
	FWoTRetailContract TextureContract;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		const bool bMultiplayerWorldFact = (Fact.Domain == EWoTFactDomain::BSP
			|| Fact.Domain == EWoTFactDomain::World)
			&& !Fact.SourcePath.Equals(
				TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
		if (bMultiplayerWorldFact)
		{
			WorldContract.Facts.Add(Fact);
			if (Fact.Domain == EWoTFactDomain::World)
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
		}
		if (Fact.Domain == EWoTFactDomain::Texture
			&& Fact.SourceExport.Equals(
				TEXT("ScottT.mountain2"), ESearchCase::IgnoreCase))
		{
			TextureContract.Facts.Add(Fact);
		}
	}
	if (!TestEqual(TEXT("nineteen multiplayer map packages"),
		WorldSources.Num(), 19)
		|| !TestEqual(TEXT("one mountain2 texture fact"),
			TextureContract.Facts.Num(), 1))
	{
		return false;
	}

	FWoTRuntimeBSPTextureFlagsWriter Writer;
	FWoTRuntimeBSPTextureFlagsProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorlds =
		ImportRequiredWorldSetPackageBytesForTesting(
			WorldSources, WorldContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds Arena_05 world and BSP objects"),
		ImportedWorlds.IsError()))
	{
		AddError(ImportedWorlds.GetError().ToString());
		return false;
	}
	TArray<uint8> ScottTBytes;
	if (!TestTrue(TEXT("retail ScottT.utx loads"), FFileHelper::LoadFileToArray(
		ScottTBytes, *(RetailRoot / TEXT("Textures/ScottT.utx")))))
	{
		return false;
	}
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedTextures =
		ImportTexturePackageBytesForTesting(ScottTBytes,
			TEXT("Textures/ScottT.utx"), TextureContract, Writer, Progress);
	if (!TestFalse(TEXT("native reader imports ScottT.mountain2"),
		ImportedTextures.IsError()))
	{
		AddError(ImportedTextures.GetError().ToString());
		return false;
	}

	const FWoTWorldDescriptor* ImportedDescriptor =
		ImportedWorlds.GetValue().Worlds.FindByPredicate(
			[](const FWoTWorldDescriptor& Candidate)
			{
				return Candidate.MapIdentity == TEXT("Arena_05");
			});
	if (!TestNotNull(TEXT("Arena_05 descriptor is imported"), ImportedDescriptor))
	{
		return false;
	}
	FWoTWorldDescriptor Descriptor = *ImportedDescriptor;
	TArray<uint8>* WorldObject = Writer.Objects.Find(Descriptor.WorldLogicalId);
	if (!TestNotNull(TEXT("Arena_05 world object is retained"), WorldObject))
	{
		return false;
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldObject),
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!TestFalse(TEXT("Arena_05 world object decodes"), DecodedWorld.IsError()))
	{
		AddError(DecodedWorld.GetError().ToString());
		return false;
	}
	FWoTRuntimeWorldResource WorldResource;
	const FWoTBootstrapStatus LoadedWorld =
		WorldResource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue());
	if (!TestFalse(TEXT("Arena_05 world payload loads"), LoadedWorld.IsError()))
	{
		AddError(LoadedWorld.GetError().ToString());
		return false;
	}

	TArray<FWoTBSPPayloadData> Parts;
	int32 MountainPartIndex = INDEX_NONE;
	int32 MountainSectionIndex = INDEX_NONE;
	for (int32 PartIndex = 0; PartIndex < Descriptor.BSPParts.Num(); ++PartIndex)
	{
		const FWoTBSPPartDescriptor& PartDescriptor = Descriptor.BSPParts[PartIndex];
		TArray<uint8>* PartObject = Writer.Objects.Find(PartDescriptor.LogicalId);
		if (!TestNotNull(PartDescriptor.LogicalId + TEXT(" object"), PartObject))
		{
			return false;
		}
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedPart =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*PartObject),
				TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
		if (!TestFalse(PartDescriptor.LogicalId + TEXT(" frame decodes"),
			DecodedPart.IsError()))
		{
			AddError(DecodedPart.GetError().ToString());
			return false;
		}
		FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
		FString PartError;
		if (!TestTrue(PartDescriptor.LogicalId + TEXT(" payload decodes"),
			UWoTBSPPayloadLibrary::DecodePayload(
				DecodedPart.GetValue().Payload(), Part, PartError)))
		{
			AddError(PartError);
			return false;
		}
		for (int32 SectionIndex = 0; SectionIndex < Part.Sections.Num(); ++SectionIndex)
		{
			const FWoTBSPPayloadSection& Section = Part.Sections[SectionIndex];
			if (Section.CanonicalPackage.Equals(
					TEXT("ScottT"), ESearchCase::IgnoreCase)
				&& Section.CanonicalName.Equals(
					TEXT("mountain2"), ESearchCase::IgnoreCase))
			{
				MountainPartIndex = PartIndex;
				MountainSectionIndex = SectionIndex;
			}
		}
	}
	if (!TestTrue(TEXT("Arena_05 contains ScottT.mountain2 section"),
		MountainPartIndex != INDEX_NONE && MountainSectionIndex != INDEX_NONE))
	{
		return false;
	}
	const FWoTBSPPayloadSection& MountainSection =
		Parts[MountainPartIndex].Sections[MountainSectionIndex];
	TestEqual(TEXT("Arena_05 mountain2 retail surface flags decode"),
		MountainSection.SurfaceFlags, 0x00002008u);
	TestFalse(TEXT("Arena_05 mountain2 surface lacks PF_Masked"),
		(MountainSection.SurfaceFlags & 0x00000002u) != 0);
	FWoTRuntimeBSPSectionStyle IdentityStyle;
	if (!TestTrue(TEXT("Arena_05 mountain2 identity style decodes"),
		WoTRuntimeBSPSectionStyleForIdentity(
			MountainSection.CanonicalPackage, MountainSection.CanonicalName,
			MountainSection.SurfaceFlags, IdentityStyle)))
	{
		return false;
	}
	TestEqual(TEXT("surface-only identity style is opaque"),
		IdentityStyle.Blend, EWoTRuntimeBSPBlend::Opaque);
	int32 MountainMaterialOrdinal = 0;
	for (int32 PartIndex = 0; PartIndex <= MountainPartIndex; ++PartIndex)
	{
		for (int32 SectionIndex = 0;
			SectionIndex < Parts[PartIndex].Sections.Num(); ++SectionIndex)
		{
			if (PartIndex == MountainPartIndex
				&& SectionIndex == MountainSectionIndex)
			{
				break;
			}
			// WoTWorldVisibleBSPPayload now retains every source section:
			// FakeBackdrop gets its opaque sky-window MID, while null or
			// malformed identities still get the ordinary fallback material.
			++MountainMaterialOrdinal;
		}
	}

	TArray<uint8>* TextureObject =
		Writer.Objects.Find(TEXT("cache/texture/ScottT__mountain2"));
	if (!TestNotNull(TEXT("ScottT.mountain2 texture object is retained"),
		TextureObject))
	{
		return false;
	}
	FWoTRuntimeBSPTextureFlagsCacheRoot CacheRoot;
	FString Error;
	if (!TestTrue(TEXT("real mountain2 texture object publishes"),
		WoTPayloadActivationTest_PublishObject(CacheRoot.Path,
			TEXT("cache/texture/ScottT__mountain2"), TEXT("wot-texture-v3"),
			*TextureObject, Error)))
	{
		AddError(Error);
		return false;
	}

	FWoTRuntimeBSPTextureFlagsActivationScope ActivationScope;
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
	if (!TestNotNull(TEXT("payload-backed Arena_05 world exists"), World))
	{
		return false;
	}
	UWoTPayloadSubsystem* Payloads =
		World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>();
	if (!TestNotNull(TEXT("payload subsystem exists"), Payloads)
		|| !TestTrue(TEXT("real mountain2 generation activates"),
			WoTPayloadActivationTest_ActivatePublished(
				Payloads, CacheRoot.Path, Error)))
	{
		if (!Error.IsEmpty()) AddError(Error);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const TSharedPtr<const FWoTTexturePayloadData> MountainPayload =
		Payloads->LoadTexturePayload(TEXT("cache/texture/ScottT__mountain2"));
	if (!TestTrue(TEXT("runtime payload path decodes real mountain2 as masked"),
		MountainPayload.IsValid() && MountainPayload->bMasked))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	FWoTRuntimeBSPSectionStyle EffectiveStyle = IdentityStyle;
	WoTRuntimeBSPApplyTextureFlags(EffectiveStyle, MountainPayload->bMasked);
	TestEqual(TEXT("effective retail mountain2 style is masked"),
		EffectiveStyle.Blend, EWoTRuntimeBSPBlend::Masked);

	FWoTRetailSkyWorldView SkyView;
	SkyView.Actors = WorldResource.Actors();
	SkyView.Zones = WorldResource.Zones();
	SkyView.Nodes = WorldResource.Nodes();
	SkyView.Surfaces = WorldResource.Surfaces();
	AWoTRuntimeBSPWorld* Owner = nullptr;
	uint32 FallbackParts = MAX_uint32;
	FString SkyError;
	if (!TestTrue(TEXT("production Arena_05 BSP materializer succeeds"),
		WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
			FallbackParts, Error, {}, nullptr, nullptr, &SkyView, &SkyError)))
	{
		AddError(Error);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("Arena_05 sky materialization has no error"), SkyError.IsEmpty());
	UMaterialInstanceDynamic* MountainMaterial = Owner != nullptr
		&& Owner->RuntimeMaterials.IsValidIndex(MountainMaterialOrdinal)
		? Owner->RuntimeMaterials[MountainMaterialOrdinal] : nullptr;
	if (TestNotNull(TEXT("production path binds mountain2 material"),
		MountainMaterial))
	{
		TestTrue(TEXT("production path binds masked world master"),
			MountainMaterial->Parent != nullptr
			&& MountainMaterial->Parent->GetName() == TEXT("M_WoT_WorldMasked"));
		bool bSkyUsesWorldMaterial = false;
		for (AWoTSkyBox* SkyBox : Owner->SkyBoxes)
		{
			UStaticMeshComponent* SkyComponent = SkyBox != nullptr
				? SkyBox->GetStaticMeshComponent() : nullptr;
			for (int32 MaterialIndex = 0;
				SkyComponent != nullptr
					&& MaterialIndex < SkyComponent->GetNumMaterials(); ++MaterialIndex)
			{
				bSkyUsesWorldMaterial |=
					SkyComponent->GetMaterial(MaterialIndex) == MountainMaterial;
			}
		}
		TestTrue(TEXT("Arena_05 sky inherits the exact mountain2 world-slot material"),
			bSkyUsesWorldMaterial);
	}
	AWoTMover* Mover = World->SpawnActor<AWoTMover>();
	if (TestNotNull(TEXT("synthetic mover using the retail texture spawns"), Mover))
	{
		FWoTRuntimeMoverModel MoverModel;
		MoverModel.Positions = {
			FVector3f(0.0f, 0.0f, 0.0f),
			FVector3f(100.0f, 0.0f, 0.0f),
			FVector3f(0.0f, 100.0f, 0.0f)};
		MoverModel.Normals.Init(FVector3f::ZAxisVector, 3);
		MoverModel.UV0 = {
			FVector2f(0.0f, 0.0f),
			FVector2f(1.0f, 0.0f),
			FVector2f(0.0f, 1.0f)};
		MoverModel.Indices = {0, 1, 2};
		FWoTRuntimeMoverSection& MoverSection =
			MoverModel.Sections.AddDefaulted_GetRef();
		MoverSection.MaterialSlot = MountainSection.MaterialSlot;
		MoverSection.IndexCount = 3;
		MoverSection.MaxVertex = 2;
		MoverSection.CanonicalPackage = MountainSection.CanonicalPackage;
		MoverSection.CanonicalName = MountainSection.CanonicalName;
		MoverSection.SourceTextureIdentity =
			MountainSection.SourceTextureIdentity;
		MoverSection.SurfaceFlags = MountainSection.SurfaceFlags;
		MoverSection.PlaneNormal = FVector3f::ZAxisVector;
		if (TestTrue(TEXT("production mover path resolves its texture before its master"),
			WoTBuildMoverMeshForTesting(*Mover, MoverModel, Error))
			&& TestEqual(TEXT("texture-only masked mover owns one material"),
				Mover->RuntimeMaterials.Num(), 1))
		{
			UMaterialInstanceDynamic* MoverMaterial = Mover->RuntimeMaterials[0];
			TestTrue(TEXT("texture-only masked mover binds masked dynamic master"),
				MoverMaterial != nullptr && MoverMaterial->Parent != nullptr
				&& MoverMaterial->Parent->GetName()
					== TEXT("M_WoT_MoverLitMasked"));
			UTexture* MoverTexture = nullptr;
			TestTrue(TEXT("texture-only masked mover exposes its resolved Tex"),
				MoverMaterial != nullptr
				&& MoverMaterial->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), MoverTexture));
			TestTrue(TEXT("mover Tex retains real retail masked metadata"),
				UWoTRuntimeAssets::ResolveTextureMasked(
					Cast<UTexture2D>(MoverTexture)));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
