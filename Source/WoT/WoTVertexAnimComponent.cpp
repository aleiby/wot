#include "WoTVertexAnimComponent.h"

#include "Assets/WoTRuntimeAssets.h"
#include "Mesh/WoTRuntimeVertexMesh.h"
#include "WoTAnimData.h"
#include "WoTCharacter.h"
#include "WoTMeshLight.h"
#include "WoTPayloadSubsystem.h"
#include "Animation/MorphTarget.h"
#include "Components/ExternalMorphSet.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialRenderProxy.h"
#include "Misc/App.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "PrimitiveViewRelevance.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "StaticMeshResources.h"
#include "DynamicMeshBuilder.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/RenderCommandPipes.h"
#if WITH_EDITOR
#include "Rendering/SkeletalMeshModel.h"
#endif

EWoTRuntimeVertexMeshStyle WoTRuntimeEffectiveMeshStyle(
	EWoTRuntimeVertexMeshStyle SlotStyle, uint8 ActorStyle)
{
	// Retail maps Actor.Style into mesh ExtraFlags,
	// unions those flags with each triangle, and resolves
	// conflicting blends Translucent > Modulated > Masked.
	if (SlotStyle == EWoTRuntimeVertexMeshStyle::Translucent || ActorStyle == 3)
	{
		return EWoTRuntimeVertexMeshStyle::Translucent;
	}
	if (SlotStyle == EWoTRuntimeVertexMeshStyle::Modulated || ActorStyle == 4)
	{
		return EWoTRuntimeVertexMeshStyle::Modulated;
	}
	if (SlotStyle == EWoTRuntimeVertexMeshStyle::Masked || ActorStyle == 2)
	{
		return EWoTRuntimeVertexMeshStyle::Masked;
	}
	return SlotStyle;
}

namespace
{
FName FrameMorphName(int32 GlobalFrame)
{
	return FName(*FString::Printf(TEXT("F%d"), GlobalFrame));
}

int32 FrameFromMorphName(FName MorphName)
{
	const FString Name = MorphName.ToString();
	return Name.Len() > 1 && Name[0] == TEXT('F')
		? FCString::Atoi(*Name.Mid(1)) : INDEX_NONE;
}

struct FWoTRuntimeVertexMeshDynamicData
{
	FWoTRuntimeVertexMeshPose Pose;
};

class FWoTRuntimeVertexMeshSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FWoTRuntimeVertexMeshSceneProxy(UWoTVertexAnimComponent* Component,
		const FWoTRuntimeVertexMeshDataPtr& Data,
		const FWoTRuntimeVertexMeshPose& Pose,
		TSharedPtr<FWoTVertexMeshDrawState, ESPMode::ThreadSafe> InDrawState)
		: FPrimitiveSceneProxy(Component)
		, VertexFactory(GetScene().GetFeatureLevel(),
			"FWoTRuntimeVertexMeshSceneProxy")
		, MaterialRelevance(Component->GetMaterialRelevance(
			GetScene().GetShaderPlatform()))
		, DrawState(MoveTemp(InDrawState))
	{
		TArray<FDynamicMeshVertex> Vertices;
		Vertices.SetNumUninitialized(Pose.RenderPositions.Num());
		for (int32 Index = 0; Index < Vertices.Num(); ++Index)
		{
			FDynamicMeshVertex& Vertex = Vertices[Index];
			Vertex.Position = Pose.RenderPositions[Index];
			Vertex.Color = FColor::White;
			Vertex.TextureCoordinate[0] = Data->GetRenderUVs()[Index];
			const FVector3f TangentX = Pose.RenderTangents[Index];
			const FVector3f TangentZ = Pose.RenderNormals[Index];
			Vertex.SetTangents(TangentX,
				FVector3f::CrossProduct(TangentZ, TangentX), TangentZ);
		}
		FRenderCommandPipe* Pipe = &UE::RenderCommandPipe::SkeletalMesh;
		// The modern helper owns buffer allocation, stream binding, SRV creation,
		// and vertex-factory initialization as one ordered render command.
		VertexBuffers.InitFromDynamicVertex(Pipe, &VertexFactory, Vertices);
		for (int32 SlotIndex = 0; SlotIndex < Data->GetSlots().Num(); ++SlotIndex)
		{
			const FWoTRuntimeVertexMeshSlot& Source = Data->GetSlots()[SlotIndex];
			FSection& Section = Sections.AddDefaulted_GetRef();
			Section.FirstIndex = IndexBuffer.Indices.Num();
			Section.NumPrimitives = Source.Indices.Num() / 3;
			Section.Material = Component->GetMaterial(SlotIndex);
			if (!Section.Material)
				Section.Material = UMaterial::GetDefaultMaterial(MD_Surface);
			IndexBuffer.Indices.Append(Source.Indices);
		}
		// Pose uploads use the skeletal-mesh command pipe. Resource creation must
		// use that same pipe: independent pipe replay can otherwise upload into a
		// buffer whose initialization is still queued on the default pipe.
		BeginInitResource(&IndexBuffer, Pipe);
	}

	~FWoTRuntimeVertexMeshSceneProxy() override
	{
		VertexBuffers.PositionVertexBuffer.ReleaseResource();
		VertexBuffers.StaticMeshVertexBuffer.ReleaseResource();
		VertexBuffers.ColorVertexBuffer.ReleaseResource();
		IndexBuffer.ReleaseResource();
		VertexFactory.ReleaseResource();
	}

	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	void SetPose_RenderThread(FRHICommandListBase& RHICmdList,
		FWoTRuntimeVertexMeshDynamicData* DynamicData)
	{
		if (!DynamicData
			|| DynamicData->Pose.RenderPositions.Num()
				!= static_cast<int32>(VertexBuffers.PositionVertexBuffer.GetNumVertices()))
		{
			delete DynamicData;
			return;
		}
		for (int32 Index = 0; Index < DynamicData->Pose.RenderPositions.Num(); ++Index)
		{
			VertexBuffers.PositionVertexBuffer.VertexPosition(Index) =
				DynamicData->Pose.RenderPositions[Index];
			const FVector3f TangentX = DynamicData->Pose.RenderTangents[Index];
			const FVector3f TangentZ = DynamicData->Pose.RenderNormals[Index];
			VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(Index, TangentX,
				FVector3f::CrossProduct(TangentZ, TangentX), TangentZ);
		}
		FPositionVertexBuffer& Positions = VertexBuffers.PositionVertexBuffer;
		void* PositionData = RHICmdList.LockBuffer(Positions.VertexBufferRHI, 0,
			Positions.GetNumVertices() * Positions.GetStride(), RLM_WriteOnly);
		FMemory::Memcpy(PositionData, Positions.GetVertexData(),
			Positions.GetNumVertices() * Positions.GetStride());
		RHICmdList.UnlockBuffer(Positions.VertexBufferRHI);
		FStaticMeshVertexBuffer& Tangents = VertexBuffers.StaticMeshVertexBuffer;
		void* TangentData = RHICmdList.LockBuffer(
			Tangents.TangentsVertexBuffer.VertexBufferRHI, 0,
			Tangents.GetTangentSize(), RLM_WriteOnly);
		FMemory::Memcpy(TangentData, Tangents.GetTangentData(),
			Tangents.GetTangentSize());
		RHICmdList.UnlockBuffer(Tangents.TangentsVertexBuffer.VertexBufferRHI);
		delete DynamicData;
	}

	void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily, uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override
	{
		bool bSubmitted = false;
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1u << ViewIndex)) == 0) continue;
			for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
			{
				const FSection& Section = Sections[SectionIndex];
				if (Section.NumPrimitives == 0) continue;
				FMeshBatch& Mesh = Collector.AllocateMesh();
				FMeshBatchElement& Element = Mesh.Elements[0];
				Element.IndexBuffer = &IndexBuffer;
				Element.FirstIndex = Section.FirstIndex;
				Element.NumPrimitives = Section.NumPrimitives;
				Element.MinVertexIndex = 0;
				Element.MaxVertexIndex =
					VertexBuffers.PositionVertexBuffer.GetNumVertices() - 1;
				FDynamicPrimitiveUniformBuffer& DynamicUniformBuffer =
					Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
				FPrimitiveUniformShaderParametersBuilder Builder;
				BuildUniformShaderParameters(Builder);
				DynamicUniformBuffer.Set(Collector.GetRHICommandList(), Builder);
				Element.PrimitiveUniformBufferResource =
					&DynamicUniformBuffer.UniformBuffer;
				Mesh.VertexFactory = &VertexFactory;
				Mesh.MaterialRenderProxy = Section.Material->GetRenderProxy();
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = true;
				Mesh.SegmentIndex = SectionIndex;
				Collector.AddMesh(ViewIndex, Mesh);
				bSubmitted = true;
			}
		}
		if (bSubmitted && DrawState.IsValid())
			DrawState->LastSubmittedFrame.Store(ViewFamily.FrameCounter);
	}

	FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = IsShown(View);
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bDynamicRelevance = true;
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		Result.bUsesLightingChannels =
			GetLightingChannelMask() != GetDefaultLightingChannelMask();
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque
			&& Result.bRenderInMainPass;
		return Result;
	}

	bool CanBeOccluded() const override
	{
		return !MaterialRelevance.bDisableDepthTest;
	}

	uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + GetAllocatedSize();
	}
	uint32 GetAllocatedSize() const
	{
		return FPrimitiveSceneProxy::GetAllocatedSize()
			+ Sections.GetAllocatedSize() + IndexBuffer.Indices.GetAllocatedSize();
	}

private:
	struct FSection
	{
		uint32 FirstIndex = 0;
		uint32 NumPrimitives = 0;
		UMaterialInterface* Material = nullptr;
	};
	FStaticMeshVertexBuffers VertexBuffers;
	FDynamicMeshIndexBuffer32 IndexBuffer;
	FLocalVertexFactory VertexFactory;
	TArray<FSection> Sections;
	FMaterialRelevance MaterialRelevance;
	TSharedPtr<FWoTVertexMeshDrawState, ESPMode::ThreadSafe> DrawState;
};
}

void UWoTVertexAnimComponent::StampOwnerAnimIntent(FName Sequence, float Rate, bool bLoopIntent)
{
	// Parity seam: when an AUTHORITY character drives its vertex anim, publish the
	// intent to the replicated channel so any future driver (the deferred anim state
	// machine, the AI stream, this slice's minimal driver) reaches simulated proxies
	// for free. Local playback is unchanged for non-authority / non-character owners;
	// on a proxy GetOwnerRole() != Authority, so OnRep_Anim's replay never re-stamps.
	if (GetOwnerRole() != ROLE_Authority)
	{
		return;
	}
	if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
	{
		Character->SetAnimIntent(Sequence, Rate, bLoopIntent);
	}
}

UWoTVertexAnimComponent::UWoTVertexAnimComponent()
{
	RuntimeDrawState = MakeShared<FWoTVertexMeshDrawState, ESPMode::ThreadSafe>();
	PrimaryComponentTick.bCanEverTick = true;
	// morph curves must evaluate every frame even with no AnimInstance
	VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	// UE1 actor-lighting split (import_map.py place_semantics): world BSP is
	// the channel-0 baked-atlas material; the placed WoT PointLights are
	// channel-1-ONLY. Every vertex-anim mesh is a UE1 "mesh actor" and must
	// receive those lights — a receiver left on the default channel 0 alone
	// gets none of them and renders black (pawn bodies and every runtime
	// visual: projectile weave meshes, carcasses, gibs, fragments, beam and
	// Mashadar segments, trap visuals).
	LightingChannels.bChannel1 = true;
}

void UWoTVertexAnimComponent::SetFatness(const uint8 InFatness)
{
	if (Fatness == InFatness) return;
	Fatness = InFatness;
	PublishRuntimePose();
	UpdateBounds();
	MarkRenderTransformDirty();
}

bool UWoTVertexAnimComponent::BindRuntimeVertexMesh(
	FWoTRuntimeVertexMeshDataPtr Data)
{
	if (!Data.IsValid()) return false;
	ClearExternalMorphInjection();
	ClearPoseCache();
	Super::SetSkeletalMesh(nullptr, true);
	RuntimeVertexMesh = MoveTemp(Data);
	RuntimeDrawState->LastSubmittedFrame.Store(0);
	// MultiSkins belongs to the old mesh's material-slot layout. An ordinary
	// bind starts from the newly decoded mesh's authored skins; explicit copy or
	// disguise restoration reapplies a captured map after this call.
	RuntimeSkinOverrides.Reset();
	RuntimeActorSkin.Reset();
	VertexMeshLogicalId = RuntimeVertexMesh->GetMeshLogicalId();
	AnimationLogicalId = RuntimeVertexMesh->GetAnimationLogicalId();
	Sequences = RuntimeVertexMesh->GetSequences();
	Current = NAME_None;
	LocalFrame = 0.0f;
	bPlaying = false;
	bTweening = false;
	bPlayAfterTween = false;
	ActiveMorphs.Reset();
	SetCurrentPoseWeights(0, 1.0f, INDEX_NONE, 0.0f);
	if (!RuntimePoseStorage)
		RuntimePoseStorage = new FWoTRuntimeVertexMeshPose();
	PublishRuntimePose();
	BuildRuntimeMaterials();
	UpdateBounds();
	MarkRenderStateDirty();
	EnsureRuntimeRenderState();
	RebakeOwnerMeshLightAfterLateBind();
	return RuntimePoseStorage && !RuntimePoseStorage->RenderPositions.IsEmpty();
}

void UWoTVertexAnimComponent::RebakeOwnerMeshLightAfterLateBind()
{
	// BeginPlay attaches and bakes the owner's mesh light against whatever mesh
	// is bound then. Weave and FX actors bind theirs later, in the actor's own
	// BeginPlay, so the new skin kept its asset defaults until the timer's
	// post-spawn re-bake (0.25 s): the Fireball LavaRock (lit, ScaleGlow 5,
	// AmbientGlow 60) drew black for most of its 0.5 s life.
	if (!HasBegunPlay())
	{
		return;
	}
	if (AActor* Owner = GetOwner())
	{
		if (UWoTMeshLight* MeshLight = Owner->FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->ForceRuntimeBake();
		}
	}
}

FWoTRuntimeVertexMeshDataPtr UWoTVertexAnimComponent::SelectRuntimeVertexMesh(
	FWoTRuntimeVertexMeshDataPtr Data) const
{
	return bMeshCurvy
		? FWoTRuntimeVertexMesh::MakeCurvy(Data, 2)
		: Data;
}

#if WITH_AUTOMATION_TESTS
bool UWoTVertexAnimComponent::IsRuntimeMeshCurvyForTesting() const
{
	return RuntimeVertexMesh.IsValid()
		&& RuntimeVertexMesh->GetCurvyLevels() > 0;
}
#endif

void UWoTVertexAnimComponent::EnsureRuntimeRenderState()
{
	// Projected map actors use UWorld::SpawnActor with deferred construction.
	// Their components can already be registered with no skeletal carrier when
	// PostInitializeComponents binds the authenticated runtime mesh. A dirty
	// mark only recreates an existing render state; it does not create the
	// missing state, leaving the component registered but permanently absent
	// from the scene. Ordinary SpawnActorDeferred gallery actors bind before
	// registration, which is why the same mesh appeared there. Complete the
	// registered late-bind transition explicitly, as SetSkeletalMesh does for
	// the inverse runtime-to-carrier transition below.
	UWorld* World = GetWorld();
	if (World && World->Scene && IsRegistered() && !IsRenderStateCreated()
		&& !GUsingNullRHI && FApp::CanEverRender())
	{
		CreateRenderState_Concurrent(nullptr);
	}
}

void UWoTVertexAnimComponent::ClearRuntimeVertexMesh()
{
	if (!RuntimeVertexMesh.IsValid())
	{
		return;
	}
	RuntimeVertexMesh.Reset();
	VertexMeshLogicalId.Empty();
	AnimationLogicalId.Empty();
	RuntimeDefaultMaterials.Reset();
	RuntimeMaterialInstances.Reset();
	RuntimeSkinOverrides.Reset();
	RuntimeActorSkin.Reset();
	OverrideMaterials.Reset();
	Sequences.Reset();
	Current = NAME_None;
	LocalFrame = 0.0f;
	bPlaying = false;
	bTweening = false;
	bPlayAfterTween = false;
	ActiveMorphs.Reset();
	CurrentPoseWeights.Reset();
	CurrentPoseGlobalA = INDEX_NONE;
	CurrentPoseGlobalB = INDEX_NONE;
	CurrentPoseWeightA = 0.0f;
	CurrentPoseWeightB = 0.0f;
	delete RuntimePoseStorage;
	RuntimePoseStorage = nullptr;
}

bool UWoTVertexAnimComponent::SetVertexMesh(FName MeshName, FName PackageHint)
{
	FString Name = MeshName.ToString();
	int32 Dot = INDEX_NONE;
	if (Name.FindLastChar(TEXT('.'), Dot))
	{
		if (PackageHint.IsNone()) PackageHint = FName(*Name.Left(Dot));
		Name.RightChopInline(Dot + 1);
	}
	if (Name.IsEmpty()) return false;
#if WOT_GUARDED_BOOTSTRAP
	return BindRuntimeVertexMesh(SelectRuntimeVertexMesh(
		FWoTRuntimeVertexMesh::ResolveAndLoad(Name, PackageHint)));
#else
	auto TryLoadMesh = [this](const FString& AssetName)
	{
		const FSoftObjectPath MeshPath(FString::Printf(
			TEXT("/Game/WoT/Meshes/%s/%s/SkeletalMeshes/%s.%s"),
			*AssetName, *AssetName, *AssetName, *AssetName));
		if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(MeshPath.TryLoad()))
		{
			SetSkeletalMesh(Mesh);
			return true;
		}
		return false;
	};
	if (bMeshCurvy)
	{
		const FString CurvyName = Name + TEXT("_Curvy");
		if (TryLoadMesh(CurvyName)) return true;
		UE_LOG(LogTemp, Warning,
			TEXT("VertexAnim: curvy mesh %s is unavailable; using flat %s"),
			*CurvyName, *Name);
	}
	return TryLoadMesh(Name);
#endif
}

bool UWoTVertexAnimComponent::SetVertexMeshPath(
	const FSoftObjectPath& MeshPath, FName PackageHint)
{
#if !WOT_GUARDED_BOOTSTRAP
	if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(MeshPath.TryLoad()))
	{
		SetSkeletalMesh(Mesh);
		return true;
	}
	return false;
#else
	FString Path = MeshPath.ToString();
	int32 Dot = INDEX_NONE;
	if (Path.FindLastChar(TEXT('.'), Dot)) Path.RightChopInline(Dot + 1);
	int32 Slash = INDEX_NONE;
	if (Path.FindLastChar(TEXT('/'), Slash)) Path.RightChopInline(Slash + 1);
	return !Path.IsEmpty() && SetVertexMesh(FName(*Path), PackageHint);
#endif
}

bool UWoTVertexAnimComponent::SetVertexMeshLogicalIds(
	FStringView MeshLogicalId, FStringView InAnimationLogicalId)
{
#if WOT_GUARDED_BOOTSTRAP
	return BindRuntimeVertexMesh(SelectRuntimeVertexMesh(
		FWoTRuntimeVertexMesh::Load(MeshLogicalId, InAnimationLogicalId)));
#else
	FString LogicalId(MeshLogicalId);
	const int32 Separator = LogicalId.Find(TEXT("__"), ESearchCase::CaseSensitive,
		ESearchDir::FromEnd);
	if (Separator == INDEX_NONE || Separator + 2 >= LogicalId.Len()) return false;
	FString Package = LogicalId.Left(Separator);
	int32 Slash = INDEX_NONE;
	if (Package.FindLastChar(TEXT('/'), Slash)) Package.RightChopInline(Slash + 1);
	return SetVertexMesh(FName(*LogicalId.Mid(Separator + 2)), FName(*Package));
#endif
}

bool UWoTVertexAnimComponent::HasDisplayMesh() const
{
	return RuntimeVertexMesh.IsValid() || GetSkeletalMeshAsset() != nullptr;
}

FName UWoTVertexAnimComponent::GetDisplayMeshName() const
{
	return RuntimeVertexMesh.IsValid() ? RuntimeVertexMesh->GetMeshName()
		: GetSkeletalMeshAsset() ? GetSkeletalMeshAsset()->GetFName() : NAME_None;
}

void UWoTVertexAnimComponent::BuildRuntimeMaterials()
{
	RuntimeDefaultMaterials.Reset();
	RuntimeMaterialInstances.Reset();
	OverrideMaterials.Reset();
	if (!RuntimeVertexMesh.IsValid()) return;
	for (int32 SlotIndex = 0;
		SlotIndex < RuntimeVertexMesh->GetSlots().Num(); ++SlotIndex)
	{
		const FWoTRuntimeVertexMeshSlot& Slot =
			RuntimeVertexMesh->GetSlots()[SlotIndex];
		const EWoTRuntimeVertexMeshStyle EffectiveStyle =
			WoTRuntimeEffectiveMeshStyle(Slot.Style, Style);
		const bool bEffectiveUnlit = Slot.bUnlit;
		const TCHAR* MasterName = bEffectiveUnlit
			? TEXT("M_WoT_Unlit") : TEXT("M_WoT_Master");
		switch (EffectiveStyle)
		{
		case EWoTRuntimeVertexMeshStyle::TwoSided:
			if (!bEffectiveUnlit) MasterName = TEXT("M_WoT_MeshLit2S"); break;
		case EWoTRuntimeVertexMeshStyle::Masked:
			MasterName = bEffectiveUnlit ? TEXT("M_WoT_UnlitMasked")
				: TEXT("M_WoT_MeshLitMasked"); break;
		case EWoTRuntimeVertexMeshStyle::Translucent:
			MasterName = bEffectiveUnlit ? TEXT("M_WoT_MeshTranslucent")
				: TEXT("M_WoT_MeshLitT"); break;
		case EWoTRuntimeVertexMeshStyle::Modulated:
			// Retail modulate is 2*Tex*Dst and ignores all lighting inputs.
			MasterName = TEXT("M_WoT_MeshModulate"); break;
		default: break;
		}
		UMaterialInterface* Master = Cast<UMaterialInterface>(
			FSoftObjectPath(FString::Printf(
				TEXT("/Game/WoT/Materials/%s.%s"), MasterName, MasterName)).TryLoad());
		UMaterialInstanceDynamic* MID = Master
			? UMaterialInstanceDynamic::Create(Master, this) : nullptr;
		const FSoftObjectPath EmbeddedTexture =
			!Slot.TexturePackage.IsEmpty() && !Slot.TextureName.IsEmpty()
			? FSoftObjectPath(FString::Printf(
				TEXT("/Game/WoT/Textures/%s/%s.%s"), *Slot.TexturePackage,
				*Slot.TextureName, *Slot.TextureName))
			: FSoftObjectPath();
		FSoftObjectPath TexturePath;
		if (const FSoftObjectPath* MultiSkin =
			RuntimeSkinOverrides.Find(Slot.TextureIndex))
		{
			TexturePath = *MultiSkin;
		}
		else if (Slot.TextureIndex != 0 && !EmbeddedTexture.IsNull())
		{
			TexturePath = EmbeddedTexture;
		}
		else if (!RuntimeActorSkin.IsNull())
		{
			TexturePath = RuntimeActorSkin;
		}
		else
		{
			TexturePath = EmbeddedTexture;
		}
		if (MID && !TexturePath.IsNull())
		{
			FWoTRuntimeTextureOptions Options;
			Options.bLoadMips = true;
			Options.bSRGB = false;
			Options.Filter = TF_Bilinear;
			Options.bForcePaletteIndexZeroTransparent =
				EffectiveStyle == EWoTRuntimeVertexMeshStyle::Masked;
			if (UTexture2D* Texture = UWoTRuntimeAssets::ResolveTexturePath(
				TexturePath, Options))
			{
				MID->SetTextureParameterValue(TEXT("Tex"), Texture);
				FColor KeyColor;
				if (UWoTRuntimeAssets::ResolveKeyColor(Texture, KeyColor))
					MID->SetVectorParameterValue(TEXT("KeyColor"),
						UWoTRuntimeAssets::PaletteColorToMaterialColor(KeyColor));
			}
		}
		if (MID)
		{
			// Safe pre-BeginPlay value; UWoTMeshLight replaces it with the
			// authenticated per-zone result as soon as the owner is live.
			MID->SetVectorParameterValue(TEXT("MLAmbient"),
				FLinearColor(0.25f, 0.25f, 0.25f, 1.0f));
		}
		RuntimeMaterialInstances.Add(MID);
		RuntimeDefaultMaterials.Add(MID ? static_cast<UMaterialInterface*>(MID)
			: UMaterial::GetDefaultMaterial(MD_Surface));
	}
}

void UWoTVertexAnimComponent::RefreshRuntimeMaterials()
{
	BuildRuntimeMaterials();
	MarkRenderStateDirty();
	if (AActor* Owner = GetOwner())
	{
		if (UWoTMeshLight* MeshLight =
			Owner->FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->ForceRuntimeBake();
		}
	}
}

void UWoTVertexAnimComponent::SetActorSkin(
	const FSoftObjectPath& TexturePath)
{
	RuntimeActorSkin = TexturePath;
	if (RuntimeVertexMesh.IsValid())
	{
		RefreshRuntimeMaterials();
		return;
	}
	// A real MultiSkins[0] wins over actor Skin. Preserve both its semantic map
	// entry and its cooked-carrier presentation regardless of setter order.
	if (const FSoftObjectPath* MultiSkin = RuntimeSkinOverrides.Find(0))
	{
		const FSoftObjectPath SavedMultiSkin = *MultiSkin;
		SetSkinOverride(0, SavedMultiSkin);
		return;
	}
	if (TexturePath.IsNull())
	{
		if (GetNumMaterials() > 0) SetMaterial(0, nullptr);
		return;
	}
	// Developer cooked carriers still present Actor.Skin through material zero;
	// keep the semantic state separate even though that carrier lacks UE1's
	// runtime texture-index lookup.
	SetSkinOverride(0, TexturePath);
	RuntimeSkinOverrides.Remove(0);
}

bool UWoTVertexAnimComponent::ApplyRuntimeSkinOverride(
	int32 SlotIndex, const FSoftObjectPath& TexturePath)
{
	if (!RuntimeVertexMesh.IsValid() || SlotIndex < 0)
		return false;
	bool bUsesAuthoredSkin = false;
	for (const FWoTRuntimeVertexMeshSlot& Slot : RuntimeVertexMesh->GetSlots())
	{
		bUsesAuthoredSkin |= Slot.TextureIndex == SlotIndex;
	}
	// MultiSkins is indexed by the UE1 mesh's authored skin index. Runtime
	// material slots are compacted by (texture index, style, unlit), so one
	// authored index can drive several render slots and need not equal any
	// compacted slot index. Preserve an authored override even while the current
	// developer carrier lacks that slot: a later authenticated mesh rebind must
	// not silently discard the replicated presentation intent.
	if (TexturePath.IsNull())
		RuntimeSkinOverrides.Remove(SlotIndex);
	else
		RuntimeSkinOverrides.Add(SlotIndex, TexturePath);
	RefreshRuntimeMaterials();
	return bUsesAuthoredSkin;
}

bool UWoTVertexAnimComponent::SetSkinOverride(
	int32 SlotIndex, const FSoftObjectPath& TexturePath)
{
	if (RuntimeVertexMesh.IsValid())
		return ApplyRuntimeSkinOverride(SlotIndex, TexturePath);
	if (TexturePath.IsNull() || SlotIndex < 0) return false;
	// Preserve the authored MultiSkins intent even when this developer carrier
	// does not include the optional cooked texture. Guarded runtime resolves the
	// same path from the authenticated texture payload contract.
	RuntimeSkinOverrides.Add(SlotIndex, TexturePath);
	if (GetSkeletalMeshAsset() == nullptr || GetNumMaterials() <= 0) return false;
	// Cooked editor carriers predate the cache-v3 render-slot model and some
	// collapse every authored skin into one material. Guarded runtime uses the
	// exact TextureIndex mapping above; keep editor presentation observable too.
	const int32 MaterialIndex = SlotIndex < GetNumMaterials() ? SlotIndex
		: (GetNumMaterials() == 1 ? 0 : INDEX_NONE);
	if (MaterialIndex == INDEX_NONE) return false;
	FWoTRuntimeTextureOptions Options;
	Options.bLoadMips = true;
	Options.bSRGB = false;
	Options.Filter = TF_Bilinear;
	const TArray<FName> SlotNames = GetMaterialSlotNames();
	Options.bForcePaletteIndexZeroTransparent = SlotNames.IsValidIndex(MaterialIndex)
		&& SlotNames[MaterialIndex].ToString().EndsWith(TEXT("_M"));
	UTexture2D* Texture = UWoTRuntimeAssets::ResolveTexturePath(
		TexturePath, Options);
	UMaterialInterface* Parent = GetMaterial(MaterialIndex);
	UMaterialInstanceDynamic* MID = Texture != nullptr && Parent != nullptr
		? CreateDynamicMaterialInstance(MaterialIndex, Parent) : nullptr;
	if (MID == nullptr) return false;
	MID->SetTextureParameterValue(TEXT("Tex"), Texture);
	FColor KeyColor;
	if (UWoTRuntimeAssets::ResolveKeyColor(Texture, KeyColor))
		MID->SetVectorParameterValue(TEXT("KeyColor"),
			UWoTRuntimeAssets::PaletteColorToMaterialColor(KeyColor));
	return true;
}

bool UWoTVertexAnimComponent::SetAllSkinOverrides(
	const FSoftObjectPath& TexturePath)
{
	if (TexturePath.IsNull()) return false;
	if (RuntimeVertexMesh.IsValid())
	{
		RuntimeSkinOverrides.Reset();
		for (const FWoTRuntimeVertexMeshSlot& Slot : RuntimeVertexMesh->GetSlots())
		{
			RuntimeSkinOverrides.Add(Slot.TextureIndex, TexturePath);
		}
		if (RuntimeSkinOverrides.IsEmpty()) return false;
		RefreshRuntimeMaterials();
		return true;
	}
	if (GetSkeletalMeshAsset() == nullptr) return false;
	ClearSkinOverrides();
	bool bApplied = false;
	for (int32 SlotIndex = 0; SlotIndex < GetNumMaterials(); ++SlotIndex)
		bApplied = SetSkinOverride(SlotIndex, TexturePath) || bApplied;
	return bApplied;
}

void UWoTVertexAnimComponent::ClearSkinOverrides()
{
	if (!RuntimeVertexMesh.IsValid())
	{
		for (const TPair<int32, FSoftObjectPath>& Pair : RuntimeSkinOverrides)
			SetMaterial(Pair.Key, nullptr);
		RuntimeSkinOverrides.Reset();
		return;
	}
	RuntimeSkinOverrides.Reset();
	RefreshRuntimeMaterials();
}

void UWoTVertexAnimComponent::SetRuntimeSkinOverrides(
	const TMap<int32, FSoftObjectPath>& SkinOverrides)
{
	if (!RuntimeVertexMesh.IsValid())
	{
		ClearSkinOverrides();
		for (const TPair<int32, FSoftObjectPath>& Pair : SkinOverrides)
			SetSkinOverride(Pair.Key, Pair.Value);
		return;
	}
	RuntimeSkinOverrides.Reset();
	for (const TPair<int32, FSoftObjectPath>& Pair : SkinOverrides)
	{
		if (!Pair.Value.IsNull())
		{
			RuntimeSkinOverrides.Add(Pair.Key, Pair.Value);
		}
	}
	RefreshRuntimeMaterials();
}

void UWoTVertexAnimComponent::CopyDisplayMeshFrom(
	const UWoTVertexAnimComponent* Source)
{
	if (!Source) return;
	const FSoftObjectPath ActorSkin = Source->RuntimeActorSkin;
	if (Source->RuntimeVertexMesh.IsValid())
	{
		const TMap<int32, FSoftObjectPath> Overrides = Source->RuntimeSkinOverrides;
		if (BindRuntimeVertexMesh(Source->RuntimeVertexMesh))
		{
			SetActorSkin(ActorSkin);
			SetRuntimeSkinOverrides(Overrides);
		}
	}
	else
	{
		const TMap<int32, FSoftObjectPath> Overrides =
			Source->RuntimeSkinOverrides;
		SetSkeletalMesh(Source->GetSkeletalMeshAsset());
		Sequences = Source->Sequences;
		SetActorSkin(ActorSkin);
		SetRuntimeSkinOverrides(Overrides);
	}
}

void UWoTVertexAnimComponent::SetSkeletalMesh(USkeletalMesh* NewMesh, bool bReinitPose)
{
	const bool bHadRuntimeMesh = RuntimeVertexMesh.IsValid();
	const bool bRecreateRuntimeRenderState =
		bHadRuntimeMesh && IsRenderStateCreated();
	if (bRecreateRuntimeRenderState)
	{
		// Detach while RuntimeVertexMesh still identifies the custom proxy. Once
		// cleared, skeletal-component paths are again allowed to assume their own
		// proxy type. Super may early-return for nullptr -> nullptr, so this
		// transition cannot rely on its FRenderStateRecreator.
		DestroyRenderState_Concurrent();
	}
	if (bHadRuntimeMesh)
	{
		ClearRuntimeVertexMesh();
	}
	ClearExternalMorphInjection();
	ClearPoseCache();
	Super::SetSkeletalMesh(NewMesh, bReinitPose);
	if (!bHadRuntimeMesh)
	{
		RuntimeSkinOverrides.Reset();
		RuntimeActorSkin.Reset();
	}
	if (bRecreateRuntimeRenderState && IsRegistered()
		&& !IsRenderStateCreated())
	{
		CreateRenderState_Concurrent(nullptr);
	}
	// The generated table is authoritative for imported meshes: a new mesh brings its own
	// #exec MESH SEQUENCE set (the old table is that mesh's, not this one's). Meshes without
	// a row (test doubles, hand-built) keep whatever Sequences were set.
	if (NewMesh)
	{
		WoTAnimData::PopulateSequences(
			this, NewMesh->GetFName(), AnimationLogicalId);
	}
	if (bCurrentPoseCPUAccessRequested)
	{
		CacheCurrentMeshPoseData();
	}
	TryBindPayloadMesh();
	if (NewMesh)
	{
		RebakeOwnerMeshLightAfterLateBind();
	}
}

void UWoTVertexAnimComponent::OnRegister()
{
	Super::OnRegister();
	// A CDO-bound mesh (generated leaf BPs set it at BP-build time) never passes through
	// SetSkeletalMesh at runtime — feed it at registration. Empty-only, so a hand-authored
	// sequence table survives.
	if (Sequences.IsEmpty())
	{
		if (USkeletalMesh* Bound = GetSkeletalMeshAsset())
		{
			WoTAnimData::PopulateSequences(
				this, Bound->GetFName(), AnimationLogicalId);
		}
	}
	if (bCurrentPoseCPUAccessRequested)
	{
		CacheCurrentMeshPoseData();
	}
	TryBindPayloadMesh();
}

void UWoTVertexAnimComponent::BeginPlay()
{
	Super::BeginPlay();
	TryBindPayloadMesh();
	EnsureRuntimeRenderState();
	UWoTMeshLight::AttachRuntime(GetOwner());
}

void UWoTVertexAnimComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearExternalMorphInjection();
	ClearPoseCache();
	if (RuntimeVertexMesh.IsValid() && IsRenderStateCreated())
	{
		DestroyRenderState_Concurrent();
	}
	ClearRuntimeVertexMesh();
	Super::EndPlay(EndPlayReason);
}

void UWoTVertexAnimComponent::BeginDestroy()
{
	if (RuntimeVertexMesh.IsValid() && IsRenderStateCreated())
	{
		DestroyRenderState_Concurrent();
	}
	ClearRuntimeVertexMesh();
	Super::BeginDestroy();
}

void UWoTVertexAnimComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags,
	ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);
	if (Fatness != 128 && RuntimeVertexMesh.IsValid()
		&& !GetComponentTransform().GetScale3D().Equals(
			LastPublishedFatnessScale))
	{
		PublishRuntimePose();
		UpdateBounds();
	}
	if (Teleport == ETeleportType::None)
	{
		return;
	}
	if (AActor* Owner = GetOwner())
	{
		if (UWoTMeshLight* MeshLight = Owner->FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->NotifyOwnerTeleported();
		}
	}
}

const FWoTAnimSequence* UWoTVertexAnimComponent::Find(FName Sequence) const
{
	for (const FWoTAnimSequence& S : Sequences)
	{
		if (S.Name == Sequence)
		{
			return &S;
		}
	}
	return nullptr;
}

void UWoTVertexAnimComponent::StartSequence(FName Sequence, float Rate, bool bInLoop)
{
	if (!Find(Sequence))
	{
		UE_LOG(LogTemp, Warning, TEXT("VertexAnim: unknown sequence %s"),
		       *Sequence.ToString());
		return;
	}
	Current = Sequence;
	LocalFrame = 0.0f;
	PlayRate = Rate;
	bLoop = bInLoop;
	bPlaying = true;
	bTweening = false;
	bPlayAfterTween = false;
	ApplyFrame();
}

void UWoTVertexAnimComponent::PlayAnim(FName Sequence, float Rate)
{
	StartSequence(Sequence, Rate, false);
	StampOwnerAnimIntent(Sequence, Rate, false);
}

void UWoTVertexAnimComponent::LoopAnim(FName Sequence, float Rate)
{
	StartSequence(Sequence, Rate, true);
	StampOwnerAnimIntent(Sequence, Rate, true);
}

void UWoTVertexAnimComponent::TweenAnim(FName Sequence, float Time)
{
	StartTween(Sequence, Time, false, 0.0f);
	if (GetOwnerRole() == ROLE_Authority)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
		{
			Character->SetTweenAnimIntent(Sequence, 0.0f, Time, true);
		}
	}
}

void UWoTVertexAnimComponent::TweenPlayAnim(FName Sequence, float Rate, float Time)
{
	StartTween(Sequence, Time, true, Rate);
	if (GetOwnerRole() == ROLE_Authority)
	{
		if (AWoTCharacter* Character = Cast<AWoTCharacter>(GetOwner()))
		{
			Character->SetTweenAnimIntent(Sequence, Rate, Time, false);
		}
	}
}

void UWoTVertexAnimComponent::StartTween(FName Sequence, float Time,
	bool bPlayAfter, float Rate)
{
	if (!Find(Sequence))
	{
		return;
	}
	// Native PlayAnim disables its requested tween when no previous sequence has
	// ever supplied a rendered pose (UE1's native PlayAnim). TweenAnim
	// itself still blends from the component's base pose, so only TweenPlay takes
	// this first-play shortcut.
	if (Time <= 0.0f || (bPlayAfter && Current.IsNone()))
	{
		StartSequence(Sequence, bPlayAfter ? Rate : 0.0f, false);
		if (!bPlayAfter)
		{
			bPlaying = false;
		}
		return;
	}
	// Capture the complete rendered pose. This includes every source frame left by
	// an interrupted crossfade, matching UE1's cached-vertex tween rather than
	// assuming the source is always a normal two-frame playback pose.
	TweenSourceWeights.Reset();
	if (RuntimeVertexMesh.IsValid())
	{
		TweenSourceWeights = CurrentPoseWeights;
	}
	else
	{
		for (const FName Morph : ActiveMorphs)
		{
			const int32 Frame = FrameFromMorphName(Morph);
			if (Frame >= 0)
			{
				TweenSourceWeights.Add(Frame,
					AccessFrameMorph(Frame, TOptional<float>()));
			}
		}
	}
	if (bExternalMorphInjectionActive)
	{
		// The native asset has no F0 morph: its effective base contribution is
		// whatever the two captured named morphs do not consume. F0 is a real
		// external frame, so publish that implicit remainder explicitly. Resolve
		// duplicate frames (including F0) with SetMorphTarget's last-write-wins
		// behavior, but subtract only nonzero-frame weights from the base.
		float CapturedNonZeroFrameWeight = 0.0f;
		for (const TPair<int32, float>& FrameWeight : TweenSourceWeights)
		{
			if (FrameWeight.Key != 0)
			{
				CapturedNonZeroFrameWeight += FrameWeight.Value;
			}
		}
		AccessFrameMorph(0, 1.0f - CapturedNonZeroFrameWeight);
	}
	Current = Sequence;
	LocalFrame = 0.0f;
	bLoop = false;
	bPlaying = false;
	bTweening = true;
	TweenTotal = Time;
	TweenElapsed = 0.0f;
	bPlayAfterTween = bPlayAfter;
	TweenPlayRate = Rate;
}

float UWoTVertexAnimComponent::GetAnimFrame() const
{
	const FWoTAnimSequence* S = Find(Current);
	return (S && S->NumFrames > 0) ? LocalFrame / S->NumFrames : 0.0f;
}

float UWoTVertexAnimComponent::FindNotifyFraction(FName Sequence, FName Function) const
{
	if (const FWoTAnimSequence* S = Find(Sequence))
	{
		for (const FWoTAnimNotify& N : S->Notifies)
		{
			if (N.Function == Function)
			{
				return N.Fraction;
			}
		}
	}
	return -1.0f;
}

void UWoTVertexAnimComponent::SetFrameAndStop(FName Sequence, float Fraction)
{
	const FWoTAnimSequence* S = Find(Sequence);
	if (!S)
	{
		return;
	}
	// Presentation-local (no intent stamp): the carcass copies the dead pawn's
	// AnimSequence+AnimFrame and holds it, exactly the CopyDisplaySettings pose.
	Current = Sequence;
	LocalFrame = FMath::Clamp(Fraction, 0.0f, 1.0f) * S->NumFrames;
	LocalFrame = FMath::Min(LocalFrame, static_cast<float>(S->NumFrames - 1));
	bLoop = false;
	bPlaying = false;
	bTweening = false;
	bPlayAfterTween = false;
	ApplyFrame();
}

void UWoTVertexAnimComponent::SetFrameMorphs(int32 GlobalA, float WeightA,
                                             int32 GlobalB, float WeightB)
{
	SetCurrentPoseWeights(GlobalA, WeightA, GlobalB, WeightB);
	if (RuntimeVertexMesh.IsValid())
	{
		ActiveMorphs.Reset();
		if (GlobalA >= 0 && WeightA > KINDA_SMALL_NUMBER)
			ActiveMorphs.Add(FrameMorphName(GlobalA));
		if (GlobalB >= 0 && WeightB > KINDA_SMALL_NUMBER && GlobalB != GlobalA)
			ActiveMorphs.Add(FrameMorphName(GlobalB));
		PublishRuntimePose();
		return;
	}
	FName NewMorphs[2] = {NAME_None, NAME_None};
	// Keep ActiveMorphs identical to the native representation: F0 is the
	// implicit base there, even though it is an explicit writable external frame.
	if (GlobalA >= 1 && WeightA > KINDA_SMALL_NUMBER)
	{
		NewMorphs[0] = FrameMorphName(GlobalA);
	}
	if (GlobalB >= 1 && WeightB > KINDA_SMALL_NUMBER)
	{
		NewMorphs[1] = FrameMorphName(GlobalB);
	}
	if (bExternalMorphInjectionActive)
	{
		// External F0 is the native pose's implicit base, not a third independent
		// morph. Build the final active-frame weights first (including native's
		// last-write-wins behavior when A and B name the same frame), then make
		// the base explicit as 1 - sum(nonzero-frame weights). An input that names
		// frame zero participates in duplicate resolution but is not subtracted.
		for (const int32 Frame : {0, GlobalA, GlobalB})
		{
			if (Frame >= 0)
			{
				AccessFrameMorph(Frame, 0.0f);
			}
		}
		for (const FName& Old : ActiveMorphs)
		{
			if (!Old.IsNone() && Old != NewMorphs[0] && Old != NewMorphs[1])
			{
				AccessFrameMorph(FrameFromMorphName(Old), 0.0f);
			}
		}
		TMap<int32, float, TInlineSetAllocator<2>> ActiveWeights;
		if (GlobalA >= 0 && WeightA > KINDA_SMALL_NUMBER)
		{
			ActiveWeights.Add(GlobalA, WeightA);
		}
		if (GlobalB >= 0 && WeightB > KINDA_SMALL_NUMBER)
		{
			ActiveWeights.FindOrAdd(GlobalB) = WeightB;
		}
		float NonZeroFrameWeight = 0.0f;
		for (const TPair<int32, float>& FrameWeight : ActiveWeights)
		{
			if (FrameWeight.Key != 0)
			{
				NonZeroFrameWeight += FrameWeight.Value;
				AccessFrameMorph(FrameWeight.Key, FrameWeight.Value);
			}
		}
		AccessFrameMorph(0, 1.0f - NonZeroFrameWeight);
		ActiveMorphs.Reset();
		if (!NewMorphs[0].IsNone()) { ActiveMorphs.Add(NewMorphs[0]); }
		if (!NewMorphs[1].IsNone() && NewMorphs[1] != NewMorphs[0]) { ActiveMorphs.Add(NewMorphs[1]); }
		return;
	}
	for (const FName& Old : ActiveMorphs)
	{
		if (!Old.IsNone() && Old != NewMorphs[0] && Old != NewMorphs[1])
		{
			AccessFrameMorph(FrameFromMorphName(Old), 0.0f);
		}
	}
	if (GlobalA >= 1 && WeightA > KINDA_SMALL_NUMBER)
	{
		AccessFrameMorph(GlobalA, WeightA);
	}
	if (GlobalB >= 1 && WeightB > KINDA_SMALL_NUMBER)
	{
		AccessFrameMorph(GlobalB, WeightB);
	}
	ActiveMorphs.Reset();
	if (!NewMorphs[0].IsNone()) { ActiveMorphs.Add(NewMorphs[0]); }
	if (!NewMorphs[1].IsNone() && NewMorphs[1] != NewMorphs[0]) { ActiveMorphs.Add(NewMorphs[1]); }
}

void UWoTVertexAnimComponent::SetCurrentPoseWeights(
	const int32 GlobalA, const float WeightA,
	const int32 GlobalB, const float WeightB)
{
	CurrentPoseGlobalA = GlobalA;
	CurrentPoseGlobalB = GlobalB;
	CurrentPoseWeightA = WeightA;
	CurrentPoseWeightB = WeightB;
	CurrentPoseWeights.Reset();
	if (GlobalA >= 0 && WeightA > KINDA_SMALL_NUMBER)
	{
		CurrentPoseWeights.Add(GlobalA, WeightA);
	}
	// SetMorphTarget and the external-set seam are both last-write-wins when
	// adjacent playback samples collapse to the same global frame.
	if (GlobalB >= 0 && WeightB > KINDA_SMALL_NUMBER)
	{
		CurrentPoseWeights.FindOrAdd(GlobalB) = WeightB;
	}
}

bool UWoTVertexAnimComponent::GetCurrentPoseVertexPositions(
	TArray<FVector>& OutPositions) const
{
	OutPositions.Reset();
	if (RuntimeVertexMesh.IsValid() && RuntimePoseStorage)
	{
		OutPositions.Reserve(RuntimePoseStorage->SourcePositions.Num());
		for (const FVector3f& Position : RuntimePoseStorage->SourcePositions)
			OutPositions.Add(FVector(Position));
		return !OutPositions.IsEmpty();
	}
	UWoTVertexAnimComponent* MutableThis =
		const_cast<UWoTVertexAnimComponent*>(this);
	if (PoseBasePositions.IsEmpty() && GetSkeletalMeshAsset())
	{
		MutableThis->RequestCurrentPoseCPUAccess();
	}
	if (PoseBasePositions.IsEmpty())
	{
		return false;
	}
	MutableThis->DeduplicatePoseCache();
	OutPositions.Reserve(PoseBasePositions.Num());
	for (const FVector3f& BasePosition : PoseBasePositions)
	{
		OutPositions.Add(FVector(BasePosition));
	}
	for (const TPair<int32, float>& FrameWeight : CurrentPoseWeights)
	{
		const TArray<FVector3f>* Deltas =
			PoseFrameDeltas.Find(FrameWeight.Key);
		if (!Deltas || Deltas->Num() != OutPositions.Num())
		{
			continue;
		}
		for (int32 Vertex = 0; Vertex < OutPositions.Num(); ++Vertex)
		{
			OutPositions[Vertex] +=
				FVector((*Deltas)[Vertex]) * FrameWeight.Value;
		}
	}
	return true;
}

void UWoTVertexAnimComponent::RequestCurrentPoseCPUAccess()
{
	bCurrentPoseCPUAccessRequested = true;
	CacheCurrentMeshPoseData();
	TryBindPayloadMesh();
}

void UWoTVertexAnimComponent::ClearPoseGeometryCache()
{
	PoseCacheMesh.Reset();
	PoseBasePositions.Reset();
	PoseFrameDeltas.Reset();
	bPayloadPoseCacheInstalled = false;
	bPoseCacheDeduplicated = false;
}

void UWoTVertexAnimComponent::ClearPoseCache()
{
	ClearPoseGeometryCache();
	CurrentPoseWeights.Reset();
	CurrentPoseGlobalA = INDEX_NONE;
	CurrentPoseGlobalB = INDEX_NONE;
	CurrentPoseWeightA = 0.0f;
	CurrentPoseWeightB = 0.0f;
}

void UWoTVertexAnimComponent::CacheCurrentMeshPoseData()
{
	USkeletalMesh* Mesh = GetSkeletalMeshAsset();
	if (!Mesh || (PoseCacheMesh.Get() == Mesh
			&& !PoseBasePositions.IsEmpty()))
	{
		return;
	}
	// Geometry can be rebuilt lazily while an animation is already publishing.
	// Do not erase that live playback state: the first pose read must blend the
	// newly cached geometry with the weights that were active before this call.
	ClearPoseGeometryCache();
	PoseCacheMesh = Mesh;

#if WITH_EDITOR
	// Editor, -nullrhi, and UnrealEditor -game retain the imported LOD model.
	// It is already in render-vertex order and does not depend on initialized
	// render buffers, so prefer it over an RHI-facing buffer read.
	if (const FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
		ImportedModel && ImportedModel->LODModels.IsValidIndex(0))
	{
		TArray<FSoftSkinVertex> ImportedVertices;
		ImportedModel->LODModels[0].GetVertices(ImportedVertices);
		PoseBasePositions.Reserve(ImportedVertices.Num());
		for (const FSoftSkinVertex& Vertex : ImportedVertices)
		{
			PoseBasePositions.Add(Vertex.Position);
		}
	}
#endif

	// Packaged targets have no imported model. Their cooked LOD position buffer
	// remains readable only when LODInfo.bAllowCPUAccess was authored before
	// cook (the payload strip path does this for stripped meshes).
	if (PoseBasePositions.IsEmpty())
	{
		const FSkeletalMeshRenderData* RenderData =
			Mesh->GetResourceForRendering();
		bool bCanReadCookedPositions = true;
#if !WITH_EDITOR
		// MeshLODInfo, not LODInfo: a local named LODInfo shadows
		// USkinnedMeshComponent::LODInfo, which MSVC's C4458-as-error rejects in
		// the non-editor configs that are the only place this block compiles.
		const FSkeletalMeshLODInfo* MeshLODInfo = Mesh->GetLODInfo(0);
		bCanReadCookedPositions = MeshLODInfo && MeshLODInfo->bAllowCPUAccess;
#endif
		if (RenderData && RenderData->LODRenderData.IsValidIndex(0)
			&& bCanReadCookedPositions)
		{
			const FPositionVertexBuffer& PositionBuffer =
				RenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer;
			PoseBasePositions.Reserve(PositionBuffer.GetNumVertices());
			for (uint32 Vertex = 0;
				Vertex < PositionBuffer.GetNumVertices(); ++Vertex)
			{
				PoseBasePositions.Add(PositionBuffer.VertexPosition(Vertex));
			}
		}
	}
	if (PoseBasePositions.IsEmpty())
	{
		return;
	}

	for (const TObjectPtr<UMorphTarget>& MorphTarget : Mesh->GetMorphTargets())
	{
		if (!MorphTarget)
		{
			continue;
		}
		const int32 Frame = FrameFromMorphName(MorphTarget->GetFName());
		if (Frame < 1)
		{
			continue;
		}
		TArray<FVector3f>& DenseDeltas =
			PoseFrameDeltas.Add(Frame);
		DenseDeltas.SetNumZeroed(PoseBasePositions.Num());
		for (FMorphTargetDeltaIterator DeltaIt =
				MorphTarget->GetDeltaIteratorForLOD(0);
			!DeltaIt.AtEnd(); ++DeltaIt)
		{
			const FMorphTargetDelta& Delta = *DeltaIt;
			if (DenseDeltas.IsValidIndex(
				static_cast<int32>(Delta.SourceIdx)))
			{
				DenseDeltas[Delta.SourceIdx] = Delta.PositionDelta;
			}
		}
	}
}

void UWoTVertexAnimComponent::DeduplicatePoseCache()
{
	if (bPoseCacheDeduplicated || PoseBasePositions.Num() < 2)
	{
		bPoseCacheDeduplicated = true;
		return;
	}
	const int32 RenderVertexCount = PoseBasePositions.Num();
	for (const TPair<int32, TArray<FVector3f>>& Frame : PoseFrameDeltas)
	{
		if (Frame.Value.Num() != RenderVertexCount)
		{
			return;
		}
	}

	// glTF has one render vertex per triangle corner so that per-corner UVs can
	// split. Retail bParticles iterated FrameVerts instead. Collapse only splits
	// that are indistinguishable for the complete animation: exact same base and
	// exact same delta in every cached morph frame.
	TArray<int32> Representatives;
	Representatives.Reserve(RenderVertexCount);
	for (int32 Candidate = 0; Candidate < RenderVertexCount; ++Candidate)
	{
		bool bDuplicate = false;
		for (const int32 Representative : Representatives)
		{
			if (!PoseBasePositions[Candidate].Equals(
					PoseBasePositions[Representative], 0.0f))
			{
				continue;
			}
			bool bEveryDeltaMatches = true;
			for (const TPair<int32, TArray<FVector3f>>& Frame : PoseFrameDeltas)
			{
				if (!Frame.Value[Candidate].Equals(
						Frame.Value[Representative], 0.0f))
				{
					bEveryDeltaMatches = false;
					break;
				}
			}
			if (bEveryDeltaMatches)
			{
				bDuplicate = true;
				break;
			}
		}
		if (!bDuplicate)
		{
			Representatives.Add(Candidate);
		}
	}

	if (Representatives.Num() != RenderVertexCount)
	{
		TArray<FVector3f> DeduplicatedBase;
		DeduplicatedBase.Reserve(Representatives.Num());
		for (const int32 Representative : Representatives)
		{
			DeduplicatedBase.Add(PoseBasePositions[Representative]);
		}
		PoseBasePositions = MoveTemp(DeduplicatedBase);

		for (TPair<int32, TArray<FVector3f>>& Frame : PoseFrameDeltas)
		{
			TArray<FVector3f> DeduplicatedDeltas;
			DeduplicatedDeltas.Reserve(Representatives.Num());
			for (const int32 Representative : Representatives)
			{
				DeduplicatedDeltas.Add(Frame.Value[Representative]);
			}
			Frame.Value = MoveTemp(DeduplicatedDeltas);
		}
	}
	bPoseCacheDeduplicated = true;
}

bool UWoTVertexAnimComponent::InstallPayloadPoseCache(
	const FWoTMeshPayloadCPUData& CPUData)
{
	if (!bCurrentPoseCPUAccessRequested)
	{
		return false;
	}
	if (bPayloadPoseCacheInstalled)
	{
		return true;
	}
	// A native read may have already collapsed corner splits before the stripped
	// payload arrived. Rebuild the raw render-order base so payload deltas can be
	// installed against their authored RenderVertexCount, then deduplicate once.
	if (bPoseCacheDeduplicated)
	{
		ClearPoseGeometryCache();
	}
	CacheCurrentMeshPoseData();
	if (CPUData.RenderVertexCount <= 0
		|| PoseBasePositions.Num() != CPUData.RenderVertexCount)
	{
		return false;
	}
	PoseFrameDeltas.Reset();
	for (int32 Frame = 0; Frame < CPUData.Frames.Num(); ++Frame)
	{
		const FWoTMeshPayloadFrame& SourceFrame = CPUData.Frames[Frame];
		if (SourceFrame.Deltas.Num() != CPUData.RenderVertexCount)
		{
			PoseFrameDeltas.Reset();
			return false;
		}
		TArray<FVector3f>& DenseDeltas = PoseFrameDeltas.Add(Frame);
		DenseDeltas.SetNumUninitialized(CPUData.RenderVertexCount);
		for (int32 Vertex = 0; Vertex < CPUData.RenderVertexCount; ++Vertex)
		{
			DenseDeltas[Vertex] =
				SourceFrame.Deltas[Vertex].PositionDelta;
		}
	}
	bPayloadPoseCacheInstalled = true;
	DeduplicatePoseCache();
	return true;
}

#if WITH_AUTOMATION_TESTS
void UWoTVertexAnimComponent::SetPoseCacheForTesting(
	const TConstArrayView<FVector3f> BasePositions,
	const TMap<int32, TArray<FVector3f>>& FrameDeltas)
{
	ClearPoseGeometryCache();
	bCurrentPoseCPUAccessRequested = true;
	PoseBasePositions.Append(BasePositions.GetData(), BasePositions.Num());
	PoseFrameDeltas = FrameDeltas;
}
#endif

float UWoTVertexAnimComponent::AccessFrameMorph(
	int32 GlobalFrame, TOptional<float> NewWeight)
{
	if (bExternalMorphInjectionActive)
	{
		if (GlobalFrame < 0 || GlobalFrame >= ExternalMorphFrameCount)
		{
			return 0.0f;
		}
		FExternalMorphSetWeights* Weights = nullptr;
#if WITH_AUTOMATION_TESTS
		if (HeadlessExternalMorphWeights.Weights.Num() == ExternalMorphFrameCount)
		{
			Weights = &HeadlessExternalMorphWeights;
		}
#endif
		if (!Weights && IsValidExternalMorphSetLODIndex(0))
		{
			Weights = GetExternalMorphWeights(0).MorphSets.Find(ExternalMorphSetID);
		}
		if (!Weights || !Weights->Weights.IsValidIndex(GlobalFrame))
		{
			return 0.0f;
		}
		if (NewWeight.IsSet())
		{
			Weights->Weights[GlobalFrame] = NewWeight.GetValue();
			Weights->UpdateNumActiveMorphTargets();
			if (IsValidExternalMorphSetLODIndex(0))
			{
				GetExternalMorphWeights(0).UpdateNumActiveMorphTargets();
			}
		}
		return Weights->Weights[GlobalFrame];
	}

	if (GlobalFrame < 1)
	{
		return 0.0f;
	}
	const FName MorphName = FrameMorphName(GlobalFrame);
	if (NewWeight.IsSet())
	{
		SetMorphTarget(MorphName, NewWeight.GetValue());
		return NewWeight.GetValue();
	}
	return GetMorphTarget(MorphName);
}

bool UWoTVertexAnimComponent::InstallExternalMorphSet(
	TSharedPtr<FExternalMorphSet> MorphSet, int32 FrameCount)
{
	if (!MorphSet.IsValid() || FrameCount <= 0)
	{
		return false;
	}
#if WITH_AUTOMATION_TESTS
	if (!GetSkeletalMeshAsset())
	{
		ClearExternalMorphInjection();
		HeadlessExternalMorphWeights.Name = MorphSet->Name;
		HeadlessExternalMorphWeights.Weights.SetNumZeroed(FrameCount);
		// A stripped mesh has a lattice base buffer. Its native idle appearance is
		// therefore the explicit external F0 pose, not an all-zero weight set.
		HeadlessExternalMorphWeights.Weights[0] = 1.0f;
		HeadlessExternalMorphWeights.UpdateNumActiveMorphTargets();
		bExternalMorphInjectionActive = true;
		ExternalMorphFrameCount = FrameCount;
		ActiveMorphs.Reset();
		if (Find(Current))
		{
			ApplyFrame();
		}
		return true;
	}
#else
	if (!GetSkeletalMeshAsset())
	{
		return false;
	}
#endif
	ClearExternalMorphInjection();
	AddExternalMorphSet(0, ExternalMorphSetID, MorphSet);
	RefreshExternalMorphTargetWeights();
	if (!IsValidExternalMorphSetLODIndex(0))
	{
		RemoveExternalMorphSet(0, ExternalMorphSetID);
		return false;
	}
	FExternalMorphSetWeights* Weights =
		GetExternalMorphWeights(0).MorphSets.Find(ExternalMorphSetID);
	if (!Weights)
	{
		RemoveExternalMorphSet(0, ExternalMorphSetID);
		return false;
	}
	const uint32 BufferMorphCount = MorphSet->MorphBuffers.GetNumMorphs();
	if (BufferMorphCount != 0 && BufferMorphCount != static_cast<uint32>(FrameCount))
	{
		RemoveExternalMorphSet(0, ExternalMorphSetID);
		RefreshExternalMorphTargetWeights();
		return false;
	}
	// A zero count occurs only in the -nullrhi automation seam; production
	// publication requires the subsystem's buffer count to equal FrameCount.
	Weights->Weights.SetNumZeroed(FrameCount);
	// Publish rest before any animation publication. With no active sequence,
	// F0 must remain at one so the stripped lattice base expands to native idle.
	Weights->Weights[0] = 1.0f;
	Weights->Name = MorphSet->Name;
	Weights->UpdateNumActiveMorphTargets();
	GetExternalMorphWeights(0).UpdateNumActiveMorphTargets();
	bExternalMorphInjectionActive = true;
	ExternalMorphFrameCount = FrameCount;
	ActiveMorphs.Reset();
	if (Find(Current))
	{
		ApplyFrame();
	}
	return true;
}

void UWoTVertexAnimComponent::ClearExternalMorphInjection()
{
	if (HasExternalMorphSet(0, ExternalMorphSetID))
	{
		RemoveExternalMorphSet(0, ExternalMorphSetID);
		RefreshExternalMorphTargetWeights();
	}
	bExternalMorphInjectionActive = false;
	ExternalMorphFrameCount = 0;
#if WITH_AUTOMATION_TESTS
	HeadlessExternalMorphWeights.Weights.Reset();
	HeadlessExternalMorphWeights.NumActiveMorphTargets = 0;
#endif
}

void UWoTVertexAnimComponent::TryBindPayloadMesh()
{
	// A caller may opt into CPU pose sampling after the external GPU set was
	// already installed. Re-enter the subsystem in that case so it can retain
	// and publish the decoded CPU deltas without rebuilding the GPU set.
	if (RuntimeVertexMesh.IsValid()
		|| (bExternalMorphInjectionActive
			&& (!bCurrentPoseCPUAccessRequested
				|| bPayloadPoseCacheInstalled))
		|| !GetWorld()
		|| !GetWorld()->GetGameInstance())
	{
		return;
	}
	if (UWoTPayloadSubsystem* Payloads =
		GetWorld()->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>())
	{
		Payloads->BindMeshComponent(this);
	}
}

void UWoTVertexAnimComponent::PublishRuntimePose()
{
	if (!RuntimeVertexMesh.IsValid()) return;
	if (!RuntimePoseStorage)
		RuntimePoseStorage = new FWoTRuntimeVertexMeshPose();
	if (!RuntimeVertexMesh->BlendPose(CurrentPoseWeights, *RuntimePoseStorage))
		return;
	LastPublishedFatnessScale = GetComponentTransform().GetScale3D();
	if (Fatness != 128)
	{
		const FVector3f LocalScale(FatnessLocalScale());
		for (int32 Index = 0;
			Index < RuntimePoseStorage->RenderPositions.Num(); ++Index)
		{
			RuntimePoseStorage->RenderPositions[Index] +=
				RuntimePoseStorage->RenderNormals[Index] * LocalScale;
		}
	}
	if (IsRegistered() && !IsRunningDedicatedServer() && !GUsingNullRHI
		&& FApp::CanEverRender())
	{
		MarkRenderDynamicDataDirty();
	}
}

FVector UWoTVertexAnimComponent::FatnessLocalScale() const
{
	const float WorldAmount = static_cast<float>(Fatness) / 16.0f - 8.0f;
	const FVector WorldScale = GetComponentTransform().GetScale3D();
	auto DivideAxis = [WorldAmount](const double Scale)
	{
		return FMath::IsNearlyZero(Scale) ? 0.0 : WorldAmount / Scale;
	};
	return FVector(DivideAxis(WorldScale.X), DivideAxis(WorldScale.Y),
		DivideAxis(WorldScale.Z));
}

FPrimitiveSceneProxy* UWoTVertexAnimComponent::CreateSceneProxy()
{
	if (!RuntimeVertexMesh.IsValid()) return Super::CreateSceneProxy();
	if (!RuntimePoseStorage || RuntimePoseStorage->RenderPositions.IsEmpty()
		|| IsRunningDedicatedServer() || GUsingNullRHI || !FApp::CanEverRender())
		return nullptr;
	return new FWoTRuntimeVertexMeshSceneProxy(
		this, RuntimeVertexMesh, *RuntimePoseStorage, RuntimeDrawState);
}

FWoTVertexMeshDiagnostics UWoTVertexAnimComponent::GetMeshDiagnostics() const
{
	FWoTVertexMeshDiagnostics Result;
	Result.MaterialCount = GetNumMaterials();
	Result.BoundsExtent = CalcBounds(FTransform::Identity).BoxExtent;
	if (RuntimeVertexMesh.IsValid())
	{
		Result.BoundMeshId = VertexMeshLogicalId;
		Result.ProxyKind = TEXT("runtime");
		Result.SectionCount = RuntimeVertexMesh->GetSlots().Num();
		const uint64 Submitted = RuntimeDrawState.IsValid()
			? RuntimeDrawState->LastSubmittedFrame.Load() : 0;
		Result.bSubmittedDrawLastFrame = Submitted != 0
			&& Submitted + 1 >= GFrameCounter;
	}
	else if (const USkeletalMesh* Carrier = GetSkeletalMeshAsset())
	{
		Result.BoundMeshId = Carrier->GetPathName();
		Result.ProxyKind = TEXT("carrier");
		if (const FSkeletalMeshRenderData* RenderData = Carrier->GetResourceForRendering();
			RenderData && !RenderData->LODRenderData.IsEmpty())
		{
			Result.SectionCount = RenderData->LODRenderData[0].RenderSections.Num();
		}
		const UWorld* World = GetWorld();
		Result.bSubmittedDrawLastFrame = World != nullptr
			&& GetLastRenderTimeOnScreen() >= World->GetTimeSeconds()
				- FMath::Max(World->GetDeltaSeconds() * 1.5f, 1.0f / 30.0f);
	}
	return Result;
}

void UWoTVertexAnimComponent::SendRenderDynamicData_Concurrent()
{
	if (!RuntimeVertexMesh.IsValid())
	{
		Super::SendRenderDynamicData_Concurrent();
		return;
	}
	if (!SceneProxy || !RuntimePoseStorage || IsRunningDedicatedServer()
		|| GUsingNullRHI || !FApp::CanEverRender())
		return;
	FWoTRuntimeVertexMeshDynamicData* DynamicData =
		new FWoTRuntimeVertexMeshDynamicData();
	DynamicData->Pose = *RuntimePoseStorage;
	FWoTRuntimeVertexMeshSceneProxy* Proxy =
		static_cast<FWoTRuntimeVertexMeshSceneProxy*>(SceneProxy);
	ENQUEUE_RENDER_COMMAND(FSendWoTRuntimeVertexMeshPose)(
		UE::RenderCommandPipe::SkeletalMesh,
		[Proxy, DynamicData](FRHICommandListBase& RHICmdList)
		{
			Proxy->SetPose_RenderThread(RHICmdList, DynamicData);
		});
}

FBoxSphereBounds UWoTVertexAnimComponent::CalcBounds(
	const FTransform& LocalToWorld) const
{
	if (!RuntimeVertexMesh.IsValid()) return Super::CalcBounds(LocalToWorld);
	FBox LocalBounds = RuntimeVertexMesh->GetBounds();
	if (Fatness != 128)
	{
		LocalBounds = LocalBounds.ExpandBy(FatnessLocalScale().GetAbs());
	}
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
}

int32 UWoTVertexAnimComponent::GetNumMaterials() const
{
	return RuntimeVertexMesh.IsValid() ? RuntimeVertexMesh->GetSlots().Num()
		: Super::GetNumMaterials();
}

UMaterialInterface* UWoTVertexAnimComponent::GetMaterial(int32 ElementIndex) const
{
	if (!RuntimeVertexMesh.IsValid()) return Super::GetMaterial(ElementIndex);
	if (OverrideMaterials.IsValidIndex(ElementIndex)
		&& OverrideMaterials[ElementIndex])
		return OverrideMaterials[ElementIndex];
	return RuntimeDefaultMaterials.IsValidIndex(ElementIndex)
		? RuntimeDefaultMaterials[ElementIndex].Get() : nullptr;
}

UMaterialInterface* UWoTVertexAnimComponent::GetMaterialByName(
	FName MaterialSlotName) const
{
	return RuntimeVertexMesh.IsValid()
		? GetMaterial(GetMaterialIndex(MaterialSlotName))
		: Super::GetMaterialByName(MaterialSlotName);
}

void UWoTVertexAnimComponent::SetMaterial(
	int32 ElementIndex, UMaterialInterface* Material)
{
	if (RuntimeVertexMesh.IsValid())
		UMeshComponent::SetMaterial(ElementIndex, Material);
	else
		Super::SetMaterial(ElementIndex, Material);
}

void UWoTVertexAnimComponent::SetMaterialByName(
	FName MaterialSlotName, UMaterialInterface* Material)
{
	if (RuntimeVertexMesh.IsValid())
	{
		const int32 Index = GetMaterialIndex(MaterialSlotName);
		if (Index != INDEX_NONE) SetMaterial(Index, Material);
	}
	else
	{
		Super::SetMaterialByName(MaterialSlotName, Material);
	}
}

int32 UWoTVertexAnimComponent::GetMaterialIndex(FName MaterialSlotName) const
{
	if (!RuntimeVertexMesh.IsValid())
		return Super::GetMaterialIndex(MaterialSlotName);
	for (int32 Index = 0; Index < RuntimeVertexMesh->GetSlots().Num(); ++Index)
		if (RuntimeVertexMesh->GetSlots()[Index].Name == MaterialSlotName)
			return Index;
	return INDEX_NONE;
}

bool UWoTVertexAnimComponent::IsMaterialSlotNameValid(
	FName MaterialSlotName) const
{
	return RuntimeVertexMesh.IsValid()
		? GetMaterialIndex(MaterialSlotName) != INDEX_NONE
		: Super::IsMaterialSlotNameValid(MaterialSlotName);
}

TArray<FName> UWoTVertexAnimComponent::GetMaterialSlotNames() const
{
	if (!RuntimeVertexMesh.IsValid()) return Super::GetMaterialSlotNames();
	TArray<FName> Result;
	Result.Reserve(RuntimeVertexMesh->GetSlots().Num());
	for (const FWoTRuntimeVertexMeshSlot& Slot : RuntimeVertexMesh->GetSlots())
		Result.Add(Slot.Name);
	return Result;
}

void UWoTVertexAnimComponent::GetUsedMaterials(
	TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (!RuntimeVertexMesh.IsValid())
	{
		Super::GetUsedMaterials(OutMaterials, bGetDebugMaterials);
		return;
	}
	for (int32 Index = 0; Index < GetNumMaterials(); ++Index)
		if (UMaterialInterface* Material = GetMaterial(Index))
			OutMaterials.AddUnique(Material);
}

void UWoTVertexAnimComponent::GetPrimitiveStats(
	FPrimitiveStats& PrimitiveStats) const
{
	if (!RuntimeVertexMesh.IsValid())
	{
		Super::GetPrimitiveStats(PrimitiveStats);
		return;
	}
	FPrimitiveLODStats& Stats = PrimitiveStats.LODStats.Emplace_GetRef(0);
	Stats.Sections = RuntimeVertexMesh->GetSlots().Num();
	for (int32 Index = 0; Index < RuntimeVertexMesh->GetSlots().Num(); ++Index)
	{
		Stats.Triangles += RuntimeVertexMesh->GetSlots()[Index].Indices.Num() / 3;
		Stats.MaterialIndices.Add(static_cast<uint16>(Index));
	}
	Stats.TotalResourceSize = RuntimeVertexMesh->GetRenderVertexCount()
		* (sizeof(FVector3f) * 3 + sizeof(FVector2f));
}

void UWoTVertexAnimComponent::OnComponentCollisionSettingsChanged(
	bool bUpdateOverlaps)
{
	if (RuntimeVertexMesh.IsValid())
	{
		UPrimitiveComponent::OnComponentCollisionSettingsChanged(bUpdateOverlaps);
		return;
	}
	Super::OnComponentCollisionSettingsChanged(bUpdateOverlaps);
}

void UWoTVertexAnimComponent::ApplyFrame()
{
	const FWoTAnimSequence* S = Find(Current);
	if (!S)
	{
		return;
	}
	const int32 I = FMath::FloorToInt(LocalFrame);
	const float T = LocalFrame - I;
	const int32 NextLocal = bLoop ? (I + 1) % S->NumFrames
	                              : FMath::Min(I + 1, S->NumFrames - 1);
	SetFrameMorphs(S->StartFrame + I, 1.0f - T,
	               S->StartFrame + NextLocal, T);
}

void UWoTVertexAnimComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                            FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bTweening)
	{
		const FWoTAnimSequence* S = Find(Current);
		TweenElapsed += DeltaTime;
		const float T = FMath::Clamp(TweenElapsed / TweenTotal, 0.0f, 1.0f);
		const int32 TargetFrame = S ? S->StartFrame : INDEX_NONE;
		if (RuntimeVertexMesh.IsValid())
		{
			TMap<int32, float> TweenWeights;
			for (const TPair<int32, float>& SourceWeight : TweenSourceWeights)
				TweenWeights.Add(SourceWeight.Key,
					SourceWeight.Value * (1.0f - T));
			if (TargetFrame >= 0)
				TweenWeights.FindOrAdd(TargetFrame) += T;
			CurrentPoseWeights = MoveTemp(TweenWeights);
			CurrentPoseGlobalA = CurrentPoseGlobalB = INDEX_NONE;
			CurrentPoseWeightA = CurrentPoseWeightB = 0.0f;
			PublishRuntimePose();
		}
		else if (bExternalMorphInjectionActive)
		{
			// The renderer caches the complete current vertex pose before a native
			// tween (UE1's mesh tween branch). Reconstruct that pose from both
			// adjacent source frames, then linearly add the destination frame.
			AccessFrameMorph(0, 0.0f);
			for (const TPair<int32, float>& SourceWeight : TweenSourceWeights)
			{
				AccessFrameMorph(SourceWeight.Key, 0.0f);
			}
			if (TargetFrame >= 0)
			{
				AccessFrameMorph(TargetFrame, 0.0f);
			}
			TMap<int32, float> TweenWeights;
			for (const TPair<int32, float>& SourceWeight : TweenSourceWeights)
			{
				TweenWeights.Add(SourceWeight.Key, SourceWeight.Value * (1.0f - T));
			}
			if (TargetFrame >= 0)
			{
				TweenWeights.FindOrAdd(TargetFrame) += T;
			}
			float NonZeroFrameWeight = 0.0f;
			for (const TPair<int32, float>& FrameWeight : TweenWeights)
			{
				if (FrameWeight.Key != 0)
				{
					NonZeroFrameWeight += FrameWeight.Value;
					AccessFrameMorph(FrameWeight.Key, FrameWeight.Value);
				}
			}
			const float BaseFrameWeight = 1.0f - NonZeroFrameWeight;
			AccessFrameMorph(0, BaseFrameWeight);
			TweenWeights.FindOrAdd(0) = BaseFrameWeight;
			CurrentPoseWeights = TweenWeights;
			CurrentPoseGlobalA = CurrentPoseGlobalB = INDEX_NONE;
			CurrentPoseWeightA = CurrentPoseWeightB = 0.0f;
			ActiveMorphs.Reset();
			for (const TPair<int32, float>& FrameWeight : TweenWeights)
			{
				if (FrameWeight.Key >= 1 && FrameWeight.Value > KINDA_SMALL_NUMBER)
				{
					ActiveMorphs.Add(FrameMorphName(FrameWeight.Key));
				}
			}
		}
		else
		{
			for (const FName& Old : ActiveMorphs)
			{
				if (!Old.IsNone())
				{
					AccessFrameMorph(FrameFromMorphName(Old), 0.0f);
				}
			}
			TMap<int32, float> TweenWeights;
			for (const TPair<int32, float>& SourceWeight : TweenSourceWeights)
			{
				TweenWeights.Add(SourceWeight.Key, SourceWeight.Value * (1.0f - T));
			}
			if (TargetFrame >= 1)
			{
				TweenWeights.FindOrAdd(TargetFrame) += T;
			}
			CurrentPoseWeights = TweenWeights;
			CurrentPoseGlobalA = CurrentPoseGlobalB = INDEX_NONE;
			CurrentPoseWeightA = CurrentPoseWeightB = 0.0f;
			ActiveMorphs.Reset();
			for (const TPair<int32, float>& FrameWeight : TweenWeights)
			{
				if (FrameWeight.Key >= 1 && FrameWeight.Value > KINDA_SMALL_NUMBER)
				{
					AccessFrameMorph(FrameWeight.Key, FrameWeight.Value);
					ActiveMorphs.Add(FrameMorphName(FrameWeight.Key));
				}
			}
		}
		if (T >= 1.0f)
		{
			bTweening = false;
			if (bPlayAfterTween)
			{
				LocalFrame = 0.0f;
				PlayRate = TweenPlayRate;
				bLoop = false;
				bPlaying = true;
				bPlayAfterTween = false;
				ApplyFrame();
			}
			else
			{
				OnAnimEnd.Broadcast(Current);
			}
		}
		return;
	}

	if (!bPlaying)
	{
		return;
	}
	const FWoTAnimSequence* S = Find(Current);
	if (!S)
	{
		bPlaying = false;
		return;
	}
	LocalFrame += S->Rate * PlayRate * DeltaTime;
	if (bLoop)
	{
		LocalFrame = FMath::Fmod(LocalFrame, (float)S->NumFrames);
	}
	else if (LocalFrame >= S->NumFrames - 1)
	{
		LocalFrame = S->NumFrames - 1;
		bPlaying = false;
		ApplyFrame();
		OnAnimEnd.Broadcast(Current);
		return;
	}
	ApplyFrame();
}
