// Headless specs for mesh manifest validation, CPU deltas, tangent pins, and
// UWoTVertexAnimComponent's unified native/external morph-access seam.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTGameInstance.h"
#include "../WoTMeshPayloadLayout.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSha256.h"
#include "../WoTVertexAnimComponent.h"

#include "Animation/MorphTarget.h"
#include "Components/ExternalMorphSet.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
#include "ReferenceSkeleton.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "SkinnedAssetCompiler.h"
#include "StaticToSkeletalMeshConverter.h"
#endif

namespace
{
	template<typename TWoTPayloadMeshTests_Tag,
		typename TWoTPayloadMeshTests_Tag::Type WoTPayloadMeshTests_Member>
	struct TWoTPayloadMeshTests_PrivateMemberAccessor
	{
		friend typename TWoTPayloadMeshTests_Tag::Type
			WoTPayloadMeshTests_GetPrivateMember(TWoTPayloadMeshTests_Tag)
		{
			return WoTPayloadMeshTests_Member;
		}
	};

	struct FWoTPayloadMeshTests_PackageHandlerTag
	{
		using Type = void (UWoTPayloadSubsystem::*)(UPackage*);
		friend Type WoTPayloadMeshTests_GetPrivateMember(
			FWoTPayloadMeshTests_PackageHandlerTag);
	};
	template struct TWoTPayloadMeshTests_PrivateMemberAccessor<
		FWoTPayloadMeshTests_PackageHandlerTag,
		&UWoTPayloadSubsystem::ProcessCompletedPackage>;

	struct FWoTPayloadMeshTests_ValidateMeshTag
	{
		using Type = bool (*)(USkeletalMesh*,
			const UWoTMeshPayloadLayout&, FString&);
		friend Type WoTPayloadMeshTests_GetPrivateMember(
			FWoTPayloadMeshTests_ValidateMeshTag);
	};
	template struct TWoTPayloadMeshTests_PrivateMemberAccessor<
		FWoTPayloadMeshTests_ValidateMeshTag,
		&UWoTPayloadSubsystem::ValidateStrippedMeshLayout>;

	struct FWoTPayloadMeshTests_AccessMorphTag
	{
		using Type = float (UWoTVertexAnimComponent::*)(
			int32, TOptional<float>);
		friend Type WoTPayloadMeshTests_GetPrivateMember(
			FWoTPayloadMeshTests_AccessMorphTag);
	};
	template struct TWoTPayloadMeshTests_PrivateMemberAccessor<
		FWoTPayloadMeshTests_AccessMorphTag,
		&UWoTVertexAnimComponent::AccessFrameMorph>;

	struct FWoTPayloadMeshTests_InstallMorphTag
	{
		using Type = bool (UWoTVertexAnimComponent::*)(
			TSharedPtr<FExternalMorphSet>, int32);
		friend Type WoTPayloadMeshTests_GetPrivateMember(
			FWoTPayloadMeshTests_InstallMorphTag);
	};
	template struct TWoTPayloadMeshTests_PrivateMemberAccessor<
		FWoTPayloadMeshTests_InstallMorphTag,
		&UWoTVertexAnimComponent::InstallExternalMorphSet>;

	struct FWoTPayloadMeshTests_SetFrameMorphsTag
	{
		using Type = void (UWoTVertexAnimComponent::*)(
			int32, float, int32, float);
		friend Type WoTPayloadMeshTests_GetPrivateMember(
			FWoTPayloadMeshTests_SetFrameMorphsTag);
	};
	template struct TWoTPayloadMeshTests_PrivateMemberAccessor<
		FWoTPayloadMeshTests_SetFrameMorphsTag,
		&UWoTVertexAnimComponent::SetFrameMorphs>;

	struct FWoTPayloadMeshTests_ClearExternalTag
	{
		using Type = void (UWoTVertexAnimComponent::*)();
		friend Type WoTPayloadMeshTests_GetPrivateMember(
			FWoTPayloadMeshTests_ClearExternalTag);
	};
	template struct TWoTPayloadMeshTests_PrivateMemberAccessor<
		FWoTPayloadMeshTests_ClearExternalTag,
		&UWoTVertexAnimComponent::ClearExternalMorphInjection>;

	enum class EWoTPayloadMeshTests_Mode
	{
		Valid,
		LyingCounts,
		ManifestBlobLengthMismatch,
		LyingBlobHeader,
	};

	struct FWoTPayloadMeshTests_Fixture
	{
		FString CacheDirectory;
		FString PackageName;
		FString AssetName;
		FString CacheKey;
		FString BlobPath;
		TArray<uint8> BlobBytes;
	};

	struct FWoTPayloadMeshTests_World
	{
		UWorld* World = nullptr;
		UWoTGameInstance* GameInstance = nullptr;
	};

	void WoTPayloadMeshTests_AppendU16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	}

	void WoTPayloadMeshTests_AppendU32(TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
		}
	}

	void WoTPayloadMeshTests_AppendU64(TArray<uint8>& Bytes, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
		}
	}

	void WoTPayloadMeshTests_AppendASCII(
		TArray<uint8>& Bytes, const FString& Value)
	{
		for (TCHAR Character : Value)
		{
			check(Character <= 127);
			Bytes.Add(static_cast<uint8>(Character));
		}
	}

	FString WoTPayloadMeshTests_CacheRoot(const FString& CanonicalCore,
		const FString& BlobPath, uint64 BlobSize, const FString& BlobSHA)
	{
		TArray<uint8> Input;
		WoTPayloadMeshTests_AppendASCII(
			Input, TEXT("WOT-PAYLOAD-CACHE-HASH-V1"));
		Input.Add(0);
		WoTPayloadMeshTests_AppendU64(Input, CanonicalCore.Len());
		WoTPayloadMeshTests_AppendASCII(Input, CanonicalCore);
		TArray<uint8> Record;
		WoTPayloadMeshTests_AppendASCII(Record, BlobPath);
		Record.Add(0);
		WoTPayloadMeshTests_AppendASCII(Record, LexToString(BlobSize));
		Record.Add(0);
		WoTPayloadMeshTests_AppendASCII(Record, BlobSHA);
		Record.Add('\n');
		WoTPayloadMeshTests_AppendU64(Input, Record.Num());
		Input.Append(Record);
		return WoTSha256::HashBuffer(Input.GetData(), Input.Num());
	}

	uint32 WoTPayloadMeshTests_Pack(int32 X, int32 Y, int32 Z)
	{
		return (static_cast<uint32>(X) & 0x7ffu)
			| ((static_cast<uint32>(Y) & 0x7ffu) << 11)
			| ((static_cast<uint32>(Z) & 0x3ffu) << 22);
	}

	TArray<uint32> WoTPayloadMeshTests_PackedFrames()
	{
		return {
			WoTPayloadMeshTests_Pack(0, 0, 0),
			WoTPayloadMeshTests_Pack(10, 0, 0),
			WoTPayloadMeshTests_Pack(0, 10, 0),
			WoTPayloadMeshTests_Pack(0, 0, 0),
			WoTPayloadMeshTests_Pack(10, 0, 0),
			WoTPayloadMeshTests_Pack(0, 10, 10),
		};
	}

	FWoTPayloadMeshTests_Fixture WoTPayloadMeshTests_WriteFixture(
		EWoTPayloadMeshTests_Mode Mode)
	{
		FWoTPayloadMeshTests_Fixture Result;
		Result.PackageName = TEXT("WoTPayloadMeshFixture_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Result.AssetName = TEXT("TriMesh");
		Result.CacheKey = FString::Printf(TEXT("mesh:%s__%s"),
			*Result.PackageName, *Result.AssetName);
		Result.CacheDirectory = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
			TEXT("WoTPayloadMeshTests_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Result.CacheDirectory, true);

		Result.BlobBytes.Append({'W', 'O', 'T', 'P', 'M', 'E', 'S', 'H'});
		WoTPayloadMeshTests_AppendU16(Result.BlobBytes, 1);
		WoTPayloadMeshTests_AppendU16(Result.BlobBytes, 0);
		WoTPayloadMeshTests_AppendU32(Result.BlobBytes,
			Mode == EWoTPayloadMeshTests_Mode::LyingBlobHeader ? 4 : 3);
		WoTPayloadMeshTests_AppendU32(Result.BlobBytes, 2);
		WoTPayloadMeshTests_AppendU32(Result.BlobBytes, 6);
		for (uint32 Packed : WoTPayloadMeshTests_PackedFrames())
		{
			WoTPayloadMeshTests_AppendU32(Result.BlobBytes, Packed);
		}
		const FString BlobSHA = WoTSha256::HashBuffer(
			Result.BlobBytes.GetData(), Result.BlobBytes.Num());
		Result.BlobPath = FString::Printf(TEXT("blobs/%s/%s.blob"),
			*BlobSHA.Left(2), *BlobSHA);

		int32 FrameVertices = 3;
		int32 Frames = 2;
		int32 PackedCount = 6;
		int32 FrameBytes = 12;
		if (Mode == EWoTPayloadMeshTests_Mode::LyingCounts)
		{
			PackedCount = 5;
		}
		else if (Mode == EWoTPayloadMeshTests_Mode::ManifestBlobLengthMismatch)
		{
			Frames = 3;
			PackedCount = 9;
		}
		const int32 ExpandedBytes = FrameBytes * Frames;
		const FString Entry = FString::Printf(
			TEXT("{\"anim_frames\":%d,\"blob\":\"%s\",\"class_name\":\"Mesh\",")
			TEXT("\"encoding\":\"ue1-packed-verts-v1\",\"frame_byte_length\":%d,")
			TEXT("\"frame_verts\":%d,\"name\":\"%s\",\"package\":\"%s\",")
			TEXT("\"packed_vertex_count\":%d,\"sha256\":\"%s\",\"size\":%d,")
			TEXT("\"type\":\"meshes\"}"), Frames, *Result.BlobPath,
			FrameBytes, FrameVertices, *Result.AssetName, *Result.PackageName,
			PackedCount, *BlobSHA, Result.BlobBytes.Num());
		const FString SourcePath = FString::Printf(
			TEXT("System/%s.u"), *Result.PackageName);
		const FString CanonicalCore = FString::Printf(
			TEXT("{\"cache_version\":1,\"entries\":{\"%s\":%s},")
			TEXT("\"format\":\"wot-payload-cache\",\"hash_algorithm\":\"sha256\",")
			TEXT("\"limits\":{\"max_blob_size\":268435456,\"max_entries\":10000,")
			TEXT("\"max_texture_dimension\":8192,\"max_total_expanded_bytes\":2147483648},")
			TEXT("\"skipped\":[],\"sources\":[{\"path\":\"%s\",")
			TEXT("\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",")
			TEXT("\"size\":1,\"type\":\"meshes\"}],")
			TEXT("\"summary\":{\"entries\":{\"meshes\":1,\"sounds\":0,\"textures\":0},")
			TEXT("\"logical_payload_bytes\":%d,")
			TEXT("\"source_packages\":{\"meshes\":1,\"sounds\":0,\"textures\":0},")
			TEXT("\"total_expanded_bytes\":%d,\"unique_blob_bytes\":%d,")
			TEXT("\"unique_blobs\":1},\"types\":[\"meshes\"]}"),
			*Result.CacheKey, *Entry, *SourcePath, Result.BlobBytes.Num(),
			ExpandedBytes, Result.BlobBytes.Num());
		const FString Root = WoTPayloadMeshTests_CacheRoot(CanonicalCore,
			Result.BlobPath, Result.BlobBytes.Num(), BlobSHA);
		const FString Manifest = FString::Printf(TEXT("{\"cache_sha256\":\"%s\",%s"),
			*Root, *CanonicalCore.RightChop(1));
		FFileHelper::SaveStringToFile(Manifest,
			*FPaths::Combine(Result.CacheDirectory, TEXT("manifest.json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString BlobFilename = FPaths::Combine(
			Result.CacheDirectory, Result.BlobPath);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(BlobFilename), true);
		FFileHelper::SaveArrayToFile(Result.BlobBytes, *BlobFilename);
		return Result;
	}

	FWoTPayloadMeshTests_World WoTPayloadMeshTests_CreateWorld(
		const FString& CacheDirectory, bool bEnabled)
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, bEnabled);
		FWoTPayloadMeshTests_World Result;
		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.BeforeInitializeStandalone = [&CacheDirectory](UGameInstance* GameInstance)
		{
			CastChecked<UWoTGameInstance>(GameInstance)->SetPayloadCacheDir(
				CacheDirectory, false);
		};
		Options.bStartPlay = false;
		Result.World = WoTTest::CreateSpecWorld(Options);
		Result.GameInstance = Result.World
			? Cast<UWoTGameInstance>(Result.World->GetGameInstance()) : nullptr;
		return Result;
	}

	void WoTPayloadMeshTests_DestroyWorld(FWoTPayloadMeshTests_World& State)
	{
		WoTTest::DestroySpecWorld(State.World);
		State = FWoTPayloadMeshTests_World();
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
	}

	void WoTPayloadMeshTests_DeleteFixture(
		const FWoTPayloadMeshTests_Fixture& Fixture)
	{
		IFileManager::Get().DeleteDirectory(
			*Fixture.CacheDirectory, false, true);
	}

	UWoTMeshPayloadLayout* WoTPayloadMeshTests_MakeLayout(
		UObject* Outer, const FString& CacheKey)
	{
		UWoTMeshPayloadLayout* Layout = NewObject<UWoTMeshPayloadLayout>(Outer);
		Layout->PayloadKey = CacheKey;
		Layout->AnimationLogicalId = TEXT("cache/animation/")
			+ CacheKey.RightChop(5);
		Layout->Scale = FVector::OneVector;
		Layout->FrameVertexCount = 3;
		Layout->AnimationFrameCount = 2;
		Layout->SmoothTriangles = {0, 1, 2};
		Layout->RenderVertexToUE1Vertex = {0, 1, 2, 0};
		Layout->PinnedBaseTangentZ = {
			FVector::UpVector, FVector::UpVector,
			FVector::UpVector, FVector::UpVector};
		Layout->PinnedBaseTangentZSHA256 =
			UWoTMeshPayloadLayout::HashTangentBasis(Layout->PinnedBaseTangentZ);
		Layout->bRenderVertexMappingVerified = true;
		Layout->bStrippedBasePositions = true;
		Layout->bTangentRecomputeDisabled = true;
		return Layout;
	}

	USkeletalMesh* WoTPayloadMeshTests_MakeBaseMesh(
		const TArray<FVector3f>& BasePositions)
	{
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
		Mesh->AllocateResourceForRendering();
		FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		const int32 LODIndex =
			RenderData->LODRenderData.Add(new FSkeletalMeshLODRenderData());
		FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
		LOD.StaticVertexBuffers.PositionVertexBuffer.Init(
			BasePositions, /*bInNeedsCPUAccess*/ true);
		return Mesh;
	}

#if WITH_EDITOR
	USkeletalMesh* WoTPayloadMeshTests_BuildCarrierFixture(
		FMeshDescription& Description)
	{
		FReferenceSkeleton ReferenceSkeleton;
		{
			FReferenceSkeletonModifier Modifier(ReferenceSkeleton, nullptr);
			Modifier.Add(FMeshBoneInfo(
				TEXT("Root"), TEXT("Root"), INDEX_NONE), FTransform::Identity);
		}
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
		TArray<const FMeshDescription*> Descriptions = {&Description};
		FStaticToSkeletalMeshConverter::FInitializationParams Params;
		Params.bRecomputeNormals = false;
		Params.bRecomputeTangents = false;
		Params.bCacheOptimize = false;
		if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(
				Mesh, Descriptions, ReferenceSkeleton, Params))
		{
			return nullptr;
		}
		FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(0);
		if (!LODInfo)
		{
			return nullptr;
		}
		LODInfo->BuildSettings.bRecomputeNormals = false;
		LODInfo->BuildSettings.bRecomputeTangents = false;
		LODInfo->BuildSettings.bUseFullPrecisionUVs = true;
		LODInfo->BuildSettings.bRemoveDegenerates = false;
		LODInfo->bAllowCPUAccess = true;
		Mesh->Build();
		FSkinnedAssetCompilingManager::Get().FinishCompilation({Mesh});
		return Mesh;
	}

	USkeletalMesh* WoTPayloadMeshTests_MakeWeldRegressionMesh()
	{
		FMeshDescription Description;
		FSkeletalMeshAttributes Attributes(Description);
		Attributes.Register();
		Description.SetNumUVChannels(2);

		TVertexAttributesRef<FVector3f> Positions =
			Attributes.GetVertexPositions();
		FSkinWeightsVertexAttributesRef SkinWeights =
			Attributes.GetVertexSkinWeights();
		TVertexInstanceAttributesRef<FVector3f> Normals =
			Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents =
			Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns =
			Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors =
			Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs =
			Attributes.GetVertexInstanceUVs();
		// Channel 1 carries the mapping carrier; writing it without declaring
		// two channels is an out-of-bounds fatal (crashed the first suite run).
		UVs.SetNumChannels(2);

		// Two wedges (A0, A1) of UE1 vertex one, plus B (UE1 vertex two) and
		// D (UE1 vertex zero). The two same-carrier wedges are placed in SEPARATE
		// triangles of a shared-edge quad: FStaticToSkeletalMeshConverter cannot
		// build a single triangle whose two corners share the carrier UV1.X (it
		// merges them and corrupts the per-vertex skin-weight buffer -> garbage
		// "non-existent bone"), and no sub-tolerance carrier nudge escapes its UV
		// merge threshold while staying inside the 0.01 decode tolerance. Split
		// across two triangles, every triangle has three distinct-carrier corners
		// and builds cleanly. The strip still keys the lattice per description
		// vertex, so A0 and A1 remain the "same UE1 vertex, two wedges" weld case.
		// The retail single-triangle (A,A,B) form is covered by the
		// AngrealGuardianPickup real-mesh probe.
		// A0 and A1 share the UE1 vertex-one retail position (only 0.01 UU apart,
		// as wedges of one UE1 vertex are in retail); B and D take the UE1 vertex
		// two and zero positions. The strip cross-checks each wedge's canonical
		// position against its decoded UE1 base position, so these must line up
		// with ExpectedBasePositions below.
		const FVector3f FixturePositions[4] = {
			FVector3f(0.0f, 0.0f, 0.0f),
			FVector3f(0.0f, 0.01f, 0.0f),
			FVector3f(100.0f, 0.0f, 0.0f),
			FVector3f(0.0f, 100.0f, 0.0f)};
		const int32 UE1Vertices[4] = {1, 1, 2, 0};
		TArray<FVertexID> Vertices;
		Vertices.Reserve(4);
		for (int32 VertexIndex = 0; VertexIndex < 4; ++VertexIndex)
		{
			const FVertexID VertexID = Description.CreateVertex();
			Vertices.Add(VertexID);
			Positions[VertexID] = FixturePositions[VertexIndex];
			const FBoneIndexType InfluenceBones[1] = {0};
			// Explicit-count Create overload: the fixed-size [MaxInlineBoneWeightCount]
			// overload scans 12 entries until a zero weight and reads past a 1-element
			// array (UB: garbage influences under MSVC; the suite's standing fail).
			const float InfluenceWeights[1] = {1.0f};
			SkinWeights.Set(VertexID, UE::AnimationCore::FBoneWeights::Create(
				InfluenceBones, InfluenceWeights, 1));
		}

		auto MakeInstance = [&](int32 VertexIndex)
		{
			const FVertexInstanceID Instance =
				Description.CreateVertexInstance(Vertices[VertexIndex]);
			const int32 UE1Vertex = UE1Vertices[VertexIndex];
			Normals[Instance] = FVector3f::UpVector;
			Tangents[Instance] = FVector3f::ForwardVector;
			BinormalSigns[Instance] = 1.0f;
			Colors[Instance] = FVector4f(
				static_cast<float>(UE1Vertex + 1) / 255.0f, 0.0f, 0.0f, 1.0f);
			UVs.Set(Instance, 0, FVector2f::ZeroVector);
			UVs.Set(Instance, 1,
				FVector2f(static_cast<float>(UE1Vertex + 1), 0.0f));
			return Instance;
		};
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		// Triangle one uses wedge A0 of UE1 vertex one; triangle two uses wedge A1.
		const TArray<FVertexInstanceID> FirstTriangle = {
			MakeInstance(0), MakeInstance(2), MakeInstance(3)};
		const TArray<FVertexInstanceID> SecondTriangle = {
			MakeInstance(1), MakeInstance(3), MakeInstance(2)};
		Description.CreateTriangle(Group, FirstTriangle);
		Description.CreateTriangle(Group, SecondTriangle);
		return WoTPayloadMeshTests_BuildCarrierFixture(Description);
	}

	USkeletalMesh* WoTPayloadMeshTests_MakeManyWedgeRegressionMesh()
	{
		FMeshDescription Description;
		FSkeletalMeshAttributes Attributes(Description);
		Attributes.Register();
		Description.SetNumUVChannels(2);

		TVertexAttributesRef<FVector3f> Positions =
			Attributes.GetVertexPositions();
		FSkinWeightsVertexAttributesRef SkinWeights =
			Attributes.GetVertexSkinWeights();
		TVertexInstanceAttributesRef<FVector3f> Normals =
			Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents =
			Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns =
			Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors =
			Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs =
			Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(2);

		auto MakeVertex = [&](const FVector3f& Position)
		{
			const FVertexID VertexID = Description.CreateVertex();
			Positions[VertexID] = Position;
			const FBoneIndexType InfluenceBones[1] = {0};
			// Explicit-count Create overload: the fixed-size [MaxInlineBoneWeightCount]
			// overload scans 12 entries until a zero weight and reads past a 1-element
			// array (UB: garbage influences under MSVC; the suite's standing fail).
			const float InfluenceWeights[1] = {1.0f};
			SkinWeights.Set(VertexID, UE::AnimationCore::FBoneWeights::Create(
				InfluenceBones, InfluenceWeights, 1));
			return VertexID;
		};
		constexpr int32 RepeatedWedgeCount = 12;
		TArray<FVertexID> RepeatedVertices;
		RepeatedVertices.Reserve(RepeatedWedgeCount);
		// Many wedges of one UE1 vertex (all carry UE1 vertex zero). Each is in its
		// OWN triangle, so there is no single-triangle carrier collision to corrupt
		// the build; the canonical positions only need to (a) clear the converter's
		// ~2e-5 UU position-weld threshold and (b) stay within the strip's 0.02 UU
		// base-position tolerance of the shared UE1 vertex-zero base at the origin.
		// A 0.001 UU step satisfies both (span 0.011 < 0.02). The strip replaces
		// these with the per-wedge lattice.
		for (int32 Wedge = 0; Wedge < RepeatedWedgeCount; ++Wedge)
		{
			RepeatedVertices.Add(MakeVertex(
				FVector3f(0.0f, 0.0f, Wedge * 0.001f)));
		}
		const FVertexID B = MakeVertex(FVector3f(100.0f, 0.0f, 0.0f));
		const FVertexID C = MakeVertex(FVector3f(0.0f, 100.0f, 0.0f));

		auto MakeInstance = [&](const FVertexID VertexID,
			const int32 UE1Vertex, const float CarrierOrdinal)
		{
			const FVertexInstanceID Instance =
				Description.CreateVertexInstance(VertexID);
			Normals[Instance] = FVector3f::UpVector;
			Tangents[Instance] = FVector3f::ForwardVector;
			BinormalSigns[Instance] = 1.0f;
			Colors[Instance] = FVector4f(
				static_cast<float>(UE1Vertex + 1) / 255.0f,
				0.0f, 0.0f, 1.0f);
			UVs.Set(Instance, 0,
				FVector2f(static_cast<float>(UE1Vertex), 0.0f));
			UVs.Set(Instance, 1,
				FVector2f(static_cast<float>(UE1Vertex + 1), CarrierOrdinal));
			return Instance;
		};
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		for (int32 Wedge = 0; Wedge < RepeatedWedgeCount; ++Wedge)
		{
			const TArray<FVertexInstanceID> Triangle = {
				MakeInstance(RepeatedVertices[Wedge], 0, static_cast<float>(Wedge)),
				MakeInstance(B, 1, 0.0f),
				MakeInstance(C, 2, 0.0f)};
			Description.CreateTriangle(Group, Triangle);
		}
		return WoTPayloadMeshTests_BuildCarrierFixture(Description);
	}

	USkeletalMesh* WoTPayloadMeshTests_MakePositionKeyCollisionMesh()
	{
		FMeshDescription Description;
		FSkeletalMeshAttributes Attributes(Description);
		Attributes.Register();
		Description.SetNumUVChannels(2);

		TVertexAttributesRef<FVector3f> Positions =
			Attributes.GetVertexPositions();
		FSkinWeightsVertexAttributesRef SkinWeights =
			Attributes.GetVertexSkinWeights();
		TVertexInstanceAttributesRef<FVector3f> Normals =
			Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents =
			Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns =
			Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors =
			Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs =
			Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(2);

		auto MakeVertex = [&](const FVector3f& Position)
		{
			const FVertexID VertexID = Description.CreateVertex();
			Positions[VertexID] = Position;
			const FBoneIndexType InfluenceBones[1] = {0};
			// Explicit-count Create overload: the fixed-size [MaxInlineBoneWeightCount]
			// overload scans 12 entries until a zero weight and reads past a 1-element
			// array (UB: garbage influences under MSVC; the suite's standing fail).
			const float InfluenceWeights[1] = {1.0f};
			SkinWeights.Set(VertexID, UE::AnimationCore::FBoneWeights::Create(
				InfluenceBones, InfluenceWeights, 1));
			return VertexID;
		};
		const FVertexID Shared = MakeVertex(FVector3f::ZeroVector);
		const FVertexID B = MakeVertex(FVector3f(100.0f, 0.0f, 0.0f));
		const FVertexID C = MakeVertex(FVector3f(0.0f, 100.0f, 0.0f));
		auto MakeInstance = [&](const FVertexID VertexID, const int32 UE1Vertex)
		{
			const FVertexInstanceID Instance =
				Description.CreateVertexInstance(VertexID);
			Normals[Instance] = FVector3f::UpVector;
			Tangents[Instance] = FVector3f::ForwardVector;
			BinormalSigns[Instance] = 1.0f;
			Colors[Instance] = FVector4f(
				static_cast<float>(UE1Vertex + 1) / 255.0f,
				0.0f, 0.0f, 1.0f);
			UVs.Set(Instance, 0, FVector2f::ZeroVector);
			UVs.Set(Instance, 1,
				FVector2f(static_cast<float>(UE1Vertex + 1), 0.0f));
			return Instance;
		};
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		const TArray<FVertexInstanceID> FirstTriangle = {
			MakeInstance(Shared, 0), MakeInstance(B, 2), MakeInstance(C, 3)};
		const TArray<FVertexInstanceID> SecondTriangle = {
			MakeInstance(Shared, 1), MakeInstance(C, 3), MakeInstance(B, 2)};
		Description.CreateTriangle(Group, FirstTriangle);
		Description.CreateTriangle(Group, SecondTriangle);
		return WoTPayloadMeshTests_BuildCarrierFixture(Description);
	}

	USkeletalMesh* WoTPayloadMeshTests_MakeSharedCarrierRegressionMesh()
	{
		FMeshDescription Description;
		FSkeletalMeshAttributes Attributes(Description);
		Attributes.Register();
		Description.SetNumUVChannels(2);

		TVertexAttributesRef<FVector3f> Positions =
			Attributes.GetVertexPositions();
		FSkinWeightsVertexAttributesRef SkinWeights =
			Attributes.GetVertexSkinWeights();
		TVertexInstanceAttributesRef<FVector3f> Normals =
			Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents =
			Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns =
			Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors =
			Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs =
			Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(2);

		const FVector3f FixturePositions[4] = {
			FVector3f(0.0f, 0.0f, 0.0f),
			FVector3f(0.0f, 0.01f, 0.0f),
			FVector3f(100.0f, 0.0f, 0.0f),
			FVector3f(0.0f, 100.0f, 0.0f)};
		const int32 UE1Vertices[4] = {0, 0, 1, 2};
		TArray<FVertexID> Vertices;
		Vertices.Reserve(4);
		for (int32 VertexIndex = 0; VertexIndex < 4; ++VertexIndex)
		{
			const FVertexID VertexID = Description.CreateVertex();
			Vertices.Add(VertexID);
			Positions[VertexID] = FixturePositions[VertexIndex];
			const FBoneIndexType InfluenceBones[1] = {0};
			// Explicit-count Create overload: the fixed-size [MaxInlineBoneWeightCount]
			// overload scans 12 entries until a zero weight and reads past a 1-element
			// array (UB: garbage influences under MSVC; the suite's standing fail).
			const float InfluenceWeights[1] = {1.0f};
			SkinWeights.Set(VertexID, UE::AnimationCore::FBoneWeights::Create(
				InfluenceBones, InfluenceWeights, 1));
		}

		auto MakeInstance = [&](int32 VertexIndex)
		{
			const FVertexInstanceID Instance =
				Description.CreateVertexInstance(Vertices[VertexIndex]);
			const int32 UE1Vertex = UE1Vertices[VertexIndex];
			Normals[Instance] = FVector3f::UpVector;
			Tangents[Instance] = FVector3f::ForwardVector;
			BinormalSigns[Instance] = 1.0f;
			Colors[Instance] = FVector4f(
				static_cast<float>(UE1Vertex + 1) / 255.0f,
				0.0f, 0.0f, 1.0f);
			UVs.Set(Instance, 0, UE1Vertex == 0
				? FVector2f::ZeroVector
				: FVector2f(static_cast<float>(UE1Vertex), 0.0f));
			UVs.Set(Instance, 1,
				FVector2f(static_cast<float>(UE1Vertex + 1), 0.0f));
			return Instance;
		};
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		const TArray<FVertexInstanceID> FirstTriangle = {
			MakeInstance(0), MakeInstance(2), MakeInstance(3)};
		const TArray<FVertexInstanceID> SecondTriangle = {
			MakeInstance(1), MakeInstance(3), MakeInstance(2)};
		Description.CreateTriangle(Group, FirstTriangle);
		Description.CreateTriangle(Group, SecondTriangle);
		return WoTPayloadMeshTests_BuildCarrierFixture(Description);
	}

	USkeletalMesh* WoTPayloadMeshTests_MakeSplitWedgeRegressionMesh()
	{
		FMeshDescription Description;
		FSkeletalMeshAttributes Attributes(Description);
		Attributes.Register();
		Description.SetNumUVChannels(2);

		TVertexAttributesRef<FVector3f> Positions =
			Attributes.GetVertexPositions();
		FSkinWeightsVertexAttributesRef SkinWeights =
			Attributes.GetVertexSkinWeights();
		TVertexInstanceAttributesRef<FVector3f> Normals =
			Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents =
			Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns =
			Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors =
			Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs =
			Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(2);

		const FVector3f FixturePositions[4] = {
			FVector3f(0.0f, 0.0f, 0.0f),
			FVector3f(100.0f, 0.0f, 0.0f),
			FVector3f(0.0f, 100.0f, 0.0f),
			FVector3f(-100.0f, 0.0f, 0.0f)};
		TArray<FVertexID> Vertices;
		Vertices.Reserve(4);
		for (int32 VertexIndex = 0; VertexIndex < 4; ++VertexIndex)
		{
			const FVertexID VertexID = Description.CreateVertex();
			Vertices.Add(VertexID);
			Positions[VertexID] = FixturePositions[VertexIndex];
			const FBoneIndexType InfluenceBones[1] = {0};
			// Explicit-count Create overload: the fixed-size [MaxInlineBoneWeightCount]
			// overload scans 12 entries until a zero weight and reads past a 1-element
			// array (UB: garbage influences under MSVC; the suite's standing fail).
			const float InfluenceWeights[1] = {1.0f};
			SkinWeights.Set(VertexID, UE::AnimationCore::FBoneWeights::Create(
				InfluenceBones, InfluenceWeights, 1));
		}

		const FVector3f SplitNormal(0.0f, 0.005f,
			FMath::Sqrt(1.0f - 0.005f * 0.005f));
		auto MakeInstance = [&](int32 VertexIndex, const FVector3f& Normal)
		{
			const FVertexInstanceID Instance =
				Description.CreateVertexInstance(Vertices[VertexIndex]);
			Normals[Instance] = Normal;
			Tangents[Instance] = FVector3f::ForwardVector;
			BinormalSigns[Instance] = 1.0f;
			Colors[Instance] = FVector4f(
				static_cast<float>(VertexIndex + 1) / 255.0f,
				0.0f, 0.0f, 1.0f);
			UVs.Set(Instance, 0,
				FVector2f(static_cast<float>(VertexIndex), 0.0f));
			UVs.Set(Instance, 1,
				FVector2f(static_cast<float>(VertexIndex + 1), 0.0f));
			return Instance;
		};
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		const TArray<FVertexInstanceID> FirstTriangle = {
			MakeInstance(0, FVector3f::UpVector),
			MakeInstance(1, FVector3f::UpVector),
			MakeInstance(2, FVector3f::UpVector)};
		const TArray<FVertexInstanceID> SecondTriangle = {
			MakeInstance(0, SplitNormal),
			MakeInstance(2, FVector3f::UpVector),
			MakeInstance(3, FVector3f::UpVector)};
		Description.CreateTriangle(Group, FirstTriangle);
		Description.CreateTriangle(Group, SecondTriangle);
		return WoTPayloadMeshTests_BuildCarrierFixture(Description);
	}
#endif
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadMeshManifestSpec,
	"WoT.Payload.Mesh.ManifestAndBlobValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadMeshManifestSpec::RunTest(const FString&)
{
	for (EWoTPayloadMeshTests_Mode Mode : {
		EWoTPayloadMeshTests_Mode::Valid,
		EWoTPayloadMeshTests_Mode::LyingCounts,
		EWoTPayloadMeshTests_Mode::ManifestBlobLengthMismatch,
		EWoTPayloadMeshTests_Mode::LyingBlobHeader})
	{
		FWoTPayloadMeshTests_Fixture Fixture =
			WoTPayloadMeshTests_WriteFixture(Mode);
		FWoTPayloadMeshTests_World State = WoTPayloadMeshTests_CreateWorld(
			Fixture.CacheDirectory, true);
		UWoTPayloadSubsystem* Payloads = State.GameInstance
			? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
		if (TestNotNull(TEXT("mesh payload subsystem exists"), Payloads))
		{
			TestTrue(TEXT("hostile per-entry metadata leaves cache preflight ready"),
				Payloads->IsManifestReady());
			const bool bManifestEntryValid =
				Mode == EWoTPayloadMeshTests_Mode::Valid
				|| Mode == EWoTPayloadMeshTests_Mode::LyingBlobHeader;
			TestEqual(TEXT("manifest mesh entry acceptance"),
				Payloads->FindMeshPayload(Fixture.CacheKey) != nullptr,
				bManifestEntryValid);
			if (bManifestEntryValid)
			{
				TestEqual(TEXT("authenticated mesh blob validation"),
					Payloads->LoadMeshPayload(Fixture.CacheKey).IsValid(),
					Mode == EWoTPayloadMeshTests_Mode::Valid);
			}
		}
		WoTPayloadMeshTests_DestroyWorld(State);
		WoTPayloadMeshTests_DeleteFixture(Fixture);
	}
	return true;
}
#endif // !WOT_GUARDED_BOOTSTRAP

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadMeshCPUDeltaSpec,
	"WoT.Payload.Mesh.CPUDeltasAndPinnedTangent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadMeshCPUDeltaSpec::RunTest(const FString&)
{
	FWoTMeshPayloadData Payload;
	Payload.FrameVertexCount = 3;
	Payload.AnimationFrameCount = 2;
	Payload.PackedVertices = WoTPayloadMeshTests_PackedFrames();
	UWoTMeshPayloadLayout* Layout = WoTPayloadMeshTests_MakeLayout(
		GetTransientPackage(), TEXT("mesh:Fixture__TriMesh"));
	const TArray<FVector3f> BasePositions = {
		FVector3f(0.00f, 0.0f, 0.0f), FVector3f(0.01f, 0.0f, 0.0f),
		FVector3f(0.02f, 0.0f, 0.0f), FVector3f(0.03f, 0.0f, 0.0f)};
	USkeletalMesh* Mesh = WoTPayloadMeshTests_MakeBaseMesh(BasePositions);
	FWoTMeshPayloadCPUData CPU;
	FString Error;
	if (!TestTrue(TEXT("fixture CPU delta build succeeds"),
		UWoTPayloadSubsystem::ComputeMeshPayloadDeltas(
			Payload, *Layout, Mesh, CPU, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("all frames emitted, including F0"), CPU.Frames.Num(), 2);
	TestEqual(TEXT("render mapping expands UE1 vertices"),
		CPU.RenderVertexCount, 4);
	const FVector3f NativeBasePositions[4] = {
		FVector3f(0, 0, 0), FVector3f(10, 0, 0),
		FVector3f(0, 10, 0), FVector3f(0, 0, 0)};
	const FVector3f ExpectedFrame1Positions[4] = {
		FVector3f(0, 0, 0), FVector3f(10, 0, 0),
		FVector3f(0, 10, 10), FVector3f(0, 0, 0)};
	const FVector3f ExpectedNormalDelta(
		0.0f, -UE_INV_SQRT_2, UE_INV_SQRT_2 - 1.0f);
	// Build the equivalent full-payload/native F1 model. Its position deltas
	// are relative to frame 0, while the external F1 deltas are relative to the
	// stripped lattice base; compare the reconstructed positions and TangentZ.
	UMorphTarget* NativeMorph = NewObject<UMorphTarget>(GetTransientPackage());
	FMorphTargetLODModel& NativeLOD =
		NativeMorph->GetMorphLODModels().AddDefaulted_GetRef();
	NativeLOD.NumBaseMeshVerts = 4;
	NativeLOD.NumVertices = 4;
	NativeLOD.SectionIndices.Add(0);
	NativeLOD.Vertices.SetNum(4);
	for (int32 RenderVertex = 0; RenderVertex < 4; ++RenderVertex)
	{
		FMorphTargetDelta& NativeDelta = NativeLOD.Vertices[RenderVertex];
		NativeDelta.PositionDelta =
			ExpectedFrame1Positions[RenderVertex] - NativeBasePositions[RenderVertex];
		NativeDelta.TangentZDelta = ExpectedNormalDelta;
		NativeDelta.SourceIdx = static_cast<uint32>(RenderVertex);
	}
	for (int32 RenderVertex = 0; RenderVertex < 4; ++RenderVertex)
	{
		TestTrue(TEXT("F0 normal delta is relative to pinned base TangentZ"),
			CPU.Frames[0].Deltas[RenderVertex].TangentZDelta.IsNearlyZero(1.e-5f));
		TestTrue(TEXT("F0 external reconstruction matches the native base"),
			(BasePositions[RenderVertex]
				+ CPU.Frames[0].Deltas[RenderVertex].PositionDelta).Equals(
				NativeBasePositions[RenderVertex], 1.e-5f));
		const FMorphTargetDelta& NativeDelta =
			NativeMorph->GetMorphLODModels()[0].Vertices[RenderVertex];
		TestTrue(TEXT("F1 external position reconstructs the native morph result"),
			(BasePositions[RenderVertex]
				+ CPU.Frames[1].Deltas[RenderVertex].PositionDelta).Equals(
				NativeBasePositions[RenderVertex]
					+ NativeDelta.PositionDelta, 1.e-5f));
		const FVector3f ExternalNormal = FVector3f(
			Layout->PinnedBaseTangentZ[RenderVertex])
			+ CPU.Frames[1].Deltas[RenderVertex].TangentZDelta;
		const FVector3f NativeNormal = FVector3f::UpVector
			+ NativeDelta.TangentZDelta;
		TestTrue(TEXT("F1 external TangentZ reconstructs the native morph result"),
			ExternalNormal.Equals(NativeNormal, 1.e-5f));
		TestEqual(TEXT("dense delta source index follows final render vertex"),
			CPU.Frames[1].Deltas[RenderVertex].SourceIndex,
			static_cast<uint32>(RenderVertex));
	}

	FWoTMeshPayloadData UnmirrorPayload;
	UnmirrorPayload.FrameVertexCount = 3;
	UnmirrorPayload.AnimationFrameCount = 1;
	UnmirrorPayload.PackedVertices = {
		WoTPayloadMeshTests_Pack(-1024, 0, 0),
		WoTPayloadMeshTests_Pack(0, 0, 0),
		WoTPayloadMeshTests_Pack(0, 1, 0)};
	UWoTMeshPayloadLayout* UnmirrorLayout = WoTPayloadMeshTests_MakeLayout(
		GetTransientPackage(), TEXT("mesh:Fixture__Unmirror"));
	UnmirrorLayout->AnimationFrameCount = 1;
	UnmirrorLayout->bUnmirror = true;
	UnmirrorLayout->RenderVertexToUE1Vertex = {0, 1, 2};
	UnmirrorLayout->PinnedBaseTangentZ = {
		FVector::UpVector, FVector::UpVector, FVector::UpVector};
	UnmirrorLayout->PinnedBaseTangentZSHA256 =
		UWoTMeshPayloadLayout::HashTangentBasis(
			UnmirrorLayout->PinnedBaseTangentZ);
	FWoTMeshPayloadCPUData UnmirrorCPU;
	USkeletalMesh* UnmirrorMesh = WoTPayloadMeshTests_MakeBaseMesh({
		FVector3f(0.00f, 0.0f, 0.0f), FVector3f(0.01f, 0.0f, 0.0f),
		FVector3f(0.02f, 0.0f, 0.0f)});
	Error.Reset();
	TestTrue(TEXT("UNMIRROR packed-minimum fixture builds"),
		UWoTPayloadSubsystem::ComputeMeshPayloadDeltas(
			UnmirrorPayload, *UnmirrorLayout, UnmirrorMesh, UnmirrorCPU, Error));
	if (!UnmirrorCPU.Frames.IsEmpty())
	{
		TestTrue(TEXT("UNMIRROR preserves 11-bit -1024 wrap semantics"),
			UnmirrorCPU.Frames[0].Deltas[0].PositionDelta.Equals(
				FVector3f(-1024, 0, 0), 1.e-5f));
	}

	FWoTMeshPayloadData TransformPayload;
	TransformPayload.FrameVertexCount = 3;
	TransformPayload.AnimationFrameCount = 1;
	TransformPayload.PackedVertices = {
		WoTPayloadMeshTests_Pack(2, 4, 6),
		WoTPayloadMeshTests_Pack(3, 4, 6),
		WoTPayloadMeshTests_Pack(2, 5, 6)};
	UWoTMeshPayloadLayout* TransformLayout = WoTPayloadMeshTests_MakeLayout(
		GetTransientPackage(), TEXT("mesh:Fixture__Transform"));
	TransformLayout->Origin = FVector(1, 2, 3);
	TransformLayout->Scale = FVector(2, 3, 4);
	TransformLayout->RotationUnits = FVector(64, 64, 64);
	TransformLayout->AnimationFrameCount = 1;
	TransformLayout->RenderVertexToUE1Vertex = {0, 1, 2};
	TransformLayout->PinnedBaseTangentZ = {
		FVector(-1, 0, 0), FVector(-1, 0, 0), FVector(-1, 0, 0)};
	TransformLayout->PinnedBaseTangentZSHA256 =
		UWoTMeshPayloadLayout::HashTangentBasis(
			TransformLayout->PinnedBaseTangentZ);
	FWoTMeshPayloadCPUData TransformCPU;
	USkeletalMesh* TransformMesh = WoTPayloadMeshTests_MakeBaseMesh({
		FVector3f(0.00f, 0.0f, 0.0f), FVector3f(0.01f, 0.0f, 0.0f),
		FVector3f(0.02f, 0.0f, 0.0f)});
	Error.Reset();
	TestTrue(TEXT("compound #exec transform fixture builds"),
		UWoTPayloadSubsystem::ComputeMeshPayloadDeltas(
			TransformPayload, *TransformLayout, TransformMesh, TransformCPU, Error));
	if (!TransformCPU.Frames.IsEmpty())
	{
		TestTrue(TEXT("CPU transform applies origin, scale, roll, pitch, yaw"),
			TransformCPU.Frames[0].Deltas[0].PositionDelta.Equals(
				FVector3f(-12, 6, 2), 1.e-5f));
		TestTrue(TEXT("compound transform normal remains relative to its pin"),
			TransformCPU.Frames[0].Deltas[0].TangentZDelta.IsNearlyZero(1.e-5f));
	}

	FWoTMeshPayloadCPUData CountRejected;
	Error.Reset();
	USkeletalMesh* WrongCountMesh = WoTPayloadMeshTests_MakeBaseMesh({
		FVector3f(0.00f, 0.0f, 0.0f), FVector3f(0.01f, 0.0f, 0.0f),
		FVector3f(0.02f, 0.0f, 0.0f)});
	TestFalse(TEXT("base buffer count mismatch fails closed"),
		UWoTPayloadSubsystem::ComputeMeshPayloadDeltas(
			Payload, *Layout, WrongCountMesh, CountRejected, Error));
	TestTrue(TEXT("base buffer count rejection identifies the render mapping"),
		Error.Contains(TEXT("base position count")));

	Layout->PinnedBaseTangentZ[0] = FVector::ForwardVector;
	FWoTMeshPayloadCPUData Rejected;
	Error.Reset();
	TestFalse(TEXT("mutated pinned basis hash rejects before delta publication"),
		UWoTPayloadSubsystem::ComputeMeshPayloadDeltas(
			Payload, *Layout, Mesh, Rejected, Error));
	TestTrue(TEXT("pin rejection reports the TangentZ hash"),
		Error.Contains(TEXT("TangentZ SHA-256")));
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadMeshLatticeWeldSpec,
	"WoT.Payload.Mesh.LatticeBasePreventsWeld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadMeshLatticeWeldSpec::RunTest(const FString&)
{
	USkeletalMesh* Mesh = WoTPayloadMeshTests_MakeWeldRegressionMesh();
	if (!TestNotNull(TEXT("carrier-bearing weld fixture builds"), Mesh))
	{
		return false;
	}
	const FSkeletalMeshRenderData* BeforeRenderData =
		Mesh->GetResourceForRendering();
	if (!TestTrue(TEXT("carrier-bearing fixture has LOD-0 render data"),
		BeforeRenderData && BeforeRenderData->LODRenderData.IsValidIndex(0)))
	{
		return false;
	}
	const int32 VerifiedRenderVertexCount = static_cast<int32>(
		BeforeRenderData->LODRenderData[0].StaticVertexBuffers
			.PositionVertexBuffer.GetNumVertices());
	TestEqual(TEXT("canonical fixture keeps the four description vertices"),
		VerifiedRenderVertexCount, 4);

	// The quad's two triangles, by UE1 vertex per corner: T1 = {1,2,0} (wedge A0),
	// T2 = {1,0,2} (wedge A1). BasePositions/BaseNormals are indexed by UE1 vertex
	// zero..two (FrameVertexCount == 3) and must match the fixture's canonical
	// per-UE1 positions: UE1 0 -> (0,100,0), UE1 1 -> (0,0,0), UE1 2 -> (100,0,0).
	const TArray<int32> Triangles = {1, 2, 0, 1, 0, 2};
	const TArray<FVector> BasePositions = {
		FVector(0.0, 100.0, 0.0), FVector(0.0, 0.0, 0.0),
		FVector(100.0, 0.0, 0.0)};
	const TArray<FVector> BaseNormals = {
		FVector::UpVector, FVector::UpVector, FVector::UpVector};
	const FString Error = UWoTMeshPayloadLibrary::CaptureAndOptionallyStrip(
		Mesh, TEXT("mesh:WeldRegression__Fixture"), FVector::ZeroVector,
		FVector::OneVector, FVector::ZeroVector, false, 3, 1,
		Triangles, Triangles, BasePositions, BaseNormals, true);
	if (!TestTrue(TEXT("production strip/rebuild path accepts lattice fixture"),
		Error.IsEmpty()))
	{
		AddError(Error);
		return false;
	}

	const UWoTMeshPayloadLayout* Layout =
		Mesh->GetAssetUserData<UWoTMeshPayloadLayout>();
	const FSkeletalMeshRenderData* AfterRenderData =
		Mesh->GetResourceForRendering();
	if (!TestNotNull(TEXT("strip path publishes the verified layout"), Layout)
		|| !TestTrue(TEXT("stripped fixture retains LOD-0 render data"),
			AfterRenderData && AfterRenderData->LODRenderData.IsValidIndex(0)))
	{
		return false;
	}
	const int32 RebuiltRenderVertexCount = static_cast<int32>(
		AfterRenderData->LODRenderData[0].StaticVertexBuffers
			.PositionVertexBuffer.GetNumVertices());
	TestEqual(TEXT("lattice strip prevents post-carrier vertex welding"),
		RebuiltRenderVertexCount, VerifiedRenderVertexCount);
	TestEqual(TEXT("rebuilt render count equals the verified mapping count"),
		RebuiltRenderVertexCount, Layout->RenderVertexToUE1Vertex.Num());
	const FSkeletalMeshLODRenderData& RebuiltLOD =
		AfterRenderData->LODRenderData[0];
	int32 RebuiltTriangleCount = 0;
	for (const FSkelMeshRenderSection& Section : RebuiltLOD.RenderSections)
	{
		RebuiltTriangleCount += static_cast<int32>(Section.NumTriangles);
	}
	TestEqual(TEXT("both carrier-split triangles survive exactly"),
		RebuiltTriangleCount, 2);
	TArray<uint32> RebuiltIndices;
	RebuiltLOD.MultiSizeIndexContainer.GetIndexBuffer(RebuiltIndices);
	TArray<int32> RebuiltMappedCorners;
	for (const FSkelMeshRenderSection& Section : RebuiltLOD.RenderSections)
	{
		const int32 BaseIndex = static_cast<int32>(Section.BaseIndex);
		const int32 CornerCount = static_cast<int32>(Section.NumTriangles) * 3;
		for (int32 Offset = 0; Offset < CornerCount; ++Offset)
		{
			if (!TestTrue(TEXT("rebuilt corner index is in range"),
				RebuiltIndices.IsValidIndex(BaseIndex + Offset)))
			{
				continue;
			}
			const int32 RenderVertex = static_cast<int32>(
				RebuiltIndices[BaseIndex + Offset]);
			if (TestTrue(TEXT("rebuilt corner maps to a render vertex"),
				Layout->RenderVertexToUE1Vertex.IsValidIndex(RenderVertex)))
			{
				RebuiltMappedCorners.Add(
					Layout->RenderVertexToUE1Vertex[RenderVertex]);
			}
		}
	}
	RebuiltMappedCorners.Sort();
	// T1 {1,2,0} + T2 {1,0,2} sorted across both triangles.
	const TArray<int32> ExpectedMappedCorners = {0, 0, 1, 1, 2, 2};
	TestEqual(TEXT("rebuilt triangles cover six corners"),
		RebuiltMappedCorners.Num(), 6);
	if (RebuiltMappedCorners.Num() == 6)
	{
		for (int32 Corner = 0; Corner < 6; ++Corner)
		{
			TestEqual(TEXT("rebuilt corner UE1 multiset matches"),
				RebuiltMappedCorners[Corner], ExpectedMappedCorners[Corner]);
		}
	}
	TArray<FVector3f> RepeatedUE1Positions;
	const FPositionVertexBuffer& RebuiltPositions =
		RebuiltLOD.StaticVertexBuffers.PositionVertexBuffer;
	for (int32 RenderVertex = 0;
		RenderVertex < Layout->RenderVertexToUE1Vertex.Num(); ++RenderVertex)
	{
		if (Layout->RenderVertexToUE1Vertex[RenderVertex] == 1)
		{
			RepeatedUE1Positions.Add(
				RebuiltPositions.VertexPosition(static_cast<uint32>(RenderVertex)));
		}
	}
	TestEqual(TEXT("both description wedges for UE1 vertex one survive"),
		RepeatedUE1Positions.Num(), 2);
	if (RepeatedUE1Positions.Num() == 2)
	{
		TestTrue(TEXT("same-UE1 wedges have distinct one-UU lattice positions"),
			(RepeatedUE1Positions[0] - RepeatedUE1Positions[1]).Length()
				>= 1.0f);
	}
	const auto ValidateStrippedMeshLayout =
		WoTPayloadMeshTests_GetPrivateMember(
			FWoTPayloadMeshTests_ValidateMeshTag());
	FString RuntimeError;
	TestTrue(TEXT("runtime gate accepts every rebuilt lattice position"),
		ValidateStrippedMeshLayout(Mesh, *Layout, RuntimeError));
	FSkeletalMeshRenderData* MutableRenderData = Mesh->GetResourceForRendering();
	FVector3f& LeakingPosition = MutableRenderData->LODRenderData[0]
		.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(0);
	const FVector3f SavedPosition = LeakingPosition;
	LeakingPosition.X += 0.005f;
	RuntimeError.Reset();
	TestFalse(TEXT("runtime gate rejects a non-lattice retail position"),
		ValidateStrippedMeshLayout(Mesh, *Layout, RuntimeError));
	TestTrue(TEXT("runtime lattice rejection identifies the base contract"),
		RuntimeError.Contains(TEXT("lattice-base")));
	LeakingPosition = SavedPosition;

	USkeletalMesh* ManyWedgeMesh =
		WoTPayloadMeshTests_MakeManyWedgeRegressionMesh();
	if (!TestNotNull(TEXT("many-wedge fixture builds"), ManyWedgeMesh))
	{
		return false;
	}
	constexpr int32 ManyWedgeCount = 12;
	TArray<int32> ManyWedgeTriangles;
	for (int32 Triangle = 0; Triangle < ManyWedgeCount; ++Triangle)
	{
		ManyWedgeTriangles.Append({0, 1, 2});
	}
	const TArray<FVector> ManyWedgeBasePositions = {
		FVector(0.0, 0.0, 0.0), FVector(100.0, 0.0, 0.0),
		FVector(0.0, 100.0, 0.0)};
	const FString ManyWedgeError =
		UWoTMeshPayloadLibrary::CaptureAndOptionallyStrip(
			ManyWedgeMesh, TEXT("mesh:ManyWedgeRegression__Fixture"),
			FVector::ZeroVector, FVector::OneVector, FVector::ZeroVector,
			false, 3, 1, ManyWedgeTriangles, ManyWedgeTriangles,
			ManyWedgeBasePositions, BaseNormals, true);
	if (!TestTrue(TEXT("twelve wedges for one UE1 vertex strip without a budget"),
		ManyWedgeError.IsEmpty()))
	{
		AddError(ManyWedgeError);
		return false;
	}
	const UWoTMeshPayloadLayout* ManyWedgeLayout =
		ManyWedgeMesh->GetAssetUserData<UWoTMeshPayloadLayout>();
	const FSkeletalMeshRenderData* ManyWedgeRenderData =
		ManyWedgeMesh->GetResourceForRendering();
	if (!TestNotNull(TEXT("many-wedge strip publishes a layout"), ManyWedgeLayout)
		|| !TestTrue(TEXT("many-wedge strip has final LOD-0 data"),
			ManyWedgeRenderData
				&& ManyWedgeRenderData->LODRenderData.IsValidIndex(0)))
	{
		return false;
	}
	const FSkeletalMeshLODRenderData& ManyWedgeLOD =
		ManyWedgeRenderData->LODRenderData[0];
	int32 ManyWedgeTriangleCount = 0;
	for (const FSkelMeshRenderSection& Section : ManyWedgeLOD.RenderSections)
	{
		ManyWedgeTriangleCount += static_cast<int32>(Section.NumTriangles);
	}
	TestEqual(TEXT("every many-wedge triangle survives carrier removal"),
		ManyWedgeTriangleCount, ManyWedgeCount);
	TArray<FVector3f> ManyWedgePositions;
	const FPositionVertexBuffer& ManyWedgePositionBuffer =
		ManyWedgeLOD.StaticVertexBuffers.PositionVertexBuffer;
	for (int32 RenderVertex = 0;
		RenderVertex < ManyWedgeLayout->RenderVertexToUE1Vertex.Num();
		++RenderVertex)
	{
		if (ManyWedgeLayout->RenderVertexToUE1Vertex[RenderVertex] == 0)
		{
			ManyWedgePositions.Add(ManyWedgePositionBuffer.VertexPosition(
				static_cast<uint32>(RenderVertex)));
		}
	}
	TestEqual(TEXT("all twelve same-UE1 wedge positions survive"),
		ManyWedgePositions.Num(), ManyWedgeCount);
	bool bManyWedgePositionsDistinct = true;
	for (int32 A = 0; A < ManyWedgePositions.Num(); ++A)
	{
		for (int32 B = A + 1; B < ManyWedgePositions.Num(); ++B)
		{
			bManyWedgePositionsDistinct &=
				(ManyWedgePositions[A] - ManyWedgePositions[B]).Length() >= 1.0f;
		}
	}
	TestTrue(TEXT("many-wedge lattice points remain pairwise weld-safe"),
		bManyWedgePositionsDistinct);
	RuntimeError.Reset();
	TestTrue(TEXT("runtime gate accepts the many-wedge final mesh"),
		ValidateStrippedMeshLayout(
			ManyWedgeMesh, *ManyWedgeLayout, RuntimeError));

	USkeletalMesh* CollisionMesh =
		WoTPayloadMeshTests_MakePositionKeyCollisionMesh();
	if (!TestNotNull(TEXT("position-key collision fixture builds"), CollisionMesh))
	{
		return false;
	}
	const TArray<int32> CollisionTriangles = {0, 2, 3, 1, 3, 2};
	const TArray<FVector> CollisionBasePositions = {
		FVector::ZeroVector, FVector::ZeroVector,
		FVector(100.0, 0.0, 0.0), FVector(0.0, 100.0, 0.0)};
	const TArray<FVector> CollisionBaseNormals = {
		FVector::UpVector, FVector::UpVector,
		FVector::UpVector, FVector::UpVector};
	const FString CollisionError =
		UWoTMeshPayloadLibrary::CaptureAndOptionallyStrip(
			CollisionMesh, TEXT("mesh:PositionKeyCollision__Fixture"),
			FVector::ZeroVector, FVector::OneVector, FVector::ZeroVector,
			false, 4, 1, CollisionTriangles, CollisionTriangles,
			CollisionBasePositions, CollisionBaseNormals, true);
	TestTrue(TEXT("different UE1 carriers at one phase-1 position fail closed"),
		CollisionError.Contains(TEXT("phase-1 position key collision")));

	USkeletalMesh* SharedCarrierMesh =
		WoTPayloadMeshTests_MakeSharedCarrierRegressionMesh();
	if (!TestNotNull(TEXT("shared-carrier description fixture builds"),
		SharedCarrierMesh))
	{
		return false;
	}
	const FSkeletalMeshRenderData* SharedBefore =
		SharedCarrierMesh->GetResourceForRendering();
	if (!TestTrue(TEXT("shared-carrier fixture has canonical LOD-0 data"),
		SharedBefore && SharedBefore->LODRenderData.IsValidIndex(0)))
	{
		return false;
	}
	const FSkeletalMeshLODRenderData& SharedBeforeLOD =
		SharedBefore->LODRenderData[0];
	int32 SharedCanonicalUE1ZeroCount = 0;
	const FColorVertexBuffer& SharedCanonicalColors =
		SharedBeforeLOD.StaticVertexBuffers.ColorVertexBuffer;
	for (uint32 RenderVertex = 0;
		RenderVertex < SharedCanonicalColors.GetNumVertices(); ++RenderVertex)
	{
		if (SharedCanonicalColors.VertexColor(RenderVertex).R == 1)
		{
			++SharedCanonicalUE1ZeroCount;
		}
	}
	TestTrue(TEXT("two description vertices share the UE1-zero carrier"),
		SharedCanonicalUE1ZeroCount >= 2);

	const TArray<int32> SharedTriangles = {0, 1, 2, 0, 2, 1};
	const TArray<FVector> SharedBasePositions = {
		FVector(0.0, 0.0, 0.0), FVector(100.0, 0.0, 0.0),
		FVector(0.0, 100.0, 0.0)};
	const TArray<FVector> SharedBaseNormals = {
		FVector::UpVector, FVector::UpVector, FVector::UpVector};
	const FString SharedError =
		UWoTMeshPayloadLibrary::CaptureAndOptionallyStrip(
			SharedCarrierMesh, TEXT("mesh:SharedCarrierRegression__Fixture"),
			FVector::ZeroVector, FVector::OneVector, FVector::ZeroVector,
			false, 3, 1, SharedTriangles, SharedTriangles,
			SharedBasePositions, SharedBaseNormals, true);
	if (!TestTrue(TEXT("shared-carrier strip and triangle mapping verification pass"),
		SharedError.IsEmpty()))
	{
		AddError(SharedError);
		return false;
	}
	const UWoTMeshPayloadLayout* SharedLayout =
		SharedCarrierMesh->GetAssetUserData<UWoTMeshPayloadLayout>();
	const FSkeletalMeshRenderData* SharedAfter =
		SharedCarrierMesh->GetResourceForRendering();
	if (!TestNotNull(TEXT("shared-carrier strip publishes a layout"), SharedLayout)
		|| !TestTrue(TEXT("shared-carrier strip has final LOD-0 data"),
			SharedAfter && SharedAfter->LODRenderData.IsValidIndex(0)))
	{
		return false;
	}
	const int32 SharedFinalCount = static_cast<int32>(
		SharedAfter->LODRenderData[0].StaticVertexBuffers
			.PositionVertexBuffer.GetNumVertices());
	TestEqual(TEXT("position-key transfer covers every final render vertex"),
		SharedLayout->RenderVertexToUE1Vertex.Num(), SharedFinalCount);
	bool bSharedMappingValid = true;
	for (int32 UE1Vertex : SharedLayout->RenderVertexToUE1Vertex)
	{
		bSharedMappingValid &= UE1Vertex >= 0 && UE1Vertex < 3;
	}
	TestTrue(TEXT("every shared-carrier stripped mapping entry is valid"),
		bSharedMappingValid);
	RuntimeError.Reset();
	TestTrue(TEXT("runtime gate accepts the shared-carrier final mesh"),
		ValidateStrippedMeshLayout(
			SharedCarrierMesh, *SharedLayout, RuntimeError));

	USkeletalMesh* SplitWedgeMesh =
		WoTPayloadMeshTests_MakeSplitWedgeRegressionMesh();
	if (!TestNotNull(TEXT("split-normal wedge fixture builds"), SplitWedgeMesh))
	{
		return false;
	}
	const FSkeletalMeshRenderData* SplitBefore =
		SplitWedgeMesh->GetResourceForRendering();
	if (!TestTrue(TEXT("split-normal fixture has canonical LOD-0 data"),
		SplitBefore && SplitBefore->LODRenderData.IsValidIndex(0)))
	{
		return false;
	}
	int32 CanonicalSplitUE1ZeroCount = 0;
	const FColorVertexBuffer& SplitCanonicalColors = SplitBefore->LODRenderData[0]
		.StaticVertexBuffers.ColorVertexBuffer;
	for (uint32 RenderVertex = 0;
		RenderVertex < SplitCanonicalColors.GetNumVertices(); ++RenderVertex)
	{
		if (SplitCanonicalColors.VertexColor(RenderVertex).R == 1)
		{
			++CanonicalSplitUE1ZeroCount;
		}
	}
	TestTrue(TEXT("distinct normals split UE1 zero into canonical render wedges"),
		CanonicalSplitUE1ZeroCount >= 2);

	const TArray<int32> SplitTriangles = {0, 1, 2, 0, 2, 3};
	const TArray<FVector> SplitBasePositions = {
		FVector(0.0, 0.0, 0.0), FVector(100.0, 0.0, 0.0),
		FVector(0.0, 100.0, 0.0), FVector(-100.0, 0.0, 0.0)};
	const TArray<FVector> SplitBaseNormals = {
		FVector::UpVector, FVector::UpVector,
		FVector::UpVector, FVector::UpVector};
	const FString SplitError =
		UWoTMeshPayloadLibrary::CaptureAndOptionallyStrip(
			SplitWedgeMesh, TEXT("mesh:SplitWedgeRegression__Fixture"),
			FVector::ZeroVector, FVector::OneVector, FVector::ZeroVector,
			false, 4, 1, SplitTriangles, SplitTriangles,
			SplitBasePositions, SplitBaseNormals, true);
	if (!TestTrue(TEXT("split-wedge strip and triangle mapping verification pass"),
		SplitError.IsEmpty()))
	{
		AddError(SplitError);
		return false;
	}
	const UWoTMeshPayloadLayout* SplitLayout =
		SplitWedgeMesh->GetAssetUserData<UWoTMeshPayloadLayout>();
	const FSkeletalMeshRenderData* SplitAfter =
		SplitWedgeMesh->GetResourceForRendering();
	if (!TestNotNull(TEXT("split-wedge strip publishes a layout"), SplitLayout)
		|| !TestTrue(TEXT("split-wedge strip has final LOD-0 data"),
			SplitAfter && SplitAfter->LODRenderData.IsValidIndex(0)))
	{
		return false;
	}
	const int32 SplitFinalCount = static_cast<int32>(
		SplitAfter->LODRenderData[0].StaticVertexBuffers
			.PositionVertexBuffer.GetNumVertices());
	TestEqual(TEXT("split-wedge mapping count survives the carrier-drop rebuild"),
		SplitLayout->RenderVertexToUE1Vertex.Num(), SplitFinalCount);
	int32 StrippedSplitUE1ZeroCount = 0;
	bool bSplitMappingValid = true;
	for (int32 UE1Vertex : SplitLayout->RenderVertexToUE1Vertex)
	{
		bSplitMappingValid &= UE1Vertex >= 0 && UE1Vertex < 4;
		if (UE1Vertex == 0)
		{
			++StrippedSplitUE1ZeroCount;
		}
	}
	TestTrue(TEXT("every split-wedge stripped mapping entry is valid"),
		bSplitMappingValid);
	TestTrue(TEXT("the UE1-zero normal split survives in the stripped mapping"),
		StrippedSplitUE1ZeroCount >= 2);
	RuntimeError.Reset();
	TestTrue(TEXT("runtime gate accepts the split-wedge final mesh"),
		ValidateStrippedMeshLayout(SplitWedgeMesh, *SplitLayout, RuntimeError));
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadMeshMappingAgreementSpec,
	"WoT.Payload.Mesh.DualCarrierMappingAgreement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadMeshMappingAgreementSpec::RunTest(const FString&)
{
	const TArray<int32> ColorCarrier = {0, 1, 2, 0};
	const TArray<int32> MatchingUVCarrier = {0, 1, 2, 0};
	const TArray<int32> SwappedUVCarrier = {1, 0, 2, 0};
	FString Error;
	TestTrue(TEXT("element-wise identical COLOR_0/TEXCOORD_1 mapping is accepted"),
		UWoTMeshPayloadLayout::ValidateRenderVertexMappingAgreement(
			ColorCarrier, MatchingUVCarrier, Error));
	Error.Reset();
	TestFalse(TEXT("a TEXCOORD_1 vertex swap is rejected even when counts agree"),
		UWoTMeshPayloadLayout::ValidateRenderVertexMappingAgreement(
			ColorCarrier, SwappedUVCarrier, Error));
	TestTrue(TEXT("mapping rejection identifies the first differing vertex"),
		Error.Contains(TEXT("vertex 0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadMeshMorphSeamSpec,
	"WoT.Payload.Mesh.ComponentMorphAccessSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadMeshMorphSeamSpec::RunTest(const FString&)
{
	FWoTPayloadMeshTests_World State = WoTPayloadMeshTests_CreateWorld(
		FString(), false);
	ON_SCOPE_EXIT
	{
		WoTPayloadMeshTests_DestroyWorld(State);
	};
	if (!TestNotNull(TEXT("morph seam test world exists"), State.World))
	{
		return false;
	}

	AActor* Owner = State.World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("morph seam component owner exists"), Owner))
	{
		return false;
	}
	UWoTVertexAnimComponent* Component =
		NewObject<UWoTVertexAnimComponent>(
			Owner, TEXT("WoTPayloadMeshTests_MorphSeamComponent"));
	Owner->AddInstanceComponent(Component);
	Owner->SetRootComponent(Component);
	Component->RegisterComponent();
	if (!TestTrue(TEXT("morph seam component is registered"),
		Component->IsRegistered()))
	{
		return false;
	}
	auto Access = WoTPayloadMeshTests_GetPrivateMember(
		FWoTPayloadMeshTests_AccessMorphTag());
	auto Install = WoTPayloadMeshTests_GetPrivateMember(
		FWoTPayloadMeshTests_InstallMorphTag());
	auto SetFrames = WoTPayloadMeshTests_GetPrivateMember(
		FWoTPayloadMeshTests_SetFrameMorphsTag());
	auto ClearExternal = WoTPayloadMeshTests_GetPrivateMember(
		FWoTPayloadMeshTests_ClearExternalTag());

	TestEqual(TEXT("injection OFF idle has no explicit F0 morph"),
		(Component->*Access)(0, TOptional<float>()), 0.0f);
	(Component->*Access)(1, TOptional<float>(0.25f));
	TestEqual(TEXT("injection OFF writes the native named morph"),
		Component->GetMorphTarget(FName(TEXT("F1"))), 0.25f);

	TSharedRef<FExternalMorphSet> HeadlessSet = MakeShared<FExternalMorphSet>();
	HeadlessSet->Name = TEXT("HeadlessMeshSeam");
	TestTrue(TEXT("headless external-weight seam installs without GPU resources"),
		(Component->*Install)(HeadlessSet, 3));
	TestEqual(TEXT("idle external install publishes the F0 rest pose"),
		(Component->*Access)(0, TOptional<float>()), 1.0f);
	TestEqual(TEXT("idle external install leaves F1 inactive"),
		(Component->*Access)(1, TOptional<float>()), 0.0f);
	TestEqual(TEXT("idle external install leaves F2 inactive"),
		(Component->*Access)(2, TOptional<float>()), 0.0f);
	Component->SetMorphTarget(FName(TEXT("F1")), 0.75f);
	(Component->*Access)(1, TOptional<float>(0.6f));
	TestEqual(TEXT("injection ON reads the external-set weight"),
		(Component->*Access)(1, TOptional<float>()), 0.6f);
	TestEqual(TEXT("injection ON never writes the native named morph"),
		Component->GetMorphTarget(FName(TEXT("F1"))), 0.75f);

	(Component->*SetFrames)(1, 0.6f, 0, 0.4f);
	// Explicit tolerance: F0 = 1 - Σweights accumulates float error (1.0f-0.6f
	// = 0.39999998), which exceeds IsNearlyEqual's 1e-8 SMALL_NUMBER default.
	TestTrue(TEXT("SetFrameMorphs makes the native base remainder explicit"),
		FMath::IsNearlyEqual(
			(Component->*Access)(0, TOptional<float>()), 0.4f, KINDA_SMALL_NUMBER));
	FWoTAnimSequence& Target = Component->Sequences.AddDefaulted_GetRef();
	Target.Name = TEXT("Target");
	Target.StartFrame = 2;
	Target.NumFrames = 1;
	Target.Rate = 1.0f;
	(Component->*Access)(0, TOptional<float>(0.9f));
	Component->TweenAnim(TEXT("Target"), 1.0f);
	TestTrue(TEXT("TweenAnim capture reconstructs the native base remainder"),
		FMath::IsNearlyEqual(
			(Component->*Access)(0, TOptional<float>()), 0.4f, KINDA_SMALL_NUMBER));
	Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("TweenAnim capture reads through the external seam"),
		FMath::IsNearlyEqual(
			(Component->*Access)(1, TOptional<float>()), 0.3f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("external tween reconstructs native's implicit base weight"),
		FMath::IsNearlyEqual(
			(Component->*Access)(0, TOptional<float>()), 0.2f, KINDA_SMALL_NUMBER));
	TestTrue(TEXT("tween tick writes target frame through the external seam"),
		FMath::IsNearlyEqual(
			(Component->*Access)(2, TOptional<float>()), 0.5f, KINDA_SMALL_NUMBER));

	(Component->*ClearExternal)();
	(Component->*Access)(2, TOptional<float>(0.2f));
	TestEqual(TEXT("clearing injection restores unchanged native routing"),
		Component->GetMorphTarget(FName(TEXT("F2"))), 0.2f);
	return true;
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTPayloadMeshPackageHandlerSpec,
	"WoT.Payload.Mesh.PackageHandlerAndToggleOff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTPayloadMeshPackageHandlerSpec::RunTest(const FString&)
{
	FWoTPayloadMeshTests_Fixture Fixture = WoTPayloadMeshTests_WriteFixture(
		EWoTPayloadMeshTests_Mode::Valid);
	FWoTPayloadMeshTests_World State = WoTPayloadMeshTests_CreateWorld(
		Fixture.CacheDirectory, true);
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Game/__WoTPayloadMeshTest_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(
		Package, TEXT("LateMesh"), RF_Public | RF_Standalone);
	Mesh->AddAssetUserData(
		WoTPayloadMeshTests_MakeLayout(Mesh, Fixture.CacheKey));
	ON_SCOPE_EXIT
	{
		WoTPayloadMeshTests_DestroyWorld(State);
		Mesh->ClearFlags(RF_Public | RF_Standalone);
		Mesh->MarkAsGarbage();
		ResetLoaders(Package);
		Package->MarkAsGarbage();
		WoTPayloadMeshTests_DeleteFixture(Fixture);
	};

	UWoTPayloadSubsystem* Payloads = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("mesh handler subsystem exists"), Payloads))
	{
		return false;
	}
	auto Handler = WoTPayloadMeshTests_GetPrivateMember(
		FWoTPayloadMeshTests_PackageHandlerTag());
	(Payloads->*Handler)(Package);
	// The handler authenticates and computes CPU deltas, then correctly refuses
	// GPU publication because this in-memory mesh has no cooked LOD render data.
	IFileManager::Get().Delete(*FPaths::Combine(
		Fixture.CacheDirectory, Fixture.BlobPath));
	TestTrue(TEXT("real package handler retained the authenticated mesh payload"),
		Payloads->LoadMeshPayload(Fixture.CacheKey).IsValid());

	WoTPayloadMeshTests_DestroyWorld(State);
	State = WoTPayloadMeshTests_CreateWorld(Fixture.CacheDirectory, false);
	Payloads = State.GameInstance
		? State.GameInstance->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	TestNotNull(TEXT("OFF mesh subsystem exists"), Payloads);
	if (Payloads)
	{
		(Payloads->*Handler)(Package);
		TestFalse(TEXT("toggle OFF has zero mesh-cache effect"),
			Payloads->LoadMeshPayload(Fixture.CacheKey).IsValid());
	}
	return true;
}
#endif // !WOT_GUARDED_BOOTSTRAP

#endif // WITH_AUTOMATION_TESTS
