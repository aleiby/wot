#pragma once

// TCP command channel for the net-test harness. Test-only.
// Newline-delimited JSON over TCP: the first client line must be {"token":"..."};
// each later line {"id":N,"cmd":"...",...} gets {"id":N,"ok":bool,...}. The socket
// is built + accepted on the game thread (no listener thread), so the subsystem
// just pumps it each tick. LAN-only, token-gated.

#include "CoreMinimal.h"
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "WoTNetTestActions.h"   // WoTNetTest::FMoveState (HeldMove)
#include "WoTTrailerDirector.h"

class UWorld;
class ULevel;
class FSocket;
class FJsonObject;
class FJsonValue;
class ACameraActor;
class AActor;
class AWoTCharacter;
class FWoTNetTestLogDevice;
struct FHitResult;

class FWoTNetTestChannel
{
public:
	~FWoTNetTestChannel();

	// Bind + listen (non-blocking) on BindAddr:Port. Port 0 => ephemeral; read
	// the actual port from GetBoundPort(). Returns false if the socket fails.
	bool Start(uint16 Port, const FString& BindAddr, const FString& Token);
	void Stop();

	// Accept/read/dispatch/respond across all connections; call every game tick.
	void PumpOnGameThread(UWorld* World);
	void TickDirector(UWorld* World);

	uint16 GetBoundPort() const { return BoundPort; }

#if WITH_AUTOMATION_TESTS
	/** Exact parser used by the host verb; malformed optional values stay LAN. */
	static bool HostLanFromCommandForTesting(const FJsonObject& Request);
	/** Forces the next successfully placed tour point through its rollback path. */
	void ForceNextTourPointPostCheckFailureForTesting()
	{
		bForceNextTourPointPostCheckFailureForTesting = true;
	}
	/** Direct verb seam for guarded automation specs (no TCP timing involved). */
	bool DispatchCommandForTesting(UWorld* World,
		const TSharedRef<FJsonObject>& Request,
		const TSharedRef<FJsonObject>& Response)
	{
		return DispatchCommand(World, Request, Response);
	}
#endif

	TMap<int32, WoTNetTest::FMoveState> HeldMoves;

private:
	struct FConn
	{
		FSocket* Socket = nullptr;
		bool bAuthed = false;
		FString RecvBuf;
	};
	void HandleCommand(UWorld* World, FConn& Conn, const TSharedRef<FJsonObject>& Req);
	bool DispatchCommand(UWorld* World, const TSharedRef<FJsonObject>& Req,
		const TSharedRef<FJsonObject>& Res);
	static bool HostLanFromCommand(const FJsonObject& Request);
	void CloseSocket(FSocket*& Socket);
	static bool PlayerIndex(const FJsonObject& Request, int32& OutPlayerIndex);
	void ResetProbes(UWorld* World);
	bool BeginProbe(UWorld* World, const FJsonObject& Request);
	bool EndProbe(UWorld* World, const FString& Label);
	void TickProbes(UWorld* World);
	void DrainLogWarnings(UWorld* World);
	void TickFallWatch(UWorld* World);
	void EndFallEpisode(AWoTCharacter* Pawn);
	void RecordRouteSnap(UWorld* World, const AWoTCharacter* Pawn,
		const TCHAR* Reason, const FVector& From, const FVector& To,
		double Distance, bool bSnapped);
	void EndAllFallEpisodes();
	void ObserveProjectileImpact(AActor* ProjectileActor, const FHitResult& Hit);
	TSharedPtr<FJsonObject> BuildProbeResults(UWorld* World) const;
	bool AddLateJoin(UWorld* World, const FString& Name, const FString& Faction,
		bool bRequireDisguise, TArray<TSharedPtr<FJsonValue>>& OutPuppets);
	bool VerifyLateJoin(UWorld* World);
	static TMap<FString, int32> AnimationFrames(UWorld* World);

	struct FLateJoinPawnState
	{
		FVector Location = FVector::ZeroVector;
		FString Class;
		FString PersonalSkin;
		bool bDisguised = false;
		FString DisguiseClass;
		FString DisguiseSkin;
	};
	struct FLateJoinProbe
	{
		TMap<FString, FLateJoinPawnState> Expected;
		double CreatedTime = 0.0;
		int32 PlayerIndex = INDEX_NONE;
		bool bRequireDisguise = false;
		bool bExpectedDisguise = false;
		bool bExpectedDistinctDisguise = false;
	};

	struct FProbeWindow
	{
		FString Label;
		FString Kind;
		FString Weave;
		FString View;
		FString DefenseCaster;
		TArray<FString> Casters;
		TArray<FString> AnimationTargets;
		double StartTime = 0.0;
		double EndTime = 0.0;
		bool bProjectileOrEffect = false;
		bool bImpactOrDecal = false;
		bool bDefenseEffect = false;
		TMap<FString, int32> AnimationStart;
		TMap<FString, int32> AnimationEnd;
		TMap<FString, int32> AnimationLast;
		TMap<FString, int32> AnimationAdvances;
		TSet<FString> BaselineActors;
		TSet<FString> BaselineComponents;
		TSet<FString> LineageActors;
		// Last observed positions survive a projectile's local teardown long
		// enough to attribute its ownerless retail impact cosmetic.
		TMap<FString, FVector> LineageLocations;
		// Heal has no spawned actor in retail; observe the caster's real health
		// trough and subsequent increase during the cast window.
		TMap<FString, int32> LowestCasterHealth;
		// Last sampled cast positions let Shift require both its real arrival
		// sparkle and its frame-local teleport, without counting setup teleports.
		TMap<FString, FVector> LastCasterLocations;
		TArray<FString> EffectEvidence;
		TArray<FString> ImpactEvidence;
		// How far local player 0 walked over the floor (XY) in the window;
		// teleports (tour points, re-stations) do not count.
		double WalkedXY = 0.0;
		FVector WalkerSample = FVector::ZeroVector;
		bool bHasWalkerSample = false;
	};

	struct FLookState
	{
		FRotator Start = FRotator::ZeroRotator;
		FRotator Target = FRotator::ZeroRotator;
		double StartTime = 0.0;
		double Duration = 0.0;
		WoTTrailer::EEase Ease = WoTTrailer::EEase::Linear;
		TWeakObjectPtr<AWoTCharacter> TrackPawn;
		bool bTracking = false;
		bool bUseSpring = false;
		float SpringSeconds = 0.0f;
		WoTTrailer::FScalarSpring YawSpring;
		WoTTrailer::FScalarSpring PitchSpring;
	};
	struct FMoveToState
	{
		TArray<FVector> Path;
		WoTTrailer::FArcLengthPath Curve;
		int32 Waypoint = 0;
		float Speed = 1.0f;
		float Tolerance = 40.0f;
		float Lead = 100.0f;
		float CurveDistance = 0.0f;
		float AccelSeconds = 0.0f;
		float DecelSeconds = 0.0f;
		// Seconds the whole route walk should take (0 = as fast as it goes).
		float PaceSeconds = 0.0f;
		float FaceLagSeconds = 0.0f;
		double StartTime = 0.0;
		bool bFacePath = false;
		bool bCurved = false;
		bool bJumpWhenBlocked = false;
		bool bHasBlockedMotionSample = false;
		FVector BlockedMotionSampleLocation = FVector::ZeroVector;
		double BlockedMotionSampleTime = 0.0;
		// Route progress (curved walks) at the blocked-motion sample.
		float BlockedMotionSampleDistance = 0.0f;
		double LastAssistedJumpTime = -1.0;
		// In-walk recovery for curved walks with snap > 0: a pawn that strays
		// more than Snap from the route, or stops advancing along it, is
		// re-stationed further along the route.
		float Snap = 0.0f;
		float ProgressSampleDistance = 0.0f;
		double ProgressSampleTime = 0.0;
		// Re-stations since the walker last advanced on the route by itself,
		// and the end of the brief grace that follows each one.
		int32 Restations = 0;
		double RestationGraceUntil = 0.0;
		WoTTrailer::FScalarSpring FaceYawSpring;
		WoTTrailer::FScalarSpring FacePitchSpring;
		// Where the walker stood when route progress was last tracked.
		FVector TrackSample = FVector::ZeroVector;
		bool bHasTrackSample = false;
		bool bDone = false;
	};
	struct FVectorSpringState
	{
		WoTTrailer::FScalarSpring X;
		WoTTrailer::FScalarSpring Y;
		WoTTrailer::FScalarSpring Z;
	};
	struct FFallEpisode
	{
		FVector Start = FVector::ZeroVector;
		double StartTime = 0.0;
		TSharedPtr<FJsonObject> Record;
	};
	enum class ECameraMode : uint8 { Pov, Free, Follow };

	TMap<int32, FLookState> Looks;
	TMap<int32, FMoveToState> MoveTos;
	TMap<int32, double> JumpReleaseTimes;
	ECameraMode CameraMode = ECameraMode::Pov;
	TWeakObjectPtr<ACameraActor> DirectorCamera;
	TArray<WoTTrailer::FCameraKey> CameraKeys;
	double CameraStartTime = 0.0;
	WoTTrailer::EEase CameraEase = WoTTrailer::EEase::Linear;
	bool bStyledCamera = false;
	TOptional<float> CameraFOVOverride;
	TWeakObjectPtr<AActor> CameraLookAtTarget;
	float CameraLookAtSpringSeconds = 0.35f;
	WoTTrailer::FScalarSpring CameraYawSpring;
	WoTTrailer::FScalarSpring CameraPitchSpring;
	TWeakObjectPtr<AWoTCharacter> FollowPawn;
	FVector FollowOffset = FVector::ZeroVector;
	FVector FollowLookAtOffset = FVector::ZeroVector;
	FVector FollowPreviousTarget = FVector::ZeroVector;
	double FollowPreviousTime = 0.0;
	float FollowHeading = 0.0f;
	float FollowLagSeconds = 0.0f;
	float FollowRotationLagSeconds = 0.0f;
	float FollowWallRadius = 12.0f;
	float FollowClearance = 6.0f;
	bool bFollowHeadingFrame = false;
	bool bFollowAvoidWalls = false;
	FVectorSpringState FollowPositionSpring;
	WoTTrailer::FScalarSpring FollowYawSpring;
	WoTTrailer::FScalarSpring FollowPitchSpring;
	WoTTrailer::FScalarSpring FollowHeadingSpring;
	WoTTrailer::FScalarSpring FollowArmSpring;
	bool bFollowLookAt = true;
	float ShakeAmplitudeDegrees = 0.0f;
	float ShakeFrequencyHz = 0.0f;
	int32 ShakeSeed = 0;

	TArray<WoTTrailer::FTimelineEntry> Timeline;
	TArray<FString> TimelineErrors;
	int32 TimelineNext = 0;
	double TimelineStartTime = 0.0;
	bool bTimelineRunning = false;
	bool bTimelineDone = false;

	TOptional<FProbeWindow> ActiveProbe;
	TArray<FProbeWindow> ProbeWindows;
	TArray<TSharedPtr<FJsonValue>> ProbeLateJoins;
	TOptional<FLateJoinProbe> PendingLateJoin;
	TArray<TSharedPtr<FJsonValue>> ProbeAnomalies;
	TArray<TSharedPtr<FJsonValue>> ProbeRouteSnaps;
	TArray<TSharedPtr<FJsonValue>> ProbeAssistedJumps;
	TArray<TSharedPtr<FJsonValue>> ProbeFalls;
	TMap<TWeakObjectPtr<AWoTCharacter>, FFallEpisode> FallEpisodes;
	TSet<TWeakObjectPtr<AWoTCharacter>> BelowMinFallLatches;
	bool bFallWatchEnabled = false;
	float FallWatchMinZ = 0.0f;
	float FallWatchSeconds = 2.5f;
	TSet<FString> ScenarioBaselineActors;
	int64 SeekerProbeSamples = 0;
	double ProbeEpochTime = -1.0;
	// Capture keeps every Nth rendered frame (the reels encode 30 of the
	// 60 simulated frames a second); 0 start frame = no decimated capture.
	int32 CaptureEvery = 1;
	uint64 CaptureStartFrame = 0;
	// Capture also records the master mix when the rig runs a non-realtime mixer;
	// TickDirector then renders that mix one frame step at a time.
	bool bCaptureAudio = false;
	bool bDrivingAudio = false;
	TWeakObjectPtr<UWorld> ObservedWorld;
	TWeakObjectPtr<ULevel> ObservedLevel;
	uint64 WorldGeneration = 0;
	FDelegateHandle ProjectileImpactHandle;
	FWoTNetTestLogDevice* LogDevice = nullptr;

	FSocket* ListenSocket = nullptr;
	TArray<FConn> Conns;
	FString Token;
	uint16 BoundPort = 0;
#if WITH_AUTOMATION_TESTS
	bool bForceNextTourPointPostCheckFailureForTesting = false;
#endif
};

#endif
