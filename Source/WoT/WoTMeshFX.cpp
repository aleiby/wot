#include "WoTMeshFX.h"

#include "WoTCarcass.h"
#include "WoTMeshModulate.h"
#include "WoTMeshParticleRenderer.h"
#include "WoTPlayerColorFX.h"
#include "WoTSound.h"
#include "WoTSpriteBillboard.h"
#include "WoTUE1Names.h"
#include "WoTUE1LightRegistry.h"
#include "WoTVertexAnimComponent.h"
#include "Assets/WoTRetailIdentity.h"
#include "Assets/WoTRuntimeAssets.h"
#include "AI/WoTNPCCharacter.h"

#include "Components/PointLightComponent.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

namespace
{
	TUniquePtr<FWoTMeshFXSet>& InstalledMeshFXRegistry()
	{
		static TUniquePtr<FWoTMeshFXSet> Set;
		return Set;
	}

	bool ResolveMeshFXLight(UPointLightComponent* Light, FName RegistryKey,
		bool bOverrideColor, float Brightness, float Hue, float Saturation,
		float Radius, FWoTUE1LightRow& Out)
	{
		if (Light == nullptr || (Light->GetWorld() != nullptr
			&& Light->GetWorld()->GetNetMode() == NM_DedicatedServer))
		{
			if (Light != nullptr) Light->SetVisibility(false);
			return false;
		}
		if (!RegistryKey.IsNone())
		{
			const FWoTUE1LightRow* Base = WoTUE1LightRegistry::Find(RegistryKey);
			if (Base == nullptr)
			{
				Light->SetVisibility(false);
				return false;
			}
			Out = *Base;
			if (bOverrideColor)
			{
				Out.Hue = static_cast<uint8>(Hue);
				Out.Saturation = static_cast<uint8>(Saturation);
			}
			return true;
		}
		Out.Brightness = static_cast<uint8>(Brightness);
		Out.Hue = static_cast<uint8>(Hue);
		Out.Saturation = static_cast<uint8>(Saturation);
		Out.Radius = Radius;
		Out.Shape = EWoTUE1LightShape::NonIncidence;
		return true;
	}

	const TMap<FName, FWoTMeshEffectRow>& ActiveMeshRegistry()
	{
		if (const TUniquePtr<FWoTMeshFXSet>& Installed = InstalledMeshFXRegistry())
			return Installed->MeshEffects;
		static const TMap<FName, FWoTMeshEffectRow> Empty;
		return Empty;
	}

	const TMap<FName, FWoTPawnOverlayRow>& ActivePawnOverlayRegistry()
	{
		if (const TUniquePtr<FWoTMeshFXSet>& Installed = InstalledMeshFXRegistry())
			return Installed->PawnOverlays;
		static const TMap<FName, FWoTPawnOverlayRow> Empty;
		return Empty;
	}

	const TMap<FName, FWoTSpriteEffectRow>& ActiveSpriteRegistry()
	{
		if (const TUniquePtr<FWoTMeshFXSet>& Installed = InstalledMeshFXRegistry())
			return Installed->SpriteEffects;
		static const TMap<FName, FWoTSpriteEffectRow> Empty;
		return Empty;
	}

	bool ActiveRetailClassIsA(FName ClassName, FName BaseName)
	{
		if (ClassName == BaseName) return true;
		if (const TUniquePtr<FWoTMeshFXSet>& Installed = InstalledMeshFXRegistry())
		{
			if (const TArray<FName>* Chain = Installed->ClassChains.Find(ClassName))
				return Chain->Contains(BaseName);
		}
		return false;
	}

	UMaterialInterface* TranslucentMeshMaster()
	{
		return Cast<UMaterialInterface>(FSoftObjectPath(
			TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent."
			     "M_WoT_MeshTranslucent")).TryLoad());
	}

	// A translucent mesh that is not bUnlit is lit like any other UE1 mesh
	// (UE1 mesh renderer: GUnlitColor only for bUnlit; light manager Diffuse =
	// ScaleGlow), through the lit translucent family.
	UMaterialInterface* LitTranslucentMeshMaster()
	{
		return Cast<UMaterialInterface>(FSoftObjectPath(
			TEXT("/Game/WoT/Materials/M_WoT_MeshLitT.M_WoT_MeshLitT")).TryLoad());
	}

	uint8 UE1ActorStyle(EWoTMeshEffectStyle Style)
	{
		return Style == EWoTMeshEffectStyle::Translucent ? 3
			: Style == EWoTMeshEffectStyle::Modulated ? 4 : 1;
	}

	float UE1UnlitColor(float ScaleGlow, float AmbientGlow)
	{
		return FMath::Clamp(0.5f * ScaleGlow + AmbientGlow / 256.0f, 0.0f, 1.0f);
	}

	// The mesh effects retail draws as bParticles meshes: ShieldParticleMesh's
	// five shield visuals (ShieldParticleMesh.uc defaults bParticles=True).
	// UE1's mesh-renderer particle branch draws them at GUnlitColor (times
	// ScaleGlow), never through the light manager, whatever bUnlit says.
	bool IsRetailParticleMesh(FName Config)
	{
		return Config.ToString().EndsWith(TEXT("ShieldVisual"));
	}

	UTexture* LoadTexture(FName Package, FName Name)
	{
		return UWoTRuntimeAssets::ResolveTexturePath(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/%s/%s.%s"), *Package.ToString(),
			*Name.ToString(), *Name.ToString())));
	}

	UTexture* LoadTextureIdentity(const FString& Identity)
	{
		FWoTRetailObjectPath Path;
		if (!WoTParseRetailObjectPath(Identity, Path)) return nullptr;
		return UWoTRuntimeAssets::ResolveTexturePath(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/%s.%s"),
			*WoTRetailTextureSubPath(Path), *Path.Name)));
	}

	UMaterialInstanceDynamic* MakeMeshMaterial(UObject* Outer,
		UWoTVertexAnimComponent* Mesh, EWoTMeshEffectStyle Style,
		UMaterialInterface* ExplicitSource = nullptr, bool bLit = false)
	{
		UMaterialInterface* Original = ExplicitSource;
		if (!Original && Mesh) { Original = Mesh->GetMaterial(0); }
		if (Style == EWoTMeshEffectStyle::Modulated)
		{
			return WoTMeshModulate::CreateInstance(Outer, Original);
		}
		if (Style == EWoTMeshEffectStyle::Translucent && !ExplicitSource)
		{
			Original = bLit ? LitTranslucentMeshMaster() : TranslucentMeshMaster();
		}
		// In the packaged (guarded) build the runtime mesh slots and derived
		// materials are already MIDs, and UE rejects a MID as a parent: the new
		// instance would have no parent and render the default lit material, i.e.
		// black (Taint's impact meshes, SoulBarbImpact). Re-parent onto the source
		// MID's parent and carry its overrides (the live texture binding).
		UMaterialInstanceDynamic* SourceMID = Cast<UMaterialInstanceDynamic>(Original);
		UMaterialInterface* Parent = SourceMID ? SourceMID->Parent.Get() : Original;
		UMaterialInstanceDynamic* MID = Parent
			? UMaterialInstanceDynamic::Create(Parent, Outer) : nullptr;
		if (MID && SourceMID) MID->CopyParameterOverrides(SourceMID);
		return MID;
	}

	constexpr float RotUnitsToDegrees = 360.0f / 65536.0f;

	int32 CosmeticSeed(const UObject* Object)
	{
		const uint64 Entropy = FPlatformTime::Cycles64()
			^ static_cast<uint64>(reinterpret_cast<UPTRINT>(Object));
		return static_cast<int32>(Entropy ^ (Entropy >> 32));
	}
}

void FWoTMeshFXBuilder::Install(FWoTMeshFXSet&& Set)
{
	InstalledMeshFXRegistry() = MakeUnique<FWoTMeshFXSet>(MoveTemp(Set));
}

#if WITH_AUTOMATION_TESTS
void FWoTMeshFXBuilder::Reset()
{
	InstalledMeshFXRegistry().Reset();
}
#endif

AWoTMeshEffect::AWoTMeshEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	ParticleQuads = CreateDefaultSubobject<UWoTMeshParticleRenderer>(
		TEXT("ParticleQuads"));
	ParticleQuads->SetupAttachment(RootComponent);
	ParticleQuads->SetVisibility(false);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	CosmeticRandom.Initialize(CosmeticSeed(this));
}

float WoTMeshFX::ResolveDrawScale(const FWoTMeshEffectRow& Row,
	const AActor* Follow)
{
	if (!Follow) return Row.DrawScale;
	const AWoTNPCCharacter* FollowNPC = Cast<AWoTNPCCharacter>(Follow);
	for (const TPair<FName, float>& Override : Row.ClassScaleOverrides)
	{
		if (WoTUE1Names::ActorIsA(Follow, Override.Key)
			|| (FollowNPC && ActiveRetailClassIsA(
				FollowNPC->CreatureRowName, Override.Key)))
		{
			return Override.Value;
		}
	}
	return Row.DrawScale;
}

void AWoTMeshEffect::Configure(FName Config, const FWoTMeshEffectRow& Row,
	                            AActor* Follow)
{
	ConfigName = Config;
	ActiveRow = Row;
	bParticles = IsRetailParticleMesh(Config);
	// A bParticles mesh is drawn unlit whatever its bUnlit says.
	if (bParticles) { ActiveRow.bUnlit = true; }
	Style = UE1ActorStyle(Row.Style);
	bUnlit = ActiveRow.bUnlit;
	ScaleGlow = Row.ScaleGlow;
	AmbientGlow = Row.AmbientGlow;
	FullLife = Row.LifeSpan;
	InitialLifeSpan = Row.LifeSpan;
	CurrentDrawScale = WoTMeshFX::ResolveDrawScale(Row, Follow);
	if (Row.bFollow) { FollowActor = Follow; }
	Mesh->bMeshCurvy = Row.bMeshCurvy;
	if (Mesh->SetVertexMesh(Row.MeshName))
	{
		Mesh->SetRelativeScale3D(FVector(CurrentDrawScale));
	}
	if (bParticles)
	{
		// The retail bParticles branch never draws triangles. The hidden mesh
		// continues ticking solely to publish the current interpolated pose.
		Mesh->RequestCurrentPoseCPUAccess();
		Mesh->SetVisibility(false);
		Mesh->SetComponentTickEnabled(true);
		ParticleQuads->SetVisibility(true);
		PrimaryActorTick.AddPrerequisite(Mesh, Mesh->PrimaryComponentTick);
		if (UWoTMeshParticleRenderer* Renderer = GetParticleRenderer())
		{
			Renderer->InitializeParticleRenderer(
				Mesh, /*bClearAndLogOnPoseFailure*/ true);
		}
	}
	const bool bLitTranslucent = !ActiveRow.bUnlit
		&& Row.Style == EWoTMeshEffectStyle::Translucent;
	bLitSkin = !ActiveRow.bUnlit && Row.Style != EWoTMeshEffectStyle::Modulated
		&& (Row.ProceduralMaterialName.IsNone() || bLitTranslucent);
	if (!Row.ProceduralMaterialName.IsNone())
	{
		UMaterialInterface* Procedural = UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(
			FString::Printf(TEXT("/Game/WoT/Materials/Instances/MI_MeshFB_Angreal__%s.MI_MeshFB_Angreal__%s"),
				*Row.ProceduralMaterialName.ToString(),
				*Row.ProceduralMaterialName.ToString())));
		if (bLitTranslucent)
		{
			// A lit translucent procedural skin (the Taint explosion's
			// TaintExpGeoA/GeoB/Torus fire) is lit like any translucent mesh:
			// its fire texture goes on the lit translucent master. The
			// additive Glow master carries only unlit ones.
			UTexture* FireTexture = nullptr;
			if (Procedural)
			{
				Procedural->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), FireTexture);
			}
			SkinMaterial = MakeMeshMaterial(this, Mesh, Row.Style, nullptr, true);
			if (SkinMaterial && FireTexture)
			{
				SkinMaterial->SetTextureParameterValue(TEXT("Tex"), FireTexture);
			}
		}
		else
		{
			SkinMaterial = MakeMeshMaterial(this, Mesh, Row.Style, Procedural);
		}
		if (SkinMaterial) { Mesh->SetMaterial(0, SkinMaterial); }
	}
	else if (!Row.SkinNames.IsEmpty())
	{
		SetSkinFrame(0);
	}
	if (SkinMaterial && Row.bEnvironmentMap)
	{
		// The mesh master exposes the UE1 PF_Environment seat directly.
		SkinMaterial->SetScalarParameterValue(TEXT("EnvMap"), 1.0f);
	}
	if (SkinMaterial && ActiveRow.bUnlit)
	{
		SkinMaterial->SetScalarParameterValue(TEXT("MLDynEnable"), 0.0f);
	}
	if (SkinMaterial && ActiveRow.bUnlit && Row.ProceduralMaterialName.IsNone()
		&& Row.Style == EWoTMeshEffectStyle::Translucent)
	{
		// UE1 unlit-mesh source term (GUnlitColor = clamp(ScaleGlow*0.5 +
		// AmbientGlow/256)) — the translucent master defaults
		// MLAmbient to white, which rendered LSring at 2x retail.
		const float Unlit = UnlitGlow();
		SkinMaterial->SetVectorParameterValue(TEXT("MLAmbient"),
			FLinearColor(Unlit, Unlit, Unlit, 1.0f));
	}
	if (!Row.AnimName.IsNone())
	{
		if (Row.bLoopAnim) { Mesh->LoopAnim(Row.AnimName, Row.AnimRate); }
		else { Mesh->PlayAnim(Row.AnimName, Row.AnimRate); }
	}
	SpinRate = CosmeticRandom.FRandRange(Row.MinSpinRate, Row.MaxSpinRate);
	RiseRate = CosmeticRandom.FRandRange(Row.MinRiseRate, Row.MaxRiseRate);
	GrowRate = CosmeticRandom.FRandRange(Row.MinGrowRate, Row.MaxGrowRate);
	if (Row.bHasLight)
	{
		FWoTUE1LightRow LightRow;
		if (ResolveMeshFXLight(Light, Row.LightRegistryKey,
			Row.bOverrideLightColor, Row.LightBrightness, Row.LightHue,
			Row.LightSaturation, Row.LightRadius, LightRow))
		{
			FullLightIntensity = LightRow.Brightness / 64.0f * 5.0f;
			WoTUE1LightRegistry::Configure(Light, LightRow);
		}
	}
	if (Row.FadeInTime > 0.0f)
	{
		// A fade-in starts dark (ShieldParticleMesh.uc:62-63 zeroes ScaleGlow
		// and LightBrightness at spawn): apply it before the first frame
		// draws, not at the first tick.
		CurrentFadeScalar = 0.0f;
		ScaleGlow = 0.0f;
		PushFadeGlow();
		if (Light && Row.bHasLight) { Light->SetIntensity(0.0f); }
	}
}

void AWoTMeshEffect::SetEffectDrawScale(float DrawScale)
{
	CurrentDrawScale = DrawScale;
	if (Mesh) { Mesh->SetRelativeScale3D(FVector(DrawScale)); }
}

void AWoTMeshEffect::SetEffectGlow(float Glow)
{
	CurrentFadeScalar = Glow;
	// Retail callers scale the actor's ScaleGlow (LightningSkinEffect.uc:113,
	// BFExpSprayer.uc:78); unlit rows render that through GUnlitColor, lit
	// ones as the lights' Diffuse factor.
	ScaleGlow = ActiveRow.ScaleGlow * Glow;
	const float Dim = ActiveRow.bUnlit ? UnlitGlow() : Glow;
	if (UMaterialInstanceDynamic* Bound = Mesh
		? Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0)) : nullptr)
	{
		if (bLitSkin)
		{
			Bound->SetScalarParameterValue(TEXT("MLScaleGlow"), ScaleGlow);
		}
		else
		{
			Bound->SetScalarParameterValue(TEXT("Glow"), Dim);
			Bound->SetVectorParameterValue(TEXT("MLAmbient"),
				FLinearColor(Dim, Dim, Dim, 1.0f));
		}
	}
}

void AWoTMeshEffect::SetSkinFrame(int32 Index)
{
	if (!ActiveRow.SkinNames.IsValidIndex(Index) || Index == CurrentSkin)
	{
		return;
	}
	const FString Identity = ActiveRow.SkinIdentities.IsValidIndex(Index)
		? ActiveRow.SkinIdentities[Index]
		: FString::Printf(TEXT("%s.%s"), *ActiveRow.SkinPackage.ToString(),
			*ActiveRow.SkinNames[Index].ToString());
	SetSkinTextureIdentity(Identity);
	CurrentSkin = Index;
	if (ActiveRow.bPlayFrameSequence)
	{
		Mesh->PlayAnim(FName(*FString::Printf(TEXT("Frame%d"), Index)), 0.5f);
	}
}

void AWoTMeshEffect::SetSkinTextureIdentity(const FString& Identity)
{
	if (Identity == CurrentTextureIdentity) return;
	UTexture* Texture = LoadTextureIdentity(Identity);
	if (bParticles)
	{
		if (UWoTMeshParticleRenderer* Renderer = GetParticleRenderer())
		{
			Renderer->SetParticleTexture(Texture);
		}
		CurrentTextureIdentity = Identity;
		return;
	}
	if (!SkinMaterial)
	{
		SkinMaterial = MakeMeshMaterial(this, Mesh, ActiveRow.Style, nullptr,
			!ActiveRow.bUnlit);
		if (SkinMaterial) { Mesh->SetMaterial(0, SkinMaterial); }
	}
	if (SkinMaterial && Texture)
	{
		SkinMaterial->SetTextureParameterValue(TEXT("Tex"), Texture);
	}
	CurrentTextureIdentity = Identity;
}

void AWoTMeshEffect::ApplyAnimatedSkin(const int32 Index)
{
	if (!ActiveRow.SkinIdentities.IsValidIndex(Index) || !Mesh
		|| CurrentAnimationDrawTimeSeconds < 0.0f) return;
	const FString& Identity = ActiveRow.SkinIdentities[Index];
	const FWoTTextureAnimationRow* Animation =
		WoTMeshFX::FindTextureAnimation(Identity);
	if (Animation == nullptr || Animation->FrameIdentities.Num() <= 1) return;
	if (UWoTTextureAnimationSubsystem* Subsystem =
		GetWorld()->GetSubsystem<UWoTTextureAnimationSubsystem>())
	{
		const int32 Frame = Subsystem->RequestFrame(Identity,
			Animation->FrameIdentities.Num(), Animation->MinFrameRate,
			Animation->MaxFrameRate, CurrentAnimationDrawTimeSeconds,
			GFrameCounter);
		SetSkinTextureIdentity(Animation->FrameIdentities[Frame]);
	}
}

#if WITH_AUTOMATION_TESTS
void AWoTMeshEffect::MarkDrawnForTesting()
{
	if (Mesh && GetWorld())
	{
		MarkDrawnForTesting(GetWorld()->GetTimeSeconds());
	}
}

void AWoTMeshEffect::MarkDrawnForTesting(const float DrawTimeSeconds)
{
	if (bParticles && ParticleQuads)
	{
		ParticleQuads->GetSceneData().SetLastRenderTimeOnScreen(DrawTimeSeconds);
	}
	else if (Mesh)
	{
		Mesh->GetSceneData().SetLastRenderTimeOnScreen(DrawTimeSeconds);
	}
}

UTexture* AWoTMeshEffect::GetBoundTextureForTesting() const
{
	UTexture* Texture = nullptr;
	const UMaterialInterface* Material = bParticles && ParticleQuads
		? ParticleQuads->GetMaterial(0) : Mesh ? Mesh->GetMaterial(0) : nullptr;
	return Material && Material->GetTextureParameterValue(
		FHashedMaterialParameterInfo(TEXT("Tex")), Texture) ? Texture : nullptr;
}

UMaterialInstanceDynamic* AWoTMeshEffect::GetParticleMaterialForTesting() const
{
	const UWoTMeshParticleRenderer* Renderer = GetParticleRenderer();
	return Renderer ? Renderer->GetParticleMaterial() : nullptr;
}

FIntPoint AWoTMeshEffect::GetParticleTextureSizeForTesting() const
{
	const UWoTMeshParticleRenderer* Renderer = GetParticleRenderer();
	return Renderer ? Renderer->GetParticleTextureSize() : FIntPoint::ZeroValue;
}
#endif

void AWoTMeshEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Elapsed += DeltaSeconds;
	CurrentAnimationDrawTimeSeconds = -1.0f;
	const UPrimitiveComponent* ObservedPrimitive = bParticles
		? static_cast<const UPrimitiveComponent*>(ParticleQuads.Get())
		: static_cast<const UPrimitiveComponent*>(Mesh.Get());
	WoTObservePrimitiveDraw(ObservedPrimitive,
		LastObservedAnimationRenderTimeSeconds,
		CurrentAnimationDrawTimeSeconds);
	if (FollowActor.IsValid())
	{
		SetActorLocation(FollowActor->GetActorLocation());
	}
	if (SpinRate != 0.0f)
	{
		AccumYaw += SpinRate * DeltaSeconds;
		FRotator R = GetActorRotation();
		R.Yaw = AccumYaw * RotUnitsToDegrees;
		SetActorRotation(R);
	}
	if (RiseRate != 0.0f)
	{
		AddActorWorldOffset(FVector(0.0f, 0.0f, RiseRate * DeltaSeconds));
	}
	if (GrowRate != 0.0f)
	{
		CurrentDrawScale += GrowRate * DeltaSeconds;
		Mesh->SetRelativeScale3D(FVector(CurrentDrawScale));
	}
	if (ActiveRow.bRandomSkinEachTick && ActiveRow.SkinNames.Num() > 1)
	{
		SetSkinFrame(CosmeticRandom.RandHelper(ActiveRow.SkinNames.Num()));
	}
	else if (ActiveRow.SkinStepSeconds > 0.0f && ActiveRow.SkinNames.Num() > 1)
	{
		int32 Index = FMath::FloorToInt(Elapsed / ActiveRow.SkinStepSeconds);
		Index = ActiveRow.bLoopSkinSeries
			? Index % ActiveRow.SkinNames.Num()
			: FMath::Min(Index, ActiveRow.SkinNames.Num() - 1);
		SetSkinFrame(Index);
	}
	if (CurrentSkin != INDEX_NONE) ApplyAnimatedSkin(CurrentSkin);
	if (ActiveRow.MeltTime > 0.0f && !ActiveRow.MeltAnim.IsNone()
		&& GetLifeSpan() <= ActiveRow.MeltTime && !bMeltStarted)
	{
		bMeltStarted = true;
		Mesh->TweenAnim(ActiveRow.MeltAnim, ActiveRow.MeltTime);
		if (!ActiveRow.MeltSoundSubPath.IsNone())
		{
			WoTSound::PlayOneShot(this,
				WoTSound::LoadBySubPath(ActiveRow.MeltSoundSubPath),
				GetActorLocation(), WoTSound::WorldSoundRadius(32));
		}
	}
	float EnvelopeScalar = 1.0f;
	if (ActiveRow.FadeInTime + ActiveRow.SustainTime + ActiveRow.FadeOutTime > 0.0f)
	{
		if (Elapsed < ActiveRow.FadeInTime)
		{
			EnvelopeScalar = ActiveRow.FadeInTime > 0.0f
				? Elapsed / ActiveRow.FadeInTime : 1.0f;
		}
		else if (Elapsed >= ActiveRow.FadeInTime + ActiveRow.SustainTime)
		{
			EnvelopeScalar = ActiveRow.FadeOutTime > 0.0f
				? 1.0f - (Elapsed - ActiveRow.FadeInTime - ActiveRow.SustainTime)
					/ ActiveRow.FadeOutTime
				: 0.0f;
		}
		EnvelopeScalar = FMath::Clamp(EnvelopeScalar, 0.0f, 1.0f);
	}
	else if (ActiveRow.MeltTime > 0.0f && GetLifeSpan() <= ActiveRow.MeltTime)
	{
		EnvelopeScalar = FMath::Clamp(GetLifeSpan() / ActiveRow.MeltTime, 0.0f, 1.0f);
	}
	else if (ActiveRow.bFadeEnvelope && FullLife > 0.0f)
	{
		EnvelopeScalar = FMath::Clamp(GetLifeSpan() / FullLife, 0.0f, 1.0f);
	}
	if (EnvelopeScalar < 1.0f || ActiveRow.bFadeEnvelope
		|| ActiveRow.MeltTime > 0.0f || ActiveRow.FadeInTime > 0.0f)
	{
		CurrentFadeScalar = EnvelopeScalar;
		// Retail effect fades scale ScaleGlow and LightBrightness, not
		// AmbientGlow (TaintExpAssets.uc:38-39, ShieldParticleMesh.uc:105-106,
		// IceLeech.uc:183). The actor state keeps a mesh-light re-bake in step.
		ScaleGlow = ActiveRow.ScaleGlow * CurrentFadeScalar;
		PushFadeGlow();
		if (Light && ActiveRow.bHasLight)
		{
			Light->SetIntensity(FullLightIntensity * CurrentFadeScalar);
		}
	}
	if (bParticles)
	{
		if (UWoTMeshParticleRenderer* Renderer = GetParticleRenderer())
		{
			Renderer->UpdateParticleQuads(CurrentDrawScale, ScaleGlow,
				static_cast<uint8>(AmbientGlow));
		}
	}
}

float AWoTMeshEffect::UnlitGlow() const
{
	// GUnlitColor; a bParticles mesh multiplies it by ScaleGlow once more
	// (UE1's mesh renderer), unless ScaleGlow is exactly 1.
	float Unlit = UE1UnlitColor(ScaleGlow, AmbientGlow);
	if (bParticles && ScaleGlow != 1.0f)
	{
		Unlit = FMath::Clamp(Unlit * ScaleGlow, 0.0f, 1.0f);
	}
	return Unlit;
}

void AWoTMeshEffect::PushFadeGlow()
{
	if (bParticles) return;
	if (!Mesh) return;
	// UWoTVertexAnimComponent::BeginPlay may reparent a mesh-light material
	// after deferred Configure. Always address the MID currently bound to the
	// mesh so the fade cannot disappear into a stale pre-BeginPlay instance.
	if (UMaterialInstanceDynamic* BoundMID =
		Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0)))
	{
		SkinMaterial = BoundMID;
	}
	if (!SkinMaterial || Mesh->GetMaterial(0) != SkinMaterial) return;
	if (bLitSkin)
	{
		// A lit mesh fades its lights (Diffuse = ScaleGlow). No lit effect
		// row carries AmbientGlow (WoT.MeshLight.MeshEffectRetailLighting),
		// so the baked ambient stands.
		SkinMaterial->SetScalarParameterValue(TEXT("MLScaleGlow"), ScaleGlow);
		return;
	}
	const float Dim = UnlitGlow();
	if (!ActiveRow.ProceduralMaterialName.IsNone())
	{
		// MI_MeshFB_* is rooted at the procedural mesh master, whose
		// authored brightness contract is the scalar Glow parameter.
		SkinMaterial->SetScalarParameterValue(TEXT("Glow"), Dim);
	}
	else
	{
		// Mesh-lit/static-skin families expose MLAmbient as a vector.
		SkinMaterial->SetVectorParameterValue(TEXT("MLAmbient"),
			FLinearColor(Dim, Dim, Dim, 1.0f));
	}
}

UWoTMeshParticleRenderer* AWoTMeshEffect::GetParticleRenderer() const
{
	return Cast<UWoTMeshParticleRenderer>(ParticleQuads);
}

AWoTPawnSkinOverlay::AWoTPawnSkinOverlay()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	// Retail toggles bOwnerNoSee from ViewTarget; in shipped multiplayer the
	// owned pawn is the first-person view target and detached/spectator cameras
	// target something else. This is the established pawn-body owner-hide seam.
	Mesh->SetOwnerNoSee(true);
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	CosmeticRandom.Initialize(CosmeticSeed(this));
}

void AWoTPawnSkinOverlay::Configure(FName Config,
	const FWoTPawnOverlayRow& Row, APawn* Pawn)
{
	ConfigName = Config;
	ActiveRow = Row;
	// Both overlay classes are STY_Translucent and bUnlit
	// (LightningSkinEffect.uc:191, ReflectSkinEffect.uc:141).
	Style = 3;
	bUnlit = true;
	ScaleGlow = Row.ScaleGlow;
	AmbientGlow = Row.AmbientGlow;
	FollowPawn = Pawn;
	FullLife = Row.LifeSpan;
	InitialLifeSpan = Row.LifeSpan;
	SetOwner(Pawn);
	if (Pawn) { SetActorTransform(Pawn->GetActorTransform()); }

	UWoTVertexAnimComponent* Source = nullptr;
	if (const ACharacter* Character = Cast<ACharacter>(Pawn))
	{
		Source = Cast<UWoTVertexAnimComponent>(Character->GetMesh());
	}
	if (Source && Source->HasDisplayMesh())
	{
		Mesh->CopyDisplayMeshFrom(Source);
		Mesh->SetRelativeTransform(Source->GetRelativeTransform());
		Mesh->SetFatness(static_cast<uint8>(Row.Fatness));
		const int32 Slots = FMath::Max(1, Source->GetNumMaterials());
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			UMaterialInstanceDynamic* MID = MakeMeshMaterial(
				this, Mesh, EWoTMeshEffectStyle::Translucent);
			if (MID)
			{
				// Both overlay classes are bUnlit (LightningSkinEffect.uc:191,
				// ReflectSkinEffect.uc:141): the source term is GUnlitColor =
				// clamp(ScaleGlow*0.5 + AmbientGlow/256), not AmbientGlow alone.
				const float Unlit = FMath::Clamp(
					0.5f * Row.ScaleGlow + Row.AmbientGlow / 256.0f, 0.0f, 1.0f);
				MID->SetVectorParameterValue(TEXT("MLAmbient"),
					FLinearColor(Unlit, Unlit, Unlit, 1.0f));
				MID->SetScalarParameterValue(TEXT("MLDynEnable"), 0.0f);
				Mesh->SetMaterial(Slot, MID);
				SkinMaterials.Add(MID);
			}
		}
	}
	if (!Row.SkinNames.IsEmpty()) { SetSkinFrame(0); }
	if (Row.bHasLight)
	{
		FWoTUE1LightRow LightRow;
		if (ResolveMeshFXLight(Light, Row.LightRegistryKey,
			Row.bOverrideLightColor, Row.LightBrightness, Row.LightHue,
			Row.LightSaturation, Row.LightRadius, LightRow))
		{
			FullLightIntensity = LightRow.Brightness / 64.0f * 5.0f;
			WoTUE1LightRegistry::Configure(Light, LightRow);
		}
	}
	if (Row.bSpawnLightningRings)
	{
		Rings.SetNum(5);
		RingRiseRates.Init(0.0f, 5);
		RingHeights.Init(0.0f, 5);
	}
}

void AWoTPawnSkinOverlay::SetSkinFrame(int32 Index)
{
	if (!ActiveRow.SkinNames.IsValidIndex(Index) || Index == CurrentSkin)
	{
		return;
	}
	const FString Identity = ActiveRow.SkinIdentities.IsValidIndex(Index)
		? ActiveRow.SkinIdentities[Index]
		: FString::Printf(TEXT("%s.%s"), *ActiveRow.SkinPackage.ToString(),
			*ActiveRow.SkinNames[Index].ToString());
	SetSkinTextureIdentity(Identity);
	CurrentSkin = Index;
}

void AWoTPawnSkinOverlay::SetSkinTextureIdentity(const FString& Identity)
{
	if (Identity == CurrentTextureIdentity) return;
	if (UTexture* Texture = LoadTextureIdentity(Identity))
	{
		for (UMaterialInstanceDynamic* MID : SkinMaterials)
		{
			if (MID) { MID->SetTextureParameterValue(TEXT("Tex"), Texture); }
		}
	}
	CurrentTextureIdentity = Identity;
}

void AWoTPawnSkinOverlay::ApplyAnimatedSkin(const int32 Index)
{
	if (!ActiveRow.SkinIdentities.IsValidIndex(Index) || !Mesh
		|| CurrentAnimationDrawTimeSeconds < 0.0f) return;
	const FString& Identity = ActiveRow.SkinIdentities[Index];
	const FWoTTextureAnimationRow* Animation =
		WoTMeshFX::FindTextureAnimation(Identity);
	if (Animation == nullptr || Animation->FrameIdentities.Num() <= 1) return;
	if (UWoTTextureAnimationSubsystem* Subsystem =
		GetWorld()->GetSubsystem<UWoTTextureAnimationSubsystem>())
	{
		const int32 Frame = Subsystem->RequestFrame(Identity,
			Animation->FrameIdentities.Num(), Animation->MinFrameRate,
			Animation->MaxFrameRate, CurrentAnimationDrawTimeSeconds,
			GFrameCounter);
		SetSkinTextureIdentity(Animation->FrameIdentities[Frame]);
	}
}

void AWoTPawnSkinOverlay::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Elapsed += DeltaSeconds;
	CurrentAnimationDrawTimeSeconds = -1.0f;
	WoTObservePrimitiveDraw(Mesh, LastObservedAnimationRenderTimeSeconds,
		CurrentAnimationDrawTimeSeconds);
	APawn* Pawn = FollowPawn.Get();
	if (!IsValid(Pawn))
	{
		Destroy();
		return;
	}
	SetActorTransform(Pawn->GetActorTransform());
	if (const ACharacter* Character = Cast<ACharacter>(Pawn))
	{
		if (const UWoTVertexAnimComponent* Source =
			Cast<UWoTVertexAnimComponent>(Character->GetMesh()))
		{
			const FName Sequence = Source->GetCurrentSequence();
			if (!Sequence.IsNone()) { LastSequence = Sequence; }
			if (!LastSequence.IsNone())
			{
				Mesh->SetFrameAndStop(LastSequence, Source->GetAnimFrame());
			}
		}
	}
	if (ActiveRow.bStepSkinEveryTick && ActiveRow.SkinNames.Num() > 1)
	{
		SetSkinFrame((CurrentSkin + 1) % ActiveRow.SkinNames.Num());
	}
	else if (ActiveRow.SkinStepSeconds > 0.0f && ActiveRow.SkinNames.Num() > 1)
	{
		int32 Index = FMath::FloorToInt(Elapsed / ActiveRow.SkinStepSeconds);
		Index = ActiveRow.bLoopSkinSeries ? Index % ActiveRow.SkinNames.Num()
			: FMath::Min(Index, ActiveRow.SkinNames.Num() - 1);
		SetSkinFrame(Index);
	}
	if (CurrentSkin != INDEX_NONE) ApplyAnimatedSkin(CurrentSkin);
	if (ActiveRow.bFadeEnvelope && FullLife > 0.0f)
	{
		CurrentFadeScalar = FMath::Clamp(GetLifeSpan() / FullLife, 0.0f, 1.0f);
		// ReflectSkinEffect.uc:42-44 scales ScaleGlow AND AmbientGlow by the
		// remaining-life fraction; GUnlitColor then shrinks linearly.
		ScaleGlow = ActiveRow.ScaleGlow * CurrentFadeScalar;
		AmbientGlow = ActiveRow.AmbientGlow * CurrentFadeScalar;
		const float Dim = FMath::Clamp(
			(0.5f * ActiveRow.ScaleGlow + ActiveRow.AmbientGlow / 256.0f)
				* CurrentFadeScalar, 0.0f, 1.0f);
		for (UMaterialInstanceDynamic* MID : SkinMaterials)
		{
			if (MID)
			{
				MID->SetVectorParameterValue(TEXT("MLAmbient"),
					FLinearColor(Dim, Dim, Dim, 1.0f));
			}
		}
		if (Light) { Light->SetIntensity(FullLightIntensity * CurrentFadeScalar); }
	}
	if (ActiveRow.bSpawnLightningRings) { TickRings(DeltaSeconds); }
}

void AWoTPawnSkinOverlay::TickRings(float DeltaSeconds)
{
	APawn* Pawn = FollowPawn.Get();
	if (!Pawn || !GetWorld()) { return; }
	if (Elapsed >= NextRingSpawnTime)
	{
		NextRingSpawnTime = Elapsed + CosmeticRandom.FRandRange(0.4f, 1.0f);
		RingIndex = (RingIndex + 1) % Rings.Num();
		if (IsValid(Rings[RingIndex])) { Rings[RingIndex]->Destroy(); }
		Rings[RingIndex] = WoTMeshFX::SpawnLocal(*GetWorld(), TEXT("LSRing"),
			Pawn->GetActorLocation());
		RingRiseRates[RingIndex] = CosmeticRandom.FRandRange(32.0f, 64.0f);
		RingHeights[RingIndex] = 0.0f;
	}
	float Radius = 0.0f, HalfHeight = 0.0f;
	Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const FVector Base = Pawn->GetActorLocation() - FVector(0.0f, 0.0f, HalfHeight);
	const float MaxHeight = HalfHeight * 2.0f;
	const float FadeHeight = MaxHeight * 0.9f;
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		AWoTMeshEffect* Ring = Rings[i];
		if (!IsValid(Ring)) { continue; }
		RingHeights[i] += RingRiseRates[i] * DeltaSeconds;
		if (RingHeights[i] > MaxHeight)
		{
			Ring->Destroy();
			Rings[i] = nullptr;
			continue;
		}
		Ring->SetActorLocation(Base + FVector(0.0f, 0.0f, RingHeights[i]));
		Ring->SetEffectDrawScale(0.5f + 0.2f * RingHeights[i] / RingRiseRates[i]);
		if (RingHeights[i] > FadeHeight && MaxHeight > FadeHeight)
		{
			Ring->SetEffectGlow(1.0f -
				(RingHeights[i] - FadeHeight) / (MaxHeight - FadeHeight));
		}
	}
}

int32 AWoTPawnSkinOverlay::LiveRingCount() const
{
	int32 Count = 0;
	for (const AWoTMeshEffect* Ring : Rings) { if (IsValid(Ring)) { ++Count; } }
	return Count;
}

void AWoTPawnSkinOverlay::Destroyed()
{
	for (AWoTMeshEffect* Ring : Rings) { if (IsValid(Ring)) { Ring->Destroy(); } }
	Super::Destroyed();
}

AWoTWeaveSpriteEffect::AWoTWeaveSpriteEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(RootComponent);
}

void AWoTWeaveSpriteEffect::Configure(FName Config,
	                                  const FWoTSpriteEffectRow& Row,
	                                  AActor* Follow)
{
	ConfigName = Config;
	FollowActor = Follow;
	InitialLifeSpan = Row.LifeSpan;
	Sprite->DrawScale = Row.DrawScale;
	Sprite->ScaleGlow = Row.ScaleGlow;
	Sprite->FrameRate = Row.FrameRate;
	Sprite->bModulateOnly = Row.bModulated;
	if (!Row.FrameIdentities.IsEmpty())
	{
		Sprite->SetAnimatedTextureSeries(Row.FrameIdentities, Row.TexWidth,
			Row.TexHeight, Row.MinFrameRate, Row.MaxFrameRate);
	}
	else
	{
		TArray<FString> Names;
		for (FName Name : Row.Frames) { Names.Add(Name.ToString()); }
		Sprite->SetSpriteSeries(*Row.Package.ToString(), Names,
			Row.TexWidth, Row.TexHeight);
	}
}

void AWoTWeaveSpriteEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (FollowActor.IsValid())
	{
		SetActorLocation(FollowActor->GetActorLocation());
	}
}

AWoTMeshEffectBurst::AWoTMeshEffectBurst()
{
	bReplicates = true;
	bNetTemporary = true;
	SetReplicatingMovement(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	InitialLifeSpan = 4.0f;
}

void AWoTMeshEffectBurst::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTMeshEffectBurst, Config);
	DOREPLIFETIME(AWoTMeshEffectBurst, FollowActor);
}

void AWoTMeshEffectBurst::BeginPlay()
{
	Super::BeginPlay();
	if (GetWorld())
	{
		WoTMeshFX::SpawnLocal(*GetWorld(), Config, GetActorLocation(),
			GetActorRotation(), FollowActor);
	}
}

AWoTPawnOverlayBurst::AWoTPawnOverlayBurst()
{
	bReplicates = true;
	bNetTemporary = true;
	SetReplicatingMovement(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	InitialLifeSpan = 4.0f;
}

void AWoTPawnOverlayBurst::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTPawnOverlayBurst, Config);
	DOREPLIFETIME(AWoTPawnOverlayBurst, FollowPawn);
}

void AWoTPawnOverlayBurst::BeginPlay()
{
	Super::BeginPlay();
	if (GetWorld() && IsValid(FollowPawn))
	{
		WoTMeshFX::SpawnPawnOverlayLocal(*GetWorld(), Config, FollowPawn);
	}
}

AWoTTaintVisual::AWoTTaintVisual()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	CometA = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("CometA"));
	CometB = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("CometB"));
	CometC = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("CometC"));
	for (UWoTVertexAnimComponent* Comet : {CometA.Get(), CometB.Get(), CometC.Get()})
	{
		Comet->SetupAttachment(RootComponent);
		Comet->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comet->SetCastShadow(false);
	}
	Center = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Center"));
	Center->SetupAttachment(RootComponent);
	Center->DrawScale = 0.45f;
	Center->bModulateOnly = true;
	Glow = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Glow"));
	Glow->SetupAttachment(RootComponent);
	Glow->DrawScale = 0.09f;
	Glow->ScaleGlow = 5.0f;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
}

void AWoTTaintVisual::BeginPlay()
{
	Super::BeginPlay();
	UTexture* CometTexture = LoadTexture(TEXT("Angreal"), TEXT("TaintRedcomet3"));
	for (UWoTVertexAnimComponent* Comet : {CometA.Get(), CometB.Get(), CometC.Get()})
	{
		Comet->SetVertexMesh(TEXT("ExpWard"));
		Comet->SetRelativeScale3D(FVector(1.5f));
		if (UMaterialInstanceDynamic* MID = MakeMeshMaterial(
			this, Comet, EWoTMeshEffectStyle::Translucent))
		{
			MID->SetTextureParameterValue(TEXT("Tex"), CometTexture);
			// GUnlitColor = clamp(ScaleGlow*0.5 + AmbientGlow/256) = 1
			// (UE1's mesh renderer); the comets are plain meshes, not
			// bParticles, so ScaleGlow is not applied again.
			const float Unlit = UE1UnlitColor(ScaleGlow, AmbientGlow);
			MID->SetVectorParameterValue(TEXT("MLAmbient"),
				FLinearColor(Unlit, Unlit, Unlit, 1.0f));
			MID->SetScalarParameterValue(TEXT("MLDynEnable"), 0.0f);
			Comet->SetMaterial(0, MID);
		}
	}
	CometA->SetRelativeRotation(FRotator(0.0f, 0.0f, 0.0f));
	CometB->SetRelativeRotation(FRotator(0.0f, 90.0f, 0.0f));
	CometC->SetRelativeRotation(FRotator(90.0f, 0.0f, 0.0f));

	Center->SetSpriteMaterials(nullptr,
		UWoTRuntimeAssets::CreateModulatedSpriteMaterial(
			Center,
			FSoftObjectPath(),
			TEXT("MI_PSMod2x_Angreal__TntCntrA")), 64.0f, 64.0f,
		TEXT("Angreal__TntCntrA"));
	TArray<FString> GlowFrames;
	GlowFrames.Add(TEXT("T2"));
	Glow->SetSpriteSeries(TEXT("Angreal"), GlowFrames, 64.0f, 64.0f);
	WoTUE1LightRegistry::Configure(Light, TEXT("TaintVisual"));
}

void AWoTTaintVisual::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (FollowActor.IsValid())
	{
		SetActorLocation(FollowActor->GetActorLocation());
	}
	CometA->AddLocalRotation(FRotator(0.0f, 0.0f,
		100000.0f * RotUnitsToDegrees * DeltaSeconds));
	CometB->AddLocalRotation(FRotator(0.0f, 0.0f,
		60000.0f * RotUnitsToDegrees * DeltaSeconds));
	CometC->AddLocalRotation(FRotator(0.0f,
		-80000.0f * RotUnitsToDegrees * DeltaSeconds, 0.0f));
}

int32 AWoTTaintVisual::GetPartCount() const
{
	return (CometA ? 1 : 0) + (CometB ? 1 : 0) + (CometC ? 1 : 0)
		+ (Center ? 1 : 0) + (Glow ? 1 : 0);
}

const FWoTMeshEffectRow* WoTMeshFX::Find(FName Config)
{
	return ActiveMeshRegistry().Find(Config);
}

AWoTMeshEffect* WoTMeshFX::SpawnLocal(UWorld& World, FName Config,
	const FVector& Location, const FRotator& Rotation, AActor* Follow)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	const FWoTMeshEffectRow* Row = Find(Config);
	if (!Row) return nullptr;
	const FTransform At(Rotation, Location);
	AWoTMeshEffect* Effect = World.SpawnActorDeferred<AWoTMeshEffect>(
		AWoTMeshEffect::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Effect)
	{
		Effect->Configure(Config, *Row, Follow);
		Effect->FinishSpawning(At);
	}
	return Effect;
}

AWoTMeshEffectBurst* WoTMeshFX::SpawnBurstMarker(UWorld& World, FName Config,
	const FVector& Location, const FRotator& Rotation, AActor* Follow)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	const FTransform At(Rotation, Location);
	AWoTMeshEffectBurst* Marker = World.SpawnActorDeferred<AWoTMeshEffectBurst>(
		AWoTMeshEffectBurst::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Marker)
	{
		Marker->Config = Config;
		Marker->FollowActor = Follow;
		Marker->FinishSpawning(At);
	}
	return Marker;
}

const FWoTPawnOverlayRow* WoTMeshFX::FindPawnOverlay(FName Config)
{
	return ActivePawnOverlayRegistry().Find(Config);
}

AWoTPawnSkinOverlay* WoTMeshFX::SpawnPawnOverlayLocal(UWorld& World,
	FName Config, APawn* Pawn)
{
	if (!IsValid(Pawn) || World.bIsTearingDown
		|| World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	const FWoTPawnOverlayRow* Row = FindPawnOverlay(Config);
	if (!Row) return nullptr;
	const FTransform At(Pawn->GetActorTransform());
	AWoTPawnSkinOverlay* Overlay = World.SpawnActorDeferred<AWoTPawnSkinOverlay>(
		AWoTPawnSkinOverlay::StaticClass(), At, Pawn, Pawn,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Overlay)
	{
		Overlay->Configure(Config, *Row, Pawn);
		Overlay->FinishSpawning(At);
	}
	return Overlay;
}

AWoTPawnOverlayBurst* WoTMeshFX::SpawnPawnOverlayBurst(UWorld& World,
	FName Config, APawn* Pawn)
{
	if (!IsValid(Pawn) || World.bIsTearingDown || World.GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	const FTransform At(Pawn->GetActorTransform());
	AWoTPawnOverlayBurst* Marker = World.SpawnActorDeferred<AWoTPawnOverlayBurst>(
		AWoTPawnOverlayBurst::StaticClass(), At, Pawn, Pawn,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Marker)
	{
		Marker->Config = Config;
		Marker->FollowPawn = Pawn;
		Marker->FinishSpawning(At);
	}
	return Marker;
}

const FWoTSpriteEffectRow* WoTMeshFX::FindSprite(FName Config)
{
	return ActiveSpriteRegistry().Find(Config);
}

const FWoTTextureAnimationRow* WoTMeshFX::FindTextureAnimation(
	FStringView Identity)
{
	if (const TUniquePtr<FWoTMeshFXSet>& Installed = InstalledMeshFXRegistry())
		return Installed->TextureAnimations.Find(FString(Identity));
	return nullptr;
}

const FString* WoTMeshFX::FindClassTextureHead(FName Config)
{
	if (const TUniquePtr<FWoTMeshFXSet>& Installed = InstalledMeshFXRegistry())
		return Installed->ClassTextureHeads.Find(Config);
	return nullptr;
}

bool WoTMeshFX::ConfigureClassSprite(UWoTSpriteBillboardComponent& Sprite,
	FName Config, const float FixedFrameRate)
{
	const FString* Head = FindClassTextureHead(Config);
	const FWoTTextureAnimationRow* Animation = Head
		? FindTextureAnimation(*Head) : nullptr;
	if (Animation == nullptr || Animation->FrameIdentities.IsEmpty()) return false;
	const float MinRate = FixedFrameRate > 0.0f
		? FixedFrameRate : Animation->MinFrameRate;
	const float MaxRate = FixedFrameRate > 0.0f
		? FixedFrameRate : Animation->MaxFrameRate;
	Sprite.SetAnimatedTextureSeries(Animation->FrameIdentities,
		Animation->TexWidth, Animation->TexHeight, MinRate, MaxRate);
	return true;
}

TConstArrayView<FString> WoTMeshFX::LightningStreakTextures()
{
	if (const TUniquePtr<FWoTMeshFXSet>& Installed = InstalledMeshFXRegistry())
		return Installed->LightningStreakTextures;
	return {};
}

AWoTWeaveSpriteEffect* WoTMeshFX::SpawnSpriteLocal(UWorld& World, FName Config,
	const FVector& Location, AActor* Follow)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	const FWoTSpriteEffectRow* Row = FindSprite(Config);
	if (!Row) return nullptr;
	const FTransform At(Location);
	AWoTWeaveSpriteEffect* Effect = World.SpawnActorDeferred<AWoTWeaveSpriteEffect>(
		AWoTWeaveSpriteEffect::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Effect)
	{
		Effect->Configure(Config, *Row, Follow);
		Effect->FinishSpawning(At);
	}
	return Effect;
}

AWoTTaintVisual* WoTMeshFX::SpawnTaintVisual(UWorld& World, AActor* Follow)
{
	if (!Follow || World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	const FTransform At(Follow->GetActorLocation());
	AWoTTaintVisual* Visual = World.SpawnActorDeferred<AWoTTaintVisual>(
		AWoTTaintVisual::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Visual)
	{
		Visual->FollowActor = Follow;
		Visual->FinishSpawning(At);
	}
	return Visual;
}
