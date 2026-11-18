#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WoTBSPProofHarness.generated.h"

class AActor;
class ACameraActor;
class UStaticMesh;
class UWorld;
class FWoTBSPProofLogDevice;
class FJsonObject;
class FJsonValue;

// Packaged-only-by-command-line proof collector for the Arena_07 BSP gate.
// With no -wotbspproof=<absolute directory> it registers no delegates, creates
// no directories, captures no logs, and never ticks.
UCLASS()
class UWoTBSPProofSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Declared here, defined in the .cpp where FWoTBSPProofLogDevice is
	// complete: the UHT-generated constructor otherwise instantiates the
	// TUniquePtr deleter against the forward declaration.
	UWoTBSPProofSubsystem(const FObjectInitializer& ObjectInitializer);
	virtual ~UWoTBSPProofSubsystem() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	static bool ValidatePlanJson(const FString& Text, FString& OutError);
	static int32 ResolveFaceSection(const UStaticMesh* Mesh, int32 FaceIndex);
	static bool BuildRenderMeshJson(UStaticMesh* Mesh,
		TSharedPtr<FJsonObject>& OutJson, FString& OutError);

	enum class EMode : uint8 { Render, Collision, Nav, Travel, Full };

private:
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	enum class EState : uint8
	{
		Dormant, WaitingForArena, WaitingForReadiness, ReferenceSettle,
		RenderDump, Screenshots, CollisionSettle, CollisionProjectiles,
		NavWait, NavSeeking, TravelToFrontEnd, TravelToArena,
		TravelCompactProjectiles, Finished, Failed
	};

	void HandleGameInstanceWorldChanged(
		UGameInstance* GameInstance, UWorld* OldWorld, UWorld* NewWorld);
	void HandleWorldBeginPlay();
	void HandleWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void BindWorld(UWorld* World);
	void BeginArenaRun(UWorld* World);
	void AdvanceReadyRun();
	void StartRender();
	bool DumpRenderRole(const FString& Role, const FString& DirectoryPrefix,
		const FString& Filename);
	void StepScreenshot();
	void StartCollision(bool bCompact);
	void FinishPawnSettle();
	void StartProjectiles(bool bCompact);
	void FinishProjectiles();
	void BeginNavWait(bool bCompact);
	void StartNav(bool bCompact);
	void FinishNavSeeking(const FString& CompletionReason);
	void BeginTravel();
	bool WriteTravelCapture();
	void FinishTravelCycle();
	void WriteCollision(const FString& Filename);
	void WriteNav(const FString& Filename);
	void WriteProofLog();
	void FinishSuccess();
	void Fail(const FString& Reason);
	bool WriteJson(const FString& Filename,
		const TSharedRef<FJsonObject>& Object, FString& OutError) const;
	TSharedPtr<FJsonObject> HitJson(
		const FString& Id, const FString& Kind, const FVector& Start,
		const FVector& End, const FHitResult& Hit) const;
	void RecordProjectileHit(AActor* Projectile, const FHitResult& Hit);

#endif  // WOT_WITH_BOOTSTRAP_TEST_HARNESSES

	// UHT forbids UFUNCTION inside preprocessor blocks; the delegate targets
	// are declared unconditionally and their bodies are stubbed when the
	// harness is compiled out.
	UFUNCTION()
	void HandleProjectileActorHit(AActor* SelfActor, AActor* OtherActor,
		FVector NormalImpulse, const FHitResult& Hit);
	UFUNCTION()
	void HandleSeekingDestroyed(AActor* DestroyedActor);

#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	bool bActive = false;
	bool bCompactRun = false;
	bool bNavCompact = false;
	bool bNavObservedActive = false;
	bool bReturningToArena = false;
	EMode Mode = EMode::Full;
	EState State = EState::Dormant;
	FString OutDir;
	FString PlanPath;
	FString FailureReason;
	TSharedPtr<FJsonObject> Plan;
	TWeakObjectPtr<UWorld> BoundWorld;
	FDelegateHandle WorldChangedHandle;
	FDelegateHandle WorldBeginPlayHandle;
	FDelegateHandle WorldTickHandle;
	FDelegateHandle ImpactProbeHandle;
	// Raw pointer, owned here, freed in Deinitialize: the UHT-generated
	// vtable-helper constructor instantiates member destructors in gen.cpp,
	// where a TUniquePtr over this forward-declared type cannot compile.
	FWoTBSPProofLogDevice* LogDevice = nullptr;
	int32 WaitTicks = 0;
	int32 ScreenshotIndex = 0;
	int32 ScreenshotWaitTicks = 0;
	int32 ProjectileTicksRemaining = 0;
	int32 NavWaitTicks = 0;
	int32 NavQuietTicks = 0;
	int32 NavEpochInitialTileCount = 0;
	int32 NavSeekingTicksRemaining = 0;
	int32 TravelCount = 10;
	int32 TravelIndex = 0;
	double SettleGameSeconds = 0.0;
	TArray<TWeakObjectPtr<AActor>> SettlingPawns;
	TArray<FString> SettlingPawnIds;
	TArray<TWeakObjectPtr<AActor>> ActiveProjectiles;
	TArray<TSharedPtr<FJsonValue>> ProbeRecords;
	TArray<TSharedPtr<FJsonValue>> SettleRecords;
	TArray<TSharedPtr<FJsonValue>> ProjectileRecords;
	TArray<TSharedPtr<FJsonValue>> NavPathRecords;
	TMap<TWeakObjectPtr<AActor>, int32> ProjectileRecordIndices;
	TMap<TWeakObjectPtr<AActor>, int32> ProjectileExpiryTicks;
	TWeakObjectPtr<AActor> SeekingProjectile;
	TWeakObjectPtr<AActor> SeekingTarget;
	TSharedPtr<FJsonObject> SeekingRecord;
	TArray<TSharedPtr<FJsonValue>> SeekingTrajectory;
	FVector LastSeekingPosition = FVector::ZeroVector;
	FVector SeekingSpawnPosition = FVector::ZeroVector;
	FVector SeekingTargetPosition = FVector::ZeroVector;
	FVector SeekingObstructionVector = FVector::ZeroVector;
	int32 SeekingSideSign = 0;
	bool bSeekingDestroyedAtTarget = false;
	FString SeekingHitActorClass;
	TSharedPtr<FJsonObject> NavBoundsRecord;
	int32 NavTileCount = 0;
	TWeakObjectPtr<ACameraActor> Camera;
#endif // WOT_WITH_BOOTSTRAP_TEST_HARNESSES
};
