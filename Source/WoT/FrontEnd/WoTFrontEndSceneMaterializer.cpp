#include "WoTFrontEndSceneMaterializer.h"

#include "WoTFrontEndSceneController.h"
#include "WoTFrontEndSky.h"
#include "WoTFrontEndGroundTrace.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../Payload/WoTCacheLease.h"
#include "../Payload/WoTFrontEndFontOwner.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeProceduralResource.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTBSPPayload.h"
#include "../WoTPayloadSoundWave.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTParticleSprayer.h"
#include "../World/WoTRuntimeParticleProjection.h"
#include "../WoTRainCyd.h"
#include "../WoTSkyBox.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeWorldProperties.h"

#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "UObject/StrongObjectPtr.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"

namespace
{
template <typename T>
TWoTBootstrapResult<T> SceneError(const TCHAR* Message)
{
	return TWoTBootstrapResult<T>::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

FWoTBootstrapStatus SceneStatus(const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message));
}

const TCHAR* ClassLeaf(const FString& Identity)
{
	const TCHAR* Start = *Identity;
	const TCHAR* Dot = FCString::Strrchr(Start, TCHAR('.'));
	return Dot != nullptr ? Dot + 1 : Start;
}

bool ScenePropertyVector(const FWoTRuntimeWorldProperty& Property,
	FVector& Out)
{
	return WoTRuntimeWorldPropertyVector(Property, Out);
}

bool ScenePropertyRotator(const FWoTRuntimeWorldProperty& Property,
	FRotator& Out)
{
	return WoTRuntimeWorldPropertyRotator(Property, Out);
}

bool ScenePropertyFloat(const FWoTRuntimeWorldProperty& Property, float& Out)
{
	return WoTRuntimeWorldPropertyFloat(Property, Out);
}

bool ScenePropertyByte(const FWoTRuntimeWorldProperty& Property, uint8& Out)
{
	return WoTRuntimeWorldPropertyByte(Property, Out);
}

bool ScenePropertyBool(const FWoTRuntimeWorldProperty& Property, bool& Out)
{
	return WoTRuntimeWorldPropertyBool(Property, Out);
}

bool ScenePropertyInteger(const FWoTRuntimeWorldProperty& Property, int32& Out)
{
	return WoTRuntimeWorldPropertyInteger(Property, Out);
}

bool ScenePropertyUE1String(const FWoTRuntimeWorldProperty& Property, FString& Out)
{
	return WoTRuntimeWorldPropertyUE1String(Property, Out);
}

const FWoTRuntimeWorldProperty* SceneFindProperty(
	const FWoTRuntimeWorldActor& Actor, FStringView Name, uint32 ArrayIndex = 0)
{
	return WoTRuntimeWorldFindProperty(Actor, Name, ArrayIndex);
}

FTransform SceneActorTransform(const FWoTRuntimeWorldActor& Actor)
{
	return WoTRuntimeWorldActorTransform(Actor);
}

FString SceneActorTag(const FWoTRuntimeWorldActor& Actor,
	const TCHAR* PropertyName)
{
	for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
	{
		if (Property.Name.Equals(PropertyName, ESearchCase::IgnoreCase))
			return Property.NameValue.ToLower();
	}
	return {};
}

FString SceneIdentityKey(FStringView Identity)
{
	FString Key;
	Key.Reserve(Identity.Len());
	for (const TCHAR Character : Identity)
	{
		if (FChar::IsAlnum(Character)) Key.AppendChar(FChar::ToLower(Character));
	}
	return Key;
}

FString SceneConverterSlotBase(FStringView SourceExport)
{
	return WoTRuntimeBSPConverterSlotBase(SourceExport);
}

bool SceneConverterSlotMatches(FStringView MaterialSlot, FStringView Base)
{
	return WoTRuntimeBSPConverterSlotMatches(MaterialSlot, Base);
}

int32 SceneResolveConverterSlot(FStringView MaterialSlot,
	TConstArrayView<FString> AuthenticatedSourceExports)
{
	return WoTRuntimeBSPResolveConverterSlot(
		MaterialSlot, AuthenticatedSourceExports);
}

TOptional<EWoTNeutralResourceKind> ResourceKind(EWoTFactDomain Domain)
{
	switch (Domain)
	{
	case EWoTFactDomain::Texture: return EWoTNeutralResourceKind::Texture;
	case EWoTFactDomain::Sound: return EWoTNeutralResourceKind::Sound;
	case EWoTFactDomain::Mesh: return EWoTNeutralResourceKind::VertexMesh;
	case EWoTFactDomain::Animation: return EWoTNeutralResourceKind::Animation;
	case EWoTFactDomain::BSP: return EWoTNeutralResourceKind::BSP;
	case EWoTFactDomain::World: return EWoTNeutralResourceKind::World;
	case EWoTFactDomain::Font: return EWoTNeutralResourceKind::Font;
	case EWoTFactDomain::Procedural: return EWoTNeutralResourceKind::Procedural;
	case EWoTFactDomain::Localization: return EWoTNeutralResourceKind::Localization;
	default: return {};
	}
}

struct FSceneLightPlan
{
	FTransform Transform;
	FString Identity;
	FString Tag;
	uint8 Brightness = 64, Hue = 0, Saturation = 255;
	FLinearColor Color = FLinearColor::White;
	float Intensity = 0.0f;
	float Radius = 0.0f;
	bool bLightningGated = false;
};

struct FSceneAmbientPlan
{
	FTransform Transform;
	FString SoundIdentity;
	float Volume = 1.0f;
	float Radius = 2400.0f;
	TStrongObjectPtr<UWoTPayloadSoundWave> Wave;
};

struct FSceneSplashPlan
{
	FTransform Transform;
	TArray<FWoTParticleTemplate> Templates;
	TArray<FString> TextureIdentities;
	TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> AddMaterials;
	TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> ModMaterials;
	float Spread = 45.0f;
	float Volume = 10.0f;
	float MinVolume = 0.0f;
	FVector Gravity = FVector::ZeroVector;
	float PrimeCount = 0.0f;
	float VolumeScalePct = 0.5f;
	float VisibilityRadius = 1600.0f;
	float VisibilityHeight = 1600.0f;
	bool bOn = true;
	bool bLOSClip = false;
	bool bLinearDistribution = false;
};

struct FSceneRainPlan
{
	FTransform Transform;
	float CylinderHeight = 48.0f;
	int32 StackNum = 5;
	float MinFallRate = 500.0f;
	float MaxFallRate = 700.0f;
	float TraceDepth = 1000.0f;
	float BuryDistance = 100.0f;
	float InitialYawDegrees = 0.0f;
	float InitialFallRate = 600.0f;
	float ScaleGlow = 1.0f;
	uint8 AmbientGlow = 0;
	TOptional<FVector> GroundHit;
	TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> Materials;
};

uint32 SceneRainSeed(const FWoTSha256& DescriptorRoot, int32 RainIndex)
{
	uint32 Seed = 2166136261u;
	for (uint8 Byte : DescriptorRoot.Bytes)
		Seed = (Seed ^ Byte) * 16777619u;
	return HashCombineFast(Seed, GetTypeHash(RainIndex));
}

bool SceneReadRainPlan(const FWoTRuntimeWorldActor& Actor, FSceneRainPlan& Out)
{
	Out.Transform = SceneActorTransform(Actor);
	auto Float = [&](const TCHAR* Name, float& Value)
	{
		if (const FWoTRuntimeWorldProperty* P = SceneFindProperty(Actor, Name))
			return ScenePropertyFloat(*P, Value);
		return true;
	};
	if (!Float(TEXT("CylinderHeight"), Out.CylinderHeight)
		|| !Float(TEXT("MinFallRate"), Out.MinFallRate)
		|| !Float(TEXT("MaxFallRate"), Out.MaxFallRate)
		|| !Float(TEXT("TraceDepth"), Out.TraceDepth)
		|| !Float(TEXT("BuryDistance"), Out.BuryDistance)
		|| !Float(TEXT("ScaleGlow"), Out.ScaleGlow)) return false;
	if (const FWoTRuntimeWorldProperty* Glow = SceneFindProperty(Actor, TEXT("AmbientGlow")))
		if (!ScenePropertyByte(*Glow, Out.AmbientGlow)) return false;
	if (const FWoTRuntimeWorldProperty* Stack =
		SceneFindProperty(Actor, TEXT("StackNum")))
		if (!ScenePropertyInteger(*Stack, Out.StackNum)) return false;
	return Out.CylinderHeight > 0.0f && Out.StackNum > 0 && Out.StackNum <= 64
		&& Out.MinFallRate >= 0.0f && Out.MaxFallRate >= Out.MinFallRate
		&& Out.TraceDepth >= 0.0f;
}

bool SceneReadSplashPlan(const FWoTRuntimeWorldActor& Actor,
	FSceneSplashPlan& Out)
{
	FWoTRuntimeParticleSprayerConfiguration Config;
	if (!WoTRuntimeReadParticleSprayerConfiguration(
		[&Actor](FStringView Name, uint32 Index)
		{
			return SceneFindProperty(Actor, Name, Index);
		}, Config)) return false;
	Out.Transform=Config.Transform;Out.Templates=MoveTemp(Config.Templates);
	Out.TextureIdentities=MoveTemp(Config.TextureIdentities);
	Out.Spread=Config.Spread;Out.Volume=Config.Volume;Out.MinVolume=Config.MinVolume;
	Out.Gravity=Config.Gravity;Out.PrimeCount=Config.PrimeCount;
	Out.VolumeScalePct=Config.VolumeScalePct;Out.VisibilityRadius=Config.VisibilityRadius;
	Out.VisibilityHeight=Config.VisibilityHeight;Out.bOn=Config.bOn;
	Out.bLOSClip=Config.bLOSClip;Out.bLinearDistribution=Config.bLinearDistribution;
	return true;
}

struct FSceneMeshTriangle
{
	uint32 Vertices[3] = {};
	int32 UVs[3] = {INDEX_NONE, INDEX_NONE, INDEX_NONE};
	uint32 Section = 0;
};

struct FSceneMeshTopology
{
	TArray<FVector3f> Positions;
	TArray<FVector2f> UVs;
	TArray<FSceneMeshTriangle> Triangles;
	TArray<int32> SectionSkins;
	TArray<FString> SkinKeys;
};

struct FSceneSurfaceBinding
{
	FString ConverterSlotBase;
	TStrongObjectPtr<UTexture2D> Texture;
	bool bProcedural = false;
	bool bMasked = false;
};

class FSceneMeshCursor
{
public:
	explicit FSceneMeshCursor(TConstArrayView<uint8> In) : Bytes(In) {}
	bool ReadU16(uint16& Out)
	{
		if (!Can(2)) return false;
		Out = static_cast<uint16>(Bytes[Offset])
			| static_cast<uint16>(Bytes[Offset + 1]) << 8;
		Offset += 2; return true;
	}
	bool ReadU32(uint32& Out)
	{
		if (!Can(4)) return false;
		Out = static_cast<uint32>(Bytes[Offset])
			| static_cast<uint32>(Bytes[Offset + 1]) << 8
			| static_cast<uint32>(Bytes[Offset + 2]) << 16
			| static_cast<uint32>(Bytes[Offset + 3]) << 24;
		Offset += 4; return true;
	}
	bool ReadI32(int32& Out)
	{
		uint32 Bits = 0; if (!ReadU32(Bits)) return false;
		FMemory::Memcpy(&Out, &Bits, sizeof(Out)); return true;
	}
	bool ReadFloat(float& Out)
	{
		uint32 Bits = 0; if (!ReadU32(Bits)) return false;
		FMemory::Memcpy(&Out, &Bits, sizeof(Out));
		return FMath::IsFinite(Out);
	}
	bool Skip(uint64 Count)
	{
		if (!Can(Count)) return false; Offset += Count; return true;
	}
	bool AtEnd() const { return Offset == static_cast<uint64>(Bytes.Num()); }
private:
	bool Can(uint64 Count) const
	{
		return Offset <= static_cast<uint64>(Bytes.Num())
			&& Count <= static_cast<uint64>(Bytes.Num()) - Offset;
	}
	TConstArrayView<uint8> Bytes;
	uint64 Offset = 0;
};

int32 SceneSignExtend(uint32 Value, uint32 Bits)
{
	const uint32 Sign = 1u << (Bits - 1u);
	const uint32 Mask = (1u << Bits) - 1u;
	Value &= Mask;
	return static_cast<int32>((Value ^ Sign) - Sign);
}

FVector3f SceneRotateMeshPoint(FVector3f Point, const int32 Rotation[3])
{
	auto Rotate = [](float A, float B, int32 Units, float& OutA, float& OutB)
	{
		const double Angle = (static_cast<uint32>(Units) & 0xffffu)
			* (2.0 * UE_DOUBLE_PI / 65536.0);
		double Sin = 0.0, Cos = 1.0;
		FMath::SinCos(&Sin, &Cos, Angle);
		OutA = static_cast<float>(A * Cos - B * Sin);
		OutB = static_cast<float>(A * Sin + B * Cos);
	};
	if (Rotation[2] != 0)
	{
		float Y = 0, Z = 0; Rotate(Point.Y, -Point.Z, Rotation[2], Y, Z);
		Point.Y = Y; Point.Z = -Z;
	}
	if (Rotation[0] != 0)
	{
		float X = 0, Z = 0; Rotate(Point.X, Point.Z, Rotation[0], X, Z);
		Point.X = X; Point.Z = Z;
	}
	if (Rotation[1] != 0)
	{
		float X = 0, Y = 0; Rotate(Point.X, Point.Y, Rotation[1], X, Y);
		Point.X = X; Point.Y = Y;
	}
	return Point;
}

bool ParseSceneMeshTopology(const FWoTRuntimeVerifiedObject& Object,
	FSceneMeshTopology& Out, FString& Error)
{
	const TConstArrayView<uint8> Bytes = Object.Payload();
	static const uint8 Magic[8] = {'W','O','T','M','S','H','3',0};
	if (Bytes.Num() < 104 || FMemory::Memcmp(Bytes.GetData(), Magic, 8) != 0)
	{
		Error = TEXT("mesh topology magic is absent"); return false;
	}
	FSceneMeshCursor Cursor(Bytes);
	if (!Cursor.Skip(8)) return false;
	uint16 Version = 0, Flags = 0;
	uint32 VertexCount = 0, TriangleCount = 0, UVCount = 0;
	uint32 SectionCount = 0, SkinCount = 0, RenderCount = 0;
	uint32 CollisionCount = 0, FrameCount = 0;
	if (!Cursor.ReadU16(Version) || !Cursor.ReadU16(Flags)
		|| !Cursor.ReadU32(VertexCount) || !Cursor.ReadU32(TriangleCount)
		|| !Cursor.ReadU32(UVCount) || !Cursor.ReadU32(SectionCount)
		|| !Cursor.ReadU32(SkinCount) || !Cursor.ReadU32(RenderCount)
		|| !Cursor.ReadU32(CollisionCount) || !Cursor.ReadU32(FrameCount)
		|| Version != 2 || Flags > 1 || VertexCount == 0
		|| VertexCount > 1000000 || TriangleCount == 0 || TriangleCount > 1520
		|| UVCount > 1000000 || SectionCount == 0 || SectionCount > 1520
		|| SkinCount > 1520 || RenderCount > 3000000
		|| CollisionCount > TriangleCount || FrameCount == 0)
	{
		Error = TEXT("mesh topology header is invalid"); return false;
	}
	FVector3f Origin, Scale;
	int32 Rotation[3] = {};
	for (float* Value : {&Origin.X, &Origin.Y, &Origin.Z,
		&Scale.X, &Scale.Y, &Scale.Z})
		if (!Cursor.ReadFloat(*Value)) { Error = TEXT("mesh transform is invalid"); return false; }
	for (int32& Value : Rotation)
		if (!Cursor.ReadI32(Value)) { Error = TEXT("mesh rotation is truncated"); return false; }
	for (int32 Index = 0; Index < 6; ++Index)
	{
		float Bound = 0; if (!Cursor.ReadFloat(Bound))
			{ Error = TEXT("mesh bounds are invalid"); return false; }
	}
	Out.Positions.Reserve(VertexCount);
	for (uint32 Index = 0; Index < VertexCount; ++Index)
	{
		uint32 Packed = 0; if (!Cursor.ReadU32(Packed)) return false;
		FVector3f Point(
			(SceneSignExtend(Packed, 11) - Origin.X) * Scale.X,
			(SceneSignExtend(Packed >> 11, 11) - Origin.Y) * Scale.Y,
			(SceneSignExtend(Packed >> 22, 10) - Origin.Z) * Scale.Z);
		Out.Positions.Add(SceneRotateMeshPoint(Point, Rotation));
	}
	Out.UVs.Reserve(UVCount);
	for (uint32 Index = 0; Index < UVCount; ++Index)
	{
		uint16 PackedUV = 0; if (!Cursor.ReadU16(PackedUV)) return false;
		Out.UVs.Add(FVector2f(
			static_cast<float>(PackedUV & 0xffu) / 255.0f,
			static_cast<float>(PackedUV >> 8) / 255.0f));
	}
	Out.Triangles.Reserve(TriangleCount);
	for (uint32 Index = 0; Index < TriangleCount; ++Index)
	{
		FSceneMeshTriangle Triangle;
		uint32 RenderIndices[3] = {}, Available = 0, Ignored = 0;
		for (uint32& Value : Triangle.Vertices) if (!Cursor.ReadU32(Value)) return false;
		for (int32& Value : Triangle.UVs) if (!Cursor.ReadI32(Value)) return false;
		for (uint32& Value : RenderIndices) if (!Cursor.ReadU32(Value)) return false;
		if (!Cursor.ReadU32(Triangle.Section) || !Cursor.ReadU32(Available)
			|| !Cursor.ReadU32(Ignored) || !Cursor.ReadU32(Ignored)
			|| !Cursor.ReadU32(Ignored) || Triangle.Section >= SectionCount)
			return false;
		for (int32 Corner = 0; Corner < 3; ++Corner)
			if (Triangle.Vertices[Corner] >= VertexCount
				|| (Triangle.UVs[Corner] != INDEX_NONE
					&& static_cast<uint32>(Triangle.UVs[Corner]) >= UVCount)) return false;
		Out.Triangles.Add(Triangle);
	}
	Out.SectionSkins.Reserve(SectionCount);
	for (uint32 Index = 0; Index < SectionCount; ++Index)
	{
		uint32 First = 0, Count = 0, Available = 0, PolyFlags = 0;
		int32 Skin = INDEX_NONE;
		if (!Cursor.ReadU32(First) || !Cursor.ReadU32(Count)
			|| !Cursor.ReadU32(Available) || !Cursor.ReadU32(PolyFlags)
			|| !Cursor.ReadI32(Skin) || First > TriangleCount
			|| Count > TriangleCount - First
			|| (Skin != INDEX_NONE && (Skin < 0 || static_cast<uint32>(Skin) >= SkinCount)))
			return false;
		if ((Available & 2u) == 0) Skin = INDEX_NONE;
		Out.SectionSkins.Add(Skin);
	}
	if (!Cursor.Skip(static_cast<uint64>(RenderCount) * 12)
		|| !Cursor.Skip(static_cast<uint64>(CollisionCount) * 4)
		|| !Cursor.AtEnd())
	{
		Error = TEXT("mesh topology stream has a size mismatch"); return false;
	}
	const FWoTCanonicalJsonValue* Skins = Object.Header().FindMember(TEXT("skins"));
	if (Skins == nullptr || !Skins->IsArray()
		|| Skins->Num() != static_cast<int32>(SkinCount))
	{
		Error = TEXT("mesh semantic skin table is missing"); return false;
	}
	for (int32 Index = 0; Index < Skins->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Skin = Skins->GetElement(Index);
		static const TCHAR* Fields[] = {TEXT("logical_id"), TEXT("name"), TEXT("package")};
		if (RequireExactMembers(Skin, MakeArrayView(Fields), TEXT("mesh skin")).IsSet())
			return false;
		const FWoTCanonicalJsonValue* Logical = Skin.FindMember(TEXT("logical_id"));
		const FWoTCanonicalJsonValue* Name = Skin.FindMember(TEXT("name"));
		const FWoTCanonicalJsonValue* Package = Skin.FindMember(TEXT("package"));
		FString Key;
		if (Logical != nullptr && Logical->GetKind() == EWoTCanonicalJsonKind::String)
			Key = Logical->GetString();
		else if (Name != nullptr && Package != nullptr
			&& Name->GetKind() == EWoTCanonicalJsonKind::String
			&& Package->GetKind() == EWoTCanonicalJsonKind::String)
			Key = Package->GetString() + TEXT("__") + Name->GetString();
		Out.SkinKeys.Add(MoveTemp(Key));
	}
	return true;
}

UStaticMesh* BuildSceneMesh(const FSceneMeshTopology& Topology, FString& Error)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	if (Mesh == nullptr) return nullptr;
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	auto Positions = Attributes.GetVertexPositions();
	auto Normals = Attributes.GetVertexInstanceNormals();
	auto UVs = Attributes.GetVertexInstanceUVs();
	auto Slots = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(1);
	TArray<FPolygonGroupID> Groups;
	for (int32 Section = 0; Section < Topology.SectionSkins.Num(); ++Section)
	{
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		const int32 Skin = Topology.SectionSkins[Section];
		const FName SlotName(*((Topology.SkinKeys.IsValidIndex(Skin)
			&& !Topology.SkinKeys[Skin].IsEmpty()) ? Topology.SkinKeys[Skin]
			: FString::Printf(TEXT("Skin_%d"), Section)));
		Slots[Group] = SlotName;
		Mesh->GetStaticMaterials().Emplace(nullptr, SlotName);
		Groups.Add(Group);
	}
	for (const FSceneMeshTriangle& Triangle : Topology.Triangles)
	{
		if (!Groups.IsValidIndex(static_cast<int32>(Triangle.Section))) return nullptr;
		const FVector3f A = Topology.Positions[Triangle.Vertices[0]];
		const FVector3f B = Topology.Positions[Triangle.Vertices[1]];
		const FVector3f C = Topology.Positions[Triangle.Vertices[2]];
		FVector3f Normal = FVector3f::CrossProduct(B - A, C - A).GetSafeNormal();
		if (Normal.IsNearlyZero()) Normal = FVector3f::ZAxisVector;
		FVertexInstanceID Instances[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVertexID Vertex = Description.CreateVertex();
			Positions[Vertex] = Topology.Positions[Triangle.Vertices[Corner]];
			Instances[Corner] = Description.CreateVertexInstance(Vertex);
			Normals[Instances[Corner]] = Normal;
			UVs.Set(Instances[Corner], 0,
				Topology.UVs.IsValidIndex(Triangle.UVs[Corner])
					? Topology.UVs[Triangle.UVs[Corner]] : FVector2f::ZeroVector);
		}
		Description.CreateTriangle(
			Groups[static_cast<int32>(Triangle.Section)], MakeArrayView(Instances, 3));
	}
	FStaticMeshOperations::ComputeMikktTangents(Description, true);
	Mesh->bAllowCPUAccess = true;
	Mesh->NeverStream = true;
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true; Params.bCommitMeshDescription = false;
	Params.bMarkPackageDirty = false; Params.bBuildSimpleCollision = false;
	Params.bAllowCpuAccess = true;
	const TArray<const FMeshDescription*> Descriptions = {&Description};
	if (!Mesh->BuildFromMeshDescriptions(Descriptions, Params))
	{
		Error = TEXT("front-end mesh topology build failed"); return nullptr;
	}
	Mesh->SetBodySetup(nullptr);
	return Mesh;
}

UTexture2D* MaterializeSceneTexture(const FWoTTexturePayloadData& Payload,
	bool bRawSceneTexture)
{
	// The front-end payload contains fonts and particle masks alongside its BSP
	// textures. Preserve their established single-mip point-sampled contract;
	// the retail world-surface mip/filter policy belongs only to map BSP/movers.
	FWoTRuntimeTextureOptions Options;
	Options.bSRGB = !bRawSceneTexture;
	return WoTRuntimeMaterializeTexture(GetTransientPackage(), Payload, Options);
}

UTexture2D* MaterializeSceneRainTexture(const FWoTTexturePayloadData& Payload)
{
	if (Payload.Mips.Num() != 10 || Payload.Palette.Num() != 256
		|| !Payload.bMasked || Payload.Palette[0].R != 0
		|| Payload.Palette[0].G != 0 || Payload.Palette[0].B != 0) return nullptr;
	const TConstArrayView<uint8> Bytes = Payload.PayloadBytes();
	for (int32 Level = 0; Level < 10; ++Level)
	{
		const FWoTTexturePayloadMip& Mip = Payload.Mips[Level];
		const uint32 Side = 512u >> Level;
		if (Mip.Width != Side || Mip.Height != Side
			|| Mip.StoredByteLength != Side * Side
			|| Mip.DataOffset > static_cast<uint64>(Bytes.Num())
			|| Mip.StoredByteLength > static_cast<uint64>(Bytes.Num()) - Mip.DataOffset) return nullptr;
	}
	UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!Texture) return nullptr;
	FTexturePlatformData* Data = new FTexturePlatformData();
	Data->SizeX = 256; Data->SizeY = 256; Data->PixelFormat = PF_B8G8R8A8;
	Data->SetNumSlices(1);
	Texture->SetPlatformData(Data);
	Texture->bNotOfflineProcessed = true;
	for (int32 Level = 1; Level < 10; ++Level)
	{
		const FWoTTexturePayloadMip& Source = Payload.Mips[Level];
		FTexture2DMipMap* Mip = new FTexture2DMipMap();
		Data->Mips.Add(Mip);
		Mip->SizeX = Source.Width; Mip->SizeY = Source.Height; Mip->SizeZ = 1;
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		FColor* Destination = reinterpret_cast<FColor*>(Mip->BulkData.Realloc(Source.StoredByteLength * 4));
		if (!Destination) { Mip->BulkData.Unlock(); return nullptr; }
		for (uint32 Pixel = 0; Pixel < Source.StoredByteLength; ++Pixel)
		{
			const uint8 Index = Bytes[static_cast<int32>(Source.DataOffset + Pixel)];
			Destination[Pixel] = Index == 0 ? FColor(0,0,0,0) : Payload.Palette[Index];
		}
		Mip->BulkData.Unlock();
	}
	Texture->SRGB = false; Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Wrap; Texture->AddressY = TA_Wrap;
	Texture->NeverStream = true;
	Texture->UpdateResource();
	return Texture;
}

UTexture2D* MaterializeSceneLightmap(const FWoTBSPPayloadData& Payload)
{
	return WoTRuntimeBSPMaterializeLightmap(Payload);
}

UTexture2D* MaterializeSceneProceduralIndices(
	const FWoTRuntimeProceduralMip& Mip)
{
	return WoTRuntimeBSPMaterializeProceduralIndices(Mip);
}

#if WITH_AUTOMATION_TESTS
bool GFailNextFrontEndSceneCommit = false;
bool GFailAfterFrontEndSceneConstruction = false;
bool GFailAfterFrontEndSceneControllerPreparation = false;
bool GCancelNextFrontEndSceneCommit = false;
bool GRevokeAfterFrontEndSceneConstruction = false;
bool GRevokeAfterFrontEndSceneControllerPreparation = false;
bool GRevokeDuringFrontEndScenePublication = false;
TFunction<void(const TCHAR*, const FWoTFrontEndSceneObservation&)>
	GFrontEndScenePublicationObserver;
TFunction<void(EWoTFactDomain, int32)> GFrontEndSceneResourceStageObserver;
#endif
thread_local int32 GFrontEndScenePublicationGuardDepth = 0;
}

struct FWoTFrontEndSceneOwnerState
{
	FCriticalSection Guard;
	FWoTFrontEndSceneMaterializer* Owner = nullptr;
	TAtomic<bool> bOwnerCallInProgress{false};
	TAtomic<uint64> PendingRevocationSerial{0};
};

struct FWoTFrontEndSceneMaterializer::FPlan
{
	~FPlan()
	{
		if (PinnedWorldLease.IsValid() && RevocationSubscription != 0)
			PinnedWorldLease->UnsubscribeRevocation(RevocationSubscription);
	}
	TWeakObjectPtr<UWorld> World;
	uint64 AuthoritySerial = 0;
	FWoTFrontEndDescriptor Descriptor;
	FWoTFrontEndScenePreferences Preferences;
	FWoTFrontEndSceneStagedCensus Census;
	FString SnapshotIdentity;
	TUniquePtr<FWoTCacheGenerationLease> PinnedWorldLease;
	TSharedPtr<const FWoTBSPPayloadData> BSP;
	FWoTFrontEndSkyPlan SkyPlan;
	TStrongObjectPtr<UStaticMesh> BSPRenderMesh;
	TStrongObjectPtr<UStaticMesh> SkyRenderMesh;
	TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> SkyMaterials;
	TWeakObjectPtr<AWoTSkyBox> SkyActor;
	TStrongObjectPtr<UTexture2D> LightmapAtlas;
	TStrongObjectPtr<UTexture2D> RainTexture;
	FWoTEntryStrikeSettings StrikeSettings;
	TStrongObjectPtr<UStaticMesh> RainRenderMesh;
	TArray<FString> RainSectionSkinKeys;
	TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> RainMaterials;
	TMap<FString, TStrongObjectPtr<UTexture2D>> TexturesBySemanticKey;
	TMap<FString, TStrongObjectPtr<UWoTPayloadSoundWave>> SoundsBySource;
	TSet<FString> MaskedTextureSemanticKeys;
	TSet<FString> ProceduralSemanticKeys;
	TMap<FString, TStrongObjectPtr<UTexture2D>> ProceduralTextures;
	TArray<FSceneSurfaceBinding> SurfaceBindings;
	TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> SurfaceMaterials;
	TArray<TStrongObjectPtr<UMaterialInstanceDynamic>> WaterMaterials;
	TArray<FSceneLightPlan> Lights;
	TArray<FSceneAmbientPlan> Ambience;
	TArray<FSceneSplashPlan> Splashes;
	TArray<FSceneRainPlan> Rains;
	TArray<TStrongObjectPtr<UWoTPayloadSoundWave>> ThunderWaves;
	TArray<FString> ThunderSoundIdentities;
	FString RandomLightningEvent;
	struct FThunderEndpoint
	{
		FTransform Transform = FTransform::Identity;
		FString Tag;
		FString LightTag;
		float Radius = 5000.0f;
		float Volume = 1.0f;
		float Length = .5f;
	};
	TArray<FThunderEndpoint> ThunderEndpoints;
	float MinLightningTime = 10.0f;
	float MaxLightningTime = 20.0f;
	float MinThunderDelay = 0.0f;
	float MaxThunderDelay = 3.0f;
	TSharedPtr<FWoTFrontEndSceneController> Controller;
	TFunction<bool()> Cancellation;
	TFunction<void(const FWoTFrontEndSceneProgress&)> Progress;
	TArray<TWeakObjectPtr<UActorComponent>> PublishedComponents;
	TWeakObjectPtr<APlayerController> ViewOwner;
	TWeakObjectPtr<AActor> PreviousViewTarget;
	uint64 RevocationSubscription = 0;
	bool bCommitted = false;
};

FWoTFrontEndSceneStaging::FWoTFrontEndSceneStaging(
	TWeakPtr<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> InOwner,
	uint64 InSerial)
	: Owner(InOwner), Serial(InSerial)
{
}

FWoTFrontEndSceneStaging::FWoTFrontEndSceneStaging(
	FWoTFrontEndSceneStaging&& Other)
	: Owner(Other.Owner), Serial(Other.Serial)
{
	Other.Owner.Reset();
	Other.Serial = 0;
}

FWoTFrontEndSceneStaging& FWoTFrontEndSceneStaging::operator=(
	FWoTFrontEndSceneStaging&& Other)
{
	if (this != &Other)
	{
		Rollback();
		Owner = Other.Owner;
		Serial = Other.Serial;
		Other.Owner.Reset();
		Other.Serial = 0;
	}
	return *this;
}

FWoTFrontEndSceneStaging::~FWoTFrontEndSceneStaging()
{
	Rollback();
}

FWoTBootstrapStatus FWoTFrontEndSceneStaging::Commit(
	APlayerController* ViewOwner) &&
{
	const TSharedPtr<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> State =
		Owner.Pin();
	if (!State.IsValid() || Serial == 0 || !IsInGameThread())
		return SceneStatus(TEXT("front-end scene staging was already consumed"));
	const uint64 TargetSerial = Serial;
	Owner.Reset();
	Serial = 0;
	FScopeLock Lock(&State->Guard);
	if (State->Owner == nullptr)
		return SceneStatus(TEXT("front-end scene staging owner no longer exists"));
	State->bOwnerCallInProgress.Store(true);
	FWoTBootstrapStatus Status =
		State->Owner->CommitStaged(TargetSerial, ViewOwner);
	State->bOwnerCallInProgress.Store(false);
	const uint64 PendingRevocation =
		State->PendingRevocationSerial.Exchange(0);
	if (PendingRevocation != 0 && State->Owner != nullptr)
	{
		State->Owner->HandleLeaseRevoked(PendingRevocation);
		if (!Status.IsError())
			Status = SceneStatus(
				TEXT("front-end scene authority was revoked during publication"));
	}
	return Status;
}

void FWoTFrontEndSceneStaging::Rollback()
{
	const TSharedPtr<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> State =
		Owner.Pin();
	if (State.IsValid() && Serial != 0)
	{
		const uint64 TargetSerial = Serial;
		auto DoRollback = [State, TargetSerial]()
		{
			FScopeLock Lock(&State->Guard);
			if (State->Owner != nullptr)
				State->Owner->RollbackStaged(TargetSerial);
		};
		if (IsInGameThread()) DoRollback();
		else AsyncTask(ENamedThreads::GameThread, MoveTemp(DoRollback));
	}
	Owner.Reset();
	Serial = 0;
}

#if WITH_AUTOMATION_TESTS
FWoTFrontEndSceneStagedCensus
FWoTFrontEndSceneStaging::StagedCensusForTesting() const
{
	const TSharedPtr<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> State =
		Owner.Pin();
	if (!State.IsValid()) return {};
	FScopeLock Lock(&State->Guard);
	return State->Owner != nullptr ? State->Owner->StagedCensus(Serial) :
		FWoTFrontEndSceneStagedCensus{};
}
#endif

FWoTBootstrapStatus WoTFrontEndSceneLoadWorld(
	const FWoTFrontEndDescriptor& Descriptor,
	FWoTRuntimeWorldResource& WorldResource)
{
	const FWoTNeutralResourceId WorldId{
		Descriptor.WorldLogicalId, EWoTNeutralResourceKind::World};
	FWoTBootstrapStatus WorldLoaded = WorldResource.LoadFirstUse(WorldId);
	if (WorldLoaded.IsError()) return WorldLoaded;
	if (WorldResource.MapIdentity() != Descriptor.MapName)
		return SceneStatus(
			TEXT("front-end world object has the wrong authenticated identity"));
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus WoTFrontEndSceneDecodeBSP(
	const FWoTNeutralResourceId& Id, FWoTBSPPayloadData& OutBSP)
{
	auto BSPObject = FWoTRuntimeRegistry::Get().ReadObject(
		Id, 1024ull * 1024ull * 1024ull,
		TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
	if (BSPObject.IsError())
		return FWoTBootstrapStatus::Error(MoveTemp(BSPObject).GetError());
	FString BSPError;
	if (!UWoTBSPPayloadLibrary::DecodePayload(
			BSPObject.GetValue().Payload(), OutBSP, BSPError))
		return SceneStatus(TEXT("front-end BSP is malformed"));
	return FWoTBootstrapStatus::Success();
}

FWoTFrontEndSceneMaterializer::FWoTFrontEndSceneMaterializer()
	: OwnerState(MakeShared<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe>())
{
	OwnerState->Owner = this;
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this, &FWoTFrontEndSceneMaterializer::HandleWorldCleanup);
}

FWoTFrontEndSceneMaterializer::~FWoTFrontEndSceneMaterializer()
{
	Shutdown();
	if (WorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		WorldCleanupHandle.Reset();
	}
	FScopeLock Lock(&OwnerState->Guard);
	OwnerState->Owner = nullptr;
}

TWoTBootstrapResult<FWoTFrontEndSceneStaging>
FWoTFrontEndSceneMaterializer::Stage(
	UWorld& World, const FWoTFrontEndDescriptor& Descriptor,
	const FWoTCacheGenerationLease& WorldLease, UWoTPayloadSubsystem& Payload,
	const FWoTFrontEndFontOwner& FontOwner,
	const FWoTFrontEndScenePreferences& Preferences,
	TFunction<bool()> IsCancelled,
	TFunction<void(const FWoTFrontEndSceneProgress&)> Progress)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndSceneStaging>;
	if (!IsInGameThread())
	{
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end scene staging is restricted to the game thread"));
	}
	Rollback();
	if (!WorldLease.IsValid()
		|| !FontOwner.IsValid() || !FontOwner.SharesSnapshotWith(WorldLease)
		|| Payload.IsDormant() || IsCancelled())
	{
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end scene staging requires one live authenticated generation"));
	}
	FWoTBootstrapStatus DescriptorStatus = ValidateFrontEndDescriptor(Descriptor);
	if (DescriptorStatus.IsError())
		return FResult::Error(MoveTemp(DescriptorStatus).GetError());
	auto RetainedDescriptor =
		WoTDeriveFrontEndDescriptorFromActiveGeneration(WorldLease);
	if (RetainedDescriptor.IsError())
		return FResult::Error(MoveTemp(RetainedDescriptor).GetError());
	if (RetainedDescriptor.GetValue().DescriptorRoot != Descriptor.DescriptorRoot)
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end descriptor is not the retained snapshot authority"));

	TUniquePtr<FPlan> Candidate = MakeUnique<FPlan>();
	Candidate->AuthoritySerial = NextSerial++;
	for (const FString& SourceExport : Descriptor.ThunderSoundSourceExports)
		Candidate->ThunderSoundIdentities.Add(SourceExport.ToLower());
	Candidate->World = &World;
	ObservationWorld = &World;
	Candidate->Descriptor = Descriptor;
	Candidate->Preferences = Preferences;
	Candidate->Cancellation = IsCancelled;
	Candidate->Progress = MoveTemp(Progress);
	Candidate->SnapshotIdentity = WorldLease.GetManifestSha256().ToHex();
	const uint64 AuthoritySerial = Candidate->AuthoritySerial;
	const TWeakPtr<FWoTFrontEndSceneOwnerState, ESPMode::ThreadSafe> WeakOwner =
		OwnerState;
	{
		FScopeLock RetentionAdmission(&WorldLease.GetVerifiedReadGuard());
		if (!WorldLease.MayBeginVerifiedReadUnderVerifiedReadGuard())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end scene retention was denied during revocation"));
		Candidate->PinnedWorldLease.Reset(new FWoTCacheGenerationLease(WorldLease));
		Candidate->RevocationSubscription =
			Candidate->PinnedWorldLease->SubscribeRevocation(
			[WeakOwner, AuthoritySerial]()
			{
			auto Teardown = [WeakOwner, AuthoritySerial]()
			{
				const auto State = WeakOwner.Pin();
				if (!State.IsValid()) return;
				FScopeLock Lock(&State->Guard);
				if (State->Owner != nullptr)
					State->Owner->HandleLeaseRevoked(AuthoritySerial);
			};
			const auto State = WeakOwner.Pin();
			if (IsInGameThread())
			{
				if (State.IsValid() && State->bOwnerCallInProgress.Load())
				{
					State->PendingRevocationSerial.Store(AuthoritySerial);
					return false;
				}
				else Teardown();
				// Re-entering the owner-state lock would deadlock. Commit drains this
				// serial synchronously before the staging capability returns.
				return true;
			}
			FEvent* Completed = FPlatformProcess::GetSynchEventFromPool(true);
			AsyncTask(ENamedThreads::GameThread,
				[Teardown = MoveTemp(Teardown), Completed]() mutable
				{
					Teardown();
					Completed->Trigger();
				});
			Completed->Wait();
			FPlatformProcess::ReturnSynchEventToPool(Completed);
			return true;
			});
	}
	if (Candidate->RevocationSubscription == 0)
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end scene could not subscribe to retained authority"));

	FWoTRuntimeWorldResource WorldResource;
	FWoTBootstrapStatus WorldLoaded =
		WoTFrontEndSceneLoadWorld(Descriptor, WorldResource);
	if (WorldLoaded.IsError()) return FResult::Error(MoveTemp(WorldLoaded).GetError());

	uint32 Lights = 0, Ambience = 0, Splash = 0, Rain = 0, Lightning = 0;
	uint32 RandomLightning = 0, Water = 0;
	uint32 AuthoredWaterResources = 0;
	float LevelBrightness = 1.0f;
	TSet<FString> SurfaceTextures;
	for (const FWoTRuntimeWorldSurface& Surface : WorldResource.Surfaces())
	{
		if (!Surface.TextureIdentity.IsEmpty())
			SurfaceTextures.Add(Surface.TextureIdentity.ToLower());
	}
	TSet<FString> LightningTags;
	for (const FWoTRuntimeWorldActor& Actor : WorldResource.Actors())
	{
		if (FString(ClassLeaf(Actor.ClassIdentity)) == TEXT("Lightning"))
		{
			const FString Tag = SceneActorTag(Actor, TEXT("LightTag"));
			if (!Tag.IsEmpty()) LightningTags.Add(Tag);
		}
	}
	for (const FWoTRuntimeWorldActor& Actor : WorldResource.Actors())
	{
		const FString Leaf(ClassLeaf(Actor.ClassIdentity));
		Lights += Leaf == TEXT("Light") ? 1u : 0u;
		Ambience += Leaf == TEXT("AmbientSound") ? 1u : 0u;
		Splash += Leaf == TEXT("ParticleSprayer") ? 1u : 0u;
		Rain += Leaf == TEXT("RainCyd") ? 1u : 0u;
		Lightning += Leaf == TEXT("Lightning") ? 1u : 0u;
		RandomLightning += Leaf == TEXT("RandomLightning") ? 1u : 0u;
		Water += Leaf == TEXT("WaterZone") ? 1u : 0u;
		if (Leaf == TEXT("Light"))
		{
			FSceneLightPlan& Light = Candidate->Lights.AddDefaulted_GetRef();
			Light.Transform = SceneActorTransform(Actor);
			Light.Identity = Actor.Identity;
			Light.Tag = SceneActorTag(Actor, TEXT("Tag"));
			uint8 Brightness = 0, Hue = 0, Saturation = 255, Radius = 0;
			Brightness = 64; Radius = 64;
			auto ReadOptionalByte = [&](const TCHAR* Name, uint8& Value)
			{
				if (const FWoTRuntimeWorldProperty* Property =
					SceneFindProperty(Actor, Name))
					return ScenePropertyByte(*Property, Value);
				return true;
			};
			if (!ReadOptionalByte(TEXT("LightBrightness"), Brightness)
				|| !ReadOptionalByte(TEXT("LightHue"), Hue)
				|| !ReadOptionalByte(TEXT("LightSaturation"), Saturation)
				|| !ReadOptionalByte(TEXT("LightRadius"), Radius))
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end light properties are malformed"));
			Light.Intensity = Brightness / 64.0f * 5.0f;
			Light.Brightness = Brightness; Light.Hue = Hue; Light.Saturation = Saturation;
			Light.Radius = (static_cast<float>(Radius) + 1.0f) * 25.0f;
			Light.Color = FLinearColor::MakeFromHSV8(
				Hue, static_cast<uint8>(255 - Saturation), 255);
			Light.bLightningGated = LightningTags.Contains(Light.Tag);
		}
		else if (Leaf == TEXT("LevelInfo"))
		{
			if (const FWoTRuntimeWorldProperty* P = SceneFindProperty(Actor, TEXT("Brightness")))
				if (!ScenePropertyFloat(*P, LevelBrightness) || LevelBrightness < 0 || LevelBrightness > 1024)
					return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end level brightness is malformed"));
		}
		else if (Leaf == TEXT("AmbientSound"))
		{
			FSceneAmbientPlan& Ambient = Candidate->Ambience.AddDefaulted_GetRef();
			Ambient.Transform = SceneActorTransform(Actor);
			const FWoTRuntimeWorldProperty* Sound =
				SceneFindProperty(Actor, TEXT("AmbientSound"));
			uint8 VolumeByte = 190, RadiusByte = 64;
			if (Sound == nullptr || Sound->Type != EWoTRuntimeWorldPropertyType::Object
				|| Sound->ObjectIdentity.IsEmpty())
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end ambient sound reference is malformed"));
			if (const FWoTRuntimeWorldProperty* VolumeProperty =
				SceneFindProperty(Actor, TEXT("SoundVolume")))
				if (!ScenePropertyByte(*VolumeProperty, VolumeByte))
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end ambient volume is malformed"));
			if (const FWoTRuntimeWorldProperty* RadiusProperty =
				SceneFindProperty(Actor, TEXT("SoundRadius")))
				if (!ScenePropertyByte(*RadiusProperty, RadiusByte))
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end ambient radius is malformed"));
			Ambient.SoundIdentity = Sound->ObjectIdentity;
			Ambient.Volume = 2.0f * VolumeByte / 255.0f;
			Ambient.Radius = 25.0f * (static_cast<float>(RadiusByte) + 1.0f);
		}
		else if (Leaf == TEXT("ParticleSprayer"))
		{
			FSceneSplashPlan SplashPlan;
			if (!SceneReadSplashPlan(Actor, SplashPlan))
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end splash authored properties are malformed"));
			Candidate->Splashes.Add(MoveTemp(SplashPlan));
		}
		else if (Leaf == TEXT("RainCyd"))
		{
			FSceneRainPlan RainPlan;
			if (!SceneReadRainPlan(Actor, RainPlan))
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end RainCyd authored properties are malformed"));
			FRandomStream RainRandom(static_cast<int32>(SceneRainSeed(
				Descriptor.DescriptorRoot, Candidate->Rains.Num())));
			RainPlan.InitialYawDegrees = RainRandom.FRandRange(0.0f, 360.0f);
			RainPlan.InitialFallRate = RainRandom.FRandRange(
				RainPlan.MinFallRate, RainPlan.MaxFallRate);
			Candidate->Rains.Add(MoveTemp(RainPlan));
		}
		else if (Leaf == TEXT("Lightning"))
		{
			FPlan::FThunderEndpoint& Endpoint =
				Candidate->ThunderEndpoints.AddDefaulted_GetRef();
			Endpoint.Transform = SceneActorTransform(Actor);
			Endpoint.Tag = SceneActorTag(Actor, TEXT("Tag"));
			Endpoint.LightTag = SceneActorTag(Actor, TEXT("LightTag"));
			if (const FWoTRuntimeWorldProperty* P = SceneFindProperty(Actor, TEXT("LightningLength")))
				if (!ScenePropertyFloat(*P, Endpoint.Length) || Endpoint.Length < 0)
					return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end lightning length is malformed"));
			if (const FWoTRuntimeWorldProperty* Radius =
				SceneFindProperty(Actor, TEXT("ThunderRadius")))
				if (!ScenePropertyFloat(*Radius, Endpoint.Radius))
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end thunder radius is malformed"));
			uint8 Volume = 255;
			if (const FWoTRuntimeWorldProperty* P =
				SceneFindProperty(Actor, TEXT("SoundVolume")))
				if (!ScenePropertyByte(*P, Volume))
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end thunder volume is malformed"));
			Endpoint.Volume = 2.0f * Volume / 255.0f;
			if (Endpoint.Tag.IsEmpty() || Endpoint.LightTag.IsEmpty()
				|| Endpoint.Radius <= 0.0f)
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end lightning endpoint is incomplete"));
		}
		else if (Leaf == TEXT("RandomLightning"))
		{
			Candidate->RandomLightningEvent = SceneActorTag(Actor, TEXT("Event"));
			auto Timing = [&](const TCHAR* Name, float& Value)
			{
				if (const FWoTRuntimeWorldProperty* P = SceneFindProperty(Actor, Name))
					return ScenePropertyFloat(*P, Value);
				return true;
			};
			if (!Timing(TEXT("MinLightningTime"), Candidate->MinLightningTime)
				|| !Timing(TEXT("MaxLightningTime"), Candidate->MaxLightningTime)
				|| !Timing(TEXT("MinThunderDelay"), Candidate->MinThunderDelay)
				|| !Timing(TEXT("MaxThunderDelay"), Candidate->MaxThunderDelay)
				|| Candidate->MinLightningTime < 0.0f
				|| Candidate->MaxLightningTime < Candidate->MinLightningTime
				|| Candidate->MinThunderDelay < 0.0f
				|| Candidate->MaxThunderDelay < Candidate->MinThunderDelay)
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end lightning timing is malformed"));
			TArray<const FWoTRuntimeWorldProperty*> AuthoredThunder;
			for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
				if (Property.Name.Equals(TEXT("ThunderSounds"),
					ESearchCase::IgnoreCase)) AuthoredThunder.Add(&Property);
			AuthoredThunder.Sort([](const auto& A, const auto& B)
				{ return A.ArrayIndex < B.ArrayIndex; });
			if (AuthoredThunder.Num() != 4)
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end thunder override count is malformed"));
			for (int32 Index = 0; Index < AuthoredThunder.Num(); ++Index)
			{
				const FWoTRuntimeWorldProperty* P = AuthoredThunder[Index];
				FString ShortIdentity;
				const uint32 EffectiveIndex = static_cast<uint32>(Index + 6);
				const FString ExpectedShortIdentity =
					Descriptor.ThunderSoundSourceExports.IsValidIndex(EffectiveIndex)
					? FString::Printf(TEXT("Environment.%s"),
						*FString(ClassLeaf(
							Descriptor.ThunderSoundSourceExports[EffectiveIndex])))
					: FString();
				if (P == nullptr || P->ArrayIndex != EffectiveIndex
					|| !ScenePropertyUE1String(*P, ShortIdentity)
					|| ExpectedShortIdentity.IsEmpty()
					|| !ShortIdentity.Equals(
						ExpectedShortIdentity, ESearchCase::CaseSensitive))
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end thunder override disagrees with descriptor authority"));
			}
		}
		else if (Leaf == TEXT("WaterZone"))
		{
			// WaterZone is semantic state; visible water is its bound BSP section.
		}
	}
	if (Lights != Descriptor.Census.Lights
		|| Ambience != Descriptor.Census.AmbientSounds
		|| Splash != Descriptor.Census.SplashEmitters
		|| Rain != Descriptor.Census.RainCylinders
		|| Lightning != Descriptor.Census.LightningActors
		|| RandomLightning != Descriptor.Census.RandomLightningControllers)
	{
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end world semantic census mismatched its descriptor"));
	}
	if (Lightning > 0 && (Candidate->RandomLightningEvent.IsEmpty()
		|| Candidate->ThunderEndpoints.ContainsByPredicate(
			[&](const FPlan::FThunderEndpoint& Endpoint)
			{
				return !Endpoint.Tag.Equals(Candidate->RandomLightningEvent,
					ESearchCase::IgnoreCase);
			})))
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end lightning event routing is malformed"));
	if (Lightning > 0)
	{
		TSet<FString> RoutedLightTags;
		for (const FPlan::FThunderEndpoint& Endpoint : Candidate->ThunderEndpoints)
		{
			if (RoutedLightTags.Contains(Endpoint.LightTag))
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end lightning endpoints do not have unique light routes"));
			RoutedLightTags.Add(Endpoint.LightTag);
			int32 Matches = 0;
			for (const FSceneLightPlan& Light : Candidate->Lights)
				Matches += Light.Tag == Endpoint.LightTag ? 1 : 0;
			if (Matches != 1)
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end lightning endpoint does not resolve exactly one light"));
		}
		if (Candidate->ThunderSoundIdentities.IsEmpty())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end lightning has no ordered thunder route"));
	}

	TSet<FString> ParticleTextures;
	for (const FSceneSplashPlan& SplashPlan : Candidate->Splashes)
		for (const FString& Identity : SplashPlan.TextureIdentities)
			ParticleTextures.Add(Identity.ToLower());
	TSet<FString> AuthenticatedTextures;
	auto ResourceStage = [](EWoTFactDomain Domain)
	{
		switch (Domain)
		{
		case EWoTFactDomain::BSP:
		case EWoTFactDomain::World: return 0;
		case EWoTFactDomain::Texture:
		case EWoTFactDomain::Procedural: return 1;
		case EWoTFactDomain::Sound:
		case EWoTFactDomain::Mesh:
		case EWoTFactDomain::Animation: return 2;
		default: return 3;
		}
	};
	auto AddSurfaceBinding = [&Candidate](
		const FWoTFrontEndResourceRequirement& Requirement,
		UTexture2D* Texture, bool bProcedural, bool bMasked)
	{
		const FString Base = SceneConverterSlotBase(Requirement.SourceExport);
		if (Base.IsEmpty())
			return SceneStatus(
				TEXT("front-end surface has a malformed converter identity"));
		if (Candidate->SurfaceBindings.ContainsByPredicate(
			[&Base](const FSceneSurfaceBinding& Existing)
			{
				return Existing.ConverterSlotBase.Equals(
					Base, ESearchCase::IgnoreCase);
			}))
		{
			return SceneStatus(
				TEXT("front-end surfaces have an ambiguous converter identity"));
		}
		FSceneSurfaceBinding& Binding =
			Candidate->SurfaceBindings.AddDefaulted_GetRef();
		Binding.ConverterSlotBase = Base;
		Binding.Texture.Reset(Texture);
		Binding.bProcedural = bProcedural;
		Binding.bMasked = bMasked;
		return FWoTBootstrapStatus::Success();
	};
	auto LoadResourceStage = [&](int32 DesiredStage) -> FResult
	{
	for (const FWoTFrontEndResourceRequirement& Requirement
		: Descriptor.RequiredResources)
	{
		if (ResourceStage(Requirement.Domain) != DesiredStage) continue;
		if (IsCancelled())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end scene staging was cancelled"));
		const TOptional<EWoTNeutralResourceKind> Kind = ResourceKind(Requirement.Domain);
		if (!Kind.IsSet())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end descriptor contains an unsupported resource domain"));
		const FWoTNeutralResourceId Id{Requirement.LogicalId, Kind.GetValue()};
		if (!FWoTRuntimeRegistry::Get().FindDescriptor(Id).IsSet())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end descriptor reference is absent from the retained manifest"));
		switch (Requirement.Domain)
		{
		case EWoTFactDomain::Texture:
		{
			const TSharedPtr<const FWoTTexturePayloadData> TexturePayload =
				Payload.LoadTexturePayload(Requirement.LogicalId);
			if (!TexturePayload.IsValid())
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end surface texture is corrupt"));
			const FString SourceExport = Requirement.SourceExport.ToLower();
			const bool bRawSceneTexture = SurfaceTextures.Contains(SourceExport)
				|| ParticleTextures.Contains(SourceExport);
			UTexture2D* Texture = MaterializeSceneTexture(
				*TexturePayload, bRawSceneTexture);
			if (SceneIdentityKey(Requirement.SourceExport) == TEXT("environmentskinsraindropsa"))
			{
				Candidate->RainTexture.Reset(MaterializeSceneRainTexture(*TexturePayload));
				if (!Candidate->RainTexture.IsValid())
					return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end rain source mip chain is malformed"));
			}
			if (Texture == nullptr)
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end surface texture could not be staged"));
			Candidate->TexturesBySemanticKey.Add(
				SceneIdentityKey(Requirement.SourceExport),
				TStrongObjectPtr<UTexture2D>(Texture));
			if (TexturePayload->bMasked)
			{
				Candidate->MaskedTextureSemanticKeys.Add(
					SceneIdentityKey(Requirement.SourceExport));
			}
			AuthenticatedTextures.Add(Requirement.SourceExport.ToLower());
			FWoTBootstrapStatus Bound = AddSurfaceBinding(
				Requirement, Texture, false, TexturePayload->bMasked);
			if (Bound.IsError()) return FResult::Error(MoveTemp(Bound).GetError());
			break;
		}
		case EWoTFactDomain::Sound:
		{
			const TSharedPtr<const FWoTSoundPayloadData> Sound =
				Payload.LoadSoundPayload(Requirement.LogicalId);
			if (!Sound.IsValid()) return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end positional sound is corrupt"));
			UWoTPayloadSoundWave* Wave = NewObject<UWoTPayloadSoundWave>(
				GetTransientPackage(), NAME_None, RF_Transient);
			if (Wave == nullptr) return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end positional sound could not be staged"));
			Wave->PayloadKey = Requirement.LogicalId;
			Wave->PayloadChannels = Sound->Channels;
			Wave->PayloadSampleRate = Sound->SampleRate;
			Wave->PayloadBitsPerSample = Sound->BitsPerSample;
			Wave->PayloadFrameCount = static_cast<int32>(FMath::Min<uint64>(
				Sound->FrameCount, MAX_int32));
			if (!Payload.InjectSound(Wave, Requirement.LogicalId))
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end positional sound binding failed"));
			const FString FoldedSound =
				(Requirement.SourceExport + TEXT("/")
					+ Requirement.LogicalId).ToLower();
			Wave->bLooping = !FoldedSound.Contains(TEXT("thunder"));
			Candidate->SoundsBySource.Add(
				Requirement.SourceExport.ToLower(),
				TStrongObjectPtr<UWoTPayloadSoundWave>(Wave));
			break;
		}
		case EWoTFactDomain::Mesh:
		{
			auto MeshObject = FWoTRuntimeRegistry::Get().ReadObject(
				Id, 256ull * 1024ull * 1024ull,
				TEXT("ue1-vertex-mesh-le-v2"), TEXT("mesh"));
			if (MeshObject.IsError())
				return FResult::Error(MoveTemp(MeshObject).GetError());
			if ((Requirement.SourceExport + Requirement.LogicalId).Contains(
				TEXT("RainCyd"), ESearchCase::IgnoreCase))
			{
				const FWoTCanonicalJsonValue* Source =
					MeshObject.GetValue().Header().FindMember(TEXT("source"));
				const FWoTCanonicalJsonValue* SourcePackage = Source != nullptr
					? Source->FindMember(TEXT("package")) : nullptr;
				const FWoTCanonicalJsonValue* SourceExport = Source != nullptr
					? Source->FindMember(TEXT("export")) : nullptr;
				if (SourcePackage == nullptr || SourceExport == nullptr
					|| SourcePackage->GetKind() != EWoTCanonicalJsonKind::String
					|| SourceExport->GetKind() != EWoTCanonicalJsonKind::String
					|| SourcePackage->GetString() != Requirement.SourcePath
					|| SourceExport->GetString() != Requirement.SourceExport)
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end RainCyd mesh source identity is not exact"));
				FSceneMeshTopology Topology;
				FString TopologyError;
				if (!ParseSceneMeshTopology(
						MeshObject.GetValue(), Topology, TopologyError))
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end RainCyd topology is malformed"));
				UStaticMesh* RainMesh = BuildSceneMesh(Topology, TopologyError);
				if (RainMesh == nullptr)
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end RainCyd topology could not be staged"));
				Candidate->RainRenderMesh.Reset(RainMesh);
				// UE1 leaves this mesh slot null; its typed class default owns the skin.
				FString AuthenticatedClassDefaultSkin;
				const bool bClassDefaultOnlyTopology =
					Requirement.SourcePath == TEXT("System/Environment.u")
					&& Requirement.SourceExport == TEXT("Environment.RainCyd")
					&& Topology.SkinKeys.Num() == 1
					&& Topology.SkinKeys[0].IsEmpty()
					&& Topology.SectionSkins.Num() == 1
					&& !Topology.SectionSkins.ContainsByPredicate(
						[](int32 SkinIndex) { return SkinIndex != 0; });
				if (bClassDefaultOnlyTopology)
				{
					int32 SkinMatches = 0;
					for (const FWoTFrontEndResourceRequirement& CandidateRequirement
						: Descriptor.RequiredResources)
					{
						if (CandidateRequirement.Domain == EWoTFactDomain::Texture
							&& CandidateRequirement.SourcePath == TEXT("System/Environment.u")
							&& CandidateRequirement.SourceExport
								== TEXT("Environment.Skins.RainDropsA"))
						{
							AuthenticatedClassDefaultSkin =
								CandidateRequirement.SourceExport;
							++SkinMatches;
						}
					}
					if (SkinMatches != 1)
						return SceneError<FWoTFrontEndSceneStaging>(
							TEXT("front-end RainCyd authenticated class-default skin is not exact"));
				}
				for (const int32 SkinIndex : Topology.SectionSkins)
				{
					if (!Topology.SkinKeys.IsValidIndex(SkinIndex)
						|| (Topology.SkinKeys[SkinIndex].IsEmpty()
							&& AuthenticatedClassDefaultSkin.IsEmpty()))
						return SceneError<FWoTFrontEndSceneStaging>(
							TEXT("front-end RainCyd section has no semantic skin"));
					Candidate->RainSectionSkinKeys.Add(
						Topology.SkinKeys[SkinIndex].IsEmpty()
							? AuthenticatedClassDefaultSkin
							: Topology.SkinKeys[SkinIndex]);
				}
			}
			break;
		}
		case EWoTFactDomain::BSP:
			if (Requirement.LogicalId == Descriptor.BspLogicalId)
			{
				TSharedRef<FWoTBSPPayloadData> BSP =
					MakeShared<FWoTBSPPayloadData>();
				FWoTBootstrapStatus BSPDecoded =
					WoTFrontEndSceneDecodeBSP(Id, *BSP);
				if (BSPDecoded.IsError())
					return FResult::Error(MoveTemp(BSPDecoded).GetError());
				Candidate->BSP = BSP;
			}
			if (!Candidate->BSP.IsValid())
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end BSP is malformed"));
			break;
		case EWoTFactDomain::World:
		case EWoTFactDomain::Font:
			break; // Parsed world and valid font owner are stronger consumers.
		case EWoTFactDomain::Procedural:
		{
			const FString Folded = Requirement.SourceExport.ToLower();
			AuthoredWaterResources += Folded.Contains(TEXT("water"))
				|| Folded.Contains(TEXT("wave"))
				|| Folded.Contains(TEXT("wet")) ? 1u : 0u;
			FWoTRuntimeProceduralResource Procedural;
			FWoTBootstrapStatus Loaded = Procedural.LoadFirstUse(Id);
			if (Loaded.IsError()) return FResult::Error(MoveTemp(Loaded).GetError());
			if (Procedural.Mips().IsEmpty())
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end procedural water has no authenticated mip"));
			UTexture2D* ProceduralTexture =
				MaterializeSceneProceduralIndices(Procedural.Mips()[0]);
			if (ProceduralTexture == nullptr)
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end procedural water could not be staged"));
			const FString ProceduralKey =
				SceneIdentityKey(Requirement.SourceExport);
			Candidate->ProceduralSemanticKeys.Add(ProceduralKey);
			Candidate->ProceduralTextures.Add(ProceduralKey,
				TStrongObjectPtr<UTexture2D>(ProceduralTexture));
			FWoTBootstrapStatus Bound = AddSurfaceBinding(
				Requirement, ProceduralTexture, true, false);
			if (Bound.IsError()) return FResult::Error(MoveTemp(Bound).GetError());
			break;
		}
		default:
		{
			auto Read = FWoTRuntimeRegistry::Get().Read(Id, 256ull * 1024ull * 1024ull);
			if (Read.IsError()) return FResult::Error(MoveTemp(Read).GetError());
			break;
		}
		}
#if WITH_AUTOMATION_TESTS
		if (GFrontEndSceneResourceStageObserver)
			GFrontEndSceneResourceStageObserver(Requirement.Domain, DesiredStage);
#endif
	}
	return FResult::Success(FWoTFrontEndSceneStaging({}, 0));
	};
	auto BSPResources = LoadResourceStage(0);
	if (BSPResources.IsError())
		return FResult::Error(MoveTemp(BSPResources).GetError());
	if (!Candidate->BSP.IsValid() || IsCancelled())
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end scene did not finish its authenticated BSP stage"));
	// Version 5 adds the animated light-map trailer over the same entry records.
	if ((Candidate->BSP->FormatVersion != 3 && Candidate->BSP->FormatVersion != 4
			&& Candidate->BSP->FormatVersion != 5)
		|| Candidate->BSP->EntryLights.Num() != 5
		|| Candidate->ThunderEndpoints.Num() != 5 || Candidate->Rains.Num() != 6)
		return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end entry requires v3/v4/v5 weather provenance and five light routes"));
	Candidate->StrikeSettings.MinInterval = Candidate->MinLightningTime;
	Candidate->StrikeSettings.MaxInterval = Candidate->MaxLightningTime;
	Candidate->StrikeSettings.MinThunderDelay = Candidate->MinThunderDelay;
	Candidate->StrikeSettings.MaxThunderDelay = Candidate->MaxThunderDelay;
	for (const FWoTEntryLightRecord& Record : Candidate->BSP->EntryLights)
	{
		const FSceneLightPlan* Light = Candidate->Lights.FindByPredicate(
			[&](const FSceneLightPlan& Value) { return Value.Identity == Record.Identity; });
		if (!Light || !Light->bLightningGated || Light->Brightness != Record.Brightness
			|| Light->Hue != Record.Hue || Light->Saturation != Record.Saturation
			|| Record.LevelBrightness != LevelBrightness)
			return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end light provenance disagrees with authenticated world properties"));
		const FPlan::FThunderEndpoint* Endpoint = Candidate->ThunderEndpoints.FindByPredicate(
			[&](const FPlan::FThunderEndpoint& Value) { return Value.LightTag.Equals(Light->Tag, ESearchCase::IgnoreCase); });
		if (!Endpoint)
			return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end light provenance has no endpoint"));
		Candidate->StrikeSettings.LightningLengths.Add(Endpoint->Length);
	}
	FString MeshError;
	if (!WoTBuildFrontEndSkyPlan(WorldResource, *Candidate->BSP, Candidate->SkyPlan, MeshError))
		return SceneError<FWoTFrontEndSceneStaging>(*MeshError);
	// The shared render builder encodes noncolliding sections with its portal
	// role bit. This is a derived render contract, not original world membership.
	Candidate->SkyPlan.SkyMesh.bPortal = true;
	for (FWoTBSPPayloadData* Mesh : {&Candidate->SkyPlan.WorldMesh, &Candidate->SkyPlan.SkyMesh})
		for (int32 Slot = 0; Slot < Mesh->Sections.Num(); ++Slot)
			// FastBuild resolves polygon-group names through the slot table. A
			// pan split needs a unique local name while retaining style index order.
			Mesh->Sections[Slot].MaterialSlot = FString::Printf(TEXT("EntrySection_%d"), Slot);
	for (FSceneRainPlan& RainPlan : Candidate->Rains)
	{
		const FVector Start = RainPlan.Transform.GetLocation();
		const FQuat Rotation(RainPlan.Transform.Rotator() + FRotator(0, RainPlan.InitialYawDegrees, 0));
		const FVector End = Start - Rotation.GetUpVector() * RainPlan.TraceDepth;
		FVector RawHit;
		if (WoTTraceEntryGround(WorldResource.Nodes(), Candidate->BSP->bRootOutside,
			Start, End, RawHit, MeshError) != EWoTEntryTraceResult::Hit)
			return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end rain has no valid authenticated solid ground hit"));
		const FVector TowardStart = Start - RawHit;
		RainPlan.GroundHit = RawHit + TowardStart.GetSafeNormal() * FMath::Min(.5, TowardStart.Size());
	}
	if (Candidate->Progress) Candidate->Progress(
		{EWoTFrontEndSceneProgressStage::WorldAndBSP, 1, 5});
	auto SurfaceResources = LoadResourceStage(1);
	if (SurfaceResources.IsError())
		return FResult::Error(MoveTemp(SurfaceResources).GetError());
	if (Candidate->BSP->LightmapWidth == 0
		|| Candidate->BSP->LightmapHeight == 0
		|| Candidate->BSP->LightmapBGRA.IsEmpty())
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end BSP has no required lightmap atlas"));
	UTexture2D* LightmapAtlas = MaterializeSceneLightmap(*Candidate->BSP);
	if (LightmapAtlas == nullptr)
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end BSP lightmap atlas could not be staged"));
	Candidate->LightmapAtlas.Reset(LightmapAtlas);
	UStaticMesh* StagedMesh = NewObject<UStaticMesh>(
		GetTransientPackage(), NAME_None, RF_Transient);
	if (StagedMesh == nullptr
		|| !WoTBuildBSPRenderMeshOnly(*StagedMesh, Candidate->SkyPlan.WorldMesh, MeshError))
	{
		return SceneError<FWoTFrontEndSceneStaging>(
			*FString::Printf(TEXT("front-end BSP render mesh could not be staged: %s"), *MeshError));
	}
	Candidate->BSPRenderMesh.Reset(StagedMesh);
	UStaticMesh* SkyMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!SkyMesh || !WoTBuildBSPRenderMeshOnly(*SkyMesh, Candidate->SkyPlan.SkyMesh, MeshError))
		return SceneError<FWoTFrontEndSceneStaging>(*FString::Printf(
			TEXT("front-end sky render mesh could not be staged: %s"), *MeshError));
	Candidate->SkyRenderMesh.Reset(SkyMesh);
	Water = FMath::Max(Water, AuthoredWaterResources);
	for (const FString& Surface : SurfaceTextures)
	{
		if (!AuthenticatedTextures.Contains(Surface)
			&& !Candidate->ProceduralSemanticKeys.Contains(
				SceneIdentityKey(Surface)))
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end BSP names a missing authenticated surface"));
	}
	UMaterialInterface* WaterMaster = nullptr;
	if (Water > 0)
	{
		WaterMaster = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/WoT/Materials/M_WoT_WorldWaveTranslucent.M_WoT_WorldWaveTranslucent"));
		if (WaterMaster == nullptr)
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end neutral water material master is absent"));
	}
	// Validate original converter slot names before selecting each partition's
	// exact authenticated surface style (a sky texture can also be on a wall).
	for (const FWoTBSPPayloadSection& Section : Candidate->BSP->Sections)
	{
		int32 Matches = 0;
		for (const FSceneSurfaceBinding& Binding : Candidate->SurfaceBindings)
			Matches += SceneConverterSlotMatches(Section.MaterialSlot, Binding.ConverterSlotBase) ? 1 : 0;
		if (Matches != 1)
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end BSP section does not resolve one authenticated texture"));
	}
	auto BindPartition = [&](const TArray<FWoTEntrySectionStyle>& Styles,
		TArray<TStrongObjectPtr<UMaterialInstanceDynamic>>& Materials) -> bool
	{
		for (const FWoTEntrySectionStyle& Style : Styles)
		{
			const FString Key = SceneIdentityKey(Style.TextureIdentity);
			const bool bProcedural = Candidate->ProceduralSemanticKeys.Contains(Key);
			const TStrongObjectPtr<UTexture2D>* Texture = bProcedural
				? Candidate->ProceduralTextures.Find(Key) : Candidate->TexturesBySemanticKey.Find(Key);
			if (!Texture || !Texture->IsValid()) return false;
			UMaterialInterface* Master = WaterMaster;
			if (!bProcedural)
			{
				const bool bMasked = Style.Blend == EWoTEntryBlend::Masked
					|| Candidate->MaskedTextureSemanticKeys.Contains(Key);
				const TCHAR* Blend = Style.Blend == EWoTEntryBlend::Translucent ? TEXT("Translucent")
					: Style.Blend == EWoTEntryBlend::Modulated ? TEXT("Modulated")
					: bMasked ? TEXT("Masked") : TEXT("World");
				const FString Name = FString::Printf(TEXT("M_WoT_Entry%s%s"), Blend, Style.bTwoSided ? TEXT("_TwoSided") : TEXT(""));
				Master = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/WoT/Materials/%s.%s"), *Name, *Name));
			}
			if (!Master) return false;
			UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Master, GetTransientPackage());
			if (!Material) return false;
			Material->SetTextureParameterValue(TEXT("Tex"), Texture->Get());
			Material->SetTextureParameterValue(TEXT("Lightmap"), Candidate->LightmapAtlas.Get());
			Material->SetScalarParameterValue(TEXT("PanU"), Style.PanUV.X);
			Material->SetScalarParameterValue(TEXT("PanV"), Style.PanUV.Y);
			Material->SetScalarParameterValue(TEXT("WoTSceneTime"), 0.f);
			Materials.Emplace(Material);
			if (bProcedural) Candidate->WaterMaterials.Emplace(Material);
		}
		return true;
	};
	if (!BindPartition(Candidate->SkyPlan.WorldStyles, Candidate->SurfaceMaterials)
		|| !BindPartition(Candidate->SkyPlan.SkyStyles, Candidate->SkyMaterials))
		return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end partition material could not be staged"));
	if (Water > 0 && Candidate->WaterMaterials.IsEmpty())
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end authored water has no BSP surface binding"));
	if (Candidate->Progress) Candidate->Progress(
		{EWoTFrontEndSceneProgressStage::SurfacesAndLightmaps, 2, 5});
	auto WeatherResources = LoadResourceStage(2);
	if (WeatherResources.IsError())
		return FResult::Error(MoveTemp(WeatherResources).GetError());
	if (Rain > 0)
	{
		if (!Candidate->RainRenderMesh.IsValid()
			|| Candidate->RainSectionSkinKeys.IsEmpty())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end RainCyd render topology or skin is absent"));
		UMaterialInterface* RainMaster = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent.M_WoT_MeshTranslucent"));
		if (RainMaster == nullptr)
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end neutral rain material master is absent"));
		for (FSceneRainPlan& RainPlan : Candidate->Rains)
		for (const FString& SkinKey : Candidate->RainSectionSkinKeys)
		{
			const TStrongObjectPtr<UTexture2D>* Texture = Candidate->TexturesBySemanticKey.Find(
				SceneIdentityKey(SkinKey));
			if (SceneIdentityKey(SkinKey) == TEXT("environmentskinsraindropsa"))
				Texture = &Candidate->RainTexture;
			if (Texture == nullptr || !Texture->IsValid())
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end RainCyd authenticated skin is absent"));
			UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(
				RainMaster, GetTransientPackage());
			if (Material == nullptr)
				return SceneError<FWoTFrontEndSceneStaging>(
					TEXT("front-end RainCyd material could not be staged"));
			Material->SetTextureParameterValue(TEXT("Tex"), Texture->Get());
			const float Glow = FMath::Clamp(RainPlan.ScaleGlow * .5f + float(RainPlan.AmbientGlow) / 256.f, 0.f, 1.f);
			Material->SetVectorParameterValue(TEXT("MLAmbient"), FLinearColor(Glow, Glow, Glow, 1.f));
			RainPlan.Materials.Emplace(Material);
		}
	}
	for (FSceneAmbientPlan& Ambient : Candidate->Ambience)
	{
		const TStrongObjectPtr<UWoTPayloadSoundWave>* Wave =
			Candidate->SoundsBySource.Find(Ambient.SoundIdentity.ToLower());
		if (Wave == nullptr || !Wave->IsValid())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end ambient actor has no authenticated sound"));
		Ambient.Wave.Reset(Wave->Get());
	}
	for (const FString& ThunderIdentity : Candidate->ThunderSoundIdentities)
	{
		const TStrongObjectPtr<UWoTPayloadSoundWave>* Wave =
			Candidate->SoundsBySource.Find(ThunderIdentity);
		if (Wave == nullptr || !Wave->IsValid())
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end ordered thunder sound is not authenticated"));
		Candidate->ThunderWaves.Emplace(Wave->Get());
	}
	if (!Candidate->Splashes.IsEmpty())
	{
		UMaterialInterface* AddMaster = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/WoT/Materials/M_WoT_ParticleAdd.M_WoT_ParticleAdd"));
		UMaterialInterface* ModMaster = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/WoT/Materials/M_WoT_ParticleMod.M_WoT_ParticleMod"));
		if (AddMaster == nullptr || ModMaster == nullptr)
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("front-end neutral splash material masters are absent"));
		for (FSceneSplashPlan& SplashPlan : Candidate->Splashes)
		{
			for (int32 Index = 0; Index < SplashPlan.Templates.Num(); ++Index)
			{
				const TStrongObjectPtr<UTexture2D>* Texture = Candidate->TexturesBySemanticKey.Find(
					SceneIdentityKey(SplashPlan.TextureIdentities[Index]));
				if (Texture == nullptr || !Texture->IsValid())
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end splash has no authenticated particle texture"));
				SplashPlan.Templates[Index].TexWidth = Texture->Get()->GetSizeX();
				SplashPlan.Templates[Index].TexHeight = Texture->Get()->GetSizeY();
				UMaterialInstanceDynamic* Add = UMaterialInstanceDynamic::Create(
					AddMaster, GetTransientPackage());
				UMaterialInstanceDynamic* Mod = UMaterialInstanceDynamic::Create(
					ModMaster, GetTransientPackage());
				if (Add == nullptr || Mod == nullptr)
					return SceneError<FWoTFrontEndSceneStaging>(
						TEXT("front-end splash materials could not be staged"));
				Add->SetTextureParameterValue(TEXT("Tex"), Texture->Get());
				Mod->SetTextureParameterValue(TEXT("Tex"), Texture->Get());
				SplashPlan.AddMaterials.Emplace(Add);
				SplashPlan.ModMaterials.Emplace(Mod);
			}
		}
	}
	Candidate->Census.BSPSurfaces = WorldResource.BSPSurfaceCount();
	Candidate->Census.Lights = Lights;
	Candidate->Census.AmbientSounds = Ambience;
	Candidate->Census.SplashEmitters = Splash;
	Candidate->Census.RainCylinders = Rain;
	Candidate->Census.LightningActors = Lightning;
	Candidate->Census.RandomLightningControllers = RandomLightning;
	Candidate->Census.WaterSurfaces = Water;
	if (Candidate->Ambience.Num() != static_cast<int32>(Ambience)
		|| (Lightning > 0 && (Candidate->ThunderEndpoints.Num()
			!= static_cast<int32>(Lightning) || Candidate->ThunderWaves.IsEmpty())))
	{
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end positional ambience or thunder census is incomplete"));
	}
	if (Candidate->Progress) Candidate->Progress(
		{EWoTFrontEndSceneProgressStage::WeatherAndAmbience, 3, 5});
	auto FontResources = LoadResourceStage(3);
	if (FontResources.IsError())
		return FResult::Error(MoveTemp(FontResources).GetError());
	if (!FontOwner.IsValid() || !FontOwner.SharesSnapshotWith(WorldLease))
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("front-end font/localization authority expired during staging"));
	if (Candidate->Progress) Candidate->Progress(
		{EWoTFrontEndSceneProgressStage::FontsAndLocalization, 4, 5});
	if (IsCancelled())
		return SceneError<FWoTFrontEndSceneStaging>(TEXT("front-end scene staging was cancelled after resources"));
	Candidate->Controller = MakeShared<FWoTFrontEndSceneController>(Preferences.bReducedMotion, Preferences.AudioVolume
#if WITH_AUTOMATION_TESTS
		, TestAtlasUploader
#endif
		);
	if (!Candidate->Controller->ConfigureEntryLightning(Candidate->BSP.ToSharedRef(),
		Candidate->LightmapAtlas.Get(), Candidate->StrikeSettings, MeshError))
		return SceneError<FWoTFrontEndSceneStaging>(*MeshError);
	Plan = MoveTemp(Candidate);
	StagedSerial = Plan->AuthoritySerial;
	return FResult::Success(FWoTFrontEndSceneStaging(OwnerState, StagedSerial));
}

FWoTBootstrapStatus FWoTFrontEndSceneMaterializer::CommitStaged(
	uint64 Serial, APlayerController* ViewOwner)
{
	if (!IsInGameThread() || !Plan.IsValid() || Plan->bCommitted
		|| Serial == 0 || Serial != StagedSerial || !Plan->World.IsValid()
		|| (Plan->PinnedWorldLease.IsValid()
			&& !Plan->PinnedWorldLease->IsValid()))
	{
		Rollback();
		return SceneStatus(TEXT("front-end scene commit received stale staging"));
	}
	if ((Plan->Cancellation && Plan->Cancellation())
#if WITH_AUTOMATION_TESTS
		|| GCancelNextFrontEndSceneCommit
#endif
		)
	{
#if WITH_AUTOMATION_TESTS
		GCancelNextFrontEndSceneCommit = false;
#endif
		Rollback();
		return SceneStatus(TEXT("front-end scene commit was cancelled"));
	}
	const IConsoleVariable* DelaySpawnNotification =
		IConsoleManager::Get().FindConsoleVariable(
			TEXT("s.DelayOnActorSpawnedUntilFinishedSpawning"));
	if (DelaySpawnNotification == nullptr || DelaySpawnNotification->GetInt() == 0)
	{
		Rollback();
		return SceneStatus(TEXT(
			"front-end scene requires finished-spawn actor notification ordering"));
	}
#if WITH_AUTOMATION_TESTS
	if (GFailNextFrontEndSceneCommit)
	{
		GFailNextFrontEndSceneCommit = false;
		Rollback();
		return SceneStatus(TEXT("front-end scene failed immediately before commit"));
	}
#endif
	UWorld* World = Plan->World.Get();
	UMaterialInterface* GammaBoundary = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/Instances/MI_GammaBoundary.MI_GammaBoundary"));
	if (GammaBoundary == nullptr)
	{
		Rollback();
		return SceneStatus(TEXT("front-end neutral gamma boundary is absent"));
	}
	struct FPublicationGuardScope
	{
		FPublicationGuardScope() { ++GFrontEndScenePublicationGuardDepth; }
		~FPublicationGuardScope() { --GFrontEndScenePublicationGuardDepth; }
	} PublicationGuard;
	FActorSpawnParameters Parameters;
	Parameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Parameters.bDeferConstruction = true;
	Parameters.ObjectFlags |= RF_Transient;
	AActor* Root = World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform::Identity, Parameters);
	if (Root == nullptr)
	{
		Rollback();
		return SceneStatus(TEXT("front-end scene root allocation failed"));
	}
	ActiveRoot = Root;
	Root->Tags.AddUnique(TEXT("WoTAuthenticatedFrontEndScene"));
	Root->SetActorHiddenInGame(true);
	Root->SetActorEnableCollision(false);
	Root->PrimaryActorTick.bStartWithTickEnabled = false;
	Root->SetActorTickEnabled(false);
	USceneComponent* SceneRoot = NewObject<USceneComponent>(Root,
		TEXT("AuthenticatedFrontEndSceneRoot"), RF_Transient);
	if (SceneRoot == nullptr)
	{
		Root->Destroy();
		Rollback();
		return SceneStatus(TEXT("front-end scene root component allocation failed"));
	}
	Root->SetRootComponent(SceneRoot);
	Root->AddInstanceComponent(SceneRoot);
	SceneRoot->RegisterComponent();
#if WITH_AUTOMATION_TESTS
	if (GFrontEndScenePublicationObserver)
		GFrontEndScenePublicationObserver(TEXT("component-register"), ObserveForTesting());
#endif

	UCameraComponent* Camera = NewObject<UCameraComponent>(Root,
		TEXT("AuthenticatedFrontEndCamera"), RF_Transient);
	if (Camera == nullptr)
	{
		Root->Destroy();
		Rollback();
		return SceneStatus(TEXT("front-end camera allocation failed"));
	}
	Root->AddInstanceComponent(Camera);
	Camera->SetupAttachment(SceneRoot);
	Camera->SetRelativeLocation(Plan->Descriptor.CameraLocation);
	Camera->SetRelativeRotation(Plan->Descriptor.CameraRotation);
	Camera->SetFieldOfView(Plan->Descriptor.HorizontalFov);
	Camera->SetConstraintAspectRatio(false);
	Camera->bOverrideAspectRatioAxisConstraint = true;
	Camera->SetAspectRatioAxisConstraint(
		EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV);
	Camera->PostProcessSettings.bOverride_ToneCurveAmount = true;
	Camera->PostProcessSettings.ToneCurveAmount = 0.0f;
	Camera->PostProcessSettings.bOverride_ExpandGamut = true;
	Camera->PostProcessSettings.ExpandGamut = 0.0f;
	Camera->PostProcessSettings.WeightedBlendables.Array.Add(
		FWeightedBlendable(1.0f, GammaBoundary));
	Camera->PostProcessBlendWeight = 1.0f;
	Camera->RegisterComponent();
#if WITH_AUTOMATION_TESTS
	if (GFrontEndScenePublicationObserver)
		GFrontEndScenePublicationObserver(TEXT("component-register"), ObserveForTesting());
#endif

	if (!Plan->Controller.IsValid())
	{
		Plan->Controller = MakeShared<FWoTFrontEndSceneController>(
			Plan->Preferences.bReducedMotion, Plan->Preferences.AudioVolume
#if WITH_AUTOMATION_TESTS
			, TestAtlasUploader
#endif
			);
		Plan->Controller->ConfigureLightningTiming(
			Plan->MinLightningTime, Plan->MaxLightningTime,
			Plan->MinThunderDelay, Plan->MaxThunderDelay);
		if (Plan->BSP.IsValid())
		{
			FString Error;
			if (!Plan->Controller->ConfigureEntryLightning(Plan->BSP.ToSharedRef(),
				Plan->LightmapAtlas.Get(), Plan->StrikeSettings, Error))
			{
				Rollback(); return SceneStatus(*Error);
			}
		}
	}
	for (const auto& Material : Plan->SurfaceMaterials)
		Plan->Controller->AddSkyMaterial(Material.Get());
	for (const auto& Material : Plan->SkyMaterials)
		Plan->Controller->AddSkyMaterial(Material.Get());
	for (const TStrongObjectPtr<UMaterialInstanceDynamic>& Water
		: Plan->WaterMaterials)
	{
		Plan->Controller->AddWaterMaterial(Water.Get());
	}
	auto AddSceneComponent = [&](USceneComponent* Component) -> bool
	{
		if (Component == nullptr) return false;
		Component->SetVisibility(false, true);
		Root->AddInstanceComponent(Component);
		Component->SetupAttachment(SceneRoot);
		Component->SetComponentTickEnabled(false);
		if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
		{
			Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Primitive->SetGenerateOverlapEvents(false);
		}
		Component->RegisterComponent();
		Plan->PublishedComponents.Add(Component);
#if WITH_AUTOMATION_TESTS
		if (GFrontEndScenePublicationObserver)
			GFrontEndScenePublicationObserver(TEXT("component-register"), ObserveForTesting());
#endif
		return true;
	};

	// The decoded mesh has one real polygon group/material slot per authenticated
	// BSP section. One component publishes that complete geometry; census is
	// verified from mesh sections, never padded with empty components.
	if (Plan->Census.BSPSurfaces > 0)
	{
		UStaticMeshComponent* Surface = NewObject<UStaticMeshComponent>(
			Root, TEXT("AuthenticatedBSPMesh"), RF_Transient);
		if (!AddSceneComponent(Surface))
		{
			Root->Destroy(); Rollback();
			return SceneStatus(
				TEXT("front-end BSP surface allocation failed"));
		}
		if (Plan->BSPRenderMesh.IsValid())
		{
			Surface->SetStaticMesh(Plan->BSPRenderMesh.Get());
			for (int32 MaterialIndex = 0;
				MaterialIndex < Plan->SurfaceMaterials.Num(); ++MaterialIndex)
			{
				Surface->SetMaterial(MaterialIndex,
					Plan->SurfaceMaterials[MaterialIndex].Get());
			}
		}
	}
	for (uint32 Index = 0; Index < Plan->Census.Lights; ++Index)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(Root,
			NAME_None, RF_Transient);
		if (!AddSceneComponent(Light))
		{
			Root->Destroy(); Rollback();
			return SceneStatus(TEXT("front-end light allocation failed"));
		}
		if (Plan->Lights.IsValidIndex(static_cast<int32>(Index)))
		{
			const FSceneLightPlan& Authored = Plan->Lights[Index];
			Light->SetMobility(EComponentMobility::Movable);
			Light->bUseInverseSquaredFalloff = false;
			Light->LightFalloffExponent = 2.0f;
			Light->SetIntensity(Authored.Intensity);
			Light->SetAttenuationRadius(Authored.Radius);
			Light->SetLightColor(Authored.Color);
			Light->SetRelativeTransform(
				Authored.Transform);
		}
		if (Plan->Lights.IsValidIndex(static_cast<int32>(Index))
			&& Plan->Lights[Index].bLightningGated)
		{
			Light->SetVisibility(false, true);
			Plan->Controller->AddLightningLight(Light);
		}
	}
	for (uint32 Index = 0; Index < Plan->Census.AmbientSounds; ++Index)
	{
		UAudioComponent* Audio = NewObject<UAudioComponent>(Root,
			NAME_None, RF_Transient);
		if (!AddSceneComponent(Audio))
		{
			Root->Destroy(); Rollback();
			return SceneStatus(TEXT("front-end ambience allocation failed"));
		}
		Audio->bAutoActivate = false;
		Audio->bIsUISound = false;
		Audio->SetVolumeMultiplier(0.0f);
		Audio->bAllowSpatialization = true;
		Audio->bOverrideAttenuation = true;
		Audio->AttenuationOverrides.bSpatialize = true;
		if (Plan->Ambience.IsValidIndex(static_cast<int32>(Index)))
		{
			const FSceneAmbientPlan& Authored = Plan->Ambience[Index];
			Audio->AttenuationOverrides.AttenuationShapeExtents.X =
				Authored.Radius;
			Audio->AttenuationOverrides.FalloffDistance = Authored.Radius;
			Audio->SetRelativeTransform(
				Authored.Transform);
			Audio->SetSound(Authored.Wave.Get());
			Audio->SetVolumeMultiplier(Authored.Volume);
		}
		Plan->Controller->AddAmbient(Audio,
			Plan->Ambience.IsValidIndex(static_cast<int32>(Index))
				? Plan->Ambience[Index].Volume : 1.0f);
	}
	for (const TStrongObjectPtr<UWoTPayloadSoundWave>& Wave : Plan->ThunderWaves)
		Plan->Controller->AddThunderWave(Wave.Get());
	for (const FPlan::FThunderEndpoint& Endpoint : Plan->ThunderEndpoints)
	{
		UAudioComponent* Thunder = NewObject<UAudioComponent>(
			Root, NAME_None, RF_Transient);
		if (!AddSceneComponent(Thunder))
		{
			Root->Destroy(); Rollback();
			return SceneStatus(TEXT("front-end thunder allocation failed"));
		}
		Thunder->bAutoActivate = false;
		Thunder->bIsUISound = false;
		Thunder->SetVolumeMultiplier(0.0f);
		Thunder->bAllowSpatialization = true;
		Thunder->bOverrideAttenuation = true;
		Thunder->AttenuationOverrides.bSpatialize = true;
		Thunder->AttenuationOverrides.AttenuationShapeExtents.X = Endpoint.Radius;
		Thunder->AttenuationOverrides.FalloffDistance = Endpoint.Radius;
		Thunder->SetRelativeTransform(Endpoint.Transform);
		Plan->Controller->AddThunderEndpoint(Thunder, Endpoint.Volume);
	}
	if (Plan->Cancellation && Plan->Cancellation())
	{
		Rollback();
		return SceneStatus(TEXT("front-end scene commit was cancelled before publication"));
	}
	Root->FinishSpawning(FTransform::Identity, true);
#if WITH_AUTOMATION_TESTS
	if (GFrontEndScenePublicationObserver)
		GFrontEndScenePublicationObserver(TEXT("actor-begin-play"), ObserveForTesting());
#endif
	auto SpawnWeatherActor = [&](UClass* ActorClass,
		const FTransform& Transform,
		TFunctionRef<bool(AActor&)> Configure) -> AActor*
	{
		FActorSpawnParameters WeatherParameters;
		WeatherParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		WeatherParameters.bDeferConstruction = true;
		WeatherParameters.ObjectFlags |= RF_Transient;
		AActor* Actor = World->SpawnActor<AActor>(
			ActorClass, Transform, WeatherParameters);
		if (Actor == nullptr) return nullptr;
		Actor->SetActorEnableCollision(false);
		Actor->Tags.AddUnique(TEXT("WoTAuthenticatedFrontEndScene"));
		Actor->SetActorHiddenInGame(true);
		Actor->PrimaryActorTick.bStartWithTickEnabled = false;
		Actor->SetActorTickEnabled(false);
		Actor->AttachToActor(Root, FAttachmentTransformRules::KeepWorldTransform);
		if (!Configure(*Actor))
		{
			Actor->Destroy();
			return nullptr;
		}
		Actor->FinishSpawning(Transform, true);
		ActiveWeatherActors.Add(Actor);
#if WITH_AUTOMATION_TESTS
		if (GFrontEndScenePublicationObserver)
			GFrontEndScenePublicationObserver(TEXT("actor-begin-play"), ObserveForTesting());
#endif
		return Actor;
	};
	if (Plan->SkyRenderMesh.IsValid())
	{
		const FTransform SkyTransform(Plan->SkyPlan.Rotation,
			Plan->Descriptor.CameraLocation, FVector(Plan->SkyPlan.Scale));
		AWoTSkyBox* Sky = Cast<AWoTSkyBox>(SpawnWeatherActor(AWoTSkyBox::StaticClass(),
			SkyTransform, [&](AActor& Actor)
			{
				AWoTSkyBox& Box = *CastChecked<AWoTSkyBox>(&Actor);
				Box.SkyRotation = Plan->SkyPlan.Rotation;
				Box.SkyScale = Plan->SkyPlan.Scale;
				UStaticMeshComponent* Mesh = Box.GetStaticMeshComponent();
				Mesh->SetStaticMesh(Plan->SkyRenderMesh.Get());
				for (int32 Slot = 0; Slot < Plan->SkyMaterials.Num(); ++Slot)
					Mesh->SetMaterial(Slot, Plan->SkyMaterials[Slot].Get());
				return true;
			}));
		if (!Sky) { Rollback(); return SceneStatus(TEXT("front-end sky actor allocation failed")); }
		Plan->SkyActor = Sky;
	}
	for (uint32 Index = 0; Index < Plan->Census.SplashEmitters; ++Index)
	{
		const FTransform Transform = Plan->Splashes.IsValidIndex(
			static_cast<int32>(Index))
			? Plan->Splashes[static_cast<int32>(Index)].Transform
			: FTransform::Identity;
		const FSceneSplashPlan* Authored = Plan->Splashes.IsValidIndex(
			static_cast<int32>(Index)) ? &Plan->Splashes[Index] : nullptr;
		AWoTParticleSprayer* Splash = Cast<AWoTParticleSprayer>(
			SpawnWeatherActor(AWoTParticleSprayer::StaticClass(), Transform,
				[Authored](AActor& Actor)
				{
					AWoTParticleSprayer* Sprayer = Cast<AWoTParticleSprayer>(&Actor);
					if (Sprayer == nullptr || Authored == nullptr
						|| Authored->Templates.IsEmpty()) return false;
					Sprayer->Templates = Authored->Templates;
					Sprayer->AddMaterials.Reset();
					Sprayer->ModMaterials.Reset();
					for (const TStrongObjectPtr<UMaterialInstanceDynamic>& Material
						: Authored->AddMaterials) Sprayer->AddMaterials.Add(Material.Get());
					for (const TStrongObjectPtr<UMaterialInstanceDynamic>& Material
						: Authored->ModMaterials) Sprayer->ModMaterials.Add(Material.Get());
					Sprayer->Spread = Authored->Spread;
					Sprayer->Volume = Authored->Volume;
					Sprayer->MinVolume = Authored->MinVolume;
					Sprayer->Gravity = Authored->Gravity;
					Sprayer->AuthoredGravity = Authored->Gravity;
					Sprayer->PrimeCount = Authored->PrimeCount;
					Sprayer->VolumeScalePct = Authored->VolumeScalePct;
					Sprayer->VisibilityRadius = Authored->VisibilityRadius;
					Sprayer->VisibilityHeight = Authored->VisibilityHeight;
					Sprayer->bLOSClip = Authored->bLOSClip;
					Sprayer->bLinearDistribution = Authored->bLinearDistribution;
					Sprayer->bOn = false;
					return Sprayer->Templates.Num() == Sprayer->AddMaterials.Num()
						&& Sprayer->Templates.Num() == Sprayer->ModMaterials.Num();
				}));
		if (Splash == nullptr)
		{
			Rollback();
			return SceneStatus(TEXT("front-end splash actor allocation failed"));
		}
	}
	for (uint32 Index = 0; Index < Plan->Census.RainCylinders; ++Index)
	{
		const FTransform Transform = Plan->Rains.IsValidIndex(
			static_cast<int32>(Index))
			? Plan->Rains[static_cast<int32>(Index)].Transform
			: FTransform::Identity;
		const FSceneRainPlan* Authored = Plan->Rains.IsValidIndex(
			static_cast<int32>(Index)) ? &Plan->Rains[Index] : nullptr;
		TArray<UMaterialInterface*> RainMaterials;
		for (const TStrongObjectPtr<UMaterialInstanceDynamic>& Material
			: Authored && !Authored->Materials.IsEmpty() ? Authored->Materials : Plan->RainMaterials)
			RainMaterials.Add(Material.Get());
		AWoTRainCyd* RainActor = Cast<AWoTRainCyd>(SpawnWeatherActor(
			AWoTRainCyd::StaticClass(), Transform,
			[&, Authored](AActor& Actor)
			{
				AWoTRainCyd* Rain = Cast<AWoTRainCyd>(&Actor);
				if (Rain == nullptr || Authored == nullptr
					|| !Plan->RainRenderMesh.IsValid()) return false;
				Rain->CylinderHeight = Authored->CylinderHeight;
				Rain->StackNum = Authored->StackNum;
				Rain->MinFallRate = Authored->MinFallRate;
				Rain->MaxFallRate = Authored->MaxFallRate;
				Rain->TraceDepth = Authored->TraceDepth;
				Rain->BuryDistance = Authored->BuryDistance;
				if (Authored->GroundHit.IsSet()
					&& !Rain->SetFrontEndGroundHit(Authored->GroundHit.GetValue())) return false;
				Rain->SetFrontEndStagedMotion(
					Authored->InitialYawDegrees, Authored->InitialFallRate);
				Rain->SetRuntimeRenderMesh(
					Plan->RainRenderMesh.Get(), RainMaterials);
				return Rain->HasRuntimeRenderMesh()
					&& Rain->RuntimeRenderMaterialCount() == RainMaterials.Num();
			}));
		if (RainActor == nullptr)
		{
			Rollback();
			return SceneStatus(TEXT("front-end RainCyd actor allocation failed"));
		}
	}
	if (Plan->Cancellation && Plan->Cancellation())
	{
		Rollback();
		return SceneStatus(TEXT("front-end scene commit was cancelled during publication"));
	}
#if WITH_AUTOMATION_TESTS
	if (GFailAfterFrontEndSceneConstruction)
	{
		GFailAfterFrontEndSceneConstruction = false;
		Rollback();
		return SceneStatus(TEXT("front-end scene failed after hidden construction"));
	}
	if (GRevokeAfterFrontEndSceneConstruction)
	{
		GRevokeAfterFrontEndSceneConstruction = false;
		if (Plan->PinnedWorldLease.IsValid()) Plan->PinnedWorldLease->Revoke();
		Rollback();
		return SceneStatus(TEXT("front-end scene authority was revoked after construction"));
	}
#endif
	if (!Plan->Controller->Prepare())
	{
		Root->Destroy(); Rollback();
		return SceneStatus(TEXT("front-end scene controller could not start"));
	}
#if WITH_AUTOMATION_TESTS
	if (GFailAfterFrontEndSceneControllerPreparation)
	{
		GFailAfterFrontEndSceneControllerPreparation = false;
		Rollback();
		return SceneStatus(TEXT("front-end scene failed after inert controller preparation"));
	}
	if (GRevokeAfterFrontEndSceneControllerPreparation)
	{
		GRevokeAfterFrontEndSceneControllerPreparation = false;
		if (Plan->PinnedWorldLease.IsValid()) Plan->PinnedWorldLease->Revoke();
		Rollback();
		return SceneStatus(TEXT("front-end scene authority was revoked after preparation"));
	}
#endif
	// Revocation serializes on the same verified-operation guard. Nothing below
	// this point can fail: it is the sole game-thread publication boundary.
	TUniquePtr<FScopeLock> AuthorityPublication;
	if (Plan->PinnedWorldLease.IsValid())
		AuthorityPublication = MakeUnique<FScopeLock>(
			&Plan->PinnedWorldLease->GetVerifiedReadGuard());
#if WITH_AUTOMATION_TESTS
	if (GRevokeDuringFrontEndScenePublication)
	{
		GRevokeDuringFrontEndScenePublication = false;
		if (Plan->PinnedWorldLease.IsValid())
		{
			FEvent* Started = FPlatformProcess::GetSynchEventFromPool(true);
			TSharedPtr<FWoTCacheGenerationLease, ESPMode::ThreadSafe> Revoker(
				new FWoTCacheGenerationLease(*Plan->PinnedWorldLease));
			TFuture<void> Revocation = Async(EAsyncExecution::Thread,
				[Revoker, Started]()
				{
					Started->Trigger();
					Revoker->Revoke();
				});
			Started->Wait();
			const bool bRacedPublication = Revocation.IsReady();
			Rollback();
			AuthorityPublication.Reset();
			Revocation.Wait();
			FPlatformProcess::ReturnSynchEventToPool(Started);
			return SceneStatus(bRacedPublication
				? TEXT("front-end scene revocation raced publication")
				: TEXT("front-end scene revocation serialized at publication"));
		}
	}
#endif
	if (Plan->PinnedWorldLease.IsValid()
		&& !Plan->PinnedWorldLease->CanPublishPresentation())
	{
		Rollback();
		return SceneStatus(TEXT("front-end scene authority expired at publication"));
	}
	int32 PublishedSplashIndex = 0;
	for (const TWeakObjectPtr<AActor>& Weak : ActiveWeatherActors)
	{
		if (AActor* Actor = Weak.Get())
		{
			Actor->SetActorHiddenInGame(false);
			Actor->SetActorTickEnabled(!Plan->Preferences.bReducedMotion);
			if (AWoTParticleSprayer* Splash = Cast<AWoTParticleSprayer>(Actor))
			{
				Splash->bOn = !Plan->Preferences.bReducedMotion
					&& Plan->Splashes.IsValidIndex(PublishedSplashIndex)
					&& Plan->Splashes[PublishedSplashIndex].bOn;
				++PublishedSplashIndex;
			}
		}
	}
	for (const TWeakObjectPtr<UActorComponent>& Weak : Plan->PublishedComponents)
		if (USceneComponent* Component = Cast<USceneComponent>(Weak.Get()))
			Component->SetVisibility(true, true);
	Plan->Controller->ApplyInitialLightningGate();
	Root->SetActorHiddenInGame(false);
	if (ViewOwner == nullptr) ViewOwner = World->GetFirstPlayerController();
	if (ViewOwner != nullptr)
	{
		Plan->ViewOwner = ViewOwner;
		Plan->PreviousViewTarget = ViewOwner->GetViewTarget();
		ViewOwner->SetViewTarget(Root);
	}
	Plan->bCommitted = true;
	StagedSerial = 0;
	Plan->Controller->Publish();
	AuthorityPublication.Reset();
	if (Plan->Progress) Plan->Progress(
		{EWoTFrontEndSceneProgressStage::Commit, 5, 5});
	return FWoTBootstrapStatus::Success();
}

void FWoTFrontEndSceneMaterializer::Rollback()
{
	check(IsInGameThread());
	TUniquePtr<FWoTCacheGenerationLease> RevocationCompletion;
	if (Plan.IsValid() && Plan->PinnedWorldLease.IsValid()
		&& Plan->PinnedWorldLease->IsRevocationInProgress())
	{
		RevocationCompletion.Reset(new FWoTCacheGenerationLease(
			*Plan->PinnedWorldLease));
	}
	if (Plan.IsValid() && Plan->PinnedWorldLease.IsValid()
		&& Plan->RevocationSubscription != 0)
	{
		Plan->PinnedWorldLease->UnsubscribeRevocation(
			Plan->RevocationSubscription);
		Plan->RevocationSubscription = 0;
	}
	RestoreViewTarget();
	HidePublishedScene();
	if (Plan.IsValid() && Plan->Controller.IsValid()) Plan->Controller->Stop();
	for (const TWeakObjectPtr<AActor>& Weak : ActiveWeatherActors)
	{
		if (AActor* Actor = Weak.Get()) Actor->Destroy();
	}
	ActiveWeatherActors.Reset();
	if (AActor* Root = ActiveRoot.Get()) Root->Destroy();
	ActiveRoot.Reset();
	Plan.Reset();
	StagedSerial = 0;
	if (RevocationCompletion.IsValid())
		RevocationCompletion->CompleteDeferredRevocation();
}

void FWoTFrontEndSceneMaterializer::HidePublishedScene()
{
	if (AActor* Root = ActiveRoot.Get()) Root->SetActorHiddenInGame(true);
	for (const TWeakObjectPtr<AActor>& Weak : ActiveWeatherActors)
		if (AActor* Actor = Weak.Get())
		{
			Actor->SetActorHiddenInGame(true);
			Actor->SetActorTickEnabled(false);
		}
}

void FWoTFrontEndSceneMaterializer::RestoreViewTarget()
{
	if (!Plan.IsValid()) return;
	APlayerController* Owner = Plan->ViewOwner.Get();
	AActor* Root = ActiveRoot.Get();
	if (Owner != nullptr && Root != nullptr && Owner->GetViewTarget() == Root)
	{
		AActor* Previous = Plan->PreviousViewTarget.Get();
		Owner->SetViewTarget(Previous != nullptr && Previous != Root
			? Previous : static_cast<AActor*>(Owner));
	}
	Plan->ViewOwner.Reset();
	Plan->PreviousViewTarget.Reset();
}

void FWoTFrontEndSceneMaterializer::RollbackStaged(uint64 Serial)
{
	check(IsInGameThread());
	if (Serial != 0 && Serial == StagedSerial
		&& Plan.IsValid() && !Plan->bCommitted) Rollback();
}

void FWoTFrontEndSceneMaterializer::HandleLeaseRevoked(uint64 AuthoritySerial)
{
	check(IsInGameThread());
	if (!Plan.IsValid() || Plan->AuthoritySerial != AuthoritySerial) return;
	if (Plan->PinnedWorldLease.IsValid()
		&& Plan->PinnedWorldLease
			->CanRecoverFrontEndAfterRuntimeIntegrityFailure())
	{
		// Every object needed by the entry scene was already verified as part of
		// the startup-critical closure and is retained strongly by this plan. Drop
		// the revoked byte authority; rematerialization performs no cache reads.
		Plan->RevocationSubscription = 0;
		Plan->PinnedWorldLease.Reset();
		return;
	}
	Rollback();
}

void FWoTFrontEndSceneMaterializer::HandleWorldCleanup(
	UWorld* World, bool, bool)
{
	if (!IsInGameThread() || World == nullptr || !Plan.IsValid()
		|| Plan->World.Get() != World) return;
	HidePublishedScene();
	if (Plan->Controller.IsValid()) Plan->Controller->Stop();
	ActiveWeatherActors.Reset();
	ActiveRoot.Reset();
	Plan->PublishedComponents.Reset();
	Plan->Controller.Reset();
	Plan->World.Reset();
	Plan->bCommitted = false;
	ObservationWorld.Reset();
}

void FWoTFrontEndSceneMaterializer::SuspendBeforeTravel()
{
	if (!Plan.IsValid()) return;
	RestoreViewTarget();
	HidePublishedScene();
	if (Plan->Controller.IsValid()) Plan->Controller->Stop();
	for (const TWeakObjectPtr<AActor>& Weak : ActiveWeatherActors)
	{
		if (AActor* Actor = Weak.Get()) Actor->Destroy();
	}
	ActiveWeatherActors.Reset();
	if (AActor* Root = ActiveRoot.Get()) Root->Destroy();
	ActiveRoot.Reset();
	Plan->PublishedComponents.Reset();
	Plan->Controller.Reset();
	Plan->bCommitted = false;
}

FWoTBootstrapStatus FWoTFrontEndSceneMaterializer::Rematerialize(
	UWorld& World, APlayerController* ViewOwner)
{
	if (!Plan.IsValid() || Plan->bCommitted
		|| (Plan->PinnedWorldLease.IsValid()
			&& !Plan->PinnedWorldLease->IsValid()))
		return SceneStatus(TEXT("front-end scene has no valid retained snapshot"));
	// Rematerialize also accepts an inert, never-committed staged plan. Its
	// controller still targets the staged atlas; every reentry needs a new
	// generation before replacing receivers, even without a prior suspension.
	if (Plan->Controller.IsValid()) Plan->Controller->Stop();
	Plan->Controller.Reset();
	if (Plan->BSP.IsValid())
	{
		// Never update/recreate the detached atlas resource: accepted RHI jobs
		// retain its raw resource pointer as well as a strong texture owner.
		UTexture2D* Fresh = MaterializeSceneLightmap(*Plan->BSP);
		if (!Fresh) { Rollback(); return SceneStatus(TEXT("front-end steady atlas reentry allocation failed")); }
		Plan->LightmapAtlas.Reset(Fresh);
		for (const auto& Material : Plan->SurfaceMaterials)
			Material->SetTextureParameterValue(TEXT("Lightmap"), Fresh);
		for (const auto& Material : Plan->SkyMaterials)
			Material->SetTextureParameterValue(TEXT("Lightmap"), Fresh);
		for (const auto& Material : Plan->WaterMaterials)
			Material->SetTextureParameterValue(TEXT("Lightmap"), Fresh);
	}
	Plan->World = &World;
	ObservationWorld = &World;
	StagedSerial = NextSerial++;
	return CommitStaged(StagedSerial, ViewOwner);
}

void FWoTFrontEndSceneMaterializer::Shutdown()
{
	check(IsInGameThread());
	Rollback();
}

bool FWoTFrontEndSceneMaterializer::IsCommitted() const
{
	return Plan.IsValid() && Plan->bCommitted && ActiveRoot.IsValid()
		&& (!Plan->PinnedWorldLease.IsValid() || Plan->PinnedWorldLease->IsValid());
}

bool FWoTFrontEndSceneMaterializer::IsSuspendedForWorld(
	const UWorld& World) const
{
	return Plan.IsValid() && !Plan->bCommitted && Plan->World.Get() == &World
		&& (!Plan->PinnedWorldLease.IsValid()
			|| Plan->PinnedWorldLease->CanPublishPresentation());
}

FWoTFrontEndSceneStagedCensus
FWoTFrontEndSceneMaterializer::StagedCensus(uint64 Serial) const
{
	return Plan.IsValid() && Serial == StagedSerial
		? Plan->Census : FWoTFrontEndSceneStagedCensus{};
}

#if WITH_AUTOMATION_TESTS
TWoTBootstrapResult<FWoTFrontEndSceneStaging>
FWoTFrontEndSceneMaterializer::StageForTesting(
	UWorld& World, const FWoTFrontEndDescriptor& Descriptor,
	const FWoTFrontEndSceneTestFixture& Fixture,
	const FWoTFrontEndScenePreferences& Preferences)
{
	if (!IsInGameThread())
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("synthetic front-end scene staging is restricted to the game thread"));
	Rollback();
	if (Fixture.SnapshotIdentity.IsEmpty()
		|| Fixture.SnapshotIdentity != Fixture.FontSnapshotIdentity
		|| Fixture.bMalformedBSP || Fixture.bMissingSurface
		|| Fixture.bCorruptSound || Fixture.bCorruptFont || Fixture.bCancelled)
	{
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("synthetic front-end scene fixture failed staging"));
	}
	Plan = MakeUnique<FPlan>();
	Plan->World = &World;
	ObservationWorld = &World;
	Plan->Descriptor = Descriptor;
	Plan->Preferences = Preferences;
	Plan->SnapshotIdentity = Fixture.SnapshotIdentity;
	Plan->Census.BSPSurfaces = Fixture.BSPSurfaceCount;
	Plan->Census.Lights = Descriptor.Census.Lights;
	Plan->Census.AmbientSounds = Fixture.PositionalAmbienceCount;
	Plan->Census.SplashEmitters = Descriptor.Census.SplashEmitters;
	Plan->Census.RainCylinders = Descriptor.Census.RainCylinders;
	Plan->Census.LightningActors = Descriptor.Census.LightningActors;
	Plan->Census.RandomLightningControllers =
		Descriptor.Census.RandomLightningControllers;
	Plan->Census.WaterSurfaces = Fixture.WaterSurfaceCount;
	for (uint32 Index = 0; Index < Plan->Census.Lights; ++Index)
		Plan->Lights.AddDefaulted();
	for (uint32 Index = 0; Index < Plan->Census.AmbientSounds; ++Index)
	{
		FSceneAmbientPlan& Ambient = Plan->Ambience.AddDefaulted_GetRef();
		Ambient.Wave.Reset(NewObject<UWoTPayloadSoundWave>(
			GetTransientPackage(), NAME_None, RF_Transient));
	}
	for (uint32 Index = 0; Index < Plan->Census.SplashEmitters; ++Index)
	{
		FSceneSplashPlan& Splash = Plan->Splashes.AddDefaulted_GetRef();
		Splash.Templates.AddDefaulted();
		Splash.AddMaterials.Emplace(nullptr);
		Splash.ModMaterials.Emplace(nullptr);
	}
	for (uint32 Index = 0; Index < Plan->Census.RainCylinders; ++Index)
		Plan->Rains.AddDefaulted();
	FSceneMeshTopology SyntheticMesh;
	SyntheticMesh.Positions = {
		FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0)};
	SyntheticMesh.UVs = {
		FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)};
	FSceneMeshTriangle& Triangle = SyntheticMesh.Triangles.AddDefaulted_GetRef();
	Triangle.Vertices[0] = 0; Triangle.Vertices[1] = 1; Triangle.Vertices[2] = 2;
	Triangle.UVs[0] = 0; Triangle.UVs[1] = 1; Triangle.UVs[2] = 2;
	SyntheticMesh.SectionSkins.Add(0);
	SyntheticMesh.SkinKeys.Add(TEXT("synthetic"));
	FString SyntheticError;
	UStaticMesh* Mesh = BuildSceneMesh(SyntheticMesh, SyntheticError);
	if (Mesh == nullptr)
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("synthetic front-end render mesh failed staging"));
	Plan->BSPRenderMesh.Reset(Mesh);
	Plan->RainRenderMesh.Reset(Mesh);
	FWoTBSPPayloadData SyntheticLightmap;
	SyntheticLightmap.LightmapWidth = 1;
	SyntheticLightmap.LightmapHeight = 1;
	SyntheticLightmap.LightmapBGRA = {128, 128, 128, 255};
	Plan->LightmapAtlas.Reset(MaterializeSceneLightmap(SyntheticLightmap));
	if (!Plan->LightmapAtlas.IsValid())
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("synthetic front-end lightmap failed staging"));
	Plan->RainSectionSkinKeys.Add(TEXT("synthetic"));
	UMaterialInterface* RainMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent.M_WoT_MeshTranslucent"));
	UMaterialInstanceDynamic* RainMaterial = RainMaster != nullptr
		? UMaterialInstanceDynamic::Create(RainMaster, GetTransientPackage())
		: nullptr;
	if (RainMaterial == nullptr)
		return SceneError<FWoTFrontEndSceneStaging>(
			TEXT("synthetic front-end rain material failed staging"));
	Plan->RainMaterials.Emplace(RainMaterial);
	if (Fixture.WaterSurfaceCount > 0)
	{
		UMaterialInterface* WaterMaster = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/WoT/Materials/M_WoT_WorldWaveTranslucent.M_WoT_WorldWaveTranslucent"));
		UMaterialInstanceDynamic* Water = WaterMaster != nullptr
			? UMaterialInstanceDynamic::Create(WaterMaster, GetTransientPackage())
			: nullptr;
		if (Water == nullptr)
			return SceneError<FWoTFrontEndSceneStaging>(
				TEXT("synthetic front-end water material failed staging"));
		Water->SetTextureParameterValue(TEXT("Lightmap"), Plan->LightmapAtlas.Get());
		Plan->WaterMaterials.Emplace(Water);
		Plan->SurfaceMaterials.Emplace(Water);
	}
	StagedSerial = NextSerial++;
	return TWoTBootstrapResult<FWoTFrontEndSceneStaging>::Success(
		FWoTFrontEndSceneStaging(OwnerState, StagedSerial));
}

int32 FWoTFrontEndSceneMaterializer::ResolveConverterSurfaceSlotForTesting(
	FStringView MaterialSlot,
	TConstArrayView<FString> AuthenticatedSourceExports)
{
	return SceneResolveConverterSlot(MaterialSlot, AuthenticatedSourceExports);
}

FWoTFrontEndSceneObservation
FWoTFrontEndSceneMaterializer::ObserveForTesting() const
{
	FWoTFrontEndSceneObservation Seen;
	if (Plan.IsValid()) Seen.SnapshotIdentity = Plan->SnapshotIdentity;
	if (UWorld* World = ObservationWorld.Get())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Actor = *It;
			if (Actor == nullptr
				|| !Actor->ActorHasTag(TEXT("WoTAuthenticatedFrontEndScene"))) continue;
			if (Actor->IsActorBeingDestroyed()) ++Seen.PendingDestroyActors;
			if (!Actor->IsHidden()) ++Seen.ObservableActors;
			TInlineComponentArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (const UActorComponent* Component : Components)
			{
				if (Component != nullptr && Component->HasAnyFlags(
					RF_BeginDestroyed | RF_FinishDestroyed))
					++Seen.PendingDestroyComponents;
				if (Component != nullptr && Component->IsRegistered())
					++Seen.RegisteredComponents;
				const USceneComponent* Scene = Cast<USceneComponent>(Component);
				if (!Actor->IsHidden() && Component != nullptr && Component->IsRegistered()
					&& (Scene == nullptr || Scene->IsVisible())) ++Seen.VisibleComponents;
				if (const UPrimitiveComponent* Primitive =
					Cast<UPrimitiveComponent>(Component))
					Seen.CollidingComponents += Primitive->GetCollisionEnabled()
						!= ECollisionEnabled::NoCollision ? 1u : 0u;
			}
		}
	}
	if (!Plan.IsValid() || !IsCommitted()) return Seen;
	Seen.TickingActors = Plan->Controller.IsValid()
		&& Plan->Controller->IsRunning() ? 1u : 0u;
	for (const TWeakObjectPtr<AActor>& Weak : ActiveWeatherActors)
	{
		const AActor* Actor = Weak.Get();
		Seen.TickingActors += Actor != nullptr && Actor->IsActorTickEnabled()
			? 1u : 0u;
	}
	Seen.AudibleComponents = Plan->Controller.IsValid()
		? Plan->Controller->AudibleCount() : 0u;
	Seen.PlayingAmbientLoops = Seen.AudibleComponents;
	if (AActor* Root = ActiveRoot.Get())
	{
		TInlineComponentArray<UAudioComponent*> Audio;
		Root->GetComponents(Audio);
		for (const UAudioComponent* Component : Audio)
			if (Component != nullptr && Component->Sound != nullptr
				&& Component->Sound->IsLooping())
			{
				Seen.FirstAmbientVolume = Component->VolumeMultiplier;
				break;
			}
	}
	Seen.AnimatedWeatherComponents = Plan->Controller.IsValid()
		? Plan->Controller->AnimatedWeatherCount(
			Plan->Census.SplashEmitters + Plan->Census.RainCylinders
			+ Plan->Census.LightningActors + Plan->Census.WaterSurfaces)
		: 0u;
	Seen.LightningEndpointCount = Plan->Controller.IsValid()
		? Plan->Controller->ThunderEndpointCount() : 0u;
	for (const FPlan::FThunderEndpoint& Endpoint : Plan->ThunderEndpoints)
		Seen.PreservedLightningEndpointProperties += !Endpoint.Tag.IsEmpty()
			&& !Endpoint.LightTag.IsEmpty() && Endpoint.Radius > 0.0f
			&& Endpoint.Volume > 0.0f ? 1u : 0u;
	Seen.MinLightningSeconds = Plan->Controller.IsValid()
		? Plan->Controller->MinStrikeSeconds() : Plan->MinLightningTime;
	Seen.MaxLightningSeconds = Plan->Controller.IsValid()
		? Plan->Controller->MaxStrikeSeconds() : Plan->MaxLightningTime;
	Seen.NextLightningSeconds = Plan->Controller.IsValid()
		? Plan->Controller->NextStrikeSeconds() : 0.0f;
	Seen.NearThunderSoundIndex = Plan->Controller.IsValid()
		? Plan->Controller->ThunderSoundIndexForDelay(Plan->MinThunderDelay)
		: INDEX_NONE;
	Seen.FarThunderSoundIndex = Plan->Controller.IsValid()
		? Plan->Controller->ThunderSoundIndexForDelay(Plan->MaxThunderDelay)
		: INDEX_NONE;
	if (Plan->ThunderSoundIdentities.IsValidIndex(Seen.NearThunderSoundIndex))
		Seen.NearThunderSoundIdentity =
			Plan->ThunderSoundIdentities[Seen.NearThunderSoundIndex];
	if (Plan->ThunderSoundIdentities.IsValidIndex(Seen.FarThunderSoundIndex))
		Seen.FarThunderSoundIdentity =
			Plan->ThunderSoundIdentities[Seen.FarThunderSoundIndex];
	if (AActor* Root = ActiveRoot.Get())
	{
		TInlineComponentArray<UStaticMeshComponent*> Meshes;
		Root->GetComponents(Meshes);
		for (const UStaticMeshComponent* Mesh : Meshes)
		{
			if (Mesh == nullptr || Mesh->GetFName() != TEXT("AuthenticatedBSPMesh")
				|| !Mesh->IsRegistered() || !Mesh->IsVisible()
				|| Mesh->GetStaticMesh() == nullptr) continue;
			++Seen.AuthenticatedBSPRenderers;
			for (int32 Index = 0; Index < Plan->SurfaceMaterials.Num(); ++Index)
			{
				UMaterialInterface* Bound = Mesh->GetMaterial(Index);
				if (Bound == nullptr
					|| Bound != Plan->SurfaceMaterials[Index].Get()) continue;
				++Seen.BoundBSPMaterialSections;
				Seen.BoundWaterMaterials += Plan->WaterMaterials.ContainsByPredicate(
					[Bound](const auto& Water) { return Water.Get() == Bound; }) ? 1u : 0u;
			}
		}
	}
	for (const TWeakObjectPtr<AActor>& Weak : ActiveWeatherActors)
	{
		if (const AWoTRainCyd* RainActor = Cast<AWoTRainCyd>(Weak.Get()))
		{
			Seen.AuthenticatedRainRenderers += RainActor->HasRuntimeRenderMesh()
				&& RainActor->RuntimeRenderMaterialCount()
						== Plan->RainSectionSkinKeys.Num() ? 1u : 0u;
			if (Seen.FirstRainFallRate == 0.0f)
			{
				Seen.FirstRainInitialYaw = RainActor->InitialYawForTesting();
				Seen.FirstRainFallRate = RainActor->FallRateForTesting();
			}
		}
		if (const AWoTParticleSprayer* Splash =
			Cast<AWoTParticleSprayer>(Weak.Get()))
			Seen.AuthoredSplashRenderers += !Splash->Templates.IsEmpty()
				&& Splash->Templates.Num() == Splash->AddMaterials.Num()
				&& Splash->Templates.Num() == Splash->ModMaterials.Num() ? 1u : 0u;
	}
	Seen.CameraLocation = Plan->Descriptor.CameraLocation;
	Seen.CameraRotation = Plan->Descriptor.CameraRotation;
	Seen.HorizontalFov = Plan->Descriptor.HorizontalFov;
	Seen.bMaintainHorizontalFov = true;
	Seen.bConstrainAspectRatio = false;
	return Seen;
}

void FWoTFrontEndSceneMaterializer::FailNextCommitForTesting()
{
	GFailNextFrontEndSceneCommit = true;
}

TSharedPtr<FWoTFrontEndSceneController>
FWoTFrontEndSceneMaterializer::ControllerForTesting() const
{
	return Plan ? Plan->Controller : nullptr;
}

void FWoTFrontEndSceneMaterializer::SetAtlasUploaderForTesting(
	FWoTEntryAtlasUploader Uploader)
{
	TestAtlasUploader = MoveTemp(Uploader);
}

UTexture2D* FWoTFrontEndSceneMaterializer::GenericTextureForTesting(FStringView SemanticIdentity) const
{
	if (!Plan) return nullptr;
	const auto* Texture = Plan->TexturesBySemanticKey.Find(SceneIdentityKey(SemanticIdentity));
	return Texture ? Texture->Get() : nullptr;
}

void FWoTFrontEndSceneMaterializer::FailAfterHiddenConstructionForTesting()
{
	GFailAfterFrontEndSceneConstruction = true;
}

void FWoTFrontEndSceneMaterializer::FailAfterControllerPreparationForTesting()
{
	GFailAfterFrontEndSceneControllerPreparation = true;
}

void FWoTFrontEndSceneMaterializer::CancelNextCommitForTesting()
{
	GCancelNextFrontEndSceneCommit = true;
}

void FWoTFrontEndSceneMaterializer::RevokeAfterHiddenConstructionForTesting()
{
	GRevokeAfterFrontEndSceneConstruction = true;
}

void FWoTFrontEndSceneMaterializer::RevokeAfterControllerPreparationForTesting()
{
	GRevokeAfterFrontEndSceneControllerPreparation = true;
}

void FWoTFrontEndSceneMaterializer::RevokeDuringPublicationForTesting()
{
	GRevokeDuringFrontEndScenePublication = true;
}

void FWoTFrontEndSceneMaterializer::SetPublicationBoundaryObserverForTesting(
	TFunction<void(const TCHAR*, const FWoTFrontEndSceneObservation&)> Observer)
{
	GFrontEndScenePublicationObserver = MoveTemp(Observer);
}

void FWoTFrontEndSceneMaterializer::SetResourceStageObserverForTesting(
	TFunction<void(EWoTFactDomain, int32)> Observer)
{
	GFrontEndSceneResourceStageObserver = MoveTemp(Observer);
}

bool FWoTFrontEndSceneMaterializer::IsPublicationGuardActiveForTesting()
{
	return GFrontEndScenePublicationGuardDepth > 0;
}

void FWoTFrontEndSceneMaterializer::RevokeAuthorityForTesting()
{
	if (Plan.IsValid() && Plan->PinnedWorldLease.IsValid())
	{
		FWoTCacheGenerationLease Revoker(*Plan->PinnedWorldLease);
		Revoker.Revoke();
	}
}

void FWoTFrontEndSceneMaterializer::RevokeLeaseForTesting(
	const FWoTCacheGenerationLease& Lease)
{
	FWoTCacheGenerationLease Revoker(Lease);
	Revoker.Revoke();
}

FWoTBootstrapStatus FWoTFrontEndSceneMaterializer::RematerializeForTesting(
	UWorld& World)
{
	return Rematerialize(World);
}
#endif
