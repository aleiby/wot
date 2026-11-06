#include "WoTMeshLight.h"

#include "WoTMapLightTable.h"
#include "WoTSpriteBillboard.h"
#include "Mesh/WoTRuntimeVertexMesh.h"

#include "Algo/BinarySearch.h"
#include "Components/MeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr int32 MaxMeshLights = 6;
constexpr float RuntimeBakeInterval = 0.25f;
constexpr float RuntimeBakeMoveDistance = 128.0f;
constexpr float MoverSurfaceAmbientScale = 0.5f;

struct FWoTLightCandidate
{
	int32 LightIndex = INDEX_NONE;
	int32 LightingTag = 0;
};

uint32 FloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

float BitsFloat(uint32 Bits)
{
	float Value = 0.0f;
	FMemory::Memcpy(&Value, &Bits, sizeof(Value));
	return Value;
}

struct FWoTSqrtMantissaTable
{
	float Values[2 << 10];

	FWoTSqrtMantissaTable()
	{
		const float SqrtTwo = FMath::Sqrt(2.0f);
		for (uint32 Mantissa = 0; Mantissa < (1u << 10); ++Mantissa)
		{
			const float Sample = BitsFloat(
				0x3f800000u + (Mantissa << (23 - 10)));
			const float Root = BitsFloat(
				FloatBits(FMath::Sqrt(Sample)) - (64u << 23));
			Values[Mantissa] = Root * SqrtTwo;
			Values[Mantissa + (1u << 10)] = Root * 2.0f;
		}
	}
};

float RetailSqrtApprox(float Value)
{
	// Original renderer's sqrt: the top ten mantissa bits plus the exponent low
	// bit select SqrtManTbl; the remaining exponent is halved and recombined
	// by integer addition to the lookup's IEEE-754 representation.
	static const FWoTSqrtMantissaTable Table;
	const uint32 Bits = FloatBits(Value);
	const uint32 TableIndex = (Bits >> (23 - 10)) & ((2u << 10) - 1u);
	const uint32 ExponentHalf = (Bits & 0x7f000000u) >> 1;
	return BitsFloat(FloatBits(Table.Values[TableIndex]) + ExponentHalf);
}

float RetailDistanceSquared(const FVector& A, const FVector& B)
{
	const float DX = static_cast<float>(A.X) - static_cast<float>(B.X);
	const float DY = static_cast<float>(A.Y) - static_cast<float>(B.Y);
	const float DZ = static_cast<float>(A.Z) - static_cast<float>(B.Z);
	const float XY = DX * DX + DY * DY;
	return XY + DZ * DZ;
}

bool ChildOutside(const UWoTMapLightTable* Table, int32 NodeIndex,
	bool bFront, bool bOutside)
{
	const bool bCsg = Table->LeafBspCsg[NodeIndex] != 0;
	return bFront ? (bOutside || bCsg) : (bOutside && !bCsg);
}

bool LineCheckNode(const UWoTMapLightTable* Table, int32 HitNode,
	int32 NodeIndex, FVector End, FVector Start, bool bOutside, int32& Budget,
	int32* OutHitNode)
{
	while (NodeIndex >= 0)
	{
		if (--Budget < 0 || !Table->ZoneBspPlanes.IsValidIndex(NodeIndex))
		{
			return false;
		}
		const FVector4& Plane = Table->ZoneBspPlanes[NodeIndex];
		const double DistStart = Start.X * Plane.X + Start.Y * Plane.Y
			+ Start.Z * Plane.Z - Plane.W;
		const double DistEnd = End.X * Plane.X + End.Y * Plane.Y
			+ End.Z * Plane.Z - Plane.W;
		if (DistStart > -0.001 && DistEnd > -0.001)
		{
			bOutside = bOutside || Table->LeafBspCsg[NodeIndex] != 0;
			NodeIndex = Table->ZoneBspFront[NodeIndex];
		}
		else if (DistStart < 0.001 && DistEnd < 0.001)
		{
			bOutside = bOutside && Table->LeafBspCsg[NodeIndex] == 0;
			NodeIndex = Table->ZoneBspBack[NodeIndex];
		}
		else
		{
			const double Alpha = DistStart / (DistEnd - DistStart);
			const FVector Middle = Start + (Start - End) * Alpha;
			const bool bFrontFirst = DistStart > 0.0;
			const int32 FirstChild = bFrontFirst
				? Table->ZoneBspFront[NodeIndex]
				: Table->ZoneBspBack[NodeIndex];
			if (!LineCheckNode(Table, HitNode, FirstChild, Middle, Start,
				ChildOutside(Table, NodeIndex, bFrontFirst, bOutside), Budget,
				OutHitNode))
			{
				return false;
			}
			const bool bOtherFront = !bFrontFirst;
			bOutside = ChildOutside(
				Table, NodeIndex, bOtherFront, bOutside);
			HitNode = NodeIndex;
			NodeIndex = bOtherFront ? Table->ZoneBspFront[NodeIndex]
				: Table->ZoneBspBack[NodeIndex];
			Start = Middle;
		}
	}
	if (!bOutside && OutHitNode)
	{
		*OutHitNode = HitNode;
	}
	return bOutside;
}

float ActorNumericProperty(const AActor* Actor, FName Name, float DefaultValue)
{
	if (!IsValid(Actor))
	{
		return DefaultValue;
	}
	const FNumericProperty* Property = FindFProperty<FNumericProperty>(Actor->GetClass(), Name);
	if (!Property)
	{
		return DefaultValue;
	}
	const void* Value = Property->ContainerPtrToValuePtr<void>(Actor);
	if (Property->IsFloatingPoint())
	{
		return static_cast<float>(Property->GetFloatingPointPropertyValue(Value));
	}
	if (Property->IsInteger())
	{
		return static_cast<float>(Property->GetSignedIntPropertyValue(Value));
	}
	return DefaultValue;
}

// Root master name of the material's parent chain, or empty.
FString MaterialRootName(const UMaterialInterface* Material)
{
	const UMaterialInterface* Current = Material;
	while (IsValid(Current))
	{
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
		if (!Instance || !IsValid(Instance->Parent))
		{
			return Current->GetName();
		}
		Current = Instance->Parent;
	}
	return FString();
}

bool ActorBoolProperty(const AActor* Actor, FName Name, bool bDefaultValue)
{
	if (!IsValid(Actor))
	{
		return bDefaultValue;
	}
	const FBoolProperty* Property = FindFProperty<FBoolProperty>(Actor->GetClass(), Name);
	if (!Property)
	{
		return bDefaultValue;
	}
	return Property->GetPropertyValue(Property->ContainerPtrToValuePtr<void>(Actor));
}

bool IsMeshLitRoot(const FString& Root)
{
	return Root == TEXT("M_WoT_MeshLit")
		|| Root == TEXT("M_WoT_MeshLit2S")
		|| Root == TEXT("M_WoT_MeshLitMasked")
		|| Root == TEXT("M_WoT_MeshLitT");
}

EWoTRuntimeVertexMeshStyle WoTMeshLightStyleForRoot(const FString& Root)
{
	if (Root == TEXT("M_WoT_MeshTranslucent")
		|| Root == TEXT("M_WoT_MeshLitT"))
	{
		return EWoTRuntimeVertexMeshStyle::Translucent;
	}
	if (Root == TEXT("M_WoT_MeshModulate"))
	{
		return EWoTRuntimeVertexMeshStyle::Modulated;
	}
	if (Root == TEXT("M_WoT_Masked")
		|| Root == TEXT("M_WoT_UnlitMasked")
		|| Root == TEXT("M_WoT_MeshLitMasked"))
	{
		return EWoTRuntimeVertexMeshStyle::Masked;
	}
	if (Root == TEXT("M_WoT_MeshLit2S"))
	{
		return EWoTRuntimeVertexMeshStyle::TwoSided;
	}
	return EWoTRuntimeVertexMeshStyle::Normal;
}

FString WoTMeshLightTargetRoot(const UMaterialInterface* Material, uint8 ActorStyle,
	bool bUnlit)
{
	if (!IsValid(Material))
	{
		return FString();
	}
	const FString Root = MaterialRootName(Material);
	const bool bKnownMeshRoot = Root == TEXT("M_WoT_Master")
		|| Root == TEXT("M_WoT_Masked") || Root == TEXT("M_WoT_Unlit")
		|| Root == TEXT("M_WoT_UnlitMasked")
		|| Root == TEXT("M_WoT_MeshTranslucent")
		|| Root == TEXT("M_WoT_MeshModulate") || IsMeshLitRoot(Root);
	const bool bFallback = Root == TEXT("M_Default")
		|| Root == TEXT("WorldGridMaterial");
	if (!bKnownMeshRoot && !bFallback)
	{
		return FString();
	}

	EWoTRuntimeVertexMeshStyle SlotStyle = WoTMeshLightStyleForRoot(Root);
	if (bFallback)
	{
		SlotStyle = Material->GetBlendMode() == BLEND_Masked
			? EWoTRuntimeVertexMeshStyle::Masked
			: Material->GetBlendMode() == BLEND_Opaque
				? EWoTRuntimeVertexMeshStyle::Normal
				: EWoTRuntimeVertexMeshStyle::Translucent;
	}
	const EWoTRuntimeVertexMeshStyle EffectiveStyle =
		WoTRuntimeEffectiveMeshStyle(SlotStyle, ActorStyle);
	if (EffectiveStyle == EWoTRuntimeVertexMeshStyle::Translucent)
	{
		// Actor STY_Translucent takes the translucent family its bUnlit
		// selects: UE1 lights a translucent mesh like any other unless it is
		// bUnlit (mesh renderer: GUnlitColor only then), so a lit one takes
		// the lit translucent master and a bUnlit one the GUnlitColor master
		// (the import-side _ml_skin_override). Otherwise the Light Orb glass
		// (PixieJar) takes the unlit master with zone ambient and goes unseen.
		// A translucent Interchange fallback still swaps to the lit
		// translucent family; a typed translucent root is already right.
		if (ActorStyle == 3)
		{
			return bUnlit ? TEXT("M_WoT_MeshTranslucent") : TEXT("M_WoT_MeshLitT");
		}
		// An actor's bUnlit makes every poly unlit whatever its Style
		// (the original renderer ORs PF_Unlit into the draw), so translucent poly
		// slots of a bUnlit STY_Normal actor take the GUnlitColor master too:
		// LightningStreak02's LSbolt segments drew lit and white otherwise.
		if (bUnlit)
		{
			return TEXT("M_WoT_MeshTranslucent");
		}
		return bFallback ? TEXT("M_WoT_MeshLitT") : Root;
	}
	if (EffectiveStyle == EWoTRuntimeVertexMeshStyle::Modulated)
	{
		return TEXT("M_WoT_MeshModulate");
	}
	if (EffectiveStyle == EWoTRuntimeVertexMeshStyle::Masked)
	{
		return Root == TEXT("M_WoT_Unlit")
			|| Root == TEXT("M_WoT_UnlitMasked")
			? TEXT("M_WoT_UnlitMasked") : TEXT("M_WoT_MeshLitMasked");
	}
	if (Root == TEXT("M_WoT_Unlit"))
	{
		return Root;
	}
	return EffectiveStyle == EWoTRuntimeVertexMeshStyle::TwoSided
		? TEXT("M_WoT_MeshLit2S") : TEXT("M_WoT_MeshLit");
}

bool IsTargetMeshFamily(const UMaterialInterface* Material, uint8 ActorStyle,
	bool bUnlit)
{
	const FString TargetRoot = WoTMeshLightTargetRoot(Material, ActorStyle, bUnlit);
	return !TargetRoot.IsEmpty() && MaterialRootName(Material) == TargetRoot;
}

// Only known static mesh-skin roots enter this routing table. Procedural and
// particle material graphs remain untouched, while Actor.Style can still
// promote an ordinary skin to its retail blend family.
UMaterialInterface* MeshLitMasterFor(const UMaterialInterface* Material,
	uint8 ActorStyle, bool bUnlit)
{
	const FString AssetName = WoTMeshLightTargetRoot(Material, ActorStyle, bUnlit);
	if (AssetName.IsEmpty()) return nullptr;
	return Cast<UMaterialInterface>(FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Materials/%s.%s"), *AssetName, *AssetName)).TryLoad());
}

FLinearColor LegacyZoneAmbientAt(const UWoTMapLightTable* Table, const FVector& Point)
{
	FLinearColor Fallback = FLinearColor::Black;
	FLinearColor Best = FLinearColor::Black;
	double BestVolume = TNumericLimits<double>::Max();
	for (const FWoTZoneAmbient& Zone : Table->ZoneAmbients)
	{
		if (Zone.bDefault)
		{
			Fallback = Zone.Color;
			continue;
		}
		if (Point.X < Zone.BoundsMin.X || Point.Y < Zone.BoundsMin.Y
			|| Point.Z < Zone.BoundsMin.Z || Point.X > Zone.BoundsMax.X
			|| Point.Y > Zone.BoundsMax.Y || Point.Z > Zone.BoundsMax.Z)
		{
			continue;
		}
		const FVector Size = Zone.BoundsMax - Zone.BoundsMin;
		const double Volume = FMath::Abs(static_cast<double>(Size.X)
			* static_cast<double>(Size.Y) * static_cast<double>(Size.Z));
		if (Volume < BestVolume)
		{
			Best = Zone.Color;
			BestVolume = Volume;
		}
	}
	FLinearColor Result = BestVolume < TNumericLimits<double>::Max() ? Best : Fallback;
	Result.A = 1.0f;
	return Result;
}
}

bool UWoTMeshLight::ClassifyRegion(const UWoTMapLightTable* Table,
	const FVector& Point, int32& OutZone, int32& OutLeaf)
{
	OutZone = 0;
	OutLeaf = INDEX_NONE;
	if (!IsValid(Table))
	{
		return false;
	}
	const int32 NodeCount = Table->ZoneBspPlanes.Num();
	if (NodeCount <= 0 || Table->ZoneBspFront.Num() != NodeCount
		|| Table->ZoneBspBack.Num() != NodeCount)
	{
		return false;
	}

	int32 NodeIndex = 0;
	for (int32 StepIndex = 0; StepIndex < NodeCount; ++StepIndex)
	{
		if (!Table->ZoneBspPlanes.IsValidIndex(NodeIndex))
		{
			return false;
		}
		const FVector4& Plane = Table->ZoneBspPlanes[NodeIndex];
		const double Distance = Point.X * Plane.X + Point.Y * Plane.Y
			+ Point.Z * Plane.Z - Plane.W;
		const bool bFront = Distance >= 0.0;
		const int32 Next = bFront ? Table->ZoneBspFront[NodeIndex]
			: Table->ZoneBspBack[NodeIndex];
		if (Next < 0)
		{
			OutZone = -Next - 1;
			const int32 LeafSlot = NodeIndex * 2 + (bFront ? 1 : 0);
			if (Table->LeafBspLeaves.IsValidIndex(LeafSlot))
			{
				OutLeaf = Table->LeafBspLeaves[LeafSlot];
			}
			return true;
		}
		NodeIndex = Next;
	}
	return false;
}

FLinearColor UWoTMeshLight::ZoneAmbientAt(const UWoTMapLightTable* Table,
	const FVector& Point)
{
	if (!IsValid(Table))
	{
		return FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
	}

	const int32 NodeCount = Table->ZoneBspPlanes.Num();
	const bool bHasExactTree = NodeCount > 0
		&& Table->ZoneBspFront.Num() == NodeCount
		&& Table->ZoneBspBack.Num() == NodeCount;
	if (!bHasExactTree)
	{
		return LegacyZoneAmbientAt(Table, Point);
	}

	int32 ZoneNumber = 0;
	int32 LeafIndex = INDEX_NONE;
	if (!ClassifyRegion(Table, Point, ZoneNumber, LeafIndex))
	{
		return LegacyZoneAmbientAt(Table, Point);
	}

	const FWoTZoneAmbient* DefaultAmbient = nullptr;
	const FWoTZoneAmbient* ZoneAmbient = nullptr;
	for (const FWoTZoneAmbient& AmbientRec : Table->ZoneAmbients)
	{
		if (AmbientRec.bDefault)
		{
			DefaultAmbient = &AmbientRec;
		}
		else if (ZoneNumber != 0 && AmbientRec.Zone == ZoneNumber)
		{
			ZoneAmbient = &AmbientRec;
		}
	}
	FLinearColor Result = ZoneAmbient ? ZoneAmbient->Color
		: DefaultAmbient ? DefaultAmbient->Color : FLinearColor::Black;
	Result.A = 1.0f;
	return Result;
}

int32 UWoTMeshLight::LeafAt(const UWoTMapLightTable* Table,
	const FVector& Point)
{
	int32 ZoneNumber = 0;
	int32 LeafIndex = INDEX_NONE;
	return ClassifyRegion(Table, Point, ZoneNumber, LeafIndex)
		? LeafIndex : INDEX_NONE;
}

bool UWoTMeshLight::BspLineCheck(const UWoTMapLightTable* Table,
	const FVector& Start, const FVector& End)
{
	if (!IsValid(Table))
	{
		return false;
	}
	const int32 NodeCount = Table->ZoneBspPlanes.Num();
	if (NodeCount == 0)
	{
		return Table->bBspRootOutside;
	}
	if (Table->ZoneBspFront.Num() != NodeCount
		|| Table->ZoneBspBack.Num() != NodeCount
		|| Table->LeafBspCsg.Num() != NodeCount)
	{
		return false;
	}
	int32 Budget = NodeCount * 4 + 1;
	// The retail zero-extent routine receives End=light, Start=actor and
	// recursively visits the start side first. Its +/-0.001 overlap and
	// ChildOutside transitions are reproduced exactly here.
	return LineCheckNode(Table, 0, 0, End, Start,
		Table->bBspRootOutside, Budget, nullptr);
}

int32 UWoTMeshLight::FirstHitNode(const UWoTMapLightTable* Table,
	const FVector& Start, const FVector& End)
{
	if (!IsValid(Table))
	{
		return INDEX_NONE;
	}
	const int32 NodeCount = Table->ZoneBspPlanes.Num();
	if (NodeCount <= 0 || Table->ZoneBspFront.Num() != NodeCount
		|| Table->ZoneBspBack.Num() != NodeCount
		|| Table->LeafBspCsg.Num() != NodeCount)
	{
		return INDEX_NONE;
	}
	int32 HitNode = INDEX_NONE;
	int32 Budget = NodeCount * 4 + 1;
	// UE1's BSP line check: iHit starts at root, recursion visits the Start
	// side first, and each split stamps iHit before descending the far side.
	// The same +/-0.001 overlap and ChildOutside transitions as BspLineCheck
	// therefore recover retail Hit.Item without involving UE5 collision.
	LineCheckNode(Table, 0, 0, End, Start,
		Table->bBspRootOutside, Budget, &HitNode);
	return HitNode;
}

float UWoTMeshLight::FrictionForNode(const UWoTMapLightTable* Table,
	int32 NodeIndex)
{
	if (!IsValid(Table) || !Table->ZoneBspPlanes.IsValidIndex(NodeIndex)
		|| Table->FrictionNodeIndices.Num() != Table->FrictionNodeValues.Num())
	{
		return 1.0f;
	}
	// zones.py emits ascending node indices. LowerBound keeps lookup O(log N)
	// while the channel stays sparse (five authored textures game-wide).
	const int32 SparseIndex = Algo::LowerBound(Table->FrictionNodeIndices, NodeIndex);
	return Table->FrictionNodeIndices.IsValidIndex(SparseIndex)
		&& Table->FrictionNodeIndices[SparseIndex] == NodeIndex
		? Table->FrictionNodeValues[SparseIndex] : 1.0f;
}

int32 UWoTMeshLight::LightingTagAt(const FWoTLightRec& Light,
	const FVector& Location)
{
	const float Radius = Light.Radius > 0.0f ? Light.Radius
		: Light.InvRadius > 0.0f ? 1.0f / Light.InvRadius : 0.0f;
	if (Radius <= 0.0)
	{
		return 0;
	}
	const float DistanceSquared = RetailDistanceSquared(Location, Light.Pos);
	const float Value = (1.0f - RetailSqrtApprox(DistanceSquared) / Radius)
		* static_cast<float>(Light.Brightness) * 1024.0f;
	return FMath::FloorToInt(Value + 0.5f);
}

TArray<int32> UWoTMeshLight::SelectLeafLights(
	const UWoTMapLightTable* Table, const FVector& Location, int32 LeafIndex,
	int32 MaxLights, bool bSpecialLit, TFunctionRef<bool(int32)> IsVisible)
{
	TArray<int32> Selected;
	if (!IsValid(Table) || LeafIndex < 0
		|| !Table->LeafLightOffsets.IsValidIndex(LeafIndex + 1))
	{
		return Selected;
	}
	const int32 RunStart = Table->LeafLightOffsets[LeafIndex];
	const int32 RunEnd = Table->LeafLightOffsets[LeafIndex + 1];
	if (RunStart < 0 || RunEnd < RunStart
		|| RunEnd > Table->LeafLightIndices.Num())
	{
		return Selected;
	}

	TArray<FWoTLightCandidate> Candidates;
	Candidates.Reserve(RunEnd - RunStart);
	for (int32 RunIndex = RunStart; RunIndex < RunEnd; ++RunIndex)
	{
		const int32 LightIndex = Table->LeafLightIndices[RunIndex];
		if (!Table->Lights.IsValidIndex(LightIndex))
		{
			continue;
		}
		const FWoTLightRec& Light = Table->Lights[LightIndex];
		const float Radius = Light.Radius > 0.0f ? Light.Radius
			: Light.InvRadius > 0.0f ? 1.0f / Light.InvRadius : 0.0f;
		if (Light.bSpecialLit != bSpecialLit || Radius <= 0.0)
		{
			continue;
		}
		const float DistanceSquared = RetailDistanceSquared(Location, Light.Pos);
		if (DistanceSquared >= Radius * Radius)
		{
			continue;
		}
		FWoTLightCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.LightIndex = LightIndex;
		Candidate.LightingTag = LightingTagAt(Light, Location);
	}
	Candidates.StableSort([](const FWoTLightCandidate& A,
		const FWoTLightCandidate& B)
	{
		return A.LightingTag > B.LightingTag;
	});

	const int32 Cap = FMath::Clamp(MaxLights, 1, 16);
	Selected.Reserve(FMath::Min(Cap, Candidates.Num()));
	int32 Threshold = INDEX_NONE;
	int32 NumRealVisible = 0;
	for (const FWoTLightCandidate& Candidate : Candidates)
	{
		if (Candidate.LightingTag < Threshold || NumRealVisible >= Cap)
		{
			break;
		}
		if (!IsVisible(Candidate.LightIndex))
		{
			continue;
		}
		if (Threshold == INDEX_NONE)
		{
			Threshold = Candidate.LightingTag / 8;
		}
		// Retail increments NumRealVisible and establishes Threshold before
		// AddLight rejects LT_None. Lightning-gated rows therefore affect later
		// selection without reaching the material arrays.
		++NumRealVisible;
		if (Table->Lights[Candidate.LightIndex].bEmits)
		{
			Selected.Add(Candidate.LightIndex);
		}
	}
	return Selected;
}

TArray<int32> UWoTMeshLight::SelectMoverSurfaceLights(
	const UWoTMapLightTable* Table, int32 LeafIndex,
	const FVector& SurfaceOrigin, const FVector& SurfaceNormal,
	bool bSpecialLit)
{
	TArray<int32> Selected;
	if (!IsValid(Table) || LeafIndex < 0
		|| !Table->LeafLightOffsets.IsValidIndex(LeafIndex + 1))
	{
		return Selected;
	}
	const int32 RunStart = Table->LeafLightOffsets[LeafIndex];
	const int32 RunEnd = Table->LeafLightOffsets[LeafIndex + 1];
	if (RunStart < 0 || RunEnd < RunStart
		|| RunEnd > Table->LeafLightIndices.Num())
	{
		return Selected;
	}
	Selected.Reserve(RunEnd - RunStart);
	for (int32 RunIndex = RunStart; RunIndex < RunEnd; ++RunIndex)
	{
		const int32 LightIndex = Table->LeafLightIndices[RunIndex];
		if (!Table->Lights.IsValidIndex(LightIndex))
		{
			continue;
		}
		const FWoTLightRec& Light = Table->Lights[LightIndex];
		if (Light.bSpecialLit != bSpecialLit
			|| FVector::DotProduct(Light.Pos - SurfaceOrigin, SurfaceNormal) <= 0.0)
		{
			continue;
		}
		// SetupMoverSurface's only consider gates are above. AddLight itself
		// rejects zero-brightness and resting LT_None/Lightning-gated actors
		// before a material row. Keep the brightness check defensive for tables
		// generated before bEmits folded that gate in.
		if (Light.Brightness != 0 && Light.bEmits)
		{
			Selected.Add(LightIndex);
		}
	}
	return Selected;
}

UWoTMeshLight* UWoTMeshLight::AttachRuntime(AActor* Owner,
	UWoTMapLightTable* TableOverride)
{
	if (!IsValid(Owner))
	{
		return nullptr;
	}

	TArray<UWoTMeshLight*> Components;
	Owner->GetComponents<UWoTMeshLight>(Components);
	for (UWoTMeshLight* Component : Components)
	{
		if (IsValid(Component) && Component->bBaked)
		{
			return Component;
		}
	}

	UWoTMapLightTable* Table = IsValid(TableOverride)
		? TableOverride : FindMapLightTable(Owner->GetWorld());
	if (!IsValid(Table))
	{
		return Components.IsEmpty() ? nullptr : Components[0];
	}

	UWoTMeshLight* Component = Components.IsEmpty() ? nullptr : Components[0];
	if (!IsValid(Component))
	{
		Component = NewObject<UWoTMeshLight>(Owner, TEXT("RuntimeMeshLight"));
		Owner->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}
	Component->StartRuntimeBake(Table);
	return Component;
}

namespace
{
struct FWoTRuntimeLightTableEntry
{
	TWeakObjectPtr<const UWorld> World;
	TWeakObjectPtr<UWoTMapLightTable> Table;
};

struct FWoTMapLightTableCacheEntry
{
	TWeakObjectPtr<const UWorld> World;
	TWeakObjectPtr<UWoTMapLightTable> Table;
	bool bKnownMiss = false;
};

TArray<FWoTMapLightTableCacheEntry>& WoTMapLightTableCache()
{
	static TArray<FWoTMapLightTableCacheEntry> Cache;
	return Cache;
}

#if WITH_AUTOMATION_TESTS
int32 GWoTMapLightTableLoadAttempts = 0;
#endif

TArray<FWoTRuntimeLightTableEntry>& WoTRuntimeLightTableBindings()
{
	static TArray<FWoTRuntimeLightTableEntry> Bindings;
	return Bindings;
}
}

UWoTMapLightTable* UWoTMeshLight::FindMapLightTable(const UWorld* World)
{
	if (!IsValid(World))
	{
		return nullptr;
	}

	TArray<FWoTRuntimeLightTableEntry>& Runtime = WoTRuntimeLightTableBindings();
	for (int32 Index = Runtime.Num() - 1; Index >= 0; --Index)
	{
		if (!Runtime[Index].World.IsValid() || !Runtime[Index].Table.IsValid())
		{
			Runtime.RemoveAtSwap(Index);
			continue;
		}
		if (Runtime[Index].World.Get() == World) return Runtime[Index].Table.Get();
	}

	TArray<FWoTMapLightTableCacheEntry>& Cache = WoTMapLightTableCache();
	for (int32 Index = Cache.Num() - 1; Index >= 0; --Index)
	{
		if (!Cache[Index].World.IsValid())
		{
			Cache.RemoveAtSwap(Index);
			continue;
		}
		if (Cache[Index].World.Get() == World)
		{
			if (UWoTMapLightTable* Cached = Cache[Index].Table.Get())
			{
				return Cached;
			}
			if (Cache[Index].bKnownMiss) return nullptr;
			// A positive entry whose weak object expired may be reloaded.
			Cache.RemoveAtSwap(Index);
			break;
		}
	}

	const FString MapName = UWorld::RemovePIEPrefix(
		FPackageName::GetShortName(World->GetPackage()->GetName()));
	const FString AssetName = FString::Printf(TEXT("LT_%s"), *MapName);
	const FSoftObjectPath AssetPath(FString::Printf(
		TEXT("/Game/WoT/Maps/LightTables/%s.%s"), *AssetName, *AssetName));
#if WITH_AUTOMATION_TESTS
	++GWoTMapLightTableLoadAttempts;
#endif
	UWoTMapLightTable* Table = Cast<UWoTMapLightTable>(AssetPath.TryLoad());
	FWoTMapLightTableCacheEntry& Entry = Cache.AddDefaulted_GetRef();
	Entry.World = World;
	Entry.Table = Table;
	Entry.bKnownMiss = Table == nullptr;
	return Table;
}

void UWoTMeshLight::RegisterRuntimeMapLightTable(
	const UWorld* World, UWoTMapLightTable* Table)
{
	if (!IsValid(World) || !IsValid(Table)) return;
	TArray<FWoTRuntimeLightTableEntry>& Bindings = WoTRuntimeLightTableBindings();
	for (int32 Index = Bindings.Num() - 1; Index >= 0; --Index)
	{
		if (!Bindings[Index].World.IsValid()
			|| Bindings[Index].World.Get() == World)
		{
			Bindings.RemoveAtSwap(Index);
		}
	}
	FWoTRuntimeLightTableEntry& Entry = Bindings.AddDefaulted_GetRef();
	Entry.World = World;
	Entry.Table = Table;
}

#if WITH_AUTOMATION_TESTS
void UWoTMeshLight::ResetMapLightTableCacheForTesting()
{
	WoTMapLightTableCache().Reset();
	GWoTMapLightTableLoadAttempts = 0;
}

int32 UWoTMeshLight::MapLightTableLoadAttemptsForTesting()
{
	return GWoTMapLightTableLoadAttempts;
}
#endif

void UWoTMeshLight::StartRuntimeBake(UWoTMapLightTable* Table)
{
	if (!IsValid(Table) || !IsValid(GetOwner()))
	{
		return;
	}
	RuntimeTable = Table;
	WaveMeans = Table->WaveMeans;
	WaveAmplitudes = Table->WaveAmplitudes;
	bRuntime = true;
	BakeRuntime();
	if (!bStaticMoverSurfaceLighting)
	{
		ArmRuntimeTimer();
	}
}

void UWoTMeshLight::ForceRuntimeBake()
{
	if (bRuntime && IsValid(RuntimeTable))
	{
		BakeRuntime();
	}
	else if (bBaked)
	{
		// Component-owned runtime mesh MIDs can be rebuilt for a skin change.
		// Reapply an authored/static bake as well as live table-driven bakes.
		PushMaterialParameters(false);
	}
}

void UWoTMeshLight::NotifyOwnerTeleported()
{
	ForceRuntimeBake();
}

void UWoTMeshLight::BeginPlay()
{
	Super::BeginPlay();
	if (bRuntime && IsValid(RuntimeTable))
	{
		if (!bBaked)
		{
			BakeRuntime();
		}
		else
		{
			PushMaterialParameters(true);
		}
		if (!bStaticMoverSurfaceLighting)
		{
			ArmRuntimeTimer();
		}
		return;
	}
	if (!bBaked)
	{
		return;
	}
	PushMaterialParameters(false);
}

void UWoTMeshLight::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RuntimeBakeTimer);
	}
	Super::EndPlay(EndPlayReason);
}

void UWoTMeshLight::ArmRuntimeTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(RuntimeBakeTimer, this,
			&UWoTMeshLight::CheckRuntimeMovement, RuntimeBakeInterval, true);
	}
}

void UWoTMeshLight::CheckRuntimeMovement()
{
	const AActor* Owner = GetOwner();
	if (!bRuntime || !IsValid(RuntimeTable) || !IsValid(Owner))
	{
		return;
	}
	// The initial bake ran during component BeginPlay, BEFORE the owning
	// actor's BeginPlay body bound its final mesh/materials (faction skins,
	// weave visuals). The first timer fire re-bakes unconditionally so those
	// late binds get the MeshLit MIDs and parameters.
	if (!bPostSpawnRebakeDone)
	{
		bPostSpawnRebakeDone = true;
		BakeRuntime();
		return;
	}
	const FVector Location = Owner->GetActorLocation();
	const FRotator Rotation = Owner->GetActorRotation();
	int32 ZoneNumber = 0;
	int32 LeafIndex = INDEX_NONE;
	ClassifyRegion(RuntimeTable, Location, ZoneNumber, LeafIndex);
	const bool bMoverSurface = !MoverSurfaces.IsEmpty();
	const bool bMoverSurfacePoseChanged = bMoverSurface
		&& (!Location.Equals(LastBakeLocation, KINDA_SMALL_NUMBER)
			|| !Rotation.Equals(LastBakeRotation, KINDA_SMALL_NUMBER));
	if (ZoneNumber != LastBakeZone || LeafIndex != LastBakeLeaf
		|| bMoverSurfacePoseChanged
		|| (!bMoverSurface
			&& FVector::DistSquared(Location, LastBakeLocation)
				> FMath::Square(RuntimeBakeMoveDistance)))
	{
		BakeRuntime();
	}
}

void UWoTMeshLight::BakeRuntime()
{
	AActor* Owner = GetOwner();
	if (!IsValid(Owner) || !IsValid(GetWorld()) || !IsValid(RuntimeTable))
	{
		return;
	}

	const FVector Location = Owner->GetActorLocation();
	const FRotator Rotation = Owner->GetActorRotation();
	int32 ZoneNumber = 0;
	int32 LeafIndex = INDEX_NONE;
	ClassifyRegion(RuntimeTable, Location, ZoneNumber, LeafIndex);
	ScaleGlow = ActorNumericProperty(Owner, TEXT("ScaleGlow"), ScaleGlow);
	const float AmbientGlowByte = ActorNumericProperty(Owner, TEXT("AmbientGlow"), 0.0f);
	if (ActorBoolProperty(Owner, TEXT("bUnlit"), false))
	{
		// UE1 draws unlit actors with the flat GUnlitColor — no lights, no
		// zone ambient (same rule the import-side bake applies). A bParticles
		// mesh multiplies it by ScaleGlow once more (UE1 mesh renderer).
		float Unlit = FMath::Clamp(
			ScaleGlow * 0.5f + AmbientGlowByte / 256.0f, 0.0f, 1.0f);
		if (ActorBoolProperty(Owner, TEXT("bParticles"), false) && ScaleGlow != 1.0f)
		{
			Unlit = FMath::Clamp(Unlit * ScaleGlow, 0.0f, 1.0f);
		}
		LightPosR.Reset();
		LightColor.Reset();
		LightWaveGroup.Reset();
		LightDirCone.Reset();
		Ambient = FLinearColor(Unlit, Unlit, Unlit, 1.0f);
		bBaked = true;
		LastBakeLocation = Location;
		LastBakeZone = ZoneNumber;
		LastBakeLeaf = LeafIndex;
		PushMaterialParameters(true);
		return;
	}
	if (!MoverSurfaces.IsEmpty())
	{
		const bool bSpecialLit = ActorBoolProperty(
			Owner, TEXT("bSpecialLit"), false);
		const FTransform OwnerTransform = Owner->GetActorTransform();
		for (FWoTMoverSurfaceLight& Surface : MoverSurfaces)
		{
			const FVector SurfaceOrigin = OwnerTransform.TransformPosition(
				Surface.LocalOrigin);
			const FVector SurfaceNormal = OwnerTransform.TransformVectorNoScale(
				Surface.LocalNormal).GetSafeNormal();
			const TArray<int32> Selected = SelectMoverSurfaceLights(
				RuntimeTable, LeafIndex, SurfaceOrigin, SurfaceNormal, bSpecialLit);
			if (Selected.Num() > MaxMoverSurfaceLights)
			{
				UE_LOG(LogTemp, Error,
					TEXT("WOT MOVER-LIGHT OVERFLOW: %s surface %s selected %d lights; material contract is %d. Truncating loudly."),
					*Owner->GetPathName(), *Surface.MaterialSlot.ToString(),
					Selected.Num(), MaxMoverSurfaceLights);
			}
			const int32 MaterialCount = FMath::Min(
				Selected.Num(), MaxMoverSurfaceLights);
			Surface.LightPosR.Reset(MaterialCount);
			Surface.LightColor.Reset(MaterialCount);
			Surface.LightWaveGroup.Reset(MaterialCount);
			Surface.LightDirCone.Reset(MaterialCount);
			for (int32 SelectedIndex = 0; SelectedIndex < MaterialCount;
				++SelectedIndex)
			{
				const FWoTLightRec& Light = RuntimeTable->Lights[
					Selected[SelectedIndex]];
				Surface.LightPosR.Emplace(
					Light.Pos.X, Light.Pos.Y, Light.Pos.Z, Light.InvRadius);
				Surface.LightColor.Add(Light.Color);
				Surface.LightWaveGroup.Add(Light.WaveGroup);
				Surface.LightDirCone.Emplace(
					Light.Dir.X, Light.Dir.Y, Light.Dir.Z, Light.Sine);
			}
		}
		Ambient = ZoneAmbientAt(RuntimeTable, Location);
		Ambient.R *= MoverSurfaceAmbientScale;
		Ambient.G *= MoverSurfaceAmbientScale;
		Ambient.B *= MoverSurfaceAmbientScale;
		bBaked = true;
		LastBakeLocation = Location;
		LastBakeRotation = Rotation;
		LastBakeZone = ZoneNumber;
		LastBakeLeaf = LeafIndex;
		PushMaterialParameters(false);
		return;
	}
	const int32 ZoneMaxLights = RuntimeTable->ZoneMaxLightCounts.IsValidIndex(
		ZoneNumber) ? RuntimeTable->ZoneMaxLightCounts[ZoneNumber] : MaxMeshLights;
	const bool bSpecialLit = ActorBoolProperty(Owner, TEXT("bSpecialLit"), false);
	const TArray<int32> Selected = SelectLeafLights(RuntimeTable, Location,
		LeafIndex, ZoneMaxLights, bSpecialLit,
		[this, Location](int32 LightIndex)
		{
			return RuntimeTable->Lights.IsValidIndex(LightIndex)
				&& BspLineCheck(RuntimeTable, Location,
					RuntimeTable->Lights[LightIndex].Pos);
		});

	LightPosR.Reset(Selected.Num());
	LightColor.Reset(Selected.Num());
	LightWaveGroup.Reset(Selected.Num());
	LightDirCone.Reset(Selected.Num());
	for (const int32 LightIndex : Selected)
	{
		const FWoTLightRec& Light = RuntimeTable->Lights[LightIndex];
		LightPosR.Emplace(Light.Pos.X, Light.Pos.Y, Light.Pos.Z, Light.InvRadius);
		LightColor.Add(Light.Color);
		LightWaveGroup.Add(Light.WaveGroup);
		LightDirCone.Emplace(Light.Dir.X, Light.Dir.Y, Light.Dir.Z, Light.Sine);
	}

	Ambient = ZoneAmbientAt(RuntimeTable, Location);
	if (bStaticMoverSurfaceLighting)
	{
		Ambient.R *= MoverSurfaceAmbientScale;
		Ambient.G *= MoverSurfaceAmbientScale;
		Ambient.B *= MoverSurfaceAmbientScale;
	}
	else
	{
		const float AmbientGlowAdd = AmbientGlowByte / 255.0f;
		Ambient.R += AmbientGlowAdd;
		Ambient.G += AmbientGlowAdd;
		Ambient.B += AmbientGlowAdd;
	}
	bBaked = true;
	LastBakeLocation = Location;
	LastBakeZone = ZoneNumber;
	LastBakeLeaf = LeafIndex;
	PushMaterialParameters(!bStaticMoverSurfaceLighting);
}

void UWoTMeshLight::PushMaterialParameters(bool bReparent)
{
	AActor* Owner = GetOwner();
	if (!IsValid(Owner))
	{
		return;
	}
	const bool bStyleTranslucent = ActorBoolProperty(
		Owner, TEXT("bStyleTranslucent"), false);
	const uint8 ActorStyle = static_cast<uint8>(FMath::Clamp(
		FMath::RoundToInt(ActorNumericProperty(Owner, TEXT("Style"),
			bStyleTranslucent ? 3.0f : 1.0f)), 0, 255));
	const bool bMoverSurface = !MoverSurfaces.IsEmpty();
	const bool bUnlit = ActorBoolProperty(Owner, TEXT("bUnlit"), false);
	const bool bDynamicLightEligible =
		!bUnlit && !ActorBoolProperty(Owner, TEXT("bSpecialLit"), false);

	TArray<UMeshComponent*> Meshes;
	Owner->GetComponents<UMeshComponent>(Meshes);
	for (UMeshComponent* Mesh : Meshes)
	{
		// Sprite billboard quads stand for UE1 sprite actors, which the mesh renderer
		// never lights and whose own Style picks their masters. A composite
		// effect (TaintVisual: comets plus a separate STY_Modulated centre
		// sprite) must not re-master the sprite to the comets' actor Style.
		if (!IsValid(Mesh) || Cast<UWoTSpriteBillboardComponent>(Mesh->GetAttachParent()))
		{
			continue;
		}
		TArray<FName> SlotNames;
		if (bMoverSurface)
		{
			SlotNames = Mesh->GetMaterialSlotNames();
		}
		for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
		{
			const FName SlotName = SlotNames.IsValidIndex(Slot)
				? SlotNames[Slot] : NAME_None;
			const FWoTMoverSurfaceLight* MoverSurface = bMoverSurface
				? MoverSurfaces.FindByPredicate(
					[SlotName](const FWoTMoverSurfaceLight& Surface)
					{
						return Surface.MaterialSlot == SlotName;
					})
				: nullptr;
			if (bMoverSurface && !MoverSurface)
			{
				UE_LOG(LogTemp, Error,
					TEXT("WOT MOVER-LIGHT PLANE MISSING: %s material slot %s has no converter surface row"),
					*Owner->GetPathName(), *SlotName.ToString());
				continue;
			}
			UMaterialInterface* Material = Mesh->GetMaterial(Slot);
			if (!IsValid(Material))
			{
				continue;
			}
			const FString TargetRoot = WoTMeshLightTargetRoot(
				Material, ActorStyle, bUnlit);
			const EWoTRuntimeVertexMeshStyle EffectiveStyle = WoTMeshLightStyleForRoot(
				TargetRoot.IsEmpty() ? MaterialRootName(Material) : TargetRoot);

			UMaterialInstanceDynamic* Mid = nullptr;
			if (bReparent && !IsTargetMeshFamily(Material, ActorStyle, bUnlit))
			{
				UMaterialInterface* Master = MeshLitMasterFor(
					Material, ActorStyle, bUnlit);
				if (IsValid(Master))
				{
					UTexture* Texture = nullptr;
					FLinearColor KeyColor = FLinearColor::Black;
					Material->GetTextureParameterValue(
						FHashedMaterialParameterInfo(TEXT("Tex")), Texture);
					// Opaque MeshLit reconstructs palette index zero as
					// RGB*A + KeyColor*(1-A). Preserve the importer's per-skin
					// palette value whenever this path creates a replacement MID.
					Material->GetVectorParameterValue(
						FHashedMaterialParameterInfo(TEXT("KeyColor")), KeyColor);
					if (!IsValid(Texture))
					{
						// Interchange fallback chains name their texture
						// parameter differently; any sampled texture is the
						// skin for these single-texture meshes.
						TArray<UTexture*> Used;
						Material->GetUsedTextures(Used, EMaterialQualityLevel::Num,
							true, ERHIFeatureLevel::Num, true);
						Texture = Used.IsEmpty() ? nullptr : Used[0];
					}
					Mid = Mesh->CreateDynamicMaterialInstance(Slot, Master);
					if (IsValid(Mid) && IsValid(Texture))
					{
						Mid->SetTextureParameterValue(TEXT("Tex"), Texture);
					}
					if (IsValid(Mid))
					{
						Mid->SetVectorParameterValue(TEXT("KeyColor"), KeyColor);
					}
				}
			}
			if (!IsValid(Mid))
			{
				Mid = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(Slot));
			}
			if (!IsValid(Mid))
			{
				Mid = Mesh->CreateDynamicMaterialInstance(Slot, Material);
			}
			if (!IsValid(Mid))
			{
				continue;
			}
			if (EffectiveStyle == EWoTRuntimeVertexMeshStyle::Modulated)
			{
				// Retail's modulated path bypasses mesh lighting and fog entirely.
				continue;
			}
			Mid->SetVectorParameterValue(TEXT("MLAmbient"), Ambient);
			Mid->SetVectorParameterValue(TEXT("MLWaveMeans"), FLinearColor(
				WaveMeans.X, WaveMeans.Y, WaveMeans.Z, WaveMeans.W));
			Mid->SetVectorParameterValue(TEXT("MLWaveAmplitudes"), FLinearColor(
				WaveAmplitudes.X, WaveAmplitudes.Y,
				WaveAmplitudes.Z, WaveAmplitudes.W));
			if (MaterialRootName(Mid) == TEXT("M_WoT_MeshTranslucent"))
			{
				// M_WoT_MeshTranslucent's contract has no per-light inputs: the
				// complete source multiplier is MLAmbient. The lit translucent
				// family (M_WoT_MeshLitT) takes the lights like M_WoT_MeshLit.
				continue;
			}
			Mid->SetScalarParameterValue(TEXT("MLScaleGlow"), ScaleGlow);
			// All dynamic-light emitters are bSpecialLit=false. The same scalar gates
			// baked-unlit actors and special-lit mover/mesh receivers.
			Mid->SetScalarParameterValue(
				TEXT("MLDynEnable"), bDynamicLightEligible ? 1.0f : 0.0f);
			if (MoverSurface)
			{
				for (int32 LightIndex = 0;
					LightIndex < MaxMoverSurfaceLights; ++LightIndex)
				{
					const bool bHave = MoverSurface->LightPosR.IsValidIndex(LightIndex)
						&& MoverSurface->LightColor.IsValidIndex(LightIndex);
					const FVector4 P = bHave
						? MoverSurface->LightPosR[LightIndex]
						: FVector4(0, 0, 0, 0);
					Mid->SetVectorParameterValue(
						*FString::Printf(TEXT("MLPos%d"), LightIndex),
						FLinearColor(P.X, P.Y, P.Z, 0.0f));
					Mid->SetScalarParameterValue(
						*FString::Printf(TEXT("MLRad%d"), LightIndex), P.W);
					Mid->SetVectorParameterValue(
						*FString::Printf(TEXT("MLCol%d"), LightIndex),
						bHave ? MoverSurface->LightColor[LightIndex]
							: FLinearColor::Black);
					Mid->SetScalarParameterValue(
						*FString::Printf(TEXT("MLWaveGroup%d"), LightIndex),
						bHave && MoverSurface->LightWaveGroup.IsValidIndex(LightIndex)
							? static_cast<float>(
								MoverSurface->LightWaveGroup[LightIndex])
							: -1.0f);
					const FVector4 D = MoverSurface->LightDirCone.IsValidIndex(LightIndex)
						? MoverSurface->LightDirCone[LightIndex]
						: FVector4(0, 0, 0, -1);
					Mid->SetVectorParameterValue(
						*FString::Printf(TEXT("MLDir%d"), LightIndex),
						FLinearColor(D.X, D.Y, D.Z, 0.0f));
					Mid->SetScalarParameterValue(
						*FString::Printf(TEXT("MLSine%d"), LightIndex), D.W);
				}
				continue;
			}
			for (int32 LightIndex = 0; LightIndex < MaxMeshLights; ++LightIndex)
			{
				const bool bHave = LightPosR.IsValidIndex(LightIndex)
					&& LightColor.IsValidIndex(LightIndex);
				const FVector4 P = bHave ? LightPosR[LightIndex] : FVector4(0, 0, 0, 0);
				Mid->SetVectorParameterValue(
					*FString::Printf(TEXT("MLPos%d"), LightIndex),
					FLinearColor(P.X, P.Y, P.Z, 0.0f));
				Mid->SetScalarParameterValue(
					*FString::Printf(TEXT("MLRad%d"), LightIndex), P.W);
				Mid->SetVectorParameterValue(
					*FString::Printf(TEXT("MLCol%d"), LightIndex),
					bHave ? LightColor[LightIndex] : FLinearColor::Black);
				Mid->SetScalarParameterValue(
					*FString::Printf(TEXT("MLWaveGroup%d"), LightIndex),
					bHave && LightWaveGroup.IsValidIndex(LightIndex)
						? static_cast<float>(LightWaveGroup[LightIndex])
						: -1.0f);
				// Spotlight dir/cone for the surface master; default omni
				// (Sine < 0). MLDir carries the unit cone axis (xyz); MLSine the
				// cone Sine, in a separate scalar because a VectorParameter feeds
				// the Custom node as float3 (its alpha would be dropped).
				const FVector4 D = LightDirCone.IsValidIndex(LightIndex)
					? LightDirCone[LightIndex] : FVector4(0, 0, 0, -1);
				Mid->SetVectorParameterValue(
					*FString::Printf(TEXT("MLDir%d"), LightIndex),
					FLinearColor(D.X, D.Y, D.Z, 0.0f));
				Mid->SetScalarParameterValue(
					*FString::Printf(TEXT("MLSine%d"), LightIndex), D.W);
			}
		}
	}
}
