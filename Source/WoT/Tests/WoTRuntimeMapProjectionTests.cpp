#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "ImageUtils.h"
#include "Misc/ScopeExit.h"
#include "Algo/AnyOf.h"
#include "Algo/IsSorted.h"

#include "../Assets/WoTRetailIdentity.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeMapSky.h"
#include "../World/WoTRuntimeMirror.h"
#include "../FrontEnd/WoTFrontEndSky.h"
#include "../FrontEnd/WoTFrontEndGroundTrace.h"
#include "../WoTFrontEndCatalog.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeActorVisuals.h"
#include "../World/WoTRuntimeParticleProjection.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../World/WoTWorldMaterializer.h"
#include "Engine/DataTable.h"
#include "../Inventory/WoTHandInfo.h"
#include "../Inventory/WoTHandSet.h"
#include "../World/WoTWorldProjector.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeLocalizationDefaults.h"
#include "WoTPayloadActivationTestSupport.h"
#include "WoTWorldMaterializerTestData.h"
#include "WoTTestWorld.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealSpawnPool.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTLegionInvSeeker.h"
#include "../Inventory/WoTAndilayRoot.h"
#include "../Inventory/WoTInventory.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTDecayProjectile.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTAmbientSound.h"
#include "../WoTActorRotator.h"
#include "../WoTBreakableDecoration.h"
#include "../WoTBudgetInfo.h"
#include "../WoTDecoration.h"
#include "../WoTDestroyDirective.h"
#include "../WoTExplodingWall.h"
#include "../WoTCorona.h"
#include "../WoTFogLight.h"
#include "../WoTLightWave.h"
#include "../WoTFactionCharacters.h"
#include "../WoTHUD.h"
#include "../WoTMapLightTable.h"
#include "../WoTLeafMap.h"
#include "../WoTMeshLight.h"
#include "../WoTMover.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTSpecialTriggers.h"
#include "../WoTCollisionBlocker.h"
#include "../WoTPlayerController.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerState.h"
#include "../WoTRainCyd.h"
#include "../WoTPlayerStart.h"
#include "../WoTSealAltar.h"
#include "../WoTSkyBox.h"
#include "../WoTTeleporter.h"
#include "../WoTZoneVolume.h"
#include "../WoTUE1Collision.h"
#include "../WoTWorldFX.h"
#include "../WoTPayloadSubsystem.h"
#include "../Traps/WoTPit.h"
#include "../Traps/WoTPortcullis.h"
#include "../Traps/WoTStaircase.h"

#include "Components/PostProcessComponent.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/BoxComponent.h"
#include "Components/BrushComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Engine/Level.h"
#include "Engine/PointLight.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif
#include "../Projectiles/WoTFireballProjectile.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/Generated/WorldTextures.generated.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/RegistryLocalization.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/TextureSound.h"
#include "../RetailData/WoTRetailBuildContext.h"

#if WITH_AUTOMATION_TESTS

bool WoTRuntimeWorldResponseRectFitsForTesting(uint32 X, uint32 Y,
	uint32 W, uint32 H, uint32 AtlasWidth, uint32 AtlasHeight);

namespace
{
constexpr EAutomationTestFlags WoTRuntimeMapProjectionFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

class FWoTRuntimeProjectionCensusWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("projection census writer overflow")));
		}
		if (Schema == TEXT("wot-world-v3")
			|| Schema == TEXT("wot-class-default-v3")
			|| Schema == TEXT("wot-bsp-v3"))
		{
			TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
			Copy.Append(Bytes.GetData(), Bytes.Num());
		}
		return FWoTBootstrapStatus::Success();
	}
	TMap<FString, TArray<uint8>> Objects;
};

class FWoTRuntimeProjectionCensusProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

FString WoTRuntimeProjectionRepoPath(FStringView Relative)
{
	return WoTRepositoryOrReferencePath(Relative);
}

FString WoTRuntimeProjectionEvidenceRoot()
{
	const FString Configured =
		FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_RUNTIME_MAP_EVIDENCE_ROOT"));
	return Configured.IsEmpty()
		? WoTRuntimeProjectionRepoPath(
			TEXT("Saved/ReleaseEvidence/runtime-map-3a1/native-raw"))
		: FPaths::ConvertRelativePathToFull(Configured);
}

bool WoTRuntimeProjectionSavePayload(TConstArrayView<uint8> Payload,
	const FString& Path)
{
	TArray<uint8> Owned;
	Owned.Append(Payload.GetData(), Payload.Num());
	return FFileHelper::SaveArrayToFile(Owned, *Path);
}

FWoTBSPPayloadData WoTRuntimeMapProjectionTriangle(bool bPortal = false)
{
	FWoTBSPPayloadData Payload;
	Payload.FormatVersion = 1;
	Payload.bPortal = bPortal;
	Payload.BoundsMin = FVector3f(0, 0, 0);
	Payload.BoundsMax = FVector3f(100, 100, 0);
	Payload.Positions = {
		FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0)};
	Payload.Normals = {
		FVector3f::ZAxisVector, FVector3f::ZAxisVector,
		FVector3f::ZAxisVector};
	Payload.UV0 = {
		FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)};
	Payload.UV1 = Payload.UV0;
	Payload.Indices = {0, 1, 2};
	FWoTBSPPayloadSection& Section = Payload.Sections.AddDefaulted_GetRef();
	Section.MaterialSlot = TEXT("ArenaTex__Floor_M_2S_U_SL");
	Section.FirstIndex = 0;
	Section.IndexCount = 3;
	Section.MinVertex = 0;
	Section.MaxVertex = 2;
	Section.bCollisionEnabled = !bPortal;
	return Payload;
}

FWoTBSPPayloadData WoTRuntimeMapProjectionLargeCollisionPlane(float Z)
{
	constexpr int32 TriangleCount = 5500;
	constexpr int32 TrianglesPerRow = 100;
	constexpr float CellSize = 200.0f;
	FWoTBSPPayloadData Payload;
	Payload.FormatVersion = 1;
	Payload.BoundsMin = FVector3f(0, 0, Z);
	Payload.BoundsMax = FVector3f(
		TrianglesPerRow * CellSize,
		FMath::DivideAndRoundUp(TriangleCount, TrianglesPerRow) * CellSize, Z);
	Payload.Positions.Reserve(TriangleCount * 3);
	Payload.Normals.Reserve(TriangleCount * 3);
	Payload.UV0.Reserve(TriangleCount * 3);
	Payload.UV1.Reserve(TriangleCount * 3);
	Payload.Indices.Reserve(TriangleCount * 3);
	for (int32 TriangleIndex = 0; TriangleIndex < TriangleCount; ++TriangleIndex)
	{
		const float X = (TriangleIndex % TrianglesPerRow) * CellSize;
		const float Y = (TriangleIndex / TrianglesPerRow) * CellSize;
		const uint32 FirstVertex = Payload.Positions.Num();
		Payload.Positions.Append({
			FVector3f(X, Y, Z), FVector3f(X + 100.0f, Y, Z),
			FVector3f(X, Y + 100.0f, Z)});
		Payload.Normals.Append({
			FVector3f::ZAxisVector, FVector3f::ZAxisVector,
			FVector3f::ZAxisVector});
		Payload.UV0.Append({
			FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
		Payload.UV1.Append({
			FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
		Payload.Indices.Append({FirstVertex, FirstVertex + 2, FirstVertex + 1});
	}
	FWoTBSPPayloadSection& Section = Payload.Sections.AddDefaulted_GetRef();
	Section.MaterialSlot = TEXT("ArenaTex__Floor_M_2S_U_SL");
	Section.FirstIndex = 0;
	Section.IndexCount = Payload.Indices.Num();
	Section.MinVertex = 0;
	Section.MaxVertex = Payload.Positions.Num() - 1;
	Section.bCollisionEnabled = true;
	return Payload;
}

FWoTBSPPayloadData WoTRuntimeMapProjectionVisibleAndSkyTriangles()
{
	FWoTBSPPayloadData Payload = WoTRuntimeMapProjectionTriangle();
	Payload.BoundsMax = FVector3f(300, 100, 0);
	Payload.Indices = {0, 2, 1};
	Payload.Positions.Append({
		FVector3f(200, 0, 0), FVector3f(300, 0, 0),
		FVector3f(200, 100, 0)});
	Payload.Normals.Append({
		FVector3f::ZAxisVector, FVector3f::ZAxisVector,
		FVector3f::ZAxisVector});
	Payload.UV0.Append({
		FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
	Payload.UV1.Append({
		FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
	Payload.Indices.Append({3, 5, 4});
	Payload.Sections[0].MaterialSlot = TEXT("DEVELOPERT__DEVELOPER_T");
	FWoTBSPPayloadSection& Sky = Payload.Sections.AddDefaulted_GetRef();
	Sky.MaterialSlot = TEXT("ArenaTex__Backdrop_Sky");
	Sky.FirstIndex = 3;
	Sky.IndexCount = 3;
	Sky.MinVertex = 3;
	Sky.MaxVertex = 5;
	Sky.bCollisionEnabled = true;
	return Payload;
}

UWorld* WoTRuntimeMapProjectionWorld(const TCHAR* Leaf)
{
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/WoTRuntimeMapProjection_%s_%s"), Leaf,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetFlags(RF_Transient);
	return UWorld::CreateWorld(EWorldType::Game, false,
		MakeUniqueObjectName(Package, UWorld::StaticClass(), TEXT("World")),
		Package, true, ERHIFeatureLevel::Num, nullptr, true);
}

void WoTRuntimeMapProjectionDestroyWorld(UWorld* World)
{
	if (World == nullptr) return;
	World->RemoveFromRoot();
	if (World->bIsWorldInitialized)
	{
		World->DestroyWorld(false);
	}
	else
	{
		World->ClearFlags(RF_Standalone);
		World->MarkAsGarbage();
	}
}

template <typename T>
FWoTRuntimeWorldProperty WoTRuntimeMapProjectionProperty(
	const TCHAR* Name, EWoTRuntimeWorldPropertyType Type,
	const TCHAR* StructName, const T& Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = Type;
	Property.StructName = StructName;
	Property.Value.SetNumUninitialized(sizeof(T));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(T));
	return Property;
}

void WoTRuntimeMapProjectionAppendU16(TArray<uint8>& Bytes, uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value));
	Bytes.Add(static_cast<uint8>(Value >> 8));
}

void WoTRuntimeMapProjectionAppendU32(TArray<uint8>& Bytes, uint32 Value)
{
	for (int32 Shift = 0; Shift < 32; Shift += 8)
		Bytes.Add(static_cast<uint8>(Value >> Shift));
}

void WoTRuntimeMapProjectionAppendI32(TArray<uint8>& Bytes, int32 Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	WoTRuntimeMapProjectionAppendU32(Bytes, Bits);
}

void WoTRuntimeMapProjectionAppendU64(TArray<uint8>& Bytes, uint64 Value)
{
	WoTRuntimeMapProjectionAppendU32(Bytes, static_cast<uint32>(Value));
	WoTRuntimeMapProjectionAppendU32(Bytes, static_cast<uint32>(Value >> 32));
}

void WoTRuntimeMapProjectionAppendFloat(TArray<uint8>& Bytes, float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	WoTRuntimeMapProjectionAppendU32(Bytes, Bits);
}

void WoTRuntimeMapProjectionAppendString(
	TArray<uint8>& Bytes, const FString& Value)
{
	const FTCHARToUTF8 Encoded(*Value);
	WoTRuntimeMapProjectionAppendU32(
		Bytes, static_cast<uint32>(Encoded.Length()));
	Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
}

enum class EWoTRuntimeWorldV3Mutation : uint8
{
	None,
	NodeLeaf,
	MissingLightSentinel,
	LightSemanticNotActor,
	RootOutside,
	Friction,
	MoverActor,
	MoverBrush,
	MoverModel,
	SectionRange,
	SectionPadding,
	PositionStream,
	NormalStream,
	UV1Stream,
	IndexStream,
	CollisionPositionStream,
	CollisionIndexStream,
	Plane,
	ResponseRectXOverflow,
	ResponseRectYOverflow,
};

TArray<uint8> WoTRuntimeMapProjectionWorldObject(
	uint16 Version, EWoTRuntimeWorldV3Mutation Mutation,
	uint32 HeaderMoverCount = 1)
{
	auto U16 = [](TArray<uint8>& Bytes, uint16 Value)
	{
		WoTRuntimeMapProjectionAppendU16(Bytes, Value);
	};
	auto U32 = [](TArray<uint8>& Bytes, uint32 Value)
	{
		WoTRuntimeMapProjectionAppendU32(Bytes, Value);
	};
	auto I32 = [](TArray<uint8>& Bytes, int32 Value)
	{
		WoTRuntimeMapProjectionAppendI32(Bytes, Value);
	};
	auto U64 = [](TArray<uint8>& Bytes, uint64 Value)
	{
		WoTRuntimeMapProjectionAppendU64(Bytes, Value);
	};
	auto F32 = [](TArray<uint8>& Bytes, float Value)
	{
		WoTRuntimeMapProjectionAppendFloat(Bytes, Value);
	};
	auto String = [](TArray<uint8>& Bytes, const FString& Value)
	{
		WoTRuntimeMapProjectionAppendString(Bytes, Value);
	};
	const FString Map = Version >= 4 ? TEXT("SyntheticV4") : TEXT("SyntheticV3");
	const FString Mover = Map + TEXT(".Mover0");
	const FString Light = Map + TEXT(".Light0");
	const FString Model = Map + TEXT(".Model0");
	const FString Texture = Map + TEXT(".Texture0");
	TArray<uint8> Payload = {'W','O','T','W','R','L','3',0};
	U16(Payload, Version); U16(Payload, 0);
	const uint32 Counts[13] = {
		2, 0, 3, 3, 1, 1, 3, 1, 4, 0, 1, 2, 1};
	const uint32 CountFields = Version == 2 ? 10u : 13u;
	for (uint32 Index = 0; Index < CountFields; ++Index)
		U32(Payload, Counts[Index]);
	if (Version >= 3)
		U32(Payload, Mutation == EWoTRuntimeWorldV3Mutation::RootOutside ? 2u : 1u);
	for (const FVector3f& Vector : {
		FVector3f(0, 0, 1), FVector3f(1, 0, 0), FVector3f(0, 1, 0)})
	{
		F32(Payload, Vector.X); F32(Payload, Vector.Y); F32(Payload, Vector.Z);
	}
	for (const FVector3f& Point : {
		FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(0, 100, 0)})
	{
		F32(Payload, Point.X); F32(Payload, Point.Y); F32(Payload, Point.Z);
	}
	for (float Plane : {0.0f, 0.0f, 1.0f, 0.0f}) F32(Payload, Plane);
	U64(Payload, 1); Payload.Add(0);
	for (int32 Value : {0, 0, static_cast<int32>(INDEX_NONE),
		static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE),
		static_cast<int32>(INDEX_NONE), static_cast<int32>(INDEX_NONE)})
		I32(Payload, Value);
	Payload.Add(0); Payload.Add(0); Payload.Add(3);
	I32(Payload, Mutation == EWoTRuntimeWorldV3Mutation::NodeLeaf ? 1 : 0);
	I32(Payload, INDEX_NONE);
	for (int32 Value : {0, 0, 1, 2, static_cast<int32>(INDEX_NONE),
		static_cast<int32>(INDEX_NONE)}) I32(Payload, Value);
	U32(Payload, 0); U16(Payload, 0); U16(Payload, 0);
	if (Version >= 3)
	{
		if (Mutation == EWoTRuntimeWorldV3Mutation::Friction)
			U32(Payload, 0x7fc00000u);
		else F32(Payload, 10.0f);
	}
	String(Payload, Texture); String(Payload, Mover);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		I32(Payload, Index); I32(Payload, INDEX_NONE);
	}
	String(Payload, FString()); U64(Payload, 1); U64(Payload, 1);
	if (Version >= 3)
	{
		I32(Payload, 0); I32(Payload, 0); I32(Payload, INDEX_NONE);
		U32(Payload, Mutation == EWoTRuntimeWorldV3Mutation::LightSemanticNotActor
			? 2u : 1u);
		U32(Payload, Mutation == EWoTRuntimeWorldV3Mutation::MissingLightSentinel
			? 1u : MAX_uint32);
	}
	for (const auto& Semantic : {
		TTuple<FString, FString, uint8>(Mover, TEXT("Engine.Mover"), 1),
		TTuple<FString, FString, uint8>(Light, TEXT("Engine.Light"), 1),
		TTuple<FString, FString, uint8>(Model, TEXT("Engine.Model"), 0),
		TTuple<FString, FString, uint8>(Texture, TEXT("Engine.Texture"), 0)})
	{
		String(Payload, Semantic.Get<0>()); String(Payload, Semantic.Get<1>());
		Payload.Add(Semantic.Get<2>());
	}
	String(Payload, Mover); String(Payload, TEXT("Engine.Mover"));
	U32(Payload, 1); String(Payload, TEXT("Brush")); Payload.Add(5);
	U32(Payload, 0); String(Payload, FString()); String(Payload, FString());
	const bool bBrushMismatch =
		Mutation == EWoTRuntimeWorldV3Mutation::MoverBrush;
	String(Payload, bBrushMismatch ? Texture : Model);
	String(Payload, bBrushMismatch ? TEXT("Engine.Texture") : TEXT("Engine.Model"));
	U32(Payload, 0); if (Version >= 3) U32(Payload, 0);
	String(Payload, FString());
	String(Payload, Light); String(Payload, TEXT("Engine.Light"));
	U32(Payload, 0); String(Payload, FString());
	if (Version >= 3)
	{
		String(Payload, Mutation == EWoTRuntimeWorldV3Mutation::MoverActor
			? Light : Mover);
		String(Payload, Mutation == EWoTRuntimeWorldV3Mutation::MoverModel
			? Texture : Model);
		U32(Payload, 1); U32(Payload, 3); U32(Payload, 3);
		if (Version >= 4)
		{
			U32(Payload, 3); U32(Payload, 3); Payload.Add(1);
		}
		U32(Payload, Mutation == EWoTRuntimeWorldV3Mutation::SectionRange ? 1u : 0u);
		U32(Payload, 3); U32(Payload, 0); U32(Payload, 2); Payload.Add(1);
		Payload.Add(Mutation == EWoTRuntimeWorldV3Mutation::SectionPadding ? 1 : 0);
		Payload.Add(0); Payload.Add(0); U32(Payload, 0);
		for (float Value : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
			Mutation == EWoTRuntimeWorldV3Mutation::Plane ? 2.0f : 1.0f})
			F32(Payload, Value);
		String(Payload, TEXT("WorldT__Stone__MP0000"));
		String(Payload, TEXT("WorldT")); String(Payload, TEXT("Stone"));
		String(Payload, TEXT("WorldT.Detail.Stone"));
		if (Version >= 5)
		{
			String(Payload, FString());
			String(Payload, FString());
			String(Payload, FString());
			F32(Payload, 0.0f);
		}
		const FVector3f Positions[3] = {
			Mutation == EWoTRuntimeWorldV3Mutation::PositionStream
				? FVector3f(10000001.0f, 0, 0)
				: FVector3f(0, 0, 0),
			FVector3f(100, 0, 0), FVector3f(0, 100, 0)};
		for (const FVector3f& Position : Positions)
		{
			F32(Payload, Position.X); F32(Payload, Position.Y); F32(Payload, Position.Z);
		}
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const FVector3f Normal = Index == 0
				&& Mutation == EWoTRuntimeWorldV3Mutation::NormalStream
				? FVector3f(0, 0, 2) : FVector3f(0, 0, 1);
			F32(Payload, Normal.X); F32(Payload, Normal.Y); F32(Payload, Normal.Z);
		}
		for (const FVector2f& UV : {
			FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)})
		{
			F32(Payload, UV.X); F32(Payload, UV.Y);
		}
		if (Version >= 4)
		{
			for (const FVector2f& UV : {
				FVector2f(0.125f, 0.125f), FVector2f(0.875f, 0.125f),
				FVector2f(0.125f, 0.875f)})
			{
				if (Mutation == EWoTRuntimeWorldV3Mutation::UV1Stream)
					U32(Payload, 0x7fc00000u);
				else F32(Payload, UV.X);
				F32(Payload, UV.Y);
			}
		}
		U32(Payload, 0); U32(Payload, 1);
		U32(Payload, Mutation == EWoTRuntimeWorldV3Mutation::IndexStream ? 3u : 2u);
		if (Version >= 4)
		{
			const FVector3f CollisionPositions[3] = {
				Mutation == EWoTRuntimeWorldV3Mutation::CollisionPositionStream
					? FVector3f(10000001.0f, 0, 0) : FVector3f(10, 20, 30),
				FVector3f(40, 20, 30), FVector3f(10, 50, 30)};
			for (const FVector3f& Position : CollisionPositions)
			{
				F32(Payload, Position.X); F32(Payload, Position.Y);
				F32(Payload, Position.Z);
			}
			U32(Payload, 0); U32(Payload, 1);
			U32(Payload,
				Mutation == EWoTRuntimeWorldV3Mutation::CollisionIndexStream ? 3u : 2u);
		}
	}
	if (Version >= 4)
	{
		U32(Payload, 3); U32(Payload, 3); U32(Payload, 36);
		for (int32 Pixel = 0; Pixel < 9; ++Pixel)
		{
			Payload.Add(127); Payload.Add(127); Payload.Add(127); Payload.Add(255);
		}
	}
	if (Version >= 6)
	{
		// One Blink group and one 1x1 response rectangle. The malformed
		// variants preserve a tiny byte count while overflowing the padded
		// destination-coordinate additions.
		U32(Payload, 1);
		Payload.Add(3); Payload.Add(0); Payload.Add(0); Payload.Add(0);
		U32(Payload, 1);
		U32(Payload, 1);
		Payload.Add(0); Payload.Add(0); Payload.Add(0); Payload.Add(0);
		U32(Payload, 1); U32(Payload, 1);
		U32(Payload,
			Mutation == EWoTRuntimeWorldV3Mutation::ResponseRectXOverflow
				? MAX_uint32 : 1u);
		U32(Payload,
			Mutation == EWoTRuntimeWorldV3Mutation::ResponseRectYOverflow
				? MAX_uint32 : 1u);
		U32(Payload, 1); U32(Payload, 1); U32(Payload, 3);
		Payload.Add(12); Payload.Add(34); Payload.Add(56);
	}
	const FString Encoding = Version == 2
		? TEXT("ue1-world-records-le-v2") : Version == 3
			? TEXT("ue1-world-records-le-v3") : Version == 4
				? TEXT("ue1-world-records-le-v4") : Version == 5
					? TEXT("ue1-world-records-le-v5")
					: TEXT("ue1-world-records-le-v6");
	const FString Header = FString::Printf(
		TEXT("{\"actor_count\":2,\"attachment_count\":0,\"bsp_node_count\":1,\"bsp_surface_count\":1,\"encoding\":\"%s\",\"fact_id\":\"spec.world-v%u\",\"importer\":\"world-v1\",\"importer_version\":%u,\"kind\":\"world\",\"light_count\":1,\"map_identity\":\"%s\",\"mover_count\":%u,\"navigation_seed_count\":0,\"payload_byte_length\":%d,\"portal_surface_count\":0,\"reach_spec_count\":0,\"semantic_object_count\":4,\"sound_count\":0,\"source\":{\"export\":\"%s.Level\",\"package\":\"Maps/%s.wot\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"zone_count\":1}"),
		*Encoding, static_cast<uint32>(Version), static_cast<uint32>(Version),
		*Map, HeaderMoverCount, Payload.Num(), *Map, *Map);
	return WoTPayloadActivationTest_FrameObject(Header, Payload);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverBrushFrameSpec,
	"WoT.World.Projector.MoverBrushFrame",
	WoTRuntimeMapProjectionFlags)

bool FWoTMoverBrushFrameSpec::RunTest(const FString& Parameters)
{
	// Battle2_02 Mover6 (WC_THRONEDOOR): Model1445 spans x+-37.000118,
	// y+-6, z+-157 brush-local; PrePivot (-36.949337,-6,154.330719). Retail
	// ToWorld puts its bottom face exactly on the world floor at z=-26158
	// (Location z -25846.667969 - 154.330719 - 157).
	auto MakeSlab = []()
	{
		FWoTRuntimeMoverModel Model;
		Model.Positions = {
			FVector3f(-37.000118f, -6.0f, -157.0f),
			FVector3f(37.000118f, -6.0f, -157.0f),
			FVector3f(37.000118f, 6.0f, 157.0f)};
		Model.Normals.Init(FVector3f(0.0f, 0.0f, 1.0f), 3);
		Model.UV0 = {FVector2f(0.25f, 0.5f), FVector2f(0.75f, 0.5f),
			FVector2f(0.75f, 1.0f)};
		Model.Indices = {0u, 1u, 2u};
		Model.CollisionPositions = {
			FVector3f(-30.0f, -4.0f, -140.0f),
			FVector3f(30.0f, -4.0f, -140.0f),
			FVector3f(30.0f, 4.0f, 140.0f)};
		Model.CollisionIndices = {0u, 1u, 2u};
		FWoTRuntimeMoverSection& Section = Model.Sections.AddDefaulted_GetRef();
		Section.PlaneOrigin = FVector3f(-37.000118f, -6.0f, -157.0f);
		Section.PlaneNormal = FVector3f(1.0f, 0.0f, 0.0f);
		Section.IndexCount = 3;
		Section.MaxVertex = 2;
		return Model;
	};

	FWoTMoverBrushFrame Pivot;
	Pivot.PrePivot = FVector(-36.949337, -6.0, 154.330719);
	FWoTRuntimeMoverModel Door = MakeSlab();
	FString Error;
	TestTrue(TEXT("PrePivot frame applies"), WoTApplyMoverBrushFrame(Pivot, Door, Error));
	TestTrue(TEXT("bottom corner lands on the retail floor"),
		FVector(Door.Positions[0]).Equals(
			FVector(-0.050781, 0.0, -311.330719), 1.0e-3));
	TestTrue(TEXT("plane origin moves with the geometry"),
		FVector(Door.Sections[0].PlaneOrigin).Equals(
			FVector(-0.050781, 0.0, -311.330719), 1.0e-3));
	TestTrue(TEXT("collision vertices receive the same PrePivot frame"),
		FVector(Door.CollisionPositions[0]).Equals(
			FVector(6.949337, 2.0, -294.330719), 1.0e-3));
	const double FloorZ = -25846.667969 + Door.Positions[0].Z;
	TestTrue(TEXT("world bottom equals the Battle2_02 floor"),
		FMath::IsNearlyEqual(FloorZ, -26157.998688, 1.0e-3));
	TestTrue(TEXT("pure translation keeps normals"),
		FVector(Door.Normals[0]).Equals(FVector(0.0, 0.0, 1.0), 1.0e-6));
	TestTrue(TEXT("texture UVs are frame-invariant"),
		Door.UV0[1].Equals(FVector2f(0.75f, 0.5f), 0.0f));

	// MainScale scales about PrePivot (Battle2_05 Mover2 ships 2.248819).
	FWoTMoverBrushFrame Scaled;
	Scaled.PrePivot = FVector(1.0, 2.0, 3.0);
	Scaled.MainScale = FVector(2.0, 3.0, 4.0);
	FWoTRuntimeMoverModel Stretched = MakeSlab();
	TestTrue(TEXT("scaled frame applies"),
		WoTApplyMoverBrushFrame(Scaled, Stretched, Error));
	TestTrue(TEXT("MainScale scales about PrePivot"),
		FVector(Stretched.Positions[2]).Equals(FVector(
			(37.000118 - 1.0) * 2.0, (6.0 - 2.0) * 3.0, (157.0 - 3.0) * 4.0), 1.0e-3));
	TestTrue(TEXT("plane normals use the inverse transpose"),
		FVector(Stretched.Sections[0].PlaneNormal).Equals(FVector(1.0, 0.0, 0.0), 1.0e-6));

	// A non-axis normal under non-uniform scale bends toward the short axis.
	FWoTRuntimeMoverModel Skewed = MakeSlab();
	Skewed.Normals[0] = FVector3f(FVector(1.0, 1.0, 0.0).GetSafeNormal());
	FWoTMoverBrushFrame Wide;
	Wide.MainScale = FVector(2.0, 1.0, 1.0);
	TestTrue(TEXT("non-uniform frame applies"), WoTApplyMoverBrushFrame(Wide, Skewed, Error));
	TestTrue(TEXT("normal is (n/scale) renormalized"),
		FVector(Skewed.Normals[0]).Equals(FVector(0.5, 1.0, 0.0).GetSafeNormal(), 1.0e-5));

	// PostScale is a world-axis scale: with a 90-degree yaw the world X
	// scale shortens the mesh's local Y.
	FWoTMoverBrushFrame Post;
	Post.PostScale = FVector(0.5, 1.0, 1.0);
	Post.InitialRotation = FRotator(0.0, 90.0, 0.0);
	FWoTRuntimeMoverModel Posted = MakeSlab();
	TestTrue(TEXT("post-scaled frame applies"), WoTApplyMoverBrushFrame(Post, Posted, Error));
	TestTrue(TEXT("PostScale conjugates through the initial rotation"),
		FVector(Posted.Positions[2]).Equals(FVector(37.000118, 3.0, 157.0), 1.0e-3));

	// FPoly::Transform flips vertex order for a negative orientation.
	FWoTMoverBrushFrame Mirror;
	Mirror.MainScale = FVector(-1.0, 1.0, 1.0);
	FWoTRuntimeMoverModel Mirrored = MakeSlab();
	TestTrue(TEXT("mirrored frame applies"), WoTApplyMoverBrushFrame(Mirror, Mirrored, Error));
	TestTrue(TEXT("mirror flips winding"),
		Mirrored.Indices == TArray<uint32>({0u, 2u, 1u}));
	TestTrue(TEXT("mirror flips collision winding"),
		Mirrored.CollisionIndices == TArray<uint32>({0u, 2u, 1u}));

	FWoTMoverBrushFrame Degenerate;
	Degenerate.MainScale = FVector(1.0, 0.0, 1.0);
	FWoTRuntimeMoverModel Rejected = MakeSlab();
	Error.Reset();
	TestFalse(TEXT("a zero scale is rejected"),
		WoTApplyMoverBrushFrame(Degenerate, Rejected, Error));
	TestFalse(TEXT("the rejection names its cause"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeStructMemberLookupSpec,
	"WoT.World.Properties.StructMemberLookup",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeStructMemberLookupSpec::RunTest(const FString&)
{
	auto Scalar = [](const TCHAR* Name, EWoTRuntimeWorldPropertyType Type,
		uint32 ArrayIndex, TArray<uint8> Value)
	{
		FWoTRuntimeWorldProperty Member;
		Member.Name = Name;
		Member.Type = Type;
		Member.ArrayIndex = ArrayIndex;
		Member.Value = MoveTemp(Value);
		return Member;
	};
	FWoTRuntimeWorldProperty Inner;
	Inner.Name = TEXT("Engine");
	Inner.Type = EWoTRuntimeWorldPropertyType::Struct;
	Inner.StructName = TEXT("EngineInfo");
	Inner.StructMembers.Add(Scalar(TEXT("Rate"),
		EWoTRuntimeWorldPropertyType::Float, 0, {0x00, 0x00, 0x80, 0x3f}));
	FWoTRuntimeWorldProperty Outer;
	Outer.Name = TEXT("Entry");
	Outer.Type = EWoTRuntimeWorldPropertyType::Struct;
	Outer.StructName = TEXT("EntryInfo");
	FWoTRuntimeWorldProperty Slot0;
	Slot0.Name = TEXT("Slot");
	Slot0.Type = EWoTRuntimeWorldPropertyType::Name;
	Slot0.NameValue = TEXT("Idle");
	FWoTRuntimeWorldProperty Slot1 = Slot0;
	Slot1.ArrayIndex = 1;
	Slot1.NameValue = TEXT("Walk");
	Outer.StructMembers = {Slot0, Slot1, Inner};

	const FWoTRuntimeWorldProperty* Found =
		WoTRuntimeWorldStructMember(Outer, TEXT("slot"), 1);
	TestTrue(TEXT("lookup is case-insensitive and per array index"),
		Found != nullptr && Found->NameValue == TEXT("Walk"));
	TestTrue(TEXT("index zero is the default"),
		WoTRuntimeWorldStructMember(Outer, TEXT("Slot")) != nullptr
		&& WoTRuntimeWorldStructMember(Outer, TEXT("Slot"))->NameValue == TEXT("Idle"));
	TestNull(TEXT("an absent index is null"),
		WoTRuntimeWorldStructMember(Outer, TEXT("Slot"), 2));
	TestNull(TEXT("an absent name is null"),
		WoTRuntimeWorldStructMember(Outer, TEXT("Odds")));
	const FWoTRuntimeWorldProperty* Nested =
		WoTRuntimeWorldStructMember(Outer, TEXT("Engine"));
	float Rate = 0.0f;
	TestTrue(TEXT("nested structs recurse and the typed readers apply"),
		Nested != nullptr
		&& WoTRuntimeWorldPropertyFloat(
			*WoTRuntimeWorldStructMember(*Nested, TEXT("Rate")), Rate)
		&& Rate == 1.0f);
	TestNull(TEXT("a non-struct property has no members"),
		WoTRuntimeWorldStructMember(Slot0, TEXT("Slot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeStructMemberDecodeSpec,
	"WoT.World.Properties.StructMembersDecodeAndRejectMalformed",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeStructMemberDecodeSpec::RunTest(const FString&)
{
	auto U32 = [](TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
			Bytes.Add(static_cast<uint8>(Value >> Shift));
	};
	auto String = [&U32](TArray<uint8>& Bytes, const TCHAR* Value)
	{
		FTCHARToUTF8 Encoded(Value);
		U32(Bytes, static_cast<uint32>(Encoded.Length()));
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	};
	auto Member = [&U32, &String](TArray<uint8>& Bytes, const TCHAR* Name,
		uint8 Type, const TCHAR* NameValue, const TCHAR* Object,
		const TCHAR* ObjectClass, TConstArrayView<uint8> Value)
	{
		String(Bytes, Name); Bytes.Add(Type); U32(Bytes, 0);
		String(Bytes, TEXT("")); String(Bytes, NameValue);
		String(Bytes, Object); String(Bytes, ObjectClass);
		U32(Bytes, static_cast<uint32>(Value.Num())); Bytes.Append(Value);
		U32(Bytes, 0);
	};
	TArray<uint8> Bytes;
	U32(Bytes, 1);
	String(Bytes, TEXT("Budget")); Bytes.Add(10); U32(Bytes, 0);
	String(Bytes, TEXT("Resource")); String(Bytes, TEXT(""));
	String(Bytes, TEXT("")); String(Bytes, TEXT("")); U32(Bytes, 0);
	const int32 ResourceMemberCountOffset = Bytes.Num();
	U32(Bytes, 3);
	Member(Bytes, TEXT("ResourcePackageName"), 6, TEXT("WOT"),
		TEXT(""), TEXT(""), {});
	Member(Bytes, TEXT("ResourceAdapterName"), 6, TEXT("SealInventory"),
		TEXT(""), TEXT(""), {});
	const uint8 One[] = {1, 0, 0, 0};
	Member(Bytes, TEXT("ItemCount"), 2, TEXT(""), TEXT(""), TEXT(""),
		MakeArrayView(One));
	int32 Offset = 0;
	TArray<FWoTRuntimeWorldProperty> Properties;
	FString Error;
	if (!TestTrue(TEXT("self-describing Resource members decode"),
		WoTRuntimeWorldDecodeProperties(Bytes, Offset, Properties, Error,
			nullptr, true, EWoTStructMemberPolicy::WorldResolvedSchema)))
	{
		AddError(Error); return false;
	}
	TestEqual(TEXT("three Resource members"),
		Properties[0].StructMembers.Num(), 3);
	TestEqual(TEXT("embedded name resolved"),
		Properties[0].StructMembers[1].NameValue,
		FString(TEXT("SealInventory")));
	int32 Count = 0;
	TestTrue(TEXT("embedded integer remains typed"),
		WoTRuntimeWorldPropertyInteger(
			Properties[0].StructMembers[2], Count));
	TestEqual(TEXT("embedded integer value"), Count, 1);

	TArray<uint8> OversizedResourceMembers = Bytes;
	const uint32 BombCount = 4000000u;
	FMemory::Memcpy(OversizedResourceMembers.GetData()
		+ ResourceMemberCountOffset, &BombCount, sizeof(BombCount));
	Offset = 0;
	TestFalse(TEXT("Resource member census rejects before recursive allocation"),
		WoTRuntimeWorldDecodeProperties(OversizedResourceMembers, Offset,
			Properties, Error, nullptr, true,
			EWoTStructMemberPolicy::WorldResolvedSchema));
	TestTrue(TEXT("Resource member-census rejection is attributed"),
		Error.Contains(TEXT("member count"), ESearchCase::IgnoreCase));

	TArray<uint8> SoundEventBytes;
	U32(SoundEventBytes, 1);
	String(SoundEventBytes, TEXT("Cue")); SoundEventBytes.Add(10);
	U32(SoundEventBytes, 0); String(SoundEventBytes, TEXT("SoundEvent"));
	String(SoundEventBytes, TEXT("")); String(SoundEventBytes, TEXT(""));
	String(SoundEventBytes, TEXT("")); U32(SoundEventBytes, 0);
	const int32 SoundMemberCountOffset = SoundEventBytes.Num();
	U32(SoundEventBytes, 6);
	const float Delay = 0.25f;
	const TConstArrayView<uint8> DelayBytes(
		reinterpret_cast<const uint8*>(&Delay), sizeof(Delay));
	Member(SoundEventBytes, TEXT("Delay"), 4, TEXT(""), TEXT(""),
		TEXT(""), DelayBytes);
	Member(SoundEventBytes, TEXT("Event"), 6, TEXT("Alarm"), TEXT(""),
		TEXT(""), {});
	Member(SoundEventBytes, TEXT("SoundSourceTag"), 6, TEXT("Source"),
		TEXT(""), TEXT(""), {});
	Member(SoundEventBytes, TEXT("Sound"), 5, TEXT(""),
		TEXT("Environment.Alarm"), TEXT("Engine.Sound"), {});
	Member(SoundEventBytes, TEXT("SoundTableTag"), 6, TEXT("Table"),
		TEXT(""), TEXT(""), {});
	Member(SoundEventBytes, TEXT("SoundTableSlot"), 6, TEXT("Slot"),
		TEXT(""), TEXT(""), {});
	Offset = 0;
	TestTrue(TEXT("declaration-ordered SoundEvent members decode"),
		WoTRuntimeWorldDecodeProperties(SoundEventBytes, Offset, Properties,
			Error, nullptr, true,
			EWoTStructMemberPolicy::WorldResolvedSchema));
	TestEqual(TEXT("six SoundEvent members"),
		Properties[0].StructMembers.Num(), 6);
	TArray<uint8> OversizedSoundMembers = SoundEventBytes;
	FMemory::Memcpy(OversizedSoundMembers.GetData() + SoundMemberCountOffset,
		&BombCount, sizeof(BombCount));
	Offset = 0;
	TestFalse(TEXT("SoundEvent member census rejects before recursive allocation"),
		WoTRuntimeWorldDecodeProperties(OversizedSoundMembers, Offset,
			Properties, Error, nullptr, true,
			EWoTStructMemberPolicy::WorldResolvedSchema));
	TestTrue(TEXT("SoundEvent member-census rejection is attributed"),
		Error.Contains(TEXT("member count"), ESearchCase::IgnoreCase));

	TArray<uint8> TruncatedPropertyTable;
	U32(TruncatedPropertyTable, 1);
	Offset = 0;
	TestFalse(TEXT("top-level property count rejects before allocation"),
		WoTRuntimeWorldDecodeProperties(TruncatedPropertyTable, Offset,
			Properties, Error, nullptr, true,
			EWoTStructMemberPolicy::WorldResolvedSchema));
	TestTrue(TEXT("top-level property byte-budget rejection is attributed"),
		Error.Contains(TEXT("count"), ESearchCase::IgnoreCase));

	TArray<uint8> WrongSchema = Bytes;
	static constexpr ANSICHAR ItemCountName[] = "ItemCount";
	for (int32 At = 0; At + 9 <= WrongSchema.Num(); ++At)
	{
		if (FMemory::Memcmp(WrongSchema.GetData() + At,
			ItemCountName, 9) == 0)
		{
			WrongSchema[At] = 'X';
			break;
		}
	}
	Offset = 0;
	TestFalse(TEXT("noncanonical Resource member schema fails closed"),
		WoTRuntimeWorldDecodeProperties(WrongSchema, Offset, Properties, Error,
			nullptr, true, EWoTStructMemberPolicy::WorldResolvedSchema));

	TArray<uint8> Malformed = Bytes;
	Malformed[Malformed.Num() - 4] = 1;
	Offset = 0;
	TestFalse(TEXT("truncated nested-member declaration fails closed"),
		WoTRuntimeWorldDecodeProperties(Malformed, Offset, Properties, Error,
			nullptr, true, EWoTStructMemberPolicy::WorldResolvedSchema));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeGenericStructMemberPolicySpec,
	"WoT.World.Properties.ReflectedGenericPolicyBounds",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeGenericStructMemberPolicySpec::RunTest(const FString&)
{
	auto U32 = [](TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
			Bytes.Add(static_cast<uint8>(Value >> Shift));
	};
	auto String = [&U32](TArray<uint8>& Bytes, const TCHAR* Value)
	{
		FTCHARToUTF8 Encoded(Value);
		U32(Bytes, static_cast<uint32>(Encoded.Length()));
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	};
	auto Header = [&U32, &String](TArray<uint8>& Bytes, const TCHAR* Name,
		uint8 Type, uint32 ArrayIndex, const TCHAR* StructName,
		TConstArrayView<uint8> Value, uint32 MemberCount)
	{
		String(Bytes, Name); Bytes.Add(Type); U32(Bytes, ArrayIndex);
		String(Bytes, StructName); String(Bytes, TEXT(""));
		String(Bytes, TEXT("")); String(Bytes, TEXT(""));
		U32(Bytes, static_cast<uint32>(Value.Num())); Bytes.Append(Value);
		U32(Bytes, MemberCount);
	};
	auto Decode = [](TConstArrayView<uint8> Bytes,
		EWoTStructMemberPolicy Policy, FString& Error)
	{
		int32 Offset = 0;
		TArray<FWoTRuntimeWorldProperty> Properties;
		return WoTRuntimeWorldDecodeProperties(
			Bytes, Offset, Properties, Error, nullptr, true, Policy);
	};

	TArray<uint8> Generic;
	U32(Generic, 1);
	Header(Generic, TEXT("Budget"), 10, 0, TEXT("Resource"), {}, 1);
	const uint8 Seven[] = {7, 0, 0, 0};
	Header(Generic, TEXT("ReflectedCount"), 2, 0, TEXT(""),
		MakeArrayView(Seven), 0);
	FString Error;
	TestTrue(TEXT("generic policy accepts reflection-shaped Resource members"),
		Decode(Generic, EWoTStructMemberPolicy::ReflectedGeneric, Error));
	Error.Reset();
	TestFalse(TEXT("world policy rejects the same noncanonical Resource schema"),
		Decode(Generic, EWoTStructMemberPolicy::WorldResolvedSchema, Error));

	auto DepthPayload = [&U32, &Header](uint32 MaximumDepth)
	{
		TArray<uint8> Bytes;
		U32(Bytes, 1);
		TFunction<void(uint32)> AppendLevel;
		AppendLevel = [&Bytes, &Header, &AppendLevel, MaximumDepth](uint32 Depth)
		{
			Header(Bytes, Depth == 0 ? TEXT("Root") : TEXT("Nested"),
				10, 0, TEXT("GenericDepth"), {}, Depth < MaximumDepth ? 1u : 0u);
			if (Depth < MaximumDepth) AppendLevel(Depth + 1);
		};
		AppendLevel(0);
		return Bytes;
	};
	Error.Reset();
	TestTrue(TEXT("runtime accepts a terminal struct at depth 8"),
		Decode(DepthPayload(8), EWoTStructMemberPolicy::ReflectedGeneric, Error));
	Error.Reset();
	TestFalse(TEXT("runtime rejects a terminal struct at depth 9"),
		Decode(DepthPayload(9), EWoTStructMemberPolicy::ReflectedGeneric, Error));

	auto BudgetPayload = [&U32, &Header](uint32 LeftCount, uint32 RightCount)
	{
		TArray<uint8> Bytes;
		U32(Bytes, 1);
		Header(Bytes, TEXT("Root"), 10, 0, TEXT("GenericBudget"), {}, 2);
		auto Branch = [&Bytes, &Header](const TCHAR* Name, uint32 Count)
		{
			Header(Bytes, Name, 10, 0, TEXT("GenericBranch"), {}, Count);
			const uint8 Value[] = {1};
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				Header(Bytes, TEXT("Entry"), 1, Index, TEXT(""),
					MakeArrayView(Value), 0);
			}
		};
		Branch(TEXT("Left"), LeftCount);
		Branch(TEXT("Right"), RightCount);
		return Bytes;
	};
	Error.Reset();
	TestTrue(TEXT("4096 descendants across sibling subtrees are accepted"),
		Decode(BudgetPayload(2047, 2047),
			EWoTStructMemberPolicy::ReflectedGeneric, Error));
	Error.Reset();
	TestFalse(TEXT("4097 descendants across sibling subtrees are rejected"),
		Decode(BudgetPayload(2047, 2048),
			EWoTStructMemberPolicy::ReflectedGeneric, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeWorldV4SyntheticValidationSpec,
	"WoT.World.Payload.WorldV4SyntheticValidation",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeWorldV4SyntheticValidationSpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTRuntimeMapProjectionWorldObject(
				4, EWoTRuntimeWorldV3Mutation::None),
			TEXT("ue1-world-records-le-v4"), TEXT("world"));
	if (!TestFalse(TEXT("synthetic world-v4 frame decodes"), Decoded.IsError()))
	{
		AddError(Decoded.GetError().ToString());
		return false;
	}
	FWoTRuntimeWorldResource Resource;
	FWoTBootstrapStatus Loaded =
		Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue());
	if (!TestFalse(TEXT("synthetic world-v4 payload validates"), Loaded.IsError()))
	{
		AddError(Loaded.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("synthetic leaf count"), Resource.Leaves().Num(), 1);
	TestEqual(TEXT("synthetic light-stream count"),
		Resource.ModelLights().Num(), 2);
	TestTrue(TEXT("synthetic BSP root-outside value"),
		Resource.BSPRootOutside());
	TestEqual(TEXT("synthetic surface friction"),
		Resource.Surfaces()[0].Friction, 10.0f);
	TestEqual(TEXT("synthetic mover count"),
		Resource.MoverModels().Num(), 1);
	TestEqual(TEXT("synthetic mover actor binding"),
		Resource.MoverModels()[0].ActorIdentity,
		FString(TEXT("SyntheticV4.Mover0")));
	TestTrue(TEXT("synthetic mover is lightmapped"),
		Resource.MoverModels()[0].bLightmapped);
	TestEqual(TEXT("synthetic mover UV1 count"),
		Resource.MoverModels()[0].UV1.Num(), 3);
	TestTrue(TEXT("synthetic mover first UV1"),
		Resource.MoverModels()[0].UV1[0].Equals(FVector2f(0.125f), 0.0f));
	TestEqual(TEXT("synthetic mover collision vertex count"),
		Resource.MoverModels()[0].CollisionPositions.Num(), 3);
	TestEqual(TEXT("synthetic mover collision index count"),
		Resource.MoverModels()[0].CollisionIndices.Num(), 3);
	TestTrue(TEXT("synthetic mover collision stream is independent of render"),
		Resource.MoverModels()[0].CollisionPositions[0].Equals(
			FVector3f(10, 20, 30), 0.0f));
	TestEqual(TEXT("synthetic mover-atlas width"), Resource.MoverAtlasWidth(), 3u);
	TestEqual(TEXT("synthetic mover-atlas height"), Resource.MoverAtlasHeight(), 3u);
	TestEqual(TEXT("synthetic mover-atlas bytes"), Resource.MoverAtlasBGRA().Num(), 36);

	struct FMalformedCase
	{
		EWoTRuntimeWorldV3Mutation Mutation;
		const TCHAR* Label;
	};
	const FMalformedCase Cases[] = {
		{EWoTRuntimeWorldV3Mutation::NodeLeaf, TEXT("out-of-range node leaf")},
		{EWoTRuntimeWorldV3Mutation::MissingLightSentinel,
			TEXT("missing light sentinel")},
		{EWoTRuntimeWorldV3Mutation::LightSemanticNotActor,
			TEXT("light semantic is not an actor")},
		{EWoTRuntimeWorldV3Mutation::RootOutside,
			TEXT("invalid root-outside scalar")},
		{EWoTRuntimeWorldV3Mutation::Friction,
			TEXT("invalid surface friction")},
		{EWoTRuntimeWorldV3Mutation::MoverActor,
			TEXT("mover actor relationship")},
		{EWoTRuntimeWorldV3Mutation::MoverBrush,
			TEXT("mover Brush relationship")},
		{EWoTRuntimeWorldV3Mutation::MoverModel,
			TEXT("mover model relationship")},
		{EWoTRuntimeWorldV3Mutation::SectionRange,
			TEXT("mover section range")},
		{EWoTRuntimeWorldV3Mutation::SectionPadding,
			TEXT("mover section padding")},
		{EWoTRuntimeWorldV3Mutation::PositionStream,
			TEXT("mover position stream")},
		{EWoTRuntimeWorldV3Mutation::NormalStream,
			TEXT("mover normal stream")},
		{EWoTRuntimeWorldV3Mutation::UV1Stream,
			TEXT("mover UV1 stream")},
		{EWoTRuntimeWorldV3Mutation::IndexStream,
			TEXT("mover index stream")},
		{EWoTRuntimeWorldV3Mutation::CollisionPositionStream,
			TEXT("mover collision position stream")},
		{EWoTRuntimeWorldV3Mutation::CollisionIndexStream,
			TEXT("mover collision index stream")},
		{EWoTRuntimeWorldV3Mutation::Plane,
			TEXT("mover section plane")},
	};
	for (const FMalformedCase& Case : Cases)
	{
		Decoded = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTRuntimeMapProjectionWorldObject(4, Case.Mutation),
			TEXT("ue1-world-records-le-v4"), TEXT("world"));
		if (!TestFalse(FString::Printf(TEXT("%s frame decodes"), Case.Label),
			Decoded.IsError()))
		{
			AddError(Decoded.GetError().ToString());
			continue;
		}
		FWoTRuntimeWorldResource MalformedResource;
		Loaded = MalformedResource.LoadObjectForTesting(
			MoveTemp(Decoded).GetValue());
		TestTrue(FString::Printf(TEXT("%s fails closed"), Case.Label),
			Loaded.IsError());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeWorldV6ResponseRectBoundsSpec,
	"WoT.World.Payload.WorldV6ResponseRectBounds",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeWorldV6ResponseRectBoundsSpec::RunTest(const FString&)
{
	TestFalse(TEXT("overflowing response X+W is rejected before indexing"),
		WoTRuntimeWorldResponseRectFitsForTesting(
			MAX_uint32, 1, 1, 1, 3, 3));
	TestFalse(TEXT("overflowing response Y+H is rejected before indexing"),
		WoTRuntimeWorldResponseRectFitsForTesting(
			1, MAX_uint32, 1, 1, 3, 3));
	TestTrue(TEXT("a one-texel response with its full gutter fits"),
		WoTRuntimeWorldResponseRectFitsForTesting(1, 1, 1, 1, 3, 3));

	auto DecodeV6 = [](EWoTRuntimeWorldV3Mutation Mutation)
	{
		return FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTRuntimeMapProjectionWorldObject(6, Mutation),
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	};
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Valid =
		DecodeV6(EWoTRuntimeWorldV3Mutation::None);
	if (!TestFalse(TEXT("synthetic world-v6 frame decodes"), Valid.IsError()))
	{
		AddError(Valid.GetError().ToString());
		return false;
	}
	FWoTRuntimeWorldResource ValidResource;
	FWoTBootstrapStatus Loaded =
		ValidResource.LoadObjectForTesting(MoveTemp(Valid).GetValue());
	if (!TestFalse(TEXT("synthetic world-v6 response validates"), Loaded.IsError()))
	{
		AddError(Loaded.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("synthetic world-v6 response term count"),
		ValidResource.MoverAnimationTerms().Num(), 1);

	for (const EWoTRuntimeWorldV3Mutation Mutation : {
		EWoTRuntimeWorldV3Mutation::ResponseRectXOverflow,
		EWoTRuntimeWorldV3Mutation::ResponseRectYOverflow})
	{
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Malformed =
			DecodeV6(Mutation);
		if (!TestFalse(TEXT("malformed world-v6 frame still decodes"),
			Malformed.IsError()))
		{
			AddError(Malformed.GetError().ToString());
			continue;
		}
		FWoTRuntimeWorldResource Rejected;
		Loaded = Rejected.LoadObjectForTesting(
			MoveTemp(Malformed).GetValue());
		TestTrue(TEXT("overflowing world-v6 response rect fails closed"),
			Loaded.IsError());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeWorldV2MoverCensusCompatibilitySpec,
	"WoT.World.Payload.WorldV2NonzeroMoverCensusCompatibility",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeWorldV2MoverCensusCompatibilitySpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTRuntimeMapProjectionWorldObject(
				2, EWoTRuntimeWorldV3Mutation::None, 1),
			TEXT("ue1-world-records-le-v3"), TEXT("world"),
			FStringView(),
			TEXT("ue1-world-records-le-v2"));
	if (!TestFalse(TEXT("world-v2 nonzero-mover frame decodes"),
		Decoded.IsError()))
	{
		AddError(Decoded.GetError().ToString());
		return false;
	}
	FWoTRuntimeWorldResource Resource;
	const FWoTBootstrapStatus Loaded =
		Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue());
	if (!TestFalse(TEXT("world-v2 nonzero header mover census validates"),
		Loaded.IsError()))
	{
		AddError(Loaded.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("world-v2 retains header mover census"),
		Resource.MoverCount(), 1u);
	TestTrue(TEXT("world-v2 has no embedded mover-model table"),
		Resource.MoverModels().IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeClassDefaultsFirstUseStructSpec,
	"WoT.World.Properties.ClassDefaultsFirstUseResourceAndSoundEvent",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeClassDefaultsFirstUseStructSpec::RunTest(const FString&)
{
	auto U16 = [](TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value));
		Bytes.Add(static_cast<uint8>(Value >> 8));
	};
	auto U32 = [](TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
			Bytes.Add(static_cast<uint8>(Value >> Shift));
	};
	auto String = [&U32](TArray<uint8>& Bytes, const TCHAR* Value)
	{
		FTCHARToUTF8 Encoded(Value);
		U32(Bytes, static_cast<uint32>(Encoded.Length()));
		Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	};
	auto Member = [&U32, &String](TArray<uint8>& Bytes, const TCHAR* Name,
		uint8 Type, const TCHAR* NameValue, const TCHAR* Object,
		const TCHAR* ObjectClass, TConstArrayView<uint8> Value)
	{
		String(Bytes, Name); Bytes.Add(Type); U32(Bytes, 0);
		String(Bytes, TEXT("")); String(Bytes, NameValue);
		String(Bytes, Object); String(Bytes, ObjectClass);
		U32(Bytes, static_cast<uint32>(Value.Num())); Bytes.Append(Value);
		U32(Bytes, 0);
	};
	auto Struct = [&U32, &String, &Member](TArray<uint8>& Bytes,
		const TCHAR* Name, const TCHAR* StructName, bool bSound)
	{
		String(Bytes, Name); Bytes.Add(10); U32(Bytes, 0);
		String(Bytes, StructName); String(Bytes, TEXT(""));
		String(Bytes, TEXT("")); String(Bytes, TEXT("")); U32(Bytes, 0);
		if (!bSound)
		{
			U32(Bytes, 3);
			Member(Bytes, TEXT("ResourcePackageName"), 6, TEXT("WOT"),
				TEXT(""), TEXT(""), {});
			Member(Bytes, TEXT("ResourceAdapterName"), 6,
				TEXT("SealInventory"), TEXT(""), TEXT(""), {});
			const uint8 One[] = {1, 0, 0, 0};
			Member(Bytes, TEXT("ItemCount"), 2, TEXT(""), TEXT(""),
				TEXT(""), MakeArrayView(One));
			return;
		}
		U32(Bytes, 6);
		const float Delay = 0.25f;
		const TConstArrayView<uint8> DelayBytes(
			reinterpret_cast<const uint8*>(&Delay), sizeof(Delay));
		Member(Bytes, TEXT("Delay"), 4, TEXT(""), TEXT(""), TEXT(""), DelayBytes);
		Member(Bytes, TEXT("Event"), 6, TEXT("Alarm"), TEXT(""), TEXT(""), {});
		Member(Bytes, TEXT("SoundSourceTag"), 6, TEXT("Source"),
			TEXT(""), TEXT(""), {});
		Member(Bytes, TEXT("Sound"), 5, TEXT(""), TEXT("AmbientA.Portal5"),
			TEXT("Engine.Sound"), {});
		Member(Bytes, TEXT("SoundTableTag"), 6, TEXT("Table"),
			TEXT(""), TEXT(""), {});
		Member(Bytes, TEXT("SoundTableSlot"), 6, TEXT("Slot"),
			TEXT(""), TEXT(""), {});
	};

	TArray<uint8> Payload = {'W','O','T','D','E','F','3',0};
	U16(Payload, 3); U16(Payload, 0); U32(Payload, 1);
	String(Payload, TEXT("WOT.StructFixture")); U32(Payload, 1);
	String(Payload, TEXT("")); Payload.Add(0);
	U32(Payload, 2);
	// Top-level properties are canonical alphabetically; members deliberately
	// stay in the declaration order of Resource and SoundEvent.
	Struct(Payload, TEXT("Budget"), TEXT("Resource"), false);
	Struct(Payload, TEXT("Cue"), TEXT("SoundEvent"), true);
	const FString Header = FString::Printf(
		TEXT("{\"class_count\":1,\"default_count\":2,\"encoding\":\"ue1-class-defaults-v2\",\"fact_id\":\"spec.struct-defaults\",\"importer\":\"class-default-v2\",\"importer_version\":2,\"kind\":\"class_default\",\"payload_byte_length\":%d,\"reference_count\":1,\"source\":{\"export\":\"WOT.StructFixture\",\"package\":\"System/WOT.u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"),
		Payload.Num());
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTPayloadActivationTest_FrameObject(Header, Payload),
			TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
	if (!TestFalse(TEXT("class-default struct frame decodes"), Decoded.IsError()))
	{
		AddError(Decoded.GetError().ToString()); return false;
	}
	FWoTRuntimeLocalizationDefaults Defaults;
	FWoTBootstrapStatus Loaded =
		Defaults.LoadClassDefaultsFirstUseObjectForTesting(
			MoveTemp(Decoded).GetValue());
	if (!TestFalse(TEXT("first-use validator accepts Resource and SoundEvent"),
		Loaded.IsError()))
	{
		AddError(Loaded.GetError().ToString()); return false;
	}
	TestEqual(TEXT("first-use retains the exact class-default payload"),
		Defaults.ClassDefaultBytes().Num(), Payload.Num());

	TArray<uint8> LegacyPayload = Payload;
	LegacyPayload[8] = 2;
	const FString LegacyHeader = FString::Printf(
		TEXT("{\"class_count\":1,\"default_count\":2,\"encoding\":\"ue1-class-defaults-v2\",\"fact_id\":\"spec.struct-defaults\",\"importer\":\"class-default-v2\",\"importer_version\":2,\"kind\":\"class_default\",\"payload_byte_length\":%d,\"reference_count\":1,\"source\":{\"export\":\"WOT.StructFixture\",\"package\":\"System/WOT.u\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}"),
		LegacyPayload.Num());
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> LegacyFirstUse =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTPayloadActivationTest_FrameObject(LegacyHeader, LegacyPayload),
			TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
	if (!TestFalse(TEXT("legacy v2 first-use frame decodes"),
		LegacyFirstUse.IsError()))
	{
		FWoTRuntimeLocalizationDefaults LegacyDefaults;
		TestTrue(TEXT("first-use reader rejects WOTDEF3 v2"),
			LegacyDefaults.LoadClassDefaultsFirstUseObjectForTesting(
				MoveTemp(LegacyFirstUse).GetValue()).IsError());
	}
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> LegacyGraph =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTPayloadActivationTest_FrameObject(LegacyHeader, LegacyPayload),
			TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
	if (!TestFalse(TEXT("legacy v2 graph frame decodes"), LegacyGraph.IsError()))
	{
		TArray<FWoTRuntimeVerifiedObject> Objects;
		Objects.Add(MoveTemp(LegacyGraph).GetValue());
		FWoTRuntimeClassDefaults LegacyDefaults;
		FString LegacyError;
		TestFalse(TEXT("class-default graph reader rejects WOTDEF3 v2"),
			LegacyDefaults.LoadObjectsForTesting(MoveTemp(Objects), LegacyError));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorDeathmatchRulesSpec,
	"WoT.World.Projector.DeathmatchRules",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorDeathmatchRulesSpec::RunTest(const FString&)
{
	auto ObjectProperty = [](const TCHAR* Name, uint32 Index,
		const TCHAR* Identity)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ArrayIndex = Index;
		Property.ObjectIdentity = Identity;
		Property.ObjectClassIdentity = TEXT("Core.Class");
		return Property;
	};
	auto ByteProperty = [](const TCHAR* Name, uint32 Index, uint8 Value)
	{
		FWoTRuntimeWorldProperty Property = WoTRuntimeMapProjectionProperty(
			Name, EWoTRuntimeWorldPropertyType::Byte, TEXT(""), Value);
		Property.ArrayIndex = Index;
		return Property;
	};
	TArray<FWoTRuntimeClassDefaultRecord> ClassRecords;
	auto AddClass = [&ClassRecords](const TCHAR* Identity, const TCHAR* Super)
		-> FWoTRuntimeClassDefaultRecord&
	{
		FWoTRuntimeClassDefaultRecord& Record = ClassRecords.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
		return Record;
	};
	AddClass(TEXT("Engine.Actor"), TEXT(""));
	AddClass(TEXT("Angreal.AngrealSpawnPool"), TEXT("Engine.Actor"));
	FWoTRuntimeClassDefaultRecord& Rare = AddClass(
		TEXT("Angreal.RareAngrealSpawnPool"),
		TEXT("Angreal.AngrealSpawnPool"));
	Rare.Properties.Add(ObjectProperty(TEXT("Pool"), 0,
		TEXT("Angreal.AngrealInvFireball")));
	// WOTDEF3 preserves AngrealSpawnPool.Weight's shipped byte tag.
	Rare.Properties.Add(ByteProperty(TEXT("Weight"), 0, 7));
	Rare.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("RespawnTime"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 80.0f));
	Rare.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bRespawn"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	FWoTRuntimeClassDefaultRecord& Water = AddClass(
		TEXT("WOT.WaterZone"), TEXT("Engine.Actor"));
	Water.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bWaterZone"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	FWoTRuntimeClassDefaultRecord& ZoneInfo = AddClass(
		TEXT("Engine.ZoneInfo"), TEXT("Engine.Actor"));
	ZoneInfo.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("AmbientBrightness"), EWoTRuntimeWorldPropertyType::Byte,
		TEXT(""), uint8(32)));
	FWoTRuntimeClassDefaultRecord& LightClass = AddClass(
		TEXT("Engine.Light"), TEXT("Engine.Actor"));
	for (const TPair<const TCHAR*, uint8>& Pair : {
		TPair<const TCHAR*, uint8>(TEXT("LightBrightness"), 128),
		TPair<const TCHAR*, uint8>(TEXT("LightHue"), 32),
		TPair<const TCHAR*, uint8>(TEXT("LightSaturation"), 64),
		TPair<const TCHAR*, uint8>(TEXT("LightRadius"), 20),
		TPair<const TCHAR*, uint8>(TEXT("LightType"), 1)})
	{
		LightClass.Properties.Add(WoTRuntimeMapProjectionProperty(
			Pair.Key, EWoTRuntimeWorldPropertyType::Byte, TEXT(""), Pair.Value));
	}
	AddClass(TEXT("Engine.LevelInfo"), TEXT("Engine.Actor"));
	AddClass(TEXT("Engine.PathNode"), TEXT("Engine.Actor"));
	FWoTRuntimeClassDefaultRecord& SprayerClass=AddClass(
		TEXT("ParticleSystems.ParticleSprayer"),TEXT("Engine.Actor"));
	SprayerClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("NumTemplates"),EWoTRuntimeWorldPropertyType::Byte,TEXT(""),uint8(1)));
	FWoTRuntimeWorldProperty ParticleTemplate;
	ParticleTemplate.Name = TEXT("Templates");
	ParticleTemplate.Type = EWoTRuntimeWorldPropertyType::Struct;
	ParticleTemplate.StructName = TEXT("Particle");
	ParticleTemplate.Value.SetNumZeroed(50);
	const float RetailTemplateFloats[8] = {
		1.0f, 1.0f, 10.0f, 5.0f, 1.0f, 0.5f, 1.0f, 0.5f};
	FMemory::Memcpy(ParticleTemplate.Value.GetData(), RetailTemplateFloats,
		sizeof(RetailTemplateFloats));
	SprayerClass.Properties.Add(MoveTemp(ParticleTemplate));
	SprayerClass.Properties.Add(ObjectProperty(TEXT("Particles"), 0,
		TEXT("ParticleSystems.SmokePuff")));
	SprayerClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bInitiallyOn"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(0)));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("projection defaults fixture loads"),
		Defaults.LoadFixture(MoveTemp(ClassRecords), Error))) return false;

	TArray<FWoTRuntimeWorldActor> Actors;
	Actors.Reserve(44);
	FWoTRuntimeWorldActor& Level = Actors.AddDefaulted_GetRef();
	Level.Identity = TEXT("Synthetic.LevelInfo0");
	Level.ClassIdentity = TEXT("Engine.LevelInfo");
	Level.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Brightness"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 0.75f));
	Level.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("IdealPlayerCount"), EWoTRuntimeWorldPropertyType::String,
		TEXT(""), uint8(0))); // Replaced below with an encoded UE1 string.
	Level.Properties.Pop();
	FWoTRuntimeWorldProperty Ideal;
	Ideal.Name = TEXT("IdealPlayerCount");
	Ideal.Type = EWoTRuntimeWorldPropertyType::String;
	Ideal.Value = {4, '6', '-', '8', 0};
	Level.Properties.Add(MoveTemp(Ideal));
	FWoTRuntimeWorldActor& Pool = Actors.AddDefaulted_GetRef();
	Pool.Identity = TEXT("Synthetic.RarePool0");
	Pool.ClassIdentity = TEXT("Angreal.RareAngrealSpawnPool");
	const float PoolLocation[3] = {50, 60, 70};
	Pool.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Location"), EWoTRuntimeWorldPropertyType::Struct,
		TEXT("Vector"), PoolLocation));
	FWoTRuntimeWorldActor& ZoneActor = Actors.AddDefaulted_GetRef();
	ZoneActor.Identity = TEXT("Synthetic.WaterZone0");
	ZoneActor.ClassIdentity = TEXT("WOT.WaterZone");
	FWoTRuntimeWorldActor& Light = Actors.AddDefaulted_GetRef();
	Light.Identity = TEXT("Synthetic.Light0");
	Light.ClassIdentity = TEXT("Engine.Light");
	const float LightLocation[3] = {10, 20, 30};
	Light.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Location"), EWoTRuntimeWorldPropertyType::Struct,
		TEXT("Vector"), LightLocation));
	FWoTRuntimeWorldActor& PathA = Actors.AddDefaulted_GetRef();
	PathA.Identity = TEXT("Synthetic.PathNode0");
	PathA.ClassIdentity = TEXT("Engine.PathNode");
	PathA.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bSnipe"), EWoTRuntimeWorldPropertyType::Boolean, TEXT(""), uint8(1)));
	FWoTRuntimeWorldProperty OutPath = WoTRuntimeMapProjectionProperty(
		TEXT("Paths"), EWoTRuntimeWorldPropertyType::Integer, TEXT(""), int32(0));
	OutPath.ArrayIndex = 0;
	PathA.Properties.Add(MoveTemp(OutPath));
	FWoTRuntimeWorldActor& PathB = Actors.AddDefaulted_GetRef();
	PathB.Identity = TEXT("Synthetic.PathNode1");
	PathB.ClassIdentity = TEXT("Engine.PathNode");
	FWoTRuntimeWorldProperty InPath = WoTRuntimeMapProjectionProperty(
		TEXT("upstreamPaths"), EWoTRuntimeWorldPropertyType::Integer,
		TEXT(""), int32(0));
	InPath.ArrayIndex = 0;
	PathB.Properties.Add(MoveTemp(InPath));
	FWoTRuntimeWorldActor& Sprayer=Actors.AddDefaulted_GetRef();
	Sprayer.Identity=TEXT("Synthetic.AuthoredOffSprayer0");
	Sprayer.ClassIdentity=TEXT("ParticleSystems.ParticleSprayer");
	FWoTRuntimeWorldActor& AmbientZone = Actors.AddDefaulted_GetRef();
	AmbientZone.Identity = TEXT("Synthetic.ZoneInfo0");
	AmbientZone.ClassIdentity = TEXT("Engine.ZoneInfo");
	FWoTRuntimeWorldActor& EmptyPool = Actors.AddDefaulted_GetRef();
	EmptyPool.Identity = TEXT("Synthetic.EmptyPool0");
	EmptyPool.ClassIdentity = TEXT("Angreal.RareAngrealSpawnPool");
	EmptyPool.Properties.Add(ObjectProperty(TEXT("Pool"), 0,
		TEXT("Angreal.UnportedInventory")));
	for (int32 Index = 0; Index < 35; ++Index)
	{
		FWoTRuntimeWorldActor& Unsupported = Actors.AddDefaulted_GetRef();
		Unsupported.Identity = FString::Printf(TEXT("Synthetic.Unsupported%d"), Index);
		Unsupported.ClassIdentity = FString::Printf(
			TEXT("Synthetic.UnsupportedClass%d"), Index);
	}

	TArray<FVector3f> Points = {
		FVector3f(-100, -50, -25), FVector3f(100, -50, -25),
		FVector3f(100, 50, 75), FVector3f(-100, 50, 75)};
	TArray<FWoTRuntimeWorldVertex> Vertices;
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		FWoTRuntimeWorldVertex& Vertex = Vertices.AddDefaulted_GetRef();
		Vertex.Point = Index;
	}
	TArray<FWoTRuntimeWorldNode> Nodes;
	FWoTRuntimeWorldNode& Node = Nodes.AddDefaulted_GetRef();
	Node.VertexPool = 0;
	Node.VertexCount = 4;
	Node.Zones[0] = 1;
	Node.Zones[1] = 0;
	Node.Plane[2] = 1.0f;
	TArray<FWoTRuntimeWorldZone> Zones;
	Zones.SetNum(2);
	Zones[1].ActorIdentity = ZoneActor.Identity;
	TArray<FWoTRuntimeWorldReachSpec> ReachSpecs;
	FWoTRuntimeWorldReachSpec& Reach = ReachSpecs.AddDefaulted_GetRef();
	Reach.StartIdentity = PathA.Identity;
	Reach.EndIdentity = PathB.Identity;
	Reach.Distance = 222;

	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("SyntheticArena");
	Source.Records = Actors;
	Source.Points = Points;
	Source.Nodes = Nodes;
	Source.Vertices = Vertices;
	Source.Zones = Zones;
	Source.ReachSpecs = ReachSpecs;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("DeathmatchRules"));
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("deathmatch rules project"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error)))
	{
		AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	TestEqual(TEXT("one pool"), Projection.RuleCount(TEXT("SpawnPool")), 1u);
	TestEqual(TEXT("one zone"), Projection.RuleCount(TEXT("Zone")), 1u);
	TestEqual(TEXT("one point light"), Projection.RuleCount(TEXT("Light")), 1u);
	TestEqual(TEXT("one skylight"), Projection.RuleCount(TEXT("SkyLight")), 1u);
	TestEqual(TEXT("one nav volume"), Projection.RuleCount(TEXT("NavBounds")), 1u);
	TestEqual(TEXT("two path nodes"), Projection.RuleCount(TEXT("PathNode")), 2u);
	TestEqual(TEXT("authored-off retail-shaped sprayer is skipped"),
		Projection.SkippedSprayerCount,1u);
	TestEqual(TEXT("unported pool entry is counted"), Projection.DroppedPoolEntries, 1u);
	TestEqual(TEXT("pool emptied by filtering is not placed"), Projection.EmptyPoolCount, 1u);
	TestEqual(TEXT("only reviewed inert LevelInfo remains inert"),Projection.InertRecordCount,1u);
	TestEqual(TEXT("unsupported records are counted"),Projection.UnsupportedRecordCount,35u);
	TestEqual(TEXT("unsupported class-name census is bounded"),
		Projection.UnsupportedClassNames.Num(), 32);
	AWoTAngrealSpawnPool* ProjectedPool = nullptr;
	AWoTZoneVolume* ProjectedZone = nullptr;
	ANavMeshBoundsVolume* NavBounds = nullptr;
	APointLight* ProjectedLight=nullptr;
	ASkyLight* ProjectedSky=nullptr;
	TArray<AWoTPathNode*> ProjectedPaths;
	for (AActor* Actor : Projection.DeferredActors)
	{
		if (!ProjectedPool) ProjectedPool = Cast<AWoTAngrealSpawnPool>(Actor);
		if (!ProjectedZone) ProjectedZone = Cast<AWoTZoneVolume>(Actor);
		if (!NavBounds) NavBounds = Cast<ANavMeshBoundsVolume>(Actor);
		if(!ProjectedLight)ProjectedLight=Cast<APointLight>(Actor);
		if(!ProjectedSky)ProjectedSky=Cast<ASkyLight>(Actor);
		if (AWoTPathNode* Path = Cast<AWoTPathNode>(Actor)) ProjectedPaths.Add(Path);
	}
	TestEqual(TEXT("authored bSnipe flag survives runtime projection"),
		ProjectedPaths.FilterByPredicate([](const AWoTPathNode* Node){return Node->bSnipe;}).Num(),1);
	if (TestNotNull(TEXT("pool actor"), ProjectedPool))
	{
		TestEqual(TEXT("pool respawn default"), ProjectedPool->RespawnTime, 80.0f);
		TestEqual(TEXT("one ported pool class"), ProjectedPool->Pool.Num(), 1);
		TestEqual(TEXT("pool weight"), ProjectedPool->Weight[0], 7);
		TestEqual(TEXT("stable pool name"), ProjectedPool->GetName(),
			FString(TEXT("WoTProjected_1_SpawnPool")));
		TestTrue(TEXT("projected startup pool is net-addressable by stable name"),
			ProjectedPool->IsNameStableForNetworking());
		ProjectedPool->SetRole(ROLE_SimulatedProxy);
		ProjectedPool->FinishSpawning(ProjectedPool->GetActorTransform(), true);
		ProjectedPool->DispatchBeginPlay();
		TestEqual(TEXT("listen-client simulated pool role"),
			ProjectedPool->GetLocalRole(), ROLE_SimulatedProxy);
		TestNull(TEXT("listen-client projected pool spawns no ghost pickup"),
			ProjectedPool->PickupItem.Get());
	}
	if (TestNotNull(TEXT("zone actor"), ProjectedZone))
	{
		TestTrue(TEXT("WaterZone class default applies"), ProjectedZone->bWaterZone);
		TestEqual(TEXT("zone center"), ProjectedZone->GetActorLocation(),
			FVector(0, 0, 25));
		TestEqual(TEXT("zone half extent"),
			ProjectedZone->Collision->GetUnscaledBoxExtent(), FVector(100, 50, 50));
	}
	if (TestNotNull(TEXT("nav bounds actor"), NavBounds))
	{
		TestTrue(TEXT("nav bounds are non-zero"),
			NavBounds->GetBrushComponent()->Bounds.BoxExtent.GetMin() > 0.0);
		const UBodySetup* Body = NavBounds->GetBrushComponent()->GetBodySetup();
		if (TestNotNull(TEXT("nav bounds have a body setup"), Body)
			&& TestEqual(TEXT("nav bounds have one box"),
				Body->AggGeom.BoxElems.Num(), 1))
		{
			const FKBoxElem& Box = Body->AggGeom.BoxElems[0];
			TestEqual(TEXT("nav X includes +100 padding"), Box.X, 400.0f);
			TestEqual(TEXT("nav Y includes +100 padding"), Box.Y, 300.0f);
			TestEqual(TEXT("nav Z includes +100 padding"), Box.Z, 300.0f);
		}
	}
	if(TestNotNull(TEXT("point light actor"),ProjectedLight))
	{
		const UPointLightComponent* Component=ProjectedLight->PointLightComponent;
		TestEqual(TEXT("light attenuation conversion"),Component->AttenuationRadius,500.0f);
		TestEqual(TEXT("light falloff conversion"),Component->LightFalloffExponent,5.3f);
		TestTrue(TEXT("light intensity conversion"),FMath::IsNearlyEqual(
			Component->Intensity,FMath::Pow(128.0f/255.0f,2.2f)*5.0f,0.0001f));
		TestEqual(TEXT("light HSV conversion uses importer raw byte RGB"),
			Component->LightColor, FColor(255,207,64,255));
		TestFalse(TEXT("dynamic retail light excludes world channel"),Component->LightingChannels.bChannel0);
		TestTrue(TEXT("dynamic retail light uses actor channel"),Component->LightingChannels.bChannel1);
	}
	if (TestNotNull(TEXT("sky light actor"), ProjectedSky))
	{
		TestTrue(TEXT("skylight uses the lowest non-zero zone ambient"),
			FMath::IsNearlyEqual(ProjectedSky->GetLightComponent()->Intensity,
				FMath::Pow(32.0f / 255.0f, 2.2f), 0.0001f));
	}
	if (ProjectedPaths.Num() == 2)
	{
		TestEqual(TEXT("reach distance projects"),
			ProjectedPaths[0]->OutEdges[0].Distance, 222.0f);
		TestEqual(TEXT("reverse reach projects"),
			ProjectedPaths[1]->InEdges[0].Other.Get(), ProjectedPaths[0]);
	}
	TestNotNull(TEXT("partial runtime light table"), Projection.MapLightTable.Get());
	TestEqual(TEXT("shell lookup returns registered transient light table"),
		UWoTMeshLight::FindMapLightTable(World),Projection.MapLightTable.Get());
	if (Projection.MapLightTable)
	{
		TestEqual(TEXT("level brightness"),
			Projection.MapLightTable->LevelBrightness, 0.75f);
		TestEqual(TEXT("ideal player count uses atoi semantics"),
			Projection.MapLightTable->IdealPlayerCount, 6);
		TestEqual(TEXT("partial table has no unavailable light rows"),
			Projection.MapLightTable->Lights.Num(), 0);
		TestEqual(TEXT("missing front child becomes its front-zone leaf"),
			Projection.MapLightTable->ZoneBspFront[0], -1);
		TestEqual(TEXT("missing back child becomes its back-zone leaf"),
			Projection.MapLightTable->ZoneBspBack[0], -2);
		TestEqual(TEXT("solid polygon node derives one CSG bit"),
			Projection.MapLightTable->LeafBspCsg[0], uint8(1));
	}
	for (AActor* Actor : Projection.DeferredActors)
		if (IsValid(Actor)) Actor->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorRetailParityRegressionsSpec,
	"WoT.World.Projector.RetailParityRegressions",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorRetailParityRegressionsSpec::RunTest(const FString&)
{
	auto AddClass = [](TArray<FWoTRuntimeClassDefaultRecord>& Records,
		const TCHAR* Identity, const TCHAR* Super)
		-> FWoTRuntimeClassDefaultRecord&
	{
		FWoTRuntimeClassDefaultRecord& Record = Records.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
		return Record;
	};
	auto IndexedInteger = [](const TCHAR* Name, uint32 Slot, int32 Value)
	{
		FWoTRuntimeWorldProperty Property = WoTRuntimeMapProjectionProperty(
			Name, EWoTRuntimeWorldPropertyType::Integer, TEXT(""), Value);
		Property.ArrayIndex = Slot;
		return Property;
	};
	auto ObjectProperty = [](const TCHAR* Name, const TCHAR* Identity)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ObjectIdentity = Identity;
		return Property;
	};
	auto AddByte = [](FWoTRuntimeWorldActor& Actor,
		const TCHAR* Name, uint8 Value)
	{
		Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
			Name, EWoTRuntimeWorldPropertyType::Byte, TEXT(""), Value));
	};
	auto AddBool = [](FWoTRuntimeWorldActor& Actor,
		const TCHAR* Name, bool Value)
	{
		Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
			Name, EWoTRuntimeWorldPropertyType::Boolean, TEXT(""),
			static_cast<uint8>(Value ? 1 : 0)));
	};
	auto AddVector = [](FWoTRuntimeWorldActor& Actor,
		const TCHAR* Name, float X, float Y, float Z)
	{
		const float Value[3] = {X, Y, Z};
		Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
			Name, EWoTRuntimeWorldPropertyType::Struct, TEXT("Vector"), Value));
	};

	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	AddClass(Classes, TEXT("Engine.Actor"), TEXT(""));
	FWoTRuntimeClassDefaultRecord& ZoneInfo = AddClass(
		Classes, TEXT("Engine.ZoneInfo"), TEXT("Engine.Actor"));
	const float NormalGravity[3] = {0.0f, 0.0f, -950.0f};
	ZoneInfo.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("ZoneGravity"), EWoTRuntimeWorldPropertyType::Struct,
		TEXT("Vector"), NormalGravity));
	AddClass(Classes, TEXT("WOT.WOTZoneInfo"), TEXT("Engine.ZoneInfo"));
	AddClass(Classes, TEXT("WOT.WaterZone"), TEXT("WOT.WOTZoneInfo"));
	AddClass(Classes, TEXT("Engine.LevelInfo"), TEXT("Engine.ZoneInfo"));
	FWoTRuntimeClassDefaultRecord& LightDefaults=AddClass(
		Classes, TEXT("Engine.Light"), TEXT("Engine.Actor"));
	LightDefaults.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("LightPeriod"),EWoTRuntimeWorldPropertyType::Byte,TEXT(""),uint8(12)));
	LightDefaults.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("LightPhase"),EWoTRuntimeWorldPropertyType::Byte,TEXT(""),uint8(3)));
	AddClass(Classes, TEXT("Engine.NavigationPoint"), TEXT("Engine.Actor"));
	AddClass(Classes, TEXT("Engine.PathNode"), TEXT("Engine.NavigationPoint"));
	AddClass(Classes, TEXT("WoTTraps.PitOpening"),
		TEXT("Engine.NavigationPoint"));

	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("retail parity defaults load"),
		Defaults.LoadFixture(MoveTemp(Classes), Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<FWoTRuntimeWorldActor> Actors;
	Actors.Reserve(9);
	FWoTRuntimeWorldActor& Level = Actors.AddDefaulted_GetRef();
	Level.Identity = TEXT("RetailShape.LevelInfo0");
	Level.ClassIdentity = TEXT("Engine.LevelInfo");
	AddByte(Level, TEXT("AmbientBrightness"), 8);
	AddByte(Level, TEXT("AmbientHue"), 0);
	AddByte(Level, TEXT("AmbientSaturation"), 0);

	FWoTRuntimeWorldActor& RetainedZone = Actors.AddDefaulted_GetRef();
	RetainedZone.Identity = TEXT("RetailShape.WOTZoneInfo0");
	RetainedZone.ClassIdentity = TEXT("WOT.WOTZoneInfo");
	AddByte(RetainedZone, TEXT("AmbientBrightness"), 8);
	AddByte(RetainedZone, TEXT("AmbientHue"), 0);
	AddByte(RetainedZone, TEXT("AmbientSaturation"), 0);

	FWoTRuntimeWorldActor& TriggerZone = Actors.AddDefaulted_GetRef();
	TriggerZone.Identity = TEXT("RetailShape.WOTZoneInfo1");
	TriggerZone.ClassIdentity = TEXT("WOT.WOTZoneInfo");
	AddVector(TriggerZone, TEXT("ZoneVelocity"), 12.0f, 0.0f, 0.0f);
	AddBool(TriggerZone, TEXT("bTrigZoneVelo2"), true);
	AddBool(TriggerZone, TEXT("bTrigZoneVelo2InitiallyOn"), false);

	FWoTRuntimeWorldActor& WaterZone = Actors.AddDefaulted_GetRef();
	WaterZone.Identity = TEXT("RetailShape.WaterZone0");
	WaterZone.ClassIdentity = TEXT("WOT.WaterZone");
	AddBool(WaterZone, TEXT("bWaterZone"), true);
	WaterZone.Properties.Add(ObjectProperty(
		TEXT("EntrySound"), TEXT("AmbientA.Water.DIVE")));
	WaterZone.Properties.Add(ObjectProperty(
		TEXT("ExitSound"), TEXT("AmbientA.Water.Stream6")));

	auto AddLight = [&Actors, &AddByte, &AddVector](
		const TCHAR* Identity, uint8 Type)
		-> FWoTRuntimeWorldActor&
	{
		FWoTRuntimeWorldActor& Light = Actors.AddDefaulted_GetRef();
		Light.Identity = Identity;
		Light.ClassIdentity = TEXT("Engine.Light");
		AddVector(Light, TEXT("Location"), 10.0f, 20.0f, 30.0f);
		AddByte(Light, TEXT("LightType"), Type);
		AddByte(Light, TEXT("LightBrightness"), 128);
		AddByte(Light, TEXT("LightHue"), 32);
		AddByte(Light, TEXT("LightSaturation"), 64);
		AddByte(Light, TEXT("LightRadius"), 20);
		return Light;
	};
	AddLight(TEXT("RetailShape.SteadyLight0"), 1);
	AddLight(TEXT("RetailShape.PulseLight0"), 2);

	FWoTRuntimeWorldActor& PathA = Actors.AddDefaulted_GetRef();
	PathA.Identity = TEXT("RetailShape.PathNode0");
	PathA.ClassIdentity = TEXT("Engine.PathNode");
	PathA.Properties.Add(IndexedInteger(TEXT("Paths"), 0, 0));
	FWoTRuntimeWorldActor& PathB = Actors.AddDefaulted_GetRef();
	PathB.Identity = TEXT("RetailShape.PathNode1");
	PathB.ClassIdentity = TEXT("Engine.PathNode");
	PathB.Properties.Add(IndexedInteger(TEXT("upstreamPaths"), 0, 0));
	FWoTRuntimeWorldActor& Pit = Actors.AddDefaulted_GetRef();
	Pit.Identity = TEXT("RetailShape.PitOpening0");
	Pit.ClassIdentity = TEXT("WoTTraps.PitOpening");

	TArray<FVector3f> Points = {
		FVector3f(-100, -100, -100), FVector3f(100, -100, -100),
		FVector3f(100, 100, 100), FVector3f(-100, 100, 100)};
	TArray<FWoTRuntimeWorldVertex> Vertices;
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		FWoTRuntimeWorldVertex& Vertex = Vertices.AddDefaulted_GetRef();
		Vertex.Point = Index;
	}
	TArray<FWoTRuntimeWorldNode> Nodes;
	for (uint8 Zone = 1; Zone <= 3; ++Zone)
	{
		FWoTRuntimeWorldNode& Node = Nodes.AddDefaulted_GetRef();
		Node.VertexPool = 0;
		Node.VertexCount = static_cast<uint8>(Vertices.Num());
		Node.Zones[0] = Zone;
		Node.Zones[1] = 0;
		Node.Plane[2] = 1.0f;
	}
	TArray<FWoTRuntimeWorldZone> Zones;
	Zones.SetNum(4);
	Zones[1].ActorIdentity = RetainedZone.Identity;
	Zones[2].ActorIdentity = TriggerZone.Identity;
	Zones[3].ActorIdentity = WaterZone.Identity;
	TArray<FWoTRuntimeWorldReachSpec> ReachSpecs;
	FWoTRuntimeWorldReachSpec& Authored = ReachSpecs.AddDefaulted_GetRef();
	Authored.StartIdentity = PathA.Identity;
	Authored.EndIdentity = PathB.Identity;
	Authored.Distance = 222;
	FWoTRuntimeWorldReachSpec& Unauthored = ReachSpecs.AddDefaulted_GetRef();
	Unauthored.StartIdentity = PathB.Identity;
	Unauthored.EndIdentity = PathA.Identity;
	Unauthored.Distance = 333;
	FWoTRuntimeWorldReachSpec& PitSpec = ReachSpecs.AddDefaulted_GetRef();
	PitSpec.StartIdentity = Pit.Identity;
	PitSpec.EndIdentity = PathA.Identity;
	PitSpec.Distance = 444;
	TArray<FWoTRuntimeWorldSurface> Surfaces;
	Surfaces.AddDefaulted_GetRef().Friction=10.0f;
	TArray<FWoTRuntimeWorldLeaf> Leaves;
	Leaves.SetNum(2);
	Leaves[0].Permeating=0;
	Leaves[1].Permeating=0;
	TArray<uint32> ModelLights={0,1,MAX_uint32};
	TArray<FWoTRuntimeWorldSemanticObject> SemanticObjects;
	for (int32 ActorIndex : {4,5})
	{
		FWoTRuntimeWorldSemanticObject& Semantic=
			SemanticObjects.AddDefaulted_GetRef();
		Semantic.Identity=Actors[ActorIndex].Identity;
		Semantic.ClassIdentity=Actors[ActorIndex].ClassIdentity;
		Semantic.bActor=true;
	}

	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("RetailShape");
	Source.Records = Actors;
	Source.Points = Points;
	Source.Nodes = Nodes;
	Source.Vertices = Vertices;
	Source.Zones = Zones;
	Source.ReachSpecs = ReachSpecs;
	Source.Surfaces = Surfaces;
	Source.Leaves = Leaves;
	Source.ModelLights = ModelLights;
	Source.SemanticObjects = SemanticObjects;
	Source.bBSPRootOutside = true;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("RetailParity"));
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("retail-shaped records project"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error)))
	{
		AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}

	TestEqual(TEXT("every bounded exact WOT/Water zone is retained"),
		Projection.RuleCount(TEXT("Zone")), 3u);
	TestEqual(TEXT("only the exact seven path-node classes are projected"),
		Projection.RuleCount(TEXT("PathNode")), 2u);
	TestEqual(TEXT("PitOpening is projected as a trap endpoint, not a path node"),
		Projection.RuleCount(TEXT("PitOpening")), 1u);
	TestEqual(TEXT("every non-None light type is projected"),
		Projection.RuleCount(TEXT("Light")), 2u);

	AWoTZoneVolume* DefaultZone = nullptr;
	AWoTZoneVolume* Triggered = nullptr;
	AWoTZoneVolume* Water = nullptr;
	AWoTPathNode* ProjectedPathA = nullptr;
	AWoTPathNode* ProjectedPathB = nullptr;
	APointLight* Steady = nullptr;
	AWoTPitOpening* ProjectedPitOpening = nullptr;
	for (AActor* Actor : Projection.DeferredActors)
	{
		const FString Name = Actor ? Actor->GetName() : FString();
		if (Name == TEXT("WoTProjected_1_Zone")) DefaultZone = Cast<AWoTZoneVolume>(Actor);
		if (Name == TEXT("WoTProjected_2_Zone")) Triggered = Cast<AWoTZoneVolume>(Actor);
		if (Name == TEXT("WoTProjected_3_Zone")) Water = Cast<AWoTZoneVolume>(Actor);
		if (Name == TEXT("WoTProjected_6_PathNode")) ProjectedPathA = Cast<AWoTPathNode>(Actor);
		if (Name == TEXT("WoTProjected_7_PathNode")) ProjectedPathB = Cast<AWoTPathNode>(Actor);
		if (Name == TEXT("WoTProjected_4_Light")) Steady = Cast<APointLight>(Actor);
		if (Name == TEXT("WoTProjected_8_PitOpening"))
			ProjectedPitOpening = Cast<AWoTPitOpening>(Actor);
	}
	if (TestNotNull(TEXT("pit opening has its dedicated native projection"),
		ProjectedPitOpening))
	{
		TestTrue(TEXT("pit opening is replicated startup state"),
			ProjectedPitOpening->GetIsReplicated()
				&& ProjectedPitOpening->IsNameStableForNetworking());
	}
	if (TestNotNull(TEXT("default-valued WOTZoneInfo exists"), DefaultZone))
	{
		TestFalse(TEXT("inherited normal gravity is not an additive override"),
			DefaultZone->bOverrideGravity);
	}
	if (TestNotNull(TEXT("TrigVelo2 zone exists"), Triggered))
	{
		TestEqual(TEXT("TrigVelo2 absent selected minimum overwrites with zero"),
			Triggered->ZoneVelocity, FVector::ZeroVector);
	}
	if (TestNotNull(TEXT("water zone exists"), Water))
	{
		TestEqual(TEXT("entry sound retains package and leaf"),
			Water->EntrySoundSubPath, FName(TEXT("AmbientA/DIVE")));
		TestEqual(TEXT("exit sound retains package and leaf"),
			Water->ExitSoundSubPath, FName(TEXT("AmbientA/Stream6")));
	}
	if (TestNotNull(TEXT("authored path source exists"), ProjectedPathA)
		&& TestNotNull(TEXT("authored path target exists"), ProjectedPathB))
	{
		TestEqual(TEXT("only authored ordered Paths entries become outgoing edges"),
			ProjectedPathA->OutEdges.Num(), 1);
		TestEqual(TEXT("authored outgoing endpoint"),
			ProjectedPathA->OutEdges[0].Other.Get(), ProjectedPathB);
		TestEqual(TEXT("unlisted global ReachSpecs do not create outgoing edges"),
			ProjectedPathB->OutEdges.Num(), 0);
		TestEqual(TEXT("only authored ordered upstreamPaths entries become incoming edges"),
			ProjectedPathB->InEdges.Num(), 1);
		TestEqual(TEXT("unlisted global ReachSpecs do not create incoming edges"),
			ProjectedPathA->InEdges.Num(), 0);
	}
	if (TestNotNull(TEXT("steady point light exists"), Steady))
	{
		const UPointLightComponent* Component = Steady->PointLightComponent;
		TestFalse(TEXT("retail exponent mode disables inverse-square falloff"),
			Component->bUseInverseSquaredFalloff);
		TestEqual(TEXT("point-light byte HSV is stored as raw importer RGB"),
			Component->LightColor, FColor(255, 207, 64, 255));
	}
	if (TestNotNull(TEXT("runtime light table exists"), Projection.MapLightTable.Get()))
	{
		TestFalse(TEXT("runtime light table is complete"),Projection.bLightTablePartial);
		TestEqual(TEXT("permeating actors become exact light rows"),
			Projection.MapLightTable->Lights.Num(),2);
		TestTrue(TEXT("pulse class defaults select waveform group zero"),
			Projection.MapLightTable->Lights[1].WaveGroup==0
			&& FMath::IsNearlyEqual(
				Projection.MapLightTable->WaveMeans[0],0.60,0.00001)
			&& FMath::IsNearlyEqual(
				Projection.MapLightTable->WaveAmplitudes[0],0.39,0.00001));
		TestTrue(TEXT("native Light radius default/override path is retained"),
			FMath::IsNearlyEqual(Projection.MapLightTable->Lights[0].Radius,525.0));
		TestEqual(TEXT("two retail leaf runs produce three CSR offsets"),
			Projection.MapLightTable->LeafLightOffsets,TArray<int32>({0,2,4}));
		TestEqual(TEXT("root-outside reaches the runtime line-check table"),
			Projection.MapLightTable->bBspRootOutside,true);
		TestEqual(TEXT("friction-10 world nodes remain ascending and sparse"),
			Projection.MapLightTable->FrictionNodeIndices,TArray<int32>({0,1,2}));
		const FWoTZoneAmbient* Ambient = Projection.MapLightTable->ZoneAmbients.FindByPredicate(
			[](const FWoTZoneAmbient& Row) { return Row.Zone == 1; });
		if (TestNotNull(TEXT("zone-one ambient exists"), Ambient))
		{
			TestTrue(TEXT("ambient uses UE1 curved brightness"),
				FMath::IsNearlyEqual(Ambient->Color.R, 0.14002111f, 0.00001f));
			TestTrue(TEXT("ambient uses UE1 piecewise hue"),
				FMath::IsNearlyZero(Ambient->Color.G)
				&& FMath::IsNearlyZero(Ambient->Color.B));
		}
	}

	for (AActor* Actor : Projection.DeferredActors)
		if (IsValid(Actor)) Actor->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityFix1WaterZoneClassViewFlashSpec,
	"WoT.Parity.Fix1.WaterZoneClassViewFlash",
	WoTRuntimeMapProjectionFlags)

bool FParityFix1WaterZoneClassViewFlashSpec::RunTest(const FString&)
{
	auto AddClass = [](TArray<FWoTRuntimeClassDefaultRecord>& Classes,
		const TCHAR* Identity, const TCHAR* Super)
		-> FWoTRuntimeClassDefaultRecord&
	{
		FWoTRuntimeClassDefaultRecord& Record = Classes.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
		return Record;
	};
	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	AddClass(Classes, TEXT("Engine.Actor"), TEXT(""));
	AddClass(Classes, TEXT("Engine.ZoneInfo"), TEXT("Engine.Actor"));
	AddClass(Classes, TEXT("WOT.WOTZoneInfo"), TEXT("Engine.ZoneInfo"));
	FWoTRuntimeClassDefaultRecord& WaterClass = AddClass(
		Classes, TEXT("WOT.WaterZone"), TEXT("WOT.WOTZoneInfo"));
	WaterClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bWaterZone"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("Fix1 zone defaults load"),
		Defaults.LoadFixture(MoveTemp(Classes), Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<FWoTRuntimeWorldActor> Actors;
	FWoTRuntimeWorldActor& Water = Actors.AddDefaulted_GetRef();
	Water.Identity = TEXT("Fix1.WaterZone0");
	Water.ClassIdentity = TEXT("WOT.WaterZone");
	FWoTRuntimeWorldActor& Generic = Actors.AddDefaulted_GetRef();
	Generic.Identity = TEXT("Fix1.WOTZoneInfo0");
	Generic.ClassIdentity = TEXT("WOT.WOTZoneInfo");
	Generic.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bWaterZone"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	TArray<FVector3f> Points = {
		FVector3f(-600,-100,-100), FVector3f(-400,-100,-100),
		FVector3f(-400, 100, 100), FVector3f(-600, 100, 100),
		FVector3f( 400,-100,-100), FVector3f( 600,-100,-100),
		FVector3f( 600, 100, 100), FVector3f( 400, 100, 100)};
	TArray<FWoTRuntimeWorldVertex> Vertices;
	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		Vertices.AddDefaulted_GetRef().Point = Point;
	}
	TArray<FWoTRuntimeWorldNode> Nodes;
	FWoTRuntimeWorldNode& WaterNode = Nodes.AddDefaulted_GetRef();
	WaterNode.VertexPool = 0;
	WaterNode.VertexCount = 4;
	WaterNode.Zones[0] = 1;
	FWoTRuntimeWorldNode& GenericNode = Nodes.AddDefaulted_GetRef();
	GenericNode.VertexPool = 4;
	GenericNode.VertexCount = 4;
	GenericNode.Zones[0] = 2;
	TArray<FWoTRuntimeWorldZone> Zones;
	Zones.SetNum(3);
	Zones[1].ActorIdentity = Water.Identity;
	Zones[2].ActorIdentity = Generic.Identity;
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("Fix1ZoneViewFlash");
	Source.Records = Actors;
	Source.Points = Points;
	Source.Vertices = Vertices;
	Source.Nodes = Nodes;
	Source.Zones = Zones;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("Fix1ZoneViewFlash"));
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("real zone projector accepts both water-shaped zones"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error)))
	{
		AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	AWoTZoneVolume* WaterVolume = nullptr;
	AWoTZoneVolume* GenericVolume = nullptr;
	for (AActor* Deferred : Projection.DeferredActors)
	{
		if (AWoTZoneVolume* Zone = Cast<AWoTZoneVolume>(Deferred))
		{
			if (Zone->GetActorLocation().X < 0.0f) WaterVolume = Zone;
			else GenericVolume = Zone;
		}
	}
	if (TestNotNull(TEXT("WOT.WaterZone projects"), WaterVolume))
	{
		TestEqual(TEXT("WOT.WaterZone receives its class-default ViewFlash"),
			WaterVolume->ViewFlash, FVector(-0.078f), KINDA_SMALL_NUMBER);
	}
	if (TestNotNull(TEXT("water-flagged WOTZoneInfo projects"), GenericVolume))
	{
		TestEqual(TEXT("WOTZoneInfo bWaterZone does not inherit WaterZone ViewFlash"),
			GenericVolume->ViewFlash, FVector::ZeroVector, KINDA_SMALL_NUMBER);
	}
	for (AActor* Deferred : Projection.DeferredActors)
		if (IsValid(Deferred)) Deferred->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorPerActorFailureSpec,
	"WoT.World.Projector.PerActorFailureDoesNotRejectWorld",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorPerActorFailureSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaultRecord ActorClass;
	ActorClass.Identity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaultRecord PoolClass;
	PoolClass.Identity = TEXT("Angreal.AngrealSpawnPool");
	PoolClass.SuperclassIdentity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("failure-policy defaults load"),
		Defaults.LoadFixture({ActorClass, PoolClass}, Error))) return false;

	FWoTRuntimeWorldActor Malformed;
	Malformed.Identity = TEXT("RetailShape.MalformedPool0");
	Malformed.ClassIdentity = TEXT("Angreal.AngrealSpawnPool");
	FWoTRuntimeWorldProperty Pool;
	Pool.Name = TEXT("Pool");
	Pool.Type = EWoTRuntimeWorldPropertyType::Object;
	Pool.ObjectIdentity = TEXT("Angreal.AngrealInvFireball");
	Malformed.Properties.Add(MoveTemp(Pool));
	Malformed.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Weight"), EWoTRuntimeWorldPropertyType::Float, TEXT(""), 1.0f));
	TArray<FWoTRuntimeWorldActor> Actors = {MoveTemp(Malformed)};
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("MalformedActorPolicy");
	Source.Records = Actors;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("PerActorFailure"));
	FWoTWorldProjectionResult Projection;
	const bool bProjected = FWoTWorldProjector::ProjectDeferred(
		*World, Source, Defaults, Projection, Error);
	TestTrue(TEXT("one malformed actor does not reject the world"), bProjected);
	TestEqual(TEXT("malformed pool is not projected"),
		Projection.RuleCount(TEXT("SpawnPool")), 0u);
	TestEqual(TEXT("malformed pool increments the per-rule skip count"),
		Projection.SkippedRuleCount(TEXT("SpawnPool")), 1u);
	TestEqual(TEXT("malformed pool emits one bounded skip sample"),
		Projection.SkippedSamples.Num(), 1);
	TestTrue(TEXT("skip sample identifies the malformed actor and reason"),
		Projection.SkippedSamples[0].Contains(TEXT("RetailShape.MalformedPool0"))
		&& Projection.SkippedSamples[0].Contains(TEXT("weight%20is%20malformed")));
	TestTrue(TEXT("the otherwise valid projection still installs its fallback start"),
		Projection.bUsedPlayerStartFallback);
	for (AActor* Actor : Projection.DeferredActors)
		if (IsValid(Actor)) Actor->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorSpewDescriptorMutationSpec,
	"WoT.World.Projector.SpewDescriptorMutation",
	WoTRuntimeMapProjectionFlags)
bool FWoTWorldProjectorSpewDescriptorMutationSpec::RunTest(const FString&)
{
	auto ObjectProperty = [](const TCHAR* Name, const TCHAR* Identity,
		uint32 ArrayIndex = 0)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.ArrayIndex = ArrayIndex;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ObjectIdentity = Identity;
		return Property;
	};
	auto ProjectFixture = [&](float CollisionRadius, float Damping,
		const TCHAR* SkinIdentity, FWoTSpewThing& OutThing)
	{
		FWoTRuntimeClassDefaultRecord Actor;
		Actor.Identity = TEXT("Engine.Actor");
		FWoTRuntimeClassDefaultRecord SpewClass;
		SpewClass.Identity = TEXT("WOT.SpewTrigger");
		SpewClass.SuperclassIdentity = Actor.Identity;
		FWoTRuntimeClassDefaultRecord ThingClass;
		ThingClass.Identity = TEXT("WOTDecorations.FixtureBounce");
		ThingClass.SuperclassIdentity = Actor.Identity;
		ThingClass.Properties.Add(ObjectProperty(TEXT("Mesh"),
			TEXT("WOTDecorations.FixtureMesh")));
		ThingClass.Properties.Add(ObjectProperty(TEXT("MultiSkins"), SkinIdentity, 3));
		ThingClass.Properties.Add(ObjectProperty(TEXT("LandSound1"),
			TEXT("WOTDecorations.FixtureBounceSound")));
		auto AddFloat = [&ThingClass](const TCHAR* Name, float Value)
		{
			ThingClass.Properties.Add(WoTRuntimeMapProjectionProperty(
				Name, EWoTRuntimeWorldPropertyType::Float, TEXT(""), Value));
		};
		AddFloat(TEXT("CollisionRadius"), CollisionRadius);
		AddFloat(TEXT("CollisionHeight"), 23.0f);
		AddFloat(TEXT("LifeSpan"), 8.0f);
		AddFloat(TEXT("DrawScale"), 1.25f);
		AddFloat(TEXT("MinBounceSoundSpeed"), 51.0f);
		AddFloat(TEXT("MinFirstHitSpeed"), 61.0f);
		AddFloat(TEXT("MinBounceSpeed"), 71.0f);
		AddFloat(TEXT("SpeedDampeningRatio"), Damping);
		AddFloat(TEXT("RotationDampeningRatio"), 0.31f);
		AddFloat(TEXT("BaseBounceVolume"), 0.8f);
		AddFloat(TEXT("BaseBouncePitch"), 1.2f);
		AddFloat(TEXT("VolumePerturbPercent"), 11.0f);
		AddFloat(TEXT("PitchPerturbPercent"), 13.0f);

		FWoTRuntimeClassDefaults Defaults;
		FString Error;
		if (!Defaults.LoadFixture({Actor, SpewClass, ThingClass}, Error))
		{
			AddError(Error);
			return false;
		}
		FWoTRuntimeWorldActor Spew;
		Spew.Identity = TEXT("FixtureMap.SpewTrigger0");
		Spew.ClassIdentity = SpewClass.Identity;
		FWoTRuntimeWorldProperty Things;
		Things.Name = TEXT("Things");
		Things.Type = EWoTRuntimeWorldPropertyType::Struct;
		Things.StructName = TEXT("ThingT");
		Things.StructMembers.Add(ObjectProperty(TEXT("Actor"), *ThingClass.Identity));
		Things.StructMembers.Add(WoTRuntimeMapProjectionProperty(TEXT("Count"),
			EWoTRuntimeWorldPropertyType::Integer, TEXT(""), int32(2)));
		Things.StructMembers.Add(WoTRuntimeMapProjectionProperty(TEXT("DetailLevel"),
			EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(1)));
		Things.StructMembers.Add(WoTRuntimeMapProjectionProperty(TEXT("MinSpeed"),
			EWoTRuntimeWorldPropertyType::Float, TEXT(""), 100.0f));
		Things.StructMembers.Add(WoTRuntimeMapProjectionProperty(TEXT("MaxSpeed"),
			EWoTRuntimeWorldPropertyType::Float, TEXT(""), 200.0f));
		Spew.Properties.Add(MoveTemp(Things));
		FWoTWorldProjectionSource Source;
		Source.MapIdentity = TEXT("FixtureMap");
		TArray<FWoTRuntimeWorldActor> Actors;
		Actors.Add(MoveTemp(Spew));
		Source.Records = Actors;
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("SpewDescriptor"));
		FWoTWorldProjectionResult Projection;
		const bool bProjected = FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error);
		AWoTSpewTrigger* Projected = nullptr;
		for (AActor* Deferred : Projection.DeferredActors)
			if (AWoTSpewTrigger* Candidate = Cast<AWoTSpewTrigger>(Deferred))
				Projected = Candidate;
		const bool bValid = bProjected && Projected && Projected->Things.Num() == 1;
		if (bValid) OutThing = Projected->Things[0];
		else AddError(Error.IsEmpty() ? TEXT("Spew fixture did not project") : Error);
		for (AActor* Deferred : Projection.DeferredActors)
			if (IsValid(Deferred)) Deferred->Destroy();
		WoTRuntimeMapProjectionDestroyWorld(World);
		return bValid;
	};

	FWoTSpewThing First;
	FWoTSpewThing Mutated;
	if (!TestTrue(TEXT("decoded Spew descriptor fixture projects"),
		ProjectFixture(17.0f, 0.42f, TEXT("WOTDecorations.Skins.First"), First))
		|| !TestTrue(TEXT("mutated Spew descriptor fixture projects"),
			ProjectFixture(29.0f, 0.68f,
				TEXT("WOTDecorations.Skins.Mutated"), Mutated))) return false;
	TestEqual(TEXT("ThingT retains the retail class identity"), First.ClassIdentity,
		FName(TEXT("WOTDecorations.FixtureBounce")));
	TestEqual(TEXT("class descriptor resolves its mesh"), First.MeshName,
		FName(TEXT("FixtureMesh")));
	TestEqual(TEXT("class descriptor retains exact skin slot"),
		First.SkinIdentities[3], FName(TEXT("WOTDecorations.Skins.First")));
	TestEqual(TEXT("class descriptor decodes collision"), First.CollisionRadius, 17.0f);
	TestEqual(TEXT("class descriptor decodes lifetime"), First.LifeSpan, 8.0f);
	TestEqual(TEXT("class descriptor decodes bounce sound"), First.LandSound,
		FName(TEXT("WOTDecorations.FixtureBounceSound")));
	TestEqual(TEXT("class descriptor decodes bounce volume"),
		First.BaseBounceVolume, 0.8f);
	TestEqual(TEXT("class descriptor decodes bounce pitch"),
		First.BaseBouncePitch, 1.2f);
	TestEqual(TEXT("mutating collision defaults changes runtime descriptor"),
		Mutated.CollisionRadius, 29.0f);
	TestEqual(TEXT("mutating damping defaults changes runtime descriptor"),
		Mutated.SpeedDampeningRatio, 0.68f);
	TestEqual(TEXT("mutating a source skin changes its exact runtime slot"),
		Mutated.SkinIdentities[3], FName(TEXT("WOTDecorations.Skins.Mutated")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorMalformedConsumedFieldsSpec,
	"WoT.World.Projector.MalformedConsumedFieldsSkipOwningRule",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorMalformedConsumedFieldsSpec::RunTest(const FString&)
{
	auto Property = [](const TCHAR* Name,
		EWoTRuntimeWorldPropertyType Type, uint8 Value = 1)
	{
		if (Type == EWoTRuntimeWorldPropertyType::Float)
		{
			return WoTRuntimeMapProjectionProperty(
				Name, Type, TEXT(""), static_cast<float>(Value));
		}
		return WoTRuntimeMapProjectionProperty(Name, Type, TEXT(""), Value);
	};
	auto AddClass = [](TArray<FWoTRuntimeClassDefaultRecord>& Records,
		const TCHAR* Identity, const TCHAR* Super)
		-> FWoTRuntimeClassDefaultRecord&
	{
		FWoTRuntimeClassDefaultRecord& Record = Records.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
		return Record;
	};
	struct FMalformedCase
	{
		const TCHAR* PropertyName;
		EWoTRuntimeWorldPropertyType WrongType;
		const TCHAR* ClassIdentity;
		const TCHAR* Rule;
		bool bZone = false;
	};
	const FMalformedCase Cases[] = {
		{TEXT("Pool"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("Angreal.AngrealSpawnPool"), TEXT("SpawnPool")},
		{TEXT("Weight"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("Angreal.AngrealSpawnPool"), TEXT("SpawnPool")},
		{TEXT("bRespawn"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("Angreal.AngrealSpawnPool"), TEXT("SpawnPool")},
		{TEXT("RespawnTime"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("Angreal.AngrealSpawnPool"), TEXT("SpawnPool")},
		{TEXT("PainInterval"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("WOT.WaterZone"), TEXT("Zone"), true},
		{TEXT("DamageType"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("WOT.WaterZone"), TEXT("Zone"), true},
		{TEXT("EntrySound"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("WOT.WaterZone"), TEXT("Zone"), true},
		{TEXT("ExitSound"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("WOT.WaterZone"), TEXT("Zone"), true},
		{TEXT("HealSound"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("WOT.WaterZone"), TEXT("Zone"), true},
		{TEXT("Tag"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("Engine.Light"), TEXT("Light")},
		{TEXT("LightTag"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("WOT.Lightning"), TEXT("Light")},
		{TEXT("Brightness"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("Engine.LevelInfo"), TEXT("LightTable")},
		{TEXT("bCheckWalkSurfaces"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("Engine.LevelInfo"), TEXT("LightTable")},
		{TEXT("AmbientHue"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("Engine.LevelInfo"), TEXT("LightTable")},
		{TEXT("AmbientSaturation"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("Engine.LevelInfo"), TEXT("LightTable")},
		{TEXT("AmbientBrightness"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("Engine.LevelInfo"), TEXT("LightTable")},
		{TEXT("IdealPlayerCount"), EWoTRuntimeWorldPropertyType::Byte,
			TEXT("Engine.LevelInfo"), TEXT("LightTable")},
		{TEXT("AmbientHue"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("WOT.WaterZone"), TEXT("LightTable"), true},
		{TEXT("AmbientSaturation"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("WOT.WaterZone"), TEXT("LightTable"), true},
		{TEXT("AmbientBrightness"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("WOT.WaterZone"), TEXT("LightTable"), true},
		{TEXT("MinLightCount"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("WOT.WaterZone"), TEXT("LightTable"), true},
		{TEXT("MaxLightCount"), EWoTRuntimeWorldPropertyType::Float,
			TEXT("WOT.WaterZone"), TEXT("LightTable"), true},
	};

	for (int32 CaseIndex = 0; CaseIndex < UE_ARRAY_COUNT(Cases); ++CaseIndex)
	{
		const FMalformedCase& Case = Cases[CaseIndex];
		TArray<FWoTRuntimeClassDefaultRecord> Classes;
		AddClass(Classes, TEXT("Engine.Actor"), TEXT(""));
		FWoTRuntimeClassDefaultRecord& PoolClass = AddClass(Classes,
			TEXT("Angreal.AngrealSpawnPool"), TEXT("Engine.Actor"));
		FWoTRuntimeWorldProperty PoolDefault;
		PoolDefault.Name = TEXT("Pool");
		PoolDefault.Type = EWoTRuntimeWorldPropertyType::Object;
		PoolDefault.ObjectIdentity = TEXT("Angreal.Inventory");
		PoolClass.Properties.Add(MoveTemp(PoolDefault));
		PoolClass.Properties.Add(Property(TEXT("Weight"),
			EWoTRuntimeWorldPropertyType::Byte));
		AddClass(Classes, TEXT("Engine.LevelInfo"), TEXT("Engine.Actor"));
		FWoTRuntimeClassDefaultRecord& LightClass = AddClass(Classes,
			TEXT("Engine.Light"), TEXT("Engine.Actor"));
		for (const TCHAR* Name : {TEXT("LightType"), TEXT("LightBrightness"),
			TEXT("LightHue"), TEXT("LightSaturation"), TEXT("LightRadius")})
		{
			LightClass.Properties.Add(Property(Name,
				EWoTRuntimeWorldPropertyType::Byte));
		}
		AddClass(Classes, TEXT("Engine.ZoneInfo"), TEXT("Engine.Actor"));
		AddClass(Classes, TEXT("WOT.WOTZoneInfo"), TEXT("Engine.ZoneInfo"));
		FWoTRuntimeClassDefaultRecord& WaterClass = AddClass(Classes,
			TEXT("WOT.WaterZone"), TEXT("WOT.WOTZoneInfo"));
		WaterClass.Properties.Add(Property(TEXT("bWaterZone"),
			EWoTRuntimeWorldPropertyType::Boolean));
		AddClass(Classes, TEXT("WOT.Lightning"), TEXT("Engine.Actor"));
		FWoTRuntimeClassDefaults Defaults;
		FString Error;
		if (!TestTrue(FString::Printf(TEXT("case %d defaults load"), CaseIndex),
			Defaults.LoadFixture(MoveTemp(Classes), Error))) return false;

		FWoTRuntimeWorldActor Actor;
		Actor.Identity = FString::Printf(TEXT("Malformed.Case%d"), CaseIndex);
		Actor.ClassIdentity = Case.ClassIdentity;
		Actor.Properties.Add(Property(Case.PropertyName, Case.WrongType));
		TArray<FWoTRuntimeWorldActor> Actors = {MoveTemp(Actor)};
		TArray<FVector3f> Points;
		TArray<FWoTRuntimeWorldVertex> Vertices;
		TArray<FWoTRuntimeWorldNode> Nodes;
		TArray<FWoTRuntimeWorldZone> Zones;
		FWoTWorldProjectionSource Source;
		Source.MapIdentity = TEXT("MalformedConsumedField");
		if (Case.bZone)
		{
			Points = {FVector3f(-10, -10, -10),
				FVector3f(10, -10, -10), FVector3f(10, 10, 10)};
			for (int32 Point = 0; Point < Points.Num(); ++Point)
			{
				FWoTRuntimeWorldVertex& Vertex =
					Vertices.AddDefaulted_GetRef();
				Vertex.Point = Point;
			}
			FWoTRuntimeWorldNode& Node = Nodes.AddDefaulted_GetRef();
			Node.VertexPool = 0;
			Node.VertexCount = 3;
			Node.Zones[0] = 1;
			Zones.SetNum(2);
			Zones[1].ActorIdentity = Actors[0].Identity;
		}
		Source.Records = Actors;
		Source.Points = Points;
		Source.Vertices = Vertices;
		Source.Nodes = Nodes;
		Source.Zones = Zones;
		UWorld* World = WoTRuntimeMapProjectionWorld(
			*FString::Printf(TEXT("MalformedConsumed%d"), CaseIndex));
		FWoTWorldProjectionResult Projection;
		const bool bProjected = FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error);
		TestTrue(FString::Printf(TEXT("case %d preserves the world"), CaseIndex),
			bProjected);
		TestEqual(FString::Printf(TEXT("case %d skips its owning rule"), CaseIndex),
			Projection.SkippedRuleCount(Case.Rule), 1u);
		TestTrue(FString::Printf(TEXT("case %d names the malformed actor"), CaseIndex),
			Projection.SkippedSamples.ContainsByPredicate(
				[&Source](const FString& Sample)
				{
					return Sample.Contains(Source.Records[0].Identity);
				}));
		for (AActor* Projected : Projection.DeferredActors)
			if (IsValid(Projected)) Projected->Destroy();
		WoTRuntimeMapProjectionDestroyWorld(World);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorSerializedActorValueSpec,
	"WoT.World.Projector.SerializedActorValueFailureDoesNotRejectWorld",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorSerializedActorValueSpec::RunTest(const FString&)
{
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
			WoTWorldMaterializerTest_MalformedPoolValueObject(),
			TEXT("ue1-world-records-le-v2"), TEXT("world"));
	if (!TestFalse(TEXT("malformed actor-value object frame decodes"),
		Decoded.IsError()))
	{
		AddError(Decoded.GetError().ToString());
		return false;
	}
	FWoTRuntimeWorldResource Resource;
	FWoTBootstrapStatus Loaded =
		Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue());
	if (!TestFalse(TEXT("actor-local NaN does not reject the serialized world"),
		Loaded.IsError()))
	{
		AddError(Loaded.GetError().ToString());
		return false;
	}

	FWoTRuntimeClassDefaultRecord ActorClass;
	ActorClass.Identity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaultRecord PathClass;
	PathClass.Identity = TEXT("Engine.PathNode");
	PathClass.SuperclassIdentity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaultRecord PoolClass;
	PoolClass.Identity = TEXT("Angreal.AngrealSpawnPool");
	PoolClass.SuperclassIdentity = TEXT("Engine.Actor");
	FWoTRuntimeWorldProperty Pool;
	Pool.Name = TEXT("Pool");
	Pool.Type = EWoTRuntimeWorldPropertyType::Object;
	Pool.ObjectIdentity = TEXT("Angreal.Inventory");
	PoolClass.Properties.Add(MoveTemp(Pool));
	PoolClass.Properties.Add(WoTRuntimeMapProjectionProperty(TEXT("Weight"),
		EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(1)));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("serialized-value defaults load"), Defaults.LoadFixture(
		{ActorClass, PathClass, PoolClass}, Error))) return false;
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = Resource.MapIdentity();
	Source.Records = Resource.Actors();
	Source.Points = Resource.Points();
	Source.Nodes = Resource.Nodes();
	Source.Vertices = Resource.Vertices();
	Source.Zones = Resource.Zones();
	Source.ReachSpecs = Resource.ReachSpecs();
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("SerializedActorValue"));
	FWoTWorldProjectionResult Projection;
	TestTrue(TEXT("serialized actor-local value failure preserves the world"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error));
	TestEqual(TEXT("serialized malformed pool is skipped"),
		Projection.SkippedRuleCount(TEXT("SpawnPool")), 1u);
	TestEqual(TEXT("serialized malformed pool is not counted"),
		Projection.RuleCount(TEXT("SpawnPool")), 0u);
	TestTrue(TEXT("serialized malformed pool is named"),
		Projection.SkippedSamples.ContainsByPredicate([](const FString& Sample)
		{
			return Sample.Contains(TEXT("MalformedPoolValue.AngrealSpawnPool0"));
		}));
	for (AActor* Projected : Projection.DeferredActors)
		if (IsValid(Projected)) Projected->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorOversizedNameSpec,
	"WoT.World.Projector.OversizedNameValueSkipsActor",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorOversizedNameSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaultRecord ActorClass;
	ActorClass.Identity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaultRecord LightClass;
	LightClass.Identity = TEXT("Engine.Light");
	LightClass.SuperclassIdentity = ActorClass.Identity;
	auto AddByte = [&LightClass](const TCHAR* Name, uint8 Value)
	{
		LightClass.Properties.Add(WoTRuntimeMapProjectionProperty(
			Name, EWoTRuntimeWorldPropertyType::Byte, TEXT(""), Value));
	};
	AddByte(TEXT("LightType"), 1);
	AddByte(TEXT("LightBrightness"), 64);
	AddByte(TEXT("LightHue"), 0);
	AddByte(TEXT("LightSaturation"), 255);
	AddByte(TEXT("LightRadius"), 64);
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("oversized-name defaults load"),
		Defaults.LoadFixture({ActorClass, LightClass}, Error))) return false;

	FWoTRuntimeWorldActor Light;
	Light.Identity = TEXT("OversizedName.Light0");
	Light.ClassIdentity = LightClass.Identity;
	FWoTRuntimeWorldProperty Tag;
	Tag.Name = TEXT("Tag");
	Tag.Type = EWoTRuntimeWorldPropertyType::Name;
	Tag.NameValue = FString::ChrN(NAME_SIZE, TEXT('T'));
	Light.Properties.Add(MoveTemp(Tag));
	TArray<FWoTRuntimeWorldActor> Actors = {MoveTemp(Light)};
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("OversizedName");
	Source.Records = Actors;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("OversizedName"));
	FWoTWorldProjectionResult Projection;
	TestTrue(TEXT("oversized Name value preserves the world"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error));
	TestEqual(TEXT("oversized Name value skips the owning light"),
		Projection.SkippedRuleCount(TEXT("Light")), 1u);
	TestEqual(TEXT("oversized Name value does not count a light"),
		Projection.RuleCount(TEXT("Light")), 0u);
	TestTrue(TEXT("oversized Name skip sample is bounded and names the actor"),
		Projection.SkippedSamples.Num() == 1
		&& Projection.SkippedSamples[0].Len() <= 257
		&& Projection.SkippedSamples[0].Contains(Source.Records[0].Identity));
	for (AActor* Projected : Projection.DeferredActors)
		if (IsValid(Projected)) Projected->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorOversizedSoundPathSpec,
	"WoT.World.Projector.OversizedSoundPathSkipsActor",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorOversizedSoundPathSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaultRecord ActorClass;
	ActorClass.Identity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaultRecord ZoneClass;
	ZoneClass.Identity = TEXT("Engine.ZoneInfo");
	ZoneClass.SuperclassIdentity = ActorClass.Identity;
	FWoTRuntimeClassDefaultRecord WotZoneClass;
	WotZoneClass.Identity = TEXT("WOT.WOTZoneInfo");
	WotZoneClass.SuperclassIdentity = ZoneClass.Identity;
	FWoTRuntimeClassDefaultRecord WaterClass;
	WaterClass.Identity = TEXT("WOT.WaterZone");
	WaterClass.SuperclassIdentity = WotZoneClass.Identity;
	WaterClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bWaterZone"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("oversized-sound defaults load"), Defaults.LoadFixture(
		{ActorClass, ZoneClass, WotZoneClass, WaterClass}, Error))) return false;

	FWoTRuntimeWorldActor Zone;
	Zone.Identity = TEXT("OversizedSound.WaterZone0");
	Zone.ClassIdentity = WaterClass.Identity;
	FWoTRuntimeWorldProperty EntrySound;
	EntrySound.Name = TEXT("EntrySound");
	EntrySound.Type = EWoTRuntimeWorldPropertyType::Object;
	EntrySound.ObjectIdentity = FString::ChrN(NAME_SIZE - 4, TEXT('P'))
		+ TEXT(".Wave");
	Zone.Properties.Add(MoveTemp(EntrySound));
	TArray<FWoTRuntimeWorldActor> Actors = {MoveTemp(Zone)};
	TArray<FVector3f> Points = {FVector3f(-10, -10, -10),
		FVector3f(10, -10, -10), FVector3f(10, 10, 10)};
	TArray<FWoTRuntimeWorldVertex> Vertices;
	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		FWoTRuntimeWorldVertex& Vertex = Vertices.AddDefaulted_GetRef();
		Vertex.Point = Point;
	}
	TArray<FWoTRuntimeWorldNode> Nodes;
	FWoTRuntimeWorldNode& Node = Nodes.AddDefaulted_GetRef();
	Node.VertexPool = 0;
	Node.VertexCount = 3;
	Node.Zones[0] = 1;
	TArray<FWoTRuntimeWorldZone> Zones;
	Zones.SetNum(2);
	Zones[1].ActorIdentity = Actors[0].Identity;
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("OversizedSound");
	Source.Records = Actors;
	Source.Points = Points;
	Source.Vertices = Vertices;
	Source.Nodes = Nodes;
	Source.Zones = Zones;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("OversizedSound"));
	FWoTWorldProjectionResult Projection;
	TestTrue(TEXT("oversized sound subpath preserves the world"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error));
	TestEqual(TEXT("oversized sound subpath skips the owning zone"),
		Projection.SkippedRuleCount(TEXT("Zone")), 1u);
	TestEqual(TEXT("oversized sound subpath does not count a zone"),
		Projection.RuleCount(TEXT("Zone")), 0u);
	TestTrue(TEXT("oversized sound skip sample is bounded and names the actor"),
		Projection.SkippedSamples.Num() == 1
		&& Projection.SkippedSamples[0].Len() <= 257
		&& Projection.SkippedSamples[0].Contains(Source.Records[0].Identity));
	for (AActor* Projected : Projection.DeferredActors)
		if (IsValid(Projected)) Projected->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorParticleWorkBoundSpec,
	"WoT.World.Projector.ParticleEmissionWorkBound",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorParticleWorkBoundSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaultRecord ActorClass;
	ActorClass.Identity = TEXT("Engine.Actor");
	FWoTRuntimeClassDefaultRecord SprayerClass;
	SprayerClass.Identity = TEXT("ParticleSystems.ParticleSprayer");
	SprayerClass.SuperclassIdentity = ActorClass.Identity;
	SprayerClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("NumTemplates"), EWoTRuntimeWorldPropertyType::Byte,
		TEXT(""), uint8(1)));
	FWoTRuntimeWorldProperty Template;
	Template.Name = TEXT("Templates");
	Template.Type = EWoTRuntimeWorldPropertyType::Struct;
	Template.StructName = TEXT("Particle");
	Template.Value.SetNumZeroed(50);
	const float TemplateFloats[] = {
		1.0f, 1.0f, 10.0f, 5.0f, 1.0f, 0.5f, 1.0f, 0.5f};
	FMemory::Memcpy(Template.Value.GetData(), TemplateFloats,
		sizeof(TemplateFloats));
	SprayerClass.Properties.Add(MoveTemp(Template));
	FWoTRuntimeWorldProperty Particle;
	Particle.Name = TEXT("Particles");
	Particle.Type = EWoTRuntimeWorldPropertyType::Object;
	Particle.ObjectIdentity = TEXT("ParticleSystems.SmokePuff");
	SprayerClass.Properties.Add(MoveTemp(Particle));
	SprayerClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bInitiallyOn"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("particle-work defaults load"),
		Defaults.LoadFixture({ActorClass, SprayerClass}, Error))) return false;

	FWoTRuntimeWorldActor Sprayer;
	Sprayer.Identity = TEXT("ParticleWork.ParticleSprayer0");
	Sprayer.ClassIdentity = SprayerClass.Identity;
	Sprayer.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Volume"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 1.0e20f));
	TArray<FWoTRuntimeWorldActor> Actors = {Sprayer};
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("ParticleWork");
	Source.Records = Actors;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("ParticleWork"));
	FWoTWorldProjectionResult Projection;
	TestTrue(TEXT("unsafe finite particle Volume preserves the world"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error));
	TestEqual(TEXT("unsafe finite particle Volume skips its sprayer"),
		Projection.SkippedRuleCount(TEXT("ParticleSprayer")), 1u);
	TestEqual(TEXT("unsafe finite particle Volume does not count a sprayer"),
		Projection.RuleCount(TEXT("ParticleSprayer")), 0u);
	TestTrue(TEXT("unsafe finite particle Volume reports configuration failure"),
		Projection.SkippedSamples.ContainsByPredicate([](const FString& Sample)
		{
			return Sample.Contains(TEXT("ParticleWork.ParticleSprayer0"))
				&& Sample.Contains(
					TEXT("configuration%20is%20malformed%20or%20unsupported"));
		}));

	FWoTRuntimeWorldActor BoundarySprayer = Sprayer;
	const float SupportedVolume = 8192.0f;
	BoundarySprayer.Properties[0] = WoTRuntimeMapProjectionProperty(
		TEXT("Volume"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), SupportedVolume);
	FWoTRuntimeParticleSprayerConfiguration BoundaryConfig;
	TestTrue(TEXT("supported emission boundary decodes"),
		WoTRuntimeReadParticleSprayerConfiguration(
			[&](FStringView Name, uint32 Slot)
			{
				return Defaults.FindProperty(BoundarySprayer, Name, Slot);
			}, BoundaryConfig));
	AWoTParticleSprayer* Consumer = World->SpawnActor<AWoTParticleSprayer>();
	if (TestNotNull(TEXT("supported boundary consumer spawns"), Consumer))
	{
		WoTRuntimeApplyParticleSprayerConfiguration(BoundaryConfig, *Consumer);
		Consumer->PrimeCount = 1.0f;
		Consumer->PrimeAuthoredCount();
		TestTrue(TEXT("supported boundary completes a real 30 Hz prime tick"),
			IsValid(Consumer));
		Consumer->Destroy();
	}
	for (AActor* Projected : Projection.DeferredActors)
		if (IsValid(Projected)) Projected->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorBattleRulesSpec,
	"WoT.World.Projector.BattleRules",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorBattleRulesSpec::RunTest(const FString&)
{
	auto AddClass = [](TArray<FWoTRuntimeClassDefaultRecord>& Records,
		const TCHAR* Identity, const TCHAR* Super)
		-> FWoTRuntimeClassDefaultRecord&
	{
		FWoTRuntimeClassDefaultRecord& Record = Records.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
		return Record;
	};
	auto ObjectProperty = [](const TCHAR* Name, uint32 Slot,
		const TCHAR* Identity)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.ArrayIndex = Slot;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ObjectIdentity = Identity;
		Property.ObjectClassIdentity = TEXT("Core.Class");
		return Property;
	};
	auto NameProperty = [](const TCHAR* Name, const TCHAR* Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Name;
		Property.NameValue = Value;
		return Property;
	};
	auto StringProperty = [](const TCHAR* Name, const TCHAR* Value)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::String;
		const FTCHARToUTF8 Encoded(Value);
		check(Encoded.Length() + 1 < 64);
		Property.Value.Add(static_cast<uint8>(Encoded.Length() + 1));
		Property.Value.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		Property.Value.Add(0);
		return Property;
	};
	auto ResourceProperty = [&NameProperty](uint32 Slot, const TCHAR* Package,
		const TCHAR* Adapter, int32 ItemCount)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = TEXT("Budget");
		Property.ArrayIndex = Slot;
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.StructName = TEXT("Resource");
		Property.StructMembers.Add(
			NameProperty(TEXT("ResourcePackageName"), Package));
		Property.StructMembers.Add(
			NameProperty(TEXT("ResourceAdapterName"), Adapter));
		Property.StructMembers.Add(WoTRuntimeMapProjectionProperty(
			TEXT("ItemCount"), EWoTRuntimeWorldPropertyType::Integer,
			TEXT(""), ItemCount));
		return Property;
	};
	auto AddBudgetDefaults = [&ResourceProperty](
		FWoTRuntimeClassDefaultRecord& Record,
		const TCHAR* Troop0, const TCHAR* Troop1, const TCHAR* Troop2)
	{
		const TCHAR* Packages[] = {
			TEXT("WOT"), TEXT("WOTPawns"), TEXT("WOTPawns"),
			TEXT("WOTPawns"), TEXT("WOTTraps"), TEXT("WOTTraps"),
			TEXT("WOTTraps"), TEXT("WOTTraps"), TEXT("WOTTraps"),
			TEXT("WOTTraps"), TEXT("WOTTraps")};
		const TCHAR* Adapters[] = {
			TEXT("SealInventory"), Troop0, Troop1, Troop2,
			TEXT("AlarmInventory"), TEXT("FireWallInventory"),
			TEXT("WallInventory"), TEXT("SpearInventory"),
			TEXT("PitInventory"), TEXT("StaircaseInventory"),
			TEXT("PortcullisInventory")};
		const int32 Counts[] = {1, 3, 2, 1, 1, 1, 2, 6, 2, 2, 2};
		for (uint32 Slot = 0; Slot < UE_ARRAY_COUNT(Counts); ++Slot)
			Record.Properties.Add(ResourceProperty(
				Slot, Packages[Slot], Adapters[Slot], Counts[Slot]));
	};

	TArray<FWoTRuntimeClassDefaultRecord> ClassRecords;
	AddClass(ClassRecords, TEXT("Engine.Actor"), TEXT(""));
	AddClass(ClassRecords, TEXT("Engine.Info"), TEXT("Engine.Actor"));
	AddClass(ClassRecords, TEXT("Engine.Keypoint"), TEXT("Engine.Actor"));
	AddClass(ClassRecords, TEXT("WOT.BattleInfo"), TEXT("Engine.Info"));
	FWoTRuntimeClassDefaultRecord& AltarClass = AddClass(
		ClassRecords, TEXT("WOT.SealAltar"), TEXT("Engine.Keypoint"));
	AltarClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(255)));
	AltarClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("SealGoal"), EWoTRuntimeWorldPropertyType::Integer, TEXT(""), int32(4)));
	AltarClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("CollisionRadius"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 48.0f));
	AltarClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("CollisionHeight"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 1.0f));
	AddClass(ClassRecords, TEXT("WOT.BudgetInfo"), TEXT("Engine.Info"));
	FWoTRuntimeClassDefaultRecord& AesBudgetClass = AddClass(
		ClassRecords, TEXT("WOT.AesSedaiBudget"), TEXT("WOT.BudgetInfo"));
	AddBudgetDefaults(AesBudgetClass, TEXT("WarderInventory"),
		TEXT("SisterInventory"), TEXT("SitterInventory"));
	FWoTRuntimeClassDefaultRecord& HoundBudgetClass = AddClass(
		ClassRecords, TEXT("WOT.HoundBudget"), TEXT("WOT.BudgetInfo"));
	AddBudgetDefaults(HoundBudgetClass, TEXT("MinionInventory"),
		TEXT("MashadarInventory"), TEXT("LegionInventory"));
	FWoTRuntimeClassDefaultRecord& GuideClass = AddClass(
		ClassRecords, TEXT("WOTPawns.MashadarGuide"), TEXT("Engine.Actor"));
	GuideClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(255)));
	GuideClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("SearchRadius"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 600.0f));
	GuideClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("TriggerRadius"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 200.0f));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("Battle projection defaults fixture loads"),
		Defaults.LoadFixture(MoveTemp(ClassRecords), Error)))
	{
		AddError(Error);
		return false;
	}

	TArray<FWoTRuntimeWorldActor> Actors;
	FWoTRuntimeWorldActor& Battle = Actors.AddDefaulted_GetRef();
	Battle.Identity = TEXT("SyntheticBattle.BattleInfo0");
	Battle.ClassIdentity = TEXT("WOT.BattleInfo");
	Battle.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("NumTeams"), EWoTRuntimeWorldPropertyType::Integer,
		TEXT(""), int32(3)));
	Battle.Properties.Add(ObjectProperty(
		TEXT("TeamClass"), 0, TEXT("WOTPawns.Forsaken#export-00000123")));
	Battle.Properties.Add(ObjectProperty(
		TEXT("TeamClass"), 1, TEXT("WOTPawns.Hound")));
	Battle.Properties.Add(ObjectProperty(
		TEXT("TeamClass"), 2, TEXT("WOTPawns.AesSedai")));
	Battle.Properties.Add(StringProperty(
		TEXT("EditingStr"), TEXT("Prepare your Citadel")));
	Battle.Properties.Add(StringProperty(
		TEXT("PlayingStr"), TEXT("Let the battle begin")));
	Battle.Properties.Add(StringProperty(
		TEXT("ReadyStr"), TEXT("Prepare yourself for battle")));
	Battle.Properties.Add(StringProperty(
		TEXT("WaitingStr"), TEXT("Waiting for players")));

	FWoTRuntimeWorldActor& DefaultAltar = Actors.AddDefaulted_GetRef();
	DefaultAltar.Identity = TEXT("SyntheticBattle.SealAltar0");
	DefaultAltar.ClassIdentity = TEXT("WOT.SealAltar");
	DefaultAltar.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(0)));
	FWoTRuntimeWorldActor& WideAltar = Actors.AddDefaulted_GetRef();
	WideAltar.Identity = TEXT("SyntheticBattle.SealAltar1");
	WideAltar.ClassIdentity = TEXT("WOT.SealAltar");
	WideAltar.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(1)));
	WideAltar.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("CollisionRadius"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 140.0f));

	FWoTRuntimeWorldActor& AesBudget = Actors.AddDefaulted_GetRef();
	AesBudget.Identity = TEXT("SyntheticBattle.AesSedaiBudget0");
	AesBudget.ClassIdentity = TEXT("WOT.AesSedaiBudget");
	AesBudget.Properties.Add(NameProperty(
		TEXT("BeginEditEvent"), TEXT("AesBeginEdit")));
	AesBudget.Properties.Add(NameProperty(
		TEXT("EndEditEvent"), TEXT("AesEndEdit")));
	AesBudget.Properties.Add(ResourceProperty(
		6, TEXT("WOTTraps"), TEXT("WallInventory"), 5));
	FWoTRuntimeWorldActor& HoundBudget = Actors.AddDefaulted_GetRef();
	HoundBudget.Identity = TEXT("SyntheticBattle.HoundBudget0");
	HoundBudget.ClassIdentity = TEXT("WOT.HoundBudget");
	HoundBudget.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(1)));
	HoundBudget.Properties.Add(ResourceProperty(
		9, TEXT("WOTTraps"), TEXT("StaircaseInventory"), 1));

	FWoTRuntimeWorldActor& Guide = Actors.AddDefaulted_GetRef();
	Guide.Identity = TEXT("SyntheticBattle.MashadarGuide0");
	Guide.ClassIdentity = TEXT("WOTPawns.MashadarGuide");
	const float GuideLocation[3] = {100.0f, 200.0f, 300.0f};
	Guide.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Location"), EWoTRuntimeWorldPropertyType::Struct,
		TEXT("Vector"), GuideLocation));
	const int32 GuideRotation[3] = {0, 16384, 0};
	Guide.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Rotation"), EWoTRuntimeWorldPropertyType::Struct,
		TEXT("Rotator"), GuideRotation));
	FWoTRuntimeWorldActor& AuthoredGuide = Actors.AddDefaulted_GetRef();
	AuthoredGuide.Identity = TEXT("SyntheticBattle.MashadarGuide1");
	AuthoredGuide.ClassIdentity = TEXT("WOTPawns.MashadarGuide");
	AuthoredGuide.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(1)));
	AuthoredGuide.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("SearchRadius"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 1600.0f));
	AuthoredGuide.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("TriggerRadius"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 275.0f));

	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("SyntheticBattle");
	Source.Records = Actors;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("BattleRules"));
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("Battle rules project"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error)))
	{
		AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	TestEqual(TEXT("one BattleInfo"), Projection.RuleCount(TEXT("BattleInfo")), 1u);
	TestEqual(TEXT("two SealAltars"), Projection.RuleCount(TEXT("SealAltar")), 2u);
	TestEqual(TEXT("two faction budgets"), Projection.RuleCount(TEXT("Budget")), 2u);
	TestEqual(TEXT("two MashadarGuides"),
		Projection.RuleCount(TEXT("MashadarGuide")), 2u);

	AWoTBattleInfo* ProjectedBattle = nullptr;
	TArray<AWoTMashadarGuide*> ProjectedGuides;
	TArray<AWoTSealAltar*> ProjectedAltars;
	TArray<AWoTBudgetInfo*> ProjectedBudgets;
	for (AActor* Actor : Projection.DeferredActors)
	{
		if (!ProjectedBattle) ProjectedBattle = Cast<AWoTBattleInfo>(Actor);
		if (AWoTMashadarGuide* ProjectedGuide = Cast<AWoTMashadarGuide>(Actor))
			ProjectedGuides.Add(ProjectedGuide);
		if (AWoTSealAltar* Altar = Cast<AWoTSealAltar>(Actor))
			ProjectedAltars.Add(Altar);
		if (AWoTBudgetInfo* Budget = Cast<AWoTBudgetInfo>(Actor))
			ProjectedBudgets.Add(Budget);
	}
	if (TestNotNull(TEXT("projected BattleInfo"), ProjectedBattle))
	{
		TestEqual(TEXT("three teams retained"), ProjectedBattle->NumTeams, 3);
		TestTrue(TEXT("team zero maps to Forsaken native pawn"),
			ProjectedBattle->TeamClass[0] == AWoTForsaken::StaticClass());
		TestTrue(TEXT("team one maps to Hound native pawn"),
			ProjectedBattle->TeamClass[1] == AWoTHound::StaticClass());
		TestTrue(TEXT("team two maps to Aes Sedai native pawn"),
			ProjectedBattle->TeamClass[2] == AWoTAesSedai::StaticClass());
		TestEqual(TEXT("B3 editing string retained"), ProjectedBattle->EditingStr,
			FString(TEXT("Prepare your Citadel")));
		TestEqual(TEXT("B3 playing string retained"), ProjectedBattle->PlayingStr,
			FString(TEXT("Let the battle begin")));
		TestEqual(TEXT("B3 ready string retained"), ProjectedBattle->ReadyStr,
			FString(TEXT("Prepare yourself for battle")));
		TestEqual(TEXT("B3 waiting string retained"), ProjectedBattle->WaitingStr,
			FString(TEXT("Waiting for players")));
		TestTrue(TEXT("BattleInfo is replicated"), ProjectedBattle->GetIsReplicated());
		TestTrue(TEXT("BattleInfo has a stable network name"),
			ProjectedBattle->IsNameStableForNetworking());
		TestEqual(TEXT("BattleInfo projected name"), ProjectedBattle->GetName(),
			FString(TEXT("WoTProjected_0_BattleInfo")));
	}
	TestEqual(TEXT("two altar actors found"), ProjectedAltars.Num(), 2);
	if (ProjectedAltars.Num() == 2)
	{
		TestFalse(TEXT("SealAltars stay authority-local"),
			ProjectedAltars[0]->GetIsReplicated());
		TestEqual(TEXT("default altar keeps shipped radius"),
			ProjectedAltars[0]->Touch->GetUnscaledCapsuleRadius(), 48.0f);
		TestEqual(TEXT("authored altar radius is honored"),
			ProjectedAltars[1]->Touch->GetUnscaledCapsuleRadius(), 140.0f);
		// The Touch capsule is only the broad phase: its straight section spans
		// the authored one-unit cylinder (plus the pawn-cap margin); the exact
		// flat cylinder is applied by the overlap filter.
		TestEqual(TEXT("UE capsule contains the authored one-unit cylinder"),
			ProjectedAltars[1]->Touch->GetUnscaledCapsuleHalfHeight(),
			WoTUE1Collision::ContainingCapsuleHalfHeight(140.0f, 1.0f));
		TestEqual(TEXT("authored altar team retained"),
			static_cast<int32>(ProjectedAltars[1]->Team), 1);
	}
	TestEqual(TEXT("two budget actors found"), ProjectedBudgets.Num(), 2);
	for (AWoTBudgetInfo* Budget : ProjectedBudgets)
	{
		TestFalse(TEXT("budgets stay authority-local"), Budget->GetIsReplicated());
		if (!TestEqual(TEXT("budget has all eleven Resource lines"),
			Budget->Budget.Num(), 11)) continue;
		TestEqual(TEXT("seal package retained"),
			Budget->Budget[0].ResourcePackageName, FName(TEXT("WOT")));
		TestEqual(TEXT("seal adapter retained"),
			Budget->Budget[0].ResourceAdapterName, FName(TEXT("SealInventory")));
		if (Budget->Budget[1].ResourceAdapterName == FName(TEXT("WarderInventory")))
		{
			TestEqual(TEXT("omitted Team uses retail byte-zero default"),
				static_cast<int32>(Budget->Team), 0);
			TestEqual(TEXT("Aes instance override wins over class default"),
				Budget->Budget[6].ItemCount, 5);
			TestEqual(TEXT("budget begin-edit event retained"),
				Budget->BeginEditEvent, FName(TEXT("AesBeginEdit")));
			TestEqual(TEXT("budget end-edit event retained"),
				Budget->EndEditEvent, FName(TEXT("AesEndEdit")));
		}
		else
		{
			TestEqual(TEXT("authored Hound Team retained"),
				static_cast<int32>(Budget->Team), 1);
			TestEqual(TEXT("Hound instance override wins over class default"),
				Budget->Budget[9].ItemCount, 1);
		}
	}
	TestEqual(TEXT("two guide actors found"), ProjectedGuides.Num(), 2);
	AWoTMashadarGuide* DefaultGuide = nullptr;
	AWoTMashadarGuide* AuthoredGuideActor = nullptr;
	for (AWoTMashadarGuide* ProjectedGuide : ProjectedGuides)
	{
		if (ProjectedGuide->Team == 255) DefaultGuide = ProjectedGuide;
		if (ProjectedGuide->Team == 1) AuthoredGuideActor = ProjectedGuide;
		TestTrue(TEXT("guide is marked citadel-placed"),
			ProjectedGuide->bCitadelPlaced);
		TestTrue(TEXT("guide is replicated"), ProjectedGuide->GetIsReplicated());
		TestTrue(TEXT("guide has a stable network name"),
			ProjectedGuide->IsNameStableForNetworking());
	}
	if (TestNotNull(TEXT("default projected MashadarGuide"), DefaultGuide))
	{
		TestEqual(TEXT("guide default team is neutral"),
			static_cast<int32>(DefaultGuide->Team), 255);
		TestEqual(TEXT("guide uses shipped search radius"),
			DefaultGuide->SearchRadius, 600.0f);
		TestEqual(TEXT("guide uses shipped trigger radius"),
			DefaultGuide->TriggerRadius, 200.0f);
		TestTrue(TEXT("guide rotation is projected"),
			FMath::IsNearlyEqual(DefaultGuide->GetActorRotation().Yaw, 90.0f));
		TestEqual(TEXT("guide seeds the client initial location before BeginPlay"),
			DefaultGuide->InitialGuideLocation, FVector(100.0, 200.0, 300.0));
		TestEqual(TEXT("guide seeds the client hiding place before BeginPlay"),
			DefaultGuide->HidingPlaceLocation, FVector(100.0, 200.0, 300.0));
		TestTrue(TEXT("guide seeds the client initial rotation before BeginPlay"),
			DefaultGuide->InitialGuideRotation.Equals(FRotator(0.0, 90.0, 0.0)));
		TestEqual(TEXT("guide seeds the client initial speed before BeginPlay"),
			DefaultGuide->InitialSpeed, DefaultGuide->Speed);
		TestEqual(TEXT("guide seeds the client initial max speed before BeginPlay"),
			DefaultGuide->InitialMaxSpeed, DefaultGuide->MaxSpeed);
		TestEqual(TEXT("guide seeds the client initial acceleration before BeginPlay"),
			DefaultGuide->InitialAcceleration, DefaultGuide->Acceleration);
	}
	if (TestNotNull(TEXT("authored projected MashadarGuide"), AuthoredGuideActor))
	{
		TestEqual(TEXT("guide authored team retained"),
			static_cast<int32>(AuthoredGuideActor->Team), 1);
		TestEqual(TEXT("guide authored search radius retained"),
			AuthoredGuideActor->SearchRadius, 1600.0f);
		TestEqual(TEXT("guide authored trigger radius retained"),
			AuthoredGuideActor->TriggerRadius, 275.0f);
	}

	for (AActor* Actor : Projection.DeferredActors)
		if (IsValid(Actor)) Actor->Destroy();
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorBattleModeFlowSpec,
	"WoT.World.Projector.BattleModeFlow",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorBattleModeFlowSpec::RunTest(const FString&)
{
	auto AddClass = [](TArray<FWoTRuntimeClassDefaultRecord>& Records,
		const TCHAR* Identity, const TCHAR* Super)
		-> FWoTRuntimeClassDefaultRecord&
	{
		FWoTRuntimeClassDefaultRecord& Record = Records.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
		return Record;
	};
	auto ObjectProperty = [](const TCHAR* Name, uint32 Slot,
		const TCHAR* Identity)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.ArrayIndex = Slot;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ObjectIdentity = Identity;
		Property.ObjectClassIdentity = TEXT("Core.Class");
		return Property;
	};
	auto ResourceProperty = [](int32 ItemCount)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = TEXT("Budget");
		Property.Type = EWoTRuntimeWorldPropertyType::Struct;
		Property.StructName = TEXT("Resource");
		FWoTRuntimeWorldProperty Package;
		Package.Name = TEXT("ResourcePackageName");
		Package.Type = EWoTRuntimeWorldPropertyType::Name;
		Package.NameValue = TEXT("WOT");
		Property.StructMembers.Add(MoveTemp(Package));
		FWoTRuntimeWorldProperty Adapter;
		Adapter.Name = TEXT("ResourceAdapterName");
		Adapter.Type = EWoTRuntimeWorldPropertyType::Name;
		Adapter.NameValue = TEXT("SealInventory");
		Property.StructMembers.Add(MoveTemp(Adapter));
		Property.StructMembers.Add(WoTRuntimeMapProjectionProperty(
			TEXT("ItemCount"), EWoTRuntimeWorldPropertyType::Integer,
			TEXT(""), ItemCount));
		return Property;
	};

	TArray<FWoTRuntimeClassDefaultRecord> ClassRecords;
	AddClass(ClassRecords, TEXT("Engine.Actor"), TEXT(""));
	AddClass(ClassRecords, TEXT("Engine.Info"), TEXT("Engine.Actor"));
	AddClass(ClassRecords, TEXT("Engine.Keypoint"), TEXT("Engine.Actor"));
	AddClass(ClassRecords, TEXT("Engine.PlayerStart"), TEXT("Engine.Actor"));
	AddClass(ClassRecords, TEXT("Engine.Teleporter"), TEXT("Engine.Actor"));
	AddClass(ClassRecords, TEXT("WOT.BattleInfo"), TEXT("Engine.Info"));
	AddClass(ClassRecords, TEXT("WOT.BudgetInfo"), TEXT("Engine.Info"));
	FWoTRuntimeClassDefaultRecord& AesBudgetClass = AddClass(
		ClassRecords, TEXT("WOT.AesSedaiBudget"), TEXT("WOT.BudgetInfo"));
	AesBudgetClass.Properties.Add(ResourceProperty(1));
	FWoTRuntimeClassDefaultRecord& HoundBudgetClass = AddClass(
		ClassRecords, TEXT("WOT.HoundBudget"), TEXT("WOT.BudgetInfo"));
	HoundBudgetClass.Properties.Add(ResourceProperty(1));
	FWoTRuntimeClassDefaultRecord& AltarClass = AddClass(
		ClassRecords, TEXT("WOT.SealAltar"), TEXT("Engine.Keypoint"));
	AltarClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("SealGoal"), EWoTRuntimeWorldPropertyType::Integer, TEXT(""), int32(4)));
	AltarClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("CollisionRadius"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 48.0f));
	AltarClass.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("CollisionHeight"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 1.0f));
	FWoTRuntimeClassDefaults Defaults;
	FString ProjectionError;
	if (!TestTrue(TEXT("Battle-mode projection defaults load"),
		Defaults.LoadFixture(MoveTemp(ClassRecords), ProjectionError)))
	{
		AddError(ProjectionError);
		return false;
	}

	TArray<FWoTRuntimeWorldActor> Actors;
	FWoTRuntimeWorldActor& Battle = Actors.AddDefaulted_GetRef();
	Battle.Identity = TEXT("ProjectedFlow.BattleInfo0");
	Battle.ClassIdentity = TEXT("WOT.BattleInfo");
	Battle.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("NumTeams"), EWoTRuntimeWorldPropertyType::Integer,
		TEXT(""), int32(2)));
	Battle.Properties.Add(ObjectProperty(
		TEXT("TeamClass"), 0, TEXT("WOTPawns.AesSedai")));
	Battle.Properties.Add(ObjectProperty(
		TEXT("TeamClass"), 1, TEXT("WOTPawns.Hound")));
	FWoTRuntimeWorldActor& Team0Budget = Actors.AddDefaulted_GetRef();
	Team0Budget.Identity = TEXT("ProjectedFlow.AesSedaiBudget0");
	Team0Budget.ClassIdentity = TEXT("WOT.AesSedaiBudget");
	FWoTRuntimeWorldActor& Team1Budget = Actors.AddDefaulted_GetRef();
	Team1Budget.Identity = TEXT("ProjectedFlow.HoundBudget0");
	Team1Budget.ClassIdentity = TEXT("WOT.HoundBudget");
	Team1Budget.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(1)));
	for (uint8 Team = 0; Team < 2; ++Team)
	{
		FWoTRuntimeWorldActor& Altar = Actors.AddDefaulted_GetRef();
		Altar.Identity = FString::Printf(TEXT("ProjectedFlow.SealAltar%d"), Team);
		Altar.ClassIdentity = TEXT("WOT.SealAltar");
		Altar.Properties.Add(WoTRuntimeMapProjectionProperty(
			TEXT("Team"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), Team));
	}
	FWoTRuntimeWorldActor& Teleporter = Actors.AddDefaulted_GetRef();
	Teleporter.Identity = TEXT("ProjectedFlow.Teleporter0");
	Teleporter.ClassIdentity = TEXT("Engine.Teleporter");
	for (uint8 Team = 0; Team < 2; ++Team)
	{
		FWoTRuntimeWorldActor& Start = Actors.AddDefaulted_GetRef();
		Start.Identity = FString::Printf(TEXT("ProjectedFlow.PlayerStart%d"), Team);
		Start.ClassIdentity = TEXT("Engine.PlayerStart");
		Start.Properties.Add(WoTRuntimeMapProjectionProperty(
			TEXT("TeamNumber"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), Team));
		const float Location[3] = {
			Team == 0 ? -2000.0f : 2000.0f, 0.0f, 100.0f};
		Start.Properties.Add(WoTRuntimeMapProjectionProperty(
			TEXT("Location"), EWoTRuntimeWorldPropertyType::Struct,
			TEXT("Vector"), Location));
	}
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("ProjectedFlow");
	Source.Records = Actors;
	FWoTWorldProjectionResult Projection;
	bool bProjectionSucceeded = false;
	WoTTest::FSpecWorldOptions Options;
	Options.GameModeClass = AWoTBattleGameMode::StaticClass();
	Options.BeforeInitializeActorsForPlay =
		[&](UWorld* World)
		{
			bProjectionSucceeded = FWoTWorldProjector::ProjectDeferred(
				*World, Source, Defaults, Projection, ProjectionError);
			if (!bProjectionSucceeded) return;
			for (AActor* Actor : Projection.DeferredActors)
			{
				if (IsValid(Actor)) Actor->FinishSpawning(Actor->GetActorTransform());
			}
		};
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (!TestNotNull(TEXT("projected Battle-mode world"), World)) return false;
	if (!TestTrue(TEXT("Battle actors project before world initialization"),
		bProjectionSucceeded))
	{
		AddError(ProjectionError);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTBattleGameMode* Mode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!TestNotNull(TEXT("Battle game mode"), Mode)
		|| !TestNotNull(TEXT("BeginPlay located projected BattleInfo"),
			Mode ? Mode->Battle.Get() : nullptr))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("the located actor keeps its stable projected name"),
		Mode->Battle->GetName(), FString(TEXT("WoTProjected_0_BattleInfo")));
	TestFalse(TEXT("ResetBattle starts in the editing phase"), Mode->CanTeleport());

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTPlayerController* Controller =
		World->SpawnActor<AWoTPlayerController>(Spawn);
	if (!TestNotNull(TEXT("host controller"), Controller))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Controller->Player = NewObject<ULocalPlayer>(GEngine);
	Controller->InitPlayerState();
	if (AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>())
	{
		State->Team = 0;
		Mode->Battle->NumPlayers[0] = 1;
	}
	Mode->PostLogin(Controller);
	AWoTPlayerState* State = Controller->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("PostLogin retained player state"), State))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("PostLogin makes a new Battle player neutral"),
		static_cast<int32>(State->Team), 255);
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>(Spawn);
	if (!TestNotNull(TEXT("host team-select HUD"), HUD))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	HUD->PlayerOwner = Controller;
	TestTrue(TEXT("host HUD digit routes through ChangeTeam + restart"),
		HUD->HandleTeamSelectDigit(0));
	TestEqual(TEXT("HUD team join updates the player state"),
		static_cast<int32>(State->Team), 0);
	TestEqual(TEXT("HUD team join updates BattleInfo headcount"),
		Mode->Battle->NumPlayers[0], 1);

	AWoTInventory* Pickup = World->SpawnActor<AWoTAndilayRoot>(Spawn);
	APawn* Pawn = Controller->GetPawn();
	if (TestNotNull(TEXT("HUD restart produced the faction pawn"), Pawn)
		&& TestNotNull(TEXT("pickup query fixture"), Pickup))
	{
		TestFalse(TEXT("pickups stay disabled during projected editing phase"),
			Mode->PickupQuery(Pawn, Pickup));
	}
	TArray<AWoTBudgetInfo*> Budgets;
	for (TActorIterator<AWoTBudgetInfo> It(World); It; ++It) Budgets.Add(*It);
	TestEqual(TEXT("both projected team budgets are live"), Budgets.Num(), 2);
	Mode->AssessBattlefield();
	TestFalse(TEXT("unspent projected seal budgets block play"),
		Mode->CanTeleport());
	for (AWoTBudgetInfo* Budget : Budgets)
	{
		while (Budget->SealBudgetRemaining() > 0)
			Budget->Decrement(FName(TEXT("SealInventory")));
	}
	Mode->SetEditing(0, true);
	Mode->AssessBattlefield();
	TestFalse(TEXT("an editing team still blocks play after spending"),
		Mode->CanTeleport());
	Mode->SetEditing(0, false);
	Mode->AssessBattlefield();
	TestTrue(TEXT("all budgets spent and editors clear enables teleporters"),
		Mode->CanTeleport());
	TestFalse(TEXT("EnableTeleporters clears the projected BattleInfo gate"),
		Mode->Battle->bTeleportersDisabled);
	if (Pawn && Pickup)
	{
		TestTrue(TEXT("pickups are allowed after projected Battle opens"),
			Mode->PickupQuery(Pawn, Pickup));
	}

	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMoverGlideInterpolationSpec,
	"WoT.Mover.GlideInterpolation",
	WoTRuntimeMapProjectionFlags)

bool FWoTMoverGlideInterpolationSpec::RunTest(const FString&)
{
	struct FSample
	{
		float Alpha;
		float Glide;
	};
	static constexpr FSample Samples[] = {
		{0.25f, 0.15625f},
		{0.50f, 0.50f},
		{0.75f, 0.84375f},
	};
	for (const FSample& Sample : Samples)
	{
		TestEqual(FString::Printf(TEXT("MoveByTime alpha %.2f stays linear"), Sample.Alpha),
			AWoTMover::EvaluateGlideAlpha(EWoTMoverGlideType::MoveByTime, Sample.Alpha),
			Sample.Alpha, UE_SMALL_NUMBER);
		TestEqual(FString::Printf(TEXT("GlideByTime alpha %.2f uses retail smoothstep"), Sample.Alpha),
			AWoTMover::EvaluateGlideAlpha(EWoTMoverGlideType::GlideByTime, Sample.Alpha),
			Sample.Glide, UE_SMALL_NUMBER);
	}
	return !HasAnyErrors();
}

#if !WOT_GUARDED_BOOTSTRAP
// This EditorContext-only oracle reads purchaser-owned raw packages through
// the development parser, whose declarations and implementation are absent by
// design in guarded targets. Keep the complete editor spec, but do not compile
// an unreachable raw-retail oracle into the guarded Program harness.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorRealRetailCensusSpec,
	"WoT.World.Projector.RealRetailCensus",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorRealRetailCensusSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: production census not verified"));
		return true;
	}
	struct FExpectedRetailProjectionCensus
	{
		const TCHAR* Map;
		uint32 PlayerStart;
		uint32 SpawnPool;
		uint32 Zone;
		uint32 Light;
		uint32 SkyLight;
		uint32 NavBounds;
		uint32 PathNode;
		uint32 ParticleSprayer;
		uint32 FixedPickup;
		uint32 AndilayRoot;
		uint32 Decoration;
		uint32 BreakableDecoration;
		uint32 Mover;
		uint32 Trigger;
		uint32 DoorTrigger;
		uint32 Dispatcher;
		uint32 Counter;
		uint32 SoundDispatcher;
		uint32 Portcullis;
		uint32 Teleporter;
		uint32 Jumper;
		uint32 BlockAll;
		uint32 BlockPlayer;
		uint32 BattleInfo;
		uint32 SealAltar;
		uint32 Budget;
		uint32 MashadarGuide;
		uint32 Skips;
		uint32 Unsupported;
	};
	static constexpr FExpectedRetailProjectionCensus ExpectedCensus[] = {
		{TEXT("Arena_01"), 7, 16, 9, 147, 1, 1, 107, 14, 0, 4, 41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0, 0, 0, 0, 0},
		{TEXT("Arena_02"), 15, 19, 5, 48, 1, 1, 170, 10, 5, 10, 10, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 2},
		{TEXT("Arena_03"), 13, 24, 2, 94, 1, 1, 237, 38, 8, 14, 42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{TEXT("Arena_04"), 16, 10, 2, 51, 1, 1, 52, 28, 0, 0, 24, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1},
		{TEXT("Arena_05"), 13, 30, 5, 102, 1, 1, 180, 1, 0, 9, 30, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 18},
		{TEXT("Arena_06"), 9, 29, 6, 98, 1, 1, 206, 20, 8, 11, 32, 8, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{TEXT("Arena_07"), 14, 9, 3, 52, 1, 1, 50, 26, 0, 0, 26, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{TEXT("Arena_08"), 16, 17, 2, 83, 1, 1, 118, 32, 0, 0, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		// Arena_09's eleven TriggerTimed oil sprayers are live through the two
		// SpecialEffectsTrigger event arrays; editor and guarded builds both derive
		// their particle materials from authenticated Sparks11.
		{TEXT("Arena_09"), 21, 25, 7, 121, 1, 1, 180, 57, 2, 4, 34, 0, 5, 5, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
		// Arena_11 CommonAngrealSpawnPool5 authors only a weight-0 Shield. Retail
		// still places it; its GetWeightedSelection totals 0 and the pool destroys
		// itself without spawning (AngrealSpawnPool.uc:24-36), as the port does.
		{TEXT("Arena_11"), 16, 26, 4, 136, 1, 1, 90, 56, 0, 3, 41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{TEXT("Arena_12"), 13, 38, 7, 171, 1, 1, 272, 47, 2, 17, 45, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 18, 0, 0, 0, 0, 0, 0, 0},
		// The source T3D lists HangingLight0 and HangingLightChain0 twice under
		// identical object identities. The native world-v3 registry correctly
		// retains one actor of each, so the authenticated runtime census is 105.
		{TEXT("Battle2_01"), 30, 21, 19, 88, 1, 1, 217, 33, 0, 0, 105, 0, 18, 14, 0, 0, 0, 4, 9, 4, 0, 26, 5, 1, 2, 2, 12, 0, 3},
		{TEXT("Battle2_02"), 20, 31, 31, 164, 1, 1, 738, 70, 0, 0, 69, 0, 51, 17, 0, 0, 0, 4, 31, 4, 0, 33, 0, 1, 2, 2, 32, 0, 8},
		// Wind0 now projects its real ParticleSprayer54 target; it is the production
		// record used by the TriggerTimed replication spec rather than a dead skip.
		{TEXT("Battle2_03"), 33, 30, 24, 206, 1, 1, 599, 48, 2, 8, 35, 0, 52, 12, 0, 0, 0, 5, 41, 6, 0, 21, 1, 1, 2, 2, 0, 0, 10},
		{TEXT("Battle2_04"), 45, 26, 9, 181, 1, 1, 271, 82, 0, 0, 58, 0, 21, 15, 2, 0, 0, 0, 10, 4, 0, 23, 0, 1, 2, 2, 0, 0, 0},
		{TEXT("Battle2_05"), 29, 37, 33, 469, 1, 1, 332, 80, 4, 4, 115, 0, 33, 14, 0, 0, 0, 2, 12, 4, 28, 23, 1, 1, 2, 2, 0, 0, 3},
		{TEXT("Battle3_01"), 38, 23, 19, 129, 1, 1, 279, 63, 0, 0, 134, 0, 30, 21, 24, 0, 0, 6, 15, 6, 0, 29, 10, 1, 3, 3, 13, 0, 3},
		{TEXT("Battle3_02"), 24, 36, 20, 61, 1, 1, 335, 31, 0, 0, 29, 0, 15, 1, 0, 0, 1, 3, 9, 0, 1, 9, 0, 1, 3, 3, 4, 0, 5},
		{TEXT("Battle4_01"), 48, 46, 33, 499, 1, 1, 416, 81, 1, 18, 59, 0, 36, 20, 8, 0, 0, 4, 22, 8, 15, 18, 8, 1, 4, 4, 12, 0, 2},
	};
	struct FExpectedNewRuleCensus
	{
		const TCHAR* Map;
		uint32 PitOpening, Staircase, SpecialEventTrigger, SpecialEvent;
		uint32 SpecialEffectsTrigger, StochasticTrigger, TriggerLight, BlockMonsters;
	};
	static constexpr FExpectedNewRuleCensus ExpectedNewRules[] = {
		{TEXT("Arena_05"),0,0,0,5,0,1,0,44},
		{TEXT("Arena_09"),0,0,0,12,2,0,0,0},
		{TEXT("Battle2_01"),9,3,9,0,0,0,0,0},
		{TEXT("Battle2_02"),10,0,0,0,0,0,1,0},
		{TEXT("Battle2_03"),9,8,0,0,0,0,0,0},
		{TEXT("Battle2_04"),9,0,2,0,0,0,0,0},
		{TEXT("Battle2_05"),12,2,0,0,0,0,0,2},
		{TEXT("Battle3_01"),9,3,9,1,0,0,0,5},
		{TEXT("Battle3_02"),9,2,0,0,0,0,0,0},
		{TEXT("Battle4_01"),12,3,0,1,0,0,0,5},
	};
	struct FExpectedRetailPresentationCensus
	{
		const TCHAR* Map;
		uint32 AmbientSound;
		uint32 DynamicAmbientSound;
		uint32 WaterSoundZone;
		uint32 Corona;
		uint32 LeafMap;
		uint32 ActorRotator;
		uint32 RainCyd;
		// Tools/convert/foglights.py --all -> exports/fog/<Map>.fog.json rows.
		uint32 FogLight;
		// import_map.py place_lightwave: one driver when the atlas animation
		// manifest has any term; its Groups are the sorted waveform keys
		// (Intermediate/Conv/<Map>_lmatlas.json).
		uint32 LightWave;
		uint32 WaveGroupCount;
		uint32 WaveGroups[4][2];
	};
	static constexpr FExpectedRetailPresentationCensus ExpectedPresentation[] = {
		{TEXT("Arena_01"), 19, 0, 1, 49, 1, 0, 16, 0, 0, 0, {}},
		{TEXT("Arena_02"), 0, 0, 0, 9, 1, 0, 0, 0, 0, 0, {}},
		{TEXT("Arena_03"), 39, 0, 0, 38, 1, 0, 0, 0, 0, 0, {}},
		{TEXT("Arena_04"), 2, 0, 0, 26, 1, 0, 0, 0, 0, 0, {}},
		{TEXT("Arena_05"), 16, 0, 0, 21, 1, 0, 0, 7, 0, 0, {}},
		{TEXT("Arena_06"), 39, 1, 0, 20, 1, 0, 0, 0, 1, 0, {}},
		{TEXT("Arena_07"), 28, 4, 0, 26, 1, 0, 0, 1, 0, 0, {}},
		{TEXT("Arena_08"), 3, 0, 2, 30, 1, 0, 0, 0, 0, 0, {}},
		{TEXT("Arena_09"), 64, 0, 0, 40, 1, 0, 0, 0, 0, 0, {}},
		{TEXT("Arena_11"), 59, 0, 0, 41, 1, 12, 0, 0, 0, 0, {}},
		{TEXT("Arena_12"), 34, 0, 2, 44, 1, 0, 0, 0, 1, 4, {{99, 102}, {134, 0}, {83, 0}, {134, 0}}},
		{TEXT("Battle2_01"), 36, 0, 3, 22, 1, 98, 0, 0, 1, 1, {{100, 0}}},
		{TEXT("Battle2_02"), 73, 4, 0, 53, 1, 8, 24, 0, 1, 1, {{150, 0}}},
		{TEXT("Battle2_03"), 19, 0, 0, 57, 1, 12, 0, 0, 1, 0, {}},
		{TEXT("Battle2_04"), 7, 2, 1, 49, 1, 11, 0, 5, 1, 0, {}},
		{TEXT("Battle2_05"), 96, 5, 3, 61, 1, 57, 0, 1, 1, 0, {}},
		{TEXT("Battle3_01"), 53, 0, 1, 47, 1, 102, 0, 0, 1, 1, {{100, 0}}},
		{TEXT("Battle3_02"), 32, 0, 0, 22, 1, 3, 0, 0, 1, 0, {}},
		{TEXT("Battle4_01"), 71, 0, 4, 56, 1, 19, 0, 1, 1, 1, {{100, 0}}},
	};
	// The fifteen extracted fog lights (exports/fog/*.fog.json): location,
	// world volumetric radius 25*(VolumeRadius+1), display color, and VolB.
	struct FExpectedRetailFogLight
	{
		const TCHAR* Map;
		const TCHAR* Light;
		float X, Y, Z;
		float Radius;
		float R, G, B;
		float VolB;
	};
	static constexpr FExpectedRetailFogLight ExpectedFogLights[] = {
		{TEXT("Arena_05"), TEXT("Light44"), -7475.550781f, 5041.806641f, -1108.938477f, 325.0f, 0.10347f, 0.12763f, 0.18198f, 0.25000f},
		{TEXT("Arena_05"), TEXT("Light46"), -11764.214844f, 3795.307861f, -1033.938477f, 325.0f, 0.10347f, 0.12763f, 0.18198f, 0.25000f},
		{TEXT("Arena_05"), TEXT("Light55"), -9301.367188f, 4655.929688f, -1378.938477f, 325.0f, 0.10347f, 0.12763f, 0.18198f, 0.25000f},
		{TEXT("Arena_05"), TEXT("Light56"), -9310.009766f, 5039.326172f, -1108.938477f, 325.0f, 0.10347f, 0.12763f, 0.18198f, 0.25000f},
		{TEXT("Arena_05"), TEXT("Light58"), -7456.170898f, 4672.482422f, -700.938477f, 325.0f, 0.10347f, 0.12763f, 0.18198f, 0.25000f},
		{TEXT("Arena_05"), TEXT("Light59"), -7773.123047f, 4662.419922f, -1405.938477f, 325.0f, 0.10347f, 0.12763f, 0.18198f, 0.25000f},
		{TEXT("Arena_05"), TEXT("Light61"), -9726.159180f, 2677.494873f, -1027.938477f, 325.0f, 0.10347f, 0.12763f, 0.18198f, 0.25000f},
		{TEXT("Arena_07"), TEXT("Light10"), -127.142548f, -514.478577f, -430.265656f, 325.0f, 0.82131f, 0.00000f, 0.00000f, 0.18750f},
		{TEXT("Battle2_04"), TEXT("Light158"), 1390.671143f, 13440.460938f, -5238.861328f, 75.0f, 0.19142f, 0.06753f, 0.04316f, 1.79688f},
		{TEXT("Battle2_04"), TEXT("Light159"), 1168.894409f, 13440.229492f, -5238.861328f, 75.0f, 0.19142f, 0.06753f, 0.04316f, 1.79688f},
		{TEXT("Battle2_04"), TEXT("Light20"), 1805.020630f, 6144.613770f, -3571.883301f, 625.0f, 0.10003f, 0.16985f, 0.16209f, 0.93750f},
		{TEXT("Battle2_04"), TEXT("Light5"), 1292.117798f, 13597.083008f, -5983.887207f, 225.0f, 0.19142f, 0.06753f, 0.04316f, 1.79688f},
		{TEXT("Battle2_04"), TEXT("Light61"), 1280.867920f, 13438.790039f, -5238.861328f, 75.0f, 0.19142f, 0.06753f, 0.04316f, 1.79688f},
		{TEXT("Battle2_05"), TEXT("Light459"), -7066.076172f, 2309.481689f, -2178.299561f, 425.0f, 0.10347f, 0.13487f, 0.17473f, 0.50000f},
		{TEXT("Battle4_01"), TEXT("Light459"), -7066.076172f, 2309.481689f, -2178.299561f, 425.0f, 0.10347f, 0.13487f, 0.17473f, 0.50000f},
	};
	struct FExpectedRetailBudget
	{
		const TCHAR* Map;
		const TCHAR* FirstTroopAdapter;
		uint8 Team;
		int32 ItemCounts[11];
	};
	static constexpr FExpectedRetailBudget ExpectedBudgets[] = {
		{TEXT("Battle2_01"), TEXT("TrollocInventory"), 0,
			{1,3,2,0,1,1,2,6,2,0,2}},
		{TEXT("Battle2_01"), TEXT("MinionInventory"), 1,
			{1,3,2,0,1,1,2,6,2,1,2}},
		{TEXT("Battle2_02"), TEXT("MinionInventory"), 0,
			{2,6,3,1,1,1,8,16,4,0,4}},
		{TEXT("Battle2_02"), TEXT("SoldierInventory"), 1,
			{2,5,2,1,1,1,8,12,4,0,6}},
		{TEXT("Battle2_03"), TEXT("MinionInventory"), 1,
			{1,6,0,0,2,2,5,12,3,1,8}},
		{TEXT("Battle2_03"), TEXT("WarderInventory"), 0,
			{1,6,0,0,2,2,3,12,3,3,8}},
		{TEXT("Battle2_04"), TEXT("WarderInventory"), 0,
			{1,3,2,0,1,1,2,6,2,0,3}},
		{TEXT("Battle2_04"), TEXT("TrollocInventory"), 1,
			{1,3,2,0,1,1,2,6,2,0,3}},
		{TEXT("Battle2_05"), TEXT("SoldierInventory"), 1,
			{1,3,2,1,1,1,2,9,2,0,2}},
		{TEXT("Battle2_05"), TEXT("TrollocInventory"), 0,
			{1,4,3,0,1,1,4,6,2,1,2}},
		{TEXT("Battle3_01"), TEXT("TrollocInventory"), 0,
			{1,3,2,0,1,1,5,6,2,0,2}},
		{TEXT("Battle3_01"), TEXT("MinionInventory"), 1,
			{1,3,2,0,1,1,5,6,2,1,2}},
		{TEXT("Battle3_01"), TEXT("WarderInventory"), 2,
			{1,3,2,0,1,1,5,6,2,0,2}},
		{TEXT("Battle3_02"), TEXT("WarderInventory"), 2,
			{1,3,1,0,0,0,0,12,2,0,1}},
		{TEXT("Battle3_02"), TEXT("TrollocInventory"), 0,
			{1,3,2,0,1,1,6,4,3,2,2}},
		{TEXT("Battle3_02"), TEXT("MinionInventory"), 1,
			{1,3,2,0,1,2,1,10,1,0,2}},
		{TEXT("Battle4_01"), TEXT("TrollocInventory"), 0,
			{1,3,2,0,1,1,5,6,2,0,2}},
		{TEXT("Battle4_01"), TEXT("WarderInventory"), 2,
			{1,3,2,0,1,1,5,6,2,0,2}},
		{TEXT("Battle4_01"), TEXT("MinionInventory"), 1,
			{1,3,2,0,1,1,2,6,2,1,2}},
		{TEXT("Battle4_01"), TEXT("SoldierInventory"), 3,
			{1,3,2,0,1,1,2,6,2,0,3}},
	};
	struct FExpectedAltarRadiusCensus
	{
		const TCHAR* Map;
		uint32 Radius48;
		uint32 Radius64;
		uint32 Radius90;
		uint32 Radius140;
	};
	static constexpr FExpectedAltarRadiusCensus ExpectedAltarRadii[] = {
		{TEXT("Battle2_01"), 0, 0, 1, 1},
		{TEXT("Battle2_02"), 2, 0, 0, 0},
		{TEXT("Battle2_03"), 2, 0, 0, 0},
		{TEXT("Battle2_04"), 2, 0, 0, 0},
		{TEXT("Battle2_05"), 2, 0, 0, 0},
		{TEXT("Battle3_01"), 1, 0, 1, 1},
		{TEXT("Battle3_02"), 0, 2, 1, 0},
		{TEXT("Battle4_01"), 2, 0, 1, 1},
	};
	struct FExpectedCacheFormatCensus
	{
		const TCHAR* Map;
		uint32 Lights;
		uint32 Leaves;
		uint32 LightReferences;
		uint32 LongestLightRun;
		uint32 FrictionNodes;
		uint32 ZeroFrictionNodes;
		uint32 MoverSections;
		uint32 MoverRenderTriangles;
		uint32 MoverCollisionTriangles;
		uint32 StaticLightmappedMovers;
		uint32 MoverLitPolys;
		uint32 MoverUV1OutsidePaddedRect;
		uint32 MoverPolyOwnershipViolations;
		uint32 MoverAnimatedLightPolys;
		uint32 MoverAtlasWidth;
		uint32 MoverAtlasHeight;
	};
	static constexpr FExpectedCacheFormatCensus ExpectedCache[] = {
		{TEXT("Arena_01"), 157,1856,16994,28,57,0,0,0,0, 0,0,0,0,0,256,256},
		{TEXT("Arena_02"), 87,1246,3950,14,0,0,6,12,12, 0,0,0,0,0,256,256},
		{TEXT("Arena_03"), 154,1029,6386,30,0,0,0,0,0, 0,0,0,0,0,256,256},
		{TEXT("Arena_04"), 52,543,2268,23,0,0,78,248,224, 2,116,0,0,0,256,256},
		{TEXT("Arena_05"), 121,2992,13243,17,0,0,0,0,0, 0,0,0,0,0,256,256},
		{TEXT("Arena_06"), 101,927,4659,30,0,0,0,0,0, 0,0,0,0,0,256,256},
		{TEXT("Arena_07"), 55,331,1335,23,0,0,32,56,56, 2,12,0,0,0,256,256},
		{TEXT("Arena_08"), 98,642,5634,21,0,0,0,0,0, 0,0,0,0,0,256,256},
		{TEXT("Arena_09"), 191,1272,15897,44,0,0,124,414,359, 5,182,0,0,0,256,256},
		{TEXT("Arena_11"), 141,1434,7254,20,30,0,0,0,0, 0,0,0,0,0,256,256},
		{TEXT("Arena_12"), 219,2482,18401,38,0,0,0,0,0, 0,0,0,0,0,256,256},
		{TEXT("Battle2_01"), 136,2660,10742,14,54,18,292,920,904, 16,292,0,0,0,256,256},
		{TEXT("Battle2_02"), 249,6955,34405,23,96,0,452,1225,1251, 20,352,0,0,3,256,256},
		{TEXT("Battle2_03"), 253,4490,23582,24,65,29,744,2116,2296, 50,844,0,0,0,256,256},
		{TEXT("Battle2_04"), 210,3073,16657,42,64,12,205,566,1086, 21,278,0,0,0,256,256},
		{TEXT("Battle2_05"), 379,4333,29159,52,76,3,311,813,835, 12,211,0,0,9,256,256},
		{TEXT("Battle3_01"), 200,3760,16898,21,50,14,679,2477,2573, 28,983,0,0,0,256,256},
		{TEXT("Battle3_02"), 90,3821,9962,11,47,11,88,184,220, 15,100,0,0,0,256,256},
		{TEXT("Battle4_01"), 424,6272,46174,44,67,5,642,2275,2371, 23,865,0,0,0,256,256},
	};
	const FString RetailRoot = WoTRuntimeProjectionRepoPath(TEXT("original"));
	const FString EvidenceRoot = WoTRuntimeProjectionEvidenceRoot();
	const FString WorldEvidence = EvidenceRoot / TEXT("worlds");
	const FString DefaultEvidence = EvidenceRoot / TEXT("class-defaults");
	IFileManager::Get().MakeDirectory(*WorldEvidence, true);
	IFileManager::Get().MakeDirectory(*DefaultEvidence, true);
	if (!IFileManager::Get().FileExists(*(RetailRoot / TEXT("Maps/Arena_01.wot"))))
	{
		AddError(TEXT("required original/Maps/Arena_01.wot is missing"));
		return false;
	}
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
	{
		AddError(LoadedContract.GetError().ToString());
		return false;
	}
	const FWoTRetailContract& Production = LoadedContract.GetValue();
	TMap<FString, TUniquePtr<FWoTDevelopmentTexturePackage>> TexturePackages;
	auto FindProductionTexture = [&Production, &RetailRoot, &TexturePackages](
		FStringView Identity, FWoTRetailTextureFacts& Out) -> bool
	{
		const FWoTRequiredFact* TextureFact = Production.Facts.FindByPredicate(
			[Identity](const FWoTRequiredFact& Fact)
			{
				return Fact.Domain == EWoTFactDomain::Texture
					&& FStringView(Fact.SourceExport).Equals(
						Identity, ESearchCase::IgnoreCase);
			});
		if (TextureFact == nullptr) return false;
		FWoTDevelopmentTexturePackage* Package = nullptr;
		if (TUniquePtr<FWoTDevelopmentTexturePackage>* Existing =
			TexturePackages.Find(TextureFact->SourcePath))
		{
			Package = Existing->Get();
		}
		else
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(
				Bytes, *(RetailRoot / TextureFact->SourcePath))) return false;
			TWoTBootstrapResult<TUniquePtr<FWoTDevelopmentTexturePackage>> Parsed =
				FWoTDevelopmentTexturePackage::Parse(
					MoveTemp(Bytes), TextureFact->SourcePath);
			if (Parsed.IsError()) return false;
			Package = Parsed.GetValue().Get();
			TexturePackages.Add(TextureFact->SourcePath, MoveTemp(Parsed).GetValue());
		}
		return Package != nullptr && Package->FindTexture(TextureFact->SourceExport,
			Out.USize, Out.VSize, Out.Format, Out.AnimNext, Out.MinFrameRate,
			Out.MaxFrameRate, Out.PrimeCount);
	};
	FWoTRetailContract WorldContract;
	FWoTRetailContract DefaultsContract;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	TArray<FString> DefaultSourcePaths;
	TArray<FString> DefaultLogicalIds;
	for (const FWoTRequiredFact& Fact : Production.Facts)
	{
		const bool bMultiplayerWorldFact =
			(Fact.Domain == EWoTFactDomain::BSP
				|| Fact.Domain == EWoTFactDomain::World)
			&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"),
				ESearchCase::IgnoreCase);
		if (bMultiplayerWorldFact)
		{
			WorldContract.Facts.Add(Fact);
		}
		if (bMultiplayerWorldFact && Fact.Domain == EWoTFactDomain::World)
		{
			FWoTWorldPackageBytesForTesting& Source =
				WorldSources.AddDefaulted_GetRef();
			Source.SourcePath = Fact.SourcePath;
			if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
				FFileHelper::LoadFileToArray(Source.Bytes,
					*(RetailRoot / Fact.SourcePath)))) return false;
		}
		if (Fact.Domain == EWoTFactDomain::ClassDefault)
		{
			DefaultsContract.Facts.Add(Fact);
			DefaultSourcePaths.Add(Fact.SourcePath);
			if (Fact.Outputs.Num() == 1)
				DefaultLogicalIds.Add(Fact.Outputs[0].LogicalId);
		}
	}
	if (!TestEqual(TEXT("native census has nineteen map packages"),
		WorldSources.Num(), 19)
		|| !TestEqual(TEXT("native census has twenty class-default packages"),
			DefaultSourcePaths.Num(), 20)) return false;

	FWoTRuntimeProjectionCensusWriter Writer;
	FWoTRuntimeProjectionCensusProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorlds =
		ImportRequiredWorldSetPackageBytesForTesting(
			WorldSources, WorldContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds all world-v6 objects"),
		ImportedWorlds.IsError()))
	{
		AddError(ImportedWorlds.GetError().ToString());
		return false;
	}
	TArray<FWoTRemainingSourceBytesForTesting> DefaultSources;
	for (const FString& SourcePath : DefaultSourcePaths)
	{
		FWoTRemainingSourceBytesForTesting& Source =
			DefaultSources.AddDefaulted_GetRef();
		Source.SourcePath = SourcePath;
		if (!TestTrue(SourcePath + TEXT(" loads"),
			FFileHelper::LoadFileToArray(Source.Bytes,
				*(RetailRoot / SourcePath)))) return false;
	}
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> ImportedDefaults =
		ImportRegistryLocalizationDefaultsSourcesForTesting(
			DefaultSources, DefaultsContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds all class-default objects"),
		ImportedDefaults.IsError()))
	{
		AddError(ImportedDefaults.GetError().ToString());
		return false;
	}

	TArray<FWoTRuntimeVerifiedObject> DefaultObjects;
	for (const FString& LogicalId : DefaultLogicalIds)
	{
		TArray<uint8>* Bytes = Writer.Objects.Find(LogicalId);
		if (!TestNotNull(LogicalId + TEXT(" native object"), Bytes)) return false;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*Bytes),
				TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
		if (!TestFalse(LogicalId + TEXT(" frame decodes"), Decoded.IsError()))
		{
			AddError(Decoded.GetError().ToString());
			return false;
		}
		FString EvidenceLeaf = LogicalId.Replace(TEXT("/"), TEXT("_"));
		TestTrue(LogicalId + TEXT(" raw WOTDEF3-v3 evidence writes"),
			WoTRuntimeProjectionSavePayload(Decoded.GetValue().Payload(),
				DefaultEvidence / (EvidenceLeaf + TEXT(".bin"))));
		DefaultObjects.Add(MoveTemp(Decoded).GetValue());
	}
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("all twenty class-default objects load through runtime"),
		Defaults.LoadObjectsForTesting(MoveTemp(DefaultObjects), Error)))
	{
		AddError(Error);
		return false;
	}
	{
		// Guarded builds cook no DT_WoTAngrealHands and fill the angreal hands
		// from the retail WOTPlayer.AngrealHandOrder class default instead; the
		// grid must equal the dev CSV table cell for cell (WOTPlayer.uc:7209+).
		UWoTHandSet* RetailHands = NewObject<UWoTHandSet>();
		RetailHands->Init(RetailHands);
		TestEqual(TEXT("retail AngrealHandOrder fills all 42 shipped cells"),
			RetailHands->PopulateFromRuntimeClassDefaults(Defaults), 42);
		const UDataTable* HandTable = LoadObject<UDataTable>(nullptr,
			TEXT("/Game/WoT/Data/DT_WoTAngrealHands.DT_WoTAngrealHands"));
		if (TestNotNull(TEXT("dev DT_WoTAngrealHands loads"), HandTable))
		{
			UWoTHandSet* TableHands = NewObject<UWoTHandSet>();
			TableHands->Init(TableHands);
			TableHands->PopulateFromTable(HandTable);
			for (int32 HandIndex = 0; HandIndex < UWoTHandSet::NumHands; ++HandIndex)
			{
				for (int32 SlotIndex = 0; SlotIndex < UWoTHandInfo::NumSlots;
					++SlotIndex)
				{
					TestEqual(FString::Printf(
						TEXT("hand %d slot %d retail default equals the CSV table"),
						HandIndex, SlotIndex),
						RetailHands->GetHand(HandIndex)->ClassName[SlotIndex],
						TableHands->GetHand(HandIndex)->ClassName[SlotIndex]);
				}
			}
		}
	}
	{
		// W2: Mission maps are outside the multiplayer cache contract, so drive a
		// synthetic directive through the decoded production class-default graph.
		FString PawnIdentity;
		TestTrue(TEXT("production defaults resolve WOTPawn canonical identity"),
			Defaults.FindClassIdentityByLeaf(TEXT("WOTPawn"), PawnIdentity));
		TArray<FString> PawnDescendants;
		Defaults.GetDerivedClassIdentities(PawnIdentity, PawnDescendants);
		TestTrue(TEXT("production WOTPawn ancestry contains Trolloc"),
			PawnDescendants.ContainsByPredicate([](const FString& Identity)
			{
				return Identity.EndsWith(TEXT(".Trolloc"), ESearchCase::IgnoreCase);
			}));
		UWorld* SelectorWorld = WoTTest::CreateSpecWorld();
		AWoTDestroyDirective* Directive =
			SelectorWorld->SpawnActor<AWoTDestroyDirective>();
		AWoTNPCCharacter* Trolloc =
			SelectorWorld->SpawnActor<AWoTNPCCharacter>();
		Directive->ObserverClasses = {FName(*PawnIdentity)};
		Trolloc->CreatureRowName = TEXT("Trolloc");
		FString SelectorError;
		TestTrue(TEXT("synthetic directive configures production selector"),
			Directive->ConfigureObserverClasses(Defaults, SelectorError));
		Directive->WoTTrigger(nullptr, nullptr);
		TestTrue(TEXT("retained production ancestry destroys its retail descendant"),
			!IsValid(Trolloc) || Trolloc->IsActorBeingDestroyed());
		WoTTest::DestroySpecWorld(SelectorWorld);
	}

	int32 CensusIndex = 0;
	uint32 TotalMoverModels = 0;
	uint32 TotalMoverSections = 0;
	uint32 TotalMoverTriangles = 0;
	uint32 TotalLightRows = 0;
	uint32 TotalLightReferences = 0;
	uint32 LongestLightRun = 0;
	uint32 TotalProjectedMovers = 0;
	uint32 TotalProjectedTriggers = 0;
	uint32 TotalProjectedDoorTriggers = 0;
	uint32 TotalProjectedSoundDispatchers = 0;
	uint32 TotalProjectedPortcullises = 0;
	uint32 TotalProjectedTeleporters = 0;
	uint32 TotalProjectedJumpers = 0;
	uint32 TotalProjectedBlockAll = 0;
	uint32 TotalProjectedBlockPlayer = 0;
	uint32 TotalProjectedPitOpenings = 0;
	uint32 TotalProjectedStaircases = 0;
	uint32 TotalProjectedSpecialEventTriggers = 0;
	uint32 TotalProjectedSpecialEvents = 0;
	uint32 TotalProjectedSpecialEffectsTriggers = 0;
	uint32 TotalProjectedStochasticTriggers = 0;
	uint32 TotalProjectedTriggerLights = 0;
	uint32 TotalProjectedBlockMonsters = 0;
	uint32 TotalProjectedBattleInfo = 0;
	uint32 TotalProjectedSealAltars = 0;
	uint32 TotalProjectedBudgets = 0;
	uint32 TotalProjectedMashadarGuides = 0;
	uint32 TotalProjectedTaggedMarkers = 0;
	uint32 TotalProjectedAmbientSounds = 0;
	uint32 TotalProjectedDynamicAmbientSounds = 0;
	uint32 TotalProjectedWaterSoundZones = 0;
	uint32 TotalProjectedCoronas = 0;
	uint32 TotalProjectedLeafMaps = 0;
	uint32 TotalProjectedActorRotators = 0;
	uint32 TotalProjectedRainCyd = 0;
	uint32 TotalProjectedFogLights = 0;
	uint32 MatchedExpectedFogLights = 0;
	uint32 TotalProjectedLightWaves = 0;
	uint32 TotalProjectedLightning = 0;
	uint32 TotalProjectedRandomLightning = 0;
	uint32 TotalProjectedDistanceLightning = 0;
	uint32 TotalProjectedWind = 0;
	uint32 TotalProjectedSpewTriggers = 0;
	uint32 ProductionTriggeredMPWind = 0;
	uint32 ProductionRandomLightningRelays = 0;
	uint32 ProductionTaggedLightning = 0;
	uint32 ProductionLightningTargetLights = 0;
	uint32 TotalDynamicMovers = 0;
	uint32 TotalStaticMovers = 0;
	uint32 TotalBakedMacroMoverSections = 0;
	uint32 TotalMoverLitPolys = 0;
	TSet<FString> ProductionMoverBindingWitnesses;
	uint32 TotalMoverCollisionTriangles = 0;
	uint32 TotalMoverUV1OutsidePaddedRect = 0;
	uint32 TotalMoverPolyOwnershipViolations = 0;
	uint32 TotalMoverAnimatedLightPolys = 0;
	uint32 TotalBattle204PortcullisBounds = 0;
	uint32 TotalBattle201NotSolidMovers = 0;
	uint32 TotalBattle204EmptyCollisionPortcullises = 0;
	uint32 ProductionRotationSweeps = 0;
	uint32 ProductionDrivenDecorationTicks = 0;
	uint32 ProductionPushableBarrels = 0;
	uint32 ProductionRetailGravityZones = 0;
	uint32 ProductionRetailTriggerCylinders = 0;
	uint32 ProductionExactPlayerStarts = 0;
	uint32 ProductionKeyNumRestingMovers = 0;
	uint32 ProductionDefaultPlayerTriggers = 0;
	uint32 ProductionDefaultPlayerBumpMovers = 0;
	uint32 ProductionTruncatedBattle4Pools = 0;
	uint32 ProductionSilentLitAmbients = 0;
	uint32 ProductionNonBlockingCarcasses = 0;
	uint32 ProductionHiddenMovers = 0;
	uint32 ProductionProjectileTouches = 0;
	uint32 ProductionForsakenReplacements = 0;
	uint32 ProductionNotSolidMoverTraceWitnesses = 0;
	uint32 TotalProjectedModulatedSprayers = 0;
	TMap<FName, uint32> TotalInertClasses;
	TMap<FName, uint32> TotalUnsupportedClasses;
	TSet<int32> ConsumedBudgetRows;
	TSet<FString> ExpectedBudgetKeys;
	struct FExpectedTruncatedPool
	{
		const TCHAR* Identity;
		const TCHAR* Entries;
	};
	static constexpr FExpectedTruncatedPool ExpectedTruncatedPools[] = {
		{TEXT("Battle4_01.UncommonAngrealSpawnPool0"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool1"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3,AngrealInvIce:1,AngrealInvSeeker:4,AngrealInvLightning:4,AngrealInvSwapPlaces:2,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:2")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool2"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3,AngrealInvIce:1,AngrealInvSeeker:4,AngrealInvLightning:4,AngrealInvSwapPlaces:8,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:2")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool3"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3,AngrealInvIce:1,AngrealInvSeeker:4,AngrealInvLightning:4,AngrealInvSwapPlaces:2,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:2")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool4"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3,AngrealInvTracer:2,AngrealInvSeeker:7,AngrealInvLightning:4,AngrealInvSwapPlaces:1,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool5"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3,AngrealInvTracer:2,AngrealInvSeeker:7,AngrealInvLightning:4,AngrealInvSwapPlaces:1,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool6"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool7"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool8"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3,AngrealInvIce:1,AngrealInvSeeker:4,AngrealInvLightning:4,AngrealInvSwapPlaces:2,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:2,AngrealInvMinion:1")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool9"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3,AngrealInvIce:1,AngrealInvSeeker:4,AngrealInvLightning:4,AngrealInvSwapPlaces:2,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:2")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool10"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool12"), TEXT("AngrealInvAMA:1,AngrealInvShift:3,AngrealInvEarthTremor:0,AngrealInvIce:2,AngrealInvSeeker:0,AngrealInvLightning:4,AngrealInvSwapPlaces:6,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:1")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool13"), TEXT("AngrealInvAMA:2,AngrealInvShift:5,AngrealInvEarthTremor:3,AngrealInvIce:2,AngrealInvSeeker:0,AngrealInvLightning:4,AngrealInvSwapPlaces:2,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool14"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool15"), TEXT("AngrealInvAMA:1,AngrealInvShift:1,AngrealInvEarthTremor:3")},
		{TEXT("Battle4_01.UncommonAngrealSpawnPool19"), TEXT("AngrealInvAMA:1,AngrealInvShift:4,AngrealInvEarthTremor:3,AngrealInvIce:1,AngrealInvSeeker:4,AngrealInvLightning:4,AngrealInvSwapPlaces:2,AngrealInvWallOfAir:2,AngrealInvLevitate:1,AngrealInvWhirlwind:2")},
	};
	for (int32 BudgetIndex = 0;
		BudgetIndex < static_cast<int32>(UE_ARRAY_COUNT(ExpectedBudgets));
		++BudgetIndex)
	{
		const FExpectedRetailBudget& ExpectedBudget = ExpectedBudgets[BudgetIndex];
		const FString Key = FString::Printf(TEXT("%s|%s|%u"),
			ExpectedBudget.Map, ExpectedBudget.FirstTroopAdapter,
			static_cast<uint32>(ExpectedBudget.Team));
		TestFalse(FString::Printf(TEXT("expected budget key %s is unique"), *Key),
			ExpectedBudgetKeys.Contains(Key));
		ExpectedBudgetKeys.Add(Key);
	}
	auto ExpectedBattleTeamClass = [](FStringView Map, int32 Team) -> UClass*
	{
		if (Map.Equals(TEXT("Battle2_01"), ESearchCase::IgnoreCase))
			return Team == 0 ? AWoTForsaken::StaticClass()
				: Team == 1 ? AWoTHound::StaticClass() : nullptr;
		if (Map.Equals(TEXT("Battle2_02"), ESearchCase::IgnoreCase))
			return Team == 0 ? AWoTHound::StaticClass()
				: Team == 1 ? AWoTWhitecloak::StaticClass() : nullptr;
		if (Map.Equals(TEXT("Battle2_03"), ESearchCase::IgnoreCase))
			return Team == 0 ? AWoTAesSedai::StaticClass()
				: Team == 1 ? AWoTHound::StaticClass() : nullptr;
		if (Map.Equals(TEXT("Battle2_04"), ESearchCase::IgnoreCase))
			return Team == 0 ? AWoTAesSedai::StaticClass()
				: Team == 1 ? AWoTForsaken::StaticClass() : nullptr;
		if (Map.Equals(TEXT("Battle2_05"), ESearchCase::IgnoreCase))
			return Team == 0 ? AWoTForsaken::StaticClass()
				: Team == 1 ? AWoTWhitecloak::StaticClass() : nullptr;
		if (Map.Equals(TEXT("Battle3_01"), ESearchCase::IgnoreCase)
			|| Map.Equals(TEXT("Battle3_02"), ESearchCase::IgnoreCase))
		{
			return Team == 0 ? AWoTForsaken::StaticClass()
				: Team == 1 ? AWoTHound::StaticClass()
				: Team == 2 ? AWoTAesSedai::StaticClass() : nullptr;
		}
		if (Map.Equals(TEXT("Battle4_01"), ESearchCase::IgnoreCase))
		{
			return Team == 0 ? AWoTForsaken::StaticClass()
				: Team == 1 ? AWoTHound::StaticClass()
				: Team == 2 ? AWoTAesSedai::StaticClass()
				: Team == 3 ? AWoTWhitecloak::StaticClass() : nullptr;
		}
		return nullptr;
	};
	auto ExpectedBudgetAdapter = [](FName FirstTroopAdapter, int32 Slot) -> FName
	{
		if (Slot == 0) return FName(TEXT("SealInventory"));
		if (Slot == 1) return FirstTroopAdapter;
		if (Slot == 2)
		{
			if (FirstTroopAdapter == FName(TEXT("WarderInventory"))) return FName(TEXT("SisterInventory"));
			if (FirstTroopAdapter == FName(TEXT("TrollocInventory"))) return FName(TEXT("MyrddraalInventory"));
			if (FirstTroopAdapter == FName(TEXT("MinionInventory"))) return FName(TEXT("MashadarInventory"));
			if (FirstTroopAdapter == FName(TEXT("SoldierInventory"))) return FName(TEXT("ArcherInventory"));
		}
		if (Slot == 3)
		{
			if (FirstTroopAdapter == FName(TEXT("WarderInventory"))) return FName(TEXT("SitterInventory"));
			if (FirstTroopAdapter == FName(TEXT("TrollocInventory"))) return FName(TEXT("BATrollocInventory"));
			if (FirstTroopAdapter == FName(TEXT("MinionInventory"))) return FName(TEXT("LegionInventory"));
			if (FirstTroopAdapter == FName(TEXT("SoldierInventory"))) return FName(TEXT("QuestionerInventory"));
		}
		static const FName Common[] = {
			NAME_None, NAME_None, NAME_None, NAME_None,
			FName(TEXT("AlarmInventory")), FName(TEXT("FireWallInventory")),
			FName(TEXT("WallInventory")), FName(TEXT("SpearInventory")),
			FName(TEXT("PitInventory")), FName(TEXT("StaircaseInventory")),
			FName(TEXT("PortcullisInventory"))};
		return Common[Slot];
	};
	for (const FWoTWorldDescriptor& Descriptor : ImportedWorlds.GetValue().Worlds)
	{
		TArray<uint8>* Bytes = Writer.Objects.Find(Descriptor.WorldLogicalId);
		if (!TestNotNull(Descriptor.MapIdentity + TEXT(" world object"), Bytes))
			continue;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*Bytes),
				TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" frame decodes"),
			Decoded.IsError())) continue;
		UE_LOG(LogTemp, Display,
			TEXT("MOVER_GEOMETRY_CENSUS map=%s static=%u lit=%u render=%u "
				"collision=%u uv1_outside=%u ownership=%u animated=%u atlas=%ux%u"),
			*Descriptor.MapIdentity, Descriptor.StaticLightmappedMoverCount,
			Descriptor.MoverLitPolyCount, Descriptor.MoverRenderTriangleCount,
			Descriptor.MoverCollisionTriangleCount,
			Descriptor.MoverUV1OutsidePaddedRectCount,
			Descriptor.MoverPolyOwnershipViolationCount,
			Descriptor.MoverAnimatedLightPolyCount,
			Descriptor.MoverAtlasWidth, Descriptor.MoverAtlasHeight);
		if (Descriptor.MapIdentity == TEXT("Battle2_02"))
		{
			// Python oracle order: poly 2 then 3; within each poly (u,v)
			// (0,0),(5,0),(4,1),(5,1),(9,2), with RGB7 channel order.
			const TArray<uint8> ExpectedOracle = {
				0,0,0, 5,4,5, 21,18,18, 21,18,18, 4,4,4,
				4,5,6, 20,20,23, 16,15,18, 16,15,17, 0,1,1};
			TestEqual(TEXT("Battle2_02 Mover6 native texels equal Python oracle"),
				Descriptor.MoverOracleRGB7, ExpectedOracle);
		}
		else
		{
			TestTrue(Descriptor.MapIdentity + TEXT(" has no unrelated oracle witness"),
				Descriptor.MoverOracleRGB7.IsEmpty());
		}
		TestTrue(Descriptor.MapIdentity + TEXT(" raw world-v6 evidence writes"),
			WoTRuntimeProjectionSavePayload(Decoded.GetValue().Payload(),
				WorldEvidence / (Descriptor.MapIdentity + TEXT(".world-v6.bin"))));
		FWoTRuntimeWorldResource Resource;
		FWoTBootstrapStatus Loaded =
			Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue());
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" runtime world decodes"),
			Loaded.IsError()))
		{
			AddError(Loaded.GetError().ToString());
			continue;
		}
		TestTrue(Descriptor.MapIdentity + TEXT(" retains BSP leaves"),
			!Resource.Leaves().IsEmpty());
		TestTrue(Descriptor.MapIdentity + TEXT(" retains Model.Lights"),
			!Resource.ModelLights().IsEmpty());
		TestEqual(Descriptor.MapIdentity + TEXT(" mover-model census"),
			Resource.MoverModels().Num(), static_cast<int32>(Descriptor.MoverCount));
		if (CensusIndex < static_cast<int32>(UE_ARRAY_COUNT(ExpectedCache)))
		{
			const FExpectedCacheFormatCensus& Expected = ExpectedCache[CensusIndex];
			TestEqual(Descriptor.MapIdentity + TEXT(" cache census order"),
				Descriptor.MapIdentity, FString(Expected.Map));
			TestEqual(Descriptor.MapIdentity + TEXT(" leaf census"),
				Resource.Leaves().Num(), static_cast<int32>(Expected.Leaves));
			TestFalse(Descriptor.MapIdentity + TEXT(" root-outside parity"),
				Resource.BSPRootOutside());
			uint32 LightReferences = 0;
			uint32 MapLongestLightRun = 0;
			for (const FWoTRuntimeWorldLeaf& Leaf : Resource.Leaves())
			{
				if (Leaf.Permeating < 0) continue;
				uint32 Run = 0;
				for (int32 Light = Leaf.Permeating;
					Resource.ModelLights().IsValidIndex(Light)
						&& Resource.ModelLights()[Light] != MAX_uint32; ++Light) ++Run;
				LightReferences += Run;
				MapLongestLightRun = FMath::Max(MapLongestLightRun, Run);
			}
			TestEqual(Descriptor.MapIdentity + TEXT(" leaf-light reference census"),
				LightReferences, Expected.LightReferences);
			TestEqual(Descriptor.MapIdentity + TEXT(" longest leaf-light run"),
				MapLongestLightRun, Expected.LongestLightRun);
			uint32 FrictionNodes = 0;
			for (const FWoTRuntimeWorldNode& Node : Resource.Nodes())
				if (Resource.Surfaces().IsValidIndex(Node.Surface)
					&& Resource.Surfaces()[Node.Surface].Friction != 1.0f)
					++FrictionNodes;
			TestEqual(Descriptor.MapIdentity + TEXT(" non-default friction nodes"),
				FrictionNodes, Expected.FrictionNodes);
			uint32 MoverSections = 0;
			uint32 MoverRenderTriangles = 0;
			uint32 MoverCollisionTriangles = 0;
			for (const FWoTRuntimeMoverModel& Mover : Resource.MoverModels())
			{
				MoverSections += Mover.Sections.Num();
				MoverRenderTriangles += Mover.Indices.Num() / 3;
				MoverCollisionTriangles += Mover.CollisionIndices.Num() / 3;
				if (Mover.Positions.IsEmpty())
				{
					AddError(Descriptor.MapIdentity + TEXT(" mover render geometry is empty"));
					continue;
				}
				const bool bHasNotSolidRenderSection = Mover.Sections.ContainsByPredicate(
					[](const FWoTRuntimeMoverSection& Section)
					{
						return (Section.SurfaceFlags & 0x00000008u) != 0;
					});
				if (Mover.CollisionPositions.IsEmpty())
				{
					TestTrue(Mover.ActorIdentity + TEXT(" empty collision has empty indices"),
						Mover.CollisionIndices.IsEmpty());
					if (Mover.ActorIdentity == TEXT("Battle2_01.Mover9"))
					{
						for (const FWoTRuntimeMoverSection& Section : Mover.Sections)
						{
							TestTrue(Mover.ActorIdentity + TEXT(" render section is PF_NotSolid"),
								(Section.SurfaceFlags & 0x00000008u) != 0);
						}
						++TotalBattle201NotSolidMovers;
					}
					if (Mover.ActorIdentity.StartsWith(
						TEXT("Battle2_04.PortcullisMover"), ESearchCase::CaseSensitive))
					{
						const int32 PortcullisIndex = FCString::Atoi(
							*Mover.ActorIdentity.Mid(FCString::Strlen(
								TEXT("Battle2_04.PortcullisMover"))));
						TestTrue(Mover.ActorIdentity + TEXT(" is an exact empty-collision portcullis"),
							PortcullisIndex >= 3 && PortcullisIndex < 10);
						++TotalBattle204EmptyCollisionPortcullises;
					}
					continue;
				}
				FVector3f RenderMin = Mover.Positions[0];
				FVector3f RenderMax = Mover.Positions[0];
				for (const FVector3f Position : Mover.Positions)
				{
					RenderMin = RenderMin.ComponentMin(Position);
					RenderMax = RenderMax.ComponentMax(Position);
				}
				FVector3f CollisionMin = Mover.CollisionPositions[0];
				FVector3f CollisionMax = Mover.CollisionPositions[0];
				for (const FVector3f Position : Mover.CollisionPositions)
				{
					CollisionMin = CollisionMin.ComponentMin(Position);
					CollisionMax = CollisionMax.ComponentMax(Position);
				}
				const bool bBattle204Portcullis =
					Descriptor.MapIdentity == TEXT("Battle2_04")
					&& Mover.ActorIdentity.Contains(
						TEXT(".PortcullisMover"), ESearchCase::CaseSensitive);
				if (bBattle204Portcullis)
				{
					++TotalBattle204PortcullisBounds;
					static const FVector3f ExpectedRenderMin[] = {
						{-100.0f,-0.5f,-96.0f}, {-128.0f,-0.5f,-96.0f},
						{-128.0f,-0.5f,-96.0f}, {-0.5f,-128.0f,-128.0f},
						{-0.5f,-128.0f,-128.0f}, {-128.0f,-0.5f,-96.0f},
						{-128.0f,-0.5f,-96.0f}, {-0.5f,-128.0f,-128.0f},
						{-0.5f,-128.0f,-128.0f}, {-0.5f,-128.0f,-128.0f}};
					static const FVector3f ExpectedRenderMax[] = {
						{100.0f,0.5f,96.0f}, {128.0f,0.5f,96.0f},
						{128.0f,0.5f,96.0f}, {0.5f,128.0f,128.0f},
						{0.5f,128.0f,128.0f}, {128.0f,0.5f,96.0f},
						{128.0f,0.5f,96.0f}, {0.5f,128.0f,128.0f},
						{0.5f,128.0f,128.0f}, {0.5f,128.0f,128.0f}};
					// Portcullises 3-9 collide only through their Invisible|
					// Semisolid slab (the bars are Masked|NotSolid); measured
					// once invisible solid mover polys collide.
					static const FVector3f ExpectedCollisionMin[] = {
						{-92.0f,-4.0f,-84.0f}, {-92.0f,-4.0f,-84.0f},
						{-92.0f,-4.0f,-84.0f}, {-6.0f,-64.0f,-96.0f},
						{-6.0f,-64.0f,-96.0f}, {-80.0f,-6.0f,-88.0f},
						{-80.0f,-6.0f,-88.0f}, {-6.0f,-64.0f,-96.0f},
						{-6.0f,-64.0f,-96.0f}, {-6.0f,-64.0f,-96.0f}};
					static const FVector3f ExpectedCollisionMax[] = {
						{100.0f,4.0f,64.0f}, {100.0f,4.0f,64.0f},
						{100.0f,4.0f,64.0f}, {6.0f,64.0f,96.0f},
						{6.0f,64.0f,96.0f}, {80.0f,6.0f,88.0f},
						{80.0f,6.0f,88.0f}, {6.0f,64.0f,96.0f},
						{6.0f,64.0f,96.0f}, {6.0f,64.0f,96.0f}};
					const int32 NameOffset = Mover.ActorIdentity.Find(
						TEXT(".PortcullisMover"), ESearchCase::CaseSensitive);
					const int32 PortcullisIndex = NameOffset == INDEX_NONE ? INDEX_NONE
						: FCString::Atoi(*Mover.ActorIdentity.Mid(
							NameOffset + FCString::Strlen(TEXT(".PortcullisMover"))));
					if (TestTrue(Mover.ActorIdentity + TEXT(" has a pinned bounds row"),
						PortcullisIndex >= 0 && PortcullisIndex < 10))
					{
						TestTrue(Mover.ActorIdentity + TEXT(" FPoly render minimum"),
							(RenderMin - ExpectedRenderMin[PortcullisIndex])
								.GetAbsMax() <= 0.05f);
						TestTrue(Mover.ActorIdentity + TEXT(" FPoly render maximum"),
							(RenderMax - ExpectedRenderMax[PortcullisIndex])
								.GetAbsMax() <= 0.05f);
						TestTrue(Mover.ActorIdentity + TEXT(" BSP collision minimum"),
							(CollisionMin - ExpectedCollisionMin[PortcullisIndex])
								.GetAbsMax() <= 0.05f);
						TestTrue(Mover.ActorIdentity + TEXT(" BSP collision maximum"),
							(CollisionMax - ExpectedCollisionMax[PortcullisIndex])
								.GetAbsMax() <= 0.05f);
					}
					UE_LOG(LogTemp, Display,
						TEXT("MOVER_PORTCULLIS_BOUNDS actor=%s "
							"render_min=(%.6f,%.6f,%.6f) render_max=(%.6f,%.6f,%.6f) "
							"collision_min=(%.6f,%.6f,%.6f) collision_max=(%.6f,%.6f,%.6f)"),
						*Mover.ActorIdentity,
						RenderMin.X, RenderMin.Y, RenderMin.Z,
						RenderMax.X, RenderMax.Y, RenderMax.Z,
						CollisionMin.X, CollisionMin.Y, CollisionMin.Z,
						CollisionMax.X, CollisionMax.Y, CollisionMax.Z);
				}
				else if (!bHasNotSolidRenderSection)
				{
					const FString BoundsPrefix = Descriptor.MapIdentity
						+ TEXT(" ") + Mover.ActorIdentity;
					TestTrue(BoundsPrefix + TEXT(" render/collision minima agree"),
						(RenderMin - CollisionMin).GetAbsMax() <= 0.05f);
					TestTrue(BoundsPrefix + TEXT(" render/collision maxima agree"),
						(RenderMax - CollisionMax).GetAbsMax() <= 0.05f);
				}
			}
			UE_LOG(LogTemp, Display,
				TEXT("MOVER_RUNTIME_GEOMETRY map=%s sections=%u render=%u collision=%u"),
				*Descriptor.MapIdentity, MoverSections,
				MoverRenderTriangles, MoverCollisionTriangles);
			TestEqual(Descriptor.MapIdentity + TEXT(" mover-section census"),
				MoverSections, Expected.MoverSections);
			TestEqual(Descriptor.MapIdentity + TEXT(" mover render-triangle census"),
				MoverRenderTriangles, Expected.MoverRenderTriangles);
			TestEqual(Descriptor.MapIdentity + TEXT(" mover collision-triangle census"),
				MoverCollisionTriangles, Expected.MoverCollisionTriangles);
			TestEqual(Descriptor.MapIdentity + TEXT(" descriptor render-triangle census"),
				Descriptor.MoverRenderTriangleCount, Expected.MoverRenderTriangles);
			TestEqual(Descriptor.MapIdentity + TEXT(" descriptor collision-triangle census"),
				Descriptor.MoverCollisionTriangleCount,
				Expected.MoverCollisionTriangles);
			TestEqual(Descriptor.MapIdentity + TEXT(" static lightmapped mover census"),
				Descriptor.StaticLightmappedMoverCount,
				Expected.StaticLightmappedMovers);
			TestEqual(Descriptor.MapIdentity + TEXT(" mover lit-poly census"),
				Descriptor.MoverLitPolyCount, Expected.MoverLitPolys);
			TestEqual(Descriptor.MapIdentity
				+ TEXT(" mover UV1 padded-rectangle violations"),
				Descriptor.MoverUV1OutsidePaddedRectCount,
				Expected.MoverUV1OutsidePaddedRect);
			TestEqual(Descriptor.MapIdentity
				+ TEXT(" mover direct-FPoly ownership violations"),
				Descriptor.MoverPolyOwnershipViolationCount,
				Expected.MoverPolyOwnershipViolations);
			TestEqual(Descriptor.MapIdentity + TEXT(" animated mover-poly census"),
				Descriptor.MoverAnimatedLightPolyCount,
				Expected.MoverAnimatedLightPolys);
			TestEqual(Descriptor.MapIdentity + TEXT(" mover-atlas width"),
				Resource.MoverAtlasWidth(), Expected.MoverAtlasWidth);
			TestEqual(Descriptor.MapIdentity + TEXT(" mover-atlas height"),
				Resource.MoverAtlasHeight(), Expected.MoverAtlasHeight);
			TestEqual(Descriptor.MapIdentity + TEXT(" mover-atlas byte count"),
				Resource.MoverAtlasBGRA().Num(),
				static_cast<int32>(Expected.MoverAtlasWidth
					* Expected.MoverAtlasHeight * 4));
			uint32 StaticLightmappedModels = 0;
			for (const FWoTRuntimeMoverModel& Mover : Resource.MoverModels())
			{
				StaticLightmappedModels += Mover.bLightmapped ? 1u : 0u;
				TestEqual(Descriptor.MapIdentity + TEXT(" mover UV1 vertex count"),
					Mover.UV1.Num(), Mover.UV0.Num());
				for (const FVector2f UV : Mover.UV1)
					TestTrue(Descriptor.MapIdentity + TEXT(" mover UV1 is finite and normalized"),
						FMath::IsFinite(UV.X) && FMath::IsFinite(UV.Y)
							&& UV.X >= 0.0f && UV.X <= 1.0f
							&& UV.Y >= 0.0f && UV.Y <= 1.0f);
			}
			TestEqual(Descriptor.MapIdentity + TEXT(" runtime lightmapped mover census"),
				StaticLightmappedModels, Expected.StaticLightmappedMovers);
			// The materializer's full census must count movers exactly as the
			// cache builder does, or the world is rejected at host time (every
			// Battle map was, from 3a-1 until this spec).
			TestEqual(Descriptor.MapIdentity
				+ TEXT(" materializer full-census mover count"),
				WoTWorldCensusMoverCount(Resource), Descriptor.MoverCount);
			TotalMoverModels += Resource.MoverModels().Num();
			TotalMoverSections += MoverSections;
			TotalMoverTriangles += MoverRenderTriangles;
			TotalMoverCollisionTriangles += MoverCollisionTriangles;
			TotalLightReferences += LightReferences;
			LongestLightRun = FMath::Max(LongestLightRun, MapLongestLightRun);
			TotalMoverLitPolys += Descriptor.MoverLitPolyCount;
			TotalMoverUV1OutsidePaddedRect +=
				Descriptor.MoverUV1OutsidePaddedRectCount;
			TotalMoverPolyOwnershipViolations +=
				Descriptor.MoverPolyOwnershipViolationCount;
			TotalMoverAnimatedLightPolys += Descriptor.MoverAnimatedLightPolyCount;
		}
		FWoTWorldProjectionSource Source;
		Source.MapIdentity = Resource.MapIdentity();
		Source.Records = Resource.Actors();
		Source.Points = Resource.Points();
		Source.Nodes = Resource.Nodes();
		Source.Vertices = Resource.Vertices();
		Source.Zones = Resource.Zones();
		Source.ReachSpecs = Resource.ReachSpecs();
		Source.Surfaces = Resource.Surfaces();
		Source.Leaves = Resource.Leaves();
		Source.ModelLights = Resource.ModelLights();
		Source.MoverModels = Resource.MoverModels();
		Source.bHasDedicatedMoverCollision = Resource.FormatVersion() >= 4;
		Source.MoverAnimationGroups = Resource.MoverAnimationGroups();
		Source.MoverAnimationTerms = Resource.MoverAnimationTerms();
		Source.MoverLightmap = WoTRuntimeBSPMaterializeLightmap(
			Resource.MoverAtlasWidth(), Resource.MoverAtlasHeight(),
			Resource.MoverAtlasBGRA());
		TestNotNull(Descriptor.MapIdentity + TEXT(" mover atlas materializes"),
			Source.MoverLightmap);
		TArray<UTexture2D*> MoverResponseTextures;
		for (const FWoTRuntimeMoverAnimationTerm& Term :
			Resource.MoverAnimationTerms())
		{
			MoverResponseTextures.Add(WoTRuntimeBSPMaterializeLightmap(
				Resource.MoverAtlasWidth(), Resource.MoverAtlasHeight(),
				Term.ResponseBGRA));
			TestNotNull(Descriptor.MapIdentity
				+ TEXT(" mover response materializes"), MoverResponseTextures.Last());
		}
		Source.MoverLightResponses = MoverResponseTextures;
		if (!MoverResponseTextures.IsEmpty())
		{
			Source.MoverBlackLightResponse =
				WoTRuntimeBSPMaterializeSolidBlackTexture();
			TestNotNull(Descriptor.MapIdentity
				+ TEXT(" mover black response materializes"),
				Source.MoverBlackLightResponse);
		}
		Source.SemanticObjects = Resource.SemanticObjects();
		Source.bBSPRootOutside = Resource.BSPRootOutside();
		if (Descriptor.MapIdentity == TEXT("Battle2_03"))
			Source.FindTexture = FindProductionTexture;
		// Keep the parallel sprayer lane's inputs unchanged while decoding the
		// dimensions and AnimNext series required by DT_Sprite decorations.
		if (Descriptor.MapIdentity == TEXT("Battle2_01")
			|| Descriptor.MapIdentity == TEXT("Battle3_01"))
			Source.FindDecorationTexture = FindProductionTexture;
		// An activated world that has NOT initialized its actors for play:
		// projected replicated actors must stay pre-network startup actors (as
		// in a real host's MaterializeBeforePlay), while the published test
		// activation lets the Battle-map EditZoneAt check below register zones.
		WoTTest::FSpecWorldOptions CensusWorldOptions;
		CensusWorldOptions.bStartPlay = false;
		UWorld* World = WoTTest::CreateSpecWorld(CensusWorldOptions);
#if WITH_EDITOR
		TFunction<void()> RunAnimationNetFixture;
#endif
		FWoTWorldProjectionResult Projection;
		uint32 ProjectedAltarRadius48 = 0;
		uint32 ProjectedAltarRadius64 = 0;
		uint32 ProjectedAltarRadius90 = 0;
		uint32 ProjectedAltarRadius140 = 0;
		uint32 ProjectedAndilayRootActors = 0;
		bool bEditZoneAtCovered = false;
		Error.Reset();
		const bool bProjected = FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error);
		TestTrue(Descriptor.MapIdentity + TEXT(" projects without world rejection"),
			bProjected);
		if (!bProjected) AddError(Descriptor.MapIdentity + TEXT(": ") + Error);
		if (bProjected)
		{
			auto FindRecordIndex = [&Source](FStringView Identity) -> int32
			{
				for (int32 Index = 0; Index < Source.Records.Num(); ++Index)
					if (FStringView(Source.Records[Index].Identity).Equals(
						Identity, ESearchCase::IgnoreCase)) return Index;
				return INDEX_NONE;
			};
			auto FindProjected = [&Projection](int32 RecordIndex) -> AActor*
			{
				const FString Prefix = FString::Printf(TEXT("WoTProjected_%d_"),
					RecordIndex);
				for (AActor* Actor : Projection.DeferredActors)
					if (Actor && Actor->GetName().StartsWith(Prefix)) return Actor;
				return nullptr;
			};
			auto FinishProjected = [World](AActor* Actor)
			{
				if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
				World->UpdateWorldComponents(false, false);
			};
			auto FindProjectedOfClass = [&Projection](int32 RecordIndex,
				UClass* NativeClass) -> AActor*
			{
				const FString Prefix = FString::Printf(TEXT("WoTProjected_%d_"),
					RecordIndex);
				for (AActor* Actor : Projection.DeferredActors)
					if (Actor && Actor->GetName().StartsWith(Prefix)
						&& Actor->IsA(NativeClass)) return Actor;
				return nullptr;
			};
			if (Descriptor.MapIdentity == TEXT("Arena_09"))
			{
				const int32 BoilerRecord = FindRecordIndex(TEXT("Arena_09.Mover0"));
				AWoTMover* Boiler = Cast<AWoTMover>(FindProjectedOfClass(
					BoilerRecord, AWoTMover::StaticClass()));
				if (TestNotNull(TEXT("Arena_09 Boiler mover projects"), Boiler))
				{
					FinishProjected(Boiler);
					FHitResult BoilerHit;
					FCollisionQueryParams BoilerQuery(
						SCENE_QUERY_STAT(RuntimeMoverInvisibleSolidBoiler), true);
					const bool bBoilerHit = World->LineTraceSingleByChannel(
						BoilerHit, FVector(-3300.0, -12084.0, -7800.0),
						FVector(-3300.0, -12084.0, -8100.0), ECC_Pawn, BoilerQuery);
					TestTrue(TEXT("Arena_09 Pawn trace hits Boiler invisible solid top"),
						bBoilerHit && BoilerHit.GetActor() == Boiler);
					if (bBoilerHit)
					{
						TestEqual(TEXT("Arena_09 Boiler collision top remains at retail rest height"),
							BoilerHit.ImpactPoint.Z, -7920.0, 2.0);
					}
				}
			}
			if (Descriptor.MapIdentity == TEXT("Battle2_02"))
			{
				const int32 ZoneRecord = FindRecordIndex(
					TEXT("Battle2_02.WOTZoneInfo13"));
				const int32 LightRecord = FindRecordIndex(
					TEXT("Battle2_02.TriggerLight16"));
				AWoTZoneVolume* HealingZone = Cast<AWoTZoneVolume>(
					FindProjectedOfClass(ZoneRecord, AWoTZoneVolume::StaticClass()));
				AWoTTriggerLight* HealingLight = Cast<AWoTTriggerLight>(
					FindProjectedOfClass(LightRecord, AWoTTriggerLight::StaticClass()));
				if (TestNotNull(TEXT("real Battle2_02 WOTZoneInfo13 projects"),
					HealingZone))
				{
					TestEqual(TEXT("real Battle2_02 ZonePlayerEvent projects"),
						HealingZone->ZonePlayerEvent,
						FName(TEXT("healing_effects")));
				}
				if (TestNotNull(TEXT("real Battle2_02 TriggerLight16 projects"),
					HealingLight))
				{
					TestEqual(TEXT("real Battle2_02 TriggerLight16 links to the zone"),
						HealingLight->TagName,
						FName(TEXT("healing_effects")));
				}
			}
			for (int32 RecordIndex = 0; RecordIndex < Source.Records.Num(); ++RecordIndex)
			{
				const FWoTRuntimeWorldActor& Record = Source.Records[RecordIndex];
				const bool bAuditedDefaultTriggerClass =
					Record.ClassIdentity.Equals(TEXT("Engine.Trigger"), ESearchCase::IgnoreCase)
					|| Record.ClassIdentity.Equals(TEXT("WOT.DoorTrigger"), ESearchCase::IgnoreCase)
					|| Record.ClassIdentity.Equals(TEXT("WOT.SpecialEventTrigger"), ESearchCase::IgnoreCase);
				if (bAuditedDefaultTriggerClass
					&& WoTRuntimeWorldFindProperty(Record, TEXT("TriggerType")) == nullptr)
				{
					AWoTTrigger* Trigger = Cast<AWoTTrigger>(FindProjectedOfClass(
						RecordIndex, AWoTTrigger::StaticClass()));
					if (TestNotNull(Record.Identity + TEXT(" defaulted trigger projects"), Trigger))
					{
						TestEqual(Record.Identity + TEXT(" defaults to TT_PlayerProximity"),
							Trigger->TriggerType, EWoTTriggerType::PlayerProximity);
						++ProductionDefaultPlayerTriggers;
					}
				}

				AWoTMover* ProjectedMover = Cast<AWoTMover>(FindProjectedOfClass(
					RecordIndex, AWoTMover::StaticClass()));
				if (ProjectedMover && (ProjectedMover->InitialState == EWoTMoverState::BumpOpenTimed
					|| ProjectedMover->InitialState == EWoTMoverState::BumpButton))
				{
					// Retail default BT_PlayerBump (Engine.Mover) unless authored:
					// Battle2_04.Mover3 authors BT_PawnBump.
					if (Record.Identity == TEXT("Battle2_04.Mover3"))
					{
						TestEqual(TEXT("Battle2_04.Mover3 keeps its authored BT_PawnBump"),
							ProjectedMover->BumpType, EWoTMoverBumpType::PawnBump);
					}
					else if (WoTRuntimeWorldFindProperty(Record, TEXT("BumpType")) == nullptr)
					{
						TestEqual(Record.Identity + TEXT(" defaults to retail BT_PlayerBump"),
							ProjectedMover->BumpType, EWoTMoverBumpType::PlayerBump);
					}
					++ProductionDefaultPlayerBumpMovers;
				}
				if (Record.Identity == TEXT("Battle3_01.Mover4")
					|| Record.Identity == TEXT("Battle4_01.Mover7"))
				{
					const FWoTRuntimeWorldProperty* Location =
						WoTRuntimeWorldFindProperty(Record, TEXT("Location"));
					FVector AuthoredLocation = FVector::ZeroVector;
					TestTrue(Record.Identity + TEXT(" has its real T3D Location"),
						Location && WoTRuntimeWorldPropertyVector(*Location, AuthoredLocation));
					if (TestNotNull(Record.Identity + TEXT(" projects as a mover"), ProjectedMover))
					{
						TestTrue(Record.Identity + TEXT(" rests at its KeyNum pose"),
							ProjectedMover->GetActorLocation().Equals(AuthoredLocation, 1.0f));
						TestEqual(Record.Identity + TEXT(" retains KeyNum as current key"),
							ProjectedMover->GetCurrentKeyNum(), 1);
						++ProductionKeyNumRestingMovers;
					}
				}

				for (const FExpectedTruncatedPool& ExpectedPool : ExpectedTruncatedPools)
				{
					if (Record.Identity != ExpectedPool.Identity) continue;
					AWoTAngrealSpawnPool* Pool = Cast<AWoTAngrealSpawnPool>(FindProjectedOfClass(
						RecordIndex, AWoTAngrealSpawnPool::StaticClass()));
					if (TestNotNull(Record.Identity + TEXT(" projects as a spawn pool"), Pool))
					{
						TArray<FString> Entries;
						for (int32 Slot = 0; Slot < Pool->Pool.Num(); ++Slot)
						{
							FString ClassLeaf = Pool->Pool[Slot]
								? Pool->Pool[Slot]->GetName() : TEXT("None");
							ClassLeaf.RemoveFromStart(TEXT("WoT"));
							Entries.Add(FString::Printf(TEXT("%s:%d"), *ClassLeaf,
								Pool->Weight.IsValidIndex(Slot) ? Pool->Weight[Slot] : 0));
						}
						TestEqual(Record.Identity + TEXT(" truncates at the first None"),
							FString::Join(Entries, TEXT(",")), FString(ExpectedPool.Entries));
						++ProductionTruncatedBattle4Pools;
					}
				}

				const bool bSilentLitAmbient =
					Record.Identity == TEXT("Arena_09.ParticleSprayer51")
					|| Record.Identity == TEXT("Battle2_05.ParticleSprayer51")
					|| Record.Identity == TEXT("Battle2_05.ParticleSprayer5");
				if (bSilentLitAmbient)
				{
					AWoTAmbientSound* Ambient = Cast<AWoTAmbientSound>(FindProjectedOfClass(
						RecordIndex, AWoTAmbientSound::StaticClass()));
					if (TestNotNull(Record.Identity + TEXT(" projects its ambient loop"), Ambient))
					{
						TestTrue(Record.Identity + TEXT(" is a light-carried loop"),
							Ambient->LightType != 0);
						TestEqual(Record.Identity + TEXT(" effective ambient volume is zero"),
							Ambient->LightBrightness, static_cast<uint8>(0));
						TestEqual(Record.Identity + TEXT(" missing LightPeriod defaults to zero"),
							Ambient->LightPeriod, static_cast<uint8>(0));
						++ProductionSilentLitAmbients;
					}
				}
				if (Record.Identity == TEXT("Battle4_01.WOTCarcassHound0"))
				{
					AWoTDecoration* Carcass = Cast<AWoTDecoration>(FindProjectedOfClass(
						RecordIndex, AWoTDecoration::StaticClass()));
					if (TestNotNull(TEXT("Battle4_01.WOTCarcassHound0 projects"), Carcass))
					{
						TestFalse(TEXT("Battle4_01.WOTCarcassHound0 blocks no actors"),
							Carcass->bBlockActors);
						TestFalse(TEXT("Battle4_01.WOTCarcassHound0 blocks no players"),
							Carcass->bBlockPlayers);
						TestEqual(TEXT("Battle4_01.WOTCarcassHound0 ignores pawn collision"),
							Carcass->Collision->GetCollisionResponseToChannel(ECC_Pawn), ECR_Ignore);
						++ProductionNonBlockingCarcasses;
					}
				}
				const bool bHiddenRotatingMover =
					Record.Identity == TEXT("Battle2_02.RotatingMover2")
					|| Record.Identity == TEXT("Battle2_02.RotatingMover7")
					|| Record.Identity == TEXT("Battle2_02.RotatingMover8")
					|| Record.Identity == TEXT("Battle2_02.RotatingMover9")
					|| Record.Identity == TEXT("Battle2_02.RotatingMover10")
					|| Record.Identity == TEXT("Battle2_02.RotatingMover11");
				if (bHiddenRotatingMover && TestNotNull(
					Record.Identity + TEXT(" projects as a hidden mover"), ProjectedMover))
				{
					TestTrue(Record.Identity + TEXT(" keeps authored bHidden"),
						ProjectedMover->IsHidden());
					TestTrue(Record.Identity + TEXT(" keeps mover collision while hidden"),
						ProjectedMover->Mesh->GetCollisionEnabled()
							!= ECollisionEnabled::NoCollision);
					++ProductionHiddenMovers;
				}
			}
			for (int32 RecordIndex = 0; RecordIndex < Source.Records.Num(); ++RecordIndex)
			{
				const FWoTRuntimeWorldActor& Record = Source.Records[RecordIndex];
				if (!Defaults.ClassIsA(Record.ClassIdentity, TEXT("Engine.PlayerStart")))
				{
					continue;
				}
				const FWoTRuntimeWorldProperty* LocationProperty =
					WoTRuntimeWorldFindProperty(Record, TEXT("Location"));
				FVector AuthoredLocation = FVector::ZeroVector;
				AActor* ProjectedStart = FindProjected(RecordIndex);
				TestTrue(Descriptor.MapIdentity
					+ TEXT(" production PlayerStart location decodes"),
					LocationProperty && WoTRuntimeWorldPropertyVector(
						*LocationProperty, AuthoredLocation));
				if (TestNotNull(Descriptor.MapIdentity
					+ TEXT(" production PlayerStart projects"), ProjectedStart))
				{
					TestTrue(Descriptor.MapIdentity
						+ TEXT(" production PlayerStart has no importer lift"),
						ProjectedStart->GetActorLocation().Equals(
							AuthoredLocation, 0.001f));
					++ProductionExactPlayerStarts;
				}
				break;
			}
			const TCHAR* RolledIdentity =
				Descriptor.MapIdentity == TEXT("Arena_02")
					? TEXT("Arena_02.GothicLamp3")
					: Descriptor.MapIdentity == TEXT("Battle3_02")
						? TEXT("Battle3_02.GnarledTree1") : nullptr;
			if (RolledIdentity != nullptr)
			{
				const int32 RecordIndex = FindRecordIndex(RolledIdentity);
				AWoTDecoration* Decoration = Cast<AWoTDecoration>(
					FindProjected(RecordIndex));
				if (TestNotNull(FString(RolledIdentity) + TEXT(" projects as decoration"),
					Decoration))
				{
					const FWoTRuntimeWorldProperty* RotationProperty =
						WoTRuntimeWorldFindProperty(Source.Records[RecordIndex],
							TEXT("Rotation"));
					FRotator AuthoredRotation = FRotator::ZeroRotator;
					TestTrue(FString(RolledIdentity) + TEXT(" has decoded authored rotation"),
						RotationProperty != nullptr && WoTRuntimeWorldPropertyRotator(
							*RotationProperty, AuthoredRotation));
					TestTrue(FString(RolledIdentity) + TEXT(" collision actor remains vertical"),
						Decoration->GetActorQuat().AngularDistance(FQuat::Identity) <= 1e-6f);
					TestTrue(FString(RolledIdentity) + TEXT(" visual retains pitch yaw and roll"),
						Decoration->Mesh->GetRelativeRotation().Quaternion().AngularDistance(
							AuthoredRotation.Quaternion()) <= 1e-5f);
					FinishProjected(Decoration);
					TestTrue(FString(RolledIdentity) + TEXT(" collision component stays world-axis aligned"),
						Decoration->Collision->GetComponentQuat().AngularDistance(
							FQuat::Identity) <= 1e-6f);
					FHitResult Hit;
					const FVector Center = Decoration->GetActorLocation();
					const float Radius = Decoration->Collision->GetUnscaledCapsuleRadius();
					const bool bSwept = World->SweepSingleByChannel(Hit,
						Center + FVector(-Radius * 2.0f, 0, 0),
						Center + FVector(Radius * 2.0f, 0, 0), FQuat::Identity,
						ECC_Pawn, FCollisionShape::MakeSphere(1.0f));
					TestTrue(FString(RolledIdentity) + TEXT(" production record pawn sweep hits"),
						bSwept && Hit.GetActor() == Decoration);
					++ProductionRotationSweeps;
				}
			}

			if (Descriptor.MapIdentity == TEXT("Arena_06"))
			{
				const int32 RecordIndex = FindRecordIndex(TEXT("Arena_06.DartForsaken0"));
				AWoTAngrealInvDart* Dart = Cast<AWoTAngrealInvDart>(
					FindProjected(RecordIndex));
				if (TestNotNull(TEXT("real Arena_06 DartForsaken projects as base dart"), Dart))
				{
					TestEqual(TEXT("real DartForsaken replacement color"),
						Dart->ConfiguredDartColor, FName(TEXT("Red")));
					TestEqual(TEXT("real DartForsaken minimum charges"), Dart->MinInitialCharges, 20);
					TestEqual(TEXT("real DartForsaken maximum initial charges"), Dart->MaxInitialCharges, 70);
					TestEqual(TEXT("real DartForsaken maximum charges"), Dart->MaxCharges, 100);
					TestEqual(TEXT("real DartForsaken charge cost"), Dart->ChargeCost, 1);
					TestTrue(TEXT("real DartForsaken uses base white icon"),
						Dart->StatusIcon.ToSoftObjectPath().ToString().Contains(TEXT("I_Dartwh")));
					TestTrue(TEXT("real DartForsaken uses base white icon frame"),
						Dart->StatusIconFrame.ToSoftObjectPath().ToString().Contains(TEXT("M_Dartwh")));
					FWoTRuntimeActorVisualPresentation BasePresentation;
					FString PresentationError;
					TestTrue(TEXT("real base dart presentation decodes"),
						WoTRuntimeReadClassVisualPresentation(Defaults,
							TEXT("Angreal.AngrealInvDart"), BasePresentation,
							PresentationError));
					const FSoftObjectPath Skin =
						BasePresentation.Skin;
					TestTrue(TEXT("real DartForsaken uses base DartAesSedai pickup skin"),
						Skin.ToString().Contains(TEXT("DartAesSedai")));
					++ProductionForsakenReplacements;
				}
				const int32 BarrelIndex = FindRecordIndex(TEXT("Arena_06.BarrelTap0"));
				AWoTBreakableDecoration* Barrel = Cast<AWoTBreakableDecoration>(
					FindProjected(BarrelIndex));
				if (TestNotNull(TEXT("real Arena_06 BarrelTap0 projects as breakable"), Barrel))
				{
					FinishProjected(Barrel);
					AWoTCharacter* Pusher = World->SpawnActor<AWoTCharacter>(
						Barrel->GetActorLocation() - FVector(50, 0, 0),
						FRotator::ZeroRotator);
					Pusher->GetCharacterMovement()->Velocity = FVector(80, 0, 0);
					FHitResult Bump;
					Bump.ImpactPoint = Barrel->GetActorLocation();
					Barrel->NotifyHit(Barrel->Collision, Pusher,
						Pusher->GetCapsuleComponent(), false, Bump.ImpactPoint,
						-FVector::ForwardVector, FVector::ZeroVector, Bump);
					TestTrue(TEXT("production barrel bPushable bump starts its falling path"),
						Barrel->bPushable && Barrel->GetFallVelocity().X > 0.0f);
					TestEqual(TEXT("production barrel records its pawn pusher"),
						Barrel->GetInstigator(), static_cast<APawn*>(Pusher));
					++ProductionPushableBarrels;
				}
			}

			if (Descriptor.MapIdentity == TEXT("Battle2_05"))
			{
				auto ExerciseDrivenDecoration = [this, &FindRecordIndex,
					&FindProjected, &FinishProjected,
					&ProductionDrivenDecorationTicks](const TCHAR* DecorationIdentity,
					const TCHAR* RotatorIdentity, bool bExpectBlocking)
				{
					AWoTDecoration* Decoration = Cast<AWoTDecoration>(FindProjected(
						FindRecordIndex(DecorationIdentity)));
					AWoTActorRotator* Rotator = Cast<AWoTActorRotator>(FindProjected(
						FindRecordIndex(RotatorIdentity)));
					if (!TestNotNull(FString(DecorationIdentity) + TEXT(" projects"), Decoration)
						|| !TestNotNull(FString(RotatorIdentity) + TEXT(" projects"), Rotator))
					{
						return;
					}
					FinishProjected(Decoration);
					FinishProjected(Rotator);
					const FQuat Before = Decoration->Mesh->GetComponentQuat();
					Rotator->Initialize();
					Rotator->Tick(0.125f);
					TestTrue(FString(DecorationIdentity) + TEXT(" collision root stays identity"),
						Decoration->GetActorQuat().AngularDistance(FQuat::Identity) <= 1e-6f
						&& Decoration->Collision->GetComponentQuat().AngularDistance(
							FQuat::Identity) <= 1e-6f);
					TestTrue(FString(DecorationIdentity) + TEXT(" visual rotation is replaced"),
						Decoration->Mesh->GetComponentQuat().AngularDistance(Before) > 1e-4f);
					if (bExpectBlocking)
					{
						TestEqual(TEXT("production Fish blocks pawns"),
							Decoration->Collision->GetCollisionResponseToChannel(ECC_Pawn),
							ECR_Block);
					}
					++ProductionDrivenDecorationTicks;
				};
				ExerciseDrivenDecoration(TEXT("Battle2_05.Fish1"),
					TEXT("Battle2_05.ActorRotator4"), true);
				ExerciseDrivenDecoration(TEXT("Battle2_05.DragonFly16"),
					TEXT("Battle2_05.ActorRotator18"), false);
			}

			if (Descriptor.MapIdentity == TEXT("Battle2_03"))
			{
				for (int32 RecordIndex = 0; RecordIndex < Source.Records.Num(); ++RecordIndex)
				{
					const FWoTRuntimeWorldProperty* Gravity = WoTRuntimeWorldFindProperty(
						Source.Records[RecordIndex], TEXT("ZoneGravity"));
					FVector Value = FVector::ZeroVector;
					if (!Gravity || !WoTRuntimeWorldPropertyVector(*Gravity, Value)
						|| !FMath::IsNearlyEqual(Value.Z, -500.0f)) continue;
					AWoTZoneVolume* Zone = Cast<AWoTZoneVolume>(FindProjected(RecordIndex));
					if (TestNotNull(TEXT("Battle2_03 -500 authored zone projects"), Zone))
					{
						TestTrue(TEXT("production zone retains the authored gravity override"),
							Zone->bOverrideGravity
								&& Zone->ZoneGravity.Equals(FVector(0, 0, -500), 0.01f));
						++ProductionRetailGravityZones;
					}
					break;
				}
#if WITH_EDITOR
				RunAnimationNetFixture = [this, &Source, &Defaults, FindRecordIndex]()
				{
					using namespace UE::Net;
					FTestWorlds Dedicated(TEXT("/Engine/Maps/Entry"),
						TEXT("/Script/WoT.WoTGameMode"));
					Dedicated.SetDescription(
						TEXT("F1b dedicated animation resource exclusion"));
					UWorld* DedicatedWorld = Dedicated.Server.GetWorld();
					if (TestTrue(TEXT("real dedicated animation fixture loads"),
						Dedicated.Server.IsLoaded())
						&& TestEqual(TEXT("animation fixture is a real dedicated server"),
							DedicatedWorld->GetNetMode(), NM_DedicatedServer))
					{
						// Isolate the consumers from the test world's one-time game-mode
						// startup. Every projected actor is spawned after this warm-up, so
						// the measured window still includes each consumer's first tick.
						Dedicated.SetTickInSeconds(1.0f / 60.0f);
						Dedicated.TickAll(1);
						WoTResetTextureAnimationResourceCountsForTesting();
						constexpr int32 DedicatedRNGSeed = 0x46314232;
						int32 ExpectedDedicatedRNG[4];
						FMath::RandInit(DedicatedRNGSeed);
						for (int32& Value : ExpectedDedicatedRNG) Value = FMath::Rand();
						FMath::RandInit(DedicatedRNGSeed);
						FActorSpawnParameters Spawn;
						Spawn.SpawnCollisionHandlingOverride =
							ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
						TestNotNull(TEXT("dedicated EarthTremor projectile"),
							DedicatedWorld->SpawnActor<AWoTEarthTremorProjectile>(
								FVector(0, 0, 10000), FRotator::ZeroRotator, Spawn));
						TestNotNull(TEXT("dedicated Decay projectile"),
							DedicatedWorld->SpawnActor<AWoTDecayProjectile>(
								FVector(1000, 0, 10000), FRotator::ZeroRotator, Spawn));
						TestNotNull(TEXT("dedicated SwapPlaces projectile"),
							DedicatedWorld->SpawnActor<AWoTSwapPlacesProjectile>(
								FVector(2000, 0, 10000), FRotator::ZeroRotator, Spawn));

						TArray<FWoTRuntimeWorldActor> DedicatedRecords;
						FWoTWorldProjectionSource DedicatedSource;
						DedicatedSource.MapIdentity = Source.MapIdentity;
						DedicatedSource.FindTexture = Source.FindTexture;
						const int32 SprayerRecord = FindRecordIndex(
							TEXT("Battle2_03.ParticleSprayer54"));
						if (TestTrue(TEXT("real Battle2_03 sprayer record exists"),
							Source.Records.IsValidIndex(SprayerRecord)))
						{
							DedicatedRecords.Add(Source.Records[SprayerRecord]);
							DedicatedSource.Records = DedicatedRecords;
						}
						FWoTWorldProjectionResult DedicatedProjection;
						FString DedicatedError;
						TestTrue(TEXT("real Battle2_03 sprayer projects on dedicated"),
							FWoTWorldProjector::ProjectDeferred(*DedicatedWorld,
								DedicatedSource, Defaults, DedicatedProjection,
								DedicatedError));
						AWoTParticleSprayer* DedicatedSprayer = nullptr;
						for (AActor* Actor : DedicatedProjection.DeferredActors)
						{
							if (AWoTParticleSprayer* Candidate =
								Cast<AWoTParticleSprayer>(Actor))
							{
								DedicatedSprayer = Candidate;
								Candidate->FinishSpawning(
									Candidate->GetActorTransform(), true);
								break;
							}
						}
						TestNotNull(TEXT("dedicated projected production sprayer"),
							DedicatedSprayer);
						for (int32 Frame = 0; Frame < 60; ++Frame)
						{
							Dedicated.TickAll(1);
						}
						const FWoTTextureAnimationResourceCounts Counts =
							WoTGetTextureAnimationResourceCountsForTesting();
						TestFalse(TEXT("dedicated projected sprayer actor tick is disabled"),
							DedicatedSprayer && DedicatedSprayer->IsActorTickEnabled());
						TestEqual(TEXT("dedicated sprayer runs zero particle substeps"),
							Counts.ParticleSubsteps, 0);
						TestEqual(TEXT("dedicated sprayer runs zero animation updates"),
							Counts.AnimationUpdates, 0);
						TestEqual(TEXT("dedicated consumers resolve zero animation textures"),
							Counts.TextureResolutions, 0);
						TestEqual(TEXT("dedicated consumers create zero animation MIDs"),
							Counts.DynamicMaterialCreations, 0);
						for (int32 Index = 0; Index < UE_ARRAY_COUNT(ExpectedDedicatedRNG);
							++Index)
						{
							TestEqual(FString::Printf(TEXT("dedicated presentation preserves "
								"global RNG sample %d"), Index), FMath::Rand(),
								ExpectedDedicatedRNG[Index]);
						}

						DedicatedWorld->SpawnActor<AWoTPlayerStart>(
							FVector(-10000, 0, 5000), FRotator::ZeroRotator, Spawn);
						Dedicated.SetTickInSeconds(1.0f / 30.0f);
						if (TestTrue(TEXT("real rendering client connects for map animation"),
							Dedicated.CreateAndConnectClient()))
						{
							UWorld* ClientWorld = Dedicated.Clients[0].GetWorld();
							FWoTWorldProjectionResult ClientProjection;
							FString ClientError;
							TestTrue(TEXT("real Battle2_03 sprayer projects on client"),
								FWoTWorldProjector::ProjectDeferred(*ClientWorld,
									DedicatedSource, Defaults, ClientProjection,
									ClientError));
							AWoTParticleSprayer* ClientSprayer = nullptr;
							for (AActor* Actor : ClientProjection.DeferredActors)
							{
								if (AWoTParticleSprayer* Candidate =
									Cast<AWoTParticleSprayer>(Actor))
								{
									ClientSprayer = Candidate;
									Candidate->FinishSpawning(
										Candidate->GetActorTransform(), true);
									break;
								}
							}
							if (TestNotNull(TEXT("client projected production sprayer"),
								ClientSprayer))
							{
								// This fixture exercises client texture animation, not
								// culling: the client camera sits at the player start,
								// far outside this sprayer's visibility cylinder, where
								// a projected map sprayer is (correctly) frozen.
								ClientSprayer->bFreezeOutsideVisibility = false;
								int32 AnimatedSlot = INDEX_NONE;
								for (int32 Slot = 0;
									Slot < ClientSprayer->Templates.Num(); ++Slot)
								{
									if (ClientSprayer->GetTemplateAnimationFrameCount(Slot)
										== 5)
									{
										AnimatedSlot = Slot;
										break;
									}
								}
								if (TestTrue(TEXT("client sprayer has real decoded ring"),
									AnimatedSlot != INDEX_NONE))
								{
									UTexture* Initial = ClientSprayer
										->GetTemplateBoundTextureForTesting(AnimatedSlot);
									TestNotNull(TEXT("client sprayer initial MID texture"),
										Initial);
									UTexture* Advanced = Initial;
									for (int32 Tick = 0;
										Tick < 120 && Advanced == Initial; ++Tick)
									{
										ClientSprayer->MarkTemplateDrawnForTesting(
											AnimatedSlot);
										Dedicated.TickAll(1);
										Advanced = ClientSprayer
											->GetTemplateBoundTextureForTesting(
												AnimatedSlot);
									}
									if (TestTrue(TEXT("production UpdateTextureAnimations "
										"writes the real client MID"),
										Advanced && Advanced != Initial))
									{
										const FString Identity = ClientSprayer
											->GetTemplateFrameIdentityForTesting(
												AnimatedSlot, 1);
										int32 Dot = INDEX_NONE;
										const FString Expected = Identity.FindLastChar(
											TEXT('.'), Dot)
											? Identity.RightChop(Dot + 1) : FString();
										TestEqual(TEXT("production sprayer binds decoded "
											"next frame"), Advanced->GetName(), Expected);
									}
								}
							}
						}
					}
				};
#endif
				for (const TCHAR* Identity : {
					TEXT("Battle2_03.ParticleSprayer54"),
					TEXT("Battle2_03.ParticleSprayer38"),
					TEXT("Battle2_03.ParticleSprayer47")})
				{
					AWoTParticleSprayer* Sprayer = Cast<AWoTParticleSprayer>(
						FindProjected(FindRecordIndex(Identity)));
					if (!TestNotNull(FString(Identity) + TEXT(" projects"), Sprayer)) continue;
					FinishProjected(Sprayer);
					int32 AnimatedSlot = INDEX_NONE;
					for (int32 Slot = 0; Slot < Sprayer->Templates.Num(); ++Slot)
					{
						if (Sprayer->GetTemplateAnimationFrameCount(Slot) == 5)
						{
							AnimatedSlot = Slot;
							break;
						}
					}
					if (!TestTrue(FString(Identity)
						+ TEXT(" has a decoded five-frame map texture ring"),
						AnimatedSlot != INDEX_NONE)) continue;
				}
				uint32 HealZoneSprayers = 0;
				for (AActor* Actor : Projection.DeferredActors)
				{
					AWoTParticleSprayer* Sprayer = Cast<AWoTParticleSprayer>(Actor);
					if (Sprayer == nullptr || Sprayer->TagName != TEXT("heal_zone")) continue;
					++HealZoneSprayers;
					bool bHasFiveFrameRing = false;
					for (int32 Slot = 0; Slot < Sprayer->Templates.Num(); ++Slot)
					{
						bHasFiveFrameRing |=
							Sprayer->GetTemplateAnimationFrameCount(Slot) == 5;
					}
					TestTrue(TEXT("every Battle2_03 heal_zone sprayer has a five-frame ring"),
						bHasFiveFrameRing);
				}
				TestTrue(TEXT("Battle2_03 contains projected heal_zone sprayers"),
					HealZoneSprayers > 0);
			}

			// ParticleSprayer.uc:250 PreBeginPlay sets bOn = bInitiallyOn, so every
			// real map sprayer starts at bInitiallyOn whatever bOn the map stores
			// (UnrealEd's preview) or leaves to the false class default. The port
			// used to start 95 multiplayer-map sprayers on: Arena_09's OilSpray
			// field, the Battle portal-stone flares and leave_portal fountains.
			int32 OilSprayNozzles = 0;
			for (AActor* ProjectedActor : Projection.DeferredActors)
			{
				const AWoTParticleSprayer* Sprayer = Cast<AWoTParticleSprayer>(ProjectedActor);
				if (Sprayer == nullptr) continue;
				TestEqual(Descriptor.MapIdentity + TEXT(" ") + Sprayer->GetName()
					+ TEXT(" starts at bInitiallyOn"), Sprayer->bOn, Sprayer->bInitiallyOn);
				if (Sprayer->TagName.ToString().StartsWith(TEXT("OilSpray")))
				{
					++OilSprayNozzles;
					TestFalse(TEXT("Arena_09 OilSpray nozzle starts off"), Sprayer->bOn);
				}
			}
			if (Descriptor.MapIdentity == TEXT("Arena_09"))
			{
				TestEqual(TEXT("Arena_09 projects its eleven OilSpray nozzles"),
					OilSprayNozzles, 11);
			}

			int32 MapModulatedSprayers = 0;
			TSet<FString> Battle301ModulatedHealZones;
			for (int32 RecordIndex = 0; RecordIndex < Source.Records.Num(); ++RecordIndex)
			{
				const FWoTRuntimeWorldActor& Record = Source.Records[RecordIndex];
				if (!Defaults.ClassIsA(Record.ClassIdentity,
					TEXT("ParticleSystems.ParticleSprayer")))
				{
					continue;
				}
				AWoTParticleSprayer* Sprayer = Cast<AWoTParticleSprayer>(
					FindProjected(RecordIndex));
				if (Sprayer == nullptr) continue;
				TestTrue(Record.Identity + TEXT(" freezes outside its retail visibility cylinder"),
					Sprayer->bFreezeOutsideVisibility);
				uint8 Style = 1;
				const FWoTRuntimeWorldProperty* StyleProperty =
					Defaults.FindProperty(Record, TEXT("Style"), 0);
				if (!TestTrue(Record.Identity + TEXT(" resolves actor Style"),
					StyleProperty != nullptr
						&& WoTRuntimeWorldPropertyByte(*StyleProperty, Style)))
				{
					continue;
				}
				const bool bModulated = Style == 4;
				TestEqual(Record.Identity + TEXT(" projects its actor Style mode"),
					Sprayer->bModulateOnly, bModulated);
				for (int32 Slot = 0; Slot < Sprayer->Templates.Num(); ++Slot)
				{
					const bool bHasAdd = Sprayer->AddMaterials.IsValidIndex(Slot)
						&& Sprayer->AddMaterials[Slot] != nullptr;
					const bool bHasMod = Sprayer->ModMaterials.IsValidIndex(Slot)
						&& Sprayer->ModMaterials[Slot] != nullptr;
					TestEqual(Record.Identity + TEXT(" additive pass follows Style"),
						bHasAdd, !bModulated);
					TestTrue(Record.Identity + TEXT(" has its required modulate pass"), bHasMod);
				}
				if (!bModulated) continue;
				++MapModulatedSprayers;
				++TotalProjectedModulatedSprayers;
				// Battle3_01's heal-zone smoke is tagged HealZone (Battle2_03 uses
				// heal_zone); FName comparison ignores case, not spelling.
				if (Descriptor.MapIdentity == TEXT("Battle3_01")
					&& Sprayer->TagName == TEXT("HealZone"))
				{
					Battle301ModulatedHealZones.Add(Record.Identity);
				}
			}
			if (Descriptor.MapIdentity == TEXT("Arena_11"))
			{
				TestEqual(TEXT("Arena_11 projects twelve modulated sprayers"),
					MapModulatedSprayers, 12);
			}
			if (Descriptor.MapIdentity == TEXT("Battle3_01"))
			{
				TestEqual(TEXT("Battle3_01 projects three modulated heal-zone sprayers"),
					Battle301ModulatedHealZones.Num(), 3);
				for (const TCHAR* Identity : {
					TEXT("Battle3_01.ParticleSprayer26"),
					TEXT("Battle3_01.ParticleSprayer28"),
					TEXT("Battle3_01.ParticleSprayer32")})
				{
					TestTrue(FString(Identity) + TEXT(" is a modulated heal-zone sprayer"),
						Battle301ModulatedHealZones.Contains(Identity));
				}
			}

			for (AActor* ProjectedActor : Projection.DeferredActors)
			{
				if (AWoTPortcullis* Portcullis = Cast<AWoTPortcullis>(ProjectedActor))
				{
					const UCapsuleComponent* TriggerCapsule =
						Cast<UCapsuleComponent>(Portcullis->GetRootComponent());
					TestEqual(TEXT("production Portcullis uses containing trigger capsule"),
						TriggerCapsule ? TriggerCapsule->GetUnscaledCapsuleHalfHeight() : 0.0f,
						WoTUE1Collision::ContainingCapsuleHalfHeight(
							Portcullis->GetUE1TriggerRadius(),
							Portcullis->GetUE1TriggerHalfHeight()));
					++ProductionRetailTriggerCylinders;
					break;
				}
				if (AWoTStaircase* Staircase = Cast<AWoTStaircase>(ProjectedActor))
				{
					const UCapsuleComponent* TriggerCapsule =
						Cast<UCapsuleComponent>(Staircase->GetRootComponent());
					TestEqual(TEXT("production Staircase uses containing trigger capsule"),
						TriggerCapsule ? TriggerCapsule->GetUnscaledCapsuleHalfHeight() : 0.0f,
						WoTUE1Collision::ContainingCapsuleHalfHeight(
							Staircase->GetUE1TriggerRadius(),
							Staircase->GetUE1TriggerHalfHeight()));
					++ProductionRetailTriggerCylinders;
					break;
				}
			}

			if (Descriptor.MapIdentity == TEXT("Battle4_01"))
			{
				UWorld* TouchWorld = WoTTest::CreateSpecWorld();
				FWoTWorldProjectionResult TouchProjection;
				FString TouchError;
				const bool bTouchProjected = FWoTWorldProjector::ProjectDeferred(
					*TouchWorld, Source, Defaults, TouchProjection, TouchError);
				TestTrue(TEXT("Battle4_01 production projectile fixture projects"),
					bTouchProjected);
				if (!bTouchProjected) AddError(TouchError);
				auto FindTouchProjected = [&TouchProjection](int32 RecordIndex) -> AActor*
				{
					const FString Prefix = FString::Printf(TEXT("WoTProjected_%d_"),
						RecordIndex);
					for (AActor* Actor : TouchProjection.DeferredActors)
						if (Actor && Actor->GetName().StartsWith(Prefix)) return Actor;
					return nullptr;
				};
				auto FinishTouchProjected = [TouchWorld](AActor* Actor)
				{
					if (Actor) Actor->FinishSpawning(Actor->GetActorTransform(), true);
					TouchWorld->UpdateWorldComponents(false, false);
				};
				auto FireMovingDart = [this, TouchWorld, &FinishTouchProjected](AActor* Target,
					const FString& Label) -> bool
				{
					if (!Target) return false;
					FinishTouchProjected(Target);
					const FVector Start = Target->GetActorLocation() - FVector(96, 0, 0);
					FActorSpawnParameters Parameters;
					Parameters.SpawnCollisionHandlingOverride =
						ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					AWoTDartProjectile* Projectile = TouchWorld->SpawnActor<AWoTDartProjectile>(
						AWoTDartProjectile::StaticClass(), Start, FRotator::ZeroRotator,
						Parameters);
					if (!TestNotNull(Label + TEXT(" moving dart spawns"), Projectile)) return false;
					WoTTest::TickWorld(TouchWorld, 0.12f, 0.01f);
					TestTrue(Label + TEXT(" moving dart reaches Touch and explodes"),
						!IsValid(Projectile) || Projectile->IsActorBeingDestroyed());
					return !IsValid(Projectile) || Projectile->IsActorBeingDestroyed();
				};
				const int32 HoundIndex = FindRecordIndex(TEXT("Battle4_01.WOTCarcassHound0"));
				AWoTDecoration* Hound = Cast<AWoTDecoration>(
					FindTouchProjected(HoundIndex));
				if (TestNotNull(TEXT("real Battle4_01 Hound carcass projects"), Hound))
				{
					TestEqual(TEXT("Hound projectile response is overlap"),
						Hound->Collision->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
						ECR_Overlap);
					FireMovingDart(Hound, TEXT("Hound carcass"));
					TestTrue(TEXT("Hound Touch dispatches direct projectile damage"),
						Hound->LastDamageTaken > 0.0f);
					++ProductionProjectileTouches;
				}
				const int32 WallIndex = FindRecordIndex(TEXT("Battle4_01.WOTExplodingWall0"));
				AWoTExplodingWall* Wall = Cast<AWoTExplodingWall>(
					FindTouchProjected(WallIndex));
				if (TestNotNull(TEXT("real Battle4_01 exploding wall projects"), Wall))
				{
					TestEqual(TEXT("wall projectile response is overlap"),
						Wall->Capsule->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
						ECR_Overlap);
					FireMovingDart(Wall, TEXT("ExplodingWall"));
					TestFalse(TEXT("unmatched dart damage type does not break the wall"),
						Wall->HasExploded());
					++ProductionProjectileTouches;
				}
				WoTTest::DestroySpecWorld(TouchWorld);
			}
			// Level.Title of LevelInfo0 (exports/t3d/<Map>.t3d line 4). Battle2_05
			// also ships a stray LevelInfo2 titled "The Inner Sanctum" (t3d line
			// 141170) that retail never reads: GetLevelInfo() is Actors(0).
			static const TCHAR* const ExpectedTitles[][2] = {
				{TEXT("Arena_01"), TEXT("Absynth")},
				{TEXT("Arena_02"), TEXT("Keep on the Borderlands (B2)")},
				{TEXT("Arena_03"), TEXT("Dance of the Marionette")},
				{TEXT("Arena_04"), TEXT("Cacophony of Silence")},
				{TEXT("Arena_05"), TEXT("Aridhol Awakens")},
				{TEXT("Arena_06"), TEXT("The Grim Guignol")},
				{TEXT("Arena_07"), TEXT("Odium's Obloquy")},
				{TEXT("Arena_08"), TEXT("Hush")},
				{TEXT("Arena_09"), TEXT("Intolerance")},
				{TEXT("Arena_11"), TEXT("Kingdom Come")},
				{TEXT("Arena_12"), TEXT("The Inquisition")},
				{TEXT("Battle2_01"), TEXT("The Stench of Decay")},
				{TEXT("Battle2_02"), TEXT("Sands of Time")},
				{TEXT("Battle2_03"), TEXT("Bone Orchard")},
				{TEXT("Battle2_04"), TEXT("Outposts")},
				{TEXT("Battle2_05"), TEXT("Heresy")},
				{TEXT("Battle3_01"), TEXT("Shattered Hope")},
				{TEXT("Battle3_02"), TEXT("RingFinger")},
				{TEXT("Battle4_01"), TEXT("Blood and Ashes")},
			};
			const TCHAR* ExpectedTitle = nullptr;
			for (const auto& Row : ExpectedTitles)
			{
				if (Descriptor.MapIdentity.Equals(Row[0], ESearchCase::CaseSensitive))
					ExpectedTitle = Row[1];
			}
			if (TestNotNull(Descriptor.MapIdentity + TEXT(" has a pinned retail title"),
				ExpectedTitle))
			{
				TestEqual(Descriptor.MapIdentity + TEXT(" projects Level.Title"),
					Projection.LevelTitle, FString(ExpectedTitle));
			}
			TotalProjectedMovers += Projection.RuleCount(TEXT("Mover"));
			TotalProjectedTriggers += Projection.RuleCount(TEXT("Trigger"));
			TotalProjectedDoorTriggers += Projection.RuleCount(TEXT("DoorTrigger"));
			TotalProjectedSoundDispatchers += Projection.RuleCount(TEXT("SoundDispatcher"));
			TotalProjectedPortcullises += Projection.RuleCount(TEXT("Portcullis"));
			TotalProjectedTeleporters += Projection.RuleCount(TEXT("Teleporter"));
			TotalProjectedJumpers += Projection.RuleCount(TEXT("Jumper"));
			TotalProjectedBlockAll += Projection.RuleCount(TEXT("BlockAll"));
			TotalProjectedBlockPlayer += Projection.RuleCount(TEXT("BlockPlayer"));
			TotalProjectedPitOpenings += Projection.RuleCount(TEXT("PitOpening"));
			TotalProjectedStaircases += Projection.RuleCount(TEXT("Staircase"));
			TotalProjectedSpecialEventTriggers += Projection.RuleCount(TEXT("SpecialEventTrigger"));
			TotalProjectedSpecialEvents += Projection.RuleCount(TEXT("SpecialEvent"));
			TotalProjectedSpecialEffectsTriggers += Projection.RuleCount(TEXT("SpecialEffectsTrigger"));
			TotalProjectedStochasticTriggers += Projection.RuleCount(TEXT("StochasticTrigger"));
			TotalProjectedTriggerLights += Projection.RuleCount(TEXT("TriggerLight"));
			TotalProjectedBlockMonsters += Projection.RuleCount(TEXT("BlockMonsters"));
			TotalProjectedBattleInfo += Projection.RuleCount(TEXT("BattleInfo"));
			TotalProjectedSealAltars += Projection.RuleCount(TEXT("SealAltar"));
			TotalProjectedBudgets += Projection.RuleCount(TEXT("Budget"));
			TotalProjectedMashadarGuides +=
				Projection.RuleCount(TEXT("MashadarGuide"));
			TotalProjectedTaggedMarkers += Projection.RuleCount(TEXT("TaggedMarker"));
			TotalProjectedAmbientSounds += Projection.RuleCount(TEXT("AmbientSound"));
			TotalProjectedDynamicAmbientSounds +=
				Projection.RuleCount(TEXT("DynamicAmbientSound"));
			TotalProjectedWaterSoundZones +=
				Projection.RuleCount(TEXT("WaterSoundZone"));
			TotalProjectedCoronas += Projection.RuleCount(TEXT("Corona"));
			TotalProjectedLeafMaps += Projection.RuleCount(TEXT("LeafMap"));
			TotalProjectedActorRotators += Projection.RuleCount(TEXT("ActorRotator"));
			TotalProjectedRainCyd += Projection.RuleCount(TEXT("RainCyd"));
			TotalProjectedFogLights += Projection.RuleCount(TEXT("FogLight"));
			TotalProjectedLightWaves += Projection.RuleCount(TEXT("LightWave"));
			TotalProjectedLightning += Projection.RuleCount(TEXT("Lightning"));
			TotalProjectedRandomLightning += Projection.RuleCount(TEXT("RandomLightning"));
			TotalProjectedDistanceLightning += Projection.RuleCount(TEXT("DistanceLightning"));
			TotalProjectedWind += Projection.RuleCount(TEXT("Wind"));
			TotalProjectedSpewTriggers += Projection.RuleCount(TEXT("SpewTrigger"));
			for (const TPair<FName, uint32>& Pair : Projection.InertClassCounts)
				TotalInertClasses.FindOrAdd(Pair.Key) += Pair.Value;
			for (const TPair<FName, uint32>& Pair : Projection.UnsupportedClassCounts)
				TotalUnsupportedClasses.FindOrAdd(Pair.Key) += Pair.Value;

			// Battle2_02 Mover6 (WC_THRONEDOOR, Model1445): retail ToWorld
			// subtracts PrePivot (-36.949337,-6,154.330719) from the brush-local
			// slab, so the generated mesh spans exactly this local box and the
			// door's bottom meets the world floor at z=-26158.
			bool bThroneDoorFramed = false;
			for (AActor* Actor : Projection.DeferredActors)
			{
				if (const AWoTLightning* Lightning = Cast<AWoTLightning>(Actor))
				{
					TestTrue(Descriptor.MapIdentity + TEXT(" Lightning uses retail simulated-proxy replication"),
						Lightning->GetIsReplicated() && Lightning->bAlwaysRelevant
							&& Lightning->IsNameStableForNetworking());
					if (const AWoTRandomLightning* Random =
						Cast<AWoTRandomLightning>(Lightning))
					{
						TestFalse(Descriptor.MapIdentity
							+ TEXT(" production RandomLightning reuses tagged strikes"),
							Random->bSpawnsLightning);
						TestFalse(Descriptor.MapIdentity
							+ TEXT(" production RandomLightning has a relay event"),
							Random->EventName.IsNone());
						++ProductionRandomLightningRelays;
					}
					else if (!Lightning->LightTag.IsNone())
					{
						++ProductionTaggedLightning;
					}
				}
				if (const APointLight* PointLight = Cast<APointLight>(Actor))
					if (!PointLight->Tags.IsEmpty())
						++ProductionLightningTargetLights;
				if (const AWoTWind* Wind = Cast<AWoTWind>(Actor))
				{
					TestTrue(Descriptor.MapIdentity + TEXT(" Wind uses retail simulated-proxy replication"),
						Wind->GetIsReplicated() && Wind->IsNameStableForNetworking());
					if (Descriptor.MapIdentity == TEXT("Battle2_03")
						&& Wind->TagName == TEXT("heal_zone_wind"))
					{
						TestEqual(TEXT("Battle2_03.Wind0 real projection targets heal-zone sprayers"),
							Wind->EventName, FName(TEXT("heal_zone")));
						TestEqual(TEXT("Battle2_03.Wind0 real projection is trigger-controlled"),
							Wind->InitialState, FName(TEXT("TriggerControl")));
						TestFalse(TEXT("Battle2_03.Wind0 real projection starts disabled"),
							Wind->bInitiallyOn);
						TestEqual(TEXT("Battle2_03.Wind0 decoded impulse cadence"),
							Wind->MinImpulseTime, 0.05f);
						TestEqual(TEXT("Battle2_03.Wind0 decoded wind speed"),
							Wind->MinWindspeed, 1200.0f);
						++ProductionTriggeredMPWind;
					}
				}
				if (const AWoTSpewTrigger* Spew = Cast<AWoTSpewTrigger>(Actor))
				{
					TestTrue(Descriptor.MapIdentity + TEXT(" SpewTrigger retains its retail simulated proxy"),
						Spew->GetIsReplicated() && Spew->IsNameStableForNetworking());
					TestTrue(Descriptor.MapIdentity + TEXT(" SpewTrigger decoded at least one authored thing"),
						!Spew->Things.IsEmpty());
				}
				if (Descriptor.MapIdentity == TEXT("Battle2_02"))
					if (const AWoTMover* Framed = Cast<AWoTMover>(Actor))
						if (Framed->RuntimeMesh)
						{
							const FBox Box = Framed->RuntimeMesh->GetBoundingBox();
							bThroneDoorFramed |= Box.Min.Equals(
									FVector(-0.050781, 0.0, -311.330719), 0.05)
								&& Box.Max.Equals(
									FVector(73.949455, 12.0, 2.669281), 0.05);
						}
				if (AWoTPortcullis* Portcullis = Cast<AWoTPortcullis>(Actor))
				{
					TestFalse(Descriptor.MapIdentity + TEXT(" Portcullis has mover tag"),
						Portcullis->MoverTag.IsNone());
					TestTrue(Descriptor.MapIdentity + TEXT(" Portcullis replicates"),
						Portcullis->GetIsReplicated()
							&& Portcullis->IsNameStableForNetworking());
				}
				if (AWoTPitOpening* Opening = Cast<AWoTPitOpening>(Actor))
				{
					TestTrue(Descriptor.MapIdentity + TEXT(" PitOpening stable name"),
						Opening->GetName().StartsWith(TEXT("WoTProjected_"))
							&& Opening->GetName().Contains(TEXT("_PitOpening")));
					TestTrue(Descriptor.MapIdentity + TEXT(" PitOpening replicates"),
						Opening->GetIsReplicated() && Opening->IsNameStableForNetworking());
				}
				if (AWoTStaircase* Staircase = Cast<AWoTStaircase>(Actor))
				{
					TestFalse(Descriptor.MapIdentity + TEXT(" Staircase has mover tag"),
						Staircase->MoverTag.IsNone());
					TestTrue(Descriptor.MapIdentity + TEXT(" Staircase replicates"),
						Staircase->GetIsReplicated() && Staircase->IsNameStableForNetworking());
				}
				if (AWoTCollisionBlocker* Blocker = Cast<AWoTCollisionBlocker>(Actor))
				{
					if (Blocker->bMonstersOnly)
						TestEqual(Descriptor.MapIdentity + TEXT(" monster blocker mode"),
							Blocker->Mode, EWoTCollisionBlockerMode::Monsters);
				}
				if (AWoTRandomEventTrigger* Random = Cast<AWoTRandomEventTrigger>(Actor))
				{
					TestTrue(Descriptor.MapIdentity + TEXT(" random trigger has events"),
						Random->Events.Num() > 0);
					TestTrue(Descriptor.MapIdentity + TEXT(" random trigger probability valid"),
						Random->TriggerProbability >= 0.0f
							&& Random->TriggerProbability <= 1.0f);
					const bool bEffectsRule = Random->GetName().Contains(
						TEXT("_SpecialEffectsTrigger"));
					TestEqual(Descriptor.MapIdentity + TEXT(" random trigger locality"),
						Random->bLocalOnly, bEffectsRule);
				}
				if (AWoTSpecialEventTrigger* Special =
					Cast<AWoTSpecialEventTrigger>(Actor))
				{
					if (Special->Config.Action == EWoTSpecialEventAction::PlaySoundEffect)
					{
						TestEqual(Descriptor.MapIdentity
							+ TEXT(" event-only sound relay has no collision"),
							Special->Capsule->GetCollisionEnabled(),
							ECollisionEnabled::NoCollision);
					}
					if (Descriptor.MapIdentity == TEXT("Battle2_04"))
					{
						TestTrue(TEXT("Battle2_04 fireplace repeats every 0.3 seconds"),
							FMath::IsNearlyEqual(Special->RepeatTriggerTime, 0.3f));
					}
				}
				if (AWoTTriggerLight* TriggerLight = Cast<AWoTTriggerLight>(Actor))
				{
					TestEqual(Descriptor.MapIdentity + TEXT(" TriggerLight control state"),
						TriggerLight->Mode, EWoTTriggerLightMode::Control);
					TestTrue(Descriptor.MapIdentity + TEXT(" TriggerLight replicates"),
						TriggerLight->GetIsReplicated()
							&& TriggerLight->IsNameStableForNetworking());
				}
				if (AWoTAmbientSound* Ambient = Cast<AWoTAmbientSound>(Actor))
				{
					TestFalse(Descriptor.MapIdentity + TEXT(" ambient sound has B1 identity"),
						Ambient->SoundSubPath.IsNone());
					TestFalse(Descriptor.MapIdentity + TEXT(" ambient sound strips export suffix"),
						Ambient->SoundSubPath.ToString().Contains(TEXT("#export-")));
					TestFalse(Descriptor.MapIdentity + TEXT(" ambient is client-local presentation"),
						Ambient->GetIsReplicated());
				}
				if (AWoTDynamicAmbientSound* Dynamic =
					Cast<AWoTDynamicAmbientSound>(Actor))
				{
					TestEqual(Descriptor.MapIdentity + TEXT(" dynamic ambient has six slots"),
						Dynamic->SoundSubPaths.Num(), 6);
				}
				if (AWoTLeafMap* LeafMap = Cast<AWoTLeafMap>(Actor))
				{
					TestEqual(Descriptor.MapIdentity + TEXT(" leaf-map plane width"),
						LeafMap->NodePlanes.Num(), Source.Nodes.Num() * 4);
					TestEqual(Descriptor.MapIdentity + TEXT(" leaf-map child width"),
						LeafMap->NodeChildren.Num(), Source.Nodes.Num() * 2);
					TestEqual(Descriptor.MapIdentity + TEXT(" leaf-map leaf count"),
						LeafMap->NumLeaves, Source.Leaves.Num());
				}
				if (AWoTCorona* Corona = Cast<AWoTCorona>(Actor))
				{
					TestFalse(Descriptor.MapIdentity + TEXT(" corona has B1 skin identity"),
						Corona->SkinSubPath.IsNone());
					TestFalse(Descriptor.MapIdentity + TEXT(" corona strips export suffix"),
						Corona->SkinSubPath.ToString().Contains(TEXT("#export-")));
					TestTrue(Descriptor.MapIdentity + TEXT(" corona leaves are sorted"),
						Algo::IsSorted(Corona->PermeatingLeaves));
				}
				if (AWoTActorRotator* Rotator = Cast<AWoTActorRotator>(Actor))
				{
					TestFalse(Descriptor.MapIdentity + TEXT(" rotator drives projected actors"),
						Rotator->DrivenActors.IsEmpty());
					for (const TSoftObjectPtr<AActor>& Driven : Rotator->DrivenActors)
					{
						AActor* DrivenActor = Driven.Get();
						if (TestNotNull(Descriptor.MapIdentity
							+ TEXT(" rotator target resolves by record identity"), DrivenActor)
							&& DrivenActor->GetRootComponent())
							TestEqual(Descriptor.MapIdentity + TEXT(" rotator target is movable"),
								DrivenActor->GetRootComponent()->Mobility,
								EComponentMobility::Movable);
					}
				}
				if (AWoTLightWave* Wave = Cast<AWoTLightWave>(Actor))
				{
					// The per-map presentation row of this (not yet consumed) census entry.
					const FExpectedRetailPresentationCensus& WaveExpected =
						ExpectedPresentation[FMath::Min(CensusIndex,
							static_cast<int32>(UE_ARRAY_COUNT(ExpectedPresentation)) - 1)];
					TestEqual(Descriptor.MapIdentity + TEXT(" light-wave group count"),
						Wave->Groups.Num(),
						static_cast<int32>(WaveExpected.WaveGroupCount));
					for (int32 Index = 0; Index < Wave->Groups.Num()
						&& Index < static_cast<int32>(WaveExpected.WaveGroupCount);
						++Index)
					{
						TestTrue(FString::Printf(TEXT("%s light-wave group %d is (%u,%u)"),
							*Descriptor.MapIdentity, Index,
							WaveExpected.WaveGroups[Index][0],
							WaveExpected.WaveGroups[Index][1]),
							Wave->Groups[Index].Period
								== static_cast<int32>(WaveExpected.WaveGroups[Index][0])
							&& Wave->Groups[Index].Phase
								== static_cast<int32>(WaveExpected.WaveGroups[Index][1]));
					}
					// Mover/mesh materials read Wave0..3 through the light
					// table's group order; the driver must agree slot for slot.
					if (Projection.MapLightTable)
					{
						int32 TableGroups = 0;
						for (int32 Index = 0; Index < 4; ++Index)
							if (Projection.MapLightTable->WaveAmplitudes[Index] > 0.0f)
								TableGroups = Index + 1;
						TestEqual(Descriptor.MapIdentity + TEXT(" light table waveform groups match the driver"),
							TableGroups, Wave->Groups.Num());
					}
				}
				if (AWoTFogLight* Fog = Cast<AWoTFogLight>(Actor))
				{
					TestTrue(Descriptor.MapIdentity + TEXT(" fog light is runtime-projected"),
						Fog->bRuntimeProjected);
					const FVector Location = Fog->GetActorLocation();
					const FExpectedRetailFogLight* Match = nullptr;
					for (const FExpectedRetailFogLight& Row : ExpectedFogLights)
					{
						if (Descriptor.MapIdentity == Row.Map
							&& FVector::DistSquared(Location, FVector(Row.X, Row.Y, Row.Z))
								< 0.01f)
						{
							Match = &Row;
							break;
						}
					}
					if (TestNotNull(Descriptor.MapIdentity
						+ TEXT(" fog light matches an extracted row at ")
						+ Location.ToString(), Match))
					{
						++MatchedExpectedFogLights;
						const FString Label = Descriptor.MapIdentity + TEXT(" ")
							+ Match->Light;
						TestEqual(Label + TEXT(" fog radius"), Fog->Radius, Match->Radius);
						TestTrue(Label + TEXT(" fog color"),
							FMath::IsNearlyEqual(Fog->FogColor.R, Match->R, 0.00002f)
							&& FMath::IsNearlyEqual(Fog->FogColor.G, Match->G, 0.00002f)
							&& FMath::IsNearlyEqual(Fog->FogColor.B, Match->B, 0.00002f));
						TestTrue(Label + TEXT(" fog VolB"),
							FMath::IsNearlyEqual(Fog->VolBrightness, Match->VolB, 0.00002f));
					}
				}
				if (AWoTRainCyd* Rain = Cast<AWoTRainCyd>(Actor))
				{
					TestTrue(Descriptor.MapIdentity + TEXT(" RainCyd stack is bounded"),
						Rain->StackNum > 0 && Rain->StackNum <= 64);
					TestTrue(Descriptor.MapIdentity + TEXT(" RainCyd fall range is ordered"),
						Rain->MinFallRate >= 0.0f
							&& Rain->MaxFallRate >= Rain->MinFallRate);
					TestTrue(Descriptor.MapIdentity + TEXT(" RainCyd retail cull extents are valid"),
						Rain->VisibilityRadius >= 0.0f
							&& Rain->VisibilityHeight >= 0.0f);
					TestFalse(Descriptor.MapIdentity + TEXT(" RainCyd is client-local presentation"),
						Rain->GetIsReplicated());
				}
				if (AWoTAndilayRoot* Root = Cast<AWoTAndilayRoot>(Actor))
				{
					++ProjectedAndilayRootActors;
					if (Descriptor.MapIdentity == TEXT("Arena_01"))
					{
						TestEqual(TEXT("Arena_01 projected root retains authored AmbientGlow"),
							Root->AmbientGlow, 64.0f);
						TestFalse(TEXT("Arena_01 projected root is not hidden before exposure"),
							Root->IsHidden());
						Root->FinishSpawning(Root->GetActorTransform(), true);
					}
				}
				if (AWoTBattleInfo* BattleInfo = Cast<AWoTBattleInfo>(Actor))
				{
					const FString Prefix = Descriptor.MapIdentity + TEXT(" ")
						+ BattleInfo->GetName();
					TestTrue(Prefix + TEXT(" has a stable projected name"),
						BattleInfo->GetName().StartsWith(TEXT("WoTProjected_"))
							&& BattleInfo->GetName().Contains(TEXT("_BattleInfo")));
					TestTrue(Prefix + TEXT(" replicates"),
						BattleInfo->GetIsReplicated());
					TestTrue(Prefix + TEXT(" is network-addressable"),
						BattleInfo->IsNameStableForNetworking());
					TestTrue(Prefix + TEXT(" is a pre-network startup actor"),
						BattleInfo->IsNetStartupActor());
					const int32 ExpectedTeams = Descriptor.MapIdentity.StartsWith(TEXT("Battle4_"))
						? 4 : Descriptor.MapIdentity.StartsWith(TEXT("Battle3_")) ? 3 : 2;
					TestEqual(Prefix + TEXT(" team count"),
						BattleInfo->NumTeams, ExpectedTeams);
					for (int32 Team = 0; Team < ExpectedTeams; ++Team)
					{
						TestTrue(FString::Printf(TEXT("%s team %d native faction class"),
							*Prefix, Team), BattleInfo->TeamClass[Team]
							== ExpectedBattleTeamClass(Descriptor.MapIdentity, Team));
					}
					if (Descriptor.MapIdentity == TEXT("Battle3_01"))
					{
						TestEqual(Prefix + TEXT(" authored editing string"),
							BattleInfo->EditingStr, FString(TEXT("Prepare your Citadel")));
						TestEqual(Prefix + TEXT(" authored playing string"),
							BattleInfo->PlayingStr, FString(TEXT("Let the battle begin")));
						TestEqual(Prefix + TEXT(" authored ready string"),
							BattleInfo->ReadyStr,
							FString(TEXT("Prepare yourself for battle")));
						TestEqual(Prefix + TEXT(" authored waiting string"),
							BattleInfo->WaitingStr, FString(TEXT("Waiting for players")));
					}
				}
				if (AWoTMashadarGuide* Guide = Cast<AWoTMashadarGuide>(Actor))
				{
					const FString Prefix = Descriptor.MapIdentity + TEXT(" ")
						+ Guide->GetName();
					TestTrue(Prefix + TEXT(" has a stable projected name"),
						Guide->GetName().StartsWith(TEXT("WoTProjected_"))
							&& Guide->GetName().Contains(TEXT("_MashadarGuide")));
					TestTrue(Prefix + TEXT(" replicates"), Guide->GetIsReplicated());
					TestTrue(Prefix + TEXT(" is network-addressable"),
						Guide->IsNameStableForNetworking());
					TestTrue(Prefix + TEXT(" is a pre-network startup actor"),
						Guide->IsNetStartupActor());
					TestTrue(Prefix + TEXT(" is marked citadel-placed"),
						Guide->bCitadelPlaced);
					TestTrue(Prefix + TEXT(" has a finite nonnegative search radius"),
						FMath::IsFinite(Guide->SearchRadius)
							&& Guide->SearchRadius >= 0.0f);
					TestTrue(Prefix + TEXT(" has a finite nonnegative trigger radius"),
						FMath::IsFinite(Guide->TriggerRadius)
							&& Guide->TriggerRadius >= 0.0f);
				}
				if (AWoTSealAltar* Altar = Cast<AWoTSealAltar>(Actor))
				{
					const float Radius = Altar->Touch
						? Altar->Touch->GetUnscaledCapsuleRadius() : 0.0f;
					TestTrue(Descriptor.MapIdentity + TEXT(" altar keeps retail radius"),
						Radius == 48.0f || Radius == 64.0f
							|| Radius == 90.0f || Radius == 140.0f);
					ProjectedAltarRadius48 += Radius == 48.0f ? 1 : 0;
					ProjectedAltarRadius64 += Radius == 64.0f ? 1 : 0;
					ProjectedAltarRadius90 += Radius == 90.0f ? 1 : 0;
					ProjectedAltarRadius140 += Radius == 140.0f ? 1 : 0;
				}
				if (AWoTBudgetInfo* Budget = Cast<AWoTBudgetInfo>(Actor))
				{
					const FString Prefix = Descriptor.MapIdentity + TEXT(" ")
						+ Budget->GetName();
					if (TestEqual(Prefix + TEXT(" has eleven Resource lines"),
						Budget->Budget.Num(), 11))
					{
						const FName FirstTroopAdapter =
							Budget->Budget[1].ResourceAdapterName;
						const FExpectedRetailBudget* ExpectedBudget = nullptr;
						int32 ExpectedBudgetIndex = INDEX_NONE;
						for (int32 BudgetIndex = 0;
							BudgetIndex < static_cast<int32>(UE_ARRAY_COUNT(ExpectedBudgets));
							++BudgetIndex)
						{
							const FExpectedRetailBudget& Candidate =
								ExpectedBudgets[BudgetIndex];
							if (Descriptor.MapIdentity == Candidate.Map
								&& FirstTroopAdapter == FName(Candidate.FirstTroopAdapter)
								&& Budget->Team == Candidate.Team)
							{
								ExpectedBudget = &Candidate;
								ExpectedBudgetIndex = BudgetIndex;
								break;
							}
						}
						if (TestNotNull(Prefix + TEXT(" has a pinned retail budget"),
							ExpectedBudget))
						{
							const bool bReusedExpectedRow =
								ConsumedBudgetRows.Contains(ExpectedBudgetIndex);
							TestFalse(Prefix + TEXT(" does not reuse a pinned budget row"),
								bReusedExpectedRow);
							if (!bReusedExpectedRow)
							{
								ConsumedBudgetRows.Add(ExpectedBudgetIndex);
							}
							TestEqual(Prefix + TEXT(" team"), Budget->Team,
								ExpectedBudget->Team);
							for (int32 Slot = 0; Slot < 11; ++Slot)
							{
								const FWoTBudgetResource& BudgetResource = Budget->Budget[Slot];
								const FName ExpectedPackage = Slot == 0
									? FName(TEXT("WOT"))
									: Slot <= 3 ? FName(TEXT("WOTPawns"))
										: FName(TEXT("WOTTraps"));
								TestEqual(FString::Printf(TEXT("%s slot %d package"),
									*Prefix, Slot), BudgetResource.ResourcePackageName,
									ExpectedPackage);
								TestEqual(FString::Printf(TEXT("%s slot %d adapter"),
									*Prefix, Slot), BudgetResource.ResourceAdapterName,
									ExpectedBudgetAdapter(FirstTroopAdapter, Slot));
								TestEqual(FString::Printf(TEXT("%s slot %d count"),
									*Prefix, Slot), BudgetResource.ItemCount,
									ExpectedBudget->ItemCounts[Slot]);
							}
							TestEqual(*(Prefix + TEXT(" begin-edit event")),
								Budget->BeginEditEvent, FName(NAME_None));
							FName ExpectedEndEditEvent = NAME_None;
							if (Descriptor.MapIdentity == TEXT("Battle3_02"))
							{
								if (FirstTroopAdapter == FName(TEXT("WarderInventory")))
									ExpectedEndEditEvent = FName(TEXT("as_ready"));
								else if (FirstTroopAdapter == FName(TEXT("TrollocInventory")))
									ExpectedEndEditEvent = FName(TEXT("for_ready"));
								else if (FirstTroopAdapter == FName(TEXT("MinionInventory")))
									ExpectedEndEditEvent = FName(TEXT("mor_ready"));
							}
							TestEqual(Prefix + TEXT(" end-edit event"),
								Budget->EndEditEvent, ExpectedEndEditEvent);
						}
					}
				}
				AWoTMover* Mover = Cast<AWoTMover>(Actor);
				if (!Mover) continue;
				const FString Prefix = Descriptor.MapIdentity + TEXT(" ") + Mover->GetName();
				TestTrue(Prefix + TEXT(" has a stable projected name"),
					Mover->GetName().StartsWith(TEXT("WoTProjected_"))
						&& Mover->GetName().Contains(TEXT("_Mover")));
				TestTrue(Prefix + TEXT(" replicates"), Mover->GetIsReplicated());
				TestTrue(Prefix + TEXT(" is network-addressable"),
					Mover->IsNameStableForNetworking());
				TestTrue(Prefix + TEXT(" is a pre-network startup actor"),
					Mover->IsNetStartupActor());
				TestNotNull(Prefix + TEXT(" owns its generated mesh"),
					Mover->RuntimeMesh.Get());
				TestNotNull(Prefix + TEXT(" has its mesh component"),
					Mover->Mesh.Get());
				if (Mover->RuntimeMesh && Mover->Mesh)
				{
					TestTrue(Prefix + TEXT(" component uses its generated mesh"),
						Mover->Mesh->GetStaticMesh() == Mover->RuntimeMesh.Get());
					TestEqual(Prefix + TEXT(" has one MID per plane section"),
						Mover->RuntimeMaterials.Num(),
						Mover->RuntimeMesh->GetStaticMaterials().Num());
				}
				TestTrue(Prefix + TEXT(" retains at least the neutral texture"),
					!Mover->RuntimeTextures.IsEmpty());
				const FString ProjectedName = Mover->GetName();
				const int32 RecordStart = ProjectedName.Find(TEXT("_"));
				const int32 RecordEnd = ProjectedName.Find(TEXT("_"),
					ESearchCase::CaseSensitive, ESearchDir::FromStart, RecordStart + 1);
				const int32 RecordIndex = RecordStart != INDEX_NONE
					&& RecordEnd > RecordStart + 1
					? FCString::Atoi(*ProjectedName.Mid(
						RecordStart + 1, RecordEnd - RecordStart - 1)) : INDEX_NONE;
				const FWoTRuntimeMoverModel* RuntimeModel = nullptr;
				if (Source.Records.IsValidIndex(RecordIndex))
				{
					for (const FWoTRuntimeMoverModel& Candidate : Source.MoverModels)
					{
						if (Candidate.ActorIdentity.Equals(
								Source.Records[RecordIndex].Identity,
								ESearchCase::IgnoreCase))
						{
							RuntimeModel = &Candidate;
							break;
						}
					}
				}
				TestNotNull(Prefix + TEXT(" resolves its authenticated mover model"),
					RuntimeModel);
				if (RuntimeModel && Mover->RuntimeMesh && Mover->Mesh)
				{
					const bool bAuthenticatedCollisionIsEmpty =
						RuntimeModel->CollisionPositions.IsEmpty()
						&& RuntimeModel->CollisionIndices.IsEmpty();
					UBodySetup* Body = Mover->RuntimeMesh->GetBodySetup();
					if (Source.bHasDedicatedMoverCollision
						&& bAuthenticatedCollisionIsEmpty)
					{
						TestNull(Prefix + TEXT(" authoritative empty collision has no body"), Body);
						TestEqual(Prefix + TEXT(" authoritative empty collision is disabled"),
							Mover->Mesh->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
					}
					else if (TestNotNull(Prefix + TEXT(" has cooked collision"), Body))
					{
						TestEqual(Prefix + TEXT(" uses complex-as-simple collision"),
							Body->CollisionTraceFlag, CTF_UseComplexAsSimple);
					}
				}
				if (RuntimeModel
					&& RuntimeModel->ActorIdentity == TEXT("Battle2_01.Mover9"))
				{
					TestTrue(TEXT("Battle2_01.Mover9 authenticated collision stream is empty"),
						RuntimeModel->CollisionPositions.IsEmpty()
							&& RuntimeModel->CollisionIndices.IsEmpty());
					FinishProjected(Mover);
					TestEqual(TEXT("Battle2_01.Mover9 has collision disabled"),
						Mover->Mesh->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
					TestEqual(TEXT("Battle2_01.Mover9 ignores visibility traces"),
						Mover->Mesh->GetCollisionResponseToChannel(ECC_Visibility), ECR_Ignore);
					const FStaticMeshRenderData* RenderData =
						Mover->RuntimeMesh ? Mover->RuntimeMesh->GetRenderData() : nullptr;
					if (TestTrue(TEXT("Battle2_01.Mover9 has a render witness triangle"),
						RenderData && !RenderData->LODResources.IsEmpty()
							&& RenderData->LODResources[0].IndexBuffer.GetNumIndices() >= 3))
					{
						const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
						const uint32 IA = LOD.IndexBuffer.GetIndex(0);
						const uint32 IB = LOD.IndexBuffer.GetIndex(1);
						const uint32 IC = LOD.IndexBuffer.GetIndex(2);
						const FPositionVertexBuffer& Positions =
							LOD.VertexBuffers.PositionVertexBuffer;
						if (TestTrue(TEXT("Battle2_01.Mover9 witness indices are valid"),
							IA < Positions.GetNumVertices() && IB < Positions.GetNumVertices()
								&& IC < Positions.GetNumVertices()))
						{
							const FTransform& ComponentToWorld =
								Mover->Mesh->GetComponentTransform();
							const FVector A = ComponentToWorld.TransformPosition(
								FVector(Positions.VertexPosition(IA)));
							const FVector B = ComponentToWorld.TransformPosition(
								FVector(Positions.VertexPosition(IB)));
							const FVector C = ComponentToWorld.TransformPosition(
								FVector(Positions.VertexPosition(IC)));
							const FVector Center = (A + B + C) / 3.0;
							const FVector Normal = FVector::CrossProduct(B - A, C - A)
								.GetSafeNormal();
							TestFalse(TEXT("Battle2_01.Mover9 witness is nondegenerate"),
								Normal.IsNearlyZero());
							const FVector Start = Center + Normal * 96.0;
							const FVector End = Center - Normal * 96.0;
							TArray<FHitResult> PointHits;
							World->LineTraceMultiByChannel(PointHits, Start, End,
								ECC_Visibility,
								FCollisionQueryParams(SCENE_QUERY_STAT(
									RuntimeMoverRealNotSolidPoint), true));
							TestFalse(TEXT("Battle2_01.Mover9 PF_NotSolid point trace does not hit mover"),
								PointHits.ContainsByPredicate([Mover](const FHitResult& Hit)
								{
									return Hit.GetActor() == Mover;
								}));
							TArray<FHitResult> PawnHits;
							World->SweepMultiByChannel(PawnHits, Start, End, FQuat::Identity,
								ECC_Pawn, FCollisionShape::MakeCapsule(17.0f, 34.0f),
								FCollisionQueryParams(SCENE_QUERY_STAT(
									RuntimeMoverRealNotSolidPawn), true));
							TestFalse(TEXT("Battle2_01.Mover9 PF_NotSolid pawn sweep does not hit mover"),
								PawnHits.ContainsByPredicate([Mover](const FHitResult& Hit)
								{
									return Hit.GetActor() == Mover;
								}));
							++ProductionNotSolidMoverTraceWitnesses;
						}
					}
				}
				const bool bDynamicLighting = Mover->MeshLight
					&& !Mover->MeshLight->MoverSurfaces.IsEmpty();
				for (int32 MaterialIndex = 0;
					MaterialIndex < Mover->RuntimeMaterials.Num(); ++MaterialIndex)
				{
					UMaterialInstanceDynamic* Material =
						Mover->RuntimeMaterials[MaterialIndex];
					if (!TestNotNull(Prefix + TEXT(" owns every plane MID"), Material))
						continue;
					if (bDynamicLighting)
					{
						const UMaterial* Base = Material->GetMaterial();
						const FString BaseName = Base ? Base->GetName() : FString();
						TestTrue(Prefix + TEXT(" keeps its dynamic-mover master"),
							BaseName == TEXT("M_WoT_MoverLit")
								|| BaseName == TEXT("M_WoT_MoverLitMasked")
								|| BaseName == TEXT("M_WoT_MeshLitT"));
						continue;
					}
					if (RuntimeModel == nullptr
						|| !RuntimeModel->Sections.IsValidIndex(MaterialIndex)) continue;
					const FWoTRuntimeMoverSection& Section =
						RuntimeModel->Sections[MaterialIndex];
					FWoTRuntimeBSPSectionStyle Style;
					const bool bNullTexture = Section.CanonicalPackage.IsEmpty()
						&& (Section.CanonicalName.IsEmpty()
							|| Section.CanonicalName.Equals(
								TEXT("None"), ESearchCase::IgnoreCase))
						&& (Section.SourceTextureIdentity.IsEmpty()
							|| Section.SourceTextureIdentity.Equals(
								TEXT("None"), ESearchCase::IgnoreCase));
					if (!bNullTexture
						&& !TestTrue(Prefix + TEXT(" derives its authenticated section style"),
							WoTRuntimeBSPSectionStyleForIdentity(
								Section.CanonicalPackage, Section.CanonicalName,
								Section.SurfaceFlags, Style))) continue;
					UTexture* BoundSurfaceTexture = nullptr;
					Material->GetTextureParameterValue(
						FHashedMaterialParameterInfo(TEXT("Tex")),
						BoundSurfaceTexture);
					WoTRuntimeBSPApplyTextureFlags(Style,
						UWoTRuntimeAssets::ResolveTextureMasked(
							Cast<UTexture2D>(BoundSurfaceTexture)));
					if (Section.bAnimatedLightmap)
					{
						const FString ParentName = Material->Parent
							? Material->Parent->GetName() : FString();
						TestTrue(Prefix + TEXT(" uses the shared animated-world response graph"),
							ParentName.StartsWith(TEXT("MI_WoT_MoverWaveRuntime")));
						for (int32 ResponseIndex = 0; ResponseIndex < 6; ++ResponseIndex)
						{
							const bool bUsed =
								Source.MoverAnimationTerms.IsValidIndex(ResponseIndex);
							const FWoTRuntimeMoverAnimationTerm* Term = bUsed
								? &Source.MoverAnimationTerms[ResponseIndex] : nullptr;
							#if WITH_EDITORONLY_DATA
							bool bUseDelta = false;
							FGuid SwitchGuid;
							TestTrue(Prefix + FString::Printf(
								TEXT(" exposes generated UseDelta%d"), ResponseIndex),
								Material->Parent
								&& Material->Parent->GetStaticSwitchParameterValue(
									FHashedMaterialParameterInfo(*FString::Printf(
										TEXT("UseDelta%d"), ResponseIndex)),
									bUseDelta, SwitchGuid));
							TestTrue(Prefix + FString::Printf(
								TEXT(" compiles generated UseDelta%d on"), ResponseIndex),
								bUseDelta);
							#endif
							UTexture* BoundResponse = nullptr;
							TestTrue(Prefix + TEXT(" exposes mover Delta response"),
								Material->GetTextureParameterValue(
									FHashedMaterialParameterInfo(*FString::Printf(
										TEXT("Delta%d"), ResponseIndex)), BoundResponse));
							TestEqual(Prefix + TEXT(" binds immutable mover response atlas"),
								BoundResponse,
								static_cast<UTexture*>(bUsed
									? MoverResponseTextures[ResponseIndex]
									: Source.MoverBlackLightResponse));
							float BoundWaveIndex = -2.0f;
							float BoundEffectSalt = -1.0f;
							TestTrue(Prefix + TEXT(" exposes mover Delta wave index"),
								Material->GetScalarParameterValue(
									FHashedMaterialParameterInfo(*FString::Printf(
										TEXT("DeltaWaveIndex%d"), ResponseIndex)),
									BoundWaveIndex));
							TestTrue(Prefix + TEXT(" exposes mover Delta effect salt"),
								Material->GetScalarParameterValue(
									FHashedMaterialParameterInfo(*FString::Printf(
										TEXT("DeltaEffectSalt%d"), ResponseIndex)),
									BoundEffectSalt));
							TestEqual(Prefix + TEXT(" binds exact world-v6 mover wave index"),
								FMath::RoundToInt(BoundWaveIndex),
								Term ? Term->Group : INDEX_NONE);
							TestEqual(Prefix + TEXT(" binds exact world-v6 mover effect salt"),
								FMath::RoundToInt(BoundEffectSalt),
								Term ? static_cast<int32>(Term->Effect) : 0);
						}

						const bool bWaveWitness =
							Descriptor.MapIdentity == TEXT("Battle2_02");
						const bool bEffectWitness =
							Descriptor.MapIdentity == TEXT("Battle2_05");
						if ((bWaveWitness || bEffectWitness)
							&& !ProductionMoverBindingWitnesses.Contains(
								Descriptor.MapIdentity))
						{
							int32 WitnessTermIndex = INDEX_NONE;
							for (int32 TermIndex = 0;
								TermIndex < Source.MoverAnimationTerms.Num(); ++TermIndex)
							{
								const FWoTRuntimeMoverAnimationTerm& Candidate =
									Source.MoverAnimationTerms[TermIndex];
								if ((bWaveWitness && Candidate.Group != INDEX_NONE)
									|| (bEffectWitness && Candidate.Effect != 0))
								{
									WitnessTermIndex = TermIndex;
									break;
								}
							}
							if (!TestTrue(Descriptor.MapIdentity
								+ TEXT(" has the required production mover term"),
								WitnessTermIndex != INDEX_NONE)) continue;
							const FWoTRuntimeMoverAnimationTerm& WitnessTerm =
								Source.MoverAnimationTerms[WitnessTermIndex];
							int32 ResponseByte = INDEX_NONE;
							for (int32 ByteIndex = 0;
								ByteIndex < WitnessTerm.ResponseBGRA.Num(); ++ByteIndex)
							{
								if ((ByteIndex & 3) != 3
									&& WitnessTerm.ResponseBGRA[ByteIndex] != 0)
								{
									ResponseByte = ByteIndex;
									break;
								}
							}
							if (!TestTrue(Descriptor.MapIdentity
								+ TEXT(" production response has a nonzero color texel"),
								ResponseByte != INDEX_NONE)) continue;
							ProductionMoverBindingWitnesses.Add(Descriptor.MapIdentity);
						}
					}
					else
					{
						TestTrue(Prefix + TEXT(" uses the exact static world material master"),
							Material->Parent == WoTWorldRuntimeBSPMaster(Style, false));
					}
					UTexture* BoundLightmap = nullptr;
					TestTrue(Prefix + TEXT(" exposes the mover Lightmap parameter"),
						Material->GetTextureParameterValue(
							FHashedMaterialParameterInfo(TEXT("Lightmap")), BoundLightmap));
					TestEqual(Prefix + TEXT(" binds the shared mover atlas"),
						BoundLightmap, static_cast<UTexture*>(Source.MoverLightmap));
					for (const TCHAR* Parameter : {
						TEXT("PanU"), TEXT("PanV"), TEXT("WoTSceneTime")})
					{
						float Value = -1.0f;
						TestTrue(Prefix + TEXT(" exposes zeroed ") + Parameter,
							Material->GetScalarParameterValue(
								FHashedMaterialParameterInfo(Parameter), Value));
						TestEqual(Prefix + TEXT(" zeroes ") + Parameter, Value, 0.0f);
					}
					float DynLightEnable = -1.0f;
					TestTrue(Prefix + TEXT(" exposes DynLightEnable"),
						Material->GetScalarParameterValue(
							FHashedMaterialParameterInfo(TEXT("DynLightEnable")),
							DynLightEnable));
					TestEqual(Prefix + TEXT(" keeps special-lit isolation"),
						DynLightEnable, Style.bSpecialLit ? 0.0f : 1.0f);
					const bool bMacroMaster = Style.Blend == EWoTRuntimeBSPBlend::Opaque
						|| Style.Blend == EWoTRuntimeBSPBlend::Masked;
					if (bMacroMaster && !Section.MacroCanonicalPackage.IsEmpty()
						&& !Section.MacroCanonicalName.IsEmpty()
						&& !Section.MacroSourceTextureIdentity.IsEmpty()
						&& Section.MacroRatio > 0.0f)
					{
						++TotalBakedMacroMoverSections;
						FWoTRetailObjectPath MacroIdentity;
						FWoTRetailObjectPath MacroSource;
						if (TestTrue(Prefix + TEXT(" macro source identity parses"),
							WoTParseRetailObjectPath(
								Section.MacroSourceTextureIdentity, MacroSource)))
						{
							MacroIdentity.Groups = MoveTemp(MacroSource.Groups);
						}
						MacroIdentity.Package = Section.MacroCanonicalPackage;
						MacroIdentity.Name = Section.MacroCanonicalName;
						const FString ExpectedMacroPath = FString::Printf(
							TEXT("/Game/WoT/Textures/%s.%s"),
							*WoTRetailTextureSubPath(MacroIdentity),
							*Section.MacroCanonicalName);
						UTexture* BoundMacro = nullptr;
						if (TestTrue(Prefix + TEXT(" exposes the baked mover MacroTex"),
							Material->GetTextureParameterValue(
								FHashedMaterialParameterInfo(TEXT("MacroTex")),
								BoundMacro)))
						{
							if (TestNotNull(Prefix + TEXT(" binds a baked mover macro texture"),
								BoundMacro))
							{
								TestEqual(Prefix + TEXT(" binds the exact baked mover macro asset"),
									BoundMacro->GetPathName(), ExpectedMacroPath);
								TestTrue(Prefix + TEXT(" retains the baked mover macro asset"),
									Mover->RuntimeTextures.Contains(
										Cast<UTexture2D>(BoundMacro)));
							}
						}
						float BoundMacroRatio = 0.0f;
						TestTrue(Prefix + TEXT(" exposes the baked mover MacroRatio"),
							Material->GetScalarParameterValue(
								FHashedMaterialParameterInfo(TEXT("MacroRatio")),
								BoundMacroRatio));
						TestEqual(Prefix + TEXT(" binds the baked mover macro ratio"),
							BoundMacroRatio, Section.MacroRatio);
					}
				}
				if (bDynamicLighting)
				{
					++TotalDynamicMovers;
					TestFalse(Prefix + TEXT(" dynamic lighting is not static-bake mode"),
						Mover->MeshLight->bStaticMoverSurfaceLighting);
					TestEqual(Prefix + TEXT(" keeps one dynamic light plane per MID"),
						Mover->MeshLight->MoverSurfaces.Num(),
						Mover->RuntimeMaterials.Num());
				}
				else
				{
					++TotalStaticMovers;
					TestTrue(Prefix + TEXT(" has authenticated baked-lightmap metadata"),
						RuntimeModel && RuntimeModel->bLightmapped);
					TestTrue(Prefix + TEXT(" disables the legacy pivot bake"),
						Mover->MeshLight
							&& !Mover->MeshLight->bStaticMoverSurfaceLighting);
					TestTrue(Prefix + TEXT(" retains the shared mover atlas"),
						Mover->RuntimeTextures.Contains(Source.MoverLightmap));
				}
			}
			if (Descriptor.MapIdentity == TEXT("Battle2_02"))
				TestTrue(TEXT("Battle2_02 throne door applies the retail PrePivot frame"),
					bThroneDoorFramed);
		}
		if (Descriptor.MapIdentity == TEXT("Arena_01"))
		{
			TestEqual(TEXT("Arena_01 exposes all four native root actors"),
				ProjectedAndilayRootActors, 4u);
		}
		if (bProjected && CensusIndex < static_cast<int32>(UE_ARRAY_COUNT(ExpectedCache)))
		{
			const FExpectedCacheFormatCensus& CacheExpected=ExpectedCache[CensusIndex];
			const UWoTMapLightTable* Table=Projection.MapLightTable.Get();
			if (TestNotNull(Descriptor.MapIdentity+TEXT(" complete runtime light table"),Table))
			{
				TestFalse(Descriptor.MapIdentity+TEXT(" light table is not partial"),
					Projection.bLightTablePartial);
				TestEqual(Descriptor.MapIdentity+TEXT(" runtime light rows"),
					Table->Lights.Num(),static_cast<int32>(CacheExpected.Lights));
				TestEqual(Descriptor.MapIdentity+TEXT(" runtime leaf CSR offsets"),
					Table->LeafLightOffsets.Num(),static_cast<int32>(CacheExpected.Leaves+1));
				TestEqual(Descriptor.MapIdentity+TEXT(" runtime leaf CSR indices"),
					Table->LeafLightIndices.Num(),static_cast<int32>(CacheExpected.LightReferences));
				TestEqual(Descriptor.MapIdentity+TEXT(" runtime friction-node indices"),
					Table->FrictionNodeIndices.Num(),static_cast<int32>(CacheExpected.FrictionNodes));
				TestEqual(Descriptor.MapIdentity+TEXT(" runtime friction-node values"),
					Table->FrictionNodeValues.Num(),static_cast<int32>(CacheExpected.FrictionNodes));
				int32 ZeroFrictionNodes=0;
				int32 ClimbFrictionNodes=0;
				for (float Friction:Table->FrictionNodeValues)
				{
					ZeroFrictionNodes+=Friction==0.0f ? 1 : 0;
					ClimbFrictionNodes+=Friction==10.0f ? 1 : 0;
				}
				TestEqual(Descriptor.MapIdentity+TEXT(" slippery friction-zero nodes"),
					ZeroFrictionNodes,static_cast<int32>(CacheExpected.ZeroFrictionNodes));
				TestEqual(Descriptor.MapIdentity+TEXT(" climbable friction-ten nodes"),
					ClimbFrictionNodes,static_cast<int32>(
						CacheExpected.FrictionNodes-CacheExpected.ZeroFrictionNodes));
				TestTrue(Descriptor.MapIdentity+TEXT(" friction indices are ascending"),
					Algo::IsSorted(Table->FrictionNodeIndices));
				TotalLightRows+=Table->Lights.Num();
			}
		}
		uint32 SkipCount = 0;
		for (const TPair<FName, uint32>& Pair : Projection.SkippedRuleCounts)
			SkipCount += Pair.Value;
		for (const FString& Sample : Projection.SkippedSamples)
			AddInfo(Descriptor.MapIdentity + TEXT(" RETAIL_PROJECTION_SKIP ") + Sample);
		for (const TPair<FName, uint32>& Pair : Projection.UnsupportedClassCounts)
			AddInfo(FString::Printf(TEXT("RETAIL_PROJECTION_UNSUPPORTED map=%s class=%s count=%u"),
				*Descriptor.MapIdentity, *Pair.Key.ToString(), Pair.Value));
		if (CensusIndex >= UE_ARRAY_COUNT(ExpectedCensus))
		{
			AddError(TEXT("native importer returned more than nineteen world descriptors"));
			WoTTest::DestroySpecWorld(World);
			continue;
		}
		const FExpectedRetailProjectionCensus& Expected = ExpectedCensus[CensusIndex++];
		const FExpectedRetailPresentationCensus& PresentationExpected =
			ExpectedPresentation[CensusIndex - 1];
		TestEqual(Descriptor.MapIdentity + TEXT(" census order"),
			Descriptor.MapIdentity, FString(Expected.Map));
		TestEqual(Descriptor.MapIdentity + TEXT(" presentation census order"),
			Descriptor.MapIdentity, FString(PresentationExpected.Map));
		TestEqual(Descriptor.MapIdentity + TEXT(" ambient-sound census"),
			Projection.RuleCount(TEXT("AmbientSound")),
			PresentationExpected.AmbientSound);
		TestEqual(Descriptor.MapIdentity + TEXT(" dynamic-ambient census"),
			Projection.RuleCount(TEXT("DynamicAmbientSound")),
			PresentationExpected.DynamicAmbientSound);
		TestEqual(Descriptor.MapIdentity + TEXT(" authored water-sound-zone census"),
			Projection.RuleCount(TEXT("WaterSoundZone")),
			PresentationExpected.WaterSoundZone);
		TestEqual(Descriptor.MapIdentity + TEXT(" corona census"),
			Projection.RuleCount(TEXT("Corona")), PresentationExpected.Corona);
		TestEqual(Descriptor.MapIdentity + TEXT(" leaf-map census"),
			Projection.RuleCount(TEXT("LeafMap")), PresentationExpected.LeafMap);
		TestEqual(Descriptor.MapIdentity + TEXT(" actor-rotator census"),
			Projection.RuleCount(TEXT("ActorRotator")),
			PresentationExpected.ActorRotator);
		TestEqual(Descriptor.MapIdentity + TEXT(" RainCyd census"),
			Projection.RuleCount(TEXT("RainCyd")), PresentationExpected.RainCyd);
		FExpectedNewRuleCensus NewExpected = {TEXT(""),0,0,0,0,0,0,0,0};
		for (const FExpectedNewRuleCensus& Candidate : ExpectedNewRules)
		{
			if (Descriptor.MapIdentity == Candidate.Map) { NewExpected=Candidate; break; }
		}
		TestEqual(Descriptor.MapIdentity + TEXT(" PitOpening census"),
			Projection.RuleCount(TEXT("PitOpening")), NewExpected.PitOpening);
		TestEqual(Descriptor.MapIdentity + TEXT(" Staircase census"),
			Projection.RuleCount(TEXT("Staircase")), NewExpected.Staircase);
		TestEqual(Descriptor.MapIdentity + TEXT(" SpecialEventTrigger census"),
			Projection.RuleCount(TEXT("SpecialEventTrigger")), NewExpected.SpecialEventTrigger);
		TestEqual(Descriptor.MapIdentity + TEXT(" SpecialEvent census"),
			Projection.RuleCount(TEXT("SpecialEvent")), NewExpected.SpecialEvent);
		TestEqual(Descriptor.MapIdentity + TEXT(" SpecialEffectsTrigger census"),
			Projection.RuleCount(TEXT("SpecialEffectsTrigger")), NewExpected.SpecialEffectsTrigger);
		TestEqual(Descriptor.MapIdentity + TEXT(" StochasticTrigger census"),
			Projection.RuleCount(TEXT("StochasticTrigger")), NewExpected.StochasticTrigger);
		TestEqual(Descriptor.MapIdentity + TEXT(" TriggerLight census"),
			Projection.RuleCount(TEXT("TriggerLight")), NewExpected.TriggerLight);
		TestEqual(Descriptor.MapIdentity + TEXT(" BlockMonsters census"),
			Projection.RuleCount(TEXT("BlockMonsters")), NewExpected.BlockMonsters);
		const uint32 ExpectedWalls =
			Descriptor.MapIdentity == TEXT("Battle2_05")
			|| Descriptor.MapIdentity == TEXT("Battle4_01") ? 3u : 0u;
		const uint32 ExpectedDestroyDirectives =
			Descriptor.MapIdentity == TEXT("Battle3_02") ? 3u : 0u;
		TestEqual(Descriptor.MapIdentity + TEXT(" WOTExplodingWall census"),
			Projection.RuleCount(TEXT("ExplodingWall")), ExpectedWalls);
		TestEqual(Descriptor.MapIdentity + TEXT(" DestroyDirective census"),
			Projection.RuleCount(TEXT("DestroyDirective")), ExpectedDestroyDirectives);
		TestEqual(Descriptor.MapIdentity + TEXT(" fog-light census"),
			Projection.RuleCount(TEXT("FogLight")), PresentationExpected.FogLight);
		TestEqual(Descriptor.MapIdentity + TEXT(" light-wave census"),
			Projection.RuleCount(TEXT("LightWave")), PresentationExpected.LightWave);
		TestEqual(Descriptor.MapIdentity + TEXT(" PlayerStart census"),
			Projection.RuleCount(TEXT("PlayerStart")), Expected.PlayerStart);
		TestEqual(Descriptor.MapIdentity + TEXT(" SpawnPool census"),
			Projection.RuleCount(TEXT("SpawnPool")), Expected.SpawnPool);
		TestEqual(Descriptor.MapIdentity + TEXT(" Zone census"),
			Projection.RuleCount(TEXT("Zone")), Expected.Zone);
		TestEqual(Descriptor.MapIdentity + TEXT(" Light census"),
			Projection.RuleCount(TEXT("Light")), Expected.Light);
		TestEqual(Descriptor.MapIdentity + TEXT(" SkyLight census"),
			Projection.RuleCount(TEXT("SkyLight")), Expected.SkyLight);
		TestEqual(Descriptor.MapIdentity + TEXT(" NavBounds census"),
			Projection.RuleCount(TEXT("NavBounds")), Expected.NavBounds);
		TestEqual(Descriptor.MapIdentity + TEXT(" PathNode census"),
			Projection.RuleCount(TEXT("PathNode")), Expected.PathNode);
		TestEqual(Descriptor.MapIdentity + TEXT(" ParticleSprayer census"),
			Projection.RuleCount(TEXT("ParticleSprayer")), Expected.ParticleSprayer);
		TestEqual(Descriptor.MapIdentity + TEXT(" fixed-pickup census"),
			Projection.RuleCount(TEXT("FixedPickup")), Expected.FixedPickup);
		TestEqual(Descriptor.MapIdentity + TEXT(" Andilay-root census"),
			Projection.RuleCount(TEXT("AndilayRoot")), Expected.AndilayRoot);
		TestEqual(Descriptor.MapIdentity + TEXT(" decoration census"),
			Projection.RuleCount(TEXT("Decoration")), Expected.Decoration);
		TestEqual(Descriptor.MapIdentity + TEXT(" breakable-decoration census"),
			Projection.RuleCount(TEXT("BreakableDecoration")),
			Expected.BreakableDecoration);
		TestEqual(Descriptor.MapIdentity + TEXT(" mover census"),
			Projection.RuleCount(TEXT("Mover")), Expected.Mover);
		TestEqual(Descriptor.MapIdentity + TEXT(" trigger census"),
			Projection.RuleCount(TEXT("Trigger")), Expected.Trigger);
		TestEqual(Descriptor.MapIdentity + TEXT(" door-trigger census"),
			Projection.RuleCount(TEXT("DoorTrigger")), Expected.DoorTrigger);
		TestEqual(Descriptor.MapIdentity + TEXT(" dispatcher census"),
			Projection.RuleCount(TEXT("Dispatcher")), Expected.Dispatcher);
		TestEqual(Descriptor.MapIdentity + TEXT(" counter census"),
			Projection.RuleCount(TEXT("Counter")), Expected.Counter);
		TestEqual(Descriptor.MapIdentity + TEXT(" sound-dispatcher census"),
			Projection.RuleCount(TEXT("SoundDispatcher")), Expected.SoundDispatcher);
		TestEqual(Descriptor.MapIdentity + TEXT(" portcullis-trigger census"),
			Projection.RuleCount(TEXT("Portcullis")), Expected.Portcullis);
		TestEqual(Descriptor.MapIdentity + TEXT(" teleporter census"),
			Projection.RuleCount(TEXT("Teleporter")), Expected.Teleporter);
		TestEqual(Descriptor.MapIdentity + TEXT(" jumper census"),
			Projection.RuleCount(TEXT("Jumper")), Expected.Jumper);
		TestEqual(Descriptor.MapIdentity + TEXT(" BlockAll census"),
			Projection.RuleCount(TEXT("BlockAll")), Expected.BlockAll);
		TestEqual(Descriptor.MapIdentity + TEXT(" BlockPlayer census"),
			Projection.RuleCount(TEXT("BlockPlayer")), Expected.BlockPlayer);
		TestEqual(Descriptor.MapIdentity + TEXT(" BattleInfo census"),
			Projection.RuleCount(TEXT("BattleInfo")), Expected.BattleInfo);
		TestEqual(Descriptor.MapIdentity + TEXT(" SealAltar census"),
			Projection.RuleCount(TEXT("SealAltar")), Expected.SealAltar);
		TestEqual(Descriptor.MapIdentity + TEXT(" budget census"),
			Projection.RuleCount(TEXT("Budget")), Expected.Budget);
		TestEqual(Descriptor.MapIdentity + TEXT(" MashadarGuide census"),
			Projection.RuleCount(TEXT("MashadarGuide")), Expected.MashadarGuide);
		if (Expected.SealAltar > 0)
		{
			const FExpectedAltarRadiusCensus* ExpectedRadii = nullptr;
			for (const FExpectedAltarRadiusCensus& Candidate : ExpectedAltarRadii)
			{
				if (Descriptor.MapIdentity == Candidate.Map)
				{
					ExpectedRadii = &Candidate;
					break;
				}
			}
			if (TestNotNull(Descriptor.MapIdentity + TEXT(" altar-radius census pin"),
				ExpectedRadii))
			{
				TestEqual(Descriptor.MapIdentity + TEXT(" 48-radius altar census"),
					ProjectedAltarRadius48, ExpectedRadii->Radius48);
				TestEqual(Descriptor.MapIdentity + TEXT(" 64-radius altar census"),
					ProjectedAltarRadius64, ExpectedRadii->Radius64);
				TestEqual(Descriptor.MapIdentity + TEXT(" 90-radius altar census"),
					ProjectedAltarRadius90, ExpectedRadii->Radius90);
				TestEqual(Descriptor.MapIdentity + TEXT(" 140-radius altar census"),
					ProjectedAltarRadius140, ExpectedRadii->Radius140);
			}
		}
		TestEqual(Descriptor.MapIdentity + TEXT(" skipped actor census"),
			SkipCount, Expected.Skips);
		if (Descriptor.MapIdentity == TEXT("Arena_09")
			|| Descriptor.MapIdentity == TEXT("Battle2_03")
			|| Descriptor.MapIdentity == TEXT("Battle2_05")
			|| Descriptor.MapIdentity == TEXT("Battle3_01"))
		{
			TestTrue(Descriptor.MapIdentity
				+ TEXT(" all formerly skipped live particle records now project"),
				Projection.SkippedSamples.IsEmpty());
		}
		AddInfo(FString::Printf(TEXT("RETAIL_PROJECTION_SKIPS map=%s samples=%s"),
			*Descriptor.MapIdentity,
			Projection.SkippedSamples.IsEmpty() ? TEXT("-")
				: *FString::Join(Projection.SkippedSamples, TEXT(","))));
		if (Descriptor.MapIdentity.StartsWith(TEXT("Battle")))
		{
			// Citadel EditZoneAt on the real projection: every team's altar
			// stands in that team's edit zone, which is what CitadelBeginEdit
			// requires of a player entering the editor from their citadel.
			TArray<AWoTSealAltar*> CitadelAltars;
			for (AActor* Actor : Projection.DeferredActors)
			{
				if (AWoTZoneVolume* Zone = Cast<AWoTZoneVolume>(Actor))
				{
					if (!Zone->HasActorBegunPlay())
					{
						Zone->FinishSpawning(Zone->GetActorTransform(), true);
					}
					if (!Zone->HasActorBegunPlay()) Zone->DispatchBeginPlay();
				}
				else if (AWoTSealAltar* Altar = Cast<AWoTSealAltar>(Actor))
				{
					CitadelAltars.Add(Altar);
				}
			}
			for (const AWoTSealAltar* Altar : CitadelAltars)
			{
				const AWoTZoneVolume* EditZone =
					AWoTZoneVolume::EditZoneAt(World, Altar->GetActorLocation());
				TestTrue(FString::Printf(TEXT("%s team %d altar stands in its team's edit zone (%s)"),
					*Descriptor.MapIdentity, static_cast<int32>(Altar->Team),
					EditZone ? *FString::Printf(TEXT("%s team %d"), *EditZone->GetName(),
						static_cast<int32>(EditZone->EditTeam)) : TEXT("none")),
					IsValid(EditZone) && EditZone->EditTeam == Altar->Team);
				bEditZoneAtCovered = true;
			}
			TestTrue(Descriptor.MapIdentity + TEXT(" exercises EditZoneAt"), bEditZoneAtCovered);
		}
		TestEqual(Descriptor.MapIdentity + TEXT(" projects its DecalMan policy actor"),
			Projection.RuleCount(TEXT("DecalManager")), 1u);
		for (const AActor* Actor : Projection.DeferredActors)
		{
			if (const AWoTZoneVolume* Zone = Cast<AWoTZoneVolume>(Actor);
				Zone && Zone->bWaterZone)
			{
				int32 RecordIndex = INDEX_NONE;
				for (int32 Candidate = 0; Candidate < Source.Records.Num(); ++Candidate)
				{
					const FString Prefix = FString::Printf(
						TEXT("WoTProjected_%d_"), Candidate);
					if (Actor->GetName().StartsWith(Prefix))
					{
						RecordIndex = Candidate;
						break;
					}
				}
				if (TestTrue(Descriptor.MapIdentity
					+ TEXT(" projected water-zone source record is identifiable"),
					Source.Records.IsValidIndex(RecordIndex)))
				{
					const bool bWaterZoneClass = Source.Records[RecordIndex].ClassIdentity.Equals(
						TEXT("WOT.WaterZone"), ESearchCase::IgnoreCase);
					TestEqual(Descriptor.MapIdentity
						+ TEXT(" water ViewFlash follows class identity, not bWaterZone"),
						Zone->ViewFlash,
						bWaterZoneClass ? FVector(-0.078f) : FVector::ZeroVector);
				}
			}
		}
		uint32 ExpectedBounceables = 0;
		if (Descriptor.MapIdentity == TEXT("Arena_01")) ExpectedBounceables = 3;
		else if (Descriptor.MapIdentity == TEXT("Arena_05")) ExpectedBounceables = 7;
		else if (Descriptor.MapIdentity == TEXT("Battle2_01")) ExpectedBounceables = 2;
		else if (Descriptor.MapIdentity == TEXT("Battle2_02")) ExpectedBounceables = 8;
		else if (Descriptor.MapIdentity == TEXT("Battle2_03")) ExpectedBounceables = 3;
		else if (Descriptor.MapIdentity == TEXT("Battle2_04")) ExpectedBounceables = 11;
		else if (Descriptor.MapIdentity == TEXT("Battle3_01")) ExpectedBounceables = 2;
		else if (Descriptor.MapIdentity == TEXT("Battle4_01")) ExpectedBounceables = 4;
		AddInfo(FString::Printf(TEXT("BOUNCEABLE_CENSUS map=%s projected=%u"),
			*Descriptor.MapIdentity,
			Projection.RuleCount(TEXT("BounceableDecoration"))));
		TestEqual(Descriptor.MapIdentity + TEXT(" bounceable-decoration census"),
			Projection.RuleCount(TEXT("BounceableDecoration")), ExpectedBounceables);
		TestEqual(Descriptor.MapIdentity + TEXT(" unsupported actor census"),
			Projection.UnsupportedRecordCount, Expected.Unsupported);
		TestFalse(Descriptor.MapIdentity + TEXT(" does not require a fallback player start"),
			Projection.bUsedPlayerStartFallback);
		AddInfo(FString::Printf(
			TEXT("RETAIL_PROJECTION_CENSUS map=%s AmbientSound=%u DynamicAmbientSound=%u WaterSoundZone=%u Corona=%u LeafMap=%u ActorRotator=%u RainCyd=%u FogLight=%u LightWave=%u PlayerStart=%u SpawnPool=%u Zone=%u Light=%u SkyLight=%u NavBounds=%u PathNode=%u ParticleSprayer=%u FixedPickup=%u AndilayRoot=%u Decoration=%u BreakableDecoration=%u ExplodingWall=%u DestroyDirective=%u Mover=%u Trigger=%u DoorTrigger=%u Dispatcher=%u Counter=%u SoundDispatcher=%u Portcullis=%u PitOpening=%u Staircase=%u Teleporter=%u Jumper=%u BlockAll=%u BlockPlayer=%u BlockMonsters=%u SpecialEventTrigger=%u SpecialEvent=%u SpecialEffectsTrigger=%u StochasticTrigger=%u TriggerLight=%u BattleInfo=%u SealAltar=%u Budget=%u MashadarGuide=%u skips=%u unsupported=%u fallback=%d"),
			*Descriptor.MapIdentity,
			Projection.RuleCount(TEXT("AmbientSound")),
			Projection.RuleCount(TEXT("DynamicAmbientSound")),
			Projection.RuleCount(TEXT("WaterSoundZone")),
			Projection.RuleCount(TEXT("Corona")),
			Projection.RuleCount(TEXT("LeafMap")),
			Projection.RuleCount(TEXT("ActorRotator")),
			Projection.RuleCount(TEXT("RainCyd")),
			Projection.RuleCount(TEXT("FogLight")),
			Projection.RuleCount(TEXT("LightWave")),
			Projection.RuleCount(TEXT("PlayerStart")),
			Projection.RuleCount(TEXT("SpawnPool")),
			Projection.RuleCount(TEXT("Zone")),
			Projection.RuleCount(TEXT("Light")),
			Projection.RuleCount(TEXT("SkyLight")),
			Projection.RuleCount(TEXT("NavBounds")),
			Projection.RuleCount(TEXT("PathNode")),
			Projection.RuleCount(TEXT("ParticleSprayer")),
			Projection.RuleCount(TEXT("FixedPickup")),
			Projection.RuleCount(TEXT("AndilayRoot")),
			Projection.RuleCount(TEXT("Decoration")),
			Projection.RuleCount(TEXT("BreakableDecoration")),
			Projection.RuleCount(TEXT("ExplodingWall")),
			Projection.RuleCount(TEXT("DestroyDirective")),
			Projection.RuleCount(TEXT("Mover")),
			Projection.RuleCount(TEXT("Trigger")),
			Projection.RuleCount(TEXT("DoorTrigger")),
			Projection.RuleCount(TEXT("Dispatcher")),
			Projection.RuleCount(TEXT("Counter")),
			Projection.RuleCount(TEXT("SoundDispatcher")),
			Projection.RuleCount(TEXT("Portcullis")),
			Projection.RuleCount(TEXT("PitOpening")),
			Projection.RuleCount(TEXT("Staircase")),
			Projection.RuleCount(TEXT("Teleporter")),
			Projection.RuleCount(TEXT("Jumper")),
			Projection.RuleCount(TEXT("BlockAll")),
			Projection.RuleCount(TEXT("BlockPlayer")),
			Projection.RuleCount(TEXT("BlockMonsters")),
			Projection.RuleCount(TEXT("SpecialEventTrigger")),
			Projection.RuleCount(TEXT("SpecialEvent")),
			Projection.RuleCount(TEXT("SpecialEffectsTrigger")),
			Projection.RuleCount(TEXT("StochasticTrigger")),
			Projection.RuleCount(TEXT("TriggerLight")),
			Projection.RuleCount(TEXT("BattleInfo")),
			Projection.RuleCount(TEXT("SealAltar")),
			Projection.RuleCount(TEXT("Budget")),
			Projection.RuleCount(TEXT("MashadarGuide")),
			SkipCount,
			Projection.UnsupportedRecordCount,
			Projection.bUsedPlayerStartFallback ? 1 : 0));
		WoTTest::DestroySpecWorld(World);
#if WITH_EDITOR
		// Net-world setup and teardown may collect weakly-held projection resources,
		// so exercise it only after the existing census has fully consumed its world.
		if (RunAnimationNetFixture) RunAnimationNetFixture();
#endif
	}
	TestEqual(TEXT("all nineteen world descriptors were projected"),
		ImportedWorlds.GetValue().Worlds.Num(), 19);
	TestEqual(TEXT("all nineteen pinned census rows were consumed"),
		CensusIndex, static_cast<int32>(UE_ARRAY_COUNT(ExpectedCensus)));
	TestEqual(TEXT("both production rolled-decoration sweep records were exercised"),
		ProductionRotationSweeps, 2u);
	TestEqual(TEXT("Battle2_05 production Fish and DragonFly rotator ticks ran"),
		ProductionDrivenDecorationTicks, 2u);
	TestEqual(TEXT("Arena_06 production pushable barrel bump ran"),
		ProductionPushableBarrels, 1u);
	TestEqual(TEXT("Battle2_03 production -500 non-pawn gravity zone ran"),
		ProductionRetailGravityZones, 1u);
	TestTrue(TEXT("production trap records exercise restored containing capsules"),
		ProductionRetailTriggerCylinders > 0u);
	TestTrue(TEXT("production PlayerStarts preserve exact authored transforms"),
		ProductionExactPlayerStarts > 0u);
	TestEqual(TEXT("Battle3_01.Mover4 and Battle4_01.Mover7 rest at KeyNum one"),
		ProductionKeyNumRestingMovers, 2u);
	TestEqual(TEXT("all MP triggers missing TriggerType default to player proximity"),
		ProductionDefaultPlayerTriggers, 147u);
	TestEqual(TEXT("MP bump-capable plain movers (BumpOpenTimed/BumpButton): 18 default BT_PlayerBump + Battle2_04.Mover3 authored BT_PawnBump"),
		ProductionDefaultPlayerBumpMovers, 19u);
	TestEqual(TEXT("all affected Battle4_01 pools match their retail first-None prefixes"),
		ProductionTruncatedBattle4Pools,
		static_cast<uint32>(UE_ARRAY_COUNT(ExpectedTruncatedPools)));
	TestEqual(TEXT("all three audited light-carried ambient loops are silent"),
		ProductionSilentLitAmbients, 3u);
	TestEqual(TEXT("the placed Battle4_01 hound carcass is non-blocking"),
		ProductionNonBlockingCarcasses, 1u);
	TestEqual(TEXT("all six Battle2_02 authored hidden movers keep collision"),
		ProductionHiddenMovers, 6u);
	TestEqual(TEXT("production Hound and ExplodingWall moving-projectile Touches ran"),
		ProductionProjectileTouches, 2u);
	TestEqual(TEXT("production DartForsaken replacement record was exercised"),
		ProductionForsakenReplacements, 1u);
	TestEqual(TEXT("all cache-format census rows were consumed"),
		CensusIndex, static_cast<int32>(UE_ARRAY_COUNT(ExpectedCache)));
	TestEqual(TEXT("world-v4 mover-model total"), TotalMoverModels, 270u);
	TestEqual(TEXT("projected mover total"), TotalProjectedMovers, 270u);
	TestEqual(TEXT("projected ambient-sound total"),
		TotalProjectedAmbientSounds, 690u);
	TestEqual(TEXT("projected dynamic-ambient total"),
		TotalProjectedDynamicAmbientSounds, 16u);
	TestEqual(TEXT("projected authored zone-sound total"),
		TotalProjectedWaterSoundZones, 17u);
	TestEqual(TEXT("projected corona total after Lightning-tag gating"),
		TotalProjectedCoronas, 711u);
	TestEqual(TEXT("projected leaf-map total"), TotalProjectedLeafMaps, 19u);
	TestEqual(TEXT("projected actor-rotator clone total"),
		TotalProjectedActorRotators, 322u);
	TestEqual(TEXT("projected RainCyd total"), TotalProjectedRainCyd, 40u);
	TestEqual(TEXT("projected fog-light total"), TotalProjectedFogLights, 15u);
	TestEqual(TEXT("projected light-wave driver total"), TotalProjectedLightWaves, 10u);
	TestEqual(TEXT("every extracted fog light was projected with its parameters"),
		MatchedExpectedFogLights,
		static_cast<uint32>(UE_ARRAY_COUNT(ExpectedFogLights)));
	TestEqual(TEXT("projected Trigger total"), TotalProjectedTriggers, 121u);
	TestEqual(TEXT("projected DoorTrigger total"), TotalProjectedDoorTriggers, 34u);
	TestEqual(TEXT("projected SoundDispatcher total"),
		TotalProjectedSoundDispatchers, 28u);
	TestEqual(TEXT("projected Portcullis total"), TotalProjectedPortcullises, 149u);
	TestEqual(TEXT("projected teleporter total"), TotalProjectedTeleporters, 40u);
	TestEqual(TEXT("projected Jumper total"), TotalProjectedJumpers, 46u);
	TestEqual(TEXT("projected BlockAll total"), TotalProjectedBlockAll, 213u);
	TestEqual(TEXT("projected BlockPlayer total"), TotalProjectedBlockPlayer, 36u);
	TestEqual(TEXT("projected PitOpening total"), TotalProjectedPitOpenings, 79u);
	TestEqual(TEXT("projected Staircase total"), TotalProjectedStaircases, 21u);
	TestEqual(TEXT("projected SpecialEventTrigger total"),
		TotalProjectedSpecialEventTriggers, 20u);
	TestEqual(TEXT("projected SpecialEvent total"), TotalProjectedSpecialEvents, 19u);
	TestEqual(TEXT("projected SpecialEffectsTrigger total"),
		TotalProjectedSpecialEffectsTriggers, 2u);
	TestEqual(TEXT("projected StochasticTrigger total"),
		TotalProjectedStochasticTriggers, 1u);
	TestEqual(TEXT("projected TriggerLight total"), TotalProjectedTriggerLights, 1u);
	TestEqual(TEXT("projected BlockMonsters total"), TotalProjectedBlockMonsters, 56u);
	TestEqual(TEXT("projected BattleInfo total"), TotalProjectedBattleInfo, 8u);
	TestEqual(TEXT("projected SealAltar total"), TotalProjectedSealAltars, 20u);
	TestEqual(TEXT("projected faction-budget total"), TotalProjectedBudgets, 20u);
	TestEqual(TEXT("all twenty unique retail budget rows were consumed exactly once"),
		ConsumedBudgetRows.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedBudgets)));
	TestEqual(TEXT("projected MashadarGuide total"),
		TotalProjectedMashadarGuides, 73u);
	TestEqual(TEXT("dynamic mover lighting total"), TotalDynamicMovers, 76u);
	TestEqual(TEXT("static mover lighting total"), TotalStaticMovers, 194u);
	TestEqual(TEXT("baked mover macro section total"),
		TotalBakedMacroMoverSections, 1u);
	TestEqual(TEXT("Battle2_02 and Battle2_05 production mover binding witnesses"),
		ProductionMoverBindingWitnesses.Num(), 2);
	TestEqual(TEXT("static mover lit-poly total"), TotalMoverLitPolys, 4235u);
	TestEqual(TEXT("mover UV1 padded-rectangle violation total"),
		TotalMoverUV1OutsidePaddedRect, 0u);
	TestEqual(TEXT("mover direct-FPoly ownership violation total"),
		TotalMoverPolyOwnershipViolations, 0u);
	TestEqual(TEXT("Battle2_04 FPoly/BSP portcullis bounds witness total"),
		TotalBattle204PortcullisBounds, 10u);
	TestEqual(TEXT("Battle2_04 empty collision portcullis witness total"),
		TotalBattle204EmptyCollisionPortcullises, 0u);
	TestEqual(TEXT("animated mover-light poly total"),
		TotalMoverAnimatedLightPolys, 12u);
	auto DispositionCount = [](const TMap<FName, uint32>& Counts,
		const TCHAR* Name)
	{
		if (const uint32* Count = Counts.Find(FName(Name))) return *Count;
		return 0u;
	};
	TestEqual(TEXT("SpecialEventTrigger is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("SpecialEventTrigger")), 0u);
	TestEqual(TEXT("SpecialEvent is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("SpecialEvent")), 0u);
	TestEqual(TEXT("all retail SpewTrigger records project"),
		TotalProjectedSpewTriggers, 11u);
	TestEqual(TEXT("SpewTrigger is no longer inert"),
		DispositionCount(TotalInertClasses, TEXT("SpewTrigger")), 0u);
	TestEqual(TEXT("reviewed FlushTrigger inert total"),
		DispositionCount(TotalInertClasses, TEXT("FlushTrigger")), 9u);
	TestEqual(TEXT("all retail Lightning records project"), TotalProjectedLightning, 20u);
	TestEqual(TEXT("all retail RandomLightning records project"),
		TotalProjectedRandomLightning, 2u);
	TestEqual(TEXT("both production storms relay into authored lightning01 actors"),
		ProductionRandomLightningRelays, 2u);
	TestEqual(TEXT("all production Lightning records retain their target light tags"),
		ProductionTaggedLightning, 20u);
	TestEqual(TEXT("all production lightning target light records project"),
		ProductionLightningTargetLights, 20u);
	TestEqual(TEXT("multiplayer map set has no DistanceLightning placement"),
		TotalProjectedDistanceLightning, 0u);
	TestEqual(TEXT("all retail Wind records project"), TotalProjectedWind, 7u);
	TestEqual(TEXT("one real Battle2_03 trigger-driven Wind record is covered"),
		ProductionTriggeredMPWind, 1u);
	TestEqual(TEXT("Lightning is no longer inert"),
		DispositionCount(TotalInertClasses, TEXT("Lightning")), 0u);
	TestEqual(TEXT("RandomLightning is no longer inert"),
		DispositionCount(TotalInertClasses, TEXT("RandomLightning")), 0u);
	TestEqual(TEXT("ParticleSystems.Wind is no longer inert"),
		DispositionCount(TotalInertClasses, TEXT("Wind")), 0u);
	TestEqual(TEXT("RainCyd is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("RainCyd")), 0u);
	TestEqual(TEXT("ActorRotator is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("ActorRotator")), 0u);
	TestEqual(TEXT("SpecialEffectsTrigger is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("SpecialEffectsTrigger")), 0u);
	TestEqual(TEXT("StochasticTrigger is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("StochasticTrigger")), 0u);
	TestEqual(TEXT("TriggerLight is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("TriggerLight")), 0u);
	TestEqual(TEXT("BlockMonsters is no longer unsupported"),
		DispositionCount(TotalUnsupportedClasses, TEXT("BlockMonsters")), 0u);
	for (const TCHAR* ClassName : {TEXT("WOTExplodingWall"),
		TEXT("DartForsaken"), TEXT("DestroyDirective"),
		TEXT("LegionInvSeeker")})
		TestEqual(FString(ClassName) + TEXT(" is no longer unsupported"),
			DispositionCount(TotalUnsupportedClasses, ClassName), 0u);
	TestEqual(TEXT("projected SoundDispatcher tagged-marker total"),
		TotalProjectedTaggedMarkers, 3u);
	TestEqual(TEXT("world-v4 mover-section/plane total"),
		TotalMoverSections, 3653u);
	TestEqual(TEXT("world-v4 mover render-triangle total"),
		TotalMoverTriangles, 11306u);
	TestEqual(TEXT("world-v4 mover collision-triangle total"),
		TotalMoverCollisionTriangles, 12187u);
	TestEqual(TEXT("production all-PF_NotSolid mover census"),
		TotalBattle201NotSolidMovers, 1u);
	TestEqual(TEXT("production PF_NotSolid mover point-and-pawn trace witnesses"),
		ProductionNotSolidMoverTraceWitnesses, 1u);
	TestEqual(TEXT("all twenty-one retail modulated map sprayers project in modulate-only mode"),
		TotalProjectedModulatedSprayers, 21u);
	TestEqual(TEXT("runtime light-table row total"), TotalLightRows, 3317u);
	TestEqual(TEXT("world-v4 leaf-light reference total"),
		TotalLightReferences, 283600u);
	TestEqual(TEXT("world-v4 longest leaf-light run"), LongestLightRun, 52u);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeActorVisualDefaultsSpec,
	"WoT.World.Projector.ActorVisualDefaults",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeActorVisualDefaultsSpec::RunTest(const FString&)
{
	auto ObjectProperty = [](const TCHAR* Name, uint32 Slot,
		const TCHAR* Identity)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.ArrayIndex = Slot;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ObjectIdentity = Identity;
		Property.ObjectClassIdentity = TEXT("Engine.Texture");
		return Property;
	};
	FWoTRuntimeClassDefaultRecord Actor;
	Actor.Identity = TEXT("Engine.Actor");
	Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("DrawScale"), EWoTRuntimeWorldPropertyType::Float, TEXT(""), 1.5f));
	Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("ScaleGlow"), EWoTRuntimeWorldPropertyType::Float, TEXT(""), 0.75f));
	Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("AmbientGlow"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(17)));
	Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Fatness"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(128)));
	Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bUnlit"), EWoTRuntimeWorldPropertyType::Boolean, TEXT(""), uint8(1)));
	Actor.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Style"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(3)));
	FWoTRuntimeClassDefaultRecord Inventory;
	Inventory.Identity = TEXT("Engine.Inventory");
	Inventory.SuperclassIdentity = TEXT("Engine.Actor");
	Inventory.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bRotatingPickup"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	Inventory.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bMeshCurvy"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	const int32 RotationUnits[3] = {0, 5000, 0};
	FWoTRuntimeWorldProperty Rotation;
	Rotation.Name = TEXT("RotationRate");
	Rotation.Type = EWoTRuntimeWorldPropertyType::Struct;
	Rotation.StructName = TEXT("Rotator");
	Rotation.Value.Append(reinterpret_cast<const uint8*>(RotationUnits),
		sizeof(RotationUnits));
	Inventory.Properties.Add(MoveTemp(Rotation));
	FWoTRuntimeClassDefaultRecord Dart;
	Dart.Identity = TEXT("Angreal.AngrealInvDart");
	Dart.SuperclassIdentity = TEXT("Engine.Inventory");
	FWoTRuntimeWorldProperty Mesh = ObjectProperty(
		TEXT("Mesh"), 0, TEXT("Angreal.DartMesh#export-00001291"));
	Mesh.ObjectClassIdentity = TEXT("Engine.Mesh");
	Dart.Properties.Add(MoveTemp(Mesh));
	Dart.Properties.Add(ObjectProperty(
		TEXT("Skin"), 0, TEXT("Angreal.Skins.JDart")));
	Dart.Properties.Add(ObjectProperty(
		TEXT("MultiSkins"), 2, TEXT("WOT.Skins.JGlow")));
	FWoTRuntimeClassDefaultRecord Cylinder;
	Cylinder.Identity = TEXT("WOTDecorations.Cylinder");
	Cylinder.SuperclassIdentity = TEXT("Engine.Actor");
	FWoTRuntimeWorldProperty PearlMesh = ObjectProperty(
		TEXT("Mesh"), 0, TEXT("Angreal.AMAPearl#export-00000001"));
	PearlMesh.ObjectClassIdentity = TEXT("Engine.Mesh");
	Cylinder.Properties.Add(MoveTemp(PearlMesh));
	FWoTRuntimeClassDefaultRecord AMA;
	AMA.Identity = TEXT("Angreal.AngrealInvAMA");
	AMA.SuperclassIdentity = TEXT("Engine.Inventory");
	FWoTRuntimeWorldProperty BoxMesh = ObjectProperty(
		TEXT("Mesh"), 0, TEXT("Angreal.AngrealAntiMagicAuraPickup#export-00000002"));
	BoxMesh.ObjectClassIdentity = TEXT("Engine.Mesh");
	AMA.Properties.Add(MoveTemp(BoxMesh));
	AMA.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bMeshCurvy"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));

	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("visual class-default fixture loads"),
		Defaults.LoadFixture({Actor, Inventory, Dart, Cylinder, AMA}, Error)))
	{
		AddError(Error);
		return false;
	}
	FString ClassIdentity;
	TestTrue(TEXT("native leaf resolves to its unique retail class"),
		Defaults.FindClassIdentityByLeaf(TEXT("AngrealInvDart"), ClassIdentity));
	TestEqual(TEXT("resolved retail class identity"), ClassIdentity,
		FString(TEXT("Angreal.AngrealInvDart")));
	TestNotNull(TEXT("curvy lookup walks the superclass chain"),
		Defaults.FindClassProperty(ClassIdentity, TEXT("bMeshCurvy")));

	FWoTRuntimeWorldActor Instance;
	Instance.ClassIdentity = ClassIdentity;
	Instance.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("DrawScale"), EWoTRuntimeWorldPropertyType::Float, TEXT(""), 2.0f));
	Instance.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Fatness"), EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(200)));
	// Matches the multiplayer Cylinder placements whose instance property turns
	// the inherited AMAPearl display mesh into a curved surface.
	Instance.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bMeshCurvy"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	FWoTRuntimeActorVisualPresentation Visual;
	TestTrue(TEXT("layered actor presentation decodes"),
		WoTRuntimeReadActorVisualPresentation(Defaults, Instance, Visual, Error));
	TestEqual(TEXT("mesh package"), Visual.MeshPackage, FName(TEXT("Angreal")));
	TestEqual(TEXT("mesh name"), Visual.MeshName, FName(TEXT("DartMesh")));
	TestEqual(TEXT("instance draw scale wins"), Visual.DrawScale, 2.0f);
	TestEqual(TEXT("inherited scale glow"), Visual.ScaleGlow, 0.75f);
	TestEqual(TEXT("inherited ambient glow"), Visual.AmbientGlow, uint8(17));
	TestEqual(TEXT("instance Fatness wins"), Visual.Fatness, uint8(200));
	TestTrue(TEXT("inherited unlit flag"), Visual.bUnlit);
	TestTrue(TEXT("instance curvy flag is retained"), Visual.bMeshCurvy);
	TestTrue(TEXT("STY_Translucent is retained"), Visual.bStyleTranslucent);
	TestTrue(TEXT("retail pickup rotation is enabled"), Visual.bRotatingPickup);
	TestTrue(TEXT("retail yaw rate is decoded"),
		Visual.RotationRate.Equals(FRotator(0.0, 5000.0 * 360.0 / 65536.0, 0.0)));
	TestEqual(TEXT("actor Skin stays separate from MultiSkins"), Visual.Skin,
		FSoftObjectPath(TEXT("/Game/WoT/Textures/Angreal/JDart.JDart")));
	TestFalse(TEXT("actor Skin does not synthesize MultiSkins[0]"),
		Visual.SkinOverrides.Contains(0));
	TestEqual(TEXT("MultiSkins retains its exact slot"),
		Visual.SkinOverrides.FindChecked(2),
		FSoftObjectPath(TEXT("/Game/WoT/Textures/WOT/JGlow.JGlow")));

	FWoTRuntimeWorldActor CylinderInstance;
	CylinderInstance.ClassIdentity = TEXT("WOTDecorations.Cylinder");
	CylinderInstance.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("bMeshCurvy"), EWoTRuntimeWorldPropertyType::Boolean,
		TEXT(""), uint8(1)));
	FWoTRuntimeActorVisualPresentation CylinderVisual;
	TestTrue(TEXT("Cylinder placement presentation decodes"),
		WoTRuntimeReadActorVisualPresentation(
			Defaults, CylinderInstance, CylinderVisual, Error));
	TestEqual(TEXT("Cylinder retains its AMAPearl display mesh"),
		CylinderVisual.MeshName, FName(TEXT("AMAPearl")));
	TestTrue(TEXT("Cylinder placement retains bMeshCurvy"),
		CylinderVisual.bMeshCurvy);

	// The AMA pickup is a box: its retail bMeshCurvy is not honored.
	FWoTRuntimeActorVisualPresentation AMAVisual;
	TestTrue(TEXT("AMA pickup presentation decodes"),
		WoTRuntimeReadClassVisualPresentation(
			Defaults, TEXT("Angreal.AngrealInvAMA"), AMAVisual, Error));
	TestEqual(TEXT("AMA pickup keeps its box mesh"),
		AMAVisual.MeshName, FName(TEXT("AngrealAntiMagicAuraPickup")));
	TestFalse(TEXT("AMA pickup box stays flat despite its retail bMeshCurvy"),
		AMAVisual.bMeshCurvy);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshFatnessRuntimeActorVisualSpec,
	"WoT.Mesh.Fatness.RuntimeActorVisual",
	WoTRuntimeMapProjectionFlags)

bool FWoTMeshFatnessRuntimeActorVisualSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaultRecord ActorDefaults;
	ActorDefaults.Identity = TEXT("Engine.Actor");
	ActorDefaults.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Fatness"), EWoTRuntimeWorldPropertyType::Byte,
		TEXT(""), uint8(128)));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("actor Fatness defaults load"),
		Defaults.LoadFixture({ActorDefaults}, Error)))
	{
		AddError(Error);
		return false;
	}

	FWoTRuntimeWorldActor Fish;
	Fish.ClassIdentity = TEXT("Engine.Actor");
	Fish.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Fatness"), EWoTRuntimeWorldPropertyType::Byte,
		TEXT(""), uint8(200)));
	FWoTRuntimeActorVisualPresentation Visual;
	TestTrue(TEXT("map-authored Fatness decodes"),
		WoTRuntimeReadActorVisualPresentation(Defaults, Fish, Visual, Error));
	TestEqual(TEXT("map-authored Fatness overrides the Actor default"),
		Visual.Fatness, uint8(200));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeClassDefaultsLayeringSpec,
	"WoT.World.Projector.ClassDefaultsLayering",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeClassDefaultsLayeringSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaultRecord ActorBase;
	ActorBase.Identity = TEXT("Engine.Actor");
	ActorBase.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("InheritedByte"), EWoTRuntimeWorldPropertyType::Byte,
		TEXT(""), static_cast<uint8>(3)));
	FWoTRuntimeClassDefaultRecord PoolBase;
	PoolBase.Identity = TEXT("Angreal.AngrealSpawnPool");
	PoolBase.SuperclassIdentity = TEXT("Engine.Actor");
	FWoTRuntimeWorldProperty BaseWeight = WoTRuntimeMapProjectionProperty(
		TEXT("Weight"), EWoTRuntimeWorldPropertyType::Byte,
		TEXT(""), static_cast<uint8>(5));
	BaseWeight.ArrayIndex = 2;
	PoolBase.Properties.Add(MoveTemp(BaseWeight));
	FWoTRuntimeClassDefaultRecord Rare;
	Rare.Identity = TEXT("Angreal.RareAngrealSpawnPool");
	Rare.SuperclassIdentity = TEXT("Angreal.AngrealSpawnPool");
	Rare.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("RespawnTime"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 80.0f));

	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	TestTrue(TEXT("cross-package class-default fixture loads"),
		Defaults.LoadFixture({ActorBase, PoolBase, Rare}, Error));

	FWoTRuntimeWorldActor Instance;
	Instance.ClassIdentity = TEXT("aNgReAl.RaReAnGrEaLsPaWnPoOl");
	Instance.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("RespawnTime"), EWoTRuntimeWorldPropertyType::Float,
		TEXT(""), 91.0f));
	uint8 InheritedByte = 0;
	uint8 InheritedWeight = 0;
	float OverriddenRespawn = 0.0f;
	TestTrue(TEXT("rule matching sees a cross-package superclass"),
		Defaults.ClassIsA(Instance.ClassIdentity,
			TEXT("Engine.Actor")));
	const FWoTRuntimeWorldProperty* ByteProperty =
		Defaults.FindProperty(Instance, TEXT("InheritedByte"));
	const FWoTRuntimeWorldProperty* WeightProperty =
		Defaults.FindProperty(Instance, TEXT("Weight"), 2);
	const FWoTRuntimeWorldProperty* RespawnProperty =
		Defaults.FindProperty(Instance, TEXT("RespawnTime"));
	TestTrue(TEXT("base default resolves"), ByteProperty != nullptr
		&& WoTRuntimeWorldPropertyByte(*ByteProperty, InheritedByte));
	TestTrue(TEXT("inherited array slot resolves"), WeightProperty != nullptr
		&& WoTRuntimeWorldPropertyByte(*WeightProperty, InheritedWeight));
	TestTrue(TEXT("instance delta overrides class chain"), RespawnProperty != nullptr
		&& WoTRuntimeWorldPropertyFloat(*RespawnProperty, OverriddenRespawn));
	TestEqual(TEXT("base byte value"), InheritedByte, uint8(3));
	TestEqual(TEXT("base array value"), InheritedWeight, uint8(5));
	TestEqual(TEXT("instance value"), OverriddenRespawn, 91.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeClassDefaultsDisambiguatedSpec,
	"WoT.World.Projector.ClassDefaultsDisambiguatedIdentity",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeClassDefaultsDisambiguatedSpec::RunTest(const FString&)
{
	// Real retail shape (a packaged Arena_01 rejection): Angreal's
	// EarthTremor class shares Pkg.Name with its EarthTremor sound group, so
	// the builder emits `Angreal.EarthTremor#export-00000416`, while
	// WoTPawns.LegionEarthTremor's imported superclass is plain
	// `Angreal.EarthTremor`.
	FWoTRuntimeClassDefaultRecord Projectile;
	Projectile.Identity = TEXT("WOT.AngrealProjectile");
	Projectile.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Speed"), EWoTRuntimeWorldPropertyType::Float, TEXT(""), 600.0f));
	FWoTRuntimeClassDefaultRecord Tremor;
	Tremor.Identity = TEXT("Angreal.EarthTremor#export-00000416");
	Tremor.SuperclassIdentity = TEXT("WOT.AngrealProjectile");
	Tremor.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Damage"), EWoTRuntimeWorldPropertyType::Float, TEXT(""), 25.0f));
	FWoTRuntimeClassDefaultRecord Legion;
	Legion.Identity = TEXT("WoTPawns.LegionEarthTremor");
	Legion.SuperclassIdentity = TEXT("Angreal.EarthTremor");

	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("an imported plain superclass resolves to its unique disambiguated class"),
		Defaults.LoadFixture({Projectile, Tremor, Legion}, Error)))
	{
		AddError(Error);
		return false;
	}
	const FWoTRuntimeWorldProperty* Damage =
		Defaults.FindClassProperty(TEXT("WoTPawns.LegionEarthTremor"), TEXT("Damage"));
	const FWoTRuntimeWorldProperty* Speed =
		Defaults.FindClassProperty(TEXT("angreal.earthtremor"), TEXT("Speed"));
	float DamageValue = 0.0f;
	float SpeedValue = 0.0f;
	TestTrue(TEXT("defaults inherit through the disambiguated class"), Damage != nullptr
		&& WoTRuntimeWorldPropertyFloat(*Damage, DamageValue) && DamageValue == 25.0f);
	TestTrue(TEXT("a plain world class reference finds the disambiguated class"), Speed != nullptr
		&& WoTRuntimeWorldPropertyFloat(*Speed, SpeedValue) && SpeedValue == 600.0f);
	TArray<FString> Chain;
	TestTrue(TEXT("class chain resolves"),
		Defaults.GetClassChain(TEXT("WoTPawns.LegionEarthTremor"), Chain));
	TestEqual(TEXT("chain reports the public identity"),
		Chain, TArray<FString>({TEXT("WoTPawns.LegionEarthTremor"),
			TEXT("Angreal.EarthTremor"), TEXT("WOT.AngrealProjectile")}));
	TestTrue(TEXT("rule matching sees the public identity"),
		Defaults.ClassIsA(TEXT("WoTPawns.LegionEarthTremor"), TEXT("Angreal.EarthTremor")));

	// Two disambiguated classes with the same base stay ambiguous: fail closed.
	FWoTRuntimeClassDefaultRecord TremorTwin = Tremor;
	TremorTwin.Identity = TEXT("Angreal.EarthTremor#export-00000417");
	FWoTRuntimeClassDefaults Ambiguous;
	FString AmbiguousError;
	TestFalse(TEXT("an ambiguous disambiguated base is never aliased"),
		Ambiguous.LoadFixture({Projectile, Tremor, TremorTwin, Legion}, AmbiguousError));
	TestTrue(TEXT("ambiguity reports the absent superclass"),
		AmbiguousError.Contains(TEXT("Angreal.EarthTremor")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPSharedAtlasSpec,
	"WoT.World.RuntimeBSP.SharedAtlasOwnershipAndFallback",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPSharedAtlasSpec::RunTest(const FString&)
{
	auto MakeDescriptor = []()
	{
		FWoTWorldDescriptor Descriptor;
		Descriptor.MapIdentity = TEXT("SyntheticArena");
		FWoTBSPPartDescriptor& Main = Descriptor.BSPParts.AddDefaulted_GetRef();
		Main.PartIdentity = Descriptor.MapIdentity;
		Main.Role = EWoTBSPPartRole::Main;
		FWoTBSPPartDescriptor& Portal = Descriptor.BSPParts.AddDefaulted_GetRef();
		Portal.PartIdentity = Descriptor.MapIdentity + TEXT("_portals");
		Portal.Role = EWoTBSPPartRole::Portal;
		return Descriptor;
	};
	auto VerifyBound = [this](AWoTRuntimeBSPWorld* RuntimeWorld,
		UTexture2D* Expected, const TCHAR* Label)
	{
		if (!TestNotNull(FString(Label) + TEXT(" runtime owner"), RuntimeWorld))
			return;
		TestEqual(FString(Label) + TEXT(" has both geometry parts"),
			RuntimeWorld->GeometryComponents.Num(), 2);
		// Shell worlds carry the dev maps' unbound GammaBoundary + flat tone
		// look; without it the display-space lightmaps wash out flat.
		UPostProcessComponent* Look = RuntimeWorld->PeriodLook;
		if (TestNotNull(FString(Label) + TEXT(" period look"), Look))
		{
			TestTrue(FString(Label) + TEXT(" period look is unbound and enabled"),
				Look->bUnbound && Look->bEnabled && Look->BlendWeight == 1.0f);
			TestTrue(FString(Label) + TEXT(" period look flattens tone and gamut"),
				Look->Settings.bOverride_ToneCurveAmount
				&& Look->Settings.ToneCurveAmount == 0.0f
				&& Look->Settings.bOverride_ExpandGamut
				&& Look->Settings.ExpandGamut == 0.0f);
			const bool bHasBoundary =
				Look->Settings.WeightedBlendables.Array.Num() == 1
				&& Look->Settings.WeightedBlendables.Array[0].Weight == 1.0f
				&& Look->Settings.WeightedBlendables.Array[0].Object != nullptr
				&& Look->Settings.WeightedBlendables.Array[0].Object->GetPathName()
					== TEXT("/Game/WoT/Materials/Instances/MI_GammaBoundary.MI_GammaBoundary");
			TestTrue(FString(Label) + TEXT(" period look binds the gamma boundary"),
				bHasBoundary);
			TestTrue(FString(Label) + TEXT(" period look is owned by the runtime world"),
				Look->GetOwner() == RuntimeWorld);
		}
		for (int32 PartIndex = 0;
			PartIndex < RuntimeWorld->GeometryComponents.Num(); ++PartIndex)
		{
			UStaticMeshComponent* Component =
				RuntimeWorld->GeometryComponents[PartIndex];
			UMaterialInstanceDynamic* MID = Component != nullptr
				? Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0)) : nullptr;
			if (!TestNotNull(FString::Printf(TEXT("%s part %d MID"),
				Label, PartIndex), MID)) continue;
			UTexture* Bound = nullptr;
			MID->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("Lightmap")), Bound);
			TestEqual(FString::Printf(TEXT("%s part %d shares lightmap"),
				Label, PartIndex), Bound, static_cast<UTexture*>(Expected));
		}
	};

	FWoTWorldDescriptor Descriptor = MakeDescriptor();
	TArray<FWoTBSPPayloadData> Parts = {
		WoTRuntimeMapProjectionTriangle(), WoTRuntimeMapProjectionTriangle(true)};
	Parts[0].FormatVersion = 2;
	Parts[0].LightmapWidth = 1;
	Parts[0].LightmapHeight = 1;
	Parts[0].LightmapBGRA = {11, 22, 33, 255};
	UWorld* AtlasWorld = WoTRuntimeMapProjectionWorld(TEXT("SharedAtlas"));
	AWoTRuntimeBSPWorld* AtlasOwner = nullptr;
	uint32 FallbackParts = MAX_uint32;
	FString Error;
	if (TestTrue(TEXT("singular main atlas materializes"),
		WoTWorldBuildRuntimeBSPForTesting(*AtlasWorld, Descriptor, Parts,
			AtlasOwner, FallbackParts, Error)))
	{
		TestEqual(TEXT("authored atlas has no neutral fallbacks"), FallbackParts, 0u);
		UTexture2D* Shared = AtlasOwner != nullptr
			&& AtlasOwner->RuntimeTextures.Num() >= 2
			? AtlasOwner->RuntimeTextures[1] : nullptr;
		VerifyBound(AtlasOwner, Shared, TEXT("authored"));
		if (TestNotNull(TEXT("authored atlas is retained"), Shared))
		{
			const FColor* Pixel = static_cast<const FColor*>(
				Shared->GetPlatformData()->Mips[0].BulkData.LockReadOnly());
			if (TestNotNull(TEXT("authored atlas texel"), Pixel))
				TestEqual(TEXT("authored atlas bytes survive materialization"),
					*Pixel, FColor(33, 22, 11, 255));
			Shared->GetPlatformData()->Mips[0].BulkData.Unlock();
		}
	}
	else AddError(Error);
	WoTRuntimeMapProjectionDestroyWorld(AtlasWorld);

	Parts[0].FormatVersion = 1;
	Parts[0].LightmapWidth = 0;
	Parts[0].LightmapHeight = 0;
	Parts[0].LightmapBGRA.Reset();
	UWorld* NeutralWorld = WoTRuntimeMapProjectionWorld(TEXT("SharedNeutral"));
	AWoTRuntimeBSPWorld* NeutralOwner = nullptr;
	FallbackParts = MAX_uint32;
	Error.Reset();
	if (TestTrue(TEXT("atlas-free map gets one shared neutral texture"),
		WoTWorldBuildRuntimeBSPForTesting(*NeutralWorld, Descriptor, Parts,
			NeutralOwner, FallbackParts, Error)))
	{
		TestEqual(TEXT("every atlas-free part is counted as fallback"),
			FallbackParts, 2u);
		UTexture2D* Shared = NeutralOwner != nullptr
			&& NeutralOwner->RuntimeTextures.Num() >= 2
			? NeutralOwner->RuntimeTextures[1] : nullptr;
		VerifyBound(NeutralOwner, Shared, TEXT("neutral"));
	}
	else AddError(Error);
	WoTRuntimeMapProjectionDestroyWorld(NeutralWorld);

	Parts[0].Sections[0].MaterialSlot = TEXT("__None");
	Parts[1].Sections[0].MaterialSlot = TEXT("NatureT__Sky_440");
	Descriptor.BSPParts[0].ProceduralMaterialSlot = TEXT("__None");
	UWorld* UnresolvedWorld =
		WoTRuntimeMapProjectionWorld(TEXT("ConservativeUnresolved"));
	AWoTRuntimeBSPWorld* UnresolvedOwner = nullptr;
	uint32 UnresolvedSlots = MAX_uint32;
	Error.Reset();
	if (TestTrue(TEXT("unresolved slots materialize conservatively"),
		WoTWorldBuildRuntimeBSPForTesting(*UnresolvedWorld, Descriptor, Parts,
			UnresolvedOwner, FallbackParts, Error, {}, &UnresolvedSlots)))
	{
		TestEqual(TEXT("unresolved slots remain renderable"),
			UnresolvedOwner->GeometryComponents.Num(), 2);
		TestEqual(TEXT("unresolved slots are counted"), UnresolvedSlots, 2u);
		UMaterialInstanceDynamic* Material = UnresolvedOwner != nullptr
			&& !UnresolvedOwner->GeometryComponents.IsEmpty()
			? Cast<UMaterialInstanceDynamic>(
				UnresolvedOwner->GeometryComponents[0]->GetMaterial(0)) : nullptr;
		if (TestNotNull(TEXT("unresolved procedural slot has a material"),
			Material))
		{
			TestEqual(TEXT("unresolved procedural slot stays opaque"),
				Material->GetBlendMode(), BLEND_Opaque);
			UTexture* Bound = nullptr;
			Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("Tex")), Bound);
			TestEqual(TEXT("unresolved procedural slot binds neutral grey"),
				Bound, static_cast<UTexture*>(UnresolvedOwner->RuntimeTextures[0]));
		}
	}
	else AddError(Error);
	WoTRuntimeMapProjectionDestroyWorld(UnresolvedWorld);
	Parts[0].Sections[0].MaterialSlot = TEXT("ArenaTex__Floor_M_2S_U_SL");
	Parts[1].Sections[0].MaterialSlot = TEXT("ArenaTex__Floor_M_2S_U_SL");
	Descriptor.BSPParts[0].ProceduralMaterialSlot.Reset();

	Parts[1].LightmapWidth = 1;
	Parts[1].LightmapHeight = 1;
	Parts[1].LightmapBGRA = {1, 2, 3, 255};
	UWorld* InvalidWorld = WoTRuntimeMapProjectionWorld(TEXT("DuplicateAtlas"));
	AWoTRuntimeBSPWorld* InvalidOwner = nullptr;
	Error.Reset();
	TestFalse(TEXT("non-main atlas ownership is rejected"),
		WoTWorldBuildRuntimeBSPForTesting(*InvalidWorld, Descriptor, Parts,
			InvalidOwner, FallbackParts, Error));
	TestTrue(TEXT("ownership rejection is diagnostic"),
		Error.Contains(TEXT("singular main part")));
	WoTRuntimeMapProjectionDestroyWorld(InvalidWorld);

	Parts[0].LightmapWidth = 1;
	Parts[0].LightmapHeight = 1;
	Parts[0].LightmapBGRA = {4, 5, 6, 255};
	Descriptor.BSPParts[1].PartIdentity = Descriptor.MapIdentity;
	UWorld* DuplicateWorld = WoTRuntimeMapProjectionWorld(TEXT("TwoAtlases"));
	InvalidOwner = nullptr;
	Error.Reset();
	TestFalse(TEXT("a second atlas owner is rejected"),
		WoTWorldBuildRuntimeBSPForTesting(*DuplicateWorld, Descriptor, Parts,
			InvalidOwner, FallbackParts, Error));
	TestTrue(TEXT("duplicate ownership rejection is diagnostic"),
		Error.Contains(TEXT("singular main part")));
	WoTRuntimeMapProjectionDestroyWorld(DuplicateWorld);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPRendererSpec,
	"WoT.World.RuntimeBSP.RendererCollisionStyleAndNeutralLightmap",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPRendererSpec::RunTest(const FString&)
{
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("Renderer"));
	if (!TestNotNull(TEXT("hidden renderer world exists"), World)) return false;
	AWoTRuntimeBSPWorld* Owner = World->SpawnActor<AWoTRuntimeBSPWorld>();
	if (!TestNotNull(TEXT("runtime BSP owner exists"), Owner))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	FString Error;
	FWoTTexturePayloadData WorldTexturePayload;
	WorldTexturePayload.Palette.SetNum(256);
	for (int32 Index = 0; Index < WorldTexturePayload.Palette.Num(); ++Index)
	{
		WorldTexturePayload.Palette[Index] = FColor(
			static_cast<uint8>(Index), static_cast<uint8>(Index),
			static_cast<uint8>(Index), 255);
	}
#if !WOT_GUARDED_BOOTSTRAP
	WorldTexturePayload.BlobBytes = {0, 1, 2, 3, 4};
#endif
	FWoTTexturePayloadMip WorldBaseMip;
	WorldBaseMip.Width = 2;
	WorldBaseMip.Height = 2;
	WorldBaseMip.StoredByteLength = 4;
	WorldBaseMip.ExpandedByteLength = 16;
	WorldBaseMip.DataOffset = 0;
	WorldTexturePayload.Mips.Add(WorldBaseMip);
	FWoTTexturePayloadMip WorldSmallMip;
	WorldSmallMip.Width = 1;
	WorldSmallMip.Height = 1;
	WorldSmallMip.StoredByteLength = 1;
	WorldSmallMip.ExpandedByteLength = 4;
	WorldSmallMip.DataOffset = 4;
	WorldTexturePayload.Mips.Add(WorldSmallMip);
	UTexture2D* SmoothWorldTexture = WoTRuntimeBSPMaterializeTexture(
		WorldTexturePayload, true, false);
	if (TestNotNull(TEXT("smooth world texture materializes"), SmoothWorldTexture))
	{
		TestEqual(TEXT("smooth world texture retains cached mips"),
			SmoothWorldTexture->GetPlatformData()->Mips.Num(), 2);
		TestEqual(TEXT("smooth world texture samples through its texture group"),
			SmoothWorldTexture->Filter, TF_Default);
		TestEqual(TEXT("smooth world texture is in the world group (anisotropic)"),
			static_cast<int32>(SmoothWorldTexture->LODGroup),
			static_cast<int32>(TEXTUREGROUP_World));
		TestEqual(TEXT("smooth world texture uses the closest integer retail LOD bias"),
			SmoothWorldTexture->LODBias, -1);
	}
	UTexture2D* NoSmoothWorldTexture = WoTRuntimeBSPMaterializeTexture(
		WorldTexturePayload, true, true);
	if (TestNotNull(TEXT("PF_NoSmooth world texture materializes"), NoSmoothWorldTexture))
	{
		TestEqual(TEXT("PF_NoSmooth still retains cached mips"),
			NoSmoothWorldTexture->GetPlatformData()->Mips.Num(), 2);
		TestEqual(TEXT("PF_NoSmooth requests point sampling"),
			NoSmoothWorldTexture->Filter, TF_Nearest);
		TestEqual(TEXT("PF_NoSmooth keeps the world LOD bias"),
			NoSmoothWorldTexture->LODBias, -1);
	}
	FWoTBSPPayloadData Main = WoTRuntimeMapProjectionTriangle();
	UStaticMesh* MainMesh = NewObject<UStaticMesh>(Owner);
	TestTrue(TEXT("synthetic render mesh builds"),
		WoTRuntimeBSPBuildRenderMesh(*MainMesh, Main, Error));
	TestTrue(TEXT("synthetic complex collision cooks synchronously"),
		WoTRuntimeBSPBuildCollision(*MainMesh, Main, Error));
	UBodySetup* MainBody = MainMesh->GetBodySetup();
	if (TestNotNull(TEXT("colliding mesh has a body setup"), MainBody))
	{
		TestEqual(TEXT("collision is complex as simple"),
			MainBody->CollisionTraceFlag, CTF_UseComplexAsSimple);
		TestTrue(TEXT("physics meshes were created before registration"),
			MainBody->bCreatedPhysicsMeshes);
		TestFalse(TEXT("collision cook did not fail"),
			MainBody->bFailedToCreatePhysicsMeshes);
	}

	FWoTBSPPayloadData Portal = WoTRuntimeMapProjectionTriangle(true);
	UStaticMesh* PortalMesh = NewObject<UStaticMesh>(Owner);
	TestTrue(TEXT("portal render mesh builds"),
		WoTRuntimeBSPBuildRenderMesh(*PortalMesh, Portal, Error));
	TestTrue(TEXT("portal collision path is an intentional no-op"),
		WoTRuntimeBSPBuildCollision(*PortalMesh, Portal, Error));
	TestNull(TEXT("portal has no body setup"), PortalMesh->GetBodySetup());

	struct FStyleCase
	{
		const TCHAR* Slot;
		EWoTRuntimeBSPBlend Blend;
		bool bTwoSided;
		bool bUnlit;
		bool bSky;
		bool bSpecialLit;
	};
	const FStyleCase Cases[] = {
		{TEXT("Pkg__Opaque"), EWoTRuntimeBSPBlend::Opaque, false, false, false, false},
		{TEXT("Pkg__Masked_M"), EWoTRuntimeBSPBlend::Masked, false, false, false, false},
		{TEXT("Pkg__Glass_T_2S"), EWoTRuntimeBSPBlend::Translucent, true, false, false, false},
		{TEXT("Pkg__Glow_Mod_U"), EWoTRuntimeBSPBlend::Modulated, false, true, false, false},
		{TEXT("Pkg__Window_Sky_SL"), EWoTRuntimeBSPBlend::Opaque, false, false, true, true},
		{TEXT("Pkg__Sky__SkyZone"), EWoTRuntimeBSPBlend::Opaque, false, false, true, false},
	};
	for (const FStyleCase& StyleCase : Cases)
	{
		FWoTRuntimeBSPSectionStyle Style;
		TestTrue(StyleCase.Slot,
			WoTRuntimeBSPSectionStyle(StyleCase.Slot, Style));
		TestEqual(TEXT("blend suffix"), Style.Blend, StyleCase.Blend);
		TestEqual(TEXT("two-sided suffix"), Style.bTwoSided, StyleCase.bTwoSided);
		TestEqual(TEXT("unlit suffix"), Style.bUnlit, StyleCase.bUnlit);
		TestEqual(TEXT("sky suffix"), Style.bSky, StyleCase.bSky);
		TestEqual(TEXT("special-lit suffix"),
			Style.bSpecialLit, StyleCase.bSpecialLit);
	}
	FWoTRuntimeBSPSectionStyle DynamicLightStyle;
	TestEqual(TEXT("ordinary world section receives dynamic lights"),
		WoTRuntimeBSPDynamicLightEnable(DynamicLightStyle), 1.0f);
	DynamicLightStyle.bUnlit = true;
	TestEqual(TEXT("fullbright world section excludes dynamic lights"),
		WoTRuntimeBSPDynamicLightEnable(DynamicLightStyle), 0.0f);
	DynamicLightStyle.bUnlit = false;
	DynamicLightStyle.bSpecialLit = true;
	TestEqual(TEXT("special-lit world section excludes ordinary dynamic lights"),
		WoTRuntimeBSPDynamicLightEnable(DynamicLightStyle), 0.0f);
	TestEqual(TEXT("converter source export becomes a slot base"),
		WoTRuntimeBSPConverterSlotBase(TEXT("Pkg.Group.Leaf")),
		FString(TEXT("Pkg__Leaf")));
	TestTrue(TEXT("ordered converter suffixes resolve"),
		WoTRuntimeBSPConverterSlotMatches(
			TEXT("Pkg__Leaf_M_2S_U"), TEXT("Pkg__Leaf")));
	const TArray<FString> OverlappingExports = {
		TEXT("Pkg.Group.Leaf"), TEXT("Pkg.Group.Leaf_T")};
	TestEqual(TEXT("authenticated texture names outrank suffix-like endings"),
		WoTRuntimeBSPResolveConverterSlot(
			TEXT("Pkg__Leaf_T"), OverlappingExports), 1);
	FWoTRuntimeBSPSectionStyle AuthenticatedStyle;
	TestTrue(TEXT("authenticated base anchors suffix parsing"),
		WoTRuntimeBSPSectionStyleForBase(TEXT("Pkg__Leaf_T"),
			TEXT("Pkg__Leaf_T"), AuthenticatedStyle));
	TestEqual(TEXT("terminal texture-name _T is not translucency"),
		AuthenticatedStyle.Blend, EWoTRuntimeBSPBlend::Opaque);
	const TArray<FWoTRuntimeBSPConverterCandidate> AuthenticatedOverlap = {
		{TEXT("Pkg__Leaf"), TEXT("Pkg.Group.Leaf")},
		{TEXT("Pkg__Leaf_T"), TEXT("Pkg.Group.Leaf_T")}};
	TestEqual(TEXT("multiple provenance-authenticated bases are ambiguous"),
		WoTRuntimeBSPResolveAuthenticatedConverterSlot(
			TEXT("Pkg__Leaf_T"), AuthenticatedOverlap), INDEX_NONE);
	FWoTRuntimeBSPSectionStyle ExplicitIdentityStyle;
	TestTrue(TEXT("BSP-v4 explicit identity produces a section style"),
		WoTRuntimeBSPSectionStyleForIdentity(
			TEXT("AesSedaiT"),TEXT("TileSymb1127_T"),0,
			ExplicitIdentityStyle));
	TestEqual(TEXT("suffix-like canonical texture name remains the exact base"),
		ExplicitIdentityStyle.ConverterSlotBase,
		FString(TEXT("AesSedaiT__TileSymb1127_T")));
	TestEqual(TEXT("BSP-v4 blend comes from flags rather than canonical suffix"),
		ExplicitIdentityStyle.Blend,EWoTRuntimeBSPBlend::Opaque);
	TestFalse(TEXT("BSP-v4 suffix-like identity is not inferred two-sided"),
		ExplicitIdentityStyle.bTwoSided);
	TestFalse(TEXT("ordinary BSP-v4 surface is smoothed"),
		ExplicitIdentityStyle.bNoSmooth);
	TestTrue(TEXT("BSP-v4 last-writer flags drive all style bits"),
		WoTRuntimeBSPSectionStyleForIdentity(
			TEXT("Pkg"),TEXT("Leaf"),
			0x00000004u|0x00000100u|0x00000800u|0x00400000u|0x00100000u,
			ExplicitIdentityStyle)
		&& ExplicitIdentityStyle.Blend==EWoTRuntimeBSPBlend::Translucent
		&& ExplicitIdentityStyle.bTwoSided && ExplicitIdentityStyle.bUnlit
		&& ExplicitIdentityStyle.bSpecialLit && ExplicitIdentityStyle.bNoSmooth);
	{
		FWoTWorldDescriptor AmbiguousDescriptor;
		AmbiguousDescriptor.MapIdentity = TEXT("SyntheticAmbiguousSlot");
		FWoTBSPPartDescriptor& AmbiguousPart =
			AmbiguousDescriptor.BSPParts.AddDefaulted_GetRef();
		AmbiguousPart.PartIdentity = AmbiguousDescriptor.MapIdentity;
		AmbiguousPart.Role = EWoTBSPPartRole::Main;
		TArray<FWoTBSPPayloadData> AmbiguousParts = {
			WoTRuntimeMapProjectionTriangle()};
		AmbiguousParts[0].Sections[0].MaterialSlot = TEXT("Pkg__Leaf_T");
		const TArray<FWoTRuntimeBSPTestConverterResource> AmbiguousResources = {
			{false, TEXT("Pkg__Leaf"), TEXT("Pkg.Group.Leaf")},
			{false, TEXT("Pkg__Leaf_T"), TEXT("Pkg.Group.Leaf_T")},
		};
		UWorld* AmbiguousWorld =
			WoTRuntimeMapProjectionWorld(TEXT("AmbiguousSlot"));
		AWoTRuntimeBSPWorld* AmbiguousOwner = nullptr;
		uint32 AmbiguousFallbackParts = MAX_uint32;
		uint32 AmbiguousUnresolvedSlots = MAX_uint32;
		TArray<FString> AmbiguousUnresolvedNames;
		FString AmbiguousError;
		if (TestTrue(TEXT("ambiguous authenticated slot stays renderable"),
			WoTWorldBuildRuntimeBSPForTesting(*AmbiguousWorld,
				AmbiguousDescriptor, AmbiguousParts, AmbiguousOwner,
				AmbiguousFallbackParts, AmbiguousError, AmbiguousResources,
				&AmbiguousUnresolvedSlots, &AmbiguousUnresolvedNames)))
		{
			TestEqual(TEXT("ambiguous authenticated slot is counted"),
				AmbiguousUnresolvedSlots, 1u);
			TestTrue(TEXT("ambiguous authenticated slot is heartbeat-named"),
				AmbiguousUnresolvedNames == TArray<FString>{TEXT("Pkg__Leaf_T")});
			UMaterialInstanceDynamic* AmbiguousMaterial =
				AmbiguousOwner != nullptr
				&& !AmbiguousOwner->GeometryComponents.IsEmpty()
				? Cast<UMaterialInstanceDynamic>(
					AmbiguousOwner->GeometryComponents[0]->GetMaterial(0))
				: nullptr;
			if (TestNotNull(TEXT("ambiguous authenticated slot has material"),
				AmbiguousMaterial))
			{
				UTexture* Bound = nullptr;
				AmbiguousMaterial->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), Bound);
				TestEqual(TEXT("ambiguous authenticated slot binds neutral grey"),
					Bound, static_cast<UTexture*>(
						AmbiguousOwner->RuntimeTextures[0]));
			}
		}
		else AddError(AmbiguousError);
		WoTRuntimeMapProjectionDestroyWorld(AmbiguousWorld);
	}
	{
		FWoTWorldDescriptor IdentityDescriptor;
		IdentityDescriptor.MapIdentity=TEXT("Battle2_03");
		FWoTBSPPartDescriptor& IdentityPart=
			IdentityDescriptor.BSPParts.AddDefaulted_GetRef();
		IdentityPart.PartIdentity=IdentityDescriptor.MapIdentity;
		IdentityPart.Role=EWoTBSPPartRole::Main;
		TArray<FWoTBSPPayloadData> IdentityParts={
			WoTRuntimeMapProjectionTriangle()};
		IdentityParts[0].FormatVersion=4;
		FWoTBSPPayloadSection& IdentitySection=IdentityParts[0].Sections[0];
		IdentitySection.MaterialSlot=TEXT("AesSedaiT__TileSymb1127_T");
		IdentitySection.CanonicalPackage=TEXT("AesSedaiT");
		IdentitySection.CanonicalName=TEXT("TileSymb1127_T");
		IdentitySection.SourceTextureIdentity=
			TEXT("AesSedaiT.SurfaceFloor.TileSymb1127_T");
		IdentitySection.SurfaceFlags=0;
		const TArray<FWoTRuntimeBSPTestConverterResource> IdentityResources={
			{false,TEXT("AesSedaiT__TileSymb1127"),
				TEXT("AesSedaiT.SurfaceFloor.TileSymb1127"),true},
			{false,TEXT("AesSedaiT__TileSymb1127_T"),
				TEXT("AesSedaiT.SurfaceFloor.TileSymb1127_T"),true}};
		UWorld* IdentityWorld=WoTRuntimeMapProjectionWorld(
			TEXT("Battle203ExplicitIdentity"));
		AWoTRuntimeBSPWorld* IdentityOwner=nullptr;
		uint32 IdentityFallback=MAX_uint32;
		uint32 IdentityUnresolved=MAX_uint32;
		FString IdentityError;
		if (TestTrue(TEXT("Battle2_03 explicit identity section materializes"),
			WoTWorldBuildRuntimeBSPForTesting(*IdentityWorld,
				IdentityDescriptor,IdentityParts,IdentityOwner,IdentityFallback,
				IdentityError,IdentityResources,&IdentityUnresolved)))
		{
			TestEqual(TEXT("Battle2_03 explicit identity reports unresolved_slots=0"),
				IdentityUnresolved,0u);
			UMaterialInstanceDynamic* IdentityMaterial=IdentityOwner!=nullptr
				&& !IdentityOwner->GeometryComponents.IsEmpty()
				? Cast<UMaterialInstanceDynamic>(
					IdentityOwner->GeometryComponents[0]->GetMaterial(0)) : nullptr;
			if (TestNotNull(TEXT("Battle2_03 identity material"),IdentityMaterial))
			{
				TestTrue(TEXT("canonical _T name does not select translucent master"),
					IdentityMaterial->Parent!=nullptr
					&& IdentityMaterial->Parent->GetName()==TEXT("M_WoT_World"));
			}
		}
		else AddError(IdentityError);
		WoTRuntimeMapProjectionDestroyWorld(IdentityWorld);
	}
	struct FAuthenticatedSlotCase
	{
		const TCHAR* Slot;
		const TCHAR* SourceExport;
		EWoTRuntimeBSPBlend Blend;
		bool bTwoSided;
		bool bSky;
	};
	const FAuthenticatedSlotCase AuthenticatedCases[] = {
		{TEXT("JamesT__WATR_M08"), TEXT("JamesT.Skins.WATR_M08"),
			EWoTRuntimeBSPBlend::Opaque, false, false},
		{TEXT("Nature_T__Sky_440_T_T_2S"),
			TEXT("Nature_T.Skins.Sky_440_T"),
			EWoTRuntimeBSPBlend::Translucent, true, false},
		{TEXT("Mordeth_T__PlasArch4376_T"),
			TEXT("Mordeth_T.Skins.PlasArch4376_T"),
			EWoTRuntimeBSPBlend::Opaque, false, false},
		{TEXT("NatureT__Sky_440"), TEXT("NatureT.Skins.Sky_440"),
			EWoTRuntimeBSPBlend::Opaque, false, false},
	};
	for (const FAuthenticatedSlotCase& AuthenticatedCase : AuthenticatedCases)
	{
		const FString SourceExport(AuthenticatedCase.SourceExport);
		const TArray<FWoTRuntimeBSPConverterCandidate> Candidates = {{
			WoTRuntimeBSPConverterSlotBase(SourceExport), SourceExport}};
		const int32 Resolved =
			WoTRuntimeBSPResolveAuthenticatedConverterSlot(
				AuthenticatedCase.Slot, Candidates);
		TestEqual(AuthenticatedCase.Slot, Resolved, 0);
		if (Resolved == 0)
		{
			FWoTRuntimeBSPSectionStyle Style;
			const FString Base = WoTRuntimeBSPConverterSlotBase(
				Candidates[Resolved].SourceExport);
			TestTrue(FString(AuthenticatedCase.Slot) + TEXT(" style"),
				WoTRuntimeBSPSectionStyleForBase(
					AuthenticatedCase.Slot, Base, Style));
			TestEqual(TEXT("authenticated blend"),
				Style.Blend, AuthenticatedCase.Blend);
			TestEqual(TEXT("authenticated two-sided"),
				Style.bTwoSided, AuthenticatedCase.bTwoSided);
			TestEqual(TEXT("authenticated sky"),
				Style.bSky, AuthenticatedCase.bSky);
		}
	}
	const TArray<FWoTRuntimeBSPConverterCandidate> MismatchedProvenance = {{
		TEXT("ArenaTex__Backdrop"), TEXT("OtherPkg.Skins.Backdrop")}};
	TestEqual(TEXT("mismatched source.export is not authenticated"),
		WoTRuntimeBSPResolveAuthenticatedConverterSlot(
			TEXT("ArenaTex__Backdrop_Sky"), MismatchedProvenance), INDEX_NONE);
	const TArray<FString> MixedCaseExport = {
		TEXT("DeveloperT.Skins.Developer")};
	TestEqual(TEXT("authenticated converter bases compare case-insensitively"),
		WoTRuntimeBSPConverterSlotMatchesFolded(
			TEXT("DEVELOPERT__DEVELOPER_T"),
			WoTRuntimeBSPConverterSlotBase(MixedCaseExport[0])), true);
	FWoTRuntimeBSPSectionStyle MixedCaseStyle;
	TestTrue(TEXT("mixed-case authenticated base anchors suffix parsing"),
		WoTRuntimeBSPSectionStyleForBase(
			TEXT("DEVELOPERT__DEVELOPER_T"),
			TEXT("DeveloperT__Developer"), MixedCaseStyle));
	TestEqual(TEXT("mixed-case suffix remains translucent"),
		MixedCaseStyle.Blend, EWoTRuntimeBSPBlend::Translucent);
	FWoTRuntimeBSPSectionStyle EmbeddedTokenStyle;
	TestTrue(TEXT("suffix-like leaf fragment is not a converter suffix"),
		WoTRuntimeBSPSectionStyle(
			TEXT("JamesT__WATR_M08"), EmbeddedTokenStyle));
	TestEqual(TEXT("suffix-like leaf remains opaque"),
		EmbeddedTokenStyle.Blend, EWoTRuntimeBSPBlend::Opaque);
	TestTrue(TEXT("sky-prefixed leaf remains a valid ordinary slot"),
		WoTRuntimeBSPSectionStyle(
			TEXT("NatureT__Sky_440"), EmbeddedTokenStyle));
	TestFalse(TEXT("sky-prefixed leaf is not fake backdrop"),
		EmbeddedTokenStyle.bSky);
	TestTrue(TEXT("missing converter base is an unresolved conservative style"),
		WoTRuntimeBSPSectionStyle(TEXT("__None"), EmbeddedTokenStyle));
	TestEqual(TEXT("unresolved conservative style is opaque"),
		EmbeddedTokenStyle.Blend, EWoTRuntimeBSPBlend::Opaque);
	TestFalse(TEXT("unresolved conservative style is visible"),
		EmbeddedTokenStyle.bSky);

	UTexture2D* Neutral = WoTRuntimeBSPMaterializeNeutralLightmap();
	if (TestNotNull(TEXT("neutral lightmap exists"), Neutral)
		&& Neutral->GetPlatformData() != nullptr
		&& Neutral->GetPlatformData()->Mips.Num() == 1)
	{
		const FColor* Pixel = static_cast<const FColor*>(
			Neutral->GetPlatformData()->Mips[0].BulkData.LockReadOnly());
		if (TestNotNull(TEXT("neutral texel can be inspected"), Pixel))
		{
			TestEqual(TEXT("native composer neutral texel"),
				*Pixel, FColor(128, 128, 128, 255));
		}
		Neutral->GetPlatformData()->Mips[0].BulkData.Unlock();
	}
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPMacroTextureSpec,
	"WoT.World.RuntimeBSP.MacroTextureBinding",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPMacroTextureSpec::RunTest(const FString&)
{
	FWoTWorldDescriptor Descriptor;
	Descriptor.MapIdentity = TEXT("SyntheticMacroTexture");
	FWoTBSPPartDescriptor& PartDescriptor =
		Descriptor.BSPParts.AddDefaulted_GetRef();
	PartDescriptor.PartIdentity = Descriptor.MapIdentity;
	PartDescriptor.Role = EWoTBSPPartRole::Main;

	TArray<FWoTBSPPayloadData> Parts = {WoTRuntimeMapProjectionTriangle()};
	Parts[0].FormatVersion = 10;
	Parts[0].TriangleSurfaces = {0};
	FWoTBSPPayloadSection& Section = Parts[0].Sections[0];
	Section.MaterialSlot = TEXT("BaseT__Stone");
	Section.CanonicalPackage = TEXT("BaseT");
	Section.CanonicalName = TEXT("Stone");
	Section.SourceTextureIdentity = TEXT("BaseT.Walls.Stone");
	Section.MacroCanonicalPackage = TEXT("MacroT");
	Section.MacroCanonicalName = TEXT("BigStone");
	Section.MacroSourceTextureIdentity = TEXT("MacroT.Macro.BigStone");
	Section.MacroRatio = 2.0f;
	const TArray<FWoTRuntimeBSPTestConverterResource> Resources = {
		{false, TEXT("BaseT__Stone"), TEXT("BaseT.Walls.Stone"), true},
		{false, TEXT("MacroT__BigStone"), TEXT("MacroT.Macro.BigStone"), true},
	};

	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("MacroTextureBinding"));
	if (!TestNotNull(TEXT("macro binding world exists"), World)) return false;
	AWoTRuntimeBSPWorld* Owner = nullptr;
	uint32 FallbackParts = MAX_uint32;
	uint32 UnresolvedSlots = MAX_uint32;
	FString Error;
	if (TestTrue(TEXT("macro section materializes"),
		WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
			FallbackParts, Error, Resources, &UnresolvedSlots)))
	{
		TestEqual(TEXT("macro section reports no unresolved base slot"),
			UnresolvedSlots, 0u);
		UMaterialInstanceDynamic* Material = Owner != nullptr
			&& !Owner->GeometryComponents.IsEmpty()
			? Cast<UMaterialInstanceDynamic>(
				Owner->GeometryComponents[0]->GetMaterial(0)) : nullptr;
		if (TestNotNull(TEXT("macro section has a material"), Material))
		{
			TestTrue(TEXT("macro section uses the world master"),
				Material->Parent != nullptr
				&& Material->Parent->GetName() == TEXT("M_WoT_World"));
			UTexture* MacroTexture = nullptr;
			Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("MacroTex")), MacroTexture);
			if (TestTrue(TEXT("macro stand-in was retained separately"),
				Owner != nullptr && Owner->RuntimeTextures.Num() >= 4))
			{
				UTexture* ExpectedMacro = Owner->RuntimeTextures.Last();
				TestNotEqual(TEXT("macro stand-in differs from the base stand-in"),
					ExpectedMacro, static_cast<UTexture*>(Owner->RuntimeTextures[0]));
				TestEqual(TEXT("macro texture binds the exact macro stand-in"),
					MacroTexture, ExpectedMacro);
			}
			float MacroRatio = 0.0f;
			Material->GetScalarParameterValue(
				FHashedMaterialParameterInfo(TEXT("MacroRatio")), MacroRatio);
			TestEqual(TEXT("macro UV ratio is bound"), MacroRatio, 2.0f);
		}
	}
	else AddError(Error);
	WoTRuntimeMapProjectionDestroyWorld(World);

	// A two-sided opaque section rides the Entry clone of M_WoT_World, which
	// carries the same macro term (entry_weather_materials.py MaterialSpec).
	Section.SurfaceFlags |= 0x00000100u; // PF_TwoSided
	UWorld* TwoSidedWorld =
		WoTRuntimeMapProjectionWorld(TEXT("MacroTextureBindingTwoSided"));
	if (!TestNotNull(TEXT("two-sided macro world exists"), TwoSidedWorld))
		return false;
	AWoTRuntimeBSPWorld* TwoSidedOwner = nullptr;
	Error.Reset();
	if (TestTrue(TEXT("two-sided macro section materializes"),
		WoTWorldBuildRuntimeBSPForTesting(*TwoSidedWorld, Descriptor, Parts,
			TwoSidedOwner, FallbackParts, Error, Resources, &UnresolvedSlots)))
	{
		UMaterialInstanceDynamic* Material = TwoSidedOwner != nullptr
			&& !TwoSidedOwner->GeometryComponents.IsEmpty()
			? Cast<UMaterialInstanceDynamic>(
				TwoSidedOwner->GeometryComponents[0]->GetMaterial(0)) : nullptr;
		if (TestNotNull(TEXT("two-sided macro section has a material"), Material))
		{
			TestTrue(TEXT("two-sided macro section uses the two-sided world clone"),
				Material->Parent != nullptr
				&& Material->Parent->GetName() == TEXT("M_WoT_EntryWorld_TwoSided"));
			UTexture* MacroTexture = nullptr;
			TestTrue(TEXT("two-sided clone exposes MacroTex"),
				Material->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("MacroTex")), MacroTexture));
			TestTrue(TEXT("two-sided macro stand-in is bound"),
				TwoSidedOwner->RuntimeTextures.Num() >= 4
				&& MacroTexture == TwoSidedOwner->RuntimeTextures.Last());
			float MacroRatio = 0.0f;
			Material->GetScalarParameterValue(
				FHashedMaterialParameterInfo(TEXT("MacroRatio")), MacroRatio);
			TestEqual(TEXT("two-sided macro UV ratio is bound"), MacroRatio, 2.0f);
		}
	}
	else AddError(Error);
	WoTRuntimeMapProjectionDestroyWorld(TwoSidedWorld);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPDetailTextureSpec,
	"WoT.World.RuntimeBSP.DetailTextureBinding",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPDetailTextureSpec::RunTest(const FString&)
{
	// Registry facts for real retail textures (utx2png manifests), with the
	// per-axis Glide texel rates: (W*DS)/(dW*dDS) on U, (H*DS)/(dH*dDS) on V.
	struct FExpectedDetail
	{
		const TCHAR* Package;
		const TCHAR* Name;
		const TCHAR* DetailSource;
		float RatioU;
		float RatioV;
	};
	static const FExpectedDetail ExpectedDetails[] = {
		// 256x256 DS 1 over detail_stone 256x256 DS 0.25.
		{TEXT("WhiteCloakT"), TEXT("BrikWall3720"),
			TEXT("WOTDetail.Detail.detail_stone"), 4.0f, 4.0f},
		// A 64x256 trim: one ratio for both axes would stretch V by 4.
		{TEXT("AesSedaiT"), TEXT("MarbVert1940"),
			TEXT("WOTDetail.Detail.detail_stone"), 1.0f, 4.0f},
		// 128x64 DS 2 over DetailMarble_010c 256x256 DS 0.1.
		{TEXT("AesSedai_T"), TEXT("MarbRed1910_T"),
			TEXT("WOTDetail.DetailMarble_010c"), 10.0f, 5.0f},
	};
	for (const FExpectedDetail& Row : ExpectedDetails)
	{
		const FString Label = FString(Row.Package) + TEXT(".") + Row.Name;
		FWoTRuntimeBSPDetailTexture Detail;
		if (!TestTrue(Label + TEXT(" authors a DetailTexture"),
			WoTRuntimeBSPFindDetailTexture(Row.Package, Row.Name, Detail)))
			continue;
		TestEqual(Label + TEXT(" detail source"), Detail.SourceTextureIdentity,
			FString(Row.DetailSource));
		TestTrue(Label + TEXT(" detail U ratio"),
			FMath::IsNearlyEqual(Detail.RatioU, Row.RatioU, 1.0e-5f));
		TestTrue(Label + TEXT(" detail V ratio"),
			FMath::IsNearlyEqual(Detail.RatioV, Row.RatioV, 1.0e-5f));
	}
	FWoTRuntimeBSPDetailTexture Absent;
	TestFalse(TEXT("a texture without DetailTexture has none"),
		WoTRuntimeBSPFindDetailTexture(TEXT("AesSedaiT"), TEXT("BookBook1752"), Absent));
	TestFalse(TEXT("an unknown texture has none"),
		WoTRuntimeBSPFindDetailTexture(TEXT("NoSuchT"), TEXT("Nothing"), Absent));
	TestTrue(TEXT("the canonical lookup folds case like UE1 names"),
		WoTRuntimeBSPFindDetailTexture(TEXT("whitecloakt"), TEXT("BRIKWALL3720"), Absent)
		&& Absent.RatioU == 4.0f);

	// One part, two opaque sections that share detail_stone.
	auto MakePart = [](const TCHAR* FirstPackage, const TCHAR* FirstName,
		const TCHAR* FirstSource, uint32 FirstFlags)
	{
		FWoTBSPPayloadData Part = WoTRuntimeMapProjectionTriangle();
		Part.FormatVersion = 10;
		Part.BoundsMax = FVector3f(300, 100, 0);
		Part.Positions.Append({
			FVector3f(200, 0, 0), FVector3f(300, 0, 0), FVector3f(200, 100, 0)});
		Part.Normals.Append({
			FVector3f::ZAxisVector, FVector3f::ZAxisVector, FVector3f::ZAxisVector});
		Part.UV0.Append({FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
		Part.UV1 = Part.UV0;
		Part.Indices.Append({3, 4, 5});
		Part.TriangleSurfaces = {0, 1};
		FWoTBSPPayloadSection Second = Part.Sections[0];
		Second.FirstIndex = 3;
		Second.MinVertex = 3;
		Second.MaxVertex = 5;
		Second.MaterialSlot = TEXT("AesSedaiT__MarbVert1940");
		Second.CanonicalPackage = TEXT("AesSedaiT");
		Second.CanonicalName = TEXT("MarbVert1940");
		Second.SourceTextureIdentity = TEXT("AesSedaiT.DetailArchitect.MarbVert1940");
		Second.SurfaceFlags = 0;
		FWoTBSPPayloadSection& First = Part.Sections[0];
		First.MaterialSlot = FString(FirstPackage) + TEXT("__") + FirstName;
		First.CanonicalPackage = FirstPackage;
		First.CanonicalName = FirstName;
		First.SourceTextureIdentity = FirstSource;
		First.SurfaceFlags = FirstFlags;
		Part.Sections.Add(Second);
		return Part;
	};
	FWoTWorldDescriptor Descriptor;
	Descriptor.MapIdentity = TEXT("SyntheticDetailTexture");
	FWoTBSPPartDescriptor& PartDescriptor =
		Descriptor.BSPParts.AddDefaulted_GetRef();
	PartDescriptor.PartIdentity = Descriptor.MapIdentity;
	PartDescriptor.Role = EWoTBSPPartRole::Main;
	const TArray<FWoTRuntimeBSPTestConverterResource> Resources = {
		{false, TEXT("WhiteCloakT__BrikWall3720"), TEXT("WhiteCloakT.BrikWall3720"), true},
		{false, TEXT("AesSedaiT__MarbVert1940"),
			TEXT("AesSedaiT.DetailArchitect.MarbVert1940"), true},
		{false, TEXT("AesSedaiT__BookBook1752"),
			TEXT("AesSedaiT.DetailArchitect.BookBook1752"), true},
		{false, TEXT("WOTDetail__detail_stone"), TEXT("WOTDetail.Detail.detail_stone"), true},
	};
	auto SectionMaterials = [](AWoTRuntimeBSPWorld* Owner)
	{
		// One material instance per materialized section, whatever component
		// its blend lands in.
		TArray<UMaterialInstanceDynamic*> Materials;
		if (Owner == nullptr) return Materials;
		for (UMaterialInstanceDynamic* Material : Owner->RuntimeMaterials)
			Materials.Add(Material);
		return Materials;
	};
	auto BoundDetail = [](UMaterialInstanceDynamic* Material)
	{
		UTexture* Texture = nullptr;
		if (Material != nullptr)
			Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("DetailTex")), Texture);
		return Texture;
	};
	auto BoundScalar = [](UMaterialInstanceDynamic* Material, const TCHAR* Name)
	{
		float Value = -1.0f;
		if (Material != nullptr)
			Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value);
		return Value;
	};

	UTexture* SharedDetail = nullptr;
	{
		TArray<FWoTBSPPayloadData> Parts = {MakePart(TEXT("WhiteCloakT"),
			TEXT("BrikWall3720"), TEXT("WhiteCloakT.BrikWall3720"), 0)};
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("DetailTextureBinding"));
		if (!TestNotNull(TEXT("detail binding world exists"), World)) return false;
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		uint32 UnresolvedSlots = MAX_uint32;
		FString Error;
		if (TestTrue(TEXT("detail sections materialize"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, Resources, &UnresolvedSlots)))
		{
			TestEqual(TEXT("detail sections report no unresolved base slot"),
				UnresolvedSlots, 0u);
			const TArray<UMaterialInstanceDynamic*> Materials = SectionMaterials(Owner);
			if (TestEqual(TEXT("two section materials"), Materials.Num(), 2))
			{
				UMaterialInstanceDynamic* Wall = nullptr;
				UMaterialInstanceDynamic* Trim = nullptr;
				for (UMaterialInstanceDynamic* Material : Materials)
				{
					if (BoundScalar(Material, TEXT("DetailRatio")) == 4.0f) Wall = Material;
					else if (BoundScalar(Material, TEXT("DetailRatio")) == 1.0f) Trim = Material;
				}
				if (TestNotNull(TEXT("the wall binds DetailRatio 4"), Wall)
					&& TestNotNull(TEXT("the trim binds DetailRatio 1"), Trim))
				{
					TestTrue(TEXT("the wall uses the world master"),
						Wall->Parent != nullptr
						&& Wall->Parent->GetName() == TEXT("M_WoT_World"));
					TestEqual(TEXT("the square wall binds DetailRatioV 4"),
						BoundScalar(Wall, TEXT("DetailRatioV")), 4.0f);
					TestEqual(TEXT("the 64x256 trim binds DetailRatioV 4"),
						BoundScalar(Trim, TEXT("DetailRatioV")), 4.0f);
					SharedDetail = BoundDetail(Wall);
					if (TestNotNull(TEXT("the wall binds a DetailTex"), SharedDetail))
					{
						TestEqual(TEXT("both sections share one detail texture"),
							BoundDetail(Trim), SharedDetail);
						UTexture* WallBase = nullptr;
						Wall->GetTextureParameterValue(
							FHashedMaterialParameterInfo(TEXT("Tex")), WallBase);
						TestNotEqual(TEXT("the detail stand-in is not the base stand-in"),
							SharedDetail, WallBase);
						int32 Retained = 0;
						for (UTexture2D* Texture : Owner->RuntimeTextures)
							Retained += Texture == SharedDetail ? 1 : 0;
						TestEqual(TEXT("the shared detail texture is retained once"),
							Retained, 1);
					}
				}
			}
		}
		else AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
	}

	// A translucent section and a texture without DetailTexture bind nothing.
	{
		TArray<FWoTBSPPayloadData> Parts = {MakePart(TEXT("AesSedaiT"),
			TEXT("BookBook1752"), TEXT("AesSedaiT.DetailArchitect.BookBook1752"), 0)};
		Parts[0].Sections[1].SurfaceFlags = 0x00000004u; // PF_Translucent
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("DetailTextureNone"));
		if (!TestNotNull(TEXT("detail-free world exists"), World)) return false;
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		FString Error;
		if (TestTrue(TEXT("detail-free sections materialize"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, Resources)))
		{
			const TArray<UMaterialInstanceDynamic*> Materials = SectionMaterials(Owner);
			TestEqual(TEXT("two detail-free section materials"), Materials.Num(), 2);
			for (UMaterialInstanceDynamic* Material : Materials)
			{
				UTexture* Detail = BoundDetail(Material);
				TestTrue(TEXT("no section binds a detail texture"),
					Detail == nullptr
					|| Detail->GetName() == TEXT("T_WoT_Neutral"));
				TestTrue(TEXT("no section binds a detail ratio"),
					BoundScalar(Material, TEXT("DetailRatio")) == -1.0f
					|| BoundScalar(Material, TEXT("DetailRatio")) == 1.0f);
			}
		}
		else AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPSkyCollisionSpec,
	"WoT.World.RuntimeBSP.SkyWindowCollisionAndDepth",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPSkyCollisionSpec::RunTest(const FString&)
{
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("SkyCollision"));
	if (!TestNotNull(TEXT("sky collision world exists"), World)) return false;
	FWoTWorldDescriptor Descriptor;
	Descriptor.MapIdentity = TEXT("SyntheticSkyCollision");
	FWoTBSPPartDescriptor& PartDescriptor =
		Descriptor.BSPParts.AddDefaulted_GetRef();
	PartDescriptor.PartIdentity = Descriptor.MapIdentity;
	PartDescriptor.Role = EWoTBSPPartRole::Main;
	const TArray<FWoTBSPPayloadData> Parts = {
		WoTRuntimeMapProjectionVisibleAndSkyTriangles()};
	const TArray<FWoTRuntimeBSPTestConverterResource> Authenticated = {
		{false, TEXT("DeveloperT__Developer"),
			TEXT("DeveloperT.Skins.Developer")},
		{false, TEXT("ArenaTex__Backdrop"), TEXT("ArenaTex.Skins.Backdrop")},
	};
	AWoTRuntimeBSPWorld* Owner = nullptr;
	uint32 FallbackParts = MAX_uint32;
	uint32 UnresolvedSlots = MAX_uint32;
	FString Error;
	if (!TestTrue(TEXT("authenticated materializer builds sky-filtered part"),
		WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts,
			Owner, FallbackParts, Error, Authenticated, &UnresolvedSlots)))
	{
		AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	if (!TestNotNull(TEXT("sky collision owner exists"), Owner)
		|| !TestEqual(TEXT("one materialized component"),
			Owner->GeometryComponents.Num(), 1))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	TestEqual(TEXT("missing synthetic payload is counted once"),
		UnresolvedSlots, 1u);
	UStaticMeshComponent* Component = Owner->GeometryComponents[0];
	UStaticMesh* Mesh = Component != nullptr ? Component->GetStaticMesh() : nullptr;
	if (!TestNotNull(TEXT("materialized sky component exists"), Component)
		|| !TestNotNull(TEXT("materialized sky mesh exists"), Mesh))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (TestNotNull(TEXT("visible render data exists"), RenderData))
	{
		TestEqual(TEXT("the ordinary and sky-window sections render"),
			RenderData->LODResources[0].Sections.Num(), 2);
		TestEqual(TEXT("the ordinary and sky-window triangles render"),
			RenderData->LODResources[0].GetNumTriangles(), 2u);
	}
	UMaterialInstanceDynamic* WindowMaterial =
		Cast<UMaterialInstanceDynamic>(Component->GetMaterial(1));
	if (TestNotNull(TEXT("sky window has a material"), WindowMaterial))
	{
		TestEqual(TEXT("sky window uses the dedicated master"),
			WindowMaterial->Parent->GetName(), FString(TEXT("M_WoT_SkyWindow")));
		TestEqual(TEXT("sky window is opaque and therefore depth-writing"),
			WindowMaterial->GetBlendMode(), BLEND_Opaque);
	}
	UMaterialInstanceDynamic* Material =
		Cast<UMaterialInstanceDynamic>(Component->GetMaterial(0));
	if (TestNotNull(TEXT("mixed-case authenticated material exists"), Material))
	{
		TestEqual(TEXT("mixed-case authenticated suffix selects translucent master"),
			Material->Parent->GetName(), FString(TEXT("M_WoT_EntryTranslucent")));
		UTexture* Bound = nullptr;
		Material->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), Bound);
		TestEqual(TEXT("missing synthetic texture binds neutral grey"), Bound,
			static_cast<UTexture*>(Owner->RuntimeTextures[0]));
	}
	Owner->FinishSpawning(FTransform::Identity, true);
	World->InitWorld(UWorld::InitializationValues()
		.AllowAudioPlayback(false)
		.RequiresHitProxies(false)
		.CreateNavigation(false)
		.CreateAISystem(false)
		.CreateFXSystem(false));
	World->UpdateWorldComponents(false, false);
	FURL URL;
	World->InitializeActorsForPlay(URL);
	World->BeginPlay();
	TestTrue(TEXT("sky collision component registers"), Component->IsRegistered());
	TestTrue(TEXT("sky collision component has physics state"),
		Component->IsPhysicsStateCreated());
	TestTrue(TEXT("sky collision component has a valid body"),
		Component->BodyInstance.IsValidBodyInstance());
	auto TraceTriangle = [Component](float X)
	{
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(RuntimeBSPSkyCollision), true);
		return Component->LineTraceComponent(
			Hit, FVector(X, 25, 100), FVector(X, 25, -100), Query);
	};
	TestTrue(TEXT("visible triangle blocks traces"), TraceTriangle(25));
	TestTrue(TEXT("opaque sky-window triangle blocks traces"), TraceTriangle(225));
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

// Retail: the original renderer draws a texture-less surface with
// Level->DefaultTexture. Arena_09's world model has two NULL-texture
// surfaces and Battle3_02 imports WOT.WaterEffects.Water.Watr_400, which no
// shipped package exports: both render Engine.DefaultTexture in retail and in
// the dev import (import_map.py:1038-1047). Battle3_01/Battle4_01 reference
// AesSedaiT.Ornament.{OrnaTrim1505,PaprWall1441}; the cache keeps only the
// last same-named export (DetailArchitect/SurfaceWall), so that identity is a
// cache gap - NOT dangling - and stays an honest unresolved slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPRetailDefaultTextureSpec,
	"WoT.World.RuntimeBSP.RetailDefaultTexture",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPRetailDefaultTextureSpec::RunTest(const FString&)
{
	struct FSectionCase
	{
		const TCHAR* Slot;
		const TCHAR* Package;
		const TCHAR* Name;
		const TCHAR* Source;
	};
	static const FSectionCase Cases[] = {
		{TEXT("__None"), TEXT(""), TEXT("None"), TEXT("None")},
		{TEXT("WOT__Watr_400"), TEXT("WOT"), TEXT("Watr_400"),
			TEXT("WOT.WaterEffects.Water.Watr_400")},
		{TEXT("AesSedaiT__PaprWall1441"), TEXT("AesSedaiT"), TEXT("PaprWall1441"),
			TEXT("AesSedaiT.Ornament.PaprWall1441")},
		{TEXT("AesSedaiT__OrnaTrim1505"), TEXT("AesSedaiT"), TEXT("OrnaTrim1505"),
			TEXT("AesSedaiT.DetailArchitect.OrnaTrim1505")},
	};
	TArray<FWoTBSPPayloadData> Parts;
	FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
	Part.FormatVersion = 4;
	Part.bPortal = false;
	Part.BoundsMin = FVector3f(0, 0, 0);
	Part.BoundsMax = FVector3f(
		100.0f * static_cast<float>(UE_ARRAY_COUNT(Cases)), 100.0f, 0.0f);
	for (int32 CaseIndex = 0; CaseIndex < static_cast<int32>(UE_ARRAY_COUNT(Cases));
		++CaseIndex)
	{
		const float OffsetX = 100.0f * static_cast<float>(CaseIndex);
		const uint32 First = static_cast<uint32>(Part.Positions.Num());
		Part.Positions.Append({FVector3f(OffsetX, 0, 0),
			FVector3f(OffsetX + 100.0f, 0, 0), FVector3f(OffsetX, 100, 0)});
		Part.Normals.Append({FVector3f::ZAxisVector, FVector3f::ZAxisVector,
			FVector3f::ZAxisVector});
		Part.UV0.Append({FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
		Part.UV1.Append({FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
		FWoTBSPPayloadSection& Section = Part.Sections.AddDefaulted_GetRef();
		Section.FirstIndex = static_cast<uint32>(Part.Indices.Num());
		Part.Indices.Append({First, First + 1, First + 2});
		Section.IndexCount = 3;
		Section.MinVertex = First;
		Section.MaxVertex = First + 2;
		Section.bCollisionEnabled = true;
		Section.MaterialSlot = Cases[CaseIndex].Slot;
		Section.CanonicalPackage = Cases[CaseIndex].Package;
		Section.CanonicalName = Cases[CaseIndex].Name;
		Section.SourceTextureIdentity = Cases[CaseIndex].Source;
		Section.SurfaceFlags = 0;
	}
	FWoTWorldDescriptor Descriptor;
	Descriptor.MapIdentity = TEXT("SyntheticDanglingTextures");
	FWoTBSPPartDescriptor& PartDescriptor = Descriptor.BSPParts.AddDefaulted_GetRef();
	PartDescriptor.PartIdentity = Descriptor.MapIdentity;
	PartDescriptor.Role = EWoTBSPPartRole::Main;
	// The cached objects exactly as the production contract names them.
	const TArray<FWoTRuntimeBSPTestConverterResource> CacheOnly = {
		{false, TEXT("AesSedaiT__PaprWall1441"),
			TEXT("AesSedaiT.SurfaceWall.PaprWall1441"), true},
		{false, TEXT("AesSedaiT__OrnaTrim1505"),
			TEXT("AesSedaiT.DetailArchitect.OrnaTrim1505"), true},
	};
	TArray<FWoTRuntimeBSPTestConverterResource> WithDefault = CacheOnly;
	WithDefault.Add({false, TEXT("Engine__DefaultTexture"),
		TEXT("Engine.DefaultTexture"), true});

	{
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("RetailDefaultTexture"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		uint32 Unresolved = MAX_uint32;
		uint32 DefaultSlots = MAX_uint32;
		TArray<FString> UnresolvedNames;
		FString Error;
		if (TestTrue(TEXT("dangling and aliased sections still materialize"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, WithDefault, &Unresolved, &UnresolvedNames,
				nullptr, nullptr, EWoTRuntimeSkyStageFault::None, &DefaultSlots)))
		{
			TestEqual(TEXT("NULL and dangling-import sections bind Engine.DefaultTexture"),
				DefaultSlots, 2u);
			TestEqual(TEXT("only the group-aliased identity stays unresolved"),
				Unresolved, 1u);
			TestTrue(TEXT("the unresolved slot is named for the heartbeat"),
				UnresolvedNames == TArray<FString>{TEXT("AesSedaiT__PaprWall1441")});
		}
		else AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
	}

	// The production contract carries every colliding export
	// under its EXACT identity (Pkg.Group__Name, Tools/dist/retail_identity.py)
	// and no plain alias. The Ornament sections resolve exact-first, the
	// DetailArchitect one too, and nothing stays unresolved.
	{
		TArray<FWoTRuntimeBSPTestConverterResource> Exact = {
			{false, TEXT("AesSedaiT.Ornament__PaprWall1441"),
				TEXT("AesSedaiT.Ornament.PaprWall1441"), true},
			{false, TEXT("AesSedaiT.SurfaceWall__PaprWall1441"),
				TEXT("AesSedaiT.SurfaceWall.PaprWall1441"), true},
			{false, TEXT("AesSedaiT.Ornament__OrnaTrim1505"),
				TEXT("AesSedaiT.Ornament.OrnaTrim1505"), true},
			{false, TEXT("AesSedaiT.DetailArchitect__OrnaTrim1505"),
				TEXT("AesSedaiT.DetailArchitect.OrnaTrim1505"), true},
			{false, TEXT("Engine__DefaultTexture"), TEXT("Engine.DefaultTexture"), true},
		};
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("RetailExactIdentity"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		uint32 Unresolved = MAX_uint32;
		uint32 DefaultSlots = MAX_uint32;
		TArray<FString> UnresolvedNames;
		FString Error;
		if (TestTrue(TEXT("exact-identity sections materialize"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, Exact, &Unresolved, &UnresolvedNames,
				nullptr, nullptr, EWoTRuntimeSkyStageFault::None, &DefaultSlots)))
		{
			TestEqual(TEXT("exact identities leave no unresolved slot"), Unresolved, 0u);
			TestTrue(TEXT("no unresolved slot is named"), UnresolvedNames.IsEmpty());
			TestEqual(TEXT("NULL and dangling sections still bind DefaultTexture"),
				DefaultSlots, 2u);
		}
		else AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
	}

	// Without an authenticated Engine.DefaultTexture nothing is invented: every
	// unmatched section keeps the conservative neutral grey and is counted.
	bool bRegistryHasDefault = false;
	for (const FWoTNeutralResourceId& Resource :
		FWoTRuntimeRegistry::Get().ListResources(EWoTNeutralResourceKind::Texture))
	{
		bRegistryHasDefault |= Resource.LogicalId.Equals(
			TEXT("cache/texture/Engine__DefaultTexture"), ESearchCase::IgnoreCase);
	}
	if (bRegistryHasDefault)
	{
		AddInfo(TEXT("an active registry serves Engine.DefaultTexture; fail-closed leg skipped"));
		return !HasAnyErrors();
	}
	{
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("RetailDefaultAbsent"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		uint32 Unresolved = MAX_uint32;
		uint32 DefaultSlots = MAX_uint32;
		FString Error;
		if (TestTrue(TEXT("missing default texture still materializes"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, CacheOnly, &Unresolved, nullptr,
				nullptr, nullptr, EWoTRuntimeSkyStageFault::None, &DefaultSlots)))
		{
			TestEqual(TEXT("no default-texture slot without the authenticated object"),
				DefaultSlots, 0u);
			TestEqual(TEXT("all three unmatched sections are counted"),
				Unresolved, 3u);
			if (Owner != nullptr && !Owner->GeometryComponents.IsEmpty())
			{
				UMaterialInstanceDynamic* Material = Cast<UMaterialInstanceDynamic>(
					Owner->GeometryComponents[0]->GetMaterial(0));
				UTexture* Bound = nullptr;
				if (Material != nullptr)
					Material->GetTextureParameterValue(
						FHashedMaterialParameterInfo(TEXT("Tex")), Bound);
				TestEqual(TEXT("the NULL section falls back to neutral grey"),
					Bound, static_cast<UTexture*>(Owner->RuntimeTextures[0]));
			}
		}
		else AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPCollisionCookIdentitySpec,
	"WoT.World.RuntimeBSP.CollisionCookUsesCurrentPayload",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPCollisionCookIdentitySpec::RunTest(const FString&)
{
	const FName ReusedMeshName(*FString::Printf(
		TEXT("RuntimeBSPCollisionCookGuard_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FWoTBSPPayloadData FirstPayload =
		WoTRuntimeMapProjectionLargeCollisionPlane(0.0f);
	const FWoTBSPPayloadData CurrentPayload =
		WoTRuntimeMapProjectionLargeCollisionPlane(500.0f);
	FString Error;
	FString ReusedMeshPath;
	{
		UStaticMesh* FirstMesh = NewObject<UStaticMesh>(
			GetTransientPackage(), ReusedMeshName, RF_Transient);
		ReusedMeshPath = FirstMesh->GetPathName();
		if (!TestTrue(TEXT("first large render mesh builds"),
			WoTRuntimeBSPBuildRenderMesh(*FirstMesh, FirstPayload, Error))
			|| !TestTrue(TEXT("first large collision mesh cooks"),
				WoTRuntimeBSPBuildCollision(*FirstMesh, FirstPayload, Error)))
		{
			AddError(Error);
			return false;
		}
	}
	CollectGarbage(RF_NoFlags, true);

	UStaticMesh* CurrentMesh = NewObject<UStaticMesh>(
		GetTransientPackage(), ReusedMeshName, RF_Transient);
	TestEqual(TEXT("the transient mesh path is deliberately reused"),
		CurrentMesh->GetPathName(), ReusedMeshPath);
	Error.Reset();
	if (!TestTrue(TEXT("current large render mesh builds"),
		WoTRuntimeBSPBuildRenderMesh(*CurrentMesh, CurrentPayload, Error))
		|| !TestTrue(TEXT("current large collision mesh cooks"),
			WoTRuntimeBSPBuildCollision(*CurrentMesh, CurrentPayload, Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("CollisionCookIdentity"));
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = World->PersistentLevel;
	Parameters.bDeferConstruction = true;
	Parameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTRuntimeBSPWorld* Owner = World->SpawnActor<AWoTRuntimeBSPWorld>(
		AWoTRuntimeBSPWorld::StaticClass(), FTransform::Identity, Parameters);
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Owner);
	Owner->AddInstanceComponent(Component);
	Owner->AddOwnedComponent(Component);
	Component->SetupAttachment(Owner->GetRootComponent());
	Component->SetStaticMesh(CurrentMesh);
	Component->SetCollisionProfileName(TEXT("BlockAll"));
	Component->SetCollisionObjectType(ECC_WorldStatic);
	Owner->GeometryComponents.Add(Component);
	Owner->RuntimeMeshes.Add(CurrentMesh);
	Owner->UnregisterAllComponents();
	Owner->FinishSpawning(FTransform::Identity, true);
	World->InitWorld(UWorld::InitializationValues().AllowAudioPlayback(false)
		.RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false)
		.CreateFXSystem(false));
	World->UpdateWorldComponents(false, false);
	TestTrue(TEXT("guard component registers"), Component->IsRegistered());
	TestTrue(TEXT("guard component has physics state"),
		Component->IsPhysicsStateCreated());
	FHitResult Hit;
	FCollisionQueryParams Query(
		SCENE_QUERY_STAT(RuntimeBSPCollisionCookIdentity), true);
	const bool bHitCurrentPlane = Component->LineTraceComponent(
		Hit, FVector(25, 25, 600), FVector(25, 25, 400), Query);
	TestTrue(TEXT("collision comes from the current payload at Z=500"),
		bHitCurrentPlane);
	if (bHitCurrentPlane)
	{
		TestEqual(TEXT("current collision height"), Hit.ImpactPoint.Z, 500.0);
	}
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPRegistrationSpec,
	"WoT.World.RuntimeBSP.RegistersAfterInitWorld",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPRegistrationSpec::RunTest(const FString&)
{
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("Registration"));
	if (!TestNotNull(TEXT("hidden registration world exists"), World)) return false;
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = World->PersistentLevel;
	Parameters.bDeferConstruction = true;
	Parameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTRuntimeBSPWorld* Owner = World->SpawnActor<AWoTRuntimeBSPWorld>(
		AWoTRuntimeBSPWorld::StaticClass(), FTransform::Identity, Parameters);
	if (!TestNotNull(TEXT("deferred geometry actor exists"), Owner))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	FWoTBSPPayloadData Payload = WoTRuntimeMapProjectionTriangle();
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Owner);
	FString Error;
	if (!TestTrue(TEXT("registration mesh renders"),
		WoTRuntimeBSPBuildRenderMesh(*Mesh, Payload, Error))
		|| !TestTrue(TEXT("registration mesh collision cooks"),
			WoTRuntimeBSPBuildCollision(*Mesh, Payload, Error)))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
		Owner, TEXT("PreInitGeometry"), RF_Transient);
	Owner->AddInstanceComponent(Component);
	Owner->AddOwnedComponent(Component);
	Component->SetupAttachment(Owner->GetRootComponent());
	Component->SetStaticMesh(Mesh);
	Component->SetCollisionProfileName(TEXT("BlockAll"));
	Component->SetCollisionObjectType(ECC_WorldStatic);
	Owner->GeometryComponents.Add(Component);
	Owner->RuntimeMeshes.Add(Mesh);
	Owner->UnregisterAllComponents();
	Owner->FinishSpawning(FTransform::Identity, true);
	TestFalse(TEXT("component remains unregistered before InitWorld"),
		Component->IsRegistered());

	World->InitWorld(UWorld::InitializationValues()
		.AllowAudioPlayback(false)
		.RequiresHitProxies(false)
		.CreateNavigation(false)
		.CreateAISystem(false)
		.CreateFXSystem(false));
	World->UpdateWorldComponents(false, false);
	TestTrue(TEXT("UpdateWorldComponents registers runtime geometry"),
		Component->IsRegistered());
	TestTrue(TEXT("registered colliding geometry has physics state"),
		Component->IsPhysicsStateCreated());
	TestTrue(TEXT("registered colliding geometry has a valid body instance"),
		Component->BodyInstance.IsValidBodyInstance());
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorDroppedFamiliesSpec,
	"WoT.World.Projector.DroppedGameplayFamilies",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorDroppedFamiliesSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	auto AddClass = [&Classes](const TCHAR* Identity, const TCHAR* Super)
		-> FWoTRuntimeClassDefaultRecord&
	{
		auto& Record = Classes.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
		return Record;
	};
	auto AddFloat = [](FWoTRuntimeClassDefaultRecord& Class,
		const TCHAR* Name, float Value)
	{
		Class.Properties.Add(WoTRuntimeMapProjectionProperty(Name,
			EWoTRuntimeWorldPropertyType::Float, TEXT(""), Value));
	};
	auto AddInt = [](FWoTRuntimeClassDefaultRecord& Class,
		const TCHAR* Name, int32 Value)
	{
		Class.Properties.Add(WoTRuntimeMapProjectionProperty(Name,
			EWoTRuntimeWorldPropertyType::Integer, TEXT(""), Value));
	};
	auto AddBool = [](FWoTRuntimeClassDefaultRecord& Class,
		const TCHAR* Name, bool Value)
	{
		Class.Properties.Add(WoTRuntimeMapProjectionProperty(Name,
			EWoTRuntimeWorldPropertyType::Boolean, TEXT(""), uint8(Value)));
	};
	auto AddObject = [](FWoTRuntimeClassDefaultRecord& Class,
		const TCHAR* Name, const TCHAR* Identity)
	{
		FWoTRuntimeWorldProperty Property;
		Property.Name = Name;
		Property.Type = EWoTRuntimeWorldPropertyType::Object;
		Property.ObjectIdentity = Identity;
		Class.Properties.Add(MoveTemp(Property));
	};
	FWoTRuntimeClassDefaultRecord& Actor = AddClass(TEXT("Engine.Actor"), TEXT(""));
	AddFloat(Actor, TEXT("CollisionRadius"), 22.0f);
	AddFloat(Actor, TEXT("CollisionHeight"), 22.0f);
	FWoTRuntimeClassDefaultRecord& Inventory = AddClass(
		TEXT("Engine.Inventory"), TEXT("Engine.Actor"));
	AddFloat(Inventory, TEXT("RespawnTime"), 30.0f);
	AddObject(Inventory, TEXT("Mesh"), TEXT("Angreal.AngrealInventoryDart"));
	FWoTRuntimeClassDefaultRecord& Angreal = AddClass(
		TEXT("WOT.AngrealInventory"), TEXT("Engine.Inventory"));
	AddInt(Angreal, TEXT("MinInitialCharges"), 1);
	AddInt(Angreal, TEXT("MaxInitialCharges"), 1);
	AddInt(Angreal, TEXT("MaxCharges"), 999);
	AddInt(Angreal, TEXT("ChargeCost"), 1);
	AddBool(Angreal, TEXT("bMPAppearEffect"), true);
	FWoTRuntimeClassDefaultRecord& BaseDart = AddClass(
		TEXT("Angreal.AngrealInvDart"), TEXT("WOT.AngrealInventory"));
	AddObject(BaseDart, TEXT("Skin"), TEXT("Angreal.DartAesSedai"));
	FWoTRuntimeClassDefaultRecord& Forsaken = AddClass(
		TEXT("Angreal.DartForsaken"), TEXT("Angreal.AngrealInvDart"));
	AddInt(Forsaken, TEXT("MinInitialCharges"), 3);
	AddInt(Forsaken, TEXT("MaxInitialCharges"), 5);
	AddInt(Forsaken, TEXT("MaxCharges"), 17);
	AddInt(Forsaken, TEXT("ChargeCost"), 2);
	AddObject(Forsaken, TEXT("Skin"), TEXT("Angreal.RedDart"));
	FWoTRuntimeClassDefaultRecord& Legion = AddClass(
		TEXT("WOTPawns.LegionInvSeeker"), TEXT("WOT.AngrealInventory"));
	// Its retail default explicitly clears the inherited pickup display.
	AddObject(Legion, TEXT("Mesh"), TEXT(""));
	FWoTRuntimeClassDefaultRecord& Wall = AddClass(
		TEXT("WOT.WOTExplodingWall"), TEXT("Engine.Actor"));
	AddFloat(Wall, TEXT("ExplosionSize"), 321.0f);
	AddFloat(Wall, TEXT("ExplosionDimensions"), 123.0f);
	AddInt(Wall, TEXT("Health"), 44);
	AddClass(TEXT("Legend.DestroyDirective"), TEXT("Engine.Actor"));

	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("dropped-family defaults load"),
		Defaults.LoadFixture(MoveTemp(Classes), Error))) return false;
	TArray<FWoTRuntimeWorldActor> Records;
	auto AddActor = [&Records](const TCHAR* Identity, const TCHAR* Class)
		-> FWoTRuntimeWorldActor&
	{
		auto& Record = Records.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.ClassIdentity = Class;
		return Record;
	};
	AddActor(TEXT("Synthetic.Wall0"), TEXT("WOT.WOTExplodingWall"));
	AddActor(TEXT("Synthetic.Dart0"), TEXT("Angreal.DartForsaken"));
	AddActor(TEXT("Synthetic.Legion0"), TEXT("WOTPawns.LegionInvSeeker"));
	FWoTRuntimeWorldActor& Directive = AddActor(
		TEXT("Synthetic.Directive0"), TEXT("Legend.DestroyDirective"));
	FWoTRuntimeWorldProperty Observer;
	Observer.Name = TEXT("ObserverTags");
	Observer.Type = EWoTRuntimeWorldPropertyType::Name;
	Observer.NameValue = TEXT("erase_me");
	Directive.Properties.Add(MoveTemp(Observer));

	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("SyntheticDroppedFamilies");
	Source.Records = Records;
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("DroppedFamilies"));
	FWoTWorldProjectionResult Projection;
	if (!TestTrue(TEXT("all dropped gameplay families project"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Source, Defaults, Projection, Error)))
	{
		AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	AWoTExplodingWall* ProjectedWall = nullptr;
	AWoTAngrealInvDart* ProjectedDart = nullptr;
	AWoTLegionInvSeeker* ProjectedLegion = nullptr;
	AWoTDestroyDirective* ProjectedDirective = nullptr;
	for (AActor* Deferred : Projection.DeferredActors)
	{
		if (!ProjectedWall) ProjectedWall = Cast<AWoTExplodingWall>(Deferred);
		if (!ProjectedDart) ProjectedDart = Cast<AWoTAngrealInvDart>(Deferred);
		if (!ProjectedLegion) ProjectedLegion = Cast<AWoTLegionInvSeeker>(Deferred);
		if (!ProjectedDirective) ProjectedDirective = Cast<AWoTDestroyDirective>(Deferred);
	}
	if (TestNotNull(TEXT("exploding wall projects"), ProjectedWall))
	{
		TestEqual(TEXT("wall ExplosionSize"), ProjectedWall->ExplosionSize, 321.0f);
		TestEqual(TEXT("wall Health"), ProjectedWall->Health, 44);
	}
	if (TestNotNull(TEXT("DartForsaken becomes ordinary dart inventory"), ProjectedDart))
	{
		TestEqual(TEXT("forsaken minimum charges"), ProjectedDart->MinInitialCharges, 3);
		TestEqual(TEXT("forsaken maximum charges"), ProjectedDart->MaxInitialCharges, 5);
		TestEqual(TEXT("forsaken capacity"), ProjectedDart->MaxCharges, 17);
		TestEqual(TEXT("forsaken charge cost"), ProjectedDart->ChargeCost, 2);
		TestEqual(TEXT("forsaken replacement is explicitly red"),
			ProjectedDart->ConfiguredDartColor, FName(TEXT("Red")));
		TestTrue(TEXT("forsaken replacement retains base white status icon"),
			ProjectedDart->StatusIcon.ToSoftObjectPath().ToString().Contains(
				TEXT("I_Dartwh"), ESearchCase::CaseSensitive));
		TestTrue(TEXT("forsaken replacement retains base white icon frame"),
			ProjectedDart->StatusIconFrame.ToSoftObjectPath().ToString().Contains(
				TEXT("M_Dartwh"), ESearchCase::CaseSensitive));
	}
	TestNotNull(TEXT("meshless Legion seeker projects"), ProjectedLegion);
	if (TestNotNull(TEXT("destroy directive projects"), ProjectedDirective))
	{
		for (AActor* Deferred : Projection.DeferredActors)
			if (IsValid(Deferred)) Deferred->FinishSpawning(
				Deferred->GetActorTransform(), true);
		World->InitWorld(UWorld::InitializationValues().AllowAudioPlayback(false)
			.RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false)
			.CreateFXSystem(false));
		World->UpdateWorldComponents(false, false);
		FURL URL;
		World->InitializeActorsForPlay(URL);
		World->BeginPlay();
		AWoTTrigger* Victim = World->SpawnActor<AWoTTrigger>();
		Victim->TagName = TEXT("erase_me");
		ProjectedDirective->WoTTrigger(nullptr, nullptr);
		TestTrue(TEXT("directive destroys its tagged observer"),
			Victim->IsActorBeingDestroyed());
		ProjectedDirective->ObserverNativeClasses = {AWoTDecoration::StaticClass()};
		AWoTDecoration* ExactClass = World->SpawnActor<AWoTDecoration>();
		AWoTBreakableDecoration* Subclass =
			World->SpawnActor<AWoTBreakableDecoration>();
		AWoTTrigger* Unrelated = World->SpawnActor<AWoTTrigger>();
		ProjectedDirective->WoTTrigger(nullptr, nullptr);
		TestTrue(TEXT("directive class selector destroys an exact native class"),
			ExactClass->IsActorBeingDestroyed());
		TestTrue(TEXT("directive class selector destroys a native subclass"),
			Subclass->IsActorBeingDestroyed());
		TestFalse(TEXT("directive class selector preserves unrelated actors"),
			Unrelated->IsActorBeingDestroyed());
	}
	for (const FName LeafName : {FName(TEXT("WOTExplodingWall")),
		FName(TEXT("DartForsaken")), FName(TEXT("DestroyDirective")),
		FName(TEXT("LegionInvSeeker"))})
		TestEqual(TEXT("family has no unsupported records"),
			Projection.UnsupportedClassCount(LeafName), 0u);
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPSolidityCollisionSpec,
	"WoT.World.RuntimeBSP.RetailSolidityCollision",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPSolidityCollisionSpec::RunTest(const FString&)
{
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("SolidityCollision"));
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = World->PersistentLevel;
	Parameters.bDeferConstruction = true;
	Parameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTRuntimeBSPWorld* Owner = World->SpawnActor<AWoTRuntimeBSPWorld>(
		AWoTRuntimeBSPWorld::StaticClass(), FTransform::Identity, Parameters);
	FWoTBSPPayloadData Payload = WoTRuntimeMapProjectionTriangle();
	Payload.Positions.Append({FVector3f(200,0,0), FVector3f(300,0,0),
		FVector3f(200,100,0), FVector3f(400,0,0), FVector3f(500,0,0),
		FVector3f(400,100,0)});
	for (int32 Index = 0; Index < 6; ++Index)
	{
		Payload.Normals.Add(FVector3f::ZAxisVector);
		Payload.UV0.Add(FVector2f::ZeroVector);
		Payload.UV1.Add(FVector2f::ZeroVector);
	}
	Payload.Indices = {0,2,1,3,5,4,6,8,7};
	Payload.Sections[0].IndexCount = 9;
	Payload.Sections[0].MaxVertex = 8;
	// All three share one visual section: only per-triangle provenance can
	// distinguish their retail solidity.
	Payload.TriangleSurfaces = {0,1,2};
	const TArray<uint32> SurfaceFlags = {0x00000008u, 0x00000004u, 0x00000020u};
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Owner);
	FString Error;
	TestTrue(TEXT("mixed-solidity render mesh builds"),
		WoTRuntimeBSPBuildRenderMesh(*Mesh, Payload, Error));
	TestTrue(TEXT("mixed-solidity collision cooks"),
		WoTRuntimeBSPBuildCollision(*Mesh, Payload, Error, SurfaceFlags));
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Owner);
	Owner->AddInstanceComponent(Component);
	Owner->AddOwnedComponent(Component);
	Component->SetupAttachment(Owner->GetRootComponent());
	Component->SetStaticMesh(Mesh);
	Component->SetCollisionProfileName(TEXT("BlockAll"));
	Component->SetCollisionObjectType(ECC_WorldStatic);
	Owner->GeometryComponents.Add(Component);
	Owner->RuntimeMeshes.Add(Mesh);
	// A direct-builder legacy fixture without per-triangle provenance retains
	// conservative collision. On-disk v7/v8 payloads are rejected by the strict
	// decoder and production v9 always supplies the provenance array.
	FWoTBSPPayloadData ProductionShape = Payload;
	ProductionShape.TriangleSurfaces.Reset();
	UStaticMesh* ProductionMesh = NewObject<UStaticMesh>(Owner);
	TestTrue(TEXT("legacy direct-builder BSP collision cooks without provenance"),
		WoTRuntimeBSPBuildRenderMesh(*ProductionMesh, ProductionShape, Error)
			&& WoTRuntimeBSPBuildCollision(
				*ProductionMesh, ProductionShape, Error, SurfaceFlags));
	UStaticMeshComponent* ProductionComponent = NewObject<UStaticMeshComponent>(Owner);
	Owner->AddInstanceComponent(ProductionComponent);
	Owner->AddOwnedComponent(ProductionComponent);
	ProductionComponent->SetupAttachment(Owner->GetRootComponent());
	ProductionComponent->SetRelativeLocation(FVector(0, 200, 0));
	ProductionComponent->SetStaticMesh(ProductionMesh);
	ProductionComponent->SetCollisionProfileName(TEXT("BlockAll"));
	ProductionComponent->SetCollisionObjectType(ECC_WorldStatic);
	Owner->GeometryComponents.Add(ProductionComponent);
	Owner->RuntimeMeshes.Add(ProductionMesh);
	Owner->UnregisterAllComponents();
	Owner->FinishSpawning(FTransform::Identity, true);
	World->InitWorld(UWorld::InitializationValues().AllowAudioPlayback(false)
		.RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false)
		.CreateFXSystem(false));
	World->UpdateWorldComponents(false, false);
	TestTrue(TEXT("solidity component registers"), Component->IsRegistered());
	TestTrue(TEXT("solidity component has physics state"),
		Component->IsPhysicsStateCreated());
	FURL URL;
	World->InitializeActorsForPlay(URL);
	World->BeginPlay();
	auto Trace = [Component](float X)
	{
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(RuntimeBSPSolidity), true);
		return Component->LineTraceComponent(
			Hit, FVector(X,25,100), FVector(X,25,-100), Query);
	};
	TestFalse(TEXT("PF_NotSolid triangle does not block"), Trace(25));
	TestTrue(TEXT("solid PF_Translucent triangle blocks"), Trace(225));
	TestTrue(TEXT("PF_Semisolid triangle blocks"), Trace(425));
	auto Sweep = [World](float X)
	{
		FHitResult Hit;
		return World->SweepSingleByChannel(Hit, FVector(X,25,100),
			FVector(X,25,-100), FQuat::Identity, ECC_Pawn,
			FCollisionShape::MakeBox(FVector(5.0f)));
	};
	TestFalse(TEXT("PF_NotSolid triangle permits extent movement"), Sweep(25));
	TestTrue(TEXT("solid PF_Translucent triangle blocks extent movement"),
		Sweep(225));
	TestTrue(TEXT("PF_Semisolid triangle blocks extent movement"), Sweep(425));
	auto ProductionTrace = [ProductionComponent](float X)
	{
		FHitResult Hit;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(RuntimeBSPProductionSolidity), true);
		return ProductionComponent->LineTraceComponent(
			Hit, FVector(X,225,100), FVector(X,225,-100), Query);
	};
	TestTrue(TEXT("legacy direct-builder former PF_NotSolid triangle remains collidable"),
		ProductionTrace(25));
	TestTrue(TEXT("legacy direct-builder translucent triangle remains collidable"),
		ProductionTrace(225));
	TestTrue(TEXT("legacy direct-builder semisolid triangle remains collidable"),
		ProductionTrace(425));
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldProjectorPlayerStartSpec,
	"WoT.World.Projector.PlayerStart",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldProjectorPlayerStartSpec::RunTest(const FString&)
{
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("Projector"));
	if (!TestNotNull(TEXT("projector world exists"), World)) return false;
	FWoTRuntimeWorldActor First;
	First.Identity = TEXT("Arena.PlayerStart3");
	First.ClassIdentity = TEXT("eNgInE.pLaYeRsTaRt");
	const float LocationValues[3] = {100.0f, -200.0f, 300.0f};
	const int32 RotationValues[3] = {16384, 32768, -8192};
	First.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Location"), EWoTRuntimeWorldPropertyType::Struct,
		TEXT("Vector"), LocationValues));
	First.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("Rotation"), EWoTRuntimeWorldPropertyType::Struct,
		TEXT("Rotator"), RotationValues));
	First.Properties.Add(WoTRuntimeMapProjectionProperty(
		TEXT("TeamNumber"), EWoTRuntimeWorldPropertyType::Byte,
		TEXT(""), static_cast<uint8>(7)));
	FWoTRuntimeWorldActor Second;
	Second.Identity = TEXT("Arena.PlayerStart4");
	Second.ClassIdentity = TEXT("Engine.PlayerStart");
	TArray<FWoTRuntimeWorldActor> Records = {First, Second};
	FWoTWorldProjectionResult Projection;
	FString Error;
	if (!TestTrue(TEXT("PlayerStart rules project"),
		FWoTWorldProjector::ProjectDeferred(
			*World, Records, Projection, Error)))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	TestEqual(TEXT("both records project"), Projection.PlayerStartCount, 2u);
	TestFalse(TEXT("authored starts suppress fallback"),
		Projection.bUsedPlayerStartFallback);
	if (Projection.DeferredActors.Num() == 2)
	{
		AWoTPlayerStart* Start = Cast<AWoTPlayerStart>(Projection.DeferredActors[0]);
		AWoTPlayerStart* DefaultTeam =
			Cast<AWoTPlayerStart>(Projection.DeferredActors[1]);
		if (TestNotNull(TEXT("case-insensitive rule produces WoT start"), Start))
		{
			TestEqual(TEXT("deterministic projected name"), Start->GetName(),
				FString(TEXT("WoTProjected_0_PlayerStart")));
			TestEqual(TEXT("projection preserves the exact authored location"),
				Start->GetActorLocation(), FVector(100, -200, 300));
			const FRotator Converted(90.0, 180.0, -45.0);
			TestTrue(TEXT("UE1 pitch/yaw/roll conversion reaches the actor transform"),
				Start->GetActorQuat().Equals(Converted.Quaternion(), 0.0001));
			TestEqual(TEXT("TeamNumber applies"), Start->TeamNumber, uint8(7));
		}
		if (TestNotNull(TEXT("absent-team start exists"), DefaultTeam))
		{
			TestEqual(TEXT("absent TeamNumber defaults to zero"),
				DefaultTeam->TeamNumber, uint8(0));
		}
	}
	WoTRuntimeMapProjectionDestroyWorld(World);

	UWorld* FallbackWorld = WoTRuntimeMapProjectionWorld(TEXT("Fallback"));
	FWoTWorldProjectionResult FallbackProjection;
	TestTrue(TEXT("empty record set projects fallback"),
		FWoTWorldProjector::ProjectDeferred(
			*FallbackWorld, TConstArrayView<FWoTRuntimeWorldActor>(),
			FallbackProjection, Error));
	TestTrue(TEXT("fallback is reported"),
		FallbackProjection.bUsedPlayerStartFallback);
	TestEqual(TEXT("fallback counts as one start"),
		FallbackProjection.PlayerStartCount, 1u);
	if (FallbackProjection.DeferredActors.Num() == 1)
	{
		APlayerStart* Fallback =
			Cast<APlayerStart>(FallbackProjection.DeferredActors[0]);
		if (TestNotNull(TEXT("fallback is an engine PlayerStart"), Fallback))
		{
			TestEqual(TEXT("fallback deterministic name"), Fallback->GetName(),
				FString(TEXT("WoTProjected_Fallback_PlayerStart")));
			TestEqual(TEXT("fallback importer location"),
				Fallback->GetActorLocation(), FVector(0, 0, 200));
		}
	}
	WoTRuntimeMapProjectionDestroyWorld(FallbackWorld);
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// Sky-zone child scene and AutoU/VPan on authenticated map worlds.
// Synthetic rule specs first, then the real-retail census on all 19 maps.
namespace
{
struct FWoTSkyTestVector3
{
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
};

struct FWoTSkyTestRotator
{
	int32 Pitch = 0;
	int32 Yaw = 0;
	int32 Roll = 0;
};

FWoTRuntimeWorldProperty WoTSkyTestName(const TCHAR* Name, const TCHAR* Value)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Name;
	Property.NameValue = Value;
	return Property;
}

/** Minimal world-v3 record for the retail sky rules. */
struct FWoTSkyTestWorld
{
	TArray<FWoTRuntimeWorldActor> Actors;
	TArray<FWoTRuntimeWorldZone> Zones;
	TArray<FWoTRuntimeWorldNode> Nodes;
	TArray<FWoTRuntimeWorldSurface> Surfaces;

	FWoTRuntimeWorldActor& AddActor(const TCHAR* Identity, const TCHAR* ClassIdentity)
	{
		FWoTRuntimeWorldActor& Actor = Actors.AddDefaulted_GetRef();
		Actor.Identity = Identity;
		Actor.ClassIdentity = ClassIdentity;
		return Actor;
	}
	void AddZone(const TCHAR* ActorIdentity)
	{
		Zones.AddDefaulted_GetRef().ActorIdentity = ActorIdentity;
	}
	int32 AddSurface(uint32 Flags, const TCHAR* TextureIdentity)
	{
		FWoTRuntimeWorldSurface& NewSurface = Surfaces.AddDefaulted_GetRef();
		NewSurface.Flags = Flags;
		NewSurface.TextureIdentity = TextureIdentity;
		return Surfaces.Num() - 1;
	}
	void AddNode(int32 SurfaceIndex, uint8 ZoneA, uint8 ZoneB, uint8 Vertices = 4)
	{
		FWoTRuntimeWorldNode& NewNode = Nodes.AddDefaulted_GetRef();
		NewNode.Surface = SurfaceIndex;
		NewNode.Zones[0] = ZoneA;
		NewNode.Zones[1] = ZoneB;
		NewNode.VertexCount = Vertices;
	}
	FWoTRetailSkyWorldView View() const
	{
		FWoTRetailSkyWorldView Result;
		Result.Actors = Actors;
		Result.Zones = Zones;
		Result.Nodes = Nodes;
		Result.Surfaces = Surfaces;
		return Result;
	}
};

/** Append one BSP-v4 section holding exactly one triangle. */
void WoTSkyTestAddSection(FWoTBSPPayloadData& Part, const TCHAR* Slot,
	const TCHAR* Package, const TCHAR* Name, const TCHAR* Source, uint32 Flags,
	const FVector3f& A, const FVector3f& B, const FVector3f& C,
	const TCHAR* PanZone = TEXT(""))
{
	FWoTBSPPayloadSection& Section = Part.Sections.AddDefaulted_GetRef();
	Section.MaterialSlot = Slot;
	Section.CanonicalPackage = Package;
	Section.CanonicalName = Name;
	Section.SourceTextureIdentity = Source;
	Section.SurfaceFlags = Flags;
	Section.PanZoneActorIdentity = PanZone;
	Section.bCollisionEnabled = !Part.bPortal;
	Section.FirstIndex = static_cast<uint32>(Part.Indices.Num());
	Section.IndexCount = 3;
	const uint32 Base = static_cast<uint32>(Part.Positions.Num());
	Section.MinVertex = Base;
	Section.MaxVertex = Base + 2;
	for (const FVector3f& Corner : {A, B, C})
	{
		Part.Positions.Add(Corner);
		Part.Normals.Add(FVector3f::ZAxisVector);
		Part.UV0.Add(FVector2f(Corner.X / 256.0f, Corner.Y / 256.0f));
		Part.UV1.Add(FVector2f(0.5f, 0.5f));
	}
	Part.Indices.Append({Base, Base + 1, Base + 2});
	Part.BoundsMin = Part.Positions[0];
	Part.BoundsMax = Part.Positions[0];
	for (const FVector3f& Position : Part.Positions)
	{
		Part.BoundsMin = FVector3f(FMath::Min(Part.BoundsMin.X, Position.X),
			FMath::Min(Part.BoundsMin.Y, Position.Y),
			FMath::Min(Part.BoundsMin.Z, Position.Z));
		Part.BoundsMax = FVector3f(FMath::Max(Part.BoundsMax.X, Position.X),
			FMath::Max(Part.BoundsMax.Y, Position.Y),
			FMath::Max(Part.BoundsMax.Z, Position.Z));
	}
}

constexpr uint32 WoTSkyTestSheetFlags = WoTRuntimeMapPFPortal
	| WoTRuntimeMapPFTranslucent | WoTRuntimeMapPFTwoSided | WoTRuntimeMapPFAutoVPan;

/**
 * One SkyZoneInfo at (1000,2000,3000) yawed -16200 units; zone 1 is the world,
 * zone 2 the sky. Slots: a world wall, a sky-only dome (AutoUPan, pan zone at
 * TexUPanSpeed 0.5), a texture shared by world and sky (the plain world copy
 * plus its __SkyZone split), a FakeBackdrop window, a NULL-texture world face,
 * and a PF_Portal translucent sky sheet in the portal part (AutoVPan).
 */
void WoTSkyTestScene(FWoTSkyTestWorld& World, TArray<FWoTBSPPayloadData>& Parts)
{
	FWoTRuntimeWorldActor& Sky = World.AddActor(
		TEXT("Map.SkyZoneInfo0"), TEXT("Engine.SkyZoneInfo"));
	FWoTSkyTestVector3 SkyLocation;
	SkyLocation.X = 1000.0f;
	SkyLocation.Y = 2000.0f;
	SkyLocation.Z = 3000.0f;
	Sky.Properties.Add(WoTRuntimeMapProjectionProperty(TEXT("Location"),
		EWoTRuntimeWorldPropertyType::Struct, TEXT("Vector"), SkyLocation));
	FWoTSkyTestRotator SkyRotation;
	SkyRotation.Yaw = -16200;
	Sky.Properties.Add(WoTRuntimeMapProjectionProperty(TEXT("Rotation"),
		EWoTRuntimeWorldPropertyType::Struct, TEXT("Rotator"), SkyRotation));
	Sky.Properties.Add(WoTSkyTestName(TEXT("Tag"), TEXT("SkyZoneInfo")));
	FWoTRuntimeWorldActor& WorldZone = World.AddActor(
		TEXT("Map.WOTZoneInfo0"), TEXT("WOT.WOTZoneInfo"));
	WorldZone.Properties.Add(WoTRuntimeMapProjectionProperty(TEXT("TexUPanSpeed"),
		EWoTRuntimeWorldPropertyType::Float, TEXT(""), 0.5f));
	World.AddZone(TEXT(""));
	World.AddZone(TEXT("Map.WOTZoneInfo0"));
	World.AddZone(TEXT("Map.SkyZoneInfo0"));
	World.AddNode(World.AddSurface(0, TEXT("Pkg.Group.Wall")), 1, 1);
	World.AddNode(World.AddSurface(WoTRuntimeMapPFAutoUPan, TEXT("Pkg.Group.Dome")), 2, 0);
	World.AddNode(World.AddSurface(0, TEXT("Pkg.Group.Shared")), 1, 1);
	World.AddNode(World.AddSurface(0, TEXT("Pkg.Group.Shared")), 2, 2);
	World.AddNode(World.AddSurface(WoTRuntimeMapPFFakeBackdrop, TEXT("Pkg.Group.Window")), 1, 0);
	World.AddNode(World.AddSurface(WoTSkyTestSheetFlags, TEXT("Pkg.Group.Sheet")), 2, 0);
	World.AddNode(World.AddSurface(WoTRuntimeMapPFInvisible, TEXT("Pkg.Group.Hidden")), 2, 2);
	World.AddNode(World.AddSurface(0, TEXT("")), 1, 1);
	// A degenerate node never votes or classifies (VertexCount < 3).
	World.AddNode(World.AddSurface(0, TEXT("Pkg.Group.Sliver")), 2, 2, 2);

	Parts.Reset();
	FWoTBSPPayloadData& Main = Parts.AddDefaulted_GetRef();
	Main.FormatVersion = 4;
	Main.bPortal = false;
	WoTSkyTestAddSection(Main, TEXT("Pkg__Wall"), TEXT("Pkg"), TEXT("Wall"),
		TEXT("Pkg.Group.Wall"), 0, FVector3f(-5000, -5000, -5000),
		FVector3f(5000, 5000, 5000), FVector3f(5000, -5000, -5000));
	WoTSkyTestAddSection(Main, TEXT("Pkg__Dome"), TEXT("Pkg"), TEXT("Dome"),
		TEXT("Pkg.Group.Dome"), WoTRuntimeMapPFAutoUPan, FVector3f(0, 1000, 3100),
		FVector3f(2000, 1000, 3100), FVector3f(1000, 3500, 3100),
		TEXT("Map.WOTZoneInfo0"));
	WoTSkyTestAddSection(Main, TEXT("Pkg__Shared"), TEXT("Pkg"), TEXT("Shared"),
		TEXT("Pkg.Group.Shared"), 0, FVector3f(0, 0, 0), FVector3f(100, 0, 0),
		FVector3f(0, 100, 0));
	WoTSkyTestAddSection(Main, TEXT("Pkg__Shared__SkyZone"), TEXT("Pkg"),
		TEXT("Shared"), TEXT("Pkg.Group.Shared"), 0, FVector3f(1000, 2000, 3500),
		FVector3f(1100, 2000, 3500), FVector3f(1000, 2100, 3500));
	WoTSkyTestAddSection(Main, TEXT("Pkg__Window_Sky"), TEXT("Pkg"),
		TEXT("Window"), TEXT("Pkg.Group.Window"), WoTRuntimeMapPFFakeBackdrop,
		FVector3f(0, 0, 500), FVector3f(100, 0, 500), FVector3f(0, 100, 500));
	WoTSkyTestAddSection(Main, TEXT("__None"), TEXT(""), TEXT("None"),
		TEXT("None"), 0, FVector3f(0, 0, 900), FVector3f(100, 0, 900),
		FVector3f(0, 100, 900));
	FWoTBSPPayloadData& Portal = Parts.AddDefaulted_GetRef();
	Portal.FormatVersion = 4;
	Portal.bPortal = true;
	WoTSkyTestAddSection(Portal, TEXT("Pkg__Sheet_T_2S"), TEXT("Pkg"),
		TEXT("Sheet"), TEXT("Pkg.Group.Sheet"), WoTSkyTestSheetFlags,
		FVector3f(900, 1900, 3300), FVector3f(1100, 1900, 3300),
		FVector3f(1000, 2100, 3300), TEXT("Map.SkyZoneInfo0"));
}

class FWoTSkyCensusWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("sky census writer overflow")));
		}
		if (Schema == TEXT("wot-world-v3") || Schema == TEXT("wot-bsp-v3"))
		{
			TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
			Copy.Append(Bytes.GetData(), Bytes.Num());
		}
		return FWoTBootstrapStatus::Success();
	}
	TMap<FString, TArray<uint8>> Objects;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPInvisibleSolidRealRetailSpec,
	"WoT.World.RuntimeBSP.InvisibleSolidRealRetail",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPInvisibleSolidRealRetailSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: invisible-solid BSP not verified"));
		return true;
	}
	struct FInvisibleSolidExpected
	{
		const TCHAR* Map;
		uint32 SurfaceCount;
		uint32 PortalWithoutNotSolidCount;
	};
	static constexpr FInvisibleSolidExpected Expected[] = {
		{TEXT("Arena_01"),50,1}, {TEXT("Arena_02"),24,0},
		{TEXT("Arena_03"),0,0}, {TEXT("Arena_04"),0,0},
		{TEXT("Arena_05"),34,0}, {TEXT("Arena_06"),0,0},
		{TEXT("Arena_07"),0,0}, {TEXT("Arena_08"),0,0},
		{TEXT("Arena_09"),0,0}, {TEXT("Arena_11"),18,0},
		{TEXT("Arena_12"),65,0}, {TEXT("Battle2_01"),60,0},
		{TEXT("Battle2_02"),33,16}, {TEXT("Battle2_03"),19,0},
		{TEXT("Battle2_04"),26,0}, {TEXT("Battle2_05"),86,2},
		{TEXT("Battle3_01"),40,0}, {TEXT("Battle3_02"),22,0},
		{TEXT("Battle4_01"),112,0},
	};
	const FString RetailRoot = WoTRuntimeProjectionRepoPath(TEXT("original"));
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
	{
		AddError(LoadedContract.GetError().ToString());
		return false;
	}
	FWoTRetailContract Contract;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::World
			&& Fact.Domain != EWoTFactDomain::BSP)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"),
				ESearchCase::IgnoreCase))
		{
			continue;
		}
		Contract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(Fact.SourcePath + TEXT(" production package loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath))))
		{
			return false;
		}
	}
	FWoTSkyCensusWriter Writer;
	FWoTRuntimeProjectionCensusProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportRequiredWorldSetPackageBytesForTesting(Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("production BSP/world payloads import"), Imported.IsError()))
	{
		AddError(Imported.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("all multiplayer maps import"), Imported.GetValue().Worlds.Num(), 19);
	uint32 PortalWithoutNotSolidTotal = 0;
	uint32 CheckedMaps = 0;
	for (const FWoTWorldDescriptor& Descriptor : Imported.GetValue().Worlds)
	{
		const FInvisibleSolidExpected* ExpectedMap = nullptr;
		for (const FInvisibleSolidExpected& Candidate : Expected)
		{
			if (Descriptor.MapIdentity == Candidate.Map)
			{
				ExpectedMap = &Candidate;
				break;
			}
		}
		if (!TestNotNull(Descriptor.MapIdentity + TEXT(" has an invisible-solid pin"),
			ExpectedMap)) continue;
		TArray<uint8>* WorldBytes = Writer.Objects.Find(Descriptor.WorldLogicalId);
		if (!TestNotNull(Descriptor.MapIdentity + TEXT(" world payload"), WorldBytes))
			continue;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldBytes),
				TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" world frame decodes"),
			DecodedWorld.IsError())) continue;
		FWoTRuntimeWorldResource Resource;
		FWoTBootstrapStatus Loaded =
			Resource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue());
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" world payload decodes"),
			Loaded.IsError())) continue;

		FWoTRuntimeBSPCollisionGeometryView Geometry;
		Geometry.Vectors = Resource.Vectors();
		Geometry.Points = Resource.Points();
		Geometry.Nodes = Resource.Nodes();
		Geometry.Surfaces = Resource.Surfaces();
		Geometry.Vertices = Resource.Vertices();
		FWoTRuntimeInvisibleBSPCollisionData CollisionData;
		FString Error;
		if (!TestTrue(Descriptor.MapIdentity + TEXT(" collision-only payload builds"),
			WoTWorldBuildInvisibleSolidBSPCollision(
				Geometry, CollisionData, Error)))
		{
			AddError(Descriptor.MapIdentity + TEXT(": ") + Error);
			continue;
		}
		TSet<uint32> Authority;
		for (const FWoTRuntimeWorldNode& Node : Resource.Nodes())
		{
			if (Node.VertexCount < 3
				|| !Resource.Surfaces().IsValidIndex(Node.Surface)) continue;
			const uint32 Flags = Resource.Surfaces()[Node.Surface].Flags;
			if ((Flags & WoTRuntimeMapPFInvisible) != 0
				&& (Flags & WoTRuntimeMapPFNotSolid) == 0
				&& (Flags & WoTRuntimeMapPFPortal) == 0)
			{
				Authority.Add(static_cast<uint32>(Node.Surface));
			}
		}
		TArray<uint32> ExpectedSurfaces = Authority.Array();
		ExpectedSurfaces.Sort();
		TestEqual(Descriptor.MapIdentity + TEXT(" collision-only surface count"),
			CollisionData.SurfaceIndices.Num(),
			static_cast<int32>(ExpectedMap->SurfaceCount));
		TestTrue(Descriptor.MapIdentity + TEXT(" collision-only set equals decoded authority"),
			CollisionData.SurfaceIndices == ExpectedSurfaces);
		TestEqual(Descriptor.MapIdentity + TEXT(" portal-without-NotSolid count"),
			CollisionData.PortalWithoutNotSolidSurfaceCount,
			ExpectedMap->PortalWithoutNotSolidCount);
		PortalWithoutNotSolidTotal +=
			CollisionData.PortalWithoutNotSolidSurfaceCount;
		UE_LOG(LogTemp, Display,
			TEXT("INVISIBLE_SOLID_LEVEL map=%s surfaces=%d triangles=%d portal_without_notsolid=%u"),
			*Descriptor.MapIdentity, CollisionData.SurfaceIndices.Num(),
			CollisionData.Payload.Indices.Num() / 3,
			CollisionData.PortalWithoutNotSolidSurfaceCount);
		++CheckedMaps;
		if (Descriptor.MapIdentity != TEXT("Arena_01")) continue;

		TArray<FWoTBSPPayloadData> Parts;
		bool bPartsDecoded = true;
		for (const FWoTBSPPartDescriptor& PartDescriptor : Descriptor.BSPParts)
		{
			TArray<uint8>* PartBytes = Writer.Objects.Find(PartDescriptor.LogicalId);
			if (!TestNotNull(PartDescriptor.PartIdentity + TEXT(" BSP payload"), PartBytes))
			{
				bPartsDecoded = false;
				break;
			}
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedPart =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*PartBytes),
					TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
			FString PartError;
			FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
			bPartsDecoded = !DecodedPart.IsError()
				&& UWoTBSPPayloadLibrary::DecodePayload(
					DecodedPart.GetValue().Payload(), Part, PartError);
			if (!bPartsDecoded)
			{
				AddError(Descriptor.MapIdentity + TEXT(" BSP decode: ") + PartError);
				break;
			}
		}
		if (!bPartsDecoded) continue;
		uint32 InvisibleRenderedTriangles = 0;
		uint32 GrateRenderedTriangles = 0;
		for (const FWoTBSPPayloadData& Part : Parts)
		{
			for (uint32 Surface : Part.TriangleSurfaces)
			{
				InvisibleRenderedTriangles += Surface == 2200u || Surface == 2201u ? 1u : 0u;
				GrateRenderedTriangles += Surface == 1328u || Surface == 1329u ? 1u : 0u;
			}
		}
		TestEqual(TEXT("Arena_01 invisible floor contributes no rendered triangles"),
			InvisibleRenderedTriangles, 0u);
		TestTrue(TEXT("Arena_01 visible grate remains in the render payload"),
			GrateRenderedTriangles > 0);
		TArray<uint32> SurfaceFlags;
		for (const FWoTRuntimeWorldSurface& Surface : Resource.Surfaces())
			SurfaceFlags.Add(Surface.Flags);
		WoTTest::FSpecWorldOptions WorldOptions;
		WorldOptions.bStartPlay = false;
		UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
		AWoTRuntimeBSPWorld* RuntimeBSP = nullptr;
		uint32 FallbackParts = 0;
		if (!TestTrue(TEXT("Arena_01 runtime BSP with collision-only geometry materializes"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, RuntimeBSP,
				FallbackParts, Error, {}, nullptr, nullptr, nullptr, nullptr,
				EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags, &CollisionData)))
		{
			AddError(Error);
			WoTTest::DestroySpecWorld(World);
			continue;
		}
		if (TestNotNull(TEXT("Arena_01 runtime BSP actor"), RuntimeBSP))
		{
			RuntimeBSP->FinishSpawning(FTransform::Identity, true);
			World->UpdateWorldComponents(false, false);
			UStaticMeshComponent* CollisionComponent =
				RuntimeBSP->InvisibleSolidCollisionComponent;
			if (TestNotNull(TEXT("Arena_01 collision-only component"), CollisionComponent))
			{
				TestFalse(TEXT("collision-only component is not visible"),
					CollisionComponent->IsVisible());
				TestTrue(TEXT("collision-only component is hidden in game"),
					CollisionComponent->bHiddenInGame);
				// The visible BSP parts are the reference: setting the object
				// type after the BlockAll profile reads back as "Custom", and
				// mobility follows the attachment tree, for both alike.
				const UStaticMeshComponent* PartComponent = nullptr;
				for (const TObjectPtr<UStaticMeshComponent>& Part :
					RuntimeBSP->GeometryComponents)
				{
					if (Part && Part->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
					{
						PartComponent = Part.Get();
						break;
					}
				}
				if (TestNotNull(TEXT("Arena_01 visible BSP part component"), PartComponent))
				{
					TestEqual(TEXT("collision-only component is world-static like the BSP parts"),
						CollisionComponent->GetCollisionObjectType(),
						PartComponent->GetCollisionObjectType());
					TestEqual(TEXT("collision-only component's mobility matches the BSP parts"),
						CollisionComponent->Mobility, PartComponent->Mobility);
					TestEqual(TEXT("collision-only component is query-and-physics like the BSP parts"),
						CollisionComponent->GetCollisionEnabled(),
						PartComponent->GetCollisionEnabled());
					for (const ECollisionChannel Channel : {ECC_Pawn, ECC_Visibility,
						ECC_Camera, ECC_WorldDynamic, ECC_PhysicsBody})
					{
						TestEqual(*FString::Printf(
							TEXT("collision-only component's response on channel %d matches the BSP parts"),
							static_cast<int32>(Channel)),
							CollisionComponent->GetCollisionResponseToChannel(Channel),
							PartComponent->GetCollisionResponseToChannel(Channel));
					}
				}
				TestEqual(TEXT("collision-only component is world-static"),
					CollisionComponent->GetCollisionObjectType(), ECC_WorldStatic);
				TestEqual(TEXT("collision-only component blocks pawns"),
					CollisionComponent->GetCollisionResponseToChannel(ECC_Pawn), ECR_Block);
				TestTrue(TEXT("collision-only component has no render materials"),
					CollisionComponent->GetNumMaterials() == 0);
			}
			auto DecodedCentroid = [&Resource](uint32 FirstSurface,
				uint32 SecondSurface, FVector& OutCentroid) -> bool
			{
				FVector Sum = FVector::ZeroVector;
				int32 Count = 0;
				for (const FWoTRuntimeWorldNode& Node : Resource.Nodes())
				{
					if ((static_cast<uint32>(Node.Surface) != FirstSurface
						&& static_cast<uint32>(Node.Surface) != SecondSurface)
						|| Node.VertexCount < 3) continue;
					for (int32 Corner = 0; Corner < Node.VertexCount; ++Corner)
					{
						const int32 VertexIndex = Node.VertexPool + Corner;
						if (!Resource.Vertices().IsValidIndex(VertexIndex)) return false;
						const int32 PointIndex = Resource.Vertices()[VertexIndex].Point;
						if (!Resource.Points().IsValidIndex(PointIndex)) return false;
						Sum += FVector(Resource.Points()[PointIndex]);
						++Count;
					}
				}
				if (Count == 0) return false;
				OutCentroid = Sum / Count;
				return true;
			};
			FVector FloorCentroid;
			if (TestTrue(TEXT("Arena_01 decoded invisible floor centroid exists"),
				DecodedCentroid(2200u, 2201u, FloorCentroid)))
			{
				FCollisionQueryParams Query(
					SCENE_QUERY_STAT(RuntimeBSPInvisibleSolidFloor), true);
				Query.bReturnFaceIndex = true;
				FHitResult Hit;
				const bool bHit = World->LineTraceSingleByChannel(Hit,
					FloorCentroid + FVector(0, 0, 128),
					FloorCentroid - FVector(0, 0, 256), ECC_Pawn, Query);
				TestTrue(TEXT("Arena_01 Pawn trace hits invisible solid floor"),
					bHit && Hit.GetComponent() == CollisionComponent);
				if (bHit)
				{
					TestEqual(TEXT("Arena_01 invisible floor hit retains retail height"),
						Hit.ImpactPoint.Z, -634.0, 2.0);
					uint32 HitSurface = MAX_uint32;
					FString HitTexture;
					TestTrue(TEXT("Arena_01 collision-only hit resolves retail provenance"),
						RuntimeBSP->ResolveInvisibleSolidCollisionFace(
							Hit.GetComponent(), Hit.FaceIndex, HitSurface, HitTexture));
					TestTrue(TEXT("Arena_01 collision-only hit resolves one floor side"),
						HitSurface == 2200u || HitSurface == 2201u);
				}
			}
			// One convex node of the visible grate: its vertex average lies on
			// that node's own plane (1328 and 1329 are separate planes, so
			// averaging both would miss the grate entirely). The invisible
			// sheet sits 6 uu above, outside this +-4 uu window.
			auto NodeCentroid = [&Resource](uint32 Surface, FVector& OutCentroid) -> bool
			{
				for (const FWoTRuntimeWorldNode& Node : Resource.Nodes())
				{
					if (static_cast<uint32>(Node.Surface) != Surface
						|| Node.VertexCount < 3) continue;
					FVector Sum = FVector::ZeroVector;
					for (int32 Corner = 0; Corner < Node.VertexCount; ++Corner)
					{
						const int32 VertexIndex = Node.VertexPool + Corner;
						if (!Resource.Vertices().IsValidIndex(VertexIndex)) return false;
						const int32 PointIndex = Resource.Vertices()[VertexIndex].Point;
						if (!Resource.Points().IsValidIndex(PointIndex)) return false;
						Sum += FVector(Resource.Points()[PointIndex]);
					}
					OutCentroid = Sum / Node.VertexCount;
					return true;
				}
				return false;
			};
			for (const uint32 GrateSurface : {1328u, 1329u})
			{
				FVector GrateCentroid;
				if (!TestTrue(*FString::Printf(
					TEXT("Arena_01 decoded visible grate surf %u node exists"), GrateSurface),
					NodeCentroid(GrateSurface, GrateCentroid))) continue;
				FHitResult GrateHit;
				FCollisionQueryParams GrateQuery(
					SCENE_QUERY_STAT(RuntimeBSPNotSolidGrate), true);
				const bool bGrateHit = World->LineTraceSingleByChannel(GrateHit,
					GrateCentroid + FVector(0, 0, 4),
					GrateCentroid - FVector(0, 0, 4), ECC_Pawn, GrateQuery);
				TestFalse(*FString::Printf(
					TEXT("Arena_01 NotSolid visible grate surf %u remains non-colliding (z %.1f)"),
					GrateSurface, GrateCentroid.Z), bGrateHit);
			}
		}
		WoTTest::DestroySpecWorld(World);
	}
	TestEqual(TEXT("nineteen multiplayer maps checked"), CheckedMaps, 19u);
	TestEqual(TEXT("nineteen portal surfaces lack PF_NotSolid and stay excluded"),
		PortalWithoutNotSolidTotal, 19u);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapSkySelectionSpec,
	"WoT.World.Sky.SelectionRules",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapSkySelectionSpec::RunTest(const FString&)
{
	FString Error;
	{
		// Arena_03/09/12: no SkyZoneInfo is a sky-less map, not an error.
		FWoTSkyTestWorld Empty;
		Empty.AddActor(TEXT("Map.WOTZoneInfo0"), TEXT("WOT.WOTZoneInfo"));
		Empty.AddZone(TEXT(""));
		Empty.AddZone(TEXT("Map.WOTZoneInfo0"));
		Empty.AddNode(Empty.AddSurface(WoTRuntimeMapPFFakeBackdrop, TEXT("P.W")), 1, 0);
		FWoTRetailSkySelection Selection;
		TestTrue(TEXT("sky-less world selects without error"),
			WoTSelectRetailSky(Empty.View(), Selection, Error));
		TestTrue(TEXT("sky-less world has no SkyZoneInfo"), Selection.SkyActors.IsEmpty());
		TestFalse(TEXT("sky-less world has no sky zone"), Selection.HasSkyZone());
		FWoTRuntimeMapSkyPlan Plan;
		TArray<FWoTBSPPayloadData> NoParts;
		TestTrue(TEXT("sky-less world plans without error"),
			WoTBuildRuntimeMapSkyPlan(Empty.View(), NoParts, Plan, Error));
		TestFalse(TEXT("sky-less world has no sky"), Plan.bHasSky);
	}
	// Three skies in actor order: A (Tag SkyA), B (bHighDetail, Tag SkyB), C.
	FWoTSkyTestWorld World;
	World.AddActor(TEXT("Map.SkyA"), TEXT("Engine.SkyZoneInfo"))
		.Properties.Add(WoTSkyTestName(TEXT("Tag"), TEXT("SkyA")));
	FWoTRuntimeWorldActor& SkyB = World.AddActor(
		TEXT("Map.SkyB"), TEXT("Engine.SkyZoneInfo"));
	SkyB.Properties.Add(WoTRuntimeMapProjectionProperty(TEXT("bHighDetail"),
		EWoTRuntimeWorldPropertyType::Boolean, TEXT(""), uint8(1)));
	SkyB.Properties.Add(WoTSkyTestName(TEXT("Tag"), TEXT("SkyB")));
	World.AddActor(TEXT("Map.SkyC"), TEXT("Engine.SkyZoneInfo"))
		.Properties.Add(WoTSkyTestName(TEXT("Tag"), TEXT("SkyC")));
	World.AddActor(TEXT("Map.EventZone"), TEXT("WOT.WOTZoneInfo"))
		.Properties.Add(WoTSkyTestName(TEXT("Event"), TEXT("skya")));
	World.AddActor(TEXT("Map.PlainWotZone"), TEXT("WOT.WaterZone"));
	World.AddActor(TEXT("Map.EngineZone"), TEXT("Engine.ZoneInfo"));
	World.AddZone(TEXT(""));                 // 0: null zone
	World.AddZone(TEXT("Map.EventZone"));    // 1: Event SkyA (case-insensitive)
	World.AddZone(TEXT("Map.PlainWotZone")); // 2: empty Event -> last (C)
	World.AddZone(TEXT("Map.EngineZone"));   // 3: engine rule -> bHighDetail (B)
	World.AddZone(TEXT("Map.SkyA"));         // 4
	World.AddZone(TEXT("Map.SkyB"));         // 5
	World.AddZone(TEXT("Map.SkyC"));         // 6
	const int32 Window = World.AddSurface(WoTRuntimeMapPFFakeBackdrop, TEXT("P.W"));
	for (int32 Vote = 0; Vote < 3; ++Vote) World.AddNode(Window, 1, 0);
	for (int32 Vote = 0; Vote < 2; ++Vote) World.AddNode(Window, 2, 0);
	// Both sides in one zone count once; a null side never counts.
	for (int32 Vote = 0; Vote < 2; ++Vote) World.AddNode(Window, 3, 3);
	World.AddNode(Window, 0, 0);
	World.AddNode(Window, 1, 0, 2);
	World.AddNode(World.AddSurface(0, TEXT("P.Floor")), 4, 4);
	FWoTRetailSkySelection Selection;
	if (!TestTrue(TEXT("three-sky world selects"),
		WoTSelectRetailSky(World.View(), Selection, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("three SkyZoneInfo records in actor order"),
		Selection.SkyActors.Num(), 3);
	TestTrue(TEXT("per-sky FakeBackdrop tallies are skybake's"),
		Selection.Tallies == TArray<int32>({3, 2, 2}));
	TestTrue(TEXT("each BSP zone retains its retail LinkToSkybox result"),
		Selection.ZoneSkyLinks == TArray<int32>({1, 0, 2, 1, 1, 1, 1}));
	TestEqual(TEXT("majority winner is SkyA"), Selection.WinnerActor, 0);
	TestEqual(TEXT("winner zone is the first zone naming it"), Selection.Zone, 4);
	TestEqual(TEXT("winner appears in exactly one zone"), Selection.ZoneMatchCount, 1);
	TestTrue(TEXT("winner sky surfaces are the nodes touching its zone"),
		Selection.Surfaces.Num() == 1 && Selection.Surfaces.Contains(1));
	TestFalse(TEXT("no portal leaves the sky zone"), Selection.bTraversablePortal);
	// A tie goes to the earliest actor: one more zone-3 vote makes B = 3.
	World.AddNode(Window, 3, 0);
	TestTrue(TEXT("tied world selects"), WoTSelectRetailSky(World.View(), Selection, Error));
	TestTrue(TEXT("tied tallies"), Selection.Tallies == TArray<int32>({3, 3, 2}));
	TestEqual(TEXT("ties keep the earliest SkyZoneInfo"), Selection.WinnerActor, 0);
	// No FakeBackdrop votes at all: the last SkyZoneInfo.
	FWoTSkyTestWorld Silent = World;
	for (FWoTRuntimeWorldSurface& Surface : Silent.Surfaces) Surface.Flags = 0;
	TestTrue(TEXT("vote-less world selects"),
		WoTSelectRetailSky(Silent.View(), Selection, Error));
	TestEqual(TEXT("vote-less winner is the last SkyZoneInfo"), Selection.WinnerActor, 2);
	TestEqual(TEXT("vote-less winner zone"), Selection.Zone, 6);
	// A portal from the sky zone into a nonzero foreign zone (sky-rest maps).
	FWoTSkyTestWorld Leaky = World;
	Leaky.AddNode(Leaky.AddSurface(WoTRuntimeMapPFPortal | WoTRuntimeMapPFTranslucent,
		TEXT("P.Sheet")), 4, 1);
	TestTrue(TEXT("leaky world selects"), WoTSelectRetailSky(Leaky.View(), Selection, Error));
	TestTrue(TEXT("a traversable sky portal is reported"), Selection.bTraversablePortal);
	// The winner absent from the zone table: no sky zone, still no error.
	FWoTSkyTestWorld Orphan = World;
	Orphan.Zones[4].ActorIdentity = TEXT("Map.Elsewhere");
	TestTrue(TEXT("orphan winner selects"), WoTSelectRetailSky(Orphan.View(), Selection, Error));
	TestFalse(TEXT("orphan winner has no sky zone"), Selection.HasSkyZone());
	// Malformed consumed fields reject (entry parity).
	FWoTSkyTestWorld Malformed = World;
	Malformed.Actors[1].Properties[0].Value = {uint8(2)};
	TestFalse(TEXT("a malformed bHighDetail rejects"),
		WoTSelectRetailSky(Malformed.View(), Selection, Error));
	TestEqual(TEXT("malformed bHighDetail keeps the entry error text"), Error,
		FString(TEXT("SkyZoneInfo bHighDetail is malformed")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapMinoritySkyPlansSpec,
	"WoT.World.Sky.MinorityZonePlans",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapMinoritySkyPlansSpec::RunTest(const FString&)
{
	FWoTSkyTestWorld World;
	FWoTRuntimeWorldActor& SkyA = World.AddActor(
		TEXT("Map.SkyA"), TEXT("Engine.SkyZoneInfo"));
	SkyA.Properties.Add(WoTSkyTestName(TEXT("Tag"), TEXT("A")));
	FWoTRuntimeWorldActor& SkyB = World.AddActor(
		TEXT("Map.SkyB"), TEXT("Engine.SkyZoneInfo"));
	SkyB.Properties.Add(WoTSkyTestName(TEXT("Tag"), TEXT("B")));
	FWoTSkyTestVector3 SkyBLocation;
	SkyBLocation.X = 1000.0f;
	SkyB.Properties.Add(WoTRuntimeMapProjectionProperty(TEXT("Location"),
		EWoTRuntimeWorldPropertyType::Struct, TEXT("Vector"), SkyBLocation));
	World.AddActor(TEXT("Map.ZoneA"), TEXT("WOT.WOTZoneInfo"))
		.Properties.Add(WoTSkyTestName(TEXT("Event"), TEXT("A")));
	World.AddActor(TEXT("Map.ZoneB"), TEXT("WOT.WOTZoneInfo"))
		.Properties.Add(WoTSkyTestName(TEXT("Event"), TEXT("B")));
	World.AddZone(TEXT(""));
	World.AddZone(TEXT("Map.ZoneA"));
	World.AddZone(TEXT("Map.ZoneB"));
	World.AddZone(TEXT("Map.SkyA"));
	World.AddZone(TEXT("Map.SkyB"));
	const int32 Window = World.AddSurface(
		WoTRuntimeMapPFFakeBackdrop, TEXT("Pkg.Window"));
	World.AddNode(Window, 1, 0);
	World.AddNode(Window, 2, 0);
	const int32 SurfaceA = World.AddSurface(0, TEXT("Pkg.Dome"));
	const int32 SurfaceB = World.AddSurface(0, TEXT("Pkg.Dome"));
	World.AddNode(SurfaceA, 3, 3);
	World.AddNode(SurfaceB, 4, 4);

	TArray<FWoTBSPPayloadData> Parts;
	FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
	Part.FormatVersion = 10;
	WoTSkyTestAddSection(Part, TEXT("Pkg__Dome"), TEXT("Pkg"), TEXT("Dome"),
		TEXT("Pkg.Dome"), 0, FVector3f(-100, -100, 100),
		FVector3f(100, -100, 100), FVector3f(0, 100, 100));
	// Deliberately share one material section between the two sky zones. The
	// v10 per-triangle provenance must partition it without a slot heuristic.
	const uint32 Base = static_cast<uint32>(Part.Positions.Num());
	for (const FVector3f& Corner : {FVector3f(900, -100, 100),
		FVector3f(1100, -100, 100), FVector3f(1000, 100, 100)})
	{
		Part.Positions.Add(Corner);
		Part.Normals.Add(FVector3f::ZAxisVector);
		Part.UV0.Add(FVector2f::ZeroVector);
		Part.UV1.Add(FVector2f::ZeroVector);
	}
	Part.Indices.Append({Base, Base + 1, Base + 2});
	Part.Sections[0].IndexCount = 6;
	Part.Sections[0].MaxVertex = Base + 2;
	Part.TriangleSurfaces = {
		static_cast<uint32>(SurfaceA), static_cast<uint32>(SurfaceB)};

	TArray<FWoTRuntimeMapSkyPlan> Plans;
	TArray<int32> ZonePlans;
	FString Error;
	if (!TestTrue(TEXT("both linked sky child scenes plan"),
		WoTBuildRuntimeMapSkyPlans(World.View(), Parts, Plans, ZonePlans, Error)))
	{
		AddError(Error);
		return false;
	}
	if (TestEqual(TEXT("one plan per linked SkyZoneInfo"), Plans.Num(), 2))
	{
		TestEqual(TEXT("majority/tie winner remains plan zero"),
			Plans[0].SkyActorIdentity, FString(TEXT("Map.SkyA")));
		TestEqual(TEXT("minority child scene is retained"),
			Plans[1].SkyActorIdentity, FString(TEXT("Map.SkyB")));
		TestEqual(TEXT("majority plan owns one exact triangle"),
			Plans[0].SkyTriangleCount, 1u);
		TestEqual(TEXT("minority plan owns one exact triangle"),
			Plans[1].SkyTriangleCount, 1u);
		TestEqual(TEXT("shared source section remains a partial world section"),
			Plans[0].SkySections[0][0], false);
	}
	TestTrue(TEXT("world zones choose their linked child scenes"),
		ZonePlans.Num() == 5 && ZonePlans[1] == 0 && ZonePlans[2] == 1);

	TArray<FWoTRuntimeWorldNode> ZoneTree;
	FWoTRuntimeWorldNode& Root = ZoneTree.AddDefaulted_GetRef();
	Root.Plane[0] = 1.0f;
	Root.Zones[0] = 1;
	Root.Zones[1] = 2;
	TestEqual(TEXT("back leaf point resolves zone A"),
		WoTRuntimeMapPointZone(ZoneTree, FVector(-1, 0, 0)), 1);
	TestEqual(TEXT("front leaf point resolves zone B"),
		WoTRuntimeMapPointZone(ZoneTree, FVector(1, 0, 0)), 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapSkyZoneRangeSpec,
	"WoT.World.Sky.RejectsOutOfRangeSkyZone",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapSkyZoneRangeSpec::RunTest(const FString&)
{
	// A node addresses zones as uint8: a winner whose first zone is 256 has no
	// usable sky zone. The pre-sharing entry SelectSky rejected it explicitly
	// ("winning SkyZoneInfo has no nonzero BSP zone"); keep that, and keep the
	// map path from ever truncating 256 to zone 0.
	FWoTSkyTestWorld World;
	World.AddActor(TEXT("Map.SkyZoneInfo0"), TEXT("Engine.SkyZoneInfo"));
	World.AddActor(TEXT("Map.ZoneInfo0"), TEXT("Engine.ZoneInfo"));
	World.AddZone(TEXT(""));
	for (int32 ZoneIndex = 1; ZoneIndex < 256; ++ZoneIndex)
		World.AddZone(TEXT("Map.ZoneInfo0"));
	World.AddZone(TEXT("Map.SkyZoneInfo0"));
	World.AddNode(World.AddSurface(WoTRuntimeMapPFFakeBackdrop, TEXT("P.W")), 1, 0);
	World.AddNode(World.AddSurface(0, TEXT("P.Floor")), 0, 0);
	FWoTRetailSkySelection Selection;
	FString Error;
	if (!TestTrue(TEXT("zone-256 world selects without a data error"),
		WoTSelectRetailSky(World.View(), Selection, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("the winner is the only SkyZoneInfo"), Selection.WinnerActor, 0);
	TestEqual(TEXT("the first zone naming the winner is 256"), Selection.Zone, 256);
	TestEqual(TEXT("zone 256 is the only match"), Selection.ZoneMatchCount, 1);
	TestFalse(TEXT("zone 256 is not a usable sky zone"), Selection.HasSkyZone());
	TestTrue(TEXT("no sky surfaces are collected past uint8"),
		Selection.Surfaces.IsEmpty());
	TestEqual(TEXT("entry policy rejects zone 256 exactly as before"),
		WoTEntrySkySelectionPolicyError(Selection),
		FString(TEXT("winning SkyZoneInfo has no nonzero BSP zone")));
	FWoTRetailSkySelection Edge = Selection;
	Edge.Zone = MAX_uint8;
	TestTrue(TEXT("zone 255 is still usable"), Edge.HasSkyZone());
	FWoTRuntimeMapSkyPlan Plan;
	TArray<FWoTBSPPayloadData> NoParts;
	TestTrue(TEXT("zone-256 map plans without error"),
		WoTBuildRuntimeMapSkyPlan(World.View(), NoParts, Plan, Error));
	TestFalse(TEXT("zone-256 map has no runtime sky"), Plan.bHasSky);

	// The rest of the entry policy, in the original order.
	FWoTRetailSkySelection Policy;
	TestEqual(TEXT("entry: no SkyZoneInfo"), WoTEntrySkySelectionPolicyError(Policy),
		FString(TEXT("front-end world has no SkyZoneInfo")));
	Policy.SkyActors = {0};
	Policy.WinnerSky = 0;
	Policy.WinnerActor = 0;
	Policy.Zone = 0;
	Policy.ZoneMatchCount = 2;
	Policy.bTraversablePortal = true;
	TestEqual(TEXT("entry: several zones outrank every later check"),
		WoTEntrySkySelectionPolicyError(Policy),
		FString(TEXT("winning SkyZoneInfo appears in multiple zones")));
	Policy.ZoneMatchCount = 1;
	TestEqual(TEXT("entry: zone 0 outranks a traversable portal"),
		WoTEntrySkySelectionPolicyError(Policy),
		FString(TEXT("winning SkyZoneInfo has no nonzero BSP zone")));
	Policy.ZoneMatchCount = 0;
	Policy.Zone = INDEX_NONE;
	TestEqual(TEXT("entry: an unnamed winner has no zone"),
		WoTEntrySkySelectionPolicyError(Policy),
		FString(TEXT("winning SkyZoneInfo has no nonzero BSP zone")));
	Policy.ZoneMatchCount = 1;
	Policy.Zone = 3;
	TestEqual(TEXT("entry: traversable portal"), WoTEntrySkySelectionPolicyError(Policy),
		FString(TEXT("winning sky zone has a traversable nonzero-zone portal")));
	Policy.bTraversablePortal = false;
	TestTrue(TEXT("entry: a single sealed nonzero zone is accepted"),
		WoTEntrySkySelectionPolicyError(Policy).IsEmpty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapSkyPartitionSpec,
	"WoT.World.Sky.PartitionPlan",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapSkyPartitionSpec::RunTest(const FString&)
{
	FWoTSkyTestWorld World;
	TArray<FWoTBSPPayloadData> Parts;
	WoTSkyTestScene(World, Parts);
	FWoTRuntimeMapSkyPlan Plan;
	FString Error;
	if (!TestTrue(TEXT("synthetic sky plan builds"),
		WoTBuildRuntimeMapSkyPlan(World.View(), Parts, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("synthetic world has a sky"), Plan.bHasSky);
	TestEqual(TEXT("sky actor identity"), Plan.SkyActorIdentity,
		FString(TEXT("Map.SkyZoneInfo0")));
	TestEqual(TEXT("sky zone"), Plan.Selection.Zone, 2);
	TestTrue(TEXT("sky slots are skygeo's sorted sky-only + __SkyZone set"),
		Plan.SkySlots == TArray<FString>({TEXT("Pkg__Dome"),
			TEXT("Pkg__Shared__SkyZone"), TEXT("Pkg__Sheet_T_2S")}));
	TestTrue(TEXT("main part hides exactly the dome and the __SkyZone split"),
		Plan.SkySections.Num() == 2 && Plan.SkySections[0]
			== TArray<bool>({false, true, false, true, false, false}));
	TestTrue(TEXT("portal sky sheet is a sky section"),
		Plan.SkySections.Num() == 2 && Plan.SkySections[1] == TArray<bool>({true}));
	TestEqual(TEXT("no FakeBackdrop sky slot"), Plan.FakeBackdropSkySlots, 0u);
	if (TestEqual(TEXT("one material source per sky slot"),
		Plan.SkyMaterialSources.Num(), 3))
	{
		TestTrue(TEXT("dome reuses main section 1"),
			Plan.SkyMaterialSources[0].Part == 0 && Plan.SkyMaterialSources[0].Section == 1);
		TestTrue(TEXT("__SkyZone reuses main section 3"),
			Plan.SkyMaterialSources[1].Part == 0 && Plan.SkyMaterialSources[1].Section == 3);
		TestTrue(TEXT("sheet reuses portal section 0"),
			Plan.SkyMaterialSources[2].Part == 1 && Plan.SkyMaterialSources[2].Section == 0);
	}
	TestEqual(TEXT("sky origin"), Plan.Origin, FVector(1000, 2000, 3000));
	TestTrue(TEXT("sky rotation is the SkyZoneInfo yaw"),
		FMath::IsNearlyEqual(Plan.Rotation.Yaw, -16200.0 * 360.0 / 65536.0, 1e-6)
		&& Plan.Rotation.Pitch == 0.0 && Plan.Rotation.Roll == 0.0);
	TestTrue(TEXT("face distance is the dome plane, not a vertex"),
		FMath::IsNearlyEqual(Plan.MinSurfaceDistance, 100.0, 1e-3));
	TestTrue(TEXT("world radius is the main part bounds sphere"),
		FMath::IsNearlyEqual(Plan.WorldRadius, FMath::Sqrt(3.0) * 5000.0, 0.05));
	TestEqual(TEXT("SkyScale = max(12, ceil(2r/d*1.25))"), Plan.Scale, 217.0f);
	TestEqual(TEXT("sky triangles"), Plan.SkyTriangleCount, 3u);
	const FWoTBSPPayloadData& Sky = Plan.SkyMesh;
	TestTrue(TEXT("sky mesh is noncolliding"), Sky.bPortal);
	if (TestEqual(TEXT("sky mesh sections"), Sky.Sections.Num(), 3)
		&& TestEqual(TEXT("sky mesh vertices"), Sky.Positions.Num(), 9))
	{
		TestEqual(TEXT("sky mesh keeps slot names"), Sky.Sections[2].MaterialSlot,
			FString(TEXT("Pkg__Sheet_T_2S")));
		TestFalse(TEXT("sky sections never collide"), Sky.Sections[0].bCollisionEnabled);
		TestTrue(TEXT("sky vertices are re-centred on the sky origin"),
			Sky.Positions[0].Equals(FVector3f(-1000, -1000, 100), 1e-3f));
		TestTrue(TEXT("sky section ranges are dense"),
			Sky.Sections[1].FirstIndex == 3 && Sky.Sections[1].MinVertex == 3
			&& Sky.Sections[1].MaxVertex == 5);
	}
	UStaticMesh* SkyMesh = NewObject<UStaticMesh>(GetTransientPackage());
	TestTrue(TEXT("sky payload builds a render mesh"),
		SkyMesh != nullptr && WoTRuntimeBSPBuildRenderMesh(*SkyMesh, Sky, Error));

	// Legacy (pre-v4) parts carry no identity: the world keeps its old sky.
	TArray<FWoTBSPPayloadData> LegacyParts = Parts;
	LegacyParts[1].FormatVersion = 3;
	TestTrue(TEXT("legacy parts plan without error"),
		WoTBuildRuntimeMapSkyPlan(World.View(), LegacyParts, Plan, Error));
	TestFalse(TEXT("legacy parts have no runtime sky"), Plan.bHasSky);
	// A __SkyZone slot the world record does not split is a contradiction.
	TArray<FWoTBSPPayloadData> Contradicted = Parts;
	Contradicted[0].Sections[0].MaterialSlot = TEXT("Pkg__Wall__SkyZone");
	TestFalse(TEXT("an unsplit __SkyZone section rejects the plan"),
		WoTBuildRuntimeMapSkyPlan(World.View(), Contradicted, Plan, Error));
	TestFalse(TEXT("a rejected plan carries no sky"), Plan.bHasSky);
	return !HasAnyErrors();
}

// The main part's version-6 sky-rest census adds skygeo's rest
// groups to the sky mesh: "SkyRest__<slot>" bound to the WORLD slot's section,
// "SkyRestBlock" drawn black, "SkyRestFB" hidden but sized into SkyScale.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapSkyRestPlanSpec,
	"WoT.World.Sky.RestPlan",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapSkyRestPlanSpec::RunTest(const FString&)
{
	FWoTSkyTestWorld World;
	TArray<FWoTBSPPayloadData> Parts;
	WoTSkyTestScene(World, Parts);
	auto AddGroup = [](FWoTBSPSkyRest& Rest, EWoTBSPSkyRestRole Role,
		const TCHAR* Slot, float Z)
	{
		FWoTBSPSkyRestGroup& Group = Rest.Groups.AddDefaulted_GetRef();
		Group.Role = Role;
		Group.Slot = Slot;
		const FVector3f Corners[] = {FVector3f(1000, 2000, Z),
			FVector3f(1100, 2000, Z), FVector3f(1000, 2100, Z)};
		for (const FVector3f& Corner : Corners)
		{
			Group.Positions.Add(Corner);
			Group.Normals.Add(FVector3f(0, 0, -1));
			Group.UV0.Add(FVector2f(0.5f, 0.5f));
			Group.UV1.Add(FVector2f(0.25f, 0.25f));
		}
	};
	FWoTBSPSkyRest& Rest = Parts[0].SkyRest;
	Rest.bPresent = true;
	Rest.SkyZone = 2;
	Rest.RayCount = 100;
	Rest.CoveredRays = 40;
	FWoTBSPSkyRestSurface& Row = Rest.Surfaces.AddDefaulted_GetRef();
	Row.Surface = 0;
	Row.VisibleRays = 60;
	AddGroup(Rest, EWoTBSPSkyRestRole::Blocker, TEXT(""), 3200.0f);
	AddGroup(Rest, EWoTBSPSkyRestRole::FakeBackdrop, TEXT(""), 3050.0f);
	AddGroup(Rest, EWoTBSPSkyRestRole::Visible, TEXT("Pkg__Wall"), 3300.0f);
	AddGroup(Rest, EWoTBSPSkyRestRole::Visible, TEXT("Pkg__Missing"), 3300.0f);
	FWoTRuntimeMapSkyPlan Plan;
	FString Error;
	if (!TestTrue(TEXT("a sky plan with rest content builds"),
		WoTBuildRuntimeMapSkyPlan(World.View(), Parts, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("the census was adopted"), Plan.bSkyRestCensus);
	TestEqual(TEXT("census surfaces"), Plan.SkyRestSurfaceCount, 1u);
	TestEqual(TEXT("the sky slots are unchanged"), Plan.SkySlots.Num(), 3);
	TestTrue(TEXT("rest sections follow the sky slots"),
		Plan.SkyRestSlots == TArray<FString>({TEXT("SkyRestBlock"),
			TEXT("SkyRest__Pkg__Wall")}));
	TestEqual(TEXT("a rest slot without a world section is left out"),
		Plan.SkyRestMissingSlots, 1u);
	TestEqual(TEXT("rest triangles drawn"), Plan.SkyRestTriangleCount, 2u);
	if (TestEqual(TEXT("five sky-mesh sections"), Plan.SkyMesh.Sections.Num(), 5)
		&& TestEqual(TEXT("section kinds"), Plan.SkySectionKinds.Num(), 5)
		&& TestEqual(TEXT("material sources"), Plan.SkyMaterialSources.Num(), 5))
	{
		TestTrue(TEXT("blocker kind"), Plan.SkySectionKinds[3]
			== EWoTRuntimeMapSkySectionKind::RestBlocker);
		TestEqual(TEXT("a blocker binds no world section"),
			Plan.SkyMaterialSources[3].Part, static_cast<int32>(INDEX_NONE));
		TestTrue(TEXT("visible rest kind"), Plan.SkySectionKinds[4]
			== EWoTRuntimeMapSkySectionKind::RestVisible);
		TestTrue(TEXT("visible rest reuses the world wall section"),
			Plan.SkyMaterialSources[4].Part == 0 && Plan.SkyMaterialSources[4].Section == 0);
		TestEqual(TEXT("rest section name"), Plan.SkyMesh.Sections[4].MaterialSlot,
			FString(TEXT("SkyRest__Pkg__Wall")));
		TestTrue(TEXT("rest vertices are re-centred on the sky origin"),
			Plan.SkyMesh.Positions[Plan.SkyMesh.Sections[4].MinVertex].Equals(
				FVector3f(0, 0, 300), 1e-3f));
	}
	TestTrue(TEXT("the hidden window sizes SkyScale"),
		FMath::IsNearlyEqual(Plan.MinSurfaceDistance, 50.0, 1e-3));
	TestTrue(TEXT("the world keeps every rest surface (only sky sections move)"),
		Plan.SkySections[0] == TArray<bool>({false, true, false, true, false, false}));
	UStaticMesh* SkyMesh = NewObject<UStaticMesh>(GetTransientPackage());
	TestTrue(TEXT("the sky payload with rest content builds a render mesh"),
		SkyMesh != nullptr && WoTRuntimeBSPBuildRenderMesh(*SkyMesh, Plan.SkyMesh, Error));

	TArray<FWoTBSPPayloadData> WrongZone = Parts;
	WrongZone[0].SkyRest.SkyZone = 1;
	TestFalse(TEXT("a census naming another sky zone rejects the plan"),
		WoTBuildRuntimeMapSkyPlan(World.View(), WrongZone, Plan, Error));
	TArray<FWoTBSPPayloadData> PortalCensus = Parts;
	PortalCensus[1].SkyRest = PortalCensus[0].SkyRest;
	TestFalse(TEXT("a census on a portal part rejects the plan"),
		WoTBuildRuntimeMapSkyPlan(World.View(), PortalCensus, Plan, Error));
	TArray<FWoTBSPPayloadData> Legacy = Parts;
	Legacy[0].SkyRest = FWoTBSPSkyRest();
	TestTrue(TEXT("a legacy cache plans without rest content"),
		WoTBuildRuntimeMapSkyPlan(World.View(), Legacy, Plan, Error)
		&& !Plan.bSkyRestCensus && Plan.SkyRestSlots.IsEmpty());
	return !HasAnyErrors();
}

// Per-channel sky translucency (single-TMU Glide FB = M*(Tex + Dst*(1-Tex)),
// in the original Glide driver) may use the SceneColor master only where
// no other translucent sky layer lies behind the sheet: Arena_08's moon01 /
// Corona1 discs (~500uu) sit IN FRONT of the Sky_440 cloud sheet (~1016uu),
// so the sheet qualifies and the discs, which have the sheet behind them,
// keep the live-framebuffer max3 master.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapSkyLayerOrderSpec,
	"WoT.World.Sky.LayerOrder",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapSkyLayerOrderSpec::RunTest(const FString&)
{
	FWoTBSPPayloadData Mesh;
	// Sky meshes are noncolliding portal-role payloads (WoTRuntimeMapSky.cpp).
	Mesh.bPortal = true;
	// One triangle per section facing the origin along +X (or -X) at Depth.
	auto AddSheet = [&Mesh](float Depth, float Direction)
	{
		FWoTBSPPayloadSection& Section = Mesh.Sections.AddDefaulted_GetRef();
		Section.MaterialSlot = FString::Printf(TEXT("Sheet%d"), Mesh.Sections.Num() - 1);
		Section.FirstIndex = static_cast<uint32>(Mesh.Indices.Num());
		Section.IndexCount = 3;
		const uint32 Base = static_cast<uint32>(Mesh.Positions.Num());
		Mesh.Positions.Add(FVector3f(Direction * Depth, -0.4f * Depth, -0.4f * Depth));
		Mesh.Positions.Add(FVector3f(Direction * Depth, 0.4f * Depth, -0.4f * Depth));
		Mesh.Positions.Add(FVector3f(Direction * Depth, 0.0f, 0.4f * Depth));
		Mesh.Normals.Append({FVector3f::XAxisVector, FVector3f::XAxisVector,
			FVector3f::XAxisVector});
		Mesh.UV0.Append({FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector});
		Mesh.UV1.Append({FVector2f::ZeroVector, FVector2f::ZeroVector, FVector2f::ZeroVector});
		Mesh.Indices.Append({Base, Base + 1, Base + 2});
		Section.MinVertex = Base;
		Section.MaxVertex = Base + 2;
	};
	AddSheet(500.0f, 1.0f);    // 0: near disc, translucent
	AddSheet(1016.0f, 1.0f);   // 1: cloud sheet behind it, translucent
	AddSheet(1536.0f, 1.0f);   // 2: opaque dome behind both
	AddSheet(800.0f, -1.0f);   // 3: modulated sheet on the other side
	AddSheet(1016.0f, -1.0f);  // 4: equal distance to 1
	Mesh.BoundsMin = Mesh.BoundsMax = Mesh.Positions[0];
	for (const FVector3f& Position : Mesh.Positions)
	{
		Mesh.BoundsMin = Mesh.BoundsMin.ComponentMin(Position);
		Mesh.BoundsMax = Mesh.BoundsMax.ComponentMax(Position);
	}
	auto Joined = [](const TArray<int32>& Values)
	{
		return FString::JoinBy(Values, TEXT(","),
			[](const int32 Value) { return FString::FromInt(Value); });
	};
	TestEqual(TEXT("layers draw far to near; equal distances keep section order"),
		Joined(WoTRuntimeMapSkyLayerOrder(Mesh, {true, true, false, true, true})),
		FString(TEXT("1,4,3,0")));
	TestEqual(TEXT("no flagged section, no layer"),
		WoTRuntimeMapSkyLayerOrder(Mesh, {false, false, false, false, false}).Num(), 0);

	// Arena_02's shape: a flat pyramid enclosing the origin (roof ~6uu above,
	// base ~1.8uu below) is nearer than a cloud sheet 125uu overhead, so the
	// pyramid draws last over the sheet, as retail's back-to-front order does.
	{
		FWoTBSPPayloadData Sky;
		auto AddTriangle = [&Sky](const FVector3f& P0, const FVector3f& P1,
			const FVector3f& P2)
		{
			FWoTBSPPayloadSection& Section = Sky.Sections.AddDefaulted_GetRef();
			Section.FirstIndex = static_cast<uint32>(Sky.Indices.Num());
			Section.IndexCount = 3;
			const uint32 Base = static_cast<uint32>(Sky.Positions.Num());
			Sky.Positions.Append({P0, P1, P2});
			Sky.Indices.Append({Base, Base + 1, Base + 2});
		};
		AddTriangle(FVector3f(-1024.0f, -1024.0f, 125.0f),
			FVector3f(1024.0f, -1024.0f, 125.0f), FVector3f(0.0f, 1024.0f, 125.0f));
		AddTriangle(FVector3f(-128.0f, -128.0f, -1.8f),
			FVector3f(128.0f, -128.0f, -1.8f), FVector3f(0.0f, 0.0f, 6.2f));
		TestEqual(TEXT("an enclosing near layer draws after the far sheet"),
			Joined(WoTRuntimeMapSkyLayerOrder(Sky, {true, true})), FString(TEXT("0,1")));
	}

	// The sub-mesh keeps the vertex streams and compacts the chosen sections.
	FWoTBSPPayloadData Sub;
	if (TestTrue(TEXT("sub-mesh of sections 3 and 1"),
		WoTRuntimeMapSkySubMesh(Mesh, {3, 1}, Sub)))
	{
		TestEqual(TEXT("vertex streams stay whole"), Sub.Positions.Num(), Mesh.Positions.Num());
		if (TestEqual(TEXT("two sections, in the requested order"), Sub.Sections.Num(), 2))
		{
			TestEqual(TEXT("first section is source 3"), Sub.Sections[0].MaterialSlot,
				FString(TEXT("Sheet3")));
			TestEqual(TEXT("its indices start at zero"), Sub.Sections[0].FirstIndex, 0u);
			TestEqual(TEXT("second section is source 1"), Sub.Sections[1].MaterialSlot,
				FString(TEXT("Sheet1")));
			TestEqual(TEXT("its indices follow"), Sub.Sections[1].FirstIndex, 3u);
		}
		TestTrue(TEXT("indices are copied from the source ranges"),
			Sub.Indices == TArray<uint32>({9, 10, 11, 3, 4, 5}));
		UStaticMesh* Built = NewObject<UStaticMesh>(GetTransientPackage());
		FString Error;
		TestTrue(TEXT("the sub-mesh builds a render mesh"), Built != nullptr
			&& WoTRuntimeBSPBuildRenderMesh(*Built, Sub, Error));
	}
	TestFalse(TEXT("an out-of-range section is refused"),
		WoTRuntimeMapSkySubMesh(Mesh, {7}, Sub));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapPanRuleSpec,
	"WoT.World.Sky.PanRule",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapPanRuleSpec::RunTest(const FString&)
{
	auto SameRate = [](const FVector2f& Actual, float ExpectedU, float ExpectedV)
	{
		return FMath::IsNearlyEqual(Actual.X, ExpectedU, 1e-7f)
			&& FMath::IsNearlyEqual(Actual.Y, ExpectedV, 1e-7f);
	};
	// import_map.py:1124-1131 in UV/sec.
	TestTrue(TEXT("AutoUPan only"), SameRate(
		WoTRuntimeMapPanRate(WoTRuntimeMapPFAutoUPan, FVector2f(0.1f, 0.2f), 256, 256),
		static_cast<float>(35.0 * static_cast<double>(0.1f) / 256.0), 0.0f));
	TestTrue(TEXT("AutoVPan only"), SameRate(
		WoTRuntimeMapPanRate(WoTRuntimeMapPFAutoVPan, FVector2f(0.1f, 0.2f), 256, 128),
		0.0f, static_cast<float>(35.0 * static_cast<double>(0.2f) / 128.0)));
	TestTrue(TEXT("dangling substitution pans over 32 texels"), SameRate(
		WoTRuntimeMapPanRate(WoTRuntimeMapPFAutoUPan | WoTRuntimeMapPFAutoVPan,
			FVector2f(0.1f, 0.1f), WoTRuntimeMapDanglingPanTextureSize,
			WoTRuntimeMapDanglingPanTextureSize),
		0.109375f, 0.109375f));
	TestTrue(TEXT("no pan flags, no pan"), SameRate(
		WoTRuntimeMapPanRate(WoTRuntimeMapPFTranslucent, FVector2f(1, 1), 256, 256),
		0.0f, 0.0f));
	TestEqual(TEXT("a zero texture size is treated as one texel"),
		WoTRuntimeMapPanRate(WoTRuntimeMapPFAutoUPan, FVector2f(1, 1), 0, 0).X, 35.0f);

	FWoTSkyTestWorld World;
	TArray<FWoTBSPPayloadData> Parts;
	WoTSkyTestScene(World, Parts);
	FWoTRuntimeWorldActor& Bad = World.AddActor(TEXT("Map.BadZone"), TEXT("WOT.WOTZoneInfo"));
	Bad.Properties.Add(WoTRuntimeMapProjectionProperty(TEXT("TexVPanSpeed"),
		EWoTRuntimeWorldPropertyType::Byte, TEXT(""), uint8(3)));
	const FWoTRuntimeMapPanSpeeds Speeds = WoTRuntimeMapBuildPanSpeeds(World.Actors);
	TestTrue(TEXT("authored TexUPanSpeed, class-default V"),
		SameRate(Speeds.Find(TEXT("map.wotzoneinfo0")), 0.5f, 1.0f));
	TestTrue(TEXT("a zone without pan properties is the ZoneInfo default 1.0"),
		SameRate(Speeds.Find(TEXT("Map.SkyZoneInfo0")), 1.0f, 1.0f));
	TestTrue(TEXT("an empty pan zone is 1.0"), SameRate(Speeds.Find(TEXT("")), 1.0f, 1.0f));
	TestEqual(TEXT("a malformed speed is counted and defaulted"), Speeds.MalformedZones, 1u);
	TestTrue(TEXT("a malformed speed keeps the default"),
		SameRate(Speeds.Find(TEXT("Map.BadZone")), 1.0f, 1.0f));

	// mats.update(portal_materials): the portal last writer wins a shared slot.
	FWoTBSPPayloadData& Portal = Parts[1];
	WoTSkyTestAddSection(Portal, TEXT("Pkg__Dome"), TEXT("Pkg"), TEXT("Dome"),
		TEXT("Pkg.Group.Dome"), WoTRuntimeMapPFPortal | WoTRuntimeMapPFAutoVPan,
		FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0),
		TEXT("Map.WOTZoneInfo0"));
	const TMap<FString, FWoTRuntimeMapSlotPan> SlotPans = WoTRuntimeMapSlotPans(Parts);
	const FWoTRuntimeMapSlotPan* Dome = SlotPans.Find(TEXT("Pkg__Dome"));
	const FWoTRuntimeMapSlotPan* Wall = SlotPans.Find(TEXT("Pkg__Wall"));
	if (TestNotNull(TEXT("dome slot pan"), Dome))
	{
		TestEqual(TEXT("portal last writer overrides the main flags"),
			Dome->Flags, WoTRuntimeMapPFPortal | WoTRuntimeMapPFAutoVPan);
		TestEqual(TEXT("shared pan zone"), Dome->PanZoneActorIdentity,
			FString(TEXT("Map.WOTZoneInfo0")));
	}
	if (TestNotNull(TEXT("main-only slot pan"), Wall))
		TestEqual(TEXT("main-only slot keeps its flags"), Wall->Flags, 0u);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCoronaLateRenderStateSpec,
	"WoT.World.Corona.ProjectedSpritesGetRenderState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCoronaLateRenderStateSpec::RunTest(const FString&)
{
	// Guarded projection registers corona sprites before UWorld::InitWorld
	// allocates the scene: registered, but with no render state. The
	// publication-boundary/BeginPlay rebind must create it (a dirty mark
	// cannot), or the corona never draws.
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("corona test world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	FSceneInterface* const Scene = World->Scene;
	if (Scene == nullptr)
	{
		AddInfo(TEXT("the spec world has no scene; nothing to verify"));
		return true;
	}
	FActorSpawnParameters Params;
	Params.bDeferConstruction = true;
	World->Scene = nullptr;
	AWoTCorona* Corona = World->SpawnActor<AWoTCorona>(
		AWoTCorona::StaticClass(), FTransform::Identity, Params);
	World->Scene = Scene;
	if (!TestNotNull(TEXT("a deferred corona spawns"), Corona))
	{
		return false;
	}
	TestTrue(TEXT("both sprites registered during the scene-less projection"),
		Corona->Sprite->IsRegistered() && Corona->SpriteMod->IsRegistered());
	TestFalse(TEXT("the scene-less registration created no render state"),
		Corona->Sprite->IsRenderStateCreated()
			|| Corona->SpriteMod->IsRenderStateCreated());
	Corona->FinishSpawning(FTransform::Identity, true);
	Corona->EnsureRuntimeRenderState();
	TestTrue(TEXT("the additive sprite has a render state after the rebind"),
		Corona->Sprite->IsRenderStateCreated());
	TestTrue(TEXT("the modulate sprite has a render state after the rebind"),
		Corona->SpriteMod->IsRenderStateCreated());
	Corona->EnsureRuntimeRenderState();
	TestTrue(TEXT("a second rebind keeps both render states"),
		Corona->Sprite->IsRenderStateCreated()
			&& Corona->SpriteMod->IsRenderStateCreated());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPSkyBoxAndPanSpec,
	"WoT.World.RuntimeBSP.SkyBoxAndPan",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPSkyBoxAndPanSpec::RunTest(const FString&)
{
	FWoTSkyTestWorld SkyWorld;
	TArray<FWoTBSPPayloadData> Parts;
	WoTSkyTestScene(SkyWorld, Parts);
	FWoTWorldDescriptor Descriptor;
	Descriptor.MapIdentity = TEXT("SyntheticSkyMap");
	FWoTBSPPartDescriptor& MainDescriptor = Descriptor.BSPParts.AddDefaulted_GetRef();
	MainDescriptor.PartIdentity = Descriptor.MapIdentity;
	MainDescriptor.Role = EWoTBSPPartRole::Main;
	FWoTBSPPartDescriptor& PortalDescriptor = Descriptor.BSPParts.AddDefaulted_GetRef();
	PortalDescriptor.PartIdentity = Descriptor.MapIdentity + TEXT("_portals");
	PortalDescriptor.Role = EWoTBSPPartRole::Portal;
	const TArray<FWoTRuntimeBSPTestConverterResource> Resources = {
		{false, TEXT("Pkg__Wall"), TEXT("Pkg.Group.Wall"), true},
		{false, TEXT("Pkg__Dome"), TEXT("Pkg.Group.Dome"), true},
		{false, TEXT("Pkg__Shared"), TEXT("Pkg.Group.Shared"), true},
		{false, TEXT("Pkg__Window"), TEXT("Pkg.Group.Window"), true},
		{false, TEXT("Pkg__Sheet"), TEXT("Pkg.Group.Sheet"), true},
	};
	auto ScalarOf = [](UMaterialInstanceDynamic* Material, const TCHAR* Name)
	{
		float Value = -1.0f;
		if (Material != nullptr)
			Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value);
		return Value;
	};
	const FWoTRetailSkyWorldView View = SkyWorld.View();
	{
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("SkyBoxAndPan"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		uint32 Unresolved = MAX_uint32;
		FString Error;
		FString SkyError;
		if (!TestTrue(TEXT("sky map materializes"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, Resources, &Unresolved, nullptr, &View,
				&SkyError)))
		{
			AddError(Error);
			WoTRuntimeMapProjectionDestroyWorld(World);
			return false;
		}
		TestTrue(TEXT("no sky error"), SkyError.IsEmpty());
		TestEqual(TEXT("only the NULL-texture world slot is unresolved"), Unresolved, 1u);
		AWoTSkyBox* SkyBox = Owner != nullptr ? Owner->SkyBox.Get() : nullptr;
		if (TestNotNull(TEXT("an AWoTSkyBox is spawned"), SkyBox))
		{
			TestEqual(TEXT("SkyScale"), SkyBox->SkyScale, 217.0f);
			TestTrue(TEXT("SkyRotation"), FMath::IsNearlyEqual(
				SkyBox->SkyRotation.Yaw, -16200.0 * 360.0 / 65536.0, 1e-4));
			UStaticMeshComponent* SkyComponent = SkyBox->GetStaticMeshComponent();
			USceneCaptureComponentCube* Capture = SkyBox->GetSkyCapture();
			UTextureRenderTargetCube* Target = SkyBox->GetSkyCaptureTarget();
			TestTrue(TEXT("runtime sky mesh is capture-only"),
				SkyComponent != nullptr && SkyComponent->bVisibleInSceneCaptureOnly);
			if (TestNotNull(TEXT("runtime sky cube capture"), Capture))
			{
				TestTrue(TEXT("sky cube captures every frame only from its own mesh"),
					Capture->bCaptureEveryFrame && !Capture->bCaptureOnMovement
					&& !Capture->bAlwaysPersistRenderingState
					&& Capture->CaptureSource == SCS_SceneColorHDR
					&& Capture->PrimitiveRenderMode
						== ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList
					&& Capture->ShowOnlyComponents.Num() == 1 + SkyBox->GetSkyLayers().Num()
					&& Capture->ShowOnlyComponents[0].Get() == SkyComponent);
				for (UStaticMeshComponent* Layer : SkyBox->GetSkyLayers())
				{
					TestTrue(TEXT("each sky layer is capture-only and captured"),
						Layer != nullptr && Layer->bVisibleInSceneCaptureOnly
						&& Capture->ShowOnlyComponents.Contains(Layer));
				}
				TestEqual(TEXT("capture owns the sky cube target"),
					Capture->TextureTarget.Get(), Target);
			}
			if (TestNotNull(TEXT("runtime sky cube target"), Target))
			{
				TestEqual(TEXT("default cube face size"), Target->SizeX,
					WoTSkyWindowCaptureSize());
				TestEqual(TEXT("cube target is raw BGRA8"), Target->GetFormat(),
					PF_B8G8R8A8);
				TestTrue(TEXT("cube target is linear and has no generated mips"),
					Target->bForceLinearGamma && !Target->bAutoGenerateMips);
			}
			UStaticMesh* SkyMesh = SkyComponent != nullptr
				? SkyComponent->GetStaticMesh() : nullptr;
			if (TestNotNull(TEXT("sky mesh"), SkyMesh)
				&& TestNotNull(TEXT("sky render data"), SkyMesh->GetRenderData()))
			{
				// The translucent sheet left the sky mesh for an ordered layer.
				TestEqual(TEXT("dome and SkyZone split stay in the sky mesh"),
					SkyMesh->GetRenderData()->LODResources[0].Sections.Num(), 2);
			}
			UMaterialInstanceDynamic* Dome = SkyComponent != nullptr
				? Cast<UMaterialInstanceDynamic>(SkyComponent->GetMaterial(0)) : nullptr;
			const bool bLayerMasters = LoadObject<UMaterialInterface>(
					nullptr, WoTRuntimeSkyModulateMasterPath) != nullptr
				&& LoadObject<UMaterialInterface>(nullptr, WoTRuntimeSkyAddMasterPath) != nullptr;
			if (!bLayerMasters)
			{
				AddInfo(TEXT("the sky-layer masters are not generated; ")
					TEXT("checking the ordered max3 Entry-family fallback"));
			}
			const TArray<TObjectPtr<UStaticMeshComponent>>& Layers = SkyBox->GetSkyLayers();
			TestEqual(TEXT("the sheet draws as two passes (one without the masters)"),
				Layers.Num(), bLayerMasters ? 2 : 1);
			UMaterialInstanceDynamic* Sheet = !Layers.IsEmpty() && Layers.Last() != nullptr
				? Cast<UMaterialInstanceDynamic>(Layers.Last()->GetMaterial(0)) : nullptr;
			if (bLayerMasters && Layers.Num() == 2 && Layers[0] != nullptr && Layers[1] != nullptr)
			{
				UMaterialInstanceDynamic* Modulate =
					Cast<UMaterialInstanceDynamic>(Layers[0]->GetMaterial(0));
				TestTrue(TEXT("the modulate pass draws first"),
					Modulate != nullptr && Modulate->Parent->GetName()
						== TEXT("M_WoT_EntrySkyModulate")
					&& Layers[0]->TranslucencySortPriority == 0
					&& Layers[1]->TranslucencySortPriority == 1);
				TestTrue(TEXT("both passes draw the same sheet mesh"),
					Layers[0]->GetStaticMesh() == Layers[1]->GetStaticMesh()
					&& Layers[0]->GetStaticMesh() != nullptr);
				TestTrue(TEXT("the modulate pass carries the sheet's pan"),
					Modulate != nullptr && FMath::IsNearlyEqual(
						ScalarOf(Modulate, TEXT("PanV")), 1.09375f, 1e-6f)
					&& Owner->SceneTimeMaterials.Contains(Modulate));
			}
			if (TestNotNull(TEXT("dome world-slot material"), Dome))
			{
				TestTrue(TEXT("dome material is an owned world instance"),
					Owner->RuntimeMaterials.Contains(Dome));
				TestEqual(TEXT("opaque dome keeps the world master"),
					Dome->Parent->GetName(), FString(TEXT("M_WoT_World")));
				TestTrue(TEXT("dome PanU = 35*0.5/32 (neutral test texture)"),
					FMath::IsNearlyEqual(ScalarOf(Dome, TEXT("PanU")), 0.546875f, 1e-6f));
				TestEqual(TEXT("dome has no V pan"), ScalarOf(Dome, TEXT("PanV")), 0.0f);
				TestFalse(TEXT("Time-panned master is not scene-time driven"),
					Owner->SceneTimeMaterials.Contains(Dome));
			}
			if (TestNotNull(TEXT("sheet world-slot material"), Sheet))
			{
				// The last pass adds Tex*M (sky_layer_materials.py); a closure
				// without the masters keeps the Entry-family max3 material.
				TestEqual(TEXT("translucent two-sided sheet's last pass"),
					Sheet->Parent->GetName(),
					FString(bLayerMasters ? TEXT("M_WoT_EntrySkyAdd")
						: TEXT("M_WoT_EntryTranslucent_TwoSided")));
				TestTrue(TEXT("the sheet material is owned by the runtime world"),
					Owner->RuntimeMaterials.Contains(Sheet));
				TestTrue(TEXT("sheet PanV = 35/32"),
					FMath::IsNearlyEqual(ScalarOf(Sheet, TEXT("PanV")), 1.09375f, 1e-6f));
				TestTrue(TEXT("Entry-family panned sheet is scene-time driven"),
					Owner->SceneTimeMaterials.Contains(Sheet));
				Owner->ApplySceneTime(2.5f);
				TestEqual(TEXT("WoTSceneTime follows the driver"),
					ScalarOf(Sheet, TEXT("WoTSceneTime")), 2.5f);
			}
		}
		TestTrue(TEXT("scene-time driver ticks from BeginPlay"),
			Owner != nullptr && Owner->PrimaryActorTick.bStartWithTickEnabled);
		if (Owner != nullptr && TestEqual(TEXT("two geometry parts"),
			Owner->GeometryComponents.Num(), 2))
		{
			UStaticMesh* MainMesh = Owner->GeometryComponents[0]->GetStaticMesh();
			TestEqual(TEXT("world mesh hides the dome and the __SkyZone split"),
				MainMesh != nullptr && MainMesh->GetRenderData() != nullptr
					? MainMesh->GetRenderData()->LODResources[0].Sections.Num() : -1,
				4);
			UMaterialInstanceDynamic* Window = Cast<UMaterialInstanceDynamic>(
				Owner->GeometryComponents[0]->GetMaterial(2));
			if (TestNotNull(TEXT("world FakeBackdrop uses the sky-window master"), Window))
			{
				TestEqual(TEXT("world FakeBackdrop master"), Window->Parent->GetName(),
					FString(TEXT("M_WoT_SkyWindow")));
				UTexture* BoundCube = nullptr;
				Window->GetTextureParameterValue(
					FHashedMaterialParameterInfo(WoTRuntimeSkyWindowCubeParameter),
					BoundCube);
				TestEqual(TEXT("world FakeBackdrop samples plan zero's live cube"),
					BoundCube, static_cast<UTexture*>(SkyBox != nullptr
						? SkyBox->GetSkyCaptureTarget() : nullptr));
			}
			TestFalse(TEXT("an all-sky portal part renders nothing in the world"),
				Owner->GeometryComponents[1]->IsVisible());
		}
		WoTRuntimeMapProjectionDestroyWorld(World);
	}
	for (const EWoTRuntimeSkyStageFault Fault : {
		EWoTRuntimeSkyStageFault::SkyBoxAllocation,
		EWoTRuntimeSkyStageFault::MaterialBinding})
	{
		// A late sky failure is skip-don't-reject: the map still materializes,
		// with every sky section left in its world mesh and no sky box.
		const bool bAllocationFault = Fault == EWoTRuntimeSkyStageFault::SkyBoxAllocation;
		UWorld* World = WoTRuntimeMapProjectionWorld(bAllocationFault
			? TEXT("SkyStageAllocationFault") : TEXT("SkyStageBindingFault"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		uint32 Unresolved = MAX_uint32;
		FString Error;
		FString SkyError;
		const FString Label = bAllocationFault ? TEXT("allocation fault: ")
			: TEXT("binding fault: ");
		if (!TestTrue(Label + TEXT("map still materializes"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, Resources, &Unresolved, nullptr, &View,
				&SkyError, Fault)))
		{
			AddError(Error);
			WoTRuntimeMapProjectionDestroyWorld(World);
			continue;
		}
		TestTrue(Label + TEXT("no hard error"), Error.IsEmpty());
		TestEqual(Label + TEXT("sky_error names the failed stage"), SkyError,
			FString(bAllocationFault ? TEXT("runtime sky box allocation failed")
				: TEXT("a runtime sky section has no world-slot material")));
		TestNull(*(Label + TEXT("no sky box is kept")),
			Owner != nullptr ? Owner->SkyBox.Get() : nullptr);
		int32 LiveSkyBoxes = 0;
		for (TActorIterator<AWoTSkyBox> It(World); It; ++It) ++LiveSkyBoxes;
		TestEqual(Label + TEXT("the partial sky box was destroyed"), LiveSkyBoxes, 0);
		if (Owner != nullptr && TestEqual(Label + TEXT("two geometry parts"),
			Owner->GeometryComponents.Num(), 2))
		{
			UStaticMesh* MainMesh = Owner->GeometryComponents[0]->GetStaticMesh();
			TestEqual(Label + TEXT("the world mesh keeps every sky section"),
				MainMesh != nullptr && MainMesh->GetRenderData() != nullptr
					? MainMesh->GetRenderData()->LODResources[0].Sections.Num() : -1,
				6);
			TestTrue(Label + TEXT("the portal sky sheet stays drawn in the world"),
				Owner->GeometryComponents[1]->IsVisible());
			UMaterialInstanceDynamic* Dome = Cast<UMaterialInstanceDynamic>(
				Owner->GeometryComponents[0]->GetMaterial(1));
			TestTrue(Label + TEXT("the kept dome section keeps its panned material"),
				FMath::IsNearlyEqual(ScalarOf(Dome, TEXT("PanU")), 0.546875f, 1e-6f));
			UMaterialInstanceDynamic* Sheet = Cast<UMaterialInstanceDynamic>(
				Owner->GeometryComponents[1]->GetMaterial(0));
			TestTrue(Label + TEXT("the kept sheet stays scene-time driven"),
				Sheet != nullptr && Owner->SceneTimeMaterials.Contains(Sheet));
		}
		WoTRuntimeMapProjectionDestroyWorld(World);
	}
	{
		// Without a world view (legacy callers): no sky, every section drawn.
		UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("SkyBoxAndPanLegacy"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		uint32 FallbackParts = MAX_uint32;
		FString Error;
		if (TestTrue(TEXT("viewless map materializes"),
			WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor, Parts, Owner,
				FallbackParts, Error, Resources)))
		{
			TestNull(TEXT("no sky box without a world view"),
				Owner != nullptr ? Owner->SkyBox.Get() : nullptr);
			if (Owner != nullptr && Owner->GeometryComponents.Num() == 2)
			{
				UStaticMesh* MainMesh = Owner->GeometryComponents[0]->GetStaticMesh();
				TestEqual(TEXT("every world section including the window renders"),
					MainMesh != nullptr && MainMesh->GetRenderData() != nullptr
						? MainMesh->GetRenderData()->LODResources[0].Sections.Num() : -1,
					6);
				UMaterialInstanceDynamic* Window = Cast<UMaterialInstanceDynamic>(
					Owner->GeometryComponents[0]->GetMaterial(4));
				TestTrue(TEXT("viewless FakeBackdrop is still opaque, never a hole"),
					Window != nullptr && Window->Parent != nullptr
					&& Window->Parent->GetName() == TEXT("M_WoT_SkyWindow"));
				UMaterialInstanceDynamic* Dome = Cast<UMaterialInstanceDynamic>(
					Owner->GeometryComponents[0]->GetMaterial(1));
				TestTrue(TEXT("viewless pan speed is ZoneInfo's 1.0"),
					FMath::IsNearlyEqual(ScalarOf(Dome, TEXT("PanU")), 1.09375f, 1e-6f));
			}
		}
		else AddError(Error);
		WoTRuntimeMapProjectionDestroyWorld(World);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMapSkyRealRetailCensusSpec,
	"WoT.World.Sky.RealRetailCensus",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeMapSkyRealRetailCensusSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: production census not verified"));
		return true;
	}
	// Development-pipeline expectations (skygeo.py/setup_childsky.py outputs for
	// the shipped maps: winner, zone, sorted slot set, origin, rotation, face
	// distance, world radius, SkyScale). Winner/zone/slots are exact; the
	// numeric columns carry float tolerances; the scale is exact.
	struct FExpectedSky
	{
		const TCHAR* Map;
		int32 SkyActors;
		const TCHAR* Winner;
		int32 Zone;
		bool bTraversablePortal;
		float Scale;
		double MinDistance;
		double WorldRadius;
		double OriginX;
		double OriginY;
		double OriginZ;
		int32 YawUnits;
		const TCHAR* Slots;
		int32 PortalSkySections;
	};
	static const FExpectedSky ExpectedSkies[] = {
		{TEXT("Arena_01"), 1, TEXT("SkyZoneInfo1"), 4, false, 119.0f, 108.1742, 5134.187,
			-1297.400269, 3081.547119, 1555.825806, 0,
			TEXT("NatureT__DirtDirt_010|NatureT__Sky_440|NatureT__Sky_440_T|ScottT__mountain2_2S|WarrenT__Black_U"), 0},
		{TEXT("Arena_02"), 1, TEXT("SkyZoneInfo0"), 5, false, 7286.0f, 1.8118, 5279.989,
			6015.179688, 1151.953857, -245.58812, 0,
			TEXT("NatureT__RockSurf503|NatureT__Sky_340_T_2S_SL|NatureT__Sky_439|NatureT__Sky_439_T_2S|ScottT__sm_tilemt01"), 0},
		{TEXT("Arena_03"), 0, nullptr, 0, false, 0.0f, 0.0, 0.0, 0.0, 0.0, 0.0, 0, TEXT(""), 0},
		{TEXT("Arena_04"), 1, TEXT("SkyZoneInfo1"), 2, true, 221.0f, 37.8562, 3335.355,
			452.996582, -408.194824, -1802.143799, 0,
			TEXT("NatureT__SkyMod443_Mod_2S|WarrenT__Black_U"), 1},
		{TEXT("Arena_05"), 1, TEXT("SkyZoneInfo0"), 6, false, 180.0f, 93.2161, 6695.239,
			-9607.353516, 11007.041016, 3933.740479, 0,
			TEXT("NatureT__SkyMod442_SL|NatureT__SkyMod442_T|ScottT__mountain2"), 0},
		{TEXT("Arena_06"), 1, TEXT("SkyZoneInfo1"), 1, false, 548.0f, 22.1, 4839.435,
			1296.829956, 4893.612793, -285.899963, 0,
			TEXT("ForsakenT__StonRail2703__SkyZone|NatureT__Sky_340a|NatureT__Sky_340a_Mod_2S_U"), 0},
		{TEXT("Arena_07"), 1, TEXT("SkyZoneInfo1"), 3, true, 248.0f, 37.8562, 3751.982,
			-83.003418, -3936.194824, -1770.143799, 0,
			TEXT("NatureT__SkyMod443_Mod_2S|WarrenT__Black_U"), 1},
		{TEXT("Arena_08"), 1, TEXT("SkyZoneInfo0"), 3, false, 53.0f, 253.8412, 5281.454,
			-1540.965454, -5763.063477, -1917.841187, 0,
			TEXT("JamesT__moon01_M_T_2S_U|LensFlareT__Corona1_M_T_2S_U|NatureT__Sky_440_T_2S|NatureT__Sky_440_U"), 0},
		{TEXT("Arena_09"), 0, nullptr, 0, false, 0.0f, 0.0, 0.0, 0.0, 0.0, 0.0, 0, TEXT(""), 0},
		{TEXT("Arena_11"), 1, TEXT("SkyZoneInfo0"), 2, false, 417.0f, 105.4238, 17548.522,
			-8245.822266, -1006.515625, 31616.576172, 0,
			TEXT("JamesT__small_moon_M_2S_SL|NatureT__Sky_340|Nature_T__Sky_440_T_T_2S"), 0},
		{TEXT("Arena_12"), 0, nullptr, 0, false, 0.0f, 0.0, 0.0, 0.0, 0.0, 0.0, 0, TEXT(""), 0},
		{TEXT("Battle2_01"), 1, TEXT("SkyZoneInfo0"), 2, false, 811.0f, 65.1986, 21130.109,
			-10459.455078, 2572.522217, 1505.19873, -16200,
			TEXT("NatureT__SkyMod443_Mod_2S_U|NatureT__Sky_440_T_2S|NatureT__Stars_436_U|ScottT__flatblack_U"), 0},
		{TEXT("Battle2_02"), 3, TEXT("SkyZoneInfo2"), 23, false, 879.0f, 102.7939, 36127.096,
			7339.84082, 12329.109375, 12666.138672, 0,
			TEXT("NatureT__Sky_Clou3670blu|NatureT__Sky_Clou3670blu_T_2S|ScottT__flatblack_SL"), 0},
		{TEXT("Battle2_03"), 3, TEXT("SkyZoneInfo2"), 6, false, 1099.0f, 78.2016, 34352.900,
			12351.97168, 3280.952637, -311.201569, 0,
			TEXT("NatureT__Sky_Clou3670blu__SkyZone"), 0},
		{TEXT("Battle2_04"), 2, TEXT("SkyZoneInfo0"), 6, false, 1596.0f, 23.3912, 14923.811,
			4034.816162, -2881.541504, 1111.391235, 0,
			TEXT("NatureT__Sky_440_SL__SkyZone|NatureT__Sky_440_T_2S__SkyZone|NatureT__Sky_440__SkyZone"), 0},
		{TEXT("Battle2_05"), 3, TEXT("SkyZoneInfo4"), 35, false, 1317.0f, 57.5859, 30334.272,
			-2265.542969, 5221.644531, 3085.249268, 0,
			TEXT("ForsakenT__Sky_340a_T_2S|NatureT__Stars_436|NatureT__Stars_436_U|Skybox01__Sun_T_2S_U"), 0},
		{TEXT("Battle3_01"), 1, TEXT("SkyZoneInfo0"), 19, false, 984.0f, 65.1986, 25657.671,
			-10459.455078, 2572.522217, 1505.19873, -16200,
			TEXT("NatureT__SkyMod443_Mod_2S_U|NatureT__Sky_440_T_2S|NatureT__Stars_436_U"), 0},
		{TEXT("Battle3_02"), 1, TEXT("SkyZoneInfo0"), 20, true, 1014.0f, 35.4482, 14365.591,
			-7393.156738, 27.39609, -2987.02417, 0,
			TEXT("NatureT__Sky_439_T_2S|ParticleSystems__PF15_T_2S_U|WOT__Watr_400"), 1},
		{TEXT("Battle4_01"), 1, TEXT("SkyZoneInfo4"), 12, false, 1302.0f, 61.4141, 31975.729,
			-2265.542969, 5221.644531, 3094.249268, 0,
			TEXT("NatureT__Sky_340_T_2S|ScottT__flatblack_U"), 0},
	};
	// import_map.py:1124-1131 over the development pipeline's own inputs
	// (bsp.json last-writer flags merged main<-portal, pan_zone, T3D speeds,
	// TEXINFO/flipbook/32 sizes): every AutoU/VPan slot on the 19 maps.
	struct FExpectedPan
	{
		const TCHAR* Map;
		const TCHAR* Slot;
		float PanU;
		float PanV;
	};
	static const FExpectedPan ExpectedPans[] = {
		{TEXT("Arena_01"), TEXT("NatureT__Sky_440"), 0.013671875f, 0.0f},
		{TEXT("Arena_01"), TEXT("NatureT__Sky_440_T"), 0.0f, 0.02734375f},
		{TEXT("Arena_02"), TEXT("NatureT__Sky_340_T_2S_SL"), 0.013671875f, 0.0f},
		{TEXT("Arena_02"), TEXT("NatureT__Sky_439_T_2S"), 0.013671875f, 0.0f},
		{TEXT("Arena_04"), TEXT("NatureT__SkyMod442"), 0.0068359375f, 0.0068359375f},
		{TEXT("Arena_04"), TEXT("NatureT__SkyMod443_Mod_2S"), 0.0205078125f, 0.0f},
		{TEXT("Arena_05"), TEXT("JHPFireEng__M01ModWet"), 0.0f, 0.13671875f},
		{TEXT("Arena_05"), TEXT("NatureT__SkyMod442_T"), 0.0068359375f, 0.01025390625f},
		{TEXT("Arena_06"), TEXT("NatureT__Sky_340a"), 0.0f, 0.013671875f},
		{TEXT("Arena_06"), TEXT("NatureT__Sky_340a_Mod_2S_U"), 0.013671875f, 0.013671875f},
		{TEXT("Arena_07"), TEXT("NatureT__SkyMod442"), 0.0068359375f, 0.0068359375f},
		{TEXT("Arena_07"), TEXT("NatureT__SkyMod443_Mod_2S"), 0.0205078125f, 0.0f},
		{TEXT("Arena_08"), TEXT("NatureT__Sky_440_T_2S"), 0.0f, 0.13671875f},
		{TEXT("Arena_08"), TEXT("NatureT__Sky_440_U"), 0.13671875f, 0.0f},
		{TEXT("Arena_11"), TEXT("Nature_T__Sky_440_T_T_2S"), 0.013671875f, 0.0f},
		{TEXT("Battle2_01"), TEXT("NatureT__SkyMod443_Mod_2S_U"), 0.013671875f, 0.0109375f},
		{TEXT("Battle2_01"), TEXT("NatureT__Sky_440_T_2S"), 0.013671875f, 0.0f},
		{TEXT("Battle2_02"), TEXT("NatureT__SkyMod442_T_2S"), 0.013671875f, 0.013671875f},
		{TEXT("Battle2_02"), TEXT("NatureT__Sky_433a_T_2S"), 0.02734375f, 0.0f},
		{TEXT("Battle2_02"), TEXT("NatureT__Sky_Clou3670blu"), 0.0068359375f, 0.0068359375f},
		{TEXT("Battle2_02"), TEXT("NatureT__Sky_Clou3670blu_T_2S"), 0.0068359375f, 0.0068359375f},
		{TEXT("Battle2_03"), TEXT("NatureT__SkyMod442_Mod_2S_U"), 0.0f, 0.013671875f},
		{TEXT("Battle2_03"), TEXT("NatureT__Sky_Clou3670blu"), 0.00615234375f, 0.00615234375f},
		{TEXT("Battle2_03"), TEXT("NatureT__Sky_Clou3670blu__SkyZone"), 0.00615234375f, 0.00615234375f},
		{TEXT("Battle2_04"), TEXT("NatureT__Sky_440_T_2S"), 0.02734375f, 0.0f},
		{TEXT("Battle2_04"), TEXT("NatureT__Sky_440_T_2S__SkyZone"), 0.02734375f, 0.0f},
		{TEXT("Battle2_05"), TEXT("ForsakenT__Sky_340a_T_2S"), 0.013671875f, 0.0068359375f},
		{TEXT("Battle2_05"), TEXT("NatureT__SkyMod445_Mod_2S_U"), 0.0f, 0.013671875f},
		{TEXT("Battle2_05"), TEXT("Nature_T__Sky_439_T_T_2S"), 0.0109375f, 0.0f},
		{TEXT("Battle3_01"), TEXT("NatureT__SkyMod443_Mod_2S_U"), 0.013671875f, 0.0109375f},
		{TEXT("Battle3_01"), TEXT("NatureT__Sky_440_T_2S"), 0.013671875f, 0.0f},
		{TEXT("Battle3_02"), TEXT("NatureT__Sky_439_T_2S"), 0.013671875f, 0.0f},
		{TEXT("Battle3_02"), TEXT("WOT__Watr_400"), 0.109375f, 0.109375f},
		{TEXT("Battle4_01"), TEXT("NatureT__Sky_340_T_2S"), 0.013671875f, 0.0068359375f},
	};

	const FString RetailRoot = WoTRuntimeProjectionRepoPath(TEXT("original"));
	if (!IFileManager::Get().FileExists(*(RetailRoot / TEXT("Maps/Arena_01.wot"))))
	{
		AddError(TEXT("required original/Maps/Arena_01.wot is missing"));
		return false;
	}
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
	{
		AddError(LoadedContract.GetError().ToString());
		return false;
	}
	FWoTRetailContract WorldContract;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		const bool bMultiplayerWorldFact =
			(Fact.Domain == EWoTFactDomain::BSP || Fact.Domain == EWoTFactDomain::World)
			&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
		if (!bMultiplayerWorldFact) continue;
		WorldContract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& Source = WorldSources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath))))
		{
			return false;
		}
	}
	if (!TestEqual(TEXT("sky census has nineteen map packages"), WorldSources.Num(), 19))
		return false;
	FWoTSkyCensusWriter Writer;
	FWoTRuntimeProjectionCensusProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorlds =
		ImportRequiredWorldSetPackageBytesForTesting(
			WorldSources, WorldContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds all world-v3 and BSP objects"),
		ImportedWorlds.IsError()))
	{
		AddError(ImportedWorlds.GetError().ToString());
		return false;
	}
	// The development pan-rate texture size rule, from the canonical texture
	// projection the native writer normalizes UVs with: static w/h (never
	// multiplied by DrawScale), the fractal record size, and 32 for a
	// dangling/NULL texture (the generator records those as 1x1).
	auto PanTextureSize = [](const FString& Package, const FString& Name,
		uint32& OutWidth, uint32& OutHeight)
	{
		OutWidth = WoTRuntimeMapDanglingPanTextureSize;
		OutHeight = WoTRuntimeMapDanglingPanTextureSize;
		for (const WoT::Retail::Generated::FWorldTexture& Texture
			: WoT::Retail::Generated::WorldTextures)
		{
			if (FStringView(Package).Equals(Texture.CanonicalPackage, ESearchCase::CaseSensitive)
				&& FStringView(Name).Equals(Texture.CanonicalName, ESearchCase::CaseSensitive))
			{
				if (Texture.Width > 1 || Texture.Height > 1)
				{
					OutWidth = Texture.Width;
					OutHeight = Texture.Height;
				}
				return;
			}
		}
	};

	// The version-6 sky-rest census (skybake.py bake_rest, native
	// parity pinned in WoT.RetailReader.BSPWorld.RealRetailAtomicSetAndPins)
	// and its skygeo rest groups in the sky mesh of the portal-unsealed maps.
	struct FExpectedRest
	{
		const TCHAR* Map;
		const TCHAR* Slots;
		uint32 Triangles;
		uint32 Surfaces;
	};
	static const FExpectedRest ExpectedRests[] = {
		{TEXT("Arena_04"), TEXT("SkyRest__JamesT__moon02_M_T_2S|SkyRest__NatureT__SkyMod442"), 10, 6},
		{TEXT("Arena_07"), TEXT("SkyRest__JamesT__moon02_M_T_2S|SkyRest__NatureT__SkyMod442"), 10, 6},
		{TEXT("Battle3_02"), TEXT("SkyRestBlock|SkyRest__NatureT__RockSurf500a|SkyRest__NatureT__Sky_Clou3670blu"), 191, 26},
	};
	uint32 SkyMaps = 0;
	uint32 ResidualMaps = 0;
	uint32 RestMaps = 0;
	uint32 PannedSlots = 0;
	uint32 CheckedMaps = 0;
	// Retail PF_Mirrored census (Tools/convert/presentation_census.py): sorted
	// "nx,ny,nz@n.P" planes, drawn mirrored world nodes, mirrored sections.
	struct FExpectedMirrors
	{
		const TCHAR* Map;
		const TCHAR* Planes;
		uint32 Nodes;
		uint32 Sections;
	};
	static const FExpectedMirrors ExpectedMirrors[] = {
		{TEXT("Battle2_01"), TEXT("0,0,1@-2944"), 21, 1},
		{TEXT("Battle2_04"), TEXT("0,0,1@-5664|0,0,1@-64|0,1,0@-467"), 4, 3},
		{TEXT("Battle3_01"), TEXT("0,0,1@-2965"), 7, 1},
	};
	uint32 MirrorPlaneTotal = 0;
	for (const FWoTWorldDescriptor& Descriptor : ImportedWorlds.GetValue().Worlds)
	{
		const FExpectedSky* Expected = nullptr;
		for (const FExpectedSky& Row : ExpectedSkies)
			if (Descriptor.MapIdentity == Row.Map) Expected = &Row;
		if (!TestNotNull(Descriptor.MapIdentity + TEXT(" has a sky expectation"), Expected))
			continue;
		TArray<uint8>* WorldBytes = Writer.Objects.Find(Descriptor.WorldLogicalId);
		if (!TestNotNull(Descriptor.MapIdentity + TEXT(" world object"), WorldBytes)) continue;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldBytes),
				TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" world frame decodes"),
			DecodedWorld.IsError())) continue;
		FWoTRuntimeWorldResource Resource;
		FWoTBootstrapStatus Loaded =
			Resource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue());
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" world decodes"), Loaded.IsError()))
			continue;
		TArray<FWoTBSPPayloadData> Parts;
		bool bPartsDecoded = true;
		for (const FWoTBSPPartDescriptor& PartDescriptor : Descriptor.BSPParts)
		{
			TArray<uint8>* PartBytes = Writer.Objects.Find(PartDescriptor.LogicalId);
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedPart = PartBytes != nullptr
				? FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*PartBytes),
					TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"))
				: TWoTBootstrapResult<FWoTRuntimeVerifiedObject>::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::CorruptInstallation, TEXT("missing BSP part object")));
			FString PartError;
			FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
			if (DecodedPart.IsError())
			{
				PartError = DecodedPart.GetError().ToString();
				bPartsDecoded = false;
			}
			else
			{
				bPartsDecoded = UWoTBSPPayloadLibrary::DecodePayload(
					DecodedPart.GetValue().Payload(), Part, PartError);
			}
			if (!bPartsDecoded)
			{
				AddError(Descriptor.MapIdentity + TEXT(" BSP part ")
					+ PartDescriptor.PartIdentity + TEXT(": ") + PartError);
				break;
			}
			// v4 introduced section identity; v7/v8 append MacroTexture facts.
			TestTrue(PartDescriptor.PartIdentity + TEXT(" carries BSP-v4+ section identity"),
				Part.FormatVersion >= 4);
		}
		if (!bPartsDecoded) continue;
		++CheckedMaps;
		FWoTRetailSkyWorldView View;
		View.Actors = Resource.Actors();
		View.Zones = Resource.Zones();
		View.Nodes = Resource.Nodes();
		View.Surfaces = Resource.Surfaces();
		FWoTRuntimeMapSkyPlan Plan;
		FString Error;
		const FString Prefix = Descriptor.MapIdentity + TEXT(" sky ");
		if (!TestTrue(Prefix + TEXT("plans without error"),
			WoTBuildRuntimeMapSkyPlan(View, Parts, Plan, Error)))
		{
			AddError(Descriptor.MapIdentity + TEXT(": ") + Error);
			continue;
		}
		TestEqual(Prefix + TEXT("SkyZoneInfo census"),
			Plan.Selection.SkyActors.Num(), Expected->SkyActors);
		TestTrue(Prefix + TEXT("presence"), Plan.bHasSky == (Expected->Winner != nullptr));
		if (Expected->Winner != nullptr && Plan.bHasSky)
		{
			++SkyMaps;
			TestTrue(Prefix + TEXT("winner is skybake's majority winner"),
				Plan.SkyActorIdentity.EndsWith(FString(TEXT(".")) + Expected->Winner,
					ESearchCase::IgnoreCase));
			TestEqual(Prefix + TEXT("zone"), Plan.Selection.Zone, Expected->Zone);
			TestEqual(Prefix + TEXT("zone is unique"), Plan.Selection.ZoneMatchCount, 1);
			TestTrue(Prefix + TEXT("sky-rest traversal"),
				Plan.Selection.bTraversablePortal == Expected->bTraversablePortal);
			ResidualMaps += Plan.Selection.bTraversablePortal ? 1u : 0u;
			TestEqual(Prefix + TEXT("slot set equals skygeo.json materials"),
				FString::Join(Plan.SkySlots, TEXT("|")), FString(Expected->Slots));
			TestEqual(Prefix + TEXT("no FakeBackdrop sky slot"), Plan.FakeBackdropSkySlots, 0u);
			int32 PortalSkySections = 0;
			for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
			{
				if (!Parts[PartIndex].bPortal) continue;
				for (const bool bSkySection : Plan.SkySections[PartIndex])
					PortalSkySections += bSkySection ? 1 : 0;
			}
			TestEqual(Prefix + TEXT("portal-part sky sections"), PortalSkySections,
				Expected->PortalSkySections);
			TestTrue(Prefix + TEXT("origin"), Plan.Origin.Equals(
				FVector(Expected->OriginX, Expected->OriginY, Expected->OriginZ), 0.01));
			TestTrue(Prefix + TEXT("rotation"), FMath::IsNearlyEqual(Plan.Rotation.Yaw,
				Expected->YawUnits * 360.0 / 65536.0, 1e-4)
				&& Plan.Rotation.Pitch == 0.0 && Plan.Rotation.Roll == 0.0);
			TestTrue(Prefix + FString::Printf(TEXT("min face distance %.4f"),
				Plan.MinSurfaceDistance), FMath::IsNearlyEqual(
					Plan.MinSurfaceDistance, Expected->MinDistance, 0.01));
			TestTrue(Prefix + FString::Printf(TEXT("world radius %.3f"), Plan.WorldRadius),
				FMath::IsNearlyEqual(Plan.WorldRadius, Expected->WorldRadius, 0.05));
			TestEqual(Prefix + TEXT("SkyScale"), Plan.Scale, Expected->Scale);
			TestTrue(Prefix + TEXT("sky mesh has triangles"), Plan.SkyTriangleCount > 0);
			TestTrue(Prefix + TEXT("the version-6 sky-rest census rides the main part"),
				Plan.bSkyRestCensus);
			TestEqual(Prefix + TEXT("every rest group binds a world slot"),
				Plan.SkyRestMissingSlots, 0u);
			const FExpectedRest* ExpectedRest = nullptr;
			for (const FExpectedRest& Row : ExpectedRests)
				if (Descriptor.MapIdentity == Row.Map) ExpectedRest = &Row;
			TestEqual(Prefix + TEXT("rest groups equal skygeo's SkyRest groups"),
				FString::Join(Plan.SkyRestSlots, TEXT("|")),
				FString(ExpectedRest != nullptr ? ExpectedRest->Slots : TEXT("")));
			TestEqual(Prefix + TEXT("rest triangles"), Plan.SkyRestTriangleCount,
				ExpectedRest != nullptr ? ExpectedRest->Triangles : 0u);
			TestEqual(Prefix + TEXT("census surfaces"), Plan.SkyRestSurfaceCount,
				ExpectedRest != nullptr ? ExpectedRest->Surfaces : 0u);
			TestEqual(Prefix + TEXT("rest content exactly on the portal-unsealed maps"),
				ExpectedRest != nullptr, Plan.Selection.bTraversablePortal);
			RestMaps += Plan.SkyRestTriangleCount > 0 ? 1u : 0u;
			UStaticMesh* SkyMesh = NewObject<UStaticMesh>(GetTransientPackage());
			FString MeshError;
			TestTrue(Prefix + TEXT("sky payload builds a render mesh"), SkyMesh != nullptr
				&& WoTRuntimeBSPBuildRenderMesh(*SkyMesh, Plan.SkyMesh, MeshError));
			if (Descriptor.MapIdentity == TEXT("Arena_02"))
			{
				// Retail draws non-opaque sky surfaces back to front. The Sky_340
				// pyramid encloses the sky origin, so its orange-lit, panning layer
				// draws last over the Sky_439 sheet and tints the whole sky sepia.
				TArray<bool> Layers;
				for (const FWoTBSPPayloadSection& Section : Plan.SkyMesh.Sections)
					Layers.Add((Section.SurfaceFlags & (0x04u | 0x40u)) != 0);
				TArray<FString> Ordered;
				for (const int32 Section : WoTRuntimeMapSkyLayerOrder(Plan.SkyMesh, Layers))
					Ordered.Add(Plan.SkySlots.IsValidIndex(Section) ? Plan.SkySlots[Section] : FString());
				TestEqual(Prefix + TEXT("translucent layers draw far to near"),
					FString::Join(Ordered, TEXT("|")),
					FString(TEXT("NatureT__Sky_439_T_2S|NatureT__Sky_340_T_2S_SL")));
			}
			AddInfo(FString::Printf(TEXT("%s sky %s zone=%d slots=%d tris=%u d=%.4f r=%.3f scale=%.0f rest=%d"),
				*Descriptor.MapIdentity, *Plan.SkyActorIdentity, Plan.Selection.Zone,
				Plan.SkySlots.Num(), Plan.SkyTriangleCount, Plan.MinSurfaceDistance,
				Plan.WorldRadius, Plan.Scale, Plan.Selection.bTraversablePortal ? 1 : 0));
		}

		const TMap<FString, FWoTRuntimeMapSlotPan> SlotPans = WoTRuntimeMapSlotPans(Parts);
		const FWoTRuntimeMapPanSpeeds Speeds = WoTRuntimeMapBuildPanSpeeds(Resource.Actors());
		TestEqual(Descriptor.MapIdentity + TEXT(" pan speeds are well formed"),
			Speeds.MalformedZones, 0u);
		TMap<FString, FVector2f> ActualPans;
		for (const FWoTBSPPayloadData& Part : Parts)
		{
			for (const FWoTBSPPayloadSection& Section : Part.Sections)
			{
				const FWoTRuntimeMapSlotPan* SlotPan = SlotPans.Find(Section.MaterialSlot);
				if (SlotPan == nullptr || (SlotPan->Flags
					& (WoTRuntimeMapPFAutoUPan | WoTRuntimeMapPFAutoVPan)) == 0) continue;
				uint32 Width = 0;
				uint32 Height = 0;
				PanTextureSize(Section.CanonicalPackage, Section.CanonicalName, Width, Height);
				ActualPans.Add(Section.MaterialSlot, WoTRuntimeMapPanRate(SlotPan->Flags,
					Speeds.Find(SlotPan->PanZoneActorIdentity), Width, Height));
			}
		}
		int32 ExpectedCount = 0;
		for (const FExpectedPan& Row : ExpectedPans)
		{
			if (Descriptor.MapIdentity != Row.Map) continue;
			++ExpectedCount;
			const FVector2f* Actual = ActualPans.Find(Row.Slot);
			if (!TestNotNull(Descriptor.MapIdentity + TEXT(" pans ") + Row.Slot, Actual))
				continue;
			TestTrue(Descriptor.MapIdentity + TEXT(" ") + Row.Slot + FString::Printf(
				TEXT(" pan (%.9f, %.9f)"), Actual->X, Actual->Y),
				FMath::IsNearlyEqual(Actual->X, Row.PanU, 1e-6f)
				&& FMath::IsNearlyEqual(Actual->Y, Row.PanV, 1e-6f));
		}
		TestEqual(Descriptor.MapIdentity + TEXT(" panned slot census"),
			ActualPans.Num(), ExpectedCount);
		PannedSlots += static_cast<uint32>(ActualPans.Num());

		// The mirror stage's section rule and plane grouping over the decoded
		// parts, cross-checked against every drawn world node whose surface
		// carries PF_Mirrored (the per-surface flags the slot's last-writer
		// flags stand in for).
		{
			const FString MirrorPrefix = Descriptor.MapIdentity + TEXT(" mirror ");
			TArray<const FWoTBSPPayloadData*> MirrorPayloads;
			TArray<TArray<bool>> MirrorFlags;
			uint32 MirrorSections = 0;
			uint32 MirrorSectionTriangles = 0;
			for (const FWoTBSPPayloadData& Part : Parts)
			{
				MirrorPayloads.Add(&Part);
				TArray<bool>& Flags = MirrorFlags.AddDefaulted_GetRef();
				for (const FWoTBSPPayloadSection& Section : Part.Sections)
				{
					const bool bMirrored =
						(Section.SurfaceFlags & WoTRuntimeMirrorPolyFlag) != 0;
					const bool bEligible =
						WoTRuntimeMirrorSectionEligible(Section.SurfaceFlags);
					TestTrue(MirrorPrefix + Section.MaterialSlot
						+ TEXT(" is a reproduced translucent mirror"), bEligible == bMirrored);
					Flags.Add(bEligible);
					if (!bEligible) continue;
					++MirrorSections;
					MirrorSectionTriangles += Section.IndexCount / 3;
				}
			}
			TArray<FWoTRuntimeMirrorPlane> MirrorPlanes;
			WoTRuntimeMirrorCollectPlanes(MirrorPayloads, MirrorFlags, MirrorPlanes);
			TArray<FString> ActualPlanes;
			uint32 PlaneTriangles = 0;
			for (const FWoTRuntimeMirrorPlane& Plane : MirrorPlanes)
			{
				ActualPlanes.Add(FString::Printf(TEXT("%.0f,%.0f,%.0f@%.0f"),
					Plane.Normal.X + 0.0f, Plane.Normal.Y + 0.0f,
					Plane.Normal.Z + 0.0f, Plane.Distance + 0.0f));
				PlaneTriangles += static_cast<uint32>(Plane.Triangles.Num());
			}
			ActualPlanes.Sort();
			const FExpectedMirrors* ExpectedMirror = nullptr;
			for (const FExpectedMirrors& Row : ExpectedMirrors)
				if (Descriptor.MapIdentity == Row.Map) ExpectedMirror = &Row;
			TestEqual(MirrorPrefix + TEXT("planes"), FString::Join(ActualPlanes, TEXT("|")),
				FString(ExpectedMirror != nullptr ? ExpectedMirror->Planes : TEXT("")));
			TestEqual(MirrorPrefix + TEXT("sections"), MirrorSections,
				ExpectedMirror != nullptr ? ExpectedMirror->Sections : 0u);
			TestEqual(MirrorPrefix + TEXT("every mirrored triangle has a plane"),
				PlaneTriangles, MirrorSectionTriangles);
			uint32 MirrorNodes = 0;
			uint32 UnmatchedNodes = 0;
			for (const FWoTRuntimeWorldNode& Node : Resource.Nodes())
			{
				if (Node.VertexCount < 3 || !Resource.Surfaces().IsValidIndex(Node.Surface))
					continue;
				const uint32 SurfaceFlags = Resource.Surfaces()[Node.Surface].Flags;
				if ((SurfaceFlags & WoTRuntimeMirrorPolyFlag) == 0
					|| (SurfaceFlags & 0x00000001u) != 0) continue;
				++MirrorNodes;
				const FVector3f NodeNormal(Node.Plane[0], Node.Plane[1], Node.Plane[2]);
				const float NodeDistance = Node.Plane[3];
				const bool bMatched = MirrorPlanes.ContainsByPredicate(
					[&NodeNormal, NodeDistance](const FWoTRuntimeMirrorPlane& Plane)
					{
						const float Dot = FVector3f::DotProduct(Plane.Normal, NodeNormal);
						return (Dot >= 0.9999f
								&& FMath::Abs(Plane.Distance - NodeDistance) <= 0.5f)
							|| (Dot <= -0.9999f
								&& FMath::Abs(Plane.Distance + NodeDistance) <= 0.5f);
					});
				UnmatchedNodes += bMatched ? 0u : 1u;
			}
			TestEqual(MirrorPrefix + TEXT("drawn mirrored world nodes"), MirrorNodes,
				ExpectedMirror != nullptr ? ExpectedMirror->Nodes : 0u);
			TestEqual(MirrorPrefix + TEXT("every mirrored node lies on a staged plane"),
				UnmatchedNodes, 0u);
			MirrorPlaneTotal += static_cast<uint32>(MirrorPlanes.Num());
		}
	}
	TestEqual(TEXT("all nineteen maps were checked"), CheckedMaps, 19u);
	TestEqual(TEXT("sixteen maps have a sky"), SkyMaps, 16u);
	TestEqual(TEXT("three portal-unsealed sky zones (A04, A07, B3_02)"), ResidualMaps, 3u);
	TestEqual(TEXT("their sky-rest content is drawn (no residual left)"), RestMaps, 3u);
	TestEqual(TEXT("thirty-four panned slots"), PannedSlots, 34u);
	TestEqual(TEXT("five mirror planes on three maps (B2_01, B2_04, B3_01)"),
		MirrorPlaneTotal, 5u);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPRealRetailMaterializationSpec,
	"WoT.World.RuntimeBSP.RealRetailProductionMaterialization",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPRealRetailMaterializationSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: production census not verified"));
		return true;
	}
	const FString RetailRoot = WoTRuntimeProjectionRepoPath(TEXT("original"));
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
	{
		AddError(LoadedContract.GetError().ToString());
		return false;
	}
	static const TCHAR* SelectedMaps[] = {
		TEXT("Maps/Arena_01.wot"), TEXT("Maps/Battle2_02.wot"),
		TEXT("Maps/Battle2_05.wot"), TEXT("Maps/Battle3_02.wot"),
		TEXT("Maps/Battle4_01.wot")};
	auto IsSelected = [](FStringView SourcePath)
	{
		for (const TCHAR* Selected : SelectedMaps)
			if (SourcePath.Equals(Selected, ESearchCase::IgnoreCase)) return true;
		return false;
	};
	FWoTRetailContract Contract;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::World
			&& Fact.Domain != EWoTFactDomain::BSP)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"),
				ESearchCase::IgnoreCase))
			continue;
		Contract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(Fact.SourcePath + TEXT(" production package loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath))))
			return false;
	}
	if (!TestEqual(TEXT("complete production multiplayer package set"),
		Sources.Num(), 19))
		return false;
	FWoTSkyCensusWriter Writer;
	FWoTRuntimeProjectionCensusProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportRequiredWorldSetPackageBytesForTesting(Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("selected production BSP/world payloads import"),
		Imported.IsError()))
	{
		AddError(Imported.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("complete production multiplayer descriptor set"),
		Imported.GetValue().Worlds.Num(), 19);
	uint32 MaterializedMaps = 0;
	for (const FWoTWorldDescriptor& Descriptor : Imported.GetValue().Worlds)
	{
		const FString SourcePath = TEXT("Maps/") + Descriptor.MapIdentity + TEXT(".wot");
		if (!IsSelected(SourcePath)) continue;
		TArray<uint8>* WorldBytes = Writer.Objects.Find(Descriptor.WorldLogicalId);
		if (!TestNotNull(Descriptor.MapIdentity + TEXT(" world payload"), WorldBytes))
			continue;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldBytes),
				TEXT("ue1-world-records-le-v6"), TEXT("world"));
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" world frame decodes"),
			DecodedWorld.IsError())) continue;
		FWoTRuntimeWorldResource Resource;
		FWoTBootstrapStatus Loaded =
			Resource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue());
		if (!TestFalse(Descriptor.MapIdentity + TEXT(" world payload decodes"),
			Loaded.IsError())) continue;
		if (Descriptor.MapIdentity == TEXT("Battle2_02"))
		{
			TestEqual(TEXT("front end resolves advertised title from authenticated production world"),
				FWoTFrontEndMapCatalog::ResolveRetailTitleFromAuthenticatedWorld(
					Resource, Descriptor.MapIdentity),
				FString(TEXT("Sands of Time")));
		}

		TArray<FWoTBSPPayloadData> Parts;
		bool bPartsDecoded = true;
		for (const FWoTBSPPartDescriptor& PartDescriptor : Descriptor.BSPParts)
		{
			TArray<uint8>* PartBytes = Writer.Objects.Find(PartDescriptor.LogicalId);
			if (!TestNotNull(PartDescriptor.PartIdentity + TEXT(" production BSP payload"),
				PartBytes))
			{
				bPartsDecoded = false;
				break;
			}
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedPart =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*PartBytes),
					TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
			FString PartError;
			FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
			bPartsDecoded = !DecodedPart.IsError()
				&& UWoTBSPPayloadLibrary::DecodePayload(
					DecodedPart.GetValue().Payload(), Part, PartError);
			if (!bPartsDecoded)
			{
				AddError(Descriptor.MapIdentity + TEXT(" BSP decode: ")
					+ (DecodedPart.IsError() ? DecodedPart.GetError().ToString() : PartError));
				break;
			}
			TestTrue(PartDescriptor.PartIdentity + TEXT(" has rendered triangles"),
				Part.Indices.Num() >= 3);
			TestEqual(PartDescriptor.PartIdentity
				+ TEXT(" has one production v9 surface provenance value per triangle"),
				Part.TriangleSurfaces.Num(), Part.Indices.Num() / 3);
			for (uint32 Surface : Part.TriangleSurfaces)
			{
				TestTrue(PartDescriptor.PartIdentity
					+ TEXT(" surface provenance is in the world table"),
					Resource.Surfaces().IsValidIndex(static_cast<int32>(Surface)));
			}
		}
		if (!bPartsDecoded) continue;
		TArray<uint32> SurfaceFlags;
		SurfaceFlags.Reserve(Resource.Surfaces().Num());
		for (const FWoTRuntimeWorldSurface& Surface : Resource.Surfaces())
			SurfaceFlags.Add(Surface.Flags);
		FWoTRetailSkyWorldView SkyView;
		SkyView.Actors = Resource.Actors();
		SkyView.Zones = Resource.Zones();
		SkyView.Nodes = Resource.Nodes();
		SkyView.Surfaces = Resource.Surfaces();
		TArray<FWoTRuntimeMapSkyPlan> ExpectedSkyPlans;
		TArray<int32> ExpectedZonePlans;
		FString SkyPlanError;
		TestTrue(Descriptor.MapIdentity + TEXT(" production sky plans build"),
			WoTBuildRuntimeMapSkyPlans(SkyView, Parts, ExpectedSkyPlans,
				ExpectedZonePlans, SkyPlanError));
		if (!SkyPlanError.IsEmpty()) AddError(SkyPlanError);
		WoTTest::FSpecWorldOptions WorldOptions;
		WorldOptions.bStartPlay = false;
		UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
		AWoTRuntimeBSPWorld* RuntimeBSP = nullptr;
		uint32 FallbackParts = 0;
		FString Error;
		FString SkyError;
		const bool bBuilt = WoTWorldBuildRuntimeBSPForTesting(*World, Descriptor,
			Parts, RuntimeBSP, FallbackParts, Error, {}, nullptr, nullptr,
			&SkyView, &SkyError, EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags);
		TestTrue(Descriptor.MapIdentity + TEXT(" production BSP materializes"), bBuilt);
		if (!bBuilt) AddError(Descriptor.MapIdentity + TEXT(": ") + Error);
		TestTrue(Descriptor.MapIdentity + TEXT(" sky stage succeeds"), SkyError.IsEmpty());
		if (TestNotNull(Descriptor.MapIdentity + TEXT(" runtime BSP actor"), RuntimeBSP))
		{
			// The production builder intentionally returns a deferred, completely
			// unregistered graph for MaterializeBeforePlay. Complete the same
			// boundary, then initialize play so the engine registers cooked BSP
			// collision in the production order.
			RuntimeBSP->FinishSpawning(FTransform::Identity, true);
			for (AWoTSkyBox* SkyBox : RuntimeBSP->SkyBoxes)
				if (SkyBox != nullptr)
					SkyBox->FinishSpawning(FTransform::Identity, true);
			// CreateSpecWorld has already run InitWorld, while the production BSP
			// builder intentionally returns an unregistered pre-InitWorld graph.
			// Mirror InitWorld's component-update boundary explicitly so this
			// already-initialized fixture cannot depend on incidental registration.
			World->UpdateWorldComponents(false, false);
			World->GetWorldSettings()->DefaultGameMode = AWoTGameMode::StaticClass();
			FURL URL;
			World->SetGameMode(URL);
			World->InitializeActorsForPlay(URL);
			World->BeginPlay();
			TestEqual(Descriptor.MapIdentity + TEXT(" geometry-part count"),
				RuntimeBSP->GeometryComponents.Num(), Parts.Num());
			++MaterializedMaps;

			if (Descriptor.MapIdentity == TEXT("Battle2_05"))
			{
				TestEqual(TEXT("Battle2_05 has one cube capture per linked sky plan"),
					RuntimeBSP->SkyBoxes.Num(), ExpectedSkyPlans.Num());
				TestEqual(TEXT("Battle2_05 retains its three child scenes"),
					RuntimeBSP->SkyBoxes.Num(), 3);
				TSet<UTextureRenderTargetCube*> PlanTargets;
				for (int32 PlanIndex = 0;
					PlanIndex < RuntimeBSP->SkyBoxes.Num(); ++PlanIndex)
				{
					AWoTSkyBox* SkyBox = RuntimeBSP->SkyBoxes[PlanIndex];
					UStaticMeshComponent* SkyMesh = SkyBox != nullptr
						? SkyBox->GetStaticMeshComponent() : nullptr;
					USceneCaptureComponentCube* Capture = SkyBox != nullptr
						? SkyBox->GetSkyCapture() : nullptr;
					UTextureRenderTargetCube* Target = SkyBox != nullptr
						? SkyBox->GetSkyCaptureTarget() : nullptr;
					TestTrue(FString::Printf(TEXT("Battle2_05 plan %d sky is capture-only"),
						PlanIndex), SkyMesh != nullptr
						&& SkyMesh->bVisibleInSceneCaptureOnly);
					TestTrue(FString::Printf(TEXT("Battle2_05 plan %d capture is isolated"),
						PlanIndex), Capture != nullptr && Target != nullptr
						&& Capture->TextureTarget == Target
						&& Capture->PrimitiveRenderMode
							== ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList
						&& Capture->ShowOnlyComponents.Num() == 1 + SkyBox->GetSkyLayers().Num()
						&& Capture->ShowOnlyComponents[0].Get() == SkyMesh);
					for (UStaticMeshComponent* Layer : SkyBox != nullptr
						? SkyBox->GetSkyLayers() : TArray<TObjectPtr<UStaticMeshComponent>>())
					{
						TestTrue(FString::Printf(TEXT("Battle2_05 plan %d layer is captured"),
							PlanIndex), Layer != nullptr && Layer->bVisibleInSceneCaptureOnly
							&& Capture != nullptr && Capture->ShowOnlyComponents.Contains(Layer));
					}
					if (Target != nullptr) PlanTargets.Add(Target);
				}
				TestEqual(TEXT("Battle2_05 cube targets are plan-private"),
					PlanTargets.Num(), RuntimeBSP->SkyBoxes.Num());

				int32 RenderedWindowMaterials = 0;
				for (UStaticMeshComponent* Component : RuntimeBSP->GeometryComponents)
					for (int32 Slot = 0; Component != nullptr
						&& Slot < Component->GetNumMaterials(); ++Slot)
					{
						UMaterialInstanceDynamic* MID =
							Cast<UMaterialInstanceDynamic>(Component->GetMaterial(Slot));
						if (MID == nullptr || MID->Parent == nullptr
							|| MID->Parent->GetName() != TEXT("M_WoT_SkyWindow")) continue;
						++RenderedWindowMaterials;
						UTexture* Cube = nullptr;
						MID->GetTextureParameterValue(FHashedMaterialParameterInfo(
							WoTRuntimeSkyWindowCubeParameter), Cube);
						TestTrue(TEXT("Battle2_05 rendered window binds a live plan cube"),
							PlanTargets.Contains(Cast<UTextureRenderTargetCube>(Cube)));
					}
				TestTrue(TEXT("Battle2_05 FakeBackdrop sections remain in world render meshes"),
					RenderedWindowMaterials > 0
					&& RenderedWindowMaterials == RuntimeBSP->SkyWindowMaterials.Num());

				const FVector LeakView(11072.0, 15832.0, -24513.0);
				RuntimeBSP->ApplySkyForViewPoint(LeakView);
				const int32 ViewZone = WoTRuntimeMapPointZone(
					Resource.Nodes(), LeakView);
				const int32 ExpectedPlan = ExpectedZonePlans.IsValidIndex(ViewZone)
					&& ExpectedSkyPlans.IsValidIndex(ExpectedZonePlans[ViewZone])
					? ExpectedZonePlans[ViewZone] : 0;
				TestEqual(TEXT("Battle2_05 view zone selects its linked sky plan"),
					RuntimeBSP->ActiveSkyPlan, ExpectedPlan);
				UTexture* SelectedCube = nullptr;
				if (!RuntimeBSP->SkyWindowMaterials.IsEmpty())
					RuntimeBSP->SkyWindowMaterials[0]->GetTextureParameterValue(
						FHashedMaterialParameterInfo(WoTRuntimeSkyWindowCubeParameter),
						SelectedCube);
				TestEqual(TEXT("Battle2_05 window binding follows the view-zone plan"),
					SelectedCube, static_cast<UTexture*>(RuntimeBSP->SkyBoxes[ExpectedPlan]
						->GetSkyCaptureTarget()));
				// One live cube per frame: only the bound plan keeps capturing.
				for (int32 PlanIndex = 0;
					PlanIndex < RuntimeBSP->SkyBoxes.Num(); ++PlanIndex)
				{
					const USceneCaptureComponentCube* PlanCapture =
						RuntimeBSP->SkyBoxes[PlanIndex] != nullptr
						? RuntimeBSP->SkyBoxes[PlanIndex]->GetSkyCapture() : nullptr;
					TestEqual(FString::Printf(
						TEXT("Battle2_05 plan %d captures only while bound"), PlanIndex),
						PlanCapture != nullptr && PlanCapture->bCaptureEveryFrame,
						PlanIndex == ExpectedPlan);
				}

				struct FLeakRay { uint32 Surface; FVector Direction; };
				static const FLeakRay LeakRays[] = {
					{145u, FVector(-0.572413675806, -0.429491441035, 0.698483847936)},
					{143u, FVector(-0.521504062480, -0.458377976689, 0.719668773328)},
					{3894u, FVector(-0.463089386653, -0.462521488158, 0.756056937645)},
				};
				for (const FLeakRay& Ray : LeakRays)
				{
					const FVector End = LeakView + Ray.Direction * 40000.0;
					double ExpectedDistance = TNumericLimits<double>::Max();
					uint32 FirstSurface = MAX_uint32;
					for (const FWoTBSPPayloadData& Part : Parts)
						for (int32 Triangle = 0;
							Triangle < Part.TriangleSurfaces.Num(); ++Triangle)
						{
							const uint32 Surface = Part.TriangleSurfaces[Triangle];
							if (!Resource.Surfaces().IsValidIndex(Surface)
								|| (Resource.Surfaces()[Surface].Flags & 0x00000008u) != 0)
								continue; // PF_NotSolid is absent from the collision cook.
							const int32 At = Triangle * 3;
							if (!Part.Indices.IsValidIndex(At + 2)) continue;
							const uint32 IA = Part.Indices[At];
							const uint32 IB = Part.Indices[At + 1];
							const uint32 IC = Part.Indices[At + 2];
							if (!Part.Positions.IsValidIndex(IA)
								|| !Part.Positions.IsValidIndex(IB)
								|| !Part.Positions.IsValidIndex(IC)) continue;
							FVector Point, Normal;
							if (!FMath::SegmentTriangleIntersection(LeakView, End,
								FVector(Part.Positions[IA]), FVector(Part.Positions[IB]),
								FVector(Part.Positions[IC]), Point, Normal)) continue;
							const double Distance = FVector::Distance(LeakView, Point);
							if (Distance < ExpectedDistance)
							{
								ExpectedDistance = Distance;
								FirstSurface = Surface;
							}
						}
					TestEqual(FString::Printf(TEXT("leak ray first decoded surface is window %u"),
						Ray.Surface), FirstSurface, Ray.Surface);
					TestTrue(TEXT("decoded first surface is PF_FakeBackdrop"),
						Resource.Surfaces().IsValidIndex(FirstSurface)
						&& (Resource.Surfaces()[FirstSurface].Flags
							& WoTRuntimeMapPFFakeBackdrop) != 0);
					FHitResult Hit;
					FCollisionQueryParams Query(
						SCENE_QUERY_STAT(RuntimeBSPSkyWindowLeakRegression), true);
					const bool bHit = World->LineTraceSingleByChannel(
						Hit, LeakView, End, ECC_Visibility, Query);
					TestTrue(FString::Printf(TEXT("leak ray %u hits the runtime window"),
						Ray.Surface), bHit && Hit.GetActor() == RuntimeBSP
						&& FMath::Abs(Hit.Distance - ExpectedDistance) <= 2.0);
				}
			}

			if (Descriptor.MapIdentity == TEXT("Arena_01"))
			{
				struct FCollisionWitnessTriangle
				{
					FVector Center = FVector::ZeroVector;
					FVector Normal = FVector::ZeroVector;
					uint32 Surface = 0;
					uint32 Flags = 0;
				};
				TArray<FCollisionWitnessTriangle> Triangles;
				for (const FWoTBSPPayloadData& Part : Parts)
				{
					for (int32 Triangle = 0;
						Triangle < Part.TriangleSurfaces.Num(); ++Triangle)
					{
						const int32 FirstIndex = Triangle * 3;
						if (!Part.Indices.IsValidIndex(FirstIndex + 2)) continue;
						const uint32 IA = Part.Indices[FirstIndex];
						const uint32 IB = Part.Indices[FirstIndex + 1];
						const uint32 IC = Part.Indices[FirstIndex + 2];
						if (!Part.Positions.IsValidIndex(static_cast<int32>(IA))
							|| !Part.Positions.IsValidIndex(static_cast<int32>(IB))
							|| !Part.Positions.IsValidIndex(static_cast<int32>(IC))) continue;
						const uint32 Surface = Part.TriangleSurfaces[Triangle];
						if (!Resource.Surfaces().IsValidIndex(
							static_cast<int32>(Surface))) continue;
						const FVector A(Part.Positions[static_cast<int32>(IA)]);
						const FVector B(Part.Positions[static_cast<int32>(IB)]);
						const FVector C(Part.Positions[static_cast<int32>(IC)]);
						const FVector Normal = FVector::CrossProduct(B - A, C - A)
							.GetSafeNormal();
						if (Normal.IsNearlyZero()) continue;
						Triangles.Add({(A + B + C) / 3.0, Normal, Surface,
							Resource.Surfaces()[static_cast<int32>(Surface)].Flags});
					}
				}
				auto Probe = [World, RuntimeBSP](
					const FCollisionWitnessTriangle& Triangle,
					bool& bVisBlocks, bool& bPawnBlocks)
				{
					const FVector Start = Triangle.Center + Triangle.Normal * 96.0;
					const FVector End = Triangle.Center - Triangle.Normal * 96.0;
					FHitResult VisHit;
					FCollisionQueryParams VisQuery(
						SCENE_QUERY_STAT(RuntimeBSPRealNotSolidVis), true);
					bVisBlocks = World->LineTraceSingleByChannel(
						VisHit, Start, End, ECC_Visibility, VisQuery);
					FHitResult PawnHit;
					FCollisionQueryParams PawnQuery(
						SCENE_QUERY_STAT(RuntimeBSPRealNotSolidPawn), true);
					bPawnBlocks = World->SweepSingleByChannel(
						PawnHit, Start, End, FQuat::Identity, ECC_Pawn,
						FCollisionShape::MakeCapsule(17.0f, 34.0f), PawnQuery);
					if (bVisBlocks)
					{
						bVisBlocks = VisHit.GetActor() == RuntimeBSP
							&& FVector::Distance(VisHit.ImpactPoint,
								Triangle.Center) <= 2.0;
					}
					if (bPawnBlocks) bPawnBlocks = PawnHit.GetActor() == RuntimeBSP;
				};
				const FVector ExpectedNotSolidCenter(-2904.0, 4530.6666667, -330.6666667);
				const FVector ExpectedSolidCenter(-4059.475, 3932.665, -512.0);
				const FCollisionWitnessTriangle* NotSolidWitness = nullptr;
				for (const FCollisionWitnessTriangle& Triangle : Triangles)
				{
					if (Triangle.Surface != 1619u
						|| !Triangle.Center.Equals(ExpectedNotSolidCenter, 0.01)) continue;
					bool bVisBlocks = false;
					bool bPawnBlocks = false;
					Probe(Triangle, bVisBlocks, bPawnBlocks);
					if (!bVisBlocks && !bPawnBlocks)
					{
						NotSolidWitness = &Triangle;
						break;
					}
				}
				TestNotNull(TEXT("Arena_01 has a real PF_NotSolid sheet that both queries pass"),
					NotSolidWitness);
				const FCollisionWitnessTriangle* SolidWitness = nullptr;
				double SolidDistance = TNumericLimits<double>::Max();
				if (NotSolidWitness != nullptr)
				{
					for (const FCollisionWitnessTriangle& Triangle : Triangles)
					{
						if (Triangle.Surface != 167u
							|| !Triangle.Center.Equals(ExpectedSolidCenter, 0.01)) continue;
						const double Distance = FVector::Distance(
							Triangle.Center, NotSolidWitness->Center);
						bool bVisBlocks = false;
						bool bPawnBlocks = false;
						Probe(Triangle, bVisBlocks, bPawnBlocks);
						if (!bVisBlocks || !bPawnBlocks) continue;
						SolidWitness = &Triangle;
						SolidDistance = Distance;
						break;
					}
				}
				TestNotNull(TEXT("Arena_01 has a nearby real solid translucent/semisolid sheet that both queries block"),
					SolidWitness);
				if (NotSolidWitness != nullptr)
				{
					TestTrue(TEXT("Arena_01 surface 1619 is PF_NotSolid"),
						(NotSolidWitness->Flags & 0x00000008u) != 0);
					TestEqual(TEXT("Arena_01 PF_NotSolid witness texture"),
						Resource.Surfaces()[1619].TextureIdentity,
						FString(TEXT("MordethT.DetailArchitect.MetlGril4000v01")));
				}
				if (SolidWitness != nullptr)
				{
					TestTrue(TEXT("Arena_01 surface 167 remains semisolid and not NotSolid"),
						(SolidWitness->Flags & 0x00000020u) != 0
						&& (SolidWitness->Flags & 0x00000008u) == 0);
					TestEqual(TEXT("Arena_01 solid neighbor texture"),
						Resource.Surfaces()[167].TextureIdentity,
						FString(TEXT("MordethT.SurfaceFloor.TileFlor4001")));
					TestEqual(TEXT("Arena_01 collision witness separation"),
						SolidDistance, 1313.625, 0.01);
				}
				auto ReportWitness = [this, &Resource](
					const TCHAR* Kind, const FCollisionWitnessTriangle* Triangle,
					double Distance)
				{
					if (Triangle == nullptr) return;
					const FWoTRuntimeWorldSurface& Surface =
						Resource.Surfaces()[static_cast<int32>(Triangle->Surface)];
					AddInfo(FString::Printf(
						TEXT("Arena_01 %s surface=%u texture=%s flags=0x%08x center=(%.3f,%.3f,%.3f) normal=(%.6f,%.6f,%.6f) distance=%.3f"),
						Kind, Triangle->Surface, *Surface.TextureIdentity,
						Triangle->Flags, Triangle->Center.X, Triangle->Center.Y,
						Triangle->Center.Z, Triangle->Normal.X, Triangle->Normal.Y,
						Triangle->Normal.Z, Distance));
				};
				ReportWitness(TEXT("PF_NotSolid witness"), NotSolidWitness, 0.0);
				ReportWitness(TEXT("solid neighbor"), SolidWitness, SolidDistance);
			}
		}

		const double TargetFloorOffset =
			Descriptor.MapIdentity == TEXT("Battle2_02") ? 14.0
			: Descriptor.MapIdentity == TEXT("Battle4_01") ? 83.0
			: Descriptor.MapIdentity == TEXT("Arena_01") ? 40.0 : -1.0;
		if (bBuilt && TargetFloorOffset > 0.0f)
		{
			FVector AuthoredStart = FVector::ZeroVector;
			double FloorZ = 0.0;
			double BestError = TNumericLimits<double>::Max();
			int32 CandidateCount = 0;
			int32 FloorHitCount = 0;
			for (const FWoTRuntimeWorldActor& Record : Resource.Actors())
			{
				if (!Record.ClassIdentity.Contains(TEXT("PlayerStart"),
						ESearchCase::IgnoreCase)
					&& !Record.Identity.Contains(TEXT("PlayerStart"),
						ESearchCase::IgnoreCase))
				{
					continue;
				}
				++CandidateCount;
				const FWoTRuntimeWorldProperty* Location =
					WoTRuntimeWorldFindProperty(Record, TEXT("Location"));
				FVector Candidate = FVector::ZeroVector;
				if (!Location || !WoTRuntimeWorldPropertyVector(*Location, Candidate))
				{
					continue;
				}
				FVector FloorHit = FVector::ZeroVector;
				FString TraceError;
				if (WoTTraceEntryGround(Resource.Nodes(), Resource.BSPRootOutside(),
					Candidate + FVector(0, 0, 2),
					Candidate - FVector(0, 0, 256), FloorHit, TraceError)
					!= EWoTEntryTraceResult::Hit)
				{
					continue;
				}
				++FloorHitCount;
				const double ObservedOffset = Candidate.Z - FloorHit.Z;
				const double OffsetError = FMath::Abs(
					ObservedOffset - TargetFloorOffset);
				if (OffsetError < BestError)
				{
					BestError = OffsetError;
					AuthoredStart = Candidate;
					FloorZ = FloorHit.Z;
				}
			}
			TestTrue(Descriptor.MapIdentity + FString::Printf(
				TEXT(" contains the audited production floor-offset witness (candidates %d, floor hits %d, best error %.3f)"),
				CandidateCount, FloorHitCount, BestError),
				BestError <= 2.0f);

			if (Descriptor.MapIdentity == TEXT("Arena_01"))
			{
				FHitResult RuntimeWallHit;
				bool bHitRuntimeWall = false;
				const FVector TraceStart = AuthoredStart + FVector(0, 0, 128);
				const FVector TraceEnd = AuthoredStart - FVector(0, 0, 512);
				double BestDistanceSquared = TNumericLimits<double>::Max();
				// Validate the synchronous cook, then find the collision witness in the
				// exact decoded triangle stream used to build it. A Chaos component trace
				// here is not stable after the editor's thousand preceding transient worlds;
				// the dedicated RuntimeBSP.RetailSolidityCollision spec owns that integration
				// boundary. This production fixture owns authenticated geometry/ownership.
				for (int32 PartIndex = 0;
					PartIndex < Parts.Num()
						&& RuntimeBSP->GeometryComponents.IsValidIndex(PartIndex);
					++PartIndex)
				{
					const FWoTBSPPayloadData& Part = Parts[PartIndex];
					UStaticMeshComponent* Component =
						RuntimeBSP->GeometryComponents[PartIndex];
					UStaticMesh* Mesh = Component != nullptr
						? Component->GetStaticMesh() : nullptr;
					UBodySetup* Body = Mesh != nullptr ? Mesh->GetBodySetup() : nullptr;
					if (Part.bPortal || Component == nullptr || Body == nullptr) continue;
					TestTrue(TEXT("Arena_01 production collision is synchronously cooked"),
						Body->bCreatedPhysicsMeshes && !Body->bFailedToCreatePhysicsMeshes);
					for (int32 Index = 0; Index + 2 < Part.Indices.Num(); Index += 3)
					{
						const uint32 IA = Part.Indices[Index];
						const uint32 IB = Part.Indices[Index + 1];
						const uint32 IC = Part.Indices[Index + 2];
						if (!Part.Positions.IsValidIndex(static_cast<int32>(IA))
							|| !Part.Positions.IsValidIndex(static_cast<int32>(IB))
							|| !Part.Positions.IsValidIndex(static_cast<int32>(IC))) continue;
						const FTransform& Transform = Component->GetComponentTransform();
						const FVector A = Transform.TransformPosition(FVector(Part.Positions[IA]));
						const FVector B = Transform.TransformPosition(FVector(Part.Positions[IB]));
						const FVector C = Transform.TransformPosition(FVector(Part.Positions[IC]));
						FVector HitPoint;
						FVector HitNormal;
						if (!FMath::SegmentTriangleIntersection(
							TraceStart, TraceEnd, A, B, C, HitPoint, HitNormal)) continue;
						const double DistanceSquared = FVector::DistSquared(TraceStart, HitPoint);
						if (DistanceSquared >= BestDistanceSquared) continue;
						BestDistanceSquared = DistanceSquared;
						if (FVector::DotProduct(HitNormal, TraceEnd - TraceStart) > 0.0)
							HitNormal *= -1.0;
						RuntimeWallHit = FHitResult(RuntimeBSP, Component, HitPoint, HitNormal);
						RuntimeWallHit.TraceStart = TraceStart;
						RuntimeWallHit.TraceEnd = TraceEnd;
						RuntimeWallHit.FaceIndex = Index / 3;
						bHitRuntimeWall = true;
					}
				}
				TestTrue(TEXT("Arena_01 production collision supplies an owned hit component"),
					bHitRuntimeWall && RuntimeWallHit.GetActor() == RuntimeBSP
					&& RuntimeWallHit.GetComponent() != nullptr);
				if (bHitRuntimeWall && RuntimeWallHit.GetComponent())
				{
					// The retail-builder specs intentionally reset their process-global decal
					// registry. DecalStyles owns registry-backed spawning; this late production
					// fixture owns the BSP component's Level/world-static classification.
					TestTrue(TEXT("fireball classifies materialized Arena_01 BSP as scorchable"),
						AWoTFireballProjectile::ShouldSpawnScorchForImpact(
							RuntimeWallHit.GetActor(), RuntimeWallHit.GetComponent()));
					AWoTCharacter* PawnImpact = World->SpawnActor<AWoTCharacter>(
						AuthoredStart + FVector(128, 0, 128), FRotator::ZeroRotator);
					if (TestNotNull(TEXT("production pawn impact spawns"), PawnImpact))
					{
						TestFalse(TEXT("fireball does not classify a pawn as scorchable"),
							AWoTFireballProjectile::ShouldSpawnScorchForImpact(
								PawnImpact, PawnImpact->GetCapsuleComponent()));
					}
				}
			}

			// Exercise spawning against a deterministic collision patch at the
			// authenticated retail BSP boundary. The runtime BSP materialization is
			// independently asserted above; this local patch isolates FindSpot and
			// falling from Chaos's late-created complex-mesh scene-query variance.
			AWoTCollisionBlocker* FloorPatch =
				World->SpawnActorDeferred<AWoTCollisionBlocker>(
					AWoTCollisionBlocker::StaticClass(),
					FTransform(FVector(AuthoredStart.X, AuthoredStart.Y, FloorZ - 10.0)));
			if (FloorPatch)
			{
				FloorPatch->Configure(EWoTCollisionBlockerMode::All, 256.0f, 10.0f);
				FloorPatch->FinishSpawning(FTransform(
					FVector(AuthoredStart.X, AuthoredStart.Y, FloorZ - 10.0)));
			}
			TestNotNull(Descriptor.MapIdentity
				+ TEXT(" authenticated floor collision patch"), FloorPatch);

			AWoTGameMode* Mode = World->GetAuthGameMode<AWoTGameMode>();
			AWoTPlayerController* PC =
				World->SpawnActor<AWoTPlayerController>();
			if (PC) PC->SetAsLocalPlayerController();
			APlayerStart* Start = World->SpawnActor<AWoTPlayerStart>(
				AuthoredStart, FRotator::ZeroRotator);
			if (TestNotNull(Descriptor.MapIdentity + TEXT(" production game mode"), Mode)
				&& TestNotNull(Descriptor.MapIdentity + TEXT(" production start"), Start)
				&& TestNotNull(Descriptor.MapIdentity + TEXT(" production controller"), PC))
			{
				for (int32 Pass = 0; Pass < 2; ++Pass)
				{
					if (APawn* Old = PC->GetPawn())
					{
						PC->UnPossess();
						Old->Destroy();
					}
					Mode->RestartPlayerAtPlayerStart(PC, Start);
					AWoTCharacter* Pawn = Cast<AWoTCharacter>(PC->GetPawn());
					const FString Prefix = Descriptor.MapIdentity
						+ (Pass == 0 ? TEXT(" login") : TEXT(" restart"));
					if (!TestNotNull(Prefix + TEXT(" spawns"), Pawn)) continue;
					const double InitialZ = Pawn->GetActorLocation().Z;
					if (TargetFloorOffset < 46.0f + KINDA_SMALL_NUMBER)
					{
						TestEqual(Prefix + TEXT(" FindSpot seats flush"),
							InitialZ - FloorZ, 48.4, 1.0);
						WoTTest::TickWorld(World, 0.1f);
						TestFalse(Prefix + TEXT(" does not fall"),
							Pawn->GetCharacterMovement()->IsFalling());
					}
					else
					{
						TestEqual(Prefix + TEXT(" FindSpot leaves free high start authored"),
							InitialZ, AuthoredStart.Z, 0.1);
						Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
						WoTTest::TickWorld(World, 0.8f);
						TestTrue(Prefix + TEXT(" falls the retail-scale distance"),
							InitialZ - Pawn->GetActorLocation().Z
								>= TargetFloorOffset - 49.0
							&& InitialZ - Pawn->GetActorLocation().Z
								<= TargetFloorOffset - 45.5);
						TestFalse(Prefix + TEXT(" lands on production BSP"),
							Pawn->GetCharacterMovement()->IsFalling());
					}
				}
			}
		}
		WoTTest::DestroySpecWorld(World);
	}
	TestEqual(TEXT("five selected production BSP worlds materialized"),
		MaterializedMaps, 5u);
	return !HasAnyErrors();
}

// Every MP map's drawn BSP sections resolve their authenticated
// texture on real retail data - exact identities for names that collide inside
// their package (Battle3_01/4_01 AesSedaiT.Ornament.*) and FName-case source
// identity (the packaged sweep's Arena_06 Naturet / Battle2_02 WOTPawns /
// Battle2_05+4_01 WOTTraps+WOTPawns slots). The production contract's facts
// stand in for the authenticated objects (logical id + source export).
// Render-backed, real data: Arena_02's sky camera sits inside a flat Sky_340
// pyramid (translucent, panning, lit orange by the bSpecialLit Light72), with a
// translucent Sky_439 sheet overhead. Retail draws the pyramid last over the
// sheet, so its sepia covers the whole sky (exports/reference/shots/Arena_02).
// The former SceneColor sky material let the far sheet overwrite the pyramid
// above ~7 degrees, leaving blue clouds. Probes the sky components from the sky
// origin and writes Saved/SkyLayers/Arena_02_pitchNN.png for inspection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRealRetailPlayerStartsInSolidSpec,
	"WoT.World.RealRetailPlayerStartsInSolid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRealRetailPlayerStartsInSolidSpec::RunTest(const FString&)
{
	// The production Battle3_01 BSP (the leaf map ProjectLeafMap builds) puts
	// two authored PlayerStart centers in solid space, under a semisolid moss
	// floor; UE1 FindSpot rejects both, so the game mode never spawns there.
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: Battle3_01 starts not verified"));
		return true;
	}
	const FString RetailRoot = WoTRuntimeProjectionRepoPath(TEXT("original"));
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
		return false;
	FWoTRetailContract Contract;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::World && Fact.Domain != EWoTFactDomain::BSP)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase))
			continue;
		Contract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(Fact.SourcePath + TEXT(" production package loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath))))
			return false;
	}
	FWoTSkyCensusWriter Writer;
	FWoTRuntimeProjectionCensusProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportRequiredWorldSetPackageBytesForTesting(Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("production world payloads import"), Imported.IsError()))
		return false;
	const FWoTWorldDescriptor* Battle = Imported.GetValue().Worlds.FindByPredicate(
		[](const FWoTWorldDescriptor& Descriptor)
		{ return Descriptor.MapIdentity == TEXT("Battle3_01"); });
	if (!TestNotNull(TEXT("Battle3_01 descriptor"), Battle)) return false;
	TArray<uint8>* WorldBytes = Writer.Objects.Find(Battle->WorldLogicalId);
	if (!TestNotNull(TEXT("Battle3_01 world payload"), WorldBytes)) return false;
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldBytes),
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	FWoTRuntimeWorldResource Resource;
	if (!TestFalse(TEXT("Battle3_01 world frame decodes"), Decoded.IsError())
		|| !TestFalse(TEXT("Battle3_01 world decodes"),
			Resource.LoadObjectForTesting(MoveTemp(Decoded).GetValue()).IsError()))
		return false;

	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("spec world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTLeafMap* LeafMap = World->SpawnActor<AWoTLeafMap>();
	// The same flattening as ProjectLeafMap (WoTWorldProjector.cpp).
	for (const FWoTRuntimeWorldNode& Node : Resource.Nodes())
	{
		for (float Value : Node.Plane) LeafMap->NodePlanes.Add(Value);
		LeafMap->NodeChildren.Add(Node.Back);
		LeafMap->NodeChildren.Add(Node.Front);
		LeafMap->NodeLeaves.Add(Node.Leaves[0]);
		LeafMap->NodeLeaves.Add(Node.Leaves[1]);
		LeafMap->NodeCsg.Add(Node.VertexCount > 0 && (Node.Flags & (0x01 | 0x20)) == 0 ? 1 : 0);
	}
	LeafMap->bRootOutside = Resource.BSPRootOutside();
	LeafMap->NumLeaves = Resource.Leaves().Num();
	// Authored retail locations (exports/t3d/Battle3_01.t3d).
	TestTrue(TEXT("PlayerStart27 center is in solid"),
		LeafMap->IsPointInSolid(FVector(3397.042480, -2718.439453, -2673.310547)));
	TestTrue(TEXT("PlayerStart28 center is in solid"),
		LeafMap->IsPointInSolid(FVector(2622.763428, -3311.658447, -2767.524414)));
	TestFalse(TEXT("PlayerStart0 center is open"),
		LeafMap->IsPointInSolid(FVector(4209.993652, -419.387909, -2389.795654)));
	TestFalse(TEXT("PlayerStart26 center is open"),
		LeafMap->IsPointInSolid(FVector(13249.025391, -16634.816406, -19533.222656)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeSkyRealArena02LayersSpec,
	"WoT.Sky.RealArena02Layers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTRuntimeSkyRealArena02LayersSpec::RunTest(const FString&)
{
	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		AddInfo(TEXT("null/non-rendering RHI: real Arena_02 sky render skipped"));
		return true;
	}
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: Arena_02 sky render not verified"));
		return true;
	}
	const FString RetailRoot = WoTRuntimeProjectionRepoPath(TEXT("original"));
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
		return false;
	FWoTRetailContract Contract;
	TArray<FWoTWorldPackageBytesForTesting> Sources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		if ((Fact.Domain != EWoTFactDomain::World && Fact.Domain != EWoTFactDomain::BSP)
			|| Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase))
			continue;
		Contract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& Source = Sources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(Fact.SourcePath + TEXT(" production package loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath))))
			return false;
	}
	FWoTSkyCensusWriter Writer;
	FWoTRuntimeProjectionCensusProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportRequiredWorldSetPackageBytesForTesting(Sources, Contract, Writer, Progress);
	if (!TestFalse(TEXT("production world payloads import"), Imported.IsError()))
		return false;
	const FWoTWorldDescriptor* Arena02 = Imported.GetValue().Worlds.FindByPredicate(
		[](const FWoTWorldDescriptor& Descriptor)
		{ return Descriptor.MapIdentity == TEXT("Arena_02"); });
	if (!TestNotNull(TEXT("Arena_02 descriptor"), Arena02)) return false;
	TArray<uint8>* WorldBytes = Writer.Objects.Find(Arena02->WorldLogicalId);
	if (!TestNotNull(TEXT("Arena_02 world payload"), WorldBytes)) return false;
	TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
		FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldBytes),
			TEXT("ue1-world-records-le-v6"), TEXT("world"));
	if (!TestFalse(TEXT("Arena_02 world frame decodes"), DecodedWorld.IsError())) return false;
	FWoTRuntimeWorldResource Resource;
	if (!TestFalse(TEXT("Arena_02 world payload decodes"),
		Resource.LoadObjectForTesting(MoveTemp(DecodedWorld).GetValue()).IsError()))
		return false;
	TArray<FWoTBSPPayloadData> Parts;
	for (const FWoTBSPPartDescriptor& PartDescriptor : Arena02->BSPParts)
	{
		TArray<uint8>* PartBytes = Writer.Objects.Find(PartDescriptor.LogicalId);
		if (!TestNotNull(PartDescriptor.PartIdentity + TEXT(" BSP payload"), PartBytes))
			return false;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedPart =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*PartBytes),
				TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
		FString PartError;
		FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
		if (!TestTrue(PartDescriptor.PartIdentity + TEXT(" BSP decodes"),
			!DecodedPart.IsError() && UWoTBSPPayloadLibrary::DecodePayload(
				DecodedPart.GetValue().Payload(), Part, PartError)))
			return false;
	}
	TArray<uint32> SurfaceFlags;
	for (const FWoTRuntimeWorldSurface& Surface : Resource.Surfaces())
		SurfaceFlags.Add(Surface.Flags);
	FWoTRetailSkyWorldView SkyView;
	SkyView.Actors = Resource.Actors();
	SkyView.Zones = Resource.Zones();
	SkyView.Nodes = Resource.Nodes();
	SkyView.Surfaces = Resource.Surfaces();

	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bStartPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	if (!TestNotNull(TEXT("Arena_02 render world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTRuntimeBSPWorld* RuntimeBSP = nullptr;
	uint32 FallbackParts = 0;
	FString Error;
	FString SkyError;
	if (!TestTrue(TEXT("Arena_02 production BSP materializes"),
		WoTWorldBuildRuntimeBSPForTesting(*World, *Arena02, Parts, RuntimeBSP,
			FallbackParts, Error, {}, nullptr, nullptr, &SkyView, &SkyError,
			EWoTRuntimeSkyStageFault::None, nullptr, SurfaceFlags))
		|| !TestNotNull(TEXT("Arena_02 runtime BSP"), RuntimeBSP))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Arena_02 sky stage succeeds"), SkyError.IsEmpty());
	RuntimeBSP->FinishSpawning(FTransform::Identity, true);
	for (AWoTSkyBox* SkyBox : RuntimeBSP->SkyBoxes)
		if (SkyBox != nullptr) SkyBox->FinishSpawning(FTransform::Identity, true);
	World->UpdateWorldComponents(false, false);
	AWoTSkyBox* Sky = RuntimeBSP->SkyBox.Get();
	if (!TestNotNull(TEXT("Arena_02 sky box"), Sky)) return false;
	TestEqual(TEXT("Arena_02 draws its two translucent sky sheets as layers"),
		Sky->GetSkyLayers().Num(), 4);
	for (UMaterialInterface* Material : {
		LoadObject<UMaterialInterface>(nullptr, WoTRuntimeSkyModulateMasterPath),
		LoadObject<UMaterialInterface>(nullptr, WoTRuntimeSkyAddMasterPath)})
		if (Material != nullptr) Material->EnsureIsComplete();
	// No player controller: the sky box keeps its spawn transform, so the sky
	// origin is the world origin, as the camera lock would place it.
	Sky->SetActorTickEnabled(false);
	Sky->SetActorLocation(FVector::ZeroVector);
	WoTTest::TickWorld(World, 4);
	FlushRenderingCommands();

	constexpr int32 Size = 256;
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(
		World, TEXT("Arena02SkyProbe"), RF_Transient);
	Target->ClearColor = FLinearColor::Black;
	Target->bAutoGenerateMips = false;
	Target->InitCustomFormat(Size, Size, PF_FloatRGBA, true);
	Target->UpdateResourceImmediate(true);
	ASceneCapture2D* ProbeActor = World->SpawnActor<ASceneCapture2D>();
	USceneCaptureComponent2D* Probe = ProbeActor != nullptr
		? ProbeActor->GetCaptureComponent2D() : nullptr;
	if (!TestNotNull(TEXT("Arena_02 sky probe"), Probe)) return false;
	Probe->TextureTarget = Target;
	Probe->FOVAngle = 60.0f;
	Probe->CaptureSource = SCS_SceneColorHDR;
	Probe->bCaptureEveryFrame = false;
	Probe->bCaptureOnMovement = false;
	Probe->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Probe->ShowOnlyComponent(Sky->GetStaticMeshComponent());
	for (UStaticMeshComponent* Layer : Sky->GetSkyLayers()) Probe->ShowOnlyComponent(Layer);
	const FString OutDir = FPaths::ProjectSavedDir() / TEXT("SkyLayers");
	IFileManager::Get().MakeDirectory(*OutDir, true);
	TMap<int32, FLinearColor> Centers;
	for (const int32 Pitch : {0, 35, 70})
	{
		ProbeActor->SetActorRotation(FRotator(Pitch, -125.0, 0.0));
		World->UpdateWorldComponents(false, false);
		WoTTest::TickWorld(World, 1);
		FlushRenderingCommands();
		Probe->CaptureScene();
		FlushRenderingCommands();
		TArray<FLinearColor> Pixels;
		FTextureRenderTargetResource* Resource2D = Target->GameThread_GetRenderTargetResource();
		if (!TestTrue(FString::Printf(TEXT("pitch %d capture reads"), Pitch),
			Resource2D != nullptr && Resource2D->ReadLinearColorPixels(Pixels)
			&& Pixels.Num() == Size * Size)) continue;
		// Mean over the central quarter: robust to the panning texture.
		FLinearColor Mean(0, 0, 0, 0);
		int32 Count = 0;
		for (int32 Y = Size * 3 / 8; Y < Size * 5 / 8; ++Y)
			for (int32 X = Size * 3 / 8; X < Size * 5 / 8; ++X, ++Count)
				Mean += Pixels[Y * Size + X];
		Mean /= static_cast<float>(FMath::Max(Count, 1));
		Centers.Add(Pitch, Mean);
		AddInfo(FString::Printf(TEXT("Arena_02 sky pitch %d: mean RGB %.3f %.3f %.3f"),
			Pitch, Mean.R, Mean.G, Mean.B));
		TArray<FColor> Bytes;
		Bytes.Reserve(Pixels.Num());
		for (const FLinearColor& Pixel : Pixels)
			Bytes.Add(FColor(
				static_cast<uint8>(FMath::Clamp(Pixel.R, 0.0f, 1.0f) * 255.0f + 0.5f),
				static_cast<uint8>(FMath::Clamp(Pixel.G, 0.0f, 1.0f) * 255.0f + 0.5f),
				static_cast<uint8>(FMath::Clamp(Pixel.B, 0.0f, 1.0f) * 255.0f + 0.5f), 255));
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size, Size, Bytes, Png);
		FFileHelper::SaveArrayToFile(Png,
			*(OutDir / FString::Printf(TEXT("Arena_02_pitch%02d.png"), Pitch)));
	}
	// Discriminating check: at 70 degrees the far Sky_439 sheet alone (layer
	// rank 0) differs from the full stack, so the near pyramid composites over
	// it there instead of being overwritten.
	if (const FLinearColor* Full = Centers.Find(70))
	{
		Probe->ClearShowOnlyComponents();
		Probe->ShowOnlyComponent(Sky->GetStaticMeshComponent());
		const TArray<TObjectPtr<UStaticMeshComponent>>& Layers = Sky->GetSkyLayers();
		for (int32 Index = 0; Index < FMath::Min(2, Layers.Num()); ++Index)
			Probe->ShowOnlyComponent(Layers[Index]);
		WoTTest::TickWorld(World, 1);
		FlushRenderingCommands();
		Probe->CaptureScene();
		FlushRenderingCommands();
		TArray<FLinearColor> Pixels;
		FTextureRenderTargetResource* Resource2D = Target->GameThread_GetRenderTargetResource();
		if (Resource2D != nullptr && Resource2D->ReadLinearColorPixels(Pixels)
			&& Pixels.Num() == Size * Size)
		{
			FLinearColor Mean(0, 0, 0, 0);
			int32 Count = 0;
			for (int32 Y = Size * 3 / 8; Y < Size * 5 / 8; ++Y)
				for (int32 X = Size * 3 / 8; X < Size * 5 / 8; ++X, ++Count)
					Mean += Pixels[Y * Size + X];
			Mean /= static_cast<float>(FMath::Max(Count, 1));
			const float Delta = FMath::Max3(FMath::Abs(Full->R - Mean.R),
				FMath::Abs(Full->G - Mean.G), FMath::Abs(Full->B - Mean.B));
			AddInfo(FString::Printf(TEXT("pitch 70 far sheet only: %.3f %.3f %.3f (delta %.3f)"),
				Mean.R, Mean.G, Mean.B, Delta));
			TestTrue(TEXT("the near pyramid layer changes the high sky over the far sheet"),
				Delta > 0.02f);
		}
	}
	for (const int32 Pitch : {35, 70})
	{
		const FLinearColor* Center = Centers.Find(Pitch);
		TestTrue(FString::Printf(TEXT("the high sky at pitch %d is sepia like retail "
			"(R %.3f > B %.3f)"), Pitch, Center ? Center->R : 0.0f, Center ? Center->B : 0.0f),
			Center != nullptr && Center->R > Center->B);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPRealRetailTextureIdentitySpec,
	"WoT.World.RuntimeBSP.RealRetailTextureIdentity",
	WoTRuntimeMapProjectionFlags)

bool FWoTRuntimeBSPRealRetailTextureIdentitySpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: production census not verified"));
		return true;
	}
	const FString RetailRoot = WoTRuntimeProjectionRepoPath(TEXT("original"));
	if (!IFileManager::Get().FileExists(*(RetailRoot / TEXT("Maps/Arena_01.wot"))))
	{
		AddError(TEXT("required original/Maps/Arena_01.wot is missing"));
		return false;
	}
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
	{
		AddError(LoadedContract.GetError().ToString());
		return false;
	}
	TArray<FWoTRuntimeBSPTestConverterResource> Authenticated;
	TMap<FString, FString> SoundSources;
	FWoTRetailContract WorldContract;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		const bool bTexture = Fact.Domain == EWoTFactDomain::Texture;
		const bool bProcedural = Fact.Domain == EWoTFactDomain::Procedural;
		if ((bTexture || bProcedural || Fact.Domain == EWoTFactDomain::Sound)
			&& Fact.Outputs.Num() == 1)
		{
			const FString& LogicalId = Fact.Outputs[0].LogicalId;
			if (Fact.Domain == EWoTFactDomain::Sound)
			{
				SoundSources.Add(LogicalId, Fact.SourceExport);
				continue;
			}
			const FString Prefix = bProcedural
				? TEXT("cache/procedural/") : TEXT("cache/texture/");
			if (TestTrue(LogicalId + TEXT(" has its domain prefix"),
				LogicalId.StartsWith(Prefix, ESearchCase::CaseSensitive)))
			{
				Authenticated.Add({bProcedural, LogicalId.Mid(Prefix.Len()),
					Fact.SourceExport, true});
			}
			continue;
		}
		const bool bMultiplayerWorldFact =
			(Fact.Domain == EWoTFactDomain::BSP || Fact.Domain == EWoTFactDomain::World)
			&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
		if (!bMultiplayerWorldFact) continue;
		WorldContract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& Source = WorldSources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath))))
		{
			return false;
		}
	}
	// The colliding exports carry only their exact identities (no alias).
	auto HasBase = [&Authenticated](const TCHAR* Base)
	{
		return Authenticated.ContainsByPredicate(
			[Base](const FWoTRuntimeBSPTestConverterResource& Row)
			{ return Row.LogicalBase.Equals(Base, ESearchCase::CaseSensitive); });
	};
	TestTrue(TEXT("exact Ornament.PaprWall1441"), HasBase(TEXT("AesSedaiT.Ornament__PaprWall1441")));
	TestTrue(TEXT("exact SurfaceWall.PaprWall1441"), HasBase(TEXT("AesSedaiT.SurfaceWall__PaprWall1441")));
	TestTrue(TEXT("exact Ornament.OrnaTrim1505"), HasBase(TEXT("AesSedaiT.Ornament__OrnaTrim1505")));
	TestTrue(TEXT("exact DetailArchitect.OrnaTrim1505"),
		HasBase(TEXT("AesSedaiT.DetailArchitect__OrnaTrim1505")));
	TestFalse(TEXT("no plain PaprWall1441 alias"), HasBase(TEXT("AesSedaiT__PaprWall1441")));
	TestFalse(TEXT("no plain OrnaTrim1505 alias"), HasBase(TEXT("AesSedaiT__OrnaTrim1505")));
	TestTrue(TEXT("exact Nature.Rustle3"),
		SoundSources.Contains(TEXT("cache/sound/AmbientA.Nature__Rustle3")));
	TestTrue(TEXT("exact Bush.Rustle3"),
		SoundSources.Contains(TEXT("cache/sound/AmbientA.Bush__Rustle3")));
	TestFalse(TEXT("no plain Rustle3 alias"),
		SoundSources.Contains(TEXT("cache/sound/AmbientA__Rustle3")));
	static const TCHAR* ExpectedMacroTargets[] = {
		TEXT("JamesT.Detail.rocks02Bump"), TEXT("JamesT.rock07"),
		TEXT("WOTDetail.Detail.DetailRock_003"),
		TEXT("WOTDetail.Macro.macromud_001"),
		TEXT("WOTDetail.Macro.macromud_002"),
		TEXT("WOTDetail.Macro.MacroRock_002"),
		TEXT("WOTDetail.Macro.MacroRock_003"),
		TEXT("WOTDetail.Macro.MacroRock_004"),
		TEXT("WOTDetail.Macro.MacroRock_005"),
		TEXT("WOTDetail.Macro.RockSurf500_Mac"),
	};
	for (const TCHAR* Target : ExpectedMacroTargets)
	{
		TestTrue(FString(Target) + TEXT(" macro target is cached"),
			Authenticated.ContainsByPredicate(
				[Target](const FWoTRuntimeBSPTestConverterResource& Row)
				{
					return !Row.bProcedural
						&& WoTRetailSourceExportMatches(Row.SourceExport, Target);
				}));
	}
	// The ten UE1 DetailTextures of the MP world textures (utx2png manifests).
	static const TCHAR* ExpectedDetailTargets[] = {
		TEXT("JamesT.Detail.Mod_Stone03"),
		TEXT("WOTDetail.Detail.detail_plas_006"),
		TEXT("WOTDetail.Detail.detail_stone"),
		TEXT("WOTDetail.Detail.DetailMarb_006"),
		TEXT("WOTDetail.Detail.DetailPlas_005"),
		TEXT("WOTDetail.Detail.DetailRock_003"),
		TEXT("WOTDetail.DetailMarble_010c"),
		TEXT("WOTDetail.DetailMarble_100"),
		TEXT("WOTDetail.DetailWood_100"),
		TEXT("WOTDetail.DetailWood_100h"),
	};
	for (const TCHAR* Target : ExpectedDetailTargets)
	{
		TestTrue(FString(Target) + TEXT(" detail target is cached"),
			Authenticated.ContainsByPredicate(
				[Target](const FWoTRuntimeBSPTestConverterResource& Row)
				{
					return !Row.bProcedural
						&& WoTRetailSourceExportMatches(Row.SourceExport, Target);
				}));
	}
	if (!TestEqual(TEXT("identity census has nineteen map packages"), WorldSources.Num(), 19))
		return false;
	FWoTSkyCensusWriter Writer;
	FWoTRuntimeProjectionCensusProgress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> ImportedWorlds =
		ImportRequiredWorldSetPackageBytesForTesting(
			WorldSources, WorldContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds all world-v3 and BSP objects"),
		ImportedWorlds.IsError()))
	{
		AddError(ImportedWorlds.GetError().ToString());
		return false;
	}

	struct FExpectedResolution
	{
		const TCHAR* Map;
		const TCHAR* Slot;
		const TCHAR* LogicalId;
	};
	// Exact grouped identities (collision) and FName-case source identities.
	static const FExpectedResolution ExpectedResolutions[] = {
		{TEXT("Battle3_01"), TEXT("AesSedaiT__OrnaTrim1505"),
			TEXT("cache/texture/AesSedaiT.Ornament__OrnaTrim1505")},
		{TEXT("Battle3_01"), TEXT("AesSedaiT__PaprWall1441"),
			TEXT("cache/texture/AesSedaiT.Ornament__PaprWall1441")},
		{TEXT("Battle4_01"), TEXT("AesSedaiT__OrnaTrim1505"),
			TEXT("cache/texture/AesSedaiT.Ornament__OrnaTrim1505")},
		{TEXT("Arena_06"), TEXT("NatureT__Sky_340a"), TEXT("cache/texture/NatureT__Sky_340a")},
		{TEXT("Arena_06"), TEXT("NatureT__Sky_340a_Mod_2S_U"),
			TEXT("cache/texture/NatureT__Sky_340a")},
		{TEXT("Arena_06"), TEXT("NatureT__Watr_22"), TEXT("cache/texture/NatureT__Watr_22")},
		{TEXT("Battle2_02"), TEXT("WoTPawns__I_SMashadar"),
			TEXT("cache/texture/WoTPawns__I_SMashadar")},
		{TEXT("Battle2_05"), TEXT("WoTTraps__TrapPit"), TEXT("cache/texture/WoTTraps__TrapPit")},
		{TEXT("Battle2_05"), TEXT("WoTPawns__MyTex2"), TEXT("cache/procedural/WoTPawns__MyTex2")},
		{TEXT("Battle4_01"), TEXT("WoTTraps__TrapPit"), TEXT("cache/texture/WoTTraps__TrapPit")},
		{TEXT("Battle4_01"), TEXT("WoTPawns__MyTex2"), TEXT("cache/procedural/WoTPawns__MyTex2")},
	};
	struct FExpectedMacroCensus
	{
		const TCHAR* Map;
		uint32 Surfaces;
		uint32 Sections;
		uint64 NotSolidTriangles;
	};
	static const FExpectedMacroCensus ExpectedMacroCensus[] = {
		{TEXT("Arena_01"),0,0,343},{TEXT("Arena_02"),1,1,83},
		{TEXT("Arena_03"),0,0,0},{TEXT("Arena_04"),0,0,7},
		{TEXT("Arena_05"),58,1,269},{TEXT("Arena_06"),0,0,136},
		{TEXT("Arena_07"),0,0,29},{TEXT("Arena_08"),0,0,56},
		{TEXT("Arena_09"),0,0,60},{TEXT("Arena_11"),2,1,18},
		{TEXT("Arena_12"),0,0,770},{TEXT("Battle2_01"),436,102,1583},
		{TEXT("Battle2_02"),590,3,3604},{TEXT("Battle2_03"),250,2,1959},
		{TEXT("Battle2_04"),82,1,2268},{TEXT("Battle2_05"),990,22,3202},
		{TEXT("Battle3_01"),453,104,2427},{TEXT("Battle3_02"),960,3,10},
		{TEXT("Battle4_01"),910,23,6873},
	};
	uint32 CheckedMaps = 0;
	uint32 ResolvedSlots = 0;
	uint32 MacroSurfaces = 0;
	uint32 MacroPannedSurfaces = 0;
	uint32 MacroSections = 0;
	uint32 DetailSections = 0;
	TSet<FString> DetailTargets;
	uint32 MacroMoverSections = 0;
	uint32 DynamicMacroMoverSections = 0;
	uint64 NotSolidTriangles = 0;
	uint32 MapsWithNotSolidTriangles = 0;
	TSet<FString> MacroBaseIdentities;
	TSet<FString> MacroTargets;
	for (const FWoTWorldDescriptor& Descriptor : ImportedWorlds.GetValue().Worlds)
	{
		const FString Prefix = Descriptor.MapIdentity + TEXT(" ");
		TArray<uint8>* WorldBytes = Writer.Objects.Find(Descriptor.WorldLogicalId);
		if (!TestNotNull(Prefix + TEXT("world object"), WorldBytes)) continue;
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedWorld =
			FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*WorldBytes),
				TEXT("ue1-world-records-le-v6"), TEXT("world"));
		FWoTRuntimeWorldResource Resource;
		if (!TestFalse(Prefix + TEXT("world frame decodes"), DecodedWorld.IsError())
			|| !TestFalse(Prefix + TEXT("world decodes"), Resource.LoadObjectForTesting(
				MoveTemp(DecodedWorld).GetValue()).IsError())) continue;
		TArray<FWoTBSPPayloadData> Parts;
		bool bPartsDecoded = true;
		for (const FWoTBSPPartDescriptor& PartDescriptor : Descriptor.BSPParts)
		{
			TArray<uint8>* PartBytes = Writer.Objects.Find(PartDescriptor.LogicalId);
			FString PartError;
			FWoTBSPPayloadData& Part = Parts.AddDefaulted_GetRef();
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> DecodedPart = PartBytes != nullptr
				? FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*PartBytes),
					TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"))
				: TWoTBootstrapResult<FWoTRuntimeVerifiedObject>::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::CorruptInstallation, TEXT("missing BSP part object")));
			bPartsDecoded = !DecodedPart.IsError() && UWoTBSPPayloadLibrary::DecodePayload(
				DecodedPart.GetValue().Payload(), Part, PartError);
			if (!bPartsDecoded)
			{
				AddError(Prefix + PartDescriptor.PartIdentity + TEXT(": ")
					+ (DecodedPart.IsError() ? DecodedPart.GetError().ToString() : PartError));
				break;
			}
		}
		if (!bPartsDecoded) continue;
		++CheckedMaps;
		uint32 MapMacroSections = 0;
		uint64 MapNotSolidTriangles = 0;
		TSet<FString> MapMacroBaseIdentities;
		for (const FWoTBSPPayloadData& Part : Parts)
		{
			TestEqual(Prefix + TEXT("BSP macro payload version"),
				Part.FormatVersion, static_cast<uint16>(10));
			TestEqual(Prefix + TEXT("BSP surface provenance length"),
				Part.TriangleSurfaces.Num(), Part.Indices.Num() / 3);
			for (uint32 Surface : Part.TriangleSurfaces)
			{
				if (!TestTrue(Prefix + TEXT("BSP surface provenance range"),
					Resource.Surfaces().IsValidIndex(static_cast<int32>(Surface))))
					continue;
				if ((Resource.Surfaces()[static_cast<int32>(Surface)].Flags
					& 0x00000008u) != 0)
					++MapNotSolidTriangles;
			}
			for (const FWoTBSPPayloadSection& Section : Part.Sections)
			{
				FWoTRuntimeBSPDetailTexture Detail;
				if (WoTRuntimeBSPFindDetailTexture(Section.CanonicalPackage,
					Section.CanonicalName, Detail))
				{
					++DetailSections;
					DetailTargets.Add(Detail.SourceTextureIdentity);
				}
			}
			for (const FWoTBSPPayloadSection& Section : Part.Sections)
			{
				if (Section.MacroRatio <= 0.0f) continue;
				++MapMacroSections;
				MapMacroBaseIdentities.Add(
					Section.SourceTextureIdentity.ToLower());
				MacroBaseIdentities.Add(
					Section.SourceTextureIdentity.ToLower());
				MacroTargets.Add(Section.MacroSourceTextureIdentity.ToLower());
				FWoTRetailObjectPath MacroPath;
				FWoTRetailObjectPath MacroSource;
				if (TestTrue(Prefix + Section.MacroSourceTextureIdentity
					+ TEXT(" macro source parses"), WoTParseRetailObjectPath(
						Section.MacroSourceTextureIdentity, MacroSource)))
				{
					MacroPath.Groups = MoveTemp(MacroSource.Groups);
					MacroPath.Package = Section.MacroCanonicalPackage;
					MacroPath.Name = Section.MacroCanonicalName;
					const FString LogicalBase = WoTRetailResolveLogicalBase(
						EWoTNeutralResourceKind::Texture, MacroPath,
						[&Authenticated](const FString& LogicalId)
						{
							return Authenticated.ContainsByPredicate(
								[&LogicalId](
									const FWoTRuntimeBSPTestConverterResource& Row)
								{
									return !Row.bProcedural && LogicalId.Equals(
										TEXT("cache/texture/") + Row.LogicalBase,
										ESearchCase::IgnoreCase);
								});
						});
					const FWoTRuntimeBSPTestConverterResource* Cached =
						Authenticated.FindByPredicate(
						[&LogicalBase](const FWoTRuntimeBSPTestConverterResource& Row)
						{
							return !Row.bProcedural && Row.LogicalBase.Equals(
								LogicalBase, ESearchCase::IgnoreCase);
						});
					if (TestNotNull(Prefix + Section.MacroSourceTextureIdentity
						+ TEXT(" is present in the production cache contract"), Cached))
					{
						TestTrue(Prefix + Section.MacroSourceTextureIdentity
							+ TEXT(" cache source matches"),
							WoTRetailSourceExportMatches(Cached->SourceExport,
								Section.MacroSourceTextureIdentity));
					}
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s PF_NotSolid rendered triangles=%llu"),
			*Descriptor.MapIdentity,
			static_cast<unsigned long long>(MapNotSolidTriangles)));
		NotSolidTriangles += MapNotSolidTriangles;
		if (MapNotSolidTriangles != 0) ++MapsWithNotSolidTriangles;
		uint32 MapMacroSurfaces = 0;
		for (const FWoTRuntimeWorldSurface& Surface : Resource.Surfaces())
		{
			if (!MapMacroBaseIdentities.Contains(
				Surface.TextureIdentity.ToLower())) continue;
			++MapMacroSurfaces;
			if (Surface.PanU != 0 || Surface.PanV != 0) ++MacroPannedSurfaces;
		}
		const FExpectedMacroCensus* ExpectedMacro = nullptr;
		for (const FExpectedMacroCensus& Row : ExpectedMacroCensus)
			if (Descriptor.MapIdentity == Row.Map) ExpectedMacro = &Row;
		if (TestNotNull(Prefix + TEXT("macro census row"), ExpectedMacro))
		{
			TestEqual(Prefix + TEXT("PF_NotSolid rendered triangle census"),
				MapNotSolidTriangles, ExpectedMacro->NotSolidTriangles);
			TestEqual(Prefix + TEXT("macro-referenced FBSPSurfs"),
				MapMacroSurfaces, ExpectedMacro->Surfaces);
			TestEqual(Prefix + TEXT("runtime macro sections"),
				MapMacroSections, ExpectedMacro->Sections);
		}
		MacroSurfaces += MapMacroSurfaces;
		MacroSections += MapMacroSections;
		for (const FWoTRuntimeMoverModel& Mover : Resource.MoverModels())
		{
			for (const FWoTRuntimeMoverSection& Section : Mover.Sections)
			{
				if (Section.MacroRatio <= 0.0f) continue;
				if (Mover.bLightmapped) ++MacroMoverSections;
				else ++DynamicMacroMoverSections;
			}
		}
		TArray<FString> Unresolved;
		TArray<FString> DefaultSlots;
		TMap<FString, FString> Resolved;
		FString Error;
		if (!TestTrue(Prefix + TEXT("sections resolve"),
			WoTWorldResolveBSPSectionTexturesForTesting(Descriptor, Parts, Authenticated,
				Unresolved, DefaultSlots, Resolved, Error)))
		{
			AddError(Error);
			continue;
		}
		ResolvedSlots += static_cast<uint32>(Resolved.Num());
		TestEqual(Prefix + TEXT("unresolved slots (") + FString::Join(Unresolved, TEXT(","))
			+ TEXT(")"), Unresolved.Num(), 0);
		const TCHAR* ExpectedDefault = Descriptor.MapIdentity == TEXT("Arena_09")
			? TEXT("__None") : Descriptor.MapIdentity == TEXT("Battle3_02")
				? TEXT("WOT__Watr_400") : TEXT("");
		TestEqual(Prefix + TEXT("retail DefaultTexture slots"),
			FString::Join(DefaultSlots, TEXT(",")), FString(ExpectedDefault));
		for (const FExpectedResolution& Row : ExpectedResolutions)
		{
			if (Descriptor.MapIdentity != Row.Map) continue;
			const FString* Actual = Resolved.Find(Row.Slot);
			if (TestNotNull(Prefix + Row.Slot + TEXT(" resolves"), Actual))
			{
				TestEqual(Prefix + Row.Slot + TEXT(" logical id"),
					*Actual, FString(Row.LogicalId));
			}
		}

		// Every Sound object reference of the map's actors resolves exact-first
		// to a contract object whose source export is that reference.
		for (const FWoTRuntimeWorldActor& Actor : Resource.Actors())
		{
			for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
			{
				if (Property.Type != EWoTRuntimeWorldPropertyType::Object
					|| !Property.ObjectClassIdentity.EndsWith(TEXT(".Sound"))
					|| Property.ObjectIdentity.IsEmpty()) continue;
				FWoTRetailObjectPath Path;
				if (!TestTrue(Prefix + Property.ObjectIdentity + TEXT(" parses"),
					WoTParseRetailObjectPath(Property.ObjectIdentity, Path))) continue;
				const FString Base = WoTRetailResolveLogicalBase(
					EWoTNeutralResourceKind::Sound, Path,
					[&SoundSources](const FString& LogicalId)
					{ return SoundSources.Contains(LogicalId); });
				const FString* Source = SoundSources.Find(TEXT("cache/sound/") + Base);
				if (TestNotNull(Prefix + Property.ObjectIdentity + TEXT(" is cached"), Source))
				{
					TestTrue(Prefix + Property.ObjectIdentity + TEXT(" source ") + *Source,
						WoTRetailSourceExportMatches(*Source, Property.ObjectIdentity));
				}
				if (Property.ObjectIdentity.Contains(TEXT("Rustle3")))
				{
					TestEqual(Prefix + TEXT("Rustle3 exact sub path"),
						WoTRetailAssetSubPath(EWoTNeutralResourceKind::Sound, Path,
							[&SoundSources](const FString& LogicalId)
							{ return SoundSources.Contains(LogicalId); }),
						Descriptor.MapIdentity == TEXT("Battle2_04")
							? FString(TEXT("AmbientA/Nature/Rustle3"))
							: FString(TEXT("AmbientA/Bush/Rustle3")));
				}
			}
		}
	}
	TestEqual(TEXT("all nineteen maps were checked"), CheckedMaps, 19u);
	TestEqual(TEXT("production PF_NotSolid rendered triangle census"),
		NotSolidTriangles, 23697ull);
	TestEqual(TEXT("production maps with PF_NotSolid rendered triangles"),
		MapsWithNotSolidTriangles, 18u);
	AddInfo(FString::Printf(
		TEXT("PF_NotSolid census total triangles=%llu maps=%u/19"),
		static_cast<unsigned long long>(NotSolidTriangles),
		MapsWithNotSolidTriangles));
	TestTrue(TEXT("sections were resolved"), ResolvedSlots > 1000u);
	TestEqual(TEXT("macro-referenced FBSPSurf census"), MacroSurfaces, 4732u);
	TestEqual(TEXT("macro-referenced non-zero pan census"),
		MacroPannedSurfaces, 272u);
	TestEqual(TEXT("runtime exact-macro section census"), MacroSections, 263u);
	TestEqual(TEXT("baked mover macro section census"), MacroMoverSections, 1u);
	TestEqual(TEXT("dynamic mover macro section census"),
		DynamicMacroMoverSections, 0u);
	TestEqual(TEXT("distinct macro targets used by the MP world surfaces"),
		MacroTargets.Num(), 5);
	TestEqual(TEXT("runtime world sections whose texture authors a detail"),
		DetailSections, 1517u);
	TestEqual(TEXT("distinct detail targets used by the MP world sections"),
		DetailTargets.Num(), 10);
	for (const FString& Target : DetailTargets)
	{
		TestTrue(Target + TEXT(" world detail target is an expected cached target"),
			Algo::AnyOf(ExpectedDetailTargets, [&Target](const TCHAR* Expected)
			{
				return Target.Equals(Expected, ESearchCase::IgnoreCase);
			}));
	}
	return !HasAnyErrors();
}

// Guarded candidates are materialized before UWorld::InitWorld creates the
// physics scene, so projected actors' native components register with NO body
// (the packaged seal-match run found every altar/pickup/trigger inert: no
// overlaps at all). The exposed world's post-initialization hook completes them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldMaterializerPhysicsCompletionSpec,
	"WoT.World.Materializer.PhysicsStateCompletion",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldMaterializerPhysicsCompletionSpec::RunTest(const FString&)
{
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("PhysicsCompletion"));
	if (!TestNotNull(TEXT("uninitialized candidate world"), World))
	{
		return false;
	}
	TestNull(TEXT("candidate has no physics scene yet (as in MaterializeBeforePlay)"),
		World->GetPhysicsScene());
	const FTransform Transform(FRotator::ZeroRotator, FVector(100, 200, 300));
	AWoTSealAltar* Altar = World->SpawnActorDeferred<AWoTSealAltar>(
		AWoTSealAltar::StaticClass(), Transform);
	if (!TestNotNull(TEXT("deferred altar"), Altar))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	Altar->SetUE1CollisionSize(140.0f, 1.0f);
	Altar->FinishSpawning(Transform, true);
	TestTrue(TEXT("the altar touch capsule is registered"), Altar->Touch->IsRegistered());
	TestFalse(TEXT("but has no physics body before the scene exists"),
		Altar->Touch->IsPhysicsStateCreated());
	TestEqual(TEXT("completion is a no-op without a physics scene"),
		WoTWorldCompleteRegisteredPhysicsState(*World), 0);

	World->InitWorld();
	TestNotNull(TEXT("InitWorld created the physics scene"), World->GetPhysicsScene());
	TestFalse(TEXT("InitWorld alone does not create bodies for registered components"),
		Altar->Touch->IsPhysicsStateCreated());
	TestTrue(TEXT("completion creates the missing bodies"),
		WoTWorldCompleteRegisteredPhysicsState(*World) >= 1);
	TestTrue(TEXT("the altar touch capsule now has a physics body"),
		Altar->Touch->IsPhysicsStateCreated());
	TestEqual(TEXT("completion is idempotent"),
		WoTWorldCompleteRegisteredPhysicsState(*World), 0);
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

// The render-state twin: a candidate is materialized before UWorld::InitWorld
// allocates the scene (World.cpp:2484), so projected actors' default components
// register with NO render state (ActorComponent.cpp:2519) and nothing later
// creates one. Packaged Arena_07: all six movers
// were registered, visible, closed and at BasePos with render_state=0 - the
// four Tag=Cross slabs over the lava pit were simply never drawn, and every
// Battle2_02 door/portcullis/rotating mover the same. The post-initialization
// hook completes them (WoTWorldCompleteRegisteredRenderState).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldMaterializerRenderCompletionSpec,
	"WoT.World.Materializer.RenderStateCompletion",
	WoTRuntimeMapProjectionFlags)

bool FWoTWorldMaterializerRenderCompletionSpec::RunTest(const FString&)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("engine cube mesh"), Cube))
	{
		return false;
	}
	UWorld* World = WoTRuntimeMapProjectionWorld(TEXT("RenderCompletion"));
	if (!TestNotNull(TEXT("uninitialized candidate world"), World))
	{
		return false;
	}
	TestNull(TEXT("candidate has no scene yet (as in MaterializeBeforePlay)"),
		World->Scene);
	// Arena_07 Mover4's BasePos; a second mover has no mesh bound.
	const FTransform Transform(FRotator::ZeroRotator, FVector(-128, -640, -160));
	AWoTMover* Slab = World->SpawnActorDeferred<AWoTMover>(
		AWoTMover::StaticClass(), Transform);
	AWoTMover* MeshLess = World->SpawnActorDeferred<AWoTMover>(
		AWoTMover::StaticClass(), Transform);
	if (!TestNotNull(TEXT("deferred slab mover"), Slab)
		|| !TestNotNull(TEXT("deferred mesh-less mover"), MeshLess))
	{
		WoTRuntimeMapProjectionDestroyWorld(World);
		return false;
	}
	Slab->Mesh->SetStaticMesh(Cube);
	Slab->FinishSpawning(Transform, true);
	MeshLess->FinishSpawning(Transform, true);
	TestTrue(TEXT("the slab mesh is registered"), Slab->Mesh->IsRegistered());
	TestTrue(TEXT("and visible"), Slab->Mesh->IsVisible());
	TestFalse(TEXT("but has no render state before the scene exists"),
		Slab->Mesh->IsRenderStateCreated());
	TestEqual(TEXT("completion is a no-op without a scene"),
		WoTWorldCompleteRegisteredRenderState(*World), 0);

	World->InitWorld();
	TestNotNull(TEXT("InitWorld allocated the scene"), World->Scene);
	TestFalse(TEXT("InitWorld alone does not create render state for registered components"),
		Slab->Mesh->IsRenderStateCreated());
	if (!FApp::CanEverRender())
	{
		// -nullrhi: registration into a live world creates none either
		// (ActorComponent.cpp:2519 gates on FApp::CanEverRender()).
		AddInfo(TEXT("no renderer (-nullrhi): render-state creation is exercised by a rendering run"));
		TestEqual(TEXT("completion creates nothing where the engine creates nothing"),
			WoTWorldCompleteRegisteredRenderState(*World), 0);
		WoTRuntimeMapProjectionDestroyWorld(World);
		return !HasAnyErrors();
	}
	TestTrue(TEXT("completion creates the missing render states"),
		WoTWorldCompleteRegisteredRenderState(*World) >= 1);
	TestTrue(TEXT("the slab mesh now has a render state"),
		Slab->Mesh->IsRenderStateCreated());
	TestFalse(TEXT("a mesh-less mover keeps none (UStaticMeshComponent::ShouldCreateRenderState)"),
		MeshLess->Mesh->IsRenderStateCreated());
	TestEqual(TEXT("completion is idempotent"),
		WoTWorldCompleteRegisteredRenderState(*World), 0);
	WoTRuntimeMapProjectionDestroyWorld(World);
	return !HasAnyErrors();
}

#endif
