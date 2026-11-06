#include "WoTRainCyd.h"
#include "WoTRandom.h"

#include "WoTMover.h"
#include "AI/WoTPathNodeIterator.h"
#include "WoTSkyBox.h"
#include "Assets/WoTRuntimeAssets.h"
#include "World/WoTRuntimeBSPRenderer.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/PlayerController.h"
#include "Mesh/WoTRuntimeVertexMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"

#if WITH_AUTOMATION_TESTS
namespace
{
TFunction<void(float, float)> GRainCydStagedMotionObserver;
}

void WoTSetRainCydStagedMotionObserverForTesting(
	TFunction<void(float, float)> Observer)
{
	GRainCydStagedMotionObserver = MoveTemp(Observer);
}
#endif

float WoTRainCydUnlitColor(float ScaleGlow, uint8 AmbientGlow)
{
	// UE1 draws a bUnlit mesh with every vertex light = GUnlitColor
	// = Clamp(ScaleGlow*0.5 + AmbientGlow/256, 0, 1) (UE1's mesh renderer,
	// applied per PF_Unlit triangle); the original Glide driver's mesh path
	// is TEXTURE_TIMES_ITRGB, so
	// this is the texture multiplier before the STY_Translucent blend.
	return FMath::Clamp(ScaleGlow * 0.5f
		+ static_cast<float>(AmbientGlow) / 256.0f, 0.0f, 1.0f);
}

void WoTRainCydApplyUnlitColor(UMaterialInstanceDynamic* Material,
	float ScaleGlow, uint8 AmbientGlow)
{
	if (Material == nullptr) return;
	// M_WoT_MeshTranslucent exposes MLAmbient as a VECTOR parameter (default
	// white, Tools/import/import_map.py make_mesh_translucent_master): a
	// scalar write of the same name is silently ignored, which left the
	// placed rain (ScaleGlow 0.1 -> 0.05) at 1.0, i.e. 20x retail.
	const float Unlit = WoTRainCydUnlitColor(ScaleGlow, AmbientGlow);
	Material->SetVectorParameterValue(TEXT("MLAmbient"),
		FLinearColor(Unlit, Unlit, Unlit, 1.0f));
}

bool WoTRainCydIsVisBlockingActor(const AActor* Actor)
{
	// RainCyd.uc:90 traces with bTraceActors=False: execTrace then uses
	// TRACE_VisBlocking = TRACE_Level|TRACE_Movers: level geometry
	// and movers only. Level geometry is the runtime BSP world (guarded) or
	// the imported BSP StaticMeshActors (editor maps, import_map.py
	// _spawn_world_meshes); the camera-locked sky box is never collidable.
	if (Actor == nullptr) return false;
	if (Actor->IsA<AWoTMover>() || Actor->IsA<AWoTRuntimeBSPWorld>()) return true;
	return Actor->IsA<AStaticMeshActor>() && !Actor->IsA<AWoTSkyBox>();
}

bool WoTRainCydVisibleFrom(const FVector& RainLocation,
	const FVector& ViewLocation, float VisibilityRadius, float VisibilityHeight)
{
	const FVector Delta = RainLocation - ViewLocation;
	return (VisibilityRadius <= 0.0f
			|| FVector2D(Delta.X, Delta.Y).SizeSquared()
				< FMath::Square(VisibilityRadius))
		&& (VisibilityHeight <= 0.0f
			|| FMath::Abs(Delta.Z) < VisibilityHeight);
}

AWoTRainCyd::AWoTRainCyd()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
}

void AWoTRainCyd::ConfigureRuntimePresentation(FName InMeshName,
	FName InMeshPackage, const FSoftObjectPath& InSkin,
	const TMap<int32, FSoftObjectPath>& InSkinOverrides,
	uint8 InAmbientGlow, float InScaleGlow)
{
	RuntimeMeshName = InMeshName;
	RuntimeMeshPackage = InMeshPackage;
	RuntimeActorSkin = InSkin;
	RuntimeSkinOverrides = InSkinOverrides;
	RuntimeAmbientGlow = InAmbientGlow;
	RuntimeScaleGlow = InScaleGlow;
	if (HasActorBegunPlay()) EnsureRuntimeRenderState();
}

void AWoTRainCyd::EnsureRuntimeRenderState()
{
	// RainCyd is a locally materialized presentation actor.  A dedicated
	// server must not decode or allocate its transient render carrier.
	if (GetNetMode() == NM_DedicatedServer)
		return;

	if (RuntimeMesh == nullptr && !RuntimeMeshName.IsNone()
		&& !RuntimeMeshPackage.IsNone())
	{
		const FWoTRuntimeVertexMeshDataPtr Data =
			FWoTRuntimeVertexMesh::ResolveAndLoad(
				RuntimeMeshName.ToString(), RuntimeMeshPackage);
		const FWoTRuntimeVertexMeshPose* Pose =
			Data.IsValid() ? Data->GetFramePose(0) : nullptr;
		if (Data.IsValid() && Pose != nullptr
			&& !Data->GetSlots().IsEmpty())
		{
			UStaticMesh* Generated = NewObject<UStaticMesh>(
				this, NAME_None, RF_Transient);
			FMeshDescription Description;
			FStaticMeshAttributes Attributes(Description);
			Attributes.Register();
			auto Positions = Attributes.GetVertexPositions();
			auto Normals = Attributes.GetVertexInstanceNormals();
			auto Tangents = Attributes.GetVertexInstanceTangents();
			auto UVs = Attributes.GetVertexInstanceUVs();
			auto Slots = Attributes.GetPolygonGroupMaterialSlotNames();
			UVs.SetNumChannels(1);
			TArray<FPolygonGroupID> Groups;
			for (const FWoTRuntimeVertexMeshSlot& Slot : Data->GetSlots())
			{
				const FPolygonGroupID Group = Description.CreatePolygonGroup();
				const FName SlotName = Slot.Name.IsNone()
					? FName(*FString::Printf(TEXT("Rain_%d"), Groups.Num()))
					: Slot.Name;
				Slots[Group] = SlotName;
				Generated->GetStaticMaterials().Emplace(nullptr, SlotName);
				Groups.Add(Group);
			}
			bool bValid = true;
			for (const FWoTRuntimeVertexMeshTriangle& Triangle :
				Data->GetTriangles())
			{
				if (!Groups.IsValidIndex(Triangle.SectionIndex))
				{
					bValid = false;
					break;
				}
				FVertexInstanceID Instances[3];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 RenderIndex = Triangle.RenderVertices[Corner];
					if (!Pose->RenderPositions.IsValidIndex(RenderIndex)
						|| !Pose->RenderNormals.IsValidIndex(RenderIndex))
					{
						bValid = false;
						break;
					}
					const FVertexID Vertex = Description.CreateVertex();
					Positions[Vertex] = Pose->RenderPositions[RenderIndex];
					Instances[Corner] = Description.CreateVertexInstance(Vertex);
					Normals[Instances[Corner]] = Pose->RenderNormals[RenderIndex];
					Tangents[Instances[Corner]] =
						Pose->RenderTangents.IsValidIndex(RenderIndex)
						? Pose->RenderTangents[RenderIndex] : FVector3f::XAxisVector;
					UVs.Set(Instances[Corner], 0,
						Data->GetRenderUVs().IsValidIndex(RenderIndex)
						? Data->GetRenderUVs()[RenderIndex] : FVector2f::ZeroVector);
				}
				if (!bValid) break;
				Description.CreateTriangle(
					Groups[Triangle.SectionIndex], MakeArrayView(Instances, 3));
			}
			if (bValid)
			{
				FStaticMeshOperations::ComputeMikktTangents(Description, true);
				Generated->bAllowCPUAccess = true;
				Generated->NeverStream = true;
				UStaticMesh::FBuildMeshDescriptionsParams Params;
				Params.bFastBuild = true;
				Params.bCommitMeshDescription = false;
				Params.bMarkPackageDirty = false;
				Params.bBuildSimpleCollision = false;
				Params.bAllowCpuAccess = true;
				const TArray<const FMeshDescription*> Descriptions = {&Description};
				if (Generated->BuildFromMeshDescriptions(Descriptions, Params))
				{
					Generated->SetBodySetup(nullptr);
					RuntimeGeneratedMesh = Generated;
					// The neutral mesh master is cooked in every build (neutral support
					// closure); ResolveMaterialPath serves only the authenticated
					// MI_* derivation grammar and returns null for a master path in a
					// guarded build, which left the rain columns on the engine default
					// (lit, black) material. Load the master directly like WoTMeshFX.
					UMaterialInterface* Master = Cast<UMaterialInterface>(FSoftObjectPath(
						TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent.M_WoT_MeshTranslucent")).TryLoad());
					for (int32 SlotIndex = 0;
						SlotIndex < Data->GetSlots().Num(); ++SlotIndex)
					{
						const FWoTRuntimeVertexMeshSlot& Slot =
							Data->GetSlots()[SlotIndex];
						const FSoftObjectPath EmbeddedTexture =
							!Slot.TexturePackage.IsEmpty()
								&& !Slot.TextureName.IsEmpty()
							? FSoftObjectPath(FString::Printf(
								TEXT("/Game/WoT/Textures/%s/%s.%s"),
								*Slot.TexturePackage, *Slot.TextureName,
								*Slot.TextureName))
							: FSoftObjectPath();
						FSoftObjectPath TexturePath;
						if (const FSoftObjectPath* MultiSkin =
							RuntimeSkinOverrides.Find(Slot.TextureIndex))
							TexturePath = *MultiSkin;
						else if (Slot.TextureIndex != 0
							&& !EmbeddedTexture.IsNull())
							TexturePath = EmbeddedTexture;
						else if (!RuntimeActorSkin.IsNull())
							TexturePath = RuntimeActorSkin;
						else
							TexturePath = EmbeddedTexture;
						FWoTRuntimeTextureOptions Options;
						Options.bSRGB = false;
						Options.Filter = TF_Bilinear;
						UTexture2D* Texture = TexturePath.IsValid()
							? UWoTRuntimeAssets::ResolveTexturePath(TexturePath, Options)
							: nullptr;
						UMaterialInstanceDynamic* Material = Master
							? UMaterialInstanceDynamic::Create(Master, this) : nullptr;
						if (Material)
						{
							Material->SetTextureParameterValue(TEXT("Tex"), Texture);
							Material->SetScalarParameterValue(TEXT("UTile"), 4.0f);
							WoTRainCydApplyUnlitColor(Material,
								RuntimeScaleGlow, RuntimeAmbientGlow);
						}
						RuntimeTextures.Add(Texture);
						RuntimeMaterials.Add(Material);
					}
					TArray<UMaterialInterface*> Materials;
					for (UMaterialInterface* Material : RuntimeMaterials)
						Materials.Add(Material);
					SetRuntimeRenderMesh(Generated, Materials);
				}
			}
		}
	}
	for (UStaticMeshComponent* Component : RuntimeSegments)
		if (Component && Component->IsRegistered()) Component->MarkRenderStateDirty();
	if (RuntimeMesh && RuntimeMesh->IsRegistered())
	{
		RuntimeMesh->MarkRenderStateDirty();
		RuntimeMesh->MarkRenderTransformDirty();
	}
}

void AWoTRainCyd::SetRuntimeRenderMesh(
	UStaticMesh* InMesh, TConstArrayView<UMaterialInterface*> Materials)
{
	if (InMesh == nullptr || RuntimeMesh != nullptr) return;
	RuntimeMesh = NewObject<UStaticMeshComponent>(this,
		TEXT("AuthenticatedRainMesh"), RF_Transient);
	if (RuntimeMesh == nullptr) return;
	RuntimeMesh->SetupAttachment(Mesh);
	RuntimeMesh->SetStaticMesh(InMesh);
	RuntimeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RuntimeMesh->SetGenerateOverlapEvents(false);
	RuntimeMesh->SetCastShadow(false);
	for (int32 Index = 0; Index < Materials.Num(); ++Index)
		if (Materials[Index] != nullptr)
			RuntimeMesh->SetMaterial(Index, Materials[Index]);
	RuntimeMesh->RegisterComponent();
	Mesh->SetVisibility(false, false);
}

int32 AWoTRainCyd::RuntimeRenderMaterialCount() const
{
	return RuntimeMesh != nullptr ? RuntimeMesh->GetNumMaterials() : 0;
}

void AWoTRainCyd::SetFrontEndStagedMotion(
	float InYawDegrees, float InFallRate)
{
	if (HasActorBegunPlay() || !FMath::IsFinite(InYawDegrees)
		|| !FMath::IsFinite(InFallRate)) return;
	InitialYawDegrees = FMath::Fmod(InYawDegrees, 360.0f);
	if (InitialYawDegrees < 0.0f) InitialYawDegrees += 360.0f;
	StagedFallRate = FMath::Clamp(InFallRate, MinFallRate, MaxFallRate);
	bUseFrontEndStagedMotion = true;
}

bool AWoTRainCyd::SetFrontEndGroundHit(FVector InHit)
{
	if (HasActorBegunPlay() || !FMath::IsFinite(InHit.X)
		|| !FMath::IsFinite(InHit.Y) || !FMath::IsFinite(InHit.Z)) return false;
	FrontEndGroundHit = InHit;
	bUseFrontEndGroundHit = true;
	return true;
}

void AWoTRainCyd::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer)
	{
		SetActorHiddenInGame(true);
		SetActorTickEnabled(false);
		return;
	}
	EnsureRuntimeRenderState();
	// RainCyd.uc:77 — randomize yaw so neighboring cylinders' streaks differ.
	if (!bUseFrontEndStagedMotion)
		InitialYawDegrees = WoTRandom::FRand() * 360.0f;
	SetActorRotation(GetActorRotation()
		+ FRotator(0.0f, InitialYawDegrees, 0.0f));
	Down = -GetActorUpVector();
	FallRate = bUseFrontEndStagedMotion
		? StagedFallRate : WoTRandom::FRandRange(MinFallRate, MaxFallRate);
#if WITH_AUTOMATION_TESTS
	if (bUseFrontEndStagedMotion && GRainCydStagedMotionObserver)
		GRainCydStagedMotionObserver(InitialYawDegrees, FallRate);
#endif
	SegmentHeight = FMath::Max(
		1.0f, CylinderHeight * GetActorScale3D().Z);
	// RainCyd.uc:90-92 — world-only ground trace (bTraceActors=False), then
	// anchor the master one segment height above the hit, BuryDistance deep.
	FVector GroundHit = FrontEndGroundHit;
	bool bHasGroundHit = bUseFrontEndGroundHit;
	if (!bHasGroundHit)
	{
		// Object-type WorldStatic also matches every projected actor blocker
		// (AWoTCollisionBlocker's capsule is ECC_WorldStatic, WoTCollisionBlocker
		// .cpp:102): Arena_01's BlockPlayer0 (r=128) sits in the roof opening
		// 7uu under RainCyd22/23, so the columns anchored there and never fell
		// into the WaterZone4 room. Skip non-VisBlocking actors and re-trace.
		const FVector Start = GetActorLocation();
		FHitResult Hit;
		if (WoTVisBlockingTrace(GetWorld(), Start,
			Start + Down * TraceDepth, this, Hit))
		{
			bHasGroundHit = true;
			GroundHit = Hit.Location;
		}
	}
	if (bHasGroundHit)
		SetActorLocation(
			GroundHit - Down * SegmentHeight + Down * BuryDistance);
	BasePosition = GetActorLocation();
	// RainCyd.uc:94-113 — the master's stacked clones, as components: the
	// spawned segments copy the master's velocity, so relative spacing is
	// invariant and attachment models it exactly.
	for (int32 Segment = 1; Segment < StackNum; ++Segment)
	{
		USkeletalMeshComponent* Clone = NewObject<USkeletalMeshComponent>(this);
		Clone->SetSkeletalMeshAsset(Mesh->GetSkeletalMeshAsset());
		Clone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Clone->SetCastShadow(false);
		Clone->SetupAttachment(Mesh);
		Clone->SetRelativeLocation(FVector(0, 0, CylinderHeight * Segment));
		Clone->RegisterComponent();
		if (UMaterialInterface* Material = Mesh->GetMaterial(0))
		{
			Clone->SetMaterial(0, Material);
		}
		ExtraSegments.Add(Clone);
		if (RuntimeMesh != nullptr)
		{
			UStaticMeshComponent* RuntimeClone =
				NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			if (RuntimeClone != nullptr)
			{
				RuntimeClone->SetStaticMesh(RuntimeMesh->GetStaticMesh());
				RuntimeClone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				RuntimeClone->SetGenerateOverlapEvents(false);
				RuntimeClone->SetCastShadow(false);
				RuntimeClone->SetupAttachment(Mesh);
				RuntimeClone->SetRelativeLocation(
					FVector(0, 0, CylinderHeight * Segment));
				RuntimeClone->RegisterComponent();
				for (int32 MaterialIndex = 0;
					MaterialIndex < RuntimeMesh->GetNumMaterials(); ++MaterialIndex)
				{
					if (UMaterialInterface* RuntimeMaterial =
						RuntimeMesh->GetMaterial(MaterialIndex))
						RuntimeClone->SetMaterial(MaterialIndex, RuntimeMaterial);
				}
				RuntimeSegments.Add(RuntimeClone);
			}
		}
	}
	if (bUseFrontEndGroundHit)
	{
		FrontEndSegmentPositions.SetNum(FMath::Max(StackNum, 1));
		for (int32 Segment = 0;
			Segment < FrontEndSegmentPositions.Num(); ++Segment)
		{
			FrontEndSegmentPositions[Segment] =
				BasePosition - Down * (SegmentHeight * Segment);
			if (Segment > 0 && ExtraSegments.IsValidIndex(Segment - 1)
				&& ExtraSegments[Segment - 1] != nullptr)
			{
				ExtraSegments[Segment - 1]->SetWorldLocation(
					FrontEndSegmentPositions[Segment]);
			}
			if (Segment > 0 && RuntimeSegments.IsValidIndex(Segment - 1)
				&& RuntimeSegments[Segment - 1] != nullptr)
			{
				RuntimeSegments[Segment - 1]->SetWorldLocation(
					FrontEndSegmentPositions[Segment]);
			}
		}
	}
}

void AWoTRainCyd::CullSegmentsFrom(const FVector& ViewLocation)
{
	// RainCyd.uc:94-109 spawns every stack segment as its own RainCyd with
	// the master's VisibilityRadius/Height, so the renderer culls each
	// segment from its own location, not the whole column from the master.
	const bool bLimited = VisibilityRadius > 0.0f || VisibilityHeight > 0.0f;
	const auto CullSegment = [&](USceneComponent* Component,
		const FVector& SegmentLocation)
	{
		if (Component == nullptr) return;
		Component->SetHiddenInGame(bLimited && !WoTRainCydVisibleFrom(
			SegmentLocation, ViewLocation, VisibilityRadius, VisibilityHeight));
	};
	CullSegment(Mesh, GetActorLocation());
	CullSegment(RuntimeMesh, GetActorLocation());
	for (int32 Index = 0; Index < ExtraSegments.Num(); ++Index)
	{
		USkeletalMeshComponent* Segment = ExtraSegments[Index];
		if (Segment == nullptr) continue;
		const FVector SegmentLocation = Segment->GetComponentLocation();
		CullSegment(Segment, SegmentLocation);
		if (RuntimeSegments.IsValidIndex(Index))
		{
			CullSegment(RuntimeSegments[Index], SegmentLocation);
		}
	}
}

void AWoTRainCyd::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	bool bVisible = GetNetMode() != NM_DedicatedServer;
	const bool bLimited = VisibilityRadius > 0.0f || VisibilityHeight > 0.0f;
	FVector ViewLocation = FVector::ZeroVector;
	if (bVisible && bLimited)
	{
		FRotator ViewRotation = FRotator::ZeroRotator;
		const APlayerController* Player = GetWorld()
			? GetWorld()->GetFirstPlayerController() : nullptr;
		bVisible = Player != nullptr;
		if (Player != nullptr)
		{
			Player->GetPlayerViewPoint(ViewLocation, ViewRotation);
		}
	}
	SetActorHiddenInGame(!bVisible);
	if (bVisible)
	{
		CullSegmentsFrom(ViewLocation);
	}
	if (bUseFrontEndGroundHit && !FrontEndSegmentPositions.IsEmpty())
	{
		const FVector Travel = Down * (FallRate * DeltaSeconds);
		const FVector WrapBoundary =
			FrontEndGroundHit + Down * BuryDistance;
		const double ColumnHeight =
			static_cast<double>(FrontEndSegmentPositions.Num()) * SegmentHeight;
		auto VectorAproxEqual = [](const FVector& A, const FVector& B)
		{
			return FMath::Abs(A.X - B.X) < 1.0e-4
				&& FMath::Abs(A.Y - B.Y) < 1.0e-4
				&& FMath::Abs(A.Z - B.Z) < 1.0e-4;
		};
		for (FVector& Position : FrontEndSegmentPositions)
		{
			Position += Travel;
			const FVector DirectionToBoundary =
				(WrapBoundary - Position).GetSafeNormal(1.0e-8);
			if (!VectorAproxEqual(Down, DirectionToBoundary))
				Position -= Down * ColumnHeight;
		}
		SetActorLocation(FrontEndSegmentPositions[0]);
		for (int32 Segment = 1;
			Segment < FrontEndSegmentPositions.Num(); ++Segment)
		{
			if (ExtraSegments.IsValidIndex(Segment - 1)
				&& ExtraSegments[Segment - 1] != nullptr)
			{
				ExtraSegments[Segment - 1]->SetWorldLocation(
					FrontEndSegmentPositions[Segment]);
			}
			if (RuntimeSegments.IsValidIndex(Segment - 1)
				&& RuntimeSegments[Segment - 1] != nullptr)
			{
				RuntimeSegments[Segment - 1]->SetWorldLocation(
					FrontEndSegmentPositions[Segment]);
			}
		}
		return;
	}
	// The whole column falls; a wrap every SegmentHeight relabels segment i
	// as segment i+1 — identical to retail's per-segment jump of
	// StackNum*CylinderHeight (RainCyd.uc:130-133).
	Offset = FMath::Fmod(Offset + FallRate * DeltaSeconds, SegmentHeight);
	SetActorLocation(BasePosition + Down * Offset);
}
