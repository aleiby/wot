#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTShotSubsystem.generated.h"

// Screenshot harness. Four modes:
//  -wotshot=<out_dir>                       PlayerStart walk (bring-up): every
//    start, facing its rotation, 4-way pan on the first, then quits.
//  -wotshot=<out_dir> -wotmanifest=<dir>    Golden-pose replay: reads
//    <dir>/<Map>/manifest.json (exports/reference/shots — the original-game
//    capture manifests) and renders every shot from the exact pose:
//    camera = pawn location + (0,0,eye_height), UE1 rotator units, FOV 90.
//    Output files carry the same names as the golden PNGs for 1:1 diffing.
//  -wotshot=<out_dir> -wotclips=<json>      Feel-clip recorder: the json is
//    {"clips":[{"name","pawn":[x,y,z],"yaw","pitch","seconds"}...]} (UE1
//    rotator units). Captures seconds*30 frames per clip into
//    <out_dir>/<name>/f%05d.png. Launch with -benchmark -fps=30 for exact
//    fixed-step animation. Coronas/waveforms run LIVE (no snap/freeze) —
//    this mode exists to compare motion against exports/reference/feel.
//  -wotshot=<out_dir> -wotgallery=<json>    Runtime-actor gallery: reads the
//    json ledger, spawns each visual actor at a lit real-map anchor, and writes
//    deterministic orbit shots as <out_dir>/<name>_y<stop>.png. Coronas and
//    light waveforms are pinned for stable per-build image A/B comparisons.
//    Trap entries (kind "trap") may carry a "steps" ledger: each named step is
//    driven through a PRODUCTION entry point (BeginEditingResource /
//    EndEditingResource / DeployResource / the touch and damage seams) and then
//    orbit-shot as <out_dir>/<name>_<step>_y<stop>.png; phase timers run live
//    (the per-step settle_seconds rides the world clock, never forced state).
//    Pose entries (kind "pose") spawn nothing: they place the
//    camera at an exact "camera" point with a UE1-unit "rotation" and write one
//    frame <name>_y0.png. Any entry may carry "map": it then runs only when the
//    hosted/loaded map identity matches (guarded shells resolve through the
//    adopted shell->map reverse lookup), so one ledger serves all 19 maps.
UCLASS()
class UWoTShotSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override
	{
		return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
	}

private:
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	struct FGoldenPose
	{
		FString File;
		FVector Pawn = FVector::ZeroVector;
		double Pitch = 0, Yaw = 0, Roll = 0;  // UE1 rotator units (65536 = 360°)
	};

	struct FFeelClip
	{
		FString Name;
		FVector Pawn = FVector::ZeroVector;
		double Pitch = 0, Yaw = 0;            // UE1 rotator units
		double Seconds = 20.0;
	};

	// One named stop on a trap entry's phase ledger. Action vocabulary (validated at
	// parse): none | begin_edit | end_edit | touch | activate | deactivate | deploy |
	// damage (damage requires amount > 0). Every action maps to a production entry
	// point on the spawned AWoTTrap — the harness never forces phase state.
	struct FGalleryStep
	{
		FString Name;                  // filename token: <entry>_<name>_y<stop>.png
		FString Action = TEXT("none");
		double Amount = 0.0;           // damage action only
		double SettleSeconds = -1.0;   // -1 → entry/global settle (counted FROM the action)
		int32 Yaws = -1;               // -1 → global orbit yaw count
	};

	struct FGalleryEntry
	{
		FString Name;
		FString ClassPath;
		FString Kind = TEXT("none");
		FString Skip;
		FString Config;
		FString DevMesh;
		FString DevSkin;
		double DevScale = 1.0;
		int32 AmbientGlow = INDEX_NONE;
		FName CreatureRow;
		bool bExpectNoMesh = false;
		FVector Anchor = FVector::ZeroVector;
		bool bHasAnchor = false;
		bool bExisting = false;
		FVector Offset = FVector::ZeroVector;
		double SettleSeconds = -1.0;
		TArray<FGalleryStep> Steps;    // kind "trap" only (setup rejects otherwise)
		FString Map;                   // empty: every map; else the one map identity
		FVector PoseCamera = FVector::ZeroVector;  // kind "pose": exact camera point
		FVector PoseRotation = FVector::ZeroVector; // kind "pose": pitch/yaw/roll, UE1 units
		bool bHasPose = false;
	};

	void Step();
	void Snap();
	bool LoadManifest(const FString& MapName);
	void StepManifest();
	bool LoadClips(const FString& Path);
	void StepClip();
	void CaptureClipFrame();
	bool LoadGallery(const FString& Path, const FString& MapName);
	void BeginHostedGallery();
	void PrepareGalleryStage();
	void BeginGalleryEntry();
	void BeginGalleryStep();
	void StepGalleryOrbit();
	void CaptureGalleryYaw();
	void FinishGalleryEntry();
	void CleanupGalleryEntry();
	bool SetupGalleryActor(AActor* Actor, const FGalleryEntry& Entry,
	                       bool bBeforeBeginPlay, FString& OutReason);
	bool ApplyGalleryTrapStep(const FGalleryStep& Step, FString& OutReason);
	class APawn* EnsureGalleryDriverPawn();
	class ACameraActor* EnsureCamera();
	void SetViewerPoint(const FVector& PawnPoint);

	FString OutDir;
	FString ManifestDir;
	FString GalleryPath;
	FString HostedGalleryMap;
	FString GalleryMapName;
	double EyeHeight = 42.0;

	TArray<TWeakObjectPtr<AActor>> Starts;
	TArray<FGoldenPose> Poses;
	TArray<FFeelClip> Clips;
	TArray<FGalleryEntry> GalleryEntries;
	TWeakObjectPtr<class ACameraActor> Camera;
	TWeakObjectPtr<AActor> GalleryActor;
	// The touch-step stand-in (the trap specs' enemy idiom): a bare APawn with no
	// PlayerState reads team 255 and carries no components, so it never renders,
	// never overlaps, and never counts against deploy occupancy. Per entry, lazily.
	TWeakObjectPtr<class APawn> GalleryDriverPawn;
	TWeakObjectPtr<class AWoTLeafMap> LeafMap;
	TSet<TWeakObjectPtr<AActor>> GalleryActorsBefore;
	FVector GalleryAnchor = FVector::ZeroVector;
	FVector ActiveGalleryAnchor = FVector::ZeroVector;
	double GalleryRadius = 180.0;
	double GalleryHeight = 60.0;
	double GallerySettleSeconds = 1.0;
	bool bLeafMapSearched = false;
	int32 StartIndex = 0;
	int32 PanIndex = 0;
	int32 ClipIndex = 0;
	int32 FrameIndex = 0;
	int32 TotalFrames = 0;
	int32 GalleryIndex = 0;
	int32 GalleryStepIndex = 0;
	int32 GalleryYawIndex = 0;
	int32 GalleryYawCount = 4;
	int32 GalleryOkCount = 0;
	int32 GallerySkippedCount = 0;
	int32 GalleryOtherMapCount = 0;
	bool bOverviewDone = false;
	bool bHostedGalleryRequestAccepted = false;
	int32 HostedGalleryAttempts = 0;
	FTimerHandle Timer;
#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
};
