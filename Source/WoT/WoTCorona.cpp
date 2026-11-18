#include "WoTCorona.h"
#include "AI/WoTPathNodeIterator.h"

#include "Algo/BinarySearch.h"
#include "Assets/WoTRuntimeAssets.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "WoTLeafMap.h"

// Replay harness sets this so golden-pose shots get steady-state coronas
// instead of catching the first frames of the fade-in.
static TAutoConsoleVariable<int32> CVarWoTCoronaSnap(
	TEXT("wot.CoronaSnap"), 0,
	TEXT("1 = corona brightness snaps to its target instead of fading."));

static TAutoConsoleVariable<int32> CVarWoTCoronaDebug(
	TEXT("wot.CoronaDebug"), 0,
	TEXT("1 = log visible coronas (name, bright, dist) once a second."));

// Pass isolation for parity debugging: the corona pair splits UE1's screen
// blend into a modulate and an additive sprite; these hide either half so a
// capture can measure each pass's contribution on its own.
static TAutoConsoleVariable<int32> CVarWoTCoronaAdd(
	TEXT("wot.CoronaAdd"), 1,
	TEXT("0 = hide the additive corona sprite."));

static TAutoConsoleVariable<int32> CVarWoTCoronaMod(
	TEXT("wot.CoronaMod"), 1,
	TEXT("0 = hide the modulate corona sprite."));

AWoTCorona::AWoTCorona()
{
	PrimaryActorTick.bCanEverTick = true;

	Sprite = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sprite"));
	RootComponent = Sprite;
	SpriteMod = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SpriteMod"));
	SpriteMod->SetupAttachment(Sprite);   // inherits billboard + scale
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(
		TEXT("/Engine/BasicShapes/Plane.Plane"));
	for (UStaticMeshComponent* C : {Sprite.Get(), SpriteMod.Get()})
	{
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		if (Plane.Succeeded())
		{
			C->SetStaticMesh(Plane.Object);
		}
	}
	// UE1 order: FB = (Dst * (1-Src)) + Src — modulate first, add second.
	SpriteMod->SetTranslucentSortPriority(100);
	Sprite->SetTranslucentSortPriority(101);
}

void AWoTCorona::BeginPlay()
{
	Super::BeginPlay();
	if (bLeafGated)
	{
		for (TActorIterator<AWoTLeafMap> It(GetWorld()); It; ++It)
		{
			LeafMap = *It;
			break;
		}
	}
	EnsureRuntimeRenderState();
	Sprite->SetWorldScale3D(FVector(0.01f));   // invisible until first tick
}

void AWoTCorona::EnsureRuntimeRenderState()
{
	if (!bRuntimeMaterialsBound && !SkinSubPath.IsNone())
	{
		const FString SubPath = SkinSubPath.ToString();
		FString Package;
		FString Name;
		if (SubPath.Split(TEXT("/"), &Package, &Name,
			ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			FWoTRuntimeTextureOptions Options;
			Options.bSRGB = false;
			Options.Filter = TF_Bilinear;
			UTexture2D* Texture = UWoTRuntimeAssets::ResolveTexturePath(
				FSoftObjectPath(FString::Printf(
					TEXT("/Game/WoT/Textures/%s/%s.%s"),
					*Package, *Name, *Name)), Options);
			// The corona masters are neutral support roots cooked into every
			// build (Tools/dist/build_stripped_pak.py NEUTRAL_SUPPORT_ROOT_PACKAGES).
			// ResolveMaterialPath serves only the authenticated MI_* derivation
			// grammar under /Materials/Instances/ and returns null for a master
			// path in a guarded build (WoTRuntimeAssets.cpp ResolveDerivedMaterial),
			// so every guarded corona fell back to the Plane's default material
			// and drew nothing - the same defect WoTRainCyd had.
			// Load the masters directly, like WoTMeshFX and WoTRainCyd.
			UMaterialInterface* Add = Cast<UMaterialInterface>(FSoftObjectPath(
				TEXT("/Game/WoT/Materials/M_WoT_CoronaAdd.M_WoT_CoronaAdd")).TryLoad());
			UMaterialInterface* Mod = Cast<UMaterialInterface>(FSoftObjectPath(
				TEXT("/Game/WoT/Materials/M_WoT_CoronaMod.M_WoT_CoronaMod")).TryLoad());
			if (Texture && Add && Mod)
			{
				Mid = UMaterialInstanceDynamic::Create(Add, this);
				MidMod = UMaterialInstanceDynamic::Create(Mod, this);
				for (UMaterialInstanceDynamic* Material : {Mid.Get(), MidMod.Get()})
				{
					Material->SetTextureParameterValue(TEXT("Tex"), Texture);
					Material->SetVectorParameterValue(TEXT("Color"), Color);
					Material->SetScalarParameterValue(TEXT("Bright"), 0.0f);
				}
				Sprite->SetMaterial(0, Mid);
				SpriteMod->SetMaterial(0, MidMod);
				bRuntimeMaterialsBound = true;
			}
		}
	}
	if (!Mid)
	{
		if (UMaterialInterface* Mat = Sprite->GetMaterial(0))
			Mid = Sprite->CreateDynamicMaterialInstance(0, Mat);
	}
	if (!MidMod)
	{
		if (UMaterialInterface* Mat = SpriteMod->GetMaterial(0))
			MidMod = SpriteMod->CreateDynamicMaterialInstance(0, Mat);
	}
	for (UMaterialInstanceDynamic* M : {Mid.Get(), MidMod.Get()})
	{
		if (M)
		{
			M->SetVectorParameterValue(TEXT("Color"), Color);
			M->SetScalarParameterValue(TEXT("Bright"), 0.0f);
		}
	}
	UWorld* World = GetWorld();
	for (UStaticMeshComponent* Component : {Sprite.Get(), SpriteMod.Get()})
		if (Component && Component->IsRegistered())
		{
			// Guarded map projection runs from UEngine::LoadMap BEFORE
			// UWorld::InitWorld allocates the world's scene (the
			// guarded-bootstrap.patch hook precedes UnrealEngine.cpp:16590;
			// World.cpp:2484 AllocateScene), so the sprites of a projected
			// corona register with NO render state, and InitWorld's component
			// update skips already-registered components. MarkRenderStateDirty
			// only recreates an EXISTING state: every guarded corona stayed
			// registered, material-bound and "visible" yet absent from the scene
			// (packaged probe after BeginPlay: registered=1 render_state=0) -
			// the late-bind defect fixed for UWoTVertexAnimComponent.
			// Create the missing state (publication boundary and BeginPlay)
			// once a scene exists.
			if (!Component->IsRenderStateCreated() && World && World->Scene)
			{
				Component->RecreateRenderState_Concurrent();
			}
			else
			{
				Component->MarkRenderStateDirty();
				Component->MarkRenderTransformDirty();
			}
		}
}

void AWoTCorona::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (!Cam)
	{
		return;
	}
	const FVector CamLoc = Cam->GetCameraLocation();
	const FVector Loc = GetActorLocation();

	// UE1 GAddCorona is only reached for lights in the viewer PAWN leaf's
	// baked iPermeating run (retail draws nothing when the viewer has no
	// leaf); un-baked maps fall back to the light-radius distance gate.
	bool bVisible;
	if (bLeafGated && LeafMap.IsValid())
	{
		const int32 Leaf = LeafMap->GetViewerLeaf();
		bVisible = Leaf != INDEX_NONE
			&& Algo::BinarySearch(PermeatingLeaves, Leaf) != INDEX_NONE;
		// GAddCorona's SingleLineCheck(camera->light, TRACE_VisBlocking) runs
		// with bCheckActors=0: the shipped BSP alone, where only CSG-solid
		// volume blocks. Movers, meshes, pawns, and non-solid translucent/
		// masked/portal sheets never hide a corona — an engine collision
		// trace against the imported mesh blocks on all of those.
		if (bVisible)
		{
			bVisible = LeafMap->LineCheck(CamLoc, Loc);
		}
	}
	else
	{
		bVisible = FVector::DistSquared(Loc, CamLoc) <= FMath::Square(Reach);
		if (bVisible)
		{
			// SingleLineCheck TRACE_VisBlocking, Start = camera, End = light.
			FHitResult Hit;
			bVisible = !WoTVisBlockingTrace(GetWorld(), CamLoc, Loc, this, Hit);
		}
	}

	// Delta = 3*dt; in: Bright += 2*Delta (to 1), out: Bright -= Delta (to 0).
	const float Delta = 3.0f * DeltaSeconds;
	const float Target = bVisible ? 1.0f : 0.0f;
	if (CVarWoTCoronaSnap.GetValueOnGameThread())
	{
		Bright = Target;
	}
	else if (bVisible)
	{
		Bright = FMath::Min(1.0f, Bright + 2.0f * Delta);
	}
	else
	{
		Bright = FMath::Max(0.0f, Bright - Delta);
	}

	const FVector ToCam = CamLoc - Loc;
	const FRotationMatrix CamBasis(Cam->GetCameraRotation());
	const FVector CamDir = CamBasis.GetUnitAxis(EAxis::X);
	const float ViewZ = FMath::Max(1.0f, FVector::DotProduct(Loc - CamLoc, CamDir));

	// Billboard: VIEW-PLANE aligned, not viewpoint-facing. UE1 draws the
	// corona as a screen-space DrawTile — an axis-aligned rectangle of fixed
	// pixel size anywhere on screen. A viewpoint-facing quad (MakeFromZ(ToCam))
	// projects anisotropically off-axis (1/cos tangential, 1/cos^2 radial);
	// only a quad perpendicular to the camera AXIS with width proportional to
	// view DEPTH projects to the same rectangle at every screen position.
	// Plane +Z looks back along the camera axis, +X tracks camera right.
	Sprite->SetWorldRotation(
		FRotationMatrix::MakeFromZX(-CamDir, CamBasis.GetUnitAxis(EAxis::Y)).Rotator());

	// Screen-constant size: UE1 draws 512*DrawScale/640 of screen width.
	// World width at view DEPTH z for fraction f: f * 2*z*tan(HFOV/2); the
	// engine Plane is 100uu, so scale = worldWidth/100. Depth is ViewZ, NOT
	// the euclidean camera->light distance — perspective divides by z, and
	// sizing by distance rendered off-axis coronas 1/cos(theta) too wide
	// (the measured "+30% hot mid-band" family in the corona photometric
	// truth table: A_ours tracked 1/cos^2 across all skins, screen-center
	// rows sat at 1.00).
	const float Dist = FMath::Max(1.0f, ToCam.Size());
	const float HalfFovTan = FMath::Tan(FMath::DegreesToRadians(Cam->GetFOVAngle()) * 0.5f);
	const float WorldWidth = (512.0f * DrawScale / 640.0f) * 2.0f * ViewZ * HalfFovTan;
	Sprite->SetWorldScale3D(FVector(WorldWidth / 100.0f));

	Sprite->SetVisibility(CVarWoTCoronaAdd.GetValueOnGameThread() != 0);
	SpriteMod->SetVisibility(CVarWoTCoronaMod.GetValueOnGameThread() != 0);

	// Color *= Bright * min(1, ProjZ/viewZ).
	const float RZ = FMath::Min(1.0f, BrightZ / FMath::Max(1.0f, ViewZ));
	for (UMaterialInstanceDynamic* M : {Mid.Get(), MidMod.Get()})
	{
		if (M)
		{
			M->SetScalarParameterValue(TEXT("Bright"), Bright * RZ);
		}
	}
	if (CVarWoTCoronaDebug.GetValueOnGameThread() && Bright > 0.01f)
	{
		DebugLogAccum += DeltaSeconds;
		if (DebugLogAccum >= 1.0f)
		{
			DebugLogAccum = 0.0f;
			UE_LOG(LogTemp, Display,
			       TEXT("CORONA-DBG %s bright=%.2f rz=%.2f dist=%.0f at %s"),
			       *GetName(), Bright, RZ, Dist, *Loc.ToCompactString());
		}
	}
}
