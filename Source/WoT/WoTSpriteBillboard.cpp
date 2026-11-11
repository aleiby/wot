#include "WoTSpriteBillboard.h"
#include "Assets/WoTRetailIdentity.h"
#include "Assets/WoTRuntimeAssets.h"
#include "WoTFractalSubsystem.h"
#include "WoTFractalSim.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

#if WITH_AUTOMATION_TESTS
namespace
{
	FWoTTextureAnimationResourceCounts GTextureAnimationResourceCounts;
}

void WoTResetTextureAnimationResourceCountsForTesting()
{
	GTextureAnimationResourceCounts = {};
}

FWoTTextureAnimationResourceCounts WoTGetTextureAnimationResourceCountsForTesting()
{
	return GTextureAnimationResourceCounts;
}

void WoTCountTextureAnimationResolutionForTesting()
{
	++GTextureAnimationResourceCounts.TextureResolutions;
}

void WoTCountTextureAnimationMIDForTesting()
{
	++GTextureAnimationResourceCounts.DynamicMaterialCreations;
}

void WoTCountParticleSprayerSubstepForTesting()
{
	++GTextureAnimationResourceCounts.ParticleSubsteps;
}

void WoTCountParticleSprayerAnimationUpdateForTesting()
{
	++GTextureAnimationResourceCounts.AnimationUpdates;
}
#endif

bool WoTObservePrimitiveDraw(const UPrimitiveComponent* Primitive,
	float& LastObservedRenderTimeSeconds, float& OutDrawTimeSeconds)
{
	if (!IsValid(Primitive)) return false;
	const float ObservedRenderTime = Primitive->GetLastRenderTimeOnScreen();
	if (ObservedRenderTime <= LastObservedRenderTimeSeconds) return false;
	LastObservedRenderTimeSeconds = ObservedRenderTime;
	OutDrawTimeSeconds = FMath::Max(OutDrawTimeSeconds, ObservedRenderTime);
	return true;
}

bool UWoTTextureAnimationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return Super::ShouldCreateSubsystem(Outer) && World != nullptr
		&& SupportsPresentationForNetMode(World->GetNetMode());
}

int32 UWoTTextureAnimationSubsystem::RequestFrame(const FString& TextureIdentity,
	const int32 FrameCount, const float MinRate, const float MaxRate,
	const double DrawTimeSeconds, const uint64 RenderFrameNumber)
{
	if (TextureIdentity.IsEmpty() || FrameCount <= 1) return 0;
	FState& State = States.FindOrAdd(TextureIdentity);
	if (State.LastRenderFrame == RenderFrameNumber) return State.Frame;
	State.LastRenderFrame = RenderFrameNumber;
	const float Elapsed = State.LastUpdateDrawTime >= 0.0
		? static_cast<float>(FMath::Max(0.0,
			DrawTimeSeconds - State.LastUpdateDrawTime))
		: 0.0f;
	State.LastUpdateDrawTime = DrawTimeSeconds;
	auto Step = [&State, FrameCount]() { State.Frame = (State.Frame + 1) % FrameCount; };
	if (MaxRate == 0.0f) Step();
	else WoTAdvanceRateLimitedTexture(State.Accumulator, MinRate, MaxRate,
		Elapsed, Step);
	return State.Frame;
}

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
TMap<FString, int32> UWoTTextureAnimationSubsystem::SnapshotFramesForHarness() const
{
	TMap<FString, int32> Result;
	for (const TPair<FString, FState>& Pair : States)
	{
		Result.Add(Pair.Key, Pair.Value.Frame);
	}
	return Result;
}
#endif

UWoTSpriteBillboardComponent::UWoTSpriteBillboardComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

namespace
{
	UStaticMesh* BillboardPlane()
	{
		static TWeakObjectPtr<UStaticMesh> Cached;
		if (!Cached.IsValid())
		{
			Cached = Cast<UStaticMesh>(FSoftObjectPath(
				TEXT("/Engine/BasicShapes/Plane.Plane")).TryLoad());
		}
		return Cached.Get();
	}

	UMaterialInterface* ParticleMaster(bool bModulate, bool bModulateOnly)
	{
		return Cast<UMaterialInterface>(FSoftObjectPath(
			bModulate && bModulateOnly
			? TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate")
			: bModulate
			? TEXT("/Game/WoT/Materials/M_WoT_ParticleMod.M_WoT_ParticleMod")
			: TEXT("/Game/WoT/Materials/M_WoT_ParticleAdd.M_WoT_ParticleAdd")).TryLoad());
	}
}

void UWoTSpriteBillboardComponent::SetOnlyOwnerSee(const bool bValue)
{
	bOnlyOwnerSeeQuads = bValue;
	for (UInstancedStaticMeshComponent* Quad : {AddQuad.Get(), ModQuad.Get()})
	{
		if (IsValid(Quad))
		{
			Quad->SetOnlyOwnerSee(bValue);
		}
	}
}

void UWoTSpriteBillboardComponent::EnsureQuads()
{
	if (ModQuad || !GetOwner()
		|| GetOwner()->GetNetMode() == NM_DedicatedServer)   // local cosmetics only
	{
		return;
	}
	UStaticMesh* Plane = BillboardPlane();
	for (int32 Pass = 0; Pass < (bModulateOnly ? 1 : 2); ++Pass)
	{
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(
			GetOwner(), *FString::Printf(TEXT("%s_%s"), *GetName(),
			                             Pass ? TEXT("Add") : TEXT("Mod")));
		C->SetupAttachment(this);
		C->RegisterComponent();
		C->SetStaticMesh(Plane);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetOnlyOwnerSee(bOnlyOwnerSeeQuads);   // pre-quad SetOnlyOwnerSee sticks
		C->NumCustomDataFloats = 1;
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetWorldLocationAndRotation(FVector::ZeroVector, FQuat::Identity);
		// modulate before additive: FB = Dst*(1-Src) + Src (the corona split)
		C->SetTranslucentSortPriority(Pass ? 91 : 90);
		C->AddInstance(FTransform(FQuat::Identity, GetComponentLocation(),
		                          FVector(0.0001f)), /*bWorldSpace*/ true);
		(Pass ? AddQuad : ModQuad) = C;
	}
}

void UWoTSpriteBillboardComponent::SetSpriteSeries(
	const TCHAR* Dir, const TArray<FString>& FrameNames, float TexW, float TexH)
{
	TArray<FString> Identities;
	for (const FString& Name : FrameNames)
	{
		Identities.Add(FString::Printf(TEXT("%s.%s"), Dir, *Name));
	}
	SetAnimatedTextureSeries(Identities, TexW, TexH, FrameRate, FrameRate);
}

void UWoTSpriteBillboardComponent::SetAnimatedTextureSeries(
	const TArray<FString>& TextureIdentities, const float TexW,
	const float TexH, const float MinRate, const float MaxRate)
{
	Frames.Reset();
	FrameIdentities = TextureIdentities;
	if (GetOwner() && GetOwner()->GetNetMode() == NM_DedicatedServer)
	{
		FrameIdentities.Reset();
		return;
	}
	const FWoTRuntimeTextureOptions Options = RuntimeTextureOptions();
	for (const FString& Identity : FrameIdentities)
	{
		FWoTRetailObjectPath Path;
		if (!WoTParseRetailObjectPath(Identity, Path)) continue;
#if WITH_AUTOMATION_TESTS
		WoTCountTextureAnimationResolutionForTesting();
#endif
		Frames.Add(UWoTRuntimeAssets::ResolveTexturePath(FSoftObjectPath(
			FString::Printf(TEXT("/Game/WoT/Textures/%s.%s"),
				*WoTRetailTextureSubPath(Path), *Path.Name)), Options));
	}
	MinFrameRate = MinRate;
	MaxFrameRate = MaxRate;
	LastObservedRenderTimeSeconds = -1.0f;
	TexWidth = TexW;
	TexHeight = TexH;
	EnsureQuads();
	if (UMaterialInterface* Mod = ParticleMaster(true, bModulateOnly))
	{
		ModMid = UMaterialInstanceDynamic::Create(Mod, this);
#if WITH_AUTOMATION_TESTS
		if (ModMid) WoTCountTextureAnimationMIDForTesting();
#endif
		if (ModQuad) { ModQuad->SetMaterial(0, ModMid); }
	}
	if (!bModulateOnly)
	{
		if (UMaterialInterface* Add = ParticleMaster(false, false))
		{
			AddMid = UMaterialInstanceDynamic::Create(Add, this);
#if WITH_AUTOMATION_TESTS
			if (AddMid) WoTCountTextureAnimationMIDForTesting();
#endif
			if (AddQuad) { AddQuad->SetMaterial(0, AddMid); }
		}
	}
	CurrentFrame = INDEX_NONE;
	ApplyFrame(0);
}

FWoTRuntimeTextureOptions UWoTSpriteBillboardComponent::RuntimeTextureOptions()
{
	FWoTRuntimeTextureOptions Options;
	Options.bLoadMips = true;
	Options.bSRGB = false;
	Options.Filter = TF_Bilinear;
	return Options;
}

void UWoTSpriteBillboardComponent::SetSpriteMaterials(
	UMaterialInterface* Add, UMaterialInterface* Mod, float TexW, float TexH,
	const FName RuntimeProceduralIdentity)
{
	Frames.Reset();   // no cycling — the material animates itself (or is static)
	FrameIdentities.Reset();
	TexWidth = TexW;
	TexHeight = TexH;
	EnsureQuads();
	if (ModQuad && Mod) { ModQuad->SetMaterial(0, Mod); }
	if (AddQuad && Add) { AddQuad->SetMaterial(0, Add); }
	if (!RuntimeProceduralIdentity.IsNone())
	{
		if (UWorld* World = GetWorld())
		{
			if (UWoTFractalSubsystem* Fractals =
				World->GetSubsystem<UWoTFractalSubsystem>())
			{
				// Cached runtime MIDs may already sample WoTLive_*, so material
				// inference cannot register a newly spawned visible consumer.
				for (UInstancedStaticMeshComponent* Quad : {ModQuad.Get(), AddQuad.Get()})
				{
					if (Quad)
						Fractals->BindRuntimeProceduralSlot(Quad, 0, RuntimeProceduralIdentity);
				}
			}
		}
	}
}

void UWoTSpriteBillboardComponent::ApplyFrame(int32 Frame)
{
	if (!Frames.IsValidIndex(Frame) || Frame == CurrentFrame)
	{
		return;
	}
	CurrentFrame = Frame;
	if (UTexture2D* T = Frames[Frame])
	{
		if (AddMid) { AddMid->SetTextureParameterValue(FName(TEXT("Tex")), T); }
		if (ModMid) { ModMid->SetTextureParameterValue(FName(TEXT("Tex")), T); }
	}
}

#if WITH_AUTOMATION_TESTS
void UWoTSpriteBillboardComponent::MarkDrawnForTesting()
{
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	MarkDrawnForTesting(Now);
}

void UWoTSpriteBillboardComponent::MarkDrawnForTesting(const float DrawTimeSeconds)
{
	for (UInstancedStaticMeshComponent* Quad : {ModQuad.Get(), AddQuad.Get()})
	{
		if (Quad) Quad->GetSceneData().SetLastRenderTimeOnScreen(DrawTimeSeconds);
	}
}

UTexture* UWoTSpriteBillboardComponent::GetBoundTextureForTesting() const
{
	UTexture* Texture = nullptr;
	const UMaterialInstanceDynamic* MID = AddMid ? AddMid.Get() : ModMid.Get();
	return MID && MID->GetTextureParameterValue(
		FHashedMaterialParameterInfo(TEXT("Tex")), Texture) ? Texture : nullptr;
}
#endif

void UWoTSpriteBillboardComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!ModQuad)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World) return;
	Elapsed += DeltaTime;
	// Play-once (Explosion.uc Exploding.Tick, exact): frame = NumFrames -
	// int(NumFrames x LifeRemaining/Default) — a ceil-shaped walk whose final
	// 1/N of life indexes past the array: shipped drew nothing there (Texture
	// None), so the quads blank out before the actor dies.
	if (Frames.Num() > 1 && PlayOnceSeconds > 0.0f)
	{
		const float Scalar = FMath::Clamp(1.0f - Elapsed / PlayOnceSeconds, 0.0f, 1.0f);
		const int32 Idx = Frames.Num() - static_cast<int32>(Frames.Num() * Scalar);
		const bool bPast = Idx >= Frames.Num();
		if (AddQuad) { AddQuad->SetVisibility(!bPast); }
		if (ModQuad) { ModQuad->SetVisibility(!bPast); }
		if (!bPast)
		{
			ApplyFrame(Idx);
		}
	}
	// The AnimNext ring is requested only after one of this component's draw
	// primitives was visible; the world subsystem shares the exact texture state.
	else if (Frames.Num() > 1 && !FrameIdentities.IsEmpty())
	{
		float DrawTime = -1.0f;
		bool bDrawn = WoTObservePrimitiveDraw(ModQuad,
			LastObservedRenderTimeSeconds, DrawTime);
		bDrawn |= WoTObservePrimitiveDraw(AddQuad,
			LastObservedRenderTimeSeconds, DrawTime);
		if (bDrawn)
		{
			if (UWoTTextureAnimationSubsystem* Animation =
				World->GetSubsystem<UWoTTextureAnimationSubsystem>())
			{
				ApplyFrame(Animation->RequestFrame(FrameIdentities[0], Frames.Num(),
					MinFrameRate, MaxFrameRate, DrawTime, GFrameCounter));
			}
		}
	}

	// Face the camera (the sprayer's exact facing), world size = DrawScale x
	// USize/VSize on the 100uu plane.
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	const FQuat Face = Cam
		? WoTSpriteFacing::ScreenAlignedQuat(Cam->GetCameraRotation())
		: FQuat::Identity;
	const float Glow = FMath::Clamp(ScaleGlow, 0.0f, 1.0f);
	const FVector Scale(DrawScale * TexWidth / 100.0f,
	                    DrawScale * TexHeight / 100.0f, 1.0f);
	for (UInstancedStaticMeshComponent* C : {ModQuad.Get(), AddQuad.Get()})
	{
		if (C)
		{
			C->UpdateInstanceTransform(0, FTransform(Face, GetComponentLocation(), Scale),
			                           /*bWorldSpace*/ true);
			C->SetCustomDataValue(0, 0, Glow);
			C->MarkRenderStateDirty();
		}
	}
}
