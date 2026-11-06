#include "WoTNetTestChannel.h"
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "WoTNetTestSnapshot.h"
#include "WoTNetTestActions.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/TcpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "AudioMixerPlatformNonRealtime.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInterface.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/HUD.h"
#include "Misc/App.h"
#include "Misc/StringOutputDevice.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Logging/StructuredLog.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Actor.h"
#include "../WoTCharacter.h"
#include "../WoTAppearEffect.h"
#include "../WoTPayloadSoundWave.h"
#include "../WoTSound.h"
#include "../WoTBeamEffect.h"
#include "../WoTMeshFX.h"
#include "../WoTVertexAnimComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "../WoTDecal.h"
#include "../WoTWeaveFX.h"
#include "../WoTPlayerController.h"
#include "../WoTParticleSprayer.h"
#include "../WoTSpriteBillboard.h"
#include "../WoTUE1Collision.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../Inventory/WoTInventory.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTLeech.h"
#include "../Inventory/WoTReflector.h"
#include "../Inventory/WoTResourceSpawnerInv.h"
#include "../Inventory/WoTAngrealInvGuardian.h"
#include "../Inventory/WoTAngrealInvDisguise.h"
#include "../Inventory/WoTAngrealInvHeal.h"
#include "../Inventory/WoTAngrealInvLevitate.h"
#include "../Inventory/WoTAngrealInvLightGlobe.h"
#include "../Inventory/WoTAngrealInvShift.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Projectiles/WoTGenericProjectile.h"
#include "../Projectiles/WoTSeekingProjectile.h"
#include "../Projectiles/WoTAngrealAMAProjectile.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTDistantEyeProjectile.h"
#include "../Projectiles/WoTAngrealIllusionProjectile.h"
#include "../Projectiles/WoTEarthTremorProjectile.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../WoTLightGlobe.h"
#include "Components/SphereComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Info.h"

class FWoTNetTestLogDevice final : public FOutputDevice
{
public:
	FWoTNetTestLogDevice()
		: RegistrationLogTime(FPlatformTime::Seconds() - GStartTime)
	{
	}

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity,
		const FName& Category) override
	{
		Enqueue(Message, Verbosity, Category);
	}

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity,
		const FName& Category, const double Time) override
	{
		// UE 5.8 replays GLog's backlog when a device is registered. Capture begins
		// at channel Start(), so discard records that predate registration.
		if (Time < RegistrationLogTime) return;
		Enqueue(Message, Verbosity, Category);
	}

	virtual void SerializeRecord(const UE::FLogRecord& Record) override
	{
		TStringBuilder<512> Message;
		Record.FormatMessageTo(Message);
		Enqueue(*Message, Record.GetVerbosity(), Record.GetCategory());
	}

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	void Drain(double CaptureTime, bool bTimelineTime)
	{
		TArray<FPendingLine> Local;
		double PlatformTime = 0.0;
		double PreviousCaptureTime = 0.0;
		double PreviousPlatformTime = 0.0;
		bool bPreviousTimelineTime = false;
		bool bHadTimeAnchor = false;
		{
			FScopeLock Lock(&PendingMutex);
			PlatformTime = FPlatformTime::Seconds();
			Local = MoveTemp(Pending);
			Pending.Reset();
			PreviousCaptureTime = AnchorCaptureTime;
			PreviousPlatformTime = AnchorPlatformTime;
			bPreviousTimelineTime = bAnchorTimelineTime;
			bHadTimeAnchor = bHasTimeAnchor;
			AnchorCaptureTime = CaptureTime;
			AnchorPlatformTime = PlatformTime;
			bAnchorTimelineTime = bTimelineTime;
			bHasTimeAnchor = true;
		}
		for (const FPendingLine& Line : Local)
		{
			const double LineTime = EventTime(Line.PlatformTime, CaptureTime,
				PlatformTime, bTimelineTime, bHadTimeAnchor,
				PreviousCaptureTime, PreviousPlatformTime,
				bPreviousTimelineTime);
			if (IsExcluded(Line.Category, Line.Message)) continue;
			const FString Normalized = NormalizeMessage(Line.Message);
			const FString Key = FString::Printf(TEXT("%s\x1f%d\x1f%s"),
				*Line.Category.ToString(), static_cast<int32>(Line.Verbosity),
				*Normalized);
			if (const int32* ExistingIndex = EntryIndices.Find(Key))
			{
				FEntry& Existing = Entries[*ExistingIndex];
				++Existing.Count;
				if (Line.PlatformTime < Existing.FirstPlatformTime)
				{
					Existing.FirstPlatformTime = Line.PlatformTime;
					Existing.FirstTime = LineTime;
				}
				if (Line.PlatformTime >= Existing.LastPlatformTime)
				{
					Existing.LastPlatformTime = Line.PlatformTime;
					Existing.LastTime = LineTime;
				}
				continue;
			}
			if (Entries.Num() >= MaxDistinctEntries)
			{
				++Overflow;
				continue;
			}
			FEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Category = Line.Category.ToString();
			Entry.Verbosity = VerbosityName(Line.Verbosity);
			Entry.Message = Normalized;
			Entry.FirstPlatformTime = Line.PlatformTime;
			Entry.LastPlatformTime = Line.PlatformTime;
			Entry.FirstTime = LineTime;
			Entry.LastTime = LineTime;
			EntryIndices.Add(Key, Entries.Num() - 1);
		}
	}

	void Reset()
	{
		FScopeLock Lock(&PendingMutex);
		Pending.Reset();
		Entries.Reset();
		EntryIndices.Reset();
		Overflow = 0;
		bHasTimeAnchor = false;
	}

	TArray<TSharedPtr<FJsonValue>> JsonEntries() const
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Entries.Num());
		for (const FEntry& Entry : Entries)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("category"), Entry.Category);
			Object->SetStringField(TEXT("verbosity"), Entry.Verbosity);
			Object->SetStringField(TEXT("message"), Entry.Message);
			Object->SetNumberField(TEXT("count"), Entry.Count);
			Object->SetNumberField(TEXT("first_t"), Entry.FirstTime);
			Object->SetNumberField(TEXT("last_t"), Entry.LastTime);
			Result.Add(MakeShared<FJsonValueObject>(Object));
		}
		return Result;
	}

	int64 GetOverflow() const { return Overflow; }

private:
	void Enqueue(const TCHAR* Message, ELogVerbosity::Type Verbosity,
		const FName& Category)
	{
		const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(
			Verbosity & ELogVerbosity::VerbosityMask);
		if (Level != ELogVerbosity::Warning
			&& Level != ELogVerbosity::Error
			&& Level != ELogVerbosity::Fatal)
		{
			return;
		}
		FScopeLock Lock(&PendingMutex);
		Pending.Add({Category, Level, Message ? FString(Message) : FString(),
			FPlatformTime::Seconds()});
	}
	struct FPendingLine
	{
		FName Category;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Warning;
		FString Message;
		double PlatformTime = 0.0;
	};
	struct FEntry
	{
		FString Category;
		FString Verbosity;
		FString Message;
		int64 Count = 1;
		double FirstPlatformTime = 0.0;
		double LastPlatformTime = 0.0;
		double FirstTime = 0.0;
		double LastTime = 0.0;
	};

	static FString NormalizeMessage(const FString& Message)
	{
		FString Result;
		Result.Reserve(Message.Len());
		for (int32 Index = 0; Index < Message.Len();)
		{
			if (Message[Index] == TEXT('0') && Index + 2 < Message.Len()
				&& (Message[Index + 1] == TEXT('x')
					|| Message[Index + 1] == TEXT('X'))
				&& FChar::IsHexDigit(Message[Index + 2]))
			{
				Result.Append(TEXT("0x#"));
				Index += 3;
				while (Index < Message.Len() && FChar::IsHexDigit(Message[Index]))
					++Index;
				continue;
			}
			if (FChar::IsDigit(Message[Index]))
			{
				Result.AppendChar(TEXT('#'));
				do { ++Index; }
				while (Index < Message.Len() && FChar::IsDigit(Message[Index]));
				continue;
			}
			Result.AppendChar(Message[Index++]);
		}
		return Result;
	}

	static double EventTime(double EventPlatformTime, double CaptureTime,
		double PlatformTime, bool bTimelineTime, bool bHadTimeAnchor,
		double PreviousCaptureTime, double PreviousPlatformTime,
		bool bPreviousTimelineTime)
	{
		if (!bHadTimeAnchor || PlatformTime <= PreviousPlatformTime
			|| EventPlatformTime <= PreviousPlatformTime
			|| bTimelineTime != bPreviousTimelineTime
			|| CaptureTime < PreviousCaptureTime)
		{
			return CaptureTime;
		}
		const double Alpha = FMath::Clamp(
			(EventPlatformTime - PreviousPlatformTime)
				/ (PlatformTime - PreviousPlatformTime), 0.0, 1.0);
		return FMath::Lerp(PreviousCaptureTime, CaptureTime, Alpha);
	}

	static FString VerbosityName(ELogVerbosity::Type Verbosity)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Fatal: return TEXT("Fatal");
		case ELogVerbosity::Error: return TEXT("Error");
		default: return TEXT("Warning");
		}
	}

	static bool IsExcluded(const FName& Category, const FString& Message)
	{
		// EOS is enabled for packaged builds but has no socket subsystem module;
		// the engine probes it during every harness startup before using WinSock.
		if (Category == FName(TEXT("LogSockets"))
			&& Message == TEXT("Unable to load SocketSubsystem module EOS"))
		{
			return true;
		}
		// Breath is absent from a known vertex-animation set and its fallback pose
		// is intentional, so the same warning is emitted on every play-through.
		if (Category == FName(TEXT("LogTemp"))
			&& Message == TEXT("VertexAnim: unknown sequence Breath"))
		{
			return true;
		}
		// The packaged-startup baseline has one unset optional class before any
		// play-through command; match only its exact engine warning text.
		if (Category == FName(TEXT("LogSpawn"))
			&& Message == TEXT("SpawnActor failed because no class was specified"))
		{
			return true;
		}
		// Device-profile/scalability setup intentionally wins the tonemapper cvar
		// priority race and reports that decision on every packaged startup.
		if (Category == FName(TEXT("LogConsoleManager"))
			&& Message.StartsWith(
				TEXT("Setting the console variable 'r.Tonemapper."),
				ESearchCase::IgnoreCase)
			&& Message.Contains(
				TEXT("was ignored as it is lower priority than the previous"),
				ESearchCase::CaseSensitive))
		{
			return true;
		}
		return false;
	}

	static constexpr int32 MaxDistinctEntries = 500;
	const double RegistrationLogTime;
	FCriticalSection PendingMutex;
	TArray<FPendingLine> Pending;
	TArray<FEntry> Entries;
	TMap<FString, int32> EntryIndices;
	int64 Overflow = 0;
	double AnchorCaptureTime = 0.0;
	double AnchorPlatformTime = 0.0;
	bool bHasTimeAnchor = false;
	bool bAnchorTimelineTime = false;
};
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/PlatformMisc.h"
#include "RenderingThread.h"

namespace
{
	AWoTCharacter* FindPawn(UWorld* World, const FString& Name)
	{
		if (!World) return nullptr;
		for (TActorIterator<AWoTCharacter> It(World); It; ++It)
		{
			if (It->GetName() == Name) return *It;
		}
		return nullptr;
	}

	AActor* FindActor(UWorld* World, const FString& Name)
	{
		if (!World) return nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetName() == Name) return *It;
		}
		return nullptr;
	}

	bool ReadStringArray(const FJsonObject& Object, const TCHAR* Field,
		TArray<FString>& Out)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.TryGetArrayField(Field, Values)) return true;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value.IsValid() || !Value->TryGetString(Text)) return false;
			Out.Add(Text);
		}
		return true;
	}

	bool ReadVector(const TArray<TSharedPtr<FJsonValue>>& Values, FVector& Out)
	{
		if (Values.Num() != 3) return false;
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!Values[0]->TryGetNumber(X) || !Values[1]->TryGetNumber(Y)
			|| !Values[2]->TryGetNumber(Z)) return false;
		Out = FVector(X, Y, Z);
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> WriteVector(const FVector& Value)
	{
		return {
			MakeShared<FJsonValueNumber>(Value.X),
			MakeShared<FJsonValueNumber>(Value.Y),
			MakeShared<FJsonValueNumber>(Value.Z)};
	}

	// In-walk route recovery: less than 32 uu of progress along the route in
	// 2 s is a stall (assisted jumps get their tries first: sampled every
	// 0.5 s, throttled to one per second). A re-station lands 200 uu further
	// along the route.
	constexpr float RouteProgressDistance = 32.0f;
	constexpr double RouteStallSeconds = 2.0;
	constexpr float RouteRestationAhead = 200.0f;
	// A map teleporter or mover can displace the walker again right after a
	// re-station (Arena_06: a leg ending in a teleporter trigger re-stationed
	// every tick for 160 s). The off-route check waits out a short grace after
	// each re-station, and three re-stations without on-route progress give
	// the walk up.
	constexpr double RouteRestationGraceSeconds = 0.5;
	constexpr int32 RouteMaxRestations = 3;
	// Route progress is searched only just ahead of the last progress: past
	// it by the lead (at least 32 uu) plus twice what the walker moved since
	// the last tick. A sweep route doubles back through corridors it has
	// already walked, and the old search over the whole remaining route
	// jumped to a later pass: in Arena_01 it skipped from 1.8k to
	// 35k uu of a 41k uu leg, the walk ended, and the walker stood still for
	// the rest of the reel.
	constexpr float RouteTrackSlack = 32.0f;
	// A route walk eases off into a sharp turn, as a player would: at 400 uu/s
	// the walker swung ~20 uu wide turning onto Battle2_03's narrow rooftop
	// ramp (PathNode62) and fell off it in rounds 2 and 3. The route's heading
	// this many leads ahead is compared with its heading here: full speed under
	// 45 degrees, down to 40% at 90 degrees and beyond.
	constexpr float RouteCornerLookLeads = 3.0f;
	constexpr float RouteCornerMinScale = 0.4f;
	// A route walk given a duration keeps to it: a walker ahead of its
	// distance-proportional schedule eases off, from full speed on time down
	// to RouteCornerMinScale at this many seconds ahead, so it runs at most a
	// fraction of it early; on time or behind it walks at full speed. The
	// gate's chapters share a leg's time in proportion to distance; the
	// walkers, faster than the modelled walk, otherwise stood still at the
	// leg's end for up to 7 s. (Walking at the schedule's average speed
	// instead, which already includes the corners the walker also eases
	// into, let a corner-heavy leg fall up to ~1.5 s behind before the
	// correction caught up; a late walker is cut at the next leg.)
	constexpr float RoutePaceSeconds = 0.5f;
	// Per-tick moves longer than this are teleports, not walking.
	constexpr double WalkerTeleportStep = 64.0;
	// How far ahead of a blocked walker its heading is swept for the blocker.
	constexpr float WalkerBlockerProbe = 24.0f;

	// What stops a walker going its way: its capsule (1 uu thinner, so the
	// floor it stands on is not an initial hit) swept along its heading.
	// Lets the gate output tell a ledge, a seam or an actor in the way from a
	// steering stall, where nothing blocks the heading at all.
	bool SweepWalkerHeading(UWorld* World, const AActor* Pawn,
		const FVector& Heading, FHitResult& OutHit)
	{
		const FVector Direction = Heading.GetSafeNormal2D();
		if (!World || !Pawn || Direction.IsNearlyZero()) return false;
		float Radius = 0.0f, HalfHeight = 0.0f;
		Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
		const FVector Start = Pawn->GetActorLocation();
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTWalkerBlocker), false, Pawn);
		return World->SweepSingleByChannel(OutHit, Start,
			Start + Direction * WalkerBlockerProbe, FQuat::Identity, ECC_Pawn,
			FCollisionShape::MakeCapsule(FMath::Max(Radius - 1.0f, 1.0f),
				FMath::Max(HalfHeight - 1.0f, 1.0f)), Params);
	}

	bool SafeTeleportToFloor(UWorld* World, AWoTCharacter* Pawn,
		const FVector& Desired, FVector& OutStation)
	{
		if (!IsValid(World) || !IsValid(Pawn)) return false;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTPlaythroughTourPoint),
			true, Pawn);
		const FVector End = Desired - FVector(0.0, 0.0, 16384.0);
		FHitResult Floor;
		auto FindWalkableFloor = [&](double StartOffset)
		{
			FVector Cursor = Desired + FVector(0.0, 0.0, StartOffset);
			for (int32 Attempt = 0; Attempt < 32 && Cursor.Z > End.Z; ++Attempt)
			{
				FHitResult Hit;
				if (!World->LineTraceSingleByChannel(
					Hit, Cursor, End, ECC_Visibility, Params)) break;
				if (Hit.ImpactNormal.Z > 0.35f)
				{
					Floor = Hit;
					return true;
				}
				Cursor = Hit.ImpactPoint - FVector(0.0, 0.0, 2.0);
			}
			return false;
		};
		if (!FindWalkableFloor(32.0) && !FindWalkableFloor(256.0)) return false;
		OutStation = Floor.ImpactPoint + FVector(0.0, 0.0,
			Pawn->GetSimpleCollisionHalfHeight() + 2.0f);
		return WoTUE1Collision::FindTeleportSpotWorldOnly(
			*World, *Pawn, OutStation, Pawn->GetActorRotation())
			&& WoTNetTest::Teleport(World, Pawn->GetName(), OutStation);
	}

	bool IsStagedWeaveLeftover(const AActor* Actor)
	{
		return IsValid(Actor) && (
			Actor->IsA<AWoTAngrealAMAProjectile>()
			|| Actor->IsA<AWoTAMAPearl>()
			|| Actor->IsA<AWoTExpWardProjectile>()
			|| Actor->IsA<AWoTDistantEyeProjectile>()
			|| Actor->IsA<AWoTAngrealIllusionProjectile>()
			|| Actor->IsA<AWoTLightGlobe>()
			|| Actor->IsA<AWoTEarthTremorProjectile>()
			|| Actor->IsA<AWoTEarthTremor>()
			|| Actor->IsA<AWoTEarthTremorRock>()
			|| Actor->IsA<AWoTEarthTremorChunk>()
			|| Actor->IsA<AWoTETSoundProxy>());
	}

	bool ReadShake(const FJsonObject& Object, float& OutAmplitude,
		float& OutFrequency, int32& OutSeed)
	{
		OutAmplitude = 0.0f;
		OutFrequency = 0.0f;
		OutSeed = 0;
		if (!Object.HasField(TEXT("shake"))) return true;
		const TSharedPtr<FJsonObject>* Shake = nullptr;
		double Amplitude = 0.0, Frequency = 0.0, Seed = 0.0;
		if (!Object.TryGetObjectField(TEXT("shake"), Shake) || !Shake
			|| !Shake->IsValid()
			|| !(*Shake)->TryGetNumberField(TEXT("amp_deg"), Amplitude)
			|| !(*Shake)->TryGetNumberField(TEXT("freq_hz"), Frequency)
			|| !FMath::IsFinite(Amplitude) || Amplitude < 0.0
			|| !FMath::IsFinite(Frequency) || Frequency <= 0.0)
		{
			return false;
		}
		if ((*Shake)->HasField(TEXT("seed"))
			&& (!(*Shake)->TryGetNumberField(TEXT("seed"), Seed)
				|| !FMath::IsFinite(Seed) || Seed != FMath::FloorToDouble(Seed)
				|| Seed < MIN_int32 || Seed > MAX_int32))
		{
			return false;
		}
		OutAmplitude = static_cast<float>(Amplitude);
		OutFrequency = static_cast<float>(Frequency);
		OutSeed = static_cast<int32>(Seed);
		return true;
	}

	FString MovieDumpDirectory()
	{
		// The movie dumper writes MovieFrame*.png beside screenshots
		// (UnrealClient.cpp names them from this exact directory). It already
		// ends in the platform name, so it must not be appended again.
		return FPaths::ConvertRelativePathToFull(
			GetDefault<UEngine>()->GameScreenshotSaveDirectory.Path);
	}

	// The rig swaps in a non-realtime mixer (WoTStereoNonRealtimeAudio.h), which
	// renders one fixed step of audio per tick, in step with the fixed-timestep
	// movie frames. A realtime device would drift from them, so only that mixer
	// is recorded.
	FAudioDevice* CaptureAudioDevice(UWorld* World)
	{
		FAudioDevice* Device = World ? World->GetAudioDeviceRaw() : nullptr;
		return Device && Device->IsNonRealtime() ? Device : nullptr;
	}

	// The non-realtime platform behind that device. Its own per-tick render uses
	// the delta time sampled when the device opened, before -FPS took effect
	// (1/30 s against the game's 1/60 s step), so TickDirector renders each
	// frame's audio itself, as Movie Render Queue does
	// (MoviePipelineAudioRendering.cpp ProcessAudioTick).
	Audio::FMixerPlatformNonRealtime* NonRealtimeMixer(UWorld* World)
	{
		FAudioDevice* Device = CaptureAudioDevice(World);
		Audio::IAudioMixerPlatformInterface* Platform = Device
			? static_cast<Audio::FMixerDevice*>(Device)->GetAudioMixerPlatform() : nullptr;
		return Platform && Platform->IsNonRealtime()
			? static_cast<Audio::FMixerPlatformNonRealtime*>(Platform) : nullptr;
	}

	void SetCaptureAudioCVar(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}

	FString CaptureAudioPath()
	{
		return FPaths::Combine(MovieDumpDirectory(), TEXT("CaptureAudio.wav"));
	}

	// Ends the master-submix recording and writes it beside the frames as
	// 16-bit PCM; returns the seconds written (0 when nothing was recorded).
	double WriteCaptureAudio(FAudioDevice& Device)
	{
		float Channels = 0.0f;
		float SampleRate = 0.0f;
		const Audio::FAlignedFloatBuffer& Samples =
			Device.StopRecording(nullptr, Channels, SampleRate);
		if (Samples.Num() == 0 || Channels < 1.0f || SampleRate < 1.0f)
		{
			return 0.0;
		}
		// 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT = 3): loud scenes sum past
		// full scale, which 16-bit PCM clipped (up to 7% of a citadel take's
		// samples); the edit sets the level instead.
		const uint16 NumChannels = static_cast<uint16>(Channels);
		const uint32 Rate = static_cast<uint32>(SampleRate);
		const uint32 DataBytes = static_cast<uint32>(Samples.Num() * sizeof(float));
		TArray<uint8> Wav;
		auto PutTag = [&Wav](const char* Tag) { Wav.Append(reinterpret_cast<const uint8*>(Tag), 4); };
		auto Put32 = [&Wav](uint32 Value) { Wav.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		auto Put16 = [&Wav](uint16 Value) { Wav.Append(reinterpret_cast<const uint8*>(&Value), 2); };
		PutTag("RIFF");
		Put32(36 + DataBytes);
		PutTag("WAVE");
		PutTag("fmt ");
		Put32(16);
		Put16(3);
		Put16(NumChannels);
		Put32(Rate);
		Put32(Rate * NumChannels * sizeof(float));
		Put16(static_cast<uint16>(NumChannels * sizeof(float)));
		Put16(32);
		PutTag("data");
		Put32(DataBytes);
		Wav.Append(reinterpret_cast<const uint8*>(Samples.GetData()), DataBytes);
		return FFileHelper::SaveArrayToFile(Wav, *CaptureAudioPath())
			? Samples.Num() / (Channels * SampleRate) : 0.0;
	}

	int32 LastMovieFrame()
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files,
			*FPaths::Combine(MovieDumpDirectory(), TEXT("MovieFrame*.png")), true, false);
		int32 Last = -1;
		for (const FString& File : Files)
		{
			FString Digits = FPaths::GetBaseFilename(File);
			Digits.RemoveFromStart(TEXT("MovieFrame"));
			Last = FMath::Max(Last, FCString::Atoi(*Digits));
		}
		return Last;
	}

	FString OneLine(const TSharedRef<FJsonObject>& O)
	{
		FString S;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&S);
		FJsonSerializer::Serialize(O, W);
		return S + TEXT("\n");
	}

	void SendLine(FSocket* Sock, const FString& Line)
	{
		if (!Sock) { return; }
		FTCHARToUTF8 Utf8(*Line);
		// A single Send on the nonblocking socket silently drops the suffix
		// of any response larger than the kernel buffer (snapshots and exec
		// "out" captures get there), leaving the reader waiting on a newline
		// that never comes. Drain with a bounded retry instead; on deadline
		// the connection is torn down so the reader sees EOF, not a stall.
		const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
		const int32 Total = Utf8.Length();
		int32 Offset = 0;
		const double Deadline = FPlatformTime::Seconds() + 2.0;
		while (Offset < Total)
		{
			int32 Sent = 0;
			Sock->Send(Bytes + Offset, Total - Offset, Sent);
			if (Sent > 0)
			{
				Offset += Sent;
				continue;
			}
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Sock->Close();
				return;
			}
			FPlatformProcess::SleepNoStats(0.005f);
		}
	}

	bool IsHarnessEffect(const AActor* Actor)
	{
		if (!IsValid(Actor) || Actor->IsA<APawn>() || Actor->IsA<AController>()
			|| Actor->IsA<AWoTInventory>() || Actor->IsA<AWoTProjectile>()
			|| Actor->IsA<AInfo>() || Actor->IsA<AHUD>()
			|| Actor->IsA<APlayerCameraManager>())
		{
			return false;
		}
		const APawn* Instigator = Actor->GetInstigator();
		const AActor* Owner = Actor->GetOwner();
		return Instigator != nullptr
			|| (Owner && (Owner->IsA<APawn>() || Owner->IsA<AWoTInventory>()));
	}

	bool LooksLikeImpact(const AActor* Actor)
	{
		return IsValid(Actor) && (Actor->IsA<AWoTDecal>()
			|| Actor->IsA<AWoTExplosionEffect>()
			|| Actor->IsA<AWoTWeaveBurst>());
	}

	int32 VisibleTextureFrame(const UPrimitiveComponent* Primitive)
	{
		const UMaterialInterface* Material = Primitive ? Primitive->GetMaterial(0) : nullptr;
		UTexture* Texture = nullptr;
		if (!Material || !Material->GetTextureParameterValue(
			FHashedMaterialParameterInfo(TEXT("Tex")), Texture) || !Texture)
		{
			return INDEX_NONE;
		}
		const FString Name = Texture->GetName();
		int32 DigitStart = Name.Len();
		while (DigitStart > 0 && FChar::IsDigit(Name[DigitStart - 1]))
		{
			--DigitStart;
		}
		return DigitStart < Name.Len()
			? FCString::Atoi(*Name.Mid(DigitStart))
			: static_cast<int32>(GetTypeHash(Texture->GetPathName()));
	}
}

void FWoTNetTestChannel::ObserveProjectileImpact(
	AActor* ProjectileActor, const FHitResult& Hit)
{
	if (!ActiveProbe.IsSet()) return;
	const AWoTAngrealProjectile* Projectile =
		Cast<AWoTAngrealProjectile>(ProjectileActor);
	if (!IsValid(Projectile)) return;
	const APawn* SourcePawn = Projectile->SourcePawn
		? Projectile->SourcePawn.Get() : Projectile->GetInstigator();
	const FString SourceName = GetNameSafe(SourcePawn);
	const FString WeaveName = GetNameSafe(Projectile->SourceAngreal
		? Projectile->SourceAngreal->GetClass() : nullptr);
	const bool bKnownCaster = ActiveProbe->Casters.IsEmpty()
		|| ActiveProbe->Casters.Contains(SourceName);
	const bool bPrimaryContact = bKnownCaster
		&& (ActiveProbe->Weave.IsEmpty()
			|| WeaveName.Equals(ActiveProbe->Weave, ESearchCase::IgnoreCase));
	// Defense windows deliberately cast the named defense on p0, then fire a
	// Dart from p1. The incoming Dart touching the actual defender is valid
	// contact evidence even though its SourceAngreal is not the defense weave.
	const bool bDefenseContact = bKnownCaster
		&& !ActiveProbe->DefenseCaster.IsEmpty()
		&& GetNameSafe(Hit.GetActor()).Equals(
			ActiveProbe->DefenseCaster, ESearchCase::CaseSensitive);
	if (!bPrimaryContact && !bDefenseContact)
	{
		return;
	}
	ActiveProbe->bProjectileOrEffect = true;
	ActiveProbe->EffectEvidence.AddUnique(FString::Printf(
		TEXT("projectile-contact:%s:%s"),
		*Projectile->GetClass()->GetName(), *SourceName));
	ActiveProbe->bImpactOrDecal = true;
	ActiveProbe->ImpactEvidence.AddUnique(FString::Printf(
		TEXT("projectile-contact:%s:%s:%s"),
		*Projectile->GetClass()->GetName(),
		*GetNameSafe(Hit.GetActor()), *SourceName));
}

TMap<FString, int32> FWoTNetTestChannel::AnimationFrames(UWorld* World)
{
	const UWoTTextureAnimationSubsystem* Animation = IsValid(World)
		? World->GetSubsystem<UWoTTextureAnimationSubsystem>() : nullptr;
	TMap<FString, int32> Result = Animation
		? Animation->SnapshotFramesForHarness() : TMap<FString, int32>();
	if (IsValid(World))
	{
		// ShieldParticleMesh.uc:119-126 advances TextureSet at 15 Hz. Some of
		// those five source textures have no AnimNext links, so the shared
		// texture subsystem alone cannot prove the visible shield changed.
		// Read the texture actually bound to each live effect instead.
		for (TActorIterator<AWoTMeshEffect> It(World); It; ++It)
		{
			AWoTMeshEffect* Effect = *It;
			if (!IsValid(Effect)
				|| !Effect->ConfigName.ToString().EndsWith(TEXT("ShieldVisual")))
			{
				continue;
			}
			const FString Element = Effect->ConfigName.ToString().LeftChop(12);
			const UPrimitiveComponent* Primitive = Effect->bParticles
				? static_cast<const UPrimitiveComponent*>(Effect->ParticleQuads.Get())
				: static_cast<const UPrimitiveComponent*>(Effect->Mesh.Get());
			const int32 Frame = VisibleTextureFrame(Primitive);
			if (Frame != INDEX_NONE)
			{
				Result.Add(FString::Printf(TEXT("Angreal.Elemental.%s_A@%s"),
					*Element, *Effect->GetName()), Frame);
			}
		}
		// LightningSkinEffect.uc:71-73 manually cycles LSE_A01..22 on the
		// victim. LB_A01..10 belongs to the separate streak segments.
		for (TActorIterator<AWoTPawnSkinOverlay> It(World); It; ++It)
		{
			AWoTPawnSkinOverlay* Overlay = *It;
			if (!IsValid(Overlay) || Overlay->ConfigName != TEXT("LightningSkinEffect"))
			{
				continue;
			}
			const int32 Frame = VisibleTextureFrame(Overlay->Mesh);
			if (Frame != INDEX_NONE)
			{
				Result.Add(TEXT("Angreal.Effects.LSE_A@") + Overlay->GetName(), Frame);
			}
		}
		// LightningStreak02 authors ten per-render-frame retail skins rather
		// than an AnimNext chain. Fold its real assignment counter into the
		// same harness frame snapshot so the close-up still proves the exact
		// visible texture state advances, rather than accepting another effect.
		for (TActorIterator<AWoTBeamEffect> It(World); It; ++It)
		{
			if (IsValid(*It)
				&& It->Config == FName(TEXT("LightningStreak02"))
				&& It->GetVisibleSegmentCount() > 0)
			{
				Result.Add(TEXT("Angreal.Skins.LB_A@") + It->GetName(),
					It->GetStreakTextureAssignmentCount());
			}
		}
	}
	return Result;
}

FWoTNetTestChannel::~FWoTNetTestChannel()
{
	Stop();
}

void FWoTNetTestChannel::DrainLogWarnings(UWorld* World)
{
	check(IsInGameThread());
	if (!LogDevice || !IsValid(World)) return;
	// GLog hands lines to thread-safe devices on its logging thread; flush the
	// buffered lines synchronously so a drain sees everything logged so far.
	if (GLog) GLog->FlushThreadedLogs();
	const double WorldTime = World->GetTimeSeconds();
	const double CaptureTime = bTimelineRunning
		? FMath::Max(0.0, WorldTime - TimelineStartTime)
		: WorldTime;
	LogDevice->Drain(CaptureTime, bTimelineRunning);
}

void FWoTNetTestChannel::ResetProbes(UWorld* World)
{
	if (LogDevice) LogDevice->Reset();
	ActiveProbe.Reset();
	ProbeWindows.Reset();
	ProbeLateJoins.Reset();
	PendingLateJoin.Reset();
	ProbeAnomalies.Reset();
	ProbeRouteSnaps.Reset();
	ProbeAssistedJumps.Reset();
	ProbeFalls.Reset();
	EndAllFallEpisodes();
	SeekerProbeSamples = 0;
	ProbeEpochTime = IsValid(World) ? World->GetTimeSeconds() : 0.0;
	ScenarioBaselineActors.Reset();
	if (IsValid(World))
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It)) ScenarioBaselineActors.Add(It->GetName());
		}
	}
}

void FWoTNetTestChannel::RecordRouteSnap(UWorld* World,
	const AWoTCharacter* Pawn, const TCHAR* Reason, const FVector& From,
	const FVector& To, double Distance, bool bSnapped)
{
	TSharedRef<FJsonObject> RouteSnap = MakeShared<FJsonObject>();
	const double ProbeTime = ProbeEpochTime >= 0.0 && IsValid(World)
		? FMath::Max(0.0, World->GetTimeSeconds() - ProbeEpochTime) : 0.0;
	RouteSnap->SetNumberField(TEXT("t"), ProbeTime);
	RouteSnap->SetStringField(TEXT("pawn"), Pawn ? Pawn->GetName() : FString());
	RouteSnap->SetStringField(TEXT("reason"), Reason);
	RouteSnap->SetArrayField(TEXT("from"), WriteVector(From));
	RouteSnap->SetArrayField(TEXT("to"), WriteVector(To));
	RouteSnap->SetNumberField(TEXT("distance"), Distance);
	RouteSnap->SetBoolField(TEXT("failed"), !bSnapped);
	ProbeRouteSnaps.Add(MakeShared<FJsonValueObject>(RouteSnap));
}

void FWoTNetTestChannel::EndFallEpisode(AWoTCharacter* Pawn)
{
	if (Pawn)
	{
		const TWeakObjectPtr<AWoTCharacter> WeakPawn(Pawn);
		FallEpisodes.Remove(WeakPawn);
		BelowMinFallLatches.Remove(WeakPawn);
	}
}

void FWoTNetTestChannel::EndAllFallEpisodes()
{
	FallEpisodes.Reset();
	BelowMinFallLatches.Reset();
}

void FWoTNetTestChannel::TickFallWatch(UWorld* World)
{
	if (!bFallWatchEnabled || !IsValid(World)) return;
	TSet<TWeakObjectPtr<AWoTCharacter>> LocalPawns;
	if (UGameInstance* GameInstance = World->GetGameInstance();
		GameInstance && GameInstance->GetNumLocalPlayers() > 0)
	{
		for (int32 Index = 0; Index < GameInstance->GetNumLocalPlayers(); ++Index)
		{
			if (AWoTCharacter* Pawn = WoTNetTest::LocalPlayerPawn(World, Index))
				LocalPawns.Add(Pawn);
		}
	}
	else if (AWoTCharacter* Pawn = WoTNetTest::LocalPlayerPawn(World, 0))
	{
		LocalPawns.Add(Pawn);
	}
	for (auto It = FallEpisodes.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || !LocalPawns.Contains(It.Key())) It.RemoveCurrent();
	}
	for (auto It = BelowMinFallLatches.CreateIterator(); It; ++It)
	{
		if (!(*It).IsValid() || !LocalPawns.Contains(*It)) It.RemoveCurrent();
	}

	const double Now = World->GetTimeSeconds();
	const double ProbeTime = ProbeEpochTime >= 0.0
		? FMath::Max(0.0, Now - ProbeEpochTime) : 0.0;
	for (const TWeakObjectPtr<AWoTCharacter>& WeakPawn : LocalPawns)
	{
		AWoTCharacter* Pawn = WeakPawn.Get();
		if (!Pawn) continue;
		const FVector Location = Pawn->GetActorLocation();
		const UCharacterMovementComponent* Movement = Pawn->GetCharacterMovement();
		const bool bFalling = Movement && Movement->IsFalling();
		const bool bBelowMinZ = Location.Z < FallWatchMinZ;
		if (!bBelowMinZ) BelowMinFallLatches.Remove(WeakPawn);
		FFallEpisode* Episode = FallEpisodes.Find(WeakPawn);
		if (!Episode && (bFalling
			|| (bBelowMinZ && !BelowMinFallLatches.Contains(WeakPawn))))
		{
			FFallEpisode NewEpisode;
			NewEpisode.Start = Location;
			NewEpisode.StartTime = Now;
			Episode = &FallEpisodes.Add(WeakPawn, MoveTemp(NewEpisode));
		}
		if (!Episode) continue;

		const double Duration = FMath::Max(0.0, Now - Episode->StartTime);
		if (!Episode->Record.IsValid()
			&& (bBelowMinZ || (bFalling && Duration > FallWatchSeconds)))
		{
			Episode->Record = MakeShared<FJsonObject>();
			Episode->Record->SetNumberField(TEXT("t"), ProbeTime);
			Episode->Record->SetStringField(TEXT("pawn"), Pawn->GetName());
			Episode->Record->SetArrayField(TEXT("start"), WriteVector(Episode->Start));
			Episode->Record->SetBoolField(TEXT("below_min_z"), bBelowMinZ);
			if (bBelowMinZ) BelowMinFallLatches.Add(WeakPawn);
			ProbeFalls.Add(MakeShared<FJsonValueObject>(Episode->Record.ToSharedRef()));
		}
		if (Episode->Record.IsValid())
		{
			bool bWasBelowMinZ = false;
			Episode->Record->TryGetBoolField(TEXT("below_min_z"), bWasBelowMinZ);
			Episode->Record->SetArrayField(TEXT("location"), WriteVector(Location));
			Episode->Record->SetNumberField(TEXT("duration"), Duration);
			Episode->Record->SetBoolField(TEXT("below_min_z"),
				bBelowMinZ || bWasBelowMinZ);
			if (bBelowMinZ) BelowMinFallLatches.Add(WeakPawn);
		}
		if (!bFalling) FallEpisodes.Remove(WeakPawn);
	}
}

bool FWoTNetTestChannel::BeginProbe(UWorld* World, const FJsonObject& Request)
{
	FString Label;
	if (!IsValid(World) || ActiveProbe.IsSet()
		|| !Request.TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		return false;
	}
	FProbeWindow Window;
	Window.Label = Label;
	Request.TryGetStringField(TEXT("kind"), Window.Kind);
	Request.TryGetStringField(TEXT("weave"), Window.Weave);
	Request.TryGetStringField(TEXT("view"), Window.View);
	Request.TryGetStringField(TEXT("defense_caster"), Window.DefenseCaster);
	if (!ReadStringArray(Request, TEXT("casters"), Window.Casters)
		|| !ReadStringArray(Request, TEXT("animation_targets"),
			Window.AnimationTargets))
	{
		return false;
	}
	FString Caster;
	if (Window.Casters.IsEmpty()
		&& Request.TryGetStringField(TEXT("caster"), Caster) && !Caster.IsEmpty())
	{
		Window.Casters.Add(Caster);
	}
	Window.StartTime = FMath::Max(0.0, World->GetTimeSeconds() - ProbeEpochTime);
	const TMap<FString, int32> Frames = AnimationFrames(World);
	for (const TPair<FString, int32>& Pair : Frames)
	{
		if (Window.AnimationTargets.IsEmpty()
			|| Window.AnimationTargets.ContainsByPredicate(
				[&Pair](const FString& Target)
				{
					return Pair.Key.Contains(Target, ESearchCase::IgnoreCase);
				}))
		{
			Window.AnimationStart.Add(Pair.Key, Pair.Value);
			Window.AnimationLast.Add(Pair.Key, Pair.Value);
		}
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!IsValid(*It)) continue;
		Window.BaselineActors.Add(It->GetName());
		TInlineComponentArray<UActorComponent*> Components(*It);
		for (const UActorComponent* Component : Components)
		{
			if (IsValid(Component))
				Window.BaselineComponents.Add(Component->GetPathName());
		}
	}
	if (const AWoTCharacter* Walker = WoTNetTest::LocalPlayerPawn(World, 0))
	{
		Window.WalkerSample = Walker->GetActorLocation();
		Window.bHasWalkerSample = true;
	}
	for (const FString& CasterName : Window.Casters)
	{
		if (const AWoTCharacter* CasterPawn = FindPawn(World, CasterName))
		{
			Window.LowestCasterHealth.Add(CasterName, CasterPawn->Health);
			Window.LastCasterLocations.Add(CasterName, CasterPawn->GetActorLocation());
		}
	}
	ActiveProbe = MoveTemp(Window);
	UE_LOG(LogTemp, Display, TEXT("WOT_PLAYTHROUGH_TIMESTAMP %.3f %s"),
		ActiveProbe->StartTime, *ActiveProbe->Label);
	return true;
}

bool FWoTNetTestChannel::EndProbe(UWorld* World, const FString& Label)
{
	if (!IsValid(World) || !ActiveProbe.IsSet()
		|| ActiveProbe->Label != Label) return false;
	ActiveProbe->EndTime = FMath::Max(0.0,
		World->GetTimeSeconds() - ProbeEpochTime);
	const TMap<FString, int32> Frames = AnimationFrames(World);
	for (const TPair<FString, int32>& Pair : Frames)
	{
		if (ActiveProbe->AnimationTargets.IsEmpty()
			|| ActiveProbe->AnimationTargets.ContainsByPredicate(
				[&Pair](const FString& Target)
				{
					return Pair.Key.Contains(Target, ESearchCase::IgnoreCase);
				}))
		{
			ActiveProbe->AnimationEnd.Add(Pair.Key, Pair.Value);
		}
	}
	ProbeWindows.Add(MoveTemp(ActiveProbe.GetValue()));
	ActiveProbe.Reset();
	return true;
}

void FWoTNetTestChannel::TickProbes(UWorld* World)
{
	if (!IsValid(World) || ProbeEpochTime < 0.0) return;
	if (ActiveProbe.IsSet())
	{
		if (const AWoTCharacter* Walker = WoTNetTest::LocalPlayerPawn(World, 0))
		{
			const FVector WalkerLocation = Walker->GetActorLocation();
			if (ActiveProbe->bHasWalkerSample)
			{
				const double Step = FVector::Dist2D(WalkerLocation, ActiveProbe->WalkerSample);
				if (Step <= WalkerTeleportStep) ActiveProbe->WalkedXY += Step;
			}
			ActiveProbe->WalkerSample = WalkerLocation;
			ActiveProbe->bHasWalkerSample = true;
		}
		const TMap<FString, int32> CurrentFrames = AnimationFrames(World);
		for (const TPair<FString, int32>& Pair : CurrentFrames)
		{
			const bool bExpectedAnimation = ActiveProbe->AnimationTargets.IsEmpty()
				|| ActiveProbe->AnimationTargets.ContainsByPredicate(
					[&Pair](const FString& Target)
					{
						return Pair.Key.Contains(Target, ESearchCase::IgnoreCase);
					});
			if (bExpectedAnimation && !ActiveProbe->AnimationStart.Contains(Pair.Key))
			{
				ActiveProbe->AnimationStart.Add(Pair.Key, Pair.Value);
				ActiveProbe->AnimationLast.Add(Pair.Key, Pair.Value);
			}
			if (bExpectedAnimation)
			{
				if (const int32* Last = ActiveProbe->AnimationLast.Find(Pair.Key);
					Last && *Last != Pair.Value)
				{
					ActiveProbe->AnimationAdvances.FindOrAdd(Pair.Key)++;
				}
				ActiveProbe->AnimationLast.Add(Pair.Key, Pair.Value);
				ActiveProbe->AnimationEnd.Add(Pair.Key, Pair.Value);
			}
		}
		// Two passes allow an effect encountered before its newly spawned owner to
		// join the same causal lineage in this tick.
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor)
					|| ActiveProbe->BaselineActors.Contains(Actor->GetName()))
				{
					continue;
				}
				bool bAttributed = ActiveProbe->Casters.IsEmpty();
				for (const AActor* Link = Actor; Link && !bAttributed;
					Link = Link->GetOwner())
				{
					bAttributed = ActiveProbe->Casters.Contains(Link->GetName())
						|| ActiveProbe->LineageActors.Contains(Link->GetName())
						|| (!ActiveProbe->Weave.IsEmpty()
							&& Link->GetClass()->GetName().Equals(
								ActiveProbe->Weave, ESearchCase::IgnoreCase));
				}
				if (!bAttributed && Actor->GetInstigator())
				{
					bAttributed = ActiveProbe->Casters.Contains(
						Actor->GetInstigator()->GetName());
				}
				bool bSpecificImpact = false;
				const FString& Weave = ActiveProbe->Weave;
				if (const AWoTAMAPearl* Pearl = Cast<AWoTAMAPearl>(Actor))
				{
					const APawn* Source = Pearl->SourcePawn
						? Pearl->SourcePawn.Get() : Pearl->GetInstigator();
					bSpecificImpact = Weave == TEXT("WoTAngrealInvAMA")
						&& ActiveProbe->Casters.Contains(GetNameSafe(Source))
						&& Pearl->SourceAngreal
						&& Pearl->SourceAngreal->GetClass()->GetName().Equals(
							Weave, ESearchCase::IgnoreCase);
				}
				if (const AWoTDecal* Decal = Cast<AWoTDecal>(Actor);
					Decal && Weave == TEXT("WoTAngrealInvBalefire")
					&& Decal->DecalType == TEXT("BalefireDecal"))
				{
					for (TActorIterator<AWoTBeamEffect> BeamIt(World); BeamIt; ++BeamIt)
					{
						const AWoTBeamEffect* Beam = *BeamIt;
						if (!IsValid(Beam) || Beam->Config != TEXT("BalefireLine")
							|| !ActiveProbe->Casters.Contains(
								GetNameSafe(Beam->GetInstigator())))
						{
							continue;
						}
						const FVector Closest = FMath::ClosestPointOnSegment(
							Decal->GetActorLocation(), Beam->Start, Beam->End);
						if (FVector::DistSquared(Closest, Decal->GetActorLocation())
							<= FMath::Square(64.0f))
						{
							bSpecificImpact = true;
							break;
						}
					}
				}
				auto NearCastLineage = [&]()
				{
					for (const TPair<FString, FVector>& Pair
						: ActiveProbe->LineageLocations)
					{
						if (FVector::DistSquared(Pair.Value, Actor->GetActorLocation())
							<= FMath::Square(192.0f)) return true;
					}
					return false;
				};
				if (const AWoTExplosionEffect* Explosion =
					Cast<AWoTExplosionEffect>(Actor))
				{
					bSpecificImpact = bSpecificImpact
						|| (Weave == TEXT("WoTAngrealInvExpWard")
							&& Explosion->ConfigName == TEXT("FireballExplode3")
							&& NearCastLineage());
				}
				if (const AWoTParticleSprayer* Sprayer =
					Cast<AWoTParticleSprayer>(Actor))
				{
					const bool bFireworkBurst = Sprayer->ConfigName == TEXT("Firework05")
						|| Sprayer->ConfigName == TEXT("Firework07")
						|| Sprayer->ConfigName == TEXT("Firework08")
						|| Sprayer->ConfigName == TEXT("Firework09");
					bSpecificImpact = bSpecificImpact
						|| (Weave == TEXT("WoTAngrealInvFireworks")
							&& bFireworkBurst && NearCastLineage());
				}
				if (!bAttributed && !bSpecificImpact) continue;
				ActiveProbe->LineageActors.Add(Actor->GetName());
				ActiveProbe->LineageLocations.Add(
					Actor->GetName(), Actor->GetActorLocation());
				if (Actor->IsA<AWoTProjectile>() || IsHarnessEffect(Actor))
				{
					ActiveProbe->bProjectileOrEffect = true;
					ActiveProbe->EffectEvidence.AddUnique(
						Actor->GetClass()->GetName() + TEXT(":") + Actor->GetName());
				}
				if ((bAttributed && LooksLikeImpact(Actor)) || bSpecificImpact)
				{
					ActiveProbe->bImpactOrDecal = true;
					ActiveProbe->ImpactEvidence.AddUnique(
						Actor->GetClass()->GetName() + TEXT(":") + Actor->GetName());
				}
			}
		}
		for (const FString& CasterName : ActiveProbe->Casters)
		{
			AWoTCharacter* Caster = FindPawn(World, CasterName);
			if (!IsValid(Caster)) continue;
			const FVector PreviousLocation = ActiveProbe->LastCasterLocations.FindRef(
				CasterName);
			int32& LowestHealth = ActiveProbe->LowestCasterHealth.FindOrAdd(
				CasterName, Caster->Health);
			if (ActiveProbe->Weave == TEXT("WoTAngrealInvHeal")
				&& Caster->Health > LowestHealth)
			{
				ActiveProbe->bProjectileOrEffect = true;
				ActiveProbe->EffectEvidence.AddUnique(
					TEXT("state:health-increased:") + CasterName);
			}
			LowestHealth = FMath::Min(LowestHealth, Caster->Health);

			if (Caster->InventoryComponent)
			{
				for (AWoTInventory* Item : Caster->InventoryComponent->Items)
				{
					if (!IsValid(Item)) continue;
					const FString ItemClass = Item->GetClass()->GetName();
					const bool bLightAlias = ActiveProbe->Weave.StartsWith(
						TEXT("WoTAngrealInvLG"))
						&& Item->IsA<AWoTAngrealInvLightGlobe>();
					if (!ItemClass.Equals(ActiveProbe->Weave,
						ESearchCase::IgnoreCase) && !bLightAlias) continue;

					if (const AWoTResourceSpawnerInv* Spawner =
						Cast<AWoTResourceSpawnerInv>(Item))
					{
						if (const AWoTNPCCharacter* Helper =
							Spawner->CurrentArenaHelper.Get())
						{
							if (!ActiveProbe->BaselineActors.Contains(Helper->GetName()))
							{
								ActiveProbe->bProjectileOrEffect = true;
								ActiveProbe->EffectEvidence.AddUnique(
									Helper->GetClass()->GetName() + TEXT(":")
									+ Helper->GetName());
							}
						}
					}
					if (const AWoTAngrealInvGuardian* Guardian =
						Cast<AWoTAngrealInvGuardian>(Item))
					{
						if (const AWoTMashadarGuide* Guide = Guardian->CurrentGuide.Get();
							Guide && !ActiveProbe->BaselineActors.Contains(Guide->GetName()))
						{
							ActiveProbe->bProjectileOrEffect = true;
							ActiveProbe->EffectEvidence.AddUnique(
								Guide->GetClass()->GetName() + TEXT(":") + Guide->GetName());
						}
					}
					if (const AWoTAngrealInvLightGlobe* GlobeItem =
						Cast<AWoTAngrealInvLightGlobe>(Item))
					{
						const FString ExpectedColor = ActiveProbe->Weave.Mid(15);
						if (const AWoTLightGlobe* Globe = GlobeItem->Light;
							IsValid(Globe) && Globe->FollowActor == Caster
							&& !ActiveProbe->BaselineActors.Contains(Globe->GetName())
							&& (!bLightAlias || Globe->Color.ToString() == ExpectedColor))
						{
							ActiveProbe->bProjectileOrEffect = true;
							ActiveProbe->EffectEvidence.AddUnique(
								Globe->GetClass()->GetName() + TEXT(":") + Globe->GetName());
						}
					}
					if (Item->IsA<AWoTAngrealInvDisguise>() && Caster->IsDisguised())
					{
						ActiveProbe->bProjectileOrEffect = true;
						ActiveProbe->EffectEvidence.AddUnique(
							TEXT("state:disguised:") + CasterName);
					}
					if (Item->IsA<AWoTAngrealInvShift>()
						&& FVector::DistSquared(PreviousLocation,
							Caster->GetActorLocation()) >= FMath::Square(80.0f))
					{
						bool bHasArrivalEffect = false;
						for (TActorIterator<AWoTAppearEffect> It(World); It; ++It)
						{
							bHasArrivalEffect = IsValid(*It)
								&& !ActiveProbe->BaselineActors.Contains(It->GetName())
								&& It->AppearTarget == Caster;
							if (bHasArrivalEffect) break;
						}
						if (bHasArrivalEffect)
						{
							ActiveProbe->bProjectileOrEffect = true;
							ActiveProbe->EffectEvidence.AddUnique(
								TEXT("state:shifted:") + CasterName);
						}
					}
					if (Item->IsA<AWoTAngrealInvLevitate>()
						&& Caster->GetCharacterMovement()
						&& Caster->GetCharacterMovement()->MovementMode == MOVE_Flying)
					{
						ActiveProbe->bProjectileOrEffect = true;
						ActiveProbe->EffectEvidence.AddUnique(
							TEXT("state:levitating:") + CasterName);
					}
				}
			}
			ActiveProbe->LastCasterLocations.Add(CasterName,
				Caster->GetActorLocation());
		}
		// Components can be installed on a victim rather than the caster
		// (Whirlwind). Attribute them through their producing angreal.
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Host = *It;
			if (!IsValid(Host)) continue;
			TInlineComponentArray<UActorComponent*> Components(Host);
			for (UActorComponent* Component : Components)
			{
				if (!IsValid(Component)
					|| ActiveProbe->BaselineComponents.Contains(Component->GetPathName()))
				{
					continue;
				}
				AWoTAngrealInventory* Source = nullptr;
				if (const UWoTLeech* Leech = Cast<UWoTLeech>(Component))
					Source = Leech->SourceAngreal;
				else if (const UWoTReflector* Reflector = Cast<UWoTReflector>(Component))
					Source = Reflector->SourceAngreal;
				else continue;
				const bool bSourceMatches = IsValid(Source)
					&& (ActiveProbe->Weave.IsEmpty()
						|| Source->GetClass()->GetName().Equals(
							ActiveProbe->Weave, ESearchCase::IgnoreCase))
					&& (ActiveProbe->Casters.IsEmpty()
						|| ActiveProbe->Casters.Contains(GetNameSafe(Source->GetOwner())));
				if (!bSourceMatches) continue;
				ActiveProbe->bProjectileOrEffect = true;
				ActiveProbe->EffectEvidence.AddUnique(
					Component->GetClass()->GetName() + TEXT(":")
					+ Component->GetPathName());
				if (!ActiveProbe->DefenseCaster.IsEmpty()
					&& Host->GetName() == ActiveProbe->DefenseCaster)
				{
					ActiveProbe->bDefenseEffect = true;
				}
			}
		}
	}
	for (TActorIterator<AWoTSeekingProjectile> It(World); It; ++It)
	{
		AWoTSeekingProjectile* Seeker = *It;
		if (!IsValid(Seeker) || !Seeker->bSeeking) continue;
		++SeekerProbeSamples;
		FHitResult Hit;
		const USphereComponent* Sphere = Seeker->GetProjectilePresentationRoot();
		const float Radius = Sphere
			? Sphere->GetScaledSphereRadius()
			: FMath::Max(1.0f, Seeker->GetSimpleCollisionRadius());
		if (!WoTVisBlockingSweep(World, Seeker->GetActorLocation(),
			Seeker->GetActorLocation(), FQuat::Identity,
			FCollisionShape::MakeSphere(Radius), Seeker, Hit))
		{
			continue;
		}
		TSharedRef<FJsonObject> Anomaly = MakeShared<FJsonObject>();
		Anomaly->SetStringField(TEXT("type"), TEXT("seeker_solid_overlap"));
		Anomaly->SetNumberField(TEXT("time"), FMath::Max(0.0,
			World->GetTimeSeconds() - ProbeEpochTime));
		Anomaly->SetStringField(TEXT("actor"), Seeker->GetName());
		const FString Detail = Hit.GetActor() ? Hit.GetActor()->GetName()
			: (Hit.GetComponent() ? Hit.GetComponent()->GetName() : TEXT("BSP"));
		Anomaly->SetStringField(TEXT("detail"), Detail);
		ProbeAnomalies.Add(MakeShared<FJsonValueObject>(Anomaly));
	}
}

TSharedPtr<FJsonObject> FWoTNetTestChannel::BuildProbeResults(UWorld* World) const
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	bool bMaterialized = false;
	if (IsValid(World))
	{
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			if (IsValid(*It)) { bMaterialized = true; break; }
		}
	}
	Result->SetBoolField(TEXT("materialized"), bMaterialized);
	TArray<TSharedPtr<FJsonValue>> Windows;
	auto FrameObject = [](const TMap<FString, int32>& Frames)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : Frames)
			Object->SetNumberField(Pair.Key, Pair.Value);
		return Object;
	};
	for (const FProbeWindow& Window : ProbeWindows)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("label"), Window.Label);
		Entry->SetStringField(TEXT("kind"), Window.Kind);
		Entry->SetStringField(TEXT("weave"), Window.Weave);
		Entry->SetStringField(TEXT("view"), Window.View);
		TArray<TSharedPtr<FJsonValue>> Casters;
		for (const FString& Caster : Window.Casters)
			Casters.Add(MakeShared<FJsonValueString>(Caster));
		Entry->SetArrayField(TEXT("casters"), Casters);
		TArray<TSharedPtr<FJsonValue>> AnimationTargets;
		for (const FString& Target : Window.AnimationTargets)
			AnimationTargets.Add(MakeShared<FJsonValueString>(Target));
		Entry->SetArrayField(TEXT("animation_targets"), AnimationTargets);
		Entry->SetNumberField(TEXT("start"), Window.StartTime);
		Entry->SetNumberField(TEXT("end"), Window.EndTime);
		Entry->SetBoolField(TEXT("projectile_or_effect"),
			Window.bProjectileOrEffect);
		Entry->SetBoolField(TEXT("impact_or_decal"), Window.bImpactOrDecal);
		Entry->SetBoolField(TEXT("defense_effect"), Window.bDefenseEffect);
		Entry->SetObjectField(TEXT("animation_start"),
			FrameObject(Window.AnimationStart));
		Entry->SetObjectField(TEXT("animation_end"),
			FrameObject(Window.AnimationEnd));
		Entry->SetObjectField(TEXT("animation_advances"),
			FrameObject(Window.AnimationAdvances));
		auto StringValues = [](const TArray<FString>& Strings)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& String : Strings)
				Values.Add(MakeShared<FJsonValueString>(String));
			return Values;
		};
		Entry->SetArrayField(TEXT("effect_evidence"),
			StringValues(Window.EffectEvidence));
		Entry->SetArrayField(TEXT("impact_evidence"),
			StringValues(Window.ImpactEvidence));
		Entry->SetNumberField(TEXT("walked_xy"), Window.WalkedXY);
		Windows.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Result->SetArrayField(TEXT("windows"), Windows);
	Result->SetArrayField(TEXT("late_joins"), ProbeLateJoins);
	Result->SetArrayField(TEXT("anomalies"), ProbeAnomalies);
	Result->SetArrayField(TEXT("route_snaps"), ProbeRouteSnaps);
	Result->SetArrayField(TEXT("assisted_jumps"), ProbeAssistedJumps);
	Result->SetArrayField(TEXT("falls"), ProbeFalls);
	Result->SetArrayField(TEXT("log_warnings"),
		LogDevice ? LogDevice->JsonEntries()
			: TArray<TSharedPtr<FJsonValue>>());
	Result->SetNumberField(TEXT("log_overflow"),
		LogDevice ? static_cast<double>(LogDevice->GetOverflow()) : 0.0);
	Result->SetNumberField(TEXT("seeker_samples"), SeekerProbeSamples);
	return Result;
}

bool FWoTNetTestChannel::AddLateJoin(UWorld* World, const FString& Name,
	const FString& Faction, bool bRequireDisguise,
	TArray<TSharedPtr<FJsonValue>>& OutPuppets)
{
	UGameInstance* GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
	if (!GameInstance || World->GetNetMode() == NM_Client
		|| PendingLateJoin.IsSet()) return false;
	auto ReadState = [](const AWoTCharacter& Pawn)
	{
		FLateJoinPawnState State;
		State.Location = Pawn.GetActorLocation();
		State.Class = Pawn.GetClass()->GetName();
		State.PersonalSkin = Pawn.PersonalSkin.ToString();
		State.bDisguised = Pawn.IsDisguised();
		State.DisguiseClass = GetNameSafe(Pawn.Disguise.SourceClass.Get());
		State.DisguiseSkin = Pawn.Disguise.SourcePersonalSkin.ToString();
		return State;
	};
	TMap<FString, FLateJoinPawnState> Expected;
	bool bExpectedDisguise = false;
	bool bExpectedDistinctDisguise = false;
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (!IsValid(*It)) continue;
		FLateJoinPawnState State = ReadState(**It);
		bExpectedDisguise |= State.bDisguised;
		bExpectedDistinctDisguise |= State.bDisguised
			&& (State.DisguiseClass != State.Class
				|| State.DisguiseSkin != State.PersonalSkin);
		Expected.Add(It->GetName(), MoveTemp(State));
	}
	const int32 NewPuppetCount = GameInstance->GetNumLocalPlayers();
	TArray<FString> Names, Factions;
	Names.SetNum(NewPuppetCount);
	Factions.SetNum(NewPuppetCount);
	for (int32 Index = 0; Index < NewPuppetCount; ++Index)
	{
		Names[Index] = FString::Printf(TEXT("Puppet%d"), Index + 1);
		Factions[Index] = TEXT("AesSedai");
	}
	Names.Last() = Name.IsEmpty() ? TEXT("LatePuppet") : Name;
	Factions.Last() = Faction.IsEmpty() ? TEXT("AesSedai") : Faction;
	const bool bCreated = WoTNetTest::CreatePuppets(
		World, NewPuppetCount, Names, Factions, OutPuppets);
	if (!bCreated) return false;
	FLateJoinProbe Pending;
	Pending.Expected = MoveTemp(Expected);
	Pending.CreatedTime = World->GetTimeSeconds();
	Pending.PlayerIndex = NewPuppetCount;
	Pending.bRequireDisguise = bRequireDisguise;
	Pending.bExpectedDisguise = bExpectedDisguise;
	Pending.bExpectedDistinctDisguise = bExpectedDistinctDisguise;
	PendingLateJoin = MoveTemp(Pending);
	return true;
}

bool FWoTNetTestChannel::VerifyLateJoin(UWorld* World)
{
	if (!IsValid(World) || !PendingLateJoin.IsSet()) return false;
	const FLateJoinProbe& Pending = PendingLateJoin.GetValue();
	const double Delay = World->GetTimeSeconds() - Pending.CreatedTime;
	if (Delay < 0.5) return false;
	auto ReadState = [](const AWoTCharacter& Pawn)
	{
		FLateJoinPawnState State;
		State.Location = Pawn.GetActorLocation();
		State.Class = Pawn.GetClass()->GetName();
		State.PersonalSkin = Pawn.PersonalSkin.ToString();
		State.bDisguised = Pawn.IsDisguised();
		State.DisguiseClass = GetNameSafe(Pawn.Disguise.SourceClass.Get());
		State.DisguiseSkin = Pawn.Disguise.SourcePersonalSkin.ToString();
		return State;
	};
	auto JsonState = [](const FString& PawnName, const FLateJoinPawnState& State)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("pawn"), PawnName);
		Json->SetNumberField(TEXT("x"), State.Location.X);
		Json->SetNumberField(TEXT("y"), State.Location.Y);
		Json->SetNumberField(TEXT("z"), State.Location.Z);
		Json->SetStringField(TEXT("class"), State.Class);
		Json->SetStringField(TEXT("personal_skin"), State.PersonalSkin);
		Json->SetBoolField(TEXT("disguised"), State.bDisguised);
		Json->SetStringField(TEXT("disguise_class"), State.DisguiseClass);
		Json->SetStringField(TEXT("disguise_skin"), State.DisguiseSkin);
		return Json;
	};
	APlayerController* LateController = WoTNetTest::LocalPlayerController(
		World, Pending.PlayerIndex);
	AWoTCharacter* LatePawn = WoTNetTest::LocalPlayerPawn(
		World, Pending.PlayerIndex);
	TSharedRef<FJsonObject> Join = MakeShared<FJsonObject>();
	Join->SetNumberField(TEXT("time"), FMath::Max(0.0,
		World->GetTimeSeconds() - ProbeEpochTime));
	Join->SetNumberField(TEXT("verification_delay"), Delay);
	Join->SetStringField(TEXT("mode"), TEXT("local_player_fallback"));
	Join->SetStringField(TEXT("scope"),
		TEXT("shared-world persistence and local-controller presentation; not network replication"));
	Join->SetStringField(TEXT("pawn"), GetNameSafe(LatePawn));
	Join->SetStringField(TEXT("controller"), GetNameSafe(LateController));
	Join->SetStringField(TEXT("view_target"),
		GetNameSafe(LateController ? LateController->GetViewTarget() : nullptr));
	Join->SetBoolField(TEXT("require_disguise"), Pending.bRequireDisguise);
	TArray<TSharedPtr<FJsonValue>> ExpectedJson, ObservedJson;
	for (const TPair<FString, FLateJoinPawnState>& Pair : Pending.Expected)
		ExpectedJson.Add(MakeShared<FJsonValueObject>(
			JsonState(Pair.Key, Pair.Value)));
	bool bStateComplete = IsValid(LateController) && IsValid(LatePawn)
		&& IsValid(WoTNetTest::LocalPlayerPawn(World, 0))
		&& (!Pending.bRequireDisguise
			|| (Pending.bExpectedDisguise
				&& Pending.bExpectedDistinctDisguise));
	bool bPresentationComplete = IsValid(LateController)
		&& IsValid(LateController->PlayerCameraManager)
		&& IsValid(LateController->GetViewTarget());
	TSet<FString> Matched;
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		AWoTCharacter* Pawn = *It;
		if (!IsValid(Pawn)) continue;
		const FLateJoinPawnState Observed = ReadState(*Pawn);
		ObservedJson.Add(MakeShared<FJsonValueObject>(
			JsonState(Pawn->GetName(), Observed)));
		if (const FLateJoinPawnState* Wanted = Pending.Expected.Find(Pawn->GetName()))
		{
			Matched.Add(Pawn->GetName());
			bStateComplete = bStateComplete
				&& !Observed.Location.ContainsNaN()
				&& Observed.Location.Equals(Wanted->Location, 1.0f)
				&& Observed.Class == Wanted->Class
				&& Observed.PersonalSkin == Wanted->PersonalSkin
				&& Observed.bDisguised == Wanted->bDisguised
				&& Observed.DisguiseClass == Wanted->DisguiseClass
				&& Observed.DisguiseSkin == Wanted->DisguiseSkin;
			bPresentationComplete = bPresentationComplete
				&& IsValid(Pawn->GetMesh()) && Pawn->GetMesh()->IsRegistered();
		}
	}
	bStateComplete = bStateComplete
		&& Matched.Num() == Pending.Expected.Num();
	Join->SetArrayField(TEXT("expected_host_state"), ExpectedJson);
	Join->SetArrayField(TEXT("observed_late_view"), ObservedJson);
	Join->SetBoolField(TEXT("presentation_complete"), bPresentationComplete);
	Join->SetBoolField(TEXT("converged"),
		bStateComplete && bPresentationComplete);
	ProbeLateJoins.Add(MakeShared<FJsonValueObject>(Join));
	PendingLateJoin.Reset();
	return bStateComplete && bPresentationComplete;
}

bool FWoTNetTestChannel::Start(uint16 Port, const FString& BindAddr, const FString& InToken)
{
	Token = InToken;
	if (!LogDevice)
	{
		LogDevice = new FWoTNetTestLogDevice();
		if (GLog) GLog->AddOutputDevice(LogDevice);
	}
	FIPv4Address Addr = FIPv4Address::Any;
	FIPv4Address::Parse(BindAddr, Addr);
	ListenSocket = FTcpSocketBuilder(TEXT("WoTNetTestChannel"))
		.AsReusable()
		.AsNonBlocking()
		.BoundToEndpoint(FIPv4Endpoint(Addr, Port))
		.Listening(8)
		.Build();
	if (!ListenSocket)
	{
		if (GLog && LogDevice) GLog->RemoveOutputDevice(LogDevice);
		delete LogDevice;
		LogDevice = nullptr;
		return false;
	}
	if (!ProjectileImpactHandle.IsValid())
	{
		ProjectileImpactHandle = AWoTGenericProjectile::OnImpactProbe.AddRaw(
			this, &FWoTNetTestChannel::ObserveProjectileImpact);
	}
	BoundPort = (uint16)ListenSocket->GetPortNo();
	return true;
}

void FWoTNetTestChannel::CloseSocket(FSocket*& Socket)
{
	if (Socket)
	{
		Socket->Close();
		if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SS->DestroySocket(Socket);
		}
		Socket = nullptr;
	}
}

void FWoTNetTestChannel::Stop()
{
	if (GLog && LogDevice) GLog->RemoveOutputDevice(LogDevice);
	delete LogDevice;
	LogDevice = nullptr;
	if (ProjectileImpactHandle.IsValid())
	{
		AWoTGenericProjectile::OnImpactProbe.Remove(ProjectileImpactHandle);
		ProjectileImpactHandle.Reset();
	}
	for (FConn& C : Conns)
	{
		CloseSocket(C.Socket);
	}
	Conns.Reset();
	CloseSocket(ListenSocket);
	BoundPort = 0;
}

void FWoTNetTestChannel::PumpOnGameThread(UWorld* World)
{
	DrainLogWarnings(World);
	if (!ListenSocket)
	{
		return;
	}

	// Accept any pending connections (non-blocking).
	bool bHasPending = false;
	while (ListenSocket->HasPendingConnection(bHasPending) && bHasPending)
	{
		if (FSocket* Client = ListenSocket->Accept(TEXT("WoTNetTestConn")))
		{
			Client->SetNonBlocking(true);
			Conns.Add(FConn{ Client, false, FString() });
		}
		else
		{
			break;
		}
	}

	for (int32 i = Conns.Num() - 1; i >= 0; --i)
	{
		FConn& C = Conns[i];
		uint32 PendingData = 0;
		while (C.Socket && C.Socket->HasPendingData(PendingData) && PendingData > 0)
		{
			uint8 Buf[4096];
			int32 Read = 0;
			if (!C.Socket->Recv(Buf, sizeof(Buf), Read) || Read <= 0)
			{
				break;
			}
			FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Buf), Read);
			C.RecvBuf.Append(Conv.Get(), Conv.Length());
		}

		bool bDropped = false;
		FString Line;
		FString Rest = C.RecvBuf;
		while (Rest.Split(TEXT("\n"), &Line, &Rest))
		{
			Line.TrimStartAndEndInline();   // tolerate trailing \r
			if (Line.IsEmpty())
			{
				continue;
			}
			TSharedPtr<FJsonObject> Req;
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Line);
			if (!FJsonSerializer::Deserialize(Reader, Req) || !Req.IsValid())
			{
				continue;
			}
			if (!C.bAuthed)
			{
				FString Tok;
				if (Req->TryGetStringField(TEXT("token"), Tok) && Tok == Token)
				{
					C.bAuthed = true;
					continue;
				}
				CloseSocket(C.Socket);
				Conns.RemoveAt(i);
				bDropped = true;
				break;
			}
			HandleCommand(World, C, Req.ToSharedRef());
		}
		if (!bDropped)
		{
			C.RecvBuf = Rest;   // keep any incomplete trailing line
		}
	}
}

void FWoTNetTestChannel::TickDirector(UWorld* World)
{
	if (!World) return;
	const double Now = World->GetTimeSeconds();
	const float DeltaSeconds = FMath::Max(0.0f, World->GetDeltaSeconds());

	if (Audio::FMixerPlatformNonRealtime* Mixer = NonRealtimeMixer(World))
	{
		if (!bDrivingAudio)
		{
			// Movie Render Queue's overrides (MoviePipelineGameOverrideSetting.cpp):
			// take over the render, never mute the offscreen device or disable a
			// silent submix, and play at full volume although the rig window
			// never has focus (UnfocusedVolumeMultiplier is silence by default).
			SetCaptureAudioCVar(TEXT("au.nrt.RenderEveryTick"), 0);
			SetCaptureAudioCVar(TEXT("au.NeverMuteNonRealtimeAudioDevices"), 1);
			SetCaptureAudioCVar(TEXT("au.NeverDisableSubmixes"), 1);
			FApp::SetUnfocusedVolumeMultiplier(1.0f);
			bDrivingAudio = true;
		}
		// UGameViewportClient::Draw moves the audio listener only while the
		// viewport has audio focus once more than one world shares the main
		// device (the guarded build's does), and the offscreen rig window never
		// receives focus, so every spatialized sound played to a listener left
		// at its default. Claim focus as a focused game window would.
		if (UGameViewportClient* Viewport = World->GetGameViewport())
		{
			Viewport->ReceivedFocus(Viewport->Viewport);
		}
		// Focus alone still left WoT's world-placed sounds silent while
		// listener-relative ones recorded, so also place listener 0 at the
		// hero's audio listener position exactly as Draw would.
		if (APlayerController* Hero = WoTNetTest::LocalPlayerController(World, 0))
		{
			FVector Location, Front, Right;
			Hero->GetAudioListenerPosition(Location, Front, Right);
			FTransform Listener(FRotationMatrix::MakeFromXY(Front, Right));
			Listener.SetTranslation(Location);
			if (FAudioDevice* Device = CaptureAudioDevice(World))
			{
				Device->SetListener(World, 0, Listener, FApp::GetDeltaTime());
			}
		}
		Mixer->RenderAudio(FApp::GetDeltaTime());
	}

	// Re-arm a decimated capture before this frame's timeline entries run,
	// so a capture turned on this frame keeps its armed first frame.
	if (CaptureEvery > 1 && GFrameCounter > CaptureStartFrame)
	{
		if (IConsoleVariable* DumpingMovie =
			IConsoleManager::Get().FindConsoleVariable(TEXT("r.DumpingMovie")))
		{
			DumpingMovie->Set(
				(GFrameCounter - CaptureStartFrame) % static_cast<uint64>(CaptureEvery) == 0 ? 1 : 0,
				ECVF_SetByCode);
		}
	}

	if (bTimelineRunning)
	{
		const double Elapsed = Now - TimelineStartTime;
		while (bTimelineRunning && Timeline.IsValidIndex(TimelineNext)
			&& Elapsed >= Timeline[TimelineNext].Time)
		{
			const WoTTrailer::FTimelineEntry Entry = Timeline[TimelineNext++];
			const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			const bool bOk = Entry.Request.IsValid()
				&& DispatchCommand(World, Entry.Request.ToSharedRef(), Response);
			// Commands can log on this game-thread pass. Drain before a final
			// timeline entry flips bTimelineRunning so its timestamp stays relative.
			DrainLogWarnings(World);
			if (!bOk)
			{
				FString Detail;
				Response->TryGetStringField(TEXT("err"), Detail);
				TimelineErrors.Add(FString::Printf(TEXT("entry %d (%s)%s%s"),
					Entry.SourceIndex, *Entry.Command,
					Detail.IsEmpty() ? TEXT("") : TEXT(": "), *Detail));
				if (Entry.bFatal)
				{
					bTimelineRunning = false;
					bTimelineDone = true;
					break;
				}
			}
			if (Entry.Command == TEXT("quit")) FPlatformMisc::RequestExit(false);
		}
		if (TimelineNext >= Timeline.Num())
		{
			bTimelineRunning = false;
			bTimelineDone = true;
		}
	}

	for (auto It = JumpReleaseTimes.CreateIterator(); It; ++It)
	{
		if (Now >= It.Value())
		{
			WoTNetTest::StopJump(World, It.Key());
			It.RemoveCurrent();
		}
	}

	for (auto It = Looks.CreateIterator(); It; ++It)
	{
		APlayerController* Controller = WoTNetTest::LocalPlayerController(World, It.Key());
		AWoTCharacter* Pawn = WoTNetTest::LocalPlayerPawn(World, It.Key());
		FLookState& State = It.Value();
		if (!Controller || !Pawn)
		{
			It.RemoveCurrent();
			continue;
		}
		if (State.bTracking)
		{
			if (!State.TrackPawn.IsValid())
			{
				It.RemoveCurrent();
				continue;
			}
			const FVector Direction = State.TrackPawn->GetActorLocation()
				- Pawn->GetPawnViewLocation();
			if (!Direction.IsNearlyZero()) State.Target = Direction.Rotation();
		}
		if (State.bUseSpring)
		{
			WoTTrailer::StepCriticalAngleSpring(State.YawSpring, State.Target.Yaw,
				State.SpringSeconds, DeltaSeconds);
			WoTTrailer::StepCriticalAngleSpring(State.PitchSpring, State.Target.Pitch,
				State.SpringSeconds, DeltaSeconds);
			Controller->SetControlRotation(FRotator(
				State.PitchSpring.Value, State.YawSpring.Value, State.Start.Roll));
			if (!State.bTracking
				&& FMath::Abs(FMath::FindDeltaAngleDegrees(State.YawSpring.Value,
					State.Target.Yaw)) < 0.05f
				&& FMath::Abs(FMath::FindDeltaAngleDegrees(State.PitchSpring.Value,
					State.Target.Pitch)) < 0.05f)
			{
				It.RemoveCurrent();
			}
		}
		else
		{
			const float RawAlpha = State.Duration <= 0.0
				? 1.0f : static_cast<float>((Now - State.StartTime) / State.Duration);
			const float Alpha = WoTTrailer::Ease(RawAlpha, State.Ease);
			FRotator ViewRotation;
			ViewRotation.Pitch = State.Start.Pitch + FMath::FindDeltaAngleDegrees(
				State.Start.Pitch, State.Target.Pitch) * Alpha;
			ViewRotation.Yaw = State.Start.Yaw + FMath::FindDeltaAngleDegrees(
				State.Start.Yaw, State.Target.Yaw) * Alpha;
			ViewRotation.Roll = State.Start.Roll;
			Controller->SetControlRotation(ViewRotation);
			if (RawAlpha >= 1.0f && !State.bTracking) It.RemoveCurrent();
			else if (RawAlpha >= 1.0f && State.bTracking) State.Start = State.Target;
		}
	}

	for (TPair<int32, FMoveToState>& Pair : MoveTos)
	{
		FMoveToState& State = Pair.Value;
		AWoTCharacter* Pawn = WoTNetTest::LocalPlayerPawn(World, Pair.Key);
		if (State.bDone || !Pawn) continue;
		FVector SteeringTarget = FVector::ZeroVector;
		FVector PathTangent = FVector::ForwardVector;
		float RemainingDistance = 0.0f;
		if (State.bCurved)
		{
			const FVector PawnPosition = Pawn->GetActorLocation();
			const float Moved = State.bHasTrackSample
				? static_cast<float>(FVector::Dist(PawnPosition, State.TrackSample)) : 0.0f;
			State.TrackSample = PawnPosition;
			State.bHasTrackSample = true;
			// A step longer than a walk makes in one tick is a teleport (a map
			// teleporter, a push), not route progress: the search stays at the
			// last progress and the off-route check re-stations from there.
			// A Battle2_02 walker was sent 38k uu by a teleporter; the
			// search followed it to the leg's end and the walk finished early.
			const float Window = FMath::Max(State.Lead, RouteTrackSlack)
				+ (Moved <= WalkerTeleportStep ? 2.0f * Moved : 0.0f);
			// Approaching a dead-end tip, the walker parks where its steering
			// point (a lead ahead, on the return pass) lands on itself, half a
			// lead short of the tip; its own spot on the return pass is then a
			// lead along the route, past the window. A later pass that ties is
			// taken out to the steering point and a little beyond.
			// Arena_03's walker parked short of WotPathNode8 and
			// InventorySpot105 until the blocked-walker jump fired.
			const float TieReach = State.Curve.AdvanceHorizontal(
				State.CurveDistance, State.Lead + RouteTrackSlack);
			// Progress is measured over the floor, like the lead: PathNodes
			// stand at their own heights, the walker at its floor's. The
			// Arena_04 spur climbs to InventorySpot42 and returns level 8 uu
			// aside; 35 uu below both, the walker on the return line still
			// measured nearer the outbound pass and parked short of the tip.
			State.CurveDistance = State.Curve.FindNearestDistance(PawnPosition,
				State.CurveDistance, State.CurveDistance + Window, TieReach, true);
			RemainingDistance = FMath::Max(0.0f,
				State.Curve.Length() - State.CurveDistance);
			State.bDone = RemainingDistance <= State.Tolerance
				&& FVector::DistSquared(PawnPosition, State.Curve.Points.Last())
					<= FMath::Square(State.Tolerance);
			if (State.bDone) continue;
			if (State.Snap > 0.0f)
			{
				// One moveto can now cover a whole route leg, so recovery runs
				// during the walk instead of only when a new moveto starts.
				const FVector RoutePoint = State.Curve.Sample(State.CurveDistance);
				const double OffRoute = FVector::Dist(PawnPosition, RoutePoint);
				// Only progress made on the route counts: the nearest-point
				// search can jump along the curve for a displaced walker.
				if (OffRoute <= State.Snap && State.CurveDistance
					>= State.ProgressSampleDistance + RouteProgressDistance)
				{
					State.ProgressSampleDistance = State.CurveDistance;
					State.ProgressSampleTime = Now;
					State.Restations = 0;
				}
				const bool bOffRoute = OffRoute > State.Snap
					&& Now >= State.RestationGraceUntil;
				if (bOffRoute
					|| Now - State.ProgressSampleTime >= RouteStallSeconds)
				{
					if (State.Restations >= RouteMaxRestations)
					{
						State.bDone = true;
						if (UCharacterMovementComponent* Movement =
							Pawn->GetCharacterMovement())
							Movement->StopMovementImmediately();
						RecordRouteSnap(World, Pawn, TEXT("abandoned"),
							PawnPosition, RoutePoint, OffRoute, false);
						continue;
					}
					const float Ahead = FMath::Min(State.Curve.Length(),
						State.CurveDistance + RouteRestationAhead);
					FVector Station = FVector::ZeroVector;
					const bool bSnapped = SafeTeleportToFloor(
						World, Pawn, State.Curve.Sample(Ahead), Station);
						if (bSnapped)
						{
							EndFallEpisode(Pawn);
							if (UCharacterMovementComponent* Movement =
								Pawn->GetCharacterMovement())
								Movement->StopMovementImmediately();
						State.CurveDistance = Ahead;
						State.TrackSample = Pawn->GetActorLocation();
					}
					RecordRouteSnap(World, Pawn,
						bOffRoute ? TEXT("off_route") : TEXT("stalled"),
						PawnPosition, bSnapped ? Station : State.Curve.Sample(Ahead),
						OffRoute, bSnapped);
					// Either way the next stall check waits a full window and
					// the next off-route check a short grace.
					State.ProgressSampleDistance = State.CurveDistance;
					State.ProgressSampleTime = Now;
					State.RestationGraceUntil = Now + RouteRestationGraceSeconds;
					++State.Restations;
					continue;
				}
			}
			// The lead is measured over the floor. Measured along the curve,
			// a steep stair (Arena_12: 40 uu of rise over 5 uu) used it up in
			// climb, put the steering point overhead, and the walker rocked in
			// place until the blocked-jump assist fired.
			const float TargetDistance =
				State.Curve.AdvanceHorizontal(State.CurveDistance, State.Lead);
			SteeringTarget = State.Curve.Sample(TargetDistance);
			PathTangent = State.Curve.Tangent(TargetDistance);
		}
		else
		{
			State.bDone = WoTTrailer::AdvanceWaypoints(
				Pawn->GetActorLocation(), State.Path, State.Tolerance, State.Waypoint);
			if (State.bDone) continue;
			SteeringTarget = State.Path[State.Waypoint];
			PathTangent = (SteeringTarget - Pawn->GetActorLocation()).GetSafeNormal();
			RemainingDistance = FVector::Distance(Pawn->GetActorLocation(), SteeringTarget);
			for (int32 PathIndex = State.Waypoint;
				PathIndex + 1 < State.Path.Num(); ++PathIndex)
			{
				RemainingDistance += FVector::Distance(
					State.Path[PathIndex], State.Path[PathIndex + 1]);
			}
		}
		const FVector MoveDirection = (SteeringTarget
			- Pawn->GetActorLocation()).GetSafeNormal2D();
		float InputScale = State.Speed;
		if (State.AccelSeconds > 0.0f)
		{
			InputScale *= FMath::Clamp(static_cast<float>(Now - State.StartTime)
				/ State.AccelSeconds, 0.0f, 1.0f);
		}
		if (State.DecelSeconds > 0.0f && State.Speed > UE_KINDA_SMALL_NUMBER)
		{
			const float DecelDistance = 400.0f * State.Speed * State.DecelSeconds;
			InputScale *= FMath::Clamp(RemainingDistance / DecelDistance, 0.0f, 1.0f);
		}
		if (State.bCurved && State.Snap > 0.0f)
		{
			const FVector Heading =
				State.Curve.Tangent(State.CurveDistance).GetSafeNormal2D();
			const FVector HeadingAhead = State.Curve.Tangent(State.Curve.AdvanceHorizontal(
				State.CurveDistance, RouteCornerLookLeads * State.Lead)).GetSafeNormal2D();
			if (!Heading.IsNearlyZero() && !HeadingAhead.IsNearlyZero())
			{
				InputScale *= FMath::GetMappedRangeValueClamped(
					FVector2f(0.0f, 0.7071f), FVector2f(RouteCornerMinScale, 1.0f),
					static_cast<float>(FVector::DotProduct(Heading, HeadingAhead)));
			}
		}
		if (State.bCurved && State.PaceSeconds > 0.0f && State.Curve.Length() > 0.0f)
		{
			const float Ahead = State.CurveDistance / State.Curve.Length()
				* State.PaceSeconds - static_cast<float>(Now - State.StartTime);
			InputScale *= FMath::Clamp(1.0f - Ahead / RoutePaceSeconds,
				RouteCornerMinScale, 1.0f);
		}
		Pawn->AddMovementInput(MoveDirection, InputScale);
		if (State.bJumpWhenBlocked)
		{
			UCharacterMovementComponent* Movement = Pawn->GetCharacterMovement();
			const FVector PawnLocation = Pawn->GetActorLocation();
			if (!Movement || Movement->MovementMode != MOVE_Walking)
			{
				State.BlockedMotionSampleLocation = PawnLocation;
				State.BlockedMotionSampleTime = Now;
				State.BlockedMotionSampleDistance = State.CurveDistance;
				State.bHasBlockedMotionSample = true;
			}
			else if (!State.bHasBlockedMotionSample)
			{
				State.BlockedMotionSampleLocation = PawnLocation;
				State.BlockedMotionSampleTime = Now;
				State.BlockedMotionSampleDistance = State.CurveDistance;
				State.bHasBlockedMotionSample = true;
			}
			else if (Now - State.BlockedMotionSampleTime >= 0.5)
			{
				const float Displacement = FVector::Dist2D(
					PawnLocation, State.BlockedMotionSampleLocation);
				// A walker turning back at a dead-end spur barely moves but
				// still advances along its route (Arena_03 hopped at
				// PathNode140). Only a walker making no route progress either
				// is blocked.
				const float RouteProgress = State.bCurved
					? State.CurveDistance - State.BlockedMotionSampleDistance : 0.0f;
				State.BlockedMotionSampleLocation = PawnLocation;
				State.BlockedMotionSampleTime = Now;
				State.BlockedMotionSampleDistance = State.CurveDistance;
				const bool bThrottleElapsed = State.LastAssistedJumpTime < 0.0
					|| Now - State.LastAssistedJumpTime >= 1.0;
				if (Displacement < 16.0f && RouteProgress < 16.0f && bThrottleElapsed
					&& WoTNetTest::Jump(World, Pair.Key))
				{
					State.LastAssistedJumpTime = Now;
					JumpReleaseTimes.Add(Pair.Key, Now + 0.1);
					TSharedRef<FJsonObject> AssistedJump = MakeShared<FJsonObject>();
					const double ProbeTime = ProbeEpochTime >= 0.0
						? FMath::Max(0.0, Now - ProbeEpochTime) : 0.0;
					AssistedJump->SetNumberField(TEXT("t"), ProbeTime);
					AssistedJump->SetStringField(TEXT("pawn"), Pawn->GetName());
					AssistedJump->SetArrayField(TEXT("location"),
						WriteVector(PawnLocation));
					AssistedJump->SetNumberField(TEXT("waypoint"),
						State.bCurved ? -1 : State.Waypoint);
					AssistedJump->SetArrayField(TEXT("heading"),
						WriteVector(MoveDirection * InputScale));
					AssistedJump->SetNumberField(TEXT("speed_xy"), Movement->Velocity.Size2D());
					if (Movement->CurrentFloor.bBlockingHit)
					{
						AssistedJump->SetNumberField(TEXT("floor_normal_z"),
							Movement->CurrentFloor.HitResult.ImpactNormal.Z);
					}
					FHitResult Blocker;
					if (SweepWalkerHeading(World, Pawn, MoveDirection, Blocker))
					{
						TSharedRef<FJsonObject> Blocked = MakeShared<FJsonObject>();
						Blocked->SetStringField(TEXT("actor"), GetNameSafe(Blocker.GetActor()));
						Blocked->SetStringField(TEXT("component"),
							GetNameSafe(Blocker.GetComponent()));
						Blocked->SetArrayField(TEXT("normal"),
							WriteVector(Blocker.ImpactNormal));
						Blocked->SetNumberField(TEXT("height"), Blocker.ImpactPoint.Z
							- (PawnLocation.Z - Pawn->GetSimpleCollisionHalfHeight()));
						Blocked->SetNumberField(TEXT("distance"), Blocker.Distance);
						AssistedJump->SetObjectField(TEXT("blocker"), Blocked);
					}
					ProbeAssistedJumps.Add(
						MakeShared<FJsonValueObject>(AssistedJump));
				}
			}
		}
		if (State.bFacePath)
		{
			if (State.FaceLagSeconds > 0.0f)
			{
				const FRotator TargetFacing = PathTangent.Rotation();
				WoTTrailer::StepCriticalAngleSpring(State.FaceYawSpring,
					TargetFacing.Yaw, State.FaceLagSeconds, DeltaSeconds);
				WoTTrailer::StepCriticalAngleSpring(State.FacePitchSpring,
					TargetFacing.Pitch, State.FaceLagSeconds, DeltaSeconds);
				if (AController* PawnController = Pawn->GetController())
				{
					FRotator ControlRotation = PawnController->GetControlRotation();
					ControlRotation.Yaw = State.FaceYawSpring.Value;
					ControlRotation.Pitch = State.FacePitchSpring.Value;
					PawnController->SetControlRotation(ControlRotation);
				}
			}
			else
			{
				WoTNetTest::FacePoint(World, SteeringTarget, Pair.Key);
			}
		}
	}

	if (CameraMode == ECameraMode::Free && DirectorCamera.IsValid())
	{
		const double CameraTime = Now - CameraStartTime;
		const WoTTrailer::FCameraPose Pose = bStyledCamera
			? WoTTrailer::InterpolateStyledCamera(CameraKeys, CameraTime, CameraEase)
			: WoTTrailer::InterpolateCamera(CameraKeys, CameraTime, CameraEase);
		FRotator CameraRotation = Pose.Rotation;
		if (CameraLookAtTarget.IsValid())
		{
			const FVector AimDirection = CameraLookAtTarget->GetActorLocation() - Pose.Location;
			if (!AimDirection.IsNearlyZero())
			{
				const FRotator AimRotation = AimDirection.Rotation();
				WoTTrailer::StepCriticalAngleSpring(CameraYawSpring, AimRotation.Yaw,
					CameraLookAtSpringSeconds, DeltaSeconds);
				WoTTrailer::StepCriticalAngleSpring(CameraPitchSpring, AimRotation.Pitch,
					CameraLookAtSpringSeconds, DeltaSeconds);
				CameraRotation = FRotator(CameraPitchSpring.Value,
					CameraYawSpring.Value, Pose.Rotation.Roll);
			}
		}
		if (ShakeAmplitudeDegrees > 0.0f && ShakeFrequencyHz > 0.0f)
		{
			const float NoiseTime = static_cast<float>(CameraTime) * ShakeFrequencyHz;
			CameraRotation.Yaw += ShakeAmplitudeDegrees
				* FMath::PerlinNoise1D(NoiseTime + ShakeSeed * 0.173f);
			CameraRotation.Pitch += ShakeAmplitudeDegrees
				* FMath::PerlinNoise1D(NoiseTime + ShakeSeed * 0.311f + 17.0f);
			CameraRotation.Roll += ShakeAmplitudeDegrees * 0.5f
				* FMath::PerlinNoise1D(NoiseTime + ShakeSeed * 0.479f + 37.0f);
		}
		DirectorCamera->SetActorLocationAndRotation(Pose.Location, CameraRotation);
		DirectorCamera->GetCameraComponent()->SetFieldOfView(
			CameraFOVOverride.IsSet() ? CameraFOVOverride.GetValue() : Pose.FOV);
	}
	else if (CameraMode == ECameraMode::Follow && DirectorCamera.IsValid()
		&& FollowPawn.IsValid())
	{
		const FVector TargetPosition = FollowPawn->GetActorLocation();
		if (FollowPreviousTime > 0.0 && Now > FollowPreviousTime)
		{
			const FVector TargetVelocity = (TargetPosition - FollowPreviousTarget)
				/ (Now - FollowPreviousTime);
			if (TargetVelocity.SizeSquared2D() > 1.0f)
			{
				WoTTrailer::StepCriticalAngleSpring(FollowHeadingSpring,
					TargetVelocity.GetSafeNormal2D().Rotation().Yaw,
					FMath::Max(0.2f, FollowLagSeconds), DeltaSeconds);
				FollowHeading = FollowHeadingSpring.Value;
			}
		}
		FollowPreviousTarget = TargetPosition;
		FollowPreviousTime = Now;
		const FVector FramedOffset = bFollowHeadingFrame
			? FRotator(0.0f, FollowHeading, 0.0f).RotateVector(FollowOffset)
			: FollowOffset;
		const FVector EyePosition = FollowPawn->GetPawnViewLocation();
		FVector DesiredPosition = TargetPosition + FramedOffset;
		const FVector DesiredRay = DesiredPosition - EyePosition;
		const float DesiredArm = DesiredRay.Size();
		const FVector RayDirection = DesiredRay.GetSafeNormal();
		if (bFollowAvoidWalls && DesiredArm > UE_KINDA_SMALL_NUMBER)
		{
			FHitResult WallHit;
			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(TrailerFollowCamera), false);
			QueryParams.AddIgnoredActor(FollowPawn.Get());
			const bool bHit = World->SweepSingleByObjectType(WallHit,
				EyePosition, DesiredPosition, FQuat::Identity,
				FCollisionObjectQueryParams(ECC_WorldStatic),
				FCollisionShape::MakeSphere(FollowWallRadius), QueryParams);
			const float SafeArm = bHit ? WoTTrailer::PullInCameraDistance(
				DesiredArm, WallHit.Distance, FollowClearance) : DesiredArm;
			if (bHit && FollowArmSpring.Value > SafeArm)
			{
				FollowArmSpring.Value = SafeArm;
				FollowArmSpring.Velocity = FMath::Min(0.0f, FollowArmSpring.Velocity);
			}
			else
			{
				WoTTrailer::StepCriticalSpring(FollowArmSpring, SafeArm,
					0.35f, DeltaSeconds);
			}
			DesiredPosition = EyePosition + RayDirection * FollowArmSpring.Value;
		}
		auto StepPosition = [DeltaSeconds, this](const FVector& Target)
		{
			WoTTrailer::StepCriticalSpring(FollowPositionSpring.X, Target.X,
				FollowLagSeconds, DeltaSeconds);
			WoTTrailer::StepCriticalSpring(FollowPositionSpring.Y, Target.Y,
				FollowLagSeconds, DeltaSeconds);
			WoTTrailer::StepCriticalSpring(FollowPositionSpring.Z, Target.Z,
				FollowLagSeconds, DeltaSeconds);
			return FVector(FollowPositionSpring.X.Value,
				FollowPositionSpring.Y.Value, FollowPositionSpring.Z.Value);
		};
		FVector CameraPosition = StepPosition(DesiredPosition);
		if (bFollowAvoidWalls)
		{
			FHitResult FinalHit;
			FCollisionQueryParams FinalParams(SCENE_QUERY_STAT(TrailerFollowCameraFinal), false);
			FinalParams.AddIgnoredActor(FollowPawn.Get());
			if (World->SweepSingleByObjectType(FinalHit, EyePosition, CameraPosition,
				FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
				FCollisionShape::MakeSphere(FollowWallRadius), FinalParams))
			{
				const FVector FinalRay = CameraPosition - EyePosition;
				CameraPosition = EyePosition + FinalRay.GetSafeNormal()
					* WoTTrailer::PullInCameraDistance(FinalRay.Size(),
						FinalHit.Distance, FollowClearance);
			}
		}
		FRotator CameraRotation = DirectorCamera->GetActorRotation();
		if (bFollowLookAt)
		{
			const FVector AimDirection = TargetPosition + FollowLookAtOffset - CameraPosition;
			if (!AimDirection.IsNearlyZero())
			{
				const FRotator AimRotation = AimDirection.Rotation();
				WoTTrailer::StepCriticalAngleSpring(FollowYawSpring, AimRotation.Yaw,
					FollowRotationLagSeconds, DeltaSeconds);
				WoTTrailer::StepCriticalAngleSpring(FollowPitchSpring, AimRotation.Pitch,
					FollowRotationLagSeconds, DeltaSeconds);
				CameraRotation = FRotator(FollowPitchSpring.Value,
					FollowYawSpring.Value, 0.0f);
			}
		}
		if (ShakeAmplitudeDegrees > 0.0f && ShakeFrequencyHz > 0.0f)
		{
			const float NoiseTime = static_cast<float>(Now - CameraStartTime) * ShakeFrequencyHz;
			CameraRotation.Yaw += ShakeAmplitudeDegrees
				* FMath::PerlinNoise1D(NoiseTime + ShakeSeed * 0.173f);
			CameraRotation.Pitch += ShakeAmplitudeDegrees
				* FMath::PerlinNoise1D(NoiseTime + ShakeSeed * 0.311f + 17.0f);
			CameraRotation.Roll += ShakeAmplitudeDegrees * 0.5f
				* FMath::PerlinNoise1D(NoiseTime + ShakeSeed * 0.479f + 37.0f);
		}
		DirectorCamera->SetActorLocationAndRotation(CameraPosition, CameraRotation);
	}
	TickFallWatch(World);
	TickProbes(World);
}

void FWoTNetTestChannel::HandleCommand(UWorld* World, FConn& Conn, const TSharedRef<FJsonObject>& Req)
{
	const TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
	double Id = 0;
	Req->TryGetNumberField(TEXT("id"), Id);
	Res->SetNumberField(TEXT("id"), Id);
	const bool bOk = DispatchCommand(World, Req, Res);
	Res->SetBoolField(TEXT("ok"), bOk);
	SendLine(Conn.Socket, OneLine(Res));
	FString Cmd;
	if (Req->TryGetStringField(TEXT("cmd"), Cmd) && Cmd == TEXT("quit"))
	{
		FPlatformMisc::RequestExit(false);
	}
}

bool FWoTNetTestChannel::PlayerIndex(const FJsonObject& Request, int32& OutPlayerIndex)
{
	OutPlayerIndex = 0;
	if (!Request.HasField(TEXT("p"))) return true;
	double Value = 0.0;
	if (!Request.TryGetNumberField(TEXT("p"), Value) || !FMath::IsFinite(Value)
		|| Value < 0.0 || Value > static_cast<double>(MAX_int32)
		|| Value != FMath::FloorToDouble(Value)) return false;
	OutPlayerIndex = static_cast<int32>(Value);
	return true;
}

bool FWoTNetTestChannel::DispatchCommand(UWorld* World,
	const TSharedRef<FJsonObject>& Req, const TSharedRef<FJsonObject>& Res)
{
	DrainLogWarnings(World);
	ULevel* CurrentLevel = IsValid(World) ? World->PersistentLevel : nullptr;
	if (ObservedWorld.Get() != World || ObservedLevel.Get() != CurrentLevel)
	{
		ObservedWorld = World;
		ObservedLevel = CurrentLevel;
		++WorldGeneration;
	}
	FString Cmd;
	if (!Req->TryGetStringField(TEXT("cmd"), Cmd) || Cmd.IsEmpty())
	{
		Res->SetStringField(TEXT("err"), TEXT("missing cmd"));
		return false;
	}
	bool bOk = true;
	int32 P = 0;
	if (!PlayerIndex(*Req, P))
	{
		Res->SetStringField(TEXT("err"), TEXT("p must be a nonnegative integer"));
		return false;
	}

	if (Cmd == TEXT("ping"))
	{
	}
	else if (Cmd == TEXT("snapshot"))
	{
		Res->SetObjectField(TEXT("data"), WoTNetTest::BuildSnapshot(World));
	}
	else if (Cmd == TEXT("status"))
	{
		TSharedPtr<FJsonObject> Status = WoTNetTest::BuildStatus(World);
		Status->SetNumberField(TEXT("world_generation"), WorldGeneration);
		Res->SetObjectField(TEXT("data"), Status);
	}
	else if (Cmd == TEXT("probe_reset"))
	{
		ResetProbes(World);
	}
	else if (Cmd == TEXT("probe_begin"))
	{
		bOk = BeginProbe(World, *Req);
	}
	else if (Cmd == TEXT("probe_end"))
	{
		FString Label;
		bOk = Req->TryGetStringField(TEXT("label"), Label)
			&& EndProbe(World, Label);
	}
	else if (Cmd == TEXT("probe_results"))
	{
		Res->SetObjectField(TEXT("data"), BuildProbeResults(World).ToSharedRef());
	}
	else if (Cmd == TEXT("fallwatch"))
	{
		bool bOn = false;
		double MinZ = 0.0;
		double Seconds = 2.5;
		bOk = Req->TryGetBoolField(TEXT("on"), bOn);
		if (bOn)
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("min_z"), MinZ)
				&& FMath::IsFinite(MinZ);
		}
		else if (Req->HasField(TEXT("min_z")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("min_z"), MinZ)
				&& FMath::IsFinite(MinZ);
		}
		if (Req->HasField(TEXT("seconds")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("seconds"), Seconds)
				&& FMath::IsFinite(Seconds) && Seconds >= 0.0;
		}
		if (bOk)
		{
			bFallWatchEnabled = bOn;
			FallWatchMinZ = static_cast<float>(MinZ);
			FallWatchSeconds = static_cast<float>(Seconds);
			if (!bOn) EndAllFallEpisodes();
		}
	}
	else if (Cmd == TEXT("resetstate"))
	{
		bOk = IsValid(World) && World->GetNetMode() != NM_Client;
		TArray<AActor*> Effects;
		if (bOk)
		{
			EndAllFallEpisodes();
			for (TActorIterator<APlayerController> It(World); It; ++It)
			{
				APlayerController* Controller = *It;
				APawn* Pawn = IsValid(Controller) ? Controller->GetPawn() : nullptr;
				if (IsValid(Pawn)
					&& IsValid(Controller->GetViewTarget())
					&& Controller->GetViewTarget()->IsA<AWoTDistantEyeProjectile>())
				{
					Controller->SetViewTarget(Pawn);
					Controller->ClientSetViewTarget(Pawn, FViewTargetTransitionParams());
				}
			}
			for (TActorIterator<AWoTCharacter> It(World); It; ++It)
			{
				AWoTCharacter* Pawn = *It;
				if (!IsValid(Pawn)) continue;
				TArray<UWoTLeech*> Leeches;
				Pawn->GetComponents<UWoTLeech>(Leeches);
				for (UWoTLeech* Leech : Leeches)
				{
					// Virtual Unattach also thaws UWoTIceLeech's freeze pin.
					if (IsValid(Leech)) Leech->Unattach();
				}
				TArray<UWoTReflector*> Reflectors;
				Pawn->GetComponents<UWoTReflector>(Reflectors);
				for (UWoTReflector* Reflector : Reflectors)
				{
					if (IsValid(Reflector)) Reflector->Uninstall();
				}
				if (Pawn->IsPlayerControlled())
				{
					Pawn->RevertDisguise();
				}
			}
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor)) continue;
				const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Actor);
				const bool bHarnessSummon = NPC && NPC->SummonOwner.IsValid()
					&& NPC->SummonOwner->IsPlayerControlled();
				const bool bPostBaselineImpact =
					!ScenarioBaselineActors.Contains(Actor->GetName())
					&& LooksLikeImpact(Actor);
				if (IsHarnessEffect(Actor) || IsStagedWeaveLeftover(Actor)
					|| bHarnessSummon || bPostBaselineImpact)
					Effects.AddUnique(Actor);
			}
			for (AActor* Effect : Effects)
			{
				if (AWoTLightGlobe* Light = Cast<AWoTLightGlobe>(Effect))
					Light->Follow(nullptr);
				else if (IsValid(Effect)) Effect->Destroy();
			}
			WoTNetTest::ClearProjectiles(World);
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor)) continue;
				const bool bPostBaselineImpact =
					!ScenarioBaselineActors.Contains(Actor->GetName())
					&& LooksLikeImpact(Actor);
				if (IsHarnessEffect(Actor) || IsStagedWeaveLeftover(Actor)
					|| bPostBaselineImpact)
					Effects.AddUnique(Actor);
			}
			for (AActor* Effect : Effects)
			{
				if (IsValid(Effect)) Effect->Destroy();
			}
			for (TActorIterator<AWoTCharacter> It(World); It; ++It)
			{
				AWoTCharacter* Pawn = *It;
				if (!IsValid(Pawn) || !Pawn->IsPlayerControlled()) continue;
				Pawn->MaxHealth = FMath::Max(Pawn->MaxHealth, 100000);
				Pawn->Health = Pawn->MaxHealth;
				Pawn->bDead = false;
				if (UWoTInventoryComponent* Inventory = Pawn->InventoryComponent)
				{
					const TArray<TObjectPtr<AWoTInventory>> Items = Inventory->Items;
					for (AWoTInventory* Item : Items)
					{
						if (IsValid(Item) && Item->IsA<AWoTAngrealInventory>())
						{
							Inventory->DeleteInventory(Item);
							Item->Destroy();
						}
					}
				}
				if (UCharacterMovementComponent* Movement = Pawn->GetCharacterMovement())
					Movement->StopMovementImmediately();
			}
		}
		Res->SetNumberField(TEXT("effects_cleared"), Effects.Num());
	}
	else if (Cmd == TEXT("host"))
	{
		FString Game;
		Req->TryGetStringField(TEXT("game"), Game);
		// Optional match rules (the host menu's FWoTHostRequest fields): absent
		// keeps DefaultHostRequest's retail values, so existing callers are unchanged.
		double FragLimitField = -1.0, TimeLimitField = -1.0;
		double SealGoalField = -1.0, SealBudgetField = -1.0;
		const bool bLan = HostLanFromCommand(*Req);
		Req->TryGetNumberField(TEXT("fraglimit"), FragLimitField);
		Req->TryGetNumberField(TEXT("timelimit"), TimeLimitField);
		Req->TryGetNumberField(TEXT("sealgoal"), SealGoalField);
		Req->TryGetNumberField(TEXT("sealbudget"), SealBudgetField);
		bOk = WoTNetTest::Host(
			World, Req->GetStringField(TEXT("map")), Game,
			(int32)FragLimitField, (int32)TimeLimitField, bLan,
			(int32)SealGoalField, (int32)SealBudgetField);
	}
	else if (Cmd == TEXT("travel"))
	{
		FString Map, Game;
		bOk = Req->TryGetStringField(TEXT("map"), Map)
			&& Req->TryGetStringField(TEXT("game"), Game)
			&& WoTNetTest::Travel(World, Map, Game);
	}
	else if (Cmd == TEXT("faction"))
	{
		// In-memory only (never saved): the next host/join URL carries this
		// ?Class=, so a two-instance run can field two different player
		// classes (Disguise needs a target of another class).
		bOk = WoTNetTest::SetFaction(World, Req->GetStringField(TEXT("name")), P);
	}
	else if (Cmd == TEXT("puppets"))
	{
		double Count = 0.0;
		TArray<FString> Names, Factions;
		bOk = Req->TryGetNumberField(TEXT("count"), Count) && FMath::IsFinite(Count)
			&& Count >= 0.0 && Count <= static_cast<double>(MAX_int32)
			&& Count == FMath::FloorToDouble(Count)
			&& ReadStringArray(*Req, TEXT("names"), Names)
			&& ReadStringArray(*Req, TEXT("factions"), Factions);
		TArray<TSharedPtr<FJsonValue>> Puppets;
		if (bOk)
		{
			bOk = WoTNetTest::CreatePuppets(
				World, static_cast<int32>(Count), Names, Factions, Puppets);
		}
		Res->SetArrayField(TEXT("puppets"), Puppets);
	}
	else if (Cmd == TEXT("latejoin"))
	{
		FString Name, Faction;
		bool bRequireDisguise = false;
		Req->TryGetStringField(TEXT("name"), Name);
		Req->TryGetStringField(TEXT("faction"), Faction);
		if (Req->HasField(TEXT("require_disguise")))
		{
			bOk = Req->TryGetBoolField(TEXT("require_disguise"), bRequireDisguise);
		}
		TArray<TSharedPtr<FJsonValue>> Puppets;
		bOk = bOk && AddLateJoin(
			World, Name, Faction, bRequireDisguise, Puppets);
		Res->SetStringField(TEXT("mode"), TEXT("local_player_fallback"));
		Res->SetArrayField(TEXT("puppets"), Puppets);
	}
	else if (Cmd == TEXT("latejoin_verify"))
	{
		bOk = VerifyLateJoin(World);
		Res->SetStringField(TEXT("mode"), TEXT("local_player_fallback"));
	}
	else if (Cmd == TEXT("join"))
	{
		// {"address": "host:port"} is the direct-IP connect; {"row": N} joins
		// the Nth row the multiplayer browser lists (after a "find").
		double Row = -1.0;
		FString Address;
		if (Req->TryGetStringField(TEXT("address"), Address))
		{
			bOk = WoTNetTest::Join(World, Address);
		}
		else if (Req->TryGetNumberField(TEXT("row"), Row))
		{
			bOk = Row >= 0.0 && WoTNetTest::BrowserJoin(World, (int32)Row);
		}
		else
		{
			bOk = false;
			Res->SetStringField(TEXT("err"), TEXT("join needs address or row"));
		}
	}
	else if (Cmd == TEXT("find"))
	{
		// The browser's Refresh; "lan" defaults to true (the Null OSS path).
		bool bLan = true;
		Req->TryGetBoolField(TEXT("lan"), bLan);
		bOk = WoTNetTest::BrowserFind(World, bLan);
	}
	else if (Cmd == TEXT("hostkind"))
	{
		// {"battle": true} selects the host panel's first Citadel map, false
		// its first Arena map; "scrollable" is the panel's overflow as last laid
		// out (repeat the call after a frame to read the new layout's).
		bool bBattle = true;
		Req->TryGetBoolField(TEXT("battle"), bBattle);
		double Scrollable = -1.0;
		bOk = WoTNetTest::BrowserSelectHostMapKind(World, bBattle, Scrollable);
		Res->SetNumberField(TEXT("scrollable"), Scrollable);
	}
	else if (Cmd == TEXT("move"))
	{
		double X = 0.0, Y = 0.0;
		Req->TryGetNumberField(TEXT("x"), X);
		Req->TryGetNumberField(TEXT("y"), Y);
		HeldMoves.FindOrAdd(P).Axis.X = X;
		HeldMoves.FindOrAdd(P).Axis.Y = Y;
	}
	else if (Cmd == TEXT("stop"))
	{
		// Stop ends a route walk too: a still-active moveto otherwise keeps
		// steering a parked pawn (the late-join verify saw it move).
		HeldMoves.FindOrAdd(P).Axis = FVector2D::ZeroVector;
		MoveTos.Remove(P);
	}
	else if (Cmd == TEXT("castpress"))
	{
		bOk = WoTNetTest::CastPress(World, P);
	}
	else if (Cmd == TEXT("castrelease"))
	{
		bOk = WoTNetTest::CastRelease(World, P);
	}
	else if (Cmd == TEXT("next"))
	{
		bOk = WoTNetTest::NextArtifact(World, P);
	}
	else if (Cmd == TEXT("jump"))
	{
		double Hold = 0.1;
		Req->TryGetNumberField(TEXT("hold"), Hold);
		bOk = Hold >= 0.0 && WoTNetTest::Jump(World, P);
		if (bOk)
		{
			JumpReleaseTimes.Add(P, World->GetTimeSeconds() + Hold);
		}
	}
	else if (Cmd == TEXT("select"))
	{
		FString ClassName;
		bOk = Req->TryGetStringField(TEXT("class"), ClassName)
			&& WoTNetTest::SelectArtifact(World, ClassName, P);
	}
	else if (Cmd == TEXT("look") || Cmd == TEXT("lookat"))
	{
		APlayerController* Controller = WoTNetTest::LocalPlayerController(World, P);
		AWoTCharacter* Pawn = WoTNetTest::LocalPlayerPawn(World, P);
		FRotator Target = Controller ? Controller->GetControlRotation() : FRotator::ZeroRotator;
		TWeakObjectPtr<AWoTCharacter> Track;
		bool bTrack = false;
		if (Cmd == TEXT("look"))
		{
			double Yaw = Target.Yaw, Pitch = Target.Pitch;
			bOk = Controller && Req->TryGetNumberField(TEXT("yaw"), Yaw)
				&& Req->TryGetNumberField(TEXT("pitch"), Pitch);
			Target.Yaw = Yaw;
			Target.Pitch = Pitch;
		}
		else
		{
			FVector Point = FVector::ZeroVector;
			FString PawnName;
			if (Req->TryGetStringField(TEXT("pawn"), PawnName))
			{
				Track = FindPawn(World, PawnName);
				bOk = Track.IsValid();
				if (bOk) Point = Track->GetActorLocation();
			}
			else
			{
				double X = 0.0, Y = 0.0, Z = 0.0;
				bOk = Req->TryGetNumberField(TEXT("x"), X)
					&& Req->TryGetNumberField(TEXT("y"), Y)
					&& Req->TryGetNumberField(TEXT("z"), Z);
				Point = FVector(X, Y, Z);
			}
			Req->TryGetBoolField(TEXT("track"), bTrack);
			const FVector Direction = Pawn ? Point - Pawn->GetPawnViewLocation() : FVector::ZeroVector;
			bOk = bOk && Controller && Pawn && !Direction.IsNearlyZero();
			if (bOk)
			{
				const FRotator Look = Direction.Rotation();
				Target.Yaw = Look.Yaw;
				Target.Pitch = Look.Pitch;
			}
		}
		double Duration = 0.0;
		double SpringSeconds = 0.0;
		FString EaseName(TEXT("linear"));
		Req->TryGetNumberField(TEXT("dur"), Duration);
		Req->TryGetStringField(TEXT("ease"), EaseName);
		const bool bSpringSpecified = Req->HasField(TEXT("spring"));
		if (bSpringSpecified)
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("spring"), SpringSeconds)
				&& FMath::IsFinite(SpringSeconds) && SpringSeconds >= 0.0;
			bOk = bOk && !Req->HasField(TEXT("dur")) && !Req->HasField(TEXT("ease"));
		}
		if (bOk)
		{
			FLookState State;
			State.Start = Controller->GetControlRotation();
			State.Target = Target;
			State.StartTime = World->GetTimeSeconds();
			State.Duration = FMath::Max(0.0, Duration);
			State.Ease = WoTTrailer::ParseEase(EaseName);
			State.TrackPawn = Track;
			State.bTracking = bTrack && Track.IsValid();
			State.bUseSpring = bSpringSpecified || State.bTracking;
			State.SpringSeconds = static_cast<float>(bSpringSpecified
				? SpringSeconds : 0.35);
			State.YawSpring.Value = State.Start.Yaw;
			State.PitchSpring.Value = State.Start.Pitch;
			Looks.Add(P, State);
		}
	}
	else if (Cmd == TEXT("moveto"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		bOk = Req->TryGetArrayField(TEXT("path"), Points) && Points && !Points->IsEmpty();
		FMoveToState State;
		if (bOk)
		{
			for (const TSharedPtr<FJsonValue>& PointValue : *Points)
			{
				const TArray<TSharedPtr<FJsonValue>>* Coordinates = nullptr;
				FVector Point;
				if (!PointValue.IsValid() || !PointValue->TryGetArray(Coordinates)
					|| !Coordinates || !ReadVector(*Coordinates, Point))
				{
					bOk = false;
					break;
				}
				State.Path.Add(Point);
			}
		}
		double Speed = 1.0, Tolerance = 40.0, Lead = 100.0, Snap = 0.0;
		double TurnRadius = 0.0, AccelSeconds = 0.0, DecelSeconds = 0.0;
		double FaceLagSeconds = 0.0, PaceSeconds = 0.0;
		bool bSmooth = false, bJumpWhenBlocked = false;
		FString Face(TEXT("none"));
		Req->TryGetNumberField(TEXT("speed"), Speed);
		Req->TryGetNumberField(TEXT("tol"), Tolerance);
		Req->TryGetStringField(TEXT("face"), Face);
		if (Req->HasField(TEXT("smooth")))
		{
			bOk = bOk && Req->TryGetBoolField(TEXT("smooth"), bSmooth);
		}
		if (Req->HasField(TEXT("turn_radius")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("turn_radius"), TurnRadius)
				&& FMath::IsFinite(TurnRadius) && TurnRadius >= 0.0;
		}
		if (Req->HasField(TEXT("lead")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("lead"), Lead)
				&& FMath::IsFinite(Lead) && Lead >= 0.0;
		}
		if (Req->HasField(TEXT("accel")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("accel"), AccelSeconds)
				&& FMath::IsFinite(AccelSeconds) && AccelSeconds >= 0.0;
		}
		if (Req->HasField(TEXT("decel")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("decel"), DecelSeconds)
				&& FMath::IsFinite(DecelSeconds) && DecelSeconds >= 0.0;
		}
		if (Req->HasField(TEXT("duration")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("duration"), PaceSeconds)
				&& FMath::IsFinite(PaceSeconds) && PaceSeconds >= 0.0;
		}
		if (Req->HasField(TEXT("face_lag")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("face_lag"), FaceLagSeconds)
				&& FMath::IsFinite(FaceLagSeconds) && FaceLagSeconds >= 0.0;
		}
		if (Req->HasField(TEXT("snap")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("snap"), Snap)
				&& FMath::IsFinite(Snap) && Snap >= 0.0;
		}
		if (Req->HasField(TEXT("jump_when_blocked")))
		{
			bOk = bOk && Req->TryGetBoolField(
				TEXT("jump_when_blocked"), bJumpWhenBlocked);
		}
		bOk = bOk && !(bSmooth && Req->HasField(TEXT("turn_radius")));
		State.Speed = FMath::Clamp(static_cast<float>(Speed), 0.0f, 1.0f);
		State.Tolerance = FMath::Max(0.0f, static_cast<float>(Tolerance));
		State.bFacePath = Face.Equals(TEXT("path"), ESearchCase::IgnoreCase);
		State.Lead = static_cast<float>(Lead);
		State.Snap = static_cast<float>(Snap);
		State.AccelSeconds = static_cast<float>(AccelSeconds);
		State.DecelSeconds = static_cast<float>(DecelSeconds);
		State.PaceSeconds = static_cast<float>(PaceSeconds);
		State.StartTime = World->GetTimeSeconds();
		State.bCurved = bSmooth || TurnRadius > 0.0;
		State.bJumpWhenBlocked = bJumpWhenBlocked;
		if (bOk && Snap > 0.0)
		{
			if (AWoTCharacter* MovePawn = WoTNetTest::LocalPlayerPawn(World, P))
			{
				const FVector From = MovePawn->GetActorLocation();
				const double Distance = FVector::Dist(From, State.Path[0]);
				if (Distance > Snap)
				{
					// A failed re-station still walks the route from where the
					// pawn stands; the record says the snap failed.
					FVector Station = FVector::ZeroVector;
					const bool bSnapped =
						SafeTeleportToFloor(World, MovePawn, State.Path[0], Station);
					if (bSnapped)
					{
						EndFallEpisode(MovePawn);
						if (UCharacterMovementComponent* Movement =
							MovePawn->GetCharacterMovement())
							Movement->StopMovementImmediately();
					}
					RecordRouteSnap(World, MovePawn, TEXT("start"), From,
						bSnapped ? Station : State.Path[0], Distance, bSnapped);
				}
			}
		}
		if (bOk && State.bCurved)
		{
			State.Curve = bSmooth
				? WoTTrailer::BuildCentripetalPath(State.Path)
				: WoTTrailer::BuildRoundedPath(State.Path, static_cast<float>(TurnRadius));
			bOk = State.Curve.Points.Num() >= 2;
			if (bOk)
			{
				if (AWoTCharacter* MovePawn = WoTNetTest::LocalPlayerPawn(World, P))
				{
					// A route walk starts within Snap of its first point (or is
					// re-stationed there), so the start is searched only that far
					// along: the route may pass the start again later. A tie keeps
					// the earlier point. Arena_09's leg starts on a 71 uu
					// spur back through its own start (InventorySpot28 -> 26 -> 28):
					// a later-pass tie began the walk 142 uu in, skipped the spur,
					// and pacing then held the walker 0.6 s "ahead" at a crawl.
					State.CurveDistance = State.Curve.FindNearestDistance(
						MovePawn->GetActorLocation(), 0.0f,
						State.Snap > 0.0f ? State.Snap : TNumericLimits<float>::Max(),
						0.0f, true, false);
					State.TrackSample = MovePawn->GetActorLocation();
					State.bHasTrackSample = true;
					State.ProgressSampleDistance = State.CurveDistance;
					State.ProgressSampleTime = State.StartTime;
					if (AController* MoveController = MovePawn->GetController())
					{
						const FRotator InitialFacing = MoveController->GetControlRotation();
						State.FaceYawSpring.Value = InitialFacing.Yaw;
						State.FacePitchSpring.Value = InitialFacing.Pitch;
					}
				}
			}
		}
		State.FaceLagSeconds = static_cast<float>(Req->HasField(TEXT("face_lag"))
			? FaceLagSeconds : (State.bCurved && State.bFacePath ? 0.2 : 0.0));
		if (bOk && State.bJumpWhenBlocked)
		{
			if (AWoTCharacter* MovePawn = WoTNetTest::LocalPlayerPawn(World, P))
			{
				State.BlockedMotionSampleLocation = MovePawn->GetActorLocation();
				State.BlockedMotionSampleTime = World->GetTimeSeconds();
				State.BlockedMotionSampleDistance = State.CurveDistance;
				State.bHasBlockedMotionSample = true;
			}
		}
		if (bOk) MoveTos.Add(P, MoveTemp(State));
		Res->SetNumberField(TEXT("speed_uu_s"), 400.0 * FMath::Clamp(Speed, 0.0, 1.0));
	}
	else if (Cmd == TEXT("moveto_status"))
	{
		const FMoveToState* State = MoveTos.Find(P);
		bOk = State != nullptr;
		if (State)
		{
			Res->SetNumberField(TEXT("waypoint"), State->Waypoint);
			Res->SetNumberField(TEXT("count"), State->Path.Num());
			Res->SetBoolField(TEXT("done"), State->bDone);
			Res->SetNumberField(TEXT("curve_distance"), State->CurveDistance);
		}
	}
	else if (Cmd == TEXT("damage"))
	{
		double Amount = 0.0;
		bool bQuiet = false;
		Req->TryGetNumberField(TEXT("amount"), Amount);
		Req->TryGetBoolField(TEXT("quiet"), bQuiet);
		if (bQuiet)
		{
			// Staging only: lower Health without the damage presentation. Retail
			// TakeDamage shakes the view by the damage dealt (0.15 + 0.005 *
			// Damage seconds, Damage * 30 roll units), so the weave reel's Heal
			// staging (99950 off the survival pool) rolled the camera upside
			// down for the next ~500 s of the reel.
			AWoTCharacter* Pawn = FindPawn(World, Req->GetStringField(TEXT("pawn")));
			bOk = IsValid(Pawn) && World->GetNetMode() != NM_Client;
			if (bOk) Pawn->Health = FMath::Max(1, Pawn->Health - FMath::RoundToInt(Amount));
		}
		else
		{
			bOk = WoTNetTest::DamagePawn(World, Req->GetStringField(TEXT("pawn")),
				static_cast<float>(Amount));
		}
	}
	else if (Cmd == TEXT("sprayer_activate"))
	{
		FString ActorName;
		AWoTParticleSprayer* Sprayer = nullptr;
		if (Req->TryGetStringField(TEXT("actor"), ActorName))
		{
			Sprayer = Cast<AWoTParticleSprayer>(FindActor(World, ActorName));
		}
		else
		{
			// Projected actors carry runtime names (WoTProjected_<record>_<rule>),
			// so a reel names a retail sprayer by its authored location: the
			// nearest sprayer within radius (default 64 uu).
			double X = 0.0, Y = 0.0, Z = 0.0, Radius = 64.0;
			bOk = Req->TryGetNumberField(TEXT("x"), X)
				&& Req->TryGetNumberField(TEXT("y"), Y)
				&& Req->TryGetNumberField(TEXT("z"), Z);
			if (Req->HasField(TEXT("radius")))
			{
				bOk = bOk && Req->TryGetNumberField(TEXT("radius"), Radius)
					&& FMath::IsFinite(Radius) && Radius > 0.0;
			}
			double BestDistanceSquared = Radius * Radius;
			for (TActorIterator<AWoTParticleSprayer> It(World); bOk && It; ++It)
			{
				const double DistanceSquared =
					FVector::DistSquared(It->GetActorLocation(), FVector(X, Y, Z));
				if (DistanceSquared <= BestDistanceSquared)
				{
					BestDistanceSquared = DistanceSquared;
					Sprayer = *It;
				}
			}
		}
		bOk = IsValid(Sprayer);
		if (bOk)
		{
			// Close-up staging only: reveal the exact projected retail sprayer.
			// Animation, particles, materials, and frame advancement remain the
			// production actor's; this does not synthesize replacement evidence.
			Sprayer->bOn = true;
			Res->SetStringField(TEXT("actor"), Sprayer->GetName());
		}
	}
	else if (Cmd == TEXT("give"))
	{
		double Charges = 0.0;
		Req->TryGetNumberField(TEXT("charges"), Charges);
		bOk = WoTNetTest::GiveArtifact(World, Req->GetStringField(TEXT("pawn")),
			Req->GetStringField(TEXT("class")), (int32)Charges);
	}
	else if (Cmd == TEXT("give_seal"))
	{
		bOk = WoTNetTest::GiveSeal(World, Req->GetStringField(TEXT("pawn")));
	}
	else if (Cmd == TEXT("face"))
	{
		FString PawnName;
		if (Req->TryGetStringField(TEXT("pawn"), PawnName))
		{
			bOk = WoTNetTest::FaceTarget(World, PawnName, P);
		}
		else
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			Req->TryGetNumberField(TEXT("x"), X);
			Req->TryGetNumberField(TEXT("y"), Y);
			Req->TryGetNumberField(TEXT("z"), Z);
			bOk = WoTNetTest::FacePoint(World, FVector(X, Y, Z), P);
		}
	}
	else if (Cmd == TEXT("aim"))
	{
		FString PawnName;
		if (Req->TryGetStringField(TEXT("pawn"), PawnName))
		{
			bOk = WoTNetTest::AimTarget(World, PawnName, P);
		}
		else
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			Req->TryGetNumberField(TEXT("x"), X);
			Req->TryGetNumberField(TEXT("y"), Y);
			Req->TryGetNumberField(TEXT("z"), Z);
			bOk = WoTNetTest::AimPoint(World, FVector(X, Y, Z), P);
		}
	}
	else if (Cmd == TEXT("teleport"))
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
		Req->TryGetNumberField(TEXT("x"), X);
		Req->TryGetNumberField(TEXT("y"), Y);
		Req->TryGetNumberField(TEXT("z"), Z);
		const FString PawnName = Req->GetStringField(TEXT("pawn"));
		bOk = WoTNetTest::Teleport(World, PawnName, FVector(X, Y, Z));
		if (bOk) EndFallEpisode(FindPawn(World, PawnName));
	}
	else if (Cmd == TEXT("tourpoint"))
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
		FString PawnName;
		bOk = IsValid(World)
			&& Req->TryGetStringField(TEXT("pawn"), PawnName)
			&& Req->TryGetNumberField(TEXT("x"), X)
			&& Req->TryGetNumberField(TEXT("y"), Y)
			&& Req->TryGetNumberField(TEXT("z"), Z);
		AWoTCharacter* Pawn = bOk ? FindPawn(World, PawnName) : nullptr;
		bOk = bOk && IsValid(Pawn);
		FVector Station = FVector::ZeroVector;
		FVector PreviousLocation = FVector::ZeroVector;
		FRotator PreviousRotation = FRotator::ZeroRotator;
		FVector PreviousVelocity = FVector::ZeroVector;
		bool bPlacedAtStation = false;
		if (bOk)
		{
			PreviousLocation = Pawn->GetActorLocation();
			PreviousRotation = Pawn->GetActorRotation();
			PreviousVelocity = Pawn->GetVelocity();
			bOk = SafeTeleportToFloor(World, Pawn, FVector(X, Y, Z), Station);
			bPlacedAtStation = bOk;
			if (bOk) EndFallEpisode(Pawn);
		}
		if (bOk)
		{
			// A tour point re-stations the pawn: an unfinished route walk
			// must not pull it away again.
			for (auto It = MoveTos.CreateIterator(); It; ++It)
			{
				if (WoTNetTest::LocalPlayerPawn(World, It.Key()) == Pawn) It.RemoveCurrent();
			}
		}
		if (bOk)
		{
			FCollisionQueryParams FloorParams(
				SCENE_QUERY_STAT(WoTPlaythroughTourFloor), true, Pawn);
			FHitResult FloorCheck;
			bool bForcePostCheckFailure = false;
#if WITH_AUTOMATION_TESTS
			bForcePostCheckFailure =
				bForceNextTourPointPostCheckFailureForTesting;
			bForceNextTourPointPostCheckFailureForTesting = false;
#endif
			bOk = !bForcePostCheckFailure
				&& World->LineTraceSingleByChannel(FloorCheck, Station,
					Station - FVector(0.0, 0.0,
						Pawn->GetSimpleCollisionHalfHeight() + 20.0f),
					ECC_Visibility, FloorParams);
			Res->SetNumberField(TEXT("x"), Station.X);
			Res->SetNumberField(TEXT("y"), Station.Y);
			Res->SetNumberField(TEXT("z"), Station.Z);
		}
		if (!bOk && bPlacedAtStation)
		{
			Pawn->SetActorLocationAndRotation(PreviousLocation, PreviousRotation,
				false, nullptr, ETeleportType::TeleportPhysics);
			if (UCharacterMovementComponent* Movement =
				Pawn->GetCharacterMovement())
			{
				Movement->Velocity = PreviousVelocity;
			}
			EndFallEpisode(Pawn);
		}
		if (!bOk) Res->SetStringField(TEXT("err"),
			TEXT("no safe walkable station near retail coverage point"));
	}
	else if (Cmd == TEXT("stage_validate"))
	{
		TArray<FString> PawnNames;
		FString SourceName, TargetPawnName;
		bool bExpectBlocked = false;
		bOk = IsValid(World)
			&& ReadStringArray(*Req, TEXT("pawns"), PawnNames)
			&& !PawnNames.IsEmpty()
			&& Req->TryGetStringField(TEXT("source"), SourceName)
			&& Req->TryGetBoolField(TEXT("expect_blocked"), bExpectBlocked);
		TArray<AActor*> Ignored;
		for (const FString& PawnName : PawnNames)
		{
			AWoTCharacter* Pawn = FindPawn(World, PawnName);
			bOk = bOk && IsValid(Pawn);
			if (!Pawn) continue;
			Ignored.Add(Pawn);
			FCollisionQueryParams OverlapParams(SCENE_QUERY_STAT(WoTPlaythroughStage),
				false, Pawn);
			bOk = bOk && !World->OverlapBlockingTestByChannel(
				Pawn->GetActorLocation(), FQuat::Identity, ECC_Visibility,
				FCollisionShape::MakeSphere(20.0f), OverlapParams);
			FHitResult FloorHit;
			bOk = bOk && World->LineTraceSingleByChannel(
				FloorHit, Pawn->GetActorLocation(),
				Pawn->GetActorLocation() - FVector(0.0f, 0.0f, 200.0f),
				ECC_Visibility, OverlapParams);
		}
		AWoTCharacter* Source = FindPawn(World, SourceName);
		FVector Target = FVector::ZeroVector;
		if (Req->TryGetStringField(TEXT("target_pawn"), TargetPawnName))
		{
			AWoTCharacter* TargetPawn = FindPawn(World, TargetPawnName);
			bOk = bOk && IsValid(TargetPawn);
			if (TargetPawn) Target = TargetPawn->GetActorLocation();
		}
		else
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			bOk = bOk && Req->TryGetNumberField(TEXT("x"), X)
				&& Req->TryGetNumberField(TEXT("y"), Y)
				&& Req->TryGetNumberField(TEXT("z"), Z);
			Target = FVector(X, Y, Z);
		}
		if (bOk && Source)
		{
			FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(WoTPlaythroughSight),
				true);
			TraceParams.AddIgnoredActors(Ignored);
			FHitResult Hit;
			const bool bBlocked = World->LineTraceSingleByChannel(
				Hit, Source->GetPawnViewLocation(), Target, ECC_Visibility,
				TraceParams);
			bOk = bBlocked == bExpectBlocked;
			Res->SetBoolField(TEXT("blocked"), bBlocked);
			if (bBlocked) Res->SetStringField(TEXT("hit"),
				Hit.GetActor() ? Hit.GetActor()->GetName() : TEXT("BSP"));
		}
	}
	else if (Cmd == TEXT("spawn_zone"))
	{
		double X = 0.0, Y = 0.0, Z = 0.0, VX = 0.0, VY = 0.0, VZ = 0.0;
		Req->TryGetNumberField(TEXT("x"), X);
		Req->TryGetNumberField(TEXT("y"), Y);
		Req->TryGetNumberField(TEXT("z"), Z);
		Req->TryGetNumberField(TEXT("vx"), VX);
		Req->TryGetNumberField(TEXT("vy"), VY);
		Req->TryGetNumberField(TEXT("vz"), VZ);
		bOk = WoTNetTest::SpawnZone(World, FVector(X, Y, Z), FVector(VX, VY, VZ));
	}
	else if (Cmd == TEXT("endgame"))
	{
		bOk = WoTNetTest::EndMatch(World);
	}
	else if (Cmd == TEXT("clearprojectiles"))
	{
		const int32 Cleared = WoTNetTest::ClearProjectiles(World);
		bOk = Cleared >= 0;
		Res->SetNumberField(TEXT("cleared"), Cleared);
	}
	else if (Cmd == TEXT("invulnerable"))
	{
		// The playthrough walker survives its tour: on a Battle team it would
		// otherwise die to falls, crushing movers and hazards (a
		// Battle2_01 walker died at a mover station). UE's god flag, which
		// AWoTCharacter's damage entry honours.
		bool bOn = true;
		if (Req->HasField(TEXT("on")))
		{
			bOk = Req->TryGetBoolField(TEXT("on"), bOn);
		}
		AWoTCharacter* Pawn = WoTNetTest::LocalPlayerPawn(World, P);
		bOk = bOk && Pawn != nullptr;
		if (bOk) Pawn->SetCanBeDamaged(!bOn);
	}
	else if (Cmd == TEXT("changeteam"))
	{
		double Team = 0.0;
		Req->TryGetNumberField(TEXT("team"), Team);
		bOk = WoTNetTest::ChangeTeam(World, (int32)Team, P);
	}
	else if (Cmd == TEXT("battle_play"))
	{
		// teams[i] is local player i's team; see WoTNetTest::OpenBattle.
		const TArray<TSharedPtr<FJsonValue>>* TeamValues = nullptr;
		TArray<int32> Teams;
		bOk = Req->TryGetArrayField(TEXT("teams"), TeamValues) && TeamValues;
		for (int32 Index = 0; bOk && Index < TeamValues->Num(); ++Index)
		{
			double Team = -1.0;
			bOk = (*TeamValues)[Index].IsValid()
				&& (*TeamValues)[Index]->TryGetNumber(Team)
				&& Team == FMath::FloorToDouble(Team) && Team >= 0.0 && Team <= 3.0;
			if (bOk) Teams.Add(static_cast<int32>(Team));
		}
		bOk = bOk && WoTNetTest::OpenBattle(World, Teams);
	}
	else if (Cmd == TEXT("listener"))
	{
		// Trailer audio: pin local player 0's audio listener at a world point
		// (x, y, z, optional yaw) instead of its view point, or {"clear": true} to
		// restore it. The capture tick places listener 0 from
		// GetAudioListenerPosition, which honours this override. Used when the
		// free camera sits too far from the action for its sounds to reach it.
		// The pin moves WoTSound's retail hearing gate with it: the gate measures
		// from the view target (the free camera), so without this a sound beside
		// the pinned listener but past radius/sqrt(1.3) of the camera is culled.
		APlayerController* Player0 = WoTNetTest::LocalPlayerController(World, 0);
		bool bClear = false;
		Req->TryGetBoolField(TEXT("clear"), bClear);
		double X = 0.0, Y = 0.0, Z = 0.0, Yaw = 0.0;
		if (!Player0)
		{
			bOk = false;
		}
		else if (bClear)
		{
			Player0->ClearAudioListenerOverride();
			WoTSound::ClearHearingOriginOverride(*World);
		}
		else
		{
			bOk = Req->TryGetNumberField(TEXT("x"), X)
				&& Req->TryGetNumberField(TEXT("y"), Y)
				&& Req->TryGetNumberField(TEXT("z"), Z);
			Req->TryGetNumberField(TEXT("yaw"), Yaw);
			if (bOk)
			{
				Player0->SetAudioListenerOverride(nullptr, FVector(X, Y, Z),
					FRotator(0.0, Yaw, 0.0));
				WoTSound::SetHearingOriginOverride(*World, FVector(X, Y, Z));
			}
		}
	}
	else if (Cmd == TEXT("camera"))
	{
		FString Mode;
		bOk = Req->TryGetStringField(TEXT("mode"), Mode);
		APlayerController* Player0 = WoTNetTest::LocalPlayerController(World, 0);
		if (bOk && Mode.Equals(TEXT("pov"), ESearchCase::IgnoreCase))
		{
			CameraMode = ECameraMode::Pov;
			CameraKeys.Reset();
			FollowPawn.Reset();
			if (Player0 && Player0->GetPawn())
			{
				Player0->GetPawn()->SetActorHiddenInGame(false);
				Player0->SetViewTarget(Player0->GetPawn());
			}
			else bOk = false;
		}
		else if (bOk && Mode.Equals(TEXT("free"), ESearchCase::IgnoreCase))
		{
			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			bOk = Player0 && Req->TryGetArrayField(TEXT("keys"), Keys)
				&& Keys && !Keys->IsEmpty();
			CameraKeys.Reset();
			CameraLookAtTarget.Reset();
			bStyledCamera = false;
			FString EaseName(TEXT("linear"));
			Req->TryGetStringField(TEXT("ease"), EaseName);
			CameraEase = WoTTrailer::ParseEase(EaseName);
			if (bOk)
			{
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					const TSharedPtr<FJsonObject> Key = KeyValue.IsValid()
						? KeyValue->AsObject() : nullptr;
					double Time = 0.0, X = 0.0, Y = 0.0, Z = 0.0;
					double Yaw = 0.0, Pitch = 0.0, Roll = 0.0, FOV = 90.0, Hold = 0.0;
					if (!Key || !Key->TryGetNumberField(TEXT("t"), Time)
						|| !Key->TryGetNumberField(TEXT("x"), X)
						|| !Key->TryGetNumberField(TEXT("y"), Y)
						|| !Key->TryGetNumberField(TEXT("z"), Z)
						|| !Key->TryGetNumberField(TEXT("yaw"), Yaw)
						|| !Key->TryGetNumberField(TEXT("pitch"), Pitch))
					{
						bOk = false;
						break;
					}
					Key->TryGetNumberField(TEXT("roll"), Roll);
					Key->TryGetNumberField(TEXT("fov"), FOV);
					if (Key->HasField(TEXT("hold"))
						&& (!Key->TryGetNumberField(TEXT("hold"), Hold)
							|| !FMath::IsFinite(Hold) || Hold < 0.0))
					{
						bOk = false;
						break;
					}
					FString KeyEaseName;
					const bool bHasKeyEase = Key->HasField(TEXT("ease"));
					if (bHasKeyEase && !Key->TryGetStringField(TEXT("ease"), KeyEaseName))
					{
						bOk = false;
						break;
					}
					WoTTrailer::FCameraKey CameraKey;
					CameraKey.Time = Time;
					CameraKey.Location = FVector(X, Y, Z);
					CameraKey.Rotation = FRotator(Pitch, Yaw, Roll);
					CameraKey.FOV = static_cast<float>(FOV);
					CameraKey.Hold = static_cast<float>(Hold);
					CameraKey.bHasEase = bHasKeyEase;
					CameraKey.Ease = bHasKeyEase
						? WoTTrailer::ParseEase(KeyEaseName) : WoTTrailer::EEase::Linear;
					CameraKeys.Add(CameraKey);
					bStyledCamera = bStyledCamera || bHasKeyEase || Hold > 0.0;
				}
			}
			FString LookAtName;
			if (bOk && Req->TryGetStringField(TEXT("lookat"), LookAtName))
			{
				CameraLookAtTarget = FindActor(World, LookAtName);
				bOk = CameraLookAtTarget.IsValid();
				bStyledCamera = true;
			}
			double LookAtSpring = 0.35;
			if (bOk && Req->HasField(TEXT("spring")))
			{
				bOk = CameraLookAtTarget.IsValid()
					&& Req->TryGetNumberField(TEXT("spring"), LookAtSpring)
					&& FMath::IsFinite(LookAtSpring) && LookAtSpring >= 0.0;
			}
			CameraLookAtSpringSeconds = static_cast<float>(LookAtSpring);
			bOk = bOk && ReadShake(*Req, ShakeAmplitudeDegrees,
				ShakeFrequencyHz, ShakeSeed);
			if (bOk)
			{
				CameraKeys.StableSort([](const WoTTrailer::FCameraKey& A,
					const WoTTrailer::FCameraKey& B) { return A.Time < B.Time; });
				if (!DirectorCamera.IsValid())
				{
					DirectorCamera = World->SpawnActor<ACameraActor>();
				}
				bOk = DirectorCamera.IsValid();
			}
			if (bOk)
			{
				UCameraComponent* Camera = DirectorCamera->GetCameraComponent();
				Camera->SetConstraintAspectRatio(true);
				Camera->SetAspectRatio(16.0f / 9.0f);
				CameraStartTime = World->GetTimeSeconds();
				CameraFOVOverride.Reset();
				const WoTTrailer::FCameraPose InitialPose = bStyledCamera
					? WoTTrailer::InterpolateStyledCamera(CameraKeys, 0.0, CameraEase)
					: WoTTrailer::InterpolateCamera(CameraKeys, 0.0, CameraEase);
				CameraYawSpring.Value = InitialPose.Rotation.Yaw;
				CameraYawSpring.Velocity = 0.0f;
				CameraPitchSpring.Value = InitialPose.Rotation.Pitch;
				CameraPitchSpring.Velocity = 0.0f;
				CameraMode = ECameraMode::Free;
				Player0->SetViewTarget(DirectorCamera.Get());
				bool bHidePawn = false;
				Req->TryGetBoolField(TEXT("hidepawn"), bHidePawn);
				if (Player0->GetPawn()) Player0->GetPawn()->SetActorHiddenInGame(bHidePawn);
			}
		}
		else if (bOk && Mode.Equals(TEXT("follow"), ESearchCase::IgnoreCase))
		{
			FString PawnName;
			if (Req->TryGetStringField(TEXT("pawn"), PawnName))
			{
				FollowPawn = FindPawn(World, PawnName);
			}
			else
			{
				FollowPawn = WoTNetTest::LocalPlayerPawn(World, P);
			}
			const TArray<TSharedPtr<FJsonValue>>* Offset = nullptr;
			FollowOffset = FVector::ZeroVector;
			bOk = Player0 && FollowPawn.IsValid()
				&& Req->TryGetArrayField(TEXT("offset"), Offset)
				&& Offset && ReadVector(*Offset, FollowOffset);
			FollowLookAtOffset = FVector::ZeroVector;
			if (bOk && Req->HasField(TEXT("lookat_offset")))
			{
				const TArray<TSharedPtr<FJsonValue>>* LookAtOffset = nullptr;
				bOk = Req->TryGetArrayField(TEXT("lookat_offset"), LookAtOffset)
					&& LookAtOffset && ReadVector(*LookAtOffset, FollowLookAtOffset);
			}
			bFollowLookAt = true;
			Req->TryGetBoolField(TEXT("lookat"), bFollowLookAt);
			FString Frame(TEXT("world"));
			Req->TryGetStringField(TEXT("frame"), Frame);
			bFollowHeadingFrame = Frame.Equals(TEXT("heading"), ESearchCase::IgnoreCase);
			bOk = bOk && (bFollowHeadingFrame
				|| Frame.Equals(TEXT("world"), ESearchCase::IgnoreCase));
			double Lag = 0.0, RotationLag = 0.0, WallRadius = 12.0;
			if (Req->HasField(TEXT("lag")))
			{
				bOk = bOk && Req->TryGetNumberField(TEXT("lag"), Lag)
					&& FMath::IsFinite(Lag) && Lag >= 0.0;
			}
			if (Req->HasField(TEXT("rot_lag")))
			{
				bOk = bOk && Req->TryGetNumberField(TEXT("rot_lag"), RotationLag)
					&& FMath::IsFinite(RotationLag) && RotationLag >= 0.0;
			}
			if (Req->HasField(TEXT("wall_radius")))
			{
				bOk = bOk && Req->TryGetNumberField(TEXT("wall_radius"), WallRadius)
					&& FMath::IsFinite(WallRadius) && WallRadius > 0.0;
			}
			bFollowAvoidWalls = false;
			if (Req->HasField(TEXT("avoid_walls")))
			{
				bOk = bOk && Req->TryGetBoolField(TEXT("avoid_walls"), bFollowAvoidWalls);
			}
			FollowLagSeconds = static_cast<float>(Lag);
			FollowRotationLagSeconds = static_cast<float>(RotationLag);
			FollowWallRadius = static_cast<float>(WallRadius);
			bOk = bOk && ReadShake(*Req, ShakeAmplitudeDegrees,
				ShakeFrequencyHz, ShakeSeed);
			if (bOk)
			{
				if (!DirectorCamera.IsValid()) DirectorCamera = World->SpawnActor<ACameraActor>();
				bOk = DirectorCamera.IsValid();
			}
			if (bOk)
			{
				UCameraComponent* Camera = DirectorCamera->GetCameraComponent();
				Camera->SetConstraintAspectRatio(true);
				Camera->SetAspectRatio(16.0f / 9.0f);
				CameraLookAtTarget.Reset();
				CameraStartTime = World->GetTimeSeconds();
				FollowPreviousTarget = FollowPawn->GetActorLocation();
				FollowPreviousTime = CameraStartTime;
				FollowHeading = FollowPawn->GetActorRotation().Yaw;
				FollowHeadingSpring.Value = FollowHeading;
				FollowHeadingSpring.Velocity = 0.0f;
				const FVector InitialOffset = bFollowHeadingFrame
					? FRotator(0.0f, FollowHeading, 0.0f).RotateVector(FollowOffset)
					: FollowOffset;
				const FVector InitialPosition = FollowPreviousTarget + InitialOffset;
				FollowPositionSpring.X.Value = InitialPosition.X;
				FollowPositionSpring.X.Velocity = 0.0f;
				FollowPositionSpring.Y.Value = InitialPosition.Y;
				FollowPositionSpring.Y.Velocity = 0.0f;
				FollowPositionSpring.Z.Value = InitialPosition.Z;
				FollowPositionSpring.Z.Velocity = 0.0f;
				FollowArmSpring.Value = FVector::Distance(
					FollowPawn->GetPawnViewLocation(), InitialPosition);
				FollowArmSpring.Velocity = 0.0f;
				const FVector InitialAim = FollowPreviousTarget
					+ FollowLookAtOffset - InitialPosition;
				const FRotator InitialRotation = InitialAim.IsNearlyZero()
					? DirectorCamera->GetActorRotation() : InitialAim.Rotation();
				FollowYawSpring.Value = InitialRotation.Yaw;
				FollowYawSpring.Velocity = 0.0f;
				FollowPitchSpring.Value = InitialRotation.Pitch;
				FollowPitchSpring.Velocity = 0.0f;
				CameraMode = ECameraMode::Follow;
				Player0->SetViewTarget(DirectorCamera.Get());
				bool bHidePawn = false;
				Req->TryGetBoolField(TEXT("hidepawn"), bHidePawn);
				if (Player0->GetPawn()) Player0->GetPawn()->SetActorHiddenInGame(bHidePawn);
			}
		}
		else if (bOk)
		{
			bOk = false;
			Res->SetStringField(TEXT("err"), TEXT("camera mode must be pov, free, or follow"));
		}
	}
	else if (Cmd == TEXT("fov"))
	{
		double Degrees = 0.0;
		bOk = Req->TryGetNumberField(TEXT("deg"), Degrees) && Degrees > 0.0;
		if (bOk && CameraMode != ECameraMode::Pov && DirectorCamera.IsValid())
		{
			DirectorCamera->GetCameraComponent()->SetFieldOfView(Degrees);
			if (CameraMode == ECameraMode::Free)
			{
				CameraFOVOverride = static_cast<float>(Degrees);
			}
		}
		else if (bOk)
		{
			AWoTPlayerController* Controller = Cast<AWoTPlayerController>(
				WoTNetTest::LocalPlayerController(World, 0));
			bOk = Controller != nullptr;
			if (Controller) Controller->ClientSetCameraFOV(Degrees);
		}
	}
	else if (Cmd == TEXT("hud"))
	{
		bool bOn = false;
		bOk = Req->TryGetBoolField(TEXT("on"), bOn);
		APlayerController* Controller = WoTNetTest::LocalPlayerController(World, 0);
		AHUD* HUD = Controller ? Controller->GetHUD() : nullptr;
		bOk = bOk && HUD;
		if (HUD) HUD->bShowHUD = bOn;
	}
	else if (Cmd == TEXT("capture"))
	{
		bool bOn = false;
		bOk = Req->TryGetBoolField(TEXT("on"), bOn);
		double Every = 1.0;
		if (Req->HasField(TEXT("every")))
		{
			bOk = bOk && Req->TryGetNumberField(TEXT("every"), Every)
				&& Every >= 1.0 && Every <= 60.0 && Every == FMath::FloorToDouble(Every);
		}
		if (bOk && bOn) ProbeEpochTime = World->GetTimeSeconds();
		IConsoleVariable* DumpingMovie = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DumpingMovie"));
		bOk = bOk && DumpingMovie;
		CaptureEvery = bOk && bOn ? static_cast<int32>(Every) : 1;
		CaptureStartFrame = CaptureEvery > 1 ? GFrameCounter : 0;
		// r.DumpingMovie >0 dumps that many frames then turns itself off
		// (FScreenshotRequest::CreateViewportScreenShotFilename), so a
		// decimated capture arms one frame now and TickDirector re-arms it
		// every Nth frame after.
		if (DumpingMovie) DumpingMovie->Set(bOk && bOn ? (CaptureEvery > 1 ? 1 : -1) : 0,
			ECVF_SetByCode);
		FAudioDevice* AudioDevice = bOk ? CaptureAudioDevice(World) : nullptr;
		if (AudioDevice && bOn && !bCaptureAudio)
		{
			AudioDevice->StartRecording(nullptr, 0.0f);
			bCaptureAudio = true;
		}
		else if (AudioDevice && !bOn && bCaptureAudio)
		{
			Res->SetNumberField(TEXT("audio_seconds"), WriteCaptureAudio(*AudioDevice));
			Res->SetStringField(TEXT("audio"), CaptureAudioPath());
			// Where silence comes from, should a take record none: the device's
			// primary volume and what the retail sound generators produced.
			const FWoTPayloadSoundStats Stats = GetWoTPayloadSoundStats();
			Res->SetNumberField(TEXT("primary_volume"), AudioDevice->GetPrimaryVolume());
			Res->SetNumberField(TEXT("generators"), static_cast<double>(Stats.Generators));
			Res->SetNumberField(TEXT("null_generators"),
				static_cast<double>(Stats.NullGenerators));
			Res->SetNumberField(TEXT("generated_samples"), static_cast<double>(Stats.Samples));
			Res->SetNumberField(TEXT("nonzero_samples"),
				static_cast<double>(Stats.NonZeroSamples));
			TArray<TSharedPtr<FJsonValue>> Listeners;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				FVector Position;
				if (AudioDevice->GetListenerPosition(Index, Position, false))
				{
					Listeners.Add(MakeShared<FJsonValueString>(Position.ToString()));
				}
			}
			Res->SetArrayField(TEXT("listeners"), Listeners);
			bCaptureAudio = false;
		}
		Res->SetBoolField(TEXT("recording_audio"), bCaptureAudio);
		Res->SetNumberField(TEXT("every"), CaptureEvery);
		Res->SetStringField(TEXT("directory"), MovieDumpDirectory());
		Res->SetNumberField(bOn ? TEXT("first") : TEXT("last"),
			bOn ? LastMovieFrame() + 1 : LastMovieFrame());
	}
	else if (Cmd == TEXT("materials"))
	{
		// Render probe for the log-less guarded build: what every primitive of
		// the matching actors draws with (material, root, blend, bound Tex).
		FString ClassPart;
		bOk = Req->TryGetStringField(TEXT("class"), ClassPart) && !ClassPart.IsEmpty();
		TArray<TSharedPtr<FJsonValue>> Actors;
		for (TActorIterator<AActor> It(World); bOk && It; ++It)
		{
			if (!It->GetClass()->GetName().Contains(ClassPart))
			{
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Slots;
			TInlineComponentArray<UPrimitiveComponent*> Primitives(*It);
			for (UPrimitiveComponent* Primitive : Primitives)
			{
				for (int32 Slot = 0; Slot < Primitive->GetNumMaterials(); ++Slot)
				{
					UMaterialInterface* Material = Primitive->GetMaterial(Slot);
					const UMaterial* Root = Material ? Material->GetMaterial() : nullptr;
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("component"), Primitive->GetName());
					Entry->SetNumberField(TEXT("slot"), Slot);
					Entry->SetBoolField(TEXT("visible"), Primitive->IsVisible());
					Entry->SetStringField(TEXT("material"),
						Material ? Material->GetName() : TEXT("none"));
					Entry->SetStringField(TEXT("root"), Root ? Root->GetName() : TEXT("none"));
					Entry->SetNumberField(TEXT("blend"),
						Material ? static_cast<int32>(Material->GetBlendMode()) : -1);
					UTexture* Texture = nullptr;
					if (Material && Material->GetTextureParameterValue(
						FHashedMaterialParameterInfo(TEXT("Tex")), Texture) && Texture)
					{
						Entry->SetStringField(TEXT("tex"), Texture->GetName());
					}
					FLinearColor Ambient;
					if (Material && Material->GetVectorParameterValue(
						FHashedMaterialParameterInfo(TEXT("MLAmbient")), Ambient))
					{
						Entry->SetStringField(TEXT("ml_ambient"), Ambient.ToString());
					}
					Slots.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			TSharedRef<FJsonObject> ActorEntry = MakeShared<FJsonObject>();
			ActorEntry->SetStringField(TEXT("name"), It->GetName());
			ActorEntry->SetArrayField(TEXT("slots"), Slots);
			Actors.Add(MakeShared<FJsonValueObject>(ActorEntry));
		}
		Res->SetArrayField(TEXT("actors"), Actors);
	}
	else if (Cmd == TEXT("capture_flush"))
	{
		IConsoleVariable* DumpingMovie = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.DumpingMovie"));
		bOk = DumpingMovie != nullptr;
		if (DumpingMovie) DumpingMovie->Set(0, ECVF_SetByCode);
		if (FAudioDevice* AudioDevice = bCaptureAudio ? CaptureAudioDevice(World) : nullptr)
		{
			Res->SetNumberField(TEXT("audio_seconds"), WriteCaptureAudio(*AudioDevice));
			Res->SetStringField(TEXT("audio"), CaptureAudioPath());
		}
		bCaptureAudio = false;
		CaptureEvery = 1;
		CaptureStartFrame = 0;
		if (bOk) FlushRenderingCommands();
		Res->SetStringField(TEXT("directory"), MovieDumpDirectory());
		Res->SetNumberField(TEXT("last"), LastMovieFrame());
	}
	else if (Cmd == TEXT("timeline"))
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const TArray<TSharedPtr<FJsonValue>>* Inline = nullptr;
		FString Path;
		if (Req->TryGetArrayField(TEXT("entries"), Inline) && Inline)
		{
			Values = *Inline;
		}
		else if (Req->TryGetStringField(TEXT("path"), Path))
		{
			FString Raw;
			TSharedPtr<FJsonValue> Root;
			bOk = FFileHelper::LoadFileToString(Raw, *Path);
			if (bOk)
			{
				const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Raw);
				bOk = FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid();
			}
			const TArray<TSharedPtr<FJsonValue>>* FileEntries = nullptr;
			if (bOk && Root->TryGetArray(FileEntries) && FileEntries) Values = *FileEntries;
			else if (bOk) bOk = false;
		}
		else bOk = false;
		TArray<FString> ParseErrors;
		if (bOk) bOk = WoTTrailer::ParseTimeline(Values, Timeline, ParseErrors);
		if (!bOk)
		{
			Timeline.Reset();
			TimelineErrors = MoveTemp(ParseErrors);
			if (TimelineErrors.IsEmpty()) TimelineErrors.Add(TEXT("timeline could not be loaded"));
		}
		else
		{
			TimelineErrors.Reset();
			TimelineNext = 0;
			TimelineStartTime = World->GetTimeSeconds();
			bTimelineRunning = true;
			bTimelineDone = Timeline.IsEmpty();
		}
		Res->SetNumberField(TEXT("count"), Timeline.Num());
	}
	else if (Cmd == TEXT("timeline_status"))
	{
		Res->SetNumberField(TEXT("elapsed"), bTimelineRunning || bTimelineDone
			? World->GetTimeSeconds() - TimelineStartTime : 0.0);
		Res->SetNumberField(TEXT("next"), TimelineNext);
		Res->SetBoolField(TEXT("done"), bTimelineDone);
		TArray<TSharedPtr<FJsonValue>> Errors;
		for (const FString& Error : TimelineErrors)
			Errors.Add(MakeShared<FJsonValueString>(Error));
		Res->SetArrayField(TEXT("errors"), Errors);
	}
	else if (Cmd == TEXT("timeline_stop"))
	{
		bTimelineRunning = false;
		bTimelineDone = true;
	}
	else if (Cmd == TEXT("screenshot"))
	{
		bOk = WoTNetTest::Screenshot(World);
	}
	else if (Cmd == TEXT("exec"))
	{
		// Route through the local player when one exists: PC->ConsoleCommand reaches
		// UFUNCTION(Exec) on the controller/pawn (ProcessConsoleExec) AND falls through
		// to the engine for cvars, so lag-profile execs keep working. A dedicated
		// server has no local player -> engine-only, as before.
		const FString Line = Req->GetStringField(TEXT("c"));
		APlayerController* LocalPC = WoTNetTest::LocalPlayerController(World, P);
		if (LocalPC)
		{
			// Surface recognition + output in the response: the packaged
			// guarded client is log-less, so "ok" alone hid an unrecognized
			// verb behind ConsoleCommand's void return (task9f host-join).
			FStringOutputDevice Captured;
			const bool bHandled = LocalPC->Player != nullptr
				&& LocalPC->Player->Exec(World, *Line, Captured);
			Res->SetBoolField(TEXT("handled"), bHandled);
			Res->SetStringField(TEXT("pc"), LocalPC->GetClass()->GetName());
			if (!Captured.IsEmpty())
			{
				Res->SetStringField(TEXT("out"), Captured);
			}
			// An unrecognized verb is a FAILED exec, not a success with a
			// footnote: ok mirrors handled so a caller checking only ok
			// cannot mistake an unrouted command for an executed one.
			bOk = bHandled;
		}
		else
		{
			bOk = GEngine != nullptr && World != nullptr && GEngine->Exec(World, *Line);
		}
	}
	else if (Cmd == TEXT("quit"))
	{
	}
	else
	{
		bOk = false;
		Res->SetStringField(TEXT("err"), TEXT("unknown cmd"));
	}

	// Advance the game-thread time anchor after state-changing commands (most
	// importantly timeline start/stop) and collect warnings they emitted.
	DrainLogWarnings(World);
	return bOk;
}

bool FWoTNetTestChannel::HostLanFromCommand(const FJsonObject& Request)
{
	const TSharedPtr<FJsonValue> Lan = Request.TryGetField(TEXT("lan"));
	// This is a remote control surface: only a JSON boolean may opt into EOS.
	// Missing or malformed values retain the historical, privacy-safe LAN path.
	return !Lan.IsValid() || Lan->Type != EJson::Boolean || Lan->AsBool();
}

#if WITH_AUTOMATION_TESTS
bool FWoTNetTestChannel::HostLanFromCommandForTesting(
	const FJsonObject& Request)
{
	return HostLanFromCommand(Request);
}
#endif

#endif
