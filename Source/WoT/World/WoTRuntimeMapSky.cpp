#include "WoTRuntimeMapSky.h"

#include "Algo/Sort.h"
#include "Algo/StableSort.h"
#include "WoTRuntimeWorldProperties.h"

namespace
{
// Leaf of a world-v3 class identity: the text after the last dot, exactly the
// entry SkyPlan rule this was shared from. World-v3 actor classes are plain
// Package.Class paths (BSPWorld.cpp WoTWorldObjectClassIdentity); the numeric
// `#export-N` disambiguator only occurs in class-default records.
FStringView WoTSkyClassLeaf(FStringView Identity)
{
	int32 Dot = INDEX_NONE;
	return Identity.FindLastChar(TEXT('.'), Dot) ? Identity.RightChop(Dot + 1) : Identity;
}

// skybake.WOT_ZONE_CLASSES: WOTZoneInfo.uc:131-141 overrides LinkToSkybox.
bool WoTSkyIsWotZoneClass(FStringView ClassIdentity)
{
	const FStringView Leaf = WoTSkyClassLeaf(ClassIdentity);
	return Leaf.Equals(TEXT("WOTZoneInfo"), ESearchCase::IgnoreCase)
		|| Leaf.Equals(TEXT("WaterZone"), ESearchCase::IgnoreCase)
		|| Leaf.Equals(TEXT("WaysZone"), ESearchCase::IgnoreCase);
}

// Absent -> empty (T3D omits a default None name); present must be a name.
bool WoTSkyReadName(const FWoTRuntimeWorldProperty* Property, FString& Out)
{
	if (Property == nullptr)
	{
		Out.Reset();
		return true;
	}
	if (Property->Type != EWoTRuntimeWorldPropertyType::Name
		|| Property->NameValue.IsEmpty())
	{
		return false;
	}
	Out = Property->NameValue;
	return true;
}

// A bsp2obj material slot is (canonical texture, flag_suffix family). The
// canonical texture is a function of the surface's source texture export, so
// (source identity, family bits) addresses exactly one slot. World-v3 names a
// NULL texture with an empty identity; the BSP-v4 writer names it "None".
FString WoTSkySlotKey(FStringView SourceIdentity, uint32 Flags)
{
	const bool bNull = SourceIdentity.IsEmpty()
		|| SourceIdentity.Equals(TEXT("None"), ESearchCase::CaseSensitive);
	return FString::Printf(TEXT("%s|%08x"),
		bNull ? TEXT("<null>") : *FString(SourceIdentity).ToLower(),
		Flags & WoTRuntimeMapSlotFamilyMask);
}

struct FWoTSkySlotGroup
{
	FString Slot;
	TArray<FWoTRuntimeMapSectionRef> Sources;
};
}

TArray<int32> WoTRuntimeMapSkyLayerOrder(const FWoTBSPPayloadData& SkyMesh,
	TConstArrayView<bool> LayerSections)
{
	struct FLayer
	{
		int32 Section;
		double Distance;
	};
	TArray<FLayer> Layers;
	const int32 SectionCount = FMath::Min(SkyMesh.Sections.Num(), LayerSections.Num());
	for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
	{
		if (!LayerSections[SectionIndex]) continue;
		const FWoTBSPPayloadSection& Section = SkyMesh.Sections[SectionIndex];
		const uint64 End = FMath::Min(
			static_cast<uint64>(Section.FirstIndex) + Section.IndexCount,
			static_cast<uint64>(SkyMesh.Indices.Num()));
		double Nearest = TNumericLimits<double>::Max();
		for (uint64 Corner = Section.FirstIndex; Corner + 2 < End; Corner += 3)
		{
			FVector3d Points[3];
			bool bValid = true;
			for (int32 Offset = 0; Offset < 3; ++Offset)
			{
				const int32 Vertex = static_cast<int32>(
					SkyMesh.Indices[static_cast<int32>(Corner) + Offset]);
				bValid &= SkyMesh.Positions.IsValidIndex(Vertex);
				if (bValid) Points[Offset] = FVector3d(SkyMesh.Positions[Vertex]);
			}
			if (!bValid) continue;
			Nearest = FMath::Min(Nearest, WoTRuntimeMapPointTriangleDistance(
				FVector3d::ZeroVector, Points[0], Points[1], Points[2]));
		}
		Layers.Add({SectionIndex, Nearest});
	}
	Algo::StableSort(Layers, [](const FLayer& A, const FLayer& B)
	{
		return A.Distance > B.Distance;
	});
	TArray<int32> Order;
	Order.Reserve(Layers.Num());
	for (const FLayer& Layer : Layers) Order.Add(Layer.Section);
	return Order;
}

bool WoTRuntimeMapSkySubMesh(const FWoTBSPPayloadData& Mesh,
	TConstArrayView<int32> Sections, FWoTBSPPayloadData& Out)
{
	const bool bSurfaces = !Mesh.TriangleSurfaces.IsEmpty()
		&& Mesh.TriangleSurfaces.Num() * 3 == Mesh.Indices.Num();
	Out = Mesh;
	Out.Indices.Reset();
	Out.Sections.Reset();
	Out.TriangleSurfaces.Reset();
	for (const int32 SectionIndex : Sections)
	{
		if (!Mesh.Sections.IsValidIndex(SectionIndex)) return false;
		FWoTBSPPayloadSection Section = Mesh.Sections[SectionIndex];
		const uint64 End = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
		if (End > static_cast<uint64>(Mesh.Indices.Num()) || Section.IndexCount % 3 != 0)
			return false;
		const int32 First = static_cast<int32>(Section.FirstIndex);
		const int32 Count = static_cast<int32>(Section.IndexCount);
		Section.FirstIndex = static_cast<uint32>(Out.Indices.Num());
		Out.Indices.Append(Mesh.Indices.GetData() + First, Count);
		if (bSurfaces)
			Out.TriangleSurfaces.Append(Mesh.TriangleSurfaces.GetData() + First / 3, Count / 3);
		Out.Sections.Add(MoveTemp(Section));
	}
	return true;
}

double WoTRuntimeMapPointTriangleDistance(const FVector3d& Point,
	const FVector3d& A, const FVector3d& B, const FVector3d& C)
{
	// Ericson, Real-Time Collision Detection 5.1.5 (skygeo.tri_dist).
	const FVector3d AB = B - A;
	const FVector3d AC = C - A;
	const FVector3d AP = Point - A;
	const double D1 = AB.Dot(AP);
	const double D2 = AC.Dot(AP);
	if (D1 <= 0.0 && D2 <= 0.0) return AP.Length();

	const FVector3d BP = Point - B;
	const double D3 = AB.Dot(BP);
	const double D4 = AC.Dot(BP);
	if (D3 >= 0.0 && D4 <= D3) return BP.Length();

	const double VC = D1 * D4 - D3 * D2;
	if (VC <= 0.0 && D1 >= 0.0 && D3 <= 0.0)
		return (AP - AB * (D1 / (D1 - D3))).Length();

	const FVector3d CP = Point - C;
	const double D5 = AB.Dot(CP);
	const double D6 = AC.Dot(CP);
	if (D6 >= 0.0 && D5 <= D6) return CP.Length();

	const double VB = D5 * D2 - D1 * D6;
	if (VB <= 0.0 && D2 >= 0.0 && D6 <= 0.0)
		return (AP - AC * (D2 / (D2 - D6))).Length();

	const double VA = D3 * D6 - D5 * D4;
	if (VA <= 0.0 && D4 - D3 >= 0.0 && D5 - D6 >= 0.0)
	{
		const double W = (D4 - D3) / ((D4 - D3) + (D5 - D6));
		return (Point - (B + W * (C - B))).Length();
	}

	const double Denominator = VA + VB + VC;
	if (!FMath::IsFinite(Denominator) || FMath::IsNearlyZero(Denominator))
		return TNumericLimits<double>::Max();
	const double V = VB / Denominator;
	const double W = VC / Denominator;
	return (Point - (A + AB * V + AC * W)).Length();
}

bool WoTSelectRetailSky(const FWoTRetailSkyWorldView& View,
	FWoTRetailSkySelection& Out, FString& Error)
{
	Out = FWoTRetailSkySelection();
	TMap<FString, int32> ActorsByIdentity;
	ActorsByIdentity.Reserve(View.Actors.Num());
	for (int32 ActorIndex = 0; ActorIndex < View.Actors.Num(); ++ActorIndex)
	{
		const FWoTRuntimeWorldActor& Actor = View.Actors[ActorIndex];
		ActorsByIdentity.Add(Actor.Identity.ToLower(), ActorIndex);
		if (WoTSkyClassLeaf(Actor.ClassIdentity).Equals(
			TEXT("SkyZoneInfo"), ESearchCase::IgnoreCase))
		{
			Out.SkyActors.Add(ActorIndex);
		}
	}
	if (Out.SkyActors.IsEmpty()) return true;

	// ZoneInfo.LinkToSkybox default: the last SkyZoneInfo, then the last one
	// whose bHighDetail matches the (True) detail mode (skybake.py:98-110).
	const int32 LastSky = Out.SkyActors.Num() - 1;
	int32 DefaultSky = LastSky;
	for (int32 SkyIndex = 0; SkyIndex < Out.SkyActors.Num(); ++SkyIndex)
	{
		if (const FWoTRuntimeWorldProperty* Property = WoTRuntimeWorldFindProperty(
			View.Actors[Out.SkyActors[SkyIndex]], TEXT("bHighDetail")))
		{
			bool bHighDetail = false;
			if (!WoTRuntimeWorldPropertyBool(*Property, bHighDetail))
			{
				Error = TEXT("SkyZoneInfo bHighDetail is malformed");
				return false;
			}
			if (bHighDetail) DefaultSky = SkyIndex;
		}
	}

	// WOTZoneInfo.uc:131-141: the last SkyZoneInfo whose Tag matches the zone
	// Event; an empty Event iterates every SkyZoneInfo (plain last).
	Out.ZoneSkyLinks.Reserve(View.Zones.Num());
	for (const FWoTRuntimeWorldZone& Zone : View.Zones)
	{
		int32 Linked = DefaultSky;
		const int32* ZoneActor = ActorsByIdentity.Find(Zone.ActorIdentity.ToLower());
		if (ZoneActor != nullptr
			&& WoTSkyIsWotZoneClass(View.Actors[*ZoneActor].ClassIdentity))
		{
			FString Event;
			if (!WoTSkyReadName(WoTRuntimeWorldFindProperty(
				View.Actors[*ZoneActor], TEXT("Event")), Event))
			{
				Error = TEXT("zone Event is malformed");
				return false;
			}
			if (Event.IsEmpty())
			{
				Linked = LastSky;
			}
			else
			{
				for (int32 SkyIndex = 0; SkyIndex < Out.SkyActors.Num(); ++SkyIndex)
				{
					FString Tag;
					if (!WoTSkyReadName(WoTRuntimeWorldFindProperty(
						View.Actors[Out.SkyActors[SkyIndex]], TEXT("Tag")), Tag))
					{
						Error = TEXT("SkyZoneInfo Tag is malformed");
						return false;
					}
					if (Tag.Equals(Event, ESearchCase::IgnoreCase)) Linked = SkyIndex;
				}
			}
		}
		Out.ZoneSkyLinks.Add(Linked);
	}

	// skybake.pick_winner: each FakeBackdrop node votes once per distinct
	// nonzero side zone for that zone's linked sky.
	Out.Tallies.Init(0, Out.SkyActors.Num());
	for (const FWoTRuntimeWorldNode& Node : View.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		if (!View.Surfaces.IsValidIndex(Node.Surface))
		{
			Error = TEXT("world node surface index is out of range");
			return false;
		}
		if ((View.Surfaces[Node.Surface].Flags & WoTRuntimeMapPFFakeBackdrop) == 0)
			continue;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const uint8 ZoneNumber = Node.Zones[Side];
			if (ZoneNumber == 0 || (Side == 1 && ZoneNumber == Node.Zones[0])) continue;
			const int32 Linked = Out.ZoneSkyLinks.IsValidIndex(ZoneNumber)
				? Out.ZoneSkyLinks[ZoneNumber]
				: Out.ZoneSkyLinks.IsEmpty() ? LastSky : Out.ZoneSkyLinks[0];
			if (!Out.Tallies.IsValidIndex(Linked))
			{
				Error = TEXT("zone sky link does not name a SkyZoneInfo");
				return false;
			}
			++Out.Tallies[Linked];
		}
	}
	// Maximum tally; ties go to the earliest actor; no votes -> the last one.
	int32 WinnerSky = LastSky;
	int32 WinnerTally = 0;
	for (int32 SkyIndex = 0; SkyIndex < Out.Tallies.Num(); ++SkyIndex)
	{
		if (Out.Tallies[SkyIndex] > WinnerTally)
		{
			WinnerSky = SkyIndex;
			WinnerTally = Out.Tallies[SkyIndex];
		}
	}
	Out.WinnerSky = WinnerSky;
	Out.WinnerActor = Out.SkyActors[WinnerSky];

	// skygeo.sky_zone_of: the first zone whose zone actor is the winner.
	const FString& WinnerIdentity = View.Actors[Out.WinnerActor].Identity;
	for (int32 ZoneIndex = 0; ZoneIndex < View.Zones.Num(); ++ZoneIndex)
	{
		if (View.Zones[ZoneIndex].ActorIdentity.Equals(
			WinnerIdentity, ESearchCase::IgnoreCase))
		{
			if (Out.Zone == INDEX_NONE) Out.Zone = ZoneIndex;
			++Out.ZoneMatchCount;
		}
	}
	if (Out.Zone <= 0 || Out.Zone > MAX_uint8) return true;

	// skygeo.sky_surface_ids: every surface with a node touching the sky zone.
	const uint8 SkyZone = static_cast<uint8>(Out.Zone);
	for (const FWoTRuntimeWorldNode& Node : View.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		if (!View.Surfaces.IsValidIndex(Node.Surface))
		{
			Error = TEXT("world node surface index is out of range");
			return false;
		}
		if (Node.Zones[0] != SkyZone && Node.Zones[1] != SkyZone) continue;
		const uint8 Other = Node.Zones[0] == SkyZone ? Node.Zones[1] : Node.Zones[0];
		if ((View.Surfaces[Node.Surface].Flags & WoTRuntimeMapPFPortal) != 0
			&& Other != 0 && Other != SkyZone)
		{
			Out.bTraversablePortal = true;
		}
		Out.Surfaces.Add(Node.Surface);
	}
	return true;
}

namespace
{
bool WoTSkySelectionForIndex(const FWoTRetailSkyWorldView& View,
	const FWoTRetailSkySelection& Base, const int32 SkyIndex,
	FWoTRetailSkySelection& Out, FString& Error)
{
	if (!Base.SkyActors.IsValidIndex(SkyIndex))
	{
		Error = TEXT("a zone sky link does not name a SkyZoneInfo");
		return false;
	}
	Out = Base;
	Out.WinnerSky = SkyIndex;
	Out.WinnerActor = Out.SkyActors[SkyIndex];
	Out.Zone = INDEX_NONE;
	Out.ZoneMatchCount = 0;
	Out.Surfaces.Reset();
	Out.bTraversablePortal = false;
	const FString& Identity = View.Actors[Out.WinnerActor].Identity;
	for (int32 ZoneIndex = 0; ZoneIndex < View.Zones.Num(); ++ZoneIndex)
	{
		if (!View.Zones[ZoneIndex].ActorIdentity.Equals(
			Identity, ESearchCase::IgnoreCase)) continue;
		if (Out.Zone == INDEX_NONE) Out.Zone = ZoneIndex;
		++Out.ZoneMatchCount;
	}
	if (Out.Zone <= 0 || Out.Zone > MAX_uint8) return true;
	const uint8 SkyZone = static_cast<uint8>(Out.Zone);
	for (const FWoTRuntimeWorldNode& Node : View.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		if (!View.Surfaces.IsValidIndex(Node.Surface))
		{
			Error = TEXT("world node surface index is out of range");
			return false;
		}
		if (Node.Zones[0] != SkyZone && Node.Zones[1] != SkyZone) continue;
		const uint8 Other = Node.Zones[0] == SkyZone ? Node.Zones[1] : Node.Zones[0];
		if ((View.Surfaces[Node.Surface].Flags & WoTRuntimeMapPFPortal) != 0
			&& Other != 0 && Other != SkyZone)
		{
			Out.bTraversablePortal = true;
		}
		Out.Surfaces.Add(Node.Surface);
	}
	return true;
}
}

double WoTRuntimeMapBoundsSphereRadius(TConstArrayView<FVector3f> Positions)
{
	if (Positions.IsEmpty()) return 0.0;
	FVector3f Minimum = Positions[0];
	FVector3f Maximum = Positions[0];
	for (const FVector3f& Position : Positions)
	{
		Minimum = FVector3f(FMath::Min(Minimum.X, Position.X),
			FMath::Min(Minimum.Y, Position.Y), FMath::Min(Minimum.Z, Position.Z));
		Maximum = FVector3f(FMath::Max(Maximum.X, Position.X),
			FMath::Max(Maximum.Y, Position.Y), FMath::Max(Maximum.Z, Position.Z));
	}
	// FBounds3f::GetCenter + the float SizeSquared of StaticMeshBuilder.cpp.
	const FVector3f Center = (Minimum + Maximum) * 0.5f;
	float RadiusSquared = 0.0f;
	for (const FVector3f& Position : Positions)
		RadiusSquared = FMath::Max(RadiusSquared, (Position - Center).SizeSquared());
	return static_cast<double>(FMath::Sqrt(RadiusSquared));
}

double WoTRuntimeMapSkyScale(double WorldRadius, double MinSurfaceDistance)
{
	if (!FMath::IsFinite(WorldRadius) || !FMath::IsFinite(MinSurfaceDistance)
		|| WorldRadius <= 0.0 || MinSurfaceDistance <= 0.0)
	{
		return 0.0;
	}
	const double Raw = 2.0 * WorldRadius / MinSurfaceDistance * 1.25;
	if (!FMath::IsFinite(Raw)) return 0.0;
	return FMath::Max(12.0, FMath::CeilToDouble(Raw));
}

bool WoTBuildRuntimeMapSkyPlan(const FWoTRetailSkyWorldView& View,
	TConstArrayView<FWoTBSPPayloadData> Parts, FWoTRuntimeMapSkyPlan& Out,
	FString& Error, const int32 ForcedSky)
{
	Out = FWoTRuntimeMapSkyPlan();
	Error.Reset();
	auto Fail = [&Out, &Error](const FString& Message)
	{
		Out = FWoTRuntimeMapSkyPlan();
		Error = Message;
		return false;
	};
	Out.SkySections.SetNum(Parts.Num());
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		Out.SkySections[PartIndex].Init(false, Part.Sections.Num());
		// setup_childsky.world_slot_materials: world_r stays main-mesh-only
		// (Geo/<Map>, one static mesh per split part); portal meshes can hold
		// far sky sheets that would inflate SkyScale.
		if (!Part.bPortal)
		{
			Out.WorldRadius = FMath::Max(Out.WorldRadius,
				WoTRuntimeMapBoundsSphereRadius(Part.Positions));
		}
	}
	FString SelectionError;
	if (!WoTSelectRetailSky(View, Out.Selection, SelectionError))
		return Fail(SelectionError);
	if (ForcedSky != INDEX_NONE)
	{
		const FWoTRetailSkySelection Base = Out.Selection;
		if (!WoTSkySelectionForIndex(
			View, Base, ForcedSky, Out.Selection, SelectionError))
		{
			return Fail(SelectionError);
		}
	}
	if (!Out.Selection.HasSkyZone()) return true;
	for (const FWoTBSPPayloadData& Part : Parts)
	{
		// Only BSP-v4 carries the exact section identity and last-writer flags
		// this partition is recomputed from; legacy parts keep the world sky.
		if (Part.FormatVersion < 4) return true;
	}

	const FWoTRuntimeWorldActor& SkyActor = View.Actors[Out.Selection.WinnerActor];
	if (const FWoTRuntimeWorldProperty* Location =
		WoTRuntimeWorldFindProperty(SkyActor, TEXT("Location")))
	{
		if (!WoTRuntimeWorldPropertyVector(*Location, Out.Origin))
			return Fail(TEXT("winning SkyZoneInfo Location is malformed"));
	}
	if (const FWoTRuntimeWorldProperty* Rotation =
		WoTRuntimeWorldFindProperty(SkyActor, TEXT("Rotation")))
	{
		if (!WoTRuntimeWorldPropertyRotator(*Rotation, Out.Rotation))
			return Fail(TEXT("winning SkyZoneInfo Rotation is malformed"));
	}

	// skygeo.py:118-129 / bsp2obj.sky_shared_split: a material used by both sky
	// and non-sky visible faces is split (__SkyZone); the others are sky-only.
	TSet<FString> SkyKeys;
	TSet<FString> WorldKeys;
	for (const FWoTRuntimeWorldNode& Node : View.Nodes)
	{
		if (Node.VertexCount < 3) continue;
		if (!View.Surfaces.IsValidIndex(Node.Surface))
			return Fail(TEXT("world node surface index is out of range"));
		const FWoTRuntimeWorldSurface& Surface = View.Surfaces[Node.Surface];
		if ((Surface.Flags & WoTRuntimeMapPFInvisible) != 0) continue;
		const FString Key = WoTSkySlotKey(Surface.TextureIdentity, Surface.Flags);
		if (Out.Selection.Surfaces.Contains(Node.Surface)) SkyKeys.Add(Key);
		else WorldKeys.Add(Key);
	}

	const FStringView SkyZoneSuffix(TEXT("__SkyZone"));
	TMap<FString, int32> GroupBySlot;
	TArray<FWoTSkySlotGroup> Groups;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData& Part = Parts[PartIndex];
		for (int32 SectionIndex = 0; SectionIndex < Part.Sections.Num(); ++SectionIndex)
		{
			const FWoTBSPPayloadSection& Section = Part.Sections[SectionIndex];
			bool bSky = false;
			bool bEntireSectionIsSky = true;
			const bool bExactProvenance = Part.FormatVersion >= 10
				&& Part.TriangleSurfaces.Num() == Part.Indices.Num() / 3;
			if (bExactProvenance)
			{
				const uint64 End = static_cast<uint64>(Section.FirstIndex)
					+ Section.IndexCount;
				if (Section.FirstIndex % 3 != 0 || Section.IndexCount % 3 != 0
					|| End > static_cast<uint64>(Part.Indices.Num()))
				{
					return Fail(TEXT("a sky BSP section range is malformed"));
				}
				for (uint64 At = Section.FirstIndex; At < End; At += 3)
				{
					const uint32 Surface = Part.TriangleSurfaces[At / 3];
					const bool bTriangleSky = Out.Selection.Surfaces.Contains(
						static_cast<int32>(Surface));
					bSky |= bTriangleSky;
					bEntireSectionIsSky &= bTriangleSky;
				}
			}
			else
			{
				const FString Key = WoTSkySlotKey(
					Section.SourceTextureIdentity, Section.SurfaceFlags);
				const bool bSplitSlot = FStringView(Section.MaterialSlot).EndsWith(
					SkyZoneSuffix, ESearchCase::CaseSensitive);
				if (bSplitSlot)
				{
					// The native writer split this slot with its own copy of the
					// winner rule; world-v3 must agree or the records disagree.
					if (!SkyKeys.Contains(Key) || !WorldKeys.Contains(Key))
					{
						return Fail(TEXT(
							"a __SkyZone BSP section is not a recomputed sky/world split slot"));
					}
					bSky = true;
				}
				else
				{
					bSky = SkyKeys.Contains(Key) && !WorldKeys.Contains(Key);
				}
			}
			if (!bSky) continue;
			if ((Section.SurfaceFlags & WoTRuntimeMapPFFakeBackdrop) != 0)
			{
				// A FakeBackdrop is the opaque world-space window onto this
				// child scene. It must stay out of the captured sky mesh and,
				// critically, must never be marked for removal from the world.
				++Out.FakeBackdropSkySlots;
				continue;
			}
			Out.SkySections[PartIndex][SectionIndex] = bEntireSectionIsSky;
			int32* Existing = GroupBySlot.Find(Section.MaterialSlot);
			if (Existing == nullptr)
			{
				const int32 NewIndex = Groups.AddDefaulted();
				Groups[NewIndex].Slot = Section.MaterialSlot;
				GroupBySlot.Add(Section.MaterialSlot, NewIndex);
				Existing = GroupBySlot.Find(Section.MaterialSlot);
			}
			FWoTRuntimeMapSectionRef Ref;
			Ref.Part = PartIndex;
			Ref.Section = SectionIndex;
			Groups[*Existing].Sources.Add(Ref);
		}
	}
	if (Groups.IsEmpty()) return true;
	// skygeo emits sorted(groups): ordinal string order.
	Algo::Sort(Groups, [](const FWoTSkySlotGroup& A, const FWoTSkySlotGroup& B)
	{
		return A.Slot.Compare(B.Slot, ESearchCase::CaseSensitive) < 0;
	});

	FWoTBSPPayloadData& Sky = Out.SkyMesh;
	Sky.FormatVersion = 4;
	// The shared render builder encodes noncolliding sections with its portal
	// role bit (the entry sky does the same, WoTFrontEndSceneMaterializer).
	Sky.bPortal = true;
	TArray<TArray<int32>> Remaps;
	Remaps.SetNum(Parts.Num());
	const FVector3d Origin = Out.Origin;
	double MinimumDistance = TNumericLimits<double>::Max();
	for (const FWoTSkySlotGroup& Group : Groups)
	{
		const FWoTBSPPayloadSection& First =
			Parts[Group.Sources[0].Part].Sections[Group.Sources[0].Section];
		FWoTBSPPayloadSection NewSection;
		NewSection.MaterialSlot = Group.Slot;
		NewSection.CanonicalPackage = First.CanonicalPackage;
		NewSection.CanonicalName = First.CanonicalName;
		NewSection.SourceTextureIdentity = First.SourceTextureIdentity;
		NewSection.SurfaceFlags = First.SurfaceFlags;
		NewSection.PanZoneActorIdentity = First.PanZoneActorIdentity;
		NewSection.MacroCanonicalPackage = First.MacroCanonicalPackage;
		NewSection.MacroCanonicalName = First.MacroCanonicalName;
		NewSection.MacroSourceTextureIdentity = First.MacroSourceTextureIdentity;
		NewSection.MacroRatio = First.MacroRatio;
		NewSection.MacroOffsetU = First.MacroOffsetU;
		NewSection.MacroOffsetV = First.MacroOffsetV;
		NewSection.bCollisionEnabled = false;
		NewSection.FirstIndex = static_cast<uint32>(Sky.Indices.Num());
		uint32 Minimum = MAX_uint32;
		uint32 Maximum = 0;
		for (const FWoTRuntimeMapSectionRef& Source : Group.Sources)
		{
			const FWoTBSPPayloadData& Part = Parts[Source.Part];
			const FWoTBSPPayloadSection& PartSection = Part.Sections[Source.Section];
			const uint64 End = static_cast<uint64>(PartSection.FirstIndex)
				+ PartSection.IndexCount;
			if (PartSection.IndexCount % 3 != 0
				|| End > static_cast<uint64>(Part.Indices.Num())
				|| Part.Normals.Num() != Part.Positions.Num()
				|| Part.UV0.Num() != Part.Positions.Num()
				|| Part.UV1.Num() != Part.Positions.Num())
			{
				return Fail(TEXT("a sky BSP section range or stream is malformed"));
			}
			TArray<int32>& Remap = Remaps[Source.Part];
			if (Remap.IsEmpty()) Remap.Init(INDEX_NONE, Part.Positions.Num());
			for (uint64 At = PartSection.FirstIndex; At < End; At += 3)
			{
				if (Part.FormatVersion >= 10
					&& Part.TriangleSurfaces.Num() == Part.Indices.Num() / 3
					&& !Out.Selection.Surfaces.Contains(static_cast<int32>(
						Part.TriangleSurfaces[At / 3])))
				{
					continue;
				}
				FVector3d Corners[3];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 SourceVertex =
						Part.Indices[static_cast<int32>(At) + Corner];
					const int32 VertexIndex = static_cast<int32>(SourceVertex);
					if (!Part.Positions.IsValidIndex(VertexIndex)
						|| !Remap.IsValidIndex(VertexIndex))
					{
						return Fail(TEXT("a sky BSP index is out of range"));
					}
					int32& Mapped = Remap[VertexIndex];
					if (Mapped == INDEX_NONE)
					{
						Mapped = Sky.Positions.Num();
						// skygeo.py:248-266: re-centre on the SkyZoneInfo origin.
						Sky.Positions.Add(FVector3f(
							FVector3d(Part.Positions[VertexIndex]) - Origin));
						Sky.Normals.Add(Part.Normals[VertexIndex]);
						Sky.UV0.Add(Part.UV0[VertexIndex]);
						Sky.UV1.Add(Part.UV1[VertexIndex]);
					}
					const uint32 MappedIndex = static_cast<uint32>(Mapped);
					Sky.Indices.Add(MappedIndex);
					Minimum = FMath::Min(Minimum, MappedIndex);
					Maximum = FMath::Max(Maximum, MappedIndex);
					Corners[Corner] = FVector3d(Part.Positions[VertexIndex]);
				}
				// skygeo.py:238-246: face (not vertex) distance to the origin.
				const double Distance = WoTRuntimeMapPointTriangleDistance(
					Origin, Corners[0], Corners[1], Corners[2]);
				if (!FMath::IsFinite(Distance))
					return Fail(TEXT("a sky face distance is nonfinite"));
				MinimumDistance = FMath::Min(MinimumDistance, Distance);
				++Out.SkyTriangleCount;
			}
		}
		NewSection.IndexCount =
			static_cast<uint32>(Sky.Indices.Num()) - NewSection.FirstIndex;
		if (NewSection.IndexCount == 0) continue;
		NewSection.MinVertex = Minimum;
		NewSection.MaxVertex = Maximum;
		Sky.Sections.Add(MoveTemp(NewSection));
		Out.SkySlots.Add(Group.Slot);
		Out.SkyMaterialSources.Add(Group.Sources[0]);
		Out.SkySectionSources.Add(Group.Sources);
		Out.SkySectionKinds.Add(EWoTRuntimeMapSkySectionKind::Sky);
	}

	// Sky rest: the main part's version-6 census (skybake.py
	// bake_rest) and its rest groups (skygeo.py:185-224). setup_childsky.py
	// :313-329 binds each "SkyRest__<slot>" group to that WORLD slot's
	// material, hides "SkyRestFB" and draws "SkyRestBlock" opaque black; every
	// emitted face (FB and blockers included) counts toward the face distance
	// that sizes SkyScale (skygeo.py:238-246).
	const FWoTBSPSkyRest* SkyRest = nullptr;
	for (const FWoTBSPPayloadData& Part : Parts)
	{
		if (!Part.SkyRest.bPresent) continue;
		if (SkyRest != nullptr || Part.bPortal)
			return Fail(TEXT("a sky-rest census rides more than the main part"));
		SkyRest = &Part.SkyRest;
	}
	if (SkyRest != nullptr)
	{
		if (static_cast<int32>(SkyRest->SkyZone) != Out.Selection.Zone)
		{
			// The authenticated census was baked for the historical majority
			// child scene. A forced minority scene has no corresponding rest
			// census and simply omits that optional extension.
			if (ForcedSky == INDEX_NONE)
				return Fail(TEXT("the sky-rest census names another sky zone"));
			SkyRest = nullptr;
		}
	}
	if (SkyRest != nullptr)
	{
		Out.bSkyRestCensus = true;
		Out.SkyRestSurfaceCount = static_cast<uint32>(SkyRest->Surfaces.Num());
		auto FindWorldSection = [&Parts](const FString& Slot)
		{
			// setup_childsky.world_slot_materials: main meshes, then portals.
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
				{
					if (Parts[PartIndex].bPortal != (Pass == 1)) continue;
					const TArray<FWoTBSPPayloadSection>& Sections = Parts[PartIndex].Sections;
					for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
					{
						const FString& Candidate = Sections[SectionIndex].MaterialSlot;
						if (Candidate.Equals(Slot, ESearchCase::CaseSensitive)
							|| Candidate.StartsWith(Slot + TEXT("__MacroPan_"),
								ESearchCase::CaseSensitive))
						{
							FWoTRuntimeMapSectionRef Ref;
							Ref.Part = PartIndex;
							Ref.Section = SectionIndex;
							return Ref;
						}
					}
				}
			}
			return FWoTRuntimeMapSectionRef();
		};
		for (const FWoTBSPSkyRestGroup& Group : SkyRest->Groups)
		{
			const int32 Corners = Group.Positions.Num();
			if (Corners == 0 || Corners % 3 != 0 || Group.Normals.Num() != Corners
				|| Group.UV0.Num() != Corners || Group.UV1.Num() != Corners)
				return Fail(TEXT("a sky-rest group stream is malformed"));
			for (int32 Corner = 0; Corner + 2 < Corners; Corner += 3)
			{
				const double Distance = WoTRuntimeMapPointTriangleDistance(Origin,
					FVector3d(Group.Positions[Corner]),
					FVector3d(Group.Positions[Corner + 1]),
					FVector3d(Group.Positions[Corner + 2]));
				if (!FMath::IsFinite(Distance))
					return Fail(TEXT("a sky-rest face distance is nonfinite"));
				MinimumDistance = FMath::Min(MinimumDistance, Distance);
			}
			if (Group.Role == EWoTBSPSkyRestRole::FakeBackdrop) continue;
			FWoTRuntimeMapSectionRef Source;
			FString SectionName = TEXT("SkyRestBlock");
			EWoTRuntimeMapSkySectionKind Kind = EWoTRuntimeMapSkySectionKind::RestBlocker;
			FWoTBSPPayloadSection NewSection;
			if (Group.Role == EWoTBSPSkyRestRole::Visible)
			{
				Source = FindWorldSection(Group.Slot);
				if (Source.Part == INDEX_NONE)
				{
					++Out.SkyRestMissingSlots;
					continue;
				}
				SectionName = TEXT("SkyRest__") + Group.Slot;
				Kind = EWoTRuntimeMapSkySectionKind::RestVisible;
				const FWoTBSPPayloadSection& World = Parts[Source.Part].Sections[Source.Section];
				NewSection.CanonicalPackage = World.CanonicalPackage;
				NewSection.CanonicalName = World.CanonicalName;
				NewSection.SourceTextureIdentity = World.SourceTextureIdentity;
				NewSection.SurfaceFlags = World.SurfaceFlags;
				NewSection.PanZoneActorIdentity = World.PanZoneActorIdentity;
			}
			NewSection.MaterialSlot = SectionName;
			NewSection.bCollisionEnabled = false;
			NewSection.FirstIndex = static_cast<uint32>(Sky.Indices.Num());
			const uint32 FirstVertex = static_cast<uint32>(Sky.Positions.Num());
			for (int32 Corner = 0; Corner < Corners; ++Corner)
			{
				Sky.Positions.Add(FVector3f(FVector3d(Group.Positions[Corner]) - Origin));
				Sky.Normals.Add(Group.Normals[Corner]);
				Sky.UV0.Add(Group.UV0[Corner]);
				Sky.UV1.Add(Group.UV1[Corner]);
				Sky.Indices.Add(FirstVertex + static_cast<uint32>(Corner));
			}
			NewSection.IndexCount = static_cast<uint32>(Corners);
			NewSection.MinVertex = FirstVertex;
			NewSection.MaxVertex = FirstVertex + static_cast<uint32>(Corners) - 1;
			Sky.Sections.Add(MoveTemp(NewSection));
			Out.SkyRestSlots.Add(SectionName);
			Out.SkyMaterialSources.Add(Source);
			Out.SkySectionSources.Add(Source.Part == INDEX_NONE
				? TArray<FWoTRuntimeMapSectionRef>() : TArray<FWoTRuntimeMapSectionRef>{Source});
			Out.SkySectionKinds.Add(Kind);
			Out.SkyRestTriangleCount += static_cast<uint32>(Corners / 3);
			Out.SkyTriangleCount += static_cast<uint32>(Corners / 3);
		}
	}
	if (Sky.Sections.IsEmpty() || Sky.Positions.IsEmpty())
	{
		// Every sky slot was degenerate or a FakeBackdrop window: no sky mesh,
		// and nothing sky-only remains visible in the world either.
		Out.SkyMesh = FWoTBSPPayloadData();
		return true;
	}
	Sky.BoundsMin = Sky.Positions[0];
	Sky.BoundsMax = Sky.Positions[0];
	for (const FVector3f& Position : Sky.Positions)
	{
		Sky.BoundsMin = FVector3f(FMath::Min(Sky.BoundsMin.X, Position.X),
			FMath::Min(Sky.BoundsMin.Y, Position.Y),
			FMath::Min(Sky.BoundsMin.Z, Position.Z));
		Sky.BoundsMax = FVector3f(FMath::Max(Sky.BoundsMax.X, Position.X),
			FMath::Max(Sky.BoundsMax.Y, Position.Y),
			FMath::Max(Sky.BoundsMax.Z, Position.Z));
	}
	if (MinimumDistance == TNumericLimits<double>::Max())
		return Fail(TEXT("the sky mesh has no measurable face"));
	Out.MinSurfaceDistance = MinimumDistance;
	const double Scale = WoTRuntimeMapSkyScale(Out.WorldRadius, Out.MinSurfaceDistance);
	if (Scale <= 0.0 || Scale > 1000000.0)
		return Fail(TEXT("the map sky scale is invalid or over bound"));
	Out.Scale = static_cast<float>(Scale);
	Out.SkyActorIdentity = SkyActor.Identity;
	Out.bHasSky = true;
	return true;
}

bool WoTBuildRuntimeMapSkyPlans(const FWoTRetailSkyWorldView& View,
	TConstArrayView<FWoTBSPPayloadData> Parts,
	TArray<FWoTRuntimeMapSkyPlan>& OutPlans, TArray<int32>& ZonePlanIndices,
	FString& Error)
{
	OutPlans.Reset();
	ZonePlanIndices.Init(INDEX_NONE, View.Zones.Num());
	Error.Reset();
	FWoTRetailSkySelection Selection;
	if (!WoTSelectRetailSky(View, Selection, Error)) return false;
	TMap<int32, int32> PlanBySky;
	auto EnsurePlan = [&](const int32 SkyIndex) -> bool
	{
		if (PlanBySky.Contains(SkyIndex)) return true;
		if (!Selection.SkyActors.IsValidIndex(SkyIndex))
		{
			Error = TEXT("a zone sky link does not name a SkyZoneInfo");
			return false;
		}
		FWoTRuntimeMapSkyPlan Plan;
		if (!WoTBuildRuntimeMapSkyPlan(View, Parts, Plan, Error, SkyIndex))
			return false;
		PlanBySky.Add(SkyIndex, Plan.bHasSky ? OutPlans.Add(MoveTemp(Plan)) : INDEX_NONE);
		return true;
	};
	// Preserve the historical majority child scene as plan zero/SkyBox.
	if (Selection.WinnerSky != INDEX_NONE && !EnsurePlan(Selection.WinnerSky))
	{
		OutPlans.Reset();
		ZonePlanIndices.Init(INDEX_NONE, View.Zones.Num());
		return false;
	}
	for (int32 ZoneIndex = 0; ZoneIndex < Selection.ZoneSkyLinks.Num(); ++ZoneIndex)
	{
		const int32 SkyIndex = Selection.ZoneSkyLinks[ZoneIndex];
		if (!EnsurePlan(SkyIndex))
		{
			OutPlans.Reset();
			ZonePlanIndices.Init(INDEX_NONE, View.Zones.Num());
			return false;
		}
		ZonePlanIndices[ZoneIndex] = PlanBySky[SkyIndex];
	}
	return true;
}

int32 WoTRuntimeMapPointZone(
	TConstArrayView<FWoTRuntimeWorldNode> Nodes, const FVector& Point)
{
	if (Nodes.IsEmpty()) return 0;
	int32 NodeIndex = 0;
	int32 Zone = 0;
	for (int32 Depth = 0; NodeIndex != INDEX_NONE && Depth <= Nodes.Num(); ++Depth)
	{
		if (!Nodes.IsValidIndex(NodeIndex)) return 0;
		const FWoTRuntimeWorldNode& Node = Nodes[NodeIndex];
		const double Distance = Point.X * static_cast<double>(Node.Plane[0])
			+ Point.Y * static_cast<double>(Node.Plane[1])
			+ Point.Z * static_cast<double>(Node.Plane[2])
			- static_cast<double>(Node.Plane[3]);
		const bool bFront = Distance >= 0.0;
		Zone = Node.Zones[bFront ? 1 : 0];
		NodeIndex = bFront ? Node.Front : Node.Back;
	}
	return Zone;
}

TMap<FString, FWoTRuntimeMapSlotPan> WoTRuntimeMapSlotPans(
	TConstArrayView<FWoTBSPPayloadData> Parts)
{
	TMap<FString, FWoTRuntimeMapSlotPan> Result;
	// import_map.py main(): mats = dict(materials); mats.update(portal_materials).
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bPortalPass = Pass == 1;
		for (const FWoTBSPPayloadData& Part : Parts)
		{
			if (Part.bPortal != bPortalPass) continue;
			for (const FWoTBSPPayloadSection& Section : Part.Sections)
			{
				FWoTRuntimeMapSlotPan& Pan = Result.FindOrAdd(Section.MaterialSlot);
				Pan.Flags = Section.SurfaceFlags;
				Pan.PanZoneActorIdentity = Section.PanZoneActorIdentity;
			}
		}
	}
	return Result;
}

FVector2f FWoTRuntimeMapPanSpeeds::Find(FStringView ZoneActorIdentity) const
{
	if (ZoneActorIdentity.IsEmpty()) return FVector2f(1.0f, 1.0f);
	const FVector2f* Found = ByZone.Find(FString(ZoneActorIdentity).ToLower());
	return Found != nullptr ? *Found : FVector2f(1.0f, 1.0f);
}

FWoTRuntimeMapPanSpeeds WoTRuntimeMapBuildPanSpeeds(
	TConstArrayView<FWoTRuntimeWorldActor> Actors)
{
	FWoTRuntimeMapPanSpeeds Result;
	for (const FWoTRuntimeWorldActor& Actor : Actors)
	{
		const FWoTRuntimeWorldProperty* U =
			WoTRuntimeWorldFindProperty(Actor, TEXT("TexUPanSpeed"));
		const FWoTRuntimeWorldProperty* V =
			WoTRuntimeWorldFindProperty(Actor, TEXT("TexVPanSpeed"));
		if (U == nullptr && V == nullptr) continue;
		FVector2f Speeds(1.0f, 1.0f);
		float Value = 0.0f;
		if (U != nullptr)
		{
			if (WoTRuntimeWorldPropertyFloat(*U, Value)) Speeds.X = Value;
			else ++Result.MalformedZones;
		}
		if (V != nullptr)
		{
			if (WoTRuntimeWorldPropertyFloat(*V, Value)) Speeds.Y = Value;
			else ++Result.MalformedZones;
		}
		Result.ByZone.Add(Actor.Identity.ToLower(), Speeds);
	}
	return Result;
}

FVector2f WoTRuntimeMapPanRate(uint32 SlotFlags, const FVector2f& Speeds,
	uint32 TextureWidth, uint32 TextureHeight)
{
	FVector2f Rate = FVector2f::ZeroVector;
	if ((SlotFlags & WoTRuntimeMapPFAutoUPan) != 0)
	{
		Rate.X = static_cast<float>(35.0 * static_cast<double>(Speeds.X)
			/ static_cast<double>(FMath::Max<uint32>(TextureWidth, 1u)));
	}
	if ((SlotFlags & WoTRuntimeMapPFAutoVPan) != 0)
	{
		Rate.Y = static_cast<float>(35.0 * static_cast<double>(Speeds.Y)
			/ static_cast<double>(FMath::Max<uint32>(TextureHeight, 1u)));
	}
	if (!FMath::IsFinite(Rate.X)) Rate.X = 0.0f;
	if (!FMath::IsFinite(Rate.Y)) Rate.Y = 0.0f;
	return Rate;
}
