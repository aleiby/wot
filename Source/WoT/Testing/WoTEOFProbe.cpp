#include "WoTEOFProbe.h"

#include "../WoTMover.h"
#include "../WoTSkyBox.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "UnrealClient.h"

// The whole rig lives far above the host map's own content so nothing from
// the map can occlude or light it.
static const FVector GProbeRig(0, 0, 50000);

AWoTEOFProbeShape::AWoTEOFProbeShape()
{
	PrimaryActorTick.bCanEverTick = true;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	// All render state is final at construction time, so registration carries
	// it — only registration-time state is in question here.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ShapeMat(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (ShapeMat.Succeeded())
	{
		Mesh->SetMaterial(0, ShapeMat.Object);
	}
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
}

void AWoTEOFProbeShape::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bTickTransform)
	{
		return;
	}
	Age += DeltaSeconds;
	// Same call as AWoTSkyBox::Tick — full-transform set, every frame, with a
	// genuine (small) change so nothing can early-out on an identical transform.
	const FVector Loc = Home + FVector(0, 0, 5.0 * FMath::Sin(Age * 2.0));
	SetActorTransform(FTransform(FRotator::ZeroRotator, Loc, FVector(2.0)));
}

bool UWoTEOFProbeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if WOT_GUARDED_BOOTSTRAP && UE_BUILD_SHIPPING
	(void)Outer;
	return false;
#else
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || World->WorldType != EWorldType::Game)
	{
		return false;
	}
	FString Dir;
	return FParse::Value(FCommandLine::Get(), TEXT("eofrepro="), Dir);
#endif
}

void UWoTEOFProbeSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FParse::Value(FCommandLine::Get(), TEXT("eofrepro="), OutDir);
	// -eofmode=none: no post-registration mutations at all (baseline — all
	// four shapes must render identically). -eofmode=sky: spawn NOTHING (no
	// sun/floor/shapes) — the camera alone at the rig point, so the frame is
	// purely whatever the map's ticking AWoTSkyBox paints there; the camera
	// teleports +2000 X between shot1 and shot2 to mimic a pose step.
	// Default: mutations on.
	FString Mode;
	FParse::Value(FCommandLine::Get(), TEXT("eofmode="), Mode);
	CloneMode = Mode.StartsWith(TEXT("clone")) ? Mode : FString();
	const bool bMutate = (Mode == TEXT("") || Mode == TEXT("full"));
	// mover keeps the rig: the sun and the bright floor slab give the
	// teleported-in mover light and a contrasting background.
	bSkyOnly = !bMutate && Mode != TEXT("none") && Mode != TEXT("mover");
	UE_LOG(LogTemp, Display, TEXT("EOFPROBE: active, outdir=%s mode=%s mutate=%d skyonly=%d"),
	       *OutDir, *Mode, bMutate ? 1 : 0, bSkyOnly ? 1 : 0);

	// Movable sun so lit shapes are visible with zero baked data. Mobility is
	// set pre-FinishSpawning: registration sees final state (the clean path).
	if (!bSkyOnly)
	{
		const FTransform SunXf(FRotator(-46.0, 30.0, 0.0), GProbeRig);
		ADirectionalLight* Sun = InWorld.SpawnActorDeferred<ADirectionalLight>(
			ADirectionalLight::StaticClass(), SunXf);
		if (Sun)
		{
			Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
			Sun->FinishSpawning(SunXf);
		}
	}

	auto Spawn = [&InWorld](const TCHAR* MeshPath, const FVector& Off,
	                        const FVector& Scale) -> AWoTEOFProbeShape* {
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTEOFProbeShape* C = InWorld.SpawnActor<AWoTEOFProbeShape>(
			AWoTEOFProbeShape::StaticClass(),
			FTransform(FRotator::ZeroRotator, GProbeRig + Off, Scale), P);
		if (C)
		{
			// SetStaticMesh routes through FComponentRecreateRenderStateContext
			// (a full unregister/re-register), NOT the end-of-frame update path
			// — the PlaceSkyClone recipe proved this safe in this project.
			UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, MeshPath);
			C->Mesh->SetStaticMesh(M);
			C->Home = GProbeRig + Off;
		}
		return C;
	};

	if (!bSkyOnly)
	{
		// Floor slab: spawn-time state, never touched. Reference surface.
		Spawn(TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(600, 0, -60),
		      FVector(40, 40, 1));
		// The four test subjects, distinct silhouettes, left to right on screen:
		ControlShape = Spawn(TEXT("/Engine/BasicShapes/Cube.Cube"),
		                     FVector(600, -375, 60), FVector(2));
		OnceShape = Spawn(TEXT("/Engine/BasicShapes/Sphere.Sphere"),
		                  FVector(600, -125, 60), FVector(2));
		TickShape = Spawn(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"),
		                  FVector(600, 125, 60), FVector(2));
		if (TickShape && bMutate)
		{
			TickShape->bTickTransform = true;
		}
		MatShape = Spawn(TEXT("/Engine/BasicShapes/Cone.Cone"),
		                 FVector(600, 375, 60), FVector(2));
		// JUMP subject: starts 100000uu off in +X (OUTSIDE the view frustum —
		// the one subject whose registration is out-of-view), teleported into
		// frame (rig +(600, 625, 60)) by Mutate(). A sphere: the sun paints a
		// bright cap on it, so presence is unambiguous against the black sky.
		JumpShape = Spawn(TEXT("/Engine/BasicShapes/Sphere.Sphere"),
		                  FVector(100600, 625, 60), FVector(2));

		// SKYCLONE subject: the map's sky mesh at the map's SkyScale on a
		// plain AStaticMeshActor, configured exactly like PlaceSkyClone
		// (mobility -> mesh -> collision, nothing else). Spawned 20000uu off
		// in +X; Mutate() teleports it onto the camera.
		for (TActorIterator<AWoTSkyBox> It(&InWorld); It; ++It)
		{
			UStaticMeshComponent* BoxSMC = It->GetStaticMeshComponent();
			if (!BoxSMC || !BoxSMC->GetStaticMesh())
			{
				continue;
			}
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AStaticMeshActor* Clone = InWorld.SpawnActor<AStaticMeshActor>(
				AStaticMeshActor::StaticClass(),
				FTransform(It->SkyRotation, GProbeRig + FVector(20000, 0, 0),
				           FVector(It->SkyScale)),
				P);
			if (Clone)
			{
				UStaticMeshComponent* SMC = Clone->GetStaticMeshComponent();
				SMC->SetMobility(EComponentMobility::Movable);
				SMC->SetStaticMesh(BoxSMC->GetStaticMesh());
				SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				SkyCloneActor = Clone;
			}
			break;
		}
	}

	Camera = InWorld.SpawnActor<ACameraActor>(
		ACameraActor::StaticClass(),
		FTransform(FRotator::ZeroRotator, GProbeRig + FVector(-200, 0, 60)));
	if (APlayerController* PC = InWorld.GetFirstPlayerController())
	{
		// Leave the pawn alive: it spawned near the map's own content 50000uu
		// below the rig, safely out of frame. Killing it put the game mode
		// into its respawn flow, which retargeted the camera mid-run
		// (measured: view rot yaw 90.55 on Battle4_01).
		if (Camera)
		{
			PC->SetViewTarget(Camera);
		}
		if (AHUD* HUD = PC->GetHUD())
		{
			HUD->bShowHUD = false;
		}
	}

	FTimerManager& TM = InWorld.GetTimerManager();
	if (bMutate)
	{
		TM.SetTimer(MutateTimer, this, &UWoTEOFProbeSubsystem::Mutate, 1.0f);
	}
	if (Mode == TEXT("sky"))
	{
		TM.SetTimer(MutateTimer, this, &UWoTEOFProbeSubsystem::TeleportCamera, 5.0f);
	}
	// boxfix: leave the placed box alone except ONE MarkRenderStateDirty() at
	// t=1s. The placed box's component reports renderstatecreated=1 but
	// SceneProxy=null (measured) — if the dirty-mark materializes the proxy
	// and the sky appears, the load-time proxy creation failed once (mesh
	// still async-compiling?) and was simply never retried.
	if (Mode == TEXT("boxfix") || Mode == TEXT("reregister"))
	{
		bReregister = (Mode == TEXT("reregister"));
		for (TActorIterator<AWoTSkyBox> It(&InWorld); It; ++It)
		{
			UStaticMeshComponent* SMC = It->GetStaticMeshComponent();
			if (SMC && SMC->GetStaticMesh())
			{
				UE_LOG(LogTemp, Display,
				       TEXT("EOFPROBE: boxfix mesh compiling %d proxy %d"),
				       SMC->GetStaticMesh()->IsCompiling() ? 1 : 0,
				       SMC->SceneProxy ? 1 : 0);
			}
		}
		TM.SetTimer(MutateTimer, this, &UWoTEOFProbeSubsystem::FixBox, 1.0f);
	}

	// propdiff: reflect over EVERY property of the level-saved box's component
	// and actor vs a runtime-spawned twin (which provably renders) and log the
	// values that differ. The saved box stays invisible through proxy
	// recreation AND full component reregistration, so whatever kills it must
	// be serialized state — enumerate it instead of guessing flags.
	if (Mode == TEXT("propdiff"))
	{
		for (TActorIterator<AWoTSkyBox> It(&InWorld); It; ++It)
		{
			AWoTSkyBox* Box = *It;
			UStaticMeshComponent* BoxSMC = Box->GetStaticMeshComponent();
			if (!BoxSMC || !BoxSMC->GetStaticMesh())
			{
				continue;
			}
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AWoTSkyBox* Twin = InWorld.SpawnActor<AWoTSkyBox>(
				AWoTSkyBox::StaticClass(),
				Box->GetActorTransform(), P);
			if (!Twin)
			{
				break;
			}
			Twin->SkyRotation = Box->SkyRotation;
			Twin->SkyScale = Box->SkyScale;
			UStaticMeshComponent* TwinSMC = Twin->GetStaticMeshComponent();
			TwinSMC->SetMobility(EComponentMobility::Movable);
			TwinSMC->SetStaticMesh(BoxSMC->GetStaticMesh());
			auto DiffObjects = [](const TCHAR* Tag, UObject* A, UObject* B) {
				int32 NumDiff = 0;
				for (TFieldIterator<FProperty> PropIt(A->GetClass()); PropIt; ++PropIt)
				{
					const FProperty* Prop = *PropIt;
					FString ValA, ValB;
					Prop->ExportTextItem_InContainer(ValA, A, nullptr, A, PPF_None);
					Prop->ExportTextItem_InContainer(ValB, B, nullptr, B, PPF_None);
					if (ValA != ValB)
					{
						NumDiff++;
						UE_LOG(LogTemp, Display,
						       TEXT("EOFPROBE: DIFF %s %s: saved=%s runtime=%s"),
						       Tag, *Prop->GetName(),
						       *ValA.Left(160), *ValB.Left(160));
					}
				}
				UE_LOG(LogTemp, Display, TEXT("EOFPROBE: propdiff %s: %d differing"),
				       Tag, NumDiff);
			};
			DiffObjects(TEXT("SMC"), BoxSMC, TwinSMC);
			DiffObjects(TEXT("ACTOR"), Box, Twin);
			break;
		}
	}

	// mover: teleport a LEVEL-SAVED AWoTMover into the rig camera's empty view
	// between the shots — the level-saved-Movable transport check with
	// guaranteed line of sight (the first version filmed it in situ and never
	// established the mover was in view at all).
	if (Mode == TEXT("mover"))
	{
		for (TActorIterator<AWoTMover> It(&InWorld); It; ++It)
		{
			UStaticMeshComponent* SMC =
				It->FindComponentByClass<UStaticMeshComponent>();
			if (!SMC || !SMC->GetStaticMesh())
			{
				continue;
			}
			MoverActor = *It;
			const FBoxSphereBounds B = SMC->Bounds;
			UE_LOG(LogTemp, Display,
			       TEXT("EOFPROBE: mover %s at %s bounds r %.1f mobility %d overrides %d"),
			       *MoverActor->GetName(), *B.Origin.ToCompactString(),
			       B.SphereRadius, int(SMC->Mobility.GetValue()),
			       SMC->OverrideMaterials.Num());
			for (UMaterialInterface* M : SMC->OverrideMaterials)
			{
				UE_LOG(LogTemp, Display, TEXT("EOFPROBE: mover override: %s"),
				       M ? *M->GetName() : TEXT("null"));
			}
			TM.SetTimer(MutateTimer, this, &UWoTEOFProbeSubsystem::MoveMover, 1.0f);
			break;
		}
		if (!MoverActor)
		{
			UE_LOG(LogTemp, Display, TEXT("EOFPROBE: mover NONE on this map"));
		}
	}

	// Clone experiments: the frame is BINARY — sky content vs pure black. The
	// placed AWoTSkyBox is hidden (it is proven black in this capture anyway;
	// hiding removes any doubt about which actor painted the sky). Each
	// variant isolates ONE property of the placed box's life cycle on a
	// fresh-spawned plain StaticMeshActor carrying the same mesh:
	//   clone_plain : spawn at camera, SkyScale       -> positive control
	//   clone_scale : spawn at camera at SCALE 1, then rescale to SkyScale at
	//                 t=1s (the box's birth transition: saved scale 1 -> tick)
	//   clone_origin: spawn at the MAP ORIGIN (the box's saved spot, inside
	//                 world geometry) at SkyScale, teleport to camera at t=1s
	//   clone_lod   : spawn at camera, SkyScale, ForcedLodModel(1) at t=1s
	//                 (the one extra property the box ctor pins)
	//   clone_tick  : spawn at camera, SkyScale, then re-set the identical
	//                 transform EVERY FRAME from t=1s (the box's steady state)
	if (!CloneMode.IsEmpty())
	{
		for (TActorIterator<AWoTSkyBox> It(&InWorld); It; ++It)
		{
			AWoTSkyBox* Box = *It;
			UStaticMeshComponent* BoxSMC = Box->GetStaticMeshComponent();
			if (!BoxSMC || !BoxSMC->GetStaticMesh())
			{
				continue;
			}
			CloneScale = Box->SkyScale;
			const FVector CamLoc = GProbeRig + FVector(-200, 0, 60);
			const bool bBirthAtOrigin = (CloneMode == TEXT("clone_origin"));
			const double BirthScale =
				(CloneMode == TEXT("clone_scale")) ? 1.0 : CloneScale;
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			// clone_wotbox: a RUNTIME-spawned AWoTSkyBox — the class's own Tick
			// camera-locks it. If this renders while the LEVEL-SAVED box stays
			// black, the class is innocent and the saved actor's serialized
			// state is the culprit.
			AStaticMeshActor* Clone;
			if (CloneMode == TEXT("clone_wotbox"))
			{
				AWoTSkyBox* NewBox = InWorld.SpawnActor<AWoTSkyBox>(
					AWoTSkyBox::StaticClass(),
					FTransform(Box->SkyRotation, CamLoc, FVector(CloneScale)), P);
				if (NewBox)
				{
					NewBox->SkyRotation = Box->SkyRotation;
					NewBox->SkyScale = Box->SkyScale;
				}
				Clone = NewBox;
			}
			else
			{
				Clone = InWorld.SpawnActor<AStaticMeshActor>(
					AStaticMeshActor::StaticClass(),
					FTransform(Box->SkyRotation,
					           bBirthAtOrigin ? FVector::ZeroVector : CamLoc,
					           FVector(BirthScale)),
					P);
			}
			if (Clone)
			{
				UStaticMeshComponent* SMC = Clone->GetStaticMeshComponent();
				SMC->SetMobility(EComponentMobility::Movable);
				SMC->SetStaticMesh(BoxSMC->GetStaticMesh());
				SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				SkyCloneActor = Clone;
				if (CloneMode != TEXT("clone_wotbox"))
				{
					TM.SetTimer(MutateTimer, this,
					            &UWoTEOFProbeSubsystem::MutateClone, 1.0f);
				}
			}
			Box->SetActorHiddenInGame(true);
			Box->SetActorTickEnabled(false);
			break;
		}
	}
	TM.SetTimer(Shot1Timer, this, &UWoTEOFProbeSubsystem::Shot1, 4.0f);
	TM.SetTimer(Shot2Timer, this, &UWoTEOFProbeSubsystem::Shot2, 6.0f);
	TM.SetTimer(QuitTimer, this, &UWoTEOFProbeSubsystem::Quit, 8.0f);
}

// The two one-shot post-registration mutations (t=1s): a location set and a
// material swap.
void UWoTEOFProbeSubsystem::Mutate()
{
	if (OnceShape)
	{
		OnceShape->SetActorLocation(OnceShape->Home + FVector(0, 0, 150));
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: ONCE sphere moved"));
	}
	if (MatShape)
	{
		UMaterialInterface* Grid = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
		if (Grid)
		{
			MatShape->Mesh->SetMaterial(0, Grid);
			UE_LOG(LogTemp, Display, TEXT("EOFPROBE: MAT cone rematerialed"));
		}
	}
	if (JumpShape)
	{
		JumpShape->SetActorLocation(GProbeRig + FVector(600, 625, 60));
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: JUMP cube teleported into frame"));
	}
	if (SkyCloneActor)
	{
		SkyCloneActor->SetActorLocation(GProbeRig + FVector(-200, 0, 60));
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: SKYCLONE teleported onto camera"));
	}
}

void UWoTEOFProbeSubsystem::FixBox()
{
	// Kill precomputed-visibility culling for the rest of the run: a
	// level-SAVED component can carry a baked VisibilityId for its save-time
	// location, which runtime spawns never have.
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->ConsoleCommand(TEXT("r.AllowPrecomputedVisibility 0"), true);
	}
	for (TActorIterator<AWoTSkyBox> It(GetWorld()); It; ++It)
	{
		UStaticMeshComponent* SMC = It->GetStaticMeshComponent();
		if (SMC)
		{
			UE_LOG(LogTemp, Display,
			       TEXT("EOFPROBE: boxfix %s (compiling %d proxy %d visid %d)"),
			       bReregister ? TEXT("REREGISTERING component")
			                   : TEXT("marking render state dirty"),
			       SMC->GetStaticMesh() && SMC->GetStaticMesh()->IsCompiling() ? 1 : 0,
			       SMC->SceneProxy ? 1 : 0, SMC->VisibilityId);
			if (bReregister)
			{
				// Full unregister+register: resets the component's
				// end-of-frame-update bookkeeping — the runtime-spawn path
				// that provably works.
				SMC->ReregisterComponent();
			}
			else
			{
				SMC->MarkRenderStateDirty();
			}
		}
	}
}

void UWoTEOFProbeSubsystem::MoveMover()
{
	if (MoverActor)
	{
		const FVector Before = MoverActor->GetActorLocation();
		// Into the rig camera's view, dead ahead — line of sight guaranteed
		// (nothing else exists at the rig altitude except the ticking sky box).
		MoverActor->SetActorLocation(GProbeRig + FVector(600, 0, 60));
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: mover moved %s -> %s"),
		       *Before.ToCompactString(),
		       *MoverActor->GetActorLocation().ToCompactString());
	}
}

void UWoTEOFProbeSubsystem::MutateClone()
{
	if (!SkyCloneActor)
	{
		return;
	}
	const FVector CamLoc = GProbeRig + FVector(-200, 0, 60);
	const FRotator Rot = SkyCloneActor->GetActorRotation();
	if (CloneMode == TEXT("clone_scale"))
	{
		SkyCloneActor->SetActorTransform(FTransform(Rot, CamLoc, FVector(CloneScale)));
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: clone rescaled 1 -> %.1f"), CloneScale);
	}
	else if (CloneMode == TEXT("clone_origin"))
	{
		SkyCloneActor->SetActorLocation(CamLoc);
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: clone teleported origin -> camera"));
	}
	else if (CloneMode == TEXT("clone_lod"))
	{
		if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(SkyCloneActor.Get()))
		{
			SMA->GetStaticMeshComponent()->SetForcedLodModel(1);
		}
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: clone ForcedLodModel(1)"));
	}
	else if (CloneMode == TEXT("clone_tick"))
	{
		SkyCloneActor->SetActorTransform(FTransform(Rot, CamLoc, FVector(CloneScale)));
		// re-fire every frame from here on
		GetWorld()->GetTimerManager().SetTimerForNextTick(
			this, &UWoTEOFProbeSubsystem::MutateClone);
	}
}

// Sky mode: a single large camera move between the two shots, mimicking the
// shot harness's pose stepping (UWoTShotSubsystem teleports its camera per
// pose the same way).
void UWoTEOFProbeSubsystem::TeleportCamera()
{
	if (Camera)
	{
		Camera->SetActorLocation(Camera->GetActorLocation() + FVector(2000, 0, 0));
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: camera teleported to %s"),
		       *Camera->GetActorLocation().ToCompactString());
	}
}

void UWoTEOFProbeSubsystem::LogSkyBox(const TCHAR* Tag)
{
	for (TActorIterator<AWoTSkyBox> It(GetWorld()); It; ++It)
	{
		AWoTSkyBox* Box = *It;
		const UStaticMeshComponent* SMC = Box->GetStaticMeshComponent();
		UE_LOG(LogTemp, Display,
		       TEXT("EOFPROBE: %s skybox %s at %s scale %.1f mesh %s hidden %d tick %d"),
		       Tag, *Box->GetName(), *Box->GetActorLocation().ToCompactString(),
		       Box->SkyScale,
		       SMC && SMC->GetStaticMesh() ? *SMC->GetStaticMesh()->GetName() : TEXT("none"),
		       Box->IsHidden() ? 1 : 0, Box->IsActorTickEnabled() ? 1 : 0);
		if (SMC)
		{
			UE_LOG(LogTemp, Display,
			       TEXT("EOFPROBE: %s skybox comp: visible %d hiddeningame %d mobility %d "
			            "maxdraw %.1f cachedmaxdraw %.1f forcedlod %d rendermain %d "
			            "registered %d renderstatecreated %d proxy %d bounds %s r %.1f"),
			       Tag, SMC->GetVisibleFlag() ? 1 : 0, SMC->bHiddenInGame ? 1 : 0,
			       int(SMC->Mobility.GetValue()), SMC->LDMaxDrawDistance,
			       SMC->CachedMaxDrawDistance, SMC->ForcedLodModel,
			       SMC->bRenderInMainPass ? 1 : 0,
			       SMC->IsRegistered() ? 1 : 0,
			       SMC->IsRenderStateCreated() ? 1 : 0,
			       SMC->SceneProxy ? 1 : 0,
			       *SMC->Bounds.Origin.ToCompactString(), SMC->Bounds.SphereRadius);
			UE_LOG(LogTemp, Display, TEXT("EOFPROBE: %s skybox comp visibilityid %d"),
			       Tag, SMC->VisibilityId);
		}
	}
}

void UWoTEOFProbeSubsystem::Shot1()
{
	LogSkyBox(TEXT("shot1"));
	auto Dump = [](const TCHAR* Name, AWoTEOFProbeShape* C) {
		if (C)
		{
			UE_LOG(LogTemp, Display, TEXT("EOFPROBE: %s at %s scale %s"), Name,
			       *C->GetActorLocation().ToCompactString(),
			       *C->GetActorScale3D().ToCompactString());
		}
	};
	Dump(TEXT("CONTROL"), ControlShape);
	Dump(TEXT("ONCE"), OnceShape);
	Dump(TEXT("TICK"), TickShape);
	Dump(TEXT("MAT"), MatShape);
	Dump(TEXT("JUMP"), JumpShape);
	if (SkyCloneActor)
	{
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: SKYCLONE at %s scale %s"),
		       *SkyCloneActor->GetActorLocation().ToCompactString(),
		       *SkyCloneActor->GetActorScale3D().ToCompactString());
	}
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (Camera)
		{
			PC->SetViewTarget(Camera); // reassert in case anything stole it
		}
		FVector VLoc; FRotator VRot;
		PC->GetPlayerViewPoint(VLoc, VRot);
		UE_LOG(LogTemp, Display, TEXT("EOFPROBE: view from %s rot %s"),
		       *VLoc.ToCompactString(), *VRot.ToCompactString());
		if (!CloneMode.IsEmpty() || bSkyOnly)
		{
			// Render-scene introspection: logs every primitive the RENDER
			// THREAD knows, with ITS transform/bounds — exposes a stale or
			// missing render-side state that game-thread queries can't see.
			PC->ConsoleCommand(TEXT("DumpPrimitives"), true);
		}
	}
	FScreenshotRequest::RequestScreenshot(OutDir / TEXT("shot1.png"), false, false);
	UE_LOG(LogTemp, Display, TEXT("EOFPROBE: shot1 requested"));
}

void UWoTEOFProbeSubsystem::Shot2()
{
	LogSkyBox(TEXT("shot2"));
	FScreenshotRequest::RequestScreenshot(OutDir / TEXT("shot2.png"), false, false);
	UE_LOG(LogTemp, Display, TEXT("EOFPROBE: shot2 requested"));
}

void UWoTEOFProbeSubsystem::Quit()
{
	UE_LOG(LogTemp, Display, TEXT("EOFPROBE: done, exiting"));
	FPlatformMisc::RequestExit(false);
}
