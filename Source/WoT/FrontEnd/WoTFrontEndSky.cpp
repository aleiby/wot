#include "WoTFrontEndSky.h"

#include "../Payload/WoTRuntimeWorldResource.h"
#include "../World/WoTRuntimeMapSky.h"

namespace
{
constexpr uint32 EntryPF_Invisible = 0x00000001u;
constexpr uint32 EntryPF_Masked = 0x00000002u;
constexpr uint32 EntryPF_Translucent = 0x00000004u;
constexpr uint32 EntryPF_Modulated = 0x00000040u;
constexpr uint32 EntryPF_FakeBackdrop = 0x00000080u;
constexpr uint32 EntryPF_TwoSided = 0x00000100u;
constexpr uint32 EntryPF_Unlit = 0x00400000u;
constexpr uint32 RendererMaxVertices = 2000000;
constexpr uint32 RendererMaxIndices = 6000000;
constexpr uint32 RendererMaxSections = 4096;

const FWoTRuntimeWorldProperty* FindProperty(
	const FWoTRuntimeWorldActor& Actor, FStringView Name)
{
	for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
	{
		if (Property.ArrayIndex == 0
			&& FStringView(Property.Name).Equals(Name, ESearchCase::IgnoreCase))
		{
			return &Property;
		}
	}
	return nullptr;
}

bool ReadVector(const FWoTRuntimeWorldProperty& Property, FVector& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase)
		|| Property.Value.Num() != 3 * sizeof(float))
	{
		return false;
	}
	float Values[3];
	FMemory::Memcpy(Values, Property.Value.GetData(), sizeof(Values));
	if (!FMath::IsFinite(Values[0]) || !FMath::IsFinite(Values[1])
		|| !FMath::IsFinite(Values[2]))
	{
		return false;
	}
	Out = FVector(Values[0], Values[1], Values[2]);
	return true;
}

bool ReadRotator(const FWoTRuntimeWorldProperty& Property, FRotator& Out)
{
	if (Property.Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property.StructName.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase)
		|| Property.Value.Num() != 3 * sizeof(int32))
	{
		return false;
	}
	int32 Values[3];
	FMemory::Memcpy(Values, Property.Value.GetData(), sizeof(Values));
	constexpr double UnitsToDegrees = 360.0 / 65536.0;
	Out = FRotator(Values[0] * UnitsToDegrees,
		Values[1] * UnitsToDegrees, Values[2] * UnitsToDegrees);
	return true;
}

struct FSkySelection
{
	const FWoTRuntimeWorldActor* Actor = nullptr;
	uint8 Zone = 0;
	TSet<int32> Surfaces;
};

// The retail rule (skybake zone_sky_links + pick_winner, skygeo sky_zone_of +
// sky_surface_ids) is shared with the map-world sky (WoTRuntimeMapSky.cpp);
// the entry scene adds its stricter single-scene policy on top.
bool SelectSky(const FWoTRuntimeWorldResource& World,
	FSkySelection& Out, FString& Error)
{
	FWoTRetailSkyWorldView View;
	View.Actors = World.Actors();
	View.Zones = World.Zones();
	View.Nodes = World.Nodes();
	View.Surfaces = World.Surfaces();
	FWoTRetailSkySelection Selection;
	if (!WoTSelectRetailSky(View, Selection, Error)) return false;
	const FString PolicyError = WoTEntrySkySelectionPolicyError(Selection);
	if (!PolicyError.IsEmpty())
	{
		Error = PolicyError;
		return false;
	}
	Out.Actor = &World.Actors()[Selection.WinnerActor];
	Out.Zone = static_cast<uint8>(Selection.Zone);
	Out.Surfaces = MoveTemp(Selection.Surfaces);
	return true;
}

bool IsFinite(const FVector3f& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
		&& FMath::IsFinite(Value.Z);
}

bool IsFinite(const FVector2f& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
}

double PointTriangleDistance(const FVector3d& Point, const FVector3d& A,
	const FVector3d& B, const FVector3d& C)
{
	return WoTRuntimeMapPointTriangleDistance(Point, A, B, C);
}

EWoTEntryBlend SurfaceBlend(uint32 Flags)
{
	if ((Flags & EntryPF_Translucent) != 0) return EWoTEntryBlend::Translucent;
	if ((Flags & EntryPF_Modulated) != 0) return EWoTEntryBlend::Modulated;
	if ((Flags & EntryPF_Masked) != 0) return EWoTEntryBlend::Masked;
	return EWoTEntryBlend::Opaque;
}

struct FTriangleSource
{
	int32 Triangle = INDEX_NONE;
	int32 Section = INDEX_NONE;
	uint32 Surface = 0;
	uint32 Flags = 0;
	FVector2f PanUV = FVector2f::ZeroVector;
	bool bSky = false;
};

struct FSectionGroup
{
	int32 SourceSection = INDEX_NONE;
	uint32 Flags = 0;
	FVector2f PanUV = FVector2f::ZeroVector;
	TArray<const FTriangleSource*> Triangles;
};

uint32 FloatEqualityBits(float Value)
{
	if (Value == 0.0f) Value = 0.0f;
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

struct FSectionGroupKey
{
	int32 SourceSection = INDEX_NONE;
	uint32 Flags = 0;
	FVector2f PanUV = FVector2f::ZeroVector;

	bool operator==(const FSectionGroupKey& Other) const
	{
		return SourceSection == Other.SourceSection && Flags == Other.Flags
			&& PanUV == Other.PanUV;
	}

	friend uint32 GetTypeHash(const FSectionGroupKey& Key)
	{
		uint32 Hash = HashCombineFast(GetTypeHash(Key.SourceSection),
			GetTypeHash(Key.Flags));
		Hash = HashCombineFast(Hash, GetTypeHash(FloatEqualityBits(Key.PanUV.X)));
		return HashCombineFast(Hash,
			GetTypeHash(FloatEqualityBits(Key.PanUV.Y)));
	}
};

bool PreflightPartition(const TArray<FTriangleSource>& Triangles, bool bSky,
	TArray<FSectionGroup>& Groups, FString& Error)
{
	Groups.Reset();
	TMap<FSectionGroupKey, int32> GroupIndices;
	uint64 ExpandedVertexCount = 0;
	uint64 ExpandedIndexCount = 0;
	for (const FTriangleSource& Triangle : Triangles)
	{
		if (Triangle.bSky != bSky) continue;
		ExpandedVertexCount += 3;
		ExpandedIndexCount += 3;
		if (ExpandedVertexCount > RendererMaxVertices)
		{
			Error = TEXT("front-end expanded vertex count exceeds renderer limit");
			return false;
		}
		if (ExpandedIndexCount > RendererMaxIndices)
		{
			Error = TEXT("front-end expanded index count exceeds renderer limit");
			return false;
		}

		const FSectionGroupKey Key = {
			Triangle.Section, Triangle.Flags, Triangle.PanUV};
		int32* GroupIndex = GroupIndices.Find(Key);
		if (GroupIndex == nullptr)
		{
			if (static_cast<uint32>(Groups.Num()) >= RendererMaxSections)
			{
				Error = TEXT("front-end derived section count exceeds renderer limit");
				return false;
			}
			const int32 NewIndex = Groups.AddDefaulted();
			FSectionGroup& Group = Groups[NewIndex];
			Group.SourceSection = Triangle.Section;
			Group.Flags = Triangle.Flags;
			Group.PanUV = Triangle.PanUV;
			GroupIndices.Add(Key, NewIndex);
			GroupIndex = GroupIndices.Find(Key);
		}
		Groups[*GroupIndex].Triangles.Add(&Triangle);
	}
	if (Groups.IsEmpty())
	{
		Error = bSky ? TEXT("sky extraction produced no visible triangles")
			: TEXT("world extraction produced no visible triangles");
		return false;
	}
	return true;
}

void CopySharedPayload(const FWoTBSPPayloadData& Source,
	FWoTBSPPayloadData& Target)
{
	Target.FormatVersion = Source.FormatVersion;
	Target.bPortal = Source.bPortal;
	Target.bRootOutside = Source.bRootOutside;
	Target.LightmapWidth = Source.LightmapWidth;
	Target.LightmapHeight = Source.LightmapHeight;
	Target.LightmapBGRA = Source.LightmapBGRA;
	Target.EntrySurfaces = Source.EntrySurfaces;
	Target.EntryLights = Source.EntryLights;
	Target.EntryIllumination = Source.EntryIllumination;
}

bool BuildPartition(const FWoTRuntimeWorldResource& World,
	const FWoTBSPPayloadData& Source,
	const TArray<FSectionGroup>& Groups, bool bSky,
	const FVector& Origin, FWoTBSPPayloadData& Target,
	TArray<FWoTEntrySectionStyle>& Styles, FString& Error)
{
	CopySharedPayload(Source, Target);
	for (const FSectionGroup& Group : Groups)
	{
		const FWoTBSPPayloadSection& SourceSection =
			Source.Sections[Group.SourceSection];
		FWoTBSPPayloadSection& Section = Target.Sections.AddDefaulted_GetRef();
		Section.MaterialSlot = SourceSection.MaterialSlot;
		Section.FirstIndex = static_cast<uint32>(Target.Indices.Num());
		Section.MinVertex = static_cast<uint32>(Target.Positions.Num());
		Section.bCollisionEnabled = bSky ? false : SourceSection.bCollisionEnabled;
		FWoTEntrySectionStyle& Style = Styles.AddDefaulted_GetRef();
		Style.Blend = SurfaceBlend(Group.Flags);
		Style.PanUV = Group.PanUV;
		Style.bUnlit = (Group.Flags & EntryPF_Unlit) != 0;
		Style.bTwoSided = (Group.Flags & EntryPF_TwoSided) != 0;

		for (const FTriangleSource* Triangle : Group.Triangles)
		{
			const FWoTRuntimeWorldSurface& Surface =
				World.Surfaces()[Triangle->Surface];
			if (Style.TextureIdentity.IsEmpty()) Style.TextureIdentity =
				Surface.TextureIdentity;
			else if (!Style.TextureIdentity.Equals(
					Surface.TextureIdentity, ESearchCase::CaseSensitive))
			{
				Error = TEXT("one sky section group has multiple texture identities");
				return false;
			}
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 SourceVertex =
					Source.Indices[Triangle->Triangle * 3 + Corner];
				FVector3f Position = Source.Positions[SourceVertex];
				if (bSky) Position -= FVector3f(Origin);
				Target.Positions.Add(Position);
				Target.Normals.Add(Source.Normals[SourceVertex]);
				Target.UV0.Add(Source.UV0[SourceVertex]);
				Target.UV1.Add(Source.UV1[SourceVertex]);
				Target.Indices.Add(Target.Indices.Num());
			}
			Target.TriangleSurfaces.Add(Triangle->Surface);
		}
		Section.IndexCount = static_cast<uint32>(Target.Indices.Num())
			- Section.FirstIndex;
		Section.MaxVertex = static_cast<uint32>(Target.Positions.Num() - 1);
	}
	Target.BoundsMin = Target.Positions[0];
	Target.BoundsMax = Target.Positions[0];
	for (const FVector3f& Position : Target.Positions)
	{
		Target.BoundsMin.X = FMath::Min(Target.BoundsMin.X, Position.X);
		Target.BoundsMin.Y = FMath::Min(Target.BoundsMin.Y, Position.Y);
		Target.BoundsMin.Z = FMath::Min(Target.BoundsMin.Z, Position.Z);
		Target.BoundsMax.X = FMath::Max(Target.BoundsMax.X, Position.X);
		Target.BoundsMax.Y = FMath::Max(Target.BoundsMax.Y, Position.Y);
		Target.BoundsMax.Z = FMath::Max(Target.BoundsMax.Z, Position.Z);
	}
	return true;
}
}

bool WoTBuildFrontEndSkyPlan(
	const FWoTRuntimeWorldResource& World,
	const FWoTBSPPayloadData& BSP,
	FWoTFrontEndSkyPlan& Out,
	FString& Error)
{
	Out = FWoTFrontEndSkyPlan();
	Error.Reset();
	auto Fail = [&Out, &Error](const TCHAR* Message)
	{
		Out = FWoTFrontEndSkyPlan();
		Error = Message;
		return false;
	};
	if (BSP.FormatVersion != 3 && BSP.FormatVersion != 4 && BSP.FormatVersion != 5)
		return Fail(TEXT("front-end sky extraction requires BSP payload v3, v4 or v5"));
	const int32 VertexCount = BSP.Positions.Num();
	if (VertexCount <= 0 || BSP.Normals.Num() != VertexCount
		|| BSP.UV0.Num() != VertexCount || BSP.UV1.Num() != VertexCount
		|| BSP.Indices.IsEmpty() || BSP.Indices.Num() % 3 != 0
		|| BSP.TriangleSurfaces.Num() != BSP.Indices.Num() / 3
		|| BSP.EntrySurfaces.Num() != World.Surfaces().Num())
	{
		return Fail(TEXT("front-end BSP geometry/provenance arrays are not dense"));
	}
	for (int32 Index = 0; Index < VertexCount; ++Index)
	{
		if (!IsFinite(BSP.Positions[Index]) || !IsFinite(BSP.Normals[Index])
			|| !IsFinite(BSP.UV0[Index]) || !IsFinite(BSP.UV1[Index]))
		{
			return Fail(TEXT("front-end BSP geometry contains nonfinite values"));
		}
	}
	for (int32 SurfaceIndex = 0; SurfaceIndex < BSP.EntrySurfaces.Num(); ++SurfaceIndex)
	{
		const FWoTEntrySurfaceRecord& Surface = BSP.EntrySurfaces[SurfaceIndex];
		if (Surface.SurfaceIndex != static_cast<uint32>(SurfaceIndex)
			|| !IsFinite(Surface.PanUV))
		{
			return Fail(TEXT("front-end BSP surface provenance is not dense or finite"));
		}
	}

	TArray<int32> TriangleSections;
	TriangleSections.Init(INDEX_NONE, BSP.Indices.Num() / 3);
	uint32 ExpectedFirstIndex = 0;
	for (int32 SectionIndex = 0; SectionIndex < BSP.Sections.Num(); ++SectionIndex)
	{
		const FWoTBSPPayloadSection& Section = BSP.Sections[SectionIndex];
		if (Section.FirstIndex != ExpectedFirstIndex || Section.IndexCount == 0
			|| Section.FirstIndex % 3 != 0 || Section.IndexCount % 3 != 0
			|| Section.FirstIndex + Section.IndexCount
				> static_cast<uint32>(BSP.Indices.Num()))
		{
			return Fail(TEXT("front-end BSP sections are not contiguous triangle ranges"));
		}
		for (uint32 Index = Section.FirstIndex;
			Index < Section.FirstIndex + Section.IndexCount; Index += 3)
		{
			TriangleSections[Index / 3] = SectionIndex;
		}
		ExpectedFirstIndex += Section.IndexCount;
	}
	if (ExpectedFirstIndex != static_cast<uint32>(BSP.Indices.Num())
		|| TriangleSections.Contains(INDEX_NONE))
	{
		return Fail(TEXT("front-end BSP sections do not cover every triangle"));
	}

	FSkySelection Selection;
	if (!SelectSky(World, Selection, Error)) return false;
	FVector Origin = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	if (const FWoTRuntimeWorldProperty* Property =
		FindProperty(*Selection.Actor, TEXT("Location")))
	{
		if (!ReadVector(*Property, Origin))
			return Fail(TEXT("winning SkyZoneInfo Location is malformed"));
	}
	if (const FWoTRuntimeWorldProperty* Property =
		FindProperty(*Selection.Actor, TEXT("Rotation")))
	{
		if (!ReadRotator(*Property, Rotation))
			return Fail(TEXT("winning SkyZoneInfo Rotation is malformed"));
	}

	TArray<FTriangleSource> Triangles;
	Triangles.Reserve(BSP.Indices.Num() / 3);
	double MinFaceDistance = TNumericLimits<double>::Max();
	for (int32 TriangleIndex = 0;
		TriangleIndex < BSP.Indices.Num() / 3; ++TriangleIndex)
	{
		const uint32 SurfaceIndex = BSP.TriangleSurfaces[TriangleIndex];
		if (!World.Surfaces().IsValidIndex(static_cast<int32>(SurfaceIndex))
			|| !BSP.EntrySurfaces.IsValidIndex(static_cast<int32>(SurfaceIndex)))
		{
			return Fail(TEXT("front-end BSP triangle surface provenance is out of range"));
		}
		uint32 VertexIndices[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			VertexIndices[Corner] = BSP.Indices[TriangleIndex * 3 + Corner];
			if (!BSP.Positions.IsValidIndex(static_cast<int32>(VertexIndices[Corner])))
				return Fail(TEXT("front-end BSP index is out of range"));
		}
		const FVector3d A(BSP.Positions[VertexIndices[0]]);
		const FVector3d B(BSP.Positions[VertexIndices[1]]);
		const FVector3d C(BSP.Positions[VertexIndices[2]]);
		const double AreaSquared = FVector3d::CrossProduct(B - A, C - A).SquaredLength();
		if (!FMath::IsFinite(AreaSquared))
			return Fail(TEXT("front-end BSP triangle area is nonfinite"));
		if (AreaSquared <= UE_DOUBLE_SMALL_NUMBER) continue;

		const uint32 Flags = World.Surfaces()[SurfaceIndex].Flags;
		if ((Flags & (EntryPF_Invisible | EntryPF_FakeBackdrop)) != 0) continue;
		FTriangleSource& Triangle = Triangles.AddDefaulted_GetRef();
		Triangle.Triangle = TriangleIndex;
		Triangle.Section = TriangleSections[TriangleIndex];
		Triangle.Surface = SurfaceIndex;
		Triangle.Flags = Flags;
		Triangle.PanUV = BSP.EntrySurfaces[SurfaceIndex].PanUV;
		Triangle.bSky = Selection.Surfaces.Contains(static_cast<int32>(SurfaceIndex));
		if (Triangle.bSky)
		{
			const double Distance = PointTriangleDistance(
				FVector3d(Origin), A, B, C);
			if (!FMath::IsFinite(Distance))
				return Fail(TEXT("front-end sky face distance is nonfinite"));
			MinFaceDistance = FMath::Min(MinFaceDistance, Distance);
		}
	}

	FWoTFrontEndSkyPlan Plan;
	Plan.Origin = Origin;
	Plan.Rotation = Rotation;
	TArray<FSectionGroup> WorldGroups;
	TArray<FSectionGroup> SkyGroups;
	if (!PreflightPartition(Triangles, false, WorldGroups, Error)
		|| !PreflightPartition(Triangles, true, SkyGroups, Error))
	{
		return false;
	}
	if (!BuildPartition(World, BSP, WorldGroups, false, Origin,
			Plan.WorldMesh, Plan.WorldStyles, Error)
		|| !BuildPartition(World, BSP, SkyGroups, true, Origin,
			Plan.SkyMesh, Plan.SkyStyles, Error))
	{
		return false;
	}
	if (MinFaceDistance < 0.001
		|| MinFaceDistance == TNumericLimits<double>::Max())
	{
		return Fail(TEXT("front-end sky shell is empty or too close to its origin"));
	}
	const FVector3f WorldExtent =
		(Plan.WorldMesh.BoundsMax - Plan.WorldMesh.BoundsMin) * 0.5f;
	const float WorldRadius = WorldExtent.Length();
	Plan.Scale = WoTEntrySkyScale(WorldRadius,
		static_cast<float>(MinFaceDistance));
	if (!FMath::IsFinite(Plan.Scale) || Plan.Scale <= 0.0f
		|| Plan.Scale > 1000000.0f)
	{
		return Fail(TEXT("front-end sky scale is invalid or over bound"));
	}
	Out = MoveTemp(Plan);
	return true;
}

FString WoTEntrySkySelectionPolicyError(const FWoTRetailSkySelection& Selection)
{
	// The checks and their order are the pre-sharing entry SelectSky: a missing
	// sky, a winner named by several zones, a zero or out-of-uint8-range zone
	// (a node addresses zones as uint8), then a portal out of the sky zone.
	if (Selection.SkyActors.IsEmpty())
		return TEXT("front-end world has no SkyZoneInfo");
	if (Selection.ZoneMatchCount > 1)
		return TEXT("winning SkyZoneInfo appears in multiple zones");
	if (!Selection.HasSkyZone())
		return TEXT("winning SkyZoneInfo has no nonzero BSP zone");
	if (Selection.bTraversablePortal)
		return TEXT("winning sky zone has a traversable nonzero-zone portal");
	return FString();
}

float WoTEntrySkyScale(float WorldRadius, float MinFaceDistance)
{
	if (!FMath::IsFinite(WorldRadius) || !FMath::IsFinite(MinFaceDistance)
		|| WorldRadius <= 0.0f || MinFaceDistance <= 0.0f)
	{
		return 0.0f;
	}
	const float Scale = 2.0f * WorldRadius / MinFaceDistance * 1.25f;
	if (!FMath::IsFinite(Scale)) return 0.0f;
	return FMath::Max(12.0f, FMath::CeilToFloat(Scale));
}
