#include "WoTPayloadSubsystem.h"

#include "WoTBSPPayload.h"

#include "PhysicsEngine/BodySetup.h"

#include "AI/NavigationSystemBase.h"
#include "Chaos/ChaosArchive.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GenericPlatform/GenericPlatformProperties.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "RHI.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/FortniteValkyrieBranchObjectVersion.h"
#include "UObject/UObjectIterator.h"
#include "WoTSha256.h"

#include <atomic>

// UE 5.8 exposes the async cook but not a public import/export method for the
// finished Chaos arrays. The BSP rebuild path uses an isolated, explicit-template
// private-member adapter below; the derived key contains the complete engine
// build identity, so an engine update cannot consume these private-layout
// bytes. A future engine integration can replace this with a tiny public API.
DEFINE_LOG_CATEGORY_STATIC(LogWoTBSPRuntime, Log, All);

namespace
{
	std::atomic<int32> GBSPProofRenderBuilds{0};
	std::atomic<int32> GBSPProofCollisionCooks{0};
	std::atomic<int32> GBSPProofDerivedLoads{0};
	std::atomic<int32> GBSPProofDerivedWrites{0};

	template<typename Tag, typename Tag::Type Member>
	struct TPrivateMemberAccessor
	{
		friend typename Tag::Type GetPrivateMember(Tag) { return Member; }
	};

	struct FTriMeshesTag
	{
		using Type = TArray<Chaos::FTriangleMeshImplicitObjectPtr> UBodySetup::*;
		friend Type GetPrivateMember(FTriMeshesTag);
	};
	template struct TPrivateMemberAccessor<
		FTriMeshesTag, &UBodySetup::TriMeshGeometries>;

	struct FUVInfoTag
	{
		using Type = FBodySetupUVInfo UBodySetup::*;
		friend Type GetPrivateMember(FUVInfoTag);
	};
	template struct TPrivateMemberAccessor<FUVInfoTag, &UBodySetup::UVInfo>;

	struct FFaceRemapTag
	{
		using Type = TArray<int32> UBodySetup::*;
		friend Type GetPrivateMember(FFaceRemapTag);
	};
	template struct TPrivateMemberAccessor<FFaceRemapTag, &UBodySetup::FaceRemap>;

	TArray<Chaos::FTriangleMeshImplicitObjectPtr>& TriMeshes(UBodySetup& Body)
	{
		return Body.*GetPrivateMember(FTriMeshesTag());
	}

	FBodySetupUVInfo& UVInfo(UBodySetup& Body)
	{
		return Body.*GetPrivateMember(FUVInfoTag());
	}

	TArray<int32>& FaceRemap(UBodySetup& Body)
	{
		return Body.*GetPrivateMember(FFaceRemapTag());
	}

	// The serialized field layout remains version 1. The v2 storage namespace
	// below invalidates files captured from the old half-precision render buffer;
	// newly captured UV values are full-precision floats.
	constexpr uint16 DerivedVersion = 1;
	constexpr uint32 MaxDerivedBytes = 512u * 1024u * 1024u;
	constexpr uint32 RtMaxVertices = 2000000;
	constexpr uint32 RtMaxIndices = 6000000;
	constexpr uint32 RtMaxSections = 4096;
	constexpr float RtMaxCoordinateAbs = 10000000.0f;
	constexpr uint32 DerivedHeaderBytes = 152;
	constexpr uint32 DerivedVertexBytes = 64;
	constexpr uint32 DigestTextBytes = 64;

	struct FDerivedRenderData
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> TangentX;
		TArray<FVector3f> TangentY;
		TArray<FVector3f> TangentZ;
		TArray<FVector2f> UV0;
		TArray<FVector2f> UV1;
		TArray<uint32> Indices;
		TArray<uint8> CollisionBytes;
	};

	bool RtCheckedAdd(uint64 A, uint64 B, uint64& Out)
	{
		if (B > MAX_uint64 - A) return false;
		Out = A + B;
		return true;
	}

	bool RtCheckedMultiply(uint64 A, uint64 B, uint64& Out)
	{
		if (A != 0 && B > MAX_uint64 / A) return false;
		Out = A * B;
		return true;
	}

#if !WOT_GUARDED_BOOTSTRAP
	void WriteU16(uint16 Value, TArray<uint8>& Out)
	{
		Out.Add(Value & 0xff); Out.Add((Value >> 8) & 0xff);
	}

	void WriteU32(uint32 Value, TArray<uint8>& Out)
	{
		for (int32 Byte = 0; Byte < 4; ++Byte)
			Out.Add(static_cast<uint8>((Value >> (Byte * 8)) & 0xff));
	}

	void WriteFloat(float Value, TArray<uint8>& Out)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, 4);
		WriteU32(Bits, Out);
	}

	bool RtReadU16(const TArray<uint8>& Bytes, uint64& Offset, uint16& Out)
	{
		uint64 End = 0;
		if (!RtCheckedAdd(Offset, 2, End) || End > static_cast<uint64>(Bytes.Num()))
			return false;
		const int32 At = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(At) || !Bytes.IsValidIndex(At + 1)) return false;
		Out = static_cast<uint16>(Bytes[At])
			| (static_cast<uint16>(Bytes[At + 1]) << 8);
		Offset = End;
		return true;
	}

	bool RtReadU32(const TArray<uint8>& Bytes, uint64& Offset, uint32& Out)
	{
		uint64 End = 0;
		if (!RtCheckedAdd(Offset, 4, End) || End > static_cast<uint64>(Bytes.Num()))
			return false;
		const int32 At = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(At) || !Bytes.IsValidIndex(At + 1)
			|| !Bytes.IsValidIndex(At + 2) || !Bytes.IsValidIndex(At + 3)) return false;
		Out = static_cast<uint32>(Bytes[At])
			| (static_cast<uint32>(Bytes[At + 1]) << 8)
			| (static_cast<uint32>(Bytes[At + 2]) << 16)
			| (static_cast<uint32>(Bytes[At + 3]) << 24);
		Offset = End;
		return true;
	}

	bool RtReadFloat(const TArray<uint8>& Bytes, uint64& Offset, float& Out)
	{
		uint32 Bits = 0;
		if (!RtReadU32(Bytes, Offset, Bits)) return false;
		FMemory::Memcpy(&Out, &Bits, 4);
		return FMath::IsFinite(Out);
	}

	bool WriteASCII64(const FString& Value, TArray<uint8>& Out)
	{
		if (Value.Len() != 64) return false;
		for (TCHAR Character : Value)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
			Out.Add(static_cast<uint8>(Character));
		}
		return true;
	}

	bool ReadASCII64(const TArray<uint8>& Bytes, uint64& Offset, FString& Out)
	{
		uint64 End = 0;
		if (!RtCheckedAdd(Offset, 64, End) || End > static_cast<uint64>(Bytes.Num()))
			return false;
		Out.Reset(64);
		for (; Offset < End; ++Offset)
		{
			const int32 At = static_cast<int32>(Offset);
			if (!Bytes.IsValidIndex(At)) return false;
			const uint8 Character = Bytes[At];
			if (!((Character >= '0' && Character <= '9')
				|| (Character >= 'a' && Character <= 'f'))) return false;
			Out.AppendChar(static_cast<TCHAR>(Character));
		}
		return true;
	}
#endif // !WOT_GUARDED_BOOTSTRAP

	bool ValidatePayloadGeometry(const FWoTBSPPayloadData& Payload, FString& Error)
	{
		const int32 VertexCount = Payload.Positions.Num();
		const int32 IndexCount = Payload.Indices.Num();
		const int32 SectionCount = Payload.Sections.Num();
		if (VertexCount <= 0 || static_cast<uint32>(VertexCount) > RtMaxVertices
			|| IndexCount < 3 || static_cast<uint32>(IndexCount) > RtMaxIndices
			|| IndexCount % 3 != 0 || SectionCount <= 0
			|| static_cast<uint32>(SectionCount) > RtMaxSections)
		{
			Error = TEXT("BSP geometry has invalid empty/oversized counts");
			return false;
		}
		if (Payload.Normals.Num() != VertexCount || Payload.UV0.Num() != VertexCount
			|| Payload.UV1.Num() != VertexCount)
		{
			Error = TEXT("BSP normal/UV arrays are shorter than the position array");
			return false;
		}
		if (!FMath::IsFinite(Payload.BoundsMin.X)
			|| !FMath::IsFinite(Payload.BoundsMin.Y)
			|| !FMath::IsFinite(Payload.BoundsMin.Z)
			|| !FMath::IsFinite(Payload.BoundsMax.X)
			|| !FMath::IsFinite(Payload.BoundsMax.Y)
			|| !FMath::IsFinite(Payload.BoundsMax.Z)
			|| Payload.BoundsMin.X > Payload.BoundsMax.X
			|| Payload.BoundsMin.Y > Payload.BoundsMax.Y
			|| Payload.BoundsMin.Z > Payload.BoundsMax.Z
			|| Payload.BoundsMin.GetAbsMax() > RtMaxCoordinateAbs
			|| Payload.BoundsMax.GetAbsMax() > RtMaxCoordinateAbs)
		{
			Error = TEXT("BSP geometry has invalid bounds");
			return false;
		}
		for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			if (!Payload.Positions.IsValidIndex(Vertex)
				|| !Payload.Normals.IsValidIndex(Vertex)
				|| !Payload.UV0.IsValidIndex(Vertex)
				|| !Payload.UV1.IsValidIndex(Vertex))
			{
				Error = FString::Printf(
					TEXT("BSP vertex %d is missing a required attribute"), Vertex);
				return false;
			}
			const FVector3f& Position = Payload.Positions[Vertex];
			const FVector3f& Normal = Payload.Normals[Vertex];
			const FVector2f& UV0 = Payload.UV0[Vertex];
			const FVector2f& UV1 = Payload.UV1[Vertex];
			if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y)
				|| !FMath::IsFinite(Position.Z) || !FMath::IsFinite(Normal.X)
				|| !FMath::IsFinite(Normal.Y) || !FMath::IsFinite(Normal.Z)
				|| !FMath::IsFinite(UV0.X) || !FMath::IsFinite(UV0.Y)
				|| !FMath::IsFinite(UV1.X) || !FMath::IsFinite(UV1.Y)
				|| Position.GetAbsMax() > RtMaxCoordinateAbs
				|| !FMath::IsNearlyEqual(Normal.SquaredLength(), 1.0f, 0.02f))
			{
				Error = FString::Printf(
					TEXT("BSP vertex %d contains a non-finite attribute"), Vertex);
				return false;
			}
		}
		uint64 ExpectedFirst = 0;
		for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
		{
			if (!Payload.Sections.IsValidIndex(SectionIndex))
			{
				Error = FString::Printf(TEXT("BSP section %d is absent"), SectionIndex);
				return false;
			}
			const FWoTBSPPayloadSection& Section = Payload.Sections[SectionIndex];
			uint64 SectionEnd = 0;
			if (Section.MaterialSlot.IsEmpty() || Section.FirstIndex != ExpectedFirst
				|| Section.IndexCount == 0 || Section.IndexCount % 3 != 0
				|| !RtCheckedAdd(Section.FirstIndex, Section.IndexCount, SectionEnd)
				|| SectionEnd > static_cast<uint64>(IndexCount)
				|| Section.MinVertex > Section.MaxVertex
				|| Section.MaxVertex >= static_cast<uint32>(VertexCount)
				|| Section.bCollisionEnabled == Payload.bPortal)
			{
				Error = FString::Printf(
					TEXT("BSP section %d has an invalid range"), SectionIndex);
				return false;
			}
			uint32 Minimum = MAX_uint32;
			uint32 Maximum = 0;
			for (uint64 At = Section.FirstIndex; At < SectionEnd; ++At)
			{
				const int32 IndexAt = static_cast<int32>(At);
				if (!Payload.Indices.IsValidIndex(IndexAt))
				{
					Error = FString::Printf(
						TEXT("BSP section %d index range exceeds the stream"), SectionIndex);
					return false;
				}
				const uint32 VertexIndex = Payload.Indices[IndexAt];
				if (VertexIndex >= static_cast<uint32>(VertexCount)
					|| !Payload.Positions.IsValidIndex(static_cast<int32>(VertexIndex))
					|| !Payload.Normals.IsValidIndex(static_cast<int32>(VertexIndex))
					|| !Payload.UV0.IsValidIndex(static_cast<int32>(VertexIndex))
					|| !Payload.UV1.IsValidIndex(static_cast<int32>(VertexIndex)))
				{
					Error = FString::Printf(
						TEXT("BSP section %d references missing vertex %u"),
						SectionIndex, VertexIndex);
					return false;
				}
				Minimum = FMath::Min(Minimum, VertexIndex);
				Maximum = FMath::Max(Maximum, VertexIndex);
			}
			if (Minimum != Section.MinVertex || Maximum != Section.MaxVertex)
			{
				Error = FString::Printf(
					TEXT("BSP section %d min/max does not match its indices"), SectionIndex);
				return false;
			}
			ExpectedFirst = SectionEnd;
		}
		if (ExpectedFirst != static_cast<uint64>(IndexCount))
		{
			Error = TEXT("BSP sections do not exactly cover the index stream");
			return false;
		}
		return true;
	}

	bool ValidateEntrySections(const FWoTBSPPayloadEntry& Entry, FString& Error)
	{
		const int32 SectionCount = static_cast<int32>(Entry.SectionCount);
		if (Entry.SectionCount == 0 || Entry.SectionCount > RtMaxSections
			|| Entry.MaterialSlots.Num() != SectionCount
			|| Entry.SectionFirstIndices.Num() != SectionCount
			|| Entry.SectionIndexCounts.Num() != SectionCount
			|| Entry.SectionMinVertices.Num() != SectionCount
			|| Entry.SectionMaxVertices.Num() != SectionCount
			|| Entry.SectionCollisionEnabled.Num() != SectionCount)
		{
			Error = TEXT("BSP manifest section arrays are empty or not equally dense");
			return false;
		}
		return true;
	}

#if !WOT_GUARDED_BOOTSTRAP
	FString EngineBuildIdentity()
	{
		const FEngineVersion& Version = FEngineVersion::Current();
		return FString::Printf(TEXT("%s|build=%s|cl=%u|compatible=%u"),
			*Version.ToString(), FApp::GetBuildVersion(), Version.GetChangelist(),
			FEngineVersion::CompatibleWith().GetChangelist());
	}

	FString DerivedKey(const FWoTBSPPayloadEntry& Entry)
	{
		return UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
			Entry.BlobSHA256, EngineBuildIdentity(),
			FString(FPlatformProperties::IniPlatformName()), DerivedVersion,
			Entry.TangentAlgorithm);
	}

	FString DerivedFilename(const FString& CacheDirectory, const FString& Key)
	{
		return FPaths::Combine(CacheDirectory, TEXT("derived"), TEXT("bsps"),
			TEXT("v2"), Key + TEXT(".wotbspdc"));
	}

	bool ReadFileBounded(const FString& Filename, TArray<uint8>& Out,
		FString& Error)
	{
		TUniquePtr<IFileHandle> File(
			FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Filename));
		if (!File)
		{
			Error = TEXT("derived file is absent");
			return false;
		}
		const int64 Size = File->Size();
		if (Size < DerivedHeaderBytes + DigestTextBytes
			|| Size > MaxDerivedBytes || Size > MAX_int32)
		{
			Error = TEXT("derived file size is invalid");
			return false;
		}
		Out.SetNumUninitialized(static_cast<int32>(Size));
		if (Out.IsEmpty() || !Out.GetData()
			|| !File->Read(Out.GetData(), Size) || File->Tell() != Size)
		{
			Out.Reset();
			Error = TEXT("derived file read was incomplete");
			return false;
		}
		return true;
	}

	bool AtomicWrite(const FString& Filename, const TArray<uint8>& Bytes,
		FString& Error)
	{
		GBSPProofDerivedWrites.fetch_add(1);
		if (Bytes.IsEmpty() || static_cast<uint64>(Bytes.Num()) > MaxDerivedBytes
			|| !Bytes.GetData())
		{
			Error = TEXT("refusing to publish empty or oversized derived bytes");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		const FString Temporary = FString::Printf(TEXT("%s.tmp.%u.%llu"),
			*Filename, FPlatformProcess::GetCurrentProcessId(),
			static_cast<unsigned long long>(FPlatformTime::Cycles64()));
		TUniquePtr<IFileHandle> File(
			FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Temporary));
		if (!File || !File->Write(Bytes.GetData(), Bytes.Num()) || !File->Flush())
		{
			IFileManager::Get().Delete(*Temporary);
			Error = TEXT("cannot flush temporary derived file");
			return false;
		}
		File.Reset();
		if (!IFileManager::Get().Move(*Filename, *Temporary, true, true, false, true))
		{
			IFileManager::Get().Delete(*Temporary);
			Error = TEXT("cannot atomically publish derived file");
			return false;
		}
		return true;
	}

	bool RtDeleteDerivedTemps(const FString& Filename, FString& Error)
	{
		const FString Directory = FPaths::GetPath(Filename);
		const FString Pattern = FPaths::GetCleanFilename(Filename) + TEXT(".tmp.*");
		TArray<FString> TemporaryFiles;
		IFileManager::Get().FindFiles(TemporaryFiles, *(Directory / Pattern), true, false);
		for (const FString& TemporaryFile : TemporaryFiles)
		{
			if (!IFileManager::Get().Delete(*(Directory / TemporaryFile), false, true, true))
			{
				Error = FString::Printf(TEXT("cannot remove stale derived temporary %s"),
					*(Directory / TemporaryFile));
				return false;
			}
		}
		return true;
	}

	bool SerializeCollision(UBodySetup* BodySetup, TArray<uint8>& Out,
		FString& Error)
	{
		Out.Reset();
		if (!BodySetup || TriMeshes(*BodySetup).IsEmpty())
		{
			Error = TEXT("completed body setup has no Chaos triangle mesh");
			return false;
		}
		FMemoryWriter Writer(Out, true);
		Writer.UsingCustomVersion(FFortniteValkyrieBranchObjectVersion::GUID);
		Chaos::FChaosArchive Archive(Writer);
		Archive << TriMeshes(*BodySetup)
			<< UVInfo(*BodySetup) << FaceRemap(*BodySetup);
		if (Writer.IsError() || Out.IsEmpty()
			|| static_cast<uint64>(Out.Num()) > MaxDerivedBytes)
		{
			Out.Reset(); Error = TEXT("Chaos derived serialization failed");
			return false;
		}
		return true;
	}

	bool DeserializeCollision(const TArray<uint8>& Bytes, UBodySetup* BodySetup,
		FString& Error)
	{
		if (!BodySetup || Bytes.IsEmpty())
		{
			Error = TEXT("cached collision bytes are empty"); return false;
		}
		BodySetup->ClearPhysicsMeshes();
		FMemoryReader Reader(Bytes, true);
		Reader.UsingCustomVersion(FFortniteValkyrieBranchObjectVersion::GUID);
		Chaos::FChaosArchive Archive(Reader);
		Archive << TriMeshes(*BodySetup)
			<< UVInfo(*BodySetup) << FaceRemap(*BodySetup);
		if (Reader.IsError() || Reader.Tell() != Bytes.Num()
			|| TriMeshes(*BodySetup).IsEmpty())
		{
			BodySetup->ClearPhysicsMeshes();
			Error = TEXT("cached Chaos collision failed structural load");
			return false;
		}
		for (Chaos::FTriangleMeshImplicitObjectPtr& TriMesh : TriMeshes(*BodySetup))
		{
			if (!TriMesh || !TriMesh->IsValidGeometry())
			{
				BodySetup->ClearPhysicsMeshes();
				Error = TEXT("cached Chaos triangle mesh is invalid");
				return false;
			}
			TriMesh->SetDoCollide(false);
		}
		BodySetup->bCreatedPhysicsMeshes = true;
		BodySetup->bFailedToCreatePhysicsMeshes = false;
		BodySetup->bHasCookedCollisionData = true;
		return true;
	}
#endif // !WOT_GUARDED_BOOTSTRAP

	void CopyBodyPolicy(const UBodySetup* Source, UBodySetup& Destination)
	{
		if (!Source) return;
		// UBodySetup::CopyBodySetupProperty is editor-only. Mirror its policy
		// fields here, plus the runtime-cook retention flags that the proof needs.
		Destination.BoneName = Source->BoneName;
		Destination.PhysicsType = Source->PhysicsType;
		Destination.bConsiderForBounds = Source->bConsiderForBounds;
		Destination.bMeshCollideAll = Source->bMeshCollideAll;
		Destination.bDoubleSidedGeometry = Source->bDoubleSidedGeometry;
		Destination.bGenerateNonMirroredCollision =
			Source->bGenerateNonMirroredCollision;
		Destination.bSharedCookedData = Source->bSharedCookedData;
		Destination.bGenerateMirroredCollision = Source->bGenerateMirroredCollision;
		Destination.bSupportUVsAndFaceRemap = Source->bSupportUVsAndFaceRemap;
		Destination.bSupportFaceRemapOnMeshBVH = Source->bSupportFaceRemapOnMeshBVH;
		Destination.bSupportVertexRemap = Source->bSupportVertexRemap;
		Destination.bNeverNeedsCookedCollisionData = false;
		Destination.PhysMaterial = Source->PhysMaterial;
		Destination.CollisionReponse = Source->CollisionReponse;
		Destination.CollisionTraceFlag = Source->CollisionTraceFlag;
		Destination.DefaultInstance = Source->DefaultInstance;
		Destination.WalkableSlopeOverride = Source->WalkableSlopeOverride;
		Destination.BuildScale3D = Source->BuildScale3D;
	}

#if !WOT_GUARDED_BOOTSTRAP
	bool CaptureRender(UStaticMesh* Mesh, const FWoTBSPPayloadData& Payload,
		FDerivedRenderData& Out, FString& Error)
	{
		Out = FDerivedRenderData();
		if (!ValidatePayloadGeometry(Payload, Error)) return false;
		const FStaticMeshRenderData* Render = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!Render || Render->LODResources.Num() != 1)
		{
			Error = TEXT("rebuilt mesh has no single LOD0"); return false;
		}
		if (!Render->LODResources.IsValidIndex(0))
		{
			Error = TEXT("rebuilt mesh LOD0 array is empty"); return false;
		}
		const FStaticMeshLODResources& LOD = Render->LODResources[0];
		const uint32 VertexCount = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		// UE 5.8's BuildFromMeshDescription always emits MAX_STATIC_TEXCOORDS
		// channels: FStaticMeshVertexBuffer::Init(verts, InNumTexCoords, flags)
		// ignores InNumTexCoords and uses the fixed-size FStaticMeshBuildVertex
		// UV view (StaticMeshVertexBuffer.cpp:113-124). Channels 0/1 still hold
		// the payload UVs; the final mesh is normalized to exactly two channels
		// by InstallDerivedRender on every path.
		if (VertexCount != static_cast<uint32>(Payload.Positions.Num())
			|| LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices() != VertexCount
			|| LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() < 2
			|| LOD.IndexBuffer.GetNumIndices() != static_cast<uint32>(Payload.Indices.Num())
			|| LOD.Sections.Num() != Payload.Sections.Num())
		{
			Error = FString::Printf(
				TEXT("rebuilt render counts disagree with authenticated payload ")
				TEXT("(verts %u/%d smverts %u uv %u indices %u/%d sections %d/%d)"),
				VertexCount, Payload.Positions.Num(),
				LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices(),
				LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords(),
				LOD.IndexBuffer.GetNumIndices(), Payload.Indices.Num(),
				LOD.Sections.Num(), Payload.Sections.Num());
			return false;
		}
		Out.Positions.Reserve(VertexCount);
		Out.TangentX.Reserve(VertexCount);
		Out.TangentY.Reserve(VertexCount);
		Out.TangentZ.Reserve(VertexCount);
		Out.UV0.Reserve(VertexCount);
		Out.UV1.Reserve(VertexCount);
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			Out.Positions.Add(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex));
			Out.TangentX.Add(FVector3f(
				LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Vertex)));
			Out.TangentY.Add(
				LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Vertex));
			Out.TangentZ.Add(FVector3f(
				LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Vertex)));
			Out.UV0.Add(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, 0));
			Out.UV1.Add(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, 1));
		}
		LOD.IndexBuffer.GetCopy(Out.Indices);
		if (Out.Indices != Payload.Indices)
		{
			Error = TEXT("rebuilt render indices disagree with payload order"); return false;
		}
		for (int32 Index = 0; Index < LOD.Sections.Num(); ++Index)
		{
			if (!LOD.Sections.IsValidIndex(Index) || !Payload.Sections.IsValidIndex(Index))
			{
				Error = FString::Printf(
					TEXT("rebuilt render section %d is missing"), Index);
				return false;
			}
			const FStaticMeshSection& Actual = LOD.Sections[Index];
			const FWoTBSPPayloadSection& Expected = Payload.Sections[Index];
			// bEnableCollision is deliberately NOT compared: the 5.8 fast build
			// hardcodes it to true for every section (StaticMesh.cpp
			// BuildFromMeshDescription); the authenticated per-section policy is
			// stamped by InstallDerivedRender, which every path finishes through.
			if (Actual.MaterialIndex != Index || Actual.FirstIndex != Expected.FirstIndex
				|| Actual.NumTriangles != Expected.IndexCount / 3
				|| Actual.MinVertexIndex != Expected.MinVertex
				|| Actual.MaxVertexIndex != Expected.MaxVertex)
			{
				Error = FString::Printf(
					TEXT("rebuilt render section %d disagrees with payload"), Index);
				return false;
			}
		}
		return true;
	}

	bool EncodeDerived(const FString& PayloadSHA, const FString& CacheKey,
		const FDerivedRenderData& Data, bool bHasCollision,
		TArray<uint8>& Out, FString& Error)
	{
		Out.Reset();
		const uint32 VertexCount = Data.Positions.Num();
		const uint32 IndexCount = Data.Indices.Num();
		if (VertexCount == 0 || VertexCount > RtMaxVertices
			|| IndexCount < 3 || IndexCount > RtMaxIndices || IndexCount % 3
			|| Data.TangentX.Num() != VertexCount || Data.TangentY.Num() != VertexCount
			|| Data.TangentZ.Num() != VertexCount || Data.UV0.Num() != VertexCount
			|| Data.UV1.Num() != VertexCount
			|| bHasCollision != !Data.CollisionBytes.IsEmpty())
		{
			Error = TEXT("invalid completed data for derived encoding"); return false;
		}
		uint64 VertexBytes = 0;
		uint64 IndexBytes = 0;
		uint64 ExpectedBytes = 0;
		if (!RtCheckedMultiply(VertexCount, DerivedVertexBytes, VertexBytes)
			|| !RtCheckedMultiply(IndexCount, 4, IndexBytes)
			|| !RtCheckedAdd(DerivedHeaderBytes, VertexBytes, ExpectedBytes)
			|| !RtCheckedAdd(ExpectedBytes, IndexBytes, ExpectedBytes)
			|| !RtCheckedAdd(ExpectedBytes, Data.CollisionBytes.Num(), ExpectedBytes)
			|| !RtCheckedAdd(ExpectedBytes, DigestTextBytes, ExpectedBytes)
			|| ExpectedBytes > MaxDerivedBytes)
		{
			Error = TEXT("derived output byte count overflowed the compiled cap");
			return false;
		}
		for (int32 IndexAt = 0; IndexAt < Data.Indices.Num(); ++IndexAt)
		{
			if (!Data.Indices.IsValidIndex(IndexAt)
				|| Data.Indices[IndexAt] >= VertexCount)
			{
				Error = FString::Printf(
					TEXT("derived index %d references a missing vertex"), IndexAt);
				return false;
			}
		}
		static constexpr uint8 RtMagic[8] = {'W','O','T','B','S','P','D','C'};
		Out.Append(RtMagic, 8);
		WriteU16(DerivedVersion, Out);
		WriteU16(bHasCollision ? 1 : 0, Out);
		WriteU32(VertexCount, Out);
		WriteU32(IndexCount, Out);
		WriteU32(Data.CollisionBytes.Num(), Out);
		if (!WriteASCII64(PayloadSHA, Out) || !WriteASCII64(CacheKey, Out))
		{
			Out.Reset(); Error = TEXT("derived payload/cache key is not lowercase SHA-256");
			return false;
		}
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			const int32 VertexIndex = static_cast<int32>(Vertex);
			if (!Data.Positions.IsValidIndex(VertexIndex)
				|| !Data.TangentX.IsValidIndex(VertexIndex)
				|| !Data.TangentY.IsValidIndex(VertexIndex)
				|| !Data.TangentZ.IsValidIndex(VertexIndex)
				|| !Data.UV0.IsValidIndex(VertexIndex)
				|| !Data.UV1.IsValidIndex(VertexIndex))
			{
				Out.Reset(); Error = TEXT("derived vertex arrays became non-dense");
				return false;
			}
			for (const FVector3f& Value : {Data.Positions[VertexIndex],
				Data.TangentX[VertexIndex], Data.TangentY[VertexIndex],
				Data.TangentZ[VertexIndex]})
			{
				WriteFloat(Value.X, Out); WriteFloat(Value.Y, Out); WriteFloat(Value.Z, Out);
			}
			for (const FVector2f& Value : {Data.UV0[VertexIndex], Data.UV1[VertexIndex]})
			{
				WriteFloat(Value.X, Out); WriteFloat(Value.Y, Out);
			}
		}
		for (uint32 Index : Data.Indices) WriteU32(Index, Out);
		Out.Append(Data.CollisionBytes);
		if (Out.Num() > static_cast<int32>(MaxDerivedBytes - DigestTextBytes))
		{
			Out.Reset(); Error = TEXT("derived file exceeds compiled cap"); return false;
		}
		const FString Digest = WoTSha256::HashBuffer(Out.GetData(), Out.Num());
		if (!WriteASCII64(Digest, Out))
		{
			Out.Reset(); Error = TEXT("derived digest construction failed"); return false;
		}
		return true;
	}

	bool DecodeDerived(const TArray<uint8>& Bytes, const FString& PayloadSHA,
		const FString& CacheKey, bool bExpectCollision,
		FDerivedRenderData& Out, FString& Error)
	{
		Out = FDerivedRenderData();
		if (Bytes.Num() < DerivedHeaderBytes + DigestTextBytes
			|| static_cast<uint64>(Bytes.Num()) > MaxDerivedBytes
			|| !Bytes.GetData())
		{
			Error = TEXT("derived file is truncated or oversized"); return false;
		}
		const int32 DigestOffset = Bytes.Num() - DigestTextBytes;
		FString StoredDigest;
		uint64 DigestReadOffset = DigestOffset;
		if (!ReadASCII64(Bytes, DigestReadOffset, StoredDigest)
			|| StoredDigest != WoTSha256::HashBuffer(Bytes.GetData(), DigestOffset))
		{
			Error = TEXT("derived file digest mismatch"); return false;
		}
		static constexpr uint8 RtMagic[8] = {'W','O','T','B','S','P','D','C'};
		if (FMemory::Memcmp(Bytes.GetData(), RtMagic, 8) != 0)
		{
			Error = TEXT("derived file magic mismatch"); return false;
		}
		uint64 Offset = 8;
		uint16 Version = 0, Flags = 0;
		uint32 VertexCount = 0, IndexCount = 0, CollisionBytes = 0;
		FString StoredPayloadSHA, StoredKey;
		if (!RtReadU16(Bytes, Offset, Version) || !RtReadU16(Bytes, Offset, Flags)
			|| !RtReadU32(Bytes, Offset, VertexCount) || !RtReadU32(Bytes, Offset, IndexCount)
			|| !RtReadU32(Bytes, Offset, CollisionBytes)
			|| !ReadASCII64(Bytes, Offset, StoredPayloadSHA)
			|| !ReadASCII64(Bytes, Offset, StoredKey)
			|| Version != DerivedVersion || (Flags & ~1) != 0
			|| static_cast<bool>(Flags & 1) != bExpectCollision
			|| StoredPayloadSHA != PayloadSHA || StoredKey != CacheKey
			|| VertexCount == 0 || VertexCount > RtMaxVertices
			|| IndexCount < 3 || IndexCount > RtMaxIndices || IndexCount % 3
			|| (bExpectCollision ? CollisionBytes == 0 : CollisionBytes != 0))
		{
			Error = TEXT("derived header/key/count contract mismatch"); return false;
		}
		uint64 VertexBytes = 0, IndexBytes = 0, ExpectedEnd = 0;
		if (!RtCheckedMultiply(VertexCount, DerivedVertexBytes, VertexBytes)
			|| !RtCheckedMultiply(IndexCount, 4, IndexBytes)
			|| !RtCheckedAdd(Offset, VertexBytes, ExpectedEnd)
			|| !RtCheckedAdd(ExpectedEnd, IndexBytes, ExpectedEnd)
			|| !RtCheckedAdd(ExpectedEnd, CollisionBytes, ExpectedEnd)
			|| ExpectedEnd != static_cast<uint64>(DigestOffset))
		{
			Error = TEXT("derived stream lengths overflow or disagree"); return false;
		}
		FDerivedRenderData Decoded;
		Decoded.Positions.SetNumUninitialized(VertexCount);
		Decoded.TangentX.SetNumUninitialized(VertexCount);
		Decoded.TangentY.SetNumUninitialized(VertexCount);
		Decoded.TangentZ.SetNumUninitialized(VertexCount);
		Decoded.UV0.SetNumUninitialized(VertexCount);
		Decoded.UV1.SetNumUninitialized(VertexCount);
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			const int32 VertexIndex = static_cast<int32>(Vertex);
			if (!Decoded.Positions.IsValidIndex(VertexIndex)
				|| !Decoded.TangentX.IsValidIndex(VertexIndex)
				|| !Decoded.TangentY.IsValidIndex(VertexIndex)
				|| !Decoded.TangentZ.IsValidIndex(VertexIndex)
				|| !Decoded.UV0.IsValidIndex(VertexIndex)
				|| !Decoded.UV1.IsValidIndex(VertexIndex))
			{
				Error = TEXT("derived vertex allocation is shorter than declared"); return false;
			}
			for (FVector3f* Value : {&Decoded.Positions[VertexIndex],
				&Decoded.TangentX[VertexIndex], &Decoded.TangentY[VertexIndex],
				&Decoded.TangentZ[VertexIndex]})
			{
				if (!RtReadFloat(Bytes, Offset, Value->X) || !RtReadFloat(Bytes, Offset, Value->Y)
					|| !RtReadFloat(Bytes, Offset, Value->Z))
				{
					Error = TEXT("non-finite derived render basis"); return false;
				}
			}
			for (FVector2f* Value : {&Decoded.UV0[VertexIndex], &Decoded.UV1[VertexIndex]})
			{
				if (!RtReadFloat(Bytes, Offset, Value->X) || !RtReadFloat(Bytes, Offset, Value->Y))
				{
					Error = TEXT("non-finite derived UV"); return false;
				}
			}
			if (!FMath::IsNearlyEqual(
					Decoded.TangentX[VertexIndex].SquaredLength(), 1.0f, 0.05f)
				|| !FMath::IsNearlyEqual(
					Decoded.TangentY[VertexIndex].SquaredLength(), 1.0f, 0.05f)
				|| !FMath::IsNearlyEqual(
					Decoded.TangentZ[VertexIndex].SquaredLength(), 1.0f, 0.05f))
			{
				Error = TEXT("invalid derived tangent basis"); return false;
			}
		}
		Decoded.Indices.SetNumUninitialized(IndexCount);
		for (uint32& Index : Decoded.Indices)
		{
			if (!RtReadU32(Bytes, Offset, Index) || Index >= VertexCount)
			{
				Error = TEXT("invalid derived index stream"); return false;
			}
		}
		uint64 CollisionEnd = 0;
		if (!RtCheckedAdd(Offset, CollisionBytes, CollisionEnd)
			|| CollisionEnd != static_cast<uint64>(DigestOffset))
		{
			Error = TEXT("derived collision range is outside the authenticated bytes");
			return false;
		}
		if (CollisionBytes > 0)
		{
			const int32 CollisionOffset = static_cast<int32>(Offset);
			if (!Bytes.IsValidIndex(CollisionOffset)
				|| !Bytes.IsValidIndex(static_cast<int32>(CollisionEnd - 1)))
			{
				Error = TEXT("derived collision bytes are truncated"); return false;
			}
			Decoded.CollisionBytes.Append(
				Bytes.GetData() + CollisionOffset, CollisionBytes);
		}
		Out = MoveTemp(Decoded);
		GBSPProofDerivedLoads.fetch_add(1);
		return true;
	}
#endif // !WOT_GUARDED_BOOTSTRAP

	bool BuildMeshDescription(const FWoTBSPPayloadData& Payload,
		FMeshDescription& MeshDescription, FString& Error)
	{
		if (!ValidatePayloadGeometry(Payload, Error)) return false;
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register();
		auto VertexPositions = Attributes.GetVertexPositions();
		auto InstanceNormals = Attributes.GetVertexInstanceNormals();
		auto InstanceUVs = Attributes.GetVertexInstanceUVs();
		auto MaterialSlots = Attributes.GetPolygonGroupMaterialSlotNames();
		InstanceUVs.SetNumChannels(2);
		TArray<FVertexInstanceID> Instances;
		Instances.Reserve(Payload.Positions.Num());
		for (int32 Source = 0; Source < Payload.Positions.Num(); ++Source)
		{
			if (!Payload.Positions.IsValidIndex(Source)
				|| !Payload.Normals.IsValidIndex(Source)
				|| !Payload.UV0.IsValidIndex(Source)
				|| !Payload.UV1.IsValidIndex(Source))
			{
				Error = FString::Printf(TEXT("BSP vertex %d is missing an attribute"), Source);
				return false;
			}
			const FVertexID Vertex = MeshDescription.CreateVertex();
			VertexPositions[Vertex] = Payload.Positions[Source];
			const FVertexInstanceID Instance =
				MeshDescription.CreateVertexInstance(Vertex);
			InstanceNormals[Instance] = Payload.Normals[Source];
			InstanceUVs.Set(Instance, 0, Payload.UV0[Source]);
			InstanceUVs.Set(Instance, 1, Payload.UV1[Source]);
			Instances.Add(Instance);
		}
		for (int32 SectionIndex = 0; SectionIndex < Payload.Sections.Num(); ++SectionIndex)
		{
			if (!Payload.Sections.IsValidIndex(SectionIndex))
			{
				Error = FString::Printf(TEXT("BSP section %d is absent"), SectionIndex);
				return false;
			}
			const FWoTBSPPayloadSection& Section = Payload.Sections[SectionIndex];
			const FPolygonGroupID Group = MeshDescription.CreatePolygonGroup();
			MaterialSlots[Group] = FName(*Section.MaterialSlot);
			uint64 SectionEnd = 0;
			if (!RtCheckedAdd(Section.FirstIndex, Section.IndexCount, SectionEnd)
				|| SectionEnd > static_cast<uint64>(Payload.Indices.Num()))
			{
				Error = FString::Printf(
					TEXT("BSP section %d range exceeds the index stream"), SectionIndex);
				return false;
			}
			for (uint64 At = Section.FirstIndex; At < SectionEnd; At += 3)
			{
				FVertexInstanceID TriangleInstances[3];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint64 IndexAt = At + static_cast<uint64>(Corner);
					if (IndexAt >= SectionEnd
						|| !Payload.Indices.IsValidIndex(static_cast<int32>(IndexAt)))
					{
						Error = FString::Printf(
							TEXT("BSP section %d has a truncated triangle"), SectionIndex);
						return false;
					}
					const uint32 Source = Payload.Indices[static_cast<int32>(IndexAt)];
					if (!Instances.IsValidIndex(static_cast<int32>(Source)))
					{
						Error = FString::Printf(
							TEXT("BSP section %d triangle references missing vertex %u"),
							SectionIndex, Source);
						return false;
					}
					TriangleInstances[Corner] = Instances[static_cast<int32>(Source)];
				}
				MeshDescription.CreateTriangle(
					Group, MakeArrayView(TriangleInstances, 3));
			}
		}
		FStaticMeshOperations::ComputeMikktTangents(
			MeshDescription, /*bIgnoreDegenerateTriangles*/ true);
		// Real BSP payloads contain zero-area sliver triangles (they survive
		// bsp2obj and ship in reference A's cooked data with zeroed basis).
		// Mikk skips them, leaving zero tangents that the invalid-basis check
		// below would reject — synthesize a normal-perpendicular basis so the
		// invisible slivers cannot fail the whole build. Authored normals are
		// never modified.
		{
			FStaticMeshAttributes SanitizeAttributes(MeshDescription);
			auto Normals = SanitizeAttributes.GetVertexInstanceNormals();
			auto Tangents = SanitizeAttributes.GetVertexInstanceTangents();
			auto Signs = SanitizeAttributes.GetVertexInstanceBinormalSigns();
			for (const FVertexInstanceID Instance :
				MeshDescription.VertexInstances().GetElementIDs())
			{
				// UNCONDITIONAL Gram-Schmidt: a tangent that is zero, non-
				// unit, or merely NEAR-parallel to the normal decodes to a
				// non-unit packed binormal, and the derived-cache preflight
				// then rejects the file on every later launch — the mesh
				// silently rebuilds forever. Caught twice by the cached-
				// launch gate; the second time Battle2_02's nearly-parallel
				// sliver tangents slipped past a cross-product threshold.
				// Orthonormalizing every corner is a within-precision no-op
				// for healthy Mikk output and guarantees a unit
				// perpendicular basis everywhere.
				const FVector3f Normal = Normals[Instance];
				FVector3f Orthogonal = Tangents[Instance]
					- Normal * FVector3f::DotProduct(Normal, Tangents[Instance]);
				if (!Orthogonal.Normalize(1e-4f))
				{
					const FVector3f Reference =
						FMath::Abs(Normal.Z) < 0.9f ? FVector3f::ZAxisVector
													: FVector3f::XAxisVector;
					Orthogonal = FVector3f::CrossProduct(
						Normal, Reference).GetSafeNormal();
					if (Orthogonal.IsNearlyZero())
					{
						Orthogonal = FVector3f::XAxisVector;
					}
				}
				Tangents[Instance] = Orthogonal;
				if (Signs[Instance] == 0.0f) Signs[Instance] = 1.0f;
			}
		}
		bool bBadNormals = false, bBadTangents = false;
		FStaticMeshOperations::HasInvalidVertexInstanceNormalsOrTangents(
			MeshDescription, bBadNormals, bBadTangents);
		if (bBadNormals || bBadTangents)
		{
			Error = TEXT("Mikk tangent construction produced invalid basis");
			return false;
		}
		return true;
	}

#if !WOT_GUARDED_BOOTSTRAP
	bool InstallDerivedRender(UStaticMesh* Mesh,
		const FWoTBSPPayloadData& Payload, const FDerivedRenderData& Data,
		FString& Error)
	{
		if (!Mesh)
		{
			Error = TEXT("cannot install derived render into a null mesh"); return false;
		}
		if (!ValidatePayloadGeometry(Payload, Error)) return false;
		if (Data.Positions.Num() != Payload.Positions.Num()
			|| Data.TangentX.Num() != Data.Positions.Num()
			|| Data.TangentY.Num() != Data.Positions.Num()
			|| Data.TangentZ.Num() != Data.Positions.Num()
			|| Data.UV0.Num() != Data.Positions.Num()
			|| Data.UV1.Num() != Data.Positions.Num()
			|| Data.Positions != Payload.Positions || Data.Indices != Payload.Indices)
		{
			Error = TEXT("cached render does not match payload topology"); return false;
		}
		auto UVMatchesRaw = [](const FVector2f& Cached, const FVector2f& Raw)
		{
			return FMath::Abs(Cached.X - Raw.X)
				<= FMath::Max(0.001f, FMath::Abs(Raw.X) * 0.001f)
				&& FMath::Abs(Cached.Y - Raw.Y)
				<= FMath::Max(0.001f, FMath::Abs(Raw.Y) * 0.001f);
		};
		for (int32 Vertex = 0; Vertex < Data.Positions.Num(); ++Vertex)
		{
			if (!Data.TangentZ.IsValidIndex(Vertex)
				|| !Data.UV0.IsValidIndex(Vertex) || !Data.UV1.IsValidIndex(Vertex)
				|| !Payload.Normals.IsValidIndex(Vertex)
				|| !Payload.UV0.IsValidIndex(Vertex) || !Payload.UV1.IsValidIndex(Vertex))
			{
				Error = FString::Printf(
					TEXT("cached render vertex %d is missing basis/UV data"), Vertex);
				return false;
			}
			if (FVector3f::DotProduct(Data.TangentZ[Vertex], Payload.Normals[Vertex])
				< 0.99f
				|| !UVMatchesRaw(Data.UV0[Vertex], Payload.UV0[Vertex])
				|| !UVMatchesRaw(Data.UV1[Vertex], Payload.UV1[Vertex]))
			{
				Error = TEXT("cached render basis/UVs disagree with the raw payload");
				return false;
			}
		}
		TOptional<FStaticMeshComponentRecreateRenderStateContext> Context;
		if (Mesh->AreRenderingResourcesInitialized())
		{
			Context.Emplace(Mesh, true, true);
			Mesh->ReleaseResources();
			Mesh->ReleaseResourcesFence.Wait();
		}
		Mesh->SetRenderData(MakeUnique<FStaticMeshRenderData>());
		FStaticMeshRenderData* Render = Mesh->GetRenderData();
		if (!Render)
		{
			Error = TEXT("failed to allocate derived render data"); return false;
		}
		Render->AllocateLODResources(1);
		if (!Render->LODResources.IsValidIndex(0))
		{
			Error = TEXT("failed to allocate derived render LOD0"); return false;
		}
		FStaticMeshLODResources& LOD = Render->LODResources[0];
		LOD.VertexBuffers.PositionVertexBuffer.Init(Data.Positions, true);
		// Canonical two-channel packed layout. The fast build transiently emits
		// MAX_STATIC_TEXCOORDS channels (5.8 Init ignores InNumTexCoords), so
		// BOTH the first-build and cached paths finish through this install to
		// end with identical render data.
		// UV1 is not merely a sampled texture coordinate: the world material
		// differentiates it and inverts that Jacobian to recover the authored UE1
		// meshel lattice. Half precision can merge the close UVs of skinny BSP
		// triangles while their world positions remain distinct, producing enormous
		// reconstructed light samples and a zero dynamic-light contribution.
		LOD.VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(true);
		LOD.VertexBuffers.StaticMeshVertexBuffer.Init(Data.Positions.Num(), 2, true);
		for (int32 Vertex = 0; Vertex < Data.Positions.Num(); ++Vertex)
		{
			if (!Data.TangentX.IsValidIndex(Vertex)
				|| !Data.TangentY.IsValidIndex(Vertex)
				|| !Data.TangentZ.IsValidIndex(Vertex)
				|| !Data.UV0.IsValidIndex(Vertex) || !Data.UV1.IsValidIndex(Vertex))
			{
				Error = FString::Printf(
					TEXT("cached render vertex %d became non-dense"), Vertex);
				return false;
			}
			LOD.VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(
				Vertex, Data.TangentX[Vertex], Data.TangentY[Vertex], Data.TangentZ[Vertex]);
			LOD.VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(Vertex, 0, Data.UV0[Vertex]);
			LOD.VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(Vertex, 1, Data.UV1[Vertex]);
		}
		LOD.VertexBuffers.ColorVertexBuffer.InitFromSingleColor(
			FColor::White, Data.Positions.Num());
		uint32 MaximumIndex = 0;
		for (uint32 Index : Data.Indices) MaximumIndex = FMath::Max(MaximumIndex, Index);
		const EIndexBufferStride::Type IndexStride = MaximumIndex > MAX_uint16
			? EIndexBufferStride::Force32Bit : EIndexBufferStride::Force16Bit;
		LOD.IndexBuffer.TrySetAllowCPUAccess(true);
		LOD.IndexBuffer.SetIndices(Data.Indices, IndexStride);
		// The project's pinned Mac package leaves both optional index-buffer
		// CVars at their engine default (off), matching the first fast build.
		LOD.bHasDepthOnlyIndices = false;
		LOD.bHasColorVertexData = true;
		LOD.DepthOnlyNumTriangles = 0;
		LOD.MaxDeviation = 0.0f;
		const FBox BoundsBox(FVector(Payload.BoundsMin), FVector(Payload.BoundsMax));
		LOD.SourceMeshBounds = FBoxSphereBounds(BoundsBox);
		for (int32 SectionIndex = 0; SectionIndex < Payload.Sections.Num(); ++SectionIndex)
		{
			if (!Payload.Sections.IsValidIndex(SectionIndex))
			{
				Error = FString::Printf(
					TEXT("cached render section %d is absent"), SectionIndex);
				return false;
			}
			const FWoTBSPPayloadSection& Source = Payload.Sections[SectionIndex];
			FStaticMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
			Section.MaterialIndex = SectionIndex;
			Section.FirstIndex = Source.FirstIndex;
			Section.NumTriangles = Source.IndexCount / 3;
			Section.MinVertexIndex = Source.MinVertex;
			Section.MaxVertexIndex = Source.MaxVertex;
			Section.bEnableCollision = Source.bCollisionEnabled;
		}
		Render->Bounds = FBoxSphereBounds(BoundsBox);
		Render->ScreenSize[0].Default = 1.0f;
		Render->CurrentFirstLODIdx = 0;
		Mesh->bAllowCPUAccess = true;
		Mesh->NeverStream = true;
		if (Mesh->bSupportRayTracing && IsRayTracingAllowed())
		{
			Render->InitializeRayTracingRepresentationFromRenderingLODs();
		}
		Mesh->InitResources();
		Mesh->CalculateExtendedBounds();
		return true;
	}
#endif // !WOT_GUARDED_BOOTSTRAP

	void RefreshPhysicsAndNavigation(UStaticMesh* Mesh)
	{
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!IsValid(Component) || Component->GetStaticMesh() != Mesh
				|| Component->HasAnyFlags(RF_ClassDefaultObject)) continue;
			if (Component->IsPhysicsStateCreated()) Component->RecreatePhysicsState();
			FNavigationSystem::UpdateComponentData(*Component);
		}
	}

	bool BindingMatches(const UWoTBSPPayloadAssetUserData& Binding,
		UStaticMesh* Mesh, const FWoTBSPPayloadEntry& Entry,
		const FWoTBSPPayloadData& Payload, FString& Error)
	{
		if (!Binding.ValidateDescriptor(Mesh, Error)
			|| !ValidateEntrySections(Entry, Error)
			|| !ValidatePayloadGeometry(Payload, Error)) return false;
		if (Binding.PayloadKey != Entry.CacheKey || Binding.Role != Entry.Role
			|| Binding.VertexCount != static_cast<int32>(Entry.VertexCount)
			|| Binding.IndexCount != static_cast<int32>(Entry.IndexCount)
			|| FVector3f(Binding.BoundsMin) != Entry.BoundsMin
			|| FVector3f(Binding.BoundsMax) != Entry.BoundsMax
			|| Binding.TangentAlgorithm != Entry.TangentAlgorithm
			|| Binding.MaterialSlots != Entry.MaterialSlots
			|| Payload.Positions.Num() != Binding.VertexCount
			|| Payload.Indices.Num() != Binding.IndexCount)
		{
			Error = TEXT("carrier, manifest and BSP blob descriptors disagree");
			return false;
		}
		return true;
	}
}

bool WoTBuildBSPRenderMeshOnly(
	UStaticMesh& Mesh, const FWoTBSPPayloadData& Payload, FString& OutError)
{
	if (!IsInGameThread())
	{
		OutError = TEXT("BSP render construction requires the game thread");
		return false;
	}
	FMeshDescription Description;
	if (!BuildMeshDescription(Payload, Description, OutError)) return false;
	// Runtime fast-build resolves each polygon group's material name against
	// this table. A fresh mesh has no slots, leaving cooked sections at -1
	// and rendering the default material despite component overrides.
	Mesh.GetStaticMaterials().Reset(Payload.Sections.Num());
	for (const FWoTBSPPayloadSection& Section : Payload.Sections)
	{
		Mesh.GetStaticMaterials().Emplace(nullptr, FName(*Section.MaterialSlot));
	}
	Mesh.bAllowCPUAccess = true;
	Mesh.NeverStream = true;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	Params.bBuildSimpleCollision = false;
	Params.bAllowCpuAccess = true;
	Params.PerLODOverrides.AddDefaulted_GetRef().bUseFullPrecisionUVs = true;
	const TArray<const FMeshDescription*> Descriptions = {&Description};
	if (!Mesh.BuildFromMeshDescriptions(Descriptions, Params))
	{
		OutError = TEXT("BSP render mesh build failed");
		return false;
	}
	Mesh.SetBodySetup(nullptr);
	return true;
}

bool UWoTPayloadSubsystem::InjectBSPStaticMesh(
	UStaticMesh* Mesh, const FString& CacheKey)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogWoTBSPRuntime, Error,
			TEXT("Rejecting BSP injection off the game thread: %s"), *CacheKey);
		return false;
	}
	if (!bInjectionEnabled || !bManifestReady || !IsValid(Mesh)
		|| InjectedBSPMeshes.Contains(FObjectKey(Mesh))) return false;
	const FWoTBSPPayloadEntry* Entry = BSPEntries.Find(CacheKey);
	const UWoTBSPPayloadAssetUserData* Binding =
		Cast<UWoTBSPPayloadAssetUserData>(Mesh->GetAssetUserDataOfClass(
			UWoTBSPPayloadAssetUserData::StaticClass()));
	const TSharedPtr<const FWoTBSPPayloadData> Payload = LoadBSPPayload(CacheKey);
	FString Error;
	if (!Entry || !Binding || !Payload.IsValid()
		|| !BindingMatches(*Binding, Mesh, *Entry, *Payload, Error))
	{
		FailedRequiredBSPKeys.Add(CacheKey);
		UE_LOG(LogWoTBSPRuntime, Error, TEXT("BSP injection preflight failed for %s: %s"),
			*CacheKey, *Error);
		return false;
	}
	// Claims this UObject for one immutable payload generation. Readiness is a
	// separate key set and is published only after render plus collision/nav.
	InjectedBSPMeshes.Add(FObjectKey(Mesh));
	if (!Payload->bPortal)
	{
		// The cooked carrier may have cached navigation for its tiny triangles.
		// Keep the mesh navigation-relevant, but force the post-cook update to
		// export the installed real BodySetup instead of stale UNavCollision data.
		Mesh->SetNavCollision(nullptr);
	}
	const double Started = FPlatformTime::Seconds();
#if !WOT_GUARDED_BOOTSTRAP
	const FString CacheKeyHash = DerivedKey(*Entry);
	if (CacheKeyHash.Len() != 64)
	{
		FailedRequiredBSPKeys.Add(CacheKey);
		UE_LOG(LogWoTBSPRuntime, Error,
			TEXT("BSP derived-key construction failed for %s"), *CacheKey);
		return false;
	}
	const FString Filename = DerivedFilename(CacheDirectory, CacheKeyHash);
	TArray<uint8> FileBytes;
	FDerivedRenderData Derived;
	if (!RtDeleteDerivedTemps(Filename, Error))
	{
		FailedRequiredBSPKeys.Add(CacheKey);
		UE_LOG(LogWoTBSPRuntime, Error,
			TEXT("BSP derived temporary cleanup failed for %s: %s"),
			*CacheKey, *Error);
		return false;
	}
	if (ReadFileBounded(Filename, FileBytes, Error)
		&& DecodeDerived(FileBytes, Entry->BlobSHA256, CacheKeyHash,
			!Payload->bPortal, Derived, Error)
		&& Derived.Positions.Num() == Payload->Positions.Num()
		&& Derived.Indices == Payload->Indices
		&& InstallDerivedRender(Mesh, *Payload, Derived, Error))
	{
		bool bCollisionInstalled = Payload->bPortal;
		if (!Payload->bPortal)
		{
			UBodySetup* Body = NewObject<UBodySetup>(Mesh);
			CopyBodyPolicy(Mesh->GetBodySetup(), *Body);
			Body->AggGeom.EmptyElements();
			Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			if (!DeserializeCollision(Derived.CollisionBytes, Body, Error))
			{
				UE_LOG(LogWoTBSPRuntime, Warning,
					TEXT("Discarding BSP derived collision %s: %s"),
					*Filename, *Error);
				IFileManager::Get().Delete(*Filename);
			}
			else
			{
				Mesh->SetBodySetup(Body);
				RefreshPhysicsAndNavigation(Mesh);
				bCollisionInstalled = true;
			}
		}
		if (bCollisionInstalled)
		{
			InjectedBSPMeshes.Add(FObjectKey(Mesh));
			ReadyBSPKeys.Add(CacheKey);
			UE_LOG(LogWoTBSPRuntime, Display,
				TEXT("WOT-BSP-PROOF key=%s path=derived load_ms=%.3f file=%s"),
				*CacheKey,
				(FPlatformTime::Seconds() - Started) * 1000.0, *Filename);
			return true;
		}
	}
	else if (IFileManager::Get().FileExists(*Filename))
	{
		UE_LOG(LogWoTBSPRuntime, Warning,
			TEXT("Discarding invalid BSP derived cache %s: %s"),
			*Filename, *Error);
		IFileManager::Get().Delete(*Filename);
	}
#endif
	const double BuildStarted = FPlatformTime::Seconds();
	FMeshDescription Description;
	if (!BuildMeshDescription(*Payload, Description, Error))
	{
		FailedRequiredBSPKeys.Add(CacheKey);
		UE_LOG(LogWoTBSPRuntime, Error, TEXT("BSP MeshDescription failed: %s"), *Error);
		return false;
	}
	Mesh->bAllowCPUAccess = true;
	Mesh->NeverStream = true;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false;
	Params.bBuildSimpleCollision = false;
	Params.bAllowCpuAccess = true;
	Params.PerLODOverrides.AddDefaulted_GetRef().bUseFullPrecisionUVs = true;
	const TArray<const FMeshDescription*> Descriptions = {&Description};
	GBSPProofRenderBuilds.fetch_add(1);
	if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params))
	{
		FailedRequiredBSPKeys.Add(CacheKey);
		UE_LOG(LogWoTBSPRuntime, Error, TEXT("BSP fast render build failed for %s"), *CacheKey);
		return false;
	}
	const double RenderMilliseconds =
		(FPlatformTime::Seconds() - BuildStarted) * 1000.0;
	if (Payload->bPortal)
	{
#if !WOT_GUARDED_BOOTSTRAP
		if (!CaptureRender(Mesh, *Payload, Derived, Error)
			|| !InstallDerivedRender(Mesh, *Payload, Derived, Error)
			|| !EncodeDerived(Entry->BlobSHA256, CacheKeyHash, Derived, false,
				FileBytes, Error)
			|| !RtDeleteDerivedTemps(Filename, Error)
			|| !AtomicWrite(Filename, FileBytes, Error))
		{
			FailedRequiredBSPKeys.Add(CacheKey);
			UE_LOG(LogWoTBSPRuntime, Error,
				TEXT("Portal derived publication failed for %s: %s"),
				*CacheKey, *Error);
			return false;
		}
#endif
		InjectedBSPMeshes.Add(FObjectKey(Mesh));
		ReadyBSPKeys.Add(CacheKey);
		// The whole UE_LOG is duplicated per branch rather than #if-ing the
		// format argument inside the invocation: preprocessor directives inside
		// a macro's argument list are undefined behavior that Clang happens to
		// expand and MSVC rejects outright (C2760 et al.).
#if !WOT_GUARDED_BOOTSTRAP
		UE_LOG(LogWoTBSPRuntime, Display,
			TEXT("WOT-BSP-PROOF key=%s path=build render_ms=%.3f collision_ms=0 total_ms=%.3f derived=written"),
			*CacheKey, RenderMilliseconds,
			(FPlatformTime::Seconds() - Started) * 1000.0);
#else
		UE_LOG(LogWoTBSPRuntime, Display,
			TEXT("WOT-BSP-PROOF key=%s path=verified-first-use render_ms=%.3f collision_ms=0 total_ms=%.3f"),
			*CacheKey, RenderMilliseconds,
			(FPlatformTime::Seconds() - Started) * 1000.0);
#endif
		return true;
	}

	UBodySetup* Pending = NewObject<UBodySetup>(Mesh);
	CopyBodyPolicy(Mesh->GetBodySetup(), *Pending);
	Pending->AggGeom.EmptyElements();
	Pending->CollisionTraceFlag = CTF_UseComplexAsSimple;
	PendingBSPBodySetups.Add(Pending);
	TWeakObjectPtr<UWoTPayloadSubsystem> WeakThis(this);
	TWeakObjectPtr<UStaticMesh> WeakMesh(Mesh);
	const double CollisionStarted = FPlatformTime::Seconds();
	GBSPProofCollisionCooks.fetch_add(1);
	Pending->CreatePhysicsMeshesAsync(FOnAsyncPhysicsCookFinished::CreateLambda(
#if !WOT_GUARDED_BOOTSTRAP
		[WeakThis, WeakMesh, Pending, Payload, EntryCopy = *Entry, CacheKeyHash,
		 Filename, Started, RenderMilliseconds, CollisionStarted](bool bSuccess)
#else
		[WeakThis, WeakMesh, Pending, EntryCopy = *Entry,
		 Started, RenderMilliseconds, CollisionStarted](bool bSuccess)
#endif
		{
			UWoTPayloadSubsystem* Self = WeakThis.Get();
			UStaticMesh* StrongMesh = WeakMesh.Get();
			if (!Self) return;
			Self->PendingBSPBodySetups.RemoveSingleSwap(Pending);
			if (!StrongMesh)
			{
				Self->FailedRequiredBSPKeys.Add(EntryCopy.CacheKey);
				return;
			}
#if !WOT_GUARDED_BOOTSTRAP
			FString CallbackError;
			FDerivedRenderData Completed;
			TArray<uint8> Encoded;
			if (!bSuccess)
			{
				CallbackError = TEXT("asynchronous Chaos triangle cook failed");
			}
			if (!bSuccess
				|| !CaptureRender(
					StrongMesh, *Payload, Completed, CallbackError)
				|| !SerializeCollision(
					Pending, Completed.CollisionBytes, CallbackError)
				|| !InstallDerivedRender(
					StrongMesh, *Payload, Completed, CallbackError)
				|| !EncodeDerived(
					EntryCopy.BlobSHA256, CacheKeyHash, Completed, true,
					Encoded, CallbackError)
				|| !RtDeleteDerivedTemps(Filename, CallbackError)
				|| !AtomicWrite(Filename, Encoded, CallbackError))
			{
				Self->FailedRequiredBSPKeys.Add(EntryCopy.CacheKey);
				UE_LOG(LogWoTBSPRuntime, Error,
					TEXT("BSP collision/derived completion failed for %s: %s"),
					*EntryCopy.CacheKey, *CallbackError);
				return;
			}
#else
			if (!bSuccess)
			{
				Self->FailedRequiredBSPKeys.Add(EntryCopy.CacheKey);
				UE_LOG(LogWoTBSPRuntime, Error,
					TEXT("BSP collision completion failed for %s"),
					*EntryCopy.CacheKey);
				return;
			}
#endif
			StrongMesh->SetBodySetup(Pending);
			RefreshPhysicsAndNavigation(StrongMesh);
			Self->InjectedBSPMeshes.Add(FObjectKey(StrongMesh));
			Self->ReadyBSPKeys.Add(EntryCopy.CacheKey);
			// Duplicated per branch, not #if-ed inside the argument list --
			// see the identical note at the synchronous site above.
#if !WOT_GUARDED_BOOTSTRAP
			UE_LOG(LogWoTBSPRuntime, Display,
				TEXT("WOT-BSP-PROOF key=%s path=build render_ms=%.3f collision_ms=%.3f total_ms=%.3f derived=written"),
				*EntryCopy.CacheKey, RenderMilliseconds,
				(FPlatformTime::Seconds() - CollisionStarted) * 1000.0,
				(FPlatformTime::Seconds() - Started) * 1000.0);
#else
			UE_LOG(LogWoTBSPRuntime, Display,
				TEXT("WOT-BSP-PROOF key=%s path=verified-first-use render_ms=%.3f collision_ms=%.3f total_ms=%.3f"),
				*EntryCopy.CacheKey, RenderMilliseconds,
				(FPlatformTime::Seconds() - CollisionStarted) * 1000.0,
				(FPlatformTime::Seconds() - Started) * 1000.0);
#endif
		}));
	return true;
}

FWoTBSPProofCounters UWoTPayloadSubsystem::GetBSPProofCounters()
{
	FWoTBSPProofCounters Result;
	Result.RenderBuilds = GBSPProofRenderBuilds.load();
	Result.CollisionCooks = GBSPProofCollisionCooks.load();
	Result.DerivedLoads = GBSPProofDerivedLoads.load();
	Result.DerivedWrites = GBSPProofDerivedWrites.load();
	return Result;
}

void UWoTPayloadSubsystem::TryBindBSPStaticMesh(UStaticMesh* Mesh)
{
	if (!IsValid(Mesh) || Mesh->HasAnyFlags(RF_ClassDefaultObject)) return;
	const UWoTBSPPayloadAssetUserData* Binding =
		Cast<UWoTBSPPayloadAssetUserData>(Mesh->GetAssetUserDataOfClass(
			UWoTBSPPayloadAssetUserData::StaticClass()));
	if (!Binding) return;
	bBSPCarrierSeen = true;
	if (InjectedBSPMeshes.Contains(FObjectKey(Mesh)))
	{
		return;
	}
	ReadyBSPKeys.Remove(Binding->PayloadKey);
	FailedRequiredBSPKeys.Remove(Binding->PayloadKey);
	if (!bInjectionEnabled || !bManifestReady)
	{
		FailedRequiredBSPKeys.Add(Binding->PayloadKey);
		return;
	}
	const FWoTBSPPayloadEntry* Entry = BSPEntries.Find(Binding->PayloadKey);
	if (!Entry || Entry->AssetPath != Mesh->GetPathName())
	{
		FailedRequiredBSPKeys.Add(Binding->PayloadKey);
		UE_LOG(LogWoTBSPRuntime, Error,
			TEXT("BSP carrier identity mismatch: %s -> %s"),
			*Mesh->GetPathName(), *Binding->PayloadKey);
		return;
	}
	InjectBSPStaticMesh(Mesh, Binding->PayloadKey);
}

void UWoTPayloadSubsystem::HandleWorldTickStart(
	UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (ActiveCacheLease.IsValid())
	{
		if (auto Failure = ActiveCacheLease->TakeReadFailure(); Failure.IsSet())
			HandleRuntimeIntegrityFailure(Failure.GetValue());
	}
	(void)TickType; (void)DeltaSeconds;
	// Tick start is the safe point for the packages queued by
	// HandlePackageLoadCompleted: no load flush is on the stack here, and the
	// scan must land before the barrier evaluation below so a newly loaded
	// carrier can hold the world in the same tick.
	DrainCompletedPackages();
	if (!bBSPCarrierSeen || !World || !World->IsGameWorld()
		|| World->GetGameInstance() != GetGameInstance()) return;
	FString MapName;
	if (!TryGetBSPMapFromWorldPackage(
			World->GetOutermost()->GetName(), MapName)) return;
	const TArray<FString>* Required = RequiredBSPKeysByMap.Find(MapName);
	if (!Required || Required->IsEmpty()) return;
	const bool bReady = IsBSPBarrierReadyForMap(MapName);
	if (!bReady)
	{
		if (!BSPPausedWorlds.Contains(World))
		{
			if (UGameplayStatics::SetGamePaused(World, true))
			{
				BSPPausedWorlds.Add(World);
				UE_LOG(LogWoTBSPRuntime, Display,
					TEXT("WOT-BSP-PROOF barrier=held world=%s"), *World->GetName());
			}
		}
		return;
	}
	if (BSPPausedWorlds.Remove(World) > 0)
	{
		UGameplayStatics::SetGamePaused(World, false);
		UE_LOG(LogWoTBSPRuntime, Display,
			TEXT("WOT-BSP-PROOF barrier=released world=%s"), *World->GetName());
	}
}
