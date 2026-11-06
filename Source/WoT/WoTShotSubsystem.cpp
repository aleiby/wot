#include "WoTShotSubsystem.h"
#include "WoTRandom.h"

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "AI/WoTBrainComponent.h"
#include "AI/WoTCreatureData.h"
#include "AI/WoTNPCCharacter.h"
#include "Dom/JsonObject.h"
#include "Engine/DamageEvents.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/UObjectGlobals.h"
#include "Projectiles/WoTExpWardProjectile.h"
#include "Projectiles/WoTFireworksProjectile.h"
#include "Projectiles/WoTWallOfAirProjectile.h"
#include "Traps/WoTAlarm.h"
#include "Traps/WoTPortcullis.h"
#include "Traps/WoTStaircase.h"
#include "Traps/WoTTrap.h"
#include "WoTAirBlastFX.h"
#include "WoTBeamEffect.h"
#include "WoTBloodFX.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTCarcass.h"
#include "WoTCreatureChunk.h"
#include "WoTDecal.h"
#include "WoTFragment.h"
#include "Inventory/WoTInventory.h"
#include "WoTDecoration.h"
#include "WoTVertexAnimComponent.h"
#include "WoTLeafMap.h"
#include "WoTParticleSprayer.h"
#include "WoTFrontEnd.h"
#include "WoTPayloadSubsystem.h"
#include "WoTSessionSubsystem.h"
#include "WoTSkyBox.h"
#include "WoTWeaveFX.h"
#include "WoTZoneVolume.h"
#include "World/WoTRuntimeActorVisuals.h"
#include "World/WoTRuntimeBSPRenderer.h"
#include "World/WoTWorldMaterializer.h"

namespace
{
// UE1 rotator units (65536 per revolution) → degrees.
constexpr double UE1RotToDeg = 360.0 / 65536.0;
}

void UWoTShotSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!FParse::Value(FCommandLine::Get(), TEXT("wotshot="), OutDir))
	{
		return;
	}
	FParse::Value(FCommandLine::Get(), TEXT("wotmanifest="), ManifestDir);
	FParse::Value(FCommandLine::Get(), TEXT("wotgallery="), GalleryPath);
	FParse::Value(FCommandLine::Get(), TEXT("wotgallerymap="), HostedGalleryMap);
	if (!HostedGalleryMap.IsEmpty())
	{
		const TOptional<FString> ShellIdentity =
			WoTMapIdentityForGuardedShellPackage(InWorld.GetOutermost()->GetName());
		const FString CurrentIdentity = ShellIdentity.IsSet()
			? ShellIdentity.GetValue()
			: InWorld.GetMapName().Replace(TEXT("UEDPIE_0_"), TEXT(""));
		if (!CurrentIdentity.Equals(HostedGalleryMap, ESearchCase::IgnoreCase))
		{
			if (GalleryPath.IsEmpty() && ManifestDir.IsEmpty())
			{
				UE_LOG(LogTemp, Error,
					TEXT("WOTSHOT: hosted map request has no gallery or manifest: %s"),
					*HostedGalleryMap);
				FPlatformMisc::RequestExit(false);
				return;
			}
			UE_LOG(LogTemp, Display,
				TEXT("WOTGALLERY: waiting to host %s from %s"),
				*HostedGalleryMap, *CurrentIdentity);
			InWorld.GetTimerManager().SetTimer(
				Timer, this, &UWoTShotSubsystem::BeginHostedGallery, 0.5f);
			return;
		}
	}

	FString ClipsPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("wotclips="), ClipsPath))
	{
		// Feel clips: animation runs LIVE (no corona snap / waveform freeze).
		// Evidence isolation: -wotclipfractalfreeze pins the fractal textures
		// and -wotcliplightwavefreeze pins the animated light maps, so a clip
		// can show one animation source alone.
		if (IConsoleVariable* Freeze =
				IConsoleManager::Get().FindConsoleVariable(TEXT("wot.FractalFreeze")))
		{
			Freeze->Set(FParse::Param(FCommandLine::Get(),
				TEXT("wotclipfractalfreeze")) ? 1 : 0);
		}
		if (IConsoleVariable* Freeze =
				IConsoleManager::Get().FindConsoleVariable(TEXT("wot.LightWaveFreeze")))
		{
			Freeze->Set(FParse::Param(FCommandLine::Get(),
				TEXT("wotcliplightwavefreeze")) ? 1 : 0);
		}
		if (!LoadClips(ClipsPath))
		{
			UE_LOG(LogTemp, Error, TEXT("WOTSHOT: clips load failed: %s"), *ClipsPath);
			FPlatformMisc::RequestExit(false);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("WOTSHOT: clip mode, %d clips -> %s"),
		       Clips.Num(), *OutDir);
		InWorld.GetTimerManager().SetTimer(
			Timer, this, &UWoTShotSubsystem::StepClip, 3.0f);
		return;
	}

	// Golden poses are steady-state views: skip the corona fade-in and pin
	// the animated-lightmap waveform at its mean (the baked static value).
	if (IConsoleVariable* Snap =
			IConsoleManager::Get().FindConsoleVariable(TEXT("wot.CoronaSnap")))
	{
		Snap->Set(1);
	}
	if (IConsoleVariable* Freeze =
			IConsoleManager::Get().FindConsoleVariable(TEXT("wot.LightWaveFreeze")))
	{
		Freeze->Set(1);
	}
	if (IConsoleVariable* Freeze =
			IConsoleManager::Get().FindConsoleVariable(TEXT("wot.FractalFreeze")))
	{
		Freeze->Set(1);
	}

	FString MapName = InWorld.GetMapName().Replace(TEXT("UEDPIE_0_"), TEXT(""));
	if (const TOptional<FString> GuardedIdentity =
		WoTMapIdentityForGuardedShellPackage(InWorld.GetOutermost()->GetName());
		GuardedIdentity.IsSet())
	{
		MapName = GuardedIdentity.GetValue();
	}
	if (!GalleryPath.IsEmpty())
	{
		if (!LoadGallery(GalleryPath, MapName))
		{
			UE_LOG(LogTemp, Error, TEXT("WOTGALLERY: config load failed: %s"), *GalleryPath);
			FPlatformMisc::RequestExit(false);
			return;
		}
		IFileManager::Get().MakeDirectory(*OutDir, true);
		EnsureCamera();
		SetViewerPoint(GalleryAnchor);
		UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %d entries anchor=%s -> %s"),
		       GalleryEntries.Num(), *GalleryAnchor.ToCompactString(), *OutDir);
		InWorld.GetTimerManager().SetTimer(
			Timer, this, &UWoTShotSubsystem::BeginGalleryEntry, 3.0f);
		return;
	}
	if (!ManifestDir.IsEmpty())
	{
		if (!LoadManifest(MapName))
		{
			UE_LOG(LogTemp, Error, TEXT("WOTSHOT: manifest load failed for %s"), *MapName);
			FPlatformMisc::RequestExit(false);
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("WOTSHOT: manifest mode, %d poses -> %s"), Poses.Num(), *OutDir);
	}
	else
	{
		for (TActorIterator<APlayerStart> It(&InWorld); It; ++It)
		{
			Starts.Add(*It);
		}
		Starts.Sort([](const TWeakObjectPtr<AActor>& A, const TWeakObjectPtr<AActor>& B)
		{
			return A->GetName() < B->GetName();
		});
		UE_LOG(LogTemp, Display, TEXT("WOTSHOT: %d starts -> %s"), Starts.Num(), *OutDir);
	}
	// Warm-up: let streaming/shaders settle before the first shot.
	InWorld.GetTimerManager().SetTimer(
		Timer, this,
		ManifestDir.IsEmpty() ? &UWoTShotSubsystem::Step : &UWoTShotSubsystem::StepManifest, 3.0f);
}

void UWoTShotSubsystem::BeginHostedGallery()
{
	if (bHostedGalleryRequestAccepted) return;
	UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UWoTSessionSubsystem* Sessions = GameInstance
		? GameInstance->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (Sessions != nullptr
		&& FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance))
	{
		if (!WoTGuardedShellPackageForMapIdentity(HostedGalleryMap).IsSet())
		{
			UE_LOG(LogTemp, Error,
				TEXT("WOTGALLERY: hosted map request has no adopted shell: %s"),
				*HostedGalleryMap);
			FPlatformMisc::RequestExit(false);
			return;
		}
		FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(
			TEXT("/Game/WoT/Maps/") + HostedGalleryMap);
		Request.bLAN = true;
		bHostedGalleryRequestAccepted = Sessions->Host(Request);
		if (bHostedGalleryRequestAccepted)
		{
			UE_LOG(LogTemp, Display,
				TEXT("WOTGALLERY: hosted map request accepted: %s"),
				*HostedGalleryMap);
			return;
		}
	}
	if (++HostedGalleryAttempts >= 240)
	{
		UE_LOG(LogTemp, Error,
			TEXT("WOTGALLERY: timed out waiting to host %s"),
			*HostedGalleryMap);
		FPlatformMisc::RequestExit(false);
		return;
	}
	GetWorld()->GetTimerManager().SetTimer(
		Timer, this, &UWoTShotSubsystem::BeginHostedGallery, 0.5f);
}

bool UWoTShotSubsystem::LoadManifest(const FString& MapName)
{
	const FString Path = ManifestDir / MapName / TEXT("manifest.json");
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		UE_LOG(LogTemp, Error, TEXT("WOTSHOT: cannot read %s"), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root) || !Root.IsValid())
	{
		return false;
	}
	Root->TryGetNumberField(TEXT("eye_height"), EyeHeight);
	const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
	if (!Root->TryGetArrayField(TEXT("shots"), Shots))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Shots)
	{
		const TSharedPtr<FJsonObject> S = V->AsObject();
		if (!S.IsValid())
		{
			continue;
		}
		FGoldenPose P;
		P.File = S->GetStringField(TEXT("file"));
		const TSharedPtr<FJsonObject> Pawn = S->GetObjectField(TEXT("pawn"));
		// T3D/UE1 coordinates land 1:1 in UE5 world space (see PORT_NOTES:
		// the GLB conversion round-trips to identity for world positions).
		P.Pawn = FVector(Pawn->GetNumberField(TEXT("x")),
		                 Pawn->GetNumberField(TEXT("y")),
		                 Pawn->GetNumberField(TEXT("z")));
		const TSharedPtr<FJsonObject> Rot = S->GetObjectField(TEXT("rot"));
		P.Pitch = Rot->GetNumberField(TEXT("pitch"));
		P.Yaw = Rot->GetNumberField(TEXT("yaw"));
		P.Roll = Rot->GetNumberField(TEXT("roll"));
		Poses.Add(MoveTemp(P));
	}
	return !Poses.IsEmpty();
}

// The corona leaf gate resolves the viewer PAWN's BSP leaf (retail:
// Viewport->Actor->Region.iLeaf). This harness's view target is a camera
// actor flying at pawn+eye_height, so pin the gate to the manifest pawn
// point for each pose.
void UWoTShotSubsystem::SetViewerPoint(const FVector& PawnPoint)
{
	if (!bLeafMapSearched)
	{
		bLeafMapSearched = true;
		if (TActorIterator<AWoTLeafMap> It(GetWorld()); It)
		{
			LeafMap = *It;
		}
	}
	if (LeafMap.IsValid())
	{
		LeafMap->SetViewerOverride(PawnPoint);
	}
}

ACameraActor* UWoTShotSubsystem::EnsureCamera()
{
	UWorld* World = GetWorld();
	if (!Camera.IsValid())
	{
		Camera = World->SpawnActor<ACameraActor>();
		UCameraComponent* Cam = Camera->GetCameraComponent();
		// Constrained aspect treats FieldOfView as TRUE horizontal FOV.
		// With bConstrainAspectRatio=false at 4:3, UE 5.8 derives the
		// projection from the 16:9-implied vertical FOV — 73.74° horizontal,
		// a uniform 4/3 magnification vs the UE1 goldens (FOV 90 horizontal
		// at any aspect). That zoom masqueraded as a per-map brightness
		// anomaly in the mean-luma baselines.
		Cam->SetConstraintAspectRatio(true);
		Cam->SetAspectRatio(1600.0f / 1200.0f);
		Cam->SetFieldOfView(90.0f);
		// Period look: no film tone curve (r.Tonemapper.Quality=0 does NOT
		// remove the S-curve in 5.8 — measured via emissive swatches).
		// Output becomes plain sRGB encode of scene linear.
		Cam->PostProcessSettings.bOverride_ToneCurveAmount = true;
		Cam->PostProcessSettings.ToneCurveAmount = 0.0f;
		Cam->PostProcessSettings.bOverride_ExpandGamut = true;
		Cam->PostProcessSettings.ExpandGamut = 0.0f;
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			PC->SetViewTarget(Camera.Get());
			// The capture pawn spawns at a PlayerStart and stands there for the
			// whole run; the pose points sit AT PlayerStarts, so its arm/hand
			// mesh photobombs whichever pose shares its spawn start (the §0-P14
			// "black ambient-creature silhouette"). Hide it outright — the
			// goldens' retail rig WAS the pawn, so no capture may contain it.
			if (APawn* CapturePawn = PC->GetPawn())
			{
				CapturePawn->SetActorHiddenInGame(true);
			}
			// Golden captures used the console `ToggleUI` (no HUD/crosshair).
			// Match it: an on-screen HUD is a top-left artifact that inflates
			// every pose's bad% against the HUD-less goldens.
			if (AHUD* HUD = PC->GetHUD())
			{
				HUD->bShowHUD = false;
			}
		}
	}
	return Camera.Get();
}

bool UWoTShotSubsystem::LoadClips(const FString& Path)
{
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root) || !Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Root->TryGetArrayField(TEXT("clips"), Arr))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Arr)
	{
		const TSharedPtr<FJsonObject> C = V->AsObject();
		if (!C.IsValid())
		{
			continue;
		}
		FFeelClip Clip;
		Clip.Name = C->GetStringField(TEXT("name"));
		const TArray<TSharedPtr<FJsonValue>>* Pawn = nullptr;
		if (C->TryGetArrayField(TEXT("pawn"), Pawn) && Pawn->Num() == 3)
		{
			Clip.Pawn = FVector((*Pawn)[0]->AsNumber(), (*Pawn)[1]->AsNumber(),
			                    (*Pawn)[2]->AsNumber());
		}
		Clip.Yaw = C->GetNumberField(TEXT("yaw"));
		Clip.Pitch = C->GetNumberField(TEXT("pitch"));
		C->TryGetNumberField(TEXT("seconds"), Clip.Seconds);
		Clips.Add(MoveTemp(Clip));
	}
	return !Clips.IsEmpty();
}

bool UWoTShotSubsystem::LoadGallery(const FString& Path, const FString& MapName)
{
	GalleryMapName = MapName;
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Root) || !Root.IsValid())
	{
		return false;
	}

	if (Root->HasTypedField<EJson::Object>(TEXT("orbit")))
	{
		const TSharedPtr<FJsonObject> Orbit = Root->GetObjectField(TEXT("orbit"));
		Orbit->TryGetNumberField(TEXT("radius"), GalleryRadius);
		Orbit->TryGetNumberField(TEXT("height"), GalleryHeight);
		double Yaws = GalleryYawCount;
		if (Orbit->TryGetNumberField(TEXT("yaws"), Yaws))
		{
			GalleryYawCount = FMath::Max(1, FMath::RoundToInt(Yaws));
		}
	}
	Root->TryGetNumberField(TEXT("settle_seconds"), GallerySettleSeconds);
	GalleryRadius = FMath::Max(1.0, GalleryRadius);
	GallerySettleSeconds = FMath::Max(0.0, GallerySettleSeconds);

	bool bHasAnchorOverride = false;
	if (Root->HasTypedField<EJson::Object>(TEXT("anchors")))
	{
		const TSharedPtr<FJsonObject> Anchors = Root->GetObjectField(TEXT("anchors"));
		// 5.8 FJsonObject keys are UE::FSharedString — go through the field
		// API rather than Values.Find(FString).
		if (const TSharedPtr<FJsonValue> Value = Anchors->TryGetField(MapName))
		{
			if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& XYZ = Value->AsArray();
				if (XYZ.Num() != 3)
				{
					return false;
				}
				GalleryAnchor = FVector(XYZ[0]->AsNumber(), XYZ[1]->AsNumber(), XYZ[2]->AsNumber());
				bHasAnchorOverride = true;
			}
			else if (Value->Type != EJson::Null)
			{
				return false;
			}
		}
	}
	if (!bHasAnchorOverride)
	{
		TArray<TWeakObjectPtr<AActor>> SortedStarts;
		for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
		{
			SortedStarts.Add(*It);
		}
		// Sort by LOCATION, not name: object names are auto-generated at
		// import and churn on every map re-import, which silently moved the
		// anchor to a different start and invalidated the whole gallery A/B
		// baseline (caught on the first post-reimport run). Positions come
		// from the T3D and are import-stable.
		SortedStarts.Sort([](const TWeakObjectPtr<AActor>& A, const TWeakObjectPtr<AActor>& B)
		{
			const FVector LA = A->GetActorLocation();
			const FVector LB = B->GetActorLocation();
			if (LA.X != LB.X)
			{
				return LA.X < LB.X;
			}
			if (LA.Y != LB.Y)
			{
				return LA.Y < LB.Y;
			}
			return LA.Z < LB.Z;
		});
		if (SortedStarts.IsEmpty() || !SortedStarts[0].IsValid())
		{
			return false;
		}
		GalleryAnchor = SortedStarts[0]->GetActorLocation();
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Root->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid())
		{
			return false;
		}
		FGalleryEntry Entry;
		if (!Object->TryGetStringField(TEXT("name"), Entry.Name) || Entry.Name.IsEmpty())
		{
			return false;
		}
		Object->TryGetStringField(TEXT("class"), Entry.ClassPath);
		Object->TryGetStringField(TEXT("kind"), Entry.Kind);
		Object->TryGetStringField(TEXT("skip"), Entry.Skip);
		Object->TryGetStringField(TEXT("config"), Entry.Config);
		Object->TryGetStringField(TEXT("dev_mesh"), Entry.DevMesh);
		Object->TryGetStringField(TEXT("dev_skin"), Entry.DevSkin);
		Object->TryGetNumberField(TEXT("dev_scale"), Entry.DevScale);
		double AmbientGlow = 0.0;
		if (Object->TryGetNumberField(TEXT("ambient_glow"), AmbientGlow))
		{
			if (!FMath::IsFinite(AmbientGlow) || AmbientGlow < 0.0
				|| AmbientGlow > 255.0
				|| AmbientGlow != FMath::RoundToDouble(AmbientGlow))
			{
				return false;
			}
			Entry.AmbientGlow = FMath::RoundToInt(AmbientGlow);
		}
		FString Creature;
		if (Object->TryGetStringField(TEXT("creature"), Creature))
		{
			Entry.CreatureRow = FName(*Creature);
		}
		Object->TryGetNumberField(TEXT("settle_seconds"), Entry.SettleSeconds);
		Object->TryGetBoolField(TEXT("existing"), Entry.bExisting);
		Object->TryGetBoolField(TEXT("expect_no_mesh"), Entry.bExpectNoMesh);
		const TArray<TSharedPtr<FJsonValue>>* Anchor = nullptr;
		if (Object->TryGetArrayField(TEXT("anchor"), Anchor))
		{
			if (!Anchor || Anchor->Num() != 3)
			{
				return false;
			}
			Entry.Anchor = FVector((*Anchor)[0]->AsNumber(), (*Anchor)[1]->AsNumber(),
				(*Anchor)[2]->AsNumber());
			Entry.bHasAnchor = true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Offset = nullptr;
		if (Object->TryGetArrayField(TEXT("offset"), Offset))
		{
			if (!Offset || Offset->Num() != 3)
			{
				return false;
			}
			Entry.Offset = FVector((*Offset)[0]->AsNumber(), (*Offset)[1]->AsNumber(),
			                       (*Offset)[2]->AsNumber());
		}
		Object->TryGetStringField(TEXT("map"), Entry.Map);
		const TArray<TSharedPtr<FJsonValue>>* PoseCamera = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* PoseRotation = nullptr;
		const bool bHasPoseCamera = Object->TryGetArrayField(TEXT("camera"), PoseCamera);
		const bool bHasPoseRotation = Object->TryGetArrayField(TEXT("rotation"), PoseRotation);
		if (Entry.Kind.Equals(TEXT("pose"), ESearchCase::IgnoreCase))
		{
			// A pose is a fixed golden-style view: both fields are mandatory so a
			// malformed ledger fails the load instead of shooting the anchor.
			if (!bHasPoseCamera || !bHasPoseRotation || !PoseCamera || !PoseRotation
				|| PoseCamera->Num() != 3 || PoseRotation->Num() != 3)
			{
				return false;
			}
			Entry.PoseCamera = FVector((*PoseCamera)[0]->AsNumber(),
				(*PoseCamera)[1]->AsNumber(), (*PoseCamera)[2]->AsNumber());
			Entry.PoseRotation = FVector((*PoseRotation)[0]->AsNumber(),
				(*PoseRotation)[1]->AsNumber(), (*PoseRotation)[2]->AsNumber());
			if (Entry.PoseCamera.ContainsNaN() || Entry.PoseRotation.ContainsNaN())
			{
				return false;
			}
			Entry.bHasPose = true;
		}
		else if (bHasPoseCamera || bHasPoseRotation)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
		if (Object->TryGetArrayField(TEXT("steps"), StepValues))
		{
			for (const TSharedPtr<FJsonValue>& StepValue : *StepValues)
			{
				const TSharedPtr<FJsonObject> StepObject = StepValue->AsObject();
				if (!StepObject.IsValid())
				{
					return false;
				}
				FGalleryStep Step;
				if (!StepObject->TryGetStringField(TEXT("name"), Step.Name) || Step.Name.IsEmpty())
				{
					return false;
				}
				StepObject->TryGetStringField(TEXT("action"), Step.Action);
				StepObject->TryGetNumberField(TEXT("amount"), Step.Amount);
				StepObject->TryGetNumberField(TEXT("settle_seconds"), Step.SettleSeconds);
				double StepYaws = 0.0;
				if (StepObject->TryGetNumberField(TEXT("yaws"), StepYaws))
				{
					Step.Yaws = FMath::Max(1, FMath::RoundToInt(StepYaws));
				}
				// Validate the action vocabulary at parse — a typo must fail the load, not
				// silently no-op a step ledger ten minutes into a run.
				static const TCHAR* const KnownActions[] = {
					TEXT("none"), TEXT("begin_edit"), TEXT("end_edit"), TEXT("touch"),
					TEXT("activate"), TEXT("deactivate"), TEXT("deploy"), TEXT("damage")};
				bool bKnownAction = false;
				for (const TCHAR* Known : KnownActions)
				{
					bKnownAction |= Step.Action.Equals(Known, ESearchCase::IgnoreCase);
				}
				if (!bKnownAction
					|| (Step.Action.Equals(TEXT("damage"), ESearchCase::IgnoreCase)
						&& Step.Amount <= 0.0))
				{
					return false;
				}
				Entry.Steps.Add(MoveTemp(Step));
			}
		}
		GalleryEntries.Add(MoveTemp(Entry));
	}
	return !GalleryEntries.IsEmpty();
}

bool UWoTShotSubsystem::SetupGalleryActor(AActor* Actor, const FGalleryEntry& Entry,
	bool bBeforeBeginPlay, FString& OutReason)
{
	const bool bProjectile = Entry.Kind.Equals(TEXT("projectile"), ESearchCase::IgnoreCase);
	const bool bBeam = Entry.Kind.Equals(TEXT("beam"), ESearchCase::IgnoreCase);
	const bool bPawn = Entry.Kind.Equals(TEXT("pawn"), ESearchCase::IgnoreCase);
	const bool bWard = Entry.Kind.Equals(TEXT("ward"), ESearchCase::IgnoreCase);
	const bool bEffect = Entry.Kind.Equals(TEXT("effect"), ESearchCase::IgnoreCase);
	const bool bTrap = Entry.Kind.Equals(TEXT("trap"), ESearchCase::IgnoreCase);
	const bool bPickup = Entry.Kind.Equals(TEXT("pickup"), ESearchCase::IgnoreCase);
	const bool bDecoration = Entry.Kind.Equals(
		TEXT("decoration"), ESearchCase::IgnoreCase);
	const bool bNone = Entry.Kind.IsEmpty() ||
		Entry.Kind.Equals(TEXT("none"), ESearchCase::IgnoreCase);
	if (!bProjectile && !bBeam && !bPawn && !bWard && !bEffect && !bTrap
		&& !bPickup && !bDecoration && !bNone)
	{
		OutReason = FString::Printf(TEXT("unknown kind '%s'"), *Entry.Kind);
		return false;
	}
	if (!Entry.Steps.IsEmpty() && !bTrap)
	{
		OutReason = TEXT("steps require kind 'trap'");
		return false;
	}

	if (bBeforeBeginPlay)
	{
		auto ApplyDevVisualFallback = [&Entry](UWoTVertexAnimComponent* Display)
		{
#if WOT_GUARDED_BOOTSTRAP
			(void)Display;
			return false;
#else
			if (Display == nullptr || Entry.DevMesh.IsEmpty()
				|| !Display->SetVertexMesh(FName(*Entry.DevMesh))) return false;
			Display->SetRelativeScale3D(FVector(Entry.DevScale));
			if (!Entry.DevSkin.IsEmpty()
				&& !Display->SetAllSkinOverrides(FSoftObjectPath(Entry.DevSkin)))
				return false;
			return true;
#endif
		};
		if (bPickup)
		{
			AWoTInventory* Pickup = Cast<AWoTInventory>(Actor);
			if (!Pickup)
			{
				OutReason = TEXT("pickup kind requires AWoTInventory");
				return false;
			}
			if (Entry.bExpectNoMesh)
			{
				FWoTRuntimeClassDefaults* Defaults =
					UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(OutReason);
				FWoTRuntimeActorVisualPresentation Presentation;
				if (Entry.Config.IsEmpty() || Defaults == nullptr
					|| !WoTRuntimeReadClassVisualPresentation(
						*Defaults, Entry.Config, Presentation, OutReason))
				{
					return false;
				}
				if (Presentation.HasMesh())
				{
					OutReason = TEXT("retail class unexpectedly has a display mesh");
					return false;
				}
			}
			else if (!Entry.Config.IsEmpty()
				&& !Pickup->ConfigureRuntimeClassDefaultVisuals(Entry.Config, OutReason)
				&& !ApplyDevVisualFallback(Pickup->Mesh))
			{
				return false;
			}
			if (Entry.AmbientGlow != INDEX_NONE)
			{
				Pickup->AmbientGlow = static_cast<float>(Entry.AmbientGlow);
			}
			OutReason.Reset();
		}
		if (bDecoration)
		{
			AWoTDecoration* Decoration = Cast<AWoTDecoration>(Actor);
			if (!Decoration || Entry.Config.IsEmpty())
			{
				OutReason = TEXT("decoration kind requires AWoTDecoration and a retail class config");
				return false;
			}
			if (!Decoration->ConfigureRuntimeClassDefaultVisuals(
				Entry.Config, OutReason)
				&& !ApplyDevVisualFallback(Decoration->Mesh)) return false;
			OutReason.Reset();
		}
		if (bTrap && !Cast<AWoTTrap>(Actor))
		{
			OutReason = TEXT("trap kind requires AWoTTrap");
			return false;
		}
		if (bBeam)
		{
			AWoTBeamEffect* Beam = Cast<AWoTBeamEffect>(Actor);
			if (!Beam)
			{
				OutReason = TEXT("beam kind requires AWoTBeamEffect");
				return false;
			}
			Beam->Config = Entry.Config.IsEmpty() ? FName(TEXT("LightningStreak02"))
			                                           : FName(*Entry.Config);
			Beam->Start = ActiveGalleryAnchor + FVector(-200.0, 0.0, 60.0);
			Beam->End = ActiveGalleryAnchor + FVector(200.0, 0.0, 60.0);
		}
		if (bPawn)
		{
			if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Actor))
			{
				if (Entry.CreatureRow.IsNone())
				{
					OutReason = TEXT("NPC pawn has no creature row");
					return false;
				}
				const FWoTCreatureRow* Row = WoTCreatureData::Find(Entry.CreatureRow);
				if (!Row)
				{
					OutReason = FString::Printf(TEXT("unknown creature row '%s'"),
					                            *Entry.CreatureRow.ToString());
					return false;
				}
				NPC->AutoPossessAI = EAutoPossessAI::Disabled;
				Row->ConfigureNPC(*NPC, nullptr);
			}
		}
		if (bWard)
		{
			AWoTExpWardProjectile* Ward = Cast<AWoTExpWardProjectile>(Actor);
			if (!Ward)
			{
				OutReason = TEXT("ward kind requires AWoTExpWardProjectile");
				return false;
			}
			// Keep the visible Waiting/arming presentation for the whole four-stop orbit.
			Ward->ActivationTime = 60.0f;
		}
		if (AWoTFireworksProjectile* Fireworks = Cast<AWoTFireworksProjectile>(Actor))
		{
			// Its random 2-6 second fuse would otherwise make later yaw stops disappear.
			Fireworks->MinExpTime = 60.0f;
			Fireworks->MaxExpTime = 60.0f;
		}
		if (bEffect)
		{
			const FName Config = Entry.Config.IsEmpty() ? NAME_None : FName(*Entry.Config);
			if (AWoTAirBlastFX* AirBlast = Cast<AWoTAirBlastFX>(Actor))
			{
				AirBlast->BlastDuration = 60.0f;
				AirBlast->BlastVelocity = FVector::ZeroVector;
			}
			else if (AWoTBloodBurst* Blood = Cast<AWoTBloodBurst>(Actor))
			{
				if (!WoTBloodFX::Find(Config))
				{
					OutReason = FString::Printf(TEXT("unknown blood config '%s'"), *Entry.Config);
					return false;
				}
				Blood->Config = Config;
			}
			else if (AWoTWeaveBurst* Burst = Cast<AWoTWeaveBurst>(Actor))
			{
				if (!WoTWeaveFX::Find(Config))
				{
					OutReason = FString::Printf(TEXT("unknown weave config '%s'"), *Entry.Config);
					return false;
				}
				Burst->Config = Config;
			}
			else if (AWoTExplosionEffect* Explosion = Cast<AWoTExplosionEffect>(Actor))
			{
				const FWoTExplosionRow* Row = WoTWeaveFX::FindExplosion(Config);
				if (!Row)
				{
					OutReason = FString::Printf(TEXT("unknown explosion config '%s'"), *Entry.Config);
					return false;
				}
				Explosion->Configure(*Row);
			}
			else if (AWoTDecal* Decal = Cast<AWoTDecal>(Actor))
			{
				// A Decal-family class (WoTDecal.cpp registry), first texture, class
				// DrawScale and AmbientGlow, as SpawnConfigured sets them.
				const FWoTDecalConfig* DecalConfig = WoTDecalFX::Find(Config);
				if (!DecalConfig)
				{
					OutReason = FString::Printf(TEXT("unknown decal config '%s'"), *Entry.Config);
					return false;
				}
				Decal->DecalType = Config;
				Decal->TextureIndex = 0;
				Decal->FinalScale = DecalConfig->DrawScale;
				Decal->AmbientGlow = DecalConfig->AmbientGlow;
			}
		}
		return true;
	}

	// bTrap: deliberately nothing in either phase beyond the class check — the trap
	// spawns fully live (capsule, blockers, burn/phase timers) and every later state
	// is driven through production entry points by the step ledger (BeginGalleryStep);
	// freezing components here would falsify the phase FSMs being verified.
	if (bPickup || bDecoration)
	{
		Actor->SetActorEnableCollision(false);
		const UWoTVertexAnimComponent* Display =
			Actor->FindComponentByClass<UWoTVertexAnimComponent>();
		if (!Display || (Entry.bExpectNoMesh
			? Display->HasDisplayMesh() : !Display->HasDisplayMesh()))
		{
			OutReason = Entry.bExpectNoMesh
				? TEXT("retail no-mesh class unexpectedly bound a display mesh")
				: TEXT("runtime class-default mesh did not bind");
			return false;
		}
	}
	if (bProjectile || bWard)
	{
		TArray<UProjectileMovementComponent*> Movements;
		Actor->GetComponents(Movements);
		for (UProjectileMovementComponent* Movement : Movements)
		{
			Movement->Velocity = FVector::ZeroVector;
			Movement->InitialSpeed = 0.0f;
			Movement->MaxSpeed = 0.0f;
			Movement->ProjectileGravityScale = 0.0f;
			Movement->StopMovementImmediately();
			Movement->Deactivate();
			Movement->SetComponentTickEnabled(false);
		}
		Actor->SetActorEnableCollision(false);
		Actor->SetLifeSpan(0.0f);
		// This wall's normal Tick destroys it without its caster/target pair. Its
		// mesh animation is component-driven, so freezing only the actor preserves
		// the standalone visual without synthesizing gameplay owners.
		if (Cast<AWoTWallOfAirProjectile>(Actor))
		{
			Actor->SetActorTickEnabled(false);
		}
	}
	if (bPawn)
	{
		ACharacter* Character = Cast<ACharacter>(Actor);
		if (!Character)
		{
			OutReason = TEXT("pawn kind requires ACharacter");
			return false;
		}
		Character->SetActorEnableCollision(false);
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
			Movement->DisableMovement();
		}
		if (AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Character))
		{
			if (NPC->Brain)
			{
				NPC->Brain->Deactivate();
				NPC->Brain->SetComponentTickEnabled(false);
			}
		}
		if (AController* Controller = Character->GetController())
		{
			Controller->UnPossess();
			Controller->Destroy();
		}
	}
	if (bEffect)
	{
		Actor->SetLifeSpan(0.0f);
		const FName Config = Entry.Config.IsEmpty() ? NAME_None : FName(*Entry.Config);
		if (Cast<AWoTParticleSprayer>(Actor))
		{
			if (!WoTWeaveFX::SpawnLocal(*GetWorld(), Config, Actor->GetActorLocation(),
			                              Actor->GetActorRotation()))
			{
				OutReason = FString::Printf(TEXT("unknown sprayer config '%s'"), *Entry.Config);
				return false;
			}
			Actor->SetActorTickEnabled(false);
		}
		else if (AWoTCarcass* Carcass = Cast<AWoTCarcass>(Actor))
		{
			Carcass->MeshName = Config.IsNone() ? FName(TEXT("Trolloc")) : Config;
			Carcass->MeshScale = 1.0f;
			Carcass->OnRep_Display();
			Carcass->SetActorEnableCollision(false);
			Carcass->SetActorTickEnabled(false);
		}
		else if (AWoTCreatureChunk* Chunk = Cast<AWoTCreatureChunk>(Actor))
		{
			Chunk->MeshName = Config.IsNone() ? FName(TEXT("ChunkA")) : Config;
			Chunk->MeshScale = 1.0f;
			Chunk->OnRep_Display();
			Chunk->SetActorEnableCollision(false);
			Chunk->SetActorTickEnabled(false);
		}
		else if (AWoTFragment* Fragment = Cast<AWoTFragment>(Actor))
		{
			Fragment->MeshName = Config.IsNone() ? FName(TEXT("wafr1")) : Config;
			Fragment->MeshScale = 1.0f;
			Fragment->OnRep_Display();
			Fragment->SetActorEnableCollision(false);
			Fragment->SetActorTickEnabled(false);
		}
		else if (Cast<AWoTExplosionEffect>(Actor))
		{
			Actor->SetActorTickEnabled(false);
		}
		else if (Cast<AWoTAirBlastFX>(Actor))
		{
			Actor->SetActorTickEnabled(false);
		}
		else if (AWoTDecal* Decal = Cast<AWoTDecal>(Actor))
		{
			// The production placement (Decal.uc Align) on the floor under the
			// spawn point: flush back-trace, random roll, 4-corner fit.
			FHitResult Floor;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTGalleryDecal), false, Decal);
			if (!GetWorld()->LineTraceSingleByChannel(Floor, Decal->GetActorLocation(),
				Decal->GetActorLocation() - FVector(0.0, 0.0, 1000.0), ECC_WorldStatic, Params))
			{
				OutReason = TEXT("decal found no floor under the anchor");
				return false;
			}
			Decal->SetActorLocation(Floor.Location + Floor.ImpactNormal * 4.0);
			Decal->Align(Floor.ImpactNormal);
			if (!IsValid(Decal) || !Decal->IsPlaced())
			{
				OutReason = TEXT("decal fit failed at the anchor");
				return false;
			}
		}
	}
	return true;
}

// EnsureCamera's pawn/HUD hide runs before login finishes in gallery mode
// (OnWorldBeginPlay), when PC->GetPawn()/GetHUD() are still null — so it
// silently no-ops. Re-assert per entry (idempotent): the first live run had
// the HUD in every frame, and a spawned Dart damaged and eventually KILLED
// the standing capture pawn, putting the auto-scoreboard over later
// captures. The pawn also stops blocking/absorbing spawned projectiles.
void UWoTShotSubsystem::PrepareGalleryStage()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC)
	{
		return;
	}
	if (APawn* CapturePawn = PC->GetPawn())
	{
		CapturePawn->SetActorHiddenInGame(true);
		CapturePawn->SetActorEnableCollision(false);
		CapturePawn->SetCanBeDamaged(false);
		if (ACharacter* Char = Cast<ACharacter>(CapturePawn))
		{
			if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
			{
				// No collision means no floor: freeze so it can't fall to KillZ.
				Move->DisableMovement();
			}
		}
	}
	if (AHUD* HUD = PC->GetHUD())
	{
		HUD->bShowHUD = false;
	}
}

void UWoTShotSubsystem::BeginGalleryEntry()
{
	UWorld* World = GetWorld();
	PrepareGalleryStage();
	if (GalleryIndex >= GalleryEntries.Num())
	{
		UE_LOG(LogTemp, Display,
		       TEXT("WOTGALLERY: done ok=%d skipped=%d other_map=%d entries=%d"),
		       GalleryOkCount, GallerySkippedCount, GalleryOtherMapCount,
		       GalleryEntries.Num());
		WoTRecordBootstrapHeartbeat(TEXT("gallery-complete"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("ok=%d skipped=%d other_map=%d entries=%d"),
				GalleryOkCount, GallerySkippedCount, GalleryOtherMapCount,
				GalleryEntries.Num()));
		FPlatformMisc::RequestExit(false);
		return;
	}
	const FGalleryEntry& Entry = GalleryEntries[GalleryIndex];
	ActiveGalleryAnchor = Entry.bHasAnchor ? Entry.Anchor : GalleryAnchor;
	if (!Entry.Map.IsEmpty()
		&& !Entry.Map.Equals(GalleryMapName, ESearchCase::IgnoreCase))
	{
		// Another map's ledger row: not a skip, the per-map run owns it.
		UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %s other-map %s (current %s)"),
		       *Entry.Name, *Entry.Map, *GalleryMapName);
		++GalleryOtherMapCount;
		++GalleryIndex;
		World->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::BeginGalleryEntry);
		return;
	}
	if (!Entry.Skip.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %s skipped %s"), *Entry.Name, *Entry.Skip);
		WoTRecordBootstrapHeartbeat(TEXT("gallery-skip"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("entry=%s reason=%s"), *Entry.Name, *Entry.Skip));
		++GallerySkippedCount;
		++GalleryIndex;
		World->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::BeginGalleryEntry);
		return;
	}

	GalleryActorsBefore.Reset();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		GalleryActorsBefore.Add(*It);
	}
	if (Entry.bHasPose)
	{
		// Fixed view, nothing spawned: the frame is the map itself (sky pose).
		GalleryActor.Reset();
		GalleryYawIndex = 0;
		GalleryStepIndex = 0;
		SetViewerPoint(Entry.PoseCamera);
		const double PoseSettle = Entry.SettleSeconds >= 0.0
			? Entry.SettleSeconds : GallerySettleSeconds;
		if (PoseSettle <= 0.0)
		{
			World->GetTimerManager().SetTimerForNextTick(
				this, &UWoTShotSubsystem::StepGalleryOrbit);
		}
		else
		{
			World->GetTimerManager().SetTimer(
				Timer, this, &UWoTShotSubsystem::StepGalleryOrbit,
				static_cast<float>(PoseSettle));
		}
		return;
	}
	// Beam skins, creature health rolls, particle templates, and gib presentation
	// all use the WoT gameplay stream. Re-seed per ledger name so an entry is
	// independent of how many random draws earlier entries happened to consume.
	WoTRandom::Seed(static_cast<int32>(GetTypeHash(Entry.Name)));
	UClass* ActorClass = LoadObject<UClass>(nullptr, *Entry.ClassPath);
	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()) ||
		ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %s skipped class did not resolve to a spawnable actor"),
		       *Entry.Name);
		++GallerySkippedCount;
		++GalleryIndex;
		World->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::BeginGalleryEntry);
		return;
	}
	if (Entry.bExisting)
	{
		AActor* Nearest = nullptr;
		double NearestDistanceSquared = FMath::Square(256.0);
		const FVector SearchPoint = ActiveGalleryAnchor + Entry.Offset;
		for (TActorIterator<AActor> It(World, ActorClass); It; ++It)
		{
			const double DistanceSquared = FVector::DistSquared(
				It->GetActorLocation(), SearchPoint);
			if (DistanceSquared <= NearestDistanceSquared)
			{
				Nearest = *It;
				NearestDistanceSquared = DistanceSquared;
			}
		}
		if (!Nearest)
		{
			UE_LOG(LogTemp, Display,
				TEXT("WOTGALLERY: %s skipped no existing actor within 256 units of anchor"),
				*Entry.Name);
			++GallerySkippedCount;
			++GalleryIndex;
			World->GetTimerManager().SetTimerForNextTick(
				this, &UWoTShotSubsystem::BeginGalleryEntry);
			return;
		}
		GalleryActor = Nearest;
		GalleryYawIndex = 0;
		GalleryStepIndex = 0;
		SetViewerPoint(ActiveGalleryAnchor);
		const double Settle = Entry.SettleSeconds >= 0.0
			? Entry.SettleSeconds : GallerySettleSeconds;
		if (Settle <= 0.0)
		{
			World->GetTimerManager().SetTimerForNextTick(
				this, &UWoTShotSubsystem::StepGalleryOrbit);
		}
		else
		{
			World->GetTimerManager().SetTimer(
				Timer, this, &UWoTShotSubsystem::StepGalleryOrbit,
				static_cast<float>(Settle));
		}
		return;
	}

	const FTransform SpawnTransform(
		FRotator::ZeroRotator, ActiveGalleryAnchor + Entry.Offset);
	AActor* Actor = World->SpawnActorDeferred<AActor>(
		ActorClass, SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Actor)
	{
		UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %s skipped spawn failed"), *Entry.Name);
		++GallerySkippedCount;
		++GalleryIndex;
		World->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::BeginGalleryEntry);
		return;
	}
	FString Reason;
	const bool bPreSetupOk = SetupGalleryActor(Actor, Entry, true, Reason);
	Actor->FinishSpawning(SpawnTransform);
	GalleryActor = Actor;
	if (!bPreSetupOk || !SetupGalleryActor(Actor, Entry, false, Reason))
	{
		UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %s skipped %s"), *Entry.Name, *Reason);
		WoTRecordBootstrapHeartbeat(TEXT("gallery-skip"),
			EWoTBootstrapPhase::Ready, 0, {}, {},
			FString::Printf(TEXT("entry=%s reason=%s"), *Entry.Name, *Reason));
		++GallerySkippedCount;
		CleanupGalleryEntry();
		++GalleryIndex;
		World->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::BeginGalleryEntry);
		return;
	}

	GalleryYawIndex = 0;
	GalleryStepIndex = 0;
	SetViewerPoint(ActiveGalleryAnchor);
	// Step entries settle the spawn first (streaming/shaders, same as any entry), then
	// BeginGalleryStep applies step 0's production action and runs its own settle.
	void (UWoTShotSubsystem::*SettleTarget)() = Entry.Steps.IsEmpty()
		? &UWoTShotSubsystem::StepGalleryOrbit : &UWoTShotSubsystem::BeginGalleryStep;
	const double Settle = Entry.SettleSeconds >= 0.0
		? Entry.SettleSeconds : GallerySettleSeconds;
	if (Settle <= 0.0)
	{
		World->GetTimerManager().SetTimerForNextTick(this, SettleTarget);
	}
	else
	{
		World->GetTimerManager().SetTimer(
			Timer, this, SettleTarget, static_cast<float>(Settle));
	}
}

void UWoTShotSubsystem::BeginGalleryStep()
{
	UWorld* World = GetWorld();
	const FGalleryEntry& Entry = GalleryEntries[GalleryIndex];
	if (!Entry.Steps.IsValidIndex(GalleryStepIndex))
	{
		FinishGalleryEntry();
		return;
	}
	const FGalleryStep& Step = Entry.Steps[GalleryStepIndex];
	FString Reason;
	if (!ApplyGalleryTrapStep(Step, Reason))
	{
		// Later steps depend on earlier ones (deploy → edit → arm → touch), so a failed
		// driver invalidates the rest of the ledger: entry-level skip, the existing
		// pattern (shots already written for earlier steps stay valid on disk).
		UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %s skipped step '%s' %s"),
		       *Entry.Name, *Step.Name, *Reason);
		++GallerySkippedCount;
		CleanupGalleryEntry();
		++GalleryIndex;
		World->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::BeginGalleryEntry);
		return;
	}
	GalleryYawIndex = 0;
	// The step settle counts FROM the action, so a timer-driven phase (Spear's
	// thrust/dwell, the burst's lifespan) reaches the intended window by world time —
	// never by forced state.
	const double Settle = Step.SettleSeconds >= 0.0
		? Step.SettleSeconds
		: (Entry.SettleSeconds >= 0.0 ? Entry.SettleSeconds : GallerySettleSeconds);
	if (Settle <= 0.0)
	{
		World->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::StepGalleryOrbit);
	}
	else
	{
		World->GetTimerManager().SetTimer(
			Timer, this, &UWoTShotSubsystem::StepGalleryOrbit,
			static_cast<float>(Settle));
	}
}

void UWoTShotSubsystem::StepGalleryOrbit()
{
	const FGalleryEntry& Entry = GalleryEntries[GalleryIndex];
	const FGalleryStep* Step = Entry.Steps.IsValidIndex(GalleryStepIndex)
		? &Entry.Steps[GalleryStepIndex] : nullptr;
	// A step may narrow the orbit (evenly spaced over its own count) when a phase's
	// live dwell can't cover the full stop count — e.g. Spear's 2s Extended window.
	const int32 YawCount = Entry.bHasPose ? 1
		: (Step && Step->Yaws > 0) ? Step->Yaws : GalleryYawCount;
	if (GalleryYawIndex >= YawCount)
	{
		if (Step && GalleryStepIndex + 1 < Entry.Steps.Num())
		{
			++GalleryStepIndex;
			BeginGalleryStep();
			return;
		}
		FinishGalleryEntry();
		return;
	}
	ACameraActor* Cam = EnsureCamera();
	if (!Cam)
	{
		FinishGalleryEntry();
		return;
	}
	if (Entry.bHasPose)
	{
		// UE1 rotator units, exactly as the golden-manifest replay (StepManifest).
		Cam->SetActorLocationAndRotation(Entry.PoseCamera, FRotator(
			Entry.PoseRotation.X * UE1RotToDeg, Entry.PoseRotation.Y * UE1RotToDeg,
			Entry.PoseRotation.Z * UE1RotToDeg));
		GetWorld()->GetTimerManager().SetTimer(
			Timer, this, &UWoTShotSubsystem::CaptureGalleryYaw, 0.5f);
		return;
	}
	const double YawRadians = 2.0 * UE_DOUBLE_PI *
		static_cast<double>(GalleryYawIndex) / static_cast<double>(YawCount);
	const FVector CameraLocation = ActiveGalleryAnchor + FVector(
		GalleryRadius * FMath::Cos(YawRadians), GalleryRadius * FMath::Sin(YawRadians),
		GalleryHeight);
	FVector Target = GalleryActor.IsValid()
		? GalleryActor->GetActorLocation() : ActiveGalleryAnchor;
	if (GalleryActor.IsValid())
	{
		const FBox Bounds = GalleryActor->GetComponentsBoundingBox(true);
		if (Bounds.IsValid)
		{
			Target = Bounds.GetCenter();
		}
	}
	Cam->SetActorLocationAndRotation(CameraLocation, (Target - CameraLocation).Rotation());
	GetWorld()->GetTimerManager().SetTimer(
		Timer, this, &UWoTShotSubsystem::CaptureGalleryYaw, 0.5f);
}

void UWoTShotSubsystem::CaptureGalleryYaw()
{
	const FGalleryEntry& Entry = GalleryEntries[GalleryIndex];
	// Step entries encode entry+step+yaw so every ledger stop is its own A/B image.
	const FString File = Entry.Steps.IsValidIndex(GalleryStepIndex)
		? FString::Printf(TEXT("%s/%s_%s_y%d.png"), *OutDir, *Entry.Name,
		                  *Entry.Steps[GalleryStepIndex].Name, GalleryYawIndex)
		: FString::Printf(TEXT("%s/%s_y%d.png"), *OutDir, *Entry.Name, GalleryYawIndex);
	FScreenshotRequest::RequestScreenshot(File, false, false);
	++GalleryYawIndex;
	GetWorld()->GetTimerManager().SetTimer(
		Timer, this, &UWoTShotSubsystem::StepGalleryOrbit, 0.4f);
}

void UWoTShotSubsystem::FinishGalleryEntry()
{
	const FGalleryEntry& Entry = GalleryEntries[GalleryIndex];
	if (const AActor* Actor = GalleryActor.Get())
	{
		if (const UWoTVertexAnimComponent* Display =
			Actor->FindComponentByClass<UWoTVertexAnimComponent>())
		{
			const FWoTVertexMeshDiagnostics State = Display->GetMeshDiagnostics();
			const AWoTInventory* Pickup = Cast<AWoTInventory>(Actor);
			const float AmbientGlow = Pickup ? Pickup->AmbientGlow : -1.0f;
			UMaterialInterface* Material = Display->GetMaterial(0);
			UTexture* BoundTexture = nullptr;
			FLinearColor MaterialAmbient = FLinearColor::Black;
			const bool bHasTexture = Material != nullptr
				&& Material->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), BoundTexture);
			const bool bHasMaterialAmbient = Material != nullptr
				&& Material->GetVectorParameterValue(
					FHashedMaterialParameterInfo(TEXT("MLAmbient")), MaterialAmbient);
			const FVector WorldBoundsOrigin = Display->Bounds.Origin;
			const FVector ComponentScale = Display->GetComponentScale();
			const FRotator ActorRotation = Actor->GetActorRotation();
			UE_LOG(LogTemp, Display,
				TEXT("WOTGALLERY-MESH: entry=%s mesh=%s proxy=%s sections=%d materials=%d bounds=(%.1f,%.1f,%.1f) submitted_last_frame=%d hidden=%d component_visible=%d registered=%d render_state=%d ambient_glow=%.1f texture=%s material_ambient=(%.3f,%.3f,%.3f) location=(%.1f,%.1f,%.1f) rotation=(%.1f,%.1f,%.1f) component_scale=(%.3f,%.3f,%.3f) world_bounds_origin=(%.1f,%.1f,%.1f)"),
				*Entry.Name,
				State.BoundMeshId.IsEmpty() ? TEXT("none") : *State.BoundMeshId,
				*State.ProxyKind.ToString(), State.SectionCount, State.MaterialCount,
				State.BoundsExtent.X, State.BoundsExtent.Y, State.BoundsExtent.Z,
				State.bSubmittedDrawLastFrame ? 1 : 0,
				Actor->IsHidden() ? 1 : 0, Display->IsVisible() ? 1 : 0,
				Display->IsRegistered() ? 1 : 0,
				Display->IsRenderStateCreated() ? 1 : 0,
				AmbientGlow,
				bHasTexture && BoundTexture != nullptr
					? *BoundTexture->GetPathName() : TEXT("none"),
				bHasMaterialAmbient ? MaterialAmbient.R : -1.0f,
				bHasMaterialAmbient ? MaterialAmbient.G : -1.0f,
				bHasMaterialAmbient ? MaterialAmbient.B : -1.0f,
				Actor->GetActorLocation().X,
				Actor->GetActorLocation().Y, Actor->GetActorLocation().Z,
				ActorRotation.Pitch, ActorRotation.Yaw, ActorRotation.Roll,
				ComponentScale.X, ComponentScale.Y, ComponentScale.Z,
				WorldBoundsOrigin.X, WorldBoundsOrigin.Y, WorldBoundsOrigin.Z);
			WoTRecordBootstrapHeartbeat(TEXT("gallery-mesh-state"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				FString::Printf(TEXT("entry=%s mesh=%s proxy=%s sections=%d materials=%d bounds=(%.1f,%.1f,%.1f) submitted_last_frame=%d hidden=%d component_visible=%d registered=%d render_state=%d ambient_glow=%.1f texture=%s material_ambient=(%.3f,%.3f,%.3f) location=(%.1f,%.1f,%.1f) rotation=(%.1f,%.1f,%.1f) component_scale=(%.3f,%.3f,%.3f) world_bounds_origin=(%.1f,%.1f,%.1f)"),
					*Entry.Name,
					State.BoundMeshId.IsEmpty() ? TEXT("none") : *State.BoundMeshId,
					*State.ProxyKind.ToString(), State.SectionCount, State.MaterialCount,
					State.BoundsExtent.X, State.BoundsExtent.Y, State.BoundsExtent.Z,
					State.bSubmittedDrawLastFrame ? 1 : 0,
					Actor->IsHidden() ? 1 : 0, Display->IsVisible() ? 1 : 0,
					Display->IsRegistered() ? 1 : 0,
					Display->IsRenderStateCreated() ? 1 : 0,
					AmbientGlow,
					bHasTexture && BoundTexture != nullptr
						? *BoundTexture->GetPathName() : TEXT("none"),
					bHasMaterialAmbient ? MaterialAmbient.R : -1.0f,
					bHasMaterialAmbient ? MaterialAmbient.G : -1.0f,
					bHasMaterialAmbient ? MaterialAmbient.B : -1.0f,
					Actor->GetActorLocation().X,
					Actor->GetActorLocation().Y, Actor->GetActorLocation().Z,
					ActorRotation.Pitch, ActorRotation.Yaw, ActorRotation.Roll,
					ComponentScale.X, ComponentScale.Y, ComponentScale.Z,
					WorldBoundsOrigin.X, WorldBoundsOrigin.Y, WorldBoundsOrigin.Z));
		}
	}
	UE_LOG(LogTemp, Display, TEXT("WOTGALLERY: %s ok"), *Entry.Name);
	++GalleryOkCount;
	CleanupGalleryEntry();
	++GalleryIndex;
	GetWorld()->GetTimerManager().SetTimerForNextTick(
		this, &UWoTShotSubsystem::BeginGalleryEntry);
}

void UWoTShotSubsystem::CleanupGalleryEntry()
{
	TArray<AActor*> SpawnedActors;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (*It != Camera.Get() &&
			!GalleryActorsBefore.Contains(TWeakObjectPtr<AActor>(*It)))
		{
			SpawnedActors.Add(*It);
		}
	}
	for (AActor* Actor : SpawnedActors)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	GalleryActor.Reset();
	GalleryDriverPawn.Reset();   // destroyed above (spawned after the entry snapshot)
	GalleryActorsBefore.Reset();
}

APawn* UWoTShotSubsystem::EnsureGalleryDriverPawn()
{
	if (GalleryDriverPawn.IsValid())
	{
		return GalleryDriverPawn.Get();
	}
	// The trap specs' enemy stand-in (WoTTrapTests): a bare APawn with no PlayerState
	// reads team 255 through Trap.IsInactive — an enemy to any team a begin_edit step
	// stamped — and owns no components, so it renders nothing, overlaps nothing, and
	// never counts against AnyActorsInArea deploy occupancy. Spawned after the entry
	// snapshot, so CleanupGalleryEntry destroys it with the rest.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	GalleryDriverPawn = GetWorld()->SpawnActor<APawn>(
		APawn::StaticClass(), ActiveGalleryAnchor, FRotator::ZeroRotator, Params);
	return GalleryDriverPawn.Get();
}

// The step-ledger driver: every action is an existing PRODUCTION entry point on the
// spawned trap (the repo's no-artificial-seams discipline). Phase state is never set
// directly — timer-driven phases advance on the world clock during the step settle.
bool UWoTShotSubsystem::ApplyGalleryTrapStep(const FGalleryStep& Step, FString& OutReason)
{
	AWoTTrap* Trap = Cast<AWoTTrap>(GalleryActor.Get());
	if (!Trap)
	{
		// A prior damage step may have legitimately destroyed the trap (WallSlab at
		// zero withstand) — nothing left to drive or photograph.
		OutReason = TEXT("trap actor is no longer valid");
		return false;
	}
	const FString& Action = Step.Action;
	if (Action.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (Action.Equals(TEXT("begin_edit"), ESearchCase::IgnoreCase))
	{
		// Trap.uc:67-71 — the citadel edit session's placement half: stamps the team
		// and the AmbientGlow=64 editing cue (the glow variant shot). Also the ONLY
		// production path that assigns a trap's team, so a touch step must be preceded
		// by an edit cycle: the teamless driver pawn reads 255 — same-team (inert) to
		// a never-edited trap's default 255.
		Trap->BeginEditingResource(0);
		return true;
	}
	if (Action.Equals(TEXT("end_edit"), ESearchCase::IgnoreCase))
	{
		// Trap.uc:73-77 — the session end: glow off, collision/blockers re-arm, live.
		Trap->EndEditingResource();
		return true;
	}
	if (Action.Equals(TEXT("touch"), ESearchCase::IgnoreCase))
	{
		APawn* Driver = EnsureGalleryDriverPawn();
		if (!Driver)
		{
			OutReason = TEXT("driver pawn spawn failed");
			return false;
		}
		// Route per class exactly as the specs do — each is that trap's public
		// production touch seam (headless overlaps are awkward to synthesize).
		if (AWoTAlarm* Alarm = Cast<AWoTAlarm>(Trap))
		{
			Alarm->Touch(Driver);          // Waiting.Touch — the player ring
		}
		else if (AWoTPortcullis* Portcullis = Cast<AWoTPortcullis>(Trap))
		{
			Portcullis->OnPortcullisOverlap(nullptr, Driver, nullptr, 0, false, FHitResult());
		}
		else if (AWoTStaircase* Staircase = Cast<AWoTStaircase>(Trap))
		{
			Staircase->OnStaircaseOverlap(nullptr, Driver, nullptr, 0, false, FHitResult());
		}
		else
		{
			Trap->TrapTouch(Driver);       // the blocker-relay seam (Spear Waiting.Touch)
		}
		return true;
	}
	if (Action.Equals(TEXT("activate"), ESearchCase::IgnoreCase)
		|| Action.Equals(TEXT("deactivate"), ESearchCase::IgnoreCase))
	{
		// ActivateResource/DeactivateResource — only the two preplaced toggle
		// resources carry the pair.
		const bool bActivate = Action.Equals(TEXT("activate"), ESearchCase::IgnoreCase);
		if (AWoTPortcullis* Portcullis = Cast<AWoTPortcullis>(Trap))
		{
			if (bActivate) { Portcullis->Activate(); } else { Portcullis->Deactivate(); }
			return true;
		}
		if (AWoTStaircase* Staircase = Cast<AWoTStaircase>(Trap))
		{
			if (bActivate) { Staircase->Activate(); } else { Staircase->Deactivate(); }
			return true;
		}
		OutReason = FString::Printf(
			TEXT("%s requires a toggle resource (Portcullis/Staircase)"), *Action);
		return false;
	}
	if (Action.Equals(TEXT("deploy"), ESearchCase::IgnoreCase))
	{
		// The real create-funnel placement: CalcLocation traces + occupancy from the
		// spawn pose (down-trace snaps to the floor; forward is the wall axis). Real
		// map geometry decides — a rejection here is an honest per-map skip.
		if (!Trap->DeployResource(Trap->GetActorLocation(), Trap->GetActorForwardVector()))
		{
			OutReason = TEXT("DeployResource rejected placement at the anchor");
			return false;
		}
		return true;
	}
	if (Action.Equals(TEXT("damage"), ESearchCase::IgnoreCase))
	{
		// The weave-hit seam (WallSlab stages, FireWallSlab fuel, PortcullisMover
		// withstand). Killer-less point damage, the spec idiom.
		FHitResult Hit;
		Hit.ImpactPoint = Trap->GetActorLocation();
		const FPointDamageEvent DamageEvent(static_cast<float>(Step.Amount), Hit,
		                                    FVector::ForwardVector, nullptr);
		Trap->TakeDamage(static_cast<float>(Step.Amount), DamageEvent, nullptr, nullptr);
		return true;
	}
	OutReason = FString::Printf(TEXT("unknown action '%s'"), *Action);
	return false;
}

void UWoTShotSubsystem::StepClip()
{
	UWorld* World = GetWorld();
	if (ClipIndex >= Clips.Num())
	{
		UE_LOG(LogTemp, Display, TEXT("WOTSHOT: clips done (%d)"), Clips.Num());
		FPlatformMisc::RequestExit(false);
		return;
	}
	const FFeelClip& C = Clips[ClipIndex];
	ACameraActor* Cam = EnsureCamera();
	Cam->SetActorLocationAndRotation(
		C.Pawn + FVector(0, 0, EyeHeight),
		FRotator(C.Pitch * UE1RotToDeg, C.Yaw * UE1RotToDeg, 0));
	SetViewerPoint(C.Pawn);
	FrameIndex = 0;
	TotalFrames = FMath::Max(1, FMath::RoundToInt32(C.Seconds * 30.0));
	UE_LOG(LogTemp, Display, TEXT("WOTSHOT: clip %s, %d frames"), *C.Name, TotalFrames);
	// 2s settle at the new pose (streaming, corona fade-in, sprayer prime)
	World->GetTimerManager().SetTimer(
		Timer, this, &UWoTShotSubsystem::CaptureClipFrame, 2.0f);
}

void UWoTShotSubsystem::CaptureClipFrame()
{
	const FFeelClip& C = Clips[ClipIndex];
	FScreenshotRequest::RequestScreenshot(
		FString::Printf(TEXT("%s/%s/f%05d.png"), *OutDir, *C.Name, FrameIndex),
		false, false);
	if (++FrameIndex < TotalFrames)
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::CaptureClipFrame);
	}
	else
	{
		ClipIndex++;
		GetWorld()->GetTimerManager().SetTimerForNextTick(
			this, &UWoTShotSubsystem::StepClip);
	}
}

void UWoTShotSubsystem::StepManifest()
{
	UWorld* World = GetWorld();
	if (StartIndex >= Poses.Num())
	{
		UE_LOG(LogTemp, Display, TEXT("WOTSHOT: done (%d poses)"), Poses.Num());
		FPlatformMisc::RequestExit(false);
		return;
	}
	EnsureCamera();
	const FGoldenPose& P = Poses[StartIndex];
	const FVector Loc = P.Pawn + FVector(0, 0, EyeHeight);
	const FRotator Rot(P.Pitch * UE1RotToDeg, P.Yaw * UE1RotToDeg, P.Roll * UE1RotToDeg);
	Camera->SetActorLocationAndRotation(Loc, Rot);
	SetViewerPoint(P.Pawn);
	World->GetTimerManager().SetTimer(Timer, this, &UWoTShotSubsystem::Snap, 0.5f);
}

void UWoTShotSubsystem::Step()
{
	UWorld* World = GetWorld();
	const int32 MaxStarts = FMath::Min(Starts.Num(), 6);
	if (StartIndex >= MaxStarts || Starts.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("WOTSHOT: done"));
		FPlatformMisc::RequestExit(false);
		return;
	}
	AActor* Start = Starts[StartIndex].Get();
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawnOrSpectator() : nullptr;
	if (!Start || !Pawn)
	{
		UE_LOG(LogTemp, Error, TEXT("WOTSHOT: no start/pawn"));
		FPlatformMisc::RequestExit(false);
		return;
	}
	if (!bOverviewDone)
	{
		bOverviewDone = true;
		// Overview first: world-geometry bounds from outside, looking at center.
		FBox Bounds(ForceInit);
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			// The camera-locked sky box sits at the view and is scaled far
			// beyond the world: it is not world geometry.
			if (It->IsA<AWoTSkyBox>()) continue;
			Bounds += It->GetComponentsBoundingBox();
		}
		// Guarded shells project the BSP into one code-only actor.
		for (TActorIterator<AWoTRuntimeBSPWorld> It(World); It; ++It)
		{
			Bounds += It->GetComponentsBoundingBox(true);
		}
		UE_LOG(LogTemp, Display, TEXT("WOTSHOT: geo bounds center=%s extent=%s"),
		       *Bounds.GetCenter().ToCompactString(), *Bounds.GetExtent().ToCompactString());
		const FVector Loc = Bounds.GetCenter() + Bounds.GetExtent() * FVector(1.3, 1.3, 1.0);
		if (Pawn->SetActorLocation(Loc, false, nullptr,
			ETeleportType::TeleportPhysics))
		{
			UWoTZoneIndex::RefreshAfterRelocation(Pawn);
		}
		PC->SetControlRotation((Bounds.GetCenter() - Loc).Rotation());
		PanIndex = -1; // sentinel: next Snap writes the overview file
		World->GetTimerManager().SetTimer(Timer, this, &UWoTShotSubsystem::Snap, 0.6f);
		return;
	}
	AActor* S = Starts[StartIndex].Get();
	FRotator Rot = S->GetActorRotation();
	Rot.Yaw += PanIndex * 90.0f;
	Rot.Pitch = 0;
	if (Pawn->SetActorLocation(S->GetActorLocation() + FVector(0, 0, 40),
		false, nullptr, ETeleportType::TeleportPhysics))
	{
		UWoTZoneIndex::RefreshAfterRelocation(Pawn);
	}
	PC->SetControlRotation(Rot);
	World->GetTimerManager().SetTimer(Timer, this, &UWoTShotSubsystem::Snap, 0.6f);
}

void UWoTShotSubsystem::Snap()
{
	if (!ManifestDir.IsEmpty())
	{
		const FGoldenPose& P = Poses[StartIndex];
		FScreenshotRequest::RequestScreenshot(OutDir / P.File, false, false);
		StartIndex++;
		GetWorld()->GetTimerManager().SetTimer(Timer, this, &UWoTShotSubsystem::StepManifest, 0.4f);
		return;
	}
	FString MapName = GetWorld()->GetMapName().Replace(TEXT("UEDPIE_0_"), TEXT(""));
	if (const TOptional<FString> GuardedIdentity =
		WoTMapIdentityForGuardedShellPackage(GetWorld()->GetOutermost()->GetName());
		GuardedIdentity.IsSet())
	{
		MapName = GuardedIdentity.GetValue();
	}
	const FString File = PanIndex < 0
		? FString::Printf(TEXT("%s/%s_overview.png"), *OutDir, *MapName)
		: FString::Printf(TEXT("%s/%s_s%d_p%d.png"), *OutDir, *MapName, StartIndex, PanIndex);
	FScreenshotRequest::RequestScreenshot(File, false, false);
	if (PanIndex < 0)
	{
		PanIndex = 0;
	}
	// 4-way pan on the first start (orientation evidence), single shot after.
	else if (StartIndex == 0 && PanIndex < 3)
	{
		PanIndex++;
	}
	else
	{
		PanIndex = 0;
		StartIndex++;
	}
	GetWorld()->GetTimerManager().SetTimer(Timer, this, &UWoTShotSubsystem::Step, 0.4f);
}

#else

void UWoTShotSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
}

#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES

bool UWoTShotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if !WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	(void)Outer;
	return false;
#else
	return Super::ShouldCreateSubsystem(Outer);
#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
}
