#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Mesh/WoTRuntimeVertexMesh.h"
#include "../Payload/WoTNeutralResources.h"
#include "../World/WoTRuntimeActorVisuals.h"
#include "../WoTMapLightTable.h"
#include "../WoTCharacter.h"
#include "../WoTMeshLight.h"
#include "../WoTMeshFX.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTFireballProjectile.h"
#include "../Projectiles/WoTTracerProjectile.h"
#include "../Projectiles/WoTWallOfAirProjectile.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWhirlwindFunnel.h"
#include "WoTRuntimeVertexMeshTestTypes.h"
#include "WoTTestWorld.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "RHI.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "RenderingThread.h"
#include "WoTRetailReader/MeshAnimation.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

namespace
{
constexpr EAutomationTestFlags WoTRuntimeVertexMeshFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
	| EAutomationTestFlags::ProductFilter;

FString RuntimeVertexMeshRepoPath(const TCHAR* Relative)
{
	return WoTRepositoryOrReferencePath(Relative);
}

bool LoadRuntimeVertexMeshFixture(const TCHAR* Name, TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *RuntimeVertexMeshRepoPath(
		*(FString(TEXT("Tools/dist/fixtures/")) + Name)));
}

FWoTRuntimeVertexMeshDataPtr LoadPlainRuntimeVertexMesh(
	FAutomationTestBase& Test)
{
	TArray<uint8> MeshBytes;
	TArray<uint8> AnimationBytes;
	if (!Test.TestTrue(TEXT("plain mesh fixture loads"),
		LoadRuntimeVertexMeshFixture(TEXT("cache-v3-mesh-plain.bin"), MeshBytes))
		|| !Test.TestTrue(TEXT("plain animation fixture loads"),
			LoadRuntimeVertexMeshFixture(
				TEXT("cache-v3-animation-plain.bin"), AnimationBytes)))
	{
		return nullptr;
	}
	auto Decoded = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
		MoveTemp(MeshBytes), MoveTemp(AnimationBytes),
		TEXT("cache/mesh/mesh__PlainMesh"),
		TEXT("cache/animation/mesh__PlainMesh"));
	if (Decoded.IsError())
	{
		Test.AddError(Decoded.GetError().ToString());
		return nullptr;
	}
	return MoveTemp(Decoded).GetValue();
}

uint32 ReadU32(TConstArrayView<uint8> Bytes, int32 Offset)
{
	return static_cast<uint32>(Bytes[Offset])
		| static_cast<uint32>(Bytes[Offset + 1]) << 8
		| static_cast<uint32>(Bytes[Offset + 2]) << 16
		| static_cast<uint32>(Bytes[Offset + 3]) << 24;
}

void WriteU32(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
{
	for (int32 Byte = 0; Byte < 4; ++Byte)
	{
		Bytes[Offset + Byte] = static_cast<uint8>(Value >> (Byte * 8));
	}
}

float ReadF32(TConstArrayView<uint8> Bytes, int32 Offset)
{
	float Value = 0.0f;
	FMemory::Memcpy(&Value, Bytes.GetData() + Offset, sizeof(Value));
	return Value;
}

int32 PayloadOffset(TConstArrayView<uint8> Object)
{
	return Object.Num() >= 12 ? 12 + static_cast<int32>(ReadU32(Object, 8))
		: INDEX_NONE;
}

class FRuntimeVertexMeshCensusWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("census object exceeds its contract")));
		TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
		Copy.Append(Bytes.GetData(), Bytes.Num());
		return FWoTBootstrapStatus::Success();
	}
	TMap<FString, TArray<uint8>> Objects;
};

class FRuntimeVertexMeshCensusProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return false; }
};

bool ParentChainContains(const UMaterialInterface* Material,
	const UMaterialInterface* ExpectedParent)
{
	const UMaterialInterface* Current = Material;
	while (IsValid(Current))
	{
		if (Current == ExpectedParent) return true;
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
		Current = Instance ? Instance->Parent : nullptr;
	}
	return false;
}

TArray<FVector3f> ComputeReferenceSourceNormals(
	const FWoTRuntimeVertexMeshData& Data,
	TConstArrayView<FVector3f> Positions)
{
	TArray<FVector3d> Accumulated;
	Accumulated.SetNumZeroed(Positions.Num());
	for (const FWoTRuntimeVertexMeshTriangle& Triangle : Data.GetTriangles())
	{
		const FVector3d A(Positions[Triangle.SourceVertices[0]]);
		const FVector3d B(Positions[Triangle.SourceVertices[1]]);
		const FVector3d C(Positions[Triangle.SourceVertices[2]]);
		FVector3d Face = FVector3d::CrossProduct(B - A, C - A);
		Face /= FMath::Sqrt(Face.SizeSquared() + 0.001);
		for (uint32 Vertex : Triangle.SourceVertices)
			Accumulated[Vertex] += Face;
	}
	TArray<FVector3f> Result;
	Result.Reserve(Accumulated.Num());
	for (FVector3d Normal : Accumulated)
	{
		const double Length = Normal.Length();
		if (Length > 0.0) Normal /= Length;
		Result.Add(FVector3f(Normal));
	}
	return Result;
}

FString QuantizedPositionKey(const FVector3f& Position)
{
	return FString::Printf(TEXT("%.3f,%.3f,%.3f"),
		Position.X, Position.Y, Position.Z);
}

FString TrianglePositionKey(const FVector3f& A, const FVector3f& B,
	const FVector3f& C)
{
	TArray<FString, TInlineAllocator<3>> Corners = {
		QuantizedPositionKey(A), QuantizedPositionKey(B), QuantizedPositionKey(C)};
	Corners.Sort();
	return Corners[0] + TEXT("|") + Corners[1] + TEXT("|") + Corners[2];
}

void AddTriangleKey(TMap<FString, int32>& Keys, const FString& Key)
{
	++Keys.FindOrAdd(Key);
}

bool RuntimeVertexMeshLoadCurvyHashes(
	TMap<FString, TArray<FString>>& OutHashes)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *RuntimeVertexMeshRepoPath(
		TEXT("Tools/convert/curvy_hashes.json")))) return false;
	auto Parsed = ParseCanonicalJson(Bytes);
	if (Parsed.IsError()) return false;
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	const FWoTCanonicalJsonValue* Schema = Root.FindMember(TEXT("schema"));
	const FWoTCanonicalJsonValue* Meshes = Root.FindMember(TEXT("meshes"));
	if (!Schema || Schema->GetKind() != EWoTCanonicalJsonKind::String
		|| Schema->GetString() != TEXT("wot-curvy-frame-hashes-v1")
		|| !Meshes || !Meshes->IsObject()) return false;
	for (int32 MeshIndex = 0; MeshIndex < Meshes->Num(); ++MeshIndex)
	{
		const FWoTCanonicalJsonValue& Frames = Meshes->GetMemberValue(MeshIndex);
		if (!Frames.IsArray()) return false;
		TArray<FString>& Hashes = OutHashes.Add(Meshes->GetMemberKey(MeshIndex));
		for (int32 FrameIndex = 0; FrameIndex < Frames.Num(); ++FrameIndex)
		{
			const FWoTCanonicalJsonValue& Hash = Frames.GetElement(FrameIndex);
			if (Hash.GetKind() != EWoTCanonicalJsonKind::String
				|| Hash.GetString().Len() != 64) return false;
			Hashes.Add(Hash.GetString());
		}
	}
	return true;
}

FString RuntimeVertexMeshCurvyFrameHash(
	const FWoTRuntimeVertexMeshData& Data,
	const FWoTRuntimeVertexMeshPose& Pose)
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	Positions.SetNumZeroed(Data.GetPositionVertexCount());
	Normals.SetNumZeroed(Data.GetPositionVertexCount());
	TBitArray<> Seen(false, Data.GetPositionVertexCount());
	if (Pose.RenderPositions.Num() != Data.GetRenderVertices().Num()
		|| Pose.RenderNormals.Num() != Data.GetRenderVertices().Num())
	{
		return FString();
	}
	for (int32 RenderIndex = 0;
		RenderIndex < Data.GetRenderVertices().Num(); ++RenderIndex)
	{
		const int32 PositionIndex = static_cast<int32>(
			Data.GetRenderVertices()[RenderIndex].SourceVertex);
		if (!Positions.IsValidIndex(PositionIndex)) return FString();
		if (Seen[PositionIndex]
			&& (Positions[PositionIndex] != Pose.RenderPositions[RenderIndex]
				|| Normals[PositionIndex] != Pose.RenderNormals[RenderIndex]))
		{
			return FString();
		}
		Positions[PositionIndex] = Pose.RenderPositions[RenderIndex];
		Normals[PositionIndex] = Pose.RenderNormals[RenderIndex];
		Seen[PositionIndex] = true;
	}
	if (Seen.Find(false) != INDEX_NONE) return FString();
	TArray<uint8> HashBytes;
	HashBytes.Reserve(Data.GetPositionVertexCount() * 6
		* static_cast<int32>(sizeof(float)));
	auto AppendVectors = [&HashBytes](TConstArrayView<FVector3f> Vectors)
	{
		for (const FVector3f& Vector : Vectors)
		{
			const float Components[3] = {Vector.X, Vector.Y, Vector.Z};
			HashBytes.Append(reinterpret_cast<const uint8*>(Components),
				static_cast<int32>(sizeof(Components)));
		}
	};
	AppendVectors(Positions);
	AppendVectors(Normals);
	return Sha256(HashBytes).ToHex();
}

#if !WOT_GUARDED_BOOTSTRAP
bool VerifyCookedBodyParity(FAutomationTestBase& Test,
	const FWoTRuntimeVertexMeshDataPtr& DataPtr,
	const FWoTRuntimeVertexMeshPose& Pose)
{
	const FWoTRuntimeVertexMeshData& Data = *DataPtr;
	const FString MeshName = Data.GetMeshName().ToString();
	USkeletalMesh* Cooked = LoadObject<USkeletalMesh>(nullptr,
		*FString::Printf(TEXT("/Game/WoT/Meshes/%s/%s/SkeletalMeshes/%s.%s"),
			*MeshName, *MeshName, *MeshName, *MeshName));
	if (!Test.TestNotNull(*FString::Printf(TEXT("cooked %s parity carrier"),
		*MeshName), Cooked))
		return false;
	const FSkeletalMeshRenderData* RenderData = Cooked->GetResourceForRendering();
	if (!Test.TestTrue(*FString::Printf(
		TEXT("cooked %s has LOD-0 render data"), *MeshName),
		RenderData && RenderData->LODRenderData.IsValidIndex(0)))
		return false;
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	const TArray<FSkeletalMaterial>& Materials = Cooked->GetMaterials();
	if (Test.TestEqual(*FString::Printf(TEXT("runtime/cooked %s slot count"),
		*MeshName),
		Data.GetSlots().Num(), Materials.Num()))
	{
		for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			Test.TestEqual(*FString::Printf(
				TEXT("runtime/cooked %s slot name %d"), *MeshName, Slot),
				Data.GetSlots()[Slot].Name, Materials[Slot].MaterialSlotName);
		}
	}

	TArray<TMap<FString, int32>> RuntimeTriangles;
	RuntimeTriangles.SetNum(Data.GetSlots().Num());
	for (int32 Slot = 0; Slot < Data.GetSlots().Num(); ++Slot)
	{
		const TArray<uint32>& Indices = Data.GetSlots()[Slot].Indices;
		for (int32 Corner = 0; Corner + 2 < Indices.Num(); Corner += 3)
		{
			AddTriangleKey(RuntimeTriangles[Slot], TrianglePositionKey(
				Pose.RenderPositions[Indices[Corner]],
				Pose.RenderPositions[Indices[Corner + 1]],
				Pose.RenderPositions[Indices[Corner + 2]]));
		}
	}
	TArray<TMap<FString, int32>> CookedTriangles;
	CookedTriangles.SetNum(Materials.Num());
	TArray<uint32> Indices;
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	const FPositionVertexBuffer& Positions =
		LOD.StaticVertexBuffers.PositionVertexBuffer;
	for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
	{
		if (!CookedTriangles.IsValidIndex(Section.MaterialIndex)) continue;
		for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
		{
			const uint32 First = Section.BaseIndex + Triangle * 3;
			if (!Indices.IsValidIndex(First + 2)) continue;
			AddTriangleKey(CookedTriangles[Section.MaterialIndex],
				TrianglePositionKey(Positions.VertexPosition(Indices[First]),
					Positions.VertexPosition(Indices[First + 1]),
					Positions.VertexPosition(Indices[First + 2])));
		}
	}
	if (RuntimeTriangles.Num() == CookedTriangles.Num())
	{
		for (int32 Slot = 0; Slot < RuntimeTriangles.Num(); ++Slot)
		{
			Test.TestTrue(*FString::Printf(
				TEXT("runtime/cooked %s triangle-position multiset %d"),
				*MeshName, Slot),
				RuntimeTriangles[Slot].OrderIndependentCompareEqual(
					CookedTriangles[Slot]));
		}
	}
	Test.AddInfo(TEXT("WallSlab is the documented cooked-parity exception: "
		"the dev asset keeps its bespoke static/citadel representation, so this "
		"vertex-mesh body comparison intentionally excludes it."));
	return !Test.HasAnyErrors();
}
#endif

bool VerifyAesSedaiSkinSemantics(FAutomationTestBase& Test,
	const FWoTRuntimeVertexMeshDataPtr& DataPtr)
{
	const FWoTRuntimeVertexMeshData& Data = *DataPtr;
	if (Test.TestEqual(TEXT("AesSedai has the three expected runtime slots"),
		Data.GetSlots().Num(), 3))
	{
		Test.TestEqual(TEXT("AesSedai slot 0 keeps noncontiguous texture index"),
			Data.GetSlots()[0].TextureIndex, 1);
		Test.TestEqual(TEXT("AesSedai slot 1 shared texture index"),
			Data.GetSlots()[1].TextureIndex, 2);
		Test.TestEqual(TEXT("AesSedai slot 2 shared texture index"),
			Data.GetSlots()[2].TextureIndex, 2);
	}
	UWoTVertexAnimComponent* Component =
		NewObject<UWoTVertexAnimComponent>(GetTransientPackage());
	if (Test.TestTrue(TEXT("AesSedai runtime skin fixture binds"),
		Component->BindRuntimeVertexMeshForTesting(DataPtr)))
	{
		UTexture* SkinOne = LoadObject<UTexture>(nullptr,
			TEXT("/Game/WoT/Textures/WoTPawns/H_AesSedai0.H_AesSedai0"));
		UTexture* SkinTwo = LoadObject<UTexture>(nullptr,
			TEXT("/Game/WoT/Textures/WoTPawns/H_AesSedai1.H_AesSedai1"));
		Test.TestNotNull(TEXT("first AesSedai override texture"), SkinOne);
		Test.TestNotNull(TEXT("second AesSedai override texture"), SkinTwo);
		Test.TestTrue(TEXT("MultiSkins(1) targets material slot one"),
			Component->SetSkinOverride(1, FSoftObjectPath(SkinOne)));
		Test.TestTrue(TEXT("MultiSkins(2) targets shared-style slot two"),
			Component->SetSkinOverride(2, FSoftObjectPath(SkinTwo)));
		UTexture* SlotTextures[3] = {};
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			if (UMaterialInterface* Material = Component->GetMaterial(Slot))
			{
				Material->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), SlotTextures[Slot]);
			}
		}
		Test.TestEqual(TEXT("MultiSkins(1) follows authored texture index one"),
			SlotTextures[0], SkinOne);
		Test.TestEqual(TEXT("MultiSkins(2) reaches the lit shared-texture section"),
			SlotTextures[1], SkinTwo);
		Test.TestEqual(TEXT("MultiSkins(2) reaches the unlit shared-texture section"),
			SlotTextures[2], SkinTwo);
	}
	return !Test.HasAnyErrors();
}

// LightningStreak02 (WoTBeamEffect) writes each frame's texture as the actor
// Skin. Every LSbolt triangle uses mesh texture 1, an empty slot that UE1 draws
// with Owner->Skin; mesh texture 0 (LB_A01) is used by no triangle, so a skin
// aimed at MultiSkins[0] left the bolt on the master's neutral default.
bool VerifyLightningBoltSkinSlot(FAutomationTestBase& Test,
	const FWoTRuntimeVertexMeshDataPtr& DataPtr)
{
	const FWoTRuntimeVertexMeshData& Data = *DataPtr;
	if (Test.TestEqual(TEXT("LSbolt has one runtime slot"), Data.GetSlots().Num(), 1))
	{
		Test.TestEqual(TEXT("LSbolt triangles use mesh texture 1"),
			Data.GetSlots()[0].TextureIndex, 1);
		Test.TestTrue(TEXT("LSbolt mesh texture 1 is empty (drawn with the actor Skin)"),
			Data.GetSlots()[0].TextureName.IsEmpty());
	}
	UWoTVertexAnimComponent* Component =
		NewObject<UWoTVertexAnimComponent>(GetTransientPackage());
	if (Test.TestTrue(TEXT("LSbolt runtime skin fixture binds"),
		Component->BindRuntimeVertexMeshForTesting(DataPtr)))
	{
		UTexture* Skin = LoadObject<UTexture>(nullptr,
			TEXT("/Game/WoT/Textures/Angreal/LB_A05.LB_A05"));
		Test.TestNotNull(TEXT("LB_A05 streak texture"), Skin);
		Test.TestTrue(TEXT("the streak skin reaches the LSbolt slot"),
			Component->SetAllSkinOverrides(FSoftObjectPath(Skin)));
		UTexture* Bound = nullptr;
		if (UMaterialInterface* Material = Component->GetMaterial(0))
		{
			Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("Tex")), Bound);
		}
		Test.TestEqual(TEXT("the LSbolt slot draws the streak skin"), Bound, Skin);
	}
	return !Test.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeVertexMeshDecodeSpec,
	"WoT.RuntimeVertexMesh.DecodeSlotPoseAndTerminalFrame",
	WoTRuntimeVertexMeshFlags)

bool FWoTRuntimeVertexMeshDecodeSpec::RunTest(const FString&)
{
	TArray<uint8> MeshBytes;
	TArray<uint8> AnimationBytes;
	if (!TestTrue(TEXT("plain mesh fixture loads"), LoadRuntimeVertexMeshFixture(
		TEXT("cache-v3-mesh-plain.bin"), MeshBytes))
		|| !TestTrue(TEXT("plain animation fixture loads"),
			LoadRuntimeVertexMeshFixture(
				TEXT("cache-v3-animation-plain.bin"), AnimationBytes)))
	{
		return false;
	}

	TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr> Decoded =
		FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
			MoveTemp(MeshBytes), MoveTemp(AnimationBytes),
			TEXT("cache/mesh/mesh__PlainMesh"),
			TEXT("cache/animation/mesh__PlainMesh"));
	if (!TestFalse(TEXT("synthetic WOTMSH3/WOTANI3 pair decodes"),
		Decoded.IsError()))
	{
		if (Decoded.IsError())
		{
			AddError(Decoded.GetError().ToString());
		}
		return false;
	}
	const FWoTRuntimeVertexMeshDataPtr Data = Decoded.GetValue();
	TestEqual(TEXT("source vertex count"), Data->GetSourceVertexCount(), 4);
	TestEqual(TEXT("render vertex count"), Data->GetRenderVertexCount(), 9);
	TestEqual(TEXT("animation frame count"), Data->GetFrameCount(), 3);
	TestEqual(TEXT("one-past sequence table is retained"),
		Data->GetSequences().Num(), 2);
	if (Data->GetSequences().Num() == 2)
	{
		TestEqual(TEXT("terminal sequence name"),
			Data->GetSequences()[0].Name, FName(TEXT("All")));
		TestEqual(TEXT("terminal sequence reaches one frame past payload"),
			Data->GetSequences()[0].StartFrame
				+ Data->GetSequences()[0].NumFrames, 4);
	}

	const TArray<FWoTRuntimeVertexMeshSlot>& Slots = Data->GetSlots();
	if (TestEqual(TEXT("cache sections preserve the unlit material boundary"),
		Slots.Num(), 3))
	{
		TestEqual(TEXT("first slot name"), Slots[0].Name, FName(TEXT("SkinA")));
		TestEqual(TEXT("unlit shared-texture slot name"),
			Slots[1].Name, FName(TEXT("SkinA")));
		TestEqual(TEXT("masked slot suffix"),
			Slots[2].Name, FName(TEXT("SkinB_M")));
		TestFalse(TEXT("first SkinA section remains lit"), Slots[0].bUnlit);
		TestTrue(TEXT("second SkinA section preserves PF_Unlit"), Slots[1].bUnlit);
		TestEqual(TEXT("lit SkinA triangle owns one section"),
			Slots[0].Indices.Num(), 3);
		if (Slots[0].Indices.Num() >= 3)
		{
			TestEqual(TEXT("stored corner 2 is emitted first"),
				Slots[0].Indices[0], 2u);
			TestEqual(TEXT("stored corner 1 is emitted second"),
				Slots[0].Indices[1], 1u);
			TestEqual(TEXT("stored corner 0 is emitted last"),
				Slots[0].Indices[2], 0u);
		}
	}

	const TArray<FVector2f>& UVs = Data->GetRenderUVs();
	if (TestTrue(TEXT("render UV table contains the second corner"),
		UVs.IsValidIndex(1)))
	{
		TestEqual(TEXT("UE1 byte UV divides by 256"), UVs[1].X,
			255.0f / 256.0f);
	}

	const FWoTRuntimeVertexMeshPose* FrameZero = Data->GetFramePose(0);
	const FWoTRuntimeVertexMeshPose* OnePast = Data->GetFramePose(3);
	if (TestNotNull(TEXT("frame zero pose"), FrameZero)
		&& TestNotNull(TEXT("one-past pose"), OnePast))
	{
		TestEqual(TEXT("pose has one source position per frame vertex"),
			FrameZero->SourcePositions.Num(), 4);
		TestEqual(TEXT("pose has one normal per render vertex"),
			FrameZero->RenderNormals.Num(), 9);
		TestTrue(TEXT("one-past terminal frame renders frame zero"),
			FrameZero->SourcePositions == OnePast->SourcePositions
			&& FrameZero->RenderNormals == OnePast->RenderNormals);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshFatnessFormulaAndNeutralSpec,
	"WoT.Mesh.Fatness.FormulaAndNeutral",
	WoTRuntimeVertexMeshFlags)

bool FWoTMeshFatnessFormulaAndNeutralSpec::RunTest(const FString&)
{
	const FWoTRuntimeVertexMeshDataPtr Data = LoadPlainRuntimeVertexMesh(*this);
	if (!Data.IsValid()) return false;
	UWoTVertexAnimComponent* Component =
		NewObject<UWoTVertexAnimComponent>(GetTransientPackage());
	if (!TestTrue(TEXT("plain runtime mesh binds for Fatness"),
		Component->BindRuntimeVertexMeshForTesting(Data))) return false;
	const FWoTRuntimeVertexMeshPose* Cached = Data->GetFramePose(0);
	const FWoTRuntimeVertexMeshPose* Published =
		Component->GetRuntimePoseForTesting();
	if (!TestNotNull(TEXT("cached neutral pose"), Cached)
		|| !TestNotNull(TEXT("published neutral pose"), Published)) return false;
	TestEqual(TEXT("UE1 Fatness defaults to neutral"), Component->Fatness,
		uint8(128));
	TestTrue(TEXT("Fatness 128 leaves render positions bit-identical"),
		Published->RenderPositions == Cached->RenderPositions);
	TArray<int32> ZeroNormalRenderVertices;
	for (int32 Index = 0; Index < Cached->RenderNormals.Num(); ++Index)
	{
		if (Cached->RenderNormals[Index].IsNearlyZero())
		{
			ZeroNormalRenderVertices.Add(Index);
		}
	}
	TestTrue(TEXT("plain fixture retail zero-normal vertices are 1, 5, 6, 7"),
		ZeroNormalRenderVertices == TArray<int32>({1, 5, 6, 7}));

	auto TestAmount = [this, Component, Cached](uint8 Fatness,
		float ExpectedWorldAmount)
	{
		Component->SetFatness(Fatness);
		const FWoTRuntimeVertexMeshPose* Pose =
			Component->GetRuntimePoseForTesting();
		if (!TestNotNull(TEXT("fattened pose is published"), Pose)) return;
		FString FirstOffender;
		for (int32 Index = 0; Index < Pose->RenderPositions.Num(); ++Index)
		{
			const float Distance = (Pose->RenderPositions[Index]
				- Cached->RenderPositions[Index]).Length();
			// UE1's mesh renderer: a source vertex touched only by degenerate faces
			// normalizes the zero accumulated normal and therefore does not move.
			const float Expected = Cached->RenderNormals[Index].IsNearlyZero()
				? 0.0f : ExpectedWorldAmount;
			if (FirstOffender.IsEmpty()
				&& !FMath::IsNearlyEqual(Distance, Expected, 1.e-5f))
			{
				FirstOffender = FString::Printf(
					TEXT("Fatness %u first mismatch at render vertex %d: got %.6f, expected %.6f (%s retail normal)"),
					Fatness, Index, Distance, Expected,
					Expected == 0.0f ? TEXT("zero") : TEXT("unit"));
			}
		}
		TestTrue(FirstOffender.IsEmpty()
			? *FString::Printf(TEXT("Fatness %u follows every retail normal"), Fatness)
			: *FirstOffender, FirstOffender.IsEmpty());
	};
	TestAmount(128, 0.0f);
	TestAmount(157, 1.8125f);
	TestAmount(164, 2.25f);
	Component->SetFatness(128);
	Published = Component->GetRuntimePoseForTesting();
	TestTrue(TEXT("returning to Fatness 128 restores bit-identical positions"),
		Published && Published->RenderPositions == Cached->RenderPositions);
	FWoTRuntimeActorVisualPresentation Visual;
	Visual.DrawScale = 2.0f;
	Visual.Fatness = 200;
	FString ApplyError;
	TestTrue(TEXT("runtime actor visual applies to a bound component"),
		WoTRuntimeApplyBoundActorVisualPresentationForTesting(
			*Component, Visual, ApplyError));
	TestEqual(TEXT("runtime actor visual routes Fatness through the component"),
		Component->Fatness, uint8(200));
	Published = Component->GetRuntimePoseForTesting();
	if (TestNotNull(TEXT("runtime actor visual publishes Fatness"), Published))
	{
		const FVector LocalDelta(Published->RenderPositions[0]
			- Cached->RenderPositions[0]);
		const FVector WorldDelta = Component->GetComponentTransform()
			.TransformVector(LocalDelta);
		TestTrue(TEXT("map-authored Fatness remains world-unit invariant"),
			FMath::IsNearlyEqual(WorldDelta.Length(), 4.5, 1.e-5));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshFatnessRetailNormalsAndSeamsSpec,
	"WoT.Mesh.Fatness.RetailNormalsAndSeams",
	WoTRuntimeVertexMeshFlags)

bool FWoTMeshFatnessRetailNormalsAndSeamsSpec::RunTest(const FString&)
{
	const TArray<FIntVector> SourcePositions = {
		FIntVector(1, 1, 1), FIntVector(-1, -1, 1),
		FIntVector(-1, 1, -1), FIntVector(1, -1, -1)};
	// Four outward-wound faces; every face owns render vertices so every source
	// vertex is split across UV seams.
	const TArray<FIntVector> SourceTriangles = {
		FIntVector(0, 2, 1), FIntVector(0, 1, 3),
		FIntVector(0, 3, 2), FIntVector(1, 2, 3)};
	const TArray<uint32> RenderSourceVertices = {
		0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	const FWoTRuntimeVertexMeshDataPtr Data =
		FWoTRuntimeVertexMesh::MakeSyntheticForTesting(
			SourcePositions, SourceTriangles, RenderSourceVertices);
	if (!TestTrue(TEXT("closed tetrahedron fixture builds"), Data.IsValid()))
		return false;
	const FWoTRuntimeVertexMeshPose* Neutral = Data->GetFramePose(0);
	if (!TestNotNull(TEXT("closed tetrahedron neutral pose"), Neutral))
		return false;
	const TArray<FVector3f> ReferenceNormals =
		ComputeReferenceSourceNormals(*Data, Neutral->SourcePositions);
	UWoTVertexAnimComponent* Component =
		NewObject<UWoTVertexAnimComponent>(GetTransientPackage());
	if (!TestTrue(TEXT("closed tetrahedron binds"),
		Component->BindRuntimeVertexMeshForTesting(Data))) return false;
	const FWoTRuntimeVertexMeshPose* PublishedNeutral =
		Component->GetRuntimePoseForTesting();
	const TArray<FVector3f> NeutralLightingNormals = PublishedNeutral
		? PublishedNeutral->RenderNormals : TArray<FVector3f>();
	Component->SetFatness(157);
	const FWoTRuntimeVertexMeshPose* Fattened =
		Component->GetRuntimePoseForTesting();
	if (!TestNotNull(TEXT("closed tetrahedron fattened pose"), Fattened))
		return false;

	TMap<uint32, FVector3f> PositionBySource;
	for (int32 RenderIndex = 0;
		RenderIndex < Data->GetRenderVertices().Num(); ++RenderIndex)
	{
		const uint32 SourceVertex =
			Data->GetRenderVertices()[RenderIndex].SourceVertex;
		const FVector3f ExpectedDelta =
			ReferenceNormals[SourceVertex] * 1.8125f;
		const FVector3f ActualDelta = Fattened->RenderPositions[RenderIndex]
			- Neutral->RenderPositions[RenderIndex];
		TestTrue(*FString::Printf(
			TEXT("render vertex %d follows the retail source normal"), RenderIndex),
			ActualDelta.Equals(ExpectedDelta, 1.e-5f));
		TestTrue(*FString::Printf(
			TEXT("convex render vertex %d moves outward"), RenderIndex),
			Fattened->RenderPositions[RenderIndex].Length()
				> Neutral->RenderPositions[RenderIndex].Length());
		if (const FVector3f* Existing = PositionBySource.Find(SourceVertex))
		{
			TestTrue(*FString::Printf(
				TEXT("source vertex %u stays watertight across seams"), SourceVertex),
				*Existing == Fattened->RenderPositions[RenderIndex]);
		}
		else
		{
			PositionBySource.Add(SourceVertex,
				Fattened->RenderPositions[RenderIndex]);
		}
		TestTrue(*FString::Printf(
			TEXT("lighting normal %d is unchanged"), RenderIndex),
			NeutralLightingNormals.IsValidIndex(RenderIndex)
				&& Fattened->RenderNormals[RenderIndex]
					== NeutralLightingNormals[RenderIndex]);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshFatnessWorldUnitScaleSpec,
	"WoT.Mesh.Fatness.WorldUnitScale",
	WoTRuntimeVertexMeshFlags)

bool FWoTMeshFatnessWorldUnitScaleSpec::RunTest(const FString&)
{
	const FWoTRuntimeVertexMeshDataPtr Data = LoadPlainRuntimeVertexMesh(*this);
	if (!Data.IsValid()) return false;
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	AActor* Owner = World ? World->SpawnActor<AActor>() : nullptr;
	UWoTVertexAnimComponent* Component = Owner
		? NewObject<UWoTVertexAnimComponent>(Owner) : nullptr;
	if (Component)
	{
		Owner->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}
	if (!TestTrue(TEXT("scaled runtime mesh binds"),
		Component && Component->BindRuntimeVertexMeshForTesting(Data)))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const FWoTRuntimeVertexMeshPose* Neutral = Data->GetFramePose(0);
	Component->SetFatness(157);
	// Changing DrawScale after Fatness is live must republish the local offset;
	// otherwise this becomes a 3.625-unit world displacement.
	Component->SetRelativeScale3D(FVector(2.0));
	const FBoxSphereBounds NeutralBounds =
		FBoxSphereBounds(Data->GetBounds()).TransformBy(
			Component->GetComponentTransform());
	const FWoTRuntimeVertexMeshPose* Fattened =
		Component->GetRuntimePoseForTesting();
	if (!TestNotNull(TEXT("scaled fattened pose"), Fattened)) return false;
	FString FirstOffender;
	for (int32 Index = 0; Index < Fattened->RenderPositions.Num(); ++Index)
	{
		const FVector LocalDelta(Fattened->RenderPositions[Index]
			- Neutral->RenderPositions[Index]);
		const FVector WorldDelta = Component->GetComponentTransform()
			.TransformVector(LocalDelta);
		const float Expected = Neutral->RenderNormals[Index].IsNearlyZero()
			? 0.0f : 1.8125f;
		if (FirstOffender.IsEmpty()
			&& !FMath::IsNearlyEqual(WorldDelta.Length(), Expected, 1.e-5))
		{
			FirstOffender = FString::Printf(
				TEXT("scaled Fatness first mismatch at render vertex %d: got %.6f, expected %.6f"),
				Index, WorldDelta.Length(), Expected);
		}
	}
	TestTrue(FirstOffender.IsEmpty()
		? TEXT("scaled Fatness remains world-unit invariant for every retail normal")
		: *FirstOffender, FirstOffender.IsEmpty());
	const FBoxSphereBounds FattenedBounds =
		Component->CalcBounds(Component->GetComponentTransform());
	TestTrue(TEXT("world bounds grow by the Fatness offset"),
		FattenedBounds.BoxExtent.X >= NeutralBounds.BoxExtent.X + 1.8125 - 1.e-5
		&& FattenedBounds.BoxExtent.Y >= NeutralBounds.BoxExtent.Y + 1.8125 - 1.e-5
		&& FattenedBounds.BoxExtent.Z >= NeutralBounds.BoxExtent.Z + 1.8125 - 1.e-5);
	const bool bSuccess = !HasAnyErrors();
	WoTTest::DestroySpecWorld(World);
	return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshFatnessPawnOverlaySpec,
	"WoT.Mesh.Fatness.PawnOverlay",
	WoTRuntimeVertexMeshFlags)

bool FWoTMeshFatnessPawnOverlaySpec::RunTest(const FString&)
{
	const FWoTRuntimeVertexMeshDataPtr Data = LoadPlainRuntimeVertexMesh(*this);
	if (!Data.IsValid()) return false;
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	AWoTCharacter* Pawn = World ? World->SpawnActor<AWoTCharacter>() : nullptr;
	UWoTVertexAnimComponent* Source = Pawn
		? Cast<UWoTVertexAnimComponent>(Pawn->GetMesh()) : nullptr;
	if (!TestNotNull(TEXT("overlay test world"), World)
		|| !TestNotNull(TEXT("overlay source pawn"), Pawn)
		|| !TestNotNull(TEXT("overlay source vertex mesh"), Source)
		|| !TestTrue(TEXT("decoded vertex-mesh fixture binds to pawn"),
			Source->BindRuntimeVertexMeshForTesting(Data)))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const FTransform SourceRelative(
		FRotator(0.0, 17.0, 0.0), FVector(3.0, -4.0, 5.0), FVector(1.5));
	Source->SetRelativeTransform(SourceRelative);
	Source->PlayAnim(TEXT("Move"), 1.0f);
	AWoTPawnSkinOverlay* Overlay =
		World->SpawnActor<AWoTPawnSkinOverlay>();
	FWoTPawnOverlayRow Row;
	Row.Fatness = 157.0f;
	if (!TestNotNull(TEXT("pawn overlay spawned"), Overlay))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Overlay->Configure(TEXT("FatnessSpec"), Row, Pawn);
	TestTrue(TEXT("overlay copies the source relative transform exactly"),
		Overlay->Mesh->GetRelativeTransform().Equals(
			Source->GetRelativeTransform(), 1.e-6));
	TestEqual(TEXT("overlay does not multiply the source scale"),
		Overlay->Mesh->GetRelativeScale3D(), Source->GetRelativeScale3D());
	TestEqual(TEXT("overlay mesh receives retail Fatness"),
		Overlay->Mesh->Fatness, uint8(157));

	auto TestOverlayPose = [this, Source, Overlay](const TCHAR* Label)
	{
		const FWoTRuntimeVertexMeshPose* SourcePose =
			Source->GetRuntimePoseForTesting();
		const FWoTRuntimeVertexMeshPose* OverlayPose =
			Overlay->Mesh->GetRuntimePoseForTesting();
		if (!TestNotNull(FString(Label) + TEXT(" source pose"), SourcePose)
			|| !TestNotNull(FString(Label) + TEXT(" overlay pose"), OverlayPose))
		{
			return TArray<FVector3f>();
		}
		for (int32 Index = 0; Index < OverlayPose->RenderPositions.Num(); ++Index)
		{
			const FVector3f Expected = SourcePose->RenderPositions[Index]
				+ OverlayPose->RenderNormals[Index] * (1.8125f / 1.5f);
			TestTrue(*FString::Printf(TEXT("%s vertex %d follows animated pose"),
				Label, Index),
				OverlayPose->RenderPositions[Index].Equals(Expected, 1.e-5f));
		}
		return OverlayPose->RenderPositions;
	};
	WoTTest::TickWorld(World, 1.0f / 60.0f);
	const TArray<FVector3f> FirstPose = TestOverlayPose(TEXT("first"));
	WoTTest::TickWorld(World, 1.0f / 30.0f);
	const TArray<FVector3f> AnimatedPose = TestOverlayPose(TEXT("animated"));
	TestTrue(TEXT("bAnimByOwner overlay does not freeze the fattened frame"),
		!FirstPose.IsEmpty() && FirstPose != AnimatedPose);
	const bool bSuccess = !HasAnyErrors();
	WoTTest::DestroySpecWorld(World);
	return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeVertexMeshCurvySpec,
	"WoT.Mesh.Runtime.Curvy",
	WoTRuntimeVertexMeshFlags)

bool FWoTRuntimeVertexMeshCurvySpec::RunTest(const FString&)
{
	FVector3d OctahedronMidpoint;
	FVector3d OctahedronMidNormal;
	FWoTRuntimeVertexMesh::EvaluateCurvyMidpointForTesting(
		FVector3d(1.0, 0.0, 0.0), FVector3d(0.0, 1.0, 0.0),
		FVector3d(1.0, 0.0, 0.0), FVector3d(0.0, 1.0, 0.0),
		OctahedronMidpoint, OctahedronMidNormal);
	TestTrue(TEXT("octahedron midpoint is the hand-computed 0.65/0.65/0"),
		OctahedronMidpoint.Equals(FVector3d(0.65, 0.65, 0.0), 1.e-12));
	TestTrue(TEXT("octahedron midpoint normal is (1,1,0)/sqrt(2)"),
		OctahedronMidNormal.Equals(FVector3d(
			FMath::Sqrt(0.5), FMath::Sqrt(0.5), 0.0), 1.e-12));
	TestEqual(TEXT("octahedron level-one topology is V+E / 4T"),
		FIntPoint(6 + 12, 8 * 4), FIntPoint(18, 32));
	TestEqual(TEXT("octahedron level-two topology is V+E / 4T"),
		FIntPoint(18 + 48, 32 * 4), FIntPoint(66, 128));

	TArray<uint8> MeshBytes;
	TArray<uint8> AnimationBytes;
	if (!TestTrue(TEXT("curvy mesh fixture loads"), LoadRuntimeVertexMeshFixture(
		TEXT("cache-v3-mesh-plain.bin"), MeshBytes))
		|| !TestTrue(TEXT("curvy animation fixture loads"),
			LoadRuntimeVertexMeshFixture(
				TEXT("cache-v3-animation-plain.bin"), AnimationBytes)))
	{
		return false;
	}
	auto Decoded = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
		MoveTemp(MeshBytes), MoveTemp(AnimationBytes),
		TEXT("cache/mesh/mesh__CurvyFixture"),
		TEXT("cache/animation/mesh__CurvyFixture"));
	if (!TestFalse(TEXT("curvy source fixture decodes"), Decoded.IsError()))
	{
		if (Decoded.IsError()) AddError(Decoded.GetError().ToString());
		return false;
	}
	const FWoTRuntimeVertexMeshDataPtr Flat = Decoded.GetValue();
	const FWoTRuntimeVertexMeshDataPtr Curvy =
		FWoTRuntimeVertexMesh::MakeCurvy(Flat, 2);
	if (!TestTrue(TEXT("two-level curvy mesh is constructed"), Curvy.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("curvy level marker"), Curvy->GetCurvyLevels(), 2);
	TestEqual(TEXT("original triangle table is retained"),
		Curvy->GetTriangles().Num(), 3);
	TestEqual(TEXT("three-face fixture position count after two levels"),
		Curvy->GetPositionVertexCount(), 31);
	TestEqual(TEXT("three-face fixture render corner count after two levels"),
		Curvy->GetRenderVertexCount(), 144);
	int32 RenderTriangleCount = 0;
	for (const FWoTRuntimeVertexMeshSlot& Slot : Curvy->GetSlots())
	{
		RenderTriangleCount += Slot.Indices.Num() / 3;
	}
	TestEqual(TEXT("each non-flat fixture triangle produces sixteen children"),
		RenderTriangleCount, 48);
	TestTrue(TEXT("the curvy result is shared by the runtime mesh cache"),
		Curvy == FWoTRuntimeVertexMesh::MakeCurvy(Flat, 2));

	const FWoTRuntimeVertexMeshPose* FlatPose = Flat->GetFramePose(0);
	const FWoTRuntimeVertexMeshPose* CurvyPose = Curvy->GetFramePose(0);
	const FWoTRuntimeVertexMeshPose* TerminalPose =
		Curvy->GetFramePose(Curvy->GetFrameCount());
	if (TestNotNull(TEXT("flat frame zero exists"), FlatPose)
		&& TestNotNull(TEXT("curvy frame zero exists"), CurvyPose)
		&& TestNotNull(TEXT("curvy terminal alias exists"), TerminalPose))
	{
		TestTrue(TEXT("frame-N remains the frame-zero alias"),
			CurvyPose == TerminalPose);
		const TArray<FVector3f> BaseNormals =
			ComputeReferenceSourceNormals(*Flat, FlatPose->SourcePositions);
		const FUintVector2 FirstParents = Curvy->GetDerivedParents()[0];
		const FVector3d ParentA(FlatPose->SourcePositions[FirstParents.X]);
		const FVector3d ParentB(FlatPose->SourcePositions[FirstParents.Y]);
		const FVector3d NormalA(BaseNormals[FirstParents.X]);
		const FVector3d NormalB(BaseNormals[FirstParents.Y]);
		const FVector3d MidNormal = (NormalA + NormalB).GetSafeNormal();
		const FVector3d ExpectedMidpoint = (ParentA + ParentB) * 0.5
			+ MidNormal * (0.15 * (ParentA - ParentB).Length()
				* FVector3d::CrossProduct(NormalA, NormalB).Length());
		const uint32 FirstDerived = static_cast<uint32>(
			Curvy->GetSourceVertexCount());
		bool bSawFirstDerived = false;
		TMap<uint32, FVector3f> PositionBySource;
		for (int32 RenderIndex = 0;
			RenderIndex < Curvy->GetRenderVertices().Num(); ++RenderIndex)
		{
			const uint32 SourceVertex =
				Curvy->GetRenderVertices()[RenderIndex].SourceVertex;
			const FVector3f& RenderPosition =
				CurvyPose->RenderPositions[RenderIndex];
			if (const FVector3f* Existing = PositionBySource.Find(SourceVertex))
			{
				TestTrue(TEXT("UV-split corners share an identical position"),
					*Existing == RenderPosition);
			}
			else
			{
				PositionBySource.Add(SourceVertex, RenderPosition);
			}
			if (SourceVertex == FirstDerived)
			{
				bSawFirstDerived = true;
				TestTrue(TEXT("first midpoint uses the curved push formula"),
					RenderPosition.Equals(FVector3f(ExpectedMidpoint), 1.e-6f));
			}
		}
		TestTrue(TEXT("first deterministic midpoint is rendered"), bSawFirstDerived);
	}
	for (int32 FrameIndex = 0; FrameIndex < Curvy->GetFrameCount(); ++FrameIndex)
	{
		const FWoTRuntimeVertexMeshPose* FramePose = Curvy->GetFramePose(FrameIndex);
		if (!TestNotNull(*FString::Printf(TEXT("curvy frame %d"), FrameIndex),
			FramePose)) continue;
		for (const FVector3f& RenderPosition : FramePose->RenderPositions)
		{
			TestTrue(TEXT("recomputed bounds contain every curvy frame"),
				Curvy->GetBounds().IsInsideOrOn(FVector(RenderPosition)));
		}
	}

	UWoTVertexAnimComponent* FlatComponent =
		NewObject<UWoTVertexAnimComponent>(GetTransientPackage());
	TestTrue(TEXT("flat component binds flat data"),
		FlatComponent->BindRuntimeVertexMeshForTesting(Flat));
	TestFalse(TEXT("flat component remains flat"),
		FlatComponent->IsRuntimeMeshCurvyForTesting());
	UWoTVertexAnimComponent* CurvyComponent =
		NewObject<UWoTVertexAnimComponent>(GetTransientPackage());
	CurvyComponent->bMeshCurvy = true;
	TestTrue(TEXT("curvy component binds derived data"),
		CurvyComponent->BindRuntimeVertexMeshForTesting(Flat));
	TestTrue(TEXT("curvy component selects the rounded variant"),
		CurvyComponent->IsRuntimeMeshCurvyForTesting());

	const AWoTAMAPearl* Pearl = GetDefault<AWoTAMAPearl>();
	TestTrue(TEXT("AMAPearl opts into the actor-level curvy flag"),
		Pearl && Pearl->DomeVisual && Pearl->DomeVisual->bMeshCurvy);
	const FWoTMeshEffectRow* Absorb = WoTMeshFX::Find(TEXT("AbsorbVisual"));
	const FWoTMeshEffectRow* Globe = WoTMeshFX::Find(TEXT("BFExpGlobe"));
	const FWoTMeshEffectRow* Jar = WoTMeshFX::Find(TEXT("PixieJar"));
	TestTrue(TEXT("AbsorbVisual row stays flat (a ring of fins)"),
		Absorb && !Absorb->bMeshCurvy);
	TestTrue(TEXT("BFExpGlobe row is curvy"), Globe && Globe->bMeshCurvy);
	TestTrue(TEXT("PixieJar explicitly remains flat"), Jar && !Jar->bMeshCurvy);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeVertexMeshValidationSpec,
	"WoT.RuntimeVertexMesh.ValidationAndNameResolution",
	WoTRuntimeVertexMeshFlags)

bool FWoTRuntimeVertexMeshValidationSpec::RunTest(const FString&)
{
	TArray<uint8> MeshBytes;
	TArray<uint8> AnimationBytes;
	if (!LoadRuntimeVertexMeshFixture(TEXT("cache-v3-mesh-plain.bin"), MeshBytes)
		|| !LoadRuntimeVertexMeshFixture(
			TEXT("cache-v3-animation-plain.bin"), AnimationBytes))
	{
		AddError(TEXT("runtime vertex mesh fixtures are missing"));
		return false;
	}
	const int32 MeshPayload = PayloadOffset(MeshBytes);
	if (!TestTrue(TEXT("mesh object has a payload"), MeshPayload >= 0))
	{
		return false;
	}
	const uint32 VertexCount = ReadU32(MeshBytes, MeshPayload + 12);
	const uint32 UVCount = ReadU32(MeshBytes, MeshPayload + 20);
	const int32 FirstTriangle = MeshPayload + 104
		+ static_cast<int32>(VertexCount * 4 + UVCount * 2);
	WriteU32(MeshBytes, FirstTriangle + 24, MAX_uint32);
	TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr> Malformed =
		FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
			MoveTemp(MeshBytes), MoveTemp(AnimationBytes),
			TEXT("cache/mesh/mesh__PlainMesh"),
			TEXT("cache/animation/mesh__PlainMesh"));
	TestTrue(TEXT("out-of-range render vertex rejects the whole pair"),
		Malformed.IsError());

	if (!LoadRuntimeVertexMeshFixture(TEXT("cache-v3-mesh-plain.bin"), MeshBytes)
		|| !LoadRuntimeVertexMeshFixture(
			TEXT("cache-v3-animation-plain.bin"), AnimationBytes))
	{
		AddError(TEXT("runtime vertex mesh fixtures disappeared during validation"));
		return false;
	}
	const int32 SectionPayload = PayloadOffset(MeshBytes);
	const uint32 SectionVertexCount = ReadU32(MeshBytes, SectionPayload + 12);
	const uint32 SectionUVCount = ReadU32(MeshBytes, SectionPayload + 20);
	const int32 SectionTriangle = SectionPayload + 104
		+ static_cast<int32>(SectionVertexCount * 4 + SectionUVCount * 2);
	// The first triangle belongs to section zero; section one is valid in the
	// header but must still be rejected when only the binary row claims it.
	WriteU32(MeshBytes, SectionTriangle + 36, 1);
	Malformed = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
		MoveTemp(MeshBytes), MoveTemp(AnimationBytes),
		TEXT("cache/mesh/mesh__PlainMesh"),
		TEXT("cache/animation/mesh__PlainMesh"));
	TestTrue(TEXT("binary/header triangle section mismatch is rejected"),
		Malformed.IsError());

	const TArray<FWoTNeutralResourceId> Resources = {
		{TEXT("cache/mesh/Angreal__LavaRock"),
			EWoTNeutralResourceKind::VertexMesh},
		{TEXT("cache/mesh/WoTDecorations__LavaRock"),
			EWoTNeutralResourceKind::VertexMesh},
		{TEXT("cache/mesh/WoTPawns__AesSedai"),
			EWoTNeutralResourceKind::VertexMesh}};
	FString MeshId;
	FString AnimationId;
	FString Failure;
	TestFalse(TEXT("ambiguous case-insensitive leaf fails closed"),
		FWoTRuntimeVertexMesh::ResolveLogicalIds(TEXT("lavarock"), NAME_None,
			Resources, MeshId, AnimationId, &Failure));
	TestTrue(TEXT("ambiguity is diagnosed"),
		Failure.Contains(TEXT("ambiguous"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("package hint resolves LavaRock"),
		FWoTRuntimeVertexMesh::ResolveLogicalIds(TEXT("LAVAROCK"),
			TEXT("wotdecorations"), Resources, MeshId, AnimationId, &Failure));
	TestEqual(TEXT("hint keeps authenticated authored identity"), MeshId,
		FString(TEXT("cache/mesh/WoTDecorations__LavaRock")));
	TestEqual(TEXT("paired animation identity"), AnimationId,
		FString(TEXT("cache/animation/WoTDecorations__LavaRock")));

	TArray<uint8> LodMesh;
	TArray<uint8> LodAnimation;
	if (TestTrue(TEXT("LodMesh fixture loads"), LoadRuntimeVertexMeshFixture(
		TEXT("cache-v3-mesh-lod.bin"), LodMesh))
		&& TestTrue(TEXT("LodMesh animation fixture loads"),
			LoadRuntimeVertexMeshFixture(
				TEXT("cache-v3-animation-lod.bin"), LodAnimation)))
	{
		auto LodDecoded = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
			MoveTemp(LodMesh), MoveTemp(LodAnimation),
			TEXT("cache/mesh/mesh__LodHero"),
			TEXT("cache/animation/mesh__LodHero"));
		if (!TestFalse(TEXT("attachment-nullable LodMesh decodes"),
			LodDecoded.IsError()))
		{
			AddError(LodDecoded.GetError().ToString());
		}
		else
		{
			TestEqual(TEXT("attachment face is absent from render slots"),
				LodDecoded.GetValue()->GetSlots()[0].Indices.Num()
				+ LodDecoded.GetValue()->GetSlots()[1].Indices.Num(), 6);
			TestNotNull(TEXT("attachment face still participates in pose normals"),
				LodDecoded.GetValue()->GetFramePose(0));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeVertexMeshComponentSpec,
	"WoT.RuntimeVertexMesh.ComponentPlaybackMaterialsAndBounds",
	WoTRuntimeVertexMeshFlags)

bool FWoTRuntimeVertexMeshComponentSpec::RunTest(const FString&)
{
	TArray<uint8> MeshBytes;
	TArray<uint8> AnimationBytes;
	if (!LoadRuntimeVertexMeshFixture(TEXT("cache-v3-mesh-plain.bin"), MeshBytes)
		|| !LoadRuntimeVertexMeshFixture(
			TEXT("cache-v3-animation-plain.bin"), AnimationBytes))
	{
		AddError(TEXT("runtime vertex mesh fixtures are missing"));
		return false;
	}
	auto Decoded = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
		MoveTemp(MeshBytes), MoveTemp(AnimationBytes),
		TEXT("cache/mesh/mesh__PlainMesh"),
		TEXT("cache/animation/mesh__PlainMesh"));
	if (Decoded.IsError())
	{
		AddError(Decoded.GetError().ToString());
		return false;
	}
	WoTTest::FSpecWorldOptions WorldOptions;
	WorldOptions.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(WorldOptions);
	AActor* Owner = World ? World->SpawnActor<AActor>() : nullptr;
	UWoTVertexAnimComponent* Component = Owner
		? NewObject<UWoTVertexAnimComponent>(Owner) : nullptr;
	if (!TestNotNull(TEXT("component test world"), World)
		|| !TestNotNull(TEXT("component test owner"), Owner)
		|| !TestNotNull(TEXT("component under test"), Component))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	Owner->AddInstanceComponent(Component);
	TestTrue(TEXT("runtime mesh binds without a skeletal carrier"),
		Component->BindRuntimeVertexMeshForTesting(Decoded.GetValue()));
	Component->RegisterComponent();
	// Start playback before flushing registration commands. This exercises the
	// shared render-command pipe ordering between resource initialization and the
	// first dynamic pose upload.
	Component->PlayAnim(TEXT("Move"), 1.0f);
	Component->TickComponent(0.01f, LEVELTICK_All, nullptr);
	World->SendAllEndOfFrameUpdates();
	if (!GUsingNullRHI && FApp::CanEverRender()) FlushRenderingCommands();
	TestTrue(TEXT("runtime mesh becomes the display mesh"),
		Component->HasDisplayMesh());
	TestEqual(TEXT("runtime display identity is exposed"),
		Component->GetDisplayMeshName(), FName(TEXT("PlainMesh")));
	const FWoTVertexMeshDiagnostics RuntimeDiagnostics =
		Component->GetMeshDiagnostics();
	TestEqual(TEXT("mesh diagnostics expose the authenticated logical id"),
		RuntimeDiagnostics.BoundMeshId,
		FString(TEXT("cache/mesh/mesh__PlainMesh")));
	TestEqual(TEXT("mesh diagnostics identify the carrier-free proxy"),
		RuntimeDiagnostics.ProxyKind, FName(TEXT("runtime")));
	TestEqual(TEXT("mesh diagnostics expose section count"),
		RuntimeDiagnostics.SectionCount, 3);
	TestEqual(TEXT("mesh diagnostics expose material count"),
		RuntimeDiagnostics.MaterialCount, 3);
	TestTrue(TEXT("mesh diagnostics expose nonzero bounds"),
		RuntimeDiagnostics.BoundsExtent.X > 0.0
		&& RuntimeDiagnostics.BoundsExtent.Y > 0.0
		&& RuntimeDiagnostics.BoundsExtent.Z > 0.0);
	TestNull(TEXT("runtime rendering does not require a skeletal carrier"),
		Component->GetSkeletalMeshAsset());
	TestEqual(TEXT("runtime slot count reaches mesh APIs"),
		Component->GetNumMaterials(), 3);
	const TArray<FName> SlotNames = Component->GetMaterialSlotNames();
	if (TestEqual(TEXT("runtime slot names reach mesh APIs"), SlotNames.Num(), 3))
	{
		TestEqual(TEXT("masked suffix reaches mesh API"),
			SlotNames[2], FName(TEXT("SkinB_M")));
		TestEqual(TEXT("slot name resolves its index"),
			Component->GetMaterialIndex(SlotNames[2]), 2);
	}
	TestNotNull(TEXT("each runtime slot has a default material"),
		Component->GetMaterial(0));
	UMaterialInterface* DefaultMaterial = Component->GetMaterial(0);
	Component->SetMaterial(0, nullptr);
	TestEqual(TEXT("null material override falls back to the slot default"),
		Component->GetMaterial(0), DefaultMaterial);
	TestTrue(TEXT("runtime slot-name validity uses component-owned slots"),
		Component->IsMaterialSlotNameValid(TEXT("SkinB_M")));
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetVisibility(false);
	Component->SetVisibility(true);
	const FBoxSphereBounds Bounds = Component->CalcBounds(FTransform::Identity);
	TestTrue(TEXT("header union bounds are used"),
		Bounds.BoxExtent.X > 0.0 && Bounds.BoxExtent.Y > 0.0
		&& Bounds.BoxExtent.Z > 0.0);

	Component->PlayAnim(TEXT("Move"), 1.0f);
	TArray<FVector> Before;
	TArray<FVector> After;
	TestTrue(TEXT("runtime pose is CPU-readable"),
		Component->GetCurrentPoseVertexPositions(Before));
	Component->TickComponent(1.0f / 30.0f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("advanced runtime pose remains CPU-readable"),
		Component->GetCurrentPoseVertexPositions(After));
	TestEqual(TEXT("CPU pose uses source vertex count"), Before.Num(), 4);
	TestTrue(TEXT("headless playback advances the decoded pose"), Before != After);

	UWoTVertexAnimComponent* CarrierPath =
		NewObject<UWoTVertexAnimComponent>(Owner);
	Owner->AddInstanceComponent(CarrierPath);
	CarrierPath->RegisterComponent();
	CarrierPath->Sequences = Decoded.GetValue()->GetSequences();
	const FWoTRuntimeVertexMeshPose* BasePose =
		Decoded.GetValue()->GetFramePose(0);
	TMap<int32, TArray<FVector3f>> FrameDeltas;
	for (int32 Frame = 1; Frame < Decoded.GetValue()->GetFrameCount(); ++Frame)
	{
		const FWoTRuntimeVertexMeshPose* Pose =
			Decoded.GetValue()->GetFramePose(Frame);
		TArray<FVector3f>& Deltas = FrameDeltas.Add(Frame);
		Deltas.SetNumUninitialized(BasePose->SourcePositions.Num());
		for (int32 Vertex = 0; Vertex < Deltas.Num(); ++Vertex)
			Deltas[Vertex] = Pose->SourcePositions[Vertex]
				- BasePose->SourcePositions[Vertex];
	}
	CarrierPath->SetPoseCacheForTesting(BasePose->SourcePositions, FrameDeltas);
	TestTrue(TEXT("carrier parity path installs headless morph weights"),
		CarrierPath->InstallHeadlessExternalMorphSetForTesting(
			Decoded.GetValue()->GetFrameCount()));
	auto TestPoseParity = [this, Component, CarrierPath](const TCHAR* Label)
	{
		TArray<FVector> RuntimePositions;
		TArray<FVector> CarrierPositions;
		return TestTrue(Label,
			Component->GetCurrentPoseVertexPositions(RuntimePositions)
			&& CarrierPath->GetCurrentPoseVertexPositions(CarrierPositions)
			&& RuntimePositions == CarrierPositions);
	};
	Component->SetFrameAndStop(TEXT("Move"), 0.5f);
	CarrierPath->SetFrameAndStop(TEXT("Move"), 0.5f);
	TestPoseParity(TEXT("SetFrameAndStop weights match the carrier path"));
	Component->PlayAnim(TEXT("Move"), 1.0f);
	CarrierPath->PlayAnim(TEXT("Move"), 1.0f);
	Component->TickComponent(0.01f, LEVELTICK_All, nullptr);
	CarrierPath->TickComponent(0.01f, LEVELTICK_All, nullptr);
	TestPoseParity(TEXT("PlayAnim weights match the carrier path"));
	Component->LoopAnim(TEXT("Move"), 1.0f);
	CarrierPath->LoopAnim(TEXT("Move"), 1.0f);
	Component->TickComponent(0.01f, LEVELTICK_All, nullptr);
	CarrierPath->TickComponent(0.01f, LEVELTICK_All, nullptr);
	TestPoseParity(TEXT("LoopAnim weights match the carrier path"));
	Component->TweenAnim(TEXT("Move"), 0.2f);
	CarrierPath->TweenAnim(TEXT("Move"), 0.2f);
	Component->TickComponent(0.1f, LEVELTICK_All, nullptr);
	CarrierPath->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestPoseParity(TEXT("TweenAnim weights match the carrier path"));
	Component->SetFrameAndStop(TEXT("Move"), 0.5f);
	CarrierPath->SetFrameAndStop(TEXT("Move"), 0.5f);
	Component->TweenPlayAnim(TEXT("Move"), 1.0f, 0.2f);
	CarrierPath->TweenPlayAnim(TEXT("Move"), 1.0f, 0.2f);
	Component->TickComponent(0.1f, LEVELTICK_All, nullptr);
	CarrierPath->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestPoseParity(TEXT("TweenPlayAnim tween weights match the carrier path"));
	Component->TickComponent(0.11f, LEVELTICK_All, nullptr);
	CarrierPath->TickComponent(0.11f, LEVELTICK_All, nullptr);
	TestPoseParity(TEXT("TweenPlayAnim playback weights match the carrier path"));

	UWoTRuntimeVertexMeshAnimEndReceiver* EndReceiver =
		NewObject<UWoTRuntimeVertexMeshAnimEndReceiver>(Owner);
	Component->OnAnimEnd.AddDynamic(
		EndReceiver, &UWoTRuntimeVertexMeshAnimEndReceiver::HandleAnimEnd);
	Component->PlayAnim(TEXT("Move"), 1.0f);
	Component->TickComponent(1.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("carrier-free one-shot broadcasts OnAnimEnd once"),
		EndReceiver->CallCount, 1);
	TestEqual(TEXT("OnAnimEnd reports the completed sequence"),
		EndReceiver->LastSequence, FName(TEXT("Move")));

	UMaterialInterface* MeshLitMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshLit.M_WoT_MeshLit"));
	UWoTMapLightTable* LightTable = NewObject<UWoTMapLightTable>(World);
	UWoTMeshLight* MeshLight = UWoTMeshLight::AttachRuntime(Owner, LightTable);
	TestNotNull(TEXT("runtime mesh light attaches to carrier-free owner"), MeshLight);
	const FSoftObjectPath ReplacementSkin(
		TEXT("/Game/WoT/Textures/WoTPawns/H_AesSedai0.H_AesSedai0"));
	const FSoftObjectPath ActorSkin(
		TEXT("/Game/WoT/Textures/Angreal/JDart.JDart"));
	Component->SetActorSkin(ActorSkin);
	TestEqual(TEXT("actor Skin is retained separately"),
		Component->GetRuntimeActorSkin(), ActorSkin);
	TestFalse(TEXT("actor Skin does not synthesize MultiSkins[0]"),
		Component->GetRuntimeSkinOverrides().Contains(0));
	TestTrue(TEXT("slot-index skin override is accepted"),
		Component->SetSkinOverride(0, ReplacementSkin));
	TestTrue(TEXT("skin rebuild immediately restores MeshLit reparenting"),
		ParentChainContains(Component->GetMaterial(0), MeshLitMaster));
	const TMap<int32, FSoftObjectPath> SavedSkins =
		Component->GetRuntimeSkinOverrides();
	UWoTVertexAnimComponent* CopiedDisplay =
		NewObject<UWoTVertexAnimComponent>(Owner);
	CopiedDisplay->CopyDisplayMeshFrom(Component);
	TestEqual(TEXT("display-mesh copy retains actor Skin independently"),
		CopiedDisplay->GetRuntimeActorSkin(), ActorSkin);
	TestTrue(TEXT("display-mesh copy retains exact MultiSkins independently"),
		CopiedDisplay->GetRuntimeSkinOverrides().OrderIndependentCompareEqual(
			SavedSkins));
	TestTrue(TEXT("ordinary runtime bind resets old-mesh skin state"),
		Component->BindRuntimeVertexMeshForTesting(Decoded.GetValue()));
	TestTrue(TEXT("new runtime mesh starts from authored skins"),
		Component->GetRuntimeSkinOverrides().IsEmpty());
	TestTrue(TEXT("new runtime mesh resets the old actor Skin"),
		Component->GetRuntimeActorSkin().IsNull());
	Component->SetActorSkin(ActorSkin);
	Component->SetRuntimeSkinOverrides(SavedSkins);
	TestEqual(TEXT("disguise snapshot can restore actor Skin state"),
		Component->GetRuntimeActorSkin(), ActorSkin);
	TestTrue(TEXT("disguise snapshot can restore runtime skin state"),
		Component->GetRuntimeSkinOverrides().OrderIndependentCompareEqual(SavedSkins));

#if !WOT_GUARDED_BOOTSTRAP
	USkeletalMesh* CookedCarrier = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/WoT/Meshes/AesSedai/AesSedai/SkeletalMeshes/AesSedai.AesSedai"));
	UWoTVertexAnimComponent* CarrierSkin =
		NewObject<UWoTVertexAnimComponent>(Owner);
	if (TestNotNull(TEXT("cooked carrier skin fixture"), CookedCarrier)
		&& TestNotNull(TEXT("cooked carrier skin component"), CarrierSkin))
	{
		Owner->AddInstanceComponent(CarrierSkin);
		CarrierSkin->SetSkeletalMesh(CookedCarrier);
		CarrierSkin->RegisterComponent();
		TestTrue(TEXT("slot skin override also applies to cooked carriers"),
			CarrierSkin->SetSkinOverride(0, ReplacementSkin));
		TestEqual(TEXT("cooked carrier remembers the exact skin override"),
			CarrierSkin->GetRuntimeSkinOverrides().FindRef(0), ReplacementSkin);
		CarrierSkin->SetActorSkin(ActorSkin);
		TestEqual(TEXT("cooked actor Skin preserves MultiSkins[0] state"),
			CarrierSkin->GetRuntimeSkinOverrides().FindRef(0), ReplacementSkin);
		CarrierSkin->SetActorSkin(FSoftObjectPath());
		TestEqual(TEXT("clearing cooked actor Skin restores MultiSkins[0] state"),
			CarrierSkin->GetRuntimeSkinOverrides().FindRef(0), ReplacementSkin);
		CarrierSkin->ClearSkinOverrides();
		TestTrue(TEXT("cooked carrier skin override can be restored to authored material"),
			CarrierSkin->GetRuntimeSkinOverrides().IsEmpty());
	}
#endif

	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		TestFalse(TEXT("headless runtime creates no component-owned proxy"),
			Component->HasSceneProxyForTesting());
	}
	else
	{
		TestTrue(TEXT("registered runtime mesh owns a proxy without a carrier"),
			Component->HasSceneProxyForTesting());
		Component->SetMaterial(0, Component->GetMaterial(0));
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetVisibility(false);
		Component->SetVisibility(true);
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();
	}
	UWoTVertexAnimComponent* LateBoundComponent =
		NewObject<UWoTVertexAnimComponent>(Owner);
	Owner->AddInstanceComponent(LateBoundComponent);
	LateBoundComponent->RegisterComponent();
	if (!GUsingNullRHI && FApp::CanEverRender())
	{
		TestTrue(TEXT("registered empty component initially owns a render state"),
			LateBoundComponent->IsRenderStateCreated());
		LateBoundComponent->DestroyRenderStateForTesting();
		TestFalse(TEXT("late-bind fixture is registered without a render state"),
			LateBoundComponent->IsRenderStateCreated());
	}
	TestTrue(TEXT("runtime mesh binds after an empty component was registered"),
		LateBoundComponent->BindRuntimeVertexMeshForTesting(Decoded.GetValue()));
	if (!GUsingNullRHI && FApp::CanEverRender())
	{
		TestTrue(TEXT("late bind immediately creates the missing render state"),
			LateBoundComponent->IsRenderStateCreated());
	}
	World->SendAllEndOfFrameUpdates();
	if (!GUsingNullRHI && FApp::CanEverRender())
	{
		FlushRenderingCommands();
		TestTrue(TEXT("late runtime bind creates the missing render state"),
			LateBoundComponent->IsRenderStateCreated());
		TestTrue(TEXT("late runtime bind creates the carrier-free proxy"),
			LateBoundComponent->HasSceneProxyForTesting());
	}
	Component->SetSkeletalMesh(nullptr);
	World->SendAllEndOfFrameUpdates();
	if (!GUsingNullRHI && FApp::CanEverRender()) FlushRenderingCommands();
	TestFalse(TEXT("runtime-to-null clears the display identity"),
		Component->HasDisplayMesh());
	TestFalse(TEXT("runtime-to-null detaches the custom scene proxy"),
		Component->HasSceneProxyForTesting());
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetMaterial(0, nullptr);
	Component->SetVisibility(false);
	Component->SetVisibility(true);
	const bool bSuccess = !HasAnyErrors();
	WoTTest::DestroySpecWorld(World);
	return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeVertexMeshRealRetailCensusSpec,
	"WoT.RuntimeVertexMesh.RealRetailCensus",
	WoTRuntimeVertexMeshFlags)

bool FWoTRuntimeVertexMeshRealRetailCensusSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	TMap<FString, TArray<FString>> CurvyHashes;
	if (!TestTrue(TEXT("Python curvy hash fixture loads"),
		RuntimeVertexMeshLoadCurvyHashes(CurvyHashes)))
	{
		return false;
	}
	auto ContractResult = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"),
		ContractResult.IsError()))
	{
		if (ContractResult.IsError()) AddError(ContractResult.GetError().ToString());
		return false;
	}
	const FWoTRetailContract& Contract = ContractResult.GetValue();
	TMap<FString, TArray<const FWoTRequiredFact*>> MeshFactsByPackage;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		if (Fact.Domain == EWoTFactDomain::Mesh)
			MeshFactsByPackage.FindOrAdd(Fact.SourcePath).Add(&Fact);
	}
	int32 DecodedCount = 0;
	TSet<FString> RequiredRealMeshes = {
		TEXT("ArrowTrail"), TEXT("InjuredWarder"), TEXT("MArcherArrow"),
		TEXT("AMAPearl"), TEXT("AbsorbVisual"),
		TEXT("AngrealAntiMagicAuraPickup"),
		TEXT("AngrealShieldPickup"), TEXT("LSbolt")};
	bool bSawLodMesh = false;
	for (const TPair<FString, TArray<const FWoTRequiredFact*>>& Package
		: MeshFactsByPackage)
	{
		TArray<uint8> PackageBytes;
		const FString SourceFile = RuntimeVertexMeshRepoPath(
			*(FString(TEXT("original/")) + Package.Key));
		if (!FFileHelper::LoadFileToArray(PackageBytes, *SourceFile))
		{
			AddError(FString::Printf(TEXT("required real retail package is missing: %s"),
				*Package.Key));
			return false;
		}
		FRuntimeVertexMeshCensusWriter Writer;
		FRuntimeVertexMeshCensusProgress Progress;
		FWoTRetailContract PackageContract = Contract;
		PackageContract.Facts.RemoveAll([&Package](const FWoTRequiredFact& Fact)
		{
			return Fact.SourcePath != Package.Key
				|| (Fact.Domain != EWoTFactDomain::Mesh
					&& Fact.Domain != EWoTFactDomain::Animation);
		});
		auto Imported = ImportMeshAnimationPackageBytesForTesting(
			PackageBytes, Package.Key, PackageContract, Writer, Progress);
		if (Imported.IsError())
		{
			AddError(FString::Printf(TEXT("%s import rejected: %s"), *Package.Key,
				*Imported.GetError().ToString()));
			continue;
		}
		for (const FWoTRequiredFact* MeshFact : Package.Value)
		{
			if (!MeshFact || MeshFact->Outputs.Num() != 1) continue;
			const FString MeshId = MeshFact->Outputs[0].LogicalId;
			FString AnimationId = MeshId;
			AnimationId.RemoveFromStart(TEXT("cache/mesh/"));
			AnimationId = TEXT("cache/animation/") + AnimationId;
			TArray<uint8>* MeshObject = Writer.Objects.Find(MeshId);
			TArray<uint8>* AnimationObject = Writer.Objects.Find(AnimationId);
			if (!MeshObject || !AnimationObject)
			{
				AddError(FString::Printf(TEXT("real retail pair was not emitted: %s"),
					*MeshId));
				continue;
			}
			const int32 RealPayloadOffset = PayloadOffset(*MeshObject);
			FBox EncodedBounds(ForceInit);
			if (RealPayloadOffset >= 0
				&& MeshObject->Num() >= RealPayloadOffset + 104)
			{
				EncodedBounds = FBox(
					FVector(ReadF32(*MeshObject, RealPayloadOffset + 80),
						ReadF32(*MeshObject, RealPayloadOffset + 84),
						ReadF32(*MeshObject, RealPayloadOffset + 88)),
					FVector(ReadF32(*MeshObject, RealPayloadOffset + 92),
						ReadF32(*MeshObject, RealPayloadOffset + 96),
						ReadF32(*MeshObject, RealPayloadOffset + 100)));
			}
			auto Decoded = FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(
				MoveTemp(*MeshObject), MoveTemp(*AnimationObject), MeshId, AnimationId);
			if (Decoded.IsError())
			{
				AddError(FString::Printf(TEXT("real retail pair rejected: %s: %s"),
					*MeshId, *Decoded.GetError().ToString()));
				continue;
			}
			const FWoTRuntimeVertexMeshPose* Pose =
				Decoded.GetValue()->GetFramePose(0);
			if (!Pose || Pose->SourcePositions.Num()
				!= Decoded.GetValue()->GetSourceVertexCount())
			{
				AddError(FString::Printf(TEXT("real retail frame zero failed: %s"),
					*MeshId));
				continue;
			}
			TestTrue(*FString::Printf(TEXT("payload/header bounds match: %s"),
				*MeshId), EncodedBounds.IsValid
				&& Decoded.GetValue()->GetBounds().Min.Equals(
					EncodedBounds.Min, KINDA_SMALL_NUMBER)
				&& Decoded.GetValue()->GetBounds().Max.Equals(
					EncodedBounds.Max, KINDA_SMALL_NUMBER));
			const TArray<FVector3f> ReferenceNormals =
				ComputeReferenceSourceNormals(*Decoded.GetValue(),
					Pose->SourcePositions);
			bool bNormalsMatch = ReferenceNormals.Num()
				== Decoded.GetValue()->GetSourceVertexCount();
			for (int32 RenderVertex = 0;
				bNormalsMatch && RenderVertex < Pose->RenderNormals.Num();
				++RenderVertex)
			{
				const uint32 SourceVertex = Decoded.GetValue()
					->GetRenderVertices()[RenderVertex].SourceVertex;
				bNormalsMatch = ReferenceNormals.IsValidIndex(SourceVertex)
					&& Pose->RenderNormals[RenderVertex].Equals(
						ReferenceNormals[SourceVertex], 1.e-5f);
			}
			TestTrue(*FString::Printf(
				TEXT("frame-zero normals match all-triangle reference: %s"),
				*MeshId), bNormalsMatch);
			const FName DecodedName = Decoded.GetValue()->GetMeshName();
			int32 ExpectedCurvyVertices = 0;
			int32 ExpectedCurvyTriangles = 0;
			if (DecodedName == FName(TEXT("AMAPearl")))
			{
				ExpectedCurvyVertices = 578;
				ExpectedCurvyTriangles = 1152;
			}
			else if (DecodedName == FName(TEXT("AngrealShieldPickup")))
			{
				ExpectedCurvyVertices = 1071;
				ExpectedCurvyTriangles = 1920;
			}
			// AbsorbVisual (fins) and AngrealAntiMagicAuraPickup (a box) are
			// decoded but never rounded.
			if (ExpectedCurvyVertices > 0)
			{
				int32 FlatTriangleCount = 0;
				for (const FWoTRuntimeVertexMeshTriangle& Triangle
					: Decoded.GetValue()->GetTriangles())
				{
					FlatTriangleCount += (Triangle.PolyFlags & 0x00004000u) != 0;
				}
				TestEqual(*FString::Printf(
					TEXT("shipped curvy mesh has no PF_Flat triangles: %s"),
					*MeshId), FlatTriangleCount, 0);
				const FWoTRuntimeVertexMeshDataPtr Curvy =
					FWoTRuntimeVertexMesh::MakeCurvy(Decoded.GetValue(), 2);
				if (TestTrue(*FString::Printf(TEXT("real curvy mesh builds: %s"),
					*MeshId), Curvy.IsValid()))
				{
					TestEqual(*FString::Printf(
						TEXT("real curvy position count: %s"), *MeshId),
						Curvy->GetPositionVertexCount(), ExpectedCurvyVertices);
					int32 CurvyTriangleCount = 0;
					for (const FWoTRuntimeVertexMeshSlot& Slot : Curvy->GetSlots())
					{
						CurvyTriangleCount += Slot.Indices.Num() / 3;
					}
					TestEqual(*FString::Printf(
						TEXT("real curvy triangle count: %s"), *MeshId),
						CurvyTriangleCount, ExpectedCurvyTriangles);
					const FString HashMeshName = DecodedName.ToString().Equals(
						TEXT("AMAPearl"), ESearchCase::IgnoreCase)
						? TEXT("AMApearl") : DecodedName.ToString();
					const TArray<FString>* ExpectedHashes =
						CurvyHashes.Find(HashMeshName);
					if (TestNotNull(*FString::Printf(
						TEXT("Python curvy hashes exist: %s"), *MeshId),
						ExpectedHashes))
					{
						TestEqual(*FString::Printf(
							TEXT("Python/runtime curvy frame count: %s"), *MeshId),
							ExpectedHashes->Num(), Curvy->GetFrameCount());
					}
					for (int32 FrameIndex = 0;
						FrameIndex < Curvy->GetFrameCount(); ++FrameIndex)
					{
						const FWoTRuntimeVertexMeshPose* CurvyPose =
							Curvy->GetFramePose(FrameIndex);
						if (!CurvyPose) continue;
						if (ExpectedHashes
							&& ExpectedHashes->IsValidIndex(FrameIndex))
						{
							TestEqual(*FString::Printf(
								TEXT("Python/runtime curvy frame hash %d: %s"),
								FrameIndex, *MeshId),
								RuntimeVertexMeshCurvyFrameHash(*Curvy, *CurvyPose),
								(*ExpectedHashes)[FrameIndex]);
						}
						for (const FVector3f& RenderPosition
							: CurvyPose->RenderPositions)
						{
							if (!Curvy->GetBounds().IsInsideOrOn(
								FVector(RenderPosition)))
							{
								AddError(FString::Printf(
									TEXT("real curvy bounds miss frame %d: %s"),
									FrameIndex, *MeshId));
								break;
							}
						}
					}
				}
			}
			RequiredRealMeshes.Remove(
				Decoded.GetValue()->GetMeshName().ToString());
			bSawLodMesh |= Decoded.GetValue()->IsLodMesh();
			if (Decoded.GetValue()->GetMeshName() == FName(TEXT("AesSedai")))
			{
				VerifyAesSedaiSkinSemantics(*this, Decoded.GetValue());
				// Fatness pushes along these normals (UE1's mesh renderer), so on a
				// a real character they must face OUT of the local body surface.
				// A whole-mesh centroid misclassifies concave limbs; the one-ring
				// neighbor centroid is the local surface interior for this check.
				TArray<FVector3f> NeighborSums;
				TArray<int32> NeighborCounts;
				NeighborSums.SetNumZeroed(Pose->SourcePositions.Num());
				NeighborCounts.SetNumZeroed(Pose->SourcePositions.Num());
				for (const FWoTRuntimeVertexMeshTriangle& Triangle
					: Decoded.GetValue()->GetTriangles())
				{
					const uint32 A = Triangle.SourceVertices[0];
					const uint32 B = Triangle.SourceVertices[1];
					const uint32 C = Triangle.SourceVertices[2];
					const uint32 Corners[] = {A, B, C};
					for (int32 Corner = 0; Corner < 3; ++Corner)
					{
						NeighborSums[Corners[Corner]] +=
							Pose->SourcePositions[Corners[(Corner + 1) % 3]]
							+ Pose->SourcePositions[Corners[(Corner + 2) % 3]];
						NeighborCounts[Corners[Corner]] += 2;
					}
				}
				int32 Outward = 0;
				int32 Directional = 0;
				for (int32 Index = 0; Index < Pose->SourcePositions.Num(); ++Index)
				{
					if (!ReferenceNormals.IsValidIndex(Index)
						|| ReferenceNormals[Index].IsNearlyZero()
						|| NeighborCounts[Index] == 0)
					{
						continue;
					}
					++Directional;
					const FVector3f NeighborCenter = NeighborSums[Index]
						/ static_cast<float>(NeighborCounts[Index]);
					Outward += FVector3f::DotProduct(ReferenceNormals[Index],
						Pose->SourcePositions[Index] - NeighborCenter) > 0.0f;
				}
				const float OutwardShare = static_cast<float>(Outward)
					/ FMath::Max(1, Directional);
				TestTrue(*FString::Printf(
					TEXT("AesSedai normals face outward from the one-ring surface (%.3f of %d directional vertices)"),
					OutwardShare, Directional),
					OutwardShare > 0.8f);
			}
			if (Decoded.GetValue()->GetMeshName() == FName(TEXT("LSbolt")))
			{
				VerifyLightningBoltSkinSlot(*this, Decoded.GetValue());
			}
			#if !WOT_GUARDED_BOOTSTRAP
			if (Decoded.GetValue()->GetMeshName() == FName(TEXT("Trolloc")))
			{
				VerifyCookedBodyParity(*this, Decoded.GetValue(), *Pose);
			}
			#endif
			for (const FVector3f& Normal : Pose->RenderNormals)
			{
				if (Normal.ContainsNaN())
				{
					AddError(FString::Printf(
						TEXT("real retail normal is non-finite: %s"), *MeshId));
					break;
				}
			}
			++DecodedCount;
		}
	}
	TestEqual(TEXT("all production cache mesh/animation pairs decode"),
		DecodedCount, 244);
	TestTrue(TEXT("targeted real retail meshes were decoded"),
		RequiredRealMeshes.IsEmpty());
	TestTrue(TEXT("real retail census includes a LodMesh"), bSawLodMesh);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshEffectRetailLightingSpec,
	"WoT.MeshLight.MeshEffectRetailLighting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMeshEffectRetailLightingSpec::RunTest(const FString&)
{
	// Retail lights a non-bUnlit STY_Translucent mesh: UE1 vertex lighting
	// from the zone ambient and the nearby lights, Diffuse = ScaleGlow. The
	// Light Orb's glass globe (PixieJar, ScaleGlow 2) went unseen when
	// lit by zone ambient alone. A bUnlit effect renders at GUnlitColor =
	// clamp(ScaleGlow/2 + AmbientGlow/256). In game every effect mesh is
	// baked against the map's light table, so the effect's own retail row
	// must decide which, before and after a re-bake.
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	FWoTLightRec Lamp;
	Lamp.Pos = FVector(0.0, 0.0, 200.0);
	Lamp.Radius = 1000.0f;
	Lamp.InvRadius = 1.0f / Lamp.Radius;
	Lamp.Color = FLinearColor(0.8f, 0.2f, 0.1f, 1.0f);
	Lamp.Brightness = 64;
	Lamp.bEmits = true;
	Table->Lights = {Lamp};
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 0.0)};
	Table->ZoneBspFront = {-1};
	Table->ZoneBspBack = {-1};
	Table->LeafBspLeaves = {0, 0};
	Table->LeafBspCsg = {0};
	Table->bBspRootOutside = true;
	Table->LeafLightOffsets = {0, 1};
	Table->LeafLightIndices = {0};
	FWoTZoneAmbient Zone;
	Zone.Zone = 0;
	Zone.bDefault = true;
	Zone.Color = FLinearColor(0.05f, 0.05f, 0.05f, 1.0f);
	Table->ZoneAmbients = {Zone};
	UWoTMeshLight::RegisterRuntimeMapLightTable(World, Table);

	auto Root = [](const UMaterialInterface* Material)
	{
		const UMaterialInterface* Current = Material;
		while (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current))
		{
			if (!IsValid(Instance->Parent)) break;
			Current = Instance->Parent.Get();
		}
		return Current ? Current->GetName() : FString();
	};
	auto Vector = [](const UMaterialInterface* Material, const TCHAR* Name)
	{
		FLinearColor Value = FLinearColor::Transparent;
		if (Material) Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Name), Value);
		return Value;
	};
	auto Scalar = [](const UMaterialInterface* Material, const TCHAR* Name)
	{
		float Value = -1.0f;
		if (Material) Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value);
		return Value;
	};
	auto Check = [&](AWoTMeshEffect* Effect, const FWoTMeshEffectRow& Row,
		const FString& Label)
	{
		UMaterialInterface* Material = Effect && Effect->bParticles
			? Effect->GetParticleMaterialForTesting()
			: Effect && Effect->Mesh ? Effect->Mesh->GetMaterial(0) : nullptr;
		if (!TestNotNull(Label + TEXT(" has a skin material"), Material)) return;
		// The effect's own UE1 state: the row's, except that a retail
		// bParticles mesh (the shield visuals) is drawn unlit.
		if (Effect->bParticles)
		{
			TestEqual(Label + TEXT(" uses the additive particle master"),
				Root(Material), FString(TEXT("M_WoT_ParticleAdd")));
			TestFalse(Label + TEXT(" never renders its solid mesh surface"),
				Effect->Mesh->IsVisible());
		}
		else if (Effect->bUnlit)
		{
			// GUnlitColor of the effect's live state; a bParticles mesh (a
			// shield) multiplies it by ScaleGlow once more, and a fade-in
			// starts at ScaleGlow 0.
			float Unlit = FMath::Clamp(
				0.5f * Effect->ScaleGlow + Effect->AmbientGlow / 256.0f, 0.0f, 1.0f);
			if (Effect->bParticles && Effect->ScaleGlow != 1.0f)
			{
				Unlit = FMath::Clamp(Unlit * Effect->ScaleGlow, 0.0f, 1.0f);
			}
			TestEqual(Label + TEXT(" keeps the unlit translucent master"),
				Root(Material), FString(TEXT("M_WoT_MeshTranslucent")));
			TestTrue(Label + TEXT(" renders at GUnlitColor"),
				Vector(Material, TEXT("MLAmbient")).Equals(
					FLinearColor(Unlit, Unlit, Unlit, 1.0f), 1.0e-4f));
		}
		else
		{
			TestEqual(Label + TEXT(" uses the lit translucent master"),
				Root(Material), FString(TEXT("M_WoT_MeshLitT")));
			TestTrue(Label + TEXT(" takes the zone ambient"),
				Vector(Material, TEXT("MLAmbient")).Equals(Zone.Color, 1.0e-4f));
			TestTrue(Label + TEXT(" takes the nearby light"),
				Vector(Material, TEXT("MLCol0")).Equals(Lamp.Color, 1.0e-4f));
			TestEqual(Label + TEXT(" scales its light by ScaleGlow"),
				Scalar(Material, TEXT("MLScaleGlow")), Effect->ScaleGlow);
		}
	};

	// Retail's live lit (non-bUnlit, non-bParticles) STY_Translucent mesh
	// classes, an unlit one, and a shield: ShieldParticleMesh is a bParticles
	// mesh, drawn at GUnlitColor in retail whatever bUnlit says.
	for (const TCHAR* Config : {TEXT("PixieJar"), TEXT("AbsorbVisual"),
		TEXT("FrozenPresentation"), TEXT("TaintExpGeoA"), TEXT("TaintExpGeoB"),
		TEXT("TaintExpTorus"), TEXT("LSRing"), TEXT("AirShieldVisual")})
	{
		const FWoTMeshEffectRow* Row = WoTMeshFX::Find(Config);
		if (!TestNotNull(FString::Printf(TEXT("production %s row"), Config), Row)) continue;
		TestEqual(FString::Printf(TEXT("%s is a translucent row"), Config),
			static_cast<int32>(Row->Style),
			static_cast<int32>(EWoTMeshEffectStyle::Translucent));
		AWoTMeshEffect* Effect = WoTMeshFX::SpawnLocal(*World, Config, FVector(0, 0, 100));
		if (!TestNotNull(FString::Printf(TEXT("%s spawned"), Config), Effect)) continue;
		Effect->SetLifeSpan(0.0f);
		const bool bShield = FString(Config).EndsWith(TEXT("ShieldVisual"));
		TestEqual(FString::Printf(TEXT("%s carries the row's lighting, shields unlit"), Config),
			Effect->bUnlit, Row->bUnlit || bShield);
		TestEqual(FString::Printf(TEXT("%s is a bParticles mesh only if a shield"), Config),
			Effect->bParticles, bShield);
		if (Row->FadeInTime > 0.0f)
		{
			TestEqual(FString::Printf(TEXT("%s fades in from dark"), Config),
				Effect->ScaleGlow, 0.0f);
		}
		else
		{
			TestEqual(FString::Printf(TEXT("%s starts at its ScaleGlow"), Config),
				Effect->ScaleGlow, Row->ScaleGlow);
		}
		Check(Effect, *Row, FString(Config));
		TArray<UWoTMeshLight*> Lights;
		Effect->GetComponents<UWoTMeshLight>(Lights);
		if (TestEqual(FString::Printf(TEXT("%s is baked against the map table"), Config),
			Lights.Num(), 1))
		{
			Lights[0]->ForceRuntimeBake();
			Check(Effect, *Row, FString(Config) + TEXT(" after a re-bake"));
		}
	}
	for (const TCHAR* Config : {TEXT("PixieJar"), TEXT("AbsorbVisual"),
		TEXT("FrozenPresentation"), TEXT("TaintExpGeoA"), TEXT("TaintExpGeoB"),
		TEXT("TaintExpTorus")})
	{
		const FWoTMeshEffectRow* Row = WoTMeshFX::Find(Config);
		TestTrue(FString::Printf(TEXT("%s is a lit row"), Config), Row && !Row->bUnlit);
	}
	TestTrue(TEXT("LSRing is an unlit row"),
		WoTMeshFX::Find(TEXT("LSRing")) && WoTMeshFX::Find(TEXT("LSRing"))->bUnlit);
	// A lit effect's fade scales only its lights (MLScaleGlow); that is whole
	// only while no lit row carries AmbientGlow into its baked ambient.
	for (const TCHAR* Config : {TEXT("SoulBarbSkull"), TEXT("SoulBarbImpact"),
		TEXT("AbsorbVisual"), TEXT("TaintExpDarts"), TEXT("TaintExpTorus"),
		TEXT("TaintExpGeoA"), TEXT("TaintExpGeoB"), TEXT("FrozenPresentation"),
		TEXT("AirShieldVisual"), TEXT("EarthShieldVisual"), TEXT("FireShieldVisual"),
		TEXT("SpiritShieldVisual"), TEXT("WaterShieldVisual"), TEXT("LSImpact"),
		TEXT("LSRing"), TEXT("BFExpGlobe"), TEXT("PixieJar")})
	{
		if (const FWoTMeshEffectRow* Row = WoTMeshFX::Find(Config))
		{
			AddInfo(FString::Printf(TEXT("%s: style %d unlit %d ScaleGlow %.2f AmbientGlow %.0f"),
				Config, static_cast<int32>(Row->Style), Row->bUnlit ? 1 : 0,
				Row->ScaleGlow, Row->AmbientGlow));
			TestTrue(FString::Printf(TEXT("lit %s carries no AmbientGlow"), Config),
				Row->bUnlit || Row->AmbientGlow == 0.0f);
		}
	}

	// The Taint visual's comets (TaintVisual.uc:32-40): bUnlit, ScaleGlow 5,
	// so GUnlitColor 1 before and after a bake.
	AWoTTaintVisual* Taint = World->SpawnActor<AWoTTaintVisual>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("Taint visual spawned"), Taint))
	{
		TArray<UWoTMeshLight*> TaintLights;
		Taint->GetComponents<UWoTMeshLight>(TaintLights);
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			for (UWoTVertexAnimComponent* Comet : {Taint->CometA.Get(),
				Taint->CometB.Get(), Taint->CometC.Get()})
			{
				UMaterialInterface* Material = Comet ? Comet->GetMaterial(0) : nullptr;
				TestEqual(TEXT("Taint comet keeps the unlit translucent master"),
					Root(Material), FString(TEXT("M_WoT_MeshTranslucent")));
				TestTrue(TEXT("Taint comet renders at GUnlitColor 1"),
					Vector(Material, TEXT("MLAmbient")).Equals(
						FLinearColor(1.0f, 1.0f, 1.0f, 1.0f), 1.0e-4f));
			}
			if (TaintLights.Num() == 1) TaintLights[0]->ForceRuntimeBake();
		}
		TestEqual(TEXT("Taint visual is baked against the map table"), TaintLights.Num(), 1);
		// The centre is its own STY_Modulated sprite actor in retail
		// (TaintVisual.uc:72-75): the visual's actor-level translucent Style
		// must not re-master its quad (the packaged reels' black square).
		TInlineComponentArray<UMeshComponent*> TaintMeshes(Taint);
		const UMeshComponent* CenterMod = nullptr;
		for (const UMeshComponent* Mesh : TaintMeshes)
		{
			if (Mesh->GetName() == TEXT("Center_Mod"))
			{
				CenterMod = Mesh;
			}
		}
		if (TestNotNull(TEXT("Taint centre quad"), CenterMod))
		{
			TestEqual(TEXT("Taint centre keeps the modulate master after a bake"),
				Root(CenterMod->GetMaterial(0)), FString(TEXT("M_WoT_MeshModulate")));
		}
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLateBoundWeaveMeshBakesSpec,
	"WoT.MeshLight.LateBoundWeaveMeshBakes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTLateBoundWeaveMeshBakesSpec::RunTest(const FString&)
{
	// Weave and FX actors bind their mesh in the actor's BeginPlay, after the
	// component BeginPlay already baked the mesh light against no mesh. The
	// Fireball LavaRock (lit, ScaleGlow 5, AmbientGlow 60) drew black until
	// the post-spawn timer re-bake, most of its 0.5 s life. The bound skin must
	// carry its bake at spawn, the same values a re-bake gives.
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	FWoTLightRec Lamp;
	Lamp.Pos = FVector(0.0, 0.0, 200.0);
	Lamp.Radius = 1000.0f;
	Lamp.InvRadius = 1.0f / Lamp.Radius;
	Lamp.Color = FLinearColor(0.8f, 0.2f, 0.1f, 1.0f);
	Lamp.Brightness = 64;
	Lamp.bEmits = true;
	Table->Lights = {Lamp};
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 0.0)};
	Table->ZoneBspFront = {-1};
	Table->ZoneBspBack = {-1};
	Table->LeafBspLeaves = {0, 0};
	Table->LeafBspCsg = {0};
	Table->bBspRootOutside = true;
	Table->LeafLightOffsets = {0, 1};
	Table->LeafLightIndices = {0};
	FWoTZoneAmbient Zone;
	Zone.Zone = 0;
	Zone.bDefault = true;
	Zone.Color = FLinearColor(0.05f, 0.05f, 0.05f, 1.0f);
	Table->ZoneAmbients = {Zone};
	UWoTMeshLight::RegisterRuntimeMapLightTable(World, Table);

	auto Vector = [](const UMaterialInterface* Material, const TCHAR* Name)
	{
		FLinearColor Value = FLinearColor::Transparent;
		if (Material) Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Name), Value);
		return Value;
	};
	auto Scalar = [](const UMaterialInterface* Material, const TCHAR* Name)
	{
		float Value = -1.0f;
		if (Material) Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value);
		return Value;
	};
	auto BoundSkin = [](AActor* Actor) -> UMaterialInterface*
	{
		TArray<UWoTVertexAnimComponent*> Visuals;
		if (Actor) Actor->GetComponents<UWoTVertexAnimComponent>(Visuals);
		for (UWoTVertexAnimComponent* Visual : Visuals)
		{
			if (Visual && Visual->GetSkeletalMeshAsset() && Visual->GetNumMaterials() > 0)
			{
				return Visual->GetMaterial(0);
			}
		}
		return nullptr;
	};
	auto SpawnedBakeMatchesReBake = [&](AActor* Actor, const FString& Label)
	{
		UMaterialInterface* Spawned = BoundSkin(Actor);
		if (!TestNotNull(Label + TEXT(" binds its weave mesh"), Spawned)) return;
		const FLinearColor SpawnAmbient = Vector(Spawned, TEXT("MLAmbient"));
		const float SpawnScaleGlow = Scalar(Spawned, TEXT("MLScaleGlow"));
		UWoTMeshLight* MeshLight = Actor->FindComponentByClass<UWoTMeshLight>();
		if (!TestNotNull(Label + TEXT(" is baked against the map table"), MeshLight)) return;
		MeshLight->ForceRuntimeBake();
		UMaterialInterface* ReBaked = BoundSkin(Actor);
		TestTrue(Label + TEXT(" carries its baked ambient at spawn"),
			SpawnAmbient.Equals(Vector(ReBaked, TEXT("MLAmbient")), 1.0e-4f));
		TestEqual(Label + TEXT(" carries its baked ScaleGlow at spawn"),
			SpawnScaleGlow, Scalar(ReBaked, TEXT("MLScaleGlow")));
	};

	AWoTFireballProjectile* Fireball = World->SpawnActor<AWoTFireballProjectile>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("Fireball spawns"), Fireball))
	{
		// AngrealFireballProjectile.uc defaults: LavaRock, ScaleGlow 5,
		// AmbientGlow 60, lit Style 1.
		UMaterialInterface* Rock = BoundSkin(Fireball);
		TestEqual(TEXT("LavaRock carries ScaleGlow 5 at spawn"),
			Scalar(Rock, TEXT("MLScaleGlow")), 5.0f);
		TestTrue(TEXT("LavaRock carries zone ambient + AmbientGlow 60 at spawn"),
			Vector(Rock, TEXT("MLAmbient")).R >= 60.0f / 255.0f);
		SpawnedBakeMatchesReBake(Fireball, TEXT("Fireball LavaRock"));
	}
	SpawnedBakeMatchesReBake(World->SpawnActor<AWoTTracerProjectile>(
		FVector(300, 0, 100), FRotator::ZeroRotator), TEXT("Tracer"));
	SpawnedBakeMatchesReBake(World->SpawnActor<AWoTWallOfAirProjectile>(
		FVector(600, 0, 100), FRotator::ZeroRotator), TEXT("Wall of Air"));
	// Not a weave projectile: a plain actor that binds its LavaRock in BeginPlay.
	SpawnedBakeMatchesReBake(World->SpawnActor<AWoTEarthTremorChunk>(
		FVector(900, 0, 100), FRotator::ZeroRotator), TEXT("Earth Tremor chunk"));
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWhirlwindFunnelTranslucentSpec,
	"WoT.MeshLight.WhirlwindFunnelTranslucent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTWhirlwindFunnelTranslucentSpec::RunTest(const FString&)
{
	// Retail's funnel is WhirlwindSprayer: STY_Translucent (ParticleSprayer
	// default), bUnlit, WhirlwindSlice mesh particles skinned WWtop at template
	// ScaleGlow 1, so GUnlitColor = clamp(0.5 * 1 + 0) = 0.5, drawn additive.
	// The port drew the slices opaque: dark jagged shards around the victim.
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	FWoTLightRec Lamp;
	Lamp.Pos = FVector(0.0, 0.0, 200.0);
	Lamp.Radius = 1000.0f;
	Lamp.InvRadius = 1.0f / Lamp.Radius;
	Lamp.Color = FLinearColor(0.8f, 0.2f, 0.1f, 1.0f);
	Lamp.Brightness = 64;
	Lamp.bEmits = true;
	Table->Lights = {Lamp};
	Table->ZoneBspPlanes = {FVector4(1.0, 0.0, 0.0, 0.0)};
	Table->ZoneBspFront = {-1};
	Table->ZoneBspBack = {-1};
	Table->LeafBspLeaves = {0, 0};
	Table->LeafBspCsg = {0};
	Table->bBspRootOutside = true;
	Table->LeafLightOffsets = {0, 1};
	Table->LeafLightIndices = {0};
	FWoTZoneAmbient Zone;
	Zone.Zone = 0;
	Zone.bDefault = true;
	Zone.Color = FLinearColor(0.05f, 0.05f, 0.05f, 1.0f);
	Table->ZoneAmbients = {Zone};
	UWoTMeshLight::RegisterRuntimeMapLightTable(World, Table);

	AWoTWhirlwindFunnel* Funnel = World->SpawnActor<AWoTWhirlwindFunnel>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("funnel spawns"), Funnel))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TArray<UWoTVertexAnimComponent*> Slices;
	Funnel->GetComponents<UWoTVertexAnimComponent>(Slices);
	TestTrue(TEXT("funnel carries its slices"), Slices.Num() > 0);
	int32 Checked = 0;
	for (UWoTVertexAnimComponent* Slice : Slices)
	{
		UMaterialInterface* Material = Slice ? Slice->GetMaterial(0) : nullptr;
		if (!Material) continue;
		const UMaterialInterface* Root = Material;
		while (const UMaterialInstance* Instance = Cast<UMaterialInstance>(Root))
		{
			if (!IsValid(Instance->Parent)) break;
			Root = Instance->Parent.Get();
		}
		FLinearColor Ambient = FLinearColor::Transparent;
		Material->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("MLAmbient")), Ambient);
		TestEqual(TEXT("slice draws on the unlit translucent master"),
			Root ? Root->GetName() : FString(), FString(TEXT("M_WoT_MeshTranslucent")));
		TestTrue(TEXT("slice renders at GUnlitColor 0.5"),
			Ambient.Equals(FLinearColor(0.5f, 0.5f, 0.5f, 1.0f), 1.0e-4f));
		++Checked;
	}
	TestEqual(TEXT("every slice is checked"), Checked, Slices.Num());
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
