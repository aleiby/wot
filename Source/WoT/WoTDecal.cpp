#include "WoTDecal.h"
#include "WoTRandom.h"
#include "AI/WoTPathNodeIterator.h"
#include "WoTCollisionBlocker.h"
#include "WoTMover.h"
#include "Assets/WoTRuntimeAssets.h"
#include "WoTPreferencesSubsystem.h"
#include "Inventory/WoTAngrealInvGuardian.h"

#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/SoftObjectPath.h"
#include "WoTMeshModulate.h"
#include "WoTAppearEffect.h"
#include "WoTSpriteBillboard.h"
#include "WoTVertexAnimComponent.h"

// UE1 decal projection keeps the renderer behavior documented in Decal.uc and
// the original UDecalManagerRI; all activated row data is builder-owned.
namespace
{
	TUniquePtr<FWoTDecalSet>& InstalledDecalRegistry()
	{
		static TUniquePtr<FWoTDecalSet> Set;
		return Set;
	}

	// UE1 draw order (the original renderer's hardware (non-SpanBased) branch): every
	// non-STY_Translucent actor — each modulated decal — draws at the end of
	// pass 1, before the pass-2 NoOcclude BSP bin (translucent/modulated world
	// surfaces, the port's default priority 0); STY_Translucent actors draw in
	// pass 2 after those surfaces, the port's sprite bucket (90/91).
	constexpr int32 ModulatedDecalSortPriority = -1;
	constexpr int32 TranslucentDecalSortPriority = 90;

	const TMap<FName, FWoTDecalConfig>& ActiveDecalRegistry()
	{
		if (const TUniquePtr<FWoTDecalSet>& Installed = InstalledDecalRegistry())
			return Installed->Configs;
		static const TMap<FName, FWoTDecalConfig> Empty;
		return Empty;
	}

	AWoTDecal* SpawnConfigured(UWorld& World, FName Type, const FTransform& At,
	                           bool bReplicated, float DrawScaleOverride,
	                           FRandomStream* CosmeticRandom = nullptr)
	{
		if (World.bIsTearingDown
			|| (World.GetNetMode() == NM_DedicatedServer && !bReplicated))
		{
			return nullptr;
		}
		const FWoTDecalConfig* C = WoTDecalFX::Find(Type);
		if (!C || C->Textures.Num() == 0)
		{
			return nullptr;
		}
		if (bReplicated && C->bRegisters)
		{
			ensureMsgf(false,
				TEXT("Registered decal %s must replicate a placement event, not an actor"),
				*Type.ToString());
			return nullptr;
		}
		AWoTDecal* D = World.SpawnActorDeferred<AWoTDecal>(
		    AWoTDecal::StaticClass(), At, nullptr, nullptr,
		    ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!D)
		{
			return nullptr;
		}
		D->DecalType = Type;
		// BloodDecal.BeginPlay draws a random list entry; BalefireDecal.BeginPlay
		// starts its burn-in at AnimTextures[0].
		D->TextureIndex = static_cast<uint8>(
			C->FrameTime > 0.0f ? 0
				: CosmeticRandom ? CosmeticRandom->RandHelper(C->Textures.Num())
					: WoTRandom::RandHelper(C->Textures.Num()));
		D->FinalScale = DrawScaleOverride > 0.0f ? DrawScaleOverride : C->DrawScale;
		D->AmbientGlow = C->AmbientGlow;
		D->SetReplicates(bReplicated);
		D->FinishSpawning(At);
		// Lifespan 180-300 (Decal.uc PreBeginPlay); local per machine — the exact
		// expiry moment is cosmetic.
		D->SetLifeSpan(C->bRegisters ? 0.0f
			: CosmeticRandom ? CosmeticRandom->FRandRange(180.0f, 300.0f)
				: WoTRandom::FRandRange(180.0f, 300.0f));
		return D;
	}
}

AWoTDecalManager::AWoTDecalManager()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true;
}

void AWoTDecalManager::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTDecalManager, TagName);
	DOREPLIFETIME(AWoTDecalManager, MaxDecals);
	DOREPLIFETIME(AWoTDecalManager, ShrinkTime);
	DOREPLIFETIME(AWoTDecalManager, DecalExpireTime);
}

AWoTDecalManager* AWoTDecalManager::Find(UWorld* World, FName ManagerTag)
{
	if (!World) return nullptr;
	for (TActorIterator<AWoTDecalManager> It(World); It; ++It)
	{
		if (It->TagName == ManagerTag || It->Tags.Contains(ManagerTag)) return *It;
	}
	return nullptr;
}

void FWoTDecalBuilder::Install(FWoTDecalSet&& Set)
{
	InstalledDecalRegistry() = MakeUnique<FWoTDecalSet>(MoveTemp(Set));
}

#if WITH_AUTOMATION_TESTS
void FWoTDecalBuilder::Reset()
{
	InstalledDecalRegistry().Reset();
}
#endif

namespace WoTDecalFX
{
	const FWoTDecalConfig* Find(FName Type)
	{
		return Type.IsNone() ? nullptr : ActiveDecalRegistry().Find(Type);
	}

	float UnlitColor(float InScaleGlow, uint8 InAmbientGlow)
	{
		return FMath::Clamp(
			InScaleGlow * 0.5f + static_cast<float>(InAmbientGlow) / 256.0f,
			0.0f, 1.0f);
	}

	bool FindAlignedFit(UWorld& World, const FVector& Location,
		const FVector& Normal, float InitialScale, FVector& OutLocation,
		FRotator& OutRotation, float& OutScale, AActor* Ignore)
	{
		// Decal.uc:118-195. Keep the fit decision independent from rendering so
		// headless authority reaches the same four-corner surface test.
		const AWoTDecal* Defaults = GetDefault<AWoTDecal>();
		FHitResult Flush;
		if (!WoTVisBlockingTrace(&World, Location, Location - Normal * 16.0f,
			Ignore, Flush))
		{
			return false;
		}

		OutLocation = Flush.Location + Normal;
		OutRotation = Normal.Rotation();
		OutRotation.Roll = WoTRandom::FRandRange(0.0f, 360.0f);
		const FRotationMatrix Axes(OutRotation);
		const FVector X = Axes.GetScaledAxis(EAxis::X);
		const FVector Y = Axes.GetScaledAxis(EAxis::Y);
		const FVector Z = Axes.GetScaledAxis(EAxis::Z);
		float Scale = InitialScale;
		for (int32 Attempts = Defaults->MaxNumAttempts; Attempts > 0;
			--Attempts, Scale *= Defaults->RetryScale)
		{
			const float Width = Defaults->EdgeRadius * Scale;
			if (Width < Defaults->MinimumSize)
			{
				break;
			}
			const FVector Corners[4] = {
				OutLocation + Width * Z + Width * Y,
				OutLocation + Width * Z - Width * Y,
				OutLocation - Width * Z + Width * Y,
				OutLocation - Width * Z - Width * Y,
			};
			bool bFits = true;
			for (const FVector& Corner : Corners)
			{
				FHitResult Hit;
				if (!WoTVisBlockingTrace(&World, Corner,
					Corner - X * Defaults->TraceDist, Ignore, Hit)
					|| !Hit.GetActor() || Hit.GetActor()->IsA<AWoTMover>()
					|| (Hit.GetActor()->IsA<AWoTCollisionBlocker>()
						&& !CastChecked<AWoTCollisionBlocker>(Hit.GetActor())->bPlayerOnly))
				{
					bFits = false;
					break;
				}
			}
			if (bFits)
			{
				OutScale = Scale;
				return true;
			}
		}
		return false;
	}

	AWoTDecal* SpawnAligned(UWorld& World, FName Type, const FVector& Location,
	                        const FVector& Normal, bool bReplicated,
	                        float DrawScaleOverride)
	{
		AWoTDecal* D = SpawnConfigured(World, Type, FTransform(Location), bReplicated,
		                               DrawScaleOverride);
		if (D)
		{
			D->Align(Normal);
			if (!IsValid(D))
			{
				return nullptr;   // the fit failed and the actor destroyed itself
			}
		}
		return D;
	}

	AWoTDecal* SpawnAlignedFit(UWorld& World, FName Type,
		const FVector& Location, const FRotator& Rotation, float Scale,
		const FVector& Normal, bool bReplicated)
	{
		AWoTDecal* D = SpawnConfigured(World, Type,
			FTransform(Rotation, Location), bReplicated, Scale);
		if (D)
		{
			D->ApplyAlignedFit(Location, Rotation, Scale, Normal);
		}
		return D;
	}

	AWoTDecal* SpawnFlying(UWorld& World, FName Type, const FVector& Location,
	                       const FVector& InVelocity,
	                       FRandomStream* CosmeticRandom)
	{
		AWoTDecal* D = SpawnConfigured(World, Type, FTransform(Location),
		                               /*bReplicated*/ false, -1.0f,
		                               CosmeticRandom);
		if (D)
		{
			D->Velocity = InVelocity;
			D->bFlying = true;
			if (const FWoTDecalConfig* C = Find(Type); C && !C->FlyingTextures.IsEmpty())
			{
				// DecaySplatter.uc: DT_Sprite, DrawScale 0.3, Texture =
				// DropTextures[Rand(2)] — both 64x64 (world 19.2uu). It
				// inherits Decal.uc's STY_Modulated style.
				TArray<FString> Frame;
				const int32 FrameIndex = CosmeticRandom
					? CosmeticRandom->RandHelper(C->FlyingTextures.Num())
					: WoTRandom::RandHelper(C->FlyingTextures.Num());
				Frame.Add(C->FlyingTextures[FrameIndex].ToString());
				D->FlyingSprite->DrawScale = 0.3f;
				D->FlyingSprite->bModulateOnly =
					C->Style == EWoTDecalStyle::Modulated;
				D->FlyingSprite->SetSpriteSeries(*C->Package.ToString(), Frame, 64, 64);
				D->FlyingSprite->SetVisibility(true);
			}
			D->SetActorTickEnabled(true);
		}
		return D;
	}
}

AWoTDecal::AWoTDecal()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;   // flight + burn-in only
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	FlyingSprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("FlyingSprite"));
	FlyingSprite->SetupAttachment(RootComponent);
	FlyingSprite->SetVisibility(false);
	// Legend.DecalTRI, bound at placement (Project).
	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DecalMesh"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->SetVisibility(false);
	bReplicates = false;   // SpawnAligned flips it for authority-observed events
}

void AWoTDecal::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTDecal, DecalType);
	DOREPLIFETIME(AWoTDecal, TextureIndex);
	DOREPLIFETIME(AWoTDecal, FinalScale);
	DOREPLIFETIME(AWoTDecal, PlacedNormal);
	DOREPLIFETIME(AWoTDecal, PlacedLocation);
	DOREPLIFETIME(AWoTDecal, PlacedRotationEuler);
	DOREPLIFETIME(AWoTDecal, bPlaced);
}

float AWoTDecal::GetUnlitColor() const
{
	return WoTDecalFX::UnlitColor(ScaleGlow, AmbientGlow);
}

void AWoTDecal::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bPlaced)
	{
		TickDecalManager(DeltaSeconds);
		TickBurnIn(DeltaSeconds);
		return;
	}
	// The sprayer-thrown flight (Decal.uc Physics=PHYS_Falling + bCollideWorld):
	// fall until the world is hit, then align there.
	if (!bFlying)
	{
		return;
	}
	Velocity.Z += GetWorld()->GetGravityZ() * DeltaSeconds;
	FHitResult Hit;
	const FVector Start = GetActorLocation();
	const FVector End = Start + Velocity * DeltaSeconds;
	// Decal.uc:137 Trace(..., false): flight impacts only level/movers.
	if (WoTVisBlockingTrace(GetWorld(), Start, End, this, Hit))
	{
		SetActorLocation(Hit.Location + Hit.ImpactNormal);
		bFlying = false;
		Align(Hit.ImpactNormal);
		return;
	}
	SetActorLocation(End);
	// A decal that falls forever dies quietly (out-of-world).
	if (End.Z < -100000.0f)
	{
		Destroy();
	}
}

void AWoTDecal::TickBurnIn(float DeltaSeconds)
{
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	// BalefireDecal.uc Tick, first: if (!bSpawnedHole) Spawn(class'BFHole',,,
	// Location).Align(vector(Rotation)) — a local, unreplicated decal on every
	// machine that renders (a dedicated server's DecalManager discards it).
	if (bCompanionPending && C && !C->CompanionDecal.IsNone())
	{
		bCompanionPending = false;
		if (UWorld* World = GetWorld())
		{
			WoTDecalFX::SpawnAligned(*World, C->CompanionDecal, GetActorLocation(),
			                         PlacedNormal, /*bReplicated*/ false);
		}
	}
	if (!C || C->FrameTime <= 0.0f)
	{
		const FWoTDecalConfig* Config = WoTDecalFX::Find(DecalType);
		SetActorTickEnabled(Config && Config->bRegisters);
		return;
	}
	// BalefireDecal.uc Tick: one AnimTextures frame per FrameTime (the timer
	// resets rather than accumulates); past the last frame AmbientGlow drops to
	// 0 and the remaining life is FadeTime with ScaleGlow = LifeSpan / FadeTime.
	if (!bLastFrame)
	{
		FrameTimer -= DeltaSeconds;
		if (FrameTimer <= 0.0f)
		{
			FrameTimer = C->FrameTime;
			++AnimFrame;
			if (C->Textures.IsValidIndex(AnimFrame))
			{
				ApplyTexture(AnimFrame);
			}
			else
			{
				bLastFrame = true;
				AmbientGlow = 0;
				FadeRemaining = C->FadeTime;
				// Replicated copies are removed by the authority's expiry.
				if (HasAuthority())
				{
					SetLifeSpan(FMath::Max(C->FadeTime, KINDA_SMALL_NUMBER));
				}
				ApplyUnlitColor();
			}
		}
		return;
	}
	FadeRemaining = FMath::Max(0.0f, FadeRemaining - DeltaSeconds);
	ScaleGlow = C->FadeTime > 0.0f ? FadeRemaining / C->FadeTime : 0.0f;
	ApplyUnlitColor();
}

void AWoTDecal::Align(const FVector& Normal)
{
	FlyingSprite->SetVisibility(false);
	// Decal.uc:118-195. Flush back-trace 16uu against the normal; random roll; then
	// the 4-corner fit loop: corners of the EdgeRadius x scale square must all touch
	// the surface within TraceDist 5, else shrink x0.85; below MinimumSize 5 after 10
	// attempts -> destroy.
	UWorld* World = GetWorld();
	if (!World)
	{
		Destroy();
		return;
	}
	FVector FitLocation;
	FRotator PlacedRotation;
	float PlacedScale = FinalScale;
	if (!WoTDecalFX::FindAlignedFit(*World, GetActorLocation(), Normal,
		FinalScale, FitLocation, PlacedRotation, PlacedScale, this))
	{
		Destroy();
		return;
	}
	SetActorLocationAndRotation(FitLocation, PlacedRotation);
	FinalScale = PlacedScale;
	PlacedNormal = Normal;
	PlacedLocation = FitLocation;
	PlacedRotationEuler = FVector(PlacedRotation.Pitch, PlacedRotation.Yaw,
		PlacedRotation.Roll);
	bPlaced = 1;
	OnPlaced();
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	if (C && C->bRegisters)
	{
		EnforceBudget();
	}
}

void AWoTDecal::ApplyAlignedFit(const FVector& Location,
	const FRotator& Rotation, float Scale, const FVector& Normal)
{
	FlyingSprite->SetVisibility(false);
	SetActorLocationAndRotation(Location, Rotation);
	FinalScale = Scale;
	PlacedNormal = Normal;
	PlacedLocation = Location;
	PlacedRotationEuler = FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
	bPlaced = 1;
	OnPlaced();
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	if (C && C->bRegisters)
	{
		EnforceBudget();
	}
}

void AWoTDecal::OnRep_Placed()
{
	if (bPlaced)
	{
		SetActorLocationAndRotation(PlacedLocation, FRotator(
			PlacedRotationEuler.X, PlacedRotationEuler.Y, PlacedRotationEuler.Z));
		OnPlaced();
	}
}

void AWoTDecal::OnPlaced()
{
	if (bPlacedPresentation)
	{
		return;
	}
	bPlacedPresentation = true;
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	if (!C)
	{
		return;
	}
	// The class defaults the burn-in starts from (replicated copies construct
	// with the AWoTDecal defaults, so every machine restates them here).
	AmbientGlow = C->AmbientGlow;
	ScaleGlow = 1.0f;
	AnimFrame = TextureIndex;
	FrameTimer = C->FrameTime;
	bLastFrame = false;
	if (DecalType == TEXT("MashadarCrack"))
	{
		// MashadarCrack.Tick is simulated: every machine creates this cosmetic on
		// its own first placed frame, including clients that receive the persistent
		// crack after an authority-side effect actor would have expired.
		const AWoTAngrealInvGuardian* GuardianDefaults =
			GetDefault<AWoTAngrealInvGuardian>();
		AWoTAppearEffect::SpawnLocalWithSprites(*GetWorld(), this,
			GuardianDefaults->TopSparkle, GuardianDefaults->BottomSparkle,
			1.5f, 20.0f, 20.0f);
	}
	if (C->bRegisters)
	{
		RegisteredManager = AWoTDecalManager::Find(GetWorld());
		if (const AWoTDecalManager* Manager = RegisteredManager.Get())
		{
			ManagerRemaining = FMath::Max(Manager->DecalExpireTime, KINDA_SMALL_NUMBER);
			ManagerShrinkTime = FMath::Clamp(Manager->ShrinkTime, 0.0f, ManagerRemaining);
			ManagerMaxDecals = FMath::Max(0, Manager->MaxDecals);
		}
	}
	Project();
	bCompanionPending = !C->CompanionDecal.IsNone()
		&& GetNetMode() != NM_DedicatedServer;
	if (C->FrameTime > 0.0f || bCompanionPending || C->bRegisters)
	{
		// Every machine runs the burn-in (the authority for its expiry) and
		// the first-Tick companion spawn.
		SetActorTickEnabled(true);
	}
}

void AWoTDecal::TickDecalManager(float DeltaSeconds)
{
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	if (!C || !C->bRegisters)
	{
		return;
	}
	ManagerRemaining -= DeltaSeconds;
	if (ManagerShrinkTime > 0.0f)
	{
		const float Scale = FMath::Clamp(ManagerRemaining / ManagerShrinkTime, 0.0f, 1.0f);
		if (GetNetMode() != NM_DedicatedServer && Mesh)
			Mesh->SetRelativeScale3D(FVector(FinalScale * Scale));
		if (ManagerRemaining <= 0.0f)
		{
			if (HasAuthority()) Destroy();
			else if (Mesh) Mesh->SetVisibility(false);
			return;
		}
	}
	APlayerController* Viewer = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* Camera = Viewer ? Viewer->PlayerCameraManager : nullptr;
	bool bVisible = false;
	if (Camera)
	{
		const FVector Delta = GetActorLocation() - Camera->GetCameraLocation();
		const float RadiusDistance = Delta.Size2D();
		const float HeightDistance = FMath::Abs(Delta.Z);
		const float ViewCos = Delta.GetSafeNormal() | Camera->GetCameraRotation().Vector();
		uint8 GoreDetail = 2;
		uint8 MaxDetail = 3;
		if (const UWoTPreferencesSubsystem* Preferences = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr)
		{
			GoreDetail = Preferences->GetGameSettings().GoreDetailLevel;
			MaxDetail = static_cast<uint8>(Preferences->GetGameSettings().GraphicsQuality);
		}
		bVisible = ViewCos > 0.57358f
			&& (C->VisibilityRadius <= 0.0f || RadiusDistance < C->VisibilityRadius)
			&& (C->VisibilityHeight <= 0.0f || HeightDistance < C->VisibilityHeight)
			&& C->DetailLevel <= MaxDetail && C->GoreLevel <= GoreDetail;
	}
	if (GetNetMode() != NM_DedicatedServer && Mesh)
		Mesh->SetVisibility(bVisible && DecalMaterial != nullptr);
	if (ManagerShrinkTime <= 0.0f && ManagerRemaining <= 0.0f)
	{
		if (bVisible) { ManagerRemaining = 0.1f; }
		else if (HasAuthority()) { Destroy(); }
		else if (Mesh) { Mesh->SetVisibility(false); }
	}
}

void AWoTDecal::Project()
{
	if (GetNetMode() == NM_DedicatedServer || !Mesh || DecalMaterial)
	{
		return;
	}
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	if (!C || !C->Textures.IsValidIndex(TextureIndex))
	{
		return;
	}
	// Legend.DecalTRI (Decal.uc #exec MESH IMPORT + MESHMAP SCALE 0.1/0.1/0.2):
	// two two-sided triangles spanning the mesh YZ plane (raw Y +-307.5 x 0.1,
	// Z +-153.5 x 0.2 = a 61.5uu square at DrawScale 1), so the aligned actor's
	// +X (the surface normal) faces it out of the surface 1uu off the wall.
	if (!Mesh->SetVertexMesh(FName(TEXT("DecalTRI")), FName(TEXT("Legend"))))
	{
		return;
	}
	Mesh->SetRelativeScale3D(FVector(FinalScale));
	if (C->Style == EWoTDecalStyle::Modulated)
	{
		// M_WoT_MeshModulate: FB = 2*Tex*Dst from the raw skin, no ML inputs,
		// no fog (PF_Modulated skips the fog pass in the original renderer).
		DecalMaterial = WoTMeshModulate::CreateInstance(this, nullptr);
	}
	else
	{
		// M_WoT_MeshTranslucent: FB = Src + Dst*(1-max3(Src)), Src = Tex*MLAmbient.
		UMaterialInterface* Master = Cast<UMaterialInterface>(FSoftObjectPath(
			TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent."
			     "M_WoT_MeshTranslucent")).TryLoad());
		DecalMaterial = Master ? UMaterialInstanceDynamic::Create(Master, this) : nullptr;
	}
	if (!DecalMaterial)
	{
		return;
	}
	ApplyTexture(TextureIndex);
	ApplyUnlitColor();
	Mesh->SetMaterial(0, DecalMaterial);
	Mesh->SetTranslucentSortPriority(C->Style == EWoTDecalStyle::Translucent
		? TranslucentDecalSortPriority : ModulatedDecalSortPriority);
	Mesh->SetVisibility(true);
}

void AWoTDecal::ApplyTexture(int32 Index)
{
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	if (!DecalMaterial || !C || !C->Textures.IsValidIndex(Index))
	{
		return;
	}
	// Raw display bytes, bilinear, all mips — the mesh masters sample linear.
	FWoTRuntimeTextureOptions Options;
	Options.bLoadMips = true;
	Options.bSRGB = false;
	Options.Filter = TF_Bilinear;
	const FString Pkg = C->Package.ToString();
	const FString Tex = C->Textures[Index].ToString();
	if (UTexture2D* Skin = UWoTRuntimeAssets::ResolveTexturePath(FSoftObjectPath(
		FString::Printf(TEXT("/Game/WoT/Textures/%s/%s.%s"), *Pkg, *Tex, *Tex)),
		Options))
	{
		DecalMaterial->SetTextureParameterValue(TEXT("Tex"), Skin);
	}
}

void AWoTDecal::ApplyUnlitColor()
{
	const FWoTDecalConfig* C = WoTDecalFX::Find(DecalType);
	if (!DecalMaterial || !C || C->Style != EWoTDecalStyle::Translucent)
	{
		return;   // PF_Modulated draws the texture alone (no vertex colour)
	}
	const float Unlit = GetUnlitColor();
	DecalMaterial->SetVectorParameterValue(TEXT("MLAmbient"),
		FLinearColor(Unlit, Unlit, Unlit, 1.0f));
}

void AWoTDecal::EnforceBudget()
{
	// UDecalManagerRI::AddDecal/First: prune the oldest registered decal past the
	// selected map manager's cap. Unregistered special decals never enter it.
	TArray<AWoTDecal*> Live;
	for (TActorIterator<AWoTDecal> It(GetWorld()); It; ++It)
	{
		const FWoTDecalConfig* Config = WoTDecalFX::Find(It->DecalType);
		if (IsValid(*It) && It->IsPlaced() && Config && Config->bRegisters
			&& It->RegisteredManager == RegisteredManager)
		{
			Live.Add(*It);
		}
	}
	while (Live.Num() > ManagerMaxDecals)
	{
		Live.Sort([](const AWoTDecal& A, const AWoTDecal& B) {
			return A.GetGameTimeSinceCreation() > B.GetGameTimeSinceCreation();
		});
		Live[0]->Destroy();
		Live.RemoveAt(0);
	}
}
